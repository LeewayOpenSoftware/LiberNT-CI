/*
 * PROJECT:     LiberNT wevtapi
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Windows Event Log API over the classic event log service
 * COPYRIGHT:   Copyright 2012 André Hentschel (original Wine stubs)
 *              Copyright 2026 LeewayOpenSoftware
 *
 * The modern API is layered on the classic event log (advapi32 OpenEventLog,
 * ReadEventLog and friends). Channels are the classic logs registered under
 * HKLM\SYSTEM\CurrentControlSet\Services\EventLog; records are rendered in the
 * Event XML schema (http://schemas.microsoft.com/win/2004/08/events/event) the
 * way classic events appear on Windows. Queries accept the XPath 1.0 subset the
 * event log documents for structured queries (see query.c).
 *
 * Handles are pointers to tagged objects, as in Wine: a handle that was never
 * returned, or one used after EvtClose, is undefined behaviour, as is closing
 * a handle while another thread still uses it.
 */

#include "wevtapi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wevtapi);

static const WCHAR eventlog_keyW[] = L"SYSTEM\\CurrentControlSet\\Services\\EventLog";

/* --------------------------------------------------------------------------
 * Handle objects
 * -------------------------------------------------------------------------- */

void *evt_alloc_object(DWORD type, SIZE_T size)
{
    struct evt_object *obj = heap_alloc_zero(size);
    if (!obj)
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }
    obj->magic = EVT_MAGIC;
    obj->type = type;
    return obj;
}

void *evt_get_object(EVT_HANDLE handle, DWORD type)
{
    struct evt_object *obj = handle;
    if (!obj || obj->magic != EVT_MAGIC || (type != EVT_TYPE_ANY && obj->type != type))
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return obj;
}

WCHAR *evt_strdupW(const WCHAR *src)
{
    WCHAR *dst;
    if (!src) return NULL;
    dst = heap_alloc((lstrlenW(src) + 1) * sizeof(WCHAR));
    if (dst) lstrcpyW(dst, src);
    return dst;
}

/* --------------------------------------------------------------------------
 * Sessions, channels, classic log handles
 * -------------------------------------------------------------------------- */

static const WCHAR *session_server(EVT_HANDLE session, BOOL *ok)
{
    struct evt_session *s;
    *ok = TRUE;
    if (!session) return NULL;
    if (!(s = evt_get_object(session, EVT_TYPE_SESSION)))
    {
        *ok = FALSE;
        return NULL;
    }
    return s->server;
}

static BOOL open_eventlog_key(const WCHAR *server, HKEY *root, HKEY *key)
{
    LONG err;

    *root = NULL;
    if (server && *server)
    {
        err = RegConnectRegistryW(server, HKEY_LOCAL_MACHINE, root);
        if (err)
        {
            SetLastError(err);
            return FALSE;
        }
    }
    err = RegOpenKeyExW(*root ? *root : HKEY_LOCAL_MACHINE, eventlog_keyW, 0, KEY_READ, key);
    if (err)
    {
        if (*root) RegCloseKey(*root);
        *root = NULL;
        SetLastError(err);
        return FALSE;
    }
    return TRUE;
}

static void close_eventlog_key(HKEY root, HKEY key)
{
    RegCloseKey(key);
    if (root) RegCloseKey(root);
}

/* A channel must be a registered classic log: OpenEventLog itself would fall
 * back to Application for an unknown name, which the modern API never does. */
BOOL evt_channel_exists(const WCHAR *server, const WCHAR *channel)
{
    HKEY root, key, sub;
    LONG err;

    if (!channel || !*channel || wcschr(channel, '\\') || wcschr(channel, '/'))
    {
        SetLastError(ERROR_EVT_CHANNEL_NOT_FOUND);
        return FALSE;
    }
    if (!open_eventlog_key(server, &root, &key))
        return FALSE;
    err = RegOpenKeyExW(key, channel, 0, KEY_READ, &sub);
    close_eventlog_key(root, key);
    if (err)
    {
        SetLastError(ERROR_EVT_CHANNEL_NOT_FOUND);
        return FALSE;
    }
    RegCloseKey(sub);
    return TRUE;
}

