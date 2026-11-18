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
#define XHCI_UCX_SLOT_PAGES            5
#define XHCI_UCX_EP0_RING_TRBS         64
#define XHCI_UCX_SLOT_EP0_RING_OFFSET  (2 * PAGE_SIZE)
#define XHCI_UCX_SLOT_DESC_OFFSET      (2 * PAGE_SIZE + XHCI_UCX_EP0_RING_TRBS * sizeof(XHCI_TRB))
#define XHCI_UCX_DESC_BUFFER_SIZE      512
#define XHCI_UCX_COMMAND_TIMEOUT_MS    5000   /* Address Device on real hardware can take seconds */
#define XHCI_UCX_COMMAND_ABORT_TIMEOUT_MS 5000
#define XHCI_UCX_TRANSFER_TIMEOUT_MS   1000
#define XHCI_UCX_CRCR_CA               0x00000004   /* Command Abort (hardware.h has CS and CRR only) */
#define XHCI_UCX_PORT_RESET_TIMEOUT_MS 500
#define XHCI_UCX_WAIT_SLICE_MS         10
#define XHCI_UCX_INPUT_CTX_ADD_SLOT    (1u << 0)
#define XHCI_UCX_INPUT_CTX_ADD_EP0     (1u << 1)
#define XHCI_UCX_INPUT_CTX_ADD(_dci)   (1u << (_dci))
#define XHCI_UCX_COMMAND_EP_FIELD(_dci) ((ULONG)(_dci) << 16)
#define XHCI_UCX_SLOT_STATE_ADDRESS(_devstate) ((_devstate) & 0xFF)

/*
 * Step 4 bulk path (one bulk IN + one bulk OUT endpoint per slot). Per slot:
 * page 3 = bulk OUT ring (1 KB) | bulk IN ring (1 KB) | CBW (64 B) | CSW (64 B),
 * page 4 = data buffer (one page: INQUIRY / capacity / one logical block).
 * Device Context Index: DCI = 2 * endpoint number + (IN ? 1 : 0); EP0 = 1.
 */
#define XHCI_UCX_BULK_RING_TRBS            64
#define XHCI_UCX_SLOT_BULK_OUT_RING_OFFSET (3 * PAGE_SIZE)
#define XHCI_UCX_SLOT_BULK_IN_RING_OFFSET  (3 * PAGE_SIZE + XHCI_UCX_BULK_RING_TRBS * sizeof(XHCI_TRB))
#define XHCI_UCX_SLOT_CBW_OFFSET           (3 * PAGE_SIZE + 2 * XHCI_UCX_BULK_RING_TRBS * sizeof(XHCI_TRB))
#define XHCI_UCX_SLOT_CSW_OFFSET           (XHCI_UCX_SLOT_CBW_OFFSET + 64)
#define XHCI_UCX_SLOT_DATA_OFFSET          (4 * PAGE_SIZE)
#define XHCI_UCX_DATA_BUFFER_SIZE          PAGE_SIZE
#define XHCI_UCX_BULK_TIMEOUT_MS           2000
#define XHCI_UCX_DMA_MAX_TRANSFER          (64 * 1024)   /* WDF DMA enabler maximum length (one page is used today) */

/* USB Mass Storage Class, Bulk-Only Transport (BOT), SCSI transparent command set */
#define XHCI_UCX_MSC_SUBCLASS_SCSI     0x06
#define XHCI_UCX_MSC_PROTOCOL_BOT      0x50
#define XHCI_UCX_CBW_SIGNATURE         0x43425355u   /* 'USBC' */
#define XHCI_UCX_CSW_SIGNATURE         0x53425355u   /* 'USBS' */
#define XHCI_UCX_CBW_LENGTH            31
#define XHCI_UCX_CSW_LENGTH            13
#define XHCI_UCX_CBW_FLAG_DATA_IN      0x80
#define XHCI_UCX_CSW_STATUS_GOOD       0
#define XHCI_UCX_CSW_STATUS_FAILED     1
#define XHCI_UCX_CSW_STATUS_PHASE      2
#define XHCI_UCX_SCSI_TEST_UNIT_READY  0x00
#define XHCI_UCX_SCSI_REQUEST_SENSE    0x03
#define XHCI_UCX_SCSI_INQUIRY          0x12
#define XHCI_UCX_SCSI_READ_CAPACITY10  0x25
#define XHCI_UCX_SCSI_READ10           0x28
#define XHCI_UCX_INQUIRY_LENGTH        36
#define XHCI_UCX_SENSE_LENGTH          18
#define XHCI_UCX_TUR_RETRIES           3

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
struct _XHCI_UCX_ENDPOINT_CLIENT;

typedef struct _XHCI_UCX_SLOT
{
    BOOLEAN InUse;
    BOOLEAN Addressed;
    UCHAR SlotId;                 /* assigned by Enable Slot, 1..MaxSlots */
    UCHAR PortId;                 /* root port, 1..NumberOfPorts */
    UCHAR Speed;                  /* PORTSC speed id, as programmed into the slot context */
    UCHAR SpeedClass;             /* speed id used for packet-size rules: SUPER for any USB3 port (SSP, PSI ids) */
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

    /* Step 4: configured BOT mass-storage interface (bulk IN + bulk OUT) */
    BOOLEAN Configured;
    UCHAR ConfigurationValue;
    UCHAR InterfaceNumber;
    UCHAR BulkInAddress;          /* bEndpointAddress, 0x81.. */
    UCHAR BulkOutAddress;         /* bEndpointAddress, 0x01.. */
    UCHAR BulkInDci;
    UCHAR BulkOutDci;
    ULONG BulkInMaxPacket;
    ULONG BulkOutMaxPacket;
    ULONG BulkInEnqueue;
    ULONG BulkInCycle;
    ULONG BulkOutEnqueue;
    ULONG BulkOutCycle;
    SIZE_T BulkInRingOffset;
    SIZE_T BulkOutRingOffset;
    SIZE_T CbwOffset;
    SIZE_T CswOffset;
    SIZE_T DataOffset;
    ULONG BotTag;
    UCXENDPOINT BulkIn;
    UCXENDPOINT BulkOut;
    /* M3 step 3: the hub-driven device's configured endpoints by DCI (EndpointLock) */
    struct _XHCI_UCX_ENDPOINT_CLIENT *Endpoints[32];
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
     * event ring (single segment) | ERST | device slot areas. The hardware
     * writes only the event ring, the scratchpads, the output contexts and
     * the slots' IN buffers; everything read back from it is validated.
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
    volatile LONG EnumStopped;    /* set under EventRingLock by D0Exit/SurpriseRemoval: no enqueue, work item exits */
    XHCI_UCX_SLOT Slots[XHCI_UCX_MAX_ENUM_SLOTS];
    UCHAR PortEnumState[XHCI_MAX_PORTS + 1];
    KEVENT CommandEvent;
    ULONGLONG PendingCommandTrb;
    volatile LONG PendingCommandDone;
    ULONG PendingCommandCode;
    UCHAR PendingCommandSlot;
    KEVENT TransferEvent;
    UCHAR PendingTransferSlot;                 /* slot id and DCI the pending TD belongs to */
    UCHAR PendingTransferEpId;
    ULONGLONG PendingDataTrb;                  /* 0 for a single-TRB TD */
    ULONGLONG PendingStatusTrb;                /* the TRB whose event completes the TD */
    volatile LONG PendingTransferDone;
    ULONG PendingTransferCode;
    ULONG PendingTransferResidual;
    volatile LONG PortRescanNeeded;

    BOOLEAN MessageInterrupt;
    BOOLEAN HardwarePrepared;
    BOOLEAN ControllerRunning;
    BOOLEAN InterruptsArmed;
    BOOLEAN FatalError;
    BOOLEAN SelfEnumerate;                     /* UseUcx=2: the M2 client-driven enumeration owns the
                                                  ports; FALSE: the hub driver does, through ucx01000 */
    FAST_MUTEX TransferMutex;                  /* hub-driven: one command/EP0 transfer sequence at a time
                                                  (the pending-completion fields are controller-wide) */
    KSPIN_LOCK EndpointLock;                   /* configured endpoints: rings, in-flight lists, slot table */
    volatile LONG PendingUsbSts;               /* USBSTS bits latched by the ISR for the DPC */
} XHCI_UCX_DEVICE, *PXHCI_UCX_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XHCI_UCX_DEVICE, XhciUcxGetDevice)

/* Used by the event-ring drain before its definition */
static
PXHCI_UCX_SLOT
XhciUcxFindSlotById(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG SlotId);

/* M3 step 3 (defined with the configured-endpoint code) */
struct _XHCI_UCX_ENDPOINT_CLIENT;
static
BOOLEAN
XhciUcxEpHandleTransferEvent(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ PXHCI_UCX_SLOT Slot,
    _In_ ULONG EndpointId,
    _In_ ULONGLONG TrbPointer,
    _In_ ULONG CompletionCode,
    _In_ ULONG Residual,
    _Inout_ PLIST_ENTRY Completed);
static
VOID
XhciUcxEpCompleteList(
    _In_opt_ struct _XHCI_UCX_ENDPOINT_CLIENT *Ep,
    _Inout_ PLIST_ENTRY List);
static
VOID
XhciUcxSlotDropEndpoints(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _Inout_ PXHCI_UCX_SLOT Slot,
    _In_ USBD_STATUS UsbdStatus);
static
NTSTATUS
NTAPI
XhciUcxEvtUsbDeviceEndpointAdd(
    _In_ UCXCONTROLLER Controller,
    _In_ UCXUSBDEVICE UsbDevice,
    _In_ PUCX_ENDPOINT_INFO Info,
    _Inout_ PUCXENDPOINT_INIT EndpointInit);
static
NTSTATUS
NTAPI
XhciUcxEvtUsbDeviceEndpointsConfigure(
    _In_ UCXCONTROLLER Controller,
    _In_ UCXUSBDEVICE UsbDevice,
    _In_ PUCX_ENDPOINTS_CONFIGURE Cfg);

/* -1 = not read yet, 0 = legacy usbport, 1 = UCX client path */
static LONG g_XhciUcxSwitch = -1;
static LONG g_XhciUcxSelfEnumerate = 0;

/*
 * Client context on the UCX root hub object: the root hub callbacks get the
 * UCXROOTHUB handle and find their device through it.
 */
typedef struct _XHCI_UCX_ROOTHUB_CLIENT {
    PXHCI_UCX_DEVICE Dev;
} XHCI_UCX_ROOTHUB_CLIENT, *PXHCI_UCX_ROOTHUB_CLIENT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XHCI_UCX_ROOTHUB_CLIENT, XhciUcxGetRootHubClient)

/* Client contexts on the controller, device and endpoint objects (M3 step 2) */
typedef struct _XHCI_UCX_CONTROLLER_CLIENT {
    PXHCI_UCX_DEVICE Dev;
} XHCI_UCX_CONTROLLER_CLIENT, *PXHCI_UCX_CONTROLLER_CLIENT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XHCI_UCX_CONTROLLER_CLIENT, XhciUcxGetControllerClient)

typedef struct _XHCI_UCX_USBDEVICE_CLIENT {
    PXHCI_UCX_DEVICE Dev;
    PXHCI_UCX_SLOT Slot;          /* NULL until EvtUsbDeviceEnable */
    USHORT PortNumber;
} XHCI_UCX_USBDEVICE_CLIENT, *PXHCI_UCX_USBDEVICE_CLIENT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XHCI_UCX_USBDEVICE_CLIENT, XhciUcxGetUsbDeviceClient)

typedef struct _XHCI_UCX_ENDPOINT_CLIENT {
    PXHCI_UCX_DEVICE Dev;
    UCXUSBDEVICE UsbDevice;
    UCHAR Dci;                    /* 1 = default control endpoint */
    /* M3 step 3: a configured bulk/interrupt/isoch endpoint (everything below is unused for EP0) */
    UCHAR Address;                /* bEndpointAddress */
    UCHAR Type;                   /* USB_ENDPOINT_TYPE_* */
    BOOLEAN DataIn;
    USHORT MaxPacket;
    UCHAR MaxBurst;
    UCHAR Interval;               /* xHCI endpoint context Interval field */
    UCHAR Mult;
    BOOLEAN Configured;           /* in the controller's device context (EndpointLock) */
    BOOLEAN Disabled;             /* never again: being torn down */
    BOOLEAN Halted;               /* the controller halted it; waits for a pipe reset */
    ULONG MaxEsit;
    ULONG MaximumTransferSize;
    UCXENDPOINT Self;
    PXHCI_UCX_SLOT Slot;
    WDFCOMMONBUFFER RingBuffer;   /* one page: the transfer ring */
    PXHCI_TRB Ring;
    ULONGLONG RingPa;
    ULONG Enqueue;                /* producer index, 0..254 */
    ULONG Dequeue;                /* index after the oldest TD still on the ring */
    ULONG Cycle;
    LIST_ENTRY InFlight;          /* XHCI_UCX_TD_CTX in ring order (EndpointLock) */
    WDFQUEUE Queue;               /* parallel: the class library forwards transfers here */
    WDFQUEUE Waiting;             /* manual: transfers the ring has no room for yet */
} XHCI_UCX_ENDPOINT_CLIENT, *PXHCI_UCX_ENDPOINT_CLIENT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XHCI_UCX_ENDPOINT_CLIENT, XhciUcxGetEndpointClient)

/* Context on each endpoint queue: the endpoint it serves */
typedef struct _XHCI_UCX_QUEUE_CLIENT {
    PXHCI_UCX_ENDPOINT_CLIENT Ep;
} XHCI_UCX_QUEUE_CLIENT, *PXHCI_UCX_QUEUE_CLIENT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XHCI_UCX_QUEUE_CLIENT, XhciUcxGetQueueClient)

/* Context on each forwarded transfer request: its TD on the ring */
typedef struct _XHCI_UCX_TD_CTX {
    LIST_ENTRY Link;
    WDFREQUEST Request;
    PXHCI_UCX_ENDPOINT_CLIENT Ep;
    ULONG FirstIndex;
    ULONG LastIndex;
    ULONG TrbCount;
    ULONG Length;
    ULONG Transferred;
    USBD_STATUS UsbdStatus;
    BOOLEAN ShortSeen;
    BOOLEAN DataIn;
} XHCI_UCX_TD_CTX, *PXHCI_UCX_TD_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XHCI_UCX_TD_CTX, XhciUcxGetTdCtx)

/* Hub class feature selectors (USB 2.0 11.24.2, USB 3.2 10.16.2) */
#define XHCI_UCX_FEATURE_C_HUB_LOCAL_POWER      0
#define XHCI_UCX_FEATURE_C_HUB_OVER_CURRENT     1
#define XHCI_UCX_FEATURE_PORT_CONNECTION        0
#define XHCI_UCX_FEATURE_PORT_ENABLE            1
#define XHCI_UCX_FEATURE_PORT_SUSPEND           2
#define XHCI_UCX_FEATURE_PORT_OVER_CURRENT      3
#define XHCI_UCX_FEATURE_PORT_RESET             4
#define XHCI_UCX_FEATURE_PORT_LINK_STATE        5
#define XHCI_UCX_FEATURE_PORT_POWER             8
#define XHCI_UCX_FEATURE_C_PORT_CONNECTION      16
#define XHCI_UCX_FEATURE_C_PORT_ENABLE          17
#define XHCI_UCX_FEATURE_C_PORT_SUSPEND         18
#define XHCI_UCX_FEATURE_C_PORT_OVER_CURRENT    19
#define XHCI_UCX_FEATURE_C_PORT_RESET           20
#define XHCI_UCX_FEATURE_PORT_U1_TIMEOUT        23
#define XHCI_UCX_FEATURE_PORT_U2_TIMEOUT        24
#define XHCI_UCX_FEATURE_C_PORT_LINK_STATE      25
#define XHCI_UCX_FEATURE_C_PORT_CONFIG_ERROR    26
#define XHCI_UCX_FEATURE_PORT_REMOTE_WAKE_MASK  27
#define XHCI_UCX_FEATURE_BH_PORT_RESET          28
#define XHCI_UCX_FEATURE_C_BH_PORT_RESET        29
#define XHCI_UCX_FEATURE_FORCE_LINKPM_ACCEPT    30

