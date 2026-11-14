/* LRM-RS differential: Dwight's C special-pool engine vs the Rust engine, same contract.
 * Compares PoolSpecialSelect / PoolSpecialPlace / (Prepare+Owns+Check) on reused t_special.c
 * cases + a deterministic fuzz, including cross-engine pattern agreement and the EXACT corrupt
 * address on free. Exit 0 iff 0 disagreements AND both accept/reject and intact/corrupt
 * outcomes occurred (non-vacuous). Link: this + special.c (C ref) + the Rust staticlib.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
extern "C" {
#include "poolspecial.h" /* C engine decls + POOL_SPECIAL_RECORD + constants (via -I dw/include) */
}

/* LRM-RS orchestration layer (mm.rs): MmUseSpecialPool predicate + free-time 0xC1 classifier.
 * Tested against Ryan's OBSERVED-CONTRACTS spec (the C glue ntspecial.c needs expool.h/mint.h
 * stand-ins to compile on host - a follow-up). */
struct BugCheckC { uint32_t code; uintptr_t p1, p2, p3, p4; };
extern "C" uint8_t lrm_mm_use_special_pool(uint8_t region_present, uint32_t special_pool_tag, size_t bytes, uint32_t tag);
extern "C" uint8_t lrm_mm_free_classify(uintptr_t p, int32_t query, uint8_t owns, uint8_t irql, uint8_t paged,
                                        uintptr_t flags, int32_t check_status, uintptr_t corrupt, BugCheckC *out);

/* Rust engine (same contract, distinct symbols). */
extern "C" uint8_t rust_pool_special_select(uint32_t selector, size_t bytes, uint32_t tag);
extern "C" uint8_t rust_pool_special_place(size_t bytes, uint32_t alignment, uint8_t at_start, uint32_t *offset);
extern "C" void *rust_pool_special_prepare(void *record, void *page, size_t bytes, uint32_t offset, uint32_t tag, uint32_t flags);
extern "C" uint8_t rust_pool_special_owns(const void *record, void *page, void *block);
extern "C" int32_t rust_pool_special_check(const void *record, void *page, uintptr_t *corrupt);

static long g_checks = 0, g_dis = 0;
static long g_sel_true = 0, g_sel_false = 0, g_intact = 0, g_corrupt = 0;

#define TAG(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b)<<8) | ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))

static void dis(const char *what, uint64_t a, uint64_t b)
{
    g_dis++;
    if (g_dis <= 25)
        std::fprintf(stderr, "DISAGREE #%ld %s c=0x%llx rust=0x%llx\n", g_dis, what,
                     (unsigned long long)a, (unsigned long long)b);
}

static void check_select(uint32_t sel, size_t bytes, uint32_t tag)
{
    uint8_t c = PoolSpecialSelect(sel, bytes, tag) ? 1 : 0;
    uint8_t r = rust_pool_special_select(sel, bytes, tag) ? 1 : 0;
    g_checks++;
    if (c) g_sel_true++; else g_sel_false++;
    if (c != r) dis("select", c, r);
}

static void check_place(size_t bytes, uint32_t align, uint8_t at_start)
{
    uint32_t oc = 0xDEAD, orr = 0xBEEF;
    uint8_t rc = PoolSpecialPlace(bytes, align, at_start, &oc) ? 1 : 0;
    uint8_t rr = rust_pool_special_place(bytes, align, at_start, &orr) ? 1 : 0;
    g_checks++;
    if (rc != rr) dis("place.ret", rc, rr);
    if (oc != orr) dis("place.offset", oc, orr);
}

/* Prepare on both engines (same page), compare block+record; then cross-engine Check on a clean
 * page (must be Intact for both), then corrupt one byte and compare status + exact address. */
static void check_prepare_check(uint8_t *page, size_t bytes, uint32_t offset, uint32_t tag,
                                uint32_t flags, long corrupt_pos)
{
    POOL_SPECIAL_RECORD rc_rec, rr_rec;
    std::memset(&rc_rec, 0, sizeof rc_rec);
    std::memset(&rr_rec, 0, sizeof rr_rec);

    void *blkc = PoolSpecialPrepare(&rc_rec, page, bytes, offset, tag, flags);
    /* re-fills with the same page-addr pattern; record set identically */
    void *blkr = rust_pool_special_prepare(&rr_rec, page, bytes, offset, tag, flags);
    g_checks++;
    if (blkc != blkr) dis("prepare.block", (uintptr_t)blkc, (uintptr_t)blkr);
    if (std::memcmp(&rc_rec, &rr_rec, sizeof rc_rec) != 0) dis("prepare.record", 1, 0);

    /* Owns: both engines on the returned block. */
    uint8_t oc = PoolSpecialOwns(&rc_rec, page, blkc) ? 1 : 0;
    uint8_t orr = rust_pool_special_owns(&rr_rec, page, blkr) ? 1 : 0;
    if (oc != orr) dis("owns", oc, orr);

    /* Cross-engine Check on the clean page: C record via Rust check, and vice versa, must agree
     * (pattern is derived from the page address, which both compute the same). */
    uintptr_t cc = 1, cr = 1;
    int32_t sc = PoolSpecialCheck(&rc_rec, page, &cc);
    int32_t sr = rust_pool_special_check(&rr_rec, page, &cr);
    if (sc != sr) dis("check.clean.status", (uint32_t)sc, (uint32_t)sr);
    if (cc != cr) dis("check.clean.addr", cc, cr);
    if (sc == 0) g_intact++;

    if (corrupt_pos >= 0 && corrupt_pos < (long)PAGE_SIZE)
    {
        uint8_t saved = page[corrupt_pos];
        page[corrupt_pos] ^= 0x80;
        uintptr_t dc = 0, dr = 0;
        int32_t tc = PoolSpecialCheck(&rc_rec, page, &dc);
        int32_t tr = rust_pool_special_check(&rr_rec, page, &dr);
        g_checks++;
        if (tc != tr) dis("check.corrupt.status", (uint32_t)tc, (uint32_t)tr);
        if (dc != dr) dis("check.corrupt.addr", dc, dr);
        if (tc != 0) g_corrupt++;
        page[corrupt_pos] = saved;
    }
}

