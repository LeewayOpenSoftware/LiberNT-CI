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

typedef struct _DESKTOP_GRANT
{
    LIST_ENTRY ListEntry;
    LONG Count;
    BOOL WinStaAdded;
    BOOL DesktopAdded;
    PSID Sid;
    WCHAR Path[MAX_PATH];
} DESKTOP_GRANT, *PDESKTOP_GRANT;

typedef struct _DESKTOP_GRANT_WAIT
{
    PDESKTOP_GRANT Grant;
    HANDLE hProcess;
    HANDLE hWait;
    LONG Owners;
} DESKTOP_GRANT_WAIT, *PDESKTOP_GRANT_WAIT;

static LIST_ENTRY DesktopGrantList = { &DesktopGrantList, &DesktopGrantList };

static
BOOL
FindAllowAce(
    _In_opt_ PACL Dacl,
    _In_ PSID Sid,
    _Out_opt_ PDWORD Index)
{
    ACL_SIZE_INFORMATION Information;
    PACCESS_ALLOWED_ACE Ace;
    DWORD i;

    if (Dacl == NULL ||
        !GetAclInformation(Dacl, &Information, sizeof(Information), AclSizeInformation))
    {
        return FALSE;
    }
    for (i = 0; i < Information.AceCount; i++)
    {
        if (GetAce(Dacl, i, (PVOID *)&Ace) &&
            Ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
            EqualSid((PSID)&Ace->SidStart, Sid))
        {
            if (Index)
                *Index = i;
            return TRUE;
        }
    }
    return FALSE;
}

static
DWORD
AllowSid(
    _In_ HANDLE hObject,
    _In_ PSID Sid,
    _In_ ACCESS_MASK Access,
    _Out_ PBOOL Added)
{
    PSECURITY_DESCRIPTOR SecurityDescriptor = NULL;
    PACL OldDacl = NULL, NewDacl = NULL;
    EXPLICIT_ACCESSW Entry;
    DWORD dwError;

    *Added = FALSE;
    dwError = GetSecurityInfo(hObject, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION,
                              NULL, NULL, &OldDacl, NULL, &SecurityDescriptor);
    if (dwError != ERROR_SUCCESS)
        return dwError;

    if (FindAllowAce(OldDacl, Sid, NULL))
    {
        LocalFree(SecurityDescriptor);
        return ERROR_SUCCESS;
    }

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
        *Added = (dwError == ERROR_SUCCESS);
        LocalFree(NewDacl);
    }
    LocalFree(SecurityDescriptor);
    return dwError;
}

static
VOID
RevokeSid(
    _In_ HANDLE hObject,
    _In_ PSID Sid)
{
    PSECURITY_DESCRIPTOR SecurityDescriptor = NULL;
    PACL Dacl = NULL;
    DWORD Index;

    if (GetSecurityInfo(hObject, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION,
                        NULL, NULL, &Dacl, NULL, &SecurityDescriptor) != ERROR_SUCCESS)
    {
        return;
    }
    if (FindAllowAce(Dacl, Sid, &Index) && DeleteAce(Dacl, Index))
        SetSecurityInfo(hObject, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, Dacl, NULL);
    LocalFree(SecurityDescriptor);
}

/* Caller holds DesktopAccessLock: the desktop is opened through the process window station */
static
DWORD
OpenDesktopPath(
    _In_ PCWSTR DesktopPath,
    _Out_ HWINSTA *phWinSta,
    _Out_ HDESK *phDesktop)
{
    WCHAR WinStaName[MAX_PATH];
    PCWSTR DesktopName, Separator;
    HWINSTA hOldWinSta;
    DWORD dwError;

    *phWinSta = NULL;
    *phDesktop = NULL;
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

    *phWinSta = OpenWindowStationW(WinStaName, FALSE, READ_CONTROL | WRITE_DAC | WINSTA_ENUMDESKTOPS);
    if (*phWinSta == NULL)
        return GetLastError();

    hOldWinSta = GetProcessWindowStation();
    if (!SetProcessWindowStation(*phWinSta))
        return GetLastError();
    *phDesktop = OpenDesktopW(DesktopName, 0, FALSE, READ_CONTROL | WRITE_DAC);
    dwError = *phDesktop ? ERROR_SUCCESS : GetLastError();
    SetProcessWindowStation(hOldWinSta);
    return dwError;
}

