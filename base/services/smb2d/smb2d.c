/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SMB2 daemon: serves the requests of the smb2rdr kernel mini-redirector
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Runs as a console process for now (smb2d.exe); the service wrapper comes with the network provider.
 * Requests are served one at a time by the thread that fetched them; a pool of worker threads is the
 * next step once more than one operation can be in flight.
 */

#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "smb2client.h"
#include "smb2rdr_ioctl.h"

/* Status values from ntstatus.h without pulling the kernel-flavoured macros */
#define ST_SUCCESS              0x00000000L
#define ST_NOT_SUPPORTED        ((LONG)0xC00000BBL)
#define ST_BAD_NETWORK_PATH     ((LONG)0xC00000BEL)

static volatile LONG g_Stop;

/* Credentials: the daemon's own identity unless SMB2D_USER / SMB2D_DOMAIN / SMB2D_PASSWORD are set.
 * Per-caller identity (impersonating the requesting token) is a later step. */
static WCHAR g_User[256], g_Domain[256], g_Password[256];
static int g_HaveUser;

/* Handle tables. The daemon serves one request at a time, so no locking is needed yet. */
typedef struct _CONNECTION
{
    struct _CONNECTION *Next;
    WCHAR Host[257];
    SMB2_CONN *Conn;
    unsigned Trees;
} CONNECTION;

typedef struct _TREE
{
    struct _TREE *Next;
    unsigned long long Handle;
    CONNECTION *Connection;
    uint32_t TreeId;
} TREE;

typedef struct _OPENFILE
{
    struct _OPENFILE *Next;
    unsigned long long Handle;
    TREE *Tree;
    SMB2_FILE_ID FileId;
} OPENFILE;

static CONNECTION *g_Connections;
static TREE *g_Trees;
static OPENFILE *g_Files;
static unsigned long long g_NextHandle = 1;

static BOOL Ioctl(HANDLE dev, DWORD code, void *in, DWORD inLen, void *out, DWORD outLen, DWORD *ret)
{
    DWORD n = 0;
    BOOL ok = DeviceIoControl(dev, code, in, inLen, out, outLen, &n, NULL);

    if (ret)
        *ret = n;
    return ok;
}

/* CONNECT: prove the server speaks SMB2 (TCP connect + NEGOTIATE), then drop the probe connection. */
static LONG DoConnect(const BYTE *payload, DWORD length)
{
    WCHAR host[257];
    SMB2_CONN *c = NULL;
    uint32_t st;

    if (length < sizeof(WCHAR) || length > 256 * sizeof(WCHAR) || (length & 1))
        return ST_BAD_NETWORK_PATH;
    memcpy(host, payload, length);
    host[length / sizeof(WCHAR)] = 0;

    st = Smb2Connect(host, 445, &c);
    if (st != SMB2_STATUS_SUCCESS)
        return (LONG)st;
    st = Smb2Negotiate(c);
    Smb2Disconnect(c);
    return (LONG)st;
}

static TREE *FindTree(unsigned long long h)
{
    TREE *t;

    for (t = g_Trees; t; t = t->Next)
        if (t->Handle == h)
            return t;
    return NULL;
}

static OPENFILE *FindFile(unsigned long long h)
{
    OPENFILE *f;

    for (f = g_Files; f; f = f->Next)
        if (f->Handle == h)
            return f;
    return NULL;
}

