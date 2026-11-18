/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Kernel power requests: PoCreatePowerRequest, PoSetPowerRequest, PoClearPowerRequest, PoDeletePowerRequest
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#include <internal/po_powreq.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

#define TAG_POWER_REQUEST 'qRoP'
#define POP_POWER_REQUEST_SIGNATURE 0x5172506B /* 'kPrQ' */
#define POP_EXECUTION_AWAYMODE_REQUIRED 0x00000040

typedef struct _POP_KERNEL_POWER_REQUEST
{
    ULONG Signature;
    volatile LONG ReferenceCount;
    LIST_ENTRY Link;
    PDEVICE_OBJECT DeviceObject;
    LONG Count[PowerRequestMaximum];
    UNICODE_STRING Reason;
} POP_KERNEL_POWER_REQUEST, *PPOP_KERNEL_POWER_REQUEST;

/* All requests and the per-type counters live under one lock; Set and Clear run up to DISPATCH_LEVEL */
static KSPIN_LOCK PopKernelPowerRequestLock;
static LIST_ENTRY PopKernelPowerRequestList = { &PopKernelPowerRequestList, &PopKernelPowerRequestList };
static ULONG PopKernelPowerRequestActive[PowerRequestMaximum];

/* PRIVATE FUNCTIONS *********************************************************/

static
BOOLEAN
PopIsKernelPowerRequestTypeSupported(
    _In_ POWER_REQUEST_TYPE Type)
{
    /* Display state belongs to the session and is requested from user mode */
    return Type == PowerRequestSystemRequired ||
           Type == PowerRequestAwayModeRequired ||
           Type == PowerRequestExecutionRequired;
}

static
PPOP_KERNEL_POWER_REQUEST
PopKernelPowerRequestFromHandle(
    _In_ PVOID PowerRequest)
{
    PPOP_KERNEL_POWER_REQUEST Request = PowerRequest;

    if (!Request || Request->Signature != POP_POWER_REQUEST_SIGNATURE)
        return NULL;
    return Request;
}

static
VOID
PopDereferenceKernelPowerRequest(
    _In_ PPOP_KERNEL_POWER_REQUEST Request)
{
    if (InterlockedDecrement(&Request->ReferenceCount))
        return;

    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);
    if (Request->DeviceObject)
        ObDereferenceObject(Request->DeviceObject);
    if (Request->Reason.Buffer)
        ExFreePoolWithTag(Request->Reason.Buffer, TAG_POWER_REQUEST);
    ExFreePoolWithTag(Request, TAG_POWER_REQUEST);
}

