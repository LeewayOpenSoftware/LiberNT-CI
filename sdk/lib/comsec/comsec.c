/*
 * PROJECT:     LiberNT COM security
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Access, launch and activation checks shared by combase and RpcSs
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Sources: the documented registry values of COM security (AppID AccessPermission and
 * LaunchPermission, HKLM\Software\Microsoft\Ole DefaultAccessPermission,
 * DefaultLaunchPermission, MachineAccessRestriction, MachineLaunchRestriction) and the
 * COM_RIGHTS_* names of combaseapi.h.
 */

#include <stdio.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <sddl.h>
#include <comsec.h>

NTSYSAPI BOOLEAN NTAPI RtlValidRelativeSecurityDescriptor(PSECURITY_DESCRIPTOR, ULONG, SECURITY_INFORMATION);

#ifndef COM_RIGHTS_EXECUTE
#define COM_RIGHTS_EXECUTE 1
#endif
#define COMSEC_ALL_RIGHTS 0x1f  /* execute, local, remote, activate local, activate remote */
#define COMSEC_MAX_SD     0x10000
#define COMSEC_MAX_EVENTS 64     /* per minute */

DWORD comsec_read_mode(void)
{
    DWORD value = COMSEC_OFF, size = sizeof(value), type;
    HKEY key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\LiberNT\\COM", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key))
        return COMSEC_OFF;
    if (RegQueryValueExW(key, L"Enforcement", NULL, &type, (BYTE *)&value, &size) || type != REG_DWORD ||
        size != sizeof(value) || value > COMSEC_ENFORCE)
        value = COMSEC_OFF;
    RegCloseKey(key);
    return value;
}

/* The rule is per descriptor: legacy when every allow and deny ACE is exactly COM_RIGHTS_EXECUTE */
static void comsec_widen_legacy(PACL acl)
{
    ACE_HEADER *ace;
    DWORD i, count = 0;

    for (i = 0; i < acl->AceCount; i++)
    {
        if (!GetAce(acl, i, (void **)&ace))
            return;
        if (ace->AceType == ACCESS_ALLOWED_ACE_TYPE || ace->AceType == ACCESS_DENIED_ACE_TYPE)
        {
            if (((ACCESS_ALLOWED_ACE *)ace)->Mask != COM_RIGHTS_EXECUTE)
                return;
            count++;
        }
    }
    for (i = 0; count && i < acl->AceCount; i++)
    {
        if (GetAce(acl, i, (void **)&ace) &&
            (ace->AceType == ACCESS_ALLOWED_ACE_TYPE || ace->AceType == ACCESS_DENIED_ACE_TYPE))
            ((ACCESS_ALLOWED_ACE *)ace)->Mask = COMSEC_ALL_RIGHTS;
    }
}

PSECURITY_DESCRIPTOR comsec_normalize(PSECURITY_DESCRIPTOR sd, DWORD length)
{
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    PSECURITY_DESCRIPTOR result = NULL;
    SECURITY_DESCRIPTOR absolute;
    BOOL present = FALSE, defaulted;
    ACL *dacl = NULL, *copy = NULL;
    PSID system = NULL;
    DWORD size = 0;

    if (!sd || (length && !RtlValidRelativeSecurityDescriptor(sd, length, 0)) ||
        !IsValidSecurityDescriptor(sd) || !GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted))
        return NULL;
    if (present && dacl)
    {
        if (!IsValidAcl(dacl) || !(copy = LocalAlloc(LMEM_FIXED, dacl->AclSize)))
            return NULL;
        memcpy(copy, dacl, dacl->AclSize);
        comsec_widen_legacy(copy);
    }
    if (AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &system) &&
        InitializeSecurityDescriptor(&absolute, SECURITY_DESCRIPTOR_REVISION) &&
        SetSecurityDescriptorOwner(&absolute, system, FALSE) &&
        SetSecurityDescriptorGroup(&absolute, system, FALSE) &&
        SetSecurityDescriptorDacl(&absolute, present, copy, FALSE))
    {
        MakeSelfRelativeSD(&absolute, NULL, &size);
        if (size && (result = LocalAlloc(LMEM_FIXED, size)) && !MakeSelfRelativeSD(&absolute, result, &size))
        {
            LocalFree(result);
            result = NULL;
        }
    }
    if (system)
        FreeSid(system);
    LocalFree(copy);
    return result;
}

static PSECURITY_DESCRIPTOR comsec_from_sddl(const WCHAR *sddl);

PSECURITY_DESCRIPTOR comsec_deny_sd(void)
{
    return comsec_from_sddl(L"D:");
}

PSECURITY_DESCRIPTOR comsec_read_sd_key(HKEY key, const WCHAR *value)
{
    PSECURITY_DESCRIPTOR raw, result = NULL;
    DWORD type, size = 0;
    LONG error;

    error = RegQueryValueExW(key, value, NULL, &type, NULL, &size);
    if (error == ERROR_FILE_NOT_FOUND)
        return NULL;
    if (!error && type == REG_BINARY && size >= sizeof(SECURITY_DESCRIPTOR_RELATIVE) && size <= COMSEC_MAX_SD &&
        (raw = LocalAlloc(LMEM_FIXED, size)))
    {
        if (!RegQueryValueExW(key, value, NULL, &type, raw, &size) && type == REG_BINARY)
            result = comsec_normalize(raw, size);
        LocalFree(raw);
    }
    /* Present but unusable: refuse rather than fall back to a wider default */
    return result ? result : comsec_deny_sd();
}

