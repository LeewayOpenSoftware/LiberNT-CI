/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The timed-shutdown reminder through wlrmdr.exe (PHASE3-PLAN commit 13):
 *              spawned as the signed-in user on the user's desktop, dismissed through
 *              its stop event, never waited on, never terminated. The inline dialog in
 *              shutdown.c stays the fallback whenever this cannot run.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "winlogon.h"

#include <userenv.h>

/* wlrmdr.exe contract (base/system/wlrmdr/wlrmdr.h): the child creates
 * Local\Wl_Stop_Reminder_Event_<pid>; a dialog dismissed through it exits 995. */
#define WLR_IMAGE               L"wlrmdr.exe"
#define WLR_STOP_EVENT_PREFIX   L"Local\\Wl_Stop_Reminder_Event_"
#define WLR_ABORT_NOTICE_MS     120000      /* the -s 120000 instance Windows spawns on shutdown /a */
#define WLR_FLAGS_WARNING       2           /* -f: warning icon, as observed */
#define WLR_ACTION_SHUTDOWN     3           /* -a: the only observed scenario */

static BOOL
WlrImagePath(
    _Out_writes_(MAX_PATH) PWSTR Path)
{
    UINT Length = GetSystemDirectoryW(Path, MAX_PATH);
    if (Length == 0 || Length >= MAX_PATH)
        return FALSE;
    return SUCCEEDED(StringCchCatW(Path, MAX_PATH, L"\\")) &&
           SUCCEEDED(StringCchCatW(Path, MAX_PATH, WLR_IMAGE)) &&
           GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

/* One quoted argument with the CommandLineToArgvW rules: \" for a quote, backslashes before it doubled. */
static BOOL
WlrAppendQuoted(
    _Inout_updates_(Count) PWSTR Line,
    _In_ SIZE_T Count,
    _In_ PCWSTR Text)
{
    SIZE_T Used = wcslen(Line), i, Slashes = 0;

    if (Used + 2 >= Count)
        return FALSE;
    Line[Used++] = L' ';
    Line[Used++] = L'"';
    for (i = 0; Text[i]; i++)
    {
        WCHAR c = Text[i];
        if (c == L'\\')
        {
            Slashes++;
        }
        else if (c == L'"')
        {
            if (Used + Slashes + 2 >= Count) return FALSE;
            while (Slashes--) Line[Used++] = L'\\';   /* double the run before the quote */
            Slashes = 0;
            Line[Used++] = L'\\';
        }
        else
        {
            Slashes = 0;
        }
        if (Used + 1 >= Count)
            return FALSE;
        Line[Used++] = c;
    }
    if (Used + Slashes + 2 >= Count)
        return FALSE;
    while (Slashes--) Line[Used++] = L'\\';           /* a trailing run must not escape the closing quote */
    Line[Used++] = L'"';
    Line[Used] = UNICODE_NULL;
    return TRUE;
}

/* Starts wlrmdr as the signed-in user on the user's desktop; the handle is kept only to tell the PID. */
static BOOL
WlrSpawn(
    _In_ PCWSTR Arguments,
    _Out_opt_ PHANDLE Process,
    _Out_opt_ PDWORD Pid)
{
    WCHAR Path[MAX_PATH];
    WCHAR Line[2048];
    STARTUPINFOW Si;
    PROCESS_INFORMATION Pi;
    PVOID Environment = NULL;
    BOOL Ok;

    if (Process) *Process = NULL;
    if (Pid) *Pid = 0;

    if (WLSession == NULL || WLSession->UserToken == NULL || WLSession->LogonState != STATE_LOGGED_ON)
        return FALSE;                   /* no user desktop to show it on: the inline dialog serves */
    if (!WlrImagePath(Path))
        return FALSE;
    if (FAILED(StringCchPrintfW(Line, ARRAYSIZE(Line), L"\"%s\" %s", Path, Arguments)))
        return FALSE;

    if (!CreateEnvironmentBlock(&Environment, WLSession->UserToken, FALSE))
        Environment = NULL;
    ZeroMemory(&Si, sizeof(Si));
    Si.cb = sizeof(Si);
    Si.lpDesktop = L"WinSta0\\Default";
    Ok = CreateProcessAsUserW(WLSession->UserToken, Path, Line, NULL, NULL, FALSE,
                              CREATE_UNICODE_ENVIRONMENT, Environment, NULL, &Si, &Pi);
    if (Environment)
        DestroyEnvironmentBlock(Environment);
    if (!Ok)
    {
        ERR("WL: wlrmdr launch failed (%lu)\n", GetLastError());
        return FALSE;
    }
    CloseHandle(Pi.hThread);
    if (Pid) *Pid = Pi.dwProcessId;
    if (Process) *Process = Pi.hProcess;
    else CloseHandle(Pi.hProcess);
    TRACE("WL: wlrmdr pid %lu: %ls\n", Pi.dwProcessId, Arguments);
    return TRUE;
}

/* The persistent reminder of a pending timed shutdown or restart. FALSE = use the inline dialog. */
BOOL
WlrShowShutdownReminder(
    _Out_ PWLR_REMINDER Reminder,
    _In_opt_ PCWSTR Message,
    _In_ ULONG TimeoutSeconds,
    _In_ BOOL Reboot)
{
    WCHAR Title[128], Format[128], Time[32], Body[512];
    WCHAR Arguments[1600];

    ZeroMemory(Reminder, sizeof(*Reminder));
    if (TimeoutSeconds == 0)
        return FALSE;

    if (LoadStringW(hAppInstance, IDS_REMINDER_TITLE, Title, ARRAYSIZE(Title)) == 0)
        return FALSE;
    if (Message && Message[0])
    {
        StringCchCopyW(Body, ARRAYSIZE(Body), Message);
    }
    else
    {
        if (TimeoutSeconds < 86400)
        {
            LoadStringW(hAppInstance, IDS_TIMEOUTSHORTFORMAT, Format, ARRAYSIZE(Format));
            StringCchPrintfW(Time, ARRAYSIZE(Time), Format, (int)(TimeoutSeconds / 3600),
                             (int)(TimeoutSeconds / 60 % 60), (int)(TimeoutSeconds % 60));
        }
        else
        {
            LoadStringW(hAppInstance, IDS_TIMEOUTLONGFORMAT, Format, ARRAYSIZE(Format));
            StringCchPrintfW(Time, ARRAYSIZE(Time), Format, (int)(TimeoutSeconds / 86400));
        }
        if (LoadStringW(hAppInstance, Reboot ? IDS_REMINDER_RESTART : IDS_REMINDER_SHUTDOWN,
                        Format, ARRAYSIZE(Format)) == 0)
            return FALSE;
        StringCchPrintfW(Body, ARRAYSIZE(Body), Format, Time);
    }

    /* The observed line: -s -1 (until dismissed) -f 2 -t <title> -m <message> -a 3 */
    StringCchPrintfW(Arguments, ARRAYSIZE(Arguments), L"-s -1 -f %u -t", WLR_FLAGS_WARNING);
    if (!WlrAppendQuoted(Arguments, ARRAYSIZE(Arguments), Title))
        return FALSE;
    if (FAILED(StringCchCatW(Arguments, ARRAYSIZE(Arguments), L" -m")) ||
        !WlrAppendQuoted(Arguments, ARRAYSIZE(Arguments), Body) ||
        FAILED(StringCchCatW(Arguments, ARRAYSIZE(Arguments), L" -a 3")))
        return FALSE;

    return WlrSpawn(Arguments, &Reminder->Process, &Reminder->Pid);
}

/* Signals the child's stop event: the dialog exits 995. Never waits on anything the child owns;
 * the only wait is a short retry while the child is still creating its event. */
VOID
WlrDismissShutdownReminder(
    _Inout_ PWLR_REMINDER Reminder)
{
    WCHAR Name[96];
    HANDLE Event = NULL;
    DWORD Error = ERROR_SUCCESS, Try;

    if (Reminder->Process == NULL)
        return;
    StringCchPrintfW(Name, ARRAYSIZE(Name), L"%s%lu", WLR_STOP_EVENT_PREFIX, Reminder->Pid);
    for (Try = 0; Try < 20 && Event == NULL; Try++)
    {
        Event = OpenEventW(EVENT_MODIFY_STATE, FALSE, Name);
        if (Event != NULL)
            break;
        Error = GetLastError();
        if (Error != ERROR_FILE_NOT_FOUND || WaitForSingleObject(Reminder->Process, 0) != WAIT_TIMEOUT)
            break;                      /* not a start-up race: the child is gone or the name is foreign */
        Sleep(100);
    }
    if (Event != NULL)
    {
        SetEvent(Event);
        CloseHandle(Event);
    }
    else
    {
        /* The event is the child's; without it the user closes the dialog by hand. */
        WARN("WL: wlrmdr pid %lu has no stop event (%lu)\n", Reminder->Pid, Error);
    }
    CloseHandle(Reminder->Process);
    Reminder->Process = NULL;
    Reminder->Pid = 0;
}

/* shutdown /a: the two-minute silent instance Windows spawns after the abort. */
VOID
WlrShowAbortNotice(VOID)
{
    WCHAR Arguments[32];

    StringCchPrintfW(Arguments, ARRAYSIZE(Arguments), L"-s %u", WLR_ABORT_NOTICE_MS);
    WlrSpawn(Arguments, NULL, NULL);
}