static
NTSTATUS
PopCaptureKernelPowerRequestReason(
    _In_opt_ PCOUNTED_REASON_CONTEXT Context,
    _Out_ PUNICODE_STRING Reason)
{
    PCUNICODE_STRING Source = NULL;
    ULONG i;

    RtlInitEmptyUnicodeString(Reason, NULL, 0);
    if (!Context)
        return STATUS_SUCCESS;
    if (Context->Version != POWER_REQUEST_CONTEXT_VERSION)
        return STATUS_INVALID_PARAMETER;

    if (Context->Flags == POWER_REQUEST_CONTEXT_SIMPLE_STRING)
    {
        Source = &Context->SimpleString;
    }
    else if (Context->Flags == POWER_REQUEST_CONTEXT_DETAILED_STRING)
    {
        /* Keep the resource file name as the label; the localized strings are resolved by whoever reports */
        Source = &Context->ResourceFileName;
        if (Context->StringCount && !Context->ReasonStrings)
            return STATUS_INVALID_PARAMETER;
        for (i = 0; i < Context->StringCount; i++)
        {
            if (Context->ReasonStrings[i].Length > Context->ReasonStrings[i].MaximumLength ||
                (Context->ReasonStrings[i].Length && !Context->ReasonStrings[i].Buffer))
                return STATUS_INVALID_PARAMETER;
        }
    }
    else if (Context->Flags != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!Source)
        return STATUS_SUCCESS;
    if (!Source->Buffer || !Source->Length || Source->Length > Source->MaximumLength ||
        (Source->Length & (sizeof(WCHAR) - 1)))
        return STATUS_INVALID_PARAMETER;

    Reason->Buffer = ExAllocatePoolWithTag(NonPagedPoolNx, Source->Length, TAG_POWER_REQUEST);
    if (!Reason->Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(Reason->Buffer, Source->Buffer, Source->Length);
    Reason->Length = Reason->MaximumLength = Source->Length;
    return STATUS_SUCCESS;
}

/* INTERNAL INTERFACE ********************************************************/

ULONG
NTAPI
PopQueryKernelPowerRequestCount(
    _In_ POWER_REQUEST_TYPE Type)
{
    ULONG Count;
    KIRQL OldIrql;

    if ((ULONG)Type >= PowerRequestMaximum)
        return 0;
    KeAcquireSpinLock(&PopKernelPowerRequestLock, &OldIrql);
    Count = PopKernelPowerRequestActive[Type];
    KeReleaseSpinLock(&PopKernelPowerRequestLock, OldIrql);
    return Count;
}

EXECUTION_STATE
NTAPI
PopKernelPowerRequestExecutionState(VOID)
{
    EXECUTION_STATE State = 0;

    if (PopQueryKernelPowerRequestCount(PowerRequestSystemRequired) ||
        PopQueryKernelPowerRequestCount(PowerRequestExecutionRequired))
        State |= ES_SYSTEM_REQUIRED;
    if (PopQueryKernelPowerRequestCount(PowerRequestAwayModeRequired))
        State |= POP_EXECUTION_AWAYMODE_REQUIRED;
    return State;
}

BOOLEAN
NTAPI
PopKernelPowerRequestBlocksSleep(VOID)
{
    return PopQueryKernelPowerRequestCount(PowerRequestSystemRequired) != 0 ||
           PopQueryKernelPowerRequestCount(PowerRequestExecutionRequired) != 0;
}

BOOLEAN
NTAPI
PopKernelPowerRequestAwayMode(VOID)
{
    return PopQueryKernelPowerRequestCount(PowerRequestAwayModeRequired) != 0;
}

/* PUBLIC FUNCTIONS **********************************************************/

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
PoCreatePowerRequest(
    _Outptr_ PVOID *PowerRequest,
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PCOUNTED_REASON_CONTEXT Context)
{
    PPOP_KERNEL_POWER_REQUEST Request;
    KIRQL OldIrql;
    NTSTATUS Status;

    PAGED_CODE();

    if (!PowerRequest)
        return STATUS_INVALID_PARAMETER;
    *PowerRequest = NULL;
    if (!DeviceObject)
        return STATUS_INVALID_PARAMETER;

    Request = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Request), TAG_POWER_REQUEST);
    if (!Request)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = PopCaptureKernelPowerRequestReason(Context, &Request->Reason);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Request, TAG_POWER_REQUEST);
        return Status;
    }

    ObReferenceObject(DeviceObject);
    Request->DeviceObject = DeviceObject;
    Request->ReferenceCount = 1;
    Request->Signature = POP_POWER_REQUEST_SIGNATURE;

    KeAcquireSpinLock(&PopKernelPowerRequestLock, &OldIrql);
    InsertTailList(&PopKernelPowerRequestList, &Request->Link);
    KeReleaseSpinLock(&PopKernelPowerRequestLock, OldIrql);

    *PowerRequest = Request;
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
NTAPI
PoSetPowerRequest(
    _Inout_ PVOID PowerRequest,
    _In_ POWER_REQUEST_TYPE Type)
{
    PPOP_KERNEL_POWER_REQUEST Request = PopKernelPowerRequestFromHandle(PowerRequest);
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    if (!Request || (ULONG)Type >= PowerRequestMaximum)
        return STATUS_INVALID_PARAMETER;
    if (!PopIsKernelPowerRequestTypeSupported(Type))
        return STATUS_NOT_SUPPORTED;

    KeAcquireSpinLock(&PopKernelPowerRequestLock, &OldIrql);
    if (Request->Count[Type] == MAXLONG)
    {
        Status = STATUS_INTEGER_OVERFLOW;
    }
    else if (Request->Count[Type]++ == 0)
    {
        PopKernelPowerRequestActive[Type]++;
    }
    KeReleaseSpinLock(&PopKernelPowerRequestLock, OldIrql);
    return Status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
NTAPI
PoClearPowerRequest(
    _Inout_ PVOID PowerRequest,
    _In_ POWER_REQUEST_TYPE Type)
{
    PPOP_KERNEL_POWER_REQUEST Request = PopKernelPowerRequestFromHandle(PowerRequest);
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    if (!Request || (ULONG)Type >= PowerRequestMaximum)
        return STATUS_INVALID_PARAMETER;
    if (!PopIsKernelPowerRequestTypeSupported(Type))
        return STATUS_NOT_SUPPORTED;

    KeAcquireSpinLock(&PopKernelPowerRequestLock, &OldIrql);
    if (Request->Count[Type] == 0)
    {
        /* More clears than sets */
        Status = STATUS_NOT_SUPPORTED;
    }
    else if (--Request->Count[Type] == 0)
    {
        ASSERT(PopKernelPowerRequestActive[Type] > 0);
        PopKernelPowerRequestActive[Type]--;
    }
    KeReleaseSpinLock(&PopKernelPowerRequestLock, OldIrql);
    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
PoDeletePowerRequest(
    _Inout_ PVOID PowerRequest)
{
    PPOP_KERNEL_POWER_REQUEST Request = PopKernelPowerRequestFromHandle(PowerRequest);
    KIRQL OldIrql;
    ULONG Type;

    PAGED_CODE();

    if (!Request)
    {
        DPRINT1("PoDeletePowerRequest: %p is not a power request\n", PowerRequest);
        return;
    }

    /* A request deleted while still set lets go of what it held */
    KeAcquireSpinLock(&PopKernelPowerRequestLock, &OldIrql);
    RemoveEntryList(&Request->Link);
    for (Type = 0; Type < PowerRequestMaximum; Type++)
    {
        if (Request->Count[Type])
        {
            ASSERT(PopKernelPowerRequestActive[Type] > 0);
            PopKernelPowerRequestActive[Type]--;
            Request->Count[Type] = 0;
        }
    }
    Request->Signature = 0;
    KeReleaseSpinLock(&PopKernelPowerRequestLock, OldIrql);

    PopDereferenceKernelPowerRequest(Request);
}
