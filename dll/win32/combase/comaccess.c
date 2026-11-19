/*
 * PROJECT:     LiberNT COM security
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Access check on incoming COM calls (DCOM step 5)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Which descriptor protects a server process, as documented for CoInitializeSecurity:
 * - an explicit descriptor passed to CoInitializeSecurity;
 * - with EOAC_APPID, the AccessPermission of that AppID (or of the executable's AppID);
 * - an explicit NULL descriptor without EOAC_APPID: no access check;
 * - CoInitializeSecurity not called: the executable's AppID AccessPermission, else the
 *   machine's DefaultAccessPermission, else the server's own user and Local System.
 * The machine-wide limit MachineAccessRestriction applies in every case.
 * Security values are read from HKLM\Software\Classes only, never from a per-user view.
 *
 * Off unless HKLM\Software\LiberNT\COM Enforcement is 1 (audit) or 2 (enforce); read once
 * per process.
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "objbase.h"
#include "rpc.h"

#include "wine/debug.h"

#include "combase_private.h"

#include <comsec.h>

WINE_DEFAULT_DEBUG_CHANNEL(ole);

#ifndef COM_RIGHTS_EXECUTE_LOCAL
#define COM_RIGHTS_EXECUTE_LOCAL 2
#endif

static CRITICAL_SECTION access_cs;
static CRITICAL_SECTION_DEBUG access_cs_debug =
{
    0, 0, &access_cs,
    { &access_cs_debug.ProcessLocksList, &access_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": access_cs") }
};
static CRITICAL_SECTION access_cs = { &access_cs_debug, -1, 0, 0, 0, 0 };

static BOOL access_explicit;            /* CoInitializeSecurity was called */
static BOOL access_explicit_none;       /* ... with a NULL descriptor and no EOAC_APPID */
static BOOL access_explicit_appid;      /* ... with EOAC_APPID and a GUID */
static GUID access_appid;
static PSECURITY_DESCRIPTOR access_explicit_sd;

static BOOL access_resolved;
static PSECURITY_DESCRIPTOR access_sd;  /* NULL after resolution: no per-process check */
static PSECURITY_DESCRIPTOR machine_sd; /* NULL: no machine limit */

DWORD com_access_mode(void)
{
    static LONG mode = -1;

    if (mode < 0)
        InterlockedCompareExchange(&mode, comsec_read_mode(), -1);
    return mode;
}

void com_access_store(PSECURITY_DESCRIPTOR sd, DWORD capabilities)
{
    EnterCriticalSection(&access_cs);
    access_explicit = TRUE;
    if (capabilities & EOAC_APPID)
    {
        if (sd)
        {
            access_appid = *(const GUID *)sd;
            access_explicit_appid = TRUE;
        }
    }
    else if (capabilities & EOAC_ACCESS_CONTROL)
    {
        /* IAccessControl is not supported: the registry decides, as when nothing was passed */
        FIXME("IAccessControl %p is not used for access checks\n", sd);
        access_explicit = FALSE;
    }
    else if (!sd)
        access_explicit_none = TRUE;
    else if (!(access_explicit_sd = comsec_normalize(sd, 0)))
        WARN("invalid security descriptor %p, the registry decides\n", sd);
    LeaveCriticalSection(&access_cs);
}

static BOOL exe_appid(GUID *appid)
{
    WCHAR path[MAX_PATH + 32], value[40], *name;
    DWORD type, size = sizeof(value) - sizeof(WCHAR);
    HKEY key;
    BOOL ret = FALSE;

    if (!GetModuleFileNameW(NULL, path, MAX_PATH))
        return FALSE;
    path[MAX_PATH - 1] = 0;
    name = wcsrchr(path, '\\');
    name = name ? name + 1 : path;
    memmove(path + ARRAY_SIZE(L"Software\\Classes\\AppID\\") - 1, name, (wcslen(name) + 1) * sizeof(WCHAR));
    memcpy(path, L"Software\\Classes\\AppID\\", sizeof(L"Software\\Classes\\AppID\\") - sizeof(WCHAR));
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key))
        return FALSE;
    if (!RegQueryValueExW(key, L"AppID", NULL, &type, (BYTE *)value, &size) && type == REG_SZ)
    {
        value[size / sizeof(WCHAR)] = 0;
        ret = CLSIDFromString(value, appid) == S_OK;
    }
    RegCloseKey(key);
    return ret;
}

static PSECURITY_DESCRIPTOR appid_access_sd(const GUID *appid)
{
    WCHAR path[64];

    lstrcpyW(path, L"Software\\Classes\\AppID\\");
    StringFromGUID2(appid, path + lstrlenW(path), 40);
    return comsec_read_sd(HKEY_LOCAL_MACHINE, path, L"AccessPermission");
}

static PSECURITY_DESCRIPTOR default_access_sd(void)
{
    PSECURITY_DESCRIPTOR sd;
    HANDLE token;
    PSID self;

    if ((sd = comsec_read_sd(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Ole", L"DefaultAccessPermission")))
        return sd;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return NULL;
    self = comsec_token_user(token);
    CloseHandle(token);
    sd = comsec_default_access_sd(self);
    LocalFree(self);
    return sd;
}

/* Returns FALSE when the process descriptor could not be built; the caller refuses then */
static BOOL access_resolve(void)
{
    BOOL ret = TRUE;
    GUID appid;

    EnterCriticalSection(&access_cs);
    if (!access_resolved)
    {
        machine_sd = comsec_read_sd(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Ole", L"MachineAccessRestriction");
        if (access_explicit && access_explicit_none)
            access_sd = NULL;
        else if (access_explicit && access_explicit_sd)
            access_sd = access_explicit_sd;
        else
        {
            if (access_explicit_appid)
                access_sd = appid_access_sd(&access_appid);
            else if (exe_appid(&appid))
                access_sd = appid_access_sd(&appid);
            if (!access_sd)
                access_sd = default_access_sd();
            ret = access_sd != NULL;
        }
        access_resolved = ret;
    }
    LeaveCriticalSection(&access_cs);
    return ret;
}

HRESULT com_access_check_call(RPC_BINDING_HANDLE binding)
{
    HANDLE token = NULL;
    DWORD mode;
    ULONG pid = 0;
    BOOL allowed;

    if ((mode = com_access_mode()) == COMSEC_OFF)
        return S_OK;

    if (RpcImpersonateClient(binding) == RPC_S_OK)
    {
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token))
            token = NULL;
        RpcRevertToSelf();
    }
    allowed = access_resolve() && comsec_check(token, machine_sd, COM_RIGHTS_EXECUTE_LOCAL) &&
              comsec_check(token, access_sd, COM_RIGHTS_EXECUTE_LOCAL);
    if (!allowed)
    {
        I_RpcBindingInqLocalClientPID(binding, &pid);
        WARN("call from process %lu %s\n", pid, mode == COMSEC_ENFORCE ? "refused" : "would be refused");
        comsec_report_denial(mode, L"call", NULL, token, pid);
    }
    if (token)
        CloseHandle(token);
    return allowed || mode != COMSEC_ENFORCE ? S_OK : E_ACCESSDENIED;
}
