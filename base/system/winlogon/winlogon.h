/*
 *  ReactOS kernel
 *  Copyright (C) 2003 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
/* COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS winlogon
 * FILE:            base/system/winlogon/winlogon.h
 * PURPOSE:         Winlogon
 * PROGRAMMER:
 */

#ifndef __WINLOGON_MAIN_H__
#define __WINLOGON_MAIN_H__

#define USE_GETLASTINPUTINFO


#include <stdarg.h>

/* PSDK/NDK Headers */
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <winreg.h>
#include <winwlx.h>
#include <ndk/rtlfuncs.h>
#include <ndk/exfuncs.h>
#include <ndk/kefuncs.h>
#include <ndk/pofuncs.h>
#include <strsafe.h>

/* PSEH for SEH Support */
#include <pseh/pseh2.h>

#include <undocuser.h>
#include <undocmpr.h>

BOOL
WINAPI
SetWindowStationUser(
    IN HWINSTA hWindowStation,
    IN PLUID pluid,
    IN PSID psid OPTIONAL,
    IN DWORD size);

#include <wine/debug.h>
WINE_DEFAULT_DEBUG_CHANNEL(winlogon);

#include "resource.h"

#define WL_WM_SETUP_DONE (WM_USER + 0x0100)
/* Posted to the SAS window by hostlogon.c from RPC and supervisor threads (wParam = WLHOST_EVENT_*). */
#define WL_WM_HOST_EVENT (WM_USER + 0x0101)
#define WLHOST_EVENT_ENDED          1   /* the live host left; relaunch per state */
#define WLHOST_EVENT_UNLOCK         2   /* a verified unlock for the session user */
#define WLHOST_EVENT_SHUTDOWN       3   /* power flyout while locked or logged off outside a SAS */
#define WLHOST_EVENT_RESTART        4
#define WLHOST_EVENT_MUST_CHANGE    5   /* the unlock credentials must change their password first */

typedef BOOL (WINAPI * PFWLXNEGOTIATE)  (DWORD, DWORD *);
typedef BOOL (WINAPI * PFWLXINITIALIZE) (LPWSTR, HANDLE, PVOID, PVOID, PVOID *);
typedef VOID (WINAPI * PFWLXDISPLAYSASNOTICE) (PVOID);
typedef int  (WINAPI * PFWLXLOGGEDOUTSAS) (PVOID, DWORD, PLUID, PSID, PDWORD,
                                           PHANDLE, PWLX_MPR_NOTIFY_INFO,
                                           PVOID *);
typedef BOOL (WINAPI * PFWLXACTIVATEUSERSHELL) (PVOID, PWSTR, PWSTR, PVOID);
typedef int  (WINAPI * PFWLXLOGGEDONSAS) (PVOID, DWORD, PVOID);
typedef VOID (WINAPI * PFWLXDISPLAYLOCKEDNOTICE) (PVOID);
typedef int  (WINAPI * PFWLXWKSTALOCKEDSAS) (PVOID, DWORD);
typedef BOOL (WINAPI * PFWLXISLOCKOK) (PVOID);
typedef BOOL (WINAPI * PFWLXISLOGOFFOK) (PVOID);
typedef VOID (WINAPI * PFWLXLOGOFF) (PVOID);
typedef VOID (WINAPI * PFWLXSHUTDOWN) (PVOID, DWORD);

/* version 1.1 */

typedef BOOL (WINAPI * PFWLXSCREENSAVERNOTIFY) (PVOID, BOOL *);
typedef BOOL (WINAPI * PFWLXSTARTAPPLICATION) (PVOID, PWSTR, PVOID, PWSTR);

/* version 1.3 */

