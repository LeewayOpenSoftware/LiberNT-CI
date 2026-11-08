/*
 * LeewayUI DPI — per-monitor DPI v2 helpers
 * LGPL-2.1+
 */

#pragma once

#include <windows.h>

UINT32 LeewayDpiGetForWindow(HWND window);
UINT32 LeewayDpiGetForMonitor(HMONITOR monitor);
int LeewayDpiLogicalToPhysical(UINT32 dpi, int logical_value);
int LeewayDpiPhysicalToLogical(UINT32 dpi, int physical_value);
int LeewayDpiScale(UINT32 dpi, int baseline_value);
