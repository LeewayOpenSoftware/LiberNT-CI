/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests credential provider and filter enumeration in credprovhost
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS

#include <apitest.h>
#include <objbase.h>
#include <aclapi.h>
#include <sddl.h>
#include <credentialprovider.h>
#include <credprovhost.h>
#include <initguid.h>
#include "testmod/testmod.h"

typedef HRESULT (WINAPI *PFN_ENUMERATE)(CREDENTIAL_PROVIDER_USAGE_SCENARIO, DWORD, DWORD, PCPH_PROVIDER_SET *);
typedef VOID (WINAPI *PFN_FREE)(PCPH_PROVIDER_SET);

static PFN_ENUMERATE pEnumerate;
static PFN_FREE pFree;

#define AUTH_KEY L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication"
#define PROVIDERS_KEY AUTH_KEY L"\\Credential Providers"
#define FILTERS_KEY AUTH_KEY L"\\Credential Provider Filters"
#define EXCLUDE_KEY L"SOFTWARE\\Policies\\Microsoft\\Windows\\System"

static VOID
ClsidKey(_In_ PCWSTR Parent, _In_ REFCLSID Clsid, _Out_writes_(Chars) LPWSTR Out, _In_ DWORD Chars)
{
    WCHAR Text[40];

    StringFromGUID2(Clsid, Text, ARRAYSIZE(Text));
    _snwprintf(Out, Chars, L"%s\\%s", Parent, Text);
    Out[Chars - 1] = UNICODE_NULL;
}

static BOOL
RegisterServer(_In_ REFCLSID Clsid, _In_ PCWSTR Module)
{
    WCHAR Key[MAX_PATH], Path[MAX_PATH];
    HKEY Handle;
    LONG Error;

    if (Module[1] != L':')
    {
        GetModuleFileNameW(NULL, Path, ARRAYSIZE(Path));
        wcscpy(wcsrchr(Path, L'\\') + 1, Module);
        Module = Path;
    }
    ClsidKey(L"SOFTWARE\\Classes\\CLSID", Clsid, Key, ARRAYSIZE(Key));
    wcscat(Key, L"\\InprocServer32");
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Key, 0, NULL, 0, KEY_SET_VALUE, NULL, &Handle, NULL);
    if (Error)
        return FALSE;
    RegSetValueExW(Handle, NULL, 0, REG_SZ, (const BYTE *)Module, (DWORD)(wcslen(Module) + 1) * sizeof(WCHAR));
    RegSetValueExW(Handle, L"ThreadingModel", 0, REG_SZ, (const BYTE *)L"Apartment", sizeof(L"Apartment"));
    RegCloseKey(Handle);
    return TRUE;
}

static VOID
UnregisterServer(_In_ REFCLSID Clsid)
{
    WCHAR Key[MAX_PATH];

    ClsidKey(L"SOFTWARE\\Classes\\CLSID", Clsid, Key, ARRAYSIZE(Key));
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, Key);
}

static BOOL
AddEntry(_In_ HKEY Root, _In_ PCWSTR Parent, _In_ REFCLSID Clsid, _In_ DWORD Disabled)
{
    WCHAR Key[MAX_PATH];
    HKEY Handle;

    ClsidKey(Parent, Clsid, Key, ARRAYSIZE(Key));
    if (RegCreateKeyExW(Root, Key, 0, NULL, 0, KEY_SET_VALUE, NULL, &Handle, NULL))
        return FALSE;
    if (Disabled)
        RegSetValueExW(Handle, L"Disabled", 0, REG_DWORD, (const BYTE *)&Disabled, sizeof(Disabled));
    RegCloseKey(Handle);
    return TRUE;
}

static VOID
RemoveEntry(_In_ HKEY Root, _In_ PCWSTR Parent, _In_ REFCLSID Clsid)
{
    WCHAR Key[MAX_PATH];

    ClsidKey(Parent, Clsid, Key, ARRAYSIZE(Key));
    RegDeleteTreeW(Root, Key);
}

