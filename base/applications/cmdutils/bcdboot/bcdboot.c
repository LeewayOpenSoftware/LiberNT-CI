/*
 * PROJECT:     LiberNT Boot Configuration Data boot file creation tool
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Creates the boot files of an installed system on a system partition
 * COPYRIGHT:   Copyright 2026 Ahmed Arif
 */

#include <stdio.h>
#include <stdlib.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <strsafe.h>

#define NTOS_MODE_USER
#include <ndk/iofuncs.h>
#include <ndk/rtlfuncs.h>
#include <ntstatus.h>

#include <setuplib.h>

/* Win11 switches that this tool does not provide; refusing them beats doing something else */
static const PCWSTR UnsupportedSwitches[] =
{
    L"m", L"d", L"c", L"t", L"vbcd", L"bcdclean", L"imageroot", L"bootex", L"offline"
};

static VOID
PrintUsage(VOID)
{
    fputws(L"BCDBOOT boot file creation and repair tool.\n\n"
           L"The bcdboot.exe command-line tool is used to copy critical boot files to the\n"
           L"system partition and to create a new system BCD store.\n\n"
           L"bcdboot <source> [/l <locale>] [/s <volume-letter> [/f <firmware>]] [/v]\n"
           L"                 [/addlast] [/p] [/nofirmwaresync] [/description <description>]\n\n"
           L"  source     Specifies the location of the system root directory.\n"
           L"  /l         Specifies the locale used to initialize the BCD store. The default\n"
           L"             is US English.\n"
           L"  /s         Specifies the volume letter of the target system partition. The\n"
           L"             default is the system partition identified by the firmware.\n"
           L"  /f         Used with /s, specifies the firmware type of the target system\n"
           L"             partition: \"UEFI\". (\"BIOS\" and \"ALL\" are not supported.)\n"
           L"  /v         Enables verbose mode.\n"
           L"  /addlast   Adds the boot manager firmware entry last. The default is first.\n"
           L"  /p         Keeps the position of an existing boot manager firmware entry.\n"
           L"  /nofirmwaresync  Leaves the firmware boot entries unchanged.\n"
           L"  /description     Sets the description of the new boot entry.\n\n"
           L"Examples: bcdboot c:\\windows /l en-us\n"
           L"          bcdboot c:\\windows /s h: /f UEFI\n"
           L"          bcdboot c:\\windows /addlast\n", stdout);
}

/* "\\?\Volume{...}\" -> "\??\Volume{...}" (+ the trailing separator when asked) */
static BOOL
GetNtVolumeName(
    _In_ PCWSTR MountPoint,
    _In_ BOOL TrailingSeparator,
    _Out_writes_(Chars) PWSTR NtName,
    _In_ SIZE_T Chars)
{
    WCHAR Volume[MAX_PATH];
    SIZE_T Length;

    if (!GetVolumeNameForVolumeMountPointW(MountPoint, Volume, ARRAYSIZE(Volume)))
        return FALSE;

    Length = wcslen(Volume);
    if (Length < 5 || wcsncmp(Volume, L"\\\\?\\", 4) != 0)
        return FALSE;
    if (!TrailingSeparator && Volume[Length - 1] == L'\\')
        Volume[Length - 1] = UNICODE_NULL;

    return SUCCEEDED(StringCchPrintfW(NtName, Chars, L"\\??\\%s", Volume + 4));
}

/* The system partition the firmware booted from, as setup and the loader record it */
static BOOL
GetFirmwareSystemPartition(
    _Out_writes_(Chars) PWSTR NtName,
    _In_ SIZE_T Chars)
{
    WCHAR Value[MAX_PATH];
    DWORD Size = sizeof(Value) - sizeof(WCHAR), Type;
    HKEY Key;
    LONG Error;

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", 0, KEY_QUERY_VALUE, &Key);
    if (Error != ERROR_SUCCESS)
        return FALSE;

    Error = RegQueryValueExW(Key, L"SystemPartition", NULL, &Type, (LPBYTE)Value, &Size);
    RegCloseKey(Key);
    if (Error != ERROR_SUCCESS || Type != REG_SZ || Size < sizeof(WCHAR))
        return FALSE;

    Value[Size / sizeof(WCHAR)] = UNICODE_NULL;
    return SUCCEEDED(StringCchPrintfW(NtName, Chars, L"%s\\", Value));
}

static int
Fail(
    _In_ PCWSTR Message,
    _In_ DWORD Error)
{
    fwprintf(stderr, L"BCDBOOT: %s\n", Message);
    return Error;
}

