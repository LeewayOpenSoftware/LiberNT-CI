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

static
NTSTATUS
SetAttribute(HANDLE Handle, ULONG Code, PCSTR Name, const VOID *Value, ULONG ValueLength)
{
    PUCHAR Buffer;
    ULONG NameSize = (ULONG)strlen(Name) + 1;
    NTSTATUS Status;

    Buffer = HeapAlloc(GetProcessHeap(), 0, NameSize + ValueLength);
    if (!Buffer)
        return STATUS_NO_MEMORY;
    RtlCopyMemory(Buffer, Name, NameSize);
    if (ValueLength)
        RtlCopyMemory(Buffer + NameSize, Value, ValueLength);
    Status = Fsctl(Handle, Code, Buffer, NameSize + ValueLength, NULL, 0, NULL);
    HeapFree(GetProcessHeap(), 0, Buffer);
    return Status;
}

static
NTSTATUS
GetAttribute(HANDLE Handle, ULONG Code, PCSTR Name, PVOID Output, ULONG OutputLength, PULONG_PTR Information)
{
    return Fsctl(Handle, Code, (PVOID)Name, (ULONG)strlen(Name) + 1, Output, OutputLength, Information);
}

static
VOID
TestAttributeRules(PCSTR Kind, HANDLE Handle, ULONG GetCode, ULONG SetCode)
{
    static const UCHAR Value[] = { 1, 2, 3, 4 };
    static const UCHAR Short = 5;
    UCHAR Buffer[64];
    PUCHAR Large;
    ULONG_PTR Information;

    ok(SetAttribute(Handle, SetCode, "A", Value, sizeof(Value)) == STATUS_SUCCESS, "%s: set A failed\n", Kind);
    memset(Buffer, 0xCC, sizeof(Buffer));
    ok(GetAttribute(Handle, GetCode, "A", Buffer, sizeof(Buffer), &Information) == STATUS_SUCCESS, "%s: get A\n", Kind);
    ok(Information == sizeof(Value) && !memcmp(Buffer, Value, sizeof(Value)), "%s: A has %lu bytes\n", Kind, (ULONG)Information);
    ok(GetAttribute(Handle, GetCode, "a", Buffer, sizeof(Buffer), NULL) == STATUS_NOT_FOUND, "%s: lookup ignored case\n", Kind);
    ok(GetAttribute(Handle, GetCode, "A", Buffer, sizeof(Value), &Information) == STATUS_SUCCESS, "%s: exact output\n", Kind);
    memset(Buffer, 0xCC, sizeof(Buffer));
    ok(GetAttribute(Handle, GetCode, "A", Buffer, sizeof(Value) - 1, &Information) == STATUS_BUFFER_TOO_SMALL,
       "%s: short output accepted\n", Kind);
    ok(Information == 0 && Buffer[0] == 0xCC, "%s: short output written\n", Kind);
    ok(GetAttribute(Handle, GetCode, "A", NULL, 0, NULL) == STATUS_BUFFER_TOO_SMALL, "%s: no output accepted\n", Kind);
    ok(Fsctl(Handle, GetCode, "A", 1, Buffer, sizeof(Buffer), NULL) == STATUS_INVALID_PARAMETER,
       "%s: name without NUL accepted\n", Kind);
    ok(Fsctl(Handle, GetCode, NULL, 0, Buffer, sizeof(Buffer), NULL) == STATUS_INVALID_PARAMETER,
       "%s: empty get input accepted\n", Kind);
    ok(GetAttribute(Handle, GetCode, "", Buffer, sizeof(Buffer), NULL) == STATUS_NOT_FOUND, "%s: empty name found\n", Kind);

    ok(SetAttribute(Handle, SetCode, "A", &Short, 1) == STATUS_SUCCESS, "%s: replace A failed\n", Kind);
    ok(GetAttribute(Handle, GetCode, "A", Buffer, sizeof(Buffer), &Information) == STATUS_SUCCESS &&
       Information == 1 && Buffer[0] == Short, "%s: A not replaced\n", Kind);
    ok(SetAttribute(Handle, SetCode, "A", NULL, 0) == STATUS_SUCCESS, "%s: delete A failed\n", Kind);
    ok(GetAttribute(Handle, GetCode, "A", Buffer, sizeof(Buffer), NULL) == STATUS_NOT_FOUND, "%s: A not deleted\n", Kind);
    ok(SetAttribute(Handle, SetCode, "B", NULL, 0) == STATUS_SUCCESS, "%s: delete of a missing name failed\n", Kind);

    ok(Fsctl(Handle, SetCode, "C", 1, NULL, 0, NULL) == STATUS_INVALID_PARAMETER, "%s: set without NUL accepted\n", Kind);
    ok(SetAttribute(Handle, SetCode, "", Value, 1) == STATUS_INVALID_PARAMETER, "%s: empty name accepted\n", Kind);
    ok(Fsctl(Handle, SetCode, NULL, 0, NULL, 0, NULL) == STATUS_INVALID_PARAMETER, "%s: empty set input accepted\n", Kind);

    Large = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 65537);
    if (Large)
    {
        Large[65536] = 0x5A;
        ok(SetAttribute(Handle, SetCode, "E", Large, 65537) == STATUS_SUCCESS, "%s: 65537-byte value refused\n", Kind);
        Large[65536] = 0;
        ok(GetAttribute(Handle, GetCode, "E", Large, 65537, &Information) == STATUS_SUCCESS &&
           Information == 65537 && Large[65536] == 0x5A, "%s: 65537-byte value lost\n", Kind);
        ok(SetAttribute(Handle, SetCode, "E", NULL, 0) == STATUS_SUCCESS, "%s: delete E failed\n", Kind);
        HeapFree(GetProcessHeap(), 0, Large);
    }
}

