/*
 * PROJECT:     LiberNT DNS Resolver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Multicast DNS responder for <computer name>.local (RFC 6762)
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "precomp.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#define NDEBUG
#include <debug.h>

BOOL LlmnrInterfaceAddress(ULONG Index, ADDRESS_FAMILY Family, BOOL LinkLocal, SOCKADDR_INET *Result);

#define MDNS_PORT 5353
#define MDNS_GROUP4 0xe00000fb
#define MDNS_HOST_TTL 120
#define MDNS_LEGACY_TTL 10
#define MDNS_MAX_JOINS 16

static const IN6_ADDR MdnsGroup6 = {{{ 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xfb }}};

static HANDLE MdnsStopEvent;
static HANDLE MdnsThread;

typedef struct _MDNS_STATE
{
    SOCKET Socket4;
    SOCKET Socket6;
    ULONG Joined4[MDNS_MAX_JOINS];
    ULONG Joined4Count;
    ULONG Joined6[MDNS_MAX_JOINS];
    ULONG Joined6Count;
} MDNS_STATE, *PMDNS_STATE;

static BOOL MdnsKnown(const ULONG *List, ULONG Count, ULONG Value)
{
    ULONG i;

    for (i = 0; i < Count; i++)
    {
        if (List[i] == Value)
            return TRUE;
    }
    return FALSE;
}

static VOID MdnsJoinInterfaces(PMDNS_STATE State)
{
    IP_ADAPTER_ADDRESSES *Addresses, *Adapter;
    IP_ADAPTER_UNICAST_ADDRESS *Unicast;
    ULONG Size = 0, Flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    struct ip_mreq Request4;
    struct ipv6_mreq Request6;

    if (GetAdaptersAddresses(AF_UNSPEC, Flags, NULL, NULL, &Size) != ERROR_BUFFER_OVERFLOW)
        return;
    Addresses = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Addresses)
        return;
    if (GetAdaptersAddresses(AF_UNSPEC, Flags, NULL, Addresses, &Size) == ERROR_SUCCESS)
    {
        for (Adapter = Addresses; Adapter; Adapter = Adapter->Next)
        {
            BOOL HasIpv6 = FALSE;
            ULONG Address = INADDR_ANY;

            if (Adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || Adapter->OperStatus != IfOperStatusUp)
                continue;
            for (Unicast = Adapter->FirstUnicastAddress; Unicast; Unicast = Unicast->Next)
            {
                if (Unicast->Address.lpSockaddr->sa_family == AF_INET6)
                    HasIpv6 = TRUE;
                else if (Unicast->Address.lpSockaddr->sa_family == AF_INET && Address == INADDR_ANY)
                    Address = ((SOCKADDR_IN *)Unicast->Address.lpSockaddr)->sin_addr.s_addr;
            }
            if (Address != INADDR_ANY && State->Socket4 != INVALID_SOCKET &&
                State->Joined4Count < MDNS_MAX_JOINS && !MdnsKnown(State->Joined4, State->Joined4Count, Adapter->IfIndex))
            {
                Request4.imr_multiaddr.s_addr = htonl(MDNS_GROUP4);
                Request4.imr_interface.s_addr = Address;
                if (!setsockopt(State->Socket4, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&Request4, sizeof(Request4)))
                    State->Joined4[State->Joined4Count++] = Adapter->IfIndex;
            }
            if (!HasIpv6 || !Adapter->Ipv6IfIndex || State->Socket6 == INVALID_SOCKET ||
                State->Joined6Count >= MDNS_MAX_JOINS || MdnsKnown(State->Joined6, State->Joined6Count, Adapter->Ipv6IfIndex))
                continue;
            Request6.ipv6mr_multiaddr = MdnsGroup6;
            Request6.ipv6mr_interface = Adapter->Ipv6IfIndex;
            if (!setsockopt(State->Socket6, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, (const char *)&Request6, sizeof(Request6)))
                State->Joined6[State->Joined6Count++] = Adapter->Ipv6IfIndex;
        }
    }
    HeapFree(GetProcessHeap(), 0, Addresses);
}

/* Matches "<Host>.local" at Offset and moves Offset past it; compression pointers are not valid in a query's first name */
static BOOL MdnsNameMatches(const UCHAR *Packet, int Length, int *Offset, const char *Host)
{
    int Position = *Offset, HostLength = (int)strlen(Host);

    if (Position + 1 + HostLength + 1 + 5 + 1 > Length)
        return FALSE;
    if (Packet[Position] != HostLength || _strnicmp((const char *)Packet + Position + 1, Host, HostLength))
        return FALSE;
    Position += 1 + HostLength;
    if (Packet[Position] != 5 || _strnicmp((const char *)Packet + Position + 1, "local", 5))
        return FALSE;
    Position += 1 + 5;
    if (Packet[Position] != 0)
        return FALSE;
    *Offset = Position + 1;
    return TRUE;
}

