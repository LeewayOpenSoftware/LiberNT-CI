/*
 * PROJECT:     LiberNT TCP/IP protocol driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Network store producer for the NDIS interface tables
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Built outside the driver's precompiled header because the row layouts
 * come from sdk/include/wine/nsi.h, a user-mode header whose socket types
 * clash with the driver's own IP definitions. The typedefs below give it
 * the Win32 names it uses and _WINIOCTL_ keeps the user-mode ioctl header
 * out. Everything the rows need from the interface list arrives through
 * the IF_INFO_ROW snapshot of include/ifinfo.h.
 */

#include <ntifs.h>
#include <ndis.h>
#include <netiodef.h>
#include <ws2def.h>
#include <ws2ipdef.h>
#include <ipifcons.h>
#include <nsikm.h>
#include <tcpip_undoc.h>

typedef UCHAR BYTE;
typedef ULONG DWORD;
typedef struct _OVERLAPPED OVERLAPPED;
#define WINAPI NTAPI

#define _WINIOCTL_
#include <wine/nsi.h>

#include "ifinfo.h"

#define NSI_PROVIDER_TAG 'pSiT'
#define TI_NSI_MAX_ROWS 4096
#define TI_NSI_LOOPBACK_SPEED 1073741824

static PNSI_PROVIDER TiNsiIfInfo;
static PNSI_PROVIDER TiNsiIndexLuid;

static NET_LUID
TiNsiLuid(
    _In_ const IF_INFO_ROW *Row)
{
    NET_LUID Luid;

    Luid.Value = 0;
    Luid.Info.NetLuidIndex = Row->Index;
    Luid.Info.IfType = Row->Type;
    return Luid;
}

static VOID
TiNsiCountedString(
    _Out_ IF_COUNTED_STRING *String,
    _In_ const IF_INFO_ROW *Row)
{
    ULONG Bytes = 0;

    RtlMultiByteToUnicodeN(String->String, sizeof(String->String) - sizeof(WCHAR), &Bytes,
                           Row->Description, Row->DescriptionLength);
    String->Length = (USHORT)Bytes;
    String->String[Bytes / sizeof(WCHAR)] = 0;
}

static VOID
TiNsiPhysicalAddress(
    _Out_ IF_PHYSICAL_ADDRESS *Address,
    _In_ const IF_INFO_ROW *Row)
{
    Address->Length = (USHORT)min(Row->PhysAddrLength, IFINFO_MAX_PHYSADDR);
    RtlCopyMemory(Address->Address, Row->PhysAddr, Address->Length);
}

static VOID
TiNsiFillRw(
    _In_ const IF_INFO_ROW *Row,
    _Out_ struct nsi_ndis_ifinfo_rw *Rw)
{
    RtlZeroMemory(Rw, sizeof(*Rw));
    Rw->network_guid = Row->Guid;
    Rw->admin_status = NET_IF_ADMIN_STATUS_UP;
    TiNsiCountedString(&Rw->alias, Row);
    TiNsiPhysicalAddress(&Rw->phys_addr, Row);
    TiNsiCountedString(&Rw->name2, Row);
}

static VOID
TiNsiFillDynamic(
    _In_ const IF_INFO_ROW *Row,
    _Out_ struct nsi_ndis_ifinfo_dynamic *Dynamic)
{
    BOOLEAN Up = Row->OperStatus == MIB_IF_OPER_STATUS_OPERATIONAL;

    RtlZeroMemory(Dynamic, sizeof(*Dynamic));
    Dynamic->oper_status = Up ? NET_IF_OPER_STATUS_UP : NET_IF_OPER_STATUS_DOWN;
    Dynamic->media_conn_state = Up ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    Dynamic->mtu = Row->Mtu;
    Dynamic->xmit_speed = (ULONG64)Row->Speed * 100;
    if (Dynamic->xmit_speed == 0 && Row->Type == MIB_IF_TYPE_LOOPBACK)
        Dynamic->xmit_speed = TI_NSI_LOOPBACK_SPEED;
    Dynamic->rcv_speed = Dynamic->xmit_speed;
    Dynamic->in_discards = Row->Stats.InDiscarded;
    Dynamic->in_errors = Row->InErrors;
    Dynamic->in_octets = Row->Stats.InBytes;
    Dynamic->in_ucast_pkts = Row->Stats.InUnicast;
    Dynamic->in_mcast_pkts = Row->Stats.InNUnicast;
    Dynamic->out_octets = Row->Stats.OutBytes;
    Dynamic->out_ucast_pkts = Row->Stats.OutUnicast;
    Dynamic->out_mcast_pkts = Row->Stats.OutNUnicast;
    Dynamic->out_errors = Row->OutErrors;
    Dynamic->out_discards = Row->Stats.OutDiscarded;
    Dynamic->in_ucast_octs = Row->Stats.InBytes;
    Dynamic->out_ucast_octs = Row->Stats.OutBytes;
}

