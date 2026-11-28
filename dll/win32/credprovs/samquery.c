/*
 * PROJECT:     LiberNT Credential Providers
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Asks SAM whether a local account has a blank password
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/* Kept out of the precompiled header: ntsam.h needs the native types. */
#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#include <windef.h>
#include <winbase.h>
#include <sddl.h>
#define NTOS_MODE_USER
#include <ndk/rtlfuncs.h>
#include <ntsecapi.h>
#include <ntsam.h>

BOOL PasswordProvider_SidHasBlankPassword(_In_ PCWSTR SidString);

/* USER_LOGON_UI_INFORMATION is what Windows' sign-in screen reads: SAM looks at the stored
 * password itself, so there is no logon attempt and no bad-password count. Any failure
 * (not a local account, no access, SAM unavailable) answers FALSE. */
BOOL
PasswordProvider_SidHasBlankPassword(_In_ PCWSTR SidString)
{
    PSID Sid = NULL, DomainSid = NULL;
    SAM_HANDLE Server = NULL, Domain = NULL, User = NULL;
    PUSER_LOGON_UI_INFORMATION Info = NULL;
    UCHAR Count;
    ULONG Rid;
    BOOL Blank = FALSE;

    if (!SidString || !ConvertStringSidToSidW(SidString, &Sid) || !IsValidSid(Sid))
        goto Done;
    Count = *GetSidSubAuthorityCount(Sid);
    if (Count < 2)
        goto Done;
    Rid = *GetSidSubAuthority(Sid, Count - 1);
    DomainSid = HeapAlloc(GetProcessHeap(), 0, GetLengthSid(Sid));
    if (!DomainSid || !CopySid(GetLengthSid(Sid), DomainSid, Sid))
        goto Done;
    *GetSidSubAuthorityCount(DomainSid) = Count - 1;

    if (NT_SUCCESS(SamConnect(NULL, &Server, SAM_SERVER_CONNECT | SAM_SERVER_LOOKUP_DOMAIN, NULL)) &&
        NT_SUCCESS(SamOpenDomain(Server, DOMAIN_LOOKUP, DomainSid, &Domain)) &&
        NT_SUCCESS(SamOpenUser(Domain, USER_READ_ACCOUNT, Rid, &User)) &&
        NT_SUCCESS(SamQueryInformationUser(User, UserLogonUIInformation, (PVOID *)&Info)) && Info)
    {
        Blank = Info->PasswordIsBlank && !Info->AccountIsDisabled;
    }

Done:
    if (Info)
        SamFreeMemory(Info);
    if (User)
        SamCloseHandle(User);
    if (Domain)
        SamCloseHandle(Domain);
    if (Server)
        SamCloseHandle(Server);
    if (DomainSid)
        HeapFree(GetProcessHeap(), 0, DomainSid);
    if (Sid)
        LocalFree(Sid);
    return Blank;
}
