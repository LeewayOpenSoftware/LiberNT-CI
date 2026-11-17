/*
 * PROJECT:     LiberNT uxtheme
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Process-init attach defer gate for the trust-event work item (ThemeLoader4 I1a-T)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windef.h>
#include <winbase.h>

/* Set across uxtheme's DLL_PROCESS_ATTACH. The initial process load runs attach WITHOUT the loader
 * lock held - ntdll LdrpInitializeProcess runs the init routines without entering LdrpLoaderLock - so
 * the loader-lock check in trust.c cannot see it, and queuing the trust-event work item there
 * deadlocks: the pool's first worker parks in LdrpInit until process init finishes, and process init
 * is the very thread waiting on the work item. While this flag is set the events are parked and
 * flushed by the first trust call after attach returns. Kept in its own translation unit, free of the
 * tl4 trust dependencies, so the gate can be unit-tested (rostests/apitests/uxtheme/TrustDefer.c). */
static LONG g_attachDefer;

void UXTHEME_TrustBeginAttachDefer(void)
{
    InterlockedExchange(&g_attachDefer, 1);
}

void UXTHEME_TrustEndAttachDefer(void)
{
    InterlockedExchange(&g_attachDefer, 0);
}

BOOL UXTHEME_TrustInProcessInitAttach(void)
{
    return g_attachDefer != 0;
}
