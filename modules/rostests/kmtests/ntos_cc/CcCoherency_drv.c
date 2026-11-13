/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test driver for Cc purge coherency: CcPurgeCacheSection on
 *              partial pages (T-A4), CcCoherencyFlushAndPurgeCache (T-A3) and
 *              a shrink while the view holding the new end is pinned
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * A small file system whose paging I/O reads and writes a non-paged backing
 * buffer, the "disk". Changing the buffer directly stands for another writer
 * updating the disk behind the cache. The rule checked everywhere: when a
 * purge (or a flush-and-purge) reports success, the next cached read of every
 * page it touched returns the bytes on the disk, never the old cached ones.
 * Reporting failure is always allowed.
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define TEST_PAGES 3
#define TEST_SIZE (TEST_PAGES * PAGE_SIZE)

typedef struct _TEST_FCB
{
    FSRTL_ADVANCED_FCB_HEADER Header;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    FAST_MUTEX HeaderMutex;
} TEST_FCB, *PTEST_FCB;

static KMT_IRP_HANDLER TestIrpHandler;
static FAST_IO_DISPATCH TestFastIoDispatch;
static PUCHAR Disk;
static PUCHAR Scratch;

static
BOOLEAN
NTAPI
FastIoRead(
    _In_ PFILE_OBJECT FileObject,
    _In_ PLARGE_INTEGER FileOffset,
    _In_ ULONG Length,
    _In_ BOOLEAN Wait,
    _In_ ULONG LockKey,
    _Out_ PVOID Buffer,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    IoStatus->Status = STATUS_NOT_SUPPORTED;
    return FALSE;
}

static
BOOLEAN
NTAPI
FastIoWrite(
    _In_ PFILE_OBJECT FileObject,
    _In_ PLARGE_INTEGER FileOffset,
    _In_ ULONG Length,
    _In_ BOOLEAN Wait,
    _In_ ULONG LockKey,
    _Out_ PVOID Buffer,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    IoStatus->Status = STATUS_NOT_SUPPORTED;
    return FALSE;
}

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(RegistryPath);

    *DeviceName = L"CcCoherency";
    *Flags = TESTENTRY_NO_EXCLUSIVE_DEVICE |
             TESTENTRY_BUFFERED_IO_DEVICE |
             TESTENTRY_NO_READONLY_DEVICE;

    Disk = ExAllocatePoolWithTag(NonPagedPool, TEST_SIZE, 'hCcK');
    Scratch = ExAllocatePoolWithTag(NonPagedPool, TEST_SIZE, 'hCcK');
    if (Disk == NULL || Scratch == NULL)
    {
        if (Disk != NULL)
            ExFreePoolWithTag(Disk, 'hCcK');
        if (Scratch != NULL)
            ExFreePoolWithTag(Scratch, 'hCcK');
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KmtRegisterIrpHandler(IRP_MJ_CLEANUP, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_CREATE, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_READ, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_WRITE, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_FLUSH_BUFFERS, NULL, TestIrpHandler);

    TestFastIoDispatch.FastIoRead = FastIoRead;
    TestFastIoDispatch.FastIoWrite = FastIoWrite;
    DriverObject->FastIoDispatch = &TestFastIoDispatch;

    return STATUS_SUCCESS;
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    if (Disk != NULL)
        ExFreePoolWithTag(Disk, 'hCcK');
    if (Scratch != NULL)
        ExFreePoolWithTag(Scratch, 'hCcK');
}

static
BOOLEAN
NTAPI
AcquireForLazyWrite(
    _In_ PVOID Context,
    _In_ BOOLEAN Wait)
{
    return TRUE;
}

static
VOID
NTAPI
ReleaseFromLazyWrite(
    _In_ PVOID Context)
{
}

static
BOOLEAN
NTAPI
AcquireForReadAhead(
    _In_ PVOID Context,
    _In_ BOOLEAN Wait)
{
    return TRUE;
}

