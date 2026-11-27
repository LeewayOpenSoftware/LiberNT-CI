/*
 * PROJECT:     LiberNT Winlogon and Userinit
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Shared names for the explorer->winlogon desktop switch
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

/* Set by explorer once the desktop has painted and the taskbar is open; in host mode winlogon
 * switches desktops on it, or after HKLM Winlogon\DelayedDesktopSwitchTimeout seconds. */
#define LIBERNT_SHELL_DESKTOP_SWITCH_EVENT L"ShellDesktopSwitchEvent"
#define LIBERNT_DESKTOP_SWITCH_TIMEOUT_DEFAULT_S 30
