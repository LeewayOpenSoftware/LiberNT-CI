/*
 * PROJECT:     LiberNT shell32
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Folder task pane: the column of links on the left of a folder view.
 *              A folder publishes its links by returning an IExplorerCommandProvider
 *              from IShellFolder::CreateViewObject; each IExplorerCommand is one link.
 *              A command with ECF_ISSEPARATOR is a gap, or a section header ("See also")
 *              when it has a title; a command in ECS_CHECKED state is the current place,
 *              shown in bold and not as a link.
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
        BOOL bChecked;  // the current place: bold text, not a link
        BOOL bHeader;   // ECF_ISSEPARATOR: a gap, or a section header with its title
        HICON hIcon;    // IExplorerCommand::GetIcon (a shield), drawn before the link
        RECT rc;
    };

    CComPtr<IExplorerCommandProvider> m_pProvider;
    CComPtr<IUnknown> m_pSite;
    CAtlArray<TASKLINK> m_Links;
    HFONT m_hFont = NULL;
    HFONT m_hFontHot = NULL;
    HFONT m_hFontBold = NULL;
    int m_Dpi = 96;
    int m_Hot = -1;
    int m_Focus = 0;
    BOOL m_bTracking = FALSE;
    BOOL m_bOwnedByWindow = FALSE; // set once Create succeeded: from then on OnFinalMessage deletes

    int Scale(int px) const { return MulDiv(px, m_Dpi, 96); }

    void FreeFonts()
    {
        if (m_hFont)
            DeleteObject(m_hFont);
        if (m_hFontHot)
            DeleteObject(m_hFontHot);
        if (m_hFontBold)
            DeleteObject(m_hFontBold);
        m_hFont = m_hFontHot = m_hFontBold = NULL;
    }

    void FreeIcons()
    {
        for (size_t i = 0; i < m_Links.GetCount(); ++i)
        {
            if (m_Links[i].hIcon)
                DestroyIcon(m_Links[i].hIcon);
            m_Links[i].hIcon = NULL;
        }
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
        lf.lfUnderline = FALSE;
        lf.lfWeight = FW_BOLD;
        m_hFontBold = CreateFontIndirectW(&lf);
    }

    /* a link the user can focus and run */
    BOOL IsLink(int i) const
    {
        return i >= 0 && i < (int)m_Links.GetCount() && !m_Links[i].bHeader && !m_Links[i].bChecked;
    }

    int NextLink(int from, int step) const
    {
        int count = (int)m_Links.GetCount();
        for (int n = 0, i = from; n < count; n++)
        {
            i = (i + step + count) % count;
            if (IsLink(i))
                return i;
        }
        return -1;
    }

