/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Power Manager: effective power mode notifications, user shutdown state
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#pragma once

/* Values of EFFECTIVE_POWER_MODE (powersetting.h) */
#define PO_EFFECTIVE_POWER_MODE_BATTERY_SAVER    0
#define PO_EFFECTIVE_POWER_MODE_BETTER_BATTERY   1
#define PO_EFFECTIVE_POWER_MODE_BALANCED         2
#define PO_EFFECTIVE_POWER_MODE_HIGH_PERFORMANCE 3
#define PO_EFFECTIVE_POWER_MODE_MAX_PERFORMANCE  4
#define PO_EFFECTIVE_POWER_MODE_GAME_MODE        5
#define PO_EFFECTIVE_POWER_MODE_MIXED_REALITY    6

#define PO_EFFECTIVE_POWER_MODE_V1 1
#define PO_EFFECTIVE_POWER_MODE_V2 2

typedef
VOID
(NTAPI *PPO_EFFECTIVE_POWER_MODE_CALLBACK)(
    _In_ ULONG Mode,
    _In_opt_ PVOID Context);

NTSTATUS
NTAPI
PoRegisterForEffectivePowerModeNotifications(
    _In_ ULONG Version,
    _In_ PPO_EFFECTIVE_POWER_MODE_CALLBACK Callback,
    _In_opt_ PVOID Context,
    _Outptr_ PVOID *RegistrationHandle);

NTSTATUS
NTAPI
PoUnregisterFromEffectivePowerModeNotifications(
    _In_ PVOID RegistrationHandle);

/* Internal: the policy code reports the mode it settled on; every registrant is told */
VOID
NTAPI
PopSetEffectivePowerMode(
    _In_ ULONG Mode);

ULONG
NTAPI
PopGetEffectivePowerMode(VOID);

VOID
NTAPI
PoUserShutdownInitiated(VOID);

VOID
NTAPI
PoUserShutdownCancelled(VOID);

BOOLEAN
NTAPI
PopIsUserShutdownInProgress(VOID);

BOOLEAN
NTAPI
PoEnergyEstimationEnabled(VOID);
