/*
 * PROJECT:     LiberNT RPC subsystem service
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Starting a COM server under the identity its AppID names (RunAs)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#include <winreg.h>
#include <winerror.h>
#include <ntsecapi.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(ole);

/* rpcrt4 export without a public declaration */
RPC_STATUS WINAPI I_RpcOpenClientProcess(RPC_BINDING_HANDLE Binding, ULONG DesiredAccess, void **ClientProcess);

#define LAUNCH_COMMAND_MAX  (MAX_PATH + 64)

static BOOL launch_identity_enabled(void)
{
    DWORD value = 0, size = sizeof(value);

    return !RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\LiberNT\\COM", L"LaunchIdentity", RRF_RT_REG_DWORD,
                         NULL, &value, &size) && value == 1;
}

/* Windows' default launch permission: SYSTEM, Administrators and interactive users */
static BOOL launch_caller_allowed(HANDLE token)
{
    static const WELL_KNOWN_SID_TYPE allowed[] = { WinLocalSystemSid, WinBuiltinAdministratorsSid, WinInteractiveSid };
    BYTE sid[SECURITY_MAX_SID_SIZE];
    HANDLE check;
    DWORD size, i;
    BOOL member = FALSE;

    if (!DuplicateToken(token, SecurityIdentification, &check))
        return FALSE;
    for (i = 0; i < ARRAYSIZE(allowed) && !member; i++)
    {
        size = sizeof(sid);
        if (CreateWellKnownSid(allowed[i], NULL, sid, &size) && !CheckTokenMembership(check, sid, &member))
            member = FALSE;
    }
    CloseHandle(check);
    return member;
}

/* Only machine-wide registrations: a per-user one must not choose who rpcss starts code as */
static LONG launch_read_string(const WCHAR *subkey, const WCHAR *name, REGSAM view, WCHAR *value, DWORD count)
{
    DWORD size = count * sizeof(WCHAR);
    HKEY key;
    LONG ret;

    ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ | view, &key);
    if (ret)
        return ret;
    ret = RegGetValueW(key, NULL, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, value, &size);
    RegCloseKey(key);
    return ret;
}

/* The command line for the class in one registry view, or an error */
static HRESULT launch_command(const GUID *clsid, REGSAM view, WCHAR *appid, WCHAR *command)
{
    WCHAR clsid_string[39], key[MAX_PATH], surrogate[MAX_PATH];

    _snwprintf(clsid_string, ARRAYSIZE(clsid_string), L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               clsid->Data1, clsid->Data2, clsid->Data3, clsid->Data4[0], clsid->Data4[1], clsid->Data4[2],
               clsid->Data4[3], clsid->Data4[4], clsid->Data4[5], clsid->Data4[6], clsid->Data4[7]);
    clsid_string[38] = 0;
    _snwprintf(key, ARRAYSIZE(key), L"Software\\Classes\\CLSID\\%ls", clsid_string);
    if (launch_read_string(key, L"AppID", view, appid, 39))
        return REGDB_E_CLASSNOTREG;

    _snwprintf(key, ARRAYSIZE(key), L"Software\\Classes\\CLSID\\%ls\\LocalServer32", clsid_string);
    if (!launch_read_string(key, NULL, view, command, MAX_PATH) && command[0])
    {
        wcscat(command, L" -Embedding");
        return S_OK;
    }

    _snwprintf(key, ARRAYSIZE(key), L"Software\\Classes\\AppID\\%ls", appid);
    if (launch_read_string(key, L"DllSurrogate", view, surrogate, ARRAYSIZE(surrogate)))
        return REGDB_E_CLASSNOTREG;
    if (surrogate[0])
        wcscpy(command, surrogate);
    else
    {
        if (view == KEY_WOW64_32KEY && sizeof(void *) == 8)
            GetSystemWow64DirectoryW(command, MAX_PATH - 16);
        else
            GetSystemDirectoryW(command, MAX_PATH - 16);
        wcscat(command, L"\\dllhost.exe");
    }
    wcscat(command, L" /Processid:");
    wcscat(command, clsid_string);
    return S_OK;
}

/* The account's password, stored by the configuration tool as the LSA secret SCM:{AppID} */
static BOOL launch_runas_password(const WCHAR *appid, WCHAR *password, DWORD count)
{
    LSA_OBJECT_ATTRIBUTES attributes = { sizeof(attributes) };
    WCHAR name[64];
    LSA_UNICODE_STRING key;
    PLSA_UNICODE_STRING data = NULL;
    LSA_HANDLE policy;
    BOOL done = FALSE;

    _snwprintf(name, ARRAYSIZE(name), L"SCM:%ls", appid);
    key.Buffer = name;
    key.Length = (USHORT)(wcslen(name) * sizeof(WCHAR));
    key.MaximumLength = key.Length + sizeof(WCHAR);
    if (LsaOpenPolicy(NULL, &attributes, POLICY_GET_PRIVATE_INFORMATION, &policy))
        return FALSE;
    if (!LsaRetrievePrivateData(policy, &key, &data) && data && data->Length / sizeof(WCHAR) < count)
    {
        memcpy(password, data->Buffer, data->Length);
        password[data->Length / sizeof(WCHAR)] = 0;
        SecureZeroMemory(data->Buffer, data->Length);
        done = TRUE;
    }
    if (data)
        LsaFreeMemory(data);
    LsaClose(policy);
    return done;
}

