/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test for DnsQueryEx and DnsCancelQuery
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <windns.h>
#include <apitest.h>

#define QUERY_CONTEXT_TAG ((PVOID)(ULONG_PTR)0x517cc1b9)

static HANDLE g_Done;
static PDNS_QUERY_RESULT g_CallbackResults;
static volatile DNS_STATUS g_CallbackStatus;
static volatile LONG g_ContextSeen;
static volatile LONG g_CallbackCalls;

static VOID CALLBACK
QueryComplete(PVOID Context, PDNS_QUERY_RESULT Results)
{
    if (Context == QUERY_CONTEXT_TAG)
        InterlockedExchange(&g_ContextSeen, 1);
    g_CallbackResults = Results;
    g_CallbackStatus = Results->QueryStatus;
    InterlockedIncrement(&g_CallbackCalls);
    if (Results->pQueryRecords)
        DnsRecordListFree(Results->pQueryRecords, DnsFreeRecordList);
    SetEvent(g_Done);
}

static VOID
ResetCompletion(VOID)
{
    if (g_Done)
        CloseHandle(g_Done);
    g_Done = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_CallbackResults = NULL;
    g_CallbackStatus = ERROR_SUCCESS;
    InterlockedExchange(&g_ContextSeen, 0);
    InterlockedExchange(&g_CallbackCalls, 0);
}

static volatile LONG g_Stop;
static SOCKET g_Sock = INVALID_SOCKET;

static DWORD WINAPI
ServerListResponder(PVOID Param)
{
    UCHAR Packet[512];
    UCHAR Reply[512];

    UNREFERENCED_PARAMETER(Param);

    while (!g_Stop)
    {
        fd_set Set;
        struct timeval Tv = { 0, 100000 };
        struct sockaddr_in From;
        int FromLen = sizeof(From), Len, Pos, QType;

        FD_ZERO(&Set);
        FD_SET(g_Sock, &Set);
        if (select(0, &Set, NULL, NULL, &Tv) <= 0)
            continue;
        Len = recvfrom(g_Sock, (char *)Packet, sizeof(Packet), 0, (struct sockaddr *)&From, &FromLen);
        if (Len < 17)
            continue;

        Pos = 12;
        while (Pos < Len && Packet[Pos] != 0)
            Pos += Packet[Pos] + 1;
        Pos++;
        if (Pos + 4 > Len)
            continue;
        QType = (Packet[Pos] << 8) | Packet[Pos + 1];
        if (QType != DNS_TYPE_A)
            continue;

        memcpy(Reply, Packet, Pos + 4);
        Reply[2] = 0x81;
        Reply[3] = 0x80;
        Reply[4] = 0; Reply[5] = 1;
        Reply[6] = 0; Reply[7] = 1;
        Reply[8] = Reply[9] = Reply[10] = Reply[11] = 0;
        {
            int Out = Pos + 4;
            Reply[Out++] = 0xC0; Reply[Out++] = 0x0C;
            Reply[Out++] = 0; Reply[Out++] = 1;
            Reply[Out++] = 0; Reply[Out++] = 1;
            Reply[Out++] = 0; Reply[Out++] = 0; Reply[Out++] = 1; Reply[Out++] = 0x2C;
            Reply[Out++] = 0; Reply[Out++] = 4;
            Reply[Out++] = 192; Reply[Out++] = 0; Reply[Out++] = 2; Reply[Out++] = 55;
            sendto(g_Sock, (char *)Reply, Out, 0, (struct sockaddr *)&From, FromLen);
        }
    }
    return 0;
}

