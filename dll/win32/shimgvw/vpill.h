/*
 * PROJECT:     LiberNT Photo Viewer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Win7-style chrome: text menu strip on top and the floating pill toolbar
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VPILL VPILL;

typedef struct VPILL_HOST
{
    void *ctx;
    UINT (*GetZoom)(void *ctx);                 /* current zoom, percent */
    BOOL (*IsFit)(void *ctx);                   /* best-fit mode is active */
    BOOL (*IsEnabled)(void *ctx, UINT cmd);     /* IDC_* / menu command state */
    BOOL (*HasImage)(void *ctx);
    void (*SetZoom)(void *ctx, UINT percent);
    void (*Execute)(void *ctx, UINT cmd);       /* routes as WM_COMMAND to the main window */
} VPILL_HOST;

/* Creates the menu strip (child of main) and the pill (popup owned by main). */
VPILL *VPill_Create(HWND main, HINSTANCE instance, const VPILL_HOST *host);
void VPill_Destroy(VPILL *p);

/* Height of the menu strip in pixels at the current DPI. */
UINT VPill_StripHeight(VPILL *p);
/* Lays the strip out over the top of the client area and places the pill at the
 * bottom of the area below it. Call from WM_SIZE and WM_MOVE. */
void VPill_Layout(VPILL *p, int cx, int cy);
/* Re-reads the host state and repaints the pill and the strip. */
void VPill_Refresh(VPILL *p);
void VPill_OnDpiChanged(VPILL *p, UINT dpi);
/* The pill window, for tests and screenshots. */
HWND VPill_PillWindow(VPILL *p);
HWND VPill_StripWindow(VPILL *p);

/* Zoom <-> slider helpers (log scale 10..1000 %), exposed for the unit of the popup. */
#define VPILL_ZOOM_MIN 5
#define VPILL_ZOOM_MAX 800

#ifdef __cplusplus
}
#endif