static CONNECTION *GetConnection(const WCHAR *host, LONG *status)
{
    CONNECTION *c;
    SMB2_CONN *sc = NULL;
    uint32_t st;

    for (c = g_Connections; c; c = c->Next)
        if (!_wcsicmp(c->Host, host))
            return c;

    st = Smb2Connect(host, 445, &sc);
    if (st == SMB2_STATUS_SUCCESS)
        st = Smb2Negotiate(sc);
    if (st == SMB2_STATUS_SUCCESS)
        st = Smb2SessionSetup(sc, host, g_HaveUser ? g_User : NULL, g_HaveUser ? g_Domain : NULL,
                              g_HaveUser ? g_Password : NULL);
    if (st != SMB2_STATUS_SUCCESS)
    {
        if (sc)
            Smb2Disconnect(sc);
        *status = (LONG)st;
        return NULL;
    }
    c = (CONNECTION *)calloc(1, sizeof(*c));
    if (!c)
    {
        Smb2Disconnect(sc);
        *status = (LONG)SMB2_STATUS_INSUFFICIENT_RESOURCES;
        return NULL;
    }
    wcsncpy(c->Host, host, 256);
    c->Conn = sc;
    c->Next = g_Connections;
    g_Connections = c;
    return c;
}

static void DropConnectionIfIdle(CONNECTION *c)
{
    CONNECTION **pp;

    if (c->Trees)
        return;
    Smb2Logoff(c->Conn);
    Smb2Disconnect(c->Conn);
    for (pp = &g_Connections; *pp; pp = &(*pp)->Next)
    {
        if (*pp == c)
        {
            *pp = c->Next;
            break;
        }
    }
    free(c);
}

static LONG DoTreeConnect(const BYTE *payload, DWORD length, BYTE *reply, DWORD *replyLen)
{
    const SMB2RDR_TREE_CONNECT_REQ *req = (const SMB2RDR_TREE_CONNECT_REQ *)payload;
    WCHAR name[600], *share;
    CONNECTION *c;
    TREE *t;
    LONG status = 0;
    uint32_t tid = 0, st;

    if (length < sizeof(*req) || req->NameLength > length - sizeof(*req) || req->NameLength < 2 * sizeof(WCHAR) ||
        req->NameLength > sizeof(name) - sizeof(WCHAR))
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    memcpy(name, payload + sizeof(*req), req->NameLength);
    name[req->NameLength / sizeof(WCHAR)] = 0;
    share = wcschr(name, L'\\');
    if (!share || share == name || !share[1])
        return (LONG)SMB2_STATUS_BAD_NETWORK_NAME;
    *share++ = 0;

    c = GetConnection(name, &status);
    if (!c)
        return status;
    st = Smb2TreeConnect(c->Conn, name, share, &tid);
    if (st != SMB2_STATUS_SUCCESS)
    {
        DropConnectionIfIdle(c);
        return (LONG)st;
    }
    t = (TREE *)calloc(1, sizeof(*t));
    if (!t)
    {
        Smb2TreeDisconnect(c->Conn, tid);
        DropConnectionIfIdle(c);
        return (LONG)SMB2_STATUS_INSUFFICIENT_RESOURCES;
    }
    t->Handle = g_NextHandle++;
    t->Connection = c;
    t->TreeId = tid;
    t->Next = g_Trees;
    g_Trees = t;
    c->Trees++;
    ((SMB2RDR_TREE_CONNECT_REPLY *)reply)->TreeHandle = t->Handle;
    *replyLen = sizeof(SMB2RDR_TREE_CONNECT_REPLY);
    return ST_SUCCESS;
}

static LONG DoTreeDisconnect(const BYTE *payload, DWORD length)
{
    const SMB2RDR_HANDLE_REQ *req = (const SMB2RDR_HANDLE_REQ *)payload;
    TREE *t, **pp;

    if (length < sizeof(*req))
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    t = FindTree(req->Handle);
    if (!t)
        return ST_SUCCESS;
    Smb2TreeDisconnect(t->Connection->Conn, t->TreeId);
    for (pp = &g_Trees; *pp; pp = &(*pp)->Next)
    {
        if (*pp == t)
        {
            *pp = t->Next;
            break;
        }
    }
    t->Connection->Trees--;
    DropConnectionIfIdle(t->Connection);
    free(t);
    return ST_SUCCESS;
}

