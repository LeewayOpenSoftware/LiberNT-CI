/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for NotifyRouteChange route-table change notifications
 * COPYRIGHT:   Copyright 2026 Leeway Open Software <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <ipmib.h>
#include <nldef.h>

static void wait_for_notification(HANDLE event, OVERLAPPED *ovr, const char *op)
{
    DWORD wait = WaitForSingleObject(event, 5000);

    ok(wait == WAIT_OBJECT_0, "%s notification timed out\n", op);
    if (wait == WAIT_OBJECT_0)
        ok(ovr->Internal == STATUS_SUCCESS, "%s notification completed with status %lx\n", op, ovr->Internal);
}

static BOOL register_route_change(HANDLE event, OVERLAPPED *ovr, HANDLE *handle)
{
    DWORD error;

    ZeroMemory(ovr, sizeof(*ovr));
    ovr->hEvent = event;
    *handle = NULL;

    error = NotifyRouteChange(handle, ovr);
    ok_long(error, ERROR_IO_PENDING);
    ok(*handle != NULL, "Expected a notification handle, got NULL\n");
    ok(ovr->Internal == STATUS_PENDING, "Expected STATUS_PENDING, got %lx\n", ovr->Internal);

    return error == ERROR_IO_PENDING;
}

static void cleanup_notification(HANDLE event, HANDLE handle, OVERLAPPED *ovr)
{
    CancelIPChangeNotify(ovr);
    if (event) CloseHandle(event);
    if (handle) CloseHandle(handle);
}

void func_NotifyRouteChange(void)
{
    OVERLAPPED ovr = {0};
    HANDLE event, handle = NULL;
    MIB_IPFORWARDROW route = {0};
    DWORD error, ifIndex;
    BOOL added = FALSE;

    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(event != NULL, "CreateEvent failed\n");
    if (!event) return;

    if (!register_route_change(event, &ovr, &handle))
        goto cleanup;

    error = GetBestInterface(htonl(INADDR_LOOPBACK), &ifIndex);
    ok_long(error, NO_ERROR);
    if (error != NO_ERROR)
        goto cleanup;

    route.dwForwardDest = htonl((127u << 24) | (254u << 16) | (254u << 8) | 254u);
    route.dwForwardMask = htonl(0xFFFFFFFF);
    route.dwForwardNextHop = htonl(INADDR_LOOPBACK);
    route.dwForwardIfIndex = ifIndex;
    route.dwForwardType = MIB_IPROUTE_TYPE_DIRECT;
    route.dwForwardProto = RouteProtocolNetmgmt;
    route.dwForwardMetric1 = 1;

    error = CreateIpForwardEntry(&route);
    if (error == ERROR_OBJECT_ALREADY_EXISTS)
    {
        win_skip("Loopback test route already exists\n");
    }
    else if (error != NO_ERROR)
    {
        win_skip("CreateIpForwardEntry failed (%lu), skipping route change checks\n", error);
        goto cleanup;
    }
    else
    {
        added = TRUE;
        wait_for_notification(event, &ovr, "add");
    }

    if (added)
    {
        if (!register_route_change(event, &ovr, &handle))
            goto cleanup;
    }

    error = DeleteIpForwardEntry(&route);
    if (error == NO_ERROR && added)
        wait_for_notification(event, &ovr, "delete");

cleanup:
    cleanup_notification(event, handle, &ovr);
}
