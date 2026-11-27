/*
 * PROJECT:     ReactOS Setup Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Bootloader support functions
 * COPYRIGHT:   Copyright 2017-2024 Hermès Bélusca-Maïto <hermes.belusca-maito@reactos.org>
 */

#pragma once

#define BOOT_OPTION_AFTER_WINDOWS   0x100
#define BOOT_OPTION_KEEP_ORDER      0x200
#define BOOT_OPTION_ADD_LAST        0x400 /* a new firmware boot entry goes last, not first */
#define BOOT_OPTION_NO_FIRMWARE_ENTRY 0x800 /* leave the firmware boot entries alone */

NTSTATUS
NTAPI
InstallBootManagerAndBootEntries(
    _In_ ARCHITECTURE_TYPE ArchType,
    _In_ PCUNICODE_STRING SystemRootPath,
    _In_ PCUNICODE_STRING SourceRootPath,
    _In_ PCUNICODE_STRING DestinationArcPath,
    _In_ ULONG_PTR Options);

NTSTATUS
NTAPI
InstallSystemBootFiles(
    _In_ PCUNICODE_STRING SystemRootPath,
    _In_ PCUNICODE_STRING SystemVolume,
    _In_ PCWSTR SystemDirectory,
    _In_opt_ PCWSTR Locale,
    _In_opt_ PCWSTR Description,
    _In_ ULONG_PTR Options,
    _Out_opt_ PBOOLEAN FallbackKept);


/* EOF */
