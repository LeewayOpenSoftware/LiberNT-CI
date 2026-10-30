/*
 * PROJECT:     LiberNT NT Layer DLL
 * FILE:        dll/ntdll/ldr/ldrdlldir.c
 * PURPOSE:     Process DLL search directories owned by the loader
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

/* The LOAD_LIBRARY_SEARCH_* values, from the public winbase.h */
#define LDRP_SEARCH_APPLICATION_DIR 0x00000200
#define LDRP_SEARCH_USER_DIRS       0x00000400
#define LDRP_SEARCH_SYSTEM32        0x00000800
#define LDRP_SEARCH_DEFAULT_DIRS    0x00001000
#define LDRP_SEARCH_VALID_DEFAULTS  (LDRP_SEARCH_APPLICATION_DIR | \
                                     LDRP_SEARCH_USER_DIRS | \
                                     LDRP_SEARCH_SYSTEM32 | \
                                     LDRP_SEARCH_DEFAULT_DIRS)

typedef struct _LDRP_DLL_DIRECTORY
{
    LIST_ENTRY Entry;
    WCHAR Path[1];
} LDRP_DLL_DIRECTORY, *PLDRP_DLL_DIRECTORY;

/*
 * Zero means SetDefaultDllDirectories was never called: the loader then keeps
 * the search path it was given (or the process default), as it always did.
 */
static RTL_SRWLOCK LdrpDllDirectoryLock = RTL_SRWLOCK_INIT;
static LIST_ENTRY LdrpDllDirectories = {&LdrpDllDirectories, &LdrpDllDirectories};
static ULONG LdrpDefaultDllDirectoryFlags;

static
BOOLEAN
LdrpIsAbsoluteDosPath(
    _In_ PCWSTR Path)
{
    RTL_PATH_TYPE Type = RtlDetermineDosPathNameType_U(Path);

    return Type == RtlPathTypeRooted ||
           Type == RtlPathTypeDriveAbsolute ||
           Type == RtlPathTypeUncAbsolute ||
           Type == RtlPathTypeLocalDevice;
}

NTSTATUS
NTAPI
LdrAddDllDirectory(
    _In_ PUNICODE_STRING Directory,
    _Out_ PVOID *Cookie)
{
    PLDRP_DLL_DIRECTORY Entry;
    FILE_BASIC_INFORMATION Information;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING NtPath;
    PWSTR Dos;
    NTSTATUS Status;
    SIZE_T Length;

    if (!Directory || !Directory->Buffer || !Cookie)
        return STATUS_INVALID_PARAMETER;

    Length = Directory->Length / sizeof(WCHAR);
    Entry = RtlAllocateHeap(RtlGetProcessHeap(), 0,
                            FIELD_OFFSET(LDRP_DLL_DIRECTORY, Path) + (Length + 1) * sizeof(WCHAR));
    if (!Entry)
        return STATUS_NO_MEMORY;

    Dos = Entry->Path;
    RtlCopyMemory(Dos, Directory->Buffer, Length * sizeof(WCHAR));
    Dos[Length] = UNICODE_NULL;

    if (!LdrpIsAbsoluteDosPath(Dos))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Entry);
        return STATUS_INVALID_PARAMETER;
    }

    Status = RtlDosPathNameToNtPathName_U_WithStatus(Dos, &NtPath, NULL, NULL);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Entry);
        return Status;
    }

    /* The directory has to exist */
    InitializeObjectAttributes(&ObjectAttributes, &NtPath, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtQueryAttributesFile(&ObjectAttributes, &Information);
    RtlFreeUnicodeString(&NtPath);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Entry);
        return Status;
    }

    RtlAcquireSRWLockExclusive(&LdrpDllDirectoryLock);
    InsertHeadList(&LdrpDllDirectories, &Entry->Entry);
    RtlReleaseSRWLockExclusive(&LdrpDllDirectoryLock);

    *Cookie = Entry;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
LdrRemoveDllDirectory(
    _In_ PVOID Cookie)
{
    PLIST_ENTRY ListEntry;
    NTSTATUS Status = STATUS_INVALID_PARAMETER;

    RtlAcquireSRWLockExclusive(&LdrpDllDirectoryLock);
    for (ListEntry = LdrpDllDirectories.Flink; ListEntry != &LdrpDllDirectories; ListEntry = ListEntry->Flink)
    {
        /* Only a cookie this loader handed out is removed */
        if ((PVOID)ListEntry == Cookie)
        {
            RemoveEntryList(ListEntry);
            RtlFreeHeap(RtlGetProcessHeap(), 0, CONTAINING_RECORD(ListEntry, LDRP_DLL_DIRECTORY, Entry));
            Status = STATUS_SUCCESS;
            break;
        }
    }
    RtlReleaseSRWLockExclusive(&LdrpDllDirectoryLock);

    return Status;
}

