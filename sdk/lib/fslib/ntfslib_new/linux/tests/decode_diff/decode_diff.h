/*
 * PROJECT:     LiberNT NTFS library host tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     C ABI of an on-disk decoder implementation under differential test
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Every implementation (the library, the spec model, an alternative such as a
 * Rust port) fills one table. A NULL entry means "not implemented" and is
 * skipped. The contracts are the library's, stated in decode_diff.md.
 */

#ifndef NTFS_DECODE_DIFF_H
#define NTFS_DECODE_DIFF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NTFS_DIFF_SUCCESS ((int32_t)0x00000000)
#define NTFS_DIFF_INVALID_PARAMETER ((int32_t)0xC000000D)
#define NTFS_DIFF_NO_MEMORY ((int32_t)0xC000009A)
#define NTFS_DIFF_FILE_CORRUPT ((int32_t)0xC0000102)

typedef int32_t (*ntfs_diff_run_sink)(void *context, uint64_t lcn, uint64_t length, uint8_t is_sparse);

typedef struct ntfs_decode_impl
{
    const char *name;

    /* Mapping pairs of the nonresident attribute record at attr; reads at most
     * avail bytes. Each run goes to sink in order. */
    int32_t (*decode_mapping_pairs)(const uint8_t *attr, uint32_t avail, uint64_t clusters_in_volume,
                                    ntfs_diff_run_sink sink, void *context);

    /* Update sequence fixup of a FILE/INDX record, in place. A bytes_per_sector
     * below 512 or not a power of two is an invalid parameter. The buffer is
     * unspecified after a failure. */
    int32_t (*apply_fixup)(uint8_t *record, uint32_t record_size, uint32_t bytes_per_sector);

    /* Directory ($I30) index entry at entry, with remaining bytes up to the end of
     * the entry area. Returns 1 when the entry may be used, 0 otherwise. */
    int (*index_entry_valid)(const uint8_t *entry, uint32_t remaining);
} ntfs_decode_impl;

const ntfs_decode_impl *ntfs_lib_impl(void);
const ntfs_decode_impl *ntfs_model_impl(void);

/* Provided by an alternative implementation linked with -DNTFSLIB_DIFF_ALT=<lib> */
const ntfs_decode_impl *ntfs_alt_impl(void);

#ifdef __cplusplus
}
#endif

#endif
