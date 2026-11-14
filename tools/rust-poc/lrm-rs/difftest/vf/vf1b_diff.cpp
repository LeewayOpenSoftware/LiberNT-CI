/* LRM-RS Driver-Verifier Stage 1b FULL-STACK differential: the real ntdrvmgmt.c verifier APIs
 * (compiled UNMODIFIED against a host <ntoskrnl.h> stub, over mock kernel services) vs the Rust
 * (lrm_vf_*). Drives MmIsDriverVerifying, MmIsVerifierEnabled, MmAddVerifierThunks (validation
 * status codes), and MiInitializeDriverVerifier (level computation, read back via MmVerifierData),
 * with MiLookupDataTableEntry / MiIsSpecialPoolEnabled controlled by the harness. MiIsVerifiedDriverName
 * is static in the C (reached only via MiVerifyLoadedImage), so its Rust is unit-checked here, not
 * differentially. Exit 0 iff 0 disagreements AND non-vacuous; a negative control must disagree.
 * Leeway Open Software. Test-only. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <ntoskrnl.h>

/* ntdrvmgmt.c exports + the globals it defines (we read/set them). */
NTSTATUS NTAPI MmAddVerifierThunks(PVOID ThunkBuffer, ULONG ThunkBufferSize);
LOGICAL NTAPI MmIsDriverVerifying(PDRIVER_OBJECT DriverObject);
NTSTATUS NTAPI MmIsVerifierEnabled(ULONG *VerifierFlags);
void NTAPI MiInitializeDriverVerifier(void);
extern MM_DRIVER_VERIFIER_DATA MmVerifierData;
extern LIST_ENTRY MiVerifierDriverAddedThunkListHead;
extern WCHAR MmVerifyDriverBuffer[512];
extern ULONG MmVerifyDriverLevel;
}

/* Rust ABI (vf.rs) */
extern "C" uint32_t lrm_vf_is_verifier_enabled(uint8_t active, uint32_t level, uint32_t *out_flags);
extern "C" uint8_t lrm_vf_is_driver_verifying(uint8_t ldr_present, uint32_t flags);
extern "C" uint32_t lrm_vf_add_verifier_thunks(uint8_t list_init, size_t buf_size, size_t sizeof_pair,
    uint8_t module_found, uintptr_t module_base, uintptr_t module_end, uint8_t loaded_imports_invalid,
    const uintptr_t *pristines, size_t n);
extern "C" uint8_t lrm_vf_init_level(uint32_t verify_driver_level, uint8_t special_pool_enabled, uint8_t buffer_empty, uint32_t *out_level);
extern "C" uint8_t lrm_vf_is_verified_driver_name(const uint16_t *buffer, size_t blen, const uint16_t *name, size_t nlen, uint8_t allow_wildcard);
extern "C" uint8_t lrm_vf_boot_image_should_verify(uint8_t verifier_active, uintptr_t dll_base, uintptr_t ntos_base,
    uint8_t is_boot_driver, const uint16_t *verify_list, size_t vlen, const uint16_t *name, size_t nlen);

/* The boot walk itself (C). */
extern "C" void NTAPI MiVerifyBootLoadedImages(PLOADER_PARAMETER_BLOCK LoaderBlock);

/* ---- harness-controlled kernel stubs ---- */
static PLDR_DATA_TABLE_ENTRY g_lookup;     /* MiLookupDataTableEntry result */
static BOOLEAN g_special_pool;             /* MiIsSpecialPoolEnabled result */

