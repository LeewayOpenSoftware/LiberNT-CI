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
 *              for. Configured endpoints and bulk/interrupt transfers are step 3.
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

    if (Device != NULL && Device->Callbacks.EvtUsbDeviceDisable != NULL)
        Device->Callbacks.EvtUsbDeviceDisable(RootHub->Controller, UsbDevice);

    UcxLog("device: port %u handle %p removed", Handle->PortNumber, Handle);
    Handle->UsbDevice = NULL;
    if (UsbDevice != NULL)
        WdfObjectDelete((WDFOBJECT)UsbDevice);      /* its endpoints go with it */
    ExFreePoolWithTag(Handle, UCX_TAG);
}

/* USB_DEVICE_INFORMATION_0 for a downstream device (no configured pipes before step 3) */
NTSTATUS
UcxpDeviceQueryInformation(
    _In_ PUCX_DEVICE_HANDLE Handle,
    _Out_writes_bytes_to_(BufferLength, *Returned) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG Returned)
{
    PUSB_DEVICE_INFORMATION_0 Info = Buffer;
    ULONG ActualLength = FIELD_OFFSET(USB_DEVICE_INFORMATION_0, PipeList);

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
    Info->NumberOfOpenPipes = 0;
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

    UNREFERENCED_PARAMETER(RootHub);

    if (!Handle->Initialized)
    {
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_DEVICE_GONE);
        return;
    }

    switch (Urb->UrbHeader.Function)
    {
        case URB_FUNCTION_SELECT_CONFIGURATION:
        case URB_FUNCTION_SELECT_INTERFACE:
        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        case URB_FUNCTION_ISOCH_TRANSFER:
        case URB_FUNCTION_ABORT_PIPE:
        case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
        case URB_FUNCTION_SYNC_RESET_PIPE:
        case URB_FUNCTION_SYNC_CLEAR_STALL:
            /* Configured endpoints arrive with M3 step 3 */
            UcxLog("device: port %u URB function 0x%04X waits for step 3", Handle->PortNumber, Urb->UrbHeader.Function);
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
