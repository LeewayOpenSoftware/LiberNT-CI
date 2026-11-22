/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS Display Control Panel
 * FILE:            dll/cpl/desk/general.c
 * PURPOSE:         Advanced General settings
 */

#include "desk.h"
#include <shellapi.h>

#if (WINVER < 0x0605)
UINT WINAPI GetDpiForSystem(VOID);
DPI_AWARENESS_CONTEXT WINAPI SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT);
#endif

/* The scale is set in one place, Settings > System > Display; this page only shows it */
static UINT
GetSessionScale(VOID)
{
    DPI_AWARENESS_CONTEXT Previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    UINT Dpi = GetDpiForSystem();

    if (Previous)
        SetThreadDpiAwarenessContext(Previous);
    return MulDiv(Dpi, 100, 96);
}

static VOID
InitScale(HWND hWnd)
{
    TCHAR Format[64], Text[80];

    if (!LoadString(hApplet, IDS_SCALE_CURRENT, Format, _countof(Format)))
        StringCchCopy(Format, _countof(Format), _T("%u%%"));
    StringCchPrintf(Text, _countof(Text), Format, GetSessionScale());
    SetDlgItemText(hWnd, IDC_SCALE_CURRENT, Text);
}

static VOID
InitRadioButtons(HWND hWnd)
{
    HKEY hKey;

    if (RegOpenKeyEx(HKEY_CURRENT_USER,
                     _T("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\Display"),
                     0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        TCHAR szBuf[64];
        DWORD dwSize = 64;

        if (RegQueryValueEx(hKey, _T("DynaSettingsChange"), 0, NULL,
                            (LPBYTE)szBuf, &dwSize) == ERROR_SUCCESS)
        {
            switch (_ttoi(szBuf))
            {
                case 0:
                    SendDlgItemMessage(hWnd, IDC_RESTART_RB, BM_SETCHECK, 1, 1);
                    break;
                case 1:
                    SendDlgItemMessage(hWnd, IDC_WITHOUTREBOOT_RB, BM_SETCHECK, 1, 1);
                    break;
                case 3:
                    SendDlgItemMessage(hWnd, IDC_ASKME_RB, BM_SETCHECK, 1, 1);
                    break;
            }
        }
        else
            SendDlgItemMessage(hWnd, IDC_WITHOUTREBOOT_RB, BM_SETCHECK, 1, 1);

        RegCloseKey(hKey);
    }
    else
        SendDlgItemMessage(hWnd, IDC_WITHOUTREBOOT_RB, BM_SETCHECK, 1, 1);
}

INT_PTR CALLBACK
AdvGeneralPageProc(HWND hwndDlg,
                   UINT uMsg,
                   WPARAM wParam,
                   LPARAM lParam)
{
    PDISPLAY_DEVICE_ENTRY DispDevice = NULL;
    INT_PTR Ret = 0;

    if (uMsg != WM_INITDIALOG)
        DispDevice = (PDISPLAY_DEVICE_ENTRY)GetWindowLongPtr(hwndDlg, DWLP_USER);

    switch (uMsg)
    {
        case WM_INITDIALOG:
            DispDevice = (PDISPLAY_DEVICE_ENTRY)(((LPPROPSHEETPAGE)lParam)->lParam);
            SetWindowLongPtr(hwndDlg, DWLP_USER, (LONG_PTR)DispDevice);

            InitScale(hwndDlg);
            InitRadioButtons(hwndDlg);

            Ret = TRUE;
            break;
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDC_SCALE_SETTINGS:
                    if (HIWORD(wParam) == BN_CLICKED &&
                        (INT_PTR)ShellExecuteW(hwndDlg, NULL, L"ms-settings:display", NULL, NULL, SW_SHOWNORMAL) <= 32)
                    {
                        MessageBeep(MB_ICONWARNING);
                    }
                    break;
                case IDC_RESTART_RB:
                case IDC_WITHOUTREBOOT_RB:
                case IDC_ASKME_RB:
                    PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                break;
            }
            break;

        case WM_NOTIFY:
            if (((LPNMHDR)lParam)->code == PSN_APPLY)
            {
                PropSheet_UnChanged(GetParent(hwndDlg), hwndDlg);
                SetWindowLongPtr(hwndDlg, DWLP_MSGRESULT, PSNRET_NOERROR);
                return TRUE;
            }
            break;
    }

    return Ret;
}
