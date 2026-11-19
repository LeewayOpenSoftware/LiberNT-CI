/*
 * PROJECT:     LiberNT NTFS library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USN change journal ($Extend\$UsnJrnl): create, query, append, read
 */

#include "ntfslib_new.h"
#include "ntfslib_new_internal.h"

/*
 * On-disk layout (shared with Windows): $Extend\$UsnJrnl holds the named data streams
 * $J (sparse; a record's USN is its byte offset) and $Max (32-byte header). Records are
 * 8-byte aligned USN_RECORD_V2 and never straddle a USN_PAGE_SIZE page; the remainder of
 * a page is left zero. The caller serializes all calls for one volume.
 */

#define NTFS_USN_PAGE_SIZE          0x1000
#define NTFS_USN_RECORD_HEADER      FIELD_OFFSET(UsnJrnlEntry, FileName)
#define NTFS_USN_MAX_USN            0x7FFFFFFFFFFF0000LL
#define NTFS_USN_ALIGN(x)           (((x) + 7) & ~7ULL)

static WCHAR UsnJournalPath[] = L"\\$Extend\\$UsnJrnl";
static WCHAR UsnJournalDataStream[] = L"$J";
static WCHAR UsnJournalMaxStream[] = L"$Max";

static
NTSTATUS
UsnOpenJournal(
    _In_ PNtfsVolume Volume,
    _Out_ PNtfsFileRecord* Journal)
{
    NTSTATUS Status;

    *Journal = NULL;
    if (!Volume)
        return STATUS_INVALID_PARAMETER;
    Status = NtfsMasterFileTableGetFileRecordFromQuery(NtfsVolumeGetMft(Volume), UsnJournalPath, Journal);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND || Status == STATUS_OBJECT_PATH_NOT_FOUND ||
        Status == STATUS_NOT_FOUND)
    {
        return STATUS_JOURNAL_NOT_ACTIVE;
    }
    return Status;
}

static
NTSTATUS
UsnReadMax(
    _In_ PNtfsFileRecord Journal,
    _Out_ UsnJrnlMaxData* Max)
{
    ULONG Length = sizeof(*Max);
    NTSTATUS Status;

    Status = NtfsFileRecordCopyData(Journal, TypeData, UsnJournalMaxStream, reinterpret_cast<PUCHAR>(Max), &Length, 0);
    if (!NT_SUCCESS(Status))
        return (Status == STATUS_NOT_FOUND || Status == STATUS_OBJECT_NAME_NOT_FOUND) ? STATUS_JOURNAL_NOT_ACTIVE : Status;
    /* Length comes back as the number of bytes that could not be copied */
    return (Length == 0) ? STATUS_SUCCESS : STATUS_FILE_CORRUPT_ERROR;
}

static
NTSTATUS
UsnWriteMax(
    _In_ PNtfsFileRecord Journal,
    _In_ const UsnJrnlMaxData* Max)
{
    UsnJrnlMaxData Copy = *Max;
    ULONG Length = sizeof(Copy);
    LARGE_INTEGER Offset;

    Offset.QuadPart = 0;
    return NtfsFileRecordWriteFileData(Journal, TypeData, UsnJournalMaxStream, reinterpret_cast<PUCHAR>(&Copy), &Length, &Offset);
}

/* Size of the $J stream. A journal file has the unnamed stream, $J and $Max. */
#define NTFS_USN_MAX_STREAMS 8

static
NTSTATUS
UsnGetDataSize(
    _In_ PNtfsFileRecord Journal,
    _Out_ ULONGLONG* Size)
{
    NtfsDataStreamInformation* Streams;
    ULONG Count = NTFS_USN_MAX_STREAMS, Index;
    NTSTATUS Status;

    *Size = 0;
    Streams = new(PagedPool, TAG_FILE_RECORD) NtfsDataStreamInformation[NTFS_USN_MAX_STREAMS];
    if (!Streams)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = NtfsFileRecordQueryDataStreams(Journal, Streams, &Count);
    if (Status == STATUS_BUFFER_OVERFLOW || Status == STATUS_BUFFER_TOO_SMALL)
        Status = STATUS_FILE_CORRUPT_ERROR;
    if (NT_SUCCESS(Status))
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        for (Index = 0; Index < Count; Index++)
        {
            if (Streams[Index].NameLength == 2 && Streams[Index].Name[0] == L'$' && Streams[Index].Name[1] == L'J')
            {
                *Size = Streams[Index].DataSize;
                Status = STATUS_SUCCESS;
                break;
            }
        }
    }
    delete[] Streams;
    return Status;
}

