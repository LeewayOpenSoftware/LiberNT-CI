/* C ABI for the Leeway experimental Rust PoC staticlib (leeway_rs_poc).
 * Hand-maintained to match src/lib.rs (cbindgen can regenerate it; the checked-in
 * copy is authoritative). no_std, no allocation, no locking: the exported routine is
 * safe to call at IRQL <= DISPATCH_LEVEL. Part of an off-by-default experiment; the
 * backing driver (lnrspoc.sys) is never placed on installation media. */
#ifndef LEEWAY_RS_POC_H
#define LEEWAY_RS_POC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Validate a length-prefixed record and compute the CRC-32 (IEEE 802.3) of its
 * payload. Record layout: [length: 4 bytes little-endian][payload bytes...].
 *   buf       readable for len_bytes bytes; caller-owned, not retained.
 *   len_bytes size of buf, in bytes.
 *   out_crc   receives the CRC-32 on success; left untouched on failure.
 * Returns 0 on success, or a negative code (see below). Never allocates, never
 * locks, and never bugchecks on any input value: expected failures are returned. */
int leeway_rs_poc_validate(const unsigned char *buf,
                           unsigned long len_bytes,
                           unsigned long *out_crc);

#define LNRSPOC_E_NULL     (-1) /* buf or out_crc was NULL                     */
#define LNRSPOC_E_SHORT    (-2) /* len_bytes < 4: no room for the length prefix */
#define LNRSPOC_E_LENFIELD (-3) /* declared length exceeds available payload    */

/* ---- v2: pool allocator + SpinLock exercisers (always present; never bugcheck) ----
 * Each returns 0 on success or a distinct negative code (see src/lib.rs). They allocate
 * from the nonpaged pool via the Rust #[global_allocator] and lock over a KSPIN_LOCK, so
 * they import the ntoskrnl pool and spin-lock routines, but none of them can panic or
 * bugcheck. */

/* Load-time self-test: one pooled KBox + one SpinLock critical section. Returns 0 ok. */
int leeway_rs_poc_v2_selftest(void);

/* Allocation stress: `iters` rounds of alloc/write/read-back/free. Returns 0 ok. */
int leeway_rs_poc_alloc_stress(unsigned long iters);

/* Lock stress: `iters` acquire/modify/release cycles, then verify the count. 0 ok. */
int leeway_rs_poc_lock_stress(unsigned long iters);

/* OOM negative test: deliberately exhaust the nonpaged pool and prove failure is a
 * returned error, not a crash. Returns the block count taken before the pool said no
 * (>0), or a negative code. Reaching the caller at all is the proof. All blocks freed. */
int leeway_rs_poc_oom_probe(void);

/* ---- R1b: IO-3 name-grafting reparse validator (always present; never bugcheck) ----
 * Validate an untrusted REPARSE_DATA_BUFFER used for name grafting, exactly as the IO
 * manager's IoIsValidNameGraftingBuffer must before IopDoNameTransmogrify touches the
 * substitute/print name ranges. Pure logic over a borrowed buffer: no allocation, no
 * locking, no bugcheck on any input; safe at IRQL <= DISPATCH_LEVEL.
 *
 * Buffer layout (REPARSE_DATA_BUFFER, name-grafting variant):
 *   [0] ReparseTag (u32 LE) [4] ReparseDataLength (u16 LE) [6] Reserved (u16)
 *   [8] per-tag field header (MOUNT_POINT 8 bytes / SYMLINK 12 bytes) + PathBuffer[]
 * Name offsets/lengths are byte values relative to PathBuffer.
 *
 *   buf          readable for `len` bytes; caller-owned, not retained; may be NULL.
 *   len          size of buf, in bytes.
 *   reparse_tag  the tag the caller expects (must be MOUNT_POINT or SYMLINK and must
 *                equal the tag embedded in the buffer).
 * Returns STATUS_SUCCESS (0) iff every invariant holds (see src/reparse_graft.rs and
 * tools/rust-poc/difftest/IO3-NAME-GRAFTING-INVARIANTS.md), else one of the NTSTATUS codes below. */
long leeway_rs_poc_validate_reparse_graft(const void *buf,
                                          unsigned long len,
                                          unsigned long reparse_tag);

#define LNRSPOC_IO_REPARSE_TAG_MOUNT_POINT     0xA0000003UL
#define LNRSPOC_IO_REPARSE_TAG_SYMLINK         0xA000000CUL
#define LNRSPOC_STATUS_SUCCESS                  0x00000000L
#define LNRSPOC_STATUS_INVALID_PARAMETER       ((long)0xC000000D) /* buf NULL / misuse  */
#define LNRSPOC_STATUS_IO_REPARSE_DATA_INVALID ((long)0xC0000278) /* malformed buffer   */
#define LNRSPOC_STATUS_IO_REPARSE_TAG_NOT_HANDLED ((long)0xC0000279) /* non-graft tag   */

/* R1a mirror: exact equivalent of the shipped C IopValidateMountPointBuffer(buf, len)
 * (fix/io3-name-grafting ff2e252ff4d). This is the swap-equivalent validated by the
 * differential. MOUNT_POINT only; returns STATUS_SUCCESS or STATUS_IO_REPARSE_DATA_INVALID
 * only (a NULL buffer is treated as malformed, not caller misuse). SUCCESS iff: len>=16;
 * tag==MOUNT_POINT; 8<=ReparseDataLength<=16384-8 and <=len-8; with path=ReparseDataLength-8
 * the substitute/print offsets and lengths are all even and off+len<=path (no wrap);
 * substitute length>=2; when print length!=0 the ranges do not overlap; and the first
 * WCHAR of the substitute name is L'\\'. No alloc/lock; safe at IRQL <= DISPATCH_LEVEL. */
long leeway_rs_poc_validate_mount_point(const void *buf, unsigned long len);

/* TEST-ONLY. Present only when the staticlib is built with `--features test-ioctls`
 * (and the driver with -DLNRSPOC_TEST_IOCTLS). Triggers the controlled bugcheck
 * (KeBugCheckEx, code LEEWAY_RUST_PANIC 0x4C450001). Reachable solely via the
 * documented test IOCTL inside a disposable VM. Does not return. Never ship. */
void leeway_rs_poc_force_panic(void);

#define LEEWAY_RUST_PANIC 0x4C450001UL /* Leeway provisional experimental bugcheck code (not public) */

#ifdef __cplusplus
}
#endif

#endif /* LEEWAY_RS_POC_H */