/* Expanded path of the file backing a local channel (the classic "File" value). */
static BOOL channel_file_path(const WCHAR *channel, WCHAR *path, DWORD count)
{
    WCHAR raw[MAX_PATH], keyname[256];
    DWORD size = sizeof(raw), type;
    LONG err;

    if (lstrlenW(channel) + ARRAY_SIZE(eventlog_keyW) + 1 > ARRAY_SIZE(keyname))
        return FALSE;
    lstrcpyW(keyname, eventlog_keyW);
    lstrcatW(keyname, L"\\");
    lstrcatW(keyname, channel);
    err = RegGetValueW(HKEY_LOCAL_MACHINE, keyname, L"File", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                       &type, raw, &size);
    if (err)
        return FALSE;
    size = ExpandEnvironmentStringsW(raw, path, count);
    return size && size <= count;
}

/* Opens the classic log behind a channel name or a .evt file path. */
HANDLE evt_open_classic(const WCHAR *server, const WCHAR *path, BOOL is_file)
{
    HANDLE log;

    if (!path || !*path)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (is_file)
    {
        log = OpenBackupEventLogW(server, path);
    }
    else
    {
        if (!evt_channel_exists(server, path))
            return NULL;
        log = OpenEventLogW(server, path);
    }
    if (!log && GetLastError() == ERROR_SUCCESS)
        SetLastError(ERROR_EVT_CHANNEL_NOT_FOUND);
    return log;
}

EVT_HANDLE WINAPI EvtOpenSession(EVT_LOGIN_CLASS login_class, void *login, DWORD timeout, DWORD flags)
{
    EVT_RPC_LOGIN *rpc = login;
    struct evt_session *session;

    TRACE("(%u %p %lu %lu)\n", login_class, login, timeout, flags);

    if (login_class != EvtRpcLogin || !rpc || flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if ((rpc->User && *rpc->User) || (rpc->Password && *rpc->Password))
    {
        /* The classic transport authenticates as the caller; explicit
         * credentials would need an IPC$ connection of their own. */
        FIXME("explicit credentials are not supported\n");
        SetLastError(ERROR_NOT_SUPPORTED);
        return NULL;
    }
    if (!(session = evt_alloc_object(EVT_TYPE_SESSION, sizeof(*session))))
        return NULL;
    if (rpc->Server && *rpc->Server && !(session->server = evt_strdupW(rpc->Server)))
    {
        heap_free(session);
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }
    return session;
}

EVT_HANDLE WINAPI EvtOpenLog(EVT_HANDLE session, const WCHAR *path, DWORD flags)
{
    struct evt_log *log;
    const WCHAR *server;
    BOOL ok;

    TRACE("(%p %s %lu)\n", session, debugstr_w(path), flags);

    server = session_server(session, &ok);
    if (!ok) return NULL;
    if (flags != EvtOpenChannelPath && flags != EvtOpenFilePath)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!(log = evt_alloc_object(EVT_TYPE_LOG, sizeof(*log))))
        return NULL;
    log->is_file = (flags == EvtOpenFilePath);
    log->remote = (server != NULL);
    log->path = evt_strdupW(path);
    log->classic = evt_open_classic(server, path, log->is_file);
    if (!log->classic || !log->path)
    {
        DWORD err = log->path ? GetLastError() : ERROR_OUTOFMEMORY;
        if (log->classic) CloseEventLog(log->classic);
        heap_free(log->path);
        heap_free(log);
        SetLastError(err);
        return NULL;
    }
    return log;
}

static BOOL put_uint64(EVT_VARIANT *v, DWORD size, DWORD *used, ULONGLONG value, DWORD type)
{
    *used = sizeof(EVT_VARIANT);
    if (size < sizeof(EVT_VARIANT) || !v)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(v, 0, sizeof(*v));
    v->Type = type;
    v->Count = 0;
    switch (type)
    {
    case EvtVarTypeBoolean: v->BooleanVal = (BOOL)value; break;
    case EvtVarTypeUInt32:  v->UInt32Val = (UINT32)value; break;
    default:                v->UInt64Val = value; break;
    }
    return TRUE;
}

