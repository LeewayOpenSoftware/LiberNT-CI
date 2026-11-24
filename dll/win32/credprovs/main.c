/*
 * PROJECT:     LiberNT Credential Providers
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     DLL entry points and class factory
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(credprovs);

HINSTANCE CredProvsInstance;
LONG CredProvsObjects;
static LONG ServerLocks;

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
Factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (outer)
        return CLASS_E_NOAGGREGATION;
    return PasswordProvider_Create(riid, ppv);
}

static HRESULT WINAPI
Factory_LockServer(IClassFactory *iface, BOOL lock)
{
    if (lock)
        InterlockedIncrement(&ServerLocks);
    else
        InterlockedDecrement(&ServerLocks);
    return S_OK;
}

static const IClassFactoryVtbl FactoryVtbl =
{
    Factory_QueryInterface,
    Factory_AddRef,
    Factory_Release,
    Factory_CreateInstance,
    Factory_LockServer,
};

static IClassFactory PasswordProviderFactory = { &FactoryVtbl };

HRESULT WINAPI
DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    TRACE("(%s, %s, %p)\n", debugstr_guid(clsid), debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (IsEqualGUID(clsid, &CLSID_PasswordCredentialProvider))
        return IClassFactory_QueryInterface(&PasswordProviderFactory, riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI
DllCanUnloadNow(void)
{
    return (CredProvsObjects || ServerLocks) ? S_FALSE : S_OK;
}

BOOL WINAPI
DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        CredProvsInstance = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}
