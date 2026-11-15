/*
 * PROJECT:     LiberNT TCP/IP protocol driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Snapshots of the interface, address, route and neighbour state for the network store producer
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#include <ipifcons.h>
#include <nldef.h>
#include <tcpip_undoc.h>

#define SNAPSHOT_TAG 'pSiT'
#define IFINFO_IP6_INITIAL_LENGTH (sizeof(IP6_TABLE_HEADER) + 4096)
#define IFINFO_IP6_ATTEMPTS 4

static PIP_INTERFACE IfInfoReferenceByIndex(UINT Index)
{
    PIP_INTERFACE Found = NULL;
    KIRQL OldIrql;
    IF_LIST_ITER(Interface);

    TcpipAcquireSpinLock(&InterfaceListLock, &OldIrql);
    ForEachInterface(Interface) {
        if (Interface->Index == Index && !Found && IPReferenceInterface(Interface))
            Found = Interface;
    } EndFor(Interface);
    TcpipReleaseSpinLock(&InterfaceListLock, OldIrql);
    return Found;
}

VOID IfInfoFree(PVOID Buffer)
{
    if (Buffer)
        ExFreePoolWithTag(Buffer, SNAPSHOT_TAG);
}

NTSTATUS IfInfoInterfaceKeys(PIF_INFO_KEY Keys, ULONG Capacity, PULONG Count)
{
    PIF_INFO_ROW Rows = NULL;
    ULONG Total = 0, i;
    NTSTATUS Status;

    PAGED_CODE();

    if (Capacity)
    {
        Rows = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(IF_INFO_ROW), SNAPSHOT_TAG);
        if (!Rows)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = IfInfoSnapshot(Capacity, Rows, &Total);
    if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
    {
        for (i = 0; i < Total && i < Capacity; i++)
        {
            Keys[i].Index = Rows[i].Index;
            Keys[i].Type = Rows[i].Type;
            Keys[i].OperStatus = Rows[i].OperStatus;
            Keys[i].Mtu = Rows[i].Mtu;
            Keys[i].Loopback = Rows[i].Type == MIB_IF_TYPE_LOOPBACK;
        }
    }
    IfInfoFree(Rows);
    *Count = Total;
    return Status;
}

NTSTATUS IfInfoIp4Interface(UINT Index, PIP4_INTERFACE_INFO Info)
{
    PAGED_CODE();

    RtlZeroMemory(Info, sizeof(*Info));
    Info->Index = Index;
    return IPQueryInterfaceV4(Info);
}

NTSTATUS IfInfoIp4Addresses(UINT Index, PIP4_ADDRESS_ROW Rows, ULONG Capacity, PULONG Count)
{
    PIP_INTERFACE Interface;
    PIPADDR_ENTRY Entries = NULL;
    ULONG Allocated = 0, Total, Copied, i, Attempt;

    PAGED_CODE();

    *Count = 0;
    Interface = IfInfoReferenceByIndex(Index);
    if (!Interface)
        return STATUS_NOT_FOUND;

    Total = CopyInterfaceAddresses(Interface, NULL, 0);
    for (Attempt = 0; Total > Allocated && Attempt < IFINFO_IP6_ATTEMPTS; Attempt++)
    {
        IfInfoFree(Entries);
        Allocated = Total;
        Entries = ExAllocatePoolWithTag(NonPagedPool, Allocated * sizeof(IPADDR_ENTRY), SNAPSHOT_TAG);
        if (!Entries)
        {
            IPDereferenceInterface(Interface);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Total = CopyInterfaceAddresses(Interface, Entries, Allocated);
    }
    Copied = Total < Allocated ? Total : Allocated;

    for (i = 0; i < Copied && i < Capacity; i++)
    {
        IP4_ADDRESS_INFO Info;

        RtlZeroMemory(&Rows[i], sizeof(Rows[i]));
        Rows[i].Address = Entries[i].Addr;
        Rows[i].Mask = Entries[i].Mask;
        Rows[i].Broadcast = Entries[i].BcastAddr;
        RtlZeroMemory(&Info, sizeof(Info));
        Info.Index = Index;
        Info.Address = Entries[i].Addr;
        if (NT_SUCCESS(IPQueryAddressV4(&Info)))
        {
            Rows[i].DadState = Info.DadState;
            Rows[i].CreationTime = Info.CreationTime;
        }
        else
        {
            Rows[i].DadState = IFINFO_DAD_PREFERRED;
        }
    }

    IPDereferenceInterface(Interface);
    IfInfoFree(Entries);
    *Count = Copied;
    return Copied > Capacity ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

NTSTATUS IfInfoIp4Routes(UINT Index, PIP4_ROUTE_ROW Rows, ULONG Capacity, PULONG Count)
{
    PIP_INTERFACE Interface;
    PFIB_ENTRY Fibs = NULL;
    ULONGLONG Now = KeQueryInterruptTime();
    ULONG Allocated = 0, Total, Copied, i, Attempt;

    PAGED_CODE();

    *Count = 0;
    Interface = IfInfoReferenceByIndex(Index);
    if (!Interface)
        return STATUS_NOT_FOUND;

    Total = CopyFIBs(Interface, NULL, 0);
    for (Attempt = 0; Total > Allocated && Attempt < IFINFO_IP6_ATTEMPTS; Attempt++)
    {
        IfInfoFree(Fibs);
        Allocated = Total;
        Fibs = ExAllocatePoolWithTag(NonPagedPool, Allocated * sizeof(FIB_ENTRY), SNAPSHOT_TAG);
        if (!Fibs)
        {
            IPDereferenceInterface(Interface);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Total = CopyFIBs(Interface, Fibs, Allocated);
        if (Total > Allocated)
        {
            for (i = 0; i < Allocated; i++)
                NBDereferenceNeighbor(Fibs[i].Router);
        }
    }
    Copied = Total < Allocated ? Total : Allocated;

    for (i = 0; i < Copied; i++)
    {
        if (i < Capacity)
        {
            ULONG Elapsed = (ULONG)((Now - Fibs[i].Updated) / 10000000);

            RtlZeroMemory(&Rows[i], sizeof(Rows[i]));
            Rows[i].Destination = Fibs[i].NetworkAddress.Address.IPv4Address;
            Rows[i].Mask = Fibs[i].Netmask.Address.IPv4Address;
            Rows[i].Gateway = Fibs[i].Router->Address.Address.IPv4Address;
            Rows[i].Metric = Fibs[i].Metric;
            Rows[i].Origin = Fibs[i].Origin;
            Rows[i].Age = Elapsed;
            if (Fibs[i].Origin == NlroRouterAdvertisement)
                Rows[i].ValidLifetime = Fibs[i].Lifetime > Elapsed ? Fibs[i].Lifetime - Elapsed : 0;
            else
                Rows[i].ValidLifetime = Fibs[i].Lifetime ? Fibs[i].Lifetime : MAXULONG;
        }
        NBDereferenceNeighbor(Fibs[i].Router);
    }

    IPDereferenceInterface(Interface);
    IfInfoFree(Fibs);
    *Count = Copied;
    return Copied > Capacity ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

NTSTATUS IfInfoIp4Neighbors(UINT Index, PIP4_NEIGHBOR_ROW Rows, ULONG Capacity, PULONG Count)
{
    PIP_INTERFACE Interface;
    PIPARP_ENTRY Entries = NULL;
    ULONG Allocated = 0, Total, Copied, i, Attempt;

    PAGED_CODE();

    *Count = 0;
    Interface = IfInfoReferenceByIndex(Index);
    if (!Interface)
        return STATUS_NOT_FOUND;

    Total = NBCopyNeighbors(Interface, NULL, 0);
    for (Attempt = 0; Total > Allocated && Attempt < IFINFO_IP6_ATTEMPTS; Attempt++)
    {
        IfInfoFree(Entries);
        Allocated = Total;
        Entries = ExAllocatePoolWithTag(NonPagedPool, Allocated * sizeof(IPARP_ENTRY), SNAPSHOT_TAG);
        if (!Entries)
        {
            IPDereferenceInterface(Interface);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(Entries, Allocated * sizeof(IPARP_ENTRY));
        Total = NBCopyNeighbors(Interface, Entries, Allocated);
    }
    Copied = Total < Allocated ? Total : Allocated;

    for (i = 0; i < Copied && i < Capacity; i++)
    {
        RtlZeroMemory(&Rows[i], sizeof(Rows[i]));
        Rows[i].Address = Entries[i].LogAddr;
        Rows[i].PhysAddrLength = min(Entries[i].AddrSize, IFINFO_MAX_PHYSADDR);
        RtlCopyMemory(Rows[i].PhysAddr, Entries[i].PhysAddr, Rows[i].PhysAddrLength);
        Rows[i].Type = Entries[i].Type;
    }

    IPDereferenceInterface(Interface);
    IfInfoFree(Entries);
    *Count = Copied;
    return Copied > Capacity ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

NTSTATUS IfInfoIp6Query(ULONG TableId, PVOID *Buffer, PULONG Count)
{
    ULONG Length = IFINFO_IP6_INITIAL_LENGTH, Attempt, Information;
    PIP6_TABLE_HEADER Header;
    NTSTATUS Status;

    PAGED_CODE();

    *Buffer = NULL;
    *Count = 0;
    for (Attempt = 0; Attempt < IFINFO_IP6_ATTEMPTS; Attempt++)
    {
        Header = ExAllocatePoolWithTag(PagedPool, Length, SNAPSHOT_TAG);
        if (!Header)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(Header, Length);
        Information = 0;
        Status = InfoIp6QueryTable(TableId, Header, Length, &Information);
        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
        {
            IfInfoFree(Header);
            return Status;
        }
        if (Status == STATUS_SUCCESS && Header->Needed <= Length)
        {
            *Buffer = Header;
            *Count = Header->Count;
            return STATUS_SUCCESS;
        }
        Length = Header->Needed + 1024;
        IfInfoFree(Header);
    }
    return STATUS_BUFFER_OVERFLOW;
}

static BOOLEAN IfInfoEndpointWanted(PADDRESS_FILE AddrFile, ULONG Family, ULONG Protocol)
{
    if (AddrFile->Protocol != IPPROTO_TCP && AddrFile->Protocol != IPPROTO_UDP)
        return FALSE;
    if (Protocol != IFINFO_PROTOCOL_ANY && AddrFile->Protocol != Protocol)
        return FALSE;
    if (Family == TI_NSI_FAMILY_IPV4)
        return AddrFile->Address.Type == IP_ADDRESS_V4;
    if (Family == TI_NSI_FAMILY_IPV6)
        return AddrFile->Address.Type == IP_ADDRESS_V6;
    return AddrFile->Address.Type == IP_ADDRESS_V4 || AddrFile->Address.Type == IP_ADDRESS_V6;
}

static BOOLEAN IfInfoIsLinkLocal(const UCHAR *Address)
{
    return Address[0] == 0xfe && (Address[1] & 0xc0) == 0x80;
}

static ULONG IfInfoLinkLocalScope(const UCHAR *Address, ULONG ScopeId)
{
    return IfInfoIsLinkLocal(Address) ? ScopeId : 0;
}

static VOID IfInfoFillAddressRow(PADDRESS_FILE AddrFile, PENDPOINT_ROW Row)
{
    BOOLEAN IsV6 = AddrFile->Address.Type == IP_ADDRESS_V6;

    RtlZeroMemory(Row, sizeof(*Row));
    Row->Family = IsV6 ? TI_NSI_FAMILY_IPV6 : TI_NSI_FAMILY_IPV4;
    Row->Protocol = AddrFile->Protocol;
    Row->LocalPort = AddrFile->Port;
    Row->Pid = HandleToUlong(AddrFile->ProcessId);
    Row->CreateTime = AddrFile->CreationTime;
    Row->ModuleInfo = (ULONG_PTR)AddrFile->SubProcessTag;
    if (IsV6)
    {
        RtlCopyMemory(Row->LocalAddress, AddrFile->Address.Address.IPv6Address, 16);
        Row->LocalScopeId = IfInfoLinkLocalScope(Row->LocalAddress, AddrFile->Address.ScopeId);
    }
    else
        RtlCopyMemory(Row->LocalAddress, &AddrFile->Address.Address.IPv4Address, 4);
}

static BOOLEAN IfInfoFillConnectionRow(PADDRESS_FILE AddrFile, PCONNECTION_ENDPOINT Connection, PENDPOINT_ROW Row)
{
    BOOLEAN IsV6 = AddrFile->Address.Type == IP_ADDRESS_V6;
    CHAR Local[sizeof(TA_IP6_ADDRESS)], Remote[sizeof(TA_IP6_ADDRESS)];

    if (!NT_SUCCESS(TCPGetSockAddress(Connection, (PTRANSPORT_ADDRESS)Local, sizeof(Local), FALSE)) ||
        !NT_SUCCESS(TCPGetSockAddress(Connection, (PTRANSPORT_ADDRESS)Remote, sizeof(Remote), TRUE)) ||
        !NT_SUCCESS(TCPGetSocketStatus(Connection, &Row->State)))
        return FALSE;
    if (IsV6)
    {
        if (((PTA_IP6_ADDRESS)Local)->Address[0].AddressType != TDI_ADDRESS_TYPE_IP6)
            return FALSE;
        RtlCopyMemory(Row->LocalAddress, ((PTA_IP6_ADDRESS)Local)->Address[0].Address[0].sin6_addr, 16);
        Row->LocalPort = ((PTA_IP6_ADDRESS)Local)->Address[0].Address[0].sin6_port;
        Row->LocalScopeId = IfInfoLinkLocalScope(Row->LocalAddress, ((PTA_IP6_ADDRESS)Local)->Address[0].Address[0].sin6_scope_id);
        RtlCopyMemory(Row->RemoteAddress, ((PTA_IP6_ADDRESS)Remote)->Address[0].Address[0].sin6_addr, 16);
        Row->RemotePort = ((PTA_IP6_ADDRESS)Remote)->Address[0].Address[0].sin6_port;
        Row->RemoteScopeId = IfInfoLinkLocalScope(Row->RemoteAddress, ((PTA_IP6_ADDRESS)Remote)->Address[0].Address[0].sin6_scope_id);
    }
    else
    {
        if (((PTA_IP_ADDRESS)Local)->Address[0].AddressLength != TDI_ADDRESS_LENGTH_IP)
            return FALSE;
        RtlCopyMemory(Row->LocalAddress, &((PTA_IP_ADDRESS)Local)->Address[0].Address[0].in_addr, 4);
        Row->LocalPort = ((PTA_IP_ADDRESS)Local)->Address[0].Address[0].sin_port;
        RtlCopyMemory(Row->RemoteAddress, &((PTA_IP_ADDRESS)Remote)->Address[0].Address[0].in_addr, 4);
        Row->RemotePort = ((PTA_IP_ADDRESS)Remote)->Address[0].Address[0].sin_port;
    }
    return TRUE;
}

static VOID IfInfoEmitRow(const ENDPOINT_ROW *Row, PENDPOINT_ROW Rows, ULONG Capacity, PULONG Emitted)
{
    if (*Emitted < Capacity)
        Rows[*Emitted] = *Row;
    (*Emitted)++;
}

/* One address file can stand for several TCP endpoints: its listener, and
 * every connection associated with it, which is how an accepted connection
 * exists at all (AFD associates it with the listening address). Report each
 * of them; reporting the listener alone lost every server-side connection. */
