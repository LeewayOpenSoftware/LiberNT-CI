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
#define VPB_TOLERANCE           8

static
BOOLEAN
QueryLiveVpbs(
    _Out_ PLONG Live)
{
    PSYSTEM_POOLTAG_INFORMATION Info;
    ULONG Length = 0;
    NTSTATUS Status;
    ULONG i;

    *Live = 0;

    Status = ZwQuerySystemInformation(SystemPoolTagInformation, NULL, 0, &Length);
    if (Status != STATUS_INFO_LENGTH_MISMATCH || Length == 0) return FALSE;

    Length += 64 * sizeof(SYSTEM_POOLTAG);
    Info = ExAllocatePoolWithTag(PagedPool, Length, 'tPmK');
    if (Info == NULL) return FALSE;

    Status = ZwQuerySystemInformation(SystemPoolTagInformation, Info, Length, NULL);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Info, 'tPmK');
        return FALSE;
    }

    for (i = 0; i < Info->Count; i++)
    {
        if (Info->TagInfo[i].TagUlong != VPB_TAG) continue;

        *Live = (LONG)(Info->TagInfo[i].NonPagedAllocs - Info->TagInfo[i].NonPagedFrees);
        ExFreePoolWithTag(Info, 'tPmK');
        return TRUE;
    }

    ExFreePoolWithTag(Info, 'tPmK');
    return FALSE;
}

START_TEST(IoDeviceVpb)
{
    PDEVICE_OBJECT Device;
    LONG Before, After;
    NTSTATUS Status;
    ULONG Rounds;
    KIRQL OldIrql;
    PVPB Vpb;

    if (skip(QueryLiveVpbs(&Before), "Pool tag information unavailable\n")) return;

    /* A disk device that nothing mounted: its VPB goes with it */
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

    /* A VPB that is still referenced is not freed with the device */
    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_DISK, 0, FALSE, &Device);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (skip(NT_SUCCESS(Status) && Device->Vpb != NULL, "No disk device with a VPB\n")) return;

    if (!QueryLiveVpbs(&Before)) Before = 0;

    Vpb = Device->Vpb;
    IoAcquireVpbSpinLock(&OldIrql);
    Vpb->ReferenceCount++;
    IoReleaseVpbSpinLock(OldIrql);

    IoDeleteDevice(Device);

    ok(QueryLiveVpbs(&After), "Pool tag information unavailable after the held VPB\n");
    ok(After == Before,
       "A referenced VPB was freed with its device (before %ld, after %ld)\n", Before, After);

    /* The device is gone and nothing else knows this VPB: it is ours to free */
    ExFreePool(Vpb);

    ok(QueryLiveVpbs(&After), "Pool tag information unavailable at the end\n");
    ok(After == Before - 1,
       "The held VPB was not counted once (before %ld, after %ld)\n", Before, After);
}
