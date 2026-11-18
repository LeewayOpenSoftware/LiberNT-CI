/*
 * PROJECT:     LiberNT Kernel-Mode Driver Framework
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     USB host Controller eXtension (UCX) class library -- private header.
 *              Object contexts shared by ucx01000.c (DDI table, object lifetime)
 *              and ucxroothub.c (the root hub PDO the hub driver binds to).
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#pragma once

#include <ntddk.h>
#include <ntstrsafe.h>
#include "wdf.h"
#include <fxldr.h>
#include "wdfcx.h"
#include <usb100.h>
#include <usb200.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <hubbusif.h>
#include <usbbusif.h>
#include <ucxclass.h>
#include "ucxurbcheck.h"

#define UCX_TAG 'xCcU'

/*
 * UCX trace is debugger/COM-only (DbgPrintEx IHVDRIVER/INFO) AND compiled out of
 * free builds. Standing rule: UCX trace must never route to a user-readable sink
 * and must not persist on retail. It logs opaque WDFOBJECT pointers only under
 * DBG; device-influenced values are traced only as numbers, never dereferenced.
 */
#if DBG
#define UcxLog(_fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "ucx01000: " _fmt "\n", ##__VA_ARGS__)
#else
#define UcxLog(_fmt, ...) ((void)0)
#endif

/* ---- device and pipe handles ---------------------------------------------
 * The hub driver addresses devices and pipes through opaque pointers it gets
 * back from us (PUSB_DEVICE_HANDLE, USBD_PIPE_HANDLE). A handle is never
 * dereferenced before it was found in the owning controller's list under its
 * lock (UcxpFindDeviceHandle); the signature is a second, cheap check.
 */
#define UCX_DEVICE_HANDLE_SIGNATURE 'vDcU'
#define UCX_PIPE_HANDLE_SIGNATURE   'pPcU'

typedef struct _UCX_PIPE_HANDLE {
    ULONG Signature;
    UCXENDPOINT Endpoint;                  /* NULL for the root hub's status-change pipe */
    USB_ENDPOINT_DESCRIPTOR Descriptor;
    ULONG MaximumTransferSize;
    volatile LONG Closing;                 /* being torn down: no new transfers */
} UCX_PIPE_HANDLE, *PUCX_PIPE_HANDLE;

/*
 * A selected interface of a configured device (M3 step 3): the pipes of its
 * current alternate setting. The USBD_INTERFACE_HANDLE the class driver gets
 * is this record's address; pipe handles are the addresses of its pipes.
 * Owned by the device handle; replaced whole on SELECT_INTERFACE.
 */
#define UCX_INTERFACE_SIGNATURE     'fIcU'
typedef struct _UCX_INTERFACE {
    ULONG Signature;
    UCHAR InterfaceNumber;
    UCHAR AlternateSetting;
    UCHAR Class;
    UCHAR SubClass;
    UCHAR Protocol;
    UCHAR PipeCount;
    USHORT Reserved;
    UCX_PIPE_HANDLE Pipes[1];              /* PipeCount entries */
} UCX_INTERFACE, *PUCX_INTERFACE;
#define UCX_INTERFACE_SIZE(_pipes)     (FIELD_OFFSET(UCX_INTERFACE, Pipes) + ((_pipes) ? (_pipes) : 1) * sizeof(UCX_PIPE_HANDLE))
#define UCX_MAX_INTERFACES  32
#define UCX_MAX_PIPES       30             /* 15 IN + 15 OUT endpoints */

