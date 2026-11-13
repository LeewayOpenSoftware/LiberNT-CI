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

DEFINE_GUID(IID_IAnalyticsInfoStatics2_Win11, 0x101704ea, 0xa7f9, 0x46d2, 0xab, 0x94, 0x01, 0x68, 0x65, 0xaf, 0xdb, 0x25);
DEFINE_GUID(IID_IAnalyticsVersionInfo2_Win11, 0x76e915b1, 0xff36, 0x407c, 0x9f, 0x57, 0x16, 0x0d, 0x3e, 0x54, 0x07, 0x47);

typedef LONG (WINAPI *RTL_GET_VERSION)(RTL_OSVERSIONINFOW *);

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
    IAnalyticsVersionInfo *VersionInfo;
    IInspectable *Instance, *Other;
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

    Other = NULL;
    hr = IActivationFactory_QueryInterface(Factory, &IID_IAnalyticsInfoStatics2_Win11, (void **)&Other);
    trace("IAnalyticsInfoStatics2 query returned hr %#lx, Windows 11 returns S_OK\n", hr);
    if (Other)
        IInspectable_Release(Other);

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

        Other = NULL;
        hr = IAnalyticsVersionInfo_QueryInterface(VersionInfo, &IID_IAnalyticsVersionInfo2_Win11, (void **)&Other);
        trace("IAnalyticsVersionInfo2 query returned hr %#lx, Windows 11 returns S_OK\n", hr);
        if (Other)
            IInspectable_Release(Other);
        IAnalyticsVersionInfo_Release(VersionInfo);
    }
    IAnalyticsInfoStatics_Release(Statics);

Done:
    RoUninitialize();
}
