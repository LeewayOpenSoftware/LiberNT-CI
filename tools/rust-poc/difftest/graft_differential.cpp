/* D-1 host differential for the IO-3 mount-point validator (the swap-equivalent of
 * Stanley's IopValidateMountPointBuffer at 1d45eab8da4).
 *
 * Asserts rust(buf,len) == c_ref(buf,len) for a hand corpus + a boundary-straddling fuzz
 * (build-valid-then-perturb). Exit 0 iff 0 disagreements AND both SUCCESS and the invalid
 * status were produced (guards against a vacuous pass). The C reference here is the author's
 * own (graft_ref.c); production equivalence is the separate run against Stanley's reparse.c
 * in submodules/host-tests/iomgr.
 *
 * Link: this .cpp + graft_ref.c + the Rust host staticlib (see run-differential.sh).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>

/* Match the kernel ABI exactly: len is 32-bit (ULONG), status is 32-bit (LONG). */
extern "C" int leeway_rs_poc_validate_mount_point(const void *buf, uint32_t len);
extern "C" int graft_ref_validate_mp(const unsigned char *buf, unsigned long len,
                                     unsigned long ignored);

static const unsigned long MOUNT_POINT = 0xA0000003UL;

static long g_checks = 0, g_disagree = 0, g_success = 0, g_invalid = 0;

static void check(const unsigned char *buf, uint32_t len)
{
    int r = leeway_rs_poc_validate_mount_point(buf, len);
    int c = graft_ref_validate_mp(buf, (unsigned long)len, 0);
    g_checks++;
    if (c == 0) g_success++; else g_invalid++;
    if (r != c)
    {
        g_disagree++;
        if (g_disagree <= 20)
            std::fprintf(stderr, "DISAGREE #%ld: len=%u rust=0x%08lx c=0x%08lx\n",
                         g_disagree, len, (unsigned)r, (unsigned)c);
    }
}

/* Build a MOUNT_POINT buffer; optionally place a backslash as the substitute's first WCHAR,
 * and optionally add trailing slack past ReparseDataLength. tag lets us test wrong tags. */
static std::vector<unsigned char> make_mp(unsigned long tag, uint16_t so, uint16_t sl,
                                          uint16_t po, uint16_t pl, unsigned long pathlen,
                                          bool backslash, unsigned long slack)
{
    unsigned long rdl = 8UL + pathlen;
    unsigned long len = 8UL + rdl + slack;
    std::vector<unsigned char> b(len, 0);
    b[0]=tag&0xff; b[1]=(tag>>8)&0xff; b[2]=(tag>>16)&0xff; b[3]=(tag>>24)&0xff;
    b[4]=rdl&0xff; b[5]=(rdl>>8)&0xff;
    b[8]=so&0xff; b[9]=(so>>8)&0xff;
    b[10]=sl&0xff; b[11]=(sl>>8)&0xff;
    b[12]=po&0xff; b[13]=(po>>8)&0xff;
    b[14]=pl&0xff; b[15]=(pl>>8)&0xff;
    if (backslash && sl >= 2 && (unsigned long)(16 + so + 1) < len)
    { b[16+so] = 0x5C; b[16+so+1] = 0x00; }
    return b;
}

