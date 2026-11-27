/*
 * PROJECT:     ReactOS Services
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        base/applications/mscutils/servman/propsheet_logon.c
 * PURPOSE:     Logon property page
 * COPYRIGHT:   Eric Kohl
 */

#include "precomp.h"

#include <ntsecapi.h>

#define NDEBUG
#include <debug.h>

#define DEFAULT_PASSWORD L"               "

typedef struct _LOGONDATA
{
    ENUM_SERVICE_STATUS_PROCESS *pService;
    LPQUERY_SERVICE_CONFIG pServiceConfig;
    WCHAR szAccountName[64];
    WCHAR szPassword1[64];
    WCHAR szPassword2[64];
    INT nInteractive;
    BOOL bInitialized;
    BOOL bLocalSystem;
    BOOL bAccountChanged;
} LOGONDATA, *PLOGONDATA;


static
VOID
SetControlStates(
    HWND hwndDlg,
    PLOGONDATA pLogonData,
    BOOL bLocalSystem)
{
    EnableWindow(GetDlgItem(hwndDlg, IDC_LOGON_INTERACTIVE), bLocalSystem);
    EnableWindow(GetDlgItem(hwndDlg, IDC_LOGON_ACCOUNTNAME), !bLocalSystem);
    EnableWindow(GetDlgItem(hwndDlg, IDC_LOGON_SEARCH), FALSE /*!bLocalSystem*/);
    EnableWindow(GetDlgItem(hwndDlg, IDC_LOGON_PW1TEXT), !bLocalSystem);
    EnableWindow(GetDlgItem(hwndDlg, IDC_LOGON_PASSWORD1), !bLocalSystem);
    EnableWindow(GetDlgItem(hwndDlg, IDC_LOGON_PW2TEXT), !bLocalSystem);
    EnableWindow(GetDlgItem(hwndDlg, IDC_LOGON_PASSWORD2), !bLocalSystem);

    if (bLocalSystem)
    {
        SendDlgItemMessageW(hwndDlg, IDC_LOGON_INTERACTIVE, BM_SETCHECK, (WPARAM)pLogonData->nInteractive, 0);

        if (pLogonData->bInitialized == TRUE)
        {
            GetDlgItemText(hwndDlg, IDC_LOGON_ACCOUNTNAME, pLogonData->szAccountName, 64);
            GetDlgItemText(hwndDlg, IDC_LOGON_PASSWORD1, pLogonData->szPassword1, 64);
            GetDlgItemText(hwndDlg, IDC_LOGON_PASSWORD2, pLogonData->szPassword2, 64);
        }

        SetDlgItemText(hwndDlg, IDC_LOGON_ACCOUNTNAME, L"");
        SetDlgItemText(hwndDlg, IDC_LOGON_PASSWORD1, L"");
        SetDlgItemText(hwndDlg, IDC_LOGON_PASSWORD2, L"");
    }
    else
    {
        if (pLogonData->bInitialized == TRUE)
            pLogonData->nInteractive = SendDlgItemMessageW(hwndDlg, IDC_LOGON_INTERACTIVE, BM_GETCHECK, 0, 0);
        SendDlgItemMessageW(hwndDlg, IDC_LOGON_INTERACTIVE, BM_SETCHECK, (WPARAM)BST_UNCHECKED, 0);

        SetDlgItemText(hwndDlg, IDC_LOGON_ACCOUNTNAME, pLogonData->szAccountName);
        SetDlgItemText(hwndDlg, IDC_LOGON_PASSWORD1, pLogonData->szPassword1);
        SetDlgItemText(hwndDlg, IDC_LOGON_PASSWORD2, pLogonData->szPassword2);
    }

    pLogonData->bLocalSystem = bLocalSystem;
}


