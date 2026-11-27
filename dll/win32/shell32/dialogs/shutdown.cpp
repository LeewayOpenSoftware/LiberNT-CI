/*
 * PROJECT:     Orosi shell32.dll
 * LICENSE:     GPL-2.0-or-later (see COPYING in the top level directory)
 * PURPOSE:     Shut Down Windows dialog: shutdown-option gating and saved state.
 * COPYRIGHT:   Origin: ReactOS msgina/shutdown.c - Lee Schroeder (spaceseel at gmail dot com),
 *                      Hermes Belusca-Maito (hermes.belusca@sfr.fr),
 *                      Arnav Bhatt (arnavbhatt288 at gmail dot com),
 *                      Ahmed ARIF <arif.ing@outlook.com>.
 *              Copyright 2026 Leeway Open Software
 *
 * On Vista+ the desktop "Shut Down Windows" dialog (Alt+F4) lives in shell32,
 * not in the logon UI. The option-gating and saved-selection logic here is the
 * portable half of the old msgina implementation; the power-capability probe
 * and the privilege test use shell32's own helpers (IsPwr*Allowed /
 * SHTestTokenPrivilegeW) instead of the native power/token calls.
 */

#include "precomp.h"

#include <winwlx.h>  /* WLX_SHUTDOWN_STATE_* */
#include <powrprof.h>

WINE_DEFAULT_DEBUG_CHANNEL(shell);

EXTERN_C BOOL WINAPI SHTestTokenPrivilegeW(_In_opt_ HANDLE hToken, _In_ LPCWSTR lpName);

static LONG
ReadPolicyDword(
    _In_ HKEY hRoot,
    _In_ PCWSTR pPolicyKey,
    _In_ PCWSTR ValueName,
    _Out_ PDWORD pValue)
{
    HKEY hKey;
    DWORD dwValue, dwType, dwSize = sizeof(dwValue);
    LONG rc;

    rc = RegOpenKeyExW(hRoot, pPolicyKey, 0, KEY_QUERY_VALUE, &hKey);
    if (rc != ERROR_SUCCESS)
        return rc;

    rc = RegQueryValueExW(hKey, ValueName, NULL, &dwType, (LPBYTE)&dwValue, &dwSize);
    RegCloseKey(hKey);

    if (rc == ERROR_SUCCESS && dwType == REG_DWORD)
    {
        *pValue = dwValue;
        return ERROR_SUCCESS;
    }
    return ERROR_NOT_FOUND;
}

/* HKEY_LOCAL_MACHINE policies trump HKEY_CURRENT_USER ones (as in Windows). */
LONG
ShellGetPolicyDWORD(
    _In_ PCWSTR pPolicyKey,
    _In_ PCWSTR ValueName,
    _Out_ PDWORD pValue)
{
    *pValue = 0;

    if (ReadPolicyDword(HKEY_LOCAL_MACHINE, pPolicyKey, ValueName, pValue) == ERROR_SUCCESS)
        return ERROR_SUCCESS;

    return ReadPolicyDword(HKEY_CURRENT_USER, pPolicyKey, ValueName, pValue);
}

DWORD
ShellGetAllowedShutdownOptions(VOID)
{
    DWORD Options = 0;
    DWORD dwPolicyValue = 0;

    /* Log off, unless the Explorer policy forbids it. */
    ShellGetPolicyDWORD(L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
                        L"NoLogoff", &dwPolicyValue);
    if (dwPolicyValue == 0)
        Options |= WLX_SHUTDOWN_STATE_LOGOFF;

    /* Shutdown/restart (and the power options) need the shutdown privilege. */
    if (!SHTestTokenPrivilegeW(NULL, SE_SHUTDOWN_NAME))
        return Options;

    Options |= WLX_SHUTDOWN_STATE_POWER_OFF | WLX_SHUTDOWN_STATE_REBOOT;

    if (IsPwrSuspendAllowed())
        Options |= WLX_SHUTDOWN_STATE_SLEEP;
    if (IsPwrHibernateAllowed())
        Options |= WLX_SHUTDOWN_STATE_HIBERNATE;

    return Options;
}

DWORD
ShellLoadShutdownSelState(VOID)
{
    HKEY hKey;
    DWORD dwValue = WLX_SHUTDOWN_STATE_POWER_OFF; /* default */
    DWORD dwTemp, dwSize = sizeof(dwTemp);

    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer",
                      0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(hKey, L"Shutdown Setting", NULL, NULL,
                             (LPBYTE)&dwTemp, &dwSize) == ERROR_SUCCESS)
            dwValue = dwTemp;
        RegCloseKey(hKey);
    }
    return dwValue;
}

VOID
ShellSaveShutdownSelState(_In_ DWORD State)
{
    HKEY hKey;

    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer",
                        0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                        NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;

    RegSetValueExW(hKey, L"Shutdown Setting", 0, REG_DWORD,
                   (const BYTE *)&State, sizeof(State));
    RegCloseKey(hKey);
}
