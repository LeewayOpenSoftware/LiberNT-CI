/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Silo context slot and silo monitor tests on the host silo
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

static LONG g_CleanupCalls;

static VOID
NTAPI
SiloTestCleanup(
    _In_ PVOID SiloContext)
{
    UNREFERENCED_PARAMETER(SiloContext);
    InterlockedIncrement(&g_CleanupCalls);
}

static NTSTATUS
NTAPI
SiloTestCreate(
    _In_ PESILO Silo)
{
    ok(Silo == PsGetHostSilo(), "host create notification for %p\n", Silo);
    return STATUS_SUCCESS;
}

static VOID
NTAPI
SiloTestTerminate(
    _In_ PESILO Silo)
{
    ok(Silo == PsGetHostSilo(), "host terminate notification for %p\n", Silo);
}

START_TEST(PsSilo)
{
    static const UNICODE_STRING ComponentName = RTL_CONSTANT_STRING(L"kmtest.PsSilo");
    SILO_MONITOR_REGISTRATION Registration;
    PSILO_MONITOR Monitor;
    ULONG Slot, SecondSlot, MonitorSlot;
    PVOID Context, Retrieved;
    NTSTATUS Status;

    g_CleanupCalls = 0;

    /* Slot allocation: reserved must be zero, slots are distinct */
    Status = PsAllocSiloContextSlot(0, &Slot);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = PsAllocSiloContextSlot(5, &SecondSlot);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = PsAllocSiloContextSlot(0, &SecondSlot);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Slot != SecondSlot, "allocated the same slot twice\n");

    /* Create: pool restricted to NonPagedPoolNx and PagedPool, host silo accepted */
    Status = PsCreateSiloContext(PsGetHostSilo(), 64, NonPagedPool, NULL, &Context);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = PsCreateSiloContext(PsGetHostSilo(), 64, NonPagedPoolNx, SiloTestCleanup, &Context);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Context != NULL, "no context returned\n");

    /* Reference and dereference outside a table: cleanup fires at zero */
    PsReferenceSiloContext(Context);
    PsDereferenceSiloContext(Context);
    ok_eq_long(g_CleanupCalls, 0);
    PsDereferenceSiloContext(Context);
    ok_eq_long(g_CleanupCalls, 1);

    /* Create a context for the table tests */
    Status = PsCreateSiloContext(PsGetHostSilo(), 32, PagedPool, NULL, &Context);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Get on an allocated but empty slot */
    Status = PsGetSiloContext(PsGetHostSilo(), Slot, &Retrieved);
    ok_eq_hex(Status, STATUS_NOT_FOUND);

    /* Insert twice, then get */
    Status = PsInsertSiloContext(PsGetHostSilo(), Slot, Context);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = PsInsertSiloContext(PsGetHostSilo(), Slot, Context);
    ok_eq_hex(Status, STATUS_NOT_SUPPORTED);
    Status = PsGetSiloContext(PsGetHostSilo(), Slot, &Retrieved);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Retrieved == Context, "get returned a different context\n");
    PsDereferenceSiloContext(Retrieved);

    /* Permanence: get rejects a non-read-only slot, then succeeds */
    Status = PsGetPermanentSiloContext(PsGetHostSilo(), Slot, &Retrieved);
    ok_eq_hex(Status, STATUS_NOT_SUPPORTED);
    Status = PsMakeSiloContextPermanent(PsGetHostSilo(), Slot);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = PsGetPermanentSiloContext(PsGetHostSilo(), Slot, &Retrieved);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Retrieved == Context, "permanent get returned a different context\n");

    /* Drop the caller reference; the table keeps the silo reference */
    PsDereferenceSiloContext(Context);
    PsFreeSiloContextSlot(Slot);
    PsFreeSiloContextSlot(SecondSlot);

    /* Monitors: registration validation, host notify at start, slot query */
    RtlZeroMemory(&Registration, sizeof(Registration));
    Status = PsRegisterSiloMonitor(&Registration, &Monitor);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Registration.Version = SILO_MONITOR_REGISTRATION_VERSION;
    Registration.MonitorHost = TRUE;
    Registration.ComponentName = (PUNICODE_STRING)&ComponentName;
    Registration.CreateCallback = SiloTestCreate;
    Registration.TerminateCallback = SiloTestTerminate;
    Status = PsRegisterSiloMonitor(&Registration, &Monitor);
    ok_eq_hex(Status, STATUS_SUCCESS);

    MonitorSlot = PsGetSiloMonitorContextSlot(Monitor);
    ok(MonitorSlot < 128, "monitor slot out of range: %lu\n", MonitorSlot);

    Status = PsStartSiloMonitor(Monitor);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = PsStartSiloMonitor(Monitor);
    ok_eq_hex(Status, STATUS_SUCCESS);

    PsUnregisterSiloMonitor(Monitor);

    /* Silo queries: with no server silos every query resolves to the host */
    ok(PsGetCurrentSilo() == PsGetHostSilo(), "current silo is not the host\n");
    ok(PsGetEffectiveServerSilo(PsGetHostSilo()) == PsGetHostSilo(),
       "effective server silo is not the host\n");
    ok(PsGetParentSilo(NULL) == PsGetHostSilo(), "host parent is not the host\n");
    ok(PsGetThreadServerSilo(PsGetCurrentThread()) == NULL,
       "current thread reports a server silo\n");
}
