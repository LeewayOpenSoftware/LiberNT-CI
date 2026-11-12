/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/fsrtl/fsfltcb.c
 * PURPOSE:         File system acquire/release entry points for Mm and Cc,
 *                  and the file system filter callbacks around them
 * PROGRAMMERS:     Dominique Cote (buzdelabuz2@gmail.com)
 *                  Alex Ionescu (alex.ionescu@reactos.org)
 *                  Aleksey Bragin (aleksey@reactos.org)
 *                  Leeway Open Software (filter callback dispatch)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PRIVATE FUNCTIONS *********************************************************/

/* Filters with callbacks for one operation that a single call dispatches to */
#define FSRTLP_MAX_FILTER_CALLS 16

typedef NTSTATUS (*PFSRTLP_FS_OPERATION)(_In_ PFILE_OBJECT FileObject,
                                         _In_ PFS_FILTER_PARAMETERS Parameters);

typedef struct _FSRTLP_FILTER_CALL
{
    PDEVICE_OBJECT DeviceObject;
    PFS_FILTER_COMPLETION_CALLBACK PostCallback;
    PVOID CompletionContext;
} FSRTLP_FILTER_CALL, *PFSRTLP_FILTER_CALL;

static
ULONG
FsRtlpPreCallbackOffset(_In_ UCHAR Operation)
{
    switch (Operation)
    {
        case FS_FILTER_ACQUIRE_FOR_SECTION_SYNCHRONIZATION:
            return FIELD_OFFSET(FS_FILTER_CALLBACKS, PreAcquireForSectionSynchronization);
        case FS_FILTER_RELEASE_FOR_SECTION_SYNCHRONIZATION:
            return FIELD_OFFSET(FS_FILTER_CALLBACKS, PreReleaseForSectionSynchronization);
        case FS_FILTER_ACQUIRE_FOR_CC_FLUSH:
            return FIELD_OFFSET(FS_FILTER_CALLBACKS, PreAcquireForCcFlush);
        case FS_FILTER_RELEASE_FOR_CC_FLUSH:
            return FIELD_OFFSET(FS_FILTER_CALLBACKS, PreReleaseForCcFlush);
        case FS_FILTER_ACQUIRE_FOR_MOD_WRITE:
            return FIELD_OFFSET(FS_FILTER_CALLBACKS, PreAcquireForModifiedPageWriter);
        case FS_FILTER_RELEASE_FOR_MOD_WRITE:
            return FIELD_OFFSET(FS_FILTER_CALLBACKS, PreReleaseForModifiedPageWriter);
        default:
            ASSERT(FALSE);
            return 0;
    }
}

/* The Pre and Post callbacks a device's driver registered for an operation.
 * Only SizeOfFsFilterCallbacks bytes were copied at registration, so a field
 * past that size was not registered and must not be read. */
static
VOID
FsRtlpGetFilterCallbacks(_In_ PDEVICE_OBJECT DeviceObject,
                         _In_ UCHAR Operation,
                         _Out_ PFS_FILTER_CALLBACK *PreCallback,
                         _Out_ PFS_FILTER_COMPLETION_CALLBACK *PostCallback)
{
    PFS_FILTER_CALLBACKS Callbacks;
    ULONG Offset;

    *PreCallback = NULL;
    *PostCallback = NULL;

    Callbacks = IoGetDrvObjExtension(DeviceObject->DriverObject)->FsFilterCallbacks;
    Offset = FsRtlpPreCallbackOffset(Operation);
    if (Callbacks == NULL || Offset == 0)
    {
        return;
    }

    if (Callbacks->SizeOfFsFilterCallbacks >= Offset + sizeof(PVOID))
    {
        *PreCallback = *(PFS_FILTER_CALLBACK *)((PUCHAR)Callbacks + Offset);
    }
    if (Callbacks->SizeOfFsFilterCallbacks >= Offset + 2 * sizeof(PVOID))
    {
        *PostCallback = *(PFS_FILTER_COMPLETION_CALLBACK *)((PUCHAR)Callbacks + Offset + sizeof(PVOID));
    }
}

