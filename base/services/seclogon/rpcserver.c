/*
 * PROJECT:     ReactOS Secondary Logon Service
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Secondary Logon service RPC server
 * COPYRIGHT:   Eric Kohl 2022 <eric.kohl@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include "precomp.h"

#include <seclogon_s.h>

WINE_DEFAULT_DEBUG_CHANNEL(seclogon);

/* GLOBALS *******************************************************************/

CRITICAL_SECTION DesktopAccessLock;

/* FUNCTIONS *****************************************************************/

static
PSID
GetLogonSid(
    _In_ HANDLE hToken,
    _Out_ PVOID *Buffer)
{
    PTOKEN_GROUPS Groups;
    PTOKEN_USER User;
    DWORD Length = 0, i;

    *Buffer = NULL;
    GetTokenInformation(hToken, TokenGroups, NULL, 0, &Length);
    if (Length && (Groups = HeapAlloc(GetProcessHeap(), 0, Length)) != NULL)
    {
        if (GetTokenInformation(hToken, TokenGroups, Groups, Length, &Length))
        {
            for (i = 0; i < Groups->GroupCount; i++)
            {
                if ((Groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID)
                {
                    *Buffer = Groups;
                    return Groups->Groups[i].Sid;
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, Groups);
    }

    Length = 0;
    GetTokenInformation(hToken, TokenUser, NULL, 0, &Length);
    if (Length && (User = HeapAlloc(GetProcessHeap(), 0, Length)) != NULL)
    {
        if (GetTokenInformation(hToken, TokenUser, User, Length, &Length))
        {
            *Buffer = User;
            return User->User.Sid;
        }
        HeapFree(GetProcessHeap(), 0, User);
    }
    return NULL;
}

static
DWORD
AllowSid(
    _In_ HANDLE hObject,
    _In_ PSID Sid,
    _In_ ACCESS_MASK Access)
{
    PSECURITY_DESCRIPTOR SecurityDescriptor = NULL;
    PACL OldDacl = NULL, NewDacl = NULL;
    EXPLICIT_ACCESSW Entry;
    DWORD dwError;

    dwError = GetSecurityInfo(hObject, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION,
                              NULL, NULL, &OldDacl, NULL, &SecurityDescriptor);
    if (dwError != ERROR_SUCCESS)
        return dwError;

    ZeroMemory(&Entry, sizeof(Entry));
    Entry.grfAccessPermissions = Access;
    Entry.grfAccessMode = GRANT_ACCESS;
    Entry.grfInheritance = NO_INHERITANCE;
    Entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    Entry.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    Entry.Trustee.ptstrName = (LPWSTR)Sid;

    dwError = SetEntriesInAclW(1, &Entry, OldDacl, &NewDacl);
    if (dwError == ERROR_SUCCESS)
    {
        dwError = SetSecurityInfo(hObject, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION,
                                  NULL, NULL, NewDacl, NULL);
        LocalFree(NewDacl);
    }
    LocalFree(SecurityDescriptor);
    return dwError;
}

/* Windows grants the new logon access to the window station and desktop it inherits */
static
DWORD
GrantDesktopAccess(
    _In_ HANDLE hToken,
    _In_ PCWSTR DesktopPath)
{
    WCHAR WinStaName[MAX_PATH];
    PCWSTR DesktopName, Separator;
    HWINSTA hWinSta = NULL, hOldWinSta;
    HDESK hDesktop = NULL;
    PVOID SidBuffer;
    PSID Sid;
    DWORD dwError;

    Separator = wcschr(DesktopPath, L'\\');
    if (Separator)
    {
        if ((SIZE_T)(Separator - DesktopPath) >= ARRAYSIZE(WinStaName))
            return ERROR_INVALID_PARAMETER;
        CopyMemory(WinStaName, DesktopPath, (Separator - DesktopPath) * sizeof(WCHAR));
        WinStaName[Separator - DesktopPath] = UNICODE_NULL;
        DesktopName = Separator + 1;
    }
    else
    {
        wcscpy(WinStaName, L"WinSta0");
        DesktopName = DesktopPath;
    }

    Sid = GetLogonSid(hToken, &SidBuffer);
    if (Sid == NULL)
        return GetLastError();

    EnterCriticalSection(&DesktopAccessLock);
    hOldWinSta = GetProcessWindowStation();
    hWinSta = OpenWindowStationW(WinStaName, FALSE, READ_CONTROL | WRITE_DAC | WINSTA_ENUMDESKTOPS);
    if (hWinSta == NULL)
    {
        dwError = GetLastError();
        goto done;
    }
    dwError = AllowSid(hWinSta, Sid, WINSTA_ALL_ACCESS | STANDARD_RIGHTS_REQUIRED);
    if (dwError != ERROR_SUCCESS)
        goto done;

    if (!SetProcessWindowStation(hWinSta))
    {
        dwError = GetLastError();
        goto done;
    }
    hDesktop = OpenDesktopW(DesktopName, 0, FALSE, READ_CONTROL | WRITE_DAC);
    dwError = hDesktop ? ERROR_SUCCESS : GetLastError();
    SetProcessWindowStation(hOldWinSta);
    if (hDesktop)
    {
        dwError = AllowSid(hDesktop, Sid,
                           DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU |
                           DESKTOP_HOOKCONTROL | DESKTOP_JOURNALRECORD | DESKTOP_JOURNALPLAYBACK |
                           DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP |
                           STANDARD_RIGHTS_REQUIRED);
        CloseDesktop(hDesktop);
    }

done:
    if (hWinSta)
        CloseWindowStation(hWinSta);
    LeaveCriticalSection(&DesktopAccessLock);
    HeapFree(GetProcessHeap(), 0, SidBuffer);
    return dwError;
}


void __RPC_FAR * __RPC_USER midl_user_allocate(SIZE_T len)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
}


void __RPC_USER midl_user_free(void __RPC_FAR * ptr)
{
    HeapFree(GetProcessHeap(), 0, ptr);
}


DWORD
StartRpcServer(VOID)
{
    NTSTATUS Status;

    Status = lpServiceGlobals->StartRpcServer(L"seclogon", ISeclogon_v1_0_s_ifspec);
    TRACE("StartRpcServer returned 0x%08lx\n", Status);

    return RtlNtStatusToDosError(Status);
}


DWORD
StopRpcServer(VOID)
{
    NTSTATUS Status;

    Status = lpServiceGlobals->StopRpcServer(ISeclogon_v1_0_s_ifspec);
    TRACE("StopRpcServer returned 0x%08lx\n", Status);

    return RtlNtStatusToDosError(Status);
}


VOID
__stdcall
SeclCreateProcessWithLogonW(
    _In_ handle_t hBinding,
    _In_ SECL_REQUEST *pRequest,
    _Out_ SECL_RESPONSE *pResponse)
{
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;

    PROFILEINFOW ProfileInfo;
    HANDLE hToken = NULL;
    HANDLE hCallerToken = NULL;
    HANDLE hTargetProcessHandle = NULL;
    PVOID Environment = NULL;
    DWORD CreationFlags;
    DWORD SessionId;

    ULONG dwError = ERROR_SUCCESS;
    BOOL rc;

    TRACE("SeclCreateProcessWithLogonW(%p %p %p)\n", hBinding, pRequest, pResponse);

    ZeroMemory(&ProfileInfo, sizeof(ProfileInfo));
    ZeroMemory(&ProcessInfo, sizeof(ProcessInfo));

    if (pRequest != NULL)
    {
        TRACE("Username: '%S'\n", pRequest->Username);
        TRACE("Domain: '%S'\n", pRequest->Domain);
        TRACE("ApplicationName: '%S'\n", pRequest->ApplicationName);
        TRACE("CommandLine: '%S'\n", pRequest->CommandLine);
        TRACE("CurrentDirectory: '%S'\n", pRequest->CurrentDirectory);
        TRACE("LogonFlags: 0x%lx\n", pRequest->dwLogonFlags);
        TRACE("CreationFlags: 0x%lx\n", pRequest->dwCreationFlags);
        TRACE("ProcessId: %lu\n", pRequest->dwProcessId);
    }

    hTargetProcessHandle = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE,
                                       pRequest->dwProcessId);
    if (hTargetProcessHandle == NULL)
    {
        dwError = GetLastError();
        WARN("OpenProcess() failed with Error %lu\n", dwError);
        goto done;
    }

    if (pRequest->dwLogonFlags & LOGON_NETCREDENTIALS_ONLY)
    {
        /* FIXME: the credentials are not attached to the new process for network access */
        rc = OpenProcessToken(hTargetProcessHandle, TOKEN_DUPLICATE, &hCallerToken) &&
             DuplicateTokenEx(hCallerToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation,
                              TokenPrimary, &hToken);
    }
    else
    {
        rc = LogonUser(pRequest->Username,
                       pRequest->Domain,
                       pRequest->Password,
                       LOGON32_LOGON_INTERACTIVE,
                       LOGON32_PROVIDER_DEFAULT,
                       &hToken);
    }
    if (rc == FALSE)
    {
        dwError = GetLastError();
        WARN("Getting the token failed with Error %lu\n", dwError);
        goto done;
    }

    if (!ProcessIdToSessionId(pRequest->dwProcessId, &SessionId) ||
        !SetTokenInformation(hToken, TokenSessionId, &SessionId, sizeof(SessionId)))
    {
        dwError = GetLastError();
        WARN("Setting the token session failed with Error %lu\n", dwError);
        goto done;
    }

    /* Load the user profile */
    if (pRequest->dwLogonFlags & LOGON_WITH_PROFILE)
    {
        ProfileInfo.dwSize = sizeof(ProfileInfo);
        ProfileInfo.lpUserName = pRequest->Username;

        rc = LoadUserProfileW(hToken,
                              &ProfileInfo);
        if (rc == FALSE)
        {
            dwError = GetLastError();
            WARN("LoadUserProfile() failed with Error %lu\n", dwError);
            goto done;
        }
    }

    /* Initialize the startup information */
    ZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);

    StartupInfo.lpDesktop = pRequest->Desktop ? pRequest->Desktop : L"WinSta0\\Default";
    if (pRequest->dwDesktopInherited)
    {
        /* TODO: remove the grant when the last process of this logon ends */
        dwError = GrantDesktopAccess(hToken, StartupInfo.lpDesktop);
        if (dwError != ERROR_SUCCESS)
            WARN("Granting access to %S failed with Error %lu\n", StartupInfo.lpDesktop, dwError);
        dwError = ERROR_SUCCESS;
    }

    /* FIXME: Get the rest of the startup info from the caller */

    CreationFlags = pRequest->dwCreationFlags;
    if (pRequest->Environment == NULL)
    {
        if (!CreateEnvironmentBlock(&Environment, hToken, FALSE))
        {
            dwError = GetLastError();
            WARN("CreateEnvironmentBlock() failed with Error %lu\n", dwError);
            goto done;
        }
        CreationFlags |= CREATE_UNICODE_ENVIRONMENT;
    }

    /* Create Process */
    rc = CreateProcessAsUserW(hToken,
                              pRequest->ApplicationName,
                              pRequest->CommandLine,
                              NULL,  // lpProcessAttributes,
                              NULL,  // lpThreadAttributes,
                              FALSE, // bInheritHandles,
                              CreationFlags,
                              Environment ? Environment : pRequest->Environment,
                              pRequest->CurrentDirectory,
                              &StartupInfo,
                              &ProcessInfo);
    if (rc == FALSE)
    {
        dwError = GetLastError();
        WARN("CreateProcessAsUser() failed with Error %lu\n", dwError);
        goto done;
    }

    /* Return process info to the caller */
    if (pResponse != NULL)
    {
        DuplicateHandle(GetCurrentProcess(),
                        ProcessInfo.hProcess,
                        hTargetProcessHandle,
                        (PHANDLE)&pResponse->hProcess,
                        0,
                        FALSE,
                        DUPLICATE_SAME_ACCESS);

        DuplicateHandle(GetCurrentProcess(),
                        ProcessInfo.hThread,
                        hTargetProcessHandle,
                        (PHANDLE)&pResponse->hThread,
                        0,
                        FALSE,
                        DUPLICATE_SAME_ACCESS);

        pResponse->dwProcessId = ProcessInfo.dwProcessId;
        pResponse->dwThreadId = ProcessInfo.dwThreadId;
    }

done:
    if (hTargetProcessHandle)
        CloseHandle(hTargetProcessHandle);

    if (ProcessInfo.hThread)
        CloseHandle(ProcessInfo.hThread);

    if (ProcessInfo.hProcess)
        CloseHandle(ProcessInfo.hProcess);

    if (Environment != NULL)
        DestroyEnvironmentBlock(Environment);

    if (ProfileInfo.hProfile != NULL)
        UnloadUserProfile(hToken, ProfileInfo.hProfile);

    if (hToken != NULL)
        CloseHandle(hToken);

    if (hCallerToken != NULL)
        CloseHandle(hCallerToken);

    if (pResponse != NULL)
        pResponse->dwError = dwError;
}
