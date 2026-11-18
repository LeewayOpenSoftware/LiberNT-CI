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

typedef struct _UCX_CONTROLLER_CONFIG {
    ULONG Size;
    USHORT NumberOfRootHubs;
    PFN_UCX_CONTROLLER_RESET                 EvtControllerReset;
    PFN_UCX_CONTROLLER_QUERY_USB_CAPABILITY  EvtControllerQueryUsbCapability;
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

typedef VOID (NTAPI *PFN_UCX_ROOTHUB_CONTROL_URB)(_In_ UCXROOTHUB, _In_ WDFREQUEST);
typedef VOID (NTAPI *PFN_UCX_ROOTHUB_GET_INFO)(_In_ UCXROOTHUB, _In_ WDFREQUEST);

typedef struct _UCX_ROOTHUB_CONFIG {
    ULONG Size;
    USHORT NumberOf20Ports;
    USHORT NumberOf30Ports;
    PFN_UCX_ROOTHUB_CONTROL_URB  EvtRootHubControlUrb;
    PFN_UCX_ROOTHUB_GET_INFO     EvtRootHubGetInfo;
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

typedef NTSTATUS (NTAPI *PFN_UCX_USBDEVICE_ENABLE)(_In_ UCXCONTROLLER, _In_ UCXUSBDEVICE);
typedef NTSTATUS (NTAPI *PFN_UCX_USBDEVICE_ADDRESS)(_In_ UCXCONTROLLER, _In_ UCXUSBDEVICE);

typedef struct _UCX_USBDEVICE_EVENT_CALLBACKS {
    ULONG Size;
    PFN_UCX_USBDEVICE_ENABLE   EvtUsbDeviceEnable;
    PFN_UCX_USBDEVICE_ADDRESS  EvtUsbDeviceAddress;
} UCX_USBDEVICE_EVENT_CALLBACKS, *PUCX_USBDEVICE_EVENT_CALLBACKS;

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

typedef struct _UCX_ENDPOINT_EVENT_CALLBACKS {
    ULONG Size;
    PFN_UCX_ENDPOINT_PURGE  EvtEndpointPurge;
    PFN_UCX_ENDPOINT_START  EvtEndpointStart;
    PFN_UCX_ENDPOINT_RESET  EvtEndpointReset;
} UCX_ENDPOINT_EVENT_CALLBACKS, *PUCX_ENDPOINT_EVENT_CALLBACKS;

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
