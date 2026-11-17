/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the Windows Event Log API (wevtapi): channels, log
 *              information, queries, rendering, bookmarks, subscriptions, export
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 *
 * The write tests report a few events with the source "WevtapiTest" into the
 * Application log. EvtClearLog is exercised on its error paths only:
 * clearing a real log is not something a test run should do.
 * Building with WEVTAPI_TEST_READONLY leaves out everything that writes.
 */

#include <apitest.h>
#include <windows.h>
#include <winevt.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_SOURCE L"WevtapiTest"
#define TEST_EVENTID(id) (0x40000000 | (id))   /* qualifiers 0x4000 */

static WCHAR *render_xml(EVT_HANDLE event)
{
    DWORD used = 0, count = 0;
    WCHAR *xml;
    BOOL ret;

    ret = EvtRender(NULL, event, EvtRenderEventXml, 0, NULL, &used, &count);
    ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "EvtRender(size 0): %d, error %lu\n", ret, GetLastError());
    ok(used > sizeof(WCHAR) && !(used & 1), "used %lu\n", used);
    xml = HeapAlloc(GetProcessHeap(), 0, used);
    if (!xml) return NULL;
    ret = EvtRender(NULL, event, EvtRenderEventXml, used, xml, &used, &count);
    ok(ret, "EvtRender(xml) failed: %lu\n", GetLastError());
    ok(count == 1, "property count %lu\n", count);
    if (!ret)
    {
        HeapFree(GetProcessHeap(), 0, xml);
        return NULL;
    }
    return xml;
}

static void check_contains(const WCHAR *xml, const WCHAR *part, int line)
{
    ok_(__FILE__, line)(xml && wcsstr(xml, part) != NULL, "%s not in %s\n", wine_dbgstr_w(part), wine_dbgstr_w(xml));
}
#define check_contains(x, p) check_contains(x, p, __LINE__)

static void test_channels(void)
{
    EVT_HANDLE chenum;
    WCHAR name[256], small[1];
    DWORD used;
    BOOL ret, application = FALSE, system = FALSE;
    int n = 0;

    SetLastError(0xdeadbeef);
    chenum = EvtOpenChannelEnum(NULL, 1);
    ok(!chenum && GetLastError() == ERROR_INVALID_PARAMETER, "flags 1: %p, error %lu\n", chenum, GetLastError());

    chenum = EvtOpenChannelEnum(NULL, 0);
    ok(chenum != NULL, "EvtOpenChannelEnum failed: %lu\n", GetLastError());
    if (!chenum) return;

    /* A short buffer reports the size and keeps the entry */
    used = 0;
    ret = EvtNextChannelPath(chenum, _countof(small), small, &used);
    ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "short buffer: %d, error %lu\n", ret, GetLastError());
    ok(used > 1, "used %lu\n", used);

    for (;;)
    {
        ret = EvtNextChannelPath(chenum, _countof(name), name, &used);
        if (!ret)
        {
            ok(GetLastError() == ERROR_NO_MORE_ITEMS, "end of enumeration: error %lu\n", GetLastError());
            break;
        }
        ok(used == (DWORD)lstrlenW(name) + 1, "used %lu for %s\n", used, wine_dbgstr_w(name));
        if (!lstrcmpiW(name, L"Application")) application = TRUE;
        if (!lstrcmpiW(name, L"System")) system = TRUE;
        n++;
    }
    ok(application && system, "Application %d, System %d among %d channels\n", application, system, n);
    ok(EvtClose(chenum), "EvtClose failed: %lu\n", GetLastError());
}

