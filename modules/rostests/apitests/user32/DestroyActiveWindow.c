/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test what a thread that is not in the foreground sends when it destroys its active window
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

typedef struct _RECORD
{
    UINT Message;
    HWND Window;
    LONG Object;
} RECORD;

#define EVENT_RECORD 0x10000

static RECORD Records[64];
static int RecordCount;
static BOOL Logging, RefuseDeactivation;
static HWND HelperWindow;

static void AddRecord(UINT Message, HWND Window, LONG Object)
{
    if (!Logging || RecordCount >= ARRAYSIZE(Records))
        return;
    Records[RecordCount].Message = Message;
    Records[RecordCount].Window = Window;
    Records[RecordCount].Object = Object;
    RecordCount++;
}

static void CALLBACK EventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                               LONG idChild, DWORD idThread, DWORD time)
{
    if (event >= EVENT_OBJECT_CREATE && event <= EVENT_OBJECT_HIDE && idObject != OBJID_CURSOR)
        AddRecord(EVENT_RECORD | event, hwnd, idObject);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_NCACTIVATE && !wParam && RefuseDeactivation)
    {
        AddRecord(uMsg, hwnd, 0);
        return FALSE;
    }
    switch (uMsg)
    {
        case WM_NCACTIVATE:
        case WM_ACTIVATE:
        case WM_ACTIVATEAPP:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_DESTROY:
            AddRecord(uMsg, hwnd, 0);
            break;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void PumpMessages(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

static void WaitForForeground(void)
{
    int i;

    for (i = 0; i < 100 && !GetForegroundWindow(); i++)
    {
        Sleep(10);
        PumpMessages();
    }
}

static HWND CreateTestWindow(void)
{
    HWND hwnd;

    hwnd = CreateWindowExW(0, L"DestroyActiveWindowClass", L"DestroyActiveWindow",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 200, 200,
                           NULL, NULL, GetModuleHandleW(NULL), NULL);
    PumpMessages();
    return hwnd;
}

static void DestroyLogged(HWND hwnd)
{
    RecordCount = 0;
    Logging = TRUE;
    DestroyWindow(hwnd);
    Logging = FALSE;
    PumpMessages();
}

static int FindRecord(UINT Message, HWND Window, LONG Object)
{
    int i;

    for (i = 0; i < RecordCount; i++)
    {
        if (Records[i].Message == Message && Records[i].Window == Window && Records[i].Object == Object)
            return i;
    }
    return -1;
}

static int CountMessages(UINT Message)
{
    int i, count = 0;

    for (i = 0; i < RecordCount; i++)
    {
        if (Records[i].Message == Message)
            count++;
    }
    return count;
}

static void CheckNoActivation(const char *Case)
{
    ok(CountMessages(WM_NCACTIVATE) == 0, "%s: %d WM_NCACTIVATE\n", Case, CountMessages(WM_NCACTIVATE));
    ok(CountMessages(WM_ACTIVATE) == 0, "%s: %d WM_ACTIVATE\n", Case, CountMessages(WM_ACTIVATE));
    ok(CountMessages(WM_KILLFOCUS) == 0, "%s: %d WM_KILLFOCUS\n", Case, CountMessages(WM_KILLFOCUS));
    ok(CountMessages(WM_SETFOCUS) == 0, "%s: %d WM_SETFOCUS\n", Case, CountMessages(WM_SETFOCUS));
}

static LRESULT CALLBACK HelperProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_USER + 1)
        return SetForegroundWindow((HWND)wParam);
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static DWORD WINAPI HelperThread(LPVOID Parameter)
{
    MSG msg;

    HelperWindow = CreateWindowExW(0, L"static", NULL, WS_POPUP | WS_VISIBLE, 100, 100, 5, 5,
                                   NULL, NULL, NULL, NULL);
    SetWindowLongPtrW(HelperWindow, GWLP_WNDPROC, (LONG_PTR)HelperProc);
    PumpMessages();
    SetEvent((HANDLE)Parameter);
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        if (msg.message == WM_USER)
        {
            DestroyWindow(HelperWindow);
            HelperWindow = NULL;
            PumpMessages();
            SetEvent((HANDLE)msg.lParam);
            continue;
        }
        DispatchMessageW(&msg);
    }
    return 0;
}

