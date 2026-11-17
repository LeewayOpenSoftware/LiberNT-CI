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
 *              EvtDevicePrepareHardware.
 *              M2 step 2: controller run in EvtDeviceD0Entry (PCI bus master,
 *              BIOS handoff, HCRST, DCBAA/command ring/event ring/ERST/scratchpads
 *              in a WDF common buffer, CONFIG/CRCR/DCBAAP), interrupter 0 armed
 *              once WDF has connected the WDFINTERRUPT, ISR/DPC event-ring drain
 *              with every device-written field validated before use, Port Status
 *              Change -> UcxRootHubPortChanged, quiesce before interrupts are
 *              disconnected. No device slots / transfers yet (steps 3-4).
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "usbxhci.h"
#include <wdf.h>
#include <wdmguid.h>
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
#define XHCI_UCX_STALL_US 10
#define XHCI_UCX_BIOS_HANDOFF_US (1000 * 1000)

/* Step 2 ring geometry: one command ring, one single-segment event ring */
#define XHCI_UCX_CMD_RING_TRBS 256
#define XHCI_UCX_EVT_RING_TRBS 256
#define XHCI_UCX_ERST_ENTRIES  1

#ifndef PCI_COMMAND_OFFSET
#define PCI_COMMAND_OFFSET 0x04
#endif
#define XHCI_UCX_PCI_STATUS_OFFSET      0x06
#define XHCI_UCX_PCI_STATUS_CAP_LIST    0x0010
#define XHCI_UCX_PCI_CAP_PTR_OFFSET     0x34
#define XHCI_UCX_PCI_CAP_ID_MSI         0x05
#define XHCI_UCX_PCI_CAP_ID_MSIX        0x11
#define XHCI_UCX_PCI_MSI_ENABLE         0x0001
#define XHCI_UCX_PCI_MSIX_ENABLE        0x8000

/* Same MMIO accessor shape as usbxhci.c (volatile register -> PULONG) */
#define XHCI_UCX_READ_REG(_reg)        READ_REGISTER_ULONG((PULONG)(ULONG_PTR)(_reg))
#define XHCI_UCX_WRITE_REG(_reg, _val) WRITE_REGISTER_ULONG((PULONG)(ULONG_PTR)(_reg), (_val))

#define XHCI_UCX_ROUND_PAGE(_x) XHCI_ALIGN_UP((SIZE_T)(_x), PAGE_SIZE)

/*
 * Step 3 enumeration: a bounded number of root-port devices, one EP0 ring and
 * one descriptor buffer each. Per slot: page 0 = output device context,
 * page 1 = input context, page 2 = EP0 transfer ring (1 KB) + descriptor buffer.
 */
#define XHCI_UCX_MAX_ENUM_SLOTS        8
#define XHCI_UCX_SLOT_PAGES            3
#define XHCI_UCX_EP0_RING_TRBS         64
#define XHCI_UCX_SLOT_EP0_RING_OFFSET  (2 * PAGE_SIZE)
#define XHCI_UCX_SLOT_DESC_OFFSET      (2 * PAGE_SIZE + XHCI_UCX_EP0_RING_TRBS * sizeof(XHCI_TRB))
#define XHCI_UCX_DESC_BUFFER_SIZE      512
#define XHCI_UCX_COMMAND_TIMEOUT_MS    500
#define XHCI_UCX_TRANSFER_TIMEOUT_MS   1000
#define XHCI_UCX_PORT_RESET_TIMEOUT_MS 500
#define XHCI_UCX_WAIT_SLICE_MS         10
#define XHCI_UCX_INPUT_CTX_ADD_SLOT    (1u << 0)
#define XHCI_UCX_INPUT_CTX_ADD_EP0     (1u << 1)
#define XHCI_UCX_SLOT_STATE_ADDRESS(_devstate) ((_devstate) & 0xFF)

/*
 * Setup Stage TRB Transfer Type (xHCI 1.2 table 6-26): 0 = no data stage,
 * 2 = OUT data stage, 3 = IN data stage. hardware.h's XHCI_TRB_TRT_* use
 * 1/2 for OUT/IN, which only works on controllers that ignore TRT (QEMU);
 * the UCX path follows the specification.
 */
#define XHCI_UCX_TRT_NO_DATA (0u << 16)
#define XHCI_UCX_TRT_OUT     (2u << 16)
#define XHCI_UCX_TRT_IN      (3u << 16)

/* Supplied by the KMDF client stub library (wdfdriverentry_1_33). */
NTSTATUS
NTAPI
FxDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath);

/* One enumerated root-port device (step 3) */
typedef struct _XHCI_UCX_SLOT
{
    BOOLEAN InUse;
    BOOLEAN Addressed;
    UCHAR SlotId;                 /* assigned by Enable Slot, 1..MaxSlots */
    UCHAR PortId;                 /* root port, 1..NumberOfPorts */
    UCHAR Speed;                  /* PORTSC speed id */
    UCHAR UsbAddress;             /* from the output slot context after Address Device */
    ULONG Ep0MaxPacket;
    ULONG Ep0Enqueue;
    ULONG Ep0Cycle;
    SIZE_T DeviceContextOffset;   /* offsets into the HC common buffer */
    SIZE_T InputContextOffset;
    SIZE_T Ep0RingOffset;
    SIZE_T DescriptorOffset;
    UCXUSBDEVICE UsbDevice;
    UCXENDPOINT Ep0;
} XHCI_UCX_SLOT, *PXHCI_UCX_SLOT;

/* Port enumeration state (indexed by root port) */
#define XHCI_UCX_PORT_IDLE      0
#define XHCI_UCX_PORT_ENUMERATED 1
#define XHCI_UCX_PORT_FAILED    2   /* stays failed until the device disconnects */

