/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for wevtutil.exe: exit codes and output shapes of el, gl, gli, qe and epl
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 *
 * Read-only on the logs: nothing is cleared or configured. epl writes one
 * backup file into the temporary directory and removes it.
 */

#include <apitest.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OUTPUT (4 * 1024 * 1024)

/* Runs wevtutil with the given arguments; stdout and stderr are merged into the returned ANSI buffer */
static char *run_wevtutil(const char *args, DWORD *exit_code, DWORD *length)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE read_pipe, write_pipe;
    char cmdline[1024], system_dir[MAX_PATH];
    char *output;
    DWORD total = 0, got;

    *exit_code = (DWORD)-1;
    *length = 0;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
        return NULL;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    GetSystemDirectoryA(system_dir, ARRAYSIZE(system_dir));
    snprintf(cmdline, sizeof(cmdline), "\"%s\\wevtutil.exe\" %s", system_dir, args);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        trace("CreateProcess(%s) failed: %lu\n", cmdline, GetLastError());
        CloseHandle(write_pipe);
        CloseHandle(read_pipe);
        return NULL;
    }
    CloseHandle(write_pipe);
    CloseHandle(pi.hThread);

    output = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_OUTPUT + 1);
    if (output)
    {
        while (total < MAX_OUTPUT && ReadFile(read_pipe, output + total, MAX_OUTPUT - total, &got, NULL) && got)
            total += got;
        output[total] = 0;
    }
    CloseHandle(read_pipe);
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0)
    {
        ok(0, "wevtutil %s did not exit within 60 s, terminating it\n", args);
        TerminateProcess(pi.hProcess, (UINT)-1);
        WaitForSingleObject(pi.hProcess, 10000);
    }
    GetExitCodeProcess(pi.hProcess, exit_code);
    CloseHandle(pi.hProcess);
    *length = total;
    return output;
}

static void free_output(char *output)
{
    HeapFree(GetProcessHeap(), 0, output);
}