static ULONG
FindProvider(_In_ PCPH_PROVIDER_SET Set, _In_ REFCLSID Clsid)
{
    ULONG i;

    for (i = 0; i < Set->Count; i++)
    {
        if (IsEqualGUID(&Set->Providers[i].Clsid, Clsid))
            return i;
    }
    return ~0u;
}

static ULONG
SkipReason(_In_ PCPH_PROVIDER_SET Set, _In_ REFCLSID Clsid)
{
    ULONG i;

    for (i = 0; i < Set->SkippedCount; i++)
    {
        if (IsEqualGUID(&Set->Skipped[i].Clsid, Clsid))
            return Set->Skipped[i].Reason;
    }
    return 0;
}

static PCPH_PROVIDER_SET
Enumerate(_In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario, _In_ DWORD HostFlags)
{
    PCPH_PROVIDER_SET Set = NULL;

    ok_hr(pEnumerate(Scenario, 0, HostFlags, &Set), S_OK);
    return Set;
}

static BOOL
PasswordProviderRegistered(void)
{
    WCHAR Key[MAX_PATH];
    HKEY Handle;

    ClsidKey(PROVIDERS_KEY, &CLSID_PasswordCredentialProvider, Key, ARRAYSIZE(Key));
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Key, 0, KEY_QUERY_VALUE, &Handle))
        return FALSE;
    RegCloseKey(Handle);
    return TRUE;
}

static VOID
TestEnumeration(void)
{
    PCPH_PROVIDER_SET Set;
    BOOL HavePassword = PasswordProviderRegistered();

    Set = Enumerate(CPUS_LOGON, 0);
    if (Set)
    {
        ok(FindProvider(Set, &CLSID_CredProvTestProvider) != ~0u, "the test provider was not enumerated\n");
        ok(FindProvider(Set, &CLSID_CredProvTestDependent) == ~0u, "the provider with a missing DLL was loaded\n");
        ok_long(SkipReason(Set, &CLSID_CredProvTestDependent), CPH_SKIP_LOAD_FAILED);
        ok_long(SkipReason(Set, &CLSID_CredProvTestMissing), CPH_SKIP_LOAD_FAILED);
        ok(FindProvider(Set, &CLSID_CredProvTestUserOnly) == ~0u, "a per-user provider was enumerated\n");
        ok_long(SkipReason(Set, &CLSID_CredProvTestUserOnly), 0);
        ok(!HavePassword || FindProvider(Set, &CLSID_PasswordCredentialProvider) != ~0u, "no PasswordProvider\n");
        pFree(Set);
    }

    Set = Enumerate(CPUS_CREDUI, 0);
    if (Set)
    {
        ok_long(SkipReason(Set, &CLSID_CredProvTestProvider), CPH_SKIP_NO_SCENARIO);
        pFree(Set);
    }

    Set = Enumerate(CPUS_LOGON, CPH_FLAG_FALLBACK);
    if (Set)
    {
        ok_long(Set->Count, HavePassword ? 1 : 0);
        ok(!HavePassword || FindProvider(Set, &CLSID_PasswordCredentialProvider) == 0, "fallback is not PasswordProvider\n");
        ok(FindProvider(Set, &CLSID_CredProvTestProvider) == ~0u, "fallback loaded a third-party provider\n");
        ok(!Set->PolicyDeniesPassword, "no policy denies the password\n");
        pFree(Set);
    }

    AddEntry(HKEY_LOCAL_MACHINE, PROVIDERS_KEY, &CLSID_CredProvTestProvider, 1);
    Set = Enumerate(CPUS_LOGON, 0);
    if (Set)
    {
        ok_long(SkipReason(Set, &CLSID_CredProvTestProvider), CPH_SKIP_DISABLED);
        pFree(Set);
    }
    RemoveEntry(HKEY_LOCAL_MACHINE, PROVIDERS_KEY, &CLSID_CredProvTestProvider);
    AddEntry(HKEY_LOCAL_MACHINE, PROVIDERS_KEY, &CLSID_CredProvTestProvider, 0);

    if (AddEntry(HKEY_LOCAL_MACHINE, FILTERS_KEY, &CLSID_CredProvTestFilter, 0))
    {
        Set = Enumerate(CPUS_LOGON, 0);
        if (Set)
        {
            ok_long(SkipReason(Set, &CLSID_CredProvTestProvider), CPH_SKIP_FILTERED);
            ok(!HavePassword || FindProvider(Set, &CLSID_PasswordCredentialProvider) != ~0u,
               "the filter must only hide the test provider\n");
            pFree(Set);
        }
        Set = Enumerate(CPUS_LOGON, CPH_FLAG_FALLBACK);
        if (Set)
        {
            ok_long(SkipReason(Set, &CLSID_CredProvTestProvider), 0);
            pFree(Set);
        }
        RemoveEntry(HKEY_LOCAL_MACHINE, FILTERS_KEY, &CLSID_CredProvTestFilter);
    }
}

