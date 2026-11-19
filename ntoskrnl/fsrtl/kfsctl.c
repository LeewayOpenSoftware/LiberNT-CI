/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     FSCTL requests sent by file object from kernel mode
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

#define TAG_FSRTL_KERNEL_FSCTL 'cFsF'

/* PRIVATE FUNCTIONS *********************************************************/

static
NTSTATUS
NTAPI
FsRtlpKernelFsControlCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    /* The caller frees the IRP and its buffers */
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* PUBLIC FUNCTIONS **********************************************************/

NTSTATUS
NTAPI
FsRtlKernelFsControlFile(
    _In_ PFILE_OBJECT FileObject,
    _In_ ULONG FsControlCode,
    _In_ PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_ PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG RetOutputBufferSize)
{
    ULONG Method = METHOD_FROM_CTL_CODE(FsControlCode);
    PDEVICE_OBJECT DeviceObject;
    PIO_STACK_LOCATION Stack;
    PVOID SystemBuffer = NULL;
    BOOLEAN Locked = FALSE;
    ULONG_PTR Information;
    PMDL Mdl = NULL;
    NTSTATUS Status = STATUS_SUCCESS;
    KEVENT Event;
    PIRP Irp;

    PAGED_CODE();

    if (FileObject == NULL || RetOutputBufferSize == NULL ||
        (InputBufferLength != 0 && InputBuffer == NULL) ||
        (OutputBufferLength != 0 && OutputBuffer == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }
    *RetOutputBufferSize = 0;

    DeviceObject = IoGetRelatedDeviceObject(FileObject);
    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* All buffers are kernel buffers, so only METHOD_BUFFERED needs a copy */
    switch (Method)
    {
        case METHOD_BUFFERED:
            if (max(InputBufferLength, OutputBufferLength) != 0)
            {
                SystemBuffer = ExAllocatePoolWithTag(NonPagedPool,
                                                     max(InputBufferLength, OutputBufferLength),
                                                     TAG_FSRTL_KERNEL_FSCTL);
                if (SystemBuffer == NULL)
                {
                    IoFreeIrp(Irp);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                if (InputBufferLength != 0)
                    RtlCopyMemory(SystemBuffer, InputBuffer, InputBufferLength);
            }
            Irp->AssociatedIrp.SystemBuffer = SystemBuffer;
            break;

        case METHOD_IN_DIRECT:
        case METHOD_OUT_DIRECT:
            Irp->AssociatedIrp.SystemBuffer = InputBuffer;
            if (OutputBufferLength != 0)
            {
                Mdl = IoAllocateMdl(OutputBuffer, OutputBufferLength, FALSE, FALSE, Irp);
                if (Mdl == NULL)
                {
                    IoFreeIrp(Irp);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                _SEH2_TRY
                {
                    MmProbeAndLockPages(Mdl, KernelMode,
                                        (Method == METHOD_IN_DIRECT) ? IoReadAccess : IoWriteAccess);
                    Locked = TRUE;
                }
                _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
                {
                    Status = _SEH2_GetExceptionCode();
                }
                _SEH2_END;
                if (!Locked)
                {
                    Irp->MdlAddress = NULL;
                    IoFreeMdl(Mdl);
                    IoFreeIrp(Irp);
                    return Status;
                }
            }
            break;

        default:
            Irp->UserBuffer = OutputBuffer;
            break;
    }

    Irp->RequestorMode = KernelMode;
    Irp->Tail.Overlay.OriginalFileObject = FileObject;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();

    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL;
    /* IRP_MN_KERNEL_CALL lets the file system skip the checks meant for user callers */
    Stack->MinorFunction = IRP_MN_KERNEL_CALL;
    Stack->FileObject = FileObject;
    Stack->Parameters.FileSystemControl.OutputBufferLength = OutputBufferLength;
    Stack->Parameters.FileSystemControl.InputBufferLength = InputBufferLength;
    Stack->Parameters.FileSystemControl.FsControlCode = FsControlCode;
    Stack->Parameters.FileSystemControl.Type3InputBuffer = (Method == METHOD_NEITHER) ? InputBuffer : NULL;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoSetCompletionRoutine(Irp, FsRtlpKernelFsControlCompletion, &Event, TRUE, TRUE, TRUE);

    ObReferenceObject(FileObject);
    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

    Status = Irp->IoStatus.Status;
    Information = Irp->IoStatus.Information;
    if (Method == METHOD_BUFFERED || Method == METHOD_IN_DIRECT || Method == METHOD_OUT_DIRECT)
        Information = min(Information, (ULONG_PTR)OutputBufferLength);

    if (Method == METHOD_BUFFERED && SystemBuffer != NULL && !NT_ERROR(Status))
        RtlCopyMemory(OutputBuffer, SystemBuffer, Information);
    if (!NT_ERROR(Status))
        *RetOutputBufferSize = (ULONG)Information;

    if (Mdl != NULL)
    {
        MmUnlockPages(Mdl);
        Irp->MdlAddress = NULL;
        IoFreeMdl(Mdl);
    }
    if (SystemBuffer != NULL)
        ExFreePoolWithTag(SystemBuffer, TAG_FSRTL_KERNEL_FSCTL);
    IoFreeIrp(Irp);
    ObDereferenceObject(FileObject);

    return Status;
}

NTSTATUS
NTAPI
FsRtlIssueDeviceIoControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG IoCtl,
    _In_ UCHAR Flags,
    _In_opt_ PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _In_opt_ PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_opt_ PULONG_PTR IosbInformation)
{
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;
    KEVENT Event;
    PIRP Irp;

    PAGED_CODE();

    if (IosbInformation != NULL)
        *IosbInformation = 0;

    if (DeviceObject == NULL ||
        (InputBufferLength != 0 && InputBuffer == NULL) ||
        (OutputBufferLength != 0 && OutputBuffer == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IoCtl, DeviceObject,
                                        InputBuffer, InputBufferLength,
                                        OutputBuffer, OutputBufferLength,
                                        FALSE, &Event, &IoStatus);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* A UCHAR of flags is the stack location's, such as SL_OVERRIDE_VERIFY_VOLUME */
    IoGetNextIrpStackLocation(Irp)->Flags |= Flags;

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (IosbInformation != NULL)
        *IosbInformation = IoStatus.Information;

    return Status;
}

NTSTATUS
NTAPI
FsRtlQueryInformationFile(
    _In_ PFILE_OBJECT FileObject,
    _Out_writes_bytes_to_(Length, *RetFileInformationSize) PVOID FileInformation,
    _In_ ULONG Length,
    _In_ FILE_INFORMATION_CLASS FileInformationClass,
    _Out_ PULONG RetFileInformationSize)
{
    ULONG Returned = 0;
    NTSTATUS Status;

    PAGED_CODE();

    if (FileObject == NULL || RetFileInformationSize == NULL ||
        (Length != 0 && FileInformation == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = IoQueryFileInformation(FileObject, FileInformationClass, Length,
                                    FileInformation, &Returned);
    *RetFileInformationSize = NT_ERROR(Status) ? 0 : Returned;
    return Status;
}

BOOLEAN
NTAPI
FsRtlIsDaxVolume(
    _In_ PFILE_OBJECT FileObject)
{
    union
    {
        FILE_FS_ATTRIBUTE_INFORMATION Info;
        UCHAR Buffer[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 32 * sizeof(WCHAR)];
    } Attributes;
    ULONG Returned;
    NTSTATUS Status;

    PAGED_CODE();

    if (FileObject == NULL)
        return FALSE;

    /* A long file system name only overflows the buffer; the attributes still arrive */
    RtlZeroMemory(&Attributes, sizeof(Attributes));
    Status = IoQueryVolumeInformation(FileObject, FileFsAttributeInformation,
                                      sizeof(Attributes), &Attributes, &Returned);
    if (NT_ERROR(Status))
        return FALSE;

    return (Attributes.Info.FileSystemAttributes & FILE_DAX_VOLUME) != 0;
}

/* EOF */
