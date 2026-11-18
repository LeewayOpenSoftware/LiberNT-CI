/*
 * PROJECT:     LiberNT Kernel-Mode Driver Framework
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     USB host Controller eXtension (UCX) class library -- downstream
 *              devices (M3 step 2). The hub driver creates, addresses, reads and
 *              removes devices through USB_BUS_INTERFACE_HUB_V5 and talks to
 *              their default endpoint with URBs. This file turns those into the
 *              client's device callbacks (enable / address / default endpoint /
 *              disable) and synchronous control transfers on the default
 *              endpoint, synthesizing the setup packet each URB function stands
 *              for. Step 3 adds the configured endpoints: SELECT_CONFIGURATION /
 *              SELECT_INTERFACE through the client's endpoint-add and
 *              endpoints-configure callbacks, bulk and interrupt transfers
 *              forwarded to the client's per-endpoint WDF queues, pipe abort
 *              and reset.
 * NOTE:        Every device handle is matched against the controller's list
 *              before use; every URB is checked for the length the function
 *              implies before a field is read; control data is bounded by
 *              UCX_CONTROL_MAX_TRANSFER.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "ucxp.h"

/* UcxpDeviceHandleUrb writes TransferBufferLength through the control-transfer
 * view for every control URB function: the layouts must agree on its offset */
#define UCX_SAME_LENGTH_OFFSET(_type) \
    C_ASSERT(FIELD_OFFSET(struct _type, TransferBufferLength) == FIELD_OFFSET(struct _URB_CONTROL_TRANSFER, TransferBufferLength))
UCX_SAME_LENGTH_OFFSET(_URB_CONTROL_DESCRIPTOR_REQUEST);
UCX_SAME_LENGTH_OFFSET(_URB_CONTROL_GET_STATUS_REQUEST);
UCX_SAME_LENGTH_OFFSET(_URB_CONTROL_VENDOR_OR_CLASS_REQUEST);
UCX_SAME_LENGTH_OFFSET(_URB_CONTROL_GET_CONFIGURATION_REQUEST);
UCX_SAME_LENGTH_OFFSET(_URB_CONTROL_GET_INTERFACE_REQUEST);

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static
VOID
UcxpDeviceUnconfigure(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle,
    _In_ BOOLEAN TellDevice);

static
PUCX_USBDEVICE_CTX
UcxpDeviceCtx(
    _In_ PUCX_DEVICE_HANDLE Handle)
{
    return (Handle->UsbDevice != NULL) ? UcxGetUsbDeviceCtx((WDFOBJECT)Handle->UsbDevice) : NULL;
}

static
ULONG
UcxpDefaultMaxPacketSize0(
    _In_ USB_DEVICE_SPEED Speed)
{
    switch (Speed)
    {
        case UsbSuperSpeed: return 512;
        case UsbHighSpeed:  return 64;
        default:            return 8;
    }
}

/* bMaxPacketSize0 as the device descriptor encodes it, or 0 when it is not legal for the speed */
static
ULONG
UcxpMaxPacketSize0FromDescriptor(
    _In_ USB_DEVICE_SPEED Speed,
    _In_ UCHAR bMaxPacketSize0)
{
    switch (Speed)
    {
        case UsbSuperSpeed: return (bMaxPacketSize0 == 9) ? 512 : 0;
        case UsbHighSpeed:  return (bMaxPacketSize0 == 64) ? 64 : 0;
        case UsbLowSpeed:   return (bMaxPacketSize0 == 8) ? 8 : 0;
        default:            return (bMaxPacketSize0 == 8 || bMaxPacketSize0 == 16 ||
                                    bMaxPacketSize0 == 32 || bMaxPacketSize0 == 64) ? bMaxPacketSize0 : 0;
    }
}

/*
 * One control transfer on the device's default endpoint through the client's
 * synchronous callback. Buffer/Mdl is the caller's memory; Length is bounded.
 */
static
NTSTATUS
UcxpControlTransfer(
    _In_ PUCX_DEVICE_HANDLE Handle,
    _In_ const USB_DEFAULT_PIPE_SETUP_PACKET *Setup,
    _In_opt_ PVOID Buffer,
    _In_opt_ PMDL Mdl,
    _In_ ULONG Length,
    _In_ BOOLEAN DataIn,
    _Out_ PULONG Transferred,
    _Out_ USBD_STATUS *UsbdStatus)
{
    PUCX_USBDEVICE_CTX Device = UcxpDeviceCtx(Handle);
    PUCX_ENDPOINT_CTX Endpoint;
    UCX_TRANSFER Transfer;
    NTSTATUS Status;

    *Transferred = 0;
    *UsbdStatus = USBD_STATUS_DEVICE_GONE;

    if (Device == NULL || Device->DefaultEndpoint == NULL)
        return STATUS_DEVICE_NOT_CONNECTED;
    Endpoint = UcxGetEndpointCtx((WDFOBJECT)Device->DefaultEndpoint);
    if (Endpoint->Callbacks.EvtEndpointControlTransfer == NULL)
    {
        *UsbdStatus = USBD_STATUS_NOT_SUPPORTED;
        return STATUS_NOT_SUPPORTED;
    }
    if (Length > UCX_CONTROL_MAX_TRANSFER || Length != Setup->wLength ||
        (Length != 0 && UcxUrbTransferBuffer(Buffer, Mdl, Length) == NULL))
    {
        *UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&Transfer, sizeof(Transfer));
    Transfer.Size = sizeof(Transfer);
    Transfer.Type = UcxTransferControl;
    Transfer.SetupPacket = *Setup;
    Transfer.DataIn = DataIn;
    Transfer.Length = Length;
    Transfer.Buffer = Buffer;
    Transfer.Mdl = Mdl;
    Transfer.UsbdStatus = USBD_STATUS_DEV_NOT_RESPONDING;

    Status = Endpoint->Callbacks.EvtEndpointControlTransfer(Device->DefaultEndpoint, &Transfer);

    *Transferred = min(Transfer.Transferred, Length);   /* the client's count, never past the buffer */
    *UsbdStatus = NT_SUCCESS(Status) ? USBD_STATUS_SUCCESS : Transfer.UsbdStatus;
    if (!NT_SUCCESS(Status) && USBD_SUCCESS(*UsbdStatus))
        *UsbdStatus = USBD_STATUS_DEV_NOT_RESPONDING;
    return Status;
}

/* GET_DESCRIPTOR(device, 0) of Length bytes into the handle's own copy */
static
NTSTATUS
UcxpReadDeviceDescriptor(
    _Inout_ PUCX_DEVICE_HANDLE Handle,
    _In_ ULONG Length,
    _Out_ PULONG Transferred)
{
    USB_DEFAULT_PIPE_SETUP_PACKET Setup;
    USBD_STATUS UsbdStatus;

    RtlZeroMemory(&Setup, sizeof(Setup));
    Setup.bmRequestType.Dir = BMREQUEST_DEVICE_TO_HOST;
    Setup.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    Setup.wValue.HiByte = USB_DEVICE_DESCRIPTOR_TYPE;
    Setup.wLength = (USHORT)Length;

    RtlZeroMemory(&Handle->DeviceDescriptor, sizeof(Handle->DeviceDescriptor));
    return UcxpControlTransfer(Handle, &Setup, &Handle->DeviceDescriptor, NULL, Length, TRUE,
                               Transferred, &UsbdStatus);
}

/* --------------------------------------------------------------------------
 * Create / initialize / descriptors / remove (USB_BUS_INTERFACE_HUB_V5)
 * -------------------------------------------------------------------------- */

