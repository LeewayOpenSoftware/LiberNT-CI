/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Power framework (PoFx) services for the rest of the power manager
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

/* Target DRIPS device power state a driver set with PoFxSetTargetDripsDevicePowerState
 * for the device behind Pdo; PowerDeviceUnspecified when none is set or the device is
 * not registered with PoFx. */
DEVICE_POWER_STATE
NTAPI
PopFxGetDripsTargetState(
    _In_ PDEVICE_OBJECT Pdo);
