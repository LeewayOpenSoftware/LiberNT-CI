/*
 * LeewayUI DPI — minimal skeleton implementation
 * LGPL-2.1+
 */

#include <windows.h>
#include <leewayui/dpi.h>

UINT32 LeewayDpiGetForWindow(HWND window)
{
    if (!window) {
        return 96;  /* Default DPI */
    }

    /* Skeleton: return system DPI; real implementation would use GetDpiForWindow on Win10+ */
    HDC hdc = GetDC(window);
    if (!hdc) {
        return 96;
    }

    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(window, hdc);

    return (dpi > 0) ? dpi : 96;
}

UINT32 LeewayDpiGetForMonitor(HMONITOR monitor)
{
    if (!monitor) {
        return 96;
    }

    /* Skeleton: return 96; real implementation would use MonitorFromWindow + GetDpiForMonitor */
    return 96;
}

int LeewayDpiLogicalToPhysical(UINT32 dpi, int logical_value)
{
    if (dpi == 0) {
        dpi = 96;
    }
    return MulDiv(logical_value, dpi, 96);
}

int LeewayDpiPhysicalToLogical(UINT32 dpi, int physical_value)
{
    if (dpi == 0) {
        dpi = 96;
    }
    return MulDiv(physical_value, 96, dpi);
}

int LeewayDpiScale(UINT32 dpi, int baseline_value)
{
    return LeewayDpiLogicalToPhysical(dpi, baseline_value);
}
