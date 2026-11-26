/*
 * PROJECT:     Orosi API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Win11/Orosi comparison test for NtSetInformationKey (native parity lote 02, row L1)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <winreg.h>

/* Leads until the Win11 run pins them: every status is traced, the asserts only say "refused" */
#define TEST_KeyWriteTimeInformation    0
#define TEST_KeyWow64FlagsInformation   1
#define TEST_KeyControlFlagsInformation 2

static WCHAR KeyPath[128];

/* HKLM\SOFTWARE needs an administrator; the user's own hive takes the same rows for a standard user */
static NTSTATUS CreateTestKey(ULONG Options, BOOLEAN UserHive, PHANDLE Key)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;
    HANDLE Root = NULL;
    NTSTATUS Status;

    if (UserHive)
    {
        Status = RtlOpenCurrentUser(KEY_CREATE_SUB_KEY, &Root);
        if (!NT_SUCCESS(Status))
            return Status;
        StringCchPrintfW(KeyPath, _countof(KeyPath), L"Software\\Leeway-np02-SetInformationKey-%lu-%lu",
                         HandleToUlong(NtCurrentTeb()->ClientId.UniqueProcess), Options);
    }
    else
    {
        StringCchPrintfW(KeyPath, _countof(KeyPath), L"\\Registry\\Machine\\SOFTWARE\\Leeway-np02-SetInformationKey-%lu-%lu",
                         HandleToUlong(NtCurrentTeb()->ClientId.UniqueProcess), Options);
    }
    RtlInitUnicodeString(&Name, KeyPath);
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE, Root, NULL);
    Status = NtCreateKey(Key, KEY_ALL_ACCESS, &Oa, 0, NULL, Options, NULL);
    if (Root)
        NtClose(Root);
    return Status;
}

static LONGLONG QueryWriteTime(HANDLE Key, NTSTATUS *Status)
{
    KEY_BASIC_INFORMATION Basic;
    ULONG Length = 0;

    Basic.LastWriteTime.QuadPart = -1;
    *Status = NtQueryKey(Key, KeyBasicInformation, &Basic, sizeof(Basic), &Length);
    if (*Status == STATUS_BUFFER_OVERFLOW)   /* the name does not fit, the fixed part is valid */
        *Status = STATUS_SUCCESS;
    return Basic.LastWriteTime.QuadPart;
}

/* NtQueryKey(KeyFlagsInformation): 12 bytes (Wow64Flags, KeyFlags, ControlFlags); the read side of this test's writes */
static void QueryFlags(HANDLE Key, const char *When, ULONG *Flags)
{
    ULONG Length = 0;
    NTSTATUS Status;

    Flags[0] = Flags[1] = Flags[2] = 0xDEADBEEF;
    Status = NtQueryKey(Key, KeyFlagsInformation, Flags, 3 * sizeof(ULONG), &Length);
    trace("PROBE %s: KeyFlagsInformation -> 0x%lx, length %lu, words 0x%lx 0x%lx 0x%lx\n",
          When, Status, Length, Flags[0], Flags[1], Flags[2]);
    ok(NT_SUCCESS(Status) && Length == 3 * sizeof(ULONG), "%s: KeyFlagsInformation 0x%lx, length %lu\n", When, Status, Length);
}