static void test_log_info(void)
{
    EVT_VARIANT value[16];
    EVT_HANDLE log;
    DWORD used;
    BOOL ret;

    SetLastError(0xdeadbeef);
    log = EvtOpenLog(NULL, L"WevtapiNoSuchChannel", EvtOpenChannelPath);
    ok(!log && GetLastError() == ERROR_EVT_CHANNEL_NOT_FOUND, "unknown channel: %p, error %lu\n", log, GetLastError());
    SetLastError(0xdeadbeef);
    log = EvtOpenLog(NULL, L"Application", 0);
    ok(!log && GetLastError() == ERROR_INVALID_PARAMETER, "flags 0: %p, error %lu\n", log, GetLastError());

    log = EvtOpenLog(NULL, L"Application", EvtOpenChannelPath);
    ok(log != NULL, "EvtOpenLog(Application) failed: %lu\n", GetLastError());
    if (!log) return;

    used = 0;
    ret = EvtGetLogInfo(log, EvtLogNumberOfLogRecords, 0, NULL, &used);
    ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "size 0: %d, error %lu\n", ret, GetLastError());
    ok(used == sizeof(EVT_VARIANT), "used %lu\n", used);

    ret = EvtGetLogInfo(log, EvtLogNumberOfLogRecords, sizeof(value), value, &used);
    ok(ret, "EvtLogNumberOfLogRecords failed: %lu\n", GetLastError());
    ok(value[0].Type == EvtVarTypeUInt64, "records type %lu\n", value[0].Type);
    ret = EvtGetLogInfo(log, EvtLogOldestRecordNumber, sizeof(value), value, &used);
    ok(ret && value[0].Type == EvtVarTypeUInt64, "oldest: %d, type %lu\n", ret, value[0].Type);
    ret = EvtGetLogInfo(log, EvtLogFull, sizeof(value), value, &used);
    ok(ret && value[0].Type == EvtVarTypeBoolean, "full: %d, type %lu\n", ret, value[0].Type);
    ret = EvtGetLogInfo(log, EvtLogFileSize, sizeof(value), value, &used);
    ok(ret && (value[0].Type == EvtVarTypeUInt64 || value[0].Type == EvtVarTypeNull), "file size: %d, type %lu\n",
       ret, value[0].Type);
    ret = EvtGetLogInfo(log, EvtLogLastWriteTime, sizeof(value), value, &used);
    ok(ret && (value[0].Type == EvtVarTypeFileTime || value[0].Type == EvtVarTypeNull), "write time: %d, type %lu\n",
       ret, value[0].Type);
    ok(EvtClose(log), "EvtClose failed: %lu\n", GetLastError());
}

static void test_invalid(void)
{
    WCHAR buffer[4];
    EVT_HANDLE query;
    DWORD used, status;
    BOOL ret;

    SetLastError(0xdeadbeef);
    query = EvtQuery(NULL, L"Application", L"*[System[EventID=", EvtQueryChannelPath);
    ok(!query && GetLastError() == ERROR_EVT_INVALID_QUERY, "bad query: %p, error %lu\n", query, GetLastError());
    SetLastError(0xdeadbeef);
    query = EvtQuery(NULL, L"Application", L"*", EvtQueryChannelPath | EvtQueryForwardDirection | EvtQueryReverseDirection);
    ok(!query && GetLastError() == ERROR_INVALID_PARAMETER, "both directions: %p, error %lu\n", query, GetLastError());
    SetLastError(0xdeadbeef);
    ret = EvtClose(NULL);
    ok(!ret && GetLastError() == ERROR_INVALID_HANDLE, "EvtClose(NULL): %d, error %lu\n", ret, GetLastError());

    /* Returns an error code; nothing to report after the calls above */
    used = 0xdeadbeef;
    status = EvtGetExtendedStatus(_countof(buffer), buffer, &used);
    ok(status == ERROR_SUCCESS && used == 0, "extended status: %lu, used %lu\n", status, used);
    status = EvtGetExtendedStatus(0, NULL, &used);
    ok(status == ERROR_INVALID_PARAMETER, "extended status without buffer: %lu\n", status);

    SetLastError(0xdeadbeef);
    ret = EvtClearLog(NULL, L"WevtapiNoSuchChannel", NULL, 0);
    ok(!ret && GetLastError() == ERROR_EVT_CHANNEL_NOT_FOUND, "clear unknown: %d, error %lu\n", ret, GetLastError());
}

