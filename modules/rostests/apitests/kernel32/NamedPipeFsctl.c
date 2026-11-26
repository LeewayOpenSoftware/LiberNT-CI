/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test named pipe file system controls, rows measured on Windows 11 26100
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <ndk/iofuncs.h>

#define PIPE_NAME L"\\\\.\\pipe\\libernt_apitest_pipe_fsctl"

static
HANDLE
CreateServer(PCWSTR Name)
{
    return CreateNamedPipeW(Name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE, 4, 4096, 4096, 0, NULL);
}

static
HANDLE
OpenClient(PCWSTR Name, ACCESS_MASK Access)
{
    return CreateFileW(Name, Access, 0, NULL, OPEN_EXISTING, 0, NULL);
}

static
NTSTATUS
Fsctl(HANDLE Handle, ULONG Code, PVOID Input, ULONG InputLength, PVOID Output, ULONG OutputLength, PULONG_PTR Information)
{
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    IoStatus.Information = (ULONG_PTR)-1;
    Status = NtFsControlFile(Handle, NULL, NULL, NULL, &IoStatus, Code, Input, InputLength, Output, OutputLength);
    if (Information)
        *Information = NT_SUCCESS(Status) || NT_WARNING(Status) ? IoStatus.Information : 0;
    return Status;
}

static
VOID
TestAccessBits(VOID)
{
    HANDLE Server, Client;
    UCHAR Buffer[16];
    ULONG Value;

    Server = CreateServer(PIPE_NAME L"_ro");
    Client = OpenClient(PIPE_NAME L"_ro", GENERIC_READ | FILE_WRITE_ATTRIBUTES);
    ok(Server != INVALID_HANDLE_VALUE && Client != INVALID_HANDLE_VALUE, "read-only pipe: %lu\n", GetLastError());
    ok_ntstatus(Fsctl(Client, FSCTL_PIPE_TRANSCEIVE, Buffer, 1, Buffer, sizeof(Buffer), NULL), STATUS_ACCESS_DENIED);
    ok_ntstatus(Fsctl(Client, FSCTL_PIPE_FLUSH, NULL, 0, NULL, 0, NULL), STATUS_ACCESS_DENIED);
    ok_ntstatus(Fsctl(Client, FSCTL_PIPE_INTERNAL_WRITE, Buffer, 1, NULL, 0, NULL), STATUS_ACCESS_DENIED);
    ok_ntstatus(Fsctl(Client, FSCTL_PIPE_PEEK, NULL, 0, Buffer, sizeof(Buffer), NULL), STATUS_SUCCESS);
    CloseHandle(Client);
    CloseHandle(Server);

    Server = CreateServer(PIPE_NAME L"_wo");
    Client = OpenClient(PIPE_NAME L"_wo", GENERIC_WRITE | FILE_READ_ATTRIBUTES);
    ok(Server != INVALID_HANDLE_VALUE && Client != INVALID_HANDLE_VALUE, "write-only pipe: %lu\n", GetLastError());
    ok_ntstatus(Fsctl(Client, FSCTL_PIPE_PEEK, NULL, 0, Buffer, sizeof(Buffer), NULL), STATUS_ACCESS_DENIED);
    ok_ntstatus(Fsctl(Client, FSCTL_PIPE_INTERNAL_READ, NULL, 0, Buffer, sizeof(Buffer), NULL), STATUS_ACCESS_DENIED);
    ok_ntstatus(Fsctl(Client, FSCTL_PIPE_TRANSCEIVE, Buffer, 1, Buffer, sizeof(Buffer), NULL), STATUS_ACCESS_DENIED);
    ok_ntstatus(Fsctl(Client, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE, "ClientProcessId", sizeof("ClientProcessId"),
                      &Value, sizeof(Value), NULL), STATUS_SUCCESS);
    ok_eq_ulong(Value, GetCurrentProcessId());
    CloseHandle(Client);
    CloseHandle(Server);

    Server = CreateServer(PIPE_NAME L"_ao");
    Client = OpenClient(PIPE_NAME L"_ao", FILE_READ_ATTRIBUTES);
    ok(Server != INVALID_HANDLE_VALUE && Client != INVALID_HANDLE_VALUE, "attributes-only pipe: %lu\n", GetLastError());
    ok_ntstatus(Fsctl(Client, FSCTL_PIPE_PEEK, NULL, 0, Buffer, sizeof(Buffer), NULL), STATUS_ACCESS_DENIED);
    ok_ntstatus(Fsctl(Client, FSCTL_PIPE_FLUSH, NULL, 0, NULL, 0, NULL), STATUS_ACCESS_DENIED);
    CloseHandle(Client);
    CloseHandle(Server);
}

static
VOID
TestQueryClientProcessV2(VOID)
{
    static const ULONG BadLengths[] = { 0, 8, 15, 17, 24, 64 };
    FILE_PIPE_CLIENT_PROCESS_BUFFER V1;
    FILE_PIPE_CLIENT_PROCESS_BUFFER_V2 V2;
    UCHAR Buffer[64];
    HANDLE Server, Client, Handles[2];
    ULONG_PTR Information;
    ULONG Index;

    Server = CreateServer(PIPE_NAME L"_v2");
    Client = OpenClient(PIPE_NAME L"_v2", GENERIC_READ | GENERIC_WRITE);
    ok(Server != INVALID_HANDLE_VALUE && Client != INVALID_HANDLE_VALUE, "pipe: %lu\n", GetLastError());

    for (Index = 0; Index < RTL_NUMBER_OF(BadLengths); Index++)
    {
        ok(Fsctl(Server, FSCTL_PIPE_QUERY_CLIENT_PROCESS_V2, NULL, 0, Buffer, BadLengths[Index], NULL) ==
           STATUS_INVALID_PARAMETER, "output length %lu accepted\n", BadLengths[Index]);
    }

    Handles[0] = Server;
    Handles[1] = Client;
    for (Index = 0; Index < RTL_NUMBER_OF(Handles); Index++)
    {
        memset(&V2, 0xCC, sizeof(V2));
        ok_ntstatus(Fsctl(Handles[Index], FSCTL_PIPE_QUERY_CLIENT_PROCESS_V2, NULL, 0, &V2, sizeof(V2), &Information),
                    STATUS_SUCCESS);
        ok_eq_ulong((ULONG)Information, (ULONG)sizeof(V2));
        ok(V2.ClientSession == 0, "end %lu: ClientSession 0x%I64x\n", Index, V2.ClientSession);
        ok_ntstatus(Fsctl(Handles[Index], FSCTL_PIPE_QUERY_CLIENT_PROCESS, NULL, 0, &V1, sizeof(V1), NULL),
                    STATUS_SUCCESS);
        ok(V2.ClientProcess == V1.ClientProcess, "end %lu: V2 process %p, V1 process %p\n",
           Index, V2.ClientProcess, V1.ClientProcess);
    }

    CloseHandle(Client);
    CloseHandle(Server);
}

START_TEST(NamedPipeFsctl)
{
    TestAccessBits();
    TestQueryClientProcessV2();
}
