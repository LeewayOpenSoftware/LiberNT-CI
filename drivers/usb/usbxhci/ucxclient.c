/*
 * PROJECT:     LiberNT USB xHCI host controller driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     UCX (USB host controller extension) client path -- milestone M2.
 *              When Services\usbxhci\Parameters\UseUcx is 1 the xHCI PCI FDO is
 *              a KMDF WDFDEVICE owning the PnP/power/interrupt contract and the
 *              controller/root-hub are UCX objects (UcxControllerCreate /
 *              UcxRootHubCreate). The default (UseUcx absent or 0) keeps the
 *              legacy usbport miniport registration untouched.
 *
 *              M2 step 1: KMDF bind on the real FDO, BAR map, HCSPARAMS/HCCPARAMS,
 *              supported-protocol port map, UCX controller + root hub created in
 *              EvtDevicePrepareHardware. No controller run, no enumeration yet.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "usbxhci.h"
#include <wdf.h>
#include <ucxclass.h>

#define NDEBUG
#include <debug.h>

/*
 * Step markers the lab leg greps on COM1. They carry counts and status codes
 * only -- never kernel addresses, never device-supplied strings (SEC-0 trace
 * rule). Opaque object handles are traced under DBG only.
 */
#define XHCI_UCX_LOG(_fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "M2-UCX: " _fmt "\n", ##__VA_ARGS__)
#if DBG
#define XHCI_UCX_TRACE(_fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "M2-UCX: " _fmt "\n", ##__VA_ARGS__)
#else
#define XHCI_UCX_TRACE(_fmt, ...) ((void)0)
#endif

#define XHCI_UCX_MAX_EXT_CAPS 64
#define XHCI_UCX_RT_INTERRUPTER0_END (0x20 + sizeof(XHCI_INTERRUPTER_REGISTER_SET))

/* Same MMIO accessor shape as usbxhci.c (volatile register -> PULONG) */
#define XHCI_UCX_READ_REG(_reg)        READ_REGISTER_ULONG((PULONG)(ULONG_PTR)(_reg))
#define XHCI_UCX_WRITE_REG(_reg, _val) WRITE_REGISTER_ULONG((PULONG)(ULONG_PTR)(_reg), (_val))

/* Supplied by the KMDF client stub library (wdfdriverentry_1_33). */
NTSTATUS
NTAPI
FxDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath);

typedef struct _XHCI_UCX_DEVICE
{
    WDFDEVICE Device;
    WDFINTERRUPT Interrupt;
    UCXCONTROLLER Controller;
    UCXROOTHUB RootHub;

    /* BAR0 mapping (EvtDevicePrepareHardware .. EvtDeviceReleaseHardware) */
    PUCHAR MmioBase;
    SIZE_T MmioLength;
    PXHCI_CAPABILITY_REGISTERS CapabilityRegisters;
    PXHCI_OPERATIONAL_REGISTERS OperationalRegisters;
    PXHCI_RUNTIME_REGISTERS RuntimeRegisters;
    PXHCI_DOORBELL_ARRAY DoorbellArray;

    /* Capability snapshot (read once per PrepareHardware) */
    ULONG CapabilityLength;
    USHORT HciVersion;
    ULONG HcsParams1;
    ULONG HcsParams2;
    ULONG HcsParams3;
    ULONG HccParams;
    ULONG MaxSlots;
    ULONG MaxInterrupters;
    ULONG NumberOfPorts;
    USHORT NumberOf20Ports;
    USHORT NumberOf30Ports;
    UCHAR PortProtocol[XHCI_MAX_PORTS + 1];   /* 1-based; 0 = unknown, 2 = USB2, 3 = USB3 */

    BOOLEAN MessageInterrupt;
    BOOLEAN HardwarePrepared;
    BOOLEAN ControllerRunning;                 /* set by step 2 (EvtDeviceD0Entry) */
    volatile LONG PendingUsbSts;               /* USBSTS bits latched by the ISR for the DPC */
} XHCI_UCX_DEVICE, *PXHCI_UCX_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XHCI_UCX_DEVICE, XhciUcxGetDevice)

/* -1 = not read yet, 0 = legacy usbport, 1 = UCX client path */
static LONG g_XhciUcxSwitch = -1;
/* Non-zero while FxDriverEntry is on the stack (it re-enters DriverEntry). */
static LONG g_XhciUcxInsideWdfBind = 0;

