/*
 * PROJECT:     LiberNT Explorer
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     File Explorer tab, address and search bands
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#include <shellframe.h>


#define CABINET_MAX_CRUMBS SHELLFRAME_MAX_CRUMBS

typedef ShellFrame::BandPalette CabinetPalette;

inline void CabinetGetPalette(CabinetPalette *palette) { ShellFrame::GetBandPalette(palette); }
inline BOOL CabinetComposedFrame(VOID) { return ShellFrame::ComposedFrame(); }
inline HIMAGELIST CabinetGlyphImages(const LPCWSTR *names, UINT count, int size, COLORREF color)
{
    return ShellFrame::GlyphImages(names, count, size, color);
}
inline void CabinetFillRound(HDC dc, const RECT &rc, int radius, COLORREF color)
{
    ShellFrame::FillRound(dc, rc, radius, color);
}
inline LRESULT CabinetToolbarCustomDraw(NMTBCUSTOMDRAW *draw, COLORREF base)
{
    return ShellFrame::ToolbarCustomDraw(draw, base);
}

class ICabinetBandHost : public ShellFrame::TabBandHost, public ShellFrame::AddressBandHost
{
};

struct CabinetTabItem
{
    LPCWSTR Title;
    int IconIndex;
};

typedef ShellFrame::Band CCabinetBand;
typedef ShellFrame::AddressBand CCabinetAddressBand;
typedef ShellFrame::SearchBox CCabinetSearchBox;

class CCabinetTabBand : public CCabinetBand
{
public:
    BOOL Create(HWND parent, ICabinetBandHost *host, HFONT font);
    void SetTabs(const CabinetTabItem *tabs, int count, int active);
    void SetShadeBottom(int bottom) { m_Band.SetShadeBottom(bottom); }
    HWND Window() const { return m_Band.Window(); }
    STDMETHODIMP GetWindow(HWND *window) override;

protected:
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) override;

private:
    ShellFrame::TabBand m_Band;
};
