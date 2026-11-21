/*
 * PROJECT:     Orosi Wine Mono mscoree shim
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Forward to the real .NET 4 CLR when one is installed
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Windows' System32\mscoree.dll is a thin OS shim that loads the
 * installed .NET 4 mscoreei.dll and forwards the public entry points to
 * it. Mirror that here: when <InstallRoot>\v4.0.30319\mscoreei.dll
 * exists, each hooked entry point forwards to the same-named export in
 * that module. When it does not exist, the existing Mono code paths run
 * unchanged - Mono stays the fallback for systems without the real CLR.
 *
 * The InstallRoot value comes from
 * HKLM\SOFTWARE\Microsoft\.NETFramework\InstallRoot (the same source as
 * metahost.c get_install_root). The read is bounds-checked: only
 * REG_SZ/REG_EXPAND_SZ are accepted, the value is forced to be
 * NUL-terminated inside the buffer (a value stored without the
 * terminator must not be read past its end), environment strings are
 * expanded, and the trailing backslash is added only when missing.
 *
 * The presence check runs once and is cached; a benign race on first
 * call is harmless because the load is reference counted. Forwarding
 * happens only when the requested export actually exists in mscoreei,
 * so unknown export names safely fall through to the Mono path.
 * mscoreei is loaded with LOAD_WITH_ALTERED_SEARCH_PATH so its imports
 * resolve from the Framework directory, never from the host exe's
 * folder (no DLL-planting channel via the current directory chain).
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winnls.h"
#include "winreg.h"
#include "ole2.h"
#include "ocidl.h"
#include "shellapi.h"
#include "strongname.h"

#include "initguid.h"
#include "corerror.h"
#include "cor.h"
#include "corhdr.h"
#include "cordebug.h"
#include "metahost.h"
#include "wine/list.h"

#include "mscoree_private.h"

static HMODULE real_mscoreei;
static LONG real_state; /* 0 unknown, >0 present, <0 absent */

static HMODULE mscoree_load_real_mscoreei(void)
{
    static const WCHAR dotnet_key[] = {'S','O','F','T','W','A','R','E','\\',
        'M','i','c','r','o','s','o','f','t','\\','.','N','E','T','F','r',
        'a','m','e','w','o','r','k',0};
    static const WCHAR install_root[] = {'I','n','s','t','a','l','l','R','o','o','t',0};
    static const WCHAR v4_shim[] = {'v','4','.','0','.','3','0','3','1','9','\\',
        'm','s','c','o','r','e','e','i','.','d','l','l',0};
    WCHAR raw[MAX_PATH];
    WCHAR path[MAX_PATH];
    DWORD len = sizeof(raw);
    DWORD type = 0;
    DWORD count;
    WCHAR *end;
    HKEY key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, dotnet_key, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return NULL;

    if (RegQueryValueExW(key, install_root, NULL, &type, (LPBYTE)raw, &len) != ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return NULL;
    }
    RegCloseKey(key);

    /* Only string types are usable, and the value must be terminated
     * inside the buffer: RegQueryValueExW returns the stored byte count,
     * which for a value written without the NUL does not include one. */
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || len < sizeof(WCHAR))
        return NULL;
    if (len > sizeof(raw))
        len = sizeof(raw);
    raw[len / sizeof(WCHAR) - 1] = L'\0';

    if (type == REG_EXPAND_SZ)
    {
        count = ExpandEnvironmentStringsW(raw, path, MAX_PATH);
        if (count == 0 || count > MAX_PATH)
            return NULL;
    }
    else
    {
        if (lstrlenW(raw) >= MAX_PATH)
            return NULL;
        lstrcpyW(path, raw);
    }

    if (!path[0])
        return NULL;

    /* InstallRoot normally ends in a backslash; cope when it does not. */
    end = path + lstrlenW(path);
    if (end[-1] != L'\\')
    {
        if (end + 1 >= path + MAX_PATH)
            return NULL;
        end[0] = L'\\';
        end[1] = L'\0';
    }

    if (lstrlenW(path) + (sizeof(v4_shim) / sizeof(WCHAR)) >= MAX_PATH)
        return NULL;
    lstrcatW(path, v4_shim);

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return NULL;

    return LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

FARPROC WINAPI mscoree_get_real_export(LPCSTR name)
{
    if (InterlockedCompareExchange(&real_state, 0, 0) == 0)
    {
        HMODULE mod = mscoree_load_real_mscoreei();
        if (mod)
        {
            real_mscoreei = mod;
            InterlockedCompareExchange(&real_state, 1, 0);
        }
        else
            InterlockedCompareExchange(&real_state, -1, 0);
    }

    if (real_state > 0)
        return GetProcAddress(real_mscoreei, name);

    return NULL;
}
