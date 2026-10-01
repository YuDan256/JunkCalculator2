#include "Sha256.h"
#include "ConstTime.h"

#include <cstring>

namespace jc2::crypto {

namespace {

constexpr uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t loadBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void storeBE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

void transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = loadBE32(block + 4 * i);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace

void Sha256::init() {
    state[0] = 0x6a09e667u;
    state[1] = 0xbb67ae85u;
    state[2] = 0x3c6ef372u;
    state[3] = 0xa54ff53au;
    state[4] = 0x510e527fu;
    state[5] = 0x9b05688cu;
    state[6] = 0x1f83d9abu;
    state[7] = 0x5be0cd19u;
    bitLen = 0;
    bufLen = 0;
    std::memset(buffer, 0, sizeof(buffer));
}

void Sha256::update(const uint8_t* data, size_t len) {
    if (len == 0) return;
    bitLen += static_cast<uint64_t>(len) * 8u;

    if (bufLen > 0) {
        size_t want = 64 - bufLen;
        size_t take = len < want ? len : want;
        std::memcpy(buffer + bufLen, data, take);
        bufLen += take;
        data += take;
        len -= take;
        if (bufLen == 64) {
            transform(state, buffer);
            bufLen = 0;
        }
    }

    while (len >= 64) {
        transform(state, data);
        data += 64;
        len -= 64;
    }

    if (len > 0) {
        std::memcpy(buffer, data, len);
        bufLen = len;
    }
}

void Sha256::final(uint8_t out[32]) {
    // 就地补齐：0x80、若干 0、最后 8 字节大端比特长度。不重入 update。
    const uint64_t bits = bitLen;
    uint8_t block[64];
    std::memcpy(block, buffer, bufLen);
    size_t n = bufLen;
    block[n++] = 0x80;
    if (n > 56) {
        std::memset(block + n, 0, 64 - n);
        transform(state, block);
        n = 0;
    }
    std::memset(block + n, 0, 56 - n);
    for (int i = 0; i < 8; ++i) {
        block[56 + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    }
    transform(state, block);

    for (int i = 0; i < 8; ++i) {
        storeBE32(out + 4 * i, state[i]);
    }

    // 清零中间状态
    ctZero(state, sizeof(state));
    ctZero(buffer, sizeof(buffer));
    ctZero(block, sizeof(block));
    bitLen = 0;
    bufLen = 0;
}

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    Sha256 ctx;
    ctx.init();
    ctx.update(data, len);
    ctx.final(out);
}

} // namespace jc2::crypto