BOOL WINAPI EvtGetLogInfo(EVT_HANDLE handle, EVT_LOG_PROPERTY_ID id, DWORD size, EVT_VARIANT *buffer, DWORD *used)
{
    struct evt_log *log;
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    WCHAR file[MAX_PATH];
    BOOL have_attrs = FALSE;
    DWORD value;

    TRACE("(%p %u %lu %p %p)\n", handle, id, size, buffer, used);

    if (!used)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!(log = evt_get_object(handle, EVT_TYPE_LOG)))
        return FALSE;

    if (id <= EvtLogAttributes)
    {
        if (log->is_file)
            have_attrs = GetFileAttributesExW(log->path, GetFileExInfoStandard, &attrs);
        else if (!log->remote && channel_file_path(log->path, file, ARRAY_SIZE(file)))
            have_attrs = GetFileAttributesExW(file, GetFileExInfoStandard, &attrs);
        if (!have_attrs)
        {
            /* Remote channels or a file the caller cannot see: no value */
            *used = sizeof(EVT_VARIANT);
            if (size < sizeof(EVT_VARIANT) || !buffer)
            {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return FALSE;
            }
            memset(buffer, 0, sizeof(*buffer));
            buffer->Type = EvtVarTypeNull;
            return TRUE;
        }
    }

    switch (id)
    {
    case EvtLogCreationTime:
        return put_uint64(buffer, size, used, ((ULONGLONG)attrs.ftCreationTime.dwHighDateTime << 32) |
                          attrs.ftCreationTime.dwLowDateTime, EvtVarTypeFileTime);
    case EvtLogLastAccessTime:
        return put_uint64(buffer, size, used, ((ULONGLONG)attrs.ftLastAccessTime.dwHighDateTime << 32) |
                          attrs.ftLastAccessTime.dwLowDateTime, EvtVarTypeFileTime);
    case EvtLogLastWriteTime:
        return put_uint64(buffer, size, used, ((ULONGLONG)attrs.ftLastWriteTime.dwHighDateTime << 32) |
                          attrs.ftLastWriteTime.dwLowDateTime, EvtVarTypeFileTime);
    case EvtLogFileSize:
        return put_uint64(buffer, size, used, ((ULONGLONG)attrs.nFileSizeHigh << 32) | attrs.nFileSizeLow,
                          EvtVarTypeUInt64);
    case EvtLogAttributes:
        return put_uint64(buffer, size, used, attrs.dwFileAttributes, EvtVarTypeUInt32);
    case EvtLogNumberOfLogRecords:
        if (!GetNumberOfEventLogRecords(log->classic, &value)) return FALSE;
        return put_uint64(buffer, size, used, value, EvtVarTypeUInt64);
    case EvtLogOldestRecordNumber:
        if (!GetOldestEventLogRecord(log->classic, &value)) return FALSE;
        return put_uint64(buffer, size, used, value, EvtVarTypeUInt64);
    case EvtLogFull:
    {
        EVENTLOG_FULL_INFORMATION full;
        DWORD needed;
        if (!GetEventLogInformation(log->classic, EVENTLOG_FULL_INFO, &full, sizeof(full), &needed))
            return FALSE;
        return put_uint64(buffer, size, used, full.dwFull != 0, EvtVarTypeBoolean);
    }
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
}

/* --------------------------------------------------------------------------
 * Channel enumeration
 * -------------------------------------------------------------------------- */

EVT_HANDLE WINAPI EvtOpenChannelEnum(EVT_HANDLE session, DWORD flags)
{
    struct evt_channel_enum *chenum;
    const WCHAR *server;
    BOOL ok;

    TRACE("(%p %lu)\n", session, flags);

    server = session_server(session, &ok);
    if (!ok) return NULL;
    if (flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!(chenum = evt_alloc_object(EVT_TYPE_CHANNEL_ENUM, sizeof(*chenum))))
        return NULL;
    if (!open_eventlog_key(server, &chenum->root, &chenum->key))
    {
        DWORD err = GetLastError();
        heap_free(chenum);
        SetLastError(err);
        return NULL;
    }
    return chenum;
}