NTSTATUS
UcxpDeviceCreate(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _In_opt_ PUCX_DEVICE_HANDLE Parent,
    _In_ USHORT PortNumber,
    _In_ USHORT PortStatus,
    _Outptr_ PUCX_DEVICE_HANDLE *NewHandle)
{
    PUCX_CONTROLLER_CTX Controller = UcxGetControllerCtx((WDFOBJECT)RootHub->Controller);
    PUCX_DEVICE_HANDLE Handle;
    struct _UCXUSBDEVICE_INIT *Init;
    UCX_USBDEVICE_INFO Info;
    UCX_USBDEVICE_PATH Path;
    PUCX_USBDEVICE_CTX DeviceCtx;
    NTSTATUS Status;

    *NewHandle = NULL;
    if (Controller->Config.EvtControllerUsbDeviceAdd == NULL)
        return STATUS_NOT_IMPLEMENTED;

    RtlZeroMemory(&Path, sizeof(Path));
    Path.Size = sizeof(Path);
    if (Parent == NULL)
    {
        if (PortNumber == 0 || PortNumber > RootHub->NumberOfPorts)
            return STATUS_INVALID_PARAMETER;
        Path.RootPortNumber = (UCHAR)PortNumber;
    }
    else
    {
        /* Behind a hub: the hub must be a configured hub we know, the port one
         * the xHCI route string can name (a nibble, 1..15), the depth at most
         * 5 tiers; the speed is what the hub's port status says, SuperSpeed on
         * a SuperSpeed hub */
        PUCX_DEVICE_HANDLE Hub;
        USHORT TtPort;

        if (!Parent->Initialized || !Parent->IsHub || !Parent->HubInfoSent)
            return STATUS_INVALID_DEVICE_STATE;
        if (PortNumber == 0 || PortNumber > 15 || (Parent->HubPorts != 0 && PortNumber > Parent->HubPorts))
            return STATUS_INVALID_PARAMETER;
        if (Parent->Path.Depth >= 5)
            return STATUS_NOT_SUPPORTED;
        Path.BehindHub = TRUE;
        Path.ParentHub = Parent->UsbDevice;
        Path.Depth = Parent->Path.Depth + 1;
        Path.RootPortNumber = Parent->Path.RootPortNumber;
        Path.HubPortNumber = (UCHAR)PortNumber;
        Path.RouteString = Parent->Path.RouteString | ((ULONG)PortNumber << (4 * Parent->Path.Depth));
        if (Parent->DeviceSpeed == UsbSuperSpeed)
            Path.Speed = UsbSuperSpeed;
        else if (PortStatus & USB_PORT_STATUS_LOW_SPEED)
            Path.Speed = UsbLowSpeed;
        else if (PortStatus & USB_PORT_STATUS_HIGH_SPEED)
            Path.Speed = UsbHighSpeed;
        else
            Path.Speed = UsbFullSpeed;
        /* A low/full-speed device is served by the transaction translator of
         * the nearest high-speed hub above it: that hub and the port of the
         * chain element below it */
        if (Path.Speed == UsbLowSpeed || Path.Speed == UsbFullSpeed)
        {
            Hub = Parent;
            TtPort = PortNumber;
            while (Hub != NULL && Hub->DeviceSpeed != UsbHighSpeed)
            {
                TtPort = Hub->Path.HubPortNumber;
                Hub = Hub->Parent;
            }
            if (Hub != NULL)
            {
                Path.TtHub = Hub->UsbDevice;
                Path.TtPortNumber = (UCHAR)TtPort;
            }
        }
    }

    /* One device per root port: a second create for the same port would mean a
     * second slot and address for the same device */
    {
        KIRQL OldIrql;
        PLIST_ENTRY Entry;
        BOOLEAN Taken = FALSE;

        KeAcquireSpinLock(&Controller->DeviceHandleLock, &OldIrql);
        for (Entry = Controller->DeviceHandles.Flink; Entry != &Controller->DeviceHandles; Entry = Entry->Flink)
        {
            PUCX_DEVICE_HANDLE Other = CONTAINING_RECORD(Entry, UCX_DEVICE_HANDLE, Link);

            if (!Other->IsRootHub && Other->Parent == Parent && Other->PortNumber == PortNumber)
            {
                Taken = TRUE;
                break;
            }
        }
        KeReleaseSpinLock(&Controller->DeviceHandleLock, OldIrql);
        if (Taken)
        {
            UcxLog("device: port %u already has a device handle", PortNumber);
            return STATUS_DEVICE_ALREADY_ATTACHED;
        }
    }

    Handle = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Handle), UCX_TAG);
    if (Handle == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeSpinLock(&Handle->PipeLock);
    Handle->RootHub = RootHub;
    Init = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Init), UCX_TAG);
    if (Init == NULL)
    {
        ExFreePoolWithTag(Handle, UCX_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Handle->IsRootHub = FALSE;
    Handle->PortNumber = PortNumber;
    Handle->Parent = Parent;
    Handle->Path = Path;
    Handle->DeviceSpeed = Path.BehindHub ? Path.Speed : UsbFullSpeed;   /* on a root port the client reports it at Enable */
    Handle->MaxPacketSize0 = 8;
    Handle->DefaultPipe.Signature = UCX_PIPE_HANDLE_SIGNATURE;
    Handle->DefaultPipe.Descriptor.bLength = sizeof(USB_ENDPOINT_DESCRIPTOR);
    Handle->DefaultPipe.Descriptor.bDescriptorType = USB_ENDPOINT_DESCRIPTOR_TYPE;
    Handle->DefaultPipe.Descriptor.bEndpointAddress = 0;
    Handle->DefaultPipe.Descriptor.bmAttributes = USB_ENDPOINT_TYPE_CONTROL;
    Handle->DefaultPipe.Descriptor.wMaxPacketSize = 8;
    Handle->DefaultPipe.MaximumTransferSize = UCX_CONTROL_MAX_TRANSFER;

    Init->Signature = UCX_USBDEVICE_INIT_SIGNATURE;
    Init->Controller = RootHub->Controller;
    Init->Handle = Handle;
    Init->Created = NULL;

    RtlZeroMemory(&Info, sizeof(Info));
    Info.Size = sizeof(Info);
    Info.PortNumber = PortNumber;
    Info.PortStatus = PortStatus;
    Info.Path = Path;

    /* The client sets its callbacks on Init and creates the object with it;
     * UcxUsbDeviceCreate consumes and frees Init and records Created here. */
    Status = Controller->Config.EvtControllerUsbDeviceAdd(RootHub->Controller, &Info, Init);
    if (NT_SUCCESS(Status) && Init->Created == NULL)
        Status = STATUS_UNSUCCESSFUL;
    if (!NT_SUCCESS(Status))
    {
        UcxLog("device: port %u add refused by the client 0x%08lX", PortNumber, (ULONG)Status);
        if (Init->Created != NULL)
            WdfObjectDelete((WDFOBJECT)Init->Created);
        ExFreePoolWithTag(Init, UCX_TAG);
        ExFreePoolWithTag(Handle, UCX_TAG);
        return Status;
    }

    Handle->UsbDevice = Init->Created;
    DeviceCtx = UcxGetUsbDeviceCtx((WDFOBJECT)Handle->UsbDevice);
    DeviceCtx->Handle = Handle;
    ExFreePoolWithTag(Init, UCX_TAG);
    if (Parent != NULL)
    {
        /* The hub stays until its children are gone: a child is a hold on it */
        InterlockedIncrement(&Parent->Children);
        InterlockedIncrement(&Parent->Holds);
    }
    UcxpAddDeviceHandle(Controller, Handle);
    *NewHandle = Handle;
    if (Parent != NULL)
        UcxLog("device: hub port %u (depth %u, route 0x%05lX, root port %u, speed %u) -> handle %p, usbdevice %p",
               PortNumber, Path.Depth, Path.RouteString, Path.RootPortNumber, Path.Speed, Handle, Handle->UsbDevice);
    else
        UcxLog("device: port %u -> handle %p, usbdevice %p", PortNumber, Handle, Handle->UsbDevice);
    return STATUS_SUCCESS;
}

/*
 * InitializeUsbDevice: slot, address, default endpoint, device descriptor.
 * Mirrors what the hub driver expects from usbport: afterwards the handle has
 * an address and a complete device descriptor, or the call failed and the hub
 * driver removes the device.
 */
NTSTATUS
UcxpDeviceInitialize(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle)
{
    PUCX_USBDEVICE_CTX Device = UcxpDeviceCtx(Handle);
    UCX_USBDEVICE_ENABLE_INFO Enable;
    struct _UCXENDPOINT_INIT *EndpointInit;
    USHORT Address = 0;
    ULONG Transferred = 0;
    ULONG DescriptorMps;
    NTSTATUS Status;

    if (Device == NULL || Handle->Initialized)
        return STATUS_INVALID_DEVICE_STATE;
    if (Handle->InitializeFailed)
        return STATUS_DEVICE_NOT_READY;    /* the hub driver removes the handle and starts over */
    if (Device->Callbacks.EvtUsbDeviceEnable == NULL || Device->Callbacks.EvtUsbDeviceAddress == NULL ||
        Device->Callbacks.EvtUsbDeviceDefaultEndpointAdd == NULL)
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    /* 1. A slot for the port the hub reset */
    RtlZeroMemory(&Enable, sizeof(Enable));
    Enable.Size = sizeof(Enable);
    Enable.PortNumber = Handle->PortNumber;
    Enable.Path = Handle->Path;
    Enable.Speed = Handle->Path.BehindHub ? Handle->Path.Speed : UsbFullSpeed;
    Status = Device->Callbacks.EvtUsbDeviceEnable(RootHub->Controller, Handle->UsbDevice, &Enable);
    if (!NT_SUCCESS(Status))
    {
        UcxLog("device: port %u enable failed 0x%08lX", Handle->PortNumber, (ULONG)Status);
        return Status;
    }
    Handle->DeviceSpeed = Enable.Speed;
    Handle->MaxPacketSize0 = Enable.MaxPacketSize0 != 0 ? Enable.MaxPacketSize0
                                                        : UcxpDefaultMaxPacketSize0(Enable.Speed);
    Handle->DefaultPipe.Descriptor.wMaxPacketSize = (USHORT)Handle->MaxPacketSize0;

    /* 2. The address (xHCI assigns it; the client reports it) */
    Status = Device->Callbacks.EvtUsbDeviceAddress(RootHub->Controller, Handle->UsbDevice, &Address);
    if (!NT_SUCCESS(Status))
    {
        UcxLog("device: port %u address failed 0x%08lX", Handle->PortNumber, (ULONG)Status);
        goto Fail;
    }
    Handle->DeviceAddress = Address;

    /* 3. The default endpoint object the transfers go through */
    EndpointInit = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*EndpointInit), UCX_TAG);
    if (EndpointInit == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }
    EndpointInit->Signature = UCX_ENDPOINT_INIT_SIGNATURE;
    EndpointInit->UsbDevice = Handle->UsbDevice;
    EndpointInit->Descriptor = Handle->DefaultPipe.Descriptor;
    Status = Device->Callbacks.EvtUsbDeviceDefaultEndpointAdd(RootHub->Controller, Handle->UsbDevice,
                                                              Handle->MaxPacketSize0, EndpointInit);
    if (NT_SUCCESS(Status) && EndpointInit->Created == NULL)
        Status = STATUS_UNSUCCESSFUL;
    if (!NT_SUCCESS(Status))
    {
        if (EndpointInit->Created != NULL)
            WdfObjectDelete((WDFOBJECT)EndpointInit->Created);
        ExFreePoolWithTag(EndpointInit, UCX_TAG);
        UcxLog("device: port %u default endpoint failed 0x%08lX", Handle->PortNumber, (ULONG)Status);
        goto Fail;
    }
    Device->DefaultEndpoint = EndpointInit->Created;
    Handle->DefaultPipe.Endpoint = EndpointInit->Created;
    ExFreePoolWithTag(EndpointInit, UCX_TAG);

    /* 4. The first 8 bytes of the device descriptor say how big EP0 really is */
    Status = UcxpReadDeviceDescriptor(Handle, 8, &Transferred);
    if (!NT_SUCCESS(Status) || Transferred < 8 ||
        Handle->DeviceDescriptor.bLength != sizeof(USB_DEVICE_DESCRIPTOR) ||
        Handle->DeviceDescriptor.bDescriptorType != USB_DEVICE_DESCRIPTOR_TYPE)
    {
        UcxLog("device: port %u first descriptor read failed 0x%08lX (%lu bytes, bLength %u type %u)",
               Handle->PortNumber, (ULONG)Status, Transferred,
               Handle->DeviceDescriptor.bLength, Handle->DeviceDescriptor.bDescriptorType);
        if (NT_SUCCESS(Status))
            Status = STATUS_DEVICE_DATA_ERROR;
        goto Fail;
    }
    DescriptorMps = UcxpMaxPacketSize0FromDescriptor(Handle->DeviceSpeed, Handle->DeviceDescriptor.bMaxPacketSize0);
    if (DescriptorMps == 0)
    {
        UcxLog("device: port %u bMaxPacketSize0 %u illegal for speed %u",
               Handle->PortNumber, Handle->DeviceDescriptor.bMaxPacketSize0, Handle->DeviceSpeed);
        Status = STATUS_DEVICE_DATA_ERROR;
        goto Fail;
    }
    if (DescriptorMps != Handle->MaxPacketSize0)
    {
        if (Device->Callbacks.EvtUsbDeviceUpdateDefaultEndpoint != NULL)
        {
            Status = Device->Callbacks.EvtUsbDeviceUpdateDefaultEndpoint(RootHub->Controller, Handle->UsbDevice,
                                                                         DescriptorMps);
            if (!NT_SUCCESS(Status))
            {
                UcxLog("device: port %u EP0 size update to %lu failed 0x%08lX",
                       Handle->PortNumber, DescriptorMps, (ULONG)Status);
                goto Fail;
            }
        }
        Handle->MaxPacketSize0 = DescriptorMps;
        Handle->DefaultPipe.Descriptor.wMaxPacketSize = (USHORT)DescriptorMps;
    }

    /* 5. The whole device descriptor */
    Status = UcxpReadDeviceDescriptor(Handle, sizeof(USB_DEVICE_DESCRIPTOR), &Transferred);
    if (!NT_SUCCESS(Status) || Transferred != sizeof(USB_DEVICE_DESCRIPTOR) ||
        Handle->DeviceDescriptor.bLength != sizeof(USB_DEVICE_DESCRIPTOR) ||
        Handle->DeviceDescriptor.bDescriptorType != USB_DEVICE_DESCRIPTOR_TYPE)
    {
        UcxLog("device: port %u device descriptor read failed 0x%08lX (%lu bytes)",
               Handle->PortNumber, (ULONG)Status, Transferred);
        if (NT_SUCCESS(Status))
            Status = STATUS_DEVICE_DATA_ERROR;
        goto Fail;
    }

    Handle->IsHub = (Handle->DeviceDescriptor.bDeviceClass == USB_DEVICE_CLASS_HUB);
    Handle->Initialized = TRUE;
    UcxLog("device: port %u addressed %u, speed %u, EP0 %lu, %04X:%04X class %02X/%02X/%02X, %u configuration(s)",
           Handle->PortNumber, Handle->DeviceAddress, Handle->DeviceSpeed, Handle->MaxPacketSize0,
           Handle->DeviceDescriptor.idVendor, Handle->DeviceDescriptor.idProduct,
           Handle->DeviceDescriptor.bDeviceClass, Handle->DeviceDescriptor.bDeviceSubClass,
           Handle->DeviceDescriptor.bDeviceProtocol, Handle->DeviceDescriptor.bNumConfigurations);
    return STATUS_SUCCESS;

Fail:
    /* Leave the hub driver a handle it can remove; the slot goes back now */
    Handle->InitializeFailed = TRUE;
    if (Device->Callbacks.EvtUsbDeviceDisable != NULL)
        Device->Callbacks.EvtUsbDeviceDisable(RootHub->Controller, Handle->UsbDevice);
    return Status;
}

/*
 * GetUsbDescriptors: the device descriptor from the handle, the configuration
 * descriptor read now (header first for wTotalLength, then as much as fits).
 * Each length is in/out: the caller's buffer size on entry, the bytes written
 * on return.
 */
