#include "Hkdf.h"
#include "ConstTime.h"
#include "Hmac.h"

#include <cstring>
#include <vector>

namespace jc2::crypto {

void hkdfExtract(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, uint8_t prk[32]) {
    // RFC 5869：salt 缺省为 HashLen 个零字节，等价于把空 salt 当作 32 个 0
    static const uint8_t zeros[32] = {0};
    if (salt == nullptr || saltLen == 0) {
        hmacSha256(zeros, sizeof(zeros), ikm, ikmLen, prk);
    } else {
        hmacSha256(salt, saltLen, ikm, ikmLen, prk);
    }
}

void hkdfExpand(const uint8_t prk[32], const uint8_t* info, size_t infoLen, uint8_t* out, size_t outLen) {
    if (outLen == 0) return;

    uint8_t t[32];
    size_t tLen = 0;
    size_t produced = 0;
    uint8_t counter = 1;

    std::vector<uint8_t> block;
    block.reserve(32 + infoLen + 1);

    while (produced < outLen) {
        block.clear();
        if (tLen > 0) {
            block.insert(block.end(), t, t + tLen);
        }
        if (infoLen > 0) {
            block.insert(block.end(), info, info + infoLen);
        }
        block.push_back(counter);

        hmacSha256(prk, 32, block.data(), block.size(), t);
        tLen = 32;

        size_t take = (outLen - produced < 32) ? (outLen - produced) : 32;
        std::memcpy(out + produced, t, take);
        produced += take;
        ++counter;
    }

    ctZero(t, sizeof(t));
    if (!block.empty()) {
        ctZero(block.data(), block.size());
    }
}

void hkdfExpandLabel(const uint8_t secret[32], const char* label, const uint8_t* context, size_t contextLen,
                     uint8_t* out, size_t outLen) {
    static const char prefix[] = "tls13 ";
    const size_t labelLen = 6 + std::strlen(label);

    // struct { uint16 length; opaque label<7..255>; opaque context<0..255>; }
    std::vector<uint8_t> info;
    info.reserve(2 + 1 + labelLen + 1 + contextLen);
    info.push_back(static_cast<uint8_t>(outLen >> 8));
    info.push_back(static_cast<uint8_t>(outLen & 0xff));
    info.push_back(static_cast<uint8_t>(labelLen));
    info.insert(info.end(), prefix, prefix + 6);
    info.insert(info.end(), label, label + std::strlen(label));
    info.push_back(static_cast<uint8_t>(contextLen));
    if (contextLen > 0 && context != nullptr) {
        info.insert(info.end(), context, context + contextLen);
    }

    hkdfExpand(secret, info.data(), info.size(), out, outLen);

    ctZero(info.data(), info.size());
}

} // namespace jc2::crypto
