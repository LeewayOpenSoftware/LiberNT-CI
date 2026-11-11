/*
 * PROJECT:     ReactOS NTFS library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     A read that meets a corrupt run list leaves the volume dirty.
 *
 * Windows marks a volume dirty when NTFS raises a corruption status, so the
 * next boot checks it. Here a read of a stream whose mapping pairs cannot be
 * valid failed, but a clean dismount then cleared the dirty flag (or never
 * set it, when nothing had been written): the damage stayed unchecked.
 *
 * The test formats a volume and runs two sessions with dirty tracking on:
 * a control that reads $Boot's $DATA and dismounts clean (flag stays 0),
 * then the same after breaking the first mapping-pairs byte of $Boot's
 * $DATA on the medium: the read must fail and the dismount must leave
 * VOL_DIRTY set on the medium.
 *
 * A crash can also come before any write or dismount. The driver commits a
 * noted corruption when it releases the metadata lock
 * (NtfsVolumeCommitCorruptionDirty); a session that reads the broken run
 * list, commits as the driver does and then stops without MarkClean must
 * leave VOL_DIRTY on the medium too. The commit is a no-op in the control.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <ntfslib_new.h>
#include "ntfsformat.h"
#include "ntfs_linux.h"

#define HOST_STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)

#ifndef O_BINARY
#define O_BINARY 0
#endif

namespace
{

struct HostContext
{
    int Descriptor;
};

NTSTATUS
HostWrite(void* Context, ULONGLONG Offset, ULONG Length, const void* Buffer)
{
    HostContext* Host = static_cast<HostContext*>(Context);
    const unsigned char* Bytes = static_cast<const unsigned char*>(Buffer);
    ULONG Done = 0;

    while (Done < Length)
    {
        ssize_t Written = pwrite(Host->Descriptor,
                                 Bytes + Done,
                                 Length - Done,
                                 static_cast<off_t>(Offset + Done));
        if (Written <= 0)
        {
            perror("pwrite");
            return HOST_STATUS_UNSUCCESSFUL;
        }
        Done += static_cast<ULONG>(Written);
    }
    return STATUS_SUCCESS;
}

void*
HostAllocate(void* Context, ULONG Length)
{
    (void)Context;
    return malloc(Length);
}

void
HostFree(void* Context, void* Buffer)
{
    (void)Context;
    free(Buffer);
}

BOOLEAN
HostProgress(void* Context, ULONG Percent)
{
    (void)Context;
    (void)Percent;
    return TRUE;
}

bool
Check(bool Condition, const char* Message)
{
    if (!Condition)
        std::fprintf(stderr, "corrupt_runlist_dirty: %s\n", Message);
    return Condition;
}

bool
FormatImage(const char* Path)
{
    const ULONG BytesPerSector = 512;
    const ULONGLONG SizeBytes = 32ULL * 1024 * 1024;
    HostContext Host;
    NtfsFormatParameters Parameters;
    WCHAR Label[] = { 'C', 'R', 'P', 'T', 0 };
    NTSTATUS Status;

    Host.Descriptor = open(Path, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (Host.Descriptor < 0)
    {
        perror("open");
        return false;
    }
    if (ftruncate(Host.Descriptor, static_cast<off_t>(SizeBytes)) != 0)
    {
        perror("ftruncate");
        close(Host.Descriptor);
        return false;
    }

    std::memset(&Parameters, 0, sizeof(Parameters));
    Parameters.TotalSectors = SizeBytes / BytesPerSector;
    Parameters.BytesPerSector = BytesPerSector;
    Parameters.SectorsPerCluster = 8;
    Parameters.SectorsPerTrack = 63;
    Parameters.NumberOfHeads = 255;
    Parameters.MediaDescriptor = 0xF8;
    Parameters.CurrentTime = (static_cast<ULONGLONG>(time(nullptr)) + 11644473600ULL) * 10000000ULL;
    Parameters.VolumeLabel = Label;
    Parameters.QuickFormat = TRUE;
    Parameters.IoContext = &Host;
    Parameters.Write = HostWrite;
    Parameters.Allocate = HostAllocate;
    Parameters.Free = HostFree;
    Parameters.Progress = HostProgress;

    Status = NtfsVolumeFormat(&Parameters);
    fsync(Host.Descriptor);
    close(Host.Descriptor);
    return Check(NT_SUCCESS(Status), "NtfsVolumeFormat failed");
}

/* Breaks the first mapping-pairs header byte of record Number's $DATA. */
bool
BreakDataRunList(const char* Path, ULONG Number)
{
    int Descriptor = open(Path, O_RDWR | O_BINARY);
    unsigned char Boot[512];
    std::vector<unsigned char> Record;
    ULONGLONG ClusterSize, MftOffset;
    signed char RecordSizeCode;
    ULONG RecordSize, Offset;
    bool Ok = false;

    if (Descriptor < 0)
    {
        perror("open");
        return false;
    }
    if (pread(Descriptor, Boot, sizeof(Boot), 0) != (ssize_t)sizeof(Boot))
        goto Out;
    ClusterSize = (ULONGLONG)(Boot[11] | (Boot[12] << 8)) * Boot[13];
    std::memcpy(&MftOffset, &Boot[0x30], sizeof(MftOffset));
    MftOffset *= ClusterSize;
    RecordSizeCode = (signed char)Boot[0x40];
    RecordSize = RecordSizeCode > 0 ? (ULONG)(RecordSizeCode * ClusterSize) : 1u << -RecordSizeCode;
    MftOffset += (ULONGLONG)Number * RecordSize;
    Record.resize(RecordSize);
    if (pread(Descriptor, Record.data(), RecordSize, (off_t)MftOffset) != (ssize_t)RecordSize ||
        !Check(std::memcmp(Record.data(), "FILE", 4) == 0, "record has no FILE signature"))
    {
        goto Out;
    }
    /* Walk the attributes: type (4), length (4), non-resident flag at 8. */
    Offset = Record[0x14] | (Record[0x15] << 8);
    while (Offset + 0x40 <= RecordSize)
    {
        ULONG Type, Length;

        std::memcpy(&Type, &Record[Offset], 4);
        std::memcpy(&Length, &Record[Offset + 4], 4);
        if (Type == 0xFFFFFFFF || Length == 0 || Offset + Length > RecordSize)
            break;
        if (Type == 0x80 && Record[Offset + 8] != 0)
        {
            ULONG RunsOffset = Record[Offset + 0x20] | (Record[Offset + 0x21] << 8);
            ULONG At = Offset + RunsOffset;

            /* A length field of 15 bytes cannot be valid; skip the update
             * sequence slots at the end of each 512-byte sector. */
            if (!Check(RunsOffset < Length && (At % 512) < 510, "run list not patchable"))
                goto Out;
            Record[At] = 0x0F;
            Ok = pwrite(Descriptor, &Record[At], 1, (off_t)(MftOffset + At)) == 1;
            goto Out;
        }
        Offset += Length;
    }
    Check(false, "no non-resident $DATA in the record");

Out:
    fsync(Descriptor);
    close(Descriptor);
    return Check(Ok, "could not break the run list");
}

