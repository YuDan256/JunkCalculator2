#include "Random.h"

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

namespace jc2::crypto {

bool randomBytes(uint8_t* out, size_t len) {
    if (len == 0) return true;
#if defined(_WIN32)
    const NTSTATUS status = BCryptGenRandom(nullptr, out, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0;
#else
    // 其它平台暂无实现（本方案 v1 只做 Windows）
    (void)out;
    return false;
#endif
}

} // namespace jc2::crypto
