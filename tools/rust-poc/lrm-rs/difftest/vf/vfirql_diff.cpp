/* LRM-RS Driver-Verifier Stage 2a FULL-STACK differential: the real vfirql.c IRQL rules (compiled
 * UNMODIFIED against a minimal <ntoskrnl.h> stub, reached through the exported VfIrqlThunks[] table
 * exactly as the kernel calls them) vs the Rust rules (lrm_vf_*). For every IRQL at each decision
 * boundary (and, for waits, each timeout shape), compare raised-vs-not and the exact 0xC4 sub-code
 * and all four parameters. C bug checks are captured via setjmp/longjmp. Exit 0 iff 0 disagreements
 * AND both raised and not-raised occurred for each rule (non-vacuous). A negative control (perturb
 * the IRQL fed to the Rust side) must produce disagreements. Leeway Open Software. Test-only. */
#include <cstdint>
#include <cstdio>
#include <csetjmp>

extern "C" {
#include <ntoskrnl.h>
extern const VF_THUNK VfIrqlThunks[];
extern const ULONG VfIrqlThunkCount;
}

/* Rust ABI (vf.rs) */
struct BugCheckRs { uint32_t code; uintptr_t p1, p2, p3, p4; };
extern "C" uint8_t lrm_vf_acquire_spinlock_at_dpc(uint8_t irql, uintptr_t p, BugCheckRs *out);
extern "C" uint8_t lrm_vf_release_spinlock_from_dpc(uint8_t irql, uintptr_t p, BugCheckRs *out);
extern "C" uint8_t lrm_vf_acquire_spinlock_raise_to_dpc(uint8_t irql, uintptr_t p, BugCheckRs *out);
extern "C" uint8_t lrm_vf_release_spinlock(uint8_t irql, uintptr_t p, BugCheckRs *out);
extern "C" uint8_t lrm_vf_acquire_fast_mutex(uint8_t irql, uintptr_t p, BugCheckRs *out);
extern "C" uint8_t lrm_vf_release_fast_mutex(uint8_t irql, uintptr_t mutex, uintptr_t cad, BugCheckRs *out);
extern "C" uint8_t lrm_vf_check_wait(uint8_t irql, uintptr_t object, uintptr_t timeout, uint8_t tzero, BugCheckRs *out);

/* ---- C-side kernel stubs ---- */
static jmp_buf g_jmp;
static BugCheckRs g_cbc;
static int g_craised;
static KIRQL g_irql;
static KTHREAD g_thread;

extern "C" {
KIRQL KeGetCurrentIrql(void) { return g_irql; }
PKTHREAD KeGetCurrentThread(void) { return &g_thread; }
void KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4) {
    g_cbc = { (uint32_t)Code, (uintptr_t)P1, (uintptr_t)P2, (uintptr_t)P3, (uintptr_t)P4 };
    g_craised = 1;
    longjmp(g_jmp, 1);
}
/* Primitives the thunks wrap (no-ops; reached only when no bug check fires). */
void KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK) {}
void KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK) {}
void ExAcquireFastMutex(PFAST_MUTEX) {}
void ExReleaseFastMutex(PFAST_MUTEX) {}
KIRQL KeAcquireSpinLockRaiseToDpc(PKSPIN_LOCK) { return 0; }
void KeReleaseSpinLock(PKSPIN_LOCK, KIRQL) {}
NTSTATUS KeWaitForSingleObject(PVOID, KWAIT_REASON, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER) { return 0; }
NTSTATUS KeWaitForMultipleObjects(ULONG, PVOID[], WAIT_TYPE, KWAIT_REASON, KPROCESSOR_MODE,
                                  BOOLEAN, PLARGE_INTEGER, PKWAIT_BLOCK) { return 0; }
}

/* Thunk function-pointer shapes (x64 / _WIN64 order). */
typedef void     (*FnVoidLock)(void *);
typedef KIRQL    (*FnRaise)(void *);
typedef void     (*FnRelSpin)(void *, KIRQL);
typedef NTSTATUS (*FnWait1)(void *, KWAIT_REASON, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER);
typedef NTSTATUS (*FnWaitN)(ULONG, PVOID *, WAIT_TYPE, KWAIT_REASON, KPROCESSOR_MODE, BOOLEAN,
                            PLARGE_INTEGER, PKWAIT_BLOCK);

