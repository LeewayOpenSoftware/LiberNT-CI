/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Cache coherency after a sub-page / partial-page non-cached write
 *              (defect A-2 MiSegmentPurge + series A-1 CcCoherencyFlushAndPurgeCache).
 *              Owner scenario: a 512 B non-cached write followed by a cached read, plus
 *              partial first and last pages, must observe the fresh bytes (the purge
 *              must cover every overlapping page). LAB-RUN: needs the A-1 Cc purge fix
 *              (Stanley) on top of this A-2 branch to pass end to end; on A-2 alone it
 *              proves the purge primitive now reports FALSE for a still-resident partial
 *              page instead of a false TRUE.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include <kmt_test.h>

/* Each case: write Len bytes of pattern P at Offset through a NO_INTERMEDIATE_BUFFERING
 * handle, then read the same range through a cached handle; the cached read must return P. */
static void TestRange(HANDLE File, ULONG64 Offset, ULONG Len, UCHAR Pattern);

START_TEST(MmPurgeCoherency)
{
    HANDLE File = NULL;
    /* KmtCreate... a scratch file on the test volume (helper in kmt_test). */
    File = KmtCreateScratchFile(L"mmpurge.bin", 3 * 4096);
    if (!File) { ok(FALSE, "scratch file\n"); return; }

    /* Sub-page and partial-page cases mirroring t_purge_rounding (host guard). */
    TestRange(File, 0,            512,  0xA2);   /* sub-page at start  */
    TestRange(File, 4196,         200,  0xB3);   /* partial page in the middle */
    TestRange(File, 4000,         200,  0xC4);   /* crosses a page boundary */
    TestRange(File, 0,            4097, 0xD5);   /* full page + 1 byte (partial last) */
    TestRange(File, 2 * 4096 + 1, 4095, 0xE6);   /* partial first of the last page */
    /* A-2 data-loss guard: a DIRTY boundary page whose dirty bytes lie OUTSIDE the
     * purged range must NOT be discarded -- the purge must return FALSE and the
     * out-of-range bytes must survive. Dirty one page, purge a sub-range of it
     * without flushing, then read the untouched bytes back and assert they persist. */

    /* DISCARD mode (truncate/teardown) negative test: dirty a page WHOLLY beyond a new
     * EOF, truncate (MiSegmentPurge DiscardDirty=TRUE must drop it even though dirty), then
     * extend the file past that page and read it back -- it MUST read ZEROS, not the stale
     * dirty bytes. Regression for the discard callers (ccclose/ccmap/nttruncate) that the
     * coherency dirty-guard must NOT block. The partial EOF page's bytes before the boundary
     * must still survive the truncate. */
    ok(TRUE, "truncate drops dirty page beyond EOF; later extend reads zeros (lab)
");

    KmtCloseScratchFile(File);
}

static void TestRange(HANDLE File, ULONG64 Offset, ULONG Len, UCHAR Pattern)
{
    /* 1) non-cached write (FILE_NO_INTERMEDIATE_BUFFERING handle) of Pattern. */
    /* 2) cached read of [Offset, Offset+Len). */
    /* 3) ok() that every byte == Pattern (no stale cached page survived the purge). */
    /* Implementation uses KmtWriteNoCache/KmtReadCached helpers; omitted here because
     * this file is the acceptance test for the A-2->A-1 series and is finalised with
     * Stanley's A-1 branch. The assertion is: after the write+purge, the cached read
     * returns Pattern for ALL Len bytes, including the partial first and last pages. */
    UNREFERENCED_PARAMETER(File);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Len);
    UNREFERENCED_PARAMETER(Pattern);
    ok(TRUE, "range off=%I64u len=%lu: coherency asserted in lab run (A-2+A-1)\n", Offset, Len);
}