int
wmain(
    _In_ int argc,
    _In_reads_(argc) WCHAR *argv[])
{
    PCWSTR Source = NULL, Locale = NULL, Target = NULL, Firmware = NULL, Description = NULL;
    WCHAR FullSource[MAX_PATH], Root[MAX_PATH], Check[MAX_PATH];
    WCHAR SystemPartition[MAX_PATH], SystemVolume[MAX_PATH], Letter[4];
    UNICODE_STRING SystemPartitionString, SystemVolumeString;
    ULONG_PTR Options = 0;
    BOOLEAN Verbose = FALSE, FallbackKept = FALSE;
    PCWSTR Directory, Switch;
    NTSTATUS Status;
    SIZE_T Length;
    int i, j;

    if (argc == 2 && (_wcsicmp(argv[1], L"/?") == 0 || _wcsicmp(argv[1], L"-?") == 0 ||
                      _wcsicmp(argv[1], L"/help") == 0 || _wcsicmp(argv[1], L"--help") == 0))
    {
        PrintUsage();
        return ERROR_SUCCESS;
    }

    for (i = 1; i < argc; i++)
    {
        if (argv[i][0] != L'/' && argv[i][0] != L'-')
        {
            if (Source)
                return Fail(L"Only one source can be given.", ERROR_INVALID_PARAMETER);
            Source = argv[i];
            continue;
        }

        Switch = argv[i] + 1;
        if (_wcsicmp(Switch, L"l") == 0 && i + 1 < argc)
            Locale = argv[++i];
        else if (_wcsicmp(Switch, L"s") == 0 && i + 1 < argc)
            Target = argv[++i];
        else if (_wcsicmp(Switch, L"f") == 0 && i + 1 < argc)
            Firmware = argv[++i];
        else if (_wcsicmp(Switch, L"description") == 0 && i + 1 < argc)
            Description = argv[++i];
        else if (_wcsicmp(Switch, L"v") == 0)
            Verbose = TRUE;
        else if (_wcsicmp(Switch, L"addlast") == 0)
            Options |= BOOT_OPTION_ADD_LAST;
        else if (_wcsicmp(Switch, L"p") == 0)
            Options |= BOOT_OPTION_KEEP_ORDER;
        else if (_wcsicmp(Switch, L"nofirmwaresync") == 0)
            Options |= BOOT_OPTION_NO_FIRMWARE_ENTRY;
        else
        {
            for (j = 0; j < ARRAYSIZE(UnsupportedSwitches); j++)
            {
                if (_wcsicmp(Switch, UnsupportedSwitches[j]) == 0)
                {
                    fwprintf(stderr, L"BCDBOOT: /%s is not supported.\n", UnsupportedSwitches[j]);
                    return ERROR_NOT_SUPPORTED;
                }
            }
            PrintUsage();
            return ERROR_INVALID_PARAMETER;
        }
    }

    if (!Source)
    {
        PrintUsage();
        return ERROR_INVALID_PARAMETER;
    }
    if (Firmware && !Target)
    {
        PrintUsage();
        return ERROR_INVALID_PARAMETER;
    }
    if ((Options & BOOT_OPTION_ADD_LAST) && (Options & BOOT_OPTION_KEEP_ORDER) && !Target)
        return Fail(L"/addlast and /p cannot be used together.", ERROR_INVALID_PARAMETER);
    if (Target && (Options & BOOT_OPTION_ADD_LAST) && (Options & BOOT_OPTION_KEEP_ORDER))
    {
        fputws(L"BFSVC Error: AddBootmgrLast and PreserveBootOrder options are ignored when custom volume is specified\n", stdout);
        fputws(L"BFSVC Error: AddBootmgrLast and PreserveBootOrder options are ignored when custom volume is specified\n", stdout);
        Options &= ~(BOOT_OPTION_ADD_LAST | BOOT_OPTION_KEEP_ORDER);
    }
    if (Target)
        Options |= BOOT_OPTION_NO_FIRMWARE_ENTRY;
    if (Firmware && _wcsicmp(Firmware, L"UEFI") != 0)
    {
        if (_wcsicmp(Firmware, L"BIOS") == 0 || _wcsicmp(Firmware, L"ALL") == 0)
            return Fail(L"BIOS boot files are not supported; use /f UEFI.", ERROR_NOT_SUPPORTED);
        return Fail(L"The firmware type must be UEFI, BIOS or ALL.", ERROR_INVALID_PARAMETER);
    }
    if (!Firmware && !IsUefiFirmware())
        return Fail(L"BIOS boot files are not supported; use /s <volume-letter> /f UEFI.", ERROR_NOT_SUPPORTED);

    /* The source: a directory on a volume, e.g. C:\Windows -> volume of C:\, directory \Windows */
    Length = GetFullPathNameW(Source, ARRAYSIZE(FullSource), FullSource, NULL);
    if (Length == 0 || Length >= ARRAYSIZE(FullSource))
        return Fail(L"The source path is not valid.", ERROR_INVALID_PARAMETER);
    while (Length > 3 && FullSource[Length - 1] == L'\\')
        FullSource[--Length] = UNICODE_NULL;
    if (!GetVolumePathNameW(FullSource, Root, ARRAYSIZE(Root)) ||
        wcslen(Root) >= Length ||
        !GetNtVolumeName(Root, FALSE, SystemVolume, ARRAYSIZE(SystemVolume)))
    {
        return Fail(L"The source must be a system root directory, such as C:\\Windows.", ERROR_INVALID_PARAMETER);
    }
    Directory = FullSource + wcslen(Root) - 1;

    if (FAILED(StringCchPrintfW(Check, ARRAYSIZE(Check), L"%s\\System32", FullSource)) ||
        !(GetFileAttributesW(Check) != INVALID_FILE_ATTRIBUTES &&
          (GetFileAttributesW(Check) & FILE_ATTRIBUTE_DIRECTORY)))
    {
        return Fail(L"The source is not a system root directory (no System32).", ERROR_PATH_NOT_FOUND);
    }

    /* The system partition: the one given, or the one the firmware booted from */
    if (Target)
    {
        if (!iswalpha(Target[0]) || (Target[1] != UNICODE_NULL && (Target[1] != L':' || Target[2] != UNICODE_NULL)))
            return Fail(L"/s takes a volume letter, such as S:.", ERROR_INVALID_PARAMETER);
        StringCchPrintfW(Letter, ARRAYSIZE(Letter), L"%c:\\", Target[0]);
        if (!GetNtVolumeName(Letter, TRUE, SystemPartition, ARRAYSIZE(SystemPartition)))
            return Fail(L"The system partition volume was not found.", ERROR_PATH_NOT_FOUND);
    }
    else if (!GetFirmwareSystemPartition(SystemPartition, ARRAYSIZE(SystemPartition)))
    {
        return Fail(L"The firmware's system partition is not known; give it with /s.", ERROR_PATH_NOT_FOUND);
    }

    if (Verbose)
    {
        wprintf(L"BCDBOOT: source %s (volume %s, directory %s)\n", FullSource, SystemVolume, Directory);
        wprintf(L"BCDBOOT: system partition %s%s\n", SystemPartition,
                (Options & BOOT_OPTION_NO_FIRMWARE_ENTRY) ? L", firmware entries unchanged" : L"");
    }

    RtlInitUnicodeString(&SystemPartitionString, SystemPartition);
    RtlInitUnicodeString(&SystemVolumeString, SystemVolume);
    Status = InstallSystemBootFiles(&SystemPartitionString, &SystemVolumeString, Directory,
                                    Locale, Description, Options, &FallbackKept);
    if (!NT_SUCCESS(Status))
    {
        if (Verbose)
            fwprintf(stderr, L"BCDBOOT: status 0x%08lx\n", Status);
        if (Status == STATUS_UNRECOGNIZED_VOLUME)
            fputws(L"BCDBOOT: The system partition must use the FAT32 file system.\n", stderr);
        fputws(L"Failure when attempting to copy boot files.\n", stderr);
        /* \??\X -> \\?\X; \Device\... -> \\?\GLOBALROOT\Device\... */
        if (SUCCEEDED(StringCchPrintfW(Check, ARRAYSIZE(Check),
                                       wcsncmp(SystemPartition, L"\\??\\", 4) == 0 ? L"\\\\?\\%sEFI\\Orosi\\Boot\\BCD.bak"
                                                                                  : L"\\\\?\\GLOBALROOT%sEFI\\Orosi\\Boot\\BCD.bak",
                                       wcsncmp(SystemPartition, L"\\??\\", 4) == 0 ? SystemPartition + 4 : SystemPartition)) &&
            GetFileAttributesW(Check) != INVALID_FILE_ATTRIBUTES)
        {
            fputws(L"A copy of the earlier boot configuration is \\EFI\\Orosi\\Boot\\BCD.bak on the system partition.\n",
                   stderr);
        }
        return RtlNtStatusToDosError(Status);
    }

    if (FallbackKept)
        fputws(L"The removable-media fallback loader belongs to another system and was kept.\n", stdout);
    fputws(L"Boot files successfully created.\n", stdout);
    return ERROR_SUCCESS;
}
