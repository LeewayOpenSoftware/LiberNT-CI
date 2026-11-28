
list(APPEND SOURCE
    BuildDriverInfoBadInf.c
    BuildDriverInfoInstalledFlag.c
    DelRegRootGuard.c
    QueueHandleGuard.c
    DriverStoreVerifyHashOnce.c
    AcpiDeviceCapabilities.c
    ClassInstallParams.c
    devclass.c
    DeviceDescription.c
    DriverFeatureScore.c
    DriverPackageSignerScore.c
    DevicePdoName.c
    DevicePowerOnDisable.c
    DeviceRemoveVeto.c
    HostBridgeStart.c
    InboxClasses.c
    InterruptAffinityPolicy.c
    InvalidDeviceInfoSet.c
    LegacyDeviceParent.c
    NetInterfaceNames.c
    NewDeviceInstall.c
    SetupCommitFileQueue.c
    SetupDiClassProperty.c
    SetupDiInstallClassExA.c
    SetupDiOpenDeviceInterface.c
    SetupDiGetActualModelsSection.c
    SetupInstallFile.c
    SetupInstallRegisterDlls.c
    SetupInstallServicesFromInfSectionEx.c
    SysButtonDevices.c
    UsbHubInformation.c
    UsbInterfaceIds.c
    testlist.c)

add_executable(setupapi_apitest ${SOURCE})
set_module_type(setupapi_apitest win32cui)
add_importlibs(setupapi_apitest advapi32 cfgmgr32 setupapi user32 ole32 wintrust crypt32 msvcrt kernel32)
add_rostests_file(TARGET setupapi_apitest)
