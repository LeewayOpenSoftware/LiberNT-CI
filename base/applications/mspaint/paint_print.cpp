/*
 * PROJECT:    PAINT for ReactOS (Leeway ribbon build)
 * LICENSE:    LGPL-2.0-or-later
 * PURPOSE:    Print, page setup and print preview through the shared LeewayUI print path
 *             (one page: the image at actual size, shrunk to fit the margin box, centred)
 */
#include "precomp.h"
#include "paint_print.h"
#include "../../../sdk/include/leewayui/print.h"
#include "../../../sdk/include/leewayui/aureline_icons.h"

static ILeewayPrintPreview *s_pv = NULL;
static LeewayPrintSettings s_settings;
static BOOL s_settingsInit = FALSE;
static HWND s_hwndMain = NULL;
static WCHAR s_iconRoot[MAX_PATH];
static LeewayAurelineConfig s_iconCfg;

static void EnsureSettings()
{
    if (!s_settingsInit)
    {
        LeewayPrintSettingsInitDefault(&s_settings);
        s_settingsInit = TRUE;
    }
}

/* Draw the image into the margin box of a device with the given dpi */
static void DrawImagePage(HDC hdc, const RECT *content, UINT dpi)
{
    int iw = imageModel.GetWidth(), ih = imageModel.GetHeight();
    if (iw <= 0 || ih <= 0 || dpi == 0)
        return;
    int cw = content->right - content->left, ch = content->bottom - content->top;
    UINT scale = s_settings.scale_percent ? s_settings.scale_percent : 100;
    BOOL fit = (s_settings.fit_pages_x || s_settings.fit_pages_y);
    LONGLONG w = (LONGLONG)MulDiv(iw, dpi, 96) * scale / 100, h = (LONGLONG)MulDiv(ih, dpi, 96) * scale / 100;
    if (fit)
    {
        /* fit to the page: scale up or down to the margin box */
        w = MulDiv(iw, dpi, 96);
        h = MulDiv(ih, dpi, 96);
        if (w * ch > h * cw)
        {
            h = h * cw / w;
            w = cw;
        }
        else
        {
            w = w * ch / h;
            h = ch;
        }
    }
    else if (w > cw || h > ch)
    {
        /* shrink to fit, keep the aspect ratio */
        if (w * ch > h * cw)
        {
            h = h * cw / w;
            w = cw;
        }
        else
        {
            w = w * ch / h;
            h = ch;
        }
    }
    if (w < 1 || h < 1)
        return;
    int x = content->left + (s_settings.center_h ? (cw - (int)w) / 2 : 0);
    int y = content->top + (s_settings.center_v ? (ch - (int)h) / 2 : 0);
    int old = ::SetStretchBltMode(hdc, HALFTONE);
    ::SetBrushOrgEx(hdc, 0, 0, NULL);
    ::StretchBlt(hdc, x, y, (int)w, (int)h, imageModel.GetDC(), 0, 0, iw, ih, SRCCOPY);
    ::SetStretchBltMode(hdc, old);
}

static UINT32 PvPageCount(void *) { return 1; }

static HRESULT PvRender(UINT32, HDC hdc, const RECT *, const RECT *content, UINT32 dpi, void *)
{
    DrawImagePage(hdc, content, dpi);
    return S_OK;
}

void PaintPrintPageSetup(HWND hwndMain)
{
    EnsureSettings();
    if (LeewayPageSetupDialog(hwndMain, &s_settings) == S_OK && s_pv)
        LeewayPrintPreviewRefresh(s_pv);
}

