/*
 * PROJECT:     LiberNT Kernel-Mode Driver Framework
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     USB host Controller eXtension (UCX) class library -- URB shape
 *              check at the root hub PDO boundary. Every URB a hub or class
 *              driver submits passes UcxUrbCheck() before any field beyond the
 *              header is read by the dispatchers. The check depends only on
 *              the URB bytes and a few limits, so it also builds as a plain
 *              user-mode function (UCX_URBCHECK_HOST) for the host test.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#pragma once

#ifdef UCX_URBCHECK_HOST
#include <windows.h>
#include <usb.h>
#else
#include <ntddk.h>
#include <usb.h>
#endif

/* What the dispatchers can carry; a URB asking for more is refused up front */
typedef struct _UCX_URB_LIMITS
{
    ULONG MaxControlTransfer;      /* bytes in one control data stage */
    ULONG MaxBulkTransfer;         /* bytes in one bulk/interrupt transfer */
    ULONG MaxIsochPackets;         /* packets in one isochronous URB */
    ULONG MaxInterfaces;           /* USBD_INTERFACE_INFORMATION entries in one select */
    ULONG MaxPipesPerInterface;
} UCX_URB_LIMITS, *PUCX_URB_LIMITS;

/*
 * Returns USBD_STATUS_SUCCESS when every field the function implies lies
 * inside UrbHeader.Length and holds a value the dispatchers accept;
 * USBD_STATUS_INVALID_URB_FUNCTION for a function this library does not
 * carry; USBD_STATUS_INVALID_PIPE_HANDLE for a NULL pipe where one is
 * needed; USBD_STATUS_INVALID_PARAMETER otherwise. Reads nothing past
 * UrbHeader.Length and follows no pointer inside the URB.
 */
USBD_STATUS
UcxUrbCheck(
    _In_reads_bytes_(Urb->UrbHeader.Length) const URB *Urb,
    _In_ const UCX_URB_LIMITS *Limits);

/* Name of a URB function for logs, "?" when unknown */
const char *
UcxUrbFunctionName(
    _In_ USHORT Function);
