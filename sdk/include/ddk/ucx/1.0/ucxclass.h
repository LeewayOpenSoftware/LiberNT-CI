/*
 * PROJECT:     LiberNT DDK
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     USB host Controller eXtension (UCX) class-extension client interface
 * NOTE:        Clean-room reconstruction of the public WDK UCX DDI surface
 *              (ucxclass.h / ucxcontroller.h / ucxroothub.h / ucxusbdevice.h /
 *              ucxendpoint.h). Milestone M1 skeleton: object + create DDIs only.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>

/* UCX framework objects (opaque handles, mirror WDFOBJECT lifetime) */
DECLARE_HANDLE(UCXCONTROLLER);
DECLARE_HANDLE(UCXROOTHUB);
DECLARE_HANDLE(UCXUSBDEVICE);
DECLARE_HANDLE(UCXENDPOINT);

/* USB types used by the DDI (speed, setup packet, USBD status); the headers guard themselves */
#include <usb100.h>
#include <usb200.h>
#include <usb.h>

typedef struct _UCXUSBDEVICE_INIT *PUCXUSBDEVICE_INIT;
typedef struct _UCXENDPOINT_INIT  *PUCXENDPOINT_INIT;

/*
 * Class function table indices. The UCX class library (ucx01000) publishes a
 * table of this shape; a client (usbxhci) binds against L"Ucx" and calls through
 * UcxClassFunctions[] via the inline wrappers below.
 */
typedef enum _UCX_CLASS_FUNCTIONS {
    UcxControllerCreateIndex = 0,
    UcxControllerSetFailedIndex,
    UcxControllerNeedsResetIndex,
    UcxControllerResetCompleteIndex,
    UcxRootHubCreateIndex,
    UcxRootHubPortChangedIndex,
    UcxUsbDeviceCreateIndex,
    UcxUsbDeviceInitSetEventCallbacksIndex,
    UcxUsbDeviceRemoteWakeNotificationIndex,
    UcxEndpointCreateIndex,
    UcxEndpointInitSetEventCallbacksIndex,
    UcxEndpointSetWdfIoQueueIndex,
    UcxEndpointPurgeCompleteIndex,
    UcxEndpointNeedToCancelTransfersIndex,
    UcxIoDeviceControlIndex,
    UcxFunctionsMax
} UCX_CLASS_FUNCTIONS;

typedef void (NTAPI *PFN_UCX_CLASS)(void);
extern PFN_UCX_CLASS UcxClassFunctions[UcxFunctionsMax];

/* ---- Controller ---------------------------------------------------------- */

typedef NTSTATUS (NTAPI *PFN_UCX_CONTROLLER_RESET)(_In_ UCXCONTROLLER Controller);
typedef NTSTATUS (NTAPI *PFN_UCX_CONTROLLER_QUERY_USB_CAPABILITY)(
    _In_ UCXCONTROLLER Controller, _In_ PGUID CapabilityType,
    _In_ ULONG OutputBufferLength, _Out_opt_ PVOID OutputBuffer, _Out_ PULONG ResultLength);

/*
 * EvtControllerUsbDeviceAdd (M3): the hub driver asked for a new device on a
 * root port. The client sets its device callbacks on UsbDeviceInit
 * (UcxUsbDeviceInitSetEventCallbacks) and calls UcxUsbDeviceCreate with it;
 * returning without creating the object fails the hub driver's request.
 */
typedef struct _UCX_USBDEVICE_INFO {
    ULONG Size;
    USHORT PortNumber;          /* root port, 1-based */
    USHORT PortStatus;          /* the hub driver's view of the port (USB_PORT_STATUS) */
} UCX_USBDEVICE_INFO, *PUCX_USBDEVICE_INFO;

typedef NTSTATUS (NTAPI *PFN_UCX_CONTROLLER_USBDEVICE_ADD)(
    _In_ UCXCONTROLLER Controller, _In_ PUCX_USBDEVICE_INFO UsbDeviceInfo, _Inout_ PUCXUSBDEVICE_INIT UsbDeviceInit);

