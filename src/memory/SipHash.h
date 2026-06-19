#ifndef JC2_SIPHASH_H
#define JC2_SIPHASH_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <random>
#include <cstring>

namespace jc {

inline uint64_t siphash_k0 = 0x0706050403020100ULL;
inline uint64_t siphash_k1 = 0x0f0e0d0c0b0a0908ULL;

inline void initSipHashSeed() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    siphash_k0 = gen();
    siphash_k1 = gen();
}

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define SIPROUND \
    do { \
        v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32); \
        v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2; \
        v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0; \
        v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32); \
    } while (0)

inline uint64_t sipHash24(const void *in, size_t inlen) {
    const uint8_t *ni = (const uint8_t *)in;
    const uint8_t *end = ni + (inlen - (inlen % sizeof(uint64_t)));
    const int left = inlen & 7;
    uint64_t b = ((uint64_t)inlen) << 56;
    uint64_t v0 = siphash_k0 ^ 0x736f6d6570736575ULL;
    uint64_t v1 = siphash_k1 ^ 0x646f72616e646f6dULL;
    uint64_t v2 = siphash_k0 ^ 0x6c7967656e657261ULL;
    uint64_t v3 = siphash_k1 ^ 0x7465646279746573ULL;

    for (; ni != end; ni += 8) {
        uint64_t m;
        std::memcpy(&m, ni, sizeof(uint64_t));
        v3 ^= m;
        SIPROUND;
        SIPROUND;
        v0 ^= m;
    }

    switch (left) {
    case 7: b |= ((uint64_t)ni[6]) << 48; [[fallthrough]];
    case 6: b |= ((uint64_t)ni[5]) << 40; [[fallthrough]];
    case 5: b |= ((uint64_t)ni[4]) << 32; [[fallthrough]];
    case 4: b |= ((uint64_t)ni[3]) << 24; [[fallthrough]];
    case 3: b |= ((uint64_t)ni[2]) << 16; [[fallthrough]];
    case 2: b |= ((uint64_t)ni[1]) << 8; [[fallthrough]];
    case 1: b |= ((uint64_t)ni[0]); break;
    case 0: break;
    }

    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b;

    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;

    return v0 ^ v1 ^ v2 ^ v3;
}

#undef ROTL
#undef SIPROUND

inline uint64_t sipHash24String(const std::string& str) {
    return sipHash24(str.data(), str.size());
}

inline uint64_t sipHash24Double(double d) {
    return sipHash24(&d, sizeof(double));
}

} // namespace jc

#endif // JC2_SIPHASH_H
