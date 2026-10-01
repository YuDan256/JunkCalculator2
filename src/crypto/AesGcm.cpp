#include "AesGcm.h"
#include "ConstTime.h"

#include <cstring>
#include <vector>

namespace jc2::crypto {

namespace {

constexpr uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

inline uint8_t xtime(uint8_t x) {
    return static_cast<uint8_t>((x << 1) ^ (static_cast<uint8_t>((x >> 7) * 0x1b)));
}

// 11 组轮密钥，共 176 字节。
void aes128ExpandKey(const uint8_t key[16], uint8_t rk[176]) {
    std::memcpy(rk, key, 16);
    uint8_t rcon = 0x01;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        std::memcpy(t, rk + i - 4, 4);
        if (i % 16 == 0) {
            const uint8_t tmp = t[0];
            t[0] = static_cast<uint8_t>(kSbox[t[1]] ^ rcon);
            t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];
            t[3] = kSbox[tmp];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; ++j) {
            rk[i + j] = static_cast<uint8_t>(rk[i - 16 + j] ^ t[j]);
        }
    }
}

void aes128EncryptBlock(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    uint8_t t[16];
    std::memcpy(s, in, 16);

    auto addRoundKey = [&](int round) {
        const uint8_t* k = rk + 16 * round;
        for (int i = 0; i < 16; ++i) s[i] ^= k[i];
    };

    addRoundKey(0);
    for (int round = 1; round <= 10; ++round) {
        for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]];

        // ShiftRows：第 r 行左移 r（state[r][c] = s[4c + r]）
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                t[4 * c + r] = s[4 * ((c + r) & 3) + r];
            }
        }
        std::memcpy(s, t, 16);

        if (round != 10) {
            for (int c = 0; c < 4; ++c) {
                uint8_t* col = s + 4 * c;
                const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                const uint8_t all = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);
                col[0] = static_cast<uint8_t>(a0 ^ all ^ xtime(static_cast<uint8_t>(a0 ^ a1)));
                col[1] = static_cast<uint8_t>(a1 ^ all ^ xtime(static_cast<uint8_t>(a1 ^ a2)));
                col[2] = static_cast<uint8_t>(a2 ^ all ^ xtime(static_cast<uint8_t>(a2 ^ a3)));
                col[3] = static_cast<uint8_t>(a3 ^ all ^ xtime(static_cast<uint8_t>(a3 ^ a0)));
            }
        }
        addRoundKey(round);
    }

    std::memcpy(out, s, 16);
    ctZero(s, sizeof(s));
    ctZero(t, sizeof(t));
}

// GF(2^128) 乘法：Z = Z * X（GCM 的位序：最高位在前）。
void gfMul(uint8_t z[16], const uint8_t x[16]) {
    uint8_t v[16];
    uint8_t acc[16] = {0};
    std::memcpy(v, x, 16);

    for (int i = 0; i < 128; ++i) {
        if ((z[i >> 3] >> (7 - (i & 7))) & 1) {
            for (int j = 0; j < 16; ++j) acc[j] ^= v[j];
        }
        const uint8_t lsb = static_cast<uint8_t>(v[15] & 1);
        for (int j = 15; j > 0; --j) {
            v[j] = static_cast<uint8_t>((v[j] >> 1) | (v[j - 1] << 7));
        }
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }

    std::memcpy(z, acc, 16);
    ctZero(v, sizeof(v));
    ctZero(acc, sizeof(acc));
}

// GHASH：把数据按 16 字节分组吸收（不足补零），y 为 128 位累加器。
void ghashUpdate(uint8_t y[16], const uint8_t h[16], const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        uint8_t block[16] = {0};
        const size_t take = (len - off < 16) ? (len - off) : 16;
        std::memcpy(block, data + off, take);
        for (int i = 0; i < 16; ++i) y[i] ^= block[i];
        gfMul(y, h);
        off += take;
    }
}

void storeBE64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
    }
}

// 由 IV 计算 J0（SP 800-38D §7.1）。
void computeJ0(const uint8_t h[16], const uint8_t* iv, size_t ivLen, uint8_t j0[16]) {
    if (ivLen == 12) {
        std::memcpy(j0, iv, 12);
        j0[12] = 0;
        j0[13] = 0;
        j0[14] = 0;
        j0[15] = 1;
        return;
    }
    std::memset(j0, 0, 16);
    ghashUpdate(j0, h, iv, ivLen);
    uint8_t lenBlock[16] = {0};
    storeBE64(lenBlock + 8, static_cast<uint64_t>(ivLen) * 8u);
    for (int i = 0; i < 16; ++i) j0[i] ^= lenBlock[i];
    gfMul(j0, h);
}

