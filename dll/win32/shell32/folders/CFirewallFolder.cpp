/*
 * PROJECT:     shell32
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Orosi Firewall: the state of the firewall for each type of network
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 *
 * The page reads and writes the firewall policy only through INetFwPolicy2 (hnetcfg), the
 * interface the firewall service's policy is kept behind, and the networks from the Network
 * List Manager (netprofm). It writes nothing itself: a change the caller may not make fails
 * in hnetcfg and is reported.
 */

#include "precomp.h"
#include <netfw.h>
#include <netlistmgr.h>

EXTERN_C const CLSID CLSID_FirewallControlPanel =
    { 0x4026492f, 0x2f69, 0x46b8, { 0xb9, 0xbf, 0x56, 0x54, 0xfc, 0x07, 0xe4, 0x23 } };

static const CLSID kClsidFwPolicy2 =
    { 0xe2b3c97f, 0x6ae1, 0x41ac, { 0x81, 0x7a, 0xf6, 0xf9, 0x21, 0x66, 0xd7, 0xdd } };
static const IID kIidFwPolicy2 =
    { 0x98325047, 0xc671, 0x4174, { 0x8d, 0x81, 0xde, 0xfc, 0xd3, 0xf0, 0x31, 0x86 } };
static const CLSID kClsidNetworkListManager =
    { 0xdcb00c01, 0x570f, 0x4a9b, { 0x8d, 0x69, 0x19, 0x9f, 0xdb, 0xa5, 0x72, 0x3b } };
static const IID kIidNetworkListManager =
    { 0xdcb00000, 0x570f, 0x4a9b, { 0x8d, 0x69, 0x19, 0x9f, 0xdb, 0xa5, 0x72, 0x3b } };

#define FW_CPL_PARSE L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::{21EC2020-3AEA-1069-A2DD-08002B30309D}"
#define FW_NSC_PARSE FW_CPL_PARSE L"\\::{8E908FC9-BECC-40f6-915B-F4CA0E70D03D}"

/* The left pane: Home, the firewall's own links, then "See also" */
enum
{
    FW_PANE_HOME,
    FW_PANE_ONOFF,
    FW_PANE_RESTORE,
    FW_PANE_SEEALSO,        /* section header */
    FW_PANE_NSC,
    FW_PANE_COUNT
};

#define WM_FW_PANE_GOTO (WM_USER + 0x220) /* wParam: FW_PANE_* */

/* The types of network the page shows, as the Customize Settings page of Windows does */
static const struct
{
    LONG Bit;
    UINT Name;
    PCWSTR English;
    UINT Settings;
    PCWSTR SettingsEnglish;
} g_FwProfiles[] =
{
    { NET_FW_PROFILE2_PRIVATE, IDS_FW_PRIVATE, L"Private networks",
      IDS_FW_PRIVATE_SETTINGS, L"Private network settings" },
    { NET_FW_PROFILE2_PUBLIC, IDS_FW_PUBLIC, L"Guest or public networks",
      IDS_FW_PUBLIC_SETTINGS, L"Public network settings" },
};
#define FW_PROFILES _countof(g_FwProfiles)

static VOID FwString(UINT id, PWSTR buffer, int cch, PCWSTR english)
{
    if (!LoadStringW(shell32_hInstance, id, buffer, cch))
        StringCchCopyW(buffer, cch, english);
}

static VOID FwPaneTitle(int index, PWSTR buffer, int cch)
{
    switch (index)
    {
        case FW_PANE_HOME:    FwString(IDS_CPL_HOME, buffer, cch, L"Control Panel Home"); break;
        case FW_PANE_ONOFF:   FwString(IDS_FW_PANE_ONOFF, buffer, cch, L"Turn Orosi Firewall on or off"); break;
        case FW_PANE_RESTORE: FwString(IDS_FW_PANE_RESTORE, buffer, cch, L"Restore defaults"); break;
        case FW_PANE_SEEALSO: FwString(IDS_NSC_SEEALSO, buffer, cch, L"See also"); break;
        case FW_PANE_NSC:     FwString(IDS_FW_PANE_NSC, buffer, cch, L"Network and Sharing Center"); break;
        default:              buffer[0] = UNICODE_NULL; break;
    }
}

static HRESULT FwPolicy(INetFwPolicy2 **ppPolicy)
{
    return CoCreateInstance(kClsidFwPolicy2, NULL, CLSCTX_INPROC_SERVER, kIidFwPolicy2, (void **)ppPolicy);
}

