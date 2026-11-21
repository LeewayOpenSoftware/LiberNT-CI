/*
 * PROJECT:     shell32
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Network and Sharing Center: the active networks and the links of that page
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 *
 * The page reads the networks and their adapters from the Network List Manager (netprofm)
 * through its public COM interface, and the connection names where Windows keeps them; a
 * connection link does what a double click on it does in Network Connections (its status).
 */

#include "precomp.h"
#include <netlistmgr.h>

EXTERN_C const CLSID CLSID_NetworkSharingCenter =
    { 0x8e908fc9, 0xbecc, 0x40f6, { 0x91, 0x5b, 0xf4, 0xca, 0x0e, 0x70, 0xd0, 0x3d } };

/* The Network List Manager (netprofm) */
static const CLSID kClsidNetworkListManager =
    { 0xdcb00c01, 0x570f, 0x4a9b, { 0x8d, 0x69, 0x19, 0x9f, 0xdb, 0xa5, 0x72, 0x3b } };
static const IID kIidNetworkListManager =
    { 0xdcb00000, 0x570f, 0x4a9b, { 0x8d, 0x69, 0x19, 0x9f, 0xdb, 0xa5, 0x72, 0x3b } };

#define NSC_CPL_PARSE     L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::{21EC2020-3AEA-1069-A2DD-08002B30309D}"
#define NSC_NETCON_PARSE  NSC_CPL_PARSE L"\\::{7007ACC7-3202-11D1-AAD2-00805FC1270E}"
#define NSC_MAX_NETWORKS  8
#define NSC_MAX_CONNS     4
#define NSC_MAX_LINKS     (NSC_MAX_NETWORKS * NSC_MAX_CONNS)

/* The left pane: Home, adapters, sharing, then "See also" */
enum
{
    NSC_PANE_HOME,
    NSC_PANE_ADAPTERS,
    NSC_PANE_SHARING,
    NSC_PANE_SEEALSO,       /* section header */
    NSC_PANE_INETOPTIONS,
    NSC_PANE_COUNT
};

#define WM_NSC_PANE_GOTO (WM_USER + 0x210) /* wParam: NSC_PANE_* */

static VOID NscString(UINT id, PWSTR buffer, int cch, PCWSTR english)
{
    if (!LoadStringW(shell32_hInstance, id, buffer, cch))
        StringCchCopyW(buffer, cch, english);
}

static VOID NscPaneTitle(int index, PWSTR buffer, int cch)
{
    switch (index)
    {
        case NSC_PANE_HOME:        NscString(IDS_CPL_HOME, buffer, cch, L"Control Panel Home"); break;
        case NSC_PANE_ADAPTERS:    NscString(IDS_NSC_ADAPTERS, buffer, cch, L"Change adapter settings"); break;
        case NSC_PANE_SHARING:     NscString(IDS_NSC_SHARING, buffer, cch, L"Change advanced sharing settings"); break;
        case NSC_PANE_SEEALSO:     NscString(IDS_NSC_SEEALSO, buffer, cch, L"See also"); break;
        case NSC_PANE_INETOPTIONS: NscString(IDS_NSC_INETOPTIONS, buffer, cch, L"Internet Options"); break;
        default:                   buffer[0] = UNICODE_NULL; break;
    }
}

class CNscPaneCommand :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IExplorerCommand
{
    HWND m_hwndView = NULL;
    int m_index = NSC_PANE_HOME;

public:
    HRESULT Initialize(HWND hwndView, int index)
    {
        m_hwndView = hwndView;
        m_index = index;
        return S_OK;
    }

