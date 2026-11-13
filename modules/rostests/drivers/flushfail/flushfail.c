/*
 * PROJECT:     LiberNT test drivers
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     flushfail.exe: loads flushfail_drv.sys as a demand-start
 *              kernel service (the .sys next to this executable) and drives
 *              it: attach to a volume by drive letter, print its counters.
 *
 *              flushfail attach E:   attach the filter above E:'s volume device
 *              flushfail stats       print the counters
 */

#include <windows.h>
#include <winioctl.h>
#include <mountmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include "flushfail.h"

#define SERVICE_NAME L"flushfail"

static
int
Fail(const wchar_t *What)
{
    DWORD Error = GetLastError();
    wprintf(L"flushfail: %s failed, error %lu\n", What, Error);
    return 2;
}

static
BOOL
EnsureDriverRunning(void)
{
    SC_HANDLE Scm, Service;
    WCHAR Path[MAX_PATH];
    WCHAR *Slash;
    BOOL Ok;

    Scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!Scm)
        return FALSE;

    Service = OpenServiceW(Scm, SERVICE_NAME, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!Service)
    {
        if (!GetModuleFileNameW(NULL, Path, MAX_PATH))
        {
            CloseServiceHandle(Scm);
            return FALSE;
        }
        Slash = wcsrchr(Path, L'\\');
        if (!Slash)
        {
            CloseServiceHandle(Scm);
            return FALSE;
        }
        wcscpy(Slash + 1, L"flushfail_drv.sys");
        Service = CreateServiceW(Scm, SERVICE_NAME, L"flushfail test volume filter",
                                 SERVICE_START | SERVICE_QUERY_STATUS,
                                 SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                                 SERVICE_ERROR_NORMAL, Path, NULL, NULL, NULL, NULL, NULL);
        if (!Service)
        {
            CloseServiceHandle(Scm);
            return FALSE;
        }
        wprintf(L"flushfail: service created for %s\n", Path);
    }

    Ok = StartServiceW(Service, 0, NULL);
    if (!Ok && GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
        Ok = TRUE;
    CloseServiceHandle(Service);
    CloseServiceHandle(Scm);
    return Ok;
}

static
HANDLE
OpenControl(void)
{
    return CreateFileW(FLUSHFAIL_WIN32_NAME, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static
int
DoAttach(const wchar_t *Drive)
{
    WCHAR Dos[3] = { Drive[0], L':', 0 };
    WCHAR Device[512];
    HANDLE Control;
    DWORD Returned;

    if (!QueryDosDeviceW(Dos, Device, ARRAYSIZE(Device)))
        return Fail(L"QueryDosDevice");
    wprintf(L"flushfail: %s is %s\n", Dos, Device);

    if (!EnsureDriverRunning())
        return Fail(L"loading the driver");
    Control = OpenControl();
    if (Control == INVALID_HANDLE_VALUE)
        return Fail(L"opening the control device");
    if (!DeviceIoControl(Control, IOCTL_FLUSHFAIL_ATTACH, Device,
                         (DWORD)((wcslen(Device) + 1) * sizeof(WCHAR)), NULL, 0, &Returned, NULL))
    {
        CloseHandle(Control);
        return Fail(L"IOCTL_FLUSHFAIL_ATTACH");
    }
    CloseHandle(Control);
    wprintf(L"flushfail: attached above %s; dismount %s so the next mount goes through the filter\n", Device, Dos);
    return 0;
}

static
int
DoWriteFail(BOOL Enable)
{
    HANDLE Control;
    DWORD Returned;
    ULONG Value = Enable ? 1 : 0;

    Control = OpenControl();
    if (Control == INVALID_HANDLE_VALUE)
        return Fail(L"opening the control device");
    if (!DeviceIoControl(Control, IOCTL_FLUSHFAIL_WRITEFAIL, &Value, sizeof(Value), NULL, 0, &Returned, NULL))
    {
        CloseHandle(Control);
        return Fail(L"IOCTL_FLUSHFAIL_WRITEFAIL");
    }
    CloseHandle(Control);
    wprintf(L"flushfail: writefail %s\n", Enable ? L"on" : L"off");
    return 0;
}

static
int
DoStats(void)
{
    FLUSHFAIL_STATS Stats;
    HANDLE Control;
    DWORD Returned;

    Control = OpenControl();
    if (Control == INVALID_HANDLE_VALUE)
        return Fail(L"opening the control device");
    if (!DeviceIoControl(Control, IOCTL_FLUSHFAIL_STATS, NULL, 0, &Stats, sizeof(Stats), &Returned, NULL))
    {
        CloseHandle(Control);
        return Fail(L"IOCTL_FLUSHFAIL_STATS");
    }
    CloseHandle(Control);
    wprintf(L"flushfail stats: attached %lu, flushes rejected %lu, cache queries faked %lu, "
            L"writes %lu, write-through writes %lu, writes rejected %lu\n",
            Stats.Attached, Stats.FlushesRejected, Stats.CacheQueriesFaked,
            Stats.WritesSeen, Stats.WriteThroughSeen, Stats.WritesRejected);
    return 0;
}

/* Ask the mount manager to reconcile every remote database (device.c ReconcileAll),
 * the one user-mode request that rewrites a volume's database while that volume
 * rejects writes; mount point and drive letter changes only touch other volumes. */
static
int
DoMmReconcile(void)
{
    HANDLE MountMgr;
    DWORD Returned;
    BOOL Ok;

    MountMgr = CreateFileW(MOUNTMGR_DOS_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (MountMgr == INVALID_HANDLE_VALUE)
        return Fail(L"open MountPointManager");
    Ok = DeviceIoControl(MountMgr, IOCTL_MOUNTMGR_AUTO_DL_ASSIGNMENTS, NULL, 0, NULL, 0, &Returned, NULL);
    CloseHandle(MountMgr);
    if (!Ok)
        return Fail(L"IOCTL_MOUNTMGR_AUTO_DL_ASSIGNMENTS");
    wprintf(L"flushfail: mount manager reconcile requested\n");
    return 0;
}

int
wmain(int argc, wchar_t *argv[])
{
    if (argc == 3 && _wcsicmp(argv[1], L"attach") == 0 && argv[2][0] && (argv[2][1] == L':' || argv[2][1] == 0))
        return DoAttach(argv[2]);
    if (argc == 2 && _wcsicmp(argv[1], L"stats") == 0)
        return DoStats();
    if (argc == 2 && _wcsicmp(argv[1], L"mmreconcile") == 0)
        return DoMmReconcile();
    if (argc == 3 && _wcsicmp(argv[1], L"writefail") == 0 && (_wcsicmp(argv[2], L"on") == 0 || _wcsicmp(argv[2], L"off") == 0))
        return DoWriteFail(_wcsicmp(argv[2], L"on") == 0);
    wprintf(L"usage: flushfail attach <drive>:  |  flushfail writefail on|off  |  flushfail stats  |  flushfail mmreconcile\n");
    return 1;
}
