/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     FsRtlAreThereCurrentOrInProgressFileLocks counts lock requests
 *              in progress, not only granted locks
 */

#include <kmt_test.h>
#include <ntstrsafe.h>

/* The in-progress half alone. A forwarder to FsRtlAreThereCurrentFileLocks
 * (the ntoskrnl_vista shim this replaces) answers FALSE on every line marked
 * (in-progress) below, so this is the fail-first half of the test. */
static
VOID
TestInProgressOnly(VOID)
{
    FILE_LOCK Lock;

    FsRtlInitializeFileLock(&Lock, NULL, NULL);
    ok_bool_false(FsRtlAreThereCurrentFileLocks(&Lock), "Current, fresh lock");
    ok_bool_false(FsRtlAreThereCurrentOrInProgressFileLocks(&Lock), "CurrentOrInProgress, fresh lock");

    FsRtlIncrementLockRequestsInProgress(&Lock);
    ok_eq_long(Lock.LockRequestsInProgress, 1L);
    ok_bool_false(FsRtlAreThereCurrentFileLocks(&Lock), "Current, 1 in progress");
    ok_bool_true(FsRtlAreThereCurrentOrInProgressFileLocks(&Lock), "CurrentOrInProgress, 1 in progress (in-progress)");

    FsRtlIncrementLockRequestsInProgress(&Lock);
    ok_bool_true(FsRtlAreThereCurrentOrInProgressFileLocks(&Lock), "CurrentOrInProgress, 2 in progress (in-progress)");

    FsRtlDecrementLockRequestsInProgress(&Lock);
    ok_bool_true(FsRtlAreThereCurrentOrInProgressFileLocks(&Lock), "CurrentOrInProgress, back to 1 (in-progress)");

    FsRtlDecrementLockRequestsInProgress(&Lock);
    ok_eq_long(Lock.LockRequestsInProgress, 0L);
    ok_bool_false(FsRtlAreThereCurrentOrInProgressFileLocks(&Lock), "CurrentOrInProgress, back to 0");

    FsRtlUninitializeFileLock(&Lock);
}

/* OR semantics against a real granted lock on a private FILE_LOCK: either
 * half alone must answer TRUE, and only both clear give FALSE. */
static
VOID
TestCombinedWithGrantedLock(VOID)
{
    WCHAR NameBuffer[128];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Handle;
    PFILE_OBJECT FileObject;
    PEPROCESS Process = PsGetCurrentProcess();
    FILE_LOCK Lock;
    LARGE_INTEGER Offset, Length;
    NTSTATUS Status;
    BOOLEAN Result;

    RtlStringCchPrintfW(NameBuffer, RTL_NUMBER_OF(NameBuffer),
                        L"\\SystemRoot\\Temp\\kmtest-lock-state-%p.tmp", PsGetCurrentThread());
    RtlInitUnicodeString(&Name, NameBuffer);
    InitializeObjectAttributes(&Attributes, &Name,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&Handle, FILE_READ_DATA | FILE_WRITE_DATA | DELETE | SYNCHRONIZE,
                          &Attributes, &IoStatus, NULL, FILE_ATTRIBUTE_NORMAL,
                          0, FILE_CREATE,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                          FILE_DELETE_ON_CLOSE, NULL, 0);
    if (skip(NT_SUCCESS(Status), "Cannot create temporary test file: %lx\n", Status))
        return;
    Status = ObReferenceObjectByHandle(Handle, 0, *IoFileObjectType,
                                       KernelMode, (PVOID *)&FileObject, NULL);
    if (!NT_SUCCESS(Status))
    {
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Handle);
        return;
    }

    FsRtlInitializeFileLock(&Lock, NULL, NULL);
    Offset.QuadPart = 0;
    Length.QuadPart = 16;

    /* Granted lock, no request in progress: TRUE from the current-locks half */
    Result = FsRtlFastLock(&Lock, FileObject, &Offset, &Length, Process, 1, FALSE, TRUE,
                           &IoStatus, NULL, TRUE);
    ok_bool_true(Result, "FsRtlFastLock");
    ok_eq_hex(IoStatus.Status, STATUS_SUCCESS);
    ok_bool_true(FsRtlAreThereCurrentFileLocks(&Lock), "Current, lock granted");
    ok_bool_true(FsRtlAreThereCurrentOrInProgressFileLocks(&Lock), "CurrentOrInProgress, lock granted");

    /* Both halves set */
    FsRtlIncrementLockRequestsInProgress(&Lock);
    ok_bool_true(FsRtlAreThereCurrentOrInProgressFileLocks(&Lock), "CurrentOrInProgress, granted + in progress");

    /* Lock released while the request is still in progress: TRUE from the
     * in-progress half alone (in-progress) */
    Status = FsRtlFastUnlockSingle(&Lock, FileObject, &Offset, &Length, Process, 1, NULL, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_bool_false(FsRtlAreThereCurrentFileLocks(&Lock), "Current, released");
    ok_bool_true(FsRtlAreThereCurrentOrInProgressFileLocks(&Lock), "CurrentOrInProgress, released but in progress (in-progress)");

    /* Both clear */
    FsRtlDecrementLockRequestsInProgress(&Lock);
    ok_bool_false(FsRtlAreThereCurrentOrInProgressFileLocks(&Lock), "CurrentOrInProgress, released and done");

    FsRtlUninitializeFileLock(&Lock);
    ok_bool_false(FsRtlAreThereCurrentOrInProgressFileLocks(&Lock), "CurrentOrInProgress, after uninitialize");

    ObDereferenceObject(FileObject);
    ZwClose(Handle);
}

START_TEST(FsRtlFileLockState)
{
    TestInProgressOnly();
    TestCombinedWithGrantedLock();
}
