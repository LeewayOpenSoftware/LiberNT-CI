/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     UMFD channel failure tests against a registered fontdrvhost (UMFD_TEST_HOOKS images),
 *              and winlogon's supervision of its host
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windows.h>
#include <reactos/umfd.h>
#include <reactos/umfdtest.h>
#include <tlhelp32.h>

INT WINAPI NtGdiExtEscape(HDC hdc, PWCHAR pDriver, INT nDriver, INT Escape,
                          INT InSize, LPSTR InData, INT OutSize, LPSTR OutData);

/* win32k's LOAD deadline and the host's own budget (umfd.c, umfd.h) */
#define KERNEL_DEADLINE_MS  60000
#define SLACK_MS            8000

static WCHAR service_name[] = L"umfd_channel_test";
static SERVICE_STATUS_HANDLE status_handle;
static BYTE *good_font;
static DWORD good_size;

static void report_status(DWORD state, DWORD win32_error, DWORD specific)
{
    SERVICE_STATUS status = { 0 };

    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = specific ? ERROR_SERVICE_SPECIFIC_ERROR : win32_error;
    status.dwServiceSpecificExitCode = specific;
    SetServiceStatus(status_handle, &status);
}

static DWORD WINAPI service_handler(DWORD control, DWORD type, void *data, void *context)
{
    return (control == SERVICE_CONTROL_INTERROGATE) ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

/* As LocalSystem (SeTcbPrivilege): replaces any registered host with a fresh one */
static DWORD register_new_host(DWORD *pid)
{
    WCHAR path[MAX_PATH];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    UMFD_REGISTER reg;
    DWORD error = 0;

    GetSystemDirectoryW(path, MAX_PATH);
    lstrcatW(path, L"\\fontdrvhost.exe");

    /* The old host's next SERVE fails, and it exits */
    NtGdiExtEscape(NULL, NULL, 0, UMFD_ESCAPE_UNREGISTER, 0, NULL, 0, NULL);

    if (!CreateProcessW(path, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
        return GetLastError();
    reg.Magic = UMFD_MAGIC;
    reg.Version = UMFD_VERSION;
    reg.Reserved = 0;
    reg.ProcessHandle = (ULONG64)(ULONG_PTR)pi.hProcess;
    if (NtGdiExtEscape(NULL, NULL, 0, UMFD_ESCAPE_REGISTER, sizeof(reg), (LPSTR)&reg, 0, NULL) != 1)
    {
        error = GetLastError();
        if (error == 0)
            error = ERROR_GEN_FAILURE;
        TerminateProcess(pi.hProcess, error);
    }
    else
    {
        ResumeThread(pi.hThread);
        *pid = pi.dwProcessId;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return error;
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
    DWORD pid = 0, error;

    status_handle = RegisterServiceCtrlHandlerExW(service_name, service_handler, NULL);
    if (!status_handle)
        return;
    report_status(SERVICE_RUNNING, NO_ERROR, 0);
    error = register_new_host(&pid);
    /* The host's pid travels back as the service-specific exit code */
    report_status(SERVICE_STOPPED, error, error ? 0 : pid);
}

static BOOL enable_privilege(const WCHAR *name)
{
    TOKEN_PRIVILEGES tp = { 1 };
    HANDLE token;
    BOOL ret;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token))
        return FALSE;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    ret = LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid) &&
          AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL) &&
          GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ret;
}

