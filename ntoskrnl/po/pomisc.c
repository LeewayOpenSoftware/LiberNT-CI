/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Power Manager: effective power mode notifications, user shutdown state
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#include <internal/po_misc.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

#define TAG_PO_EFFMODE 'MEoP'
#define POP_EFFMODE_SIGNATURE 'dMfE'

typedef struct _POP_EFFMODE_REGISTRATION
{
    LIST_ENTRY Link;
    ULONG Signature;
    ULONG Version;
    PPO_EFFECTIVE_POWER_MODE_CALLBACK Callback;
    PVOID Context;
    LONG References;
    ULONG Epoch;
    BOOLEAN Removed;
    BOOLEAN Delivering;
    BOOLEAN Pending;
    PKTHREAD DeliveringThread;
    KEVENT Idle;
} POP_EFFMODE_REGISTRATION, *PPOP_EFFMODE_REGISTRATION;

/* The lock is statically initialised (a spin lock at zero is free); the list is set up on first use */
static KSPIN_LOCK PopEffModeLock;
static LIST_ENTRY PopEffModeList;
static BOOLEAN PopEffModeInitialized;
static ULONG PopEffModeCurrent = PO_EFFECTIVE_POWER_MODE_BALANCED;
static ULONG PopEffModeEpoch;
static volatile LONG PopUserShutdownState;

/* PRIVATE FUNCTIONS *********************************************************/

/* Called with the lock held */
static
VOID
PopEffModeEnsureInitialized(VOID)
{
    if (!PopEffModeInitialized)
    {
        InitializeListHead(&PopEffModeList);
        PopEffModeInitialized = TRUE;
    }
}

static
VOID
PopEffModeDereference(
    _In_ PPOP_EFFMODE_REGISTRATION Registration)
{
    if (InterlockedDecrement(&Registration->References) == 0)
    {
        Registration->Signature = 0;
        ExFreePoolWithTag(Registration, TAG_PO_EFFMODE);
    }
}

/*
 * Tells one registrant the current mode. Deliveries to the same registrant are
 * serialised and the last one it receives is always the latest mode: a mode change
 * that arrives while its callback runs makes the running delivery go round again.
 */
static
VOID
PopEffModeDeliver(
    _In_ PPOP_EFFMODE_REGISTRATION Registration)
{
    KIRQL OldIrql;
    ULONG Mode;

    KeAcquireSpinLock(&PopEffModeLock, &OldIrql);
    if (Registration->Removed)
    {
        KeReleaseSpinLock(&PopEffModeLock, OldIrql);
        return;
    }

    if (Registration->Delivering)
    {
        Registration->Pending = TRUE;
        KeReleaseSpinLock(&PopEffModeLock, OldIrql);
        return;
    }

    Registration->Delivering = TRUE;
    Registration->DeliveringThread = KeGetCurrentThread();
    do
    {
        Registration->Pending = FALSE;
        Mode = PopEffModeCurrent;
        KeReleaseSpinLock(&PopEffModeLock, OldIrql);

        Registration->Callback(Mode, Registration->Context);

        KeAcquireSpinLock(&PopEffModeLock, &OldIrql);
    } while (Registration->Pending && !Registration->Removed);

    Registration->Delivering = FALSE;
    Registration->DeliveringThread = NULL;
    if (Registration->Removed)
        KeSetEvent(&Registration->Idle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&PopEffModeLock, OldIrql);
}

/* PUBLIC FUNCTIONS **********************************************************/