NTSTATUS
UcxpDeviceGetDescriptors(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _In_ PUCX_DEVICE_HANDLE Handle,
    _Out_writes_bytes_to_opt_(*DeviceLength, *DeviceLength) PUCHAR DeviceBuffer,
    _Inout_ PULONG DeviceLength,
    _Out_writes_bytes_to_opt_(*ConfigLength, *ConfigLength) PUCHAR ConfigBuffer,
    _Inout_ PULONG ConfigLength)
{
    USB_DEFAULT_PIPE_SETUP_PACKET Setup;
    USB_CONFIGURATION_DESCRIPTOR Header;
    USBD_STATUS UsbdStatus;
    ULONG Transferred = 0;
    ULONG Length;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RootHub);

    if (!Handle->Initialized)
        return STATUS_INVALID_DEVICE_STATE;

    Length = (DeviceBuffer != NULL) ? min(*DeviceLength, (ULONG)sizeof(USB_DEVICE_DESCRIPTOR)) : 0;
    if (Length != 0)
        RtlCopyMemory(DeviceBuffer, &Handle->DeviceDescriptor, Length);
    *DeviceLength = Length;

    if (*ConfigLength == 0 || ConfigBuffer == NULL)
    {
        *ConfigLength = 0;
        return STATUS_SUCCESS;
    }

    /* Header first: the device says how long the whole descriptor is */
    RtlZeroMemory(&Setup, sizeof(Setup));
    Setup.bmRequestType.Dir = BMREQUEST_DEVICE_TO_HOST;
    Setup.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    Setup.wValue.HiByte = USB_CONFIGURATION_DESCRIPTOR_TYPE;
    Setup.wValue.LowByte = 0;
    Setup.wLength = sizeof(Header);
    RtlZeroMemory(&Header, sizeof(Header));
    Status = UcxpControlTransfer(Handle, &Setup, &Header, NULL, sizeof(Header), TRUE, &Transferred, &UsbdStatus);
    if (!NT_SUCCESS(Status) || Transferred < sizeof(Header) ||
        Header.bLength != sizeof(USB_CONFIGURATION_DESCRIPTOR) ||
        Header.bDescriptorType != USB_CONFIGURATION_DESCRIPTOR_TYPE ||
        Header.wTotalLength < sizeof(USB_CONFIGURATION_DESCRIPTOR))
    {
        UcxLog("device: port %u configuration header read failed 0x%08lX (%lu bytes, bLength %u type %u total %u)",
               Handle->PortNumber, (ULONG)Status, Transferred, Header.bLength, Header.bDescriptorType,
               Header.wTotalLength);
        *ConfigLength = 0;
        return NT_SUCCESS(Status) ? STATUS_DEVICE_DATA_ERROR : Status;
    }

    /* Then as much as the caller can take. A descriptor longer than one control
     * transfer's worth is refused rather than cut short with success. */
    Length = min(*ConfigLength, (ULONG)Header.wTotalLength);
    if (Length > UCX_CONTROL_MAX_TRANSFER)
    {
        UcxLog("device: port %u configuration descriptor of %u bytes exceeds the %u-byte transfer limit",
               Handle->PortNumber, Header.wTotalLength, UCX_CONTROL_MAX_TRANSFER);
        *ConfigLength = 0;
        return STATUS_NOT_SUPPORTED;
    }
    Setup.wLength = (USHORT)Length;
    RtlZeroMemory(ConfigBuffer, Length);
    Status = UcxpControlTransfer(Handle, &Setup, ConfigBuffer, NULL, Length, TRUE, &Transferred, &UsbdStatus);
    if (!NT_SUCCESS(Status))
    {
        UcxLog("device: port %u configuration read (%lu bytes) failed 0x%08lX", Handle->PortNumber, Length, (ULONG)Status);
        *ConfigLength = 0;
        return Status;
    }
    /* The body must still be the descriptor the header announced */
    if (Transferred >= sizeof(USB_CONFIGURATION_DESCRIPTOR) &&
        (((PUSB_CONFIGURATION_DESCRIPTOR)ConfigBuffer)->wTotalLength != Header.wTotalLength ||
         ((PUSB_CONFIGURATION_DESCRIPTOR)ConfigBuffer)->bDescriptorType != USB_CONFIGURATION_DESCRIPTOR_TYPE))
    {
        UcxLog("device: port %u configuration descriptor changed between the two reads", Handle->PortNumber);
        *ConfigLength = 0;
        return STATUS_DEVICE_DATA_ERROR;
    }
    *ConfigLength = Transferred;
    return STATUS_SUCCESS;
}

/*
 * RemoveUsbDevice: unlist the handle so no new caller finds it, wait briefly
 * for callers inside it, then set the removed bit. If that finds no hold, this
 * thread destroys; otherwise the hold whose drop leaves the word at "removed,
 * no holds" does (UcxpDeviceDropHold). The bit is set exactly once; nothing
 * reads the handle after the atomic operation that did not win.
 */
VOID
UcxpDeviceRemove(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle)
{
    PUCX_CONTROLLER_CTX Controller = UcxGetControllerCtx((WDFOBJECT)RootHub->Controller);
    LARGE_INTEGER Slice;
    ULONG Waited = 0;
    USHORT PortNumber = Handle->PortNumber;
    LONG Holds, Children, Old;

    if (InterlockedCompareExchange(&Handle->Holds, 0, 0) & UCX_HANDLE_REMOVING)
        return;                                        /* a second Remove for the same handle */

    /* Out of the list first: no URB finds it from here on (the caller's own
     * hold was released before this call) */
    UcxpRemoveDeviceHandle(Controller, Handle);

    /* Callers inside are short (a hub-interface call or one URB): give them a
     * moment; children are not waited for */
    Slice.QuadPart = -10 * 1000 * 10;                  /* 10 ms */
    for (;;)
    {
        Holds = InterlockedCompareExchange(&Handle->Holds, 0, 0) & UCX_HANDLE_HOLD_MASK;
        Children = InterlockedCompareExchange(&Handle->Children, 0, 0);
        if (Holds <= Children || Waited >= 100)
            break;
        KeDelayExecutionThread(KernelMode, FALSE, &Slice);
        Waited++;
    }

    /* The decision: set the bit; if no hold was there, it is ours to destroy */
    Old = InterlockedOr(&Handle->Holds, UCX_HANDLE_REMOVING);
    if (Old & UCX_HANDLE_REMOVING)
        return;                                        /* raced another Remove: it owns the rest */
    if ((Old & UCX_HANDLE_HOLD_MASK) == 0)
    {
        UcxpDeviceDestroy(RootHub, Handle);
        return;
    }
    /* Holds remain: whoever drops the last one destroys. Handle is not read again here. */
    if (Children != 0)
        UcxLog("device: port %u remove: %ld child device(s) still attached, the last one's removal destroys the hub",
               PortNumber, Children);
    else
        UcxLog("device: port %u remove: %ld caller(s) still inside after %lu ms, the last one out destroys",
               PortNumber, Old & UCX_HANDLE_HOLD_MASK, Waited * 10);
}

/* The teardown proper: pipes, endpoints, slot, objects, memory. Runs once per handle, at PASSIVE_LEVEL. */
VOID
UcxpDeviceDestroy(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle)
{
    PUCX_USBDEVICE_CTX Device = UcxpDeviceCtx(Handle);
    UCXUSBDEVICE UsbDevice = Handle->UsbDevice;

    /* Its pipes give their transfers back and the endpoints leave the controller */
    UcxpDeviceUnconfigure(RootHub, Handle, FALSE);

    if (Device != NULL && Device->Callbacks.EvtUsbDeviceDisable != NULL)
        Device->Callbacks.EvtUsbDeviceDisable(RootHub->Controller, UsbDevice);

    UcxLog("device: port %u handle %p removed", Handle->PortNumber, Handle);
    Handle->UsbDevice = NULL;
    if (UsbDevice != NULL)
        WdfObjectDelete((WDFOBJECT)UsbDevice);      /* its endpoints go with it */
    if (Handle->Parent != NULL)
    {
        PUCX_DEVICE_HANDLE Parent = Handle->Parent;

        Handle->Parent = NULL;
        InterlockedDecrement(&Parent->Children);
        UcxpDeviceDropHold(Parent);                 /* takes the hub down if it was removed first and this was the last hold */
    }
    ExFreePoolWithTag(Handle, UCX_TAG);
}

/* --------------------------------------------------------------------------
 * Hubs (M3 step 4b): the controller's slot must know a hub is a hub
 * -------------------------------------------------------------------------- */

/* The hub descriptor (class request); ports and TT think time */
static
NTSTATUS
UcxpDeviceReadHubDescriptor(
    _Inout_ PUCX_DEVICE_HANDLE Handle)
{
    USB_DEFAULT_PIPE_SETUP_PACKET Setup;
    USBD_STATUS UsbdStatus;
    UCHAR Buffer[sizeof(USB_30_HUB_DESCRIPTOR)];
    ULONG Transferred = 0;
    UCHAR Type = (Handle->DeviceSpeed == UsbSuperSpeed) ? USB_30_HUB_DESCRIPTOR_TYPE : USB_20_HUB_DESCRIPTOR_TYPE;
    NTSTATUS Status;

    RtlZeroMemory(&Setup, sizeof(Setup));
    Setup.bmRequestType.Dir = BMREQUEST_DEVICE_TO_HOST;
    Setup.bmRequestType.Type = BMREQUEST_CLASS;
    Setup.bmRequestType.Recipient = BMREQUEST_TO_DEVICE;
    Setup.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    Setup.wValue.HiByte = Type;
    Setup.wLength = sizeof(Buffer);
    RtlZeroMemory(Buffer, sizeof(Buffer));
    Status = UcxpControlTransfer(Handle, &Setup, Buffer, NULL, sizeof(Buffer), TRUE, &Transferred, &UsbdStatus);
    if (!NT_SUCCESS(Status))
        return Status;
    /* Both layouts start with bLength, bDescriptorType, bNumberOfPorts, wHubCharacteristics */
    if (Transferred < 5 || Buffer[0] < 5 || Buffer[1] != Type || Buffer[2] == 0)
    {
        UcxLog("device: port %u hub descriptor unusable (%lu bytes, length %u type %02X ports %u)",
               Handle->PortNumber, Transferred, Buffer[0], Buffer[1], Buffer[2]);
        return STATUS_DEVICE_DATA_ERROR;
    }
    Handle->HubPorts = Buffer[2];
    Handle->TtThinkTime = (Type == USB_20_HUB_DESCRIPTOR_TYPE) ? (UCHAR)((Buffer[3] >> 5) & 0x3) : 0;
    return STATUS_SUCCESS;
}

static
NTSTATUS
UcxpDeviceSendHubInfo(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle)
{
    PUCX_USBDEVICE_CTX Device = UcxpDeviceCtx(Handle);
    UCX_USBDEVICE_HUB_INFO Info;
    NTSTATUS Status;

    if (Device == NULL || Device->Callbacks.EvtUsbDeviceHubInfo == NULL)
        return STATUS_NOT_SUPPORTED;
    RtlZeroMemory(&Info, sizeof(Info));
    Info.Size = sizeof(Info);
    Info.NumberOfPorts = Handle->HubPorts;
    Info.TtThinkTime = Handle->TtThinkTime;
    Info.MultiTt = Handle->MultiTt;
    Status = Device->Callbacks.EvtUsbDeviceHubInfo(RootHub->Controller, Handle->UsbDevice, &Info);
    UcxLog("device: port %u is a hub: %u port(s), TT think time %u, %s -> 0x%08lX", Handle->PortNumber,
           Info.NumberOfPorts, Info.TtThinkTime, Info.MultiTt ? "multi-TT" : "single TT", (ULONG)Status);
    if (NT_SUCCESS(Status))
        Handle->HubInfoSent = TRUE;
    return Status;
}

/* Initialize20Hub from the hub driver: the transaction-translator count */
NTSTATUS
UcxpDeviceSetHubTt(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle,
    _In_ ULONG TtCount)
{
    if (!Handle->Initialized || !Handle->IsHub)
        return STATUS_INVALID_PARAMETER;
    Handle->MultiTt = (TtCount > 1);
    /* Before the hub is configured the information goes with the configuration */
    if (!Handle->HubInfoSent)
        return STATUS_SUCCESS;
    return UcxpDeviceSendHubInfo(RootHub, Handle);
}

