/*
 * PROJECT:         LiberNT api tests
 * LICENSE:         GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:         LLMNR (RFC 4795) and mDNS (RFC 6762) name resolution through DnsQuery_W
 *
 * Fake responders sit on the multicast groups of the first up IPv4 interface (SO_REUSEADDR, so they
 * coexist with the DNS Client service) and answer one name each. The test skips, and never passes, when
 * there is no such interface or when a multicast datagram sent here is not received here.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <ctype.h>
#include <strsafe.h>
#include <windns.h>
#include <winsvc.h>
#include <apitest.h>

#define MDNS_PORT   5353
#define LLMNR_PORT  5355
#define MDNS_GROUP  0xE00000FBUL
#define LLMNR_GROUP 0xE00000FCUL
#define ANSWER_ADDR ((DWORD)(192 | (0 << 8) | (2 << 16) | (77UL << 24)))

typedef struct _RESPONDER
{
    SOCKET Socket;
    HANDLE Thread;
    volatile LONG Stop;
    volatile LONG Queries;      /* datagrams that were queries for Name */
    volatile LONG OtherQueries; /* all other queries seen */
    char Name[96];              /* dotted, lower case */
    DWORD Group;
    USHORT Port;
} RESPONDER;

static DWORD g_Interface;       /* network order */

