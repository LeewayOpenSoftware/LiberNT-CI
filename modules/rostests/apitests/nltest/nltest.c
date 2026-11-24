/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for nltest.exe: argument handling and the no-DC error paths
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 *
 * Read-only: only query verbs are run. On a workgroup machine every DC-bound
 * verb must fail with the real status, never report success.
 */

#include <apitest.h>
#include <windows.h>
#include <winsvc.h>
#include <lm.h>
#include <stdio.h>
#include <string.h>

#define MAX_OUTPUT (64 * 1024)

/* Runs nltest with the given arguments; stdout and stderr are merged into the returned ANSI buffer */
static char *run_nltest(const char *args, DWORD *exit_code)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE read_pipe, write_pipe;
    char cmdline[1024], system_dir[MAX_PATH];
    char *output;
    DWORD total = 0, got;

    *exit_code = (DWORD)-1;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
        return NULL;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    GetSystemDirectoryA(system_dir, ARRAYSIZE(system_dir));
    snprintf(cmdline, sizeof(cmdline), "\"%s\\nltest.exe\" %s", system_dir, args);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        trace("CreateProcess(%s) failed: %lu\n", cmdline, GetLastError());
        CloseHandle(write_pipe);
        CloseHandle(read_pipe);
        return NULL;
    }
    CloseHandle(write_pipe);
    CloseHandle(pi.hThread);

    output = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_OUTPUT + 1);
    if (output)
    {
        while (total < MAX_OUTPUT && ReadFile(read_pipe, output + total, MAX_OUTPUT - total, &got, NULL) && got)
            total += got;
        output[total] = 0;
    }
    CloseHandle(read_pipe);
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0)
    {
        ok(0, "nltest %s did not exit within 120 s, terminating it\n", args);
        TerminateProcess(pi.hProcess, (UINT)-1);
        WaitForSingleObject(pi.hProcess, 10000);
    }
    GetExitCodeProcess(pi.hProcess, exit_code);
    CloseHandle(pi.hProcess);
    return output;
}

static void free_output(char *output)
{
    HeapFree(GetProcessHeap(), 0, output);
}

static BOOL starts_with(const char *output, const char *start)
{
    return output && strncmp(output, start, strlen(start)) == 0;
}

static BOOL netlogon_running(void)
{
    SC_HANDLE scm, service;
    SERVICE_STATUS status;
    BOOL running = FALSE;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm)
        return FALSE;
    service = OpenServiceW(scm, L"Netlogon", SERVICE_QUERY_STATUS);
    if (service)
    {
        if (QueryServiceStatus(service, &status))
            running = (status.dwCurrentState == SERVICE_RUNNING);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
    return running;
}

static BOOL domain_joined(void)
{
    LPWSTR name = NULL;
    NETSETUP_JOIN_STATUS status = NetSetupUnknownStatus;
    BOOL joined;

    if (NetGetJoinInformation(NULL, &name, &status) != NERR_Success)
        return FALSE;
    joined = (status == NetSetupDomainName);
    NetApiBufferFree(name);
    return joined;
}

/* A DC-bound verb on a workgroup machine: a real failure line, never the success trailer */
static void test_no_dc_verb(const char *args, const char *failure_prefix, BOOL expect_rpc_unavailable)
{
    DWORD exit_code;
    char *output = run_nltest(args, &exit_code);

    if (!output)
    {
        skip("nltest %s could not be started\n", args);
        return;
    }
    ok(exit_code != 0, "nltest %s: exit code %lu, expected a failure\n", args, exit_code);
    ok(starts_with(output, failure_prefix), "nltest %s: output does not start with \"%s\": %s\n",
       args, failure_prefix, output);
    ok(strstr(output, "The command completed successfully") == NULL,
       "nltest %s reported success without a DC: %s\n", args, output);
    if (expect_rpc_unavailable)
    {
        /* As on Windows 11 in a workgroup: no Netlogon endpoint to talk to */
        ok(exit_code == RPC_S_SERVER_UNAVAILABLE, "nltest %s: exit code %lu, expected 1722\n", args, exit_code);
        ok(strstr(output, "Status = 1722 0x6ba RPC_S_SERVER_UNAVAILABLE") != NULL,
           "nltest %s: no 1722 status line: %s\n", args, output);
    }
    free_output(output);
}

