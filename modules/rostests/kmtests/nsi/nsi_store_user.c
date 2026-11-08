/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Network store (NSI) provider and table access test, user half
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netiodef.h>
#include <wine/nsi.h>

#include "nsi_store.h"

static const GUID StoreTestGuid = NSISTORE_MODULE_GUID;

static HANDLE
OpenNsiDevice(
    _In_ DWORD Flags)
{
    return CreateFileW(L"\\\\.\\Nsi", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, Flags, NULL);
}

static DWORD
Ioctl(
    _In_ HANDLE Device,
    _In_ DWORD Code,
    _In_reads_bytes_(InSize) PVOID In,
    _In_ DWORD InSize,
    _Out_writes_bytes_opt_(OutSize) PVOID Out,
    _In_ DWORD OutSize,
    _Out_ PDWORD Returned)
{
    *Returned = 0;
    if (DeviceIoControl(Device, Code, In, InSize, Out, OutSize, Returned, NULL))
        return ERROR_SUCCESS;
    return GetLastError();
}

static void
TestDeviceEnumerate(
    _In_ HANDLE Device,
    _In_ const NPI_MODULEID *Module)
{
    struct nsiproxy_enumerate_all in;
    BYTE out[sizeof(DWORD) + 4 * (sizeof(ULONG) + sizeof(NSISTORE_RW) + sizeof(NSISTORE_DYNAMIC) + sizeof(NSISTORE_STATIC))];
    DWORD returned, err, count;
    const ULONG *keys;
    const NSISTORE_DYNAMIC *dynamic;

    memset(&in, 0, sizeof(in));
    in.module = *Module;
    in.table = NSISTORE_TABLE;
    in.key_size = sizeof(ULONG);
    in.rw_size = sizeof(NSISTORE_RW);
    in.dynamic_size = sizeof(NSISTORE_DYNAMIC);
    in.static_size = sizeof(NSISTORE_STATIC);
    in.count = 4;
    memset(out, 0, sizeof(out));
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
    count = *(DWORD *)out;
    ok_eq_ulong(count, (DWORD)NSISTORE_ROWS);
    keys = (const ULONG *)(out + sizeof(DWORD));
    dynamic = (const NSISTORE_DYNAMIC *)(out + sizeof(DWORD) + 4 * (sizeof(ULONG) + sizeof(NSISTORE_RW)));
    ok_eq_ulong(keys[0], 1UL);
    ok_eq_ulong(keys[1], 2UL);
    ok_eq_ulong(dynamic[1].X, (ULONG)NSISTORE_X(2));
    ok(returned >= sizeof(DWORD) + 2 * sizeof(ULONG), "enumerate returned %lu bytes\n", returned);

    in.count = 1;
    memset(out, 0, sizeof(out));
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_MORE_DATA);
    ok_eq_ulong(*(DWORD *)out, (DWORD)NSISTORE_ROWS);
    ok_eq_ulong(keys[0], 1UL);

    in.count = 4;
    in.rw_size = sizeof(NSISTORE_RW) + 4;
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INVALID_PARAMETER);
    in.rw_size = sizeof(NSISTORE_RW);

    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, sizeof(DWORD), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INSUFFICIENT_BUFFER);

    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in) - 4, out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INVALID_PARAMETER);

    in.table = NSISTORE_SCRATCH_TABLE;
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_NOT_FOUND);
}