    STDMETHODIMP GetTitle(IShellItemArray *psiItemArray, PWSTR *ppszName) override
    {
        WCHAR title[128];
        NscPaneTitle(m_index, title, _countof(title));
        return SHStrDupW(title, ppszName);
    }
    STDMETHODIMP GetIcon(IShellItemArray *psiItemArray, PWSTR *ppszIcon) override
    {
        *ppszIcon = NULL;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetToolTip(IShellItemArray *psiItemArray, PWSTR *ppszInfotip) override
    {
        *ppszInfotip = NULL;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetCanonicalName(GUID *pguidCommandName) override
    {
        *pguidCommandName = GUID_NULL;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetState(IShellItemArray *psiItemArray, BOOL fOkToBeSlow, EXPCMDSTATE *pCmdState) override
    {
        *pCmdState = ECS_ENABLED;
        return S_OK;
    }
    STDMETHODIMP Invoke(IShellItemArray *psiItemArray, IBindCtx *pbc) override
    {
        if (m_index == NSC_PANE_SEEALSO || !IsWindow(m_hwndView))
            return E_FAIL;
        return PostMessageW(m_hwndView, WM_NSC_PANE_GOTO, (WPARAM)m_index, 0) ? S_OK : E_FAIL;
    }
    STDMETHODIMP GetFlags(EXPCMDFLAGS *pFlags) override
    {
        *pFlags = m_index == NSC_PANE_SEEALSO ? ECF_ISSEPARATOR : ECF_DEFAULT;
        return S_OK;
    }
    STDMETHODIMP EnumSubCommands(IEnumExplorerCommand **ppEnum) override
    {
        *ppEnum = NULL;
        return E_NOTIMPL;
    }

    DECLARE_NOT_AGGREGATABLE(CNscPaneCommand)
    DECLARE_PROTECT_FINAL_CONSTRUCT()
    BEGIN_COM_MAP(CNscPaneCommand)
        COM_INTERFACE_ENTRY_IID(IID_IExplorerCommand, IExplorerCommand)
    END_COM_MAP()
};

class CNscPaneEnum :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IEnumExplorerCommand
{
    HWND m_hwndView = NULL;
    int m_pos = 0;

public:
    HRESULT Initialize(HWND hwndView)
    {
        m_hwndView = hwndView;
        return S_OK;
    }

    STDMETHODIMP Next(ULONG celt, IExplorerCommand **pUICommand, ULONG *pceltFetched) override
    {
        ULONG fetched = 0;

        if (!pUICommand)
            return E_POINTER;
        while (fetched < celt && m_pos < NSC_PANE_COUNT)
        {
            CComPtr<CNscPaneCommand> pCommand;
            HRESULT hr = ShellObjectCreator(pCommand);
            if (FAILED(hr))
            {
                if (!fetched)
                    return hr;
                break;
            }
            pCommand->Initialize(m_hwndView, m_pos++);
            pUICommand[fetched++] = pCommand.Detach();
        }
        if (pceltFetched)
            *pceltFetched = fetched;
        return fetched == celt ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG celt) override
    {
        m_pos = min(m_pos + (int)celt, (int)NSC_PANE_COUNT);
        return m_pos < NSC_PANE_COUNT ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override
    {
        m_pos = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumExplorerCommand **ppenum) override
    {
        return E_NOTIMPL;
    }

    DECLARE_NOT_AGGREGATABLE(CNscPaneEnum)
    DECLARE_PROTECT_FINAL_CONSTRUCT()
    BEGIN_COM_MAP(CNscPaneEnum)
        COM_INTERFACE_ENTRY_IID(IID_IEnumExplorerCommand, IEnumExplorerCommand)
    END_COM_MAP()
};

class CNscPaneProvider :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IExplorerCommandProvider
{
    HWND m_hwndView = NULL;

public:
    HRESULT Initialize(HWND hwndView)
    {
        m_hwndView = hwndView;
        return S_OK;
    }

    STDMETHODIMP GetCommands(IUnknown *punkSite, REFIID riid, void **ppv) override
    {
        CComPtr<CNscPaneEnum> pEnum;
        HRESULT hr = ShellObjectCreator(pEnum);
        if (FAILED(hr))
            return hr;
        pEnum->Initialize(m_hwndView);
        return pEnum->QueryInterface(riid, ppv);
    }
    STDMETHODIMP GetCommand(REFGUID rguidCommandId, REFIID riid, void **ppv) override
    {
        *ppv = NULL;
        return E_NOTIMPL;
    }

    DECLARE_NOT_AGGREGATABLE(CNscPaneProvider)
    DECLARE_PROTECT_FINAL_CONSTRUCT()
    BEGIN_COM_MAP(CNscPaneProvider)
        COM_INTERFACE_ENTRY_IID(IID_IExplorerCommandProvider, IExplorerCommandProvider)
    END_COM_MAP()
};

/* ---------------------------------------------------------------------------------------------- */

struct NscConnection
{
    GUID Id;
    WCHAR Name[128];
    BOOL Used;
};

struct NscNetwork
{
    WCHAR Name[128];
    int Category;           /* NLM_NETWORK_CATEGORY, or -1 when not known */
    BOOL Internet;
    BOOL Identified;
    int Conns[NSC_MAX_CONNS];
    int ConnCount;
};

struct NscLink
{
    RECT Rect;
    int Conn;               /* index in m_conns */
};

class CNetCenterView :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public CWindowImpl<CNetCenterView, CWindow, CControlWinTraits>,
    public IShellView
{
private:
    CComPtr<IShellBrowser> m_psb;
    FOLDERSETTINGS m_fs;
    HWND m_hwndTasks;
    int m_dpi;
    HFONT m_fontTitle;
    HFONT m_fontBold;
    HFONT m_fontText;
    HFONT m_fontLink;
    NscConnection m_conns[NSC_MAX_NETWORKS * NSC_MAX_CONNS];
    int m_connCount;
    NscNetwork m_nets[NSC_MAX_NETWORKS];
    int m_netCount;
    NscLink m_links[NSC_MAX_LINKS];
    int m_linkCount;
    int m_hot;
    int m_focus;
    BOOL m_tracking;

    int S(int value) { return MulDiv(value, m_dpi, 96); }

    VOID FreeFonts()
    {
        HFONT *fonts[] = { &m_fontTitle, &m_fontBold, &m_fontText, &m_fontLink };
        for (UINT i = 0; i < _countof(fonts); i++)
        {
            if (*fonts[i])
                DeleteObject(*fonts[i]);
            *fonts[i] = NULL;
        }
    }

    VOID CreateFonts()
    {
        LOGFONTW lf;
        HDC hdc = GetDC();

        FreeFonts();
        m_dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
        if (hdc)
            ReleaseDC(hdc);
        if (m_dpi <= 0)
            m_dpi = 96;
        if (!SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0))
            GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
        m_fontText = CreateFontIndirectW(&lf);
        lf.lfUnderline = TRUE;
        m_fontLink = CreateFontIndirectW(&lf);
        lf.lfUnderline = FALSE;
        lf.lfWeight = FW_BOLD;
        m_fontBold = CreateFontIndirectW(&lf);
        lf.lfWeight = FW_NORMAL;
        lf.lfHeight = -MulDiv(15, m_dpi, 72);
        m_fontTitle = CreateFontIndirectW(&lf);
    }

    /* The name Network Connections shows for an adapter, kept where Windows keeps it */
    static BOOL ConnectionName(const GUID *adapter, PWSTR name, DWORD cch)
    {
        WCHAR guid[40], key[160];
        DWORD cb = cch * sizeof(WCHAR);

        if (!StringFromGUID2(*adapter, guid, _countof(guid)))
            return FALSE;
        StringCchPrintfW(key, _countof(key),
                         L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\%s\\Connection",
                         guid);
        return RegGetValueW(HKEY_LOCAL_MACHINE, key, L"Name", RRF_RT_REG_SZ, NULL, name, &cb) == ERROR_SUCCESS && name[0];
    }

    /* The connected networks and, for each, the connections (adapters) it goes through */
    VOID LoadNetworks()
    {
        CComPtr<INetworkListManager> pList;
        CComPtr<IEnumNetworks> pEnumNet;

        m_connCount = m_netCount = 0;
        if (SUCCEEDED(CoCreateInstance(kClsidNetworkListManager, NULL, CLSCTX_INPROC_SERVER, kIidNetworkListManager, (void **)&pList)) &&
            SUCCEEDED(pList->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &pEnumNet)) && pEnumNet)
        {
            CComPtr<INetwork> pNet;
            ULONG fetched;
            while (m_netCount < (int)_countof(m_nets) && pEnumNet->Next(1, &pNet, &fetched) == S_OK && fetched)
            {
                NscNetwork *net = &m_nets[m_netCount];
                CComPtr<IEnumNetworkConnections> pEnumNc;
                BSTR name = NULL;
                NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
                NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;

                ZeroMemory(net, sizeof(*net));
                if (SUCCEEDED(pNet->GetName(&name)) && name && *name)
                {
                    StringCchCopyW(net->Name, _countof(net->Name), name);
                    net->Identified = TRUE;
                }
                SysFreeString(name);
                net->Category = SUCCEEDED(pNet->GetCategory(&category)) ? (int)category : -1;
                if (SUCCEEDED(pNet->GetConnectivity(&connectivity)))
                    net->Internet = !!(connectivity & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET));
                if (SUCCEEDED(pNet->GetNetworkConnections(&pEnumNc)) && pEnumNc)
                {
                    CComPtr<INetworkConnection> pNc;
                    ULONG got;
                    while (net->ConnCount < NSC_MAX_CONNS && pEnumNc->Next(1, &pNc, &got) == S_OK && got)
                    {
                        GUID adapter;
                        if (m_connCount < (int)_countof(m_conns) && SUCCEEDED(pNc->GetAdapterId(&adapter)))
                        {
                            NscConnection *conn = &m_conns[m_connCount];
                            if (ConnectionName(&adapter, conn->Name, _countof(conn->Name)))
                            {
                                conn->Id = adapter;
                                conn->Used = TRUE;
                                net->Conns[net->ConnCount++] = m_connCount++;
                            }
                        }
                        pNc.Release();
                    }
                }
                m_netCount++;
                pNet.Release();
            }
        }
    }

    /* What a double click on the connection does in Network Connections: its status */
    VOID OpenConnection(int conn)
    {
        CComPtr<IShellFolder> psfDesktop, psfNetCon;
        CComPtr<IEnumIDList> pEnum;
        PIDLIST_ABSOLUTE pidlFolder = NULL;
        PITEMID_CHILD pidl;
        ULONG fetched;

        if (conn < 0 || conn >= m_connCount)
            return;
        if (FAILED(SHParseDisplayName(NSC_NETCON_PARSE, NULL, &pidlFolder, 0, NULL)) || !pidlFolder)
            return;
        if (SUCCEEDED(SHGetDesktopFolder(&psfDesktop)) &&
            SUCCEEDED(psfDesktop->BindToObject(pidlFolder, NULL, IID_PPV_ARG(IShellFolder, &psfNetCon))) &&
            SUCCEEDED(psfNetCon->EnumObjects(m_hWnd, SHCONTF_NONFOLDERS | SHCONTF_FOLDERS, &pEnum)) && pEnum)
        {
            while (pEnum->Next(1, &pidl, &fetched) == S_OK && fetched)
            {
                STRRET str;
                WCHAR name[128];
                BOOL match = SUCCEEDED(psfNetCon->GetDisplayNameOf(pidl, SHGDN_NORMAL, &str)) &&
                             SUCCEEDED(StrRetToBufW(&str, pidl, name, _countof(name))) &&
                             !lstrcmpiW(name, m_conns[conn].Name);
                if (match)
                {
                    CComPtr<IContextMenu> pcm;
                    PCUITEMID_CHILD child = pidl;
                    if (SUCCEEDED(psfNetCon->GetUIObjectOf(m_hWnd, 1, &child, IID_IContextMenu, NULL, (void **)&pcm)))
                    {
                        HMENU hMenu = CreatePopupMenu();
                        if (hMenu && SUCCEEDED(pcm->QueryContextMenu(hMenu, 0, 1, 0x7FFF, CMF_NORMAL)))
                        {
                            /* "Status" is the third command of a connection's menu (netshell
                               shfldr_netconnect.cpp); when it is not there or greyed, the default one */
                            MENUITEMINFOW mii = { sizeof(mii), MIIM_STATE };
                            UINT id = (GetMenuItemInfoW(hMenu, 1 + 2, FALSE, &mii) && !(mii.fState & MFS_GRAYED))
                                      ? 1 + 2 : GetMenuDefaultItem(hMenu, FALSE, 0);
                            if (id != (UINT)-1 && id >= 1)
                            {
                                CMINVOKECOMMANDINFO cmi = { sizeof(cmi) };
                                cmi.hwnd = NULL; /* as in Windows, the status window has no owner */
                                cmi.lpVerb = MAKEINTRESOURCEA(id - 1);
                                cmi.nShow = SW_SHOWNORMAL;
                                pcm->InvokeCommand(&cmi);
                            }
                        }
                        if (hMenu)
                            DestroyMenu(hMenu);
                    }
                }
                ILFree(pidl);
                if (match)
                    break;
            }
        }
        ILFree(pidlFolder);
    }

    int LayoutPane(int width, int height)
    {
        if (!m_hwndTasks)
        {
            CComPtr<CNscPaneProvider> pProvider;
            if (SUCCEEDED(ShellObjectCreator(pProvider)))
            {
                pProvider->Initialize(m_hWnd);
                m_hwndTasks = DefViewTasks_CreateFromProvider(m_hWnd, pProvider, static_cast<IShellView *>(this));
            }
        }
        BOOL paneFocused = m_hwndTasks && ::GetFocus() == m_hwndTasks;
        int pane = DefViewTasks_Layout(m_hwndTasks, width, height);
        if (!pane && paneFocused)
            SetFocus();
        return pane;
    }

    int TextWidth(HDC hdc, HFONT font, PCWSTR text)
    {
        SIZE size = { 0, 0 };
        HGDIOBJ old = SelectObject(hdc, font);
        GetTextExtentPoint32W(hdc, text, lstrlenW(text), &size);
        SelectObject(hdc, old);
        return size.cx;
    }

    /* Draws the page, or only lays out its links when hdc is a measuring DC and paint is FALSE */
    VOID DrawPage(HDC hdc, BOOL paint)
    {
        RECT client, rc;
        WCHAR text[160];
        int left, y, col2;

        GetClientRect(&client);
        {
            /* the page starts right of the left pane when it is shown */
            RECT pane;
            left = 0;
            if (m_hwndTasks && (::GetWindowLongW(m_hwndTasks, GWL_STYLE) & WS_VISIBLE) &&
                ::GetWindowRect(m_hwndTasks, &pane))
                left = pane.right - pane.left;
        }
        left += S(24);
        col2 = left + max(S(260), (int)(client.right - left) / 2);
        if (!paint)
            m_linkCount = 0;
        if (paint)
        {
            FillRect(hdc, &client, GetSysColorBrush(COLOR_WINDOW));
            SetBkMode(hdc, TRANSPARENT);
        }

        NscString(IDS_NSC_TITLE, text, _countof(text), L"View your basic network information and set up connections");
        SetRect(&rc, left, S(16), client.right - S(16), S(16) + S(30));
        if (paint)
        {
            SelectObject(hdc, m_fontTitle);
            SetTextColor(hdc, GetSysColor(COLOR_HOTLIGHT));
            DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        y = S(64);
        NscString(IDS_NSC_ACTIVE, text, _countof(text), L"View your active networks");
        SetRect(&rc, left, y, client.right - S(16), y + S(20));
        if (paint)
        {
            SelectObject(hdc, m_fontText);
            SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
            DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            RECT line = { left, y + S(22), client.right - S(16), y + S(23) };
            FillRect(hdc, &line, GetSysColorBrush(COLOR_3DLIGHT));
        }
        y += S(34);

        if (!m_netCount)
        {
            NscString(IDS_NSC_NONE, text, _countof(text), L"You are currently not connected to any networks.");
            SetRect(&rc, left, y, client.right - S(16), y + S(20));
            if (paint)
            {
                SelectObject(hdc, m_fontText);
                SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            }
            return;
        }

        for (int n = 0; n < m_netCount; n++)
        {
            const NscNetwork *net = &m_nets[n];
            int rowH = S(22), x;

            /* left column: the network and its category */
            if (net->Identified)
                StringCchCopyW(text, _countof(text), net->Name);
            else
                NscString(IDS_NSC_UNIDENTIFIED, text, _countof(text), L"Unidentified network");
            SetRect(&rc, left, y, col2 - S(16), y + rowH);
            if (paint)
            {
                SelectObject(hdc, m_fontBold);
                SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            if (net->Category == NLM_NETWORK_CATEGORY_PRIVATE)
                NscString(IDS_NSC_PRIVATE, text, _countof(text), L"Private network");
            else if (net->Category == NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED)
                NscString(IDS_NSC_DOMAIN, text, _countof(text), L"Domain network");
            else
                NscString(IDS_NSC_PUBLIC, text, _countof(text), L"Public network");
            SetRect(&rc, left, y + rowH, col2 - S(16), y + 2 * rowH);
            if (paint)
            {
                SelectObject(hdc, m_fontText);
                SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            /* right column: access type, then the connections as links */
            WCHAR label[64];
            NscString(IDS_NSC_ACCESS, label, _countof(label), L"Access type:");
            x = col2 + max(TextWidth(hdc, m_fontText, label), S(90)) + S(12);
            SetRect(&rc, col2, y, x, y + rowH);
            if (paint)
            {
                SelectObject(hdc, m_fontText);
                SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                DrawTextW(hdc, label, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            }
            if (net->Internet)
                NscString(IDS_NSC_INTERNET, text, _countof(text), L"Internet");
            else if (net->Identified)
                NscString(IDS_NSC_NOINTERNET, text, _countof(text), L"No Internet access");
            else
                NscString(IDS_NSC_NOACCESS, text, _countof(text), L"No network access");
            SetRect(&rc, x, y, client.right - S(16), y + rowH);
            if (paint)
                DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

            NscString(IDS_NSC_CONNECTIONS, label, _countof(label), L"Connections:");
            SetRect(&rc, col2, y + rowH, x, y + 2 * rowH);
            if (paint)
                DrawTextW(hdc, label, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            for (int c = 0; c < net->ConnCount; c++)
            {
                const NscConnection *conn = &m_conns[net->Conns[c]];
                int tw = TextWidth(hdc, m_fontText, conn->Name);
                SetRect(&rc, x, y + (c + 1) * rowH, min(x + tw, (int)client.right - S(16)), y + (c + 2) * rowH);
                if (!paint && m_linkCount < NSC_MAX_LINKS)
                {
                    m_links[m_linkCount].Rect = rc;
                    m_links[m_linkCount].Conn = net->Conns[c];
                    m_linkCount++;
                }
                if (paint)
                {
                    int link = -1;
                    for (int i = 0; i < m_linkCount; i++)
                    {
                        if (m_links[i].Conn == net->Conns[c])
                            link = i;
                    }
                    SelectObject(hdc, link >= 0 && link == m_hot ? m_fontLink : m_fontText);
                    SetTextColor(hdc, GetSysColor(COLOR_HOTLIGHT));
                    DrawTextW(hdc, conn->Name, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                    SelectObject(hdc, m_fontText);
                    SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                    if (link >= 0 && link == m_focus && ::GetFocus() == m_hWnd)
                    {
                        RECT focus = rc;
                        InflateRect(&focus, S(2), 0);
                        DrawFocusRect(hdc, &focus);
                    }
                }
            }
            y += max(2, net->ConnCount + 1) * rowH + S(18);
        }
    }

    VOID Relayout()
    {
        RECT client;
        HDC hdc;

        if (!m_hWnd)
            return;
        GetClientRect(&client);
        LayoutPane(client.right, client.bottom);
        hdc = GetDC();
        if (hdc)
        {
            DrawPage(hdc, FALSE);
            ReleaseDC(hdc);
        }
        if (m_hot >= m_linkCount)
            m_hot = -1;
        if (m_focus >= m_linkCount)
            m_focus = m_linkCount ? 0 : -1;
        InvalidateRect(NULL, TRUE);
    }

    int HitTest(POINT pt)
    {
        for (int i = 0; i < m_linkCount; i++)
        {
            if (PtInRect(&m_links[i].Rect, pt))
                return i;
        }
        return -1;
    }

    /* The view cannot navigate from its own window procedure (Personalization does the same) */
    VOID NavigateTo(PCWSTR parse)
    {
        PIDLIST_ABSOLUTE pidl = NULL;
        if (m_psb && SUCCEEDED(SHParseDisplayName(parse, NULL, &pidl, 0, NULL)) && pidl)
            PersNavigateOutside(m_psb, pidl);
    }

public:
    CNetCenterView() :
        m_hwndTasks(NULL),
        m_dpi(96),
        m_fontTitle(NULL),
        m_fontBold(NULL),
        m_fontText(NULL),
        m_fontLink(NULL),
        m_connCount(0),
        m_netCount(0),
        m_linkCount(0),
        m_hot(-1),
        m_focus(-1),
        m_tracking(FALSE)
    {
        ZeroMemory(&m_fs, sizeof(m_fs));
    }

    ~CNetCenterView()
    {
        FreeFonts();
    }

    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL &)
    {
        CreateFonts();
        return 0;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL &)
    {
        m_hwndTasks = NULL; /* a child: destroyed with us */
        FreeFonts();
        return 0;
    }

    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL &)
    {
        Relayout();
        return 0;
    }

    LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL &)
    {
        return 1;
    }

    LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL &)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(&ps);
        if (hdc)
        {
            RECT client;
            GetClientRect(&client);
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bitmap = mem ? CreateCompatibleBitmap(hdc, client.right, client.bottom) : NULL;
            if (mem && bitmap)
            {
                HGDIOBJ old = SelectObject(mem, bitmap);
                DrawPage(mem, TRUE);
                BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
            }
            else
            {
                DrawPage(hdc, TRUE);
            }
            if (bitmap)
                DeleteObject(bitmap);
            if (mem)
                DeleteDC(mem);
        }
        EndPaint(&ps);
        return 0;
    }

    LRESULT OnMouseMove(UINT, WPARAM, LPARAM lParam, BOOL &)
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hot = HitTest(pt);
        if (!m_tracking)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hWnd, 0 };
            m_tracking = TrackMouseEvent(&tme);
        }
        if (hot != m_hot)
        {
            m_hot = hot;
            InvalidateRect(NULL, FALSE);
        }
        return 0;
    }

    LRESULT OnMouseLeave(UINT, WPARAM, LPARAM, BOOL &)
    {
        m_tracking = FALSE;
        if (m_hot >= 0)
        {
            m_hot = -1;
            InvalidateRect(NULL, FALSE);
        }
        return 0;
    }

    LRESULT OnLButtonDown(UINT, WPARAM, LPARAM lParam, BOOL &)
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hit = HitTest(pt);
        SetFocus();
        if (hit >= 0)
        {
            m_focus = hit;
            InvalidateRect(NULL, FALSE);
        }
        return 0;
    }

    LRESULT OnLButtonUp(UINT, WPARAM, LPARAM lParam, BOOL &)
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hit = HitTest(pt);
        if (hit >= 0 && hit == m_focus)
            OpenConnection(m_links[hit].Conn);
        return 0;
    }

    LRESULT OnKeyDown(UINT, WPARAM wParam, LPARAM, BOOL &bHandled)
    {
        if (!m_linkCount)
        {
            bHandled = FALSE;
            return 0;
        }
        switch (wParam)
        {
            case VK_UP:
            case VK_DOWN:
            case VK_TAB:
                m_focus = (m_focus + ((wParam == VK_UP || GetKeyState(VK_SHIFT) < 0) ? m_linkCount - 1 : 1)) % m_linkCount;
                InvalidateRect(NULL, FALSE);
                return 0;
            case VK_RETURN:
            case VK_SPACE:
                if (m_focus >= 0)
                    OpenConnection(m_links[m_focus].Conn);
                return 0;
        }
        bHandled = FALSE;
        return 0;
    }

    LRESULT OnGetDlgCode(UINT, WPARAM, LPARAM, BOOL &)
    {
        return DLGC_WANTARROWS | DLGC_WANTTAB;
    }

    LRESULT OnSetCursor(UINT, WPARAM, LPARAM lParam, BOOL &bHandled)
    {
        if (LOWORD(lParam) == HTCLIENT && m_hot >= 0)
        {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        bHandled = FALSE;
        return 0;
    }

    LRESULT OnFocus(UINT uMsg, WPARAM, LPARAM, BOOL &)
    {
        if (uMsg == WM_SETFOCUS && m_focus < 0 && m_linkCount)
            m_focus = 0;
        InvalidateRect(NULL, FALSE);
        return 0;
    }

    LRESULT OnSettingChange(UINT, WPARAM, LPARAM, BOOL &)
    {
        CreateFonts();
        Relayout();
        return 0;
    }

    /* a pane link has the focus: the view stays the active one, as the folder view does */
    LRESULT OnPaneFocus(UINT, WPARAM, LPARAM, BOOL &)
    {
        if (m_psb)
            m_psb->OnViewWindowActive(static_cast<IShellView *>(this));
        return 0;
    }

    LRESULT OnPaneGoto(UINT, WPARAM wParam, LPARAM, BOOL &)
    {
        switch ((int)wParam)
        {
            case NSC_PANE_HOME:
                NavigateTo(NSC_CPL_PARSE);
                break;
            case NSC_PANE_ADAPTERS:
                NavigateTo(NSC_NETCON_PARSE);
                break;
            case NSC_PANE_SHARING:
                ShellExecuteW(m_hWnd, NULL, L"ms-settings:network-advancedsharing", NULL, NULL, SW_SHOWNORMAL);
                break;
            case NSC_PANE_INETOPTIONS:
                ShellExecuteW(m_hWnd, NULL, L"rundll32.exe", L"shell32.dll,Control_RunDLL inetcpl.cpl", NULL, SW_SHOWNORMAL);
                break;
        }
        return 0;
    }

    DECLARE_WND_CLASS_EX(L"NetworkSharingCenterView", CS_DBLCLKS, COLOR_WINDOW)

    BEGIN_COM_MAP(CNetCenterView)
    COM_INTERFACE_ENTRY_IID(IID_IShellView, IShellView)
    COM_INTERFACE_ENTRY_IID(IID_IOleWindow, IShellView)
    END_COM_MAP()

    BEGIN_MSG_MAP(CNetCenterView)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
    MESSAGE_HANDLER(WM_PAINT, OnPaint)
    MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
    MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
    MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
    MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
    MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
    MESSAGE_HANDLER(WM_GETDLGCODE, OnGetDlgCode)
    MESSAGE_HANDLER(WM_SETCURSOR, OnSetCursor)
    MESSAGE_HANDLER(WM_SETFOCUS, OnFocus)
    MESSAGE_HANDLER(WM_KILLFOCUS, OnFocus)
    MESSAGE_HANDLER(WM_SETTINGCHANGE, OnSettingChange)
    MESSAGE_HANDLER(WM_THEMECHANGED, OnSettingChange)
    MESSAGE_HANDLER(SHV_TASKPANE_FOCUS, OnPaneFocus)
    MESSAGE_HANDLER(WM_NSC_PANE_GOTO, OnPaneGoto)
    END_MSG_MAP()

    // IOleWindow
    STDMETHOD(GetWindow)(HWND *lphwnd) override
    {
        if (!lphwnd)
            return E_POINTER;
        *lphwnd = m_hWnd;
        return S_OK;
    }
    STDMETHOD(ContextSensitiveHelp)(BOOL) override { return E_NOTIMPL; }

    // IShellView
    STDMETHOD(TranslateAccelerator)(MSG *pmsg) override { return S_FALSE; }
    STDMETHOD(EnableModeless)(BOOL) override { return S_OK; }
    STDMETHOD(UIActivate)(UINT uState) override
    {
        if (uState != SVUIA_DEACTIVATE && m_hWnd)
            SetFocus();
        return S_OK;
    }
    STDMETHOD(Refresh)() override
    {
        LoadNetworks();
        Relayout();
        return S_OK;
    }
    STDMETHOD(CreateViewWindow)(IShellView *, LPCFOLDERSETTINGS lpfs, IShellBrowser *psb, RECT *prcView, HWND *phWnd) override
    {
        HWND hParent = NULL;
        RECT rc = { 0, 0, 0, 0 };

        if (m_hWnd)
            return E_UNEXPECTED;
        if (!lpfs || !psb || !phWnd)
            return E_INVALIDARG;
        *phWnd = NULL;
        if (FAILED_UNEXPECTEDLY(psb->GetWindow(&hParent)) || !hParent)
            return E_FAIL;
        if (prcView)
            rc = *prcView;
        m_fs = *lpfs;
        m_psb = psb;
        LoadNetworks();
        Create(hParent, rc, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP);
        if (!m_hWnd)
            return E_FAIL;
        Relayout();
        *phWnd = m_hWnd;
        return S_OK;
    }
    STDMETHOD(DestroyViewWindow)() override
    {
        if (m_hWnd)
            DestroyWindow();
        m_psb.Release();
        return S_OK;
    }
    STDMETHOD(GetCurrentInfo)(LPFOLDERSETTINGS lpfs) override
    {
        if (!lpfs)
            return E_POINTER;
        *lpfs = m_fs;
        return S_OK;
    }
    STDMETHOD(AddPropertySheetPages)(DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM) override { return E_NOTIMPL; }
    STDMETHOD(SaveViewState)() override { return S_OK; }
    STDMETHOD(SelectItem)(PCUITEMID_CHILD, SVSIF) override { return E_NOTIMPL; }
    STDMETHOD(GetItemObject)(UINT, REFIID, LPVOID *ppv) override
    {
        if (ppv)
            *ppv = NULL;
        return E_NOINTERFACE;
    }
};

