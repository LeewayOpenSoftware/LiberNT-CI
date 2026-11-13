/*
 * PROJECT:     LiberNT WinRT twinapi.appcore
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     AnalyticsInfo runtime class that reports the device family and the system version
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "initguid.h"
#include "private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appcore);

static HRESULT
GetIidsNone(ULONG *Count, IID **Iids)
{
    *Count = 0;
    *Iids = NULL;
    return S_OK;
}

static HRESULT WINAPI
VersionInfo_QueryInterface(IAnalyticsVersionInfo *Iface, REFIID Iid, void **Out)
{
    if (IsEqualGUID(Iid, &IID_IUnknown) || IsEqualGUID(Iid, &IID_IInspectable) ||
        IsEqualGUID(Iid, &IID_IAgileObject) || IsEqualGUID(Iid, &IID_IAnalyticsVersionInfo))
    {
        *Out = Iface;
        return S_OK;
    }
    /* TODO: IAnalyticsVersionInfo2 (ProductName), answered on Windows 11 */
    FIXME("AnalyticsVersionInfo: no interface %s\n", debugstr_guid(Iid));
    *Out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
VersionInfo_AddRef(IAnalyticsVersionInfo *Iface)
{
    return 2;
}

static ULONG WINAPI
VersionInfo_Release(IAnalyticsVersionInfo *Iface)
{
    return 1;
}

static HRESULT WINAPI
VersionInfo_GetIids(IAnalyticsVersionInfo *Iface, ULONG *Count, IID **Iids)
{
    return GetIidsNone(Count, Iids);
}

static HRESULT WINAPI
VersionInfo_GetRuntimeClassName(IAnalyticsVersionInfo *Iface, HSTRING *Name)
{
    return WindowsCreateString(RuntimeClass_Windows_System_Profile_AnalyticsVersionInfo,
                               wcslen(RuntimeClass_Windows_System_Profile_AnalyticsVersionInfo), Name);
}

static HRESULT WINAPI
VersionInfo_GetTrustLevel(IAnalyticsVersionInfo *Iface, TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI
VersionInfo_get_DeviceFamily(IAnalyticsVersionInfo *Iface, HSTRING *Value)
{
    static const WCHAR Family[] = L"Windows.Desktop";

    return WindowsCreateString(Family, ARRAY_SIZE(Family) - 1, Value);
}

static HRESULT WINAPI
VersionInfo_get_DeviceFamilyVersion(IAnalyticsVersionInfo *Iface, HSTRING *Value)
{
    RTL_OSVERSIONINFOEXW Version;
    DWORD Revision = 0, Size = sizeof(Revision);
    ULONGLONG Packed;
    WCHAR Text[24];

    Version.dwOSVersionInfoSize = sizeof(Version);
    if (RtlGetVersion(&Version))
        return E_FAIL;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                 RRF_RT_REG_DWORD, NULL, &Revision, &Size);
    Packed = ((ULONGLONG)(Version.dwMajorVersion & 0xFFFF) << 48) |
             ((ULONGLONG)(Version.dwMinorVersion & 0xFFFF) << 32) |
             ((ULONGLONG)(Version.dwBuildNumber & 0xFFFF) << 16) |
             (Revision & 0xFFFF);
    _snwprintf(Text, ARRAY_SIZE(Text), L"%I64u", Packed);
    return WindowsCreateString(Text, wcslen(Text), Value);
}

static const IAnalyticsVersionInfoVtbl VersionInfoVtbl =
{
    VersionInfo_QueryInterface,
    VersionInfo_AddRef,
    VersionInfo_Release,
    VersionInfo_GetIids,
    VersionInfo_GetRuntimeClassName,
    VersionInfo_GetTrustLevel,
    VersionInfo_get_DeviceFamily,
    VersionInfo_get_DeviceFamilyVersion,
};

static IAnalyticsVersionInfo VersionInfo = { &VersionInfoVtbl };

