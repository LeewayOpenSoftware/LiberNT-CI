/*
 * PROJECT:     ReactOS Version Program
 * PURPOSE:     Operating system identity shown by the About dialog
 */

#include <windef.h>
#include <winbase.h>
#include <strsafe.h>

#include "brand.h"

VOID
Winver_ApplyBrand(
    _Inout_updates_(cchName) PWSTR szName,
    _In_ SIZE_T cchName)
{
    if (!szName[0] ||
        lstrcmpiW(szName, L"ReactOS") == 0 ||
        lstrcmpiW(szName, L"Orosi") == 0 ||
        lstrcmpiW(szName, L"LiberNT") == 0)
    {
        StringCchCopyW(szName, cchName, WINVER_BRAND_NAME);
    }
}
