/*
 * PROJECT:     Orosi API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Win11/Orosi comparison tests for NtSetInformationSymbolicLink
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#ifndef SYMBOLIC_LINK_SET
#define SYMBOLIC_LINK_SET 0x0002
#endif
#define TEST_SymbolicLinkGlobalInformation 1
#define TEST_SymbolicLinkAccessMask        2

typedef NTSTATUS (NTAPI *FN_NtSetInformationSymbolicLink)(HANDLE, ULONG, PVOID, ULONG);
static FN_NtSetInformationSymbolicLink pNtSetInformationSymbolicLink;

static WCHAR BnoPath[64];
static ULONG Unique;
static BOOLEAN HaveTcb;

static
NTSTATUS
CreateLink(ACCESS_MASK Access, PCWSTR Target, PHANDLE Handle)
{
    WCHAR Buffer[128];
    UNICODE_STRING Name, TargetName;
    OBJECT_ATTRIBUTES Oa;

    StringCchPrintfW(Buffer, _countof(Buffer), L"%s\\npsl-%lu-%lu", BnoPath,
                     HandleToUlong(NtCurrentTeb()->ClientId.UniqueProcess), ++Unique);
    RtlInitUnicodeString(&Name, Buffer);
    RtlInitUnicodeString(&TargetName, Target);
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    *Handle = NULL;
    return NtCreateSymbolicLinkObject(Handle, Access, &Oa, &TargetName);
}

static
VOID
TestAccessMaskMapping(VOID)
{
    HANDLE Link;
    NTSTATUS Status;
    OBJECT_BASIC_INFORMATION Info;

    Status = CreateLink(SYMBOLIC_LINK_ALL_ACCESS | SYMBOLIC_LINK_SET, L"\\BaseNamedObjects", &Link);
    trace("PROBE create link ALL|SET -> 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;
    Status = NtQueryObject(Link, ObjectBasicInformation, &Info, sizeof(Info), NULL);
    ok_hex(Status, STATUS_SUCCESS);
    trace("PROBE granted access for ALL|SET = 0x%08lx\n", Info.GrantedAccess);
    NtClose(Link);

    Status = CreateLink(GENERIC_ALL, L"\\BaseNamedObjects", &Link);
    if (NT_SUCCESS(Status))
    {
        NtQueryObject(Link, ObjectBasicInformation, &Info, sizeof(Info), NULL);
        trace("PROBE GENERIC_ALL maps to 0x%08lx\n", Info.GrantedAccess);
        NtClose(Link);
    }
}

static
VOID
TestInvalidInput(VOID)
{
    HANDLE Link = NULL, QueryOnly = NULL, Event = NULL;
    ULONG Value = 0, Class, Len;
    ULONGLONG Big = 0;
    NTSTATUS Status;
    static const ULONG Classes[] = { 0, 3, 4, 0xFF, 0x80000000 };
    static const ULONG Lengths[] = { 0, 1, 3, 5, 8 };

    Status = CreateLink(SYMBOLIC_LINK_ALL_ACCESS | SYMBOLIC_LINK_SET, L"\\BaseNamedObjects", &Link);
    if (!NT_SUCCESS(Status))
        Status = CreateLink(SYMBOLIC_LINK_ALL_ACCESS, L"\\BaseNamedObjects", &Link);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    for (Class = 0; Class < _countof(Classes); Class++)
    {
        Status = pNtSetInformationSymbolicLink(Link, Classes[Class], &Value, sizeof(Value));
        trace("PROBE class %lu -> 0x%lx\n", Classes[Class], Status);
        ok(!NT_SUCCESS(Status), "class %lu accepted\n", Classes[Class]);
    }

    for (Class = TEST_SymbolicLinkGlobalInformation; Class <= TEST_SymbolicLinkAccessMask; Class++)
    {
        for (Len = 0; Len < _countof(Lengths); Len++)
        {
            Status = pNtSetInformationSymbolicLink(Link, Class, &Big, Lengths[Len]);
            trace("PROBE class %lu len %lu -> 0x%lx\n", Class, Lengths[Len], Status);
        }
        Status = pNtSetInformationSymbolicLink(Link, Class, NULL, sizeof(ULONG));
        trace("PROBE class %lu NULL buffer -> 0x%lx\n", Class, Status);
        ok(!NT_SUCCESS(Status), "class %lu NULL buffer accepted\n", Class);
        Status = pNtSetInformationSymbolicLink(Link, Class, (PVOID)(ULONG_PTR)1, sizeof(ULONG));
        trace("PROBE class %lu bad buffer -> 0x%lx\n", Class, Status);
        ok(!NT_SUCCESS(Status), "class %lu bad buffer accepted\n", Class);
    }

    /* Wrong object type */
    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    ok_hex(Status, STATUS_SUCCESS);
    Status = pNtSetInformationSymbolicLink(Event, TEST_SymbolicLinkGlobalInformation, &Value, sizeof(Value));
    trace("PROBE event handle -> 0x%lx\n", Status);
    ok_hex(Status, STATUS_OBJECT_TYPE_MISMATCH);
    NtClose(Event);

    Status = pNtSetInformationSymbolicLink(NULL, TEST_SymbolicLinkGlobalInformation, &Value, sizeof(Value));
    ok_hex(Status, STATUS_INVALID_HANDLE);

    /* Handle without SYMBOLIC_LINK_SET */
    Status = NtDuplicateObject(NtCurrentProcess(), Link, NtCurrentProcess(), &QueryOnly,
                               SYMBOLIC_LINK_QUERY, 0, 0);
    ok_hex(Status, STATUS_SUCCESS);
    for (Class = TEST_SymbolicLinkGlobalInformation; Class <= TEST_SymbolicLinkAccessMask; Class++)
    {
        Status = pNtSetInformationSymbolicLink(QueryOnly, Class, &Value, sizeof(Value));
        trace("PROBE class %lu QUERY-only handle -> 0x%lx\n", Class, Status);
        ok(!NT_SUCCESS(Status), "class %lu with QUERY-only handle accepted\n", Class);
    }
    NtClose(QueryOnly);
    NtClose(Link);
}

