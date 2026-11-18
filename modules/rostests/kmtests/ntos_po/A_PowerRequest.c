/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Kernel power requests: reason contexts, request types, counting and several requests at once
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>

static
BOOLEAN
QueryState(
    _Out_ EXECUTION_STATE *State)
{
    *State = 0;
    return NT_SUCCESS(ZwPowerInformation(SystemExecutionState, NULL, 0, State, sizeof(*State)));
}

static
VOID
CheckContexts(VOID)
{
    COUNTED_REASON_CONTEXT Context;
    UNICODE_STRING Strings[1];
    PVOID Request;
    NTSTATUS Status;

    RtlZeroMemory(&Context, sizeof(Context));
    Context.Version = POWER_REQUEST_CONTEXT_VERSION + 1;
    Context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    RtlInitUnicodeString(&Context.SimpleString, L"wrong version");
    Request = (PVOID)(ULONG_PTR)1;
    Status = PoCreatePowerRequest(&Request, KmtDriverObject->DeviceObject, &Context);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok_eq_pointer(Request, NULL);

    Context.Version = POWER_REQUEST_CONTEXT_VERSION;
    Context.Flags = 0x80000000;
    Request = (PVOID)(ULONG_PTR)1;
    Status = PoCreatePowerRequest(&Request, KmtDriverObject->DeviceObject, &Context);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok_eq_pointer(Request, NULL);

    Context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    RtlInitEmptyUnicodeString(&Context.SimpleString, NULL, 0);
    Status = PoCreatePowerRequest(&Request, KmtDriverObject->DeviceObject, &Context);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    RtlZeroMemory(&Context, sizeof(Context));
    Context.Version = POWER_REQUEST_CONTEXT_VERSION;
    Context.Flags = POWER_REQUEST_CONTEXT_DETAILED_STRING;
    RtlInitUnicodeString(&Context.ResourceFileName, L"\\SystemRoot\\System32\\drivers\\kmtest.sys");
    Context.ResourceReasonId = 1;
    RtlInitUnicodeString(&Strings[0], L"kmtest");
    Context.StringCount = 1;
    Context.ReasonStrings = Strings;
    Status = PoCreatePowerRequest(&Request, KmtDriverObject->DeviceObject, &Context);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        PoDeletePowerRequest(Request);

    Status = PoCreatePowerRequest(&Request, KmtDriverObject->DeviceObject, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        PoDeletePowerRequest(Request);
}

static
VOID
CheckTypes(VOID)
{
    PVOID Request;
    NTSTATUS Status;

    Status = PoCreatePowerRequest(&Request, KmtDriverObject->DeviceObject, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    Status = PoSetPowerRequest(Request, PowerRequestMaximum);
    ok(Status == STATUS_INVALID_PARAMETER || Status == STATUS_NOT_SUPPORTED, "an unknown type returned 0x%lx\n", Status);

    ok_eq_hex(PoSetPowerRequest(Request, PowerRequestAwayModeRequired), STATUS_SUCCESS);
    ok_eq_hex(PoClearPowerRequest(Request, PowerRequestAwayModeRequired), STATUS_SUCCESS);
    ok_eq_hex(PoClearPowerRequest(Request, PowerRequestAwayModeRequired), STATUS_NOT_SUPPORTED);

    ok_eq_hex(PoSetPowerRequest(Request, PowerRequestExecutionRequired), STATUS_SUCCESS);
    ok_eq_hex(PoClearPowerRequest(Request, PowerRequestExecutionRequired), STATUS_SUCCESS);

    /* Delete while two types are still set */
    ok_eq_hex(PoSetPowerRequest(Request, PowerRequestSystemRequired), STATUS_SUCCESS);
    ok_eq_hex(PoSetPowerRequest(Request, PowerRequestAwayModeRequired), STATUS_SUCCESS);
    PoDeletePowerRequest(Request);
}

static
VOID
CheckTwoRequests(VOID)
{
    PVOID First, Second;
    EXECUTION_STATE State;
    BOOLEAN CanQuery, Held;
    NTSTATUS Status;

    CanQuery = QueryState(&State);
    if (!CanQuery)
    {
        skip(FALSE, "SystemExecutionState cannot be queried\n");
        return;
    }
    Held = (State & ES_SYSTEM_REQUIRED) != 0;
    if (Held)
    {
        skip(FALSE, "another component keeps the system required\n");
        return;
    }

    Status = PoCreatePowerRequest(&First, KmtDriverObject->DeviceObject, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = PoCreatePowerRequest(&Second, KmtDriverObject->DeviceObject, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        PoDeletePowerRequest(First);
        return;
    }

    ok_eq_hex(PoSetPowerRequest(First, PowerRequestSystemRequired), STATUS_SUCCESS);
    ok_eq_hex(PoSetPowerRequest(Second, PowerRequestSystemRequired), STATUS_SUCCESS);
    PoDeletePowerRequest(First);
    ok(QueryState(&State) && (State & ES_SYSTEM_REQUIRED), "the second request no longer keeps the system required\n");

    ok_eq_hex(PoClearPowerRequest(Second, PowerRequestSystemRequired), STATUS_SUCCESS);
    ok(QueryState(&State) && !(State & ES_SYSTEM_REQUIRED), "no request is set but the system is still required\n");
    PoDeletePowerRequest(Second);
}

START_TEST(PoPowerRequestCounting)
{
    _SEH2_TRY
    {
        CheckContexts();
        CheckTypes();
        CheckTwoRequests();
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        ok(FALSE, "the power request routines raised 0x%08lx\n", _SEH2_GetExceptionCode());
    }
    _SEH2_END;
}