extern "C" {
KMUTANT MmSystemLoadLock;
UCHAR MmDisablePagingExecutive = 1;
const VF_THUNK VfPoolThunks[] = { { 0, 0 } };
const ULONG VfPoolThunkCount = 0;
const VF_THUNK VfIrqlThunks[] = { { 0, 0 } };
const ULONG VfIrqlThunkCount = 0;
const VF_THUNK VfIoThunks[] = { { 0, 0 } };
const ULONG VfIoThunkCount = 0;
LIST_ENTRY PsLoadedModuleList;      /* the boot walk iterates this */
ULONG_PTR PsNtosImageBase;          /* kernel image base (excluded by the walk) */

unsigned long DbgPrint(const char *, ...) { return 0; }
PVOID ExAllocatePoolWithTag(POOL_TYPE, SIZE_T b, ULONG) { return malloc(b ? b : 1); }
void ExFreePoolWithTag(PVOID p, ULONG) { free(p); }
NTSTATUS KeWaitForSingleObject(PVOID, KWAIT_REASON, KPROCESSOR_MODE, BOOLEAN, PVOID) { return 0; }
LONG KeReleaseMutant(KMUTANT *, LONG, BOOLEAN, BOOLEAN) { return 0; }
PLDR_DATA_TABLE_ENTRY NTAPI MiLookupDataTableEntry(PVOID) { return g_lookup; }
BOOLEAN MiIsSpecialPoolEnabled(void) { return g_special_pool; }
void VfInitializePoolTracking(void) {}
void NTAPI VfInitializeIrpTracking(void) {}          /* vfio.c; the boot walk doesn't need real tracking */
void NTAPI VfInitializeDeadlockDetection(void) {}    /* vfdeadlock.c; ditto */
PVOID NTAPI RtlImageDirectoryEntryToData(PVOID, BOOLEAN, USHORT, ULONG *Size) { if (Size) *Size = 0; return NULL; }
BOOLEAN RtlEqualUnicodeString(PCUNICODE_STRING a, PCUNICODE_STRING b, BOOLEAN ci) {
    if (a->Length != b->Length) return FALSE;
    for (USHORT i = 0; i < a->Length / 2; i++) {
        WCHAR x = a->Buffer[i], y = b->Buffer[i];
        if (ci) { if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; }
        if (x != y) return FALSE;
    }
    return TRUE;
}
}

static long g_checks = 0, g_dis = 0, g_cov_true = 0, g_cov_false = 0;
static long g_cov_status[4] = {0,0,0,0}; /* SUCCESS, NOT_SUPPORTED, IP1, IP2 */

static void disu(const char *w, uint64_t c, uint64_t r) {
    g_dis++;
    if (g_dis <= 25) std::fprintf(stderr, "DISAGREE #%ld %s  C=0x%llx  R=0x%llx\n", g_dis, w,
                                  (unsigned long long)c, (unsigned long long)r);
}

/* ---- MmIsDriverVerifying ---- */
static void test_is_driver_verifying(int neg) {
    uint32_t flagsets[4] = { 0, LDRP_IMAGE_VERIFYING, 0x1, LDRP_IMAGE_VERIFYING | 0x4 };
    for (int present = 0; present < 2; present++) for (int fi = 0; fi < 4; fi++) {
        LDR_DATA_TABLE_ENTRY ldr; ldr.Flags = flagsets[fi];
        DRIVER_OBJECT drv; drv.DriverSection = present ? &ldr : NULL;
        uint8_t c = MmIsDriverVerifying(&drv) ? 1 : 0;
        uint32_t rf = neg ? (flagsets[fi] ^ LDRP_IMAGE_VERIFYING) : flagsets[fi];
        uint8_t r = lrm_vf_is_driver_verifying((uint8_t)present, present ? rf : 0);
        g_checks++; if (c) g_cov_true++; else g_cov_false++;
        if (c != r) disu("is_driver_verifying", c, r);
    }
}

