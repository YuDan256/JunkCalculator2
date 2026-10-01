// HKDF（RFC 5869）+ TLS 1.3 的 HKDF-Expand-Label（RFC 8446 §7.1）。
#pragma once

#include <cstddef>
#include <cstdint>

namespace jc2::crypto {

// HKDF-Extract：PRK = HMAC(salt, IKM)，输出 32 字节。saltLen 可为 0。
void hkdfExtract(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, uint8_t prk[32]);

// HKDF-Expand：out 长 outLen（≤ 255*32）。prk 固定为 32 字节。
void hkdfExpand(const uint8_t prk[32], const uint8_t* info, size_t infoLen, uint8_t* out, size_t outLen);

// TLS 1.3 HKDF-Expand-Label：label 是 ASCII 文本，内部拼成
//   struct { uint16 length; opaque label<7..255> = "tls13 " + label; opaque context<0..255>; }
void hkdfExpandLabel(const uint8_t secret[32], const char* label, const uint8_t* context, size_t contextLen,
                     uint8_t* out, size_t outLen);

} // namespace jc2::crypto