static BOOL FindInterface(void)
{
    ULONG Size = 0;
    PIP_ADAPTER_ADDRESSES List, A;
    BOOL Found = FALSE;

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             NULL, NULL, &Size) != ERROR_BUFFER_OVERFLOW)
        return FALSE;
    List = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!List)
        return FALSE;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             NULL, List, &Size) == ERROR_SUCCESS)
    {
        for (A = List; A && !Found; A = A->Next)
        {
            PIP_ADAPTER_UNICAST_ADDRESS U;

            if (A->OperStatus != IfOperStatusUp || A->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (U = A->FirstUnicastAddress; U; U = U->Next)
            {
                if (U->Address.lpSockaddr && U->Address.lpSockaddr->sa_family == AF_INET)
                {
                    g_Interface = ((SOCKADDR_IN *)U->Address.lpSockaddr)->sin_addr.s_addr;
                    Found = TRUE;
                    break;
                }
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, List);
    return Found;
}

static SOCKET OpenGroupSocket(DWORD Group, USHORT Port)
{
    SOCKET S = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKADDR_IN Any;
    struct ip_mreq Req;
    DWORD Reuse = TRUE;

    if (S == INVALID_SOCKET)
        return S;
    setsockopt(S, SOL_SOCKET, SO_REUSEADDR, (const char *)&Reuse, sizeof(Reuse));
    ZeroMemory(&Any, sizeof(Any));
    Any.sin_family = AF_INET;
    Any.sin_port = htons(Port);
    if (bind(S, (SOCKADDR *)&Any, sizeof(Any)))
    {
        closesocket(S);
        return INVALID_SOCKET;
    }
    Req.imr_multiaddr.s_addr = htonl(Group);
    Req.imr_interface.s_addr = g_Interface;
    if (setsockopt(S, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&Req, sizeof(Req)))
    {
        closesocket(S);
        return INVALID_SOCKET;
    }
    return S;
}

/* Reads the first question name of a query as a dotted lower case string; returns the offset after the question */
static int ReadQuestion(const UCHAR *P, int Length, char *Name, int NameSize)
{
    int Off = 12, Out = 0;

    if (Length < 17 || (P[2] & 0xF8) || !(P[4] | P[5]))
        return -1;
    while (Off < Length && P[Off])
    {
        int Label = P[Off++], i;

        if ((Label & 0xC0) || Off + Label >= Length || Out + Label + 2 >= NameSize)
            return -1;
        if (Out)
            Name[Out++] = '.';
        for (i = 0; i < Label; i++)
            Name[Out++] = (char)tolower(P[Off++]);
    }
    Name[Out] = 0;
    if (Off + 5 > Length)
        return -1;
    return Off + 5;
}

static DWORD WINAPI ResponderThread(LPVOID Param)
{
    RESPONDER *R = Param;
    UCHAR Packet[512], Reply[600];

    while (!R->Stop)
    {
        fd_set Read;
        struct timeval Wait = { 0, 100000 };
        SOCKADDR_IN From;
        int FromLength = sizeof(From), Length, End, Size;
        DWORD Answer = ANSWER_ADDR;
        char Name[96];

        FD_ZERO(&Read);
        FD_SET(R->Socket, &Read);
        if (select(0, &Read, NULL, NULL, &Wait) <= 0)
            continue;
        Length = recvfrom(R->Socket, (char *)Packet, sizeof(Packet), 0, (SOCKADDR *)&From, &FromLength);
        if (Length <= 0)
            continue;
        End = ReadQuestion(Packet, Length, Name, sizeof(Name));
        if (End < 0)
            continue;
        if (strcmp(Name, R->Name))
        {
            InterlockedIncrement(&R->OtherQueries);
            continue;
        }
        InterlockedIncrement(&R->Queries);

        /* A legacy unicast answer (RFC 6762 6.7): same ID, question echoed; the class carries the cache-flush bit */
        memcpy(Reply, Packet, End);
        Reply[2] = 0x84;
        Reply[3] = 0;
        Reply[4] = 0; Reply[5] = 1;
        Reply[6] = 0; Reply[7] = 1;
        Reply[8] = Reply[9] = Reply[10] = Reply[11] = 0;
        Size = End;
        Reply[Size++] = 0xC0; Reply[Size++] = 12;
        Reply[Size++] = 0; Reply[Size++] = 1;
        Reply[Size++] = (R->Port == MDNS_PORT) ? 0x80 : 0x00; Reply[Size++] = 1;
        Reply[Size++] = 0; Reply[Size++] = 0; Reply[Size++] = 0; Reply[Size++] = 5;
        Reply[Size++] = 0; Reply[Size++] = 4;
        memcpy(Reply + Size, &Answer, 4);
        Size += 4;
        sendto(R->Socket, (char *)Reply, Size, 0, (SOCKADDR *)&From, FromLength);
    }
    return 0;
}

static BOOL StartResponder(RESPONDER *R, DWORD Group, USHORT Port, const char *Name)
{
    DWORD Id;

    ZeroMemory(R, sizeof(*R));
    R->Group = Group;
    R->Port = Port;
    strcpy(R->Name, Name);
    R->Socket = OpenGroupSocket(Group, Port);
    if (R->Socket == INVALID_SOCKET)
        return FALSE;
    R->Thread = CreateThread(NULL, 0, ResponderThread, R, 0, &Id);
    if (!R->Thread)
    {
        closesocket(R->Socket);
        return FALSE;
    }
    return TRUE;
}

static void StopResponder(RESPONDER *R)
{
    InterlockedExchange(&R->Stop, 1);
    WaitForSingleObject(R->Thread, 5000);
    CloseHandle(R->Thread);
    closesocket(R->Socket);
}

/* A multicast datagram sent from here must reach a listener here, or none of the checks below can mean anything */
static BOOL MulticastLoopbackWorks(void)
{
    SOCKET Rx = OpenGroupSocket(MDNS_GROUP, MDNS_PORT), Tx;
    SOCKADDR_IN To;
    fd_set Read;
    struct timeval Wait = { 1, 0 };
    char Buffer[16];
    BOOL Ok = FALSE;

    if (Rx == INVALID_SOCKET)
        return FALSE;
    Tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (Tx != INVALID_SOCKET)
    {
        ZeroMemory(&To, sizeof(To));
        To.sin_family = AF_INET;
        To.sin_port = htons(MDNS_PORT);
        To.sin_addr.s_addr = htonl(MDNS_GROUP);
        setsockopt(Tx, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&g_Interface, sizeof(g_Interface));
        sendto(Tx, "probe", 5, 0, (SOCKADDR *)&To, sizeof(To));
        FD_ZERO(&Read);
        FD_SET(Rx, &Read);
        Ok = select(0, &Read, NULL, NULL, &Wait) > 0 && recv(Rx, Buffer, sizeof(Buffer), 0) > 0;
        closesocket(Tx);
    }
    closesocket(Rx);
    return Ok;
}

static void ExpectAnswer(PCWSTR Name, DWORD Options, const char *What)
{
    PDNS_RECORD Record = NULL;
    DNS_STATUS Status = DnsQuery_W(Name, DNS_TYPE_A, Options, NULL, &Record, NULL);

    ok(Status == ERROR_SUCCESS, "%s: DnsQuery_W(%ls) returned %lu\n", What, Name, Status);
    if (Status == ERROR_SUCCESS && Record)
    {
        ok(Record->wType == DNS_TYPE_A, "%s: wType %u\n", What, Record->wType);
        ok(Record->Data.A.IpAddress == ANSWER_ADDR, "%s: address %08lx\n", What, Record->Data.A.IpAddress);
        PCWSTR Owner = (PCWSTR)Record->pName;

        ok(!_wcsicmp(Owner, Name), "%s: owner %ls\n", What, Owner);
        DnsRecordListFree(Record, DnsFreeRecordList);
    }
    else
    {
        ok(Record == NULL, "%s: a failed query left a record\n", What);
    }
}

static BOOL IsLocalAddress(DWORD Address)
{
    ULONG Size = 0;
    PIP_ADAPTER_ADDRESSES List, A;
    BOOL Found = FALSE;

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             NULL, NULL, &Size) != ERROR_BUFFER_OVERFLOW)
        return FALSE;
    List = HeapAlloc(GetProcessHeap(), 0, Size);
    if (List && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                     NULL, List, &Size) == ERROR_SUCCESS)
    {
        for (A = List; A; A = A->Next)
        {
            PIP_ADAPTER_UNICAST_ADDRESS U;

            for (U = A->FirstUnicastAddress; U; U = U->Next)
            {
                if (U->Address.lpSockaddr && U->Address.lpSockaddr->sa_family == AF_INET &&
                    ((SOCKADDR_IN *)U->Address.lpSockaddr)->sin_addr.s_addr == Address)
                    Found = TRUE;
            }
        }
    }
    if (List)
        HeapFree(GetProcessHeap(), 0, List);
    return Found;
}

