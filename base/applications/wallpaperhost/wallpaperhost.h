/*
 * PROJECT:     Orosi Wallpaper Host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Internal interface for the wallpaper host core and its
 *              test-only fault-injection seam.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Include <windows.h> first. WPH_FAULT_INJECTION is defined only by the
 * wallpaperhost_faulttest build, never by production.
 */

#ifndef _OROSI_WALLPAPERHOST_H_
#define _OROSI_WALLPAPERHOST_H_

#ifdef __cplusplus
extern "C" {
#endif

/* The host core. Returns the process exit code (see the EXIT_* values in
 * wallpaperhost.cpp). Production wWinMain is a thin wrapper around this. */
int WallpaperHostRun(HINSTANCE hInstance, PWSTR pCmdLine);

#ifdef WPH_FAULT_INJECTION
/* Forced outcomes for each fallible step, read by the seam helpers. A zeroed
 * instance means "no injection": every step takes its real/success path. */
typedef struct _WPH_INJECT
{
    BOOL    bMutexNull;     /* single-instance mutex returns NULL -> EXIT_MUTEX */
    BOOL    bMutexAlready;  /* report ERROR_ALREADY_EXISTS        -> clean exit 0 */
    HRESULT hrComInit;      /* forced CoInitializeEx result (S_OK = success) */
    HRESULT hrActivate;     /* forced activation result       (S_OK = success) */
    HRESULT hrApply;        /* forced ApplyChanges result (S_OK/S_FALSE/failure) */
    BOOL    bWindowFail;    /* session window cannot be created   -> EXIT_WINDOW */
} WPH_INJECT;

extern WPH_INJECT g_WphInject;  /* defined by the test driver */
#endif /* WPH_FAULT_INJECTION */

#ifdef __cplusplus
}
#endif

#endif /* _OROSI_WALLPAPERHOST_H_ */
