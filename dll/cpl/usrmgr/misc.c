/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS User Manager Control Panel
 * FILE:            dll/cpl/usrmgr/misc.c
 * PURPOSE:         Miscellaneous functions
 *
 * PROGRAMMERS:     Eric Kohl
 */

#include "usrmgr.h"

BOOL
CheckAccountName(HWND hwndDlg,
                 INT nIdDlgItem,
                 LPTSTR lpAccountName)
{
    TCHAR szAccountName[256];
    UINT uLen;

    if (lpAccountName)
        uLen = _tcslen(lpAccountName);
    else
        uLen = GetDlgItemText(hwndDlg, nIdDlgItem, szAccountName, 256);

    /* Check the account name */
    if (uLen > 0 &&
        _tcspbrk((lpAccountName) ? lpAccountName : szAccountName, TEXT("\"*+,/\\:;<=>?[]|")) != NULL)
    {
        MessageBox(hwndDlg,
                   TEXT("The account name you entered is invalid! An account name must not contain the following characters: *+,/:;<=>?[\\]|"),
                   TEXT("ERROR"),
                   MB_OK | MB_ICONERROR);
        return FALSE;
    }

    return TRUE;
}

/* Shows the text of a failed NetXxx call instead of its number */
VOID
ShowNetworkError(NET_API_STATUS status)
{
    TCHAR szCaption[64];
    TCHAR szText[512];
    HMODULE hNetMsg = NULL;
    DWORD dwFlags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD dwLen;

    /* The NERR_xxx codes (2100 - 2999) are described by netmsg.dll */
    if (status >= NERR_BASE && status <= MAX_NERR)
    {
        hNetMsg = LoadLibraryEx(TEXT("netmsg.dll"), NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (hNetMsg)
            dwFlags = FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS;
    }

    dwLen = FormatMessage(dwFlags, hNetMsg, status, 0, szText, ARRAYSIZE(szText), NULL);
    if (hNetMsg)
        FreeLibrary(hNetMsg);
    if (dwLen == 0)
        wsprintf(szText, TEXT("%lu"), (ULONG)status);      /* no text for this code: show the number */

    if (!LoadString(hApplet, IDS_CPLNAME, szCaption, ARRAYSIZE(szCaption)))
        szCaption[0] = TEXT('\0');

    MessageBox(NULL, szText, szCaption, MB_ICONERROR | MB_OK);
}
