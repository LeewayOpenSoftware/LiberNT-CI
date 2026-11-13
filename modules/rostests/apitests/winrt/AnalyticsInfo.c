/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the Windows.System.Profile.AnalyticsInfo static class
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "winrt_apitest.h"

#define WIDL_using_Windows_System_Profile
#include <windows.system.profile.h>
#include <initguid.h>


typedef LONG (WINAPI *RTL_GET_VERSION)(RTL_OSVERSIONINFOW *);

static const WCHAR *const PropertyNames[] =
{
    L"App", L"AppVer", L"DeviceFamily", L"FlightRing", L"OSVersionFull", L"LiberNTNoSuchAttribute",
};

static UINT32 NameCount;

typedef struct _NAME_ITERATOR
{
    IIterator_HSTRING IIterator_HSTRING_iface;
    LONG Ref;
    UINT32 Index;
} NAME_ITERATOR;

static HRESULT WINAPI
Iterator_QueryInterface(IIterator_HSTRING *Iface, REFIID Iid, void **Out)
{
    if (IsEqualGUID(Iid, &IID_IUnknown) || IsEqualGUID(Iid, &IID_IInspectable) ||
        IsEqualGUID(Iid, &IID_IAgileObject) || IsEqualGUID(Iid, &IID_IIterator_HSTRING))
    {
        *Out = Iface;
        IIterator_HSTRING_AddRef(Iface);
        return S_OK;
    }
    *Out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Iterator_AddRef(IIterator_HSTRING *Iface)
{
    return InterlockedIncrement(&CONTAINING_RECORD(Iface, NAME_ITERATOR, IIterator_HSTRING_iface)->Ref);
}

static ULONG WINAPI
Iterator_Release(IIterator_HSTRING *Iface)
{
    NAME_ITERATOR *Iterator = CONTAINING_RECORD(Iface, NAME_ITERATOR, IIterator_HSTRING_iface);
    ULONG Ref = InterlockedDecrement(&Iterator->Ref);

    if (!Ref)
        HeapFree(GetProcessHeap(), 0, Iterator);
    return Ref;
}

static HRESULT WINAPI
Iterator_GetIids(IIterator_HSTRING *Iface, ULONG *Count, IID **Iids)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI
Iterator_GetRuntimeClassName(IIterator_HSTRING *Iface, HSTRING *Name)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI
Iterator_GetTrustLevel(IIterator_HSTRING *Iface, TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI
Iterator_get_Current(IIterator_HSTRING *Iface, HSTRING *Value)
{
    NAME_ITERATOR *Iterator = CONTAINING_RECORD(Iface, NAME_ITERATOR, IIterator_HSTRING_iface);

    *Value = NULL;
    if (Iterator->Index >= NameCount)
        return E_BOUNDS;
    *Value = CreateHString(PropertyNames[Iterator->Index]);
    return S_OK;
}

static HRESULT WINAPI
Iterator_get_HasCurrent(IIterator_HSTRING *Iface, boolean *Value)
{
    *Value = CONTAINING_RECORD(Iface, NAME_ITERATOR, IIterator_HSTRING_iface)->Index < NameCount;
    return S_OK;
}

static HRESULT WINAPI
Iterator_MoveNext(IIterator_HSTRING *Iface, boolean *Value)
{
    NAME_ITERATOR *Iterator = CONTAINING_RECORD(Iface, NAME_ITERATOR, IIterator_HSTRING_iface);

    if (Iterator->Index < NameCount)
        Iterator->Index++;
    return Iterator_get_HasCurrent(Iface, Value);
}

static HRESULT WINAPI
Iterator_GetMany(IIterator_HSTRING *Iface, UINT32 Size, HSTRING *Items, UINT32 *Count)
{
    NAME_ITERATOR *Iterator = CONTAINING_RECORD(Iface, NAME_ITERATOR, IIterator_HSTRING_iface);

    for (*Count = 0; *Count < Size && Iterator->Index < NameCount; (*Count)++)
        Items[*Count] = CreateHString(PropertyNames[Iterator->Index++]);
    return S_OK;
}

static const IIterator_HSTRINGVtbl IteratorVtbl =
{
    Iterator_QueryInterface,
    Iterator_AddRef,
    Iterator_Release,
    Iterator_GetIids,
    Iterator_GetRuntimeClassName,
    Iterator_GetTrustLevel,
    Iterator_get_Current,
    Iterator_get_HasCurrent,
    Iterator_MoveNext,
    Iterator_GetMany,
};

static HRESULT WINAPI
Names_QueryInterface(IIterable_HSTRING *Iface, REFIID Iid, void **Out)
{
    if (IsEqualGUID(Iid, &IID_IUnknown) || IsEqualGUID(Iid, &IID_IInspectable) ||
        IsEqualGUID(Iid, &IID_IAgileObject) || IsEqualGUID(Iid, &IID_IIterable_HSTRING))
    {
        *Out = Iface;
        return S_OK;
    }
    *Out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Names_AddRef(IIterable_HSTRING *Iface)
{
    return 2;
}

static ULONG WINAPI
Names_Release(IIterable_HSTRING *Iface)
{
    return 1;
}

static HRESULT WINAPI
Names_GetIids(IIterable_HSTRING *Iface, ULONG *Count, IID **Iids)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI
Names_GetRuntimeClassName(IIterable_HSTRING *Iface, HSTRING *Name)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI
Names_GetTrustLevel(IIterable_HSTRING *Iface, TrustLevel *Level)
{
    *Level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI
Names_First(IIterable_HSTRING *Iface, IIterator_HSTRING **Value)
{
    NAME_ITERATOR *Iterator = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Iterator));

    *Value = NULL;
    if (!Iterator)
        return E_OUTOFMEMORY;
    Iterator->IIterator_HSTRING_iface.lpVtbl = (IIterator_HSTRINGVtbl *)&IteratorVtbl;
    Iterator->Ref = 1;
    *Value = &Iterator->IIterator_HSTRING_iface;
    return S_OK;
}

static const IIterable_HSTRINGVtbl NamesVtbl =
{
    Names_QueryInterface,
    Names_AddRef,
    Names_Release,
    Names_GetIids,
    Names_GetRuntimeClassName,
    Names_GetTrustLevel,
    Names_First,
};

static IIterable_HSTRING Names = { (IIterable_HSTRINGVtbl *)&NamesVtbl };

static void
CheckProperty(
    _In_ IMapView_HSTRING_HSTRING *Map,
    _In_ const char *Call,
    _In_ const WCHAR *Key,
    _In_opt_ const WCHAR *Expected)
{
    HSTRING Name = CreateHString(Key), Value = NULL;
    boolean Found = FALSE;
    HRESULT hr;

    hr = IMapView_HSTRING_HSTRING_HasKey(Map, Name, &Found);
    ok(hr == S_OK, "%s: HasKey(%ls) returned hr %#lx\n", Call, Key, hr);
    if (!Expected)
    {
        ok(!Found, "%s: %ls is present, Windows 11 leaves it out\n", Call, Key);
    }
    else
    {
        ok(Found, "%s: %ls is absent, expected %ls\n", Call, Key, Expected);
        if (Found && SUCCEEDED(IMapView_HSTRING_HSTRING_Lookup(Map, Name, &Value)))
        {
            ok(!wcscmp(WindowsGetStringRawBuffer(Value, NULL), Expected), "%s: %ls is %ls, expected %ls\n",
               Call, Key, WindowsGetStringRawBuffer(Value, NULL), Expected);
        }
    }
    WindowsDeleteString(Value);
    WindowsDeleteString(Name);
}

static IMapView_HSTRING_HSTRING *
GetSystemProperties(
    _In_ IAnalyticsInfoStatics2 *Statics2,
    _In_ UINT32 Count)
{
    IAsyncOperation_IMapView_HSTRING_HSTRING *Operation = NULL;
    IMapView_HSTRING_HSTRING *Map = NULL;
    AsyncStatus Status;
    HRESULT hr, Error;

    NameCount = Count;
    hr = IAnalyticsInfoStatics2_GetSystemPropertiesAsync(Statics2, &Names, &Operation);
    ok(hr == S_OK && Operation != NULL, "GetSystemPropertiesAsync(%u names) returned hr %#lx\n", Count, hr);
    if (FAILED(hr) || !Operation)
        return NULL;

    Status = WaitAsync((IUnknown *)Operation, 5000, &Error);
    ok(Status == Completed, "GetSystemPropertiesAsync(%u names) status %d, error %#lx\n", Count, Status, Error);
    hr = IAsyncOperation_IMapView_HSTRING_HSTRING_GetResults(Operation, &Map);
    ok(hr == S_OK && Map != NULL, "GetResults returned hr %#lx\n", hr);
    IAsyncOperation_IMapView_HSTRING_HSTRING_Release(Operation);
    return SUCCEEDED(hr) ? Map : NULL;
}

static void
TestSystemProperties(
    _In_ IAnalyticsInfoStatics2 *Statics2)
{
    const WCHAR *Family = sizeof(void *) == 8 ? L"Windows.Desktop" : NULL;
    IMapView_HSTRING_HSTRING *Map;
    WCHAR Image[MAX_PATH], *App;
    HSTRING Value = NULL, Name;
    UINT32 Size;

    GetModuleFileNameW(NULL, Image, ARRAYSIZE(Image));
    App = wcsrchr(Image, L'\\') ? wcsrchr(Image, L'\\') + 1 : Image;

    Map = GetSystemProperties(Statics2, ARRAYSIZE(PropertyNames));
    if (Map)
    {
        CheckProperty(Map, "named", L"App", App);
        CheckProperty(Map, "named", L"AppVer", NULL);
        CheckProperty(Map, "named", L"DeviceFamily", Family);
        CheckProperty(Map, "named", L"FlightRing", L"Retail");
        CheckProperty(Map, "named", L"LiberNTNoSuchAttribute", NULL);
        CheckProperty(Map, "named", L"AttrDataVer", NULL);
        Name = CreateHString(L"OSVersionFull");
        if (SUCCEEDED(IMapView_HSTRING_HSTRING_Lookup(Map, Name, &Value)))
            trace("OSVersionFull is %ls\n", WindowsGetStringRawBuffer(Value, NULL));
        else
            trace("OSVersionFull is absent, Windows 11 reports it to 64-bit callers\n");
        WindowsDeleteString(Value);
        WindowsDeleteString(Name);
        IMapView_HSTRING_HSTRING_Release(Map);
    }

    Map = GetSystemProperties(Statics2, 0);
    if (Map)
    {
        Size = 0;
        IMapView_HSTRING_HSTRING_get_Size(Map, &Size);
        trace("the default set has %u properties\n", Size);
        CheckProperty(Map, "default", L"App", App);
        CheckProperty(Map, "default", L"AttrDataVer", L"508");
        CheckProperty(Map, "default", L"DeviceFamily", Family);
        CheckProperty(Map, "default", L"FlightRing", L"Retail");
        CheckProperty(Map, "default", L"AppVer", NULL);
        IMapView_HSTRING_HSTRING_Release(Map);
    }
}

static void
CheckString(
    _In_ const char *Name,
    _In_ HRESULT hr,
    _In_ HSTRING Value,
    _In_ const WCHAR *Expected)
{
    const WCHAR *Text = WindowsGetStringRawBuffer(Value, NULL);

    ok(hr == S_OK, "%s returned hr %#lx\n", Name, hr);
    ok(!wcscmp(Text, Expected), "%s is %ls, expected %ls\n", Name, Text, Expected);
    WindowsDeleteString(Value);
}

static void
ExpectedFamilyVersion(
    _Out_writes_(Length) WCHAR *Text,
    _In_ SIZE_T Length)
{
    RTL_GET_VERSION GetVersionNt;
    RTL_OSVERSIONINFOW Version;
    DWORD Revision = 0, Size = sizeof(Revision);
    ULONGLONG Packed;

    Text[0] = UNICODE_NULL;
    GetVersionNt = (RTL_GET_VERSION)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (!GetVersionNt)
        return;
    Version.dwOSVersionInfoSize = sizeof(Version);
    if (GetVersionNt(&Version))
        return;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                 RRF_RT_REG_DWORD, NULL, &Revision, &Size);
    Packed = ((ULONGLONG)Version.dwMajorVersion << 48) | ((ULONGLONG)Version.dwMinorVersion << 32) |
             ((ULONGLONG)Version.dwBuildNumber << 16) | (Revision & 0xFFFF);
    _snwprintf(Text, Length, L"%I64u", Packed);
}

START_TEST(AnalyticsInfo)
{
    IActivationFactory *Factory;
    IAnalyticsInfoStatics *Statics;
    IAnalyticsInfoStatics2 *Statics2;
    IAnalyticsVersionInfo *VersionInfo;
    IAnalyticsVersionInfo2 *VersionInfo2;
    WCHAR ProductName[256];
    DWORD Size;
    IInspectable *Instance;
    TrustLevel Trust = FullTrust;
    WCHAR Expected[24];
    HSTRING Value;
    HRESULT hr;

    hr = RoInitialize(RO_INIT_MULTITHREADED);
    ok(hr == S_OK || hr == S_FALSE, "RoInitialize returned hr %#lx\n", hr);

    TraceActivatableClass(RuntimeClass_Windows_System_Profile_AnalyticsInfo);
    hr = GetFactory(RuntimeClass_Windows_System_Profile_AnalyticsInfo, &IID_IActivationFactory, (void **)&Factory);
    ok(hr == S_OK, "AnalyticsInfo factory failed, hr %#lx\n", hr);
    if (FAILED(hr))
        goto Done;

    hr = IActivationFactory_GetTrustLevel(Factory, &Trust);
    ok(hr == S_OK && Trust == BaseTrust, "trust level returned hr %#lx value %d\n", hr, Trust);

    Instance = (IInspectable *)(ULONG_PTR)1;
    hr = IActivationFactory_ActivateInstance(Factory, &Instance);
    ok(hr == E_NOTIMPL, "ActivateInstance returned hr %#lx\n", hr);

    Statics2 = NULL;
    hr = IActivationFactory_QueryInterface(Factory, &IID_IAnalyticsInfoStatics2, (void **)&Statics2);
    ok(hr == S_OK && Statics2 != NULL, "IAnalyticsInfoStatics2 query returned hr %#lx\n", hr);
    if (SUCCEEDED(hr) && Statics2)
    {
        TestSystemProperties(Statics2);
        IAnalyticsInfoStatics2_Release(Statics2);
    }

    hr = IActivationFactory_QueryInterface(Factory, &IID_IAnalyticsInfoStatics, (void **)&Statics);
    ok(hr == S_OK, "IAnalyticsInfoStatics query failed, hr %#lx\n", hr);
    IActivationFactory_Release(Factory);
    if (FAILED(hr))
        goto Done;

    Value = NULL;
    hr = IAnalyticsInfoStatics_get_DeviceForm(Statics, &Value);
    CheckString("DeviceForm", hr, Value, L"Unknown");

    VersionInfo = NULL;
    hr = IAnalyticsInfoStatics_get_VersionInfo(Statics, &VersionInfo);
    ok(hr == S_OK && VersionInfo != NULL, "VersionInfo returned hr %#lx\n", hr);
    if (SUCCEEDED(hr) && VersionInfo)
    {
        Value = NULL;
        hr = IAnalyticsVersionInfo_get_DeviceFamily(VersionInfo, &Value);
        CheckString("DeviceFamily", hr, Value, L"Windows.Desktop");

        ExpectedFamilyVersion(Expected, ARRAYSIZE(Expected));
        Value = NULL;
        hr = IAnalyticsVersionInfo_get_DeviceFamilyVersion(VersionInfo, &Value);
        CheckString("DeviceFamilyVersion", hr, Value, Expected);

        VersionInfo2 = NULL;
        hr = IAnalyticsVersionInfo_QueryInterface(VersionInfo, &IID_IAnalyticsVersionInfo2, (void **)&VersionInfo2);
        ok(hr == S_OK && VersionInfo2 != NULL, "IAnalyticsVersionInfo2 query returned hr %#lx\n", hr);
        if (SUCCEEDED(hr) && VersionInfo2)
        {
            Size = sizeof(ProductName);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName",
                             RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, ProductName, &Size))
            {
                ProductName[0] = UNICODE_NULL;
            }
            Value = NULL;
            hr = IAnalyticsVersionInfo2_get_ProductName(VersionInfo2, &Value);
            CheckString("ProductName", hr, Value, ProductName);
            IAnalyticsVersionInfo2_Release(VersionInfo2);
        }
        IAnalyticsVersionInfo_Release(VersionInfo);
    }
    IAnalyticsInfoStatics_Release(Statics);

Done:
    RoUninitialize();
}
