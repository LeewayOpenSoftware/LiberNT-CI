/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test for RtlCreateAndSetSD, rows measured on Windows 11 26100
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

typedef NTSTATUS (NTAPI *FN_RtlCreateAndSetSD)(PRTL_ACE_DATA, ULONG, PSID, PSID, PSECURITY_DESCRIPTOR *);

static FN_RtlCreateAndSetSD pRtlCreateAndSetSD;

static SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
static SID_IDENTIFIER_AUTHORITY WorldAuthority = {SECURITY_WORLD_SID_AUTHORITY};
static ULONG AdminsBuf[SECURITY_MAX_SID_SIZE / sizeof(ULONG)];
static ULONG WorldBuf[SECURITY_MAX_SID_SIZE / sizeof(ULONG)];
static ULONG SystemBuf[SECURITY_MAX_SID_SIZE / sizeof(ULONG)];
static ULONG UsersBuf[SECURITY_MAX_SID_SIZE / sizeof(ULONG)];
static PSID Admins = AdminsBuf, World = WorldBuf, LocalSystem = SystemBuf, Users = UsersBuf;

static
VOID
InitSids(VOID)
{
    RtlInitializeSid(Admins, &NtAuthority, 2);
    *RtlSubAuthoritySid(Admins, 0) = SECURITY_BUILTIN_DOMAIN_RID;
    *RtlSubAuthoritySid(Admins, 1) = DOMAIN_ALIAS_RID_ADMINS;
    RtlInitializeSid(Users, &NtAuthority, 2);
    *RtlSubAuthoritySid(Users, 0) = SECURITY_BUILTIN_DOMAIN_RID;
    *RtlSubAuthoritySid(Users, 1) = DOMAIN_ALIAS_RID_USERS;
    RtlInitializeSid(World, &WorldAuthority, 1);
    *RtlSubAuthoritySid(World, 0) = SECURITY_WORLD_RID;
    RtlInitializeSid(LocalSystem, &NtAuthority, 1);
    *RtlSubAuthoritySid(LocalSystem, 0) = SECURITY_LOCAL_SYSTEM_RID;
}

static
RTL_ACE_DATA
MakeAce(UCHAR Type, UCHAR Inherit, UCHAR Flags, ACCESS_MASK Mask, PSID *Sid)
{
    RTL_ACE_DATA Data;

    Data.AceType = Type;
    Data.InheritFlags = Inherit;
    Data.AceFlags = Flags;
    Data.Mask = Mask;
    Data.Sid = Sid;
    return Data;
}

/* One ACE: header, mask and SID. The 4 bytes after the SID are not compared. */
static
VOID
CheckAce(PACL Acl, ULONG Index, UCHAR Type, UCHAR Flags, ACCESS_MASK Mask, PSID Sid)
{
    PACCESS_ALLOWED_ACE Entry;
    NTSTATUS Status;

    Status = RtlGetAce(Acl, Index, (PVOID *)&Entry);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    ok_hex(Entry->Header.AceType, Type);
    ok_hex(Entry->Header.AceFlags, Flags);
    ok_int(Entry->Header.AceSize, (int)(sizeof(ACCESS_ALLOWED_ACE) + RtlLengthSid(Sid)));
    ok_hex(Entry->Mask, Mask);
    ok(RtlEqualMemory(&Entry->SidStart, Sid, RtlLengthSid(Sid)), "ACE %lu SID differs\n", Index);
}

static
VOID
GetParts(PSECURITY_DESCRIPTOR Sd, PACL *Dacl, PACL *Sacl, BOOLEAN *DaclPresent, BOOLEAN *SaclPresent)
{
    BOOLEAN Defaulted;

    *Dacl = *Sacl = NULL;
    ok_ntstatus(RtlGetDaclSecurityDescriptor(Sd, DaclPresent, Dacl, &Defaulted), STATUS_SUCCESS);
    ok_ntstatus(RtlGetSaclSecurityDescriptor(Sd, SaclPresent, Sacl, &Defaulted), STATUS_SUCCESS);
}

