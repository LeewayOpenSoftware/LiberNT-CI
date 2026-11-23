/*
 * PROJECT:     LiberNT Explorer
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Navigation pane with Quick access, shared by File Explorer and the file dialogs
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "shellframe.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>

namespace ShellFrame
{

NavigationPane::NavigationPane() :
    m_Control(NULL),
    m_Window(NULL),
    m_RootCount(0),
    m_PinnedCount(0),
    m_PinIcon(NULL)
{
}

NavigationPane::~NavigationPane()
{
    Destroy();
}

void
NavigationPane::AppendRoot(REFKNOWNFOLDERID id, SHCONTF flags)
{
    IShellItem *item;

    if (m_RootCount >= _countof(m_Roots) ||
        FAILED(SHGetKnownFolderItem(id, KF_FLAG_DEFAULT, NULL, IID_IShellItem, (void **)&item)))
    {
        return;
    }
    if (SUCCEEDED(m_Control->AppendRoot(item, flags, NSTCRS_VISIBLE, NULL)))
        m_Roots[m_RootCount++] = item;
    else
        item->Release();
}

/* Quick access pins first, then This PC and Network, as in Windows 11. */
BOOL
NavigationPane::Create(HWND parent, int itemHeight)
{
    static const KNOWNFOLDERID *QuickAccess[] =
    {
        &FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Documents, &FOLDERID_Pictures, &FOLDERID_Music,
        &FOLDERID_Videos
    };
    static const KNOWNFOLDERID *Locations[] = { &FOLDERID_ComputerFolder, &FOLDERID_NetworkFolder };
    IOleWindow *window = NULL;
    RECT rc = { 0, 0, 0, 0 };

    if (FAILED(CoCreateInstance(CLSID_NamespaceTreeControl, NULL, CLSCTX_INPROC_SERVER,
                                IID_INameSpaceTreeControl, (void **)&m_Control)))
    {
        m_Control = NULL;
        return FALSE;
    }
    if (FAILED(m_Control->Initialize(parent, &rc, NSTCS_HASEXPANDOS | NSTCS_ROOTHASEXPANDO |
                                     NSTCS_SINGLECLICKEXPAND | NSTCS_FULLROWSELECT |
                                     NSTCS_SHOWSELECTIONALWAYS | NSTCS_AUTOHSCROLL)))
    {
        m_Control->Release();
        m_Control = NULL;
        return FALSE;
    }
    m_Control->SetTheme(L"Explorer");
    for (UINT i = 0; i < _countof(QuickAccess); i++)
        AppendRoot(*QuickAccess[i], 0);
    m_PinnedCount = m_RootCount;
    for (UINT i = 0; i < _countof(Locations); i++)
        AppendRoot(*Locations[i], SHCONTF_FOLDERS);

    if (SUCCEEDED(m_Control->QueryInterface(IID_IOleWindow, (void **)&window)))
    {
        window->GetWindow(&m_Window);
        window->Release();
    }
    if (m_Window && itemHeight)
    {
        HWND tree = FindWindowExW(m_Window, NULL, WC_TREEVIEWW, NULL);

        if (tree)
            TreeView_SetItemHeight(tree, max(itemHeight, TreeView_GetItemHeight(tree)));
    }
    return TRUE;
}

void
NavigationPane::Destroy()
{
    if (m_Control)
    {
        m_Control->Release();
        m_Control = NULL;
    }
    m_Window = NULL;
    for (UINT i = 0; i < m_RootCount; i++)
        m_Roots[i]->Release();
    m_RootCount = 0;
    m_PinnedCount = 0;
    if (m_PinIcon)
    {
        DestroyIcon(m_PinIcon);
        m_PinIcon = NULL;
    }
}

void
NavigationPane::SetBackground(COLORREF color)
{
    HWND tree = m_Window ? FindWindowExW(m_Window, NULL, WC_TREEVIEWW, NULL) : NULL;

    if (tree)
        TreeView_SetBkColor(tree, color);
}

void
NavigationPane::SetPinGlyph(int size, COLORREF color)
{
    if (m_PinIcon)
        DestroyIcon(m_PinIcon);
    m_PinIcon = CreateGlyphIcon(L"pin_16_regular", size, color);
}

/* Pinned roots get a pin glyph; a line separates them from the other locations. */
void
NavigationPane::PaintRoot(HDC dc, const RECT &rc, const NSTCCUSTOMDRAW *item, int dpi)
{
    BandPalette palette;
    UINT index;
    int order;

    if (!item || !item->psi || item->iLevel)
        return;
    for (index = 0; index < m_RootCount; index++)
    {
        if (m_Roots[index] == item->psi ||
            (SUCCEEDED(m_Roots[index]->Compare(item->psi, SICHINT_CANONICAL, &order)) && !order))
            break;
    }
    if (index >= m_RootCount)
        return;
    if (index < m_PinnedCount)
    {
        if (m_PinIcon)
            DrawIconEx(dc, rc.right - MulDiv(28, dpi, 96), rc.top + (rc.bottom - rc.top - MulDiv(16, dpi, 96)) / 2,
                       m_PinIcon, MulDiv(16, dpi, 96), MulDiv(16, dpi, 96), 0, NULL, DI_NORMAL);
    }
    else if (index == m_PinnedCount && index)
    {
        RECT line = { rc.left + MulDiv(12, dpi, 96), rc.top, rc.right - MulDiv(12, dpi, 96), rc.top + 1 };

        GetBandPalette(&palette);
        SetDCBrushColor(dc, palette.BandLine);
        FillRect(dc, &line, (HBRUSH)GetStockObject(DC_BRUSH));
    }
}

void
NavigationPane::Select(PCIDLIST_ABSOLUTE pidl)
{
    IShellItem *item = NULL;

    if (m_Control && pidl && SUCCEEDED(SHCreateItemFromIDList(pidl, IID_IShellItem, (void **)&item)))
    {
        m_Control->SetItemState(item, NSTCIS_SELECTED, NSTCIS_SELECTED);
        item->Release();
    }
}

} // namespace ShellFrame
