/*
 * PROJECT:     Orosi PowerShell WinRM plugin stub
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Clean-room pwrshplugin.dll
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * This is an original implementation for the Orosi operating system.
 * It implements the ten documented WSMan plugin entry points so that
 * the PowerShell MSI (PWRSHPLUGIN_VERSION AppSearch, MinVersion
 * 6.3.9600.16383) accepts the system; the plugin always reports itself
 * as unavailable because Orosi has no WinRM remoting endpoint yet.
 * No Microsoft code or binary material is used.
 *
 * The signatures match the documented wsman.h WSMAN_PLUGIN_* typedefs;
 * the WSMAN_* struct pointers are represented as PVOID because wsman.h
 * is not part of our SDK. Only the argument counts and order matter
 * for the stdcall ABI.
 *
 * Two exports known on Windows are deliberately NOT provided because
 * they are undocumented and we do not invent their signatures:
 *   GetCLRVersionForPSVersion, PerformWSManPluginReportCompletion.
 * We add them only if a run shows a caller.
 */

#include <windows.h>

extern ULONG __cdecl DbgPrint(PCSTR Format, ...);

/* Resolved dynamically from wsmsvc.dll (loaded by the WinRM service
 * only); never imported statically. */
typedef VOID (WINAPI *WSMAN_PLUGIN_OPERATION_COMPLETE_FN)(
    PVOID requestDetails, DWORD flags, DWORD errorCode, PVOID data);

static VOID
ReportOperationNotSupported(_In_opt_ PVOID requestDetails)
{
    HMODULE hModule;
    WSMAN_PLUGIN_OPERATION_COMPLETE_FN operationComplete;

    if (requestDetails == NULL)
        return;

    hModule = GetModuleHandleW(L"wsmsvc.dll");
    if (hModule == NULL)
        return;

    operationComplete = (WSMAN_PLUGIN_OPERATION_COMPLETE_FN)(VOID*)
        GetProcAddress(hModule, "WSManPluginOperationComplete");
    if (operationComplete)
        operationComplete(requestDetails, 0, ERROR_NOT_SUPPORTED, NULL);
}

DWORD WINAPI
WSManPluginStartup(
    _In_ DWORD flags,
    _In_ PCWSTR applicationIdentification,
    _In_opt_ PCWSTR extraInfo,
    _Out_opt_ PVOID *pluginContext)
{
    (void)flags;
    (void)applicationIdentification;
    (void)extraInfo;

    if (pluginContext)
        *pluginContext = NULL;

    DbgPrint("pwrshplugin: WinRM remoting not available\n");
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI
WSManPluginShutdown(
    _In_ PVOID pluginContext,
    _In_ DWORD flags,
    _In_ DWORD reason)
{
    (void)pluginContext;
    (void)flags;
    (void)reason;
    return NO_ERROR;
}

VOID WINAPI
WSManPluginShell(
    _In_ PVOID pluginContext,
    _In_ PVOID requestDetails,
    _In_ DWORD flags,
    _In_ PVOID startupInfo,
    _In_opt_ PVOID inboundShellInformation)
{
    (void)pluginContext;
    (void)flags;
    (void)startupInfo;
    (void)inboundShellInformation;
    ReportOperationNotSupported(requestDetails);
}

VOID WINAPI
WSManPluginCommand(
    _In_ PVOID requestDetails,
    _In_ DWORD flags,
    _In_ PVOID shellContext,
    _In_ PCWSTR commandLine,
    _In_opt_ PVOID arguments)
{
    (void)flags;
    (void)shellContext;
    (void)commandLine;
    (void)arguments;
    ReportOperationNotSupported(requestDetails);
}

VOID WINAPI
WSManPluginSend(
    _In_ PVOID requestDetails,
    _In_ DWORD flags,
    _In_ PVOID shellContext,
    _In_ PVOID commandContext,
    _In_ PCWSTR stream,
    _In_opt_ PVOID inboundData)
{
    (void)flags;
    (void)shellContext;
    (void)commandContext;
    (void)stream;
    (void)inboundData;
    ReportOperationNotSupported(requestDetails);
}

VOID WINAPI
WSManPluginReceive(
    _In_ PVOID requestDetails,
    _In_ DWORD flags,
    _In_ PVOID shellContext,
    _In_ PVOID commandContext,
    _In_opt_ PVOID streamSet)
{
    (void)flags;
    (void)shellContext;
    (void)commandContext;
    (void)streamSet;
    ReportOperationNotSupported(requestDetails);
}

VOID WINAPI
WSManPluginSignal(
    _In_ PVOID requestDetails,
    _In_ DWORD flags,
    _In_ PVOID shellContext,
    _In_ PVOID commandContext,
    _In_ PCWSTR code)
{
    (void)flags;
    (void)shellContext;
    (void)commandContext;
    (void)code;
    ReportOperationNotSupported(requestDetails);
}

VOID WINAPI
WSManPluginConnect(
    _In_ PVOID requestDetails,
    _In_ DWORD flags,
    _In_ PVOID shellContext,
    _In_ PVOID commandContext,
    _In_opt_ PVOID inboundConnectInformation)
{
    (void)flags;
    (void)shellContext;
    (void)commandContext;
    (void)inboundConnectInformation;
    ReportOperationNotSupported(requestDetails);
}

VOID WINAPI
WSManPluginReleaseShellContext(
    _In_ PVOID shellContext)
{
    (void)shellContext;
}

VOID WINAPI
WSManPluginReleaseCommandContext(
    _In_ PVOID shellContext,
    _In_ PVOID commandContext)
{
    (void)shellContext;
    (void)commandContext;
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

/* The module links without the C runtime (kernel32 + ntdll only), so
 * provide the entry point the linker expects. */
BOOL WINAPI
DllMainCRTStartup(
    _In_ HINSTANCE hinstDLL,
    _In_ DWORD fdwReason,
    _In_opt_ LPVOID lpvReserved)
{
    return DllMain(hinstDLL, fdwReason, lpvReserved);
}
