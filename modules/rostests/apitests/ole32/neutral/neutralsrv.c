/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     In-process server of a class with the neutral threading model
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS
#define CONST_VTABLE

#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <oleauto.h>

#include "neutral.h"

typedef struct
{
    IDispatch IDispatch_iface;
    LONG Ref;
    APTTYPE CreatedType;
    APTTYPEQUALIFIER CreatedQualifier;
} NEUTRAL_OBJECT;

static LONG ServerLocks;

static NEUTRAL_OBJECT *ImplFromIDispatch(IDispatch *iface)
{
    return CONTAINING_RECORD(iface, NEUTRAL_OBJECT, IDispatch_iface);
}

static HRESULT WINAPI Object_QueryInterface(IDispatch *iface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch))
    {
        *ppv = iface;
        IDispatch_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Object_AddRef(IDispatch *iface)
{
    return InterlockedIncrement(&ImplFromIDispatch(iface)->Ref);
}

static ULONG WINAPI Object_Release(IDispatch *iface)
{
    NEUTRAL_OBJECT *This = ImplFromIDispatch(iface);
    ULONG Ref = InterlockedDecrement(&This->Ref);

    if (!Ref)
    {
        HeapFree(GetProcessHeap(), 0, This);
        InterlockedDecrement(&ServerLocks);
    }
    return Ref;
}

static HRESULT WINAPI Object_GetTypeInfoCount(IDispatch *iface, UINT *Count)
{
    *Count = 0;
    return S_OK;
}

static HRESULT WINAPI Object_GetTypeInfo(IDispatch *iface, UINT Index, LCID Lcid, ITypeInfo **TypeInfo)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI Object_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *Names, UINT Count, LCID Lcid, DISPID *Ids)
{
    return E_NOTIMPL;
}

static HRESULT CallBack(DISPPARAMS *Params, LONG *Result)
{
    DISPPARAMS NoParams = { NULL, NULL, 0, 0 };
    VARIANT Value;
    HRESULT hr;

    if (!Params || Params->cArgs != 1 || V_VT(&Params->rgvarg[0]) != VT_DISPATCH || !V_DISPATCH(&Params->rgvarg[0]))
        return E_INVALIDARG;

    VariantInit(&Value);
    hr = IDispatch_Invoke(V_DISPATCH(&Params->rgvarg[0]), NEUTRAL_DISPID_THREAD, &IID_NULL, 0,
                          DISPATCH_METHOD, &NoParams, &Value, NULL, NULL);
    if (SUCCEEDED(hr))
        *Result = V_VT(&Value) == VT_I4 ? V_I4(&Value) : -1;
    VariantClear(&Value);
    return hr;
}

static HRESULT WINAPI Object_Invoke(IDispatch *iface, DISPID Id, REFIID riid, LCID Lcid, WORD Flags,
        DISPPARAMS *Params, VARIANT *Result, EXCEPINFO *ExcepInfo, UINT *ArgErr)
{
    NEUTRAL_OBJECT *This = ImplFromIDispatch(iface);
    APTTYPEQUALIFIER Qualifier;
    APTTYPE Type;
    LONG Value = 0;
    HRESULT hr = S_OK;

    switch (Id)
    {
    case NEUTRAL_DISPID_THREAD:
        Value = (LONG)GetCurrentThreadId();
        break;
    case NEUTRAL_DISPID_TYPE:
    case NEUTRAL_DISPID_QUALIFIER:
        hr = CoGetApartmentType(&Type, &Qualifier);
        Value = Id == NEUTRAL_DISPID_TYPE ? (LONG)Type : (LONG)Qualifier;
        break;
    case NEUTRAL_DISPID_CALLBACK:
        hr = CallBack(Params, &Value);
        break;
    case NEUTRAL_DISPID_DISCONNECT:
        Value = CoDisconnectContext(0);
        break;
    case NEUTRAL_DISPID_INIT_STA:
    case NEUTRAL_DISPID_INIT_MTA:
        Value = CoInitializeEx(NULL, Id == NEUTRAL_DISPID_INIT_STA ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED);
        if (SUCCEEDED(Value))
            CoUninitialize();
        break;
    case NEUTRAL_DISPID_CREATED_TYPE:
        Value = (LONG)This->CreatedType;
        break;
    case NEUTRAL_DISPID_CREATED_QUALIFIER:
        Value = (LONG)This->CreatedQualifier;
        break;
    default:
        return DISP_E_MEMBERNOTFOUND;
    }

    if (FAILED(hr))
        return hr;
    if (Result)
    {
        V_VT(Result) = VT_I4;
        V_I4(Result) = Value;
    }
    return S_OK;
}

static const IDispatchVtbl ObjectVtbl =
{
    Object_QueryInterface,
    Object_AddRef,
    Object_Release,
    Object_GetTypeInfoCount,
    Object_GetTypeInfo,
    Object_GetIDsOfNames,
    Object_Invoke
};

static HRESULT WINAPI Factory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory))
    {
        *ppv = iface;
        IClassFactory_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Factory_AddRef(IClassFactory *iface)
{
    InterlockedIncrement(&ServerLocks);
    return 2;
}

static ULONG WINAPI Factory_Release(IClassFactory *iface)
{
    InterlockedDecrement(&ServerLocks);
    return 1;
}

static HRESULT WINAPI Factory_CreateInstance(IClassFactory *iface, IUnknown *Outer, REFIID riid, void **ppv)
{
    NEUTRAL_OBJECT *Object;
    HRESULT hr;

    *ppv = NULL;
    if (Outer)
        return CLASS_E_NOAGGREGATION;

    Object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Object));
    if (!Object)
        return E_OUTOFMEMORY;

    Object->IDispatch_iface.lpVtbl = &ObjectVtbl;
    Object->Ref = 1;
    if (FAILED(CoGetApartmentType(&Object->CreatedType, &Object->CreatedQualifier)))
    {
        Object->CreatedType = APTTYPE_CURRENT;
        Object->CreatedQualifier = APTTYPEQUALIFIER_NONE;
    }
    InterlockedIncrement(&ServerLocks);

    hr = IDispatch_QueryInterface(&Object->IDispatch_iface, riid, ppv);
    IDispatch_Release(&Object->IDispatch_iface);
    return hr;
}

static HRESULT WINAPI Factory_LockServer(IClassFactory *iface, BOOL Lock)
{
    if (Lock)
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
    Factory_LockServer
};

static IClassFactory Factory = { &FactoryVtbl };

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    if (!IsEqualCLSID(rclsid, &CLSID_NeutralTest))
    {
        *ppv = NULL;
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return IClassFactory_QueryInterface(&Factory, riid, ppv);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return ServerLocks ? S_FALSE : S_OK;
}