static VOID IfInfoEmitEndpoints(PADDRESS_FILE AddrFile, PENDPOINT_ROW Rows, ULONG Capacity, PULONG Emitted)
{
    ENDPOINT_ROW Row;
    PCONNECTION_ENDPOINT *Connections = NULL, Connection;
    ULONG Count = 0, i;

    IfInfoFillAddressRow(AddrFile, &Row);
    if (AddrFile->Protocol != IPPROTO_TCP)
    {
        IfInfoEmitRow(&Row, Rows, Capacity, Emitted);
        return;
    }
    if (AddrFile->Listener)
    {
        Row.State = MIB_TCP_STATE_LISTEN;
        IfInfoEmitRow(&Row, Rows, Capacity, Emitted);
    }

    /* Pin the associated connections under the address file lock and query
     * them after releasing it: the socket queries take the connection lock,
     * which is never taken below the address file lock. */
    LockObject(AddrFile);
    for (Connection = AddrFile->Connection; Connection != NULL; Connection = Connection->Next)
        Count++;
    if (Count != 0)
        Connections = ExAllocatePoolWithTag(NonPagedPool, Count * sizeof(*Connections), SNAPSHOT_TAG);
    if (Connections != NULL)
    {
        Count = 0;
        for (Connection = AddrFile->Connection; Connection != NULL; Connection = Connection->Next)
        {
            ReferenceObject(Connection);
            Connections[Count++] = Connection;
        }
    }
    UnlockObject(AddrFile);
    if (Connections == NULL)
        return;

    for (i = 0; i < Count; i++)
    {
        if (Connections[i] != AddrFile->Listener && Connections[i]->SocketContext != NULL)
        {
            IfInfoFillAddressRow(AddrFile, &Row);
            if (IfInfoFillConnectionRow(AddrFile, Connections[i], &Row))
                IfInfoEmitRow(&Row, Rows, Capacity, Emitted);
        }
        DereferenceObject(Connections[i]);
    }
    ExFreePoolWithTag(Connections, SNAPSHOT_TAG);
}