/* Runs this binary as a LocalSystem service that registers a new host; returns the host */
static HANDLE start_host(void)
{
    WCHAR cmd[MAX_PATH + 64], exe[MAX_PATH];
    SERVICE_STATUS status = { 0 };
    SC_HANDLE scm, service;
    HANDLE host = NULL;
    int i;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm)
    {
        ok(0, "OpenSCManager failed: %lu\n", GetLastError());
        return NULL;
    }
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wsprintfW(cmd, L"\"%s\" umfd_channel service", exe);
    service = CreateServiceW(scm, service_name, service_name, SERVICE_ALL_ACCESS,
                             SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE,
                             cmd, NULL, NULL, NULL, NULL, NULL);
    if (!service && GetLastError() == ERROR_SERVICE_EXISTS)
        service = OpenServiceW(scm, service_name, SERVICE_ALL_ACCESS);
    if (!service)
    {
        ok(0, "CreateService failed: %lu\n", GetLastError());
        CloseServiceHandle(scm);
        return NULL;
    }

    if (StartServiceW(service, 0, NULL))
    {
        for (i = 0; i < 300; ++i)
        {
            if (!QueryServiceStatus(service, &status) || status.dwCurrentState == SERVICE_STOPPED)
                break;
            Sleep(100);
        }
        if (status.dwCurrentState == SERVICE_STOPPED && status.dwWin32ExitCode == ERROR_SERVICE_SPECIFIC_ERROR)
        {
            host = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, status.dwServiceSpecificExitCode);
            ok(host != NULL, "OpenProcess(host %lu) failed: %lu\n", status.dwServiceSpecificExitCode, GetLastError());
        }
        else
        {
            ok(0, "the service did not register a host: state %lu, error %lu\n",
               status.dwCurrentState, status.dwWin32ExitCode);
        }
    }
    else
    {
        ok(0, "StartService failed: %lu\n", GetLastError());
    }
    DeleteService(service);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return host;
}

/* Number of fonts added (then removed again), 0 if refused, and how long it took */
static DWORD add_font(const void *data, DWORD size, DWORD *ms)
{
    DWORD count = 0, start = GetTickCount();
    HANDLE font;

    font = AddFontMemResourceEx((PVOID)data, size, NULL, &count);
    *ms = GetTickCount() - start;
    if (!font)
        return 0;
    RemoveFontMemResourceEx(font);
    return count;
}

static DWORD add_command(ULONG command, ULONG param, DWORD *ms)
{
    BYTE blob[UMFD_TEST_BLOB_SIZE] = { 0 };
    UMFD_TEST_BLOB *header = (UMFD_TEST_BLOB *)blob;

    memcpy(header->Magic, UMFD_TEST_MAGIC, sizeof(header->Magic));
    header->Command = command;
    header->Param = param;
    return add_font(blob, sizeof(blob), ms);
}

static BOOL host_alive(HANDLE host, DWORD *exit_code)
{
    *exit_code = 0;
    if (WaitForSingleObject(host, 0) == WAIT_TIMEOUT)
        return TRUE;
    GetExitCodeProcess(host, exit_code);
    return FALSE;
}

/* The first plain TrueType file in the fonts folder (under 4 MB) */
static BOOL load_good_font(void)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find, file;
    DWORD read = 0;

    if (good_font)
        return TRUE;
    GetWindowsDirectoryW(dir, MAX_PATH);
    lstrcatW(dir, L"\\Fonts\\");
    wsprintfW(path, L"%s*.ttf", dir);
    find = FindFirstFileW(path, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return FALSE;
    do
    {
        if (fd.nFileSizeHigh || fd.nFileSizeLow == 0 || fd.nFileSizeLow > 4 * 1024 * 1024)
            continue;
        wsprintfW(path, L"%s%s", dir, fd.cFileName);
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (file == INVALID_HANDLE_VALUE)
            continue;
        good_font = HeapAlloc(GetProcessHeap(), 0, fd.nFileSizeLow);
        if (good_font && ReadFile(file, good_font, fd.nFileSizeLow, &read, NULL) && read == fd.nFileSizeLow)
        {
            good_size = read;
            trace("good font: %S (%lu bytes)\n", path, read);
        }
        else if (good_font)
        {
            HeapFree(GetProcessHeap(), 0, good_font);
            good_font = NULL;
        }
        CloseHandle(file);
    } while (!good_font && FindNextFileW(find, &fd));
    FindClose(find);
    return good_font != NULL;
}

static DWORD umfd_mode(void)
{
    DWORD mode = 0, size = sizeof(mode);

    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Leeway\\Win32k", L"UmfdMode",
                     RRF_RT_REG_DWORD, NULL, &mode, &size) != ERROR_SUCCESS)
        return 0;
    return mode;
}

