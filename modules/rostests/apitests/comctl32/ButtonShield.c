/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     BCM_SETSHIELD on the v6 button and the IDI_SHIELD system icon
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "wine/test.h"
#include <stdio.h>
#include <windows.h>
#include <commctrl.h>

START_TEST(ButtonShield)
{
    HWND hwnd;
    HICON icon;
    LRESULT ret;
    SIZE plain = { 0 }, shielded = { 0 };

    icon = LoadIconW(NULL, (LPCWSTR)IDI_SHIELD);
    ok(icon != NULL, "LoadIcon(IDI_SHIELD) failed, error %lu\n", GetLastError());

    hwnd = CreateWindowExW(0, WC_BUTTONW, L"Install", WS_POPUP | BS_PUSHBUTTON,
                           0, 0, 120, 30, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "CreateWindowEx failed, error %lu\n", GetLastError());
    if (!hwnd)
        return;

    ok(SendMessageW(hwnd, BCM_GETIDEALSIZE, 0, (LPARAM)&plain), "BCM_GETIDEALSIZE failed\n");

    ret = SendMessageW(hwnd, BCM_SETSHIELD, 0, TRUE);
    ok(ret == 1, "BCM_SETSHIELD(TRUE) returned %Id\n", ret);
    ok(SendMessageW(hwnd, BCM_GETIDEALSIZE, 0, (LPARAM)&shielded), "BCM_GETIDEALSIZE failed\n");
    ok(shielded.cx > plain.cx, "shield did not widen the button: %ld -> %ld\n", plain.cx, shielded.cx);

    ret = SendMessageW(hwnd, BCM_SETSHIELD, 0, TRUE);
    ok(ret == 1, "second BCM_SETSHIELD(TRUE) returned %Id\n", ret);

    ret = SendMessageW(hwnd, BCM_SETSHIELD, 0, FALSE);
    ok(ret == 1, "BCM_SETSHIELD(FALSE) returned %Id\n", ret);
    ok(SendMessageW(hwnd, BCM_GETIDEALSIZE, 0, (LPARAM)&shielded), "BCM_GETIDEALSIZE failed\n");
    ok(shielded.cx == plain.cx, "width after clearing the shield: %ld, expected %ld\n", shielded.cx, plain.cx);

    ok(Button_SetElevationRequiredState(hwnd, TRUE) == 1, "Button_SetElevationRequiredState failed\n");

    DestroyWindow(hwnd);
}