/*
 * One session: open, enable dirty tracking, read $Boot's $DATA, dismount
 * clean (MarkClean), close. Returns the read status through ReadStatus.
 */
bool
Session(const char* Path, NTSTATUS* ReadStatus, bool Crash = false)
{
    PNtfsVolume Volume = NULL;
    PNtfsFileRecord Record = NULL;
    ULONG BytesPerSector = 0;
    unsigned char Buffer[512];
    ULONG Length = sizeof(Buffer);
    USHORT Flags = 0xFFFF;
    NTSTATUS Status;
    bool Ok = false;

    Status = NtfsDiskInitializeLinuxWritable(Path, &BytesPerSector);
    if (!Check(NT_SUCCESS(Status), "NtfsDiskInitializeLinuxWritable failed"))
        goto Out;
    Status = NtfsProbePartitionAndOpenVolume(BytesPerSector, &Volume);
    if (!Check(NT_SUCCESS(Status), "volume did not open"))
        goto Out;
    if (!Check(NT_SUCCESS(NtfsVolumeQueryFlags(Volume, &Flags)) && !(Flags & VOL_DIRTY),
               "volume dirty at mount"))
    {
        goto Out;
    }
    if (!Check(NT_SUCCESS(NtfsVolumeEnableDirtyTracking(Volume)), "EnableDirtyTracking failed"))
        goto Out;
    Status = NtfsMasterFileTableGetFileRecordByReference(NtfsVolumeGetMft(Volume), (7ULL << 48) | 7, &Record);
    if (!Check(NT_SUCCESS(Status), "record 7 not read"))
        goto Out;
    *ReadStatus = NtfsFileRecordCopyData(Record, TypeData, NULL, Buffer, &Length, 0);
    std::printf("read $Boot:$DATA: 0x%08x\n", (unsigned)*ReadStatus);
    /* What the driver does on the outermost metadata-lock release; a second
     * call must be harmless. */
    Status = NtfsVolumeCommitCorruptionDirty(Volume);
    if (!Check(NT_SUCCESS(Status), "CommitCorruptionDirty failed") ||
        !Check(NT_SUCCESS(NtfsVolumeCommitCorruptionDirty(Volume)), "second commit failed"))
    {
        goto Out;
    }
    if (Crash)
    {
        /* Crash: no further write, no dismount. */
        std::printf("crash after the read\n");
        Ok = true;
        goto Out;
    }
    Status = NtfsVolumeMarkClean(Volume);
    std::printf("clean dismount: 0x%08x\n", (unsigned)Status);
    Ok = Check(NT_SUCCESS(Status), "MarkClean failed");

Out:
    if (Record)
        NtfsFileRecordDestroy(Record);
    if (Volume)
        NtfsVolumeDestroy(Volume);
    NtfsDiskCloseLinux();
    return Ok;
}

