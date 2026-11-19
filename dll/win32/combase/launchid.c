/*
 * PROJECT:     LiberNT COM base
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Launch identity: the server's bitness, and starting it as its AppID's RunAs account
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winreg.h"

#include "wine/debug.h"

#include "combase_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(ole);

BOOL com_launch_identity_enabled(void)
{
    static LONG enabled = -1;

    if (enabled < 0)
    {
        DWORD value = 0, size = sizeof(value);
        LONG on = 0;

        if (!RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\LiberNT\\COM", L"LaunchIdentity", RRF_RT_REG_DWORD,
                          NULL, &value, &size) && value == 1)
            on = 1;
        InterlockedCompareExchange(&enabled, on, -1);
    }
    return enabled == 1;
}

/*
 * The bitness the server must have: 0 if any, else 32 or 64. The caller's
 * CLSCTX_ACTIVATE_32/64_BIT_SERVER comes first, then the AppID's PreferredServerBitness
 * (1 as the client, 2 32-bit, 3 64-bit). Without either the old order stays: the client's
 * own bitness first, then the other one.
 */
DWORD launch_required_bitness(REFCLSID clsid, DWORD clsctx)
{
    DWORD value = 0, size = sizeof(value);
    BOOL want32, want64;
    HKEY key;

    if (!com_launch_identity_enabled())
        return 0;
    want32 = !!(clsctx & CLSCTX_ACTIVATE_32_BIT_SERVER);
    want64 = !!(clsctx & CLSCTX_ACTIVATE_64_BIT_SERVER);
    if (want32 != want64)
        return want32 ? 32 : 64;

    if (FAILED(open_appidkey_from_clsid(clsid, KEY_READ, &key)))
        return 0;
    if (RegGetValueW(key, NULL, L"PreferredServerBitness", RRF_RT_REG_DWORD, NULL, &value, &size))
        value = 0;
    RegCloseKey(key);
    switch (value)
    {
    case 1: return sizeof(void *) * 8;
    case 2: return 32;
    case 3: return 64;
    default: return 0;
    }
}

/* The registry view of that bitness, and whether it is the other one than this process's */
HRESULT launch_bitness_view(DWORD bitness, REGSAM *view, BOOL *opposite)
{
    BOOL wow64 = FALSE;

    IsWow64Process(GetCurrentProcess(), &wow64);
    if (bitness == 64 && sizeof(void *) == 4 && !wow64)
        return REGDB_E_CLASSNOTREG;
    *view = bitness == 32 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;
    *opposite = bitness != sizeof(void *) * 8;
    return S_OK;
}

static LONG launch_read_string(const WCHAR *subkey, const WCHAR *name, REGSAM view, WCHAR *value, DWORD size)
{
    HKEY key;
    LONG ret;

    ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ | view, &key);
    if (ret)
        return ret;
    ret = RegGetValueW(key, NULL, name, RRF_RT_REG_SZ, NULL, value, &size);
    RegCloseKey(key);
    return ret;
}

/* Whether the class's AppID names a RunAs account; only machine-wide registrations count */
static BOOL launch_has_runas(REFCLSID clsid, DWORD bitness)
{
    WCHAR clsid_string[CHARS_IN_GUID], appid[CHARS_IN_GUID], key[MAX_PATH], runas[256];
    REGSAM view = 0;
    BOOL opposite;

    if (bitness && FAILED(launch_bitness_view(bitness, &view, &opposite)))
        return FALSE;
    StringFromGUID2(clsid, clsid_string, ARRAY_SIZE(clsid_string));
    _snwprintf(key, ARRAY_SIZE(key), L"Software\\Classes\\CLSID\\%ls", clsid_string);
    if (launch_read_string(key, L"AppID", view, appid, sizeof(appid)))
        return FALSE;
    _snwprintf(key, ARRAY_SIZE(key), L"Software\\Classes\\AppID\\%ls", appid);
    return !launch_read_string(key, L"RunAs", view, runas, sizeof(runas)) && runas[0];
}

/*
 * Starts the server through rpcss when its AppID has RunAs: S_FALSE if it has none (the caller
 * starts it itself), else rpcss's answer. A RunAs server is never started as the caller.
 */
HRESULT launch_runas_server(REFCLSID clsid, DWORD bitness, HANDLE *process)
{
    DWORD pid = 0;
    HRESULT hr;

    if (!com_launch_identity_enabled() || !launch_has_runas(clsid, bitness))
        return S_FALSE;
    hr = rpcss_launch_server(clsid, bitness, &pid);
    TRACE("%s: rpcss started pid %lu, hr %#lx\n", debugstr_guid(clsid), pid, hr);
    if (FAILED(hr))
        return hr;
    /* Another user's process may not be ours to wait on; then the registration is polled */
    *process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    return S_OK;
}