typedef BOOL (WINAPI *FN_GetNamedPipeServerProcessId)(HANDLE, PULONG);

static
VOID
TestPipeAttributes(VOID)
{
    static const UCHAR Value[] = { 7, 8 };
    FN_GetNamedPipeServerProcessId pGetNamedPipeServerProcessId;
    HANDLE Server, Server2, Client, Duplicate;
    UCHAR Buffer[16];
    ULONG Pid, One = 1;
    ULONG_PTR Information;

    Server = CreateServer(PIPE_NAME L"_pipe");
    Server2 = CreateServer(PIPE_NAME L"_pipe");
    Client = OpenClient(PIPE_NAME L"_pipe", GENERIC_READ | GENERIC_WRITE);
    ok(Server != INVALID_HANDLE_VALUE && Server2 != INVALID_HANDLE_VALUE && Client != INVALID_HANDLE_VALUE,
       "pipe: %lu\n", GetLastError());
    ok(DuplicateHandle(GetCurrentProcess(), Server, GetCurrentProcess(), &Duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS),
       "DuplicateHandle failed\n");

    TestAttributeRules("pipe", Server, FSCTL_PIPE_GET_PIPE_ATTRIBUTE, FSCTL_PIPE_SET_PIPE_ATTRIBUTE);

    ok_ntstatus(SetAttribute(Server, FSCTL_PIPE_SET_PIPE_ATTRIBUTE, "V", Value, sizeof(Value)), STATUS_SUCCESS);
    ok_ntstatus(GetAttribute(Client, FSCTL_PIPE_GET_PIPE_ATTRIBUTE, "V", Buffer, sizeof(Buffer), &Information), STATUS_SUCCESS);
    ok_eq_ulong((ULONG)Information, (ULONG)sizeof(Value));
    ok_ntstatus(GetAttribute(Duplicate, FSCTL_PIPE_GET_PIPE_ATTRIBUTE, "V", Buffer, sizeof(Buffer), NULL), STATUS_SUCCESS);
    ok_ntstatus(GetAttribute(Server2, FSCTL_PIPE_GET_PIPE_ATTRIBUTE, "V", Buffer, sizeof(Buffer), NULL), STATUS_SUCCESS);
    ok_ntstatus(SetAttribute(Client, FSCTL_PIPE_SET_PIPE_ATTRIBUTE, "W", Value, sizeof(Value)), STATUS_ACCESS_DENIED);
    ok_ntstatus(GetAttribute(Server, FSCTL_PIPE_GET_PIPE_ATTRIBUTE, "W", Buffer, sizeof(Buffer), NULL), STATUS_NOT_FOUND);

    ok_ntstatus(SetAttribute(Server, FSCTL_PIPE_SET_PIPE_ATTRIBUTE, "ServerProcessId", &One, sizeof(One)), STATUS_ACCESS_DENIED);
    ok_ntstatus(SetAttribute(Server, FSCTL_PIPE_SET_PIPE_ATTRIBUTE, "ServerSessionId", &One, sizeof(One)), STATUS_ACCESS_DENIED);
    Pid = 0;
    ok_ntstatus(GetAttribute(Server, FSCTL_PIPE_GET_PIPE_ATTRIBUTE, "ServerProcessId", &Pid, sizeof(Pid), NULL), STATUS_SUCCESS);
    ok_eq_ulong(Pid, GetCurrentProcessId());
    Pid = 0;
    ok_ntstatus(GetAttribute(Client, FSCTL_PIPE_GET_PIPE_ATTRIBUTE, "ServerProcessId", &Pid, sizeof(Pid), NULL), STATUS_SUCCESS);
    ok_eq_ulong(Pid, GetCurrentProcessId());
    pGetNamedPipeServerProcessId = (FN_GetNamedPipeServerProcessId)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                                  "GetNamedPipeServerProcessId");
    ok(pGetNamedPipeServerProcessId != NULL, "GetNamedPipeServerProcessId is not exported\n");
    Pid = 0;
    if (pGetNamedPipeServerProcessId)
        ok(pGetNamedPipeServerProcessId(Client, &Pid), "GetNamedPipeServerProcessId failed: %lu\n", GetLastError());
    ok_eq_ulong(Pid, GetCurrentProcessId());
    ok_ntstatus(SetAttribute(Server, FSCTL_PIPE_SET_PIPE_ATTRIBUTE, "ClientProcessId", &One, sizeof(One)), STATUS_SUCCESS);
    Pid = 0;
    ok_ntstatus(GetAttribute(Server, FSCTL_PIPE_GET_PIPE_ATTRIBUTE, "ClientProcessId", &Pid, sizeof(Pid), NULL), STATUS_SUCCESS);
    ok_eq_ulong(Pid, One);

    ok(DisconnectNamedPipe(Server), "DisconnectNamedPipe failed: %lu\n", GetLastError());
    ok_ntstatus(GetAttribute(Server, FSCTL_PIPE_GET_PIPE_ATTRIBUTE, "V", Buffer, sizeof(Buffer), NULL), STATUS_SUCCESS);

    CloseHandle(Duplicate);
    CloseHandle(Client);
    CloseHandle(Server2);
    CloseHandle(Server);
}

