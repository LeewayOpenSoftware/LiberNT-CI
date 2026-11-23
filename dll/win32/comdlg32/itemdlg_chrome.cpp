/*
 * PROJECT:     LiberNT
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Windows 11 chrome of the item dialog: address, search, commands and Quick access
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <shellframe.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <strsafe.h>

#include "itemdlg_chrome.h"

extern "C" HINSTANCE COMDLG32_hInstance;

enum OrganizeCommand
{
    OrganizeCut = 1,
    OrganizeCopy,
    OrganizePaste,
    OrganizeDelete,
    OrganizeRename,
    OrganizeSelectAll,
    OrganizeProperties,
};

static void
LoadText(UINT id, LPCWSTR fallback, LPWSTR buffer, int length)
{
    if (!LoadStringW(COMDLG32_hInstance, id, buffer, length))
        StringCchCopyW(buffer, length, fallback);
}

struct ItemDlgChrome : public ShellFrame::AddressBandHost, public ShellFrame::NavigationPaneHost
{
    HWND Dialog;
    const ItemDlgChromeHost *Host;
    void *Context;
    BOOL Started;
    HWND NavBar;
    HWND RefreshBar;
    HWND Commands;
    HWND ViewBar;
    HIMAGELIST NavImages;
    HIMAGELIST NavDisabled;
    HIMAGELIST CommandImages;
    HIMAGELIST ChevronImages;
    HWND HideBar;
    HBRUSH Background;
    ShellFrame::BandPalette Palette;
    BOOL Save;
    BOOL Collapsed;
    ShellFrame::AddressBand *Address;
    ShellFrame::SearchBox *Search;
    ShellFrame::NavigationPane Pane;
    ShellFrame::NavigationPaneEvents *Events;
    DWORD Cookie;
    WCHAR Texts[10][64];
    PIDLIST_ABSOLUTE Location;
    PIDLIST_ABSOLUTE SearchOrigin;
    WCHAR SearchQuery[MAX_PATH];
    WCHAR SearchPaths[26][MAX_PATH];
    UINT SearchPathCount;

    int Scale(int value) const
    {
        return MulDiv(value, GetDpiForWindow(Dialog), 96);
    }

    void OnAddressNavigate(PCIDLIST_ABSOLUTE pidl) override
    {
        Host->navigate(Context, pidl);
    }

    void OnAddressText(LPCWSTR text) override
    {
        Host->navigate_text(Context, text);
    }

    void OnAddressCancel() override
    {
    }

    void OnNavigationItem(IShellItem *item, BOOL newTab) override
    {
        PIDLIST_ABSOLUTE pidl = NULL;

        UNREFERENCED_PARAMETER(newTab);
        if (SUCCEEDED(SHGetIDListFromObject(item, &pidl)))
        {
            Host->navigate(Context, pidl);
            ILFree(pidl);
        }
    }

    void PaintNavigationItem(HDC dc, const RECT &rc, const NSTCCUSTOMDRAW *item) override
    {
        Pane.PaintRoot(dc, rc, item, GetDpiForWindow(Dialog));
    }
};

static HWND
CreateBar(HWND parent, UINT id)
{
    HWND bar = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBSTYLE_FLAT | TBSTYLE_TRANSPARENT |
                               TBSTYLE_TOOLTIPS | TBSTYLE_LIST | CCS_NODIVIDER | CCS_NORESIZE | CCS_NOPARENTALIGN,
                               0, 0, 0, 0, parent, (HMENU)(UINT_PTR)id, COMDLG32_hInstance, NULL);

    if (bar)
    {
        SendMessageW(bar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        SendMessageW(bar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS | TBSTYLE_EX_DRAWDDARROWS);
    }
    return bar;
}

/* Text shows only with BTNS_SHOWTEXT; otherwise it is the tooltip, as in File Explorer. */
static void
AddButton(HWND bar, int image, UINT id, BYTE style, LPCWSTR text)
{
    TBBUTTON button = { 0 };

    button.iBitmap = image;
    button.idCommand = (int)id;
    button.fsState = TBSTATE_ENABLED;
    button.fsStyle = style | BTNS_AUTOSIZE;
    button.iString = (INT_PTR)text;
    SendMessageW(bar, TB_ADDBUTTONSW, 1, (LPARAM)&button);
}

static SIZE
BarSize(HWND bar)
{
    SIZE size = { 0, 0 };

    if (bar)
    {
        SendMessageW(bar, TB_AUTOSIZE, 0, 0);
        SendMessageW(bar, TB_GETMAXSIZE, 0, (LPARAM)&size);
    }
    return size;
}