/* These tests need a mode 1 boot of a UMFD_TEST_HOOKS image, run elevated */
static BOOL can_run(void)
{
    if (!is_reactos())
    {
        skip("the UMFD channel is Orosi's own\n");
        return FALSE;
    }
#ifndef UMFD_TEST_HOOKS
    skip("built without UMFD_TEST_HOOKS\n");
    return FALSE;
#endif
    if (umfd_mode() != 1)
    {
        skip("UmfdMode is not 1\n");
        return FALSE;
    }
    if (!enable_privilege(L"SeDebugPrivilege"))
    {
        skip("not elevated (no SeDebugPrivilege)\n");
        return FALSE;
    }
    if (!load_good_font())
    {
        skip("no TrueType font found\n");
        return FALSE;
    }
    return TRUE;
}

/* Ends with a working host registered, so the session's font adds keep working */
static void restore_host(void)
{
    HANDLE host = start_host();
    DWORD ms;

    if (!host)
        return;
    ok(add_font(good_font, good_size, &ms) > 0, "a good font was refused by a fresh host\n");
    CloseHandle(host);
}

typedef struct _CALL
{
    ULONG command;
    ULONG param;
    DWORD result;
    DWORD ms;
} CALL;

static DWORD WINAPI command_thread(void *arg)
{
    CALL *call = arg;

    call->result = add_command(call->command, call->param, &call->ms);
    return 0;
}

START_TEST(umfd_channel)
{
    char **argv;
    int argc = winetest_get_mainargs(&argv);
    SERVICE_TABLE_ENTRYW table[] = { { service_name, service_main }, { NULL, NULL } };
    DWORD ms, code = 0;
    HANDLE host;

    if (argc >= 3 && !strcmp(argv[2], "service"))
    {
        StartServiceCtrlDispatcherW(table);
        return;
    }
    if (!can_run())
        return;
    host = start_host();
    if (!host)
        return;

    ok(add_font(good_font, good_size, &ms) > 0, "a good font was refused\n");

    /* R1a: a fault in the host's FreeType costs one refusal, not the host */
    ok(add_command(UMFD_TEST_CRASH, 0, &ms) == 0, "a crashing file was accepted\n");
    ok(ms < KERNEL_DEADLINE_MS / 2, "the crash refusal took %lu ms\n", ms);
    ok(host_alive(host, &code), "the host died on a fault: exit 0x%lx\n", code);
    ok(add_font(good_font, good_size, &ms) > 0, "a good font was refused after a fault\n");

    /* Timeout after delivery: refused at the deadline, the view stays mapped while the
       host still reads it, the late reply is dropped, and the view goes at the next SERVE */
    ok(add_command(UMFD_TEST_STALL, KERNEL_DEADLINE_MS + 5000, &ms) == 0, "a stalled request was accepted\n");
    ok(ms >= KERNEL_DEADLINE_MS - 1000 && ms < KERNEL_DEADLINE_MS + SLACK_MS,
       "the stalled request was refused after %lu ms\n", ms);
    Sleep(5000 + SLACK_MS);
    ok(host_alive(host, &code), "the host died after the timeout: exit 0x%lx (0x%lx: view unmapped under it)\n",
       code, (DWORD)UMFD_TEST_EXIT_VIEW_GONE);
    ok(add_font(good_font, good_size, &ms) > 0, "a good font was refused after a stale reply\n");
    ok(host_alive(host, &code), "the host died at the next LOAD: exit 0x%lx (0x%lx: stalled view never unmapped)\n",
       code, (DWORD)UMFD_TEST_EXIT_VIEW_LEAKED);
    CloseHandle(host);
}

