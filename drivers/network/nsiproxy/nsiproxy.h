/*
 * PROJECT:     LiberNT Network Store Interface proxy driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Serves the \Device\Nsi ioctls of nsi.dll from the netio.sys network store
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The wire format is defined once, in sdk/include/wine/nsi.h, which is a
 * user-mode header: the four typedefs below give it the Win32 names it uses
 * (UINT comes from ndis.h) and _WINIOCTL_ keeps the user-mode ioctl header
 * out, since wdm.h already defines CTL_CODE and FILE_DEVICE_NETWORK.
 */

#ifndef _NSIPROXY_H_
#define _NSIPROXY_H_

#include <ntifs.h>
#include <ndis.h>
#include <netiodef.h>
#include <ws2def.h>
#include <ws2ipdef.h>
#include <nsikm.h>

typedef UCHAR BYTE;
typedef ULONG DWORD;
typedef struct _OVERLAPPED OVERLAPPED;
#define WINAPI NTAPI

#define _WINIOCTL_
#include <wine/nsi.h>

#define TAG_NSIPROXY 'xPsN'
#define NSIPROXY_MAX_KEY_SIZE 1024

#endif
