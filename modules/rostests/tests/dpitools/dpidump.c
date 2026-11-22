/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Dump the DPI state of a session as comparable text (Orosi and Windows 11 reference)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <shlobj.h>
#include <shlwapi.h>

/* Everything newer than the Vista SDK is loaded by name so one source builds on both toolchains */
typedef HANDLE DPICTX;
typedef BOOL (WINAPI *PFN_SETPROCESSCTX)(DPICTX);
typedef DPICTX (WINAPI *PFN_GETTHREADCTX)(VOID);
typedef DPICTX (WINAPI *PFN_SETTHREADCTX)(DPICTX);
typedef INT (WINAPI *PFN_GETAWARENESS)(DPICTX);
typedef UINT (WINAPI *PFN_GETDPIFORSYSTEM)(VOID);
typedef UINT (WINAPI *PFN_GETDPIFORWINDOW)(HWND);
typedef BOOL (WINAPI *PFN_GETDPIFORMONITORINTERNAL)(HMONITOR, UINT, UINT *, UINT *);
typedef HRESULT (WINAPI *PFN_GETDPIFORMONITOR)(HMONITOR, UINT, UINT *, UINT *);
typedef HRESULT (WINAPI *PFN_GETSCALEFACTOR)(HMONITOR, INT *);
typedef INT (WINAPI *PFN_GETMETRICSFORDPI)(INT, UINT);
typedef BOOL (WINAPI *PFN_SPIFORDPI)(UINT, UINT, PVOID, UINT, UINT);
typedef BOOL (WINAPI *PFN_ADJUSTFORDPI)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef HTHEME (WINAPI *PFN_OPENTHEMEFORDPI)(HWND, LPCWSTR, UINT);
typedef BOOL (WINAPI *PFN_ENABLENCDPI)(HWND);
typedef HRESULT (WINAPI *PFN_DWMISCOMPOSITIONENABLED)(BOOL *);
typedef LONG (WINAPI *PFN_RTLGETVERSION)(PRTL_OSVERSIONINFOW);

#define CTX_UNAWARE ((DPICTX)-1)
#define CTX_SYSTEM ((DPICTX)-2)
#define CTX_PMV1 ((DPICTX)-3)
#define CTX_PMV2 ((DPICTX)-4)
#define CTX_GDISCALED ((DPICTX)-5)

static const UINT Dpis[] = { 96, 120, 144, 168, 192, 240, 288 };
#define DPI_COUNT (sizeof(Dpis) / sizeof(Dpis[0]))

static struct
{
    PFN_SETPROCESSCTX SetProcessCtx;
    PFN_GETTHREADCTX GetThreadCtx;
    PFN_SETTHREADCTX SetThreadCtx;
    PFN_GETAWARENESS GetAwareness;
    PFN_GETDPIFORSYSTEM GetDpiForSystem;
    PFN_GETDPIFORWINDOW GetDpiForWindow;
    PFN_GETDPIFORMONITORINTERNAL GetDpiForMonitorInternal;
    PFN_GETDPIFORMONITOR GetDpiForMonitor;
    PFN_GETSCALEFACTOR GetScaleFactorForMonitor;
    PFN_GETMETRICSFORDPI GetMetricsForDpi;
    PFN_SPIFORDPI SpiForDpi;
    PFN_ADJUSTFORDPI AdjustForDpi;
    PFN_OPENTHEMEFORDPI OpenThemeForDpi;
    PFN_ENABLENCDPI EnableNcDpi;
    PFN_DWMISCOMPOSITIONENABLED DwmIsCompositionEnabled;
} Api;

static const char *g_Variant = "unaware";
static HWND g_Window;

static void
Row(const char *Name, const INT *Values)
{
    UINT i;

    printf("%s", Name);
    for (i = 0; i < DPI_COUNT; i++)
        printf(" %d", Values[i]);
    printf("\n");
}

static void
LoadApis(void)
{
    HMODULE User32 = GetModuleHandleW(L"user32.dll");
    HMODULE ShCore = LoadLibraryW(L"shcore.dll");
    HMODULE UxTheme = LoadLibraryW(L"uxtheme.dll");
    HMODULE DwmApi = LoadLibraryW(L"dwmapi.dll");

#define L(Mod, Field, Name) *(FARPROC *)&Api.Field = (Mod) ? GetProcAddress(Mod, Name) : NULL
    L(User32, SetProcessCtx, "SetProcessDpiAwarenessContext");
    L(User32, GetThreadCtx, "GetThreadDpiAwarenessContext");
    L(User32, SetThreadCtx, "SetThreadDpiAwarenessContext");
    L(User32, GetAwareness, "GetAwarenessFromDpiAwarenessContext");
    L(User32, GetDpiForSystem, "GetDpiForSystem");
    L(User32, GetDpiForWindow, "GetDpiForWindow");
    L(User32, GetDpiForMonitorInternal, "GetDpiForMonitorInternal");
    L(User32, GetMetricsForDpi, "GetSystemMetricsForDpi");
    L(User32, SpiForDpi, "SystemParametersInfoForDpi");
    L(User32, AdjustForDpi, "AdjustWindowRectExForDpi");
    L(User32, EnableNcDpi, "EnableNonClientDpiScaling");
    L(ShCore, GetDpiForMonitor, "GetDpiForMonitor");
    L(ShCore, GetScaleFactorForMonitor, "GetScaleFactorForMonitor");
    L(UxTheme, OpenThemeForDpi, "OpenThemeDataForDpi");
    L(DwmApi, DwmIsCompositionEnabled, "DwmIsCompositionEnabled");
#undef L
}