/* A record header that a reader may walk past: version 2, aligned, inside its page */
static
BOOLEAN
UsnRecordLooksValid(
    _In_ const UsnJrnlEntry* Record,
    _In_ ULONGLONG Usn)
{
    ULONG PageLeft = NTFS_USN_PAGE_SIZE - (ULONG)(Usn % NTFS_USN_PAGE_SIZE);

    return Record->EntrySize >= NTFS_USN_RECORD_HEADER &&
           (Record->EntrySize & 7) == 0 &&
           Record->EntrySize <= PageLeft &&
           Record->MajorVersion == 2 &&
           Record->EntryOffset == Usn &&
           Record->FileNameOffset == NTFS_USN_RECORD_HEADER &&
           (ULONG)Record->FileNameOffset + Record->FileNameSize <= Record->EntrySize;
}

/*
 * The next USN is the end of the last complete record. A crash can leave a torn record at
 * the tail (appends are not logged), so walk the last page and stop at the first bad one.
 */
static
NTSTATUS
UsnComputeNextUsn(
    _In_ PNtfsFileRecord Journal,
    _In_ ULONGLONG DataSize,
    _In_ ULONGLONG Floor,
    _Out_ ULONGLONG* NextUsn)
{
    PUCHAR Page;
    ULONGLONG PageStart, Cursor;
    ULONG Requested, Length;
    NTSTATUS Status;

    *NextUsn = DataSize;
    if (DataSize == 0 || DataSize <= Floor)
        return STATUS_SUCCESS;
    Page = new(PagedPool, TAG_FILE_RECORD) UCHAR[NTFS_USN_PAGE_SIZE];
    if (!Page)
        return STATUS_INSUFFICIENT_RESOURCES;

    PageStart = (DataSize - 1) & ~(ULONGLONG)(NTFS_USN_PAGE_SIZE - 1);
    if (PageStart < Floor)
        PageStart = Floor & ~(ULONGLONG)(NTFS_USN_PAGE_SIZE - 1);
    Requested = (ULONG)(DataSize - PageStart);
    Length = Requested;
    Status = NtfsFileRecordCopyData(Journal, TypeData, UsnJournalDataStream, Page, &Length, PageStart);
    if (!NT_SUCCESS(Status))
    {
        delete[] Page;
        return Status;
    }
    Length = Requested - Length;

    Cursor = PageStart;
    while (Cursor + NTFS_USN_RECORD_HEADER <= PageStart + Length)
    {
        const UsnJrnlEntry* Record = reinterpret_cast<const UsnJrnlEntry*>(Page + (Cursor - PageStart));

        if (Record->EntrySize == 0 || !UsnRecordLooksValid(Record, Cursor))
            break;
        Cursor += Record->EntrySize;
    }
    *NextUsn = (Cursor < Floor) ? Floor : Cursor;
    delete[] Page;
    return STATUS_SUCCESS;
}

extern "C" {

NTSTATUS
NtfsUsnJournalCreate(
    _In_ PNtfsVolume Volume,
    _In_ ULONGLONG MaximumSize,
    _In_ ULONGLONG AllocationDelta,
    _In_ ULONGLONG NewJournalId)
{
    PNtfsFileRecord Journal = NULL;
    UsnJrnlMaxData Max;
    NTSTATUS Status;

    if (!Volume || NewJournalId == 0)
        return STATUS_INVALID_PARAMETER;

    Status = UsnOpenJournal(Volume, &Journal);
    if (NT_SUCCESS(Status))
    {
        /* An existing journal keeps its identity; only the size targets change */
        Status = UsnReadMax(Journal, &Max);
        if (NT_SUCCESS(Status))
        {
            Max.MaxSize = MaximumSize;
            Max.AllocationDelta = AllocationDelta;
            Status = UsnWriteMax(Journal, &Max);
        }
        NtfsFileRecordDestroy(Journal);
        return Status;
    }
    if (Status != STATUS_JOURNAL_NOT_ACTIVE)
        return Status;

    Status = NtfsMasterFileTableCreateFile(NtfsVolumeGetMft(Volume),
                                           UsnJournalPath,
                                           (ULONG)(sizeof(UsnJournalPath) / sizeof(WCHAR) - 1),
                                           FALSE,
                                           FILE_PERM_HIDDEN | FILE_PERM_SYSTEM,
                                           &Journal);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsFileRecordCreateNamedDataStream(Journal, UsnJournalDataStream);
    if (NT_SUCCESS(Status))
        Status = NtfsFileRecordSetSparse(Journal, TypeData, UsnJournalDataStream, TRUE);
    if (NT_SUCCESS(Status))
        Status = NtfsFileRecordCreateNamedDataStream(Journal, UsnJournalMaxStream);
    if (NT_SUCCESS(Status))
    {
        Max.MaxSize = MaximumSize;
        Max.AllocationDelta = AllocationDelta;
        Max.USN_ID = NewJournalId;
        Max.LowestValidUSN = 0;
        Status = UsnWriteMax(Journal, &Max);
    }

    NtfsFileRecordDestroy(Journal);
    return Status;
}

NTSTATUS
NtfsUsnJournalQuery(
    _In_ PNtfsVolume Volume,
    _Out_ PNtfsUsnJournalData Data)
{
    PNtfsFileRecord Journal;
    UsnJrnlMaxData Max;
    ULONGLONG DataSize, NextUsn;
    NTSTATUS Status;

    if (!Data)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Data, sizeof(*Data));

    Status = UsnOpenJournal(Volume, &Journal);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = UsnReadMax(Journal, &Max);
    if (NT_SUCCESS(Status))
        Status = UsnGetDataSize(Journal, &DataSize);
    if (NT_SUCCESS(Status))
        Status = UsnComputeNextUsn(Journal, DataSize, Max.LowestValidUSN, &NextUsn);
    if (NT_SUCCESS(Status))
    {
        Data->UsnJournalId = Max.USN_ID;
        Data->LowestValidUsn = (LONGLONG)Max.LowestValidUSN;
        Data->FirstUsn = (LONGLONG)Max.LowestValidUSN;
        Data->NextUsn = (LONGLONG)NextUsn;
        Data->MaxUsn = NTFS_USN_MAX_USN;
        Data->MaximumSize = Max.MaxSize;
        Data->AllocationDelta = Max.AllocationDelta;
    }
    NtfsFileRecordDestroy(Journal);
    return Status;
}