static VOID
TestPolicy(void)
{
    WCHAR Old[4096], Value[64];
    DWORD OldSize = sizeof(Old), OldType = 0;
    BOOL HadOld;
    PCPH_PROVIDER_SET Set;
    HKEY Key;

    if (!PasswordProviderRegistered())
    {
        skip("PasswordProvider is not registered\n");
        return;
    }
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, EXCLUDE_KEY, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &Key, NULL))
    {
        skip("cannot write the exclusion policy\n");
        return;
    }
    HadOld = !RegQueryValueExW(Key, L"ExcludedCredentialProviders", NULL, &OldType, (PBYTE)Old, &OldSize);
    StringFromGUID2(&CLSID_PasswordCredentialProvider, Value, ARRAYSIZE(Value));
    RegSetValueExW(Key, L"ExcludedCredentialProviders", 0, REG_SZ, (const BYTE *)Value,
                   (DWORD)(wcslen(Value) + 1) * sizeof(WCHAR));

    Set = Enumerate(CPUS_LOGON, CPH_FLAG_FALLBACK);
    if (Set)
    {
        ok_long(Set->Count, 0);
        ok(Set->PolicyDeniesPassword, "the policy must deny the password in fallback\n");
        ok_long(SkipReason(Set, &CLSID_PasswordCredentialProvider), CPH_SKIP_EXCLUDED);
        pFree(Set);
    }
    Set = Enumerate(CPUS_LOGON, 0);
    if (Set)
    {
        ok(FindProvider(Set, &CLSID_PasswordCredentialProvider) == ~0u, "an excluded provider was loaded\n");
        ok(FindProvider(Set, &CLSID_CredProvTestProvider) != ~0u, "exclusion must not hide other providers\n");
        pFree(Set);
    }

    if (HadOld)
        RegSetValueExW(Key, L"ExcludedCredentialProviders", 0, OldType, (const BYTE *)Old, OldSize);
    else
        RegDeleteValueW(Key, L"ExcludedCredentialProviders");
    RegCloseKey(Key);
}

static BOOL
AceAllowsWrite(_In_ PACL Dacl, _In_ PSID Sid)
{
    ACCESS_ALLOWED_ACE *Ace;
    DWORD i;

    for (i = 0; Dacl && i < Dacl->AceCount; i++)
    {
        if (!GetAce(Dacl, i, (void **)&Ace) || Ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE)
            continue;
        if (EqualSid((PSID)&Ace->SidStart, Sid) &&
            (Ace->Mask & (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | WRITE_DAC | WRITE_OWNER | DELETE | GENERIC_WRITE |
                          GENERIC_ALL)))
        {
            return TRUE;
        }
    }
    return FALSE;
}