typedef BOOL (WINAPI * PFWLXNETWORKPROVIDERLOAD) (PVOID, PWLX_MPR_NOTIFY_INFO);
typedef BOOL (WINAPI * PFWLXDISPLAYSTATUSMESSAGE) (PVOID, HDESK, DWORD, PWSTR, PWSTR);
typedef BOOL (WINAPI * PFWLXGETSTATUSMESSAGE) (PVOID, DWORD *, PWSTR, DWORD);
typedef BOOL (WINAPI * PFWLXREMOVESTATUSMESSAGE) (PVOID);

typedef struct _GINAFUNCTIONS
{
    /* Functions always available for a valid GINA */
    PFWLXNEGOTIATE            WlxNegotiate; /* optional */
    PFWLXINITIALIZE           WlxInitialize;

    /* Functions available if WlxVersion >= WLX_VERSION_1_0 (MS Windows 3.5.0) */
    PFWLXDISPLAYSASNOTICE     WlxDisplaySASNotice;
    PFWLXLOGGEDOUTSAS         WlxLoggedOutSAS;
    PFWLXACTIVATEUSERSHELL    WlxActivateUserShell;
    PFWLXLOGGEDONSAS          WlxLoggedOnSAS;
    PFWLXDISPLAYLOCKEDNOTICE  WlxDisplayLockedNotice;
    PFWLXWKSTALOCKEDSAS       WlxWkstaLockedSAS;
    PFWLXISLOCKOK             WlxIsLockOk;
    PFWLXISLOGOFFOK           WlxIsLogoffOk;
    PFWLXLOGOFF               WlxLogoff;
    PFWLXSHUTDOWN             WlxShutdown;

    /* Functions available if WlxVersion >= WLX_VERSION_1_1 (MS Windows 3.5.1) */
    PFWLXSCREENSAVERNOTIFY    WlxScreenSaverNotify; /* optional */
    PFWLXSTARTAPPLICATION     WlxStartApplication; /* optional */

    /* Functions available if WlxVersion >= WLX_VERSION_1_2 (MS Windows NT 4.0) */

    /* Functions available if WlxVersion >= WLX_VERSION_1_3 (MS Windows 2000) */
    PFWLXNETWORKPROVIDERLOAD  WlxNetworkProviderLoad; /* not called ATM */
    PFWLXDISPLAYSTATUSMESSAGE WlxDisplayStatusMessage;
    PFWLXGETSTATUSMESSAGE     WlxGetStatusMessage; /* doesn't need to be called */
    PFWLXREMOVESTATUSMESSAGE  WlxRemoveStatusMessage;

    /* Functions available if WlxVersion >= WLX_VERSION_1_4 (MS Windows XP) */
} GINAFUNCTIONS, *PGINAFUNCTIONS;

typedef struct _GINAINSTANCE
{
    HMODULE hDllInstance;
    GINAFUNCTIONS Functions;
    PVOID Context;
    DWORD Version;
    BOOL UseCtrlAltDelete;
} GINAINSTANCE, *PGINAINSTANCE;


