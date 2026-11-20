/*
 * PROJECT:     LiberNT Explorer Frame
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Folder verb handler that opens a folder window through the shell
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

HWND FindShellProxy(LPITEMIDLIST pidl);

// LaunchExplorerFlags of the folder verbs (values as registered by Windows):
// 0x1 opens a new window (opennewwindow), 0x20 | 0x40 a new tab (opennewtab)
#define LAUNCHEXPLORER_NEWWINDOW 0x00000001
#define LAUNCHEXPLORER_NEWTAB    0x00000060

CExecuteFolder::CExecuteFolder() :
    m_Show(SW_SHOWNORMAL),
    m_Explore(FALSE),
    m_LaunchFlags(0)
{
}

// Ask the Explorer window the command came from to open every selected folder
// in a new tab or window. S_FALSE when there is no such window.
HRESULT CExecuteFolder::OpenInFrame(DWORD nCommand)
{
    CComPtr<IOleCommandTarget> pFrame;
    DWORD cItems = 0;

    if (FAILED(IUnknown_QueryService(m_Site, SID_SCabinetFrame, IID_PPV_ARG(IOleCommandTarget, &pFrame))))
        return S_FALSE;
    if (FAILED(m_Selection->GetCount(&cItems)))
        return E_FAIL;
    for (DWORD i = 0; i < cItems; i++)
    {
        CComPtr<IShellItem> Item;
        if (FAILED(m_Selection->GetItemAt(i, &Item)))
            continue;
        VARIANT vIn;
        V_VT(&vIn) = VT_UNKNOWN;
        V_UNKNOWN(&vIn) = Item;
        pFrame->Exec(&CGID_CabinetFrame, nCommand, OLECMDEXECOPT_DODEFAULT, &vIn, NULL);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CExecuteFolder::SetKeyState(DWORD grfKeyState)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CExecuteFolder::SetParameters(LPCWSTR pszParameters)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CExecuteFolder::SetPosition(POINT pt)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CExecuteFolder::SetShowWindow(int nShow)
{
    m_Show = nShow;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CExecuteFolder::SetNoShowUI(BOOL fNoShowUI)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CExecuteFolder::SetDirectory(LPCWSTR pszDirectory)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CExecuteFolder::Execute()
{
    EXPLORER_CMDLINE_PARSE_RESULTS Results = { 0 };
    CComPtr<IShellItem> Item;
    CComHeapPtr<ITEMIDLIST> Pidl;
    HRESULT hr;

    if (!m_Selection)
        return E_UNEXPECTED;

    // Open in new tab / new window: done by the window the menu belongs to
    if (m_LaunchFlags & (LAUNCHEXPLORER_NEWTAB | LAUNCHEXPLORER_NEWWINDOW))
    {
        hr = OpenInFrame((m_LaunchFlags & LAUNCHEXPLORER_NEWTAB) ? CABINETFRAME_CMD_OPENNEWTAB
                                                                 : CABINETFRAME_CMD_OPENNEWWINDOW);
        if (hr != S_FALSE)
            return hr;
        // no Explorer window (called through the shell): a new window, below
    }

    hr = m_Selection->GetItemAt(0, &Item);
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;
    hr = SHGetIDListFromObject(Item, &Pidl);
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;
    if (!FindShellProxy(Pidl))
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    Results.dwFlags = SH_EXPLORER_CMDLINE_FLAG_IDLIST;
    if (m_Explore)
        Results.dwFlags |= SH_EXPLORER_CMDLINE_FLAG_E;
    Results.nCmdShow = m_Show;
    Results.pidlPath = Pidl;
    return SHCreateFromDesktop(&Results) ? S_OK : E_FAIL;
}

HRESULT STDMETHODCALLTYPE CExecuteFolder::SetSelection(IShellItemArray *psia)
{
    m_Selection = psia;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CExecuteFolder::GetSelection(REFIID riid, void **ppv)
{
    if (!m_Selection)
    {
        *ppv = NULL;
        return E_FAIL;
    }
    return m_Selection->QueryInterface(riid, ppv);
}

HRESULT STDMETHODCALLTYPE CExecuteFolder::Initialize(LPCWSTR pszCommandName, IPropertyBag *ppb)
{
    m_Explore = pszCommandName && !_wcsicmp(pszCommandName, L"explore");
    m_LaunchFlags = 0;
    if (ppb)
    {
        VARIANT v;
        VariantInit(&v);
        V_VT(&v) = VT_UI4;
        if (SUCCEEDED(ppb->Read(L"LaunchExplorerFlags", &v, NULL)) &&
            SUCCEEDED(VariantChangeType(&v, &v, 0, VT_UI4)))
        {
            m_LaunchFlags = V_UI4(&v);
        }
        VariantClear(&v);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CExecuteFolder::SetSite(IUnknown *pUnkSite)
{
    m_Site = pUnkSite;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CExecuteFolder::GetSite(REFIID riid, void **ppvSite)
{
    if (!m_Site)
    {
        *ppvSite = NULL;
        return E_FAIL;
    }
    return m_Site->QueryInterface(riid, ppvSite);
}