typedef struct _UCX_CONTROLLER_CONFIG {
    ULONG Size;
    USHORT NumberOfRootHubs;
    PFN_UCX_CONTROLLER_RESET                 EvtControllerReset;
    PFN_UCX_CONTROLLER_QUERY_USB_CAPABILITY  EvtControllerQueryUsbCapability;
    /* Optional (M3): the controller's PCI identity, carried into the root hub's
     * hardware id and device descriptor. All zero when the client has no bus
     * interface; the root hub still enumerates. */
    USHORT VendorId;
    USHORT DeviceId;
    USHORT RevisionId;
    PFN_UCX_CONTROLLER_USBDEVICE_ADD         EvtControllerUsbDeviceAdd;     /* M3 step 2 */
} UCX_CONTROLLER_CONFIG, *PUCX_CONTROLLER_CONFIG;

FORCEINLINE VOID UCX_CONTROLLER_CONFIG_INIT(_Out_ PUCX_CONTROLLER_CONFIG C)
{ RtlZeroMemory(C, sizeof(*C)); C->Size = sizeof(*C); }

typedef NTSTATUS (NTAPI *PFN_UCXCONTROLLERCREATE)(
    _In_ WDFDEVICE, _In_ PUCX_CONTROLLER_CONFIG, _In_opt_ PWDF_OBJECT_ATTRIBUTES, _Out_ UCXCONTROLLER *);
typedef VOID (NTAPI *PFN_UCXCONTROLLERSETFAILED)(_In_ UCXCONTROLLER);
typedef VOID (NTAPI *PFN_UCXCONTROLLERNEEDSRESET)(_In_ UCXCONTROLLER);
typedef VOID (NTAPI *PFN_UCXCONTROLLERRESETCOMPLETE)(_In_ UCXCONTROLLER, _In_ BOOLEAN);

#define UcxControllerCreate(d,c,a,o) \
    ((PFN_UCXCONTROLLERCREATE)UcxClassFunctions[UcxControllerCreateIndex])((d),(c),(a),(o))
#define UcxControllerSetFailed(x) \
    ((PFN_UCXCONTROLLERSETFAILED)UcxClassFunctions[UcxControllerSetFailedIndex])((x))
#define UcxControllerNeedsReset(x) \
    ((PFN_UCXCONTROLLERNEEDSRESET)UcxClassFunctions[UcxControllerNeedsResetIndex])((x))
#define UcxControllerResetComplete(x,b) \
    ((PFN_UCXCONTROLLERRESETCOMPLETE)UcxClassFunctions[UcxControllerResetCompleteIndex])((x),(b))

/* ---- Root hub ------------------------------------------------------------ */

/*
 * EvtRootHubControlUrb: a hub-class or hub-descriptor request for the root hub
 * (URB_FUNCTION_CLASS_DEVICE / CLASS_OTHER / CLASS_INTERFACE, or GET_DESCRIPTOR
 * of type 0x29/0x2A). The client answers it from the port registers and completes
 * the request; UcxRequestGetUrb() gives the URB. The class library has already
 * validated the URB header, the device handle and the buffer against its length.
 *
 * EvtRootHubGetPortChangeBitmap: called at <= DISPATCH_LEVEL when the class
 * library is about to complete a status-change (interrupt IN) transfer. The
 * client sets bit N of Bitmap for every port N (1-based) whose change bits are
 * set and returns TRUE when any bit was set; Length is the buffer size in bytes
 * and the client never writes past it.
 */
typedef VOID (NTAPI *PFN_UCX_ROOTHUB_CONTROL_URB)(_In_ UCXROOTHUB, _In_ WDFREQUEST);
typedef VOID (NTAPI *PFN_UCX_ROOTHUB_GET_INFO)(_In_ UCXROOTHUB, _In_ WDFREQUEST);
typedef BOOLEAN (NTAPI *PFN_UCX_ROOTHUB_GET_PORT_CHANGE_BITMAP)(
    _In_ UCXROOTHUB RootHub, _Out_writes_bytes_(Length) PUCHAR Bitmap, _In_ ULONG Length);

