/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Check that API set contracts resolve to the Win11 host DLL and that
 *              the host exports every function of the contract
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>

typedef struct _CONTRACT_HOST
{
    PCWSTR Contract;
    PCWSTR Host;
    const PCSTR *Functions;
    ULONG Count;
} CONTRACT_HOST;

static const PCSTR SecurityBaseFunctions[] =
{
    "AccessCheck",
    "AccessCheckAndAuditAlarmW",
    "AccessCheckByType",
    "AccessCheckByTypeAndAuditAlarmW",
    "AccessCheckByTypeResultList",
    "AccessCheckByTypeResultListAndAuditAlarmByHandleW",
    "AccessCheckByTypeResultListAndAuditAlarmW",
    "AddAccessAllowedAce",
    "AddAccessAllowedAceEx",
    "AddAccessAllowedObjectAce",
    "AddAccessDeniedAce",
    "AddAccessDeniedAceEx",
    "AddAccessDeniedObjectAce",
    "AddAce",
    "AddAuditAccessAce",
    "AddAuditAccessAceEx",
    "AddAuditAccessObjectAce",
    "AddMandatoryAce",
    "AdjustTokenGroups",
    "AdjustTokenPrivileges",
    "AllocateAndInitializeSid",
    "AllocateLocallyUniqueId",
    "AreAllAccessesGranted",
    "AreAnyAccessesGranted",
    "CheckTokenMembership",
    "ConvertToAutoInheritPrivateObjectSecurity",
    "CopySid",
    "CreatePrivateObjectSecurity",
    "CreatePrivateObjectSecurityEx",
    "CreatePrivateObjectSecurityWithMultipleInheritance",
    "CreateRestrictedToken",
    "CreateWellKnownSid",
    "DeleteAce",
    "DestroyPrivateObjectSecurity",
    "DuplicateToken",
    "DuplicateTokenEx",
    "EqualDomainSid",
    "EqualPrefixSid",
    "EqualSid",
    "FindFirstFreeAce",
    "FreeSid",
    "GetAce",
    "GetAclInformation",
    "GetFileSecurityW",
    "GetKernelObjectSecurity",
    "GetLengthSid",
    "GetPrivateObjectSecurity",
    "GetSecurityDescriptorControl",
    "GetSecurityDescriptorDacl",
    "GetSecurityDescriptorGroup",
    "GetSecurityDescriptorLength",
    "GetSecurityDescriptorOwner",
    "GetSecurityDescriptorRMControl",
    "GetSecurityDescriptorSacl",
    "GetSidIdentifierAuthority",
    "GetSidLengthRequired",
    "GetSidSubAuthority",
    "GetSidSubAuthorityCount",
    "GetTokenInformation",
    "GetWindowsAccountDomainSid",
    "ImpersonateAnonymousToken",
    "ImpersonateLoggedOnUser",
    "ImpersonateSelf",
    "InitializeAcl",
    "InitializeSecurityDescriptor",
    "InitializeSid",
    "IsTokenRestricted",
    "IsValidAcl",
    "IsValidSecurityDescriptor",
    "IsValidSid",
    "IsWellKnownSid",
    "MakeAbsoluteSD",
    "MakeSelfRelativeSD",
    "MapGenericMask",
    "ObjectCloseAuditAlarmW",
    "ObjectDeleteAuditAlarmW",
    "ObjectOpenAuditAlarmW",
    "ObjectPrivilegeAuditAlarmW",
    "PrivilegeCheck",
    "PrivilegedServiceAuditAlarmW",
    "QuerySecurityAccessMask",
    "RevertToSelf",
    "SetAclInformation",
    "SetFileSecurityW",
    "SetKernelObjectSecurity",
    "SetPrivateObjectSecurity",
    "SetPrivateObjectSecurityEx",
    "SetSecurityAccessMask",
    "SetSecurityDescriptorControl",
    "SetSecurityDescriptorDacl",
    "SetSecurityDescriptorGroup",
    "SetSecurityDescriptorOwner",
    "SetSecurityDescriptorRMControl",
    "SetSecurityDescriptorSacl",
    "SetTokenInformation",
};

