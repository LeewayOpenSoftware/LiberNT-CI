/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for EvtOpenPublisherMetadata and EvtFormatMessage
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 *
 * Read-only: the tests format events that already exist in the System and
 * Application logs and never write, clear or register anything.
 */

#include <apitest.h>
#include <windows.h>
#include <winevt.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_SCANNED_EVENTS 200

static WCHAR *format_alloc(EVT_HANDLE publisher, EVT_HANDLE event, DWORD id, DWORD flags, DWORD *used_out,
                           DWORD *error_out)
{
    DWORD used = 0;
    WCHAR *text;
    BOOL ret;

    SetLastError(0xdeadbeef);
    ret = EvtFormatMessage(publisher, event, id, 0, NULL, flags, 0, NULL, &used);
    *error_out = GetLastError();
    if (ret || *error_out != ERROR_INSUFFICIENT_BUFFER)
    {
        if (used_out) *used_out = used;
        return NULL;
    }
    text = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, used * sizeof(WCHAR));
    if (!text) return NULL;
    ret = EvtFormatMessage(publisher, event, id, 0, NULL, flags, used, text, &used);
    *error_out = ret ? ERROR_SUCCESS : GetLastError();
    if (used_out) *used_out = used;
    if (!ret && *error_out != ERROR_EVT_UNRESOLVED_VALUE_INSERT && *error_out != ERROR_EVT_UNRESOLVED_PARAMETER_INSERT)
    {
        HeapFree(GetProcessHeap(), 0, text);
        return NULL;
    }
    return text;
}

static WCHAR *render_system_string(EVT_HANDLE event, EVT_SYSTEM_PROPERTY_ID id)
{
    EVT_HANDLE context;
    DWORD used = 0, count = 0;
    EVT_VARIANT *values;
    WCHAR *copy = NULL;

    context = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);
    ok(context != NULL, "EvtCreateRenderContext failed: %lu\n", GetLastError());
    if (!context) return NULL;
    EvtRender(context, event, EvtRenderEventValues, 0, NULL, &used, &count);
    values = HeapAlloc(GetProcessHeap(), 0, used);
    if (values && EvtRender(context, event, EvtRenderEventValues, used, values, &used, &count) &&
        count > (DWORD)id && values[id].Type == EvtVarTypeString && values[id].StringVal)
    {
        copy = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(values[id].StringVal) + 1) * sizeof(WCHAR));
        if (copy) lstrcpyW(copy, values[id].StringVal);
    }
    HeapFree(GetProcessHeap(), 0, values);
    EvtClose(context);
    return copy;
}

static void test_open_publisher(void)
{
    EVT_HANDLE publisher;

    /* A NULL identity is accepted: Windows 11 returns a handle */
    SetLastError(0xdeadbeef);
    publisher = EvtOpenPublisherMetadata(NULL, NULL, NULL, 0, 0);
    ok(publisher != NULL, "NULL identity: error %lu\n", GetLastError());
    if (publisher) EvtClose(publisher);

    SetLastError(0xdeadbeef);
    publisher = EvtOpenPublisherMetadata(NULL, L"Application", NULL, 0, 1);
    ok(!publisher, "flags 1: %p\n", publisher);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "flags 1: error %lu\n", GetLastError());
    if (publisher) EvtClose(publisher);

    SetLastError(0xdeadbeef);
    publisher = EvtOpenPublisherMetadata(NULL, L"NoSuchPublisher-LiberNT-Test", NULL, 0, 0);
    ok(!publisher, "unknown publisher: %p\n", publisher);
    ok(GetLastError() == ERROR_FILE_NOT_FOUND, "unknown publisher: error %lu\n", GetLastError());
    if (publisher) EvtClose(publisher);
}

