/*
 * PROJECT:     LiberNT NT User-Mode DLL
 * PURPOSE:     Placeholder compatibility mode of the process and the thread
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <ntdll.h>

#if (NTDDI_VERSION >= NTDDI_WIN10)

/*
 * The process mode is kept in the PEB and the thread mode in the TEB.
 * A process starts with a zero PEB value, which reads as the documented
 * process default, PHCM_DISGUISE_PLACEHOLDER. A thread starts with
 * PHCM_APPLICATION_DEFAULT, meaning the process mode applies.
 */

CHAR
NTAPI
RtlQueryProcessPlaceholderCompatibilityMode(VOID)
{
    PPEB Peb = NtCurrentPeb();
    CHAR Mode;

    if (!Peb)
        return PHCM_ERROR_NO_PEB;

    Mode = Peb->PlaceholderCompatibilityMode;
    if (Mode == PHCM_APPLICATION_DEFAULT)
        Mode = PHCM_DISGUISE_PLACEHOLDER;
    return Mode;
}

CHAR
NTAPI
RtlSetProcessPlaceholderCompatibilityMode(
    _In_ CHAR Mode)
{
    PPEB Peb = NtCurrentPeb();
    CHAR Previous;

    if (Mode < PHCM_DISGUISE_PLACEHOLDER || Mode > PHCM_MAX)
        return PHCM_ERROR_INVALID_PARAMETER;
    if (!Peb)
        return PHCM_ERROR_NO_PEB;

    Previous = RtlQueryProcessPlaceholderCompatibilityMode();
    Peb->PlaceholderCompatibilityMode = Mode;
    return Previous;
}

CHAR
NTAPI
RtlQueryThreadPlaceholderCompatibilityMode(VOID)
{
    PTEB Teb = NtCurrentTeb();

    if (!Teb)
        return PHCM_ERROR_NO_TEB;

    return Teb->PlaceholderCompatibilityMode;
}

CHAR
NTAPI
RtlSetThreadPlaceholderCompatibilityMode(
    _In_ CHAR Mode)
{
    PTEB Teb = NtCurrentTeb();
    CHAR Previous;

    if (Mode < PHCM_APPLICATION_DEFAULT || Mode > PHCM_MAX)
        return PHCM_ERROR_INVALID_PARAMETER;
    if (!Teb)
        return PHCM_ERROR_NO_TEB;

    Previous = Teb->PlaceholderCompatibilityMode;
    Teb->PlaceholderCompatibilityMode = Mode;
    return Previous;
}

#endif /* NTDDI_VERSION >= NTDDI_WIN10 */
