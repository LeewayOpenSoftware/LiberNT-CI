/* Local differential for the R1c mapping-pairs port: the Rust ntfs_alt_impl()->
 * decode_mapping_pairs vs model.c's ntfs_model_impl()->decode_mapping_pairs. For each input
 * both are called with a recording sink; two outcomes are equal when they have the same
 * status and, on success, the same run list. Exit 0 iff 0 disagreements AND both SUCCESS and
 * a corrupt/invalid status were produced (non-vacuous).
 *
 * NOTE: this is a local rust-vs-model check (two independent transcriptions of M1-M11). The
 * production run is rust == ntfslib(C++) == model inside the rig (ntfslib_decode_diff_test);
 * that runs on Linux via -DNTFSLIB_DIFF_ALT. Link: this + mp_model.c + the Rust staticlib.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>
extern "C" {
#include "decode_diff.h"
}

struct Run { uint64_t lcn, length; uint8_t sparse; };
static std::vector<Run> g_runs;
static int32_t record_sink(void *, uint64_t lcn, uint64_t length, uint8_t is_sparse) {
    g_runs.push_back(Run{lcn, length, is_sparse});
    return 0;
}

static long g_checks=0, g_dis=0, g_succ=0, g_corrupt=0, g_invalid=0, g_other=0;

static void tally(int32_t s){
    if (s==NTFS_DIFF_SUCCESS) g_succ++;
    else if (s==NTFS_DIFF_FILE_CORRUPT) g_corrupt++;
    else if (s==NTFS_DIFF_INVALID_PARAMETER) g_invalid++;
    else g_other++;
}

static void check(const uint8_t *a, uint32_t avail, uint64_t clusters){
    const ntfs_decode_impl *rust = ntfs_alt_impl();
    const ntfs_decode_impl *model = ntfs_model_impl();
    g_runs.clear();
    int32_t rs = rust->decode_mapping_pairs(a, avail, clusters, record_sink, nullptr);
    std::vector<Run> rr = g_runs;
    g_runs.clear();
    int32_t ms = model->decode_mapping_pairs(a, avail, clusters, record_sink, nullptr);
    std::vector<Run> mr = g_runs;
    g_checks++; tally(ms);
    bool eq = (rs == ms);
    if (eq && rs == NTFS_DIFF_SUCCESS) {
        eq = (rr.size() == mr.size());
        for (size_t i=0; eq && i<rr.size(); i++)
            eq = rr[i].lcn==mr[i].lcn && rr[i].length==mr[i].length && rr[i].sparse==mr[i].sparse;
    }
    if (!eq) {
        g_dis++;
        if (g_dis <= 20) {
            std::fprintf(stderr, "DISAGREE #%ld avail=%u clusters=%llu rust=0x%08x(%zu runs) model=0x%08x(%zu runs)\n",
                g_dis, avail, (unsigned long long)clusters, (unsigned)rs, rr.size(), (unsigned)ms, mr.size());
        }
    }
}

/* ---- builder ---------------------------------------------------------------- */
static unsigned min_bytes_unsigned_positive(uint64_t v){ /* top bit must stay clear */
    for (unsigned n=1;n<=8;n++){ uint64_t lim = (n==8)? (uint64_t)1<<63 : ((uint64_t)1<<(8*n-1)); if (v < lim) return n; } return 8;
}
static unsigned min_bytes_signed(int64_t v){
    for (unsigned n=1;n<=8;n++){ int64_t hi = (n==8)? INT64_MAX : (((int64_t)1<<(8*n-1))-1); int64_t lo = (n==8)? INT64_MIN : -((int64_t)1<<(8*n-1)); if (v>=lo && v<=hi) return n; } return 8;
}
static void put_le(std::vector<uint8_t>&b, size_t off, uint64_t v, unsigned n){ for(unsigned i=0;i<n;i++) b[off+i]=(uint8_t)(v>>(8*i)); }

/* xorshift64* deterministic */
static uint64_t st=0x123456789abcdefULL;
static uint64_t xr(){ uint64_t x=st; x^=x>>12; x^=x<<25; x^=x>>27; st=x; return x*0x2545F4914F6CDD1DULL; }

