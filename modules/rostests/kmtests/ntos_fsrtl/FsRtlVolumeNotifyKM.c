/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for FsRtlIncrementCcFastMdlReadWait and FsRtlNotifyVolumeEventEx
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

extern ULONG CcFastMdlReadWait;
VOID NTAPI FsRtlIncrementCcFastMdlReadWait(VOID);
NTSTATUS NTAPI FsRtlNotifyVolumeEventEx(PFILE_OBJECT FileObject,
                                        ULONG EventCode,
                                        PTARGET_DEVICE_CUSTOM_NOTIFICATION Event);

START_TEST(FsRtlVolumeNotifyKM)
{
    ULONG Before, After;
    NTSTATUS Status;
    TARGET_DEVICE_CUSTOM_NOTIFICATION Notification;

    Before = CcFastMdlReadWait;
    FsRtlIncrementCcFastMdlReadWait();
    After = CcFastMdlReadWait;
    ok_eq_ulong(After, Before + 1);

    Status = FsRtlNotifyVolumeEventEx(NULL, FSRTL_VOLUME_MOUNT, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    RtlZeroMemory(&Notification, sizeof(Notification));
    Notification.Version = 1;
    Notification.Size = sizeof(TARGET_DEVICE_CUSTOM_NOTIFICATION);
    Notification.FileObject = NULL;
    Notification.NameBufferOffset = -1;
    RtlCopyMemory(&Notification.Event, &GUID_IO_VOLUME_DISMOUNT, sizeof(GUID));

    Status = FsRtlNotifyVolumeEventEx(NULL, FSRTL_VOLUME_DISMOUNT, &Notification);
    ok(Status != STATUS_SUCCESS, "NULL file object reported success\n");
}
