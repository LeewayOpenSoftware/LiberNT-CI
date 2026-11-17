/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Round-trip tests for ChangeServiceConfig2/QueryServiceConfig2:
 *              SERVICE_CONFIG_DELAYED_AUTO_START_INFO and SERVICE_CONFIG_PRESHUTDOWN_INFO
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#define TEST_SERVICE_NAME L"ApitestSvcConfig2"

static SC_HANDLE CreateTestService(SC_HANDLE hScm)
{
    SC_HANDLE hSvc;

    /* Leftover of an aborted run */
    hSvc = OpenServiceW(hScm, TEST_SERVICE_NAME, DELETE);
    if (hSvc)
    {
        DeleteService(hSvc);
        CloseServiceHandle(hSvc);
    }

    return CreateServiceW(hScm,
                          TEST_SERVICE_NAME,
                          TEST_SERVICE_NAME,
                          SERVICE_ALL_ACCESS,
                          SERVICE_WIN32_OWN_PROCESS,
                          SERVICE_DEMAND_START,
                          SERVICE_ERROR_IGNORE,
                          L"C:\\Windows\\System32\\apitest-svcconfig2-unused.exe",
                          NULL, NULL, NULL, NULL, NULL);
}

static void
TestDelayedAutoStart(SC_HANDLE hSvc)
{
    SERVICE_DELAYED_AUTO_START_INFO info;
    BYTE buf[64];
    DWORD needed;
    BOOL ret;

    /* Default: not delayed */
    needed = 0;
    ret = QueryServiceConfig2W(hSvc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, buf, sizeof(buf), &needed);
    ok(ret, "QueryServiceConfig2W(delayed) failed, error %lu\n", GetLastError());
    ok(needed == sizeof(SERVICE_DELAYED_AUTO_START_INFO), "needed = %lu\n", needed);
    ok(((SERVICE_DELAYED_AUTO_START_INFO *)buf)->fDelayedAutostart == FALSE, "default should be FALSE\n");

    /* Set TRUE and read it back (W) */
    info.fDelayedAutostart = TRUE;
    ret = ChangeServiceConfig2W(hSvc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &info);
    ok(ret, "ChangeServiceConfig2W(delayed TRUE) failed, error %lu\n", GetLastError());
    ZeroMemory(buf, sizeof(buf));
    ret = QueryServiceConfig2W(hSvc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, buf, sizeof(buf), &needed);
    ok(ret, "QueryServiceConfig2W failed, error %lu\n", GetLastError());
    ok(((SERVICE_DELAYED_AUTO_START_INFO *)buf)->fDelayedAutostart != FALSE, "delayed flag was not stored\n");

    /* The A flavour sees the same value */
    ZeroMemory(buf, sizeof(buf));
    ret = QueryServiceConfig2A(hSvc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, buf, sizeof(buf), &needed);
    ok(ret, "QueryServiceConfig2A failed, error %lu\n", GetLastError());
    ok(((SERVICE_DELAYED_AUTO_START_INFO *)buf)->fDelayedAutostart != FALSE, "A: delayed flag mismatch\n");

    /* Set FALSE via the A flavour */
    info.fDelayedAutostart = FALSE;
    ret = ChangeServiceConfig2A(hSvc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &info);
    ok(ret, "ChangeServiceConfig2A(delayed FALSE) failed, error %lu\n", GetLastError());
    ZeroMemory(buf, sizeof(buf));
    ret = QueryServiceConfig2W(hSvc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, buf, sizeof(buf), &needed);
    ok(ret, "QueryServiceConfig2W failed, error %lu\n", GetLastError());
    ok(((SERVICE_DELAYED_AUTO_START_INFO *)buf)->fDelayedAutostart == FALSE, "delayed flag was not cleared\n");

    /* Too small a buffer reports the needed size */
    needed = 0;
    SetLastError(0xdeadbeef);
    ret = QueryServiceConfig2W(hSvc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, buf, 1, &needed);
    ok(!ret, "query with a 1-byte buffer should fail\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "error = %lu\n", GetLastError());
    ok(needed == sizeof(SERVICE_DELAYED_AUTO_START_INFO), "needed = %lu\n", needed);
}

static void
TestPreshutdown(SC_HANDLE hSvc)
{
    SERVICE_PRESHUTDOWN_INFO info;
    BYTE buf[64];
    DWORD needed;
    BOOL ret;

    /* Default timeout is 180 s, like Windows */
    needed = 0;
    ret = QueryServiceConfig2W(hSvc, SERVICE_CONFIG_PRESHUTDOWN_INFO, buf, sizeof(buf), &needed);
    ok(ret, "QueryServiceConfig2W(preshutdown) failed, error %lu\n", GetLastError());
    ok(needed == sizeof(SERVICE_PRESHUTDOWN_INFO), "needed = %lu\n", needed);
    ok(((SERVICE_PRESHUTDOWN_INFO *)buf)->dwPreshutdownTimeout == 180000,
       "default timeout = %lu\n", ((SERVICE_PRESHUTDOWN_INFO *)buf)->dwPreshutdownTimeout);

    info.dwPreshutdownTimeout = 12345;
    ret = ChangeServiceConfig2W(hSvc, SERVICE_CONFIG_PRESHUTDOWN_INFO, &info);
    ok(ret, "ChangeServiceConfig2W(preshutdown) failed, error %lu\n", GetLastError());
    ZeroMemory(buf, sizeof(buf));
    ret = QueryServiceConfig2A(hSvc, SERVICE_CONFIG_PRESHUTDOWN_INFO, buf, sizeof(buf), &needed);
    ok(ret, "QueryServiceConfig2A failed, error %lu\n", GetLastError());
    ok(((SERVICE_PRESHUTDOWN_INFO *)buf)->dwPreshutdownTimeout == 12345,
       "timeout = %lu\n", ((SERVICE_PRESHUTDOWN_INFO *)buf)->dwPreshutdownTimeout);

    info.dwPreshutdownTimeout = 60000;
    ret = ChangeServiceConfig2A(hSvc, SERVICE_CONFIG_PRESHUTDOWN_INFO, &info);
    ok(ret, "ChangeServiceConfig2A(preshutdown) failed, error %lu\n", GetLastError());
    ZeroMemory(buf, sizeof(buf));
    ret = QueryServiceConfig2W(hSvc, SERVICE_CONFIG_PRESHUTDOWN_INFO, buf, sizeof(buf), &needed);
    ok(ret, "QueryServiceConfig2W failed, error %lu\n", GetLastError());
    ok(((SERVICE_PRESHUTDOWN_INFO *)buf)->dwPreshutdownTimeout == 60000,
       "timeout = %lu\n", ((SERVICE_PRESHUTDOWN_INFO *)buf)->dwPreshutdownTimeout);
}

START_TEST(ServiceConfig2)
{
    SC_HANDLE hScm, hSvc;

    hScm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hScm)
    {
        skip("OpenSCManagerW failed, error %lu\n", GetLastError());
        return;
    }

    hSvc = CreateTestService(hScm);
    if (!hSvc)
    {
        skip("CreateServiceW failed, error %lu\n", GetLastError());
        CloseServiceHandle(hScm);
        return;
    }

    TestDelayedAutoStart(hSvc);
    TestPreshutdown(hSvc);

    DeleteService(hSvc);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
}