/* A workgroup machine has no trusts: the real failure or an empty list, never an entry */
static void test_domain_trusts(void)
{
    DWORD exit_code;
    char *output = run_nltest("/domain_trusts", &exit_code);

    if (!output)
    {
        skip("nltest /domain_trusts could not be started\n");
        return;
    }
    if (exit_code != 0)
        ok(starts_with(output, "Enumerating domain trusts failed: Status = "), "/domain_trusts: %s\n", output);
    else
        ok(starts_with(output, "List of domain trusts:"), "/domain_trusts: %s\n", output);
    ok(strstr(output, "    0: ") == NULL, "/domain_trusts listed a trust without a DC: %s\n", output);
    free_output(output);
}

START_TEST(nltest)
{
    DWORD exit_code;
    char *output;
    BOOL no_netlogon;

    /* No arguments: a hint, exit code 1 */
    output = run_nltest("", &exit_code);
    if (!output)
    {
        skip("nltest.exe is not installed\n");
        return;
    }
    ok(exit_code == 1, "no arguments: exit code %lu\n", exit_code);
    ok(starts_with(output, "No parameters specified."), "no arguments: %s\n", output);
    free_output(output);

    /* Help: usage on stdout, exit code 0 */
    output = run_nltest("/?", &exit_code);
    ok(exit_code == 0, "/?: exit code %lu\n", exit_code);
    ok(starts_with(output, "Usage: nltest [/OPTIONS]"), "/?: %s\n", output);
    ok(strstr(output, "/SC_QUERY:<DomainName>") != NULL, "/?: no /SC_QUERY line\n");
    ok(strstr(output, "/DSGETDC:<DomainName>") != NULL, "/?: no /DSGETDC line\n");
    free_output(output);

    /* Unknown option and a verb missing its value: usage, exit code 1 */
    output = run_nltest("/bogus_option", &exit_code);
    ok(exit_code == 1, "/bogus_option: exit code %lu\n", exit_code);
    ok(starts_with(output, "Usage: nltest [/OPTIONS]"), "/bogus_option: %s\n", output);
    free_output(output);

    output = run_nltest("/sc_query", &exit_code);
    ok(exit_code == 1, "/sc_query without a domain: exit code %lu\n", exit_code);
    ok(starts_with(output, "Usage: nltest [/OPTIONS]"), "/sc_query without a domain: %s\n", output);
    free_output(output);

    /* /TIME is local arithmetic: 0x01D0 0000 0000 0000 is in 2015, two hex words, local time */
    output = run_nltest("/time:0 1d00000", &exit_code);
    ok(exit_code == 0, "/time: exit code %lu\n", exit_code);
    ok(starts_with(output, "0 1d00000 = "), "/time: %s\n", output);
    ok(strstr(output, "/2015 ") != NULL || strstr(output, "/2014 ") != NULL, "/time: not a 2014/2015 date: %s\n", output);
    free_output(output);

    /* /DBFLAG rejects a non-hex value before any RPC */
    output = run_nltest("/dbflag:zz", &exit_code);
    ok(exit_code == ERROR_INVALID_PARAMETER, "/dbflag:zz: exit code %lu\n", exit_code);
    ok(starts_with(output, "/DBFLAG:<HexFlags> must be"), "/dbflag:zz: %s\n", output);
    free_output(output);

    if (domain_joined())
    {
        skip("domain member: the no-DC error paths are not exercised here\n");
        return;
    }

    /* Workgroup: every DC-bound verb fails honestly under its own prefix. Without a
     * running Netlogon service the status is exactly Windows 11's 1722; with one it is
     * whatever the server returns, including ERROR_INVALID_DATA for success without data. */
    no_netlogon = !netlogon_running();
    trace("Netlogon service running: %s\n", no_netlogon ? "no" : "yes");
    test_no_dc_verb("/sc_query:WORKGROUP", "I_NetLogonControl failed: Status = ", no_netlogon);
    test_no_dc_verb("/query", "I_NetLogonControl failed: Status = ", no_netlogon);
    test_no_dc_verb("/dsgetdc:WORKGROUP", "Getting DC name failed: Status = ", no_netlogon);
    test_no_dc_verb("/dclist:WORKGROUP", "Cannot find DC to get DC list from. Status = ", no_netlogon);
    test_no_dc_verb("/dcname:WORKGROUP", "NetGetDCName failed: Status = ", no_netlogon);
    test_no_dc_verb("/dsgetsite", "Getting site name failed: Status = ", no_netlogon);
    test_domain_trusts();
}