void inc32(uint8_t ctr[16]) {
    for (int i = 15; i >= 12; --i) {
        ctr[i] = static_cast<uint8_t>(ctr[i] + 1);
        if (ctr[i] != 0) break;
    }
}

// 用 J0 起算的 CTR 流加解密（就地安全）。
void ctrXor(const uint8_t rk[176], const uint8_t j0[16], const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t ctr[16];
    std::memcpy(ctr, j0, 16);
    uint8_t ks[16];
    size_t off = 0;
    while (off < len) {
        inc32(ctr);
        aes128EncryptBlock(rk, ctr, ks);
        const size_t take = (len - off < 16) ? (len - off) : 16;
        for (size_t i = 0; i < take; ++i) {
            out[off + i] = static_cast<uint8_t>(in[off + i] ^ ks[i]);
        }
        off += take;
    }
    ctZero(ctr, sizeof(ctr));
    ctZero(ks, sizeof(ks));
}

// 计算 tag：T = AES(J0) ^ GHASH(A, C)。
void computeTag(const uint8_t rk[176], const uint8_t h[16], const uint8_t j0[16], const uint8_t* aad,
                size_t aadLen, const uint8_t* ct, size_t ctLen, uint8_t tag[16]) {
    uint8_t y[16] = {0};
    ghashUpdate(y, h, aad, aadLen);
    ghashUpdate(y, h, ct, ctLen);

    uint8_t lenBlock[16];
    storeBE64(lenBlock, static_cast<uint64_t>(aadLen) * 8u);
    storeBE64(lenBlock + 8, static_cast<uint64_t>(ctLen) * 8u);
    for (int i = 0; i < 16; ++i) y[i] ^= lenBlock[i];
    gfMul(y, h);

    uint8_t ek[16];
    aes128EncryptBlock(rk, j0, ek);
    for (int i = 0; i < 16; ++i) tag[i] = static_cast<uint8_t>(ek[i] ^ y[i]);

    ctZero(y, sizeof(y));
    ctZero(ek, sizeof(ek));
}

void gcmSetup(const uint8_t key[16], const uint8_t* iv, size_t ivLen, uint8_t rk[176], uint8_t h[16], uint8_t j0[16]) {
    aes128ExpandKey(key, rk);
    const uint8_t zero[16] = {0};
    aes128EncryptBlock(rk, zero, h);
    computeJ0(h, iv, ivLen, j0);
}

} // namespace

void aes128GcmSeal(const uint8_t key[16], const uint8_t* iv, size_t ivLen, const uint8_t* aad, size_t aadLen,
                   const uint8_t* pt, size_t ptLen, uint8_t* ct, uint8_t tag[16]) {
    uint8_t rk[176];
    uint8_t h[16];
    uint8_t j0[16];
    gcmSetup(key, iv, ivLen, rk, h, j0);

    if (ptLen > 0) {
        ctrXor(rk, j0, pt, ptLen, ct);
    }
    computeTag(rk, h, j0, aad, aadLen, ct, ptLen, tag);

    ctZero(rk, sizeof(rk));
    ctZero(h, sizeof(h));
    ctZero(j0, sizeof(j0));
}

bool aes128GcmOpen(const uint8_t key[16], const uint8_t* iv, size_t ivLen, const uint8_t* aad, size_t aadLen,
                   const uint8_t* ct, size_t ctLen, const uint8_t tag[16], uint8_t* pt) {
    uint8_t rk[176];
    uint8_t h[16];
    uint8_t j0[16];
    gcmSetup(key, iv, ivLen, rk, h, j0);

    uint8_t expect[16];
    computeTag(rk, h, j0, aad, aadLen, ct, ctLen, expect);
    const bool ok = ctEqual(expect, tag, 16);

    if (ok && ctLen > 0) {
        ctrXor(rk, j0, ct, ctLen, pt);
    }

    ctZero(rk, sizeof(rk));
    ctZero(h, sizeof(h));
    ctZero(j0, sizeof(j0));
    ctZero(expect, sizeof(expect));
    return ok;
}

} // namespace jc2::crypto