static void corpus()
{
    /* valid: substitute at [0,4) starting with a backslash, no print. */
    { auto b = make_mp(MOUNT_POINT,0,4,0,0,8,true,0); check(b.data(),(uint32_t)b.size()); }
    /* valid with non-overlapping print. */
    { auto b = make_mp(MOUNT_POINT,0,4,4,4,8,true,0); check(b.data(),(uint32_t)b.size()); }
    /* valid with trailing slack (rdl < len-8). */
    { auto b = make_mp(MOUNT_POINT,0,4,0,0,8,true,16); check(b.data(),(uint32_t)b.size()); }
    /* valid longer name. */
    { auto b = make_mp(MOUNT_POINT,0,8,0,0,16,true,0); check(b.data(),(uint32_t)b.size()); }
    /* valid ALIASED (print overlaps substitute) - legal since the overlap rule was dropped. */
    { auto b = make_mp(MOUNT_POINT,0,4,0,4,8,true,0); check(b.data(),(uint32_t)b.size()); }
    { auto b = make_mp(MOUNT_POINT,0,4,2,4,8,true,0); check(b.data(),(uint32_t)b.size()); }
    /* missing backslash -> invalid. */
    { auto b = make_mp(MOUNT_POINT,0,4,0,0,8,false,0); check(b.data(),(uint32_t)b.size()); }
    /* empty substitute -> invalid. */
    { auto b = make_mp(MOUNT_POINT,0,0,0,0,8,false,0); check(b.data(),(uint32_t)b.size()); }
    /* odd offset / odd length -> invalid. */
    { auto b = make_mp(MOUNT_POINT,1,4,0,0,8,true,0); check(b.data(),(uint32_t)b.size()); }
    { auto b = make_mp(MOUNT_POINT,0,3,0,0,8,true,0); check(b.data(),(uint32_t)b.size()); }
    /* substitute OOB -> invalid. */
    { auto b = make_mp(MOUNT_POINT,0,20,0,0,8,true,0); check(b.data(),(uint32_t)b.size()); }
    /* print OOB -> invalid. */
    { auto b = make_mp(MOUNT_POINT,0,4,10,2,8,true,0); check(b.data(),(uint32_t)b.size()); }
    /* wrong tag (symlink) -> invalid. */
    { auto b = make_mp(0xA000000CUL,0,4,0,0,8,true,0); check(b.data(),(uint32_t)b.size()); }
    /* rdl < field header (set manually). */
    { auto b = make_mp(MOUNT_POINT,0,4,0,0,8,true,0); b[4]=4; b[5]=0; check(b.data(),(uint32_t)b.size()); }
    /* rdl > len-8 (set manually). */
    { auto b = make_mp(MOUNT_POINT,0,4,0,0,8,true,0); b[4]=0x00; b[5]=0x10; check(b.data(),(uint32_t)b.size()); }
    /* len < 16. */
    { unsigned char z[12]={0}; z[0]=0x03; z[3]=0xA0; check(z,12); }
    /* NULL. */
    { check(nullptr,16); }
    /* exactly max size, valid. */
    { auto b = make_mp(MOUNT_POINT,0,2,0,0,16384-8-8,true,0); check(b.data(),(uint32_t)b.size()); }
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
    std::vector<unsigned char> b;
    for (long i = 0; i < iters; i++)
    {
        uint64_t r = xrand();
        if ((r & 3) != 0)
        {
            /* Boundary-straddling: build a WELL-FORMED MP buffer, then perturb one field. */
            unsigned long pathlen = ((unsigned long)(xrand() % 32)) * 2 + 2;
            unsigned long so = ((unsigned long)(xrand() % (pathlen / 2))) * 2;
            unsigned long maxsl = pathlen - so;
            unsigned long sl = maxsl >= 2 ? (((unsigned long)(xrand() % (maxsl / 2)) + 1) * 2) : 2;
            if (sl > maxsl) sl = maxsl;
            unsigned long po = 0, pl = 0;
            if (xrand() & 1) { po = (xrand() % (pathlen/2)) * 2; unsigned long room = pathlen - po;
                               pl = room >= 2 ? ((xrand() % (room/2) + 1) * 2) : 0; if (pl > room) pl = room; }
            unsigned long slack = (xrand() % 3) * 2;
            auto buf = make_mp(MOUNT_POINT,(uint16_t)so,(uint16_t)sl,(uint16_t)po,(uint16_t)pl,
                               pathlen, true, slack);
            switch (xrand() % 7)
            {
            case 0: break;                              /* leave valid */
            case 1: buf[16+so]=0x2F; buf[16+so+1]=0x00; break; /* '/' not backslash */
            case 2: buf[10]=(unsigned char)(sl+1); break;      /* odd substitute length */
            case 3: buf[10]=(unsigned char)((maxsl+2)&0xff); break; /* OOB */
            case 4: buf[8]=1; break;                    /* odd offset */
            case 5: { unsigned long rdl=8+pathlen; buf[4]=(unsigned char)((rdl-2)&0xff); } break;
            default: buf[0]=0x0C; break;                /* corrupt tag low byte */
            }
            check(buf.data(),(uint32_t)buf.size());
        }
        else
        {
            unsigned long len = (r & 0x3f) == 0 ? (unsigned long)(xrand() % 16500)
                                                : (unsigned long)(xrand() % 300);
            b.assign(len, 0);
            for (unsigned long j = 0; j < len; j++) b[j] = (unsigned char)(xrand() & 0xff);
            check(len ? b.data() : (const unsigned char *)"", (uint32_t)len);
        }
    }
}

int main(int argc, char **argv)
{
    long iters = (argc > 1) ? std::atol(argv[1]) : 3000000L;
    corpus();
    fuzz(iters);
    std::printf("[mount-point] checks=%ld disagreements=%ld SUCCESS=%ld INVALID=%ld\n",
                g_checks, g_disagree, g_success, g_invalid);
    if (g_disagree != 0) { std::printf("RESULT=FAIL\n"); return 1; }
    if (g_success == 0 || g_invalid == 0)
    { std::printf("RESULT=FAIL (coverage gap)\n"); return 2; }
    std::printf("RESULT=PASS\n");
    return 0;
}
