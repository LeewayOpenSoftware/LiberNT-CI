/*
 * PROJECT:     LiberNT Kernel-Mode Driver Framework
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     USB host Controller eXtension (UCX) class library -- the root hub PDO.
 *              UcxRootHubCreate makes the root hub a child device of the client's
 *              host controller FDO (USB\ROOT_HUB30). The hub driver binds to it
 *              unmodified: it finds the same internal IOCTLs, the same two bus
 *              interfaces (USB_BUS_INTERFACE_HUB_V5, USB_BUS_INTERFACE_USBDI_V2)
 *              and the same URB vocabulary a usbport root hub answers. Standard
 *              requests are answered here from synthesized descriptors; hub class
 *              requests go to the client (EvtRootHubControlUrb), which owns the
 *              port registers; status-change transfers are pended here and
 *              completed with the client's change bitmap on UcxRootHubPortChanged.
 * NOTE:        Every pointer the hub driver hands back (device handle, pipe
 *              handle, configuration handle) is matched against what this
 *              library issued before it is dereferenced. M3 step 1: root hub
 *              only; downstream device handles arrive in step 2.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

/* This translation unit defines the bus interface GUIDs the PDO exposes */
#include <initguid.h>
#include "ucxp.h"

#define UCX_ROOTHUB_LANGUAGE            0x0409
#define UCX_ROOTHUB_SCE_INTERVAL        0x0C      /* as the usbport root hub reports */
#define UCX_ROOTHUB_MAX_TRANSFER        4096
#define UCX_ROOTHUB_HUB_DESCRIPTOR_20   USB_20_HUB_DESCRIPTOR_TYPE
#define UCX_ROOTHUB_HUB_DESCRIPTOR_30   USB_30_HUB_DESCRIPTOR_TYPE

static const WCHAR UcxRootHubControllerName[] = L"USB xHCI Compliant Host Controller (UCX)";

static EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL UcxpRootHubEvtInternalIoctl;
static EVT_WDF_WORKITEM UcxpRootHubEvtInitNotifyWorkItem;

/* --------------------------------------------------------------------------
 * Small helpers
 * -------------------------------------------------------------------------- */

static
PUCX_ROOTHUB_CTX
UcxpRootHubFromBusContext(
    _In_opt_ PVOID BusContext)
{
    /* BusContext is the pointer this library placed in the interface it
     * exposed; the hub driver hands it back unchanged. */
    return (PUCX_ROOTHUB_CTX)BusContext;
}

static
PUCX_CONTROLLER_CTX
UcxpRootHubController(
    _In_ PUCX_ROOTHUB_CTX RootHub)
{
    return UcxGetControllerCtx((WDFOBJECT)RootHub->Controller);
}

NTSTATUS
UcxpUsbdToNtStatus(
    _In_ USBD_STATUS UsbdStatus)
{
    switch (UsbdStatus)
    {
        case USBD_STATUS_SUCCESS:            return STATUS_SUCCESS;
        case USBD_STATUS_INVALID_PARAMETER:  return STATUS_INVALID_PARAMETER;
        case USBD_STATUS_INVALID_URB_FUNCTION:
        case USBD_STATUS_NOT_SUPPORTED:      return STATUS_NOT_SUPPORTED;
        case USBD_STATUS_INVALID_PIPE_HANDLE: return STATUS_INVALID_PARAMETER;
        case USBD_STATUS_DEVICE_GONE:        return STATUS_DEVICE_NOT_CONNECTED;
        case USBD_STATUS_CANCELED:           return STATUS_CANCELLED;
        case USBD_STATUS_BUFFER_TOO_SMALL:   return STATUS_BUFFER_TOO_SMALL;
        case USBD_STATUS_TIMEOUT:            return STATUS_IO_TIMEOUT;
        default:                             return STATUS_UNSUCCESSFUL;
    }
}

VOID
UcxpCompleteUrb(
    _In_ WDFREQUEST Request,
    _Inout_ PURB Urb,
    _In_ USBD_STATUS UsbdStatus)
{
    Urb->UrbHeader.Status = UsbdStatus;
    WdfRequestComplete(Request, UcxpUsbdToNtStatus(UsbdStatus));
}

