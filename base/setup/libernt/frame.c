/*
 * PROJECT:     LiberNT Setup
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Window shared by Setup and the first-boot setup
 * COPYRIGHT:   Copyright 2018-2024 Hermès Bélusca-Maïto <hermes.belusca-maito@reactos.org>
 *              Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "libernt.h"

#include "resource.h"

#define NDEBUG
#include <debug.h>

typedef struct _SETUP_FRAME
{
    PSETUPDATA SetupData;
    HWND hwnd;
    const SETUP_PAGE_INFO* PageInfo;
    INT PageCount;
    HWND Pages[SETUP_MAX_PAGES];
    INT Current;
    INT First;
    INT Restart;
    COLORREF TitleColor;
    COLORREF SubtitleColor;
} SETUP_FRAME;

static SETUP_FRAME Frame;

static const WCHAR szSetupFramePageProc[] = L"SetupFramePageProc";

static LRESULT CALLBACK
FramePageWndProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam);

VOID
LoadPrintfW(
    _Out_writes_(cchText) PWSTR Text,
    _In_ SIZE_T cchText,
    _In_ UINT uID,
    ...)
{
    WCHAR Format[512];
    va_list Args;

    Text[0] = UNICODE_NULL;
    if (!LoadStringW(GetModuleHandleW(NULL), uID, Format, ARRAYSIZE(Format)))
        return;

    va_start(Args, uID);
    StringCchVPrintfW(Text, cchText, Format, Args);
    va_end(Args);
}

INT
SetupMessage(
    _In_opt_ HWND hwndOwner,
    _In_opt_ PCWSTR Icon,
    _In_ TASKDIALOG_COMMON_BUTTON_FLAGS Buttons,
    _In_ INT DefaultButton,
    _In_opt_ PCWSTR MainInstruction,
    _In_opt_ PCWSTR Content)
{
    TASKDIALOGCONFIG Config;
    INT Button = IDCANCEL;

    ZeroMemory(&Config, sizeof(Config));
    Config.cbSize = sizeof(Config);
    Config.hwndParent = hwndOwner;
    Config.hInstance = GetModuleHandleW(NULL);
    Config.dwFlags = hwndOwner ? TDF_POSITION_RELATIVE_TO_WINDOW : 0;
    Config.dwCommonButtons = Buttons;
    Config.pszWindowTitle = MAKEINTRESOURCEW(IDS_CAPTION);
    Config.pszMainIcon = Icon;
    Config.pszMainInstruction = MainInstruction;
    Config.pszContent = Content;
    Config.nDefaultButton = DefaultButton;

    if (FAILED(TaskDialogIndirect(&Config, &Button, NULL, NULL)))
        return IDCANCEL;
    return Button;
}

static HFONT
CreateFrameFont(
    _In_ INT PointSize)
{
    NONCLIENTMETRICSW Metrics;
    LOGFONTW Font;
    HDC hdc;

    Metrics.cbSize = sizeof(Metrics);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(Metrics), &Metrics, 0))
        return NULL;

    Font = Metrics.lfMessageFont;
    Font.lfWeight = FW_BOLD;
    if (PointSize)
    {
        hdc = GetDC(NULL);
        Font.lfHeight = -MulDiv(PointSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
        ReleaseDC(NULL, hdc);
    }
    return CreateFontIndirectW(&Font);
}

VOID
SetResText(
    _In_ HWND hWnd,
    _In_ UINT uID)
{
    WCHAR Text[512];

    Text[0] = UNICODE_NULL;
    if (uID)
        LoadStringW(Frame.SetupData->hInstance, uID, Text, ARRAYSIZE(Text));
    SetWindowTextW(hWnd, Text);
}

VOID
EnableSetupCancel(
    _In_ HWND hFrame,
    _In_ BOOL Enable)
{
    EnableDlgItem(hFrame, IDCANCEL, Enable);
    EnableMenuItem(GetSystemMenu(hFrame, FALSE),
                   SC_CLOSE,
                   MF_BYCOMMAND | (Enable ? MF_ENABLED : MF_GRAYED));
}

VOID
SetupFrameSetButtons(
    _In_ HWND hPage,
    _In_ BOOL Back,
    _In_ BOOL Next)
{
    HWND hFrame = GetParent(hPage);

    EnableDlgItem(hFrame, IDC_FRAME_BACK, Back);
    EnableDlgItem(hFrame, IDC_FRAME_NEXT, Next);
}

VOID
SetupFrameShowButtons(
    _In_ HWND hPage,
    _In_ BOOL Back,
    _In_ BOOL Next,
    _In_ BOOL Cancel)
{
    HWND hFrame = GetParent(hPage);

    ShowDlgItem(hFrame, IDC_FRAME_BACK, Back ? SW_SHOW : SW_HIDE);
    ShowDlgItem(hFrame, IDC_FRAME_NEXT, Next ? SW_SHOW : SW_HIDE);
    ShowDlgItem(hFrame, IDCANCEL, Cancel ? SW_SHOW : SW_HIDE);
}

VOID
SetupFrameSetButtonText(
    _In_ HWND hPage,
    _In_ INT ButtonId,
    _In_ UINT uID)
{
    SetResText(GetDlgItem(GetParent(hPage), ButtonId), uID);
}

VOID
SetupFrameSetTitle(
    _In_ HWND hPage,
    _In_ UINT Title,
    _In_ UINT Subtitle)
{
    HWND hFrame = GetParent(hPage);

    SetResText(GetDlgItem(hFrame, IDC_FRAME_TITLE), Title);
    SetResText(GetDlgItem(hFrame, IDC_FRAME_SUBTITLE), Subtitle);
}

VOID
SetupFramePressNext(
    _In_ HWND hPage)
{
    HWND hFrame = GetParent(hPage);

    PostMessageW(hFrame, WM_COMMAND, MAKEWPARAM(IDC_FRAME_NEXT, BN_CLICKED),
                 (LPARAM)GetDlgItem(hFrame, IDC_FRAME_NEXT));
}

static VOID
FrameShowPage(
    _In_ INT Page)
{
    HWND hPage = Frame.Pages[Page];
    BOOL Back = (Page > Frame.First) && (Frame.PageInfo[Page].Flags & SETUP_PAGE_BACK);
    BOOL Cancel = !(Frame.PageInfo[Page].Flags & SETUP_PAGE_NO_CANCEL);

    if (Frame.Current >= 0)
    {
        SendMessageW(Frame.Pages[Frame.Current], SPN_KILLACTIVE, 0, 0);
        ShowWindow(Frame.Pages[Frame.Current], SW_HIDE);
    }

    Frame.Current = Page;
    SetupFrameSetTitle(hPage, Frame.PageInfo[Page].Title, Frame.PageInfo[Page].Subtitle);
    SetupFrameSetButtonText(hPage, IDC_FRAME_BACK, IDS_BACKBTN);
    SetupFrameSetButtonText(hPage, IDC_FRAME_NEXT, IDS_NEXTBTN);
    SetupFrameSetButtonText(hPage, IDCANCEL, IDS_CANCELBTN);
    SetupFrameShowButtons(hPage, Back, TRUE, Cancel);
    SetupFrameSetButtons(hPage, Back, TRUE);
    EnableSetupCancel(Frame.hwnd, Cancel);

    ShowWindow(hPage, SW_SHOW);
    SendMessageW(hPage, SPN_SETACTIVE, 0, 0);
}

VOID
SetupFrameGoTo(
    _In_ HWND hPage,
    _In_ INT Page)
{
    UNREFERENCED_PARAMETER(hPage);
    FrameShowPage(Page);
}

static BOOL
FrameSkipPage(
    _In_ INT Page)
{
    if (Frame.SetupData->bUnattend && (Frame.PageInfo[Page].Flags & SETUP_PAGE_SKIP_UNATTEND))
        return TRUE;
    if (Frame.SetupData->bUpgrade && (Frame.PageInfo[Page].Flags & SETUP_PAGE_SKIP_UPGRADE))
        return TRUE;
    return (Frame.PageInfo[Page].Flags & SETUP_PAGE_OPTIONAL) &&
           SendMessageW(Frame.Pages[Page], SPN_QUERYSKIP, 0, 0);
}

static INT
FrameNextPage(
    _In_ INT Page)
{
    while (Page < Frame.PageCount && FrameSkipPage(Page))
        Page++;
    return Page;
}

static VOID
FrameNext(VOID)
{
    HWND hNext = GetDlgItem(Frame.hwnd, IDC_FRAME_NEXT);
    INT Current = Frame.Current;
    INT Page;

    if (!IsWindowVisible(hNext) || !IsWindowEnabled(hNext))
        return;

    if (SendMessageW(Frame.Pages[Current], SPN_WIZNEXT, 0, 0) == -1 || Frame.Current != Current)
        return;

    Page = FrameNextPage(Current + 1);
    if (Page < Frame.PageCount)
        FrameShowPage(Page);
}

static VOID
FrameBack(VOID)
{
    HWND hBack = GetDlgItem(Frame.hwnd, IDC_FRAME_BACK);
    INT Current = Frame.Current;
    INT Page;

    if (!IsWindowVisible(hBack) || !IsWindowEnabled(hBack))
        return;

    if (SendMessageW(Frame.Pages[Current], SPN_WIZBACK, 0, 0) == -1 || Frame.Current != Current)
        return;

    Page = Current - 1;
    while (Page > Frame.First && FrameSkipPage(Page))
        Page--;
    if (Page >= Frame.First)
        FrameShowPage(Page);
}

static VOID
FrameCancel(VOID)
{
    if (!IsWindowEnabled(GetDlgItem(Frame.hwnd, IDCANCEL)))
        return;

    if (Frame.PageInfo[Frame.Current].Flags & SETUP_PAGE_CLOSE)
    {
        EndDialog(Frame.hwnd, IDCANCEL);
        return;
    }

    if (SendMessageW(Frame.Pages[Frame.Current], SPN_QUERYCANCEL, 0, 0))
        return;

    if (SetupMessage(Frame.hwnd, TD_WARNING_ICON, TDCBF_YES_BUTTON | TDCBF_NO_BUTTON, IDNO,
                     MAKEINTRESOURCEW(IDS_ABORTSETUP2), MAKEINTRESOURCEW(IDS_ABORTSETUP)) == IDYES)
    {
        EndDialog(Frame.hwnd, IDCANCEL);
    }
}

static VOID
FrameCenter(
    _In_ HWND hwnd)
{
    RECT rcWork, rcWindow;

    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    GetWindowRect(hwnd, &rcWindow);
    SetWindowPos(hwnd, HWND_TOP,
                 rcWork.left + ((rcWork.right - rcWork.left) - (rcWindow.right - rcWindow.left)) / 2,
                 rcWork.top + ((rcWork.bottom - rcWork.top) - (rcWindow.bottom - rcWindow.top)) / 2,
                 0, 0, SWP_NOSIZE);
}

/* White content painting for a page window; the frame applies this to every
   page it creates so the setup looks the same with and without themes. */
