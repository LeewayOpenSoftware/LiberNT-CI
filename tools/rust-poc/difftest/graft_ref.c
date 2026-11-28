/* Independent C reference oracle for the IO-3 mount-point validator.
 *
 * Deliberately plain, struct-free, byte-level, written independently of the Rust, encoding
 * the SAME contract as Stanley's shipped IopValidateMountPointBuffer at 1d45eab8da4 (the
 * overlap rule is DROPPED there: aliased substitute/print names are legal) and as
 * src/reparse_graft.rs / IO3-NAME-GRAFTING-INVARIANTS.md (V1-V7). Returns SUCCESS or
 * IO_REPARSE_DATA_INVALID only; a NULL buffer is a malformed buffer, not caller misuse.
 *
 * NOTE: this is the AUTHOR's own reference, so a 0-disagreement run here shows the Rust
 * agrees with a second transcription of the contract, NOT with the production C. The
 * production-equivalence run links the Rust staticlib into Stanley's host harness
 * (submodules/host-tests/iomgr, reparse.c + Oracle()); run via difftest/run-io3-harness.sh.
 */
#include <stddef.h>

#define GR_SUCCESS                 ((int)0x00000000)
#define GR_IO_REPARSE_DATA_INVALID ((int)0xC0000278)
#define GR_MOUNT_POINT 0xA0000003UL
#define GR_FIXED       8UL
#define GR_FIELDHDR    8UL
#define GR_MAX         (16UL * 1024UL)

static unsigned long gr_rd16(const unsigned char *b, unsigned long o)
{
    return (unsigned long)b[o] | ((unsigned long)b[o + 1] << 8);
}
static unsigned long gr_rd32(const unsigned char *b, unsigned long o)
{
    return (unsigned long)b[o] | ((unsigned long)b[o + 1] << 8) |
           ((unsigned long)b[o + 2] << 16) | ((unsigned long)b[o + 3] << 24);
}

/* The `ignored` parameter keeps a uniform (buf,len,x) harness signature; there is no tag
 * argument in the real contract (the tag is read from the buffer). */
int graft_ref_validate_mp(const unsigned char *buf, unsigned long len, unsigned long ignored)
{
    unsigned long tag, rdl, path, so, sl, po, pl, first;
    (void)ignored;
    if (buf == NULL) return GR_IO_REPARSE_DATA_INVALID;                 /* V1 */
    if (len < GR_FIXED + GR_FIELDHDR) return GR_IO_REPARSE_DATA_INVALID; /* V2: len>=16 */
    tag = gr_rd32(buf, 0);
    if (tag != GR_MOUNT_POINT) return GR_IO_REPARSE_DATA_INVALID;       /* V3 */
    rdl = gr_rd16(buf, 4);
    if (rdl < GR_FIELDHDR || rdl > GR_MAX - GR_FIXED || rdl > len - GR_FIXED)
        return GR_IO_REPARSE_DATA_INVALID;                             /* V4 */
    path = rdl - GR_FIELDHDR;
    so = gr_rd16(buf, 8); sl = gr_rd16(buf, 10); po = gr_rd16(buf, 12); pl = gr_rd16(buf, 14);
    if (((so | sl | po | pl) & 1UL) != 0UL) return GR_IO_REPARSE_DATA_INVALID; /* V5 even */
    if (so > path || sl > path - so) return GR_IO_REPARSE_DATA_INVALID;        /* V5 range */
    if (po > path || pl > path - po) return GR_IO_REPARSE_DATA_INVALID;        /* V5 range */
    if (sl < 2UL) return GR_IO_REPARSE_DATA_INVALID;                   /* V6 */
    /* NO overlap check: aliased names are legal at 1d45eab8da4. */
    first = gr_rd16(buf, 16UL + so);   /* PathBuffer at byte 16, + substitute offset */
    if (first != 0x005CUL) return GR_IO_REPARSE_DATA_INVALID;          /* V7: leading backslash */
    return GR_SUCCESS;
}
