/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS TCP/IP protocol driver
 * FILE:        tcpip/ainfo.c
 * PURPOSE:     Per-socket information.
 * PROGRAMMER:  Cameron Gutman
 */

#include "precomp.h"

TDI_STATUS SetAddressFileInfo(TDIObjectID *ID,
                              PADDRESS_FILE AddrFile,
                              PVOID Buffer,
                              UINT BufferSize)
{
    switch (ID->toi_id)
    {
      case AO_OPTION_MCASTIF:
      {
         ULONG Selector;
         PIP_INTERFACE Interface;
         if (BufferSize < sizeof(Selector)) return TDI_INVALID_PARAMETER;
         RtlCopyMemory(&Selector, Buffer, sizeof(Selector));
         if (Selector)
         {
             Interface = GetMulticastInterface(Selector);
             if (!Interface) return TDI_INVALID_PARAMETER;
             IPDereferenceInterface(Interface);
         }
         LockObject(AddrFile);
         AddrFile->MulticastInterface = Selector;
         UnlockObject(AddrFile);
         return TDI_SUCCESS;
      }

      case AO_OPTION_IP_UCASTIF:
      {
         ULONG Selector;
         PIP_INTERFACE Interface;
         if (BufferSize < sizeof(Selector)) return TDI_INVALID_PARAMETER;
         RtlCopyMemory(&Selector, Buffer, sizeof(Selector));
         if (Selector)
         {
             if (DN2H(Selector) >= 0x01000000) return TDI_INVALID_PARAMETER;
             Interface = GetMulticastInterface(Selector);
             if (!Interface) return TDI_INVALID_PARAMETER;
             IPDereferenceInterface(Interface);
         }
         LockObject(AddrFile);
         AddrFile->UnicastInterface = Selector;
         UnlockObject(AddrFile);
         return TDI_SUCCESS;
      }

      case AO_OPTION_ADD_MCAST:
      case AO_OPTION_DEL_MCAST:
      {
         ULONG Request[2];
         PIP_INTERFACE Interface;
         ULONG Index;

         if (AddrFile->Family == TDI_ADDRESS_TYPE_IP6 && BufferSize >= 16 + sizeof(ULONG))
         {
             RtlCopyMemory(&Index, (PUCHAR)Buffer + 16, sizeof(Index));
             return LibIP6ChangeMembership(AddrFile, Index, Buffer, ID->toi_id == AO_OPTION_ADD_MCAST);
         }
         if (BufferSize < sizeof(Request)) return TDI_INVALID_PARAMETER;
         RtlCopyMemory(Request, Buffer, sizeof(Request));
         Interface = GetMulticastInterface(Request[1]);
         if (!Interface) return TDI_INVALID_PARAMETER;
         Index = Interface->Index;
         IPDereferenceInterface(Interface);
         if (ID->toi_id == AO_OPTION_ADD_MCAST)
             return IGMPJoinGroup(AddrFile, Index, Request[0]);
         return IGMPLeaveGroup(AddrFile, Index, Request[0]);
      }

      case AO_OPTION_XSUM:
         if (BufferSize < sizeof(ULONG))
             return TDI_INVALID_PARAMETER;

         LockObject(AddrFile);
         AddrFile->NoChecksum = (*(PULONG)Buffer == 0);
         UnlockObject(AddrFile);

         return TDI_SUCCESS;

      case AO_OPTION_UDP_SEND_MSG_SIZE:
         if (BufferSize < sizeof(ULONG))
             return TDI_INVALID_PARAMETER;

         LockObject(AddrFile);
         AddrFile->SendMsgSize = *(PULONG)Buffer;
         UnlockObject(AddrFile);

         return TDI_SUCCESS;

      case AO_OPTION_IP_PKTINFO:
      case AO_OPTION_IP_RECVTTL:
      case AO_OPTION_IP_RECVTOS:
      case AO_OPTION_IP_RECVECN:
      case AO_OPTION_IP_RECVDSTADDR:
      {
         BOOLEAN Enable;

         if (BufferSize < sizeof(ULONG))
             return TDI_INVALID_PARAMETER;

         Enable = (*(PULONG)Buffer != 0);
         LockObject(AddrFile);
         switch (ID->toi_id)
         {
            case AO_OPTION_IP_PKTINFO: AddrFile->RecvPktInfo = Enable; break;
            case AO_OPTION_IP_RECVTTL: AddrFile->RecvTtl = Enable; break;
            case AO_OPTION_IP_RECVTOS: AddrFile->RecvTos = Enable; break;
            case AO_OPTION_IP_RECVECN: AddrFile->RecvEcn = Enable; break;
            default: AddrFile->RecvDstAddr = Enable; break;
         }
         UnlockObject(AddrFile);

         return TDI_SUCCESS;
      }

      case AO_OPTION_MCASTTTL:
         if (BufferSize < sizeof(UINT) || *(PUINT)Buffer > 255)
             return TDI_INVALID_PARAMETER;
         LockObject(AddrFile);
         AddrFile->MulticastTTL = (UCHAR)*(PUINT)Buffer;
         UnlockObject(AddrFile);
         return TDI_SUCCESS;

      case AO_OPTION_TTL:
         if (BufferSize < sizeof(UINT))
             return TDI_INVALID_PARAMETER;

         LockObject(AddrFile);
         AddrFile->TTL = *((PUCHAR)Buffer);
         UnlockObject(AddrFile);

         return TDI_SUCCESS;

      case AO_OPTION_IP_DONTFRAGMENT:
         if (BufferSize < sizeof(UINT))
             return TDI_INVALID_PARAMETER;

         LockObject(AddrFile);
         AddrFile->DF = *((PUINT)Buffer);
         UnlockObject(AddrFile);

         return TDI_SUCCESS;

      case AO_OPTION_BROADCAST:
         if (BufferSize < sizeof(UINT))
             return TDI_INVALID_PARAMETER;

         LockObject(AddrFile);
         AddrFile->BCast = *((PUINT)Buffer);
         UnlockObject(AddrFile);

         return TDI_SUCCESS;

      case AO_OPTION_IP_HDRINCL:
         if (BufferSize < sizeof(UINT))
             return TDI_INVALID_PARAMETER;

         LockObject(AddrFile);
         AddrFile->HeaderIncl = *((PUINT)Buffer);
         UnlockObject(AddrFile);

         return TDI_SUCCESS;

      default:
         DbgPrint("Unimplemented option %x\n", ID->toi_id);

         return TDI_INVALID_REQUEST;
    }
}

