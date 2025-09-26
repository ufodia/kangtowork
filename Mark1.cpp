/***************************************************************************************************
 * Pollard–Kangaroo  (wrap-aware, user-configurable k, live counter, loop detector, restart counter)
 * Coded by DooKoo2
 * Load/Save DP tech by NoMachine
 * SSD rework (strong DP on SSD instead of RAM. Bloom – RAM, DP_table – SSD)
 * Added a few improvements with runtime security
 * Patched for higher performance (dual-hash DP table, better Bloom/DP sizing, batched I/O)
 *
 *  g++ Mark1.cpp Int.cpp SECP256K1.cpp Point.cpp Random.cpp IntMod.cpp IntGroup.cpp Timer.cpp \
 *      -O3 -march=native -funroll-loops -ftree-vectorize -fstrict-aliasing -fno-semantic-interposition \
 *      -fvect-cost-model=unlimited -fno-trapping-math -fipa-ra -fipa-modref -flto -fassociative-math \
 *      -fopenmp -mavx2 -mbmi2 -madx -std=c++17 -pthread -o Mark1
 *
 ***************************************************************************************************/
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#include <omp.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "Int.h"
#include "Point.h"
#include "SECP256K1.h"
#include "IntGroup.h"
#include "simd_block_bloom.h"

// ─── Misc ─────────────────────────────────────────────────────────────────────
static inline uint64_t splitmix64(uint64_t x){
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x>>30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x>>27)) * 0x94D049BB133111EBULL;
    return x ^ (x>>31);
}
static std::string humanBytes(size_t bytes){
    constexpr const char* u[]{"B","Kb","Mb","Gb","Tb"};
    double v = double(bytes); int s = 0;
    while(v >= 1024.0 && s < 4){ v /= 1024.0; ++s; }
    std::ostringstream o;
    o << std::fixed << std::setprecision((v<10)?2:(v<100)?1:0) << v << u[s];
    return o.str();
}
static inline void intCopy(Int& d,const Int& s){ d.Set(&const_cast<Int&>(s)); }
static inline uint64_t IntLow64(const Int& n){ return n.bits64[0]; }
static inline int bitlen(const Int& n){ return const_cast<Int&>(n).GetBitLength(); }
static inline bool intGE(const Int& a,const Int& b){
    return const_cast<Int&>(a).IsGreaterOrEqual(&const_cast<Int&>(b));
}

// ─── Scalar256 ────────────────────────────────────────────────────────────────
struct Scalar256{ uint64_t w[4]; };
static inline void intToScalar(const Int& n, Scalar256& s){
    s.w[0]=n.bits64[0]; s.w[1]=n.bits64[1];
    s.w[2]=n.bits64[2]; s.w[3]=n.bits64[3];
}
static inline void scalarToInt(const Scalar256& s, Int& n){
    n.SetInt32(0);
    for(int i=3;i>=0;--i){ n.ShiftL(64); Int tmp(s.w[i]); n.Add(&tmp); }
}

using fp_t = uint64_t;

// ─── SSD DP storage ────────────────────────────────────────────────────────────
#pragma pack(push,1)
struct DPSlot{ fp_t fp; Scalar256 key; };
#pragma pack(pop)
static_assert(sizeof(DPSlot)==40);

struct DPStorage{
    size_t                        cap=0, mapBytes=0;
    int                           fd=-1;
    DPSlot*                       slots=nullptr;
    std::unique_ptr<std::atomic<uint8_t>[]> st_used, st_lock;
    std::atomic<size_t>           size{0};           
    std::atomic<size_t>           dirty{0};
    std::atomic<size_t>           flush_counter{0};   
    std::atomic<bool>             enable_flush{true};

    void   init(const std::string& path,size_t c);
    void   flushIfNeeded(size_t slotIdx) noexcept;
    void   fullSync() noexcept;
    void   close();
};
static DPStorage dp;

static constexpr size_t FLUSH_STEP = 1ull<<24;   