/* Hub descriptor wHubCharacteristics */
#define XHCI_UCX_HUB_CHAR_POWER_INDIVIDUAL      0x0001
#define XHCI_UCX_HUB_CHAR_NO_POWER_SWITCHING    0x0002
#define XHCI_UCX_HUB_CHAR_OVERCURRENT_PER_PORT  0x0008
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

    /* 1 = UCX path, hub-driven: ucx01000 owns the root hub PDO and usbhub binds to it (M3).
     * 2 = UCX path, client-driven enumeration and BOT probe (the M2 demo legs).
     * Anything else = legacy usbport. */
    g_XhciUcxSwitch = (UseUcx == 1 || UseUcx == 2) ? 1 : 0;
    g_XhciUcxSelfEnumerate = (UseUcx == 2) ? 1 : 0;
    if (g_XhciUcxSwitch)
        XHCI_UCX_LOG("UseUcx=%lu: selecting the UCX client path, %s (legacy usbport is the fallback)",
                     UseUcx, g_XhciUcxSelfEnumerate ? "client-driven enumeration (M2 demo)"
                                                    : "hub-driven through the ucx01000 root hub PDO");

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

        /* Only the 4-byte header must fit for the walk to continue; the
         * requested capability must fit entirely before it is handed back. */
        if (!XhciUcxRangeInBar(Dev, Offset, sizeof(ULONG)))
            return 0;

        CapHeader = XhciUcxReadMmio(Dev, Offset);
        if (XHCI_EXT_CAP_ID(CapHeader) == CapabilityId)
            return XhciUcxRangeInBar(Dev, Offset, CapabilityLength) ? Offset : 0;

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

        if (!XhciUcxRangeInBar(Dev, Offset, sizeof(ULONG)))
            break;

        CapHeader = XhciUcxReadMmio(Dev, Offset);

        if (XHCI_EXT_CAP_ID(CapHeader) == XHCI_EXT_CAP_ID_PROTOCOL &&
            XhciUcxRangeInBar(Dev, Offset, sizeof(XHCI_PROTOCOL_CAPABILITY)))
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
                                    XHCI_UCX_DMA_MAX_TRANSFER);
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

    /* Hub-driven (UseUcx=1): the change bits are the hub driver's to read
     * (GET_PORT_STATUS) and to clear (CLEAR_FEATURE C_PORT_*); acknowledging
     * them here would hide the change from it. Client-driven (UseUcx=2): we
     * are the hub and acknowledge now. */
    if (Changes != 0 && Dev->SelfEnumerate)
    {
        /* Acknowledge (W1C) the change bits while preserving every plain RW bit
         * (PP, PIC, WCE/WDE/WOE): a bare write of the change mask writes PP=0
         * and powers the port off on a PPC controller. PED/PR/WPR/LWS are
         * write-1-to-act bits and are always written as 0 here. */
        XHCI_UCX_WRITE_REG(PortScReg,
                           (PortSc & ~(XHCI_PORTSC_CHANGE_MASK | XHCI_PORTSC_PED | XHCI_PORTSC_PR |
                                       XHCI_PORTSC_WPR | XHCI_PORTSC_LWS)) | Changes);
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
    LIST_ENTRY Completed;                      /* configured-endpoint TDs finished by this drain */

    InitializeListHead(&Completed);
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

                if (CompletionCode == XHCI_COMPLETION_COMMAND_RING_STOPPED)
                {
                    /* Result of our own CRCR.CA after a command timeout; the abort path polls CRR */
                    XHCI_UCX_TRACE("command ring stopped");
                    break;
                }
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
                {
                    PXHCI_UCX_SLOT EventSlot = XhciUcxFindSlotById(Dev, SlotId);

                    if (EventSlot == NULL)
                    {
                        XHCI_UCX_LOG("ignoring transfer event for unknown slot %lu endpoint %lu code=%lu",
                                     SlotId, EndpointId, CompletionCode);
                        break;
                    }
                    /* A configured endpoint of a hub-driven device (M3 step 3) */
                    if (XhciUcxEpHandleTransferEvent(Dev, EventSlot, EndpointId, TrbPointer, CompletionCode,
                                                     Residual, &Completed))
                        break;
                }
/*
                 * Steps 3/4: the only TDs are the ones the enumeration work item
                 * issued, one at a time, on EP0 or on the slot's two bulk endpoints.
                 * The event must name the pending slot, the pending DCI and one of
                 * the pending TRB pointers; anything else is device-written noise.
                 */
                if (SlotId == Dev->PendingTransferSlot && EndpointId == Dev->PendingTransferEpId &&
                    TrbPointer != 0 &&
                    InterlockedCompareExchange(&Dev->PendingTransferDone, 0, 0) == 0 &&
                    (TrbPointer == Dev->PendingDataTrb || TrbPointer == Dev->PendingStatusTrb))
                {
                    if (TrbPointer == Dev->PendingDataTrb || Dev->PendingDataTrb == 0)
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
    XhciUcxEpCompleteList(NULL, &Completed);
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
        Slot->BulkOutRingOffset = Base + XHCI_UCX_SLOT_BULK_OUT_RING_OFFSET;
        Slot->BulkInRingOffset = Base + XHCI_UCX_SLOT_BULK_IN_RING_OFFSET;
        Slot->CbwOffset = Base + XHCI_UCX_SLOT_CBW_OFFSET;
        Slot->CswOffset = Base + XHCI_UCX_SLOT_CSW_OFFSET;
        Slot->DataOffset = Base + XHCI_UCX_SLOT_DATA_OFFSET;
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

/* Input endpoint context for any DCI (slot context is entry 1, EP DCI d is entry d + 1) */
static
PXHCI_ENDPOINT_CONTEXT
XhciUcxInputEndpointContext(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ PXHCI_UCX_SLOT Slot,
    _In_ ULONG Dci)
{
    return (PXHCI_ENDPOINT_CONTEXT)(Dev->HcVa + Slot->InputContextOffset + ((SIZE_T)Dci + 1) * Dev->ContextSize);
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
    ULONGLONG Deadline = KeQueryInterruptTime() + (ULONGLONG)TimeoutMs * 10000;

    Slice.QuadPart = -(LONGLONG)XHCI_UCX_WAIT_SLICE_MS * 10000;

    while (InterlockedCompareExchange(Done, 0, 0) == 0)
    {
        (VOID)KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &Slice);
        if (InterlockedCompareExchange(Done, 0, 0) != 0)
            break;

        if (Dev->FatalError || !Dev->ControllerRunning)
            return FALSE;

        /* Manual drain (emulator poll quirk): a port change seen here is reported
         * like the DPC does, and the work item rescans after this transfer */
        if (XhciUcxDrainEventRing(Dev, FALSE))
        {
            InterlockedExchange(&Dev->PortRescanNeeded, 1);
            if (Dev->RootHub != NULL)
                UcxRootHubPortChanged(Dev->RootHub);
        }

        /* Deadline in interrupt time: a timed wait rounds up to clock ticks */
        if (InterlockedCompareExchange(Done, 0, 0) == 0 && KeQueryInterruptTime() >= Deadline)
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

/*
 * Command timed out: xHCI 4.6.1.2. Set CRCR.CA, wait for CRR = 0 (the Command
 * Ring Stopped event is drained meanwhile), then point CRCR at the producer
 * position so the stuck TRB is abandoned and the next doorbell restarts the
 * ring. If the controller does not stop, it is treated as fatal.
 */
static
VOID
XhciUcxAbortCommandRing(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    PXHCI_OPERATIONAL_REGISTERS Op = Dev->OperationalRegisters;
    ULONGLONG Restart;
    ULONGLONG Deadline = KeQueryInterruptTime() + (ULONGLONG)XHCI_UCX_COMMAND_ABORT_TIMEOUT_MS * 10000;
    LARGE_INTEGER Slice;

    Slice.QuadPart = -(LONGLONG)1 * 10000;   /* 1 ms */

    /* CA is honoured only while CRR = 1; while CRR = 1 the pointer bits of a write are ignored */
    if (XHCI_UCX_READ_REG(&Op->CrCrLow) & XHCI_CRCR_CRR)
    {
        XHCI_UCX_WRITE_REG(&Op->CrCrLow, XHCI_UCX_CRCR_CA);
        XHCI_UCX_WRITE_REG(&Op->CrCrHigh, 0);

        while (XHCI_UCX_READ_REG(&Op->CrCrLow) & XHCI_CRCR_CRR)
        {
            if (Dev->FatalError || KeQueryInterruptTime() >= Deadline)
            {
                XHCI_UCX_LOG("command ring did not stop after CRCR.CA within %u ms: controller marked fatal",
                             XHCI_UCX_COMMAND_ABORT_TIMEOUT_MS);
                Dev->FatalError = TRUE;
                return;
            }
            KeDelayExecutionThread(KernelMode, FALSE, &Slice);
            (VOID)XhciUcxDrainEventRing(Dev, FALSE);
        }
    }

    /* Ring stopped: restart at the producer position with the producer cycle (RCS) */
    Restart = (ULONGLONG)XhciUcxHcPhysical(Dev, Dev->CmdRingOffset).QuadPart +
              (ULONGLONG)Dev->CmdRingEnqueue * sizeof(XHCI_TRB);
    XHCI_UCX_WRITE_REG(&Op->CrCrLow, (ULONG)(Restart & 0xFFFFFFFF) | (Dev->CmdRingCycle & XHCI_TRB_CYCLE));
    XHCI_UCX_WRITE_REG(&Op->CrCrHigh, (ULONG)(Restart >> 32));
    XHCI_UCX_LOG("command ring aborted and restarted at index %lu", Dev->CmdRingEnqueue);
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
        XHCI_UCX_LOG("command type %lu timed out after %u ms", TrbType, XHCI_UCX_COMMAND_TIMEOUT_MS);
        if (Dev->ControllerRunning && !Dev->FatalError)
            XhciUcxAbortCommandRing(Dev);
        Dev->PendingCommandTrb = 0;
        return STATUS_IO_TIMEOUT;
    }
    Dev->PendingCommandTrb = 0;

    *CompletionCode = Dev->PendingCommandCode;
    if (SlotIdOut != NULL)
        *SlotIdOut = Dev->PendingCommandSlot;

    return (Dev->PendingCommandCode == XHCI_COMPLETION_SUCCESS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

/* Transfer ring: zero, link TRB back to the base (toggle cycle), producer cycle 1 */
static
VOID
XhciUcxInitTransferRing(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ SIZE_T RingOffset,
    _In_ ULONG Trbs,
    _Out_ PULONG Enqueue,
    _Out_ PULONG Cycle)
{
    PXHCI_TRB Ring = (PXHCI_TRB)(Dev->HcVa + RingOffset);
    ULONGLONG RingPa = (ULONGLONG)XhciUcxHcPhysical(Dev, RingOffset).QuadPart;

    RtlZeroMemory(Ring, (SIZE_T)Trbs * sizeof(XHCI_TRB));
    Ring[Trbs - 1].Parameter1 = (ULONG)(RingPa & 0xFFFFFFFF);
    Ring[Trbs - 1].Parameter2 = (ULONG)(RingPa >> 32);
    Ring[Trbs - 1].Control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    KeMemoryBarrier();
    *Enqueue = 0;
    *Cycle = 1;
}

/* Next producer TRB of a transfer ring; advances over the link TRB (refreshing its cycle bit) */
static
PXHCI_TRB
XhciUcxRingNextTrb(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ SIZE_T RingOffset,
    _In_ ULONG Trbs,
    _Inout_ PULONG Enqueue,
    _Inout_ PULONG Cycle,
    _Out_ PULONGLONG TrbPa,
    _Out_ PULONG CycleOut)
{
    PXHCI_TRB Ring = (PXHCI_TRB)(Dev->HcVa + RingOffset);
    PXHCI_TRB Trb = &Ring[*Enqueue];

    *TrbPa = (ULONGLONG)XhciUcxHcPhysical(Dev, RingOffset).QuadPart + (ULONGLONG)*Enqueue * sizeof(XHCI_TRB);
    *CycleOut = *Cycle & XHCI_TRB_CYCLE;

    (*Enqueue)++;
    if (*Enqueue >= Trbs - 1)
    {
        PXHCI_TRB Link = &Ring[Trbs - 1];

        Link->Control = (Link->Control & ~XHCI_TRB_CYCLE) | (*Cycle & XHCI_TRB_CYCLE);
        KeMemoryBarrier();
        *Enqueue = 0;
        *Cycle ^= 1;
    }

    return Trb;
}

static
VOID
XhciUcxInitEp0Ring(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _Inout_ PXHCI_UCX_SLOT Slot)
{
    XhciUcxInitTransferRing(Dev, Slot->Ep0RingOffset, XHCI_UCX_EP0_RING_TRBS, &Slot->Ep0Enqueue, &Slot->Ep0Cycle);
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
    return XhciUcxRingNextTrb(Dev, Slot->Ep0RingOffset, XHCI_UCX_EP0_RING_TRBS,
                              &Slot->Ep0Enqueue, &Slot->Ep0Cycle, TrbPa, Cycle);
}

/*
 * One control transfer on EP0 (Setup / optional Data / Status TD), data in
 * one of the slot's buffers: the 512-byte descriptor area or the data page
 * (BufferOffset selects it and bounds Length). Returns the bytes transferred.
 */
static
NTSTATUS
XhciUcxEp0Control(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _Inout_ PXHCI_UCX_SLOT Slot,
    _In_ const USB_DEFAULT_PIPE_SETUP_PACKET *Setup,
    _In_ SIZE_T BufferOffset,
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

    if (BufferOffset == Slot->DataOffset ? (Length > PAGE_SIZE) : (Length > XHCI_UCX_DESC_BUFFER_SIZE))
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
        ULONGLONG BufferPa = (ULONGLONG)XhciUcxHcPhysical(Dev, BufferOffset).QuadPart;

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

    Dev->PendingTransferSlot = Slot->SlotId;
    Dev->PendingTransferEpId = 1;
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
    ULONGLONG Deadline = KeQueryInterruptTime() + (ULONGLONG)XHCI_UCX_PORT_RESET_TIMEOUT_MS * 10000;

    *SpeedOut = 0;

    if ((Value & XHCI_PORTSC_CCS) == 0)
        return FALSE;

    if (PortId <= XHCI_MAX_PORTS && Dev->PortProtocol[PortId] != 3)
        XhciUcxWritePortSc(Dev, PortId, XHCI_PORTSC_PR);

    Slice.QuadPart = -(LONGLONG)1 * 10000;   /* 1 ms (rounds up to a clock tick; the deadline is real time) */
    for (;;)
    {
        Value = XHCI_UCX_READ_REG(PortSc);
        if ((Value & XHCI_PORTSC_CCS) == 0)
            return FALSE;
        if ((Value & XHCI_PORTSC_PED) && (Value & XHCI_PORTSC_PR) == 0)
            break;
        if (KeQueryInterruptTime() >= Deadline)
            break;
        KeDelayExecutionThread(KernelMode, FALSE, &Slice);
    }

    if ((Value & XHCI_PORTSC_PED) == 0 || (Value & XHCI_PORTSC_PR) != 0)
    {
        XHCI_UCX_LOG("port %lu did not reach Enabled within %u ms (PORTSC=%08lx)",
                     PortId, XHCI_UCX_PORT_RESET_TIMEOUT_MS, Value);
        return FALSE;
    }

    /* Acknowledge only the change bits this reset produced (PRC, PEC, WRC), keeping PP and
     * the other RW bits; CSC/OCC/PLC/CEC stay for the port-change path to see and report */
    if (Value & (XHCI_PORTSC_PRC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC))
    {
        XHCI_UCX_WRITE_REG(PortSc,
                           (Value & ~(XHCI_PORTSC_CHANGE_MASK | XHCI_PORTSC_PED | XHCI_PORTSC_PR |
                                      XHCI_PORTSC_WPR | XHCI_PORTSC_LWS)) |
                           (Value & (XHCI_PORTSC_PRC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC)));
    }

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


/* --------------------------------------------------------------------------
 * Step 4: client-driven bulk path. For a device whose first configuration
 * carries a Mass Storage / SCSI transparent / Bulk-Only Transport interface,
 * the controller driver configures its bulk IN and bulk OUT endpoints
 * (Configure Endpoint + SET_CONFIGURATION) and runs INQUIRY, TEST UNIT READY,
 * READ CAPACITY(10) and READ(10) of logical block 0 over CBW / data / CSW.
 * Same serialization as step 3 (one TD outstanding, the DPC matches it by
 * slot, DCI and TRB pointer). Every device-written byte is validated:
 * descriptor walk bounds (SEC-1), endpoint sizes per speed, CSW signature /
 * tag / residue, capacity block size. Only counts and status reach COM1.
 * -------------------------------------------------------------------------- */

/* One bulk TD = one Normal TRB (IOC; ISP on IN); the data lives in the HC buffer */
static
NTSTATUS
XhciUcxBulkTransfer(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _Inout_ PXHCI_UCX_SLOT Slot,
    _In_ BOOLEAN DataIn,
    _In_ SIZE_T BufferOffset,
    _In_ ULONG Length,
    _Out_ PULONG Transferred,
    _Out_ PULONG CompletionCode)
{
    SIZE_T RingOffset = DataIn ? Slot->BulkInRingOffset : Slot->BulkOutRingOffset;
    PULONG Enqueue = DataIn ? &Slot->BulkInEnqueue : &Slot->BulkOutEnqueue;
    PULONG RingCycle = DataIn ? &Slot->BulkInCycle : &Slot->BulkOutCycle;
    UCHAR Dci = DataIn ? Slot->BulkInDci : Slot->BulkOutDci;
    PXHCI_TRB Trb;
    ULONGLONG TrbPa;
    ULONGLONG BufferPa;
    ULONG Cycle;
    ULONG Control;
    ULONG Residual;

    *Transferred = 0;
    *CompletionCode = XHCI_COMPLETION_INVALID;

    if (Length == 0 || Length > XHCI_UCX_DATA_BUFFER_SIZE || BufferOffset + Length > Dev->HcLength)
        return STATUS_INVALID_PARAMETER;
    if (Dci == 0 || !Slot->Configured)
        return STATUS_INVALID_DEVICE_STATE;
    if (!Dev->ControllerRunning || Dev->FatalError)
        return STATUS_DEVICE_NOT_READY;

    KeClearEvent(&Dev->TransferEvent);
    Dev->PendingTransferCode = XHCI_COMPLETION_INVALID;
    Dev->PendingTransferResidual = Length;
    InterlockedExchange(&Dev->PendingTransferDone, 0);

    BufferPa = (ULONGLONG)XhciUcxHcPhysical(Dev, BufferOffset).QuadPart;
    Trb = XhciUcxRingNextTrb(Dev, RingOffset, XHCI_UCX_BULK_RING_TRBS, Enqueue, RingCycle, &TrbPa, &Cycle);
    Trb->Parameter1 = (ULONG)(BufferPa & 0xFFFFFFFF);
    Trb->Parameter2 = (ULONG)(BufferPa >> 32);
    Trb->Status = Length & XHCI_TRB_LEN_MASK;          /* TD size 0: last TRB of the TD */
    Control = (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC | (DataIn ? XHCI_TRB_ISP : 0);

    Dev->PendingTransferSlot = Slot->SlotId;
    Dev->PendingTransferEpId = Dci;
    Dev->PendingDataTrb = 0;
    Dev->PendingStatusTrb = TrbPa;
    KeMemoryBarrier();

    /* Cycle bit last, then the doorbell for this DCI */
    Trb->Control = Control | Cycle;
    KeMemoryBarrier();
    XHCI_UCX_WRITE_REG(&Dev->DoorbellArray->Doorbell[Slot->SlotId], Dci);

    if (!XhciUcxWaitForCompletion(Dev, &Dev->TransferEvent, &Dev->PendingTransferDone, XHCI_UCX_BULK_TIMEOUT_MS))
    {
        ULONG StopCode = 0;
        ULONG DeqCode = 0;
        NTSTATUS StopStatus;

        Dev->PendingStatusTrb = 0;
        XHCI_UCX_LOG("STEP4 slot %u: bulk %s transfer (%lu bytes) timed out after %u ms",
                     Slot->SlotId, DataIn ? "IN" : "OUT", Length, XHCI_UCX_BULK_TIMEOUT_MS);

        /* Take the TD off the endpoint: Stop Endpoint, then dequeue = producer position */
        if (Dev->ControllerRunning && !Dev->FatalError)
        {
            ULONG Fields = XHCI_COMMAND_SLOT_FIELD(Slot->SlotId) | XHCI_UCX_COMMAND_EP_FIELD(Dci);

            StopStatus = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_STOP_EP, 0, Fields, &StopCode, NULL);
            if (NT_SUCCESS(StopStatus))
            {
                ULONGLONG Dequeue = (ULONGLONG)XhciUcxHcPhysical(Dev, RingOffset).QuadPart +
                                    (ULONGLONG)*Enqueue * sizeof(XHCI_TRB);

                StopStatus = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_SET_DEQ, Dequeue | (*RingCycle & XHCI_TRB_CYCLE),
                                                Fields, &DeqCode, NULL);
            }
            XHCI_UCX_LOG("STEP4 slot %u: endpoint 0x%02x stopped after timeout rc=0x%08lX (stop code=%lu, dequeue code=%lu)",
                         Slot->SlotId, DataIn ? Slot->BulkInAddress : Slot->BulkOutAddress,
                         (ULONG)StopStatus, StopCode, DeqCode);
        }
        return STATUS_IO_TIMEOUT;
    }
    Dev->PendingStatusTrb = 0;

    *CompletionCode = Dev->PendingTransferCode;
    if (*CompletionCode != XHCI_COMPLETION_SUCCESS && *CompletionCode != XHCI_COMPLETION_SHORT_PACKET)
    {
        if (*CompletionCode != XHCI_COMPLETION_STALL_ERROR)
        {
            XHCI_UCX_LOG("STEP4 slot %u: bulk %s transfer (%lu bytes) failed, completion code %lu",
                         Slot->SlotId, DataIn ? "IN" : "OUT", Length, *CompletionCode);
        }
        return STATUS_DEVICE_DATA_ERROR;
    }

    Residual = Dev->PendingTransferResidual;
    if (Residual > Length)
        Residual = Length;   /* device-written field: clamp */
    *Transferred = Length - Residual;
    return STATUS_SUCCESS;
}

/*
 * Halted bulk endpoint (STALL): Reset Endpoint, Set TR Dequeue Pointer to the
 * producer position, CLEAR_FEATURE(ENDPOINT_HALT) so the device resets its
 * data toggle / sequence number.
 */
static
NTSTATUS
XhciUcxRecoverHaltedEndpoint(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _Inout_ PXHCI_UCX_SLOT Slot,
    _In_ BOOLEAN DataIn)
{
    SIZE_T RingOffset = DataIn ? Slot->BulkInRingOffset : Slot->BulkOutRingOffset;
    ULONG Enqueue = DataIn ? Slot->BulkInEnqueue : Slot->BulkOutEnqueue;
    ULONG Cycle = DataIn ? Slot->BulkInCycle : Slot->BulkOutCycle;
    UCHAR Dci = DataIn ? Slot->BulkInDci : Slot->BulkOutDci;
    UCHAR Address = DataIn ? Slot->BulkInAddress : Slot->BulkOutAddress;
    ULONG Fields = XHCI_COMMAND_SLOT_FIELD(Slot->SlotId) | XHCI_UCX_COMMAND_EP_FIELD(Dci);
    USB_DEFAULT_PIPE_SETUP_PACKET Setup;
    ULONGLONG Dequeue;
    ULONG Code = 0;
    ULONG Transferred = 0;
    NTSTATUS Status;

    Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_RESET_EP, 0, Fields, &Code, NULL);
    XHCI_UCX_LOG("STEP4 slot %u: endpoint 0x%02x halted; Reset Endpoint rc=0x%08lX code=%lu",
                 Slot->SlotId, Address, (ULONG)Status, Code);
    if (!NT_SUCCESS(Status))
        return Status;

    Dequeue = (ULONGLONG)XhciUcxHcPhysical(Dev, RingOffset).QuadPart + (ULONGLONG)Enqueue * sizeof(XHCI_TRB);
    Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_SET_DEQ, Dequeue | (Cycle & XHCI_TRB_CYCLE), Fields, &Code, NULL);
    XHCI_UCX_LOG("STEP4 slot %u: Set TR Dequeue Pointer rc=0x%08lX code=%lu", Slot->SlotId, (ULONG)Status, Code);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&Setup, sizeof(Setup));
    Setup.bmRequestType.B = 0x02;                      /* host-to-device, standard, endpoint */
    Setup.bRequest = USB_REQUEST_CLEAR_FEATURE;
    Setup.wValue.W = USB_FEATURE_ENDPOINT_STALL;
    Setup.wIndex.W = Address;
    Setup.wLength = 0;
    Status = XhciUcxEp0Control(Dev, Slot, &Setup, Slot->DescriptorOffset, 0, FALSE, &Transferred);
    XHCI_UCX_LOG("STEP4 slot %u: CLEAR_FEATURE(ENDPOINT_HALT) rc=0x%08lX", Slot->SlotId, (ULONG)Status);
    return Status;
}

static
ULONG
XhciUcxReadLe32(
    _In_reads_bytes_(4) const UCHAR *Bytes)
{
    return (ULONG)Bytes[0] | ((ULONG)Bytes[1] << 8) | ((ULONG)Bytes[2] << 16) | ((ULONG)Bytes[3] << 24);
}

static
ULONG
XhciUcxReadBe32(
    _In_reads_bytes_(4) const UCHAR *Bytes)
{
    return ((ULONG)Bytes[0] << 24) | ((ULONG)Bytes[1] << 16) | ((ULONG)Bytes[2] << 8) | (ULONG)Bytes[3];
}

static
VOID
XhciUcxWriteLe32(
    _Out_writes_bytes_(4) PUCHAR Bytes,
    _In_ ULONG Value)
{
    Bytes[0] = (UCHAR)Value;
    Bytes[1] = (UCHAR)(Value >> 8);
    Bytes[2] = (UCHAR)(Value >> 16);
    Bytes[3] = (UCHAR)(Value >> 24);
}

/*
 * One BOT command: CBW on bulk OUT, optional data phase in the slot's data
 * page, CSW on bulk IN. A STALL in the data phase is recovered and the CSW
 * is still read (BOT 6.7.2/6.7.3). Returns success when a valid CSW arrived;
 * *CswStatus carries the command status (0 good, 1 failed, 2 phase error).
 */
static
NTSTATUS
XhciUcxBotCommand(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _Inout_ PXHCI_UCX_SLOT Slot,
    _In_reads_bytes_(CdbLength) const UCHAR *Cdb,
    _In_ UCHAR CdbLength,
    _In_ BOOLEAN DataIn,
    _In_ ULONG DataLength,
    _Out_ PULONG DataTransferred,
    _Out_ PUCHAR CswStatus)
{
    PUCHAR Cbw = Dev->HcVa + Slot->CbwOffset;
    PUCHAR Csw = Dev->HcVa + Slot->CswOffset;
    ULONG Tag = ++Slot->BotTag;
    ULONG Transferred = 0;
    ULONG Code = 0;
    ULONG CswResidue;
    NTSTATUS Status;

    *DataTransferred = 0;
    *CswStatus = 0xFF;

    if (CdbLength == 0 || CdbLength > 16 || DataLength > XHCI_UCX_DATA_BUFFER_SIZE)
        return STATUS_INVALID_PARAMETER;

    /* Command Block Wrapper (31 bytes, little-endian fields) */
    RtlZeroMemory(Cbw, 64);
    XhciUcxWriteLe32(&Cbw[0], XHCI_UCX_CBW_SIGNATURE);
    XhciUcxWriteLe32(&Cbw[4], Tag);
    XhciUcxWriteLe32(&Cbw[8], DataLength);
    Cbw[12] = (DataLength != 0 && DataIn) ? XHCI_UCX_CBW_FLAG_DATA_IN : 0;
    Cbw[13] = 0;                                       /* LUN 0 */
    Cbw[14] = CdbLength;
    RtlCopyMemory(&Cbw[15], Cdb, CdbLength);
    if (DataIn && DataLength != 0)
        RtlZeroMemory(Dev->HcVa + Slot->DataOffset, DataLength);
    KeMemoryBarrier();

    Status = XhciUcxBulkTransfer(Dev, Slot, FALSE, Slot->CbwOffset, XHCI_UCX_CBW_LENGTH, &Transferred, &Code);
    if (!NT_SUCCESS(Status) || Transferred != XHCI_UCX_CBW_LENGTH)
    {
        XHCI_UCX_LOG("STEP4 slot %u: CBW phase failed rc=0x%08lX code=%lu sent=%lu (opcode %02x)",
                     Slot->SlotId, (ULONG)Status, Code, Transferred, Cdb[0]);
        return NT_SUCCESS(Status) ? STATUS_DEVICE_PROTOCOL_ERROR : Status;
    }

    if (DataLength != 0)
    {
        Status = XhciUcxBulkTransfer(Dev, Slot, DataIn, Slot->DataOffset, DataLength, DataTransferred, &Code);
        if (!NT_SUCCESS(Status))
        {
            if (Code != XHCI_COMPLETION_STALL_ERROR)
                return Status;
            Status = XhciUcxRecoverHaltedEndpoint(Dev, Slot, DataIn);
            if (!NT_SUCCESS(Status))
                return Status;
            *DataTransferred = 0;                      /* unknown after a stall: trust the CSW residue */
        }
    }

    /* Command Status Wrapper (13 bytes); one retry after a STALL on bulk IN */
    RtlZeroMemory(Csw, 64);
    KeMemoryBarrier();
    Status = XhciUcxBulkTransfer(Dev, Slot, TRUE, Slot->CswOffset, XHCI_UCX_CSW_LENGTH, &Transferred, &Code);
    if (!NT_SUCCESS(Status) && Code == XHCI_COMPLETION_STALL_ERROR)
    {
        Status = XhciUcxRecoverHaltedEndpoint(Dev, Slot, TRUE);
        if (NT_SUCCESS(Status))
            Status = XhciUcxBulkTransfer(Dev, Slot, TRUE, Slot->CswOffset, XHCI_UCX_CSW_LENGTH, &Transferred, &Code);
    }
    if (!NT_SUCCESS(Status) || Transferred != XHCI_UCX_CSW_LENGTH)
    {
        XHCI_UCX_LOG("STEP4 slot %u: CSW phase failed rc=0x%08lX code=%lu got=%lu (opcode %02x)",
                     Slot->SlotId, (ULONG)Status, Code, Transferred, Cdb[0]);
        return NT_SUCCESS(Status) ? STATUS_DEVICE_PROTOCOL_ERROR : Status;
    }

    KeMemoryBarrier();
    CswResidue = XhciUcxReadLe32(&Csw[8]);
    if (XhciUcxReadLe32(&Csw[0]) != XHCI_UCX_CSW_SIGNATURE || XhciUcxReadLe32(&Csw[4]) != Tag ||
        CswResidue > DataLength)
    {
        XHCI_UCX_LOG("STEP4 slot %u: SEC-1 reject, CSW invalid (signature ok=%u tag ok=%u residue=%lu of %lu)",
                     Slot->SlotId,
                     (XhciUcxReadLe32(&Csw[0]) == XHCI_UCX_CSW_SIGNATURE) ? 1 : 0,
                     (XhciUcxReadLe32(&Csw[4]) == Tag) ? 1 : 0, CswResidue, DataLength);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    if (*DataTransferred > DataLength - CswResidue)
        *DataTransferred = DataLength - CswResidue;    /* the smaller of controller count and device residue */
    *CswStatus = Csw[12];

    if (*CswStatus == XHCI_UCX_CSW_STATUS_PHASE)
    {
        XHCI_UCX_LOG("STEP4 slot %u: CSW phase error (opcode %02x)", Slot->SlotId, Cdb[0]);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return STATUS_SUCCESS;
}

/* Legal bulk wMaxPacketSize for the negotiated speed (USB 2.0 5.8.3, USB 3.2 9.6.6); 0 = illegal */
static
ULONG
XhciUcxBulkMaxPacketLegal(
    _In_ ULONG Speed,
    _In_ USHORT wMaxPacketSize)
{
    ULONG Mps = wMaxPacketSize & 0x7FF;

    switch (Speed)
    {
        case XHCI_PORTSC_SPEED_SUPER:
            return (Mps == 1024) ? Mps : 0;
        case XHCI_PORTSC_SPEED_HIGH:
            return (Mps == 512) ? Mps : 0;
        case XHCI_PORTSC_SPEED_FULL:
            return (Mps == 8 || Mps == 16 || Mps == 32 || Mps == 64) ? Mps : 0;
        default:
            return 0;                                  /* low speed has no bulk endpoints */
    }
}

typedef struct _XHCI_UCX_BOT_INTERFACE
{
    UCHAR ConfigurationValue;
    UCHAR InterfaceNumber;
    UCHAR InterfaceCount;
    UCHAR FirstClass[3];          /* class/subclass/protocol of the first interface, for the skip line */
    UCHAR BulkInAddress;
    UCHAR BulkOutAddress;
    USHORT BulkInMaxPacket;
    USHORT BulkOutMaxPacket;
} XHCI_UCX_BOT_INTERFACE, *PXHCI_UCX_BOT_INTERFACE;

/*
 * Walk a configuration descriptor set (SEC-1: every descriptor header is
 * bounds-checked against the bytes actually received) and find the first
 * Mass Storage / SCSI / BOT interface with one bulk IN and one bulk OUT endpoint.
 */
static
BOOLEAN
XhciUcxFindBotInterface(
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ ULONG Length,
    _Out_ PXHCI_UCX_BOT_INTERFACE Out)
{
    ULONG Offset = 0;
    BOOLEAN InBotInterface = FALSE;

    RtlZeroMemory(Out, sizeof(*Out));
    if (Length < sizeof(USB_CONFIGURATION_DESCRIPTOR))
        return FALSE;
    Out->ConfigurationValue = ((const USB_CONFIGURATION_DESCRIPTOR *)Buffer)->bConfigurationValue;

    while (Offset + 2 <= Length)
    {
        UCHAR bLength = Buffer[Offset];
        UCHAR bType = Buffer[Offset + 1];

        if (bLength < 2 || Offset + bLength > Length)
            break;                                     /* truncated or malformed: stop, never read past Length */

        if (bType == USB_INTERFACE_DESCRIPTOR_TYPE && bLength >= sizeof(USB_INTERFACE_DESCRIPTOR))
        {
            const USB_INTERFACE_DESCRIPTOR *Iface = (const USB_INTERFACE_DESCRIPTOR *)&Buffer[Offset];

            if (InBotInterface && Out->BulkInAddress != 0 && Out->BulkOutAddress != 0)
                return TRUE;                           /* complete interface found before the next one */
            if (Out->InterfaceCount == 0)
            {
                Out->FirstClass[0] = Iface->bInterfaceClass;
                Out->FirstClass[1] = Iface->bInterfaceSubClass;
                Out->FirstClass[2] = Iface->bInterfaceProtocol;
            }
            if (Out->InterfaceCount < 0xFF)
                Out->InterfaceCount++;

            InBotInterface = (Iface->bInterfaceClass == USB_DEVICE_CLASS_STORAGE &&
                              Iface->bInterfaceSubClass == XHCI_UCX_MSC_SUBCLASS_SCSI &&
                              Iface->bInterfaceProtocol == XHCI_UCX_MSC_PROTOCOL_BOT &&
                              Iface->bAlternateSetting == 0);
            Out->BulkInAddress = 0;
            Out->BulkOutAddress = 0;
            if (InBotInterface)
                Out->InterfaceNumber = Iface->bInterfaceNumber;
        }
        else if (bType == USB_ENDPOINT_DESCRIPTOR_TYPE && bLength >= sizeof(USB_ENDPOINT_DESCRIPTOR) && InBotInterface)
        {
            const USB_ENDPOINT_DESCRIPTOR *Ep = (const USB_ENDPOINT_DESCRIPTOR *)&Buffer[Offset];

            if ((Ep->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_BULK &&
                (Ep->bEndpointAddress & 0x0F) != 0)
            {
                if (USB_ENDPOINT_DIRECTION_IN(Ep->bEndpointAddress) && Out->BulkInAddress == 0)
                {
                    Out->BulkInAddress = Ep->bEndpointAddress;
                    Out->BulkInMaxPacket = Ep->wMaxPacketSize;
                }
                else if (USB_ENDPOINT_DIRECTION_OUT(Ep->bEndpointAddress) && Out->BulkOutAddress == 0)
                {
                    Out->BulkOutAddress = Ep->bEndpointAddress;
                    Out->BulkOutMaxPacket = Ep->wMaxPacketSize;
                }
            }
        }

        Offset += bLength;
    }

    return InBotInterface && Out->BulkInAddress != 0 && Out->BulkOutAddress != 0;
}

/* Configure Endpoint: add the two bulk endpoints to the slot's device context */
static
NTSTATUS
XhciUcxConfigureBulkEndpoints(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _Inout_ PXHCI_UCX_SLOT Slot)
{
    PXHCI_INPUT_CONTROL_CONTEXT Icc = XhciUcxInputControlContext(Dev, Slot);
    PXHCI_SLOT_CONTEXT InSlot = XhciUcxInputSlotContext(Dev, Slot);
    PXHCI_SLOT_CONTEXT OutSlot = XhciUcxOutputSlotContext(Dev, Slot);
    PXHCI_ENDPOINT_CONTEXT EpIn = XhciUcxInputEndpointContext(Dev, Slot, Slot->BulkInDci);
    PXHCI_ENDPOINT_CONTEXT EpOut = XhciUcxInputEndpointContext(Dev, Slot, Slot->BulkOutDci);
    ULONG LastCtx = (Slot->BulkInDci > Slot->BulkOutDci) ? Slot->BulkInDci : Slot->BulkOutDci;
    ULONGLONG InRingPa;
    ULONGLONG OutRingPa;
    ULONG Code = 0;
    NTSTATUS Status;

    XhciUcxInitTransferRing(Dev, Slot->BulkInRingOffset, XHCI_UCX_BULK_RING_TRBS, &Slot->BulkInEnqueue, &Slot->BulkInCycle);
    XhciUcxInitTransferRing(Dev, Slot->BulkOutRingOffset, XHCI_UCX_BULK_RING_TRBS, &Slot->BulkOutEnqueue, &Slot->BulkOutCycle);
    InRingPa = (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->BulkInRingOffset).QuadPart;
    OutRingPa = (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->BulkOutRingOffset).QuadPart;

    RtlZeroMemory(Dev->HcVa + Slot->InputContextOffset, PAGE_SIZE);
    Icc->AddContextFlags = XHCI_UCX_INPUT_CTX_ADD_SLOT |
                           XHCI_UCX_INPUT_CTX_ADD(Slot->BulkInDci) | XHCI_UCX_INPUT_CTX_ADD(Slot->BulkOutDci);
    Icc->DropContextFlags = 0;

    /* Input slot context mirrors the controller's output copy, with the new Context Entries */
    RtlCopyMemory(InSlot, OutSlot, Dev->ContextSize);
    XhciSlotContextSetLastCtx(InSlot, LastCtx);

    XhciEndpointContextInit(EpIn, XHCI_ENDPOINT_TYPE_BULK_IN, Slot->BulkInMaxPacket,
                            0, 0, 0, 0, Slot->BulkInMaxPacket, InRingPa | XHCI_TRB_CYCLE);
    XhciEndpointContextInit(EpOut, XHCI_ENDPOINT_TYPE_BULK_OUT, Slot->BulkOutMaxPacket,
                            0, 0, 0, 0, Slot->BulkOutMaxPacket, OutRingPa | XHCI_TRB_CYCLE);
    KeMemoryBarrier();

    Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_CONFIG_EP,
                                (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->InputContextOffset).QuadPart,
                                XHCI_COMMAND_SLOT_FIELD(Slot->SlotId), &Code, NULL);
    XHCI_UCX_LOG("STEP4 slot %u: Configure Endpoint rc=0x%08lX code=%lu (bulk IN 0x%02x DCI %u MPS %lu, bulk OUT 0x%02x DCI %u MPS %lu, context entries %lu)",
                 Slot->SlotId, (ULONG)Status, Code,
                 Slot->BulkInAddress, Slot->BulkInDci, Slot->BulkInMaxPacket,
                 Slot->BulkOutAddress, Slot->BulkOutDci, Slot->BulkOutMaxPacket, LastCtx);
    return Status;
}

/* Is BlockSize a power of two we can hold in the data page? */
static
BOOLEAN
XhciUcxBlockSizeLegal(
    _In_ ULONG BlockSize)
{
    return BlockSize >= 512 && BlockSize <= XHCI_UCX_DATA_BUFFER_SIZE && (BlockSize & (BlockSize - 1)) == 0;
}

/* Runs after STEP3 DONE on an addressed, identified slot. Failures leave the slot addressed. */
static
VOID
XhciUcxProbeStorage(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _Inout_ PXHCI_UCX_SLOT Slot)
{
    PUCHAR Buffer = Dev->HcVa + Slot->DescriptorOffset;
    PUCHAR Data = Dev->HcVa + Slot->DataOffset;
    USB_DEFAULT_PIPE_SETUP_PACKET Setup;
    XHCI_UCX_BOT_INTERFACE Bot;
    UCHAR Cdb[16];
    ULONG Transferred = 0;
    ULONG TotalLength;
    ULONG InMps;
    ULONG OutMps;
    ULONG LastLba;
    ULONG BlockSize;
    ULONG Attempt;
    UCHAR CswStatus = 0xFF;
    NTSTATUS Status;

    /* Configuration descriptor header, then the whole set (clamped to the descriptor buffer) */
    RtlZeroMemory(Buffer, XHCI_UCX_DESC_BUFFER_SIZE);
    RtlZeroMemory(&Setup, sizeof(Setup));
    Setup.bmRequestType.B = 0x80;
    Setup.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    Setup.wValue.W = (USHORT)(USB_CONFIGURATION_DESCRIPTOR_TYPE << 8);
    Setup.wIndex.W = 0;
    Setup.wLength = sizeof(USB_CONFIGURATION_DESCRIPTOR);

    Status = XhciUcxEp0Control(Dev, Slot, &Setup, Slot->DescriptorOffset, sizeof(USB_CONFIGURATION_DESCRIPTOR), TRUE, &Transferred);
    if (!NT_SUCCESS(Status) || Transferred < sizeof(USB_CONFIGURATION_DESCRIPTOR) ||
        Buffer[0] != sizeof(USB_CONFIGURATION_DESCRIPTOR) || Buffer[1] != USB_CONFIGURATION_DESCRIPTOR_TYPE)
    {
        XHCI_UCX_LOG("STEP4 slot %u: GET_DESCRIPTOR(configuration, 9) failed rc=0x%08lX got %lu bytes bLength=%u type=%u",
                     Slot->SlotId, (ULONG)Status, Transferred, Buffer[0], Buffer[1]);
        return;
    }
    TotalLength = ((const USB_CONFIGURATION_DESCRIPTOR *)Buffer)->wTotalLength;
    if (TotalLength < sizeof(USB_CONFIGURATION_DESCRIPTOR))
    {
        XHCI_UCX_LOG("STEP4 slot %u: SEC-1 reject, wTotalLength=%lu", Slot->SlotId, TotalLength);
        return;
    }
    if (TotalLength > XHCI_UCX_DESC_BUFFER_SIZE)
        TotalLength = XHCI_UCX_DESC_BUFFER_SIZE;

    RtlZeroMemory(Buffer, XHCI_UCX_DESC_BUFFER_SIZE);
    Setup.wLength = (USHORT)TotalLength;
    Status = XhciUcxEp0Control(Dev, Slot, &Setup, Slot->DescriptorOffset, TotalLength, TRUE, &Transferred);
    if (!NT_SUCCESS(Status) || Transferred < sizeof(USB_CONFIGURATION_DESCRIPTOR) ||
        Buffer[0] != sizeof(USB_CONFIGURATION_DESCRIPTOR) || Buffer[1] != USB_CONFIGURATION_DESCRIPTOR_TYPE)
    {
        XHCI_UCX_LOG("STEP4 slot %u: GET_DESCRIPTOR(configuration, %lu) failed rc=0x%08lX got %lu bytes",
                     Slot->SlotId, TotalLength, (ULONG)Status, Transferred);
        return;
    }

    /* Only the bytes that arrived are parsed */
    if (!XhciUcxFindBotInterface(Buffer, Transferred, &Bot))
    {
        XHCI_UCX_LOG("STEP4 skipped: slot %u has no Mass Storage/SCSI/BOT interface with bulk IN+OUT (%u interface(s), first class %02x/%02x/%02x, %lu descriptor bytes)",
                     Slot->SlotId, Bot.InterfaceCount, Bot.FirstClass[0], Bot.FirstClass[1], Bot.FirstClass[2], Transferred);
        return;
    }

    if (Bot.ConfigurationValue == 0)
    {
        XHCI_UCX_LOG("STEP4 slot %u: SEC-1 reject, bConfigurationValue=0", Slot->SlotId);
        return;
    }

    InMps = XhciUcxBulkMaxPacketLegal(Slot->SpeedClass, Bot.BulkInMaxPacket);
    OutMps = XhciUcxBulkMaxPacketLegal(Slot->SpeedClass, Bot.BulkOutMaxPacket);
    if (InMps == 0 || OutMps == 0)
    {
        XHCI_UCX_LOG("STEP4 slot %u: SEC-1 reject, bulk wMaxPacketSize IN=%u OUT=%u illegal for speed id %u (class %u)",
                     Slot->SlotId, Bot.BulkInMaxPacket, Bot.BulkOutMaxPacket, Slot->Speed, Slot->SpeedClass);
        return;
    }

    Slot->ConfigurationValue = Bot.ConfigurationValue;
    Slot->InterfaceNumber = Bot.InterfaceNumber;
    Slot->BulkInAddress = Bot.BulkInAddress;
    Slot->BulkOutAddress = Bot.BulkOutAddress;
    Slot->BulkInMaxPacket = InMps;
    Slot->BulkOutMaxPacket = OutMps;
    Slot->BulkInDci = (UCHAR)(2 * (Bot.BulkInAddress & 0x0F) + 1);
    Slot->BulkOutDci = (UCHAR)(2 * (Bot.BulkOutAddress & 0x0F));
    XHCI_UCX_LOG("STEP4 slot %u: BOT interface %u in configuration %u (bulk IN 0x%02x, bulk OUT 0x%02x)",
                 Slot->SlotId, Slot->InterfaceNumber, Slot->ConfigurationValue, Slot->BulkInAddress, Slot->BulkOutAddress);

    Status = XhciUcxConfigureBulkEndpoints(Dev, Slot);
    if (!NT_SUCCESS(Status))
        return;

    /* SET_CONFIGURATION: the device enables its endpoints */
    RtlZeroMemory(&Setup, sizeof(Setup));
    Setup.bmRequestType.B = 0x00;                      /* host-to-device, standard, device */
    Setup.bRequest = USB_REQUEST_SET_CONFIGURATION;
    Setup.wValue.W = Slot->ConfigurationValue;
    Setup.wIndex.W = 0;
    Setup.wLength = 0;
    Status = XhciUcxEp0Control(Dev, Slot, &Setup, Slot->DescriptorOffset, 0, FALSE, &Transferred);
    XHCI_UCX_LOG("STEP4 slot %u: SET_CONFIGURATION(%u) rc=0x%08lX", Slot->SlotId, Slot->ConfigurationValue, (ULONG)Status);
    if (!NT_SUCCESS(Status))
        return;
    Slot->Configured = TRUE;

    /* UCX endpoint objects for the two bulk pipes (ucx01000 M1 skeleton: lifecycle only) */
    if (Slot->UsbDevice != NULL)
    {
        PUCXENDPOINT_INIT EndpointInit = NULL;

        Status = UcxEndpointCreate(Slot->UsbDevice, &EndpointInit, WDF_NO_OBJECT_ATTRIBUTES, &Slot->BulkIn);
        XHCI_UCX_LOG("UcxEndpointCreate (bulk IN) rc=0x%08lX", (ULONG)Status);
        if (!NT_SUCCESS(Status))
            Slot->BulkIn = NULL;
        EndpointInit = NULL;
        Status = UcxEndpointCreate(Slot->UsbDevice, &EndpointInit, WDF_NO_OBJECT_ATTRIBUTES, &Slot->BulkOut);
        XHCI_UCX_LOG("UcxEndpointCreate (bulk OUT) rc=0x%08lX", (ULONG)Status);
        if (!NT_SUCCESS(Status))
            Slot->BulkOut = NULL;
    }

    /* INQUIRY (36 bytes): peripheral type and removable bit only, no strings */
    RtlZeroMemory(Cdb, sizeof(Cdb));
    Cdb[0] = XHCI_UCX_SCSI_INQUIRY;
    Cdb[4] = XHCI_UCX_INQUIRY_LENGTH;
    Status = XhciUcxBotCommand(Dev, Slot, Cdb, 6, TRUE, XHCI_UCX_INQUIRY_LENGTH, &Transferred, &CswStatus);
    if (!NT_SUCCESS(Status) || CswStatus != XHCI_UCX_CSW_STATUS_GOOD || Transferred < 5)
    {
        XHCI_UCX_LOG("STEP4 slot %u: INQUIRY failed rc=0x%08lX csw=%u bytes=%lu", Slot->SlotId, (ULONG)Status, CswStatus, Transferred);
        return;
    }
    XHCI_UCX_LOG("STEP4 slot %u INQUIRY: peripheral type %u qualifier %u removable=%u version %u (%lu bytes)",
                 Slot->SlotId, Data[0] & 0x1F, (Data[0] >> 5) & 0x7, (Data[1] & 0x80) ? 1 : 0, Data[2], Transferred);

    /* TEST UNIT READY; a CHECK CONDITION (e.g. unit attention after power-on) is cleared with REQUEST SENSE */
    for (Attempt = 0; Attempt < XHCI_UCX_TUR_RETRIES; Attempt++)
    {
        RtlZeroMemory(Cdb, sizeof(Cdb));
        Cdb[0] = XHCI_UCX_SCSI_TEST_UNIT_READY;
        Status = XhciUcxBotCommand(Dev, Slot, Cdb, 6, FALSE, 0, &Transferred, &CswStatus);
        if (!NT_SUCCESS(Status))
        {
            XHCI_UCX_LOG("STEP4 slot %u: TEST UNIT READY failed rc=0x%08lX", Slot->SlotId, (ULONG)Status);
            return;
        }
        if (CswStatus == XHCI_UCX_CSW_STATUS_GOOD)
            break;

        RtlZeroMemory(Cdb, sizeof(Cdb));
        Cdb[0] = XHCI_UCX_SCSI_REQUEST_SENSE;
        Cdb[4] = XHCI_UCX_SENSE_LENGTH;
        Status = XhciUcxBotCommand(Dev, Slot, Cdb, 6, TRUE, XHCI_UCX_SENSE_LENGTH, &Transferred, &CswStatus);
        XHCI_UCX_LOG("STEP4 slot %u: unit not ready, REQUEST SENSE rc=0x%08lX csw=%u key=%u asc=%02x ascq=%02x (%lu bytes)",
                     Slot->SlotId, (ULONG)Status, CswStatus,
                     (Transferred > 2) ? (Data[2] & 0x0F) : 0,
                     (Transferred > 12) ? Data[12] : 0,
                     (Transferred > 13) ? Data[13] : 0, Transferred);
        if (!NT_SUCCESS(Status))
            return;
        CswStatus = XHCI_UCX_CSW_STATUS_FAILED;
    }
    if (CswStatus != XHCI_UCX_CSW_STATUS_GOOD)
    {
        XHCI_UCX_LOG("STEP4 slot %u: unit still not ready after %u attempts", Slot->SlotId, XHCI_UCX_TUR_RETRIES);
        return;
    }

    /* READ CAPACITY(10): big-endian last LBA and block size, validated before use */
    RtlZeroMemory(Cdb, sizeof(Cdb));
    Cdb[0] = XHCI_UCX_SCSI_READ_CAPACITY10;
    Status = XhciUcxBotCommand(Dev, Slot, Cdb, 10, TRUE, 8, &Transferred, &CswStatus);
    if (!NT_SUCCESS(Status) || CswStatus != XHCI_UCX_CSW_STATUS_GOOD || Transferred != 8)
    {
        XHCI_UCX_LOG("STEP4 slot %u: READ CAPACITY(10) failed rc=0x%08lX csw=%u bytes=%lu", Slot->SlotId, (ULONG)Status, CswStatus, Transferred);
        return;
    }
    LastLba = XhciUcxReadBe32(&Data[0]);
    BlockSize = XhciUcxReadBe32(&Data[4]);
    if (!XhciUcxBlockSizeLegal(BlockSize))
    {
        XHCI_UCX_LOG("STEP4 slot %u: SEC-1 reject, block size %lu (last LBA %lu)", Slot->SlotId, BlockSize, LastLba);
        return;
    }
    XHCI_UCX_LOG("STEP4 slot %u capacity: last LBA %lu, block size %lu", Slot->SlotId, LastLba, BlockSize);

    /* READ(10) logical block 0, one block */
    RtlZeroMemory(Cdb, sizeof(Cdb));
    Cdb[0] = XHCI_UCX_SCSI_READ10;
    Cdb[8] = 1;                                        /* transfer length (blocks), big-endian in [7..8] */
    Status = XhciUcxBotCommand(Dev, Slot, Cdb, 10, TRUE, BlockSize, &Transferred, &CswStatus);
    if (!NT_SUCCESS(Status) || CswStatus != XHCI_UCX_CSW_STATUS_GOOD || Transferred != BlockSize)
    {
        XHCI_UCX_LOG("STEP4 slot %u: READ(10) LBA 0 failed rc=0x%08lX csw=%u bytes=%lu of %lu",
                     Slot->SlotId, (ULONG)Status, CswStatus, Transferred, BlockSize);
        return;
    }

    XHCI_UCX_LOG("STEP4 DONE: slot %u read LBA 0 (%lu bytes, block size %lu, last LBA %lu, boot signature %s)",
                 Slot->SlotId, Transferred, BlockSize, LastLba,
                 (Data[510] == 0x55 && Data[511] == 0xAA) ? "55AA" : "absent");
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
    /* Packet-size rules follow the port's protocol: any USB3 port (SuperSpeed,
     * SuperSpeedPlus, vendor PSI ids above 4) uses the SuperSpeed rules */
    Slot->SpeedClass = (PortId <= XHCI_MAX_PORTS && Dev->PortProtocol[PortId] == 3) ? XHCI_PORTSC_SPEED_SUPER : (UCHAR)Speed;
    Slot->Ep0MaxPacket = (Slot->SpeedClass == XHCI_PORTSC_SPEED_SUPER) ? 512 :
                         (Slot->SpeedClass == XHCI_PORTSC_SPEED_HIGH) ? 64 : 8;
    XHCI_UCX_LOG("STEP3 port %lu enabled: speed id %lu (class %u), initial EP0 MPS %lu",
                 PortId, Speed, Slot->SpeedClass, Slot->Ep0MaxPacket);

    /* Enable Slot */
    Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_ENABLE_SLOT, 0, 0, &Code, &SlotId);
    if (!NT_SUCCESS(Status) || SlotId == 0 || SlotId > Dev->MaxSlots || XhciUcxFindSlotById(Dev, SlotId) != NULL)
    {
        XHCI_UCX_LOG("STEP3 port %lu: Enable Slot failed rc=0x%08lX code=%lu slot=%u", PortId, (ULONG)Status, Code, SlotId);
        if (NT_SUCCESS(Status) && SlotId != 0 && SlotId <= Dev->MaxSlots && XhciUcxFindSlotById(Dev, SlotId) != NULL)
        {
            /* The controller handed out an id we already own: give the new one back, keep the owner */
            ULONG DisableCode = 0;
            NTSTATUS DisableStatus = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_DISABLE_SLOT, 0,
                                                        XHCI_COMMAND_SLOT_FIELD(SlotId), &DisableCode, NULL);

            XHCI_UCX_LOG("STEP3 port %lu: duplicate slot id %u returned, Disable Slot rc=0x%08lX code=%lu",
                         PortId, SlotId, (ULONG)DisableStatus, DisableCode);
        }
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

    Status = XhciUcxEp0Control(Dev, Slot, &Setup, Slot->DescriptorOffset, 8, TRUE, &Transferred);
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
    DescriptorMps = XhciUcxEp0MaxPacketFromDescriptor(Slot->SpeedClass, DevDesc->bMaxPacketSize0);
    if (DescriptorMps == 0)
    {
        XHCI_UCX_LOG("STEP3 slot %u: SEC-1 reject, bMaxPacketSize0=%u illegal for speed id %lu (class %u)",
                     SlotId, DevDesc->bMaxPacketSize0, Speed, Slot->SpeedClass);
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
    Status = XhciUcxEp0Control(Dev, Slot, &Setup, Slot->DescriptorOffset, sizeof(USB_DEVICE_DESCRIPTOR), TRUE, &Transferred);
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

    /* Step 4: bulk path for a BOT mass-storage device; a failure leaves the device addressed */
    XhciUcxProbeStorage(Dev, Slot);
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

        if (InterlockedCompareExchange(&Dev->EnumStopped, 0, 0) != 0 ||
            !Dev->ControllerRunning || Dev->FatalError || Dev->OperationalRegisters == NULL || Dev->HcVa == NULL)
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

/*
 * Queue the enumeration work item unless enumeration has been stopped. The
 * check and the enqueue happen under EventRingLock, the same lock under which
 * D0Exit/SurpriseRemoval set EnumStopped before flushing the work item: an
 * enqueue either precedes the flag (and the flush waits for it) or is refused.
 */
static
VOID
XhciUcxRequestPortRescan(
    _Inout_ PXHCI_UCX_DEVICE Dev)
{
    KIRQL OldIrql;

    InterlockedExchange(&Dev->PortRescanNeeded, 1);

    KeAcquireSpinLock(&Dev->EventRingLock, &OldIrql);
    if (Dev->EnumWorkItem != NULL && InterlockedCompareExchange(&Dev->EnumStopped, 0, 0) == 0)
        WdfWorkItemEnqueue(Dev->EnumWorkItem);
    KeReleaseSpinLock(&Dev->EventRingLock, OldIrql);
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

/* --------------------------------------------------------------------------
 * Root hub as the hub driver sees it (M3): hub class requests on PORTSC.
 * ucx01000 owns the PDO, the standard requests and the status-change pipe; it
 * hands us the hub descriptor, hub/port status and port feature requests with
 * the URB header, device handle and buffer already checked.
 * -------------------------------------------------------------------------- */

/*
 * One PORTSC read-modify-write that never acknowledges a change bit by
 * accident: all RW1C/RW1S bits are masked out first, then only what the caller
 * names is written. PED is RW1C-to-disable, so it is reachable through SetBits.
 */
static
VOID
XhciUcxPortScModify(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG PortId,
    _In_ ULONG SetBits,
    _In_ ULONG ClearBits,
    _In_ BOOLEAN SetLinkState,
    _In_ ULONG LinkState)
{
    volatile ULONG *PortSc = &Dev->OperationalRegisters->PortRegister[PortId - 1].PortStatusAndControl;
    ULONG Value = XHCI_UCX_READ_REG(PortSc);

    Value &= ~(XHCI_PORTSC_CHANGE_MASK | XHCI_PORTSC_PED | XHCI_PORTSC_PR | XHCI_PORTSC_WPR | XHCI_PORTSC_LWS);
    Value &= ~ClearBits;
    if (SetLinkState)
        Value = (Value & ~XHCI_PORTSC_PLS_MASK) | XHCI_PORTSC_PLS(LinkState) | XHCI_PORTSC_LWS;
    XHCI_UCX_WRITE_REG(PortSc, Value | SetBits);
}

/* USB hub port status and change words from a PORTSC value (USB 2.0 11.24.2.7, USB 3.2 10.16.2.6) */
static
VOID
XhciUcxPortStatusFromPortSc(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG PortId,
    _In_ ULONG PortSc,
    _Out_ PUSB_PORT_STATUS_AND_CHANGE Out)
{
    BOOLEAN Usb3 = (PortId <= XHCI_MAX_PORTS && Dev->PortProtocol[PortId] == 3);
    ULONG LinkState = (PortSc & XHCI_PORTSC_PLS_MASK) >> XHCI_PORTSC_PLS_SHIFT;
    ULONG Speed = (PortSc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
    BOOLEAN Powered = (!Dev->PortPowerControl || (PortSc & XHCI_PORTSC_PP) != 0);

    RtlZeroMemory(Out, sizeof(*Out));

    /* Bits 0-4 have the same meaning in the USB 2.0 and USB 3.x port status words */
    if (PortSc & XHCI_PORTSC_CCS)
        Out->PortStatus.AsUshort16 |= USB_PORT_STATUS_CONNECT;
    if (PortSc & XHCI_PORTSC_PED)
        Out->PortStatus.AsUshort16 |= USB_PORT_STATUS_ENABLE;
    if (PortSc & XHCI_PORTSC_OCA)
        Out->PortStatus.AsUshort16 |= USB_PORT_STATUS_OVER_CURRENT;
    if (PortSc & XHCI_PORTSC_PR)
        Out->PortStatus.AsUshort16 |= USB_PORT_STATUS_RESET;

    if (Usb3)
    {
        /* USB 3.x layout: link state bits 5-8, power bit 9, negotiated speed
         * bits 10-12 (0 = SuperSpeed). USB_PORT_STATUS_POWER (0x0100) is the
         * USB 2.0 position and would land inside the link state here. */
        Out->PortStatus.Usb30PortStatus.PortLinkState = (USHORT)LinkState;
        Out->PortStatus.Usb30PortStatus.PortPower = Powered ? 1 : 0;
        Out->PortStatus.Usb30PortStatus.NegotiatedDeviceSpeed = 0;
    }
    else
    {
        if (Powered)
            Out->PortStatus.AsUshort16 |= USB_PORT_STATUS_POWER;
        if (LinkState == PORT_LINK_STATE_U3)
            Out->PortStatus.AsUshort16 |= USB_PORT_STATUS_SUSPEND;
        if (Speed == XHCI_PORTSC_SPEED_LOW)
            Out->PortStatus.AsUshort16 |= USB_PORT_STATUS_LOW_SPEED;
        else if (Speed == XHCI_PORTSC_SPEED_HIGH)
            Out->PortStatus.AsUshort16 |= USB_PORT_STATUS_HIGH_SPEED;
    }

    if (PortSc & XHCI_PORTSC_CSC)
        Out->PortChange.Usb20PortChange.ConnectStatusChange = 1;
    if ((PortSc & XHCI_PORTSC_PEC) && !Usb3)
        Out->PortChange.Usb20PortChange.PortEnableDisableChange = 1;
    if (PortSc & XHCI_PORTSC_OCC)
        Out->PortChange.Usb20PortChange.OverCurrentIndicatorChange = 1;
    if (PortSc & XHCI_PORTSC_PRC)
        Out->PortChange.Usb20PortChange.ResetChange = 1;
    if (PortSc & XHCI_PORTSC_PLC)
    {
        if (Usb3)
            Out->PortChange.Usb30PortChange.PortLinkStateChange = 1;
        else
            Out->PortChange.Usb20PortChange.SuspendChange = 1;
    }
    if (Usb3)
    {
        if (PortSc & XHCI_PORTSC_WRC)
            Out->PortChange.Usb30PortChange.BHResetChange = 1;
        if (PortSc & XHCI_PORTSC_CEC)
            Out->PortChange.Usb30PortChange.PortConfigErrorChange = 1;
    }
}

/* Builds the hub descriptor the hub driver asked for into Buffer; returns the bytes written */
static
ULONG
XhciUcxBuildHubDescriptor(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ UCHAR DescriptorType,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length)
{
    USHORT Characteristics = XHCI_UCX_HUB_CHAR_OVERCURRENT_PER_PORT |
                             (Dev->PortPowerControl ? XHCI_UCX_HUB_CHAR_POWER_INDIVIDUAL
                                                    : XHCI_UCX_HUB_CHAR_NO_POWER_SWITCHING);
    ULONG Ports = Dev->NumberOfPorts;
    ULONG Copy;

    if (DescriptorType == USB_30_HUB_DESCRIPTOR_TYPE)
    {
        /* 12 bytes: the two DeviceRemovable bytes say every port is removable */
        UCHAR Descriptor[12];

        RtlZeroMemory(Descriptor, sizeof(Descriptor));
        Descriptor[0] = sizeof(Descriptor);
        Descriptor[1] = USB_30_HUB_DESCRIPTOR_TYPE;
        Descriptor[2] = (UCHAR)Ports;
        Descriptor[3] = (UCHAR)(Characteristics & 0xFF);
        Descriptor[4] = (UCHAR)(Characteristics >> 8);
        Descriptor[5] = 2;      /* bPowerOnToPowerGood: 4 ms */
        Descriptor[6] = 0;      /* bHubControlCurrent */
        Descriptor[7] = 0;      /* bHubHdrDecLat */
        Descriptor[8] = 0;      /* wHubDelay */
        Descriptor[9] = 0;
        Copy = min(Length, (ULONG)sizeof(Descriptor));
        RtlCopyMemory(Buffer, Descriptor, Copy);
        return Copy;
    }
    else
    {
        /* USB 2.0 hub descriptor: 7 bytes + DeviceRemovable + PortPwrCtrlMask */
        UCHAR Descriptor[7 + 2 * ((XHCI_MAX_PORTS + 1 + 7) / 8)];
        ULONG MaskBytes = (Ports + 1 + 7) / 8;
        ULONG Total = 7 + 2 * MaskBytes;
        ULONG i;

        RtlZeroMemory(Descriptor, sizeof(Descriptor));
        Descriptor[0] = (UCHAR)Total;
        Descriptor[1] = USB_20_HUB_DESCRIPTOR_TYPE;
        Descriptor[2] = (UCHAR)Ports;
        Descriptor[3] = (UCHAR)(Characteristics & 0xFF);
        Descriptor[4] = (UCHAR)(Characteristics >> 8);
        Descriptor[5] = 2;
        Descriptor[6] = 0;
        for (i = 0; i < MaskBytes; i++)
            Descriptor[7 + MaskBytes + i] = 0xFF;   /* PortPwrCtrlMask, all ones as required */
        Copy = min(Length, Total);
        RtlCopyMemory(Buffer, Descriptor, Copy);
        return Copy;
    }
}

/* SET_FEATURE / CLEAR_FEATURE on a port; FALSE = STALL (unknown or unsupported selector) */
static
BOOLEAN
XhciUcxPortFeature(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_ ULONG PortId,
    _In_ USHORT Feature,
    _In_ UCHAR Selector,
    _In_ BOOLEAN Set)
{
    if (Set)
    {
        switch (Feature)
        {
            case XHCI_UCX_FEATURE_PORT_RESET:
                XhciUcxPortScModify(Dev, PortId, XHCI_PORTSC_PR, 0, FALSE, 0);
                return TRUE;
            case XHCI_UCX_FEATURE_BH_PORT_RESET:
                XhciUcxPortScModify(Dev, PortId, XHCI_PORTSC_WPR, 0, FALSE, 0);
                return TRUE;
            case XHCI_UCX_FEATURE_PORT_POWER:
                if (Dev->PortPowerControl)
                    XhciUcxPortScModify(Dev, PortId, XHCI_PORTSC_PP, 0, FALSE, 0);
                return TRUE;
            case XHCI_UCX_FEATURE_PORT_SUSPEND:
                XhciUcxPortScModify(Dev, PortId, 0, 0, TRUE, PORT_LINK_STATE_U3);
                return TRUE;
            case XHCI_UCX_FEATURE_PORT_LINK_STATE:
                if (Selector > PORT_LINK_STATE_U3 && Selector != PORT_LINK_STATE_DISABLED &&
                    Selector != PORT_LINK_STATE_RX_DETECT && Selector != PORT_LINK_STATE_COMPLIANCE_MODE)
                    return FALSE;
                XhciUcxPortScModify(Dev, PortId, 0, 0, TRUE, Selector);
                return TRUE;
            case XHCI_UCX_FEATURE_PORT_ENABLE:          /* xHCI ports enable themselves */
            case XHCI_UCX_FEATURE_PORT_U1_TIMEOUT:
            case XHCI_UCX_FEATURE_PORT_U2_TIMEOUT:
            case XHCI_UCX_FEATURE_PORT_REMOTE_WAKE_MASK:
            case XHCI_UCX_FEATURE_FORCE_LINKPM_ACCEPT:
                return TRUE;                            /* accepted, no effect yet (M4) */
            default:
                return FALSE;
        }
    }

    switch (Feature)
    {
        case XHCI_UCX_FEATURE_PORT_ENABLE:
            XhciUcxPortScModify(Dev, PortId, XHCI_PORTSC_PED, 0, FALSE, 0);
            return TRUE;
        case XHCI_UCX_FEATURE_PORT_SUSPEND:
            XhciUcxPortScModify(Dev, PortId, 0, 0, TRUE, PORT_LINK_STATE_U0);
            return TRUE;
        case XHCI_UCX_FEATURE_PORT_POWER:
            if (Dev->PortPowerControl)
                XhciUcxPortScModify(Dev, PortId, 0, XHCI_PORTSC_PP, FALSE, 0);
            return TRUE;
        case XHCI_UCX_FEATURE_C_PORT_CONNECTION:
            XhciUcxPortScModify(Dev, PortId, XHCI_PORTSC_CSC, 0, FALSE, 0);
            return TRUE;
        case XHCI_UCX_FEATURE_C_PORT_ENABLE:
            XhciUcxPortScModify(Dev, PortId, XHCI_PORTSC_PEC, 0, FALSE, 0);
            return TRUE;
        case XHCI_UCX_FEATURE_C_PORT_SUSPEND:
        case XHCI_UCX_FEATURE_C_PORT_LINK_STATE:
            XhciUcxPortScModify(Dev, PortId, XHCI_PORTSC_PLC, 0, FALSE, 0);
            return TRUE;
        case XHCI_UCX_FEATURE_C_PORT_OVER_CURRENT:
            XhciUcxPortScModify(Dev, PortId, XHCI_PORTSC_OCC, 0, FALSE, 0);
            return TRUE;
        case XHCI_UCX_FEATURE_C_PORT_RESET:
            XhciUcxPortScModify(Dev, PortId, XHCI_PORTSC_PRC, 0, FALSE, 0);
            return TRUE;
        case XHCI_UCX_FEATURE_C_BH_PORT_RESET:
            XhciUcxPortScModify(Dev, PortId, XHCI_PORTSC_WRC, 0, FALSE, 0);
            return TRUE;
        case XHCI_UCX_FEATURE_C_PORT_CONFIG_ERROR:
            XhciUcxPortScModify(Dev, PortId, XHCI_PORTSC_CEC, 0, FALSE, 0);
            return TRUE;
        default:
            return FALSE;
    }
}

static
VOID
NTAPI
XhciUcxEvtRootHubControlUrb(
    _In_ UCXROOTHUB RootHub,
    _In_ WDFREQUEST Request)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetRootHubClient((WDFOBJECT)RootHub)->Dev;
    PURB Urb = (PURB)UcxRequestGetUrb(Request);
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *Class = &Urb->UrbControlVendorClassRequest;
    USBD_STATUS UsbdStatus = USBD_STATUS_STALL_PID;
    PUCHAR Buffer;
    ULONG Length;
    UCHAR Request_, DescriptorType;
    USHORT Value, Index;
    BOOLEAN Done = FALSE;

    if (Dev == NULL || !Dev->HardwarePrepared || Dev->OperationalRegisters == NULL)
    {
        Urb->UrbHeader.Status = USBD_STATUS_DEVICE_GONE;
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_CONNECTED);
        return;
    }

    /* A hub descriptor can arrive as a standard GET_DESCRIPTOR or as a class request */
    if (Urb->UrbHeader.Function == URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE)
    {
        struct _URB_CONTROL_DESCRIPTOR_REQUEST *Desc = &Urb->UrbControlDescriptorRequest;

        Length = Desc->TransferBufferLength;
        Buffer = (PUCHAR)UcxUrbTransferBuffer(Desc->TransferBuffer, Desc->TransferBufferMDL, Length);
        if (Buffer == NULL && Length != 0)
        {
            UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
        }
        else
        {
            Desc->TransferBufferLength = XhciUcxBuildHubDescriptor(Dev, Desc->DescriptorType, Buffer, Length);
            UsbdStatus = USBD_STATUS_SUCCESS;
        }
        Urb->UrbHeader.Status = UsbdStatus;
        WdfRequestComplete(Request, USBD_SUCCESS(UsbdStatus) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER);
        return;
    }

    Request_ = Class->Request;
    Value = Class->Value;
    Index = Class->Index;
    Length = Class->TransferBufferLength;
    Buffer = (PUCHAR)UcxUrbTransferBuffer(Class->TransferBuffer, Class->TransferBufferMDL, Length);
    if (Buffer == NULL && Length != 0)
    {
        Urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    if (Urb->UrbHeader.Function == URB_FUNCTION_CLASS_DEVICE)
    {
        switch (Request_)
        {
            case USB_REQUEST_GET_DESCRIPTOR:
                DescriptorType = (UCHAR)(Value >> 8);
                if (DescriptorType != USB_20_HUB_DESCRIPTOR_TYPE && DescriptorType != USB_30_HUB_DESCRIPTOR_TYPE)
                    break;
                Class->TransferBufferLength = XhciUcxBuildHubDescriptor(Dev, DescriptorType, Buffer, Length);
                UsbdStatus = USBD_STATUS_SUCCESS;
                Done = TRUE;
                break;

            case USB_REQUEST_GET_STATUS:
                /* wHubStatus | wHubChange: local power good, no over-current, no change */
                if (Buffer != NULL)
                    RtlZeroMemory(Buffer, min(Length, 4UL));
                Class->TransferBufferLength = min(Length, 4UL);
                UsbdStatus = USBD_STATUS_SUCCESS;
                Done = TRUE;
                break;

            case USB_REQUEST_CLEAR_FEATURE:
                if (Value == XHCI_UCX_FEATURE_C_HUB_LOCAL_POWER || Value == XHCI_UCX_FEATURE_C_HUB_OVER_CURRENT)
                {
                    UsbdStatus = USBD_STATUS_SUCCESS;
                    Done = TRUE;
                }
                break;

            default:
                break;
        }
    }
    else if (Urb->UrbHeader.Function == URB_FUNCTION_CLASS_OTHER)
    {
        ULONG PortId = Index & 0xFF;
        UCHAR Selector = (UCHAR)(Index >> 8);

        if (PortId == 0 || PortId > Dev->NumberOfPorts || PortId > XHCI_MAX_PORTS)
        {
            XHCI_UCX_LOG("root hub: port %lu out of range for class request %u", PortId, Request_);
        }
        else
        {
            switch (Request_)
            {
                case USB_REQUEST_GET_STATUS:
                {
                    USB_PORT_STATUS_AND_CHANGE PortStatus;
                    ULONG PortSc = XHCI_UCX_READ_REG(
                        &Dev->OperationalRegisters->PortRegister[PortId - 1].PortStatusAndControl);

                    XhciUcxPortStatusFromPortSc(Dev, PortId, PortSc, &PortStatus);
                    if (Buffer != NULL)
                        RtlCopyMemory(Buffer, &PortStatus, min(Length, (ULONG)sizeof(PortStatus)));
                    Class->TransferBufferLength = min(Length, (ULONG)sizeof(PortStatus));
                    XHCI_UCX_TRACE("root hub: port %lu status %04X change %04X (PORTSC %08lx)",
                                   PortId, PortStatus.PortStatus.AsUshort16,
                                   PortStatus.PortChange.AsUshort16, PortSc);
                    UsbdStatus = USBD_STATUS_SUCCESS;
                    Done = TRUE;
                    break;
                }

                case USB_REQUEST_SET_FEATURE:
                case USB_REQUEST_CLEAR_FEATURE:
                    if (XhciUcxPortFeature(Dev, PortId, Value, Selector, (Request_ == USB_REQUEST_SET_FEATURE)))
                    {
                        XHCI_UCX_TRACE("root hub: port %lu %s feature %u (selector %u)",
                                       PortId, (Request_ == USB_REQUEST_SET_FEATURE) ? "set" : "clear",
                                       Value, Selector);
                        UsbdStatus = USBD_STATUS_SUCCESS;
                        Done = TRUE;
                    }
                    else
                    {
                        XHCI_UCX_LOG("root hub: port %lu %s feature %u not supported",
                                     PortId, (Request_ == USB_REQUEST_SET_FEATURE) ? "set" : "clear", Value);
                    }
                    break;

                default:
                    break;
            }
        }
    }

    if (!Done)
    {
        /* Anything the hub does not implement is a STALL, as a real hub answers */
        UsbdStatus = USBD_STATUS_STALL_PID;
        Class->TransferBufferLength = 0;
    }

    Urb->UrbHeader.Status = UsbdStatus;
    WdfRequestComplete(Request, USBD_SUCCESS(UsbdStatus) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL);
}

static
VOID
NTAPI
XhciUcxEvtRootHubGetInfo(
    _In_ UCXROOTHUB RootHub,
    _In_ WDFREQUEST Request)
{
    UNREFERENCED_PARAMETER(RootHub);
    /* The port counts travel in UCX_ROOTHUB_CONFIG; nothing else is asked yet */
    WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
}

/*
 * Status-change bitmap for the hub driver's interrupt pipe: bit N for port N
 * whose PORTSC still carries a change bit (the hub clears them one by one
 * through CLEAR_FEATURE). Bit 0 (the hub itself) never changes. At most
 * Length bytes are written. Runs at <= DISPATCH_LEVEL.
 */
static
BOOLEAN
NTAPI
XhciUcxEvtRootHubGetPortChangeBitmap(
    _In_ UCXROOTHUB RootHub,
    _Out_writes_bytes_(Length) PUCHAR Bitmap,
    _In_ ULONG Length)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetRootHubClient((WDFOBJECT)RootHub)->Dev;
    BOOLEAN Any = FALSE;
    ULONG PortId;

    if (Dev == NULL || !Dev->HardwarePrepared || Dev->OperationalRegisters == NULL || Length == 0)
        return FALSE;

    RtlZeroMemory(Bitmap, Length);
    for (PortId = 1; PortId <= Dev->NumberOfPorts && PortId <= XHCI_MAX_PORTS; PortId++)
    {
        ULONG PortSc = XHCI_UCX_READ_REG(&Dev->OperationalRegisters->PortRegister[PortId - 1].PortStatusAndControl);

        if ((PortSc & XHCI_PORTSC_CHANGE_MASK) == 0)
            continue;
        if ((PortId / 8) >= Length)
            break;
        Bitmap[PortId / 8] |= (UCHAR)(1u << (PortId % 8));
        Any = TRUE;
    }

    if (Any)
        XHCI_UCX_LOG("root hub: status change bitmap delivered to the hub driver");
    return Any;
}

/* --------------------------------------------------------------------------
 * Hub-driven devices (M3 step 2): the class library asks for a device on a
 * root port, we take a slot, address it, give it a default endpoint and run
 * its control transfers. All callbacks are synchronous at PASSIVE_LEVEL and
 * serialized through TransferMutex, because the command and EP0 completion
 * bookkeeping (the PendingCommand and PendingTransfer fields) is controller-wide.
 * -------------------------------------------------------------------------- */

static
USB_DEVICE_SPEED
XhciUcxUsbSpeedFromPortSc(
    _In_ ULONG SpeedId)
{
    switch (SpeedId)
    {
        case XHCI_PORTSC_SPEED_LOW:  return UsbLowSpeed;
        case XHCI_PORTSC_SPEED_FULL: return UsbFullSpeed;
        case XHCI_PORTSC_SPEED_HIGH: return UsbHighSpeed;
        default:                     return UsbSuperSpeed;   /* SS and every PSI id above it */
    }
}

static
USBD_STATUS
XhciUcxUsbdStatusFromCompletion(
    _In_ ULONG CompletionCode,
    _In_ NTSTATUS Status)
{
    if (Status == STATUS_IO_TIMEOUT)
        return USBD_STATUS_TIMEOUT;
    switch (CompletionCode)
    {
        case XHCI_COMPLETION_SUCCESS:
        case XHCI_COMPLETION_SHORT_PACKET:        return USBD_STATUS_SUCCESS;
        case XHCI_COMPLETION_STALL_ERROR:         return USBD_STATUS_STALL_PID;
        case XHCI_COMPLETION_BABBLE_ERROR:        return USBD_STATUS_BABBLE_DETECTED;
        case XHCI_COMPLETION_DATA_BUFFER_ERROR:   return USBD_STATUS_DATA_OVERRUN;
        case XHCI_COMPLETION_USB_TRANSACTION_ERROR: return USBD_STATUS_XACT_ERROR;
        default:                                  return USBD_STATUS_DEV_NOT_RESPONDING;
    }
}

static
NTSTATUS
NTAPI
XhciUcxEvtEndpointControlTransfer(
    _In_ UCXENDPOINT Endpoint,
    _Inout_ PUCX_TRANSFER Transfer)
{
    PXHCI_UCX_ENDPOINT_CLIENT Ep = XhciUcxGetEndpointClient((WDFOBJECT)Endpoint);
    PXHCI_UCX_DEVICE Dev = Ep->Dev;
    PXHCI_UCX_USBDEVICE_CLIENT Device = XhciUcxGetUsbDeviceClient((WDFOBJECT)Ep->UsbDevice);
    PXHCI_UCX_SLOT Slot = Device->Slot;
    PVOID Caller;
    PUCHAR Bounce;
    ULONG Transferred = 0;
    NTSTATUS Status;

    Transfer->Transferred = 0;
    Transfer->UsbdStatus = USBD_STATUS_DEV_NOT_RESPONDING;

    if (Transfer->Size < sizeof(UCX_TRANSFER) || Transfer->Type != UcxTransferControl || Ep->Dci != 1)
    {
        Transfer->UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }
    if (Slot == NULL || !Slot->InUse || !Slot->Addressed)
    {
        Transfer->UsbdStatus = USBD_STATUS_DEVICE_GONE;
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (Transfer->Length > PAGE_SIZE)
    {
        Transfer->UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }
    Caller = UcxUrbTransferBuffer(Transfer->Buffer, Transfer->Mdl, Transfer->Length);
    if (Transfer->Length != 0 && Caller == NULL)
    {
        Transfer->UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    /* The data page of the slot is the bounce buffer; the caller's memory is
     * never handed to the controller */
    ExAcquireFastMutex(&Dev->TransferMutex);
    Bounce = Dev->HcVa + Slot->DataOffset;
    if (Transfer->Length != 0)
    {
        if (Transfer->DataIn)
            RtlZeroMemory(Bounce, Transfer->Length);
        else
            RtlCopyMemory(Bounce, Caller, Transfer->Length);
    }

    Status = XhciUcxEp0Control(Dev, Slot, &Transfer->SetupPacket, Slot->DataOffset,
                               Transfer->Length, Transfer->DataIn, &Transferred);
    Transfer->UsbdStatus = XhciUcxUsbdStatusFromCompletion(Dev->PendingTransferCode, Status);

    /* A STALL halts the default endpoint (xHCI 4.10.2.1): Reset Endpoint and
     * point the dequeue at the producer, so the next request is not stuck
     * behind this one. The device recovers EP0 by itself (no CLEAR_FEATURE). */
    if (!NT_SUCCESS(Status) && Dev->PendingTransferCode == XHCI_COMPLETION_STALL_ERROR &&
        Dev->ControllerRunning && !Dev->FatalError)
    {
        ULONG Fields = XHCI_COMMAND_SLOT_FIELD(Slot->SlotId) | XHCI_UCX_COMMAND_EP_FIELD(1);
        ULONGLONG Dequeue = (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->Ep0RingOffset).QuadPart +
                            (ULONGLONG)Slot->Ep0Enqueue * sizeof(XHCI_TRB);
        ULONG Code = 0;
        NTSTATUS ResetStatus;

        ResetStatus = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_RESET_EP, 0, Fields, &Code, NULL);
        if (NT_SUCCESS(ResetStatus) || Code == XHCI_COMPLETION_CONTEXT_STATE_ERROR)
            ResetStatus = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_SET_DEQ, Dequeue | (Slot->Ep0Cycle & XHCI_TRB_CYCLE),
                                             Fields, &Code, NULL);
        XHCI_UCX_LOG("slot %u: EP0 stalled (bRequest %u); endpoint reset -> 0x%08lX code %lu",
                     Slot->SlotId, Transfer->SetupPacket.bRequest, (ULONG)ResetStatus, Code);
    }

    if (NT_SUCCESS(Status) && Transfer->DataIn && Transferred != 0)
    {
        Transferred = min(Transferred, Transfer->Length);
        RtlCopyMemory(Caller, Bounce, Transferred);
    }
    ExReleaseFastMutex(&Dev->TransferMutex);

    Transfer->Transferred = NT_SUCCESS(Status) ? Transferred : 0;
    XHCI_UCX_TRACE("slot %u EP0 %s bRequest %u wLength %u -> 0x%08lX / USBD 0x%08lX, %lu bytes",
                   Slot->SlotId, Transfer->DataIn ? "IN" : "OUT", Transfer->SetupPacket.bRequest,
                   Transfer->SetupPacket.wLength, (ULONG)Status, (ULONG)Transfer->UsbdStatus, Transfer->Transferred);
    return Status;
}

/* Enable Slot, contexts, EP0 ring, DCBAA entry: the device's slot, not yet addressed */
static
NTSTATUS
NTAPI
XhciUcxEvtUsbDeviceEnable(
    _In_ UCXCONTROLLER Controller,
    _In_ UCXUSBDEVICE UsbDevice,
    _Inout_ PUCX_USBDEVICE_ENABLE_INFO Info)
{
    PXHCI_UCX_USBDEVICE_CLIENT Device = XhciUcxGetUsbDeviceClient((WDFOBJECT)UsbDevice);
    PXHCI_UCX_DEVICE Dev = Device->Dev;
    PULONGLONG Dcbaa = (PULONGLONG)(Dev->HcVa + Dev->DcbaaOffset);
    PXHCI_UCX_SLOT Slot;
    ULONGLONG Ep0RingPa;
    ULONG PortId = Info->PortNumber;
    ULONG PortSc, Speed, Code = 0;
    UCHAR SlotId = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Controller);

    if (Info->Size < sizeof(*Info) || PortId == 0 || PortId > Dev->NumberOfPorts || PortId > XHCI_MAX_PORTS)
        return STATUS_INVALID_PARAMETER;
    if (Device->Slot != NULL)
        return STATUS_INVALID_DEVICE_STATE;
    if (!Dev->ControllerRunning || Dev->FatalError)
        return STATUS_DEVICE_NOT_READY;

    /* The hub driver reset the port through our root hub; the device must be there and enabled */
    PortSc = XHCI_UCX_READ_REG(&Dev->OperationalRegisters->PortRegister[PortId - 1].PortStatusAndControl);
    if ((PortSc & XHCI_PORTSC_CCS) == 0 || (PortSc & XHCI_PORTSC_PED) == 0)
    {
        XHCI_UCX_LOG("port %lu: enable asked but PORTSC=%08lx is not connected+enabled", PortId, PortSc);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    Speed = (PortSc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;

    ExAcquireFastMutex(&Dev->TransferMutex);

    Slot = XhciUcxAllocateSlot(Dev);
    if (Slot == NULL)
    {
        ExReleaseFastMutex(&Dev->TransferMutex);
        XHCI_UCX_LOG("port %lu: no free slot", PortId);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Slot->PortId = (UCHAR)PortId;
    Slot->Speed = (UCHAR)Speed;
    Slot->SpeedClass = (Dev->PortProtocol[PortId] == 3) ? XHCI_PORTSC_SPEED_SUPER : (UCHAR)Speed;
    Slot->Ep0MaxPacket = (Slot->SpeedClass == XHCI_PORTSC_SPEED_SUPER) ? 512 :
                         (Slot->SpeedClass == XHCI_PORTSC_SPEED_HIGH) ? 64 : 8;

    Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_ENABLE_SLOT, 0, 0, &Code, &SlotId);
    if (!NT_SUCCESS(Status) || SlotId == 0 || SlotId > Dev->MaxSlots || XhciUcxFindSlotById(Dev, SlotId) != NULL)
    {
        if (NT_SUCCESS(Status) && SlotId != 0 && SlotId <= Dev->MaxSlots && XhciUcxFindSlotById(Dev, SlotId) != NULL)
        {
            /* The controller handed out an id we already own: give the new one back, keep the owner */
            ULONG DisableCode = 0;
            XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_DISABLE_SLOT, 0, XHCI_COMMAND_SLOT_FIELD(SlotId), &DisableCode, NULL);
            XHCI_UCX_LOG("port %lu: Enable Slot returned the owned id %u, refused", PortId, SlotId);
            Status = STATUS_DEVICE_PROTOCOL_ERROR;
        }
        Slot->SlotId = 0;
        XhciUcxReleaseSlot(Dev, Slot, FALSE);
        ExReleaseFastMutex(&Dev->TransferMutex);
        XHCI_UCX_LOG("port %lu: Enable Slot failed 0x%08lX code %lu", PortId, (ULONG)Status, Code);
        return NT_SUCCESS(Status) ? STATUS_DEVICE_PROTOCOL_ERROR : Status;
    }
    Slot->SlotId = SlotId;

    RtlZeroMemory(Dev->HcVa + Slot->DeviceContextOffset, PAGE_SIZE);
    XhciUcxInitEp0Ring(Dev, Slot);
    Ep0RingPa = (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->Ep0RingOffset).QuadPart;
    XhciUcxBuildEp0InputContext(Dev, Slot, XHCI_UCX_INPUT_CTX_ADD_SLOT | XHCI_UCX_INPUT_CTX_ADD_EP0,
                                Ep0RingPa | XHCI_TRB_CYCLE);
    Dcbaa[SlotId] = (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->DeviceContextOffset).QuadPart;
    KeMemoryBarrier();

    Slot->UsbDevice = UsbDevice;
    Device->Slot = Slot;
    Device->PortNumber = (USHORT)PortId;
    ExReleaseFastMutex(&Dev->TransferMutex);

    Info->Speed = XhciUcxUsbSpeedFromPortSc(Speed);
    Info->MaxPacketSize0 = Slot->Ep0MaxPacket;
    XHCI_UCX_LOG("port %lu: slot %u enabled for the hub driver, speed id %lu (class %u), EP0 %lu",
                 PortId, SlotId, Speed, Slot->SpeedClass, Slot->Ep0MaxPacket);
    return STATUS_SUCCESS;
}

/* Address Device (BSR = 0); the controller picks the USB address */
static
NTSTATUS
NTAPI
XhciUcxEvtUsbDeviceAddress(
    _In_ UCXCONTROLLER Controller,
    _In_ UCXUSBDEVICE UsbDevice,
    _Out_ PUSHORT DeviceAddress)
{
    PXHCI_UCX_USBDEVICE_CLIENT Device = XhciUcxGetUsbDeviceClient((WDFOBJECT)UsbDevice);
    PXHCI_UCX_DEVICE Dev = Device->Dev;
    PXHCI_UCX_SLOT Slot = Device->Slot;
    ULONG Code = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Controller);

    *DeviceAddress = 0;
    if (Slot == NULL || Slot->Addressed)
        return STATUS_INVALID_DEVICE_STATE;

    ExAcquireFastMutex(&Dev->TransferMutex);
    Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_ADDRESS_DEV,
                                (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->InputContextOffset).QuadPart,
                                XHCI_COMMAND_SLOT_FIELD(Slot->SlotId), &Code, NULL);
    if (NT_SUCCESS(Status))
    {
        Slot->Addressed = TRUE;
        Slot->UsbAddress = (UCHAR)XHCI_UCX_SLOT_STATE_ADDRESS(XhciUcxOutputSlotContext(Dev, Slot)->DevState);
        *DeviceAddress = Slot->UsbAddress;
    }
    ExReleaseFastMutex(&Dev->TransferMutex);

    XHCI_UCX_LOG("slot %u: Address Device -> 0x%08lX code %lu, USB address %u",
                 Slot->SlotId, (ULONG)Status, Code, Slot->UsbAddress);
    return Status;
}

/* The default control endpoint object; its transfers run through XhciUcxEvtEndpointControlTransfer */
static
NTSTATUS
NTAPI
XhciUcxEvtUsbDeviceDefaultEndpointAdd(
    _In_ UCXCONTROLLER Controller,
    _In_ UCXUSBDEVICE UsbDevice,
    _In_ ULONG MaxPacketSize0,
    _Inout_ PUCXENDPOINT_INIT EndpointInit)
{
    PXHCI_UCX_USBDEVICE_CLIENT Device = XhciUcxGetUsbDeviceClient((WDFOBJECT)UsbDevice);
    UCX_ENDPOINT_EVENT_CALLBACKS Callbacks;
    WDF_OBJECT_ATTRIBUTES Attributes;
    PXHCI_UCX_ENDPOINT_CLIENT Ep;
    UCXENDPOINT Endpoint = NULL;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Controller);
    UNREFERENCED_PARAMETER(MaxPacketSize0);

    if (Device->Slot == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    UCX_ENDPOINT_EVENT_CALLBACKS_INIT(&Callbacks);
    Callbacks.EvtEndpointControlTransfer = XhciUcxEvtEndpointControlTransfer;
    UcxEndpointInitSetEventCallbacks(EndpointInit, &Callbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, XHCI_UCX_ENDPOINT_CLIENT);
    Status = UcxEndpointCreate(UsbDevice, &EndpointInit, &Attributes, &Endpoint);
    if (!NT_SUCCESS(Status))
        return Status;

    Ep = XhciUcxGetEndpointClient((WDFOBJECT)Endpoint);
    Ep->Dev = Device->Dev;
    Ep->UsbDevice = UsbDevice;
    Ep->Dci = 1;
    Device->Slot->Ep0 = Endpoint;
    return STATUS_SUCCESS;
}

/* The device descriptor named another EP0 size: Evaluate Context */
static
NTSTATUS
NTAPI
XhciUcxEvtUsbDeviceUpdateDefaultEndpoint(
    _In_ UCXCONTROLLER Controller,
    _In_ UCXUSBDEVICE UsbDevice,
    _In_ ULONG MaxPacketSize0)
{
    PXHCI_UCX_USBDEVICE_CLIENT Device = XhciUcxGetUsbDeviceClient((WDFOBJECT)UsbDevice);
    PXHCI_UCX_DEVICE Dev = Device->Dev;
    PXHCI_UCX_SLOT Slot = Device->Slot;
    ULONGLONG Ep0RingPa, Dequeue;
    ULONG Code = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Controller);

    if (Slot == NULL || !Slot->Addressed)
        return STATUS_INVALID_DEVICE_STATE;
    if (MaxPacketSize0 != 8 && MaxPacketSize0 != 16 && MaxPacketSize0 != 32 &&
        MaxPacketSize0 != 64 && MaxPacketSize0 != 512)
        return STATUS_INVALID_PARAMETER;
    if (MaxPacketSize0 == Slot->Ep0MaxPacket)
        return STATUS_SUCCESS;

    ExAcquireFastMutex(&Dev->TransferMutex);
    Ep0RingPa = (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->Ep0RingOffset).QuadPart;
    Dequeue = Ep0RingPa + (ULONGLONG)Slot->Ep0Enqueue * sizeof(XHCI_TRB);
    Slot->Ep0MaxPacket = MaxPacketSize0;
    XhciUcxBuildEp0InputContext(Dev, Slot, XHCI_UCX_INPUT_CTX_ADD_EP0, Dequeue | (Slot->Ep0Cycle & XHCI_TRB_CYCLE));
    Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_EVAL_CTX,
                                (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->InputContextOffset).QuadPart,
                                XHCI_COMMAND_SLOT_FIELD(Slot->SlotId), &Code, NULL);
    ExReleaseFastMutex(&Dev->TransferMutex);

    XHCI_UCX_LOG("slot %u: EP0 max packet -> %lu, Evaluate Context 0x%08lX code %lu",
                 Slot->SlotId, MaxPacketSize0, (ULONG)Status, Code);
    return Status;
}