static int MdnsPutRecord(UCHAR *Reply, int Size, BOOL Legacy, const char *Host, USHORT Type,
                         const SOCKADDR_INET *Address)
{
    int HostLength = (int)strlen(Host);

    if (Legacy)
    {
        /* Name points at the question that was echoed back at offset 12 */
        Reply[Size++] = 0xc0;
        Reply[Size++] = 12;
    }
    else
    {
        Reply[Size++] = (UCHAR)HostLength;
        memcpy(Reply + Size, Host, HostLength);
        Size += HostLength;
        Reply[Size++] = 5;
        memcpy(Reply + Size, "local", 5);
        Size += 5;
        Reply[Size++] = 0;
    }
    Reply[Size++] = 0;
    Reply[Size++] = (UCHAR)Type;
    /* Unique record: the cache-flush bit goes in the class of multicast answers, not of legacy unicast ones */
    Reply[Size++] = Legacy ? 0x00 : 0x80;
    Reply[Size++] = 1;
    Reply[Size++] = 0;
    Reply[Size++] = 0;
    Reply[Size++] = 0;
    Reply[Size++] = Legacy ? MDNS_LEGACY_TTL : MDNS_HOST_TTL;
    Reply[Size++] = 0;
    if (Type == 1)
    {
        Reply[Size++] = 4;
        memcpy(Reply + Size, &Address->Ipv4.sin_addr, 4);
        Size += 4;
    }
    else
    {
        Reply[Size++] = 16;
        memcpy(Reply + Size, &Address->Ipv6.sin6_addr, 16);
        Size += 16;
    }
    return Size;
}