static
VOID
CloseDesktopPath(
    _In_opt_ HWINSTA hWinSta,
    _In_opt_ HDESK hDesktop)
{
    if (hDesktop)
        CloseDesktop(hDesktop);
    if (hWinSta)
        CloseWindowStation(hWinSta);
}

/* Windows grants the new logon access to the window station and desktop it inherits */
static
PDESKTOP_GRANT
GrantDesktopAccess(
    _In_ HANDLE hToken,
    _In_ PCWSTR DesktopPath,
    _Out_ PDWORD pdwError)
{
    PDESKTOP_GRANT Grant = NULL;
    PLIST_ENTRY Entry;
    HWINSTA hWinSta;
    HDESK hDesktop;
    PVOID SidBuffer;
    PSID Sid;
    DWORD dwError, SidLength;

    if (wcslen(DesktopPath) >= MAX_PATH)
    {
        *pdwError = ERROR_INVALID_PARAMETER;
        return NULL;
    }
    Sid = GetLogonSid(hToken, &SidBuffer);
    if (Sid == NULL)
    {
        *pdwError = GetLastError();
        return NULL;
    }

    EnterCriticalSection(&DesktopAccessLock);
    for (Entry = DesktopGrantList.Flink; Entry != &DesktopGrantList; Entry = Entry->Flink)
    {
        Grant = CONTAINING_RECORD(Entry, DESKTOP_GRANT, ListEntry);
        if (EqualSid(Grant->Sid, Sid) && !_wcsicmp(Grant->Path, DesktopPath))
        {
            Grant->Count++;
            dwError = ERROR_SUCCESS;
            goto done;
        }
    }

    SidLength = GetLengthSid(Sid);
    Grant = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Grant) + SidLength);
    if (Grant == NULL)
    {
        dwError = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    Grant->Count = 1;
    Grant->Sid = (PSID)(Grant + 1);
    CopySid(SidLength, Grant->Sid, Sid);
    wcscpy(Grant->Path, DesktopPath);

    dwError = OpenDesktopPath(DesktopPath, &hWinSta, &hDesktop);
    if (dwError == ERROR_SUCCESS)
        dwError = AllowSid(hWinSta, Sid, WINSTA_ALL_ACCESS | STANDARD_RIGHTS_REQUIRED, &Grant->WinStaAdded);
    if (dwError == ERROR_SUCCESS)
    {
        dwError = AllowSid(hDesktop, Sid,
                           DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU |
                           DESKTOP_HOOKCONTROL | DESKTOP_JOURNALRECORD | DESKTOP_JOURNALPLAYBACK |
                           DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP |
                           STANDARD_RIGHTS_REQUIRED,
                           &Grant->DesktopAdded);
    }
    if (dwError != ERROR_SUCCESS)
    {
        if (Grant->WinStaAdded)
            RevokeSid(hWinSta, Sid);
        HeapFree(GetProcessHeap(), 0, Grant);
        Grant = NULL;
    }
    else
    {
        InsertTailList(&DesktopGrantList, &Grant->ListEntry);
    }
    CloseDesktopPath(hWinSta, hDesktop);

done:
    LeaveCriticalSection(&DesktopAccessLock);
    HeapFree(GetProcessHeap(), 0, SidBuffer);
    *pdwError = dwError;
    return Grant;
}