/* Copies at most *TransferLength bytes of Source and reports what was copied */
static
USBD_STATUS
UcxpCopyToTransferBuffer(
    _In_opt_ PVOID TransferBuffer,
    _In_opt_ PMDL TransferMdl,
    _Inout_ PULONG TransferLength,
    _In_reads_bytes_(SourceLength) const VOID *Source,
    _In_ ULONG SourceLength)
{
    PVOID Destination;
    ULONG Length = min(*TransferLength, SourceLength);

    if (Length == 0)
    {
        *TransferLength = 0;
        return USBD_STATUS_SUCCESS;
    }

    Destination = UcxUrbTransferBuffer(TransferBuffer, TransferMdl, Length);
    if (Destination == NULL)
        return USBD_STATUS_INVALID_PARAMETER;

    RtlCopyMemory(Destination, Source, Length);
    *TransferLength = Length;
    return USBD_STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Descriptors: the root hub as the hub driver sees it
 * -------------------------------------------------------------------------- */

static
VOID
UcxpRootHubBuildDescriptors(
    _Inout_ PUCX_ROOTHUB_CTX RootHub)
{
    PUCX_CONTROLLER_CTX Controller = UcxpRootHubController(RootHub);
    PUSB_DEVICE_DESCRIPTOR Dev = &RootHub->DeviceDescriptor;
    PUCX_ROOTHUB_CONFIG_BLOCK Cfg = &RootHub->ConfigBlock;
    BOOLEAN SuperSpeed = (RootHub->Config.NumberOf30Ports != 0);
    USHORT BitmapBytes;

    RtlZeroMemory(Dev, sizeof(*Dev));
    Dev->bLength = sizeof(USB_DEVICE_DESCRIPTOR);
    Dev->bDescriptorType = USB_DEVICE_DESCRIPTOR_TYPE;
    Dev->bcdUSB = SuperSpeed ? 0x0300 : 0x0200;
    Dev->bDeviceClass = USB_DEVICE_CLASS_HUB;
    Dev->bDeviceSubClass = 0;
    Dev->bDeviceProtocol = SuperSpeed ? 3 : 1;          /* SS hub / single-TT HS hub */
    Dev->bMaxPacketSize0 = SuperSpeed ? 9 : 64;
    Dev->idVendor = Controller->Config.VendorId;
    Dev->idProduct = Controller->Config.DeviceId;
    Dev->bcdDevice = Controller->Config.RevisionId;
    Dev->bNumConfigurations = 1;

    RtlZeroMemory(Cfg, sizeof(*Cfg));
    Cfg->Configuration.bLength = sizeof(USB_CONFIGURATION_DESCRIPTOR);
    Cfg->Configuration.bDescriptorType = USB_CONFIGURATION_DESCRIPTOR_TYPE;
    Cfg->Configuration.wTotalLength = sizeof(UCX_ROOTHUB_CONFIG_BLOCK);
    Cfg->Configuration.bNumInterfaces = 1;
    Cfg->Configuration.bConfigurationValue = 1;
    Cfg->Configuration.bmAttributes = USB_CONFIG_BUS_POWERED | USB_CONFIG_SELF_POWERED;
    Cfg->Configuration.MaxPower = 0;

    Cfg->Interface.bLength = sizeof(USB_INTERFACE_DESCRIPTOR);
    Cfg->Interface.bDescriptorType = USB_INTERFACE_DESCRIPTOR_TYPE;
    Cfg->Interface.bInterfaceNumber = 0;
    Cfg->Interface.bAlternateSetting = 0;
    Cfg->Interface.bNumEndpoints = 1;
    Cfg->Interface.bInterfaceClass = USB_DEVICE_CLASS_HUB;
    Cfg->Interface.bInterfaceSubClass = 0;
    Cfg->Interface.bInterfaceProtocol = 0;

    /* One bit per port plus bit 0 for the hub itself; never below the 2 bytes
     * the hub driver sizes its bitmap from */
    BitmapBytes = (USHORT)((RootHub->NumberOfPorts + 1 + 7) / 8);
    if (BitmapBytes < 2)
        BitmapBytes = 2;

    Cfg->StatusChangeEndpoint.bLength = sizeof(USB_ENDPOINT_DESCRIPTOR);
    Cfg->StatusChangeEndpoint.bDescriptorType = USB_ENDPOINT_DESCRIPTOR_TYPE;
    Cfg->StatusChangeEndpoint.bEndpointAddress = 0x81;
    Cfg->StatusChangeEndpoint.bmAttributes = USB_ENDPOINT_TYPE_INTERRUPT;
    Cfg->StatusChangeEndpoint.wMaxPacketSize = BitmapBytes;
    Cfg->StatusChangeEndpoint.bInterval = UCX_ROOTHUB_SCE_INTERVAL;

    RtlZeroMemory(&RootHub->ScePipe, sizeof(RootHub->ScePipe));
    RootHub->ScePipe.Signature = UCX_PIPE_HANDLE_SIGNATURE;
    RootHub->ScePipe.Endpoint = NULL;
    RootHub->ScePipe.Descriptor = Cfg->StatusChangeEndpoint;
    RootHub->ScePipe.MaximumTransferSize = UCX_ROOTHUB_MAX_TRANSFER;

    RtlZeroMemory(&RootHub->DeviceHandle, sizeof(RootHub->DeviceHandle));
    RootHub->DeviceHandle.Signature = UCX_DEVICE_HANDLE_SIGNATURE;
    RootHub->DeviceHandle.IsRootHub = TRUE;
    RootHub->DeviceHandle.UsbDevice = NULL;
    RootHub->DeviceHandle.DeviceAddress = 0;
    RootHub->DeviceHandle.DeviceSpeed = SuperSpeed ? UsbSuperSpeed : UsbHighSpeed;
    RootHub->DeviceHandle.DeviceDescriptor = *Dev;
    RootHub->DeviceHandle.ConfigurationValue = 0;
    /* DefaultPipe stays zero: the root hub's standard requests never reach an endpoint */
}

/* --------------------------------------------------------------------------
 * Status-change (interrupt IN) pipe
 * -------------------------------------------------------------------------- */

/*
 * Completes one pended status-change transfer with the client's bitmap. Runs
 * at <= DISPATCH_LEVEL from the IOCTL path (Poll = TRUE: a transfer was just
 * submitted, read the live port registers whatever the flag says) and from
 * UcxRootHubPortChanged (Poll = FALSE: only when a change was flagged).
 *
 * The flag is consumed BEFORE the client reads the hardware, so a change that
 * lands during the read sets it again; after a "no change" answer the request
 * is requeued and the flag re-tested, so a change flagged while the request
 * was out of the queue is still delivered. The client's bitmap is built from
 * the live PORTSC change bits, which the hub driver clears one by one, so a
 * late poll always sees what is still pending.
 */
static
VOID
UcxpRootHubDeliverChange(
    _Inout_ PUCX_ROOTHUB_CTX RootHub,
    _In_ BOOLEAN Poll)
{
    WDFQUEUE Queue = RootHub->SceQueue;    /* one read: Destroy clears the field */
    WDFREQUEST Request;
    PURB Urb;
    PVOID Buffer;
    ULONG Length;
    BOOLEAN Any;
    NTSTATUS Status;

    if (Queue == NULL)
        return;

    for (;;)
    {
        if (!Poll && InterlockedCompareExchange(&RootHub->PortChangePending, 0, 0) == 0)
            return;

        Status = WdfIoQueueRetrieveNextRequest(Queue, &Request);
        if (!NT_SUCCESS(Status))
            return;                        /* nobody is listening; the flag stays for the next submit */

        /* Consume the notification first; a change during the read re-arms it */
        InterlockedExchange(&RootHub->PortChangePending, 0);

        Urb = (PURB)UcxRequestGetUrb(Request);
        Length = Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
        Buffer = UcxUrbTransferBuffer(Urb->UrbBulkOrInterruptTransfer.TransferBuffer,
                                      Urb->UrbBulkOrInterruptTransfer.TransferBufferMDL,
                                      Length);
        if (Buffer == NULL)
        {
            UcxpCompleteUrb(Request, Urb, USBD_STATUS_INVALID_PARAMETER);
            return;
        }

        RtlZeroMemory(Buffer, Length);
        Any = FALSE;
        if (RootHub->Config.EvtRootHubGetPortChangeBitmap != NULL)
            Any = RootHub->Config.EvtRootHubGetPortChangeBitmap((UCXROOTHUB)WdfObjectContextGetObject(RootHub),
                                                                (PUCHAR)Buffer, Length);
        if (Any)
        {
            UcxLog("roothub: status change delivered (%lu bytes)", Length);
            UcxpCompleteUrb(Request, Urb, USBD_STATUS_SUCCESS);
            return;
        }

        /* No change bit set: keep waiting, unless a change was flagged while
         * the request was out of the queue, in which case go round again */
        Status = WdfRequestForwardToIoQueue(Request, Queue);
        if (!NT_SUCCESS(Status))
        {
            UcxpCompleteUrb(Request, Urb, USBD_STATUS_CANCELED);
            return;
        }
        Poll = FALSE;
    }
}

/* Completes every pended status-change transfer as canceled (ABORT_PIPE, unconfigure, teardown) */
static
VOID
UcxpRootHubAbortSce(
    _Inout_ PUCX_ROOTHUB_CTX RootHub)
{
    WDFQUEUE Queue = RootHub->SceQueue;
    WDFREQUEST Request;

    if (Queue == NULL)
        return;

    while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Queue, &Request)))
    {
        PURB Urb = (PURB)UcxRequestGetUrb(Request);
        Urb->UrbBulkOrInterruptTransfer.TransferBufferLength = 0;
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_CANCELED);
    }
}

VOID
UcxpRootHubPortChanged(
    _In_ UCXROOTHUB RootHubHandle)
{
    PUCX_ROOTHUB_CTX RootHub = UcxGetRootHubCtx((WDFOBJECT)RootHubHandle);

    if (RootHub->Pdo == NULL)
        return;

    InterlockedExchange(&RootHub->PortChangePending, 1);
    UcxpRootHubDeliverChange(RootHub, FALSE);
}

/* --------------------------------------------------------------------------
 * URBs addressed to the root hub device
 * -------------------------------------------------------------------------- */

static
VOID
UcxpRootHubFillInterface(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUSBD_INTERFACE_INFORMATION Interface)
{
    PUSBD_PIPE_INFORMATION Pipe = &Interface->Pipes[0];

    Interface->InterfaceNumber = 0;
    Interface->AlternateSetting = 0;
    Interface->Class = USB_DEVICE_CLASS_HUB;
    Interface->SubClass = 0;
    Interface->Protocol = 0;
    Interface->Reserved = 0;
    Interface->InterfaceHandle = (USBD_INTERFACE_HANDLE)&RootHub->ConfigBlock.Interface;
    Interface->NumberOfPipes = 1;

    Pipe->MaximumPacketSize = RootHub->ScePipe.Descriptor.wMaxPacketSize;
    Pipe->EndpointAddress = RootHub->ScePipe.Descriptor.bEndpointAddress;
    Pipe->Interval = RootHub->ScePipe.Descriptor.bInterval;
    Pipe->PipeType = UsbdPipeTypeInterrupt;
    Pipe->PipeHandle = (USBD_PIPE_HANDLE)&RootHub->ScePipe;
    if (Pipe->MaximumTransferSize == 0 || Pipe->MaximumTransferSize > UCX_ROOTHUB_MAX_TRANSFER)
        Pipe->MaximumTransferSize = UCX_ROOTHUB_MAX_TRANSFER;
    Pipe->PipeFlags = 0;
}

/* SELECT_CONFIGURATION: the hub driver's URB carries one USBD_INTERFACE_INFORMATION */
static
USBD_STATUS
UcxpRootHubSelectConfiguration(
    _Inout_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PURB Urb)
{
    struct _URB_SELECT_CONFIGURATION *Select = &Urb->UrbSelectConfiguration;
    PUSBD_INTERFACE_INFORMATION Interface = &Select->Interface;
    ULONG HeaderLength = FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface);

    if (Select->ConfigurationDescriptor == NULL)
    {
        /* Unconfigure: the status-change pipe is gone */
        UcxpRootHubAbortSce(RootHub);
        RootHub->DeviceHandle.ConfigurationValue = 0;
        Select->ConfigurationHandle = NULL;
        return USBD_STATUS_SUCCESS;
    }

    /* The hub driver selects the configuration it read from us: the only one */
    if (Select->ConfigurationDescriptor->bLength < sizeof(USB_CONFIGURATION_DESCRIPTOR) ||
        Select->ConfigurationDescriptor->bConfigurationValue !=
            RootHub->ConfigBlock.Configuration.bConfigurationValue)
    {
        return USBD_STATUS_INVALID_CONFIGURATION_DESCRIPTOR;
    }

    /* The interface block must fit inside the URB the caller sized */
    if (Urb->UrbHeader.Length < HeaderLength + GET_USBD_INTERFACE_SIZE(1) ||
        Interface->Length < GET_USBD_INTERFACE_SIZE(1) ||
        Urb->UrbHeader.Length < HeaderLength + Interface->Length)
    {
        return USBD_STATUS_INVALID_CONFIGURATION_DESCRIPTOR;
    }

    if (Interface->InterfaceNumber != 0 || Interface->AlternateSetting != 0)
        return USBD_STATUS_INVALID_PARAMETER;

    UcxpRootHubFillInterface(RootHub, Interface);
    Select->ConfigurationHandle = (USBD_CONFIGURATION_HANDLE)&RootHub->ConfigurationHandleCookie;
    RootHub->DeviceHandle.ConfigurationValue = RootHub->ConfigBlock.Configuration.bConfigurationValue;
    return USBD_STATUS_SUCCESS;
}