/* Reads the newest record of Application through a reverse query and checks the shape of its XML */
static void test_query_existing(void)
{
    EVT_HANDLE query, event, bookmark, query2, event2, context;
    EVT_VARIANT *values;
    WCHAR *xml, *bmxml;
    DWORD returned, used, count;
    ULONGLONG recid;
    BOOL ret;

    query = EvtQuery(NULL, L"Application", L"*", EvtQueryChannelPath | EvtQueryReverseDirection);
    ok(query != NULL, "EvtQuery(*) failed: %lu\n", GetLastError());
    if (!query) return;
    ret = EvtNext(query, 1, &event, INFINITE, 0, &returned);
    if (!ret && GetLastError() == ERROR_NO_MORE_ITEMS)
    {
        skip("the Application log is empty\n");
        EvtClose(query);
        return;
    }
    ok(ret && returned == 1, "EvtNext: %d, returned %lu, error %lu\n", ret, returned, GetLastError());
    if (!ret)
    {
        EvtClose(query);
        return;
    }

    xml = render_xml(event);
    check_contains(xml, L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'>");
    check_contains(xml, L"<Channel>Application</Channel>");
    check_contains(xml, L"<EventRecordID>");
    HeapFree(GetProcessHeap(), 0, xml);

    context = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);
    ok(context != NULL, "EvtCreateRenderContext(System) failed: %lu\n", GetLastError());
    ret = EvtRender(context, event, EvtRenderEventValues, 0, NULL, &used, &count);
    ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "values size 0: %d, error %lu\n", ret, GetLastError());
    ok(count == EvtSystemPropertyIdEND, "property count %lu\n", count);
    values = HeapAlloc(GetProcessHeap(), 0, used);
    ret = EvtRender(context, event, EvtRenderEventValues, used, values, &used, &count);
    ok(ret, "EvtRender(values) failed: %lu\n", GetLastError());
    recid = 0;
    if (ret)
    {
        ok(values[EvtSystemEventRecordId].Type == EvtVarTypeUInt64, "record id type %lu\n",
           values[EvtSystemEventRecordId].Type);
        ok(values[EvtSystemTimeCreated].Type == EvtVarTypeFileTime, "time type %lu\n", values[EvtSystemTimeCreated].Type);
        ok(values[EvtSystemChannel].Type == EvtVarTypeString &&
           !lstrcmpiW(values[EvtSystemChannel].StringVal, L"Application"), "channel %lu %s\n",
           values[EvtSystemChannel].Type, wine_dbgstr_w(values[EvtSystemChannel].StringVal));
        ok(values[EvtSystemEventID].Type == EvtVarTypeUInt16, "event id type %lu\n", values[EvtSystemEventID].Type);
        ok(values[EvtSystemLevel].Type == EvtVarTypeByte, "level type %lu\n", values[EvtSystemLevel].Type);
        ok(values[EvtSystemKeywords].Type == EvtVarTypeHexInt64, "keywords type %lu\n", values[EvtSystemKeywords].Type);
        ok(values[EvtSystemChannel].Count == 11, "channel count %lu\n", values[EvtSystemChannel].Count);
        recid = values[EvtSystemEventRecordId].UInt64Val;
    }
    HeapFree(GetProcessHeap(), 0, values);
    EvtClose(context);

    /* Bookmark: update from the event, render, recreate from XML, seek to it */
    bookmark = EvtCreateBookmark(NULL);
    ok(bookmark != NULL, "EvtCreateBookmark(NULL) failed: %lu\n", GetLastError());
    ok(EvtUpdateBookmark(bookmark, event), "EvtUpdateBookmark failed: %lu\n", GetLastError());
    ret = EvtRender(NULL, bookmark, EvtRenderBookmark, 0, NULL, &used, &count);
    ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "bookmark size 0: %d, error %lu\n", ret, GetLastError());
    bmxml = HeapAlloc(GetProcessHeap(), 0, used);
    ret = EvtRender(NULL, bookmark, EvtRenderBookmark, used, bmxml, &used, &count);
    ok(ret, "EvtRender(bookmark) failed: %lu\n", GetLastError());
    check_contains(bmxml, L"<BookmarkList");
    check_contains(bmxml, L"Channel='Application'");
    EvtClose(bookmark);

    bookmark = ret ? EvtCreateBookmark(bmxml) : NULL;
    ok(bookmark != NULL, "EvtCreateBookmark(%s) failed: %lu\n", wine_dbgstr_w(bmxml), GetLastError());
    HeapFree(GetProcessHeap(), 0, bmxml);
    query2 = EvtQuery(NULL, L"Application", NULL, EvtQueryChannelPath);
    ok(query2 != NULL, "EvtQuery(forward) failed: %lu\n", GetLastError());
    if (bookmark && query2)
    {
        ret = EvtSeek(query2, 0, bookmark, 0, EvtSeekRelativeToBookmark);
        ok(ret, "EvtSeek(bookmark) failed: %lu\n", GetLastError());
        ret = EvtNext(query2, 1, &event2, INFINITE, 0, &returned);
        ok(ret && returned == 1, "EvtNext after seek: %d, error %lu\n", ret, GetLastError());
        if (ret)
        {
            context = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);
            values = HeapAlloc(GetProcessHeap(), 0, 4096);
            ret = EvtRender(context, event2, EvtRenderEventValues, 4096, values, &used, &count);
            ok(ret && values[EvtSystemEventRecordId].UInt64Val == recid, "seek landed on %s, expected %s\n",
               wine_dbgstr_longlong(values[EvtSystemEventRecordId].UInt64Val), wine_dbgstr_longlong(recid));
            HeapFree(GetProcessHeap(), 0, values);
            EvtClose(context);
            EvtClose(event2);
        }
    }
    if (bookmark) EvtClose(bookmark);
    if (query2) EvtClose(query2);
    EvtClose(event);
    EvtClose(query);
}