static VOID
ServerListTest(PDNS_QUERY_REQUEST Request)
{
    DNS_ADDR_ARRAY Servers;
    DNS_QUERY_RESULT Results;
    PDNS_RECORD Records;
    struct sockaddr_in Local;
    WSADATA Wsa;
    HANDLE Thread;
    DNS_STATUS Status;

    if (WSAStartup(MAKEWORD(2, 2), &Wsa) != 0)
    {
        skip("WSAStartup failed\n");
        return;
    }

    Local.sin_family = AF_INET;
    Local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Local.sin_port = htons(53);
    g_Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_Sock == INVALID_SOCKET ||
        bind(g_Sock, (struct sockaddr *)&Local, sizeof(Local)) != 0)
    {
        skip("cannot bind 127.0.0.1:53\n");
        if (g_Sock != INVALID_SOCKET)
            closesocket(g_Sock);
        WSACleanup();
        return;
    }

    RtlZeroMemory(&Servers, sizeof(Servers));
    Servers.MaxCount = 1;
    Servers.AddrCount = 1;
    Servers.Family = AF_INET;
    {
        SOCKADDR_IN *Sa = (SOCKADDR_IN *)Servers.AddrArray[0].MaxSa;
        Sa->sin_family = AF_INET;
        Sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Sa->sin_port = htons(53);
    }

    InterlockedExchange(&g_Stop, 0);
    Thread = CreateThread(NULL, 0, ServerListResponder, NULL, 0, NULL);
    if (Thread == NULL)
    {
        skip("cannot start the responder\n");
        closesocket(g_Sock);
        WSACleanup();
        return;
    }

    Request->QueryName = L"spltest.example";
    Request->QueryType = DNS_TYPE_A;
    Request->pDnsServerList = &Servers;
    ZeroMemory(&Results, sizeof(Results));
    Status = DnsQueryEx(Request, &Results, NULL);
    Request->pDnsServerList = NULL;
    Request->QueryName = L"localhost";

    ok(Status == ERROR_SUCCESS, "server-list query failed: %lu\n", Status);
    Records = Results.pQueryRecords;
    ok(Records != NULL && Records->wType == DNS_TYPE_A,
       "server-list query returned no A record\n");
    if (Records)
    {
        ok(!memcmp(&Records->Data.A.IpAddress, "\xC0\x00\x02\x37", 4),
           "server-list answer mismatch\n");
        DnsRecordListFree(Records, DnsFreeRecordList);
    }

    InterlockedExchange(&g_Stop, 1);
    WaitForSingleObject(Thread, 5000);
    CloseHandle(Thread);
    closesocket(g_Sock);
    g_Sock = INVALID_SOCKET;
    WSACleanup();
}

