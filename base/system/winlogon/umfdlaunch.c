/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Launching and supervising the user-mode font driver host
 *              (UMFD-LAUNCHER-DESIGN).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "winlogon.h"

#include <sddl.h>
#include <ndk/setypes.h>
#include <wlimgcheck.h>
#include <reactos/umfd.h>

#define UMFD_EXE             L"fontdrvhost.exe"
/* Orosi interim placement; the Win11 virtual account (U2) decides where the host lives */
#define UMFD_WINSTA          L"UmfdHost"
#define UMFD_DESKTOP         UMFD_WINSTA L"\\Default"
#define UMFD_CAP_LAUNCHES    5
#define UMFD_CAP_WINDOW_MS   (10 * 60 * 1000)
#define UMFD_RECYCLE_MAX     3      /* a real recycle takes 32 faulting files */
#define UMFD_BACKOFF_MIN_MS  1000
#define UMFD_BACKOFF_MAX_MS  30000
#define UMFD_STOP_WAIT_MS    5000
#define UMFD_JOIN_MS         (UMFD_STOP_WAIT_MS + 5000)   /* past this the supervisor is left, never freed under */
#define UMFD_FIRST_WAIT_MS   10000

/* SYSTEM only on the host itself; its window station also admits LS (normal check) and RC (restricting check) */
#define UMFD_PROCESS_SDDL    L"O:SYG:SYD:P(A;;GA;;;SY)"
#define UMFD_WINSTA_SDDL     L"O:SYG:SYD:P(A;;GA;;;SY)(A;;GRGX;;;LS)(A;;GRGX;;;RC)"

INT WINAPI NtGdiExtEscape(HDC hdc, PWCHAR pDriver, INT nDriver, INT Escape,
                          INT InSize, LPSTR InData, INT OutSize, LPSTR OutData);
HDESK NTAPI NtUserCreateDesktop(POBJECT_ATTRIBUTES poa, PUNICODE_STRING lpszDesktopDevice,
                                LPDEVMODEW lpdmw, DWORD dwFlags, ACCESS_MASK dwDesiredAccess);

typedef enum _UMFD_LAUNCH
{
    UmfdLaunchOk,
    UmfdLaunchFailed,
    UmfdLaunchTaken     /* another TCB caller holds the channel */
} UMFD_LAUNCH;

static HWINSTA g_Winsta;
static HDESK g_Desktop;
static HANDLE g_Supervisor;
static HANDLE g_Cancel;
static HANDLE g_FirstDone;
static WLREC_BUDGET g_Budget;           /* crash budget; only the supervisor thread touches either */
static WLREC_BUDGET g_RecycleBudget;
static BOOL g_StopDone;

static VOID
WlUmfdLog(
    _In_ WORD Type,
    _In_ PCWSTR Format,
    ...)
{
    WCHAR Text[MAX_PATH + 160];
    PCWSTR Strings[1] = { Text };
    HANDLE Source;
    va_list Args;

    va_start(Args, Format);
    StringCchVPrintfW(Text, ARRAYSIZE(Text), Format, Args);
    va_end(Args);

    if (Type == EVENTLOG_ERROR_TYPE)
        ERR("WL: %ls\n", Text);
    else
        TRACE("WL: %ls\n", Text);

    Source = RegisterEventSourceW(NULL, L"Winlogon");
    if (Source != NULL)
    {
        /* Event 4002: font driver host supervision; the text carries the detail. */
        ReportEventW(Source, Type, 0, 4002, NULL, 1, 0, Strings, NULL);
        DeregisterEventSource(Source);
    }
}

/* The value win32k read at boot */
static DWORD
WlUmfdReadMode(VOID)
{
    DWORD Mode = 0, Size = sizeof(Mode);

    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Leeway\\Win32k", L"UmfdMode",
                     RRF_RT_REG_DWORD, NULL, &Mode, &Size) != ERROR_SUCCESS)
    {
        return 0;
    }
    return Mode;
}

