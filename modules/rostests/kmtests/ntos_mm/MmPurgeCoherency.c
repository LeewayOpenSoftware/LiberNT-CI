/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Cache coherency after sub-page and partial-page non-cached writes
 *              (A-2 MiSegmentPurge rounding and discard mode, A-1 Cc flush-and-purge,
 *              A-3 ntfs_new non-cached write purge)
 */

#include <kmt_test.h>
#include <ntstrsafe.h>

/*
 * Owner scenario: a 512-byte non-cached write followed by a cached read must
 * return the fresh bytes. Every page that the write touches has to leave the
 * cache (A-2: the purge used to round inward and keep the partial first and
 * last pages), the file system has to flush dirty neighbours before it purges
 * (A-1/A-3: otherwise cached data outside the written range is lost), and a
 * truncation has to drop dirty pages beyond the new end of file even though
 * they are dirty (A-2 discard mode), so that a later extension reads zeros.
 *
 * Runs on the system volume (NTFS) and, when present, on the FAT32 payload
 * volume D: that carries kmtest. Everything is sector aligned (non-cached I/O
 * demands it) but deliberately not page aligned.
 */

#define PC_PAGES        3
#define PC_SIZE         (PC_PAGES * PAGE_SIZE)
#define PC_SECTOR       512
#define PC_BASE         0x11
#define PC_TRUNC        0x33
#define PC_TAG          'CPmK'

static PUCHAR IoBuffer;     /* non-paged, page aligned: the non-cached buffer */
static PUCHAR CheckBuffer;

static
NTSTATUS
OpenScratch(
    _In_ PCWSTR Path,
    _In_ ULONG Disposition,
    _In_ ULONG ExtraOptions,
    _Out_ PHANDLE Handle)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateFile(Handle,
                        FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
                        FILE_WRITE_ATTRIBUTES | DELETE | SYNCHRONIZE,
                        &Attributes, &IoStatus, NULL, FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        Disposition,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | ExtraOptions,
                        NULL, 0);
}

static
NTSTATUS
WriteAt(
    _In_ HANDLE Handle,
    _In_ ULONG64 Offset,
    _In_ PVOID Buffer,
    _In_ ULONG Length)
{
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER ByteOffset;
    NTSTATUS Status;

    ByteOffset.QuadPart = Offset;
    Status = ZwWriteFile(Handle, NULL, NULL, NULL, &IoStatus, Buffer, Length, &ByteOffset, NULL);
    if (NT_SUCCESS(Status) && IoStatus.Information != Length)
        Status = STATUS_PARTIAL_COPY;
    return Status;
}

static
NTSTATUS
ReadAt(
    _In_ HANDLE Handle,
    _In_ ULONG64 Offset,
    _Out_ PVOID Buffer,
    _In_ ULONG Length)
{
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER ByteOffset;
    NTSTATUS Status;

    ByteOffset.QuadPart = Offset;
    RtlFillMemory(Buffer, Length, 0xEE);
    Status = ZwReadFile(Handle, NULL, NULL, NULL, &IoStatus, Buffer, Length, &ByteOffset, NULL);
    if (NT_SUCCESS(Status) && IoStatus.Information != Length)
        Status = STATUS_PARTIAL_COPY;
    return Status;
}

static
NTSTATUS
SetEndOfFile(
    _In_ HANDLE Handle,
    _In_ ULONG64 Size)
{
    IO_STATUS_BLOCK IoStatus;
    FILE_END_OF_FILE_INFORMATION Info;

    Info.EndOfFile.QuadPart = Size;
    return ZwSetInformationFile(Handle, &IoStatus, &Info, sizeof(Info), FileEndOfFileInformation);
}

/* First index in [Start, End) that is not Value, or -1 */
static
LONG
FirstMismatch(
    _In_ PUCHAR Buffer,
    _In_ ULONG Start,
    _In_ ULONG End,
    _In_ UCHAR Value)
{
    ULONG i;

    for (i = Start; i < End; i++)
    {
        if (Buffer[i] != Value)
            return (LONG)i;
    }
    return -1;
}

/* The whole file must read as Pattern inside [Offset, Offset + Length) and as
 * Base everywhere else; reports the first offending byte. */
static
VOID
CheckImage(
    _In_ PCSTR What,
    _In_ PUCHAR Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ UCHAR Pattern,
    _In_ UCHAR Base)
{
    LONG Bad;

    Bad = FirstMismatch(Buffer, Offset, Offset + Length, Pattern);
    ok(Bad < 0, "%s [%lu,%lu): byte %ld is 0x%02x, expected pattern 0x%02x\n",
       What, Offset, Offset + Length, Bad, Bad < 0 ? 0 : Buffer[Bad], Pattern);
    Bad = FirstMismatch(Buffer, 0, Offset, Base);
    ok(Bad < 0, "%s before [%lu,%lu): byte %ld is 0x%02x, expected 0x%02x (neighbour lost)\n",
       What, Offset, Offset + Length, Bad, Bad < 0 ? 0 : Buffer[Bad], Base);
    Bad = FirstMismatch(Buffer, Offset + Length, PC_SIZE, Base);
    ok(Bad < 0, "%s after [%lu,%lu): byte %ld is 0x%02x, expected 0x%02x (neighbour lost)\n",
       What, Offset, Offset + Length, Bad, Bad < 0 ? 0 : Buffer[Bad], Base);
}

