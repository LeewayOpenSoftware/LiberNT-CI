/*
 * PROJECT:     LiberNT Kernel-Mode Driver Framework
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     USB host Controller eXtension (UCX) class library.
 *              Loads as a KMDF class library, registers the L"Ucx" class, and
 *              publishes the UCX DDI table a client (usbxhci) binds to. Owns the
 *              controller, root hub, USB device and endpoint objects and, from
 *              M3, the root hub PDO the hub driver binds to (ucxroothub.c).
 * NOTE:        UCX DDI surface is a clean-room reconstruction from the public
 *              WDK headers (ucxclass.h et al). No Microsoft source was used.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "ucxp.h"
#include "classlibrary.h"       /* WdfCxBindClient / WdfCxUnbindClient: this file only */

/* --------------------------------------------------------------------------
 * Object creation: our context first, the client's context (if any) on top
 * -------------------------------------------------------------------------- */

NTSTATUS
UcxpCreateObject(
    _In_ WDFOBJECT Parent,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES ClientAttributes,
    _In_ PCWDF_OBJECT_CONTEXT_TYPE_INFO ContextType,
    _In_opt_ PFN_WDF_OBJECT_CONTEXT_CLEANUP EvtCleanup,
    _Out_ WDFOBJECT *Object)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFOBJECT Created;
    NTSTATUS Status;

    *Object = NULL;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Parent;
    Attributes.ContextTypeInfo = ContextType;
    Attributes.EvtCleanupCallback = EvtCleanup;
    if (ClientAttributes != NULL)
    {
        /* Lifetime decisions stay with the client; the context is ours */
        Attributes.ExecutionLevel = ClientAttributes->ExecutionLevel;
        Attributes.SynchronizationScope = ClientAttributes->SynchronizationScope;
    }

    Status = WdfObjectCreate(&Attributes, &Created);
    if (!NT_SUCCESS(Status))
        return Status;

    if (ClientAttributes != NULL && ClientAttributes->ContextTypeInfo != NULL)
    {
        PVOID ClientContext;
        WDF_OBJECT_ATTRIBUTES ClientCopy = *ClientAttributes;

        ClientCopy.ParentObject = NULL;
        Status = WdfObjectAllocateContext(Created, &ClientCopy, &ClientContext);
        if (!NT_SUCCESS(Status))
        {
            WdfObjectDelete(Created);
            return Status;
        }
    }

    *Object = Created;
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Device handle registry (per controller)
 * -------------------------------------------------------------------------- */

VOID
UcxpAddDeviceHandle(
    _In_ PUCX_CONTROLLER_CTX Controller,
    _Inout_ PUCX_DEVICE_HANDLE Handle)
{
    KIRQL OldIrql;

    Handle->Signature = UCX_DEVICE_HANDLE_SIGNATURE;
    KeAcquireSpinLock(&Controller->DeviceHandleLock, &OldIrql);
    InsertTailList(&Controller->DeviceHandles, &Handle->Link);
    Controller->DeviceHandleCount++;
    KeReleaseSpinLock(&Controller->DeviceHandleLock, OldIrql);
}

VOID
UcxpRemoveDeviceHandle(
    _In_ PUCX_CONTROLLER_CTX Controller,
    _Inout_ PUCX_DEVICE_HANDLE Handle)
{
    KIRQL OldIrql;
    PLIST_ENTRY Entry;

    KeAcquireSpinLock(&Controller->DeviceHandleLock, &OldIrql);
    for (Entry = Controller->DeviceHandles.Flink; Entry != &Controller->DeviceHandles; Entry = Entry->Flink)
    {
        if (Entry == &Handle->Link)
        {
            RemoveEntryList(Entry);
            Controller->DeviceHandleCount--;
            Handle->Signature = 0;
            break;
        }
    }
    KeReleaseSpinLock(&Controller->DeviceHandleLock, OldIrql);
}

BOOLEAN
UcxpFindDeviceHandle(
    _In_ PUCX_CONTROLLER_CTX Controller,
    _In_opt_ PVOID Handle)
{
    KIRQL OldIrql;
    PLIST_ENTRY Entry;
    BOOLEAN Found = FALSE;

    if (Handle == NULL)
        return FALSE;

    /* Compare addresses only: nothing is read through Handle until it was found */
    KeAcquireSpinLock(&Controller->DeviceHandleLock, &OldIrql);
    for (Entry = Controller->DeviceHandles.Flink; Entry != &Controller->DeviceHandles; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, UCX_DEVICE_HANDLE, Link) == (PUCX_DEVICE_HANDLE)Handle)
        {
            Found = (((PUCX_DEVICE_HANDLE)Handle)->Signature == UCX_DEVICE_HANDLE_SIGNATURE);
            break;
        }
    }
    KeReleaseSpinLock(&Controller->DeviceHandleLock, OldIrql);
    return Found;
}

