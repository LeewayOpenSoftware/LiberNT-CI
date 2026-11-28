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

#define VF_IO_CALL_CANCEL_ROUTINE       0x203
#define VF_IO_CALL_QUEUED_BELOW         0x205
#define VF_IO_COMPLETE_QUEUED_BELOW     0x209
#define VF_IO_FREE_IN_USE               0x20B
#define VF_IO_REINITIALIZE_IN_USE       0x310

#define VF_IRP_BUCKETS                  256
#define VF_IRP_TAG                      'iTfV'
#define VF_IRP_LINES_PRINTED            64

typedef struct _VF_IRP_ENTRY
{
    LIST_ENTRY Link;
    PIRP Irp;
    PVOID Sender;
    PDEVICE_OBJECT Target;
    ULONG Sequence;
    BOOLEAN Pended;
} VF_IRP_ENTRY, *PVF_IRP_ENTRY;

BOOLEAN VfIrpTrackingEnabled;
ULONG VfIrpRuleHits;
ULONG VfIrpUntrackSkipped;

static LIST_ENTRY VfpIrpBucket[VF_IRP_BUCKETS];
static KSPIN_LOCK VfpIrpLock;
static ULONG VfpIrpSequence;
static LONG VfpIrpTracked;

static
PLIST_ENTRY
VfpIrpHead(
    _In_ PIRP Irp)
{
    ULONG_PTR Value = (ULONG_PTR)Irp;

    return &VfpIrpBucket[((Value >> PAGE_SHIFT) ^ (Value >> 6)) & (VF_IRP_BUCKETS - 1)];
}

static
PVF_IRP_ENTRY
VfpFindIrpLocked(
    _In_ PIRP Irp)
{
    PLIST_ENTRY Head = VfpIrpHead(Irp);
    PLIST_ENTRY Next;
    PVF_IRP_ENTRY Entry;

    for (Next = Head->Flink; Next != Head; Next = Next->Flink)
    {
        Entry = CONTAINING_RECORD(Next, VF_IRP_ENTRY, Link);
        if (Entry->Irp == Irp)
            return Entry;
    }

    return NULL;
}

VOID
NTAPI
VfInitializeIrpTracking(VOID)
{
    ULONG i;

    for (i = 0; i < VF_IRP_BUCKETS; i++)
        InitializeListHead(&VfpIrpBucket[i]);

    KeInitializeSpinLock(&VfpIrpLock);
    VfIrpTrackingEnabled = TRUE;
}

VOID
NTAPI
VfUntrackIrp(
    _In_ PIRP Irp)
{
    PVF_IRP_ENTRY Entry;
    KIRQL OldIrql;

    if (VfpIrpTracked == 0)
        return;

    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        if (InterlockedIncrement((PLONG)&VfIrpUntrackSkipped) == 1)
            DbgPrint("Driver Verifier: IRP %p left above DISPATCH_LEVEL, its tracking entry may be stale\n", Irp);

        return;
    }

    KeAcquireSpinLock(&VfpIrpLock, &OldIrql);
    Entry = VfpFindIrpLocked(Irp);
    if (Entry != NULL)
    {
        RemoveEntryList(&Entry->Link);
        InterlockedDecrement(&VfpIrpTracked);
    }
    KeReleaseSpinLock(&VfpIrpLock, OldIrql);

    if (Entry != NULL)
        ExFreePoolWithTag(Entry, VF_IRP_TAG);
}

static
BOOLEAN
VfpIsIrpBelow(
    _In_ PIRP Irp)
{
    PVF_IRP_ENTRY Entry;
    BOOLEAN Below;
    KIRQL OldIrql;

    if (!VfIrpTrackingEnabled || VfpIrpTracked == 0)
        return FALSE;

    KeAcquireSpinLock(&VfpIrpLock, &OldIrql);
    Entry = VfpFindIrpLocked(Irp);
    Below = (BOOLEAN)(Entry != NULL && Entry->Pended);
    KeReleaseSpinLock(&VfpIrpLock, OldIrql);
    return Below;
}