static void TestBackground(void)
{
    HWND hwnd, hwndOther;
    int hide, caret, destroy, ncactivate;

    hwnd = CreateTestWindow();
    ok(GetActiveWindow() == hwnd, "The window is not active\n");
    ok(GetFocus() == hwnd, "The window has no focus\n");
    ok(GetForegroundWindow() == HelperWindow, "The window took the foreground\n");
    DestroyLogged(hwnd);
    hide = FindRecord(EVENT_RECORD | EVENT_OBJECT_HIDE, hwnd, OBJID_WINDOW);
    caret = FindRecord(EVENT_RECORD | EVENT_OBJECT_HIDE, NULL, OBJID_CARET);
    destroy = FindRecord(EVENT_RECORD | EVENT_OBJECT_DESTROY, hwnd, OBJID_WINDOW);
    ok(hide >= 0, "No hide event for the window\n");
    ok(caret > hide, "Caret hide event at %d, window hide at %d\n", caret, hide);
    ok(destroy > caret, "Window destroy event at %d, caret hide at %d\n", destroy, caret);
    ok(FindRecord(WM_DESTROY, hwnd, 0) > destroy, "WM_DESTROY is not after the destroy event\n");
    CheckNoActivation("focused");

    hwndOther = CreateTestWindow();
    hwnd = CreateTestWindow();
    DestroyLogged(hwnd);
    CheckNoActivation("other window");
    ok(FindRecord(EVENT_RECORD | EVENT_OBJECT_FOCUS, hwndOther, OBJID_CLIENT) < 0, "The other window took the focus\n");
    ok(GetActiveWindow() == NULL, "Active window %p\n", GetActiveWindow());
    ok(GetFocus() == NULL, "Focus window %p\n", GetFocus());
    DestroyWindow(hwndOther);
    PumpMessages();

    hwnd = CreateTestWindow();
    CreateCaret(hwnd, NULL, 2, 10);
    ShowCaret(hwnd);
    DestroyLogged(hwnd);
    caret = FindRecord(EVENT_RECORD | EVENT_OBJECT_HIDE, hwnd, OBJID_CARET);
    destroy = FindRecord(EVENT_RECORD | EVENT_OBJECT_DESTROY, hwnd, OBJID_CARET);
    ok(caret >= 0, "No caret hide event\n");
    ok(destroy > caret, "Caret destroy event at %d, caret hide at %d\n", destroy, caret);
    ok(FindRecord(EVENT_RECORD | EVENT_OBJECT_DESTROY, hwnd, OBJID_WINDOW) > destroy,
       "The caret is not destroyed before the window\n");
    CheckNoActivation("visible caret");

    hwnd = CreateTestWindow();
    CreateCaret(hwnd, NULL, 2, 10);
    DestroyLogged(hwnd);
    ok(FindRecord(EVENT_RECORD | EVENT_OBJECT_HIDE, hwnd, OBJID_CARET) < 0, "Hide event for a hidden caret\n");
    ok(FindRecord(EVENT_RECORD | EVENT_OBJECT_HIDE, NULL, OBJID_CARET) < 0, "Caret hide event without a window\n");
    ok(FindRecord(EVENT_RECORD | EVENT_OBJECT_DESTROY, hwnd, OBJID_CARET) >= 0, "No caret destroy event\n");

    hwndOther = CreateTestWindow();
    hwnd = CreateTestWindow();
    CreateCaret(hwndOther, NULL, 2, 10);
    ShowCaret(hwndOther);
    DestroyLogged(hwnd);
    ok(FindRecord(EVENT_RECORD | EVENT_OBJECT_HIDE, hwndOther, OBJID_CARET) >= 0, "No caret hide event\n");
    ok(FindRecord(EVENT_RECORD | EVENT_OBJECT_DESTROY, hwndOther, OBJID_CARET) >= 0, "No caret destroy event\n");
    ok(!DestroyCaret(), "The caret of the other window survived\n");
    CheckNoActivation("caret in other window");
    DestroyWindow(hwndOther);
    PumpMessages();

    hwndOther = CreateTestWindow();
    hwnd = CreateTestWindow();
    DestroyLogged(hwndOther);
    ok_int(FindRecord(EVENT_RECORD | EVENT_OBJECT_HIDE, NULL, OBJID_CARET), -1);
    ok(GetActiveWindow() == hwnd, "The active window changed\n");
    DestroyWindow(hwnd);
    PumpMessages();

    hwnd = CreateTestWindow();
    SendMessageW(HelperWindow, WM_USER + 1, (WPARAM)hwnd, 0);
    PumpMessages();
    ok(GetForegroundWindow() == hwnd, "The window is not the foreground window\n");
    RefuseDeactivation = TRUE;
    DestroyLogged(hwnd);
    RefuseDeactivation = FALSE;
    ncactivate = FindRecord(WM_NCACTIVATE, hwnd, 0);
    ok(ncactivate >= 0, "No WM_NCACTIVATE\n");
    ok(CountMessages(WM_ACTIVATE) == 0, "%d WM_ACTIVATE\n", CountMessages(WM_ACTIVATE));
    ok(CountMessages(WM_ACTIVATEAPP) == 0, "%d WM_ACTIVATEAPP\n", CountMessages(WM_ACTIVATEAPP));
    ok(FindRecord(EVENT_RECORD | EVENT_OBJECT_HIDE, NULL, OBJID_CARET) > ncactivate,
       "No caret hide event after the refused deactivation\n");
    ok(GetActiveWindow() == NULL, "Active window %p\n", GetActiveWindow());
    WaitForForeground();
    ok(GetForegroundWindow() == HelperWindow, "Foreground window %p\n", GetForegroundWindow());
}