static void
DumpVersion(void)
{
    RTL_OSVERSIONINFOW Version;
    PFN_RTLGETVERSION RtlGetVersionPtr;
    HMODULE NtDll = GetModuleHandleW(L"ntdll.dll");
    BOOL Composition = FALSE;
    HRESULT hr = E_NOTIMPL;
    WCHAR Theme[MAX_PATH] = L"", Color[64] = L"", Size[64] = L"";

    ZeroMemory(&Version, sizeof(Version));
    Version.dwOSVersionInfoSize = sizeof(Version);
    RtlGetVersionPtr = NtDll ? (PFN_RTLGETVERSION)GetProcAddress(NtDll, "RtlGetVersion") : NULL;
    if (RtlGetVersionPtr)
        RtlGetVersionPtr(&Version);
    printf("VERSION major=%lu minor=%lu build=%lu csd=\"%ls\"\n", Version.dwMajorVersion, Version.dwMinorVersion,
           Version.dwBuildNumber, Version.szCSDVersion);
    if (Api.DwmIsCompositionEnabled)
        hr = Api.DwmIsCompositionEnabled(&Composition);
    printf("DWM composition=%d hr=0x%08lx\n", Composition, (ULONG)hr);
    GetCurrentThemeName(Theme, MAX_PATH, Color, 64, Size, 64);
    printf("THEME active=%d app_themed=%d file=\"%ls\" color=\"%ls\" size=\"%ls\"\n", IsThemeActive(), IsAppThemed(),
           Theme, Color, Size);
}

static void
RegDword(HKEY Root, const char *RootName, const WCHAR *Path, const WCHAR *Value)
{
    DWORD Data = 0, Size = sizeof(Data), Type = 0;
    HKEY Key;
    LONG Error;

    Error = RegOpenKeyExW(Root, Path, 0, KEY_READ, &Key);
    if (Error == ERROR_SUCCESS)
    {
        Error = RegQueryValueExW(Key, Value, NULL, &Type, (BYTE *)&Data, &Size);
        RegCloseKey(Key);
    }
    if (Error == ERROR_SUCCESS)
        printf("REG %s\\%ls\\%ls type=%lu value=%lu\n", RootName, Path, Value, Type, Data);
    else
        printf("REG %s\\%ls\\%ls error=%ld\n", RootName, Path, Value, Error);
}

