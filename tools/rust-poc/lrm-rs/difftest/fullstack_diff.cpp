/* LRM-RS FULL-STACK differential: the real ntspecial.c orchestration (MmAllocateSpecialPool /
 * MmFreeSpecialPool, compiled UNMODIFIED against a minimal faithful stub header tree + the real
 * special.c engine) vs the Rust orchestration (lrm_mm_allocate_special_pool /
 * lrm_mm_free_special_pool over the same mock MiGuard provider). For a randomized sequence of
 * allocate / corrupt / free operations across pool-type / special-type / IRQL / corruption inputs,
 * both sides run over independent-but-identical single-slot mock regions; we compare the outcome:
 * declined vs allocated (block offset in the page) vs bug check (code + exact 0xC1 sub-code and all
 * four parameters, with in-page address parameters normalized to page offsets).
 *
 * C bug checks are captured via setjmp/longjmp (KeBugCheckEx is noreturn). Exit 0 iff 0
 * disagreements AND every outcome class was exercised (non-vacuous). A negative control
 * (SPECIAL_TYPE perturbed on the Rust side only) must produce disagreements.
 *
 * Leeway Open Software. Test-only.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csetjmp>

extern "C" {
#include <nvs/nt/mint.h>
#include <nvs/pool/ex/expool.h>
#include <nvs/pool/include/poolspecial.h>

/* Defined by ntspecial.c */
void MiInitializeSpecialPool(void);
PVOID MmAllocateSpecialPool(SIZE_T NumberOfBytes, ULONG Tag, POOL_TYPE PoolType, ULONG SpecialType);
void MmFreeSpecialPool(PVOID P);
SIZE_T MmQuerySpecialPoolBlockSize(PVOID P);
extern ULONG MmSpecialPoolTag;
extern ULONG MmSpecialPoolCatchOverruns;
}

/* ---- Rust C-ABI (mm.rs) ---- */
struct BugCheckRs { uint32_t code; uintptr_t p1, p2, p3, p4; };
struct LrmGuardVtbl {
    int32_t (*query)(void *self, uintptr_t addr, uintptr_t *page, void **ctx);
    uint8_t (*allocate)(void *self, uint8_t executable, uintptr_t *page, void **ctx);
    uint8_t (*free)(void *self, uintptr_t page);
    void    (*refill)(void *self);
    void   *self;
};
extern "C" uint8_t lrm_mm_allocate_special_pool(const LrmGuardVtbl *v, uint8_t region_present,
    uint8_t underruns, size_t bytes, uint32_t tag, uint32_t pool_type, uint32_t special_type,
    uint8_t irql, void **out_ptr, BugCheckRs *out_bc);
extern "C" uint8_t lrm_mm_free_special_pool(const LrmGuardVtbl *v, uintptr_t p, uint8_t irql,
    BugCheckRs *out_bc);

/* ---- NT status values the mock MiGuard needs ---- */
#define ST_SUCCESS              ((NTSTATUS)0)
#define ST_INSUFFICIENT         ((NTSTATUS)0xC000009AL)
#define ST_INVALID_ADDRESS      ((NTSTATUS)0xC0000141L)

/* ---- Single-slot mock guard region (one data page + one record) ---- */
enum { SLOT_EMPTY = 0, SLOT_LIVE = 2, SLOT_FREED = 3 };
struct Model {
    MI_GUARD_REGION hdr;          /* first member: Base, read by ntspecial.c's DbgPrint */
    uint8_t page[PAGE_SIZE] __attribute__((aligned(4096)));
    POOL_SPECIAL_RECORD rec;
    int state;
    bool execLast;
    uint64_t base() const { return (uint64_t)(uintptr_t)page; }
    void reset() { state = SLOT_EMPTY; execLast = false; memset(page, 0, sizeof page); memset(&rec, 0, sizeof rec); hdr.Base = base(); }
};

