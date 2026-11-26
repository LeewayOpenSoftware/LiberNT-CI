/*
 * PROJECT:     Orosi API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Win11/Orosi comparison test for NtQueryMultipleValueKey (native parity lote 02, row L2)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <winreg.h>

typedef LONG (WINAPI *FN_RegQueryMultipleValuesW)(HKEY, PVALENTW, DWORD, LPWSTR, LPDWORD);

static WCHAR KeyPath[128];
static const ULONG DwordData = 0x11223344;
static const UCHAR OddData[7] = { 1, 2, 3, 4, 5, 6, 7 };
static const WCHAR StringData[] = L"Hello";
static const UCHAR BinData[13] = { 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };
#define BIG_SIZE 70000

static UNICODE_STRING NameDw  = RTL_CONSTANT_STRING(L"Dw");
static UNICODE_STRING NameOdd = RTL_CONSTANT_STRING(L"Odd");
static UNICODE_STRING NameStr = RTL_CONSTANT_STRING(L"Str");
static UNICODE_STRING NameBin = RTL_CONSTANT_STRING(L"Bin");
static UNICODE_STRING NameBig = RTL_CONSTANT_STRING(L"Big");
static UNICODE_STRING NameNo  = RTL_CONSTANT_STRING(L"DoesNotExist");

static void TraceEntries(const char *When, const KEY_VALUE_ENTRY *Entries, ULONG Count, NTSTATUS Status, ULONG Length, ULONG Required);
static void ResetEntries(KEY_VALUE_ENTRY *Entries, ULONG Count);

static BOOLEAN UserHiveKey;

/* HKLM\SOFTWARE needs an administrator; the caller's own hive serves the standard-user run */
static NTSTATUS CreateTestKey(BOOLEAN UserHive, PHANDLE Key)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;
    HANDLE Root = NULL;
    NTSTATUS Status;

    UserHiveKey = UserHive;
    if (UserHive)
    {
        Status = RtlOpenCurrentUser(KEY_CREATE_SUB_KEY, &Root);
        if (!NT_SUCCESS(Status))
            return Status;
        StringCchPrintfW(KeyPath, _countof(KeyPath), L"Software\\Leeway-np02-QueryMultipleValueKey-%lu",
                         HandleToUlong(NtCurrentTeb()->ClientId.UniqueProcess));
    }
    else
    {
        StringCchPrintfW(KeyPath, _countof(KeyPath), L"\\Registry\\Machine\\SOFTWARE\\Leeway-np02-QueryMultipleValueKey-%lu",
                         HandleToUlong(NtCurrentTeb()->ClientId.UniqueProcess));
    }
    RtlInitUnicodeString(&Name, KeyPath);
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE, Root, NULL);
    Status = NtCreateKey(Key, KEY_ALL_ACCESS, &Oa, 0, NULL, REG_OPTION_VOLATILE, NULL);
    if (Root)
        NtClose(Root);
    return Status;
}

/* L2-S2: duplicate names, a zero-length value and a REG_SZ stored without its terminator */
static void TestOddShapes(HANDLE Key)
{
    UNICODE_STRING NameZero = RTL_CONSTANT_STRING(L"Zero"), NameNoTerm = RTL_CONSTANT_STRING(L"NoTerm");
    KEY_VALUE_ENTRY Entries[4] = { { &NameDw }, { &NameDw }, { &NameZero }, { &NameNoTerm } };
    UCHAR Buffer[256];
    ULONG Length = sizeof(Buffer), Required = 0;
    NTSTATUS Status;

    Status = NtSetValueKey(Key, &NameZero, 0, REG_BINARY, NULL, 0);
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtSetValueKey(Key, &NameNoTerm, 0, REG_SZ, (PVOID)L"ab", 4);
    ok_hex(Status, STATUS_SUCCESS);
    ResetEntries(Entries, 4);
    Status = NtQueryMultipleValueKey(Key, Entries, 4, Buffer, &Length, &Required);
    TraceEntries("duplicate Dw, zero-length, REG_SZ without terminator", Entries, 4, Status, Length, Required);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(Entries[0].DataLength == sizeof(DwordData) && Entries[1].DataLength == sizeof(DwordData), "duplicate entries differ\n");
        ok(Entries[2].DataLength == 0, "zero-length value reported %lu bytes\n", Entries[2].DataLength);
        ok(Entries[3].DataLength == 4, "REG_SZ without terminator reported %lu bytes\n", Entries[3].DataLength);
    }
    NtDeleteValueKey(Key, &NameZero);
    NtDeleteValueKey(Key, &NameNoTerm);
}