static void
DumpRegistry(void)
{
    HKEY Key;
    WCHAR Name[256];
    DWORD Index, Length;

    RegDword(HKEY_LOCAL_MACHINE, "HKLM", L"SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current\\Software\\Fonts",
             L"LogPixels");
    RegDword(HKEY_LOCAL_MACHINE, "HKLM", L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontDPI", L"LogPixels");
    RegDword(HKEY_CURRENT_USER, "HKCU", L"Control Panel\\Desktop\\WindowMetrics", L"AppliedDPI");
    RegDword(HKEY_CURRENT_USER, "HKCU", L"Control Panel\\Desktop", L"LogPixels");
    RegDword(HKEY_CURRENT_USER, "HKCU", L"Control Panel\\Desktop", L"Win8DpiScaling");
    RegDword(HKEY_CURRENT_USER, "HKCU", L"Control Panel\\Desktop", L"DpiScalingVer");
    RegDword(HKEY_CURRENT_USER, "HKCU", L"Control Panel\\Desktop", L"EnablePerProcessSystemDPI");
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop\\PerMonitorSettings", 0, KEY_READ, &Key) ==
        ERROR_SUCCESS)
    {
        for (Index = 0;; Index++)
        {
            WCHAR Path[512];

            Length = 256;
            if (RegEnumKeyExW(Key, Index, Name, &Length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            wsprintfW(Path, L"Control Panel\\Desktop\\PerMonitorSettings\\%s", Name);
            RegDword(HKEY_CURRENT_USER, "HKCU", Path, L"DpiValue");
        }
        RegCloseKey(Key);
    }
    else
    {
        printf("REG HKCU\\Control Panel\\Desktop\\PerMonitorSettings absent\n");
    }
}

static void
DumpContext(void)
{
    DPICTX Context = NULL;
    HMONITOR Monitor;
    UINT Type, X, Y;
    INT Scale = 0;
    HRESULT hr;

    if (Api.GetThreadCtx)
        Context = Api.GetThreadCtx();
    printf("CONTEXT value=%p awareness=%d process_aware=%d\n", Context, Api.GetAwareness ? Api.GetAwareness(Context) : -2,
           IsProcessDPIAware());
    printf("DPI system=%u window=%u\n", Api.GetDpiForSystem ? Api.GetDpiForSystem() : 0,
           Api.GetDpiForWindow ? Api.GetDpiForWindow(g_Window) : 0);
    Monitor = MonitorFromWindow(g_Window, MONITOR_DEFAULTTOPRIMARY);
    for (Type = 0; Type <= 2; Type++)
    {
        X = Y = 0;
        if (Api.GetDpiForMonitor)
        {
            hr = Api.GetDpiForMonitor(Monitor, Type, &X, &Y);
            printf("DPI shcore_monitor type=%u hr=0x%08lx x=%u y=%u\n", Type, (ULONG)hr, X, Y);
        }
        X = Y = 0;
        if (Api.GetDpiForMonitorInternal)
        {
            SetLastError(0);
            if (Api.GetDpiForMonitorInternal(Monitor, Type, &X, &Y))
                printf("DPI monitor_internal type=%u x=%u y=%u\n", Type, X, Y);
            else
                printf("DPI monitor_internal type=%u error=%lu\n", Type, GetLastError());
        }
    }
    if (Api.GetScaleFactorForMonitor)
    {
        hr = Api.GetScaleFactorForMonitor(Monitor, &Scale);
        printf("DPI scale_factor hr=0x%08lx percent=%d\n", (ULONG)hr, Scale);
    }
}

static void
DumpCaps(void)
{
    static const struct { int Index; const char *Name; } Caps[] =
    {
        { LOGPIXELSX, "LOGPIXELSX" }, { LOGPIXELSY, "LOGPIXELSY" }, { HORZRES, "HORZRES" }, { VERTRES, "VERTRES" },
        { DESKTOPHORZRES, "DESKTOPHORZRES" }, { DESKTOPVERTRES, "DESKTOPVERTRES" }, { HORZSIZE, "HORZSIZE" },
        { VERTSIZE, "VERTSIZE" }, { BITSPIXEL, "BITSPIXEL" },
    };
    HDC Dc = GetDC(NULL);
    UINT i;

    for (i = 0; i < sizeof(Caps) / sizeof(Caps[0]); i++)
        printf("CAPS %s=%d\n", Caps[i].Name, Dc ? GetDeviceCaps(Dc, Caps[i].Index) : -1);
    if (Dc)
        ReleaseDC(NULL, Dc);
    printf("SCREEN cx=%d cy=%d vcx=%d vcy=%d monitors=%d\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
           GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), GetSystemMetrics(SM_CMONITORS));
}

static void
DumpMetrics(void)
{
    INT Index, Values[DPI_COUNT];
    UINT d;
    char Name[32];
    BOOL Any;

    for (Index = 0; Index < 100; Index++)
    {
        INT Current = GetSystemMetrics(Index);

        Any = Current != 0;
        for (d = 0; d < DPI_COUNT; d++)
        {
            Values[d] = Api.GetMetricsForDpi ? Api.GetMetricsForDpi(Index, Dpis[d]) : 0;
            Any |= Values[d] != 0;
        }
        if (!Any)
            continue;
        sprintf(Name, "METRIC %d cur=%d fordpi", Index, Current);
        Row(Name, Values);
    }
}

static void
LogFontRow(const char *Name, const LOGFONTW *Font)
{
    printf("%s height=%ld width=%ld weight=%ld quality=%u face=\"%ls\"\n", Name, Font->lfHeight, Font->lfWidth,
           Font->lfWeight, Font->lfQuality, Font->lfFaceName);
}

static void
TextMetricRow(const char *Name, const LOGFONTW *Font)
{
    HDC Dc = GetDC(NULL);
    HFONT hFont = CreateFontIndirectW(Font), Old;
    TEXTMETRICW Tm;

    ZeroMemory(&Tm, sizeof(Tm));
    if (Dc && hFont)
    {
        Old = SelectObject(Dc, hFont);
        GetTextMetricsW(Dc, &Tm);
        SelectObject(Dc, Old);
    }
    printf("%s tm_height=%ld ascent=%ld descent=%ld internal=%ld external=%ld avg=%ld\n", Name, Tm.tmHeight,
           Tm.tmAscent, Tm.tmDescent, Tm.tmInternalLeading, Tm.tmExternalLeading, Tm.tmAveCharWidth);
    if (hFont)
        DeleteObject(hFont);
    if (Dc)
        ReleaseDC(NULL, Dc);
}

static void
DumpNonClient(void)
{
    NONCLIENTMETRICSW Ncm;
    ICONMETRICSW Im;
    LOGFONTW Title;
    MINIMIZEDMETRICS Mm;
    UINT d;
    INT Border[DPI_COUNT], ScrollW[DPI_COUNT], ScrollH[DPI_COUNT], CapW[DPI_COUNT], CapH[DPI_COUNT];
    INT SmCapW[DPI_COUNT], SmCapH[DPI_COUNT], MenuW[DPI_COUNT], MenuH[DPI_COUNT], Padded[DPI_COUNT];
    INT CapFont[DPI_COUNT], SmCapFont[DPI_COUNT], MenuFont[DPI_COUNT], StatusFont[DPI_COUNT], MsgFont[DPI_COUNT];
    INT HorzSp[DPI_COUNT], VertSp[DPI_COUNT], IconFont[DPI_COUNT], TitleFont[DPI_COUNT];

    ZeroMemory(&Ncm, sizeof(Ncm));
    Ncm.cbSize = sizeof(Ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(Ncm), &Ncm, 0))
    {
        printf("NCM border=%d padded=%d scroll=%dx%d caption=%dx%d smcaption=%dx%d menu=%dx%d\n", Ncm.iBorderWidth,
               Ncm.iPaddedBorderWidth, Ncm.iScrollWidth, Ncm.iScrollHeight, Ncm.iCaptionWidth, Ncm.iCaptionHeight,
               Ncm.iSmCaptionWidth, Ncm.iSmCaptionHeight, Ncm.iMenuWidth, Ncm.iMenuHeight);
        LogFontRow("NCM_FONT caption", &Ncm.lfCaptionFont);
        LogFontRow("NCM_FONT smcaption", &Ncm.lfSmCaptionFont);
        LogFontRow("NCM_FONT menu", &Ncm.lfMenuFont);
        LogFontRow("NCM_FONT status", &Ncm.lfStatusFont);
        LogFontRow("NCM_FONT message", &Ncm.lfMessageFont);
        TextMetricRow("NCM_TM message", &Ncm.lfMessageFont);
        TextMetricRow("NCM_TM caption", &Ncm.lfCaptionFont);
        TextMetricRow("NCM_TM menu", &Ncm.lfMenuFont);
    }
    else
    {
        printf("NCM error=%lu\n", GetLastError());
    }
    ZeroMemory(&Title, sizeof(Title));
    if (SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(Title), &Title, 0))
        LogFontRow("NCM_FONT icontitle", &Title);
    ZeroMemory(&Mm, sizeof(Mm));
    Mm.cbSize = sizeof(Mm);
    if (SystemParametersInfoW(SPI_GETMINIMIZEDMETRICS, sizeof(Mm), &Mm, 0))
        printf("MINIMIZED width=%d hgap=%d vgap=%d arrange=%d\n", Mm.iWidth, Mm.iHorzGap, Mm.iVertGap, Mm.iArrange);

    if (!Api.SpiForDpi)
    {
        printf("NCM_FOR_DPI unsupported\n");
        return;
    }
    for (d = 0; d < DPI_COUNT; d++)
    {
        ZeroMemory(&Ncm, sizeof(Ncm));
        Ncm.cbSize = sizeof(Ncm);
        Api.SpiForDpi(SPI_GETNONCLIENTMETRICS, sizeof(Ncm), &Ncm, 0, Dpis[d]);
        Border[d] = Ncm.iBorderWidth; ScrollW[d] = Ncm.iScrollWidth; ScrollH[d] = Ncm.iScrollHeight;
        CapW[d] = Ncm.iCaptionWidth; CapH[d] = Ncm.iCaptionHeight; SmCapW[d] = Ncm.iSmCaptionWidth;
        SmCapH[d] = Ncm.iSmCaptionHeight; MenuW[d] = Ncm.iMenuWidth; MenuH[d] = Ncm.iMenuHeight;
        Padded[d] = Ncm.iPaddedBorderWidth; CapFont[d] = Ncm.lfCaptionFont.lfHeight;
        SmCapFont[d] = Ncm.lfSmCaptionFont.lfHeight; MenuFont[d] = Ncm.lfMenuFont.lfHeight;
        StatusFont[d] = Ncm.lfStatusFont.lfHeight; MsgFont[d] = Ncm.lfMessageFont.lfHeight;
        ZeroMemory(&Im, sizeof(Im));
        Im.cbSize = sizeof(Im);
        Api.SpiForDpi(SPI_GETICONMETRICS, sizeof(Im), &Im, 0, Dpis[d]);
        HorzSp[d] = Im.iHorzSpacing; VertSp[d] = Im.iVertSpacing; IconFont[d] = Im.lfFont.lfHeight;
        ZeroMemory(&Title, sizeof(Title));
        Api.SpiForDpi(SPI_GETICONTITLELOGFONT, sizeof(Title), &Title, 0, Dpis[d]);
        TitleFont[d] = Title.lfHeight;
    }
    Row("NCM_FOR_DPI border", Border); Row("NCM_FOR_DPI padded", Padded);
    Row("NCM_FOR_DPI scroll_width", ScrollW); Row("NCM_FOR_DPI scroll_height", ScrollH);
    Row("NCM_FOR_DPI caption_width", CapW); Row("NCM_FOR_DPI caption_height", CapH);
    Row("NCM_FOR_DPI smcaption_width", SmCapW); Row("NCM_FOR_DPI smcaption_height", SmCapH);
    Row("NCM_FOR_DPI menu_width", MenuW); Row("NCM_FOR_DPI menu_height", MenuH);
    Row("NCM_FOR_DPI caption_font", CapFont); Row("NCM_FOR_DPI smcaption_font", SmCapFont);
    Row("NCM_FOR_DPI menu_font", MenuFont); Row("NCM_FOR_DPI status_font", StatusFont);
    Row("NCM_FOR_DPI message_font", MsgFont); Row("NCM_FOR_DPI icon_hspacing", HorzSp);
    Row("NCM_FOR_DPI icon_vspacing", VertSp); Row("NCM_FOR_DPI icon_font", IconFont);
    Row("NCM_FOR_DPI icontitle_font", TitleFont);
}

