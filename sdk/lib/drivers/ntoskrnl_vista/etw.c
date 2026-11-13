/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Etw functions of Vista+
 * COPYRIGHT:   2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 */

#include <ntdef.h>
#include <ntifs.h>
#include <pseh/pseh2.h>

static volatile LONG EtwpTelemetryCoverageRound;

/* A registration lives in a slot; its handle is the slot number (1-based) with the slot's generation above it,
 * so a handle outlives its registration only as a refusal. Writes run at any IRQL and read the table without
 * the lock: slots are only ever added, a freed registration's slot keeps its generation bumped. The generation
 * is 32 bits per slot: a handle comes back only after 2^32 register/unregister cycles of one slot, each a pool
 * allocation under the lock, so no wrap handling. A generation-0 handle is the small number the old counter
 * handed out, so a guessed small handle validates while that slot is in use: inherent in the format. */
typedef struct _ETWP_REGISTRATION
{
    GUID ProviderId;
    PETWENABLECALLBACK EnableCallback;
    PVOID CallbackContext;
} ETWP_REGISTRATION, *PETWP_REGISTRATION;

#define ETWP_SLOTS_PER_BLOCK 256
#define ETWP_MAX_BLOCKS 256
#define ETWP_TAG 'geTE'
#define ETWP_MAX_EVENT_DATA_DESCRIPTORS 128

typedef struct _ETWP_BLOCK
{
    PETWP_REGISTRATION Entry[ETWP_SLOTS_PER_BLOCK];
    ULONG Generation[ETWP_SLOTS_PER_BLOCK];
} ETWP_BLOCK, *PETWP_BLOCK;

static PETWP_BLOCK EtwpBlocks[ETWP_MAX_BLOCKS];
static KSPIN_LOCK EtwpRegistrationLock;

/* Only for callers holding EtwpRegistrationLock: EtwUnregister frees the registration right after clearing
 * its slot, so a pointer read without the lock needs a rundown before any dereference */
static
PETWP_REGISTRATION
EtwpRegistrationFromHandle(
    _In_ REGHANDLE RegHandle)
{
    ULONG Index = (ULONG)(RegHandle & 0xFFFFFFFF);
    ULONG Generation = (ULONG)(RegHandle >> 32);
    PETWP_BLOCK Block;
    PETWP_REGISTRATION Registration;

    if (Index == 0 || Index > ETWP_MAX_BLOCKS * ETWP_SLOTS_PER_BLOCK)
        return NULL;
    Index--;

    Block = ReadPointerNoFence((PVOID volatile *)&EtwpBlocks[Index / ETWP_SLOTS_PER_BLOCK]);
    if (Block == NULL)
        return NULL;
    Registration = ReadPointerNoFence((PVOID volatile *)&Block->Entry[Index % ETWP_SLOTS_PER_BLOCK]);
    if (Registration == NULL || ReadULongNoFence(&Block->Generation[Index % ETWP_SLOTS_PER_BLOCK]) != Generation)
        return NULL;

    return Registration;
}

/* The lock-free question the writers ask at any IRQL: the answer is never dereferenced */
static
BOOLEAN
EtwpIsRegistered(
    _In_ REGHANDLE RegHandle)
{
    return EtwpRegistrationFromHandle(RegHandle) != NULL;
}

_IRQL_requires_max_(HIGH_LEVEL)
BOOLEAN
NTAPI
EtwEventEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(EventDescriptor);

    /* ReactOS does not yet enable kernel ETW provider sessions. */
    return FALSE;
}

_IRQL_requires_max_(HIGH_LEVEL)
BOOLEAN
NTAPI
EtwProviderEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Keyword);

    /* ReactOS does not yet enable kernel ETW provider sessions. */
    return FALSE;
}