static
ULONG
VfpIrpSent(
    _In_ PIRP Irp,
    _In_ PDEVICE_OBJECT Target,
    _In_ PVOID Sender)
{
    PVF_IRP_ENTRY Entry, Fresh;
    ULONG Sequence;
    KIRQL OldIrql;

    Fresh = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Fresh), VF_IRP_TAG);

    KeAcquireSpinLock(&VfpIrpLock, &OldIrql);
    Entry = VfpFindIrpLocked(Irp);
    if (Entry == NULL && Fresh != NULL)
    {
        Entry = Fresh;
        Fresh = NULL;
        Entry->Irp = Irp;
        InsertHeadList(VfpIrpHead(Irp), &Entry->Link);
        InterlockedIncrement(&VfpIrpTracked);
    }

    Sequence = 0;
    if (Entry != NULL)
    {
        if (++VfpIrpSequence == 0)
            VfpIrpSequence = 1;

        Sequence = VfpIrpSequence;
        Entry->Sequence = Sequence;
        Entry->Sender = Sender;
        Entry->Target = Target;
        Entry->Pended = FALSE;
    }
    KeReleaseSpinLock(&VfpIrpLock, OldIrql);

    if (Fresh != NULL)
        ExFreePoolWithTag(Fresh, VF_IRP_TAG);

    return Sequence;
}

static
VOID
VfpIrpReturned(
    _In_ PIRP Irp,
    _In_ ULONG Sequence,
    _In_ NTSTATUS Status)
{
    PVF_IRP_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&VfpIrpLock, &OldIrql);
    Entry = VfpFindIrpLocked(Irp);
    if (Entry != NULL && Entry->Sequence == Sequence && Status == STATUS_PENDING)
    {
        Entry->Pended = TRUE;
        Entry = NULL;
    }
    else if (Entry != NULL && Entry->Sequence == Sequence)
    {
        RemoveEntryList(&Entry->Link);
        InterlockedDecrement(&VfpIrpTracked);
    }
    else
    {
        Entry = NULL;
    }
    KeReleaseSpinLock(&VfpIrpLock, OldIrql);

    if (Entry != NULL)
        ExFreePoolWithTag(Entry, VF_IRP_TAG);
}

/* Finds the loaded image that holds an address. The list of loaded images is
 * walked with its lock held: a driver can be unloaded on another processor.
 * The first two images are the kernel and the HAL. */
BOOLEAN
NTAPI
VfFindImage(
    _In_ PVOID Address,
    _Out_ PULONG_PTR ImageBase,
    _Out_ PULONG ImageSize,
    _Out_ PBOOLEAN InKernel)
{
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    PLIST_ENTRY Entry;
    BOOLEAN Found = FALSE;
    ULONG Index = 0;
    KIRQL OldIrql;

    *ImageBase = 0;
    *ImageSize = 0;
    *InKernel = FALSE;

    OldIrql = KeAcquireSpinLockRaiseToSynch(&PsLoadedModuleSpinLock);
    for (Entry = PsLoadedModuleList.Flink; Entry != &PsLoadedModuleList; Entry = Entry->Flink)
    {
        LdrEntry = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        Index++;

        if ((ULONG_PTR)Address >= (ULONG_PTR)LdrEntry->DllBase &&
            (ULONG_PTR)Address < (ULONG_PTR)LdrEntry->DllBase + LdrEntry->SizeOfImage)
        {
            *ImageBase = (ULONG_PTR)LdrEntry->DllBase;
            *ImageSize = LdrEntry->SizeOfImage;
            *InKernel = (BOOLEAN)(Index <= 2);
            Found = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&PsLoadedModuleSpinLock, OldIrql);

    return Found;
}

/* The image of a dispatch routine is looked up once and remembered, so that
 * the ownership test does not walk the list of loaded modules for each
 * request. An entry is keyed by the address of the routine. Every unload of an
 * image empties the table: an address can belong to another image later. */
#define VF_DISPATCH_IMAGES 32

typedef struct _VF_DISPATCH_IMAGE
{
    PVOID Routine;
    ULONG_PTR Base;
    ULONG Size;
    BOOLEAN InKernel;
} VF_DISPATCH_IMAGE, *PVF_DISPATCH_IMAGE;

static VF_DISPATCH_IMAGE VfpDispatchImages[VF_DISPATCH_IMAGES];

/* Counts the times the table was emptied. A lookup that walked the list of
 * modules while an image was unloaded must not store what it found. */
static ULONG VfpDispatchGeneration;

/* Called when an image has left the list of loaded modules */
VOID
NTAPI
VfForgetDispatchImages(VOID)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&VfpIrpLock, &OldIrql);
    RtlZeroMemory(VfpDispatchImages, sizeof(VfpDispatchImages));
    VfpDispatchGeneration++;
    KeReleaseSpinLock(&VfpIrpLock, OldIrql);
}