/* The device is gone or failed: Disable Slot and give the slot back */
static
VOID
NTAPI
XhciUcxEvtUsbDeviceDisable(
    _In_ UCXCONTROLLER Controller,
    _In_ UCXUSBDEVICE UsbDevice)
{
    PXHCI_UCX_USBDEVICE_CLIENT Device = XhciUcxGetUsbDeviceClient((WDFOBJECT)UsbDevice);
    PXHCI_UCX_DEVICE Dev = Device->Dev;
    PXHCI_UCX_SLOT Slot = Device->Slot;

    UNREFERENCED_PARAMETER(Controller);

    if (Slot == NULL)
        return;

    ExAcquireFastMutex(&Dev->TransferMutex);
    XHCI_UCX_LOG("slot %u (port %u): disabled for the hub driver", Slot->SlotId, Slot->PortId);
    XhciUcxSlotDropEndpoints(Dev, Slot, USBD_STATUS_DEVICE_GONE);
    Slot->UsbDevice = NULL;
    Slot->Ep0 = NULL;
    XhciUcxReleaseSlot(Dev, Slot, TRUE);
    Device->Slot = NULL;
    ExReleaseFastMutex(&Dev->TransferMutex);
}

/* The class library asks for a device on a root port: our callbacks, our context, the object */
static
NTSTATUS
NTAPI
XhciUcxEvtControllerUsbDeviceAdd(
    _In_ UCXCONTROLLER Controller,
    _In_ PUCX_USBDEVICE_INFO Info,
    _Inout_ PUCXUSBDEVICE_INIT UsbDeviceInit)
{
    PXHCI_UCX_DEVICE Dev = XhciUcxGetControllerClient((WDFOBJECT)Controller)->Dev;
    UCX_USBDEVICE_EVENT_CALLBACKS Callbacks;
    WDF_OBJECT_ATTRIBUTES Attributes;
    PXHCI_UCX_USBDEVICE_CLIENT Device;
    UCXUSBDEVICE UsbDevice = NULL;
    NTSTATUS Status;

    if (Info->Size < sizeof(*Info) || Info->PortNumber == 0 || Info->PortNumber > Dev->NumberOfPorts)
        return STATUS_INVALID_PARAMETER;
    if (Dev->SelfEnumerate)
        return STATUS_NOT_SUPPORTED;           /* UseUcx=2: the client owns the ports */

    UCX_USBDEVICE_EVENT_CALLBACKS_INIT(&Callbacks);
    Callbacks.EvtUsbDeviceEnable = XhciUcxEvtUsbDeviceEnable;
    Callbacks.EvtUsbDeviceAddress = XhciUcxEvtUsbDeviceAddress;
    Callbacks.EvtUsbDeviceDefaultEndpointAdd = XhciUcxEvtUsbDeviceDefaultEndpointAdd;
    Callbacks.EvtUsbDeviceUpdateDefaultEndpoint = XhciUcxEvtUsbDeviceUpdateDefaultEndpoint;
    Callbacks.EvtUsbDeviceDisable = XhciUcxEvtUsbDeviceDisable;
    Callbacks.EvtUsbDeviceEndpointAdd = XhciUcxEvtUsbDeviceEndpointAdd;
    Callbacks.EvtUsbDeviceEndpointsConfigure = XhciUcxEvtUsbDeviceEndpointsConfigure;
    UcxUsbDeviceInitSetEventCallbacks(UsbDeviceInit, &Callbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, XHCI_UCX_USBDEVICE_CLIENT);
    Status = UcxUsbDeviceCreate(Controller, &UsbDeviceInit, &Attributes, &UsbDevice);
    if (!NT_SUCCESS(Status))
    {
        XHCI_UCX_LOG("port %u: UcxUsbDeviceCreate failed 0x%08lX", Info->PortNumber, (ULONG)Status);
        return Status;
    }

    Device = XhciUcxGetUsbDeviceClient((WDFOBJECT)UsbDevice);
    Device->Dev = Dev;
    Device->Slot = NULL;
    Device->PortNumber = Info->PortNumber;
    XHCI_UCX_LOG("port %u: device object %p for the hub driver (port status %04X)",
                 Info->PortNumber, UsbDevice, Info->PortStatus);
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Configured endpoints (M3 step 3). Each bulk/interrupt endpoint of a device
 * on a root port gets its own transfer ring (one page of TRBs in its own
 * common buffer), a parallel WDF queue on this FDO that ucx01000 forwards the
 * class driver's transfer requests to, and a manual queue for the ones the
 * ring has no room for yet. A transfer is one TD: one Normal TRB per
 * physically contiguous run of the caller's MDL (never across a 64 KB
 * boundary), chained, IOC on the last, ISP on IN. Completions are matched in
 * the event-ring drain by slot, DCI and TRB pointer against the endpoint's
 * in-flight list and completed after the locks are dropped. The caller's
 * memory is handed to the controller as the MDL describes it; nothing the
 * controller writes to the event ring is trusted beyond range checks.
 * -------------------------------------------------------------------------- */

