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

#include "shutdown.h"  /* WLX_SHUTDOWN_STATE_* */
#include <powrprof.h>

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

    /* NoClose removes shut down / restart / sleep / hibernate (log off may remain). */
    dwPolicyValue = 0;
    ShellGetPolicyDWORD(L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
                        L"NoClose", &dwPolicyValue);
    if (dwPolicyValue != 0)
        return Options;

    Options |= WLX_SHUTDOWN_STATE_POWER_OFF | WLX_SHUTDOWN_STATE_REBOOT;

    if (IsPwrSuspendAllowed())
        Options |= WLX_SHUTDOWN_STATE_SLEEP;
    if (IsPwrHibernateAllowed())
        Options |= WLX_SHUTDOWN_STATE_HIBERNATE;

    return Options;
}

/* --- The classic "Shut Down Windows" dialog (on Vista+ it lives in shell32). --- */

struct SHUTDOWN_DLG_CTX
{
    DWORD Options;
    DWORD Selected;
};

static UINT
ShutdownChoiceDescription(DWORD Code)
{
    switch (Code)
    {
        case WLX_SHUTDOWN_STATE_LOGOFF:    return IDS_SHUTDOWN_LOGOFF_DESC;
        case WLX_SHUTDOWN_STATE_POWER_OFF: return IDS_SHUTDOWN_POWEROFF_DESC;
        case WLX_SHUTDOWN_STATE_REBOOT:    return IDS_SHUTDOWN_REBOOT_DESC;
        case WLX_SHUTDOWN_STATE_SLEEP:     return IDS_SHUTDOWN_SLEEP_DESC;
        case WLX_SHUTDOWN_STATE_HIBERNATE: return IDS_SHUTDOWN_HIBERNATE_DESC;
    }
    return 0;
}

static VOID
ShutdownShowDescription(HWND hDlg)
{
    HWND hCombo = GetDlgItem(hDlg, IDC_SHUTDOWN_ACTION);
    WCHAR szText[256] = L"";
    int iSel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    UINT Id = (iSel == CB_ERR) ? 0 :
        ShutdownChoiceDescription((DWORD)SendMessageW(hCombo, CB_GETITEMDATA, iSel, 0));

    if (Id != 0)
        LoadStringW(shell32_hInstance, Id, szText, _countof(szText));
    SetDlgItemTextW(hDlg, IDC_SHUTDOWN_DESC, szText);
}

static VOID
ShutdownAddChoice(HWND hCombo, UINT StringId, DWORD Code, const SHUTDOWN_DLG_CTX *Ctx)
{
    WCHAR szText[128];
    int iIndex;

    if (!(Ctx->Options & Code))
        return;
    if (!LoadStringW(shell32_hInstance, StringId, szText, _countof(szText)))
        return;

    iIndex = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)szText);
    if (iIndex < 0)
        return;
    SendMessageW(hCombo, CB_SETITEMDATA, iIndex, (LPARAM)Code);
    if (Code == Ctx->Selected)
        SendMessageW(hCombo, CB_SETCURSEL, iIndex, 0);
}

static INT_PTR CALLBACK
ShutdownDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    SHUTDOWN_DLG_CTX *Ctx;
    HWND hCombo;

    switch (uMsg)
    {
        case WM_INITDIALOG:
            Ctx = (SHUTDOWN_DLG_CTX *)lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Ctx);
            hCombo = GetDlgItem(hDlg, IDC_SHUTDOWN_ACTION);
            /* Windows order: Sign out, Sleep, Hibernate, Shut down, Restart (Switch user is not offered). */
            ShutdownAddChoice(hCombo, IDS_SHUTDOWN_LOGOFF, WLX_SHUTDOWN_STATE_LOGOFF, Ctx);
            ShutdownAddChoice(hCombo, IDS_SHUTDOWN_SLEEP, WLX_SHUTDOWN_STATE_SLEEP, Ctx);
            ShutdownAddChoice(hCombo, IDS_SHUTDOWN_HIBERNATE, WLX_SHUTDOWN_STATE_HIBERNATE, Ctx);
            ShutdownAddChoice(hCombo, IDS_SHUTDOWN_POWEROFF, WLX_SHUTDOWN_STATE_POWER_OFF, Ctx);
            ShutdownAddChoice(hCombo, IDS_SHUTDOWN_REBOOT, WLX_SHUTDOWN_STATE_REBOOT, Ctx);
            if (SendMessageW(hCombo, CB_GETCURSEL, 0, 0) == CB_ERR)
                SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
            ShutdownShowDescription(hDlg);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDOK:
                {
                    int iSel;
                    Ctx = (SHUTDOWN_DLG_CTX *)GetWindowLongPtrW(hDlg, DWLP_USER);
                    hCombo = GetDlgItem(hDlg, IDC_SHUTDOWN_ACTION);
                    iSel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
                    Ctx->Selected = (iSel == CB_ERR) ? 0 :
                        (DWORD)SendMessageW(hCombo, CB_GETITEMDATA, iSel, 0);
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
                case IDC_SHUTDOWN_ACTION:
                    if (HIWORD(wParam) == CBN_SELCHANGE)
                        ShutdownShowDescription(hDlg);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

DWORD
ShellShutdownWindowsDialog(HWND hWndOwner, DWORD dwExcludeOptions)
{
    SHUTDOWN_DLG_CTX Ctx;

    /* Windows 11 opens on Shut down every time and keeps no choice (win11-shutdown-dialog.md). */
    Ctx.Options = ShellGetAllowedShutdownOptions() & ~dwExcludeOptions;
    Ctx.Selected = WLX_SHUTDOWN_STATE_POWER_OFF;
    if (Ctx.Options == 0)
        return 0;

    if (DialogBoxParamW(shell32_hInstance, MAKEINTRESOURCEW(IDD_SHUTDOWN),
                        hWndOwner, ShutdownDlgProc, (LPARAM)&Ctx) != IDOK)
        return 0;

    return Ctx.Selected;
}
