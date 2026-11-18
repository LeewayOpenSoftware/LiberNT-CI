/*
 * PROJECT:     LiberNT Kernel-Mode Driver Framework
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     USB host Controller eXtension (UCX) class library -- M1 skeleton.
 *              Loads as a KMDF class library, registers the L"Ucx" class, and
 *              publishes the UCX DDI table so a client (usbxhci) can bind and
 *              create a UcxController. Object lifecycle and event wiring only;
 *              xHCI transfer/URB handling arrives in M2.
 * NOTE:        UCX DDI surface is a clean-room reconstruction from the public
 *              WDK headers (ucxclass.h et al). No Microsoft source was used.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <ntddk.h>
#include <ntstrsafe.h>
#include "wdf.h"
#include <fxldr.h>
#include "wdfcx.h"
#include "classlibrary.h"
#include <ucxclass.h>

#define UCX_TAG 'xCcU'

/*
 * UCX trace is debugger/COM-only (DbgPrintEx IHVDRIVER/INFO) AND compiled out of
 * free builds. Standing rule: UCX trace must never route to a user-readable sink
 * and must not persist on retail. It logs opaque WDFOBJECT pointers only under
 * DBG; this becomes load-bearing at M2 when device-influenced values are traced.
 */
#if DBG
#define UcxLog(_fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "ucx01000: " _fmt "\n", ##__VA_ARGS__)
#else
#define UcxLog(_fmt, ...) ((void)0)
#endif

/* --------------------------------------------------------------------------
 * DDI implementations (M1: object lifecycle + logging; no hardware I/O yet).
 * Each returns a live WDFOBJECT cast to the opaque UCX handle so the client's
 * create calls succeed and the enumeration path can be exercised in M2.
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
    WDF_OBJECT_ATTRIBUTES attribs;
    WDFOBJECT object;
    NTSTATUS status;

    *Controller = NULL;
    if (Config == NULL || Config->Size != sizeof(UCX_CONTROLLER_CONFIG))
        return STATUS_INVALID_PARAMETER;

    if (Attributes != NULL)
    {
        attribs = *Attributes;
    }
    else
    {
        WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
    }
    attribs.ParentObject = Device;

    status = WdfObjectCreate(&attribs, &object);
    if (!NT_SUCCESS(status))
    {
        UcxLog("UcxControllerCreate: WdfObjectCreate failed 0x%08lX", (ULONG)status);
        return status;
    }

    *Controller = (UCXCONTROLLER)object;
    UcxLog("UcxControllerCreate: controller %p, %u root hub(s)", object, Config->NumberOfRootHubs);
    return STATUS_SUCCESS;
}

static VOID NTAPI UcxControllerSetFailedInternal(_In_ UCXCONTROLLER Controller)
{ UcxLog("UcxControllerSetFailed: %p", Controller); }

static VOID NTAPI UcxControllerNeedsResetInternal(_In_ UCXCONTROLLER Controller)
{ UcxLog("UcxControllerNeedsReset: %p", Controller); }

static VOID NTAPI UcxControllerResetCompleteInternal(_In_ UCXCONTROLLER Controller, _In_ BOOLEAN Succeeded)
{ UcxLog("UcxControllerResetComplete: %p succeeded=%u", Controller, Succeeded); }

static
NTSTATUS
NTAPI
UcxRootHubCreateInternal(
    _In_ UCXCONTROLLER Controller,
    _In_ PUCX_ROOTHUB_CONFIG Config,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES Attributes,
    _Out_ UCXROOTHUB *RootHub)
{
    WDF_OBJECT_ATTRIBUTES attribs;
    WDFOBJECT object;
    NTSTATUS status;

    *RootHub = NULL;
    if (Controller == NULL || Config == NULL || Config->Size != sizeof(UCX_ROOTHUB_CONFIG))
        return STATUS_INVALID_PARAMETER;

    if (Attributes != NULL)
        attribs = *Attributes;
    else
        WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
    attribs.ParentObject = (WDFOBJECT)Controller;

    status = WdfObjectCreate(&attribs, &object);
    if (!NT_SUCCESS(status))
    {
        UcxLog("UcxRootHubCreate: WdfObjectCreate failed 0x%08lX", (ULONG)status);
        return status;
    }

    *RootHub = (UCXROOTHUB)object;
    UcxLog("UcxRootHubCreate: roothub %p (%u USB2 + %u USB3 ports)",
           object, Config->NumberOf20Ports, Config->NumberOf30Ports);
    return STATUS_SUCCESS;
}

static VOID NTAPI UcxRootHubPortChangedInternal(_In_ UCXROOTHUB RootHub)
{ UcxLog("UcxRootHubPortChanged: %p", RootHub); }

static
NTSTATUS
NTAPI
UcxUsbDeviceCreateInternal(
    _In_ UCXCONTROLLER Controller,
    _Inout_ PUCXUSBDEVICE_INIT *UsbDeviceInit,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES Attributes,
    _Out_ UCXUSBDEVICE *UsbDevice)
{
    WDF_OBJECT_ATTRIBUTES attribs;
    WDFOBJECT object;
    NTSTATUS status;

    *UsbDevice = NULL;
    if (Controller == NULL)
        return STATUS_INVALID_PARAMETER;

    if (Attributes != NULL)
        attribs = *Attributes;
    else
        WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
    attribs.ParentObject = (WDFOBJECT)Controller;

    status = WdfObjectCreate(&attribs, &object);
    if (!NT_SUCCESS(status))
    {
        UcxLog("UcxUsbDeviceCreate: WdfObjectCreate failed 0x%08lX", (ULONG)status);
        return status;
    }

    if (UsbDeviceInit != NULL)
        *UsbDeviceInit = NULL;   /* M1: init blob consumed, nothing to retain yet */

    *UsbDevice = (UCXUSBDEVICE)object;
    UcxLog("UcxUsbDeviceCreate: usbdevice %p", object);
    return STATUS_SUCCESS;
}

