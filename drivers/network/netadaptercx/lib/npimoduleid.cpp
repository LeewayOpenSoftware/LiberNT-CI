/*
 * PROJECT:     LiberNT NetAdapterCx
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Network module identifier NetAdapterCx registers with Packet Monitor
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif193.001@gmail.com>
 */

#include <netiodef.h>

EXTERN_C
CONST NPI_MODULEID NPI_MS_NDIS_TRANSLATOR_MODULEID =
{
    sizeof(NPI_MODULEID),
    MIT_GUID,
    { 0xeb004a33, 0x9b1a, 0x11d4, { 0x91, 0x23, 0x00, 0x50, 0x04, 0x77, 0x59, 0xbc } }
};