static
BOOL
SetServiceAccount(
    LPWSTR lpServiceName,
    DWORD dwServiceType,
    LPWSTR lpStartName,
    LPWSTR lpPassword)
{
    SC_HANDLE hSCManager;
    SC_HANDLE hSc;
    SC_LOCK scLock;
    BOOL bRet = FALSE;

    hSCManager = OpenSCManagerW(NULL,
                                NULL,
                                SC_MANAGER_LOCK);
    if (hSCManager)
    {
        scLock = LockServiceDatabase(hSCManager);
        if (scLock)
        {
            hSc = OpenServiceW(hSCManager,
                               lpServiceName,
                               SERVICE_CHANGE_CONFIG);
            if (hSc)
            {
                if (ChangeServiceConfigW(hSc,
                                         dwServiceType,
                                         SERVICE_NO_CHANGE,
                                         SERVICE_NO_CHANGE,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL,
                                         lpStartName,
                                         lpPassword,
                                         NULL))
                {
                    bRet = TRUE;
                }

                CloseServiceHandle(hSc);
            }

            UnlockServiceDatabase(scLock);
        }

        CloseServiceHandle(hSCManager);
    }

    if (!bRet)
        GetError();

    return bRet;
}


/* As the Windows Services snap-in: an account set here gets SeServiceLogonRight if it lacks it. */
static
VOID
GrantServiceLogonRight(
    HWND hwndDlg,
    LPCWSTR lpAccountName)
{
    static SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    LSA_OBJECT_ATTRIBUTES ObjectAttributes;
    LSA_HANDLE PolicyHandle;
    LSA_UNICODE_STRING Right, *Rights = NULL;
    BYTE SidBuffer[SECURITY_MAX_SID_SIZE];
    WCHAR szDomain[256], szFormat[256], szMessage[512], szTitle[256];
    DWORD cbSid = sizeof(SidBuffer), cchDomain = ARRAYSIZE(szDomain);
    ULONG i, Count = 0, Rid;
    SID_NAME_USE Use;
    BOOL bHeld = FALSE;

    if (!LookupAccountNameW(NULL, lpAccountName, SidBuffer, &cbSid, szDomain, &cchDomain, &Use))
        return;

    /* LocalSystem, LocalService and NetworkService need no right. */
    if (*GetSidSubAuthorityCount(SidBuffer) == 1 &&
        memcmp(GetSidIdentifierAuthority(SidBuffer), &NtAuthority, sizeof(NtAuthority)) == 0)
    {
        Rid = *GetSidSubAuthority(SidBuffer, 0);
        if (Rid == SECURITY_LOCAL_SYSTEM_RID || Rid == SECURITY_LOCAL_SERVICE_RID || Rid == SECURITY_NETWORK_SERVICE_RID)
            return;
    }

    ZeroMemory(&ObjectAttributes, sizeof(ObjectAttributes));
    if (LsaOpenPolicy(NULL, &ObjectAttributes, POLICY_LOOKUP_NAMES | POLICY_CREATE_ACCOUNT, &PolicyHandle) != 0)
        return;

    if (LsaEnumerateAccountRights(PolicyHandle, SidBuffer, &Rights, &Count) == 0)
    {
        for (i = 0; i < Count; i++)
        {
            if (Rights[i].Length == sizeof(SE_SERVICE_LOGON_NAME) - sizeof(WCHAR) &&
                _wcsnicmp(Rights[i].Buffer, SE_SERVICE_LOGON_NAME, Rights[i].Length / sizeof(WCHAR)) == 0)
                bHeld = TRUE;
        }
        LsaFreeMemory(Rights);
    }

    if (!bHeld)
    {
        Right.Buffer = (PWSTR)SE_SERVICE_LOGON_NAME;
        Right.Length = sizeof(SE_SERVICE_LOGON_NAME) - sizeof(WCHAR);
        Right.MaximumLength = sizeof(SE_SERVICE_LOGON_NAME);
        if (LsaAddAccountRights(PolicyHandle, SidBuffer, &Right, 1) == 0 &&
            LoadStringW(GetModuleHandle(NULL), IDS_SERVICE_LOGON_GRANTED, szFormat, ARRAYSIZE(szFormat)) &&
            LoadStringW(GetModuleHandle(NULL), IDS_APPNAME, szTitle, ARRAYSIZE(szTitle)))
        {
            StringCchPrintfW(szMessage, ARRAYSIZE(szMessage), szFormat, lpAccountName);
            MessageBoxW(hwndDlg, szMessage, szTitle, MB_OK | MB_ICONINFORMATION);
        }
    }

    LsaClose(PolicyHandle);
}


