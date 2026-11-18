/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Directed-DRIPS device flags and wake-source registry
 */

#pragma once

#define POP_DRIPS_DEVICE_PARTICIPATES      0x00000001
#define POP_DRIPS_DEVICE_WAKE_ARMED        0x00000002
#define POP_DRIPS_DEVICE_NO_TRANSITION     0x00000004
#define POP_DRIPS_DEVICE_VALID_MASK        0x00000007

#define POP_FIXED_WAKE_POWER_BUTTON        0
#define POP_FIXED_WAKE_SLEEP_BUTTON        1
#define POP_FIXED_WAKE_RTC                 2
#define POP_FIXED_WAKE_SOURCE_COUNT        3

NTSTATUS
NTAPI
PopDirectedDripsSetDeviceFlags(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG Flags);

NTSTATUS
NTAPI
PopDirectedDripsClearDeviceFlags(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG Flags);

NTSTATUS
NTAPI
PopSetFixedWakeSource(
    _In_ ULONG Source,
    _In_opt_ PDEVICE_OBJECT DeviceObject);

NTSTATUS
NTAPI
PopSetSystemWakeDevice(
    _In_opt_ PDEVICE_OBJECT DeviceObject);

ULONG
NTAPI
PopDripsGetDeviceFlags(
    _In_ PDEVICE_OBJECT DeviceObject);

PDEVICE_OBJECT
NTAPI
PopDripsGetFixedWakeSource(
    _In_ ULONG Source);

PDEVICE_OBJECT
NTAPI
PopDripsReferenceSystemWakeDevice(VOID);

VOID
NTAPI
PopDripsDeviceRemoved(
    _In_ PDEVICE_OBJECT DeviceObject);
