/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the wlui launch table and call authorizer (P4)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <sddl.h>
#include <wluiauth.h>

static PSID g_System, g_Admins, g_Users;

static void
MakeSids(void)
{
    ConvertStringSidToSidW(L"S-1-5-18", &g_System);
    ConvertStringSidToSidW(L"S-1-5-32-544", &g_Admins);
    ConvertStringSidToSidW(L"S-1-5-32-545", &g_Users);
}

START_TEST(Authorize)
{
    WLUI_LAUNCH_TABLE Table;
    GUID Host, Recovery, Unknown = { 0x12345678, 0x1, 0x2, { 1, 2, 3, 4, 5, 6, 7, 8 } };
    WLUI_AUTH_ROLE Role;
    PVOID Context;
    WLUI_AUTH_OP Op;
    int Marker = 42;

    MakeSids();
    WluiLaunchTableInit(&Table);

    /* Add rejects garbage. */
    ok(!WluiLaunchTableAdd(&Table, 0, g_System, WluiAuthRoleHost, NULL, &Host), "pid 0 accepted\n");
    ok(!WluiLaunchTableAdd(&Table, 100, NULL, WluiAuthRoleHost, NULL, &Host), "NULL sid accepted\n");
    ok(!WluiLaunchTableAdd(&Table, 100, g_System, (WLUI_AUTH_ROLE)7, NULL, &Host), "bad role accepted\n");

    ok(WluiLaunchTableAdd(&Table, 100, g_System, WluiAuthRoleHost, &Marker, &Host), "add host\n");
    ok(WluiLaunchTableAdd(&Table, 200, g_System, WluiAuthRoleRecovery, NULL, &Recovery), "add recovery\n");
    ok(!IsEqualGUID(&Host, &Recovery), "launch ids must differ\n");

    /* Exact match: pid, sid and launch id. */
    ok(WluiAuthorize(&Table, 100, g_System, &Host, WluiOpReady, &Role, &Context) == WluiAuthOk, "host match\n");
    ok(Role == WluiAuthRoleHost, "role %u\n", Role);
    ok(Context == &Marker, "context %p\n", Context);

    /* Wrong pid, wrong sid, unknown launch id. */
    ok(WluiAuthorize(&Table, 101, g_System, &Host, WluiOpReady, NULL, NULL) == WluiAuthPidMismatch, "pid\n");
    ok(WluiAuthorize(&Table, 100, g_Users, &Host, WluiOpReady, NULL, NULL) == WluiAuthSidMismatch, "sid\n");
    ok(WluiAuthorize(&Table, 100, g_System, &Unknown, WluiOpReady, NULL, NULL) == WluiAuthUnknownLaunch, "guid\n");

    /* A caller cannot use another launch's id even with a matching sid. */
    ok(WluiAuthorize(&Table, 100, g_System, &Recovery, WluiOpReady, NULL, NULL) == WluiAuthPidMismatch,
       "host pid with recovery id\n");

    /* P4: the role comes from the entry. Recovery may not report provider faults. */
    for (Op = 0; Op < WluiOpCount; Op++)
    {
        ok(WluiAuthorize(&Table, 100, g_System, &Host, Op, NULL, NULL) == WluiAuthOk, "host op %u\n", Op);
    }
    ok(WluiAuthorize(&Table, 200, g_System, &Recovery, WluiOpReportProviderFault, &Role, NULL) == WluiAuthRoleDenied,
       "recovery provider fault\n");
    ok(Role == 0, "role leaked on denial\n");
    ok(WluiAuthorize(&Table, 200, g_System, &Recovery, WluiOpSubmitSerialization, &Role, NULL) == WluiAuthOk,
       "recovery submit\n");
    ok(Role == WluiAuthRoleRecovery, "recovery role %u\n", Role);

    /* Out-of-range op and NULL inputs. */
    ok(WluiAuthorize(&Table, 100, g_System, &Host, WluiOpCount, NULL, NULL) == WluiAuthUnknownLaunch, "bad op\n");
    ok(WluiAuthorize(&Table, 100, NULL, &Host, WluiOpReady, NULL, NULL) == WluiAuthUnknownLaunch, "NULL sid\n");

    /* Removal: the id dies with the entry; a reused pid with a new launch gets a new id. */
    ok(WluiLaunchTableRemove(&Table, &Host), "remove host\n");
    ok(!WluiLaunchTableRemove(&Table, &Host), "remove twice\n");
    ok(WluiAuthorize(&Table, 100, g_System, &Host, WluiOpReady, NULL, NULL) == WluiAuthUnknownLaunch, "after remove\n");
    {
        GUID Again;
        ok(WluiLaunchTableAdd(&Table, 100, g_Admins, WluiAuthRoleHost, NULL, &Again), "re-add pid 100\n");
        ok(!IsEqualGUID(&Again, &Host), "stale id reused\n");
        ok(WluiAuthorize(&Table, 100, g_System, &Again, WluiOpReady, NULL, NULL) == WluiAuthSidMismatch,
           "old sid against new launch\n");
        ok(WluiAuthorize(&Table, 100, g_Admins, &Again, WluiOpReady, NULL, NULL) == WluiAuthOk, "new launch\n");
    }

    /* The table is bounded. */
    {
        GUID Ids[WLUI_MAX_LAUNCHES + 1];
        ULONG i, Added = 0;
        WluiLaunchTableDelete(&Table);
        WluiLaunchTableInit(&Table);
        for (i = 0; i <= WLUI_MAX_LAUNCHES; i++)
            Added += WluiLaunchTableAdd(&Table, 1000 + i, g_System, WluiAuthRoleHost, NULL, &Ids[i]) ? 1 : 0;
        ok(Added == WLUI_MAX_LAUNCHES, "added %lu\n", Added);
    }

    ok(wcscmp(WluiAuthResultName(WluiAuthRoleDenied), L"role-denied") == 0, "result name\n");

    WluiLaunchTableDelete(&Table);
    LocalFree(g_System);
    LocalFree(g_Admins);
    LocalFree(g_Users);
}
