/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Third-party credential provider and filter used by the credprov framework tests
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS
#define CONST_VTABLE

#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <credentialprovider.h>
#include <initguid.h>
#include "testmod.h"

typedef struct _TEST_PROVIDER
{
    ICredentialProvider ICredentialProvider_iface;
    LONG Ref;
} TEST_PROVIDER;

typedef struct _TEST_FILTER
{
    ICredentialProviderFilter ICredentialProviderFilter_iface;
    LONG Ref;
} TEST_FILTER;

static HRESULT WINAPI
Provider_QueryInterface(ICredentialProvider *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ICredentialProvider))
    {
        *ppv = iface;
        ICredentialProvider_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Provider_AddRef(ICredentialProvider *iface)
{
    return InterlockedIncrement(&CONTAINING_RECORD(iface, TEST_PROVIDER, ICredentialProvider_iface)->Ref);
}

static ULONG WINAPI
Provider_Release(ICredentialProvider *iface)
{
    TEST_PROVIDER *This = CONTAINING_RECORD(iface, TEST_PROVIDER, ICredentialProvider_iface);
    ULONG Ref = InterlockedDecrement(&This->Ref);
    if (!Ref)
        HeapFree(GetProcessHeap(), 0, This);
    return Ref;
}

static HRESULT WINAPI
Provider_SetUsageScenario(ICredentialProvider *iface, CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD dwFlags)
{
    return (cpus == CPUS_LOGON || cpus == CPUS_UNLOCK_WORKSTATION) ? S_OK : E_NOTIMPL;
}

static HRESULT WINAPI
Provider_SetSerialization(ICredentialProvider *iface, const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcs)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI
Provider_Advise(ICredentialProvider *iface, ICredentialProviderEvents *pcpe, UINT_PTR upAdviseContext)
{
    return S_OK;
}

static HRESULT WINAPI
Provider_UnAdvise(ICredentialProvider *iface)
{
    return S_OK;
}

/*
 * A credential exists only when CREDPROV_TESTMOD_MODE is set in the environment, so the
 * framework tests keep seeing a provider with no tiles. Modes: ok (asks for more input),
 * hang (GetSerialization never returns), crash (GetSerialization faults).
 */
static const WCHAR *
TestMode(void)
{
    static WCHAR Mode[16];
    if (!GetEnvironmentVariableW(L"CREDPROV_TESTMOD_MODE", Mode, ARRAYSIZE(Mode)))
        Mode[0] = 0;
    return Mode;
}

typedef struct _TEST_CREDENTIAL
{
    ICredentialProviderCredential ICredentialProviderCredential_iface;
    LONG Ref;
} TEST_CREDENTIAL;

static const struct { DWORD Id; CREDENTIAL_PROVIDER_FIELD_TYPE Type; const WCHAR *Label; } TestFields[] =
{
    { 1, CPFT_EDIT_TEXT,     L"User name" },
    { 2, CPFT_PASSWORD_TEXT, L"Password" },
    { 3, CPFT_SUBMIT_BUTTON, L"Submit" },
};

static LPWSTR
DupString(const WCHAR *Text)
{
    SIZE_T Bytes = (wcslen(Text) + 1) * sizeof(WCHAR);
    LPWSTR Copy = CoTaskMemAlloc(Bytes);
    if (Copy)
        CopyMemory(Copy, Text, Bytes);
    return Copy;
}

static HRESULT WINAPI
Credential_QueryInterface(ICredentialProviderCredential *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ICredentialProviderCredential))
    {
        *ppv = iface;
        ICredentialProviderCredential_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Credential_AddRef(ICredentialProviderCredential *iface)
{
    return InterlockedIncrement(&CONTAINING_RECORD(iface, TEST_CREDENTIAL, ICredentialProviderCredential_iface)->Ref);
}

static ULONG WINAPI
Credential_Release(ICredentialProviderCredential *iface)
{
    TEST_CREDENTIAL *This = CONTAINING_RECORD(iface, TEST_CREDENTIAL, ICredentialProviderCredential_iface);
    ULONG Ref = InterlockedDecrement(&This->Ref);
    if (!Ref)
        HeapFree(GetProcessHeap(), 0, This);
    return Ref;
}

static HRESULT WINAPI Credential_Advise(ICredentialProviderCredential *iface, ICredentialProviderCredentialEvents *pcpce) { return S_OK; }
static HRESULT WINAPI Credential_UnAdvise(ICredentialProviderCredential *iface) { return S_OK; }
static HRESULT WINAPI Credential_SetSelected(ICredentialProviderCredential *iface, BOOL *pbAutoLogon) { *pbAutoLogon = FALSE; return S_OK; }
static HRESULT WINAPI Credential_SetDeselected(ICredentialProviderCredential *iface) { return S_OK; }

static HRESULT WINAPI
Credential_GetFieldState(ICredentialProviderCredential *iface, DWORD dwFieldID,
                         CREDENTIAL_PROVIDER_FIELD_STATE *pcpfs, CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE *pcpfis)
{
    if (dwFieldID < 1 || dwFieldID > ARRAYSIZE(TestFields))
        return E_INVALIDARG;
    *pcpfs = CPFS_DISPLAY_IN_SELECTED_TILE;
    *pcpfis = CPFIS_NONE;
    return S_OK;
}

static HRESULT WINAPI
Credential_GetStringValue(ICredentialProviderCredential *iface, DWORD dwFieldID, LPWSTR *ppsz)
{
    if (dwFieldID < 1 || dwFieldID > ARRAYSIZE(TestFields))
        return E_INVALIDARG;
    *ppsz = DupString(TestFields[dwFieldID - 1].Label);
    return *ppsz ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI Credential_GetBitmapValue(ICredentialProviderCredential *iface, DWORD dwFieldID, HBITMAP *phbmp) { return E_NOTIMPL; }
static HRESULT WINAPI Credential_GetCheckboxValue(ICredentialProviderCredential *iface, DWORD dwFieldID, BOOL *pbChecked, LPWSTR *ppszLabel) { return E_NOTIMPL; }

static HRESULT WINAPI
Credential_GetSubmitButtonValue(ICredentialProviderCredential *iface, DWORD dwFieldID, DWORD *pdwAdjacentTo)
{
    if (dwFieldID != 3)
        return E_INVALIDARG;
    *pdwAdjacentTo = 2;
    return S_OK;
}

static HRESULT WINAPI Credential_GetComboBoxValueCount(ICredentialProviderCredential *iface, DWORD dwFieldID, DWORD *pcItems, DWORD *pdwSelectedItem) { return E_NOTIMPL; }
static HRESULT WINAPI Credential_GetComboBoxValueAt(ICredentialProviderCredential *iface, DWORD dwFieldID, DWORD dwItem, LPWSTR *ppszItem) { return E_NOTIMPL; }
static HRESULT WINAPI Credential_SetStringValue(ICredentialProviderCredential *iface, DWORD dwFieldID, LPCWSTR psz) { return (dwFieldID == 1 || dwFieldID == 2) ? S_OK : E_INVALIDARG; }
static HRESULT WINAPI Credential_SetCheckboxValue(ICredentialProviderCredential *iface, DWORD dwFieldID, BOOL bChecked) { return E_NOTIMPL; }
static HRESULT WINAPI Credential_SetComboBoxSelectedValue(ICredentialProviderCredential *iface, DWORD dwFieldID, DWORD dwSelectedItem) { return E_NOTIMPL; }
static HRESULT WINAPI Credential_CommandLinkClicked(ICredentialProviderCredential *iface, DWORD dwFieldID) { return E_NOTIMPL; }

static HRESULT WINAPI
Credential_GetSerialization(ICredentialProviderCredential *iface, CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE *pcpgsr,
                            CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcs, LPWSTR *ppszOptionalStatusText,
                            CREDENTIAL_PROVIDER_STATUS_ICON *pcpsiOptionalStatusIcon)
{
    const WCHAR *Mode = TestMode();

    if (wcscmp(Mode, L"hang") == 0)
        Sleep(INFINITE);
    if (wcscmp(Mode, L"crash") == 0)
        *(volatile int *)0 = 1;

    ZeroMemory(pcpcs, sizeof(*pcpcs));
    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    *ppszOptionalStatusText = DupString(L"testmod: nothing to submit");
    *pcpsiOptionalStatusIcon = CPSI_NONE;
    return S_OK;
}

static HRESULT WINAPI
Credential_ReportResult(ICredentialProviderCredential *iface, NTSTATUS ntsStatus, NTSTATUS ntsSubstatus,
                        LPWSTR *ppszOptionalStatusText, CREDENTIAL_PROVIDER_STATUS_ICON *pcpsiOptionalStatusIcon)
{
    *ppszOptionalStatusText = NULL;
    *pcpsiOptionalStatusIcon = CPSI_NONE;
    return S_OK;
}

static const ICredentialProviderCredentialVtbl CredentialVtbl =
{
    Credential_QueryInterface, Credential_AddRef, Credential_Release, Credential_Advise, Credential_UnAdvise,
    Credential_SetSelected, Credential_SetDeselected, Credential_GetFieldState, Credential_GetStringValue,
    Credential_GetBitmapValue, Credential_GetCheckboxValue, Credential_GetSubmitButtonValue,
    Credential_GetComboBoxValueCount, Credential_GetComboBoxValueAt, Credential_SetStringValue,
    Credential_SetCheckboxValue, Credential_SetComboBoxSelectedValue, Credential_CommandLinkClicked,
    Credential_GetSerialization, Credential_ReportResult,
};

static HRESULT WINAPI
Provider_GetFieldDescriptorCount(ICredentialProvider *iface, DWORD *pdwCount)
{
    *pdwCount = TestMode()[0] ? ARRAYSIZE(TestFields) : 0;
    return S_OK;
}

static HRESULT WINAPI
Provider_GetFieldDescriptorAt(ICredentialProvider *iface, DWORD dwIndex, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR **ppcpfd)
{
    CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR *Desc;

    *ppcpfd = NULL;
    if (!TestMode()[0] || dwIndex >= ARRAYSIZE(TestFields))
        return E_INVALIDARG;
    Desc = CoTaskMemAlloc(sizeof(*Desc));
    if (!Desc)
        return E_OUTOFMEMORY;
    ZeroMemory(Desc, sizeof(*Desc));
    Desc->dwFieldID = TestFields[dwIndex].Id;
    Desc->cpft = TestFields[dwIndex].Type;
    Desc->pszLabel = DupString(TestFields[dwIndex].Label);
    *ppcpfd = Desc;
    return S_OK;
}

static HRESULT WINAPI
Provider_GetCredentialCount(ICredentialProvider *iface, DWORD *pdwCount, DWORD *pdwDefault, BOOL *pbAutoLogonWithDefault)
{
    /* hang-startup: wedge before any credential exists, where the host cannot name a CLSID. */
    if (wcscmp(TestMode(), L"hang-startup") == 0)
        Sleep(INFINITE);
    *pdwCount = TestMode()[0] ? 1 : 0;
    *pdwDefault = CREDENTIAL_PROVIDER_NO_DEFAULT;
    *pbAutoLogonWithDefault = FALSE;
    return S_OK;
}

static HRESULT WINAPI
Provider_GetCredentialAt(ICredentialProvider *iface, DWORD dwIndex, ICredentialProviderCredential **ppcpc)
{
    TEST_CREDENTIAL *Credential;

    *ppcpc = NULL;
    if (!TestMode()[0] || dwIndex != 0)
        return E_INVALIDARG;
    Credential = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Credential));
    if (!Credential)
        return E_OUTOFMEMORY;
    Credential->ICredentialProviderCredential_iface.lpVtbl = &CredentialVtbl;
    Credential->Ref = 1;
    *ppcpc = &Credential->ICredentialProviderCredential_iface;
    return S_OK;
}