/*
 * The picture Microsoft is trying to paint here
 * (https://learn.microsoft.com/en-us/windows/win32/secauthn/winlogon-states)
 * about the Winlogon states is a little too simple.
 *
 * The real picture should look more like this:
 *
 * STATE_INIT
 *    Initial state. Required for session initialization. After initialization,
 *    the state will automatically change to STATE_LOGGED_OFF.
 *
 * STATE_LOGGED_OFF
 *    User is logged off. Winlogon shows the "Press Ctrl-Alt-Del for logon"
 *    dialog. The state changes to STATE_LOGGED_OFF_SAS when the user presses
 *    "Ctrl-Alt-Del". If DisableCAD is true, the state will automatically
 *    change to STATE_LOGGED_OFF_SAS without showing the dialog.
 *
 * STATE_LOGGED_OFF_SAS
 *    State shows the logon dialog. Entering the right credentials and pressing
 *    "OK" changes the state to STATE_LOGGED_ON. Pressing "Cancel" or a timeout
 *    changes the state back to STATE_LOGGED_OFF.
 *
 * STATE_LOGGED_ON
 *    User is logged on. Winlogon does not show any dialog. Pressing
 *    "Ctrl-Alt-Del" changes the state to STATE_LOGGED_ON_SAS and user
 *    inactivity changes the state to STATE_SCREENSAVER.
 *
 * STATE_LOGGED_ON_SAS
 *    Winlogon shows the security dialog. Pressing "Cancel" or "Task Manager"
 *    or a timeout change the state back to STATE_LOGGED_ON. Pressing "Change
 *    Password" does not change the state, because the security dialog is still
 *    visible behind the change password dialog. Pressing "Log off" changes the
 *    state to STATE_LOGGING_OFF. Pressing "Lock Computer" changes the state to
 *    STATE_LOCKED. Pressing "Shutdown" changes the state to
 *    STATE_SHUTTING_DOWN.
 *
 * STATE_LOCKED
 *    Winlogon shows the locked message dialog. When the user presses "Ctrl-
 *    Alt-Del" the state changes to STATE_LOCKED_SAS. If DisableCAD is true,
 *    the state will automatically change to STATE_LOCKED_SAS without showing
 *    the dialog.
 *
 * STATE_LOCKED_SAS
 *    Winlogon shows the unlock dialog. Pressing "Cancel" or a timeout will
 *    change the state back to STATE_LOCKED. Entering the right credentials and
 *    pressing "OK" unlocks the computer and changes the state to
 *    STATE_LOGGED_ON.
 *
 * STATE_LOGGING_OFF
 *    Winlogon shows the logoff dialog. Pressing "Cancel" or a timeout changes
 *    the state back to STATE_LOGGED_ON_SAS. Pressing "OK" logs off the user
 *    and changes the state to STATE_LOGGED_OFF.
 *
 * STATE_SHUTTING_DOWN
 *    Winlogon shows the shutdown dialog. Pressing "Cancel" or a timeout will
 *    change the state back to STATE_LOGGED_ON_SAS. Pressing "OK" will change
 *    the state to STATE_SHUT_DOWN.
 *
 * STATE_SHUT_DOWN
 *    Terminates Winlogon and initiates shut-down.
 *
 * STATE_SCREENSAVER
 *    Winlogon runs the screen saver. Upon user activity, the screensaver
 *    terminates and the state changes back to STATE_LOGGED_ON if the secure
 *    screen saver option is off. Otherwise, the state changes to STATE_LOCKED.
 */
typedef enum _LOGON_STATE
{
    STATE_INIT,
    STATE_LOGGED_OFF,
    STATE_LOGGED_OFF_SAS,
    STATE_LOGGED_ON,
    STATE_LOGGED_ON_SAS,
    STATE_LOCKED,
    STATE_LOCKED_SAS,
    STATE_LOGGING_OFF,     // not used yet
    STATE_SHUTTING_DOWN,   // not used yet
    STATE_SHUT_DOWN,       // not used yet
    STATE_SCREENSAVER      // not used yet
} LOGON_STATE, *PLOGON_STATE;

#define LockWorkstation(Session)
#define UnlockWorkstation(Session)

