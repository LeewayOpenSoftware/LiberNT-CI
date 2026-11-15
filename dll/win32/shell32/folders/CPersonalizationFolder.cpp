/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Control Panel > Appearance and Personalization > Personalization,
 *              a shell folder shown in the Explorer view pane
 *
 * The folder owns the navigation: the root and the four child pages are PIDLs, so Explorer
 * draws the breadcrumb and keeps the Back/Forward travel log. The content of a page is
 * created by the page component exported by leewaydisp.cpl (LeewayPageCreate and friends).
 * When the component is not installed the view draws a plain page of links instead.
 */

#include <precomp.h>
#include "CPersonalizationFolder.h"
#include "CCplCategoryView.h"

EXTERN_C const CLSID CLSID_PersonalizationFolder =
    { 0xED834ED6, 0x4B5A, 0x4bfe, { 0x8F, 0x11, 0xA6, 0x26, 0xDC, 0xB6, 0xA9, 0x21 } };

#define PERS_CLSID_TEXT L"{ED834ED6-4B5A-4bfe-8F11-A626DCB6A921}"
#define PERS_ROOT_PARSE L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::{21EC2020-3AEA-1069-A2DD-08002B30309D}"
#define PERS_PIDL_SIG   0x5043

/* The "Appearance and Personalization" category between the Control Panel and Personalization */
#define LWPAGE_CATEGORY 0x200
#define LWPAGE_NONE     0xFFFF

/* Actions of the host callback that are not pages */
#define LWNAV_SOUNDS        0x100
#define LWNAV_SCREENSAVER   0x101
#define LWNAV_CLASSIC       0x102

#pragma pack(push, 1)
struct PersPidl
{
    WORD cb;
    WORD sig;
    WORD page;
};
#pragma pack(pop)

/* ABI of the page component, see shared/libernt/shell/personalization-cp-namespace/DESIGN.md */
struct LWPAGE_HOST
{
    DWORD cbSize;
    void *ctx;
    HRESULT (CALLBACK *Navigate)(void *ctx, UINT pageId);
    HRESULT (CALLBACK *SetStatus)(void *ctx, PCWSTR text);
};
typedef HWND (WINAPI *PFN_PAGECREATE)(UINT pageId, HWND hwndParent, const RECT *prc, const LWPAGE_HOST *host, void **ppPage);
typedef void (WINAPI *PFN_PAGERESIZE)(void *page, const RECT *prc);
typedef BOOL (WINAPI *PFN_PAGETRANSLATE)(void *page, MSG *msg);
typedef void (WINAPI *PFN_PAGEDESTROY)(void *page);

static BOOL PersIsSpanish(VOID)
{
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH;
}

static PCWSTR PersPageName(UINT page)
{
    static const PCWSTR en[LWPAGE_COUNT] = { L"Personalization", L"Desktop Background", L"Window Color", L"Display", L"Screen Resolution" };
    static const PCWSTR es[LWPAGE_COUNT] = { L"Personalizaci\x00F3n", L"Fondo de escritorio", L"Color de ventana", L"Pantalla", L"Resoluci\x00F3n de pantalla" };
    if (page >= LWPAGE_COUNT)
        return L"";
    return PersIsSpanish() ? es[page] : en[page];
}

static PITEMID_CHILD PersCreatePidl(UINT page)
{
    PersPidl *p = (PersPidl *)SHAlloc(sizeof(PersPidl) + sizeof(WORD));
    if (!p)
        return NULL;
    p->cb = sizeof(PersPidl);
    p->sig = PERS_PIDL_SIG;
    p->page = (WORD)page;
    *(WORD *)(p + 1) = 0;
    return (PITEMID_CHILD)p;
}

/* The page value of one of our ids (a page, LWPAGE_MAIN or LWPAGE_CATEGORY), LWPAGE_NONE otherwise */
static UINT PersRawPage(PCUIDLIST_RELATIVE pidl)
{
    const PersPidl *p = (const PersPidl *)pidl;
    if (!pidl || p->cb != sizeof(PersPidl) || p->sig != PERS_PIDL_SIG)
        return LWPAGE_NONE;
    return p->page;
}

