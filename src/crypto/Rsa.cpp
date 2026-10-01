#include "Rsa.h"
#include "BigNum.h"
#include "ConstTime.h"
#include "Sha256.h"

#include <cstring>

namespace jc2::crypto {

namespace {

// SHA-256 的 DigestInfo 前缀（RFC 8017 A.2.4）
const uint8_t kSha256DigestInfo[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
    0x00, 0x04, 0x20
};

// s^e mod n → 定长 k 字节 EM
bool rsaPublicOp(const Bytes& n, const Bytes& e, const Bytes& sig, size_t k, Bytes& em) {
    const BigNum nv = BigNum::fromBytes(n.data(), n.size());
    const BigNum ev = BigNum::fromBytes(e.data(), e.size());
    const BigNum sv = BigNum::fromBytes(sig.data(), sig.size());
    if (nv.isZero() || sv >= nv) return false;
    const BigNum m = BigNum::expMod(sv, ev, nv);
    em.assign(k, 0);
    return m.toBytes(em.data(), em.size());
}

// MGF1-SHA256
void mgf1Sha256(const uint8_t* seed, size_t seedLen, uint8_t* out, size_t outLen) {
    size_t produced = 0;
    uint32_t counter = 0;
    while (produced < outLen) {
        Sha256 ctx;
        ctx.init();
        ctx.update(seed, seedLen);
        const uint8_t c[4] = {static_cast<uint8_t>(counter >> 24), static_cast<uint8_t>(counter >> 16),
                              static_cast<uint8_t>(counter >> 8), static_cast<uint8_t>(counter)};
        ctx.update(c, 4);
        uint8_t digest[32];
        ctx.final(digest);
        const size_t take = (outLen - produced < 32) ? (outLen - produced) : 32;
        std::memcpy(out + produced, digest, take);
        produced += take;
        ++counter;
    }
}

} // namespace

size_t rsaModulusBits(const Bytes& n) {
    size_t i = 0;
    while (i < n.size() && n[i] == 0x00) ++i;
    if (i == n.size()) return 0;
    size_t bits = (n.size() - i - 1) * 8;
    uint8_t top = n[i];
    while (top != 0) {
        ++bits;
        top >>= 1;
    }
    return bits;
}

bool rsaVerifyPkcs1Sha256(const Bytes& n, const Bytes& e, const uint8_t hash32[32], const Bytes& sig) {
    const size_t k = n.size();
    const size_t tLen = sizeof(kSha256DigestInfo) + 32;
    if (k < 3 + tLen + 8) return false;

    Bytes em;
    if (!rsaPublicOp(n, e, sig, k, em)) return false;

    // EM = 0x00 || 0x01 || 0xFF... || 0x00 || DigestInfo
    if (em[0] != 0x00 || em[1] != 0x01) return false;
    const size_t psLen = k - 3 - tLen;
    if (psLen < 8) return false;
    for (size_t i = 2; i < 2 + psLen; ++i) {
        if (em[i] != 0xff) return false;
    }
    if (em[2 + psLen] != 0x00) return false;

    Bytes expected;
    expected.reserve(tLen);
    expected.insert(expected.end(), kSha256DigestInfo, kSha256DigestInfo + sizeof(kSha256DigestInfo));
    expected.insert(expected.end(), hash32, hash32 + 32);
    return ctEqual(em.data() + 3 + psLen, expected.data(), tLen);
}

bool rsaVerifyPssSha256(const Bytes& n, const Bytes& e, const uint8_t hash32[32], const Bytes& sig, size_t saltLen) {
    const size_t modBits = rsaModulusBits(n);
    if (modBits == 0) return false;
    const size_t emBits = modBits - 1;
    const size_t emLen = (emBits + 7) / 8;
    const size_t k = n.size();

    Bytes em;
    if (!rsaPublicOp(n, e, sig, k, em)) return false;
    if (em.size() != emLen) {
        if (k != emLen + 1) return false;
        em.erase(em.begin());   // modBits 不是 8 的倍数时，去掉最左的 0x00
    }

    const size_t hLen = 32;
    if (emLen < hLen + saltLen + 2) return false;
    if (em[emLen - 1] != 0xbc) return false;

    const uint8_t* maskedDB = em.data();
    const uint8_t* h = em.data() + (emLen - hLen - 1);
    const size_t dbLen = emLen - hLen - 1;

    const unsigned unusedBits = static_cast<unsigned>(8 * emLen - emBits);
    if (unusedBits > 0 && (maskedDB[0] & static_cast<uint8_t>(0xffu << (8 - unusedBits))) != 0) {
        return false;
    }

    Bytes db(dbLen);
    mgf1Sha256(h, hLen, db.data(), dbLen);
    for (size_t i = 0; i < dbLen; ++i) {
        db[i] ^= maskedDB[i];
    }
    if (unusedBits > 0) {
        db[0] &= static_cast<uint8_t>(0xffu >> unusedBits);
    }

    // DB = 0x00 * (dbLen - saltLen - 1) || 0x01 || salt
    if (saltLen > 64 || dbLen < saltLen + 1) return false;
    const size_t psLen = dbLen - saltLen - 1;
    for (size_t i = 0; i < psLen; ++i) {
        if (db[i] != 0x00) return false;
    }
    if (db[psLen] != 0x01) return false;
    const uint8_t* salt = db.data() + psLen + 1;

    uint8_t mPrime[8 + 32 + 64];
    std::memset(mPrime, 0, 8);
    std::memcpy(mPrime + 8, hash32, 32);
    std::memcpy(mPrime + 8 + 32, salt, saltLen);

    uint8_t hPrime[32];
    sha256(mPrime, 8 + 32 + saltLen, hPrime);
    return ctEqual(h, hPrime, 32);
}

} // namespace jc2::crypto
