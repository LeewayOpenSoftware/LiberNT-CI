/*
 * PROJECT:     LiberNT test drivers
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     flushfail: a volume filter that models a disk with a volatile
 *              write cache that rejects cache flushes and has no FUA.
 *              Shared between the driver and its loader tool.
 */

#pragma once

#define FLUSHFAIL_DEVICE_NAME   L"\\Device\\FlushFail"
#define FLUSHFAIL_DOS_NAME      L"\\DosDevices\\FlushFail"
#define FLUSHFAIL_WIN32_NAME    L"\\\\.\\FlushFail"

#define FILE_DEVICE_FLUSHFAIL   0x9f1f

/* Input: the NT name of the volume device to attach to, as a NUL-terminated
 * UTF-16 string (for example \Device\HarddiskVolume4). */
#define IOCTL_FLUSHFAIL_ATTACH \
    CTL_CODE(FILE_DEVICE_FLUSHFAIL, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Output: FLUSHFAIL_STATS. */
#define IOCTL_FLUSHFAIL_STATS \
    CTL_CODE(FILE_DEVICE_FLUSHFAIL, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _FLUSHFAIL_STATS
{
    ULONG Attached;          /* 1 once a volume is filtered */
    ULONG FlushesRejected;   /* IRP_MJ_FLUSH_BUFFERS completed with STATUS_INVALID_DEVICE_REQUEST */
    ULONG CacheQueriesFaked; /* StorageDeviceWriteCacheProperty / IOCTL_DISK_GET_CACHE_INFORMATION answers rewritten */
    ULONG WritesSeen;        /* IRP_MJ_WRITE passed through */
    ULONG WriteThroughSeen;  /* of those, with SL_WRITE_THROUGH (FUA requested although not advertised) */
} FLUSHFAIL_STATS, *PFLUSHFAIL_STATS;