NTSTATUS
NTAPI
PoRegisterForEffectivePowerModeNotifications(
    _In_ ULONG Version,
    _In_ PPO_EFFECTIVE_POWER_MODE_CALLBACK Callback,
    _In_opt_ PVOID Context,
    _Outptr_ PVOID *RegistrationHandle)
{
    PPOP_EFFMODE_REGISTRATION Registration;
    KIRQL OldIrql;
    PAGED_CODE();

    if (RegistrationHandle != NULL)
        *RegistrationHandle = NULL;

    if ((Callback == NULL) || (RegistrationHandle == NULL) ||
        ((Version != PO_EFFECTIVE_POWER_MODE_V1) && (Version != PO_EFFECTIVE_POWER_MODE_V2)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Registration = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Registration), TAG_PO_EFFMODE);
    if (Registration == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Registration, sizeof(*Registration));
    Registration->Signature = POP_EFFMODE_SIGNATURE;
    Registration->Version = Version;
    Registration->Callback = Callback;
    Registration->Context = Context;
    Registration->References = 2; /* the registration itself and this call */
    KeInitializeEvent(&Registration->Idle, NotificationEvent, FALSE);

    KeAcquireSpinLock(&PopEffModeLock, &OldIrql);
    PopEffModeEnsureInitialized();
    Registration->Epoch = PopEffModeEpoch;
    InsertTailList(&PopEffModeList, &Registration->Link);
    KeReleaseSpinLock(&PopEffModeLock, OldIrql);

    /* Like the power setting callbacks, a new registrant is told the current mode at once */
    PopEffModeDeliver(Registration);

    *RegistrationHandle = Registration;
    PopEffModeDereference(Registration);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PoUnregisterFromEffectivePowerModeNotifications(
    _In_ PVOID RegistrationHandle)
{
    PPOP_EFFMODE_REGISTRATION Registration = NULL, Candidate;
    PLIST_ENTRY Entry;
    BOOLEAN Wait = FALSE;
    KIRQL OldIrql;
    PAGED_CODE();

    if (RegistrationHandle == NULL)
        return STATUS_INVALID_PARAMETER;

    KeAcquireSpinLock(&PopEffModeLock, &OldIrql);
    PopEffModeEnsureInitialized();
    for (Entry = PopEffModeList.Flink; Entry != &PopEffModeList; Entry = Entry->Flink)
    {
        Candidate = CONTAINING_RECORD(Entry, POP_EFFMODE_REGISTRATION, Link);
        if ((Candidate == RegistrationHandle) && !Candidate->Removed)
        {
            Registration = Candidate;
            break;
        }
    }

    if (Registration == NULL)
    {
        KeReleaseSpinLock(&PopEffModeLock, OldIrql);
        return STATUS_INVALID_PARAMETER;
    }

    Registration->Removed = TRUE;
    RemoveEntryList(&Registration->Link);
    InitializeListHead(&Registration->Link);
    /* A callback may unregister itself; it cannot wait for its own return */
    Wait = Registration->Delivering && (Registration->DeliveringThread != KeGetCurrentThread());
    if (!Registration->Delivering)
        KeSetEvent(&Registration->Idle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&PopEffModeLock, OldIrql);

    /* After this returns, the callback is not running and will not run again */
    if (Wait)
        KeWaitForSingleObject(&Registration->Idle, Executive, KernelMode, FALSE, NULL);

    PopEffModeDereference(Registration);
    return STATUS_SUCCESS;
}

VOID
NTAPI
PopSetEffectivePowerMode(
    _In_ ULONG Mode)
{
    PPOP_EFFMODE_REGISTRATION Registration;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    if (Mode > PO_EFFECTIVE_POWER_MODE_MIXED_REALITY)
        return;

    KeAcquireSpinLock(&PopEffModeLock, &OldIrql);
    PopEffModeEnsureInitialized();
    if (Mode == PopEffModeCurrent)
    {
        KeReleaseSpinLock(&PopEffModeLock, OldIrql);
        return;
    }

    PopEffModeCurrent = Mode;
    PopEffModeEpoch++;

    /* Walk again from the head after each callback: the list may have changed meanwhile */
    for (;;)
    {
        Registration = NULL;
        for (Entry = PopEffModeList.Flink; Entry != &PopEffModeList; Entry = Entry->Flink)
        {
            PPOP_EFFMODE_REGISTRATION Candidate = CONTAINING_RECORD(Entry, POP_EFFMODE_REGISTRATION, Link);

            if (!Candidate->Removed && (Candidate->Epoch != PopEffModeEpoch))
            {
                Registration = Candidate;
                break;
            }
        }

        if (Registration == NULL)
            break;

        Registration->Epoch = PopEffModeEpoch;
        InterlockedIncrement(&Registration->References);
        KeReleaseSpinLock(&PopEffModeLock, OldIrql);

        PopEffModeDeliver(Registration);
        PopEffModeDereference(Registration);

        KeAcquireSpinLock(&PopEffModeLock, &OldIrql);
    }
    KeReleaseSpinLock(&PopEffModeLock, OldIrql);
}

ULONG
NTAPI
PopGetEffectivePowerMode(VOID)
{
    return PopEffModeCurrent;
}

VOID
NTAPI
PoUserShutdownInitiated(VOID)
{
    InterlockedExchange(&PopUserShutdownState, 1);
}

VOID
NTAPI
PoUserShutdownCancelled(VOID)
{
    InterlockedExchange(&PopUserShutdownState, 0);
}

BOOLEAN
NTAPI
PopIsUserShutdownInProgress(VOID)
{
    return PopUserShutdownState != 0;
}

/* There is no energy estimation engine in this kernel: nothing produces estimates, so they are not enabled */
BOOLEAN
NTAPI
PoEnergyEstimationEnabled(VOID)
{
    return FALSE;
}
