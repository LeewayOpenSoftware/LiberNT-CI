#ifndef WINVER_BRAND_H
#define WINVER_BRAND_H

#define WINVER_BRAND_NAME L"Orosi"

VOID
Winver_ApplyBrand(
    _Inout_updates_(cchName) PWSTR szName,
    _In_ SIZE_T cchName);

#endif