typedef struct _XHCI_UCX_DEVICE
{
    WDFDEVICE Device;
    WDFINTERRUPT Interrupt;
    UCXCONTROLLER Controller;
    UCXROOTHUB RootHub;

    /* PCI config access (absent on ACPI-enumerated controllers) */
    BUS_INTERFACE_STANDARD BusInterface;
    BOOLEAN HaveBusInterface;

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
    ULONG ScratchpadCount;
    USHORT NumberOf20Ports;
    USHORT NumberOf30Ports;
    UCHAR PortProtocol[XHCI_MAX_PORTS + 1];   /* 1-based; 0 = unknown, 2 = USB2, 3 = USB3 */
    BOOLEAN Supports64Bit;
    BOOLEAN PortPowerControl;

    /*
     * Host-controller DMA memory (one WDF common buffer, page-aligned blocks):
     * DCBAA | scratchpad pointer array | scratchpad pages | command ring |
     * event ring (single segment) | ERST. The hardware writes only the event
     * ring and the scratchpads; everything read back from it is validated.
     */
    WDFDMAENABLER DmaEnabler;
    WDFCOMMONBUFFER HcBuffer;
    PUCHAR HcVa;
    PHYSICAL_ADDRESS HcPa;
    SIZE_T HcLength;
    SIZE_T DcbaaOffset;
    SIZE_T ScratchArrayOffset;
    SIZE_T ScratchBufferOffset;
    SIZE_T CmdRingOffset;
    SIZE_T EvtRingOffset;
    SIZE_T ErstOffset;
    SIZE_T SlotAreaOffset;
    ULONG ContextSize;            /* 32 or 64 (HCCPARAMS.CSZ) */

    /* Ring software state */
    ULONG CmdRingEnqueue;
    ULONG CmdRingCycle;
    ULONG EvtRingDequeue;
    ULONG EvtRingCycle;
    KSPIN_LOCK EventRingLock;

    /*
     * Step 3 enumeration (serialized by one WDF work item at PASSIVE_LEVEL):
     * at most one command and one EP0 transfer outstanding; the DPC matches
     * completions by TRB pointer and signals the events.
     */
    WDFWORKITEM EnumWorkItem;
    XHCI_UCX_SLOT Slots[XHCI_UCX_MAX_ENUM_SLOTS];
    UCHAR PortEnumState[XHCI_MAX_PORTS + 1];
    KEVENT CommandEvent;
    ULONGLONG PendingCommandTrb;
    volatile LONG PendingCommandDone;
    ULONG PendingCommandCode;
    UCHAR PendingCommandSlot;
    KEVENT TransferEvent;
    ULONGLONG PendingDataTrb;
    ULONGLONG PendingStatusTrb;
    volatile LONG PendingTransferDone;
    ULONG PendingTransferCode;
    ULONG PendingTransferResidual;
    volatile LONG PortRescanNeeded;

    BOOLEAN MessageInterrupt;
    BOOLEAN HardwarePrepared;
    BOOLEAN ControllerRunning;
    BOOLEAN InterruptsArmed;
    BOOLEAN FatalError;
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

/* Poll a register until (Mask set) / (Mask clear); bounded by TimeoutUs. */
static
BOOLEAN
XhciUcxWaitRegisterBits(
    _In_ volatile ULONG *Reg,
    _In_ ULONG Mask,
    _In_ BOOLEAN WaitSet,
    _In_ ULONG TimeoutUs)
{
    ULONG Loops = (TimeoutUs + (XHCI_UCX_STALL_US - 1)) / XHCI_UCX_STALL_US;

    while (Loops--)
    {
        ULONG Value = XHCI_UCX_READ_REG(Reg);

        if (WaitSet ? ((Value & Mask) == Mask) : ((Value & Mask) == 0))
            return TRUE;

        KeStallExecutionProcessor(XHCI_UCX_STALL_US);
    }

    return FALSE;
}

/*
 * Find an xHCI extended capability by id. Every hop is bounds-checked against
 * the BAR length and the walk is capped, so a malformed capability chain
 * cannot take the driver outside the mapping or into an infinite loop.
 * Returns the byte offset of the capability header, or 0.
 */
static
ULONG
XhciUcxFindExtendedCapability(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ UCHAR CapabilityId,
    _In_ SIZE_T CapabilityLength)
{
    ULONG Offset = XHCI_HCC_EXT_CAP_PTR(Dev->HccParams) << 2;   /* dword units -> bytes */
    ULONG Hops = 0;

    while (Offset != 0 && Hops++ < XHCI_UCX_MAX_EXT_CAPS)
    {
        ULONG CapHeader;
        ULONG Next;

        if (!XhciUcxRangeInBar(Dev, Offset, CapabilityLength))
            return 0;

        CapHeader = XhciUcxReadMmio(Dev, Offset);
        if (XHCI_EXT_CAP_ID(CapHeader) == CapabilityId)
            return Offset;

        Next = XHCI_EXT_CAP_NEXT(CapHeader);
        if (Next == 0)
            break;
        Offset += Next << 2;
    }

    return 0;
}

/* Build the per-port protocol map from every Supported Protocol capability. */
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

    Offset = XHCI_HCC_EXT_CAP_PTR(Dev->HccParams) << 2;

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
 * PCI config space through BUS_INTERFACE_STANDARD (step 2)
 * -------------------------------------------------------------------------- */

static
BOOLEAN
XhciUcxPciRead(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    if (!Dev->HaveBusInterface || Dev->BusInterface.GetBusData == NULL)
        return FALSE;

    return (Dev->BusInterface.GetBusData(Dev->BusInterface.Context,
                                         PCI_WHICHSPACE_CONFIG,
                                         Buffer, Offset, Length) == Length);
}

static
BOOLEAN
XhciUcxPciWrite(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG Offset,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    if (!Dev->HaveBusInterface || Dev->BusInterface.SetBusData == NULL)
        return FALSE;

    return (Dev->BusInterface.SetBusData(Dev->BusInterface.Context,
                                         PCI_WHICHSPACE_CONFIG,
                                         Buffer, Offset, Length) == Length);
}

/*
 * TRUE when the device's MSI or MSI-X capability is enabled in config space,
 * i.e. the bus driver really switched the function to message interrupts.
 * Walks the PCI capability list with a hop cap.
 */
static
BOOLEAN
XhciUcxPciMessageInterruptsEnabled(
    _In_ PXHCI_UCX_DEVICE Dev)
{
    USHORT Status = 0;
    UCHAR CapPtr = 0;
    ULONG Hops = 0;

    if (!XhciUcxPciRead(Dev, XHCI_UCX_PCI_STATUS_OFFSET, &Status, sizeof(Status)) ||
        (Status & XHCI_UCX_PCI_STATUS_CAP_LIST) == 0 ||
        !XhciUcxPciRead(Dev, XHCI_UCX_PCI_CAP_PTR_OFFSET, &CapPtr, sizeof(CapPtr)))
    {
        return FALSE;
    }

    CapPtr &= 0xFC;
    while (CapPtr >= 0x40 && Hops++ < 48)
    {
        UCHAR Header[2];
        USHORT Control = 0;

        if (!XhciUcxPciRead(Dev, CapPtr, Header, sizeof(Header)))
            return FALSE;

        if (Header[0] == XHCI_UCX_PCI_CAP_ID_MSI &&
            XhciUcxPciRead(Dev, (ULONG)CapPtr + 2, &Control, sizeof(Control)) &&
            (Control & XHCI_UCX_PCI_MSI_ENABLE))
        {
            return TRUE;
        }
        if (Header[0] == XHCI_UCX_PCI_CAP_ID_MSIX &&
            XhciUcxPciRead(Dev, (ULONG)CapPtr + 2, &Control, sizeof(Control)) &&
            (Control & XHCI_UCX_PCI_MSIX_ENABLE))
        {
            return TRUE;
        }

        CapPtr = Header[1] & 0xFC;
    }

    return FALSE;
}

/*
 * Enable memory decode + bus mastering; when the function really runs on
 * MSI/MSI-X also set Interrupt Disable so an emulator that asserts INTx
 * alongside MSI (QEMU xhci-pci) cannot storm the shared line. A controller
 * without config space (ACPI-enumerated) is left as firmware set it up.
 */
static
NTSTATUS
XhciUcxEnableBusMaster(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    USHORT Command = 0;
    USHORT NewCommand;

    if (!Dev->HaveBusInterface)
    {
        XHCI_UCX_LOG("no PCI config space (ACPI-enumerated controller), keeping firmware MEM/bus-master setup");
        return STATUS_SUCCESS;
    }

    if (!XhciUcxPciRead(Dev, PCI_COMMAND_OFFSET, &Command, sizeof(Command)))
    {
        XHCI_UCX_LOG("PCI command register read failed");
        return STATUS_DEVICE_DATA_ERROR;
    }

    NewCommand = Command | PCI_ENABLE_MEMORY_SPACE | PCI_ENABLE_BUS_MASTER;
    if (Dev->MessageInterrupt && XhciUcxPciMessageInterruptsEnabled(Dev))
        NewCommand |= PCI_DISABLE_LEVEL_INTERRUPT;

    if (NewCommand != Command &&
        !XhciUcxPciWrite(Dev, PCI_COMMAND_OFFSET, &NewCommand, sizeof(NewCommand)))
    {
        XHCI_UCX_LOG("PCI command register write failed (cmd %04x -> %04x)", Command, NewCommand);
        return STATUS_DEVICE_DATA_ERROR;
    }

    XHCI_UCX_LOG("PCI command %04x -> %04x (MEM+BusMaster%s)", Command, NewCommand,
                 (NewCommand & PCI_DISABLE_LEVEL_INTERRUPT) ? ", INTx disabled for MSI" : "");
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Controller bring-up primitives (step 2) -- all MMIO offsets were validated
 * against the BAR in PrepareHardware; waits are bounded.
 * -------------------------------------------------------------------------- */

/* xHCI USB Legacy Support capability: take ownership from the BIOS, mute SMIs. */
static
VOID
XhciUcxBiosHandoff(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    ULONG Offset = XhciUcxFindExtendedCapability(Dev, XHCI_EXT_CAP_ID_LEGACY, 2 * sizeof(ULONG));
    ULONG Value;
    ULONG Retry;

    if (Offset == 0)
        return;

    Value = XhciUcxReadMmio(Dev, Offset + XHCI_LEGACY_SUPPORT_OFFSET);
    if ((Value & XHCI_HC_BIOS_OWNED) == 0)
        return;

    XHCI_UCX_WRITE_REG(Dev->MmioBase + Offset + XHCI_LEGACY_SUPPORT_OFFSET, Value | XHCI_HC_OS_OWNED);

    for (Retry = 0; Retry < XHCI_UCX_BIOS_HANDOFF_US / 100; Retry++)
    {
        Value = XhciUcxReadMmio(Dev, Offset + XHCI_LEGACY_SUPPORT_OFFSET);
        if ((Value & XHCI_HC_BIOS_OWNED) == 0)
            break;
        KeStallExecutionProcessor(100);
    }

    if (Value & XHCI_HC_BIOS_OWNED)
    {
        /* Tolerant like the legacy path and Windows: continue with shared control */
        XHCI_UCX_LOG("BIOS kept legacy ownership for %lu us, continuing with shared control",
                     (ULONG)XHCI_UCX_BIOS_HANDOFF_US);
        return;
    }

    Value = XhciUcxReadMmio(Dev, Offset + XHCI_LEGACY_CONTROL_OFFSET);
    Value &= ~(XHCI_LEGACY_DISABLE_SMI | XHCI_LEGACY_SMI_EVENTS);
    XHCI_UCX_WRITE_REG(Dev->MmioBase + Offset + XHCI_LEGACY_CONTROL_OFFSET, Value);
    XHCI_UCX_LOG("BIOS handoff done");
}

/* Halt (if running) + HCRST + wait CNR clear; clears latched status bits. */
static
NTSTATUS
XhciUcxResetController(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    volatile ULONG *UsbCmd = &Dev->OperationalRegisters->UsbCmd;
    volatile ULONG *UsbSts = &Dev->OperationalRegisters->UsbSts;
    ULONG Command = XHCI_UCX_READ_REG(UsbCmd);

    if (Command & XHCI_USBCMD_RS)
    {
        XHCI_UCX_WRITE_REG(UsbCmd, Command & ~XHCI_USBCMD_RS);
        if (!XhciUcxWaitRegisterBits(UsbSts, XHCI_USBSTS_HCH, TRUE, XHCI_WAIT_HALT_US))
        {
            XHCI_UCX_LOG("controller failed to halt before reset");
            return STATUS_DEVICE_HARDWARE_ERROR;
        }
    }

    XHCI_UCX_WRITE_REG(UsbCmd, XHCI_USBCMD_HCRST);
    if (!XhciUcxWaitRegisterBits(UsbCmd, XHCI_USBCMD_HCRST, FALSE, XHCI_WAIT_RESET_US))
    {
        XHCI_UCX_LOG("controller reset timed out");
        return STATUS_DEVICE_HARDWARE_ERROR;
    }
    if (!XhciUcxWaitRegisterBits(UsbSts, XHCI_USBSTS_CNR, FALSE, XHCI_WAIT_CNR_US))
    {
        XHCI_UCX_LOG("controller not ready after reset");
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    XHCI_UCX_WRITE_REG(UsbSts, XHCI_USBSTS_HCE | XHCI_USBSTS_HSE | XHCI_USBSTS_PCD | XHCI_USBSTS_EINT);
    return STATUS_SUCCESS;
}

/* Clear Run/Stop and wait for HCHalted (bounded). PASSIVE_LEVEL callers only. */
static
NTSTATUS
XhciUcxHaltController(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    volatile ULONG *UsbCmd = &Dev->OperationalRegisters->UsbCmd;
    volatile ULONG *UsbSts = &Dev->OperationalRegisters->UsbSts;
    ULONG Command = XHCI_UCX_READ_REG(UsbCmd);

    if (Command & XHCI_USBCMD_RS)
        XHCI_UCX_WRITE_REG(UsbCmd, Command & ~XHCI_USBCMD_RS);

    Dev->ControllerRunning = FALSE;

    if (!XhciUcxWaitRegisterBits(UsbSts, XHCI_USBSTS_HCH, TRUE, XHCI_WAIT_HALT_US))
    {
        XHCI_UCX_LOG("halt timed out");
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    return STATUS_SUCCESS;
}

/*
 * Allocate (once) the host-controller DMA memory as a WDF common buffer and
 * lay out the page-aligned blocks. Re-used across stop/start cycles.
 */
static
NTSTATUS
XhciUcxAllocateHcMemory(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    WDF_DMA_ENABLER_CONFIG DmaConfig;
    SIZE_T Offset = 0;
    NTSTATUS Status;

    Dev->DcbaaOffset = Offset;
    Offset += XHCI_UCX_ROUND_PAGE(((SIZE_T)Dev->MaxSlots + 1) * sizeof(ULONGLONG));
    Dev->ScratchArrayOffset = Offset;
    if (Dev->ScratchpadCount != 0)
        Offset += XHCI_UCX_ROUND_PAGE((SIZE_T)Dev->ScratchpadCount * sizeof(ULONGLONG));
    Dev->ScratchBufferOffset = Offset;
    Offset += (SIZE_T)Dev->ScratchpadCount * PAGE_SIZE;
    Dev->CmdRingOffset = Offset;
    Offset += XHCI_UCX_ROUND_PAGE(XHCI_UCX_CMD_RING_TRBS * sizeof(XHCI_TRB));
    Dev->EvtRingOffset = Offset;
    Offset += XHCI_UCX_ROUND_PAGE(XHCI_UCX_EVT_RING_TRBS * sizeof(XHCI_TRB));
    Dev->ErstOffset = Offset;
    Offset += XHCI_UCX_ROUND_PAGE(XHCI_UCX_ERST_ENTRIES * sizeof(XHCI_ERST_ENTRY));
    Dev->SlotAreaOffset = Offset;
    Offset += (SIZE_T)XHCI_UCX_MAX_ENUM_SLOTS * XHCI_UCX_SLOT_PAGES * PAGE_SIZE;

    if (Dev->HcBuffer != NULL)
    {
        if (Dev->HcLength >= Offset)
            return STATUS_SUCCESS;

        /* Geometry grew across a re-prepare: replace the buffer */
        WdfObjectDelete((WDFOBJECT)Dev->HcBuffer);
        Dev->HcBuffer = NULL;
        Dev->HcVa = NULL;
        Dev->HcLength = 0;
    }

    if (Dev->DmaEnabler == NULL)
    {
        WDF_DMA_ENABLER_CONFIG_INIT(&DmaConfig,
                                    Dev->Supports64Bit ? WdfDmaProfileScatterGather64
                                                       : WdfDmaProfileScatterGather,
                                    XHCI_BULK_MAX_TRANSFER_SIZE);
        Status = WdfDmaEnablerCreate(Dev->Device, &DmaConfig, WDF_NO_OBJECT_ATTRIBUTES, &Dev->DmaEnabler);
        if (!NT_SUCCESS(Status))
        {
            XHCI_UCX_LOG("WdfDmaEnablerCreate failed 0x%08lX", (ULONG)Status);
            Dev->DmaEnabler = NULL;
            return Status;
        }
    }

    Status = WdfCommonBufferCreate(Dev->DmaEnabler, Offset, WDF_NO_OBJECT_ATTRIBUTES, &Dev->HcBuffer);
    if (!NT_SUCCESS(Status))
    {
        XHCI_UCX_LOG("WdfCommonBufferCreate(%lu bytes) failed 0x%08lX", (ULONG)Offset, (ULONG)Status);
        Dev->HcBuffer = NULL;
        return Status;
    }

    Dev->HcVa = WdfCommonBufferGetAlignedVirtualAddress(Dev->HcBuffer);
    Dev->HcPa = WdfCommonBufferGetAlignedLogicalAddress(Dev->HcBuffer);
    Dev->HcLength = Offset;

    if (Dev->HcVa == NULL || (Dev->HcPa.QuadPart & (PAGE_SIZE - 1)) != 0 ||
        (!Dev->Supports64Bit && ((ULONGLONG)Dev->HcPa.QuadPart + Offset) > 0x100000000ULL))
    {
        XHCI_UCX_LOG("common buffer unusable (page-aligned=%u, 32-bit-reachable=%u)",
                     ((Dev->HcPa.QuadPart & (PAGE_SIZE - 1)) == 0) ? 1 : 0,
                     (((ULONGLONG)Dev->HcPa.QuadPart + Offset) <= 0x100000000ULL) ? 1 : 0);
        WdfObjectDelete((WDFOBJECT)Dev->HcBuffer);
        Dev->HcBuffer = NULL;
        Dev->HcVa = NULL;
        Dev->HcLength = 0;
        return STATUS_NOT_SUPPORTED;
    }

    XHCI_UCX_LOG("HC memory %lu bytes (DCBAA %lu entries, %lu scratchpads, cmd ring %u, event ring %u, %u device slots)",
                 (ULONG)Offset, Dev->MaxSlots + 1, Dev->ScratchpadCount,
                 XHCI_UCX_CMD_RING_TRBS, XHCI_UCX_EVT_RING_TRBS, XHCI_UCX_MAX_ENUM_SLOTS);
    return STATUS_SUCCESS;
}

static
PHYSICAL_ADDRESS
XhciUcxHcPhysical(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ SIZE_T Offset)
{
    PHYSICAL_ADDRESS Pa;

    Pa.QuadPart = Dev->HcPa.QuadPart + (LONGLONG)Offset;
    return Pa;
}

/*
 * Program the halted, reset controller: page size, DCBAA (+scratchpads),
 * command ring, CONFIG, interrupter 0 ERST/ERDP/IMOD. Interrupts stay masked.
 */
static
NTSTATUS
XhciUcxProgramController(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    PXHCI_OPERATIONAL_REGISTERS Op = Dev->OperationalRegisters;
    PXHCI_INTERRUPTER_REGISTER_SET Interrupter = &Dev->RuntimeRegisters->Interrupter[0];
    PULONGLONG Dcbaa = (PULONGLONG)(Dev->HcVa + Dev->DcbaaOffset);
    PXHCI_TRB CmdRing = (PXHCI_TRB)(Dev->HcVa + Dev->CmdRingOffset);
    PXHCI_ERST_ENTRY Erst = (PXHCI_ERST_ENTRY)(Dev->HcVa + Dev->ErstOffset);
    PHYSICAL_ADDRESS DcbaaPa = XhciUcxHcPhysical(Dev, Dev->DcbaaOffset);
    PHYSICAL_ADDRESS CmdRingPa = XhciUcxHcPhysical(Dev, Dev->CmdRingOffset);
    PHYSICAL_ADDRESS EvtRingPa = XhciUcxHcPhysical(Dev, Dev->EvtRingOffset);
    PHYSICAL_ADDRESS ErstPa = XhciUcxHcPhysical(Dev, Dev->ErstOffset);
    ULONG PageSizeSupported;
    ULONGLONG Crcr;

    PageSizeSupported = XHCI_UCX_READ_REG(&Op->PageSize);
    if ((PageSizeSupported & XHCI_PAGE_SIZE_4K) == 0)
    {
        XHCI_UCX_LOG("controller lacks 4K page-size support (PAGESIZE=%08lx)", PageSizeSupported);
        return STATUS_NOT_SUPPORTED;
    }
    XHCI_UCX_WRITE_REG(&Op->PageSize, XHCI_PAGE_SIZE_4K);

    RtlZeroMemory(Dev->HcVa, Dev->HcLength);

    /* DCBAA[0] = scratchpad pointer array (or 0 when the controller needs none) */
    if (Dev->ScratchpadCount != 0)
    {
        PULONGLONG ScratchArray = (PULONGLONG)(Dev->HcVa + Dev->ScratchArrayOffset);
        ULONGLONG ScratchBase = (ULONGLONG)XhciUcxHcPhysical(Dev, Dev->ScratchBufferOffset).QuadPart;
        ULONG Index;

        for (Index = 0; Index < Dev->ScratchpadCount; Index++)
            ScratchArray[Index] = ScratchBase + ((ULONGLONG)Index * PAGE_SIZE);

        Dcbaa[0] = (ULONGLONG)XhciUcxHcPhysical(Dev, Dev->ScratchArrayOffset).QuadPart;
    }

    /* Command ring: empty, producer cycle 1, link TRB back to the base with Toggle Cycle */
    Dev->CmdRingEnqueue = 0;
    Dev->CmdRingCycle = 1;
    CmdRing[XHCI_UCX_CMD_RING_TRBS - 1].Parameter1 = (ULONG)(CmdRingPa.QuadPart & 0xFFFFFFFF);
    CmdRing[XHCI_UCX_CMD_RING_TRBS - 1].Parameter2 = (ULONG)(CmdRingPa.QuadPart >> 32);
    CmdRing[XHCI_UCX_CMD_RING_TRBS - 1].Status = 0;
    CmdRing[XHCI_UCX_CMD_RING_TRBS - 1].Control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;

    /* Event ring: one segment, consumer cycle 1 */
    Dev->EvtRingDequeue = 0;
    Dev->EvtRingCycle = 1;
    Erst[0].RingSegmentBaseAddress = (ULONGLONG)EvtRingPa.QuadPart;
    Erst[0].RingSegmentSize = XHCI_UCX_EVT_RING_TRBS;
    Erst[0].Reserved = 0;

    KeMemoryBarrier();

    XHCI_UCX_WRITE_REG(&Op->DcbaapLow, (ULONG)(DcbaaPa.QuadPart & 0xFFFFFFFF));
    XHCI_UCX_WRITE_REG(&Op->DcbaapHigh, (ULONG)(DcbaaPa.QuadPart >> 32));

    Crcr = (ULONGLONG)CmdRingPa.QuadPart | XHCI_TRB_CYCLE;   /* RCS = 1 */
    XHCI_UCX_WRITE_REG(&Op->CrCrLow, (ULONG)(Crcr & 0xFFFFFFFF));
    XHCI_UCX_WRITE_REG(&Op->CrCrHigh, (ULONG)(Crcr >> 32));

    XHCI_UCX_WRITE_REG(&Op->Config, Dev->MaxSlots);

    XHCI_UCX_WRITE_REG(&Interrupter->Imod, XHCI_IMOD_DEFAULT);
    XHCI_UCX_WRITE_REG(&Interrupter->ErstSize, XHCI_UCX_ERST_ENTRIES);
    XHCI_UCX_WRITE_REG(&Interrupter->ErstBaseLow, (ULONG)(ErstPa.QuadPart & 0xFFFFFFFF));
    XHCI_UCX_WRITE_REG(&Interrupter->ErstBaseHigh, (ULONG)(ErstPa.QuadPart >> 32));
    XHCI_UCX_WRITE_REG(&Interrupter->ErdpLow, ((ULONG)(EvtRingPa.QuadPart & 0xFFFFFFFF)) | XHCI_ERDP_BUSY);
    XHCI_UCX_WRITE_REG(&Interrupter->ErdpHigh, (ULONG)(EvtRingPa.QuadPart >> 32));

    return STATUS_SUCCESS;
}

/* Set Run/Stop, wait for HCHalted to clear, refuse a controller that latches HCE. */
static
NTSTATUS
XhciUcxRunController(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    volatile ULONG *UsbCmd = &Dev->OperationalRegisters->UsbCmd;
    volatile ULONG *UsbSts = &Dev->OperationalRegisters->UsbSts;
    ULONG Command = XHCI_UCX_READ_REG(UsbCmd);
    ULONG Status;

    XHCI_UCX_WRITE_REG(UsbCmd, Command | XHCI_USBCMD_RS);

    if (!XhciUcxWaitRegisterBits(UsbSts, XHCI_USBSTS_HCH, FALSE, XHCI_WAIT_HALT_US))
    {
        XHCI_UCX_LOG("controller failed to leave the halted state");
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    Status = XHCI_UCX_READ_REG(UsbSts);
    if (Status & XHCI_USBSTS_HCE)
    {
        XHCI_UCX_LOG("host controller error latched after run (USBSTS=%08lx)", Status);
        (VOID)XhciUcxHaltController(Dev);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    Dev->ControllerRunning = TRUE;
    return STATUS_SUCCESS;
}

/*
 * Port power (PPC controllers). PORTSC mixes RW, RW1C and write-1-to-trigger
 * bits: mask out every change bit, PED (1 = disable!), PR/WPR (reset triggers)
 * and LWS before OR-ing PP back in.
 */
static
VOID
XhciUcxPowerOnPorts(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    ULONG Port;

    if (!Dev->PortPowerControl)
        return;

    for (Port = 1; Port <= Dev->NumberOfPorts; Port++)
    {
        volatile ULONG *PortSc = &Dev->OperationalRegisters->PortRegister[Port - 1].PortStatusAndControl;
        ULONG Value = XHCI_UCX_READ_REG(PortSc);

        if (Value & XHCI_PORTSC_PP)
            continue;

        Value &= ~(XHCI_PORTSC_CHANGE_MASK | XHCI_PORTSC_PED | XHCI_PORTSC_PR |
                   XHCI_PORTSC_WPR | XHCI_PORTSC_LWS);
        XHCI_UCX_WRITE_REG(PortSc, Value | XHCI_PORTSC_PP);
    }
}

/* IMAN.IE + USBCMD.INTE. IMAN.IP is RW1C; writing IE alone (bit 0 clear) leaves IP untouched. */
static
VOID
XhciUcxArmInterrupts(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ BOOLEAN Arm)
{
    PXHCI_INTERRUPTER_REGISTER_SET Interrupter = &Dev->RuntimeRegisters->Interrupter[0];
    volatile ULONG *UsbCmd = &Dev->OperationalRegisters->UsbCmd;
    ULONG Command = XHCI_UCX_READ_REG(UsbCmd);

    if (Arm)
    {
        Dev->InterruptsArmed = TRUE;
        KeMemoryBarrier();
        XHCI_UCX_WRITE_REG(&Interrupter->Iman, XHCI_IMAN_IE);
        XHCI_UCX_WRITE_REG(UsbCmd, Command | XHCI_USBCMD_INTE);
    }
    else
    {
        XHCI_UCX_WRITE_REG(UsbCmd, Command & ~XHCI_USBCMD_INTE);
        XHCI_UCX_WRITE_REG(&Interrupter->Iman, 0);
        (VOID)XHCI_UCX_READ_REG(&Interrupter->Iman);
        Dev->InterruptsArmed = FALSE;
    }
}

/* --------------------------------------------------------------------------
 * Event ring (step 2). The ring is device-written: every field is validated
 * on read before it indexes anything (SEC-3 validate-on-read).
 * -------------------------------------------------------------------------- */

static
BOOLEAN
XhciUcxPointerInCommandRing(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONGLONG Pointer)
{
    ULONGLONG Base = (ULONGLONG)XhciUcxHcPhysical(Dev, Dev->CmdRingOffset).QuadPart;

    return (Pointer >= Base &&
            Pointer < Base + (ULONGLONG)XHCI_UCX_CMD_RING_TRBS * sizeof(XHCI_TRB) &&
            (Pointer & 0xF) == 0);
}

/*
 * Read PORTSC for a validated port, report it to UCX and acknowledge the
 * change bits. Step 2 note: the controller driver acknowledges the W1C change
 * bits itself; from step 3 that moves to the root hub's ClearPortFeature path
 * so the hub driver sees them. Returns TRUE when a change was present.
 */
static
BOOLEAN
XhciUcxHandlePortChange(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG PortId,
    _In_ BOOLEAN FromEvent)
{
    volatile ULONG *PortScReg;
    ULONG PortSc;
    ULONG Changes;

    if (PortId == 0 || PortId > Dev->NumberOfPorts)
        return FALSE;

    PortScReg = &Dev->OperationalRegisters->PortRegister[PortId - 1].PortStatusAndControl;
    PortSc = XHCI_UCX_READ_REG(PortScReg);
    Changes = PortSc & XHCI_PORTSC_CHANGE_MASK;

    XHCI_UCX_LOG("port %lu %s PORTSC=%08lx connected=%lu enabled=%lu speed=%lu changes=%08lx usb%u",
                 PortId, FromEvent ? "status change event" : "scan",
                 PortSc,
                 (PortSc & XHCI_PORTSC_CCS) ? 1UL : 0UL,
                 (PortSc & XHCI_PORTSC_PED) ? 1UL : 0UL,
                 (PortSc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT,
                 Changes,
                 (PortId <= XHCI_MAX_PORTS && Dev->PortProtocol[PortId] == 3) ? 3 : 2);

    if (Changes != 0)
    {
        /* W1C: write only the change bits; RW bits written as 0 are untouched
         * except PED/PR/WPR which are write-1 only, so writing 0 is safe. */
        XHCI_UCX_WRITE_REG(PortScReg, Changes);
    }

    return (Changes != 0);
}

/*
 * Drain interrupter 0's event ring. Called at DISPATCH_LEVEL from the DPC and
 * once from D0EntryPostInterruptsEnabled. Returns TRUE when a root-hub port
 * change was seen (the caller notifies UCX outside the lock).
 */
static
BOOLEAN
XhciUcxDrainEventRing(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ BOOLEAN ScanPorts)
{
    PXHCI_TRB EventRing = (PXHCI_TRB)(Dev->HcVa + Dev->EvtRingOffset);
    PXHCI_INTERRUPTER_REGISTER_SET Interrupter = &Dev->RuntimeRegisters->Interrupter[0];
    ULONGLONG EvtRingPa = (ULONGLONG)XhciUcxHcPhysical(Dev, Dev->EvtRingOffset).QuadPart;
    BOOLEAN PortChanged = FALSE;
    ULONG Processed = 0;
    ULONGLONG Dequeue;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Dev->EventRingLock, &OldIrql);

    if (!Dev->ControllerRunning || Dev->FatalError || Dev->HcVa == NULL)
    {
        KeReleaseSpinLock(&Dev->EventRingLock, OldIrql);
        return FALSE;
    }

    while (Processed < XHCI_UCX_EVT_RING_TRBS)
    {
        PXHCI_TRB EventTrb = &EventRing[Dev->EvtRingDequeue];
        ULONG Control;
        ULONG TrbType;

        /* DMA memory, not MMIO: volatile read behind a barrier */
        KeMemoryBarrier();
        Control = *(volatile ULONG *)&EventTrb->Control;
        KeMemoryBarrier();

        if ((Control & XHCI_TRB_CYCLE) != Dev->EvtRingCycle)
            break;

        TrbType = (Control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;

        switch (TrbType)
        {
            case XHCI_TRB_TYPE_PORT_STATUS_CHANGE:
            {
                ULONG PortId = (*(volatile ULONG *)&EventTrb->Parameter1 >> 24) & 0xFF;

                if (PortId == 0 || PortId > Dev->NumberOfPorts)
                {
                    XHCI_UCX_LOG("ignoring port status change for out-of-range port %lu", PortId);
                    break;
                }
                if (XhciUcxHandlePortChange(Dev, PortId, TRUE))
                    PortChanged = TRUE;
                break;
            }

            case XHCI_TRB_TYPE_COMMAND_COMPLETION:
            {
                ULONGLONG CommandPointer = ((ULONGLONG)(*(volatile ULONG *)&EventTrb->Parameter2) << 32) |
                                           *(volatile ULONG *)&EventTrb->Parameter1;
                ULONG CompletionCode = XHCI_GET_COMPLETION_CODE(*(volatile ULONG *)&EventTrb->Status);
                ULONG SlotId = XHCI_TRB_TO_SLOT_ID(Control);

                if (!XhciUcxPointerInCommandRing(Dev, CommandPointer))
                {
                    XHCI_UCX_LOG("ignoring command completion outside our command ring (code=%lu slot=%lu)",
                                 CompletionCode, SlotId);
                    break;
                }
                if (Dev->PendingCommandTrb != 0 && CommandPointer == Dev->PendingCommandTrb &&
                    InterlockedCompareExchange(&Dev->PendingCommandDone, 0, 0) == 0)
                {
                    Dev->PendingCommandCode = CompletionCode;
                    Dev->PendingCommandSlot = (UCHAR)SlotId;
                    KeMemoryBarrier();
                    InterlockedExchange(&Dev->PendingCommandDone, 1);
                    KeSetEvent(&Dev->CommandEvent, IO_NO_INCREMENT, FALSE);
                    XHCI_UCX_TRACE("command completed code=%lu slot=%lu", CompletionCode, SlotId);
                    break;
                }
                XHCI_UCX_LOG("unexpected command completion code=%lu slot=%lu", CompletionCode, SlotId);
                break;
            }

            case XHCI_TRB_TYPE_TRANSFER_EVENT:
            {
                ULONG SlotId = XHCI_TRB_TO_SLOT_ID(Control);
                ULONG EndpointId = XHCI_TRB_TO_EP_ID(Control);
                ULONG EventStatus = *(volatile ULONG *)&EventTrb->Status;
                ULONGLONG TrbPointer = ((ULONGLONG)(*(volatile ULONG *)&EventTrb->Parameter2) << 32) |
                                       *(volatile ULONG *)&EventTrb->Parameter1;
                ULONG CompletionCode = XHCI_GET_COMPLETION_CODE(EventStatus);
                ULONG Residual = EventStatus & XHCI_TRB_LEN_MASK;

                if (SlotId == 0 || SlotId > Dev->MaxSlots || EndpointId == 0 || EndpointId > 31)
                {
                    XHCI_UCX_LOG("ignoring transfer event with out-of-range slot %lu / endpoint %lu",
                                 SlotId, EndpointId);
                    break;
                }
                if (XhciUcxFindSlotById(Dev, SlotId) == NULL)
                {
                    XHCI_UCX_LOG("ignoring transfer event for unknown slot %lu endpoint %lu code=%lu",
                                 SlotId, EndpointId, CompletionCode);
                    break;
                }
                /* Step 3: only EP0 TDs issued by the enumeration work item exist */
                if (EndpointId == 1 && TrbPointer != 0 &&
                    InterlockedCompareExchange(&Dev->PendingTransferDone, 0, 0) == 0 &&
                    (TrbPointer == Dev->PendingDataTrb || TrbPointer == Dev->PendingStatusTrb))
                {
                    if (TrbPointer == Dev->PendingDataTrb)
                        Dev->PendingTransferResidual = Residual;

                    if (TrbPointer == Dev->PendingStatusTrb ||
                        (CompletionCode != XHCI_COMPLETION_SUCCESS && CompletionCode != XHCI_COMPLETION_SHORT_PACKET))
                    {
                        Dev->PendingTransferCode = CompletionCode;
                        KeMemoryBarrier();
                        InterlockedExchange(&Dev->PendingTransferDone, 1);
                        KeSetEvent(&Dev->TransferEvent, IO_NO_INCREMENT, FALSE);
                    }
                    XHCI_UCX_TRACE("transfer event slot=%lu ep=%lu code=%lu residual=%lu", SlotId, EndpointId, CompletionCode, Residual);
                    break;
                }
                XHCI_UCX_LOG("unexpected transfer event slot=%lu endpoint=%lu code=%lu", SlotId, EndpointId, CompletionCode);
                break;
            }

            default:
                XHCI_UCX_TRACE("event type %lu ignored", TrbType);
                break;
        }

        Processed++;
        if (++Dev->EvtRingDequeue >= XHCI_UCX_EVT_RING_TRBS)
        {
            Dev->EvtRingDequeue = 0;
            Dev->EvtRingCycle ^= 1;
        }
    }

    if (ScanPorts)
    {
        ULONG Port;

        for (Port = 1; Port <= Dev->NumberOfPorts; Port++)
        {
            volatile ULONG *PortSc = &Dev->OperationalRegisters->PortRegister[Port - 1].PortStatusAndControl;

            if ((XHCI_UCX_READ_REG(PortSc) & XHCI_PORTSC_CHANGE_MASK) == 0)
                continue;
            if (XhciUcxHandlePortChange(Dev, Port, FALSE))
                PortChanged = TRUE;
        }
    }

    /* Publish the dequeue pointer and clear Event Handler Busy */
    Dequeue = EvtRingPa + ((ULONGLONG)Dev->EvtRingDequeue * sizeof(XHCI_TRB));
    XHCI_UCX_WRITE_REG(&Interrupter->ErdpLow, ((ULONG)(Dequeue & 0xFFFFFFFF)) | XHCI_ERDP_BUSY);
    XHCI_UCX_WRITE_REG(&Interrupter->ErdpHigh, (ULONG)(Dequeue >> 32));

    KeReleaseSpinLock(&Dev->EventRingLock, OldIrql);

    if (Processed != 0)
        XHCI_UCX_TRACE("drained %lu event(s)", Processed);

    return PortChanged;
}

/* --------------------------------------------------------------------------
 * Step 3: root-port device enumeration. Until ucx01000 grows a root-hub PDO
 * and a hub driver sits above it, the controller driver itself reacts to a
 * port connect (hub role): port reset -> Enable Slot -> Address Device ->
 * GET_DESCRIPTOR(device) on EP0 -> UcxUsbDeviceCreate/UcxEndpointCreate.
 * Everything runs in one WDF work item at PASSIVE_LEVEL, so at most one
 * command and one EP0 transfer are outstanding; the DPC matches completions
 * by TRB pointer. Every device-written byte is validated before use (SEC-1
 * subset: bLength / bDescriptorType / bMaxPacketSize0; SEC-3: TRB pointers).
 * -------------------------------------------------------------------------- */

static
BOOLEAN
XhciUcxDrainEventRing(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ BOOLEAN ScanPorts);

static
PXHCI_UCX_SLOT
XhciUcxFindSlotById(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG SlotId)
{
    ULONG Index;

    for (Index = 0; Index < XHCI_UCX_MAX_ENUM_SLOTS; Index++)
    {
        if (Dev->Slots[Index].InUse && Dev->Slots[Index].SlotId == SlotId)
            return &Dev->Slots[Index];
    }
    return NULL;
}

static
PXHCI_UCX_SLOT
XhciUcxFindSlotByPort(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG PortId)
{
    ULONG Index;

    for (Index = 0; Index < XHCI_UCX_MAX_ENUM_SLOTS; Index++)
    {
        if (Dev->Slots[Index].InUse && Dev->Slots[Index].PortId == PortId)
            return &Dev->Slots[Index];
    }
    return NULL;
}

static
PXHCI_UCX_SLOT
XhciUcxAllocateSlot(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    ULONG Index;

    for (Index = 0; Index < XHCI_UCX_MAX_ENUM_SLOTS; Index++)
    {
        PXHCI_UCX_SLOT Slot = &Dev->Slots[Index];
        SIZE_T Base;

        if (Slot->InUse)
            continue;

        RtlZeroMemory(Slot, sizeof(*Slot));
        Base = Dev->SlotAreaOffset + (SIZE_T)Index * XHCI_UCX_SLOT_PAGES * PAGE_SIZE;
        Slot->DeviceContextOffset = Base;
        Slot->InputContextOffset = Base + PAGE_SIZE;
        Slot->Ep0RingOffset = Base + XHCI_UCX_SLOT_EP0_RING_OFFSET;
        Slot->DescriptorOffset = Base + XHCI_UCX_SLOT_DESC_OFFSET;
        Slot->InUse = TRUE;
        return Slot;
    }
    return NULL;
}

/* Context accessors honouring HCCPARAMS.CSZ (32- or 64-byte contexts) */
static
PXHCI_INPUT_CONTROL_CONTEXT
XhciUcxInputControlContext(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ PXHCI_UCX_SLOT Slot)
{
    return (PXHCI_INPUT_CONTROL_CONTEXT)(Dev->HcVa + Slot->InputContextOffset);
}

static
PXHCI_SLOT_CONTEXT
XhciUcxInputSlotContext(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ PXHCI_UCX_SLOT Slot)
{
    return (PXHCI_SLOT_CONTEXT)(Dev->HcVa + Slot->InputContextOffset + Dev->ContextSize);
}

static
PXHCI_ENDPOINT_CONTEXT
XhciUcxInputEp0Context(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ PXHCI_UCX_SLOT Slot)
{
    return (PXHCI_ENDPOINT_CONTEXT)(Dev->HcVa + Slot->InputContextOffset + 2 * (SIZE_T)Dev->ContextSize);
}

static
PXHCI_SLOT_CONTEXT
XhciUcxOutputSlotContext(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ PXHCI_UCX_SLOT Slot)
{
    return (PXHCI_SLOT_CONTEXT)(Dev->HcVa + Slot->DeviceContextOffset);
}

/*
 * Wait for a completion the DPC signals. Waits in short slices and drains the
 * event ring by hand after each slice, so an emulator that drops the interrupt
 * (QEMU xhci poll quirk) still completes; bounded by TimeoutMs.
 */
static
BOOLEAN
XhciUcxWaitForCompletion(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ PKEVENT Event,
    _In_ volatile LONG *Done,
    _In_ ULONG TimeoutMs)
{
    LARGE_INTEGER Slice;
    ULONG Elapsed = 0;

    Slice.QuadPart = -(LONGLONG)XHCI_UCX_WAIT_SLICE_MS * 10000;

    while (InterlockedCompareExchange(Done, 0, 0) == 0)
    {
        (VOID)KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &Slice);
        if (InterlockedCompareExchange(Done, 0, 0) != 0)
            break;

        if (Dev->FatalError || !Dev->ControllerRunning)
            return FALSE;

        if (XhciUcxDrainEventRing(Dev, FALSE))
            InterlockedExchange(&Dev->PortRescanNeeded, 1);

        Elapsed += XHCI_UCX_WAIT_SLICE_MS;
        if (InterlockedCompareExchange(Done, 0, 0) == 0 && Elapsed >= TimeoutMs)
            return FALSE;
    }

    KeMemoryBarrier();
    return TRUE;
}

/* Write one command TRB (cycle bit from the producer state), advance over the link TRB. */
static
ULONGLONG
XhciUcxQueueCommandTrb(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG Parameter1,
    _In_ ULONG Parameter2,
    _In_ ULONG Control)
{
    PXHCI_TRB Ring = (PXHCI_TRB)(Dev->HcVa + Dev->CmdRingOffset);
    PXHCI_TRB Trb = &Ring[Dev->CmdRingEnqueue];
    ULONGLONG TrbPa = (ULONGLONG)XhciUcxHcPhysical(Dev, Dev->CmdRingOffset).QuadPart +
                      (ULONGLONG)Dev->CmdRingEnqueue * sizeof(XHCI_TRB);

    Trb->Parameter1 = Parameter1;
    Trb->Parameter2 = Parameter2;
    Trb->Status = 0;
    KeMemoryBarrier();
    Trb->Control = (Control & ~XHCI_TRB_CYCLE) | (Dev->CmdRingCycle & XHCI_TRB_CYCLE);
    KeMemoryBarrier();

    Dev->CmdRingEnqueue++;
    if (Dev->CmdRingEnqueue >= XHCI_UCX_CMD_RING_TRBS - 1)
    {
        PXHCI_TRB Link = &Ring[XHCI_UCX_CMD_RING_TRBS - 1];

        /* The controller follows the link only while its cycle bit matches this pass */
        Link->Control = (Link->Control & ~XHCI_TRB_CYCLE) | (Dev->CmdRingCycle & XHCI_TRB_CYCLE);
        KeMemoryBarrier();
        Dev->CmdRingEnqueue = 0;
        Dev->CmdRingCycle ^= 1;
    }

    return TrbPa;
}

/* Issue one command and wait for its completion event (PASSIVE_LEVEL). */
static
NTSTATUS
XhciUcxSendCommand(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG TrbType,
    _In_ ULONGLONG Parameter,
    _In_ ULONG ControlFields,
    _Out_ PULONG CompletionCode,
    _Out_opt_ PUCHAR SlotIdOut)
{
    ULONGLONG TrbPa;

    *CompletionCode = XHCI_COMPLETION_INVALID;
    if (SlotIdOut != NULL)
        *SlotIdOut = 0;

    if (!Dev->ControllerRunning || Dev->FatalError)
        return STATUS_DEVICE_NOT_READY;

    KeClearEvent(&Dev->CommandEvent);
    Dev->PendingCommandCode = XHCI_COMPLETION_INVALID;
    Dev->PendingCommandSlot = 0;
    InterlockedExchange(&Dev->PendingCommandDone, 0);

    /* The controller fetches nothing before the doorbell, so publishing the
     * pending pointer between the TRB write and the doorbell is race-free. */
    TrbPa = XhciUcxQueueCommandTrb(Dev,
                                   (ULONG)(Parameter & 0xFFFFFFFF),
                                   (ULONG)(Parameter >> 32),
                                   (TrbType << XHCI_TRB_TYPE_SHIFT) | ControlFields);
    Dev->PendingCommandTrb = TrbPa;
    KeMemoryBarrier();
    XHCI_UCX_WRITE_REG(&Dev->DoorbellArray->Doorbell[0], 0);

    if (!XhciUcxWaitForCompletion(Dev, &Dev->CommandEvent, &Dev->PendingCommandDone, XHCI_UCX_COMMAND_TIMEOUT_MS))
    {
        Dev->PendingCommandTrb = 0;
        XHCI_UCX_LOG("command type %lu timed out after %u ms", TrbType, XHCI_UCX_COMMAND_TIMEOUT_MS);
        return STATUS_IO_TIMEOUT;
    }
    Dev->PendingCommandTrb = 0;

    *CompletionCode = Dev->PendingCommandCode;
    if (SlotIdOut != NULL)
        *SlotIdOut = Dev->PendingCommandSlot;

    return (Dev->PendingCommandCode == XHCI_COMPLETION_SUCCESS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

/* EP0 transfer ring: zero, link TRB back to the base, producer cycle 1 */
static
VOID
XhciUcxInitEp0Ring(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _Inout_ PXHCI_UCX_SLOT Slot)
{
    PXHCI_TRB Ring = (PXHCI_TRB)(Dev->HcVa + Slot->Ep0RingOffset);
    ULONGLONG RingPa = (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->Ep0RingOffset).QuadPart;

    RtlZeroMemory(Ring, XHCI_UCX_EP0_RING_TRBS * sizeof(XHCI_TRB));
    Ring[XHCI_UCX_EP0_RING_TRBS - 1].Parameter1 = (ULONG)(RingPa & 0xFFFFFFFF);
    Ring[XHCI_UCX_EP0_RING_TRBS - 1].Parameter2 = (ULONG)(RingPa >> 32);
    Ring[XHCI_UCX_EP0_RING_TRBS - 1].Control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    Slot->Ep0Enqueue = 0;
    Slot->Ep0Cycle = 1;
    KeMemoryBarrier();
}

/* Take the next EP0 TRB; returns its physical address and the cycle bit it must carry. */
static
PXHCI_TRB
XhciUcxEp0NextTrb(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _Inout_ PXHCI_UCX_SLOT Slot,
    _Out_ PULONGLONG TrbPa,
    _Out_ PULONG Cycle)
{
    PXHCI_TRB Ring = (PXHCI_TRB)(Dev->HcVa + Slot->Ep0RingOffset);
    PXHCI_TRB Trb = &Ring[Slot->Ep0Enqueue];

    *TrbPa = (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->Ep0RingOffset).QuadPart +
             (ULONGLONG)Slot->Ep0Enqueue * sizeof(XHCI_TRB);
    *Cycle = Slot->Ep0Cycle & XHCI_TRB_CYCLE;

    Slot->Ep0Enqueue++;
    if (Slot->Ep0Enqueue >= XHCI_UCX_EP0_RING_TRBS - 1)
    {
        PXHCI_TRB Link = &Ring[XHCI_UCX_EP0_RING_TRBS - 1];

        Link->Control = (Link->Control & ~XHCI_TRB_CYCLE) | (Slot->Ep0Cycle & XHCI_TRB_CYCLE);
        KeMemoryBarrier();
        Slot->Ep0Enqueue = 0;
        Slot->Ep0Cycle ^= 1;
    }

    return Trb;
}

/*
 * One control transfer on EP0 (Setup / optional Data / Status TD), data in
 * the slot's descriptor buffer. Returns the bytes actually transferred.
 */
static
NTSTATUS
XhciUcxEp0Control(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _Inout_ PXHCI_UCX_SLOT Slot,
    _In_ const USB_DEFAULT_PIPE_SETUP_PACKET *Setup,
    _In_ ULONG Length,
    _In_ BOOLEAN DataIn,
    _Out_ PULONG Transferred)
{
    PXHCI_TRB SetupTrb;
    PXHCI_TRB StageTrb;
    ULONGLONG SetupPa;
    ULONGLONG DataPa = 0;
    ULONGLONG StatusPa;
    ULONG SetupCycle;
    ULONG Cycle;
    ULONG SetupControl;
    ULONG SetupWords[2];
    ULONG Residual;

    *Transferred = 0;

    if (Length > XHCI_UCX_DESC_BUFFER_SIZE)
        return STATUS_INVALID_PARAMETER;
    if (!Dev->ControllerRunning || Dev->FatalError)
        return STATUS_DEVICE_NOT_READY;

    KeClearEvent(&Dev->TransferEvent);
    Dev->PendingTransferCode = XHCI_COMPLETION_INVALID;
    Dev->PendingTransferResidual = Length;
    InterlockedExchange(&Dev->PendingTransferDone, 0);

    RtlCopyMemory(SetupWords, Setup, sizeof(SetupWords));

    /* Setup Stage: immediate data, written with the inverted cycle so the
     * controller cannot start before the Data/Status TRBs exist */
    SetupTrb = XhciUcxEp0NextTrb(Dev, Slot, &SetupPa, &SetupCycle);
    SetupTrb->Parameter1 = SetupWords[0];
    SetupTrb->Parameter2 = SetupWords[1];
    SetupTrb->Status = sizeof(USB_DEFAULT_PIPE_SETUP_PACKET);
    SetupControl = (XHCI_TRB_TYPE_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IDT |
                   (Length == 0 ? XHCI_UCX_TRT_NO_DATA : (DataIn ? XHCI_UCX_TRT_IN : XHCI_UCX_TRT_OUT));
    SetupTrb->Control = SetupControl | (SetupCycle ^ XHCI_TRB_CYCLE);

    /* Data Stage (single TRB, TD size 0; ISP so a short IN reports its residual) */
    if (Length != 0)
    {
        ULONGLONG BufferPa = (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->DescriptorOffset).QuadPart;

        StageTrb = XhciUcxEp0NextTrb(Dev, Slot, &DataPa, &Cycle);
        StageTrb->Parameter1 = (ULONG)(BufferPa & 0xFFFFFFFF);
        StageTrb->Parameter2 = (ULONG)(BufferPa >> 32);
        StageTrb->Status = Length & XHCI_TRB_LEN_MASK;
        StageTrb->Control = (XHCI_TRB_TYPE_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) |
                            (DataIn ? (XHCI_TRB_DIR_IN | XHCI_TRB_ISP) : 0) | Cycle;
    }

    /* Status Stage: IOC; direction opposite to the data stage (IN when no data) */
    StageTrb = XhciUcxEp0NextTrb(Dev, Slot, &StatusPa, &Cycle);
    StageTrb->Parameter1 = 0;
    StageTrb->Parameter2 = 0;
    StageTrb->Status = 0;
    StageTrb->Control = (XHCI_TRB_TYPE_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC |
                        ((Length == 0 || !DataIn) ? XHCI_TRB_DIR_IN : 0) | Cycle;

    Dev->PendingDataTrb = DataPa;
    Dev->PendingStatusTrb = StatusPa;
    KeMemoryBarrier();

    /* Publish the Setup TRB, then ring EP0 (doorbell target 1) */
    SetupTrb->Control = SetupControl | SetupCycle;
    KeMemoryBarrier();
    XHCI_UCX_WRITE_REG(&Dev->DoorbellArray->Doorbell[Slot->SlotId], 1);

    if (!XhciUcxWaitForCompletion(Dev, &Dev->TransferEvent, &Dev->PendingTransferDone, XHCI_UCX_TRANSFER_TIMEOUT_MS))
    {
        Dev->PendingDataTrb = 0;
        Dev->PendingStatusTrb = 0;
        XHCI_UCX_LOG("slot %u EP0 transfer timed out (request %02x len %lu)",
                     Slot->SlotId, Setup->bRequest, Length);
        return STATUS_IO_TIMEOUT;
    }
    Dev->PendingDataTrb = 0;
    Dev->PendingStatusTrb = 0;

    if (Dev->PendingTransferCode != XHCI_COMPLETION_SUCCESS &&
        Dev->PendingTransferCode != XHCI_COMPLETION_SHORT_PACKET)
    {
        XHCI_UCX_LOG("slot %u EP0 transfer failed, completion code %lu (request %02x len %lu)",
                     Slot->SlotId, Dev->PendingTransferCode, Setup->bRequest, Length);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    Residual = Dev->PendingTransferResidual;
    if (Residual > Length)
        Residual = Length;   /* device-written field: clamp */
    *Transferred = Length - Residual;
    return STATUS_SUCCESS;
}

/* Masked PORTSC write: never touches change bits, PED (1 = disable), PR/WPR, LWS */
static
VOID
XhciUcxWritePortSc(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG PortId,
    _In_ ULONG SetBits)
{
    volatile ULONG *PortSc = &Dev->OperationalRegisters->PortRegister[PortId - 1].PortStatusAndControl;
    ULONG Value = XHCI_UCX_READ_REG(PortSc);

    Value &= ~(XHCI_PORTSC_CHANGE_MASK | XHCI_PORTSC_PED | XHCI_PORTSC_PR | XHCI_PORTSC_WPR | XHCI_PORTSC_LWS);
    XHCI_UCX_WRITE_REG(PortSc, Value | SetBits);
}

/*
 * Bring a connected root port to Enabled: USB2 ports need a hot reset (PR),
 * USB3 ports enable themselves after link training. Waits bounded; returns
 * the negotiated speed id.
 */
static
BOOLEAN
XhciUcxResetPort(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG PortId,
    _Out_ PULONG SpeedOut)
{
    volatile ULONG *PortSc = &Dev->OperationalRegisters->PortRegister[PortId - 1].PortStatusAndControl;
    LARGE_INTEGER Slice;
    ULONG Value = XHCI_UCX_READ_REG(PortSc);
    ULONG Elapsed = 0;

    *SpeedOut = 0;

    if ((Value & XHCI_PORTSC_CCS) == 0)
        return FALSE;

    if (PortId <= XHCI_MAX_PORTS && Dev->PortProtocol[PortId] != 3)
        XhciUcxWritePortSc(Dev, PortId, XHCI_PORTSC_PR);

    Slice.QuadPart = -(LONGLONG)1 * 10000;   /* 1 ms */
    while (Elapsed < XHCI_UCX_PORT_RESET_TIMEOUT_MS)
    {
        Value = XHCI_UCX_READ_REG(PortSc);
        if ((Value & XHCI_PORTSC_CCS) == 0)
            return FALSE;
        if ((Value & XHCI_PORTSC_PED) && (Value & XHCI_PORTSC_PR) == 0)
            break;
        KeDelayExecutionThread(KernelMode, FALSE, &Slice);
        Elapsed++;
    }

    if ((Value & XHCI_PORTSC_PED) == 0 || (Value & XHCI_PORTSC_PR) != 0)
    {
        XHCI_UCX_LOG("port %lu did not reach Enabled within %u ms (PORTSC=%08lx)",
                     PortId, XHCI_UCX_PORT_RESET_TIMEOUT_MS, Value);
        return FALSE;
    }

    /* Acknowledge the change bits this reset produced (W1C) */
    if (Value & XHCI_PORTSC_CHANGE_MASK)
        XHCI_UCX_WRITE_REG(PortSc, Value & XHCI_PORTSC_CHANGE_MASK);

    /* USB 2.0 9.2.6.2: 10 ms reset recovery before the first request */
    Slice.QuadPart = -(LONGLONG)10 * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &Slice);

    *SpeedOut = (Value & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
    return TRUE;
}

/* Disable Slot (optional), clear the DCBAA entry, delete the UCX objects, forget the slot */
static
VOID
XhciUcxReleaseSlot(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _Inout_ PXHCI_UCX_SLOT Slot,
    _In_ BOOLEAN IssueDisable)
{
    if (Slot->SlotId != 0)
    {
        PULONGLONG Dcbaa = (PULONGLONG)(Dev->HcVa + Dev->DcbaaOffset);

        if (IssueDisable && Dev->ControllerRunning && !Dev->FatalError)
        {
            ULONG Code = 0;
            NTSTATUS Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_DISABLE_SLOT, 0,
                                                 XHCI_COMMAND_SLOT_FIELD(Slot->SlotId), &Code, NULL);

            XHCI_UCX_LOG("slot %u disabled rc=0x%08lX code=%lu", Slot->SlotId, (ULONG)Status, Code);
        }
        if (Slot->SlotId <= Dev->MaxSlots)
            Dcbaa[Slot->SlotId] = 0;
        KeMemoryBarrier();
    }

    if (Slot->UsbDevice != NULL)
        WdfObjectDelete((WDFOBJECT)Slot->UsbDevice);   /* its EP0 child goes with it */

    RtlZeroMemory(Slot, sizeof(*Slot));
}

static
VOID
XhciUcxForgetAllSlots(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ BOOLEAN IssueDisable)
{
    ULONG Index;

    for (Index = 0; Index < XHCI_UCX_MAX_ENUM_SLOTS; Index++)
    {
        if (Dev->Slots[Index].InUse)
            XhciUcxReleaseSlot(Dev, &Dev->Slots[Index], IssueDisable);
    }
    RtlZeroMemory(Dev->PortEnumState, sizeof(Dev->PortEnumState));
}

/* SEC-1 subset: is bMaxPacketSize0 legal for the negotiated speed? Returns the byte size or 0. */
static
ULONG
XhciUcxEp0MaxPacketFromDescriptor(
    _In_ ULONG Speed,
    _In_ UCHAR bMaxPacketSize0)
{
    switch (Speed)
    {
        case XHCI_PORTSC_SPEED_SUPER:
            return (bMaxPacketSize0 == 9) ? 512 : 0;       /* encoded as 2^n */
        case XHCI_PORTSC_SPEED_HIGH:
            return (bMaxPacketSize0 == 64) ? 64 : 0;
        case XHCI_PORTSC_SPEED_LOW:
            return (bMaxPacketSize0 == 8) ? 8 : 0;
        case XHCI_PORTSC_SPEED_FULL:
        default:
            return (bMaxPacketSize0 == 8 || bMaxPacketSize0 == 16 ||
                    bMaxPacketSize0 == 32 || bMaxPacketSize0 == 64) ? bMaxPacketSize0 : 0;
    }
}

/* Build the input context for Address Device / Evaluate Context */
static
VOID
XhciUcxBuildEp0InputContext(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ PXHCI_UCX_SLOT Slot,
    _In_ ULONG AddFlags,
    _In_ ULONGLONG Ep0Dequeue)
{
    PXHCI_INPUT_CONTROL_CONTEXT Icc = XhciUcxInputControlContext(Dev, Slot);
    PXHCI_SLOT_CONTEXT SlotCtx = XhciUcxInputSlotContext(Dev, Slot);
    PXHCI_ENDPOINT_CONTEXT Ep0Ctx = XhciUcxInputEp0Context(Dev, Slot);

    RtlZeroMemory(Dev->HcVa + Slot->InputContextOffset, PAGE_SIZE);

    Icc->AddContextFlags = AddFlags;
    Icc->DropContextFlags = 0;

    XhciSlotContextSetRoute(SlotCtx, 0);              /* directly on a root port */
    XhciSlotContextSetSpeed(SlotCtx, Slot->Speed);
    XhciSlotContextSetLastCtx(SlotCtx, 1);            /* EP0 only */
    XhciSlotContextSetRootPort(SlotCtx, Slot->PortId);

    XhciEndpointContextInit(Ep0Ctx,
                            XHCI_ENDPOINT_TYPE_CONTROL,
                            Slot->Ep0MaxPacket,
                            0, 0, 0, 0,
                            8,                        /* average TRB length */
                            Ep0Dequeue);
    KeMemoryBarrier();
}

static
VOID
XhciUcxEnumeratePort(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG PortId)
{
    PXHCI_UCX_SLOT Slot;
    PULONGLONG Dcbaa = (PULONGLONG)(Dev->HcVa + Dev->DcbaaOffset);
    USB_DEFAULT_PIPE_SETUP_PACKET Setup;
    const USB_DEVICE_DESCRIPTOR *DevDesc;
    PUCHAR Buffer;
    ULONGLONG Ep0RingPa;
    ULONG Speed = 0;
    ULONG Code = 0;
    ULONG Transferred = 0;
    ULONG DescriptorMps;
    UCHAR SlotId = 0;
    NTSTATUS Status;

    if (!XhciUcxResetPort(Dev, PortId, &Speed))
    {
        XHCI_UCX_LOG("STEP3 port %lu: reset/enable failed", PortId);
        Dev->PortEnumState[PortId] = XHCI_UCX_PORT_FAILED;
        return;
    }

    Slot = XhciUcxAllocateSlot(Dev);
    if (Slot == NULL)
    {
        XHCI_UCX_LOG("STEP3 port %lu: no free device slot (max %u)", PortId, XHCI_UCX_MAX_ENUM_SLOTS);
        Dev->PortEnumState[PortId] = XHCI_UCX_PORT_FAILED;
        return;
    }
    Slot->PortId = (UCHAR)PortId;
    Slot->Speed = (UCHAR)Speed;
    Slot->Ep0MaxPacket = (Speed == XHCI_PORTSC_SPEED_SUPER) ? 512 :
                         (Speed == XHCI_PORTSC_SPEED_HIGH) ? 64 : 8;
    XHCI_UCX_LOG("STEP3 port %lu enabled: speed id %lu, initial EP0 MPS %lu", PortId, Speed, Slot->Ep0MaxPacket);

    /* Enable Slot */
    Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_ENABLE_SLOT, 0, 0, &Code, &SlotId);
    if (!NT_SUCCESS(Status) || SlotId == 0 || SlotId > Dev->MaxSlots || XhciUcxFindSlotById(Dev, SlotId) != NULL)
    {
        XHCI_UCX_LOG("STEP3 port %lu: Enable Slot failed rc=0x%08lX code=%lu slot=%u", PortId, (ULONG)Status, Code, SlotId);
        Slot->SlotId = 0;
        XhciUcxReleaseSlot(Dev, Slot, FALSE);
        Dev->PortEnumState[PortId] = XHCI_UCX_PORT_FAILED;
        return;
    }
    Slot->SlotId = SlotId;
    XHCI_UCX_LOG("STEP3 slot %u enabled for port %lu", SlotId, PortId);

    /* Contexts, EP0 ring, DCBAA entry, then Address Device (BSR = 0) */
    RtlZeroMemory(Dev->HcVa + Slot->DeviceContextOffset, PAGE_SIZE);
    XhciUcxInitEp0Ring(Dev, Slot);
    Ep0RingPa = (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->Ep0RingOffset).QuadPart;
    XhciUcxBuildEp0InputContext(Dev, Slot, XHCI_UCX_INPUT_CTX_ADD_SLOT | XHCI_UCX_INPUT_CTX_ADD_EP0,
                                Ep0RingPa | XHCI_TRB_CYCLE);
    Dcbaa[SlotId] = (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->DeviceContextOffset).QuadPart;
    KeMemoryBarrier();

    Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_ADDRESS_DEV,
                                (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->InputContextOffset).QuadPart,
                                XHCI_COMMAND_SLOT_FIELD(SlotId), &Code, NULL);
    if (!NT_SUCCESS(Status))
    {
        XHCI_UCX_LOG("STEP3 slot %u: Address Device failed rc=0x%08lX code=%lu", SlotId, (ULONG)Status, Code);
        XhciUcxReleaseSlot(Dev, Slot, TRUE);
        Dev->PortEnumState[PortId] = XHCI_UCX_PORT_FAILED;
        return;
    }
    Slot->Addressed = TRUE;
    Slot->UsbAddress = (UCHAR)XHCI_UCX_SLOT_STATE_ADDRESS(XhciUcxOutputSlotContext(Dev, Slot)->DevState);
    XHCI_UCX_LOG("STEP3 slot %u addressed (USB address %u)", SlotId, Slot->UsbAddress);

    /* GET_DESCRIPTOR(device): first 8 bytes to learn bMaxPacketSize0 */
    Buffer = Dev->HcVa + Slot->DescriptorOffset;
    RtlZeroMemory(Buffer, XHCI_UCX_DESC_BUFFER_SIZE);
    RtlZeroMemory(&Setup, sizeof(Setup));
    Setup.bmRequestType.B = 0x80;                      /* device-to-host, standard, device */
    Setup.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    Setup.wValue.W = (USHORT)(USB_DEVICE_DESCRIPTOR_TYPE << 8);
    Setup.wIndex.W = 0;
    Setup.wLength = 8;

    Status = XhciUcxEp0Control(Dev, Slot, &Setup, 8, TRUE, &Transferred);
    DevDesc = (const USB_DEVICE_DESCRIPTOR *)Buffer;
    if (!NT_SUCCESS(Status) || Transferred < 8)
    {
        XHCI_UCX_LOG("STEP3 slot %u: GET_DESCRIPTOR(8) failed rc=0x%08lX got %lu bytes", SlotId, (ULONG)Status, Transferred);
        XhciUcxReleaseSlot(Dev, Slot, TRUE);
        Dev->PortEnumState[PortId] = XHCI_UCX_PORT_FAILED;
        return;
    }
    if (DevDesc->bLength != sizeof(USB_DEVICE_DESCRIPTOR) || DevDesc->bDescriptorType != USB_DEVICE_DESCRIPTOR_TYPE)
    {
        XHCI_UCX_LOG("STEP3 slot %u: SEC-1 reject, device descriptor header bLength=%u bDescriptorType=%u",
                     SlotId, DevDesc->bLength, DevDesc->bDescriptorType);
        XhciUcxReleaseSlot(Dev, Slot, TRUE);
        Dev->PortEnumState[PortId] = XHCI_UCX_PORT_FAILED;
        return;
    }
    DescriptorMps = XhciUcxEp0MaxPacketFromDescriptor(Speed, DevDesc->bMaxPacketSize0);
    if (DescriptorMps == 0)
    {
        XHCI_UCX_LOG("STEP3 slot %u: SEC-1 reject, bMaxPacketSize0=%u illegal for speed id %lu",
                     SlotId, DevDesc->bMaxPacketSize0, Speed);
        XhciUcxReleaseSlot(Dev, Slot, TRUE);
        Dev->PortEnumState[PortId] = XHCI_UCX_PORT_FAILED;
        return;
    }

    /* Full-speed devices may use a bigger EP0 than the 8-byte default: Evaluate Context */
    if (DescriptorMps != Slot->Ep0MaxPacket)
    {
        ULONGLONG Dequeue = Ep0RingPa + (ULONGLONG)Slot->Ep0Enqueue * sizeof(XHCI_TRB);

        Slot->Ep0MaxPacket = DescriptorMps;
        XhciUcxBuildEp0InputContext(Dev, Slot, XHCI_UCX_INPUT_CTX_ADD_EP0,
                                    Dequeue | (Slot->Ep0Cycle & XHCI_TRB_CYCLE));
        Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_EVAL_CTX,
                                    (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->InputContextOffset).QuadPart,
                                    XHCI_COMMAND_SLOT_FIELD(SlotId), &Code, NULL);
        XHCI_UCX_LOG("STEP3 slot %u: EP0 MPS -> %lu via Evaluate Context rc=0x%08lX code=%lu",
                     SlotId, DescriptorMps, (ULONG)Status, Code);
        if (!NT_SUCCESS(Status))
        {
            XhciUcxReleaseSlot(Dev, Slot, TRUE);
            Dev->PortEnumState[PortId] = XHCI_UCX_PORT_FAILED;
            return;
        }
    }

    /* Full 18-byte device descriptor */
    RtlZeroMemory(Buffer, XHCI_UCX_DESC_BUFFER_SIZE);
    Setup.wLength = sizeof(USB_DEVICE_DESCRIPTOR);
    Status = XhciUcxEp0Control(Dev, Slot, &Setup, sizeof(USB_DEVICE_DESCRIPTOR), TRUE, &Transferred);
    if (!NT_SUCCESS(Status) || Transferred != sizeof(USB_DEVICE_DESCRIPTOR) ||
        DevDesc->bLength != sizeof(USB_DEVICE_DESCRIPTOR) || DevDesc->bDescriptorType != USB_DEVICE_DESCRIPTOR_TYPE)
    {
        XHCI_UCX_LOG("STEP3 slot %u: GET_DESCRIPTOR(18) failed rc=0x%08lX got %lu bytes bLength=%u type=%u",
                     SlotId, (ULONG)Status, Transferred, DevDesc->bLength, DevDesc->bDescriptorType);
        XhciUcxReleaseSlot(Dev, Slot, TRUE);
        Dev->PortEnumState[PortId] = XHCI_UCX_PORT_FAILED;
        return;
    }

    XHCI_UCX_LOG("STEP3 slot %u device descriptor: bcdUSB=%04x class=%02x/%02x/%02x bMaxPacketSize0=%u VID=%04x PID=%04x bcdDevice=%04x configurations=%u",
                 SlotId, DevDesc->bcdUSB, DevDesc->bDeviceClass, DevDesc->bDeviceSubClass,
                 DevDesc->bDeviceProtocol, DevDesc->bMaxPacketSize0, DevDesc->idVendor,
                 DevDesc->idProduct, DevDesc->bcdDevice, DevDesc->bNumConfigurations);

    /* Exercise the UCX device/endpoint DDIs (ucx01000 M1 skeleton: object lifecycle) */
    {
        PUCXUSBDEVICE_INIT UsbDeviceInit = NULL;
        PUCXENDPOINT_INIT EndpointInit = NULL;

        Status = UcxUsbDeviceCreate(Dev->Controller, &UsbDeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &Slot->UsbDevice);
        XHCI_UCX_LOG("UcxUsbDeviceCreate rc=0x%08lX", (ULONG)Status);
        if (NT_SUCCESS(Status))
        {
            Status = UcxEndpointCreate(Slot->UsbDevice, &EndpointInit, WDF_NO_OBJECT_ATTRIBUTES, &Slot->Ep0);
            XHCI_UCX_LOG("UcxEndpointCreate (EP0) rc=0x%08lX", (ULONG)Status);
        }
        else
        {
            Slot->UsbDevice = NULL;
        }
    }

    Dev->PortEnumState[PortId] = XHCI_UCX_PORT_ENUMERATED;
    XHCI_UCX_LOG("STEP3 DONE: port %lu slot %u addressed and identified", PortId, SlotId);
}

/* The enumeration work item: reconcile every root port with its slot state */
static
VOID
NTAPI
XhciUcxEvtEnumWorkItem(
    _In_ WDFWORKITEM WorkItem)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetDevice((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));
    ULONG Pass = 0;

    do
    {
        ULONG Port;

        InterlockedExchange(&Dev->PortRescanNeeded, 0);

        if (!Dev->ControllerRunning || Dev->FatalError || Dev->OperationalRegisters == NULL || Dev->HcVa == NULL)
            return;

        for (Port = 1; Port <= Dev->NumberOfPorts && Port <= XHCI_MAX_PORTS; Port++)
        {
            volatile ULONG *PortSc = &Dev->OperationalRegisters->PortRegister[Port - 1].PortStatusAndControl;
            BOOLEAN Connected = (XHCI_UCX_READ_REG(PortSc) & XHCI_PORTSC_CCS) != 0;
            PXHCI_UCX_SLOT Slot = XhciUcxFindSlotByPort(Dev, Port);

            if (!Connected)
            {
                if (Slot != NULL)
                {
                    XHCI_UCX_LOG("port %lu disconnected: releasing slot %u", Port, Slot->SlotId);
                    XhciUcxReleaseSlot(Dev, Slot, TRUE);
                }
                Dev->PortEnumState[Port] = XHCI_UCX_PORT_IDLE;
                continue;
            }

            if (Dev->PortEnumState[Port] == XHCI_UCX_PORT_IDLE && Slot == NULL)
                XhciUcxEnumeratePort(Dev, Port);
        }
    } while (InterlockedCompareExchange(&Dev->PortRescanNeeded, 0, 0) != 0 && ++Pass < 4);
}

/* --------------------------------------------------------------------------
 * UCX controller / root-hub callbacks (minimal until step 4)
 * -------------------------------------------------------------------------- */

static
NTSTATUS
NTAPI
XhciUcxEvtControllerReset(
    _In_ UCXCONTROLLER Controller)
{
    UNREFERENCED_PARAMETER(Controller);
    XHCI_UCX_LOG("EvtControllerReset (no-op until step 3)");
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
    /* Root-hub class requests (port status / features) arrive with step 3 */
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
 * WDFINTERRUPT: shared-line-safe ISR + DPC event-ring drain
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
    if (UsbSts == 0xFFFFFFFF)
        return FALSE;   /* MMIO gone (surprise removal) */

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

    if (Pending & (XHCI_USBSTS_HSE | XHCI_USBSTS_HCE))
    {
        /* Fatal from this driver's point of view: stop the controller so a
         * permanently asserted error bit cannot storm the DPC. */
        if (!Dev->FatalError && Dev->OperationalRegisters != NULL)
        {
            ULONG Command;

            Dev->FatalError = TRUE;
            XhciUcxArmInterrupts(Dev, FALSE);
            Command = XHCI_UCX_READ_REG(&Dev->OperationalRegisters->UsbCmd);
            XHCI_UCX_WRITE_REG(&Dev->OperationalRegisters->UsbCmd, Command & ~XHCI_USBCMD_RS);
            Dev->ControllerRunning = FALSE;
            XHCI_UCX_LOG("FATAL host controller error (USBSTS bits %08lx), controller stopped", Pending);
        }
        return;
    }

    if (Pending & (XHCI_USBSTS_EINT | XHCI_USBSTS_PCD))
    {
        if (XhciUcxDrainEventRing(Dev, (Pending & XHCI_USBSTS_PCD) != 0))
        {
            if (Dev->RootHub != NULL)
            {
                XHCI_UCX_LOG("root hub port change -> UcxRootHubPortChanged");
                UcxRootHubPortChanged(Dev->RootHub);
            }
            /* Step 3: reconcile ports with device slots at PASSIVE_LEVEL */
            InterlockedExchange(&Dev->PortRescanNeeded, 1);
            if (Dev->EnumWorkItem != NULL)
                WdfWorkItemEnqueue(Dev->EnumWorkItem);
        }
    }
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

static
VOID
XhciUcxReleaseBusInterface(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    if (Dev->HaveBusInterface)
    {
        if (Dev->BusInterface.InterfaceDereference != NULL)
            Dev->BusInterface.InterfaceDereference(Dev->BusInterface.Context);
        RtlZeroMemory(&Dev->BusInterface, sizeof(Dev->BusInterface));
        Dev->HaveBusInterface = FALSE;
    }
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
    Dev->ScratchpadCount = XHCI_HCS2_MAX_SCRATCH(Dev->HcsParams2);
    Dev->Supports64Bit = (BOOLEAN)XHCI_HCC_64BIT_ADDR(Dev->HccParams);
    Dev->PortPowerControl = (BOOLEAN)XHCI_HCC_PORT_POWER_CONTROL(Dev->HccParams);
    Dev->ContextSize = XHCI_HCC_64B_CONTEXT(Dev->HccParams) ? 64 : 32;

    if (Dev->MaxSlots == 0 || Dev->NumberOfPorts == 0 || Dev->MaxInterrupters == 0)
    {
        XHCI_UCX_LOG("unsupported controller geometry (MaxSlots=%lu MaxPorts=%lu MaxIntrs=%lu)",
                     Dev->MaxSlots, Dev->NumberOfPorts, Dev->MaxInterrupters);
        XhciUcxUnmapHardware(Dev);
        return STATUS_NOT_SUPPORTED;
    }
    if (Dev->ScratchpadCount > XHCI_MAX_SCRATCHPADS)
    {
        XHCI_UCX_LOG("controller wants %lu scratchpads, max supported %u", Dev->ScratchpadCount, XHCI_MAX_SCRATCHPADS);
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

    XHCI_UCX_LOG("xHCI %u.%02u BAR len=%lu CAPLENGTH=%lu MaxSlots=%lu MaxIntrs=%lu ports=%lu (USB2=%u USB3=%u) scratchpads=%lu AC64=%u PPC=%u %s interrupt",
                 (Dev->HciVersion >> 8) & 0xFF, Dev->HciVersion & 0xFF,
                 (ULONG)Dev->MmioLength, Dev->CapabilityLength,
                 Dev->MaxSlots, Dev->MaxInterrupters, Dev->NumberOfPorts,
                 Dev->NumberOf20Ports, Dev->NumberOf30Ports, Dev->ScratchpadCount,
                 Dev->Supports64Bit ? 1 : 0, Dev->PortPowerControl ? 1 : 0,
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

    /* Objects persist across a stop/start rebalance; only the device's own
     * teardown (WDF parent deletion) ends their life (contract 7.1). */
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

    /* PCI config access; absent (ACPI-enumerated) is not an error */
    if (!Dev->HaveBusInterface)
    {
        Status = WdfFdoQueryForInterface(Device, &GUID_BUS_INTERFACE_STANDARD,
                                         (PINTERFACE)&Dev->BusInterface,
                                         sizeof(Dev->BusInterface), 1, NULL);
        Dev->HaveBusInterface = NT_SUCCESS(Status);
        if (!Dev->HaveBusInterface)
            RtlZeroMemory(&Dev->BusInterface, sizeof(Dev->BusInterface));
    }

    Status = XhciUcxMapHardware(Dev, ResourcesTranslated);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = XhciUcxAllocateHcMemory(Dev);
    if (!NT_SUCCESS(Status))
    {
        XhciUcxUnmapHardware(Dev);
        return Status;
    }

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

    /* The common buffer / DMA enabler stay with the device (reused on re-prepare) */
    XhciUcxUnmapHardware(Dev);
    XhciUcxReleaseBusInterface(Dev);
    XHCI_UCX_LOG("ReleaseHardware: BAR unmapped");
    return STATUS_SUCCESS;
}

/*
 * Contract 7.1 step 2: the controller RUNS here. Interrupts are not connected
 * yet (WDF connects them after this callback), so interrupter 0 stays masked
 * until D0EntryPostInterruptsEnabled; any event generated meanwhile sits in
 * the event ring and is drained there.
 */
static
NTSTATUS
NTAPI
XhciUcxEvtDeviceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetDevice(Device);
    NTSTATUS Status;

    if (!Dev->HardwarePrepared || Dev->HcVa == NULL)
        return STATUS_DEVICE_NOT_READY;

    /* The reset below wipes every slot in hardware: drop the software view too */
    XhciUcxForgetAllSlots(Dev, FALSE);

    Dev->FatalError = FALSE;
    Dev->PendingUsbSts = 0;

    Status = XhciUcxEnableBusMaster(Dev);
    if (!NT_SUCCESS(Status))
        return Status;

    XhciUcxBiosHandoff(Dev);

    Status = XhciUcxResetController(Dev);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = XhciUcxProgramController(Dev);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = XhciUcxRunController(Dev);
    if (!NT_SUCCESS(Status))
    {
        /* One retry after a fresh reset, like the legacy path */
        if (!NT_SUCCESS(XhciUcxResetController(Dev)) ||
            !NT_SUCCESS(XhciUcxProgramController(Dev)) ||
            !NT_SUCCESS(XhciUcxRunController(Dev)))
        {
            XHCI_UCX_LOG("D0Entry: controller did not start");
            return STATUS_DEVICE_HARDWARE_ERROR;
        }
    }

    XhciUcxPowerOnPorts(Dev);

    XHCI_UCX_LOG("STEP2 controller running (from state %u): USBCMD=%08lx USBSTS=%08lx",
                 (ULONG)PreviousState,
                 XHCI_UCX_READ_REG(&Dev->OperationalRegisters->UsbCmd),
                 XHCI_UCX_READ_REG(&Dev->OperationalRegisters->UsbSts));
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
XhciUcxEvtDeviceD0EntryPostInterruptsEnabled(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetDevice(Device);

    UNREFERENCED_PARAMETER(PreviousState);

    if (!Dev->ControllerRunning)
        return STATUS_SUCCESS;

    XhciUcxArmInterrupts(Dev, TRUE);

    /* Pick up anything the controller queued before the interrupter was armed
     * (boot-time connects) and report the current port picture once. */
    if (XhciUcxDrainEventRing(Dev, TRUE))
    {
        if (Dev->RootHub != NULL)
        {
            XHCI_UCX_LOG("initial root hub port change -> UcxRootHubPortChanged");
            UcxRootHubPortChanged(Dev->RootHub);
        }
        InterlockedExchange(&Dev->PortRescanNeeded, 1);
        if (Dev->EnumWorkItem != NULL)
            WdfWorkItemEnqueue(Dev->EnumWorkItem);
    }

    XHCI_UCX_LOG("STEP2 DONE: interrupter 0 armed (IMAN=%08lx), event ring live",
                 XHCI_UCX_READ_REG(&Dev->RuntimeRegisters->Interrupter[0].Iman));
    return STATUS_SUCCESS;
}

/*
 * Quiesce while the WDFINTERRUPT is still connected (WDF disconnects it right
 * after this callback): mask the interrupter, clear Run/Stop, wait HCHalted.
 * TargetState is recorded for the selective-suspend branch (M4).
 */
static
NTSTATUS
NTAPI
XhciUcxEvtDeviceD0ExitPreInterruptsDisabled(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetDevice(Device);
    KIRQL OldIrql;

    if (Dev->OperationalRegisters == NULL || Dev->RuntimeRegisters == NULL)
        return STATUS_SUCCESS;

    /* Let an in-flight enumeration finish (its waits are bounded), then release the slots */
    if (Dev->EnumWorkItem != NULL)
        WdfWorkItemFlush(Dev->EnumWorkItem);
    XhciUcxForgetAllSlots(Dev, TRUE);

    XhciUcxArmInterrupts(Dev, FALSE);

    KeAcquireSpinLock(&Dev->EventRingLock, &OldIrql);
    Dev->ControllerRunning = FALSE;   /* a racing DPC drain now exits early */
    KeReleaseSpinLock(&Dev->EventRingLock, OldIrql);

    (VOID)XhciUcxHaltController(Dev);

    XHCI_UCX_LOG("D0Exit to state %u: controller halted, interrupter masked", (ULONG)TargetState);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
XhciUcxEvtDeviceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(TargetState);
    /* Quiesce happened in D0ExitPreInterruptsDisabled (interrupt still connected there) */
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
XhciUcxEvtDeviceSurpriseRemoval(
    _In_ WDFDEVICE Device)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetDevice(Device);

    /* MMIO may already be gone: stop touching the controller from the DPC */
    Dev->FatalError = TRUE;
    Dev->ControllerRunning = FALSE;
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
    PnpPowerCallbacks.EvtDeviceD0EntryPostInterruptsEnabled = XhciUcxEvtDeviceD0EntryPostInterruptsEnabled;
    PnpPowerCallbacks.EvtDeviceD0ExitPreInterruptsDisabled = XhciUcxEvtDeviceD0ExitPreInterruptsDisabled;
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
    KeInitializeSpinLock(&Dev->EventRingLock);
    KeInitializeEvent(&Dev->CommandEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&Dev->TransferEvent, NotificationEvent, FALSE);

    /* Step 3 enumeration runs in this work item (PASSIVE_LEVEL, not serialized with the device) */
    {
        WDF_WORKITEM_CONFIG WorkItemConfig;
        WDF_OBJECT_ATTRIBUTES WorkItemAttributes;

        WDF_WORKITEM_CONFIG_INIT(&WorkItemConfig, XhciUcxEvtEnumWorkItem);
        WorkItemConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&WorkItemAttributes);
        WorkItemAttributes.ParentObject = Device;
        Status = WdfWorkItemCreate(&WorkItemConfig, &WorkItemAttributes, &Dev->EnumWorkItem);
        if (!NT_SUCCESS(Status))
        {
            XHCI_UCX_LOG("WdfWorkItemCreate failed 0x%08lX", (ULONG)Status);
            return Status;
        }
    }

    /* xHCI contexts/rings want 64-byte alignment; the common buffer is page-aligned anyway */
    WdfDeviceSetAlignmentRequirement(Device, FILE_64_BYTE_ALIGNMENT);

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