#ifndef WEVTAPI_TEST_READONLY

static BOOL report(DWORD id, WORD type, WORD category, const WCHAR *s1, const WCHAR *s2)
{
    static const BYTE data[] = { 0xde, 0xad };
    const WCHAR *strings[2] = { s1, s2 };
    HANDLE source = RegisterEventSourceW(NULL, TEST_SOURCE);
    BOOL ret;

    ok(source != NULL, "RegisterEventSource failed: %lu\n", GetLastError());
    if (!source) return FALSE;
    ret = ReportEventW(source, type, category, TEST_EVENTID(id), NULL, 2, sizeof(data), strings, (void *)data);
    ok(ret, "ReportEvent(%lu) failed: %lu\n", id, GetLastError());
    DeregisterEventSource(source);
    return ret;
}

/* Newest event matching the query, or NULL */
static EVT_HANDLE newest(const WCHAR *path, const WCHAR *query, DWORD flags)
{
    EVT_HANDLE set, event = NULL;
    DWORD returned;

    set = EvtQuery(NULL, path, query, flags | EvtQueryReverseDirection);
    ok(set != NULL, "EvtQuery(%s) failed: %lu\n", wine_dbgstr_w(query), GetLastError());
    if (!set) return NULL;
    if (!EvtNext(set, 1, &event, INFINITE, 0, &returned))
    {
        ok(0, "no event for %s: error %lu\n", wine_dbgstr_w(query), GetLastError());
        event = NULL;
    }
    EvtClose(set);
    return event;
}