#define XHCI_UCX_EP_RING_TRBS     (PAGE_SIZE / sizeof(XHCI_TRB))     /* 256: 255 usable + link */
#define XHCI_UCX_EP_RING_USABLE   (XHCI_UCX_EP_RING_TRBS - 1)
#define XHCI_UCX_EP_MAX_TD_TRBS   128                                /* a TD never takes more than half the ring */
#define XHCI_UCX_TRB_MAX_RUN      (64 * 1024)

/* Walks the physical runs of an MDL: one run per contiguous stretch within a 64 KB window */
typedef struct _XHCI_UCX_MDL_RUNS {
    PPFN_NUMBER Pfn;
    ULONG PageCount;
    ULONG PageIndex;
    ULONG PageOffset;
    ULONG Remaining;
} XHCI_UCX_MDL_RUNS, *PXHCI_UCX_MDL_RUNS;

static
VOID
XhciUcxMdlRunsInit(
    _Out_ PXHCI_UCX_MDL_RUNS It,
    _In_ PMDL Mdl,
    _In_ ULONG Length)
{
    It->Pfn = MmGetMdlPfnArray(Mdl);
    It->PageOffset = MmGetMdlByteOffset(Mdl);
    It->PageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(Mdl), Length);
    It->PageIndex = 0;
    It->Remaining = Length;
}

