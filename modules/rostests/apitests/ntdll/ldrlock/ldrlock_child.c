/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Child of LdrInitLoaderLock; its exit code is ldrlock_dll's attach-time lock state
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windef.h>

DWORD WINAPI LdrLockStateAtAttach(VOID);

int main(void)
{
    return (int)LdrLockStateAtAttach();
}