static BOOL DnsClientRunning(void)
{
    SC_HANDLE Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), Service;
    SERVICE_STATUS Status;
    BOOL Running = FALSE;

    if (!Manager)
        return FALSE;
    Service = OpenServiceW(Manager, L"Dnscache", SERVICE_QUERY_STATUS);
    if (Service)
    {
        Running = QueryServiceStatus(Service, &Status) && Status.dwCurrentState == SERVICE_RUNNING;
        CloseServiceHandle(Service);
    }
    CloseServiceHandle(Manager);
    return Running;
}

static void TestMdns(DWORD Tag)
{
    RESPONDER Responder;
    WCHAR Name[64], Silent[64], NoMulticast[64];
    char NameA[64];
    PDNS_RECORD Record = NULL;
    DNS_STATUS Status;
    DWORD Start;

    StringCchPrintfW(Name, ARRAYSIZE(Name), L"pamtest%08lx.local", Tag);
    sprintf(NameA, "pamtest%08lx.local", Tag);
    StringCchPrintfW(Silent, ARRAYSIZE(Silent), L"nobody%08lx.local", Tag);
    StringCchPrintfW(NoMulticast, ARRAYSIZE(NoMulticast), L"nomc%08lx.local", Tag);

    if (!StartResponder(&Responder, MDNS_GROUP, MDNS_PORT, NameA))
    {
        skip("cannot start an mDNS responder on 224.0.0.251:5353\n");
        return;
    }

    /* A .local name is answered from the link, the cache-flush bit of the class notwithstanding */
    ExpectAnswer(Name, DNS_QUERY_MULTICAST_ONLY, "mDNS multicast-only");
    ok(Responder.Queries >= 1, "The responder saw %ld queries for its name\n", Responder.Queries);

    /* The same through the normal query order: .local goes to mDNS before unicast DNS */
    InterlockedExchange(&Responder.Queries, 0);
    ExpectAnswer(Name, DNS_QUERY_BYPASS_CACHE, "mDNS default order");
    ok(Responder.Queries >= 1, "Default order sent no mDNS query (%ld)\n", Responder.Queries);

    /* Nobody answers: an error, within the two query rounds */
    Start = GetTickCount();
    Status = DnsQuery_W(Silent, DNS_TYPE_A, DNS_QUERY_MULTICAST_ONLY, NULL, &Record, NULL);
    ok(Status != ERROR_SUCCESS, "A name nobody answers resolved\n");
    ok(Record == NULL, "A failed query left a record\n");
    ok(GetTickCount() - Start < 8000, "The unanswered query took %lu ms\n", GetTickCount() - Start);
    ok(Responder.OtherQueries >= 1, "The unanswered name was never asked on the link\n");

    /* NO_MULTICAST must not send the question on the link at all */
    InterlockedExchange(&Responder.OtherQueries, 0);
    InterlockedExchange(&Responder.Queries, 0);
    Record = NULL;
    Status = DnsQuery_W(NoMulticast, DNS_TYPE_A, DNS_QUERY_NO_MULTICAST | DNS_QUERY_BYPASS_CACHE, NULL, &Record, NULL);
    if (Status == ERROR_SUCCESS)
        DnsRecordListFree(Record, DnsFreeRecordList);
    ok(Responder.OtherQueries == 0 && Responder.Queries == 0, "NO_MULTICAST still asked on the link (%ld)\n",
       Responder.OtherQueries);

    /* Only .local and single-label names have a multicast form */
    InterlockedExchange(&Responder.OtherQueries, 0);
    Record = NULL;
    Status = DnsQuery_W(L"host.example.com", DNS_TYPE_A, DNS_QUERY_MULTICAST_ONLY, NULL, &Record, NULL);
    ok(Status == ERROR_INVALID_NAME, "MULTICAST_ONLY on a global name returned %lu\n", Status);
    ok(Responder.OtherQueries == 0, "A global name was sent to the mDNS group\n");

    StopResponder(&Responder);
}