// ─── DPStorage impl ───────────────────────────────────────────────────────────
void DPStorage::init(const std::string& path,size_t c){
    cap=c; mapBytes=cap*sizeof(DPSlot);

    int flags = O_RDWR | O_CREAT;
#ifdef O_DIRECT
    flags |= O_DIRECT;         
#endif
#ifdef O_SYNC
    flags |= O_SYNC;           
#endif
    fd = ::open(path.c_str(),flags,0644);
    if(fd<0){
        perror("open(dp)"); throw std::runtime_error("open(dp)");
    }
    if(posix_fallocate(fd,0,mapBytes)){
        perror("fallocate(dp)"); throw std::runtime_error("fallocate(dp)");
    }
    void* p = mmap(nullptr,mapBytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(p==MAP_FAILED){
        perror("mmap(dp)"); throw std::runtime_error("mmap(dp)");
    }
    slots = static_cast<DPSlot*>(p);

    st_used = std::make_unique<std::atomic<uint8_t>[]>(cap);
    st_lock = std::make_unique<std::atomic<uint8_t>[]>(cap);
#pragma omp parallel for schedule(static)
    for(size_t i=0;i<cap;++i){
        st_used[i].store(0,std::memory_order_relaxed);
        st_lock[i].store(0,std::memory_order_relaxed);
    }
    madvise(slots,mapBytes,MADV_RANDOM);
}

void DPStorage::flushIfNeeded(size_t slotIdx) noexcept{
    if(!enable_flush.load(std::memory_order_relaxed)) return;
    if(flush_counter.fetch_add(1,std::memory_order_relaxed) % FLUSH_STEP == 0){
        size_t start = (slotIdx / FLUSH_STEP) * FLUSH_STEP;
        size_t end   = std::min(start + FLUSH_STEP, cap);
        size_t len   = (end - start) * sizeof(DPSlot);
        msync(reinterpret_cast<char*>(slots) + start*sizeof(DPSlot), len, MS_ASYNC);
    }
}
void DPStorage::fullSync() noexcept{
    msync(slots,mapBytes,MS_SYNC);
}
void DPStorage::close(){
    if(slots) munmap(slots,mapBytes);
    if(fd>=0) ::close(fd);
}

// ─── Curve ────────────────────────────────────────────────────────────────────
static const char *P_HEX="FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F";
static const char *N_HEX="FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141";
static Int P_PRIME, ORDER_N; static Secp256K1 secp;

static inline Point addP(const Point& a,const Point& b){
    if(const_cast<Int&>(a.x).IsZero() && const_cast<Int&>(a.y).IsZero()) return b;
    if(const_cast<Int&>(b.x).IsZero() && const_cast<Int&>(b.y).IsZero()) return a;
    return secp.AddDirect(const_cast<Point&>(a),const_cast<Point&>(b));
}
static inline Point mulP(const Int& k){ Int t(k); return secp.ComputePublicKey(&t); }

// ─── Bloom + DP API ───────────────────────────────────────────────────────────
static simd_bloom::SimdBlockFilterFixed<>* bloom=nullptr;
static std::atomic<uint64_t> dpDone{0};

inline bool sameScalar(const Scalar256& a,const Scalar256& b){
    return std::memcmp(&a,&b,sizeof(Scalar256))==0;
}

// ─── Dual-hash DP ────────────────────────────────────────────────────────────
bool dp_insert_unique(fp_t fp,const Int& idx){
    Int t(idx); t.Mod(&ORDER_N);
    Scalar256 key; intToScalar(t,key);

    size_t mask = dp.cap - 1;
    size_t h1   = fp & mask;
    size_t h2   = ((fp << 1) | 1) & mask;
    if(h2 == 0) h2 = 1;

    size_t h = h1;
    // Prefetch first few probe positions
    __builtin_prefetch(&dp.st_used[h], 0, 1);
    __builtin_prefetch(&dp.slots[h], 0, 1);

    for(size_t i=0;i<dp.cap;++i){
        if(!dp.st_used[h].load(std::memory_order_acquire)){
            uint8_t exp=0;
            if(dp.st_lock[h].compare_exchange_strong(exp,1,std::memory_order_acq_rel)){
                if(!dp.st_used[h].load(std::memory_order_acquire)){
                    dp.slots[h].fp  = fp;
                    dp.slots[h].key = key;
                    dp.st_used[h].store(1,std::memory_order_release);
                    dp.st_lock[h].store(0,std::memory_order_release);

                    dp.size.fetch_add(1,std::memory_order_relaxed);
                    dp.flushIfNeeded(h);
                    dpDone.fetch_add(1,std::memory_order_relaxed);
                    return true;
                }
                dp.st_lock[h].store(0,std::memory_order_release);
            }
        }else if(dp.slots[h].fp==fp && sameScalar(dp.slots[h].key,key)){
            return false;
        }
        h = (h + h2) & mask;
        // Prefetch next position if not too many iterations
        if(i < 8) {
            size_t next_h = (h + h2) & mask;
            __builtin_prefetch(&dp.st_used[next_h], 0, 1);
        }
    }
    return false;
}
bool dp_find(fp_t fp,Int& out){
    size_t mask = dp.cap - 1;
    size_t h1   = fp & mask;
    size_t h2   = ((fp << 1) | 1) & mask;
    if(h2 == 0) h2 = 1;

    size_t h = h1;
    // Prefetch first probe position
    __builtin_prefetch(&dp.st_used[h], 0, 1);
    __builtin_prefetch(&dp.slots[h], 0, 1);

    for(size_t i=0;i<dp.cap;++i){
        if(!dp.st_used[h].load(std::memory_order_acquire))
            return false;
        if(dp.slots[h].fp == fp){
            scalarToInt(dp.slots[h].key,out);
            return true;
        }
        h = (h + h2) & mask;
        // Prefetch next position for better performance
        if(i < 4) {
            size_t next_h = (h + h2) & mask;
            __builtin_prefetch(&dp.st_used[next_h], 0, 1);
        }
    }
    return false;
}

// ─── Binary DP I/O ────────────────────────────────────────────────────────────
#pragma pack(push,1)
struct DpItem{ fp_t fp; uint8_t priv[32]; };
#pragma pack(pop)

void saveDPBinary(const std::string& fn){
    std::ofstream f(fn,std::ios::binary|std::ios::trunc);
    if(!f){ std::cerr<<"[ERR] open "<<fn<<"\n"; return; }
    uint64_t cnt=0;
    for(size_t h=0;h<dp.cap;++h){
        if(!dp.st_used[h].load(std::memory_order_acquire)) continue;
        DpItem it{dp.slots[h].fp};
        Int p; scalarToInt(dp.slots[h].key,p); p.Get32Bytes(it.priv);
        f.write(reinterpret_cast<char*>(&it),sizeof(it)); ++cnt;
    }
    std::cout<<"Saved "<<cnt<<" traps to "<<fn
             <<" ("<<humanBytes(f.tellp())<<")\n";
}
bool loadDPBinary(const std::string& fn){
    std::ifstream f(fn,std::ios::binary|std::ios::ate);
    if(!f){ std::cerr<<"[ERR] open "<<fn<<"\n"; return false; }
    if(f.tellg()%sizeof(DpItem)){ std::cerr<<"[ERR] bad size\n"; return false; }
    f.seekg(0);
    DpItem it; uint64_t n = 0;
    while(f.read(reinterpret_cast<char*>(&it),sizeof(it))){
        Int p; p.Set32Bytes(it.priv);
        if(dp_insert_unique(it.fp,p)) bloom->Add(uint32_t(it.fp));
        if((++n & 0xFFFFF) == 0) std::cout<<"\rLoaded "<<n<<std::flush;
    }
    std::cout<<"\rLoaded "<<n<<" traps (done)\n";
    return true;
}

// ─── range split ──────────────────────────────────────────────────────────────
struct RangeSeg{ Int start,length; };
static std::vector<RangeSeg>
splitRange(const Int& A,const Int& total,unsigned parts){
    std::vector<RangeSeg> seg(parts);
    Int chunk(total); Int div((uint64_t)parts); chunk.Div(&div,nullptr);
    Int lenLast(total);
    if(parts>1){ Int t(chunk); Int m((uint64_t)(parts-1)); t.Mult(&m); lenLast.Sub(&t); }
    for(unsigned i=0;i<parts;++i){
        seg[i].start = A;
        if(i){
            Int off(chunk); Int k((uint64_t)i); off.Mult(&k); seg[i].start.Add(&off);
        }
        seg[i].length = (i==parts-1)?lenLast:chunk;
    }
    return seg;
}

// ─── xoshiro rng ──────────────────────────────────────────────────────────────
struct xoshiro256ss {
    uint64_t s[4];

    static inline uint64_t rotl(uint64_t x, int k) noexcept {
        return (x << k) | (x >> (64 - k));
    }

    explicit xoshiro256ss(uint64_t seed = 1) {
        for (int i = 0; i < 4; ++i) {
            seed = splitmix64(seed);
            s[i] = seed;
        }
    }

    inline uint64_t operator()() noexcept { return next(); }

    inline uint64_t next() noexcept {
        const uint64_t result = rotl(s[1] * 5, 7) * 9;
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0];  s[3] ^= s[1];
        s[1] ^= s[2];  s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }

    using result_type = uint64_t;
    static constexpr uint64_t min() { return 0; }
    static constexpr uint64_t max() { return ~0ULL; }
};