static LRESULT CALLBACK
FramePageWndProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    WNDPROC OldProc = (WNDPROC)GetPropW(hwnd, szSetupFramePageProc);

    switch (uMsg)
    {
        case WM_ERASEBKGND:
        {
            RECT rc;
            HDC hdc = (HDC)wParam;

            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
            return 1;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
        {
            HDC hdc = (HDC)wParam;

            SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
    }

    if (OldProc)
        return CallWindowProcW(OldProc, hwnd, uMsg, wParam, lParam);
    return 0;
}

/* Flat back arrow for the title area, per the Aero wizard chrome. */
static VOID
FrameDrawBackButton(
    _In_ LPDRAWITEMSTRUCT lpDrawItem)
{
    HDC hdc = lpDrawItem->hDC;
    RECT rc = lpDrawItem->rcItem;
    INT cx = (rc.left + rc.right) / 2;
    INT cy = (rc.top + rc.bottom) / 2;
    HBRUSH hbrArrow;
    POINT Arrow[3];
    HGDIOBJ hOld, hOldPen;

    FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));

    if (lpDrawItem->itemState & ODS_SELECTED)
    {
        HBRUSH hbrHover = CreateSolidBrush(RGB(0xE5, 0xE5, 0xE5));

        if (hbrHover)
        {
            FillRect(hdc, &rc, hbrHover);
            DeleteObject(hbrHover);
        }
    }

    if (lpDrawItem->itemState & ODS_DISABLED)
        hbrArrow = GetSysColorBrush(COLOR_GRAYTEXT);
    else
        hbrArrow = GetSysColorBrush(COLOR_WINDOWTEXT);

    Arrow[0].x = cx - 1;
    Arrow[0].y = cy - 5;
    Arrow[1].x = cx - 7;
    Arrow[1].y = cy;
    Arrow[2].x = cx - 1;
    Arrow[2].y = cy + 5;
    hOld = SelectObject(hdc, hbrArrow);
    hOldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    SetPolyFillMode(hdc, WINDING);
    Polygon(hdc, Arrow, ARRAYSIZE(Arrow));
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOld);
}

