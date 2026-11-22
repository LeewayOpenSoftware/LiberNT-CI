#include "wined3dcfg.h"

#include <winreg.h>

/* wined3d only honours MaxShaderModelGS/PS/VS and VideoMemorySize; the former
 * dead controls wrote keys the engine never reads (see wined3d_main.c). */
WINED3D_SETTINGS gwd3dsShaderLvl[] =
{
    {VALUE_DEFAULT, REG_NONE, 0},
    {L"1.4", REG_DWORD, 1},
    {L"2", REG_DWORD, 2},
    {L"3", REG_DWORD, 3},
};

WINED3D_SETTINGS gwd3dsVidMem[] =
{
    {VALUE_DEFAULT, REG_NONE, 0},
    {L"8 MB", REG_SZ, 8},
    {L"16 MB", REG_SZ, 16},
    {L"32 MB", REG_SZ, 32},
    {L"64 MB", REG_SZ, 64},
    {L"128 MB", REG_SZ, 128},
    {L"256 MB", REG_SZ, 256},
    {L"512 MB", REG_SZ, 512},
};


void InitControl(HWND hWndDlg, HKEY hKey, PWCHAR szKey, PWINED3D_SETTINGS pSettings, INT iControlId, INT iCount)
{
    WCHAR szBuffer[MAX_KEY_LENGTH];
    DWORD dwSize = sizeof(szBuffer);
    DWORD dwType = 0;
    INT iCurrent;
    INT iActive = 0;

    RegQueryValueExW(hKey, szKey, NULL, &dwType, (LPBYTE)szBuffer, &dwSize);

    for(iCurrent = 0; iCurrent < iCount; iCurrent++)
    {
        SendDlgItemMessageW(hWndDlg, iControlId, CB_ADDSTRING, 0, (LPARAM)pSettings[iCurrent].szValue);

        if(dwSize && ((dwType == REG_DWORD && *szBuffer == pSettings[iCurrent].iValue) ||
           (dwType == REG_SZ && !wcscmp(szBuffer, pSettings[iCurrent].szValue))))
        {
            iActive = iCurrent;
        }
    }

    SendDlgItemMessageW(hWndDlg, iControlId, CB_SETCURSEL, iActive, 0);

}

static VOID InitSettings(HWND hWndDlg)
{
    HKEY hKey;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, KEY_WINE, 0, NULL, 0, MAXIMUM_ALLOWED, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;

    INIT_CONTROL(GSLEVEL, gwd3dsShaderLvl);
    INIT_CONTROL(PSLEVEL, gwd3dsShaderLvl);
    INIT_CONTROL(VSLEVEL, gwd3dsShaderLvl);
    INIT_CONTROL(VIDMEMSIZE, gwd3dsVidMem);

    RegCloseKey(hKey);
}


static VOID SaveSetting(HWND hWnd, HKEY hKey, PWCHAR szKey, PWINED3D_SETTINGS pCfg, INT iControlId, INT iCount)
{
    INT iSel = 0;

    iSel = (INT)SendDlgItemMessageW(hWnd, iControlId, CB_GETCURSEL, 0, 0);

    if(iSel < 0 || iSel > iCount)
        return;

    if(pCfg[iSel].iType == REG_NONE)
    {
        RegDeleteValueW(hKey, szKey);
        return;
    }

    if(pCfg[iSel].iType == REG_DWORD)
    {
        RegSetValueExW(hKey, szKey, 0, REG_DWORD, (LPBYTE)&pCfg[iSel].iValue, sizeof(pCfg[iSel].iValue));
        return;
    } else if (pCfg[iSel].iType == REG_SZ)
    {
        RegSetValueExW(hKey, szKey, 0, pCfg[iSel].iType, (LPBYTE)pCfg[iSel].szValue, (wcslen(pCfg[iSel].szValue) + 1) * sizeof(WCHAR));
    }
}


static VOID WriteSettings(HWND hWndDlg)
{
    HKEY hKey;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, KEY_WINE, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return;

    SAVE_CONTROL(GSLEVEL, gwd3dsShaderLvl);
    SAVE_CONTROL(PSLEVEL, gwd3dsShaderLvl);
    SAVE_CONTROL(VSLEVEL, gwd3dsShaderLvl);
    SAVE_CONTROL(VIDMEMSIZE, gwd3dsVidMem);

    RegCloseKey(hKey);
}


INT_PTR CALLBACK GeneralPageProc(HWND hWndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    LPPSHNOTIFY lppsn;

    switch (uMsg)
    {
        case WM_INITDIALOG:
            InitSettings(hWndDlg);
            return TRUE;

        case WM_COMMAND:
            if (LOWORD(wParam) > IDC_MIN && LOWORD(wParam) < IDC_MAX && HIWORD(wParam) == CBN_SELCHANGE)
                PropSheet_Changed(GetParent(hWndDlg), hWndDlg);
            break;

        case WM_NOTIFY:
            lppsn = (LPPSHNOTIFY)lParam;
            if (lppsn->hdr.code == PSN_APPLY)
            {
                WriteSettings(hWndDlg);
                return TRUE;
            }
            break;
    }

    return FALSE;
}
