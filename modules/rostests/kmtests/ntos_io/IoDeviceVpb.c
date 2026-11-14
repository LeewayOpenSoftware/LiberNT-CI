/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The VPB of a disk device goes away with the device, unless it is still held
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define VPB_TAG                 ' BPV'
#define VPB_ROUNDS              256
#define VPB_HELD                32
#define VPB_TOLERANCE           8

static
BOOLEAN
QueryLiveVpbs(
    _Out_ PLONG Live)
{
    PSYSTEM_POOLTAG_INFORMATION Info;
    ULONG Length = 64 * 1024;
    ULONG Needed;
    NTSTATUS Status;
    ULONG Tries;
    ULONG i;

    *Live = 0;

    /* A query with no buffer returns no length, so start with a real buffer
     * and grow it to the length that a too small one reports */
    for (Tries = 0; ; Tries++)
    {
        Info = ExAllocatePoolWithTag(PagedPool, Length, 'tPmK');
        if (Info == NULL) return FALSE;

        Needed = 0;
        Status = ZwQuerySystemInformation(SystemPoolTagInformation, Info, Length, &Needed);
        if (NT_SUCCESS(Status)) break;

        ExFreePoolWithTag(Info, 'tPmK');
        if (Status != STATUS_INFO_LENGTH_MISMATCH || Needed <= Length || Tries == 4)
        {
            trace("Pool tag query failed: status 0x%08lx, buffer %lu, needed %lu\n", Status, Length, Needed);
            return FALSE;
        }

        Length = Needed + 64 * sizeof(SYSTEM_POOLTAG);
    }

    for (i = 0; i < Info->Count; i++)
    {
        if (Info->TagInfo[i].TagUlong != VPB_TAG) continue;

        *Live = (LONG)(Info->TagInfo[i].NonPagedAllocs - Info->TagInfo[i].NonPagedFrees);
        ExFreePoolWithTag(Info, 'tPmK');
        return TRUE;
    }

    trace("Pool tag query has no entry for the VPB tag (%lu tags)\n", Info->Count);
    ExFreePoolWithTag(Info, 'tPmK');
    return FALSE;
}

/* A disk device that nothing mounted: its VPB goes with it */
static
VOID
TestUnusedVpb(VOID)
{
    PDEVICE_OBJECT Device;
    LONG Before, After;
    NTSTATUS Status;
    ULONG Rounds;

    /* The system disk has a VPB, so the tag is always known: no count means
     * that the test cannot see anything, and that is a failure, not a skip */
    ok(QueryLiveVpbs(&Before), "Pool tag information unavailable\n");

    for (Rounds = 0; Rounds < VPB_ROUNDS; Rounds++)
    {
        Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_DISK, 0, FALSE, &Device);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) break;

        if (Rounds == 0)
        {
            ok(Device->Vpb != NULL, "A disk device has no VPB\n");
            if (Device->Vpb != NULL)
            {
                ok_eq_pointer(Device->Vpb->RealDevice, Device);
                ok_eq_pointer(Device->Vpb->DeviceObject, NULL);
                ok_eq_ulong(Device->Vpb->ReferenceCount, 0UL);
            }
        }

        IoDeleteDevice(Device);
    }

    ok(QueryLiveVpbs(&After), "Pool tag information unavailable after the devices\n");
    ok(After - Before <= VPB_TOLERANCE,
       "%lu disk devices were created and deleted and %ld VPBs stay (before %ld, after %ld)\n",
       Rounds, After - Before, Before, After);
}

/* A VPB that is still referenced is not freed with the device */
static
VOID
TestHeldVpb(VOID)
{
    PVPB Held[VPB_HELD];
    PDEVICE_OBJECT Device;
    LONG Before, After;
    NTSTATUS Status;
    ULONG Count, i;
    KIRQL OldIrql;

    ok(QueryLiveVpbs(&Before), "Pool tag information unavailable before the held VPBs\n");

    for (Count = 0; Count < VPB_HELD; Count++)
    {
        Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_DISK, 0, FALSE, &Device);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) break;

        Held[Count] = Device->Vpb;
        if (Held[Count] == NULL)
        {
            ok(FALSE, "A disk device has no VPB\n");
            IoDeleteDevice(Device);
            break;
        }

        IoAcquireVpbSpinLock(&OldIrql);
        Held[Count]->ReferenceCount++;
        IoReleaseVpbSpinLock(OldIrql);

        IoDeleteDevice(Device);
    }

    ok(QueryLiveVpbs(&After), "Pool tag information unavailable with the held VPBs\n");
    ok(After - Before >= (LONG)Count - VPB_TOLERANCE,
       "%lu referenced VPBs lost their devices and only %ld are left (before %ld, after %ld)\n",
       Count, After - Before, Before, After);

    /* The devices are gone and nothing else knows these VPBs: they are ours to free */
    for (i = 0; i < Count; i++)
    {
        ok_eq_ulong(Held[i]->ReferenceCount, 1UL);
        ExFreePool(Held[i]);
    }

    ok(QueryLiveVpbs(&After), "Pool tag information unavailable at the end\n");
    ok(After - Before <= VPB_TOLERANCE,
       "%lu held VPBs were freed by the test and %ld stay (before %ld, after %ld)\n",
       Count, After - Before, Before, After);
}

/* A volume device of a file system points to the VPB of the disk. It is not
 * its own: it stays when the volume device is deleted. */
static
VOID
TestBorrowedVpb(VOID)
{
    PDEVICE_OBJECT Disk, Volume;
    NTSTATUS Status;
    PVPB Vpb;

    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_DISK, 0, FALSE, &Disk);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_DISK_FILE_SYSTEM, 0, FALSE, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(Disk);
        return;
    }

    Vpb = Disk->Vpb;
    ok(Vpb != NULL, "A disk device has no VPB\n");
    ok_eq_pointer(Volume->Vpb, NULL);

    Volume->Vpb = Vpb;
    IoDeleteDevice(Volume);

    ok_eq_pointer(Disk->Vpb, Vpb);
    if (Vpb != NULL)
    {
        ok_eq_int(Vpb->Type, IO_TYPE_VPB);
        ok_eq_pointer(Vpb->RealDevice, Disk);
        ok_eq_ulong(Vpb->ReferenceCount, 0UL);
    }

    IoDeleteDevice(Disk);
}

START_TEST(IoDeviceVpb)
{
    TestUnusedVpb();
    TestHeldVpb();
    TestBorrowedVpb();
}