static LONG DoOpen(const BYTE *payload, DWORD length, BYTE *reply, DWORD *replyLen)
{
    const SMB2RDR_OPEN_REQ *req = (const SMB2RDR_OPEN_REQ *)payload;
    SMB2RDR_OPEN_REPLY *rep = (SMB2RDR_OPEN_REPLY *)reply;
    WCHAR path[1024];
    SMB2_CREATE_RESULT res;
    OPENFILE *f;
    TREE *t;
    uint32_t st;

    if (length < sizeof(*req) || req->PathLength > length - sizeof(*req) || (req->PathLength & 1) ||
        req->PathLength > sizeof(path) - sizeof(WCHAR))
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    t = FindTree(req->TreeHandle);
    if (!t)
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    memcpy(path, payload + sizeof(*req), req->PathLength);
    path[req->PathLength / sizeof(WCHAR)] = 0;

    st = Smb2CreateEx(t->Connection->Conn, t->TreeId, path, req->DesiredAccess, req->ShareAccess,
                      req->FileAttributes, req->Disposition, req->CreateOptions, &res);
    if (st != SMB2_STATUS_SUCCESS)
        return (LONG)st;
    f = (OPENFILE *)calloc(1, sizeof(*f));
    if (!f)
    {
        Smb2Close(t->Connection->Conn, t->TreeId, &res.FileId);
        return (LONG)SMB2_STATUS_INSUFFICIENT_RESOURCES;
    }
    f->Handle = g_NextHandle++;
    f->Tree = t;
    f->FileId = res.FileId;
    f->Next = g_Files;
    g_Files = f;

    rep->FileHandle = f->Handle;
    rep->CreationTime = (long long)res.CreationTime;
    rep->LastAccessTime = (long long)res.LastAccessTime;
    rep->LastWriteTime = (long long)res.LastWriteTime;
    rep->ChangeTime = (long long)res.ChangeTime;
    rep->AllocationSize = (long long)res.AllocationSize;
    rep->EndOfFile = (long long)res.EndOfFile;
    rep->FileAttributes = res.Attributes;
    rep->CreateAction = res.CreateAction;
    *replyLen = sizeof(*rep);
    return ST_SUCCESS;
}

static LONG DoClose(const BYTE *payload, DWORD length)
{
    const SMB2RDR_HANDLE_REQ *req = (const SMB2RDR_HANDLE_REQ *)payload;
    OPENFILE *f, **pp;
    uint32_t st;

    if (length < sizeof(*req))
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    f = FindFile(req->Handle);
    if (!f)
        return ST_SUCCESS;
    st = Smb2Close(f->Tree->Connection->Conn, f->Tree->TreeId, &f->FileId);
    for (pp = &g_Files; *pp; pp = &(*pp)->Next)
    {
        if (*pp == f)
        {
            *pp = f->Next;
            break;
        }
    }
    free(f);
    return (LONG)st;
}

static LONG DoRead(const BYTE *payload, DWORD length, BYTE *reply, DWORD *replyLen)
{
    const SMB2RDR_READ_REQ *req = (const SMB2RDR_READ_REQ *)payload;
    OPENFILE *f;
    uint32_t n = 0, st;

    if (length < sizeof(*req) || req->Length > SMB2RDR_MAX_IO)
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    f = FindFile(req->FileHandle);
    if (!f)
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    st = Smb2Read(f->Tree->Connection->Conn, f->Tree->TreeId, &f->FileId, req->Offset, req->Length, reply, &n);
    *replyLen = n;
    return (LONG)st;
}