/* USB_DEVICE_INFORMATION_0 for a downstream device, with its open pipes */
NTSTATUS
UcxpDeviceQueryInformation(
    _In_ PUCX_DEVICE_HANDLE Handle,
    _Out_writes_bytes_to_(BufferLength, *Returned) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG Returned)
{
    PUSB_DEVICE_INFORMATION_0 Info = Buffer;
    ULONG Pipes = 0, i, p, n = 0;
    ULONG ActualLength;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Handle->PipeLock, &OldIrql);
    for (i = 0; i < Handle->InterfaceCount; i++)
        if (Handle->Interfaces[i] != NULL)
            Pipes += Handle->Interfaces[i]->PipeCount;
    ActualLength = FIELD_OFFSET(USB_DEVICE_INFORMATION_0, PipeList) + Pipes * sizeof(USB_PIPE_INFORMATION_0);
    KeReleaseSpinLock(&Handle->PipeLock, OldIrql);

    *Returned = 0;
    if (BufferLength < sizeof(USB_LEVEL_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;
    if (Info->InformationLevel > 0)
        return STATUS_NOT_SUPPORTED;
    if (BufferLength < ActualLength)
    {
        Info->ActualLength = ActualLength;
        *Returned = sizeof(USB_LEVEL_INFORMATION);
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(Info, ActualLength);
    Info->InformationLevel = 0;
    Info->ActualLength = ActualLength;
    Info->PortNumber = Handle->PortNumber;
    Info->DeviceDescriptor = Handle->DeviceDescriptor;
    Info->CurrentConfigurationValue = Handle->ConfigurationValue;
    Info->DeviceAddress = Handle->DeviceAddress;
    Info->HubAddress = (Handle->Parent != NULL) ? Handle->Parent->DeviceAddress : 0;
    Info->DeviceSpeed = Handle->DeviceSpeed;
    Info->DeviceType = (Handle->DeviceSpeed == UsbLowSpeed || Handle->DeviceSpeed == UsbFullSpeed) ?
                       Usb11Device : Usb20Device;
    KeAcquireSpinLock(&Handle->PipeLock, &OldIrql);
    for (i = 0; i < Handle->InterfaceCount; i++)
    {
        PUCX_INTERFACE Iface = Handle->Interfaces[i];

        if (Iface == NULL)
            continue;
        for (p = 0; p < Iface->PipeCount && n < Pipes; p++, n++)
        {
            Info->PipeList[n].EndpointDescriptor = Iface->Pipes[p].Descriptor;
            Info->PipeList[n].ScheduleOffset = 0;
        }
    }
    KeReleaseSpinLock(&Handle->PipeLock, OldIrql);
    Info->NumberOfOpenPipes = n;                        /* what was there when the list was copied */
    *Returned = ActualLength;
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------
 * URBs for a downstream device: the default endpoint
 * -------------------------------------------------------------------------- */

/*
 * The setup packet a standard URB function stands for (the vocabulary the
 * hub driver and the class drivers use; usbport synthesizes the same ones).
 * Returns FALSE for a function this step does not carry.
 */
static
USBD_STATUS
UcxpUrbToSetup(
    _In_ PURB Urb,
    _Out_ PUSB_DEFAULT_PIPE_SETUP_PACKET Setup,
    _Out_ PBOOLEAN DataIn,
    _Out_ PVOID *Buffer,
    _Out_ PMDL *Mdl,
    _Out_ PULONG Length)
{
    USHORT Function = Urb->UrbHeader.Function;

    RtlZeroMemory(Setup, sizeof(*Setup));
    *DataIn = FALSE;
    *Buffer = NULL;
    *Mdl = NULL;
    *Length = 0;

    switch (Function)
    {
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
        case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
        {
            struct _URB_CONTROL_DESCRIPTOR_REQUEST *Req = &Urb->UrbControlDescriptorRequest;
            BOOLEAN Get = (Function == URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE ||
                           Function == URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE ||
                           Function == URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT);

            if (Urb->UrbHeader.Length < sizeof(*Req))
                return USBD_STATUS_INVALID_PARAMETER;
            Setup->bmRequestType.Dir = Get ? BMREQUEST_DEVICE_TO_HOST : BMREQUEST_HOST_TO_DEVICE;
            Setup->bmRequestType.Type = BMREQUEST_STANDARD;
            Setup->bmRequestType.Recipient =
                (Function == URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE || Function == URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE) ? BMREQUEST_TO_INTERFACE :
                (Function == URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT || Function == URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT) ? BMREQUEST_TO_ENDPOINT :
                BMREQUEST_TO_DEVICE;
            Setup->bRequest = Get ? USB_REQUEST_GET_DESCRIPTOR : USB_REQUEST_SET_DESCRIPTOR;
            Setup->wValue.HiByte = Req->DescriptorType;
            Setup->wValue.LowByte = Req->Index;
            Setup->wIndex.W = Req->LanguageId;
            *DataIn = Get;
            *Buffer = Req->TransferBuffer;
            *Mdl = Req->TransferBufferMDL;
            *Length = Req->TransferBufferLength;
            break;
        }

        case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
        case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
        case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
        case URB_FUNCTION_GET_STATUS_FROM_OTHER:
        {
            struct _URB_CONTROL_GET_STATUS_REQUEST *Req = &Urb->UrbControlGetStatusRequest;

            if (Urb->UrbHeader.Length < sizeof(*Req) || Req->TransferBufferLength != sizeof(USHORT))
                return USBD_STATUS_INVALID_PARAMETER;
            Setup->bmRequestType.Dir = BMREQUEST_DEVICE_TO_HOST;
            Setup->bmRequestType.Type = BMREQUEST_STANDARD;
            Setup->bmRequestType.Recipient =
                (Function == URB_FUNCTION_GET_STATUS_FROM_INTERFACE) ? BMREQUEST_TO_INTERFACE :
                (Function == URB_FUNCTION_GET_STATUS_FROM_ENDPOINT) ? BMREQUEST_TO_ENDPOINT :
                (Function == URB_FUNCTION_GET_STATUS_FROM_OTHER) ? BMREQUEST_TO_OTHER : BMREQUEST_TO_DEVICE;
            Setup->bRequest = USB_REQUEST_GET_STATUS;
            Setup->wIndex.W = Req->Index;
            *DataIn = TRUE;
            *Buffer = Req->TransferBuffer;
            *Mdl = Req->TransferBufferMDL;
            *Length = Req->TransferBufferLength;
            break;
        }

        case URB_FUNCTION_CLASS_DEVICE:
        case URB_FUNCTION_CLASS_INTERFACE:
        case URB_FUNCTION_CLASS_ENDPOINT:
        case URB_FUNCTION_CLASS_OTHER:
        case URB_FUNCTION_VENDOR_DEVICE:
        case URB_FUNCTION_VENDOR_INTERFACE:
        case URB_FUNCTION_VENDOR_ENDPOINT:
        case URB_FUNCTION_VENDOR_OTHER:
        {
            struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *Req = &Urb->UrbControlVendorClassRequest;
            BOOLEAN Class = (Function == URB_FUNCTION_CLASS_DEVICE || Function == URB_FUNCTION_CLASS_INTERFACE ||
                             Function == URB_FUNCTION_CLASS_ENDPOINT || Function == URB_FUNCTION_CLASS_OTHER);

            if (Urb->UrbHeader.Length < sizeof(*Req))
                return USBD_STATUS_INVALID_PARAMETER;
            /* The reserved bits are the caller's; direction, type and recipient are the function's */
            Setup->bmRequestType.B = Req->RequestTypeReservedBits;
            Setup->bmRequestType.Dir = (Req->TransferFlags & USBD_TRANSFER_DIRECTION_IN) ? BMREQUEST_DEVICE_TO_HOST
                                                                                         : BMREQUEST_HOST_TO_DEVICE;
            Setup->bmRequestType.Type = Class ? BMREQUEST_CLASS : BMREQUEST_VENDOR;
            Setup->bmRequestType.Recipient =
                (Function == URB_FUNCTION_CLASS_INTERFACE || Function == URB_FUNCTION_VENDOR_INTERFACE) ? BMREQUEST_TO_INTERFACE :
                (Function == URB_FUNCTION_CLASS_ENDPOINT || Function == URB_FUNCTION_VENDOR_ENDPOINT) ? BMREQUEST_TO_ENDPOINT :
                (Function == URB_FUNCTION_CLASS_OTHER || Function == URB_FUNCTION_VENDOR_OTHER) ? BMREQUEST_TO_OTHER :
                BMREQUEST_TO_DEVICE;
            Setup->bRequest = Req->Request;
            Setup->wValue.W = Req->Value;
            Setup->wIndex.W = Req->Index;
            *DataIn = (Req->TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
            *Buffer = Req->TransferBuffer;
            *Mdl = Req->TransferBufferMDL;
            *Length = Req->TransferBufferLength;
            break;
        }

        case URB_FUNCTION_SET_FEATURE_TO_DEVICE:
        case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:
        case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:
        case URB_FUNCTION_SET_FEATURE_TO_OTHER:
        case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:
        case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:
        case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:
        case URB_FUNCTION_CLEAR_FEATURE_TO_OTHER:
        {
            struct _URB_CONTROL_FEATURE_REQUEST *Req = &Urb->UrbControlFeatureRequest;
            BOOLEAN Set = (Function == URB_FUNCTION_SET_FEATURE_TO_DEVICE || Function == URB_FUNCTION_SET_FEATURE_TO_INTERFACE ||
                           Function == URB_FUNCTION_SET_FEATURE_TO_ENDPOINT || Function == URB_FUNCTION_SET_FEATURE_TO_OTHER);

            if (Urb->UrbHeader.Length < sizeof(*Req))
                return USBD_STATUS_INVALID_PARAMETER;
            Setup->bmRequestType.Dir = BMREQUEST_HOST_TO_DEVICE;
            Setup->bmRequestType.Type = BMREQUEST_STANDARD;
            Setup->bmRequestType.Recipient =
                (Function == URB_FUNCTION_SET_FEATURE_TO_INTERFACE || Function == URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE) ? BMREQUEST_TO_INTERFACE :
                (Function == URB_FUNCTION_SET_FEATURE_TO_ENDPOINT || Function == URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT) ? BMREQUEST_TO_ENDPOINT :
                (Function == URB_FUNCTION_SET_FEATURE_TO_OTHER || Function == URB_FUNCTION_CLEAR_FEATURE_TO_OTHER) ? BMREQUEST_TO_OTHER :
                BMREQUEST_TO_DEVICE;
            Setup->bRequest = Set ? USB_REQUEST_SET_FEATURE : USB_REQUEST_CLEAR_FEATURE;
            Setup->wValue.W = Req->FeatureSelector;
            Setup->wIndex.W = Req->Index;
            break;
        }

        case URB_FUNCTION_GET_CONFIGURATION:
        {
            struct _URB_CONTROL_GET_CONFIGURATION_REQUEST *Req = &Urb->UrbControlGetConfigurationRequest;

            if (Urb->UrbHeader.Length < sizeof(*Req) || Req->TransferBufferLength != 1)
                return USBD_STATUS_INVALID_PARAMETER;
            Setup->bmRequestType.Dir = BMREQUEST_DEVICE_TO_HOST;
            Setup->bRequest = USB_REQUEST_GET_CONFIGURATION;
            *DataIn = TRUE;
            *Buffer = Req->TransferBuffer;
            *Mdl = Req->TransferBufferMDL;
            *Length = 1;
            break;
        }

        case URB_FUNCTION_GET_INTERFACE:
        {
            struct _URB_CONTROL_GET_INTERFACE_REQUEST *Req = &Urb->UrbControlGetInterfaceRequest;

            if (Urb->UrbHeader.Length < sizeof(*Req) || Req->TransferBufferLength != 1)
                return USBD_STATUS_INVALID_PARAMETER;
            Setup->bmRequestType.Dir = BMREQUEST_DEVICE_TO_HOST;
            Setup->bmRequestType.Recipient = BMREQUEST_TO_INTERFACE;
            Setup->bRequest = USB_REQUEST_GET_INTERFACE;
            Setup->wIndex.W = Req->Interface;
            *DataIn = TRUE;
            *Buffer = Req->TransferBuffer;
            *Mdl = Req->TransferBufferMDL;
            *Length = 1;
            break;
        }

        case URB_FUNCTION_CONTROL_TRANSFER:
        {
            struct _URB_CONTROL_TRANSFER *Req = &Urb->UrbControlTransfer;

            if (Urb->UrbHeader.Length < sizeof(*Req))
                return USBD_STATUS_INVALID_PARAMETER;
            /* The caller's setup packet is the request: its direction and its
             * wLength stand; the buffer only has to hold what the device may send */
            RtlCopyMemory(Setup, Req->SetupPacket, sizeof(*Setup));
            *DataIn = (Setup->bmRequestType.Dir == BMREQUEST_DEVICE_TO_HOST);
            *Buffer = Req->TransferBuffer;
            *Mdl = Req->TransferBufferMDL;
            if (Setup->wLength > Req->TransferBufferLength)
                return USBD_STATUS_INVALID_PARAMETER;
            *Length = Setup->wLength;
            return USBD_STATUS_SUCCESS;
        }

        default:
            return USBD_STATUS_INVALID_URB_FUNCTION;
    }

    Setup->wLength = (USHORT)min(*Length, (ULONG)MAXUSHORT);
    return USBD_STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Configured endpoints (M3 step 3): SELECT_CONFIGURATION / SELECT_INTERFACE,
 * bulk and interrupt transfers forwarded to the client's endpoint queues,
 * pipe abort and reset.
 *
 * A USBD_PIPE_HANDLE or USBD_INTERFACE_HANDLE from a class driver is only
 * ever compared against the addresses this library handed out for the device
 * (UcxpFindPipe / UcxpFindInterface); it is never dereferenced before that.
 * The configuration descriptor is the class driver's copy and is walked within
 * the bounds it states (wTotalLength) and each descriptor's own bLength.
 * -------------------------------------------------------------------------- */

/* Caller holds Handle->PipeLock */
static
PUCX_PIPE_HANDLE
UcxpFindPipeLocked(
    _In_ PUCX_DEVICE_HANDLE Handle,
    _In_opt_ USBD_PIPE_HANDLE PipeHandle)
{
    ULONG i, p;

    if (PipeHandle == NULL)
        return NULL;
    if (PipeHandle == (USBD_PIPE_HANDLE)&Handle->DefaultPipe)
        return &Handle->DefaultPipe;
    for (i = 0; i < Handle->InterfaceCount; i++)
    {
        PUCX_INTERFACE Iface = Handle->Interfaces[i];

        if (Iface == NULL)
            continue;
        for (p = 0; p < Iface->PipeCount; p++)
        {
            if (PipeHandle == (USBD_PIPE_HANDLE)&Iface->Pipes[p])
                return &Iface->Pipes[p];
        }
    }
    return NULL;
}

/*
 * Resolve a pipe handle from an URB to a referenced endpoint object and a copy
 * of the pipe's descriptor, or fail. The record itself may be freed the moment
 * the lock is dropped; only the copy and the referenced object are used after.
 */
static
USBD_STATUS
UcxpPipeOpen(
    _In_ PUCX_DEVICE_HANDLE Handle,
    _In_opt_ USBD_PIPE_HANDLE PipeHandle,
    _Out_ UCXENDPOINT *Endpoint,
    _Out_ PUSB_ENDPOINT_DESCRIPTOR Descriptor,
    _Out_opt_ PBOOLEAN IsDefault)
{
    PUCX_PIPE_HANDLE Pipe;
    KIRQL OldIrql;
    USBD_STATUS UsbdStatus = USBD_STATUS_SUCCESS;

    *Endpoint = NULL;
    RtlZeroMemory(Descriptor, sizeof(*Descriptor));
    if (IsDefault != NULL)
        *IsDefault = FALSE;

    KeAcquireSpinLock(&Handle->PipeLock, &OldIrql);
    Pipe = UcxpFindPipeLocked(Handle, PipeHandle);
    if (Pipe == NULL)
        UsbdStatus = USBD_STATUS_INVALID_PIPE_HANDLE;
    else if (Pipe == &Handle->DefaultPipe)
    {
        if (IsDefault != NULL)
            *IsDefault = TRUE;
        *Descriptor = Pipe->Descriptor;
    }
    else if (InterlockedCompareExchange(&Pipe->Closing, 0, 0) != 0 || Pipe->Endpoint == NULL)
        UsbdStatus = USBD_STATUS_DEVICE_GONE;
    else
    {
        *Descriptor = Pipe->Descriptor;
        *Endpoint = Pipe->Endpoint;
        WdfObjectReferenceWithTag((WDFOBJECT)*Endpoint, (PVOID)'pPcU');
    }
    KeReleaseSpinLock(&Handle->PipeLock, OldIrql);
    return UsbdStatus;
}

static
VOID
UcxpPipeClose(
    _In_opt_ UCXENDPOINT Endpoint)
{
    if (Endpoint != NULL)
        WdfObjectDereferenceWithTag((WDFOBJECT)Endpoint, (PVOID)'pPcU');
}

/* Marks an interface's pipes closing (no new transfers); caller holds PipeLock */
static
VOID
UcxpInterfaceSetClosingLocked(
    _In_ PUCX_INTERFACE Iface,
    _In_ LONG Closing)
{
    ULONG p;

    for (p = 0; p < Iface->PipeCount; p++)
        InterlockedExchange(&Iface->Pipes[p].Closing, Closing);
}

static
ULONG
UcxpFindInterfaceIndex(
    _In_ PUCX_DEVICE_HANDLE Handle,
    _In_ UCHAR InterfaceNumber)
{
    ULONG i;

    for (i = 0; i < Handle->InterfaceCount; i++)
    {
        if (Handle->Interfaces[i] != NULL && Handle->Interfaces[i]->InterfaceNumber == InterfaceNumber)
            return i;
    }
    return (ULONG)-1;
}

/* The descriptor after Current inside the configuration, or NULL at the end / on a malformed length */
static
PUSB_COMMON_DESCRIPTOR
UcxpNextDescriptor(
    _In_ PUSB_CONFIGURATION_DESCRIPTOR Config,
    _In_ ULONG TotalLength,
    _In_opt_ PUSB_COMMON_DESCRIPTOR Current)
{
    PUCHAR Base = (PUCHAR)Config;
    PUCHAR End = Base + TotalLength;
    PUCHAR Next;

    if (Current == NULL)
        Next = Base + Config->bLength;
    else
        Next = (PUCHAR)Current + Current->bLength;

    if (Next < Base || Next + sizeof(USB_COMMON_DESCRIPTOR) > End)
        return NULL;
    if (((PUSB_COMMON_DESCRIPTOR)Next)->bLength < sizeof(USB_COMMON_DESCRIPTOR) ||
        Next + ((PUSB_COMMON_DESCRIPTOR)Next)->bLength > End)
        return NULL;
    return (PUSB_COMMON_DESCRIPTOR)Next;
}

static
PUSB_INTERFACE_DESCRIPTOR
UcxpFindInterfaceDescriptor(
    _In_ PUSB_CONFIGURATION_DESCRIPTOR Config,
    _In_ ULONG TotalLength,
    _In_ UCHAR InterfaceNumber,
    _In_ UCHAR AlternateSetting)
{
    PUSB_COMMON_DESCRIPTOR D = NULL;

    while ((D = UcxpNextDescriptor(Config, TotalLength, D)) != NULL)
    {
        if (D->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE && D->bLength >= sizeof(USB_INTERFACE_DESCRIPTOR))
        {
            PUSB_INTERFACE_DESCRIPTOR I = (PUSB_INTERFACE_DESCRIPTOR)D;

            if (I->bInterfaceNumber == InterfaceNumber && I->bAlternateSetting == AlternateSetting)
                return I;
        }
    }
    return NULL;
}

/*
 * The next endpoint descriptor of the interface after *Cursor (stops at the
 * next interface descriptor). A SuperSpeed companion right behind it is
 * returned too.
 */
static
PUSB_ENDPOINT_DESCRIPTOR
UcxpNextEndpointDescriptor(
    _In_ PUSB_CONFIGURATION_DESCRIPTOR Config,
    _In_ ULONG TotalLength,
    _Inout_ PUSB_COMMON_DESCRIPTOR *Cursor,
    _Out_ PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR *Companion)
{
    PUSB_COMMON_DESCRIPTOR D = *Cursor;

    *Companion = NULL;
    while ((D = UcxpNextDescriptor(Config, TotalLength, D)) != NULL)
    {
        if (D->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE)
        {
            *Cursor = D;
            return NULL;
        }
        if (D->bDescriptorType == USB_ENDPOINT_DESCRIPTOR_TYPE && D->bLength >= sizeof(USB_ENDPOINT_DESCRIPTOR))
        {
            PUSB_COMMON_DESCRIPTOR Peek = UcxpNextDescriptor(Config, TotalLength, D);

            if (Peek != NULL && Peek->bDescriptorType == USB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR_TYPE &&
                Peek->bLength >= sizeof(USB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR))
            {
                *Companion = (PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR)Peek;
            }
            *Cursor = D;
            return (PUSB_ENDPOINT_DESCRIPTOR)D;
        }
    }
    *Cursor = NULL;
    return NULL;
}

static
USBD_PIPE_TYPE
UcxpPipeTypeFromAttributes(
    _In_ UCHAR bmAttributes)
{
    switch (bmAttributes & USB_ENDPOINT_TYPE_MASK)
    {
        case USB_ENDPOINT_TYPE_CONTROL:     return UsbdPipeTypeControl;
        case USB_ENDPOINT_TYPE_ISOCHRONOUS: return UsbdPipeTypeIsochronous;
        case USB_ENDPOINT_TYPE_BULK:        return UsbdPipeTypeBulk;
        default:                            return UsbdPipeTypeInterrupt;
    }
}

/* Abort the transfers of every pipe of an interface (client callback); the
 * pipes were marked closing under the lock before */
static
VOID
UcxpInterfaceAbortPipes(
    _In_ PUCX_INTERFACE Iface)
{
    ULONG p;

    for (p = 0; p < Iface->PipeCount; p++)
    {
        PUCX_PIPE_HANDLE Pipe = &Iface->Pipes[p];
        PUCX_ENDPOINT_CTX Ep;

        if (Pipe->Endpoint == NULL)
            continue;
        Ep = UcxGetEndpointCtx((WDFOBJECT)Pipe->Endpoint);
        if (Ep->Callbacks.EvtEndpointAbort != NULL)
            Ep->Callbacks.EvtEndpointAbort(Pipe->Endpoint);
    }
}

/*
 * Free an interface record. Its endpoint objects are deleted when the
 * controller no longer references their rings (DeleteEndpoints); otherwise
 * they stay children of the device object and go with it at Remove, after the
 * slot was disabled.
 */
static
VOID
UcxpInterfaceDestroy(
    _Inout_ PUCX_INTERFACE Iface,
    _In_ BOOLEAN DeleteEndpoints)
{
    ULONG p;

    for (p = 0; p < Iface->PipeCount; p++)
    {
        UCXENDPOINT Endpoint = Iface->Pipes[p].Endpoint;

        Iface->Pipes[p].Endpoint = NULL;
        Iface->Pipes[p].Signature = 0;
        if (Endpoint != NULL && DeleteEndpoints)
            WdfObjectDelete((WDFOBJECT)Endpoint);
    }
    Iface->Signature = 0;
    ExFreePoolWithTag(Iface, UCX_TAG);
}

/*
 * Build the interface record for one USBD_INTERFACE_INFORMATION of a select
 * URB: find its descriptor, create one endpoint object per endpoint through
 * the client, fill in what the class driver reads back (class codes, handles,
 * pipe properties). *InfoBytes is the URB memory left for Info on entry.
 */
static
USBD_STATUS
UcxpInterfaceBuild(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _In_ PUCX_DEVICE_HANDLE Handle,
    _In_ PUSB_CONFIGURATION_DESCRIPTOR Config,
    _In_ ULONG TotalLength,
    _Inout_ PUSBD_INTERFACE_INFORMATION Info,
    _In_ ULONG InfoBytes,
    _Outptr_ PUCX_INTERFACE *Out)
{
    PUCX_USBDEVICE_CTX Device = UcxpDeviceCtx(Handle);
    PUSB_INTERFACE_DESCRIPTOR IfaceDesc;
    PUSB_COMMON_DESCRIPTOR Cursor;
    PUCX_INTERFACE Iface;
    ULONG Needed, p;

    *Out = NULL;
    if (Device == NULL || Device->Callbacks.EvtUsbDeviceEndpointAdd == NULL)
        return USBD_STATUS_NOT_SUPPORTED;

    if (InfoBytes < FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) ||
        Info->NumberOfPipes > UCX_MAX_PIPES)
    {
        return USBD_STATUS_INVALID_PARAMETER;
    }
    Needed = FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) + Info->NumberOfPipes * sizeof(USBD_PIPE_INFORMATION);
    if (Info->Length < Needed || Info->Length > InfoBytes)
        return USBD_STATUS_INVALID_PARAMETER;

    IfaceDesc = UcxpFindInterfaceDescriptor(Config, TotalLength, Info->InterfaceNumber, Info->AlternateSetting);
    if (IfaceDesc == NULL || IfaceDesc->bNumEndpoints != Info->NumberOfPipes)
    {
        UcxLog("device: port %u interface %u alt %u: %s (%lu pipes asked)", Handle->PortNumber,
               Info->InterfaceNumber, Info->AlternateSetting,
               IfaceDesc == NULL ? "no such descriptor" : "endpoint count differs", Info->NumberOfPipes);
        return USBD_STATUS_INVALID_CONFIGURATION_DESCRIPTOR;
    }

    Iface = ExAllocatePoolZero(NonPagedPoolNx, UCX_INTERFACE_SIZE(Info->NumberOfPipes), UCX_TAG);
    if (Iface == NULL)
        return USBD_STATUS_INSUFFICIENT_RESOURCES;
    Iface->Signature = UCX_INTERFACE_SIGNATURE;
    Iface->InterfaceNumber = IfaceDesc->bInterfaceNumber;
    Iface->AlternateSetting = IfaceDesc->bAlternateSetting;
    Iface->Class = IfaceDesc->bInterfaceClass;
    Iface->SubClass = IfaceDesc->bInterfaceSubClass;
    Iface->Protocol = IfaceDesc->bInterfaceProtocol;
    Iface->PipeCount = (UCHAR)Info->NumberOfPipes;

    Cursor = (PUSB_COMMON_DESCRIPTOR)IfaceDesc;
    for (p = 0; p < Info->NumberOfPipes; p++)
    {
        PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR Companion;
        PUSB_ENDPOINT_DESCRIPTOR EpDesc = UcxpNextEndpointDescriptor(Config, TotalLength, &Cursor, &Companion);
        PUCX_PIPE_HANDLE Pipe = &Iface->Pipes[p];
        PUSBD_PIPE_INFORMATION PipeInfo = &Info->Pipes[p];
        struct _UCXENDPOINT_INIT *Init;
        UCX_ENDPOINT_INFO EpInfo;
        NTSTATUS Status;

        if (EpDesc == NULL || (EpDesc->bEndpointAddress & USB_ENDPOINT_ADDRESS_MASK) == 0 ||
            (EpDesc->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_CONTROL)
        {
            UcxLog("device: port %u interface %u: endpoint %lu missing or unusable", Handle->PortNumber,
                   Info->InterfaceNumber, p);
            UcxpInterfaceDestroy(Iface, TRUE);
            return USBD_STATUS_INVALID_CONFIGURATION_DESCRIPTOR;
        }

        Pipe->Signature = UCX_PIPE_HANDLE_SIGNATURE;
        RtlCopyMemory(&Pipe->Descriptor, EpDesc, sizeof(USB_ENDPOINT_DESCRIPTOR));
        Pipe->MaximumTransferSize = (PipeInfo->MaximumTransferSize != 0 &&
                                     PipeInfo->MaximumTransferSize < UCX_BULK_MAX_TRANSFER) ?
                                    PipeInfo->MaximumTransferSize : UCX_BULK_MAX_TRANSFER;

        Init = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Init), UCX_TAG);
        if (Init == NULL)
        {
            UcxpInterfaceDestroy(Iface, TRUE);
            return USBD_STATUS_INSUFFICIENT_RESOURCES;
        }
        Init->Signature = UCX_ENDPOINT_INIT_SIGNATURE;
        Init->UsbDevice = Handle->UsbDevice;
        Init->Descriptor = Pipe->Descriptor;

        RtlZeroMemory(&EpInfo, sizeof(EpInfo));
        EpInfo.Size = sizeof(EpInfo);
        EpInfo.Descriptor = Pipe->Descriptor;
        if (Companion != NULL)
        {
            RtlCopyMemory(&EpInfo.Companion, Companion, sizeof(EpInfo.Companion));
            EpInfo.HasCompanion = TRUE;
        }
        EpInfo.InterfaceNumber = Iface->InterfaceNumber;
        EpInfo.AlternateSetting = Iface->AlternateSetting;
        EpInfo.MaximumTransferSize = Pipe->MaximumTransferSize;

        Status = Device->Callbacks.EvtUsbDeviceEndpointAdd(RootHub->Controller, Handle->UsbDevice, &EpInfo, Init);
        if (NT_SUCCESS(Status) && Init->Created == NULL)
            Status = STATUS_UNSUCCESSFUL;
        if (!NT_SUCCESS(Status))
        {
            UcxLog("device: port %u endpoint 0x%02x add refused by the client 0x%08lX", Handle->PortNumber,
                   EpDesc->bEndpointAddress, (ULONG)Status);
            if (Init->Created != NULL)
                WdfObjectDelete((WDFOBJECT)Init->Created);
            ExFreePoolWithTag(Init, UCX_TAG);
            UcxpInterfaceDestroy(Iface, TRUE);
            return USBD_STATUS_INSUFFICIENT_RESOURCES;
        }
        Pipe->Endpoint = Init->Created;
        ExFreePoolWithTag(Init, UCX_TAG);

        /* What the class driver reads back */
        PipeInfo->EndpointAddress = EpDesc->bEndpointAddress;
        PipeInfo->Interval = EpDesc->bInterval;
        PipeInfo->PipeType = UcxpPipeTypeFromAttributes(EpDesc->bmAttributes);
        PipeInfo->MaximumPacketSize = (Handle->DeviceSpeed == UsbSuperSpeed) ? EpDesc->wMaxPacketSize
                                                                             : (EpDesc->wMaxPacketSize & 0x07FF);
        PipeInfo->PipeHandle = (USBD_PIPE_HANDLE)Pipe;
        PipeInfo->MaximumTransferSize = Pipe->MaximumTransferSize;
    }

    Info->Class = Iface->Class;
    Info->SubClass = Iface->SubClass;
    Info->Protocol = Iface->Protocol;
    Info->InterfaceHandle = (USBD_INTERFACE_HANDLE)Iface;
    *Out = Iface;
    return USBD_STATUS_SUCCESS;
}

/* Tell the client which endpoints to enable / disable (one Configure Endpoint) */
static
NTSTATUS
UcxpConfigureEndpoints(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _In_ PUCX_DEVICE_HANDLE Handle,
    _In_ UCX_ENDPOINTS_CONFIGURE_TYPE Type,
    _In_ UCHAR ConfigurationValue,
    _In_ UCHAR InterfaceNumber,
    _In_ UCHAR AlternateSetting,
    _In_reads_opt_(EnableCount) PUCX_INTERFACE *Enable,
    _In_ ULONG EnableCount,
    _In_reads_opt_(DisableCount) PUCX_INTERFACE *Disable,
    _In_ ULONG DisableCount)
{
    PUCX_USBDEVICE_CTX Device = UcxpDeviceCtx(Handle);
    UCX_ENDPOINTS_CONFIGURE Cfg;
    UCXENDPOINT *Endpoints;
    ULONG Total = 0, n = 0, i, p;
    NTSTATUS Status;

    if (Device == NULL || Device->Callbacks.EvtUsbDeviceEndpointsConfigure == NULL)
        return STATUS_NOT_SUPPORTED;

    for (i = 0; i < EnableCount; i++)
        Total += Enable[i]->PipeCount;
    for (i = 0; i < DisableCount; i++)
        Total += Disable[i]->PipeCount;

    Endpoints = ExAllocatePoolZero(NonPagedPoolNx, (Total ? Total : 1) * sizeof(UCXENDPOINT), UCX_TAG);
    if (Endpoints == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(&Cfg, sizeof(Cfg));
    Cfg.Size = sizeof(Cfg);
    Cfg.Type = Type;
    Cfg.ConfigurationValue = ConfigurationValue;
    Cfg.InterfaceNumber = InterfaceNumber;
    Cfg.AlternateSetting = AlternateSetting;
    Cfg.EndpointsToEnable = Endpoints;
    for (i = 0; i < EnableCount; i++)
        for (p = 0; p < Enable[i]->PipeCount; p++)
            Endpoints[n++] = Enable[i]->Pipes[p].Endpoint;
    Cfg.EnableCount = n;
    Cfg.EndpointsToDisable = Endpoints + n;
    for (i = 0; i < DisableCount; i++)
        for (p = 0; p < Disable[i]->PipeCount; p++)
            Endpoints[n++] = Disable[i]->Pipes[p].Endpoint;
    Cfg.DisableCount = n - Cfg.EnableCount;

    Status = Device->Callbacks.EvtUsbDeviceEndpointsConfigure(RootHub->Controller, Handle->UsbDevice, &Cfg);
    UcxLog("device: port %u endpoints configure type %u (+%lu -%lu) -> 0x%08lX", Handle->PortNumber,
           Type, Cfg.EnableCount, Cfg.DisableCount, (ULONG)Status);
    ExFreePoolWithTag(Endpoints, UCX_TAG);
    return Status;
}

/* SET_CONFIGURATION(value) / SET_INTERFACE(alt, interface) on the default endpoint */
static
USBD_STATUS
UcxpDeviceSetConfigurationOrInterface(
    _In_ PUCX_DEVICE_HANDLE Handle,
    _In_ BOOLEAN Interface,
    _In_ USHORT Value,
    _In_ USHORT Index)
{
    USB_DEFAULT_PIPE_SETUP_PACKET Setup;
    USBD_STATUS UsbdStatus;
    ULONG Transferred = 0;

    RtlZeroMemory(&Setup, sizeof(Setup));
    Setup.bmRequestType.Dir = BMREQUEST_HOST_TO_DEVICE;
    Setup.bmRequestType.Type = BMREQUEST_STANDARD;
    Setup.bmRequestType.Recipient = Interface ? BMREQUEST_TO_INTERFACE : BMREQUEST_TO_DEVICE;
    Setup.bRequest = Interface ? USB_REQUEST_SET_INTERFACE : USB_REQUEST_SET_CONFIGURATION;
    Setup.wValue.W = Value;
    Setup.wIndex.W = Index;
    Setup.wLength = 0;
    (VOID)UcxpControlTransfer(Handle, &Setup, NULL, NULL, 0, FALSE, &Transferred, &UsbdStatus);
    return UsbdStatus;
}

/* Everything of the current configuration goes: transfers, endpoints, records */
static
VOID
UcxpDeviceUnconfigure(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle,
    _In_ BOOLEAN TellDevice)
{
    PUCX_INTERFACE Ifaces[UCX_MAX_INTERFACES];
    PUSB_CONFIGURATION_DESCRIPTOR Config;
    ULONG Count = 0, i;
    UCHAR OldValue = Handle->ConfigurationValue;
    KIRQL OldIrql;
    NTSTATUS Status = STATUS_SUCCESS;

    /* Out of the table first: no URB finds a pipe from here on */
    KeAcquireSpinLock(&Handle->PipeLock, &OldIrql);
    for (i = 0; i < Handle->InterfaceCount; i++)
    {
        if (Handle->Interfaces[i] != NULL)
        {
            UcxpInterfaceSetClosingLocked(Handle->Interfaces[i], 1);
            Ifaces[Count++] = Handle->Interfaces[i];
        }
    }
    RtlZeroMemory(Handle->Interfaces, sizeof(Handle->Interfaces));
    Handle->InterfaceCount = 0;
    Handle->ConfigurationValue = 0;
    Handle->HubInfoSent = FALSE;           /* the Deconfigure drops it with the endpoints */
    Config = Handle->ConfigDescriptor;
    Handle->ConfigDescriptor = NULL;
    Handle->ConfigDescriptorLength = 0;
    KeReleaseSpinLock(&Handle->PipeLock, OldIrql);

    for (i = 0; i < Count; i++)
        UcxpInterfaceAbortPipes(Ifaces[i]);
    if (Count != 0 || OldValue != 0)
        Status = UcxpConfigureEndpoints(RootHub, Handle, UcxEndpointsConfigureDisableAll, 0, 0, 0,
                                        NULL, 0, Ifaces, Count);
    if (TellDevice && OldValue != 0)
        (VOID)UcxpDeviceSetConfigurationOrInterface(Handle, FALSE, 0, 0);

    /* Rings the controller may still reference (a failed Deconfigure) are not
     * freed now: those endpoint objects go with the device at Remove */
    for (i = 0; i < Count; i++)
        UcxpInterfaceDestroy(Ifaces[i], NT_SUCCESS(Status));
    if (Config != NULL)
        ExFreePoolWithTag(Config, UCX_TAG);
}

static
USBD_STATUS
UcxpDeviceSelectConfiguration(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle,
    _Inout_ PURB Urb)
{
    struct _URB_SELECT_CONFIGURATION *Req = &Urb->UrbSelectConfiguration;
    PUSB_CONFIGURATION_DESCRIPTOR Config = Req->ConfigurationDescriptor;
    PUSBD_INTERFACE_INFORMATION Info;
    PUCX_INTERFACE Built[UCX_MAX_INTERFACES];
    ULONG BuiltCount = 0, Remaining, TotalLength, Pipes = 0, i, p;
    ULONG SeenInterfaces = 0, SeenEndpoints = 0;      /* bit per interface number / per endpoint address */
    USBD_STATUS UsbdStatus;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (Urb->UrbHeader.Length < FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface))
        return USBD_STATUS_INVALID_PARAMETER;

    /* Whatever was selected before goes first (also for a NULL descriptor: unconfigure) */
    UcxpDeviceUnconfigure(RootHub, Handle, Config == NULL);
    Req->ConfigurationHandle = NULL;
    if (Config == NULL)
    {
        UcxLog("device: port %u unconfigured", Handle->PortNumber);
        return USBD_STATUS_SUCCESS;
    }

    if (Config->bLength != sizeof(USB_CONFIGURATION_DESCRIPTOR) ||
        Config->bDescriptorType != USB_CONFIGURATION_DESCRIPTOR_TYPE ||
        Config->wTotalLength < sizeof(USB_CONFIGURATION_DESCRIPTOR) ||
        Config->bNumInterfaces == 0 || Config->bNumInterfaces > UCX_MAX_INTERFACES ||
        Config->bConfigurationValue == 0)
    {
        return USBD_STATUS_INVALID_CONFIGURATION_DESCRIPTOR;
    }
    TotalLength = Config->wTotalLength;

    /* Our own copy: SELECT_INTERFACE later needs the alternate settings */
    Handle->ConfigDescriptor = ExAllocatePoolZero(NonPagedPoolNx, TotalLength, UCX_TAG);
    if (Handle->ConfigDescriptor == NULL)
        return USBD_STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(Handle->ConfigDescriptor, Config, TotalLength);
    Handle->ConfigDescriptorLength = TotalLength;
    Config = Handle->ConfigDescriptor;

    Info = &Req->Interface;
    Remaining = Urb->UrbHeader.Length - FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface);
    for (i = 0; i < Config->bNumInterfaces; i++)
    {
        if (Remaining < FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes))
        {
            UcxLog("device: port %u SELECT_CONFIGURATION names %u interfaces, URB holds %lu",
                   Handle->PortNumber, Config->bNumInterfaces, i);
            UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
            goto Fail;
        }
        UsbdStatus = UcxpInterfaceBuild(RootHub, Handle, Config, TotalLength, Info, Remaining, &Built[BuiltCount]);
        if (!USBD_SUCCESS(UsbdStatus))
            goto Fail;
        BuiltCount++;
        /* Each interface number once, each endpoint address once */
        {
            PUCX_INTERFACE B = Built[BuiltCount - 1];

            if (B->InterfaceNumber >= 32 || (SeenInterfaces & (1u << B->InterfaceNumber)))
                UsbdStatus = USBD_STATUS_INVALID_CONFIGURATION_DESCRIPTOR;
            SeenInterfaces |= (B->InterfaceNumber < 32) ? (1u << B->InterfaceNumber) : 0;
            for (p = 0; p < B->PipeCount && USBD_SUCCESS(UsbdStatus); p++)
            {
                UCHAR Address = B->Pipes[p].Descriptor.bEndpointAddress;
                ULONG Bit = 1u << ((Address & USB_ENDPOINT_ADDRESS_MASK) | ((Address & USB_ENDPOINT_DIRECTION_MASK) ? 16 : 0));

                if (SeenEndpoints & Bit)
                    UsbdStatus = USBD_STATUS_INVALID_CONFIGURATION_DESCRIPTOR;
                SeenEndpoints |= Bit;
            }
            if (!USBD_SUCCESS(UsbdStatus))
            {
                UcxLog("device: port %u SELECT_CONFIGURATION repeats interface %u or one of its endpoints",
                       Handle->PortNumber, B->InterfaceNumber);
                goto Fail;
            }
            Pipes += B->PipeCount;
        }
        Remaining -= Info->Length;
        Info = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)Info + Info->Length);
    }

    Status = UcxpConfigureEndpoints(RootHub, Handle, UcxEndpointsConfigureConfiguration,
                                    Config->bConfigurationValue, 0, 0, Built, BuiltCount, NULL, 0);
    if (!NT_SUCCESS(Status))
    {
        UsbdStatus = USBD_STATUS_INSUFFICIENT_RESOURCES;   /* a failed Configure Endpoint changes nothing on the controller */
        goto Fail;
    }
    KeAcquireSpinLock(&Handle->PipeLock, &OldIrql);
    for (i = 0; i < BuiltCount; i++)
        Handle->Interfaces[i] = Built[i];
    Handle->InterfaceCount = BuiltCount;
    Handle->ConfigurationValue = Config->bConfigurationValue;
    KeReleaseSpinLock(&Handle->PipeLock, OldIrql);

    /* A hub: the controller's slot must say so before devices behind it can be addressed */
    if (Handle->IsHub)
    {
        Status = UcxpDeviceReadHubDescriptor(Handle);
        if (NT_SUCCESS(Status))
            Status = UcxpDeviceSendHubInfo(RootHub, Handle);
        if (!NT_SUCCESS(Status))
        {
            UcxLog("device: port %u hub information failed 0x%08lX", Handle->PortNumber, (ULONG)Status);
            UcxpDeviceUnconfigure(RootHub, Handle, FALSE);
            return USBD_STATUS_REQUEST_FAILED;
        }
    }

    UsbdStatus = UcxpDeviceSetConfigurationOrInterface(Handle, FALSE, Config->bConfigurationValue, 0);
    if (!USBD_SUCCESS(UsbdStatus))
    {
        UcxLog("device: port %u SET_CONFIGURATION(%u) failed, USBD 0x%08lX", Handle->PortNumber,
               Config->bConfigurationValue, (ULONG)UsbdStatus);
        UcxpDeviceUnconfigure(RootHub, Handle, FALSE);
        return UsbdStatus;
    }

    Req->ConfigurationHandle = (USBD_CONFIGURATION_HANDLE)&Handle->ConfigurationCookie;
    UcxLog("device: port %u configured %u: %lu interface(s), %lu pipe(s)", Handle->PortNumber,
           Handle->ConfigurationValue, BuiltCount, Pipes);
    return USBD_STATUS_SUCCESS;