BOOL WINAPI EvtNextChannelPath(EVT_HANDLE handle, DWORD buffer_len, WCHAR *buffer, DWORD *used)
{
    struct evt_channel_enum *chenum;
    WCHAR name[256];
    DWORD len = ARRAY_SIZE(name);
    LONG err;

    TRACE("(%p %lu %p %p)\n", handle, buffer_len, buffer, used);

    if (!used || (buffer_len && !buffer))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!(chenum = evt_get_object(handle, EVT_TYPE_CHANNEL_ENUM)))
        return FALSE;

    err = RegEnumKeyExW(chenum->key, chenum->index, name, &len, NULL, NULL, NULL, NULL);
    if (err)
    {
        SetLastError(err == ERROR_NO_MORE_ITEMS ? ERROR_NO_MORE_ITEMS : err);
        return FALSE;
    }
    *used = len + 1;
    if (buffer_len < len + 1)
    {
        /* The entry is kept: the caller retries with a larger buffer */
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(buffer, name, (len + 1) * sizeof(WCHAR));
    chenum->index++;
    return TRUE;
}

/* --------------------------------------------------------------------------
 * Clear and export
 * -------------------------------------------------------------------------- */

BOOL WINAPI EvtClearLog(EVT_HANDLE session, const WCHAR *channel, const WCHAR *target, DWORD flags)
{
    const WCHAR *server;
    HANDLE log;
    BOOL ok, ret;

    TRACE("(%p %s %s %#lx)\n", session, debugstr_w(channel), debugstr_w(target), flags);

    server = session_server(session, &ok);
    if (!ok) return FALSE;
    if (flags || !channel)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!(log = evt_open_classic(server, channel, FALSE)))
        return FALSE;
    ret = ClearEventLogW(log, target);
    if (!ret)
    {
        DWORD err = GetLastError();
        CloseEventLog(log);
        SetLastError(err);
        return FALSE;
    }
    CloseEventLog(log);
    return TRUE;
}

static BOOL query_is_everything(const WCHAR *query)
{
    if (!query) return TRUE;
    while (*query == ' ' || *query == '\t') query++;
    if (!*query) return TRUE;
    if (*query != '*') return FALSE;
    query++;
    while (*query == ' ' || *query == '\t') query++;
    return !*query;
}

