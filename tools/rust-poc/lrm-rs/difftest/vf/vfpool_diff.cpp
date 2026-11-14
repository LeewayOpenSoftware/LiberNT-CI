/* LRM-RS Driver-Verifier Stage 5 FULL-STACK differential: the real vfpool.c (compiled UNMODIFIED
 * against a host <ntoskrnl.h> stub, over mock pool/Mm/Ke services) vs the Rust decisions + tracker
 * (lrm_vf_*). Drives, through the kernel's own call paths:
 *   - VfpCheckAllocate / VfpCheckFree  (0xC4 sub-codes 0x00/0x01/0x02 and 0x12/0x11), via the
 *     VfPoolThunks[] alloc/free wrappers, captured with setjmp/longjmp;
 *   - VfpSpecialType (priority -> placement) and VfpAllocatePool2 POOL_FLAGS decode, observed by a
 *     recording MmAllocateSpecialPool stub;
 *   - pool tracking counters (MmVerifierData) and the unload leak check VfCheckPoolAtUnload
 *     (0xC4 / 0x62), compared against the Rust PoolTracker driven in lockstep.
 * Exit 0 iff 0 disagreements AND coverage is non-vacuous; a negative control (perturbed Rust IRQL)
 * must disagree. Leeway Open Software. Test-only. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csetjmp>

extern "C" {
#include <ntoskrnl.h>
extern const VF_THUNK VfPoolThunks[];
extern const ULONG VfPoolThunkCount;
extern void VfInitializePoolTracking(void);
extern void VfUntrackPool(PVOID Block);
extern void VfCheckPoolAtUnload(PLDR_DATA_TABLE_ENTRY LdrEntry);
extern BOOLEAN VfPoolTrackingEnabled;
}

/* Rust ABI (vf.rs) */
struct BugCheckRs { uint32_t code; uintptr_t p1, p2, p3, p4; };
struct TrackEntryRs { uintptr_t block, caller; uint32_t size; uint8_t paged, used; };
struct VfTrackStateRs {
    uint32_t paged_bytes, nonpaged_bytes, cur_paged, cur_nonpaged,
             peak_paged, peak_nonpaged, peak_paged_bytes, peak_nonpaged_bytes, untracked;
};
extern "C" uint8_t lrm_vf_check_allocate(uint32_t pool_type, size_t bytes, uint8_t irql, BugCheckRs *out);
extern "C" uint8_t lrm_vf_check_free(uint8_t irql, uint8_t tracked_paged, uintptr_t block, BugCheckRs *out);
extern "C" uint32_t lrm_vf_special_type(uint32_t priority);
extern "C" uint8_t lrm_vf_pool2_decode(uint32_t flags, uint32_t tag, uint32_t *out_type, uint8_t *out_zero);
extern "C" void lrm_vf_track(VfTrackStateRs *st, TrackEntryRs *e, size_t cap, uintptr_t block, uint32_t size, uint8_t paged, uintptr_t caller);
extern "C" void lrm_vf_untrack(VfTrackStateRs *st, TrackEntryRs *e, size_t cap, uintptr_t block);
extern "C" uint8_t lrm_vf_is_tracked_paged(const TrackEntryRs *e, size_t cap, uintptr_t block);
extern "C" uint8_t lrm_vf_check_at_unload(const TrackEntryRs *e, size_t cap, uintptr_t name, uintptr_t start, uintptr_t end, BugCheckRs *out);

/* ---- C-side stubs ---- */
MM_DRIVER_VERIFIER_DATA MmVerifierData;
static jmp_buf g_jmp;
static BugCheckRs g_cbc;
static int g_craised;
static KIRQL g_irql;

struct SpecialRec { int called; POOL_TYPE pt; size_t bytes; ULONG tag; ULONG stype; };
static SpecialRec g_sp;
static int g_fallback2;   /* ExAllocatePool2 fallback hit */