/* ---- MmAddVerifierThunks ---- */
static int status_idx(uint32_t s) {
    switch (s) { case 0: return 0; case 0xC00000BB: return 1; case 0xC00000EF: return 2; case 0xC00000F0: return 3; }
    return -1;
}
static void test_add_thunks(int neg) {
    const size_t SP = sizeof(DRIVER_VERIFIER_THUNK_PAIRS);
    /* case 1: list not initialised (Flink == NULL) -> NOT_SUPPORTED. Must run before init. */
    MiVerifierDriverAddedThunkListHead.Flink = NULL;
    {
        DRIVER_VERIFIER_THUNK_PAIRS buf[2]; memset(buf, 0, sizeof buf);
        uint32_t c = (uint32_t)MmAddVerifierThunks(buf, sizeof buf);
        uint32_t r = lrm_vf_add_verifier_thunks(0, sizeof buf, SP, 1, 0, 0, 0, NULL, 0);
        g_checks++; int si = status_idx(c); if (si >= 0) g_cov_status[si]++;
        if (c != r) disu("add.notinit", c, r);
    }
    InitializeListHead(&MiVerifierDriverAddedThunkListHead);

    /* a module [0x100000, 0x110000) */
    static LDR_DATA_TABLE_ENTRY mod;
    mod.DllBase = (PVOID)0x100000; mod.SizeOfImage = 0x10000; mod.LoadedImports = NULL;
    uintptr_t base = 0x100000, end = 0x110000;

    struct Case { const char *name; size_t bufbytes; int lookup_ok; int imports_invalid; uintptr_t p0, p1; };
    Case cases[] = {
        { "count0",      0,       1, 0, base + 0x10, base + 0x20 },      /* buf < one pair */
        { "nomodule",    2 * SP,  0, 0, base + 0x10, base + 0x20 },      /* lookup NULL */
        { "imports-1",   2 * SP,  1, 1, base + 0x10, base + 0x20 },      /* LoadedImports == -1 */
        { "outofrange",  2 * SP,  1, 0, base + 0x10, end + 0x8 },        /* p1 past module end */
        { "ok",          2 * SP,  1, 0, base + 0x10, base + 0x20 },      /* all valid */
    };
    for (auto &cs : cases) {
        DRIVER_VERIFIER_THUNK_PAIRS buf[2]; memset(buf, 0, sizeof buf);
        buf[0].PristineRoutine = (PVOID)cs.p0; buf[1].PristineRoutine = (PVOID)cs.p1;
        g_lookup = cs.lookup_ok ? &mod : NULL;
        if (cs.lookup_ok) mod.LoadedImports = cs.imports_invalid ? (PVOID)(ULONG_PTR)-1 : NULL;
        uint32_t c = (uint32_t)MmAddVerifierThunks(buf, (ULONG)cs.bufbytes);

        uintptr_t pr[2] = { cs.p0, cs.p1 };
        uintptr_t mb = cs.lookup_ok ? base : 0, me = cs.lookup_ok ? end : 0;
        uint8_t mfound = cs.lookup_ok ? 1 : 0;
        if (neg) me = cs.lookup_ok ? (end + 0x1000) : 0;   /* widen range on Rust side -> diverge on outofrange */
        uint32_t r = lrm_vf_add_verifier_thunks(1, cs.bufbytes, SP, mfound, mb, me,
                                               (uint8_t)(cs.lookup_ok && cs.imports_invalid), pr, 2);
        g_checks++; int si = status_idx(c); if (si >= 0) g_cov_status[si]++;
        if (c != r) disu(cs.name, c, r);
    }
}

/* ---- MiInitializeDriverVerifier level computation (read back via MmVerifierData.Level) ---- */
static void test_init_level(int neg) {
    uint32_t levels[7] = { 0xFFFFFFFF, 0x1, 0x8, 0x9, 0x0, 0xF, 0x10 };
    for (int li = 0; li < 7; li++) for (int sp = 0; sp < 2; sp++) for (int empty = 0; empty < 2; empty++) {
        MmVerifyDriverLevel = levels[li];
        g_special_pool = (BOOLEAN)sp;
        MmVerifyDriverBuffer[0] = empty ? UNICODE_NULL : (WCHAR)'n';
        MmVerifyDriverBuffer[1] = UNICODE_NULL;
        const uint32_t SENT = 0xDEAD0000;
        MmVerifierData.Level = SENT;
        MiInitializeDriverVerifier();
        /* C: enabled iff Level changed from the sentinel. */
        int c_enabled = (MmVerifierData.Level != SENT);
        uint32_t c_level = c_enabled ? MmVerifierData.Level : 0;

        uint32_t rvl = neg ? (levels[li] ^ 0x8) : levels[li];
        uint32_t r_level = 0;
        uint8_t r_enabled = lrm_vf_init_level(rvl, (uint8_t)sp, (uint8_t)empty, &r_level);
        g_checks++;
        if ((int)r_enabled != c_enabled || (c_enabled && c_level != r_level))
            disu("init_level", ((uint64_t)c_enabled << 32) | c_level, ((uint64_t)r_enabled << 32) | r_level);
    }
}

