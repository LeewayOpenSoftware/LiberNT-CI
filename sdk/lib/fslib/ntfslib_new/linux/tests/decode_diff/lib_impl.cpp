/*
 * PROJECT:     LiberNT NTFS library host tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The library's own decoders behind the differential-test ABI
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntfslib_new.h>
#include "ntfslib_new_internal.h"

#include "decode_diff.h"

namespace
{

struct SinkAdapter
{
    ntfs_diff_run_sink Sink;
    void *Context;
};

NTSTATUS
ForwardRun(PVOID Context, ULONGLONG Lcn, ULONGLONG Length, BOOLEAN IsSparse)
{
    SinkAdapter *Adapter = static_cast<SinkAdapter *>(Context);

    return Adapter->Sink(Adapter->Context, Lcn, Length, IsSparse ? 1 : 0);
}

int32_t
LibMappingPairs(const uint8_t *attr, uint32_t avail, uint64_t clusters, ntfs_diff_run_sink sink, void *context)
{
    SinkAdapter Adapter = { sink, context };

    return NtfsDecodeMappingPairs(attr, avail, clusters, sink ? ForwardRun : NULL, &Adapter);
}

int32_t
LibApplyFixup(uint8_t *record, uint32_t size, uint32_t bps)
{
    return NtfsApplyFixup(reinterpret_cast<PNTFSRecordHeader>(record), size, bps);
}

int
LibIndexEntryValid(const uint8_t *entry, uint32_t remaining)
{
    return NtfsIsDirectoryIndexEntryValid(reinterpret_cast<PIndexEntry>(const_cast<uint8_t *>(entry)),
                                          remaining) ? 1 : 0;
}

const ntfs_decode_impl Lib =
{
    "ntfslib",
    LibMappingPairs,
    LibApplyFixup,
    LibIndexEntryValid,
};

}

extern "C" const ntfs_decode_impl *
ntfs_lib_impl(void)
{
    return &Lib;
}