static void
DumpStockFonts(void)
{
    static const struct { int Id; const char *Name; } Fonts[] =
    {
        { SYSTEM_FONT, "SYSTEM_FONT" }, { DEFAULT_GUI_FONT, "DEFAULT_GUI_FONT" },
        { SYSTEM_FIXED_FONT, "SYSTEM_FIXED_FONT" }, { OEM_FIXED_FONT, "OEM_FIXED_FONT" },
        { ANSI_VAR_FONT, "ANSI_VAR_FONT" }, { ANSI_FIXED_FONT, "ANSI_FIXED_FONT" },
        { DEVICE_DEFAULT_FONT, "DEVICE_DEFAULT_FONT" },
    };
    UINT i;
    LOGFONTW Font;
    char Name[64];

    for (i = 0; i < sizeof(Fonts) / sizeof(Fonts[0]); i++)
    {
        ZeroMemory(&Font, sizeof(Font));
        GetObjectW(GetStockObject(Fonts[i].Id), sizeof(Font), &Font);
        sprintf(Name, "STOCK %s", Fonts[i].Name);
        LogFontRow(Name, &Font);
        sprintf(Name, "STOCK_TM %s", Fonts[i].Name);
        TextMetricRow(Name, &Font);
    }
}

static void
DumpFrames(void)
{
    static const struct { const char *Name; DWORD Style; DWORD ExStyle; BOOL Menu; } Frames[] =
    {
        { "overlapped", WS_OVERLAPPEDWINDOW, 0, FALSE }, { "overlapped_menu", WS_OVERLAPPEDWINDOW, 0, TRUE },
        { "caption", WS_CAPTION | WS_SYSMENU, 0, FALSE },
        { "dialog", WS_POPUP | WS_CAPTION | WS_SYSMENU, WS_EX_DLGMODALFRAME, FALSE },
        { "toolwindow", WS_OVERLAPPEDWINDOW, WS_EX_TOOLWINDOW, FALSE }, { "popup_border", WS_POPUP | WS_BORDER, 0, FALSE },
        { "popup_thick", WS_POPUP | WS_THICKFRAME, 0, FALSE }, { "child_clientedge", WS_CHILD, WS_EX_CLIENTEDGE, FALSE },
        { "child_staticedge", WS_CHILD, WS_EX_STATICEDGE, FALSE },
    };
    UINT f, d;
    INT Left[DPI_COUNT], Top[DPI_COUNT], Right[DPI_COUNT], Bottom[DPI_COUNT];
    RECT Rect;
    char Name[64];

    SetRect(&Rect, 0, 0, 100, 100);
    AdjustWindowRectEx(&Rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    printf("FRAME_CURRENT overlapped left=%ld top=%ld right=%ld bottom=%ld\n", -Rect.left, -Rect.top, Rect.right - 100,
           Rect.bottom - 100);
    if (!Api.AdjustForDpi)
        return;
    for (f = 0; f < sizeof(Frames) / sizeof(Frames[0]); f++)
    {
        for (d = 0; d < DPI_COUNT; d++)
        {
            SetRect(&Rect, 0, 0, 100, 100);
            Api.AdjustForDpi(&Rect, Frames[f].Style, Frames[f].Menu, Frames[f].ExStyle, Dpis[d]);
            Left[d] = -Rect.left; Top[d] = -Rect.top; Right[d] = Rect.right - 100; Bottom[d] = Rect.bottom - 100;
        }
        sprintf(Name, "FRAME_FOR_DPI %s left", Frames[f].Name); Row(Name, Left);
        sprintf(Name, "FRAME_FOR_DPI %s top", Frames[f].Name); Row(Name, Top);
        sprintf(Name, "FRAME_FOR_DPI %s right", Frames[f].Name); Row(Name, Right);
        sprintf(Name, "FRAME_FOR_DPI %s bottom", Frames[f].Name); Row(Name, Bottom);
    }
}

static void
ThemePartRow(HTHEME Theme, const char *Class, const char *Part, int PartId, int StateId)
{
    SIZE True = { 0, 0 }, Draw = { 0, 0 };
    HRESULT hr1, hr2;

    hr1 = GetThemePartSize(Theme, NULL, PartId, StateId, NULL, TS_TRUE, &True);
    hr2 = GetThemePartSize(Theme, NULL, PartId, StateId, NULL, TS_DRAW, &Draw);
    printf("THEME_PART %s %s true=%ldx%ld draw=%ldx%ld hr=0x%08lx/0x%08lx\n", Class, Part, True.cx, True.cy, Draw.cx,
           Draw.cy, (ULONG)hr1, (ULONG)hr2);
}

static void
DumpThemeAtDpi(UINT Dpi)
{
    static const struct { const WCHAR *Class; const char *ClassName; const char *Part; int PartId; int StateId; } Parts[] =
    {
        { L"WINDOW", "WINDOW", "caption", WP_CAPTION, CS_ACTIVE }, { L"WINDOW", "WINDOW", "close", WP_CLOSEBUTTON, CBS_NORMAL },
        { L"WINDOW", "WINDOW", "max", WP_MAXBUTTON, MAXBS_NORMAL }, { L"WINDOW", "WINDOW", "min", WP_MINBUTTON, MINBS_NORMAL },
        { L"WINDOW", "WINDOW", "smallclose", WP_SMALLCLOSEBUTTON, CBS_NORMAL },
        { L"WINDOW", "WINDOW", "frameleft", WP_FRAMELEFT, FS_ACTIVE }, { L"WINDOW", "WINDOW", "framebottom", WP_FRAMEBOTTOM, FS_ACTIVE },
        { L"BUTTON", "BUTTON", "checkbox", BP_CHECKBOX, CBS_UNCHECKEDNORMAL }, { L"BUTTON", "BUTTON", "radio", BP_RADIOBUTTON, RBS_UNCHECKEDNORMAL },
        { L"BUTTON", "BUTTON", "push", BP_PUSHBUTTON, PBS_NORMAL },
        { L"SCROLLBAR", "SCROLLBAR", "arrow", SBP_ARROWBTN, ABS_UPNORMAL }, { L"SCROLLBAR", "SCROLLBAR", "thumb", SBP_THUMBBTNVERT, SCRBS_NORMAL },
        { L"TREEVIEW", "TREEVIEW", "glyph", TVP_GLYPH, GLPS_CLOSED }, { L"COMBOBOX", "COMBOBOX", "dropdown", CP_DROPDOWNBUTTON, CBXS_NORMAL },
        { L"HEADER", "HEADER", "item", HP_HEADERITEM, HIS_NORMAL }, { L"TAB", "TAB", "item", TABP_TABITEM, TIS_NORMAL },
        { L"TRACKBAR", "TRACKBAR", "thumb", TKP_THUMB, TUS_NORMAL }, { L"TOOLBAR", "TOOLBAR", "button", TP_BUTTON, TS_NORMAL },
        { L"MENU", "MENU", "checkmark", MENU_POPUPCHECK, MC_CHECKMARKNORMAL }, { L"SPIN", "SPIN", "up", SPNP_UP, UPS_NORMAL },
    };
    UINT i;
    HTHEME Theme = NULL;
    const WCHAR *Open = NULL;
    char Class[32];

    for (i = 0; i < sizeof(Parts) / sizeof(Parts[0]); i++)
    {
        if (Open != Parts[i].Class)
        {
            if (Theme)
                CloseThemeData(Theme);
            Theme = Api.OpenThemeForDpi ? Api.OpenThemeForDpi(g_Window, Parts[i].Class, Dpi) : NULL;
            Open = Parts[i].Class;
            if (!Theme)
                printf("THEME_OPEN dpi=%u class=%s failed\n", Dpi, Parts[i].ClassName);
        }
        if (!Theme)
            continue;
        sprintf(Class, "dpi=%u %s", Dpi, Parts[i].ClassName);
        ThemePartRow(Theme, Class, Parts[i].Part, Parts[i].PartId, Parts[i].StateId);
    }
    if (Theme)
    {
        LOGFONTW Font;

        printf("THEME_SYSSIZE dpi=%u cxvscroll=%d cycaption=%d cxsize=%d cxsmsize=%d cymenusize=%d\n", Dpi,
               GetThemeSysSize(Theme, SM_CXVSCROLL), GetThemeSysSize(Theme, SM_CYCAPTION), GetThemeSysSize(Theme, SM_CXSIZE),
               GetThemeSysSize(Theme, SM_CXSMSIZE), GetThemeSysSize(Theme, SM_CYMENUSIZE));
        ZeroMemory(&Font, sizeof(Font));
        if (SUCCEEDED(GetThemeSysFont(Theme, TMT_CAPTIONFONT, &Font)))
        {
            sprintf(Class, "THEME_SYSFONT dpi=%u caption", Dpi);
            LogFontRow(Class, &Font);
        }
        ZeroMemory(&Font, sizeof(Font));
        if (SUCCEEDED(GetThemeSysFont(Theme, TMT_MSGBOXFONT, &Font)))
        {
            sprintf(Class, "THEME_SYSFONT dpi=%u msgbox", Dpi);
            LogFontRow(Class, &Font);
        }
        CloseThemeData(Theme);
    }
}

static void
DumpTheme(void)
{
    UINT d;
    HTHEME Theme;

    if (!IsThemeActive())
    {
        printf("THEME inactive\n");
        return;
    }
    Theme = OpenThemeData(g_Window, L"WINDOW");
    if (Theme)
    {
        ThemePartRow(Theme, "window_dpi WINDOW", "caption", WP_CAPTION, CS_ACTIVE);
        ThemePartRow(Theme, "window_dpi WINDOW", "close", WP_CLOSEBUTTON, CBS_NORMAL);
        CloseThemeData(Theme);
    }
    for (d = 0; d < DPI_COUNT; d++)
        DumpThemeAtDpi(Dpis[d]);
}

static void
DumpShellIcons(void)
{
    static const GUID IID_IImageList_Local =
        { 0x46EB5926, 0x582E, 0x4017, { 0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50 } };
    static const struct { int Shil; const char *Name; } Lists[] =
    {
        { 1, "SHIL_SMALL" }, { 0, "SHIL_LARGE" }, { 2, "SHIL_EXTRALARGE" }, { 4, "SHIL_JUMBO" }, { 3, "SHIL_SYSSMALL" },
    };
    UINT i;
    HKEY Key;
    WCHAR Value[32];
    DWORD Size, Type;
    HICON Icon;
    ICONINFO Info;
    BITMAP Bitmap;
    static const int Sizes[] = { 16, 20, 24, 32, 40, 48, 64, 96, 128, 256 };

    for (i = 0; i < sizeof(Lists) / sizeof(Lists[0]); i++)
    {
        void *List = NULL;
        int cx = 0, cy = 0;
        HRESULT hr = SHGetImageList(Lists[i].Shil, &IID_IImageList_Local, &List);

        if (SUCCEEDED(hr) && List)
        {
            ImageList_GetIconSize((HIMAGELIST)List, &cx, &cy);
            printf("SHELL_IMAGELIST %s size=%dx%d count=%d\n", Lists[i].Name, cx, cy, ImageList_GetImageCount((HIMAGELIST)List));
            ((IUnknown *)List)->lpVtbl->Release((IUnknown *)List);
        }
        else
        {
            printf("SHELL_IMAGELIST %s hr=0x%08lx\n", Lists[i].Name, (ULONG)hr);
        }
    }
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop\\WindowMetrics", 0, KEY_READ, &Key) == ERROR_SUCCESS)
    {
        Size = sizeof(Value);
        if (RegQueryValueExW(Key, L"Shell Icon Size", NULL, &Type, (BYTE *)Value, &Size) == ERROR_SUCCESS)
            printf("SHELL_ICON_SIZE reg=\"%ls\"\n", Value);
        else
            printf("SHELL_ICON_SIZE reg=absent\n");
        RegCloseKey(Key);
    }
    /* What LoadImage hands back from shell32 for each requested size (folder icon 4) */
    for (i = 0; i < sizeof(Sizes) / sizeof(Sizes[0]); i++)
    {
        HMODULE Shell32 = GetModuleHandleW(L"shell32.dll");

        Icon = Shell32 ? (HICON)LoadImageW(Shell32, MAKEINTRESOURCEW(4), IMAGE_ICON, Sizes[i], Sizes[i], LR_DEFAULTCOLOR) : NULL;
        if (Icon && GetIconInfo(Icon, &Info))
        {
            ZeroMemory(&Bitmap, sizeof(Bitmap));
            GetObjectW(Info.hbmColor ? Info.hbmColor : Info.hbmMask, sizeof(Bitmap), &Bitmap);
            printf("SHELL_LOADIMAGE request=%d got=%ldx%ld bpp=%u\n", Sizes[i], Bitmap.bmWidth,
                   Info.hbmColor ? Bitmap.bmHeight : Bitmap.bmHeight / 2, Bitmap.bmBitsPixel);
            if (Info.hbmColor) DeleteObject(Info.hbmColor);
            if (Info.hbmMask) DeleteObject(Info.hbmMask);
        }
        else
        {
            printf("SHELL_LOADIMAGE request=%d failed error=%lu\n", Sizes[i], GetLastError());
        }
        if (Icon)
            DestroyIcon(Icon);
    }
}

static void
DumpControls(void)
{
    INITCOMMONCONTROLSEX Icc = { sizeof(Icc), ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_TREEVIEW_CLASSES };
    HWND Check, Tree, Track, Bar, Tab, Status, Combo, Edit, List;
    SIZE Ideal = { 0, 0 };
    RECT Rect;
    TBBUTTON Buttons[2];
    TBADDBITMAP Bitmaps;
    TCITEMW Item;
    TVINSERTSTRUCTW Insert;
    DWORD Spacing;

    InitCommonControlsEx(&Icc);
    Check = CreateWindowExW(0, WC_BUTTONW, L"Check box", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 10, 10, g_Window, NULL, NULL, NULL);
    if (Check && SendMessageW(Check, BCM_GETIDEALSIZE, 0, (LPARAM)&Ideal))
        printf("CTRL checkbox ideal=%ldx%ld\n", Ideal.cx, Ideal.cy);
    else
        printf("CTRL checkbox ideal=unsupported\n");
    Tree = CreateWindowExW(0, WC_TREEVIEWW, NULL, WS_CHILD | TVS_HASBUTTONS | TVS_HASLINES | TVS_CHECKBOXES, 0, 0, 200, 100, g_Window, NULL, NULL, NULL);
    if (Tree)
    {
        ZeroMemory(&Insert, sizeof(Insert));
        Insert.hParent = TVI_ROOT;
        Insert.item.mask = TVIF_TEXT;
        Insert.item.pszText = L"Item";
        SendMessageW(Tree, TVM_INSERTITEMW, 0, (LPARAM)&Insert);
        printf("CTRL treeview itemheight=%d indent=%d\n", (int)SendMessageW(Tree, TVM_GETITEMHEIGHT, 0, 0),
               (int)SendMessageW(Tree, TVM_GETINDENT, 0, 0));
    }
    Track = CreateWindowExW(0, TRACKBAR_CLASSW, NULL, WS_CHILD | TBS_HORZ, 0, 0, 200, 40, g_Window, NULL, NULL, NULL);
    if (Track)
    {
        SetRectEmpty(&Rect);
        SendMessageW(Track, TBM_GETTHUMBRECT, 0, (LPARAM)&Rect);
        printf("CTRL trackbar thumb=%ldx%ld thumblength=%d\n", Rect.right - Rect.left, Rect.bottom - Rect.top,
               (int)SendMessageW(Track, TBM_GETTHUMBLENGTH, 0, 0));
    }
    Bar = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL, WS_CHILD | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS, 0, 0, 300, 40, g_Window, NULL, NULL, NULL);
    if (Bar)
    {
        SendMessageW(Bar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        Bitmaps.hInst = HINST_COMMCTRL;
        Bitmaps.nID = IDB_STD_SMALL_COLOR;
        SendMessageW(Bar, TB_ADDBITMAP, 0, (LPARAM)&Bitmaps);
        ZeroMemory(Buttons, sizeof(Buttons));
        Buttons[0].iBitmap = STD_FILENEW; Buttons[0].idCommand = 1; Buttons[0].fsState = TBSTATE_ENABLED; Buttons[0].fsStyle = BTNS_BUTTON;
        Buttons[1].iBitmap = STD_FILEOPEN; Buttons[1].idCommand = 2; Buttons[1].fsState = TBSTATE_ENABLED; Buttons[1].fsStyle = BTNS_BUTTON;
        SendMessageW(Bar, TB_ADDBUTTONSW, 2, (LPARAM)Buttons);
        SendMessageW(Bar, TB_AUTOSIZE, 0, 0);
        Spacing = (DWORD)SendMessageW(Bar, TB_GETBUTTONSIZE, 0, 0);
        GetWindowRect(Bar, &Rect);
        printf("CTRL toolbar button=%ux%u height=%ld\n", LOWORD(Spacing), HIWORD(Spacing), Rect.bottom - Rect.top);
    }
    Tab = CreateWindowExW(0, WC_TABCONTROLW, NULL, WS_CHILD, 0, 0, 300, 100, g_Window, NULL, NULL, NULL);
    if (Tab)
    {
        ZeroMemory(&Item, sizeof(Item));
        Item.mask = TCIF_TEXT;
        Item.pszText = L"Tab";
        SendMessageW(Tab, TCM_INSERTITEMW, 0, (LPARAM)&Item);
        SetRectEmpty(&Rect);
        SendMessageW(Tab, TCM_GETITEMRECT, 0, (LPARAM)&Rect);
        printf("CTRL tab item=%ldx%ld\n", Rect.right - Rect.left, Rect.bottom - Rect.top);
    }
    Status = CreateWindowExW(0, STATUSCLASSNAMEW, L"Status", WS_CHILD | SBARS_SIZEGRIP, 0, 0, 0, 0, g_Window, NULL, NULL, NULL);
    if (Status)
    {
        SendMessageW(Status, WM_SIZE, 0, 0);
        GetWindowRect(Status, &Rect);
        printf("CTRL statusbar height=%ld\n", Rect.bottom - Rect.top);
    }
    Combo = CreateWindowExW(0, WC_COMBOBOXW, NULL, WS_CHILD | CBS_DROPDOWN, 0, 0, 150, 200, g_Window, NULL, NULL, NULL);
    if (Combo)
    {
        GetWindowRect(Combo, &Rect);
        printf("CTRL combobox height=%ld itemheight=%d\n", Rect.bottom - Rect.top, (int)SendMessageW(Combo, CB_GETITEMHEIGHT, (WPARAM)-1, 0));
    }
    Edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"Edit", WS_CHILD, 0, 0, 150, 10, g_Window, NULL, NULL, NULL);
    if (Edit)
    {
        HDC Dc = GetDC(Edit);
        HFONT Font = (HFONT)SendMessageW(Edit, WM_GETFONT, 0, 0), Old = NULL;
        TEXTMETRICW Tm;

        ZeroMemory(&Tm, sizeof(Tm));
        if (Dc)
        {
            if (Font) Old = SelectObject(Dc, Font);
            GetTextMetricsW(Dc, &Tm);
            if (Old) SelectObject(Dc, Old);
            ReleaseDC(Edit, Dc);
        }
        printf("CTRL edit default_font=%p tm_height=%ld\n", Font, Tm.tmHeight);
    }
    List = CreateWindowExW(0, WC_LISTVIEWW, NULL, WS_CHILD | LVS_ICON, 0, 0, 300, 200, g_Window, NULL, NULL, NULL);
    if (List)
    {
        Spacing = (DWORD)SendMessageW(List, LVM_GETITEMSPACING, FALSE, 0);
        printf("CTRL listview iconspacing=%ux%u smallspacing=%lx\n", LOWORD(Spacing), HIWORD(Spacing),
               (ULONG)SendMessageW(List, LVM_GETITEMSPACING, TRUE, 0));
    }
    {
        HMODULE Comctl = GetModuleHandleW(L"comctl32.dll");
        typedef HRESULT (CALLBACK *PFN_DLLGETVERSION)(DLLVERSIONINFO *);
        PFN_DLLGETVERSION GetVersionPtr = Comctl ? (PFN_DLLGETVERSION)GetProcAddress(Comctl, "DllGetVersion") : NULL;
        DLLVERSIONINFO Dvi;
        WCHAR Path[MAX_PATH] = L"";

        ZeroMemory(&Dvi, sizeof(Dvi));
        Dvi.cbSize = sizeof(Dvi);
        if (GetVersionPtr)
            GetVersionPtr(&Dvi);
        if (Comctl)
            GetModuleFileNameW(Comctl, Path, MAX_PATH);
        printf("CTRL comctl32 dll=\"%ls\" version=%lu.%lu.%lu\n", Path, Dvi.dwMajorVersion, Dvi.dwMinorVersion, Dvi.dwBuildNumber);
    }
}