extern "C" {
KIRQL KeGetCurrentIrql(void) { return g_irql; }
unsigned long DbgPrint(const char *, ...) { return 0; }
void KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4) {
    g_cbc = { (uint32_t)Code, (uintptr_t)P1, (uintptr_t)P2, (uintptr_t)P3, (uintptr_t)P4 };
    g_craised = 1; longjmp(g_jmp, 1);
}
PVOID MmAllocateSpecialPool(SIZE_T b, ULONG tag, POOL_TYPE pt, ULONG st) {
    g_sp.called = 1; g_sp.pt = pt; g_sp.bytes = b; g_sp.tag = tag; g_sp.stype = st;
    return NULL;   /* force the normal-allocator fallback so a block is still produced + tracked */
}
PVOID ExAllocatePool(POOL_TYPE, SIZE_T b) { return malloc(b ? b : 1); }           /* pristine addr only */
PVOID ExAllocatePoolWithQuota(POOL_TYPE, SIZE_T b) { return malloc(b ? b : 1); }   /* pristine addr only */
PVOID ExAllocatePoolWithTag(POOL_TYPE, SIZE_T b, ULONG) { return malloc(b ? b : 1); }
PVOID ExAllocatePoolWithQuotaTag(POOL_TYPE, SIZE_T b, ULONG) { return malloc(b ? b : 1); }
PVOID ExAllocatePoolWithTagPriority(POOL_TYPE, SIZE_T b, ULONG, EX_POOL_PRIORITY) { return malloc(b ? b : 1); }
PVOID ExAllocatePool2(POOL_FLAGS, SIZE_T b, ULONG) { g_fallback2 = 1; return malloc(b ? b : 1); }
PVOID ExAllocatePool3(POOL_FLAGS, SIZE_T b, ULONG, PCPOOL_EXTENDED_PARAMETER, ULONG) { return malloc(b ? b : 1); }
void ExFreePoolWithTag(PVOID, ULONG) {}
void ExFreePool(PVOID) {}
}

/* Thunk shapes (vfpool.c order). */
typedef PVOID (*FnAllocTag)(POOL_TYPE, SIZE_T, ULONG);
typedef PVOID (*FnAllocPrio)(POOL_TYPE, SIZE_T, ULONG, EX_POOL_PRIORITY);
typedef PVOID (*FnAlloc2)(POOL_FLAGS, SIZE_T, ULONG);
typedef void  (*FnFreeTag)(PVOID, ULONG);
enum { P_ALLOC = 0, P_ALLOC_TAG, P_ALLOC_QUOTA, P_ALLOC_QUOTA_TAG, P_ALLOC_PRIO, P_ALLOC2, P_ALLOC3, P_FREE_TAG, P_FREE };

/* setjmp-bearing C-drive helpers (keep setjmp in its own frame so loop vars are not clobbered). */
static int c_alloc_tag(FnAllocTag fn, POOL_TYPE pt, size_t b, ULONG tag, KIRQL irql, BugCheckRs *out) {
    g_irql = irql; g_craised = 0;
    if (setjmp(g_jmp) == 0) { (void)fn(pt, b, tag); return 0; }
    *out = g_cbc; return 1;
}
static PVOID c_alloc_block(FnAllocTag fn, POOL_TYPE pt, size_t b, ULONG tag, KIRQL irql) {
    g_irql = irql; g_craised = 0;
    if (setjmp(g_jmp) == 0) return fn(pt, b, tag);
    return (PVOID)0;
}
static int c_free_tag(FnFreeTag fn, PVOID block, ULONG tag, KIRQL irql, BugCheckRs *out) {
    g_irql = irql; g_craised = 0;
    if (setjmp(g_jmp) == 0) { fn(block, tag); return 0; }
    *out = g_cbc; return 1;
}
static void c_alloc_prio(FnAllocPrio fn, POOL_TYPE pt, size_t b, ULONG tag, EX_POOL_PRIORITY prio) {
    g_irql = 0; g_craised = 0; g_sp.called = 0;
    if (setjmp(g_jmp) == 0) (void)fn(pt, b, tag, prio);
}
static void c_alloc2(FnAlloc2 fn, POOL_FLAGS flags, size_t b, ULONG tag) {
    g_irql = 0; g_craised = 0; g_sp.called = 0; g_fallback2 = 0;
    if (setjmp(g_jmp) == 0) (void)fn(flags, b, tag);
}
static int c_unload(PLDR_DATA_TABLE_ENTRY ldr, BugCheckRs *out) {
    g_craised = 0;
    if (setjmp(g_jmp) == 0) { VfCheckPoolAtUnload(ldr); return 0; }
    *out = g_cbc; return 1;
}

static long g_checks = 0, g_dis = 0, g_raised = 0, g_ok = 0, g_declined = 0, g_decoded = 0, g_leak = 0, g_noleak = 0;

