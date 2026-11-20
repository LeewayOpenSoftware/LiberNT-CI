/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     ncacn_ip_tcp server endpoints answer on IPv4 and IPv6 loopback
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <rpc.h>

static int TcpConnect(int Family, USHORT Port)
{
    struct sockaddr_in a4 = { AF_INET };
    struct sockaddr_in6 a6 = { AF_INET6 };
    SOCKET s;
    int Error = 0;

    s = socket(Family, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return WSAGetLastError();
    if (Family == AF_INET6)
    {
        a6.sin6_port = htons(Port);
        a6.sin6_addr.s6_addr[15] = 1; /* ::1 */
        if (connect(s, (struct sockaddr *)&a6, sizeof(a6)))
            Error = WSAGetLastError();
    }
    else
    {
        a4.sin_port = htons(Port);
        a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (struct sockaddr *)&a4, sizeof(a4)))
            Error = WSAGetLastError();
    }
    closesocket(s);
    return Error;
}

START_TEST(TcpListen)
{
    RPC_BINDING_VECTOR *Vector;
    RPC_WSTR String;
    RPC_STATUS Status;
    WSADATA Wsa;
    ULONG i, Dynamic = 0, Fixed = 0;

    ok(WSAStartup(MAKEWORD(2, 2), &Wsa) == 0, "WSAStartup failed\n");

    Status = RpcServerUseProtseqW((RPC_WSTR)L"ncacn_ip_tcp", 10, NULL);
    ok(Status == RPC_S_OK, "dynamic endpoint: %ld\n", Status);
    Status = RpcServerUseProtseqEpW((RPC_WSTR)L"ncacn_ip_tcp", 10, (RPC_WSTR)L"47123", NULL);
    ok(Status == RPC_S_OK, "fixed endpoint: %ld\n", Status);

    Status = RpcServerInqBindings(&Vector);
    ok(Status == RPC_S_OK, "RpcServerInqBindings: %ld\n", Status);
    if (Status != RPC_S_OK)
        return;

    for (i = 0; i < Vector->Count; i++)
    {
        WCHAR *Start, *End;
        USHORT Port;

        if (RpcBindingToStringBindingW(Vector->BindingH[i], &String) != RPC_S_OK)
            continue;
        Start = wcsstr((WCHAR *)String, L"ncacn_ip_tcp:");
        if (Start && (Start = wcschr(Start, L'[')) && (End = wcschr(Start, L']')))
        {
            *End = 0;
            Port = (USHORT)_wtoi(Start + 1);
            if (Port == 47123)
                Fixed++;
            else
                Dynamic++;
            /* Windows listens on 0.0.0.0 and [::] with the same port */
            ok(TcpConnect(AF_INET, Port) == 0, "port %u: 127.0.0.1 refused (%d)\n", Port, TcpConnect(AF_INET, Port));
            ok(TcpConnect(AF_INET6, Port) == 0, "port %u: ::1 refused (%d)\n", Port, TcpConnect(AF_INET6, Port));
        }
        RpcStringFreeW(&String);
    }
    RpcBindingVectorFree(&Vector);

    /* one binding per endpoint, however many address families listen */
    ok(Dynamic == 1, "dynamic endpoint listed %lu times\n", Dynamic);
    ok(Fixed == 1, "fixed endpoint listed %lu times\n", Fixed);

    WSACleanup();
}