static
BOOL
OnQueryInitialFocus(
    HWND hwndDlg,
    PLOGONDATA pLogonData)
{
    HWND hwnd = GetDlgItem(hwndDlg, pLogonData->bLocalSystem ? IDC_LOGON_SYSTEMACCOUNT : IDC_LOGON_THISACCOUNT);

    SetWindowLongPtr(hwndDlg, DWLP_MSGRESULT, (LPARAM)hwnd);

    return TRUE;
}


static
BOOL
OnApply(
    HWND hwndDlg,
    PLOGONDATA pLogonData)
{
    WCHAR szAccountName[64];
    WCHAR szPassword1[64];
    WCHAR szPassword2[64];
    DWORD dwServiceType = SERVICE_NO_CHANGE;
    BOOL bRet = TRUE;

    if (!pLogonData->bAccountChanged)
        return TRUE;

    if (SendDlgItemMessageW(hwndDlg, IDC_LOGON_SYSTEMACCOUNT, BM_GETCHECK, 0, 0) == BST_CHECKED)
    {
        /* System account selected */
        wcscpy(szAccountName, L"LocalSystem");
        wcscpy(szPassword1, L"");
        wcscpy(szPassword2, L"");

        /* Handle the interactive flag */
        dwServiceType = pLogonData->pServiceConfig->dwServiceType;
        if (SendDlgItemMessageW(hwndDlg, IDC_LOGON_INTERACTIVE, BM_GETCHECK, 0, 0) == BST_CHECKED)
            dwServiceType |= SERVICE_INTERACTIVE_PROCESS;
        else
            dwServiceType &= ~SERVICE_INTERACTIVE_PROCESS;
    }
    else
    {
        /* Other account selected */
        GetDlgItemText(hwndDlg, IDC_LOGON_ACCOUNTNAME, szAccountName, 64);
        GetDlgItemText(hwndDlg, IDC_LOGON_PASSWORD1, szPassword1, 64);
        GetDlgItemText(hwndDlg, IDC_LOGON_PASSWORD2, szPassword2, 64);

        if (wcscmp(szPassword1, szPassword2))
        {
            ResourceMessageBox(GetModuleHandle(NULL), hwndDlg, MB_OK | MB_ICONWARNING, IDS_APPNAME, IDS_NOT_SAME_PASSWORD);
            return FALSE;
        }

        if (!wcscmp(szPassword1, DEFAULT_PASSWORD))
        {
            ResourceMessageBox(GetModuleHandle(NULL), hwndDlg, MB_OK | MB_ICONWARNING, IDS_APPNAME, IDS_INVALID_PASSWORD);
            return FALSE;
        }
    }


    bRet = SetServiceAccount(pLogonData->pService->lpServiceName,
                             dwServiceType,
                             szAccountName,
                             szPassword1);
    if (bRet == FALSE)
    {

    }

    if (bRet == TRUE)
    {
        pLogonData->bAccountChanged = FALSE;
        if (dwServiceType == SERVICE_NO_CHANGE)
            GrantServiceLogonRight(hwndDlg, szAccountName);

    }

    return bRet;
}


/*
 * Logon Property dialog callback.
 * Controls messages to the Logon dialog
 */
