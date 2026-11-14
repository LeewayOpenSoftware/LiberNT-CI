/* LRM-RS Driver-Verifier Stage 3 FULL-STACK differential: the real vfio.c I/O-manager rules
 * (compiled UNMODIFIED against a host <ntoskrnl.h> stub, over mock Io/Ke/Ex services) vs the Rust
 * decisions (lrm_vfio_*). Drives, through the kernel's own call paths — the exported VfIoThunks[]
 * replacement wrappers (VfIoCallDriver / VfIoCompleteRequest / VfIoFreeIrp / VfIoReuseIrp):
 *   - the hard rules 0x10/0x03/0x04 (call), 0x05 (call-return IRQL), 0x0E/0x06/0x07 (complete),
 *     0x01/0x02 (free), captured with setjmp/longjmp and compared on code + all four params;
 *   - the soft (table) rules 0x203 (fatal), 0x205/0x209/0x310 (log), 0x20B (fatal, and WITHHELD to a
 *     log line once VfIrpUntrackSkipped != 0) — each gated on a *pended* IRP entry primed through a
 *     real IoCallDriver that returns STATUS_PENDING, with the pended-gate and the untrack-skipped
 *     guard both exercised, and the shared 64-line report cap — compared, per violation, against
 *     lrm_vfio_soft_eval. Exit 0 iff 0 disagreements AND coverage is non-vacuous; a negative control
 *     (perturb the determining input fed to the Rust side) must disagree. The soft `caller` is a
 *     return address (may be a kernel address on a driver tail-call), so for the fatal soft rules it
 *     is taken from the C bug check and fed to the Rust side; all other params are predicted. Every
 *     verified driver sets DriverObject == NULL here, so VfpCallerOwnsCurrentLocation is always
 *     FALSE (caller does not own the stack location) and the soft call-site conditions reduce to the
 *     pended gate, matching the harness `owns = 0` fed to the Rust side.
 * Leeway Open Software. Test-only. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csetjmp>

extern "C" {
#include <ntoskrnl.h>
extern const VF_THUNK VfIoThunks[];
extern const ULONG VfIoThunkCount;
extern void VfInitializeIrpTracking(void);
extern BOOLEAN VfIrpTrackingEnabled;
extern ULONG VfIrpRuleHits;
extern ULONG VfIrpUntrackSkipped;
}

/* Rust ABI (vf.rs) */
struct BugCheckRs { uint32_t code; uintptr_t p1, p2, p3, p4; };
extern "C" uint8_t lrm_vfio_check_call(uint8_t irql, uintptr_t irp, uint8_t irp_is_irp, uintptr_t dev, uint8_t dev_is_dev, BugCheckRs *out);
extern "C" uint8_t lrm_vfio_check_call_return(uintptr_t dev, uint8_t before, uint8_t after, BugCheckRs *out);
extern "C" uint8_t lrm_vfio_check_complete(uint8_t irql, uintptr_t irp, int32_t status, uintptr_t cancel, BugCheckRs *out);
extern "C" uint8_t lrm_vfio_free_irp(uintptr_t irp, uint8_t is_irp, uint8_t thread_list_empty, BugCheckRs *out);
extern "C" uint8_t lrm_vfio_soft_eval(uint32_t rule, uint8_t cancel_present, uint8_t pended, uint8_t caller_owns,
                                      uintptr_t caller, uintptr_t irp, uint32_t untrack_skipped,
                                      uint32_t *hits, BugCheckRs *out_bc, uint32_t *out_rule);

/* rule sub-codes */
#define IO_CALL_CANCEL    0x203u
#define IO_CALL_BELOW     0x205u
#define IO_COMPLETE_BELOW 0x209u
#define IO_FREE_IN_USE    0x20Bu
#define IO_REINIT_IN_USE  0x310u

/* ---- C-side kernel stubs ---- */
static jmp_buf g_jmp;
static BugCheckRs g_cbc;
static int g_craised, g_log;
static KIRQL g_irql, g_after;
static NTSTATUS g_call_status;