static
BOOLEAN
XhciUcxMdlRunsNext(
    _Inout_ PXHCI_UCX_MDL_RUNS It,
    _Out_ PULONGLONG RunPa,
    _Out_ PULONG RunLength)
{
    ULONGLONG Pa;
    ULONG Len;

    *RunPa = 0;
    *RunLength = 0;
    if (It->Remaining == 0 || It->PageIndex >= It->PageCount)
        return FALSE;

    Pa = ((ULONGLONG)It->Pfn[It->PageIndex] << PAGE_SHIFT) + It->PageOffset;
    Len = min(It->Remaining, PAGE_SIZE - It->PageOffset);
    It->PageIndex++;
    It->PageOffset = 0;
    It->Remaining -= Len;

    /* Merge the following pages while they are contiguous and the run stays inside one 64 KB window */
    while (It->Remaining != 0 && It->PageIndex < It->PageCount)
    {
        ULONGLONG NextPa = (ULONGLONG)It->Pfn[It->PageIndex] << PAGE_SHIFT;
        ULONG NextLen = min(It->Remaining, (ULONG)PAGE_SIZE);

        if (NextPa != Pa + Len)
            break;
        if (((Pa + Len + NextLen - 1) & ~(ULONGLONG)(XHCI_UCX_TRB_MAX_RUN - 1)) != (Pa & ~(ULONGLONG)(XHCI_UCX_TRB_MAX_RUN - 1)))
            break;
        Len += NextLen;
        It->PageIndex++;
        It->Remaining -= NextLen;
    }
    *RunPa = Pa;
    *RunLength = Len;
    return TRUE;
}

