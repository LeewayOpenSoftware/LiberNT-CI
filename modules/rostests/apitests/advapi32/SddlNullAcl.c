/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the NO_ACCESS_CONTROL SDDL flag round trip through sechost
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <sddl.h>

typedef struct _PARSE_CASE
{
    PCWSTR String;
    SECURITY_DESCRIPTOR_CONTROL Control;
    BOOL DaclPresent;
    BOOL DaclNull;
    BOOL SaclPresent;
    BOOL SaclNull;
    ULONG Size;
    PCWSTR Reverse;
} PARSE_CASE;

/* Expected values were read from Windows 11 24H2 with the same calls */
static const PARSE_CASE ParseCases[] =
{
    { L"D:NO_ACCESS_CONTROL", SE_SELF_RELATIVE | SE_DACL_PRESENT, TRUE, TRUE, FALSE, TRUE, 20, L"D:NO_ACCESS_CONTROL" },
    { L"S:NO_ACCESS_CONTROL", SE_SELF_RELATIVE | SE_SACL_PRESENT, FALSE, TRUE, TRUE, TRUE, 20, L"S:NO_ACCESS_CONTROL" },
    { L"D:", SE_SELF_RELATIVE | SE_DACL_PRESENT, TRUE, FALSE, FALSE, TRUE, 28, L"D:" },
    { L"S:", SE_SELF_RELATIVE | SE_SACL_PRESENT, FALSE, TRUE, TRUE, FALSE, 28, L"S:" },
    { L"D:PNO_ACCESS_CONTROL", SE_SELF_RELATIVE | SE_DACL_PRESENT | SE_DACL_PROTECTED, TRUE, TRUE, FALSE, TRUE, 20, L"D:PNO_ACCESS_CONTROL" },
    { L"D:AINO_ACCESS_CONTROL", SE_SELF_RELATIVE | SE_DACL_PRESENT | SE_DACL_AUTO_INHERITED, TRUE, TRUE, FALSE, TRUE, 20, L"D:AINO_ACCESS_CONTROL" },
    { L"D:PARAINO_ACCESS_CONTROL", SE_SELF_RELATIVE | SE_DACL_PRESENT | SE_DACL_PROTECTED | SE_DACL_AUTO_INHERIT_REQ | SE_DACL_AUTO_INHERITED, TRUE, TRUE, FALSE, TRUE, 20, L"D:PARAINO_ACCESS_CONTROL" },
    { L"D:no_access_control", SE_SELF_RELATIVE | SE_DACL_PRESENT, TRUE, TRUE, FALSE, TRUE, 20, L"D:NO_ACCESS_CONTROL" },
    { L"D:p", SE_SELF_RELATIVE | SE_DACL_PRESENT | SE_DACL_PROTECTED, TRUE, FALSE, FALSE, TRUE, 28, L"D:P" },
    { L"D: P", SE_SELF_RELATIVE | SE_DACL_PRESENT | SE_DACL_PROTECTED, TRUE, FALSE, FALSE, TRUE, 28, L"D:P" },
    { L"S:PNO_ACCESS_CONTROL", SE_SELF_RELATIVE | SE_SACL_PRESENT | SE_SACL_PROTECTED, FALSE, TRUE, TRUE, TRUE, 20, L"S:PNO_ACCESS_CONTROL" },
    { L"O:BAD:NO_ACCESS_CONTROLS:NO_ACCESS_CONTROL", SE_SELF_RELATIVE | SE_DACL_PRESENT | SE_SACL_PRESENT, TRUE, TRUE, TRUE, TRUE, 36, L"O:BAD:NO_ACCESS_CONTROLS:NO_ACCESS_CONTROL" },
    { L"D:NO_ACCESS_CONTROLS:(AU;SA;GA;;;WD)", SE_SELF_RELATIVE | SE_DACL_PRESENT | SE_SACL_PRESENT, TRUE, TRUE, TRUE, FALSE, 48, L"D:NO_ACCESS_CONTROLS:(AU;SA;GA;;;WD)" },
    { L"D:NO_ACCESS_CONTROLG:BA", SE_SELF_RELATIVE | SE_DACL_PRESENT, TRUE, TRUE, FALSE, TRUE, 36, L"G:BAD:NO_ACCESS_CONTROL" },
    { L"D:(A;;GA;;;WD)NO_ACCESS_CONTROL", SE_SELF_RELATIVE | SE_DACL_PRESENT, TRUE, FALSE, FALSE, TRUE, 48, L"D:(A;;GA;;;WD)" },
    { L"D:PS:", SE_SELF_RELATIVE | SE_DACL_PRESENT | SE_DACL_PROTECTED | SE_SACL_PRESENT, TRUE, FALSE, TRUE, FALSE, 36, L"D:PS:" },
};

static const PCWSTR RejectedStrings[] =
{
    L"D:NO_ACCESS_CONTROL(A;;GA;;;WD)",
    L"D:NO_ACCESS_CONTROLP",
    L"D:NO_ACCESS_CONTROL:",
    L"D:NO_ACCESS",
    L"D:XYZ",
    L"D:A",
    L"D:AX",
    L"S:NO_ACCESS_CONTROL(AU;SA;GA;;;WD)",
};

static PWSTR ReverseString(PSECURITY_DESCRIPTOR Sd)
{
    PWSTR String = NULL;
    ULONG Length = 0;
    BOOL Ret;

    Ret = ConvertSecurityDescriptorToStringSecurityDescriptorW(Sd, SDDL_REVISION_1,
                                                               OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                                                               DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION,
                                                               &String, &Length);
    ok(Ret, "ConvertSecurityDescriptorToStringSecurityDescriptorW failed: %lu\n", GetLastError());
    if (!Ret)
        return NULL;
    ok(Length == wcslen(String) + 1, "Length %lu for '%ls'\n", Length, String);
    return String;
}

