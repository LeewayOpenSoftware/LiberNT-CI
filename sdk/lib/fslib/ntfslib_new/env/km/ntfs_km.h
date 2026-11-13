#ifndef _NTFS_KM_H_
#define _NTFS_KM_H_

/* For the AttributeType enum; C++ forbids forward-declaring an enum
 * without a fixed underlying type. */
#include <ntfslib_new.h>

#ifdef __cplusplus
extern "C" {
#endif

NTSTATUS
NtfsDiskInitializeKm(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG SectorBytes);

NTSTATUS
NtfsDiskPrepareMountKm(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG SectorBytes);

/* Drops clean blocks cached for a device whose volume is gone (a failed
 * mount, a dismount), and what was learned about the device for media
 * ordering. Dirty blocks stay. */
VOID
NtfsDiskReleaseKm(_In_ PDEVICE_OBJECT DeviceObject);

/* Forget what was learned about a device for media ordering (write cache,
 * FUA, counters); the next use probes it again. Call when a volume on it is
 * released, so a reused device object is never trusted on an old answer. */
VOID
NtfsDiskForgetDeviceKm(_In_ PDEVICE_OBJECT DeviceObject);

/* Writes out metadata held back by the block cache. */
NTSTATUS
NtfsDiskFlushKm(VOID);

/* Reads the environment's registry settings; call from DriverEntry, with no
 * file system lock held. */
VOID
NtfsDiskReadSettingsKm(VOID);

NTSTATUS
NtfsDiskUnloadKm(VOID);

/* Ask the storage device to commit its write cache to the medium. */
NTSTATUS
NtfsDiskFlushDeviceKm(_In_ PDEVICE_OBJECT DeviceObject);

/* Flush one mounted volume without submitting writes to previous devices,
 * then commit the device's write cache. */
NTSTATUS
NtfsDiskFlushVolumeKm(_In_ PDEVICE_OBJECT DeviceObject);

/* Make writes held in the block cache visible to direct storage reads. */
NTSTATUS
NtfsDiskFlushRangeKm(_In_ PDEVICE_OBJECT DeviceObject,
                    _In_ ULONGLONG Offset,
                    _In_ ULONG Length);

/* As NtfsDiskFlushRangeKm, and the bytes are on the medium when it returns:
 * the device's write cache is flushed when the device may hold writes, so a
 * pointer written next cannot overtake the range on power loss. This is the
 * library's CommitRange, and what a write-through write needs. */
NTSTATUS
NtfsDiskCommitRangeKm(_In_ PDEVICE_OBJECT DeviceObject,
                     _In_ ULONGLONG Offset,
                     _In_ ULONG Length);

/* What the environment learned about a device and what it did for media
 * ordering; for tests and for the debugger. Zeroed for an unknown device. */
typedef struct _NTFS_DURABILITY_STATS
{
    BOOLEAN Probed;
    BOOLEAN WriteCache;      /* the device may hold completed writes */
    BOOLEAN Fua;             /* SL_WRITE_THROUGH reaches the medium */
    BOOLEAN FlushRejected;   /* the device refused IRP_MJ_FLUSH_BUFFERS */
    BOOLEAN NotDurable;      /* volatile cache, flush refused, no FUA: ordering is not on the medium */
    ULONG Flushes;           /* device flushes issued for ordering and explicitly */
    ULONG FlushesSkipped;    /* ordering edges that needed no flush */
    ULONG FuaWrites;
    ULONG WritesSinceFlush;
} NTFS_DURABILITY_STATS, *PNTFS_DURABILITY_STATS;

VOID
NtfsDiskDurabilityStatsKm(_In_ PDEVICE_OBJECT DeviceObject,
                          _Out_ PNTFS_DURABILITY_STATS Stats);

NTSTATUS
NtfsDiskReadKm(_In_ PDEVICE_OBJECT DeviceObject,
               _In_ ULONG SectorBytes,
               _In_ ULONGLONG Offset,
               _In_ ULONG Length,
               _Out_writes_bytes_(Length) PVOID Buffer);

NTSTATUS
NtfsDiskWriteKm(_In_ PDEVICE_OBJECT DeviceObject,
                _In_ ULONG SectorBytes,
                _In_ ULONGLONG Offset,
                _In_ ULONG Length,
                _In_reads_bytes_(Length) const VOID* Buffer);

/* Declared here rather than in the public header because
 * FILE_BOTH_DIR_INFORMATION is a kernel-mode type.
 */
NTSTATUS
NtfsDirectoryGetFileBothDirInfo(
    _In_    PNtfsDirectory Dir,
    _In_    BOOLEAN ReturnSingleEntry,
    _In_    BOOLEAN RestartScan,
    _In_    PUNICODE_STRING FileNameFilter,
    _In_    FILE_INFORMATION_CLASS InformationClass,
    _Inout_ PVOID Buffer,
    _Inout_ PULONG BufferLength);

NTSTATUS
NtfsDirectoryStoreInfo(
    _In_ FILE_INFORMATION_CLASS InformationClass,
    _Inout_ PFILE_ID_BOTH_DIR_INFORMATION Info,
    _In_reads_bytes_(NameLength) PCWSTR Name,
    _In_ ULONG NameLength,
    _Out_writes_bytes_(*BufferLength) PVOID Buffer,
    _Inout_ PULONG BufferLength,
    _Out_ PULONG EntrySize);

NTSTATUS
NtfsDirectoryLoadForEnumeration(
    _In_ PNtfsDirectory Dir,
    _In_ PNtfsFileRecord File);

#ifdef __cplusplus
}
#endif

#endif /* _NTFS_KM_H_ */