/* Number of TRBs a transfer needs, or 0 when it cannot be carried */
static
ULONG
XhciUcxMdlTrbCount(
    _In_ PXHCI_UCX_DEVICE Dev,
    _In_opt_ PMDL Mdl,
    _In_ ULONG Length)
{
    XHCI_UCX_MDL_RUNS It;
    ULONGLONG Pa;
    ULONG Len, Count = 0;

    if (Length == 0)
        return 1;                                   /* a zero-length packet is one TRB */
    if (Mdl == NULL)
        return 0;
    XhciUcxMdlRunsInit(&It, Mdl, Length);
    while (XhciUcxMdlRunsNext(&It, &Pa, &Len))
    {
        if (!Dev->Supports64Bit && (Pa + Len) > 0x100000000ULL)
            return 0;                               /* a 32-bit controller cannot reach this page */
        if (++Count > XHCI_UCX_EP_MAX_TD_TRBS)
            return 0;
    }
    return (It.Remaining == 0) ? Count : 0;
}

/* xHCI endpoint context Interval field (6.2.3.6) from the descriptor's bInterval */
static
UCHAR
XhciUcxEndpointIntervalField(
    _In_ UCHAR SpeedClass,
    _In_ UCHAR Type,
    _In_ UCHAR bInterval)
{
    ULONG Field;

    if (Type == USB_ENDPOINT_TYPE_BULK || Type == USB_ENDPOINT_TYPE_CONTROL)
        return 0;
    if (SpeedClass == XHCI_PORTSC_SPEED_HIGH || SpeedClass == XHCI_PORTSC_SPEED_SUPER)
    {
        /* 2^(bInterval-1) microframes: the field is bInterval - 1, 0..15 */
        if (bInterval == 0)
            return 0;
        return (UCHAR)min(bInterval - 1, 15);
    }
    if (Type == USB_ENDPOINT_TYPE_ISOCHRONOUS)
        return (UCHAR)min((bInterval ? bInterval - 1 : 0) + 3, 18);   /* full speed isoch: 2^(n-1) frames */
    /* Full/low-speed interrupt: bInterval frames, rounded down to a power of two, in 125 us units */
    Field = 3;
    while (bInterval >= 2 && Field < 10)
    {
        bInterval >>= 1;
        Field++;
    }
    return (UCHAR)Field;
}

static
UCHAR
XhciUcxEndpointTypeField(
    _In_ UCHAR Type,
    _In_ BOOLEAN In)
{
    switch (Type)
    {
        case USB_ENDPOINT_TYPE_ISOCHRONOUS: return In ? XHCI_ENDPOINT_TYPE_ISOCH_IN : XHCI_ENDPOINT_TYPE_ISOCH_OUT;
        case USB_ENDPOINT_TYPE_BULK:        return In ? XHCI_ENDPOINT_TYPE_BULK_IN : XHCI_ENDPOINT_TYPE_BULK_OUT;
        case USB_ENDPOINT_TYPE_INTERRUPT:   return In ? XHCI_ENDPOINT_TYPE_INTERRUPT_IN : XHCI_ENDPOINT_TYPE_INTERRUPT_OUT;
        default:                            return XHCI_ENDPOINT_TYPE_CONTROL;
    }
}

static
VOID
XhciUcxEpResetRing(
    _Inout_ PXHCI_UCX_ENDPOINT_CLIENT Ep)
{
    PXHCI_TRB Link = &Ep->Ring[XHCI_UCX_EP_RING_TRBS - 1];

    RtlZeroMemory(Ep->Ring, PAGE_SIZE);
    Link->Parameter1 = (ULONG)(Ep->RingPa & 0xFFFFFFFF);
    Link->Parameter2 = (ULONG)(Ep->RingPa >> 32);
    Link->Status = 0;
    Link->Control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    KeMemoryBarrier();
    Ep->Enqueue = 0;
    Ep->Dequeue = 0;
    Ep->Cycle = 1;
}

/* Usable TRBs between the producer and the oldest TD still on the ring */
static
ULONG
XhciUcxEpFreeTrbs(
    _In_ PXHCI_UCX_ENDPOINT_CLIENT Ep)
{
    ULONG Used = (Ep->Enqueue + XHCI_UCX_EP_RING_USABLE - Ep->Dequeue) % XHCI_UCX_EP_RING_USABLE;

    return XHCI_UCX_EP_RING_USABLE - 1 - Used;
}

static
ULONG
XhciUcxEpIndexAdd(
    _In_ ULONG Index,
    _In_ ULONG Count)
{
    return (Index + Count) % XHCI_UCX_EP_RING_USABLE;
}

/* Bytes of the TD that precede ring index Index (its TRBs from First up to Index, exclusive) */
static
ULONG
XhciUcxEpTdOffsetAt(
    _In_ PXHCI_UCX_ENDPOINT_CLIENT Ep,
    _In_ PXHCI_UCX_TD_CTX Td,
    _In_ ULONG Index)
{
    ULONG i = Td->FirstIndex, Offset = 0, n;

    for (n = 0; n < Td->TrbCount && i != Index; n++)
    {
        Offset += Ep->Ring[i].Status & XHCI_TRB_LEN_MASK;
        i = XhciUcxEpIndexAdd(i, 1);
    }
    return Offset;
}

static
BOOLEAN
XhciUcxEpTdContains(
    _In_ PXHCI_UCX_TD_CTX Td,
    _In_ ULONG Index)
{
    ULONG i = Td->FirstIndex, n;

    for (n = 0; n < Td->TrbCount; n++)
    {
        if (i == Index)
            return TRUE;
        i = XhciUcxEpIndexAdd(i, 1);
    }
    return FALSE;
}

/*
 * Write the TD of a transfer onto the ring and ring the doorbell. Caller holds
 * EndpointLock and checked the room. The first TRB is published last (its
 * cycle bit), so the controller never sees a partial TD.
 */
static
VOID
XhciUcxEpWriteTd(
    _Inout_ PXHCI_UCX_ENDPOINT_CLIENT Ep,
    _Inout_ PXHCI_UCX_TD_CTX Td,
    _In_opt_ PMDL Mdl)
{
    PXHCI_UCX_DEVICE Dev = Ep->Dev;
    XHCI_UCX_MDL_RUNS It;
    PXHCI_TRB First = NULL;
    ULONG FirstControl = 0;
    ULONGLONG Pa = 0;
    ULONG Len = 0, Done = 0, n = 0;
    BOOLEAN More;

    Td->FirstIndex = Ep->Enqueue;
    if (Td->Length != 0)
        XhciUcxMdlRunsInit(&It, Mdl, Td->Length);

    More = (Td->Length != 0) ? XhciUcxMdlRunsNext(&It, &Pa, &Len) : TRUE;
    while (More)
    {
        PXHCI_TRB Trb = &Ep->Ring[Ep->Enqueue];
        ULONG Cycle = Ep->Cycle & XHCI_TRB_CYCLE;
        ULONG Control, TdSize, Left;
        BOOLEAN Last;
        ULONGLONG NextPa = 0;
        ULONG NextLen = 0;

        if (Td->Length != 0)
        {
            More = XhciUcxMdlRunsNext(&It, &NextPa, &NextLen);
            Last = !More;
        }
        else
        {
            Last = TRUE;
            More = FALSE;
        }
        Done += Len;
        Left = Td->Length - Done;
        TdSize = Last ? 0 : min((Left + Ep->MaxPacket - 1) / Ep->MaxPacket, (ULONG)XHCI_TRB_TD_SIZE_MAX);

        Trb->Parameter1 = (ULONG)(Pa & 0xFFFFFFFF);
        Trb->Parameter2 = (ULONG)(Pa >> 32);
        Trb->Status = (Len & XHCI_TRB_LEN_MASK) | (TdSize << XHCI_TRB_TD_SIZE_SHIFT);
        Control = (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) |
                  (Last ? XHCI_TRB_IOC : XHCI_TRB_CHAIN_BIT) |
                  (Td->DataIn ? XHCI_TRB_ISP : 0);
        if (First == NULL)
        {
            First = Trb;
            FirstControl = Control | Cycle;
            Trb->Control = Control | (Cycle ^ XHCI_TRB_CYCLE);   /* not yet */
        }
        else
        {
            Trb->Control = Control | Cycle;
        }
        Td->LastIndex = Ep->Enqueue;
        n++;

        /* Advance; crossing the link: the link carries this pass's cycle and the chain bit of a TD that spans it */
        Ep->Enqueue++;
        if (Ep->Enqueue >= XHCI_UCX_EP_RING_USABLE)
        {
            PXHCI_TRB Link = &Ep->Ring[XHCI_UCX_EP_RING_TRBS - 1];

            Link->Control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TOGGLE_CYCLE |
                            (Last ? 0 : XHCI_TRB_CHAIN_BIT) | (Ep->Cycle & XHCI_TRB_CYCLE);
            Ep->Enqueue = 0;
            Ep->Cycle ^= 1;
        }
        Pa = NextPa;
        Len = NextLen;
    }
    Td->TrbCount = n;
    Td->Transferred = 0;
    Td->ShortSeen = FALSE;
    Td->Ep = Ep;
    InsertTailList(&Ep->InFlight, &Td->Link);
    KeMemoryBarrier();
    First->Control = FirstControl;
    KeMemoryBarrier();
    XHCI_UCX_WRITE_REG(&Dev->DoorbellArray->Doorbell[Ep->Slot->SlotId], Ep->Dci);
    XHCI_UCX_TRACE("slot %u ep 0x%02x: TD %lu bytes in %lu TRB(s) at %lu..%lu",
                   Ep->Slot->SlotId, Ep->Address, Td->Length, n, Td->FirstIndex, Td->LastIndex);
}