Fail:
    for (i = 0; i < BuiltCount; i++)
        UcxpInterfaceDestroy(Built[i], TRUE);              /* never reached the controller */
    if (Handle->ConfigDescriptor != NULL)
    {
        ExFreePoolWithTag(Handle->ConfigDescriptor, UCX_TAG);
        Handle->ConfigDescriptor = NULL;
        Handle->ConfigDescriptorLength = 0;
    }
    return UsbdStatus;
}

static
USBD_STATUS
UcxpDeviceSelectInterface(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle,
    _Inout_ PURB Urb)
{
    struct _URB_SELECT_INTERFACE *Req = &Urb->UrbSelectInterface;
    PUSBD_INTERFACE_INFORMATION Info = &Req->Interface;
    PUCX_INTERFACE Old, New = NULL;
    ULONG Index, Remaining;
    KIRQL OldIrql;
    USBD_STATUS UsbdStatus;
    NTSTATUS Status;

    if (Urb->UrbHeader.Length < FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface) +
                                FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes))
    {
        return USBD_STATUS_INVALID_PARAMETER;
    }
    if (Handle->ConfigurationValue == 0 || Handle->ConfigDescriptor == NULL ||
        Req->ConfigurationHandle != (USBD_CONFIGURATION_HANDLE)&Handle->ConfigurationCookie)
    {
        return USBD_STATUS_INVALID_PARAMETER;
    }
    Index = UcxpFindInterfaceIndex(Handle, Info->InterfaceNumber);
    if (Index == (ULONG)-1)
        return USBD_STATUS_INVALID_PARAMETER;
    Old = Handle->Interfaces[Index];

    /* The new setting's endpoint objects first (the controller does not see them yet) */
    Remaining = Urb->UrbHeader.Length - FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface);
    UsbdStatus = UcxpInterfaceBuild(RootHub, Handle, Handle->ConfigDescriptor, Handle->ConfigDescriptorLength,
                                    Info, Remaining, &New);
    if (!USBD_SUCCESS(UsbdStatus))
        return UsbdStatus;

    /* The old setting's pipes are done; the controller swaps the endpoints in one command */
    KeAcquireSpinLock(&Handle->PipeLock, &OldIrql);
    UcxpInterfaceSetClosingLocked(Old, 1);
    KeReleaseSpinLock(&Handle->PipeLock, OldIrql);
    UcxpInterfaceAbortPipes(Old);

    Status = UcxpConfigureEndpoints(RootHub, Handle, UcxEndpointsConfigureInterface, Handle->ConfigurationValue,
                                    New->InterfaceNumber, New->AlternateSetting, &New, 1, &Old, 1);
    if (!NT_SUCCESS(Status))
    {
        /* Nothing changed on the controller: the old setting stays in force */
        KeAcquireSpinLock(&Handle->PipeLock, &OldIrql);
        UcxpInterfaceSetClosingLocked(Old, 0);
        KeReleaseSpinLock(&Handle->PipeLock, OldIrql);
        UcxpInterfaceDestroy(New, TRUE);
        return USBD_STATUS_INSUFFICIENT_RESOURCES;
    }
    KeAcquireSpinLock(&Handle->PipeLock, &OldIrql);
    Handle->Interfaces[Index] = New;
    KeReleaseSpinLock(&Handle->PipeLock, OldIrql);

    UsbdStatus = UcxpDeviceSetConfigurationOrInterface(Handle, TRUE, New->AlternateSetting, New->InterfaceNumber);
    if (!USBD_SUCCESS(UsbdStatus))
    {
        /* The device kept the old setting: put the controller back to it too */
        Status = UcxpConfigureEndpoints(RootHub, Handle, UcxEndpointsConfigureInterface, Handle->ConfigurationValue,
                                        Old->InterfaceNumber, Old->AlternateSetting, &Old, 1, &New, 1);
        UcxLog("device: port %u SET_INTERFACE(%u, alt %u) failed USBD 0x%08lX; controller rolled back -> 0x%08lX",
               Handle->PortNumber, New->InterfaceNumber, New->AlternateSetting, (ULONG)UsbdStatus, (ULONG)Status);
        if (NT_SUCCESS(Status))
        {
            KeAcquireSpinLock(&Handle->PipeLock, &OldIrql);
            Handle->Interfaces[Index] = Old;
            UcxpInterfaceSetClosingLocked(Old, 0);
            KeReleaseSpinLock(&Handle->PipeLock, OldIrql);
            UcxpInterfaceDestroy(New, TRUE);
        }
        else
        {
            /* The controller has the new endpoints and could not be told otherwise:
             * the old objects stay (with the device) and the new setting stands host-side */
            UcxpInterfaceDestroy(Old, FALSE);
        }
        return UsbdStatus;
    }

    UcxpInterfaceDestroy(Old, TRUE);
    UcxLog("device: port %u interface %u -> alternate setting %u, %u pipe(s)", Handle->PortNumber,
           New->InterfaceNumber, New->AlternateSetting, New->PipeCount);
    return USBD_STATUS_SUCCESS;
}