static VOID
TiNsiFillStatic(
    _In_ const IF_INFO_ROW *Row,
    _Out_ struct nsi_ndis_ifinfo_static *Static)
{
    BOOLEAN Loopback = Row->Type == MIB_IF_TYPE_LOOPBACK;

    RtlZeroMemory(Static, sizeof(*Static));
    Static->if_index = Row->Index;
    TiNsiCountedString(&Static->descr, Row);
    Static->type = Row->Type;
    Static->access_type = Loopback ? NET_IF_ACCESS_LOOPBACK : NET_IF_ACCESS_BROADCAST;
    Static->conn_type = NET_IF_CONNECTION_DEDICATED;
    Static->if_guid = Row->Guid;
    Static->conn_present = !Loopback;
    TiNsiPhysicalAddress(&Static->perm_phys_addr, Row);
    Static->flags.hw = !Loopback;
    Static->phys_medium_type = Row->PhysicalMedium;
}

static NTSTATUS NTAPI
TiNsiIfInfoEnumerate(
    _In_opt_ PVOID Context,
    _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    ULONG Capacity = Request->Count, Count = 0, i;
    PIF_INFO_ROW Rows = NULL;
    NTSTATUS Status;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);

    if (Capacity > TI_NSI_MAX_ROWS)
        Capacity = TI_NSI_MAX_ROWS;
    if (Capacity != 0)
    {
        Rows = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(IF_INFO_ROW), NSI_PROVIDER_TAG);
        if (Rows == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = IfInfoSnapshot(Capacity, Rows, &Count);
    if (Status == STATUS_SUCCESS || Status == STATUS_BUFFER_OVERFLOW)
    {
        for (i = 0; i < Count && i < Capacity; i++)
        {
            if (Request->KeyData != NULL)
                ((NET_LUID *)Request->KeyData)[i] = TiNsiLuid(&Rows[i]);
            if (Request->RwData != NULL)
                TiNsiFillRw(&Rows[i], (struct nsi_ndis_ifinfo_rw *)Request->RwData + i);
            if (Request->DynamicData != NULL)
                TiNsiFillDynamic(&Rows[i], (struct nsi_ndis_ifinfo_dynamic *)Request->DynamicData + i);
            if (Request->StaticData != NULL)
                TiNsiFillStatic(&Rows[i], (struct nsi_ndis_ifinfo_static *)Request->StaticData + i);
        }
        Request->Count = Count;
    }
    if (Rows != NULL)
        ExFreePoolWithTag(Rows, NSI_PROVIDER_TAG);
    return Status;
}

static NTSTATUS NTAPI
TiNsiIfInfoGetAll(
    _In_opt_ PVOID Context,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _Out_writes_bytes_opt_(sizeof(struct nsi_ndis_ifinfo_rw)) PVOID Rw,
    _Out_writes_bytes_opt_(sizeof(struct nsi_ndis_ifinfo_dynamic)) PVOID Dynamic,
    _Out_writes_bytes_opt_(sizeof(struct nsi_ndis_ifinfo_static)) PVOID Static)
{
    const NET_LUID *Luid = Key;
    IF_INFO_ROW Row;
    NTSTATUS Status;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(FirstArg);

    if (KeySize != sizeof(NET_LUID))
        return STATUS_INVALID_PARAMETER;
    Status = IfInfoSnapshotByIndex(Luid->Info.NetLuidIndex, &Row);
    if (!NT_SUCCESS(Status))
        return Status;
    if (TiNsiLuid(&Row).Value != Luid->Value)
        return STATUS_NOT_FOUND;
    if (Rw != NULL)
        TiNsiFillRw(&Row, Rw);
    if (Dynamic != NULL)
        TiNsiFillDynamic(&Row, Dynamic);
    if (Static != NULL)
        TiNsiFillStatic(&Row, Static);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
TiNsiIndexLuidGetAll(
    _In_opt_ PVOID Context,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _Out_writes_bytes_opt_(0) PVOID Rw,
    _Out_writes_bytes_opt_(0) PVOID Dynamic,
    _Out_writes_bytes_opt_(sizeof(NET_LUID)) PVOID Static)
{
    IF_INFO_ROW Row;
    NTSTATUS Status;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(FirstArg);
    UNREFERENCED_PARAMETER(Rw);
    UNREFERENCED_PARAMETER(Dynamic);

    if (KeySize != sizeof(UINT))
        return STATUS_INVALID_PARAMETER;
    Status = IfInfoSnapshotByIndex(*(const UINT *)Key, &Row);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Static != NULL)
        *(NET_LUID *)Static = TiNsiLuid(&Row);
    return STATUS_SUCCESS;
}

