/*
 * Event tracing API
 *
 * Copyright 1995 Sven Verdoolaege
 * Copyright 1998 Juergen Schmied
 * Copyright 2003 Mike Hearn
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "wmistr.h"
#define _WMI_SOURCE_
#include "evntrace.h"
#ifdef __REACTOS__
#include "winternl.h"
#include <ntwmi.h>
#endif

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(eventlog);

#ifndef __REACTOS__
/******************************************************************************
 *     ControlTraceA   (sechost.@)
 */
ULONG WINAPI ControlTraceA( TRACEHANDLE handle, const char *session,
                            EVENT_TRACE_PROPERTIES *properties, ULONG control )
{
    FIXME("(%s, %s, %p, %ld) stub\n", wine_dbgstr_longlong(handle), debugstr_a(session), properties, control);
    return ERROR_SUCCESS;
}

/******************************************************************************
 *     ControlTraceW   (sechost.@)
 */
ULONG WINAPI ControlTraceW( TRACEHANDLE handle, const WCHAR *session,
                            EVENT_TRACE_PROPERTIES *properties, ULONG control )
{
    FIXME("(%s, %s, %p, %ld) stub\n", wine_dbgstr_longlong(handle), debugstr_w(session), properties, control);
    return ERROR_SUCCESS;
}

#endif

#ifndef __REACTOS__
/******************************************************************************
 *     QueryAllTracesA   (sechost.@)
 */
ULONG WINAPI QueryAllTracesA( EVENT_TRACE_PROPERTIES **properties, ULONG count, ULONG *ret_count )
{
    FIXME("(%p, %ld, %p) stub\n", properties, count, ret_count);

    if (ret_count) *ret_count = 0;
    return ERROR_SUCCESS;
}

/******************************************************************************
 *     QueryAllTracesW   (sechost.@)
 */
ULONG WINAPI QueryAllTracesW( EVENT_TRACE_PROPERTIES **properties, ULONG count, ULONG *ret_count )
{
    FIXME("(%p, %ld, %p) stub\n", properties, count, ret_count);

    if (ret_count) *ret_count = 0;
    return ERROR_SUCCESS;
}

/******************************************************************************
 *     StartTraceA   (sechost.@)
 */
ULONG WINAPI StartTraceA( TRACEHANDLE *handle, const char *session, EVENT_TRACE_PROPERTIES *properties )
{
    FIXME("(%p, %s, %p) stub\n", handle, debugstr_a(session), properties);
    if (handle) *handle = 0xcafe4242;
    return ERROR_SUCCESS;
}

/******************************************************************************
 *     StartTraceW   (sechost.@)
 */
ULONG WINAPI StartTraceW( TRACEHANDLE *handle, const WCHAR *session, EVENT_TRACE_PROPERTIES *properties )
{
    FIXME("(%p, %s, %p) stub\n", handle, debugstr_w(session), properties);
    if (handle) *handle = 0xcafe4242;
    return ERROR_SUCCESS;
}

/******************************************************************************
 *     StopTraceW   (sechost.@)
 */
ULONG WINAPI StopTraceW( TRACEHANDLE handle, const WCHAR *session, EVENT_TRACE_PROPERTIES *properties )
{
    FIXME("(%s, %s, %p) stub\n", wine_dbgstr_longlong(handle), debugstr_w(session), properties);
    return ERROR_SUCCESS;
}

#endif

#ifdef __REACTOS__
/******************************************************************************
 *     EnumerateTraceGuidsEx   (sechost.@)
 */
ULONG WINAPI EnumerateTraceGuidsEx( TRACE_QUERY_INFO_CLASS class, void *in, ULONG in_size,
                                    void *out, ULONG out_size, ULONG *ret_size )
{
    ULONG flags = ETW_ENUM_GUIDS_CURRENT_PROCESS;
    ULONG size = 0;
    NTSTATUS status;

    TRACE("%d %p %lu %p %lu %p\n", class, in, in_size, out, out_size, ret_size);

    if (!ret_size) return ERROR_INVALID_PARAMETER;

    switch (class)
    {
    case TraceGuidQueryList:
        if (in || in_size) return ERROR_INVALID_PARAMETER;
        status = NtTraceControl( EtwEnumTraceGuidListCode, NULL, 0, out, out_size, &size );
        break;

    case TraceGuidQueryProcess:
        if (in || in_size) return ERROR_INVALID_PARAMETER;
        status = NtTraceControl( EtwEnumTraceGuidListCode, &flags, sizeof(flags), out, out_size, &size );
        break;

    case TraceGuidQueryInfo:
        if (!in || in_size != sizeof(GUID)) return ERROR_INVALID_PARAMETER;
        status = NtTraceControl( EtwGetTraceGuidInfoCode, in, in_size, out, out_size, &size );
        break;

    default:
        FIXME("unsupported class %d\n", class);
        return ERROR_NOT_SUPPORTED;
    }

    *ret_size = size;
    return RtlNtStatusToDosError( status );
}
#endif

ULONG WINAPI SetTraceCallback( LPCGUID Guid, PEVENT_CALLBACK EventCallback )
{
    return ERROR_NOT_SUPPORTED;
}

ULONG WINAPI RemoveTraceCallback( LPCGUID Guid )
{
    return ERROR_NOT_SUPPORTED;
}