/* Warm the cache with Base over the whole file (dirty, or flushed clean), write
 * Pattern non-cached over [Offset, Offset + Length), then read everything back
 * through the cache and non-cached. */
static
VOID
TestRange(
    _In_ HANDLE Cached,
    _In_ HANDLE NonCached,
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ UCHAR Pattern,
    _In_ BOOLEAN Dirty)
{
    NTSTATUS Status;
    CHAR What[64];

    RtlStringCbPrintfA(What, sizeof(What), "%s cached read", Dirty ? "dirty" : "clean");

    RtlFillMemory(CheckBuffer, PC_SIZE, PC_BASE);
    Status = WriteAt(Cached, 0, CheckBuffer, PC_SIZE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!Dirty)
    {
        IO_STATUS_BLOCK IoStatus;
        Status = ZwFlushBuffersFile(Cached, &IoStatus);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    /* Make sure every page is resident in the cache before the non-cached write */
    Status = ReadAt(Cached, 0, CheckBuffer, PC_SIZE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(FirstMismatch(CheckBuffer, 0, PC_SIZE, PC_BASE) < 0, "warm-up read is not uniform\n");

    RtlFillMemory(IoBuffer, Length, Pattern);
    Status = WriteAt(NonCached, Offset, IoBuffer, Length);
    ok(Status == STATUS_SUCCESS, "non-cached write [%lu,%lu) = 0x%lx\n", Offset, Offset + Length, Status);
    if (!NT_SUCCESS(Status))
        return;

    /* Cached view: fresh bytes in the range (purge covered every page it touches),
     * old bytes around it (nothing outside the range was discarded) */
    Status = ReadAt(Cached, 0, CheckBuffer, PC_SIZE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    CheckImage(What, CheckBuffer, Offset, Length, Pattern, PC_BASE);

    /* On-disk view: the file system flushed the dirty neighbours before it
     * purged, so the medium holds Base around the fresh range */
    RtlStringCbPrintfA(What, sizeof(What), "%s non-cached read", Dirty ? "dirty" : "clean");
    Status = ReadAt(NonCached, 0, IoBuffer, PC_SIZE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    CheckImage(What, IoBuffer, Offset, Length, Pattern, PC_BASE);
}

/* A-2 discard mode: dirty pages wholly beyond a new end of file must be dropped
 * by the truncation; extending the file again must then read zeros, not the
 * stale dirty bytes, while the bytes before the cut survive. */
static
VOID
TestTruncateDiscard(
    _In_ HANDLE Cached,
    _In_ HANDLE NonCached)
{
    const ULONG Cut = PAGE_SIZE + PC_SECTOR;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    FILE_STANDARD_INFORMATION Standard;
    LONG Bad;

    RtlFillMemory(CheckBuffer, PC_SIZE, PC_TRUNC);
    Status = WriteAt(Cached, 0, CheckBuffer, PC_SIZE);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = SetEndOfFile(Cached, Cut);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = SetEndOfFile(Cached, PC_SIZE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ZwQueryInformationFile(Cached, &IoStatus, &Standard, sizeof(Standard), FileStandardInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(Standard.EndOfFile.QuadPart, (LONGLONG)PC_SIZE);

    Status = ReadAt(Cached, 0, CheckBuffer, PC_SIZE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Bad = FirstMismatch(CheckBuffer, 0, Cut, PC_TRUNC);
    ok(Bad < 0, "truncate: cached byte %ld before the cut is 0x%02x, expected 0x%02x\n",
       Bad, Bad < 0 ? 0 : CheckBuffer[Bad], PC_TRUNC);
    Bad = FirstMismatch(CheckBuffer, Cut, PC_SIZE, 0);
    ok(Bad < 0, "truncate: cached byte %ld beyond the old cut is 0x%02x, expected 0 (stale dirty page survived)\n",
       Bad, Bad < 0 ? 0 : CheckBuffer[Bad]);

    Status = ZwFlushBuffersFile(Cached, &IoStatus);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ReadAt(NonCached, 0, IoBuffer, PC_SIZE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Bad = FirstMismatch(IoBuffer, 0, Cut, PC_TRUNC);
    ok(Bad < 0, "truncate: on-disk byte %ld before the cut is 0x%02x, expected 0x%02x\n",
       Bad, Bad < 0 ? 0 : IoBuffer[Bad], PC_TRUNC);
    Bad = FirstMismatch(IoBuffer, Cut, PC_SIZE, 0);
    ok(Bad < 0, "truncate: on-disk byte %ld beyond the old cut is 0x%02x, expected 0\n",
       Bad, Bad < 0 ? 0 : IoBuffer[Bad]);
}

static
VOID
TestVolume(
    _In_ PCWSTR Directory,
    _In_ BOOLEAN Optional)
{
    WCHAR Path[160];
    HANDLE Cached = NULL, NonCached = NULL;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    UCHAR AttrBuffer[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 32 * sizeof(WCHAR)];
    PFILE_FS_ATTRIBUTE_INFORMATION Attr = (PFILE_FS_ATTRIBUTE_INFORMATION)AttrBuffer;
    FILE_FS_SIZE_INFORMATION FsSize;

    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%skmt-purge-%p.bin", Directory, PsGetCurrentThread());

    Status = OpenScratch(Path, FILE_OVERWRITE_IF, FILE_DELETE_ON_CLOSE, &Cached);
    if (Optional && !NT_SUCCESS(Status))
    {
        trace("Volume %S not available (0x%lx), skipping\n", Directory, Status);
        return;
    }
    if (skip(NT_SUCCESS(Status), "Cannot create %S: 0x%lx\n", Path, Status))
        return;

    RtlZeroMemory(AttrBuffer, sizeof(AttrBuffer));
    Status = ZwQueryVolumeInformationFile(Cached, &IoStatus, Attr, sizeof(AttrBuffer), FileFsAttributeInformation);
    if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
        trace("Volume %S: file system %.*S\n", Directory,
              (int)(Attr->FileSystemNameLength / sizeof(WCHAR)), Attr->FileSystemName);
    Status = ZwQueryVolumeInformationFile(Cached, &IoStatus, &FsSize, sizeof(FsSize), FileFsSizeInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        trace("Volume %S: %lu bytes per sector\n", Directory, FsSize.BytesPerSector);
    if (skip(!NT_SUCCESS(Status) || FsSize.BytesPerSector <= PC_SECTOR,
             "Sector size %lu > %u, sub-page cases need 512-byte sectors\n",
             FsSize.BytesPerSector, PC_SECTOR))
    {
        ZwClose(Cached);
        return;
    }

    Status = OpenScratch(Path, FILE_OPEN, FILE_NO_INTERMEDIATE_BUFFERING, &NonCached);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(Cached);
        return;
    }

    /* Sector aligned, page unaligned ranges: each one is a purge-rounding case */
    TestRange(Cached, NonCached, 0,                           PC_SECTOR,             0xA2, FALSE); /* sub-page at the start */
    TestRange(Cached, NonCached, PAGE_SIZE + PC_SECTOR,       PC_SECTOR,             0xB3, FALSE); /* sub-page inside page 1 */
    TestRange(Cached, NonCached, PAGE_SIZE - PC_SECTOR,       2 * PC_SECTOR,         0xC4, FALSE); /* crosses the page 0/1 boundary */
    TestRange(Cached, NonCached, 0,                           PAGE_SIZE + PC_SECTOR, 0xD5, FALSE); /* full page plus a partial last page */
    TestRange(Cached, NonCached, 2 * PAGE_SIZE + PC_SECTOR,   PAGE_SIZE - PC_SECTOR, 0xE6, FALSE); /* partial first page up to EOF */
    /* Same ranges with the cache dirty: the neighbours must be flushed, not discarded */
    TestRange(Cached, NonCached, 0,                           PC_SECTOR,             0x1A, TRUE);
    TestRange(Cached, NonCached, PAGE_SIZE + PC_SECTOR,       PC_SECTOR,             0x2B, TRUE);
    TestRange(Cached, NonCached, PAGE_SIZE - PC_SECTOR,       2 * PC_SECTOR,         0x3C, TRUE);
    TestRange(Cached, NonCached, 0,                           PAGE_SIZE + PC_SECTOR, 0x4D, TRUE);
    TestRange(Cached, NonCached, 2 * PAGE_SIZE + PC_SECTOR,   PAGE_SIZE - PC_SECTOR, 0x5E, TRUE);

    TestTruncateDiscard(Cached, NonCached);

    ZwClose(NonCached);
    ZwClose(Cached);
}

START_TEST(MmPurgeCoherency)
{
    IoBuffer = ExAllocatePoolWithTag(NonPagedPool, PC_SIZE, PC_TAG);
    CheckBuffer = ExAllocatePoolWithTag(NonPagedPool, PC_SIZE, PC_TAG);
    if (skip(IoBuffer != NULL && CheckBuffer != NULL, "Cannot allocate the test buffers\n"))
    {
        if (IoBuffer) ExFreePoolWithTag(IoBuffer, PC_TAG);
        if (CheckBuffer) ExFreePoolWithTag(CheckBuffer, PC_TAG);
        return;
    }

    TestVolume(L"\\SystemRoot\\Temp\\", FALSE);
    TestVolume(L"\\??\\D:\\", TRUE);

    ExFreePoolWithTag(IoBuffer, PC_TAG);
    ExFreePoolWithTag(CheckBuffer, PC_TAG);
}