/* Context cleanup on a forwarded request that was never completed through us */
static
VOID
NTAPI
UcxpRequestCtxCleanup(
    _In_ WDFOBJECT Object)
{
    PUCX_REQUEST_CTX Ctx = UcxGetRequestCtx(Object);

    if (Ctx->OwnedMdl != NULL)
    {
        IoFreeMdl(Ctx->OwnedMdl);
        Ctx->OwnedMdl = NULL;
    }
    if (Ctx->Endpoint != NULL)
    {
        InterlockedDecrement(&UcxGetEndpointCtx((WDFOBJECT)Ctx->Endpoint)->Transfers);
        Ctx->Endpoint = NULL;
    }
}

/*
 * BULK_OR_INTERRUPT_TRANSFER: checked here, carried out by the client. The
 * request leaves this routine either forwarded to the endpoint's queue or
 * completed with an error. The URB is read, never written, until completion.
 */
static
VOID
UcxpDeviceBulkTransfer(
    _In_ PUCX_DEVICE_HANDLE Handle,
    _In_ WDFREQUEST Request,
    _Inout_ PURB Urb)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *Req = &Urb->UrbBulkOrInterruptTransfer;
    USB_ENDPOINT_DESCRIPTOR Descriptor;
    UCXENDPOINT Endpoint = NULL;
    PUCX_ENDPOINT_CTX Ep;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_REQUEST_FORWARD_OPTIONS Options;
    PUCX_REQUEST_CTX Ctx = NULL;
    PMDL OwnedMdl = NULL;
    USBD_STATUS UsbdStatus;
    BOOLEAN IsDefault;
    UCHAR Type;
    NTSTATUS Status;

    if (Urb->UrbHeader.Length < sizeof(*Req))
    {
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_INVALID_PARAMETER);
        return;
    }
    UsbdStatus = UcxpPipeOpen(Handle, Req->PipeHandle, &Endpoint, &Descriptor, &IsDefault);
    if (USBD_SUCCESS(UsbdStatus) && (IsDefault || Endpoint == NULL))
        UsbdStatus = USBD_STATUS_INVALID_PIPE_HANDLE;
    Type = Descriptor.bmAttributes & USB_ENDPOINT_TYPE_MASK;
    if (USBD_SUCCESS(UsbdStatus) && Type != USB_ENDPOINT_TYPE_BULK && Type != USB_ENDPOINT_TYPE_INTERRUPT)
        UsbdStatus = USBD_STATUS_INVALID_PIPE_HANDLE;
    if (USBD_SUCCESS(UsbdStatus))
    {
        Ep = UcxGetEndpointCtx((WDFOBJECT)Endpoint);
        if (Ep->Queue == NULL)
            UsbdStatus = USBD_STATUS_NOT_SUPPORTED;
    }
    if (USBD_SUCCESS(UsbdStatus) && Req->TransferBufferLength > UCX_BULK_MAX_TRANSFER)
        UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
    if (!USBD_SUCCESS(UsbdStatus))
    {
        UcxpPipeClose(Endpoint);
        Req->TransferBufferLength = 0;
        UcxpCompleteUrb(Request, Urb, UsbdStatus);
        return;
    }

    /* The endpoint's direction is the transfer's (usbport does the same) */
    if (USB_ENDPOINT_DIRECTION_IN(Descriptor.bEndpointAddress))
        Req->TransferFlags |= USBD_TRANSFER_DIRECTION_IN;
    else
        Req->TransferFlags &= ~USBD_TRANSFER_DIRECTION_IN;

    if (Req->TransferBufferLength != 0)
    {
        if (Req->TransferBufferMDL != NULL)
        {
            PMDL Mdl = Req->TransferBufferMDL;

            if (MmGetMdlByteCount(Mdl) < Req->TransferBufferLength ||
                (Mdl->MdlFlags & (MDL_PAGES_LOCKED | MDL_SOURCE_IS_NONPAGED_POOL)) == 0)
            {
                UcxpPipeClose(Endpoint);
                Req->TransferBufferLength = 0;
                UcxpCompleteUrb(Request, Urb, USBD_STATUS_INVALID_PARAMETER);
                return;
            }
        }
        else if (Req->TransferBuffer != NULL)
        {
            /* Ours, kept in the request context; the client asks for it with
             * UcxRequestGetTransferMdl and the URB keeps its NULL */
            OwnedMdl = IoAllocateMdl(Req->TransferBuffer, Req->TransferBufferLength, FALSE, FALSE, NULL);
            if (OwnedMdl == NULL)
            {
                UcxpPipeClose(Endpoint);
                Req->TransferBufferLength = 0;
                UcxpCompleteUrb(Request, Urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
                return;
            }
            MmBuildMdlForNonPagedPool(OwnedMdl);
        }
        else
        {
            UcxpPipeClose(Endpoint);
            Req->TransferBufferLength = 0;
            UcxpCompleteUrb(Request, Urb, USBD_STATUS_INVALID_PARAMETER);
            return;
        }
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, UCX_REQUEST_CTX);
    Attributes.EvtCleanupCallback = UcxpRequestCtxCleanup;
    Status = WdfObjectAllocateContext((WDFOBJECT)Request, &Attributes, (PVOID *)&Ctx);
    if (Status == STATUS_OBJECT_NAME_EXISTS)
    {
        Ctx = UcxGetRequestCtx((WDFOBJECT)Request);
        Status = (Ctx != NULL) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    }
    if (!NT_SUCCESS(Status) || Ctx == NULL)
    {
        if (OwnedMdl != NULL)
            IoFreeMdl(OwnedMdl);
        UcxpPipeClose(Endpoint);
        Req->TransferBufferLength = 0;
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    Ctx->Endpoint = Endpoint;
    Ctx->Pipe = NULL;                                 /* the record may go; the object is referenced below */
    Ctx->OwnedMdl = OwnedMdl;
    InterlockedIncrement(&Ep->Transfers);

    /* The queue belongs to the client's FDO, the request to our PDO: KMDF
     * allows exactly this hop (WdfPdoInitAllowForwardingRequestToParent) */
    WDF_REQUEST_FORWARD_OPTIONS_INIT(&Options);
    Status = WdfRequestForwardToParentDeviceIoQueue(Request, Ep->Queue, &Options);
    if (!NT_SUCCESS(Status))
    {
        UcxLog("device: port %u forward to endpoint 0x%02x queue failed 0x%08lX", Handle->PortNumber,
               Descriptor.bEndpointAddress, (ULONG)Status);
        InterlockedDecrement(&Ep->Transfers);
        Ctx->Endpoint = NULL;
        if (OwnedMdl != NULL)
        {
            IoFreeMdl(OwnedMdl);
            Ctx->OwnedMdl = NULL;
        }
        UcxpPipeClose(Endpoint);
        Req->TransferBufferLength = 0;
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    UcxpPipeClose(Endpoint);
}

/* ABORT_PIPE, SYNC_RESET_PIPE_AND_CLEAR_STALL, SYNC_RESET_PIPE, SYNC_CLEAR_STALL */
static
USBD_STATUS
UcxpDevicePipeRequest(
    _In_ PUCX_DEVICE_HANDLE Handle,
    _In_ PURB Urb)
{
    struct _URB_PIPE_REQUEST *Req = &Urb->UrbPipeRequest;
    USHORT Function = Urb->UrbHeader.Function;
    USB_ENDPOINT_DESCRIPTOR Descriptor;
    UCXENDPOINT Endpoint = NULL;
    PUCX_ENDPOINT_CTX Ep;
    USBD_STATUS UsbdStatus;
    BOOLEAN IsDefault;
    NTSTATUS Status;

    if (Urb->UrbHeader.Length < sizeof(*Req))
        return USBD_STATUS_INVALID_PARAMETER;
    UsbdStatus = UcxpPipeOpen(Handle, Req->PipeHandle, &Endpoint, &Descriptor, &IsDefault);
    if (!USBD_SUCCESS(UsbdStatus))
        return (UsbdStatus == USBD_STATUS_DEVICE_GONE) ? USBD_STATUS_SUCCESS : UsbdStatus;   /* a closing pipe has nothing left */
    if (IsDefault || Endpoint == NULL)
        return USBD_STATUS_SUCCESS;        /* control transfers are synchronous: nothing to abort or reset */

    Ep = UcxGetEndpointCtx((WDFOBJECT)Endpoint);
    if (Function == URB_FUNCTION_ABORT_PIPE)
    {
        if (Ep->Callbacks.EvtEndpointAbort != NULL)
        {
            Status = Ep->Callbacks.EvtEndpointAbort(Endpoint);
            UcxLog("device: port %u pipe 0x%02x aborted -> 0x%08lX", Handle->PortNumber,
                   Descriptor.bEndpointAddress, (ULONG)Status);
        }
        UcxpPipeClose(Endpoint);
        return USBD_STATUS_SUCCESS;
    }

    if (Function == URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL || Function == URB_FUNCTION_SYNC_RESET_PIPE)
    {
        if (Ep->Callbacks.EvtEndpointResetHalted != NULL)
        {
            Status = Ep->Callbacks.EvtEndpointResetHalted(Endpoint);
            if (!NT_SUCCESS(Status))
                UsbdStatus = USBD_STATUS_REQUEST_FAILED;
        }
    }
    if (Function == URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL || Function == URB_FUNCTION_SYNC_CLEAR_STALL)
    {
        USB_DEFAULT_PIPE_SETUP_PACKET Setup;
        USBD_STATUS ClearStatus;
        ULONG Transferred = 0;

        RtlZeroMemory(&Setup, sizeof(Setup));
        Setup.bmRequestType.Dir = BMREQUEST_HOST_TO_DEVICE;
        Setup.bmRequestType.Type = BMREQUEST_STANDARD;
        Setup.bmRequestType.Recipient = BMREQUEST_TO_ENDPOINT;
        Setup.bRequest = USB_REQUEST_CLEAR_FEATURE;
        Setup.wValue.W = USB_FEATURE_ENDPOINT_STALL;
        Setup.wIndex.W = Descriptor.bEndpointAddress;
        Setup.wLength = 0;
        (VOID)UcxpControlTransfer(Handle, &Setup, NULL, NULL, 0, FALSE, &Transferred, &ClearStatus);
        if (!USBD_SUCCESS(ClearStatus))
            UsbdStatus = ClearStatus;
    }
    UcxpPipeClose(Endpoint);
    UcxLog("device: port %u pipe 0x%02x reset (function 0x%04X) -> USBD 0x%08lX", Handle->PortNumber,
           Descriptor.bEndpointAddress, Function, (ULONG)UsbdStatus);
    return UsbdStatus;
}

/* FlushTransfers (hub interface): every pipe of the device gives its transfers back */
VOID
UcxpDeviceFlushTransfers(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _In_ PUCX_DEVICE_HANDLE Handle)
{
    UCXENDPOINT Endpoints[UCX_MAX_PIPES];
    ULONG i, p, n;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(RootHub);
    for (i = 0; i < UCX_MAX_INTERFACES; i++)
    {
        PUCX_INTERFACE Iface;

        /* Reference this interface's endpoints under the lock, abort them outside it */
        n = 0;
        KeAcquireSpinLock(&Handle->PipeLock, &OldIrql);
        Iface = (i < Handle->InterfaceCount) ? Handle->Interfaces[i] : NULL;
        if (Iface != NULL)
        {
            for (p = 0; p < Iface->PipeCount && n < UCX_MAX_PIPES; p++)
            {
                if (Iface->Pipes[p].Endpoint != NULL)
                {
                    Endpoints[n] = Iface->Pipes[p].Endpoint;
                    WdfObjectReferenceWithTag((WDFOBJECT)Endpoints[n], (PVOID)'pPcU');
                    n++;
                }
            }
        }
        KeReleaseSpinLock(&Handle->PipeLock, OldIrql);
        if (Iface == NULL && i >= Handle->InterfaceCount)
            break;
        for (p = 0; p < n; p++)
        {
            PUCX_ENDPOINT_CTX Ep = UcxGetEndpointCtx((WDFOBJECT)Endpoints[p]);

            if (Ep->Callbacks.EvtEndpointAbort != NULL)
                (VOID)Ep->Callbacks.EvtEndpointAbort(Endpoints[p]);
            UcxpPipeClose(Endpoints[p]);
        }
    }
}

VOID
UcxpDeviceHandleUrb(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _In_ PUCX_DEVICE_HANDLE Handle,
    _In_ WDFREQUEST Request,
    _Inout_ PURB Urb)
{
    USB_DEFAULT_PIPE_SETUP_PACKET Setup;
    USBD_STATUS UsbdStatus;
    PVOID Buffer;
    PMDL Mdl;
    ULONG Length, Transferred = 0;
    BOOLEAN DataIn;
    NTSTATUS Status;

    if (!Handle->Initialized)
    {
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_DEVICE_GONE);
        return;
    }

    switch (Urb->UrbHeader.Function)
    {
        case URB_FUNCTION_SELECT_CONFIGURATION:
            UcxpCompleteUrb(Request, Urb, UcxpDeviceSelectConfiguration(RootHub, Handle, Urb));
            return;
        case URB_FUNCTION_SELECT_INTERFACE:
            UcxpCompleteUrb(Request, Urb, UcxpDeviceSelectInterface(RootHub, Handle, Urb));
            return;
        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
            UcxpDeviceBulkTransfer(Handle, Request, Urb);
            return;
        case URB_FUNCTION_ABORT_PIPE:
        case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
        case URB_FUNCTION_SYNC_RESET_PIPE:
        case URB_FUNCTION_SYNC_CLEAR_STALL:
            UcxpCompleteUrb(Request, Urb, UcxpDevicePipeRequest(Handle, Urb));
            return;
        case URB_FUNCTION_ISOCH_TRANSFER:
            /* Isochronous pipes are configured but not driven in M3 */
            UcxLog("device: port %u isochronous transfer not supported", Handle->PortNumber);
            UcxpCompleteUrb(Request, Urb, USBD_STATUS_NOT_SUPPORTED);
            return;
        default:
            break;
    }

    UsbdStatus = UcxpUrbToSetup(Urb, &Setup, &DataIn, &Buffer, &Mdl, &Length);
    if (!USBD_SUCCESS(UsbdStatus))
    {
        UcxLog("device: port %u URB function 0x%04X rejected (USBD 0x%08lX)", Handle->PortNumber,
               Urb->UrbHeader.Function, (ULONG)UsbdStatus);
        UcxpCompleteUrb(Request, Urb, UsbdStatus);
        return;
    }

    /* A control transfer through the client; the pipe (if named) must be the default one */
    if (Urb->UrbHeader.Function == URB_FUNCTION_CONTROL_TRANSFER &&
        Urb->UrbControlTransfer.PipeHandle != NULL &&
        Urb->UrbControlTransfer.PipeHandle != (USBD_PIPE_HANDLE)&Handle->DefaultPipe)
    {
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_INVALID_PIPE_HANDLE);
        return;
    }

    Status = UcxpControlTransfer(Handle, &Setup, Buffer, Mdl, Length, DataIn, &Transferred, &UsbdStatus);

    /* Every control URB layout keeps TransferBufferLength at the same place */
    Urb->UrbControlTransfer.TransferBufferLength = NT_SUCCESS(Status) ? Transferred : 0;
    UcxpCompleteUrb(Request, Urb, UsbdStatus);
}