START_TEST(umfd_freeze)
{
    CALL frozen = { UMFD_TEST_FREEZE, KERNEL_DEADLINE_MS + 30000 };
    HANDLE host, thread;
    DWORD ms, count, code = 0;

    if (!can_run())
        return;
    host = start_host();
    if (!host)
        return;

    /* A frozen host holds the slot; a second add waits for it on the same deadline */
    thread = CreateThread(NULL, 0, command_thread, &frozen, 0, NULL);
    Sleep(2000);
    count = add_font(good_font, good_size, &ms);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    ok(frozen.result == 0, "a frozen request was accepted\n");
    ok(frozen.ms >= KERNEL_DEADLINE_MS - 1000 && frozen.ms < KERNEL_DEADLINE_MS + SLACK_MS,
       "the frozen request was refused after %lu ms\n", frozen.ms);
    ok(count == 0, "a font queued behind a frozen host was accepted\n");
    ok(ms < KERNEL_DEADLINE_MS + SLACK_MS, "the queued add waited %lu ms (one deadline covers both waits)\n", ms);

    /* After the first host, a dead host refuses every font at once */
    ok(WaitForSingleObject(host, 60000) == WAIT_OBJECT_0, "the frozen host did not exit\n");
    ok(GetExitCodeProcess(host, &code) && code == UMFD_TEST_EXIT_FROZEN, "host exit 0x%lx\n", code);
    ok(add_font(good_font, good_size, &ms) == 0, "a font was accepted with no host\n");
    ok(ms < 5000, "the no-host refusal took %lu ms\n", ms);
    CloseHandle(host);
    restore_host();
}

START_TEST(umfd_exit)
{
    HANDLE host;
    DWORD ms, code = 0;

    if (!can_run())
        return;
    host = start_host();
    if (!host)
        return;

    /* The host exits with the view mapped and the request open: refused now, not at the deadline */
    ok(add_command(UMFD_TEST_EXIT, 0, &ms) == 0, "a request whose host exited was accepted\n");
    ok(ms < 10000, "the refusal took %lu ms\n", ms);
    ok(WaitForSingleObject(host, 10000) == WAIT_OBJECT_0, "the host did not exit\n");
    ok(GetExitCodeProcess(host, &code) && code == UMFD_TEST_EXIT_ON_REQUEST, "host exit 0x%lx\n", code);
    ok(add_font(good_font, good_size, &ms) == 0, "a font was accepted with no host\n");
    CloseHandle(host);
    restore_host();
}

START_TEST(umfd_recycle)
{
    HANDLE host;
    DWORD ms, code = 0, i, accepted = 0;

    if (!can_run())
        return;
    host = start_host();
    if (!host)
        return;

    /* Faults below the limit cost one refusal each and keep the host */
    for (i = 1; i < UMFD_HOST_MAX_FAULTS; ++i)
        accepted += add_command(UMFD_TEST_CRASH, 0, &ms);
    ok(accepted == 0, "%lu crashing files were accepted\n", accepted);
    ok(host_alive(host, &code), "the host exited after %u faults: exit 0x%lx\n", UMFD_HOST_MAX_FAULTS - 1, code);
    ok(add_font(good_font, good_size, &ms) > 0, "a good font was refused below the fault limit\n");

    /* The fault at the limit is refused at once and the host exits with the recycle code */
    ok(add_command(UMFD_TEST_CRASH, 0, &ms) == 0, "the crashing file at the limit was accepted\n");
    ok(ms < 10000, "the refusal at the limit took %lu ms\n", ms);
    ok(WaitForSingleObject(host, 10000) == WAIT_OBJECT_0, "the host did not exit at the fault limit\n");
    ok(GetExitCodeProcess(host, &code) && code == UMFD_HOST_EXIT_RECYCLE, "host exit 0x%lx\n", code);
    ok(add_font(good_font, good_size, &ms) == 0, "a font was accepted with no host\n");
    CloseHandle(host);
    restore_host();
}

/* ---- winlogon's launcher (umfdlaunch.c); run these before the tests that register their own host ---- */

#define LAUNCHER_BACKOFF_MAX_MS 30000

static BOOL token_user_is(HANDLE token, WELL_KNOWN_SID_TYPE type)
{
    BYTE buf[256];
    DWORD len;

    return GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len) &&
           IsWellKnownSid(((TOKEN_USER *)buf)->User.Sid, type);
}