/* Build a valid nonresident attribute with a random run list over a volume of `clusters`. */
static std::vector<uint8_t> build_valid(uint64_t clusters, uint32_t extra_slack, bool compressed){
    uint32_t header = compressed ? 0x48 : 0x40;
    std::vector<uint8_t> rl; /* run list bytes */
    uint64_t cur = 0; uint64_t total = 0;
    int nruns = 1 + (int)(xr()%6);
    for (int i=0;i<nruns;i++){
        uint64_t length = 1 + (xr()% (clusters>8? 8 : (clusters>1?clusters-1:1)));
        bool sparse = (xr()&3)==0;
        unsigned ls = min_bytes_unsigned_positive(length);
        if (sparse){
            uint8_t head = (uint8_t)(0u<<4 | ls);
            rl.push_back(head);
            for(unsigned k=0;k<ls;k++) rl.push_back((uint8_t)(length>>(8*k)));
            total += length;
        } else {
            /* pick a target lcn in [0, clusters-length] */
            uint64_t room = clusters - length;
            uint64_t target = room? (xr()%(room+1)) : 0;
            int64_t delta = (int64_t)target - (int64_t)cur;
            unsigned os = min_bytes_signed(delta);
            uint8_t head = (uint8_t)((os<<4) | ls);
            rl.push_back(head);
            for(unsigned k=0;k<ls;k++) rl.push_back((uint8_t)(length>>(8*k)));
            uint64_t du = (uint64_t)delta;
            for(unsigned k=0;k<os;k++) rl.push_back((uint8_t)(du>>(8*k)));
            cur = target; total += length;
        }
    }
    rl.push_back(0x00); /* terminator */
    uint32_t length_field = header + (uint32_t)rl.size();
    uint32_t avail = length_field + extra_slack;
    if (avail < 0x40) avail = 0x40;
    std::vector<uint8_t> b(avail, 0);
    b[8] = 1; /* nonresident */
    put_le(b, 0x0C, compressed?0x8000:0x0000, 2); /* flags */
    put_le(b, 4, length_field, 4);
    put_le(b, 0x20, header, 2);
    put_le(b, 0x10, 0, 8);               /* FirstVCN */
    put_le(b, 0x18, total? total-1 : (uint64_t)-1, 8); /* LastVCN; empty-like if total==0 */
    for (size_t i=0;i<rl.size();i++) b[header+i]=rl[i];
    return b;
}

static void corpus(uint64_t clusters){
    /* empty list: FirstVCN=0 LastVCN=-1, offset at header, immediate terminator */
    { std::vector<uint8_t> b(0x48,0); b[8]=1; put_le(b,0x0C,0,2); put_le(b,4,0x41,4); put_le(b,0x20,0x40,2);
      put_le(b,0x10,0,8); put_le(b,0x18,(uint64_t)-1,8); b[0x40]=0x00; check(b.data(),0x48,clusters); }
    /* resident -> INVALID_PARAMETER */
    { auto b=build_valid(clusters,0,false); b[8]=0; check(b.data(),(uint32_t)b.size(),clusters); }
    /* avail < 0x40 -> CORRUPT (before resident flag) */
    { uint8_t z[16]={0}; check(z,16,clusters); }
    /* missing terminator: shrink Length so the list has no 0x00 */
    { auto b=build_valid(clusters,0,false); put_le(b,4,(uint32_t)b.size()-1,4); /* also keep avail */ check(b.data(),(uint32_t)b.size(),clusters); }
    /* ls = 9 is impossible (head low nibble max 0xF); craft head with ls=9 -> CORRUPT */
    { auto b=build_valid(clusters,0,false); uint32_t off=0x40; b[off]=0x09; check(b.data(),(uint32_t)b.size(),clusters); }
    /* run past the volume: a non-sparse run with length>clusters */
    { std::vector<uint8_t> b(0x60,0); b[8]=1; put_le(b,0x0C,0,2); put_le(b,0x20,0x40,2); put_le(b,0x10,0,8); put_le(b,0x18,0,8);
      uint32_t off=0x40; b[off]=0x11; b[off+1]=0xFF; b[off+2]=0x01; /* len huge, delta 1 */ b[off+3]=0x00; put_le(b,4,off+4,4); check(b.data(),(uint32_t)b.size(),clusters); }
    /* negative run length (top bit set) -> CORRUPT */
    { std::vector<uint8_t> b(0x60,0); b[8]=1; put_le(b,0x0C,0,2); put_le(b,0x20,0x40,2); put_le(b,0x10,0,8); put_le(b,0x18,0,8);
      uint32_t off=0x40; b[off]=0x11; b[off+1]=0x80; /* len=0x80 => negative in 1 byte */ b[off+2]=0x00; b[off+3]=0x00; put_le(b,4,off+4,4); check(b.data(),(uint32_t)b.size(),clusters); }
    /* sign-boundary run lengths: a structurally perfect attribute whose single sparse run
     * has a length with the sign bit set in its ls-byte encoding (0x80, 0x8000, ...). The
     * model rejects it (M5 negative length) and so must the port; this is what makes the
     * negative control on the sign check bite. VCN is set consistent with the raw length. */
    for (unsigned ls=1; ls<=8; ls++){
        uint64_t len = (uint64_t)1 << (8*ls-1); /* exactly the sign bit for ls bytes */
        std::vector<uint8_t> b(0x60 + ls, 0);
        b[8]=1; put_le(b,0x0C,0,2); put_le(b,0x20,0x40,2); put_le(b,0x10,0,8);
        put_le(b,0x18, len-1, 8);          /* LastVCN = total-1 if length were accepted */
        uint32_t off=0x40; b[off]=(uint8_t)(0u<<4 | ls); /* sparse run, ls bytes */
        for(unsigned k=0;k<ls;k++) b[off+1+k]=(uint8_t)(len>>(8*k));
        b[off+1+ls]=0x00;                  /* terminator */
        put_le(b,4, off+1+ls+1, 4);        /* Length */
        check(b.data(),(uint32_t)b.size(),clusters);
    }
    /* valid with trailing slack */
    { auto b=build_valid(clusters,32,false); check(b.data(),(uint32_t)b.size(),clusters); }
    /* valid compressed (header 0x48) */
    { auto b=build_valid(clusters,0,true); check(b.data(),(uint32_t)b.size(),clusters); }
}