static
VOID
XhciUcxEpCompleteRequest(
    _In_ PXHCI_UCX_ENDPOINT_CLIENT Ep,
    _In_ PXHCI_UCX_TD_CTX Td,
    _In_ USBD_STATUS UsbdStatus)
{
    WDFREQUEST Request = Td->Request;
    UCXENDPOINT Endpoint = Ep->Self;

    Td->Request = NULL;
    UcxEndpointCompleteTransfer(Endpoint, Request, UsbdStatus, Td->Transferred);
    WdfObjectDereferenceWithTag((WDFOBJECT)Endpoint, (PVOID)'dTcU');
}

/* Starts transfers from the waiting queue while the ring has room; EndpointLock held on entry and exit */
static
VOID
XhciUcxEpKickLocked(
    _Inout_ PXHCI_UCX_ENDPOINT_CLIENT Ep,
    _Inout_ PLIST_ENTRY Failed)
{
    while (Ep->Configured && !Ep->Disabled && !Ep->Halted && Ep->Waiting != NULL)
    {
        WDFREQUEST Request;
        PXHCI_UCX_TD_CTX Td;
        PURB Urb;
        PMDL Mdl;
        ULONG Count;

        /* Peek: the next waiting request's TD size decides whether it can start */
        if (!NT_SUCCESS(WdfIoQueueFindRequest(Ep->Waiting, NULL, NULL, NULL, &Request)))
            break;
        Td = XhciUcxGetTdCtx((WDFOBJECT)Request);
        Urb = (PURB)UcxRequestGetUrb(Request);
        Mdl = Urb->UrbBulkOrInterruptTransfer.TransferBufferMDL;
        Count = XhciUcxMdlTrbCount(Ep->Dev, Mdl, Td->Length);
        if (Count != 0 && Count > XhciUcxEpFreeTrbs(Ep))
        {
            WdfObjectDereference((WDFOBJECT)Request);   /* FindRequest's reference */
            break;
        }
        if (!NT_SUCCESS(WdfIoQueueRetrieveFoundRequest(Ep->Waiting, Request, &Request)))
        {
            WdfObjectDereference((WDFOBJECT)Request);
            break;
        }
        WdfObjectDereference((WDFOBJECT)Request);       /* FindRequest's reference; we own the request now */
        if (Count == 0)
        {
            Td->UsbdStatus = USBD_STATUS_INVALID_PARAMETER;
            InsertTailList(Failed, &Td->Link);
            continue;
        }
        XhciUcxEpWriteTd(Ep, Td, Mdl);
    }
}

/* Takes every in-flight and waiting transfer off the endpoint (EndpointLock held) */
static
VOID
XhciUcxEpCollectAllLocked(
    _Inout_ PXHCI_UCX_ENDPOINT_CLIENT Ep,
    _In_ USBD_STATUS UsbdStatus,
    _Inout_ PLIST_ENTRY List)
{
    WDFREQUEST Request;

    while (!IsListEmpty(&Ep->InFlight))
    {
        PXHCI_UCX_TD_CTX Td = CONTAINING_RECORD(RemoveHeadList(&Ep->InFlight), XHCI_UCX_TD_CTX, Link);

        Td->UsbdStatus = UsbdStatus;
        Td->Transferred = 0;
        InsertTailList(List, &Td->Link);
    }
    Ep->Dequeue = Ep->Enqueue;
    while (Ep->Waiting != NULL && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Ep->Waiting, &Request)))
    {
        PXHCI_UCX_TD_CTX Td = XhciUcxGetTdCtx((WDFOBJECT)Request);

        Td->UsbdStatus = UsbdStatus;
        Td->Transferred = 0;
        InsertTailList(List, &Td->Link);
    }
}

static
VOID
XhciUcxEpCompleteList(
    _In_opt_ PXHCI_UCX_ENDPOINT_CLIENT Ep,
    _Inout_ PLIST_ENTRY List)
{
    while (!IsListEmpty(List))
    {
        PXHCI_UCX_TD_CTX Td = CONTAINING_RECORD(RemoveHeadList(List), XHCI_UCX_TD_CTX, Link);

        XhciUcxEpCompleteRequest(Ep != NULL ? Ep : Td->Ep, Td, Td->UsbdStatus);
    }
}

/*
 * A transfer event for a configured endpoint (EventRingLock held, DISPATCH).
 * Finds the TD by TRB pointer, accounts the bytes, moves finished TDs to
 * Completed for the caller to complete after it drops its lock.
 */
static
BOOLEAN
XhciUcxEpHandleTransferEvent(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _In_ PXHCI_UCX_SLOT Slot,
    _In_ ULONG EndpointId,
    _In_ ULONGLONG TrbPointer,
    _In_ ULONG CompletionCode,
    _In_ ULONG Residual,
    _Inout_ PLIST_ENTRY Completed)
{
    PXHCI_UCX_ENDPOINT_CLIENT Ep;
    KIRQL OldIrql;
    PLIST_ENTRY Entry;
    PXHCI_UCX_TD_CTX Td = NULL;
    ULONG Index;
    BOOLEAN Finished = FALSE, Halted = FALSE;

    KeAcquireSpinLock(&Dev->EndpointLock, &OldIrql);
    Ep = (EndpointId < 32) ? Slot->Endpoints[EndpointId] : NULL;
    if (Ep == NULL || Ep->Ring == NULL)
    {
        KeReleaseSpinLock(&Dev->EndpointLock, OldIrql);
        return FALSE;                        /* not a configured endpoint of ours: the EP0 path decides */
    }
    if (TrbPointer < Ep->RingPa || TrbPointer >= Ep->RingPa + PAGE_SIZE || (TrbPointer & (sizeof(XHCI_TRB) - 1)) != 0)
    {
        KeReleaseSpinLock(&Dev->EndpointLock, OldIrql);
        XHCI_UCX_LOG("slot %u ep 0x%02x: transfer event points outside the ring, ignored", Slot->SlotId, Ep->Address);
        return TRUE;
    }
    Index = (ULONG)((TrbPointer - Ep->RingPa) / sizeof(XHCI_TRB));

    for (Entry = Ep->InFlight.Flink; Entry != &Ep->InFlight; Entry = Entry->Flink)
    {
        PXHCI_UCX_TD_CTX Candidate = CONTAINING_RECORD(Entry, XHCI_UCX_TD_CTX, Link);

        if (XhciUcxEpTdContains(Candidate, Index))
        {
            Td = Candidate;
            break;
        }
    }
    if (Td == NULL)
    {
        KeReleaseSpinLock(&Dev->EndpointLock, OldIrql);
        XHCI_UCX_TRACE("slot %u ep 0x%02x: event for TRB %lu matches no TD (code %lu)", Slot->SlotId, Ep->Address, Index, CompletionCode);
        return TRUE;
    }

    switch (CompletionCode)
    {
        case XHCI_COMPLETION_SUCCESS:
            if (Index == Td->LastIndex)
            {
                if (!Td->ShortSeen)
                    Td->Transferred = Td->Length - min(Residual, Td->Length);
                Td->UsbdStatus = USBD_STATUS_SUCCESS;
                Finished = TRUE;
            }
            break;
        case XHCI_COMPLETION_SHORT_PACKET:
        {
            ULONG TrbLen = Ep->Ring[Index].Status & XHCI_TRB_LEN_MASK;

            if (!Td->ShortSeen)
            {
                Td->Transferred = XhciUcxEpTdOffsetAt(Ep, Td, Index) + TrbLen - min(Residual, TrbLen);
                Td->Transferred = min(Td->Transferred, Td->Length);
                Td->ShortSeen = TRUE;
            }
            if (Index == Td->LastIndex)
            {
                Td->UsbdStatus = USBD_STATUS_SUCCESS;
                Finished = TRUE;
            }
            break;
        }
        case XHCI_COMPLETION_STOPPED:
        case XHCI_COMPLETION_STOPPED_LENGTH_INVALID:
        case XHCI_COMPLETION_STOPPED_SHORT_PACKET:
            /* Stop Endpoint during an abort: the abort path completes the TD */
            break;
        default:
        {
            ULONG TrbLen = Ep->Ring[Index].Status & XHCI_TRB_LEN_MASK;

            Td->Transferred = 0;
            if (CompletionCode == XHCI_COMPLETION_DATA_BUFFER_ERROR || CompletionCode == XHCI_COMPLETION_BABBLE_ERROR)
                Td->Transferred = min(XhciUcxEpTdOffsetAt(Ep, Td, Index) + TrbLen - min(Residual, TrbLen), Td->Length);
            Td->UsbdStatus = XhciUcxUsbdStatusFromCompletion(CompletionCode, STATUS_UNSUCCESSFUL);
            Finished = TRUE;
            Halted = TRUE;                   /* the controller halted the endpoint (4.10.2) */
            XHCI_UCX_LOG("slot %u ep 0x%02x: transfer failed, completion code %lu (%lu of %lu bytes)",
                         Ep->Slot->SlotId, Ep->Address, CompletionCode, Td->Transferred, Td->Length);
            break;
        }
    }

    if (Finished)
    {
        RemoveEntryList(&Td->Link);
        InsertTailList(Completed, &Td->Link);
        if (Ep->InFlight.Flink == &Ep->InFlight)
            Ep->Dequeue = Ep->Enqueue;
        else
            Ep->Dequeue = XhciUcxEpIndexAdd(Td->LastIndex, 1);
        if (Halted)
        {
            /* Everything behind the failed TD is stuck until the pipe is reset */
            Ep->Halted = TRUE;
            XhciUcxEpCollectAllLocked(Ep, USBD_STATUS_ENDPOINT_HALTED, Completed);
        }
        else
        {
            XhciUcxEpKickLocked(Ep, Completed);
        }
    }
    KeReleaseSpinLock(&Dev->EndpointLock, OldIrql);
    return TRUE;
}

/* Transfer requests the class library forwards to this endpoint's queue */
static
VOID
NTAPI
XhciUcxEvtEndpointIoctl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PXHCI_UCX_ENDPOINT_CLIENT Ep = XhciUcxGetQueueClient((WDFOBJECT)Queue)->Ep;
    WDF_OBJECT_ATTRIBUTES Attributes;
    PXHCI_UCX_TD_CTX Td = NULL;
    PURB Urb;
    PMDL Mdl;
    ULONG Count;
    KIRQL OldIrql;
    LIST_ENTRY Failed;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    Urb = (PURB)UcxRequestGetUrb(Request);
    if (IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB || Urb == NULL ||
        Urb->UrbHeader.Length < sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER) ||
        Urb->UrbHeader.Function != URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER)
    {
        UcxEndpointCompleteTransfer(Ep->Self, Request, USBD_STATUS_INVALID_URB_FUNCTION, 0);
        return;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, XHCI_UCX_TD_CTX);
    Status = WdfObjectAllocateContext((WDFOBJECT)Request, &Attributes, (PVOID *)&Td);
    if (Status == STATUS_OBJECT_NAME_EXISTS)
        Td = XhciUcxGetTdCtx((WDFOBJECT)Request);
    if (Td == NULL)
    {
        UcxEndpointCompleteTransfer(Ep->Self, Request, USBD_STATUS_INSUFFICIENT_RESOURCES, 0);
        return;
    }
    RtlZeroMemory(Td, sizeof(*Td));
    Td->Request = Request;
    Td->Ep = Ep;
    Td->Length = Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    Td->DataIn = (Urb->UrbBulkOrInterruptTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
    Mdl = Urb->UrbBulkOrInterruptTransfer.TransferBufferMDL;

    Count = XhciUcxMdlTrbCount(Ep->Dev, Mdl, Td->Length);
    if (Count == 0 || Td->Length > Ep->MaximumTransferSize)
    {
        UcxEndpointCompleteTransfer(Ep->Self, Request, USBD_STATUS_INVALID_PARAMETER, 0);
        return;
    }

    /* The endpoint object outlives every transfer it holds */
    WdfObjectReferenceWithTag((WDFOBJECT)Ep->Self, (PVOID)'dTcU');
    InitializeListHead(&Failed);

    KeAcquireSpinLock(&Ep->Dev->EndpointLock, &OldIrql);
    if (!Ep->Configured || Ep->Disabled)
    {
        KeReleaseSpinLock(&Ep->Dev->EndpointLock, OldIrql);
        XhciUcxEpCompleteRequest(Ep, Td, USBD_STATUS_DEVICE_GONE);
        return;
    }
    if (Ep->Halted)
    {
        KeReleaseSpinLock(&Ep->Dev->EndpointLock, OldIrql);
        XhciUcxEpCompleteRequest(Ep, Td, USBD_STATUS_ENDPOINT_HALTED);
        return;
    }
    if (Ep->Waiting == NULL)
    {
        KeReleaseSpinLock(&Ep->Dev->EndpointLock, OldIrql);
        XhciUcxEpCompleteRequest(Ep, Td, USBD_STATUS_DEVICE_GONE);
        return;
    }
    if (WdfIoQueueGetState(Ep->Waiting, NULL, NULL) & WdfIoQueueNoRequests)
    {
        if (Count <= XhciUcxEpFreeTrbs(Ep))
        {
            XhciUcxEpWriteTd(Ep, Td, Mdl);
            KeReleaseSpinLock(&Ep->Dev->EndpointLock, OldIrql);
            return;
        }
    }
    /* No room, or others wait already: keep the order */
    Status = WdfRequestForwardToIoQueue(Request, Ep->Waiting);
    if (!NT_SUCCESS(Status))
    {
        KeReleaseSpinLock(&Ep->Dev->EndpointLock, OldIrql);
        XhciUcxEpCompleteRequest(Ep, Td, USBD_STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    XhciUcxEpKickLocked(Ep, &Failed);
    KeReleaseSpinLock(&Ep->Dev->EndpointLock, OldIrql);
    XhciUcxEpCompleteList(Ep, &Failed);
}

/* Stop Endpoint + Set TR Dequeue Pointer at the producer: the ring is empty from the controller's view */
static
NTSTATUS
XhciUcxEpStopAndFlush(
    _Inout_ PXHCI_UCX_ENDPOINT_CLIENT Ep,
    _In_ BOOLEAN ResetFirst)
{
    PXHCI_UCX_DEVICE Dev = Ep->Dev;
    ULONG Fields = XHCI_COMMAND_SLOT_FIELD(Ep->Slot->SlotId) | XHCI_UCX_COMMAND_EP_FIELD(Ep->Dci);
    ULONGLONG Dequeue;
    ULONG Code = 0;
    NTSTATUS Status;

    if (!Dev->ControllerRunning || Dev->FatalError)
        return STATUS_DEVICE_NOT_READY;

    if (ResetFirst)
    {
        Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_RESET_EP, 0, Fields, &Code, NULL);
        XHCI_UCX_LOG("slot %u ep 0x%02x: Reset Endpoint rc=0x%08lX code=%lu", Ep->Slot->SlotId, Ep->Address, (ULONG)Status, Code);
    }
    else
    {
        Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_STOP_EP, 0, Fields, &Code, NULL);
        XHCI_UCX_TRACE("slot %u ep 0x%02x: Stop Endpoint rc=0x%08lX code=%lu", Ep->Slot->SlotId, Ep->Address, (ULONG)Status, Code);
    }
    /* Context State Error: the endpoint was already stopped or halted; the dequeue move still applies */
    if (!NT_SUCCESS(Status) && Code != XHCI_COMPLETION_CONTEXT_STATE_ERROR)
        return Status;

    Dequeue = Ep->RingPa + (ULONGLONG)Ep->Enqueue * sizeof(XHCI_TRB);
    Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_SET_DEQ, Dequeue | (Ep->Cycle & XHCI_TRB_CYCLE), Fields, &Code, NULL);
    XHCI_UCX_TRACE("slot %u ep 0x%02x: Set TR Dequeue Pointer rc=0x%08lX code=%lu", Ep->Slot->SlotId, Ep->Address, (ULONG)Status, Code);
    return Status;
}

/* EvtEndpointAbort: everything the endpoint holds comes back canceled */
static
NTSTATUS
NTAPI
XhciUcxEvtEndpointAbort(
    _In_ UCXENDPOINT Endpoint)
{
    PXHCI_UCX_ENDPOINT_CLIENT Ep = XhciUcxGetEndpointClient((WDFOBJECT)Endpoint);
    PXHCI_UCX_DEVICE Dev = Ep->Dev;
    LIST_ENTRY Canceled;
    KIRQL OldIrql;
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN Halted;

    if (Ep->Slot == NULL)
        return STATUS_SUCCESS;
    InitializeListHead(&Canceled);

    ExAcquireFastMutex(&Dev->TransferMutex);
    KeAcquireSpinLock(&Dev->EndpointLock, &OldIrql);
    Halted = Ep->Halted;
    if (Ep->Configured && !Ep->Disabled)
    {
        KeReleaseSpinLock(&Dev->EndpointLock, OldIrql);
        Status = XhciUcxEpStopAndFlush(Ep, Halted);
        KeAcquireSpinLock(&Dev->EndpointLock, &OldIrql);
    }
    XhciUcxEpCollectAllLocked(Ep, USBD_STATUS_CANCELED, &Canceled);
    Ep->Halted = FALSE;
    KeReleaseSpinLock(&Dev->EndpointLock, OldIrql);
    ExReleaseFastMutex(&Dev->TransferMutex);

    XHCI_UCX_LOG("slot %u ep 0x%02x: aborted -> 0x%08lX", Ep->Slot->SlotId, Ep->Address, (ULONG)Status);
    XhciUcxEpCompleteList(Ep, &Canceled);
    return Status;
}

/* EvtEndpointResetHalted: Reset Endpoint + Set TR Dequeue Pointer */
static
NTSTATUS
NTAPI
XhciUcxEvtEndpointResetHalted(
    _In_ UCXENDPOINT Endpoint)
{
    PXHCI_UCX_ENDPOINT_CLIENT Ep = XhciUcxGetEndpointClient((WDFOBJECT)Endpoint);
    PXHCI_UCX_DEVICE Dev = Ep->Dev;
    LIST_ENTRY Canceled;
    KIRQL OldIrql;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Ep->Slot == NULL)
        return STATUS_SUCCESS;
    InitializeListHead(&Canceled);

    ExAcquireFastMutex(&Dev->TransferMutex);
    if (Ep->Configured && !Ep->Disabled)
        Status = XhciUcxEpStopAndFlush(Ep, TRUE);
    KeAcquireSpinLock(&Dev->EndpointLock, &OldIrql);
    XhciUcxEpCollectAllLocked(Ep, USBD_STATUS_CANCELED, &Canceled);
    Ep->Halted = FALSE;
    KeReleaseSpinLock(&Dev->EndpointLock, OldIrql);
    ExReleaseFastMutex(&Dev->TransferMutex);

    XHCI_UCX_LOG("slot %u ep 0x%02x: halted pipe reset -> 0x%08lX", Ep->Slot->SlotId, Ep->Address, (ULONG)Status);
    XhciUcxEpCompleteList(Ep, &Canceled);
    return Status;
}

/* Object cleanup: the endpoint object goes (after it was disabled on the controller) */
static
VOID
NTAPI
XhciUcxEndpointEvtCleanup(
    _In_ WDFOBJECT Object)
{
    PXHCI_UCX_ENDPOINT_CLIENT Ep = XhciUcxGetEndpointClient(Object);
    PXHCI_UCX_DEVICE Dev = Ep->Dev;
    WDFQUEUE Queue, Waiting;
    WDFCOMMONBUFFER RingBuffer;
    LIST_ENTRY Gone;
    KIRQL OldIrql;

    if (Dev == NULL || Ep->Dci == 1)
        return;                                 /* the default endpoint owns nothing here */
    InitializeListHead(&Gone);

    KeAcquireSpinLock(&Dev->EndpointLock, &OldIrql);
    Ep->Disabled = TRUE;
    Ep->Configured = FALSE;
    XhciUcxEpCollectAllLocked(Ep, USBD_STATUS_DEVICE_GONE, &Gone);
    if (Ep->Slot != NULL && Ep->Slot->Endpoints[Ep->Dci] == Ep)
        Ep->Slot->Endpoints[Ep->Dci] = NULL;
    Queue = Ep->Queue;
    Waiting = Ep->Waiting;
    RingBuffer = Ep->RingBuffer;
    Ep->Queue = NULL;
    Ep->Waiting = NULL;
    Ep->RingBuffer = NULL;
    Ep->Ring = NULL;
    KeReleaseSpinLock(&Dev->EndpointLock, OldIrql);

    XhciUcxEpCompleteList(Ep, &Gone);
    if (Queue != NULL)
    {
        WdfIoQueuePurgeSynchronously(Queue);
        WdfObjectDelete((WDFOBJECT)Queue);
    }
    if (Waiting != NULL)
    {
        WdfIoQueuePurgeSynchronously(Waiting);
        WdfObjectDelete((WDFOBJECT)Waiting);
    }
    if (RingBuffer != NULL)
        WdfObjectDelete((WDFOBJECT)RingBuffer);
    XHCI_UCX_TRACE("ep 0x%02x object %p released", Ep->Address, Object);
}