/* L2-M2: many entries sharing one name; the kernel must bound what it captures */
static void TestManyEntries(HANDLE Key)
{
    static const ULONG Counts[] = { 0x10000, 0x100000 };
    PKEY_VALUE_ENTRY Entries;
    UCHAR Buffer[64];
    ULONG Length, Required, i, c;
    NTSTATUS Status;

    for (c = 0; c < _countof(Counts); c++)
    {
        Entries = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Counts[c] * sizeof(KEY_VALUE_ENTRY));
        if (!Entries)
        {
            skip("no memory for %lu entries\n", Counts[c]);
            continue;
        }
        for (i = 0; i < Counts[c]; i++)
            Entries[i].ValueName = &NameDw;
        Length = sizeof(Buffer);
        Required = 0;
        Status = NtQueryMultipleValueKey(Key, Entries, Counts[c], Buffer, &Length, &Required);
        trace("PROBE %lu entries sharing one name, 64-byte buffer -> 0x%lx, length %lu, required %lu\n",
              Counts[c], Status, Length, Required);
        ok(Status != STATUS_SUCCESS, "%lu entries fit into 64 bytes\n", Counts[c]);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Entries);
    }
}

static void TraceEntries(const char *When, const KEY_VALUE_ENTRY *Entries, ULONG Count, NTSTATUS Status, ULONG Length, ULONG Required)
{
    ULONG i;

    trace("PROBE %s -> 0x%lx, length %lu, required %lu\n", When, Status, Length, Required);
    for (i = 0; i < Count; i++)
        trace("PROBE   entry %lu '%wZ': type %lu, length %lu, offset %lu\n", i, Entries[i].ValueName,
              Entries[i].Type, Entries[i].DataLength, Entries[i].DataOffset);
}

static void ResetEntries(KEY_VALUE_ENTRY *Entries, ULONG Count)
{
    ULONG i;

    for (i = 0; i < Count; i++)
    {
        Entries[i].DataLength = 0xEEEEEEEE;
        Entries[i].DataOffset = 0xEEEEEEEE;
        Entries[i].Type = 0xEEEEEEEE;
    }
}

static void TestThreeValues(HANDLE Key)
{
    KEY_VALUE_ENTRY Entries[3] = { { &NameDw }, { &NameOdd }, { &NameStr } };
    ULONG Length, Required, i;
    PUCHAR Buffer;
    NTSTATUS Status;

    /* Size query with no room at all */
    ResetEntries(Entries, 3);
    Length = 0;
    Required = 0xEEEEEEEE;
    Status = NtQueryMultipleValueKey(Key, Entries, 3, NULL, &Length, &Required);
    TraceEntries("three values, length 0", Entries, 3, Status, Length, Required);
    ok(!NT_SUCCESS(Status), "a zero-length buffer was accepted\n"); /* lead: STATUS_BUFFER_OVERFLOW */
    ok(Length >= sizeof(DwordData) + sizeof(OddData) + sizeof(StringData) && Length < 0x1000,
       "needed length %lu\n", Length);
    if (Length == 0 || Length >= 0x1000)
        return;

    /* Exact buffer */
    Buffer = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Length);
    if (!Buffer)
        return;
    ResetEntries(Entries, 3);
    Required = 0xEEEEEEEE;
    Status = NtQueryMultipleValueKey(Key, Entries, 3, Buffer, &Length, &Required);
    TraceEntries("three values, exact", Entries, 3, Status, Length, Required);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(Entries[0].Type == REG_DWORD && Entries[0].DataLength == sizeof(DwordData), "Dw: type %lu length %lu\n", Entries[0].Type, Entries[0].DataLength);
        ok(Entries[1].Type == REG_BINARY && Entries[1].DataLength == sizeof(OddData), "Odd: type %lu length %lu\n", Entries[1].Type, Entries[1].DataLength);
        ok(Entries[2].Type == REG_SZ && Entries[2].DataLength == sizeof(StringData), "Str: type %lu length %lu\n", Entries[2].Type, Entries[2].DataLength);
        for (i = 0; i < 3; i++)
            ok(Entries[i].DataOffset + Entries[i].DataLength <= Length, "entry %lu runs past the buffer\n", i);
        ok(memcmp(Buffer + Entries[0].DataOffset, &DwordData, sizeof(DwordData)) == 0, "Dw data differs\n");
        ok(memcmp(Buffer + Entries[1].DataOffset, OddData, sizeof(OddData)) == 0, "Odd data differs\n");
        ok(memcmp(Buffer + Entries[2].DataOffset, StringData, sizeof(StringData)) == 0, "Str data differs\n");
    }

    /* One byte short */
    ResetEntries(Entries, 3);
    Length--;
    Required = 0xEEEEEEEE;
    Status = NtQueryMultipleValueKey(Key, Entries, 3, Buffer, &Length, &Required);
    TraceEntries("three values, one byte short", Entries, 3, Status, Length, Required);
    ok(!NT_SUCCESS(Status), "a short buffer was accepted\n");

    /* Required length pointer omitted */
    Length = 0;
    Status = NtQueryMultipleValueKey(Key, Entries, 3, NULL, &Length, NULL);
    trace("PROBE three values, no required pointer -> 0x%lx, length %lu\n", Status, Length);

    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
}

