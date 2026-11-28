/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Section creation holds the file for section synchronization
 *              (B-1 MiCreateSectionWithMode acquire): image loads and
 *              create-vs-truncate stress
 */

#include <kmt_test.h>
#include <ntstrsafe.h>

/*
 * B-1 wraps the size query and the control-area setup of NtCreateSection in
 * FsRtlAcquireToCreateMappedSection / FsRtlReleaseFile. Two runtime legs:
 *
 *  1. Image sections keep working: SEC_IMAGE over the system DLLs and an EXE,
 *     mapped into the system process, must expose the MZ header on every
 *     iteration (the acquire must neither deadlock nor fail the load).
 *  2. Create-vs-truncate: one thread shrinks and grows a scratch file without
 *     pause while this thread creates, queries, maps and touches a data
 *     section over it. With the file held during creation the section size is
 *     always one of the two sizes the writer sets, every create succeeds, and
 *     touching the view never raises an in-page error. A shrink that meets a
 *     mapped view is refused with STATUS_USER_MAPPED_FILE, which is counted,
 *     not failed.
 */

#define SA_TAG          'ASmK'
#define SA_SMALL        PAGE_SIZE
#define SA_LARGE        (16 * PAGE_SIZE)
#define SA_IMAGE_ITER   16
#define SA_STRESS_ITER  1500
#define SA_STRESS_SECS  15

typedef struct _TRUNCATE_CONTEXT
{
    HANDLE File;
    volatile LONG Stop;
    LONG Shrinks;
    LONG Grows;
    LONG MappedConflicts;
    LONG OtherFailures;
    NTSTATUS LastOtherStatus;
} TRUNCATE_CONTEXT, *PTRUNCATE_CONTEXT;

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

