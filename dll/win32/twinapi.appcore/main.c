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

static IAnalyticsVersionInfo VersionInfo;
static IAnalyticsVersionInfo2 VersionInfo2;

static HRESULT WINAPI
VersionInfo_QueryInterface(IAnalyticsVersionInfo *Iface, REFIID Iid, void **Out)
{
    if (IsEqualGUID(Iid, &IID_IUnknown) || IsEqualGUID(Iid, &IID_IInspectable) ||
        IsEqualGUID(Iid, &IID_IAgileObject) || IsEqualGUID(Iid, &IID_IAnalyticsVersionInfo))
    {
        *Out = &VersionInfo;
        return S_OK;
    }
    if (IsEqualGUID(Iid, &IID_IAnalyticsVersionInfo2))
    {
        *Out = &VersionInfo2;
        return S_OK;
    }
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

static HRESULT WINAPI
VersionInfo2_QueryInterface(IAnalyticsVersionInfo2 *Iface, REFIID Iid, void **Out)
{
    return VersionInfo_QueryInterface(&VersionInfo, Iid, Out);
}

static ULONG WINAPI
VersionInfo2_AddRef(IAnalyticsVersionInfo2 *Iface)
{
    return 2;
}

static ULONG WINAPI
VersionInfo2_Release(IAnalyticsVersionInfo2 *Iface)
{
    return 1;
}

static HRESULT WINAPI
VersionInfo2_GetIids(IAnalyticsVersionInfo2 *Iface, ULONG *Count, IID **Iids)
{
    return GetIidsNone(Count, Iids);
}

static HRESULT WINAPI
VersionInfo2_GetRuntimeClassName(IAnalyticsVersionInfo2 *Iface, HSTRING *Name)
{
    return VersionInfo_GetRuntimeClassName(&VersionInfo, Name);
}

static HRESULT WINAPI
VersionInfo2_GetTrustLevel(IAnalyticsVersionInfo2 *Iface, TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI
VersionInfo2_get_ProductName(IAnalyticsVersionInfo2 *Iface, HSTRING *Value)
{
    WCHAR Name[256];
    DWORD Size = sizeof(Name);
    LSTATUS Error;

    Error = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName",
                         RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, Name, &Size);
    if (Error)
        return HRESULT_FROM_WIN32(Error);
    return WindowsCreateString(Name, wcslen(Name), Value);
}

static const IAnalyticsVersionInfo2Vtbl VersionInfo2Vtbl =
{
    VersionInfo2_QueryInterface,
    VersionInfo2_AddRef,
    VersionInfo2_Release,
    VersionInfo2_GetIids,
    VersionInfo2_GetRuntimeClassName,
    VersionInfo2_GetTrustLevel,
    VersionInfo2_get_ProductName,
};

static IAnalyticsVersionInfo VersionInfo = { &VersionInfoVtbl };
static IAnalyticsVersionInfo2 VersionInfo2 = { &VersionInfo2Vtbl };

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

static BOOL
GetOsVersionFull(_Out_writes_(Length) WCHAR *Text, _In_ SIZE_T Length)
{
    RTL_OSVERSIONINFOEXW Version;
    DWORD Revision = 0, Size = sizeof(Revision);
    WCHAR BuildLab[128];
    WCHAR *Tail;
    int Written;

    Size = sizeof(BuildLab);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"BuildLabEx",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, BuildLab, &Size))
    {
        return FALSE;
    }
    Tail = wcschr(BuildLab, L'.');
    if (!Tail || !(Tail = wcschr(Tail + 1, L'.')))
        return FALSE;

    Version.dwOSVersionInfoSize = sizeof(Version);
    if (RtlGetVersion(&Version))
        return FALSE;
    Size = sizeof(Revision);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                 RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, NULL, &Revision, &Size);
    Written = _snwprintf(Text, Length, L"%lu.%lu.%lu.%lu%ls", Version.dwMajorVersion, Version.dwMinorVersion,
                         Version.dwBuildNumber, Revision, Tail);
    return Written > 0 && (SIZE_T)Written < Length;
}

static UINT32
GetAvailableProperties(_Out_writes_(5) struct string_entry *Entries, _Out_writes_(MAX_PATH) WCHAR *Image,
                       _Out_writes_(OsVersionLength) WCHAR *OsVersion, _In_ SIZE_T OsVersionLength)
{
    UINT32 Count = 0;
    WCHAR *Name;
    DWORD Length;

    Length = GetModuleFileNameW(NULL, Image, MAX_PATH);
    if (Length && Length < MAX_PATH)
    {
        Name = wcsrchr(Image, L'\\');
        Entries[Count].key = L"App";
        Entries[Count++].value = Name ? Name + 1 : Image;
    }
    Entries[Count].key = L"FlightRing";
    Entries[Count++].value = L"Retail";
    Entries[Count].key = L"AttrDataVer";
    Entries[Count++].value = L"508";
    if (sizeof(void *) == 8)
    {
        /* Windows 11 reports DeviceFamily only to 64-bit callers */
        Entries[Count].key = L"DeviceFamily";
        Entries[Count++].value = L"Windows.Desktop";
        if (GetOsVersionFull(OsVersion, OsVersionLength))
        {
            Entries[Count].key = L"OSVersionFull";
            Entries[Count++].value = OsVersion;
        }
    }
    return Count;
}

static HRESULT
GetSystemProperties(IUnknown *Invoker, IUnknown *Param, PROPVARIANT *Result, BOOL CalledAsync)
{
    IIterable_HSTRING *Names = (IIterable_HSTRING *)Param;
    struct string_entry Available[5], Selected[5];
    UINT32 AvailableCount, SelectedCount = 0, Requested = 0, i;
    IIterator_HSTRING *Iterator;
    IMapView_HSTRING_HSTRING *View;
    boolean HasCurrent = FALSE;
    WCHAR Image[MAX_PATH], OsVersion[192];
    const WCHAR *Text;
    HSTRING Name;
    HRESULT hr;

    AvailableCount = GetAvailableProperties(Available, Image, OsVersion, ARRAY_SIZE(OsVersion));

    if (FAILED(hr = IIterable_HSTRING_First(Names, &Iterator)))
        return hr;
    hr = IIterator_HSTRING_get_HasCurrent(Iterator, &HasCurrent);
    while (SUCCEEDED(hr) && HasCurrent)
    {
        if (FAILED(hr = IIterator_HSTRING_get_Current(Iterator, &Name)))
            break;
        Requested++;
        Text = WindowsGetStringRawBuffer(Name, NULL);
        for (i = 0; i < AvailableCount; i++)
        {
            if (!wcscmp(Text, Available[i].key))
                break;
        }
        if (i < AvailableCount && SelectedCount < ARRAY_SIZE(Selected))
            Selected[SelectedCount++] = Available[i];
        else
            TRACE("system property %s is not available\n", debugstr_hstring(Name));
        WindowsDeleteString(Name);
        hr = IIterator_HSTRING_MoveNext(Iterator, &HasCurrent);
    }
    IIterator_HSTRING_Release(Iterator);
    if (FAILED(hr))
        return hr;

    if (Requested)
        hr = string_view_create(Selected, SelectedCount, &View);
    else
        hr = string_view_create(Available, AvailableCount, &View);
    if (FAILED(hr))
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
    *Operation = NULL;
    if (!Names)
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