extern "C" {
KIRQL KeGetCurrentIrql(void) { return g_irql; }
unsigned long DbgPrint(const char *, ...) { g_log++; return 0; }
void KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4) {
    g_cbc = { (uint32_t)Code, (uintptr_t)P1, (uintptr_t)P2, (uintptr_t)P3, (uintptr_t)P4 };
    g_craised = 1; longjmp(g_jmp, 1);
}
PVOID ExAllocatePoolWithTag(POOL_TYPE, SIZE_T b, ULONG) { return malloc(b ? b : 1); }
void  ExFreePoolWithTag(PVOID p, ULONG) { free(p); }
/* The IRP primitives the thunks wrap. IofCallDriver is the only one that must behave: it changes the
 * running IRQL (to exercise 0x05) and returns the primed status (to pend an entry). */
NTSTATUS FASTCALL IofCallDriver(PDEVICE_OBJECT, PIRP) { g_irql = g_after; return g_call_status; }
VOID FASTCALL IofCompleteRequest(PIRP, CCHAR) {}
NTSTATUS NTAPI IoCallDriver(PDEVICE_OBJECT, PIRP) { g_irql = g_after; return g_call_status; }
VOID NTAPI IoCompleteRequest(PIRP, CCHAR) {}
VOID NTAPI IoFreeIrp(PIRP) {}
VOID NTAPI IoReuseIrp(PIRP, NTSTATUS) {}
VOID NTAPI IoInitializeIrp(PIRP, USHORT, CCHAR) {}
}

/* thunk shapes (VfIoThunks order) */
typedef NTSTATUS (*FnCall)(PDEVICE_OBJECT, PIRP);
typedef void (*FnComplete)(PIRP, CCHAR);
typedef void (*FnFree)(PIRP);
typedef void (*FnReuse)(PIRP, NTSTATUS);
enum { T_IOFCALL = 0, T_IOCALL, T_IOFCOMPLETE, T_IOCOMPLETE, T_IOFREE, T_IOREUSE, T_IOINITIRP };

/* setjmp-bearing drive helpers (setjmp in its own frame). */
static int c_call(PDEVICE_OBJECT dev, PIRP irp, KIRQL before, KIRQL after, NTSTATUS status, BugCheckRs *out) {
    FnCall fn = (FnCall)VfIoThunks[T_IOCALL].Replacement;
    g_irql = before; g_after = after; g_call_status = status; g_craised = 0; g_log = 0;
    if (setjmp(g_jmp) == 0) { (void)fn(dev, irp); return 0; }
    *out = g_cbc; return 1;
}
static int c_complete(PIRP irp, KIRQL irql, BugCheckRs *out) {
    FnComplete fn = (FnComplete)VfIoThunks[T_IOCOMPLETE].Replacement;
    g_irql = irql; g_craised = 0; g_log = 0;
    if (setjmp(g_jmp) == 0) { fn(irp, 0); return 0; }
    *out = g_cbc; return 1;
}
static int c_free(PIRP irp, BugCheckRs *out) {
    FnFree fn = (FnFree)VfIoThunks[T_IOFREE].Replacement;
    g_irql = 0; g_craised = 0; g_log = 0;
    if (setjmp(g_jmp) == 0) { fn(irp); return 0; }
    *out = g_cbc; return 1;
}
static int c_reuse(PIRP irp, BugCheckRs *out) {
    FnReuse fn = (FnReuse)VfIoThunks[T_IOREUSE].Replacement;
    g_irql = 0; g_craised = 0; g_log = 0;
    if (setjmp(g_jmp) == 0) { fn(irp, 0); return 0; }
    *out = g_cbc; return 1;
}

static long g_checks = 0, g_dis = 0, g_hraised = 0, g_sfatal = 0, g_slog = 0, g_sok = 0;
static uint32_t r_hits = 0;   /* mirrors VfIrpRuleHits */