static
VOID
ReleaseDesktopGrant(
    _In_ PDESKTOP_GRANT Grant)
{
    HWINSTA hWinSta;
    HDESK hDesktop;

    EnterCriticalSection(&DesktopAccessLock);
    if (--Grant->Count != 0)
    {
        LeaveCriticalSection(&DesktopAccessLock);
        return;
    }
    RemoveEntryList(&Grant->ListEntry);
    OpenDesktopPath(Grant->Path, &hWinSta, &hDesktop);
    if (hDesktop && Grant->DesktopAdded)
        RevokeSid(hDesktop, Grant->Sid);
    if (hWinSta && Grant->WinStaAdded)
        RevokeSid(hWinSta, Grant->Sid);
    CloseDesktopPath(hWinSta, hDesktop);
    LeaveCriticalSection(&DesktopAccessLock);
    HeapFree(GetProcessHeap(), 0, Grant);
}

static
VOID
FinishDesktopGrantWait(
    _In_ PDESKTOP_GRANT_WAIT Wait)
{
    if (InterlockedDecrement(&Wait->Owners) != 0)
        return;
    UnregisterWait(Wait->hWait);
    ReleaseDesktopGrant(Wait->Grant);
    CloseHandle(Wait->hProcess);
    HeapFree(GetProcessHeap(), 0, Wait);
}

static
VOID
CALLBACK
DesktopGrantProcessExited(
    _In_ PVOID Context,
    _In_ BOOLEAN TimedOut)
{
    UNREFERENCED_PARAMETER(TimedOut);
    FinishDesktopGrantWait(Context);
}

/* Keeps Grant until hProcess exits; FALSE leaves Grant with the caller */
static
BOOL
WatchDesktopGrant(
    _In_ PDESKTOP_GRANT Grant,
    _In_ HANDLE hProcess)
{
    PDESKTOP_GRANT_WAIT Wait;

    Wait = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Wait));
    if (Wait == NULL)
        return FALSE;
    if (!DuplicateHandle(GetCurrentProcess(), hProcess, GetCurrentProcess(), &Wait->hProcess,
                         SYNCHRONIZE, FALSE, 0))
    {
        HeapFree(GetProcessHeap(), 0, Wait);
        return FALSE;
    }
    Wait->Grant = Grant;
    Wait->Owners = 2;
    if (!RegisterWaitForSingleObject(&Wait->hWait, Wait->hProcess, DesktopGrantProcessExited,
                                     Wait, INFINITE, WT_EXECUTEONLYONCE))
    {
        CloseHandle(Wait->hProcess);
        HeapFree(GetProcessHeap(), 0, Wait);
        return FALSE;
    }
    FinishDesktopGrantWait(Wait);
    return TRUE;
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


/* The token of the RPC client itself, never that of a process the request names */
static
BOOL
SeclClientPrimaryToken(
    _In_ handle_t hBinding,
    _Out_ PHANDLE phToken)
{
    HANDLE hClientToken = NULL;
    DWORD dwError;
    BOOL bResult;

    *phToken = NULL;

    if (RpcImpersonateClient(hBinding) != RPC_S_OK)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    bResult = OpenThreadToken(GetCurrentThread(), TOKEN_DUPLICATE | TOKEN_QUERY, TRUE, &hClientToken);
    dwError = GetLastError();
    RpcRevertToSelf();

    if (!bResult)
    {
        SetLastError(dwError);
        return FALSE;
    }

    bResult = DuplicateTokenEx(hClientToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation,
                               TokenPrimary, phToken);
    CloseHandle(hClientToken);
    return bResult;
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
    PDESKTOP_GRANT Grant = NULL;
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

    hTargetProcessHandle = OpenProcess(PROCESS_DUP_HANDLE,
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
        rc = SeclClientPrimaryToken(hBinding, &hToken);
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
        Grant = GrantDesktopAccess(hToken, StartupInfo.lpDesktop, &dwError);
        if (Grant == NULL)
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

    if (Grant && WatchDesktopGrant(Grant, ProcessInfo.hProcess))
        Grant = NULL;

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

    if (Grant != NULL)
        ReleaseDesktopGrant(Grant);

    if (Environment != NULL)
        DestroyEnvironmentBlock(Environment);

    if (ProfileInfo.hProfile != NULL)
        UnloadUserProfile(hToken, ProfileInfo.hProfile);

    if (hToken != NULL)
        CloseHandle(hToken);


    if (pResponse != NULL)
        pResponse->dwError = dwError;
}
