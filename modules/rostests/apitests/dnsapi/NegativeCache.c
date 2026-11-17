/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A negative DNS answer (NXDOMAIN) is cached by the DNS Client service:
 *              a second query stays off the wire, the entry appears in DnsGetCacheDataTable,
 *              and flushing the entry forces a new wire query
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <windns.h>
#include <windns_undoc.h>
#include <winreg.h>
#include <apitest.h>

#define TEST_NAME  L"negcache-apitest.test"
#define PARAMS_KEY L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters"

static volatile LONG g_Stop;
static volatile LONG g_Queries;
static SOCKET g_Sock = INVALID_SOCKET;

static DWORD WINAPI
ResponderThread(LPVOID Param)
{
    UCHAR Packet[512];
    UCHAR Reply[512 + 16];

    UNREFERENCED_PARAMETER(Param);

    while (!g_Stop)
    {
        fd_set Set;
        struct timeval Tv = { 0, 100000 };
        struct sockaddr_in From;
        int FromLen = sizeof(From), Len, Pos;

        FD_ZERO(&Set);
        FD_SET(g_Sock, &Set);
        if (select(0, &Set, NULL, NULL, &Tv) <= 0)
            continue;
        Len = recvfrom(g_Sock, (char *)Packet, sizeof(Packet), 0, (struct sockaddr *)&From, &FromLen);
        if (Len < 17)
            continue;

        /* skip the question name */
        Pos = 12;
        while (Pos < Len && Packet[Pos] != 0)
            Pos += Packet[Pos] + 1;
        Pos++;
        if (Pos + 4 > Len)
            continue;

        InterlockedIncrement(&g_Queries);
        memcpy(Reply, Packet, Pos);
        Reply[2] = 0x81;                /* response, recursion desired */
        Reply[3] = 0x83;                /* recursion available, NXDOMAIN */
        Reply[4] = 0; Reply[5] = 1;     /* one question */
        Reply[6] = 0; Reply[7] = 0;
        Reply[8] = Reply[9] = Reply[10] = Reply[11] = 0;
        sendto(g_Sock, (char *)Reply, Pos, 0, (struct sockaddr *)&From, FromLen);
    }
    return 0;
}

static DNS_STATUS
QueryTestName(void)
{
    PDNS_RECORD Record = NULL;
    DNS_STATUS Status = DnsQuery_W(TEST_NAME, DNS_TYPE_A, DNS_QUERY_STANDARD, NULL, &Record, NULL);

    if (Record)
        DnsRecordListFree(Record, DnsFreeRecordList);
    return Status;
}

void func_NegativeCache(void)
{
    HKEY Key;
    WSADATA Wsa;
    WCHAR OldServers[256] = { 0 };
    DWORD OldSize = sizeof(OldServers);
    BOOL HadServers, SetServers;
    struct sockaddr_in Local;
    HANDLE Thread;
    PDNS_CACHE_ENTRY Table, Entry, Next;
    DNS_STATUS Status;
    BOOL Found;

    if (!DnsFlushResolverCache())
    {
        skip("DNS Client service is not available\n");
        return;
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PARAMS_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &Key) != ERROR_SUCCESS)
    {
        skip("cannot open Tcpip\\Parameters\n");
        return;
    }

    if (WSAStartup(MAKEWORD(2, 2), &Wsa) != 0)
    {
        skip("WSAStartup failed\n");
        RegCloseKey(Key);
        return;
    }

    Local.sin_family = AF_INET;
    Local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Local.sin_port = htons(53);
    g_Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_Sock == INVALID_SOCKET || bind(g_Sock, (struct sockaddr *)&Local, sizeof(Local)) != 0)
    {
        skip("cannot bind 127.0.0.1:53\n");
        if (g_Sock != INVALID_SOCKET)
            closesocket(g_Sock);
        WSACleanup();
        RegCloseKey(Key);
        return;
    }

    HadServers = RegQueryValueExW(Key, L"NameServer", NULL, NULL, (PBYTE)OldServers, &OldSize) == ERROR_SUCCESS &&
                 OldServers[0];
    SetServers = RegSetValueExW(Key, L"NameServer", 0, REG_SZ, (const BYTE *)L"127.0.0.1", sizeof(L"127.0.0.1")) ==
                 ERROR_SUCCESS;
    if (!SetServers)
    {
        skip("cannot set the NameServer value\n");
        closesocket(g_Sock);
        WSACleanup();
        RegCloseKey(Key);
        return;
    }

    DnsFlushResolverCache();
    InterlockedExchange(&g_Stop, 0);
    InterlockedExchange(&g_Queries, 0);
    Thread = CreateThread(NULL, 0, ResponderThread, NULL, 0, NULL);
    if (Thread == NULL)
    {
        skip("cannot start the responder thread\n");
        if (HadServers)
            RegSetValueExW(Key, L"NameServer", 0, REG_SZ, (const BYTE *)OldServers, (lstrlenW(OldServers) + 1) * sizeof(WCHAR));
        else
            RegDeleteValueW(Key, L"NameServer");
        closesocket(g_Sock);
        WSACleanup();
        RegCloseKey(Key);
        return;
    }

    /* First query: the responder answers NXDOMAIN */
    Status = QueryTestName();
    ok(Status == DNS_ERROR_RCODE_NAME_ERROR, "first query status %lu\n", Status);
    ok(g_Queries == 1, "responder saw %ld queries\n", g_Queries);

    /* Second query: the cached negative answers, the wire is not touched */
    Status = QueryTestName();
    ok(Status == DNS_ERROR_RCODE_NAME_ERROR, "cached query status %lu\n", Status);
    ok(g_Queries == 1, "negative answer was not cached, %ld wire queries\n", g_Queries);

    /* The negative entry appears in the cache table */
    Table = NULL;
    ok(DnsGetCacheDataTable(&Table), "DnsGetCacheDataTable failed\n");
    Found = FALSE;
    for (Entry = Table; Entry; Entry = Entry->pNext)
    {
        /* the service stores the name with the spelling of the query that cached it */
        if (Entry->pszName && !wcscmp(Entry->pszName, TEST_NAME))
            Found = TRUE;
    }
    for (Entry = Table; Entry; Entry = Next)
    {
        Next = Entry->pNext;
        if (Entry->pszName)
            LocalFree(Entry->pszName);
        LocalFree(Entry);
    }
    ok(Found, "negative entry missing from the cache table\n");

    /* Flushing the entry evicts the negative: the next query reaches the wire */
    ok(DnsFlushResolverCacheEntry_W(TEST_NAME), "DnsFlushResolverCacheEntry_W failed\n");
    Status = QueryTestName();
    ok(Status == DNS_ERROR_RCODE_NAME_ERROR, "post-flush query status %lu\n", Status);
    ok(g_Queries == 2, "flush did not evict the negative entry, %ld wire queries\n", g_Queries);

    /* Restore */
    InterlockedExchange(&g_Stop, 1);
    WaitForSingleObject(Thread, 5000);
    CloseHandle(Thread);
    if (HadServers)
        RegSetValueExW(Key, L"NameServer", 0, REG_SZ, (const BYTE *)OldServers, (lstrlenW(OldServers) + 1) * sizeof(WCHAR));
    else
        RegDeleteValueW(Key, L"NameServer");
    closesocket(g_Sock);
    g_Sock = INVALID_SOCKET;
    WSACleanup();
    RegCloseKey(Key);
    DnsFlushResolverCache();
}