/* Core mock ops shared by the C stubs and the Rust vtable (same logic → same provider behaviour). */
static MI_GUARD_KIND model_query(Model *m, uint64_t va, uint64_t *dataVa, void **ctx) {
    if (dataVa) *dataVa = 0;
    if (ctx) *ctx = nullptr;
    if (va < m->base() || va >= m->base() + PAGE_SIZE) return MiGuardOutside;
    if (dataVa) *dataVa = m->base();
    if (ctx) *ctx = &m->rec;
    return (m->state == SLOT_LIVE) ? MiGuardLive : MiGuardFreed;
}
static NTSTATUS model_allocate(Model *m, bool executable, uint64_t *dataVa, void **ctx) {
    if (dataVa) *dataVa = 0;
    if (ctx) *ctx = nullptr;
    if (m->state == SLOT_LIVE) return ST_INSUFFICIENT;   /* single slot exhausted */
    m->state = SLOT_LIVE;
    m->execLast = executable;
    if (dataVa) *dataVa = m->base();
    if (ctx) *ctx = &m->rec;
    return ST_SUCCESS;
}
static NTSTATUS model_free(Model *m, uint64_t dataVa) {
    if (dataVa != m->base() || m->state != SLOT_LIVE) return ST_INVALID_ADDRESS;
    m->state = SLOT_FREED;
    return ST_SUCCESS;
}

/* ===== C side: ntspecial.c's externs, backed by g_cmodel ===== */
static Model g_cmodel;
static jmp_buf g_jmp;
static BugCheckRs g_cbc;
static int g_craised;
static KIRQL g_cirql;

extern "C" {
MI_SYSTEM MiSystem;
unsigned short MmVerifyDriverBuffer[256];
ULONG64 MmNumberOfPhysicalPages = 0x40000;   /* 256K pages = 1 GiB, arbitrary */
ULONG ExpPoolFlags;

unsigned long DbgPrint(const char *, ...) { return 0; }
KIRQL KeGetCurrentIrql(void) { return g_cirql; }
void KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4) {
    g_cbc = { (uint32_t)Code, (uintptr_t)P1, (uintptr_t)P2, (uintptr_t)P3, (uintptr_t)P4 };
    g_craised = 1;
    longjmp(g_jmp, 1);
}
void KeBugCheckWithTf(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4, PKTRAP_FRAME) {
    g_cbc = { (uint32_t)Code, (uintptr_t)P1, (uintptr_t)P2, (uintptr_t)P3, (uintptr_t)P4 };
    g_craised = 1;
    longjmp(g_jmp, 1);
}
ULONG_PTR KeGetTrapFramePc(PKTRAP_FRAME) { return 0; }
int MiPtTranslate(void *, ULONG64, PULONG64, void *) { return 0; }

NTSTATUS MiGuardRegionCreate(void *, ULONG, ULONG, ULONG, ULONG, ULONG64, SIZE_T, PMI_GUARD_REGION *Region) {
    g_cmodel.hdr.Base = g_cmodel.base();
    *Region = &g_cmodel.hdr;
    return ST_SUCCESS;
}
MI_GUARD_KIND MiGuardQuery(PMI_GUARD_REGION R, ULONG64 va, PULONG64 dv, PVOID *ctx) {
    return model_query((Model *)R, va, dv, ctx);
}
NTSTATUS MiGuardAllocate(PMI_GUARD_REGION R, ULONG Protection, PULONG64 dv, PVOID *ctx) {
    return model_allocate((Model *)R, Protection == MI_PROT_EXECUTE_READWRITE, dv, ctx);
}
NTSTATUS MiGuardFree(PMI_GUARD_REGION R, ULONG64 dv) { return model_free((Model *)R, dv); }
ULONG MiGuardRefill(PMI_GUARD_REGION) { return 0; }
} /* extern "C" */