NTSTATUS
NTAPI
LdrSetDefaultDllDirectories(
    _In_ ULONG DirectoryFlags)
{
    if (!DirectoryFlags || (DirectoryFlags & ~LDRP_SEARCH_VALID_DEFAULTS))
        return STATUS_INVALID_PARAMETER;

    RtlAcquireSRWLockExclusive(&LdrpDllDirectoryLock);
    LdrpDefaultDllDirectoryFlags = DirectoryFlags;
    RtlReleaseSRWLockExclusive(&LdrpDllDirectoryLock);
    return STATUS_SUCCESS;
}

/*
 * The search path for a load that was given none, once the process chose its
 * default directories: the application directory, the user directories, then
 * system32, in that order. NULL means "no choice made" (or no memory): the
 * caller keeps the legacy behaviour. The caller frees the string with
 * RtlFreeHeap on the process heap.
 */
PWSTR
NTAPI
LdrpGetDefaultDllPath(VOID)
{
    PLIST_ENTRY ListEntry;
    PUNICODE_STRING Image;
    PWSTR Path, Current;
    ULONG Flags;
    SIZE_T Length = 1, ImageDirLength = 0, SystemLength = 0;
    static const WCHAR System32[] = L"\\system32";

    RtlAcquireSRWLockShared(&LdrpDllDirectoryLock);
    Flags = LdrpDefaultDllDirectoryFlags;
    if (!Flags)
    {
        RtlReleaseSRWLockShared(&LdrpDllDirectoryLock);
        return NULL;
    }

    if (Flags & LDRP_SEARCH_DEFAULT_DIRS)
        Flags |= LDRP_SEARCH_APPLICATION_DIR | LDRP_SEARCH_USER_DIRS | LDRP_SEARCH_SYSTEM32;

    Image = &NtCurrentPeb()->ProcessParameters->ImagePathName;
    if ((Flags & LDRP_SEARCH_APPLICATION_DIR) && Image->Buffer)
    {
        SIZE_T i = Image->Length / sizeof(WCHAR);

        while (i && Image->Buffer[i - 1] != L'\\')
            i--;
        if (i > 1)
            ImageDirLength = i - 1;
        Length += ImageDirLength + 1;
    }

    if (Flags & LDRP_SEARCH_USER_DIRS)
    {
        for (ListEntry = LdrpDllDirectories.Flink; ListEntry != &LdrpDllDirectories; ListEntry = ListEntry->Flink)
            Length += wcslen(CONTAINING_RECORD(ListEntry, LDRP_DLL_DIRECTORY, Entry)->Path) + 1;
    }

    if (Flags & LDRP_SEARCH_SYSTEM32)
    {
        SystemLength = wcslen(SharedUserData->NtSystemRoot) + RTL_NUMBER_OF(System32) - 1;
        Length += SystemLength + 1;
    }

    Path = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length * sizeof(WCHAR));
    if (!Path)
    {
        RtlReleaseSRWLockShared(&LdrpDllDirectoryLock);
        return NULL;
    }

    Current = Path;
    if (ImageDirLength)
    {
        RtlCopyMemory(Current, Image->Buffer, ImageDirLength * sizeof(WCHAR));
        Current += ImageDirLength;
        *Current++ = L';';
    }

    if (Flags & LDRP_SEARCH_USER_DIRS)
    {
        for (ListEntry = LdrpDllDirectories.Flink; ListEntry != &LdrpDllDirectories; ListEntry = ListEntry->Flink)
        {
            PCWSTR Directory = CONTAINING_RECORD(ListEntry, LDRP_DLL_DIRECTORY, Entry)->Path;
            SIZE_T DirectoryLength = wcslen(Directory);

            RtlCopyMemory(Current, Directory, DirectoryLength * sizeof(WCHAR));
            Current += DirectoryLength;
            *Current++ = L';';
        }
    }

    if (SystemLength)
    {
        SIZE_T RootLength = wcslen(SharedUserData->NtSystemRoot);

        RtlCopyMemory(Current, SharedUserData->NtSystemRoot, RootLength * sizeof(WCHAR));
        Current += RootLength;
        RtlCopyMemory(Current, System32, (RTL_NUMBER_OF(System32) - 1) * sizeof(WCHAR));
        Current += RTL_NUMBER_OF(System32) - 1;
        *Current++ = L';';
    }

    /* Drop the last separator */
    if (Current != Path)
        Current--;
    *Current = UNICODE_NULL;

    RtlReleaseSRWLockShared(&LdrpDllDirectoryLock);
    return Path;
}
