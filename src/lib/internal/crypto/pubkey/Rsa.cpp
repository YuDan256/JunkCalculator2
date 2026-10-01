#include "Rsa.h"

#include "BigNum.h"
#include "../util/ConstTime.h"
#include "../hash/Sha256.h"
#include "../hash/Sha512.h"

#include <cstring>
#include <vector>

namespace jc2::crypto {

namespace {

// RFC 8017 A.2.4 的 DigestInfo 前缀（含外层 SEQUENCE 与 OCTET STRING 头）
const uint8_t kDigestInfoSha256[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
    0x00, 0x04, 0x20
};
const uint8_t kDigestInfoSha384[] = {
    0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05,
    0x00, 0x04, 0x30
};
const uint8_t kDigestInfoSha512[] = {
    0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05,
    0x00, 0x04, 0x40
};

const uint8_t* digestInfoPrefix(RsaHash h, size_t& len) {
    switch (h) {
        case RsaHash::Sha384: len = sizeof(kDigestInfoSha384); return kDigestInfoSha384;
        case RsaHash::Sha512: len = sizeof(kDigestInfoSha512); return kDigestInfoSha512;
        case RsaHash::Sha256:
        default: len = sizeof(kDigestInfoSha256); return kDigestInfoSha256;
    }
}

void digestOf(RsaHash h, const uint8_t* data, size_t len, uint8_t* out) {
    switch (h) {
        case RsaHash::Sha384: sha384(data, len, out); return;
        case RsaHash::Sha512: sha512(data, len, out); return;
        case RsaHash::Sha256:
        default: sha256(data, len, out); return;
    }
}

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

// MGF1（可选摘要）
void mgf1(RsaHash h, const uint8_t* seed, size_t seedLen, uint8_t* out, size_t outLen) {
    const size_t hLen = rsaHashLength(h);
    size_t produced = 0;
    uint32_t counter = 0;
    while (produced < outLen) {
        const uint8_t c[4] = {static_cast<uint8_t>(counter >> 24), static_cast<uint8_t>(counter >> 16),
                              static_cast<uint8_t>(counter >> 8), static_cast<uint8_t>(counter)};
        Sha256 s256;
        Sha512 s512(h == RsaHash::Sha384);
        uint8_t digest[64];
        if (h == RsaHash::Sha256) {
            s256.init();
            s256.update(seed, seedLen);
            s256.update(c, 4);
            s256.final(digest);
        } else {
            s512.update(seed, seedLen);
            s512.update(c, 4);
            s512.finish(digest);
        }
        const size_t take = (outLen - produced < hLen) ? (outLen - produced) : hLen;
        std::memcpy(out + produced, digest, take);
        produced += take;
        ++counter;
    }
}

} // namespace

size_t rsaHashLength(RsaHash h) {
    switch (h) {
        case RsaHash::Sha384: return 48;
        case RsaHash::Sha512: return 64;
        case RsaHash::Sha256:
        default: return 32;
    }
}

const char* rsaHashName(RsaHash h) {
    switch (h) {
        case RsaHash::Sha384: return "sha384";
        case RsaHash::Sha512: return "sha512";
        case RsaHash::Sha256:
        default: return "sha256";
    }
}

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

bool rsaVerifyPkcs1(const Bytes& n, const Bytes& e, const uint8_t* digest, const Bytes& sig, RsaHash hash) {
    const size_t hLen = rsaHashLength(hash);
    size_t prefixLen = 0;
    const uint8_t* prefix = digestInfoPrefix(hash, prefixLen);
    const size_t k = n.size();
    const size_t tLen = prefixLen + hLen;
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

    std::vector<uint8_t> expected;
    expected.reserve(tLen);
    expected.insert(expected.end(), prefix, prefix + prefixLen);
    expected.insert(expected.end(), digest, digest + hLen);
    return ctEqual(em.data() + 3 + psLen, expected.data(), tLen);
}

bool rsaVerifyPss(const Bytes& n, const Bytes& e, const uint8_t* digest, const Bytes& sig, size_t saltLen,
                  RsaHash hash) {
    const size_t modBits = rsaModulusBits(n);
    if (modBits == 0) return false;
    const size_t emBits = modBits - 1;
    const size_t emLen = (emBits + 7) / 8;
    const size_t k = n.size();
    const size_t hLen = rsaHashLength(hash);

    Bytes em;
    if (!rsaPublicOp(n, e, sig, k, em)) return false;
    if (em.size() != emLen) {
        if (k != emLen + 1) return false;
        em.erase(em.begin());   // modBits 不是 8 的倍数时，去掉最左的 0x00
    }

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
    mgf1(hash, h, hLen, db.data(), dbLen);
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

    std::vector<uint8_t> mPrime(8 + hLen + saltLen, 0);
    std::memcpy(mPrime.data() + 8, digest, hLen);
    std::memcpy(mPrime.data() + 8 + hLen, salt, saltLen);

    std::vector<uint8_t> hPrime(hLen);
    digestOf(hash, mPrime.data(), mPrime.size(), hPrime.data());
    return ctEqual(h, hPrime.data(), hLen);
}

bool rsaVerifyPkcs1Sha256(const Bytes& n, const Bytes& e, const uint8_t hash32[32], const Bytes& sig) {
    return rsaVerifyPkcs1(n, e, hash32, sig, RsaHash::Sha256);
}

bool rsaVerifyPssSha256(const Bytes& n, const Bytes& e, const uint8_t hash32[32], const Bytes& sig, size_t saltLen) {
    return rsaVerifyPss(n, e, hash32, sig, saltLen, RsaHash::Sha256);
}

} // namespace jc2::crypto
