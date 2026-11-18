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
    _In_ USHORT PortNumber,
    _In_ USHORT PortStatus,
    _Outptr_ PUCX_DEVICE_HANDLE *NewHandle)
{
    PUCX_CONTROLLER_CTX Controller = UcxGetControllerCtx((WDFOBJECT)RootHub->Controller);
    PUCX_DEVICE_HANDLE Handle;
    struct _UCXUSBDEVICE_INIT *Init;
    UCX_USBDEVICE_INFO Info;
    PUCX_USBDEVICE_CTX DeviceCtx;
    NTSTATUS Status;

    *NewHandle = NULL;

    if (PortNumber == 0 || PortNumber > RootHub->NumberOfPorts)
        return STATUS_INVALID_PARAMETER;
    if (Controller->Config.EvtControllerUsbDeviceAdd == NULL)
        return STATUS_NOT_IMPLEMENTED;

    Handle = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Handle), UCX_TAG);
    if (Handle == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Init = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Init), UCX_TAG);
    if (Init == NULL)
    {
        ExFreePoolWithTag(Handle, UCX_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Handle->IsRootHub = FALSE;
    Handle->PortNumber = PortNumber;
    Handle->DeviceSpeed = UsbFullSpeed;    /* the client reports the real one at Enable */
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

    UcxpAddDeviceHandle(Controller, Handle);
    *NewHandle = Handle;
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
    if (Device->Callbacks.EvtUsbDeviceEnable == NULL || Device->Callbacks.EvtUsbDeviceAddress == NULL ||
        Device->Callbacks.EvtUsbDeviceDefaultEndpointAdd == NULL)
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    /* 1. A slot for the port the hub reset */
    RtlZeroMemory(&Enable, sizeof(Enable));
    Enable.Size = sizeof(Enable);
    Enable.PortNumber = Handle->PortNumber;
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

    Handle->Initialized = TRUE;
    UcxLog("device: port %u addressed %u, speed %u, EP0 %lu, %04X:%04X class %02X/%02X/%02X, %u configuration(s)",
           Handle->PortNumber, Handle->DeviceAddress, Handle->DeviceSpeed, Handle->MaxPacketSize0,
           Handle->DeviceDescriptor.idVendor, Handle->DeviceDescriptor.idProduct,
           Handle->DeviceDescriptor.bDeviceClass, Handle->DeviceDescriptor.bDeviceSubClass,
           Handle->DeviceDescriptor.bDeviceProtocol, Handle->DeviceDescriptor.bNumConfigurations);
    return STATUS_SUCCESS;

Fail:
    /* Leave the hub driver a handle it can remove; the slot goes back now */
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

    Length = min(*DeviceLength, (ULONG)sizeof(USB_DEVICE_DESCRIPTOR));
    if (Length != 0 && DeviceBuffer != NULL)
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

    /* Then as much as the caller can take, never more than one transfer's worth */
    Length = min(*ConfigLength, (ULONG)Header.wTotalLength);
    Length = min(Length, (ULONG)UCX_CONTROL_MAX_TRANSFER);
    Setup.wLength = (USHORT)Length;
    RtlZeroMemory(ConfigBuffer, Length);
    Status = UcxpControlTransfer(Handle, &Setup, ConfigBuffer, NULL, Length, TRUE, &Transferred, &UsbdStatus);
    if (!NT_SUCCESS(Status))
    {
        UcxLog("device: port %u configuration read (%lu bytes) failed 0x%08lX", Handle->PortNumber, Length, (ULONG)Status);
        *ConfigLength = 0;
        return Status;
    }
    *ConfigLength = Transferred;
    return STATUS_SUCCESS;
}

VOID
UcxpDeviceRemove(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle)
{
    PUCX_CONTROLLER_CTX Controller = UcxGetControllerCtx((WDFOBJECT)RootHub->Controller);
    PUCX_USBDEVICE_CTX Device = UcxpDeviceCtx(Handle);
    UCXUSBDEVICE UsbDevice = Handle->UsbDevice;

    /* Out of the list first: no URB finds it from here on */
    UcxpRemoveDeviceHandle(Controller, Handle);

    /* Its pipes give their transfers back and the endpoints leave the controller */
    UcxpDeviceUnconfigure(RootHub, Handle, FALSE);

    if (Device != NULL && Device->Callbacks.EvtUsbDeviceDisable != NULL)
        Device->Callbacks.EvtUsbDeviceDisable(RootHub->Controller, UsbDevice);

    UcxLog("device: port %u handle %p removed", Handle->PortNumber, Handle);
    Handle->UsbDevice = NULL;
    if (UsbDevice != NULL)
        WdfObjectDelete((WDFOBJECT)UsbDevice);      /* its endpoints go with it */
    ExFreePoolWithTag(Handle, UCX_TAG);
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

    for (i = 0; i < Handle->InterfaceCount; i++)
        if (Handle->Interfaces[i] != NULL)
            Pipes += Handle->Interfaces[i]->PipeCount;
    ActualLength = FIELD_OFFSET(USB_DEVICE_INFORMATION_0, PipeList) + Pipes * sizeof(USB_PIPE_INFORMATION_0);

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
    Info->HubAddress = 0;
    Info->DeviceSpeed = Handle->DeviceSpeed;
    Info->DeviceType = (Handle->DeviceSpeed == UsbLowSpeed || Handle->DeviceSpeed == UsbFullSpeed) ?
                       Usb11Device : Usb20Device;
    Info->NumberOfOpenPipes = Pipes;
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
BOOLEAN
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
                return FALSE;
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
                return FALSE;
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
                return FALSE;
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
                return FALSE;
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
                return FALSE;
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
                return FALSE;
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
                return FALSE;
            RtlCopyMemory(Setup, Req->SetupPacket, sizeof(*Setup));
            *DataIn = (Req->TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
            *Buffer = Req->TransferBuffer;
            *Mdl = Req->TransferBufferMDL;
            *Length = Req->TransferBufferLength;
            break;
        }

        default:
            return FALSE;
    }

    Setup->wLength = (USHORT)min(*Length, (ULONG)MAXUSHORT);
    return TRUE;
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

static
PUCX_PIPE_HANDLE
UcxpFindPipe(
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

/* Abort the transfers of every pipe of an interface (client callback), then
 * refuse new ones */
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

        InterlockedExchange(&Pipe->Closing, 1);
        if (Pipe->Endpoint == NULL)
            continue;
        Ep = UcxGetEndpointCtx((WDFOBJECT)Pipe->Endpoint);
        if (Ep->Callbacks.EvtEndpointAbort != NULL)
            Ep->Callbacks.EvtEndpointAbort(Pipe->Endpoint);
    }
}

