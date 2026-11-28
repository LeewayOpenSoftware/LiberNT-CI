/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Show common controls, system fonts and shell icons for a DPI screenshot
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef HANDLE DPICTX;
typedef BOOL (WINAPI *PFN_SETPROCESSCTX)(DPICTX);
typedef UINT (WINAPI *PFN_GETDPIFORSYSTEM)(VOID);
typedef UINT (WINAPI *PFN_GETDPIFORWINDOW)(HWND);

static PFN_GETDPIFORSYSTEM pGetDpiForSystem;
static PFN_GETDPIFORWINDOW pGetDpiForWindow;
static const char *g_Variant = "unaware";
static HIMAGELIST g_Lists[4];
static int g_ListSizes[4];

static void
DrawIcons(HDC Dc, int x, int y)
{
    static const GUID IID_IImageList_Local =
        { 0x46EB5926, 0x582E, 0x4017, { 0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50 } };
    /* small, large, extra large (jumbo is 256 pixels and only its size is reported) */
    static const int Shil[3] = { 1, 0, 2 };
    static const WCHAR *Paths[] = { L"C:\\Windows", L"C:\\", L"C:\\Windows\\System32\\notepad.exe",
                                    L"C:\\Windows\\win.ini", L"C:\\Windows\\System32\\shell32.dll" };
    static const int Sizes[] = { 16, 20, 24, 32, 40, 48, 64 };
    HMODULE Shell32 = GetModuleHandleW(L"shell32.dll");
    SHFILEINFOW Info;
    int Indices[5];
    WCHAR Text[128];
    UINT l, i;

    /* LoadImage at explicit sizes: what the loader picks for 16..64 from the shell32 folder icon */
    TextOutW(Dc, x, y, L"LoadImage", 9);
    for (i = 0; i < sizeof(Sizes) / sizeof(Sizes[0]); i++)
    {
        HICON Icon = Shell32 ? (HICON)LoadImageW(Shell32, MAKEINTRESOURCEW(4), IMAGE_ICON, Sizes[i], Sizes[i], LR_DEFAULTCOLOR) : NULL;

        if (Icon)
        {
            DrawIconEx(Dc, x + 110 + (int)i * 72, y, Icon, Sizes[i], Sizes[i], 0, NULL, DI_NORMAL);
            DestroyIcon(Icon);
        }
        wsprintfW(Text, L"%d", Sizes[i]);
        TextOutW(Dc, x + 110 + (int)i * 72, y + 66, Text, lstrlenW(Text));
    }
    y += 84;

    /* The system image list fills lazily: ask the shell for the icons of real objects first */
    for (i = 0; i < 5; i++)
    {
        ZeroMemory(&Info, sizeof(Info));
        Indices[i] = SHGetFileInfoW(Paths[i], 0, &Info, sizeof(Info), SHGFI_SYSICONINDEX | SHGFI_SMALLICON) ? Info.iIcon : -1;
    }
    for (l = 0; l < 3; l++)
    {
        void *List = NULL;
        int cx = 0, h = 0;

        if (!g_Lists[l] && SUCCEEDED(SHGetImageList(Shil[l], &IID_IImageList_Local, &List)) && List)
            g_Lists[l] = (HIMAGELIST)List;
        if (!g_Lists[l])
            continue;
        ImageList_GetIconSize(g_Lists[l], &cx, &h);
        g_ListSizes[l] = cx;
        wsprintfW(Text, L"SHIL %d (%dx%d, %d)", Shil[l], cx, h, ImageList_GetImageCount(g_Lists[l]));
        TextOutW(Dc, x, y, Text, lstrlenW(Text));
        for (i = 0; i < 5; i++)
        {
            if (Indices[i] >= 0)
                ImageList_Draw(g_Lists[l], Indices[i], Dc, x + 150 + (int)i * (cx + 8), y, ILD_NORMAL);
        }
        y += max(h, 18) + 4;
    }
}

