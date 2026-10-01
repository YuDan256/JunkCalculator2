// 系统信任库：枚举 Windows ROOT 证书存储，返回 DER 列表（进程内缓存）。
// 生产环境用它当信任锚；测试则注入固定夹具，保证结果与机器无关。
#pragma once

#include "../util/BytesUtil.h"

#include <vector>

namespace jc2::crypto {

// 首次调用时枚举并缓存；之后返回同一份引用。
const std::vector<Bytes>& systemRootCertificates();

} // namespace jc2::crypto