/* Delete the endpoint objects of an interface and free it (after they were
 * disabled on the controller) */
static
VOID
UcxpInterfaceDestroy(
    _Inout_ PUCX_INTERFACE Iface)
{
    ULONG p;

    for (p = 0; p < Iface->PipeCount; p++)
    {
        UCXENDPOINT Endpoint = Iface->Pipes[p].Endpoint;

        Iface->Pipes[p].Endpoint = NULL;
        Iface->Pipes[p].Signature = 0;
        if (Endpoint != NULL)
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
            UcxpInterfaceDestroy(Iface);
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
            UcxpInterfaceDestroy(Iface);
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
            UcxpInterfaceDestroy(Iface);
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
    ULONG Count = 0, i;

    for (i = 0; i < Handle->InterfaceCount; i++)
    {
        if (Handle->Interfaces[i] != NULL)
        {
            UcxpInterfaceAbortPipes(Handle->Interfaces[i]);
            Ifaces[Count++] = Handle->Interfaces[i];
        }
    }
    if (Count != 0 || Handle->ConfigurationValue != 0)
        (VOID)UcxpConfigureEndpoints(RootHub, Handle, UcxEndpointsConfigureDisableAll, 0, 0, 0,
                                     NULL, 0, Ifaces, Count);
    if (TellDevice && Handle->ConfigurationValue != 0)
        (VOID)UcxpDeviceSetConfigurationOrInterface(Handle, FALSE, 0, 0);

    for (i = 0; i < Count; i++)
        UcxpInterfaceDestroy(Ifaces[i]);
    RtlZeroMemory(Handle->Interfaces, sizeof(Handle->Interfaces));
    Handle->InterfaceCount = 0;
    Handle->ConfigurationValue = 0;
    if (Handle->ConfigDescriptor != NULL)
    {
        ExFreePoolWithTag(Handle->ConfigDescriptor, UCX_TAG);
        Handle->ConfigDescriptor = NULL;
        Handle->ConfigDescriptorLength = 0;
    }
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
    ULONG BuiltCount = 0, Remaining, TotalLength, Pipes = 0, i;
    USBD_STATUS UsbdStatus;
    NTSTATUS Status;

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
        Pipes += Built[BuiltCount]->PipeCount;
        BuiltCount++;
        Remaining -= Info->Length;
        Info = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)Info + Info->Length);
    }

    Status = UcxpConfigureEndpoints(RootHub, Handle, UcxEndpointsConfigureConfiguration,
                                    Config->bConfigurationValue, 0, 0, Built, BuiltCount, NULL, 0);
    if (!NT_SUCCESS(Status))
    {
        UsbdStatus = USBD_STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }
    for (i = 0; i < BuiltCount; i++)
        Handle->Interfaces[i] = Built[i];
    Handle->InterfaceCount = BuiltCount;

    UsbdStatus = UcxpDeviceSetConfigurationOrInterface(Handle, FALSE, Config->bConfigurationValue, 0);
    if (!USBD_SUCCESS(UsbdStatus))
    {
        UcxLog("device: port %u SET_CONFIGURATION(%u) failed, USBD 0x%08lX", Handle->PortNumber,
               Config->bConfigurationValue, (ULONG)UsbdStatus);
        Handle->ConfigurationValue = Config->bConfigurationValue;   /* so the unconfigure drops the contexts */
        UcxpDeviceUnconfigure(RootHub, Handle, FALSE);
        return UsbdStatus;
    }

    Handle->ConfigurationValue = Config->bConfigurationValue;
    Req->ConfigurationHandle = (USBD_CONFIGURATION_HANDLE)&Handle->ConfigurationCookie;
    UcxLog("device: port %u configured %u: %lu interface(s), %lu pipe(s)", Handle->PortNumber,
           Handle->ConfigurationValue, BuiltCount, Pipes);
    return USBD_STATUS_SUCCESS;

