/*
 * PROJECT:     LiberNT Explorer
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     File Explorer tab, address and search bands
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "cabinet.h"
#include <shellframe.h>
#include <commoncontrols.h>
#include <reactos/dwmframe.h>

BOOL
CCabinetTabBand::Create(HWND parent, ICabinetBandHost *host, HFONT font)
{
    m_Host = host;
    return m_Band.Create(parent, host, font, L"LiberNTExplorerTabBand");
}

STDMETHODIMP
CCabinetTabBand::GetWindow(HWND *window)
{
    if (!window)
        return E_POINTER;
    *window = m_Band.Window();
    return *window ? S_OK : E_FAIL;
}

void
CCabinetTabBand::SetTabs(const CabinetTabItem *tabs, int count, int active)
{
    ShellFrame::TabBandItem items[32];

    count = min(count, (int)_countof(items));
    for (int i = 0; i < count; i++)
    {
        items[i].Title = tabs[i].Title;
        items[i].IconIndex = tabs[i].IconIndex;
        items[i].Attention = FALSE;
    }
    m_Band.SetTabs(items, count, active);
}

LRESULT
CCabinetTabBand::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}
