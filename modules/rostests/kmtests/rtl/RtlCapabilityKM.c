/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Capability SID, persisted-state location, ACE lookup and Ex2 decompression tests
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

static
NTSTATUS
AddDeniedAce(
    _In_ PACL Acl,
    _In_ ACCESS_MASK Mask,
    _In_ PSID Sid)
{
    UCHAR Buffer[FIELD_OFFSET(ACCESS_DENIED_ACE, SidStart) + SECURITY_MAX_SID_SIZE];
    PACCESS_DENIED_ACE Ace = (PACCESS_DENIED_ACE)Buffer;
    ULONG SidLength = RtlLengthSid(Sid);

    Ace->Header.AceType = ACCESS_DENIED_ACE_TYPE;
    Ace->Header.AceFlags = 0;
    Ace->Header.AceSize = (USHORT)(FIELD_OFFSET(ACCESS_DENIED_ACE, SidStart) + SidLength);
    Ace->Mask = Mask;
    RtlCopySid(SidLength, (PSID)&Ace->SidStart, Sid);
    return RtlAddAce(Acl, ACL_REVISION, MAXULONG, Ace, Ace->Header.AceSize);
}

static
VOID
TestDeriveCapabilitySids(VOID)
{
    UCHAR GroupSidBuffer[SECURITY_MAX_SID_SIZE];
    UCHAR CapSidBuffer[SECURITY_MAX_SID_SIZE];
    UCHAR GroupSidBuffer2[SECURITY_MAX_SID_SIZE];
    UCHAR CapSidBuffer2[SECURITY_MAX_SID_SIZE];
    PSID GroupSid = (PSID)GroupSidBuffer;
    PSID CapSid = (PSID)CapSidBuffer;
    UNICODE_STRING Name;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, L"internetClient");
    Status = RtlDeriveCapabilitySidsFromName(&Name, GroupSid, CapSid);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(RtlIdentifierAuthoritySid(CapSid)->Value[5], 15UL);
    ok_eq_ulong(*RtlSubAuthorityCountSid(CapSid), 2UL);
    ok_eq_ulong(*RtlSubAuthoritySid(CapSid, 0), 3UL);
    ok_eq_ulong(*RtlSubAuthoritySid(CapSid, 1), SECURITY_CAPABILITY_INTERNET_CLIENT);
    ok_eq_ulong(RtlIdentifierAuthoritySid(GroupSid)->Value[5], 5UL);
    ok_eq_ulong(*RtlSubAuthorityCountSid(GroupSid), 9UL);
    ok_eq_ulong(*RtlSubAuthoritySid(GroupSid, 0), 32UL);

    RtlInitUnicodeString(&Name, L"internetClient");
    Status = RtlDeriveCapabilitySidsFromName(&Name, (PSID)GroupSidBuffer2, (PSID)CapSidBuffer2);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(RtlEqualSid(GroupSid, (PSID)GroupSidBuffer2), "group SID not deterministic\n");
    ok(RtlEqualSid(CapSid, (PSID)CapSidBuffer2), "capability SID not deterministic\n");

    RtlInitUnicodeString(&Name, L"someMadeUpCapabilityName");
    Status = RtlDeriveCapabilitySidsFromName(&Name, GroupSid, CapSid);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(*RtlSubAuthorityCountSid(CapSid), 10UL);
    ok_eq_ulong(*RtlSubAuthoritySid(CapSid, 0), 3UL);
    ok_eq_ulong(*RtlSubAuthoritySid(CapSid, 1), SECURITY_CAPABILITY_APP_RID);

    RtlInitUnicodeString(&Name, L"isolatedWin32-example");
    Status = RtlDeriveCapabilitySidsFromName(&Name, GroupSid, CapSid);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(*RtlSubAuthoritySid(CapSid, 1), SECURITY_CAPABILITY_APP_SILO_RID);

    Status = RtlDeriveCapabilitySidsFromName(&Name, NULL, CapSid);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
}

static
VOID
TestCheckTokenCapability(VOID)
{
    UCHAR CapSidBuffer[SECURITY_MAX_SID_SIZE];
    PSID CapSid = (PSID)CapSidBuffer;
    UNICODE_STRING Name;
    BOOLEAN HasCapability = FALSE;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, L"internetClient");
    Status = RtlDeriveCapabilitySidsFromName(&Name, CapSidBuffer, CapSid);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    Status = RtlCheckTokenCapability(NULL, CapSid, &HasCapability);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(HasCapability, 1UL);
}