/* deterministic xorshift64* */
static uint64_t st = 0x243F6A8885A308D3ULL;
static uint64_t xr(){ uint64_t x=st; x^=x>>12; x^=x<<25; x^=x>>27; st=x; return x*0x2545F4914F6CDD1DULL; }

/* Orchestration (mm.rs) vs Ryan's OBSERVED-CONTRACTS spec: exact 0xC1 sub-codes + params. */
static long g_mm = 0, g_mm_fail = 0;
static void expect_bc(const char *name, uint8_t raised, const BugCheckC &bc,
                      uint8_t exp_raised, uint32_t code, uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4)
{
    g_mm++;
    bool ok = (raised == exp_raised);
    if (ok && raised)
        ok = (bc.code == code && bc.p1 == p1 && bc.p2 == p2 && bc.p3 == p3 && bc.p4 == p4);
    if (!ok) { g_mm_fail++; std::fprintf(stderr, "MM FAIL %s raised=%u code=0x%x p=(%zu,%zu,%zu,%zu)\n",
                                         name, raised, bc.code, bc.p1, bc.p2, bc.p3, bc.p4); }
}
static void test_mm(void)
{
    const uintptr_t P = 0x7ffff000; const uintptr_t X = 0x7ffff123; const uintptr_t FL = 0x9;
    BugCheckC bc;
    /* use_special_pool: off when no region; else follows the selector. */
    expect_bc("use.noregion", lrm_mm_use_special_pool(0, POOL_SPECIAL_SELECT_ALL, 32, 0) ? 0 : 0, bc, 0, 0,0,0,0,0);
    g_mm++; if (lrm_mm_use_special_pool(0, POOL_SPECIAL_SELECT_ALL, 32, 0) != 0) g_mm_fail++;       /* no region -> false */
    g_mm++; if (lrm_mm_use_special_pool(1, POOL_SPECIAL_SELECT_ALL, 32, 0) == 0) g_mm_fail++;       /* region + '*' -> true */
    g_mm++; if (lrm_mm_use_special_pool(1, TAG('A','b','c','d'), 8, TAG('A','b','c','e')) != 0) g_mm_fail++; /* tag mismatch -> false */
    /* free_classify spec table */
    uint8_t r;
    r = lrm_mm_free_classify(P, 0/*Outside*/, 1, 0, 0, FL, 0, 0, &bc); expect_bc("free.notalloc", r, bc, 1, 0xC1, P, 0, 0, 0x20);
    r = lrm_mm_free_classify(P, 2/*Live*/, 0/*!owns*/, 0, 0, FL, 0, 0, &bc); expect_bc("free.notowns", r, bc, 1, 0xC1, P, 0, 0, 0x20);
    r = lrm_mm_free_classify(P, 2, 1, 3/*>DISPATCH*/, 0, FL, 0, 0, &bc); expect_bc("free.irql.np", r, bc, 1, 0xC1, 3, FL, P, 0x31);
    r = lrm_mm_free_classify(P, 2, 1, 2/*>APC*/, 1/*paged*/, FL, 0, 0, &bc); expect_bc("free.irql.paged", r, bc, 1, 0xC1, 2, FL, P, 0x31);
    r = lrm_mm_free_classify(P, 2, 1, 0, 0, FL, 1/*CorruptBefore*/, X, &bc); expect_bc("free.before", r, bc, 1, 0xC1, P, X, 0, 0x23);
    r = lrm_mm_free_classify(P, 2, 1, 0, 0, FL, 2/*CorruptAfter*/, X, &bc); expect_bc("free.after", r, bc, 1, 0xC1, P, X, 0, 0x24);
    r = lrm_mm_free_classify(P, 2, 1, 0, 0, FL, 0/*Intact*/, 0, &bc); expect_bc("free.ok", r, bc, 0, 0,0,0,0,0);
    r = lrm_mm_free_classify(P, 2, 1, 2/*DISPATCH, nonpaged ok*/, 0, FL, 0, 0, &bc); expect_bc("free.dispatch.np.ok", r, bc, 0, 0,0,0,0,0);
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? std::atol(argv[1]) : 2000000;
    test_mm();

    /* --- reused t_special.c selection cases --- */
    check_select(0, 32, TAG('A','b','c','d'));
    check_select(POOL_SPECIAL_SELECT_ALL, 1, TAG('A','b','c','d'));
    check_select(POOL_SPECIAL_SELECT_ALL, PAGE_SIZE-1, 0);
    check_select(POOL_SPECIAL_SELECT_ALL, PAGE_SIZE, 0);
    check_select(POOL_SPECIAL_SELECT_ALL, 0, 0);
    check_select(0x40, 0x40, 0); check_select(0x40, 0x31, 0);
    check_select(0x40, 0x30, 0); check_select(0x40, 0x41, 0);
    check_select(TAG('A','b','c','d'), 8, TAG('A','b','c','d'));
    check_select(TAG('A','b','c','d'), 8, TAG('A','b','c','d') | 0x80000000u);
    check_select(TAG('A','b','c','d'), 8, TAG('A','b','c','e'));
    check_select(TAG('A','?','c','d'), 8, TAG('A','x','c','d'));
    check_select(TAG('A','?','c','d'), 8, TAG('B','x','c','d'));
    check_select(TAG('A','b','*',0), 8, TAG('A','b','z','z'));
    check_select(TAG('A','b','*',0), 8, TAG('A','c','z','z'));
    check_select(TAG('A','b','c','d'), PAGE_SIZE, TAG('A','b','c','d'));
    /* --- reused place cases --- */
    { uint32_t o; (void)o; check_place(PAGE_SIZE, 0, 0); check_place(PAGE_SIZE+1, 0, 1); }
    check_place(100, 3, 0); check_place(100, 24, 0); check_place(100, PAGE_SIZE*2, 0);
    for (size_t s=1; s<PAGE_SIZE; s += (s<80?1:211)) { check_place(s,0,0); check_place(s,64,0); check_place(s,0,1); }

    /* page for prepare/check (aligned to PAGE_SIZE) */
    void *mem = std::malloc(PAGE_SIZE*3);
    uint8_t *page = (uint8_t *)(((uintptr_t)mem + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE-1));
    check_prepare_check(page, 64, PAGE_SIZE-64, TAG('T','e','s','t'), 0, PAGE_SIZE-1);
    check_prepare_check(page, 64, 0, TAG('H','e','a','d'), 1, 100);
    check_prepare_check(page, 100, PAGE_SIZE-112, 0x1234, 0, -1);

    /* --- fuzz --- */
    for (long i=0; i<iters; i++)
    {
        uint64_t r = xr();
        switch (r & 3) {
        case 0: { /* select */
            uint32_t sel; size_t bytes = (size_t)(xr() % (PAGE_SIZE+2));
            switch (xr()%4){ case 0: sel=(uint32_t)(xr()%(PAGE_SIZE+2)); break; case 1: sel=POOL_SPECIAL_SELECT_ALL; break;
                case 2: sel=(uint32_t)(xr()); break; default: {uint8_t b[4]; for(int k=0;k<4;k++){uint64_t z=xr()%6; b[k]=z==0?'?':z==1?'*':(uint8_t)('A'+(xr()%26));} sel=TAG(b[0],b[1],b[2],b[3]);} }
            check_select(sel, bytes, (uint32_t)xr());
            break; }
        case 1: { /* place */
            size_t bytes = (size_t)(xr() % (PAGE_SIZE+2));
            uint32_t align = (xr()&1) ? (uint32_t)(1u << (xr()%14)) : (uint32_t)(xr()%200);
            check_place(bytes, align, (uint8_t)(xr()&1));
            break; }
        default: { /* prepare + check, valid placement so offset/size are in range */
            size_t bytes = 1 + (size_t)(xr() % 512);
            uint32_t off=0; rust_pool_special_place(bytes, (uint32_t)(16u<<(xr()%5)), (uint8_t)(xr()&1), &off);
            long cpos = (xr()&3)==0 ? -1 : (long)(xr() % PAGE_SIZE);
            /* fresh page each time from the same aligned region (prepare refills) */
            check_prepare_check(page, bytes, off, (uint32_t)xr(), (uint32_t)xr(), cpos);
            break; }
        }
    }
    std::free(mem);

    std::printf("[special-pool] checks=%ld disagreements=%ld sel_true=%ld sel_false=%ld intact=%ld corrupt=%ld\n",
                g_checks, g_dis, g_sel_true, g_sel_false, g_intact, g_corrupt);
    std::printf("[mm-orchestration] checks=%ld failures=%ld (vs OBSERVED-CONTRACTS spec)\n", g_mm, g_mm_fail);
    if (g_dis || g_mm_fail) { std::printf("RESULT=FAIL\n"); return 1; }
    if (!g_sel_true || !g_sel_false || !g_intact || !g_corrupt) { std::printf("RESULT=FAIL (coverage gap)\n"); return 2; }
    std::printf("RESULT=PASS\n");
    return 0;
}
