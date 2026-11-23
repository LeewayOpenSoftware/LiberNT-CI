/*
 * PROJECT:     LiberNT Explorer
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Events of the shared navigation pane
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "shellframe.h"

#include <shlobj.h>
#include <shobjidl.h>

namespace ShellFrame
{

NavigationPaneEvents::NavigationPaneEvents(NavigationPaneHost *host) :
    m_Ref(1),
    m_Host(host)
{
}

NavigationPaneEvents::~NavigationPaneEvents()
{
}

STDMETHODIMP
NavigationPaneEvents::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_INameSpaceTreeControlEvents))
        *ppv = static_cast<INameSpaceTreeControlEvents *>(this);
    else if (IsEqualIID(riid, IID_INameSpaceTreeControlCustomDraw))
        *ppv = static_cast<INameSpaceTreeControlCustomDraw *>(this);
    else
    {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG)
NavigationPaneEvents::AddRef()
{
    return InterlockedIncrement(&m_Ref);
}

STDMETHODIMP_(ULONG)
NavigationPaneEvents::Release()
{
    ULONG ref = InterlockedDecrement(&m_Ref);

    if (!ref)
        delete this;
    return ref;
}

STDMETHODIMP
NavigationPaneEvents::OnItemClick(IShellItem *item, NSTCEHITTEST hit, NSTCECLICKTYPE click)
{
    if (!item || !m_Host ||
        !(hit & (NSTCEHT_ONITEMICON | NSTCEHT_ONITEMLABEL | NSTCEHT_ONITEMRIGHT | NSTCEHT_ONITEMIDENT)))
    {
        return S_FALSE;
    }
    if ((click & NSTCECT_BUTTON) == NSTCECT_LBUTTON)
        m_Host->OnNavigationItem(item, FALSE);
    else if ((click & NSTCECT_BUTTON) == NSTCECT_MBUTTON)
        m_Host->OnNavigationItem(item, TRUE);
    return S_FALSE;
}

STDMETHODIMP NavigationPaneEvents::OnPropertyItemCommit(IShellItem *) { return S_FALSE; }
STDMETHODIMP NavigationPaneEvents::OnItemStateChanging(IShellItem *, NSTCITEMSTATE, NSTCITEMSTATE) { return S_OK; }
STDMETHODIMP NavigationPaneEvents::OnItemStateChanged(IShellItem *, NSTCITEMSTATE, NSTCITEMSTATE) { return S_OK; }
STDMETHODIMP NavigationPaneEvents::OnSelectionChanged(IShellItemArray *) { return S_OK; }
STDMETHODIMP NavigationPaneEvents::OnKeyboardInput(UINT, WPARAM, LPARAM) { return S_FALSE; }
STDMETHODIMP NavigationPaneEvents::OnBeforeExpand(IShellItem *) { return S_OK; }
STDMETHODIMP NavigationPaneEvents::OnAfterExpand(IShellItem *) { return S_OK; }
STDMETHODIMP NavigationPaneEvents::OnBeginLabelEdit(IShellItem *) { return S_OK; }
STDMETHODIMP NavigationPaneEvents::OnEndLabelEdit(IShellItem *) { return S_OK; }
STDMETHODIMP NavigationPaneEvents::OnGetToolTip(IShellItem *, LPWSTR, int) { return E_NOTIMPL; }
STDMETHODIMP NavigationPaneEvents::OnBeforeItemDelete(IShellItem *) { return S_OK; }
STDMETHODIMP NavigationPaneEvents::OnItemAdded(IShellItem *, BOOL) { return S_OK; }
STDMETHODIMP NavigationPaneEvents::OnItemDeleted(IShellItem *, BOOL) { return S_OK; }

STDMETHODIMP
NavigationPaneEvents::OnBeforeContextMenu(IShellItem *, REFIID, void **ppv)
{
    *ppv = NULL;
    return E_NOTIMPL;
}

STDMETHODIMP
NavigationPaneEvents::OnAfterContextMenu(IShellItem *, IContextMenu *menu, REFIID riid, void **ppv)
{
    return menu->QueryInterface(riid, ppv);
}

STDMETHODIMP NavigationPaneEvents::OnBeforeStateImageChange(IShellItem *) { return S_OK; }
STDMETHODIMP NavigationPaneEvents::OnGetDefaultIconIndex(IShellItem *, int *, int *) { return E_NOTIMPL; }

STDMETHODIMP
NavigationPaneEvents::PrePaint(HDC, RECT *, LRESULT *plres)
{
    *plres = CDRF_DODEFAULT;
    return S_OK;
}

STDMETHODIMP NavigationPaneEvents::PostPaint(HDC, RECT *) { return S_OK; }

STDMETHODIMP
NavigationPaneEvents::ItemPrePaint(HDC, RECT *, NSTCCUSTOMDRAW *, COLORREF *, COLORREF *, LRESULT *plres)
{
    *plres = CDRF_DODEFAULT;
    return S_OK;
}

STDMETHODIMP
NavigationPaneEvents::ItemPostPaint(HDC hdc, RECT *prc, NSTCCUSTOMDRAW *item)
{
    if (m_Host && prc)
        m_Host->PaintNavigationItem(hdc, *prc, item);
    return S_OK;
}

} // namespace ShellFrame