static const NSI_PROVIDER_DISPATCH TiNsiIfInfoDispatch =
{
    sizeof(NSI_PROVIDER_DISPATCH),
    { sizeof(NET_LUID), sizeof(struct nsi_ndis_ifinfo_rw), sizeof(struct nsi_ndis_ifinfo_dynamic), sizeof(struct nsi_ndis_ifinfo_static) },
    TiNsiIfInfoEnumerate,
    TiNsiIfInfoGetAll,
    NULL
};

static const NSI_PROVIDER_DISPATCH TiNsiIndexLuidDispatch =
{
    sizeof(NSI_PROVIDER_DISPATCH),
    { sizeof(UINT), 0, 0, sizeof(NET_LUID) },
    NULL,
    TiNsiIndexLuidGetAll,
    NULL
};


typedef int (*TI_NSI_COMPARE)(const VOID *Left, const VOID *Right);

static VOID
TiNsiSort(
    _Inout_updates_bytes_(Count * Size) PVOID Base,
    _In_ ULONG Count,
    _In_ ULONG Size,
    _In_ TI_NSI_COMPARE Compare,
    _In_reads_bytes_(Size) PVOID Scratch)
{
    PUCHAR Rows = Base;
    ULONG i, j;

    for (i = 1; i < Count; i++)
    {
        RtlCopyMemory(Scratch, Rows + i * Size, Size);
        for (j = i; j > 0 && Compare(Rows + (j - 1) * Size, Scratch) > 0; j--)
            RtlCopyMemory(Rows + j * Size, Rows + (j - 1) * Size, Size);
        RtlCopyMemory(Rows + j * Size, Scratch, Size);
    }
}

