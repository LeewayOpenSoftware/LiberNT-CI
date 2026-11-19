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
#include <comsec.h>

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

/*
 * Starting code as another account is a privilege boundary of its own, so the AppID's
 * LaunchPermission (else DefaultLaunchPermission, else SYSTEM, Administrators and interactive
 * users) and MachineLaunchRestriction are checked whatever the Enforcement switch says.
 */
static BOOL launch_permitted(const GUID *clsid, const WCHAR *appid, REGSAM view, HANDLE caller, DWORD pid)
{
    PSECURITY_DESCRIPTOR sd = NULL, machine;
    WCHAR subkey[MAX_PATH];
    HANDLE check;
    HKEY key;
    BOOL allowed;

    if (!DuplicateToken(caller, SecurityIdentification, &check))
        return FALSE;
    _snwprintf(subkey, ARRAYSIZE(subkey), L"Software\\Classes\\AppID\\%ls", appid);
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ | view, &key))
    {
        /* A damaged value refuses; only an absent one falls back */
        sd = comsec_read_sd_key(key, L"LaunchPermission");
        RegCloseKey(key);
    }
    if (!sd)
        sd = comsec_read_sd(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Ole", L"DefaultLaunchPermission");
    if (!sd)
        sd = comsec_default_launch_sd();
    machine = comsec_read_sd(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Ole", L"MachineLaunchRestriction");
    allowed = sd && comsec_check(check, machine, COM_RIGHTS_EXECUTE_LOCAL) &&
              comsec_check(check, sd, COM_RIGHTS_EXECUTE_LOCAL);
    if (!allowed)
    {
        WARN("RunAs launch of %s by process %lu refused\n", debugstr_guid(clsid), pid);
        comsec_report_denial(COMSEC_ENFORCE, L"RunAs launch", clsid, check, pid);
    }
    LocalFree(machine);
    LocalFree(sd);
    CloseHandle(check);
    return allowed;
}

/* S2: an AppID read from the registry names a key and an LSA secret: it must look like {GUID} */
static BOOL launch_valid_guid_string(const WCHAR *s)
{
    int i;

    if (wcslen(s) != 38 || s[0] != '{' || s[37] != '}')
        return FALSE;
    for (i = 1; i < 37; i++)
    {
        if (i == 9 || i == 14 || i == 19 || i == 24)
        {
            if (s[i] != '-')
                return FALSE;
        }
        else if (!iswxdigit(s[i]))
            return FALSE;
    }
    return TRUE;
}

/* S1: the image path, so CreateProcess does not search an unquoted path that has spaces */
static BOOL launch_split_command(const WCHAR *command, WCHAR *application, DWORD count)
{
    const WCHAR *end, *p;

    if (command[0] == '"')
    {
        if (!(end = wcschr(command + 1, '"')) || (DWORD)(end - command - 1) >= count)
            return FALSE;
        memcpy(application, command + 1, (end - command - 1) * sizeof(WCHAR));
        application[end - command - 1] = 0;
        return TRUE;
    }
    /* Unquoted: up to the first ".exe" that ends a word */
    for (p = command; (p = wcschr(p, '.')); p++)
    {
        if (!_wcsnicmp(p, L".exe", 4) && (!p[4] || p[4] == ' '))
        {
            end = p + 4;
            if ((DWORD)(end - command) >= count)
                return FALSE;
            memcpy(application, command, (end - command) * sizeof(WCHAR));
            application[end - command] = 0;
            return TRUE;
        }
    }
    return FALSE;
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
    if (launch_read_string(key, L"AppID", view, appid, 39) || !launch_valid_guid_string(appid))
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
    if (!LsaRetrievePrivateData(policy, &key, &data) && data && !(data->Length % sizeof(WCHAR)) &&
        data->Length / sizeof(WCHAR) < count)
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
    WCHAR appid[39], key[MAX_PATH], runas[256], command[LAUNCH_COMMAND_MAX], application[MAX_PATH];
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION info;
    HANDLE process = NULL, caller = NULL, token = NULL;
    ULONG caller_pid = 0;
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
    I_RpcBindingInqLocalClientPID(handle, &caller_pid);

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
    /*
     * rpcss alone decides whether RunAs applies, in the view where it finds the machine-wide
     * class: S_FALSE (no such class, or no RunAs) lets the caller start the server itself.
     */
    if (FAILED(hr))
    {
        hr = S_FALSE;
        goto done;
    }
    _snwprintf(key, ARRAYSIZE(key), L"Software\\Classes\\AppID\\%ls", appid);
    if (launch_read_string(key, L"RunAs", views[i - 1], runas, ARRAYSIZE(runas)) || !runas[0])
    {
        hr = S_FALSE;
        goto done;
    }
    if (!launch_permitted(clsid, appid, views[i - 1], caller, caller_pid))
    {
        hr = E_ACCESSDENIED;
        goto done;
    }
    if (!launch_split_command(command, application, ARRAYSIZE(application)))
    {
        WARN("no image path in %s\n", debugstr_w(command));
        hr = CO_E_SERVER_EXEC_FAILURE;
        goto done;
    }
    hr = launch_token(appid, runas, caller, &token, &desktop);
    if (FAILED(hr))
        goto done;

    TRACE("starting %s as %s\n", debugstr_w(command), debugstr_w(runas));
    CreateEnvironmentBlock(&environment, token, FALSE);
    startup.lpDesktop = (WCHAR *)desktop;
    if (!CreateProcessAsUserW(token, application, command, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS,
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
