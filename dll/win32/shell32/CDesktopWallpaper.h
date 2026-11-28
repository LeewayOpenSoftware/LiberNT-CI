/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IDesktopWallpaper header
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

class CDesktopWallpaper :
    public CComCoClass<CDesktopWallpaper, &CLSID_DesktopWallpaper>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IDesktopWallpaper
{
public:
    /*** IDesktopWallpaper methods ***/
    STDMETHOD(SetWallpaper)(LPCWSTR monitorID, LPCWSTR wallpaper) override;
    STDMETHOD(GetWallpaper)(LPCWSTR monitorID, LPWSTR *wallpaper) override;
    STDMETHOD(GetMonitorDevicePathAt)(UINT monitorIndex, LPWSTR *monitorID) override;
    STDMETHOD(GetMonitorDevicePathCount)(UINT *count) override;
    STDMETHOD(GetMonitorRECT)(LPCWSTR monitorID, RECT *displayRect) override;
    STDMETHOD(SetBackgroundColor)(COLORREF color) override;
    STDMETHOD(GetBackgroundColor)(COLORREF *color) override;
    STDMETHOD(SetPosition)(DESKTOP_WALLPAPER_POSITION position) override;
    STDMETHOD(GetPosition)(DESKTOP_WALLPAPER_POSITION *position) override;
    STDMETHOD(SetSlideshow)(IShellItemArray *items) override;
    STDMETHOD(GetSlideshow)(IShellItemArray **items) override;
    STDMETHOD(SetSlideshowOptions)(DESKTOP_SLIDESHOW_OPTIONS options, UINT slideshowTick) override;
    STDMETHOD(GetSlideshowOptions)(DESKTOP_SLIDESHOW_OPTIONS *options, UINT *slideshowTick) override;
    STDMETHOD(AdvanceSlideshow)(LPCWSTR monitorID, DESKTOP_SLIDESHOW_DIRECTION direction) override;
    STDMETHOD(GetStatus)(DESKTOP_SLIDESHOW_STATE *state) override;
    STDMETHOD(Enable)(BOOL enable) override;

DECLARE_REGISTRY_RESOURCEID(IDR_DESKTOPWALLPAPER)
DECLARE_NOT_AGGREGATABLE(CDesktopWallpaper)

DECLARE_PROTECT_FINAL_CONSTRUCT()

BEGIN_COM_MAP(CDesktopWallpaper)
    COM_INTERFACE_ENTRY_IID(IID_IDesktopWallpaper, IDesktopWallpaper)
END_COM_MAP()
};