typedef struct _UCX_ROOTHUB_CONFIG {
    ULONG Size;
    USHORT NumberOf20Ports;
    USHORT NumberOf30Ports;
    PFN_UCX_ROOTHUB_CONTROL_URB  EvtRootHubControlUrb;
    PFN_UCX_ROOTHUB_GET_INFO     EvtRootHubGetInfo;
    PFN_UCX_ROOTHUB_GET_PORT_CHANGE_BITMAP EvtRootHubGetPortChangeBitmap;   /* M3 */
} UCX_ROOTHUB_CONFIG, *PUCX_ROOTHUB_CONFIG;

FORCEINLINE VOID UCX_ROOTHUB_CONFIG_INIT(_Out_ PUCX_ROOTHUB_CONFIG C)
{ RtlZeroMemory(C, sizeof(*C)); C->Size = sizeof(*C); }

typedef NTSTATUS (NTAPI *PFN_UCXROOTHUBCREATE)(
    _In_ UCXCONTROLLER, _In_ PUCX_ROOTHUB_CONFIG, _In_opt_ PWDF_OBJECT_ATTRIBUTES, _Out_ UCXROOTHUB *);
typedef VOID (NTAPI *PFN_UCXROOTHUBPORTCHANGED)(_In_ UCXROOTHUB);

#define UcxRootHubCreate(c,cfg,a,o) \
    ((PFN_UCXROOTHUBCREATE)UcxClassFunctions[UcxRootHubCreateIndex])((c),(cfg),(a),(o))
#define UcxRootHubPortChanged(r) \
    ((PFN_UCXROOTHUBPORTCHANGED)UcxClassFunctions[UcxRootHubPortChangedIndex])((r))

/* ---- USB device ---------------------------------------------------------- */

/*
 * Device callbacks (M3 step 2). All synchronous, PASSIVE_LEVEL, called by the
 * class library on behalf of the hub driver, one at a time per device:
 *   Enable:  the port was reset by the hub; take a slot for it. The client
 *            reports the port's negotiated speed and the default EP0 packet size.
 *   Address: assign the USB address (xHCI: Address Device); report it.
 *   DefaultEndpointAdd: create the default control endpoint object
 *            (UcxEndpointCreate on the init handed in) and its transfer callback.
 *   UpdateDefaultEndpoint: the device descriptor named a different EP0 size.
 *   Disable: the device is gone or failed; release the slot.
 */
typedef struct _UCX_USBDEVICE_ENABLE_INFO {
    ULONG Size;
    USHORT PortNumber;              /* in */
    USHORT Reserved;
    USB_DEVICE_SPEED Speed;         /* out */
    ULONG MaxPacketSize0;           /* out: 8 / 64 / 512 */
} UCX_USBDEVICE_ENABLE_INFO, *PUCX_USBDEVICE_ENABLE_INFO;

typedef NTSTATUS (NTAPI *PFN_UCX_USBDEVICE_ENABLE)(
    _In_ UCXCONTROLLER, _In_ UCXUSBDEVICE, _Inout_ PUCX_USBDEVICE_ENABLE_INFO);
typedef NTSTATUS (NTAPI *PFN_UCX_USBDEVICE_ADDRESS)(
    _In_ UCXCONTROLLER, _In_ UCXUSBDEVICE, _Out_ PUSHORT DeviceAddress);
typedef NTSTATUS (NTAPI *PFN_UCX_USBDEVICE_DEFAULT_ENDPOINT_ADD)(
    _In_ UCXCONTROLLER, _In_ UCXUSBDEVICE, _In_ ULONG MaxPacketSize0, _Inout_ PUCXENDPOINT_INIT);
typedef NTSTATUS (NTAPI *PFN_UCX_USBDEVICE_UPDATE_DEFAULT_ENDPOINT)(
    _In_ UCXCONTROLLER, _In_ UCXUSBDEVICE, _In_ ULONG MaxPacketSize0);
typedef VOID (NTAPI *PFN_UCX_USBDEVICE_DISABLE)(_In_ UCXCONTROLLER, _In_ UCXUSBDEVICE);

