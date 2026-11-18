/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the directed-DRIPS device flags and the wake-source registry
 */

#include <kmt_test.h>

#define TEST_DRIPS_PARTICIPATES  0x00000001
#define TEST_DRIPS_WAKE_ARMED    0x00000002
#define TEST_DRIPS_ALL           0x00000007
#define TEST_FIXED_SOURCES       3

NTKERNELAPI NTSTATUS NTAPI PoDirectedDripsSetDeviceFlags(_In_ PDEVICE_OBJECT DeviceObject, _In_ ULONG Flags);
NTKERNELAPI NTSTATUS NTAPI PoDirectedDripsClearDeviceFlags(_In_ PDEVICE_OBJECT DeviceObject, _In_ ULONG Flags);
NTKERNELAPI NTSTATUS NTAPI PoSetFixedWakeSource(_In_ ULONG Source, _In_opt_ PDEVICE_OBJECT DeviceObject);
NTKERNELAPI NTSTATUS NTAPI PoSetSystemWakeDevice(_In_opt_ PDEVICE_OBJECT DeviceObject);

START_TEST(PoDripsRegistry)
{
    PDEVICE_OBJECT Device = NULL;
    NTSTATUS Status;
    ULONG Source;

    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &Device);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = PoDirectedDripsSetDeviceFlags(NULL, TEST_DRIPS_PARTICIPATES);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = PoDirectedDripsSetDeviceFlags(Device, 0);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = PoDirectedDripsSetDeviceFlags(Device, 0x80000000);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = PoDirectedDripsClearDeviceFlags(Device, 0x80000000);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Status = PoDirectedDripsSetDeviceFlags(Device, TEST_DRIPS_PARTICIPATES);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = PoDirectedDripsSetDeviceFlags(Device, TEST_DRIPS_WAKE_ARMED);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = PoDirectedDripsClearDeviceFlags(Device, TEST_DRIPS_ALL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = PoDirectedDripsClearDeviceFlags(Device, TEST_DRIPS_ALL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = PoSetFixedWakeSource(TEST_FIXED_SOURCES, Device);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    for (Source = 0; Source < TEST_FIXED_SOURCES; Source++)
    {
        Status = PoSetFixedWakeSource(Source, Device);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = PoSetFixedWakeSource(Source, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    Status = PoSetSystemWakeDevice(Device);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = PoSetSystemWakeDevice(NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    IoDeleteDevice(Device);
}
