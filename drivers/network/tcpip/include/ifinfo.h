/*
 * PROJECT:     LiberNT TCP/IP protocol driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Interface facts shared by the TDI information arm and the network store producer
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * This header uses only NT base types so that tcpip/nsiprov.c, which is
 * built outside the driver's precompiled header, can include it.
 */

#pragma once

#define IFINFO_MAX_DESCRIPTION 128
#define IFINFO_MAX_PHYSADDR 8

typedef struct _SEND_RECV_STATS {
    ULONG64 InBytes;
    ULONG64 InUnicast;
    ULONG64 InNUnicast;
    ULONG64 InDiscarded;
    ULONG64 InErrors;
    ULONG64 InDiscardedUnknownProto;
    ULONG64 OutBytes;
    ULONG64 OutUnicast;
    ULONG64 OutNUnicast;
    ULONG64 OutDiscarded;
    ULONG64 OutErrors;
} SEND_RECV_STATS, *PSEND_RECV_STATS;

typedef struct _IF_INFO_ROW {
    UINT Index;
    UINT Type;
    UINT Mtu;
    UINT Speed;
    UINT AdminStatus;
    ULONG OperStatus;
    UINT PhysAddrLength;
    UCHAR PhysAddr[IFINFO_MAX_PHYSADDR];
    SEND_RECV_STATS Stats;
    ULONG InErrors;
    ULONG OutErrors;
    ULONG PhysicalMedium;
    GUID Guid;
    UINT DescriptionLength;
    CHAR Description[IFINFO_MAX_DESCRIPTION + 1];
} IF_INFO_ROW, *PIF_INFO_ROW;

struct _IP_INTERFACE;

VOID IfInfoFillRow(struct _IP_INTERFACE *Interface, PIF_INFO_ROW Row);
NTSTATUS IfInfoSnapshot(ULONG Capacity, PIF_INFO_ROW Rows, PULONG Count);
NTSTATUS IfInfoSnapshotByIndex(UINT Index, PIF_INFO_ROW Row);

#define TI_NSI_CHANGE_ADD 1
#define TI_NSI_CHANGE_DELETE 2
#define TI_NSI_CHANGE_PARAMETER 3

VOID TiNsiRegister(VOID);
VOID TiNsiDeregister(VOID);
VOID TiNsiNotifyInterface(UINT Index, ULONG Change);