/* What chkdsk leaves behind: the dirty flag cleared on the medium. */
bool
ClearDirty(const char* Path)
{
    PNtfsVolume Volume = NULL;
    ULONG BytesPerSector = 0;
    bool Ok = false;

    if (NT_SUCCESS(NtfsDiskInitializeLinuxWritable(Path, &BytesPerSector)) &&
        NT_SUCCESS(NtfsProbePartitionAndOpenVolume(BytesPerSector, &Volume)))
    {
        Ok = NT_SUCCESS(NtfsVolumeSetFlags(Volume, 0, VOL_DIRTY));
    }
    if (Volume)
        NtfsVolumeDestroy(Volume);
    NtfsDiskCloseLinux();
    return Check(Ok, "could not clear the dirty flag");
}

/* The dirty flag as it is on the medium, read by a fresh mount. */
bool
FlagsOnMedium(const char* Path, USHORT* Flags)
{
    PNtfsVolume Volume = NULL;
    ULONG BytesPerSector = 0;
    bool Ok = false;

    if (NT_SUCCESS(NtfsDiskInitializeLinux(Path, &BytesPerSector)) &&
        NT_SUCCESS(NtfsProbePartitionAndOpenVolume(BytesPerSector, &Volume)))
    {
        Ok = NT_SUCCESS(NtfsVolumeQueryFlags(Volume, Flags));
    }
    if (Volume)
        NtfsVolumeDestroy(Volume);
    NtfsDiskCloseLinux();
    return Check(Ok, "could not read the volume flags");
}

} /* namespace */

int
main()
{
    const char* TempRoot = std::getenv("TMPDIR");
    std::string Template = std::string(TempRoot && *TempRoot ? TempRoot : "/tmp") +
                           "/ntfslib-corrupt-runlist-dirty-XXXXXX";
    std::vector<char> PathBuffer(Template.begin(), Template.end());
    char* Path;
    NTSTATUS ReadStatus = STATUS_SUCCESS;
    USHORT Flags = 0xFFFF;
    int Result = 1;
    int Fd;

    PathBuffer.push_back(0);
    Path = PathBuffer.data();
    Fd = mkstemp(Path);
    if (Fd < 0)
    {
        perror("mkstemp");
        return 1;
    }
    close(Fd);

    if (!FormatImage(Path))
        goto Out;

    /* Control: a good read and a clean dismount leave the volume clean. */
    if (!Session(Path, &ReadStatus) ||
        !Check(NT_SUCCESS(ReadStatus), "control read failed") ||
        !FlagsOnMedium(Path, &Flags))
    {
        goto Out;
    }
    std::printf("control: flags 0x%04x\n", Flags);
    if (!Check(!(Flags & VOL_DIRTY), "control left the volume dirty"))
        goto Out;

    if (!BreakDataRunList(Path, 7))
        goto Out;

    /* A crash right after the corrupt read: no write, no dismount. */
    ReadStatus = STATUS_SUCCESS;
    if (!Session(Path, &ReadStatus, true) ||
        !Check(!NT_SUCCESS(ReadStatus), "read over a corrupt run list succeeded") ||
        !FlagsOnMedium(Path, &Flags))
    {
        goto Out;
    }
    std::printf("after corrupt read and crash: flags 0x%04x\n", Flags);
    if (!Check((Flags & VOL_DIRTY) != 0, "volume clean after a corrupt read and a crash") ||
        !ClearDirty(Path))
    {
        goto Out;
    }

    ReadStatus = STATUS_SUCCESS;
    if (!Session(Path, &ReadStatus) ||
        !Check(!NT_SUCCESS(ReadStatus), "read over a corrupt run list succeeded") ||
        !FlagsOnMedium(Path, &Flags))
    {
        goto Out;
    }
    std::printf("after corrupt read and clean dismount: flags 0x%04x\n", Flags);
    if (!Check((Flags & VOL_DIRTY) != 0, "volume clean after a corrupt run list was read"))
        goto Out;

    std::printf("corrupt_runlist_dirty: passed\n");
    Result = 0;

Out:
    unlink(Path);
    return Result;
}