static void dis(const char *w, const BugCheckRs &c, int cr, const BugCheckRs &r, int rr) {
    g_dis++;
    if (g_dis <= 25)
        std::fprintf(stderr, "DISAGREE #%ld %s  C{r=%d %08x %zx %zx %zx %zx}  R{r=%d %08x %zx %zx %zx %zx}\n",
            g_dis, w, cr, c.code, (size_t)c.p1, (size_t)c.p2, (size_t)c.p3, (size_t)c.p4,
            rr, r.code, (size_t)r.p1, (size_t)r.p2, (size_t)r.p3, (size_t)r.p4);
}
static bool bceq(const BugCheckRs &c, const BugCheckRs &r) {
    return c.code == r.code && c.p1 == r.p1 && c.p2 == r.p2 && c.p3 == r.p3 && c.p4 == r.p4;
}

/* ---- A: check_allocate via VfExAllocatePoolWithTag ---- */
static void test_allocate(int neg) {
    FnAllocTag fn = (FnAllocTag)VfPoolThunks[P_ALLOC_TAG].Replacement;
    POOL_TYPE pts[3] = { NonPagedPoolNx, PagedPool, NonPagedPool };
    size_t sizes[2] = { 0, 32 };
    for (int pi = 0; pi < 3; pi++) for (int si = 0; si < 2; si++) for (uint8_t irql = 0; irql <= 4; irql++) {
        uint8_t ri = neg ? (uint8_t)(irql ^ 2) : irql;
        BugCheckRs cb{}, rb{}; int cr, rr;
        cr = c_alloc_tag(fn, pts[pi], sizes[si], 0x41424344, irql, &cb);
        rr = lrm_vf_check_allocate((uint32_t)pts[pi], sizes[si], ri, &rb);
        g_checks++; if (cr) g_raised++; else g_ok++;
        if (cr != rr || (cr && !bceq(cb, rb))) dis("check_allocate", cb, cr, rb, rr);
    }
}

/* ---- B: check_free via VfExFreePoolWithTag, with a pre-tracked paged block ---- */
static void test_free(int neg) {
    VfInitializePoolTracking();
    FnAllocTag al = (FnAllocTag)VfPoolThunks[P_ALLOC_TAG].Replacement;
    FnFreeTag fr = (FnFreeTag)VfPoolThunks[P_FREE_TAG].Replacement;
    /* allocate one paged + one nonpaged tracked block at IRQL 0 (no bugcheck) */
    PVOID paged = c_alloc_block(al, PagedPool, 32, 0x50414745, 0);
    PVOID np = c_alloc_block(al, NonPagedPoolNx, 32, 0x4E504147, 0);
    PVOID blocks[3] = { paged, np, (PVOID)0xDEAD };  /* paged-tracked, nonpaged-tracked, untracked */
    for (int bi = 0; bi < 3; bi++) for (uint8_t irql = 0; irql <= 4; irql++) {
        uint8_t ri = neg ? (uint8_t)(irql ^ 2) : irql;
        uint8_t tp = (bi == 0) ? 1 : 0;  /* only the paged block is tracked-paged */
        BugCheckRs cb{}, rb{}; int cr, rr;
        cr = c_free_tag(fr, blocks[bi], 0x50414745, irql, &cb);
        rr = lrm_vf_check_free(ri, tp, (uintptr_t)blocks[bi], &rb);
        g_checks++; if (cr) g_raised++; else g_ok++;
        if (cr != rr || (cr && !bceq(cb, rb))) dis("check_free", cb, cr, rb, rr);
    }
}

/* ---- C: special_type via VfExAllocatePoolWithTagPriority ---- */
static void test_special_type(int neg) {
    MmVerifierData.Level = DRIVER_VERIFIER_SPECIAL_POOLING;
    FnAllocPrio fn = (FnAllocPrio)VfPoolThunks[P_ALLOC_PRIO].Replacement;
    int prios[9] = { 0, 8, 9, 16, 24, 25, 40, 41, 7 };
    for (int i = 0; i < 9; i++) {
        c_alloc_prio(fn, NonPagedPoolNx, 32, 0x54455354, prios[i]);
        uint32_t rst = lrm_vf_special_type(neg ? (uint32_t)(prios[i] ^ 1) : (uint32_t)prios[i]);
        g_checks++;
        if (!g_sp.called) { BugCheckRs z{}; dis("special_type.notcalled", z, 0, z, 0); continue; }
        if (g_sp.stype != rst) {
            g_dis++;
            if (g_dis <= 25) std::fprintf(stderr, "DISAGREE #%ld special_type prio=%d C=%lu R=%u\n",
                g_dis, prios[i], (unsigned long)g_sp.stype, rst);
        }
    }
}

