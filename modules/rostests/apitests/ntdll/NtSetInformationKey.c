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

static NTSTATUS CreateTestKey(ULONG Options, PHANDLE Key)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;

    StringCchPrintfW(KeyPath, _countof(KeyPath), L"\\Registry\\Machine\\SOFTWARE\\Leeway-np02-SetInformationKey-%lu-%lu",
                     HandleToUlong(NtCurrentTeb()->ClientId.UniqueProcess), Options);
    RtlInitUnicodeString(&Name, KeyPath);
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NtCreateKey(Key, KEY_ALL_ACCESS, &Oa, 0, NULL, Options, NULL);
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

/* NtQueryKey(KeyFlagsInformation): Win11 answers 12 bytes (Wow64Flags, KeyFlags, ControlFlags); Orosi refuses it */
static void TraceFlags(HANDLE Key, const char *When)
{
    ULONG Flags[3] = { 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF }, Length = 0;
    NTSTATUS Status;

    Status = NtQueryKey(Key, KeyFlagsInformation, Flags, sizeof(Flags), &Length);
    trace("PROBE %s: KeyFlagsInformation -> 0x%lx, length %lu, words 0x%lx 0x%lx 0x%lx\n",
          When, Status, Length, Flags[0], Flags[1], Flags[2]);
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
        ok(!NT_SUCCESS(Status), "class 0 accepted length %lu\n", TimeLengths[i]); /* lead: STATUS_INFO_LENGTH_MISMATCH */
    }
    for (Class = TEST_KeyWow64FlagsInformation; Class <= TEST_KeyControlFlagsInformation; Class++)
    {
        for (i = 0; i < _countof(FlagLengths); i++)
        {
            Status = NtSetInformationKey(Key, Class, Buffer, FlagLengths[i]);
            trace("PROBE class %lu length %lu -> 0x%lx\n", Class, FlagLengths[i], Status);
            ok(!NT_SUCCESS(Status), "class %lu accepted length %lu\n", Class, FlagLengths[i]);
        }
    }

    /* Classes this row does not implement: what Win11 says defines them */
    for (i = 0; i < _countof(OtherClasses); i++)
    {
        Status = NtSetInformationKey(Key, OtherClasses[i], Buffer, sizeof(ULONG));
        trace("PROBE class %lu with 4 bytes -> 0x%lx\n", OtherClasses[i], Status);
    }

    /* Buffers */
    Status = NtSetInformationKey(Key, TEST_KeyWriteTimeInformation, NULL, sizeof(LARGE_INTEGER));
    trace("PROBE class 0 NULL buffer -> 0x%lx\n", Status);
    ok(!NT_SUCCESS(Status), "class 0 accepted a NULL buffer\n");
    Status = NtSetInformationKey(Key, TEST_KeyWriteTimeInformation, Buffer + 1, sizeof(LARGE_INTEGER));
    trace("PROBE class 0 misaligned buffer -> 0x%lx\n", Status);
    Status = NtSetInformationKey(Key, TEST_KeyWow64FlagsInformation, (PVOID)(ULONG_PTR)1, sizeof(ULONG));
    trace("PROBE class 1 bad buffer -> 0x%lx\n", Status);
    ok(!NT_SUCCESS(Status), "class 1 accepted a bad buffer\n");

    /* Wrong object type, bad handle */
    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtSetInformationKey(Event, TEST_KeyWriteTimeInformation, Buffer, sizeof(LARGE_INTEGER));
    trace("PROBE event handle -> 0x%lx\n", Status);
    ok(!NT_SUCCESS(Status), "an event handle was accepted\n"); /* lead: STATUS_OBJECT_TYPE_MISMATCH */
    NtClose(Event);
    Status = NtSetInformationKey(NULL, TEST_KeyWriteTimeInformation, Buffer, sizeof(LARGE_INTEGER));
    trace("PROBE NULL handle -> 0x%lx\n", Status);
    ok(!NT_SUCCESS(Status), "a NULL handle was accepted\n");

    /* A read-only handle for each class */
    Status = NtSetInformationKey(ReadOnly, TEST_KeyWriteTimeInformation, Buffer, sizeof(LARGE_INTEGER));
    trace("PROBE class 0 KEY_READ handle -> 0x%lx\n", Status);
    ok(!NT_SUCCESS(Status), "class 0 accepted a KEY_READ handle\n"); /* lead: STATUS_ACCESS_DENIED */
    for (Class = TEST_KeyWow64FlagsInformation; Class <= TEST_KeyControlFlagsInformation; Class++)
    {
        Status = NtSetInformationKey(ReadOnly, Class, Buffer, sizeof(ULONG));
        trace("PROBE class %lu KEY_READ handle -> 0x%lx\n", Class, Status);
        ok(!NT_SUCCESS(Status), "class %lu accepted a KEY_READ handle\n", Class);
    }

    /* Flag values beyond the four bits the key node stores */
    for (Class = TEST_KeyWow64FlagsInformation; Class <= TEST_KeyControlFlagsInformation; Class++)
    {
        ULONG Value = 0x10;
        Status = NtSetInformationKey(Key, Class, &Value, sizeof(Value));
        trace("PROBE class %lu value 0x10 -> 0x%lx\n", Class, Status);
        Value = 0xFFFFFFFF;
        Status = NtSetInformationKey(Key, Class, &Value, sizeof(Value));
        trace("PROBE class %lu value 0xFFFFFFFF -> 0x%lx\n", Class, Status);
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

static void TestFlags(HANDLE Key)
{
    static const ULONG ControlFlags[] = { 0x2, 0x4, 0x8, 0xE, 0 };
    ULONG Value, i;
    NTSTATUS Status;

    TraceFlags(Key, "before");
    for (Value = 0; Value <= 0xF; Value++)
    {
        Status = NtSetInformationKey(Key, TEST_KeyWow64FlagsInformation, &Value, sizeof(Value));
        ok(NT_SUCCESS(Status), "Wow64 flags 0x%lx refused: 0x%lx\n", Value, Status);
        if (Value == 0xF || Value == 0x1)
            TraceFlags(Key, "after Wow64 flags");
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
        TraceFlags(Key, "after control flags");
    }
}

START_TEST(NtSetInformationKey)
{
    HANDLE Key = NULL, ReadOnly = NULL, Persistent = NULL;
    NTSTATUS Status;

    Status = CreateTestKey(REG_OPTION_VOLATILE, &Key);
    if (!NT_SUCCESS(Status))
    {
        skip("cannot create the volatile test key: 0x%lx (run elevated)\n", Status);
        return;
    }
    Status = NtDuplicateObject(NtCurrentProcess(), Key, NtCurrentProcess(), &ReadOnly, KEY_READ, 0, 0);
    ok_hex(Status, STATUS_SUCCESS);

    TestInvalid(Key, ReadOnly);
    TestWriteTime(Key, "volatile key");
    TestFlags(Key);

    /* The same on a key in the persistent SOFTWARE hive: the dirty-cell path */
    Status = CreateTestKey(REG_OPTION_NON_VOLATILE, &Persistent);
    if (NT_SUCCESS(Status))
    {
        TestWriteTime(Persistent, "persistent key");
        NtDeleteKey(Persistent);
        NtClose(Persistent);
    }
    else
    {
        skip("cannot create a persistent test key: 0x%lx\n", Status);
    }

    if (ReadOnly)
        NtClose(ReadOnly);
    NtDeleteKey(Key);
    NtClose(Key);
}