typedef struct _WLSESSION
{
    GINAINSTANCE Gina;
    DWORD SASAction;
    BOOL SuppressStatus;
    BOOL TaskManHotkey;
    BOOL LockWkStaHotkey;
    HWND SASWindow;
    HWINSTA InteractiveWindowStation;
    PWSTR InteractiveWindowStationName;
    PWSTR UserName;
    PWSTR Domain;
    HDESK ApplicationDesktop;
    HDESK WinlogonDesktop;
    HDESK ScreenSaverDesktop;
    LUID LogonId;
    HANDLE UserToken;
    HANDLE hProfileInfo;
    LOGON_STATE LogonState;
    DWORD DialogTimeout; /* Timeout for dialog boxes, in seconds */
    LARGE_INTEGER LastLogon;

    /* Screen-saver informations */
#ifndef USE_GETLASTINPUTINFO
    HHOOK KeyboardHook;
    HHOOK MouseHook;
#endif
    HANDLE hEndOfScreenSaverThread;
    HANDLE hScreenSaverParametersChanged;
    HANDLE hUserActivity;
    HANDLE hEndOfScreenSaver;
#ifndef USE_GETLASTINPUTINFO
    DWORD LastActivity;
#endif

    /* Logon informations */
    DWORD Options;
    WLX_MPR_NOTIFY_INFO MprNotifyInfo;
    PWLX_PROFILE_V2_0 Profile;

    /* Experimental LogonUI-mode selector (Winlogon\UseLogonUI), read once at start */
    BOOL UseLogonUI;
    BOOL HostAutoSas;           /* the pending SAS came from hostlogon.c, not from the user */
} WLSESSION, *PWLSESSION;

typedef enum _NOTIFICATION_TYPE
{
    LogonHandler,
    LogoffHandler,
    LockHandler,
    UnlockHandler,
    StartupHandler,
    ShutdownHandler,
    StartScreenSaverHandler,
    StopScreenSaverHandler,
    DisconnectHandler,
    ReconnectHandler,
    StartShellHandler,
    PostShellHandler,
    LastHandler
} NOTIFICATION_TYPE, *PNOTIFICATION_TYPE;

extern HINSTANCE hAppInstance;
extern PWLSESSION WLSession;

#define WLX_LOGGINGOFF(wlxAction) \
  (((wlxAction) == WLX_SAS_ACTION_LOGOFF) || \
   ((wlxAction) == WLX_SAS_ACTION_FORCE_LOGOFF))

#define WLX_SHUTTINGDOWN(wlxAction) \
  (((wlxAction) == WLX_SAS_ACTION_SHUTDOWN) || \
   ((wlxAction) == WLX_SAS_ACTION_SHUTDOWN_POWER_OFF) || \
   ((wlxAction) == WLX_SAS_ACTION_SHUTDOWN_REBOOT))

#define WLX_SUSPENDING(wlxAction) \
  (((wlxAction) == WLX_SAS_ACTION_SHUTDOWN_SLEEP)  || \
   ((wlxAction) == WLX_SAS_ACTION_SHUTDOWN_SLEEP2) || \
   ((wlxAction) == WLX_SAS_ACTION_SHUTDOWN_HIBERNATE))

FORCEINLINE
VOID
SetLogonTimestamp(
    _Inout_ PWLSESSION Session)
{
    NtQuerySystemTime(&Session->LastLogon);
}

FORCEINLINE
BOOL
IsFirstLogon(
    _In_ PWLSESSION Session)
{
    /* The WLSESSION::LastLogon is initialized to 0 so this is OK */
    return (Session->LastLogon.QuadPart == 0);
}

/* environment.c */
BOOL
CreateUserEnvironment(IN PWLSESSION Session);

/* notify.c */
BOOL
InitNotifications(VOID);

VOID
CleanupNotifications(VOID);

VOID
CallNotificationDlls(
    PWLSESSION pSession,
    NOTIFICATION_TYPE Type);

/* rpclogonui.c */
#include <wluiauth.h>
struct _WLUI_COMMAND;

typedef DWORD (*WLUI_SUBMIT_HANDLER)(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ WLUI_AUTH_ROLE Role,
    _In_ DWORD Scenario,
    _In_ const GUID *ProviderClsid,
    _In_ DWORD AuthenticationPackage,
    _In_reads_bytes_(cbSerialization) const BYTE *Serialization,
    _In_ DWORD cbSerialization);

typedef DWORD (*WLUI_ACTION_HANDLER)(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ WLUI_AUTH_ROLE Role,
    _In_ DWORD Action);

BOOL
WluiServerStart(VOID);