static NTSTATUS
TiNsiInterfaceKeys(
    _Outptr_result_maybenull_ PIF_INFO_KEY *Keys,
    _Out_ PULONG Count)
{
    PIF_INFO_KEY Buffer = NULL;
    ULONG Capacity = 0, Total = 0, Attempt;
    NTSTATUS Status;

    *Keys = NULL;
    *Count = 0;
    for (Attempt = 0; Attempt < 4; Attempt++)
    {
        Status = IfInfoInterfaceKeys(Buffer, Capacity, &Total);
        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
        {
            IfInfoFree(Buffer);
            return Status;
        }
        if (Total <= Capacity)
        {
            *Keys = Buffer;
            *Count = Total;
            return STATUS_SUCCESS;
        }
        IfInfoFree(Buffer);
        Capacity = Total + 4;
        Buffer = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(IF_INFO_KEY), NSI_PROVIDER_TAG);
        if (Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    ExFreePoolWithTag(Buffer, NSI_PROVIDER_TAG);
    return STATUS_BUFFER_OVERFLOW;
}

static NET_LUID
TiNsiLuidForIndex(
    _In_reads_(Count) const IF_INFO_KEY *Keys,
    _In_ ULONG Count,
    _In_ UINT Index)
{
    NET_LUID Luid;
    ULONG i;

    Luid.Value = 0;
    Luid.Info.NetLuidIndex = Index;
    Luid.Info.IfType = MIB_IF_TYPE_OTHER;
    for (i = 0; i < Count; i++)
    {
        if (Keys[i].Index == Index)
        {
            Luid.Info.IfType = Keys[i].Type;
            break;
        }
    }
    return Luid;
}

static VOID
TiNsiZoneIndices(
    _Out_writes_(ScopeLevelCount) ULONG *Zones,
    _In_ UINT Index,
    _In_ BOOLEAN Loopback,
    _In_reads_(ScopeLevelCount) const ULONG *Overrides)
{
    UINT Level;

    for (Level = 0; Level < ScopeLevelCount; Level++)
        Zones[Level] = (Level < ScopeLevelAdmin && !Loopback) ? Index : 1;
    Zones[ScopeLevelGlobal] = 0;
    for (Level = 0; Level < ScopeLevelCount; Level++)
    {
        if (Overrides[Level] != 0)
            Zones[Level] = Overrides[Level];
    }
}

static VOID
TiNsiFillIp6Interface(
    _In_ const IP6_INTERFACE_ROW *Row,
    _In_ NET_LUID Luid,
    _Out_opt_ struct nsi_ip_interface_key *Key,
    _Out_opt_ struct nsi_ip_interface_rw *Rw,
    _Out_opt_ struct nsi_ip_interface_dynamic *Dynamic,
    _Out_opt_ struct nsi_ip_interface_static *Static)
{
    if (Key != NULL)
        Key->luid = Luid;
    if (Rw != NULL)
    {
        RtlZeroMemory(Rw, sizeof(*Rw));
        Rw->metric = Row->Metric;
        Rw->base_reachable_time = Row->BaseReachableTime;
        Rw->retransmit_time = Row->RetransmitTime;
        Rw->path_mtu_discovery_timeout = Row->PathMtuDiscoveryTimeout;
        Rw->dad_transmits = Row->DadTransmits;
        Rw->link_local_address_behavior = LinkLocalAlwaysOn;
        Rw->link_local_address_timeout = Row->LinkLocalAddressTimeout;
        Rw->mtu = Row->Loopback ? ~0u : Row->Mtu;
        Rw->site_prefix_len = Row->SitePrefixLength;
        Rw->router_discovery_behaviour = Row->RouterDiscoveryBehavior;
        TiNsiZoneIndices(Rw->zone_indices, Row->Index, Row->Loopback, Row->ZoneIndices);
    }
    if (Dynamic != NULL)
    {
        RtlZeroMemory(Dynamic, sizeof(*Dynamic));
        Dynamic->if_index = Row->Index;
        Dynamic->reachable_time = Row->ReachableTime;
        Dynamic->connected = Row->Connected;
    }
    if (Static != NULL)
        RtlZeroMemory(Static, sizeof(*Static));
}

static int
TiNsiCompareIp6Address(
    _In_ const VOID *Left,
    _In_ const VOID *Right)
{
    const IP6_ADDRESS_ROW *A = Left, *B = Right;

    if (A->Index != B->Index)
        return A->Index < B->Index ? -1 : 1;
    return memcmp(A->Address, B->Address, sizeof(A->Address));
}

static int
TiNsiCompareIp6Route(
    _In_ const VOID *Left,
    _In_ const VOID *Right)
{
    const IP6_ROUTE_ROW *A = Left, *B = Right;
    int Diff = memcmp(A->Prefix, B->Prefix, sizeof(A->Prefix));

    if (Diff != 0)
        return Diff;
    if (A->PrefixLength != B->PrefixLength)
        return A->PrefixLength < B->PrefixLength ? -1 : 1;
    if (A->Index != B->Index)
        return A->Index < B->Index ? -1 : 1;
    return memcmp(A->NextHop, B->NextHop, sizeof(A->NextHop));
}

static NTSTATUS
TiNsiIp6Enumerate(
    _In_ ULONG TableId,
    _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PIF_INFO_KEY Keys = NULL;
    PVOID Table = NULL;
    PIP6_TABLE_HEADER Header;
    ULONG KeyCount = 0, Count = 0, Capacity = Request->Count, i;
    NTSTATUS Status;

    Status = TiNsiInterfaceKeys(&Keys, &KeyCount);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = IfInfoIp6Query(TableId, &Table, &Count);
    if (!NT_SUCCESS(Status))
    {
        IfInfoFree(Keys);
        return Status;
    }
    Header = Table;

    switch (TableId)
    {
        case Ip6TableInterfaces:
        {
            IP6_INTERFACE_ROW *Rows = (IP6_INTERFACE_ROW *)(Header + 1);

            for (i = 0; i < Count && i < Capacity; i++)
            {
                TiNsiFillIp6Interface(&Rows[i], TiNsiLuidForIndex(Keys, KeyCount, Rows[i].Index),
                                      Request->KeyData ? (struct nsi_ip_interface_key *)Request->KeyData + i : NULL,
                                      Request->RwData ? (struct nsi_ip_interface_rw *)Request->RwData + i : NULL,
                                      Request->DynamicData ? (struct nsi_ip_interface_dynamic *)Request->DynamicData + i : NULL,
                                      Request->StaticData ? (struct nsi_ip_interface_static *)Request->StaticData + i : NULL);
            }
            break;
        }
        case Ip6TableAddresses:
        {
            IP6_ADDRESS_ROW *Rows = (IP6_ADDRESS_ROW *)(Header + 1), Scratch;

            TiNsiSort(Rows, Count, sizeof(*Rows), TiNsiCompareIp6Address, &Scratch);
            for (i = 0; i < Count && i < Capacity; i++)
            {
                const IP6_ADDRESS_ROW *Row = &Rows[i];

                if (Request->KeyData != NULL)
                {
                    struct nsi_ipv6_unicast_key *Key = (struct nsi_ipv6_unicast_key *)Request->KeyData + i;

                    RtlZeroMemory(Key, sizeof(*Key));
                    Key->luid = TiNsiLuidForIndex(Keys, KeyCount, Row->Index);
                    RtlCopyMemory(&Key->addr, Row->Address, sizeof(Key->addr));
                }
                if (Request->RwData != NULL)
                {
                    struct nsi_ip_unicast_rw *Rw = (struct nsi_ip_unicast_rw *)Request->RwData + i;

                    RtlZeroMemory(Rw, sizeof(*Rw));
                    Rw->preferred_lifetime = Row->PreferredLifetime;
                    Rw->valid_lifetime = Row->ValidLifetime;
                    Rw->prefix_origin = Row->PrefixOrigin;
                    Rw->suffix_origin = Row->SuffixOrigin;
                    Rw->on_link_prefix = Row->PrefixLength;
                }
                if (Request->DynamicData != NULL)
                {
                    struct nsi_ip_unicast_dynamic *Dynamic = (struct nsi_ip_unicast_dynamic *)Request->DynamicData + i;

                    RtlZeroMemory(Dynamic, sizeof(*Dynamic));
                    Dynamic->scope_id = IN6_IS_ADDR_LINKLOCAL((const IN6_ADDR *)Row->Address) ? Row->Index : 0;
                    Dynamic->dad_state = Row->DadState;
                }
                if (Request->StaticData != NULL)
                {
                    struct nsi_ip_unicast_static *Static = (struct nsi_ip_unicast_static *)Request->StaticData + i;

                    RtlZeroMemory(Static, sizeof(*Static));
                    Static->creation_time = Row->CreationTime.QuadPart;
                }
            }
            break;
        }
        case Ip6TableNeighbors:
        {
            IP6_NEIGHBOR_ROW *Rows = (IP6_NEIGHBOR_ROW *)(Header + 1);

            for (i = 0; i < Count && i < Capacity; i++)
            {
                const IP6_NEIGHBOR_ROW *Row = &Rows[i];
                ULONG Length = min(Row->PhysicalAddressLength, IF_MAX_PHYS_ADDRESS_LENGTH);

                if (Request->KeyData != NULL)
                {
                    struct nsi_ipv6_neighbour_key *Key = (struct nsi_ipv6_neighbour_key *)Request->KeyData + i;

                    RtlZeroMemory(Key, sizeof(*Key));
                    Key->luid = Key->luid2 = TiNsiLuidForIndex(Keys, KeyCount, Row->Index);
                    RtlCopyMemory(&Key->addr, Row->Address, sizeof(Key->addr));
                }
                if (Request->RwData != NULL)
                {
                    struct nsi_ip_neighbour_rw *Rw = (struct nsi_ip_neighbour_rw *)Request->RwData + i;

                    RtlZeroMemory(Rw, sizeof(*Rw));
                    RtlCopyMemory(Rw->phys_addr, Row->PhysicalAddress, Length);
                }
                if (Request->DynamicData != NULL)
                {
                    struct nsi_ip_neighbour_dynamic *Dynamic = (struct nsi_ip_neighbour_dynamic *)Request->DynamicData + i;

                    RtlZeroMemory(Dynamic, sizeof(*Dynamic));
                    Dynamic->state = Row->State;
                    Dynamic->flags.is_router = Row->IsRouter;
                    Dynamic->flags.is_unreachable = Row->IsUnreachable;
                    Dynamic->phys_addr_len = (USHORT)Length;
                }
            }
            break;
        }
        case Ip6TableRoutes:
        {
            IP6_ROUTE_ROW *Rows = (IP6_ROUTE_ROW *)(Header + 1), Scratch;

            TiNsiSort(Rows, Count, sizeof(*Rows), TiNsiCompareIp6Route, &Scratch);
            for (i = 0; i < Count && i < Capacity; i++)
            {
                const IP6_ROUTE_ROW *Row = &Rows[i];

                if (Request->KeyData != NULL)
                {
                    struct nsi_ipv6_forward_key *Key = (struct nsi_ipv6_forward_key *)Request->KeyData + i;

                    RtlZeroMemory(Key, sizeof(*Key));
                    RtlCopyMemory(&Key->prefix, Row->Prefix, sizeof(Key->prefix));
                    Key->prefix_len = (BYTE)Row->PrefixLength;
                    Key->luid = Key->luid2 = TiNsiLuidForIndex(Keys, KeyCount, Row->Index);
                    RtlCopyMemory(&Key->next_hop, Row->NextHop, sizeof(Key->next_hop));
                }
                if (Request->RwData != NULL)
                {
                    struct nsi_ip_forward_rw *Rw = (struct nsi_ip_forward_rw *)Request->RwData + i;

                    RtlZeroMemory(Rw, sizeof(*Rw));
                    Rw->valid_lifetime = Row->ValidLifetime;
                    Rw->preferred_lifetime = Row->PreferredLifetime;
                    Rw->metric = Row->Metric;
                    Rw->protocol = Row->Protocol;
                    Rw->loopback = Row->Loopback;
                    Rw->autoconf = Row->Autoconfigure;
                    Rw->publish = Row->Publish;
                    Rw->immortal = Row->Immortal;
                }
                if (Request->DynamicData != NULL)
                {
                    struct nsi_ipv6_forward_dynamic *Dynamic = (struct nsi_ipv6_forward_dynamic *)Request->DynamicData + i;

                    RtlZeroMemory(Dynamic, sizeof(*Dynamic));
                    RtlCopyMemory(&Dynamic->addr2, Row->Prefix, sizeof(Dynamic->addr2));
                }
                if (Request->StaticData != NULL)
                {
                    struct nsi_ip_forward_static *Static = (struct nsi_ip_forward_static *)Request->StaticData + i;

                    RtlZeroMemory(Static, sizeof(*Static));
                    Static->origin = Row->Origin;
                    Static->if_index = Row->Index;
                }
            }
            break;
        }
        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
    }

    IfInfoFree(Table);
    IfInfoFree(Keys);
    if (!NT_SUCCESS(Status))
        return Status;
    Request->Count = Count;
    return Count > Capacity ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

typedef NTSTATUS (*TI_NSI_ENUMERATE)(_In_ ULONG Table, _Inout_ PNSI_ENUMERATE_REQUEST Request);

static NTSTATUS
TiNsiGetFromEnumeration(
    _In_ TI_NSI_ENUMERATE Enumerate,
    _In_ ULONG Table,
    _In_ const NSI_ROW_SIZES *Sizes,
    _In_reads_bytes_(Sizes->Key) const VOID *Key,
    _Out_writes_bytes_opt_(Sizes->Rw) PVOID Rw,
    _Out_writes_bytes_opt_(Sizes->Dynamic) PVOID Dynamic,
    _Out_writes_bytes_opt_(Sizes->Static) PVOID Static)
{
    NSI_ENUMERATE_REQUEST Request;
    PUCHAR Block = NULL;
    ULONG Count = 0, Capacity = 0, RowSize, i, Attempt;
    NTSTATUS Status = STATUS_NOT_FOUND;

    RowSize = Sizes->Key + (Rw ? Sizes->Rw : 0) + (Dynamic ? Sizes->Dynamic : 0) + (Static ? Sizes->Static : 0);
    for (Attempt = 0; Attempt < 4; Attempt++)
    {
        RtlZeroMemory(&Request, sizeof(Request));
        Request.Count = Capacity;
        if (Capacity != 0)
        {
            Request.KeyData = Block;
            Request.RwData = Rw ? Block + Sizes->Key * Capacity : NULL;
            Request.DynamicData = Dynamic ? Block + (Sizes->Key + (Rw ? Sizes->Rw : 0)) * Capacity : NULL;
            Request.StaticData = Static ? Block + (Sizes->Key + (Rw ? Sizes->Rw : 0) + (Dynamic ? Sizes->Dynamic : 0)) * Capacity : NULL;
        }
        Status = Enumerate(Table, &Request);
        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
            break;
        Count = Request.Count;
        if (Count <= Capacity)
        {
            Status = STATUS_NOT_FOUND;
            for (i = 0; i < Count; i++)
            {
                if (RtlCompareMemory(Block + Sizes->Key * i, Key, Sizes->Key) != Sizes->Key)
                    continue;
                if (Rw != NULL)
                    RtlCopyMemory(Rw, (PUCHAR)Request.RwData + Sizes->Rw * i, Sizes->Rw);
                if (Dynamic != NULL)
                    RtlCopyMemory(Dynamic, (PUCHAR)Request.DynamicData + Sizes->Dynamic * i, Sizes->Dynamic);
                if (Static != NULL)
                    RtlCopyMemory(Static, (PUCHAR)Request.StaticData + Sizes->Static * i, Sizes->Static);
                Status = STATUS_SUCCESS;
                break;
            }
            break;
        }
        if (Block != NULL)
            ExFreePoolWithTag(Block, NSI_PROVIDER_TAG);
        Capacity = Count + 4;
        if (Capacity > TI_NSI_MAX_ROWS || RowSize == 0 || Capacity > MAXULONG / RowSize)
        {
            Block = NULL;
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        Block = ExAllocatePoolWithTag(PagedPool, Capacity * RowSize, NSI_PROVIDER_TAG);
        if (Block == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        RtlZeroMemory(Block, Capacity * RowSize);
    }
    if (Block != NULL)
        ExFreePoolWithTag(Block, NSI_PROVIDER_TAG);
    return Status;
}

static NTSTATUS NTAPI
TiNsiIp6InterfaceEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiIp6Enumerate(Ip6TableInterfaces, Request);
}

static NTSTATUS NTAPI
TiNsiIp6UnicastEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiIp6Enumerate(Ip6TableAddresses, Request);
}

static NTSTATUS NTAPI
TiNsiIp6NeighbourEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiIp6Enumerate(Ip6TableNeighbors, Request);
}

static NTSTATUS NTAPI
TiNsiIp6ForwardEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiIp6Enumerate(Ip6TableRoutes, Request);
}

