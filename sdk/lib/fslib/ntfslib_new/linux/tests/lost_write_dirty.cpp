/*
 * PROJECT:     LiberNT NTFS library tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A metadata write that fails leaves the volume dirty after a clean dismount
 * COPYRIGHT:   Copyright 2026 Leeway Open Software <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include <ntfslib_new.h>
#include "ntfsformat.h"
#include "ntfs_linux.h"

#define HOST_STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define TEST_FLAG 0x0020

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
        std::fprintf(stderr, "lost_write_dirty: %s\n", Message);
    return Condition;
}

bool
FormatImage(const char* Path)
{
    const ULONG BytesPerSector = 512;
    const ULONGLONG SizeBytes = 32ULL * 1024 * 1024;
    HostContext Host;
    NtfsFormatParameters Parameters;
    WCHAR Label[] = { 'L', 'O', 'S', 'T', 0 };
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

/* Points the descriptor the library writes through at the image opened
 * with Flags: O_RDONLY makes every write fail, O_RDWR makes writes work. */
bool
ReopenImage(const char* Path, int Descriptor, int Flags)
{
    int Other = open(Path, Flags | O_BINARY);
    bool Ok;

    if (Other < 0)
    {
        perror("open");
        return false;
    }
    Ok = dup2(Other, Descriptor) == Descriptor;
    close(Other);
    return Check(Ok, "dup2 failed");
}

bool
IsImageDescriptor(const char* Path, int Descriptor)
{
    char Link[64], Target[PATH_MAX + 1], Expected[PATH_MAX];
    ssize_t Length;

    std::snprintf(Link, sizeof(Link), "/proc/self/fd/%d", Descriptor);
    Length = readlink(Link, Target, PATH_MAX);
    if (Length <= 0 || !realpath(Path, Expected))
        return false;
    Target[Length] = 0;
    return std::strcmp(Target, Expected) == 0;
}

bool
Session(const char* Path, bool FailWrite)
{
    PNtfsVolume Volume = NULL;
    ULONG BytesPerSector = 0;
    USHORT Flags = 0xFFFF;
    NTSTATUS Status;
    int Descriptor;
    bool Ok = false;

    Descriptor = open("/dev/null", O_RDONLY);
    if (!Check(Descriptor >= 0, "/dev/null not opened"))
        return false;
    close(Descriptor);

    Status = NtfsDiskInitializeLinuxWritable(Path, &BytesPerSector);
    if (!Check(NT_SUCCESS(Status), "NtfsDiskInitializeLinuxWritable failed") ||
        !Check(IsImageDescriptor(Path, Descriptor), "the image is not on the expected descriptor"))
    {
        goto Out;
    }
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

    if (FailWrite && !ReopenImage(Path, Descriptor, O_RDONLY))
        goto Out;
    Status = NtfsVolumeSetFlags(Volume, (Flags & TEST_FLAG) ? 0 : TEST_FLAG, (Flags & TEST_FLAG) ? TEST_FLAG : 0);
    std::printf("metadata write%s: 0x%08x\n", FailWrite ? " with writes failing" : "", (unsigned)Status);
    if (!Check(FailWrite ? !NT_SUCCESS(Status) : NT_SUCCESS(Status), "unexpected metadata write status"))
        goto Out;
    if (FailWrite && !ReopenImage(Path, Descriptor, O_RDWR))
        goto Out;

    Status = NtfsVolumeCommitCorruptionDirty(Volume);
    if (!Check(NT_SUCCESS(Status), "CommitCorruptionDirty failed"))
        goto Out;
    Status = NtfsVolumeMarkClean(Volume);
    std::printf("clean dismount: 0x%08x\n", (unsigned)Status);
    Ok = Check(NT_SUCCESS(Status), "MarkClean failed");

Out:
    if (Volume)
        NtfsVolumeDestroy(Volume);
    NtfsDiskCloseLinux();
    return Ok;
}

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
                           "/ntfslib-lost-write-dirty-XXXXXX";
    std::vector<char> PathBuffer(Template.begin(), Template.end());
    char* Path;
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

    if (!Session(Path, false) || !FlagsOnMedium(Path, &Flags))
        goto Out;
    std::printf("control: flags 0x%04x\n", Flags);
    if (!Check(!(Flags & VOL_DIRTY), "control left the volume dirty"))
        goto Out;

    if (!Session(Path, true) || !FlagsOnMedium(Path, &Flags))
        goto Out;
    std::printf("after a failed metadata write and a clean dismount: flags 0x%04x\n", Flags);
    if (!Check((Flags & VOL_DIRTY) != 0, "volume clean after a metadata write was lost"))
        goto Out;

    std::printf("lost_write_dirty: passed\n");
    Result = 0;

Out:
    unlink(Path);
    return Result;
}