NTSTATUS
NtfsUsnJournalAppend(
    _In_ PNtfsVolume Volume,
    _In_ const NtfsUsnRecordInput* Input,
    _Out_ LONGLONG* Usn)
{
    PUCHAR Buffer = NULL;
    UsnJrnlEntry* Record;
    PNtfsFileRecord Journal;
    UsnJrnlMaxData Max;
    ULONGLONG DataSize, NextUsn;
    ULONG RecordLength, PageLeft, PadLength, Length;
    LARGE_INTEGER Offset;
    NTSTATUS Status;

    if (!Input || !Usn || (Input->FileNameLength & 1) ||
        Input->FileNameLength > NTFS_MAX_FILE_NAME_LENGTH * sizeof(WCHAR) ||
        (Input->FileNameLength && !Input->FileName))
    {
        return STATUS_INVALID_PARAMETER;
    }
    *Usn = 0;

    Status = UsnOpenJournal(Volume, &Journal);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = UsnReadMax(Journal, &Max);
    if (NT_SUCCESS(Status))
        Status = UsnGetDataSize(Journal, &DataSize);
    if (NT_SUCCESS(Status))
        Status = UsnComputeNextUsn(Journal, DataSize, Max.LowestValidUSN, &NextUsn);
    if (!NT_SUCCESS(Status))
        goto Done;

    /* Drop a torn tail before writing past it */
    if (NextUsn < DataSize)
    {
        Status = NtfsFileRecordSetFileDataSize(Journal, TypeData, UsnJournalDataStream, NextUsn);
        if (!NT_SUCCESS(Status))
            goto Done;
    }

    RecordLength = (ULONG)NTFS_USN_ALIGN(NTFS_USN_RECORD_HEADER + Input->FileNameLength);
    NextUsn = NTFS_USN_ALIGN(NextUsn);
    /* A record that does not fit in this page starts the next one; the rest of the page is written as zeros */
    PageLeft = NTFS_USN_PAGE_SIZE - (ULONG)(NextUsn % NTFS_USN_PAGE_SIZE);
    PadLength = (RecordLength > PageLeft) ? PageLeft : 0;
    if ((LONGLONG)(NextUsn + PadLength) + RecordLength > NTFS_USN_MAX_USN)
    {
        Status = STATUS_DISK_FULL;
        goto Done;
    }

    Buffer = new(PagedPool, TAG_FILE_RECORD) UCHAR[PadLength + RecordLength];
    if (!Buffer)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    RtlZeroMemory(Buffer, PadLength + RecordLength);
    Record = reinterpret_cast<UsnJrnlEntry*>(Buffer + PadLength);
    Record->EntrySize = RecordLength;
    Record->MajorVersion = 2;
    Record->MinorVersion = 0;
    Record->MFTReference = Input->FileReferenceNumber;
    Record->ParentMFTReference = Input->ParentFileReferenceNumber;
    Record->EntryOffset = NextUsn + PadLength;
    Record->Timestamp = Input->TimeStamp;
    Record->Reason = Input->Reason;
    Record->SourceInfo = Input->SourceInfo;
    Record->SecurityID = Input->SecurityId;
    Record->FileAttributes = Input->FileAttributes;
    Record->FileNameSize = Input->FileNameLength;
    Record->FileNameOffset = (UINT16)NTFS_USN_RECORD_HEADER;
    if (Input->FileNameLength)
        RtlCopyMemory(Buffer + PadLength + NTFS_USN_RECORD_HEADER, Input->FileName, Input->FileNameLength);

    Length = PadLength + RecordLength;
    Offset.QuadPart = (LONGLONG)NextUsn;
    Status = NtfsFileRecordWriteFileData(Journal, TypeData, UsnJournalDataStream, Buffer, &Length, &Offset);
    if (NT_SUCCESS(Status) && Length != PadLength + RecordLength)
        Status = STATUS_DISK_FULL;
    if (NT_SUCCESS(Status))
        *Usn = (LONGLONG)(NextUsn + PadLength);

Done:
    delete[] Buffer;
    NtfsFileRecordDestroy(Journal);
    return Status;
}