static const NSI_PROVIDER_DISPATCH TiNsiIp6InterfaceDispatch;
static const NSI_PROVIDER_DISPATCH TiNsiIp6UnicastDispatch;
static const NSI_PROVIDER_DISPATCH TiNsiIp6NeighbourDispatch;
static const NSI_PROVIDER_DISPATCH TiNsiIp6ForwardDispatch;

static NTSTATUS NTAPI
TiNsiIp6GetAll(
    _In_opt_ PVOID Context,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _Out_writes_bytes_opt_(0) PVOID Rw,
    _Out_writes_bytes_opt_(0) PVOID Dynamic,
    _Out_writes_bytes_opt_(0) PVOID Static)
{
    const NSI_PROVIDER_DISPATCH *Dispatch = Context;
    ULONG Table;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(FirstArg);

    if (Dispatch == NULL || KeySize != Dispatch->RowSizes.Key)
        return STATUS_INVALID_PARAMETER;
    if (Dispatch == &TiNsiIp6InterfaceDispatch)
        Table = Ip6TableInterfaces;
    else if (Dispatch == &TiNsiIp6UnicastDispatch)
        Table = Ip6TableAddresses;
    else if (Dispatch == &TiNsiIp6NeighbourDispatch)
        Table = Ip6TableNeighbors;
    else if (Dispatch == &TiNsiIp6ForwardDispatch)
        Table = Ip6TableRoutes;
    else
        return STATUS_INVALID_PARAMETER;
    return TiNsiGetFromEnumeration(TiNsiIp6Enumerate, Table, &Dispatch->RowSizes, Key, Rw, Dynamic, Static);
}

