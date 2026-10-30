/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test DefFrameProc with a client window handle that is not a window
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

START_TEST(DefFrameProc)
{
    CLIENTCREATESTRUCT ccs = { 0 };
    HMENU menu, submenu;
    HWND frame, client, child;
    WCHAR textW[32];

    menu = CreateMenu();
    submenu = CreatePopupMenu();
    AppendMenuW(submenu, MF_STRING, 1, L"x");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)submenu, L"Window");
    ccs.hWindowMenu = submenu;
    ccs.idFirstChild = 100;

    frame = CreateWindowExW(0, L"static", L"frame", WS_OVERLAPPEDWINDOW, 50, 50, 400, 300,
                            NULL, menu, GetModuleHandleW(NULL), NULL);
    ok(frame != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    client = CreateWindowExW(0, L"MDICLIENT", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 300, 200,
                             frame, NULL, GetModuleHandleW(NULL), &ccs);
    ok(client != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    child = CreateWindowExW(0, L"static", NULL, WS_CHILD, 0, 0, 10, 10, frame, NULL, NULL, NULL);
    if (!frame || !client || !child)
        return;

    ok_long((LONG)DefFrameProcW(frame, client, WM_NCACTIVATE, FALSE, 0), 1);
    ok_long((LONG)DefFrameProcA(frame, client, WM_GETTEXTLENGTH, 0, 0), 5);
    ok_long((LONG)DefFrameProcW(frame, NULL, WM_NCACTIVATE, FALSE, 0), 1);
    ok_long((LONG)DefFrameProcW(frame, child, WM_NCACTIVATE, FALSE, 0), 1);

    DestroyWindow(client);
    ok_long((LONG)DefFrameProcA(frame, client, WM_NCACTIVATE, FALSE, 0), 0);
    ok_long((LONG)DefFrameProcW(frame, client, WM_NCACTIVATE, TRUE, 0), 0);
    ok_long((LONG)DefFrameProcA(frame, client, WM_GETTEXTLENGTH, 0, 0), 0);
    ok_long((LONG)DefFrameProcW(frame, client, WM_GETTEXTLENGTH, 0, 0), 0);
    ok_long((LONG)DefFrameProcW(frame, client, WM_GETTEXT, ARRAYSIZE(textW), (LPARAM)textW), 0);
    ok_long((LONG)DefFrameProcA(frame, client, WM_SETTEXT, 0, (LPARAM)"changed"), 0);
    ok_int(GetWindowTextLengthW(frame), 5);
    ok_long((LONG)DefFrameProcA(frame, (HWND)(ULONG_PTR)0x1234, WM_GETTEXTLENGTH, 0, 0), 0);

    DestroyWindow(frame);
}