/* LWPAGE_COUNT if the id is not one of ours */
static UINT PersPidlPage(PCUIDLIST_RELATIVE pidl)
{
    const PersPidl *p = (const PersPidl *)pidl;
    if (!pidl || p->cb != sizeof(PersPidl) || p->sig != PERS_PIDL_SIG || p->page == LWPAGE_MAIN || p->page >= LWPAGE_COUNT)
        return LWPAGE_COUNT;
    return p->page;
}

static PIDLIST_ABSOLUTE PersPageAbsolute(PCIDLIST_ABSOLUTE root, UINT page)
{
    if (page == LWPAGE_MAIN)
        return ILClone(root);
    PITEMID_CHILD child = PersCreatePidl(page);
    if (!child)
        return NULL;
    PIDLIST_ABSOLUTE abs = ILCombine(root, child);
    ILFree(child);
    return abs;
}

/* ---------------------------------------------------------------------------------------------- */

/* The category, as seen by the Control Panel folder that holds it */
PCWSTR PersCategoryName(VOID)
{
    return PersIsSpanish() ? L"Apariencia y personalizaci\x00F3n" : L"Appearance and Personalization";
}

PITEMID_CHILD CplCategoryCreatePidl(VOID)
{
    return PersCreatePidl(LWPAGE_CATEGORY);
}

BOOL CplIsCategoryPidl(PCUIDLIST_RELATIVE pidl)
{
    return PersRawPage(pidl) == LWPAGE_CATEGORY;
}

BOOL CplIsCategoryName(PCWSTR name)
{
    return !lstrcmpiW(name, L"Appearance and Personalization") || !lstrcmpiW(name, PersCategoryName());
}

BOOL CplIsPersonalizationParseName(PCWSTR name)
{
    return !lstrcmpiW(name, L"::" PERS_CLSID_TEXT);
}

/* pidlAbsCategory: the Control Panel's pidl plus the category id */
HRESULT CplCategoryBind(PCIDLIST_ABSOLUTE pidlAbsCategory, REFIID riid, LPVOID *ppvOut)
{
    CComPtr<CPersonalizationFolder> pFolder;
    HRESULT hr = ShellObjectCreator(pFolder);
    if (SUCCEEDED(hr))
        hr = pFolder->Initialize(pidlAbsCategory);
    if (FAILED(hr))
        return hr;
    return pFolder->QueryInterface(riid, ppvOut);
}

/* ---------------------------------------------------------------------------------------------- */

#define PERS_MAX_LINKS 8