/* --------------------------------------------------------------------------
 * Registry switch: Services\usbxhci\Parameters\UseUcx (REG_DWORD, default 0)
 * -------------------------------------------------------------------------- */

static
NTSTATUS
NTAPI
XhciUcxQueryUseUcx(
    _In_ PWSTR ValueName,
    _In_ ULONG ValueType,
    _In_ PVOID ValueData,
    _In_ ULONG ValueLength,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID EntryContext)
{
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(Context);

    /* Only a well-formed REG_DWORD can turn the UCX path on. */
    if (ValueType == REG_DWORD && ValueLength == sizeof(ULONG) &&
        ValueData != NULL && EntryContext != NULL)
    {
        *(PULONG)EntryContext = *(PULONG)ValueData;
    }

    return STATUS_SUCCESS;
}

BOOLEAN
XHCI_UcxSwitchEnabled(
    _In_ PUNICODE_STRING RegistryPath)
{
    static const WCHAR ParametersSuffix[] = L"\\Parameters";
    RTL_QUERY_REGISTRY_TABLE Table[2];
    UNICODE_STRING ParametersPath;
    ULONG UseUcx = 0;
    USHORT BufferLength;
    NTSTATUS Status;

    if (g_XhciUcxSwitch >= 0)
        return (g_XhciUcxSwitch != 0);

    if (RegistryPath == NULL || RegistryPath->Buffer == NULL || RegistryPath->Length == 0 ||
        RegistryPath->Length > (MAXUSHORT - sizeof(ParametersSuffix)))
    {
        g_XhciUcxSwitch = 0;
        return FALSE;
    }

    BufferLength = (USHORT)(RegistryPath->Length + sizeof(ParametersSuffix));
    ParametersPath.Buffer = ExAllocatePoolWithTag(PagedPool, BufferLength, XHCI_TAG);
    if (ParametersPath.Buffer == NULL)
    {
        g_XhciUcxSwitch = 0;
        return FALSE;
    }
    ParametersPath.Length = 0;
    ParametersPath.MaximumLength = BufferLength;
    RtlCopyUnicodeString(&ParametersPath, RegistryPath);
    Status = RtlAppendUnicodeToString(&ParametersPath, ParametersSuffix);
    if (NT_SUCCESS(Status))
    {
        /* RTL_REGISTRY_ABSOLUTE takes a NUL-terminated path; one WCHAR was reserved */
        ParametersPath.Buffer[ParametersPath.Length / sizeof(WCHAR)] = UNICODE_NULL;

        RtlZeroMemory(Table, sizeof(Table));
        Table[0].QueryRoutine = XhciUcxQueryUseUcx;
        Table[0].Flags = RTL_QUERY_REGISTRY_REQUIRED;
        Table[0].Name = L"UseUcx";
        Table[0].EntryContext = &UseUcx;

        Status = RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
                                        ParametersPath.Buffer,
                                        Table,
                                        NULL,
                                        NULL);
        if (!NT_SUCCESS(Status))
            UseUcx = 0;   /* key or value absent: legacy */
    }
    ExFreePoolWithTag(ParametersPath.Buffer, XHCI_TAG);

    g_XhciUcxSwitch = (UseUcx == 1) ? 1 : 0;
    if (g_XhciUcxSwitch)
        XHCI_UCX_LOG("UseUcx=1: selecting the UCX client path (legacy usbport is the fallback)");

    return (g_XhciUcxSwitch != 0);
}

/* --------------------------------------------------------------------------
 * Bounded MMIO helpers (BAR length is the trust boundary for every offset)
 * -------------------------------------------------------------------------- */

static
BOOLEAN
XhciUcxRangeInBar(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG Offset,
    _In_ SIZE_T Length)
{
    if (Dev->MmioBase == NULL || Length == 0)
        return FALSE;
    if ((SIZE_T)Offset > Dev->MmioLength)
        return FALSE;
    return (Length <= Dev->MmioLength - Offset);
}

static
ULONG
XhciUcxReadMmio(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG Offset)
{
    return XHCI_UCX_READ_REG(Dev->MmioBase + Offset);
}

/*
 * Walk the xHCI extended-capability list and build the per-port protocol map
 * (Supported Protocol capability, id 2). Every hop is bounds-checked against
 * the BAR length and the walk is capped, so a malformed capability chain
 * cannot take the driver outside the mapping or into an infinite loop.
 */