/* --------------------------------------------------------------------------
 * Controller
 * -------------------------------------------------------------------------- */

static
NTSTATUS
NTAPI
UcxControllerCreateInternal(
    _In_ WDFDEVICE Device,
    _In_ PUCX_CONTROLLER_CONFIG Config,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES Attributes,
    _Out_ UCXCONTROLLER *Controller)
{
    PUCX_CONTROLLER_CTX Ctx;
    WDFOBJECT Object;
    NTSTATUS Status;

    *Controller = NULL;
    if (Device == NULL || Config == NULL || Config->Size != sizeof(UCX_CONTROLLER_CONFIG))
        return STATUS_INVALID_PARAMETER;

    Status = UcxpCreateObject((WDFOBJECT)Device, Attributes,
                              WDF_GET_CONTEXT_TYPE_INFO(UCX_CONTROLLER_CTX), NULL, &Object);
    if (!NT_SUCCESS(Status))
    {
        UcxLog("UcxControllerCreate: object creation failed 0x%08lX", (ULONG)Status);
        return Status;
    }

    Ctx = UcxGetControllerCtx(Object);
    Ctx->ClientDevice = Device;
    Ctx->Config = *Config;
    Ctx->RootHub = NULL;
    KeInitializeSpinLock(&Ctx->DeviceHandleLock);
    InitializeListHead(&Ctx->DeviceHandles);
    Ctx->DeviceHandleCount = 0;

    *Controller = (UCXCONTROLLER)Object;
    UcxLog("UcxControllerCreate: controller %p, %u root hub(s), PCI %04X:%04X rev %04X",
           Object, Config->NumberOfRootHubs, Config->VendorId, Config->DeviceId, Config->RevisionId);
    return STATUS_SUCCESS;
}

static VOID NTAPI UcxControllerSetFailedInternal(_In_ UCXCONTROLLER Controller)
{ UcxLog("UcxControllerSetFailed: %p", Controller); }

static VOID NTAPI UcxControllerNeedsResetInternal(_In_ UCXCONTROLLER Controller)
{ UcxLog("UcxControllerNeedsReset: %p", Controller); }

static VOID NTAPI UcxControllerResetCompleteInternal(_In_ UCXCONTROLLER Controller, _In_ BOOLEAN Succeeded)
{ UcxLog("UcxControllerResetComplete: %p succeeded=%u", Controller, Succeeded); }

/* --------------------------------------------------------------------------
 * Root hub
 * -------------------------------------------------------------------------- */

static
VOID
NTAPI
UcxRootHubEvtCleanup(
    _In_ WDFOBJECT Object)
{
    PUCX_ROOTHUB_CTX Ctx = UcxGetRootHubCtx(Object);
    PUCX_CONTROLLER_CTX Controller;

    /* The client deleted the controller (or its device is going away): the
     * hub driver's PDO leaves with it */
    UcxpRootHubDestroyPdo((UCXROOTHUB)Object);

    if (Ctx->Controller != NULL)
    {
        Controller = UcxGetControllerCtx((WDFOBJECT)Ctx->Controller);
        if (Controller->RootHub == (UCXROOTHUB)Object)
            Controller->RootHub = NULL;
    }
}

static
NTSTATUS
NTAPI
UcxRootHubCreateInternal(
    _In_ UCXCONTROLLER Controller,
    _In_ PUCX_ROOTHUB_CONFIG Config,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES Attributes,
    _Out_ UCXROOTHUB *RootHub)
{
    PUCX_CONTROLLER_CTX ControllerCtx;
    PUCX_ROOTHUB_CTX Ctx;
    WDFOBJECT Object;
    NTSTATUS Status;

    *RootHub = NULL;
    if (Controller == NULL || Config == NULL || Config->Size != sizeof(UCX_ROOTHUB_CONFIG))
        return STATUS_INVALID_PARAMETER;

    ControllerCtx = UcxGetControllerCtx((WDFOBJECT)Controller);
    if (ControllerCtx->RootHub != NULL)
        return STATUS_INVALID_DEVICE_STATE;  /* one root hub per controller */

    Status = UcxpCreateObject((WDFOBJECT)Controller, Attributes,
                              WDF_GET_CONTEXT_TYPE_INFO(UCX_ROOTHUB_CTX), UcxRootHubEvtCleanup, &Object);
    if (!NT_SUCCESS(Status))
    {
        UcxLog("UcxRootHubCreate: object creation failed 0x%08lX", (ULONG)Status);
        return Status;
    }

    Ctx = UcxGetRootHubCtx(Object);
    Ctx->Controller = Controller;
    Ctx->Config = *Config;

    /* The hub driver's device: a child of the client's FDO */
    Status = UcxpRootHubCreatePdo((UCXROOTHUB)Object);
    if (!NT_SUCCESS(Status))
    {
        UcxLog("UcxRootHubCreate: root hub PDO failed 0x%08lX", (ULONG)Status);
        Ctx->Controller = NULL;              /* nothing to unhook in cleanup */
        WdfObjectDelete(Object);
        return Status;
    }

    ControllerCtx->RootHub = (UCXROOTHUB)Object;
    *RootHub = (UCXROOTHUB)Object;
    UcxLog("UcxRootHubCreate: roothub %p (%u USB2 + %u USB3 ports)",
           Object, Config->NumberOf20Ports, Config->NumberOf30Ports);
    return STATUS_SUCCESS;
}