typedef struct _UCX_DEVICE_HANDLE {
    ULONG Signature;
    LIST_ENTRY Link;                       /* UCX_CONTROLLER_CTX.DeviceHandles */
    UCXUSBDEVICE UsbDevice;                /* NULL for the root hub itself */
    BOOLEAN IsRootHub;
    BOOLEAN Initialized;                   /* addressed, device descriptor read */
    USHORT PortNumber;                     /* root port (downstream devices) */
    USHORT DeviceAddress;
    USB_DEVICE_SPEED DeviceSpeed;
    ULONG MaxPacketSize0;
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    UCHAR ConfigurationValue;              /* 0 = unconfigured */
    UCX_PIPE_HANDLE DefaultPipe;           /* EP0 (unused for the root hub: standard
                                              requests are answered in software) */
    /* M3 step 3: the selected configuration (downstream devices) */
    ULONG ConfigurationCookie;             /* its address is the USBD configuration handle */
    ULONG InterfaceCount;
    PUCX_INTERFACE Interfaces[UCX_MAX_INTERFACES];
    PUSB_CONFIGURATION_DESCRIPTOR ConfigDescriptor;   /* our copy of the selected one */
    ULONG ConfigDescriptorLength;
    KSPIN_LOCK PipeLock;                   /* Interfaces[] and the pipes' Closing flags */
    volatile LONG Busy;                    /* callers inside a hub-interface or URB path */
    volatile LONG Children;                /* devices behind this hub (each holds the hub) */
    volatile LONG Removing;                /* RemoveUsbDevice ran: the last caller out destroys */
    volatile LONG Destroyed;               /* claimed once by whoever destroys */
    struct _UCX_ROOTHUB_CTX *RootHub;      /* for a destroy from a release */
    BOOLEAN InitializeFailed;              /* a failed Initialize is final for this handle */
    /* M3 step 4b: where it sits, and what it is when it is a hub */
    struct _UCX_DEVICE_HANDLE *Parent;     /* the hub it hangs from (holds a Busy count on it); NULL on a root port */
    UCX_USBDEVICE_PATH Path;
    BOOLEAN IsHub;                         /* bDeviceClass == hub */
    BOOLEAN HubInfoSent;                   /* the client's slot says "hub" */
    BOOLEAN MultiTt;                       /* Initialize20Hub said more than one TT */
    UCHAR HubPorts;                        /* from the hub descriptor */
    UCHAR TtThinkTime;
} UCX_DEVICE_HANDLE, *PUCX_DEVICE_HANDLE;

/*
 * Per-request context on a forwarded bulk/interrupt transfer (M3 step 3): the
 * pipe it was checked against and the MDL this library built when the caller
 * gave a buffer pointer only (freed at completion, the URB field restored).
 */
typedef struct _UCX_REQUEST_CTX {
    UCXENDPOINT Endpoint;
    PUCX_PIPE_HANDLE Pipe;
    PMDL OwnedMdl;
} UCX_REQUEST_CTX, *PUCX_REQUEST_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(UCX_REQUEST_CTX, UcxGetRequestCtx)

/*
 * The opaque init blobs the client fills between the Add callback and the
 * Create call. Allocated and freed by this library.
 */
#define UCX_USBDEVICE_INIT_SIGNATURE 'iDcU'
#define UCX_ENDPOINT_INIT_SIGNATURE  'iEcU'

struct _UCXUSBDEVICE_INIT {
    ULONG Signature;
    UCXCONTROLLER Controller;
    PUCX_DEVICE_HANDLE Handle;
    UCX_USBDEVICE_EVENT_CALLBACKS Callbacks;
    UCXUSBDEVICE Created;                  /* set by UcxUsbDeviceCreate */
};

struct _UCXENDPOINT_INIT {
    ULONG Signature;
    UCXUSBDEVICE UsbDevice;
    USB_ENDPOINT_DESCRIPTOR Descriptor;
    UCX_ENDPOINT_EVENT_CALLBACKS Callbacks;
    UCXENDPOINT Created;                   /* set by UcxEndpointCreate */
};

/* ---- object contexts ----------------------------------------------------- */

typedef struct _UCX_CONTROLLER_CTX {
    WDFDEVICE ClientDevice;                /* the client's (xHCI) FDO */
    UCX_CONTROLLER_CONFIG Config;
    UCXROOTHUB RootHub;                    /* the single root hub, once created */
    KSPIN_LOCK DeviceHandleLock;
    LIST_ENTRY DeviceHandles;              /* every live UCX_DEVICE_HANDLE */
    ULONG DeviceHandleCount;
} UCX_CONTROLLER_CTX, *PUCX_CONTROLLER_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(UCX_CONTROLLER_CTX, UcxGetControllerCtx)