static bool bceq(const BugCheckRs &c, const BugCheckRs &r) {
    return c.code == r.code && c.p1 == r.p1 && c.p2 == r.p2 && c.p3 == r.p3 && c.p4 == r.p4;
}
static void dis(const char *w, int cr, const BugCheckRs &c, int rr, const BugCheckRs &r) {
    g_dis++;
    if (g_dis <= 25)
        std::fprintf(stderr, "DISAGREE #%ld %s  C{r=%d %08x %zx %zx %zx %zx}  R{r=%d %08x %zx %zx %zx %zx}\n",
            g_dis, w, cr, c.code, (size_t)c.p1, (size_t)c.p2, (size_t)c.p3, (size_t)c.p4,
            rr, r.code, (size_t)r.p1, (size_t)r.p2, (size_t)r.p3, (size_t)r.p4);
}
static void cmp_hard(const char *w, int cr, const BugCheckRs &cb, int rr, const BugCheckRs &rb) {
    g_checks++; if (cr) g_hraised++;
    if (cr != rr || (cr && !bceq(cb, rb))) dis(w, cr, cb, rr, rb);
}

/* A verified-driver device whose DriverObject is NULL: VfpCallerOwnsCurrentLocation -> FALSE. */
static DEVICE_OBJECT g_gooddev, g_baddev, g_sldev;
static IO_STACK_LOCATION g_sl;

/* distinct IRP addresses per test (so one test's leaked pended entry never collides with the next) */
static IRP g_irp_pool[48];
static int g_irp_n;
static PIRP NI(CSHORT type) {
    PIRP irp = &g_irp_pool[g_irp_n++];
    memset(irp, 0, sizeof *irp);
    irp->Type = type;
    irp->CurrentLocation = 1; irp->StackCount = 1;
    InitializeListHead(&irp->ThreadListEntry);
    irp->IoStatus.Status = 0;
    irp->CancelRoutine = NULL;
    irp->CurrentStackLocation = &g_sl;   /* valid: DeviceObject set, its DriverObject NULL -> owns FALSE */
    return irp;
}
/* Prime a pended tracking entry for `irp` via a real IoCallDriver returning STATUS_PENDING. */
static void prime_pended(PDEVICE_OBJECT dev, PIRP irp) {
    irp->CurrentLocation = 1; irp->StackCount = 1; irp->CancelRoutine = NULL;
    BugCheckRs cb{};
    (void)c_call(dev, irp, 0, 0, STATUS_PENDING, &cb);
}

/* Compare a soft-rule drive (C observable: fatal cb / log / silent) with lrm_vfio_soft_eval. */
static void cmp_soft(const char *w, uint32_t rule, uint8_t cancel, uint8_t pended, uint8_t owns,
                     uintptr_t irp, uint32_t untrack, int cr, const BugCheckRs &cb, int clog, int neg) {
    uint8_t n_cancel = cancel, n_pended = pended;
    if (neg) {
        /* perturb the determining gate so the Rust outcome must flip */
        if (rule == IO_CALL_CANCEL) n_cancel = !cancel; else n_pended = !pended;
    }
    uintptr_t caller = cr ? cb.p2 : 0;   /* fatal soft: take the return address from the C bug check */
    BugCheckRs rb{}; uint32_t out_rule = 0;
    uint8_t disc = lrm_vfio_soft_eval(rule, n_cancel, n_pended, owns, caller, irp, untrack, &r_hits, &rb, &out_rule);
    g_checks++;
    int c_obs = cr ? 2 : (clog > 0 ? 1 : 0);
    if (!neg) { if (cr) g_sfatal++; else if (clog > 0) g_slog++; else g_sok++; }
    if (c_obs != disc) { dis(w, cr, cb, disc, rb); return; }
    if (c_obs == 2 && !bceq(cb, rb)) { dis(w, cr, cb, disc, rb); return; }
    if (c_obs == 1 && out_rule != rule) { dis(w, cr, cb, disc, rb); return; }
}