/* One LocalService logon per launch: the logon session ends with the host that holds its token */
static BOOL
WlUmfdMakeToken(
    _Out_ PHANDLE Token)
{
    static const PCWSTR Restrict[] = { L"S-1-1-0", L"S-1-5-12", L"S-1-5-32-545", L"S-1-5-11" };
    SID_AND_ATTRIBUTES Res[ARRAYSIZE(Restrict)];
    TOKEN_MANDATORY_LABEL Label;
    PSID Low = NULL;
    HANDLE Base = NULL;
    DWORD i, Error = 0;
    BOOL Ok = FALSE;

    *Token = NULL;
    ZeroMemory(Res, sizeof(Res));
    for (i = 0; i < ARRAYSIZE(Restrict); ++i)
    {
        if (!ConvertStringSidToSidW(Restrict[i], &Res[i].Sid))
            goto Out;
    }
    if (!ConvertStringSidToSidW(L"S-1-16-4096", &Low))
        goto Out;

    if (!LogonUserW(L"LocalService", L"NT AUTHORITY", NULL, LOGON32_LOGON_SERVICE,
                    LOGON32_PROVIDER_DEFAULT, &Base))
    {
        Error = GetLastError();
        goto Out;
    }
    if (CreateRestrictedToken(Base, DISABLE_MAX_PRIVILEGE, 0, NULL, 0, NULL,
                              ARRAYSIZE(Res), Res, Token))
    {
        Label.Label.Sid = Low;
        Label.Label.Attributes = SE_GROUP_INTEGRITY;
        Ok = SetTokenInformation(*Token, TokenIntegrityLevel, &Label, sizeof(Label) + GetLengthSid(Low));
    }
    if (!Ok)
        Error = GetLastError();

Out:
    if (!Ok)
    {
        WlUmfdLog(EVENTLOG_ERROR_TYPE, L"font driver host: no LocalService restricted token (%lu)", Error);
        if (*Token)
            CloseHandle(*Token);
        *Token = NULL;
    }
    if (Base)
        CloseHandle(Base);
    for (i = 0; i < ARRAYSIZE(Restrict); ++i)
    {
        if (Res[i].Sid)
            LocalFree(Res[i].Sid);
    }
    if (Low)
        LocalFree(Low);
    return Ok;
}

/* The restricting SIDs cannot open the LocalService service window station, so GUI-thread conversion would fail */
static BOOL
WlUmfdMakeDesktop(VOID)
{
    SECURITY_ATTRIBUTES Sa = { sizeof(Sa), NULL, FALSE };
    OBJECT_ATTRIBUTES Oa;
    UNICODE_STRING Name, Device;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(UMFD_WINSTA_SDDL, SDDL_REVISION_1,
                                                              &Sa.lpSecurityDescriptor, NULL))
    {
        return FALSE;
    }
    g_Winsta = CreateWindowStationW(UMFD_WINSTA, 0, WINSTA_ALL_ACCESS, &Sa);
    if (g_Winsta != NULL)
    {
        /* Created under that window station directly: winlogon's own stays its process window station */
        RtlInitUnicodeString(&Name, L"Default");
        RtlInitUnicodeString(&Device, NULL);
        InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE, g_Winsta, Sa.lpSecurityDescriptor);
        g_Desktop = NtUserCreateDesktop(&Oa, &Device, NULL, 0, GENERIC_ALL);
    }
    if (g_Desktop == NULL)
        WlUmfdLog(EVENTLOG_ERROR_TYPE, L"font driver host: no window station or desktop %s (%lu)",
                  UMFD_DESKTOP, GetLastError());
    LocalFree(Sa.lpSecurityDescriptor);
    return g_Desktop != NULL;
}

static UMFD_LAUNCH
WlUmfdLaunch(
    _Out_ PHANDLE Process,
    _Out_ PDWORD Pid)
{
    SECURITY_ATTRIBUTES Sa = { sizeof(Sa), NULL, FALSE };
    WCHAR Path[MAX_PATH];
    HANDLE Image = INVALID_HANDLE_VALUE, Token = NULL;
    WLIMG_STATUS ImgStatus;
    STARTUPINFOW Si;
    PROCESS_INFORMATION Pi;
    UMFD_REGISTER Reg;
    UMFD_LAUNCH Result = UmfdLaunchFailed;
    DWORD Error;

    *Process = NULL;
    *Pid = 0;
    if (!GetSystemDirectoryW(Path, ARRAYSIZE(Path)) ||
        FAILED(StringCchCatW(Path, ARRAYSIZE(Path), L"\\" UMFD_EXE)))
    {
        return UmfdLaunchFailed;
    }

    /* The verified handle stays open until CreateProcess has returned, as in hostlaunch.c */
    ImgStatus = WlImgOpenVerified(Path, WLIMG_REQUIRE_SYSTEM32 | WLIMG_CHECK_DIRECTORY, &Image);
    if (ImgStatus != WlImgOk)
    {
        WlUmfdLog(EVENTLOG_ERROR_TYPE, L"font driver host image %s rejected: %s (%lu)",
                  Path, WlImgStatusName(ImgStatus), GetLastError());
        return UmfdLaunchFailed;
    }
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(UMFD_PROCESS_SDDL, SDDL_REVISION_1,
                                                              &Sa.lpSecurityDescriptor, NULL))
    {
        CloseHandle(Image);
        return UmfdLaunchFailed;
    }
    if (!WlUmfdMakeToken(&Token))
        goto Out;

    ZeroMemory(&Si, sizeof(Si));
    Si.cb = sizeof(Si);
    Si.lpDesktop = UMFD_DESKTOP;
    ZeroMemory(&Pi, sizeof(Pi));

    /* Suspended: win32k must know the host before its first SERVE */
    if (!CreateProcessAsUserW(Token, Path, NULL, &Sa, &Sa, FALSE, CREATE_SUSPENDED | DETACHED_PROCESS,
                              NULL, NULL, &Si, &Pi))
    {
        WlUmfdLog(EVENTLOG_ERROR_TYPE, L"font driver host launch failed: %s (%lu)", Path, GetLastError());
        goto Out;
    }
    CloseHandle(Token);
    Token = NULL;

    Reg.Magic = UMFD_MAGIC;
    Reg.Version = UMFD_VERSION;
    Reg.Reserved = 0;
    Reg.ProcessHandle = (ULONG64)(ULONG_PTR)Pi.hProcess;
    if (NtGdiExtEscape(NULL, NULL, 0, UMFD_ESCAPE_REGISTER, sizeof(Reg), (LPSTR)&Reg, 0, NULL) != 1)
    {
        Error = GetLastError();
        TerminateProcess(Pi.hProcess, Error ? Error : ERROR_GEN_FAILURE);
        CloseHandle(Pi.hThread);
        CloseHandle(Pi.hProcess);
        if (Error == ERROR_ALREADY_REGISTERED)
            Result = UmfdLaunchTaken;
        else
            WlUmfdLog(EVENTLOG_ERROR_TYPE, L"font driver host pid %lu: REGISTER failed (%lu)", Pi.dwProcessId, Error);
        goto Out;
    }

    ResumeThread(Pi.hThread);
    CloseHandle(Pi.hThread);
    *Process = Pi.hProcess;
    *Pid = Pi.dwProcessId;
    Result = UmfdLaunchOk;