static
VOID
XhciUcxBuildProtocolPortMap(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    ULONG Offset;
    ULONG Hops = 0;

    RtlZeroMemory(Dev->PortProtocol, sizeof(Dev->PortProtocol));
    Dev->NumberOf20Ports = 0;
    Dev->NumberOf30Ports = 0;

    Offset = XHCI_HCC_EXT_CAP_PTR(Dev->HccParams) << 2;   /* dword units -> bytes */

    while (Offset != 0 && Hops++ < XHCI_UCX_MAX_EXT_CAPS)
    {
        ULONG CapHeader;
        ULONG Next;

        if (!XhciUcxRangeInBar(Dev, Offset, sizeof(XHCI_PROTOCOL_CAPABILITY)))
            break;

        CapHeader = XhciUcxReadMmio(Dev, Offset);

        if (XHCI_EXT_CAP_ID(CapHeader) == XHCI_EXT_CAP_ID_PROTOCOL)
        {
            ULONG PortInfo = XhciUcxReadMmio(Dev, Offset + FIELD_OFFSET(XHCI_PROTOCOL_CAPABILITY, PortInfo));
            UCHAR Major = (UCHAR)XHCI_EXT_PORT_MAJOR(CapHeader);
            ULONG PortOffset = XHCI_EXT_PORT_OFFSET(PortInfo);
            ULONG PortCount = XHCI_EXT_PORT_COUNT(PortInfo);
            ULONG Index;

            for (Index = 0; Index < PortCount; Index++)
            {
                ULONG PortNumber = PortOffset + Index;
                UCHAR Protocol;

                if (PortNumber == 0 || PortNumber > Dev->NumberOfPorts || PortNumber > XHCI_MAX_PORTS)
                    continue;

                Protocol = (Major >= 3) ? 3 : 2;
                if (Protocol > Dev->PortProtocol[PortNumber])
                    Dev->PortProtocol[PortNumber] = Protocol;
            }
        }

        Next = XHCI_EXT_CAP_NEXT(CapHeader);
        if (Next == 0)
            break;
        Offset += Next << 2;
    }

    {
        ULONG Port;

        for (Port = 1; Port <= Dev->NumberOfPorts && Port <= XHCI_MAX_PORTS; Port++)
        {
            if (Dev->PortProtocol[Port] == 3)
                Dev->NumberOf30Ports++;
            else
                Dev->NumberOf20Ports++;   /* no protocol capability: treat as USB2 */
        }
    }
}

/* --------------------------------------------------------------------------
 * UCX controller / root-hub callbacks (step 1: minimal, no hardware action)
 * -------------------------------------------------------------------------- */

