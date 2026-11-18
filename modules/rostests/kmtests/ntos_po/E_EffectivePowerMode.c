/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Effective power mode notification registration, user shutdown and energy estimation exports
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

typedef VOID (NTAPI *PTEST_EFFECTIVE_POWER_MODE_CALLBACK)(_In_ ULONG Mode, _In_opt_ PVOID Context);

NTSYSAPI
NTSTATUS
NTAPI
PoRegisterForEffectivePowerModeNotifications(
    _In_ ULONG Version,
    _In_ PTEST_EFFECTIVE_POWER_MODE_CALLBACK Callback,
    _In_opt_ PVOID Context,
    _Outptr_ PVOID *RegistrationHandle);

NTSYSAPI
NTSTATUS
NTAPI
PoUnregisterFromEffectivePowerModeNotifications(
    _In_ PVOID RegistrationHandle);

NTSYSAPI VOID NTAPI PoUserShutdownInitiated(VOID);
NTSYSAPI VOID NTAPI PoUserShutdownCancelled(VOID);
NTSYSAPI BOOLEAN NTAPI PoEnergyEstimationEnabled(VOID);

#define TEST_MODE_MIXED_REALITY 6

typedef struct _TEST_CONTEXT
{
    LONG Calls;
    ULONG LastMode;
    PVOID Tag;
} TEST_CONTEXT, *PTEST_CONTEXT;

static
VOID
NTAPI
ModeCallback(
    _In_ ULONG Mode,
    _In_opt_ PVOID Context)
{
    PTEST_CONTEXT Test = Context;

    if (Test == NULL)
        return;

    InterlockedIncrement(&Test->Calls);
    Test->LastMode = Mode;
}

static
VOID
TestEffectivePowerModeRegistration(VOID)
{
    TEST_CONTEXT First = { 0, MAXULONG, (PVOID)1 }, Second = { 0, MAXULONG, (PVOID)2 };
    PVOID Handle1 = (PVOID)(ULONG_PTR)1, Handle2 = NULL;
    NTSTATUS Status;

    /* Invalid parameters never produce a handle */
    Status = PoRegisterForEffectivePowerModeNotifications(1, NULL, &First, &Handle1);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok(Handle1 == NULL, "A failed registration returned the handle %p\n", Handle1);

    Status = PoRegisterForEffectivePowerModeNotifications(0, ModeCallback, &First, &Handle1);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = PoRegisterForEffectivePowerModeNotifications(3, ModeCallback, &First, &Handle1);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok_eq_long(First.Calls, 0);

    Status = PoUnregisterFromEffectivePowerModeNotifications(NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    /* A registrant is told the current mode before the call returns */
    Handle1 = NULL;
    Status = PoRegisterForEffectivePowerModeNotifications(1, ModeCallback, &First, &Handle1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    ok(Handle1 != NULL, "No handle\n");
    ok_eq_long(First.Calls, 1);
    ok(First.LastMode <= TEST_MODE_MIXED_REALITY, "Mode %lu is not an EFFECTIVE_POWER_MODE\n", First.LastMode);

    /* A second registrant gets the same mode and its own handle */
    Status = PoRegisterForEffectivePowerModeNotifications(2, ModeCallback, &Second, &Handle2);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(Handle2 != NULL && Handle2 != Handle1, "Handles %p and %p\n", Handle1, Handle2);
        ok_eq_long(Second.Calls, 1);
        ok_eq_ulong(Second.LastMode, First.LastMode);
        ok_eq_long(First.Calls, 1);
    }

    /* Unregister removes exactly one registrant, and a handle works once */
    Status = PoUnregisterFromEffectivePowerModeNotifications(Handle1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = PoUnregisterFromEffectivePowerModeNotifications(Handle1);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    if (Handle2 != NULL)
    {
        Status = PoUnregisterFromEffectivePowerModeNotifications(Handle2);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    ok_eq_long(First.Calls, 1);
    ok_eq_long(Second.Calls, 1);
}

static
VOID
TestMiscState(VOID)
{
    /* Nothing in this kernel produces energy estimates */
    ok_bool_false(PoEnergyEstimationEnabled(), "PoEnergyEstimationEnabled");

    /* The pair has no result; cancelling without a pending shutdown and repeating must be harmless */
    PoUserShutdownCancelled();
    PoUserShutdownInitiated();
    PoUserShutdownInitiated();
    PoUserShutdownCancelled();
    PoUserShutdownCancelled();
}

START_TEST(E_EffectivePowerMode)
{
    TestEffectivePowerModeRegistration();
    TestMiscState();
}