static
BOOLEAN
VfpFindDispatchImage(
    _In_ PVOID Routine,
    _Out_ PULONG_PTR ImageBase,
    _Out_ PULONG ImageSize,
    _Out_ PBOOLEAN InKernel)
{
    PVF_DISPATCH_IMAGE Slot = &VfpDispatchImages[((ULONG_PTR)Routine >> 4) % VF_DISPATCH_IMAGES];
    BOOLEAN Found = FALSE;
    ULONG Generation;
    KIRQL OldIrql;

    KeAcquireSpinLock(&VfpIrpLock, &OldIrql);
    Generation = VfpDispatchGeneration;
    if (Slot->Routine == Routine && Routine != NULL)
    {
        *ImageBase = Slot->Base;
        *ImageSize = Slot->Size;
        *InKernel = Slot->InKernel;
        Found = TRUE;
    }
    KeReleaseSpinLock(&VfpIrpLock, OldIrql);

    if (Found)
        return TRUE;

    if (!VfFindImage(Routine, ImageBase, ImageSize, InKernel))
        return FALSE;

    KeAcquireSpinLock(&VfpIrpLock, &OldIrql);
    if (Generation == VfpDispatchGeneration)
    {
        Slot->Routine = Routine;
        Slot->Base = *ImageBase;
        Slot->Size = *ImageSize;
        Slot->InKernel = *InKernel;
    }
    KeReleaseSpinLock(&VfpIrpLock, OldIrql);

    return TRUE;
}

static
BOOLEAN
VfpCallerOwnsCurrentLocation(
    _In_ PIRP Irp,
    _In_ PVOID Caller)
{
    PIO_STACK_LOCATION Stack;
    PDRIVER_OBJECT Driver;
    ULONG_PTR Start;

    if (Irp->CurrentLocation > Irp->StackCount)
        return FALSE;

    Stack = IoGetCurrentIrpStackLocation(Irp);
    if (Stack->DeviceObject == NULL)
        return FALSE;

    Driver = Stack->DeviceObject->DriverObject;
    if (Driver == NULL)
        return FALSE;

    Start = (ULONG_PTR)Driver->DriverStart;
    if ((ULONG_PTR)Caller >= Start && (ULONG_PTR)Caller < Start + Driver->DriverSize)
        return TRUE;

    /* A framework or port driver handles the requests of its client drivers:
     * the dispatch routine of the client's driver object is in the image of
     * the framework, and so is the call that completes or forwards the
     * request. The caller owns the location if it is in the image that
     * handles this request for the driver. */
    if (Stack->MajorFunction <= IRP_MJ_MAXIMUM_FUNCTION)
    {
        ULONG_PTR DispatchBase;
        ULONG DispatchSize;
        BOOLEAN InKernel;

        if (VfpFindDispatchImage((PVOID)(ULONG_PTR)Driver->MajorFunction[Stack->MajorFunction],
                                 &DispatchBase, &DispatchSize, &InKernel) &&
            !InKernel &&
            (ULONG_PTR)Caller >= DispatchBase &&
            (ULONG_PTR)Caller < DispatchBase + DispatchSize)
        {
            return TRUE;
        }
    }

    return FALSE;
}