static LONG DoQueryDir(const BYTE *payload, DWORD length, BYTE *reply, DWORD *replyLen)
{
    const SMB2RDR_QUERYDIR_REQ *req = (const SMB2RDR_QUERYDIR_REQ *)payload;
    WCHAR pattern[260];
    OPENFILE *f;
    uint32_t n = 0, st;
    uint8_t flags = 0;

    if (length < sizeof(*req) || req->PatternLength > length - sizeof(*req) || (req->PatternLength & 1) ||
        req->PatternLength > sizeof(pattern) - sizeof(WCHAR) || req->OutputLength > SMB2RDR_MAX_IO ||
        req->InfoClass > 0xFF)
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    f = FindFile(req->FileHandle);
    if (!f)
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    memcpy(pattern, payload + sizeof(*req), req->PatternLength);
    pattern[req->PatternLength / sizeof(WCHAR)] = 0;
    if (req->Flags & SMB2RDR_QD_RESTART)
        flags |= SMB2_RESTART_SCANS;
    if (req->Flags & SMB2RDR_QD_SINGLE)
        flags |= SMB2_RETURN_SINGLE_ENTRY;
    st = Smb2QueryDirectoryRaw(f->Tree->Connection->Conn, f->Tree->TreeId, &f->FileId, (uint8_t)req->InfoClass,
                               flags, req->PatternLength ? pattern : NULL, req->OutputLength, reply, &n);
    *replyLen = n;
    return (LONG)st;
}

static LONG DoWrite(const BYTE *payload, DWORD length, BYTE *reply, DWORD *replyLen)
{
    const SMB2RDR_WRITE_REQ *req = (const SMB2RDR_WRITE_REQ *)payload;
    OPENFILE *f;
    uint32_t n = 0, st;

    if (length < sizeof(*req) || req->Length > SMB2RDR_MAX_IO || req->Length > length - sizeof(*req))
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    f = FindFile(req->FileHandle);
    if (!f)
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    st = Smb2Write(f->Tree->Connection->Conn, f->Tree->TreeId, &f->FileId, req->Offset, payload + sizeof(*req),
                   req->Length, &n);
    *(ULONG *)reply = n;
    *replyLen = sizeof(ULONG);
    return (LONG)st;
}

static LONG DoSetInfo(const BYTE *payload, DWORD length)
{
    const SMB2RDR_SETINFO_REQ *req = (const SMB2RDR_SETINFO_REQ *)payload;
    OPENFILE *f;
    DWORD want;

    if (length < sizeof(*req) || req->BufferLength > length - sizeof(*req))
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    /* Only the classes the redirector sends, each with its exact MS-FSCC size */
    switch (req->InfoClass)
    {
        case SMB2_FILE_BASIC_INFORMATION:
            want = 40;
            break;
        case SMB2_FILE_DISPOSITION_INFORMATION:
            want = 1;
            break;
        case SMB2_FILE_ALLOCATION_INFORMATION:
        case SMB2_FILE_END_OF_FILE_INFORMATION:
            want = 8;
            break;
        default:
            return ST_NOT_SUPPORTED;
    }
    if (req->BufferLength != want)
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    f = FindFile(req->FileHandle);
    if (!f)
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    return (LONG)Smb2SetInfo(f->Tree->Connection->Conn, f->Tree->TreeId, &f->FileId, (uint8_t)req->InfoClass,
                             payload + sizeof(*req), req->BufferLength);
}

static LONG DoFlush(const BYTE *payload, DWORD length)
{
    const SMB2RDR_HANDLE_REQ *req = (const SMB2RDR_HANDLE_REQ *)payload;
    OPENFILE *f;

    if (length < sizeof(*req))
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    f = FindFile(req->Handle);
    if (!f)
        return (LONG)SMB2_STATUS_INVALID_PARAMETER;
    return (LONG)Smb2Flush(f->Tree->Connection->Conn, f->Tree->TreeId, &f->FileId);
}