static VOID FwReportError(HWND hwnd, UINT id, PCWSTR english, HRESULT hr)
{
    WCHAR format[256], text[300], title[64];

    FwString(id, format, _countof(format), english);
    StringCchPrintfW(text, _countof(text), format, hr);
    FwString(IDS_FW_NAME, title, _countof(title), L"Orosi Firewall");
    MessageBoxW(hwnd, text, title, MB_OK | MB_ICONERROR);
}

class CFwPaneCommand :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IExplorerCommand
{
    HWND m_hwndView = NULL;
    int m_index = FW_PANE_HOME;

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
        FwPaneTitle(m_index, title, _countof(title));
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
        if (m_index == FW_PANE_SEEALSO || !IsWindow(m_hwndView))
            return E_FAIL;
        return PostMessageW(m_hwndView, WM_FW_PANE_GOTO, (WPARAM)m_index, 0) ? S_OK : E_FAIL;
    }
    STDMETHODIMP GetFlags(EXPCMDFLAGS *pFlags) override
    {
        *pFlags = m_index == FW_PANE_SEEALSO ? ECF_ISSEPARATOR : ECF_DEFAULT;
        return S_OK;
    }
    STDMETHODIMP EnumSubCommands(IEnumExplorerCommand **ppEnum) override
    {
        *ppEnum = NULL;
        return E_NOTIMPL;
    }

    DECLARE_NOT_AGGREGATABLE(CFwPaneCommand)
    DECLARE_PROTECT_FINAL_CONSTRUCT()
    BEGIN_COM_MAP(CFwPaneCommand)
        COM_INTERFACE_ENTRY_IID(IID_IExplorerCommand, IExplorerCommand)
    END_COM_MAP()
};

class CFwPaneEnum :
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
        while (fetched < celt && m_pos < FW_PANE_COUNT)
        {
            CComPtr<CFwPaneCommand> pCommand;
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
        m_pos = min(m_pos + (int)celt, (int)FW_PANE_COUNT);
        return m_pos < FW_PANE_COUNT ? S_OK : S_FALSE;
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

    DECLARE_NOT_AGGREGATABLE(CFwPaneEnum)
    DECLARE_PROTECT_FINAL_CONSTRUCT()
    BEGIN_COM_MAP(CFwPaneEnum)
        COM_INTERFACE_ENTRY_IID(IID_IEnumExplorerCommand, IEnumExplorerCommand)
    END_COM_MAP()
};

class CFwPaneProvider :
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
        CComPtr<CFwPaneEnum> pEnum;
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

    DECLARE_NOT_AGGREGATABLE(CFwPaneProvider)
    DECLARE_PROTECT_FINAL_CONSTRUCT()
    BEGIN_COM_MAP(CFwPaneProvider)
        COM_INTERFACE_ENTRY_IID(IID_IExplorerCommandProvider, IExplorerCommandProvider)
    END_COM_MAP()
};

/* ---- Customize Settings: on or off, and "block all", for each type of network ---------------- */

/* The dialog is built from the string table, so every language has it without a template */
#define IDC_FW_INTRO     100
#define IDC_FW_HEADER(i) (110 + 10 * (i))
#define IDC_FW_ON(i)     (111 + 10 * (i))
#define IDC_FW_BLOCK(i)  (112 + 10 * (i))
#define IDC_FW_OFF(i)    (113 + 10 * (i))

struct FwCustomize
{
    BOOL On[FW_PROFILES];
    BOOL Block[FW_PROFILES];
};

struct FwDlgBuilder
{
    DWORD Storage[2048];
    WORD *Pos;
    WORD *Count;