typedef struct _UCX_USBDEVICE_EVENT_CALLBACKS {
    ULONG Size;
    PFN_UCX_USBDEVICE_ENABLE                   EvtUsbDeviceEnable;
    PFN_UCX_USBDEVICE_ADDRESS                  EvtUsbDeviceAddress;
    PFN_UCX_USBDEVICE_DEFAULT_ENDPOINT_ADD     EvtUsbDeviceDefaultEndpointAdd;
    PFN_UCX_USBDEVICE_UPDATE_DEFAULT_ENDPOINT  EvtUsbDeviceUpdateDefaultEndpoint;
    PFN_UCX_USBDEVICE_DISABLE                  EvtUsbDeviceDisable;
} UCX_USBDEVICE_EVENT_CALLBACKS, *PUCX_USBDEVICE_EVENT_CALLBACKS;

FORCEINLINE VOID UCX_USBDEVICE_EVENT_CALLBACKS_INIT(_Out_ PUCX_USBDEVICE_EVENT_CALLBACKS C)
{ RtlZeroMemory(C, sizeof(*C)); C->Size = sizeof(*C); }

/*
 * A transfer the class library hands to an endpoint (M3 step 2: control
 * transfers, synchronously). Buffer or Mdl names the caller's memory; the
 * client reads or writes at most Length bytes of it and reports Transferred
 * and the USBD status (STALL, device not responding, ...) besides the NTSTATUS.
 */
typedef enum _UCX_TRANSFER_TYPE {
    UcxTransferControl = 0,
    UcxTransferBulk,
    UcxTransferInterrupt
} UCX_TRANSFER_TYPE;

typedef struct _UCX_TRANSFER {
    ULONG Size;
    UCX_TRANSFER_TYPE Type;
    USB_DEFAULT_PIPE_SETUP_PACKET SetupPacket;   /* control only */
    BOOLEAN DataIn;
    ULONG Length;
    PVOID Buffer;
    PMDL Mdl;
    ULONG Transferred;              /* out */
    USBD_STATUS UsbdStatus;         /* out */
} UCX_TRANSFER, *PUCX_TRANSFER;

#define UCX_CONTROL_MAX_TRANSFER 4096   /* one page per control transfer in M3 */

typedef NTSTATUS (NTAPI *PFN_UCXUSBDEVICECREATE)(
    _In_ UCXCONTROLLER, _Inout_ PUCXUSBDEVICE_INIT *, _In_opt_ PWDF_OBJECT_ATTRIBUTES, _Out_ UCXUSBDEVICE *);
typedef VOID (NTAPI *PFN_UCXUSBDEVICEINITSETEVENTCALLBACKS)(
    _In_ PUCXUSBDEVICE_INIT, _In_ PUCX_USBDEVICE_EVENT_CALLBACKS);
typedef VOID (NTAPI *PFN_UCXUSBDEVICEREMOTEWAKENOTIFICATION)(_In_ UCXUSBDEVICE, _In_ NTSTATUS);

#define UcxUsbDeviceCreate(c,i,a,o) \
    ((PFN_UCXUSBDEVICECREATE)UcxClassFunctions[UcxUsbDeviceCreateIndex])((c),(i),(a),(o))
#define UcxUsbDeviceInitSetEventCallbacks(i,cb) \
    ((PFN_UCXUSBDEVICEINITSETEVENTCALLBACKS)UcxClassFunctions[UcxUsbDeviceInitSetEventCallbacksIndex])((i),(cb))
#define UcxUsbDeviceRemoteWakeNotification(d,s) \
    ((PFN_UCXUSBDEVICEREMOTEWAKENOTIFICATION)UcxClassFunctions[UcxUsbDeviceRemoteWakeNotificationIndex])((d),(s))

/* ---- Endpoint ------------------------------------------------------------ */

typedef VOID (NTAPI *PFN_UCX_ENDPOINT_PURGE)(_In_ UCXENDPOINT);
typedef VOID (NTAPI *PFN_UCX_ENDPOINT_START)(_In_ UCXENDPOINT);
typedef VOID (NTAPI *PFN_UCX_ENDPOINT_RESET)(_In_ UCXENDPOINT, _In_ WDFREQUEST);
/* M3 step 2: one control transfer, synchronous at PASSIVE_LEVEL, serialized by the client */
typedef NTSTATUS (NTAPI *PFN_UCX_ENDPOINT_CONTROL_TRANSFER)(_In_ UCXENDPOINT, _Inout_ PUCX_TRANSFER);