static
USBD_STATUS
UcxpRootHubSelectInterface(
    _Inout_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PURB Urb)
{
    struct _URB_SELECT_INTERFACE *Select = &Urb->UrbSelectInterface;
    PUSBD_INTERFACE_INFORMATION Interface = &Select->Interface;
    ULONG HeaderLength = FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface);

    if (Select->ConfigurationHandle != (USBD_CONFIGURATION_HANDLE)&RootHub->ConfigurationHandleCookie ||
        RootHub->DeviceHandle.ConfigurationValue == 0)
    {
        return USBD_STATUS_INVALID_PARAMETER;
    }

    if (Urb->UrbHeader.Length < HeaderLength + GET_USBD_INTERFACE_SIZE(1) ||
        Interface->Length < GET_USBD_INTERFACE_SIZE(1) ||
        Urb->UrbHeader.Length < HeaderLength + Interface->Length)
    {
        return USBD_STATUS_INVALID_PARAMETER;
    }

    if (Interface->InterfaceNumber != 0 || Interface->AlternateSetting != 0)
        return USBD_STATUS_INVALID_PARAMETER;

    UcxpRootHubFillInterface(RootHub, Interface);
    return USBD_STATUS_SUCCESS;
}

static
USBD_STATUS
UcxpRootHubGetDescriptor(
    _Inout_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PURB Urb,
    _Out_ PBOOLEAN ForwardToClient)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST *Req = &Urb->UrbControlDescriptorRequest;

    *ForwardToClient = FALSE;

    switch (Req->DescriptorType)
    {
        case USB_DEVICE_DESCRIPTOR_TYPE:
            return UcxpCopyToTransferBuffer(Req->TransferBuffer, Req->TransferBufferMDL,
                                            &Req->TransferBufferLength,
                                            &RootHub->DeviceDescriptor,
                                            sizeof(RootHub->DeviceDescriptor));

        case USB_CONFIGURATION_DESCRIPTOR_TYPE:
            if (Req->Index != 0)
                return USBD_STATUS_INVALID_PARAMETER;
            return UcxpCopyToTransferBuffer(Req->TransferBuffer, Req->TransferBufferMDL,
                                            &Req->TransferBufferLength,
                                            &RootHub->ConfigBlock,
                                            sizeof(RootHub->ConfigBlock));

        case UCX_ROOTHUB_HUB_DESCRIPTOR_20:
        case UCX_ROOTHUB_HUB_DESCRIPTOR_30:
            /* The hub descriptor comes from the port registers' owner */
            *ForwardToClient = TRUE;
            return USBD_STATUS_SUCCESS;

        default:
            /* The root hub has no strings and no other descriptors */
            Req->TransferBufferLength = 0;
            return USBD_STATUS_INVALID_PARAMETER;
    }
}

/*
 * A forwarded hub-descriptor request is a _URB_CONTROL_DESCRIPTOR_REQUEST; the
 * buffer check below reads it through the class-request view, which is only
 * right because the three transfer fields sit at the same offsets.
 */
C_ASSERT(FIELD_OFFSET(struct _URB_CONTROL_DESCRIPTOR_REQUEST, TransferBufferLength) ==
         FIELD_OFFSET(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST, TransferBufferLength));
C_ASSERT(FIELD_OFFSET(struct _URB_CONTROL_DESCRIPTOR_REQUEST, TransferBuffer) ==
         FIELD_OFFSET(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST, TransferBuffer));
C_ASSERT(FIELD_OFFSET(struct _URB_CONTROL_DESCRIPTOR_REQUEST, TransferBufferMDL) ==
         FIELD_OFFSET(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST, TransferBufferMDL));

/*
 * Hands a hub-class request to the client after the parts this library owns
 * are checked: a buffer must exist for a non-zero length. The client completes
 * the request.
 */
static
VOID
UcxpRootHubForwardControlUrb(
    _Inout_ PUCX_ROOTHUB_CTX RootHub,
    _In_ WDFREQUEST Request,
    _Inout_ PURB Urb)
{
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *Req = &Urb->UrbControlVendorClassRequest;

    if (Req->TransferBufferLength != 0 &&
        UcxUrbTransferBuffer(Req->TransferBuffer, Req->TransferBufferMDL, Req->TransferBufferLength) == NULL)
    {
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_INVALID_PARAMETER);
        return;
    }

    if (RootHub->Config.EvtRootHubControlUrb == NULL)
    {
        UcxpCompleteUrb(Request, Urb, USBD_STATUS_NOT_SUPPORTED);
        return;
    }

    RootHub->Config.EvtRootHubControlUrb((UCXROOTHUB)WdfObjectContextGetObject(RootHub), Request);
}