TDI_STATUS GetAddressFileInfo(TDIObjectID *ID,
                              PADDRESS_FILE AddrFile,
                              PVOID Buffer,
                              PUINT BufferSize)
{
    ULONG Value;

    /* The options that SetAddressFileInfo stores are read back from where
     * it stored them. Each one is returned as a ULONG. */
    LockObject(AddrFile);
    switch (ID->toi_id)
    {
      case AO_OPTION_MCASTIF: Value = AddrFile->MulticastInterface; break;
      case AO_OPTION_IP_UCASTIF: Value = AddrFile->UnicastInterface; break;
      case AO_OPTION_XSUM: Value = !AddrFile->NoChecksum; break;
      case AO_OPTION_UDP_SEND_MSG_SIZE: Value = AddrFile->SendMsgSize; break;
      case AO_OPTION_IP_PKTINFO: Value = AddrFile->RecvPktInfo; break;
      case AO_OPTION_IP_RECVTTL: Value = AddrFile->RecvTtl; break;
      case AO_OPTION_IP_RECVTOS: Value = AddrFile->RecvTos; break;
      case AO_OPTION_IP_RECVECN: Value = AddrFile->RecvEcn; break;
      case AO_OPTION_IP_RECVDSTADDR: Value = AddrFile->RecvDstAddr; break;
      case AO_OPTION_MCASTTTL: Value = AddrFile->MulticastTTL; break;
      case AO_OPTION_TTL: Value = AddrFile->TTL; break;
      case AO_OPTION_IP_DONTFRAGMENT: Value = AddrFile->DF; break;
      case AO_OPTION_BROADCAST: Value = AddrFile->BCast; break;
      case AO_OPTION_IP_HDRINCL: Value = AddrFile->HeaderIncl; break;

      default:
         UnlockObject(AddrFile);
         return TDI_INVALID_REQUEST;
    }
    UnlockObject(AddrFile);

    return InfoCopyOut((PCHAR)&Value, sizeof(Value), Buffer, BufferSize);
}
