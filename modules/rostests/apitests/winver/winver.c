/*
 * PROJECT:     Orosi API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test winver's brand acceptance (Orosi and legacy LiberNT)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windows.h>

#define STRSAFE_INLINE 1
#include <strsafe.h>

#include "../../../base/applications/winver/brand.h"

static void
expect_brand(
    _In_ PCWSTR input,
    _In_ PCWSTR expected)
{
    WCHAR szName[64];

    StringCchCopyW(szName, _countof(szName), input);
    Winver_ApplyBrand(szName, _countof(szName));
    ok(lstrcmpW(szName, expected) == 0,
       "Winver_ApplyBrand(%s) -> %s, expected %s\n",
       wine_dbgstr_w(input), wine_dbgstr_w(szName), wine_dbgstr_w(expected));
}

void func_winver_brand(void)
{
    /* Empty and legacy ReactOS names get the brand. */
    expect_brand(L"", WINVER_BRAND_NAME);
    expect_brand(L"ReactOS", WINVER_BRAND_NAME);

    /* The current product name and the legacy LiberNT name are both
       accepted for branding (as the upgrade detection does). */
    expect_brand(L"Orosi", WINVER_BRAND_NAME);
    expect_brand(L"LiberNT", WINVER_BRAND_NAME);

    /* Case-insensitive acceptance. */
    expect_brand(L"orosi", WINVER_BRAND_NAME);
    expect_brand(L"LIBERNT", WINVER_BRAND_NAME);

    /* Unrelated names are left alone. */
    expect_brand(L"Contoso", L"Contoso");
}