class CPersonalizationView :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public CWindowImpl<CPersonalizationView, CWindow, CControlWinTraits>,
    public IShellView
{
private:
    struct Link
    {
        RECT rc;
        UINT action;
        WCHAR text[64];
    };

    UINT m_page;
    LPITEMIDLIST m_pidlRoot;        /* absolute pidl of the Personalization folder */
    CComPtr<IShellBrowser> m_psb;
    FOLDERSETTINGS m_fs;
    HFONT m_fontTitle;
    HFONT m_fontLink;
    HFONT m_fontText;
    int m_hot;
    Link m_links[PERS_MAX_LINKS];
    int m_linkCount;

    HMODULE m_hLib;
    PFN_PAGECREATE m_pfnCreate;
    PFN_PAGERESIZE m_pfnResize;
    PFN_PAGETRANSLATE m_pfnTranslate;
    PFN_PAGEDESTROY m_pfnDestroy;
    void *m_component;
    HWND m_hwndComponent;
    LWPAGE_HOST m_host;

    static HRESULT CALLBACK HostNavigate(void *ctx, UINT pageId)
    {
        return ((CPersonalizationView *)ctx)->NavigateTo(pageId);
    }

    HRESULT NavigateTo(UINT action)
    {
        if (action == LWNAV_SOUNDS)
            return RunCpl(L"shell32.dll,Control_RunDLL mmsys.cpl,,2");
        if (action == LWNAV_SCREENSAVER || action == LWNAV_CLASSIC)
            return RunCpl(L"shell32.dll,Control_RunDLL leewaydisp.cpl,@0");
        if (!m_psb)
            return E_FAIL;

        PIDLIST_ABSOLUTE pidl = NULL;
        if (action == LWNAV_CPHOME)
            SHParseDisplayName(PERS_ROOT_PARSE, NULL, &pidl, 0, NULL);
        else if (action < LWPAGE_COUNT)
            pidl = PersPageAbsolute(m_pidlRoot, action);
        if (!pidl)
            return E_FAIL;
        HRESULT hr = m_psb->BrowseObject(pidl, SBSP_SAMEBROWSER | SBSP_ABSOLUTE);
        ILFree(pidl);
        return hr;
    }

    HRESULT RunCpl(PCWSTR params)
    {
        HINSTANCE h = ShellExecuteW(m_hWnd, L"open", L"rundll32.exe", params, NULL, SW_SHOWNORMAL);
        return (UINT_PTR)h > 32 ? S_OK : E_FAIL;
    }

    VOID LoadComponent()
    {
        m_hLib = LoadLibraryW(L"leewaydisp.cpl");
        if (!m_hLib)
            return;
        m_pfnCreate = (PFN_PAGECREATE)GetProcAddress(m_hLib, "LeewayPageCreate");
        m_pfnResize = (PFN_PAGERESIZE)GetProcAddress(m_hLib, "LeewayPageResize");
        m_pfnTranslate = (PFN_PAGETRANSLATE)GetProcAddress(m_hLib, "LeewayPageTranslateKey");
        m_pfnDestroy = (PFN_PAGEDESTROY)GetProcAddress(m_hLib, "LeewayPageDestroy");
        if (!m_pfnCreate || !m_pfnResize || !m_pfnDestroy)
        {
            m_pfnCreate = NULL;
            FreeLibrary(m_hLib);
            m_hLib = NULL;
        }
    }

    VOID CreateComponent()
    {
        RECT rc;

        if (!m_pfnCreate)
            return;
        GetClientRect(&rc);
        m_host.cbSize = sizeof(m_host);
        m_host.ctx = this;
        m_host.Navigate = HostNavigate;
        m_host.SetStatus = NULL;
        m_hwndComponent = m_pfnCreate(m_page, m_hWnd, &rc, &m_host, &m_component);
        if (!m_hwndComponent)
            m_component = NULL;
    }

    VOID DestroyComponent()
    {
        if (m_component && m_pfnDestroy)
            m_pfnDestroy(m_component);
        m_component = NULL;
        m_hwndComponent = NULL;
    }

    VOID AddLink(UINT action, PCWSTR en, PCWSTR es)
    {
        if (m_linkCount >= PERS_MAX_LINKS)
            return;
        Link &l = m_links[m_linkCount++];
        l.action = action;
        StringCchCopyW(l.text, _countof(l.text), PersIsSpanish() ? es : en);
        SetRectEmpty(&l.rc);
    }

    VOID BuildLinks()
    {
        m_linkCount = 0;
        switch (m_page)
        {
            case LWPAGE_MAIN:
                AddLink(LWPAGE_BACKGROUND, L"Desktop Background", L"Fondo de escritorio");
                AddLink(LWPAGE_WINCOLOR, L"Window Color", L"Color de ventana");
                AddLink(LWNAV_SOUNDS, L"Sounds", L"Sonidos");
                AddLink(LWNAV_SCREENSAVER, L"Screen Saver", L"Protector de pantalla");
                AddLink(LWPAGE_DISPLAY, L"Display", L"Pantalla");
                AddLink(LWNAV_CLASSIC, L"Classic personalization dialog", L"Cuadro cl\x00E1sico de personalizaci\x00F3n");
                break;
            case LWPAGE_DISPLAY:
                AddLink(LWPAGE_RESOLUTION, L"Screen Resolution", L"Resoluci\x00F3n de pantalla");
                AddLink(LWPAGE_MAIN, L"Personalization", L"Personalizaci\x00F3n");
                break;
            case LWPAGE_RESOLUTION:
                AddLink(LWPAGE_DISPLAY, L"Display", L"Pantalla");
                AddLink(LWNAV_CLASSIC, L"Classic display dialog", L"Cuadro cl\x00E1sico de pantalla");
                break;
            default:
                AddLink(LWPAGE_MAIN, L"Personalization", L"Personalizaci\x00F3n");
                AddLink(LWNAV_CLASSIC, L"Classic personalization dialog", L"Cuadro cl\x00E1sico de personalizaci\x00F3n");
                break;
        }
    }

    VOID CreateFonts()
    {
        LOGFONTW lf;
        HFONT base = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        GetObjectW(base, sizeof(lf), &lf);
        lf.lfWeight = FW_NORMAL;
        m_fontText = CreateFontIndirectW(&lf);
        lf.lfUnderline = TRUE;
        m_fontLink = CreateFontIndirectW(&lf);
        lf.lfUnderline = FALSE;
        lf.lfHeight = lf.lfHeight * 5 / 3;
        m_fontTitle = CreateFontIndirectW(&lf);
    }

    VOID DeleteFonts()
    {
        if (m_fontText) DeleteObject(m_fontText);
        if (m_fontLink) DeleteObject(m_fontLink);
        if (m_fontTitle) DeleteObject(m_fontTitle);
        m_fontText = m_fontLink = m_fontTitle = NULL;
    }

    int HitTest(POINT pt)
    {
        for (int i = 0; i < m_linkCount; i++)
            if (PtInRect(&m_links[i].rc, pt))
                return i;
        return -1;
    }

    VOID Draw(HDC hdc, const RECT *prc)
    {
        const int margin = 24, lineH = 28;
        RECT rc;
        int y = margin;
        HGDIOBJ old;

        FillRect(hdc, prc, (HBRUSH)(UINT_PTR)(COLOR_WINDOW + 1));
        SetBkMode(hdc, TRANSPARENT);

        old = SelectObject(hdc, m_fontTitle);
        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        SetRect(&rc, margin, y, prc->right - margin, y + 40);
        DrawTextW(hdc, PersPageName(m_page), -1, &rc, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
        y += 48;

        SelectObject(hdc, m_fontText);
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        SetRect(&rc, margin, y, prc->right - margin, y + 20);
        DrawTextW(hdc, PersIsSpanish() ? L"Cambie los efectos visuales y los sonidos del equipo." :
                                         L"Change the visuals and sounds on your computer.",
                  -1, &rc, DT_SINGLELINE | DT_NOPREFIX);
        y += 40;

        for (int i = 0; i < m_linkCount; i++)
        {
            SIZE sz;
            SelectObject(hdc, m_fontLink);
            GetTextExtentPoint32W(hdc, m_links[i].text, lstrlenW(m_links[i].text), &sz);
            SetRect(&m_links[i].rc, margin, y, margin + sz.cx + 4, y + lineH - 6);
            SetTextColor(hdc, i == m_hot ? RGB(0x00, 0x66, 0xCC) : RGB(0x00, 0x33, 0x99));
            DrawTextW(hdc, m_links[i].text, -1, &m_links[i].rc, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
            y += lineH;
        }
        SelectObject(hdc, old);
    }

    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL &)
    {
        CreateFonts();
        BuildLinks();
        return 0;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL &)
    {
        DestroyComponent();
        DeleteFonts();
        return 0;
    }

    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL &)
    {
        if (m_component && m_pfnResize)
        {
            RECT rc;
            GetClientRect(&rc);
            m_pfnResize(m_component, &rc);
        }
        else
            InvalidateRect(NULL, TRUE);
        return 0;
    }

    LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL &)
    {
        return 1;
    }

    LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL &)
    {
        PAINTSTRUCT ps;
        RECT rc;
        HDC hdc = BeginPaint(&ps);
        GetClientRect(&rc);
        Draw(hdc, &rc);
        EndPaint(&ps);
        return 0;
    }

    LRESULT OnMouseMove(UINT, WPARAM, LPARAM lParam, BOOL &)
    {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int hot = HitTest(pt);
        if (hot != m_hot)
        {
            m_hot = hot;
            InvalidateRect(NULL, FALSE);
        }
        return 0;
    }

    LRESULT OnLButtonUp(UINT, WPARAM, LPARAM lParam, BOOL &)
    {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int hit = HitTest(pt);
        if (hit >= 0)
            NavigateTo(m_links[hit].action);
        return 0;
    }

    LRESULT OnSetCursor(UINT, WPARAM, LPARAM, BOOL &bHandled)
    {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(&pt);
        if (HitTest(pt) >= 0)
        {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        bHandled = FALSE;
        return 0;
    }

    LRESULT OnSetFocus(UINT, WPARAM, LPARAM, BOOL &bHandled)
    {
        if (m_hwndComponent)
            ::SetFocus(m_hwndComponent);
        bHandled = FALSE;
        return 0;
    }

    BEGIN_COM_MAP(CPersonalizationView)
    COM_INTERFACE_ENTRY_IID(IID_IShellView, IShellView)
    COM_INTERFACE_ENTRY_IID(IID_IOleWindow, IShellView)
    END_COM_MAP()

    BEGIN_MSG_MAP(CPersonalizationView)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
    MESSAGE_HANDLER(WM_PAINT, OnPaint)
    MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
    MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
    MESSAGE_HANDLER(WM_SETCURSOR, OnSetCursor)
    MESSAGE_HANDLER(WM_SETFOCUS, OnSetFocus)
    END_MSG_MAP()

public:
    DECLARE_WND_CLASS_EX(L"PersonalizationView", CS_DBLCLKS, COLOR_WINDOW)

    CPersonalizationView() :
        m_page(LWPAGE_MAIN), m_pidlRoot(NULL), m_fontTitle(NULL), m_fontLink(NULL), m_fontText(NULL),
        m_hot(-1), m_linkCount(0), m_hLib(NULL), m_pfnCreate(NULL), m_pfnResize(NULL), m_pfnTranslate(NULL),
        m_pfnDestroy(NULL), m_component(NULL), m_hwndComponent(NULL)
    {
        ZeroMemory(&m_fs, sizeof(m_fs));
        ZeroMemory(&m_host, sizeof(m_host));
    }

    ~CPersonalizationView()
    {
        DeleteFonts();
        if (m_hLib)
            FreeLibrary(m_hLib);
        if (m_pidlRoot)
            ILFree(m_pidlRoot);
    }

    HRESULT Initialize(PCIDLIST_ABSOLUTE pidlRoot, UINT page)
    {
        m_pidlRoot = ILClone(pidlRoot);
        m_page = page;
        return m_pidlRoot ? S_OK : E_OUTOFMEMORY;
    }

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
    STDMETHOD(TranslateAccelerator)(MSG *pmsg) override
    {
        if (m_component && m_pfnTranslate && m_pfnTranslate(m_component, pmsg))
            return S_OK;
        return S_FALSE;
    }
    STDMETHOD(EnableModeless)(BOOL) override { return S_OK; }
    STDMETHOD(UIActivate)(UINT uState) override
    {
        if (uState != SVUIA_DEACTIVATE && m_hWnd)
        {
            if (m_hwndComponent)
                ::SetFocus(m_hwndComponent);
            else
                SetFocus();
        }
        return S_OK;
    }
    STDMETHOD(Refresh)() override
    {
        if (m_hWnd)
            InvalidateRect(NULL, TRUE);
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
        Create(hParent, rc, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP);
        if (!m_hWnd)
            return E_FAIL;
        LoadComponent();
        CreateComponent();
        *phWnd = m_hWnd;
        return S_OK;
    }
    STDMETHOD(DestroyViewWindow)() override
    {
        DestroyComponent();
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

CPersonalizationFolder::CPersonalizationFolder() :
    m_pidlRoot(NULL),
    m_page(LWPAGE_MAIN)
{
}

CPersonalizationFolder::~CPersonalizationFolder()
{
    if (m_pidlRoot)
        ILFree(m_pidlRoot);
}

HRESULT WINAPI CPersonalizationFolder::GetClassID(CLSID *lpClassId)
{
    if (!lpClassId)
        return E_POINTER;
    *lpClassId = CLSID_PersonalizationFolder;
    return S_OK;
}

HRESULT WINAPI CPersonalizationFolder::Initialize(PCIDLIST_ABSOLUTE pidl)
{
    if (m_pidlRoot)
        ILFree(m_pidlRoot);
    m_pidlRoot = ILClone(pidl);
    if (!m_pidlRoot)
        return E_OUTOFMEMORY;
    UINT raw = PersRawPage(ILFindLastID(m_pidlRoot));
    UINT page = PersPidlPage(ILFindLastID(m_pidlRoot));
    m_page = raw == LWPAGE_CATEGORY ? LWPAGE_CATEGORY : (page < LWPAGE_COUNT ? page : LWPAGE_MAIN);
    return S_OK;
}

HRESULT WINAPI CPersonalizationFolder::GetCurFolder(PIDLIST_ABSOLUTE *pidl)
{
    if (!pidl)
        return E_POINTER;
    *pidl = ILClone(m_pidlRoot);
    return *pidl ? S_OK : E_FAIL;
}

HRESULT WINAPI CPersonalizationFolder::ParseDisplayName(HWND hwndOwner, LPBC pbc, LPOLESTR lpszDisplayName,
    DWORD *pchEaten, PIDLIST_RELATIVE *ppidl, DWORD *pdwAttributes)
{
    if (!lpszDisplayName || !ppidl)
        return E_INVALIDARG;
    *ppidl = NULL;
    if (m_page == LWPAGE_CATEGORY)
    {
        /* the category holds Personalization, by its name or by its class id */
        if (lstrcmpiW(lpszDisplayName, L"::" PERS_CLSID_TEXT) && lstrcmpiW(lpszDisplayName, L"Personalization") &&
            lstrcmpiW(lpszDisplayName, PersPageName(LWPAGE_MAIN)))
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        *ppidl = (PIDLIST_RELATIVE)PersCreatePidl(LWPAGE_MAIN);
        if (!*ppidl)
            return E_OUTOFMEMORY;
        if (pchEaten)
            *pchEaten = lstrlenW(lpszDisplayName);
        if (pdwAttributes)
            *pdwAttributes &= SFGAO_FOLDER | SFGAO_BROWSABLE;
        return S_OK;
    }
    if (m_page != LWPAGE_MAIN)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    for (UINT page = 1; page < LWPAGE_COUNT; page++)
    {
        /* both the English and the Spanish name parse, whatever the UI language is */
        static const PCWSTR en[LWPAGE_COUNT] = { L"", L"Desktop Background", L"Window Color", L"Display", L"Screen Resolution" };
        if (lstrcmpiW(lpszDisplayName, en[page]) && lstrcmpiW(lpszDisplayName, PersPageName(page)))
            continue;
        *ppidl = (PIDLIST_RELATIVE)PersCreatePidl(page);
        if (!*ppidl)
            return E_OUTOFMEMORY;
        if (pchEaten)
            *pchEaten = lstrlenW(lpszDisplayName);
        if (pdwAttributes)
            *pdwAttributes &= SFGAO_FOLDER | SFGAO_BROWSABLE;
        return S_OK;
    }
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

HRESULT WINAPI CPersonalizationFolder::EnumObjects(HWND hwndOwner, DWORD dwFlags, LPENUMIDLIST *ppEnumIDList)
{
    /* The pages are reached by navigation, they are not listed as children of the folder */
    return ShellObjectCreator<CEnumIDListBase>(IID_PPV_ARG(IEnumIDList, ppEnumIDList));
}

HRESULT WINAPI CPersonalizationFolder::BindToObject(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut)
{
    if (!pidl || !ppvOut)
        return E_INVALIDARG;
    *ppvOut = NULL;
    UINT page = PersPidlPage(pidl);
    if (m_page == LWPAGE_CATEGORY)
    {
        if (PersRawPage(pidl) != LWPAGE_MAIN)
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    else if (m_page != LWPAGE_MAIN || page >= LWPAGE_COUNT)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (!ILIsEmpty(ILGetNext(pidl)))
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    PIDLIST_ABSOLUTE abs = ILCombine(m_pidlRoot, pidl);
    if (!abs)
        return E_OUTOFMEMORY;
    CComPtr<CPersonalizationFolder> pChild;
    HRESULT hr = ShellObjectCreator(pChild);
    if (SUCCEEDED(hr))
        hr = pChild->Initialize(abs);
    ILFree(abs);
    if (FAILED(hr))
        return hr;
    return pChild->QueryInterface(riid, ppvOut);
}

HRESULT WINAPI CPersonalizationFolder::BindToStorage(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CPersonalizationFolder::CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2)
{
    UINT a = PersRawPage(pidl1), b = PersRawPage(pidl2);
    return MAKE_COMPARE_HRESULT((int)a - (int)b);
}

HRESULT WINAPI CPersonalizationFolder::CreateViewObject(HWND hwndOwner, REFIID riid, LPVOID *ppvOut)
{
    if (!ppvOut)
        return E_INVALIDARG;
    *ppvOut = NULL;
    if (!IsEqualIID(riid, IID_IShellView))
        return E_NOINTERFACE;

    /* the pages of a folder are addressed from the folder that holds them */
    PIDLIST_ABSOLUTE pidlRoot = ILClone(m_pidlRoot);
    if (!pidlRoot)
        return E_OUTOFMEMORY;
    if (m_page == LWPAGE_CATEGORY)
    {
        /* the category page of the Control Panel, in the Control Panel's own view */
        CComPtr<IShellFolder> psfDesktop, psfCpl;
        ILRemoveLastID(pidlRoot);
        HRESULT hrCat = SHGetDesktopFolder(&psfDesktop);
        if (SUCCEEDED(hrCat))
            hrCat = psfDesktop->BindToObject(pidlRoot, NULL, IID_PPV_ARG(IShellFolder, &psfCpl));
        ILFree(pidlRoot);
        if (FAILED_UNEXPECTEDLY(hrCat))
            return hrCat;
        CplSetNextViewCategory(1);
        hrCat = psfCpl->CreateViewObject(hwndOwner, riid, ppvOut);
        CplSetNextViewCategory(0);
        return hrCat;
    }
    if (m_page != LWPAGE_MAIN)
        ILRemoveLastID(pidlRoot);

    CComPtr<CPersonalizationView> pView;
    HRESULT hr = ShellObjectCreator(pView);
    if (SUCCEEDED(hr))
        hr = pView->Initialize(pidlRoot, m_page);
    ILFree(pidlRoot);
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;
    return pView->QueryInterface(riid, ppvOut);
}

HRESULT WINAPI CPersonalizationFolder::GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, DWORD *rgfInOut)
{
    if (!rgfInOut)
        return E_INVALIDARG;
    *rgfInOut &= SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_CANLINK;
    return S_OK;
}

static HRESULT CALLBACK PersMenuCallback(IShellFolder *psf, HWND hwnd, IDataObject *pdtobj, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    return SHELL32_DefaultContextMenuCallBack(psf, pdtobj, uMsg);
}

HRESULT WINAPI CPersonalizationFolder::GetUIObjectOf(HWND hwndOwner, UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
    REFIID riid, UINT *prgfInOut, LPVOID *ppvOut)
{
    if (!ppvOut)
        return E_INVALIDARG;
    *ppvOut = NULL;
    if (cidl < 1 || !apidl)
        return E_NOINTERFACE;
    if (IsEqualIID(riid, IID_IContextMenu))
    {
        CRegKeyHandleArray keys;
        CComPtr<IContextMenu> pCm;
        AddClsidKeyToArray(CLSID_PersonalizationFolder, keys, keys);
        AddClassKeyToArray(L"Folder", keys, keys);
        HRESULT hr = CDefFolderMenu_Create2(m_pidlRoot, hwndOwner, cidl, apidl, this, PersMenuCallback, keys, keys, &pCm);
        if (SUCCEEDED(hr))
            *ppvOut = pCm.Detach();
        return hr;
    }
    if (IsEqualIID(riid, IID_IDataObject))
        return IDataObject_Constructor(hwndOwner, m_pidlRoot, apidl, cidl, TRUE, (IDataObject **)ppvOut);
    return E_NOINTERFACE;
}

HRESULT WINAPI CPersonalizationFolder::GetDisplayNameOf(PCUITEMID_CHILD pidl, DWORD dwFlags, LPSTRRET strRet)
{
    UINT page = PersRawPage(pidl);
    if (!strRet || (page >= LWPAGE_COUNT && page != LWPAGE_CATEGORY))
        return E_INVALIDARG;
    return SHSetStrRet(strRet, page == LWPAGE_CATEGORY ? PersCategoryName() : PersPageName(page));
}

HRESULT WINAPI CPersonalizationFolder::SetNameOf(HWND hwndOwner, PCUITEMID_CHILD pidl, LPCOLESTR lpName,
    DWORD dwFlags, PITEMID_CHILD *pPidlOut)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CPersonalizationFolder::GetDefaultSearchGUID(GUID *pguid)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CPersonalizationFolder::EnumSearches(IEnumExtraSearch **ppenum)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CPersonalizationFolder::GetDefaultColumn(DWORD dwRes, ULONG *pSort, ULONG *pDisplay)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CPersonalizationFolder::GetDefaultColumnState(UINT iColumn, DWORD *pcsFlags)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CPersonalizationFolder::GetDetailsEx(PCUITEMID_CHILD pidl, const SHCOLUMNID *pscid, VARIANT *pv)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CPersonalizationFolder::GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn, SHELLDETAILS *psd)
{
    return E_NOTIMPL;
}

HRESULT WINAPI CPersonalizationFolder::MapColumnToSCID(UINT column, SHCOLUMNID *pscid)
{
    return E_NOTIMPL;
}