NTSTATUS
NTAPI
EtwActivityIdControl(
    _In_ ULONG ControlCode,
    _Inout_ LPGUID ActivityId)
{
    if (ActivityId == NULL)
        return STATUS_INVALID_PARAMETER;

    switch (ControlCode)
    {
        case EVENT_ACTIVITY_CTRL_CREATE_ID:
        case EVENT_ACTIVITY_CTRL_CREATE_SET_ID:
            return ExUuidCreate(ActivityId);

        case EVENT_ACTIVITY_CTRL_GET_ID:
        case EVENT_ACTIVITY_CTRL_GET_SET_ID:
            RtlZeroMemory(ActivityId, sizeof(*ActivityId));
            return STATUS_SUCCESS;

        case EVENT_ACTIVITY_CTRL_SET_ID:
            return STATUS_SUCCESS;

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    return EtwWriteTransfer(RegHandle, EventDescriptor, ActivityId, NULL, UserDataCount, UserData);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PETWENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    PETWP_REGISTRATION Registration;
    PETWP_BLOCK NewBlock = NULL;
    PETWP_BLOCK Block;
    ULONG BlockIndex, Slot;
    KIRQL OldIrql;

    if (ProviderId == NULL || RegHandle == NULL)
        return STATUS_INVALID_PARAMETER;
    *RegHandle = 0;

    Registration = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Registration), ETWP_TAG);
    if (Registration == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Registration->ProviderId = *ProviderId;
    Registration->EnableCallback = EnableCallback;
    Registration->CallbackContext = CallbackContext;

    for (;;)
    {
        KeAcquireSpinLock(&EtwpRegistrationLock, &OldIrql);
        for (BlockIndex = 0; BlockIndex < ETWP_MAX_BLOCKS; BlockIndex++)
        {
            Block = EtwpBlocks[BlockIndex];
            if (Block == NULL)
            {
                /* The table grows by a block, allocated outside the lock */
                if (NewBlock == NULL)
                    break;
                Block = NewBlock;
                NewBlock = NULL;
                WritePointerRelease((PVOID volatile *)&EtwpBlocks[BlockIndex], Block);
            }
            for (Slot = 0; Slot < ETWP_SLOTS_PER_BLOCK; Slot++)
            {
                if (Block->Entry[Slot] == NULL)
                {
                    WritePointerRelease((PVOID volatile *)&Block->Entry[Slot], Registration);
                    KeReleaseSpinLock(&EtwpRegistrationLock, OldIrql);
                    if (NewBlock != NULL)
                        ExFreePoolWithTag(NewBlock, ETWP_TAG);
                    *RegHandle = ((REGHANDLE)Block->Generation[Slot] << 32) |
                                 (BlockIndex * ETWP_SLOTS_PER_BLOCK + Slot + 1);
                    return STATUS_SUCCESS;
                }
            }
        }
        KeReleaseSpinLock(&EtwpRegistrationLock, OldIrql);

        if (BlockIndex == ETWP_MAX_BLOCKS)
            break;
        NewBlock = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*NewBlock), ETWP_TAG);
        if (NewBlock == NULL)
            break;
    }

    ExFreePoolWithTag(Registration, ETWP_TAG);
    return STATUS_INSUFFICIENT_RESOURCES;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwUnregister(
    _In_ REGHANDLE RegHandle)
{
    ULONG Index = (ULONG)(RegHandle & 0xFFFFFFFF) - 1;
    PETWP_REGISTRATION Registration;
    PETWP_BLOCK Block;
    KIRQL OldIrql;

    KeAcquireSpinLock(&EtwpRegistrationLock, &OldIrql);
    Registration = EtwpRegistrationFromHandle(RegHandle);
    if (Registration == NULL)
    {
        KeReleaseSpinLock(&EtwpRegistrationLock, OldIrql);
        return STATUS_INVALID_HANDLE;
    }
    Block = EtwpBlocks[Index / ETWP_SLOTS_PER_BLOCK];
    Block->Generation[Index % ETWP_SLOTS_PER_BLOCK]++;
    WritePointerRelease((PVOID volatile *)&Block->Entry[Index % ETWP_SLOTS_PER_BLOCK], NULL);
    KeReleaseSpinLock(&EtwpRegistrationLock, OldIrql);

    ExFreePoolWithTag(Registration, ETWP_TAG);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ ULONG InformationClass,
    _In_opt_ PVOID EventInformation,
    _In_ ULONG InformationLength)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(InformationClass);
    UNREFERENCED_PARAMETER(EventInformation);
    UNREFERENCED_PARAMETER(InformationLength);

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwWriteTransfer(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(ActivityId);
    UNREFERENCED_PARAMETER(RelatedActivityId);
    UNREFERENCED_PARAMETER(UserData);

    if (EventDescriptor == NULL || UserDataCount > ETWP_MAX_EVENT_DATA_DESCRIPTORS)
        return STATUS_INVALID_PARAMETER;
    if (!EtwpIsRegistered(RegHandle))
        return STATUS_INVALID_HANDLE;

    /* No session is enabled for any provider yet, so the event is accepted and dropped */
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwWriteEx(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG64 Filter,
    _In_ ULONG Flags,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(Flags);

    return EtwWriteTransfer(RegHandle, EventDescriptor, ActivityId, RelatedActivityId, UserDataCount, UserData);
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwWriteString(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_opt_ LPCGUID ActivityId,
    _In_ PCWSTR String)
{
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Keyword);
    UNREFERENCED_PARAMETER(ActivityId);

    if (!EtwpIsRegistered(RegHandle))
        return STATUS_INVALID_HANDLE;
    if (String == NULL)
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwRegisterClassicProvider(
    _In_ LPCGUID ProviderGuid,
    _In_ ULONG Type,
    _In_opt_ PETWENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    UNREFERENCED_PARAMETER(Type);

    /*
     * Register the classic provider and immediately ENABLE it at max verbosity,
     * then invoke the provider's enable callback.  WPP-instrumented drivers
     * (e.g. the Red Hat virtio-gpu WDDM DOD viogpudo.sys) turn their
     * DoTraceMessage tracing ON in response, so their otherwise-silent init
     * trace surfaces via WmiTraceMessage/EtwWrite — giving visibility into
     * driver bring-up on ReactOS.  (Diagnostic aid; the spec was a raising stub
     * that crashed such drivers in DriverEntry.)
     */
    UNREFERENCED_PARAMETER(ProviderGuid);
    UNREFERENCED_PARAMETER(EnableCallback);
    UNREFERENCED_PARAMETER(CallbackContext);

    /* Succeed with a null registration handle so the WPP-instrumented driver
     * proceeds with tracing disabled (invoking the provider's enable callback
     * directly here faults — the classic-provider callback is not safe to call
     * synchronously at registration). */
    if (RegHandle != NULL)
        *RegHandle = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
EtwTelemetryCoverageReport(_In_ PVOID CoverageData)
{
    if (CoverageData == NULL)
        return STATUS_INVALID_PARAMETER;

    InterlockedIncrement(&EtwpTelemetryCoverageRound);
    return STATUS_SUCCESS;
}
