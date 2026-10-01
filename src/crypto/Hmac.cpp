#include "Hmac.h"
#include "ConstTime.h"
#include "Sha256.h"

#include <cstring>

namespace jc2::crypto {

void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[32]) {
    uint8_t k[64];
    std::memset(k, 0, sizeof(k));

    if (keyLen > 64) {
        sha256(key, keyLen, k);   // 长密钥先压缩成 32 字节（其余补零）
    } else {
        std::memcpy(k, key, keyLen);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
    }

    uint8_t inner[32];
    Sha256 ctx;
    ctx.init();
    ctx.update(ipad, sizeof(ipad));
    ctx.update(msg, msgLen);
    ctx.final(inner);

    ctx.init();
    ctx.update(opad, sizeof(opad));
    ctx.update(inner, sizeof(inner));
    ctx.final(out);

    ctZero(k, sizeof(k));
    ctZero(ipad, sizeof(ipad));
    ctZero(opad, sizeof(opad));
    ctZero(inner, sizeof(inner));
}

} // namespace jc2::crypto
