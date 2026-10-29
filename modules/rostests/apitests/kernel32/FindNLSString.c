/*
 * PROJECT:     LiberNT kernel32 API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     kernel32!FindNLSString is a real export that reaches the NLS code
 */

#include "precomp.h"

#define EN_US MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT)

START_TEST(FindNLSString)
{
    static const WCHAR Haystack[] = L"Hello World";
    INT Found, Index;

    /* The export must exist in kernel32 and must not be the generic stub */
    ok(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FindNLSString") != NULL,
       "kernel32!FindNLSString is not exported\n");

    /* Forward search */
    Found = 0xCCCC;
    Index = FindNLSString(EN_US, FIND_FROMSTART, Haystack, -1, L"World", -1, &Found);
    ok(Index == 6, "Index = %d, expected 6\n", Index);
    ok(Found == 5, "Found = %d, expected 5\n", Found);

    /* Case sensitivity: the default is case sensitive */
    Found = 0xCCCC;
    SetLastError(0xDEADBEEF);
    Index = FindNLSString(EN_US, FIND_FROMSTART, Haystack, -1, L"world", -1, &Found);
    ok(Index == -1, "Index = %d, expected -1\n", Index);

    Found = 0xCCCC;
    Index = FindNLSString(EN_US, FIND_FROMSTART | NORM_IGNORECASE, Haystack, -1, L"world", -1, &Found);
    ok(Index == 6, "Index = %d, expected 6\n", Index);
    ok(Found == 5, "Found = %d, expected 5\n", Found);

    /* Other directions, with an explicit length that excludes the terminator */
    Index = FindNLSString(EN_US, FIND_STARTSWITH, Haystack, 11, L"Hello", 5, NULL);
    ok(Index == 0, "STARTSWITH: Index = %d, expected 0\n", Index);
    Index = FindNLSString(EN_US, FIND_ENDSWITH, Haystack, 11, L"World", 5, NULL);
    ok(Index == 6, "ENDSWITH: Index = %d, expected 6\n", Index);
    Index = FindNLSString(EN_US, FIND_FROMEND, Haystack, 11, L"o", 1, NULL);
    ok(Index == 7, "FROMEND: Index = %d, expected 7\n", Index);

    /* Invalid input: no source, an empty value */
    SetLastError(0xDEADBEEF);
    Index = FindNLSString(EN_US, FIND_FROMSTART, NULL, -1, L"World", -1, NULL);
    ok(Index == -1, "NULL source: Index = %d, expected -1\n", Index);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "NULL source: error %lu\n", GetLastError());

    SetLastError(0xDEADBEEF);
    Index = FindNLSString(EN_US, FIND_FROMSTART, Haystack, -1, NULL, -1, NULL);
    ok(Index == -1, "NULL value: Index = %d, expected -1\n", Index);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "NULL value: error %lu\n", GetLastError());

    SetLastError(0xDEADBEEF);
    Index = FindNLSString(EN_US, FIND_FROMSTART, Haystack, -1, L"World", 0, NULL);
    ok(Index == -1, "Empty value: Index = %d, expected -1\n", Index);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "Empty value: error %lu\n", GetLastError());
}
