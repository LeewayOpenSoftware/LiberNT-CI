/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later
 * PURPOSE:     Regression guard for MiSegmentPurge page-range selection (defect A-2).
 *              MiSegmentPurge must purge EVERY page that overlaps [Offset, Offset+Length)
 *              (outward rounding). The pre-fix code rounded INWARD
 *              (First = ALIGN_UP(Offset), Last = (Offset+Length) >> PAGE_SHIFT), so a
 *              partial first/last page or a sub-page range was never purged yet the
 *              function still returned TRUE -> stale cached/mapped data after a
 *              non-cached write. Guards the rounding contract without linking the MM.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include <stdio.h>
#include <stdint.h>

#define PAGE_SIZE   4096ULL
#define PAGE_SHIFT  12
#define ALIGN_UP(a) (((a) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

/* NEW (fixed): outward rounding. */
static void sel_new(uint64_t off, uint64_t len, uint64_t *first, uint64_t *last)
{
    *first = off >> PAGE_SHIFT;
    *last  = ALIGN_UP(off + len) >> PAGE_SHIFT;
}
/* OLD (buggy): inward rounding. */
static void sel_old(uint64_t off, uint64_t len, uint64_t *first, uint64_t *last)
{
    *first = ALIGN_UP(off) >> PAGE_SHIFT;
    *last  = (off + len) >> PAGE_SHIFT;
}

static int fails = 0;

static void check(const char *name, uint64_t off, uint64_t len,
                  uint64_t efirst, uint64_t elast, int old_should_miss)
{
    uint64_t nf, nl, of, ol;
    sel_new(off, len, &nf, &nl);
    sel_old(off, len, &of, &ol);

    int new_ok = (nf == efirst) && (nl == elast + 1);           /* new covers exactly [efirst,elast] */
    int old_covers = (of <= efirst) && (ol >= elast + 1);       /* does old cover every expected page? */
    int old_missed = !old_covers;

    printf("  %-16s off=%-6llu len=%-6llu  NEW=[%llu,%llu) OLD=[%llu,%llu)  %s",
           name, (unsigned long long)off, (unsigned long long)len,
           (unsigned long long)nf, (unsigned long long)nl,
           (unsigned long long)of, (unsigned long long)ol,
           new_ok ? "new:PASS" : "new:FAIL");
    if (!new_ok) fails++;
    if (old_should_miss) {
        printf("  old-missed:%s", old_missed ? "YES(expected)" : "NO(UNEXPECTED)");
        if (!old_missed) fails++;
    }
    printf("\n");
}

int main(void)
{
    printf("MiSegmentPurge outward-rounding regression (A-2)\n");
    check("512B@0",        0,      512,  0, 0, 1);  /* sub-page: new {0}, old {} */
    check("partial-mid",   0x1064, 200,  1, 1, 1);  /* page 1 only: new {1}, old {} */
    check("cross-bound",   4000,   200,  0, 1, 1);  /* p0 tail + p1 head: new {0,1}, old {} */
    check("partial-last",  0,      4097, 0, 1, 1);  /* p0 + 1B of p1: new {0,1}, old missed p1 */
    check("aligned-1page", 0,      4096, 0, 0, 0);  /* exactly {0}; no over-purge */
    check("aligned-2page", 4096,   8192, 1, 2, 0);  /* {1,2} */
    printf("RESULT: %s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails==1?"":"s");
    return fails ? 1 : 0;
}