static
VOID
FsRtlpInitFilterCallbackData(_Out_ PFS_FILTER_CALLBACK_DATA Data,
                             _In_ UCHAR Operation,
                             _In_ PDEVICE_OBJECT DeviceObject,
                             _In_ PFILE_OBJECT FileObject,
                             _In_ PFS_FILTER_PARAMETERS Parameters)
{
    RtlZeroMemory(Data, sizeof(*Data));
    Data->SizeOfFsFilterCallbackData = sizeof(*Data);
    Data->Operation = Operation;
    Data->DeviceObject = DeviceObject;
    Data->FileObject = FileObject;
    Data->Parameters = *Parameters;
}

/*
 * Run FsOperation with the filter callbacks of every device in the file
 * system stack around it: the Pre callbacks from the top of the stack down,
 * then the operation, then the Post callbacks from the bottom up with the
 * operation's status.
 *
 * A Pre callback that fails an operation that can fail stops it: the filters
 * above get their Post callback with that status, the filter that failed and
 * everything below it are not called, and the status is returned. Releases
 * cannot fail, so there a failing Pre callback is ignored. A Pre callback that
 * returns STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY performed the operation
 * itself: the filters below and the file system are skipped, and the Post
 * callbacks see success. Any other success status from a Pre callback (such as
 * STATUS_FILE_LOCKED_WITH_WRITERS) is passed back to the caller once the
 * operation itself succeeded.
 */
static
NTSTATUS
FsRtlpCallFilterCallbacks(_In_ PFILE_OBJECT FileObject,
                          _In_ UCHAR Operation,
                          _In_ PFS_FILTER_PARAMETERS Parameters,
                          _In_ BOOLEAN CanFail,
                          _In_ PFSRTLP_FS_OPERATION FsOperation)
{
    FSRTLP_FILTER_CALL Calls[FSRTLP_MAX_FILTER_CALLS];
    FS_FILTER_CALLBACK_DATA Data;
    PFS_FILTER_CALLBACK PreCallback;
    PFS_FILTER_COMPLETION_CALLBACK PostCallback;
    PDEVICE_OBJECT DeviceObject, LowerDeviceObject;
    NTSTATUS Status, OperationStatus, InformationStatus = STATUS_SUCCESS;
    ULONG Count = 0;
    BOOLEAN Kept;

    DeviceObject = IoGetRelatedDeviceObject(FileObject);
    ObReferenceObject(DeviceObject);

    while (DeviceObject != NULL)
    {
        Kept = FALSE;
        FsRtlpGetFilterCallbacks(DeviceObject, Operation, &PreCallback, &PostCallback);

        if (PreCallback != NULL || PostCallback != NULL)
        {
            if (Count == FSRTLP_MAX_FILTER_CALLS)
            {
                DPRINT1("FsRtl: more than %u filters with callbacks, the lower ones are not called\n",
                        FSRTLP_MAX_FILTER_CALLS);
                ObDereferenceObject(DeviceObject);
                break;
            }

            Calls[Count].DeviceObject = DeviceObject;
            Calls[Count].PostCallback = PostCallback;
            Calls[Count].CompletionContext = NULL;

            if (PreCallback != NULL)
            {
                FsRtlpInitFilterCallbackData(&Data, Operation, DeviceObject, FileObject, Parameters);
                Status = PreCallback(&Data, &Calls[Count].CompletionContext);

                if (!NT_SUCCESS(Status))
                {
                    if (CanFail)
                    {
                        ObDereferenceObject(DeviceObject);
                        OperationStatus = Status;
                        goto CallPost;
                    }

                    DPRINT1("FsRtl: filter failed a release (0x%lx), releasing anyway\n", Status);
                }
                else if (Status == STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY)
                {
                    Count++;
                    OperationStatus = STATUS_SUCCESS;
                    goto CallPost;
                }
                else if (Status != STATUS_SUCCESS && InformationStatus == STATUS_SUCCESS)
                {
                    InformationStatus = Status;
                }
            }

            Count++;
            Kept = TRUE;
        }

        LowerDeviceObject = IoGetLowerDeviceObject(DeviceObject);
        if (!Kept)
        {
            ObDereferenceObject(DeviceObject);
        }
        DeviceObject = LowerDeviceObject;
    }

    OperationStatus = FsOperation(FileObject, Parameters);

CallPost:
    while (Count != 0)
    {
        Count--;
        if (Calls[Count].PostCallback != NULL)
        {
            FsRtlpInitFilterCallbackData(&Data, Operation, Calls[Count].DeviceObject, FileObject, Parameters);
            Calls[Count].PostCallback(&Data, OperationStatus, Calls[Count].CompletionContext);
        }
        ObDereferenceObject(Calls[Count].DeviceObject);
    }

    if (OperationStatus == STATUS_SUCCESS)
    {
        return InformationStatus;
    }
    return OperationStatus;
}

