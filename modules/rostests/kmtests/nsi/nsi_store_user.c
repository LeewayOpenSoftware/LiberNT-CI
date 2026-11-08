/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Network store (NSI) provider and table access test, user half
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>
#include <winsock2.h>
#include <ws2ipdef.h>
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
        CloseHandle(device);
    }

    KmtCloseDriver();
    KmtUnloadDriver();
}