/* fontdrvhost.exe running as LocalService is winlogon's; a test's own host runs as SYSTEM */
static DWORD find_host(DWORD not_pid, BOOL launcher_only)
{
    PROCESSENTRY32W pe = { sizeof(pe) };
    HANDLE snap, process, token;
    DWORD found = 0;
    BOOL more, mine;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    for (more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe))
    {
        if (lstrcmpiW(pe.szExeFile, L"fontdrvhost.exe") || pe.th32ProcessID == not_pid)
            continue;
        if (!launcher_only)
        {
            found = pe.th32ProcessID;
            break;
        }
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!process)
            continue;
        mine = OpenProcessToken(process, TOKEN_QUERY, &token);
        if (mine)
        {
            mine = token_user_is(token, WinLocalServiceSid);
            CloseHandle(token);
        }
        CloseHandle(process);
        if (mine)
            found = pe.th32ProcessID;
    }
    CloseHandle(snap);
    return found;
}

static DWORD wait_launcher_host(DWORD not_pid, DWORD timeout)
{
    DWORD start = GetTickCount(), pid;

    while (!(pid = find_host(not_pid, TRUE)) && GetTickCount() - start < timeout)
        Sleep(200);
    return pid;
}

/* UMFD-LAUNCHER-DESIGN §6: LocalService, restricted to E+R+U+AU, Low, SeChangeNotify only, its own desktop */
static void check_launcher_host(DWORD pid)
{
    BYTE buf[1024];
    DWORD len;
    HANDLE process, token = NULL;

    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    ok(process != NULL, "OpenProcess(host %lu) failed: %lu\n", pid, GetLastError());
    if (!process)
        return;
    ok(OpenProcessToken(process, TOKEN_QUERY, &token), "OpenProcessToken failed: %lu\n", GetLastError());
    CloseHandle(process);
    if (!token)
        return;

    ok(token_user_is(token, WinLocalServiceSid), "the host does not run as LocalService\n");
    ok(IsTokenRestricted(token), "the host token is not restricted\n");
    if (GetTokenInformation(token, TokenRestrictedSids, buf, sizeof(buf), &len))
        ok(((TOKEN_GROUPS *)buf)->GroupCount == 4, "%lu restricting SIDs\n", ((TOKEN_GROUPS *)buf)->GroupCount);
    else
        ok(0, "TokenRestrictedSids failed: %lu\n", GetLastError());
    if (GetTokenInformation(token, TokenIntegrityLevel, buf, sizeof(buf), &len))
    {
        PSID label = ((TOKEN_MANDATORY_LABEL *)buf)->Label.Sid;
        ok(*GetSidSubAuthority(label, *GetSidSubAuthorityCount(label) - 1) == SECURITY_MANDATORY_LOW_RID,
           "the host is not at Low integrity\n");
    }
    else
        ok(0, "TokenIntegrityLevel failed: %lu\n", GetLastError());
    if (GetTokenInformation(token, TokenPrivileges, buf, sizeof(buf), &len))
        ok(((TOKEN_PRIVILEGES *)buf)->PrivilegeCount <= 1, "the host holds %lu privileges\n",
           ((TOKEN_PRIVILEGES *)buf)->PrivilegeCount);
    else
        ok(0, "TokenPrivileges failed: %lu\n", GetLastError());
    CloseHandle(token);
}

static BOOL kill_host(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    BOOL ret;

    if (!process)
        return FALSE;
    ret = TerminateProcess(process, 0xDEAD) && WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
    CloseHandle(process);
    return ret;
}

static BOOL can_run_launcher(void)
{
    if (!enable_privilege(L"SeDebugPrivilege"))
    {
        skip("not elevated (no SeDebugPrivilege)\n");
        return FALSE;
    }
    if (!load_good_font())
    {
        skip("no TrueType font found\n");
        return FALSE;
    }
    return TRUE;
}