static void TestParse(const PARSE_CASE *Case)
{
    PSECURITY_DESCRIPTOR Sd = NULL;
    SECURITY_DESCRIPTOR_CONTROL Control = 0;
    DWORD Revision = 0;
    ULONG Size = 0;
    BOOL Present, Defaulted, Ret;
    PACL Acl;
    PWSTR String;

    SetLastError(0xdeadbeef);
    Ret = ConvertStringSecurityDescriptorToSecurityDescriptorW(Case->String, SDDL_REVISION_1, &Sd, &Size);
    ok(Ret, "'%ls': conversion failed: %lu\n", Case->String, GetLastError());
    if (!Ret)
        return;

    ok(Size == Case->Size, "'%ls': size %lu, expected %lu\n", Case->String, Size, Case->Size);
    ok(IsValidSecurityDescriptor(Sd), "'%ls': descriptor is not valid\n", Case->String);

    Ret = GetSecurityDescriptorControl(Sd, &Control, &Revision);
    ok(Ret, "'%ls': GetSecurityDescriptorControl failed: %lu\n", Case->String, GetLastError());
    ok(Control == Case->Control, "'%ls': control 0x%04x, expected 0x%04x\n", Case->String, Control, Case->Control);

    Present = !Case->DaclPresent;
    Acl = (PACL)(ULONG_PTR)-1;
    Ret = GetSecurityDescriptorDacl(Sd, &Present, &Acl, &Defaulted);
    ok(Ret, "'%ls': GetSecurityDescriptorDacl failed: %lu\n", Case->String, GetLastError());
    ok(Present == Case->DaclPresent, "'%ls': DACL present %d, expected %d\n", Case->String, Present, Case->DaclPresent);
    ok((Acl == NULL) == Case->DaclNull, "'%ls': DACL %p, expected %s\n", Case->String, Acl, Case->DaclNull ? "NULL" : "an ACL");
    if (Acl)
        ok(IsValidAcl(Acl), "'%ls': DACL is not valid\n", Case->String);

    Present = !Case->SaclPresent;
    Acl = (PACL)(ULONG_PTR)-1;
    Ret = GetSecurityDescriptorSacl(Sd, &Present, &Acl, &Defaulted);
    ok(Ret, "'%ls': GetSecurityDescriptorSacl failed: %lu\n", Case->String, GetLastError());
    ok(Present == Case->SaclPresent, "'%ls': SACL present %d, expected %d\n", Case->String, Present, Case->SaclPresent);
    ok((Acl == NULL) == Case->SaclNull, "'%ls': SACL %p, expected %s\n", Case->String, Acl, Case->SaclNull ? "NULL" : "an ACL");
    if (Acl)
        ok(IsValidAcl(Acl), "'%ls': SACL is not valid\n", Case->String);

    String = ReverseString(Sd);
    if (String)
    {
        ok(wcscmp(String, Case->Reverse) == 0, "'%ls': reverse is '%ls', expected '%ls'\n", Case->String, String, Case->Reverse);
        LocalFree(String);
    }

    LocalFree(Sd);
}

static void TestRejected(PCWSTR String)
{
    PSECURITY_DESCRIPTOR Sd = (PSECURITY_DESCRIPTOR)(ULONG_PTR)-1;
    ULONG Size = 0xdeadbeef;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = ConvertStringSecurityDescriptorToSecurityDescriptorW(String, SDDL_REVISION_1, &Sd, &Size);
    ok(!Ret, "'%ls': conversion succeeded, expected failure\n", String);
    if (!Ret)
        ok(GetLastError() == ERROR_INVALID_PARAMETER, "'%ls': last error %lu, expected %u\n", String, GetLastError(), ERROR_INVALID_PARAMETER);
    else
        LocalFree(Sd);
}

static void TestAbsoluteNullAcls(void)
{
    SECURITY_DESCRIPTOR Sd;
    PWSTR String;
    BOOL Ret;

    Ret = InitializeSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION);
    ok(Ret, "InitializeSecurityDescriptor failed: %lu\n", GetLastError());

    String = ReverseString(&Sd);
    if (String)
    {
        ok(String[0] == 0, "no ACLs: reverse is '%ls', expected ''\n", String);
        LocalFree(String);
    }

    Ret = SetSecurityDescriptorDacl(&Sd, TRUE, NULL, FALSE);
    ok(Ret, "SetSecurityDescriptorDacl failed: %lu\n", GetLastError());
    String = ReverseString(&Sd);
    if (String)
    {
        ok(wcscmp(String, L"D:NO_ACCESS_CONTROL") == 0, "NULL DACL: reverse is '%ls'\n", String);
        LocalFree(String);
    }

    Ret = SetSecurityDescriptorSacl(&Sd, TRUE, NULL, FALSE);
    ok(Ret, "SetSecurityDescriptorSacl failed: %lu\n", GetLastError());
    String = ReverseString(&Sd);
    if (String)
    {
        ok(wcscmp(String, L"D:NO_ACCESS_CONTROLS:NO_ACCESS_CONTROL") == 0, "NULL DACL and SACL: reverse is '%ls'\n", String);
        LocalFree(String);
    }
}

START_TEST(SddlNullAcl)
{
    ULONG i;

    for (i = 0; i < ARRAYSIZE(ParseCases); i++)
        TestParse(&ParseCases[i]);

    for (i = 0; i < ARRAYSIZE(RejectedStrings); i++)
        TestRejected(RejectedStrings[i]);

    TestAbsoluteNullAcls();
}
