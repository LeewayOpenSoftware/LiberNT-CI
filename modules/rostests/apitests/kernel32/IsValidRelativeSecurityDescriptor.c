/*
 * PROJECT:     LiberNT kernel32 API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     kernelbase!IsValidRelativeSecurityDescriptor validates a self-relative descriptor
 */

#include "precomp.h"
#include <aclapi.h>

/* The self-relative layout is public: header, then four offsets */
typedef struct _REL_SD
{
    BYTE Revision;
    BYTE Sbz1;
    WORD Control;
    DWORD Owner;
    DWORD Group;
    DWORD Sacl;
    DWORD Dacl;
} REL_SD, *PREL_SD;

typedef BOOL (WINAPI *PFN_ISVALIDRELATIVESD)(PSECURITY_DESCRIPTOR, ULONG, SECURITY_INFORMATION);

START_TEST(IsValidRelativeSecurityDescriptor)
{
    PFN_ISVALIDRELATIVESD pIsValid;
    SID_IDENTIFIER_AUTHORITY NtAuthority = { SECURITY_NT_AUTHORITY };
    SECURITY_DESCRIPTOR Absolute;
    PSECURITY_DESCRIPTOR Relative;
    PSID System = NULL;
    BYTE AclBuffer[256];
    PACL Acl = (PACL)AclBuffer;
    DWORD Length = 0;
    BOOL Result;
    HMODULE Module;

    Module = LoadLibraryW(L"kernelbase.dll");
    ok(Module != NULL, "kernelbase.dll did not load\n");
    if (!Module)
        return;

    pIsValid = (PFN_ISVALIDRELATIVESD)GetProcAddress(Module, "IsValidRelativeSecurityDescriptor");
    ok(pIsValid != NULL, "kernelbase!IsValidRelativeSecurityDescriptor is not exported\n");
    if (!pIsValid)
        return;

    ok(AllocateAndInitializeSid(&NtAuthority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &System),
       "AllocateAndInitializeSid failed: %lu\n", GetLastError());
    ok(InitializeAcl(Acl, sizeof(AclBuffer), ACL_REVISION), "InitializeAcl failed\n");
    ok(AddAccessAllowedAce(Acl, ACL_REVISION, GENERIC_ALL, System), "AddAccessAllowedAce failed\n");
    ok(InitializeSecurityDescriptor(&Absolute, SECURITY_DESCRIPTOR_REVISION), "InitializeSecurityDescriptor failed\n");
    ok(SetSecurityDescriptorOwner(&Absolute, System, FALSE), "SetSecurityDescriptorOwner failed\n");
    ok(SetSecurityDescriptorGroup(&Absolute, System, FALSE), "SetSecurityDescriptorGroup failed\n");
    ok(SetSecurityDescriptorDacl(&Absolute, TRUE, Acl, FALSE), "SetSecurityDescriptorDacl failed\n");

    MakeSelfRelativeSD(&Absolute, NULL, &Length);
    ok(Length != 0, "MakeSelfRelativeSD gave no size\n");
    Relative = LocalAlloc(LPTR, Length);
    ok(Relative != NULL, "LocalAlloc failed\n");
    if (!Relative || !MakeSelfRelativeSD(&Absolute, Relative, &Length))
    {
        skip("Could not build a self-relative descriptor\n");
        goto Done;
    }

    /* A well formed descriptor with every part required */
    Result = pIsValid(Relative, Length, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                      DACL_SECURITY_INFORMATION);
    ok(Result, "A valid descriptor was rejected\n");

    /* Truncated in every part: owner, group and ACL no longer fit */
    Result = pIsValid(Relative, Length - 1, 0);
    ok(!Result, "A descriptor one byte short was accepted\n");
    Result = pIsValid(Relative, Length / 2, 0);
    ok(!Result, "A half descriptor was accepted\n");
    Result = pIsValid(Relative, sizeof(REL_SD) - 1, 0);
    ok(!Result, "A descriptor shorter than its header was accepted\n");
    Result = pIsValid(Relative, 0, 0);
    ok(!Result, "A zero length descriptor was accepted\n");

    /* Not self-relative, or the wrong revision */
    Result = pIsValid(&Absolute, sizeof(Absolute), 0);
    ok(!Result, "An absolute descriptor was accepted\n");
    ((PREL_SD)Relative)->Revision = 2;
    Result = pIsValid(Relative, Length, 0);
    ok(!Result, "A descriptor with revision 2 was accepted\n");
    ((PREL_SD)Relative)->Revision = SECURITY_DESCRIPTOR_REVISION;

    /* An owner offset that points outside the buffer */
    {
        ULONG Saved = ((PREL_SD)Relative)->Owner;

        ((PREL_SD)Relative)->Owner = Length + 16;
        Result = pIsValid(Relative, Length, 0);
        ok(!Result, "An owner offset beyond the buffer was accepted\n");
        ((PREL_SD)Relative)->Owner = Saved;
    }

    /* The caller requires an owner the descriptor does not have */
    ((PREL_SD)Relative)->Owner = 0;
    Result = pIsValid(Relative, Length, OWNER_SECURITY_INFORMATION);
    ok(!Result, "A missing required owner was accepted\n");
    Result = pIsValid(Relative, Length, 0);
    ok(Result, "A descriptor without an owner was rejected when none is required\n");

    /* NULL pointer */
    Result = pIsValid(NULL, Length, 0);
    ok(!Result, "A NULL descriptor was accepted\n");

Done:
    if (Relative)
        LocalFree(Relative);
    if (System)
        FreeSid(System);
    FreeLibrary(Module);
}
