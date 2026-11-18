/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite device idle detection and device busy references
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

static
VOID
WaitMilliseconds(
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -10000LL * Milliseconds;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

START_TEST(PoDeviceBusy)
{
    PDEVICE_OBJECT DeviceObject;
    PULONG Idle, Again;
    NTSTATUS Status;

    ok(PoRegisterDeviceForIdleDetection(NULL, 60, 60, PowerDeviceD3) == NULL,
       "a NULL device was registered\n");

    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    ok(PoRegisterDeviceForIdleDetection(DeviceObject, 60, 60, PowerDeviceD0) == NULL,
       "D0 was accepted as the idle state\n");
    ok(PoRegisterDeviceForIdleDetection(DeviceObject, 60, 60, PowerDeviceMaximum) == NULL,
       "an invalid idle state was accepted\n");

    Idle = PoRegisterDeviceForIdleDetection(DeviceObject, 60, 60, PowerDeviceD3);
    ok(Idle != NULL, "registration failed\n");
    if (!Idle)
    {
        IoDeleteDevice(DeviceObject);
        return;
    }
    ok_eq_ulong(*Idle, 0UL);

    Again = PoRegisterDeviceForIdleDetection(DeviceObject, 120, 120, PowerDeviceD2);
    ok(Again == Idle, "a second registration returned %p, expected %p\n", Again, Idle);

    /* The power manager counts an idle device up once a second */
    WaitMilliseconds(2500);
    ok(*Idle >= 1, "idle count %lu after 2.5 s\n", *Idle);

    PoSetDeviceBusyEx(Idle);
    ok_eq_ulong(*Idle, 0UL);

    PoSetDeviceBusy(Idle);
    ok_eq_ulong(*Idle, 0UL);

    /* A busy reference keeps the device busy; references nest */
    PoStartDeviceBusy(Idle);
    PoStartDeviceBusy(Idle);
    WaitMilliseconds(2500);
    ok_eq_ulong(*Idle, 0UL);

    PoEndDeviceBusy(Idle);
    WaitMilliseconds(2500);
    ok_eq_ulong(*Idle, 0UL);

    PoEndDeviceBusy(Idle);
    ok_eq_ulong(*Idle, 0UL);
    WaitMilliseconds(2500);
    ok(*Idle >= 1, "idle count %lu after the last busy reference ended\n", *Idle);

    /* Zero times cancel the registration */
    ok(PoRegisterDeviceForIdleDetection(DeviceObject, 0, 0, PowerDeviceD3) == NULL,
       "cancelling the registration returned a pointer\n");
    ok(PoRegisterDeviceForIdleDetection(DeviceObject, 0, 0, PowerDeviceD3) == NULL,
       "cancelling twice returned a pointer\n");

    /* A short idle time sends the idle power request; the test device fails it */
    Idle = PoRegisterDeviceForIdleDetection(DeviceObject, 1, 1, PowerDeviceD3);
    ok(Idle != NULL, "registration with a short idle time failed\n");
    if (Idle)
    {
        WaitMilliseconds(3500);
        ok(*Idle >= 1, "idle count %lu with a 1 s idle time\n", *Idle);
        ok(PoRegisterDeviceForIdleDetection(DeviceObject, 0, 0, PowerDeviceD3) == NULL,
           "cancelling the registration returned a pointer\n");
    }

    /* The power request may still hold the device; deletion waits for its last reference */
    IoDeleteDevice(DeviceObject);
}