static void
Paint(HWND Window, HDC Dc)
{
    RECT Client;
    HFONT Old;
    WCHAR Text[256];
    HDC Screen = GetDC(NULL);
    int LogPixels = Screen ? GetDeviceCaps(Screen, LOGPIXELSY) : 0;
    int i;
    TEXTMETRICW Tm;
    NONCLIENTMETRICSW Ncm;
    HFONT Message = NULL;

    if (Screen)
        ReleaseDC(NULL, Screen);
    GetClientRect(Window, &Client);
    SetBkMode(Dc, TRANSPARENT);

    /* ruler: a tick every 10 logical pixels, a number every 100 */
    for (i = 0; i < Client.right; i += 10)
    {
        MoveToEx(Dc, i, 0, NULL);
        LineTo(Dc, i, (i % 100) ? 4 : 10);
        if (i && !(i % 100))
        {
            wsprintfW(Text, L"%d", i);
            TextOutW(Dc, i + 2, 2, Text, lstrlenW(Text));
        }
    }
    for (i = 0; i < Client.bottom; i += 10)
    {
        MoveToEx(Dc, 0, i, NULL);
        LineTo(Dc, (i % 100) ? 4 : 10, i);
    }

    Old = SelectObject(Dc, GetStockObject(DEFAULT_GUI_FONT));
    GetTextMetricsW(Dc, &Tm);
    wsprintfW(Text, L"dpishot %S  GetDpiForWindow=%u  GetDpiForSystem=%u  LOGPIXELSY=%d  client=%ldx%ld",
              g_Variant, pGetDpiForWindow ? pGetDpiForWindow(Window) : 0, pGetDpiForSystem ? pGetDpiForSystem() : 0,
              LogPixels, Client.right, Client.bottom);
    TextOutW(Dc, 16, 16, Text, lstrlenW(Text));
    wsprintfW(Text, L"DEFAULT_GUI_FONT: The quick brown fox jumps over the lazy dog 0123456789 (tmHeight %ld)", Tm.tmHeight);
    TextOutW(Dc, 16, 34, Text, lstrlenW(Text));
    SelectObject(Dc, GetStockObject(SYSTEM_FONT));
    GetTextMetricsW(Dc, &Tm);
    wsprintfW(Text, L"SYSTEM_FONT: The quick brown fox jumps over the lazy dog (tmHeight %ld)", Tm.tmHeight);
    TextOutW(Dc, 16, 52, Text, lstrlenW(Text));
    ZeroMemory(&Ncm, sizeof(Ncm));
    Ncm.cbSize = sizeof(Ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(Ncm), &Ncm, 0))
        Message = CreateFontIndirectW(&Ncm.lfMessageFont);
    if (Message)
    {
        SelectObject(Dc, Message);
        GetTextMetricsW(Dc, &Tm);
        wsprintfW(Text, L"lfMessageFont %s %ld: The quick brown fox jumps over the lazy dog (tmHeight %ld)",
                  Ncm.lfMessageFont.lfFaceName, Ncm.lfMessageFont.lfHeight, Tm.tmHeight);
        TextOutW(Dc, 16, 72, Text, lstrlenW(Text));
    }
    SelectObject(Dc, GetStockObject(DEFAULT_GUI_FONT));
    DrawIcons(Dc, 16, 300);
    SelectObject(Dc, Old);
    if (Message)
        DeleteObject(Message);
}