    WORD *Align(WORD *p)
    {
        return (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
    }
    BOOL Fits(SIZE_T words)
    {
        return Pos + words + 2 <= (WORD *)(Storage + _countof(Storage));
    }
    VOID String(PCWSTR text)
    {
        SIZE_T len = wcslen(text) + 1;
        if (!Fits(len))
            len = 1, text = L"";
        CopyMemory(Pos, text, len * sizeof(WCHAR));
        Pos += len;
    }
    VOID Begin(PCWSTR title, short cx, short cy)
    {
        DLGTEMPLATE *dlg = (DLGTEMPLATE *)Storage;

        ZeroMemory(Storage, sizeof(Storage));
        dlg->style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | DS_SHELLFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
        dlg->cx = cx;
        dlg->cy = cy;
        Count = &dlg->cdit;
        Pos = (WORD *)(dlg + 1);
        *Pos++ = 0;                 /* no menu */
        *Pos++ = 0;                 /* the dialog class */
        String(title);
        *Pos++ = 8;                 /* point size */
        String(L"MS Shell Dlg 2");
    }
    VOID Item(WORD id, WORD atom, DWORD style, short x, short y, short cx, short cy, PCWSTR text)
    {
        DLGITEMTEMPLATE *item;

        Pos = Align(Pos);
        if (!Fits(sizeof(*item) / sizeof(WORD) + 4))
            return;
        item = (DLGITEMTEMPLATE *)Pos;
        item->style = style | WS_CHILD | WS_VISIBLE;
        item->x = x;
        item->y = y;
        item->cx = cx;
        item->cy = cy;
        item->id = id;
        Pos = (WORD *)(item + 1);
        *Pos++ = 0xFFFF;
        *Pos++ = atom;
        String(text);
        *Pos++ = 0;                 /* no creation data */
        (*Count)++;
    }
};

static VOID FwCustomizeSync(HWND hDlg, UINT profile)
{
    BOOL on = IsDlgButtonChecked(hDlg, IDC_FW_ON(profile)) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_FW_BLOCK(profile)), on);
}

static INT_PTR CALLBACK FwCustomizeProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    FwCustomize *settings = (FwCustomize *)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (uMsg)
    {
        case WM_INITDIALOG:
            settings = (FwCustomize *)lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)settings);
            for (UINT i = 0; i < FW_PROFILES; i++)
            {
                CheckDlgButton(hDlg, IDC_FW_ON(i), settings->On[i] ? BST_CHECKED : BST_UNCHECKED);
                CheckDlgButton(hDlg, IDC_FW_OFF(i), settings->On[i] ? BST_UNCHECKED : BST_CHECKED);
                CheckDlgButton(hDlg, IDC_FW_BLOCK(i), settings->Block[i] ? BST_CHECKED : BST_UNCHECKED);
                FwCustomizeSync(hDlg, i);
            }
            return TRUE;

        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                UINT id = LOWORD(wParam);
                for (UINT i = 0; i < FW_PROFILES; i++)
                {
                    /* the radio buttons of a type are apart from its check box: set by hand */
                    if (id == IDC_FW_ON(i) || id == IDC_FW_OFF(i))
                    {
                        CheckDlgButton(hDlg, IDC_FW_ON(i), id == IDC_FW_ON(i) ? BST_CHECKED : BST_UNCHECKED);
                        CheckDlgButton(hDlg, IDC_FW_OFF(i), id == IDC_FW_OFF(i) ? BST_CHECKED : BST_UNCHECKED);
                        FwCustomizeSync(hDlg, i);
                        return TRUE;
                    }
                }
                if (id == IDOK && settings)
                {
                    for (UINT i = 0; i < FW_PROFILES; i++)
                    {
                        settings->On[i] = IsDlgButtonChecked(hDlg, IDC_FW_ON(i)) == BST_CHECKED;
                        settings->Block[i] = IsDlgButtonChecked(hDlg, IDC_FW_BLOCK(i)) == BST_CHECKED;
                    }
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                }
                if (id == IDCANCEL)
                {
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
                }
            }
            break;
    }
    return FALSE;
}

