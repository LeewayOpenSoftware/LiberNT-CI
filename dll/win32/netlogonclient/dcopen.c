/*
 * PROJECT:     LiberNT Netlogon client
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later.html)
 * PURPOSE:     Domain controller enumeration context entry points
 * COPYRIGHT:   Copyright 2001 Mike McCormack
 * COPYRIGHT:   Copyright 2003 Juan Lang
 */

#include <stdarg.h>

#include <windef.h>
#include <winbase.h>
#include <dsgetdc.h>

#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(logoncli);

DWORD WINAPI DsGetDcOpenA(LPCSTR domain, ULONG flags, LPCSTR site,
    GUID *domain_guid, LPCSTR forest, ULONG dc_flags, PHANDLE context)
{
    FIXME("(%s, %08lx, %s, %s, %s, %08lx, %p)\n", debugstr_a(domain),
        flags, debugstr_a(site), wine_dbgstr_guid(domain_guid),
        debugstr_a(forest), dc_flags, context);

    *context = NULL;

    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI DsGetDcOpenW(LPCWSTR domain, ULONG flags, LPCWSTR site,
    GUID *domain_guid, LPCWSTR forest, ULONG dc_flags, PHANDLE context)
{
    FIXME("(%s, %08lx, %s, %s, %s, %08lx, %p)\n", debugstr_w(domain),
        flags, debugstr_w(site), wine_dbgstr_guid(domain_guid),
        debugstr_w(forest), dc_flags, context);

    *context = NULL;

    return ERROR_CALL_NOT_IMPLEMENTED;
}
