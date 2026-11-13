/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test that a dismounted NTFS volume is mounted again by the next open
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * After FSCTL_DISMOUNT_VOLUME succeeds the volume must give up the VPB of the
 * device: the next open mounts a new volume through the current top of the
 * storage stack, even while the handle that dismounted (and locked) the old
 * volume is still open.
 */

#include <apitest.h>
#include <winioctl.h>
#include <strsafe.h>

#define TEST_VOLUME_LABEL L"NTFSTEST"
#define TEST_CONTENT "dismount-remount"

static WCHAR VolumeRoot[] = L"C:\\";
static WCHAR VolumeDevice[] = L"\\\\.\\C:";
static WCHAR TestFile[MAX_PATH];

static BOOL
FindTestVolume(VOID)
{
    WCHAR Label[MAX_PATH + 1];
    WCHAR FileSystem[MAX_PATH + 1];
    WCHAR Letter;

    for (Letter = L'C'; Letter <= L'Z'; Letter++)
    {
        VolumeRoot[0] = Letter;
        if (GetVolumeInformationW(VolumeRoot, Label, ARRAYSIZE(Label), NULL, NULL, NULL,
                                  FileSystem, ARRAYSIZE(FileSystem)) &&
            !wcscmp(Label, TEST_VOLUME_LABEL) &&
            !wcscmp(FileSystem, L"NTFS"))
        {
            VolumeDevice[4] = Letter;
            StringCchPrintfW(TestFile, ARRAYSIZE(TestFile), L"%c:\\DismountRemount.txt", Letter);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL
FileHasTestContent(VOID)
{
    CHAR Buffer[sizeof(TEST_CONTENT)];
    HANDLE Handle;
    DWORD Bytes = 0;
    BOOL Ok;

    Handle = CreateFileW(TestFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Opening the test file failed with %lu\n", GetLastError());
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;

    ZeroMemory(Buffer, sizeof(Buffer));
    Ok = ReadFile(Handle, Buffer, sizeof(TEST_CONTENT) - 1, &Bytes, NULL);
    CloseHandle(Handle);
    ok(Ok, "Reading the test file failed with %lu\n", GetLastError());
    return Ok && Bytes == sizeof(TEST_CONTENT) - 1 && !memcmp(Buffer, TEST_CONTENT, Bytes);
}

static HANDLE
OpenVolume(VOID)
{
    HANDLE Volume;

    Volume = CreateFileW(VolumeDevice, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    ok(Volume != INVALID_HANDLE_VALUE, "Opening the volume failed with %lu\n", GetLastError());
    return Volume;
}

static VOID
TestLockedDismount(VOID)
{
    NTFS_VOLUME_DATA_BUFFER VolumeData;
    WCHAR Label[MAX_PATH + 1];
    HANDLE Volume;
    DWORD Bytes;
    BOOL Ok;

    Volume = OpenVolume();
    if (Volume == INVALID_HANDLE_VALUE)
        return;

    Ok = DeviceIoControl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL);
    ok(Ok, "FSCTL_LOCK_VOLUME failed with %lu\n", GetLastError());
    if (!Ok)
    {
        CloseHandle(Volume);
        return;
    }

    Ok = DeviceIoControl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL);
    ok(Ok, "FSCTL_DISMOUNT_VOLUME failed with %lu\n", GetLastError());
    if (Ok)
    {
        /* The locked, dismounted volume no longer owns the device */
        ok(FileHasTestContent(), "The file is not readable after a locked dismount\n");
        ok(GetVolumeInformationW(VolumeRoot, Label, ARRAYSIZE(Label), NULL, NULL, NULL, NULL, 0),
           "GetVolumeInformationW failed with %lu\n", GetLastError());

        SetLastError(0xdeadbeef);
        Ok = DeviceIoControl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &VolumeData, sizeof(VolumeData),
                             &Bytes, NULL);
        ok(!Ok, "The dismounted volume still answered FSCTL_GET_NTFS_VOLUME_DATA\n");
    }

    ok(DeviceIoControl(Volume, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL),
       "FSCTL_UNLOCK_VOLUME failed with %lu\n", GetLastError());
    CloseHandle(Volume);
}

static VOID
TestPlainDismount(VOID)
{
    HANDLE Volume;
    DWORD Bytes;
    BOOL Ok;

    Volume = OpenVolume();
    if (Volume == INVALID_HANDLE_VALUE)
        return;

    Ok = DeviceIoControl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL);
    ok(Ok, "FSCTL_DISMOUNT_VOLUME failed with %lu\n", GetLastError());

    /* A second dismount through the same handle reaches the old volume */
    SetLastError(0xdeadbeef);
    Ok = DeviceIoControl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL);
    ok(!Ok, "The old volume was dismounted twice\n");

    ok(FileHasTestContent(), "The file is not readable after a dismount with the handle open\n");
    CloseHandle(Volume);

    ok(FileHasTestContent(), "The file is not readable after the dismounting handle closed\n");
}

START_TEST(DismountRemount)
{
    HANDLE Handle;
    DWORD Bytes;
    BOOL Ok;

    if (!FindTestVolume())
    {
        skip("No NTFS volume labelled %ls\n", TEST_VOLUME_LABEL);
        return;
    }

    Handle = CreateFileW(TestFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Creating the test file failed with %lu\n", GetLastError());
    if (Handle == INVALID_HANDLE_VALUE)
        return;
    Ok = WriteFile(Handle, TEST_CONTENT, sizeof(TEST_CONTENT) - 1, &Bytes, NULL);
    ok(Ok, "Writing the test file failed with %lu\n", GetLastError());
    CloseHandle(Handle);

    TestLockedDismount();
    TestPlainDismount();
    TestPlainDismount();

    ok(DeleteFileW(TestFile), "Deleting the test file failed with %lu\n", GetLastError());
}