static BOOL FwCustomizeDialog(HWND hwndOwner, FwCustomize *settings)
{
    FwDlgBuilder *b = (FwDlgBuilder *)HeapAlloc(GetProcessHeap(), 0, sizeof(FwDlgBuilder));
    WCHAR text[256];
    short y;
    INT_PTR result;

    if (!b)
        return FALSE;
    FwString(IDS_FW_CUSTOMIZE, text, _countof(text), L"Customize Settings");
    b->Begin(text, 270, 50 + 66 * FW_PROFILES);
    FwString(IDS_FW_CUSTOMIZE_INTRO, text, _countof(text), L"Customize settings for each type of network");
    b->Item(IDC_FW_INTRO, 0x0082, SS_LEFT, 7, 7, 256, 10, text);
    y = 24;
    for (UINT i = 0; i < FW_PROFILES; i++)
    {
        FwString(g_FwProfiles[i].Settings, text, _countof(text), g_FwProfiles[i].SettingsEnglish);
        b->Item(IDC_FW_HEADER(i), 0x0082, SS_LEFT, 7, y, 256, 10, text);
        FwString(IDS_FW_TURN_ON, text, _countof(text), L"Turn on Orosi Firewall");
        b->Item(IDC_FW_ON(i), 0x0080, BS_RADIOBUTTON | WS_GROUP | WS_TABSTOP, 16, y + 13, 246, 10, text);
        FwString(IDS_FW_IN_BLOCKALL, text, _countof(text),
                 L"Block all incoming connections, including those in the list of allowed apps");
        b->Item(IDC_FW_BLOCK(i), 0x0080, BS_AUTOCHECKBOX | BS_MULTILINE | WS_GROUP | WS_TABSTOP, 28, y + 25, 234, 18, text);
        FwString(IDS_FW_TURN_OFF, text, _countof(text), L"Turn off Orosi Firewall (not recommended)");
        b->Item(IDC_FW_OFF(i), 0x0080, BS_RADIOBUTTON | WS_GROUP | WS_TABSTOP, 16, y + 45, 246, 10, text);
        y += 66;
    }
    y += 4;
    FwString(IDS_FW_OK, text, _countof(text), L"OK");
    b->Item(IDOK, 0x0080, BS_DEFPUSHBUTTON | WS_GROUP | WS_TABSTOP, 156, y, 50, 14, text);
    FwString(IDS_FW_CANCEL, text, _countof(text), L"Cancel");
    b->Item(IDCANCEL, 0x0080, BS_PUSHBUTTON | WS_TABSTOP, 213, y, 50, 14, text);

    result = DialogBoxIndirectParamW(shell32_hInstance, (LPCDLGTEMPLATEW)b->Storage, hwndOwner,
                                     FwCustomizeProc, (LPARAM)settings);
    HeapFree(GetProcessHeap(), 0, b);
    return result == IDOK;
}

/* ---------------------------------------------------------------------------------------------- */

struct FwProfileState
{
    BOOL Enabled;
    BOOL BlockAll;          /* BlockAllInboundTraffic: allowed apps do not get in either */
    BOOL AllowInbound;      /* DefaultInboundAction allow */
    BOOL Connected;         /* a connected network is of this type */
    WCHAR Networks[256];
};