static VOID MdnsRespond(PMDNS_STATE State, SOCKET Socket, const UCHAR *Query, int Length, const SOCKADDR_INET *From,
                        int FromLength, const char *Host)
{
    UCHAR Reply[512];
    int Offset = 12, Size, QuestionEnd;
    USHORT Type, Class;
    DWORD Index = 0;
    BOOL LinkLocal = FALSE, Legacy, Have4 = FALSE, Have6 = FALSE;
    SOCKADDR_INET Address4, Address6, To;
    int ToLength;

    UNREFERENCED_PARAMETER(State);

    /* A query: QR clear and opcode 0, response code 0 (RFC 6762 18.3, 18.11), at least one question */
    if (Length < 12 || (Query[2] & 0xf8) || (Query[3] & 0x0f) || (Query[4] == 0 && Query[5] == 0))
        return;
    if (!MdnsNameMatches(Query, Length, &Offset, Host) || Offset + 4 > Length)
        return;
    Type = (Query[Offset] << 8) | Query[Offset + 1];
    /* The top bit of the class is the unicast-response request, not part of the class */
    Class = ((Query[Offset + 2] << 8) | Query[Offset + 3]) & 0x7fff;
    Offset += 4;
    QuestionEnd = Offset;
    if (Class != 1 && Class != 255)
        return;

    if (From->si_family == AF_INET6)
    {
        LinkLocal = IN6_IS_ADDR_LINKLOCAL(&From->Ipv6.sin6_addr);
        if (LinkLocal)
            Index = From->Ipv6.sin6_scope_id;
        else if (GetBestInterfaceEx((struct sockaddr *)&From->Ipv6, &Index))
            return;
    }
    else if (GetBestInterface(From->Ipv4.sin_addr.s_addr, &Index))
    {
        return;
    }

    if (Type == 1 || Type == 255)
        Have4 = LlmnrInterfaceAddress(Index, AF_INET, FALSE, &Address4);
    if (Type == 28 || Type == 255)
        Have6 = LlmnrInterfaceAddress(Index, AF_INET6, LinkLocal, &Address6);
    /* No NSEC negative answers yet: a name we hold with no record of that type gets silence */
    if (!Have4 && !Have6)
        return;

    Legacy = (From->si_family == AF_INET6 ? From->Ipv6.sin6_port : From->Ipv4.sin_port) != htons(MDNS_PORT);
    if (Legacy)
    {
        /* RFC 6762 6.7: same ID, question repeated, unicast back to the asker */
        memcpy(Reply, Query, QuestionEnd);
        Reply[4] = 0;
        Reply[5] = 1;
        Size = QuestionEnd;
    }
    else
    {
        memset(Reply, 0, 12);
        Size = 12;
    }
    Reply[2] = 0x84;
    Reply[3] = 0;
    Reply[6] = 0;
    Reply[7] = (Have4 ? 1 : 0) + (Have6 ? 1 : 0);
    Reply[8] = Reply[9] = Reply[10] = Reply[11] = 0;
    if (Have4)
        Size = MdnsPutRecord(Reply, Size, Legacy, Host, 1, &Address4);
    if (Have6)
        Size = MdnsPutRecord(Reply, Size, Legacy, Host, 28, &Address6);

    To = *From;
    ToLength = FromLength;
    if (!Legacy)
    {
        memset(&To, 0, sizeof(To));
        if (From->si_family == AF_INET6)
        {
            DWORD Interface = Index;

            To.Ipv6.sin6_family = AF_INET6;
            To.Ipv6.sin6_port = htons(MDNS_PORT);
            To.Ipv6.sin6_addr = MdnsGroup6;
            To.Ipv6.sin6_scope_id = Index;
            ToLength = sizeof(To.Ipv6);
            setsockopt(Socket, IPPROTO_IPV6, IPV6_MULTICAST_IF, (const char *)&Interface, sizeof(Interface));
        }
        else
        {
            To.Ipv4.sin_family = AF_INET;
            To.Ipv4.sin_port = htons(MDNS_PORT);
            To.Ipv4.sin_addr.s_addr = htonl(MDNS_GROUP4);
            ToLength = sizeof(To.Ipv4);
            if (Have4)
                setsockopt(Socket, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&Address4.Ipv4.sin_addr, sizeof(Address4.Ipv4.sin_addr));
        }
    }
    sendto(Socket, (const char *)Reply, Size, 0, (const struct sockaddr *)&To, ToLength);
}

static SOCKET MdnsOpenSocket(ADDRESS_FAMILY Family, HANDLE Event)
{
    SOCKADDR_INET Address;
    DWORD V6Only = TRUE, Reuse = TRUE, Hops = 255;
    SOCKET Socket = socket(Family, SOCK_DGRAM, IPPROTO_UDP);

    if (Socket == INVALID_SOCKET)
        return INVALID_SOCKET;
    /* Other mDNS software on the machine shares the port (RFC 6762 15.1) */
    setsockopt(Socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&Reuse, sizeof(Reuse));
    memset(&Address, 0, sizeof(Address));
    Address.si_family = Family;
    if (Family == AF_INET6)
    {
        Address.Ipv6.sin6_port = htons(MDNS_PORT);
        setsockopt(Socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&V6Only, sizeof(V6Only));
        setsockopt(Socket, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (const char *)&Hops, sizeof(Hops));
    }
    else
    {
        Address.Ipv4.sin_port = htons(MDNS_PORT);
        setsockopt(Socket, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&Hops, sizeof(Hops));
    }
    if (bind(Socket, (struct sockaddr *)&Address, Family == AF_INET6 ? sizeof(Address.Ipv6) : sizeof(Address.Ipv4)) ||
        WSAEventSelect(Socket, Event, FD_READ))
    {
        closesocket(Socket);
        return INVALID_SOCKET;
    }
    return Socket;
}