static void
Place(HDWP *hdwp, HWND hwnd, int x, int y, int cx, int cy)
{
    if (!hwnd)
        return;
    if (*hdwp)
        *hdwp = DeferWindowPos(*hdwp, hwnd, NULL, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
    else
        SetWindowPos(hwnd, NULL, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
}

static const WCHAR FindFolder[] = L"shell:::{E17D4FC0-5564-11D1-83F2-00A0C90DC849}";

static BOOL
IsFindFolder(PCIDLIST_ABSOLUTE pidl)
{
    PIDLIST_ABSOLUTE find = NULL;
    BOOL same;

    if (!pidl || FAILED(SHParseDisplayName(FindFolder, NULL, &find, 0, NULL)))
        return FALSE;
    same = ILIsEqual(pidl, find);
    ILFree(find);
    return same;
}

/* Like File Explorer: search the shown folder, or every local drive from a virtual folder. */
static void
StartSearch(ItemDlgChrome *chrome, LPCWSTR text)
{
    PIDLIST_ABSOLUTE find = NULL;
    WCHAR path[MAX_PATH];

    StringCchCopyW(chrome->SearchQuery, _countof(chrome->SearchQuery), text);
    StrTrimW(chrome->SearchQuery, L" \t");
    if (!chrome->SearchQuery[0])
    {
        if (chrome->SearchOrigin && IsFindFolder(chrome->Location))
            chrome->Host->navigate(chrome->Context, chrome->SearchOrigin);
        return;
    }
    if (!IsFindFolder(chrome->Location))
    {
        chrome->SearchPathCount = 0;
        if (chrome->Location && SHGetPathFromIDListW(chrome->Location, path))
        {
            StringCchCopyW(chrome->SearchPaths[chrome->SearchPathCount++], MAX_PATH, path);
        }
        else
        {
            WCHAR drives[128];
            DWORD length = GetLogicalDriveStringsW(_countof(drives), drives);

            for (PWSTR drive = drives; length && *drive && chrome->SearchPathCount < 26; drive += lstrlenW(drive) + 1)
            {
                UINT type = GetDriveTypeW(drive);

                if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE)
                    StringCchCopyW(chrome->SearchPaths[chrome->SearchPathCount++], MAX_PATH, drive);
            }
        }
        if (chrome->SearchOrigin)
            ILFree(chrome->SearchOrigin);
        chrome->SearchOrigin = chrome->Location ? ILClone(chrome->Location) : NULL;
    }
    if (!chrome->SearchPathCount)
        return;
    if (IsFindFolder(chrome->Location))
    {
        ItemDlgChrome_SetLocation(chrome, chrome->Location);
        return;
    }
    if (SUCCEEDED(SHParseDisplayName(FindFolder, NULL, &find, 0, NULL)))
    {
        chrome->Host->navigate(chrome->Context, find);
        ILFree(find);
    }
}

/* The results folder is shown; hand it the query. The search engine lives in explorerframe. */
static void
RunPendingSearch(ItemDlgChrome *chrome)
{
    typedef HRESULT (WINAPI *START_SEARCH)(HWND, PCWSTR *, SIZE_T, PCWSTR);
    IShellView *view;
    HMODULE module;
    START_SEARCH start;
    HWND window = NULL;
    PCWSTR paths[26];

    if (!chrome->SearchQuery[0])
        return;
    module = GetModuleHandleW(L"explorerframe.dll");
    if (!module)
    {
        WCHAR path[MAX_PATH];

        /* Only the system copy, never one found on the application path. */
        if (GetSystemDirectoryW(path, _countof(path)) &&
            SUCCEEDED(StringCchCatW(path, _countof(path), L"\\explorerframe.dll")))
            module = LoadLibraryW(path);
    }
    start = module ? (START_SEARCH)GetProcAddress(module, "ShellFind_StartSearch") : NULL;
    view = chrome->Host->get_view(chrome->Context);
    if (start && view && SUCCEEDED(view->GetWindow(&window)) && window)
    {
        for (UINT i = 0; i < chrome->SearchPathCount; i++)
            paths[i] = chrome->SearchPaths[i];
        start(window, paths, chrome->SearchPathCount, chrome->SearchQuery);
    }
    if (view)
        view->Release();
    /* explorerframe stays loaded while its folder is shown. */
    chrome->SearchQuery[0] = UNICODE_NULL;
}

/* Enter and Escape belong to the address and search boxes, not to the dialog's buttons. */
static LRESULT CALLBACK
EditSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    ItemDlgChrome *chrome = (ItemDlgChrome *)data;

    switch (msg)
    {
        case WM_GETDLGCODE:
        {
            MSG *key = (MSG *)lParam;

            if (key && key->message == WM_KEYDOWN && (key->wParam == VK_RETURN || key->wParam == VK_ESCAPE))
                return DLGC_WANTALLKEYS | DefSubclassProc(hwnd, msg, wParam, lParam);
            break;
        }
        case WM_KEYDOWN:
            if (id == 2 && wParam == VK_RETURN)
            {
                WCHAR text[MAX_PATH];

                GetWindowTextW(hwnd, text, _countof(text));
                StartSearch(chrome, text);
                return 0;
            }
            if (id == 2 && wParam == VK_ESCAPE)
            {
                SetWindowTextW(hwnd, L"");
                StartSearch(chrome, L"");
                return 0;
            }
            break;
        case WM_CHAR:
            if (id == 2 && (wParam == L'\r' || wParam == 0x1b))
                return 0;
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static BOOL
IsDark(COLORREF color)
{
    return GetRValue(color) * 299 + GetGValue(color) * 587 + GetBValue(color) * 114 < 128000;
}

static void
SetImages(HWND bar, HIMAGELIST images, HIMAGELIST disabled)
{
    if (!bar)
        return;
    SendMessageW(bar, TB_SETIMAGELIST, 0, (LPARAM)images);
    if (disabled)
        SendMessageW(bar, TB_SETDISABLEDIMAGELIST, 0, (LPARAM)disabled);
    InvalidateRect(bar, NULL, TRUE);
}

/* Colors and glyphs follow the system colors, light or dark, like File Explorer. */
static void
ApplyTheme(ItemDlgChrome *chrome)
{
    static const LPCWSTR NavGlyphs[] =
    {
        L"arrow_left_16_regular", L"arrow_right_16_regular", L"arrow_up_16_regular", L"arrow_clockwise_16_regular"
    };
    static const LPCWSTR CommandGlyphs[] = { L"add_circle_20_regular", L"list_20_regular" };
    static const LPCWSTR ChevronGlyphs[] = { L"chevron_up_12_regular", L"chevron_down_12_regular" };
    HIMAGELIST old[4] = { chrome->NavImages, chrome->NavDisabled, chrome->CommandImages, chrome->ChevronImages };
    BOOL dark;

    ShellFrame::GetBandPalette(&chrome->Palette);
    chrome->NavImages = ShellFrame::GlyphImages(NavGlyphs, _countof(NavGlyphs), chrome->Scale(16), chrome->Palette.Glyph);
    chrome->NavDisabled = ShellFrame::GlyphImages(NavGlyphs, _countof(NavGlyphs), chrome->Scale(16),
                                                  chrome->Palette.GlyphDisabled);
    chrome->CommandImages = ShellFrame::GlyphImages(CommandGlyphs, _countof(CommandGlyphs), chrome->Scale(20),
                                                    chrome->Palette.Glyph);
    chrome->ChevronImages = ShellFrame::GlyphImages(ChevronGlyphs, _countof(ChevronGlyphs), chrome->Scale(12),
                                                    chrome->Palette.Glyph);
    SetImages(chrome->NavBar, chrome->NavImages, chrome->NavDisabled);
    SetImages(chrome->RefreshBar, chrome->NavImages, NULL);
    SetImages(chrome->Commands, chrome->CommandImages, NULL);
    SetImages(chrome->ViewBar, chrome->CommandImages, NULL);
    SetImages(chrome->HideBar, chrome->ChevronImages, NULL);
    for (UINT i = 0; i < _countof(old); i++)
    {
        if (old[i])
            ImageList_Destroy(old[i]);
    }

    if (chrome->Background)
        DeleteObject(chrome->Background);
    chrome->Background = CreateSolidBrush(chrome->Palette.Band);
    chrome->Pane.SetPinGlyph(chrome->Scale(16), chrome->Palette.GlyphDisabled);
    chrome->Pane.SetBackground(chrome->Palette.Body);
    if (chrome->Address)
        chrome->Address->RefreshGlyphs();
    if (chrome->Search)
        chrome->Search->RefreshGlyphs();

    /* DWMWA_USE_IMMERSIVE_DARK_MODE */
    dark = IsDark(chrome->Palette.Window);
    DwmSetWindowAttribute(chrome->Dialog, 20, &dark, sizeof(dark));
}

static void
UpdateHideButton(ItemDlgChrome *chrome)
{
    TBBUTTONINFOW info = { sizeof(info) };

    if (!chrome->HideBar)
        return;
    info.dwMask = TBIF_TEXT | TBIF_IMAGE;
    info.iImage = chrome->Collapsed ? 1 : 0;
    info.pszText = chrome->Collapsed ? chrome->Texts[8] : chrome->Texts[7];
    SendMessageW(chrome->HideBar, TB_SETBUTTONINFOW, IDC_HIDEFOLDERS, (LPARAM)&info);
}

extern "C" ItemDlgChrome *
ItemDlgChrome_Create(HWND dialog, const ItemDlgChromeHost *host, void *context, BOOL save)
{
    ItemDlgChrome *chrome;
    HFONT font = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);

    chrome = new ItemDlgChrome();
    if (!chrome)
        return NULL;
    chrome->Dialog = dialog;
    chrome->Host = host;
    chrome->Context = context;
    chrome->Save = save;
    chrome->Started = ShellFrame::Startup();
    if (!chrome->Started)
    {
        delete chrome;
        return NULL;
    }

    LoadText(IDS_NAV_BACK, L"Back", chrome->Texts[0], _countof(chrome->Texts[0]));
    LoadText(IDS_NAV_FORWARD, L"Forward", chrome->Texts[1], _countof(chrome->Texts[1]));
    LoadText(IDS_NAV_UP, L"Up", chrome->Texts[2], _countof(chrome->Texts[2]));
    LoadText(IDS_NAV_REFRESH, L"Refresh", chrome->Texts[3], _countof(chrome->Texts[3]));
    LoadText(IDS_ORGANIZE, L"Organize", chrome->Texts[4], _countof(chrome->Texts[4]));
    LoadText(IDS_NEW_FOLDER, L"New folder", chrome->Texts[5], _countof(chrome->Texts[5]));
    LoadText(IDS_VIEW_DETAILS, L"Details", chrome->Texts[6], _countof(chrome->Texts[6]));
    LoadText(IDS_HIDE_FOLDERS, L"Hide Folders", chrome->Texts[7], _countof(chrome->Texts[7]));
    LoadText(IDS_BROWSE_FOLDERS, L"Browse Folders", chrome->Texts[8], _countof(chrome->Texts[8]));

    chrome->NavBar = CreateBar(dialog, 0x7f0);
    AddButton(chrome->NavBar, 0, IDC_NAVBACK, BTNS_BUTTON, chrome->Texts[0]);
    AddButton(chrome->NavBar, 1, IDC_NAVFORWARD, BTNS_BUTTON, chrome->Texts[1]);
    AddButton(chrome->NavBar, 2, IDC_NAVUP, BTNS_BUTTON, chrome->Texts[2]);

    chrome->RefreshBar = CreateBar(dialog, 0x7f1);
    AddButton(chrome->RefreshBar, 3, IDC_NAVREFRESH, BTNS_BUTTON, chrome->Texts[3]);

    chrome->Commands = CreateBar(dialog, 0x7f2);
    AddButton(chrome->Commands, I_IMAGENONE, IDC_ORGANIZE, BTNS_WHOLEDROPDOWN | BTNS_SHOWTEXT, chrome->Texts[4]);
    AddButton(chrome->Commands, 0, IDC_NEWFOLDER, BTNS_BUTTON | BTNS_SHOWTEXT, chrome->Texts[5]);

    chrome->ViewBar = CreateBar(dialog, 0x7f3);
    AddButton(chrome->ViewBar, 1, IDC_VIEWMENU, BTNS_WHOLEDROPDOWN, chrome->Texts[6]);

    /* Only the save dialog folds its browser away, as on Windows. */
    if (save)
    {
        chrome->HideBar = CreateBar(dialog, 0x7f4);
        AddButton(chrome->HideBar, 0, IDC_HIDEFOLDERS, BTNS_BUTTON | BTNS_SHOWTEXT, chrome->Texts[7]);
    }

    chrome->Address = new ShellFrame::AddressBand();
    if (chrome->Address && chrome->Address->Create(dialog, chrome, font))
        SetWindowSubclass(chrome->Address->EditWindow(), EditSubclass, 1, (DWORD_PTR)chrome);
    chrome->Search = new ShellFrame::SearchBox();
    if (chrome->Search && chrome->Search->Create(dialog, font))
        SetWindowSubclass(chrome->Search->EditWindow(), EditSubclass, 2, (DWORD_PTR)chrome);

    if (chrome->Pane.Create(dialog, chrome->Scale(26)))
    {
        chrome->Events = new ShellFrame::NavigationPaneEvents(chrome);
        if (chrome->Events)
            chrome->Pane.Control()->TreeAdvise(static_cast<INameSpaceTreeControlEvents *>(chrome->Events),
                                               &chrome->Cookie);
    }
    ApplyTheme(chrome);
    return chrome;
}

extern "C" void
ItemDlgChrome_Destroy(ItemDlgChrome *chrome)
{
    if (!chrome)
        return;
    if (chrome->Pane.Control())
        IUnknown_SetSite(chrome->Pane.Control(), NULL);
    if (chrome->Events)
    {
        if (chrome->Pane.Control() && chrome->Cookie)
            chrome->Pane.Control()->TreeUnadvise(chrome->Cookie);
        chrome->Events->Release();
    }
    if (chrome->Pane.Window())
        DestroyWindow(chrome->Pane.Window());
    chrome->Pane.Destroy();
    /* The band windows point at their objects, so they go first. */
    if (chrome->Address)
    {
        if (chrome->Address->Window())
            DestroyWindow(chrome->Address->Window());
        chrome->Address->Release();
    }
    if (chrome->Search)
    {
        if (chrome->Search->Window())
            DestroyWindow(chrome->Search->Window());
        chrome->Search->Release();
    }
    /* Toolbars before the image lists they draw from. */
    HWND bars[] = { chrome->NavBar, chrome->RefreshBar, chrome->Commands, chrome->ViewBar, chrome->HideBar };
    for (UINT i = 0; i < _countof(bars); i++)
    {
        if (bars[i])
            DestroyWindow(bars[i]);
    }
    if (chrome->NavImages)
        ImageList_Destroy(chrome->NavImages);
    if (chrome->NavDisabled)
        ImageList_Destroy(chrome->NavDisabled);
    if (chrome->CommandImages)
        ImageList_Destroy(chrome->CommandImages);
    if (chrome->ChevronImages)
        ImageList_Destroy(chrome->ChevronImages);
    if (chrome->Background)
        DeleteObject(chrome->Background);
    if (chrome->Location)
        ILFree(chrome->Location);
    if (chrome->SearchOrigin)
        ILFree(chrome->SearchOrigin);
    if (chrome->Started)
        ShellFrame::Shutdown();
    delete chrome;
}

static int
AddressRowBottom(ItemDlgChrome *chrome)
{
    return chrome->Scale(8) + chrome->Scale(32) + chrome->Scale(8);
}

/* Two rows on top as in Windows 11, the navigation pane on the left, the browser in the rest. */
extern "C" void
ItemDlgChrome_Layout(ItemDlgChrome *chrome, HDWP *hdwp, const RECT *client, int bottom, const RECT *buttons,
                     RECT *browser)
{
    int width = client->right, row = chrome->Scale(32), top = chrome->Scale(8), left, search, refresh, pane = 0;
    int shown = chrome->Collapsed ? SW_HIDE : SW_SHOW;
    SIZE size;

    size = BarSize(chrome->NavBar);
    Place(hdwp, chrome->NavBar, chrome->Scale(6), top + (row - size.cy) / 2, size.cx, size.cy);
    left = chrome->Scale(12) + size.cx;
    search = min(chrome->Scale(260), max(chrome->Scale(140), (width - chrome->Scale(214)) / 3));
    if (chrome->Search)
        Place(hdwp, chrome->Search->Window(), width - chrome->Scale(10) - search, top, search, row);
    size = BarSize(chrome->RefreshBar);
    refresh = width - chrome->Scale(18) - search - size.cx;
    Place(hdwp, chrome->RefreshBar, refresh, top + (row - size.cy) / 2, size.cx, size.cy);
    if (chrome->Address)
        Place(hdwp, chrome->Address->Window(), left, top, max(0, refresh - chrome->Scale(4) - left), row);

    if (chrome->HideBar && buttons)
    {
        size = BarSize(chrome->HideBar);
        Place(hdwp, chrome->HideBar, chrome->Scale(8), buttons->top + (buttons->bottom - buttons->top - size.cy) / 2,
              size.cx, size.cy);
    }

    ShowWindow(chrome->Commands, shown);
    ShowWindow(chrome->ViewBar, shown);
    if (chrome->Pane.Window())
        ShowWindow(chrome->Pane.Window(), shown);
    if (chrome->Collapsed)
    {
        top = AddressRowBottom(chrome);
        SetRect(browser, 0, top, 0, top);
        return;
    }

    top += row + chrome->Scale(6);
    size = BarSize(chrome->Commands);
    Place(hdwp, chrome->Commands, chrome->Scale(8), top, size.cx, size.cy);
    row = size.cy;
    size = BarSize(chrome->ViewBar);
    Place(hdwp, chrome->ViewBar, width - chrome->Scale(10) - size.cx, top, size.cx, size.cy);
    top += max(row, (int)size.cy) + chrome->Scale(6);

    if (chrome->Pane.Window())
    {
        Place(hdwp, chrome->Pane.Window(), chrome->Scale(2), top, chrome->Scale(170), max(0, bottom - top));
        pane = chrome->Scale(176);
    }
    SetRect(browser, pane ? pane : chrome->Scale(4), top, width - chrome->Scale(4), bottom);
}

extern "C" BOOL
ItemDlgChrome_IsCollapsed(ItemDlgChrome *chrome)
{
    return chrome && chrome->Collapsed;
}

extern "C" int
ItemDlgChrome_TopHeight(ItemDlgChrome *chrome)
{
    return chrome ? AddressRowBottom(chrome) : 0;
}

/* The dialog and its labels take the band color; edit boxes and lists keep their own. */
extern "C" BOOL
ItemDlgChrome_OnColor(ItemDlgChrome *chrome, UINT msg, HDC dc, INT_PTR *result)
{
    if (!chrome || !chrome->Background)
        return FALSE;
    switch (msg)
    {
        case WM_CTLCOLORDLG:
            *result = (INT_PTR)chrome->Background;
            return TRUE;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetTextColor(dc, chrome->Palette.Text);
            SetBkColor(dc, chrome->Palette.Band);
            *result = (INT_PTR)chrome->Background;
            return TRUE;
    }
    return FALSE;
}

extern "C" void
ItemDlgChrome_SettingsChanged(ItemDlgChrome *chrome)
{
    if (!chrome)
        return;
    ApplyTheme(chrome);
    RedrawWindow(chrome->Dialog, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

extern "C" void
ItemDlgChrome_SetLocation(ItemDlgChrome *chrome, PCIDLIST_ABSOLUTE pidl)
{
    WCHAR format[64], cue[MAX_PATH];
    PWSTR name = NULL;

    if (!chrome || !pidl)
        return;
    if (pidl != chrome->Location)
    {
        if (chrome->Location)
            ILFree(chrome->Location);
        chrome->Location = ILClone(pidl);
    }
    if (chrome->Address)
        chrome->Address->SetLocation(pidl);
    if (IsFindFolder(pidl))
    {
        RunPendingSearch(chrome);
        return;
    }
    /* A search whose results folder never showed must not run later. */
    chrome->SearchQuery[0] = UNICODE_NULL;
    if (chrome->Search)
        SetWindowTextW(chrome->Search->EditWindow(), L"");
    chrome->Pane.Select(pidl);
    if (chrome->Search && SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &name)))
    {
        LoadText(IDS_SEARCH_IN, L"Search %s", format, _countof(format));
        StringCchPrintfW(cue, _countof(cue), format, name);
        SendMessageW(chrome->Search->EditWindow(), EM_SETCUEBANNER, FALSE, (LPARAM)cue);
        CoTaskMemFree(name);
    }
    SendMessageW(chrome->NavBar, TB_ENABLEBUTTON, IDC_NAVUP, MAKELONG(!ILIsEmpty(pidl), 0));
}

static void
InvokeVerb(IShellView *view, UINT what, LPCSTR verb, HWND owner)
{
    IContextMenu *menu = NULL;
    CMINVOKECOMMANDINFO info = { sizeof(info) };
    HMENU popup;

    if (FAILED(view->GetItemObject(what, IID_IContextMenu, (void **)&menu)))
        return;
    /* Some handlers expect their menu to exist before a verb is invoked. */
    popup = CreatePopupMenu();
    if (popup)
        menu->QueryContextMenu(popup, 0, 1, 0x7fff, CMF_NORMAL);
    info.hwnd = owner;
    info.lpVerb = verb;
    info.nShow = SW_SHOWNORMAL;
    menu->InvokeCommand(&info);
    if (popup)
        DestroyMenu(popup);
    menu->Release();
}

static HWND
ViewList(IShellView *view)
{
    HWND window = NULL;

    if (FAILED(view->GetWindow(&window)) || !window)
        return NULL;
    return FindWindowExW(window, NULL, WC_LISTVIEWW, NULL);
}

static void
AppendText(HMENU menu, UINT id, UINT text, LPCWSTR fallback, BOOL enabled)
{
    WCHAR buffer[64];

    LoadText(text, fallback, buffer, _countof(buffer));
    AppendMenuW(menu, MF_STRING | (enabled ? 0 : MF_GRAYED), id, buffer);
}

/* Rename the selected item in place in the view list; focus can sit on an unselected item. */
static void
RenameSelection(IShellView *view, HWND owner)
{
    HWND list = ViewList(view);
    int index = list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;

    if (index >= 0)
    {
        SetFocus(list);
        if (ListView_EditLabel(list, index))
            return;
    }
    InvokeVerb(view, SVGIO_SELECTION, "rename", owner);
}

static void
ShowOrganize(ItemDlgChrome *chrome, const RECT *button)
{
    IShellView *view = chrome->Host->get_view(chrome->Context);
    IFolderView *folder = NULL;
    int selected = 0;
    HMENU menu;
    HWND list;
    UINT command;

    if (!view)
        return;
    if (SUCCEEDED(view->QueryInterface(IID_IFolderView, (void **)&folder)))
    {
        folder->ItemCount(SVGIO_SELECTION, &selected);
        folder->Release();
    }
    menu = CreatePopupMenu();
    if (menu)
    {
        AppendText(menu, OrganizeCut, IDS_ORG_CUT, L"Cut", selected > 0);
        AppendText(menu, OrganizeCopy, IDS_ORG_COPY, L"Copy", selected > 0);
        AppendText(menu, OrganizePaste, IDS_ORG_PASTE, L"Paste", TRUE);
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendText(menu, OrganizeSelectAll, IDS_ORG_SELECTALL, L"Select all", TRUE);
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendText(menu, OrganizeDelete, IDS_ORG_DELETE, L"Delete", selected > 0);
        AppendText(menu, OrganizeRename, IDS_ORG_RENAME, L"Rename", selected == 1);
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendText(menu, OrganizeProperties, IDS_ORG_PROPERTIES, L"Properties", TRUE);
        command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, button->left, button->bottom,
                                 0, chrome->Dialog, NULL);
        DestroyMenu(menu);
        switch (command)
        {
            case OrganizeCut: InvokeVerb(view, SVGIO_SELECTION, "cut", chrome->Dialog); break;
            case OrganizeCopy: InvokeVerb(view, SVGIO_SELECTION, "copy", chrome->Dialog); break;
            case OrganizePaste: InvokeVerb(view, SVGIO_BACKGROUND, "paste", chrome->Dialog); break;
            case OrganizeDelete: InvokeVerb(view, SVGIO_SELECTION, "delete", chrome->Dialog); break;
            case OrganizeRename: RenameSelection(view, chrome->Dialog); break;
            case OrganizeProperties:
                InvokeVerb(view, selected ? SVGIO_SELECTION : SVGIO_BACKGROUND, "properties", chrome->Dialog);
                break;
            case OrganizeSelectAll:
                list = ViewList(view);
                if (list)
                {
                    ListView_SetItemState(list, -1, LVIS_SELECTED, LVIS_SELECTED);
                    SetFocus(list);
                }
                break;
        }
    }
    view->Release();
}

