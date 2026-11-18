/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Kernel power requests (PoCreate/Set/Clear/DeletePowerRequest), internal interface
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

/* Number of kernel power requests that currently hold the given type (0 for an unknown type) */
ULONG
NTAPI
PopQueryKernelPowerRequestCount(
    _In_ POWER_REQUEST_TYPE Type);

/* ES_SYSTEM_REQUIRED (and the away-mode bit 0x40) contributed by kernel power requests */
EXECUTION_STATE
NTAPI
PopKernelPowerRequestExecutionState(VOID);

/* TRUE while a kernel power request keeps the system out of idle sleep */
BOOLEAN
NTAPI
PopKernelPowerRequestBlocksSleep(VOID);

/* TRUE while a kernel power request asks that sleep go to away mode instead */
BOOLEAN
NTAPI
PopKernelPowerRequestAwayMode(VOID);