static
VOID
UcxpRootHubHandleUrb(
    _Inout_ PUCX_ROOTHUB_CTX RootHub,
    _In_ WDFREQUEST Request,
    _In_opt_ PURB Urb)
{
    PUCX_CONTROLLER_CTX Controller = UcxpRootHubController(RootHub);
    PVOID DeviceHandle;
    USBD_STATUS UsbdStatus;
    BOOLEAN Forward;
    WDFQUEUE Queue;

    if (Urb == NULL)
    {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    if (Urb->UrbHeader.Length < sizeof(struct _URB_HEADER))
    {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    /* A NULL handle on the root hub PDO means the root hub itself (usbport convention) */
    DeviceHandle = Urb->UrbHeader.UsbdDeviceHandle;
    if (DeviceHandle != NULL && DeviceHandle != &RootHub->DeviceHandle)
    {
        if (!UcxpFindDeviceHandle(Controller, DeviceHandle))
        {
            UcxpCompleteUrb(Request, Urb, USBD_STATUS_DEVICE_GONE);
            return;
        }
        /* A downstream device: its default endpoint (step 2) */
        UcxpDeviceHandleUrb(RootHub, (PUCX_DEVICE_HANDLE)DeviceHandle, Request, Urb);
        return;
    }

    switch (Urb->UrbHeader.Function)
    {
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
            if (Urb->UrbHeader.Length < sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST))
            {
                UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
                break;
            }
            UsbdStatus = UcxpRootHubGetDescriptor(RootHub, Urb, &Forward);
            if (Forward)
            {
                UcxpRootHubForwardControlUrb(RootHub, Request, Urb);
                return;
            }
            break;

        case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
        {
            static const USHORT SelfPowered = 0x0001;
            struct _URB_CONTROL_GET_STATUS_REQUEST *Req = &Urb->UrbControlGetStatusRequest;

            if (Urb->UrbHeader.Length < sizeof(*Req))
            {
                UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
                break;
            }
            UsbdStatus = UcxpCopyToTransferBuffer(Req->TransferBuffer, Req->TransferBufferMDL,
                                                  &Req->TransferBufferLength,
                                                  &SelfPowered, sizeof(SelfPowered));
            break;
        }

        case URB_FUNCTION_GET_CONFIGURATION:
        {
            struct _URB_CONTROL_GET_CONFIGURATION_REQUEST *Req = &Urb->UrbControlGetConfigurationRequest;

            if (Urb->UrbHeader.Length < sizeof(*Req))
            {
                UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
                break;
            }
            UsbdStatus = UcxpCopyToTransferBuffer(Req->TransferBuffer, Req->TransferBufferMDL,
                                                  &Req->TransferBufferLength,
                                                  &RootHub->DeviceHandle.ConfigurationValue, 1);
            break;
        }

        case URB_FUNCTION_SELECT_CONFIGURATION:
            if (Urb->UrbHeader.Length < FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface))
            {
                UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
                break;
            }
            UsbdStatus = UcxpRootHubSelectConfiguration(RootHub, Urb);
            break;

        case URB_FUNCTION_SELECT_INTERFACE:
            if (Urb->UrbHeader.Length < FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface))
            {
                UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
                break;
            }
            UsbdStatus = UcxpRootHubSelectInterface(RootHub, Urb);
            break;

        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        {
            struct _URB_BULK_OR_INTERRUPT_TRANSFER *Req = &Urb->UrbBulkOrInterruptTransfer;
            NTSTATUS Status;

            if (Urb->UrbHeader.Length < sizeof(*Req))
            {
                UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
                break;
            }
            if (Req->PipeHandle != (USBD_PIPE_HANDLE)&RootHub->ScePipe ||
                RootHub->DeviceHandle.ConfigurationValue == 0)
            {
                UsbdStatus = USBD_STATUS_INVALID_PIPE_HANDLE;
                break;
            }
            /* The bitmap must fit: one bit per port plus the hub bit, i.e. the
             * endpoint's packet size the hub driver sized its buffer from */
            if (!(Req->TransferFlags & USBD_TRANSFER_DIRECTION_IN) ||
                Req->TransferBufferLength < RootHub->ScePipe.Descriptor.wMaxPacketSize ||
                Req->TransferBufferLength > UCX_ROOTHUB_MAX_TRANSFER ||
                UcxUrbTransferBuffer(Req->TransferBuffer, Req->TransferBufferMDL, Req->TransferBufferLength) == NULL)
            {
                UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
                break;
            }

            /* Pend it; the manual queue completes it as canceled if the hub
             * driver cancels, and UcxpRootHubDeliverChange completes it with
             * the bitmap. A freshly submitted transfer always polls the live
             * port registers (no flag needed). */
            Queue = RootHub->SceQueue;
            if (Queue == NULL)
            {
                UsbdStatus = USBD_STATUS_DEVICE_GONE;
                break;
            }
            Status = WdfRequestForwardToIoQueue(Request, Queue);
            if (!NT_SUCCESS(Status))
            {
                UsbdStatus = USBD_STATUS_CANCELED;
                break;
            }
            UcxpRootHubDeliverChange(RootHub, TRUE);
            return;
        }

        case URB_FUNCTION_ABORT_PIPE:
            if (Urb->UrbHeader.Length < sizeof(struct _URB_PIPE_REQUEST))
            {
                UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
                break;
            }
            if (Urb->UrbPipeRequest.PipeHandle != (USBD_PIPE_HANDLE)&RootHub->ScePipe)
            {
                UsbdStatus = USBD_STATUS_INVALID_PIPE_HANDLE;
                break;
            }
            UcxpRootHubAbortSce(RootHub);
            UsbdStatus = USBD_STATUS_SUCCESS;
            break;

        case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
        case URB_FUNCTION_SYNC_RESET_PIPE:
        case URB_FUNCTION_SYNC_CLEAR_STALL:
            if (Urb->UrbHeader.Length < sizeof(struct _URB_PIPE_REQUEST))
            {
                UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
                break;
            }
            /* A software pipe never stalls */
            UsbdStatus = (Urb->UrbPipeRequest.PipeHandle == (USBD_PIPE_HANDLE)&RootHub->ScePipe) ?
                         USBD_STATUS_SUCCESS : USBD_STATUS_INVALID_PIPE_HANDLE;
            break;

        case URB_FUNCTION_CLASS_DEVICE:
        case URB_FUNCTION_CLASS_INTERFACE:
        case URB_FUNCTION_CLASS_OTHER:
            if (Urb->UrbHeader.Length < sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST))
            {
                UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
                break;
            }
            UcxpRootHubForwardControlUrb(RootHub, Request, Urb);
            return;

        default:
            UcxLog("roothub: URB function 0x%04X not supported", Urb->UrbHeader.Function);
            UsbdStatus = USBD_STATUS_INVALID_URB_FUNCTION;
            break;
    }

    UcxpCompleteUrb(Request, Urb, UsbdStatus);
}

/* --------------------------------------------------------------------------
 * Internal IOCTLs the hub driver sends to its lower (root hub) device
 * -------------------------------------------------------------------------- */

static
VOID
NTAPI
UcxpRootHubEvtInternalIoctl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    WDFDEVICE Pdo = WdfIoQueueGetDevice(Queue);
    PUCX_PDO_CTX PdoCtx = UcxGetPdoCtx(Pdo);
    PUCX_ROOTHUB_CTX RootHub = UcxGetRootHubCtx((WDFOBJECT)PdoCtx->RootHub);
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(Request));

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode)
    {
        case IOCTL_INTERNAL_USB_SUBMIT_URB:
            UcxpRootHubHandleUrb(RootHub, Request, (PURB)Stack->Parameters.Others.Argument1);
            return;

        case IOCTL_INTERNAL_USB_GET_ROOTHUB_PDO:
        {
            PDEVICE_OBJECT Wdm = WdfDeviceWdmGetDeviceObject(Pdo);

            if (Stack->Parameters.Others.Argument1 != NULL)
                *(PDEVICE_OBJECT *)Stack->Parameters.Others.Argument1 = Wdm;
            if (Stack->Parameters.Others.Argument2 != NULL)
                *(PDEVICE_OBJECT *)Stack->Parameters.Others.Argument2 = Wdm;
            WdfRequestComplete(Request, STATUS_SUCCESS);
            return;
        }

        case IOCTL_INTERNAL_USB_GET_HUB_COUNT:
            if (Stack->Parameters.Others.Argument1 != NULL)
                ++*(PULONG)Stack->Parameters.Others.Argument1;
            WdfRequestComplete(Request, STATUS_SUCCESS);
            return;

        case IOCTL_INTERNAL_USB_GET_DEVICE_HANDLE:
            if (Stack->Parameters.Others.Argument1 != NULL)
                *(PVOID *)Stack->Parameters.Others.Argument1 = &RootHub->DeviceHandle;
            WdfRequestComplete(Request, STATUS_SUCCESS);
            return;

        case IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION:
            /* Selective suspend of the root hub is M4 */
            WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
            return;

        default:
            UcxLog("roothub: internal IOCTL 0x%08lX not supported", IoControlCode);
            WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
            return;
    }
}

/* --------------------------------------------------------------------------
 * USB_BUS_INTERFACE_HUB_V5
 * -------------------------------------------------------------------------- */

static VOID USB_BUSIFFN UcxpBusInterfaceReference(_In_ PVOID BusContext)
{ UNREFERENCED_PARAMETER(BusContext); }

static VOID USB_BUSIFFN UcxpBusInterfaceDereference(_In_ PVOID BusContext)
{ UNREFERENCED_PARAMETER(BusContext); }

static
NTSTATUS
USB_BUSIFFN
UcxpHubCreateUsbDevice(
    _In_ PVOID BusContext,
    _Outptr_ PUSB_DEVICE_HANDLE *NewDeviceHandle,
    _In_ PUSB_DEVICE_HANDLE HubDeviceHandle,
    _In_ USHORT PortStatus,
    _In_ USHORT PortNumber)
{
    PUCX_ROOTHUB_CTX RootHub = UcxpRootHubFromBusContext(BusContext);

    UNREFERENCED_PARAMETER(PortStatus);

    if (NewDeviceHandle == NULL || RootHub == NULL)
        return STATUS_INVALID_PARAMETER;
    *NewDeviceHandle = NULL;

    if (HubDeviceHandle != &RootHub->DeviceHandle)
    {
        if (!UcxpFindDeviceHandle(UcxpRootHubController(RootHub), HubDeviceHandle))
            return STATUS_DEVICE_NOT_CONNECTED;
        /* Devices behind an external hub: M3 step 3 */
        UcxLog("roothub: CreateUsbDevice behind a downstream hub refused (step 3)");
        return STATUS_NOT_IMPLEMENTED;
    }

    return UcxpDeviceCreate(RootHub, PortNumber, PortStatus, (PUCX_DEVICE_HANDLE *)NewDeviceHandle);
}

static
NTSTATUS
USB_BUSIFFN
UcxpHubInitializeUsbDevice(
    _In_ PVOID BusContext,
    _Inout_ PUSB_DEVICE_HANDLE DeviceHandle)
{
    PUCX_ROOTHUB_CTX RootHub = UcxpRootHubFromBusContext(BusContext);

    if (RootHub == NULL)
        return STATUS_INVALID_PARAMETER;
    if (DeviceHandle == &RootHub->DeviceHandle)
        return STATUS_SUCCESS;              /* the root hub needs no address */
    if (!UcxpFindDeviceHandle(UcxpRootHubController(RootHub), DeviceHandle))
        return STATUS_DEVICE_NOT_CONNECTED;
    return UcxpDeviceInitialize(RootHub, (PUCX_DEVICE_HANDLE)DeviceHandle);
}

