/*
 * PROJECT:     LiberNT NetIO driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Private management interface between fwpuclnt.dll and the WFP filter engine
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#ifndef _WFPIOCTL_H_
#define _WFPIOCTL_H_

#define WFP_ENGINE_DEVICE_NAME     L"\\Device\\WfpEngine"
#define WFP_ENGINE_SYMLINK_NAME    L"\\DosDevices\\WfpEngine"
#define WFP_ENGINE_WIN32_NAME      L"\\\\.\\WfpEngine"

#define WFP_IOCTL(Function) CTL_CODE(FILE_DEVICE_NETWORK, 0x900 + (Function), METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_WFP_OPEN                WFP_IOCTL(0)
#define IOCTL_WFP_TRANSACTION_BEGIN   WFP_IOCTL(1)
#define IOCTL_WFP_TRANSACTION_COMMIT  WFP_IOCTL(2)
#define IOCTL_WFP_TRANSACTION_ABORT   WFP_IOCTL(3)
#define IOCTL_WFP_PROVIDER_ADD        WFP_IOCTL(4)
#define IOCTL_WFP_PROVIDER_DELETE     WFP_IOCTL(5)
#define IOCTL_WFP_SUBLAYER_ADD        WFP_IOCTL(6)
#define IOCTL_WFP_SUBLAYER_DELETE     WFP_IOCTL(7)
#define IOCTL_WFP_CALLOUT_ADD         WFP_IOCTL(8)
#define IOCTL_WFP_CALLOUT_DELETE_KEY  WFP_IOCTL(9)
#define IOCTL_WFP_CALLOUT_DELETE_ID   WFP_IOCTL(10)
#define IOCTL_WFP_FILTER_ADD          WFP_IOCTL(11)
#define IOCTL_WFP_FILTER_DELETE_KEY   WFP_IOCTL(12)
#define IOCTL_WFP_FILTER_DELETE_ID    WFP_IOCTL(13)

typedef struct _WFP_IOCTL_OPEN
{
    ULONG Flags;
    ULONG TransactionTimeout;
} WFP_IOCTL_OPEN, *PWFP_IOCTL_OPEN;

typedef struct _WFP_IOCTL_OBJECT
{
    GUID Key;
    GUID ProviderKey;
    GUID LayerKey;
    ULONG Flags;
    USHORT Weight;
    BOOLEAN HasProvider;
} WFP_IOCTL_OBJECT, *PWFP_IOCTL_OBJECT;

typedef struct _WFP_IOCTL_VALUE
{
    ULONG Type;
    ULONG Size;
    ULONG Offset;
    ULONG64 Number;
} WFP_IOCTL_VALUE, *PWFP_IOCTL_VALUE;

typedef struct _WFP_IOCTL_CONDITION
{
    GUID FieldKey;
    ULONG MatchType;
    WFP_IOCTL_VALUE Value;
    WFP_IOCTL_VALUE High;
} WFP_IOCTL_CONDITION, *PWFP_IOCTL_CONDITION;

typedef struct _WFP_IOCTL_FILTER
{
    GUID FilterKey;
    GUID ProviderKey;
    GUID LayerKey;
    GUID SubLayerKey;
    GUID ActionKey;
    ULONG Flags;
    ULONG ActionType;
    ULONG WeightType;
    ULONG ConditionCount;
    ULONG64 Weight;
    ULONG64 RawContext;
    BOOLEAN HasProvider;
    WFP_IOCTL_CONDITION Conditions[ANYSIZE_ARRAY];
} WFP_IOCTL_FILTER, *PWFP_IOCTL_FILTER;


/* ---------------- M3 stage 0: enforcement mode + event ring ----------------
 * These are additive; filters/callouts/sessions above are unchanged. The enums and
 * structs are plain C so fwpuclnt.dll and the wfpmode.exe lab helper share them. */

typedef enum _WFP_ENFORCEMENT_MODE
{
    WfpModeOff = 0,  /* shim forces permit; classify still runs for counters/events */
    WfpModeOn  = 1   /* verdicts enforced */
} WFP_ENFORCEMENT_MODE;

#define IOCTL_WFP_SET_MODE    WFP_IOCTL(14)  /* in: WFP_IOCTL_SET_MODE; LocalSystem only, audited */
#define IOCTL_WFP_EVENT_ENUM  WFP_IOCTL(15)  /* out: WFP_IOCTL_EVENT_HEADER + WFP_IOCTL_EVENT[] */

typedef struct _WFP_IOCTL_SET_MODE
{
    ULONG Mode;   /* WFP_ENFORCEMENT_MODE */
} WFP_IOCTL_SET_MODE, *PWFP_IOCTL_SET_MODE;

typedef enum _WFP_EVENT_TYPE
{
    WfpEventClassifyPermit = 0,  /* a filter permitted (deciding filterId) */
    WfpEventClassifyBlock  = 1,  /* a filter blocked; enforced iff mode On */
    WfpEventClassifyError  = 2,  /* classification FAILED (pool/flow) - not a no-match */
    WfpEventWouldBlock     = 3,  /* mode Off let a block verdict through (A/B visibility) */
    WfpEventModeChange     = 4   /* audit: set-mode attempt (Status = success/denied) */
} WFP_EVENT_TYPE;

#define WFP_EVENT_IMAGE_MAX 64

typedef struct _WFP_IOCTL_EVENT
{
    ULONG64 Sequence;      /* monotonic; a gap means events were dropped */
    ULONG64 Timestamp;     /* system time, 100 ns units */
    ULONG   Type;          /* WFP_EVENT_TYPE */
    ULONG   Mode;          /* enforcement mode in force when recorded */
    ULONG   LayerId;
    ULONG   ProcessId;
    ULONG64 FilterId;      /* deciding filter, or 0 */
    LONG    Status;        /* NTSTATUS for error / mode-change events, else 0 */
    ULONG   LocalAddress;  /* IPv4, host order; 0 for non-classify events */
    ULONG   RemoteAddress;
    USHORT  LocalPort;
    USHORT  RemotePort;
    UCHAR   Protocol;
    UCHAR   Direction;     /* 0 inbound, 1 outbound */
    USHORT  Reserved;
    WCHAR   Image[WFP_EVENT_IMAGE_MAX];  /* image basename for a mode-change audit */
} WFP_IOCTL_EVENT, *PWFP_IOCTL_EVENT;

typedef struct _WFP_IOCTL_EVENT_HEADER
{
    ULONG64 Dropped;       /* events overwritten before they were read */
    ULONG64 NextSequence;  /* sequence of the next event to be produced */
    ULONG   Count;         /* WFP_IOCTL_EVENT records that follow in this buffer */
    ULONG   Reserved;
} WFP_IOCTL_EVENT_HEADER, *PWFP_IOCTL_EVENT_HEADER;

#define IOCTL_WFP_FILTER_GET_STATS  WFP_IOCTL(16)  /* in: UINT64 filterId; out: WFP_IOCTL_FILTER_STATS */
#define IOCTL_WFP_GET_MODE          WFP_IOCTL(17)  /* out: ULONG current WFP_ENFORCEMENT_MODE */

typedef struct _WFP_IOCTL_FILTER_STATS
{
    ULONG64 FilterId;
    ULONG64 Hits;       /* classify hits accumulated since the filter was added */
    ULONG   LayerId;
    ULONG   Reserved;
} WFP_IOCTL_FILTER_STATS, *PWFP_IOCTL_FILTER_STATS;

#endif