Fail:
    for (i = 0; i < BuiltCount; i++)
        UcxpInterfaceDestroy(Built[i]);
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

    Remaining = Urb->UrbHeader.Length - FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface);
    UsbdStatus = UcxpInterfaceBuild(RootHub, Handle, Handle->ConfigDescriptor, Handle->ConfigDescriptorLength,
                                    Info, Remaining, &New);
    if (!USBD_SUCCESS(UsbdStatus))
        return UsbdStatus;

    /* The old setting's pipes are done; the controller swaps the endpoints in one command */
    UcxpInterfaceAbortPipes(Old);
    Status = UcxpConfigureEndpoints(RootHub, Handle, UcxEndpointsConfigureInterface, Handle->ConfigurationValue,
                                    New->InterfaceNumber, New->AlternateSetting, &New, 1, &Old, 1);
    if (!NT_SUCCESS(Status))
    {
        /* The old endpoints are gone from the controller either way; the
         * interface keeps no usable pipes until the class driver selects again */
        UcxpInterfaceDestroy(New);
        UcxpInterfaceDestroy(Old);
        Handle->Interfaces[Index] = NULL;
        return USBD_STATUS_INSUFFICIENT_RESOURCES;
    }
    Handle->Interfaces[Index] = New;
    UcxpInterfaceDestroy(Old);

    UsbdStatus = UcxpDeviceSetConfigurationOrInterface(Handle, TRUE, New->AlternateSetting, New->InterfaceNumber);
    UcxLog("device: port %u interface %u -> alternate setting %u, %u pipe(s), USBD 0x%08lX", Handle->PortNumber,
           New->InterfaceNumber, New->AlternateSetting, New->PipeCount, (ULONG)UsbdStatus);
    return UsbdStatus;
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
 * completed with an error.
 */
