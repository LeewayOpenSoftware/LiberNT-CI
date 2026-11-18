/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the FsTrackOffset / FsZeroing IRP extensions and associated IRPs
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

START_TEST(IoIrpExtensionKM)
{
    PIRP Irp, Master, Associated;
    IO_IRP_EXT_TRACK_OFFSET_HEADER Blob;
    PIO_IRP_EXT_TRACK_OFFSET_HEADER RetBlob;
    LONGLONG RetOffset;
    ULONG ZeroingOffset;
    NTSTATUS Status;

    Blob.Validation = IRP_EXT_TRACK_OFFSET_HEADER_VALIDATION_VALUE;
    Blob.Flags = EFS_TRACKED_OFFSET_HEADER_FLAG;
    Blob.TrackedOffsetCallback = NULL;

    Irp = IoAllocateIrp(1, FALSE);
    ok(Irp != NULL, "no IRP\n");
    if (Irp == NULL) return;

    ok_bool_false(IoIrpHasFsTrackOffsetExtensionType(Irp), "unset track offset");
    Status = IoGetFsTrackOffsetState(Irp, &RetBlob, &RetOffset);
    ok_eq_hex(Status, STATUS_NOT_FOUND);

    Status = IoSetFsTrackOffsetState(Irp, &Blob, 0x1234);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_bool_true(IoIrpHasFsTrackOffsetExtensionType(Irp), "set track offset");
    RetBlob = NULL;
    RetOffset = 0;
    Status = IoGetFsTrackOffsetState(Irp, &RetBlob, &RetOffset);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_pointer(RetBlob, &Blob);
    ok_eq_ulonglong(RetOffset, 0x1234);

    Status = IoSetFsTrackOffsetState(Irp, NULL, 0);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Status = IoClearFsTrackOffsetState(Irp);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_bool_false(IoIrpHasFsTrackOffsetExtensionType(Irp), "cleared track offset");

    Status = IoGetFsZeroingOffset(Irp, &ZeroingOffset);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    Status = IoSetFsZeroingOffset(Irp, 0xABCD);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZeroingOffset = 0;
    Status = IoGetFsZeroingOffset(Irp, &ZeroingOffset);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_hex(ZeroingOffset, 0xABCD);

    ok_eq_pointer(IoGetSilo(NULL), NULL);
    ok_eq_pointer(IoGetSiloParameters(NULL), NULL);

    Master = IoAllocateIrp(1, FALSE);
    ok(Master != NULL, "no master IRP\n");
    if (Master != NULL)
    {
        Associated = IoMakeAssociatedIrpEx(Master, NULL, 1);
        ok(Associated != NULL, "no associated IRP\n");
        if (Associated != NULL)
        {
            ok_eq_pointer(Associated->AssociatedIrp.MasterIrp, Master);
            ok(Associated->Flags & IRP_ASSOCIATED_IRP, "not marked associated\n");
            IoFreeIrp(Associated);
        }
        IoFreeIrp(Master);
    }

    IoFreeIrp(Irp);
}