/* EvtUsbDeviceEndpointAdd: ring, queues, the endpoint object with our context */
static
NTSTATUS
NTAPI
XhciUcxEvtUsbDeviceEndpointAdd(
    _In_ UCXCONTROLLER Controller,
    _In_ UCXUSBDEVICE UsbDevice,
    _In_ PUCX_ENDPOINT_INFO Info,
    _Inout_ PUCXENDPOINT_INIT EndpointInit)
{
    PXHCI_UCX_USBDEVICE_CLIENT Device = XhciUcxGetUsbDeviceClient((WDFOBJECT)UsbDevice);
    PXHCI_UCX_DEVICE Dev = Device->Dev;
    PXHCI_UCX_SLOT Slot = Device->Slot;
    const USB_ENDPOINT_DESCRIPTOR *Desc = &Info->Descriptor;
    UCX_ENDPOINT_EVENT_CALLBACKS Callbacks;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDFCOMMONBUFFER RingBuffer = NULL;
    WDFQUEUE Queue = NULL, Waiting = NULL;
    UCXENDPOINT Endpoint = NULL;
    PXHCI_UCX_ENDPOINT_CLIENT Ep;
    UCHAR Number, Type;
    BOOLEAN In;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Controller);

    if (Info->Size < sizeof(*Info) || Slot == NULL || !Slot->Addressed)
        return STATUS_INVALID_DEVICE_STATE;
    Number = Desc->bEndpointAddress & USB_ENDPOINT_ADDRESS_MASK;
    Type = Desc->bmAttributes & USB_ENDPOINT_TYPE_MASK;
    In = USB_ENDPOINT_DIRECTION_IN(Desc->bEndpointAddress) != 0;
    if (Number == 0 || Type == USB_ENDPOINT_TYPE_CONTROL || (Desc->wMaxPacketSize & 0x07FF) == 0)
        return STATUS_INVALID_PARAMETER;
    if (Slot->Endpoints[2 * Number + (In ? 1 : 0)] != NULL)
        return STATUS_OBJECT_NAME_COLLISION;          /* the library disables before it re-adds */

    /* The ring: one page of its own, physically contiguous by construction */
    Status = WdfCommonBufferCreate(Dev->DmaEnabler, PAGE_SIZE, WDF_NO_OBJECT_ATTRIBUTES, &RingBuffer);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The queues: transfers arrive on Queue, wait on Waiting */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, XHCI_UCX_QUEUE_CLIENT);
    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoInternalDeviceControl = XhciUcxEvtEndpointIoctl;
    QueueConfig.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(Dev->Device, &QueueConfig, &Attributes, &Queue);
    if (NT_SUCCESS(Status))
    {
        WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);
        QueueConfig.PowerManaged = WdfFalse;
        Status = WdfIoQueueCreate(Dev->Device, &QueueConfig, &Attributes, &Waiting);
    }
    if (!NT_SUCCESS(Status))
        goto Fail;

    UCX_ENDPOINT_EVENT_CALLBACKS_INIT(&Callbacks);
    Callbacks.EvtEndpointAbort = XhciUcxEvtEndpointAbort;
    Callbacks.EvtEndpointResetHalted = XhciUcxEvtEndpointResetHalted;
    UcxEndpointInitSetEventCallbacks(EndpointInit, &Callbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, XHCI_UCX_ENDPOINT_CLIENT);
    Attributes.EvtCleanupCallback = XhciUcxEndpointEvtCleanup;
    Status = UcxEndpointCreate(UsbDevice, &EndpointInit, &Attributes, &Endpoint);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Ep = XhciUcxGetEndpointClient((WDFOBJECT)Endpoint);
    Ep->Dev = Dev;
    Ep->UsbDevice = UsbDevice;
    Ep->Self = Endpoint;
    Ep->Slot = Slot;
    Ep->Dci = (UCHAR)(2 * Number + (In ? 1 : 0));
    Ep->Address = Desc->bEndpointAddress;
    Ep->Type = Type;
    Ep->DataIn = In;
    Ep->MaxPacket = (USHORT)(Desc->wMaxPacketSize & 0x07FF);
    Ep->Interval = XhciUcxEndpointIntervalField(Slot->SpeedClass, Type, Desc->bInterval);
    if (Slot->SpeedClass == XHCI_PORTSC_SPEED_SUPER)
    {
        Ep->MaxBurst = Info->HasCompanion ? Info->Companion.bMaxBurst : 0;
        Ep->Mult = (Info->HasCompanion && Type == USB_ENDPOINT_TYPE_ISOCHRONOUS) ? Info->Companion.bmAttributes.Isochronous.Mult : 0;
        Ep->MaxEsit = Info->HasCompanion ? Info->Companion.wBytesPerInterval : (ULONG)Ep->MaxPacket * (Ep->MaxBurst + 1);
    }
    else
    {
        /* High-speed high-bandwidth interrupt/isoch: extra transactions per microframe in bits 11..12 */
        Ep->MaxBurst = (Slot->SpeedClass == XHCI_PORTSC_SPEED_HIGH && Type != USB_ENDPOINT_TYPE_BULK) ?
                       (UCHAR)((Desc->wMaxPacketSize >> 11) & 0x3) : 0;
        Ep->Mult = 0;
        Ep->MaxEsit = (ULONG)Ep->MaxPacket * (Ep->MaxBurst + 1);
    }
    Ep->MaximumTransferSize = Info->MaximumTransferSize ? Info->MaximumTransferSize : UCX_BULK_MAX_TRANSFER;
    Ep->RingBuffer = RingBuffer;
    Ep->Ring = WdfCommonBufferGetAlignedVirtualAddress(RingBuffer);
    Ep->RingPa = (ULONGLONG)WdfCommonBufferGetAlignedLogicalAddress(RingBuffer).QuadPart;
    Ep->Queue = Queue;
    Ep->Waiting = Waiting;
    InitializeListHead(&Ep->InFlight);
    XhciUcxGetQueueClient((WDFOBJECT)Queue)->Ep = Ep;
    XhciUcxGetQueueClient((WDFOBJECT)Waiting)->Ep = Ep;
    XhciUcxEpResetRing(Ep);
    UcxEndpointSetWdfIoQueue(Endpoint, Queue);

    XHCI_UCX_LOG("slot %u: endpoint 0x%02x added (type %u, DCI %u, MPS %u, burst %u, interval field %u, max transfer %lu)",
                 Slot->SlotId, Ep->Address, Type, Ep->Dci, Ep->MaxPacket, Ep->MaxBurst, Ep->Interval, Ep->MaximumTransferSize);
    return STATUS_SUCCESS;

Fail:
    if (Waiting != NULL)
        WdfObjectDelete((WDFOBJECT)Waiting);
    if (Queue != NULL)
        WdfObjectDelete((WDFOBJECT)Queue);
    if (RingBuffer != NULL)
        WdfObjectDelete((WDFOBJECT)RingBuffer);
    XHCI_UCX_LOG("slot %u: endpoint 0x%02x add failed 0x%08lX", Slot->SlotId, Desc->bEndpointAddress, (ULONG)Status);
    return Status;
}

/* EvtUsbDeviceEndpointsConfigure: one Configure Endpoint command for the whole change */
static
NTSTATUS
NTAPI
XhciUcxEvtUsbDeviceEndpointsConfigure(
    _In_ UCXCONTROLLER Controller,
    _In_ UCXUSBDEVICE UsbDevice,
    _In_ PUCX_ENDPOINTS_CONFIGURE Cfg)
{
    PXHCI_UCX_USBDEVICE_CLIENT Device = XhciUcxGetUsbDeviceClient((WDFOBJECT)UsbDevice);
    PXHCI_UCX_DEVICE Dev = Device->Dev;
    PXHCI_UCX_SLOT Slot = Device->Slot;
    PXHCI_INPUT_CONTROL_CONTEXT Icc;
    PXHCI_SLOT_CONTEXT InSlot, OutSlot;
    ULONG LastCtx = 1, Code = 0, Control, i, Dci;
    KIRQL OldIrql;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Controller);

    if (Cfg->Size < sizeof(*Cfg) || Slot == NULL || !Slot->Addressed)
        return STATUS_INVALID_DEVICE_STATE;
    if (!Dev->ControllerRunning || Dev->FatalError)
        return STATUS_DEVICE_NOT_READY;

    ExAcquireFastMutex(&Dev->TransferMutex);

    Icc = XhciUcxInputControlContext(Dev, Slot);
    InSlot = XhciUcxInputSlotContext(Dev, Slot);
    OutSlot = XhciUcxOutputSlotContext(Dev, Slot);
    RtlZeroMemory(Dev->HcVa + Slot->InputContextOffset, PAGE_SIZE);
    Control = XHCI_COMMAND_SLOT_FIELD(Slot->SlotId);

    if (Cfg->Type == UcxEndpointsConfigureDisableAll)
    {
        Control |= XHCI_TRB_DC;                          /* Deconfigure: every endpoint but EP0 */
    }
    else
    {
        /* Mirror the controller's slot context, then say which endpoints change */
        RtlCopyMemory(InSlot, OutSlot, Dev->ContextSize);
        Icc->AddContextFlags = XHCI_UCX_INPUT_CTX_ADD_SLOT;
        for (i = 0; i < Cfg->DisableCount; i++)
        {
            PXHCI_UCX_ENDPOINT_CLIENT Ep = XhciUcxGetEndpointClient((WDFOBJECT)Cfg->EndpointsToDisable[i]);

            if (Ep->Dci > 1)
                Icc->DropContextFlags |= XHCI_UCX_INPUT_CTX_ADD(Ep->Dci);
        }
        for (i = 0; i < Cfg->EnableCount; i++)
        {
            PXHCI_UCX_ENDPOINT_CLIENT Ep = XhciUcxGetEndpointClient((WDFOBJECT)Cfg->EndpointsToEnable[i]);
            ULONG AvgTrb;

            if (Ep->Dci <= 1 || Ep->Ring == NULL)
            {
                ExReleaseFastMutex(&Dev->TransferMutex);
                return STATUS_INVALID_PARAMETER;
            }
            KeAcquireSpinLock(&Dev->EndpointLock, &OldIrql);
            XhciUcxEpResetRing(Ep);
            KeReleaseSpinLock(&Dev->EndpointLock, OldIrql);
            AvgTrb = (Ep->Type == USB_ENDPOINT_TYPE_BULK) ? 3072 : (Ep->Type == USB_ENDPOINT_TYPE_INTERRUPT) ? Ep->MaxPacket : Ep->MaxEsit;
            XhciEndpointContextInit(XhciUcxInputEndpointContext(Dev, Slot, Ep->Dci),
                                    XhciUcxEndpointTypeField(Ep->Type, Ep->DataIn),
                                    Ep->MaxPacket, Ep->MaxBurst, Ep->Interval, Ep->Mult, Ep->MaxEsit, AvgTrb,
                                    Ep->RingPa | XHCI_TRB_CYCLE);
            Icc->AddContextFlags |= XHCI_UCX_INPUT_CTX_ADD(Ep->Dci);
            Icc->DropContextFlags &= ~XHCI_UCX_INPUT_CTX_ADD(Ep->Dci);
        }
        /* Context Entries: the highest DCI in use after the change */
        for (Dci = 2; Dci < 32; Dci++)
        {
            BOOLEAN Present = (Slot->Endpoints[Dci] != NULL && (Icc->DropContextFlags & XHCI_UCX_INPUT_CTX_ADD(Dci)) == 0) ||
                              (Icc->AddContextFlags & XHCI_UCX_INPUT_CTX_ADD(Dci)) != 0;

            if (Present)
                LastCtx = Dci;
        }
        XhciSlotContextSetLastCtx(InSlot, LastCtx);
    }
    KeMemoryBarrier();

    Status = XhciUcxSendCommand(Dev, XHCI_TRB_TYPE_CONFIG_EP,
                                (ULONGLONG)XhciUcxHcPhysical(Dev, Slot->InputContextOffset).QuadPart,
                                Control, &Code, NULL);
    XHCI_UCX_LOG("slot %u: Configure Endpoint (type %u, +%lu -%lu, context entries %lu) rc=0x%08lX code=%lu",
                 Slot->SlotId, Cfg->Type, Cfg->EnableCount, Cfg->DisableCount, LastCtx, (ULONG)Status, Code);

    KeAcquireSpinLock(&Dev->EndpointLock, &OldIrql);
    if (Cfg->Type == UcxEndpointsConfigureDisableAll)
    {
        for (Dci = 2; Dci < 32; Dci++)
        {
            if (Slot->Endpoints[Dci] != NULL)
            {
                Slot->Endpoints[Dci]->Configured = FALSE;
                Slot->Endpoints[Dci] = NULL;
            }
        }
    }
    else
    {
        for (i = 0; i < Cfg->DisableCount; i++)
        {
            PXHCI_UCX_ENDPOINT_CLIENT Ep = XhciUcxGetEndpointClient((WDFOBJECT)Cfg->EndpointsToDisable[i]);

            Ep->Configured = FALSE;
            if (Ep->Dci > 1 && Slot->Endpoints[Ep->Dci] == Ep)
                Slot->Endpoints[Ep->Dci] = NULL;
        }
        if (NT_SUCCESS(Status))
        {
            for (i = 0; i < Cfg->EnableCount; i++)
            {
                PXHCI_UCX_ENDPOINT_CLIENT Ep = XhciUcxGetEndpointClient((WDFOBJECT)Cfg->EndpointsToEnable[i]);

                Ep->Configured = TRUE;
                Ep->Halted = FALSE;
                Slot->Endpoints[Ep->Dci] = Ep;
            }
        }
    }
    KeReleaseSpinLock(&Dev->EndpointLock, OldIrql);
    ExReleaseFastMutex(&Dev->TransferMutex);
    return Status;
}

/* Every configured endpoint of a slot gives its transfers back (device disable); EndpointLock taken inside */
static
VOID
XhciUcxSlotDropEndpoints(
    _Inout_ PXHCI_UCX_DEVICE Dev,
    _Inout_ PXHCI_UCX_SLOT Slot,
    _In_ USBD_STATUS UsbdStatus)
{
    PXHCI_UCX_ENDPOINT_CLIENT Eps[32];
    LIST_ENTRY Gone[32];
    KIRQL OldIrql;
    ULONG Dci;

    KeAcquireSpinLock(&Dev->EndpointLock, &OldIrql);
    for (Dci = 0; Dci < 32; Dci++)
    {
        Eps[Dci] = Slot->Endpoints[Dci];
        InitializeListHead(&Gone[Dci]);
        if (Eps[Dci] == NULL)
            continue;
        Eps[Dci]->Configured = FALSE;
        Eps[Dci]->Disabled = TRUE;
        XhciUcxEpCollectAllLocked(Eps[Dci], UsbdStatus, &Gone[Dci]);
        Slot->Endpoints[Dci] = NULL;
    }
    KeReleaseSpinLock(&Dev->EndpointLock, OldIrql);
    for (Dci = 0; Dci < 32; Dci++)
    {
        if (Eps[Dci] != NULL)
            XhciUcxEpCompleteList(Eps[Dci], &Gone[Dci]);
    }
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

    /* An IMAN.IP-only interrupt (EINT already acknowledged by a racing path)
     * still means events may be queued: make the DPC drain in that case too. */
    if (Iman & XHCI_IMAN_IP)
        AckMask |= XHCI_USBSTS_EINT;

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
            /* M2 demo only: reconcile ports with device slots at PASSIVE_LEVEL.
             * Hub-driven, the hub driver reacts to the status change instead. */
            if (Dev->SelfEnumerate)
                XhciUcxRequestPortRescan(Dev);
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
     * teardown (WDF parent deletion) ends their life (contract 7.1). A
     * controller left without its root hub by an earlier failure is rebuilt. */
    if (Dev->Controller != NULL)
    {
        if (Dev->RootHub != NULL)
            return STATUS_SUCCESS;

        WdfObjectDelete((WDFOBJECT)Dev->Controller);
        Dev->Controller = NULL;
    }

    UCX_CONTROLLER_CONFIG_INIT(&ControllerConfig);
    ControllerConfig.NumberOfRootHubs = 1;
    ControllerConfig.EvtControllerReset = XhciUcxEvtControllerReset;
    ControllerConfig.EvtControllerQueryUsbCapability = XhciUcxEvtControllerQueryUsbCapability;

    /* The PCI identity names the root hub (USB\ROOT_HUB30&VID_&PID_&REV_) and
     * fills its device descriptor; absent (ACPI-enumerated controller) the
     * root hub still enumerates under the bare id. */
    {
        USHORT Ids[2] = { 0, 0 };
        UCHAR Revision = 0;

        if (XhciUcxPciRead(Dev, 0x00, Ids, sizeof(Ids)))
        {
            ControllerConfig.VendorId = Ids[0];
            ControllerConfig.DeviceId = Ids[1];
        }
        if (XhciUcxPciRead(Dev, 0x08, &Revision, sizeof(Revision)))
            ControllerConfig.RevisionId = Revision;
    }
    ControllerConfig.EvtControllerUsbDeviceAdd = XhciUcxEvtControllerUsbDeviceAdd;

    {
        WDF_OBJECT_ATTRIBUTES ControllerAttributes;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&ControllerAttributes, XHCI_UCX_CONTROLLER_CLIENT);
        Status = UcxControllerCreate(Dev->Device, &ControllerConfig, &ControllerAttributes, &Dev->Controller);
    }
    XHCI_UCX_LOG("UcxControllerCreate rc=0x%08lX", (ULONG)Status);
    if (!NT_SUCCESS(Status))
    {
        Dev->Controller = NULL;
        return Status;
    }
    XhciUcxGetControllerClient((WDFOBJECT)Dev->Controller)->Dev = Dev;
    XHCI_UCX_TRACE("controller object %p", Dev->Controller);

    UCX_ROOTHUB_CONFIG_INIT(&RootHubConfig);
    RootHubConfig.NumberOf20Ports = Dev->NumberOf20Ports;
    RootHubConfig.NumberOf30Ports = Dev->NumberOf30Ports;
    RootHubConfig.EvtRootHubControlUrb = XhciUcxEvtRootHubControlUrb;
    RootHubConfig.EvtRootHubGetInfo = XhciUcxEvtRootHubGetInfo;
    RootHubConfig.EvtRootHubGetPortChangeBitmap = XhciUcxEvtRootHubGetPortChangeBitmap;

    {
        WDF_OBJECT_ATTRIBUTES RootHubAttributes;

        /* Our context on the UCX object: the callbacks' way back to Dev */
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&RootHubAttributes, XHCI_UCX_ROOTHUB_CLIENT);
        Status = UcxRootHubCreate(Dev->Controller, &RootHubConfig, &RootHubAttributes, &Dev->RootHub);
    }
    if (NT_SUCCESS(Status))
        XhciUcxGetRootHubClient((WDFOBJECT)Dev->RootHub)->Dev = Dev;
    XHCI_UCX_LOG("UcxRootHubCreate rc=0x%08lX ports USB2=%u USB3=%u",
                 (ULONG)Status, Dev->NumberOf20Ports, Dev->NumberOf30Ports);
    if (!NT_SUCCESS(Status))
    {
        /* Never leave a controller without its root hub behind */
        Dev->RootHub = NULL;
        WdfObjectDelete((WDFOBJECT)Dev->Controller);
        Dev->Controller = NULL;
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

    InterlockedExchange(&Dev->EnumStopped, 0);
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
        if (Dev->SelfEnumerate)
            XhciUcxRequestPortRescan(Dev);
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

    /* Stop enumeration first (no further enqueue, a running pass exits at its next
     * check), let the in-flight pass finish (bounded waits), then release the slots.
     * The interrupter stays armed meanwhile; a port change now only sets PortRescanNeeded. */
    KeAcquireSpinLock(&Dev->EventRingLock, &OldIrql);
    InterlockedExchange(&Dev->EnumStopped, 1);
    KeReleaseSpinLock(&Dev->EventRingLock, OldIrql);
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
    KIRQL OldIrql;

    /* MMIO may already be gone: stop touching the controller from the DPC.
     * Taken under the ring lock so a drain in progress sees it on its next check. */
    KeAcquireSpinLock(&Dev->EventRingLock, &OldIrql);
    Dev->FatalError = TRUE;
    Dev->ControllerRunning = FALSE;
    InterlockedExchange(&Dev->EnumStopped, 1);
    KeReleaseSpinLock(&Dev->EventRingLock, OldIrql);
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
    Dev->SelfEnumerate = (g_XhciUcxSelfEnumerate != 0);
    ExInitializeFastMutex(&Dev->TransferMutex);
    KeInitializeSpinLock(&Dev->EventRingLock);
    KeInitializeSpinLock(&Dev->EndpointLock);
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
