/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the wlrmdr.exe command-line parser (compiled into this test)
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <apitest.h>
#include <windows.h>
#include <wchar.h>

#include "wlrmdr.h"

#define PARSE(Args, ...) \
    do { PWSTR argv_[] = { L"wlrmdr.exe", __VA_ARGS__ }; \
         WlrParseCmdLine(ARRAYSIZE(argv_), argv_, &(Args)); } while (0)

static void test_shutdown_reminder(void)
{
    WLRMDR_ARGS a;

    /* The command line winlogon uses on Win11 for a timed shutdown */
    PARSE(a, L"-s", L"-1", L"-f", L"2", L"-t", L"You're about to be signed out",
          L"-m", L"Windows will shut down in 1 minute.", L"-a", L"3");
    ok(a.TimeoutMs < 0, "TimeoutMs = %ld\n", a.TimeoutMs);
    ok(a.Flags == 2, "Flags = %lu\n", a.Flags);
    ok(a.HasAction && a.Action == 3, "Action = %lu (%d)\n", a.Action, a.HasAction);
    ok(a.Title && !wcscmp(a.Title, L"You're about to be signed out"), "Title\n");
    ok(a.Message && !wcscmp(a.Message, L"Windows will shut down in 1 minute."), "Message\n");
}

static void test_defaults(void)
{
    WLRMDR_ARGS a;
    PWSTR none[] = { L"wlrmdr.exe" };

    WlrParseCmdLine(ARRAYSIZE(none), none, &a);
    ok(a.TimeoutMs < 0, "no -s must mean no auto-dismiss, got %ld\n", a.TimeoutMs);
    ok(a.Flags == 0 && !a.HasAction && !a.Title && !a.Message, "defaults\n");

    /* The bare persistent and abort instances */
    PARSE(a, L"-s", L"-1");
    ok(a.TimeoutMs < 0 && !a.Message, "-s -1\n");
    PARSE(a, L"-s", L"120000");
    ok(a.TimeoutMs == 120000 && !a.Message, "-s 120000: %ld\n", a.TimeoutMs);
    PARSE(a, L"-s", L"0");
    ok(a.TimeoutMs == 0, "-s 0: %ld\n", a.TimeoutMs);
}

static void test_ignored_input(void)
{
    WLRMDR_ARGS a;

    PARSE(a, L"-x", L"-s", L"500", L"/q", L"-zz", L"-m", L"hi");
    ok(a.TimeoutMs == 500, "unknown switches must not disturb -s: %ld\n", a.TimeoutMs);
    ok(a.Message && !wcscmp(a.Message, L"hi"), "Message after unknown switches\n");

    /* Non-numeric or out-of-range numbers leave the default and do not eat the next token */
    PARSE(a, L"-s", L"abc", L"-f", L"3x", L"-a", L"99999999999999999999", L"-m", L"m");
    ok(a.TimeoutMs < 0, "bad -s: %ld\n", a.TimeoutMs);
    ok(a.Flags == 0, "bad -f: %lu\n", a.Flags);
    ok(!a.HasAction, "out of range -a accepted\n");
    ok(a.Message && !wcscmp(a.Message, L"m"), "Message after bad numbers\n");

    /* A switch with no value at the end is ignored */
    PARSE(a, L"-m", L"x", L"-t");
    ok(!a.Title, "trailing -t\n");
    ok(a.Message && !wcscmp(a.Message, L"x"), "Message before trailing -t\n");

    /* Positional arguments are not switches */
    PARSE(a, L"stray", L"-s", L"7");
    ok(a.TimeoutMs == 7, "stray argument: %ld\n", a.TimeoutMs);
}

START_TEST(cmdline)
{
    test_shutdown_reminder();
    test_defaults();
    test_ignored_input();
}