Out:
    if (Token)
        CloseHandle(Token);
    CloseHandle(Image);
    LocalFree(Sa.lpSecurityDescriptor);
    return Result;
}

static DWORD
WlUmfdNextBackoff(
    _In_ DWORD Backoff)
{
    if (Backoff == 0)
        return UMFD_BACKOFF_MIN_MS;
    return min(Backoff * 2, UMFD_BACKOFF_MAX_MS);
}

static DWORD WINAPI
WlUmfdSupervisor(
    _In_ LPVOID Parameter)
{
    HANDLE Process, Waits[2];
    DWORD Backoff = 0, Pid, ExitCode, Which;
    ULONGLONG Started;
    BOOL TakenLogged = FALSE, Free = FALSE;
    UMFD_LAUNCH Launch;

    UNREFERENCED_PARAMETER(Parameter);

    for (;;)
    {
        if (WaitForSingleObject(g_Cancel, Backoff) == WAIT_OBJECT_0)
            break;
        /* A recycle inside its own budget relaunches without touching the crash budget */
        if (!Free && !WlRecBudgetAdmit(&g_Budget, GetTickCount64()))
        {
            /* D1 cap: only crash loops get here; mode 1 now refuses fonts until reboot */
            WlUmfdLog(EVENTLOG_ERROR_TYPE,
                      L"font driver host: %u launches within %u minutes without a normal end; not relaunched, "
                      L"fonts added from now on are refused until reboot",
                      UMFD_CAP_LAUNCHES, UMFD_CAP_WINDOW_MS / 60000);
            break;
        }
        Free = FALSE;

        Launch = WlUmfdLaunch(&Process, &Pid);
        SetEvent(g_FirstDone);
        if (Launch == UmfdLaunchTaken)
        {
            /* Only a TCB caller can hold the channel: not a crash, so the history is cleared */
            if (!TakenLogged)
                WlUmfdLog(EVENTLOG_WARNING_TYPE,
                          L"font driver host: the channel is held by another registration; retrying every %u s",
                          UMFD_BACKOFF_MAX_MS / 1000);
            TakenLogged = TRUE;
            WlRecBudgetReset(&g_Budget);
            Backoff = UMFD_BACKOFF_MAX_MS;
            continue;
        }
        TakenLogged = FALSE;
        if (Launch == UmfdLaunchFailed)
        {
            Backoff = WlUmfdNextBackoff(Backoff);
            continue;
        }

        WlUmfdLog(EVENTLOG_INFORMATION_TYPE, L"font driver host pid %lu registered", Pid);
        Started = GetTickCount64();
        Waits[0] = Process;
        Waits[1] = g_Cancel;
        Which = WaitForMultipleObjects(2, Waits, FALSE, INFINITE);
        if (Which != WAIT_OBJECT_0)
        {
            /* Stop: after UNREGISTER the host's next SERVE fails and it exits */
            NtGdiExtEscape(NULL, NULL, 0, UMFD_ESCAPE_UNREGISTER, 0, NULL, 0, NULL);
            if (WaitForSingleObject(Process, UMFD_STOP_WAIT_MS) != WAIT_OBJECT_0)
                TerminateProcess(Process, ERROR_PROCESS_ABORTED);
            CloseHandle(Process);
            break;
        }

        ExitCode = 0;
        GetExitCodeProcess(Process, &ExitCode);
        CloseHandle(Process);

        /* The contained host picks its exit code: recycles get their own budget and never clear g_Budget */
        if (ExitCode == UMFD_HOST_EXIT_RECYCLE && WlRecBudgetAdmit(&g_RecycleBudget, GetTickCount64()))
        {
            WlUmfdLog(EVENTLOG_INFORMATION_TYPE, L"font driver host pid %lu recycled after %u faulted files",
                      Pid, UMFD_HOST_MAX_FAULTS);
            Backoff = 0;
            Free = TRUE;
            continue;
        }

        /* A host that served for a while starts the backoff over */
        if (GetTickCount64() - Started >= UMFD_BACKOFF_MAX_MS)
            Backoff = 0;
        Backoff = WlUmfdNextBackoff(Backoff);
        if (ExitCode == UMFD_HOST_EXIT_RECYCLE)
            WlUmfdLog(EVENTLOG_WARNING_TYPE, L"font driver host pid %lu: recycle rate exceeded (%u in %u minutes); "
                      L"counted as an abnormal end, relaunch in %lu ms",
                      Pid, UMFD_RECYCLE_MAX, UMFD_CAP_WINDOW_MS / 60000, Backoff);
        else
            WlUmfdLog(EVENTLOG_WARNING_TYPE, L"font driver host pid %lu ended abnormally (exit 0x%lx); relaunch in %lu ms",
                      Pid, ExitCode, Backoff);
    }

    SetEvent(g_FirstDone);
    return 0;
}