/* ---------------------------------------------------------------------------------------------- */

CNetCenterFolder::CNetCenterFolder() :
    m_pidlRoot(NULL)
{
}

CNetCenterFolder::~CNetCenterFolder()
{
    if (m_pidlRoot)
        ILFree(m_pidlRoot);
}

HRESULT WINAPI CNetCenterFolder::GetClassID(CLSID *lpClassId)
{
    if (!lpClassId)
        return E_POINTER;
    *lpClassId = CLSID_NetworkSharingCenter;
    return S_OK;
}

HRESULT WINAPI CNetCenterFolder::Initialize(PCIDLIST_ABSOLUTE pidl)
{
    if (m_pidlRoot)
        ILFree(m_pidlRoot);
    m_pidlRoot = ILClone(pidl);
    return m_pidlRoot ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI CNetCenterFolder::GetCurFolder(PIDLIST_ABSOLUTE *pidl)
{
    if (!pidl)
        return E_POINTER;
    *pidl = ILClone(m_pidlRoot);
    return *pidl ? S_OK : E_FAIL;
}

HRESULT WINAPI CNetCenterFolder::ParseDisplayName(HWND hwndOwner, LPBC pbc, LPOLESTR lpszDisplayName,
    DWORD *pchEaten, PIDLIST_RELATIVE *ppidl, DWORD *pdwAttributes)
{
    if (ppidl)
        *ppidl = NULL;
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

HRESULT WINAPI CNetCenterFolder::EnumObjects(HWND hwndOwner, DWORD dwFlags, LPENUMIDLIST *ppEnumIDList)
{
    /* the page lists networks, the folder has no children */
    return ShellObjectCreator<CEnumIDListBase>(IID_PPV_ARG(IEnumIDList, ppEnumIDList));
}

HRESULT WINAPI CNetCenterFolder::BindToObject(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut)
{
    if (ppvOut)
        *ppvOut = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI CNetCenterFolder::BindToStorage(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut)
{
    if (ppvOut)
        *ppvOut = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI CNetCenterFolder::CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2)
{
    return MAKE_COMPARE_HRESULT(0);
}

HRESULT WINAPI CNetCenterFolder::CreateViewObject(HWND hwndOwner, REFIID riid, LPVOID *ppvOut)
{
    if (!ppvOut)
        return E_INVALIDARG;
    *ppvOut = NULL;
    /* a Control Panel page: no navigation pane by default, as in Windows */
    if (IsEqualIID(riid, IID_IExplorerPaneVisibility))
        return CPaneVisibility_CreateInstance(EPS_DEFAULT_OFF, riid, ppvOut);
    if (!IsEqualIID(riid, IID_IShellView))
        return E_NOINTERFACE;

    CComPtr<CNetCenterView> pView;
    HRESULT hr = ShellObjectCreator(pView);
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;
    return pView->QueryInterface(riid, ppvOut);
}

HRESULT WINAPI CNetCenterFolder::GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, DWORD *rgfInOut)
{
    if (!rgfInOut)
        return E_INVALIDARG;
    *rgfInOut &= SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_CANLINK;
    return S_OK;
}

HRESULT WINAPI CNetCenterFolder::GetUIObjectOf(HWND hwndOwner, UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
    REFIID riid, UINT *prgfInOut, LPVOID *ppvOut)
{
    if (ppvOut)
        *ppvOut = NULL;
    return E_NOINTERFACE;
}

HRESULT WINAPI CNetCenterFolder::GetDisplayNameOf(PCUITEMID_CHILD pidl, DWORD dwFlags, LPSTRRET strRet)
{
    return E_INVALIDARG;
}

HRESULT WINAPI CNetCenterFolder::SetNameOf(HWND hwndOwner, PCUITEMID_CHILD pidl, LPCOLESTR lpName,
    DWORD dwFlags, PITEMID_CHILD *pPidlOut)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CNetCenterFolder::GetDefaultSearchGUID(GUID *pguid)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CNetCenterFolder::EnumSearches(IEnumExtraSearch **ppenum)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CNetCenterFolder::GetDefaultColumn(DWORD dwRes, ULONG *pSort, ULONG *pDisplay)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CNetCenterFolder::GetDefaultColumnState(UINT iColumn, DWORD *pcsFlags)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CNetCenterFolder::GetDetailsEx(PCUITEMID_CHILD pidl, const SHCOLUMNID *pscid, VARIANT *pv)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CNetCenterFolder::GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn, SHELLDETAILS *psd)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CNetCenterFolder::MapColumnToSCID(UINT column, SHCOLUMNID *pscid)
{
    return E_NOTIMPL;
}