static
NTSTATUS
NTAPI
XhciUcxEvtControllerReset(
    _In_ UCXCONTROLLER Controller)
{
    UNREFERENCED_PARAMETER(Controller);
    XHCI_UCX_LOG("EvtControllerReset (no-op in step 1)");
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
XhciUcxEvtControllerQueryUsbCapability(
    _In_ UCXCONTROLLER Controller,
    _In_ PGUID CapabilityType,
    _In_ ULONG OutputBufferLength,
    _Out_opt_ PVOID OutputBuffer,
    _Out_ PULONG ResultLength)
{
    UNREFERENCED_PARAMETER(Controller);
    UNREFERENCED_PARAMETER(CapabilityType);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(OutputBuffer);

    *ResultLength = 0;
    return STATUS_NOT_SUPPORTED;
}

static
VOID
NTAPI
XhciUcxEvtRootHubControlUrb(
    _In_ UCXROOTHUB RootHub,
    _In_ WDFREQUEST Request)
{
    UNREFERENCED_PARAMETER(RootHub);
    /* Root-hub class requests arrive with step 2 (port status) */
    WdfRequestComplete(Request, STATUS_NOT_IMPLEMENTED);
}

static
VOID
NTAPI
XhciUcxEvtRootHubGetInfo(
    _In_ UCXROOTHUB RootHub,
    _In_ WDFREQUEST Request)
{
    UNREFERENCED_PARAMETER(RootHub);
    WdfRequestComplete(Request, STATUS_NOT_IMPLEMENTED);
}

/* --------------------------------------------------------------------------
 * WDFINTERRUPT: shared-line-safe ISR + DPC skeleton (event ring drain = step 2)
 * -------------------------------------------------------------------------- */

static
BOOLEAN
NTAPI
XhciUcxEvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetDevice(WdfInterruptGetDevice(Interrupt));
    ULONG UsbSts;
    ULONG AckMask;
    ULONG Iman;

    UNREFERENCED_PARAMETER(MessageID);

    if (Dev->OperationalRegisters == NULL || Dev->RuntimeRegisters == NULL)
        return FALSE;

    /*
     * Claim the interrupt only when the controller says it is ours: a
     * line-based INTx may be shared. MSI makes this check always-true-for-us.
     */
    UsbSts = XHCI_UCX_READ_REG(&Dev->OperationalRegisters->UsbSts);
    AckMask = UsbSts & (XHCI_USBSTS_EINT | XHCI_USBSTS_PCD | XHCI_USBSTS_HSE | XHCI_USBSTS_HCE);
    Iman = XHCI_UCX_READ_REG(&Dev->RuntimeRegisters->Interrupter[0].Iman);

    if (AckMask == 0 && (Iman & XHCI_IMAN_IP) == 0)
        return FALSE;

    /* USBSTS is write-1-to-clear; acknowledge EINT before IMAN.IP */
    if (AckMask != 0)
        XHCI_UCX_WRITE_REG(&Dev->OperationalRegisters->UsbSts, AckMask);

    /* IMAN.IP is write-1-to-clear, IMAN.IE is read/write: writing the value back clears IP and keeps IE */
    if (Iman & XHCI_IMAN_IP)
        XHCI_UCX_WRITE_REG(&Dev->RuntimeRegisters->Interrupter[0].Iman, Iman);

    InterlockedOr(&Dev->PendingUsbSts, (LONG)AckMask);
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

static
VOID
NTAPI
XhciUcxEvtInterruptDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetDevice(WdfInterruptGetDevice(Interrupt));
    ULONG Pending;

    UNREFERENCED_PARAMETER(AssociatedObject);

    Pending = (ULONG)InterlockedExchange(&Dev->PendingUsbSts, 0);
    XHCI_UCX_TRACE("DPC pending USBSTS=%08lx (event ring drain arrives with step 2)", Pending);
}

/* --------------------------------------------------------------------------
 * WDFDEVICE PnP/power contract (M2-PNP-POWER-CONTRACT.md section 2 / 7.1)
 * -------------------------------------------------------------------------- */

static
ULONGLONG
XhciUcxMemoryResourceLength(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor)
{
    if (Descriptor->Type == CmResourceTypeMemory)
        return Descriptor->u.Memory.Length;

    if (Descriptor->Type == CmResourceTypeMemoryLarge)
    {
        if (Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_40)
            return ((ULONGLONG)Descriptor->u.Memory40.Length40) << 8;
        if (Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_48)
            return ((ULONGLONG)Descriptor->u.Memory48.Length48) << 16;
        if (Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_64)
            return ((ULONGLONG)Descriptor->u.Memory64.Length64) << 32;
    }

    return 0;
}

static
VOID
XhciUcxUnmapHardware(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    if (Dev->MmioBase != NULL)
        MmUnmapIoSpace(Dev->MmioBase, Dev->MmioLength);

    Dev->MmioBase = NULL;
    Dev->MmioLength = 0;
    Dev->CapabilityRegisters = NULL;
    Dev->OperationalRegisters = NULL;
    Dev->RuntimeRegisters = NULL;
    Dev->DoorbellArray = NULL;
    Dev->HardwarePrepared = FALSE;
}

/*
 * Map BAR0 and take the capability snapshot. Every derived register block is
 * checked to lie inside the BAR before a pointer to it is published.
 */
