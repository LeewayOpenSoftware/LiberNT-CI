/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the Usermode Font Driver Host started outside its launcher
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windows.h>

static WCHAR host_path[MAX_PATH];

/* Starts the host; TRUE with its exit code if it exited within 10 s */
static BOOL run_host(const WCHAR *args, DWORD *exit_code)
{
    WCHAR cmdline[MAX_PATH + 64];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    BOOL exited = FALSE;

    wsprintfW(cmdline, L"\"%s\"%s%s", host_path, args ? L" " : L"", args ? args : L"");
    if (!CreateProcessW(host_path, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        ok(0, "CreateProcessW(%S) failed: %lu\n", cmdline, GetLastError());
        return FALSE;
    }
    if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0)
        exited = GetExitCodeProcess(pi.hProcess, exit_code);
    else
        ok(0, "%S is still running after 10 s\n", cmdline);
    if (!exited)
        TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exited;
}

START_TEST(fontdrvhost)
{
    DWORD code;

    GetSystemDirectoryW(host_path, MAX_PATH);
    lstrcatW(host_path, L"\\fontdrvhost.exe");
    if (GetFileAttributesW(host_path) == INVALID_FILE_ATTRIBUTES)
    {
        skip("%S is not installed\n", host_path);
        return;
    }

    /* Outside its launcher the host never stays resident and never reports success */
    if (run_host(NULL, &code))
    {
        ok(code != 0, "host without arguments exited with success\n");
        trace("no arguments: exit code %lu\n", code);
        if (is_reactos())
            ok_dec(code, ERROR_NOT_SUPPORTED);
    }

    if (run_host(L"/x", &code))
    {
        ok(code != 0, "host with an argument exited with success\n");
        trace("one argument: exit code %lu\n", code);
        if (is_reactos())
            ok_dec(code, ERROR_INVALID_PARAMETER);
    }
}
