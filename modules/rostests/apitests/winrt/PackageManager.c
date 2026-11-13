/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests Windows.Management.Deployment.PackageManager queries for packages that are not installed
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "winrt_apitest.h"

#define WIDL_using_Windows_ApplicationModel
#include <windows.applicationmodel.h>
#define WIDL_using_Windows_Management_Deployment
#include <windows.management.deployment.h>

static const WCHAR MissingFamily[] = L"Microsoft.Office.Desktop_8wekyb3d8bbwe";

static void
CheckEmpty(
    _In_ const char *Name,
    _In_ HRESULT hr,
    _In_opt_ IIterable_Package *Packages)
{
    IIterator_Package *Iterator;
    IPackage *Items[1];
    boolean HasCurrent = TRUE;
    UINT32 Count = 1;

    ok(hr == S_OK, "%s returned hr %#lx\n", Name, hr);
    ok(Packages != NULL, "%s returned no iterable\n", Name);
    if (FAILED(hr) || !Packages)
        return;
    hr = IIterable_Package_First(Packages, &Iterator);
    ok(hr == S_OK, "%s First returned hr %#lx\n", Name, hr);
    if (SUCCEEDED(hr))
    {
        hr = IIterator_Package_get_HasCurrent(Iterator, &HasCurrent);
        ok(hr == S_OK && !HasCurrent, "%s HasCurrent returned hr %#lx value %d\n", Name, hr, HasCurrent);
        hr = IIterator_Package_GetMany(Iterator, ARRAYSIZE(Items), Items, &Count);
        ok(hr == S_OK && Count == 0, "%s GetMany returned hr %#lx count %u\n", Name, hr, Count);
        IIterator_Package_Release(Iterator);
    }
    IIterable_Package_Release(Packages);
}

typedef LONG (WINAPI *GET_PACKAGES_BY_FAMILY)(const WCHAR *, UINT32 *, WCHAR **, UINT32 *, WCHAR *);

static void
TestPackagesByFamily(void)
{
    GET_PACKAGES_BY_FAMILY GetPackages;
    UINT32 Count, Length;
    LONG Error;

    GetPackages = (GET_PACKAGES_BY_FAMILY)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetPackagesByPackageFamily");
    if (!GetPackages)
    {
        skip("GetPackagesByPackageFamily is not exported\n");
        return;
    }

    Count = Length = 0;
    Error = GetPackages(MissingFamily, &Count, NULL, &Length, NULL);
    ok(Error == ERROR_SUCCESS && Count == 0 && Length == 0,
       "missing family: error %ld count %u length %u\n", Error, Count, Length);

    Count = 1;
    Length = 0;
    Error = GetPackages(MissingFamily, &Count, NULL, &Length, NULL);
    ok(Error == ERROR_INVALID_PARAMETER && Count == 1, "count without names: error %ld count %u\n", Error, Count);

    Count = 0;
    Length = 16;
    Error = GetPackages(MissingFamily, &Count, NULL, &Length, NULL);
    ok(Error == ERROR_INVALID_PARAMETER && Length == 16, "length without buffer: error %ld length %u\n", Error, Length);
}

START_TEST(PackageManager)
{
    IPackageManager *Manager;
    IIterable_Package *Packages;
    IInspectable *Instance;
    HSTRING Class, Family, User, Name, Publisher;
    TrustLevel Trust = BaseTrust;
    HRESULT hr;

    TestPackagesByFamily();

    hr = RoInitialize(RO_INIT_MULTITHREADED);
    ok(hr == S_OK || hr == S_FALSE, "RoInitialize returned hr %#lx\n", hr);

    TraceActivatableClass(RuntimeClass_Windows_Management_Deployment_PackageManager);
    Class = CreateHString(RuntimeClass_Windows_Management_Deployment_PackageManager);
    hr = RoActivateInstance(Class, &Instance);
    WindowsDeleteString(Class);
    ok(hr == S_OK, "PackageManager activation failed, hr %#lx\n", hr);
    if (FAILED(hr))
        goto Done;

    hr = IInspectable_GetTrustLevel(Instance, &Trust);
    ok(hr == S_OK && Trust == PartialTrust, "trust level returned hr %#lx value %d\n", hr, Trust);

    hr = IInspectable_QueryInterface(Instance, &IID_IPackageManager, (void **)&Manager);
    ok(hr == S_OK, "IPackageManager query failed, hr %#lx\n", hr);
    IInspectable_Release(Instance);
    if (FAILED(hr))
        goto Done;

    Family = CreateHString(MissingFamily);
    User = CreateHString(L"");
    Name = CreateHString(L"Microsoft.Office.Desktop");
    Publisher = CreateHString(L"CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US");

    Packages = NULL;
    hr = IPackageManager_FindPackagesByUserSecurityIdPackageFamilyName(Manager, User, Family, &Packages);
    CheckEmpty("FindPackagesForUser(family)", hr, Packages);

    Packages = NULL;
    hr = IPackageManager_FindPackagesByUserSecurityIdNamePublisher(Manager, User, Name, Publisher, &Packages);
    CheckEmpty("FindPackagesForUser(name, publisher)", hr, Packages);

    WindowsDeleteString(Publisher);
    WindowsDeleteString(Name);
    WindowsDeleteString(User);
    WindowsDeleteString(Family);
    IPackageManager_Release(Manager);

Done:
    RoUninitialize();
}
