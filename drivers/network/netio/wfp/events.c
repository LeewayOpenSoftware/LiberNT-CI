/*
 * PROJECT:     LiberNT NetIO driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     WFP event ring (M3 stage 0). A bounded, drop-oldest ring of classify
 *              and mode-change (audit) records, read through IOCTL_WFP_EVENT_ENUM. It is produced in both enforcement
 *              modes so an Off/On A/B runs on one image. An event-ring failure never
 *              fails a commit: emit is best-effort (audit availability < admin path).
 *              Classify events may be stored at DISPATCH; the mode-change audit (which
 *              resolves the caller image) is PASSIVE only.
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "wfp.h"
#include <drivers/wfp/wfpioctl.h>

#define WFP_EVENT_RING_CAPACITY 512

static KSPIN_LOCK WfpEventLock;
static PWFP_IOCTL_EVENT WfpEventRing = NULL;
static ULONG WfpEventHead = 0;     /* index of the oldest valid record */
static ULONG WfpEventCount = 0;    /* number of valid records */
static ULONG64 WfpEventSequence = 1;
static ULONG64 WfpEventDropped = 0;

VOID
WfpInitializeEvents(VOID)
{
    KeInitializeSpinLock(&WfpEventLock);
    if (WfpEventRing == NULL)
    {
        WfpEventRing = ExAllocatePoolWithTag(NonPagedPoolNx,
                                             WFP_EVENT_RING_CAPACITY * sizeof(WFP_IOCTL_EVENT),
                                             WFP_TAG);
        /* On failure the ring stays NULL and every emit becomes a no-op. */
    }
}

/* Stamp sequence + timestamp and append, dropping the oldest when full. Safe at
 * DISPATCH: no allocation, spin lock only. Caller fills Type and the payload. */
static
VOID
WfpStoreEvent(
    _In_ PWFP_IOCTL_EVENT Event)
{
    KIRQL OldIrql;
    ULONG Slot;
    LARGE_INTEGER Now;

    if (WfpEventRing == NULL)
    {
        return;
    }

    KeQuerySystemTime(&Now);
    KeAcquireSpinLock(&WfpEventLock, &OldIrql);

    Event->Sequence = WfpEventSequence++;
    Event->Timestamp = (ULONG64)Now.QuadPart;

    if (WfpEventCount < WFP_EVENT_RING_CAPACITY)
    {
        Slot = (WfpEventHead + WfpEventCount) % WFP_EVENT_RING_CAPACITY;
        WfpEventCount++;
    }
    else
    {
        Slot = WfpEventHead;
        WfpEventHead = (WfpEventHead + 1) % WFP_EVENT_RING_CAPACITY;
        WfpEventDropped++;
    }
    WfpEventRing[Slot] = *Event;

    KeReleaseSpinLock(&WfpEventLock, OldIrql);
}

/* PASSIVE only (resolves the caller image). Records a set-mode attempt: Status tells
 * success from access-denied, and OldMode == NewMode marks a refused or no-op change. */
VOID
WfpEmitModeEvent(
    _In_ ULONG OldMode,
    _In_ ULONG NewMode,
    _In_ HANDLE ProcessId,
    _In_ NTSTATUS Result)
{
    WFP_IOCTL_EVENT Event;
    PUNICODE_STRING ImageName = NULL;

    RtlZeroMemory(&Event, sizeof(Event));
    Event.Type = WfpEventModeChange;
    Event.Mode = NewMode;
    Event.LayerId = (ULONG)-1;
    Event.ProcessId = HandleToUlong(ProcessId);
    Event.Status = Result;
    Event.FilterId = (ULONG64)OldMode;   /* previous mode, for the audit trail */

    if (NT_SUCCESS(SeLocateProcessImageName(PsGetCurrentProcess(), &ImageName)) &&
        ImageName != NULL)
    {
        USHORT Chars = ImageName->Length / sizeof(WCHAR);
        PWCHAR Base = ImageName->Buffer;
        USHORT i;
        for (i = 0; i < Chars; i++)
        {
            if (ImageName->Buffer[i] == L'\\')
            {
                Base = &ImageName->Buffer[i + 1];
            }
        }
        Chars = (USHORT)(ImageName->Buffer + Chars - Base);
        if (Chars > WFP_EVENT_IMAGE_MAX - 1)
        {
            Chars = WFP_EVENT_IMAGE_MAX - 1;
        }
        RtlCopyMemory(Event.Image, Base, Chars * sizeof(WCHAR));
        Event.Image[Chars] = L'\0';
        ExFreePool(ImageName);
    }

    WfpStoreEvent(&Event);
}

/* Callable at DISPATCH from the classify path. Classification ERRORS (Type
 * WfpEventClassifyError) are kept distinct from a no-match, which produces no event. */
VOID
WfpEmitClassifyEvent(
    _In_ ULONG Type,
    _In_ UINT16 LayerId,
    _In_ UINT64 FilterId,
    _In_ ULONG LocalAddress,
    _In_ ULONG RemoteAddress,
    _In_ USHORT LocalPort,
    _In_ USHORT RemotePort,
    _In_ UCHAR Protocol,
    _In_ UCHAR Direction)
{
    WFP_IOCTL_EVENT Event;

    RtlZeroMemory(&Event, sizeof(Event));
    Event.Type = Type;
    Event.Mode = WfpCurrentMode();
    Event.LayerId = LayerId;
    Event.ProcessId = HandleToUlong(PsGetCurrentProcessId());
    Event.FilterId = FilterId;
    Event.LocalAddress = LocalAddress;
    Event.RemoteAddress = RemoteAddress;
    Event.LocalPort = LocalPort;
    Event.RemotePort = RemotePort;
    Event.Protocol = Protocol;
    Event.Direction = Direction;

    WfpStoreEvent(&Event);
}

/* IOCTL_WFP_EVENT_ENUM: snapshot of the ring, oldest first, with a header carrying
 * the dropped count and the next sequence so a reader can detect gaps and new events.
 * Read access is gated by the device ACL (SYSTEM + Administrators). */
NTSTATUS
WfpEnumEvents(
    _Out_writes_bytes_(OutputLength) PVOID Buffer,
    _In_ ULONG OutputLength,
    _Out_ PULONG_PTR Information)
{
    PWFP_IOCTL_EVENT_HEADER Header = Buffer;
    PWFP_IOCTL_EVENT Out;
    KIRQL OldIrql;
    ULONG Capacity, Produced, i, Slot;

    *Information = 0;
    if (OutputLength < sizeof(WFP_IOCTL_EVENT_HEADER))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    Capacity = (OutputLength - sizeof(WFP_IOCTL_EVENT_HEADER)) / sizeof(WFP_IOCTL_EVENT);
    Out = (PWFP_IOCTL_EVENT)(Header + 1);

    KeAcquireSpinLock(&WfpEventLock, &OldIrql);

    Produced = WfpEventCount;
    if (Produced > Capacity)
    {
        Produced = Capacity;   /* truncate to what fits; reader re-polls by sequence */
    }
    for (i = 0; i < Produced; i++)
    {
        Slot = (WfpEventHead + i) % WFP_EVENT_RING_CAPACITY;
        Out[i] = WfpEventRing[Slot];
    }
    Header->Dropped = WfpEventDropped;
    Header->NextSequence = WfpEventSequence;
    Header->Count = Produced;
    Header->Reserved = 0;

    KeReleaseSpinLock(&WfpEventLock, OldIrql);

    *Information = sizeof(WFP_IOCTL_EVENT_HEADER) + (ULONG_PTR)Produced * sizeof(WFP_IOCTL_EVENT);
    return STATUS_SUCCESS;
}
