/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Records whether the loader lock is held at static-import process attach
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <ndk/rtlfuncs.h>

/* 0 = attach not seen, 1 = lock held, 2 = lock not held */
static DWORD StateAtAttach;

DWORD WINAPI LdrLockStateAtAttach(VOID)
{
    return StateAtAttach;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
        StateAtAttach = RtlIsCriticalSectionLockedByThread(NtCurrentPeb()->LoaderLock) ? 1 : 2;
    return TRUE;
}