static
VOID
TestLayout(VOID)
{
    RTL_ACE_DATA Data[3];
    PSECURITY_DESCRIPTOR Sd = NULL;
    PISECURITY_DESCRIPTOR Abs;
    PACL Dacl, Sacl;
    BOOLEAN DaclPresent, SaclPresent;
    NTSTATUS Status;

    /* Two allowed ACEs; owner and group are referenced, not copied */
    Data[0] = MakeAce(ACCESS_ALLOWED_ACE_TYPE, 0, 0, FILE_ALL_ACCESS, &Admins);
    Data[1] = MakeAce(ACCESS_ALLOWED_ACE_TYPE, 0, 0, FILE_GENERIC_READ, &World);
    Status = pRtlCreateAndSetSD(Data, 2, LocalSystem, Users, &Sd);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Abs = Sd;
        ok_hex(Abs->Revision, SECURITY_DESCRIPTOR_REVISION);
        ok_hex(Abs->Control, SE_DACL_PRESENT);
        ok(Abs->Owner == LocalSystem, "owner is not the SID passed in\n");
        ok(Abs->Group == Users, "group is not the SID passed in\n");
        ok_size_t(RtlSizeHeap(RtlGetProcessHeap(), 0, Sd),
                  sizeof(SECURITY_DESCRIPTOR) + sizeof(ACL) +
                  2 * sizeof(ACCESS_ALLOWED_ACE) + RtlLengthSid(Admins) + RtlLengthSid(World));
        GetParts(Sd, &Dacl, &Sacl, &DaclPresent, &SaclPresent);
        ok(DaclPresent && Dacl != NULL, "no DACL\n");
        ok(!SaclPresent, "SACL present\n");
        if (Dacl)
        {
            ok(Dacl == (PACL)((PUCHAR)Sd + sizeof(SECURITY_DESCRIPTOR)), "DACL not after the SD\n");
            ok_hex(Dacl->AclRevision, ACL_REVISION);
            ok_int(Dacl->AclSize, (int)(sizeof(ACL) + 2 * sizeof(ACCESS_ALLOWED_ACE) +
                                        RtlLengthSid(Admins) + RtlLengthSid(World)));
            ok_int(Dacl->AceCount, 2);
            CheckAce(Dacl, 0, ACCESS_ALLOWED_ACE_TYPE, 0, FILE_ALL_ACCESS, Admins);
            CheckAce(Dacl, 1, ACCESS_ALLOWED_ACE_TYPE, 0, FILE_GENERIC_READ, World);
        }
        ok(RtlValidSecurityDescriptor(Sd), "SD not valid\n");
        ok(RtlFreeHeap(RtlGetProcessHeap(), 0, Sd), "RtlFreeHeap failed\n");
    }

    /* Audit, allowed, denied: the DACL keeps input order, the SACL follows it */
    Sd = NULL;
    Data[0] = MakeAce(SYSTEM_AUDIT_ACE_TYPE, 0, FAILED_ACCESS_ACE_FLAG, 1, &World);
    Data[1] = MakeAce(ACCESS_ALLOWED_ACE_TYPE, 0, 0, FILE_ALL_ACCESS, &Admins);
    Data[2] = MakeAce(ACCESS_DENIED_ACE_TYPE, 0, 0, 2, &World);
    Status = pRtlCreateAndSetSD(Data, 3, LocalSystem, Users, &Sd);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_hex(((PISECURITY_DESCRIPTOR)Sd)->Control, SE_DACL_PRESENT | SE_SACL_PRESENT);
        GetParts(Sd, &Dacl, &Sacl, &DaclPresent, &SaclPresent);
        ok(Dacl && Sacl, "missing ACL\n");
        if (Dacl && Sacl)
        {
            ok(Sacl == (PACL)((PUCHAR)Dacl + Dacl->AclSize), "SACL not after the DACL\n");
            ok_int(Dacl->AceCount, 2);
            CheckAce(Dacl, 0, ACCESS_ALLOWED_ACE_TYPE, 0, FILE_ALL_ACCESS, Admins);
            CheckAce(Dacl, 1, ACCESS_DENIED_ACE_TYPE, 0, 2, World);
            ok_int(Sacl->AceCount, 1);
            CheckAce(Sacl, 0, SYSTEM_AUDIT_ACE_TYPE, FAILED_ACCESS_ACE_FLAG, 1, World);
        }
        RtlFreeHeap(RtlGetProcessHeap(), 0, Sd);
    }
}