/* The token to start the server with, and the desktop it gets */
static HRESULT launch_token(const WCHAR *appid, const WCHAR *runas, HANDLE caller, HANDLE *token, const WCHAR **desktop)
{
    WCHAR account[256], password[256], *user, *domain;
    DWORD session = 0, size;
    BOOL logged_on;

    if (!_wcsicmp(runas, L"Interactive User"))
    {
        /* The user of the caller's session, else of the console */
        GetTokenInformation(caller, TokenSessionId, &session, sizeof(session), &size);
        if (!WTSQueryUserToken(session, token) && !WTSQueryUserToken(WTSGetActiveConsoleSessionId(), token))
            return CO_E_RUNAS_LOGON_FAILURE;
        *desktop = L"winsta0\\default";
        return S_OK;
    }

    lstrcpynW(account, runas, ARRAYSIZE(account));
    if ((user = wcschr(account, '\\')))
    {
        *user++ = 0;
        domain = account;
    }
    else
    {
        user = account;
        domain = L".";
    }
    if (!launch_runas_password(appid, password, ARRAYSIZE(password)))
        return CO_E_RUNAS_LOGON_FAILURE;
    logged_on = LogonUserW(user, domain, password, LOGON32_LOGON_BATCH, LOGON32_PROVIDER_DEFAULT, token);
    SecureZeroMemory(password, sizeof(password));
    if (!logged_on)
    {
        WARN("logon of %ls\\%ls failed, error %lu\n", debugstr_w(domain), debugstr_w(user), GetLastError());
        return CO_E_RUNAS_LOGON_FAILURE;
    }
    /* A window station of its own, not an interactive desktop */
    *desktop = L"";
    return S_OK;
}

HRESULT __cdecl irpcss_launch_server(handle_t handle, const GUID *clsid, DWORD bitness, DWORD *pid)
{
    WCHAR appid[39], key[MAX_PATH], runas[256], command[LAUNCH_COMMAND_MAX];
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION info;
    HANDLE process = NULL, caller = NULL, token = NULL;
    const WCHAR *desktop = NULL;
    void *environment = NULL;
    REGSAM views[2];
    DWORD view_count = 0, i;
    HRESULT hr = REGDB_E_CLASSNOTREG;

    *pid = 0;
    if (!launch_identity_enabled())
        return E_ACCESSDENIED;
    if (I_RpcOpenClientProcess(handle, PROCESS_QUERY_LIMITED_INFORMATION, &process) != RPC_S_OK)
        return E_ACCESSDENIED;
    if (!OpenProcessToken(process, TOKEN_QUERY | TOKEN_DUPLICATE, &caller))
    {
        CloseHandle(process);
        return E_ACCESSDENIED;
    }
    CloseHandle(process);
    if (!launch_caller_allowed(caller))
    {
        CloseHandle(caller);
        return E_ACCESSDENIED;
    }

    if (bitness == 32)
        views[view_count++] = KEY_WOW64_32KEY;
    else if (bitness == 64)
        views[view_count++] = KEY_WOW64_64KEY;
    else
    {
        views[view_count++] = 0;
        if (sizeof(void *) == 8)
            views[view_count++] = KEY_WOW64_32KEY;
    }
    for (i = 0; i < view_count && FAILED(hr); i++)
        hr = launch_command(clsid, views[i], appid, command);
    if (FAILED(hr))
        goto done;

    _snwprintf(key, ARRAYSIZE(key), L"Software\\Classes\\AppID\\%ls", appid);
    if (launch_read_string(key, L"RunAs", views[i - 1], runas, ARRAYSIZE(runas)) || !runas[0])
    {
        /* Without RunAs the caller starts the server itself */
        hr = REGDB_E_CLASSNOTREG;
        goto done;
    }
    hr = launch_token(appid, runas, caller, &token, &desktop);
    if (FAILED(hr))
        goto done;

    TRACE("starting %s as %s\n", debugstr_w(command), debugstr_w(runas));
    CreateEnvironmentBlock(&environment, token, FALSE);
    startup.lpDesktop = (WCHAR *)desktop;
    if (!CreateProcessAsUserW(token, NULL, command, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS,
                              environment, NULL, &startup, &info))
    {
        WARN("starting %s failed, error %lu\n", debugstr_w(command), GetLastError());
        hr = CO_E_SERVER_EXEC_FAILURE;
        goto done;
    }
    *pid = info.dwProcessId;
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    hr = S_OK;

done:
    if (environment)
        DestroyEnvironmentBlock(environment);
    if (token)
        CloseHandle(token);
    CloseHandle(caller);
    return hr;
}
