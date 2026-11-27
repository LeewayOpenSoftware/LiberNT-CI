/*
 * PROJECT:     LiberNT Boot Configuration Data editor
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     bcdedit.exe - lists and edits the boot configuration data store.
 *              The store is the system BCD hive (HKLM\BCD00000000) by default, or a hive
 *              file opened with /store. Object and element identifiers and the element/device
 *              layout come from the boot loader's own definitions (reactos/bcddef.h and
 *              bootloader/include/bcd.h); the registry layout is Objects\{id}\Description\Type
 *              and Objects\{id}\Elements\<type>\Element.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <strsafe.h>

#include <reactos/bcddef.h>
#include "resource.h"

#define BCD_SYSTEM_STORE    L"BCD00000000"
#define BCD_FIELD_WIDTH     24
#define BCD_MAX_ELEMENTS    256

/* The fixed, store-independent object identifiers, so /enum shows {bootmgr} and friends. */
static const struct { PCWSTR Guid; PCWSTR Alias; } g_WellKnownIds[] =
{
    { L"{9dea862c-5cdd-4e70-acc1-f32b344d4795}", L"{bootmgr}" },
    { L"{a5a30fa2-3d06-4e9f-b5f4-a01df9d1fcba}", L"{fwbootmgr}" },
    { L"{b2721d73-1db4-4c62-bf78-c548a880142d}", L"{memdiag}" },
    { L"{466f5a88-0af2-4f76-9038-095b170dc21c}", L"{ntldr}" },
    { L"{ae5534e0-a924-466c-b836-758539a3ee3a}", L"{ramdiskoptions}" },
    { L"{4636856e-540f-4170-a130-a84776f4c654}", L"{dbgsettings}" },
    { L"{0ce4991b-e6b3-4b16-b23c-5e0d9250e5d9}", L"{emssettings}" },
    { L"{5189b25c-5558-4bf2-bca4-289b11bd29e2}", L"{badmemory}" },
    { L"{6efb52bf-1766-41db-a6b3-0ee5eff72bd7}", L"{bootloadersettings}" },
    { L"{7ff607e0-4395-11db-b0de-0800200c9a66}", L"{hypervisorsettings}" },
    { L"{1afa9c49-16ab-4a5c-901b-212802da9460}", L"{resumeloadersettings}" },
    { L"{7ea2e1ac-2e61-4728-aaa3-896d9d0a9f0e}", L"{globalsettings}" },
};

/* The canonical names bcdedit prints for each element; they are not localised, matching
 * Windows, so a capture compares byte for byte. Unknown elements print as custom:<type>. */
static const struct { ULONG Type; PCWSTR Name; } g_ElementNames[] =
{
    { 0x11000001, L"device" },
    { 0x12000002, L"path" },
    { 0x12000004, L"description" },
    { 0x12000005, L"locale" },
    { 0x14000006, L"inherit" },
    { 0x16000010, L"bootdebug" },
    { 0x12000030, L"loadoptions" },
    { 0x21000001, L"osdevice" },
    { 0x22000002, L"systemroot" },
    { 0x23000003, L"default" },
    { 0x23000006, L"resumeobject" },
    { 0x24000001, L"displayorder" },
    { 0x24000010, L"toolsdisplayorder" },
    { 0x25000004, L"timeout" },
    { 0x25000020, L"nx" },
    { 0x25000061, L"numproc" },
    { 0x25000080, L"safeboot" },
    { 0x26000010, L"detecthal" },
    { 0x260000a0, L"debug" },
    { 0x22000011, L"kernel" },
    { 0x22000012, L"hal" },
    { 0x26000022, L"winpe" },
    { 0x26000020, L"displaybootmenu" },
    { 0x26000040, L"vga" },
    { 0x26000041, L"quietboot" },
    { 0x26000091, L"sos" },
    { 0x26000090, L"bootlog" },
};

/* Object Type value -> heading resource id. */
static const struct { ULONG Type; UINT Heading; } g_ObjectHeadings[] =
{
    { 0x10100001, IDS_HDR_FWBOOTMGR },
    { 0x10100002, IDS_HDR_BOOTMGR },
    { 0x10200005, IDS_HDR_MEMDIAG },
    { 0x10200004, IDS_HDR_RESUME },
    { 0x10200003, IDS_HDR_BOOTLOADER },
    { 0x10300006, IDS_HDR_NTLDR },
    { 0x10400008, IDS_HDR_BOOTSECTOR },
    { 0x10200006, IDS_HDR_STARTUP },
    { 0x20100000, IDS_HDR_GLOBALSETTINGS },
    { 0x20200003, IDS_HDR_BOOTLOADERSET },
    { 0x20200004, IDS_HDR_RESUMESET },
    { 0x20100001, IDS_HDR_DBGSETTINGS },
    { 0x20100002, IDS_HDR_EMSSETTINGS },
    { 0x20100003, IDS_HDR_BADMEMORY },
    { 0x30000000, IDS_HDR_DEVOPTIONS },
};