void PaintPrintNow(HWND hwndMain)
{
    EnsureSettings();
    LeewayPrintRange range = { 0 };
    range.min_page = 1;
    range.max_page = 1;
    range.copies = 1;
    HDC hdcPrn = NULL;
    if (LeewayPrintDialog(hwndMain, &s_settings, &range, &hdcPrn) != S_OK || !hdcPrn)
        return;
    UINT dpi = ::GetDeviceCaps(hdcPrn, LOGPIXELSX);
    RECT margin = { 0 };
    LeewayPrintSettingsGetPaper(&s_settings, dpi, NULL, NULL, &margin);
    DOCINFOW di = { sizeof(di) };
    di.lpszDocName = g_szFileName[0] ? PathFindFileNameW(g_szFileName) : L"Paint";
    UINT copies = range.copies ? range.copies : 1;
    if (::StartDocW(hdcPrn, &di) > 0)
    {
        for (UINT i = 0; i < copies; i++)
        {
            if (::StartPage(hdcPrn) <= 0)
                break;
            DrawImagePage(hdcPrn, &margin, dpi);
            ::EndPage(hdcPrn);
        }
        ::EndDoc(hdcPrn);
    }
    ::DeleteDC(hdcPrn);
}

static void PvAction(LeewayPreviewAction action, void *)
{
    switch (action)
    {
    case LWUI_PREVIEW_PRINT:
        PaintPrintNow(s_hwndMain);
        break;
    case LWUI_PREVIEW_PAGESETUP:
        PaintPrintPageSetup(s_hwndMain);
        break;
    case LWUI_PREVIEW_CLOSE:
        LeewayPrintPreviewShow(s_pv, FALSE);
        mainWindow.ShowChromeForPreview(TRUE);
        mainWindow.alignChildrenToMainWindow();
        break;
    }
}

void PaintPrintPreview(HWND hwndMain)
{
    EnsureSettings();
    s_hwndMain = hwndMain;
    if (!s_pv)
    {
        LeewayPrintPreviewConfig cfg = { 0 };
        cfg.page_count = PvPageCount;
        cfg.render_page = PvRender;
        cfg.on_action = PvAction;
        cfg.settings = &s_settings;
        cfg.app_name = L"Paint";
        cfg.theme = LEEWAY_THEME_PREF_SYSTEM; /* app_name carries Paint's Theme override */
        if (FAILED(LeewayPrintPreviewCreate(hwndMain, &cfg, &s_pv)))
        {
            s_pv = NULL;
            return;
        }
        if (GetModuleFileNameW(NULL, s_iconRoot, _countof(s_iconRoot)))
        {
            WCHAR *pSlash = wcsrchr(s_iconRoot, L'\\');
            if (pSlash && (size_t)(pSlash - s_iconRoot) + 7 < _countof(s_iconRoot))
            {
                wcscpy(pSlash + 1, L"icons");
                s_iconCfg.root = s_iconRoot;
                s_iconCfg.mono_fallback_to_colour = TRUE;
                LeewayPrintPreviewSetIconResolver(s_pv, LeewayRibbonAurelineResolver, &s_iconCfg);
            }
        }
    }
    canvasWindow.OnEndDraw(FALSE); /* commit a pending selection so it is printed */
    mainWindow.ShowChromeForPreview(FALSE);
    PaintPrintLayout(hwndMain);
    LeewayPrintPreviewShow(s_pv, TRUE);
}

BOOL PaintPrintIsActive(void)
{
    return s_pv && LeewayPrintPreviewIsActive(s_pv);
}

void PaintPrintLayout(HWND hwndMain)
{
    if (!s_pv)
        return;
    RECT rc;
    ::GetClientRect(hwndMain, &rc);
    ::MoveWindow(LeewayPrintPreviewGetHWND(s_pv), 0, 0, rc.right, rc.bottom, TRUE);
}

void PaintPrintSetDPI(UINT dpi)
{
    if (s_pv)
        LeewayPrintPreviewSetDPI(s_pv, dpi);
}

void PaintPrintThemeChanged(void)
{
    if (s_pv)
        LeewayPrintPreviewOnThemeChanged(s_pv);
}

BOOL PaintPrintPreTranslate(MSG *pMsg)
{
    return PaintPrintIsActive() && LeewayPrintPreviewPreTranslateMessage(s_pv, pMsg);
}

void PaintPrintCleanup(void)
{
    if (s_pv)
    {
        LeewayPrintPreviewDestroy(s_pv);
        s_pv = NULL;
    }
    if (s_settingsInit)
    {
        LeewayPrintSettingsFree(&s_settings);
        s_settingsInit = FALSE;
    }
}
