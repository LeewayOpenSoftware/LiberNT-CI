/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test that switching the input desktop away and back keeps its foreground window
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

static HWND MainWindow;
static LONG KeyDowns;
static HDESK OriginalDesktop;
static HDESK OtherDesktop;
static BOOL HelperSwitched;
static BOOL HelperForeground;
static HWND ExitWindow;
static BOOL ExitForeground;

static LRESULT CALLBACK MainProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_KEYDOWN && wParam == 'Q')
        InterlockedIncrement(&KeyDowns);
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void PumpMessages(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void PumpFor(DWORD Milliseconds)
{
    DWORD Start = GetTickCount();

    do
    {
        PumpMessages();
        Sleep(10);
    } while (GetTickCount() - Start < Milliseconds);
    PumpMessages();
}

/* Like the LogonUI host at lock: a window on another desktop becomes the foreground
 * there, the input desktop switches back, then the window and its thread go away. */
static DWORD WINAPI HelperThread(LPVOID Parameter)
{
    HWND Window;
    MSG msg;

    if (!SetThreadDesktop(OtherDesktop))
        return 1;

    Window = CreateWindowExW(WS_EX_TOPMOST, L"static", NULL, WS_POPUP | WS_VISIBLE, 0, 0, 100, 100,
                             NULL, NULL, NULL, NULL);
    if (!Window)
        return 2;

    if (SwitchDesktop(OtherDesktop))
    {
        HelperSwitched = TRUE;
        SetForegroundWindow(Window);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
        HelperForeground = (GetForegroundWindow() == Window);
        SwitchDesktop(OriginalDesktop);
    }

    DestroyWindow(Window);
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
    return 0;
}

/* A thread that owns the foreground window on the input desktop exits without
 * destroying it: the foreground must not keep pointing at its queue. */
static DWORD WINAPI ExitingThread(LPVOID Parameter)
{
    MSG msg;
    int i;

    ExitWindow = CreateWindowExW(0, L"static", NULL, WS_POPUP | WS_VISIBLE, 150, 150, 100, 100,
                                 NULL, NULL, NULL, NULL);
    if (!ExitWindow)
        return 1;
    SetForegroundWindow(ExitWindow);
    for (i = 0; i < 50 && GetForegroundWindow() != ExitWindow; i++)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
        Sleep(10);
    }
    ExitForeground = (GetForegroundWindow() == ExitWindow);
    return 0;
}

static void TestForegroundThreadExit(void)
{
    HANDLE Thread;
    HWND Foreground;
    DWORD ExitCode = 0;

    Thread = CreateThread(NULL, 0, ExitingThread, NULL, 0, NULL);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Thread)
        return;
    while (MsgWaitForMultipleObjects(1, &Thread, FALSE, 5000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
        PumpMessages();
    GetExitCodeThread(Thread, &ExitCode);
    CloseHandle(Thread);
    ok(ExitCode == 0, "Exiting thread failed: %lu\n", ExitCode);
    if (!ExitForeground)
    {
        skip("The exiting thread's window did not become the foreground window\n");
        return;
    }

    PumpFor(200);
    ok(!IsWindow(ExitWindow), "The exiting thread's window still exists\n");
    Foreground = GetForegroundWindow();
    ok(Foreground != ExitWindow, "The foreground window is still the exited thread's window %p\n", Foreground);

    /* Input routing must not touch the dead queue */
    keybd_event('Q', 0, 0, 0);
    keybd_event('Q', 0, KEYEVENTF_KEYUP, 0);
    PumpFor(200);
    ok(TRUE, "Keyboard input after the foreground thread exited did not fault\n");
}

START_TEST(ForegroundSwitchDesktop)
{
    WNDCLASSW wc = { 0 };
    HANDLE Thread;
    DWORD ExitCode = 0;
    LONG Before;
    int i;

    OriginalDesktop = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (!OriginalDesktop)
    {
        skip("OpenInputDesktop failed: %lu\n", GetLastError());
        return;
    }
    OtherDesktop = CreateDesktopW(L"ForegroundSwitchDesktop", NULL, NULL, 0, GENERIC_ALL, NULL);
    if (!OtherDesktop)
    {
        skip("CreateDesktopW failed: %lu\n", GetLastError());
        CloseDesktop(OriginalDesktop);
        return;
    }

    wc.lpfnWndProc = MainProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ForegroundSwitchDesktopMain";
    RegisterClassW(&wc);
    MainWindow = CreateWindowExW(0, wc.lpszClassName, L"ForegroundSwitchDesktop", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 100, 100, 300, 200, NULL, NULL, wc.hInstance, NULL);
    ok(MainWindow != NULL, "CreateWindowExW failed: %lu\n", GetLastError());
    if (!MainWindow)
        goto Cleanup;

    SetForegroundWindow(MainWindow);
    SetFocus(MainWindow);
    for (i = 0; i < 100 && GetForegroundWindow() != MainWindow; i++)
        PumpFor(10);
    if (GetForegroundWindow() != MainWindow)
    {
        skip("The test window could not become the foreground window\n");
        goto Cleanup;
    }

    Thread = CreateThread(NULL, 0, HelperThread, NULL, 0, NULL);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Thread)
        goto Cleanup;
    while (MsgWaitForMultipleObjects(1, &Thread, FALSE, 5000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
        PumpMessages();
    GetExitCodeThread(Thread, &ExitCode);
    CloseHandle(Thread);
    ok(ExitCode == 0, "Helper thread failed: %lu\n", ExitCode);
    if (!HelperSwitched)
    {
        skip("SwitchDesktop to the test desktop failed\n");
        goto Cleanup;
    }
    ok(HelperForeground, "The helper window did not become the foreground window on the test desktop\n");

    PumpFor(200);
    ok(GetForegroundWindow() == MainWindow, "Foreground after switching back is %p, expected %p\n",
       GetForegroundWindow(), MainWindow);
    ok(GetFocus() == MainWindow, "Focus after switching back is %p, expected %p\n", GetFocus(), MainWindow);

    /* The keyboard must reach the window without a click */
    Before = KeyDowns;
    keybd_event('Q', 0, 0, 0);
    keybd_event('Q', 0, KEYEVENTF_KEYUP, 0);
    PumpFor(300);
    ok(KeyDowns == Before + 1, "The key went to %ld window(s), expected 1\n", KeyDowns - Before);

    TestForegroundThreadExit();

Cleanup:
    if (MainWindow)
        DestroyWindow(MainWindow);
    UnregisterClassW(L"ForegroundSwitchDesktopMain", GetModuleHandleW(NULL));
    CloseDesktop(OtherDesktop);
    CloseDesktop(OriginalDesktop);
}