/* ---- MmIsVerifierEnabled: inactive (before any init) then active (after) ---- */
static void test_verifier_enabled_inactive(void) {
    ULONG f = 0xABCD;
    uint32_t c = (uint32_t)MmIsVerifierEnabled(&f);
    uint32_t rf = 0; uint32_t r = lrm_vf_is_verifier_enabled(0, 0, &rf);
    g_checks++; if (status_idx(c) >= 0) g_cov_status[status_idx(c)]++;
    if (c != r || (uint32_t)f != rf) disu("verifier_enabled.inactive", ((uint64_t)c << 32) | f, ((uint64_t)r << 32) | rf);
}
static void test_verifier_enabled_active(void) {
    /* Make it active with a known level. */
    MmVerifyDriverLevel = 0xFFFFFFFF; g_special_pool = TRUE;
    MmVerifyDriverBuffer[0] = (WCHAR)'n'; MmVerifyDriverBuffer[1] = UNICODE_NULL;
    MmVerifierData.Level = 0;
    MiInitializeDriverVerifier();
    uint32_t known = MmVerifierData.Level;   /* should be 0x9 */
    ULONG f = 0;
    uint32_t c = (uint32_t)MmIsVerifierEnabled(&f);
    uint32_t rf = 0; uint32_t r = lrm_vf_is_verifier_enabled(1, known, &rf);
    g_checks++; if (status_idx(c) >= 0) g_cov_status[status_idx(c)]++;
    if (c != r || (uint32_t)f != rf) disu("verifier_enabled.active", ((uint64_t)c << 32) | f, ((uint64_t)r << 32) | rf);
}

/* ---- MiIsVerifiedDriverName: Rust unit check (C is static) ---- */
static int g_name_fail = 0, g_name_checks = 0;
static void chk_name(const char *lbl, const char *buf, const char *nm, int allow_wildcard, int expect) {
    uint16_t b[64], n[64]; size_t bl = 0, nl = 0;
    for (const char *p = buf; *p && bl < 63; p++) b[bl++] = (uint16_t)(unsigned char)*p;
    for (const char *p = nm; *p && nl < 63; p++) n[nl++] = (uint16_t)(unsigned char)*p;
    int got = lrm_vf_is_verified_driver_name(b, bl, n, nl, (uint8_t)allow_wildcard);
    g_name_checks++;
    if (got != expect) { g_name_fail++; std::fprintf(stderr, "NAME FAIL %s: buf='%s' nm='%s' got=%d want=%d\n", lbl, buf, nm, got, expect); }
}
static void test_name_unit(void) {
    chk_name("exact", "ntfs.sys", "ntfs.sys", 1, 1);
    chk_name("ci", "NTFS.SYS", "ntfs.sys", 1, 1);
    chk_name("list-mid", "a.sys ntfs.sys b.sys", "ntfs.sys", 1, 1);
    chk_name("sep-comma", "a.sys,ntfs.sys", "ntfs.sys", 1, 1);
    chk_name("sep-semi", "a.sys;ntfs.sys", "ntfs.sys", 1, 1);
    chk_name("wildcard-allowed", "*", "anything.sys", 1, 1);
    chk_name("wildcard-denied", "*", "anything.sys", 0, 0);           /* AllowWildcard=0 -> lone * does not match */
    chk_name("wildcard-denied-but-explicit", "* ntfs.sys", "ntfs.sys", 0, 1); /* explicit name still matches past a denied * */
    chk_name("no-match", "a.sys b.sys", "ntfs.sys", 1, 0);
    chk_name("empty", "", "ntfs.sys", 1, 0);
    chk_name("prefix-not-match", "ntfs.sys", "ntfs", 1, 0);
    chk_name("star-not-lone", "a*b", "ntfs.sys", 1, 0);
}