static
VOID
PrintResource(
    _In_ UINT Id)
{
    WCHAR Buffer[1024];

    if (LoadStringW(GetModuleHandleW(NULL), Id, Buffer, ARRAYSIZE(Buffer)) > 0)
        fputws(Buffer, stdout);
}

static
PCWSTR
AliasForId(
    _In_ PCWSTR Guid)
{
    SIZE_T i;

    for (i = 0; i < ARRAYSIZE(g_WellKnownIds); i++)
    {
        if (_wcsicmp(Guid, g_WellKnownIds[i].Guid) == 0)
            return g_WellKnownIds[i].Alias;
    }
    return Guid;
}

static
PCWSTR
NameForElement(
    _In_ ULONG Type,
    _Out_writes_z_(Chars) PWSTR Custom,
    _In_ SIZE_T Chars)
{
    SIZE_T i;

    for (i = 0; i < ARRAYSIZE(g_ElementNames); i++)
    {
        if (g_ElementNames[i].Type == Type)
            return g_ElementNames[i].Name;
    }
    StringCchPrintfW(Custom, Chars, L"custom:%08lx", Type);
    return Custom;
}

static
UINT
HeadingForType(
    _In_ ULONG Type)
{
    SIZE_T i;

    for (i = 0; i < ARRAYSIZE(g_ObjectHeadings); i++)
    {
        if (g_ObjectHeadings[i].Type == Type)
            return g_ObjectHeadings[i].Heading;
    }
    return IDS_HDR_UNKNOWN;
}

static
ULONG
LeDword(
    _In_reads_bytes_(4) const BYTE *Data)
{
    return (ULONG)Data[0] | ((ULONG)Data[1] << 8) | ((ULONG)Data[2] << 16) | ((ULONG)Data[3] << 24);
}

/* Print "name" padded to the field column; the caller adds the value. */
static
VOID
PrintFieldName(
    _In_ PCWSTR Name)
{
    int Pad = BCD_FIELD_WIDTH - (int)wcslen(Name);

    fputws(Name, stdout);
    while (Pad-- > 0)
        fputwc(L' ', stdout);
}

/* The 16-byte GUID as it is stored (Data1/2/3 little-endian, Data4 as bytes). */
static
VOID
PrintGuidBytes(
    _In_reads_bytes_(16) const BYTE *Guid)
{
    wprintf(L"{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
            LeDword(Guid),
            (unsigned)(Guid[4] | (Guid[5] << 8)),
            (unsigned)(Guid[6] | (Guid[7] << 8)),
            Guid[8], Guid[9], Guid[10], Guid[11], Guid[12], Guid[13], Guid[14], Guid[15]);
}

/* A REG_MULTI_SZ of object ids, one per line, aligned under the value column. */
static
VOID
PrintGuidList(
    _In_ PCWSTR MultiSz)
{
    PCWSTR Id = MultiSz;
    BOOL First = TRUE;

    while (*Id)
    {
        if (!First)
        {
            fputwc(L'\n', stdout);
            PrintFieldName(L"");
        }
        fputws(AliasForId(Id), stdout);
        First = FALSE;
        Id += wcslen(Id) + 1;
    }
    fputwc(L'\n', stdout);
}

static
VOID
PrintDevice(
    _In_reads_bytes_(Length) const BYTE *Data,
    _In_ ULONG Length)
{
    const BYTE *Dev;
    ULONG Type;

    /* Need the device type and the disk sub-type at least. */
    if (Length < BCD_ELEMENT_DEVICE_OFFSET + BCD_DISK_TYPE_OFFSET + 4)
    {
        fputws(L"unknown\n", stdout);
        return;
    }
    Dev = Data + BCD_ELEMENT_DEVICE_OFFSET;
    Type = LeDword(Dev);

    switch (Type)
    {
        case BCD_DEVICE_BOOT:
            fputws(L"boot\n", stdout);
            return;

        case BCD_DEVICE_DISK:
            if (LeDword(Dev + BCD_DISK_TYPE_OFFSET) == BCD_DISK_RAMDISK)
                fputws(L"ramdisk\n", stdout);
            else
                fputws(L"disk\n", stdout);
            return;

        case BCD_DEVICE_PARTITION:
            if (Length < BCD_ELEMENT_DEVICE_OFFSET + BCD_DEVICE_DISK_OFFSET + 16)
            {
                fputws(L"partition\n", stdout);
                return;
            }
            if (LeDword(Dev + BCD_DEVICE_STYLE_OFFSET) == BCD_PARTITION_MBR)
            {
                wprintf(L"partition=mbr,signature=%08lx,offset=%08lx%08lx\n",
                        LeDword(Dev + BCD_DEVICE_SIGNATURE_OFFSET),
                        LeDword(Dev + BCD_DEVICE_PARTITION_OFFSET + 4),
                        LeDword(Dev + BCD_DEVICE_PARTITION_OFFSET));
            }
            else
            {
                fputws(L"partition=gpt,disk=", stdout);
                PrintGuidBytes(Dev + BCD_DEVICE_DISK_OFFSET);
                fputws(L",partition=", stdout);
                PrintGuidBytes(Dev + BCD_DEVICE_PARTITION_OFFSET);
                fputwc(L'\n', stdout);
            }
            return;

        default:
            wprintf(L"device type %lu\n", Type);
            return;
    }
}

