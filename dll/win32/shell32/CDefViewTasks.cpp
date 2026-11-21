/*
 * PROJECT:     LiberNT shell32
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Folder task pane: the column of links on the left of a folder view.
 *              A folder publishes its links by returning an IExplorerCommandProvider
 *              from IShellFolder::CreateViewObject; each IExplorerCommand is one link.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

#define TASKPANE_CLASS L"LiberNT_DefViewTaskPane"
#define TASKPANE_MAX_LINKS 32
#define TASKPANE_RELOAD (WM_USER + 0x100)

class CDefViewTaskPane :
    public CWindowImpl<CDefViewTaskPane, CWindow, CControlWinTraits>
{
    struct TASKLINK
    {
        CComPtr<IExplorerCommand> pCommand;
        CStringW strTitle;
        BOOL bEnabled;
        RECT rc;
    };

    CComPtr<IExplorerCommandProvider> m_pProvider;
    CComPtr<IUnknown> m_pSite;
    CAtlArray<TASKLINK> m_Links;
    HFONT m_hFont = NULL;
    HFONT m_hFontHot = NULL;
    int m_Dpi = 96;
    int m_Hot = -1;
    int m_Focus = 0;
    BOOL m_bTracking = FALSE;

    int Scale(int px) const { return MulDiv(px, m_Dpi, 96); }

    void FreeFonts()
    {
        if (m_hFont)
            DeleteObject(m_hFont);
        if (m_hFontHot)
            DeleteObject(m_hFontHot);
        m_hFont = m_hFontHot = NULL;
    }

    void CreateFonts()
    {
        LOGFONTW lf;
        FreeFonts();
        HDC hdc = ::GetDC(m_hWnd);
        m_Dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
        if (hdc)
            ::ReleaseDC(m_hWnd, hdc);
        /* the font of the view's items */
        if (!SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0))
            GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
        m_hFont = CreateFontIndirectW(&lf);
        lf.lfUnderline = TRUE;
        m_hFontHot = CreateFontIndirectW(&lf);
    }