enum { T_ACQ_AT_DPC = 0, T_REL_FROM_DPC, T_ACQ_RAISE, T_REL_SPIN, T_ACQ_FM, T_REL_FM, T_WAIT1, T_WAIT_N };

struct Outcome { int raised; BugCheckRs bc; };
static long g_checks = 0, g_dis = 0, g_raised = 0, g_ok = 0;

static void dis(const char *what, uint8_t irql, const Outcome &c, const Outcome &r) {
    g_dis++;
    if (g_dis <= 25)
        std::fprintf(stderr, "DISAGREE #%ld %s irql=%u  C{r=%d %08x %zx %zx %zx %zx}  R{r=%d %08x %zx %zx %zx %zx}\n",
            g_dis, what, irql, c.raised, c.bc.code, (size_t)c.bc.p1, (size_t)c.bc.p2, (size_t)c.bc.p3, (size_t)c.bc.p4,
            r.raised, r.bc.code, (size_t)r.bc.p1, (size_t)r.bc.p2, (size_t)r.bc.p3, (size_t)r.bc.p4);
}
static bool eq(const Outcome &c, const Outcome &r) {
    if (c.raised != r.raised) return false;
    if (!c.raised) return true;
    return c.bc.code == r.bc.code && c.bc.p1 == r.bc.p1 && c.bc.p2 == r.bc.p2 && c.bc.p3 == r.bc.p3 && c.bc.p4 == r.bc.p4;
}

/* run a C void(lock) thunk */
static Outcome c_void(FnVoidLock fn, void *lock, KIRQL irql) {
    Outcome o{}; g_irql = irql; g_craised = 0;
    if (setjmp(g_jmp) == 0) { fn(lock); o.raised = 0; } else { o.raised = 1; o.bc = g_cbc; }
    return o;
}
static Outcome c_raise(FnRaise fn, void *lock, KIRQL irql) {
    Outcome o{}; g_irql = irql; g_craised = 0;
    if (setjmp(g_jmp) == 0) { (void)fn(lock); o.raised = 0; } else { o.raised = 1; o.bc = g_cbc; }
    return o;
}
static Outcome c_relspin(FnRelSpin fn, void *lock, KIRQL irql) {
    Outcome o{}; g_irql = irql; g_craised = 0;
    if (setjmp(g_jmp) == 0) { fn(lock, 0); o.raised = 0; } else { o.raised = 1; o.bc = g_cbc; }
    return o;
}
static Outcome c_wait(FnWait1 fn, void *obj, PLARGE_INTEGER to, KIRQL irql) {
    Outcome o{}; g_irql = irql; g_craised = 0;
    if (setjmp(g_jmp) == 0) { (void)fn(obj, 0, 0, 0, to); o.raised = 0; } else { o.raised = 1; o.bc = g_cbc; }
    return o;
}
static Outcome c_waitN(FnWaitN fn, PVOID *objarr, PLARGE_INTEGER to, KIRQL irql) {
    Outcome o{}; g_irql = irql; g_craised = 0;
    if (setjmp(g_jmp) == 0) { (void)fn(1, objarr, 0, 0, 0, 0, to, NULL); o.raised = 0; }
    else { o.raised = 1; o.bc = g_cbc; }
    return o;
}

static Outcome rwrap(uint8_t rc, const BugCheckRs &bc) { Outcome o{}; o.raised = rc; if (rc) o.bc = bc; return o; }

static void tally(const Outcome &c) { if (c.raised) g_raised++; else g_ok++; }

