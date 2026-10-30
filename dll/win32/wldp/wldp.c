/*
 * PROJECT:     LiberNT Windows Lockdown Policy
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Lockdown, application control and dynamic code policy queries
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <stdarg.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <wldp.h>

/* LiberNT enforces no code integrity or application control policy. */
#define WLDP_NO_POLICY_STATE WLDP_LOCKDOWN_DEFINED_FLAG

static
BOOL
WldpIsValidHost(
    _In_ const WLDP_HOST_INFORMATION *HostInformation)
{
    return HostInformation->dwRevision == WLDP_HOST_INFORMATION_REVISION &&
           HostInformation->dwHostId < WLDP_HOST_ID_MAX;
}

HRESULT
WINAPI
WldpGetLockdownPolicy(
    _In_opt_ WLDP_HOST_INFORMATION *HostInformation,
    _Out_ DWORD *LockdownState,
    _In_ DWORD LockdownFlags)
{
    UNREFERENCED_PARAMETER(LockdownFlags);

    if ((HostInformation && !WldpIsValidHost(HostInformation)) || !LockdownState)
        return E_INVALIDARG;
    *LockdownState = WLDP_NO_POLICY_STATE;
    return S_OK;
}

HRESULT
WINAPI
WldpIsClassInApprovedList(
    _In_ REFCLSID ClassId,
    _In_ WLDP_HOST_INFORMATION *HostInformation,
    _Out_ BOOL *IsApproved,
    _In_ DWORD OptionalFlags)
{
    UNREFERENCED_PARAMETER(ClassId);
    UNREFERENCED_PARAMETER(OptionalFlags);

    if (!WldpIsValidHost(HostInformation) || HostInformation->dwHostId == WLDP_HOST_ID_GLOBAL)
        return E_INVALIDARG;
    *IsApproved = TRUE;
    return S_OK;
}

HRESULT
WINAPI
WldpIsDynamicCodePolicyEnabled(
    _Out_ BOOL *IsEnabled)
{
    *IsEnabled = FALSE;
    return S_OK;
}

HRESULT
WINAPI
WldpQueryWindowsLockdownMode(
    _Out_ WLDP_WINDOWS_LOCKDOWN_MODE *LockdownMode)
{
    *LockdownMode = WLDP_WINDOWS_LOCKDOWN_MODE_UNLOCKED;
    return S_OK;
}

HRESULT
WINAPI
WldpQueryWindowsLockdownRestriction(
    _Out_ WLDP_WINDOWS_LOCKDOWN_RESTRICTION *LockdownRestriction)
{
    if (!LockdownRestriction)
        return E_INVALIDARG;
    *LockdownRestriction = WLDP_WINDOWS_LOCKDOWN_RESTRICTION_NONE;
    return S_OK;
}

HRESULT
WINAPI
WldpSetWindowsLockdownRestriction(
    _In_ WLDP_WINDOWS_LOCKDOWN_RESTRICTION LockdownRestriction)
{
    if ((ULONG)LockdownRestriction >= WLDP_WINDOWS_LOCKDOWN_RESTRICTION_MAX)
        return E_INVALIDARG;
    /* FIXME: only the NONE restriction is measured on Windows 11; NOUNLOCK values are accepted without effect */
    return S_OK;
}

HRESULT
WINAPI
WldpQueryPolicySettingEnabled(
    _In_ WLDP_POLICY_SETTING Setting,
    _Out_ BOOL *Enabled)
{
    if (!Enabled)
        return HRESULT_FROM_NT(STATUS_INVALID_PARAMETER);
    *Enabled = FALSE;
    return Setting == WLDP_POLICY_SETTING_AV_PERF_MODE ? S_OK : HRESULT_FROM_NT(STATUS_INVALID_PARAMETER);
}

HRESULT
WINAPI
WldpQueryPolicySettingEnabled2(
    _In_ const WCHAR *SettingString,
    _Out_ BOOL *Enabled)
{
    if (!SettingString)
        return HRESULT_FROM_NT(STATUS_INVALID_PARAMETER);
    *Enabled = FALSE;
    return S_OK;
}

HRESULT
WINAPI
WldpIsProductionConfiguration(
    _Out_ BOOL *IsProductionConfiguration)
{
    *IsProductionConfiguration = TRUE;
    return S_OK;
}