static void
ShowViewMenu(ItemDlgChrome *chrome, const RECT *button)
{
    static const struct { UINT Text; LPCWSTR Fallback; FOLDERVIEWMODE Mode; int Size; } Modes[] =
    {
        { IDS_VIEW_LARGE, L"Large icons", FVM_ICON, 96 },
        { IDS_VIEW_MEDIUM, L"Medium icons", FVM_ICON, 48 },
        { IDS_VIEW_SMALL, L"Small icons", FVM_SMALLICON, 16 },
        { IDS_VIEW_LIST, L"List", FVM_LIST, 16 },
        { IDS_VIEW_DETAILS, L"Details", FVM_DETAILS, 16 },
    };
    IShellView *view = chrome->Host->get_view(chrome->Context);
    IFolderView *folder = NULL;
    IFolderView2 *folder2 = NULL;
    UINT current = 0, mode = 0, command;
    HMENU menu;

    if (!view)
        return;
    if (FAILED(view->QueryInterface(IID_IFolderView, (void **)&folder)))
    {
        view->Release();
        return;
    }
    folder->GetCurrentViewMode(&current);
    menu = CreatePopupMenu();
    if (menu)
    {
        for (UINT i = 0; i < _countof(Modes); i++)
        {
            AppendText(menu, i + 1, Modes[i].Text, Modes[i].Fallback, TRUE);
            if (!mode && Modes[i].Mode == current)
                mode = i + 1;
        }
        if (mode)
            CheckMenuRadioItem(menu, 1, _countof(Modes), mode, MF_BYCOMMAND);
        command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, button->right, button->bottom,
                                 0, chrome->Dialog, NULL);
        DestroyMenu(menu);
        if (command)
        {
            command--;
            if (SUCCEEDED(folder->QueryInterface(IID_IFolderView2, (void **)&folder2)))
            {
                folder2->SetViewModeAndIconSize(Modes[command].Mode, Modes[command].Size);
                folder2->Release();
            }
            else
            {
                folder->SetCurrentViewMode(Modes[command].Mode);
            }
        }
    }
    folder->Release();
    view->Release();
}

