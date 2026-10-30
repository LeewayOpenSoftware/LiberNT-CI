/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test the foreground right of a process started by the foreground process
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

static HWND CreateTestWindow(void)
{
    return CreateWindowExW(0, L"static", L"ForegroundStartup", WS_OVERLAPPEDWINDOW,
                           100, 100, 200, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static void PumpMessages(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

static int RunChild(void)
{
    HWND hwnd = CreateTestWindow();
    int ret;

    if (!hwnd)
        return 2;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    PumpMessages();
    ret = (GetForegroundWindow() == hwnd) ? 0 : 1;
    DestroyWindow(hwnd);
    return ret;
}

START_TEST(ForegroundStartup)
{
    char **test_argv;
    int argc = winetest_get_mainargs(&test_argv);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    char cmdline[MAX_PATH + 32];
    DWORD code = 0xdeadbeef;
    HWND hwnd;

    if (argc >= 3 && !strcmp(test_argv[2], "child"))
        ExitProcess(RunChild());

    ok(GetProcAddress(GetModuleHandleW(L"user32.dll"), "ConsoleControl") != NULL,
       "ConsoleControl is not exported\n");

    hwnd = CreateTestWindow();
    ok(hwnd != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    if (!hwnd)
        return;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    PumpMessages();
    if (GetForegroundWindow() != hwnd)
    {
        skip("The test process is not the foreground process\n");
        DestroyWindow(hwnd);
        return;
    }

    sprintf(cmdline, "\"%s\" ForegroundStartup child", test_argv[0]);
    ok(CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi),
       "CreateProcessA failed, error %lu\n", GetLastError());
    if (pi.hProcess)
    {
        WaitForSingleObject(pi.hProcess, 30000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    ok(code == 0, "A process started by the foreground process did not get the foreground, code %lu\n", code);

    DestroyWindow(hwnd);
}
