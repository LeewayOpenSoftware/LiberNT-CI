/*
 * PROJECT:     Orosi Wallpaper Host - tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Deterministic coverage of wallpaperhost.exe exit codes through
 *              the test-only fault-injection seam (WPH_FAULT_INJECTION). This
 *              module is never installed (no add_cd_file), so the production
 *              binary carries none of it - the same discipline as credprovs_test.dll.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <stdio.h>

#include "wallpaperhost.h"   /* WallpaperHostRun(), WPH_INJECT, g_WphInject */

/* Definition of the injection block declared (extern "C") by wallpaperhost.cpp. */
extern "C" { WPH_INJECT g_WphInject; }

/* Exit codes under test (must match wallpaperhost.cpp). */
#define EXIT_CLEAN        0
#define EXIT_COM_INIT     1
#define EXIT_ACTIVATE     2
#define EXIT_APPLY        3
#define EXIT_WINDOW       4
#define EXIT_MUTEX        5

static int g_Failures = 0;

static void
Reset(void)
{
    ZeroMemory(&g_WphInject, sizeof(g_WphInject));
}

static void
Check(const char *Name, int Expected)
{
    int rc = WallpaperHostRun(GetModuleHandleW(NULL), (PWSTR)L"-faulttest");
    const char *verdict = (rc == Expected) ? "PASS" : "FAIL";
    if (rc != Expected)
        g_Failures++;
    printf("  [%s] %-28s expected exit %d, got %d\n", verdict, Name, Expected, rc);
}

int
main(void)
{
    printf("wallpaperhost_faulttest: exit-code coverage via fault injection\n");

    /* 0 - clean run: mutex acquired, COM up, object activated, wallpaper applied,
     *     window created, session loop returns. */
    Reset();
    Check("clean_exit", EXIT_CLEAN);

    /* 0 - a second instance finds the mutex already held and exits quietly. */
    Reset();
    g_WphInject.bMutexAlready = TRUE;
    Check("second_instance", EXIT_CLEAN);

    /* 0 - ApplyChanges reports S_FALSE ("nothing to apply"): not an error, the
     *     host still enters its session loop and exits cleanly. */
    Reset();
    g_WphInject.hrApply = S_FALSE;
    Check("apply_nothing_to_do", EXIT_CLEAN);

    /* 5 - the single-instance mutex could not be created. */
    Reset();
    g_WphInject.bMutexNull = TRUE;
    Check("mutex_failure", EXIT_MUTEX);

    /* 1 - CoInitializeEx failed. */
    Reset();
    g_WphInject.hrComInit = E_OUTOFMEMORY;
    Check("com_init_failure", EXIT_COM_INIT);

    /* 2 - CoCreateInstance(CLSID_ActiveDesktop) failed (object not registered). */
    Reset();
    g_WphInject.hrActivate = REGDB_E_CLASSNOTREG;
    Check("activation_failure", EXIT_ACTIVATE);

    /* 3 - ApplyChanges failed. */
    Reset();
    g_WphInject.hrApply = E_FAIL;
    Check("apply_failure", EXIT_APPLY);

    /* 4 - the hidden session window could not be registered or created. */
    Reset();
    g_WphInject.bWindowFail = TRUE;
    Check("window_failure", EXIT_WINDOW);

    printf("wallpaperhost_faulttest: %s (%d failure%s)\n",
           g_Failures == 0 ? "ALL PASS" : "FAILURES",
           g_Failures, g_Failures == 1 ? "" : "s");
    return g_Failures;
}
