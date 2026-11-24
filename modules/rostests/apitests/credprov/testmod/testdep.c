/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test provider that imports a DLL which is not installed
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windef.h>

DWORD WINAPI CredProvAbsent(void);

DWORD WINAPI
CredProvTestDep(void)
{
    return CredProvAbsent();
}