static LRESULT CALLBACK
WndProc(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam)
{
    if (Message == WM_DPICHANGED)
        printf("MESSAGE WM_DPICHANGED x=%u y=%u\n", LOWORD(wParam), HIWORD(wParam));
    return DefWindowProcW(Window, Message, wParam, lParam);
}

static void
DumpWindow(void)
{
    RECT Window, Client;
    BOOL Nc = FALSE;
    DWORD Error = 0;

    GetWindowRect(g_Window, &Window);
    GetClientRect(g_Window, &Client);
    printf("WINDOW requested=300x200 window=%ldx%ld at=%ld,%ld client=%ldx%ld dpi=%u\n", Window.right - Window.left,
           Window.bottom - Window.top, Window.left, Window.top, Client.right, Client.bottom,
           Api.GetDpiForWindow ? Api.GetDpiForWindow(g_Window) : 0);
    if (Api.EnableNcDpi)
    {
        SetLastError(0);
        Nc = Api.EnableNcDpi(g_Window);
        Error = GetLastError();
    }
    printf("WINDOW enable_nc_dpi_after_create=%d error=%lu\n", Nc, Error);
}

int
main(int argc, char **argv)
{
    WNDCLASSW Class;
    DPICTX Ctx = CTX_UNAWARE;
    BOOL Set = FALSE;
    DWORD Error = 0;
    const char *Label = "";
    int i;

    for (i = 1; i < argc; i++)
    {
        if (!strncmp(argv[i], "--variant=", 10)) g_Variant = argv[i] + 10;
        else if (!strncmp(argv[i], "--label=", 8)) Label = argv[i] + 8;
    }
    if (!strcmp(g_Variant, "system")) Ctx = CTX_SYSTEM;
    else if (!strcmp(g_Variant, "pmv1")) Ctx = CTX_PMV1;
    else if (!strcmp(g_Variant, "pmv2")) Ctx = CTX_PMV2;
    else if (!strcmp(g_Variant, "gdiscaled")) Ctx = CTX_GDISCALED;
    else if (strcmp(g_Variant, "unaware"))
    {
        fprintf(stderr, "usage: dpidump [--variant=unaware|system|pmv1|pmv2|gdiscaled] [--label=text]\n");
        return 2;
    }

    LoadApis();
    if (Api.SetProcessCtx)
    {
        SetLastError(0);
        Set = Api.SetProcessCtx(Ctx);
        Error = GetLastError();
    }
    else if (Ctx != CTX_UNAWARE)
    {
        Set = SetProcessDPIAware();
        Error = GetLastError();
    }
    printf("DPIDUMP_BEGIN variant=%s label=%s set=%d error=%lu\n", g_Variant, Label, Set, Error);
    DumpVersion();
    DumpRegistry();

    ZeroMemory(&Class, sizeof(Class));
    Class.lpfnWndProc = WndProc;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.lpszClassName = L"LiberNTDpiDump";
    Class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&Class);
    /* Never shown: the host reference run must not open a window on the owner's desktop */
    g_Window = CreateWindowExW(0, Class.lpszClassName, L"dpidump", WS_OVERLAPPEDWINDOW, 10, 10, 300, 200, NULL, NULL,
                               Class.hInstance, NULL);
    if (!g_Window)
    {
        printf("DPIDUMP_END rc=4 error=%lu\n", GetLastError());
        return 4;
    }
    DumpContext();
    DumpCaps();
    DumpWindow();
    DumpMetrics();
    DumpNonClient();
    DumpStockFonts();
    DumpFrames();
    DumpTheme();
    DumpShellIcons();
    DumpControls();
    DestroyWindow(g_Window);
    printf("DPIDUMP_END rc=0\n");
    return 0;
}
