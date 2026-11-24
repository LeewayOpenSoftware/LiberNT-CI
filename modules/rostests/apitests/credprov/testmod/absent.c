/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Dependency that is built but never installed, so its importers fail to load
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windef.h>

DWORD WINAPI
CredProvAbsent(void)
{
    return 0;
}
