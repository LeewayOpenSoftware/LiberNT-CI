/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Backend contract between the Display pages and the display stack
 *
 * The pages only talk to IDisplayBackend. The Win32 implementation (EnumDisplayDevices, EnumDisplaySettingsEx,
 * ChangeDisplaySettingsEx) is the default; the display backend owner can replace or extend it per capability
 * without touching the UI. Every call reports real success or failure; a capability the stack cannot provide
 * is reported through Capabilities(), never faked.
 */

#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace DisplayBackend
{

struct Monitor
{
    std::wstring Device;      /* \.\DISPLAYn */
    std::wstring Adapter;     /* adapter description */
    std::wstring MonitorName; /* attached monitor description, may be empty */
    bool Attached;
    bool Primary;
    RECT Bounds;              /* desktop coordinates, valid when Attached */
};

struct Mode
{
    DWORD Width;
    DWORD Height;
    DWORD Bits;
    DWORD Frequency;          /* 0 or 1 = hardware default */
    DWORD Orientation;        /* DMDO_* */
};

enum Capability : DWORD
{
    CapResolution = 1 << 0,
    CapRefreshRate = 1 << 1,
    CapOrientation = 1 << 2,
    CapMultiMonitor = 1 << 3,     /* attach/detach/position/primary */
    CapPerMonitorDpi = 1 << 4,
    CapSystemDpi = 1 << 5,        /* system scale, restart required */
};

/* LONG results are DISP_CHANGE_* values (DISP_CHANGE_SUCCESSFUL == 0). */
class IDisplayBackend
{
public:
    virtual ~IDisplayBackend() {}
    virtual DWORD Capabilities() = 0;
    virtual std::vector<Monitor> EnumMonitors() = 0;
    virtual std::vector<Mode> EnumModes(const std::wstring &device) = 0;
    virtual bool GetCurrent(const std::wstring &device, Mode &mode) = 0;
    /* Ask the driver whether the mode would work without changing anything (CDS_TEST). */
    virtual LONG TestMode(const std::wstring &device, const Mode &mode) = 0;
    /* Apply a mode and persist it. The caller must offer a revert with the previous mode it saved via GetCurrent. */
    virtual LONG ApplyMode(const std::wstring &device, const Mode &mode) = 0;
    virtual LONG SetPrimary(const std::wstring &device) = 0;
    virtual LONG SetAttached(const std::wstring &device, bool attached) = 0;
    virtual UINT GetDpi(const std::wstring &device) = 0;
};

/* Default implementation over USER/GDI (display_backend_win32.cpp). */
IDisplayBackend *CreateWin32Backend();

}
