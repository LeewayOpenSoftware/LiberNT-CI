/*
 * PROJECT:     LiberNT kernel32 API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     IsValidLocaleName answers from the real locale table, not with TRUE
 */

#include "precomp.h"

typedef BOOL (WINAPI *PFN_ISVALIDLOCALENAME)(LPCWSTR);

START_TEST(IsValidLocaleName)
{
    PFN_ISVALIDLOCALENAME pIsValidLocaleName;
    static const PCWSTR Valid[] = { L"en-US", L"es-ES", L"fr-FR", L"de-DE", L"ja-JP" };
    static const PCWSTR Invalid[] = { L"xx-YY", L"en-", L"-US", L"en_US_x", L"zzzzzzzz",
                                      L"not a locale", L"en-US-garbage-name" };
    ULONG i;

    pIsValidLocaleName = (PFN_ISVALIDLOCALENAME)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                               "IsValidLocaleName");
    ok(pIsValidLocaleName != NULL, "kernel32!IsValidLocaleName is not exported\n");
    if (!pIsValidLocaleName)
        return;

    for (i = 0; i < ARRAYSIZE(Valid); i++)
        ok(pIsValidLocaleName(Valid[i]), "%ls should be a valid locale name\n", Valid[i]);

    for (i = 0; i < ARRAYSIZE(Invalid); i++)
        ok(!pIsValidLocaleName(Invalid[i]), "%ls should not be a valid locale name\n", Invalid[i]);
}
