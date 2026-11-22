/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Rename/delete/open semantics for a file with an active image section
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/*
 * Windows (measured on Win11 26100) treats a file that is currently mapped as an
 * IMAGE (LoadLibrary) as follows, and this test encodes that contract:
 *   - open with DELETE access (share R/W/D):            succeeds
 *   - rename to a FREE name (FileRenameInformation):    succeeds
 *   - rename OVER it / replace (ReplaceIfExists),
 *       classic and POSIX:                              STATUS_ACCESS_DENIED
 *   - in-place delete (FileDispositionInformation):     STATUS_CANNOT_DELETE
 *   - open for write (FILE_WRITE_DATA, share R/W/D):    STATUS_SHARING_VIOLATION
 * Some of these are expected to FAIL on LiberNT today where our stack diverges;
 * the test documents the target behaviour (servicing / PendingFileRenameOperations
 * depends on rename-to-a-free-name being permitted on a mapped image).
 */

#include "precomp.h"
#include <ndk/iofuncs.h>

#define RENAME_REPLACE_IF_EXISTS 0x00000001
#define RENAME_POSIX_SEMANTICS   0x00000002

static WCHAR TestDirectory[MAX_PATH];

static
VOID
MakePath(
    _Out_writes_(MAX_PATH) PWSTR Path,
    _In_ PCWSTR Name)
{
    StringCchPrintfW(Path, MAX_PATH, L"%s\\%s", TestDirectory, Name);
}

/* Copy a leaf system DLL into our test dir and map it as an image. Returns the
 * module handle, or NULL (with the test skipped) if no candidate could be used. */
