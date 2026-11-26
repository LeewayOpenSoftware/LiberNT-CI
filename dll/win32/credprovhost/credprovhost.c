/*
 * PROJECT:     LiberNT Credential Provider Host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Enumerates credential providers and filters, with the fallback mode
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS
#define CONST_VTABLE

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <objbase.h>
#include <credentialprovider.h>
#include <credprovhost.h>
#include <wine/debug.h>
#include <wchar.h>
#include <strsafe.h>

WINE_DEFAULT_DEBUG_CHANNEL(credprovhost);

static const WCHAR ProvidersKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers";
static const WCHAR FiltersKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Provider Filters";
static const WCHAR ExcludePolicyKey[] = L"SOFTWARE\\Policies\\Microsoft\\Windows\\System";
static const WCHAR SystemPolicyKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";

typedef struct _CPH_CANDIDATES
{
    ULONG Count;
    CLSID Clsid[CPH_MAX_PROVIDERS];
    BOOL Allow[CPH_MAX_PROVIDERS];
} CPH_CANDIDATES;

static BOOL
IsSubkeyDisabled(_In_ HKEY Parent, _In_ PCWSTR Name)
{
    DWORD Disabled = 0, Size = sizeof(Disabled), Type;
    HKEY Key;
    BOOL Ret = FALSE;

    if (RegOpenKeyExW(Parent, Name, 0, KEY_QUERY_VALUE, &Key) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(Key, L"Disabled", NULL, &Type, (PBYTE)&Disabled, &Size) == ERROR_SUCCESS &&
        Type == REG_DWORD && Disabled)
    {
        Ret = TRUE;
    }
    RegCloseKey(Key);
    return Ret;
}

static BOOL
IsPolicyExcluded(_In_ REFCLSID Clsid)
{
    WCHAR *List = NULL, *Item, *End, Saved;
    DWORD Size = 0, Type, Error;
    CLSID Excluded;
    HKEY Key;
    BOOL Ret = FALSE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ExcludePolicyKey, 0, KEY_QUERY_VALUE, &Key) != ERROR_SUCCESS)
        return FALSE;
    Error = RegQueryValueExW(Key, L"ExcludedCredentialProviders", NULL, &Type, NULL, &Size);
    while (Error == ERROR_SUCCESS || Error == ERROR_MORE_DATA)
    {
        HeapFree(GetProcessHeap(), 0, List);
        List = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size + 2 * sizeof(WCHAR));
        if (!List)
            break;
        Error = RegQueryValueExW(Key, L"ExcludedCredentialProviders", NULL, &Type, (PBYTE)List, &Size);
        if (Error == ERROR_SUCCESS)
            break;
    }
    if (List && Error == ERROR_SUCCESS && (Type == REG_SZ || Type == REG_EXPAND_SZ))
    {
        for (Item = List; *Item && !Ret; Item = End)
        {
            while (*Item && wcschr(L",; \t", *Item))
                Item++;
            for (End = Item; *End && !wcschr(L",; \t", *End); End++)
                ;
            Saved = *End;
            *End = UNICODE_NULL;
            if (*Item && SUCCEEDED(CLSIDFromString(Item, &Excluded)) && IsEqualGUID(&Excluded, Clsid))
                Ret = TRUE;
            *End = Saved;
        }
    }
    HeapFree(GetProcessHeap(), 0, List);
    RegCloseKey(Key);
    return Ret;
}

static BOOL
IsSmartCardRequired(void)
{
    DWORD Value = 0, Size = sizeof(Value), Type;
    HKEY Key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SystemPolicyKey, 0, KEY_QUERY_VALUE, &Key) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(Key, L"scforceoption", NULL, &Type, (PBYTE)&Value, &Size) != ERROR_SUCCESS ||
        Type != REG_DWORD)
    {
        Value = 0;
    }
    RegCloseKey(Key);
    return Value != 0;
}

static BOOL
PolicyDeniesPassword(_In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario)
{
    if (IsPolicyExcluded(&CLSID_PasswordCredentialProvider))
        return TRUE;
    return (Scenario == CPUS_LOGON || Scenario == CPUS_UNLOCK_WORKSTATION) && IsSmartCardRequired();
}

static VOID
AddSkipped(_Inout_ PCPH_PROVIDER_SET Set, _In_ REFCLSID Clsid, _In_ ULONG Reason, _In_ HRESULT Result)
{
    WARN("skipping provider %s: reason %#lx, %#lx\n", debugstr_guid(Clsid), Reason, Result);
    if (Set->SkippedCount < CPH_MAX_PROVIDERS)
    {
        Set->Skipped[Set->SkippedCount].Clsid = *Clsid;
        Set->Skipped[Set->SkippedCount].Reason = Reason;
        Set->Skipped[Set->SkippedCount].Result = Result;
        Set->SkippedCount++;
    }
}

static VOID
ReadCandidates(_Inout_ PCPH_PROVIDER_SET Set, _Out_ CPH_CANDIDATES *Candidates)
{
    WCHAR Name[64];
    DWORD Index, Chars, Error;
    CLSID Clsid;
    HKEY Key;

    ZeroMemory(Candidates, sizeof(*Candidates));
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ProvidersKey, 0, KEY_ENUMERATE_SUB_KEYS, &Key) != ERROR_SUCCESS)
        return;
    for (Index = 0; Candidates->Count < CPH_MAX_PROVIDERS; Index++)
    {
        Chars = ARRAYSIZE(Name);
        Error = RegEnumKeyExW(Key, Index, Name, &Chars, NULL, NULL, NULL, NULL);
        if (Error == ERROR_NO_MORE_ITEMS)
            break;
        if (Error != ERROR_SUCCESS)
        {
            WARN("skipping subkey %lu: %lu\n", Index, Error);
            continue;
        }
        if (FAILED(CLSIDFromString(Name, &Clsid)))
            continue;
        if (IsSubkeyDisabled(Key, Name))
        {
            AddSkipped(Set, &Clsid, CPH_SKIP_DISABLED, S_OK);
            continue;
        }
        if (IsPolicyExcluded(&Clsid))
        {
            AddSkipped(Set, &Clsid, CPH_SKIP_EXCLUDED, S_OK);
            continue;
        }
        Candidates->Clsid[Candidates->Count] = Clsid;
        Candidates->Allow[Candidates->Count] = TRUE;
        Candidates->Count++;
    }
    RegCloseKey(Key);
}

static ULONG
LoadFilters(_Out_writes_(Max) ICredentialProviderFilter **Filters, _In_ ULONG Max)
{
    WCHAR Name[64];
    DWORD Index, Chars, Error;
    ULONG Count = 0;
    CLSID Clsid;
    HKEY Key;
    HRESULT hr;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, FiltersKey, 0, KEY_ENUMERATE_SUB_KEYS, &Key) != ERROR_SUCCESS)
        return 0;
    for (Index = 0; Count < Max; Index++)
    {
        Chars = ARRAYSIZE(Name);
        Error = RegEnumKeyExW(Key, Index, Name, &Chars, NULL, NULL, NULL, NULL);
        if (Error == ERROR_NO_MORE_ITEMS)
            break;
        if (Error != ERROR_SUCCESS)
        {
            WARN("skipping subkey %lu: %lu\n", Index, Error);
            continue;
        }
        if (FAILED(CLSIDFromString(Name, &Clsid)) || IsSubkeyDisabled(Key, Name))
            continue;
        hr = CoCreateInstance(&Clsid, NULL, CLSCTX_INPROC_SERVER, &IID_ICredentialProviderFilter,
                              (void **)&Filters[Count]);
        if (FAILED(hr))
        {
            WARN("skipping filter %s: %#lx\n", debugstr_guid(&Clsid), hr);
            continue;
        }
        Count++;
    }
    RegCloseKey(Key);
    return Count;
}

static VOID
ApplyFilters(_Inout_ PCPH_PROVIDER_SET Set, _Inout_ CPH_CANDIDATES *Candidates,
             _In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario, _In_ DWORD ScenarioFlags)
{
    ICredentialProviderFilter *Filters[CPH_MAX_PROVIDERS];
    BOOL Allow[CPH_MAX_PROVIDERS];
    CLSID Clsids[CPH_MAX_PROVIDERS];
    ULONG FilterCount, i, j;

    if (!Candidates->Count)
        return;
    FilterCount = LoadFilters(Filters, ARRAYSIZE(Filters));
    for (i = 0; i < FilterCount; i++)
    {
        CopyMemory(Allow, Candidates->Allow, Candidates->Count * sizeof(BOOL));
        CopyMemory(Clsids, Candidates->Clsid, Candidates->Count * sizeof(CLSID));
        if (SUCCEEDED(ICredentialProviderFilter_Filter(Filters[i], Scenario, ScenarioFlags, Clsids,
                                                       Allow, Candidates->Count)))
        {
            for (j = 0; j < Candidates->Count; j++)
                Candidates->Allow[j] = Candidates->Allow[j] && Allow[j];
        }
        ICredentialProviderFilter_Release(Filters[i]);
    }
    for (j = 0; j < Candidates->Count; j++)
    {
        if (!Candidates->Allow[j])
            AddSkipped(Set, &Candidates->Clsid[j], CPH_SKIP_FILTERED, S_OK);
    }
}

/* The fallback tier must survive a damaged class registration: load the system
 * PasswordProvider by absolute path and ask its class factory directly (Q5). */