static void
TestDeviceRows(
    _In_ HANDLE Device,
    _In_ const NPI_MODULEID *Module)
{
    struct
    {
        struct nsiproxy_get_all_parameters header;
        BYTE key_rest[sizeof(ULONG) - 1];
    } get_all;
    struct
    {
        struct nsiproxy_get_parameter header;
        BYTE key_rest[sizeof(ULONG) - 1];
    } get_param;
    BYTE out[sizeof(NSISTORE_RW) + sizeof(NSISTORE_DYNAMIC) + sizeof(NSISTORE_STATIC)];
    const NSISTORE_RW *rw = (const NSISTORE_RW *)out;
    const NSISTORE_STATIC *stat = (const NSISTORE_STATIC *)(out + sizeof(NSISTORE_RW) + sizeof(NSISTORE_DYNAMIC));
    DWORD returned, err;
    ULONG value, key;

    memset(&get_all, 0, sizeof(get_all));
    get_all.header.module = *Module;
    get_all.header.table = NSISTORE_TABLE;
    get_all.header.key_size = sizeof(ULONG);
    get_all.header.rw_size = sizeof(NSISTORE_RW);
    get_all.header.dynamic_size = sizeof(NSISTORE_DYNAMIC);
    get_all.header.static_size = sizeof(NSISTORE_STATIC);
    key = 2;
    memcpy(get_all.header.key, &key, sizeof(key));
    memset(out, 0, sizeof(out));
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_ALL_PARAMETERS, &get_all, sizeof(get_all), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
    ok_eq_ulong(returned, (DWORD)sizeof(out));
    ok_eq_ulong(rw->B, (ULONG)NSISTORE_RW_B(2));
    ok_eq_ulong(stat->S[1], (ULONG)NSISTORE_S1(2));

    key = 3;
    memcpy(get_all.header.key, &key, sizeof(key));
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_ALL_PARAMETERS, &get_all, sizeof(get_all), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_NOT_FOUND);

    key = 1;
    memcpy(get_all.header.key, &key, sizeof(key));
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_ALL_PARAMETERS, &get_all, sizeof(get_all), out, sizeof(out) - 1, &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INSUFFICIENT_BUFFER);

    get_all.header.key_size = sizeof(USHORT);
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_ALL_PARAMETERS, &get_all, sizeof(get_all), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INVALID_PARAMETER);

    memset(&get_param, 0, sizeof(get_param));
    get_param.header.module = *Module;
    get_param.header.table = NSISTORE_TABLE;
    get_param.header.key_size = sizeof(ULONG);
    get_param.header.param_type = NSI_PARAM_TYPE_DYNAMIC;
    get_param.header.data_offset = FIELD_OFFSET(NSISTORE_DYNAMIC, Y);
    key = 1;
    memcpy(get_param.header.key, &key, sizeof(key));
    value = 0;
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_PARAMETER, &get_param, sizeof(get_param), &value, sizeof(value), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
    ok_eq_ulong(returned, (DWORD)sizeof(value));
    ok_eq_ulong(value, (ULONG)NSISTORE_Y(1));

    get_param.header.data_offset = FIELD_OFFSET(NSISTORE_DYNAMIC, X);
    memset(out, 0xEE, sizeof(out));
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_PARAMETER, &get_param, sizeof(get_param), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
    ok_eq_ulong(returned, (DWORD)(sizeof(NSISTORE_DYNAMIC) - FIELD_OFFSET(NSISTORE_DYNAMIC, X)));
    ok_eq_ulong(((const ULONG *)out)[0], (ULONG)NSISTORE_X(1));
    ok_eq_ulong(((const ULONG *)out)[1], (ULONG)NSISTORE_Y(1));
    ok(out[returned] == 0xEE, "byte after the clamped parameter read was written: 0x%02x\n", out[returned]);

    get_param.header.data_offset = sizeof(NSISTORE_DYNAMIC);
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_PARAMETER, &get_param, sizeof(get_param), &value, sizeof(value), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INVALID_PARAMETER);

    get_param.header.data_offset = 0;
    get_param.header.param_type = NSI_PARAM_TYPE_STATIC + 1;
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_PARAMETER, &get_param, sizeof(get_param), &value, sizeof(value), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INVALID_PARAMETER);

    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ICMP_ECHO, &get_param, sizeof(get_param), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_NOT_SUPPORTED);
}