static VOID NTAPI UcxRootHubPortChangedInternal(_In_ UCXROOTHUB RootHub)
{
    if (RootHub == NULL)
        return;
    UcxpRootHubPortChanged(RootHub);
}

/* --------------------------------------------------------------------------
 * USB device and endpoint objects (M3 step 2 wires their callbacks)
 * -------------------------------------------------------------------------- */

static
NTSTATUS
NTAPI
UcxUsbDeviceCreateInternal(
    _In_ UCXCONTROLLER Controller,
    _Inout_ PUCXUSBDEVICE_INIT *UsbDeviceInit,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES Attributes,
    _Out_ UCXUSBDEVICE *UsbDevice)
{
    PUCX_USBDEVICE_CTX Ctx;
    WDFOBJECT Object;
    NTSTATUS Status;

    *UsbDevice = NULL;
    if (Controller == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = UcxpCreateObject((WDFOBJECT)Controller, Attributes,
                              WDF_GET_CONTEXT_TYPE_INFO(UCX_USBDEVICE_CTX), NULL, &Object);
    if (!NT_SUCCESS(Status))
    {
        UcxLog("UcxUsbDeviceCreate: object creation failed 0x%08lX", (ULONG)Status);
        return Status;
    }

    Ctx = UcxGetUsbDeviceCtx(Object);
    Ctx->Controller = Controller;

    /* An init blob this library handed to EvtControllerUsbDeviceAdd carries the
     * callbacks and the device handle; the client's own creations (M2 demo)
     * come without one and get no callbacks. */
    if (UsbDeviceInit != NULL && *UsbDeviceInit != NULL)
    {
        struct _UCXUSBDEVICE_INIT *Init = *UsbDeviceInit;

        if (Init->Signature != UCX_USBDEVICE_INIT_SIGNATURE || Init->Controller != Controller || Init->Created != NULL)
        {
            WdfObjectDelete(Object);
            return STATUS_INVALID_PARAMETER;
        }
        Ctx->Handle = Init->Handle;
        Ctx->Callbacks = Init->Callbacks;
        Init->Created = (UCXUSBDEVICE)Object;
        *UsbDeviceInit = NULL;               /* consumed; ucxdevice.c frees it */
    }

    *UsbDevice = (UCXUSBDEVICE)Object;
    UcxLog("UcxUsbDeviceCreate: usbdevice %p", Object);
    return STATUS_SUCCESS;
}

static VOID NTAPI UcxUsbDeviceInitSetEventCallbacksInternal(
    _In_ PUCXUSBDEVICE_INIT Init, _In_ PUCX_USBDEVICE_EVENT_CALLBACKS Callbacks)
{
    if (Init == NULL || Callbacks == NULL || Init->Signature != UCX_USBDEVICE_INIT_SIGNATURE ||
        Callbacks->Size != sizeof(UCX_USBDEVICE_EVENT_CALLBACKS))
    {
        UcxLog("UcxUsbDeviceInitSetEventCallbacks: bad init or callbacks, ignored");
        return;
    }
    Init->Callbacks = *Callbacks;
}

static VOID NTAPI UcxUsbDeviceRemoteWakeNotificationInternal(_In_ UCXUSBDEVICE UsbDevice, _In_ NTSTATUS Status)
{ UcxLog("UcxUsbDeviceRemoteWakeNotification: %p 0x%08lX", UsbDevice, (ULONG)Status); }

static
NTSTATUS
NTAPI
UcxEndpointCreateInternal(
    _In_ UCXUSBDEVICE UsbDevice,
    _Inout_ PUCXENDPOINT_INIT *EndpointInit,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES Attributes,
    _Out_ UCXENDPOINT *Endpoint)
{
    PUCX_ENDPOINT_CTX Ctx;
    WDFOBJECT Object;
    NTSTATUS Status;

    *Endpoint = NULL;
    if (UsbDevice == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = UcxpCreateObject((WDFOBJECT)UsbDevice, Attributes,
                              WDF_GET_CONTEXT_TYPE_INFO(UCX_ENDPOINT_CTX), NULL, &Object);
    if (!NT_SUCCESS(Status))
    {
        UcxLog("UcxEndpointCreate: object creation failed 0x%08lX", (ULONG)Status);
        return Status;
    }

    Ctx = UcxGetEndpointCtx(Object);
    Ctx->UsbDevice = UsbDevice;

    if (EndpointInit != NULL && *EndpointInit != NULL)
    {
        struct _UCXENDPOINT_INIT *Init = *EndpointInit;

        if (Init->Signature != UCX_ENDPOINT_INIT_SIGNATURE || Init->UsbDevice != UsbDevice || Init->Created != NULL)
        {
            WdfObjectDelete(Object);
            return STATUS_INVALID_PARAMETER;
        }
        Ctx->Descriptor = Init->Descriptor;
        Ctx->Callbacks = Init->Callbacks;
        Init->Created = (UCXENDPOINT)Object;
        *EndpointInit = NULL;                /* consumed; ucxdevice.c frees it */
    }

    *Endpoint = (UCXENDPOINT)Object;
    UcxLog("UcxEndpointCreate: endpoint %p", Object);
    return STATUS_SUCCESS;
}

static VOID NTAPI UcxEndpointInitSetEventCallbacksInternal(
    _In_ PUCXENDPOINT_INIT Init, _In_ PUCX_ENDPOINT_EVENT_CALLBACKS Callbacks)
{
    if (Init == NULL || Callbacks == NULL || Init->Signature != UCX_ENDPOINT_INIT_SIGNATURE ||
        Callbacks->Size != sizeof(UCX_ENDPOINT_EVENT_CALLBACKS))
    {
        UcxLog("UcxEndpointInitSetEventCallbacks: bad init or callbacks, ignored");
        return;
    }
    Init->Callbacks = *Callbacks;
}

static VOID NTAPI UcxEndpointSetWdfIoQueueInternal(_In_ UCXENDPOINT Endpoint, _In_ WDFQUEUE Queue)
{
    if (Endpoint != NULL)
        UcxGetEndpointCtx((WDFOBJECT)Endpoint)->Queue = Queue;
    UcxLog("UcxEndpointSetWdfIoQueue: ep %p queue %p", Endpoint, Queue);
}

static VOID NTAPI UcxEndpointPurgeCompleteInternal(_In_ UCXENDPOINT Endpoint)
{ UcxLog("UcxEndpointPurgeComplete: %p", Endpoint); }

static VOID NTAPI UcxEndpointNeedToCancelTransfersInternal(_In_ UCXENDPOINT Endpoint)
{ UcxLog("UcxEndpointNeedToCancelTransfers: %p", Endpoint); }

static VOID NTAPI UcxIoDeviceControlInternal(VOID)
{ UcxLog("UcxIoDeviceControl"); }

/* --------------------------------------------------------------------------
 * Published DDI table + class-library registration (mirror of ufx01000.c).
 * The table order is the ABI the client stub binds to: append only.
 * -------------------------------------------------------------------------- */

static PVOID UcxFunctions[UcxFunctionsMax] =
{
    UcxControllerCreateInternal,
    UcxControllerSetFailedInternal,
    UcxControllerNeedsResetInternal,
    UcxControllerResetCompleteInternal,
    UcxRootHubCreateInternal,
    UcxRootHubPortChangedInternal,
    UcxUsbDeviceCreateInternal,
    UcxUsbDeviceInitSetEventCallbacksInternal,
    UcxUsbDeviceRemoteWakeNotificationInternal,
    UcxEndpointCreateInternal,
    UcxEndpointInitSetEventCallbacksInternal,
    UcxEndpointSetWdfIoQueueInternal,
    UcxEndpointPurgeCompleteInternal,
    UcxEndpointNeedToCancelTransfersInternal,
    UcxIoDeviceControlInternal
};

static
NTSTATUS
NTAPI
UcxLibraryBindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ClientGlobals)
{
    UcxLog("UcxLibraryBindClient: client bound");
    return WdfCxBindClient(ClassBindInfo, ClientGlobals,
                           (PVOID const *)UcxFunctions, RTL_NUMBER_OF(UcxFunctions), 1);
}

static
VOID
NTAPI
UcxLibraryUnbindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ClientGlobals)
{
    UNREFERENCED_PARAMETER(ClientGlobals);
    UcxLog("UcxLibraryUnbindClient: client unbound");
    WdfCxUnbindClient(ClassBindInfo);
}

static WDF_CLASS_LIBRARY_INFO UcxLibraryInfo =
{
    sizeof(WDF_CLASS_LIBRARY_INFO),
    {1, 0, 0},
    NULL,
    NULL,
    UcxLibraryBindClient,
    UcxLibraryUnbindClient
};

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UcxLog("DriverEntry: registering UCX host controller extension class library");
    return WdfCxRegisterLibrary(DriverObject, RegistryPath, L"\\Device\\Ucx01000", &UcxLibraryInfo);
}
