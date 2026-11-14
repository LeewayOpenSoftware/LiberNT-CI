/* LRM-RS Driver-Verifier Stage 4 FULL-STACK differential: the real vfdeadlock.c lock-order engine
 * (compiled UNMODIFIED against a host <ntoskrnl.h> stub, over mock Ke/Io services) vs the Rust
 * severity decision (lrm_vfdl_*). The graph-walk / held-record bookkeeping stays in C (it owns the
 * graph state); this harness drives the exported lock hooks (VfLockAcquiring/Acquired/Releasing,
 * VfForgetLocksInRange) to make each of the five rules fire, captures the C's KeBugCheckEx via
 * setjmp/longjmp and counts its "(log only)" lines, and compares, per violation, against
 * lrm_vfdl_report: the fatal rules (0x1004 WRONG_THREAD, 0x100B DELETED_OWNED) must match code 0xC4
 * and all four params; the log-only rules (0x1000/0x1001/0x1007) must match the Log outcome. Exit 0
 * iff 0 disagreements AND coverage is non-vacuous (every rule exercised, both fatal and log seen);
 * a negative control (feed the Rust side the opposite-severity rule) must disagree.
 * Leeway Open Software. Test-only. */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <csetjmp>

extern "C" {
#include <ntoskrnl.h>
extern void VfInitializeDeadlockDetection(void);
extern void VfLockAcquiring(PVOID Lock, PVOID Caller);
extern void VfLockAcquired(PVOID Lock, PVOID Caller);
extern void VfLockReleasing(PVOID Lock, PVOID Caller);
extern void VfForgetLocksInRange(PVOID Base, SIZE_T Size);
extern ULONG VfDeadlockRuleHits;
}

/* Rust ABI (vf.rs) */
struct BugCheckRs { uint32_t code; uintptr_t p1, p2, p3, p4; };
extern "C" uint8_t lrm_vfdl_rule_fatal(uint32_t rule);
extern "C" uint8_t lrm_vfdl_report(uint32_t rule, uintptr_t lock, uintptr_t second, uintptr_t third,
                                   uint32_t *hits, BugCheckRs *out_bc, uint32_t *out_rule);

/* ---- rule sub-codes (shared with vfdeadlock.c) ---- */
#define R_SELF        0x1000u
#define R_HIERARCHY   0x1001u
#define R_WRONG       0x1004u
#define R_UNACQUIRED  0x1007u
#define R_DELETED     0x100Bu
#define BUGCHECK_0xC4 0xC4u

/* ---- C-side kernel stubs ---- */
static jmp_buf g_jmp;
static BugCheckRs g_cbc;
static int g_craised;
static int g_log;
static KIRQL g_irql;
static PKTHREAD g_cur_thread;
static ULONG_PTR g_stack_lo = 0x10, g_stack_hi = 0x20;   /* no real lock address falls in here */

extern "C" {
KIRQL KeGetCurrentIrql(void) { return g_irql; }
PKTHREAD KeGetCurrentThread(void) { return g_cur_thread; }
unsigned long DbgPrint(const char *, ...) { g_log++; return 0; }
void IoGetStackLimits(ULONG_PTR *lo, ULONG_PTR *hi) { *lo = g_stack_lo; *hi = g_stack_hi; }
void KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4) {
    g_cbc = { (uint32_t)Code, (uintptr_t)P1, (uintptr_t)P2, (uintptr_t)P3, (uintptr_t)P4 };
    g_craised = 1; longjmp(g_jmp, 1);
}
}

/* setjmp-bearing drive helpers (each keeps setjmp in its own frame). */
static void h_acquiring(PVOID lock, PVOID caller) {
    g_craised = 0; g_log = 0;
    if (setjmp(g_jmp) == 0) VfLockAcquiring(lock, caller);
}
static void h_acquired(PVOID lock, PVOID caller) { VfLockAcquired(lock, caller); }
static int h_releasing(PVOID lock, PVOID caller, BugCheckRs *out) {
    g_craised = 0; g_log = 0;
    if (setjmp(g_jmp) == 0) { VfLockReleasing(lock, caller); return 0; }
    *out = g_cbc; return 1;
}
static int h_forget(PVOID base, SIZE_T size, BugCheckRs *out) {
    g_craised = 0; g_log = 0;
    if (setjmp(g_jmp) == 0) { VfForgetLocksInRange(base, size); return 0; }
    *out = g_cbc; return 1;
}