static
NTSTATUS
USB_BUSIFFN
UcxpHubGetUsbDescriptors(
    _In_ PVOID BusContext,
    _Inout_ PUSB_DEVICE_HANDLE DeviceHandle,
    _Out_writes_bytes_to_(*DeviceDescriptorBufferLength, *DeviceDescriptorBufferLength) PUCHAR DeviceDescriptorBuffer,
    _Inout_ PULONG DeviceDescriptorBufferLength,
    _Out_writes_bytes_to_(*ConfigDescriptorBufferLength, *ConfigDescriptorBufferLength) PUCHAR ConfigDescriptorBuffer,
    _Inout_ PULONG ConfigDescriptorBufferLength)
{
    PUCX_ROOTHUB_CTX RootHub = UcxpRootHubFromBusContext(BusContext);
    ULONG Length;

    if (RootHub == NULL || DeviceDescriptorBufferLength == NULL || ConfigDescriptorBufferLength == NULL)
        return STATUS_INVALID_PARAMETER;
    if (DeviceHandle != &RootHub->DeviceHandle)
    {
        if (!UcxpFindDeviceHandle(UcxpRootHubController(RootHub), DeviceHandle))
            return STATUS_DEVICE_NOT_CONNECTED;
        return UcxpDeviceGetDescriptors(RootHub, (PUCX_DEVICE_HANDLE)DeviceHandle,
                                        DeviceDescriptorBuffer, DeviceDescriptorBufferLength,
                                        ConfigDescriptorBuffer, ConfigDescriptorBufferLength);
    }

    Length = min(*DeviceDescriptorBufferLength, sizeof(RootHub->DeviceDescriptor));
    if (Length != 0 && DeviceDescriptorBuffer != NULL)
        RtlCopyMemory(DeviceDescriptorBuffer, &RootHub->DeviceDescriptor, Length);
    *DeviceDescriptorBufferLength = Length;

    Length = min(*ConfigDescriptorBufferLength, sizeof(RootHub->ConfigBlock));
    if (Length != 0 && ConfigDescriptorBuffer != NULL)
        RtlCopyMemory(ConfigDescriptorBuffer, &RootHub->ConfigBlock, Length);
    *ConfigDescriptorBufferLength = Length;

    return STATUS_SUCCESS;
}

static
NTSTATUS
USB_BUSIFFN
UcxpHubRemoveUsbDevice(
    _In_ PVOID BusContext,
    _Inout_ PUSB_DEVICE_HANDLE DeviceHandle,
    _In_ ULONG Flags)
{
    PUCX_ROOTHUB_CTX RootHub = UcxpRootHubFromBusContext(BusContext);

    UNREFERENCED_PARAMETER(Flags);

    if (RootHub == NULL)
        return STATUS_INVALID_PARAMETER;
    if (DeviceHandle == &RootHub->DeviceHandle)
        return STATUS_INVALID_PARAMETER;    /* the root hub lives as long as its PDO */
    if (!UcxpFindDeviceHandle(UcxpRootHubController(RootHub), DeviceHandle))
        return STATUS_DEVICE_NOT_CONNECTED;
    UcxpDeviceRemove(RootHub, (PUCX_DEVICE_HANDLE)DeviceHandle);
    return STATUS_SUCCESS;
}