/* Without TCB a session-local link must never become global. */
static
VOID
TestPrivilege(VOID)
{
    HANDLE Link;
    ULONG Value = 1;
    ACCESS_MASK Mask = SYMBOLIC_LINK_QUERY;
    NTSTATUS Status;

    Status = CreateLink(SYMBOLIC_LINK_ALL_ACCESS | SYMBOLIC_LINK_SET, L"\\BaseNamedObjects", &Link);
    if (!NT_SUCCESS(Status))
    {
        skip("cannot create a link with SYMBOLIC_LINK_SET: 0x%lx\n", Status);
        return;
    }

    Status = pNtSetInformationSymbolicLink(Link, TEST_SymbolicLinkGlobalInformation, &Value, sizeof(Value));
    trace("PROBE global=1 (tcb=%d) -> 0x%lx\n", HaveTcb, Status);
    if (!HaveTcb)
        ok_hex(Status, STATUS_PRIVILEGE_NOT_HELD);
    else
    {
        ok_hex(Status, STATUS_SUCCESS);
        Value = 0;
        Status = pNtSetInformationSymbolicLink(Link, TEST_SymbolicLinkGlobalInformation, &Value, sizeof(Value));
        trace("PROBE global=0 (tcb) -> 0x%lx\n", Status);
    }

    Status = pNtSetInformationSymbolicLink(Link, TEST_SymbolicLinkAccessMask, &Mask, sizeof(Mask));
    trace("PROBE accessmask=QUERY (tcb=%d) -> 0x%lx\n", HaveTcb, Status);
    if (!HaveTcb)
        ok(!NT_SUCCESS(Status), "access mask set without TCB: 0x%lx\n", Status);
    NtClose(Link);
}

