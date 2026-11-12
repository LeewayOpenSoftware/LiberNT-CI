/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS TCP/IP protocol driver
 * FILE:        network/icmp.c
 * PURPOSE:     Internet Control Message Protocol routines
 * PROGRAMMERS: Casper S. Hornstrup (chorns@users.sourceforge.net)
 * REVISIONS:
 *   CSH 01/08-2000 Created
 */

#include "precomp.h"

#include <icmp.h>

/* IPv4 ICMP MIB counters (served via ICMP_MIB_STATS_ID). Plain increments at
 * dispatch/DPC level, matching the UDPStats/IPStats convention in this driver
 * (stats are advisory; no interlock). */
ICMP_STATISTICS ICMPStats = { 0 };

static VOID ICMPCountType(PICMP_MIB_STATS Stats, UCHAR Type)
{
    switch (Type)
    {
        case ICMP_TYPE_ECHO_REPLY:        Stats->dwEchoReps++; break;
        case ICMP_TYPE_DEST_UNREACH:      Stats->dwDestUnreachs++; break;
        case ICMP_TYPE_SOURCE_QUENCH:     Stats->dwSrcQuenchs++; break;
        case ICMP_TYPE_REDIRECT:          Stats->dwRedirects++; break;
        case ICMP_TYPE_ECHO_REQUEST:      Stats->dwEchos++; break;
        case ICMP_TYPE_TIME_EXCEEDED:     Stats->dwTimeExcds++; break;
        case ICMP_TYPE_PARAMETER:         Stats->dwParmProbs++; break;
        case ICMP_TYPE_TIMESTAMP_REQUEST: Stats->dwTimestamps++; break;
        case ICMP_TYPE_TIMESTAMP_REPLY:   Stats->dwTimestampReps++; break;
        case 17 /* ICMP address mask request */: Stats->dwAddrMasks++; break;
        case 18 /* ICMP address mask reply   */: Stats->dwAddrMaskReps++; break;
        default: break;
    }
}

NTSTATUS ICMPStartup()
{
    RdiscStartup();
    PmtuStartup();
    IPRegisterProtocol(IPPROTO_ICMP, ICMPReceive);

    return STATUS_SUCCESS;
}

NTSTATUS ICMPShutdown()
{
    IPRegisterProtocol(IPPROTO_ICMP, NULL);

    return STATUS_SUCCESS;
}

NTSTATUS ICMPSendDatagram(
    PADDRESS_FILE AddrFile,
    PTDI_CONNECTION_INFORMATION ConnInfo,
    PCHAR BufferData,
    ULONG DataSize,
    PULONG DataUsed )
/*
 * FUNCTION: Sends an ICMP datagram to a remote address
 * ARGUMENTS:
 *     Request   = Pointer to TDI request
 *     ConnInfo  = Pointer to connection information
 *     Buffer    = Pointer to NDIS buffer with data
 *     DataSize  = Size in bytes of data to be sent
 * RETURNS:
 *     Status of operation
 */
{
    NTSTATUS Status;

    TI_DbgPrint(DEBUG_ICMP, ("Sending ICMP datagram (0x%x)\n", AddrFile));

    ICMPStats.Out.dwMsgs++;
    if (BufferData && DataSize >= 1)
        ICMPCountType(&ICMPStats.Out, (UCHAR)BufferData[0]);

    /* just forward the call to RawIP handler */
    Status = RawIPSendDatagram(AddrFile, ConnInfo, BufferData, DataSize, DataUsed);
    if (!NT_SUCCESS(Status))
        ICMPStats.Out.dwErrors++;
    return Status;
}


VOID ICMPReceive(
    PIP_INTERFACE Interface,
    PIP_PACKET IPPacket)