static
NTSTATUS
USB_BUSIFFN
UcxpHubRestoreUsbDevice(
    _In_ PVOID BusContext,
    _Inout_ PUSB_DEVICE_HANDLE OldDeviceHandle,
    _Inout_ PUSB_DEVICE_HANDLE NewDeviceHandle)
{
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(OldDeviceHandle);
    UNREFERENCED_PARAMETER(NewDeviceHandle);
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
USB_BUSIFFN
UcxpHubGetPortHackFlags(
    _In_ PVOID BusContext,
    _Inout_ PULONG Flags)
{
    UNREFERENCED_PARAMETER(BusContext);
    if (Flags != NULL)
        *Flags = 0;
    return STATUS_SUCCESS;
}

static
NTSTATUS
USB_BUSIFFN
UcxpHubQueryDeviceInformation(
    _In_ PVOID BusContext,
    _In_ PUSB_DEVICE_HANDLE DeviceHandle,
    _Out_writes_bytes_to_(DeviceInformationBufferLength, *LengthOfDataCopied) PVOID DeviceInformationBuffer,
    _In_ ULONG DeviceInformationBufferLength,
    _Inout_ PULONG LengthOfDataCopied)
{
    PUCX_ROOTHUB_CTX RootHub = UcxpRootHubFromBusContext(BusContext);
    PUSB_DEVICE_INFORMATION_0 Info = DeviceInformationBuffer;
    ULONG Pipes, ActualLength;

    if (RootHub == NULL || Info == NULL || LengthOfDataCopied == NULL)
        return STATUS_INVALID_PARAMETER;
    *LengthOfDataCopied = 0;

    if (DeviceInformationBufferLength < sizeof(USB_LEVEL_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;
    if (Info->InformationLevel > 0)
        return STATUS_NOT_SUPPORTED;

    if (DeviceHandle != &RootHub->DeviceHandle)
    {
        if (!UcxpFindDeviceHandle(UcxpRootHubController(RootHub), DeviceHandle))
            return STATUS_DEVICE_NOT_CONNECTED;
        return UcxpDeviceQueryInformation((PUCX_DEVICE_HANDLE)DeviceHandle, DeviceInformationBuffer,
                                          DeviceInformationBufferLength, LengthOfDataCopied);
    }

    Pipes = (RootHub->DeviceHandle.ConfigurationValue != 0) ? 1 : 0;
    ActualLength = FIELD_OFFSET(USB_DEVICE_INFORMATION_0, PipeList) + Pipes * sizeof(USB_PIPE_INFORMATION_0);
    if (DeviceInformationBufferLength < ActualLength)
    {
        Info->ActualLength = ActualLength;
        *LengthOfDataCopied = sizeof(USB_LEVEL_INFORMATION);
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(Info, ActualLength);
    Info->InformationLevel = 0;
    Info->ActualLength = ActualLength;
    Info->PortNumber = 0;
    Info->DeviceDescriptor = RootHub->DeviceDescriptor;
    Info->CurrentConfigurationValue = RootHub->DeviceHandle.ConfigurationValue;
    Info->DeviceAddress = RootHub->DeviceHandle.DeviceAddress;
    Info->HubAddress = 0;
    Info->DeviceSpeed = RootHub->DeviceHandle.DeviceSpeed;
    /* The hub driver knows two kinds; a SuperSpeed root hub is a 2.0-style hub to it
     * (the same choice usbport makes for its xHCI root hub) */
    Info->DeviceType = Usb20Device;
    Info->NumberOfOpenPipes = Pipes;
    if (Pipes != 0)
    {
        Info->PipeList[0].EndpointDescriptor = RootHub->ScePipe.Descriptor;
        Info->PipeList[0].ScheduleOffset = 0;
    }

    *LengthOfDataCopied = ActualLength;
    return STATUS_SUCCESS;
}

static
NTSTATUS
USB_BUSIFFN
UcxpHubGetControllerInformation(
    _In_ PVOID BusContext,
    _Inout_updates_bytes_to_(ControllerInformationBufferLength, *LengthOfDataCopied) PVOID ControllerInformationBuffer,
    _In_ ULONG ControllerInformationBufferLength,
    _Inout_ PULONG LengthOfDataCopied)
{
    PUSB_CONTROLLER_INFORMATION_0 Info = ControllerInformationBuffer;

    UNREFERENCED_PARAMETER(BusContext);

    if (Info == NULL || LengthOfDataCopied == NULL)
        return STATUS_INVALID_PARAMETER;
    *LengthOfDataCopied = 0;

    if (ControllerInformationBufferLength < sizeof(USB_LEVEL_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;
    if (Info->InformationLevel > 0)
        return STATUS_NOT_SUPPORTED;
    if (ControllerInformationBufferLength < sizeof(*Info))
    {
        Info->ActualLength = sizeof(*Info);
        *LengthOfDataCopied = sizeof(USB_LEVEL_INFORMATION);
        return STATUS_BUFFER_TOO_SMALL;
    }

    Info->ActualLength = sizeof(*Info);
    Info->SelectiveSuspendEnabled = FALSE;   /* M4 */
    Info->IsHighSpeedController = TRUE;
    *LengthOfDataCopied = sizeof(*Info);
    return STATUS_SUCCESS;
}

static
NTSTATUS
USB_BUSIFFN
UcxpHubControllerSelectiveSuspend(
    _In_ PVOID BusContext,
    _In_ BOOLEAN Enable)
{
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(Enable);
    return STATUS_SUCCESS;                   /* accepted, acted on in M4 */
}

static
NTSTATUS
USB_BUSIFFN
UcxpHubGetExtendedHubInformation(
    _In_ PVOID BusContext,
    _In_ PDEVICE_OBJECT HubPhysicalDeviceObject,
    _Inout_updates_bytes_to_(HubInformationBufferLength, *LengthOfDataCopied) PVOID HubInformationBuffer,
    _In_ ULONG HubInformationBufferLength,
    _Out_ PULONG LengthOfDataCopied)
{
    PUCX_ROOTHUB_CTX RootHub = UcxpRootHubFromBusContext(BusContext);
    PUSB_EXTHUB_INFORMATION_0 Info = HubInformationBuffer;
    ULONG Needed, Port;

    UNREFERENCED_PARAMETER(HubPhysicalDeviceObject);

    if (RootHub == NULL || Info == NULL || LengthOfDataCopied == NULL)
        return STATUS_INVALID_PARAMETER;
    *LengthOfDataCopied = 0;

    if (HubInformationBufferLength < sizeof(ULONG) || Info->InformationLevel > 0)
        return STATUS_NOT_SUPPORTED;

    Needed = FIELD_OFFSET(USB_EXTHUB_INFORMATION_0, Port) +
             RootHub->NumberOfPorts * sizeof(USB_EXTPORT_INFORMATION_0);
    if (HubInformationBufferLength < Needed)
        return STATUS_BUFFER_TOO_SMALL;

    Info->NumberOfPorts = RootHub->NumberOfPorts;
    for (Port = 0; Port < RootHub->NumberOfPorts; Port++)
    {
        Info->Port[Port].PhysicalPortNumber = Port + 1;
        Info->Port[Port].PortLabelNumber = Port + 1;
        Info->Port[Port].VidOverride = 0;
        Info->Port[Port].PidOverride = 0;
        Info->Port[Port].PortAttributes = 0;
    }

    *LengthOfDataCopied = Needed;
    return STATUS_SUCCESS;
}

static
NTSTATUS
USB_BUSIFFN
UcxpHubGetRootHubSymbolicName(
    _In_ PVOID BusContext,
    _Inout_updates_bytes_to_(HubSymNameBufferLength, *HubSymNameActualLength) PVOID HubSymNameBuffer,
    _In_ ULONG HubSymNameBufferLength,
    _Out_ PULONG HubSymNameActualLength)
{
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(HubSymNameBuffer);
    UNREFERENCED_PARAMETER(HubSymNameBufferLength);
    if (HubSymNameActualLength != NULL)
        *HubSymNameActualLength = 0;
    return STATUS_NOT_SUPPORTED;
}

static
PVOID
USB_BUSIFFN
UcxpHubGetDeviceBusContext(
    _In_ PVOID HubBusContext,
    _In_ PVOID DeviceHandle)
{
    UNREFERENCED_PARAMETER(DeviceHandle);
    return HubBusContext;
}

static
NTSTATUS
USB_BUSIFFN
UcxpHubInitialize20Hub(
    _In_ PVOID BusContext,
    _In_ PUSB_DEVICE_HANDLE HubDeviceHandle,
    _In_ ULONG TtCount)
{
    PUCX_ROOTHUB_CTX RootHub = UcxpRootHubFromBusContext(BusContext);

    UNREFERENCED_PARAMETER(TtCount);

    if (RootHub == NULL)
        return STATUS_INVALID_PARAMETER;
    if (HubDeviceHandle == &RootHub->DeviceHandle)
        return STATUS_SUCCESS;              /* the root hub has no transaction translator */
    if (!UcxpFindDeviceHandle(UcxpRootHubController(RootHub), HubDeviceHandle))
        return STATUS_DEVICE_NOT_CONNECTED;
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
USB_BUSIFFN
UcxpHubRootHubInitNotification(
    _In_ PVOID BusContext,
    _In_ PVOID CallbackContext,
    _In_ PRH_INIT_CALLBACK CallbackRoutine)
{
    PUCX_ROOTHUB_CTX RootHub = UcxpRootHubFromBusContext(BusContext);
    WDFWORKITEM WorkItem;
    KIRQL OldIrql;

    if (RootHub == NULL)
        return STATUS_INVALID_PARAMETER;

    KeAcquireSpinLock(&RootHub->Lock, &OldIrql);
    RootHub->InitCallback = CallbackRoutine;
    RootHub->InitCallbackContext = CallbackContext;
    WorkItem = RootHub->InitNotifyWorkItem;   /* one read: Destroy clears the field */
    KeReleaseSpinLock(&RootHub->Lock, OldIrql);

    /* NULL unregisters. A registration is answered once, from a work item, so
     * the hub driver is never called back inside its own registration. */
    if (CallbackRoutine != NULL && WorkItem != NULL)
        WdfWorkItemEnqueue(WorkItem);

    return STATUS_SUCCESS;
}

static
VOID
USB_BUSIFFN
UcxpHubFlushTransfers(
    _In_ PVOID BusContext,
    _In_ PVOID DeviceHandle)
{
    PUCX_ROOTHUB_CTX RootHub = UcxpRootHubFromBusContext(BusContext);

    /* Nothing is queued for the root hub itself but the SCE; a downstream
     * device's pipes give their transfers back */
    if (RootHub == NULL || DeviceHandle == NULL || DeviceHandle == &RootHub->DeviceHandle)
        return;
    if (!UcxpFindDeviceHandle(UcxpRootHubController(RootHub), DeviceHandle))
        return;
    UcxpDeviceFlushTransfers(RootHub, (PUCX_DEVICE_HANDLE)DeviceHandle);
}

static
VOID
USB_BUSIFFN
UcxpHubSetDeviceHandleData(
    _In_ PVOID BusContext,
    _In_ PVOID DeviceHandle,
    _In_ PDEVICE_OBJECT UsbDevicePdo)
{
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(DeviceHandle);
    UNREFERENCED_PARAMETER(UsbDevicePdo);
}

static
VOID
NTAPI
UcxpRootHubEvtInitNotifyWorkItem(
    _In_ WDFWORKITEM WorkItem)
{
    PUCX_PDO_CTX PdoCtx = UcxGetPdoCtx(WdfWorkItemGetParentObject(WorkItem));
    PUCX_ROOTHUB_CTX RootHub = UcxGetRootHubCtx((WDFOBJECT)PdoCtx->RootHub);
    PRH_INIT_CALLBACK Callback;
    PVOID Context;
    KIRQL OldIrql;

    KeAcquireSpinLock(&RootHub->Lock, &OldIrql);
    Callback = RootHub->InitCallback;
    Context = RootHub->InitCallbackContext;
    KeReleaseSpinLock(&RootHub->Lock, OldIrql);

    if (Callback != NULL)
    {
        UcxLog("roothub: root hub init notification -> hub driver");
        Callback(Context);
    }
}

/* --------------------------------------------------------------------------
 * USB_BUS_INTERFACE_USBDI_V2
 * -------------------------------------------------------------------------- */

static
VOID
USB_BUSIFFN
UcxpUsbdiGetUSBDIVersion(
    _In_ PVOID BusContext,
    _Out_opt_ PUSBD_VERSION_INFORMATION VersionInformation,
    _Out_opt_ PULONG HcdCapabilities)
{
    PUCX_ROOTHUB_CTX RootHub = UcxpRootHubFromBusContext(BusContext);

    if (VersionInformation != NULL)
    {
        VersionInformation->USBDI_Version = USBDI_VERSION;
        VersionInformation->Supported_USB_Version =
            (RootHub != NULL && RootHub->Config.NumberOf30Ports != 0) ? 0x0300 : 0x0200;
    }
    if (HcdCapabilities != NULL)
        *HcdCapabilities = 0;
}

static
NTSTATUS
USB_BUSIFFN
UcxpUsbdiQueryBusTime(
    _In_ PVOID BusContext,
    _Out_opt_ PULONG CurrentFrame)
{
    UNREFERENCED_PARAMETER(BusContext);

    /* One frame per millisecond, 11 bits, as a host controller counts them;
     * reading MFINDEX through the client is a later refinement. */
    if (CurrentFrame != NULL)
        *CurrentFrame = (ULONG)((KeQueryInterruptTime() / 10000ULL) & 0x7FF);
    return STATUS_SUCCESS;
}

static
NTSTATUS
USB_BUSIFFN
UcxpUsbdiSubmitIsoOutUrb(
    _In_ PVOID BusContext,
    _In_ PURB Urb)
{
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(Urb);
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
USB_BUSIFFN
UcxpUsbdiQueryBusInformation(
    _In_ PVOID BusContext,
    _In_ ULONG Level,
    _Inout_ PVOID BusInformationBuffer,
    _Out_ PULONG BusInformationBufferLength,
    _Out_opt_ PULONG BusInformationActualLength)
{
    PUCX_ROOTHUB_CTX RootHub = UcxpRootHubFromBusContext(BusContext);
    ULONG Available, Needed;
    ULONG TotalBandwidth;

    if (RootHub == NULL || BusInformationBufferLength == NULL)
        return STATUS_INVALID_PARAMETER;

    Available = *BusInformationBufferLength;
    TotalBandwidth = (RootHub->Config.NumberOf30Ports != 0) ? 400000 : 12000;

    switch (Level)
    {
        case 0:
            Needed = sizeof(USB_BUS_INFORMATION_LEVEL_0);
            break;
        case 1:
            Needed = FIELD_OFFSET(USB_BUS_INFORMATION_LEVEL_1, ControllerNameUnicodeString) +
                     sizeof(UcxRootHubControllerName);
            break;
        default:
            return STATUS_NOT_SUPPORTED;
    }

    *BusInformationBufferLength = Needed;
    if (BusInformationActualLength != NULL)
        *BusInformationActualLength = Needed;

    if (BusInformationBuffer == NULL || Available < Needed)
        return STATUS_BUFFER_TOO_SMALL;

    if (Level == 0)
    {
        PUSB_BUS_INFORMATION_LEVEL_0 Info = BusInformationBuffer;
        Info->TotalBandwidth = TotalBandwidth;
        Info->ConsumedBandwidth = 0;
    }
    else
    {
        PUSB_BUS_INFORMATION_LEVEL_1 Info = BusInformationBuffer;
        Info->TotalBandwidth = TotalBandwidth;
        Info->ConsumedBandwidth = 0;
        Info->ControllerNameLength = sizeof(UcxRootHubControllerName);
        RtlCopyMemory(Info->ControllerNameUnicodeString, UcxRootHubControllerName,
                      sizeof(UcxRootHubControllerName));
    }
    return STATUS_SUCCESS;
}

static
BOOLEAN
USB_BUSIFFN
UcxpUsbdiIsDeviceHighSpeed(
    _In_opt_ PVOID BusContext)
{
    UNREFERENCED_PARAMETER(BusContext);
    return TRUE;
}

static
NTSTATUS
USB_BUSIFFN
UcxpUsbdiEnumLogEntry(
    _In_ PVOID BusContext,
    _In_ ULONG DriverTag,
    _In_ ULONG EnumTag,
    _In_ ULONG P1,
    _In_ ULONG P2)
{
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(DriverTag);
    UNREFERENCED_PARAMETER(EnumTag);
    UNREFERENCED_PARAMETER(P1);
    UNREFERENCED_PARAMETER(P2);
    return STATUS_SUCCESS;
}

static
VOID
UcxpRootHubInitInterfaces(
    _Inout_ PUCX_ROOTHUB_CTX RootHub)
{
    PUSB_BUS_INTERFACE_HUB_V5 Hub = &RootHub->HubInterface;
    PUSB_BUS_INTERFACE_USBDI_V2 Usbdi = &RootHub->UsbdiInterface;

    RtlZeroMemory(Hub, sizeof(*Hub));
    Hub->Size = sizeof(*Hub);
    Hub->Version = USB_BUSIF_HUB_VERSION_5;
    Hub->BusContext = RootHub;
    Hub->InterfaceReference = UcxpBusInterfaceReference;
    Hub->InterfaceDereference = UcxpBusInterfaceDereference;
    Hub->CreateUsbDevice = UcxpHubCreateUsbDevice;
    Hub->InitializeUsbDevice = UcxpHubInitializeUsbDevice;
    Hub->GetUsbDescriptors = UcxpHubGetUsbDescriptors;
    Hub->RemoveUsbDevice = UcxpHubRemoveUsbDevice;
    Hub->RestoreUsbDevice = UcxpHubRestoreUsbDevice;
    Hub->GetPortHackFlags = UcxpHubGetPortHackFlags;
    Hub->QueryDeviceInformation = UcxpHubQueryDeviceInformation;
    Hub->GetControllerInformation = UcxpHubGetControllerInformation;
    Hub->ControllerSelectiveSuspend = UcxpHubControllerSelectiveSuspend;
    Hub->GetExtendedHubInformation = UcxpHubGetExtendedHubInformation;
    Hub->GetRootHubSymbolicName = UcxpHubGetRootHubSymbolicName;
    Hub->GetDeviceBusContext = UcxpHubGetDeviceBusContext;
    Hub->Initialize20Hub = UcxpHubInitialize20Hub;
    Hub->RootHubInitNotification = UcxpHubRootHubInitNotification;
    Hub->FlushTransfers = UcxpHubFlushTransfers;
    Hub->SetDeviceHandleData = UcxpHubSetDeviceHandleData;

    RtlZeroMemory(Usbdi, sizeof(*Usbdi));
    Usbdi->Size = sizeof(*Usbdi);
    Usbdi->Version = USB_BUSIF_USBDI_VERSION_2;
    Usbdi->BusContext = RootHub;
    Usbdi->InterfaceReference = UcxpBusInterfaceReference;
    Usbdi->InterfaceDereference = UcxpBusInterfaceDereference;
    Usbdi->GetUSBDIVersion = UcxpUsbdiGetUSBDIVersion;
    Usbdi->QueryBusTime = UcxpUsbdiQueryBusTime;
    Usbdi->SubmitIsoOutUrb = UcxpUsbdiSubmitIsoOutUrb;
    Usbdi->QueryBusInformation = UcxpUsbdiQueryBusInformation;
    Usbdi->IsDeviceHighSpeed = UcxpUsbdiIsDeviceHighSpeed;
    Usbdi->EnumLogEntry = UcxpUsbdiEnumLogEntry;
}

/* --------------------------------------------------------------------------
 * The PDO
 * -------------------------------------------------------------------------- */

NTSTATUS
UcxpRootHubCreatePdo(
    _In_ UCXROOTHUB RootHubHandle)
{
    PUCX_ROOTHUB_CTX RootHub = UcxGetRootHubCtx((WDFOBJECT)RootHubHandle);
    PUCX_CONTROLLER_CTX Controller = UcxpRootHubController(RootHub);
    DECLARE_CONST_UNICODE_STRING(DeviceId, L"USB\\ROOT_HUB30");
    DECLARE_CONST_UNICODE_STRING(InstanceId, L"0");
    DECLARE_CONST_UNICODE_STRING(DeviceText, L"USB Root Hub (USB 3.0)");
    DECLARE_UNICODE_STRING_SIZE(HardwareId, 96);
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_QUERY_INTERFACE_CONFIG InterfaceConfig;
    WDF_DEVICE_PNP_CAPABILITIES PnpCaps;
    WDF_DEVICE_POWER_CAPABILITIES PowerCaps;
    WDF_WORKITEM_CONFIG WorkItemConfig;
    PWDFDEVICE_INIT Init;
    WDFDEVICE Pdo = NULL;
    PUCX_PDO_CTX PdoCtx;
    NTSTATUS Status;
    ULONG Index;

    if (RootHub->Pdo != NULL)
        return STATUS_SUCCESS;

    RootHub->NumberOfPorts = RootHub->Config.NumberOf20Ports + RootHub->Config.NumberOf30Ports;
    if (RootHub->NumberOfPorts == 0 || RootHub->NumberOfPorts > 255)
        return STATUS_INVALID_PARAMETER;

    KeInitializeSpinLock(&RootHub->Lock);
    UcxpRootHubBuildDescriptors(RootHub);
    UcxpRootHubInitInterfaces(RootHub);

    /* A client without a PnP stack (the M1 smoke client runs on a control
     * device) gets the root hub object without a PDO: nothing can bind to it,
     * UcxRootHubPortChanged is a no-op, and the smoke leg keeps its meaning. */
    if (WdfDeviceWdmGetPhysicalDevice(Controller->ClientDevice) == NULL)
    {
        UcxLog("roothub: client device has no PnP stack, no root hub PDO");
        return STATUS_SUCCESS;
    }

    Init = WdfPdoInitAllocate(Controller->ClientDevice);
    if (Init == NULL)
    {
        UcxLog("roothub: WdfPdoInitAllocate failed, no root hub PDO");
        return STATUS_SUCCESS;
    }

    WdfDeviceInitSetDeviceType(Init, FILE_DEVICE_BUS_EXTENDER);
    WdfDeviceInitSetExclusive(Init, FALSE);

    Status = WdfPdoInitAssignDeviceID(Init, &DeviceId);
    if (NT_SUCCESS(Status) && Controller->Config.VendorId != 0)
    {
        Status = RtlUnicodeStringPrintf(&HardwareId, L"USB\\ROOT_HUB30&VID_%04X&PID_%04X&REV_%04X",
                                        Controller->Config.VendorId, Controller->Config.DeviceId,
                                        Controller->Config.RevisionId);
        if (NT_SUCCESS(Status))
            Status = WdfPdoInitAddHardwareID(Init, &HardwareId);
        if (NT_SUCCESS(Status))
            Status = RtlUnicodeStringPrintf(&HardwareId, L"USB\\ROOT_HUB30&VID_%04X&PID_%04X",
                                            Controller->Config.VendorId, Controller->Config.DeviceId);
        if (NT_SUCCESS(Status))
            Status = WdfPdoInitAddHardwareID(Init, &HardwareId);
    }
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAddHardwareID(Init, &DeviceId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAssignInstanceID(Init, &InstanceId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAddDeviceText(Init, &DeviceText, &DeviceText, UCX_ROOTHUB_LANGUAGE);
    if (!NT_SUCCESS(Status))
    {
        WdfDeviceInitFree(Init);
        UcxLog("roothub: PDO ids failed 0x%08lX", (ULONG)Status);
        return Status;
    }
    WdfPdoInitSetDefaultLocale(Init, UCX_ROOTHUB_LANGUAGE);
    WdfPdoInitAllowForwardingRequestToParent(Init);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, UCX_PDO_CTX);
    Status = WdfDeviceCreate(&Init, &Attributes, &Pdo);
    if (!NT_SUCCESS(Status))
    {
        if (Init != NULL)
            WdfDeviceInitFree(Init);
        UcxLog("roothub: WdfDeviceCreate(PDO) failed 0x%08lX", (ULONG)Status);
        return Status;
    }
    PdoCtx = UcxGetPdoCtx(Pdo);
    PdoCtx->RootHub = RootHubHandle;

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&PnpCaps);
    PnpCaps.Removable = WdfFalse;
    PnpCaps.SurpriseRemovalOK = WdfTrue;
    PnpCaps.UniqueID = WdfFalse;
    PnpCaps.Address = 0;
    PnpCaps.UINumber = 0;
    WdfDeviceSetPnpCapabilities(Pdo, &PnpCaps);

    WDF_DEVICE_POWER_CAPABILITIES_INIT(&PowerCaps);
    PowerCaps.DeviceD1 = WdfFalse;
    PowerCaps.DeviceD2 = WdfFalse;
    PowerCaps.WakeFromD0 = WdfFalse;
    PowerCaps.WakeFromD1 = WdfFalse;
    PowerCaps.WakeFromD2 = WdfFalse;
    PowerCaps.WakeFromD3 = WdfFalse;
    PowerCaps.DeviceState[PowerSystemWorking] = PowerDeviceD0;
    for (Index = PowerSystemSleeping1; Index < PowerSystemMaximum; Index++)
        PowerCaps.DeviceState[Index] = PowerDeviceD3;
    PowerCaps.DeviceWake = PowerDeviceUnspecified;
    PowerCaps.SystemWake = PowerSystemUnspecified;
    PowerCaps.IdealDxStateForSx = PowerDeviceD3;
    WdfDeviceSetPowerCapabilities(Pdo, &PowerCaps);

    /* The hub driver's IOCTLs: parallel, not power managed (the PDO's own
     * power state has no bearing on the controller), and at PASSIVE_LEVEL:
     * class drivers submit URBs from completion routines at DISPATCH_LEVEL,
     * while the device paths below wait for the controller (control
     * transfers, endpoint configuration, pipe abort and reset). */
    {
        WDF_OBJECT_ATTRIBUTES QueueAttributes;

        WDF_OBJECT_ATTRIBUTES_INIT(&QueueAttributes);
        QueueAttributes.ExecutionLevel = WdfExecutionLevelPassive;
        WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
        QueueConfig.EvtIoInternalDeviceControl = UcxpRootHubEvtInternalIoctl;
        QueueConfig.PowerManaged = WdfFalse;
        Status = WdfIoQueueCreate(Pdo, &QueueConfig, &QueueAttributes, WDF_NO_HANDLE);
    }

    /* Pended status-change transfers */
    if (NT_SUCCESS(Status))
    {
        WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);
        QueueConfig.PowerManaged = WdfFalse;
        Status = WdfIoQueueCreate(Pdo, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &RootHub->SceQueue);
    }

    /* Root hub init notification is delivered from PASSIVE_LEVEL, off the caller */
    if (NT_SUCCESS(Status))
    {
        WDF_WORKITEM_CONFIG_INIT(&WorkItemConfig, UcxpRootHubEvtInitNotifyWorkItem);
        WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
        Attributes.ParentObject = Pdo;
        Status = WdfWorkItemCreate(&WorkItemConfig, &Attributes, &RootHub->InitNotifyWorkItem);
    }

    /* The two interfaces the hub driver queries from its lower device */
    if (NT_SUCCESS(Status))
    {
        WDF_QUERY_INTERFACE_CONFIG_INIT(&InterfaceConfig, (PINTERFACE)&RootHub->HubInterface,
                                        &USB_BUS_INTERFACE_HUB_GUID, NULL);
        Status = WdfDeviceAddQueryInterface(Pdo, &InterfaceConfig);
    }
    if (NT_SUCCESS(Status))
    {
        WDF_QUERY_INTERFACE_CONFIG_INIT(&InterfaceConfig, (PINTERFACE)&RootHub->UsbdiInterface,
                                        &USB_BUS_INTERFACE_USBDI_GUID, NULL);
        Status = WdfDeviceAddQueryInterface(Pdo, &InterfaceConfig);
    }

    if (NT_SUCCESS(Status))
        Status = WdfFdoAddStaticChild(Controller->ClientDevice, Pdo);

    if (!NT_SUCCESS(Status))
    {
        UcxLog("roothub: PDO setup failed 0x%08lX", (ULONG)Status);
        RootHub->SceQueue = NULL;
        RootHub->InitNotifyWorkItem = NULL;
        WdfObjectDelete(Pdo);
        return Status;
    }

    UcxpAddDeviceHandle(Controller, &RootHub->DeviceHandle);
    RootHub->Pdo = Pdo;
    UcxLog("roothub: PDO %p created, %u ports (%u USB2 + %u USB3), hub descriptor type 0x%02X",
           Pdo, RootHub->NumberOfPorts, RootHub->Config.NumberOf20Ports, RootHub->Config.NumberOf30Ports,
           (RootHub->Config.NumberOf30Ports != 0) ? UCX_ROOTHUB_HUB_DESCRIPTOR_30 : UCX_ROOTHUB_HUB_DESCRIPTOR_20);
    return STATUS_SUCCESS;
}

VOID
UcxpRootHubDestroyPdo(
    _In_ UCXROOTHUB RootHubHandle)
{
    PUCX_ROOTHUB_CTX RootHub = UcxGetRootHubCtx((WDFOBJECT)RootHubHandle);
    PUCX_CONTROLLER_CTX Controller;
    WDFWORKITEM WorkItem;
    KIRQL OldIrql;

    if (RootHub->Pdo == NULL)
        return;

    /* No more init callbacks: clear the registration, take the work item out
     * of reach of new registrations, then wait for one already running (this
     * runs at PASSIVE_LEVEL from the root hub object's cleanup, never from the
     * work item itself). The client's PortChanged calls stop with its device,
     * which is this object's ancestor, before this cleanup runs. */
    KeAcquireSpinLock(&RootHub->Lock, &OldIrql);
    RootHub->InitCallback = NULL;
    RootHub->InitCallbackContext = NULL;
    WorkItem = RootHub->InitNotifyWorkItem;
    RootHub->InitNotifyWorkItem = NULL;
    KeReleaseSpinLock(&RootHub->Lock, OldIrql);
    if (WorkItem != NULL)
        WdfWorkItemFlush(WorkItem);

    /* No more status changes */
    UcxpRootHubAbortSce(RootHub);
    RootHub->SceQueue = NULL;

    Controller = UcxpRootHubController(RootHub);
    UcxpRemoveDeviceHandle(Controller, &RootHub->DeviceHandle);

    if (InterlockedExchange(&RootHub->PdoMarkedMissing, 1) == 0)
    {
        NTSTATUS Status = WdfPdoMarkMissing(RootHub->Pdo);
        UcxLog("roothub: PDO %p marked missing 0x%08lX", RootHub->Pdo, (ULONG)Status);
    }
    RootHub->Pdo = NULL;
}
