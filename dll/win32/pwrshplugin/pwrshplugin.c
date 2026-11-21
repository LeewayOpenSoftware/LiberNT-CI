/*
 * PROJECT:     Orosi PowerShell WSMan remoting plugin stub
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Clean-room pwrshplugin.dll
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * This is an original implementation for the Orosi operating system.
 * It implements the publicly documented WSMan plugin entry points that
 * the PowerShell MSI (PWRSHPLUGIN_VERSION AppSearch) probes for; no
 * Microsoft source or binary material is used. Every entry point fails
 * cleanly with ERROR_NOT_SUPPORTED because Orosi does not ship a WSMan
 * remoting endpoint yet; the version resource satisfies the MSI file
 * signature (MinVersion 6.3.9600.16383).
 */

#include <windows.h>

/* Publicly documented WSMan plugin entry points, simplified signatures.
 * The stub behaviour (clean unsupported failure) is intentional. */

BOOL WINAPI
GetCLRVersionForPSVersion(
    _In_ PCWSTR pszPSVersion,
    _Out_writes_(cchCLRVersion) PWSTR pszCLRVersion,
    _In_ DWORD cchCLRVersion)
{
    (void)pszPSVersion;
    if (pszCLRVersion && cchCLRVersion)
        pszCLRVersion[0] = L'\0';
    return FALSE;
}

VOID WINAPI
PerformWSManPluginReportCompletion(
    _In_ PVOID pRequestDetails,
    _In_ DWORD dwError,
    _In_ DWORD dwShellResult,
    _In_opt_ PCWSTR pszCommandResult)
{
    (void)pRequestDetails;
    (void)dwError;
    (void)dwShellResult;
    (void)pszCommandResult;
}

DWORD WINAPI
WSManPluginCommand(
    _In_ PVOID pRequest,
    _In_ DWORD dwFlags,
    _In_ PVOID pCommandLineInput)
{
    (void)pRequest;
    (void)dwFlags;
    (void)pCommandLineInput;
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI
WSManPluginConnect(
    _In_ PVOID pRequest,
    _In_ DWORD dwFlags)
{
    (void)pRequest;
    (void)dwFlags;
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI
WSManPluginReceive(
    _In_ PVOID pRequest,
    _In_ DWORD dwFlags)
{
    (void)pRequest;
    (void)dwFlags;
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI
WSManPluginReleaseCommandContext(
    _In_ PVOID pCommandContext)
{
    (void)pCommandContext;
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI
WSManPluginReleaseShellContext(
    _In_ PVOID pShellContext)
{
    (void)pShellContext;
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI
WSManPluginSend(
    _In_ PVOID pRequest,
    _In_ DWORD dwFlags)
{
    (void)pRequest;
    (void)dwFlags;
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI
WSManPluginShell(
    _In_ PVOID pRequest,
    _In_ DWORD dwFlags,
    _In_ PVOID pStartupInput)
{
    (void)pRequest;
    (void)dwFlags;
    (void)pStartupInput;
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI
WSManPluginShutdown(VOID)
{
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI
WSManPluginSignal(
    _In_ PVOID pRequest,
    _In_ DWORD dwFlags)
{
    (void)pRequest;
    (void)dwFlags;
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI
WSManPluginStartup(
    _In_ DWORD dwFlags,
    _In_ PCWSTR pszApplicationId,
    _In_opt_ PVOID pCapabilities)
{
    (void)dwFlags;
    (void)pszApplicationId;
    (void)pCapabilities;
    return ERROR_NOT_SUPPORTED;
}

BOOL WINAPI
DllMain(
    _In_ HINSTANCE hinstDLL,
    _In_ DWORD fdwReason,
    _In_opt_ LPVOID lpvReserved)
{
    (void)hinstDLL;
    (void)fdwReason;
    (void)lpvReserved;
    return TRUE;
}