static LRESULT CALLBACK
WndProc(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT Ps;
    HDC Dc;

    switch (Message)
    {
        case WM_PAINT:
            Dc = BeginPaint(Window, &Ps);
            Paint(Window, Dc);
            EndPaint(Window, &Ps);
            return 0;
        case WM_TIMER:
            DestroyWindow(Window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(Window, Message, wParam, lParam);
}

static HWND
Child(HWND Parent, const WCHAR *Class, const WCHAR *Text, DWORD Style, int x, int y, int cx, int cy, HFONT Font)
{
    HWND Wnd = CreateWindowExW(0, Class, Text, WS_CHILD | WS_VISIBLE | Style, x, y, cx, cy, Parent, NULL, NULL, NULL);

    if (Wnd && Font)
        SendMessageW(Wnd, WM_SETFONT, (WPARAM)Font, FALSE);
    return Wnd;
}

static void
CreateControls(HWND Window)
{
    INITCOMMONCONTROLSEX Icc = { sizeof(Icc), ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_TREEVIEW_CLASSES |
                                 ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES };
    NONCLIENTMETRICSW Ncm;
    HFONT Font = NULL;
    HWND Tree, Bar, Tab, Track, Progress, Combo, Status;
    TVINSERTSTRUCTW Insert;
    TBBUTTON Buttons[3];
    TBADDBITMAP Bitmaps;
    TCITEMW Item;
    HTREEITEM Root;

    InitCommonControlsEx(&Icc);
    ZeroMemory(&Ncm, sizeof(Ncm));
    Ncm.cbSize = sizeof(Ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(Ncm), &Ncm, 0))
        Font = CreateFontIndirectW(&Ncm.lfMessageFont);

    Child(Window, WC_BUTTONW, L"Group box", BS_GROUPBOX, 16, 96, 260, 120, Font);
    Child(Window, WC_BUTTONW, L"Check box", BS_AUTOCHECKBOX, 28, 118, 110, 20, Font);
    Child(Window, WC_BUTTONW, L"Checked", BS_AUTOCHECKBOX, 150, 118, 110, 20, Font);
    SendMessageW(Child(Window, WC_BUTTONW, L"Radio", BS_AUTORADIOBUTTON, 28, 142, 110, 20, Font), BM_SETCHECK, BST_CHECKED, 0);
    Child(Window, WC_BUTTONW, L"Push button", BS_PUSHBUTTON, 28, 168, 100, 26, Font);
    Child(Window, WC_BUTTONW, L"Default", BS_DEFPUSHBUTTON, 140, 168, 100, 26, Font);
    Combo = Child(Window, WC_COMBOBOXW, NULL, CBS_DROPDOWN | WS_VSCROLL, 290, 100, 150, 200, Font);
    SendMessageW(Combo, CB_ADDSTRING, 0, (LPARAM)L"Combo box");
    SendMessageW(Combo, CB_SETCURSEL, 0, 0);
    Child(Window, WC_EDITW, L"Edit control", WS_BORDER | ES_AUTOHSCROLL, 290, 130, 150, 22, Font);
    Tree = Child(Window, WC_TREEVIEWW, NULL, WS_BORDER | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_CHECKBOXES, 290, 158, 150, 100, Font);
    ZeroMemory(&Insert, sizeof(Insert));
    Insert.hParent = TVI_ROOT;
    Insert.item.mask = TVIF_TEXT;
    Insert.item.pszText = L"Tree root";
    Root = (HTREEITEM)SendMessageW(Tree, TVM_INSERTITEMW, 0, (LPARAM)&Insert);
    Insert.hParent = Root;
    Insert.item.pszText = L"Child item";
    SendMessageW(Tree, TVM_INSERTITEMW, 0, (LPARAM)&Insert);
    SendMessageW(Tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)Root);
    Track = Child(Window, TRACKBAR_CLASSW, NULL, TBS_HORZ | TBS_AUTOTICKS, 460, 100, 180, 30, Font);
    SendMessageW(Track, TBM_SETPOS, TRUE, 3);
    Progress = Child(Window, PROGRESS_CLASSW, NULL, 0, 460, 140, 180, 18, Font);
    SendMessageW(Progress, PBM_SETPOS, 50, 0);
    Tab = Child(Window, WC_TABCONTROLW, NULL, 0, 460, 168, 180, 90, Font);
    ZeroMemory(&Item, sizeof(Item));
    Item.mask = TCIF_TEXT;
    Item.pszText = L"First";
    SendMessageW(Tab, TCM_INSERTITEMW, 0, (LPARAM)&Item);
    Item.pszText = L"Second";
    SendMessageW(Tab, TCM_INSERTITEMW, 1, (LPARAM)&Item);
    Bar = Child(Window, TOOLBARCLASSNAMEW, NULL, TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TOOLTIPS | CCS_NORESIZE, 16, 226, 400, 36, Font);
    SendMessageW(Bar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    Bitmaps.hInst = HINST_COMMCTRL;
    Bitmaps.nID = IDB_STD_SMALL_COLOR;
    SendMessageW(Bar, TB_ADDBITMAP, 0, (LPARAM)&Bitmaps);
    ZeroMemory(Buttons, sizeof(Buttons));
    Buttons[0].iBitmap = STD_FILENEW; Buttons[0].idCommand = 1; Buttons[0].fsState = TBSTATE_ENABLED; Buttons[0].fsStyle = BTNS_BUTTON | BTNS_SHOWTEXT; Buttons[0].iString = (INT_PTR)L"New";
    Buttons[1].iBitmap = STD_FILEOPEN; Buttons[1].idCommand = 2; Buttons[1].fsState = TBSTATE_ENABLED; Buttons[1].fsStyle = BTNS_BUTTON | BTNS_SHOWTEXT; Buttons[1].iString = (INT_PTR)L"Open";
    Buttons[2].iBitmap = STD_FILESAVE; Buttons[2].idCommand = 3; Buttons[2].fsState = TBSTATE_ENABLED; Buttons[2].fsStyle = BTNS_BUTTON | BTNS_SHOWTEXT; Buttons[2].iString = (INT_PTR)L"Save";
    SendMessageW(Bar, TB_ADDBUTTONSW, 3, (LPARAM)Buttons);
    SendMessageW(Bar, TB_AUTOSIZE, 0, 0);
    Status = Child(Window, STATUSCLASSNAMEW, L"Status bar text", SBARS_SIZEGRIP, 0, 0, 0, 0, Font);
    SendMessageW(Status, WM_SIZE, 0, 0);
}

int WINAPI
wWinMain(HINSTANCE Instance, HINSTANCE Previous, LPWSTR CmdLine, int Show)
{
    WNDCLASSW Class;
    HWND Window;
    MSG Msg;
    HMODULE User32 = GetModuleHandleW(L"user32.dll");
    PFN_SETPROCESSCTX pSetProcessCtx = User32 ? (PFN_SETPROCESSCTX)GetProcAddress(User32, "SetProcessDpiAwarenessContext") : NULL;
    DPICTX Ctx = (DPICTX)-1;
    UINT Ms = 10000;
    int Argc = 0;
    LPWSTR *Argv = CommandLineToArgvW(GetCommandLineW(), &Argc);
    static char Variant[32];

    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CmdLine);
    UNREFERENCED_PARAMETER(Show);
    if (Argc > 1)
    {
        WideCharToMultiByte(CP_ACP, 0, Argv[1], -1, Variant, sizeof(Variant), NULL, NULL);
        g_Variant = Variant;
    }
    if (Argc > 2)
        Ms = (UINT)_wtoi(Argv[2]);
    if (!strcmp(g_Variant, "system")) Ctx = (DPICTX)-2;
    else if (!strcmp(g_Variant, "pmv1")) Ctx = (DPICTX)-3;
    else if (!strcmp(g_Variant, "pmv2")) Ctx = (DPICTX)-4;
    else if (!strcmp(g_Variant, "gdiscaled")) Ctx = (DPICTX)-5;
    if (pSetProcessCtx)
        pSetProcessCtx(Ctx);
    else if (Ctx != (DPICTX)-1)
        SetProcessDPIAware();
    pGetDpiForSystem = User32 ? (PFN_GETDPIFORSYSTEM)GetProcAddress(User32, "GetDpiForSystem") : NULL;
    pGetDpiForWindow = User32 ? (PFN_GETDPIFORWINDOW)GetProcAddress(User32, "GetDpiForWindow") : NULL;

    ZeroMemory(&Class, sizeof(Class));
    Class.lpfnWndProc = WndProc;
    Class.hInstance = Instance;
    Class.hCursor = LoadCursor(NULL, IDC_ARROW);
    Class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    Class.lpszClassName = L"LiberNTDpiShot";
    RegisterClassW(&Class);
    /* The window must fit a 1280x800 screen at any scale, aware or virtualized, so its logical size shrinks
       with the real system DPI (read from the registry, which no awareness context virtualizes) */
    {
        DWORD Real = 96, Size = sizeof(Real);
        INT Cx = 700, Cy = 560;

        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current\\Software\\Fonts",
                         L"LogPixels", RRF_RT_REG_DWORD, NULL, &Real, &Size) != ERROR_SUCCESS || Real < 96)
            Real = 96;
        Cx = min(Cx, (INT)(1240 * 96 / Real));
        Cy = min(Cy, (INT)(760 * 96 / Real));
        Window = CreateWindowExW(0, Class.lpszClassName, L"dpishot", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_VSCROLL, 8, 8, Cx, Cy,
                                 NULL, NULL, Instance, NULL);
    }
    if (!Window)
        return 1;
    CreateControls(Window);
    SetTimer(Window, 1, Ms, NULL);
    UpdateWindow(Window);
    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }
    return 0;
}