static
VOID
TestFlagsAndEdges(VOID)
{
    RTL_ACE_DATA Data[2];
    PSECURITY_DESCRIPTOR Sd;
    PACL Dacl, Sacl;
    BOOLEAN DaclPresent, SaclPresent;
    NTSTATUS Status;
    ULONG BadSidBuf[3] = { 0 };
    PSID BadSid = BadSidBuf;

    /* Header flags are InheritFlags | AceFlags; generic bits are not mapped */
    Sd = NULL;
    Data[0] = MakeAce(ACCESS_ALLOWED_ACE_TYPE, OBJECT_INHERIT_ACE,
                      CONTAINER_INHERIT_ACE | INHERITED_ACE, GENERIC_ALL, &World);
    Data[1] = MakeAce(SYSTEM_AUDIT_ACE_TYPE, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                      SUCCESSFUL_ACCESS_ACE_FLAG, 1, &World);
    Status = pRtlCreateAndSetSD(Data, 2, NULL, NULL, &Sd);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(((PISECURITY_DESCRIPTOR)Sd)->Owner == NULL, "owner set\n");
        ok(((PISECURITY_DESCRIPTOR)Sd)->Group == NULL, "group set\n");
        GetParts(Sd, &Dacl, &Sacl, &DaclPresent, &SaclPresent);
        ok(Dacl && Sacl, "missing ACL\n");
        if (Dacl)
            CheckAce(Dacl, 0, ACCESS_ALLOWED_ACE_TYPE,
                     OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERITED_ACE, GENERIC_ALL, World);
        if (Sacl)
            CheckAce(Sacl, 0, SYSTEM_AUDIT_ACE_TYPE,
                     OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | SUCCESSFUL_ACCESS_ACE_FLAG, 1, World);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Sd);
    }

    /* Only audit ACEs: the DACL is present and NULL */
    Sd = NULL;
    Data[0] = MakeAce(SYSTEM_AUDIT_ACE_TYPE, 0, 0, 1, &World);
    Status = pRtlCreateAndSetSD(Data, 1, LocalSystem, Users, &Sd);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        GetParts(Sd, &Dacl, &Sacl, &DaclPresent, &SaclPresent);
        ok(DaclPresent && Dacl == NULL, "expected a present NULL DACL\n");
        ok(SaclPresent && Sacl != NULL, "expected a SACL\n");
        ok_size_t(RtlSizeHeap(RtlGetProcessHeap(), 0, Sd),
                  sizeof(SECURITY_DESCRIPTOR) + sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + RtlLengthSid(World));
        RtlFreeHeap(RtlGetProcessHeap(), 0, Sd);
    }

    /* No ACEs at all */
    Sd = NULL;
    Status = pRtlCreateAndSetSD(NULL, 0, LocalSystem, Users, &Sd);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        GetParts(Sd, &Dacl, &Sacl, &DaclPresent, &SaclPresent);
        ok(DaclPresent && Dacl == NULL, "expected a present NULL DACL\n");
        ok(!SaclPresent, "SACL present\n");
        ok_size_t(RtlSizeHeap(RtlGetProcessHeap(), 0, Sd), sizeof(SECURITY_DESCRIPTOR));
        RtlFreeHeap(RtlGetProcessHeap(), 0, Sd);
    }

    /* SIDs are not validated: a revision 2 SID is accepted */
    ((PUCHAR)BadSidBuf)[0] = 2;
    ((PUCHAR)BadSidBuf)[1] = 1;
    ((PUCHAR)BadSidBuf)[7] = 1;
    Sd = NULL;
    Data[0] = MakeAce(ACCESS_ALLOWED_ACE_TYPE, 0, 0, 1, &BadSid);
    Status = pRtlCreateAndSetSD(Data, 1, BadSid, Users, &Sd);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(!RtlValidSecurityDescriptor(Sd), "SD with a revision 2 SID is valid\n");
        RtlFreeHeap(RtlGetProcessHeap(), 0, Sd);
    }
}

static
VOID
TestInvalid(VOID)
{
    static const UCHAR BadTypes[] = { ACCESS_ALLOWED_COMPOUND_ACE_TYPE, SYSTEM_ALARM_ACE_TYPE,
                                      ACCESS_ALLOWED_OBJECT_ACE_TYPE, 7, ACCESS_ALLOWED_CALLBACK_ACE_TYPE,
                                      0x11, 0xFF };
    RTL_ACE_DATA Data[2], *Many;
    PSECURITY_DESCRIPTOR Sd;
    NTSTATUS Status;
    ULONG Index, Count;

    /* Any type other than allowed, denied or audit, also when not first */
    for (Index = 0; Index < ARRAYSIZE(BadTypes); Index++)
    {
        Sd = NULL;
        Data[0] = MakeAce(ACCESS_ALLOWED_ACE_TYPE, 0, 0, 1, &World);
        Data[1] = MakeAce(BadTypes[Index], 0, 0, 1, &World);
        Status = pRtlCreateAndSetSD(Data, 2, LocalSystem, Users, &Sd);
        ok(Status == STATUS_INVALID_PARAMETER, "type 0x%x: status 0x%lx\n", BadTypes[Index], Status);
        if (NT_SUCCESS(Status))
            RtlFreeHeap(RtlGetProcessHeap(), 0, Sd);
    }

    /* The largest DACL: 8 + 2340 * 28 = 65528 bytes fits, one more ACE does not */
    Many = RtlAllocateHeap(RtlGetProcessHeap(), 0, 2341 * sizeof(*Many));
    if (!Many)
    {
        skip("no memory\n");
        return;
    }
    for (Index = 0; Index < 2341; Index++)
        Many[Index] = MakeAce(ACCESS_ALLOWED_ACE_TYPE, 0, 0, 1, &Admins);
    for (Count = 2340; Count <= 2341; Count++)
    {
        Sd = NULL;
        Status = pRtlCreateAndSetSD(Many, Count, LocalSystem, Users, &Sd);
        ok_ntstatus(Status, Count == 2340 ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER);
        if (NT_SUCCESS(Status))
            RtlFreeHeap(RtlGetProcessHeap(), 0, Sd);
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Many);
}

START_TEST(RtlCreateAndSetSD)
{
    pRtlCreateAndSetSD = (FN_RtlCreateAndSetSD)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                              "RtlCreateAndSetSD");
    if (!pRtlCreateAndSetSD)
    {
        skip("RtlCreateAndSetSD is not exported\n");
        return;
    }
    InitSids();
    TestLayout();
    TestFlagsAndEdges();
    TestInvalid();
}