static void test_written_event(void)
{
    static const WCHAR *paths[] = { L"Event/System/EventID", L"Event/EventData/Data[2]",
                                    L"Event/System/Provider/@Name", L"Event/System/NoSuchThing" };
    EVT_HANDLE event, context, session;
    EVT_RPC_LOGIN login = { 0 };
    EVT_VARIANT *values;
    FILETIME now;
    ULONGLONG t;
    WCHAR *xml;
    DWORD used, count;
    BOOL ret;

    if (!report(1234, EVENTLOG_WARNING_TYPE, 3, L"alpha & <beta>", L"gamma"))
        return;

    event = newest(L"Application", L"*[System[(EventID=1234) and Level=3 and Provider[@Name='WevtapiTest']]]",
                   EvtQueryChannelPath);
    if (!event) return;

    xml = render_xml(event);
    check_contains(xml, L"<Provider Name='WevtapiTest'/>");
    check_contains(xml, L"<EventID Qualifiers='16384'>1234</EventID>");
    check_contains(xml, L"<Level>3</Level>");
    check_contains(xml, L"<Task>3</Task>");
    check_contains(xml, L"<Data>alpha &amp; &lt;beta&gt;</Data><Data>gamma</Data>");
    check_contains(xml, L"<Binary>DEAD</Binary>");
    HeapFree(GetProcessHeap(), 0, xml);

    context = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);
    values = HeapAlloc(GetProcessHeap(), 0, 8192);
    ret = EvtRender(context, event, EvtRenderEventValues, 8192, values, &used, &count);
    ok(ret, "EvtRender(system) failed: %lu\n", GetLastError());
    if (ret)
    {
        ok(values[EvtSystemEventID].UInt16Val == 1234, "event id %u\n", values[EvtSystemEventID].UInt16Val);
        ok(values[EvtSystemQualifiers].Type == EvtVarTypeUInt16 && values[EvtSystemQualifiers].UInt16Val == 0x4000,
           "qualifiers %lu %u\n", values[EvtSystemQualifiers].Type, values[EvtSystemQualifiers].UInt16Val);
        ok(values[EvtSystemLevel].Type == EvtVarTypeByte && values[EvtSystemLevel].ByteVal == 3, "level %u\n",
           values[EvtSystemLevel].ByteVal);
        ok(values[EvtSystemTask].UInt16Val == 3, "task %u\n", values[EvtSystemTask].UInt16Val);
        ok(values[EvtSystemProviderName].Type == EvtVarTypeString &&
           !lstrcmpW(values[EvtSystemProviderName].StringVal, TEST_SOURCE), "provider %s\n",
           wine_dbgstr_w(values[EvtSystemProviderName].StringVal));
        GetSystemTimeAsFileTime(&now);
        t = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
        ok(values[EvtSystemTimeCreated].FileTimeVal <= t &&
           t - values[EvtSystemTimeCreated].FileTimeVal < (ULONGLONG)3600 * 10000000, "time created is not recent\n");
    }
    EvtClose(context);

    context = EvtCreateRenderContext(0, NULL, EvtRenderContextUser);
    ret = EvtRender(context, event, EvtRenderEventValues, 8192, values, &used, &count);
    ok(ret && count == 2, "EvtRender(user): %d, count %lu, error %lu\n", ret, count, GetLastError());
    if (ret && count == 2)
    {
        ok(values[0].Type == EvtVarTypeString && !lstrcmpW(values[0].StringVal, L"alpha & <beta>"), "data 1 %s\n",
           wine_dbgstr_w(values[0].StringVal));
        ok(values[1].Type == EvtVarTypeString && !lstrcmpW(values[1].StringVal, L"gamma"), "data 2 %s\n",
           wine_dbgstr_w(values[1].StringVal));
    }
    EvtClose(context);

    context = EvtCreateRenderContext(_countof(paths), paths, EvtRenderContextValues);
    ok(context != NULL, "EvtCreateRenderContext(paths) failed: %lu\n", GetLastError());
    ret = EvtRender(context, event, EvtRenderEventValues, 8192, values, &used, &count);
    ok(ret && count == _countof(paths), "EvtRender(paths): %d, count %lu, error %lu\n", ret, count, GetLastError());
    if (ret)
    {
        ok(values[0].UInt16Val == 1234, "path EventID %u\n", values[0].UInt16Val);
        ok(values[1].Type == EvtVarTypeString && !lstrcmpW(values[1].StringVal, L"gamma"), "path Data[2] %s\n",
           wine_dbgstr_w(values[1].StringVal));
        ok(values[2].Type == EvtVarTypeString && !lstrcmpW(values[2].StringVal, TEST_SOURCE), "path provider\n");
        ok(values[3].Type == EvtVarTypeNull, "unknown path type %lu\n", values[3].Type);
    }
    EvtClose(context);
    HeapFree(GetProcessHeap(), 0, values);
    EvtClose(event);

    /* The same event through a QueryList, through a local session, and not through a non-matching filter */
    event = newest(NULL, L"<QueryList><Query Id='0' Path='Application'><Select Path='Application'>"
                         L"*[System[EventID=1234]]</Select></Query></QueryList>", EvtQueryChannelPath);
    ok(event != NULL, "QueryList found nothing\n");
    if (event) EvtClose(event);

    login.Server = NULL;
    session = EvtOpenSession(EvtRpcLogin, &login, 0, 0);
    ok(session != NULL, "EvtOpenSession(local) failed: %lu\n", GetLastError());
    if (session)
    {
        EVT_HANDLE set = EvtQuery(session, L"Application", L"*[System/EventID=1234]", EvtQueryChannelPath);
        DWORD returned;
        ok(set != NULL, "EvtQuery(session) failed: %lu\n", GetLastError());
        if (set)
        {
            ret = EvtNext(set, 1, &event, INFINITE, 0, &returned);
            ok(ret && returned == 1, "EvtNext(session): %d, error %lu\n", ret, GetLastError());
            if (ret) EvtClose(event);
            EvtClose(set);
        }
        EvtClose(session);
    }
}

static void test_export(void)
{
    WCHAR dir[MAX_PATH], file[MAX_PATH];
    EVT_HANDLE event;
    BOOL ret;

    GetTempPathW(_countof(dir), dir);
    GetTempFileNameW(dir, L"evt", 0, file);
    DeleteFileW(file);
    lstrcatW(file, L".evtx");

    ret = EvtExportLog(NULL, L"Application", L"*", file, EvtExportLogChannelPath);
    ok(ret, "EvtExportLog failed: %lu\n", GetLastError());
    if (!ret) return;
    SetLastError(0xdeadbeef);
    ret = EvtExportLog(NULL, L"Application", NULL, file, EvtExportLogChannelPath);
    ok(!ret && GetLastError() == ERROR_FILE_EXISTS, "second export: %d, error %lu\n", ret, GetLastError());
    ret = EvtExportLog(NULL, L"Application", NULL, file, EvtExportLogChannelPath | EvtExportLogOverwrite);
    ok(ret, "export with overwrite failed: %lu\n", GetLastError());

    event = newest(file, L"*[System[EventID=1234]]", EvtQueryFilePath);
    ok(event != NULL, "the exported file has no test event\n");
    if (event) EvtClose(event);
    ok(DeleteFileW(file), "DeleteFile failed: %lu\n", GetLastError());
}

