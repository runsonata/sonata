// @sonata/crypto: hashing, encryption and randoms
//
//     local crypto = require("@sonata/crypto")
//
// Documentation WIP


#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif
// AES-NI is selected at runtime (cpuid) so no special compiler flags are needed.
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define SONATA_AESNI 1
#endif

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"

namespace sonata::lib::libs {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;
using Bytes = std::string;
using SV = std::string_view;

const u8* cu8(SV s) { return reinterpret_cast<const u8*>(s.data()); }
u8* mu8(Bytes& s) { return reinterpret_cast<u8*>(s.data()); }
Bytes bytesOf(const void* p, size_t n) { return Bytes(static_cast<const char*>(p), n); }
SV viewOf(const void* p, size_t n) { return SV(static_cast<const char*>(p), n); }

template <class T> T rol(T x, int n) { return T((x << n) | (x >> (sizeof(T) * 8 - n))); }
template <class T> T ror(T x, int n) { return T((x >> n) | (x << (sizeof(T) * 8 - n))); }
template <class T> T LD(const u8* p, bool be) {
    T v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) v |= T(p[be ? sizeof(T) - 1 - i : i]) << (8 * i);
    return v;
}
template <class T> void ST(u8* p, T v, bool be) {
    for (size_t i = 0; i < sizeof(T); ++i) p[be ? sizeof(T) - 1 - i : i] = u8(v >> (8 * i));
}

bool ctEq(SV a, SV b) {
    if (a.size() != b.size()) return false;
    u8 d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= u8(a[i] ^ b[i]);
    return d == 0;
}