PWLUI_LAUNCH_STATE
WluiLaunchRegister(
    _In_ DWORD Pid,
    _In_ PSID Sid,
    _In_ WLUI_AUTH_ROLE Role,
    _In_opt_ const GUID *RequestedId,
    _Out_ GUID *LaunchId);

VOID
WluiLaunchUnregister(
    _In_ PWLUI_LAUNCH_STATE State);

BOOL
WluiLaunchPostCommand(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ const struct _WLUI_COMMAND *Command);

BOOL
WluiLaunchWaitReady(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ DWORD TimeoutMs);

ULONGLONG
WluiLaunchLastHeartbeat(
    _In_ PWLUI_LAUNCH_STATE State);

BOOL
WluiLaunchExited(
    _In_ PWLUI_LAUNCH_STATE State,
    _Out_opt_ DWORD *Reason);

VOID
WluiSetSubmitHandler(
    _In_opt_ WLUI_SUBMIT_HANDLER Handler);

VOID
WluiSetActionHandler(
    _In_opt_ WLUI_ACTION_HANDLER Handler);

/* hostlaunch.c */
#include <wlrecovery.h>
typedef struct _WLHOST_LAUNCH *PWLHOST_LAUNCH;
typedef VOID (*WLHOST_ENDED_CALLBACK)(_In_ PWLHOST_LAUNCH Launch);

/* Why nothing can be launched (the N4 dialog says it). */
typedef enum _WLHOST_TERMINAL
{
    WlHostTerminalNone = 0,
    WlHostTerminalLevels,       /* every tier failed or the launch cap was hit */
    WlHostTerminalMissing,      /* neither logonuihost.exe nor logonrecovery.exe is there (R1b) */
    WlHostTerminalPolicy        /* 8.10 c: policy forbids the password-only tiers */
} WLHOST_TERMINAL;

VOID
WlHostLog(
    _In_ WORD Type,
    _In_ PCWSTR Text);

VOID
WlHostInit(VOID);

VOID
WlHostSetEndedCallback(
    _In_opt_ WLHOST_ENDED_CALLBACK Callback);

VOID
WlHostOnSas(VOID);

BOOL
WlHostIsTerminal(VOID);

PWLHOST_LAUNCH
WlHostStart(
    _In_ DWORD Scenario,
    _Out_ PBOOL Terminal,
    _Out_ WLHOST_TERMINAL *Reason);

BOOL
WlHostWaitReady(
    _In_ PWLHOST_LAUNCH Launch,
    _In_ DWORD TimeoutMs);

BOOL
WlHostPost(
    _In_ PWLHOST_LAUNCH Launch,
    _In_ const struct _WLUI_COMMAND *Command);

HANDLE
WlHostEndedEvent(
    _In_ PWLHOST_LAUNCH Launch);

BOOL
WlHostHasEnded(
    _In_ PWLHOST_LAUNCH Launch,
    _Out_opt_ WLREC_END *End,
    _Out_opt_ PDWORD ExitCode);

PWLUI_LAUNCH_STATE
WlHostState(
    _In_ PWLHOST_LAUNCH Launch);

VOID
WlHostStop(
    _In_ PWLHOST_LAUNCH Launch);

/* hostlogon.c */
VOID
WlHostAttach(
    _In_ PWLSESSION Session);

DWORD
WlHostLoggedOutSas(
    _In_ PWLSESSION Session);

VOID
WlHostLock(
    _In_ PWLSESSION Session);

VOID
WlHostMustChange(
    _In_ PWLSESSION Session);

VOID
WlHostLogoffAborted(
    _In_ PWLSESSION Session);

VOID
WlHostRelaunch(
    _In_ PWLSESSION Session);

BOOL
WlHostScreenSaverSecure(
    _In_ PWLSESSION Session);

VOID
WlHostDisplaySasNotice(
    _In_ PWLSESSION Session);

VOID
WlHostStatus(
    _In_ PCWSTR Text);

VOID
WlHostThemeWait(VOID);