static volatile LONG push_count;
static HANDLE push_done;

static DWORD WINAPI push_callback(EVT_SUBSCRIBE_NOTIFY_ACTION action, void *context, EVT_HANDLE event)
{
    WCHAR *xml;

    ok(context == (void *)0x1234, "context %p\n", context);
    ok(action == EvtSubscribeActionDeliver, "action %u (error %lu)\n", action, (DWORD)(ULONG_PTR)event);
    if (action != EvtSubscribeActionDeliver) return 0;
    xml = render_xml(event);
    check_contains(xml, L">1236</EventID>");
    HeapFree(GetProcessHeap(), 0, xml);
    if (InterlockedIncrement(&push_count) == 1)
        SetEvent(push_done);
    return 0;
}

static void test_subscribe(void)
{
    EVT_HANDLE sub, event;
    HANDLE signal;
    DWORD returned, wait;
    WCHAR *xml;
    BOOL ret;

    SetLastError(0xdeadbeef);
    sub = EvtSubscribe(NULL, NULL, L"Application", L"*", NULL, NULL, NULL, EvtSubscribeToFutureEvents);
    ok(!sub && GetLastError() == ERROR_INVALID_PARAMETER, "no signal, no callback: %p, error %lu\n", sub, GetLastError());

    /* Pull */
    signal = CreateEventW(NULL, TRUE, FALSE, NULL);
    sub = EvtSubscribe(NULL, signal, L"Application", L"*[System[EventID=1235]]", NULL, NULL, NULL,
                       EvtSubscribeToFutureEvents);
    ok(sub != NULL, "EvtSubscribe(pull) failed: %lu\n", GetLastError());
    if (sub)
    {
        ret = EvtNext(sub, 1, &event, 0, 0, &returned);
        ok(!ret && GetLastError() == ERROR_NO_MORE_ITEMS, "nothing yet: %d, error %lu\n", ret, GetLastError());
        report(1235, EVENTLOG_INFORMATION_TYPE, 0, L"pull", L"");
        wait = WaitForSingleObject(signal, 10000);
        ok(wait == WAIT_OBJECT_0, "pull signal: %lu\n", wait);
        ret = EvtNext(sub, 1, &event, 0, 0, &returned);
        ok(ret && returned == 1, "EvtNext(pull): %d, error %lu\n", ret, GetLastError());
        if (ret)
        {
            xml = render_xml(event);
            check_contains(xml, L">1235</EventID>");
            check_contains(xml, L"<Data>pull</Data>");
            HeapFree(GetProcessHeap(), 0, xml);
            EvtClose(event);
        }
        ok(EvtClose(sub), "EvtClose(pull) failed: %lu\n", GetLastError());
    }
    CloseHandle(signal);

    /* Push */
    push_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    push_count = 0;
    sub = EvtSubscribe(NULL, NULL, L"Application", L"*[System[EventID=1236]]", NULL, (void *)0x1234, push_callback,
                       EvtSubscribeToFutureEvents);
    ok(sub != NULL, "EvtSubscribe(push) failed: %lu\n", GetLastError());
    if (sub)
    {
        report(1236, EVENTLOG_INFORMATION_TYPE, 0, L"push", L"");
        wait = WaitForSingleObject(push_done, 10000);
        ok(wait == WAIT_OBJECT_0, "push callback: %lu\n", wait);
        ok(EvtClose(sub), "EvtClose(push) failed: %lu\n", GetLastError());
        ok(push_count == 1, "push count %ld\n", push_count);
    }
    CloseHandle(push_done);
}

#endif /* WEVTAPI_TEST_READONLY */

START_TEST(EvtQuery)
{
    test_channels();
    test_log_info();
    test_invalid();
#ifndef WEVTAPI_TEST_READONLY
    test_written_event();
    test_export();
    test_subscribe();
#endif
    test_query_existing();
}
