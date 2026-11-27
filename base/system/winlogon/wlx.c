/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS Winlogon
 * FILE:            base/system/winlogon/wlx.c
 * PURPOSE:         Logon
 * PROGRAMMERS:     Thomas Weidenmueller (w3seek@users.sourceforge.net)
 *                  Ge van Geldorp (gvg@reactos.com)
 *                  Hervé Poussineau (hpoussin@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include "winlogon.h"

typedef struct _DIALOG_LIST_ENTRY
{
    LIST_ENTRY Entry;
    HWND hWnd;
    DLGPROC DlgProc;
    LPARAM lParam;
} DIALOG_LIST_ENTRY, *PDIALOG_LIST_ENTRY;

/* GLOBALS ******************************************************************/

//static UINT_PTR IdTimer;
static LIST_ENTRY DialogListHead;

/* FUNCTIONS ****************************************************************/

VOID
InitDialogListHead(VOID)
{
    InitializeListHead(&DialogListHead);
}

VOID
CloseAllDialogWindows(VOID)
{
    PDIALOG_LIST_ENTRY Current;
    PLIST_ENTRY ListEntry;

    ListEntry = DialogListHead.Flink;
    while (ListEntry != &DialogListHead)
    {
        Current = CONTAINING_RECORD(ListEntry,
                                    DIALOG_LIST_ENTRY,
                                    Entry);

        PostMessage(Current->hWnd, WLX_WM_SAS, WLX_SAS_TYPE_TIMEOUT, 0);

        ListEntry = ListEntry->Flink;
    }
}

BOOL
CreateWindowStationAndDesktops(
    _Inout_ PWLSESSION Session)
{
    SECURITY_ATTRIBUTES WinstaSecurity;
    SECURITY_ATTRIBUTES ApplicationDesktopSecurity;
    SECURITY_ATTRIBUTES WinlogonDesktopSecurity;
    SECURITY_ATTRIBUTES ScreenSaverDesktopSecurity;
    PSECURITY_DESCRIPTOR WlWinstaSecurityDescriptor = NULL;
    PSECURITY_DESCRIPTOR WlApplicationDesktopSecurityDescriptor = NULL;
    PSECURITY_DESCRIPTOR WlWinlogonDesktopSecurityDescriptor = NULL;
    PSECURITY_DESCRIPTOR WlScreenSaverDesktopSecurityDescriptor = NULL;
    BOOL ret = FALSE;

    if (!CreateWinstaSecurity(&WlWinstaSecurityDescriptor))
    {
        ERR("WL: Failed to create winsta security!\n");
        return ret;
    }

    WinstaSecurity.nLength = sizeof(SECURITY_ATTRIBUTES);
    WinstaSecurity.lpSecurityDescriptor = WlWinstaSecurityDescriptor;
    WinstaSecurity.bInheritHandle = TRUE;

    if (!CreateApplicationDesktopSecurity(&WlApplicationDesktopSecurityDescriptor))
    {
        ERR("WL: Failed to create application desktop security!\n");
        goto cleanup;
    }

    ApplicationDesktopSecurity.nLength = sizeof(SECURITY_ATTRIBUTES);
    ApplicationDesktopSecurity.lpSecurityDescriptor = WlApplicationDesktopSecurityDescriptor;
    ApplicationDesktopSecurity.bInheritHandle = TRUE;

    if (!CreateWinlogonDesktopSecurity(&WlWinlogonDesktopSecurityDescriptor))
    {
        ERR("WL: Failed to create winlogon desktop security!\n");
        goto cleanup;
    }

    WinlogonDesktopSecurity.nLength = sizeof(SECURITY_ATTRIBUTES);
    WinlogonDesktopSecurity.lpSecurityDescriptor = WlWinlogonDesktopSecurityDescriptor;
    WinlogonDesktopSecurity.bInheritHandle = FALSE;

    if (!CreateScreenSaverSecurity(&WlScreenSaverDesktopSecurityDescriptor))
    {
        ERR("WL: Failed to create winlogon desktop security!\n");
        goto cleanup;
    }

    ScreenSaverDesktopSecurity.nLength = sizeof(SECURITY_ATTRIBUTES);
    ScreenSaverDesktopSecurity.lpSecurityDescriptor = WlScreenSaverDesktopSecurityDescriptor;
    ScreenSaverDesktopSecurity.bInheritHandle = TRUE;

    /*
     * Create the interactive window station
     */
    Session->InteractiveWindowStationName = L"WinSta0";
    Session->InteractiveWindowStation = CreateWindowStationW(
        Session->InteractiveWindowStationName,
        0,
        MAXIMUM_ALLOWED,
        &WinstaSecurity);
    if (!Session->InteractiveWindowStation)
    {
        ERR("WL: Failed to create window station (%lu)\n", GetLastError());
        goto cleanup;
    }

    if (!SetProcessWindowStation(Session->InteractiveWindowStation))
    {
        ERR("WL: SetProcessWindowStation() failed (error %lu)\n", GetLastError());
        goto cleanup;
    }

    /*
     * Create the application desktop
     */
    Session->ApplicationDesktop = CreateDesktopW(
        L"Default",
        NULL,
        NULL,
        0, /* FIXME: Add DF_ALLOWOTHERACCOUNTHOOK flag? */
        MAXIMUM_ALLOWED,
        &ApplicationDesktopSecurity);
    if (!Session->ApplicationDesktop)
    {
        ERR("WL: Failed to create Default desktop (%lu)\n", GetLastError());
        goto cleanup;
    }

    /*
     * Create the winlogon desktop
     */
    Session->WinlogonDesktop = CreateDesktopW(
        L"Winlogon",
        NULL,
        NULL,
        0,
        MAXIMUM_ALLOWED,
        &WinlogonDesktopSecurity);
    if (!Session->WinlogonDesktop)
    {
        ERR("WL: Failed to create Winlogon desktop (%lu)\n", GetLastError());
        goto cleanup;
    }

    /*
     * Create the screen saver desktop
     */
    Session->ScreenSaverDesktop = CreateDesktopW(
        L"Screen-Saver",
        NULL,
        NULL,
        0,
        MAXIMUM_ALLOWED,
        &ScreenSaverDesktopSecurity);
    if(!Session->ScreenSaverDesktop)
    {
        ERR("WL: Failed to create Screen-Saver desktop (%lu)\n", GetLastError());
        goto cleanup;
    }

    /*
     * Switch to winlogon desktop
    */
    if (!SetThreadDesktop(Session->WinlogonDesktop) ||
        !SwitchDesktop(Session->WinlogonDesktop))
    {
        ERR("WL: Cannot switch to Winlogon desktop (%lu)\n", GetLastError());
        goto cleanup;
    }

    SetWindowStationUser(Session->InteractiveWindowStation,
                         &LuidNone, NULL, 0);

    ret = TRUE;

cleanup:
    if (!ret)
    {
        if (Session->ApplicationDesktop)
        {
            CloseDesktop(Session->ApplicationDesktop);
            Session->ApplicationDesktop = NULL;
        }
        if (Session->WinlogonDesktop)
        {
            CloseDesktop(Session->WinlogonDesktop);
            Session->WinlogonDesktop = NULL;
        }
        if (Session->ScreenSaverDesktop)
        {
            CloseDesktop(Session->ScreenSaverDesktop);
            Session->ScreenSaverDesktop = NULL;
        }
        if (Session->InteractiveWindowStation)
        {
            CloseWindowStation(Session->InteractiveWindowStation);
            Session->InteractiveWindowStation = NULL;
        }
    }

    /* Free security descriptors regardless of success or failure */
    if (WlWinstaSecurityDescriptor)
        RtlFreeHeap(RtlGetProcessHeap(), 0, WlWinstaSecurityDescriptor);
    if (WlApplicationDesktopSecurityDescriptor)
        RtlFreeHeap(RtlGetProcessHeap(), 0, WlApplicationDesktopSecurityDescriptor);
    if (WlWinlogonDesktopSecurityDescriptor)
        RtlFreeHeap(RtlGetProcessHeap(), 0, WlWinlogonDesktopSecurityDescriptor);
    if (WlScreenSaverDesktopSecurityDescriptor)
        RtlFreeHeap(RtlGetProcessHeap(), 0, WlScreenSaverDesktopSecurityDescriptor);

    return ret;
}