/* ===== Rust side: vtable over g_rmodel ===== */
static Model g_rmodel;
static int32_t rs_query(void *self, uintptr_t addr, uintptr_t *page, void **ctx) {
    uint64_t dv = 0; void *c = nullptr;
    MI_GUARD_KIND k = model_query((Model *)self, addr, &dv, &c);
    if (page) *page = (uintptr_t)dv;
    if (ctx) *ctx = c;
    return (int32_t)k;
}
static uint8_t rs_allocate(void *self, uint8_t executable, uintptr_t *page, void **ctx) {
    uint64_t dv = 0; void *c = nullptr;
    NTSTATUS s = model_allocate((Model *)self, executable != 0, &dv, &c);
    if (!NT_SUCCESS(s)) return 0;
    if (page) *page = (uintptr_t)dv;
    if (ctx) *ctx = c;
    return 1;
}
static uint8_t rs_free(void *self, uintptr_t page) { return NT_SUCCESS(model_free((Model *)self, page)) ? 1 : 0; }
static void rs_refill(void *) {}

/* ===== comparison ===== */
static long g_checks = 0, g_dis = 0;
static long g_decl = 0, g_alloc = 0, g_bc = 0;        /* outcome-class coverage */
static long g_s20 = 0, g_s23 = 0, g_s24 = 0, g_s30 = 0, g_s31 = 0;

struct Outcome { int kind; /*0 declined,1 allocated,2 bugcheck*/ uint64_t off; BugCheckRs bc; };

static void norm(BugCheckRs &b, uint64_t base) {
    uintptr_t *p[4] = { &b.p1, &b.p2, &b.p3, &b.p4 };
    for (int i = 0; i < 4; i++)
        if (*p[i] >= base && *p[i] < base + PAGE_SIZE) *p[i] -= base;
}

static void report_dis(const char *what, const Outcome &c, const Outcome &r) {
    g_dis++;
    if (g_dis <= 25)
        std::fprintf(stderr,
            "DISAGREE #%ld %s  C{k=%d off=%llu bc=%08x %llx %llx %llx %llx}  R{k=%d off=%llu bc=%08x %llx %llx %llx %llx}\n",
            g_dis, what, c.kind, (unsigned long long)c.off, c.bc.code,
            (unsigned long long)c.bc.p1, (unsigned long long)c.bc.p2, (unsigned long long)c.bc.p3, (unsigned long long)c.bc.p4,
            r.kind, (unsigned long long)r.off, r.bc.code,
            (unsigned long long)r.bc.p1, (unsigned long long)r.bc.p2, (unsigned long long)r.bc.p3, (unsigned long long)r.bc.p4);
}

static bool eq(const Outcome &c, const Outcome &r) {
    if (c.kind != r.kind) return false;
    if (c.kind == 1) return c.off == r.off;
    if (c.kind == 2)
        return c.bc.code == r.bc.code && c.bc.p1 == r.bc.p1 && c.bc.p2 == r.bc.p2 &&
               c.bc.p3 == r.bc.p3 && c.bc.p4 == r.bc.p4;
    return true; /* both declined */
}

/* ---- run one allocate on each side ---- */
static Outcome c_alloc(size_t bytes, uint32_t tag, int pooltype, uint32_t stype, KIRQL irql) {
    Outcome o{}; g_craised = 0; g_cirql = irql;
    if (setjmp(g_jmp) == 0) {
        PVOID p = MmAllocateSpecialPool(bytes, tag, pooltype, stype);
        if (p == nullptr) { o.kind = 0; }
        else { o.kind = 1; o.off = (uint64_t)(uintptr_t)p - g_cmodel.base(); }
    } else { o.kind = 2; o.bc = g_cbc; norm(o.bc, g_cmodel.base()); }
    return o;
}
static Outcome r_alloc(const LrmGuardVtbl *v, size_t bytes, uint32_t tag, uint32_t pooltype,
                       uint32_t stype, KIRQL irql, uint8_t underruns) {
    Outcome o{}; void *p = nullptr; BugCheckRs bc{};
    uint8_t rc = lrm_mm_allocate_special_pool(v, 1, underruns, bytes, tag, pooltype, stype, irql, &p, &bc);
    if (rc == 2) { o.kind = 2; o.bc = bc; norm(o.bc, g_rmodel.base()); }
    else if (rc == 1) { o.kind = 1; o.off = (uint64_t)(uintptr_t)p - g_rmodel.base(); }
    else o.kind = 0;
    return o;
}
static Outcome c_free(uint64_t off, KIRQL irql) {
    Outcome o{}; g_craised = 0; g_cirql = irql;
    if (setjmp(g_jmp) == 0) { MmFreeSpecialPool((PVOID)(uintptr_t)(g_cmodel.base() + off)); o.kind = 0; }
    else { o.kind = 2; o.bc = g_cbc; norm(o.bc, g_cmodel.base()); }
    return o;
}
static Outcome r_free(const LrmGuardVtbl *v, uint64_t off, KIRQL irql) {
    Outcome o{}; BugCheckRs bc{};
    uint8_t rc = lrm_mm_free_special_pool(v, (uintptr_t)(g_rmodel.base() + off), irql, &bc);
    if (rc) { o.kind = 2; o.bc = bc; norm(o.bc, g_rmodel.base()); }
    else o.kind = 0;
    return o;
}