/* A new folder in the shown folder, named like Explorer does, ready for renaming. */
static void
NewFolder(ItemDlgChrome *chrome)
{
    IShellView *view = chrome->Host->get_view(chrome->Context);
    IFolderView *folder = NULL;
    IPersistFolder2 *persist = NULL;
    PIDLIST_ABSOLUTE current = NULL, created;
    WCHAR dir[MAX_PATH], name[64], path[MAX_PATH];
    BOOL done = FALSE;

    if (!view)
        return;
    if (SUCCEEDED(view->QueryInterface(IID_IFolderView, (void **)&folder)) &&
        SUCCEEDED(folder->GetFolder(IID_IPersistFolder2, (void **)&persist)) &&
        SUCCEEDED(persist->GetCurFolder(&current)) && SHGetPathFromIDListW(current, dir))
    {
        LoadText(IDS_NEW_FOLDER, L"New folder", name, _countof(name));
        for (UINT i = 1; i < 100 && !done; i++)
        {
            if (i == 1)
                StringCchPrintfW(path, _countof(path), L"%s\\%s", dir, name);
            else
                StringCchPrintfW(path, _countof(path), L"%s\\%s (%u)", dir, name, i);
            if (CreateDirectoryW(path, NULL))
                done = TRUE;
            else if (GetLastError() != ERROR_ALREADY_EXISTS)
                break;
        }
        if (done)
        {
            /* The flushed notification lists the folder; refresh only if it is still missing. */
            SHChangeNotify(SHCNE_MKDIR, SHCNF_PATHW | SHCNF_FLUSH, path, NULL);
            created = ILCreateFromPathW(path);
            if (created)
            {
                const UINT flags = SVSI_DESELECTOTHERS | SVSI_SELECT | SVSI_ENSUREVISIBLE | SVSI_FOCUSED | SVSI_EDIT;

                if (FAILED(view->SelectItem(ILFindLastID(created), flags)))
                {
                    view->Refresh();
                    view->SelectItem(ILFindLastID(created), flags);
                }
                ILFree(created);
            }
        }
    }
    if (!done)
        MessageBeep(MB_ICONWARNING);
    if (current)
        ILFree(current);
    if (persist)
        persist->Release();
    if (folder)
        folder->Release();
    view->Release();
}