// ─── batch-EC-add ─────────────────────────────────────────────────────────────
template<unsigned N>
static inline void batchAdd(Point* base,Point* plus){
    std::array<Int,N> dX;
    // Unroll small loops for better performance
    if constexpr(N >= 4) {
        for(unsigned i=0;i<N;i+=4){
            dX[i].ModSub(&plus[i].x,&base[i].x);
            dX[i+1].ModSub(&plus[i+1].x,&base[i+1].x);
            dX[i+2].ModSub(&plus[i+2].x,&base[i+2].x);
            dX[i+3].ModSub(&plus[i+3].x,&base[i+3].x);
        }
    } else {
        for(unsigned i=0;i<N;++i) dX[i].ModSub(&plus[i].x,&base[i].x);
    }
    static thread_local IntGroup grp(N); grp.Set(dX.data()); grp.ModInv();

    // Unroll the main computation loop
    if constexpr(N >= 4) {
        for(unsigned i=0;i<N;i+=4){
            // First point
            Int dY0; dY0.ModSub(&plus[i].y,&base[i].y);
            Int k0 ; k0 .ModMulK1(&dY0,&dX[i]);
            Int k2_0; k2_0.ModSquareK1(&k0);
            Int xn0(base[i].x); xn0.ModNeg(); xn0.ModAdd(&k2_0); xn0.ModSub(&plus[i].x);
            Int dx0(base[i].x); dx0.ModSub(&xn0); dx0.ModMulK1(&k0);
            base[i].x = xn0;
            base[i].y.ModNeg();
            base[i].y.ModAdd(&dx0);

            // Second point
            Int dY1; dY1.ModSub(&plus[i+1].y,&base[i+1].y);
            Int k1 ; k1 .ModMulK1(&dY1,&dX[i+1]);
            Int k2_1; k2_1.ModSquareK1(&k1);
            Int xn1(base[i+1].x); xn1.ModNeg(); xn1.ModAdd(&k2_1); xn1.ModSub(&plus[i+1].x);
            Int dx1(base[i+1].x); dx1.ModSub(&xn1); dx1.ModMulK1(&k1);
            base[i+1].x = xn1;
            base[i+1].y.ModNeg();
            base[i+1].y.ModAdd(&dx1);

            // Third point
            Int dY2; dY2.ModSub(&plus[i+2].y,&base[i+2].y);
            Int k2 ; k2 .ModMulK1(&dY2,&dX[i+2]);
            Int k2_2; k2_2.ModSquareK1(&k2);
            Int xn2(base[i+2].x); xn2.ModNeg(); xn2.ModAdd(&k2_2); xn2.ModSub(&plus[i+2].x);
            Int dx2(base[i+2].x); dx2.ModSub(&xn2); dx2.ModMulK1(&k2);
            base[i+2].x = xn2;
            base[i+2].y.ModNeg();
            base[i+2].y.ModAdd(&dx2);

            // Fourth point
            Int dY3; dY3.ModSub(&plus[i+3].y,&base[i+3].y);
            Int k3 ; k3 .ModMulK1(&dY3,&dX[i+3]);
            Int k2_3; k2_3.ModSquareK1(&k3);
            Int xn3(base[i+3].x); xn3.ModNeg(); xn3.ModAdd(&k2_3); xn3.ModSub(&plus[i+3].x);
            Int dx3(base[i+3].x); dx3.ModSub(&xn3); dx3.ModMulK1(&k3);
            base[i+3].x = xn3;
            base[i+3].y.ModNeg();
            base[i+3].y.ModAdd(&dx3);
        }
    } else {
        for(unsigned i=0;i<N;++i){
            Int dY; dY.ModSub(&plus[i].y,&base[i].y);
            Int k ; k .ModMulK1(&dY,&dX[i]);
            Int k2; k2.ModSquareK1(&k);
            Int xn(base[i].x); xn.ModNeg(); xn.ModAdd(&k2); xn.ModSub(&plus[i].x);
            Int dx(base[i].x); dx.ModSub(&xn); dx.ModMulK1(&k);
            base[i].x = xn;
            base[i].y.ModNeg();
            base[i].y.ModAdd(&dx);
        }
    }
}

