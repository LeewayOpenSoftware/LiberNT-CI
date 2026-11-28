/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Unit test for the uxtheme process-init attach defer gate (ThemeLoader4 I1a-T)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windows.h>

/* Compile the real defer-gate translation unit directly, so the gate is exercised with no new DLL
 * exports and none of the tl4 trust dependencies. This is the gate uxtheme's DllMain(DLL_PROCESS_ATTACH)
 * sets so the trust-event work item is never queued while the process is still initialising - the
 * first-logon explorer loader-init deadlock seen on OOBE. */
#include "../../../../dll/win32/uxtheme/trustdefer.c"

START_TEST(TrustDefer)
{
    /* outside attach the gate does not defer */
    ok(UXTHEME_TrustInProcessInitAttach() == FALSE, "gate must start clear\n");

    /* across process-init attach the gate defers, so TrustEvent parks instead of queuing a work item */
    UXTHEME_TrustBeginAttachDefer();
    ok(UXTHEME_TrustInProcessInitAttach() == TRUE, "gate must be set across process-init attach\n");

    /* begin is idempotent */
    UXTHEME_TrustBeginAttachDefer();
    ok(UXTHEME_TrustInProcessInitAttach() == TRUE, "gate stays set on a second begin\n");

    /* after attach returns the gate clears, so the next trust call flushes the parked events */
    UXTHEME_TrustEndAttachDefer();
    ok(UXTHEME_TrustInProcessInitAttach() == FALSE, "gate must clear after attach\n");

    /* end is idempotent */
    UXTHEME_TrustEndAttachDefer();
    ok(UXTHEME_TrustInProcessInitAttach() == FALSE, "gate stays clear on a second end\n");
}