static HRESULT
CreatePasswordProviderDirect(_Outptr_ ICredentialProvider **Provider)
{
    typedef HRESULT (WINAPI *GETCLASSOBJECT)(REFCLSID, REFIID, void **);
    static HMODULE Module;      /* kept loaded for the life of the process */
    WCHAR Path[MAX_PATH];
    UINT Length;
    GETCLASSOBJECT GetClassObject;
    IClassFactory *Factory = NULL;
    HRESULT hr;

    *Provider = NULL;
    Length = GetSystemDirectoryW(Path, ARRAYSIZE(Path));
    if (Length == 0 || Length >= ARRAYSIZE(Path) ||
        FAILED(StringCchCatW(Path, ARRAYSIZE(Path), L"\\credprovs.dll")))
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);

    if (Module == NULL)
        Module = LoadLibraryExW(Path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (Module == NULL)
        return HRESULT_FROM_WIN32(GetLastError());

    GetClassObject = (GETCLASSOBJECT)GetProcAddress(Module, "DllGetClassObject");
    if (GetClassObject == NULL)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    hr = GetClassObject(&CLSID_PasswordCredentialProvider, &IID_IClassFactory, (void **)&Factory);
    if (FAILED(hr))
        return hr;
    hr = IClassFactory_CreateInstance(Factory, NULL, &IID_ICredentialProvider, (void **)Provider);
    IClassFactory_Release(Factory);
    return hr;
}