/*
 * FUNCTION: Receives an ICMP packet
 * ARGUMENTS:
 *     NTE      = Pointer to net table entry which the packet was received on
 *     IPPacket = Pointer to an IP packet that was received
 */
{
    PICMP_HEADER ICMPHeader = (PICMP_HEADER)IPPacket->Data;
    UINT32 DataSize = IPPacket->TotalSize - IPPacket->HeaderSize;

    ICMPStats.In.dwMsgs++;

    TI_DbgPrint(DEBUG_ICMP, ("ICMPReceive: Size (%d) HeaderSize (%d) Type (%d) Code (%d) Checksum (0x%x)\n",
        IPPacket->TotalSize, IPPacket->HeaderSize, ICMPHeader->Type, ICMPHeader->Code, ICMPHeader->Checksum));

    /* Discard too short packets */
    if (DataSize < sizeof(ICMP_HEADER))
    {
        ICMPStats.In.dwErrors++;
        TI_DbgPrint(DEBUG_ICMP, ("Packet doesn't fit ICMP header. Discarded\n"));
        return;
    }

    /* Discard packets with bad checksum */
    if (!IPv4CorrectChecksum(IPPacket->Data, DataSize))
    {
        ICMPStats.In.dwErrors++;
        TI_DbgPrint(DEBUG_ICMP, ("Bad ICMP checksum. Packet discarded\n"));
        return;
    }

    ICMPCountType(&ICMPStats.In, ICMPHeader->Type);

    RawIpReceive(Interface, IPPacket);

    if (ICMPHeader->Type == ICMP_TYPE_ECHO_REQUEST)
    {
        ICMPReply(Interface, IPPacket, ICMP_TYPE_ECHO_REPLY, 0);
    }
    else if (ICMPHeader->Type == ICMP_TYPE_ROUTER_ADVERT)
    {
        RdiscReceive(Interface, IPPacket);
    }
    else if (ICMPHeader->Type == ICMP_TYPE_DEST_UNREACH && ICMPHeader->Code == ICMP_CODE_DU_FRAG_DF_SET)
    {
        PmtuReceive(Interface, IPPacket);
    }
}

VOID ICMPReply(
    PIP_INTERFACE Interface,
    PIP_PACKET IPPacket,
    UCHAR Type,
    UCHAR Code)
/*
 * FUNCTION: Transmits an ICMP packet in response to an incoming packet
 * ARGUMENTS:
 *     NTE      = Pointer to net table entry to use
 *     IPPacket = Pointer to IP packet that was received
 *     Type     = ICMP message type
 *     Code     = ICMP message code
 * NOTES:
 *     We have received a packet from someone and is unable to
 *     process it due to error(s) in the packet or we have run out
 *     of resources. We transmit an ICMP message to the host to
 *     notify him of the problem
 */
{
    UINT DataSize;
    IP_PACKET NewPacket;
    ADDRESS_FILE FakeAddrFile;
    PNEIGHBOR_CACHE_ENTRY NCE;
    IP_ADDRESS Source;

    TI_DbgPrint(DEBUG_ICMP, ("Called. Type (%d)  Code (%d).\n", Type, Code));

    DataSize = IPPacket->TotalSize - IPPacket->HeaderSize;

    /* First check if we have a route to sender */
    NCE = RouteGetRouteToDestination(&IPPacket->SrcAddr);
    if (!NCE)
    {
        ICMPStats.Out.dwErrors++;
        return;
    }

    if (IsInterfaceAddress(Interface, &IPPacket->DstAddr))
        Source = IPPacket->DstAddr;
    else
        GetInterfaceSourceAddress(Interface, &IPPacket->SrcAddr, &NCE->Address, &Source);

    /* This is the only data needed to generate a packet */
    RtlZeroMemory(&FakeAddrFile, sizeof(FakeAddrFile));
    FakeAddrFile.Protocol = IPPROTO_ICMP;
    FakeAddrFile.TTL = 128;

    if (!NT_SUCCESS(BuildRawIpPacket(
        &FakeAddrFile, &NewPacket, &IPPacket->SrcAddr, 0, &Source, 0, IPPacket->Data, DataSize)))
    {
        ICMPStats.Out.dwErrors++;
        NBDereferenceNeighbor(NCE);
        return;
    }

    ((PICMP_HEADER)NewPacket.Data)->Type     = Type;
    ((PICMP_HEADER)NewPacket.Data)->Code     = Code;
    ((PICMP_HEADER)NewPacket.Data)->Checksum = 0;
    ((PICMP_HEADER)NewPacket.Data)->Checksum = (USHORT)IPv4Checksum(NewPacket.Data, DataSize, 0);

    ICMPStats.Out.dwMsgs++;
    ICMPCountType(&ICMPStats.Out, Type);

    IPSendDatagram(&NewPacket, NCE);
    NBDereferenceNeighbor(NCE);
}

/* EOF */