public:
    /* Ask the provider again; titles and states may have changed. S_FALSE: nothing to show */
    HRESULT LoadLinks()
    {
        CComPtr<IEnumExplorerCommand> pEnum;
        CComPtr<IExplorerCommand> pCommand;

        m_Links.SetCount(0);
        m_Hot = -1;
        m_Focus = 0;
        HRESULT hr = m_pProvider->GetCommands(m_pSite, IID_PPV_ARG(IEnumExplorerCommand, &pEnum));
        if (FAILED(hr))
            return hr;
        while (m_Links.GetCount() < TASKPANE_MAX_LINKS && pEnum->Next(1, &pCommand, NULL) == S_OK)
        {
            EXPCMDSTATE state = ECS_ENABLED;
            CComHeapPtr<WCHAR> pszTitle;
            if (FAILED(pCommand->GetState(NULL, TRUE, &state)))
                state = ECS_ENABLED;
            if (!(state & ECS_HIDDEN) && SUCCEEDED(pCommand->GetTitle(NULL, &pszTitle)) && pszTitle && *pszTitle)
            {
                TASKLINK link;
                link.pCommand = pCommand;
                link.strTitle = pszTitle;
                link.bEnabled = !(state & ECS_DISABLED);
                SetRectEmpty(&link.rc);
                m_Links.Add(link);
            }
            pCommand.Release();
        }
        return m_Links.GetCount() ? S_OK : S_FALSE;
    }

    HRESULT Initialize(IExplorerCommandProvider *pProvider, IUnknown *pSite)
    {
        m_pProvider = pProvider;
        m_pSite = pSite;
        return LoadLinks();
    }

    void Layout()
    {
        RECT rcClient;
        GetClientRect(&rcClient);
        HDC hdc = ::GetDC(m_hWnd);
        if (!hdc)
            return;
        HGDIOBJ hOld = SelectObject(hdc, m_hFont);
        int x = Scale(16), y = Scale(16), cx = max(rcClient.right - x - Scale(12), Scale(40));
        for (size_t i = 0; i < m_Links.GetCount(); ++i)
        {
            RECT rc = { 0, 0, cx, 0 };
            DrawTextW(hdc, m_Links[i].strTitle, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            SetRect(&m_Links[i].rc, x, y, x + min((int)(rc.right - rc.left), cx), y + (rc.bottom - rc.top));
            y = m_Links[i].rc.bottom + Scale(10);
        }
        SelectObject(hdc, hOld);
        ::ReleaseDC(m_hWnd, hdc);
    }

    int HitTest(POINT pt) const
    {
        for (size_t i = 0; i < m_Links.GetCount(); ++i)
        {
            RECT rc = m_Links[i].rc;
            InflateRect(&rc, Scale(2), Scale(2));
            if (PtInRect(&rc, pt))
                return (int)i;
        }
        return -1;
    }

    void InvalidateLink(int i)
    {
        if (i >= 0 && i < (int)m_Links.GetCount())
        {
            RECT rc = m_Links[i].rc;
            InflateRect(&rc, Scale(4), Scale(4));
            InvalidateRect(&rc, TRUE);
        }
    }

    void SetHot(int i)
    {
        if (i == m_Hot)
            return;
        InvalidateLink(m_Hot);
        m_Hot = i;
        InvalidateLink(m_Hot);
    }

    void InvokeLink(int i)
    {
        if (i < 0 || i >= (int)m_Links.GetCount() || !m_Links[i].bEnabled)
            return;
        CComPtr<IExplorerCommand> pCommand = m_Links[i].pCommand;
        IUnknown_SetSite(pCommand, m_pSite);
        HRESULT hr = pCommand->Invoke(NULL, NULL);
        IUnknown_SetSite(pCommand, NULL);
        if (FAILED(hr))
            WARN("task %d failed: 0x%08lx\n", i, hr);
    }

    void Paint(HDC hdc)
    {
        RECT rcClient;
        GetClientRect(&rcClient);
        FillRect(hdc, &rcClient, GetSysColorBrush(COLOR_WINDOW));
        RECT rcLine = { rcClient.right - 1, rcClient.top, rcClient.right, rcClient.bottom };
        FillRect(hdc, &rcLine, GetSysColorBrush(COLOR_3DLIGHT));

        SetBkMode(hdc, TRANSPARENT);
        HGDIOBJ hOld = SelectObject(hdc, m_hFont);
        for (size_t i = 0; i < m_Links.GetCount(); ++i)
        {
            const TASKLINK &link = m_Links[i];
            RECT rc = link.rc;
            SelectObject(hdc, ((int)i == m_Hot && link.bEnabled) ? m_hFontHot : m_hFont);
            SetTextColor(hdc, GetSysColor(link.bEnabled ? COLOR_HOTLIGHT : COLOR_GRAYTEXT));
            DrawTextW(hdc, link.strTitle, -1, &rc, DT_WORDBREAK | DT_NOPREFIX);
            if ((int)i == m_Focus && ::GetFocus() == m_hWnd)
            {
                InflateRect(&rc, Scale(2), Scale(1));
                DrawFocusRect(hdc, &rc);
            }
        }
        SelectObject(hdc, hOld);
    }

    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL &)
    {
        CreateFonts();
        return 0;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL &bHandled)
    {
        FreeFonts();
        m_Links.SetCount(0);
        m_pProvider.Release();
        m_pSite.Release();
        bHandled = FALSE;
        return 0;
    }

    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL &)
    {
        Layout();
        Invalidate(TRUE);
        return 0;
    }

    LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL &)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(&ps);
        if (hdc)
            Paint(hdc);
        EndPaint(&ps);
        return 0;
    }

    LRESULT OnPrintClient(UINT, WPARAM wParam, LPARAM, BOOL &)
    {
        Paint((HDC)wParam);
        return 0;
    }

    LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL &)
    {
        return 1;
    }

    LRESULT OnMouseMove(UINT, WPARAM, LPARAM lParam, BOOL &)
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        SetHot(HitTest(pt));
        if (!m_bTracking)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hWnd, 0 };
            m_bTracking = TrackMouseEvent(&tme);
        }
        return 0;
    }

    LRESULT OnMouseLeave(UINT, WPARAM, LPARAM, BOOL &)
    {
        m_bTracking = FALSE;
        SetHot(-1);
        return 0;
    }

    LRESULT OnSetCursor(UINT, WPARAM, LPARAM, BOOL &bHandled)
    {
        if (m_Hot >= 0 && m_Links[m_Hot].bEnabled)
        {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        bHandled = FALSE;
        return 0;
    }

    LRESULT OnLButtonDown(UINT, WPARAM, LPARAM lParam, BOOL &)
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int i = HitTest(pt);
        if (i >= 0)
        {
            InvalidateLink(m_Focus);
            m_Focus = i;
            SetFocus();
            InvalidateLink(m_Focus);
        }
        return 0;
    }

    LRESULT OnLButtonUp(UINT, WPARAM, LPARAM lParam, BOOL &)
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int i = HitTest(pt);
        if (i >= 0 && i == m_Focus)
            InvokeLink(i);
        return 0;
    }

    LRESULT OnKeyDown(UINT, WPARAM wParam, LPARAM, BOOL &bHandled)
    {
        int count = (int)m_Links.GetCount();
        if (!count)
            return 0;
        switch (wParam)
        {
            case VK_UP:
            case VK_DOWN:
                InvalidateLink(m_Focus);
                m_Focus = (m_Focus + (wParam == VK_DOWN ? 1 : count - 1)) % count;
                InvalidateLink(m_Focus);
                return 0;
            case VK_RETURN:
            case VK_SPACE:
                InvokeLink(m_Focus);
                return 0;
        }
        bHandled = FALSE;
        return 0;
    }

    LRESULT OnFocusChange(UINT, WPARAM, LPARAM, BOOL &)
    {
        InvalidateLink(m_Focus);
        return 0;
    }

    LRESULT OnGetDlgCode(UINT, WPARAM, LPARAM, BOOL &)
    {
        return DLGC_WANTARROWS;
    }

    LRESULT OnSettingChange(UINT, WPARAM, LPARAM, BOOL &)
    {
        CreateFonts();
        Layout();
        Invalidate(TRUE);
        return 0;
    }

    LRESULT OnReload(UINT, WPARAM, LPARAM, BOOL &)
    {
        LoadLinks();
        Layout();
        Invalidate(TRUE);
        return 0;
    }

    void OnFinalMessage(HWND) override
    {
        delete this;
    }

    DECLARE_WND_CLASS_EX(TASKPANE_CLASS, CS_DBLCLKS, COLOR_WINDOW)

    BEGIN_MSG_MAP(CDefViewTaskPane)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_PAINT, OnPaint)
        MESSAGE_HANDLER(WM_PRINTCLIENT, OnPrintClient)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
        MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
        MESSAGE_HANDLER(WM_SETCURSOR, OnSetCursor)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
        MESSAGE_HANDLER(WM_LBUTTONDBLCLK, OnLButtonDown)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
        MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
        MESSAGE_HANDLER(WM_SETFOCUS, OnFocusChange)
        MESSAGE_HANDLER(WM_KILLFOCUS, OnFocusChange)
        MESSAGE_HANDLER(WM_GETDLGCODE, OnGetDlgCode)
        MESSAGE_HANDLER(WM_SETTINGCHANGE, OnSettingChange)
        MESSAGE_HANDLER(WM_SYSCOLORCHANGE, OnSettingChange)
        MESSAGE_HANDLER(TASKPANE_RELOAD, OnReload)
    END_MSG_MAP()
};