static long g_checks = 0, g_dis = 0, g_fatal = 0, g_logged = 0, g_silent = 0;
static uint32_t r_hits = 0;   /* mirrors VfDeadlockRuleHits */

static bool bceq(const BugCheckRs &c, const BugCheckRs &r) {
    return c.code == r.code && c.p1 == r.p1 && c.p2 == r.p2 && c.p3 == r.p3 && c.p4 == r.p4;
}
static void dis(const char *w, int cr, const BugCheckRs &c, int rdisc, const BugCheckRs &r) {
    g_dis++;
    if (g_dis <= 25)
        std::fprintf(stderr, "DISAGREE #%ld %s  C{raised=%d %08x %zx %zx %zx %zx}  R{disc=%d %08x %zx %zx %zx %zx}\n",
            g_dis, w, cr, c.code, (size_t)c.p1, (size_t)c.p2, (size_t)c.p3, (size_t)c.p4,
            rdisc, r.code, (size_t)r.p1, (size_t)r.p2, (size_t)r.p3, (size_t)r.p4);
}

/* Compare one fired violation: C observable (fatal cb / logged / silent) vs Rust lrm_vfdl_report.
 * `neg` feeds the Rust side the opposite-severity rule to force a disagreement (negative control). */
static void compare(const char *w, uint32_t rule, uintptr_t lock, uintptr_t second, uintptr_t third,
                    int cr, const BugCheckRs &cb, int clog, int neg) {
    uint32_t rr_rule = rule;
    if (neg) rr_rule = lrm_vfdl_rule_fatal(rule) ? R_SELF : R_WRONG;   /* flip fatal<->non-fatal */
    BugCheckRs rb{}; uint32_t out_rule = 0;
    uint8_t disc = lrm_vfdl_report(rr_rule, lock, second, third, &r_hits, &rb, &out_rule);
    g_checks++;

    int c_obs = cr ? 2 : (clog > 0 ? 1 : 0);   /* 2=fatal,1=log,0=silent */
    if (cr) g_fatal++; else if (clog > 0) g_logged++; else g_silent++;

    if (c_obs != disc) { dis(w, cr, cb, disc, rb); return; }
    if (c_obs == 2 && !bceq(cb, rb)) { dis(w, cr, cb, disc, rb); return; }
    if (c_obs == 1 && out_rule != rule) { dis(w, cr, cb, disc, rb); return; }
}

/* distinct per-scenario addresses so stale held entries never collide */
static PVOID LKA = (PVOID)0x100000, LKB = (PVOID)0x200000;
static PVOID CALLER = (PVOID)0xC0DE00;
static PKTHREAD T1 = (PKTHREAD)0x1111000, T2 = (PKTHREAD)0x2222000;

static void fresh(void) { VfInitializeDeadlockDetection(); g_irql = 0; }