VOID
WlHostCreateDesktopSwitchEvent(
    _In_ PWLSESSION Session);

VOID
WlHostWaitDesktopSwitch(
    _In_ PWLSESSION Session);

VOID
WlHostBeforeDesktopSwitch(VOID);

VOID
WlHostAfterDesktopSwitch(VOID);

VOID
WlHostLogonFailed(
    _In_ NTSTATUS Status);

BOOL
WlHostStartApplication(
    _In_ PWLSESSION Session,
    _In_ PCWSTR Desktop,
    _In_opt_ PVOID Environment,
    _In_ PCWSTR Command);

BOOL
WlHostActivateUserShell(
    _In_ PWLSESSION Session,
    _In_opt_ PVOID Environment);

/* rpcserver.c */
BOOL
StartRpcServer(VOID);

/* sas.c */
extern LUID LuidNone;

BOOL
SetDefaultLanguage(IN PWLSESSION Session);

NTSTATUS
HandleShutdown(IN OUT PWLSESSION Session,
               IN DWORD wlxAction);

BOOL
InitializeSAS(IN OUT PWLSESSION Session);

/* screensaver.c */
BOOL
InitializeScreenSaver(IN OUT PWLSESSION Session);

VOID
StartScreenSaver(IN PWLSESSION Session);

/* security.c */
PSECURITY_DESCRIPTOR
ConvertToSelfRelative(
    _In_ PSECURITY_DESCRIPTOR AbsoluteSd);

BOOL
CreateWinstaSecurity(
    _Out_ PSECURITY_DESCRIPTOR *WinstaSd);

BOOL
CreateApplicationDesktopSecurity(
    _Out_ PSECURITY_DESCRIPTOR *ApplicationDesktopSd);

BOOL
CreateWinlogonDesktopSecurity(
    _Out_ PSECURITY_DESCRIPTOR *WinlogonDesktopSd);

BOOL
CreateScreenSaverSecurity(
    _Out_ PSECURITY_DESCRIPTOR *ScreenSaverDesktopSd);

BOOL
AllowWinstaAccessToUser(
    _In_ HWINSTA WinSta,
    _In_ PSID LogonSid);

BOOL
AllowDesktopAccessToUser(
    _In_ HDESK Desktop,
    _In_ PSID LogonSid);

BOOL
AllowAccessOnSession(
    _In_ PWLSESSION Session);

BOOL
UpdateNamedObjectAccess(
    _In_ HANDLE UserToken,
    _In_ BOOL Grant);

/* setup.c */
DWORD
GetSetupType(VOID);

BOOL
RunSetup(VOID);

/* shutdown.h */
DWORD
TerminateSystemShutdown(VOID);

DWORD
StartSystemShutdown(
    IN PUNICODE_STRING pMessage,
    IN ULONG dwTimeout,
    IN BOOLEAN bForceAppsClosed,
    IN BOOLEAN bRebootAfterShutdown,
    IN ULONG dwReason);

/* winlogon.c */
PWSTR
WlStrDup(
    _In_opt_ PCWSTR String);

BOOL
PlaySoundRoutine(IN LPCWSTR FileName,
                 IN UINT Logon,
                 IN UINT Flags);

BOOL
DisplayStatusMessage(IN PWLSESSION Session,
                     IN HDESK hDesktop,
                     IN UINT ResourceId);

BOOL
RemoveStatusMessage(IN PWLSESSION Session);

/* wlx.c */
VOID
InitDialogListHead(VOID);

VOID
CloseAllDialogWindows(VOID);

BOOL
GinaInit(IN OUT PWLSESSION Session);

BOOL
CreateWindowStationAndDesktops(
    _Inout_ PWLSESSION Session);