static void run(int neg) {
    VfInitializeIrpTracking();          /* clears the buckets (orphans prior entries) -> fresh state */
    g_irp_n = 0;
    g_gooddev = {}; g_gooddev.Type = IO_TYPE_DEVICE; g_gooddev.DriverObject = NULL;
    g_baddev = {}; g_baddev.Type = 99;
    g_sldev = {}; g_sldev.Type = IO_TYPE_DEVICE; g_sldev.DriverObject = NULL;
    g_sl = {}; g_sl.DeviceObject = &g_sldev;
    PDEVICE_OBJECT gd = &g_gooddev, bd = &g_baddev;
    BugCheckRs cb{}, rb{}; int cr; uint8_t rr;

    /* ---------- hard rules ---------- */
    /* 0x10 CALL_IRQL: irql 3 > DISPATCH */
    { PIRP irp = NI(IO_TYPE_IRP);
      cr = c_call(gd, irp, 3, 3, 0, &cb);
      rr = lrm_vfio_check_call(neg ? 1 : 3, (uintptr_t)irp, 1, (uintptr_t)gd, 1, &rb);
      cmp_hard("call.irql", cr, cb, rr, rb); }
    /* 0x03 CALL_NOT_AN_IRP */
    { PIRP irp = NI(99);
      cr = c_call(gd, irp, 0, 0, 0, &cb);
      rr = lrm_vfio_check_call(0, (uintptr_t)irp, neg ? 1 : 0, (uintptr_t)gd, 1, &rb);
      cmp_hard("call.not_irp", cr, cb, rr, rb); }
    /* 0x04 CALL_BAD_DEVICE */
    { PIRP irp = NI(IO_TYPE_IRP);
      cr = c_call(bd, irp, 0, 0, 0, &cb);
      rr = lrm_vfio_check_call(0, (uintptr_t)irp, 1, (uintptr_t)bd, neg ? 1 : 0, &rb);
      cmp_hard("call.bad_dev", cr, cb, rr, rb); }
    /* 0x05 CALL_IRQL_CHANGED: before 0, after 1 */
    { PIRP irp = NI(IO_TYPE_IRP);
      cr = c_call(gd, irp, 0, 1, 0, &cb);
      rr = lrm_vfio_check_call_return((uintptr_t)gd, 0, neg ? 0 : 1, &rb);
      cmp_hard("call.irql_changed", cr, cb, rr, rb); }
    /* 0x0E COMPLETE_IRQL */
    { PIRP irp = NI(IO_TYPE_IRP);
      cr = c_complete(irp, 3, &cb);
      rr = lrm_vfio_check_complete(neg ? 0 : 3, (uintptr_t)irp, 0, 0, &rb);
      cmp_hard("complete.irql", cr, cb, rr, rb); }
    /* 0x06 COMPLETE_PENDING_STATUS: STATUS_PENDING */
    { PIRP irp = NI(IO_TYPE_IRP); irp->IoStatus.Status = STATUS_PENDING;
      cr = c_complete(irp, 0, &cb);
      rr = lrm_vfio_check_complete(0, (uintptr_t)irp, neg ? 0 : (int32_t)STATUS_PENDING, 0, &rb);
      cmp_hard("complete.pending", cr, cb, rr, rb); }
    /* 0x06 COMPLETE_PENDING_STATUS: status -1 (sign-extended param) */
    { PIRP irp = NI(IO_TYPE_IRP); irp->IoStatus.Status = (NTSTATUS)-1;
      cr = c_complete(irp, 0, &cb);
      rr = lrm_vfio_check_complete(0, (uintptr_t)irp, neg ? 0 : -1, 0, &rb);
      cmp_hard("complete.status_m1", cr, cb, rr, rb); }
    /* 0x07 COMPLETE_CANCEL_ROUTINE */
    { PIRP irp = NI(IO_TYPE_IRP); irp->CancelRoutine = (PVOID)0xCA11;
      cr = c_complete(irp, 0, &cb);
      rr = lrm_vfio_check_complete(0, (uintptr_t)irp, 0, neg ? 0 : (uintptr_t)0xCA11, &rb);
      cmp_hard("complete.cancel", cr, cb, rr, rb); }
    /* 0x01 FREE_NOT_AN_IRP */
    { PIRP irp = NI(99);
      cr = c_free(irp, &cb);
      rr = lrm_vfio_free_irp((uintptr_t)irp, neg ? 1 : 0, 1, &rb);
      cmp_hard("free.not_irp", cr, cb, rr, rb); }
    /* 0x02 FREE_QUEUED_TO_THREAD: thread list non-empty */
    { PIRP irp = NI(IO_TYPE_IRP);
      static LIST_ENTRY other;
      irp->ThreadListEntry.Flink = &other; irp->ThreadListEntry.Blink = &other;   /* non-empty */
      cr = c_free(irp, &cb);
      rr = lrm_vfio_free_irp((uintptr_t)irp, 1, neg ? 1 : 0, &rb);
      cmp_hard("free.queued", cr, cb, rr, rb); }

    /* ---------- soft rules (require the tracker) ---------- */
    /* 0x203 CALL_CANCEL_ROUTINE (fatal, not pended-gated) */
    { PIRP irp = NI(IO_TYPE_IRP); irp->CancelRoutine = (PVOID)0xCA12;
      cr = c_call(gd, irp, 0, 0, 0, &cb); int clog = g_log;
      cmp_soft("soft.cancel_203", IO_CALL_CANCEL, 1, 0, 0, (uintptr_t)irp, 0, cr, cb, clog, neg); }
    /* 0x205 CALL_QUEUED_BELOW (log): pended + caller !owns */
    { PIRP irp = NI(IO_TYPE_IRP);
      prime_pended(gd, irp);
      irp->CurrentLocation = 2; irp->StackCount = 1;   /* owns -> FALSE (also DriverObject NULL) */
      cr = c_call(gd, irp, 0, 0, 0, &cb); int clog = g_log;
      cmp_soft("soft.call_below_205", IO_CALL_BELOW, 0, 1, 0, (uintptr_t)irp, 0, cr, cb, clog, neg); }
    /* pended-gate coverage: NOT pended -> no report */
    { PIRP irp = NI(IO_TYPE_IRP);
      irp->CurrentLocation = 2; irp->StackCount = 1;
      cr = c_call(gd, irp, 0, 0, 0, &cb); int clog = g_log;
      cmp_soft("soft.call_notpended", IO_CALL_BELOW, 0, 0, 0, (uintptr_t)irp, 0, cr, cb, clog, neg); }
    /* 0x209 COMPLETE_QUEUED_BELOW (log): pended + caller !owns */
    { PIRP irp = NI(IO_TYPE_IRP);
      prime_pended(gd, irp);
      irp->CurrentLocation = 2; irp->StackCount = 1; irp->IoStatus.Status = 0; irp->CancelRoutine = NULL;
      cr = c_complete(irp, 0, &cb); int clog = g_log;
      cmp_soft("soft.complete_below_209", IO_COMPLETE_BELOW, 0, 1, 0, (uintptr_t)irp, 0, cr, cb, clog, neg); }
    /* 0x310 REINITIALIZE_IN_USE (log): pended */
    { PIRP irp = NI(IO_TYPE_IRP);
      prime_pended(gd, irp);
      cr = c_reuse(irp, &cb); int clog = g_log;
      cmp_soft("soft.reinit_310", IO_REINIT_IN_USE, 0, 1, 0, (uintptr_t)irp, 0, cr, cb, clog, neg); }
    /* 0x20B FREE_IN_USE (fatal when untrack_skipped == 0) */
    { PIRP irp = NI(IO_TYPE_IRP);
      prime_pended(gd, irp);
      VfIrpUntrackSkipped = 0;
      cr = c_free(irp, &cb); int clog = g_log;
      cmp_soft("soft.free_in_use_20B.fatal", IO_FREE_IN_USE, 0, 1, 0, (uintptr_t)irp, 0, cr, cb, clog, neg); }
    /* 0x20B FREE_IN_USE WITHHELD to a log line when untrack_skipped != 0 */
    { PIRP irp = NI(IO_TYPE_IRP);
      prime_pended(gd, irp);
      VfIrpUntrackSkipped = 1;
      cr = c_free(irp, &cb); int clog = g_log;
      cmp_soft("soft.free_in_use_20B.withheld", IO_FREE_IN_USE, 0, 1, 0, (uintptr_t)irp, 1, cr, cb, clog, neg);
      VfIrpUntrackSkipped = 0; }
}

