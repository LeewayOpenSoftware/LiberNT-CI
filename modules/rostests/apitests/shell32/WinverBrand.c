/* ReactOS API tests. LGPL-2.1-or-later. */
#include <apitest.h>
#include <windows.h>
#include <strsafe.h>

#include "../../../../base/applications/winver/brand.c"

static void check_brand(PCWSTR in, PCWSTR expected)
{
    WCHAR name[64];

    StringCchCopyW(name, _countof(name), in);
    Winver_ApplyBrand(name, _countof(name));
    ok(lstrcmpW(name, expected) == 0, "'%S' mapped to '%S', expected '%S'\n", in, name, expected);
}

START_TEST(WinverBrand)
{
    check_brand(L"LiberNT", L"Orosi");
    check_brand(L"ReactOS", L"Orosi");
    check_brand(L"liberNT", L"Orosi");
    check_brand(L"", L"Orosi");
    check_brand(L"Orosi", L"Orosi");
    check_brand(L"Contoso OS", L"Contoso OS");
}
