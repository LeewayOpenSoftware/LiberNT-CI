/* D-1 host differential for the IO-3 name-grafting validator.
 *
 * Asserts rust(buf,len,tag) == c_ref(buf,len,tag) for:
 *   (A) a hand-written corpus of boundary cases (T-C1/T-C4 shape);
 *   (B) structured generated cases that sweep each field across its boundary;
 *   (C) a deterministic pseudo-random fuzz of ITERS inputs (default 3,000,000).
 * Exit 0 iff zero disagreements. Prints per-status coverage so a vacuous pass (e.g. all
 * inputs rejected the same way) is visible.
 *
 * Link: this .cpp + graft_ref.c + the Rust host staticlib (see run-differential.sh).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>

extern "C" long leeway_rs_poc_validate_reparse_graft(const void *buf, unsigned long len,
                                                     unsigned long reparse_tag);
extern "C" long graft_ref_validate(const unsigned char *buf, unsigned long len,
                                   unsigned long reparse_tag);

static const unsigned long MOUNT_POINT = 0xA0000003UL;
static const unsigned long SYMLINK = 0xA000000CUL;

static long g_success = 0, g_invalid = 0, g_data_inv = 0, g_tag_nh = 0, g_other = 0;
static long g_disagree = 0;
static long g_checks = 0;

static void tally(long s)
{
    switch (s)
    {
    case 0x00000000L: g_success++; break;
    case (long)0xC000000DL: g_invalid++; break;
    case (long)0xC0000278L: g_data_inv++; break;
    case (long)0xC0000279L: g_tag_nh++; break;
    default: g_other++; break;
    }
}

/* One differential check. Also handles the NULL case when buf==nullptr. */
static void check(const unsigned char *buf, unsigned long len, unsigned long tag)
{
    long r = leeway_rs_poc_validate_reparse_graft(buf, len, tag);
    long c = graft_ref_validate(buf, len, tag);
    g_checks++;
    tally(c);
    if (r != c)
    {
        g_disagree++;
        if (g_disagree <= 20)
            std::fprintf(stderr,
                         "DISAGREE #%ld: len=%lu tag=0x%lx rust=0x%08lx c=0x%08lx\n",
                         g_disagree, len, tag, (unsigned long)r, (unsigned long)c);
    }
}

/* Build a well-formed name-grafting buffer, then let the caller perturb it. */
static std::vector<unsigned char> make_buf(unsigned long tag, uint16_t so, uint16_t sl,
                                           uint16_t po, uint16_t pl, unsigned long pathlen)
{
    unsigned long fh = (tag == MOUNT_POINT) ? 8UL : 12UL;
    unsigned long rdl = fh + pathlen;
    unsigned long len = 8UL + rdl;
    std::vector<unsigned char> b(len, 0);
    b[0] = tag & 0xff; b[1] = (tag >> 8) & 0xff; b[2] = (tag >> 16) & 0xff; b[3] = (tag >> 24) & 0xff;
    b[4] = rdl & 0xff; b[5] = (rdl >> 8) & 0xff; /* Reserved @6,7 = 0 */
    b[8] = so & 0xff; b[9] = (so >> 8) & 0xff;
    b[10] = sl & 0xff; b[11] = (sl >> 8) & 0xff;
    b[12] = po & 0xff; b[13] = (po >> 8) & 0xff;
    b[14] = pl & 0xff; b[15] = (pl >> 8) & 0xff;
    return b;
}