// ─── jump-table ───────────────────────────────────────────────────────────────
static std::vector<Point> jumps;
static void buildJumpTable(unsigned k){
    jumps.resize(k);
#pragma omp parallel for schedule(static)
    for(unsigned i=0;i<k;++i){
        Int e((uint64_t)1); e.ShiftL(int(i+1));
        jumps[i] = mulP(e);
    }
}

// ─── globals ──────────────────────────────────────────────────────────────────
static std::atomic<uint64_t> hops{0}, restarts{0};
static std::atomic<bool>     solved{false};
static Int  privFound;
static std::atomic<unsigned> found_tid{0};
static std::atomic<uint64_t> winner_wraps{0};
static std::once_flag        record_flag;

// ─── wrap helper ──────────────────────────────────────────────────────────────
static inline void addWrapCnt(Int& v,const Int& d,const Int& len,uint64_t& wraps){
    v.Add(&const_cast<Int&>(d));
    if(intGE(v,len)){ v.Sub(&const_cast<Int&>(len)); ++wraps; }
}

// ─── Traps (Phase-1) ─────────────────────────────────────────────────────────
static constexpr unsigned K_DP = 512;
static void buildDP_segment(const RangeSeg& seg,uint64_t target,
                            unsigned k,unsigned bits,uint64_t seed){
    const uint64_t mask = (1ULL<<bits)-1;
    xoshiro256ss rng(seed);
    std::uniform_int_distribution<uint64_t> rd;

    std::array<Int,   K_DP> dist;
    std::array<uint64_t,K_DP> wraps{};
    std::array<Point, K_DP> cur, stepPts;

    const size_t BATCH_SIZE = 4096;  // Increased for better cache utilization
    std::vector<std::pair<fp_t,Int>> batch;
    batch.reserve(BATCH_SIZE);

    auto rndMod=[&](Int& o){
        o.SetInt32(0); int parts=(bitlen(seg.length)+63)/64;
        for(int p=0;p<parts;++p){
            Int t((uint64_t)rd(rng)); t.ShiftL(p*64); o.Add(&t);
        }
        o.Mod(&const_cast<Int&>(seg.length));
    };
    for(unsigned i=0;i<K_DP;++i){
        rndMod(dist[i]); Int a(seg.start); a.Add(&dist[i]); cur[i] = mulP(a);
    }

    uint64_t made = 0;
    while(made < target){
        for(unsigned i=0;i<K_DP;++i){
            uint64_t h = splitmix64(IntLow64(cur[i].x)) % k;
            Int step((uint64_t)1); step.ShiftL(int(h+1));

            if((IntLow64(cur[i].x) & mask) == 0){
                fp_t fp = splitmix64(IntLow64(cur[i].x) ^
                                     uint64_t(!cur[i].y.IsEven()));

                Int scalar(seg.length);
                Int w((uint64_t)wraps[i]); scalar.Mult(&w);
                scalar.Add(&const_cast<Int&>(dist[i]));
                scalar.Add(&const_cast<Int&>(seg.start));
                scalar.Mod(&ORDER_N);

                batch.emplace_back(fp,scalar);
                if(batch.size() >= BATCH_SIZE || made + batch.size() >= target){
#pragma omp critical(dp_insert)
                    {
                        for(auto& it: batch){
                            if(dp_insert_unique(it.first,it.second)){
                                bloom->Add(uint32_t(it.first));
                                ++made;
                                if(made==target) break;
                            }
                        }
                        batch.clear();
                    }
                }
            }

            stepPts[i] = jumps[h];
            addWrapCnt(dist[i],step,seg.length,wraps[i]);
        }
        batchAdd<K_DP>(cur.data(),stepPts.data());
    }
    if(!batch.empty()){
#pragma omp critical(dp_insert)
        {
            for(auto& it: batch){
                if(dp_insert_unique(it.first,it.second)){
                    bloom->Add(uint32_t(it.first));
                    ++made;
                }
            }
            batch.clear();
        }
    }
}

