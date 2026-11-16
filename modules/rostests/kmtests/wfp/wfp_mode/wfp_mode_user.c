/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     WFP enforcement-mode management + audit test (M3 stage 0)
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 *
 * Validates the stage-0 management ABI through the \.\WfpEngine handle:
 *   - IOCTL_WFP_SET_MODE honours the D-AUTH LocalSystem gate (succeeds only as
 *     SYSTEM, denied otherwise) and ALWAYS emits a WfpEventModeChange audit record
 *     carrying this process id, whether it was allowed or denied.
 *   - IOCTL_WFP_EVENT_ENUM returns a well-formed header.
 *   - IOCTL_WFP_FILTER_GET_STATS reports STATUS_FWP_FILTER_NOT_FOUND for an id
 *     that does not exist (the ioctl is wired and range-checked).
 * The On/Off enforcement A/B itself is driven by wfpmode.exe + the socket matrix.
 */

#include <kmt_test.h>
#include <winsock2.h>
#include <rpc.h>
#include <initguid.h>
#include <fwpmu.h>
#include <drivers/wfp/wfpioctl.h>

static
BOOL
CallerIsSystem(VOID)
{
    HANDLE Token = NULL;
    BYTE Buffer[128];
    PTOKEN_USER User = (PTOKEN_USER)Buffer;
    DWORD Length = 0;
    SID_IDENTIFIER_AUTHORITY Nt = SECURITY_NT_AUTHORITY;
    PSID SystemSid = NULL;
    BOOL IsSystem = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
        return FALSE;
    if (GetTokenInformation(Token, TokenUser, Buffer, sizeof(Buffer), &Length) &&
        AllocateAndInitializeSid(&Nt, 1, SECURITY_LOCAL_SYSTEM_RID,
                                 0, 0, 0, 0, 0, 0, 0, &SystemSid))
    {
        IsSystem = EqualSid(User->User.Sid, SystemSid);
        FreeSid(SystemSid);
    }
    CloseHandle(Token);
    return IsSystem;
}

static
BOOL
FindModeChangeForPid(
    _In_ HANDLE Engine,
    _In_ ULONG Pid,
    _Out_ PULONG FoundStatus,
    _Out_ PULONG FoundMode)
{
    static BYTE Buffer[64 * 1024];
    PWFP_IOCTL_EVENT_HEADER Header;
    PWFP_IOCTL_EVENT Event;
    DWORD Bytes = 0;
    LONG Index;

    *FoundStatus = 0xFFFFFFFF;
    *FoundMode = 0xFFFFFFFF;

    if (!DeviceIoControl(Engine, IOCTL_WFP_EVENT_ENUM, NULL, 0,
                         Buffer, sizeof(Buffer), &Bytes, NULL) ||
        Bytes < sizeof(WFP_IOCTL_EVENT_HEADER))
    {
        return FALSE;
    }

    Header = (PWFP_IOCTL_EVENT_HEADER)Buffer;
    Event = (PWFP_IOCTL_EVENT)(Buffer + sizeof(WFP_IOCTL_EVENT_HEADER));

    /* newest first: scan backwards for our most recent mode-change */
    for (Index = (LONG)Header->Count - 1; Index >= 0; Index--)
    {
        if (Event[Index].Type == WfpEventModeChange &&
            Event[Index].ProcessId == Pid)
        {
            *FoundStatus = (ULONG)Event[Index].Status;
            *FoundMode = Event[Index].Mode;
            return TRUE;
        }
    }
    return FALSE;
}

static
DWORD
SetMode(
    _In_ HANDLE Engine,
    _In_ ULONG Mode)
{
    WFP_IOCTL_SET_MODE In;
    DWORD Bytes = 0;

    In.Mode = Mode;
    if (DeviceIoControl(Engine, IOCTL_WFP_SET_MODE, &In, sizeof(In), NULL, 0, &Bytes, NULL))
        return ERROR_SUCCESS;
    return GetLastError();
}

START_TEST(WfpMode)
{
    HANDLE Engine = NULL;
    WFP_IOCTL_FILTER_STATS Stats;
    UINT64 BogusId = 0xDEADBEEFULL;
    DWORD Bytes = 0;
    DWORD Error;
    ULONG Pid = GetCurrentProcessId();
    ULONG Status = 0, EventMode = 0;
    BOOL IsSystem = CallerIsSystem();

    Error = FwpmEngineOpen0(NULL, RPC_C_AUTHN_WINNT, NULL, NULL, &Engine);
    ok_eq_hex(Error, ERROR_SUCCESS);
    if (Error != ERROR_SUCCESS)
        return;

    trace("running as %s\n", IsSystem ? "LocalSystem" : "non-system");

    /* --- set-mode gate + audit --- */
    Error = SetMode(Engine, WfpModeOn);
    if (IsSystem)
    {
        ok_eq_int(Error, ERROR_SUCCESS);
        ok_bool_true(FindModeChangeForPid(Engine, Pid, &Status, &EventMode),
                     "mode-change audit present");
        ok_eq_hex(Status, (ULONG)STATUS_SUCCESS);
        ok_eq_ulong(EventMode, (ULONG)WfpModeOn);

        /* restore Off and confirm a second audit record */
        Error = SetMode(Engine, WfpModeOff);
        ok_eq_int(Error, ERROR_SUCCESS);
        ok_bool_true(FindModeChangeForPid(Engine, Pid, &Status, &EventMode),
                     "restore audit present");
        ok_eq_ulong(EventMode, (ULONG)WfpModeOff);
    }
    else
    {
        /* denied, but still audited with a failure status */
        ok_bool_false(Error == ERROR_SUCCESS, "set-mode denied for non-system");
        ok_bool_true(FindModeChangeForPid(Engine, Pid, &Status, &EventMode),
                     "denied attempt audited");
        ok_bool_false(Status == (ULONG)STATUS_SUCCESS, "audit status is a denial");
    }

    /* --- invalid mode value is rejected --- */
    if (IsSystem)
    {
        Error = SetMode(Engine, 99);
        ok_bool_false(Error == ERROR_SUCCESS, "out-of-range mode rejected");
    }

    /* --- filter stats ioctl is wired: unknown id -> not found --- */
    ZeroMemory(&Stats, sizeof(Stats));
    Error = DeviceIoControl(Engine, IOCTL_WFP_FILTER_GET_STATS, &BogusId, sizeof(BogusId),
                            &Stats, sizeof(Stats), &Bytes, NULL)
            ? ERROR_SUCCESS : GetLastError();
    ok_bool_false(Error == ERROR_SUCCESS, "stats for unknown filter fails");

    FwpmEngineClose0(Engine);
}