static void Serve(HANDLE dev)
{
    static BYTE reqbuf[sizeof(SMB2RDR_UPCALL) + SMB2RDR_MAX_PAYLOAD];
    static BYTE repbuf[sizeof(SMB2RDR_DOWNCALL) + SMB2RDR_MAX_PAYLOAD];

    while (!g_Stop)
    {
        SMB2RDR_UPCALL *up = (SMB2RDR_UPCALL *)reqbuf;
        SMB2RDR_DOWNCALL *down = (SMB2RDR_DOWNCALL *)repbuf;
        DWORD got = 0, replyLen = 0;
        LONG status;

        if (!Ioctl(dev, IOCTL_SMB2RDR_GETREQ, NULL, 0, reqbuf, sizeof(reqbuf), &got))
        {
            if (g_Stop)
                break;
            fprintf(stderr, "GETREQ failed: %lu\n", GetLastError());
            Sleep(500);
            continue;
        }
        if (got < sizeof(*up) || up->Length > got - sizeof(*up))
            continue;

        switch (up->Opcode)
        {
            case SMB2RDR_UP_PING:
                status = ST_SUCCESS;
                memcpy(repbuf + sizeof(*down), reqbuf + sizeof(*up), up->Length);
                replyLen = up->Length;
                break;
            case SMB2RDR_UP_CONNECT:
                status = DoConnect(reqbuf + sizeof(*up), up->Length);
                break;
            case SMB2RDR_UP_TREE_CONNECT:
                status = DoTreeConnect(reqbuf + sizeof(*up), up->Length, repbuf + sizeof(*down), &replyLen);
                break;
            case SMB2RDR_UP_TREE_DISCONNECT:
                status = DoTreeDisconnect(reqbuf + sizeof(*up), up->Length);
                break;
            case SMB2RDR_UP_OPEN:
                status = DoOpen(reqbuf + sizeof(*up), up->Length, repbuf + sizeof(*down), &replyLen);
                break;
            case SMB2RDR_UP_CLOSE:
                status = DoClose(reqbuf + sizeof(*up), up->Length);
                break;
            case SMB2RDR_UP_READ:
                status = DoRead(reqbuf + sizeof(*up), up->Length, repbuf + sizeof(*down), &replyLen);
                break;
            case SMB2RDR_UP_WRITE:
                status = DoWrite(reqbuf + sizeof(*up), up->Length, repbuf + sizeof(*down), &replyLen);
                break;
            case SMB2RDR_UP_SETINFO:
                status = DoSetInfo(reqbuf + sizeof(*up), up->Length);
                break;
            case SMB2RDR_UP_FLUSH:
                status = DoFlush(reqbuf + sizeof(*up), up->Length);
                break;
            case SMB2RDR_UP_QUERYDIR:
                status = DoQueryDir(reqbuf + sizeof(*up), up->Length, repbuf + sizeof(*down), &replyLen);
                break;
            default:
                status = ST_NOT_SUPPORTED;
                break;
        }

        down->Id = up->Id;
        down->Status = status;
        down->Length = replyLen;
        if (!Ioctl(dev, IOCTL_SMB2RDR_PUTREPLY, repbuf, (DWORD)sizeof(*down) + replyLen, NULL, 0, NULL))
            fprintf(stderr, "PUTREPLY failed: %lu\n", GetLastError());
    }
}

/* ---------------- start-up, console and service modes ---------------- */

#define SERVICE_NAME    L"smb2d"
#define DRIVER_NAME     L"smb2rdr"

static SERVICE_STATUS_HANDLE g_StatusHandle;
static SERVICE_STATUS g_ServiceStatus;
static HANDLE g_Device = INVALID_HANDLE_VALUE;

static void LoadIdentity(void)
{
    if (GetEnvironmentVariableW(L"SMB2D_USER", g_User, 256) > 0)
    {
        g_HaveUser = 1;
        GetEnvironmentVariableW(L"SMB2D_DOMAIN", g_Domain, 256);
        GetEnvironmentVariableW(L"SMB2D_PASSWORD", g_Password, 256);
    }
}

