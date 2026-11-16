/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     ATAPI crash-dump storage interface test, user half
 * COPYRIGHT:   Copyright 2026 Leeway Development Works
 */

#include <kmt_test.h>
#include <winioctl.h>
#include <reactos/drivers/dumpstor.h>

static
HANDLE
OpenBootDisk(VOID)
{
    HANDLE Volume, Disk;
    VOLUME_DISK_EXTENTS Extents;
    DWORD Returned;
    WCHAR DiskPath[64];

    Volume = CreateFileW(L"\\\\.\\C:", 0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, 0, NULL);
    if (Volume == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;

    if (!DeviceIoControl(Volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         NULL, 0, &Extents, sizeof(Extents), &Returned, NULL) ||
        Extents.NumberOfDiskExtents != 1)
    {
        CloseHandle(Volume);
        return INVALID_HANDLE_VALUE;
    }
    CloseHandle(Volume);

    StringCchPrintfW(DiskPath, RTL_NUMBER_OF(DiskPath),
                     L"\\\\.\\PhysicalDrive%lu", Extents.Extents[0].DiskNumber);
    Disk = CreateFileW(DiskPath, 0,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, 0, NULL);
    if (Disk != INVALID_HANDLE_VALUE)
        return Disk;

    StringCchPrintfW(DiskPath, RTL_NUMBER_OF(DiskPath),
                     L"\\\\.\\GLOBALROOT\\Device\\Harddisk%lu\\Partition0",
                     Extents.Extents[0].DiskNumber);
    return CreateFileW(DiskPath, 0,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, 0, NULL);
}

static
VOID
TestUserModeRefusal(VOID)
{
    HANDLE Disk;
    ROS_STORAGE_DUMP_INTERFACE Interface;
    DWORD Returned = 0, Error;
    BOOL Success;

    Disk = OpenBootDisk();
    if (Disk == INVALID_HANDLE_VALUE)
    {
        skip(TRUE, "Cannot open the boot disk from user mode (layout or rights); refusal check skipped\n");
        return;
    }

    ZeroMemory(&Interface, sizeof(Interface));
    Interface.BytesPerSector = 512;

    SetLastError(ERROR_SUCCESS);
    Success = DeviceIoControl(Disk, IOCTL_REACTOS_STORAGE_GET_DUMP_INTERFACE,
                              &Interface, sizeof(Interface),
                              &Interface, sizeof(Interface),
                              &Returned, NULL);
    Error = GetLastError();
    CloseHandle(Disk);

    ok(!Success, "User-mode dump-interface query unexpectedly succeeded\n");
    ok(Error == ERROR_ACCESS_DENIED, "Expected ERROR_ACCESS_DENIED, got %lu\n", Error);
}

START_TEST(AtaDump)
{
    DWORD Error;

    TestUserModeRefusal();

    Error = KmtLoadAndOpenDriver(L"AtaDump", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    KmtUnloadDriver();
}