/* S4: what SymbolicLinkAccessMask restricts is defined by this probe on Win11. */
static
VOID
TestAccessMaskEffect(VOID)
{
    static const ACCESS_MASK Masks[] = { 0, DIRECTORY_QUERY, DIRECTORY_TRAVERSE, SYMBOLIC_LINK_QUERY, DIRECTORY_ALL_ACCESS };
    static const ACCESS_MASK Wants[] = { DIRECTORY_QUERY, DIRECTORY_TRAVERSE, DIRECTORY_CREATE_OBJECT, DIRECTORY_ALL_ACCESS };
    UNICODE_STRING Empty = RTL_CONSTANT_STRING(L"");
    OBJECT_ATTRIBUTES Oa;
    HANDLE Link, H;
    ULONG m, w;
    NTSTATUS Status;

    if (!HaveTcb)
    {
        skip("access-mask effect needs TCB (run as SYSTEM)
");
        return;
    }
    for (m = 0; m < _countof(Masks); m++)
    {
        if (!NT_SUCCESS(CreateLink(SYMBOLIC_LINK_ALL_ACCESS | SYMBOLIC_LINK_SET, BnoPath, &Link)))
            return;
        Status = pNtSetInformationSymbolicLink(Link, TEST_SymbolicLinkAccessMask, (PVOID)&Masks[m], sizeof(ACCESS_MASK));
        trace("PROBE accessmask set 0x%08lx -> 0x%lx
", Masks[m], Status);
        InitializeObjectAttributes(&Oa, &Empty, 0, Link, NULL);
        for (w = 0; w < _countof(Wants); w++)
        {
            Status = NtOpenDirectoryObject(&H, Wants[w], &Oa);
            trace("PROBE mask 0x%08lx open target want 0x%08lx -> 0x%lx
", Masks[m], Wants[w], Status);
            if (NT_SUCCESS(Status))
                NtClose(H);
        }
        Status = NtOpenSymbolicLinkObject(&H, SYMBOLIC_LINK_QUERY, &Oa);
        trace("PROBE mask 0x%08lx reopen link itself -> 0x%lx
", Masks[m], Status);
        if (NT_SUCCESS(Status))
            NtClose(H);
        NtClose(Link);
    }
}

/* S6: a standard user may not globalise even a link it created and fully owns. */
static
VOID
TestOwnLinkNotGlobal(VOID)
{
    HANDLE Link;
    ULONG Value = 1;
    NTSTATUS Status;

    if (HaveTcb)
        return;
    if (!NT_SUCCESS(CreateLink(GENERIC_ALL, L"\BaseNamedObjects", &Link)))
        return;
    Status = pNtSetInformationSymbolicLink(Link, TEST_SymbolicLinkGlobalInformation, &Value, sizeof(Value));
    trace("PROBE own GENERIC_ALL link global=1 (no tcb) -> 0x%lx
", Status);
    ok(!NT_SUCCESS(Status), "standard user globalised its own link
");
    NtClose(Link);
}

typedef struct _RACE { HANDLE Link; volatile LONG Stop; LONG Failures; } RACE;

static
DWORD
WINAPI
OpenThread(PVOID Param)
{
    RACE *Race = Param;
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"");
    OBJECT_ATTRIBUTES Oa;
    HANDLE Dir;

    InitializeObjectAttributes(&Oa, &Name, 0, Race->Link, NULL);
    while (!Race->Stop)
    {
        if (NT_SUCCESS(NtOpenDirectoryObject(&Dir, DIRECTORY_QUERY, &Oa)))
            NtClose(Dir);
    }
    return 0;
}

static
VOID
TestConcurrency(VOID)
{
    RACE Race = { NULL, 0, 0 };
    HANDLE Threads[3];
    ULONG i, Value;
    NTSTATUS Status;

    if (!HaveTcb)
    {
        skip("concurrency needs TCB (run as SYSTEM)\n");
        return;
    }
    if (!NT_SUCCESS(CreateLink(SYMBOLIC_LINK_ALL_ACCESS | SYMBOLIC_LINK_SET, BnoPath, &Race.Link)))
        return;
    for (i = 0; i < _countof(Threads); i++)
        Threads[i] = CreateThread(NULL, 0, OpenThread, &Race, 0, NULL);
    for (i = 0; i < 2000; i++)
    {
        Value = i & 1;
        Status = pNtSetInformationSymbolicLink(Race.Link, TEST_SymbolicLinkGlobalInformation, &Value, sizeof(Value));
        if (!NT_SUCCESS(Status))
            Race.Failures++;
    }
    Race.Stop = 1;
    WaitForMultipleObjects(_countof(Threads), Threads, TRUE, 60000);
    for (i = 0; i < _countof(Threads); i++)
        CloseHandle(Threads[i]);
    ok(Race.Failures == 0, "%ld flag toggles failed\n", Race.Failures);
    NtClose(Race.Link);
}

START_TEST(NtSetInformationSymbolicLink)
{
    ULONG Session = NtCurrentPeb()->SessionId;
    BOOLEAN Old;

    pNtSetInformationSymbolicLink = (FN_NtSetInformationSymbolicLink)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationSymbolicLink");
    if (!pNtSetInformationSymbolicLink)
    {
        skip("NtSetInformationSymbolicLink is not exported\n");
        return;
    }
    if (Session == 0)
        StringCchCopyW(BnoPath, _countof(BnoPath), L"\\BaseNamedObjects");
    else
        StringCchPrintfW(BnoPath, _countof(BnoPath), L"\\Sessions\\%lu\\BaseNamedObjects", Session);
    HaveTcb = NT_SUCCESS(RtlAdjustPrivilege(SE_TCB_PRIVILEGE, TRUE, FALSE, &Old));
    trace("session %lu, TCB %s\n", Session, HaveTcb ? "held" : "not held");

    TestAccessMaskMapping();
    TestInvalidInput();
    TestPrivilege();
    TestOwnLinkNotGlobal();
    TestAccessMaskEffect();
    TestConcurrency();
}
