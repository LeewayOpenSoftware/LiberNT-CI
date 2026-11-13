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

/* ---- R1b: IO-3 mount-point reparse validator (always present; never bugcheck) ----
 * Mirror of the shipped C IopValidateMountPointBuffer (fix/io3-name-grafting 1d45eab8da4):
 * validate an untrusted REPARSE_DATA_BUFFER used for a mount point, before the IO manager
 * touches the substitute/print name ranges. Pure logic over a borrowed buffer: no alloc, no
 * lock, no bugcheck on any input; safe at IRQL <= DISPATCH_LEVEL for a captured kernel copy.
 *
 *   buf  the REPARSE_DATA_BUFFER bytes; caller-owned kernel copy, not retained; may be NULL.
 *   len  size of buf in bytes (an UPPER BOUND; the declared ReparseDataLength may be smaller).
 * Returns STATUS_SUCCESS (0) iff every invariant holds (see src/reparse_graft.rs and
 * tools/rust-poc/difftest/IO3-NAME-GRAFTING-INVARIANTS.md V1-V7), else
 * STATUS_IO_REPARSE_DATA_INVALID. MOUNT_POINT only; a NULL buffer or any other tag is
 * STATUS_IO_REPARSE_DATA_INVALID (a single failure code). `len` is 32-bit to match ULONG. */
/* Returns NTSTATUS as a fixed 32-bit int (matches the Rust i32 / kernel LONG). */
int leeway_rs_poc_validate_mount_point(const void *buf, unsigned int len);

#define LNRSPOC_IO_REPARSE_TAG_MOUNT_POINT     0xA0000003UL
#define LNRSPOC_STATUS_SUCCESS                  0x00000000L
#define LNRSPOC_STATUS_IO_REPARSE_DATA_INVALID ((long)0xC0000278)

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