static
VOID
NTAPI
TruncateThread(
    _In_ PVOID Parameter)
{
    PTRUNCATE_CONTEXT Context = Parameter;
    NTSTATUS Status;
    ULONG64 Size = SA_SMALL;

    while (!Context->Stop)
    {
        Status = SetEndOfFile(Context->File, Size);
        if (NT_SUCCESS(Status))
        {
            if (Size == SA_SMALL)
                Context->Shrinks++;
            else
                Context->Grows++;
        }
        else if (Status == STATUS_USER_MAPPED_FILE)
        {
            Context->MappedConflicts++;
        }
        else
        {
            Context->OtherFailures++;
            Context->LastOtherStatus = Status;
        }
        Size = (Size == SA_SMALL) ? SA_LARGE : SA_SMALL;
        ZwYieldExecution();
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Map a SEC_IMAGE section of Path into the system process and look at the
 * DOS header. Returns TRUE when the whole chain worked. */
static
BOOLEAN
LoadImageOnce(
    _In_ PCWSTR Path,
    _Out_ PNTSTATUS FailedStatus)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    HANDLE File = NULL, Section = NULL;
    PVOID Base = NULL;
    SIZE_T ViewSize = 0;
    NTSTATUS Status;
    BOOLEAN Ok = FALSE;
    USHORT Magic = 0;

    *FailedStatus = STATUS_SUCCESS;
    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwOpenFile(&File, FILE_READ_DATA | FILE_EXECUTE | SYNCHRONIZE, &Attributes, &IoStatus,
                        FILE_SHARE_READ | FILE_SHARE_DELETE,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
    {
        *FailedStatus = Status;
        return FALSE;
    }
    Status = ZwCreateSection(&Section, SECTION_MAP_READ | SECTION_MAP_EXECUTE | SECTION_QUERY,
                             NULL, NULL, PAGE_EXECUTE, SEC_IMAGE, File);
    if (NT_SUCCESS(Status))
    {
        Status = ZwMapViewOfSection(Section, ZwCurrentProcess(), &Base, 0, 0, NULL, &ViewSize,
                                    ViewUnmap, 0, PAGE_READONLY);
        if (NT_SUCCESS(Status))
        {
            _SEH2_TRY
            {
                Magic = *(volatile USHORT *)Base;
                Ok = (Magic == IMAGE_DOS_SIGNATURE);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
            }
            _SEH2_END;
            ZwUnmapViewOfSection(ZwCurrentProcess(), Base);
        }
        ZwClose(Section);
    }
    ZwClose(File);
    if (!Ok)
        *FailedStatus = NT_SUCCESS(Status) ? STATUS_INVALID_IMAGE_FORMAT : Status;
    return Ok;
}

static
VOID
TestImageLoads(VOID)
{
    static const PCWSTR Images[] =
    {
        L"\\SystemRoot\\system32\\ntdll.dll",
        L"\\SystemRoot\\system32\\kernel32.dll",
        L"\\SystemRoot\\system32\\user32.dll",
        L"\\SystemRoot\\system32\\cmd.exe",
    };
    ULONG i, n, Good;
    NTSTATUS Failed, FirstFailed;

    for (i = 0; i < RTL_NUMBER_OF(Images); i++)
    {
        Good = 0;
        FirstFailed = STATUS_SUCCESS;
        for (n = 0; n < SA_IMAGE_ITER; n++)
        {
            if (LoadImageOnce(Images[i], &Failed))
                Good++;
            else if (FirstFailed == STATUS_SUCCESS)
                FirstFailed = Failed;
        }
        ok(Good == SA_IMAGE_ITER, "%S: %lu of %u SEC_IMAGE loads showed the MZ header (first failure 0x%lx)\n",
           Images[i], Good, SA_IMAGE_ITER, FirstFailed);
    }
}

static
VOID
TestCreateVersusTruncate(VOID)
{
    WCHAR Path[128];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    HANDLE File = NULL, Section;
    PUCHAR Fill;
    NTSTATUS Status;
    TRUNCATE_CONTEXT Context;
    PKTHREAD Thread;
    ULONG Iteration, Creates = 0, CreateFailures = 0, BadSizes = 0, MapFailures = 0, InPageErrors = 0;
    NTSTATUS FirstCreateFailure = STATUS_SUCCESS, FirstMapFailure = STATUS_SUCCESS;
    LARGE_INTEGER Start, Now;
    SECTION_BASIC_INFORMATION Basic;
    PVOID Base;
    SIZE_T ViewSize;
    volatile UCHAR Touch;

    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"\\SystemRoot\\Temp\\kmt-section-%p.bin", PsGetCurrentThread());
    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&File,
                          FILE_READ_DATA | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | DELETE | SYNCHRONIZE,
                          &Attributes, &IoStatus, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OVERWRITE_IF,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_DELETE_ON_CLOSE,
                          NULL, 0);
    if (skip(NT_SUCCESS(Status), "Cannot create %S: 0x%lx\n", Path, Status))
        return;

    Fill = ExAllocatePoolWithTag(NonPagedPool, SA_LARGE, SA_TAG);
    if (skip(Fill != NULL, "Cannot allocate the fill buffer\n"))
    {
        ZwClose(File);
        return;
    }
    RtlFillMemory(Fill, SA_LARGE, 0x5A);
    Status = ZwWriteFile(File, NULL, NULL, NULL, &IoStatus, Fill, SA_LARGE, NULL, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ZwFlushBuffersFile(File, &IoStatus);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ExFreePoolWithTag(Fill, SA_TAG);

    RtlZeroMemory(&Context, sizeof(Context));
    Context.File = File;
    Thread = KmtStartThread(TruncateThread, &Context);
    if (skip(Thread != NULL, "No truncate thread\n"))
    {
        ZwClose(File);
        return;
    }

    KeQuerySystemTime(&Start);
    for (Iteration = 0; Iteration < SA_STRESS_ITER; Iteration++)
    {
        Section = NULL;
        Status = ZwCreateSection(&Section, SECTION_MAP_READ | SECTION_QUERY, NULL, NULL,
                                 PAGE_READONLY, SEC_COMMIT, File);
        if (!NT_SUCCESS(Status))
        {
            CreateFailures++;
            if (FirstCreateFailure == STATUS_SUCCESS)
                FirstCreateFailure = Status;
            ZwYieldExecution();
            continue;
        }
        Creates++;

        Status = ZwQuerySection(Section, SectionBasicInformation, &Basic, sizeof(Basic), NULL);
        if (!NT_SUCCESS(Status) ||
            (Basic.Size.QuadPart != SA_SMALL && Basic.Size.QuadPart != SA_LARGE))
        {
            BadSizes++;
            if (BadSizes == 1)
                trace("section size %I64d (query 0x%lx) is neither %u nor %u\n",
                      Basic.Size.QuadPart, Status, SA_SMALL, SA_LARGE);
        }

        Base = NULL;
        ViewSize = 0;
        Status = ZwMapViewOfSection(Section, ZwCurrentProcess(), &Base, 0, 0, NULL, &ViewSize,
                                    ViewUnmap, 0, PAGE_READONLY);
        if (NT_SUCCESS(Status))
        {
            _SEH2_TRY
            {
                Touch = *(volatile UCHAR *)Base;
                Touch = *((volatile UCHAR *)Base + ViewSize - 1);
                (VOID)Touch;
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                InPageErrors++;
                if (InPageErrors == 1)
                    trace("in-page error 0x%lx touching a %Iu byte view\n", _SEH2_GetExceptionCode(), ViewSize);
            }
            _SEH2_END;
            ZwUnmapViewOfSection(ZwCurrentProcess(), Base);
        }
        else
        {
            MapFailures++;
            if (FirstMapFailure == STATUS_SUCCESS)
                FirstMapFailure = Status;
        }
        ZwClose(Section);

        KeQuerySystemTime(&Now);
        if ((Now.QuadPart - Start.QuadPart) / 10000000 >= SA_STRESS_SECS)
            break;
        ZwYieldExecution();
    }

    Context.Stop = 1;
    KmtFinishThread(Thread, NULL);

    trace("stress: %lu iterations, %lu creates, shrinks %ld, grows %ld, shrink refused by a mapped view %ld\n",
          Iteration, Creates, Context.Shrinks, Context.Grows, Context.MappedConflicts);
    ok(Creates > 100, "only %lu sections were created\n", Creates);
    ok(Context.Shrinks + Context.Grows > 100, "the writer only changed the size %ld times\n",
       Context.Shrinks + Context.Grows);
    ok(CreateFailures == 0, "%lu of %lu creates failed, first 0x%lx\n", CreateFailures, Iteration, FirstCreateFailure);
    ok(BadSizes == 0, "%lu sections had a size that the writer never set\n", BadSizes);
    ok(MapFailures == 0, "%lu views could not be mapped, first 0x%lx\n", MapFailures, FirstMapFailure);
    ok(InPageErrors == 0, "%lu in-page errors touching mapped views\n", InPageErrors);
    ok(Context.OtherFailures == 0, "writer saw %ld unexpected set-EOF failures, last 0x%lx\n",
       Context.OtherFailures, Context.LastOtherStatus);

    /* The file is usable afterwards */
    Status = SetEndOfFile(File, SA_LARGE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(File);
}

START_TEST(MmSectionCreateAcquire)
{
    TestImageLoads();
    TestCreateVersusTruncate();
}