static void TestBigValue(HANDLE Key)
{
    KEY_VALUE_ENTRY Entries[2] = { { &NameBig }, { &NameBin } };
    ULONG Length = 0, Required = 0, i;
    PUCHAR Buffer, Expected;
    NTSTATUS Status;

    Status = NtQueryMultipleValueKey(Key, Entries, 2, NULL, &Length, &Required);
    TraceEntries("big + bin, length 0", Entries, 2, Status, Length, Required);
    if (Length < BIG_SIZE + sizeof(BinData) || Length > BIG_SIZE + 64)
    {
        ok(0, "needed length %lu for a %lu-byte value plus %lu bytes\n", Length, (ULONG)BIG_SIZE, (ULONG)sizeof(BinData));
        return;
    }
    Buffer = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Length);
    Expected = RtlAllocateHeap(RtlGetProcessHeap(), 0, BIG_SIZE);
    if (!Buffer || !Expected)
        return;
    for (i = 0; i < BIG_SIZE; i++)
        Expected[i] = (UCHAR)(i * 7 + 3);
    ResetEntries(Entries, 2);
    Status = NtQueryMultipleValueKey(Key, Entries, 2, Buffer, &Length, &Required);
    TraceEntries("big + bin, exact", Entries, 2, Status, Length, Required);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(Entries[0].Type == REG_BINARY && Entries[0].DataLength == BIG_SIZE, "Big: type %lu length %lu\n", Entries[0].Type, Entries[0].DataLength);
        ok(Entries[0].DataOffset + BIG_SIZE <= Length && memcmp(Buffer + Entries[0].DataOffset, Expected, BIG_SIZE) == 0, "Big data differs\n");
        ok(Entries[1].DataLength == sizeof(BinData) && memcmp(Buffer + Entries[1].DataOffset, BinData, sizeof(BinData)) == 0, "Bin data differs\n");
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Expected);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
}