/* Start the kernel driver service if the device is not there yet */
static void EnsureDriverStarted(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), svc;

    if (!scm)
        return;
    svc = OpenServiceW(scm, DRIVER_NAME, SERVICE_START | SERVICE_QUERY_STATUS);
    if (svc)
    {
        if (!StartServiceW(svc, 0, NULL) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
            fprintf(stderr, "cannot start the %ls driver service: %lu\n", DRIVER_NAME, GetLastError());
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

static HANDLE OpenDevice(void)
{
    return CreateFileW(SMB2RDR_USER_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

/* Wake the serving thread and shut the redirector down. A second handle is needed: the first one is
 * busy inside the blocking GETREQ and a synchronous handle serializes its requests. */
static void RequestStop(void)
{
    HANDLE ctl;
    ULONG force = 1;

    InterlockedExchange(&g_Stop, 1);
    ctl = OpenDevice();
    if (ctl != INVALID_HANDLE_VALUE)
    {
        Ioctl(ctl, IOCTL_SMB2RDR_STOP, &force, sizeof(force), NULL, 0, NULL);
        CloseHandle(ctl);
    }
}

/* Runs until stopped. Returns 0 on a clean run. */
static int RunDaemon(void)
{
    ULONG version = SMB2RDR_PROTOCOL_VERSION;
    HANDLE dev;
    int tries;

    LoadIdentity();
    dev = OpenDevice();
    if (dev == INVALID_HANDLE_VALUE)
    {
        EnsureDriverStarted();
        for (tries = 0; tries < 20 && dev == INVALID_HANDLE_VALUE; tries++)
        {
            Sleep(500);
            dev = OpenDevice();
        }
    }
    if (dev == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "cannot open the smb2rdr device: %lu\n", GetLastError());
        return 1;
    }
    if (!Ioctl(dev, IOCTL_SMB2RDR_START, &version, sizeof(version), NULL, 0, NULL))
    {
        fprintf(stderr, "start failed: %lu\n", GetLastError());
        CloseHandle(dev);
        return 1;
    }
    g_Device = dev;
    printf("smb2d: redirector started, serving requests\n");
    Serve(dev);
    g_Device = INVALID_HANDLE_VALUE;
    CloseHandle(dev);
    return 0;
}

static BOOL WINAPI CtrlHandler(DWORD type)
{
    (void)type;
    RequestStop();
    return TRUE;
}

static void SetServiceState(DWORD state)
{
    g_ServiceStatus.dwCurrentState = state;
    g_ServiceStatus.dwControlsAccepted = (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

static DWORD WINAPI ServiceHandler(DWORD control, DWORD eventType, LPVOID eventData, LPVOID context)
{
    (void)eventType; (void)eventData; (void)context;
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
    {
        SetServiceState(SERVICE_STOP_PENDING);
        RequestStop();
        return NO_ERROR;
    }
    if (control == SERVICE_CONTROL_INTERROGATE)
    {
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI ServiceMain(DWORD argc, LPWSTR *argv)
{
    int rc;

    (void)argc; (void)argv;
    g_StatusHandle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, ServiceHandler, NULL);
    if (!g_StatusHandle)
        return;
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    SetServiceState(SERVICE_START_PENDING);
    SetServiceState(SERVICE_RUNNING);
    rc = RunDaemon();
    g_ServiceStatus.dwWin32ExitCode = rc ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
    g_ServiceStatus.dwServiceSpecificExitCode = (DWORD)rc;
    SetServiceState(SERVICE_STOPPED);
}

/* smb2d.exe -console runs in the foreground; with no argument it is the service process. */
int main(int argc, char **argv)
{
    static SERVICE_TABLE_ENTRYW table[] = {{(LPWSTR)SERVICE_NAME, ServiceMain}, {NULL, NULL}};

    if (argc > 1 && !strcmp(argv[1], "-console"))
    {
        SetConsoleCtrlHandler(CtrlHandler, TRUE);
        return RunDaemon();
    }
    if (!StartServiceCtrlDispatcherW(table))
    {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
        {
            /* started from a console without arguments */
            SetConsoleCtrlHandler(CtrlHandler, TRUE);
            return RunDaemon();
        }
        return 1;
    }
    return 0;
}
