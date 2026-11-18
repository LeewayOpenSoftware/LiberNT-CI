/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Directed-DRIPS device flags and wake-source registry
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>
#include <internal/po_drips.h>

#define TAG_PO_DRIPS 'DPoP'

typedef struct _POP_DRIPS_ENTRY
{
    LIST_ENTRY Link;
    PDEVICE_OBJECT DeviceObject;
    ULONG Flags;
} POP_DRIPS_ENTRY, *PPOP_DRIPS_ENTRY;

static KSPIN_LOCK PopDripsLock;
static LIST_ENTRY PopDripsList;
static PDEVICE_OBJECT PopFixedWakeSource[POP_FIXED_WAKE_SOURCE_COUNT];
static PDEVICE_OBJECT PopSystemWakeDevice;

static
PPOP_DRIPS_ENTRY
PopDripsFindLocked(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PLIST_ENTRY Entry;

    if (PopDripsList.Flink == NULL)
    {
        InitializeListHead(&PopDripsList);
    }

    for (Entry = PopDripsList.Flink; Entry != &PopDripsList; Entry = Entry->Flink)
    {
        PPOP_DRIPS_ENTRY Drips = CONTAINING_RECORD(Entry, POP_DRIPS_ENTRY, Link);
        if (Drips->DeviceObject == DeviceObject)
        {
            return Drips;
        }
    }
    return NULL;
}

static
NTSTATUS
PopDripsUpdateFlags(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG Flags,
    _In_ BOOLEAN Set)
{
    PPOP_DRIPS_ENTRY Drips, Spare = NULL;
    PDEVICE_OBJECT Release = NULL;
    KIRQL OldIrql;

    if (DeviceObject == NULL || Flags == 0 || (Flags & ~POP_DRIPS_DEVICE_VALID_MASK) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Set)
    {
        Spare = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Spare), TAG_PO_DRIPS);
        if (Spare == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(Spare, sizeof(*Spare));
    }

    KeAcquireSpinLock(&PopDripsLock, &OldIrql);
    Drips = PopDripsFindLocked(DeviceObject);
    if (Set)
    {
        if (Drips == NULL)
        {
            Drips = Spare;
            Spare = NULL;
            ObReferenceObject(DeviceObject);
            Drips->DeviceObject = DeviceObject;
            InsertTailList(&PopDripsList, &Drips->Link);
        }
        Drips->Flags |= Flags;
    }
    else if (Drips != NULL)
    {
        Drips->Flags &= ~Flags;
        if (Drips->Flags == 0)
        {
            RemoveEntryList(&Drips->Link);
            Release = Drips->DeviceObject;
            Spare = Drips;
        }
    }
    KeReleaseSpinLock(&PopDripsLock, OldIrql);

    if (Release != NULL)
    {
        ObDereferenceObject(Release);
    }
    if (Spare != NULL)
    {
        ExFreePoolWithTag(Spare, TAG_PO_DRIPS);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PoDirectedDripsSetDeviceFlags(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG Flags)
{
    return PopDripsUpdateFlags(DeviceObject, Flags, TRUE);
}

NTSTATUS
NTAPI
PoDirectedDripsClearDeviceFlags(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG Flags)
{
    return PopDripsUpdateFlags(DeviceObject, Flags, FALSE);
}

NTSTATUS
NTAPI
PoSetFixedWakeSource(
    _In_ ULONG Source,
    _In_opt_ PDEVICE_OBJECT DeviceObject)
{
    PDEVICE_OBJECT Old;
    KIRQL OldIrql;

    if (Source >= POP_FIXED_WAKE_SOURCE_COUNT)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (DeviceObject != NULL)
    {
        ObReferenceObject(DeviceObject);
    }

    KeAcquireSpinLock(&PopDripsLock, &OldIrql);
    Old = PopFixedWakeSource[Source];
    PopFixedWakeSource[Source] = DeviceObject;
    KeReleaseSpinLock(&PopDripsLock, OldIrql);

    if (Old != NULL)
    {
        ObDereferenceObject(Old);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PoSetSystemWakeDevice(
    _In_opt_ PDEVICE_OBJECT DeviceObject)
{
    PDEVICE_OBJECT Old;
    KIRQL OldIrql;

    if (DeviceObject != NULL)
    {
        ObReferenceObject(DeviceObject);
    }

    KeAcquireSpinLock(&PopDripsLock, &OldIrql);
    Old = PopSystemWakeDevice;
    PopSystemWakeDevice = DeviceObject;
    KeReleaseSpinLock(&PopDripsLock, OldIrql);

    if (Old != NULL)
    {
        ObDereferenceObject(Old);
    }
    return STATUS_SUCCESS;
}

ULONG
NTAPI
PopDripsGetDeviceFlags(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PPOP_DRIPS_ENTRY Drips;
    ULONG Flags = 0;
    KIRQL OldIrql;

    KeAcquireSpinLock(&PopDripsLock, &OldIrql);
    Drips = PopDripsFindLocked(DeviceObject);
    if (Drips != NULL)
    {
        Flags = Drips->Flags;
    }
    KeReleaseSpinLock(&PopDripsLock, OldIrql);
    return Flags;
}

PDEVICE_OBJECT
NTAPI
PopDripsGetFixedWakeSource(
    _In_ ULONG Source)
{
    PDEVICE_OBJECT Device;
    KIRQL OldIrql;

    if (Source >= POP_FIXED_WAKE_SOURCE_COUNT)
    {
        return NULL;
    }

    KeAcquireSpinLock(&PopDripsLock, &OldIrql);
    Device = PopFixedWakeSource[Source];
    if (Device != NULL)
    {
        ObReferenceObject(Device);
    }
    KeReleaseSpinLock(&PopDripsLock, OldIrql);
    return Device;
}

PDEVICE_OBJECT
NTAPI
PopDripsReferenceSystemWakeDevice(VOID)
{
    PDEVICE_OBJECT Device;
    KIRQL OldIrql;

    KeAcquireSpinLock(&PopDripsLock, &OldIrql);
    Device = PopSystemWakeDevice;
    if (Device != NULL)
    {
        ObReferenceObject(Device);
    }
    KeReleaseSpinLock(&PopDripsLock, OldIrql);
    return Device;
}

VOID
NTAPI
PopDripsDeviceRemoved(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PDEVICE_OBJECT Drop[POP_FIXED_WAKE_SOURCE_COUNT + 2];
    PPOP_DRIPS_ENTRY Drips;
    ULONG Count = 0, i;
    KIRQL OldIrql;

    if (DeviceObject == NULL)
    {
        return;
    }

    KeAcquireSpinLock(&PopDripsLock, &OldIrql);
    Drips = PopDripsFindLocked(DeviceObject);
    if (Drips != NULL)
    {
        RemoveEntryList(&Drips->Link);
        Drop[Count++] = Drips->DeviceObject;
    }
    for (i = 0; i < POP_FIXED_WAKE_SOURCE_COUNT; i++)
    {
        if (PopFixedWakeSource[i] == DeviceObject)
        {
            PopFixedWakeSource[i] = NULL;
            Drop[Count++] = DeviceObject;
        }
    }
    if (PopSystemWakeDevice == DeviceObject)
    {
        PopSystemWakeDevice = NULL;
        Drop[Count++] = DeviceObject;
    }
    KeReleaseSpinLock(&PopDripsLock, OldIrql);

    if (Drips != NULL)
    {
        ExFreePoolWithTag(Drips, TAG_PO_DRIPS);
    }
    for (i = 0; i < Count; i++)
    {
        ObDereferenceObject(Drop[i]);
    }
}
