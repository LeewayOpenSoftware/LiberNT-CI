/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/fsrtl/fsfltcb.c
 * PURPOSE:         File system acquire/release entry points for Mm and Cc,
 *                  and the file system filter callbacks around them
 * PROGRAMMERS:     Dominique Cote (buzdelabuz2@gmail.com)
 *                  Alex Ionescu (alex.ionescu@reactos.org)
 *                  Aleksey Bragin (aleksey@reactos.org)
 *                  (moved unchanged from fastio.c)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PUBLIC FUNCTIONS **********************************************************/

NTSTATUS
NTAPI
FsRtlAcquireFileExclusiveCommon(IN PFILE_OBJECT FileObject,
                                IN FS_FILTER_SECTION_SYNC_TYPE SyncType,
                                IN ULONG Reserved)
{
    PFSRTL_COMMON_FCB_HEADER FcbHeader;
    PDEVICE_OBJECT DeviceObject;
    PFAST_IO_DISPATCH FastDispatch;
    PEXTENDED_DRIVER_EXTENSION DriverExtension;
    PFS_FILTER_CALLBACKS FilterCallbacks;

    /* Get Device Object and Fast Calls */
    FcbHeader = (PFSRTL_COMMON_FCB_HEADER)FileObject->FsContext;
    DeviceObject = IoGetRelatedDeviceObject(FileObject);

    /* Get master FsRtl lock */
    FsRtlEnterFileSystem();

    DriverExtension = IoGetDrvObjExtension(DeviceObject->DriverObject);
    FilterCallbacks = DriverExtension->FsFilterCallbacks;

    /* Check if Filter Callbacks are supported */
    if (FilterCallbacks && FilterCallbacks->PreAcquireForSectionSynchronization)
    {
        NTSTATUS Status;
        PVOID CompletionContext;

        FS_FILTER_CALLBACK_DATA CbData;

        RtlZeroMemory(&CbData, sizeof(CbData));

        CbData.SizeOfFsFilterCallbackData = sizeof(CbData);
        CbData.Operation = FS_FILTER_ACQUIRE_FOR_SECTION_SYNCHRONIZATION;
        CbData.DeviceObject = DeviceObject;
        CbData.FileObject = FileObject;
        CbData.Parameters.AcquireForSectionSynchronization.PageProtection = Reserved;
        CbData.Parameters.AcquireForSectionSynchronization.SyncType = SyncType;

        Status = FilterCallbacks->PreAcquireForSectionSynchronization(&CbData, &CompletionContext);
        if (!NT_SUCCESS(Status))
        {
            FsRtlExitFileSystem();
            return Status;
        }

        /* Should we do something in-between ? */

        if (FilterCallbacks->PostAcquireForSectionSynchronization)
        {
            FilterCallbacks->PostAcquireForSectionSynchronization(&CbData, Status, CompletionContext);
        }

        /* Return here when the status is based on the synchonization type and write access to the file */
        if (Status == STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY ||
            Status == STATUS_FILE_LOCKED_WITH_ONLY_READERS ||
            Status == STATUS_FILE_LOCKED_WITH_WRITERS)
        {
            return Status;
        }
    }

    FastDispatch = DeviceObject->DriverObject->FastIoDispatch;

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
    PFSRTL_COMMON_FCB_HEADER FcbHeader;
    PDEVICE_OBJECT DeviceObject;
    PFAST_IO_DISPATCH FastDispatch;

    /* Get Device Object and Fast Calls */
    FcbHeader = (PFSRTL_COMMON_FCB_HEADER)FileObject->FsContext;
    DeviceObject = IoGetRelatedDeviceObject(FileObject);
    FastDispatch = DeviceObject->DriverObject->FastIoDispatch;

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
    PFSRTL_COMMON_FCB_HEADER FcbHeader;
    PDEVICE_OBJECT DeviceObject, BaseDeviceObject;
    PFAST_IO_DISPATCH FastDispatch;
    NTSTATUS Status;

    /* Get the Base File System (Volume) and Fast Calls */
    FcbHeader = (PFSRTL_COMMON_FCB_HEADER)FileObject->FsContext;
    DeviceObject = IoGetRelatedDeviceObject(FileObject);
    BaseDeviceObject = IoGetBaseFileSystemDeviceObject(FileObject);
    FastDispatch = DeviceObject->DriverObject->FastIoDispatch;

    /* Get master FsRtl lock */
    FsRtlEnterFileSystem();

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
            FsRtlExitFileSystem();
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
    PFSRTL_COMMON_FCB_HEADER FcbHeader;
    PDEVICE_OBJECT DeviceObject, BaseDeviceObject;
    PFAST_IO_DISPATCH FastDispatch;
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;

    /* Get Device Object and Fast Calls */
    FcbHeader = (PFSRTL_COMMON_FCB_HEADER)FileObject->FsContext;
    DeviceObject = IoGetRelatedDeviceObject(FileObject);
    BaseDeviceObject = IoGetBaseFileSystemDeviceObject(FileObject);
    FastDispatch = DeviceObject->DriverObject->FastIoDispatch;

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

    /* Release master FsRtl lock */
    FsRtlExitFileSystem();
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

/**
 * @brief Lock a file object before flushing pages to disk. 
 *        To be called by the Modified Page Writer (MPW)
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
    PFSRTL_COMMON_FCB_HEADER FcbHeader;
    PDEVICE_OBJECT DeviceObject, BaseDeviceObject;
    PFAST_IO_DISPATCH FastDispatch;
    PERESOURCE ResourceToAcquire = NULL;
    BOOLEAN Exclusive;
    BOOLEAN Result;
    NTSTATUS Status = STATUS_SUCCESS;

    /* Get Device Object and Fast Calls */
    FcbHeader = (PFSRTL_COMMON_FCB_HEADER)FileObject->FsContext;
    DeviceObject = IoGetRelatedDeviceObject(FileObject);
    BaseDeviceObject = IoGetBaseFileSystemDeviceObject(FileObject);
    FastDispatch = DeviceObject->DriverObject->FastIoDispatch;

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

/**
 * @brief Unlock a file object after flushing pages to disk. 
 *        To be called by the Modified Page Writer (MPW) after a succesful call to
 *        FsRtlAcquireFileForModWriteEx
 *
 * @param FileObject - The file object to unlock
 * @param ResourceToRelease - The resource to release
 */
VOID
NTAPI
FsRtlReleaseFileForModWrite(_In_ PFILE_OBJECT FileObject,
                            _In_ PERESOURCE ResourceToRelease)
{
    PDEVICE_OBJECT DeviceObject, BaseDeviceObject;
    PFAST_IO_DISPATCH FastDispatch;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    /* Get Device Object and Fast Calls */
    DeviceObject = IoGetRelatedDeviceObject(FileObject);
    BaseDeviceObject = IoGetBaseFileSystemDeviceObject(FileObject);
    FastDispatch = DeviceObject->DriverObject->FastIoDispatch;

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
}


/*++
 * @name FsRtlRegisterFileSystemFilterCallbacks
 * @unimplemented
 *
 * FILLME
 *
 * @param FilterDriverObject
 *        FILLME
 *
 * @param Callbacks
 *        FILLME
 *
 * @return None
 *
 * @remarks None
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

    /* Verify parameters */
    if ((FilterDriverObject == NULL) || (Callbacks == NULL))
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