static const NSI_PROVIDER_DISPATCH TiNsiIp6InterfaceDispatch =
{
    sizeof(NSI_PROVIDER_DISPATCH),
    { sizeof(struct nsi_ip_interface_key), sizeof(struct nsi_ip_interface_rw), sizeof(struct nsi_ip_interface_dynamic), sizeof(struct nsi_ip_interface_static) },
    TiNsiIp6InterfaceEnumerate,
    TiNsiIp6GetAll,
    NULL
};

static const NSI_PROVIDER_DISPATCH TiNsiIp6UnicastDispatch =
{
    sizeof(NSI_PROVIDER_DISPATCH),
    { sizeof(struct nsi_ipv6_unicast_key), sizeof(struct nsi_ip_unicast_rw), sizeof(struct nsi_ip_unicast_dynamic), sizeof(struct nsi_ip_unicast_static) },
    TiNsiIp6UnicastEnumerate,
    TiNsiIp6GetAll,
    NULL
};

static const NSI_PROVIDER_DISPATCH TiNsiIp6NeighbourDispatch =
{
    sizeof(NSI_PROVIDER_DISPATCH),
    { sizeof(struct nsi_ipv6_neighbour_key), sizeof(struct nsi_ip_neighbour_rw), sizeof(struct nsi_ip_neighbour_dynamic), 0 },
    TiNsiIp6NeighbourEnumerate,
    TiNsiIp6GetAll,
    NULL
};