static void TestLlmnr(DWORD Tag)
{
    RESPONDER Responder;
    WCHAR Name[64];
    char NameA[64];
    PDNS_RECORD Record = NULL;
    DNS_STATUS Status;

    StringCchPrintfW(Name, ARRAYSIZE(Name), L"pamllmnr%08lx", Tag);
    sprintf(NameA, "pamllmnr%08lx", Tag);

    if (!StartResponder(&Responder, LLMNR_GROUP, LLMNR_PORT, NameA))
    {
        skip("cannot share 224.0.0.252:5355 (the DNS Client responder may hold it exclusively)\n");
        return;
    }

    ExpectAnswer(Name, DNS_QUERY_MULTICAST_ONLY, "LLMNR multicast-only");
    ok(Responder.Queries >= 1, "The responder saw %ld LLMNR queries\n", Responder.Queries);

    /* A dotted name is not an LLMNR name */
    InterlockedExchange(&Responder.OtherQueries, 0);
    Status = DnsQuery_W(L"a.b", DNS_TYPE_A, DNS_QUERY_MULTICAST_ONLY, NULL, &Record, NULL);
    ok(Status == ERROR_INVALID_NAME, "Dotted LLMNR name returned %lu\n", Status);
    ok(Responder.OtherQueries == 0, "A dotted name went to the LLMNR group\n");

    /* NO_MULTICAST keeps a single-label name off the link */
    InterlockedExchange(&Responder.Queries, 0);
    Record = NULL;
    Status = DnsQuery_W(Name, DNS_TYPE_A, DNS_QUERY_NO_MULTICAST | DNS_QUERY_BYPASS_CACHE, NULL, &Record, NULL);
    if (Status == ERROR_SUCCESS)
        DnsRecordListFree(Record, DnsFreeRecordList);
    ok(Responder.Queries == 0, "NO_MULTICAST still sent an LLMNR query (%ld)\n", Responder.Queries);

    StopResponder(&Responder);
}

/* The DNS Client service answers for this computer's own name on both protocols */
static void TestOwnResponders(void)
{
    WCHAR Host[MAX_COMPUTERNAME_LENGTH + 1], Local[MAX_COMPUTERNAME_LENGTH + 8];
    DWORD Length = ARRAYSIZE(Host);
    PDNS_RECORD Record = NULL;
    DNS_STATUS Status;

    if (!DnsClientRunning())
    {
        skip("The DNS Client service is not running\n");
        return;
    }
    if (!GetComputerNameW(Host, &Length))
    {
        skip("no computer name\n");
        return;
    }

    Status = DnsQuery_W(Host, DNS_TYPE_A, DNS_QUERY_MULTICAST_ONLY | DNS_QUERY_BYPASS_CACHE, NULL, &Record, NULL);
    ok(Status == ERROR_SUCCESS, "LLMNR for our own name %ls returned %lu\n", Host, Status);
    if (Status == ERROR_SUCCESS && Record)
    {
        ok(IsLocalAddress(Record->Data.A.IpAddress), "LLMNR answered %08lx, not an address of this host\n",
           Record->Data.A.IpAddress);
        DnsRecordListFree(Record, DnsFreeRecordList);
    }

    StringCchPrintfW(Local, ARRAYSIZE(Local), L"%ls.local", Host);
    Record = NULL;
    Status = DnsQuery_W(Local, DNS_TYPE_A, DNS_QUERY_MULTICAST_ONLY | DNS_QUERY_BYPASS_CACHE, NULL, &Record, NULL);
    ok(Status == ERROR_SUCCESS, "mDNS for our own name %ls returned %lu\n", Local, Status);
    if (Status == ERROR_SUCCESS && Record)
    {
        ok(IsLocalAddress(Record->Data.A.IpAddress), "mDNS answered %08lx, not an address of this host\n",
           Record->Data.A.IpAddress);
        DnsRecordListFree(Record, DnsFreeRecordList);
    }
}

START_TEST(LlmnrMdns)
{
    WSADATA Data;
    DWORD Tag = GetTickCount() ^ (GetCurrentProcessId() << 8);

    if (WSAStartup(MAKEWORD(2, 2), &Data))
    {
        skip("WSAStartup failed\n");
        return;
    }
    if (!FindInterface())
    {
        skip("no up non-loopback IPv4 interface\n");
        WSACleanup();
        return;
    }
    if (!MulticastLoopbackWorks())
    {
        skip("a multicast datagram sent here is not received here\n");
        WSACleanup();
        return;
    }

    TestMdns(Tag);
    TestLlmnr(Tag);
    TestOwnResponders();
    WSACleanup();
}