/*
 * Descriptors the root hub answers standard requests with. One block so a
 * GET_DESCRIPTOR(configuration) copies it with a single bounded copy. The
 * three USB descriptor types are byte-packed by their own header; the assert
 * keeps the block that way (9 + 9 + 7 bytes, no padding).
 */
typedef struct _UCX_ROOTHUB_CONFIG_BLOCK {
    USB_CONFIGURATION_DESCRIPTOR Configuration;
    USB_INTERFACE_DESCRIPTOR Interface;
    USB_ENDPOINT_DESCRIPTOR StatusChangeEndpoint;
} UCX_ROOTHUB_CONFIG_BLOCK, *PUCX_ROOTHUB_CONFIG_BLOCK;

C_ASSERT(sizeof(UCX_ROOTHUB_CONFIG_BLOCK) == 25);

typedef struct _UCX_ROOTHUB_CTX {
    UCXCONTROLLER Controller;
    UCX_ROOTHUB_CONFIG Config;
    USHORT NumberOfPorts;                  /* 2.0 + 3.0 ports, as the hub driver sees them */

    WDFDEVICE Pdo;                         /* USB\ROOT_HUB30, child of the client FDO */
    WDFQUEUE SceQueue;                     /* manual queue: pended status-change transfers */
    WDFWORKITEM InitNotifyWorkItem;
    volatile LONG PortChangePending;       /* UcxRootHubPortChanged seen, bitmap not yet delivered */
    volatile LONG PdoMarkedMissing;

    /* The root hub as a USB device: handle, descriptors, the one pipe */
    UCX_DEVICE_HANDLE DeviceHandle;
    UCX_PIPE_HANDLE ScePipe;
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    UCX_ROOTHUB_CONFIG_BLOCK ConfigBlock;
    ULONG ConfigurationHandleCookie;       /* its address is the USBD configuration handle */

    /* Root hub init notification (USB_BUS_INTERFACE_HUB_V5.RootHubInitNotification) */
    KSPIN_LOCK Lock;
    PRH_INIT_CALLBACK InitCallback;
    PVOID InitCallbackContext;

    /* The bus interfaces the hub driver queries from the PDO */
    USB_BUS_INTERFACE_HUB_V5 HubInterface;
    USB_BUS_INTERFACE_USBDI_V2 UsbdiInterface;
} UCX_ROOTHUB_CTX, *PUCX_ROOTHUB_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(UCX_ROOTHUB_CTX, UcxGetRootHubCtx)

typedef struct _UCX_PDO_CTX {
    UCXROOTHUB RootHub;
} UCX_PDO_CTX, *PUCX_PDO_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(UCX_PDO_CTX, UcxGetPdoCtx)

typedef struct _UCX_USBDEVICE_CTX {
    UCXCONTROLLER Controller;
    PUCX_DEVICE_HANDLE Handle;             /* NULL for a device the client created on its own (M2 demo) */
    UCX_USBDEVICE_EVENT_CALLBACKS Callbacks;
    UCXENDPOINT DefaultEndpoint;
} UCX_USBDEVICE_CTX, *PUCX_USBDEVICE_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(UCX_USBDEVICE_CTX, UcxGetUsbDeviceCtx)

typedef struct _UCX_ENDPOINT_CTX {
    UCXUSBDEVICE UsbDevice;
    USB_ENDPOINT_DESCRIPTOR Descriptor;
    UCX_ENDPOINT_EVENT_CALLBACKS Callbacks;
    WDFQUEUE Queue;                        /* UcxEndpointSetWdfIoQueue: where transfers go (M3 step 3) */
    volatile LONG Transfers;               /* forwarded, not yet completed */
} UCX_ENDPOINT_CTX, *PUCX_ENDPOINT_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(UCX_ENDPOINT_CTX, UcxGetEndpointCtx)

/* ---- ucx01000.c ---------------------------------------------------------- */