u64 nowNs() {
    using namespace std::chrono;
    return u64(duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

///////////// randomness

bool osRandom(void* p, size_t n) {
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(p), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(p, n);
    return true;
#else
    std::FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) return false;
    const bool ok = std::fread(p, 1, n, f) == n;
    std::fclose(f);
    return ok;
#endif
}

bool randomFill(Bytes& b, size_t n) {
    b.assign(n, '\0');
    return n == 0 || osRandom(b.data(), n);
}

// Nonces/IVs: OS randomness with the nanosecond clock folded in, so a value is never a
// constant and never repeats even if the entropy source misbehaves.
bool nonceFill(Bytes& b, size_t n) {
    if (!randomFill(b, n)) return false;
    const u64 t = nowNs();
    for (size_t i = 0; i < n && i < 8; ++i) b[i] = char(b[i] ^ u8(t >> (8 * i)));
    return true;
}

///////////// hashes

// SHA-512 round constants and IVs (first 8: SHA-512, next 8: SHA-384). Every other
// 32/64-bit constant used below (SHA-256/224, BLAKE2) is derived from these.
constexpr u64 kK[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc,
    0x3956c25bf348b538, 0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118,
    0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
    0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694,
    0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4,
    0xc6e00bf33da88fc2, 0xd5a79147930aa725, 0x06ca6351e003826f, 0x142929670a0e6e70,
    0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
    0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30,
    0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8,
    0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3,
    0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b,
    0xca273eceea26619c, 0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178,
    0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c,
    0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817
};
constexpr u64 kIV[16] = {
    0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
    0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179,
    0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
    0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4
};

struct Hasher {
    virtual ~Hasher() = default;
    virtual void update(const u8* p, size_t n) = 0;
    virtual void final(u8* out) = 0;
};
using HasherPtr = std::unique_ptr<Hasher>;

// Merkle-Damgard buffering/padding shared by MD5, SHA-1 and SHA-2.
struct MDHash : Hasher {
    size_t block;
    bool le;
    u8 buf[128];
    size_t n = 0;
    u64 total = 0;
    MDHash(size_t b, bool little) : block(b), le(little) {}
    virtual void compress(const u8* p) = 0;
    void update(const u8* p, size_t len) override {
        total += len;
        while (len) {
            const size_t c = std::min(block - n, len);
            std::memcpy(buf + n, p, c);
            n += c; p += c; len -= c;
            if (n == block) { compress(buf); n = 0; }
        }
    }
    void pad() {
        const u64 bits = total * 8;
        buf[n++] = 0x80;
        if (n > block - (block == 128 ? 16 : 8)) {
            std::memset(buf + n, 0, block - n);
            compress(buf);
            n = 0;
        }
        std::memset(buf + n, 0, block - n);
        ST<u64>(buf + block - 8, bits, !le);
        compress(buf);
    }
};

template <class W> struct BEHash : MDHash {
    W h[8]{};
    size_t outLen;
    BEHash(size_t blk, size_t out) : MDHash(blk, false), outLen(out) {}
    void final(u8* out) override {
        pad();
        u8 t[sizeof(W) * 8];
        for (int i = 0; i < 8; ++i) ST<W>(t + i * sizeof(W), h[i], true);
        std::memcpy(out, t, outLen);
    }
};

struct Md5 : MDHash {
    u32 h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    Md5() : MDHash(64, true) {}
    void compress(const u8* p) override {
        static const std::array<u32, 64> K = [] {  // floor(2^32 * |sin(i + 1)|)
            std::array<u32, 64> k{};
            for (int i = 0; i < 64; ++i) k[i] = u32(std::fabs(std::sin(double(i + 1))) * 4294967296.0);
            return k;
        }();
        static const u8 S[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
        u32 m[16], a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 16; ++i) m[i] = LD<u32>(p + 4 * i, false);
        for (int i = 0; i < 64; ++i) {
            u32 f;
            int g;
            if (i < 16)      { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) % 16; }
            else             { f = c ^ (b | ~d);       g = (7 * i) % 16; }
            f += a + K[i] + m[g];
            a = d; d = c; c = b;
            b += rol(f, S[(i / 16) * 4 + i % 4]);
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    void final(u8* out) override {
        pad();
        for (int i = 0; i < 4; ++i) ST<u32>(out + 4 * i, h[i], false);
    }
};

struct Sha1 : BEHash<u32> {
    Sha1() : BEHash(64, 20) {
        const u32 iv[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
        std::copy(iv, iv + 5, h);
    }
    void compress(const u8* p) override {
        u32 w[80];
        for (int i = 0; i < 16; ++i) w[i] = LD<u32>(p + 4 * i, true);
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            const u32 t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
};

// SHA-224/256 (W = u32) and SHA-384/512 (W = u64). `variant` 0 = 256/512, 1 = 224/384.
template <class W> struct Sha2 : BEHash<W> {
    static constexpr bool kWide = sizeof(W) == 8;
    Sha2(int variant, size_t outLen) : BEHash<W>(kWide ? 128 : 64, outLen) {
        for (int i = 0; i < 8; ++i) {
            const u64 v = kIV[i + 8 * variant];
            this->h[i] = (!kWide && !variant) ? W(v >> 32) : W(v);
        }
    }
    void compress(const u8* p) override {
        // rotations: Sigma0 (3), Sigma1 (3), sigma0 (2 + shift), sigma1 (2 + shift)
        static constexpr int R[2][12] = {{2, 13, 22, 6, 11, 25, 7, 18, 3, 17, 19, 10},
                                         {28, 34, 39, 14, 18, 41, 1, 8, 7, 19, 61, 6}};
        const int* r = R[kWide];
        const int rounds = kWide ? 80 : 64;
        W w[80], v[8];
        for (int i = 0; i < 16; ++i) w[i] = LD<W>(p + i * sizeof(W), true);
        for (int i = 16; i < rounds; ++i) {
            const W a = w[i - 15], b = w[i - 2];
            w[i] = w[i - 16] + w[i - 7] + (ror(a, r[6]) ^ ror(a, r[7]) ^ (a >> r[8])) +
                   (ror(b, r[9]) ^ ror(b, r[10]) ^ (b >> r[11]));
        }
        std::copy(this->h, this->h + 8, v);
        for (int i = 0; i < rounds; ++i) {
            const W k = kWide ? W(kK[i]) : W(kK[i] >> 32);
            const W t1 = v[7] + (ror(v[4], r[3]) ^ ror(v[4], r[4]) ^ ror(v[4], r[5])) +
                         ((v[4] & v[5]) ^ (~v[4] & v[6])) + k + w[i];
            const W t2 = (ror(v[0], r[0]) ^ ror(v[0], r[1]) ^ ror(v[0], r[2])) +
                         ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            std::rotate(v, v + 7, v + 8);  // h,a,b,c,d,e,f,g
            v[0] = t1 + t2;
            v[4] += t1;
        }
        for (int i = 0; i < 8; ++i) this->h[i] += v[i];
    }
};

struct Sha3 : Hasher {
    u64 s[25] = {};
    size_t rate, outLen, pos = 0;
    explicit Sha3(size_t out) : rate(200 - 2 * out), outLen(out) {}
    static void permute(u64* a) {
        struct Tables { int rot[25] = {}; u64 rc[24] = {}; };
        static const Tables T = [] {  // rotation offsets and round constants, derived per FIPS 202
            Tables t;
            int x = 1, y = 0;
            for (int i = 0; i < 24; ++i) {
                t.rot[x + 5 * y] = ((i + 1) * (i + 2) / 2) % 64;
                const int nx = y;
                y = (2 * x + 3 * y) % 5;
                x = nx;
            }
            unsigned lfsr = 1;
            for (int r = 0; r < 24; ++r)
                for (int j = 0; j < 7; ++j) {
                    if (lfsr & 1) t.rc[r] |= 1ULL << ((1 << j) - 1);
                    lfsr <<= 1;
                    if (lfsr & 0x100) lfsr ^= 0x171;
                }
            return t;
        }();
        for (int r = 0; r < 24; ++r) {
            u64 c[5], b[25];
            for (int x = 0; x < 5; ++x) c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
            for (int x = 0; x < 5; ++x) {
                const u64 d = c[(x + 4) % 5] ^ rol(c[(x + 1) % 5], 1);
                for (int y = 0; y < 5; ++y) a[x + 5 * y] ^= d;
            }
            for (int x = 0; x < 5; ++x)
                for (int y = 0; y < 5; ++y) {
                    const int n = T.rot[x + 5 * y];
                    b[y + 5 * ((2 * x + 3 * y) % 5)] = n ? rol(a[x + 5 * y], n) : a[x + 5 * y];
                }
            for (int y = 0; y < 5; ++y)
                for (int x = 0; x < 5; ++x)
                    a[x + 5 * y] = b[x + 5 * y] ^ (~b[(x + 1) % 5 + 5 * y] & b[(x + 2) % 5 + 5 * y]);
            a[0] ^= T.rc[r];
        }
    }
    void update(const u8* p, size_t len) override {
        for (size_t i = 0; i < len; ++i) {
            s[pos / 8] ^= u64(p[i]) << (8 * (pos % 8));
            if (++pos == rate) { permute(s); pos = 0; }
        }
    }
    void final(u8* out) override {
        s[pos / 8] ^= u64(0x06) << (8 * (pos % 8));
        s[(rate - 1) / 8] ^= u64(0x80) << (8 * ((rate - 1) % 8));
        permute(s);
        for (size_t i = 0; i < outLen; ++i) out[i] = u8(s[i / 8] >> (8 * (i % 8)));
    }
};

constexpr u8 kBlakeSigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}};

// BLAKE2b (W = u64, 64-byte digest) and BLAKE2s (W = u32, 32-byte digest), unkeyed.
template <class W> struct Blake2 : Hasher {
    static constexpr bool kWide = sizeof(W) == 8;
    static constexpr size_t BS = sizeof(W) * 16, kOut = sizeof(W) * 8;
    W h[8];
    u64 t = 0;
    u8 buf[BS];
    size_t n = 0;
    static W iv(int i) { return kWide ? W(kIV[i]) : W(kIV[i] >> 32); }
    Blake2() {
        for (int i = 0; i < 8; ++i) h[i] = iv(i);
        h[0] ^= W(0x01010000 ^ kOut);
    }
    void compress(bool last) {
        W v[16], m[16];
        for (int i = 0; i < 16; ++i) m[i] = LD<W>(buf + i * sizeof(W), false);
        for (int i = 0; i < 8; ++i) { v[i] = h[i]; v[i + 8] = iv(i); }
        v[12] ^= W(t);
        if (!kWide) v[13] ^= W(t >> 32);
        if (last) v[14] = ~v[14];
        const int rot[4] = {kWide ? 32 : 16, kWide ? 24 : 12, kWide ? 16 : 8, kWide ? 63 : 7};
        auto g = [&](int a, int b, int c, int d, W x, W y) {
            v[a] = v[a] + v[b] + x; v[d] = ror<W>(v[d] ^ v[a], rot[0]);
            v[c] = v[c] + v[d];     v[b] = ror<W>(v[b] ^ v[c], rot[1]);
            v[a] = v[a] + v[b] + y; v[d] = ror<W>(v[d] ^ v[a], rot[2]);
            v[c] = v[c] + v[d];     v[b] = ror<W>(v[b] ^ v[c], rot[3]);
        };
        for (int r = 0; r < (kWide ? 12 : 10); ++r) {
            const u8* s = kBlakeSigma[r % 10];
            g(0, 4, 8, 12, m[s[0]], m[s[1]]);   g(1, 5, 9, 13, m[s[2]], m[s[3]]);
            g(2, 6, 10, 14, m[s[4]], m[s[5]]);  g(3, 7, 11, 15, m[s[6]], m[s[7]]);
            g(0, 5, 10, 15, m[s[8]], m[s[9]]);  g(1, 6, 11, 12, m[s[10]], m[s[11]]);
            g(2, 7, 8, 13, m[s[12]], m[s[13]]); g(3, 4, 9, 14, m[s[14]], m[s[15]]);
        }
        for (int i = 0; i < 8; ++i) h[i] ^= v[i] ^ v[i + 8];
    }
    void update(const u8* p, size_t len) override {
        while (len) {
            if (n == BS) { t += BS; compress(false); n = 0; }  // the last block is held back for final()
            const size_t c = std::min(BS - n, len);
            std::memcpy(buf + n, p, c);
            n += c; p += c; len -= c;
        }
    }
    void final(u8* out) override {
        t += n;
        std::memset(buf + n, 0, BS - n);
        compress(true);
        for (size_t i = 0; i < kOut / sizeof(W); ++i) ST<W>(out + i * sizeof(W), h[i], false);
    }
};

struct HashInfo {
    const char* name;
    size_t digest, block;
    HasherPtr (*make)();
};
#define HASH(name, dig, blk, expr) {name, dig, blk, []() -> HasherPtr { return HasherPtr(new expr); }}
const HashInfo kHashes[] = {
    HASH("md5", 16, 64, Md5()),
    HASH("sha1", 20, 64, Sha1()),
    HASH("sha224", 28, 64, Sha2<u32>(1, 28)),
    HASH("sha256", 32, 64, Sha2<u32>(0, 32)),
    HASH("sha384", 48, 128, Sha2<u64>(1, 48)),
    HASH("sha512", 64, 128, Sha2<u64>(0, 64)),
    HASH("sha3-224", 28, 144, Sha3(28)),
    HASH("sha3-256", 32, 136, Sha3(32)),
    HASH("sha3-384", 48, 104, Sha3(48)),
    HASH("sha3-512", 64, 72, Sha3(64)),
    HASH("blake2b", 64, 128, Blake2<u64>()),
    HASH("blake2s", 32, 64, Blake2<u32>()),
};
#undef HASH

const HashInfo* findHash(SV name) {
    for (const auto& h : kHashes) if (name == h.name) return &h;
    return nullptr;
}

Bytes hashOf(const HashInfo& h, std::initializer_list<SV> parts) {
    const HasherPtr hs = h.make();
    for (SV p : parts) hs->update(cu8(p), p.size());
    Bytes out(h.digest, '\0');
    hs->final(mu8(out));
    return out;
}

Bytes hmac(const HashInfo& h, SV key, SV data) {
    Bytes k(h.block, '\0');
    if (key.size() > h.block) { const Bytes d = hashOf(h, {key}); std::copy(d.begin(), d.end(), k.begin()); }
    else std::copy(key.begin(), key.end(), k.begin());
    Bytes ip = k, op = k;
    for (char& c : ip) c = char(c ^ 0x36);
    for (char& c : op) c = char(c ^ 0x5c);
    return hashOf(h, {op, hashOf(h, {ip, data})});
}

///////////// encodings

struct Codec {
    const char* name;
    const char* alpha;
    int bits;
    bool pad;
};
const Codec kCodecs[] = {
    {"hex", "0123456789abcdef", 4, false},
    {"base64", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", 6, true},
    {"base64url", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", 6, false},
    {"base32", "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567", 5, true},
};

const Codec* findCodec(SV name) {
    for (const auto& c : kCodecs) if (name == c.name) return &c;
    return nullptr;
}

Bytes encode(const Codec& c, SV d) {
    Bytes out;
    u32 acc = 0;
    int nb = 0;
    const u32 mask = (1u << c.bits) - 1;
    for (unsigned char b : d) {
        acc = (acc << 8) | b;
        nb += 8;
        while (nb >= c.bits) { nb -= c.bits; out += c.alpha[(acc >> nb) & mask]; }
        acc &= (1u << nb) - 1;
    }
    if (nb) out += c.alpha[(acc << (c.bits - nb)) & mask];
    if (c.pad) while (out.size() % (c.bits == 5 ? 8 : 4)) out += '=';
    return out;
}

bool decode(const Codec& c, SV s, Bytes& out) {
    out.clear();
    u32 acc = 0;
    int nb = 0;
    for (char ch : s) {
        if (ch == '=' || ch == ' ' || ch == '\n' || ch == '\r') continue;
        if (!ch) return false;
        const char* q = std::strchr(c.alpha, ch);
        if (!q && c.bits != 6) q = std::strchr(c.alpha, c.bits == 4 ? std::tolower((unsigned char)ch) : std::toupper((unsigned char)ch));
        if (!q) return false;
        acc = (acc << c.bits) | u32(q - c.alpha);
        nb += c.bits;
        if (nb >= 8) { nb -= 8; out += char(acc >> nb); acc &= (1u << nb) - 1; }
    }
    return true;
}

///////////// key derivation

Bytes pbkdf2(const HashInfo& h, SV pw, SV salt, u32 iters, size_t len) {
    Bytes out;
    for (u32 i = 1; out.size() < len; ++i) {
        u8 ib[4];
        ST<u32>(ib, i, true);
        Bytes u = hmac(h, pw, Bytes(salt) + bytesOf(ib, 4)), t = u;
        for (u32 j = 1; j < iters; ++j) {
            u = hmac(h, pw, u);
            for (size_t k = 0; k < t.size(); ++k) t[k] = char(t[k] ^ u[k]);
        }
        out += t;
    }
    out.resize(len);
    return out;
}

// Iteration count that makes PBKDF2 take about `ms` milliseconds on this machine.
u32 calibrate(const HashInfo& h, double ms, size_t len) {
    const u32 probe = 2048;
    const auto t0 = std::chrono::steady_clock::now();
    const Bytes sink = pbkdf2(h, "probe", "probe", probe, h.digest);
    const double dt = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const double blocks = double((len + h.digest - (float)1) / h.digest);
    const double n = ms / (std::max(dt / probe, 1e-6) * blocks);
    return u32(std::clamp(n + (sink.empty() ? 1 : 0), 1000.0, 100000000.0));
}

Bytes hkdf(const HashInfo& h, SV ikm, SV salt, SV info, size_t len) {
    const Bytes zero(h.digest, '\0');
    const Bytes prk = hmac(h, salt.empty() ? SV(zero) : salt, ikm);
    Bytes out, t;
    for (u8 i = 1; out.size() < len; ++i) {
        t = hmac(h, prk, t + Bytes(info) + char(i));
        out += t;
    }
    out.resize(len);
    return out;
}

///////////// stream ciphers

constexpr u32 kExpand[4] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};  // "expand 32-byte k"

void chachaRounds(u32* x) {
    auto qr = [&](int a, int b, int c, int d) {
        x[a] += x[b]; x[d] = rol(x[d] ^ x[a], 16);
        x[c] += x[d]; x[b] = rol(x[b] ^ x[c], 12);
        x[a] += x[b]; x[d] = rol(x[d] ^ x[a], 8);
        x[c] += x[d]; x[b] = rol(x[b] ^ x[c], 7);
    };
    for (int i = 0; i < 10; ++i) {
        qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15);
        qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14);
    }
}

void salsaRounds(u32* x, int rounds) {
    auto qr = [&](int a, int b, int c, int d) {
        x[b] ^= rol(x[a] + x[d], 7); x[c] ^= rol(x[b] + x[a], 9);
        x[d] ^= rol(x[c] + x[b], 13); x[a] ^= rol(x[d] + x[c], 18);
    };
    for (int i = 0; i < rounds; i += 2) {
        qr(0, 4, 8, 12); qr(5, 9, 13, 1); qr(10, 14, 2, 6); qr(15, 3, 7, 11);
        qr(0, 1, 2, 3); qr(5, 6, 7, 4); qr(10, 11, 8, 9); qr(15, 12, 13, 14);
    }
}

void chachaInit(u32* s, const u8* key) {
    for (int i = 0; i < 4; ++i) s[i] = kExpand[i];
    for (int i = 0; i < 8; ++i) s[4 + i] = LD<u32>(key + 4 * i, false);
}

void salsaInit(u32* s, const u8* key) {
    for (int i = 0; i < 4; ++i) {
        s[1 + i] = LD<u32>(key + 4 * i, false);
        s[11 + i] = LD<u32>(key + 16 + 4 * i, false);
        s[5 * i] = kExpand[i];
    }
}

// One 64-byte keystream block. ChaCha: 96-bit nonce + 32-bit counter. Salsa: 64-bit nonce + 64-bit counter.
void chachaBlock(const u8* key, const u8* nonce, u64 ctr, u8* out) {
    u32 s[16], x[16];
    chachaInit(s, key);
    s[12] = u32(ctr);
    for (int i = 0; i < 3; ++i) s[13 + i] = LD<u32>(nonce + 4 * i, false);
    std::copy(s, s + 16, x);
    chachaRounds(x);
    for (int i = 0; i < 16; ++i) ST<u32>(out + 4 * i, x[i] + s[i], false);
}

void salsaBlock(const u8* key, const u8* nonce, u64 ctr, u8* out) {
    u32 s[16], x[16];
    salsaInit(s, key);
    s[6] = LD<u32>(nonce, false);
    s[7] = LD<u32>(nonce + 4, false);
    s[8] = u32(ctr);
    s[9] = u32(ctr >> 32);
    std::copy(s, s + 16, x);
    salsaRounds(x, 20);
    for (int i = 0; i < 16; ++i) ST<u32>(out + 4 * i, x[i] + s[i], false);
}

void hchacha(const u8* key, const u8* n16, u8* out) {
    u32 x[16];
    chachaInit(x, key);
    for (int i = 0; i < 4; ++i) x[12 + i] = LD<u32>(n16 + 4 * i, false);
    chachaRounds(x);
    for (int i = 0; i < 4; ++i) { ST<u32>(out + 4 * i, x[i], false); ST<u32>(out + 16 + 4 * i, x[12 + i], false); }
}

void hsalsa(const u8* key, const u8* n16, u8* out) {
    static const int idx[8] = {0, 5, 10, 15, 6, 7, 8, 9};
    u32 x[16];
    salsaInit(x, key);
    for (int i = 0; i < 4; ++i) x[6 + i] = LD<u32>(n16 + 4 * i, false);
    salsaRounds(x, 20);
    for (int i = 0; i < 8; ++i) ST<u32>(out + 4 * i, x[idx[i]], false);
}

// id: 0 chacha20, 1 xchacha20, 2 salsa20, 3 xsalsa20 (odd ids use a 24-byte nonce).
Bytes streamXor(int id, const u8* key, const u8* nonce, u64 ctr, SV data) {
    const bool chacha = id < 2;
    u8 sub[32], n12[12];
    if (id & 1) {
        (chacha ? hchacha : hsalsa)(key, nonce, sub);
        key = sub;
        nonce += 16;
        if (chacha) { std::memset(n12, 0, 4); std::memcpy(n12 + 4, nonce, 8); nonce = n12; }
    }
    Bytes out(data.size(), '\0');
    u8 blk[64];
    for (size_t o = 0; o < data.size(); o += 64, ++ctr) {
        (chacha ? chachaBlock : salsaBlock)(key, nonce, ctr, blk);
        const size_t m = std::min<size_t>(64, data.size() - o);
        for (size_t i = 0; i < m; ++i) out[o + i] = char(u8(data[o + i]) ^ blk[i]);
    }
    return out;
}

// Poly1305 (radix-2^8 limbs; slow but small and branch-free).
Bytes poly1305(const u8* k, SV msg) {
    u32 x[17], r[17] = {}, h[17] = {}, c[17], g[17];
    static const u32 minusp[17] = {5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 252};
    auto add = [](u32* a, const u32* b) {
        u32 u = 0;
        for (int j = 0; j < 17; ++j) { u += a[j] + b[j]; a[j] = u & 255; u >>= 8; }
    };
    for (int j = 0; j < 16; ++j) r[j] = k[j];
    r[3] &= 15; r[4] &= 252; r[7] &= 15; r[8] &= 252; r[11] &= 15; r[12] &= 252; r[15] &= 15;
    const u8* p = cu8(msg);
    size_t n = msg.size();
    while (n > 0) {
        std::fill(c, c + 17, 0u);
        size_t j = 0;
        for (; j < 16 && j < n; ++j) c[j] = p[j];
        c[j] = 1;
        p += j; n -= j;
        add(h, c);
        for (int i = 0; i < 17; ++i) {
            x[i] = 0;
            for (int m = 0; m < 17; ++m) x[i] += h[m] * (m <= i ? r[i - m] : 320 * r[i + 17 - m]);
        }
        std::copy(x, x + 17, h);
        u32 u = 0;
        for (int m = 0; m < 16; ++m) { u += h[m]; h[m] = u & 255; u >>= 8; }
        u += h[16]; h[16] = u & 3; u = 5 * (u >> 2);
        for (int m = 0; m < 16; ++m) { u += h[m]; h[m] = u & 255; u >>= 8; }
        u += h[16]; h[16] = u;
    }
    std::copy(h, h + 17, g);
    add(h, minusp);
    const u32 s = u32(0) - (h[16] >> 7);
    for (int m = 0; m < 17; ++m) h[m] ^= s & (g[m] ^ h[m]);
    for (int m = 0; m < 16; ++m) c[m] = k[16 + m];
    c[16] = 0;
    add(h, c);
    u8 out[16];
    for (int m = 0; m < 16; ++m) out[m] = u8(h[m]);
    return bytesOf(out, 16);
}

Bytes aeadTag(SV otk, SV aad, SV ct) {  // RFC 8439 construction
    Bytes m;
    auto pad = [&](SV d) { m.append(d); m.append((16 - d.size() % 16) % 16, '\0'); };
    pad(aad);
    pad(ct);
    u8 l[16];
    ST<u64>(l, aad.size(), false);
    ST<u64>(l + 8, ct.size(), false);
    m.append(bytesOf(l, 16));
    return poly1305(cu8(otk), m);
}

///////////// block ciphers

struct Block {
    size_t bs;
    explicit Block(size_t b) : bs(b) {}
    virtual ~Block() = default;
    virtual void enc(const u8* in, u8* out) = 0;
    virtual void dec(const u8* in, u8* out) = 0;
};
using BlockPtr = std::unique_ptr<Block>;

struct AesTables {
    u8 sb[256], isb[256];
    AesTables() {  // S-box from GF(2^8) inversion + affine map, no table to mistype
        u8 p = 1, q = 1;
        do {
            p = u8(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q ^= u8(q << 1); q ^= u8(q << 2); q ^= u8(q << 4);
            if (q & 0x80) q ^= 0x09;
            sb[p] = u8(q ^ rol(q, 1) ^ rol(q, 2) ^ rol(q, 3) ^ rol(q, 4) ^ 0x63);
        } while (p != 1);
        sb[0] = 0x63;
        for (int i = 0; i < 256; ++i) isb[sb[i]] = u8(i);
    }
};
const AesTables& aesTables() { static const AesTables t; return t; }
u8 xt(u8 x) { return u8((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

#ifdef SONATA_AESNI
#define AESNI __attribute__((target("aes,sse2")))
bool aesHardware() {
    static const bool v = (__builtin_cpu_init(), __builtin_cpu_supports("aes") && __builtin_cpu_supports("sse2"));
    return v;
}
AESNI void aesniInvKeys(const u8 (*rk)[16], u8 (*drk)[16], int nr) {
    for (int i = 1; i < nr; ++i)
        _mm_storeu_si128((__m128i*)drk[i], _mm_aesimc_si128(_mm_loadu_si128((const __m128i*)rk[i])));
}
AESNI void aesniBlock(const u8 (*rk)[16], const u8 (*drk)[16], int nr, const u8* in, u8* out, bool dec) {
    __m128i s = _mm_loadu_si128((const __m128i*)in);
    if (!dec) {
        s = _mm_xor_si128(s, _mm_loadu_si128((const __m128i*)rk[0]));
        for (int i = 1; i < nr; ++i) s = _mm_aesenc_si128(s, _mm_loadu_si128((const __m128i*)rk[i]));
        s = _mm_aesenclast_si128(s, _mm_loadu_si128((const __m128i*)rk[nr]));
    } else {
        s = _mm_xor_si128(s, _mm_loadu_si128((const __m128i*)rk[nr]));
        for (int i = nr - 1; i > 0; --i) s = _mm_aesdec_si128(s, _mm_loadu_si128((const __m128i*)drk[i]));
        s = _mm_aesdeclast_si128(s, _mm_loadu_si128((const __m128i*)rk[0]));
    }
    _mm_storeu_si128((__m128i*)out, s);
}
#else
bool aesHardware() { return false; }
#endif

struct Aes : Block {
    u8 rk[15][16], drk[15][16];
    int nr;
    bool hw = false;
    Aes(const u8* key, size_t kl) : Block(16), nr(int(kl / 4) + 6) {
        const u8* sb = aesTables().sb;
        const int nk = int(kl / 4);
        u8 w[240], rc = 1;
        std::memcpy(w, key, kl);
        for (int i = nk; i < 4 * (nr + 1); ++i) {
            u8 t[4];
            std::memcpy(t, w + 4 * (i - 1), 4);
            if (i % nk == 0) {
                const u8 x = t[0];
                t[0] = u8(sb[t[1]] ^ rc); t[1] = sb[t[2]]; t[2] = sb[t[3]]; t[3] = sb[x];
                rc = xt(rc);
            } else if (nk > 6 && i % nk == 4) {
                for (u8& b : t) b = sb[b];
            }
            for (int j = 0; j < 4; ++j) w[4 * i + j] = u8(w[4 * (i - nk) + j] ^ t[j]);
        }
        std::memcpy(rk, w, 16 * (nr + 1));
#ifdef SONATA_AESNI
        if ((hw = aesHardware())) aesniInvKeys(rk, drk, nr);
#endif
    }
    static void mix(u8* s, bool inv) {
        for (int c = 0; c < 4; ++c) {
            u8* a = s + 4 * c;
            if (inv) {
                const u8 u = xt(xt(a[0] ^ a[2])), v = xt(xt(a[1] ^ a[3]));
                a[0] ^= u; a[1] ^= v; a[2] ^= u; a[3] ^= v;
            }
            const u8 t = a[0] ^ a[1] ^ a[2] ^ a[3], a0 = a[0];
            a[0] ^= t ^ xt(a[0] ^ a[1]); a[1] ^= t ^ xt(a[1] ^ a[2]);
            a[2] ^= t ^ xt(a[2] ^ a[3]); a[3] ^= t ^ xt(a[3] ^ a0);
        }
    }
    void enc(const u8* in, u8* out) override {
#ifdef SONATA_AESNI
        if (hw) return aesniBlock(rk, drk, nr, in, out, false);
#endif
        const u8* sb = aesTables().sb;
        u8 s[16], t[16];
        for (int i = 0; i < 16; ++i) s[i] = u8(in[i] ^ rk[0][i]);
        for (int r = 1; r <= nr; ++r) {
            for (int c = 0; c < 4; ++c)
                for (int y = 0; y < 4; ++y) t[4 * c + y] = sb[s[4 * ((c + y) % 4) + y]];
            if (r < nr) mix(t, false);
            for (int i = 0; i < 16; ++i) s[i] = u8(t[i] ^ rk[r][i]);
        }
        std::memcpy(out, s, 16);
    }
    void dec(const u8* in, u8* out) override {
#ifdef SONATA_AESNI
        if (hw) return aesniBlock(rk, drk, nr, in, out, true);
#endif
        const u8* isb = aesTables().isb;
        u8 s[16], t[16];
        for (int i = 0; i < 16; ++i) s[i] = u8(in[i] ^ rk[nr][i]);
        for (int r = nr - 1; r >= 0; --r) {
            for (int c = 0; c < 4; ++c)
                for (int y = 0; y < 4; ++y) t[4 * c + y] = isb[s[4 * ((c + 4 - y) % 4) + y]];
            for (int i = 0; i < 16; ++i) s[i] = u8(t[i] ^ rk[r][i]);
            if (r > 0) mix(s, true);
        }
        std::memcpy(out, s, 16);
    }
};

struct Sm4 : Block {
    u32 rk[32];
    static u32 T(u32 x, bool key) {
        static const u8 S[256] = {
            0xd6, 0x90, 0xe9, 0xfe, 0xcc, 0xe1, 0x3d, 0xb7, 0x16, 0xb6, 0x14, 0xc2, 0x28, 0xfb, 0x2c, 0x05,
            0x2b, 0x67, 0x9a, 0x76, 0x2a, 0xbe, 0x04, 0xc3, 0xaa, 0x44, 0x13, 0x26, 0x49, 0x86, 0x06, 0x99,
            0x9c, 0x42, 0x50, 0xf4, 0x91, 0xef, 0x98, 0x7a, 0x33, 0x54, 0x0b, 0x43, 0xed, 0xcf, 0xac, 0x62,
            0xe4, 0xb3, 0x1c, 0xa9, 0xc9, 0x08, 0xe8, 0x95, 0x80, 0xdf, 0x94, 0xfa, 0x75, 0x8f, 0x3f, 0xa6,
            0x47, 0x07, 0xa7, 0xfc, 0xf3, 0x73, 0x17, 0xba, 0x83, 0x59, 0x3c, 0x19, 0xe6, 0x85, 0x4f, 0xa8,
            0x68, 0x6b, 0x81, 0xb2, 0x71, 0x64, 0xda, 0x8b, 0xf8, 0xeb, 0x0f, 0x4b, 0x70, 0x56, 0x9d, 0x35,
            0x1e, 0x24, 0x0e, 0x5e, 0x63, 0x58, 0xd1, 0xa2, 0x25, 0x22, 0x7c, 0x3b, 0x01, 0x21, 0x78, 0x87,
            0xd4, 0x00, 0x46, 0x57, 0x9f, 0xd3, 0x27, 0x52, 0x4c, 0x36, 0x02, 0xe7, 0xa0, 0xc4, 0xc8, 0x9e,
            0xea, 0xbf, 0x8a, 0xd2, 0x40, 0xc7, 0x38, 0xb5, 0xa3, 0xf7, 0xf2, 0xce, 0xf9, 0x61, 0x15, 0xa1,
            0xe0, 0xae, 0x5d, 0xa4, 0x9b, 0x34, 0x1a, 0x55, 0xad, 0x93, 0x32, 0x30, 0xf5, 0x8c, 0xb1, 0xe3,
            0x1d, 0xf6, 0xe2, 0x2e, 0x82, 0x66, 0xca, 0x60, 0xc0, 0x29, 0x23, 0xab, 0x0d, 0x53, 0x4e, 0x6f,
            0xd5, 0xdb, 0x37, 0x45, 0xde, 0xfd, 0x8e, 0x2f, 0x03, 0xff, 0x6a, 0x72, 0x6d, 0x6c, 0x5b, 0x51,
            0x8d, 0x1b, 0xaf, 0x92, 0xbb, 0xdd, 0xbc, 0x7f, 0x11, 0xd9, 0x5c, 0x41, 0x1f, 0x10, 0x5a, 0xd8,
            0x0a, 0xc1, 0x31, 0x88, 0xa5, 0xcd, 0x7b, 0xbd, 0x2d, 0x74, 0xd0, 0x12, 0xb8, 0xe5, 0xb4, 0xb0,
            0x89, 0x69, 0x97, 0x4a, 0x0c, 0x96, 0x77, 0x7e, 0x65, 0xb9, 0xf1, 0x09, 0xc5, 0x6e, 0xc6, 0x84,
            0x18, 0xf0, 0x7d, 0xec, 0x3a, 0xdc, 0x4d, 0x20, 0x79, 0xee, 0x5f, 0x3e, 0xd7, 0xcb, 0x39, 0x48};
        const u32 b = u32(S[x >> 24]) << 24 | u32(S[(x >> 16) & 255]) << 16 | u32(S[(x >> 8) & 255]) << 8 | S[x & 255];
        return key ? b ^ rol(b, 13) ^ rol(b, 23) : b ^ rol(b, 2) ^ rol(b, 10) ^ rol(b, 18) ^ rol(b, 24);
    }
    explicit Sm4(const u8* key) : Block(16) {
        static const u32 FK[4] = {0xa3b1bac6, 0x56aa3350, 0x677d9197, 0xb27022dc};
        u32 k[36];
        for (int i = 0; i < 4; ++i) k[i] = LD<u32>(key + 4 * i, true) ^ FK[i];
        for (int i = 0; i < 32; ++i) {
            u32 ck = 0;
            for (int j = 0; j < 4; ++j) ck = (ck << 8) | u8((4 * i + j) * 7);
            k[i + 4] = k[i] ^ T(k[i + 1] ^ k[i + 2] ^ k[i + 3] ^ ck, true);
            rk[i] = k[i + 4];
        }
    }
    void run(const u8* in, u8* out, bool dec) const {
        u32 x[36];
        for (int i = 0; i < 4; ++i) x[i] = LD<u32>(in + 4 * i, true);
        for (int i = 0; i < 32; ++i) x[i + 4] = x[i] ^ T(x[i + 1] ^ x[i + 2] ^ x[i + 3] ^ rk[dec ? 31 - i : i], false);
        for (int i = 0; i < 4; ++i) ST<u32>(out + 4 * i, x[35 - i], true);
    }
    void enc(const u8* in, u8* out) override { run(in, out, false); }
    void dec(const u8* in, u8* out) override { run(in, out, true); }
};

struct Xtea : Block {
    u32 k[4];
    explicit Xtea(const u8* key) : Block(8) { for (int i = 0; i < 4; ++i) k[i] = LD<u32>(key + 4 * i, true); }
    void enc(const u8* in, u8* out) override {
        u32 a = LD<u32>(in, true), b = LD<u32>(in + 4, true), s = 0;
        for (int i = 0; i < 32; ++i) {
            a += (((b << 4) ^ (b >> 5)) + b) ^ (s + k[s & 3]);
            s += 0x9E3779B9;
            b += (((a << 4) ^ (a >> 5)) + a) ^ (s + k[(s >> 11) & 3]);
        }
        ST<u32>(out, a, true); ST<u32>(out + 4, b, true);
    }
    void dec(const u8* in, u8* out) override {
        u32 a = LD<u32>(in, true), b = LD<u32>(in + 4, true), s = 0x9E3779B9u * 32;
        for (int i = 0; i < 32; ++i) {
            b -= (((a << 4) ^ (a >> 5)) + a) ^ (s + k[(s >> 11) & 3]);
            s -= 0x9E3779B9;
            a -= (((b << 4) ^ (b >> 5)) + b) ^ (s + k[s & 3]);
        }
        ST<u32>(out, a, true); ST<u32>(out + 4, b, true);
    }
};

struct Speck : Block {  // Speck128/{128,192,256}; block = LE(y) || LE(x)
    u64 k[34];
    int rounds;
    Speck(const u8* key, size_t kl) : Block(16) {
        const int m = int(kl / 8);
        rounds = m + 30;
        u64 l[36] = {};
        k[0] = LD<u64>(key, false);
        for (int i = 0; i < m - 1; ++i) l[i] = LD<u64>(key + 8 * (i + 1), false);
        for (int i = 0; i < rounds - 1; ++i) {
            l[i + m - 1] = (k[i] + ror(l[i], 8)) ^ u64(i);
            k[i + 1] = rol(k[i], 3) ^ l[i + m - 1];
        }
    }
    void enc(const u8* in, u8* out) override {
        u64 y = LD<u64>(in, false), x = LD<u64>(in + 8, false);
        for (int i = 0; i < rounds; ++i) { x = (ror(x, 8) + y) ^ k[i]; y = rol(y, 3) ^ x; }
        ST<u64>(out, y, false); ST<u64>(out + 8, x, false);
    }
    void dec(const u8* in, u8* out) override {
        u64 y = LD<u64>(in, false), x = LD<u64>(in + 8, false);
        for (int i = rounds - 1; i >= 0; --i) { y = ror(y ^ x, 3); x = rol((x ^ k[i]) - y, 8); }
        ST<u64>(out, y, false); ST<u64>(out + 8, x, false);
    }
};

template <class T> BlockPtr mkBlock(const u8* key, size_t kl) {
    if constexpr (std::is_constructible_v<T, const u8*, size_t>) return std::make_unique<T>(key, kl);
    else return std::make_unique<T>(key);
}

struct BlockInfo {
    const char* name;
    size_t keyLen, bs;
    BlockPtr (*make)(const u8*, size_t);
};
const BlockInfo kBlocks[] = {
    {"aes-128", 16, 16, mkBlock<Aes>},      {"aes-192", 24, 16, mkBlock<Aes>},
    {"aes-256", 32, 16, mkBlock<Aes>},      {"sm4", 16, 16, mkBlock<Sm4>},
    {"xtea", 16, 8, mkBlock<Xtea>},         {"speck-128", 16, 16, mkBlock<Speck>},
    {"speck-192", 24, 16, mkBlock<Speck>},  {"speck-256", 32, 16, mkBlock<Speck>},
};
enum Mode { ECB, CBC, CTR, CFB, OFB, GCM };
constexpr const char* kModes[] = {"ecb", "cbc", "ctr", "cfb", "ofb", "gcm"};

// ECB/CBC (PKCS#7) and CTR/CFB/OFB over any block cipher. `iv` is one block.
bool blockCrypt(Block& c, int mode, bool enc, const u8* iv, SV in, Bytes& out) {
    const size_t bs = c.bs;
    if (mode == ECB || mode == CBC) {
        Bytes data(in);
        if (enc) data.append(bs - in.size() % bs, char(bs - in.size() % bs));
        else if (in.empty() || in.size() % bs) return false;
        out.assign(data.size(), '\0');
        u8 prev[16] = {}, t[16];
        if (mode == CBC) std::memcpy(prev, iv, bs);
        for (size_t o = 0; o < data.size(); o += bs) {
            const u8* src = cu8(data) + o;
            u8* dst = mu8(out) + o;
            if (enc) {
                for (size_t i = 0; i < bs; ++i) t[i] = u8(src[i] ^ prev[i]);
                c.enc(t, dst);
                if (mode == CBC) std::memcpy(prev, dst, bs);
            } else {
                c.dec(src, t);
                for (size_t i = 0; i < bs; ++i) dst[i] = u8(t[i] ^ prev[i]);
                if (mode == CBC) std::memcpy(prev, src, bs);
            }
        }
        if (!enc) {
            const size_t pad = u8(out.back());
            if (pad == 0 || pad > bs) return false;
            for (size_t i = out.size() - pad; i < out.size(); ++i) if (u8(out[i]) != pad) return false;
            out.resize(out.size() - pad);
        }
        return true;
    }
    out.assign(in.size(), '\0');
    u8 reg[16], ks[16];
    std::memcpy(reg, iv, bs);
    size_t pos = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        if (pos == 0) {
            c.enc(reg, ks);
            if (mode == CTR) { for (size_t j = bs; j-- > 0 && ++reg[j] == 0;) {} }  // big-endian increment
            else if (mode == OFB) std::memcpy(reg, ks, bs);
        }
        const u8 o = u8(u8(in[i]) ^ ks[pos]);
        out[i] = char(o);
        if (mode == CFB) reg[pos] = enc ? o : u8(in[i]);
        pos = (pos + 1) % bs;
    }
    return true;
}

Bytes gcmTag(Block& c, const u8* j0, SV aad, SV ct) {
    u8 zero[16] = {}, hb[16];
    c.enc(zero, hb);
    const u64 h0 = LD<u64>(hb, true), h1 = LD<u64>(hb + 8, true);
    u64 y0 = 0, y1 = 0;
    auto blk = [&](const u8* b) {  // y = (y ^ b) * H in GF(2^128)
        y0 ^= LD<u64>(b, true);
        y1 ^= LD<u64>(b + 8, true);
        u64 z0 = 0, z1 = 0, v0 = h0, v1 = h1;
        for (int i = 0; i < 128; ++i) {
            if (i < 64 ? (y0 >> (63 - i)) & 1 : (y1 >> (127 - i)) & 1) { z0 ^= v0; z1 ^= v1; }
            const u64 lsb = v1 & 1;
            v1 = (v1 >> 1) | (v0 << 63);
            v0 >>= 1;
            if (lsb) v0 ^= 0xe100000000000000ULL;
        }
        y0 = z0; y1 = z1;
    };
    auto feed = [&](SV d) {
        for (size_t o = 0; o < d.size(); o += 16) {
            u8 b[16] = {};
            std::memcpy(b, d.data() + o, std::min<size_t>(16, d.size() - o));
            blk(b);
        }
    };
    feed(aad);
    feed(ct);
    u8 lb[16], ek[16], tag[16];
    ST<u64>(lb, u64(aad.size()) * 8, true);
    ST<u64>(lb + 8, u64(ct.size()) * 8, true);
    blk(lb);
    c.enc(j0, ek);
    ST<u64>(tag, y0, true);
    ST<u64>(tag + 8, y1, true);
    for (int i = 0; i < 16; ++i) tag[i] ^= ek[i];
    return bytesOf(tag, 16);
}

///////////// cipher dispatch

enum Kind { K_BLOCK, K_STREAM, K_AEAD };
struct Plan {
    int kind = K_BLOCK;
    const BlockInfo* bi = nullptr;
    int mode = 0, sid = 0;
    size_t keyLen = 0, ivLen = 0;
};
struct StreamInfo {
    const char* name;
    size_t iv;
    int sid;
    bool aead;
};
constexpr StreamInfo kStreams[] = {
    {"chacha20", 12, 0, false},          {"xchacha20", 24, 1, false},
    {"salsa20", 8, 2, false},            {"xsalsa20", 24, 3, false},
    {"chacha20-poly1305", 12, 0, true},  {"xchacha20-poly1305", 24, 1, true},
};

bool resolve(SV name, Plan& p) {
    for (const auto& s : kStreams)
        if (name == s.name) { p = Plan{s.aead ? K_AEAD : K_STREAM, nullptr, 0, s.sid, 32, s.iv}; return true; }
    const size_t dash = name.rfind('-');
    if (dash == SV::npos) return false;
    const SV cn = name.substr(0, dash), mn = name.substr(dash + 1);
    const BlockInfo* bi = nullptr;
    int mode = -1;
    for (const auto& b : kBlocks) if (cn == b.name) bi = &b;
    for (int i = 0; i < 6; ++i) if (mn == kModes[i]) mode = i;
    if (!bi || mode < 0 || (mode == GCM && bi->bs != 16)) return false;
    p = Plan{K_BLOCK, bi, mode, 0, bi->keyLen, mode == ECB ? 0 : mode == GCM ? 12 : bi->bs};
    return true;
}

// Encrypts/decrypts `in` (decrypt input = ciphertext || tag, without the IV).
// Returns nullptr on success or an error message; never raises Lua errors.
const char* process(const Plan& p, bool enc, SV key, SV iv, SV aad, SV in, Bytes& out) {
    const u8 *k = cu8(key), *n = cu8(iv);
    if (p.kind == K_STREAM) { out = streamXor(p.sid, k, n, 0, in); return nullptr; }
    SV ct = in;
    if (!enc && p.kind != K_STREAM && (p.kind == K_AEAD || p.mode == GCM)) {
        if (in.size() < 16) return "ciphertext too short";
        ct = in.substr(0, in.size() - 16);
    }
    if (p.kind == K_AEAD) {
        const Bytes otk = streamXor(p.sid, k, n, 0, Bytes(32, '\0'));
        if (!enc && !ctEq(aeadTag(otk, aad, ct), in.substr(in.size() - 16))) return "authentication failed";
        out = streamXor(p.sid, k, n, 1, ct);
        if (enc) out += aeadTag(otk, aad, out);
        return nullptr;
    }
    const BlockPtr c = p.bi->make(k, key.size());
    if (p.mode == GCM) {
        u8 j[16];
        std::memcpy(j, n, 12);
        ST<u32>(j + 12, 1, true);
        if (!enc && !ctEq(gcmTag(*c, j, aad, ct), in.substr(in.size() - 16))) return "authentication failed";
        ST<u32>(j + 12, 2, true);
        blockCrypt(*c, CTR, true, j, ct, out);
        ST<u32>(j + 12, 1, true);
        if (enc) out += gcmTag(*c, j, aad, out);
        return nullptr;
    }
    return blockCrypt(*c, p.mode, enc, n, in, out) ? nullptr : "invalid padding or data length";
}

///////////// scrypt

void salsa8(u32* b) {
    u32 x[16];
    std::copy(b, b + 16, x);
    salsaRounds(x, 8);
    for (int i = 0; i < 16; ++i) b[i] += x[i];
}

void blockMix(u32* b, u32* y, u32 r) {
    u32 x[16];
    std::copy(b + (2 * r - 1) * 16, b + 2 * r * 16, x);
    for (u32 i = 0; i < 2 * r; ++i) {
        for (int j = 0; j < 16; ++j) x[j] ^= b[i * 16 + j];
        salsa8(x);
        std::copy(x, x + 16, y + ((i & 1) ? r + i / 2 : i / 2) * 16);
    }
    std::copy(y, y + 32 * r, b);
}

Bytes scrypt(SV pw, SV salt, u32 N, u32 r, u32 p, size_t len) {
    const HashInfo& h = *findHash("sha256");
    Bytes B = pbkdf2(h, pw, salt, 1, size_t(p) * 128 * r);
    std::vector<u32> X(32 * r), Y(32 * r), V(size_t(32) * r * N);
    for (u32 k = 0; k < p; ++k) {
        u8* blk = mu8(B) + size_t(k) * 128 * r;
        for (u32 i = 0; i < 32 * r; ++i) X[i] = LD<u32>(blk + 4 * i, false);
        for (u32 i = 0; i < N; ++i) {
            std::copy(X.begin(), X.end(), V.begin() + size_t(i) * 32 * r);
            blockMix(X.data(), Y.data(), r);
        }
        for (u32 i = 0; i < N; ++i) {
            const size_t j = X[(2 * r - 1) * 16] & (N - 1);
            for (u32 m = 0; m < 32 * r; ++m) X[m] ^= V[j * 32 * r + m];
            blockMix(X.data(), Y.data(), r);
        }
        for (u32 i = 0; i < 32 * r; ++i) ST<u32>(blk + 4 * i, X[i], false);
    }
    return pbkdf2(h, pw, B, 1, len);
}

///////////// Curve25519 / Ed25519
// Field and curve arithmetic after TweetNaCl (16 x 16-bit limbs), constant-time.

using gf = i64[16];
#define FOR(i, n) for (int i = 0; i < (n); ++i)
const gf gf0 = {0}, gf1 = {1}, k121665 = {0xDB41, 1};
const gf kD = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
const gf kD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
const gf kX = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
const gf kY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
const gf kI = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

void car(i64* o) {
    FOR(i, 16) {
        o[i] += 65536;
        const i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}
void sel(i64* p, i64* q, i64 b) {
    const i64 c = ~(b - 1);
    FOR(i, 16) { const i64 t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
void pack25519(u8* o, const i64* n) {
    i64 m[16], t[16];
    FOR(i, 16) t[i] = n[i];
    car(t); car(t); car(t);
    FOR(j, 2) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const i64 b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel(t, m, 1 - b);
    }
    FOR(i, 16) { o[2 * i] = u8(t[i] & 0xff); o[2 * i + 1] = u8(t[i] >> 8); }
}
void unpack25519(i64* o, const u8* n) {
    FOR(i, 16) o[i] = n[2 * i] + (i64(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}
bool neq(const i64* a, const i64* b) { u8 c[32], d[32]; pack25519(c, a); pack25519(d, b); return std::memcmp(c, d, 32) != 0; }
int par(const i64* a) { u8 d[32]; pack25519(d, a); return d[0] & 1; }
void fa(i64* o, const i64* a, const i64* b) { FOR(i, 16) o[i] = a[i] + b[i]; }
void fz(i64* o, const i64* a, const i64* b) { FOR(i, 16) o[i] = a[i] - b[i]; }
void fm(i64* o, const i64* a, const i64* b) {
    i64 t[31] = {};
    FOR(i, 16) FOR(j, 16) t[i + j] += a[i] * b[j];
    FOR(i, 15) t[i] += 38 * t[i + 16];
    FOR(i, 16) o[i] = t[i];
    car(o); car(o);
}
void fs(i64* o, const i64* a) { fm(o, a, a); }
void inv25519(i64* o, const i64* in) {
    gf c;
    FOR(a, 16) c[a] = in[a];
    for (int a = 253; a >= 0; a--) { fs(c, c); if (a != 2 && a != 4) fm(c, c, in); }
    FOR(a, 16) o[a] = c[a];
}
void pow2523(i64* o, const i64* in) {
    gf c;
    FOR(a, 16) c[a] = in[a];
    for (int a = 250; a >= 0; a--) { fs(c, c); if (a != 1) fm(c, c, in); }
    FOR(a, 16) o[a] = c[a];
}

void x25519(u8* q, const u8* n, const u8* p) {  // X25519 scalar multiplication (clamps n)
    u8 z[32];
    i64 x[80];
    gf a, b, c, d, e, f;
    FOR(i, 31) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, p);
    FOR(i, 16) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        const i64 r = (z[i >> 3] >> (i & 7)) & 1;
        sel(a, b, r); sel(c, d, r);
        fa(e, a, c); fz(a, a, c); fa(c, b, d); fz(b, b, d); fs(d, e); fs(f, a); fm(a, c, a); fm(c, b, e);
        fa(e, a, c); fz(a, a, c); fs(b, a); fz(c, d, f); fm(a, c, k121665); fa(a, a, d); fm(c, c, a);
        fm(a, d, f); fm(d, b, x); fs(b, e);
        sel(a, b, r); sel(c, d, r);
    }
    FOR(i, 16) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    inv25519(x + 32, x + 32);
    fm(x + 16, x + 16, x + 32);
    pack25519(q, x + 16);
}

void edAdd(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    fz(a, p[1], p[0]); fz(t, q[1], q[0]); fm(a, a, t);
    fa(b, p[0], p[1]); fa(t, q[0], q[1]); fm(b, b, t);
    fm(c, p[3], q[3]); fm(c, c, kD2);
    fm(d, p[2], q[2]); fa(d, d, d);
    fz(e, b, a); fz(f, d, c); fa(g, d, c); fa(h, b, a);
    fm(p[0], e, f); fm(p[1], h, g); fm(p[2], g, f); fm(p[3], e, h);
}
void edPack(u8* r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    fm(tx, p[0], zi); fm(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= u8(par(tx) << 7);
}
void edMul(gf p[4], gf q[4], const u8* s) {
    FOR(i, 16) { p[0][i] = 0; p[1][i] = gf1[i]; p[2][i] = gf1[i]; p[3][i] = 0; }
    for (int i = 255; i >= 0; --i) {
        const i64 b = (s[i / 8] >> (i & 7)) & 1;
        FOR(m, 4) sel(p[m], q[m], b);
        edAdd(q, p);
        edAdd(p, p);
        FOR(m, 4) sel(p[m], q[m], b);
    }
}
void edBase(gf p[4], const u8* s) {
    gf q[4];
    FOR(i, 16) { q[0][i] = kX[i]; q[1][i] = kY[i]; q[2][i] = gf1[i]; }
    fm(q[3], kX, kY);
    edMul(p, q, s);
}
const i64 kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};
void modL(u8* r, i64 x[64]) {
    i64 carry;
    int j;
    for (int i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * kL[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    FOR(k, 32) { x[k] += carry - (x[31] >> 4) * kL[k]; carry = x[k] >> 8; x[k] &= 255; }
    FOR(k, 32) x[k] -= carry * kL[k];
    FOR(i, 32) { x[i + 1] += x[i] >> 8; r[i] = u8(x[i] & 255); }
}
void reduce64(u8* r) {  // r[64] = r mod L
    i64 x[64];
    FOR(i, 64) x[i] = r[i];
    std::memset(r, 0, 64);
    modL(r, x);
}
bool edUnpackNeg(gf r[4], const u8* p) {
    gf t, chk, num, den, den2, den4, den6;
    FOR(i, 16) r[2][i] = gf1[i];
    unpack25519(r[1], p);
    fs(num, r[1]); fm(den, num, kD); fz(num, num, r[2]); fa(den, r[2], den);
    fs(den2, den); fs(den4, den2); fm(den6, den4, den2); fm(t, den6, num); fm(t, t, den);
    pow2523(t, t); fm(t, t, num); fm(t, t, den); fm(t, t, den); fm(r[0], t, den);
    fs(chk, r[0]); fm(chk, chk, den);
    if (neq(chk, num)) fm(r[0], r[0], kI);
    fs(chk, r[0]); fm(chk, chk, den);
    if (neq(chk, num)) return false;
    if (par(r[0]) == (p[31] >> 7)) fz(r[0], gf0, r[0]);
    fm(r[3], r[0], r[1]);
    return true;
}

Bytes sha512(std::initializer_list<SV> parts) { return hashOf(*findHash("sha512"), parts); }

void edExpand(SV seed, u8* d) {  // d[64]: clamped secret scalar || prefix
    std::memcpy(d, sha512({seed}).data(), 64);
    d[0] &= 248; d[31] &= 127; d[31] |= 64;
}
Bytes edPublic(SV seed) {
    u8 d[64], pk[32];
    gf p[4];
    edExpand(seed, d);
    edBase(p, d);
    edPack(pk, p);
    return bytesOf(pk, 32);
}
Bytes edSign(SV seed, SV msg) {
    u8 d[64], r[64], h[64], sig[64];
    gf p[4];
    edExpand(seed, d);
    const Bytes pk = edPublic(seed);
    std::memcpy(r, sha512({viewOf(d + 32, 32), msg}).data(), 64);
    reduce64(r);
    edBase(p, r);
    edPack(sig, p);
    std::memcpy(h, sha512({viewOf(sig, 32), pk, msg}).data(), 64);
    reduce64(h);
    i64 x[64] = {};
    FOR(i, 32) x[i] = r[i];
    FOR(i, 32) FOR(j, 32) x[i + j] += i64(h[i]) * d[j];
    modL(sig + 32, x);
    return bytesOf(sig, 64);
}
bool edVerify(SV pk, SV msg, SV sig) {
    u8 h[64], t[32];
    gf p[4], q[4];
    if (pk.size() != 32 || sig.size() != 64 || !edUnpackNeg(q, cu8(pk))) return false;
    std::memcpy(h, sha512({sig.substr(0, 32), pk, msg}).data(), 64);
    reduce64(h);
    edMul(p, q, h);
    edBase(q, cu8(sig) + 32);
    edAdd(p, q);
    edPack(t, p);
    return ctEq(viewOf(t, 32), sig.substr(0, 32));
}
#undef FOR

Bytes x25519Base(SV priv) {
    u8 base[32] = {9}, q[32];
    x25519(q, cu8(priv), base);
    return bytesOf(q, 32);
}

///////////// Lua helpers
// Rule of thumb (see example.cpp): validate every argument first, then build C++
// objects. Failures after that point are returned as `nil, message`, never raised.

constexpr const char* kRngError = "system random source unavailable";

SV checkSV(lua_State* L, int idx) {
    size_t n = 0;
    const char* s = luaL_checklstring(L, idx, &n);
    return SV(s, n);
}
SV optSV(lua_State* L, int idx) {
    size_t n = 0;
    const char* s = luaL_optlstring(L, idx, "", &n);
    return SV(s, n);
}
int checkLen(lua_State* L, int idx, int maxLen = 1 << 20) {
    const int n = luaL_checkinteger(L, idx);
    luaL_argcheck(L, n >= 0 && n <= maxLen, idx, "length out of range");
    return n;
}
const HashInfo* checkHash(lua_State* L, int idx) {
    const HashInfo* h = findHash(luaL_checkstring(L, idx));
    luaL_argcheck(L, h != nullptr, idx, "unknown hash algorithm (see crypto.hashes)");
    return h;
}
const Codec* optCodec(lua_State* L, int idx) {
    const char* n = luaL_optstring(L, idx, nullptr);
    if (!n) return nullptr;
    const Codec* c = findCodec(n);
    luaL_argcheck(L, c != nullptr, idx, "unknown encoding (hex, base64, base64url, base32)");
    return c;
}
SV checkKey32(lua_State* L, int idx, const char* what) {
    const SV k = checkSV(L, idx);
    luaL_argcheck(L, k.size() == 32, idx, what);
    return k;
}
const char* strField(lua_State* L, int t, const char* f, size_t* len) {  // value stays on the stack
    lua_getfield(L, t, f);
    if (lua_isnil(L, -1)) return nullptr;
    if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "option '%s' must be a string", f);
    return lua_tolstring(L, -1, len);
}
double numField(lua_State* L, int t, const char* f, double def) {
    lua_getfield(L, t, f);
    if (lua_isnil(L, -1)) return def;
    if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "option '%s' must be a number", f);
    return lua_tonumber(L, -1);
}
int pushOut(lua_State* L, const Bytes& b, const Codec* c) {
    if (c) {
        const Bytes e = encode(*c, b);
        lua_pushlstring(L, e.data(), e.size());
    } else {
        lua_pushlstring(L, b.data(), b.size());
    }
    return 1;
}
int pushErr(lua_State* L, const char* msg) {
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}

///////////// Lua bindings

// crypto.hash(algo: string, data: string, enc: string?): string
int cryptoHash(lua_State* L) {
    const HashInfo* h = checkHash(L, 1);
    const SV data = checkSV(L, 2);
    const Codec* c = optCodec(L, 3);
    return pushOut(L, hashOf(*h, {data}), c);
}

// crypto.hmac(algo: string, key: string, data: string, enc: string?): string
int cryptoHmac(lua_State* L) {
    const HashInfo* h = checkHash(L, 1);
    const SV key = checkSV(L, 2), data = checkSV(L, 3);
    const Codec* c = optCodec(L, 4);
    return pushOut(L, hmac(*h, key, data), c);
}

// crypto.pbkdf2(algo, password, salt, iterations: number, length: number, enc: string?): string
int cryptoPbkdf2(lua_State* L) {
    const HashInfo* h = checkHash(L, 1);
    const SV pw = checkSV(L, 2), salt = checkSV(L, 3);
    const int iters = luaL_checkinteger(L, 4);
    luaL_argcheck(L, iters >= 1, 4, "iterations must be positive");
    const int len = checkLen(L, 5, 1 << 16);
    const Codec* c = optCodec(L, 6);
    return pushOut(L, pbkdf2(*h, pw, salt, u32(iters), size_t(len)), c);
}

// crypto.pbkdf2Timed(algo, password, salt, ms: number, length: number, enc: string?): (string, number)
int cryptoPbkdf2Timed(lua_State* L) {
    const HashInfo* h = checkHash(L, 1);
    const SV pw = checkSV(L, 2), salt = checkSV(L, 3);
    const double ms = luaL_checknumber(L, 4);
    luaL_argcheck(L, ms >= 1 && ms <= 60000, 4, "ms must be between 1 and 60000");
    const int len = checkLen(L, 5, 1 << 16);
    const Codec* c = optCodec(L, 6);
    const u32 iters = calibrate(*h, ms, size_t(len));
    pushOut(L, pbkdf2(*h, pw, salt, iters, size_t(len)), c);
    lua_pushnumber(L, iters);
    return 2;
}

// crypto.hkdf(algo, ikm, salt: string?, info: string?, length: number, enc: string?): string
int cryptoHkdf(lua_State* L) {
    const HashInfo* h = checkHash(L, 1);
    const SV ikm = checkSV(L, 2), salt = optSV(L, 3), info = optSV(L, 4);
    const int len = checkLen(L, 5, 255 * int(h->digest));
    const Codec* c = optCodec(L, 6);
    return pushOut(L, hkdf(*h, ikm, salt, info, size_t(len)), c);
}

// crypto.scrypt(password, salt, N, r, p, length, enc: string?): string
int cryptoScrypt(lua_State* L) {
    const SV pw = checkSV(L, 1), salt = checkSV(L, 2);
    const int N = luaL_checkinteger(L, 3), r = luaL_checkinteger(L, 4), p = luaL_checkinteger(L, 5);
    luaL_argcheck(L, N >= 2 && (N & (N - 1)) == 0, 3, "N must be a power of two >= 2");
    luaL_argcheck(L, r >= 1 && p >= 1 && p <= 1024 && r <= 256, 4, "r in 1..256 and p in 1..1024");
    luaL_argcheck(L, i64(N) * r <= (1 << 20), 3, "N * r too large (memory limit 128 MiB)");
    const int len = checkLen(L, 6, 1 << 16);
    const Codec* c = optCodec(L, 7);
    return pushOut(L, scrypt(pw, salt, u32(N), u32(r), u32(p), size_t(len)), c);
}

int cipherOp(lua_State* L, bool enc) {
    const char* algo = luaL_checkstring(L, 1);
    const SV key = checkSV(L, 2);
    SV data = checkSV(L, 3), aad, iv;
    bool hasIv = false;
    if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TTABLE);
        size_t n = 0;
        if (const char* s = strField(L, 4, "aad", &n)) aad = SV(s, n);
        if (const char* s = strField(L, 4, "iv", &n)) { iv = SV(s, n); hasIv = true; }
    }
    Plan p;
    luaL_argcheck(L, resolve(algo, p), 1, "unknown cipher (see crypto.ciphers)");
    luaL_argcheck(L, key.size() == p.keyLen, 2, "wrong key length for this cipher");
    if (hasIv && iv.size() != p.ivLen) luaL_error(L, "options.iv must be %d bytes for this cipher", int(p.ivLen));

    Bytes out, ivb;
    const char* err = nullptr;
    if (enc) {
        if (hasIv) ivb.assign(iv);
        else if (!nonceFill(ivb, p.ivLen)) return pushErr(L, kRngError);
        err = process(p, true, key, ivb, aad, data, out);
        if (!err && !hasIv) out.insert(0, ivb);
    } else {
        if (hasIv) ivb.assign(iv);
        else if (data.size() < p.ivLen) err = "ciphertext too short";
        else { ivb.assign(data.substr(0, p.ivLen)); data.remove_prefix(p.ivLen); }
        if (!err) err = process(p, false, key, ivb, aad, data, out);
    }
    return err ? pushErr(L, err) : pushOut(L, out, nullptr);
}

// crypto.encrypt(algo: string, key: string, data: string, opts: { aad: string?, iv: string? }?): string | (nil, string)
int cryptoEncrypt(lua_State* L) { return cipherOp(L, true); }
// crypto.decrypt(algo: string, key: string, data: string, opts: { aad: string?, iv: string? }?): string | (nil, string)
int cryptoDecrypt(lua_State* L) { return cipherOp(L, false); }

// crypto.randomBytes(n: number, enc: string?): string
int cryptoRandomBytes(lua_State* L) {
    const int n = checkLen(L, 1);
    const Codec* c = optCodec(L, 2);
    Bytes b;
    if (!randomFill(b, size_t(n))) return pushErr(L, kRngError);
    return pushOut(L, b, c);
}

// crypto.randomInt(min: number, max: number): number   (inclusive, unbiased)
int cryptoRandomInt(lua_State* L) {
    const int lo = luaL_checkinteger(L, 1), hi = luaL_checkinteger(L, 2);
    luaL_argcheck(L, lo <= hi, 2, "max must be >= min");
    const u64 range = u64(i64(hi) - lo) + 1;
    const u64 limit = ~u64(0) - (~u64(0) % range);  // rejection sampling
    u64 v;
    do {
        if (!osRandom(&v, sizeof v)) return pushErr(L, kRngError);
    } while (v >= limit);
    lua_pushinteger(L, int(i64(lo) + i64(v % range)));
    return 1;
}

bool parseNamespace(SV s, u8* out) {
    static const u8 base[16] = {0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
    static const char* names[] = {"dns", "url", "oid", "x500"};
    static const u8 low[] = {0x10, 0x11, 0x12, 0x14};
    for (int i = 0; i < 4; ++i)
        if (s == names[i]) { std::memcpy(out, base, 16); out[3] = low[i]; return true; }
    if (s.size() != 36) return false;
    int nib = 0;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; continue; }
        const char* q = std::strchr("0123456789abcdef", std::tolower((unsigned char)s[i]));
        if (!q || !s[i]) return false;
        const u8 v = u8(q - "0123456789abcdef");
        out[nib / 2] = (nib % 2) ? u8(out[nib / 2] | v) : u8(v << 4);
        ++nib;
    }
    return true;
}

// crypto.uuid(version: number?, namespace: string?, name: string?): string
int cryptoUuid(lua_State* L) {
    const int ver = luaL_optinteger(L, 1, 4);
    luaL_argcheck(L, ver == 1 || ver == 3 || ver == 4 || ver == 5 || ver == 7, 1, "version must be 1, 3, 4, 5 or 7");
    const bool named = ver == 3 || ver == 5;
    u8 ns[16] = {}, b[16];
    SV name;
    if (named) {
        luaL_argcheck(L, parseNamespace(checkSV(L, 2), ns), 2, "namespace must be a UUID or dns/url/oid/x500");
        name = checkSV(L, 3);
    }
    if (named) {
        const Bytes d = hashOf(*findHash(ver == 3 ? "md5" : "sha1"), {viewOf(ns, 16), name});
        std::memcpy(b, d.data(), 16);
    } else {
        Bytes r;
        if (!randomFill(r, 16)) return pushErr(L, kRngError);
        std::memcpy(b, r.data(), 16);
        const u64 ns1 = nowNs();
        if (ver == 7) {  // 48-bit unix milliseconds, then random
            const u64 ms = ns1 / 1000000;
            for (int i = 0; i < 6; ++i) b[i] = u8(ms >> (40 - 8 * i));
        } else if (ver == 1) {  // 60-bit count of 100ns ticks since 1582-10-15
            const u64 t = ns1 / 100 + 0x01B21DD213814000ULL;
            for (int i = 0; i < 4; ++i) b[i] = u8(t >> (24 - 8 * i));
            b[4] = u8(t >> 40); b[5] = u8(t >> 32);
            b[6] = u8((t >> 56) & 0x0f); b[7] = u8(t >> 48);
            b[10] |= 1;  // random node id: multicast bit set, per RFC 4122
        }
    }
    b[6] = u8((b[6] & 0x0f) | (ver << 4));
    b[8] = u8((b[8] & 0x3f) | 0x80);
    static const char* hex = "0123456789abcdef";
    Bytes s;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        s += hex[b[i] >> 4];
        s += hex[b[i] & 15];
    }
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

// crypto.ulid(): string   (48-bit ms timestamp + 80 random bits, Crockford base32, sortable)
int cryptoUlid(lua_State* L) {
    Bytes r;
    if (!randomFill(r, 16)) return pushErr(L, kRngError);
    u8 b[16];
    std::memcpy(b, r.data(), 16);
    const u64 ms = nowNs() / 1000000;
    for (int i = 0; i < 6; ++i) b[i] = u8(ms >> (40 - 8 * i));
    char s[26];
    for (int i = 0; i < 26; ++i) {  // 26 chars x 5 bits = 130 bits: two leading zero bits
        int v = 0;
        for (int k = 0; k < 5; ++k) {
            const int bit = 5 * i - 2 + k;
            v = (v << 1) | (bit < 0 ? 0 : (b[bit / 8] >> (7 - bit % 8)) & 1);
        }
        s[i] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"[v];
    }
    lua_pushlstring(L, s, 26);
    return 1;
}

// crypto.totp(secret: string, opts: { digits: number?, period: number?, hash: string?, time: number? }?): string
int cryptoTotp(lua_State* L) {
    const SV secret = checkSV(L, 1);
    double digits = 6, period = 30, now = double(nowNs()) / 1000000000.0;
    const char* hashName = "sha1";
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        digits = numField(L, 2, "digits", digits);
        period = numField(L, 2, "period", period);
        now = numField(L, 2, "time", now);
        size_t n = 0;
        if (const char* s = strField(L, 2, "hash", &n)) hashName = s;
    }
    const HashInfo* h = findHash(hashName);
    luaL_argcheck(L, h != nullptr, 2, "unknown hash algorithm");
    luaL_argcheck(L, digits >= 1 && digits <= 9 && period >= 1 && now >= 0, 2, "digits must be 1-9, period >= 1, time >= 0");
    u8 msg[8];
    ST<u64>(msg, u64(now / period), true);
    const Bytes mac = hmac(*h, secret, viewOf(msg, 8));
    const size_t off = u8(mac.back()) & 15;
    u64 mod = 1;
    for (int i = 0; i < int(digits); ++i) mod *= 10;
    const u64 code = (LD<u32>(cu8(mac) + off, true) & 0x7fffffffu) % mod;
    char fmt[16], out[16];
    std::snprintf(fmt, sizeof fmt, "%%0%dllu", int(digits));
    std::snprintf(out, sizeof out, fmt, static_cast<unsigned long long>(code));
    lua_pushstring(L, out);
    return 1;
}

// crypto.hashPassword(password: string, ms: number?): string   (PBKDF2-SHA256, cost tuned to ~ms, default 100)
int cryptoHashPassword(lua_State* L) {
    const SV pw = checkSV(L, 1);
    const double ms = luaL_optnumber(L, 2, 100);
    luaL_argcheck(L, ms >= 1 && ms <= 60000, 2, "ms must be between 1 and 60000");
    const HashInfo& h = *findHash("sha256");
    const Codec& b64 = *findCodec("base64");
    Bytes salt;
    if (!randomFill(salt, 16)) return pushErr(L, kRngError);
    const u32 iters = calibrate(h, ms, h.digest);
    const Bytes out = "$pbkdf2-sha256$" + std::to_string(iters) + "$" + encode(b64, salt) + "$" +
                      encode(b64, pbkdf2(h, pw, salt, iters, h.digest));
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// crypto.verifyPassword(password: string, encoded: string): boolean
int cryptoVerifyPassword(lua_State* L) {
    const SV pw = checkSV(L, 1), enc = checkSV(L, 2);
    std::vector<SV> parts;
    for (size_t s = 0;;) {
        const size_t e = enc.find('$', s);
        parts.push_back(enc.substr(s, e == SV::npos ? SV::npos : e - s));
        if (e == SV::npos) break;
        s = e + 1;
    }
    bool ok = false;
    if (parts.size() == 5 && parts[0].empty() && parts[1].substr(0, 7) == "pbkdf2-") {
        const HashInfo* h = findHash(parts[1].substr(7));
        const unsigned long iters = std::strtoul(Bytes(parts[2]).c_str(), nullptr, 10);
        Bytes salt, key;
        const Codec& b64 = *findCodec("base64");
        if (h && iters >= 1 && iters <= 100000000UL && decode(b64, parts[3], salt) && decode(b64, parts[4], key) &&
            !key.empty() && key.size() <= 1024)
            ok = ctEq(pbkdf2(*h, pw, salt, u32(iters), key.size()), key);
    }
    lua_pushboolean(L, ok);
    return 1;
}

// crypto.encode(format: string, data: string): string
int cryptoEncode(lua_State* L) {
    const Codec* c = findCodec(luaL_checkstring(L, 1));
    luaL_argcheck(L, c != nullptr, 1, "unknown encoding (hex, base64, base64url, base32)");
    const SV data = checkSV(L, 2);
    return pushOut(L, bytesOf(data.data(), data.size()), c);
}

// crypto.decode(format: string, text: string): string?   (nil if the text is invalid)
int cryptoDecode(lua_State* L) {
    const Codec* c = findCodec(luaL_checkstring(L, 1));
    luaL_argcheck(L, c != nullptr, 1, "unknown encoding (hex, base64, base64url, base32)");
    const SV text = checkSV(L, 2);
    Bytes out;
    if (!decode(*c, text, out)) { lua_pushnil(L); return 1; }
    return pushOut(L, out, nullptr);
}

// crypto.equals(a: string, b: string): boolean   (constant time for equal lengths)
int cryptoEquals(lua_State* L) {
    const SV a = checkSV(L, 1), b = checkSV(L, 2);
    lua_pushboolean(L, ctEq(a, b));
    return 1;
}

// crypto.keypair(kind: "ed25519" | "x25519"): (string, string)   -> public, private
int cryptoKeypair(lua_State* L) {
    const SV kind = luaL_checkstring(L, 1);
    luaL_argcheck(L, kind == "ed25519" || kind == "x25519", 1, "kind must be \"ed25519\" or \"x25519\"");
    Bytes sk;
    if (!randomFill(sk, 32)) return pushErr(L, kRngError);
    const Bytes pk = kind == "ed25519" ? edPublic(sk) : x25519Base(sk);
    lua_pushlstring(L, pk.data(), pk.size());
    lua_pushlstring(L, sk.data(), sk.size());
    return 2;
}

// crypto.sign(privateKey: string, message: string, enc: string?): string
int cryptoSign(lua_State* L) {
    const SV sk = checkKey32(L, 1, "private key must be 32 bytes"), msg = checkSV(L, 2);
    const Codec* c = optCodec(L, 3);
    return pushOut(L, edSign(sk, msg), c);
}

// crypto.verify(publicKey: string, message: string, signature: string): boolean
int cryptoVerify(lua_State* L) {
    const SV pk = checkKey32(L, 1, "public key must be 32 bytes"), msg = checkSV(L, 2), sig = checkSV(L, 3);
    lua_pushboolean(L, edVerify(pk, msg, sig));
    return 1;
}

// crypto.sharedSecret(privateKey: string, peerPublicKey: string, enc: string?): string | (nil, string)
int cryptoSharedSecret(lua_State* L) {
    const SV sk = checkKey32(L, 1, "private key must be 32 bytes"), pk = checkKey32(L, 2, "public key must be 32 bytes");
    const Codec* c = optCodec(L, 3);
    u8 q[32];
    x25519(q, cu8(sk), cu8(pk));
    u8 acc = 0;
    for (u8 v : q) acc |= v;
    if (!acc) return pushErr(L, "invalid public key");
    return pushOut(L, bytesOf(q, 32), c);
}

Bytes sealKey(SV shared, SV epk, SV rpk) {
    return hkdf(*findHash("sha256"), shared, Bytes(epk) + Bytes(rpk), "sonata-seal-v1", 32);
}

// crypto.seal(recipientPublicKey: string, message: string): string | (nil, string)
// Output: ephemeralPublic(32) || nonce(24) || ciphertext || tag(16)
int cryptoSeal(lua_State* L) {
    const SV rpk = checkKey32(L, 1, "public key must be 32 bytes"), msg = checkSV(L, 2);
    Bytes esk, nonce;
    if (!randomFill(esk, 32) || !nonceFill(nonce, 24)) return pushErr(L, kRngError);
    const Bytes epk = x25519Base(esk);
    u8 q[32];
    x25519(q, cu8(esk), cu8(rpk));
    Plan p;
    resolve("xchacha20-poly1305", p);
    Bytes ct;
    const char* err = process(p, true, sealKey(viewOf(q, 32), epk, rpk), nonce, {}, msg, ct);
    return err ? pushErr(L, err) : pushOut(L, epk + nonce + ct, nullptr);
}

// crypto.open(privateKey: string, sealed: string): string | (nil, string)
int cryptoOpen(lua_State* L) {
    const SV sk = checkKey32(L, 1, "private key must be 32 bytes"), in = checkSV(L, 2);
    if (in.size() < 32 + 24 + 16) return pushErr(L, "sealed message too short");
    const Bytes rpk = x25519Base(sk);
    u8 q[32];
    x25519(q, cu8(sk), cu8(in));
    Plan p;
    resolve("xchacha20-poly1305", p);
    Bytes pt;
    const char* err = process(p, false, sealKey(viewOf(q, 32), in.substr(0, 32), rpk), in.substr(32, 24), {}, in.substr(56), pt);
    return err ? pushErr(L, err) : pushOut(L, pt, nullptr);
}

constexpr NativeFunction kFunctions[] = {
    {"hash", cryptoHash},
    {"hmac", cryptoHmac},
    {"pbkdf2", cryptoPbkdf2},
    {"pbkdf2Timed", cryptoPbkdf2Timed},
    {"hkdf", cryptoHkdf},
    {"scrypt", cryptoScrypt},
    {"encrypt", cryptoEncrypt},
    {"decrypt", cryptoDecrypt},
    {"randomBytes", cryptoRandomBytes},
    {"randomInt", cryptoRandomInt},
    {"uuid", cryptoUuid},
    {"ulid", cryptoUlid},
    {"totp", cryptoTotp},
    {"hashPassword", cryptoHashPassword},
    {"verifyPassword", cryptoVerifyPassword},
    {"encode", cryptoEncode},
    {"decode", cryptoDecode},
    {"equals", cryptoEquals},
    {"keypair", cryptoKeypair},
    {"sign", cryptoSign},
    {"verify", cryptoVerify},
    {"sharedSecret", cryptoSharedSecret},
    {"seal", cryptoSeal},
    {"open", cryptoOpen},
};

} // namespace

void openCrypto(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 4);
    setFunctions(L, kFunctions);

    lua_pushboolean(L, aesHardware());
    lua_setfield(L, -2, "hardwareAes");

    int i = 0;
    lua_createtable(L, static_cast<int>(std::size(kHashes)), 0);
    for (const auto& h : kHashes) { lua_pushstring(L, h.name); lua_rawseti(L, -2, ++i); }
    lua_setfield(L, -2, "hashes");

    i = 0;
    lua_createtable(L, 0, 0);
    for (const auto& s : kStreams) { lua_pushstring(L, s.name); lua_rawseti(L, -2, ++i); }
    for (const auto& b : kBlocks)
        for (int m = 0; m < 6; ++m) {
            if (m == GCM && b.bs != 16) continue;
            const std::string name = std::string(b.name) + "-" + kModes[m];
            lua_pushstring(L, name.c_str());
            lua_rawseti(L, -2, ++i);
        }
    lua_setfield(L, -2, "ciphers");
}

} // namespace sonata::lib::libs