static VOID NTAPI UcxUsbDeviceInitSetEventCallbacksInternal(
    _In_ PUCXUSBDEVICE_INIT Init, _In_ PUCX_USBDEVICE_EVENT_CALLBACKS Callbacks)
{ UNREFERENCED_PARAMETER(Init); UNREFERENCED_PARAMETER(Callbacks); UcxLog("UcxUsbDeviceInitSetEventCallbacks"); }

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
    WDF_OBJECT_ATTRIBUTES attribs;
    WDFOBJECT object;
    NTSTATUS status;

    *Endpoint = NULL;
    if (UsbDevice == NULL)
        return STATUS_INVALID_PARAMETER;

    if (Attributes != NULL)
        attribs = *Attributes;
    else
        WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
    attribs.ParentObject = (WDFOBJECT)UsbDevice;

    status = WdfObjectCreate(&attribs, &object);
    if (!NT_SUCCESS(status))
    {
        UcxLog("UcxEndpointCreate: WdfObjectCreate failed 0x%08lX", (ULONG)status);
        return status;
    }

    if (EndpointInit != NULL)
        *EndpointInit = NULL;

    *Endpoint = (UCXENDPOINT)object;
    UcxLog("UcxEndpointCreate: endpoint %p", object);
    return STATUS_SUCCESS;
}

static VOID NTAPI UcxEndpointInitSetEventCallbacksInternal(
    _In_ PUCXENDPOINT_INIT Init, _In_ PUCX_ENDPOINT_EVENT_CALLBACKS Callbacks)
{ UNREFERENCED_PARAMETER(Init); UNREFERENCED_PARAMETER(Callbacks); UcxLog("UcxEndpointInitSetEventCallbacks"); }

static VOID NTAPI UcxEndpointSetWdfIoQueueInternal(_In_ UCXENDPOINT Endpoint, _In_ WDFQUEUE Queue)
{ UcxLog("UcxEndpointSetWdfIoQueue: ep %p queue %p", Endpoint, Queue); }

static VOID NTAPI UcxEndpointPurgeCompleteInternal(_In_ UCXENDPOINT Endpoint)
{ UcxLog("UcxEndpointPurgeComplete: %p", Endpoint); }

static VOID NTAPI UcxEndpointNeedToCancelTransfersInternal(_In_ UCXENDPOINT Endpoint)
{ UcxLog("UcxEndpointNeedToCancelTransfers: %p", Endpoint); }

static VOID NTAPI UcxIoDeviceControlInternal(VOID)
{ UcxLog("UcxIoDeviceControl"); }

/* --------------------------------------------------------------------------
 * Published DDI table + class-library registration (mirror of ufx01000.c).
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
