/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver Verifier: I/O verification rules at the IRP calls of a verified driver
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#undef IoCallDriver
#undef IoCompleteRequest

NTSTATUS NTAPI IoCallDriver(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
VOID NTAPI IoCompleteRequest(IN PIRP Irp, IN CCHAR PriorityBoost);

#define VF_IO_FREE_NOT_AN_IRP           0x01
#define VF_IO_FREE_QUEUED_TO_THREAD     0x02
#define VF_IO_CALL_NOT_AN_IRP           0x03
#define VF_IO_CALL_BAD_DEVICE           0x04
#define VF_IO_CALL_IRQL_CHANGED         0x05
#define VF_IO_COMPLETE_PENDING_STATUS   0x06
#define VF_IO_COMPLETE_CANCEL_ROUTINE   0x07
#define VF_IO_COMPLETE_IRQL             0x0E
#define VF_IO_CALL_IRQL                 0x10

static
VOID
VfpCheckCall(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    KIRQL Irql = KeGetCurrentIrql();

    if (Irql > DISPATCH_LEVEL)
        KeBugCheckEx(DRIVER_VERIFIER_IOMANAGER_VIOLATION, VF_IO_CALL_IRQL, Irql, 0, 0);

    if (Irp == NULL || Irp->Type != IO_TYPE_IRP)
        KeBugCheckEx(DRIVER_VERIFIER_IOMANAGER_VIOLATION, VF_IO_CALL_NOT_AN_IRP, (ULONG_PTR)Irp, 0, 0);

    if (DeviceObject == NULL || DeviceObject->Type != IO_TYPE_DEVICE)
        KeBugCheckEx(DRIVER_VERIFIER_IOMANAGER_VIOLATION, VF_IO_CALL_BAD_DEVICE, (ULONG_PTR)DeviceObject, 0, 0);
}

static
VOID
VfpCheckCallReturn(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ KIRQL Before)
{
    KIRQL After = KeGetCurrentIrql();

    if (After != Before)
    {
        KeBugCheckEx(DRIVER_VERIFIER_IOMANAGER_VIOLATION, VF_IO_CALL_IRQL_CHANGED, (ULONG_PTR)DeviceObject, Before,
                     After);
    }
}

static
VOID
VfpCheckComplete(
    _In_ PIRP Irp)
{
    KIRQL Irql = KeGetCurrentIrql();

    if (Irql > DISPATCH_LEVEL)
        KeBugCheckEx(DRIVER_VERIFIER_IOMANAGER_VIOLATION, VF_IO_COMPLETE_IRQL, Irql, (ULONG_PTR)Irp, 0);

    if (Irp->IoStatus.Status == STATUS_PENDING || Irp->IoStatus.Status == (NTSTATUS)-1)
    {
        KeBugCheckEx(DRIVER_VERIFIER_IOMANAGER_VIOLATION, VF_IO_COMPLETE_PENDING_STATUS,
                     (ULONG_PTR)Irp->IoStatus.Status, (ULONG_PTR)Irp, 0);
    }

    if (Irp->CancelRoutine != NULL)
    {
        KeBugCheckEx(DRIVER_VERIFIER_IOMANAGER_VIOLATION, VF_IO_COMPLETE_CANCEL_ROUTINE,
                     (ULONG_PTR)Irp->CancelRoutine, (ULONG_PTR)Irp, 0);
    }
}

static
NTSTATUS
FASTCALL
VfIofCallDriver(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    KIRQL Before = KeGetCurrentIrql();
    NTSTATUS Status;

    VfpCheckCall(DeviceObject, Irp);
    Status = IofCallDriver(DeviceObject, Irp);
    VfpCheckCallReturn(DeviceObject, Before);
    return Status;
}

static
NTSTATUS
NTAPI
VfIoCallDriver(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    KIRQL Before = KeGetCurrentIrql();
    NTSTATUS Status;

    VfpCheckCall(DeviceObject, Irp);
    Status = IofCallDriver(DeviceObject, Irp);
    VfpCheckCallReturn(DeviceObject, Before);
    return Status;
}

static
VOID
FASTCALL
VfIofCompleteRequest(
    _In_ PIRP Irp,
    _In_ CCHAR PriorityBoost)
{
    VfpCheckComplete(Irp);
    IofCompleteRequest(Irp, PriorityBoost);
}

static
VOID
NTAPI
VfIoCompleteRequest(
    _In_ PIRP Irp,
    _In_ CCHAR PriorityBoost)
{
    VfpCheckComplete(Irp);
    IofCompleteRequest(Irp, PriorityBoost);
}

static
VOID
NTAPI
VfIoFreeIrp(
    _In_ PIRP Irp)
{
    if (Irp->Type != IO_TYPE_IRP)
        KeBugCheckEx(DRIVER_VERIFIER_IOMANAGER_VIOLATION, VF_IO_FREE_NOT_AN_IRP, (ULONG_PTR)Irp, 0, 0);

    if (!IsListEmpty(&Irp->ThreadListEntry))
        KeBugCheckEx(DRIVER_VERIFIER_IOMANAGER_VIOLATION, VF_IO_FREE_QUEUED_TO_THREAD, (ULONG_PTR)Irp, 0, 0);

    IoFreeIrp(Irp);
}

const VF_THUNK VfIoThunks[] =
{
    { (ULONG_PTR)IofCallDriver, (ULONG_PTR)VfIofCallDriver },
    { (ULONG_PTR)IoCallDriver, (ULONG_PTR)VfIoCallDriver },
    { (ULONG_PTR)IofCompleteRequest, (ULONG_PTR)VfIofCompleteRequest },
    { (ULONG_PTR)IoCompleteRequest, (ULONG_PTR)VfIoCompleteRequest },
    { (ULONG_PTR)IoFreeIrp, (ULONG_PTR)VfIoFreeIrp },
};

const ULONG VfIoThunkCount = RTL_NUMBER_OF(VfIoThunks);