/*
 * Read Elements\<type>\Element and print it under its field name. The value may be untrusted
 * (a /store file), unterminated, or larger than a fixed buffer: size it, add room for a double
 * NUL, terminate it, and only walk a string/list when its registry type matches the format.
 */
static
BOOL
PrintElement(
    _In_ HKEY Elements,
    _In_ ULONG Type)
{
    WCHAR Sub[16], Custom[32];
    HKEY Key;
    BYTE *Data;
    DWORD Length = 0, RegType = 0;
    ULONG Format = BCD_ELEMENT_FORMAT(Type);
    LONG Error;

    StringCchPrintfW(Sub, ARRAYSIZE(Sub), L"%08lx", Type);
    if (RegOpenKeyExW(Elements, Sub, 0, KEY_READ, &Key) != ERROR_SUCCESS)
        return FALSE;

    Error = RegQueryValueExW(Key, L"Element", NULL, &RegType, NULL, &Length);
    if (Error != ERROR_SUCCESS || Length == 0)
    {
        RegCloseKey(Key);
        return FALSE;
    }

    Data = HeapAlloc(GetProcessHeap(), 0, Length + 2 * sizeof(WCHAR));
    if (Data == NULL)
    {
        RegCloseKey(Key);
        return FALSE;
    }

    Error = RegQueryValueExW(Key, L"Element", NULL, &RegType, Data, &Length);
    RegCloseKey(Key);
    if (Error != ERROR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, Data);
        return FALSE;
    }
    /* Guarantee a double-NUL terminator past the data for string and list walks. */
    Data[Length] = 0;
    Data[Length + 1] = 0;
    Data[Length + 2] = 0;
    Data[Length + 3] = 0;

    PrintFieldName(NameForElement(Type, Custom, ARRAYSIZE(Custom)));

    switch (Format)
    {
        case BCD_ELEMENT_FORMAT_STRING:
            if (RegType == REG_SZ)
                fputws((PCWSTR)Data, stdout);
            fputwc(L'\n', stdout);
            break;
        case BCD_ELEMENT_FORMAT_OBJECT:
            if (RegType == REG_SZ)
                fputws(AliasForId((PCWSTR)Data), stdout);
            fputwc(L'\n', stdout);
            break;
        case BCD_ELEMENT_FORMAT_OBJECTLIST:
            if (RegType == REG_MULTI_SZ)
                PrintGuidList((PCWSTR)Data);
            else
                fputwc(L'\n', stdout);
            break;
        case BCD_ELEMENT_FORMAT_BOOLEAN:
            fputws((Length >= 1 && Data[0]) ? L"Yes\n" : L"No\n", stdout);
            break;
        case BCD_ELEMENT_FORMAT_INTEGER:
        case BCD_ELEMENT_FORMAT_INTEGERLIST:
        {
            ULONGLONG Value = 0;
            DWORD i;
            for (i = 0; i < Length && i < 8; i++)
                Value |= (ULONGLONG)Data[i] << (8 * i);
            wprintf(L"%llu\n", Value);
            break;
        }
        case BCD_ELEMENT_FORMAT_DEVICE:
            PrintDevice(Data, Length);
            break;
        default:
            fputwc(L'\n', stdout);
            break;
    }

    HeapFree(GetProcessHeap(), 0, Data);
    return TRUE;
}

static
int
__cdecl
CompareUlong(
    _In_ const void *A,
    _In_ const void *B)
{
    ULONG Ua = *(const ULONG *)A, Ub = *(const ULONG *)B;

    if (Ua < Ub) return -1;
    if (Ua > Ub) return 1;
    return 0;
}