HRESULT
WINAPI
WldpIsWcosProductionConfiguration(
    _Out_ BOOL *IsProductionConfiguration)
{
    *IsProductionConfiguration = FALSE;
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT
WINAPI
WldpResetProductionConfiguration(VOID)
{
    return S_OK;
}

HRESULT
WINAPI
WldpResetWcosProductionConfiguration(VOID)
{
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT
WINAPI
WldpCanExecuteFile(
    _In_ REFGUID Host,
    _In_ WLDP_EXECUTION_EVALUATION_OPTIONS Options,
    _In_ HANDLE FileHandle,
    _In_opt_ const WCHAR *AuditInfo,
    _Out_ WLDP_EXECUTION_POLICY *Result)
{
    UNREFERENCED_PARAMETER(Options);
    UNREFERENCED_PARAMETER(AuditInfo);

    if (IsEqualGUID(Host, &GUID_NULL) || !FileHandle || FileHandle == INVALID_HANDLE_VALUE)
        return E_INVALIDARG;
    *Result = WLDP_EXECUTION_POLICY_ALLOWED;
    return S_OK;
}

HRESULT
WINAPI
WldpCanExecuteBuffer(
    _In_ REFGUID Host,
    _In_ WLDP_EXECUTION_EVALUATION_OPTIONS Options,
    _In_reads_(BufferSize) const BYTE *Buffer,
    _In_ ULONG BufferSize,
    _In_opt_ const WCHAR *AuditInfo,
    _Out_ WLDP_EXECUTION_POLICY *Result)
{
    UNREFERENCED_PARAMETER(Host);
    UNREFERENCED_PARAMETER(Options);
    UNREFERENCED_PARAMETER(AuditInfo);

    if (!Buffer || !BufferSize)
        return E_INVALIDARG;
    *Result = WLDP_EXECUTION_POLICY_ALLOWED;
    return S_OK;
}

HRESULT
WINAPI
WldpCanExecuteStream(
    _In_ REFGUID Host,
    _In_ WLDP_EXECUTION_EVALUATION_OPTIONS Options,
    _In_ IStream *Stream,
    _In_opt_ const WCHAR *AuditInfo,
    _Out_ WLDP_EXECUTION_POLICY *Result)
{
    UNREFERENCED_PARAMETER(Host);
    UNREFERENCED_PARAMETER(Options);
    UNREFERENCED_PARAMETER(AuditInfo);

    if (!Stream)
        return E_INVALIDARG;
    *Result = WLDP_EXECUTION_POLICY_ALLOWED;
    return S_OK;
}

HRESULT
WINAPI
WldpQueryDynamicCodeTrust(
    _In_opt_ HANDLE FileHandle,
    _In_reads_bytes_opt_(ImageSize) void *BaseImage,
    _In_ ULONG ImageSize)
{
    UNREFERENCED_PARAMETER(ImageSize);

    if (!FileHandle && !BaseImage)
        return E_INVALIDARG;
    return S_OK;
}

HRESULT
WINAPI
WldpSetDynamicCodeTrust(
    _In_ HANDLE FileHandle)
{
    /* Without a code integrity policy no file can be granted dynamic code trust. */
    return HRESULT_FROM_NT(FileHandle ? STATUS_ACCESS_DENIED : STATUS_INVALID_HANDLE);
}

HRESULT
WINAPI
WldpQueryDeviceSecurityInformation(
    _Out_writes_bytes_to_opt_(InformationLength, *ReturnLength) WLDP_DEVICE_SECURITY_INFORMATION *Information,
    _In_ DWORD InformationLength,
    _Out_ DWORD *ReturnLength)
{
    UNREFERENCED_PARAMETER(Information);
    UNREFERENCED_PARAMETER(InformationLength);
    UNREFERENCED_PARAMETER(ReturnLength);

    return S_OK;
}

HRESULT
WINAPI
WldpIsAppApprovedByPolicy(
    _In_ const WCHAR *PackageFamilyName,
    _In_ ULONGLONG PackageVersion)
{
    UNREFERENCED_PARAMETER(PackageFamilyName);
    UNREFERENCED_PARAMETER(PackageVersion);

    return HRESULT_FROM_NT(STATUS_SUCCESS);
}

HRESULT
WINAPI
WldpQuerySecurityPolicy(
    _In_ const struct _UNICODE_STRING *ProviderName,
    _In_ const struct _UNICODE_STRING *KeyName,
    _In_ const struct _UNICODE_STRING *ValueName,
    _Out_ WLDP_SECURE_SETTING_VALUE_TYPE *ValueType,
    _Out_writes_bytes_opt_(*ValueSize) void *ValueAddress,
    _Inout_ ULONG *ValueSize)
{
    UNREFERENCED_PARAMETER(ProviderName);
    UNREFERENCED_PARAMETER(KeyName);
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(ValueType);
    UNREFERENCED_PARAMETER(ValueSize);

    if (!ValueAddress)
        return HRESULT_FROM_NT(STATUS_INVALID_PARAMETER);
    /* LiberNT keeps no secure policy settings, so every lookup misses. */
    return HRESULT_FROM_NT(STATUS_NOT_FOUND);
}