static void fuzz(long iters, uint64_t clusters){
    std::vector<uint8_t> b;
    for (long i=0;i<iters;i++){
        uint64_t r = xr();
        if ((r&3)!=0){
            /* build valid, then perturb one byte/field so we straddle the accept boundary */
            auto v = build_valid(clusters, (uint32_t)(xr()%48), (xr()&1)!=0);
            switch (xr()%8){
                case 0: break;
                case 1: v[0x40 + (xr()% (v.size()-0x40))] ^= (uint8_t)(1u<<(xr()&7)); break; /* flip a runlist byte */
                case 2: put_le(v,4,(uint32_t)(xr()%(v.size()+8)),4); break; /* corrupt Length */
                case 3: put_le(v,0x20,(uint32_t)(xr()%0x60),2); break; /* corrupt offset */
                case 4: put_le(v,0x18,xr(),8); break; /* corrupt LastVCN */
                case 5: v[8] = (uint8_t)(xr()&1); break; /* maybe resident */
                case 6: v.resize(0x40 + (xr()% (v.size()-0x3F))); break; /* truncate */
                default: put_le(v,0x0C,xr()&0xFFFF,2); break; /* flags */
            }
            uint32_t avail = (uint32_t)v.size();
            check(v.data(), avail, clusters);
        } else {
            uint32_t avail = (r&0x3f)==0 ? (uint32_t)(xr()%8) : (uint32_t)(0x40 + xr()%200);
            b.assign(avail,0);
            for (uint32_t j=0;j<avail;j++) b[j]=(uint8_t)(xr()&0xff);
            check(avail? b.data() : (const uint8_t*)"", avail, clusters);
        }
    }
}

int main(int argc, char**argv){
    long iters = argc>1? std::atol(argv[1]) : 2000000;
    const uint64_t volumes[3] = {64, 4096, 1u<<20};
    for (uint64_t c : volumes){ corpus(c); fuzz(iters/3, c); }
    std::printf("[mapping-pairs] checks=%ld disagreements=%ld SUCCESS=%ld CORRUPT=%ld INVALID=%ld other=%ld\n",
        g_checks, g_dis, g_succ, g_corrupt, g_invalid, g_other);
    if (g_dis) { std::printf("RESULT=FAIL\n"); return 1; }
    if (!g_succ || !g_corrupt || !g_invalid) { std::printf("RESULT=FAIL (coverage gap)\n"); return 2; }
    std::printf("RESULT=PASS\n");
    return 0;
}