NTSTATUS
NtfsUsnJournalRead(
    _In_ PNtfsVolume Volume,
    _In_ LONGLONG StartUsn,
    _Out_writes_bytes_(BufferLength) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG BytesReturned,
    _Out_ LONGLONG* NextUsn)
{
    PUCHAR Page = NULL;
    PNtfsFileRecord Journal;
    UsnJrnlMaxData Max;
    ULONGLONG DataSize, End, Cursor;
    ULONG Copied = 0;
    NTSTATUS Status;

    if (!Buffer || !BytesReturned || !NextUsn || StartUsn < 0)
        return STATUS_INVALID_PARAMETER;
    *BytesReturned = 0;
    *NextUsn = StartUsn;

    Status = UsnOpenJournal(Volume, &Journal);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = UsnReadMax(Journal, &Max);
    if (NT_SUCCESS(Status))
        Status = UsnGetDataSize(Journal, &DataSize);
    if (NT_SUCCESS(Status))
        Status = UsnComputeNextUsn(Journal, DataSize, Max.LowestValidUSN, &End);
    if (!NT_SUCCESS(Status))
        goto Done;

    /* 0 means the first record; a nonzero USN below the valid range was trimmed away */
    if (StartUsn == 0)
        StartUsn = (LONGLONG)Max.LowestValidUSN;
    else if ((ULONGLONG)StartUsn < Max.LowestValidUSN)
    {
        Status = STATUS_JOURNAL_ENTRY_DELETED;
        goto Done;
    }

    Page = new(PagedPool, TAG_FILE_RECORD) UCHAR[NTFS_USN_PAGE_SIZE];
    if (!Page)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    Cursor = (ULONGLONG)StartUsn;
    while (Cursor < End)
    {
        ULONGLONG PageStart = Cursor & ~(ULONGLONG)(NTFS_USN_PAGE_SIZE - 1);
        ULONG Requested = (ULONG)(((End - PageStart) < NTFS_USN_PAGE_SIZE) ? (End - PageStart) : NTFS_USN_PAGE_SIZE);
        ULONG Length = Requested;
        BOOLEAN Full = FALSE;

        Status = NtfsFileRecordCopyData(Journal, TypeData, UsnJournalDataStream, Page, &Length, PageStart);
        if (!NT_SUCCESS(Status))
            goto Done;
        Length = Requested - Length;

        while (Cursor < PageStart + Length)
        {
            const UsnJrnlEntry* Record = reinterpret_cast<const UsnJrnlEntry*>(Page + (Cursor - PageStart));

            if (Cursor + NTFS_USN_RECORD_HEADER > PageStart + Length || Record->EntrySize == 0)
            {
                /* Zero fill to the end of the page */
                Cursor = PageStart + NTFS_USN_PAGE_SIZE;
                break;
            }
            if (!UsnRecordLooksValid(Record, Cursor))
            {
                Status = STATUS_FILE_CORRUPT_ERROR;
                goto Done;
            }
            if (Copied + Record->EntrySize > BufferLength)
            {
                Full = TRUE;
                break;
            }
            RtlCopyMemory(Buffer + Copied, Record, Record->EntrySize);
            Copied += Record->EntrySize;
            Cursor += Record->EntrySize;
        }
        if (Full)
            break;
    }

    if (Cursor > End)
        Cursor = End;
    *BytesReturned = Copied;
    *NextUsn = (LONGLONG)Cursor;
    if (Copied == 0 && Cursor < End)
        Status = STATUS_BUFFER_TOO_SMALL;

Done:
    delete[] Page;
    NtfsFileRecordDestroy(Journal);
    return Status;
}

} /* extern "C" */