static
NTSTATUS
FsRtlpAcquireForSectionSync(_In_ PFILE_OBJECT FileObject,
                            _In_ PFS_FILTER_PARAMETERS Parameters)
{
    PFSRTL_COMMON_FCB_HEADER FcbHeader;
    PFAST_IO_DISPATCH FastDispatch;

    UNREFERENCED_PARAMETER(Parameters);

    FcbHeader = (PFSRTL_COMMON_FCB_HEADER)FileObject->FsContext;
    FastDispatch = IoGetRelatedDeviceObject(FileObject)->DriverObject->FastIoDispatch;

    /* Check if Fast Calls are supported, and check AcquireFileForNtCreateSection */
    if (FastDispatch &&
        FastDispatch->AcquireFileForNtCreateSection)
    {
        /* Call the AcquireFileForNtCreateSection FastIo handler */
        FastDispatch->AcquireFileForNtCreateSection(FileObject);
    }
    else
    {
        /* No FastIo handler, acquire file's resource exclusively */
        if (FcbHeader && FcbHeader->Resource) ExAcquireResourceExclusiveLite(FcbHeader->Resource, TRUE);
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
FsRtlpReleaseForSectionSync(_In_ PFILE_OBJECT FileObject,
                            _In_ PFS_FILTER_PARAMETERS Parameters)
{
    PFSRTL_COMMON_FCB_HEADER FcbHeader;
    PFAST_IO_DISPATCH FastDispatch;

    UNREFERENCED_PARAMETER(Parameters);

    FcbHeader = (PFSRTL_COMMON_FCB_HEADER)FileObject->FsContext;
    FastDispatch = IoGetRelatedDeviceObject(FileObject)->DriverObject->FastIoDispatch;

    /* Check if Fast Calls are supported and check ReleaseFileForNtCreateSection */
    if (FastDispatch &&
        FastDispatch->ReleaseFileForNtCreateSection)
    {
        /* Call the ReleaseFileForNtCreateSection FastIo handler */
        FastDispatch->ReleaseFileForNtCreateSection(FileObject);
    }
    else
    {
        /* No FastIo handler, release file's resource */
        if (FcbHeader && FcbHeader->Resource) ExReleaseResourceLite(FcbHeader->Resource);
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
FsRtlpAcquireForCcFlush(_In_ PFILE_OBJECT FileObject,
                        _In_ PFS_FILTER_PARAMETERS Parameters)
{
    PFSRTL_COMMON_FCB_HEADER FcbHeader;
    PDEVICE_OBJECT BaseDeviceObject;
    PFAST_IO_DISPATCH FastDispatch;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Parameters);

    /* Get the Base File System (Volume) and Fast Calls */
    FcbHeader = (PFSRTL_COMMON_FCB_HEADER)FileObject->FsContext;
    BaseDeviceObject = IoGetBaseFileSystemDeviceObject(FileObject);
    FastDispatch = IoGetRelatedDeviceObject(FileObject)->DriverObject->FastIoDispatch;

    /* Check if Fast Calls are supported, and check AcquireForCcFlush */
    if (FastDispatch &&
        FastDispatch->AcquireForCcFlush)
    {
        /* Call the AcquireForCcFlush FastIo handler */
        Status = FastDispatch->AcquireForCcFlush(FileObject, BaseDeviceObject);

        /* Return either success or inability to wait.
           In case of other failure - fall through */
        if (NT_SUCCESS(Status))
            return Status;

        if (Status == STATUS_CANT_WAIT)
        {
            DPRINT1("STATUS_CANT_WAIT\n");
            return Status;
        }
    }

    /* No FastIo handler (or it failed). Acquire Main resource */
    if (FcbHeader->Resource)
    {
        /* Acquire it - either shared if it's already acquired
            or exclusively if we are the first */
        if (ExIsResourceAcquiredSharedLite(FcbHeader->Resource))
            ExAcquireResourceSharedLite(FcbHeader->Resource, TRUE);
        else
            ExAcquireResourceExclusiveLite(FcbHeader->Resource, TRUE);
    }

    /* Also acquire its PagingIO resource */
    if (FcbHeader->PagingIoResource)
        ExAcquireResourceSharedLite(FcbHeader->PagingIoResource, TRUE);

    return STATUS_SUCCESS;
}

static
NTSTATUS
FsRtlpReleaseForCcFlush(_In_ PFILE_OBJECT FileObject,
                        _In_ PFS_FILTER_PARAMETERS Parameters)
{
    PFSRTL_COMMON_FCB_HEADER FcbHeader;
    PDEVICE_OBJECT BaseDeviceObject;
    PFAST_IO_DISPATCH FastDispatch;
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;

    UNREFERENCED_PARAMETER(Parameters);

    /* Get Device Object and Fast Calls */
    FcbHeader = (PFSRTL_COMMON_FCB_HEADER)FileObject->FsContext;
    BaseDeviceObject = IoGetBaseFileSystemDeviceObject(FileObject);
    FastDispatch = IoGetRelatedDeviceObject(FileObject)->DriverObject->FastIoDispatch;

    /* Check if Fast Calls are supported, and check ReleaseForCcFlush */
    if (FastDispatch &&
        FastDispatch->ReleaseForCcFlush)
    {
        /* Call the ReleaseForCcFlush FastIo handler */
        Status = FastDispatch->ReleaseForCcFlush(FileObject, BaseDeviceObject);
    }

    if (!NT_SUCCESS(Status))
    {
        /* No FastIo handler (or it failed). Release PagingIO resource and
           then Main resource */
        if (FcbHeader->PagingIoResource) ExReleaseResourceLite(FcbHeader->PagingIoResource);
        if (FcbHeader->Resource) ExReleaseResourceLite(FcbHeader->Resource);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Get the resource to acquire when Mod Writer flushes data to disk
 *
 * @param FcbHeader - FCB header from the file object
 * @param EndingOffset - The end offset of the write to be done
 * @param ResourceToAcquire - Pointer receiving the resource to acquire before doing the write
 *
 * @return BOOLEAN specifying whether the resource must be acquired exclusively
 */
static
BOOLEAN
FsRtlpGetResourceForModWrite(_In_ PFSRTL_COMMON_FCB_HEADER FcbHeader,
                             _In_ PLARGE_INTEGER EndingOffset,
                             _Outptr_result_maybenull_ PERESOURCE* ResourceToAcquire)
{
    /*
     * Decide on type of locking and type of resource based on
     *  - Flags
     *  - Whether we're extending ValidDataLength
     */
    if (FlagOn(FcbHeader->Flags, FSRTL_FLAG_ACQUIRE_MAIN_RSRC_EX))
    {
        /* Acquire main resource, exclusive */
        *ResourceToAcquire = FcbHeader->Resource;
        return TRUE;
    }

    /* We will acquire shared. Which one ? */
    if (FlagOn(FcbHeader->Flags, FSRTL_FLAG_ACQUIRE_MAIN_RSRC_SH))
    {
        *ResourceToAcquire = FcbHeader->Resource;
    }
    else
    {
        *ResourceToAcquire = FcbHeader->PagingIoResource;
    }

    /* We force exclusive lock if this write modifies the valid data length */
    if (EndingOffset->QuadPart > FcbHeader->ValidDataLength.QuadPart &&
        FcbHeader->FileSize.QuadPart > FcbHeader->ValidDataLength.QuadPart)
    {
        if (FcbHeader->Resource != NULL)
            *ResourceToAcquire = FcbHeader->Resource;
        return TRUE;
    }

    return FALSE;
}

static
NTSTATUS
FsRtlpAcquireForModWrite(_In_ PFILE_OBJECT FileObject,
                         _In_ PFS_FILTER_PARAMETERS Parameters)
{
    PLARGE_INTEGER EndingOffset = Parameters->AcquireForModifiedPageWriter.EndingOffset;
    PERESOURCE *ResourceToRelease = Parameters->AcquireForModifiedPageWriter.ResourceToRelease;
    PFSRTL_COMMON_FCB_HEADER FcbHeader;
    PDEVICE_OBJECT BaseDeviceObject;
    PFAST_IO_DISPATCH FastDispatch;
    PERESOURCE ResourceToAcquire = NULL;
    BOOLEAN Exclusive;
    BOOLEAN Result;
    NTSTATUS Status = STATUS_SUCCESS;

    /* Get Device Object and Fast Calls */
    FcbHeader = (PFSRTL_COMMON_FCB_HEADER)FileObject->FsContext;
    BaseDeviceObject = IoGetBaseFileSystemDeviceObject(FileObject);
    FastDispatch = IoGetRelatedDeviceObject(FileObject)->DriverObject->FastIoDispatch;

    /* Check if Fast Calls are supported, and check AcquireForModWrite */
    if (FastDispatch &&
        FastDispatch->AcquireForModWrite)
    {
        /* Call the AcquireForModWrite FastIo handler */
        Status = FastDispatch->AcquireForModWrite(FileObject,
                                                  EndingOffset,
                                                  ResourceToRelease,
                                                  BaseDeviceObject);

        /* Return either success or inability to wait.
           In case of other failure - fall through */
        if (Status == STATUS_SUCCESS ||
            Status == STATUS_CANT_WAIT)
        {
            return Status;
        }
    }

    /* Check what and how we should acquire */
    Exclusive = FsRtlpGetResourceForModWrite(FcbHeader, EndingOffset, &ResourceToAcquire);

    /* Acquire the resource and loop until we're sure we got this right. */
    while (TRUE)
    {
        BOOLEAN OldExclusive;
        PERESOURCE OldResourceToAcquire;

        if (ResourceToAcquire == NULL)
        {
            /*
             * There's nothing to acquire, we can simply return success
             */

            break;
        }

        if (Exclusive)
        {
            Result = ExAcquireResourceExclusiveLite(ResourceToAcquire, FALSE);
        }
        else
        {
            Result = ExAcquireSharedWaitForExclusive(ResourceToAcquire, FALSE);
        }

        if (!Result)
        {
            return STATUS_CANT_WAIT;
        }

        /* Does this still hold true? */
        OldExclusive = Exclusive;
        OldResourceToAcquire = ResourceToAcquire;
        Exclusive = FsRtlpGetResourceForModWrite(FcbHeader, EndingOffset, &ResourceToAcquire);

        if ((OldExclusive == Exclusive) && (OldResourceToAcquire == ResourceToAcquire))
        {
            /* We're good */
            break;
        }

        /* Can we fix this situation? */
        if ((OldResourceToAcquire == ResourceToAcquire) && !Exclusive)
        {
            /* We can easily do so */
            ExConvertExclusiveToSharedLite(ResourceToAcquire);
            break;
        }

        /* Things have changed since we acquired the lock. Start again */
        ExReleaseResourceLite(OldResourceToAcquire);
    }

    /* If we're here, this means that we succeeded */
    *ResourceToRelease = ResourceToAcquire;
    return STATUS_SUCCESS;
}

static
NTSTATUS
FsRtlpReleaseForModWrite(_In_ PFILE_OBJECT FileObject,
                         _In_ PFS_FILTER_PARAMETERS Parameters)
{
    PERESOURCE ResourceToRelease = Parameters->ReleaseForModifiedPageWriter.ResourceToRelease;
    PDEVICE_OBJECT BaseDeviceObject;
    PFAST_IO_DISPATCH FastDispatch;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    /* Get Device Object and Fast Calls */
    BaseDeviceObject = IoGetBaseFileSystemDeviceObject(FileObject);
    FastDispatch = IoGetRelatedDeviceObject(FileObject)->DriverObject->FastIoDispatch;

    /* Check if Fast Calls are supported and check ReleaseForModWrite */
    if (FastDispatch &&
        FastDispatch->ReleaseForModWrite)
    {
        /* Call the ReleaseForModWrite FastIo handler */
        Status = FastDispatch->ReleaseForModWrite(FileObject,
                                                  ResourceToRelease,
                                                  BaseDeviceObject);
    }

    /* Just release the resource if previous op failed */
    if (!NT_SUCCESS(Status))
    {
        ExReleaseResourceLite(ResourceToRelease);
    }

    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS **********************************************************/

NTSTATUS
NTAPI
FsRtlAcquireFileExclusiveCommon(IN PFILE_OBJECT FileObject,
                                IN FS_FILTER_SECTION_SYNC_TYPE SyncType,
                                IN ULONG Reserved)
{
    FS_FILTER_PARAMETERS Parameters;
    NTSTATUS Status;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.AcquireForSectionSynchronization.SyncType = SyncType;
    Parameters.AcquireForSectionSynchronization.PageProtection = Reserved;

    /* Get master FsRtl lock; FsRtlReleaseFile leaves it */
    FsRtlEnterFileSystem();

    Status = FsRtlpCallFilterCallbacks(FileObject,
                                       FS_FILTER_ACQUIRE_FOR_SECTION_SYNCHRONIZATION,
                                       &Parameters,
                                       TRUE,
                                       FsRtlpAcquireForSectionSync);
    if (!NT_SUCCESS(Status))
    {
        FsRtlExitFileSystem();
    }

    return Status;
}

/*
* @implemented
*/
VOID
NTAPI
FsRtlAcquireFileExclusive(IN PFILE_OBJECT FileObject)
{
    PAGED_CODE();

    /* Call the common routine. Don't care about the result */
    (VOID)FsRtlAcquireFileExclusiveCommon(FileObject, SyncTypeOther, 0);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
FsRtlAcquireToCreateMappedSection(_In_ PFILE_OBJECT FileObject,
                                  _In_ ULONG SectionPageProtection)
{
    PAGED_CODE();

    return FsRtlAcquireFileExclusiveCommon(FileObject, SyncTypeCreateSection, SectionPageProtection);
}

/*
* @implemented
*/
VOID
NTAPI
FsRtlReleaseFile(IN PFILE_OBJECT FileObject)
{
    FS_FILTER_PARAMETERS Parameters;

    RtlZeroMemory(&Parameters, sizeof(Parameters));

    (VOID)FsRtlpCallFilterCallbacks(FileObject,
                                    FS_FILTER_RELEASE_FOR_SECTION_SYNCHRONIZATION,
                                    &Parameters,
                                    FALSE,
                                    FsRtlpReleaseForSectionSync);

    /* Release master FsRtl lock */
    FsRtlExitFileSystem();
}

/*
* @implemented
*/
NTSTATUS
NTAPI
FsRtlAcquireFileForCcFlushEx(IN PFILE_OBJECT FileObject)
{
    FS_FILTER_PARAMETERS Parameters;
    NTSTATUS Status;

    RtlZeroMemory(&Parameters, sizeof(Parameters));

    /* Get master FsRtl lock; FsRtlReleaseFileForCcFlush leaves it */
    FsRtlEnterFileSystem();

    Status = FsRtlpCallFilterCallbacks(FileObject,
                                       FS_FILTER_ACQUIRE_FOR_CC_FLUSH,
                                       &Parameters,
                                       TRUE,
                                       FsRtlpAcquireForCcFlush);
    if (!NT_SUCCESS(Status))
    {
        FsRtlExitFileSystem();
    }

    return Status;
}

/*
* @implemented
*/
VOID
NTAPI
FsRtlAcquireFileForCcFlush(IN PFILE_OBJECT FileObject)
{
    PAGED_CODE();

    /* Call the common routine. Don't care about the result */
    (VOID)FsRtlAcquireFileForCcFlushEx(FileObject);
}

/*
* @implemented
*/
VOID
NTAPI
FsRtlReleaseFileForCcFlush(IN PFILE_OBJECT FileObject)
{
    FS_FILTER_PARAMETERS Parameters;

    RtlZeroMemory(&Parameters, sizeof(Parameters));

    (VOID)FsRtlpCallFilterCallbacks(FileObject,
                                    FS_FILTER_RELEASE_FOR_CC_FLUSH,
                                    &Parameters,
                                    FALSE,
                                    FsRtlpReleaseForCcFlush);

    /* Release master FsRtl lock */
    FsRtlExitFileSystem();
}

/**
 * @brief Lock a file object before flushing pages to disk.
 * To be called by the Modified Page Writer (MPW)
 *
 * @param FileObject - The file object to lock
 * @param EndingOffset - The end offset of the write to be done
 * @param ResourceToRelease - Pointer receiving the resource to release after the write
 *
 * @return Relevant NTSTATUS value
 */
_Check_return_
NTSTATUS
NTAPI
FsRtlAcquireFileForModWriteEx(_In_ PFILE_OBJECT FileObject,
                              _In_ PLARGE_INTEGER EndingOffset,
                              _Outptr_result_maybenull_ PERESOURCE *ResourceToRelease)
{
    FS_FILTER_PARAMETERS Parameters;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.AcquireForModifiedPageWriter.EndingOffset = EndingOffset;
    Parameters.AcquireForModifiedPageWriter.ResourceToRelease = ResourceToRelease;

    return FsRtlpCallFilterCallbacks(FileObject,
                                     FS_FILTER_ACQUIRE_FOR_MOD_WRITE,
                                     &Parameters,
                                     TRUE,
                                     FsRtlpAcquireForModWrite);
}

/**
 * @brief Unlock a file object after flushing pages to disk.
 * To be called by the Modified Page Writer (MPW) after a call to FsRtlAcquireFileForModWriteEx
 *
 * @param FileObject - The file object to unlock
 * @param ResourceToRelease - The resource to release
 */
VOID
NTAPI
FsRtlReleaseFileForModWrite(_In_ PFILE_OBJECT FileObject,
                            _In_ PERESOURCE ResourceToRelease)
{
    FS_FILTER_PARAMETERS Parameters;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.ReleaseForModifiedPageWriter.ResourceToRelease = ResourceToRelease;

    (VOID)FsRtlpCallFilterCallbacks(FileObject,
                                    FS_FILTER_RELEASE_FOR_MOD_WRITE,
                                    &Parameters,
                                    FALSE,
                                    FsRtlpReleaseForModWrite);
}

/*++
 * @name FsRtlRegisterFileSystemFilterCallbacks
 * @implemented
 *
 * Registers the file system filter callbacks of a filter driver. They are
 * called around the acquire/release operations above for every device of
 * that driver in a file system stack.
 *
 * @param FilterDriverObject
 *        The filter driver
 *
 * @param Callbacks
 *        The callbacks; SizeOfFsFilterCallbacks bytes of it are copied
 *
 * @return STATUS_SUCCESS, STATUS_INVALID_PARAMETER or
 *         STATUS_INSUFFICIENT_RESOURCES
 *
 *--*/
NTSTATUS
NTAPI
FsRtlRegisterFileSystemFilterCallbacks(
    PDRIVER_OBJECT FilterDriverObject,
    PFS_FILTER_CALLBACKS Callbacks)
{
    PFS_FILTER_CALLBACKS NewCallbacks;
    PEXTENDED_DRIVER_EXTENSION DriverExtension;
    PAGED_CODE();

    /* Verify parameters: the copy must at least hold its own header */
    if ((FilterDriverObject == NULL) || (Callbacks == NULL) ||
        (Callbacks->SizeOfFsFilterCallbacks < FIELD_OFFSET(FS_FILTER_CALLBACKS, PreAcquireForSectionSynchronization)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Allocate a buffer for a copy of the callbacks */
    NewCallbacks = ExAllocatePoolWithTag(NonPagedPool,
                                         Callbacks->SizeOfFsFilterCallbacks,
                                         'gmSF');
    if (NewCallbacks == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Copy the callbacks */
    RtlCopyMemory(NewCallbacks, Callbacks, Callbacks->SizeOfFsFilterCallbacks);

    /* Set the callbacks in the driver extension */
    DriverExtension = IoGetDrvObjExtension(FilterDriverObject);
    DriverExtension->FsFilterCallbacks = NewCallbacks;

    return STATUS_SUCCESS;
}
