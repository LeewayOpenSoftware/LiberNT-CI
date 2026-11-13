/* Independent C reference oracle for the IO-3 name-grafting validator.
 *
 * This is a deliberately plain, struct-free byte-level implementation of the SAME
 * invariants documented in src/reparse_graft.rs and kernel/rust/IO3-NAME-GRAFTING-
 * INVARIANTS.md. It is written independently of the Rust (different style, no shared
 * code) so it is a genuine oracle for the differential test, not a transliteration.
 *
 * Decision order matches the Rust FFI entry exactly so that inputs failing more than one
 * invariant still produce the same status in both:
 *   NULL -> INVALID_PARAMETER;  bad tag -> TAG_NOT_HANDLED;  then structural checks.
 *
 * Once Stanley's R1a C correction (IoIsValidNameGraftingBuffer) lands, this oracle is
 * replaced in the harness by his function and the differential re-run against it.
 */
#include <stddef.h>

#define GR_SUCCESS                 ((long)0x00000000)
#define GR_INVALID_PARAMETER       ((long)0xC000000D)
#define GR_IO_REPARSE_DATA_INVALID ((long)0xC0000278)
#define GR_TAG_NOT_HANDLED         ((long)0xC0000279)

#define GR_MOUNT_POINT 0xA0000003UL
#define GR_SYMLINK     0xA000000CUL
#define GR_FIXED       8UL
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

long graft_ref_validate(const unsigned char *buf, unsigned long len, unsigned long tag)
{
    unsigned long etag, rdl, fh, pathlen, so, sl, po, pl;

    if (buf == NULL)                                     /* I1  */
        return GR_INVALID_PARAMETER;
    if (tag != GR_MOUNT_POINT && tag != GR_SYMLINK)      /* I4  */
        return GR_TAG_NOT_HANDLED;
    if (len < GR_FIXED || len > GR_MAX)                  /* I2  */
        return GR_IO_REPARSE_DATA_INVALID;

    etag = gr_rd32(buf, 0);
    rdl = gr_rd16(buf, 4);                               /* Reserved @6 ignored */
    if (etag != tag)                                     /* I3  */
        return GR_IO_REPARSE_DATA_INVALID;
    if (GR_FIXED + rdl != len)                           /* I5  */
        return GR_IO_REPARSE_DATA_INVALID;

    fh = (tag == GR_MOUNT_POINT) ? 8UL : 12UL;
    if (rdl < fh)                                        /* I6  */
        return GR_IO_REPARSE_DATA_INVALID;
    pathlen = rdl - fh;

    so = gr_rd16(buf, 8);
    sl = gr_rd16(buf, 10);
    po = gr_rd16(buf, 12);
    pl = gr_rd16(buf, 14);

    if (((so | sl | po | pl) & 1UL) != 0UL)              /* I7  */
        return GR_IO_REPARSE_DATA_INVALID;
    if (so > pathlen || sl > pathlen - so)               /* I8  */
        return GR_IO_REPARSE_DATA_INVALID;
    if (po > pathlen || pl > pathlen - po)               /* I9  */
        return GR_IO_REPARSE_DATA_INVALID;
    if (sl == 0UL)                                       /* I10 */
        return GR_IO_REPARSE_DATA_INVALID;

    return GR_SUCCESS;
}

/* Independent C oracle for Stanley's IopValidateMountPointBuffer contract (R1a,
 * fix/io3-name-grafting ff2e252ff4d). MOUNT_POINT only; returns SUCCESS or
 * IO_REPARSE_DATA_INVALID only (NULL is a malformed buffer, not caller misuse).
 * The `tag` parameter is ignored (the tag is read from the buffer); kept for a
 * uniform harness signature. Written independently of the Rust mirror. */
long graft_ref_validate_mp(const unsigned char *buf, unsigned long len, unsigned long ignored)
{
    unsigned long tag, rdl, path, so, sl, po, pl, first;
    (void)ignored;
    if (buf == NULL) return GR_IO_REPARSE_DATA_INVALID;
    if (len < 16UL) return GR_IO_REPARSE_DATA_INVALID;
    tag = gr_rd32(buf, 0);
    if (tag != GR_MOUNT_POINT) return GR_IO_REPARSE_DATA_INVALID;
    rdl = gr_rd16(buf, 4);
    if (rdl < 8UL) return GR_IO_REPARSE_DATA_INVALID;
    if (rdl > GR_MAX - 8UL) return GR_IO_REPARSE_DATA_INVALID;
    if (rdl > len - 8UL) return GR_IO_REPARSE_DATA_INVALID;
    path = rdl - 8UL;
    so = gr_rd16(buf, 8); sl = gr_rd16(buf, 10); po = gr_rd16(buf, 12); pl = gr_rd16(buf, 14);
    if (((so | sl | po | pl) & 1UL) != 0UL) return GR_IO_REPARSE_DATA_INVALID;
    if (so > path || sl > path - so) return GR_IO_REPARSE_DATA_INVALID;
    if (po > path || pl > path - po) return GR_IO_REPARSE_DATA_INVALID;
    if (sl < 2UL) return GR_IO_REPARSE_DATA_INVALID;
    if (pl != 0UL && so < po + pl && po < so + sl) return GR_IO_REPARSE_DATA_INVALID;
    first = gr_rd16(buf, 16UL + so);          /* PathBuffer at offset 16, + substitute offset */
    if (first != 0x005CUL) return GR_IO_REPARSE_DATA_INVALID; /* L'\' */
    return GR_SUCCESS;
}
