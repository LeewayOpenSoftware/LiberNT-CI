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

    /* The shield and a caller image list must coexist: the shield is shown, but the
     * caller's list is parked (not lost) and is what BCM_GETIMAGELIST keeps reporting. */
    {
        HIMAGELIST himl;
        BUTTON_IMAGELIST bil = { 0 }, empty = { 0 }, q = { 0 };
        HWND hwnd2;

        himl = ImageList_Create(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                ILC_COLOR32 | ILC_MASK, 1, 0);
        ok(himl != NULL, "ImageList_Create failed\n");
        ImageList_AddIcon(himl, LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION));

        hwnd2 = CreateWindowExW(0, WC_BUTTONW, L"Install", WS_POPUP | BS_PUSHBUTTON,
                                0, 0, 120, 30, NULL, NULL, NULL, NULL);
        ok(hwnd2 != NULL, "CreateWindowEx failed, error %lu\n", GetLastError());

        bil.himl = himl;
        bil.uAlign = BUTTON_IMAGELIST_ALIGN_LEFT;
        ok(SendMessageW(hwnd2, BCM_SETIMAGELIST, 0, (LPARAM)&bil), "BCM_SETIMAGELIST failed\n");

        /* With a caller list and then the shield, GETIMAGELIST still returns the caller list. */
        ret = SendMessageW(hwnd2, BCM_SETSHIELD, 0, TRUE);
        ok(ret == 1, "BCM_SETSHIELD(TRUE) with a list returned %Id\n", ret);
        memset(&q, 0, sizeof(q));
        ok(SendMessageW(hwnd2, BCM_GETIMAGELIST, 0, (LPARAM)&q), "BCM_GETIMAGELIST failed\n");
        ok(q.himl == himl, "GETIMAGELIST returned %p, expected the caller list %p (shield leaked)\n",
           q.himl, himl);

        /* Clearing the shield restores the caller list. */
        ret = SendMessageW(hwnd2, BCM_SETSHIELD, 0, FALSE);
        ok(ret == 1, "BCM_SETSHIELD(FALSE) with a list returned %Id\n", ret);
        memset(&q, 0, sizeof(q));
        ok(SendMessageW(hwnd2, BCM_GETIMAGELIST, 0, (LPARAM)&q), "BCM_GETIMAGELIST failed\n");
        ok(q.himl == himl, "caller list not restored after clearing the shield: %p\n", q.himl);

        /* A shield on a listless button must not surface as a caller list. */
        ok(SendMessageW(hwnd2, BCM_SETIMAGELIST, 0, (LPARAM)&empty), "BCM_SETIMAGELIST(clear) failed\n");
        ret = SendMessageW(hwnd2, BCM_SETSHIELD, 0, TRUE);
        ok(ret == 1, "BCM_SETSHIELD(TRUE) no list returned %Id\n", ret);
        memset(&q, 0, sizeof(q));
        SendMessageW(hwnd2, BCM_GETIMAGELIST, 0, (LPARAM)&q);
        ok(q.himl == NULL, "shield surfaced via GETIMAGELIST on a listless button: %p\n", q.himl);

        DestroyWindow(hwnd2);
        ImageList_Destroy(himl);
    }
}
