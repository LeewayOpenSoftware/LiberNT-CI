/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for wlrmdr.exe: exit codes, dialog text, stop-event dismissal and timeout
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 *
 * The dialog cases need an interactive desktop; they fail honestly without one.
 */

#include <apitest.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "wlrmdr.h"
#include "resource.h"

#define WAIT_MS 15000

static BOOL start_wlrmdr(PCWSTR args, PROCESS_INFORMATION *pi)
{
    WCHAR cmd[1024], dir[MAX_PATH];
    STARTUPINFOW si = { sizeof(si) };

    GetSystemDirectoryW(dir, ARRAYSIZE(dir));
    swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\\wlrmdr.exe\" %ls", dir, args);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, pi))
    {
        skip("cannot start wlrmdr.exe: %lu\n", GetLastError());
        return FALSE;
    }
    return TRUE;
}

static DWORD finish(PROCESS_INFORMATION *pi, DWORD timeout)
{
    DWORD code = (DWORD)-1;

    if (WaitForSingleObject(pi->hProcess, timeout) != WAIT_OBJECT_0)
    {
        TerminateProcess(pi->hProcess, 0xDEAD);
        WaitForSingleObject(pi->hProcess, 5000);
    }
    GetExitCodeProcess(pi->hProcess, &code);
    CloseHandle(pi->hThread);
    CloseHandle(pi->hProcess);
    return code;
}

/* The stop event is created by wlrmdr itself; poll until it exists, then signal it as winlogon would */
static BOOL signal_stop(DWORD pid)
{
    WCHAR name[96];
    HANDLE ev = NULL;
    DWORD i;

    swprintf(name, ARRAYSIZE(name), L"%ls%lu", WLRMDR_STOP_EVENT_PREFIX, pid);
    for (i = 0; i < 100 && !ev; i++)
    {
        ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
        if (!ev)
            Sleep(100);
    }
    if (!ev)
        return FALSE;
    SetEvent(ev);
    CloseHandle(ev);
    return TRUE;
}

typedef struct { DWORD pid; HWND hwnd; } FINDWND;

static BOOL CALLBACK find_proc(HWND hwnd, LPARAM lp)
{
    FINDWND *f = (FINDWND *)lp;
    DWORD pid;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == f->pid && IsWindowVisible(hwnd))
    {
        f->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND wait_for_dialog(DWORD pid)
{
    FINDWND f = { pid, NULL };
    DWORD i;

    for (i = 0; i < 100 && !f.hwnd; i++)
    {
        EnumWindows(find_proc, (LPARAM)&f);
        if (!f.hwnd)
            Sleep(100);
    }
    return f.hwnd;
}

static void test_silent_instances(void)
{
    PROCESS_INFORMATION pi;
    DWORD code, start;

    /* -s 0 and a short -s without a message: nothing to show, clean exit */
    if (start_wlrmdr(L"-s 0", &pi))
        ok((code = finish(&pi, WAIT_MS)) == 0, "-s 0 exit %lu\n", code);

    start = GetTickCount();
    if (start_wlrmdr(L"-s 400", &pi))
    {
        code = finish(&pi, WAIT_MS);
        ok(code == 0, "-s 400 exit %lu\n", code);
        ok(GetTickCount() - start >= 350, "-s 400 returned after %lu ms\n", GetTickCount() - start);
    }

    /* The persistent instance stays up until the stop event is signalled, then exits 0 */
    if (start_wlrmdr(L"-s -1", &pi))
    {
        ok(WaitForSingleObject(pi.hProcess, 1000) == WAIT_TIMEOUT, "-s -1 exited by itself\n");
        ok(signal_stop(pi.dwProcessId), "stop event was never created\n");
        ok((code = finish(&pi, WAIT_MS)) == 0, "-s -1 after stop exit %lu\n", code);
    }
}

static void test_dialog(void)
{
    PROCESS_INFORMATION pi;
    HWND hwnd, msg;
    WCHAR text[128];
    DWORD code;

    /* Dismissal by winlogon's stop event closes the dialog with ERROR_OPERATION_ABORTED */
    if (start_wlrmdr(L"-s -1 -f 2 -t \"Test title\" -m \"Test message\" -a 3", &pi))
    {
        hwnd = wait_for_dialog(pi.dwProcessId);
        ok(hwnd != NULL, "dialog did not appear\n");
        if (hwnd)
        {
            GetWindowTextW(hwnd, text, ARRAYSIZE(text));
            ok(!wcscmp(text, L"Test title"), "title %ls\n", text);
            msg = GetDlgItem(hwnd, IDC_REM_MESSAGE);
            GetWindowTextW(msg, text, ARRAYSIZE(text));
            ok(!wcscmp(text, L"Test message"), "message %ls\n", text);
            ok(GetDlgItem(hwnd, IDCLOSE) != NULL, "Close button missing\n");
        }
        ok(signal_stop(pi.dwProcessId), "stop event was never created\n");
        ok((code = finish(&pi, WAIT_MS)) == WLRMDR_EXIT_DISMISSED, "dismissed exit %lu\n", code);
    }

    /* The Close button ends it with 0 */
    if (start_wlrmdr(L"-s -1 -t Close -m \"Close me\"", &pi))
    {
        hwnd = wait_for_dialog(pi.dwProcessId);
        ok(hwnd != NULL, "dialog did not appear\n");
        if (hwnd)
            PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDCLOSE, BN_CLICKED), (LPARAM)GetDlgItem(hwnd, IDCLOSE));
        ok((code = finish(&pi, WAIT_MS)) == 0, "Close exit %lu\n", code);
    }

    /* A finite -s closes the dialog by itself with 0 */
    if (start_wlrmdr(L"-s 800 -t Timeout -m \"Goes away\"", &pi))
        ok((code = finish(&pi, WAIT_MS)) == 0, "timeout exit %lu\n", code);
}

START_TEST(process)
{
    test_silent_instances();
    test_dialog();
}