static void TestInvalid(HANDLE Key)
{
    KEY_VALUE_ENTRY Entries[2] = { { &NameDw }, { &NameNo } };
    KEY_VALUE_ENTRY One = { &NameDw };
    UCHAR Raw[sizeof(KEY_VALUE_ENTRY) + 8];
    UCHAR Buffer[64];
    ULONG Length, Required;
    HANDLE Event = NULL, Notify = NULL;
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;
    NTSTATUS Status;

    /* A name that does not exist fails the whole call */
    Length = sizeof(Buffer);
    Status = NtQueryMultipleValueKey(Key, Entries, 2, Buffer, &Length, &Required);
    TraceEntries("missing name", Entries, 2, Status, Length, Required);
    ok(!NT_SUCCESS(Status), "a missing value name was accepted\n"); /* lead: STATUS_OBJECT_NAME_NOT_FOUND */

    /* Counts */
    Length = sizeof(Buffer);
    Status = NtQueryMultipleValueKey(Key, &One, 0, Buffer, &Length, &Required);
    trace("PROBE count 0 -> 0x%lx, length %lu\n", Status, Length);
    Length = sizeof(Buffer);
    Status = NtQueryMultipleValueKey(Key, &One, 0x10000001, Buffer, &Length, &Required);
    trace("PROBE count 0x10000001 -> 0x%lx\n", Status);
    ok(!NT_SUCCESS(Status), "an overflowing entry count was accepted\n");

    /* Pointers */
    Length = sizeof(Buffer);
    Status = NtQueryMultipleValueKey(Key, NULL, 1, Buffer, &Length, &Required);
    trace("PROBE NULL entries -> 0x%lx\n", Status);
    ok(!NT_SUCCESS(Status), "NULL entries were accepted\n");
    Status = NtQueryMultipleValueKey(Key, &One, 1, Buffer, NULL, &Required);
    trace("PROBE NULL length pointer -> 0x%lx\n", Status);
    ok(!NT_SUCCESS(Status), "a NULL length pointer was accepted\n");
    Length = sizeof(Buffer);
    Status = NtQueryMultipleValueKey(Key, &One, 1, NULL, &Length, &Required);
    trace("PROBE NULL buffer with length %lu -> 0x%lx\n", (ULONG)sizeof(Buffer), Status);
    One.ValueName = (PUNICODE_STRING)(ULONG_PTR)8;
    Length = sizeof(Buffer);
    Status = NtQueryMultipleValueKey(Key, &One, 1, Buffer, &Length, &Required);
    trace("PROBE bad name pointer -> 0x%lx\n", Status);
    ok(!NT_SUCCESS(Status), "a bad name pointer was accepted\n");
    One.ValueName = &NameDw;
    memcpy(Raw + 1, &One, sizeof(One));
    Length = sizeof(Buffer);
    Status = NtQueryMultipleValueKey(Key, (PKEY_VALUE_ENTRY)(Raw + 1), 1, Buffer, &Length, &Required);
    trace("PROBE misaligned entries pointer -> 0x%lx\n", Status);

    /* Wrong object, missing access */
    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    ok_hex(Status, STATUS_SUCCESS);
    Length = sizeof(Buffer);
    Status = NtQueryMultipleValueKey(Event, &One, 1, Buffer, &Length, &Required);
    trace("PROBE event handle -> 0x%lx\n", Status);
    ok(!NT_SUCCESS(Status), "an event handle was accepted\n"); /* lead: STATUS_OBJECT_TYPE_MISMATCH */
    NtClose(Event);

    RtlInitUnicodeString(&Name, KeyPath);
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenKey(&Notify, KEY_NOTIFY, &Oa);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Length = sizeof(Buffer);
        Status = NtQueryMultipleValueKey(Notify, &One, 1, Buffer, &Length, &Required);
        trace("PROBE KEY_NOTIFY-only handle -> 0x%lx\n", Status);
        ok(!NT_SUCCESS(Status), "a handle without KEY_QUERY_VALUE was accepted\n"); /* lead: STATUS_ACCESS_DENIED */
        NtClose(Notify);
    }
}

/* The Win32 wrapper on the same key, for the comparison with the native result */
static void TestWin32Wrapper(void)
{
    HMODULE Advapi = LoadLibraryW(L"advapi32.dll");
    FN_RegQueryMultipleValuesW pRegQueryMultipleValuesW;
    HKEY Hkey;
    VALENTW Vals[3] = { { L"Dw" }, { L"Odd" }, { L"Str" } };
    DWORD Total = 0;
    LONG Error;
    PWSTR Buffer;

    pRegQueryMultipleValuesW = Advapi ? (FN_RegQueryMultipleValuesW)GetProcAddress(Advapi, "RegQueryMultipleValuesW") : NULL;
    if (!pRegQueryMultipleValuesW)
    {
        skip("RegQueryMultipleValuesW is not available\n");
        return;
    }
    Error = RegOpenKeyExW(UserHiveKey ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE,
                          UserHiveKey ? KeyPath : KeyPath + wcslen(L"\\Registry\\Machine\\"), 0, KEY_QUERY_VALUE, &Hkey);
    if (Error != ERROR_SUCCESS)
    {
        skip("RegOpenKeyExW failed: %ld\n", Error);
        return;
    }
    Error = pRegQueryMultipleValuesW(Hkey, Vals, 3, NULL, &Total);
    trace("PROBE RegQueryMultipleValuesW size query -> %ld, total %lu\n", Error, Total);
    Buffer = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Total ? Total : 1);
    if (Buffer)
    {
        Error = pRegQueryMultipleValuesW(Hkey, Vals, 3, Buffer, &Total);
        trace("PROBE RegQueryMultipleValuesW -> %ld, total %lu; offsets %Iu %Iu %Iu, lengths %lu %lu %lu\n", Error, Total,
              (SIZE_T)(Vals[0].ve_valueptr - (DWORD_PTR)Buffer), (SIZE_T)(Vals[1].ve_valueptr - (DWORD_PTR)Buffer),
              (SIZE_T)(Vals[2].ve_valueptr - (DWORD_PTR)Buffer), Vals[0].ve_valuelen, Vals[1].ve_valuelen, Vals[2].ve_valuelen);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
    }
    RegCloseKey(Hkey);
}