static
VOID
TestGetPersistedStateLocation(VOID)
{
    WCHAR Buffer[MAX_PATH];
    ULONG OutLength = 0;
    NTSTATUS Status;

    Status = RtlGetPersistedStateLocation(L"Source", NULL, L"\\SystemRoot\\State",
                                          LocationTypeFileSystem, Buffer, sizeof(Buffer), &OutLength);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(OutLength, (ULONG)((wcslen(L"\\SystemRoot\\State") + 1) * sizeof(WCHAR)));
    ok_eq_ulong(Buffer[0], L'\\');

    Status = RtlGetPersistedStateLocation(L"Source", NULL, L"\\SystemRoot\\State",
                                          LocationTypeFileSystem, Buffer, 2, &OutLength);
    ok_eq_hex(Status, STATUS_BUFFER_OVERFLOW);
    ok_eq_ulong(OutLength, (ULONG)((wcslen(L"\\SystemRoot\\State") + 1) * sizeof(WCHAR)));

    Status = RtlGetPersistedStateLocation(NULL, NULL, L"\\SystemRoot\\State",
                                          LocationTypeFileSystem, Buffer, sizeof(Buffer), &OutLength);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
}

static
VOID
TestFindAceByType(VOID)
{
    UCHAR AclBuffer[256];
    PACL Acl = (PACL)AclBuffer;
    UCHAR SidBuffer[SECURITY_MAX_SID_SIZE];
    PSID Sid = (PSID)SidBuffer;
    SID_IDENTIFIER_AUTHORITY WorldAuthority = SECURITY_WORLD_SID_AUTHORITY;
    PVOID Ace;
    ULONG Index;
    NTSTATUS Status;

    RtlInitializeSid(Sid, &WorldAuthority, 1);
    *RtlSubAuthoritySid(Sid, 0) = SECURITY_WORLD_RID;

    Status = RtlCreateAcl(Acl, sizeof(AclBuffer), ACL_REVISION);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    Status = AddDeniedAce(Acl, GENERIC_ALL, Sid);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlAddAccessAllowedAce(Acl, ACL_REVISION, GENERIC_READ, Sid);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlAddAccessAllowedAce(Acl, ACL_REVISION, GENERIC_WRITE, Sid);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Index = 0;
    Ace = RtlFindAceByType(Acl, ACCESS_DENIED_ACE_TYPE, &Index);
    ok(Ace != NULL, "deny ACE not found\n");
    ok_eq_ulong(Index, 0UL);

    Index = 0;
    Ace = RtlFindAceByType(Acl, ACCESS_ALLOWED_ACE_TYPE, &Index);
    ok(Ace != NULL, "first allowed ACE not found\n");
    ok_eq_ulong(Index, 1UL);

    Ace = RtlFindAceByType(Acl, ACCESS_ALLOWED_ACE_TYPE, &Index);
    ok(Ace != NULL, "second allowed ACE not found\n");
    ok_eq_ulong(Index, 2UL);

    Ace = RtlFindAceByType(Acl, SYSTEM_AUDIT_ACE_TYPE, &Index);
    ok_eq_pointer(Ace, NULL);
}

static
VOID
TestDecompressBufferEx2(VOID)
{
    static const UCHAR Plain[] =
        "The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. "
        "The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. "
        "The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog.";
    UCHAR Compressed[sizeof(Plain) * 2];
    UCHAR Output[sizeof(Plain)];
    ULONG CompressedSize = 0, FinalSize = 0;
    NTSTATUS Status;

    Status = RtlCompressBuffer(COMPRESSION_FORMAT_LZNT1,
                               (PUCHAR)Plain, sizeof(Plain),
                               Compressed, sizeof(Compressed),
                               4096, &CompressedSize, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    Status = RtlDecompressBufferEx2(COMPRESSION_FORMAT_LZNT1,
                                    Output, sizeof(Output),
                                    Compressed, CompressedSize,
                                    4096, &FinalSize, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(FinalSize, sizeof(Plain));
    ok(RtlCompareMemory(Output, Plain, sizeof(Plain)) == sizeof(Plain), "round trip mismatch\n");

    Status = RtlDecompressBufferEx2(COMPRESSION_FORMAT_LZNT1,
                                    Output, sizeof(Output),
                                    Compressed, CompressedSize,
                                    8192, &FinalSize, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Status = RtlDecompressBufferEx2(COMPRESSION_FORMAT_NONE,
                                    Output, sizeof(Output),
                                    Compressed, CompressedSize,
                                    4096, &FinalSize, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Status = RtlDecompressBufferEx2(0x7F,
                                    Output, sizeof(Output),
                                    Compressed, CompressedSize,
                                    4096, &FinalSize, NULL);
    ok_eq_hex(Status, STATUS_UNSUPPORTED_COMPRESSION);
}

START_TEST(RtlCapabilityKM)
{
    TestDeriveCapabilitySids();
    TestCheckTokenCapability();
    TestGetPersistedStateLocation();
    TestFindAceByType();
    TestDecompressBufferEx2();
}