static
VOID
UcxpDeviceBulkTransfer(
    _In_ PUCX_DEVICE_HANDLE Handle,
    _In_ WDFREQUEST Request,
    _Inout_ PURB Urb)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *Req = &Urb->UrbBulkOrInterruptTransfer;
    PUCX_PIPE_HANDLE Pipe;
    PUCX_ENDPOINT_CTX Ep;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_REQUEST_FORWARD_OPTIONS Options;
    PUCX_REQUEST_CTX Ctx = NULL;
    PMDL OwnedMdl = NULL;
    UCHAR Type;
    NTSTATUS Status;

    if (Urb->UrbHeader.Length < sizeof(*Req))
    {
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_INVALID_PARAMETER);
        return;
    }
    Pipe = UcxpFindPipe(Handle, Req->PipeHandle);
    if (Pipe == NULL || Pipe == &Handle->DefaultPipe || Pipe->Endpoint == NULL)
    {
        Req->TransferBufferLength = 0;
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_INVALID_PIPE_HANDLE);
        return;
    }
    Type = Pipe->Descriptor.bmAttributes & USB_ENDPOINT_TYPE_MASK;
    if (Type != USB_ENDPOINT_TYPE_BULK && Type != USB_ENDPOINT_TYPE_INTERRUPT)
    {
        Req->TransferBufferLength = 0;
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_INVALID_PIPE_HANDLE);
        return;
    }
    if (InterlockedCompareExchange(&Pipe->Closing, 0, 0) != 0)
    {
        Req->TransferBufferLength = 0;
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_DEVICE_GONE);
        return;
    }
    Ep = UcxGetEndpointCtx((WDFOBJECT)Pipe->Endpoint);
    if (Ep->Queue == NULL)
    {
        Req->TransferBufferLength = 0;
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_NOT_SUPPORTED);
        return;
    }

    /* The endpoint's direction is the transfer's (usbport does the same) */
    if (USB_ENDPOINT_DIRECTION_IN(Pipe->Descriptor.bEndpointAddress))
        Req->TransferFlags |= USBD_TRANSFER_DIRECTION_IN;
    else
        Req->TransferFlags &= ~USBD_TRANSFER_DIRECTION_IN;

    if (Req->TransferBufferLength > UCX_BULK_MAX_TRANSFER)
    {
        Req->TransferBufferLength = 0;
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_INVALID_PARAMETER);
        return;
    }
    if (Req->TransferBufferLength != 0)
    {
        if (Req->TransferBufferMDL != NULL)
        {
            PMDL Mdl = Req->TransferBufferMDL;

            if (MmGetMdlByteCount(Mdl) < Req->TransferBufferLength ||
                (Mdl->MdlFlags & (MDL_PAGES_LOCKED | MDL_SOURCE_IS_NONPAGED_POOL)) == 0)
            {
                Req->TransferBufferLength = 0;
                UcxpCompleteUrb(Request, Urb, USBD_STATUS_INVALID_PARAMETER);
                return;
            }
        }
        else if (Req->TransferBuffer != NULL)
        {
            OwnedMdl = IoAllocateMdl(Req->TransferBuffer, Req->TransferBufferLength, FALSE, FALSE, NULL);
            if (OwnedMdl == NULL)
            {
                Req->TransferBufferLength = 0;
                UcxpCompleteUrb(Request, Urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
                return;
            }
            MmBuildMdlForNonPagedPool(OwnedMdl);
            Req->TransferBufferMDL = OwnedMdl;
        }
        else
        {
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
        {
            Req->TransferBufferMDL = NULL;
            IoFreeMdl(OwnedMdl);
        }
        Req->TransferBufferLength = 0;
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    Ctx->Endpoint = Pipe->Endpoint;
    Ctx->Pipe = Pipe;
    Ctx->OwnedMdl = OwnedMdl;
    InterlockedIncrement(&Ep->Transfers);

    /* The queue belongs to the client's FDO, the request to our PDO: KMDF
     * allows exactly this hop (WdfPdoInitAllowForwardingRequestToParent) */
    WDF_REQUEST_FORWARD_OPTIONS_INIT(&Options);
    Status = WdfRequestForwardToParentDeviceIoQueue(Request, Ep->Queue, &Options);
    if (!NT_SUCCESS(Status))
    {
        UcxLog("device: port %u forward to endpoint 0x%02x queue failed 0x%08lX", Handle->PortNumber,
               Pipe->Descriptor.bEndpointAddress, (ULONG)Status);
        InterlockedDecrement(&Ep->Transfers);
        Ctx->Endpoint = NULL;
        if (OwnedMdl != NULL)
        {
            Req->TransferBufferMDL = NULL;
            IoFreeMdl(OwnedMdl);
            Ctx->OwnedMdl = NULL;
        }
        Req->TransferBufferLength = 0;
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
    }
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
    PUCX_PIPE_HANDLE Pipe;
    PUCX_ENDPOINT_CTX Ep;
    USBD_STATUS UsbdStatus = USBD_STATUS_SUCCESS;
    NTSTATUS Status;

    if (Urb->UrbHeader.Length < sizeof(*Req))
        return USBD_STATUS_INVALID_PARAMETER;
    Pipe = UcxpFindPipe(Handle, Req->PipeHandle);
    if (Pipe == NULL)
        return USBD_STATUS_INVALID_PIPE_HANDLE;
    if (Pipe == &Handle->DefaultPipe || Pipe->Endpoint == NULL)
        return USBD_STATUS_SUCCESS;        /* control transfers are synchronous: nothing to abort or reset */

    Ep = UcxGetEndpointCtx((WDFOBJECT)Pipe->Endpoint);
    if (Function == URB_FUNCTION_ABORT_PIPE)
    {
        if (Ep->Callbacks.EvtEndpointAbort != NULL)
        {
            Status = Ep->Callbacks.EvtEndpointAbort(Pipe->Endpoint);
            UcxLog("device: port %u pipe 0x%02x aborted -> 0x%08lX", Handle->PortNumber,
                   Pipe->Descriptor.bEndpointAddress, (ULONG)Status);
        }
        return USBD_STATUS_SUCCESS;
    }

    if (Function == URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL || Function == URB_FUNCTION_SYNC_RESET_PIPE)
    {
        if (Ep->Callbacks.EvtEndpointResetHalted != NULL)
        {
            Status = Ep->Callbacks.EvtEndpointResetHalted(Pipe->Endpoint);
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
        Setup.wIndex.W = Pipe->Descriptor.bEndpointAddress;
        Setup.wLength = 0;
        (VOID)UcxpControlTransfer(Handle, &Setup, NULL, NULL, 0, FALSE, &Transferred, &ClearStatus);
        if (!USBD_SUCCESS(ClearStatus))
            UsbdStatus = ClearStatus;
    }
    UcxLog("device: port %u pipe 0x%02x reset (function 0x%04X) -> USBD 0x%08lX", Handle->PortNumber,
           Pipe->Descriptor.bEndpointAddress, Function, (ULONG)UsbdStatus);
    return UsbdStatus;
}

/* FlushTransfers (hub interface): every pipe of the device gives its transfers back */
VOID
UcxpDeviceFlushTransfers(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _In_ PUCX_DEVICE_HANDLE Handle)
{
    ULONG i;

    UNREFERENCED_PARAMETER(RootHub);
    for (i = 0; i < Handle->InterfaceCount; i++)
    {
        PUCX_INTERFACE Iface = Handle->Interfaces[i];
        ULONG p;

        if (Iface == NULL)
            continue;
        for (p = 0; p < Iface->PipeCount; p++)
        {
            PUCX_PIPE_HANDLE Pipe = &Iface->Pipes[p];
            PUCX_ENDPOINT_CTX Ep;

            if (Pipe->Endpoint == NULL)
                continue;
            Ep = UcxGetEndpointCtx((WDFOBJECT)Pipe->Endpoint);
            if (Ep->Callbacks.EvtEndpointAbort != NULL)
                (VOID)Ep->Callbacks.EvtEndpointAbort(Pipe->Endpoint);
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

    if (!UcxpUrbToSetup(Urb, &Setup, &DataIn, &Buffer, &Mdl, &Length))
    {
        UcxLog("device: port %u URB function 0x%04X rejected", Handle->PortNumber, Urb->UrbHeader.Function);
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_INVALID_URB_FUNCTION);
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
