/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the AppModel runtime API-set host
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <appmodel.h>

typedef LONG (WINAPI *PFN_VERIFYSTRING)(PCWSTR);
typedef LONG (WINAPI *PFN_VERIFYID)(const PACKAGE_ID *);
typedef LONG (WINAPI *PFN_FORMATAUMID)(PCWSTR, PCWSTR, UINT32 *, PWSTR);
typedef LONG (WINAPI *PFN_GETAUMID)(HANDLE, UINT32 *, PWSTR);
typedef LONG (WINAPI *PFN_GETCURRENTINFO2)(UINT32, PackagePathType, UINT32 *, BYTE *, UINT32 *);
typedef LONG (WINAPI *PFN_GETCURRENTPATH2)(PackagePathType, UINT32 *, PWSTR);
typedef LONG (WINAPI *PFN_FAMILYFROMID)(const PACKAGE_ID *, UINT32 *, PWSTR);
typedef LONG (WINAPI *PFN_FULLFROMID)(const PACKAGE_ID *, UINT32 *, PWSTR);
typedef LONG (WINAPI *PFN_IDFROMFULL)(PCWSTR, UINT32, UINT32 *, BYTE *);
typedef LONG (WINAPI *PFN_NAMEFROMFAMILY)(PCWSTR, UINT32 *, PWSTR, UINT32 *, PWSTR);
typedef LONG (WINAPI *PFN_POLICY)(HANDLE, PVOID);

static FARPROC
FindAppModelExport(PCSTR Name)
{
    static HMODULE Module;

    if (!Module)
    {
        Module = LoadLibraryW(L"api-ms-win-appmodel-runtime-l1-1-7.dll");
        if (!Module)
            Module = LoadLibraryW(L"kernel.appcore.dll");
    }
    return Module ? GetProcAddress(Module, Name) : NULL;
}

static VOID
TestVerifyFunctions(VOID)
{
    PFN_VERIFYSTRING VerifyApplicationUserModelId;
    PFN_VERIFYSTRING VerifyPackageFamilyName;
    PFN_VERIFYSTRING VerifyPackageFullName;
    PFN_VERIFYSTRING VerifyPackageRelativeApplicationId;
    PFN_VERIFYID VerifyPackageId;
    PACKAGE_ID Id;

    VerifyApplicationUserModelId = (PFN_VERIFYSTRING)FindAppModelExport("VerifyApplicationUserModelId");
    VerifyPackageFamilyName = (PFN_VERIFYSTRING)FindAppModelExport("VerifyPackageFamilyName");
    VerifyPackageFullName = (PFN_VERIFYSTRING)FindAppModelExport("VerifyPackageFullName");
    VerifyPackageRelativeApplicationId = (PFN_VERIFYSTRING)FindAppModelExport("VerifyPackageRelativeApplicationId");
    VerifyPackageId = (PFN_VERIFYID)FindAppModelExport("VerifyPackageId");
    ok(VerifyApplicationUserModelId != NULL, "VerifyApplicationUserModelId is missing\n");
    ok(VerifyPackageFamilyName != NULL, "VerifyPackageFamilyName is missing\n");
    ok(VerifyPackageFullName != NULL, "VerifyPackageFullName is missing\n");
    ok(VerifyPackageRelativeApplicationId != NULL, "VerifyPackageRelativeApplicationId is missing\n");
    ok(VerifyPackageId != NULL, "VerifyPackageId is missing\n");
    if (!VerifyApplicationUserModelId || !VerifyPackageFamilyName || !VerifyPackageFullName ||
        !VerifyPackageRelativeApplicationId || !VerifyPackageId)
        return;

    ok(VerifyPackageFamilyName(L"Contoso.App_8wekyb3d8bbwe") == ERROR_SUCCESS, "valid family rejected\n");
    ok(VerifyPackageFamilyName(L"Contoso.App_bad") == ERROR_INVALID_PARAMETER, "invalid family accepted\n");
    ok(VerifyPackageFullName(L"Contoso.App_1.2.3.4_x64__8wekyb3d8bbwe") == ERROR_SUCCESS, "valid full name rejected\n");
    ok(VerifyPackageFullName(L"Contoso.App_1.2.3_x64__8wekyb3d8bbwe") == ERROR_INVALID_PARAMETER, "invalid full name accepted\n");
    ok(VerifyPackageRelativeApplicationId(L"App.Main2") == ERROR_SUCCESS, "valid relative ID rejected\n");
    ok(VerifyPackageRelativeApplicationId(L"2App") == ERROR_INVALID_PARAMETER, "invalid relative ID accepted\n");
    ok(VerifyApplicationUserModelId(L"Contoso.App_8wekyb3d8bbwe!App.Main2") == ERROR_SUCCESS, "valid AUMID rejected\n");
    ok(VerifyApplicationUserModelId(L"Contoso.App_8wekyb3d8bbwe!") == ERROR_INVALID_PARAMETER, "invalid AUMID accepted\n");

    ZeroMemory(&Id, sizeof(Id));
    Id.processorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
    Id.version.Major = 1;
    Id.version.Minor = 2;
    Id.version.Build = 3;
    Id.version.Revision = 4;
    Id.name = L"Contoso.App";
    Id.resourceId = L"";
    Id.publisherId = L"8wekyb3d8bbwe";
    ok(VerifyPackageId(&Id) == ERROR_SUCCESS, "valid package ID rejected\n");
    Id.reserved = 1;
    ok(VerifyPackageId(&Id) == ERROR_INVALID_PARAMETER, "reserved package ID accepted\n");
}