static VOID
TestRegistryAcl(void)
{
    static const PCWSTR Sids[] = { L"S-1-5-32-545", L"S-1-5-11", L"S-1-1-0", L"S-1-5-4" };
    PSECURITY_DESCRIPTOR Sd = NULL;
    PACL Dacl = NULL;
    PSID Sid;
    ULONG i;
    DWORD Error;

    Error = GetNamedSecurityInfoW(L"MACHINE\\" PROVIDERS_KEY, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL,
                                  &Dacl, NULL, &Sd);
    ok_long(Error, ERROR_SUCCESS);
    if (Error)
        return;
    for (i = 0; i < ARRAYSIZE(Sids); i++)
    {
        if (!ConvertStringSidToSidW(Sids[i], &Sid))
            continue;
        ok(!AceAllowsWrite(Dacl, Sid), "%ls may write the credential provider list\n", Sids[i]);
        LocalFree(Sid);
    }
    LocalFree(Sd);
}

START_TEST(Framework)
{
    WCHAR Path[MAX_PATH];
    HMODULE Host;
    HRESULT hr;
    BOOL Registered;

    if (!GetEnvironmentVariableW(L"CREDPROV_TEST_CREDPROVHOST", Path, ARRAYSIZE(Path)))
        wcscpy(Path, L"credprovhost.dll");
    Host = LoadLibraryW(Path);
    pEnumerate = Host ? (PFN_ENUMERATE)GetProcAddress(Host, "CredProvHostEnumerateProviders") : NULL;
    pFree = Host ? (PFN_FREE)GetProcAddress(Host, "CredProvHostFreeProviders") : NULL;
    if (!pEnumerate || !pFree)
    {
        skip("credprovhost.dll is not available\n");
        return;
    }
    trace("testing %ls\n", Path);
    TestRegistryAcl();

    Registered = RegisterServer(&CLSID_CredProvTestProvider, L"credprov_testmod.dll") &&
                 RegisterServer(&CLSID_CredProvTestFilter, L"credprov_testmod.dll") &&
                 RegisterServer(&CLSID_CredProvTestDependent, L"credprov_testdep.dll") &&
                 RegisterServer(&CLSID_CredProvTestMissing, L"credprov_notthere.dll") &&
                 RegisterServer(&CLSID_CredProvTestUserOnly, L"credprov_testmod.dll") &&
                 AddEntry(HKEY_LOCAL_MACHINE, PROVIDERS_KEY, &CLSID_CredProvTestProvider, 0) &&
                 AddEntry(HKEY_LOCAL_MACHINE, PROVIDERS_KEY, &CLSID_CredProvTestDependent, 0) &&
                 AddEntry(HKEY_LOCAL_MACHINE, PROVIDERS_KEY, &CLSID_CredProvTestMissing, 0) &&
                 AddEntry(HKEY_CURRENT_USER, PROVIDERS_KEY, &CLSID_CredProvTestUserOnly, 0);
    if (!Registered)
    {
        skip("registering the test providers needs administrator rights\n");
    }
    else
    {
        hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        ok_hr(hr, S_OK);
        TestEnumeration();
        TestPolicy();
        if (SUCCEEDED(hr))
            CoUninitialize();
    }

    RemoveEntry(HKEY_LOCAL_MACHINE, PROVIDERS_KEY, &CLSID_CredProvTestProvider);
    RemoveEntry(HKEY_LOCAL_MACHINE, PROVIDERS_KEY, &CLSID_CredProvTestDependent);
    RemoveEntry(HKEY_LOCAL_MACHINE, PROVIDERS_KEY, &CLSID_CredProvTestMissing);
    RemoveEntry(HKEY_LOCAL_MACHINE, FILTERS_KEY, &CLSID_CredProvTestFilter);
    RemoveEntry(HKEY_CURRENT_USER, PROVIDERS_KEY, &CLSID_CredProvTestUserOnly);
    UnregisterServer(&CLSID_CredProvTestProvider);
    UnregisterServer(&CLSID_CredProvTestFilter);
    UnregisterServer(&CLSID_CredProvTestDependent);
    UnregisterServer(&CLSID_CredProvTestMissing);
    UnregisterServer(&CLSID_CredProvTestUserOnly);
    FreeLibrary(Host);
}