typedef struct _UCX_ENDPOINT_EVENT_CALLBACKS {
    ULONG Size;
    PFN_UCX_ENDPOINT_PURGE  EvtEndpointPurge;
    PFN_UCX_ENDPOINT_START  EvtEndpointStart;
    PFN_UCX_ENDPOINT_RESET  EvtEndpointReset;
    PFN_UCX_ENDPOINT_CONTROL_TRANSFER EvtEndpointControlTransfer;   /* M3 step 2 */
} UCX_ENDPOINT_EVENT_CALLBACKS, *PUCX_ENDPOINT_EVENT_CALLBACKS;

FORCEINLINE VOID UCX_ENDPOINT_EVENT_CALLBACKS_INIT(_Out_ PUCX_ENDPOINT_EVENT_CALLBACKS C)
{ RtlZeroMemory(C, sizeof(*C)); C->Size = sizeof(*C); }

typedef NTSTATUS (NTAPI *PFN_UCXENDPOINTCREATE)(
    _In_ UCXUSBDEVICE, _Inout_ PUCXENDPOINT_INIT *, _In_opt_ PWDF_OBJECT_ATTRIBUTES, _Out_ UCXENDPOINT *);
typedef VOID (NTAPI *PFN_UCXENDPOINTINITSETEVENTCALLBACKS)(
    _In_ PUCXENDPOINT_INIT, _In_ PUCX_ENDPOINT_EVENT_CALLBACKS);
typedef VOID (NTAPI *PFN_UCXENDPOINTSETWDFIOQUEUE)(_In_ UCXENDPOINT, _In_ WDFQUEUE);
typedef VOID (NTAPI *PFN_UCXENDPOINTPURGECOMPLETE)(_In_ UCXENDPOINT);
typedef VOID (NTAPI *PFN_UCXENDPOINTNEEDTOCANCELTRANSFERS)(_In_ UCXENDPOINT);

#define UcxEndpointCreate(d,i,a,o) \
    ((PFN_UCXENDPOINTCREATE)UcxClassFunctions[UcxEndpointCreateIndex])((d),(i),(a),(o))
#define UcxEndpointInitSetEventCallbacks(i,cb) \
    ((PFN_UCXENDPOINTINITSETEVENTCALLBACKS)UcxClassFunctions[UcxEndpointInitSetEventCallbacksIndex])((i),(cb))
#define UcxEndpointSetWdfIoQueue(e,q) \
    ((PFN_UCXENDPOINTSETWDFIOQUEUE)UcxClassFunctions[UcxEndpointSetWdfIoQueueIndex])((e),(q))
#define UcxEndpointPurgeComplete(e) \
    ((PFN_UCXENDPOINTPURGECOMPLETE)UcxClassFunctions[UcxEndpointPurgeCompleteIndex])((e))
#define UcxEndpointNeedToCancelTransfers(e) \
    ((PFN_UCXENDPOINTNEEDTOCANCELTRANSFERS)UcxClassFunctions[UcxEndpointNeedToCancelTransfersIndex])((e))

/* ---- URB access helpers (M3) --------------------------------------------- */

/*
 * The URB of an IOCTL_INTERNAL_USB_SUBMIT_URB request handed to a client
 * callback. The class library has checked it is non-NULL and at least a header
 * long before the callback runs.
 */
FORCEINLINE PVOID UcxRequestGetUrb(_In_ WDFREQUEST Request)
{
    PIRP Irp = WdfRequestWdmGetIrp(Request);
    return IoGetCurrentIrpStackLocation(Irp)->Parameters.Others.Argument1;
}

/*
 * The system address of a transfer buffer given as a pointer or as an MDL.
 * Returns NULL for a zero length or an MDL that cannot be mapped; a caller
 * must never read or write more than Length bytes.
 */
FORCEINLINE PVOID UcxUrbTransferBuffer(_In_opt_ PVOID Buffer, _In_opt_ PMDL Mdl, _In_ ULONG Length)
{
    if (Length == 0)
        return NULL;
    if (Buffer != NULL)
        return Buffer;
    if (Mdl != NULL)
        return MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
    return NULL;
}