static VOID
TestConversions(VOID)
{
    static const WCHAR FullName[] = L"Contoso.App_1.2.3.4_x64__8wekyb3d8bbwe";
    static const WCHAR Family[] = L"Contoso.App_8wekyb3d8bbwe";
    PFN_FORMATAUMID FormatApplicationUserModelId;
    PFN_FAMILYFROMID PackageFamilyNameFromId;
    PFN_FULLFROMID PackageFullNameFromId;
    PFN_IDFROMFULL PackageIdFromFullName;
    PFN_NAMEFROMFAMILY PackageNameAndPublisherIdFromFamilyName;
    UINT32 Length, NameLength, PublisherLength;
    BYTE *Buffer;
    PACKAGE_ID *Id;
    WCHAR Value[160], Name[64], Publisher[32];
    LONG Result;

    FormatApplicationUserModelId = (PFN_FORMATAUMID)FindAppModelExport("FormatApplicationUserModelId");
    PackageFamilyNameFromId = (PFN_FAMILYFROMID)FindAppModelExport("PackageFamilyNameFromId");
    PackageFullNameFromId = (PFN_FULLFROMID)FindAppModelExport("PackageFullNameFromId");
    PackageIdFromFullName = (PFN_IDFROMFULL)FindAppModelExport("PackageIdFromFullName");
    PackageNameAndPublisherIdFromFamilyName = (PFN_NAMEFROMFAMILY)FindAppModelExport("PackageNameAndPublisherIdFromFamilyName");
    ok(FormatApplicationUserModelId != NULL, "FormatApplicationUserModelId is missing\n");
    ok(PackageFamilyNameFromId != NULL, "PackageFamilyNameFromId is missing\n");
    ok(PackageFullNameFromId != NULL, "PackageFullNameFromId is missing\n");
    ok(PackageIdFromFullName != NULL, "PackageIdFromFullName is missing\n");
    ok(PackageNameAndPublisherIdFromFamilyName != NULL, "PackageNameAndPublisherIdFromFamilyName is missing\n");
    if (!FormatApplicationUserModelId || !PackageFamilyNameFromId || !PackageFullNameFromId ||
        !PackageIdFromFullName || !PackageNameAndPublisherIdFromFamilyName)
        return;

    Length = _countof(Value);
    Result = FormatApplicationUserModelId(Family, L"App.Main", &Length, Value);
    ok(Result == ERROR_SUCCESS, "FormatApplicationUserModelId returned %ld\n", Result);
    ok(!wcscmp(Value, L"Contoso.App_8wekyb3d8bbwe!App.Main"), "unexpected AUMID %s\n", wine_dbgstr_w(Value));

    NameLength = _countof(Name);
    PublisherLength = _countof(Publisher);
    Result = PackageNameAndPublisherIdFromFamilyName(Family, &NameLength, Name, &PublisherLength, Publisher);
    ok(Result == ERROR_SUCCESS, "PackageNameAndPublisherIdFromFamilyName returned %ld\n", Result);
    ok(!wcscmp(Name, L"Contoso.App"), "unexpected name %s\n", wine_dbgstr_w(Name));
    ok(!wcscmp(Publisher, L"8wekyb3d8bbwe"), "unexpected publisher ID %s\n", wine_dbgstr_w(Publisher));

    Length = 0;
    Result = PackageIdFromFullName(FullName, PACKAGE_INFORMATION_BASIC, &Length, NULL);
    ok(Result == ERROR_INSUFFICIENT_BUFFER, "PackageIdFromFullName size returned %ld\n", Result);
    Buffer = HeapAlloc(GetProcessHeap(), 0, Length);
    ok(Buffer != NULL, "HeapAlloc failed\n");
    if (!Buffer)
        return;
    Result = PackageIdFromFullName(FullName, PACKAGE_INFORMATION_BASIC, &Length, Buffer);
    ok(Result == ERROR_SUCCESS, "PackageIdFromFullName returned %ld\n", Result);
    Id = (PACKAGE_ID *)Buffer;
    ok(!wcscmp(Id->name, L"Contoso.App"), "unexpected ID name %s\n", wine_dbgstr_w(Id->name));
    ok(Id->processorArchitecture == PROCESSOR_ARCHITECTURE_AMD64, "unexpected architecture %u\n", Id->processorArchitecture);
    ok(Id->version.Major == 1 && Id->version.Minor == 2 && Id->version.Build == 3 && Id->version.Revision == 4,
       "unexpected version %u.%u.%u.%u\n", Id->version.Major, Id->version.Minor, Id->version.Build, Id->version.Revision);

    Length = _countof(Value);
    Result = PackageFamilyNameFromId(Id, &Length, Value);
    ok(Result == ERROR_SUCCESS, "PackageFamilyNameFromId returned %ld\n", Result);
    ok(!wcscmp(Value, Family), "unexpected family %s\n", wine_dbgstr_w(Value));
    Length = _countof(Value);
    Result = PackageFullNameFromId(Id, &Length, Value);
    ok(Result == ERROR_SUCCESS, "PackageFullNameFromId returned %ld\n", Result);
    ok(!wcscmp(Value, FullName), "unexpected full name %s\n", wine_dbgstr_w(Value));
    Length = 0;
    Result = PackageIdFromFullName(FullName, PACKAGE_INFORMATION_FULL, &Length, NULL);
    ok(Result == ERROR_NOT_FOUND, "full PackageIdFromFullName returned %ld\n", Result);
    HeapFree(GetProcessHeap(), 0, Buffer);
}

