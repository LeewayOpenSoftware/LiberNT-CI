/*
 * PROJECT:     LiberNT
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Boot Configuration Data store on-disk / registry layout constants, shared by the
 *              boot loader (bootloader/lib/bcd.c) and the bcdedit tool so the two read the same
 *              element format, device layout and device types from one source.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

/* An element's value format is bits 24-27 of its identifier. */
#define BCD_ELEMENT_FORMAT(e)          (((e) >> 24) & 0xF)
#define BCD_ELEMENT_FORMAT_DEVICE      1
#define BCD_ELEMENT_FORMAT_STRING      2
#define BCD_ELEMENT_FORMAT_OBJECT      3
#define BCD_ELEMENT_FORMAT_OBJECTLIST  4
#define BCD_ELEMENT_FORMAT_INTEGER     5
#define BCD_ELEMENT_FORMAT_BOOLEAN     6
#define BCD_ELEMENT_FORMAT_INTEGERLIST 7

/* A device element is a 0x10-byte header followed by the device body; offsets are into the
 * element value as stored. */
#define BCD_ELEMENT_DEVICE_OFFSET   0x10
#define BCD_DEVICE_SIZE_OFFSET      0x08
#define BCD_DEVICE_PARTITION_OFFSET 0x10
#define BCD_DEVICE_LOCAL_OFFSET     0x20
#define BCD_DEVICE_STYLE_OFFSET     0x24
#define BCD_DEVICE_DISK_OFFSET      0x28
#define BCD_DEVICE_SIGNATURE_OFFSET 0x28
#define BCD_DEVICE_HEADER_LENGTH    0x10
#define BCD_DEVICE_PARTITION_LENGTH 0x48
#define BCD_DISK_TYPE_OFFSET        0x10
#define BCD_DISK_FILE_OFFSET        0x28
#define BCD_FILE_LENGTH_OFFSET      0x04
#define BCD_FILE_TYPE_OFFSET        0x08
#define BCD_FILE_DEVICE_OFFSET      0x0C

/* Device types (the first dword of the device body). */
#define BCD_DEVICE_DISK      0
#define BCD_DEVICE_BOOT      5
#define BCD_DEVICE_PARTITION 6

/* Disk device sub-types (BCD_DISK_TYPE_OFFSET within a DISK device body). */
#define BCD_DISK_RAMDISK     3

/* File device sub-types (BCD_FILE_TYPE_OFFSET). */
#define BCD_FILE_VERSION     1
#define BCD_FILE_PATH        5

/* Partition styles (BCD_DEVICE_STYLE_OFFSET). */
#define BCD_PARTITION_GPT    0
#define BCD_PARTITION_MBR    1
