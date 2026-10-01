#include "Sha512.h"

#include <cstring>

namespace jc2::crypto {

namespace {

// 前 80 个素数立方根小数部分的前 64 位（FIPS 180-4 §4.2.3）
const uint64_t K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

// 前 8 个素数平方根小数部分的前 64 位（SHA-512 初值）
const uint64_t kIv512[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

// SHA-384 初值（同上前 9~16 个素数平方根的小数部分）
const uint64_t kIv384[8] = {
    0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
    0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL,
};

inline uint64_t rotr(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

inline uint64_t bigEndian64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

} // namespace

void Sha512::reset() {
    const uint64_t* iv = is384_ ? kIv384 : kIv512;
    for (int i = 0; i < 8; ++i) h_[i] = iv[i];
    total_[0] = total_[1] = 0;
    buffered_ = 0;
}

void Sha512::compress(const uint8_t* block) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = bigEndian64(block + 8 * i);
    for (int i = 16; i < 80; ++i) {
        const uint64_t s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const uint64_t s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint64_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint64_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];

    for (int i = 0; i < 80; ++i) {
        const uint64_t S1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
        const uint64_t ch = (e & f) ^ ((~e) & g);
        const uint64_t t1 = hh + S1 + ch + K[i] + w[i];
        const uint64_t S0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
        const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint64_t t2 = S0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
}

void Sha512::update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    // 长度按比特计（128 位，但实际消息不会超过 2^64 位，这里只维护高 64 位的进位）
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    const uint64_t prev = total_[0];
    total_[0] += bits;
    if (total_[0] < prev) total_[1]++;
    total_[1] += static_cast<uint64_t>(len) >> 61;

    if (buffered_ > 0) {
        const size_t need = kBlockSize - buffered_;
        const size_t take = len < need ? len : need;
        std::memcpy(buffer_ + buffered_, p, take);
        buffered_ += take;
        p += take;
        len -= take;
        if (buffered_ == kBlockSize) {
            compress(buffer_);
            buffered_ = 0;
        }
    }
    while (len >= kBlockSize) {
        compress(p);
        p += kBlockSize;
        len -= kBlockSize;
    }
    if (len > 0) {
        std::memcpy(buffer_, p, len);
        buffered_ = len;
    }
}

void Sha512::finish(uint8_t* out) {
    // 填充：0x80 + 若干 0，使长度 ≡ 112 (mod 128)，再写 16 字节比特长度
    const uint64_t bitLow = total_[0];
    const uint64_t bitHigh = total_[1];

    uint8_t pad[kBlockSize * 2];
    std::memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    const size_t padLen = (buffered_ < 112) ? (112 - buffered_) : (240 - buffered_);
    update(pad, padLen);

    uint8_t lenBytes[16];
    for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<uint8_t>(bitHigh >> (56 - 8 * i));
    for (int i = 0; i < 8; ++i) lenBytes[8 + i] = static_cast<uint8_t>(bitLow >> (56 - 8 * i));
    update(lenBytes, 16);

    uint8_t full[64];
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) full[8 * i + j] = static_cast<uint8_t>(h_[i] >> (56 - 8 * j));
    }
    std::memcpy(out, full, digestSize());
}

void sha384(const void* data, size_t len, uint8_t out[48]) {
    Sha512 h(true);
    h.update(data, len);
    h.finish(out);
}

void sha512(const void* data, size_t len, uint8_t out[64]) {
    Sha512 h(false);
    h.update(data, len);
    h.finish(out);
}

} // namespace jc2::crypto