static void test_format_arguments(EVT_HANDLE publisher, EVT_HANDLE event)
{
    WCHAR buffer[16];
    DWORD used;
    BOOL ret;

    SetLastError(0xdeadbeef);
    ret = EvtFormatMessage(publisher, event, 0, 0, NULL, 0, ARRAYSIZE(buffer), buffer, &used);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "flags 0: %d, error %lu\n", ret, GetLastError());

    SetLastError(0xdeadbeef);
    ret = EvtFormatMessage(publisher, event, 0, 0, NULL, EvtFormatMessageXml + 1, ARRAYSIZE(buffer), buffer, &used);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "flags 10: %d, error %lu\n", ret, GetLastError());

    /* A NULL BufferUsed is tolerated: the call still reports the short buffer */
    SetLastError(0xdeadbeef);
    ret = EvtFormatMessage(publisher, event, 0, 0, NULL, EvtFormatMessageEvent, 1, buffer, NULL);
    ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "NULL used: %d, error %lu\n", ret, GetLastError());

    SetLastError(0xdeadbeef);
    ret = EvtFormatMessage(publisher, NULL, 0, 0, NULL, EvtFormatMessageEvent, ARRAYSIZE(buffer), buffer, &used);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "NULL event: %d, error %lu\n", ret, GetLastError());

    /* A size without a buffer counts as a buffer that is too small */
    SetLastError(0xdeadbeef);
    ret = EvtFormatMessage(publisher, event, 0, 0, NULL, EvtFormatMessageEvent, 4, NULL, &used);
    ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "size without buffer: %d, error %lu\n", ret, GetLastError());

    /* Without publisher metadata nothing is resolved, not even from the event's own publisher */
    SetLastError(0xdeadbeef);
    ret = EvtFormatMessage(NULL, event, 0, 0, NULL, EvtFormatMessageEvent, 0, NULL, &used);
    ok(!ret && GetLastError() == ERROR_EVT_MESSAGE_NOT_FOUND, "NULL publisher: %d, error %lu\n", ret, GetLastError());
}

static void test_formats(EVT_HANDLE publisher, EVT_HANDLE event, const WCHAR *provider)
{
    WCHAR *text, tiny[2];
    DWORD used, error, level_used;
    BOOL ret;

    /* The event message: the whole buffer contract on one call.
     * BufferUsed counts the text, its terminator and one more character. */
    text = format_alloc(publisher, event, 0, EvtFormatMessageEvent, &used, &error);
    ok(text != NULL, "EvtFormatMessageEvent failed: %lu\n", error);
    if (text)
    {
        ok(used == (DWORD)lstrlenW(text) + 2, "used %lu for length %d\n", used, lstrlenW(text));
        ok(text[0] != 0, "empty message\n");
        SetLastError(0xdeadbeef);
        ret = EvtFormatMessage(publisher, event, 0, 0, NULL, EvtFormatMessageEvent, 1, tiny, &level_used);
        ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "one char: %d, error %lu\n", ret, GetLastError());
        ok(level_used == used, "one char: used %lu, expected %lu\n", level_used, used);
        HeapFree(GetProcessHeap(), 0, text);
    }

    /* The level name is non-empty for every event that has a level */
    text = format_alloc(publisher, event, 0, EvtFormatMessageLevel, &used, &error);
    ok(text != NULL, "EvtFormatMessageLevel failed: %lu\n", error);
    if (text)
    {
        ok(text[0] != 0 && used == (DWORD)lstrlenW(text) + 2, "level %s used %lu\n", wine_dbgstr_w(text), used);
        HeapFree(GetProcessHeap(), 0, text);
    }

    /* The provider display name: the provider name itself when there is none */
    text = format_alloc(publisher, event, 0, EvtFormatMessageProvider, &used, &error);
    ok(text != NULL, "EvtFormatMessageProvider failed: %lu\n", error);
    if (text)
    {
        ok(text[0] != 0, "empty provider for %s\n", wine_dbgstr_w(provider));
        HeapFree(GetProcessHeap(), 0, text);
    }

    /* The channel display name; a channel without one reports the id as not found */
    text = format_alloc(publisher, event, 0, EvtFormatMessageChannel, &used, &error);
    ok(text != NULL || error == ERROR_EVT_MESSAGE_ID_NOT_FOUND, "EvtFormatMessageChannel failed: %lu\n", error);
    if (text)
    {
        ok(text[0] != 0, "empty channel\n");
        HeapFree(GetProcessHeap(), 0, text);
    }

    /* Keywords come back as a multi-string; an event without any reports the id as not found */
    text = format_alloc(publisher, event, 0, EvtFormatMessageKeyword, &used, &error);
    ok(text != NULL || error == ERROR_EVT_MESSAGE_ID_NOT_FOUND, "EvtFormatMessageKeyword failed: %lu\n", error);
    if (text)
    {
        ok(used >= 2, "keyword used %lu\n", used);
        ok(text[used - 1] == 0 && text[used - 2] == 0, "keywords not double terminated (used %lu)\n", used);
        HeapFree(GetProcessHeap(), 0, text);
    }

    /* The rendered XML carries the event XML plus RenderingInfo */
    text = format_alloc(publisher, event, 0, EvtFormatMessageXml, &used, &error);
    ok(text != NULL, "EvtFormatMessageXml failed: %lu\n", error);
    if (text)
    {
        ok(wcsstr(text, L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'>") != NULL,
           "no Event root in %s\n", wine_dbgstr_w(text));
        ok(wcsstr(text, L"<RenderingInfo Culture='") != NULL, "no RenderingInfo in %s\n", wine_dbgstr_w(text));
        ok(wcsstr(text, L"<Message>") != NULL, "no Message in %s\n", wine_dbgstr_w(text));
        ok(wcsstr(text, L"</RenderingInfo></Event>") != NULL, "RenderingInfo not last in %s\n", wine_dbgstr_w(text));
        HeapFree(GetProcessHeap(), 0, text);
    }

    /* A message id that no message file has: reported as missing for the locale */
    text = format_alloc(publisher, NULL, 0xFFFFFFFE, EvtFormatMessageId, &used, &error);
    ok(text == NULL, "unknown message id succeeded: %s\n", wine_dbgstr_w(text));
    ok(error == ERROR_EVT_MESSAGE_LOCALE_NOT_FOUND, "unknown message id: error %lu\n", error);
    HeapFree(GetProcessHeap(), 0, text);
}

