/*
 * PROJECT:     LiberNT AppModel Runtime
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Windows AppModel runtime host
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "appcore.h"

#define APPCORE_LONG_STUB(Name, Parameters) LONG WINAPI Name Parameters { return ERROR_CALL_NOT_IMPLEMENTED; }
#define APPCORE_HRESULT_STUB(Name, Parameters) HRESULT WINAPI Name Parameters { return E_NOTIMPL; }

APPCORE_HRESULT_STUB(AddPackageDependency, (PCWSTR Id, INT32 Rank, AddPackageDependencyOptions Options, PACKAGEDEPENDENCY_CONTEXT *Context, PWSTR *FullName))
APPCORE_HRESULT_STUB(AddPackageDependency2, (PCWSTR Id, INT32 Rank, AddPackageDependencyOptions2 Options, PACKAGEDEPENDENCY_CONTEXT *Context, PWSTR *FullName))
APPCORE_LONG_STUB(AppPolicyGetClrCompat, (HANDLE Token, AppPolicyClrCompat *Policy))
APPCORE_LONG_STUB(AppPolicyGetCreateFileAccess, (HANDLE Token, AppPolicyCreateFileAccess *Policy))
APPCORE_LONG_STUB(AppPolicyGetLifecycleManagement, (HANDLE Token, AppPolicyLifecycleManagement *Policy))
APPCORE_LONG_STUB(AppPolicyGetMediaFoundationCodecLoading, (HANDLE Token, AppPolicyMediaFoundationCodecLoading *Policy))
APPCORE_LONG_STUB(AppPolicyGetProcessTerminationMethod, (HANDLE Token, AppPolicyProcessTerminationMethod *Policy))
APPCORE_LONG_STUB(AppPolicyGetShowDeveloperDiagnostic, (HANDLE Token, AppPolicyShowDeveloperDiagnostic *Policy))
APPCORE_LONG_STUB(AppPolicyGetThreadInitializationType, (HANDLE Token, AppPolicyThreadInitializationType *Policy))
APPCORE_LONG_STUB(AppPolicyGetWindowingModel, (HANDLE Token, AppPolicyWindowingModel *Policy))
APPCORE_HRESULT_STUB(CheckIsMSIXPackage, (PCWSTR FullName, BOOL *IsMsix))
APPCORE_LONG_STUB(ClosePackageInfo, (PACKAGE_INFO_REFERENCE Reference))
APPCORE_HRESULT_STUB(DeletePackageDependency, (PCWSTR Id))
APPCORE_HRESULT_STUB(FindPackageDependency, (const FindPackageDependencyCriteria *Criteria, UINT32 *Count, PWSTR **Ids))
APPCORE_LONG_STUB(FormatApplicationUserModelId, (PCWSTR Family, PCWSTR RelativeId, UINT32 *Length, PWSTR Value))
APPCORE_LONG_STUB(GetApplicationUserModelId, (HANDLE Process, UINT32 *Length, PWSTR Value))
APPCORE_LONG_STUB(GetCurrentPackageInfo2, (UINT32 Flags, PackagePathType Type, UINT32 *Length, BYTE *Buffer, UINT32 *Count))
APPCORE_LONG_STUB(GetCurrentPackagePath2, (PackagePathType Type, UINT32 *Length, PWSTR Path))
APPCORE_HRESULT_STUB(GetIdForPackageDependencyContext, (PACKAGEDEPENDENCY_CONTEXT Context, PWSTR *Id))
APPCORE_LONG_STUB(GetPackageApplicationIds, (PACKAGE_INFO_REFERENCE Reference, UINT32 *Length, BYTE *Buffer, UINT32 *Count))
APPCORE_HRESULT_STUB(GetPackageDependencyInformation, (PCWSTR Id, PSID *User, PWSTR *Family, PACKAGE_VERSION *Version, PackageDependencyProcessorArchitectures *Architectures, PackageDependencyLifetimeKind *Lifetime, PWSTR *Artifact, CreatePackageDependencyOptions *Options, FILETIME *Expiration))
APPCORE_LONG_STUB(GetPackageInfo, (PACKAGE_INFO_REFERENCE Reference, UINT32 Flags, UINT32 *Length, BYTE *Buffer, UINT32 *Count))
APPCORE_LONG_STUB(GetPackageInfo2, (PACKAGE_INFO_REFERENCE Reference, UINT32 Flags, PackagePathType Type, UINT32 *Length, BYTE *Buffer, UINT32 *Count))
APPCORE_LONG_STUB(GetPackagePath, (const PACKAGE_ID *Id, UINT32 Reserved, UINT32 *Length, PWSTR Path))
APPCORE_LONG_STUB(GetPackagePathByFullName2, (PCWSTR FullName, PackagePathType Type, UINT32 *Length, PWSTR Path))
APPCORE_HRESULT_STUB(GetProcessesUsingPackageDependency, (PCWSTR Id, PSID User, BOOL System, UINT32 *Count, DWORD **Processes))
APPCORE_HRESULT_STUB(GetResolvedPackageFullNameForPackageDependency, (PCWSTR Id, PWSTR *FullName))
APPCORE_HRESULT_STUB(GetResolvedPackageFullNameForPackageDependency2, (PCWSTR Id, PWSTR *FullName))
APPCORE_LONG_STUB(GetStagedPackageOrigin, (PCWSTR FullName, PackageOrigin *Origin))
APPCORE_LONG_STUB(GetStagedPackagePathByFullName, (PCWSTR FullName, UINT32 *Length, PWSTR Path))
APPCORE_LONG_STUB(GetStagedPackagePathByFullName2, (PCWSTR FullName, PackagePathType Type, UINT32 *Length, PWSTR Path))
APPCORE_LONG_STUB(OpenPackageInfoByFullName, (PCWSTR FullName, UINT32 Reserved, PACKAGE_INFO_REFERENCE *Reference))
APPCORE_LONG_STUB(OpenPackageInfoByFullNameForUser, (PSID User, PCWSTR FullName, UINT32 Reserved, PACKAGE_INFO_REFERENCE *Reference))
APPCORE_LONG_STUB(PackageFamilyNameFromId, (const PACKAGE_ID *Id, UINT32 *Length, PWSTR Family))
APPCORE_LONG_STUB(PackageFullNameFromId, (const PACKAGE_ID *Id, UINT32 *Length, PWSTR FullName))
APPCORE_LONG_STUB(PackageIdFromFullName, (PCWSTR FullName, UINT32 Flags, UINT32 *Length, BYTE *Buffer))
APPCORE_LONG_STUB(PackageNameAndPublisherIdFromFamilyName, (PCWSTR Family, UINT32 *NameLength, PWSTR Name, UINT32 *PublisherLength, PWSTR Publisher))
APPCORE_HRESULT_STUB(RemovePackageDependency, (PACKAGEDEPENDENCY_CONTEXT Context))
APPCORE_HRESULT_STUB(TryCreatePackageDependency, (PSID User, PCWSTR Family, PACKAGE_VERSION Version, PackageDependencyProcessorArchitectures Architectures, PackageDependencyLifetimeKind Lifetime, PCWSTR Artifact, CreatePackageDependencyOptions Options, PWSTR *Id))
APPCORE_HRESULT_STUB(TryCreatePackageDependency2, (PSID User, PCWSTR Family, PACKAGE_VERSION Version, PackageDependencyProcessorArchitectures Architectures, PackageDependencyLifetimeKind Lifetime, PCWSTR Artifact, CreatePackageDependencyOptions Options, const FILETIME *Expiration, PWSTR *Id))
APPCORE_LONG_STUB(VerifyApplicationUserModelId, (PCWSTR Value))
APPCORE_LONG_STUB(VerifyPackageFamilyName, (PCWSTR Value))
APPCORE_LONG_STUB(VerifyPackageFullName, (PCWSTR Value))
APPCORE_LONG_STUB(VerifyPackageId, (const PACKAGE_ID *Id))
APPCORE_LONG_STUB(VerifyPackageRelativeApplicationId, (PCWSTR Value))

LONG
WINAPI
GetPackageId(
    _In_ HANDLE Process,
    _Inout_ UINT32 *Length,
    _Out_writes_bytes_opt_(*Length) BYTE *Buffer)
{
    UNREFERENCED_PARAMETER(Buffer);

    if (!Process || !Length)
        return ERROR_INVALID_PARAMETER;
    if (!GetProcessId(Process))
        return GetLastError();
    return APPMODEL_ERROR_NO_PACKAGE;
}

UINT32
WINAPI
GetPackageGraphRevisionId(VOID)
{
    return 0;
}