static
VOID
PrintObject(
    _In_ HKEY Objects,
    _In_ PCWSTR Id)
{
    HKEY Object, Description, Elements;
    WCHAR Heading[128];
    DWORD Type = 0, Length = sizeof(Type), RegType = 0, Index;
    ULONG Types[BCD_MAX_ELEMENTS], Count = 0;
    SIZE_T i;

    if (RegOpenKeyExW(Objects, Id, 0, KEY_READ, &Object) != ERROR_SUCCESS)
        return;

    if (RegOpenKeyExW(Object, L"Description", 0, KEY_READ, &Description) == ERROR_SUCCESS)
    {
        RegQueryValueExW(Description, L"Type", NULL, &RegType, (BYTE *)&Type, &Length);
        RegCloseKey(Description);
    }

    if (LoadStringW(GetModuleHandleW(NULL), HeadingForType(Type), Heading, ARRAYSIZE(Heading)) > 0)
    {
        int Dashes = (int)wcslen(Heading);
        fputws(Heading, stdout);
        fputwc(L'\n', stdout);
        while (Dashes-- > 0)
            fputwc(L'-', stdout);
        fputwc(L'\n', stdout);
    }

    PrintFieldName(L"identifier");
    fputws(AliasForId(Id), stdout);
    fputwc(L'\n', stdout);

    if (RegOpenKeyExW(Object, L"Elements", 0, KEY_READ, &Elements) == ERROR_SUCCESS)
    {
        /* Enumerate every element present, not only the named ones, so nothing is hidden. */
        for (Index = 0; Count < ARRAYSIZE(Types); Index++)
        {
            WCHAR Name[16];
            DWORD Chars = ARRAYSIZE(Name);

            if (RegEnumKeyExW(Elements, Index, Name, &Chars, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            Types[Count++] = wcstoul(Name, NULL, 16);
        }
        qsort(Types, Count, sizeof(ULONG), CompareUlong);
        for (i = 0; i < Count; i++)
            PrintElement(Elements, Types[i]);
        RegCloseKey(Elements);
    }

    fputwc(L'\n', stdout);
    RegCloseKey(Object);
}

static
LONG
OpenStore(
    _In_opt_ PCWSTR StoreFile,
    _In_ REGSAM Access,
    _Out_ PHKEY Store)
{
    if (StoreFile == NULL)
        return RegOpenKeyExW(HKEY_LOCAL_MACHINE, BCD_SYSTEM_STORE, 0, Access, Store);

    return RegLoadAppKeyW(StoreFile, Store, Access, 0, 0);
}

static
BOOL
EnumStore(
    _In_opt_ PCWSTR StoreFile)
{
    HKEY Store, Objects;
    DWORD Index;
    LONG Error;

    Error = OpenStore(StoreFile, KEY_READ, &Store);
    if (Error != ERROR_SUCCESS)
    {
        PrintResource(IDS_ERR_OPENSTORE);
        return FALSE;
    }

    Error = RegOpenKeyExW(Store, L"Objects", 0, KEY_READ, &Objects);
    if (Error != ERROR_SUCCESS)
    {
        RegCloseKey(Store);
        PrintResource(IDS_ERR_OPENSTORE);
        return FALSE;
    }

    for (Index = 0; ; Index++)
    {
        WCHAR Name[64];
        DWORD Chars = ARRAYSIZE(Name);

        Error = RegEnumKeyExW(Objects, Index, Name, &Chars, NULL, NULL, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            break;
        PrintObject(Objects, Name);
    }

    RegCloseKey(Objects);
    RegCloseKey(Store);
    return TRUE;
}

int
wmain(
    _In_ int argc,
    _In_reads_(argc) WCHAR *argv[])
{
    PCWSTR StoreFile = NULL;
    BOOL DoEnum = FALSE;
    int i;

    if (argc == 1)
        DoEnum = TRUE;   /* a bare "bcdedit" lists the store */

    for (i = 1; i < argc; i++)
    {
        if (_wcsicmp(argv[i], L"/?") == 0 || _wcsicmp(argv[i], L"-?") == 0 ||
            _wcsicmp(argv[i], L"/help") == 0)
        {
            PrintResource(IDS_USAGE);
            return 0;
        }
        else if (_wcsicmp(argv[i], L"/enum") == 0)
        {
            DoEnum = TRUE;
            /* The only filter understood so far is "all"; /enum lists every object either way.
             * A narrower filter (ACTIVE, a type, an id) is a later change, so reject the rest
             * rather than ignore it. */
            if (i + 1 < argc && argv[i + 1][0] != L'/')
            {
                if (_wcsicmp(argv[i + 1], L"all") != 0)
                {
                    PrintResource(IDS_ERR_BADPARAM);
                    return 1;
                }
                i++;
            }
        }
        else if (_wcsicmp(argv[i], L"/store") == 0)
        {
            if (i + 1 >= argc)
            {
                PrintResource(IDS_ERR_BADPARAM);
                return 1;
            }
            StoreFile = argv[++i];
        }
        else
        {
            PrintResource(IDS_ERR_BADPARAM);
            return 1;
        }
    }

    if (DoEnum)
        return EnumStore(StoreFile) ? 0 : 1;

    PrintResource(IDS_USAGE);
    return 0;
}
