// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#define NETEXPORT(a) imp_ ## a

#include <netadapter.h>
#include <netadapter_p.h>
#include <nettxqueue_p.h>
#include <netadapteroffload.h>

#include <nxdynamics.h>
#include <netclientadapter.h>

static_assert(NetPacketFilterFlagDirected == NDIS_PACKET_TYPE_DIRECTED, "Inconsistent NetPacketFilterFlagDirected value");
static_assert(NetPacketFilterFlagMulticast == NDIS_PACKET_TYPE_MULTICAST, "Inconsistent NetPacketFilterFlagMulticast value");
static_assert(NetPacketFilterFlagAllMulticast == NDIS_PACKET_TYPE_ALL_MULTICAST, "Inconsistent NetPacketFilterFlagAllMulticast value");
static_assert(NetPacketFilterFlagBroadcast == NDIS_PACKET_TYPE_BROADCAST, "Inconsistent NetPacketFilterFlagBroadcast value");
static_assert(NetPacketFilterFlagPromiscuous == NDIS_PACKET_TYPE_PROMISCUOUS, "Inconsistent NetPacketFilterFlagPromiscuous value");

static_assert(static_cast<ULONG>(NetAdapterPauseFunctionTypeUnsupported) == static_cast<ULONG>(NdisPauseFunctionsUnsupported));
static_assert(static_cast<ULONG>(NetAdapterPauseFunctionTypeSendOnly) == static_cast<ULONG>(NdisPauseFunctionsSendOnly));
static_assert(static_cast<ULONG>(NetAdapterPauseFunctionTypeReceiveOnly) == static_cast<ULONG>(NdisPauseFunctionsReceiveOnly));
static_assert(static_cast<ULONG>(NetAdapterPauseFunctionTypeSendAndReceive) == static_cast<ULONG>(NdisPauseFunctionsSendAndReceive));
static_assert(static_cast<ULONG>(NetAdapterPauseFunctionTypeUnknown) == static_cast<ULONG>(NdisPauseFunctionsUnknown));

static_assert(NetAdapterAutoNegotiationFlagNone == 0);
static_assert(NetAdapterAutoNegotiationFlagXmitLinkSpeedAutoNegotiated == NDIS_LINK_STATE_XMIT_LINK_SPEED_AUTO_NEGOTIATED);
static_assert(NetAdapterAutoNegotiationFlagRcvLinkSpeedautoNegotiated == NDIS_LINK_STATE_RCV_LINK_SPEED_AUTO_NEGOTIATED);
static_assert(NetAdapterAutoNegotiationFlagDuplexAutoNegotiated == NDIS_LINK_STATE_DUPLEX_AUTO_NEGOTIATED);
static_assert(NetAdapterAutoNegotiationFlagPauseFunctionsAutoNegotiated == NDIS_LINK_STATE_PAUSE_FUNCTIONS_AUTO_NEGOTIATED);

static_assert(sizeof(NDIS_IF_PHYSICAL_ADDRESS) == sizeof(NET_ADAPTER_LINK_LAYER_ADDRESS));
static_assert(RTL_SIZEOF_THROUGH_FIELD(NDIS_IF_PHYSICAL_ADDRESS, Address) == RTL_SIZEOF_THROUGH_FIELD(NET_ADAPTER_LINK_LAYER_ADDRESS, Address));

static_assert(NET_ADAPTER_LINK_SPEED_UNKNOWN == NDIS_LINK_SPEED_UNKNOWN);

static_assert(NetAdapterWakeMagicPatternId == NDIS_PM_MAGIC_PATTERN_ID);
static_assert(NetAdapterWakeEapolPatternId == NDIS_PM_EAPOL_PATTERN_ID);
#ifdef NDIS_PM_FILTER_PATTERN_ID
static_assert(NetAdapterWakeFilterPatternId == NDIS_PM_FILTER_PATTERN_ID);
#endif

static_assert(static_cast<ULONG>(NetAdapterOffloadLayer3FlagIPv4NoOptions) == static_cast<ULONG>(NetClientAdapterOffloadLayer3FlagIPv4NoOptions));
static_assert(static_cast<ULONG>(NetAdapterOffloadLayer3FlagIPv4WithOptions) == static_cast<ULONG>(NetClientAdapterOffloadLayer3FlagIPv4WithOptions));
static_assert(static_cast<ULONG>(NetAdapterOffloadLayer3FlagIPv6NoExtensions) == static_cast<ULONG>(NetClientAdapterOffloadLayer3FlagIPv6NoExtensions));
static_assert(static_cast<ULONG>(NetAdapterOffloadLayer3FlagIPv6WithExtensions) == static_cast<ULONG>(NetClientAdapterOffloadLayer3FlagIPv6WithExtensions));

static_assert(static_cast<ULONG>(NetAdapterOffloadLayer4FlagTcpNoOptions) == static_cast<ULONG>(NetClientAdapterOffloadLayer4FlagTcpNoOptions));
static_assert(static_cast<ULONG>(NetAdapterOffloadLayer4FlagTcpWithOptions) == static_cast<ULONG>(NetClientAdapterOffloadLayer4FlagTcpWithOptions));
static_assert(static_cast<ULONG>(NetAdapterOffloadLayer4FlagUdp) == static_cast<ULONG>(NetClientAdapterOffloadLayer4FlagUdp));

#ifdef _KERNEL_MODE
static_assert(static_cast<ULONG>(NetAdapterReceiveScalingHashTypeNone) == static_cast<ULONG>(NetClientAdapterReceiveScalingHashTypeNone));
static_assert(static_cast<ULONG>(NetAdapterReceiveScalingHashTypeToeplitz) == static_cast<ULONG>(NetClientAdapterReceiveScalingHashTypeToeplitz));

static_assert(static_cast<ULONG>(NetAdapterReceiveScalingProtocolTypeNone) == static_cast<ULONG>(NetClientAdapterReceiveScalingProtocolTypeNone));
static_assert(static_cast<ULONG>(NetAdapterReceiveScalingProtocolTypeIPv4) == static_cast<ULONG>(NetClientAdapterReceiveScalingProtocolTypeIPv4));
static_assert(static_cast<ULONG>(NetAdapterReceiveScalingProtocolTypeIPv4Options) == static_cast<ULONG>(NetClientAdapterReceiveScalingProtocolTypeIPv4Options));
static_assert(static_cast<ULONG>(NetAdapterReceiveScalingProtocolTypeIPv6) == static_cast<ULONG>(NetClientAdapterReceiveScalingProtocolTypeIPv6));
static_assert(static_cast<ULONG>(NetAdapterReceiveScalingProtocolTypeIPv6Extensions) == static_cast<ULONG>(NetClientAdapterReceiveScalingProtocolTypeIPv6Extensions));
static_assert(static_cast<ULONG>(NetAdapterReceiveScalingProtocolTypeTcp) == static_cast<ULONG>(NetClientAdapterReceiveScalingProtocolTypeTcp));
static_assert(static_cast<ULONG>(NetAdapterReceiveScalingProtocolTypeUdp) == static_cast<ULONG>(NetClientAdapterReceiveScalingProtocolTypeUdp));
#endif