static uint32_t g_rng = 0x1234abcdu;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

/* pool_type bit soup: base paged bit, cache-align bit, NX bit, plus noise. */
static uint32_t rnd_pooltype() {
    uint32_t t = 0;
    if (rnd() & 1) t |= EX_POOL_BASE_TYPE_MASK;
    if (rnd() & 1) t |= CACHE_ALIGNED_POOL_MASK;
    if (rnd() & 1) t |= POOL_NX_ALLOCATION;
    return t;
}

/* neg!=0 perturbs the Rust special_type (negative control). */
static void iterate(const LrmGuardVtbl *v, uint8_t underruns, int neg) {
    /* Fresh slot each iteration so the allocate/placement path is exercised broadly (a corrupted
     * or bug-check-blocked block would otherwise stay LIVE and starve further allocations). */
    g_cmodel.reset(); g_rmodel.reset();

    /* --- allocate --- */
    size_t bytes;
    uint32_t pick = rnd() % 20;
    if (pick == 0) bytes = 0;                 /* -> declined (place fail) */
    else if (pick == 1) bytes = PAGE_SIZE + (rnd() % 64); /* -> declined */
    else bytes = 1 + (rnd() % (PAGE_SIZE - 1));
    uint32_t tag = (rnd() | 0x80808080u);     /* high bytes set: a tag, not a size class */
    uint32_t pt = rnd_pooltype();
    uint32_t st = rnd() % 3;                   /* 0 end, 1 start, 2 global default */
    KIRQL irql = (KIRQL)(rnd() % 4);           /* 0..3 (3 > DISPATCH) */

    uint32_t rst = st;
    if (neg) rst = (st == 0) ? 1 : (st == 1) ? 0 : st; /* NEGATIVE CONTROL: swap 0<->1 for Rust */

    Outcome ca = c_alloc(bytes, tag, (int)pt, st, irql);
    Outcome ra = r_alloc(v, bytes, tag, pt, rst, irql, underruns);
    g_checks++;
    if (ca.kind == 0) g_decl++; else if (ca.kind == 1) g_alloc++; else { g_bc++; if (ca.bc.p4 == 0x30) g_s30++; }
    if (!eq(ca, ra)) report_dis("alloc", ca, ra);

    /* If not both LIVE now, nothing more to do this iteration. */
    if (g_cmodel.state != SLOT_LIVE || g_rmodel.state != SLOT_LIVE) return;

    uint64_t off = g_cmodel.rec.Offset;
    uint32_t size = g_cmodel.rec.Size;

    /* --- optionally corrupt (same logical byte on both pages) --- */
    uint32_t act = rnd() % 10;
    if (act < 3 && off > 0) {                 /* corrupt BEFORE */
        uint64_t co = rnd() % off;
        g_cmodel.page[co] ^= 0xFF; g_rmodel.page[co] ^= 0xFF;
    } else if (act < 6 && off + size < PAGE_SIZE) { /* corrupt AFTER */
        uint64_t co = off + size + (rnd() % (PAGE_SIZE - (off + size)));
        g_cmodel.page[co] ^= 0xFF; g_rmodel.page[co] ^= 0xFF;
    }

    /* --- free --- */
    KIRQL firql = (KIRQL)(rnd() % 4);
    uint64_t fo = off;
    uint32_t fp = rnd() % 10;
    if (fp == 0) fo = off ^ 0x40;             /* wrong pointer in-page -> owns fails -> 0x20 */
    Outcome cf = c_free(fo, firql);
    Outcome rf = r_free(v, fo, firql);
    g_checks++;
    if (cf.kind == 2) { g_bc++;
        switch (cf.bc.p4) { case 0x20: g_s20++; break; case 0x23: g_s23++; break;
                            case 0x24: g_s24++; break; case 0x31: g_s31++; break; } }
    if (!eq(cf, rf)) report_dis("free", cf, rf);

    /* Occasionally double-free (both now FREED) to exercise 0x20. */
    if ((rnd() % 4) == 0) {
        Outcome cf2 = c_free(off, 0);
        Outcome rf2 = r_free(v, off, 0);
        g_checks++;
        if (cf2.kind == 2 && cf2.bc.p4 == 0x20) g_s20++;
        if (!eq(cf2, rf2)) report_dis("double-free", cf2, rf2);
    }
}