static VOID
Instantiate(_Inout_ PCPH_PROVIDER_SET Set, _In_ REFCLSID Clsid,
            _In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario, _In_ DWORD ScenarioFlags,
            _In_ BOOL Direct)
{
    ICredentialProvider *Provider;
    HRESULT hr;

    if (Set->Count >= CPH_MAX_PROVIDERS)
        return;
    if (Direct)
        hr = CreatePasswordProviderDirect(&Provider);
    else
        hr = CoCreateInstance(Clsid, NULL, CLSCTX_INPROC_SERVER, &IID_ICredentialProvider, (void **)&Provider);
    if (FAILED(hr))
    {
        AddSkipped(Set, Clsid, CPH_SKIP_LOAD_FAILED, hr);
        return;
    }
    hr = ICredentialProvider_SetUsageScenario(Provider, Scenario, ScenarioFlags);
    if (FAILED(hr))
    {
        ICredentialProvider_Release(Provider);
        AddSkipped(Set, Clsid, CPH_SKIP_NO_SCENARIO, hr);
        return;
    }
    Set->Providers[Set->Count].Clsid = *Clsid;
    Set->Providers[Set->Count].Provider = Provider;
    Set->Count++;
}

HRESULT WINAPI
CredProvHostEnumerateProviders(
    _In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario,
    _In_ DWORD ScenarioFlags,
    _In_ DWORD HostFlags,
    _Outptr_ PCPH_PROVIDER_SET *Result)
{
    CPH_CANDIDATES Candidates;
    PCPH_PROVIDER_SET Set;
    ULONG i;

    TRACE("(%d, %08lx, %08lx, %p)\n", Scenario, ScenarioFlags, HostFlags, Result);

    if (!Result)
        return E_POINTER;
    *Result = NULL;
    if (HostFlags & ~CPH_FLAG_FALLBACK)
        return E_INVALIDARG;

    Set = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Set));
    if (!Set)
        return E_OUTOFMEMORY;
    Set->PolicyDeniesPassword = PolicyDeniesPassword(Scenario);

    if (HostFlags & CPH_FLAG_FALLBACK)
    {
        /* TODO: honour the ProhibitFallbacks policy once its storage is known (INFO_NEEDED). */
        if (Set->PolicyDeniesPassword)
            AddSkipped(Set, &CLSID_PasswordCredentialProvider, CPH_SKIP_EXCLUDED, S_OK);
        else
            Instantiate(Set, &CLSID_PasswordCredentialProvider, Scenario, ScenarioFlags, TRUE);
        *Result = Set;
        return S_OK;
    }

    ReadCandidates(Set, &Candidates);
    ApplyFilters(Set, &Candidates, Scenario, ScenarioFlags);
    for (i = 0; i < Candidates.Count; i++)
    {
        if (!Candidates.Allow[i])
            continue;
        if (Set->PolicyDeniesPassword && IsEqualGUID(&Candidates.Clsid[i], &CLSID_PasswordCredentialProvider))
        {
            AddSkipped(Set, &Candidates.Clsid[i], CPH_SKIP_EXCLUDED, S_OK);
            continue;
        }
        Instantiate(Set, &Candidates.Clsid[i], Scenario, ScenarioFlags, FALSE);
    }
    *Result = Set;
    return S_OK;
}

VOID WINAPI
CredProvHostFreeProviders(
    _In_opt_ _Post_invalid_ PCPH_PROVIDER_SET Set)
{
    ULONG i;

    if (!Set)
        return;
    for (i = 0; i < Set->Count; i++)
        ICredentialProvider_Release(Set->Providers[i].Provider);
    HeapFree(GetProcessHeap(), 0, Set);
}

HRESULT WINAPI
CredProvHostUpdateRemoteCredential(
    _In_ const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *In,
    _Out_ CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *Out)
{
    ICredentialProviderFilter *Filters[CPH_MAX_PROVIDERS];
    ULONG FilterCount, i;
    HRESULT hr = S_FALSE;

    if (!In || !Out)
        return E_POINTER;
    ZeroMemory(Out, sizeof(*Out));
    FilterCount = LoadFilters(Filters, ARRAYSIZE(Filters));
    for (i = 0; i < FilterCount; i++)
    {
        if (hr == S_FALSE && ICredentialProviderFilter_UpdateRemoteCredential(Filters[i], In, Out) == S_OK)
            hr = S_OK;
        ICredentialProviderFilter_Release(Filters[i]);
    }
    return hr;
}