static IActivationFactory AnalyticsFactory;
static IAnalyticsInfoStatics AnalyticsStatics;
static IAnalyticsInfoStatics2 AnalyticsStatics2;

static HRESULT
AnalyticsQueryInterface(REFIID Iid, void **Out)
{
    if (IsEqualGUID(Iid, &IID_IUnknown) || IsEqualGUID(Iid, &IID_IInspectable) ||
        IsEqualGUID(Iid, &IID_IAgileObject) || IsEqualGUID(Iid, &IID_IActivationFactory))
    {
        *Out = &AnalyticsFactory;
        return S_OK;
    }
    if (IsEqualGUID(Iid, &IID_IAnalyticsInfoStatics))
    {
        *Out = &AnalyticsStatics;
        return S_OK;
    }
    if (IsEqualGUID(Iid, &IID_IAnalyticsInfoStatics2))
    {
        *Out = &AnalyticsStatics2;
        return S_OK;
    }
    FIXME("AnalyticsInfo factory: no interface %s\n", debugstr_guid(Iid));
    *Out = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI
Statics_QueryInterface(IAnalyticsInfoStatics *Iface, REFIID Iid, void **Out)
{
    return AnalyticsQueryInterface(Iid, Out);
}

static ULONG WINAPI
Statics_AddRef(IAnalyticsInfoStatics *Iface)
{
    return 2;
}

static ULONG WINAPI
Statics_Release(IAnalyticsInfoStatics *Iface)
{
    return 1;
}

static HRESULT WINAPI
Statics_GetIids(IAnalyticsInfoStatics *Iface, ULONG *Count, IID **Iids)
{
    return GetIidsNone(Count, Iids);
}

static HRESULT WINAPI
Statics_GetRuntimeClassName(IAnalyticsInfoStatics *Iface, HSTRING *Name)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI
Statics_GetTrustLevel(IAnalyticsInfoStatics *Iface, TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI
Statics_get_VersionInfo(IAnalyticsInfoStatics *Iface, IAnalyticsVersionInfo **Value)
{
    *Value = &VersionInfo;
    return S_OK;
}

static HRESULT WINAPI
Statics_get_DeviceForm(IAnalyticsInfoStatics *Iface, HSTRING *Value)
{
    static const WCHAR Form[] = L"Unknown";

    return WindowsCreateString(Form, ARRAY_SIZE(Form) - 1, Value);
}

static const IAnalyticsInfoStaticsVtbl StaticsVtbl =
{
    Statics_QueryInterface,
    Statics_AddRef,
    Statics_Release,
    Statics_GetIids,
    Statics_GetRuntimeClassName,
    Statics_GetTrustLevel,
    Statics_get_VersionInfo,
    Statics_get_DeviceForm,
};

static HRESULT WINAPI
Statics2_QueryInterface(IAnalyticsInfoStatics2 *Iface, REFIID Iid, void **Out)
{
    return AnalyticsQueryInterface(Iid, Out);
}

static ULONG WINAPI
Statics2_AddRef(IAnalyticsInfoStatics2 *Iface)
{
    return 2;
}

static ULONG WINAPI
Statics2_Release(IAnalyticsInfoStatics2 *Iface)
{
    return 1;
}

static HRESULT WINAPI
Statics2_GetIids(IAnalyticsInfoStatics2 *Iface, ULONG *Count, IID **Iids)
{
    return GetIidsNone(Count, Iids);
}

static HRESULT WINAPI
Statics2_GetRuntimeClassName(IAnalyticsInfoStatics2 *Iface, HSTRING *Name)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI
Statics2_GetTrustLevel(IAnalyticsInfoStatics2 *Iface, TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static HRESULT
GetSystemProperties(IUnknown *Invoker, IUnknown *Param, PROPVARIANT *Result, BOOL CalledAsync)
{
    IIterable_HSTRING *Names = (IIterable_HSTRING *)Param;
    IIterator_HSTRING *Iterator;
    IMapView_HSTRING_HSTRING *View;
    boolean HasCurrent = FALSE;
    HSTRING Name;
    HRESULT hr;

    if (Names)
    {
        if (FAILED(hr = IIterable_HSTRING_First(Names, &Iterator)))
            return hr;
        hr = IIterator_HSTRING_get_HasCurrent(Iterator, &HasCurrent);
        while (SUCCEEDED(hr) && HasCurrent)
        {
            if (SUCCEEDED(hr = IIterator_HSTRING_get_Current(Iterator, &Name)))
            {
                /* TODO: system property values; unknown names are left out of the map */
                FIXME("AnalyticsInfo system property %s\n", debugstr_hstring(Name));
                WindowsDeleteString(Name);
                hr = IIterator_HSTRING_MoveNext(Iterator, &HasCurrent);
            }
        }
        IIterator_HSTRING_Release(Iterator);
        if (FAILED(hr))
            return hr;
    }

    if (FAILED(hr = string_view_create(NULL, 0, &View)))
        return hr;
    Result->vt = VT_UNKNOWN;
    Result->punkVal = (IUnknown *)View;
    return S_OK;
}

static HRESULT WINAPI
Statics2_GetSystemPropertiesAsync(IAnalyticsInfoStatics2 *Iface, IIterable_HSTRING *Names,
                                  IAsyncOperation_IMapView_HSTRING_HSTRING **Operation)
{
    if (!Operation)
        return E_POINTER;
    return async_operation_inspectable_create(&IID_IAsyncOperation_IMapView_HSTRING_HSTRING, (IUnknown *)Iface,
                                              (IUnknown *)Names, GetSystemProperties,
                                              (IAsyncOperation_IInspectable **)Operation);
}

static const IAnalyticsInfoStatics2Vtbl Statics2Vtbl =
{
    Statics2_QueryInterface,
    Statics2_AddRef,
    Statics2_Release,
    Statics2_GetIids,
    Statics2_GetRuntimeClassName,
    Statics2_GetTrustLevel,
    Statics2_GetSystemPropertiesAsync,
};

static HRESULT WINAPI
Factory_QueryInterface(IActivationFactory *Iface, REFIID Iid, void **Out)
{
    return AnalyticsQueryInterface(Iid, Out);
}

static ULONG WINAPI
Factory_AddRef(IActivationFactory *Iface)
{
    return 2;
}

static ULONG WINAPI
Factory_Release(IActivationFactory *Iface)
{
    return 1;
}

static HRESULT WINAPI
Factory_GetIids(IActivationFactory *Iface, ULONG *Count, IID **Iids)
{
    return GetIidsNone(Count, Iids);
}

static HRESULT WINAPI
Factory_GetRuntimeClassName(IActivationFactory *Iface, HSTRING *Name)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI
Factory_GetTrustLevel(IActivationFactory *Iface, TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI
Factory_ActivateInstance(IActivationFactory *Iface, IInspectable **Instance)
{
    *Instance = NULL;
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl FactoryVtbl =
{
    Factory_QueryInterface,
    Factory_AddRef,
    Factory_Release,
    Factory_GetIids,
    Factory_GetRuntimeClassName,
    Factory_GetTrustLevel,
    Factory_ActivateInstance,
};

static IActivationFactory AnalyticsFactory = { &FactoryVtbl };
static IAnalyticsInfoStatics AnalyticsStatics = { &StaticsVtbl };
static IAnalyticsInfoStatics2 AnalyticsStatics2 = { &Statics2Vtbl };

HRESULT WINAPI
DllGetClassObject(REFCLSID Clsid, REFIID Iid, void **Out)
{
    *Out = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI
DllGetActivationFactory(HSTRING ClassId, IActivationFactory **Factory)
{
    const WCHAR *Name = WindowsGetStringRawBuffer(ClassId, NULL);

    *Factory = NULL;
    if (wcscmp(Name, RuntimeClass_Windows_System_Profile_AnalyticsInfo))
    {
        ERR("no class %s\n", debugstr_hstring(ClassId));
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    *Factory = &AnalyticsFactory;
    return S_OK;
}
