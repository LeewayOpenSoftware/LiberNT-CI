/*
 * PROJECT:     Orosi Logon Reminder
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     wlrmdr.exe: modal reminder dialog launched by winlogon on the user's desktop
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <stdlib.h>
#include <stdio.h>
#include <windef.h>
#include <winbase.h>
#include <winuser.h>

#include "wlrmdr.h"
#include "resource.h"

#define POLL_TIMER_ID   1
#define POLL_INTERVAL   100

typedef struct _REMINDER
{
    const WLRMDR_ARGS *Args;
    HANDLE StopEvent;
    DWORD StartTick;
    BOOL Dismissed;
} REMINDER;

static HANDLE CreateStopEvent(void)
{
    WCHAR Name[ARRAYSIZE(WLRMDR_STOP_EVENT_PREFIX) + 12];

    swprintf(Name, ARRAYSIZE(Name), L"%ls%lu", WLRMDR_STOP_EVENT_PREFIX, GetCurrentProcessId());
    return CreateEventW(NULL, TRUE, FALSE, Name);
}

static BOOL TimedOut(const REMINDER *Reminder)
{
    LONG Timeout = Reminder->Args->TimeoutMs;

    return Timeout >= 0 && (DWORD)(GetTickCount() - Reminder->StartTick) >= (DWORD)Timeout;
}

static LPCWSTR IconForFlags(ULONG Flags)
{
    switch (Flags & 0x0F)
    {
        case 1: return IDI_INFORMATION;
        case 2: return IDI_WARNING;
        case 3: return IDI_ERROR;
        default: return NULL;
    }
}

static UINT BeepForFlags(ULONG Flags)
{
    switch (Flags & 0x0F)
    {
        case 1: return MB_ICONINFORMATION;
        case 2: return MB_ICONWARNING;
        case 3: return MB_ICONERROR;
        default: return MB_OK;
    }
}

static INT_PTR CALLBACK ReminderDlgProc(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    REMINDER *Reminder;

    switch (Msg)
    {
        case WM_INITDIALOG:
        {
            LPCWSTR Icon;

            Reminder = (REMINDER *)lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Reminder);
            if (Reminder->Args->Title)
                SetWindowTextW(hDlg, Reminder->Args->Title);
            SetDlgItemTextW(hDlg, IDC_REM_MESSAGE, Reminder->Args->Message);

            Icon = IconForFlags(Reminder->Args->Flags);
            if (Icon)
                SendDlgItemMessageW(hDlg, IDC_REM_ICON, STM_SETICON, (WPARAM)LoadIconW(NULL, Icon), 0);
            else
                ShowWindow(GetDlgItem(hDlg, IDC_REM_ICON), SW_HIDE);
            if (!(Reminder->Args->Flags & 0x10))
                MessageBeep(BeepForFlags(Reminder->Args->Flags));

            Reminder->StartTick = GetTickCount();
            SetTimer(hDlg, POLL_TIMER_ID, POLL_INTERVAL, NULL);
            SetForegroundWindow(hDlg);
            return TRUE;
        }

        case WM_TIMER:
            Reminder = (REMINDER *)GetWindowLongPtrW(hDlg, DWLP_USER);
            if (WaitForSingleObject(Reminder->StopEvent, 0) == WAIT_OBJECT_0)
            {
                Reminder->Dismissed = TRUE;
                EndDialog(hDlg, IDABORT);
            }
            else if (TimedOut(Reminder))
            {
                EndDialog(hDlg, IDOK);
            }
            return TRUE;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDCLOSE || LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hDlg, IDCLOSE);
                return TRUE;
            }
            break;

        case WM_CLOSE:
            EndDialog(hDlg, IDCLOSE);
            return TRUE;

        case WM_DESTROY:
            KillTimer(hDlg, POLL_TIMER_ID);
            break;
    }
    return FALSE;
}

/* Without a message there is nothing to show: wait for the stop event or the timeout */
static DWORD WaitSilently(const WLRMDR_ARGS *Args, HANDLE StopEvent)
{
    DWORD Wait = WaitForSingleObject(StopEvent, Args->TimeoutMs < 0 ? INFINITE : (DWORD)Args->TimeoutMs);

    return (Wait == WAIT_OBJECT_0 || Wait == WAIT_TIMEOUT) ? ERROR_SUCCESS : GetLastError();
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, PWSTR pCmdLine, int nShow)
{
    WLRMDR_ARGS Args;
    REMINDER Reminder;
    INT_PTR Result;
    DWORD ExitCode;
    HANDLE StopEvent;

    UNREFERENCED_PARAMETER(hPrev);
    UNREFERENCED_PARAMETER(pCmdLine);
    UNREFERENCED_PARAMETER(nShow);

    WlrParseCmdLine(__argc, __wargv, &Args);

    StopEvent = CreateStopEvent();
    if (!StopEvent)
        return GetLastError();

    if (!Args.Message)
    {
        ExitCode = WaitSilently(&Args, StopEvent);
        CloseHandle(StopEvent);
        return ExitCode;
    }

    if (Args.TimeoutMs == 0)
    {
        CloseHandle(StopEvent);
        return ERROR_SUCCESS;
    }

    Reminder.Args = &Args;
    Reminder.StopEvent = StopEvent;
    Reminder.StartTick = 0;
    Reminder.Dismissed = FALSE;

    Result = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_REMINDER), NULL, ReminderDlgProc, (LPARAM)&Reminder);
    ExitCode = (Result == -1) ? GetLastError() : (Reminder.Dismissed ? WLRMDR_EXIT_DISMISSED : ERROR_SUCCESS);
    CloseHandle(StopEvent);
    return ExitCode;
}
