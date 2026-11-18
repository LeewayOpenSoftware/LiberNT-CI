/*
 * PROJECT:         LiberNT kernel-mode tests
 * LICENSE:         GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:         PoFx wake hint, multiple perf-state changes, surprise power-on, DRIPS target
 * COPYRIGHT:       Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>
#include "PoFxState_pnp.h"

/* wdm.h (Microsoft Learn); not declared by our DDK headers yet */
NTKERNELAPI VOID NTAPI PoFxSetComponentWake(POHANDLE Handle, ULONG Component, BOOLEAN WakeHint);
NTKERNELAPI VOID NTAPI PoFxIssueComponentPerfStateChangeMultiple(POHANDLE Handle, ULONG Flags, ULONG Component,
    ULONG PerfChangesCount, PPO_FX_PERF_STATE_CHANGE PerfChanges, PVOID Context);
NTKERNELAPI VOID NTAPI PoFxNotifySurprisePowerOn(PDEVICE_OBJECT Pdo);
NTKERNELAPI NTSTATUS NTAPI PoFxSetTargetDripsDevicePowerState(POHANDLE Handle, DEVICE_POWER_STATE TargetState);

typedef struct _TEST_C_CONTEXT
{
    POHANDLE Handle;
    volatile LONG F1Count;
    volatile LONG LastState;
    volatile LONG PerfCalls;
    volatile LONG PerfSucceeded;
    KEVENT F1Event;
    KEVENT PerfEvent;
} TEST_C_CONTEXT, *PTEST_C_CONTEXT;

static VOID NTAPI TestCActive(PVOID Context, ULONG Component)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Component);
}

static VOID NTAPI TestCIdleCondition(PVOID Parameter, ULONG Component)
{
    PTEST_C_CONTEXT Context = Parameter;
    PoFxCompleteIdleCondition(Context->Handle, Component);
}

static VOID NTAPI TestCIdleState(PVOID Parameter, ULONG Component, ULONG State)
{
    PTEST_C_CONTEXT Context = Parameter;

    InterlockedExchange(&Context->LastState, (LONG)State);
    if (State == 1)
    {
        InterlockedIncrement(&Context->F1Count);
        KeSetEvent(&Context->F1Event, IO_NO_INCREMENT, FALSE);
    }
    PoFxCompleteIdleState(Context->Handle, Component);
}

static VOID NTAPI TestCPowerRequired(PVOID Parameter)
{
    PTEST_C_CONTEXT Context = Parameter;
    PoFxReportDevicePoweredOn(Context->Handle);
}

static VOID NTAPI TestCPowerNotRequired(PVOID Parameter)
{
    PTEST_C_CONTEXT Context = Parameter;
    PoFxCompleteDevicePowerNotRequired(Context->Handle);
}