static const ICredentialProviderVtbl ProviderVtbl =
{
    Provider_QueryInterface, Provider_AddRef, Provider_Release, Provider_SetUsageScenario,
    Provider_SetSerialization, Provider_Advise, Provider_UnAdvise, Provider_GetFieldDescriptorCount,
    Provider_GetFieldDescriptorAt, Provider_GetCredentialCount, Provider_GetCredentialAt,
};

static HRESULT WINAPI
Filter_QueryInterface(ICredentialProviderFilter *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ICredentialProviderFilter))
    {
        *ppv = iface;
        ICredentialProviderFilter_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Filter_AddRef(ICredentialProviderFilter *iface)
{
    return InterlockedIncrement(&CONTAINING_RECORD(iface, TEST_FILTER, ICredentialProviderFilter_iface)->Ref);
}

static ULONG WINAPI
Filter_Release(ICredentialProviderFilter *iface)
{
    TEST_FILTER *This = CONTAINING_RECORD(iface, TEST_FILTER, ICredentialProviderFilter_iface);
    ULONG Ref = InterlockedDecrement(&This->Ref);
    if (!Ref)
        HeapFree(GetProcessHeap(), 0, This);
    return Ref;
}

static HRESULT WINAPI
Filter_Filter(ICredentialProviderFilter *iface, CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD dwFlags,
              GUID *rgclsidProviders, BOOL *rgbAllow, DWORD cProviders)
{
    DWORD i;

    for (i = 0; i < cProviders; i++)
    {
        if (IsEqualGUID(&rgclsidProviders[i], &CLSID_CredProvTestProvider))
            rgbAllow[i] = FALSE;
    }
    return S_OK;
}

static HRESULT WINAPI
Filter_UpdateRemoteCredential(ICredentialProviderFilter *iface,
                              const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcsIn,
                              CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcsOut)
{
    *pcpcsOut = *pcpcsIn;
    pcpcsOut->rgbSerialization = NULL;
    pcpcsOut->cbSerialization = 0;
    pcpcsOut->clsidCredentialProvider = CLSID_CredProvTestProvider;
    return S_OK;
}

static const ICredentialProviderFilterVtbl FilterVtbl =
{
    Filter_QueryInterface, Filter_AddRef, Filter_Release, Filter_Filter, Filter_UpdateRemoteCredential,
};

static HRESULT WINAPI
Factory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IClassFactory))
    {
        *ppv = iface;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Factory_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI
Factory_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI
ProviderFactory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    TEST_PROVIDER *This;
    HRESULT hr;

    *ppv = NULL;
    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;
    This->ICredentialProvider_iface.lpVtbl = &ProviderVtbl;
    This->Ref = 1;
    hr = ICredentialProvider_QueryInterface(&This->ICredentialProvider_iface, riid, ppv);
    ICredentialProvider_Release(&This->ICredentialProvider_iface);
    return hr;
}

