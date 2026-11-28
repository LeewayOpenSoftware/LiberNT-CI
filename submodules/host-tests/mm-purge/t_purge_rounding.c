/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later
 * PURPOSE:     Regression guard for MiSegmentPurge page-range selection (defect A-2), both modes.
 *              COHERENCY (DiscardDirty=FALSE): outward rounding - must reach the partial first
 *              and last pages (a sub-page non-cached write must be purged).
 *              DISCARD   (DiscardDirty=TRUE, truncate/teardown): start rounded UP - the partial
 *              boundary page is KEPT (its pre-boundary bytes stay valid; the FS zeroes the tail),
 *              and pages wholly at/beyond the boundary are dropped. Guards the arithmetic of both
 *              contracts without linking the MM.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include <stdio.h>
#include <stdint.h>

#define PAGE_SIZE   4096ULL
#define PAGE_SHIFT  12
#define ALIGN_UP(a) (((a) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define SEGPAGES    8ULL   /* model an 8-page segment for Length==0 cases */

/* Mirrors MiSegmentPurge's First/Last selection. */
static void select(int discard, uint64_t off, uint64_t len, uint64_t *first, uint64_t *last)
{
    if (discard)
        *first = ALIGN_UP(off) >> PAGE_SHIFT;
    else
        *first = off >> PAGE_SHIFT;

    if (len == 0)
        *last = SEGPAGES;
    else if (discard)
        *last = (off + len) >> PAGE_SHIFT;
    else
        *last = ALIGN_UP(off + len) >> PAGE_SHIFT;
    if (*last > SEGPAGES) *last = SEGPAGES;
}

static int fails = 0;
static void chk(const char *name, int discard, uint64_t off, uint64_t len,
                uint64_t efirst, uint64_t elastExcl)
{
    uint64_t f, l; select(discard, off, len, &f, &l);
    int ok = (f == efirst) && (l == elastExcl);
    printf("  %-22s %-9s off=%-6llu len=%-6llu [%llu,%llu)  %s\n",
           name, discard ? "DISCARD" : "COHEREN", (unsigned long long)off, (unsigned long long)len,
           (unsigned long long)f, (unsigned long long)l, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    printf("MiSegmentPurge page selection (A-2, both modes)\n");
    /* Coherency: reach partial pages. */
    chk("coh 512B@0",        0, 0,    512,  0, 1);
    chk("coh partial-mid",   0, 4196, 200,  1, 2);
    chk("coh cross-bound",   0, 4000, 200,  0, 2);
    chk("coh partial-last",  0, 0,    4097, 0, 2);
    /* Discard (truncate): keep the partial boundary page, drop everything wholly beyond. */
    chk("disc trunc@5000",   1, 5000, 0,    2, SEGPAGES); /* boundary in page1 -> keep page1, drop 2..7 */
    chk("disc trunc@4096",   1, 4096, 0,    1, SEGPAGES); /* aligned -> drop from page1 */
    chk("disc trunc@0",      1, 0,    0,    0, SEGPAGES); /* whole segment */
    chk("disc @6000",        1, 6000, 0,    2, SEGPAGES); /* boundary in page1 (4096..8192) -> keep page1 */
    /* Contrast: same unaligned boundary, coherency reaches the partial page (would purge it). */
    chk("coh @5000..end",    0, 5000, (SEGPAGES*4096)-5000, 1, SEGPAGES);
    printf("RESULT: %s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails==1?"":"s");
    return fails ? 1 : 0;
}
