# TODO: implement the undocumented WLDP exports below; no SDK header or documentation defines their contract (INFO_NEEDED)
15 stub WldpAddDeveloperCertificateForDynamicCodeTrust
16 stdcall WldpCanExecuteBuffer(ptr long ptr long wstr ptr)
17 stdcall WldpCanExecuteFile(ptr long ptr wstr ptr)
18 stdcall WldpCanExecuteStream(ptr long ptr wstr ptr)
19 stub WldpChangeVulnerableDriverBlocklistState
20 stub WldpCheckDeviceEncryptionNotStarted
21 stub WldpCheckRetailConfiguration
22 stub WldpCheckWcosDeviceEncryptionSecure
23 stub WldpDisableDeveloperMode
24 stub WldpEnableDeveloperMode
25 stdcall WldpGetLockdownPolicy(ptr ptr long)
26 stub WldpIsAllowedEntryPoint
1 stdcall WldpIsAppApprovedByPolicy(wstr int64)
27 stdcall WldpIsClassInApprovedList(ptr ptr ptr long)
28 stub WldpIsDebugAllowed
29 stdcall WldpIsDynamicCodePolicyEnabled(ptr)
2 stdcall WldpIsProductionConfiguration(ptr)
30 stub WldpIsProductionConfigurationInProc
31 stub WldpIsVulnerableDriverBlocklistDisabled
32 stub WldpIsVulnerableDriverBlocklistEligibleToDisable
3 stdcall WldpIsWcosProductionConfiguration(ptr)
33 stdcall WldpQueryDeviceSecurityInformation(ptr long ptr)
34 stdcall WldpQueryDynamicCodeTrust(ptr ptr long)
5 stdcall WldpQueryPolicySettingEnabled(long ptr)
4 stdcall WldpQueryPolicySettingEnabled2(wstr ptr)
6 stub WldpQueryPolicySettingEnabledInternal
35 stdcall WldpQuerySecurityPolicy(ptr ptr ptr ptr ptr ptr)
7 stdcall WldpQueryWindowsLockdownMode(ptr)
8 stdcall WldpQueryWindowsLockdownRestriction(ptr)
9 stdcall WldpResetProductionConfiguration()
10 stdcall WldpResetWcosProductionConfiguration()
11 stub WldpSendSmartAppControlBlockToast
12 stub WldpSendSmartAppControlSwitchEnforceToast
36 stdcall WldpSetDynamicCodeTrust(ptr)
37 stub WldpSetDynamicCodeTrust2
13 stdcall WldpSetWindowsLockdownRestriction(long)
14 stub WldpTraceLoggingWDACBlockDialogShown
