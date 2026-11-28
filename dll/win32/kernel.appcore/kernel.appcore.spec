@ stdcall AddPackageDependency(wstr long long ptr ptr)
@ stdcall AddPackageDependency2(wstr long long ptr ptr)
@ stdcall AppPolicyGetClrCompat(ptr ptr)
@ stdcall AppPolicyGetCreateFileAccess(ptr ptr)
@ stdcall AppPolicyGetLifecycleManagement(ptr ptr)
@ stdcall AppPolicyGetMediaFoundationCodecLoading(ptr ptr) kernelbase.AppPolicyGetMediaFoundationCodecLoading
@ stdcall AppPolicyGetProcessTerminationMethod(ptr ptr) kernelbase.AppPolicyGetProcessTerminationMethod
@ stdcall AppPolicyGetShowDeveloperDiagnostic(ptr ptr) kernelbase.AppPolicyGetShowDeveloperDiagnostic
@ stdcall AppPolicyGetThreadInitializationType(ptr ptr) kernelbase.AppPolicyGetThreadInitializationType
@ stdcall AppPolicyGetWindowingModel(ptr ptr) kernelbase.AppPolicyGetWindowingModel
@ stdcall CheckIsMSIXPackage(wstr ptr)
@ stdcall ClosePackageInfo(ptr)
@ stdcall DeletePackageDependency(wstr)
@ stdcall FindPackageDependency(ptr ptr ptr)
@ stdcall FindPackagesByPackageFamily(wstr long ptr ptr ptr ptr ptr) kernelbase.FindPackagesByPackageFamily
@ stdcall FormatApplicationUserModelId(wstr wstr ptr ptr)
@ stdcall GetApplicationUserModelId(ptr ptr ptr)
@ stdcall GetApplicationUserModelIdFromToken(ptr ptr ptr) kernelbase.GetApplicationUserModelIdFromToken
@ stdcall GetCurrentApplicationUserModelId(ptr ptr) kernelbase.GetCurrentApplicationUserModelId
@ stdcall GetCurrentPackageFamilyName(ptr ptr) kernelbase.GetCurrentPackageFamilyName
@ stdcall GetCurrentPackageFullName(ptr ptr) kernel32.GetCurrentPackageFullName
@ stdcall GetCurrentPackageId(ptr ptr) kernel32.GetCurrentPackageId
@ stdcall GetCurrentPackageInfo(long ptr ptr ptr) kernelbase.GetCurrentPackageInfo
@ stdcall GetCurrentPackageInfo2(long long ptr ptr ptr)
@ stdcall GetCurrentPackagePath(ptr ptr) kernelbase.GetCurrentPackagePath
@ stdcall GetCurrentPackagePath2(long ptr ptr)
@ stdcall GetIdForPackageDependencyContext(ptr ptr)
@ stdcall GetPackageApplicationIds(ptr ptr ptr ptr)
@ stdcall GetPackageDependencyInformation(wstr ptr ptr ptr ptr ptr ptr ptr ptr)
@ stdcall GetPackageFamilyName(ptr ptr ptr) kernelbase.GetPackageFamilyName
@ stdcall GetPackageFamilyNameFromToken(ptr ptr ptr) kernelbase.GetPackageFamilyNameFromToken
@ stdcall GetPackageFullName(ptr ptr ptr) kernelbase.GetPackageFullName
@ stdcall GetPackageFullNameFromToken(ptr ptr ptr) kernelbase.GetPackageFullNameFromToken
@ stdcall GetPackageGraphRevisionId()
@ stdcall GetPackageId(ptr ptr ptr)
@ stdcall GetPackageInfo(ptr long ptr ptr ptr)
@ stdcall GetPackageInfo2(ptr long long ptr ptr ptr)
@ stdcall GetPackagePath(ptr long ptr ptr)
@ stdcall GetPackagePathByFullName(wstr ptr ptr) kernelbase.GetPackagePathByFullName
@ stdcall GetPackagePathByFullName2(wstr long ptr ptr)
@ stdcall GetPackagesByPackageFamily(wstr ptr ptr ptr ptr) kernelbase.GetPackagesByPackageFamily
@ stdcall GetProcessesUsingPackageDependency(wstr ptr long ptr ptr)
@ stdcall GetResolvedPackageFullNameForPackageDependency(wstr ptr)
@ stdcall GetResolvedPackageFullNameForPackageDependency2(wstr ptr)
@ stdcall GetStagedPackageOrigin(wstr ptr)
@ stdcall GetStagedPackagePathByFullName(wstr ptr ptr)
@ stdcall GetStagedPackagePathByFullName2(wstr long ptr ptr)
@ stdcall OpenPackageInfoByFullName(wstr long ptr)
@ stdcall OpenPackageInfoByFullNameForUser(ptr wstr long ptr)
@ stdcall PackageFamilyNameFromFullName(wstr ptr ptr) kernelbase.PackageFamilyNameFromFullName
@ stdcall PackageFamilyNameFromId(ptr ptr ptr)
@ stdcall PackageFullNameFromId(ptr ptr ptr)
@ stdcall PackageIdFromFullName(wstr long ptr ptr) kernelbase.PackageIdFromFullName
@ stdcall PackageNameAndPublisherIdFromFamilyName(wstr ptr ptr ptr ptr)
@ stdcall ParseApplicationUserModelId(wstr ptr ptr ptr ptr) kernelbase.ParseApplicationUserModelId
@ stdcall RemovePackageDependency(ptr)
@ stdcall TryCreatePackageDependency(ptr wstr int64 long long wstr long ptr)
@ stdcall TryCreatePackageDependency2(ptr wstr int64 long long wstr long ptr ptr)
@ stdcall VerifyApplicationUserModelId(wstr)
@ stdcall VerifyPackageFamilyName(wstr)
@ stdcall VerifyPackageFullName(wstr)
@ stdcall VerifyPackageId(ptr)
@ stdcall VerifyPackageRelativeApplicationId(wstr)