static
VOID
NTAPI
ReleaseFromReadAhead(
    _In_ PVOID Context)
{
}

static CACHE_MANAGER_CALLBACKS Callbacks = {
    AcquireForLazyWrite,
    ReleaseFromLazyWrite,
    AcquireForReadAhead,
    ReleaseFromReadAhead,
};

/* Helpers ------------------------------------------------------------------ */

static
VOID
ResetCache(
    _In_ PFILE_OBJECT FileObject)
{
    IO_STATUS_BLOCK IoStatus;

    CcFlushCache(FileObject->SectionObjectPointer, NULL, 0, &IoStatus);
    CcPurgeCacheSection(FileObject->SectionObjectPointer, NULL, 0, FALSE);
}

static
BOOLEAN
CachedRangeIs(
    _In_ PFILE_OBJECT FileObject,
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ UCHAR Pattern)
{
    LARGE_INTEGER FileOffset;
    IO_STATUS_BLOCK IoStatus;
    BOOLEAN Ok = FALSE;
    ULONG i;

    FileOffset.QuadPart = Offset;
    RtlFillMemory(Scratch, Length, (UCHAR)~Pattern);
    _SEH2_TRY
    {
        Ok = CcCopyRead(FileObject, &FileOffset, Length, TRUE, Scratch, &IoStatus);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Ok = FALSE;
    }
    _SEH2_END;

    if (!Ok)
        return FALSE;

    for (i = 0; i < Length; i++)
    {
        if (Scratch[i] != Pattern)
            return FALSE;
    }
    return TRUE;
}

static
VOID
WarmCache(
    _In_ PFILE_OBJECT FileObject,
    _In_ UCHAR Pattern)
{
    ResetCache(FileObject);
    RtlFillMemory(Disk, TEST_SIZE, Pattern);
    ok(CachedRangeIs(FileObject, 0, TEST_SIZE, Pattern), "cache warm-up read the wrong bytes\n");
}

static
BOOLEAN
DirtyRange(
    _In_ PFILE_OBJECT FileObject,
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ UCHAR Pattern)
{
    LARGE_INTEGER FileOffset;
    BOOLEAN Ok = FALSE;

    FileOffset.QuadPart = Offset;
    RtlFillMemory(Scratch, Length, Pattern);
    _SEH2_TRY
    {
        Ok = CcCopyWrite(FileObject, &FileOffset, Length, TRUE, Scratch);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Ok = FALSE;
    }
    _SEH2_END;

    return Ok;
}

static
BOOLEAN
DiskRangeIs(
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ UCHAR Pattern)
{
    ULONG i;

    for (i = Offset; i < Offset + Length; i++)
    {
        if (Disk[i] != Pattern)
            return FALSE;
    }
    return TRUE;
}

/* T-A4: CcPurgeCacheSection over byte ranges that cover partial pages ------ */

