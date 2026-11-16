/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     GetThemePartSize TS_DRAW contract checks against the Windows 11 wave CSVs
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windows.h>
#include <uxtheme.h>
#include <vssym32.h>

static void TestBorderFillReject(void)
{
    HTHEME theme = OpenThemeData(NULL, L"EDIT");
    int bgtype = -1;
    HRESULT hr;
    SIZE sz;

    if (!theme)
    {
        skip("EDIT theme class not available\n");
        return;
    }
    if (FAILED(GetThemeEnumValue(theme, EP_EDITTEXT, ETS_NORMAL, TMT_BGTYPE, &bgtype)) || bgtype != BT_BORDERFILL)
    {
        skip("EDIT part 1 is not border-fill in the active theme (bgtype %d)\n", bgtype);
        CloseThemeData(theme);
        return;
    }

    hr = GetThemePartSize(theme, NULL, EP_EDITTEXT, ETS_NORMAL, NULL, TS_MIN, &sz);
    ok(hr == S_OK, "TS_MIN failed hr 0x%08lx\n", (unsigned long)hr);
    hr = GetThemePartSize(theme, NULL, EP_EDITTEXT, ETS_NORMAL, NULL, TS_TRUE, &sz);
    ok(hr == S_OK, "TS_TRUE failed hr 0x%08lx\n", (unsigned long)hr);
    hr = GetThemePartSize(theme, NULL, EP_EDITTEXT, ETS_NORMAL, NULL, TS_DRAW, &sz);
    ok(hr == E_INVALIDARG, "TS_DRAW border-fill expected E_INVALIDARG, got 0x%08lx\n", (unsigned long)hr);

    CloseThemeData(theme);
}

static void TestStretchDrawEchoesRect(void)
{
    HTHEME theme = OpenThemeData(NULL, L"BUTTON");
    int bgtype = -1, sizingtype = -1;
    SIZE sz;
    RECT rc = { 0, 0, 120, 40 };

    if (!theme)
    {
        skip("BUTTON theme class not available\n");
        return;
    }
    if (FAILED(GetThemeEnumValue(theme, BP_PUSHBUTTON, PBS_NORMAL, TMT_BGTYPE, &bgtype)) || bgtype != BT_IMAGEFILE ||
        FAILED(GetThemeEnumValue(theme, BP_PUSHBUTTON, PBS_NORMAL, TMT_SIZINGTYPE, &sizingtype)) || sizingtype != ST_STRETCH)
    {
        skip("BUTTON push button is not a stretched image part (bgtype %d sizingtype %d)\n", bgtype, sizingtype);
        CloseThemeData(theme);
        return;
    }

    /* Stretched parts are drawn into the whole rect: native uxtheme reports
       the rect size, never a uniform-scaled fit (wave CSVs, Window caption
       buttons included). */
    ok(GetThemePartSize(theme, NULL, BP_PUSHBUTTON, PBS_NORMAL, &rc, TS_DRAW, &sz) == S_OK,
       "TS_DRAW failed\n");
    ok(sz.cx == 120 && sz.cy == 40, "TS_DRAW stretch expected 120x40, got %ldx%ld\n", sz.cx, sz.cy);

    rc.right = 28;
    rc.bottom = 20;
    ok(GetThemePartSize(theme, NULL, BP_PUSHBUTTON, PBS_NORMAL, &rc, TS_DRAW, &sz) == S_OK,
       "TS_DRAW failed\n");
    ok(sz.cx == 28 && sz.cy == 20, "TS_DRAW stretch expected 28x20, got %ldx%ld\n", sz.cx, sz.cy);

    CloseThemeData(theme);
}

START_TEST(PartSize)
{
    TestBorderFillReject();
    TestStretchDrawEchoesRect();
}