static const NSI_PROVIDER_DISPATCH TiNsiIp6ForwardDispatch =
{
    sizeof(NSI_PROVIDER_DISPATCH),
    { sizeof(struct nsi_ipv6_forward_key), sizeof(struct nsi_ip_forward_rw), sizeof(struct nsi_ipv6_forward_dynamic), sizeof(struct nsi_ip_forward_static) },
    TiNsiIp6ForwardEnumerate,
    TiNsiIp6GetAll,
    NULL
};

static PNSI_PROVIDER TiNsiIp6Providers[4];

static VOID
TiNsiRegisterIp6(VOID)
{
    static const struct
    {
        ULONG Table;
        const NSI_PROVIDER_DISPATCH *Dispatch;
    } Tables[4] =
    {
        { NSI_IP_INTERFACE_TABLE, &TiNsiIp6InterfaceDispatch },
        { NSI_IP_UNICAST_TABLE, &TiNsiIp6UnicastDispatch },
        { NSI_IP_NEIGHBOUR_TABLE, &TiNsiIp6NeighbourDispatch },
        { NSI_IP_FORWARD_TABLE, &TiNsiIp6ForwardDispatch },
    };
    ULONG i;

    for (i = 0; i < 4; i++)
    {
        if (!NT_SUCCESS(NsiRegisterProvider(&NPI_MS_IPV6_MODULEID, Tables[i].Table, Tables[i].Dispatch,
                                            (PVOID)Tables[i].Dispatch, &TiNsiIp6Providers[i])))
            TiNsiIp6Providers[i] = NULL;
    }
}