static const PCSTR StringAnsiFunctions[] =
{
    "CharLowerA",
    "CharLowerBuffA",
    "CharNextA",
    "CharNextExA",
    "CharPrevA",
    "CharPrevExA",
    "CharUpperA",
    "CharUpperBuffA",
    "IsCharAlphaA",
    "IsCharAlphaNumericA",
    "IsCharLowerA",
    "IsCharUpperA",
};

static const PCSTR UnicodeAnsiFunctions[] =
{
    "SHAnsiToAnsi",
    "SHAnsiToUnicode",
    "SHUnicodeToAnsi",
    "SHUnicodeToUnicode",
};

static const PCSTR MidlProxyStubFunctions[] =
{
    "CStdAsyncStubBuffer2_Connect",
    "CStdAsyncStubBuffer2_Disconnect",
    "CStdAsyncStubBuffer2_Release",
    "CStdAsyncStubBuffer_AddRef",
    "CStdAsyncStubBuffer_Connect",
    "CStdAsyncStubBuffer_Disconnect",
    "CStdAsyncStubBuffer_Invoke",
    "CStdAsyncStubBuffer_QueryInterface",
    "CStdAsyncStubBuffer_Release",
    "CStdStubBuffer2_Connect",
    "CStdStubBuffer2_CountRefs",
    "CStdStubBuffer2_Disconnect",
    "CStdStubBuffer2_QueryInterface",
    "NdrProxyForwardingFunction10",
    "NdrProxyForwardingFunction11",
    "NdrProxyForwardingFunction12",
    "NdrProxyForwardingFunction13",
    "NdrProxyForwardingFunction14",
    "NdrProxyForwardingFunction15",
    "NdrProxyForwardingFunction16",
    "NdrProxyForwardingFunction17",
    "NdrProxyForwardingFunction18",
    "NdrProxyForwardingFunction19",
    "NdrProxyForwardingFunction20",
    "NdrProxyForwardingFunction21",
    "NdrProxyForwardingFunction22",
    "NdrProxyForwardingFunction23",
    "NdrProxyForwardingFunction24",
    "NdrProxyForwardingFunction25",
    "NdrProxyForwardingFunction26",
    "NdrProxyForwardingFunction27",
    "NdrProxyForwardingFunction28",
    "NdrProxyForwardingFunction29",
    "NdrProxyForwardingFunction3",
    "NdrProxyForwardingFunction30",
    "NdrProxyForwardingFunction31",
    "NdrProxyForwardingFunction32",
    "NdrProxyForwardingFunction4",
    "NdrProxyForwardingFunction5",
    "NdrProxyForwardingFunction6",
    "NdrProxyForwardingFunction7",
    "NdrProxyForwardingFunction8",
    "NdrProxyForwardingFunction9",
    "ObjectStublessClient10",
    "ObjectStublessClient11",
    "ObjectStublessClient12",
    "ObjectStublessClient13",
    "ObjectStublessClient14",
    "ObjectStublessClient15",
    "ObjectStublessClient16",
    "ObjectStublessClient17",
    "ObjectStublessClient18",
    "ObjectStublessClient19",
    "ObjectStublessClient20",
    "ObjectStublessClient21",
    "ObjectStublessClient22",
    "ObjectStublessClient23",
    "ObjectStublessClient24",
    "ObjectStublessClient25",
    "ObjectStublessClient26",
    "ObjectStublessClient27",
    "ObjectStublessClient28",
    "ObjectStublessClient29",
    "ObjectStublessClient3",
    "ObjectStublessClient30",
    "ObjectStublessClient31",
    "ObjectStublessClient32",
    "ObjectStublessClient4",
    "ObjectStublessClient5",
    "ObjectStublessClient6",
    "ObjectStublessClient7",
    "ObjectStublessClient8",
    "ObjectStublessClient9",
};

