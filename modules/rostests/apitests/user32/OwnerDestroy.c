/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test a window that outlives its destroyed child-style owner
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

static HWND CreateTestWindow(DWORD Style, HWND Parent)
{
    return CreateWindowExW(0, L"OwnerDestroyClass", L"OwnerDestroy", Style,
                           10, 10, 100, 100, Parent, NULL, GetModuleHandleW(NULL), NULL);
}

static void FlushMessages(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

static void TestOwner(DWORD OwnerStyle, BOOL Reparent)
{
    HWND hwndMain, hwndOwner, hwndOwned;

    hwndMain = CreateTestWindow(WS_OVERLAPPEDWINDOW | WS_VISIBLE, NULL);
    ok(hwndMain != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    hwndOwner = CreateTestWindow(OwnerStyle, Reparent ? NULL : hwndMain);
    ok(hwndOwner != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    if (Reparent)
        SetParent(hwndOwner, hwndMain);
    hwndOwned = CreateTestWindow(WS_OVERLAPPEDWINDOW | WS_VISIBLE, hwndOwner);
    ok(hwndOwned != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    ok(GetWindow(hwndOwned, GW_OWNER) == hwndOwner, "GW_OWNER is %p, expected %p\n",
       GetWindow(hwndOwned, GW_OWNER), hwndOwner);
    SetActiveWindow(hwndOwned);
    FlushMessages();

    ok(DestroyWindow(hwndOwner), "DestroyWindow failed, error %lu\n", GetLastError());
    ok(!IsWindow(hwndOwner), "The owner still exists\n");
    ok(IsWindow(hwndOwned), "The owned window was destroyed with its owner\n");
    ok(GetWindow(hwndOwned, GW_OWNER) == hwndOwner, "GW_OWNER is %p, expected %p\n",
       GetWindow(hwndOwned, GW_OWNER), hwndOwner);
    FlushMessages();

    ok(DestroyWindow(hwndOwned), "DestroyWindow failed, error %lu\n", GetLastError());
    ok(!IsWindow(hwndOwned), "The owned window still exists\n");
    ok(DestroyWindow(hwndMain), "DestroyWindow failed, error %lu\n", GetLastError());
}

START_TEST(OwnerDestroy)
{
    WNDCLASSW wc = { 0 };

    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"OwnerDestroyClass";
    ok(RegisterClassW(&wc) != 0, "RegisterClassW failed, error %lu\n", GetLastError());

    TestOwner(WS_VISIBLE | WS_POPUP | WS_CHILD, TRUE);
    TestOwner(WS_VISIBLE | WS_POPUP | WS_CHILD, FALSE);

    UnregisterClassW(L"OwnerDestroyClass", GetModuleHandleW(NULL));
}
