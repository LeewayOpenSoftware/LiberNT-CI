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
        Entries = ExAllocatePoolWithTag(PagedPool, Allocated * sizeof(IPADDR_ENTRY), SNAPSHOT_TAG);
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
        Fibs = ExAllocatePoolWithTag(PagedPool, Allocated * sizeof(FIB_ENTRY), SNAPSHOT_TAG);
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
        Entries = ExAllocatePoolWithTag(PagedPool, Allocated * sizeof(IPARP_ENTRY), SNAPSHOT_TAG);
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

static BOOLEAN IfInfoFillEndpoint(PADDRESS_FILE AddrFile, PENDPOINT_ROW Row)
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
        RtlCopyMemory(Row->LocalAddress, AddrFile->Address.Address.IPv6Address, 16);
    else
        RtlCopyMemory(Row->LocalAddress, &AddrFile->Address.Address.IPv4Address, 4);

    if (AddrFile->Protocol != IPPROTO_TCP)
        return TRUE;

    if (AddrFile->Listener)
    {
        PADDRESS_FILE EndPoint = AddrFile->Listener->AddressFile;

        Row->State = MIB_TCP_STATE_LISTEN;
        if (!IsV6 && EndPoint)
        {
            RtlCopyMemory(Row->RemoteAddress, &EndPoint->Address.Address.IPv4Address, 4);
            Row->RemotePort = EndPoint->Port;
        }
        return TRUE;
    }
    if (AddrFile->Connection && AddrFile->Connection->SocketContext)
    {
        CHAR Local[sizeof(TA_IP6_ADDRESS)], Remote[sizeof(TA_IP6_ADDRESS)];

        if (!NT_SUCCESS(TCPGetSockAddress(AddrFile->Connection, (PTRANSPORT_ADDRESS)Local, sizeof(Local), FALSE)) ||
            !NT_SUCCESS(TCPGetSockAddress(AddrFile->Connection, (PTRANSPORT_ADDRESS)Remote, sizeof(Remote), TRUE)) ||
            !NT_SUCCESS(TCPGetSocketStatus(AddrFile->Connection, &Row->State)))
            return FALSE;
        if (IsV6)
        {
            if (((PTA_IP6_ADDRESS)Local)->Address[0].AddressType != TDI_ADDRESS_TYPE_IP6)
                return FALSE;
            RtlCopyMemory(Row->LocalAddress, ((PTA_IP6_ADDRESS)Local)->Address[0].Address[0].sin6_addr, 16);
            Row->LocalPort = ((PTA_IP6_ADDRESS)Local)->Address[0].Address[0].sin6_port;
            RtlCopyMemory(Row->RemoteAddress, ((PTA_IP6_ADDRESS)Remote)->Address[0].Address[0].sin6_addr, 16);
            Row->RemotePort = ((PTA_IP6_ADDRESS)Remote)->Address[0].Address[0].sin6_port;
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
    return FALSE;
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
        ENDPOINT_ROW Row;

        if (IfInfoFillEndpoint(Files[i], &Row))
        {
            if (Emitted < Capacity)
                Rows[Emitted] = Row;
            Emitted++;
        }
        DereferenceObject(Files[i]);
    }
    ExFreePoolWithTag(Files, SNAPSHOT_TAG);

    *Count = Emitted;
    return Emitted > Capacity ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}