/* ---- D: pool2 decode via VfExAllocatePool2 ---- */
static void test_pool2(int neg) {
    MmVerifierData.Level = DRIVER_VERIFIER_SPECIAL_POOLING;
    FnAlloc2 fn = (FnAlloc2)VfPoolThunks[P_ALLOC2].Replacement;
    uint64_t base[6] = { POOL_FLAG_NON_PAGED, POOL_FLAG_NON_PAGED_EXECUTE, POOL_FLAG_PAGED,
                         POOL_FLAG_NON_PAGED | POOL_FLAG_PAGED, 0, POOL_FLAG_NON_PAGED | POOL_FLAG_CACHE_ALIGNED };
    uint32_t tags[2] = { 0, 0x54414732 };
    for (int fi = 0; fi < 6; fi++) for (int ti = 0; ti < 2; ti++) {
        uint64_t flags = base[fi];
        uint32_t tag = tags[ti];
        c_alloc2(fn, (POOL_FLAGS)flags, 32, tag);
        uint32_t rpt = 0; uint8_t rzero = 0;
        uint8_t rdec = lrm_vf_pool2_decode(neg ? (uint32_t)(flags ^ (uint32_t)POOL_FLAG_PAGED) : (uint32_t)flags, tag, &rpt, &rzero);
        g_checks++;
        if (rdec) {  /* Rust says special pool with type rpt -> C must have called MmAllocateSpecialPool with that type */
            g_decoded++;
            if (!g_sp.called || (uint32_t)g_sp.pt != rpt) {
                g_dis++;
                if (g_dis <= 25) std::fprintf(stderr, "DISAGREE #%ld pool2 flags=%llx tag=%x  C{called=%d pt=%d} R{dec=1 pt=%u}\n",
                    g_dis, (unsigned long long)flags, tag, g_sp.called, g_sp.pt, rpt);
            }
        } else {     /* Rust routes to the normal allocator -> C must NOT have hit special pool */
            g_declined++;
            if (g_sp.called) {
                g_dis++;
                if (g_dis <= 25) std::fprintf(stderr, "DISAGREE #%ld pool2 flags=%llx tag=%x  C called special, R declined\n",
                    g_dis, (unsigned long long)flags, tag);
            }
        }
    }
}

/* ---- E: tracking counters + leak at unload ---- */
extern "C" void e_anchor(void) {}  /* a harness code address, to size the "driver image" window */

static TrackEntryRs g_rentries[256];
static VfTrackStateRs g_rst;

static bool counters_eq(void) {
    return MmVerifierData.CurrentPagedPoolAllocations == g_rst.cur_paged &&
           MmVerifierData.CurrentNonPagedPoolAllocations == g_rst.cur_nonpaged &&
           MmVerifierData.PeakPagedPoolAllocations == g_rst.peak_paged &&
           MmVerifierData.PeakNonPagedPoolAllocations == g_rst.peak_nonpaged &&
           MmVerifierData.PagedBytes == g_rst.paged_bytes &&
           MmVerifierData.NonPagedBytes == g_rst.nonpaged_bytes &&
           MmVerifierData.PeakPagedBytes == g_rst.peak_paged_bytes &&
           MmVerifierData.PeakNonPagedBytes == g_rst.peak_nonpaged_bytes;
}