static
HMODULE
MapScratchImage(
    _Out_writes_(MAX_PATH) PWSTR ImagePath)
{
    static PCWSTR const Candidates[] = { L"msimg32.dll", L"version.dll", L"imagehlp.dll", L"winmm.dll" };
    WCHAR SystemDir[MAX_PATH], Source[MAX_PATH];
    HMODULE Module;
    UINT i;

    if (!GetSystemDirectoryW(SystemDir, RTL_NUMBER_OF(SystemDir)))
        return NULL;
    for (i = 0; i < RTL_NUMBER_OF(Candidates); i++)
    {
        StringCchPrintfW(Source, MAX_PATH, L"%s\\%s", SystemDir, Candidates[i]);
        MakePath(ImagePath, L"mir_probe.dll");
        DeleteFileW(ImagePath);
        if (!CopyFileW(Source, ImagePath, FALSE))
            continue;
        /* DONT_RESOLVE_DLL_REFERENCES still creates an image section but skips
         * DllMain and import resolution, so a leaf DLL maps cleanly. */
        Module = LoadLibraryExW(ImagePath, NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (Module)
            return Module;
        DeleteFileW(ImagePath);
    }
    return NULL;
}

static
NTSTATUS
RenameTo(
    _In_ PCWSTR Source,
    _In_ PCWSTR NewLeaf,
    _In_ ULONG Flags,
    _In_ FILE_INFORMATION_CLASS Class)
{
    UCHAR Buffer[sizeof(FILE_RENAME_INFORMATION) + MAX_PATH * sizeof(WCHAR)];
    PFILE_RENAME_INFORMATION Info = (PFILE_RENAME_INFORMATION)Buffer;
    WCHAR NtTarget[MAX_PATH];
    UNICODE_STRING NtName;
    IO_STATUS_BLOCK IoStatusBlock;
    HANDLE File;
    NTSTATUS Status;
    ULONG Length;

    StringCchPrintfW(NtTarget, MAX_PATH, L"\\??\\%s\\%s", TestDirectory, NewLeaf);
    RtlInitUnicodeString(&NtName, NtTarget);
    Length = NtName.Length;

    File = CreateFileW(Source, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return STATUS_UNSUCCESSFUL;
    memset(Buffer, 0, sizeof(Buffer));
    if (Class == FileRenameInformationEx)
        *(PULONG)Info = Flags;
    else
        Info->ReplaceIfExists = !!(Flags & RENAME_REPLACE_IF_EXISTS);
    Info->FileNameLength = Length;
    memcpy(Info->FileName, NtName.Buffer, Length);
    Status = NtSetInformationFile(File, &IoStatusBlock, Info,
                                  FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) + Length, Class);
    CloseHandle(File);
    return Status;
}

START_TEST(MappedImageRename)
{
    WCHAR Image[MAX_PATH], Free[MAX_PATH], NewFile[MAX_PATH];
    HMODULE Module;
    HANDLE File;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_DISPOSITION_INFORMATION Disposition;
    NTSTATUS Status;
    DWORD Length;

    Length = GetTempPathW(RTL_NUMBER_OF(TestDirectory), TestDirectory);
    ok(Length != 0, "GetTempPathW failed with %lu\n", GetLastError());
    StringCchCatW(TestDirectory, MAX_PATH, L"mir_test");
    CreateDirectoryW(TestDirectory, NULL);

    Module = MapScratchImage(Image);
    if (!Module)
    {
        skip("Could not map a scratch image\n");
        RemoveDirectoryW(TestDirectory);
        return;
    }

    /* open DELETE on the mapped image -> allowed */
    File = CreateFileW(Image, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    ok(File != INVALID_HANDLE_VALUE, "open DELETE on a mapped image failed with %lu\n", GetLastError());
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);

    /* open FILE_WRITE_DATA on the mapped image -> sharing violation */
    SetLastError(0);
    File = CreateFileW(Image, FILE_WRITE_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, 0, NULL);
    ok(File == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
       "open FILE_WRITE_DATA on a mapped image: handle %p error %lu (want sharing violation)\n", File, GetLastError());
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);

    /* rename the mapped image to a FREE name -> allowed; then rename it back */
    Status = RenameTo(Image, L"mir_moved.dll", 0, FileRenameInformation);
    ok(Status == STATUS_SUCCESS, "rename of a mapped image to a free name returned 0x%lx (want SUCCESS)\n", Status);
    if (Status == STATUS_SUCCESS)
    {
        MakePath(Free, L"mir_moved.dll");
        Status = RenameTo(Free, L"mir_probe.dll", 0, FileRenameInformation);
        ok(Status == STATUS_SUCCESS, "rename back returned 0x%lx\n", Status);
    }

    /* replace (rename-over) the mapped image -> access denied, classic and POSIX */
    MakePath(NewFile, L"mir_new.dll");
    DeleteFileW(NewFile);
    ok(CopyFileW(Image, NewFile, FALSE), "staging replacement file failed with %lu\n", GetLastError());
    Status = RenameTo(NewFile, L"mir_probe.dll", RENAME_REPLACE_IF_EXISTS, FileRenameInformation);
    ok(Status == STATUS_ACCESS_DENIED, "classic replace over a mapped image returned 0x%lx (want ACCESS_DENIED)\n", Status);
    Status = RenameTo(NewFile, L"mir_probe.dll", RENAME_REPLACE_IF_EXISTS | RENAME_POSIX_SEMANTICS,
                      FileRenameInformationEx);
    ok(Status == STATUS_ACCESS_DENIED, "POSIX replace over a mapped image returned 0x%lx (want ACCESS_DENIED)\n", Status);
    DeleteFileW(NewFile);

    /* in-place delete of the mapped image -> cannot delete */
    File = CreateFileW(Image, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (File != INVALID_HANDLE_VALUE)
    {
        memset(&Disposition, 0, sizeof(Disposition));
        Disposition.DeleteFile = TRUE;
        Status = NtSetInformationFile(File, &IoStatusBlock, &Disposition, sizeof(Disposition), FileDispositionInformation);
        ok(Status == STATUS_CANNOT_DELETE, "in-place delete of a mapped image returned 0x%lx (want CANNOT_DELETE)\n", Status);
        CloseHandle(File);
    }

    /* teardown: unmap, then the file can be deleted */
    FreeLibrary(Module);
    ok(DeleteFileW(Image), "delete after FreeLibrary failed with %lu\n", GetLastError());
    RemoveDirectoryW(TestDirectory);
}