/* The shared 64-line cap: drive 0x205 (log only) past VF_IRP_LINES_PRINTED and confirm C stops
 * logging while Rust returns Ok (both counted, both suppressed). Resets the shared counter first and
 * reuses one IRP (the 0x205 path removes its own entry each iteration). */
static long g_capchecks = 0, g_capdis = 0;
static void cap_test(void) {
    VfInitializeIrpTracking();
    VfIrpRuleHits = 0; uint32_t cap_hits = 0;
    DEVICE_OBJECT gd{}; gd.Type = IO_TYPE_DEVICE; gd.DriverObject = NULL;
    IO_STACK_LOCATION sl{}; DEVICE_OBJECT sldev{}; sldev.Type = IO_TYPE_DEVICE; sl.DeviceObject = &sldev;
    IRP irp;
    for (int i = 0; i < 70; i++) {
        memset(&irp, 0, sizeof irp); irp.Type = IO_TYPE_IRP;
        InitializeListHead(&irp.ThreadListEntry); irp.CurrentStackLocation = &sl;
        prime_pended(&gd, &irp);
        irp.CurrentLocation = 2; irp.StackCount = 1;   /* owns FALSE */
        BugCheckRs cb{}; int cr = c_call(&gd, &irp, 0, 0, 0, &cb); int clog = g_log;
        BugCheckRs rb{}; uint32_t out_rule = 0;
        uint8_t disc = lrm_vfio_soft_eval(IO_CALL_BELOW, 0, 1, 0, 0, (uintptr_t)&irp, 0, &cap_hits, &rb, &out_rule);
        g_capchecks++;
        int c_obs = cr ? 2 : (clog > 0 ? 1 : 0);   /* <=64 -> 1 (log); >64 -> 0 (silent) */
        if (c_obs != disc) { g_capdis++; if (g_capdis <= 10) std::fprintf(stderr, "CAP DISAGREE i=%d c=%d r=%d\n", i, c_obs, disc); }
    }
    if (cap_hits != VfIrpRuleHits)
        std::fprintf(stderr, "CAP hit desync r=%u c=%u\n", cap_hits, VfIrpRuleHits);
}

