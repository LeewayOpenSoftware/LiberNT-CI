/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The theme data a process sees must not change when the theme hook loads into it
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windows.h>
#include <uxtheme.h>
#include <vssym32.h>

typedef struct _THEME_SNAPSHOT
{
    BOOL Active;
    WCHAR Name[MAX_PATH];
    WCHAR Glyph[MAX_PATH];
    WCHAR Image1[MAX_PATH];
    int Select;
    int Layout;
    int Count;
    MARGINS Caption;
} THEME_SNAPSHOT;

static int
QueryEnumValue(HTHEME Theme, int Part, int State, int Property)
{
    int Value = -1;

    if (FAILED(GetThemeEnumValue(Theme, Part, State, Property, &Value)))
        return -1;
    return Value;
}

static void
TakeSnapshot(const char *Phase, THEME_SNAPSHOT *Shot)
{
    HTHEME Theme;

    ZeroMemory(Shot, sizeof(*Shot));
    Shot->Select = Shot->Layout = Shot->Count = -1;
    Shot->Active = IsThemeActive();
    GetCurrentThemeName(Shot->Name, ARRAYSIZE(Shot->Name), NULL, 0, NULL, 0);

    /* The scroll bar arrow carries the three image properties that went missing (TS_TRUE then returned the
       whole strip); a version-4 image property reads back as "#<id>", a version-3 one as a path */
    Theme = OpenThemeData(NULL, L"SCROLLBAR");
    if (Theme)
    {
        GetThemeFilename(Theme, SBP_ARROWBTN, ABS_UPNORMAL, TMT_IMAGEFILE, Shot->Glyph, ARRAYSIZE(Shot->Glyph));
        GetThemeFilename(Theme, SBP_ARROWBTN, ABS_UPNORMAL, TMT_IMAGEFILE1, Shot->Image1, ARRAYSIZE(Shot->Image1));
        Shot->Select = QueryEnumValue(Theme, SBP_ARROWBTN, ABS_UPNORMAL, TMT_IMAGESELECTTYPE);
        Shot->Layout = QueryEnumValue(Theme, SBP_ARROWBTN, ABS_UPNORMAL, TMT_IMAGELAYOUT);
        if (FAILED(GetThemeInt(Theme, SBP_ARROWBTN, ABS_UPNORMAL, TMT_IMAGECOUNT, &Shot->Count)))
            Shot->Count = -1;
        CloseThemeData(Theme);
    }
    Theme = OpenThemeData(NULL, L"WINDOW");
    if (Theme)
    {
        GetThemeMargins(Theme, NULL, WP_CAPTION, CS_ACTIVE, TMT_SIZINGMARGINS, NULL, &Shot->Caption);
        CloseThemeData(Theme);
    }
    trace("%s: active %d theme %S arrow glyph %S image1 %S select %d layout %d count %d caption margins %d,%d,%d,%d\n",
          Phase, Shot->Active, Shot->Name, Shot->Glyph, Shot->Image1, Shot->Select, Shot->Layout, Shot->Count,
          Shot->Caption.cxLeftWidth, Shot->Caption.cxRightWidth, Shot->Caption.cyTopHeight,
          Shot->Caption.cyBottomHeight);
}

/* A top-level window and a short message loop: the first DefWindowProc is where user32 loads the theme hook into
   the process, and the hook reloads the theme (UXTHEME_LoadThemeIfChanged) */
static void
LoadThemeHook(void)
{
    WNDCLASSW Class = { 0 };
    DWORD Start;
    MSG Msg;
    HWND Window;

    Class.lpfnWndProc = DefWindowProcW;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.lpszClassName = L"ThemeReloadTest";
    RegisterClassW(&Class);
    Window = CreateWindowExW(0, Class.lpszClassName, L"ThemeReload", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             10, 10, 200, 120, NULL, NULL, Class.hInstance, NULL);
    ok(Window != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    Start = GetTickCount();
    while (GetTickCount() - Start < 2000)
    {
        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&Msg);
        Sleep(20);
    }
    if (Window)
        DestroyWindow(Window);
    UnregisterClassW(Class.lpszClassName, Class.hInstance);
}

/* uxtheme opens the ContentLight:: classes instead of the plain ones while the default system colours are in use
   (UXTHEME_UseLightClasses), so which set a process gets depends on when it opens the theme. Whatever the plain
   class resolves for these parts must resolve through the light class too (the values may differ). */