static void
TestDeviceInterfaces(
    _In_ HANDLE Device)
{
    struct nsiproxy_enumerate_all in;
    struct
    {
        struct nsiproxy_get_parameter header;
        BYTE key_rest[sizeof(UINT) - 1];
    } get_param;
    const UINT rows = 32;
    BYTE *out;
    DWORD out_size, returned, err, count, i;
    const NET_LUID *luids;
    const struct nsi_ndis_ifinfo_static *stat;
    MIB_IFTABLE *table = NULL;
    ULONG table_size = 0;

    memset(&in, 0, sizeof(in));
    in.module = NPI_MS_NDIS_MODULEID;
    in.table = NSI_NDIS_IFINFO_TABLE;
    in.key_size = sizeof(NET_LUID);
    in.static_size = sizeof(struct nsi_ndis_ifinfo_static);
    in.count = rows;
    out_size = sizeof(DWORD) + rows * (in.key_size + in.static_size);
    out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, out_size);
    if (!skip(out != NULL, "no buffer for the interface table\n"))
    {
        err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, out_size, &returned);
        if (!skip(err != ERROR_NOT_FOUND, "tcpip does not provide the NDIS interface table\n"))
        {
            ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
            count = *(DWORD *)out;
            ok(count >= 1 && count <= rows, "interface table has %lu rows\n", count);
            luids = (const NET_LUID *)(out + sizeof(DWORD));
            stat = (const struct nsi_ndis_ifinfo_static *)(out + sizeof(DWORD) + rows * in.key_size);
            for (i = 0; i < count && i < rows; i++)
            {
                NET_LUID luid;

                ok(stat[i].if_index != 0, "row %lu has interface index 0\n", i);
                ok_eq_ulong((ULONG)luids[i].Info.NetLuidIndex, (ULONG)stat[i].if_index);
                ok_eq_ulong((ULONG)luids[i].Info.IfType, (ULONG)stat[i].type);
                ok(stat[i].descr.Length != 0, "row %lu has no description\n", i);
                trace("interface %u type %u luid %I64x\n", stat[i].if_index, stat[i].type, luids[i].Value);

                memset(&get_param, 0, sizeof(get_param));
                get_param.header.module = NPI_MS_NDIS_MODULEID;
                get_param.header.table = NSI_NDIS_INDEX_LUID_TABLE;
                get_param.header.key_size = sizeof(UINT);
                get_param.header.param_type = NSI_PARAM_TYPE_STATIC;
                memcpy(get_param.header.key, &stat[i].if_index, sizeof(UINT));
                luid.Value = 0;
                err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_PARAMETER, &get_param, sizeof(get_param), &luid, sizeof(luid), &returned);
                ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
                ok(luid.Value == luids[i].Value, "index %u maps to luid %I64x, enumerated %I64x\n", stat[i].if_index, luid.Value, luids[i].Value);
            }
            if (GetIfTable(NULL, &table_size, FALSE) == ERROR_INSUFFICIENT_BUFFER)
                table = HeapAlloc(GetProcessHeap(), 0, table_size);
            if (table != NULL && GetIfTable(table, &table_size, FALSE) == NO_ERROR)
                ok_eq_ulong(count, table->dwNumEntries);
            else
                skip(FALSE, "GetIfTable unavailable for the row count comparison\n");
            if (table != NULL)
                HeapFree(GetProcessHeap(), 0, table);
        }
        HeapFree(GetProcessHeap(), 0, out);
    }
}

typedef DWORD (WINAPI *PFN_NSI_ENUMERATE)(DWORD, DWORD, const NPI_MODULEID *, DWORD, void *, DWORD, void *, DWORD,
                                          void *, DWORD, void *, DWORD, DWORD *);
typedef DWORD (WINAPI *PFN_NSI_GET_PARAMETER)(DWORD, const NPI_MODULEID *, DWORD, const void *, DWORD, DWORD, void *, DWORD, DWORD);