BOOL WINAPI EvtExportLog(EVT_HANDLE session, const WCHAR *path, const WCHAR *query, const WCHAR *file, DWORD flags)
{
    const WCHAR *server;
    DWORD source = flags & (EvtExportLogChannelPath | EvtExportLogFilePath);
    HANDLE log;
    BOOL ok;

    TRACE("(%p %s %s %s %#lx)\n", session, debugstr_w(path), debugstr_w(query), debugstr_w(file), flags);

    server = session_server(session, &ok);
    if (!ok) return FALSE;
    if (!path || !file || !*file || (source != EvtExportLogChannelPath && source != EvtExportLogFilePath) ||
        (flags & ~(EvtExportLogChannelPath | EvtExportLogFilePath | EvtExportLogTolerateQueryErrors |
                   EvtExportLogOverwrite)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!query_is_everything(query))
    {
        /* The export file is the classic log format written by the service;
         * a filtered export would need a writer of our own. */
        FIXME("filtered export (%s) is not supported\n", debugstr_w(query));
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES)
    {
        if (!(flags & EvtExportLogOverwrite))
        {
            SetLastError(ERROR_FILE_EXISTS);
            return FALSE;
        }
        if (!DeleteFileW(file))
            return FALSE;
    }
    if (source == EvtExportLogFilePath)
        return CopyFileW(path, file, TRUE);

    if (!(log = evt_open_classic(server, path, FALSE)))
        return FALSE;
    ok = BackupEventLogW(log, file);
    if (!ok)
    {
        DWORD err = GetLastError();
        CloseEventLog(log);
        SetLastError(err);
        return FALSE;
    }
    CloseEventLog(log);
    return TRUE;
}

/* Returns a Win32 error code, not a BOOL */
DWORD WINAPI EvtGetExtendedStatus(DWORD buffer_size, WCHAR *buffer, DWORD *used)
{
    TRACE("(%lu %p %p)\n", buffer_size, buffer, used);

    if (!used || !buffer)
        return ERROR_INVALID_PARAMETER;
    /* No call of this implementation leaves extended status behind */
    *used = 0;
    if (buffer_size)
        buffer[0] = 0;
    return ERROR_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Channel configuration (classic log settings)
 * -------------------------------------------------------------------------- */

EVT_HANDLE WINAPI EvtOpenChannelConfig(EVT_HANDLE session, LPCWSTR channel, DWORD flags)
{
    struct evt_channel_config *config;
    const WCHAR *server;
    WCHAR keyname[256];
    DWORD value, size;
    BOOL ok;

    TRACE("(%p %s %lu)\n", session, debugstr_w(channel), flags);

    server = session_server(session, &ok);
    if (!ok) return NULL;
    if (flags || !channel)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (server)
    {
        FIXME("remote channel configuration is not supported\n");
        SetLastError(ERROR_NOT_SUPPORTED);
        return NULL;
    }
    if (!evt_channel_exists(NULL, channel))
        return NULL;
    if (lstrlenW(channel) + ARRAY_SIZE(eventlog_keyW) + 1 > ARRAY_SIZE(keyname))
    {
        SetLastError(ERROR_EVT_CHANNEL_NOT_FOUND);
        return NULL;
    }
    if (!(config = evt_alloc_object(EVT_TYPE_CHANNEL_CONFIG, sizeof(*config))))
        return NULL;
    config->name = evt_strdupW(channel);
    lstrcpyW(keyname, eventlog_keyW);
    lstrcatW(keyname, L"\\");
    lstrcatW(keyname, channel);
    config->keyname = evt_strdupW(keyname);
    if (!config->name || !config->keyname)
    {
        heap_free(config->name);
        heap_free(config->keyname);
        heap_free(config);
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }
    size = sizeof(value);
    config->max_size = RegGetValueW(HKEY_LOCAL_MACHINE, keyname, L"MaxSize", RRF_RT_REG_DWORD, NULL, &value, &size)
                       ? 0x80000 : value;
    size = sizeof(value);
    config->retention = RegGetValueW(HKEY_LOCAL_MACHINE, keyname, L"Retention", RRF_RT_REG_DWORD, NULL, &value, &size)
                        ? 0 : value;
    return config;
}

static BOOL put_string(EVT_VARIANT *v, DWORD size, DWORD *used, const WCHAR *str)
{
    DWORD need = sizeof(EVT_VARIANT) + (lstrlenW(str) + 1) * sizeof(WCHAR);

    *used = need;
    if (size < need || !v)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(v, 0, sizeof(*v));
    v->Type = EvtVarTypeString;
    v->StringVal = (WCHAR *)(v + 1);
    lstrcpyW((WCHAR *)(v + 1), str);
    return TRUE;
}

BOOL WINAPI EvtGetChannelConfigProperty(EVT_HANDLE handle, EVT_CHANNEL_CONFIG_PROPERTY_ID id, DWORD flags,
                                        DWORD size, PEVT_VARIANT buffer, DWORD *used)
{
    struct evt_channel_config *config;
    WCHAR file[MAX_PATH];

    TRACE("(%p %i %lu %lu %p %p)\n", handle, id, flags, size, buffer, used);

    if (!used || flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!(config = evt_get_object(handle, EVT_TYPE_CHANNEL_CONFIG)))
        return FALSE;

    switch (id)
    {
    case EvtChannelConfigEnabled:
    case EvtChannelConfigClassicEventlog:
        return put_uint64(buffer, size, used, TRUE, EvtVarTypeBoolean);
    case EvtChannelConfigType:
        return put_uint64(buffer, size, used, 0 /* EvtChannelTypeAdmin */, EvtVarTypeUInt32);
    case EvtChannelLoggingConfigRetention:
        /* Classic Retention: 0 = overwrite as needed; anything else keeps records */
        return put_uint64(buffer, size, used, config->retention != 0, EvtVarTypeBoolean);
    case EvtChannelLoggingConfigAutoBackup:
        return put_uint64(buffer, size, used, FALSE, EvtVarTypeBoolean);
    case EvtChannelLoggingConfigMaxSize:
        return put_uint64(buffer, size, used, config->max_size, EvtVarTypeUInt64);
    case EvtChannelLoggingConfigLogFilePath:
        if (!channel_file_path(config->name, file, ARRAY_SIZE(file)))
            file[0] = 0;
        return put_string(buffer, size, used, file);
    default:
        FIXME("property %u is not supported\n", id);
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
}

BOOL WINAPI EvtSetChannelConfigProperty(EVT_HANDLE handle, EVT_CHANNEL_CONFIG_PROPERTY_ID id, DWORD flags,
                                        PEVT_VARIANT value)
{
    struct evt_channel_config *config;

    TRACE("(%p %i %lu %p)\n", handle, id, flags, value);

    if (!value || flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!(config = evt_get_object(handle, EVT_TYPE_CHANNEL_CONFIG)))
        return FALSE;

    switch (id)
    {
    case EvtChannelLoggingConfigMaxSize:
        if (value->Type != EvtVarTypeUInt64 || value->UInt64Val < 0x10000 || value->UInt64Val > 0xFFFFFFFF)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        config->max_size = (DWORD)value->UInt64Val;
        config->dirty = TRUE;
        return TRUE;
    case EvtChannelLoggingConfigRetention:
        if (value->Type != EvtVarTypeBoolean)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        config->retention = value->BooleanVal ? 0xFFFFFFFF : 0;
        config->dirty = TRUE;
        return TRUE;
    default:
        FIXME("property %u is not supported\n", id);
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
}

BOOL WINAPI EvtSaveChannelConfig(EVT_HANDLE handle, DWORD flags)
{
    struct evt_channel_config *config;
    HKEY key;
    LONG err;

    TRACE("(%p,%08lx)\n", handle, flags);

    if (flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!(config = evt_get_object(handle, EVT_TYPE_CHANNEL_CONFIG)))
        return FALSE;
    if (!config->dirty)
        return TRUE;

    err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, config->keyname, 0, KEY_SET_VALUE, &key);
    if (!err)
    {
        err = RegSetValueExW(key, L"MaxSize", 0, REG_DWORD, (BYTE *)&config->max_size, sizeof(DWORD));
        if (!err)
            err = RegSetValueExW(key, L"Retention", 0, REG_DWORD, (BYTE *)&config->retention, sizeof(DWORD));
        RegCloseKey(key);
    }
    if (err)
    {
        SetLastError(err);
        return FALSE;
    }
    config->dirty = FALSE;
    return TRUE;
}

/* --------------------------------------------------------------------------
 * Close and cancel
 * -------------------------------------------------------------------------- */

BOOL WINAPI EvtClose(EVT_HANDLE handle)
{
    struct evt_object *obj;

    TRACE("(%p)\n", handle);

    if (!(obj = evt_get_object(handle, EVT_TYPE_ANY)))
        return FALSE;

    switch (obj->type)
    {
    case EVT_TYPE_SESSION:
        heap_free(((struct evt_session *)obj)->server);
        break;
    case EVT_TYPE_LOG:
        CloseEventLog(((struct evt_log *)obj)->classic);
        heap_free(((struct evt_log *)obj)->path);
        break;
    case EVT_TYPE_CHANNEL_ENUM:
        close_eventlog_key(((struct evt_channel_enum *)obj)->root, ((struct evt_channel_enum *)obj)->key);
        break;
    case EVT_TYPE_CHANNEL_CONFIG:
        heap_free(((struct evt_channel_config *)obj)->name);
        heap_free(((struct evt_channel_config *)obj)->keyname);
        break;
    case EVT_TYPE_RESULT_SET:
        evt_free_result_set((struct evt_result_set *)obj);
        break;
    case EVT_TYPE_EVENT:
        evt_free_event((struct evt_event *)obj);
        break;
    case EVT_TYPE_RENDER_CONTEXT:
        evt_free_render_context((struct evt_render_context *)obj);
        break;
    case EVT_TYPE_BOOKMARK:
        heap_free(((struct evt_bookmark *)obj)->channel);
        break;
    case EVT_TYPE_PUBLISHER:
        evt_free_publisher((struct evt_publisher *)obj);
        break;
    case EVT_TYPE_SUBSCRIPTION:
        /* Frees the object itself, possibly later from its own thread */
        return evt_close_subscription((struct evt_subscription *)obj);
    }
    obj->magic = 0;
    heap_free(obj);
    return TRUE;
}

BOOL WINAPI EvtCancel(EVT_HANDLE handle)
{
    struct evt_object *obj;

    TRACE("(%p)\n", handle);

    if (!handle)
        return TRUE;   /* no pending operation on the calling thread */
    if (!(obj = evt_get_object(handle, EVT_TYPE_ANY)))
        return FALSE;
    if (obj->type == EVT_TYPE_RESULT_SET)
    {
        ((struct evt_result_set *)obj)->cancelled = TRUE;
        return TRUE;
    }
    if (obj->type == EVT_TYPE_SUBSCRIPTION)
        return evt_cancel_subscription((struct evt_subscription *)obj);
    return TRUE;
}