/* Two 1 MB values: the Win32 documentation names a 1 MB total limit; the native status is unknown */
static void TestTwoMegabytes(HANDLE Key)
{
    UNICODE_STRING Name1 = RTL_CONSTANT_STRING(L"Mb1"), Name2 = RTL_CONSTANT_STRING(L"Mb2");
    KEY_VALUE_ENTRY Entries[2] = { { &Name1 }, { &Name2 } };
    ULONG Length = 0, Required = 0;
    PUCHAR Data;
    NTSTATUS Status;

    Data = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, 1024 * 1024);
    if (!Data)
        return;
    Status = NtSetValueKey(Key, &Name1, 0, REG_BINARY, Data, 1024 * 1024);
    if (NT_SUCCESS(Status))
        Status = NtSetValueKey(Key, &Name2, 0, REG_BINARY, Data, 1024 * 1024);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Data);
    if (!NT_SUCCESS(Status))
    {
        skip("cannot store two 1 MB values: 0x%lx\n", Status);
        return;
    }
    Status = NtQueryMultipleValueKey(Key, Entries, 2, NULL, &Length, &Required);
    TraceEntries("two 1 MB values, length 0", Entries, 2, Status, Length, Required);
    NtDeleteValueKey(Key, &Name1);
    NtDeleteValueKey(Key, &Name2);
}

static void RunOnKey(HANDLE Key, const char *Kind)
{
    PUCHAR Big;
    ULONG i;
    NTSTATUS Status;

    trace("---- %s key ----\n", Kind);
    Status = NtSetValueKey(Key, &NameDw, 0, REG_DWORD, (PVOID)&DwordData, sizeof(DwordData));
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtSetValueKey(Key, &NameOdd, 0, REG_BINARY, (PVOID)OddData, sizeof(OddData));
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtSetValueKey(Key, &NameStr, 0, REG_SZ, (PVOID)StringData, sizeof(StringData));
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtSetValueKey(Key, &NameBin, 0, REG_BINARY, (PVOID)BinData, sizeof(BinData));
    ok_hex(Status, STATUS_SUCCESS);
    Big = RtlAllocateHeap(RtlGetProcessHeap(), 0, BIG_SIZE);
    if (Big)
    {
        for (i = 0; i < BIG_SIZE; i++)
            Big[i] = (UCHAR)(i * 7 + 3);
        Status = NtSetValueKey(Key, &NameBig, 0, REG_BINARY, Big, BIG_SIZE);
        ok_hex(Status, STATUS_SUCCESS);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Big);
    }

    TestThreeValues(Key);
    TestBigValue(Key);
    TestOddShapes(Key);
    TestInvalid(Key);
    TestManyEntries(Key);
    TestWin32Wrapper();
    TestTwoMegabytes(Key);
}

START_TEST(NtQueryMultipleValueKey)
{
    HANDLE Key = NULL;
    NTSTATUS Status;
    BOOLEAN Any = FALSE;

    Status = CreateTestKey(TRUE, &Key);
    if (NT_SUCCESS(Status))
    {
        RunOnKey(Key, "HKCU volatile");
        NtDeleteKey(Key);
        NtClose(Key);
        Any = TRUE;
    }
    else
    {
        trace("PROBE HKCU volatile key: 0x%lx\n", Status);
    }
    Status = CreateTestKey(FALSE, &Key);
    if (NT_SUCCESS(Status))
    {
        RunOnKey(Key, "HKLM volatile");
        NtDeleteKey(Key);
        NtClose(Key);
        Any = TRUE;
    }
    else
    {
        trace("PROBE HKLM volatile key: 0x%lx\n", Status);
    }
    if (!Any)
        skip("no test key could be created\n");
}
