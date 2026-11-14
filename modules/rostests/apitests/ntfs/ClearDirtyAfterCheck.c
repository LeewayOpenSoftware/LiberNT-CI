/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test that only the holder of the volume lock can clear the dirty flag after a check
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * FSCTL_NTFS_CLEAR_DIRTY_AFTER_CHECK is what the boot-time checker sends on the
 * handle that holds the volume lock when its check wrote nothing: the mounted
 * instance then clears the dirty flag itself and stays mounted. Anyone else
 * must be refused: a handle on an unlocked volume, a handle that is not the
 * lock owner, and a dismounted volume. The lock owner's request must clear a
 * flag that was set by FSCTL_MARK_VOLUME_DIRTY (retained, so a plain clean
 * dismount would keep it).
 */

#include <apitest.h>
#include <winioctl.h>
#include <strsafe.h>

/* Same value as in the file system library header (ntfslib_new.h) */
#define FSCTL_NTFS_CLEAR_DIRTY_AFTER_CHECK \
    CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8E0, METHOD_BUFFERED, FILE_WRITE_DATA)

#define TEST_VOLUME_LABEL L"NTFSTEST"

static WCHAR VolumeRoot[] = L"C:\\";
static WCHAR VolumeDevice[] = L"\\\\.\\C:";

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
            return TRUE;
        }
    }
    return FALSE;
}

static HANDLE
OpenVolume(VOID)
{
    return CreateFileW(VolumeDevice, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

static BOOL
VolumeIsDirty(HANDLE Volume, PBOOL Dirty)
{
    ULONG Flags = 0;
    DWORD Bytes;
    BOOL Ok;

    Ok = DeviceIoControl(Volume, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &Flags, sizeof(Flags), &Bytes, NULL);
    ok(Ok, "FSCTL_IS_VOLUME_DIRTY failed with %lu\n", GetLastError());
    *Dirty = (Flags & VOLUME_IS_DIRTY) != 0;
    return Ok;
}

static VOID
ExpectRefused(HANDLE Volume, PCSTR Why)
{
    DWORD Bytes;
    BOOL Ok;

    SetLastError(0xdeadbeef);
    Ok = DeviceIoControl(Volume, FSCTL_NTFS_CLEAR_DIRTY_AFTER_CHECK, NULL, 0, NULL, 0, &Bytes, NULL);
    ok(!Ok, "The clear-after-check was accepted %s\n", Why);
    ok(GetLastError() == ERROR_ACCESS_DENIED, "The clear-after-check %s failed with %lu, expected %u\n",
       Why, GetLastError(), ERROR_ACCESS_DENIED);
}

START_TEST(ClearDirtyAfterCheck)
{
    HANDLE Volume;
    HANDLE Other;
    DWORD Bytes;
    BOOL Dirty;
    BOOL Ok;

    if (!FindTestVolume())
    {
        skip("No NTFS volume labelled %ls\n", TEST_VOLUME_LABEL);
        return;
    }

    Volume = OpenVolume();
    ok(Volume != INVALID_HANDLE_VALUE, "Opening the volume failed with %lu\n", GetLastError());
    if (Volume == INVALID_HANDLE_VALUE)
        return;

    /* Not locked: refused */
    ExpectRefused(Volume, "on an unlocked volume");

    Ok = DeviceIoControl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL);
    ok(Ok, "FSCTL_LOCK_VOLUME failed with %lu\n", GetLastError());
    if (!Ok)
    {
        CloseHandle(Volume);
        return;
    }

    /* Another handle: the lock keeps it out, or the request is refused on it */
    SetLastError(0xdeadbeef);
    Other = OpenVolume();
    if (Other != INVALID_HANDLE_VALUE)
    {
        ExpectRefused(Other, "from a handle that does not hold the lock");
        CloseHandle(Other);
    }
    else
    {
        ok(GetLastError() == ERROR_ACCESS_DENIED, "A second open on the locked volume failed with %lu\n",
           GetLastError());
    }

    /* The lock owner: the request clears a retained flag through the mounted instance */
    Ok = DeviceIoControl(Volume, FSCTL_MARK_VOLUME_DIRTY, NULL, 0, NULL, 0, &Bytes, NULL);
    ok(Ok, "FSCTL_MARK_VOLUME_DIRTY failed with %lu\n", GetLastError());
    if (VolumeIsDirty(Volume, &Dirty))
        ok(Dirty, "The volume is not dirty after FSCTL_MARK_VOLUME_DIRTY\n");

    Ok = DeviceIoControl(Volume, FSCTL_NTFS_CLEAR_DIRTY_AFTER_CHECK, NULL, 0, NULL, 0, &Bytes, NULL);
    ok(Ok, "The clear-after-check from the lock owner failed with %lu\n", GetLastError());
    if (VolumeIsDirty(Volume, &Dirty))
        ok(!Dirty, "The volume is still dirty after the lock owner's clear-after-check\n");

    /* A second request from the owner is harmless */
    Ok = DeviceIoControl(Volume, FSCTL_NTFS_CLEAR_DIRTY_AFTER_CHECK, NULL, 0, NULL, 0, &Bytes, NULL);
    ok(Ok, "A second clear-after-check from the lock owner failed with %lu\n", GetLastError());

    Ok = DeviceIoControl(Volume, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL);
    ok(Ok, "FSCTL_UNLOCK_VOLUME failed with %lu\n", GetLastError());

    /* Unlocked again: refused again, and the flag stays clear */
    ExpectRefused(Volume, "after the unlock");
    if (VolumeIsDirty(Volume, &Dirty))
        ok(!Dirty, "The volume turned dirty after the unlock\n");

    CloseHandle(Volume);
}