static
VOID
TestConnectionAttributes(VOID)
{
    static const UCHAR Value[] = { 7, 8 };
    HANDLE Server, Server2, Client, Duplicate;
    UCHAR Buffer[16];
    ULONG Pid, One = 1;
    ULONG_PTR Information;

    Server = CreateServer(PIPE_NAME L"_conn");
    Server2 = CreateServer(PIPE_NAME L"_conn");
    Client = OpenClient(PIPE_NAME L"_conn", GENERIC_READ | GENERIC_WRITE);
    ok(Server != INVALID_HANDLE_VALUE && Server2 != INVALID_HANDLE_VALUE && Client != INVALID_HANDLE_VALUE,
       "pipe: %lu\n", GetLastError());
    ok(DuplicateHandle(GetCurrentProcess(), Server, GetCurrentProcess(), &Duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS),
       "DuplicateHandle failed\n");

    TestAttributeRules("connection", Server, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE, FSCTL_PIPE_SET_CONNECTION_ATTRIBUTE);

    ok_ntstatus(SetAttribute(Server, FSCTL_PIPE_SET_CONNECTION_ATTRIBUTE, "V", Value, sizeof(Value)), STATUS_SUCCESS);
    ok_ntstatus(GetAttribute(Client, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE, "V", Buffer, sizeof(Buffer), &Information),
                STATUS_SUCCESS);
    ok_eq_ulong((ULONG)Information, (ULONG)sizeof(Value));
    ok_ntstatus(GetAttribute(Duplicate, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE, "V", Buffer, sizeof(Buffer), NULL), STATUS_SUCCESS);
    ok_ntstatus(GetAttribute(Server2, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE, "V", Buffer, sizeof(Buffer), NULL), STATUS_NOT_FOUND);
    ok_ntstatus(SetAttribute(Client, FSCTL_PIPE_SET_CONNECTION_ATTRIBUTE, "W", Value, sizeof(Value)), STATUS_ACCESS_DENIED);

    ok_ntstatus(SetAttribute(Server, FSCTL_PIPE_SET_CONNECTION_ATTRIBUTE, "ClientProcessId", &One, sizeof(One)),
                STATUS_ACCESS_DENIED);
    ok_ntstatus(SetAttribute(Server, FSCTL_PIPE_SET_CONNECTION_ATTRIBUTE, "ClientSessionId", &One, sizeof(One)),
                STATUS_ACCESS_DENIED);
    ok_ntstatus(SetAttribute(Server, FSCTL_PIPE_SET_CONNECTION_ATTRIBUTE, "ClientComputerName", &One, sizeof(One)),
                STATUS_ACCESS_DENIED);
    Pid = 0;
    ok_ntstatus(GetAttribute(Server, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE, "ClientProcessId", &Pid, sizeof(Pid), NULL),
                STATUS_SUCCESS);
    ok_eq_ulong(Pid, GetCurrentProcessId());
    ok_ntstatus(GetAttribute(Server, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE, "ClientComputerName", Buffer, sizeof(Buffer), NULL),
                STATUS_NOT_FOUND);
    ok_ntstatus(GetAttribute(Server, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE, "ServerProcessId", &Pid, sizeof(Pid), NULL),
                STATUS_NOT_FOUND);
    ok_ntstatus(SetAttribute(Server, FSCTL_PIPE_SET_CONNECTION_ATTRIBUTE, "ServerProcessId", &One, sizeof(One)),
                STATUS_SUCCESS);
    Pid = 0;
    ok_ntstatus(GetAttribute(Server, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE, "ServerProcessId", &Pid, sizeof(Pid), NULL),
                STATUS_SUCCESS);
    ok_eq_ulong(Pid, One);
    ok_ntstatus(GetAttribute(Server2, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE, "ClientProcessId", &Pid, sizeof(Pid), NULL),
                STATUS_NOT_FOUND);

    CloseHandle(Client);
    ok_ntstatus(GetAttribute(Server, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE, "V", Buffer, sizeof(Buffer), NULL), STATUS_SUCCESS);
    ok(DisconnectNamedPipe(Server), "DisconnectNamedPipe failed: %lu\n", GetLastError());
    ok_ntstatus(GetAttribute(Server, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE, "V", Buffer, sizeof(Buffer), NULL), STATUS_NOT_FOUND);

    CloseHandle(Duplicate);
    CloseHandle(Server2);
    CloseHandle(Server);
}

