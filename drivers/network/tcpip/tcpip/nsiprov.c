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
}

VOID
TiNsiDeregister(VOID)
{
    PAGED_CODE();

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