static INT_PTR CALLBACK
SetupFrameProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            PSETUPDATA pSetupData = (PSETUPDATA)lParam;
            HICON hIcon;
            RECT rc;
            INT Page;

            Frame.SetupData = pSetupData;
            Frame.hwnd = hwnd;
            Frame.Current = -1;

            Frame.TitleColor = GetSysColor(COLOR_WINDOWTEXT);
            Frame.SubtitleColor = GetSysColor(COLOR_GRAYTEXT);

            hIcon = LoadIconW(pSetupData->hInstance, MAKEINTRESOURCEW(IDI_MAIN));
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            if (!pSetupData->hTitleFont)
                pSetupData->hTitleFont = CreateFrameFont(12);
            if (!pSetupData->hBoldFont)
                pSetupData->hBoldFont = CreateFrameFont(0);
            SetDlgItemFont(hwnd, IDC_FRAME_TITLE, pSetupData->hTitleFont, FALSE);

            GetWindowRect(GetDlgItem(hwnd, IDC_FRAME_PAGE), &rc);
            MapWindowPoints(HWND_DESKTOP, hwnd, (PPOINT)&rc, 2);
            for (Page = 0; Page < Frame.PageCount; Page++)
            {
                Frame.Pages[Page] = CreateDialogParamW(pSetupData->hInstance,
                                                       MAKEINTRESOURCEW(Frame.PageInfo[Page].Template),
                                                       hwnd,
                                                       Frame.PageInfo[Page].Proc,
                                                       (LPARAM)pSetupData);
                if (!Frame.Pages[Page])
                {
                    EndDialog(hwnd, IDABORT);
                    return FALSE;
                }
                SetWindowPos(Frame.Pages[Page], HWND_TOP, rc.left, rc.top,
                             rc.right - rc.left, rc.bottom - rc.top, SWP_NOACTIVATE);
                SetPropW(Frame.Pages[Page], szSetupFramePageProc,
                         (HANDLE)SetWindowLongPtrW(Frame.Pages[Page],
                                                   GWLP_WNDPROC,
                                                   (LONG_PTR)FramePageWndProc));
            }

            FrameCenter(hwnd);
            Frame.First = FrameNextPage(0);
            Page = (Frame.Restart >= 0) ? Frame.Restart : Frame.First;
            Frame.Restart = -1;
            FrameShowPage(Page);
            return FALSE;
        }

        case WM_ERASEBKGND:
        {
            RECT rc;
            HDC hdc = (HDC)wParam;

            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
            return TRUE;
        }

        case WM_DRAWITEM:
        {
            LPDRAWITEMSTRUCT lpDrawItem = (LPDRAWITEMSTRUCT)lParam;

            if (lpDrawItem->CtlID == IDC_FRAME_BACK)
            {
                FrameDrawBackButton(lpDrawItem);
                return TRUE;
            }
            break;
        }

        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = (HDC)wParam;
            HWND hwndControl = (HWND)lParam;

            SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
            if (GetDlgCtrlID(hwndControl) == IDC_FRAME_TITLE)
                SetTextColor(hdc, Frame.TitleColor);
            else if (GetDlgCtrlID(hwndControl) == IDC_FRAME_SUBTITLE)
                SetTextColor(hdc, Frame.SubtitleColor);
            return (INT_PTR)GetSysColorBrush(COLOR_WINDOW);
        }

        case WM_ACTIVATE:
            if (Frame.Current >= 0)
                SendMessageW(Frame.Pages[Frame.Current], WM_ACTIVATE, wParam, lParam);
            break;

        case WM_SYSCOLORCHANGE:
            Frame.TitleColor = GetSysColor(COLOR_WINDOWTEXT);
            Frame.SubtitleColor = GetSysColor(COLOR_GRAYTEXT);
            InvalidateRect(hwnd, NULL, TRUE);
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDOK:
                case IDC_FRAME_NEXT:
                    FrameNext();
                    return TRUE;
                case IDC_FRAME_BACK:
                    FrameBack();
                    return TRUE;
                case IDCANCEL:
                    FrameCancel();
                    return TRUE;
            }
            break;

        case WM_DESTROY:
            if (Frame.SetupData->hTitleFont)
                DeleteObject(Frame.SetupData->hTitleFont);
            if (Frame.SetupData->hBoldFont)
                DeleteObject(Frame.SetupData->hBoldFont);
            Frame.SetupData->hTitleFont = NULL;
            Frame.SetupData->hBoldFont = NULL;
            break;
    }

    return FALSE;
}