static void run(int neg) {
    /* ---- WRONG_THREAD (0x1004, fatal): T1 acquires, T2 releases ---- */
    {
        fresh();
        PVOID L = (PVOID)0x300000;
        g_cur_thread = T1; h_acquired(L, CALLER);
        g_cur_thread = T2;
        BugCheckRs cb{}; int cr = h_releasing(L, CALLER, &cb); int clog = g_log;
        compare("wrong_thread", R_WRONG, (uintptr_t)L, (uintptr_t)T1, (uintptr_t)T2, cr, cb, clog, neg);
    }
    /* ---- DELETED_OWNED (0x100B, fatal): T1 holds a lock inside a freed range ---- */
    {
        fresh();
        PVOID L = (PVOID)0x410000;
        PVOID base = (PVOID)0x400000; SIZE_T size = 0x20000;   /* covers L */
        g_cur_thread = T1; h_acquired(L, CALLER);
        BugCheckRs cb{}; int cr = h_forget(base, size, &cb); int clog = g_log;
        compare("deleted_owned", R_DELETED, (uintptr_t)L, (uintptr_t)T1, 1u, cr, cb, clog, neg);
    }
    /* ---- UNACQUIRED (0x1007, log): release a lock never acquired ---- */
    {
        fresh();
        PVOID L = (PVOID)0x500000;
        g_cur_thread = T1;
        BugCheckRs cb{}; int cr = h_releasing(L, CALLER, &cb); int clog = g_log;
        compare("unacquired", R_UNACQUIRED, (uintptr_t)L, 0, 0, cr, cb, clog, neg);
    }
    /* ---- SELF (0x1000, log): same thread re-acquires a held lock ---- */
    {
        fresh();
        PVOID L = (PVOID)0x600000;
        g_cur_thread = T1; h_acquired(L, CALLER);
        h_acquiring(L, CALLER);                 /* recursion -> SELF (log) */
        int clog = g_log; BugCheckRs cb{};
        compare("self", R_SELF, (uintptr_t)L, (uintptr_t)CALLER, 0, 0, cb, clog, neg);
        g_cur_thread = T1; h_releasing(L, CALLER, &cb);   /* cleanup */
    }
    /* ---- HIERARCHY (0x1001, log): A->B edge then acquire B then A ---- */
    {
        fresh();
        PVOID A = LKA, B = LKB;
        g_cur_thread = T1;
        h_acquired(A, CALLER); h_acquiring(B, CALLER); h_acquired(B, CALLER);  /* edge A->B */
        BugCheckRs cx{};
        h_releasing(B, CALLER, &cx); h_releasing(A, CALLER, &cx);              /* clean release */
        h_acquired(B, CALLER); h_acquiring(A, CALLER);                         /* inversion -> HIERARCHY */
        int clog = g_log; BugCheckRs cb{};
        compare("hierarchy", R_HIERARCHY, (uintptr_t)A, (uintptr_t)B, (uintptr_t)CALLER, 0, cb, clog, neg);
        g_cur_thread = T1; h_releasing(B, CALLER, &cb);   /* cleanup (A was never held-recorded on the acquiring call) */
    }
    /* ---- clean no-report coverage: acquire + same-thread release, nothing fires ---- */
    {
        fresh();
        PVOID L = (PVOID)0x700000;
        g_cur_thread = T1; h_acquired(L, CALLER);
        BugCheckRs cb{}; int cr = h_releasing(L, CALLER, &cb);
        g_checks++;
        if (cr || g_log) { dis("clean.silent", cr, cb, 0, cb); } else { g_silent++; }
    }
}

int main(void) {
    run(0);
    std::printf("[vf-deadlock] checks=%ld disagreements=%ld (fatal=%ld logged=%ld silent=%ld) rhits=%u chits=%u\n",
                g_checks, g_dis, g_fatal, g_logged, g_silent, r_hits, VfDeadlockRuleHits);
    long dis0 = g_dis;
    bool nonvacuous = (g_fatal >= 2 && g_logged >= 3 && g_silent >= 1);
    bool hits_synced = (r_hits == VfDeadlockRuleHits);

    long savedchecks = g_checks; g_dis = 0; g_checks = 0; r_hits = 0;
    VfInitializeDeadlockDetection();           /* reset C rule hits via a fresh process-wide drive is N/A; counter persists */
    /* Negative control: VfDeadlockRuleHits keeps counting in C; we only need Rust to disagree. */
    run(1);
    long negdis = g_dis; g_checks = savedchecks;
    std::printf("[vf-deadlock] negative-control disagreements=%ld (expect > 0)\n", negdis);

    if (dis0 != 0) { std::fprintf(stderr, "FAIL: %ld disagreements\n", dis0); return 1; }
    if (!nonvacuous) { std::fprintf(stderr, "FAIL: vacuous coverage (fatal=%ld logged=%ld silent=%ld)\n", g_fatal, g_logged, g_silent); return 1; }
    if (!hits_synced) { std::fprintf(stderr, "FAIL: hit counter desync r=%u c=%u\n", r_hits, VfDeadlockRuleHits); return 1; }
    if (negdis == 0) { std::fprintf(stderr, "FAIL: negative control did not bite\n"); return 1; }
    std::printf("RESULT=PASS\n");
    return 0;
}