// ─── Phase-2: wild kangaroos ─────────────────────────────────────────────────
static constexpr unsigned K   = 512;
static constexpr unsigned CACHE_LIMIT = 1024;

struct PendingCheck{ fp_t fp; unsigned idx; };

static void worker(uint32_t tid,const RangeSeg& seg,const Point& pub,
                   unsigned k,unsigned bits){
    struct LoopDet{
        uint64_t next,cnt,sig;
        void reset(uint64_t s){ next=1024; cnt=0; sig=s; }
    };

    const uint64_t mask = (1ULL<<bits)-1;
    std::mt19937_64 rng(splitmix64(0xDEADBEEF*tid));
    std::uniform_int_distribution<uint64_t> rd;

    std::array<Int,   K> dist;
    std::array<uint64_t,K> wraps{};
    std::array<Point, K> cur, stepPts;
    std::array<LoopDet,K> loop;

    auto rndMod=[&](Int& o){
        o.SetInt32(0); int parts=(bitlen(seg.length)+63)/64;
        for(int p=0;p<parts;++p){
            Int t((uint64_t)rd(rng)); t.ShiftL(p*64); o.Add(&t);
        }
        o.Mod(&const_cast<Int&>(seg.length));
    };
    for(unsigned i=0;i<K;++i){
        rndMod(dist[i]);
        cur[i]  = addP(pub,mulP(dist[i]));
        loop[i].reset(splitmix64(IntLow64(cur[i].x)^uint64_t(!cur[i].y.IsEven())));
    }

    madvise(dp.slots,dp.mapBytes,MADV_SEQUENTIAL);

    uint64_t local=0; const uint64_t FLUSH = 1ULL<<18;
    std::vector<PendingCheck> cache; cache.reserve(CACHE_LIMIT);

    // Use aligned cache for better SIMD performance
    alignas(64) std::array<PendingCheck, CACHE_LIMIT> aligned_cache;
    size_t cache_idx = 0;

    // Prefetch hints for better cache performance
    auto prefetch_dp = [](size_t idx) {
        __builtin_prefetch(&dp.slots[idx], 0, 1);  // Read prefetch
    };

    while(!solved.load()){
        for(unsigned i=0;i<K;++i){
            if(solved.load()) return;

            uint64_t x64 = IntLow64(cur[i].x);
            uint64_t h   = splitmix64(x64)%k;

            auto& ld = loop[i];
            if(++ld.cnt == ld.next){
                uint64_t sig = splitmix64(x64^uint64_t(!cur[i].y.IsEven()));
                if(sig == ld.sig){
                    rndMod(dist[i]);
                    cur[i]  = addP(pub,mulP(dist[i]));
                    wraps[i]=0; ld.reset(sig);
                    restarts.fetch_add(1,std::memory_order_relaxed);
                    continue;
                }
                ld.sig = sig; ld.next <<= 1;
            }

            stepPts[i] = jumps[h];
            Int step((uint64_t)1); step.ShiftL(int(h+1));
            addWrapCnt(dist[i],step,seg.length,wraps[i]);
            ++local;
        }
        batchAdd<K>(cur.data(),stepPts.data());
        if(local >= FLUSH){ hops.fetch_add(local); local = 0; }

        for(unsigned i=0;i<K;++i){
            if(solved.load()) return;
            if((IntLow64(cur[i].x)&mask)!=0) continue;

            fp_t fp = splitmix64(IntLow64(cur[i].x)^uint64_t(!cur[i].y.IsEven()));
            aligned_cache[cache_idx++] = {fp,i};

            if(cache_idx >= CACHE_LIMIT){
#pragma omp critical(dp_query)
                {
                    for(size_t c=0; c<cache_idx; ++c){
                        auto& item = aligned_cache[c];
                        if(!bloom->Find(uint32_t(item.fp))) continue;
                        Int trap;
                        if(!dp_find(item.fp,trap)) continue;

                        Int dw(seg.length);
                        Int w((uint64_t)wraps[item.idx]); dw.Mult(&w);
                        dw.Add(&const_cast<Int&>(dist[item.idx]));
                        dw.Mod(&ORDER_N);

                        Int priv; intCopy(priv,trap); priv.Sub(&dw); priv.Mod(&ORDER_N);
                        Point tst = mulP(priv);
                        if(tst.x.IsEqual(&const_cast<Int&>(pub.x)) &&
                           tst.y.IsEqual(&const_cast<Int&>(pub.y)))
                        {
                            std::call_once(record_flag,[]{});
                            intCopy(privFound,priv);
                            found_tid.store(tid);
                            winner_wraps.store(wraps[item.idx]);
                            solved.store(true);
                        }
                    }
                }
                cache_idx = 0;
                if(solved.load()) return;
            }
        }
    }
    if(local) hops.fetch_add(local);
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(int argc,char** argv)
{
    /* init curve */
    P_PRIME.SetBase16((char*)P_HEX); ORDER_N.SetBase16((char*)N_HEX); secp.Init();

    /* ── CLI ── */
    Int A,B; uint64_t traps=0; unsigned bits=12; size_t ramGB=8;
    Point pub; unsigned k_user=0; bool saveDP=false, loadDP=false;
    std::string dpFile;
    for(int i=1;i<argc;++i){
        std::string a=argv[i];
        if(a=="--range"){ std::string s=argv[++i]; auto p=s.find(':');
            A.SetBase10((char*)s.substr(0,p).c_str());
            B.SetBase10((char*)s.substr(p+1).c_str());
        }else if(a=="--dp_point") traps=std::strtoull(argv[++i],nullptr,10);
        else if(a=="--dp_bits")  bits=std::stoul(argv[++i]);
        else if(a=="--ram"||a=="--ram-limit") ramGB=std::stoull(argv[++i]);
        else if(a=="--k") k_user=std::stoul(argv[++i]);
        else if(a=="--pubkey"){
            std::string h=argv[++i]; if(h.rfind("0x",0)==0) h.erase(0,2);
            char pc=h[1]; Int x; x.SetBase16((char*)h.substr(2).c_str());
            pub.x=x; pub.y=secp.GetY(x,pc=='2');
        }else if(a=="--save-dp"||a=="-s") saveDP=true;
        else if(a=="--load-dp"){ loadDP=true; dpFile=argv[++i]; }
        else{ std::cerr<<"Unknown "<<a<<'\n'; return 1; }
    }
    if(A.IsZero()||B.IsZero()){ std::cerr<<"--range missing\n"; return 1; }

    /* ── params ── */
    Int range(B); range.Sub(&A);
    unsigned Lbits=range.GetBitLength();
    if(!traps){
        traps=(Lbits>=52)?(1ULL<<(Lbits/2))
             : uint64_t(std::ceil(range.ToDouble()/std::sqrt(range.ToDouble())));
    }
    unsigned k = k_user? k_user : std::max(1u,Lbits/2);

    /* новые параметры */
    constexpr double MAX_LOAD     = 0.50; 
    constexpr double bloomFactor  = 10.0;  

    size_t cap0 = size_t(std::ceil(double(traps) / MAX_LOAD));
    size_t cap  = 1;
    while(cap < cap0) cap <<= 1;

    dp.init("dp_table.bin",cap);

    size_t bloomBytes=size_t(traps*bloomFactor);
    std::cout<<"\n=========== Phase-0: Data summary ==========\n";
    std::cout<<"DP table (SSD): "<<humanBytes(cap*sizeof(DPSlot))
             <<"  ( "<<traps<<" / "<<cap<<" slots, load "
             <<std::fixed<<std::setprecision(2)
             <<double(traps)/cap*100<<"% )\n";
    std::cout<<"Bloom    (RAM): "<<humanBytes(bloomBytes)<<'\n';

    bloom=new simd_bloom::SimdBlockFilterFixed<>(bloomBytes);

    unsigned th=std::max(1u,std::thread::hardware_concurrency());
    auto segs=splitRange(A,range,th);
    uint64_t per=(traps+th-1)/th;
    buildJumpTable(k);

    // ─── Phase-1 ────────────────────────────────────────────────────────────
    dp.enable_flush.store(false);            
    std::cout<<"\n========== Phase-1: Building traps =========\n";
    if(loadDP){
        if(!loadDPBinary(dpFile)) return 1;
    }else{
        std::thread progress([&]{
            while(dpDone.load()<traps){
                std::cout<<"\rUnique traps: "<<dpDone<<'/'<<traps<<std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            std::cout<<"\rUnique traps: "<<traps<<"/"<<traps<<" (done)\n";
        });
#pragma omp parallel for schedule(static)
        for(unsigned t=0;t<th;++t)
            buildDP_segment(segs[t],per,k,bits,
                            splitmix64(0xABCDEF12345678ULL^t));
        progress.join();
        if(saveDP) saveDPBinary("DP.bin");
    }
    dp.fullSync();                         
    dp.enable_flush.store(true);

    // ─── Phase-2 ────────────────────────────────────────────────────────────
    std::cout<<"\n=========== Phase-2: Kangaroos =============\n";
    auto t0=std::chrono::steady_clock::now();
    std::thread pool([&]{
#pragma omp parallel for num_threads(th) schedule(static)
        for(unsigned id=0;id<th;++id) worker(id,segs[id],pub,k,bits);
    });

    uint64_t lastHops=0;
    auto lastStat=t0;
    while(true){
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if(solved.load()) break;
        auto nowTick=std::chrono::steady_clock::now();
        if(nowTick-lastStat<std::chrono::seconds(5)) continue;

        double dt=std::chrono::duration<double>(nowTick-lastStat).count();
        lastStat=nowTick;
        uint64_t now=hops.load(); uint64_t delta=now-lastHops; lastHops=now;
        double sp=delta/dt; const char* u=" H/s";
        if(sp>1e6){ sp/=1e6; u=" MH/s";}
        else if(sp>1e3){ sp/=1e3; u=" kH/s";}
        uint64_t sec=std::chrono::duration_cast<std::chrono::seconds>(nowTick-t0).count();

        std::cout<<"\rSpeed: "<<std::fixed<<std::setprecision(2)<<sp<<u
                 <<" | Hops: "<<now
                 <<" | Restart wild: "<<restarts.load()
                 <<" | Time: "<<sec/3600<<':'<<std::setw(2)<<std::setfill('0')
                 <<(sec/60)%60<<':'<<std::setw(2)<<sec%60<<std::flush;
    }
    pool.join();

    // ─── Phase-3 ────────────────────────────────────────────────────────────
    auto msTot=std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now()-t0).count();
    uint64_t h=msTot/3'600'000,m=(msTot/60'000)%60,s=(msTot/1'000)%60,ms=msTot%1'000;

    std::cout<<"\n\n============= Phase-3: Result ==============\n";
    std::cout<<"Private key : 0x"<<std::setw(64)<<std::setfill('0')
             <<privFound.GetBase16()<<"\n";
    std::cout<<"Found by thr: "<<found_tid.load()<<"\n";
    std::cout<<"Wild wraps  : "<<winner_wraps.load()
             <<(winner_wraps.load()?"  [wrapped]\n":"  [no wrap]\n");
    std::cout<<"Wild restart: "<<restarts.load()<<"\n";
    std::cout<<"Total time  : "<<std::setw(2)<<h<<':'<<std::setw(2)<<m<<':'
             <<std::setw(2)<<s<<'.'<<std::setw(3)<<ms<<"\n";

    { std::ofstream("FOUND.txt")<<"0x"<<std::setw(64)<<std::setfill('0')
                                <<privFound.GetBase16()<<"\n"; }
    std::cout<<"Private key : saved to FOUND.txt\n";

    delete bloom; dp.close();
    return 0;
}