static void
TestApiBackend(
    _In_ HANDLE Device)
{
    HMODULE nsi = LoadLibraryW(L"nsi.dll");
    PFN_NSI_ENUMERATE pNsiEnumerateObjectsAllParameters;
    PFN_NSI_GET_PARAMETER pNsiGetParameter;
    struct nsiproxy_enumerate_all in;
    const DWORD rows = 32;
    NET_LUID *api_keys = NULL, *dev_keys;
    struct nsi_ndis_ifinfo_static *api_stat = NULL, *dev_stat;
    BYTE *out = NULL;
    DWORD out_size, returned, err, api_count = rows, dev_count, i, j, matched = 0;

    if (skip(nsi != NULL, "nsi.dll not loadable: %lu\n", GetLastError()))
        return;
    pNsiEnumerateObjectsAllParameters = (PFN_NSI_ENUMERATE)GetProcAddress(nsi, "NsiEnumerateObjectsAllParameters");
    pNsiGetParameter = (PFN_NSI_GET_PARAMETER)GetProcAddress(nsi, "NsiGetParameter");
    ok(pNsiEnumerateObjectsAllParameters != NULL && pNsiGetParameter != NULL, "nsi.dll exports missing\n");
    api_keys = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, rows * sizeof(*api_keys));
    api_stat = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, rows * sizeof(*api_stat));
    memset(&in, 0, sizeof(in));
    in.module = NPI_MS_NDIS_MODULEID;
    in.table = NSI_NDIS_IFINFO_TABLE;
    in.key_size = sizeof(NET_LUID);
    in.static_size = sizeof(struct nsi_ndis_ifinfo_static);
    in.count = rows;
    out_size = sizeof(DWORD) + rows * (in.key_size + in.static_size);
    out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, out_size);
    if (pNsiEnumerateObjectsAllParameters != NULL && pNsiGetParameter != NULL && api_keys != NULL && api_stat != NULL && out != NULL)
    {
        err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, out_size, &returned);
        if (!skip(err == ERROR_SUCCESS, "device interface table: %lu\n", err))
        {
            dev_count = *(DWORD *)out;
            dev_keys = (NET_LUID *)(out + sizeof(DWORD));
            dev_stat = (struct nsi_ndis_ifinfo_static *)(out + sizeof(DWORD) + rows * in.key_size);
            err = pNsiEnumerateObjectsAllParameters(1, 1, &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE,
                                                    api_keys, sizeof(*api_keys), NULL, 0, NULL, 0,
                                                    api_stat, sizeof(*api_stat), &api_count);
            ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
            ok_eq_ulong(api_count, dev_count);
            for (i = 0; i < api_count && i < rows; i++)
            {
                NET_LUID luid;

                for (j = 0; j < dev_count && j < rows; j++)
                {
                    if (dev_keys[j].Value != api_keys[i].Value)
                        continue;
                    matched++;
                    ok_eq_ulong((ULONG)api_stat[i].if_index, (ULONG)dev_stat[j].if_index);
                    ok_eq_ulong((ULONG)api_stat[i].type, (ULONG)dev_stat[j].type);
                    ok(IsEqualGUID(&api_stat[i].if_guid, &dev_stat[j].if_guid), "interface %u: guid differs between api and device\n", api_stat[i].if_index);
                    ok(api_stat[i].descr.Length != 0, "interface %u: empty description through the api\n", api_stat[i].if_index);
                    trace("interface %u api descr %ls / device descr %ls\n", api_stat[i].if_index, api_stat[i].descr.String, dev_stat[j].descr.String);
                }
                luid.Value = 0;
                err = pNsiGetParameter(1, &NPI_MS_NDIS_MODULEID, NSI_NDIS_INDEX_LUID_TABLE, &api_stat[i].if_index, sizeof(UINT),
                                       NSI_PARAM_TYPE_STATIC, &luid, sizeof(luid), 0);
                ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
                ok(luid.Value == api_keys[i].Value, "api index %u maps to luid %I64x, enumerated %I64x\n", api_stat[i].if_index, luid.Value, api_keys[i].Value);
            }
            ok(matched == api_count, "%lu of %lu api rows found on the device\n", matched, api_count);

            if (!skip(dev_count >= 2, "need two interfaces for the short-buffer check, have %lu\n", dev_count))
            {
                BYTE *guard = (BYTE *)&api_stat[1];
                DWORD short_count = 1;

                memset(api_keys, 0xCD, 2 * sizeof(*api_keys));
                memset(api_stat, 0xCD, 2 * sizeof(*api_stat));
                err = pNsiEnumerateObjectsAllParameters(1, 1, &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE,
                                                        api_keys, sizeof(*api_keys), NULL, 0, NULL, 0,
                                                        api_stat, sizeof(*api_stat), &short_count);
                ok_eq_ulong(err, (DWORD)ERROR_MORE_DATA);
                ok_eq_ulong(short_count, dev_count);
                ok(api_keys[0].Value != 0xCDCDCDCDCDCDCDCDULL, "first key not written on a short enumerate\n");
                ok(api_keys[1].Value == 0xCDCDCDCDCDCDCDCDULL, "second key written although only one row fit\n");
                for (i = 0; i < sizeof(*api_stat); i++)
                {
                    if (guard[i] != 0xCD)
                        break;
                }
                ok(i == sizeof(*api_stat), "second static row written at offset %lu although only one row fit\n", i);
            }
        }
    }
    if (out != NULL)
        HeapFree(GetProcessHeap(), 0, out);
    if (api_stat != NULL)
        HeapFree(GetProcessHeap(), 0, api_stat);
    if (api_keys != NULL)
        HeapFree(GetProcessHeap(), 0, api_keys);
    FreeLibrary(nsi);
}

static DWORD
ArmNotification(
    _In_ HANDLE Device,
    _In_ const NPI_MODULEID *Module,
    _In_ UINT Table,
    _Inout_ OVERLAPPED *Overlapped)
{
    struct nsiproxy_request_notification in;
    DWORD returned;

    memset(&in, 0, sizeof(in));
    in.module = *Module;
    in.table = Table;
    ResetEvent(Overlapped->hEvent);
    if (DeviceIoControl(Device, IOCTL_NSIPROXY_WINE_CHANGE_NOTIFICATION, &in, sizeof(in), NULL, 0, &returned, Overlapped))
        return ERROR_SUCCESS;
    return GetLastError();
}