START_TEST(DestroyActiveWindow)
{
    WNDCLASSW wc = { 0 };
    HWINEVENTHOOK hook;
    HANDLE ready, thread;
    DWORD tid;
    HWND hwnd;

    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"DestroyActiveWindowClass";
    RegisterClassW(&wc);

    ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    thread = CreateThread(NULL, 0, HelperThread, ready, 0, &tid);
    WaitForSingleObject(ready, INFINITE);
    if (GetForegroundWindow() != HelperWindow)
    {
        skip("The helper window is not the foreground window\n");
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
        WaitForSingleObject(thread, 5000);
        return;
    }

    hook = SetWinEventHook(EVENT_MIN, EVENT_MAX, GetModuleHandleW(NULL), EventProc,
                           GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_INCONTEXT);
    ok(hook != NULL, "SetWinEventHook failed, error %lu\n", GetLastError());

    TestBackground();

    PostThreadMessageW(tid, WM_USER, 0, (LPARAM)ready);
    WaitForSingleObject(ready, INFINITE);
    PumpMessages();

    hwnd = CreateTestWindow();
    SetForegroundWindow(hwnd);
    PumpMessages();
    if (GetForegroundWindow() == hwnd)
    {
        DestroyLogged(hwnd);
        ok(FindRecord(EVENT_RECORD | EVENT_OBJECT_DESTROY, hwnd, OBJID_WINDOW) >= 0, "No destroy event for the window\n");
        ok_int(FindRecord(EVENT_RECORD | EVENT_OBJECT_HIDE, NULL, OBJID_CARET), -1);
        ok(FindRecord(WM_NCACTIVATE, hwnd, 0) >= 0, "No WM_NCACTIVATE for the foreground window\n");
    }
    else
    {
        skip("The window could not take the foreground\n");
        DestroyWindow(hwnd);
    }

    UnhookWinEvent(hook);
    PostThreadMessageW(tid, WM_QUIT, 0, 0);
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    CloseHandle(ready);
    UnregisterClassW(L"DestroyActiveWindowClass", GetModuleHandleW(NULL));
}