static
BOOLEAN
VfpIsIrpRuleFatal(
    _In_ ULONG Rule)
{
    switch (Rule)
    {
        case VF_IO_CALL_CANCEL_ROUTINE:
            return TRUE;
        case VF_IO_CALL_QUEUED_BELOW:
            return FALSE;
        case VF_IO_COMPLETE_QUEUED_BELOW:
            return FALSE;
        case VF_IO_FREE_IN_USE:
            return TRUE;
        case VF_IO_REINITIALIZE_IN_USE:
            return FALSE;
        default:
            return FALSE;
    }
}

static
BOOLEAN
VfpIsIrpTableTrusted(
    _In_ ULONG Rule)
{
    switch (Rule)
    {
        case VF_IO_CALL_QUEUED_BELOW:
        case VF_IO_COMPLETE_QUEUED_BELOW:
        case VF_IO_FREE_IN_USE:
        case VF_IO_REINITIALIZE_IN_USE:
            return (BOOLEAN)(VfIrpUntrackSkipped == 0);
        default:
            return TRUE;
    }
}

static
VOID
VfpReportIrpRule(
    _In_ ULONG Rule,
    _In_ PVOID Caller,
    _In_ PIRP Irp)
{
    ULONG Hits = (ULONG)InterlockedIncrement((PLONG)&VfIrpRuleHits);

    if (VfpIsIrpRuleFatal(Rule) && VfpIsIrpTableTrusted(Rule))
        KeBugCheckEx(DRIVER_VERIFIER_IOMANAGER_VIOLATION, Rule, (ULONG_PTR)Caller, (ULONG_PTR)Irp, 0);

    if (Hits <= VF_IRP_LINES_PRINTED)
    {
        ULONG_PTR ImageBase;
        ULONG ImageSize;
        BOOLEAN InKernel;

        if (VfFindImage(Caller, &ImageBase, &ImageSize, &InKernel))
        {
            DbgPrint("Driver Verifier (log only): I/O rule 0x%lx, IRP %p, caller %p (image %p+0x%lx)\n",
                     Rule, Irp, Caller, (PVOID)ImageBase, (ULONG)((ULONG_PTR)Caller - ImageBase));
        }
        else
        {
            DbgPrint("Driver Verifier (log only): I/O rule 0x%lx, IRP %p, caller %p\n", Rule, Irp, Caller);
        }
    }
}

VOID
NTAPI
VfCheckIrpsAtUnload(
    _In_ PLDR_DATA_TABLE_ENTRY LdrEntry)
{
    ULONG_PTR Start = (ULONG_PTR)LdrEntry->DllBase;
    ULONG_PTR End = Start + LdrEntry->SizeOfImage;
    PVF_IRP_ENTRY Entry;
    PLIST_ENTRY Next;
    ULONG Outstanding = 0;
    KIRQL OldIrql;
    ULONG i;

    if (!VfIrpTrackingEnabled || !(LdrEntry->Flags & LDRP_IMAGE_VERIFYING))
        return;

    KeAcquireSpinLock(&VfpIrpLock, &OldIrql);
    for (i = 0; i < VF_IRP_BUCKETS; i++)
    {
        for (Next = VfpIrpBucket[i].Flink; Next != &VfpIrpBucket[i]; Next = Next->Flink)
        {
            Entry = CONTAINING_RECORD(Next, VF_IRP_ENTRY, Link);
            if ((ULONG_PTR)Entry->Sender >= Start && (ULONG_PTR)Entry->Sender < End)
                Outstanding++;
        }
    }
    KeReleaseSpinLock(&VfpIrpLock, OldIrql);

    if (Outstanding != 0)
    {
        DbgPrint("Driver Verifier (log only): %wZ unloads with %lu IRP(s) it sent still pending below it\n",
                 &LdrEntry->BaseDllName, Outstanding);
    }
}

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
    _In_ PIRP Irp,
    _In_ PVOID Caller)
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

    if (VfpIsIrpBelow(Irp) && !VfpCallerOwnsCurrentLocation(Irp, Caller))
        VfpReportIrpRule(VF_IO_COMPLETE_QUEUED_BELOW, Caller, Irp);
}

