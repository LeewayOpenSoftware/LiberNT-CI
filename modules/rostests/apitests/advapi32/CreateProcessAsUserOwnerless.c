/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     CreateProcessAsUser takes the owner and group of a process or thread descriptor that lacks them from the token
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <sddl.h>

static
BOOL
TokenSid(
    _In_ HANDLE hToken,
    _In_ TOKEN_INFORMATION_CLASS Class,
    _Out_writes_bytes_(Size) PBYTE Buffer,
    _In_ ULONG Size)
{
    ULONG Length;

    return GetTokenInformation(hToken, Class, Buffer, Size, &Length);
}

static
VOID
CheckObject(
    _In_ PCSTR What,
    _In_ HANDLE hObject,
    _In_ PSID ExpectedOwner,
    _In_ PSID ExpectedGroup)
{
    BYTE Buffer[512];
    PSECURITY_DESCRIPTOR Sd = Buffer;
    PSID Owner = NULL, Group = NULL;
    PACL Dacl = NULL;
    BOOL Defaulted, Present = FALSE;
    DWORD Length;

    if (!GetKernelObjectSecurity(hObject, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                 Sd, sizeof(Buffer), &Length))
    {
        ok(0, "%s: GetKernelObjectSecurity failed, error %lu\n", What, GetLastError());
        return;
    }
    GetSecurityDescriptorOwner(Sd, &Owner, &Defaulted);
    GetSecurityDescriptorGroup(Sd, &Group, &Defaulted);
    GetSecurityDescriptorDacl(Sd, &Present, &Dacl, &Defaulted);
    ok(Owner && EqualSid(Owner, ExpectedOwner), "%s: owner is not the token's owner\n", What);
    ok(Group && EqualSid(Group, ExpectedGroup), "%s: group is not the token's primary group\n", What);
    ok(Present && Dacl != NULL, "%s: the given DACL was not applied (present %d, acl %p)\n", What, Present, Dacl);
}

static
VOID
TestCase(
    _In_ HANDLE hToken,
    _In_ PCWSTR Sddl,
    _In_ BOOL OnProcess,
    _In_ BOOL OnThread,
    _In_ PSID ExpectedOwner,
    _In_ PSID ExpectedGroup)
{
    SECURITY_ATTRIBUTES ProcessAttributes = {sizeof(ProcessAttributes)};
    SECURITY_ATTRIBUTES ThreadAttributes = {sizeof(ThreadAttributes)};
    STARTUPINFOW StartupInfo = {sizeof(StartupInfo)};
    PROCESS_INFORMATION ProcessInfo = {0};
    WCHAR CommandLine[MAX_PATH + 32];
    PSECURITY_DESCRIPTOR Sd = NULL;
    BOOL Result;
    DWORD Error;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl, SDDL_REVISION_1, &Sd, NULL))
    {
        ok(0, "cannot convert %ls, error %lu\n", Sddl, GetLastError());
        return;
    }
    ProcessAttributes.lpSecurityDescriptor = OnProcess ? Sd : NULL;
    ThreadAttributes.lpSecurityDescriptor = OnThread ? Sd : NULL;

    GetSystemDirectoryW(CommandLine, MAX_PATH);
    StringCchCatW(CommandLine, ARRAYSIZE(CommandLine), L"\\cmd.exe /c exit");

    SetLastError(0xdeadbeef);
    Result = CreateProcessAsUserW(hToken, NULL, CommandLine, &ProcessAttributes, &ThreadAttributes, FALSE,
                                  CREATE_SUSPENDED, NULL, NULL, &StartupInfo, &ProcessInfo);
    Error = GetLastError();
    if (!Result && Error == ERROR_PRIVILEGE_NOT_HELD)
    {
        skip("%ls: the caller lacks SeIncreaseQuotaPrivilege\n", Sddl);
    }
    else
    {
        ok(Result, "%ls (process %d, thread %d): CreateProcessAsUserW failed, error %lu\n", Sddl, OnProcess, OnThread, Error);
        if (Result)
        {
            if (OnProcess)
                CheckObject("process", ProcessInfo.hProcess, ExpectedOwner, ExpectedGroup);
            if (OnThread)
                CheckObject("thread", ProcessInfo.hThread, ExpectedOwner, ExpectedGroup);
            TerminateProcess(ProcessInfo.hProcess, 0);
            CloseHandle(ProcessInfo.hThread);
            CloseHandle(ProcessInfo.hProcess);
        }
    }

    LocalFree(Sd);
}

START_TEST(CreateProcessAsUserOwnerless)
{
    BYTE OwnerBuffer[sizeof(TOKEN_OWNER) + SECURITY_MAX_SID_SIZE];
    BYTE GroupBuffer[sizeof(TOKEN_PRIMARY_GROUP) + SECURITY_MAX_SID_SIZE];
    HANDLE hProcessToken, hToken;
    PWSTR OwnerString = NULL, GroupString = NULL;
    WCHAR FullSddl[256];

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &hProcessToken))
    {
        skip("cannot open the process token, error %lu\n", GetLastError());
        return;
    }
    if (!DuplicateTokenEx(hProcessToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &hToken))
    {
        skip("cannot duplicate the token, error %lu\n", GetLastError());
        CloseHandle(hProcessToken);
        return;
    }
    CloseHandle(hProcessToken);

    ok(TokenSid(hToken, TokenOwner, OwnerBuffer, sizeof(OwnerBuffer)), "TokenOwner failed\n");
    ok(TokenSid(hToken, TokenPrimaryGroup, GroupBuffer, sizeof(GroupBuffer)), "TokenPrimaryGroup failed\n");

    /* Owner and group given: unchanged behaviour (the caller's own, as another owner needs privileges) */
    if (ConvertSidToStringSidW(((PTOKEN_OWNER)OwnerBuffer)->Owner, &OwnerString) &&
        ConvertSidToStringSidW(((PTOKEN_PRIMARY_GROUP)GroupBuffer)->PrimaryGroup, &GroupString))
    {
        StringCchPrintfW(FullSddl, ARRAYSIZE(FullSddl), L"O:%lsG:%lsD:P(A;;GA;;;SY)(A;;GA;;;BA)", OwnerString, GroupString);
        TestCase(hToken, FullSddl, TRUE, TRUE,
                 ((PTOKEN_OWNER)OwnerBuffer)->Owner, ((PTOKEN_PRIMARY_GROUP)GroupBuffer)->PrimaryGroup);
    }
    else
    {
        ok(0, "cannot format the token's owner and group, error %lu\n", GetLastError());
    }
    LocalFree(OwnerString);
    LocalFree(GroupString);
    /* No owner and no group: Windows applies the token's */
    TestCase(hToken, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", TRUE, FALSE,
             ((PTOKEN_OWNER)OwnerBuffer)->Owner, ((PTOKEN_PRIMARY_GROUP)GroupBuffer)->PrimaryGroup);
    TestCase(hToken, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", FALSE, TRUE,
             ((PTOKEN_OWNER)OwnerBuffer)->Owner, ((PTOKEN_PRIMARY_GROUP)GroupBuffer)->PrimaryGroup);
    TestCase(hToken, L"D:(A;;GA;;;SY)(A;;GA;;;BA)", TRUE, TRUE,
             ((PTOKEN_OWNER)OwnerBuffer)->Owner, ((PTOKEN_PRIMARY_GROUP)GroupBuffer)->PrimaryGroup);

    CloseHandle(hToken);
}
