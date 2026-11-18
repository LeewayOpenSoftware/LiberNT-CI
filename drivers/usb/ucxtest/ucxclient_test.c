/*
 * PROJECT:     LiberNT USB host controller extension (UCX) -- M1 smoke test client
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Minimal KMDF client that binds the L"Ucx" class library and
 *              exercises the UCX DDI table (UcxControllerCreate / UcxRootHubCreate),
 *              logging each step to COM1. Proves M1: ucx01000.sys registers/loads,
 *              the client binds, and the published DDIs are callable. No hardware.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <ntddk.h>
#include "wdf.h"
#include <ucxclass.h>

#define SMOKE(_fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "M1-SMOKE: " _fmt "\n", ##__VA_ARGS__)

/* Client-side UCX callbacks. Never invoked in the no-hardware smoke, but must be
 * valid pointers in the config structs. */
static NTSTATUS NTAPI SmokeControllerReset(_In_ UCXCONTROLLER Controller)
{ UNREFERENCED_PARAMETER(Controller); return STATUS_SUCCESS; }

static NTSTATUS NTAPI SmokeControllerQueryUsbCapability(
    _In_ UCXCONTROLLER Controller, _In_ PGUID CapabilityType,
    _In_ ULONG OutputBufferLength, _Out_opt_ PVOID OutputBuffer, _Out_ PULONG ResultLength)
{
    UNREFERENCED_PARAMETER(Controller); UNREFERENCED_PARAMETER(CapabilityType);
    UNREFERENCED_PARAMETER(OutputBufferLength); UNREFERENCED_PARAMETER(OutputBuffer);
    *ResultLength = 0;
    return STATUS_NOT_SUPPORTED;
}

static VOID NTAPI SmokeRootHubControlUrb(_In_ UCXROOTHUB RootHub, _In_ WDFREQUEST Request)
{ UNREFERENCED_PARAMETER(RootHub); WdfRequestComplete(Request, STATUS_NOT_SUPPORTED); }

static VOID NTAPI SmokeRootHubGetInfo(_In_ UCXROOTHUB RootHub, _In_ WDFREQUEST Request)
{ UNREFERENCED_PARAMETER(RootHub); WdfRequestComplete(Request, STATUS_NOT_SUPPORTED); }

static VOID NTAPI SmokeEvtDriverUnload(_In_ WDFDRIVER Driver)
{ UNREFERENCED_PARAMETER(Driver); SMOKE("driver unload"); }

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    DECLARE_CONST_UNICODE_STRING(ctrlName, L"\\Device\\UcxSmokeClient");
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    WDF_DRIVER_CONFIG config;
    PWDFDEVICE_INIT deviceInit;
    WDFDRIVER driver;
    WDFDEVICE control;
    UCX_CONTROLLER_CONFIG controllerConfig;
    UCX_ROOTHUB_CONFIG rootHubConfig;
    UCXCONTROLLER controller;
    UCXROOTHUB rootHub;
    NTSTATUS status;

    SMOKE("DriverEntry: binding the UCX class library");

    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.DriverInitFlags = WdfDriverInitNonPnpDriver;
    config.EvtDriverUnload = SmokeEvtDriverUnload;

    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(status)) { SMOKE("WdfDriverCreate FAILED 0x%08lX", (ULONG)status); return status; }
    SMOKE("WdfDriverCreate ok (UCX class bind should have logged above)");

    deviceInit = WdfControlDeviceInitAllocate(driver, &sddl);
    if (deviceInit == NULL) { SMOKE("WdfControlDeviceInitAllocate FAILED"); return STATUS_INSUFFICIENT_RESOURCES; }
    (VOID)WdfDeviceInitAssignName(deviceInit, &ctrlName);

    status = WdfDeviceCreate(&deviceInit, WDF_NO_OBJECT_ATTRIBUTES, &control);
    if (!NT_SUCCESS(status)) { SMOKE("WdfDeviceCreate FAILED 0x%08lX", (ULONG)status); WdfDeviceInitFree(deviceInit); return status; }
    WdfControlFinishInitializing(control);

    /* Exercise the UCX DDI table through the bound function pointers. */
    UCX_CONTROLLER_CONFIG_INIT(&controllerConfig);
    controllerConfig.NumberOfRootHubs = 1;
    controllerConfig.EvtControllerReset = SmokeControllerReset;
    controllerConfig.EvtControllerQueryUsbCapability = SmokeControllerQueryUsbCapability;

    status = UcxControllerCreate(control, &controllerConfig, WDF_NO_OBJECT_ATTRIBUTES, &controller);
    SMOKE("UcxControllerCreate rc=0x%08lX controller=%p", (ULONG)status, controller);
    if (!NT_SUCCESS(status)) return status;

    UCX_ROOTHUB_CONFIG_INIT(&rootHubConfig);
    rootHubConfig.NumberOf20Ports = 1;
    rootHubConfig.NumberOf30Ports = 1;
    rootHubConfig.EvtRootHubControlUrb = SmokeRootHubControlUrb;
    rootHubConfig.EvtRootHubGetInfo = SmokeRootHubGetInfo;

    status = UcxRootHubCreate(controller, &rootHubConfig, WDF_NO_OBJECT_ATTRIBUTES, &rootHub);
    SMOKE("UcxRootHubCreate rc=0x%08lX roothub=%p", (ULONG)status, rootHub);
    if (!NT_SUCCESS(status)) return status;

    SMOKE("DONE pass: UCX registered, client bound, controller+roothub created");
    return STATUS_SUCCESS;
}