static VOID
WlUmfdRelease(VOID)
{
    if (g_Cancel)
        CloseHandle(g_Cancel);
    if (g_FirstDone)
        CloseHandle(g_FirstDone);
    if (g_Desktop)
        CloseDesktop(g_Desktop);
    if (g_Winsta)
        CloseWindowStation(g_Winsta);
    g_Cancel = g_FirstDone = NULL;
    g_Desktop = NULL;
    g_Winsta = NULL;
}

/* Needs lsass for the LocalService logon; call before the first logon UI adds fonts */
VOID
WlUmfdStart(VOID)
{
    DWORD Mode = WlUmfdReadMode();
    BOOLEAN Old;

    if (Mode == 0)
        return;
    if (Mode != 1)
    {
        WlUmfdLog(EVENTLOG_WARNING_TYPE, L"font driver host: UmfdMode %lu is not available; none started", Mode);
        return;
    }

    /* REGISTER needs TCB, and CreateProcessAsUser needs to assign the host's primary token */
    RtlAdjustPrivilege(SE_TCB_PRIVILEGE, TRUE, FALSE, &Old);
    RtlAdjustPrivilege(SE_ASSIGNPRIMARYTOKEN_PRIVILEGE, TRUE, FALSE, &Old);

    if (!WlUmfdMakeDesktop())
        goto Fail;

    WlRecBudgetInit(&g_Budget, UMFD_CAP_LAUNCHES + 1, UMFD_CAP_WINDOW_MS);
    WlRecBudgetInit(&g_RecycleBudget, UMFD_RECYCLE_MAX + 1, UMFD_CAP_WINDOW_MS);
    g_Cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_FirstDone = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_Cancel == NULL || g_FirstDone == NULL)
        goto Fail;
    g_Supervisor = CreateThread(NULL, 0, WlUmfdSupervisor, NULL, 0, NULL);
    if (g_Supervisor == NULL)
        goto Fail;

    /* Serving before the logon UI if it can be, but winlogon never blocks on the host */
    if (WaitForSingleObject(g_FirstDone, UMFD_FIRST_WAIT_MS) != WAIT_OBJECT_0)
        TRACE("WL: no first font driver host launch within %u ms; boot goes on\n", UMFD_FIRST_WAIT_MS);
    return;

Fail:
    WlUmfdLog(EVENTLOG_ERROR_TYPE, L"font driver host not started; fonts stay on the kernel path");
    WlUmfdRelease();
}

VOID
WlUmfdStop(VOID)
{
    /* A supervisor left running by an earlier call is not waited for twice */
    if (g_Supervisor == NULL || g_StopDone)
        return;
    g_StopDone = TRUE;

    SetEvent(g_Cancel);
    if (WaitForSingleObject(g_Supervisor, UMFD_JOIN_MS) != WAIT_OBJECT_0)
    {
        /* It may be inside an event-log RPC; leave it and its handles rather than free under it */
        ERR("WL: font driver host supervisor still busy after %u ms; left running\n", UMFD_JOIN_MS);
        return;
    }
    CloseHandle(g_Supervisor);
    g_Supervisor = NULL;
    WlUmfdRelease();
}