static HRESULT WINAPI
FilterFactory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    TEST_FILTER *This;
    HRESULT hr;

    *ppv = NULL;
    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;
    This->ICredentialProviderFilter_iface.lpVtbl = &FilterVtbl;
    This->Ref = 1;
    hr = ICredentialProviderFilter_QueryInterface(&This->ICredentialProviderFilter_iface, riid, ppv);
    ICredentialProviderFilter_Release(&This->ICredentialProviderFilter_iface);
    return hr;
}

static HRESULT WINAPI
Factory_LockServer(IClassFactory *iface, BOOL lock)
{
    return S_OK;
}

static const IClassFactoryVtbl ProviderFactoryVtbl =
{
    Factory_QueryInterface, Factory_AddRef, Factory_Release, ProviderFactory_CreateInstance, Factory_LockServer,
};

static const IClassFactoryVtbl FilterFactoryVtbl =
{
    Factory_QueryInterface, Factory_AddRef, Factory_Release, FilterFactory_CreateInstance, Factory_LockServer,
};

static IClassFactory ProviderFactory = { &ProviderFactoryVtbl };
static IClassFactory FilterFactory = { &FilterFactoryVtbl };

HRESULT WINAPI
DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (IsEqualGUID(clsid, &CLSID_CredProvTestProvider) || IsEqualGUID(clsid, &CLSID_CredProvTestDependent))
        return IClassFactory_QueryInterface(&ProviderFactory, riid, ppv);
    if (IsEqualGUID(clsid, &CLSID_CredProvTestFilter))
        return IClassFactory_QueryInterface(&FilterFactory, riid, ppv);
    *ppv = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI
DllCanUnloadNow(void)
{
    return S_FALSE;
}