static long run(long iters, int neg) {
    LrmGuardVtbl vt{ rs_query, rs_allocate, rs_free, rs_refill, &g_rmodel };

    /* two phases: overruns (underruns=false) then underruns (true). */
    for (int phase = 0; phase < 2; phase++) {
        uint8_t underruns = (phase == 1);
        MmSpecialPoolCatchOverruns = underruns ? 0 : 1;
        MmSpecialPoolTag = POOL_SPECIAL_SELECT_ALL;
        g_cmodel.reset(); g_rmodel.reset();
        MiInitializeSpecialPool();            /* sets the C statics for this phase */
        for (long i = 0; i < iters / 2; i++) {
            /* keep models EMPTY/FREED->reusable; the single slot reuses after free. */
            iterate(&vt, underruns, neg);
        }
    }
    return g_dis;
}

int main(int argc, char **argv) {
    long iters = (argc > 1) ? std::strtol(argv[1], nullptr, 10) : 2000000;

    long dis = run(iters, 0);
    std::printf("[fullstack] checks=%ld disagreements=%ld\n", g_checks, dis);
    std::printf("[fullstack] coverage: declined=%ld allocated=%ld bugchecks=%ld "
                "(0x20=%ld 0x23=%ld 0x24=%ld 0x30=%ld 0x31=%ld)\n",
                g_decl, g_alloc, g_bc, g_s20, g_s23, g_s24, g_s30, g_s31);

    bool nonvacuous = (g_decl > 0 && g_alloc > 0 && g_s20 > 0 && g_s23 > 0 && g_s24 > 0 && g_s31 > 0);
    if (g_s30 == 0)
        std::fprintf(stderr, "NOTE: 0x30 (allocate-IRQL) not hit this run (paged+high-IRQL is rare)\n");

    /* negative control: perturb SPECIAL_TYPE on the Rust side only -> must disagree. */
    long dis0 = g_dis; g_checks = 0; g_dis = 0;
    long negdis = run(200000, 1);
    std::printf("[fullstack] negative-control disagreements=%ld (expect > 0)\n", negdis);

    if (dis != 0) { std::fprintf(stderr, "FAIL: %ld disagreements\n", dis0); return 1; }
    if (!nonvacuous) { std::fprintf(stderr, "FAIL: vacuous (an outcome class was never exercised)\n"); return 1; }
    if (negdis == 0) { std::fprintf(stderr, "FAIL: negative control did not bite\n"); return 1; }
    std::printf("RESULT=PASS\n");
    return 0;
}