static void run(int neg) {
    test_is_driver_verifying(neg);
    test_add_thunks(neg);
    test_init_level(neg);
}

/* ---- Stage 1b boot walk: MiVerifyBootLoadedImages per-image decision vs lrm_vf_boot_image_should_verify.
 * The walk/iteration stays in C; we compare, per loaded image, whether C verified it (it sets
 * LDRP_IMAGE_VERIFYING on the entry) against the Rust decision. Covers: kernel excluded by base, HAL
 * excluded by name, a lone '*' matching only boot drivers, an explicit name matching a non-boot image. */
static long g_bw_verified = 0, g_bw_skipped = 0;

static void bw_setname(LDR_DATA_TABLE_ENTRY *e, uint16_t *buf, const char *nm) {
    size_t n = 0;
    for (const char *p = nm; *p && n < 63; p++) buf[n++] = (uint16_t)(unsigned char)*p;
    buf[n] = 0;
    e->BaseDllName.Buffer = (PWSTR)buf;
    e->BaseDllName.Length = (USHORT)(n * sizeof(uint16_t));
    e->BaseDllName.MaximumLength = (USHORT)((n + 1) * sizeof(uint16_t));
}

static void test_bootwalk(int neg) {
    /* Activate the verifier with a VerifyDrivers list of "* explicit.sys". */
    size_t vlen = 0;
    const char *vlist = "* explicit.sys";
    for (const char *p = vlist; *p; p++) MmVerifyDriverBuffer[vlen++] = (WCHAR)(unsigned char)*p;
    MmVerifyDriverBuffer[vlen] = UNICODE_NULL;
    MmVerifyDriverLevel = 0xFFFFFFFF; g_special_pool = TRUE; MmVerifierData.Level = 0;
    MiInitializeDriverVerifier();   /* -> MiVerifierActive = TRUE */

    static uint16_t nm_k[64], nm_h[64], nm_bd[64], nm_nb[64], nm_ex[64];
    LDR_DATA_TABLE_ENTRY kern{}, hal{}, bootdrv{}, nonboot{}, expl{};
    kern.DllBase = (PVOID)0x100000; PsNtosImageBase = (ULONG_PTR)kern.DllBase;
    hal.DllBase = (PVOID)0x200000; bootdrv.DllBase = (PVOID)0x300000;
    nonboot.DllBase = (PVOID)0x400000; expl.DllBase = (PVOID)0x500000;
    kern.SizeOfImage = hal.SizeOfImage = bootdrv.SizeOfImage = nonboot.SizeOfImage = expl.SizeOfImage = 0x1000;
    bw_setname(&kern, nm_k, "ntoskrnl.exe"); bw_setname(&hal, nm_h, "hal.dll");
    bw_setname(&bootdrv, nm_bd, "bootdrv.sys"); bw_setname(&nonboot, nm_nb, "nonboot.sys");
    bw_setname(&expl, nm_ex, "explicit.sys");

    InitializeListHead(&PsLoadedModuleList);
    InsertTailList(&PsLoadedModuleList, &kern.InLoadOrderLinks);
    InsertTailList(&PsLoadedModuleList, &hal.InLoadOrderLinks);
    InsertTailList(&PsLoadedModuleList, &bootdrv.InLoadOrderLinks);
    InsertTailList(&PsLoadedModuleList, &nonboot.InLoadOrderLinks);
    InsertTailList(&PsLoadedModuleList, &expl.InLoadOrderLinks);

    LOADER_PARAMETER_BLOCK lpb;
    InitializeListHead(&lpb.BootDriverListHead);
    BOOT_DRIVER_LIST_ENTRY bde{};            /* only bootdrv.sys is a boot driver */
    bde.LdrEntry = &bootdrv;
    InsertTailList(&lpb.BootDriverListHead, &bde.Link);

    MiVerifyBootLoadedImages(&lpb);

    struct Img { LDR_DATA_TABLE_ENTRY *e; int is_boot; const char *nm; };
    Img imgs[5] = { {&kern,0,"ntoskrnl.exe"}, {&hal,0,"hal.dll"}, {&bootdrv,1,"bootdrv.sys"},
                    {&nonboot,0,"nonboot.sys"}, {&expl,0,"explicit.sys"} };
    uint16_t vbuf[64]; size_t vb = 0;
    for (WCHAR *p = MmVerifyDriverBuffer; *p && vb < 63; p++) vbuf[vb++] = (uint16_t)*p;
    for (int i = 0; i < 5; i++) {
        int c_verified = (imgs[i].e->Flags & LDRP_IMAGE_VERIFYING) ? 1 : 0;
        uint16_t nb[64]; size_t nl = 0;
        for (const char *p = imgs[i].nm; *p; p++) nb[nl++] = (uint16_t)(unsigned char)*p;
        uint8_t ib = neg ? (uint8_t)(!imgs[i].is_boot) : (uint8_t)imgs[i].is_boot;
        int r = lrm_vf_boot_image_should_verify(1, (uintptr_t)imgs[i].e->DllBase, PsNtosImageBase,
                                                ib, vbuf, vb, nb, nl);
        g_checks++;
        if (!neg) { if (c_verified) g_bw_verified++; else g_bw_skipped++; }
        if (c_verified != r) { g_dis++; if (g_dis <= 25) std::fprintf(stderr, "BW DISAGREE %s c=%d r=%d\n", imgs[i].nm, c_verified, r); }
    }
}