START_TEST(umfd_launcher)
{
    DWORD mode, pid, next, ms;

    if (!is_reactos())
    {
        skip("the UMFD launcher is Orosi's own\n");
        return;
    }
    mode = umfd_mode();
    if (mode == 0)
    {
        ok(find_host(0, FALSE) == 0, "a font driver host runs in mode 0\n");
        return;
    }
    if (mode != 1 || !can_run_launcher())
        return;

    pid = wait_launcher_host(0, 10000);
    ok(pid != 0, "winlogon started no font driver host\n");
    if (!pid)
        return;
    check_launcher_host(pid);
    ok(add_font(good_font, good_size, &ms) > 0, "a good font was refused by winlogon's host\n");

    /* An abnormal end is relaunched after the first backoff step */
    ok(kill_host(pid), "could not end host %lu\n", pid);
    next = wait_launcher_host(pid, 5000);
    ok(next != 0, "the host was not relaunched within 5 s\n");
    if (!next)
        return;
    check_launcher_host(next);
    ok(add_font(good_font, good_size, &ms) > 0, "a good font was refused after the relaunch\n");

#ifdef UMFD_TEST_HOOKS
    {
        HANDLE host = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, next);
        DWORD i, code = 0;

        ok(host != NULL, "OpenProcess(host %lu) failed: %lu\n", next, GetLastError());
        if (!host)
            return;
        /* A recycle is a normal end: relaunched at once, and it clears the launch history */
        for (i = 0; i < UMFD_HOST_MAX_FAULTS; ++i)
            add_command(UMFD_TEST_CRASH, 0, &ms);
        ok(WaitForSingleObject(host, 10000) == WAIT_OBJECT_0, "the host did not recycle\n");
        ok(GetExitCodeProcess(host, &code) && code == UMFD_HOST_EXIT_RECYCLE, "host exit 0x%lx\n", code);
        CloseHandle(host);
        pid = wait_launcher_host(next, 3000);
        ok(pid != 0, "the recycled host was not relaunched within 3 s\n");
        ok(add_font(good_font, good_size, &ms) > 0, "a good font was refused after the recycle\n");
    }
#endif
}

/* The event-log entry the cap leaves; 0 if the log cannot be read */
static int cap_logged(void)
{
    BYTE buf[0x10000];
    DWORD read, needed, off;
    HANDLE log = OpenEventLogW(NULL, L"Application");
    int found = -1;

    if (!log)
        return 0;
    while (found < 0 && ReadEventLogW(log, EVENTLOG_SEQUENTIAL_READ | EVENTLOG_BACKWARDS_READ, 0,
                                      buf, sizeof(buf), &read, &needed))
    {
        for (off = 0; off < read && found < 0; off += ((EVENTLOGRECORD *)(buf + off))->Length)
        {
            EVENTLOGRECORD *rec = (EVENTLOGRECORD *)(buf + off);
            if ((rec->EventID & 0xFFFF) == 4002 && rec->NumStrings &&
                wcsstr((WCHAR *)((BYTE *)rec + rec->StringOffset), L"not relaunched"))
                found = 1;
        }
    }
    CloseEventLog(log);
    return found < 0 ? -1 : 1;
}

/* Destructive: leaves mode 1 refusing every added font until reboot, so the leg runs it last */
START_TEST(umfd_launcher_cap)
{
    DWORD pid = 0, prev = 0, kills, ms;
    int logged;

    if (!is_reactos() || umfd_mode() != 1 || !can_run_launcher())
    {
        skip("needs Orosi in mode 1, elevated\n");
        return;
    }
    for (kills = 0; kills <= 6; ++kills)
    {
        pid = wait_launcher_host(prev, LAUNCHER_BACKOFF_MAX_MS + 5000);
        if (!pid)
            break;
        ok(kill_host(pid), "could not end host %lu\n", pid);
        prev = pid;
    }
    ok(pid == 0, "the host was still relaunched after %lu abnormal ends\n", kills);
    ok(kills >= 1 && kills <= 5, "%lu abnormal ends before the cap (at most 5 launches in 10 minutes)\n", kills);
    ok(add_font(good_font, good_size, &ms) == 0, "a font was accepted after the cap\n");
    ok(ms < 5000, "the refusal after the cap took %lu ms\n", ms);
    logged = cap_logged();
    if (logged == 0)
        skip("the Application event log cannot be read\n");
    else
        ok(logged > 0, "no event 4002 for the cap\n");
}