static
VOID
TestHandleAttributes(VOID)
{
    static const UCHAR Value[] = { 7, 8 };
    HANDLE Server, Server2, Client, Duplicate, AttributesOnly;
    UCHAR Buffer[16];
    ULONG Pid, One = 1;
    ULONG_PTR Information;

    Server = CreateServer(PIPE_NAME L"_handle");
    Server2 = CreateServer(PIPE_NAME L"_handle");
    Client = OpenClient(PIPE_NAME L"_handle", GENERIC_READ | GENERIC_WRITE);
    AttributesOnly = OpenClient(PIPE_NAME L"_handle", FILE_READ_ATTRIBUTES);
    ok(Server != INVALID_HANDLE_VALUE && Server2 != INVALID_HANDLE_VALUE && Client != INVALID_HANDLE_VALUE &&
       AttributesOnly != INVALID_HANDLE_VALUE, "pipe: %lu\n", GetLastError());
    ok(DuplicateHandle(GetCurrentProcess(), Server, GetCurrentProcess(), &Duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS),
       "DuplicateHandle failed\n");

    TestAttributeRules("handle", Server, FSCTL_PIPE_GET_HANDLE_ATTRIBUTE, FSCTL_PIPE_SET_HANDLE_ATTRIBUTE);

    ok_ntstatus(SetAttribute(Server, FSCTL_PIPE_SET_HANDLE_ATTRIBUTE, "V", Value, sizeof(Value)), STATUS_SUCCESS);
    ok_ntstatus(GetAttribute(Duplicate, FSCTL_PIPE_GET_HANDLE_ATTRIBUTE, "V", Buffer, sizeof(Buffer), &Information),
                STATUS_SUCCESS);
    ok_eq_ulong((ULONG)Information, (ULONG)sizeof(Value));
    ok_ntstatus(GetAttribute(Client, FSCTL_PIPE_GET_HANDLE_ATTRIBUTE, "V", Buffer, sizeof(Buffer), NULL), STATUS_NOT_FOUND);
    ok_ntstatus(GetAttribute(Server2, FSCTL_PIPE_GET_HANDLE_ATTRIBUTE, "V", Buffer, sizeof(Buffer), NULL), STATUS_NOT_FOUND);

    ok_ntstatus(SetAttribute(Client, FSCTL_PIPE_SET_HANDLE_ATTRIBUTE, "D", Value, 1), STATUS_SUCCESS);
    ok_ntstatus(GetAttribute(Client, FSCTL_PIPE_GET_HANDLE_ATTRIBUTE, "D", Buffer, sizeof(Buffer), &Information),
                STATUS_SUCCESS);
    ok_eq_ulong((ULONG)Information, 1);
    ok_ntstatus(GetAttribute(Server, FSCTL_PIPE_GET_HANDLE_ATTRIBUTE, "D", Buffer, sizeof(Buffer), NULL), STATUS_NOT_FOUND);

    ok_ntstatus(SetAttribute(AttributesOnly, FSCTL_PIPE_SET_HANDLE_ATTRIBUTE, "H", Value, 1), STATUS_SUCCESS);
    ok_ntstatus(GetAttribute(AttributesOnly, FSCTL_PIPE_GET_HANDLE_ATTRIBUTE, "H", Buffer, sizeof(Buffer), NULL),
                STATUS_SUCCESS);

    ok_ntstatus(SetAttribute(Server, FSCTL_PIPE_SET_HANDLE_ATTRIBUTE, "ClientProcessId", &One, sizeof(One)), STATUS_SUCCESS);
    Pid = 0;
    ok_ntstatus(GetAttribute(Server, FSCTL_PIPE_GET_HANDLE_ATTRIBUTE, "ClientProcessId", &Pid, sizeof(Pid), NULL),
                STATUS_SUCCESS);
    ok_eq_ulong(Pid, One);

    CloseHandle(Client);
    ok(DisconnectNamedPipe(Server), "DisconnectNamedPipe failed: %lu\n", GetLastError());
    ok_ntstatus(GetAttribute(Server, FSCTL_PIPE_GET_HANDLE_ATTRIBUTE, "V", Buffer, sizeof(Buffer), NULL), STATUS_SUCCESS);

    CloseHandle(AttributesOnly);
    CloseHandle(Duplicate);
    CloseHandle(Server2);
    CloseHandle(Server);
}

START_TEST(NamedPipeFsctl)
{
    TestAccessBits();
    TestQueryClientProcessV2();
    TestPipeAttributes();
    TestConnectionAttributes();
    TestHandleAttributes();
}