static
VOID
Test_PurgePartialPage(
    _In_ PFILE_OBJECT FileObject)
{
    static const struct
    {
        ULONG Offset;
        ULONG Length;
    } Ranges[] = {
        { 0x200, 0x200 },       /* inside page 0 */
        { 0xE00, 0x400 },       /* the end of page 0 and the start of page 1 */
        { 0x1000, 0x2000 },     /* pages 1 and 2, aligned */
        { 0x1200, 0x1C00 },     /* partial page 1 to the end of the file */
    };
    LARGE_INTEGER Offset;
    ULONG i, First, Last;
    BOOLEAN Purged;

    for (i = 0; i < RTL_NUMBER_OF(Ranges); i++)
    {
        WarmCache(FileObject, 0x11);
        RtlFillMemory(Disk, TEST_SIZE, 0x77);

        Offset.QuadPart = Ranges[i].Offset;
        Purged = CcPurgeCacheSection(FileObject->SectionObjectPointer, &Offset, Ranges[i].Length, FALSE);

        First = Ranges[i].Offset & ~(PAGE_SIZE - 1);
        Last = (Ranges[i].Offset + Ranges[i].Length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (Purged)
        {
            ok(CachedRangeIs(FileObject, First, Last - First, 0x77),
               "purge [0x%lx, 0x%lx) returned TRUE but a page it touches still holds stale data\n",
               Ranges[i].Offset, Ranges[i].Offset + Ranges[i].Length);
        }
        else
        {
            trace("purge [0x%lx, 0x%lx) returned FALSE\n",
                  Ranges[i].Offset, Ranges[i].Offset + Ranges[i].Length);
        }
    }
}

/* T-A3: CcCoherencyFlushAndPurgeCache -------------------------------------- */

static
VOID
CheckCoherent(
    _In_ PFILE_OBJECT FileObject,
    _In_ PCSTR Case,
    _In_ NTSTATUS Status,
    _In_ ULONG Offset,
    _In_ UCHAR FreshPattern)
{
    if (NT_SUCCESS(Status))
    {
        ok(CachedRangeIs(FileObject, Offset, PAGE_SIZE, FreshPattern),
           "%s: reported 0x%lx but the cache still returns stale data\n", Case, Status);
    }
    else
    {
        ok(Status == STATUS_PURGE_FAILED, "%s: unexpected failure status 0x%lx\n", Case, Status);
    }
}

static
VOID
Test_CoherencyFlushAndPurge(
    _In_ PFILE_OBJECT FileObject)
{
    LARGE_INTEGER Offset;
    IO_STATUS_BLOCK IoStatus;
    PVOID Bcb, Buffer;
    BOOLEAN Pinned = FALSE;

    Offset.QuadPart = PAGE_SIZE;

    /* Flags 0: the dirty page reaches the disk and leaves the cache */
    WarmCache(FileObject, 0x11);
    ok(DirtyRange(FileObject, PAGE_SIZE, PAGE_SIZE, 0xD1), "CcCopyWrite failed\n");
    IoStatus.Status = STATUS_UNSUCCESSFUL;
    CcCoherencyFlushAndPurgeCache(FileObject->SectionObjectPointer, &Offset, PAGE_SIZE, &IoStatus, 0);
    ok(DiskRangeIs(PAGE_SIZE, PAGE_SIZE, 0xD1), "flags 0: the dirty page was not written\n");
    RtlFillMemory(Disk + PAGE_SIZE, PAGE_SIZE, 0xE1);
    CheckCoherent(FileObject, "flags 0", IoStatus.Status, PAGE_SIZE, 0xE1);

    /* CC_FLUSH_AND_PURGE_NO_PURGE: written, and still cached */
    WarmCache(FileObject, 0x11);
    ok(DirtyRange(FileObject, PAGE_SIZE, PAGE_SIZE, 0xD2), "CcCopyWrite failed\n");
    IoStatus.Status = STATUS_UNSUCCESSFUL;
    CcCoherencyFlushAndPurgeCache(FileObject->SectionObjectPointer, &Offset, PAGE_SIZE, &IoStatus,
                                  CC_FLUSH_AND_PURGE_NO_PURGE);
    ok(NT_SUCCESS(IoStatus.Status), "no-purge: status 0x%lx\n", IoStatus.Status);
    ok(DiskRangeIs(PAGE_SIZE, PAGE_SIZE, 0xD2), "no-purge: the dirty page was not written\n");
    RtlFillMemory(Disk + PAGE_SIZE, PAGE_SIZE, 0xE2);
    ok(CachedRangeIs(FileObject, PAGE_SIZE, PAGE_SIZE, 0xD2), "no-purge: the page left the cache\n");

    /* A pinned range: success only if the cache really became coherent */
    WarmCache(FileObject, 0x11);
    _SEH2_TRY
    {
        Pinned = CcPinRead(FileObject, &Offset, PAGE_SIZE, PIN_WAIT, &Bcb, &Buffer);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Pinned = FALSE;
    }
    _SEH2_END;
    ok(Pinned, "CcPinRead failed\n");
    RtlFillMemory(Disk + PAGE_SIZE, PAGE_SIZE, 0xE3);
    IoStatus.Status = STATUS_UNSUCCESSFUL;
    CcCoherencyFlushAndPurgeCache(FileObject->SectionObjectPointer, &Offset, PAGE_SIZE, &IoStatus, 0);
    if (Pinned)
        CcUnpinData(Bcb);
    CheckCoherent(FileObject, "pinned", IoStatus.Status, PAGE_SIZE, 0xE3);

    ResetCache(FileObject);
}

/* Shrink while the view holding the new end of file is pinned --------------- */

static
VOID
SetTestFileSize(
    _In_ PFILE_OBJECT FileObject,
    _In_ LONGLONG FileSize,
    _In_ LONGLONG ValidDataLength)
{
    PTEST_FCB Fcb = FileObject->FsContext;

    Fcb->Header.FileSize.QuadPart = FileSize;
    Fcb->Header.ValidDataLength.QuadPart = ValidDataLength;
    CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
}

static
VOID
Test_ShrinkPinnedEofView(
    _In_ PFILE_OBJECT FileObject)
{
    const ULONG Cut = PAGE_SIZE + 0x200;
    LARGE_INTEGER Offset;
    IO_STATUS_BLOCK IoStatus;
    PVOID Bcb, Buffer;
    BOOLEAN Pinned = FALSE;

    WarmCache(FileObject, 0x11);
    ok(DirtyRange(FileObject, 0, TEST_SIZE, 0x33), "CcCopyWrite failed\n");

    Offset.QuadPart = 0;
    _SEH2_TRY
    {
        Pinned = CcPinRead(FileObject, &Offset, PAGE_SIZE, PIN_WAIT, &Bcb, &Buffer);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Pinned = FALSE;
    }
    _SEH2_END;
    ok(Pinned, "CcPinRead failed\n");

    /* The file system frees the clusters beyond the cut, and zeroes them on the extend */
    SetTestFileSize(FileObject, Cut, Cut);
    if (Pinned)
        CcUnpinData(Bcb);
    RtlZeroMemory(Disk + Cut, TEST_SIZE - Cut);
    SetTestFileSize(FileObject, TEST_SIZE, Cut);

    ok(CachedRangeIs(FileObject, 0, Cut, 0x33), "pinned shrink: bytes before the cut were lost\n");
    ok(CachedRangeIs(FileObject, Cut, TEST_SIZE - Cut, 0),
       "pinned shrink: stale bytes beyond the cut reappeared after the extend\n");

    CcFlushCache(FileObject->SectionObjectPointer, NULL, 0, &IoStatus);
    ok(DiskRangeIs(0, Cut, 0x33), "pinned shrink: bytes before the cut did not reach the disk\n");
    ok(DiskRangeIs(Cut, TEST_SIZE - Cut, 0), "pinned shrink: stale bytes beyond the cut reached the disk\n");

    SetTestFileSize(FileObject, TEST_SIZE, TEST_SIZE);
    ResetCache(FileObject);
}

/* IRP handler --------------------------------------------------------------- */

static
NTSTATUS
TestIrpHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStack)
{
    LARGE_INTEGER Zero = RTL_CONSTANT_LARGE_INTEGER(0LL);
    CACHE_UNINITIALIZE_EVENT CacheUninitEvent;
    NTSTATUS Status = STATUS_NOT_SUPPORTED;
    PTEST_FCB Fcb;
    PUCHAR Buffer;
    ULONG Length, Count;
    LONGLONG Offset;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(DeviceObject);

    /* Every Cc call below takes resources: kernel APCs must be off, as in a real FS */
    FsRtlEnterFileSystem();

    Irp->IoStatus.Information = 0;

    if (IoStack->MajorFunction == IRP_MJ_CREATE)
    {
        Fcb = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Fcb), 'hCcK');
        if (Fcb == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Complete;
        }
        RtlZeroMemory(Fcb, sizeof(*Fcb));
        ExInitializeFastMutex(&Fcb->HeaderMutex);
        FsRtlSetupAdvancedHeader(&Fcb->Header, &Fcb->HeaderMutex);
        Fcb->Header.AllocationSize.QuadPart = TEST_SIZE;
        Fcb->Header.FileSize.QuadPart = TEST_SIZE;
        Fcb->Header.ValidDataLength.QuadPart = TEST_SIZE;
        Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
        IoStack->FileObject->FsContext = Fcb;
        IoStack->FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;

        CcInitializeCacheMap(IoStack->FileObject,
                             (PCC_FILE_SIZES)&Fcb->Header.AllocationSize,
                             FALSE, &Callbacks, NULL);

        Irp->IoStatus.Information = FILE_OPENED;
        Status = STATUS_SUCCESS;
    }
    else if (IoStack->MajorFunction == IRP_MJ_READ || IoStack->MajorFunction == IRP_MJ_WRITE)
    {
        if (IoStack->MajorFunction == IRP_MJ_WRITE && !FlagOn(Irp->Flags, IRP_NOCACHE))
        {
            /* The user-mode half writes once: run the tests from here */
            Test_PurgePartialPage(IoStack->FileObject);
            Test_CoherencyFlushAndPurge(IoStack->FileObject);
            Test_ShrinkPinnedEofView(IoStack->FileObject);
            Irp->IoStatus.Information = IoStack->Parameters.Write.Length;
            Status = STATUS_SUCCESS;
            goto Complete;
        }

        /* Paging I/O from Cc: the backing buffer is the disk */
        if (IoStack->MajorFunction == IRP_MJ_READ)
        {
            Offset = IoStack->Parameters.Read.ByteOffset.QuadPart;
            Length = IoStack->Parameters.Read.Length;
        }
        else
        {
            Offset = IoStack->Parameters.Write.ByteOffset.QuadPart;
            Length = IoStack->Parameters.Write.Length;
        }

        Buffer = (Irp->MdlAddress != NULL) ? MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority) : NULL;
        ok(Buffer != NULL, "paging I/O without a mapped MDL\n");
        if (Buffer == NULL || Offset < 0)
        {
            Status = STATUS_INVALID_PARAMETER;
            goto Complete;
        }

        Count = (Offset >= TEST_SIZE) ? 0 : (ULONG)min((LONGLONG)Length, TEST_SIZE - Offset);
        if (IoStack->MajorFunction == IRP_MJ_READ)
        {
            RtlZeroMemory(Buffer, Length);
            RtlCopyMemory(Buffer, Disk + Offset, Count);
        }
        else
        {
            RtlCopyMemory(Disk + Offset, Buffer, Count);
        }

        Irp->IoStatus.Information = Length;
        Status = STATUS_SUCCESS;
    }
    else if (IoStack->MajorFunction == IRP_MJ_FLUSH_BUFFERS)
    {
        IO_STATUS_BLOCK IoStatus;

        Fcb = IoStack->FileObject->FsContext;
        CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &IoStatus);
        Status = STATUS_SUCCESS;
    }
    else if (IoStack->MajorFunction == IRP_MJ_CLEANUP)
    {
        KeInitializeEvent(&CacheUninitEvent.Event, NotificationEvent, FALSE);
        CcUninitializeCacheMap(IoStack->FileObject, &Zero, &CacheUninitEvent);
        KeWaitForSingleObject(&CacheUninitEvent.Event, Executive, KernelMode, FALSE, NULL);
        Fcb = IoStack->FileObject->FsContext;
        ExFreePoolWithTag(Fcb, 'hCcK');
        IoStack->FileObject->FsContext = NULL;
        Status = STATUS_SUCCESS;
    }

Complete:
    FsRtlExitFileSystem();

    if (Status == STATUS_PENDING)
    {
        IoMarkIrpPending(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        Status = STATUS_PENDING;
    }
    else
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    return Status;
}