static VOID MdnsDrain(PMDNS_STATE State, SOCKET Socket, const char *Host)
{
    SOCKADDR_INET From;
    UCHAR Packet[1500];
    int Length, FromLength;

    if (Socket == INVALID_SOCKET)
        return;
    for (;;)
    {
        FromLength = sizeof(From);
        Length = recvfrom(Socket, (char *)Packet, sizeof(Packet), 0, (struct sockaddr *)&From, &FromLength);
        if (Length == SOCKET_ERROR)
            break;
        MdnsRespond(State, Socket, Packet, Length, &From, FromLength, Host);
    }
}

static DWORD WINAPI MdnsThreadRoutine(LPVOID Parameter)
{
    MDNS_STATE State;
    WSADATA Data;
    HANDLE Events[3], Change = NULL;
    OVERLAPPED Overlapped;
    char Host[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD HostLength = sizeof(Host), Wait;

    UNREFERENCED_PARAMETER(Parameter);

    if (!GetComputerNameA(Host, &HostLength))
        return 0;
    if (WSAStartup(MAKEWORD(2, 2), &Data))
        return 0;

    memset(&State, 0, sizeof(State));
    State.Socket4 = State.Socket6 = INVALID_SOCKET;
    Events[0] = MdnsStopEvent;
    Events[1] = WSACreateEvent();
    Events[2] = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (Events[1] == WSA_INVALID_EVENT || !Events[2])
        goto done;
    State.Socket4 = MdnsOpenSocket(AF_INET, Events[1]);
    State.Socket6 = MdnsOpenSocket(AF_INET6, Events[1]);
    if (State.Socket4 == INVALID_SOCKET && State.Socket6 == INVALID_SOCKET)
        goto done;

    for (;;)
    {
        memset(&Overlapped, 0, sizeof(Overlapped));
        Overlapped.hEvent = Events[2];
        NotifyAddrChange(&Change, &Overlapped);
        MdnsJoinInterfaces(&State);

        Wait = WaitForMultipleObjects(3, Events, FALSE, INFINITE);
        if (Wait == WAIT_OBJECT_0)
            break;
        if (Wait == WAIT_OBJECT_0 + 2)
            continue;
        CancelIPChangeNotify(&Overlapped);
        WSAResetEvent(Events[1]);
        MdnsDrain(&State, State.Socket4, Host);
        MdnsDrain(&State, State.Socket6, Host);
    }
    CancelIPChangeNotify(&Overlapped);

done:
    if (State.Socket4 != INVALID_SOCKET)
        closesocket(State.Socket4);
    if (State.Socket6 != INVALID_SOCKET)
        closesocket(State.Socket6);
    if (Events[1] != WSA_INVALID_EVENT)
        WSACloseEvent(Events[1]);
    if (Events[2])
        CloseHandle(Events[2]);
    WSACleanup();
    return 0;
}

VOID MdnsStart(VOID)
{
    MdnsStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!MdnsStopEvent)
        return;
    MdnsThread = CreateThread(NULL, 0, MdnsThreadRoutine, NULL, 0, NULL);
}

VOID MdnsStop(VOID)
{
    if (!MdnsStopEvent)
        return;
    SetEvent(MdnsStopEvent);
    if (MdnsThread)
    {
        WaitForSingleObject(MdnsThread, 5000);
        CloseHandle(MdnsThread);
        MdnsThread = NULL;
    }
}