VOID WINAPI WlxUseCtrlAltDel(HANDLE hWlx);
VOID WINAPI WlxSetContextPointer(HANDLE hWlx, PVOID pWlxContext);
VOID WINAPI WlxSasNotify(HANDLE hWlx, DWORD dwSasType);
BOOL WINAPI WlxSetTimeout(HANDLE hWlx, DWORD Timeout);
int WINAPI WlxAssignShellProtection(HANDLE hWlx, HANDLE hToken, HANDLE hProcess, HANDLE hThread);
int WINAPI WlxMessageBox(HANDLE hWlx, HWND hwndOwner, LPWSTR lpszText, LPWSTR lpszTitle, UINT fuStyle);
int WINAPI WlxDialogBox(HANDLE hWlx, HANDLE hInst, LPWSTR lpszTemplate, HWND hwndOwner, DLGPROC dlgprc);
int WINAPI WlxDialogBoxParam(HANDLE hWlx, HANDLE hInst, LPWSTR lpszTemplate, HWND hwndOwner, DLGPROC dlgprc, LPARAM dwInitParam);
int WINAPI WlxDialogBoxIndirect(HANDLE hWlx, HANDLE hInst, LPCDLGTEMPLATE hDialogTemplate, HWND hwndOwner, DLGPROC dlgprc);
int WINAPI WlxDialogBoxIndirectParam(HANDLE hWlx, HANDLE hInst, LPCDLGTEMPLATE hDialogTemplate, HWND hwndOwner, DLGPROC dlgprc, LPARAM dwInitParam);
int WINAPI WlxSwitchDesktopToUser(HANDLE hWlx);
int WINAPI WlxSwitchDesktopToWinlogon(HANDLE hWlx);
int WINAPI WlxChangePasswordNotify(HANDLE hWlx, PWLX_MPR_NOTIFY_INFO pMprInfo, DWORD dwChangeInfo);
BOOL WINAPI WlxGetSourceDesktop(HANDLE hWlx, PWLX_DESKTOP* ppDesktop);
BOOL WINAPI WlxSetReturnDesktop(HANDLE hWlx, PWLX_DESKTOP pDesktop);
BOOL WINAPI WlxCreateUserDesktop(HANDLE hWlx, HANDLE hToken, DWORD Flags, PWSTR pszDesktopName, PWLX_DESKTOP* ppDesktop);
int WINAPI WlxChangePasswordNotifyEx(HANDLE hWlx, PWLX_MPR_NOTIFY_INFO pMprInfo, DWORD dwChangeInfo, PWSTR ProviderName, PVOID Reserved);
BOOL WINAPI WlxCloseUserDesktop(HANDLE hWlx, PWLX_DESKTOP pDesktop, HANDLE hToken);
BOOL WINAPI WlxSetOption(HANDLE hWlx, DWORD Option, ULONG_PTR Value, ULONG_PTR* OldValue);
BOOL WINAPI WlxGetOption(HANDLE hWlx, DWORD Option, ULONG_PTR* Value);
VOID WINAPI WlxWin31Migrate(HANDLE hWlx);
BOOL WINAPI WlxQueryClientCredentials(PWLX_CLIENT_CREDENTIALS_INFO_V1_0 pCred);
BOOL WINAPI WlxQueryInetConnectorCredentials(PWLX_CLIENT_CREDENTIALS_INFO_V1_0 pCred);
DWORD WINAPI WlxQueryConsoleSwitchCredentials(PWLX_CONSOLESWITCH_CREDENTIALS_INFO_V1_0 pCred);
BOOL WINAPI WlxQueryTsLogonCredentials(PWLX_CLIENT_CREDENTIALS_INFO_V2_0 pCred);
BOOL WINAPI WlxDisconnect(VOID);
DWORD WINAPI WlxQueryTerminalServicesData(HANDLE hWlx, PWLX_TERMINAL_SERVICES_DATA pTSData, WCHAR* UserName, WCHAR* Domain);

/* winlogon.c */
VOID
LogonStage(
    _In_ PCSTR Stage);

#endif /* __WINLOGON_MAIN_H__ */