static VOID IfInfoResolveLinkLocalScopes(PENDPOINT_ROW Rows, ULONG Count)
{
    PVOID Table = NULL;
    IP6_ADDRESS_ROW *Addresses;
    ULONG TableCount = 0, i, j;
    BOOLEAN Needed = FALSE;

    for (i = 0; i < Count && !Needed; i++)
        Needed = Rows[i].Family == TI_NSI_FAMILY_IPV6 && Rows[i].LocalScopeId == 0 && IfInfoIsLinkLocal(Rows[i].LocalAddress);
    if (Needed && NT_SUCCESS(IfInfoIp6Query(Ip6TableAddresses, &Table, &TableCount)))
    {
        Addresses = (IP6_ADDRESS_ROW *)((PIP6_TABLE_HEADER)Table + 1);
        for (i = 0; i < Count; i++)
        {
            if (Rows[i].Family != TI_NSI_FAMILY_IPV6 || Rows[i].LocalScopeId != 0 || !IfInfoIsLinkLocal(Rows[i].LocalAddress))
                continue;
            for (j = 0; j < TableCount; j++)
            {
                if (RtlCompareMemory(Addresses[j].Address, Rows[i].LocalAddress, 16) == 16)
                {
                    Rows[i].LocalScopeId = Addresses[j].Index;
                    break;
                }
            }
        }
    }
    IfInfoFree(Table);
    for (i = 0; i < Count; i++)
    {
        if (Rows[i].Family == TI_NSI_FAMILY_IPV6 && Rows[i].RemoteScopeId == 0 && IfInfoIsLinkLocal(Rows[i].RemoteAddress))
            Rows[i].RemoteScopeId = Rows[i].LocalScopeId;
    }
}

