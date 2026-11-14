/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Display backend over USER/GDI
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <leewayui/display_backend.h>

BOOL LDispLayoutWorks(void);

DWORD
LDispCapabilities(void)
{
    return LDISP_CAP_RESOLUTION | LDISP_CAP_REFRESH | (LSaverIsAvailable() ? LDISP_CAP_SCREENSAVER : 0) |
           (LDispLayoutWorks() ? LDISP_CAP_MULTIMON_POSITION : 0);
}

int
LDispEnumMonitors(LDISP_MONITOR *out, int max)
{
    DISPLAY_DEVICEW adapter, monitor;
    int count = 0;
    DWORD i;

    for (i = 0; count < max; i++)
    {
        DEVMODEW mode;

        ZeroMemory(&adapter, sizeof(adapter));
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(NULL, i, &adapter, 0))
            break;
        if (adapter.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)
            continue;
        ZeroMemory(&out[count], sizeof(out[count]));
        lstrcpynW(out[count].Device, adapter.DeviceName, ARRAYSIZE(out[count].Device));
        lstrcpynW(out[count].Adapter, adapter.DeviceString, ARRAYSIZE(out[count].Adapter));
        out[count].Attached = (adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        out[count].Primary = (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        ZeroMemory(&monitor, sizeof(monitor));
        monitor.cb = sizeof(monitor);
        if (EnumDisplayDevicesW(adapter.DeviceName, 0, &monitor, 0))
            lstrcpynW(out[count].Monitor, monitor.DeviceString, ARRAYSIZE(out[count].Monitor));
        ZeroMemory(&mode, sizeof(mode));
        mode.dmSize = sizeof(mode);
        if (out[count].Attached && EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0))
        {
            out[count].Bounds.left = (mode.dmFields & DM_POSITION) ? mode.dmPosition.x : 0;
            out[count].Bounds.top = (mode.dmFields & DM_POSITION) ? mode.dmPosition.y : 0;
            out[count].Bounds.right = out[count].Bounds.left + (LONG)mode.dmPelsWidth;
            out[count].Bounds.bottom = out[count].Bounds.top + (LONG)mode.dmPelsHeight;
        }
        count++;
    }
    return count;
}

static void
FromDevMode(const DEVMODEW *dm, LDISP_MODE *mode)
{
    mode->Width = dm->dmPelsWidth;
    mode->Height = dm->dmPelsHeight;
    mode->Bits = dm->dmBitsPerPel;
    mode->Frequency = dm->dmDisplayFrequency;
    mode->Orientation = (dm->dmFields & DM_DISPLAYORIENTATION) ? dm->dmDisplayOrientation : DMDO_DEFAULT;
}

int
LDispEnumModes(const WCHAR *device, LDISP_MODE *out, int max)
{
    int count = 0;
    DWORD i;

    for (i = 0; count < max; i++)
    {
        DEVMODEW dm;
        int j;
        BOOL duplicate = FALSE;

        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(device, i, &dm, 0))
            break;
        for (j = 0; j < count && !duplicate; j++)
        {
            duplicate = out[j].Width == dm.dmPelsWidth && out[j].Height == dm.dmPelsHeight &&
                        out[j].Bits == dm.dmBitsPerPel && out[j].Frequency == dm.dmDisplayFrequency;
        }
        if (!duplicate)
            FromDevMode(&dm, &out[count++]);
    }
    return count;
}

BOOL
LDispGetCurrent(const WCHAR *device, LDISP_MODE *mode)
{
    DEVMODEW dm;

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &dm, 0))
        return FALSE;
    FromDevMode(&dm, mode);
    return TRUE;
}

static LONG
Change(const WCHAR *device, const LDISP_MODE *mode, DWORD flags)
{
    DEVMODEW dm;

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &dm, 0))
        return DISP_CHANGE_FAILED;
    dm.dmPelsWidth = mode->Width;
    dm.dmPelsHeight = mode->Height;
    dm.dmBitsPerPel = mode->Bits;
    dm.dmDisplayFrequency = mode->Frequency;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    return ChangeDisplaySettingsExW(device, &dm, NULL, flags, NULL);
}

LONG
LDispTestMode(const WCHAR *device, const LDISP_MODE *mode)
{
    return Change(device, mode, CDS_TEST);
}

LONG
LDispApplyMode(const WCHAR *device, const LDISP_MODE *mode)
{
    return Change(device, mode, 0);
}

LONG
LDispCommitMode(const WCHAR *device, const LDISP_MODE *mode)
{
    return Change(device, mode, CDS_UPDATEREGISTRY);
}

UINT
LDispGetSystemDpi(void)
{
    HDC hdc = GetDC(NULL);
    UINT dpi = hdc ? (UINT)GetDeviceCaps(hdc, LOGPIXELSX) : 96;

    if (hdc)
        ReleaseDC(NULL, hdc);
    return dpi;
}