PSECURITY_DESCRIPTOR comsec_read_sd(HKEY root, const WCHAR *subkey, const WCHAR *value)
{
    PSECURITY_DESCRIPTOR result;
    HKEY key;

    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key))
        return NULL;
    result = comsec_read_sd_key(key, value);
    RegCloseKey(key);
    return result;
}

static PSECURITY_DESCRIPTOR comsec_from_sddl(const WCHAR *sddl)
{
    PSECURITY_DESCRIPTOR sd, result;
    ULONG size;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, &size))
        return NULL;
    result = comsec_normalize(sd, size);
    LocalFree(sd);
    return result;
}

PSECURITY_DESCRIPTOR comsec_default_launch_sd(void)
{
    return comsec_from_sddl(L"D:(A;;0x1f;;;SY)(A;;0x1f;;;BA)(A;;0x1f;;;IU)");
}

PSECURITY_DESCRIPTOR comsec_default_access_sd(PSID self)
{
    PSECURITY_DESCRIPTOR result = NULL;
    WCHAR *string, sddl[256];

    if (!self || !ConvertSidToStringSidW(self, &string))
        return NULL;
    if (wcslen(string) < 180)
    {
        _snwprintf(sddl, ARRAYSIZE(sddl), L"D:(A;;0x7;;;SY)(A;;0x7;;;%s)", string);
        result = comsec_from_sddl(sddl);
    }
    LocalFree(string);
    return result;
}

BOOL comsec_check(HANDLE token, PSECURITY_DESCRIPTOR sd, DWORD rights)
{
    GENERIC_MAPPING mapping = { COM_RIGHTS_EXECUTE, COM_RIGHTS_EXECUTE, COM_RIGHTS_EXECUTE, COMSEC_ALL_RIGHTS };
    BYTE privileges[sizeof(PRIVILEGE_SET) + 4 * sizeof(LUID_AND_ATTRIBUTES)];
    DWORD privileges_size = sizeof(privileges), granted = 0;
    BOOL status = FALSE;

    if (!sd)
        return TRUE;
    if (!token)
        return FALSE;
    if (!AccessCheck(sd, token, COM_RIGHTS_EXECUTE | rights, &mapping, (PRIVILEGE_SET *)privileges,
                     &privileges_size, &granted, &status))
        return FALSE;
    return status;
}

PSID comsec_token_user(HANDLE token)
{
    BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *user = (TOKEN_USER *)buffer;
    DWORD size;
    PSID sid;

    if (!GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size))
        return NULL;
    size = GetLengthSid(user->User.Sid);
    if ((sid = LocalAlloc(LMEM_FIXED, size)) && !CopySid(size, sid, user->User.Sid))
    {
        LocalFree(sid);
        sid = NULL;
    }
    return sid;
}

DWORD comsec_token_integrity(HANDLE token)
{
    BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    TOKEN_MANDATORY_LABEL *label = (TOKEN_MANDATORY_LABEL *)buffer;
    DWORD size;

    if (!GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size) ||
        !*GetSidSubAuthorityCount(label->Label.Sid))
        return SECURITY_MANDATORY_MEDIUM_RID;
    return *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
}

BOOL comsec_token_logon(HANDLE token, LUID *logon)
{
    TOKEN_STATISTICS statistics;
    DWORD size;

    if (!GetTokenInformation(token, TokenStatistics, &statistics, sizeof(statistics), &size))
        return FALSE;
    *logon = statistics.AuthenticationId;
    return TRUE;
}

void comsec_report_denial(DWORD mode, const WCHAR *what, const GUID *clsid, HANDLE token, DWORD pid)
{
    static LONG reported, window;
    WCHAR guid[40] = L"-", process[16], *user = NULL;
    const WCHAR *strings[5];
    HANDLE source;
    PSID sid = NULL;
    LONG count;

    /* A client that polls (combase asks RpcSs again every few milliseconds while a server
     * starts) must not flood the log, and RpcSs must keep logging after a burst */
    if ((LONG)(GetTickCount() / 60000) != window)
    {
        window = GetTickCount() / 60000;
        reported = 0;
    }
    count = InterlockedIncrement(&reported);
    if (count > COMSEC_MAX_EVENTS + 1)
        return;
    if (clsid)
        _snwprintf(guid, ARRAYSIZE(guid), L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", clsid->Data1,
                   clsid->Data2, clsid->Data3, clsid->Data4[0], clsid->Data4[1], clsid->Data4[2], clsid->Data4[3],
                   clsid->Data4[4], clsid->Data4[5], clsid->Data4[6], clsid->Data4[7]);
    _snwprintf(process, ARRAYSIZE(process), L"%lu", pid);
    if (token && (sid = comsec_token_user(token)))
        ConvertSidToStringSidW(sid, &user);
    strings[0] = mode == COMSEC_ENFORCE ? L"refused" : L"would be refused (audit)";
    strings[1] = what;
    strings[2] = guid;
    strings[3] = user ? user : L"unknown";
    strings[4] = count > COMSEC_MAX_EVENTS ? L"further denials in this minute are not logged" : process;

    if ((source = RegisterEventSourceW(NULL, L"LiberNT-COM")))
    {
        ReportEventW(source, mode == COMSEC_ENFORCE ? EVENTLOG_ERROR_TYPE : EVENTLOG_WARNING_TYPE, 0,
                     mode == COMSEC_ENFORCE ? 1 : 2, sid, ARRAYSIZE(strings), 0, strings, NULL);
        DeregisterEventSource(source);
    }
    if (user)
        LocalFree(user);
    LocalFree(sid);
}