NTSTATUS IfInfoEndpoints(ULONG Family, ULONG Protocol, PENDPOINT_ROW Rows, ULONG Capacity, PULONG Count)
{
    PADDRESS_FILE *Files;
    PLIST_ENTRY Entry;
    ULONG Total = 0, Taken = 0, Emitted = 0, i;
    KIRQL OldIrql;

    PAGED_CODE();

    *Count = 0;
    TcpipAcquireSpinLock(&AddressFileListLock, &OldIrql);
    for (Entry = AddressFileListHead.Flink; Entry != &AddressFileListHead; Entry = Entry->Flink)
    {
        if (IfInfoEndpointWanted(CONTAINING_RECORD(Entry, ADDRESS_FILE, ListEntry), Family, Protocol))
            Total++;
    }
    TcpipReleaseSpinLock(&AddressFileListLock, OldIrql);

    Files = ExAllocatePoolWithTag(NonPagedPool, max(Total, 1) * sizeof(PADDRESS_FILE), SNAPSHOT_TAG);
    if (!Files)
        return STATUS_INSUFFICIENT_RESOURCES;

    TcpipAcquireSpinLock(&AddressFileListLock, &OldIrql);
    for (Entry = AddressFileListHead.Flink; Entry != &AddressFileListHead && Taken < Total; Entry = Entry->Flink)
    {
        PADDRESS_FILE AddrFile = CONTAINING_RECORD(Entry, ADDRESS_FILE, ListEntry);

        if (IfInfoEndpointWanted(AddrFile, Family, Protocol))
        {
            ReferenceObject(AddrFile);
            Files[Taken++] = AddrFile;
        }
    }
    TcpipReleaseSpinLock(&AddressFileListLock, OldIrql);

    for (i = 0; i < Taken; i++)
    {
        IfInfoEmitEndpoints(Files[i], Rows, Capacity, &Emitted);
        DereferenceObject(Files[i]);
    }
    ExFreePoolWithTag(Files, SNAPSHOT_TAG);

    IfInfoResolveLinkLocalScopes(Rows, min(Emitted, Capacity));
    *Count = Emitted;
    return Emitted > Capacity ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

/* The ICMP types the IPv4 counters know, in the order of ICMP_MIB_STATS after dwErrors */
const UCHAR IfInfoIcmpTypes[IFINFO_ICMP_TYPES] = { 3, 11, 12, 4, 5, 8, 0, 13, 14, 17, 18 };

static VOID IfInfoIcmpTypeCounts(const ICMP_MIB_STATS *Stats, PULONG Counts)
{
    Counts[0] = Stats->dwDestUnreachs;
    Counts[1] = Stats->dwTimeExcds;
    Counts[2] = Stats->dwParmProbs;
    Counts[3] = Stats->dwSrcQuenchs;
    Counts[4] = Stats->dwRedirects;
    Counts[5] = Stats->dwEchos;
    Counts[6] = Stats->dwEchoReps;
    Counts[7] = Stats->dwTimestamps;
    Counts[8] = Stats->dwTimestampReps;
    Counts[9] = Stats->dwAddrMasks;
    Counts[10] = Stats->dwAddrMaskReps;
}

static NTSTATUS IfInfoIp6Statistics(PIP6_STATISTICS Stats)
{
    ULONG Ignored, IgnoredNeeded;

    RtlZeroMemory(Stats, sizeof(*Stats));
    return LibIP6QueryTable(Ip6TableStatistics, NULL, 0, &Ignored, &IgnoredNeeded, Stats);
}

/* The interfaces and addresses of the whole IPv4 stack. The routes are counted by the producer of the
 * forward table, which adds the routes it derives from the addresses. */
static NTSTATUS IfInfoIp4Counts(PIP_STATS_ROW Row)
{
    PIF_INFO_KEY Keys = NULL;
    ULONG Capacity = 0, Total = 0, Count, i;
    NTSTATUS Status;

    Status = IfInfoInterfaceKeys(NULL, 0, &Total);
    if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
        return Status;
    if (Total != 0)
    {
        Capacity = Total;
        Keys = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(IF_INFO_KEY), SNAPSHOT_TAG);
        if (!Keys)
            return STATUS_INSUFFICIENT_RESOURCES;
        Status = IfInfoInterfaceKeys(Keys, Capacity, &Total);
        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
        {
            ExFreePoolWithTag(Keys, SNAPSHOT_TAG);
            return Status;
        }
    }
    Row->NumInterfaces = Total;
    for (i = 0; i < Total && i < Capacity; i++)
    {
        /* An interface that went away meanwhile has no rows */
        Count = 0;
        Status = IfInfoIp4Addresses(Keys[i].Index, NULL, 0, &Count);
        if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
            Row->NumAddresses += Count;
    }
    if (Keys)
        ExFreePoolWithTag(Keys, SNAPSHOT_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS IfInfoIpStats(ULONG Family, PIP_STATS_ROW Row)
{
    IP6_STATISTICS Stats;
    NTSTATUS Status;

    PAGED_CODE();

    RtlZeroMemory(Row, sizeof(*Row));
    if (Family == TI_NSI_FAMILY_IPV4)
    {
        Row->InReceives = IPStats.InReceives;
        Row->InOctets = IPStats.InOctets;
        Row->ForwDatagrams = IPStats.ForwDatagrams;
        Row->InDelivers = IPStats.InDelivers;
        Row->OutRequests = IPStats.OutRequests;
        Row->OutOctets = IPStats.OutOctets;
        Row->InHdrErrors = IPStats.InHdrErrors;
        Row->InAddrErrors = IPStats.InAddrErrors;
        Row->InUnknownProtos = IPStats.InUnknownProtos;
        Row->ReasmReqds = IPStats.ReasmReqds;
        Row->ReasmOks = IPStats.ReasmOks;
        Row->ReasmFails = IPStats.ReasmFails;
        Row->InDiscards = IPStats.InDiscards;
        Row->OutNoRoutes = IPStats.OutNoRoutes;
        Row->OutDiscards = IPStats.OutDiscards;
        Row->FragOks = IPStats.FragOks;
        Row->FragFails = IPStats.FragFails;
        Row->FragCreates = IPStats.FragCreates;
        Row->IcmpInMsgs = ICMPStats.In.dwMsgs;
        Row->IcmpInErrors = ICMPStats.In.dwErrors;
        Row->IcmpOutMsgs = ICMPStats.Out.dwMsgs;
        Row->IcmpOutErrors = ICMPStats.Out.dwErrors;
        IfInfoIcmpTypeCounts(&ICMPStats.In, Row->IcmpInTypes);
        IfInfoIcmpTypeCounts(&ICMPStats.Out, Row->IcmpOutTypes);
        Row->DefaultTtl = IP_DEFAULT_TTL;
        return IfInfoIp4Counts(Row);
    }

    Status = IfInfoIp6Statistics(&Stats);
    if (!NT_SUCCESS(Status))
        return Status;
    Row->InReceives = Stats.InReceives;
    Row->ForwDatagrams = Stats.ForwardedDatagrams;
    Row->InDelivers = Stats.InDelivers;
    Row->OutRequests = Stats.OutRequests;
    Row->InHdrErrors = Stats.InHeaderErrors;
    Row->InAddrErrors = Stats.InAddressErrors;
    Row->InUnknownProtos = Stats.InUnknownProtocols;
    Row->ReasmReqds = Stats.ReassemblyRequired;
    Row->ReasmOks = Stats.ReassemblyOk;
    Row->ReasmFails = Stats.ReassemblyFailures;
    Row->InDiscards = Stats.InDiscards;
    Row->OutNoRoutes = Stats.OutNoRoutes;
    Row->OutDiscards = Stats.OutDiscards;
    Row->FragOks = Stats.FragmentOk;
    Row->FragFails = Stats.FragmentFailures;
    Row->FragCreates = Stats.FragmentsCreated;
    Row->ReasmTimeout = Stats.ReassemblyTimeout;
    Row->IcmpInMsgs = Stats.IcmpInMessages;
    Row->IcmpInErrors = Stats.IcmpInErrors;
    Row->IcmpOutMsgs = Stats.IcmpOutMessages;
    Row->IcmpOutErrors = Stats.IcmpOutErrors;
    Row->DefaultTtl = Stats.DefaultHopLimit;
    Row->NumInterfaces = Stats.NumInterfaces;
    Row->NumAddresses = Stats.NumAddresses;
    Row->NumRoutes = Stats.NumRoutes;
    return STATUS_SUCCESS;
}

/* The rows of the endpoint tables, counted: the tables and the statistics cannot disagree */
static NTSTATUS IfInfoCountEndpoints(ULONG Family, ULONG Protocol, PULONG Total, PULONG Established)
{
    PENDPOINT_ROW Rows = NULL;
    ULONG Capacity = 0, Count = 0, Attempt, i;
    NTSTATUS Status;

    *Total = 0;
    *Established = 0;
    for (Attempt = 0; Attempt < 4; Attempt++)
    {
        Status = IfInfoEndpoints(Family, Protocol, Rows, Capacity, &Count);
        if (Status != STATUS_BUFFER_OVERFLOW || Protocol != IFINFO_PROTOCOL_TCP)
            break;
        /* The connections came and went between the two walks: try again with more room */
        if (Rows)
            ExFreePoolWithTag(Rows, SNAPSHOT_TAG);
        Capacity = Count + 16;
        Rows = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(ENDPOINT_ROW), SNAPSHOT_TAG);
        if (!Rows)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
    {
        *Total = Count;
        for (i = 0; Rows && i < Count && i < Capacity; i++)
        {
            if (Rows[i].State == MIB_TCP_STATE_ESTAB || Rows[i].State == MIB_TCP_STATE_CLOSE_WAIT)
                (*Established)++;
        }
        Status = STATUS_SUCCESS;
    }
    if (Rows)
        ExFreePoolWithTag(Rows, SNAPSHOT_TAG);
    return Status;
}

NTSTATUS IfInfoTcpStats(ULONG Family, PTCP_STATS_ROW Row)
{
    struct lib_ip_stats Stats;

    PAGED_CODE();

    /* lwIP carries the TCP of both families and counts them together: each family reports the sum */
    RtlZeroMemory(Row, sizeof(*Row));
    LibIPGetStats(&Stats);
    Row->ActiveOpens = Stats.TcpActiveOpens;
    Row->PassiveOpens = Stats.TcpPassiveOpens;
    Row->AttemptFails = Stats.TcpAttemptFails;
    Row->EstabResets = Stats.TcpEstabResets;
    Row->InSegs = Stats.TcpInSegs;
    Row->OutSegs = Stats.TcpOutSegs;
    Row->RetransSegs = Stats.TcpRetransSegs;
    Row->OutRsts = Stats.TcpOutRsts;
    Row->InErrs = Stats.TcpInErrs;
    Row->MaxConn = Stats.TcpMaxConn;
    return IfInfoCountEndpoints(Family, IFINFO_PROTOCOL_TCP, &Row->NumConns, &Row->CurrEstab);
}

NTSTATUS IfInfoUdpStats(ULONG Family, PUDP_STATS_ROW Row)
{
    IP6_STATISTICS Stats;
    ULONG Ignored;
    NTSTATUS Status;

    PAGED_CODE();

    RtlZeroMemory(Row, sizeof(*Row));
    if (Family == TI_NSI_FAMILY_IPV4)
    {
        Row->InDatagrams = UDPStats.InputDatagrams;
        Row->OutDatagrams = UDPStats.OutputDatagrams;
        Row->NoPorts = UDPStats.NumPorts;
        Row->InErrors = UDPStats.InputErrors;
    }
    else
    {
        Status = IfInfoIp6Statistics(&Stats);
        if (!NT_SUCCESS(Status))
            return Status;
        Row->InDatagrams = Stats.UdpInDatagrams;
        Row->OutDatagrams = Stats.UdpOutDatagrams;
        Row->NoPorts = Stats.UdpNoPorts;
        Row->InErrors = Stats.UdpInErrors;
    }
    return IfInfoCountEndpoints(Family, IFINFO_PROTOCOL_UDP, &Row->NumAddrs, &Ignored);
}