public:
    /* Ask the provider again; titles and states may have changed. S_FALSE: nothing to show */
    HRESULT LoadLinks()
    {
        CComPtr<IEnumExplorerCommand> pEnum;
        CComPtr<IExplorerCommand> pCommand;

        FreeIcons();
        m_Links.SetCount(0);
        m_Hot = -1;
        m_Focus = 0;
        HRESULT hr = m_pProvider->GetCommands(m_pSite, IID_PPV_ARG(IEnumExplorerCommand, &pEnum));
        if (FAILED(hr))
            return hr;
        while (m_Links.GetCount() < TASKPANE_MAX_LINKS && pEnum->Next(1, &pCommand, NULL) == S_OK)
        {
            EXPCMDSTATE state = ECS_ENABLED;
            EXPCMDFLAGS flags = ECF_DEFAULT;
            CComHeapPtr<WCHAR> pszTitle;
            if (FAILED(pCommand->GetState(NULL, TRUE, &state)))
                state = ECS_ENABLED;
            if (FAILED(pCommand->GetFlags(&flags)))
                flags = ECF_DEFAULT;
            BOOL bHeader = !!(flags & ECF_ISSEPARATOR);
            if (FAILED(pCommand->GetTitle(NULL, &pszTitle)))
                pszTitle.Free();
            if (!(state & ECS_HIDDEN) && (bHeader || (pszTitle && *pszTitle)))
            {
                TASKLINK link;
                link.pCommand = pCommand;
                link.strTitle = pszTitle ? (PCWSTR)pszTitle : L"";
                link.bEnabled = !(state & ECS_DISABLED);
                link.bChecked = !!(state & ECS_CHECKED);
                link.bHeader = bHeader;
                link.hIcon = NULL;
                CComHeapPtr<WCHAR> pszIcon;
                if (!bHeader && SUCCEEDED(pCommand->GetIcon(NULL, &pszIcon)) && pszIcon && *pszIcon)
                {
                    WCHAR location[MAX_PATH];
                    StringCchCopyW(location, _countof(location), pszIcon);
                    int index = PathParseIconLocationW(location);
                    ExtractIconExW(location, index, NULL, &link.hIcon, 1);
                }
                SetRectEmpty(&link.rc);
                m_Links.Add(link);
            }
            pCommand.Release();
        }
        m_Focus = NextLink(-1, 1);
        if (m_Focus < 0)
            m_Focus = 0;
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
            int ix = m_Links[i].hIcon ? Scale(20) : 0;
            RECT rc = { 0, 0, cx - ix, 0 };
            if (m_Links[i].bHeader && m_Links[i].strTitle.IsEmpty())
            {
                SetRect(&m_Links[i].rc, x, y, x, y + Scale(6)); // a gap
                y = m_Links[i].rc.bottom;
                continue;
            }
            if (m_Links[i].bHeader && i)
                y += Scale(10); // a section starts
            SelectObject(hdc, m_Links[i].bChecked ? m_hFontBold : m_hFont);
            DrawTextW(hdc, m_Links[i].strTitle, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            SetRect(&m_Links[i].rc, x + ix, y, x + ix + min((int)(rc.right - rc.left), cx - ix),
                    y + max((int)(rc.bottom - rc.top), ix ? Scale(16) : 0));
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
            if (IsLink((int)i) && PtInRect(&rc, pt))
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
        if (!IsLink(i) || !m_Links[i].bEnabled)
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
            if (link.bHeader || link.bChecked)
            {
                SelectObject(hdc, link.bChecked ? m_hFontBold : m_hFont);
                SetTextColor(hdc, GetSysColor(link.bChecked ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
                DrawTextW(hdc, link.strTitle, -1, &rc, DT_WORDBREAK | DT_NOPREFIX);
                continue;
            }
            if (link.hIcon)
                DrawIconEx(hdc, rc.left - Scale(20), rc.top, link.hIcon, Scale(16), Scale(16), 0, NULL, DI_NORMAL);
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
        FreeIcons();
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
        if (IsLink(m_Hot) && m_Links[m_Hot].bEnabled)
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
            {
                int next = NextLink(m_Focus, wParam == VK_DOWN ? 1 : -1);
                if (next < 0)
                    return 0;
                InvalidateLink(m_Focus);
                m_Focus = next;
                InvalidateLink(m_Focus);
                return 0;
            }
            case VK_RETURN:
            case VK_SPACE:
                InvokeLink(m_Focus);
                return 0;
        }
        bHandled = FALSE;
        return 0;
    }

    LRESULT OnFocusChange(UINT uMsg, WPARAM, LPARAM, BOOL &)
    {
        InvalidateLink(m_Focus);
        if (uMsg == WM_SETFOCUS)
            ::SendMessageW(GetParent(), SHV_TASKPANE_FOCUS, 0, 0);
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
        if (m_bOwnedByWindow)
            delete this;
    }

    void SetOwnedByWindow() { m_bOwnedByWindow = TRUE; }

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
    return DefViewTasks_CreateFromProvider(hwndParent, pProvider, pSite);
}

/* The pane window for a view that has its own links (Control Panel category pages), or NULL */
HWND DefViewTasks_CreateFromProvider(HWND hwndParent, IExplorerCommandProvider *pProvider, IUnknown *pSite)
{
    if (!pProvider)
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
    /* until Create succeeds the pane is ours to delete, even if a half-created window
       already went through WM_NCDESTROY and OnFinalMessage */
    if (!pPane->Create(hwndParent, rc, NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, 0))
    {
        delete pPane;
        return NULL;
    }
    pPane->SetOwnedByWindow();
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
    int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96; // same as the pane's own scale
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

/* IExplorerPaneVisibility for a folder that sets the navigation pane state (Control Panel) */
class CPaneVisibility :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IExplorerPaneVisibility
{
    EXPLORERPANESTATE m_NavPane = EPS_DONTCARE;

public:
    HRESULT Initialize(EXPLORERPANESTATE navPane)
    {
        m_NavPane = navPane;
        return S_OK;
    }

    STDMETHODIMP GetPaneState(REFEXPLORERPANE ep, EXPLORERPANESTATE *peps) override
    {
        if (!peps)
            return E_POINTER;
        *peps = (ep && IsEqualGUID(*ep, EP_NavPane)) ? m_NavPane : EPS_DONTCARE;
        return S_OK;
    }

    DECLARE_NOT_AGGREGATABLE(CPaneVisibility)
    DECLARE_PROTECT_FINAL_CONSTRUCT()
    BEGIN_COM_MAP(CPaneVisibility)
        COM_INTERFACE_ENTRY_IID(IID_IExplorerPaneVisibility, IExplorerPaneVisibility)
    END_COM_MAP()
};

HRESULT CPaneVisibility_CreateInstance(EXPLORERPANESTATE navPane, REFIID riid, void **ppv)
{
    return ShellObjectCreatorInit<CPaneVisibility>(navPane, riid, ppv);
}