static VOID
TiNsiDeregisterIp6(VOID)
{
    ULONG i;

    for (i = 0; i < 4; i++)
    {
        if (TiNsiIp6Providers[i] != NULL)
        {
            NsiDeregisterProvider(TiNsiIp6Providers[i]);
            TiNsiIp6Providers[i] = NULL;
        }
    }
}

VOID
TiNsiRegister(VOID)
{
    PAGED_CODE();

    if (!NT_SUCCESS(NsiRegisterProvider(&NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE,
                                        &TiNsiIfInfoDispatch, NULL, &TiNsiIfInfo)))
        TiNsiIfInfo = NULL;
    if (!NT_SUCCESS(NsiRegisterProvider(&NPI_MS_NDIS_MODULEID, NSI_NDIS_INDEX_LUID_TABLE,
                                        &TiNsiIndexLuidDispatch, NULL, &TiNsiIndexLuid)))
        TiNsiIndexLuid = NULL;
    TiNsiRegisterIp6();
}

VOID
TiNsiNotifyInterface(
    _In_ UINT Index,
    _In_ ULONG Change)
{
    if (TiNsiIfInfo != NULL)
        NsiNotifyChange(TiNsiIfInfo, (NSI_CHANGE_TYPE)Change, &Index, sizeof(Index));
    if (TiNsiIndexLuid != NULL && Change != TI_NSI_CHANGE_PARAMETER)
        NsiNotifyChange(TiNsiIndexLuid, (NSI_CHANGE_TYPE)Change, &Index, sizeof(Index));
}

VOID
TiNsiDeregister(VOID)
{
    PAGED_CODE();

    TiNsiDeregisterIp6();
    if (TiNsiIfInfo != NULL)
    {
        NsiDeregisterProvider(TiNsiIfInfo);
        TiNsiIfInfo = NULL;
    }
    if (TiNsiIndexLuid != NULL)
    {
        NsiDeregisterProvider(TiNsiIndexLuid);
        TiNsiIndexLuid = NULL;
    }
}