static void run(int neg) {
    void *lock = (void *)0x1000, *mutex = (void *)0x2000, *obj = (void *)0x3000;
    g_thread.CombinedApcDisable = 0x55;
    FnVoidLock acqAtDpc = (FnVoidLock)VfIrqlThunks[T_ACQ_AT_DPC].Replacement;
    FnVoidLock relFromDpc = (FnVoidLock)VfIrqlThunks[T_REL_FROM_DPC].Replacement;
    FnRaise    acqRaise = (FnRaise)VfIrqlThunks[T_ACQ_RAISE].Replacement;
    FnRelSpin  relSpin = (FnRelSpin)VfIrqlThunks[T_REL_SPIN].Replacement;
    FnVoidLock acqFm = (FnVoidLock)VfIrqlThunks[T_ACQ_FM].Replacement;
    FnVoidLock relFm = (FnVoidLock)VfIrqlThunks[T_REL_FM].Replacement;
    FnWait1    wait1 = (FnWait1)VfIrqlThunks[T_WAIT1].Replacement;
    FnWaitN    waitN = (FnWaitN)VfIrqlThunks[T_WAIT_N].Replacement; /* same VfpCheckWait */
    PVOID      objarr[1] = { obj };

    for (uint8_t irql = 0; irql <= 4; irql++) {
        uint8_t ri = neg ? (uint8_t)(irql ^ 2) : irql;   /* negative control: perturb Rust IRQL */
        BugCheckRs bc;
        Outcome c, r;

        c = c_void(acqAtDpc, lock, irql);
        r = rwrap(lrm_vf_acquire_spinlock_at_dpc(ri, (uintptr_t)lock, &bc), bc);
        g_checks++; tally(c); if (!eq(c, r)) dis("acq_at_dpc", irql, c, r);

        c = c_void(relFromDpc, lock, irql);
        r = rwrap(lrm_vf_release_spinlock_from_dpc(ri, (uintptr_t)lock, &bc), bc);
        g_checks++; tally(c); if (!eq(c, r)) dis("rel_from_dpc", irql, c, r);

        c = c_raise(acqRaise, lock, irql);
        r = rwrap(lrm_vf_acquire_spinlock_raise_to_dpc(ri, (uintptr_t)lock, &bc), bc);
        g_checks++; tally(c); if (!eq(c, r)) dis("acq_raise", irql, c, r);

        c = c_relspin(relSpin, lock, irql);
        r = rwrap(lrm_vf_release_spinlock(ri, (uintptr_t)lock, &bc), bc);
        g_checks++; tally(c); if (!eq(c, r)) dis("rel_spin", irql, c, r);

        c = c_void(acqFm, mutex, irql);
        r = rwrap(lrm_vf_acquire_fast_mutex(ri, (uintptr_t)mutex, &bc), bc);
        g_checks++; tally(c); if (!eq(c, r)) dis("acq_fast_mutex", irql, c, r);

        c = c_void(relFm, mutex, irql);
        r = rwrap(lrm_vf_release_fast_mutex(ri, (uintptr_t)mutex, (uintptr_t)g_thread.CombinedApcDisable, &bc), bc);
        g_checks++; tally(c); if (!eq(c, r)) dis("rel_fast_mutex", irql, c, r);

        /* waits: NULL timeout, non-NULL zero timeout, non-NULL non-zero timeout */
        LARGE_INTEGER zero; zero.QuadPart = 0;
        LARGE_INTEGER nz; nz.QuadPart = -1000;
        PLARGE_INTEGER tos[3] = { NULL, &zero, &nz };
        for (int ti = 0; ti < 3; ti++) {
            PLARGE_INTEGER to = tos[ti];
            uint8_t tzero = (to != NULL && to->QuadPart == 0) ? 1 : 0;
            uintptr_t top = (uintptr_t)to;
            c = c_wait(wait1, obj, to, irql);
            r = rwrap(lrm_vf_check_wait(ri, (uintptr_t)obj, top, tzero, &bc), bc);
            g_checks++; tally(c); if (!eq(c, r)) dis("wait1", irql, c, r);
            c = c_waitN(waitN, objarr, to, irql);
            r = rwrap(lrm_vf_check_wait(ri, (uintptr_t)objarr, top, tzero, &bc), bc);
            g_checks++; tally(c); if (!eq(c, r)) dis("waitN", irql, c, r);
        }
    }
}

int main(void) {
    run(0);
    std::printf("[vf-irql] checks=%ld disagreements=%ld (raised=%ld ok=%ld) thunks=%lu\n",
                g_checks, g_dis, g_raised, g_ok, (unsigned long)VfIrqlThunkCount);
    long dis0 = g_dis;
    bool nonvacuous = (g_raised > 0 && g_ok > 0);

    long savedchecks = g_checks; g_checks = 0; g_dis = 0;
    run(1);
    long negdis = g_dis;
    std::printf("[vf-irql] negative-control disagreements=%ld (expect > 0)\n", negdis);
    g_checks = savedchecks;

    if (dis0 != 0) { std::fprintf(stderr, "FAIL: %ld disagreements\n", dis0); return 1; }
    if (!nonvacuous) { std::fprintf(stderr, "FAIL: vacuous (missing raised or ok)\n"); return 1; }
    if (negdis == 0) { std::fprintf(stderr, "FAIL: negative control did not bite\n"); return 1; }
    std::printf("RESULT=PASS\n");
    return 0;
}