INT_PTR
CALLBACK
LogonPageProc(
    HWND hwndDlg,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    PLOGONDATA pLogonData;

    /* Get the window context */
    pLogonData = (PLOGONDATA)GetWindowLongPtr(hwndDlg,
                                              GWLP_USERDATA);
    if (pLogonData == NULL && uMsg != WM_INITDIALOG)
    {
        return FALSE;
    }

    switch (uMsg)
    {
        case WM_INITDIALOG:
            pLogonData = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LOGONDATA));
            if (pLogonData != NULL)
            {
                SetWindowLongPtr(hwndDlg,
                                 GWLP_USERDATA,
                                 (LONG_PTR)pLogonData);

                pLogonData->bInitialized = FALSE;
                pLogonData->pService = ((PSERVICEPROPSHEET)(((LPPROPSHEETPAGE)lParam)->lParam))->pService;

                pLogonData->pServiceConfig = GetServiceConfig(pLogonData->pService->lpServiceName);
                if (pLogonData->pServiceConfig != NULL)
                {
                    wcscpy(pLogonData->szPassword1, DEFAULT_PASSWORD);
                    wcscpy(pLogonData->szPassword2, DEFAULT_PASSWORD);

                    if (pLogonData->pServiceConfig->lpServiceStartName == NULL ||
                        _wcsicmp(pLogonData->pServiceConfig->lpServiceStartName, L"LocalSystem") == 0)
                    {
                        SendDlgItemMessageW(hwndDlg, IDC_LOGON_SYSTEMACCOUNT, BM_SETCHECK, (WPARAM)BST_CHECKED, 0);
                        if (pLogonData->pServiceConfig->dwServiceType & SERVICE_INTERACTIVE_PROCESS) {
                            pLogonData->nInteractive = BST_CHECKED;
                            SendDlgItemMessageW(hwndDlg, IDC_LOGON_INTERACTIVE, BM_SETCHECK, (WPARAM)BST_CHECKED, 0);
                        }
                        SetControlStates(hwndDlg, pLogonData, TRUE);
                    }
                    else
                    {
                        wcscpy(pLogonData->szAccountName, pLogonData->pServiceConfig->lpServiceStartName);
                        SendDlgItemMessageW(hwndDlg, IDC_LOGON_THISACCOUNT, BM_SETCHECK, (WPARAM)BST_CHECKED, 0);
                        SetControlStates(hwndDlg, pLogonData, FALSE);
                    }
                }

                pLogonData->bInitialized = TRUE;
            }

            EnableWindow(GetDlgItem(hwndDlg, IDC_LOGON_HWPROFILE), FALSE);
            break;

        case WM_DESTROY:
            if (pLogonData->pServiceConfig)
                HeapFree(GetProcessHeap(), 0, pLogonData->pServiceConfig);

            HeapFree(GetProcessHeap(), 0, pLogonData);
            break;

        case WM_COMMAND:
            switch(LOWORD(wParam))
            {
                case IDC_LOGON_SYSTEMACCOUNT:
                    if (HIWORD(wParam) == BN_CLICKED)
                    {
                        if (pLogonData->bInitialized)
                        {
                            pLogonData->bAccountChanged = TRUE;
                            PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                        }
                        SetControlStates(hwndDlg, pLogonData, TRUE);
                    }
                    break;

                case IDC_LOGON_THISACCOUNT:
                    if (HIWORD(wParam) == BN_CLICKED)
                    {
                        if (pLogonData->bInitialized)
                        {
                            pLogonData->bAccountChanged = TRUE;
                            PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                        }
                        SetControlStates(hwndDlg, pLogonData, FALSE);
                    }
                    break;

                case IDC_LOGON_INTERACTIVE:
                    if (HIWORD(wParam) == BN_CLICKED)
                    {
                        if (pLogonData->bInitialized)
                        {
                            pLogonData->bAccountChanged = TRUE;
                            PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                        }
                    }
                    break;

                case IDC_LOGON_ACCOUNTNAME:
                case IDC_LOGON_PASSWORD1:
                case IDC_LOGON_PASSWORD2:
                    if (HIWORD(wParam) == EN_CHANGE && pLogonData->bInitialized)
                    {
                        pLogonData->bAccountChanged = TRUE;
                        PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                    }
                    break;
            }
            break;

        case WM_NOTIFY:
            switch (((LPNMHDR)lParam)->code)
            {
                case PSN_QUERYINITIALFOCUS:
                    return OnQueryInitialFocus(hwndDlg, pLogonData);

                case PSN_APPLY:
                    return OnApply(hwndDlg, pLogonData);
            }
            break;
    }

    return FALSE;
}