/* The pane window for a folder that publishes links, or NULL (folder without links) */
HWND DefViewTasks_Create(HWND hwndParent, IShellFolder *psf, HWND hwndOwner, IUnknown *pSite)
{
    CComPtr<IExplorerCommandProvider> pProvider;

    if (!psf || FAILED(psf->CreateViewObject(hwndOwner, IID_PPV_ARG(IExplorerCommandProvider, &pProvider))) || !pProvider)
        return NULL;

    CDefViewTaskPane *pPane = new CDefViewTaskPane();
    if (!pPane)
        return NULL;
    if (pPane->Initialize(pProvider, pSite) != S_OK)
    {
        delete pPane;
        return NULL;
    }
    RECT rc = { 0, 0, 0, 0 };
    if (!pPane->Create(hwndParent, rc, NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, 0))
    {
        delete pPane;
        return NULL;
    }
    pPane->Layout();
    return pPane->m_hWnd;
}

/* Places the pane at the left of the parent's cx * cy client area and returns its width;
   0 (pane hidden) when there is no pane or the view is too narrow to share */
int DefViewTasks_Layout(HWND hwndPane, int cx, int cy)
{
    if (!hwndPane || !IsWindow(hwndPane))
        return 0;
    HDC hdc = GetDC(hwndPane);
    int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    if (hdc)
        ReleaseDC(hwndPane, hdc);
    int cxPane = MulDiv(210, dpi, 96);
    if (cx < 2 * cxPane)
    {
        ShowWindow(hwndPane, SW_HIDE);
        return 0;
    }
    MoveWindow(hwndPane, 0, 0, cxPane, cy, TRUE);
    ShowWindow(hwndPane, SW_SHOWNA);
    return cxPane;
}

/* The folder's links may have changed (F5 / refresh) */
void DefViewTasks_Refresh(HWND hwndPane)
{
    if (hwndPane && IsWindow(hwndPane))
        SendMessageW(hwndPane, TASKPANE_RELOAD, 0, 0);
}