VOID
SetupFrameReload(
    _In_ HWND hPage)
{
    UNREFERENCED_PARAMETER(hPage);
    Frame.Restart = Frame.Current;
    EndDialog(Frame.hwnd, IDRETRY);
}

VOID
SetSetupLanguage(
    _In_ PCWSTR LocaleId)
{
    LANGID LangId = LANGIDFROMLCID(wcstoul(LocaleId, NULL, 16));

    if (LangId && SetThreadUILanguage(LangId) != LangId)
        DPRINT1("SetThreadUILanguage(%04x) failed, error %lu\n", LangId, GetLastError());
}

INT_PTR
RunSetupFrame(
    _In_ PSETUPDATA pSetupData,
    _In_reads_(PageCount) const SETUP_PAGE_INFO* PageInfo,
    _In_ INT PageCount)
{
    INT_PTR Result;

    Frame.PageInfo = PageInfo;
    Frame.PageCount = min(PageCount, SETUP_MAX_PAGES);
    Frame.Restart = -1;
    do
    {
        Result = DialogBoxParamW(pSetupData->hInstance, MAKEINTRESOURCEW(IDD_SETUPFRAME),
                                 NULL, SetupFrameProc, (LPARAM)pSetupData);
    } while (Result == IDRETRY);
    return Result;
}