NTSTATUS
UcxpCreateObject(
    _In_ WDFOBJECT Parent,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES ClientAttributes,
    _In_ PCWDF_OBJECT_CONTEXT_TYPE_INFO ContextType,
    _In_opt_ PFN_WDF_OBJECT_CONTEXT_CLEANUP EvtCleanup,
    _Out_ WDFOBJECT *Object);

VOID
UcxpAddDeviceHandle(
    _In_ PUCX_CONTROLLER_CTX Controller,
    _Inout_ PUCX_DEVICE_HANDLE Handle);

VOID
UcxpRemoveDeviceHandle(
    _In_ PUCX_CONTROLLER_CTX Controller,
    _Inout_ PUCX_DEVICE_HANDLE Handle);

/* TRUE when Handle is a live handle of this controller; dereferences nothing otherwise */
BOOLEAN
UcxpAcquireDeviceHandle(
    _In_ PUCX_CONTROLLER_CTX Controller,
    _In_opt_ PVOID Handle);

VOID
UcxpReleaseDeviceHandle(
    _In_ PUCX_DEVICE_HANDLE Handle);

VOID
UcxpDeviceTryDestroy(
    _In_ PUCX_DEVICE_HANDLE Handle);

/* The URB limits the dispatchers enforce (ucxroothub.c) */
extern const UCX_URB_LIMITS UcxpUrbLimits;

BOOLEAN
UcxpFindDeviceHandle(
    _In_ PUCX_CONTROLLER_CTX Controller,
    _In_opt_ PVOID Handle);

/* ---- ucxroothub.c -------------------------------------------------------- */

NTSTATUS
UcxpRootHubCreatePdo(
    _In_ UCXROOTHUB RootHub);

VOID
UcxpRootHubDestroyPdo(
    _In_ UCXROOTHUB RootHub);

VOID
UcxpRootHubPortChanged(
    _In_ UCXROOTHUB RootHub);

/* URB status to NTSTATUS and the common completion (shared with ucxdevice.c) */
NTSTATUS
UcxpUsbdToNtStatus(
    _In_ USBD_STATUS UsbdStatus);

VOID
UcxpCompleteUrb(
    _In_ WDFREQUEST Request,
    _Inout_ PURB Urb,
    _In_ USBD_STATUS UsbdStatus);

/* ---- ucxdevice.c (downstream devices, M3 step 2) -------------------------- */

NTSTATUS
UcxpDeviceCreate(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _In_opt_ PUCX_DEVICE_HANDLE Parent,
    _In_ USHORT PortNumber,
    _In_ USHORT PortStatus,
    _Outptr_ PUCX_DEVICE_HANDLE *NewHandle);

NTSTATUS
UcxpDeviceInitialize(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle);

NTSTATUS
UcxpDeviceGetDescriptors(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _In_ PUCX_DEVICE_HANDLE Handle,
    _Out_writes_bytes_to_opt_(*DeviceLength, *DeviceLength) PUCHAR DeviceBuffer,
    _Inout_ PULONG DeviceLength,
    _Out_writes_bytes_to_opt_(*ConfigLength, *ConfigLength) PUCHAR ConfigBuffer,
    _Inout_ PULONG ConfigLength);

VOID
UcxpDeviceRemove(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle);

VOID
UcxpDeviceDestroy(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle);

NTSTATUS
UcxpDeviceSetHubTt(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _Inout_ PUCX_DEVICE_HANDLE Handle,
    _In_ ULONG TtCount);

NTSTATUS
UcxpDeviceQueryInformation(
    _In_ PUCX_DEVICE_HANDLE Handle,
    _Out_writes_bytes_to_(BufferLength, *Returned) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG Returned);

VOID
UcxpDeviceFlushTransfers(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _In_ PUCX_DEVICE_HANDLE Handle);

VOID
UcxpDeviceHandleUrb(
    _In_ PUCX_ROOTHUB_CTX RootHub,
    _In_ PUCX_DEVICE_HANDLE Handle,
    _In_ WDFREQUEST Request,
    _Inout_ PURB Urb);