void func_DnsQueryEx(void)
{
    DNS_QUERY_REQUEST Request;
    DNS_QUERY_RESULT Results;
    DNS_QUERY_CANCEL Cancel;
    DNS_STATUS Status;
    DWORD Wait;

    /* A query without a completion routine completes synchronously */
    ZeroMemory(&Request, sizeof(Request));
    Request.Version = DNS_QUERY_REQUEST_VERSION1;
    Request.QueryName = L"localhost";
    Request.QueryType = DNS_TYPE_A;
    ZeroMemory(&Results, sizeof(Results));
    Status = DnsQueryEx(&Request, &Results, NULL);
    ok(Status == ERROR_SUCCESS, "DnsQueryEx sync failed: %lu\n", Status);
    ok(Results.Version == DNS_QUERY_RESULTS_VERSION1, "Result version %lu\n", Results.Version);
    ok(Results.QueryStatus == Status, "Result status %lu\n", Results.QueryStatus);
    ok(Results.pQueryRecords != NULL, "Sync query returned no records\n");
    if (Results.pQueryRecords)
        DnsRecordListFree(Results.pQueryRecords, DnsFreeRecordList);

    /* Argument validation */
    Status = DnsQueryEx(NULL, &Results, NULL);
    ok(Status == ERROR_INVALID_PARAMETER, "NULL request: %lu\n", Status);
    Status = DnsQueryEx(&Request, NULL, NULL);
    ok(Status == ERROR_INVALID_PARAMETER, "NULL results: %lu\n", Status);
    Request.Version = 2;
    Status = DnsQueryEx(&Request, &Results, NULL);
    ok(Status == ERROR_INVALID_PARAMETER, "Bad request version: %lu\n", Status);
    Request.Version = DNS_QUERY_REQUEST_VERSION1;
    Request.QueryName = NULL;
    Status = DnsQueryEx(&Request, &Results, NULL);
    ok(Status == ERROR_INVALID_PARAMETER, "NULL name: %lu\n", Status);
    Request.QueryName = L"localhost";
    Request.InterfaceIndex = 3;
    Status = DnsQueryEx(&Request, &Results, NULL);
    ok(Status == ERROR_INVALID_PARAMETER, "Interface index: %lu\n", Status);
    Request.InterfaceIndex = 0;

    ServerListTest(&Request);

    /* A query with a completion routine is pending and delivers asynchronously
     * into the caller's result structure */
    ResetCompletion();
    Request.QueryName = L"localhost";
    Request.QueryType = DNS_TYPE_AAAA;
    Request.pQueryCompletionCallback = QueryComplete;
    Request.pQueryContext = QUERY_CONTEXT_TAG;
    ZeroMemory(&Cancel, sizeof(Cancel));
    ZeroMemory(&Results, sizeof(Results));
    Status = DnsQueryEx(&Request, &Results, &Cancel);
    ok(Status == DNS_REQUEST_PENDING, "Async submit returned %lu, expected DNS_REQUEST_PENDING\n", Status);
    Wait = WaitForSingleObject(g_Done, 10000);
    ok(Wait == WAIT_OBJECT_0, "Completion never delivered\n");
    ok(g_CallbackCalls == 1, "Callback calls %ld\n", g_CallbackCalls);
    ok(g_ContextSeen == 1, "Query context not passed through\n");
    ok(g_CallbackStatus == ERROR_SUCCESS, "Callback status %lu\n", g_CallbackStatus);
    ok(g_CallbackResults == &Results, "Callback did not receive the caller's result pointer\n");
    ok(Results.QueryStatus == ERROR_SUCCESS, "Caller result not filled: %lu\n", Results.QueryStatus);

    /* A finished query's cancel handle is inert */
    DnsCancelQuery(&Cancel);
    ok(g_CallbackCalls == 1, "Cancel after completion redelivered\n");

    /* Cancelling a pending query still delivers one completion with ERROR_CANCELLED */
    ResetCompletion();
    Request.QueryName = L"localhost";
    Request.QueryType = DNS_TYPE_A;
    ZeroMemory(&Cancel, sizeof(Cancel));
    ZeroMemory(&Results, sizeof(Results));
    Status = DnsQueryEx(&Request, &Results, &Cancel);
    ok(Status == DNS_REQUEST_PENDING, "Second async submit returned %lu\n", Status);
    DnsCancelQuery(&Cancel);
    Wait = WaitForSingleObject(g_Done, 10000);
    ok(Wait == WAIT_OBJECT_0, "Cancelled query never delivered its completion\n");
    ok(g_CallbackCalls == 1, "Cancelled query delivered %ld completions\n", g_CallbackCalls);
    ok(g_CallbackStatus == ERROR_CANCELLED, "Cancelled status %lu\n", g_CallbackStatus);
    ok(Results.QueryStatus == ERROR_CANCELLED, "Caller result status %lu\n", Results.QueryStatus);

    /* A cleared handle cancels nothing and touches no memory */
    ResetCompletion();
    Status = DnsQueryEx(&Request, &Results, &Cancel);
    ok(Status == DNS_REQUEST_PENDING, "Third async submit returned %lu\n", Status);
    DnsCancelQuery(&Cancel);
    DnsCancelQuery(&Cancel);
    Wait = WaitForSingleObject(g_Done, 10000);
    ok(Wait == WAIT_OBJECT_0, "Completion after double cancel never delivered\n");
    ok(g_CallbackCalls == 1, "Double cancel delivered %ld completions\n", g_CallbackCalls);

    ResetCompletion();
}