static void corpus()
{
    /* (a) valid mount point: 8-byte path area, substitute at [0,4), print at [4,4). */
    { auto b = make_buf(MOUNT_POINT, 0, 4, 4, 4, 8); check(b.data(), (unsigned long)b.size(), MOUNT_POINT); }
    /* (b) valid symlink. */
    { auto b = make_buf(SYMLINK, 0, 6, 6, 2, 8); check(b.data(), (unsigned long)b.size(), SYMLINK); }
    /* (c) substitute offset beyond path area. */
    { auto b = make_buf(MOUNT_POINT, 10, 2, 0, 0, 8); check(b.data(), (unsigned long)b.size(), MOUNT_POINT); }
    /* (d) substitute offset+length just past end (off+len = pathlen+2). */
    { auto b = make_buf(MOUNT_POINT, 4, 6, 0, 0, 8); check(b.data(), (unsigned long)b.size(), MOUNT_POINT); }
    /* (e) u16 overflow: so=0xFFFE, sl=4. */
    { auto b = make_buf(MOUNT_POINT, 0xFFFE, 4, 0, 0, 8); check(b.data(), (unsigned long)b.size(), MOUNT_POINT); }
    /* (f) odd offset. */
    { auto b = make_buf(MOUNT_POINT, 1, 4, 0, 0, 8); check(b.data(), (unsigned long)b.size(), MOUNT_POINT); }
    /* (g) odd length. */
    { auto b = make_buf(MOUNT_POINT, 0, 3, 0, 0, 8); check(b.data(), (unsigned long)b.size(), MOUNT_POINT); }
    /* (h) zero substitute length (empty target) -> invalid. */
    { auto b = make_buf(MOUNT_POINT, 0, 0, 0, 0, 8); check(b.data(), (unsigned long)b.size(), MOUNT_POINT); }
    /* (i) print name OOB but substitute ok. */
    { auto b = make_buf(SYMLINK, 0, 4, 10, 2, 8); check(b.data(), (unsigned long)b.size(), SYMLINK); }
    /* (j) ReparseDataLength shorter than the field header (set rdl manually). */
    { auto b = make_buf(SYMLINK, 0, 4, 0, 0, 8); b[4] = 10; b[5] = 0; /* rdl=10 < 12, but len unchanged */ check(b.data(), (unsigned long)b.size(), SYMLINK); }
    /* (k) declared length mismatch: bump rdl by 2 without resizing. */
    { auto b = make_buf(MOUNT_POINT, 0, 4, 4, 4, 8); b[4] = (unsigned char)((8 + 8 + 2) & 0xff); check(b.data(), (unsigned long)b.size(), MOUNT_POINT); }
    /* (l) tag mismatch: buffer says MOUNT_POINT, caller asks SYMLINK. */
    { auto b = make_buf(MOUNT_POINT, 0, 4, 0, 0, 8); check(b.data(), (unsigned long)b.size(), SYMLINK); }
    /* (m) non-grafting tag -> TAG_NOT_HANDLED. */
    { auto b = make_buf(MOUNT_POINT, 0, 4, 0, 0, 8); check(b.data(), (unsigned long)b.size(), 0x80000017UL); }
    /* (n) too short (len<8). */
    { unsigned char z[4] = {0,0,0,0}; check(z, 4, MOUNT_POINT); }
    /* (o) NULL buffer. */
    { check(nullptr, 16, MOUNT_POINT); }
    { check(nullptr, 0, SYMLINK); }
    /* (p) oversize (len>16384). */
    { auto b = make_buf(MOUNT_POINT, 0, 4, 0, 0, 16400); check(b.data(), (unsigned long)b.size(), MOUNT_POINT); }
    /* (q) exactly max size, valid. */
    { auto b = make_buf(MOUNT_POINT, 0, 2, 0, 0, 16384 - 8 - 8); check(b.data(), (unsigned long)b.size(), MOUNT_POINT); }
}

static void structured()
{
    const unsigned long tags[2] = {MOUNT_POINT, SYMLINK};
    for (int t = 0; t < 2; t++)
    {
        unsigned long tag = tags[t];
        for (unsigned long pathlen = 0; pathlen <= 64; pathlen += 2)
            for (unsigned so = 0; so <= pathlen + 4; so += 1)
                for (unsigned sl = 0; sl <= pathlen + 4; sl += 1)
                {
                    auto b = make_buf(tag, (uint16_t)so, (uint16_t)sl, 0, 0, pathlen);
                    check(b.data(), (unsigned long)b.size(), tag);
                }
    }
}

/* Deterministic xorshift64* so runs are reproducible. */
static uint64_t g_state = 0x9E3779B97F4A7C15ULL;
static uint64_t xrand()
{
    uint64_t x = g_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    g_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static void fuzz(long iters)
{
    const unsigned long tagpool[4] = {MOUNT_POINT, SYMLINK, 0x00000000UL, 0xA000000CUL};
    std::vector<unsigned char> b;
    for (long i = 0; i < iters; i++)
    {
        uint64_t r = xrand();
        /* Mostly small buffers; occasionally near/over the maximum. */
        unsigned long len = (r & 0x3f) == 0 ? (unsigned long)(xrand() % 16500)
                                            : (unsigned long)(xrand() % 300);
        unsigned long tag;
        switch (r & 3)
        {
        case 0: tag = MOUNT_POINT; break;
        case 1: tag = SYMLINK; break;
        case 2: tag = (unsigned long)(xrand() & 0xFFFFFFFFUL); break;
        default: tag = tagpool[xrand() & 3]; break;
        }
        b.assign(len, 0);
        for (unsigned long j = 0; j < len; j++)
            b[j] = (unsigned char)(xrand() & 0xff);
        /* Half the time, seed a plausible header so deeper paths are reached often. */
        if ((r & 1) && len >= 16)
        {
            b[0] = tag & 0xff; b[1] = (tag >> 8) & 0xff; b[2] = (tag >> 16) & 0xff; b[3] = (tag >> 24) & 0xff;
            unsigned long rdl = len - 8;
            b[4] = rdl & 0xff; b[5] = (rdl >> 8) & 0xff;
        }
        check(len ? b.data() : (const unsigned char *)"", len, tag);
    }
}

int main(int argc, char **argv)
{
    long iters = (argc > 1) ? std::atol(argv[1]) : 3000000L;
    corpus();
    structured();
    fuzz(iters);
    std::printf("checks=%ld disagreements=%ld\n", g_checks, g_disagree);
    std::printf("coverage: SUCCESS=%ld INVALID_PARAMETER=%ld IO_REPARSE_DATA_INVALID=%ld "
                "TAG_NOT_HANDLED=%ld other=%ld\n",
                g_success, g_invalid, g_data_inv, g_tag_nh, g_other);
    if (g_disagree != 0)
    {
        std::printf("RESULT=FAIL\n");
        return 1;
    }
    /* Guard against a vacuous pass: require every status to have been produced. */
    if (g_success == 0 || g_data_inv == 0 || g_tag_nh == 0 || g_invalid == 0)
    {
        std::printf("RESULT=FAIL (coverage gap: a status class was never produced)\n");
        return 2;
    }
    std::printf("RESULT=PASS\n");
    return 0;
}