static void
TestLightClassesMatchPlain(void)
{
    static const struct { LPCWSTR Plain, Light; int Part, State; } Parts[] =
    {
        { L"SCROLLBAR", L"ContentLight::SCROLLBAR", SBP_ARROWBTN, ABS_UPNORMAL },
        { L"WINDOW", L"ContentLight::WINDOW", WP_CAPTION, CS_ACTIVE },
    };
    static const int Enums[] = { TMT_IMAGELAYOUT, TMT_IMAGESELECTTYPE, TMT_BGTYPE, TMT_SIZINGTYPE };
    UINT i, j;

    for (i = 0; i < ARRAYSIZE(Parts); ++i)
    {
        HTHEME Plain = OpenThemeData(NULL, Parts[i].Plain), Light = OpenThemeData(NULL, Parts[i].Light);
        MARGINS PlainMargins, LightMargins;
        HRESULT PlainHr, LightHr;
        int PlainValue, LightValue;

        if (!Plain || !Light)
        {
            skip("%S or %S not available\n", Parts[i].Plain, Parts[i].Light);
            if (Plain) CloseThemeData(Plain);
            if (Light) CloseThemeData(Light);
            continue;
        }
        for (j = 0; j < ARRAYSIZE(Enums); ++j)
        {
            PlainValue = LightValue = -1;
            PlainHr = GetThemeEnumValue(Plain, Parts[i].Part, Parts[i].State, Enums[j], &PlainValue);
            LightHr = GetThemeEnumValue(Light, Parts[i].Part, Parts[i].State, Enums[j], &LightValue);
            trace("%S part %d enum %d: plain 0x%08lx %d light 0x%08lx %d\n", Parts[i].Plain, Parts[i].Part, Enums[j],
                  (unsigned long)PlainHr, PlainValue, (unsigned long)LightHr, LightValue);
            ok(FAILED(PlainHr) || SUCCEEDED(LightHr), "%S part %d enum %d: plain %d, light 0x%08lx\n",
               Parts[i].Plain, Parts[i].Part, Enums[j], PlainValue, (unsigned long)LightHr);
        }
        ZeroMemory(&PlainMargins, sizeof(PlainMargins));
        ZeroMemory(&LightMargins, sizeof(LightMargins));
        PlainHr = GetThemeMargins(Plain, NULL, Parts[i].Part, Parts[i].State, TMT_SIZINGMARGINS, NULL, &PlainMargins);
        LightHr = GetThemeMargins(Light, NULL, Parts[i].Part, Parts[i].State, TMT_SIZINGMARGINS, NULL, &LightMargins);
        trace("%S part %d sizing margins: plain 0x%08lx %d,%d,%d,%d light 0x%08lx %d,%d,%d,%d\n", Parts[i].Plain,
              Parts[i].Part, (unsigned long)PlainHr, PlainMargins.cxLeftWidth, PlainMargins.cxRightWidth,
              PlainMargins.cyTopHeight, PlainMargins.cyBottomHeight, (unsigned long)LightHr, LightMargins.cxLeftWidth,
              LightMargins.cxRightWidth, LightMargins.cyTopHeight, LightMargins.cyBottomHeight);
        ok(FAILED(PlainHr) || SUCCEEDED(LightHr), "%S part %d: sizing margins 0x%08lx through the light class\n",
           Parts[i].Plain, Parts[i].Part, (unsigned long)LightHr);
        if (Parts[i].Part == SBP_ARROWBTN && !lstrcmpW(Parts[i].Plain, L"SCROLLBAR"))
        {
            SIZE True = { -1, -1 };

            LightHr = GetThemePartSize(Light, NULL, Parts[i].Part, Parts[i].State, NULL, TS_TRUE, &True);
            /* one frame of the strip, never the whole strip or a zero-width slice */
            ok(SUCCEEDED(LightHr) && True.cx > 0 && True.cy > 0 && True.cy < 4 * True.cx,
               "light arrow TS_TRUE 0x%08lx %ldx%ld\n", (unsigned long)LightHr, True.cx, True.cy);
        }
        CloseThemeData(Plain);
        CloseThemeData(Light);
    }
}

START_TEST(ThemeReload)
{
    THEME_SNAPSHOT Before, After;

    TestLightClassesMatchPlain();

    TakeSnapshot("before the hook", &Before);
    if (!Before.Active)
    {
        skip("no visual style is active\n");
        return;
    }
    LoadThemeHook();
    TakeSnapshot("after the hook", &After);

    ok(!lstrcmpiW(Before.Name, After.Name), "theme file changed: %S -> %S\n", Before.Name, After.Name);
    ok(!lstrcmpiW(Before.Glyph, After.Glyph) && !lstrcmpiW(Before.Image1, After.Image1),
       "arrow images changed: %S/%S -> %S/%S\n", Before.Glyph, Before.Image1, After.Glyph, After.Image1);
    ok(Before.Select == After.Select && Before.Layout == After.Layout && Before.Count == After.Count,
       "arrow image properties changed: select %d layout %d count %d -> select %d layout %d count %d\n",
       Before.Select, Before.Layout, Before.Count, After.Select, After.Layout, After.Count);
    ok(!memcmp(&Before.Caption, &After.Caption, sizeof(MARGINS)),
       "caption sizing margins changed: %d,%d,%d,%d -> %d,%d,%d,%d\n",
       Before.Caption.cxLeftWidth, Before.Caption.cxRightWidth, Before.Caption.cyTopHeight,
       Before.Caption.cyBottomHeight, After.Caption.cxLeftWidth, After.Caption.cxRightWidth,
       After.Caption.cyTopHeight, After.Caption.cyBottomHeight);
}