/* Finds the newest event of the channel whose publisher resolves its message */
static void test_channel(const WCHAR *channel)
{
    EVT_HANDLE query, event, publisher = NULL;
    DWORD returned, used, error;
    WCHAR *provider = NULL, *text;
    int scanned = 0;
    BOOL found = FALSE;

    query = EvtQuery(NULL, channel, NULL, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!query)
    {
        skip("cannot query %s: %lu\n", wine_dbgstr_w(channel), GetLastError());
        return;
    }
    while (!found && scanned < MAX_SCANNED_EVENTS && EvtNext(query, 1, &event, INFINITE, 0, &returned))
    {
        scanned++;
        HeapFree(GetProcessHeap(), 0, provider);
        provider = render_system_string(event, EvtSystemProviderName);
        if (provider)
        {
            publisher = EvtOpenPublisherMetadata(NULL, provider, NULL, 0, 0);
            if (publisher)
            {
                text = format_alloc(publisher, event, 0, EvtFormatMessageEvent, &used, &error);
                if (text)
                {
                    found = TRUE;
                    HeapFree(GetProcessHeap(), 0, text);
                    break;
                }
                EvtClose(publisher);
                publisher = NULL;
            }
        }
        EvtClose(event);
    }
    if (!found)
    {
        skip("%s: no event with a resolvable message among %d\n", wine_dbgstr_w(channel), scanned);
        HeapFree(GetProcessHeap(), 0, provider);
        EvtClose(query);
        return;
    }
    trace("%s: provider %s\n", wine_dbgstr_w(channel), wine_dbgstr_w(provider));

    test_format_arguments(publisher, event);
    test_formats(publisher, event, provider);

    EvtClose(publisher);
    EvtClose(event);
    HeapFree(GetProcessHeap(), 0, provider);
    EvtClose(query);
}

START_TEST(EvtFormatMessage)
{
    test_open_publisher();
    test_channel(L"System");
    test_channel(L"Application");
}