static const CONTRACT_HOST Contracts[] =
{
    { L"api-ms-win-security-base-l1-1-0.dll", L"kernelbase.dll", SecurityBaseFunctions, RTL_NUMBER_OF(SecurityBaseFunctions) },
    { L"api-ms-win-core-stringansi-l1-1-0.dll", L"kernelbase.dll", StringAnsiFunctions, RTL_NUMBER_OF(StringAnsiFunctions) },
    { L"api-ms-win-shcore-unicodeansi-l1-1-0.dll", L"shcore.dll", UnicodeAnsiFunctions, RTL_NUMBER_OF(UnicodeAnsiFunctions) },
    { L"api-ms-win-core-com-midlproxystub-l1-1-0.dll", L"combase.dll", MidlProxyStubFunctions, RTL_NUMBER_OF(MidlProxyStubFunctions) },
};

static void
test_contract(const CONTRACT_HOST *Entry)
{
    HMODULE Contract, Host;
    ULONG Index, Missing = 0;

    winetest_push_context("%S", Entry->Contract);

    Host = LoadLibraryW(Entry->Host);
    ok(Host != NULL, "LoadLibrary(%S) failed: %lu\n", Entry->Host, GetLastError());

    Contract = LoadLibraryW(Entry->Contract);
    ok(Contract != NULL, "LoadLibrary failed: %lu\n", GetLastError());
    if (Contract == NULL)
    {
        winetest_pop_context();
        if (Host)
            FreeLibrary(Host);
        return;
    }

    ok(Contract == Host, "resolved to %p, expected %S at %p\n", Contract, Entry->Host, Host);

    for (Index = 0; Index < Entry->Count; Index++)
    {
        if (GetProcAddress(Contract, Entry->Functions[Index]) == NULL)
        {
            ok(0, "%s is not exported\n", Entry->Functions[Index]);
            Missing++;
        }
    }
    ok(Missing == 0, "%lu of %lu functions are not exported\n", Missing, Entry->Count);

    FreeLibrary(Contract);
    if (Host)
        FreeLibrary(Host);
    winetest_pop_context();
}

typedef LPSTR (WINAPI *PCHARLOWERA)(LPSTR);
typedef INT (WINAPI *PSHANSITOUNICODE)(LPCSTR, LPWSTR, INT);

static void
test_calls(void)
{
    HMODULE Module;
    PCHARLOWERA pCharLowerA;
    PSHANSITOUNICODE pSHAnsiToUnicode;
    CHAR Text[] = "OROSI";
    WCHAR Wide[8];

    Module = LoadLibraryW(L"api-ms-win-core-stringansi-l1-1-0.dll");
    pCharLowerA = Module ? (PCHARLOWERA)GetProcAddress(Module, "CharLowerA") : NULL;
    if (pCharLowerA)
    {
        pCharLowerA(Text);
        ok(lstrcmpA(Text, "orosi") == 0, "CharLowerA gave %s\n", Text);
    }
    else
    {
        skip("CharLowerA is not available\n");
    }
    if (Module)
        FreeLibrary(Module);

    Module = LoadLibraryW(L"api-ms-win-shcore-unicodeansi-l1-1-0.dll");
    pSHAnsiToUnicode = Module ? (PSHANSITOUNICODE)GetProcAddress(Module, "SHAnsiToUnicode") : NULL;
    if (pSHAnsiToUnicode)
    {
        ok(pSHAnsiToUnicode("Orosi", Wide, RTL_NUMBER_OF(Wide)) == 6, "SHAnsiToUnicode returned the wrong length\n");
        ok(lstrcmpW(Wide, L"Orosi") == 0, "SHAnsiToUnicode gave %S\n", Wide);
    }
    else
    {
        skip("SHAnsiToUnicode is not available\n");
    }
    if (Module)
        FreeLibrary(Module);
}

START_TEST(apisethosts)
{
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(Contracts); Index++)
        test_contract(&Contracts[Index]);
    test_calls();
}