static void
TestDeviceNotifications(
    _In_ const NPI_MODULEID *Module)
{
    HANDLE device = OpenNsiDevice(FILE_FLAG_OVERLAPPED);
    OVERLAPPED ovl;
    DWORD err, bytes, first;

    ok(device != INVALID_HANDLE_VALUE, "overlapped open of the device: %lu\n", GetLastError());
    if (device == INVALID_HANDLE_VALUE)
        return;
    memset(&ovl, 0, sizeof(ovl));
    ovl.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    err = ArmNotification(device, Module, NSISTORE_TABLE, &ovl);
    ok_eq_ulong(err, (DWORD)ERROR_IO_PENDING);
    ok(WaitForSingleObject(ovl.hEvent, 300) == WAIT_TIMEOUT, "notification fired without a change\n");
    ok(KmtSendToDriver(IOCTL_NSISTORE_NOTIFY) == ERROR_SUCCESS, "notify request to the test driver failed\n");
    ok(WaitForSingleObject(ovl.hEvent, 5000) == WAIT_OBJECT_0, "notification not delivered after a change\n");
    bytes = 0;
    ok(GetOverlappedResult(device, &ovl, &bytes, FALSE), "GetOverlappedResult: %lu\n", GetLastError());
    ok(bytes != 0, "sequence is 0 after a change\n");
    first = bytes;

    err = ArmNotification(device, Module, NSISTORE_TABLE, &ovl);
    ok_eq_ulong(err, (DWORD)ERROR_IO_PENDING);
    ok(KmtSendToDriver(IOCTL_NSISTORE_NOTIFY) == ERROR_SUCCESS, "second notify request failed\n");
    ok(WaitForSingleObject(ovl.hEvent, 5000) == WAIT_OBJECT_0, "second notification not delivered\n");
    bytes = 0;
    ok(GetOverlappedResult(device, &ovl, &bytes, FALSE), "second GetOverlappedResult: %lu\n", GetLastError());
    ok_eq_ulong(bytes, first + 1);

    err = ArmNotification(device, Module, NSISTORE_TABLE, &ovl);
    ok_eq_ulong(err, (DWORD)ERROR_IO_PENDING);
    ok(CancelIoEx(device, &ovl), "CancelIoEx: %lu\n", GetLastError());
    ok(WaitForSingleObject(ovl.hEvent, 5000) == WAIT_OBJECT_0, "cancelled request not completed\n");
    ok(!GetOverlappedResult(device, &ovl, &bytes, FALSE), "cancelled request completed successfully\n");
    ok_eq_ulong(GetLastError(), (DWORD)ERROR_OPERATION_ABORTED);

    err = ArmNotification(device, Module, NSISTORE_SCRATCH_TABLE, &ovl);
    ok_eq_ulong(err, (DWORD)ERROR_NOT_FOUND);

    err = ArmNotification(device, Module, NSISTORE_TABLE, &ovl);
    ok_eq_ulong(err, (DWORD)ERROR_IO_PENDING);
    CloseHandle(device);
    ok(WaitForSingleObject(ovl.hEvent, 5000) == WAIT_OBJECT_0, "request not completed by closing the handle\n");
    ok_eq_hex((ULONG)ovl.Internal, 0xC0000120UL);
    CloseHandle(ovl.hEvent);
}

START_TEST(NsiStore)
{
    NPI_MODULEID module;
    HANDLE device;
    DWORD error;

    error = KmtLoadAndOpenDriver(L"NsiStore", FALSE);
    ok_eq_ulong(error, (DWORD)ERROR_SUCCESS);
    if (error)
        return;
    ok(KmtSendToDriver(IOCTL_NSISTORE_RUN) == ERROR_SUCCESS, "kernel half failed\n");

    device = OpenNsiDevice(0);
    if (!skip(device != INVALID_HANDLE_VALUE, "no \\\\.\\Nsi device: %lu\n", GetLastError()))
    {
        memset(&module, 0, sizeof(module));
        module.Length = sizeof(module);
        module.Type = MIT_GUID;
        module.Guid = StoreTestGuid;
        TestDeviceEnumerate(device, &module);
        TestDeviceRows(device, &module);
        TestDeviceInterfaces(device);
        TestApiBackend(device);
        CloseHandle(device);
        TestDeviceNotifications(&module);
    }

    KmtCloseDriver();
    KmtUnloadDriver();
}
