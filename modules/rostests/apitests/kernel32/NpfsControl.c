/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     NPFS XS batch: connection attributes while listening, pipe flush, silo/symlink rejections
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include "precomp.h"
#include <ndk/iofuncs.h>

#define TEST_PIPE_DEVICE_NAMED_PIPE 0x11
#define RQMV_CTL_CODE(fn, method, access) \
    ((TEST_PIPE_DEVICE_NAMED_PIPE << 16) | ((access) << 14) | ((fn) << 2) | (method))

#define RQMV_GET_CONNECTION_ATTRIBUTE RQMV_CTL_CODE(12, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RQMV_PIPE_FLUSH               RQMV_CTL_CODE(16, METHOD_BUFFERED, FILE_WRITE_DATA)
#define RQMV_SILO_ARRIVAL             RQMV_CTL_CODE(18, METHOD_BUFFERED, FILE_WRITE_DATA)
#define RQMV_CREATE_SYMLINK           RQMV_CTL_CODE(19, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RQMV_DELETE_SYMLINK           RQMV_CTL_CODE(20, METHOD_BUFFERED, FILE_ANY_ACCESS)

static WCHAR g_PipeName[64];

typedef NTSTATUS (NTAPI *PNT_FS_CONTROL_FILE)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK,
                                              ULONG, PVOID, ULONG, PVOID, ULONG);
static PNT_FS_CONTROL_FILE pNtFsControlFile;

static NTSTATUS
Fsctl(HANDLE Pipe, ULONG Code, PVOID InBuffer, ULONG InLength, PVOID OutBuffer, ULONG OutLength)
{
    IO_STATUS_BLOCK Iosb;

    RtlZeroMemory(&Iosb, sizeof(Iosb));
    return pNtFsControlFile(Pipe, NULL, NULL, NULL, &Iosb, Code,
                            InBuffer, InLength, OutBuffer, OutLength);
}

static NTSTATUS
GetConnectionAttribute(HANDLE Pipe, PCSTR Name, ULONG NameLength, PULONG Value)
{
    CHAR buffer[32];
    ULONG outLength = sizeof(*Value);
    NTSTATUS status;

    RtlZeroMemory(buffer, sizeof(buffer));
    memcpy(buffer, Name, NameLength);
    status = Fsctl(Pipe, RQMV_GET_CONNECTION_ATTRIBUTE, buffer, NameLength, buffer, outLength);
    if (NT_SUCCESS(status))
        memcpy(Value, buffer, sizeof(*Value));
    return status;
}

static HANDLE
CreateListeningServer(VOID)
{
    HANDLE server;

    _snwprintf(g_PipeName, _countof(g_PipeName), L"\\\\.\\pipe\\libernt_npfsctl_%08lx", GetCurrentProcessId());
    server = CreateNamedPipeW(g_PipeName,
                              PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                              PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                              1, 4096, 4096, 0, NULL);
    ok(server != INVALID_HANDLE_VALUE, "CreateNamedPipeW failed: %lu\n", GetLastError());
    return server;
}

static HANDLE
ConnectClient(VOID)
{
    HANDLE client;

    client = CreateFileW(g_PipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(client != INVALID_HANDLE_VALUE, "CreateFileW(client) failed: %lu\n", GetLastError());
    return client;
}

static VOID
TestAttributesWhileListening(HANDLE Server)
{
    static const CHAR ServerProcessId[] = "ServerProcessId";
    static const CHAR ServerSessionId[] = "ServerSessionId";
    static const CHAR ClientProcessId[] = "ClientProcessId";
    ULONG value;
    NTSTATUS status;

    /* P1: Win11 answers NOT_FOUND for every built-in connection attribute
       while the server end is still listening */
    status = GetConnectionAttribute(Server, ServerProcessId, sizeof(ServerProcessId), &value);
    ok(status == STATUS_NOT_FOUND, "listening ServerProcessId -> 0x%08lx\n", status);
    status = GetConnectionAttribute(Server, ServerSessionId, sizeof(ServerSessionId), &value);
    ok(status == STATUS_NOT_FOUND, "listening ServerSessionId -> 0x%08lx\n", status);
    status = GetConnectionAttribute(Server, ClientProcessId, sizeof(ClientProcessId), &value);
    ok(status == STATUS_NOT_FOUND, "listening ClientProcessId -> 0x%08lx\n", status);
}

static VOID
TestConnectedServerAttributes(HANDLE Server)
{
    static const CHAR ClientProcessId[] = "ClientProcessId";
    ULONG value;
    NTSTATUS status;

    /* regression guard: a connected server still answers client attributes */
    status = GetConnectionAttribute(Server, ClientProcessId, sizeof(ClientProcessId), &value);
    ok(status == STATUS_SUCCESS, "connected ServerProcessId -> 0x%08lx\n", status);
    ok(value != 0, "connected client pid is 0\n");
}

static VOID
TestPipeFlushOnServer(HANDLE Server)
{
    NTSTATUS status;

    /* P5: FSCTL_PIPE_FLUSH on the server end succeeds through the flush path */
    status = Fsctl(Server, RQMV_PIPE_FLUSH, NULL, 0, NULL, 0);
    ok(status == STATUS_SUCCESS, "server FLUSH -> 0x%08lx\n", status);
}

static VOID
TestSiloAndSymlinkRejections(HANDLE Server)
{
    BYTE input[16];
    NTSTATUS status;

    RtlZeroMemory(input, sizeof(input));

    /* P11: Win11 rejects these from user mode with STATUS_ILLEGAL_FUNCTION */
    status = Fsctl(Server, RQMV_SILO_ARRIVAL, input, sizeof(input), NULL, 0);
    ok(status == STATUS_ILLEGAL_FUNCTION, "SILO_ARRIVAL -> 0x%08lx\n", status);
    status = Fsctl(Server, RQMV_CREATE_SYMLINK, input, sizeof(input), NULL, 0);
    ok(status == STATUS_ILLEGAL_FUNCTION, "CREATE_SYMLINK -> 0x%08lx\n", status);
    status = Fsctl(Server, RQMV_DELETE_SYMLINK, input, sizeof(input), NULL, 0);
    ok(status == STATUS_ILLEGAL_FUNCTION, "DELETE_SYMLINK -> 0x%08lx\n", status);
}

START_TEST(NpfsControl)
{
    HANDLE server, client;
    HMODULE ntdll;

    ntdll = GetModuleHandleW(L"ntdll.dll");
    pNtFsControlFile = (PNT_FS_CONTROL_FILE)GetProcAddress(ntdll, "NtFsControlFile");
    if (pNtFsControlFile == NULL)
    {
        skip("NtFsControlFile unavailable\n");
        return;
    }

    server = CreateListeningServer();
    if (server == INVALID_HANDLE_VALUE)
        return;

    TestAttributesWhileListening(server);
    TestPipeFlushOnServer(server);
    TestSiloAndSymlinkRejections(server);

    client = ConnectClient();
    if (client != INVALID_HANDLE_VALUE)
    {
        TestConnectedServerAttributes(server);
        CloseHandle(client);
    }

    CloseHandle(server);
}