static void test_tracking(void) {
    /* clean tracking state (re-init orphans prior entries from the scan) */
    memset(&MmVerifierData, 0, sizeof MmVerifierData);
    MmVerifierData.Level = DRIVER_VERIFIER_SPECIAL_POOLING;
    VfInitializePoolTracking();
    memset(g_rentries, 0, sizeof g_rentries);
    memset(&g_rst, 0, sizeof g_rst);

    FnAllocTag al = (FnAllocTag)VfPoolThunks[P_ALLOC_TAG].Replacement;
    uintptr_t window = (uintptr_t)&e_anchor;              /* in-range sentinel for the Rust side */
    PVOID tracked[40]; int ntracked = 0;

    /* allocate 20 paged + 20 nonpaged, tracking each in the Rust mirror in lockstep */
    for (int i = 0; i < 40; i++) {
        bool paged = (i % 2) == 0;
        uint32_t size = 16 + (uint32_t)(i * 8);
        PVOID blk = c_alloc_block(al, paged ? PagedPool : NonPagedPoolNx, size, 0x4C45414B, 0);
        tracked[ntracked++] = blk;
        lrm_vf_track(&g_rst, g_rentries, 256, (uintptr_t)blk, size, paged ? 1 : 0, window);
        g_checks++;
        if (!counters_eq()) { BugCheckRs z{}; dis("track.counters", z, 0, z, 0); }
    }
    /* untrack 12 of them */
    for (int i = 0; i < 12; i++) {
        VfUntrackPool(tracked[i]);
        lrm_vf_untrack(&g_rst, g_rentries, 256, (uintptr_t)tracked[i]);
        g_checks++;
        if (!counters_eq()) { BugCheckRs z{}; dis("untrack.counters", z, 0, z, 0); }
    }

    /* leak at unload: image window covers e_anchor (hence the harness call sites) -> all 28
       remaining tracked blocks are leaks. Compare the C bugcheck to the Rust tracker's. */
    LDR_DATA_TABLE_ENTRY ldr; memset(&ldr, 0, sizeof ldr);
    ldr.Flags = LDRP_IMAGE_VERIFYING;
    ldr.DllBase = (PVOID)(window - 0x400000);
    ldr.SizeOfImage = 0x800000;                           /* 8 MiB window around e_anchor */
    static WCHAR nm[] = { 'd','r','v',0 };
    ldr.BaseDllName.Buffer = nm; ldr.BaseDllName.Length = 6; ldr.BaseDllName.MaximumLength = 8;

    BugCheckRs cb{}, rb{}; int cr, rr;
    cr = c_unload(&ldr, &cb);
    rr = lrm_vf_check_at_unload(g_rentries, 256, (uintptr_t)ldr.BaseDllName.Buffer,
                               (uintptr_t)ldr.DllBase, (uintptr_t)ldr.DllBase + ldr.SizeOfImage, &rb);
    g_checks++; if (cr) g_leak++;
    /* both must raise, same sub-code (0x62) and the same leak count (p4); name ptr (p2) matches too. */
    if (cr != rr || (cr && (cb.code != rb.code || cb.p1 != rb.p1 || cb.p4 != rb.p4))) dis("leak", cb, cr, rb, rr);

    /* no-leak case: untrack the rest, unload again -> neither raises */
    for (int i = 12; i < ntracked; i++) {
        VfUntrackPool(tracked[i]);
        lrm_vf_untrack(&g_rst, g_rentries, 256, (uintptr_t)tracked[i]);
    }
    cr = c_unload(&ldr, &cb);
    rr = lrm_vf_check_at_unload(g_rentries, 256, (uintptr_t)ldr.BaseDllName.Buffer,
                               (uintptr_t)ldr.DllBase, (uintptr_t)ldr.DllBase + ldr.SizeOfImage, &rb);
    g_checks++; if (!cr) g_noleak++;
    if (cr != rr) dis("leak.none", cb, cr, rb, rr);
}

static void run(int neg) {
    memset(&MmVerifierData, 0, sizeof MmVerifierData);
    test_allocate(neg);
    test_free(neg);
    test_special_type(neg);
    test_pool2(neg);
    if (!neg) test_tracking();   /* tracking/leak compares counters directly; no IRQL perturbation */
}

int main(void) {
    run(0);
    std::printf("[vf-pool] checks=%ld disagreements=%ld (raised=%ld ok=%ld declined=%ld decoded=%ld leak=%ld noleak=%ld) thunks=%lu\n",
                g_checks, g_dis, g_raised, g_ok, g_declined, g_decoded, g_leak, g_noleak, (unsigned long)VfPoolThunkCount);
    long dis0 = g_dis;
    bool nonvacuous = (g_raised > 0 && g_ok > 0 && g_declined > 0 && g_decoded > 0 && g_leak > 0 && g_noleak > 0);

    g_dis = 0; long savedchecks = g_checks; g_checks = 0;
    run(1);
    long negdis = g_dis; g_checks = savedchecks;
    std::printf("[vf-pool] negative-control disagreements=%ld (expect > 0)\n", negdis);

    if (dis0 != 0) { std::fprintf(stderr, "FAIL: %ld disagreements\n", dis0); return 1; }
    if (!nonvacuous) { std::fprintf(stderr, "FAIL: vacuous coverage\n"); return 1; }
    if (negdis == 0) { std::fprintf(stderr, "FAIL: negative control did not bite\n"); return 1; }
    std::printf("RESULT=PASS\n");
    return 0;
}