int main(void) {
    run(0);
    cap_test();
    std::printf("[vf-io] checks=%ld disagreements=%ld (hard_raised=%ld soft_fatal=%ld soft_log=%ld soft_ok=%ld) cap:checks=%ld dis=%ld thunks=%u\n",
                g_checks, g_dis, g_hraised, g_sfatal, g_slog, g_sok, g_capchecks, g_capdis, (unsigned)VfIoThunkCount);
    long dis0 = g_dis + g_capdis;
    bool nonvacuous = (g_hraised >= 10 && g_sfatal >= 2 && g_slog >= 3 && g_sok >= 1 && g_capchecks > 64);

    long savedchecks = g_checks; g_dis = 0; g_checks = 0;
    run(1);
    long negdis = g_dis; g_checks = savedchecks;
    std::printf("[vf-io] negative-control disagreements=%ld (expect > 0)\n", negdis);

    if (dis0 != 0) { std::fprintf(stderr, "FAIL: %ld disagreements\n", dis0); return 1; }
    if (!nonvacuous) { std::fprintf(stderr, "FAIL: vacuous coverage (hard=%ld fatal=%ld log=%ld ok=%ld cap=%ld)\n", g_hraised, g_sfatal, g_slog, g_sok, g_capchecks); return 1; }
    if (negdis == 0) { std::fprintf(stderr, "FAIL: negative control did not bite\n"); return 1; }
    std::printf("RESULT=PASS\n");
    return 0;
}