static VOID
TestUnpackagedAndPolicy(VOID)
{
    static const struct
    {
        PCSTR Name;
        UINT32 Expected;
    } Policies[] =
    {
        { "AppPolicyGetClrCompat", AppPolicyClrCompat_ClassicDesktop },
        { "AppPolicyGetCreateFileAccess", AppPolicyCreateFileAccess_Full },
        { "AppPolicyGetLifecycleManagement", AppPolicyLifecycleManagement_Unmanaged },
        { "AppPolicyGetMediaFoundationCodecLoading", AppPolicyMediaFoundationCodecLoading_All },
        { "AppPolicyGetProcessTerminationMethod", AppPolicyProcessTerminationMethod_ExitProcess },
        { "AppPolicyGetShowDeveloperDiagnostic", AppPolicyShowDeveloperDiagnostic_None },
        { "AppPolicyGetThreadInitializationType", AppPolicyThreadInitializationType_None },
        { "AppPolicyGetWindowingModel", AppPolicyWindowingModel_ClassicDesktop }
    };
    PFN_GETAUMID GetApplicationUserModelId;
    PFN_GETCURRENTINFO2 GetCurrentPackageInfo2;
    PFN_GETCURRENTPATH2 GetCurrentPackagePath2;
    HANDLE Token;
    UINT32 Index, Length, Count, Value;
    LONG Result;

    GetApplicationUserModelId = (PFN_GETAUMID)FindAppModelExport("GetApplicationUserModelId");
    GetCurrentPackageInfo2 = (PFN_GETCURRENTINFO2)FindAppModelExport("GetCurrentPackageInfo2");
    GetCurrentPackagePath2 = (PFN_GETCURRENTPATH2)FindAppModelExport("GetCurrentPackagePath2");
    ok(GetApplicationUserModelId != NULL, "GetApplicationUserModelId is missing\n");
    ok(GetCurrentPackageInfo2 != NULL, "GetCurrentPackageInfo2 is missing\n");
    ok(GetCurrentPackagePath2 != NULL, "GetCurrentPackagePath2 is missing\n");
    if (GetApplicationUserModelId)
    {
        Length = 0;
        Result = GetApplicationUserModelId(GetCurrentProcess(), &Length, NULL);
        ok(Result == APPMODEL_ERROR_NO_APPLICATION, "GetApplicationUserModelId returned %ld\n", Result);
    }
    if (GetCurrentPackageInfo2)
    {
        Length = Count = 0;
        Result = GetCurrentPackageInfo2(PACKAGE_FILTER_HEAD, PackagePathType_Install, &Length, NULL, &Count);
        ok(Result == APPMODEL_ERROR_NO_PACKAGE, "GetCurrentPackageInfo2 returned %ld\n", Result);
    }
    if (GetCurrentPackagePath2)
    {
        Length = 0;
        Result = GetCurrentPackagePath2(PackagePathType_Install, &Length, NULL);
        ok(Result == APPMODEL_ERROR_NO_PACKAGE, "GetCurrentPackagePath2 returned %ld\n", Result);
    }

    Result = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token);
    ok(Result != FALSE, "OpenProcessToken failed %lu\n", GetLastError());
    if (!Result)
        return;
    for (Index = 0; Index < _countof(Policies); ++Index)
    {
        PFN_POLICY Function = (PFN_POLICY)FindAppModelExport(Policies[Index].Name);
        ok(Function != NULL, "%s is missing\n", Policies[Index].Name);
        if (!Function)
            continue;
        Value = MAXUINT32;
        Result = Function(Token, &Value);
        ok(Result == ERROR_SUCCESS, "%s returned %ld\n", Policies[Index].Name, Result);
        ok(Value == Policies[Index].Expected, "%s returned value %u\n", Policies[Index].Name, Value);
        ok(Function(NULL, &Value) == ERROR_INVALID_PARAMETER, "%s accepted null token\n", Policies[Index].Name);
        ok(Function(Token, NULL) == ERROR_INVALID_PARAMETER, "%s accepted null output\n", Policies[Index].Name);
    }
    CloseHandle(Token);
}

START_TEST(AppModelRuntime)
{
    TestVerifyFunctions();
    TestConversions();
    TestUnpackagedAndPolicy();
}