class CFirewallView :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public CWindowImpl<CFirewallView, CWindow, CControlWinTraits>,
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
    FwProfileState m_state[FW_PROFILES];
    HRESULT m_loadError;

    int S(int value) { return MulDiv(value, m_dpi, 96); }

    VOID FreeFonts()
    {
        HFONT *fonts[] = { &m_fontTitle, &m_fontBold, &m_fontText };
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
        lf.lfWeight = FW_BOLD;
        m_fontBold = CreateFontIndirectW(&lf);
        lf.lfWeight = FW_NORMAL;
        lf.lfHeight = -MulDiv(15, m_dpi, 72);
        m_fontTitle = CreateFontIndirectW(&lf);
    }

    static UINT ProfileOfCategory(NLM_NETWORK_CATEGORY category)
    {
        switch (category)
        {
            case NLM_NETWORK_CATEGORY_PRIVATE:              return NET_FW_PROFILE2_PRIVATE;
            case NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED: return NET_FW_PROFILE2_DOMAIN;
            default:                                        return NET_FW_PROFILE2_PUBLIC;
        }
    }

    /* The connected networks, under the type of network the firewall applies to them */
    VOID LoadNetworks()
    {
        CComPtr<INetworkListManager> pList;
        CComPtr<IEnumNetworks> pEnum;
        CComPtr<INetwork> pNet;
        ULONG fetched;

        if (FAILED(CoCreateInstance(kClsidNetworkListManager, NULL, CLSCTX_INPROC_SERVER, kIidNetworkListManager, (void **)&pList)) ||
            FAILED(pList->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &pEnum)) || !pEnum)
            return;
        while (pEnum->Next(1, &pNet, &fetched) == S_OK && fetched)
        {
            NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
            WCHAR name[128];
            BSTR bstr = NULL;
            UINT bit;

            pNet->GetCategory(&category);
            bit = ProfileOfCategory(category);
            if (SUCCEEDED(pNet->GetName(&bstr)) && bstr && *bstr)
                StringCchCopyW(name, _countof(name), bstr);
            else
                FwString(IDS_NSC_UNIDENTIFIED, name, _countof(name), L"Unidentified network");
            SysFreeString(bstr);
            for (UINT i = 0; i < FW_PROFILES; i++)
            {
                FwProfileState *state = &m_state[i];
                if ((UINT)g_FwProfiles[i].Bit != bit)
                    continue;
                if (state->Networks[0])
                    StringCchCatW(state->Networks, _countof(state->Networks), L", ");
                StringCchCatW(state->Networks, _countof(state->Networks), name);
                state->Connected = TRUE;
            }
            pNet.Release();
        }
    }

    VOID LoadState()
    {
        CComPtr<INetFwPolicy2> pPolicy;

        ZeroMemory(m_state, sizeof(m_state));
        m_loadError = FwPolicy(&pPolicy);
        if (SUCCEEDED(m_loadError))
        {
            for (UINT i = 0; i < FW_PROFILES && SUCCEEDED(m_loadError); i++)
            {
                NET_FW_PROFILE_TYPE2 bit = (NET_FW_PROFILE_TYPE2)g_FwProfiles[i].Bit;
                VARIANT_BOOL enabled = VARIANT_FALSE, block = VARIANT_FALSE;
                NET_FW_ACTION action = NET_FW_ACTION_BLOCK;

                m_loadError = pPolicy->get_FirewallEnabled(bit, &enabled);
                if (SUCCEEDED(m_loadError))
                    m_loadError = pPolicy->get_BlockAllInboundTraffic(bit, &block);
                if (SUCCEEDED(m_loadError))
                    m_loadError = pPolicy->get_DefaultInboundAction(bit, &action);
                m_state[i].Enabled = enabled != VARIANT_FALSE;
                m_state[i].BlockAll = block != VARIANT_FALSE;
                m_state[i].AllowInbound = action == NET_FW_ACTION_ALLOW;
            }
        }
        LoadNetworks();
    }

    int LayoutPane(int width, int height)
    {
        if (!m_hwndTasks)
        {
            CComPtr<CFwPaneProvider> pProvider;
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

    /* text in rc, wrapped; returns its height */
    int DrawWrapped(HDC hdc, HFONT font, COLORREF color, PCWSTR text, const RECT *rc)
    {
        RECT measure = *rc;
        SelectObject(hdc, font);
        SetTextColor(hdc, color);
        DrawTextW(hdc, text, -1, &measure, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        RECT draw = *rc;
        draw.bottom = draw.top + (measure.bottom - measure.top);
        DrawTextW(hdc, text, -1, &draw, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        return measure.bottom - measure.top;
    }

    VOID DrawPage(HDC hdc)
    {
        RECT client, rc;
        WCHAR text[300], label[64];
        int left, right, y, col2;

        GetClientRect(&client);
        {
            RECT pane;
            left = 0;
            if (m_hwndTasks && (::GetWindowLongW(m_hwndTasks, GWL_STYLE) & WS_VISIBLE) &&
                ::GetWindowRect(m_hwndTasks, &pane))
                left = pane.right - pane.left;
        }
        left += S(24);
        right = max(left + S(200), (int)client.right - S(16));
        FillRect(hdc, &client, GetSysColorBrush(COLOR_WINDOW));
        SetBkMode(hdc, TRANSPARENT);

        FwString(IDS_FW_TITLE, text, _countof(text), L"Help protect your PC with Orosi Firewall");
        SetRect(&rc, left, S(16), right, S(46));
        SelectObject(hdc, m_fontTitle);
        SetTextColor(hdc, GetSysColor(COLOR_HOTLIGHT));
        DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        y = S(54);
        FwString(IDS_FW_INTRO, text, _countof(text),
                 L"Orosi Firewall can help prevent hackers or malicious software from gaining access to your PC through the Internet or a network.");
        SetRect(&rc, left, y, right, y + S(200));
        y += DrawWrapped(hdc, m_fontText, GetSysColor(COLOR_WINDOWTEXT), text, &rc) + S(18);

        if (FAILED(m_loadError))
        {
            WCHAR format[256];
            FwString(IDS_FW_UNAVAILABLE, format, _countof(format), L"The Orosi Firewall settings cannot be read (error 0x%08lX).");
            StringCchPrintfW(text, _countof(text), format, m_loadError);
            SetRect(&rc, left, y, right, y + S(200));
            DrawWrapped(hdc, m_fontText, GetSysColor(COLOR_WINDOWTEXT), text, &rc);
            return;
        }

        FwString(IDS_FW_INCOMING, label, _countof(label), L"Incoming connections:");
        col2 = left + S(16) + S(180);
        for (UINT i = 0; i < FW_PROFILES; i++)
        {
            const FwProfileState *state = &m_state[i];
            int rowH = S(22);

            /* the header: a band marked green when the firewall is on, red when it is off */
            RECT band = { left, y, right, y + S(28) };
            RECT mark = { left, y, left + S(6), y + S(28) };
            HBRUSH brush = CreateSolidBrush(state->Enabled ? RGB(0x10, 0x7C, 0x10) : RGB(0xC4, 0x2B, 0x1C));
            FillRect(hdc, &band, GetSysColorBrush(COLOR_3DFACE));
            if (brush)
            {
                FillRect(hdc, &mark, brush);
                DeleteObject(brush);
            }
            FwString(g_FwProfiles[i].Name, text, _countof(text), g_FwProfiles[i].English);
            SetRect(&rc, left + S(14), y, right - S(120), y + S(28));
            SelectObject(hdc, m_fontBold);
            SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
            DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (state->Connected)
                FwString(IDS_FW_CONNECTED, text, _countof(text), L"Connected");
            else
                FwString(IDS_FW_NOTCONNECTED, text, _countof(text), L"Not connected");
            SetRect(&rc, right - S(120), y, right - S(10), y + S(28));
            SelectObject(hdc, m_fontText);
            DrawTextW(hdc, text, -1, &rc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            y += S(28) + S(8);

            /* the rows: state, incoming connections, active networks */
            for (int row = 0; row < 3; row++)
            {
                WCHAR name[64];
                int h;

                if (row == 0)
                {
                    FwString(IDS_FW_STATE, name, _countof(name), L"Orosi Firewall state:");
                    if (state->Enabled)
                        FwString(IDS_FW_ON, text, _countof(text), L"On");
                    else
                        FwString(IDS_FW_OFF, text, _countof(text), L"Off");
                }
                else if (row == 1)
                {
                    StringCchCopyW(name, _countof(name), label);
                    if (!state->Enabled || state->AllowInbound)
                        FwString(IDS_FW_IN_ALLOW, text, _countof(text), L"Allow all incoming connections");
                    else if (state->BlockAll)
                        FwString(IDS_FW_IN_BLOCKALL, text, _countof(text),
                                 L"Block all incoming connections, including those in the list of allowed apps");
                    else
                        FwString(IDS_FW_IN_ALLOWED, text, _countof(text),
                                 L"Block all connections to apps that are not on the list of allowed apps");
                }
                else
                {
                    FwString(IDS_FW_ACTIVE, name, _countof(name), L"Active networks:");
                    if (state->Networks[0])
                        StringCchCopyW(text, _countof(text), state->Networks);
                    else
                        FwString(IDS_FW_NONE, text, _countof(text), L"None");
                }
                SetRect(&rc, left + S(16), y, col2 - S(8), y + rowH);
                SelectObject(hdc, m_fontText);
                SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                DrawTextW(hdc, name, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                SetRect(&rc, col2, y, right, y + S(200));
                h = DrawWrapped(hdc, m_fontText, GetSysColor(COLOR_WINDOWTEXT), text, &rc);
                y += max(h, S(16)) + S(8);
            }
            y += S(14);
        }
    }

    VOID Relayout()
    {
        RECT client;

        if (!m_hWnd)
            return;
        GetClientRect(&client);
        LayoutPane(client.right, client.bottom);
        InvalidateRect(NULL, TRUE);
    }

    /* The view cannot navigate from its own window procedure (Personalization does the same) */
    VOID NavigateTo(PCWSTR parse)
    {
        PIDLIST_ABSOLUTE pidl = NULL;
        if (m_psb && SUCCEEDED(SHParseDisplayName(parse, NULL, &pidl, 0, NULL)) && pidl)
            PersNavigateOutside(m_psb, pidl);
    }

    /* Turn on or off: the settings of each type of network, written through INetFwPolicy2 */
    VOID Customize()
    {
        CComPtr<INetFwPolicy2> pPolicy;
        FwCustomize settings;
        HRESULT hr;

        LoadState();
        for (UINT i = 0; i < FW_PROFILES; i++)
        {
            settings.On[i] = m_state[i].Enabled;
            settings.Block[i] = m_state[i].BlockAll;
        }
        if (!FwCustomizeDialog(m_hWnd, &settings))
            return;
        hr = FwPolicy(&pPolicy);
        for (UINT i = 0; i < FW_PROFILES && SUCCEEDED(hr); i++)
        {
            NET_FW_PROFILE_TYPE2 bit = (NET_FW_PROFILE_TYPE2)g_FwProfiles[i].Bit;

            if (settings.On[i] && settings.Block[i] != m_state[i].BlockAll)
                hr = pPolicy->put_BlockAllInboundTraffic(bit, settings.Block[i] ? VARIANT_TRUE : VARIANT_FALSE);
            if (SUCCEEDED(hr) && settings.On[i] != m_state[i].Enabled)
                hr = pPolicy->put_FirewallEnabled(bit, settings.On[i] ? VARIANT_TRUE : VARIANT_FALSE);
        }
        if (FAILED(hr))
            FwReportError(m_hWnd, IDS_FW_FAILED, L"The settings could not be changed (error 0x%08lX).", hr);
        LoadState();
        Relayout();
    }

    VOID RestoreDefaults()
    {
        CComPtr<INetFwPolicy2> pPolicy;
        WCHAR text[400], title[64];
        HRESULT hr;

        FwString(IDS_FW_RESTORE_ASK, text, _countof(text),
                 L"Restoring the defaults removes all the Orosi Firewall settings you have configured for all network types. Do you want to restore the defaults?");
        FwString(IDS_FW_NAME, title, _countof(title), L"Orosi Firewall");
        if (::MessageBoxW(m_hWnd, text, title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
        hr = FwPolicy(&pPolicy);
        if (SUCCEEDED(hr))
            hr = pPolicy->RestoreLocalFirewallDefaults();
        if (FAILED(hr))
            FwReportError(m_hWnd, IDS_FW_FAILED, L"The settings could not be changed (error 0x%08lX).", hr);
        LoadState();
        Relayout();
    }

public:
    CFirewallView() :
        m_hwndTasks(NULL),
        m_dpi(96),
        m_fontTitle(NULL),
        m_fontBold(NULL),
        m_fontText(NULL),
        m_loadError(S_OK)
    {
        ZeroMemory(&m_fs, sizeof(m_fs));
        ZeroMemory(m_state, sizeof(m_state));
    }

    ~CFirewallView()
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
                HGDIOBJ oldFont = SelectObject(mem, m_fontText);
                DrawPage(mem);
                BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, oldFont);
                SelectObject(mem, old);
            }
            else
            {
                DrawPage(hdc);
            }
            if (bitmap)
                DeleteObject(bitmap);
            if (mem)
                DeleteDC(mem);
        }
        EndPaint(&ps);
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

    LRESULT OnKeyDown(UINT, WPARAM wParam, LPARAM, BOOL &bHandled)
    {
        if (wParam == VK_F5)
        {
            Refresh();
            return 0;
        }
        bHandled = FALSE;
        return 0;
    }

    LRESULT OnPaneGoto(UINT, WPARAM wParam, LPARAM, BOOL &)
    {
        switch ((int)wParam)
        {
            case FW_PANE_HOME:
                NavigateTo(FW_CPL_PARSE);
                break;
            case FW_PANE_ONOFF:
                Customize();
                break;
            case FW_PANE_RESTORE:
                RestoreDefaults();
                break;
            case FW_PANE_NSC:
                NavigateTo(FW_NSC_PARSE);
                break;
        }
        return 0;
    }

    DECLARE_WND_CLASS_EX(L"OrosiFirewallView", CS_DBLCLKS, COLOR_WINDOW)

    BEGIN_COM_MAP(CFirewallView)
    COM_INTERFACE_ENTRY_IID(IID_IShellView, IShellView)
    COM_INTERFACE_ENTRY_IID(IID_IOleWindow, IShellView)
    END_COM_MAP()

    BEGIN_MSG_MAP(CFirewallView)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
    MESSAGE_HANDLER(WM_PAINT, OnPaint)
    MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
    MESSAGE_HANDLER(WM_SETTINGCHANGE, OnSettingChange)
    MESSAGE_HANDLER(WM_THEMECHANGED, OnSettingChange)
    MESSAGE_HANDLER(SHV_TASKPANE_FOCUS, OnPaneFocus)
    MESSAGE_HANDLER(WM_FW_PANE_GOTO, OnPaneGoto)
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
        LoadState();
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
        LoadState();
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

CFirewallFolder::CFirewallFolder() :
    m_pidlRoot(NULL)
{
}

CFirewallFolder::~CFirewallFolder()
{
    if (m_pidlRoot)
        ILFree(m_pidlRoot);
}

HRESULT WINAPI CFirewallFolder::GetClassID(CLSID *lpClassId)
{
    if (!lpClassId)
        return E_POINTER;
    *lpClassId = CLSID_FirewallControlPanel;
    return S_OK;
}

HRESULT WINAPI CFirewallFolder::Initialize(PCIDLIST_ABSOLUTE pidl)
{
    if (m_pidlRoot)
        ILFree(m_pidlRoot);
    m_pidlRoot = ILClone(pidl);
    return m_pidlRoot ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI CFirewallFolder::GetCurFolder(PIDLIST_ABSOLUTE *pidl)
{
    if (!pidl)
        return E_POINTER;
    *pidl = ILClone(m_pidlRoot);
    return *pidl ? S_OK : E_FAIL;
}

HRESULT WINAPI CFirewallFolder::ParseDisplayName(HWND hwndOwner, LPBC pbc, LPOLESTR lpszDisplayName,
    DWORD *pchEaten, PIDLIST_RELATIVE *ppidl, DWORD *pdwAttributes)
{
    if (ppidl)
        *ppidl = NULL;
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

HRESULT WINAPI CFirewallFolder::EnumObjects(HWND hwndOwner, DWORD dwFlags, LPENUMIDLIST *ppEnumIDList)
{
    /* the page shows settings, the folder has no children */
    return ShellObjectCreator<CEnumIDListBase>(IID_PPV_ARG(IEnumIDList, ppEnumIDList));
}

HRESULT WINAPI CFirewallFolder::BindToObject(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut)
{
    if (ppvOut)
        *ppvOut = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI CFirewallFolder::BindToStorage(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut)
{
    if (ppvOut)
        *ppvOut = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI CFirewallFolder::CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2)
{
    return MAKE_COMPARE_HRESULT(0);
}

HRESULT WINAPI CFirewallFolder::CreateViewObject(HWND hwndOwner, REFIID riid, LPVOID *ppvOut)
{
    if (!ppvOut)
        return E_INVALIDARG;
    *ppvOut = NULL;
    /* a Control Panel page: no navigation pane by default, as in Windows */
    if (IsEqualIID(riid, IID_IExplorerPaneVisibility))
        return CPaneVisibility_CreateInstance(EPS_DEFAULT_OFF, riid, ppvOut);
    if (!IsEqualIID(riid, IID_IShellView))
        return E_NOINTERFACE;

    CComPtr<CFirewallView> pView;
    HRESULT hr = ShellObjectCreator(pView);
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;
    return pView->QueryInterface(riid, ppvOut);
}

HRESULT WINAPI CFirewallFolder::GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, DWORD *rgfInOut)
{
    if (!rgfInOut)
        return E_INVALIDARG;
    *rgfInOut &= SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_CANLINK;
    return S_OK;
}

HRESULT WINAPI CFirewallFolder::GetUIObjectOf(HWND hwndOwner, UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
    REFIID riid, UINT *prgfInOut, LPVOID *ppvOut)
{
    if (ppvOut)
        *ppvOut = NULL;
    return E_NOINTERFACE;
}

HRESULT WINAPI CFirewallFolder::GetDisplayNameOf(PCUITEMID_CHILD pidl, DWORD dwFlags, LPSTRRET strRet)
{
    return E_INVALIDARG;
}

HRESULT WINAPI CFirewallFolder::SetNameOf(HWND hwndOwner, PCUITEMID_CHILD pidl, LPCOLESTR lpName,
    DWORD dwFlags, PITEMID_CHILD *pPidlOut)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CFirewallFolder::GetDefaultSearchGUID(GUID *pguid)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CFirewallFolder::EnumSearches(IEnumExtraSearch **ppenum)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CFirewallFolder::GetDefaultColumn(DWORD dwRes, ULONG *pSort, ULONG *pDisplay)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CFirewallFolder::GetDefaultColumnState(UINT iColumn, DWORD *pcsFlags)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CFirewallFolder::GetDetailsEx(PCUITEMID_CHILD pidl, const SHCOLUMNID *pscid, VARIANT *pv)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CFirewallFolder::GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn, SHELLDETAILS *psd)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CFirewallFolder::MapColumnToSCID(UINT column, SHCOLUMNID *pscid)
{
    return E_NOTIMPL;
}