static
NTSTATUS
VfpCallDriver(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ PVOID Caller)
{
    KIRQL Before = KeGetCurrentIrql();
    ULONG Sequence = 0;
    NTSTATUS Status;

    VfpCheckCall(DeviceObject, Irp);

    if (VfIrpTrackingEnabled)
    {
        if (Irp->CancelRoutine != NULL)
            VfpReportIrpRule(VF_IO_CALL_CANCEL_ROUTINE, Caller, Irp);

        if (VfpIsIrpBelow(Irp) && !VfpCallerOwnsCurrentLocation(Irp, Caller))
            VfpReportIrpRule(VF_IO_CALL_QUEUED_BELOW, Caller, Irp);

        Sequence = VfpIrpSent(Irp, DeviceObject, Caller);
    }

    Status = IofCallDriver(DeviceObject, Irp);

    if (Sequence != 0)
        VfpIrpReturned(Irp, Sequence, Status);

    VfpCheckCallReturn(DeviceObject, Before);
    return Status;
}

static
NTSTATUS
FASTCALL
VfIofCallDriver(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    return VfpCallDriver(DeviceObject, Irp, _ReturnAddress());
}

static
NTSTATUS
NTAPI
VfIoCallDriver(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    return VfpCallDriver(DeviceObject, Irp, _ReturnAddress());
}

static
VOID
FASTCALL
VfIofCompleteRequest(
    _In_ PIRP Irp,
    _In_ CCHAR PriorityBoost)
{
    VfpCheckComplete(Irp, _ReturnAddress());
    IofCompleteRequest(Irp, PriorityBoost);
}

static
VOID
NTAPI
VfIoCompleteRequest(
    _In_ PIRP Irp,
    _In_ CCHAR PriorityBoost)
{
    VfpCheckComplete(Irp, _ReturnAddress());
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

    if (VfpIsIrpBelow(Irp))
        VfpReportIrpRule(VF_IO_FREE_IN_USE, _ReturnAddress(), Irp);

    IoFreeIrp(Irp);
}

static
VOID
NTAPI
VfIoReuseIrp(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status)
{
    if (VfpIsIrpBelow(Irp))
    {
        VfpReportIrpRule(VF_IO_REINITIALIZE_IN_USE, _ReturnAddress(), Irp);
        VfUntrackIrp(Irp);
    }

    IoReuseIrp(Irp, Status);
}

static
VOID
NTAPI
VfIoInitializeIrp(
    _Inout_ PIRP Irp,
    _In_ USHORT PacketSize,
    _In_ CCHAR StackSize)
{
    if (VfpIsIrpBelow(Irp))
    {
        VfpReportIrpRule(VF_IO_REINITIALIZE_IN_USE, _ReturnAddress(), Irp);
        VfUntrackIrp(Irp);
    }

    IoInitializeIrp(Irp, PacketSize, StackSize);
}

const VF_THUNK VfIoThunks[] =
{
    { (ULONG_PTR)IofCallDriver, (ULONG_PTR)VfIofCallDriver },
    { (ULONG_PTR)IoCallDriver, (ULONG_PTR)VfIoCallDriver },
    { (ULONG_PTR)IofCompleteRequest, (ULONG_PTR)VfIofCompleteRequest },
    { (ULONG_PTR)IoCompleteRequest, (ULONG_PTR)VfIoCompleteRequest },
    { (ULONG_PTR)IoFreeIrp, (ULONG_PTR)VfIoFreeIrp },
    { (ULONG_PTR)IoReuseIrp, (ULONG_PTR)VfIoReuseIrp },
    { (ULONG_PTR)IoInitializeIrp, (ULONG_PTR)VfIoInitializeIrp },
};

const ULONG VfIoThunkCount = RTL_NUMBER_OF(VfIoThunks);