static
NTSTATUS
XhciUcxMapHardware(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    ULONG Count = WdfCmResourceListGetCount(ResourcesTranslated);
    ULONG Index;
    ULONG CapHeader0;
    ULONG DbOffset;
    ULONG RtOffset;
    SIZE_T OperationalLength;

    Dev->MessageInterrupt = FALSE;

    for (Index = 0; Index < Count; Index++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Translated =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, Index);
        ULONGLONG Length;

        if (Translated == NULL)
            continue;

        if (Translated->Type == CmResourceTypeInterrupt)
        {
            if (Translated->Flags & CM_RESOURCE_INTERRUPT_MESSAGE)
                Dev->MessageInterrupt = TRUE;
            continue;
        }

        if (Translated->Type != CmResourceTypeMemory && Translated->Type != CmResourceTypeMemoryLarge)
            continue;

        if (Dev->MmioBase != NULL)
            continue;   /* xHCI exposes one MMIO BAR; keep the first */

        Length = XhciUcxMemoryResourceLength(Translated);
        if (Length < sizeof(XHCI_CAPABILITY_REGISTERS) || Length > MAXULONG)
            continue;

        Dev->MmioBase = MmMapIoSpace(Translated->u.Memory.Start, (SIZE_T)Length, MmNonCached);
        if (Dev->MmioBase == NULL)
        {
            XHCI_UCX_LOG("MmMapIoSpace failed for a %lu-byte BAR", (ULONG)Length);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Dev->MmioLength = (SIZE_T)Length;
    }

    if (Dev->MmioBase == NULL)
    {
        XHCI_UCX_LOG("no memory resource for the controller");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Dev->CapabilityRegisters = (PXHCI_CAPABILITY_REGISTERS)Dev->MmioBase;
    CapHeader0 = XhciUcxReadMmio(Dev, 0);
    Dev->CapabilityLength = CapHeader0 & 0xFF;
    Dev->HciVersion = (USHORT)(CapHeader0 >> 16);

    if (Dev->CapabilityLength < sizeof(XHCI_CAPABILITY_REGISTERS) || Dev->HciVersion == 0)
    {
        XHCI_UCX_LOG("invalid capability header (CAPLENGTH=%lu HCIVERSION=%04x)",
                     Dev->CapabilityLength, Dev->HciVersion);
        XhciUcxUnmapHardware(Dev);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    Dev->HcsParams1 = XhciUcxReadMmio(Dev, FIELD_OFFSET(XHCI_CAPABILITY_REGISTERS, HcsParams1));
    Dev->HcsParams2 = XhciUcxReadMmio(Dev, FIELD_OFFSET(XHCI_CAPABILITY_REGISTERS, HcsParams2));
    Dev->HcsParams3 = XhciUcxReadMmio(Dev, FIELD_OFFSET(XHCI_CAPABILITY_REGISTERS, HcsParams3));
    Dev->HccParams = XhciUcxReadMmio(Dev, FIELD_OFFSET(XHCI_CAPABILITY_REGISTERS, HccParams));
    DbOffset = XhciUcxReadMmio(Dev, FIELD_OFFSET(XHCI_CAPABILITY_REGISTERS, DbOff)) & ~0x3UL;
    RtOffset = XhciUcxReadMmio(Dev, FIELD_OFFSET(XHCI_CAPABILITY_REGISTERS, Rtsoff)) & ~0x1FUL;

    Dev->MaxSlots = XHCI_HCS1_MAX_SLOTS(Dev->HcsParams1);
    Dev->MaxInterrupters = XHCI_HCS1_MAX_INTERRUPTS(Dev->HcsParams1);
    Dev->NumberOfPorts = XHCI_HCS1_MAX_PORTS(Dev->HcsParams1);

    if (Dev->MaxSlots == 0 || Dev->NumberOfPorts == 0 || Dev->MaxInterrupters == 0)
    {
        XHCI_UCX_LOG("unsupported controller geometry (MaxSlots=%lu MaxPorts=%lu MaxIntrs=%lu)",
                     Dev->MaxSlots, Dev->NumberOfPorts, Dev->MaxInterrupters);
        XhciUcxUnmapHardware(Dev);
        return STATUS_NOT_SUPPORTED;
    }
    if (Dev->MaxSlots > XHCI_MAX_SLOTS)
        Dev->MaxSlots = XHCI_MAX_SLOTS;
    if (Dev->NumberOfPorts > XHCI_MAX_PORTS)
    {
        XHCI_UCX_LOG("clamping %lu ports to %u", Dev->NumberOfPorts, XHCI_MAX_PORTS);
        Dev->NumberOfPorts = XHCI_MAX_PORTS;
    }

    /* Operational block: fixed registers + one PORTSC set per port */
    OperationalLength = FIELD_OFFSET(XHCI_OPERATIONAL_REGISTERS, PortRegister) +
                        (SIZE_T)Dev->NumberOfPorts * sizeof(XHCI_PORT_REGISTER);
    if (!XhciUcxRangeInBar(Dev, Dev->CapabilityLength, OperationalLength) ||
        !XhciUcxRangeInBar(Dev, DbOffset, ((SIZE_T)Dev->MaxSlots + 1) * sizeof(ULONG)) ||
        !XhciUcxRangeInBar(Dev, RtOffset, XHCI_UCX_RT_INTERRUPTER0_END))
    {
        XHCI_UCX_LOG("register blocks exceed the BAR (CAPLENGTH=%lu DBOFF=%lu RTSOFF=%lu len=%lu)",
                     Dev->CapabilityLength, DbOffset, RtOffset, (ULONG)Dev->MmioLength);
        XhciUcxUnmapHardware(Dev);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    Dev->OperationalRegisters = (PXHCI_OPERATIONAL_REGISTERS)(Dev->MmioBase + Dev->CapabilityLength);
    Dev->DoorbellArray = (PXHCI_DOORBELL_ARRAY)(Dev->MmioBase + DbOffset);
    Dev->RuntimeRegisters = (PXHCI_RUNTIME_REGISTERS)(Dev->MmioBase + RtOffset);

    XhciUcxBuildProtocolPortMap(Dev);

    XHCI_UCX_LOG("xHCI %u.%02u BAR len=%lu CAPLENGTH=%lu MaxSlots=%lu MaxIntrs=%lu ports=%lu (USB2=%u USB3=%u) AC64=%lu CSZ=%lu %s interrupt",
                 (Dev->HciVersion >> 8) & 0xFF, Dev->HciVersion & 0xFF,
                 (ULONG)Dev->MmioLength, Dev->CapabilityLength,
                 Dev->MaxSlots, Dev->MaxInterrupters, Dev->NumberOfPorts,
                 Dev->NumberOf20Ports, Dev->NumberOf30Ports,
                 XHCI_HCC_64BIT_ADDR(Dev->HccParams), XHCI_HCC_64B_CONTEXT(Dev->HccParams),
                 Dev->MessageInterrupt ? "message" : "line");

    Dev->HardwarePrepared = TRUE;
    return STATUS_SUCCESS;
}

static
NTSTATUS
XhciUcxCreateUcxObjects(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    UCX_CONTROLLER_CONFIG ControllerConfig;
    UCX_ROOTHUB_CONFIG RootHubConfig;
    NTSTATUS Status;

    /* Objects persist across a stop/start rebalance; only ReleaseHardware's
     * device teardown (WDF parent deletion) ends their life (contract 7.1). */
    if (Dev->Controller != NULL)
        return STATUS_SUCCESS;

    UCX_CONTROLLER_CONFIG_INIT(&ControllerConfig);
    ControllerConfig.NumberOfRootHubs = 1;
    ControllerConfig.EvtControllerReset = XhciUcxEvtControllerReset;
    ControllerConfig.EvtControllerQueryUsbCapability = XhciUcxEvtControllerQueryUsbCapability;

    Status = UcxControllerCreate(Dev->Device, &ControllerConfig, WDF_NO_OBJECT_ATTRIBUTES, &Dev->Controller);
    XHCI_UCX_LOG("UcxControllerCreate rc=0x%08lX", (ULONG)Status);
    if (!NT_SUCCESS(Status))
    {
        Dev->Controller = NULL;
        return Status;
    }
    XHCI_UCX_TRACE("controller object %p", Dev->Controller);

    UCX_ROOTHUB_CONFIG_INIT(&RootHubConfig);
    RootHubConfig.NumberOf20Ports = Dev->NumberOf20Ports;
    RootHubConfig.NumberOf30Ports = Dev->NumberOf30Ports;
    RootHubConfig.EvtRootHubControlUrb = XhciUcxEvtRootHubControlUrb;
    RootHubConfig.EvtRootHubGetInfo = XhciUcxEvtRootHubGetInfo;

    Status = UcxRootHubCreate(Dev->Controller, &RootHubConfig, WDF_NO_OBJECT_ATTRIBUTES, &Dev->RootHub);
    XHCI_UCX_LOG("UcxRootHubCreate rc=0x%08lX ports USB2=%u USB3=%u",
                 (ULONG)Status, Dev->NumberOf20Ports, Dev->NumberOf30Ports);
    if (!NT_SUCCESS(Status))
    {
        Dev->RootHub = NULL;
        return Status;
    }
    XHCI_UCX_TRACE("root hub object %p", Dev->RootHub);

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
XhciUcxEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetDevice(Device);
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    Status = XhciUcxMapHardware(Dev, ResourcesTranslated);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Contract 7.1: CREATE the UCX objects here; the controller RUNS in D0Entry */
    Status = XhciUcxCreateUcxObjects(Dev);
    if (!NT_SUCCESS(Status))
    {
        XhciUcxUnmapHardware(Dev);
        return Status;
    }

    XHCI_UCX_LOG("STEP1 DONE: KMDF FDO bound, BAR mapped, controller+roothub created (controller not running)");
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
XhciUcxEvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetDevice(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    XhciUcxUnmapHardware(Dev);
    XHCI_UCX_LOG("ReleaseHardware: BAR unmapped");
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
XhciUcxEvtDeviceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetDevice(Device);

    UNREFERENCED_PARAMETER(Dev);

    /* Step 2 sets USBCMD.R/S and arms interrupter 0 here (contract 7.1 step 2) */
    XHCI_UCX_LOG("D0Entry from state %u (controller run deferred to step 2)", (ULONG)PreviousState);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
XhciUcxEvtDeviceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetDevice(Device);

    UNREFERENCED_PARAMETER(Dev);

    /* Step 5 quiesces (clear R/S, wait HCHalted) and branches on TargetState */
    XHCI_UCX_LOG("D0Exit to state %u", (ULONG)TargetState);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
XhciUcxEvtDeviceSurpriseRemoval(
    _In_ WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
    XHCI_UCX_LOG("SurpriseRemoval");
}

static
NTSTATUS
NTAPI
XhciUcxEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDF_INTERRUPT_CONFIG InterruptConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    PXHCI_UCX_DEVICE Dev;
    WDFDEVICE Device;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDevicePrepareHardware = XhciUcxEvtDevicePrepareHardware;
    PnpPowerCallbacks.EvtDeviceReleaseHardware = XhciUcxEvtDeviceReleaseHardware;
    PnpPowerCallbacks.EvtDeviceD0Entry = XhciUcxEvtDeviceD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit = XhciUcxEvtDeviceD0Exit;
    PnpPowerCallbacks.EvtDeviceSurpriseRemoval = XhciUcxEvtDeviceSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, XHCI_UCX_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
    {
        XHCI_UCX_LOG("WdfDeviceCreate failed 0x%08lX", (ULONG)Status);
        return Status;
    }

    Dev = XhciUcxGetDevice(Device);
    RtlZeroMemory(Dev, sizeof(*Dev));
    Dev->Device = Device;

    /* One WDFINTERRUPT over interrupter 0, allocation-agnostic (contract 7.3) */
    WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, XhciUcxEvtInterruptIsr, XhciUcxEvtInterruptDpc);
    Status = WdfInterruptCreate(Device, &InterruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &Dev->Interrupt);
    if (!NT_SUCCESS(Status))
    {
        XHCI_UCX_LOG("WdfInterruptCreate failed 0x%08lX", (ULONG)Status);
        return Status;
    }

    XHCI_UCX_LOG("EvtDeviceAdd: WDFDEVICE is the xHCI PCI FDO, WDFINTERRUPT created");
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Entry: the legacy DriverEntry stays the image entry point. With UseUcx=1 it
 * calls here; we run the KMDF client stub (FxDriverEntry) which binds WDF +
 * the Ucx class and re-enters DriverEntry; on that inner call we create the
 * WDFDRIVER. A failure anywhere unwinds the stub and the caller falls back to
 * the legacy registration, so the switch can never leave the system without
 * its xHCI driver.
 * -------------------------------------------------------------------------- */

NTSTATUS
XHCI_UcxDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath,
    _Out_ PBOOLEAN WdfReentry)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    if (g_XhciUcxInsideWdfBind == 0)
    {
        *WdfReentry = FALSE;

        InterlockedExchange(&g_XhciUcxInsideWdfBind, 1);
        Status = FxDriverEntry(DriverObject, RegistryPath);
        InterlockedExchange(&g_XhciUcxInsideWdfBind, 0);

        if (!NT_SUCCESS(Status))
            XHCI_UCX_LOG("WDF/Ucx bind failed 0x%08lX (class library Ucx01000 loaded?)", (ULONG)Status);
        else
            XHCI_UCX_LOG("UCX client path active");
        return Status;
    }

    /* Inner call from FxDriverEntry: WDF and the Ucx class are bound now */
    *WdfReentry = TRUE;

    WDF_DRIVER_CONFIG_INIT(&Config, XhciUcxEvtDeviceAdd);
    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
        XHCI_UCX_LOG("WdfDriverCreate failed 0x%08lX", (ULONG)Status);

    return Status;
}
