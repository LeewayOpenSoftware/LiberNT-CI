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

static void
TestRequiredPrivileges(SC_HANDLE hSvc)
{
    static const WCHAR privsW[] = L"SeBackupPrivilege\0SeRestorePrivilege\0";
    static const char privsA[] = "SeShutdownPrivilege\0";
    static const WCHAR badW[] = L"SeNoSuchPrivilegeAtAll\0";
    SERVICE_REQUIRED_PRIVILEGES_INFOW infoW;
    SERVICE_REQUIRED_PRIVILEGES_INFOA infoA;
    BYTE buf[512];
    DWORD needed;
    BOOL ret;

    /* Not set: a size probe with no buffer reports the needed size, never success */
    needed = 0;
    SetLastError(0xdeadbeef);
    ret = QueryServiceConfig2W(hSvc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, NULL, 0, &needed);
    ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "size probe: ret %d error %lu\n", ret, GetLastError());
    ok(needed >= sizeof(SERVICE_REQUIRED_PRIVILEGES_INFOW), "size probe needed = %lu\n", needed);

    /* Not set: success with a NULL list */
    needed = 0;
    ret = QueryServiceConfig2W(hSvc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, buf, sizeof(buf), &needed);
    ok(ret, "QueryServiceConfig2W(privileges) failed, error %lu\n", GetLastError());
    ok(((SERVICE_REQUIRED_PRIVILEGES_INFOW *)buf)->pmszRequiredPrivileges == NULL, "list should be NULL when unset\n");

    /* Set (W) and read back (W and A) */
    infoW.pmszRequiredPrivileges = (LPWSTR)privsW;
    ret = ChangeServiceConfig2W(hSvc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &infoW);
    ok(ret, "ChangeServiceConfig2W(privileges) failed, error %lu\n", GetLastError());

    ZeroMemory(buf, sizeof(buf));
    ret = QueryServiceConfig2W(hSvc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, buf, sizeof(buf), &needed);
    ok(ret, "QueryServiceConfig2W failed, error %lu\n", GetLastError());
    if (ret)
    {
        LPWSTR p = ((SERVICE_REQUIRED_PRIVILEGES_INFOW *)buf)->pmszRequiredPrivileges;

        ok(p != NULL && !lstrcmpW(p, L"SeBackupPrivilege"), "first name: %S\n", p ? p : L"(null)");
        if (p)
        {
            p += lstrlenW(p) + 1;
            ok(!lstrcmpW(p, L"SeRestorePrivilege"), "second name: %S\n", p);
            p += lstrlenW(p) + 1;
            ok(*p == L'\0', "list not terminated\n");
        }
    }

    ZeroMemory(buf, sizeof(buf));
    ret = QueryServiceConfig2A(hSvc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, buf, sizeof(buf), &needed);
    ok(ret, "QueryServiceConfig2A failed, error %lu\n", GetLastError());
    if (ret)
    {
        LPSTR p = ((SERVICE_REQUIRED_PRIVILEGES_INFOA *)buf)->pmszRequiredPrivileges;

        ok(p != NULL && !lstrcmpA(p, "SeBackupPrivilege"), "A first name: %s\n", p ? p : "(null)");
    }

    /* Too small a buffer reports the size */
    needed = 0;
    SetLastError(0xdeadbeef);
    ret = QueryServiceConfig2W(hSvc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, buf, 1, &needed);
    ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "small buffer: ret %d error %lu\n", ret, GetLastError());
    ok(needed > sizeof(SERVICE_REQUIRED_PRIVILEGES_INFOW), "needed = %lu\n", needed);

    /* Replace (A) */
    infoA.pmszRequiredPrivileges = (LPSTR)privsA;
    ret = ChangeServiceConfig2A(hSvc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &infoA);
    ok(ret, "ChangeServiceConfig2A(privileges) failed, error %lu\n", GetLastError());
    ZeroMemory(buf, sizeof(buf));
    ret = QueryServiceConfig2W(hSvc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, buf, sizeof(buf), &needed);
    ok(ret, "QueryServiceConfig2W failed, error %lu\n", GetLastError());
    if (ret)
    {
        LPWSTR p = ((SERVICE_REQUIRED_PRIVILEGES_INFOW *)buf)->pmszRequiredPrivileges;

        ok(p != NULL && !lstrcmpW(p, L"SeShutdownPrivilege"), "replaced name: %S\n", p ? p : L"(null)");
    }

    /* A name that is not a privilege is refused and leaves the value alone */
    infoW.pmszRequiredPrivileges = (LPWSTR)badW;
    SetLastError(0xdeadbeef);
    ret = ChangeServiceConfig2W(hSvc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &infoW);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "bad name: ret %d error %lu\n", ret, GetLastError());
    ZeroMemory(buf, sizeof(buf));
    ret = QueryServiceConfig2W(hSvc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, buf, sizeof(buf), &needed);
    ok(ret && ((SERVICE_REQUIRED_PRIVILEGES_INFOW *)buf)->pmszRequiredPrivileges != NULL, "value lost after a refused change\n");

    /* A NULL list clears it */
    infoW.pmszRequiredPrivileges = NULL;
    ret = ChangeServiceConfig2W(hSvc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &infoW);
    ok(ret, "clearing failed, error %lu\n", GetLastError());
    ZeroMemory(buf, sizeof(buf));
    ret = QueryServiceConfig2W(hSvc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, buf, sizeof(buf), &needed);
    ok(ret && ((SERVICE_REQUIRED_PRIVILEGES_INFOW *)buf)->pmszRequiredPrivileges == NULL, "value not cleared\n");
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
    TestRequiredPrivileges(hSvc);

    DeleteService(hSvc);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
}