static VOID NTAPI TestCPerf(PVOID Parameter, ULONG Component, BOOLEAN Succeeded, PVOID RequestContext)
{
    PTEST_C_CONTEXT Context = Parameter;

    UNREFERENCED_PARAMETER(Component);
    ok_eq_pointer(RequestContext, (PVOID)(ULONG_PTR)0x1234);
    InterlockedIncrement(&Context->PerfCalls);
    InterlockedExchange(&Context->PerfSucceeded, Succeeded);
    KeSetEvent(&Context->PerfEvent, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS TestCWait(PKEVENT Event, LONG Milliseconds)
{
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = -10000LL * Milliseconds;
    return KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &Timeout);
}

static VOID TestCInvalidParameters(VOID)
{
    PO_FX_PERF_STATE_CHANGE Change = {0};

    /* No registration: nothing may happen and nothing may crash */
    PoFxSetComponentWake(NULL, 0, TRUE);
    PoFxIssueComponentPerfStateChangeMultiple(NULL, 0, 0, 1, &Change, NULL);
    PoFxNotifySurprisePowerOn(NULL);
    ok_eq_hex(PoFxSetTargetDripsDevicePowerState(NULL, PowerDeviceD3), STATUS_INVALID_HANDLE);
    ok_eq_hex(PoFxSetTargetDripsDevicePowerState(NULL, PowerDeviceMaximum), STATUS_INVALID_PARAMETER);
}

static VOID TestCDevice(PDEVICE_OBJECT Pdo)
{
    TEST_C_CONTEXT Context;
    PO_FX_DEVICE Registration;
    PO_FX_COMPONENT_IDLE_STATE IdleStates[2];
    PO_FX_PERF_STATE Discrete[3];
    PO_FX_COMPONENT_PERF_INFO Info;
    PO_FX_PERF_STATE_CHANGE Changes[2];
    ULONGLONG Current;
    NTSTATUS Status;
    LONG Before;

    RtlZeroMemory(&Context, sizeof(Context));
    KeInitializeEvent(&Context.F1Event, NotificationEvent, FALSE);
    KeInitializeEvent(&Context.PerfEvent, NotificationEvent, FALSE);
    RtlZeroMemory(&Registration, sizeof(Registration));
    RtlZeroMemory(IdleStates, sizeof(IdleStates));
    Registration.Version = PO_FX_VERSION;
    Registration.DeviceContext = &Context;
    Registration.ComponentCount = 1;
    Registration.Components[0].IdleStateCount = RTL_NUMBER_OF(IdleStates);
    Registration.Components[0].DeepestWakeableIdleState = 0;   /* F1 cannot wake */
    Registration.Components[0].IdleStates = IdleStates;
    Registration.ComponentActiveConditionCallback = TestCActive;
    Registration.ComponentIdleConditionCallback = TestCIdleCondition;
    Registration.ComponentIdleStateCallback = TestCIdleState;
    Registration.DevicePowerRequiredCallback = TestCPowerRequired;
    Registration.DevicePowerNotRequiredCallback = TestCPowerNotRequired;
    Status = PoFxRegisterDevice(Pdo, &Registration, &Context.Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status) || !Context.Handle)
        return;
    PoFxSetComponentLatency(Context.Handle, 0, PO_FX_UNKNOWN_TIME);
    PoFxSetComponentResidency(Context.Handle, 0, PO_FX_UNKNOWN_TIME);
    PoFxSetDeviceIdleTimeout(Context.Handle, 3600ULL * 10000000ULL);

    /* Armed before management starts: the idle component must stay in F0 */
    PoFxSetComponentWake(Context.Handle, 0, TRUE);
    PoFxStartDevicePowerManagement(Context.Handle);
    Status = TestCWait(&Context.F1Event, 2000);
    ok_eq_hex(Status, STATUS_TIMEOUT);
    ok_eq_long(Context.F1Count, 0);

    /* Disarmed: F1 is allowed again */
    PoFxSetComponentWake(Context.Handle, 0, FALSE);
    Status = TestCWait(&Context.F1Event, 5000);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_long(Context.LastState, 1);

    /* Re-arming brings the component back to a wakeable state */
    PoFxSetComponentWake(Context.Handle, 0, TRUE);
    ok_eq_long(Context.LastState, 0);
    PoFxSetComponentWake(Context.Handle, 0, FALSE);

    /* Surprise power-on: the components are F0 again and get idled again */
    KeClearEvent(&Context.F1Event);
    Before = Context.F1Count;
    TestCWait(&Context.F1Event, 5000);
    KeClearEvent(&Context.F1Event);
    Before = Context.F1Count;
    PoFxNotifySurprisePowerOn(Pdo);
    Status = TestCWait(&Context.F1Event, 5000);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Context.F1Count > Before, "The component was not idled again after a surprise power-on (%ld <= %ld)\n",
       Context.F1Count, Before);

    /* Two discrete perf-state sets of three states each */
    RtlZeroMemory(Discrete, sizeof(Discrete));
    Discrete[0].Value = 100;
    Discrete[1].Value = 200;
    Discrete[2].Value = 300;
    RtlZeroMemory(&Info, sizeof(Info));
    Info.PerfStateSetsCount = 1;
    Info.PerfStateSets[0].Type = PoFxPerfStateTypeDiscrete;
    Info.PerfStateSets[0].Discrete.Count = RTL_NUMBER_OF(Discrete);
    Info.PerfStateSets[0].Discrete.States = Discrete;
    Status = PoFxRegisterComponentPerfStates(Context.Handle, 0, 0, TestCPerf, &Info, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        /* A valid request moves the set and reports success */
        RtlZeroMemory(Changes, sizeof(Changes));
        Changes[0].Set = 0;
        Changes[0].StateIndex = 2;
        PoFxIssueComponentPerfStateChangeMultiple(Context.Handle, 0, 0, 1, Changes, (PVOID)(ULONG_PTR)0x1234);
        ok_eq_long(Context.PerfCalls, 1);
        ok_eq_long(Context.PerfSucceeded, TRUE);
        Current = 0;
        PoFxQueryCurrentComponentPerfState(Context.Handle, 0, 0, 0, &Current);
        ok_eq_ulonglong(Current, 2ULL);

        /* One bad change in the batch: nothing moves and the callback says so */
        Changes[0].StateIndex = 1;
        Changes[1].Set = 7;
        Changes[1].StateIndex = 0;
        PoFxIssueComponentPerfStateChangeMultiple(Context.Handle, 0, 0, 2, Changes, (PVOID)(ULONG_PTR)0x1234);
        ok_eq_long(Context.PerfCalls, 2);
        ok_eq_long(Context.PerfSucceeded, FALSE);
        Current = 0;
        PoFxQueryCurrentComponentPerfState(Context.Handle, 0, 0, 0, &Current);
        ok_eq_ulonglong(Current, 2ULL);

        /* Asynchronous: the callback comes from a worker */
        KeClearEvent(&Context.PerfEvent);
        Changes[0].StateIndex = 0;
        PoFxIssueComponentPerfStateChangeMultiple(Context.Handle, PO_FX_FLAG_ASYNC_ONLY, 0, 1, Changes,
                                                  (PVOID)(ULONG_PTR)0x1234);
        ok_eq_hex(TestCWait(&Context.PerfEvent, 5000), STATUS_SUCCESS);
        ok_eq_long(Context.PerfCalls, 3);
        ok_eq_long(Context.PerfSucceeded, TRUE);
    }

    /* DRIPS target: valid states are accepted, others refused */
    ok_eq_hex(PoFxSetTargetDripsDevicePowerState(Context.Handle, PowerDeviceD3), STATUS_SUCCESS);
    ok_eq_hex(PoFxSetTargetDripsDevicePowerState(Context.Handle, PowerDeviceUnspecified), STATUS_SUCCESS);
    ok_eq_hex(PoFxSetTargetDripsDevicePowerState(Context.Handle, PowerDeviceMaximum), STATUS_INVALID_PARAMETER);

    PoFxUnregisterDevice(Context.Handle);
}

START_TEST(PoFxTeamC)
{
    PDEVICE_OBJECT Pdo, Fdo;

    TestCInvalidParameters();
    Fdo = KmtPoFxAcquireDevice(&Pdo);
    if (!Fdo)
    {
        skip(FALSE, "No started PoFx test device; run through kmtest.exe with its signed INF package\n");
        return;
    }
    TestCDevice(Pdo);
    KmtPoFxReleaseDevice(Fdo);
}
