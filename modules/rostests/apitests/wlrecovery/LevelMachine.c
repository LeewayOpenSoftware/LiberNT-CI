/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the logon UI recovery levels (budgets, SAS retry, global cap)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <wlrecovery.h>

static ULONGLONG g_Now;
static ULONGLONG NTAPI FakeClock(VOID) { return g_Now; }
static void Advance(ULONGLONG Ms) { g_Now += Ms; }

START_TEST(LevelMachine)
{
    WLREC_STATE S;
    int i, Launches;

    g_Now = 1000000;
    WlRecInit(&S, FakeClock);
    ok(WlRecLevel(&S) == WlRecLevel1, "starts at %u\n", WlRecLevel(&S));

    /* N1: one crash is enough to drop to N2; a normal end changes nothing. */
    ok(WlRecNextAction(&S) == WlRecActLaunchHost, "N1 action\n");
    WlRecOnEnd(&S, WlRecEndNormal);
    ok(WlRecLevel(&S) == WlRecLevel1, "normal end moved the level\n");
    ok(WlRecNextAction(&S) == WlRecActLaunchHost, "N1 action again\n");
    WlRecOnEnd(&S, WlRecEndCrash);
    ok(WlRecLevel(&S) == WlRecLevel2, "N1 crash -> %u\n", WlRecLevel(&S));
    ok(WlRecNextAction(&S) == WlRecActLaunchHostFallback, "N2 action\n");

    /* N2: two faults inside 60 s drop to N3; two faults 61 s apart do not. */
    WlRecOnEnd(&S, WlRecEndHang);
    ok(WlRecLevel(&S) == WlRecLevel2, "N2 first hang left N2\n");
    Advance(61 * 1000);
    ok(WlRecNextAction(&S) == WlRecActLaunchHostFallback, "N2 action after 61 s\n");
    WlRecOnEnd(&S, WlRecEndCrash);
    ok(WlRecLevel(&S) == WlRecLevel2, "N2 fault outside the window dropped the level\n");
    Advance(1000);
    ok(WlRecNextAction(&S) == WlRecActLaunchHostFallback, "N2 action\n");
    WlRecOnEnd(&S, WlRecEndCrash);
    ok(WlRecLevel(&S) == WlRecLevel3, "N2 two faults in 60 s -> %u\n", WlRecLevel(&S));
    ok(WlRecNextAction(&S) == WlRecActLaunchRecovery, "N3 action\n");

    /* N3: a missing or invalid binary goes straight to N4. */
    WlRecOnEnd(&S, WlRecEndImageInvalid);
    ok(WlRecLevel(&S) == WlRecLevel4, "N3 image invalid -> %u\n", WlRecLevel(&S));
    ok(WlRecNextAction(&S) == WlRecActTerminal, "N4 action\n");

    /* A new SAS retries from N1. */
    WlRecOnSas(&S);
    ok(WlRecLevel(&S) == WlRecLevel1, "SAS did not restart at N1\n");
    ok(WlRecNextAction(&S) == WlRecActLaunchHost, "N1 after SAS\n");

    /* N2: providers missing (credprovs.dll gone) skips the window. */
    WlRecOnEnd(&S, WlRecEndCrash);
    ok(WlRecNextAction(&S) == WlRecActLaunchHostFallback, "N2\n");
    WlRecOnEnd(&S, WlRecEndProvidersMissing);
    ok(WlRecLevel(&S) == WlRecLevel3, "N2 providers missing -> %u\n", WlRecLevel(&S));

    /* P3: 50 SAS presses with a host that always crashes never exceed 10 launches in
     * 10 minutes; the state ends in N4 and stays there, SAS or not. */
    g_Now = 5000000;
    WlRecInit(&S, FakeClock);
    Launches = 0;
    for (i = 0; i < 50; i++)
    {
        WLREC_ACTION Action;
        WlRecOnSas(&S);
        Action = WlRecNextAction(&S);
        if (Action != WlRecActTerminal)
        {
            Launches++;
            WlRecOnEnd(&S, WlRecEndCrash);
        }
        Advance(500);
    }
    ok(Launches == WLREC_CAP_LAUNCHES - 1, "launches before the cap: %d (expected %d)\n", Launches, WLREC_CAP_LAUNCHES - 1);
    ok(WlRecLevel(&S) == WlRecLevel4, "capped level %u\n", WlRecLevel(&S));
    ok(S.Capped, "cap flag\n");
    WlRecOnSas(&S);
    ok(WlRecLevel(&S) == WlRecLevel4 && WlRecNextAction(&S) == WlRecActTerminal, "SAS after the cap left N4\n");
    Advance(WLREC_CAP_WINDOW_MS + 1000);
    WlRecOnSas(&S);
    ok(WlRecNextAction(&S) == WlRecActTerminal, "the cap is until reboot, not until the window passes\n");

    /* Below the cap rate, launches keep flowing: 9 launches, then 11 minutes, then 9 more. */
    g_Now = 9000000;
    WlRecInit(&S, FakeClock);
    for (i = 0; i < WLREC_CAP_LAUNCHES - 1; i++)
    {
        WlRecOnSas(&S);
        ok(WlRecNextAction(&S) != WlRecActTerminal, "launch %d refused below the cap\n", i);
        WlRecOnEnd(&S, WlRecEndCrash);
        Advance(1000);
    }
    Advance(11 * 60 * 1000);
    for (i = 0; i < WLREC_CAP_LAUNCHES - 1; i++)
    {
        WlRecOnSas(&S);
        ok(WlRecNextAction(&S) != WlRecActTerminal, "launch %d refused after the window moved\n", i);
        WlRecOnEnd(&S, WlRecEndCrash);
        Advance(1000);
    }
    ok(!S.Capped, "cap hit although the rate stayed below it\n");

    /* Use is not a crash loop: 50 launches that each end normally within one minute (locks,
     * unlocks, logons) never cap; a crash loop right after still caps at 10. */
    g_Now = 12000000;
    WlRecInit(&S, FakeClock);
    for (i = 0; i < 50; i++)
    {
        ok(WlRecNextAction(&S) == WlRecActLaunchHost, "normal launch %d refused\n", i);
        WlRecOnEnd(&S, WlRecEndNormal);
        Advance(1000);
    }
    ok(!S.Capped && WlRecLevel(&S) == WlRecLevel1, "normal use capped or moved the level\n");
    Launches = 0;
    for (i = 0; i < 50; i++)
    {
        WlRecOnSas(&S);
        if (WlRecNextAction(&S) != WlRecActTerminal)
        {
            Launches++;
            WlRecOnEnd(&S, WlRecEndCrash);
        }
        Advance(500);
    }
    ok(Launches == WLREC_CAP_LAUNCHES - 1, "crash loop after normal use: %d launches (expected %d)\n", Launches, WLREC_CAP_LAUNCHES - 1);
    ok(S.Capped, "crash loop after normal use did not cap\n");

    /* A normal end between crashes restarts the count: 8 crashes, one launch that ends
     * normally (the ninth), then 9 more crashes; without the reset the tenth launch caps. */
    g_Now = 13000000;
    WlRecInit(&S, FakeClock);
    for (i = 0; i < WLREC_CAP_LAUNCHES - 2; i++)
    {
        WlRecOnSas(&S);
        ok(WlRecNextAction(&S) != WlRecActTerminal, "crash %d before the normal end refused\n", i);
        WlRecOnEnd(&S, WlRecEndCrash);
        Advance(100);
    }
    WlRecOnSas(&S);
    ok(WlRecNextAction(&S) != WlRecActTerminal, "launch for the normal end refused\n");
    WlRecOnEnd(&S, WlRecEndNormal);
    for (i = 0; i < WLREC_CAP_LAUNCHES - 1; i++)
    {
        WlRecOnSas(&S);
        ok(WlRecNextAction(&S) != WlRecActTerminal, "crash %d after the normal end refused\n", i);
        WlRecOnEnd(&S, WlRecEndCrash);
        Advance(100);
    }
    ok(!S.Capped, "a normal end did not restart the count\n");

    ok(wcscmp(WlRecLevelName(WlRecLevel3), L"N3") == 0, "level name\n");
    ok(wcscmp(WlRecEndName(WlRecEndHang), L"hang") == 0, "end name\n");
}