extern "C" BOOL
ItemDlgChrome_OnCommand(ItemDlgChrome *chrome, UINT id)
{
    IShellView *view;

    if (!chrome)
        return FALSE;
    switch (id)
    {
        case IDC_NAVREFRESH:
            view = chrome->Host->get_view(chrome->Context);
            if (view)
            {
                view->Refresh();
                view->Release();
            }
            return TRUE;
        case IDC_NEWFOLDER:
            NewFolder(chrome);
            return TRUE;
        case IDC_HIDEFOLDERS:
            chrome->Collapsed = !chrome->Collapsed;
            UpdateHideButton(chrome);
            chrome->Host->collapse(chrome->Context, chrome->Collapsed);
            return TRUE;
    }
    return FALSE;
}

extern "C" BOOL
ItemDlgChrome_OnNotify(ItemDlgChrome *chrome, NMHDR *header, LRESULT *result)
{
    if (!chrome || !header)
        return FALSE;
    if (header->hwndFrom != chrome->NavBar && header->hwndFrom != chrome->RefreshBar &&
        header->hwndFrom != chrome->Commands && header->hwndFrom != chrome->ViewBar &&
        header->hwndFrom != chrome->HideBar)
    {
        return FALSE;
    }
    switch (header->code)
    {
        case NM_CUSTOMDRAW:
            *result = ShellFrame::ToolbarCustomDraw((NMTBCUSTOMDRAW *)header, chrome->Palette.Band);
            return TRUE;
        case TBN_DROPDOWN:
        {
            NMTOOLBARW *toolbar = (NMTOOLBARW *)header;
            RECT button;

            SendMessageW(header->hwndFrom, TB_GETRECT, toolbar->iItem, (LPARAM)&button);
            MapWindowPoints(header->hwndFrom, NULL, (POINT *)&button, 2);
            if (toolbar->iItem == IDC_ORGANIZE)
                ShowOrganize(chrome, &button);
            else if (toolbar->iItem == IDC_VIEWMENU)
                ShowViewMenu(chrome, &button);
            *result = TBDDRET_DEFAULT;
            return TRUE;
        }
    }
    return FALSE;
}

/* The browser serves SID_SShellBrowser, so opening a pane item navigates the dialog, not a new window. */
extern "C" void
ItemDlgChrome_SetBrowser(ItemDlgChrome *chrome, IUnknown *browser)
{
    if (chrome && chrome->Pane.Control())
        IUnknown_SetSite(chrome->Pane.Control(), browser);
}

extern "C" void
ItemDlgChrome_Unregister(void)
{
    ShellFrame::UnregisterBandClasses(COMDLG32_hInstance);
}
