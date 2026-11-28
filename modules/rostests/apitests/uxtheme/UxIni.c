/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the uxtheme INI line/value parser (uxini.c, compiled into this test)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <stdlib.h>
#include <stdio.h>
#include <windows.h>

/* Same layout as UXINI_FILE in dll/win32/uxtheme/uxini.c */
typedef struct _TEST_INI
{
    LPCWSTR lpIni;
    LPCWSTR lpCurLoc;
    LPCWSTR lpEnd;
    LPWSTR lpBuffer;
} TEST_INI;

LPCWSTR UXINI_GetNextSection(TEST_INI *uf, DWORD *dwLen);
LPCWSTR UXINI_GetNextValue(TEST_INI *uf, DWORD *dwNameLen, LPCWSTR *lpValue, DWORD *dwValueLen);

#define PAGE 0x1000

/*
 * Place the text at the very start (mode 0) or the very end (mode 1) of a committed page
 * whose neighbour in the read direction is PAGE_NOACCESS, so a read before the first or
 * after the last character faults instead of silently reading neighbouring memory.
 */
static PWSTR PlaceText(PVOID *pBase, const WCHAR *text, SIZE_T cch, int mode)
{
    SIZE_T pages = (cch * sizeof(WCHAR) + PAGE - 1) / PAGE + 1;
    PBYTE base = VirtualAlloc(NULL, (pages + 1) * PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    PWSTR dst;

    *pBase = base;
    if (!base)
        return NULL;
    if (mode == 0)
    {
        VirtualProtect(base, PAGE, PAGE_NOACCESS, &old);
        dst = (PWSTR)(base + PAGE);
    }
    else
    {
        VirtualProtect(base + pages * PAGE, PAGE, PAGE_NOACCESS, &old);
        dst = (PWSTR)(base + pages * PAGE - cch * sizeof(WCHAR));
    }
    memcpy(dst, text, cch * sizeof(WCHAR));
    return dst;
}

/* Walk sections and values the way msstyles.c does and describe what was found */
static BOOL Parse(const WCHAR *text, SIZE_T cch, int mode, char *out, SIZE_T cbOut, DWORD *pdwTicks)
{
    PVOID base;
    PWSTR buf = PlaceText(&base, text, cch, mode);
    TEST_INI uf;
    LPCWSTR name, value;
    DWORD len, nameLen, valueLen, t0 = GetTickCount();
    SIZE_T n = 0;
    BOOL ret = TRUE;

    out[0] = 0;
    if (!buf)
        return FALSE;
    uf.lpIni = uf.lpCurLoc = buf;
    uf.lpEnd = buf + cch;
    uf.lpBuffer = NULL;

    _SEH2_TRY
    {
        while ((name = UXINI_GetNextSection(&uf, &len)) != NULL)
        {
            n += snprintf(out + n, cbOut - n, "S:%.*ls;", (int)min(len, 64), name);
            while ((name = UXINI_GetNextValue(&uf, &nameLen, &value, &valueLen)) != NULL)
                n += snprintf(out + n, cbOut - n, "V:%.*ls=%.*ls;", (int)min(nameLen, 64), name, (int)min(valueLen, 64), value);
            if (n > cbOut - 200)
                break;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        snprintf(out, cbOut, "EXCEPTION 0x%lx", (unsigned long)_SEH2_GetExceptionCode());
        ret = FALSE;
    }
    _SEH2_END;

    *pdwTicks = GetTickCount() - t0;
    VirtualFree(base, 0, MEM_RELEASE);
    return ret;
}

static void Check(const WCHAR *text, const char *expected, const char *what)
{
    char out[1024];
    DWORD ticks = 0;
    int mode;

    for (mode = 0; mode < 2; mode++)
    {
        BOOL ok_run = Parse(text, wcslen(text), mode, out, sizeof(out), &ticks);

        ok(ok_run, "%s (mode %d): faulted: %s\n", what, mode, out);
        if (ok_run)
        {
            ok(strcmp(out, expected) == 0, "%s (mode %d): got '%s', expected '%s'\n", what, mode, out, expected);
            ok(ticks < 2000, "%s (mode %d): took %lu ms\n", what, mode, ticks);
        }
    }
}

START_TEST(UxIni)
{
    PWSTR big;
    SIZE_T i, cch = 200000;
    char out[256];
    DWORD ticks;

    /* well-formed text, CRLF and LF */
    Check(L"[Sec]\r\nName = Value\r\nA=b\r\n", "S:Sec;V:Name=Value;V:A=b;", "CRLF");
    Check(L"[Sec]\nName = Value\nA=b\n", "S:Sec;V:Name=Value;V:A=b;", "LF");
    Check(L"[Sec]\r\nName = Value\r\nA=b", "S:Sec;V:Name=Value;V:A=b;", "no final newline");
    Check(L"[One]\r\nk=v\r\n[Two]\r\nx = y z \r\n", "S:One;V:k=v;S:Two;V:x=y z;", "two sections, trailing blanks");

    /* shape of the text resources inside theme files: a byte order mark, then CRLF, then sections */
    Check(L"\xFEFF\r\n[Sec]\r\nk=v\r\n", "S:Sec;V:k=v;", "BOM first line");
    Check(L"\xFEFF", "", "BOM only");
    Check(L"\r\n", "", "CRLF only (4 byte resource)");
    Check(L"", "", "empty text");

    /* whitespace only lines and runs */
    Check(L"   \r\n\t\r\n \r\n", "", "whitespace only");
    Check(L"[S]\r\n   \r\n\t \r\nk=v\r\n   ", "S:S;V:k=v;", "whitespace lines and tail");

    /* lines without a usable name are skipped */
    Check(L"[S]\r\n=x\r\nk=v\r\n", "S:S;V:k=v;", "leading '='");
    Check(L"[S]\r\n  \t=x\r\nk=v\r\n", "S:S;V:k=v;", "blanks then '='");
    Check(L"[S]\r\n=", "S:S;", "'=' is the last character");
    Check(L"=", "", "'=' only");
    Check(L"[S]\r\n=\r\n=\r\nk=v", "S:S;V:k=v;", "only empty names before a value");

    /* empty values */
    Check(L"[S]\r\nk=\r\n", "S:S;V:k=;", "empty value");
    Check(L"[S]\r\nk =   \r\n", "S:S;V:k=;", "blank value");
    Check(L"[S]\r\nk=", "S:S;V:k=;", "empty value at end of text");

    /* malformed section lines */
    Check(L"[\r\nk=v\r\n", "S:;V:k=v;", "lone '['");
    Check(L"[abc\r\nk=v\r\n", "S:abc;V:k=v;", "section without ']'");
    Check(L"[", "S:;", "'[' is the last character");

    /* comments */
    Check(L"[S]\r\n; c\r\nk=v ; trailing\r\n;\r\nx=y\r\n", "S:S;V:k=v;V:x=y;", "comments");
    Check(L";only a comment", "", "comment only");
    Check(L";", "", "';' only");

    /* a very long run of blanks must not take long */
    big = malloc((cch + 16) * sizeof(WCHAR));
    if (big)
    {
        for (i = 0; i < cch; i++)
            big[i] = ' ';
        memcpy(big + cch, L"\r\n[S]\r\nk=v", 11 * sizeof(WCHAR));
        big[cch + 11] = 0;
        for (i = 0; i < 2; i++)
        {
            BOOL r = Parse(big, cch + 11, (int)i, out, sizeof(out), &ticks);
            ok(r, "long blank run (mode %d): faulted: %s\n", (int)i, out);
            ok(strcmp(out, "S:S;V:k=v;") == 0, "long blank run (mode %d): got '%s'\n", (int)i, out);
            ok(ticks < 2000, "long blank run (mode %d): took %lu ms\n", (int)i, ticks);
        }
        free(big);
    }
}