/* TRUE when the output has a line that starts with the given text */
static BOOL has_line_starting(const char *output, const char *start)
{
    const char *p = output;
    size_t len = strlen(start);

    while (p)
    {
        if (!strncmp(p, start, len))
            return TRUE;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return FALSE;
}

static BOOL has_line(const char *output, const char *line)
{
    const char *p = output;
    size_t len = strlen(line);

    while (p)
    {
        if (!strncmp(p, line, len) && (p[len] == '\r' || p[len] == '\n' || !p[len]))
            return TRUE;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return FALSE;
}

static int count_occurrences(const char *output, const char *part)
{
    int n = 0;
    size_t len = strlen(part);

    while ((output = strstr(output, part)))
    {
        n++;
        output += len;
    }
    return n;
}

static BOOL ends_with(const char *output, DWORD length, const char *tail)
{
    size_t len = strlen(tail);
    return length >= len && !memcmp(output + length - len, tail, len);
}

static void check_exit(const char *args, DWORD expected, int line)
{
    DWORD code, length;
    char *output = run_wevtutil(args, &code, &length);

    ok_(__FILE__, line)(output != NULL, "wevtutil %s did not run\n", args);
    ok_(__FILE__, line)(code == expected, "wevtutil %s: exit %lu, expected %lu; output: %s\n", args, code, expected,
                        output ? output : "(none)");
    free_output(output);
}
#define check_exit(a, e) check_exit(a, e, __LINE__)

static void test_usage(void)
{
    DWORD code, length;
    char *output;

    /* No command: the help, exit 0 */
    output = run_wevtutil("", &code, &length);
    ok(output != NULL, "no output\n");
    if (output)
    {
        ok(code == 0, "no command: exit %lu\n", code);
        ok(strstr(output, "el | enum-logs") != NULL, "no command: command list not printed: %s\n", output);
        ok(strstr(output, "cl | clear-log") != NULL, "no command: command list incomplete\n");
        free_output(output);
    }
    output = run_wevtutil("qe /?", &code, &length);
    ok(output != NULL, "no output\n");
    if (output)
    {
        ok(code == 0, "qe /?: exit %lu\n", code);
        ok(strstr(output, "query-events") != NULL, "qe /?: no qe help: %s\n", output);
        free_output(output);
    }

    /* Usage errors are ERROR_INVALID_PARAMETER */
    check_exit("bogus", ERROR_INVALID_PARAMETER);
    check_exit("gl", ERROR_INVALID_PARAMETER);
    check_exit("cl", ERROR_INVALID_PARAMETER);
    check_exit("qe Application /f:bogus", ERROR_INVALID_PARAMETER);
    check_exit("qe Application /zz:1", ERROR_INVALID_PARAMETER);
    check_exit("gl Application /f", ERROR_INVALID_PARAMETER);
    check_exit("qe System /c:1 /rd:", ERROR_INVALID_PARAMETER);
    check_exit("qe System /c:1 /rd:yes", ERROR_INVALID_PARAMETER);
    check_exit("gli System /f:xml", ERROR_INVALID_PARAMETER);

    /* An unknown channel is reported with the wevtapi error */
    check_exit("gl NoSuchLogLiberNTTest", ERROR_EVT_CHANNEL_NOT_FOUND);
    check_exit("qe NoSuchLogLiberNTTest", ERROR_EVT_CHANNEL_NOT_FOUND);
}

static void test_enum_logs(void)
{
    DWORD code, length, i;
    char *output = run_wevtutil("el", &code, &length);

    ok(output != NULL, "no output\n");
    if (!output) return;
    ok(code == 0, "el: exit %lu\n", code);
    ok(has_line(output, "Application"), "el: Application missing\n");
    ok(has_line(output, "System"), "el: System missing\n");
    ok(length >= 3 && !(output[0] == (char)0xEF && output[1] == (char)0xBB && output[2] == (char)0xBF), "el: BOM\n");
    for (i = 0; i < length; i++)
    {
        if (!output[i])
        {
            ok(0, "el: NUL byte at %lu, output is not ANSI\n", i);
            break;
        }
    }
    free_output(output);
}

static void test_get_log(void)
{
    static const char *text_lines[] =
    {
        "name: Application", "enabled: ", "type: ", "owningPublisher: ", "isolation: ", "channelAccess: O:BAG:SYD:",
        "logging:", "  logFileName: ", "  retention: ", "  autoBackup: ", "  maxSize: ", "publishing:", "  fileMax: "
    };
    DWORD code, length, i;
    char *output;

    output = run_wevtutil("gl Application", &code, &length);
    ok(output != NULL, "no output\n");
    if (output)
    {
        ok(code == 0, "gl: exit %lu\n", code);
        for (i = 0; i < ARRAYSIZE(text_lines); i++)
            ok(has_line_starting(output, text_lines[i]), "gl: line '%s' missing in:\n%s\n", text_lines[i], output);
        free_output(output);
    }

    output = run_wevtutil("gl Application /f:xml", &code, &length);
    ok(output != NULL, "no output\n");
    if (output)
    {
        ok(code == 0, "gl xml: exit %lu\n", code);
        ok(!strncmp(output, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n", 40), "gl xml: declaration: %.60s\n", output);
        ok(strstr(output, "<channel name=\"Application\" enabled=\"") != NULL, "gl xml: channel element\n");
        ok(strstr(output, " xmlns=\"http://schemas.microsoft.com/win/2004/08/events\">") != NULL, "gl xml: namespace\n");
        ok(has_line(output, "  <logging>"), "gl xml: logging element\n");
        ok(has_line_starting(output, "    <maxSize>"), "gl xml: maxSize element\n");
        ok(ends_with(output, length, "</channel>\r\n"), "gl xml: tail: %s\n", output);
        free_output(output);
    }

    output = run_wevtutil("gli Application", &code, &length);
    ok(output != NULL, "no output\n");
    if (output)
    {
        static const char *gli_lines[] =
        {
            "creationTime: ", "lastAccessTime: ", "lastWriteTime: ", "fileSize: ", "attributes: ",
            "numberOfLogRecords: ", "oldestRecordNumber: "
        };
        const char *p = output;

        ok(code == 0, "gli: exit %lu\n", code);
        for (i = 0; i < ARRAYSIZE(gli_lines) && p; i++)
        {
            ok(!strncmp(p, gli_lines[i], strlen(gli_lines[i])), "gli: line %lu is '%.40s', expected '%s'\n", i, p,
               gli_lines[i]);
            p = strchr(p, '\n');
            if (p) p++;
        }
        ok(p && !*p, "gli: unexpected trailing output: %s\n", p ? p : "(cut)");
        free_output(output);
    }
}

static BOOL is_iso_date(const char *p)
{
    static const char pattern[] = "dddd-dd-ddTdd:dd:dd.dddddddZ";
    size_t i;

    for (i = 0; pattern[i]; i++)
    {
        if (pattern[i] == 'd' ? !(p[i] >= '0' && p[i] <= '9') : p[i] != pattern[i])
            return FALSE;
    }
    return TRUE;
}

static void test_query_events(void)
{
    static const char *text_lines[] =
    {
        "Event[0]", "  Log Name: Application", "  Source: ", "  Date: ", "  Event ID: ", "  Task: ", "  Level: ",
        "  Opcode: ", "  Keyword: ", "  User: ", "  User Name: ", "  Computer: ", "  Description: "
    };
    static const char event_start[] = "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='";
    DWORD code, length, i;
    char *output, *p;

    output = run_wevtutil("qe Application /c:1 /rd:true /f:xml", &code, &length);
    ok(output != NULL, "no output\n");
    if (!output) return;
    ok(code == 0, "qe xml: exit %lu: %s\n", code, output);
    if (code == 0 && !length)
    {
        skip("the Application log is empty\n");
        free_output(output);
        return;
    }
    ok(!strncmp(output, event_start, strlen(event_start)), "qe xml: start: %.120s\n", output);
    ok(ends_with(output, length, "</Event>"), "qe xml: tail: %.80s\n", output + (length > 80 ? length - 80 : 0));
    ok(count_occurrences(output, "<Event ") == 1, "qe xml: %d events\n", count_occurrences(output, "<Event "));
    ok(strstr(output, "<Channel>Application</Channel>") != NULL, "qe xml: channel element\n");
    free_output(output);

    output = run_wevtutil("qe Application /c:2 /rd:true /f:xml /e:root", &code, &length);
    ok(output != NULL, "no output\n");
    if (output)
    {
        ok(code == 0, "qe /e: exit %lu\n", code);
        ok(!strncmp(output, "<root>\r\n<Event ", 15), "qe /e: start: %.40s\n", output);
        ok(ends_with(output, length, "</Event></root>\r\n"), "qe /e: tail: %.60s\n",
           output + (length > 60 ? length - 60 : 0));
        ok(count_occurrences(output, "<Event ") <= 2, "qe /e: %d events\n", count_occurrences(output, "<Event "));
        free_output(output);
    }

    output = run_wevtutil("qe Application /c:1 /rd:true /f:text", &code, &length);
    ok(output != NULL, "no output\n");
    if (output)
    {
        ok(code == 0, "qe text: exit %lu\n", code);
        p = output;
        for (i = 0; i < ARRAYSIZE(text_lines) && p; i++)
        {
            ok(!strncmp(p, text_lines[i], strlen(text_lines[i])), "qe text: line %lu is '%.40s', expected '%s'\n", i,
               p, text_lines[i]);
            if (i == 3)
                ok(is_iso_date(p + strlen("  Date: ")), "qe text: date format: %.40s\n", p);
            p = strchr(p, '\n');
            if (p) p++;
        }
        ok(ends_with(output, length, "\r\n\r\n"), "qe text: no blank line after the event\n");
        free_output(output);
    }

    output = run_wevtutil("qe Application /c:1 /rd:true /f:renderedxml", &code, &length);
    ok(output != NULL, "no output\n");
    if (output)
    {
        ok(code == 0, "qe renderedxml: exit %lu\n", code);
        ok(strstr(output, "<RenderingInfo Culture='") != NULL, "qe renderedxml: no RenderingInfo: %.200s\n", output);
        ok(ends_with(output, length, "</RenderingInfo></Event>"), "qe renderedxml: tail: %.80s\n",
           output + (length > 80 ? length - 80 : 0));
        free_output(output);
    }

    /* A bare boolean switch means true */
    check_exit("qe Application /c:1 /rd", 0);
    check_exit("qe Application /c:1 /rd:true /uni", 0);

    output = run_wevtutil("qe Application /c:1 /rd:true /f:xml /uni:true", &code, &length);
    ok(output != NULL, "no output\n");
    if (output)
    {
        BOOL wide = FALSE;
        ok(code == 0, "qe /uni: exit %lu\n", code);
        for (i = 1; i < length && !wide; i += 2)
            wide = !output[i];
        ok(wide, "qe /uni: output is not UTF-16\n");
        free_output(output);
    }
}

static void test_export(void)
{
    char temp[MAX_PATH], file[MAX_PATH + 64], args[MAX_PATH + 128];
    WIN32_FILE_ATTRIBUTE_DATA before = { 0 }, after = { 0 };
    DWORD code, length, i;
    char *output;

    GetTempPathA(ARRAYSIZE(temp), temp);
    snprintf(file, sizeof(file), "%swevtutil_apitest_%lu.evtx", temp, GetCurrentProcessId());
    DeleteFileA(file);

    snprintf(args, sizeof(args), "epl Application \"%s\"", file);
    output = run_wevtutil(args, &code, &length);
    ok(output != NULL, "no output\n");
    if (!output) return;
    if (code == ERROR_ACCESS_DENIED)
    {
        skip("epl: access denied\n");
        free_output(output);
        return;
    }
    ok(code == 0, "epl: exit %lu: %s\n", code, output);
    ok(GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES, "epl: no file written\n");
    free_output(output);

    /* The target exists: refused without /ow */
    check_exit(args, ERROR_FILE_EXISTS);

    /* With /ow the overwrite succeeds once the Event Log service has let go of the
     * fresh export: Windows 11 keeps a 20 MB export open for up to six seconds
     * and answers ERROR_SHARING_VIOLATION until then. */
    GetFileAttributesExA(file, GetFileExInfoStandard, &before);
    snprintf(args, sizeof(args), "epl Application \"%s\" /ow:true", file);
    for (i = 0; i < 100; i++)
    {
        output = run_wevtutil(args, &code, &length);
        free_output(output);
        if (code != ERROR_SHARING_VIOLATION)
            break;
        Sleep(100);
    }
    ok(code == 0, "epl /ow: exit %lu after %lu retries\n", code, i);
    trace("epl /ow: succeeded after %lu retries\n", i);
    ok(GetFileAttributesExA(file, GetFileExInfoStandard, &after), "epl /ow: file gone\n");
    ok(CompareFileTime(&after.ftLastWriteTime, &before.ftLastWriteTime) > 0, "epl /ow: file not rewritten\n");

    for (i = 0; i < 100 && !DeleteFileA(file) && GetLastError() == ERROR_SHARING_VIOLATION; i++)
        Sleep(100);
}

START_TEST(wevtutil)
{
    test_usage();
    test_enum_logs();
    test_get_log();
    test_query_events();
    test_export();
}