int main(void) {
    test_verifier_enabled_inactive();   /* before any init: MiVerifierActive == FALSE */
    run(0);
    test_verifier_enabled_active();     /* after an enabling init */
    test_name_unit();
    test_bootwalk(0);

    std::printf("[vf-1b] checks=%ld disagreements=%ld (true=%ld false=%ld; status S=%ld NS=%ld IP1=%ld IP2=%ld) name=%ld/%ld bootwalk(verified=%ld skipped=%ld)\n",
                g_checks, g_dis, g_cov_true, g_cov_false,
                g_cov_status[0], g_cov_status[1], g_cov_status[2], g_cov_status[3],
                g_name_checks - g_name_fail, g_name_checks, g_bw_verified, g_bw_skipped);
    long dis0 = g_dis;
    bool nonvacuous = (g_cov_true > 0 && g_cov_false > 0 &&
                       g_cov_status[0] > 0 && g_cov_status[1] > 0 && g_cov_status[2] > 0 && g_cov_status[3] > 0 &&
                       g_name_fail == 0 && g_name_checks > 0 &&
                       g_bw_verified > 0 && g_bw_skipped > 0);

    g_dis = 0; long saved = g_checks; g_checks = 0;
    run(1);
    test_bootwalk(1);
    long negdis = g_dis; g_checks = saved;
    std::printf("[vf-1b] negative-control disagreements=%ld (expect > 0)\n", negdis);

    if (dis0 != 0) { std::fprintf(stderr, "FAIL: %ld disagreements\n", dis0); return 1; }
    if (!nonvacuous) { std::fprintf(stderr, "FAIL: vacuous coverage or name unit failed\n"); return 1; }
    if (negdis == 0) { std::fprintf(stderr, "FAIL: negative control did not bite\n"); return 1; }
    std::printf("RESULT=PASS\n");
    return 0;
}
