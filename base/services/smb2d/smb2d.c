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

#include "smb2client.h"
#include "smb2rdr_ioctl.h"

/* Status values from ntstatus.h without pulling the kernel-flavoured macros */
#define ST_SUCCESS              0x00000000L
#define ST_NOT_SUPPORTED        ((LONG)0xC00000BBL)
#define ST_BAD_NETWORK_PATH     ((LONG)0xC00000BEL)

static volatile LONG g_Stop;

static BOOL WINAPI CtrlHandler(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_Stop, 1);
    return TRUE;
}

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

int main(void)
{
    HANDLE dev;
    ULONG version = SMB2RDR_PROTOCOL_VERSION;

    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    dev = CreateFileW(SMB2RDR_USER_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, 0, NULL);
    if (dev == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "cannot open the smb2rdr device (driver not loaded?): %lu\n", GetLastError());
        return 1;
    }
    if (!Ioctl(dev, IOCTL_SMB2RDR_START, &version, sizeof(version), NULL, 0, NULL))
    {
        fprintf(stderr, "start failed: %lu\n", GetLastError());
        CloseHandle(dev);
        return 1;
    }
    printf("smb2d: redirector started, serving requests\n");
    Serve(dev);
    Ioctl(dev, IOCTL_SMB2RDR_STOP, NULL, 0, NULL, 0, NULL);
    CloseHandle(dev);
    return 0;
}