/* L1-M3: combined faults, so the Win11 run pins which check comes first */
static void TestValidationOrder(HANDLE Key, HANDLE ReadOnly)
{
    UCHAR Buffer[16] = { 0 };
    HANDLE Event = NULL;
    NTSTATUS Status;

    NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    /* Win11 26100: class, then length, then the buffer, then the handle (type and access) */
    Status = NtSetInformationKey(Key, 0xFF, Buffer, 3);
    trace("PROBE order: bad class + bad length -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_INVALID_INFO_CLASS);
    Status = NtSetInformationKey(Event, TEST_KeyWriteTimeInformation, Buffer, 3);
    trace("PROBE order: bad length + event handle -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    Status = NtSetInformationKey(ReadOnly, TEST_KeyWriteTimeInformation, Buffer, 3);
    trace("PROBE order: KEY_READ handle + bad length -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    Status = NtSetInformationKey(ReadOnly, TEST_KeyWriteTimeInformation, NULL, sizeof(LARGE_INTEGER));
    trace("PROBE order: KEY_READ handle + NULL buffer -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);
    Status = NtSetInformationKey(Event, 0xFF, Buffer, sizeof(ULONG));
    trace("PROBE order: bad class + event handle -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_INVALID_INFO_CLASS);
    Status = NtSetInformationKey(Key, TEST_KeyWow64FlagsInformation, Buffer + 1, sizeof(ULONG));
    trace("PROBE order: valid class + misaligned buffer -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtSetInformationKey(NULL, 0xFF, Buffer, 3);
    trace("PROBE order: NULL handle + bad class + bad length -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_INVALID_INFO_CLASS);
    if (Event)
        NtClose(Event);
}

static void TestInvalid(HANDLE Key, HANDLE ReadOnly)
{
    static const ULONG TimeLengths[] = { 0, 4, 7, 12, 16 };
    static const ULONG FlagLengths[] = { 0, 3, 5, 8 };
    static const ULONG OtherClasses[] = { 3, 4, 5, 6, 7, 0xFF, 0x80000000 };
    UCHAR Buffer[32] = { 0 };
    ULONG Class, i;
    HANDLE Event = NULL;
    NTSTATUS Status;

    for (i = 0; i < _countof(TimeLengths); i++)
    {
        Status = NtSetInformationKey(Key, TEST_KeyWriteTimeInformation, Buffer, TimeLengths[i]);
        trace("PROBE class 0 length %lu -> 0x%lx\n", TimeLengths[i], Status);
        ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    }
    for (Class = TEST_KeyWow64FlagsInformation; Class <= TEST_KeyControlFlagsInformation; Class++)
    {
        for (i = 0; i < _countof(FlagLengths); i++)
        {
            Status = NtSetInformationKey(Key, Class, Buffer, FlagLengths[i]);
            trace("PROBE class %lu length %lu -> 0x%lx\n", Class, FlagLengths[i], Status);
            ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
        }
    }

    /* Win11 26100 implements classes 3-5 (a 4-byte zero buffer is accepted) and refuses 6 and above
       with STATUS_INVALID_INFO_CLASS. Orosi has no classes 3-5 yet: traced, not pinned (follow-up row) */
    for (i = 0; i < _countof(OtherClasses); i++)
    {
        Status = NtSetInformationKey(Key, OtherClasses[i], Buffer, sizeof(ULONG));
        trace("PROBE class %lu with 4 bytes -> 0x%lx\n", OtherClasses[i], Status);
        if (OtherClasses[i] >= 6)
            ok_hex(Status, STATUS_INVALID_INFO_CLASS);
    }

    /* Buffers */
    Status = NtSetInformationKey(Key, TEST_KeyWriteTimeInformation, NULL, sizeof(LARGE_INTEGER));
    trace("PROBE class 0 NULL buffer -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);
    /* Win11 has no alignment rule for the buffer: this writes a zero time */
    Status = NtSetInformationKey(Key, TEST_KeyWriteTimeInformation, Buffer + 1, sizeof(LARGE_INTEGER));
    trace("PROBE class 0 misaligned buffer -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtSetInformationKey(Key, TEST_KeyWow64FlagsInformation, (PVOID)(ULONG_PTR)1, sizeof(ULONG));
    trace("PROBE class 1 bad buffer -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Wrong object type, bad handle */
    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtSetInformationKey(Event, TEST_KeyWriteTimeInformation, Buffer, sizeof(LARGE_INTEGER));
    trace("PROBE event handle -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_OBJECT_TYPE_MISMATCH);
    NtClose(Event);
    Status = NtSetInformationKey(NULL, TEST_KeyWriteTimeInformation, Buffer, sizeof(LARGE_INTEGER));
    trace("PROBE NULL handle -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_INVALID_HANDLE);

    /* A read-only handle for each class */
    Status = NtSetInformationKey(ReadOnly, TEST_KeyWriteTimeInformation, Buffer, sizeof(LARGE_INTEGER));
    trace("PROBE class 0 KEY_READ handle -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_ACCESS_DENIED);
    for (Class = TEST_KeyWow64FlagsInformation; Class <= TEST_KeyControlFlagsInformation; Class++)
    {
        Status = NtSetInformationKey(ReadOnly, Class, Buffer, sizeof(ULONG));
        trace("PROBE class %lu KEY_READ handle -> 0x%lx\n", Class, Status);
        ok_hex(Status, STATUS_ACCESS_DENIED);
    }

    /* Flag values beyond four bits: Win11 refuses them for the Wow64 flags and takes them for the control flags */
    for (Class = TEST_KeyWow64FlagsInformation; Class <= TEST_KeyControlFlagsInformation; Class++)
    {
        ULONG Value = 0x10, Flags[3];
        Status = NtSetInformationKey(Key, Class, &Value, sizeof(Value));
        trace("PROBE class %lu value 0x10 -> 0x%lx\n", Class, Status);
        ok_hex(Status, Class == TEST_KeyWow64FlagsInformation ? STATUS_INVALID_PARAMETER : STATUS_SUCCESS);
        if (Class == TEST_KeyControlFlagsInformation)
            QueryFlags(Key, "after control flags 0x10", Flags);
        Value = 0xFFFFFFFF;
        Status = NtSetInformationKey(Key, Class, &Value, sizeof(Value));
        trace("PROBE class %lu value 0xFFFFFFFF -> 0x%lx\n", Class, Status);
        ok_hex(Status, Class == TEST_KeyWow64FlagsInformation ? STATUS_INVALID_PARAMETER : STATUS_SUCCESS);
        if (Class == TEST_KeyControlFlagsInformation)
        {
            QueryFlags(Key, "after control flags 0xFFFFFFFF", Flags);
            Value = 0;
            NtSetInformationKey(Key, Class, &Value, sizeof(Value));
        }
    }
}

static void TestWriteTime(HANDLE Key, const char *Kind)
{
    KEY_WRITE_TIME_INFORMATION Info;
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"Touch");
    ULONG Data = 1;
    LONGLONG Before, After, Later;
    NTSTATUS Status, QueryStatus;

    Before = QueryWriteTime(Key, &QueryStatus);
    ok(NT_SUCCESS(QueryStatus), "%s: KeyBasicInformation failed: 0x%lx\n", Kind, QueryStatus);

    /* 2001-01-01 00:00:00 UTC */
    RtlSecondsSince1970ToTime(978307200, &Info.LastWriteTime);
    Status = NtSetInformationKey(Key, TEST_KeyWriteTimeInformation, &Info, sizeof(Info));
    trace("PROBE %s: set write time -> 0x%lx\n", Kind, Status);
    ok(NT_SUCCESS(Status), "%s: setting the write time failed: 0x%lx\n", Kind, Status);
    After = QueryWriteTime(Key, &QueryStatus);
    ok(After == Info.LastWriteTime.QuadPart, "%s: write time read back 0x%I64x, set 0x%I64x (before 0x%I64x)\n",
       Kind, After, Info.LastWriteTime.QuadPart, Before);

    /* A value write moves the time forward again */
    Status = NtSetValueKey(Key, &ValueName, 0, REG_DWORD, &Data, sizeof(Data));
    ok_hex(Status, STATUS_SUCCESS);
    Later = QueryWriteTime(Key, &QueryStatus);
    ok(Later > After, "%s: the write time did not advance after a value write: 0x%I64x <= 0x%I64x\n", Kind, Later, After);
    NtDeleteValueKey(Key, &ValueName);
}

static void TestFlags(HANDLE Key, BOOLEAN Volatile)
{
    static const ULONG ControlFlags[] = { 0x2, 0x4, 0x8, 0xE, 0 };
    ULONG Value, i, Flags[3];
    NTSTATUS Status;

    QueryFlags(Key, "before", Flags);
    ok(Flags[1] == (Volatile ? 0x1 : 0), "KeyFlags 0x%lx on a %s key\n", Flags[1], Volatile ? "volatile" : "persistent"); /* lead: REG_FLAG_VOLATILE */
    for (Value = 0; Value <= 0xF; Value++)
    {
        Status = NtSetInformationKey(Key, TEST_KeyWow64FlagsInformation, &Value, sizeof(Value));
        ok(NT_SUCCESS(Status), "Wow64 flags 0x%lx refused: 0x%lx\n", Value, Status);
        QueryFlags(Key, "after Wow64 flags", Flags);
        ok(Flags[0] == Value, "Wow64Flags read back 0x%lx, set 0x%lx\n", Flags[0], Value);
    }
    Value = 0;
    Status = NtSetInformationKey(Key, TEST_KeyWow64FlagsInformation, &Value, sizeof(Value));
    ok_hex(Status, STATUS_SUCCESS);

    for (i = 0; i < _countof(ControlFlags); i++)
    {
        Value = ControlFlags[i];
        Status = NtSetInformationKey(Key, TEST_KeyControlFlagsInformation, &Value, sizeof(Value));
        trace("PROBE control flags 0x%lx -> 0x%lx\n", Value, Status);
        ok(NT_SUCCESS(Status), "control flags 0x%lx refused: 0x%lx\n", Value, Status);
        QueryFlags(Key, "after control flags", Flags);
        ok(Flags[2] == Value, "ControlFlags read back 0x%lx, set 0x%lx\n", Flags[2], Value);
    }

    /* The short-buffer answer of the query side */
    Status = NtQueryKey(Key, KeyFlagsInformation, Flags, 8, &i);
    trace("PROBE KeyFlagsInformation with 8 bytes -> 0x%lx, result length %lu\n", Status, i);
    ok_hex(Status, STATUS_BUFFER_TOO_SMALL);
    ok(i == 3 * sizeof(ULONG), "KeyFlagsInformation result length %lu\n", i);
}

static void RunOnKey(HANDLE Key, BOOLEAN Volatile, const char *Kind)
{
    HANDLE ReadOnly = NULL;
    NTSTATUS Status;

    Status = NtDuplicateObject(NtCurrentProcess(), Key, NtCurrentProcess(), &ReadOnly, KEY_READ, 0, 0);
    ok_hex(Status, STATUS_SUCCESS);
    if (!ReadOnly)
        return;
    trace("---- %s key ----\n", Kind);
    TestInvalid(Key, ReadOnly);
    TestValidationOrder(Key, ReadOnly);
    TestWriteTime(Key, Kind);
    TestFlags(Key, Volatile);
    NtClose(ReadOnly);
}

START_TEST(NtSetInformationKey)
{
    HANDLE Key = NULL;
    NTSTATUS Status;
    BOOLEAN Any = FALSE;

    /* The caller's own hive first: a standard user can run these rows */
    Status = CreateTestKey(REG_OPTION_VOLATILE, TRUE, &Key);
    if (NT_SUCCESS(Status))
    {
        RunOnKey(Key, TRUE, "HKCU volatile");
        NtDeleteKey(Key);
        NtClose(Key);
        Any = TRUE;
    }
    else
    {
        trace("PROBE HKCU volatile key: 0x%lx\n", Status);
    }

    /* HKLM\SOFTWARE: volatile, then persistent for the dirty-cell path (administrator) */
    Status = CreateTestKey(REG_OPTION_VOLATILE, FALSE, &Key);
    if (NT_SUCCESS(Status))
    {
        RunOnKey(Key, TRUE, "HKLM volatile");
        NtDeleteKey(Key);
        NtClose(Key);
        Any = TRUE;
    }
    else
    {
        trace("PROBE HKLM volatile key: 0x%lx\n", Status);
    }
    Status = CreateTestKey(REG_OPTION_NON_VOLATILE, FALSE, &Key);
    if (NT_SUCCESS(Status))
    {
        trace("---- HKLM persistent key ----\n");
        TestWriteTime(Key, "HKLM persistent");
        TestFlags(Key, FALSE);
        NtDeleteKey(Key);
        NtClose(Key);
        Any = TRUE;
    }
    else
    {
        trace("PROBE HKLM persistent key: 0x%lx\n", Status);
    }
    if (!Any)
        skip("no test key could be created\n");
}
