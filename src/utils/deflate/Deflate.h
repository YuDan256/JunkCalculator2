#ifndef JC2_DEFLATE_H
#define JC2_DEFLATE_H

#include <cstdint>
#include <cstddef>
#include <vector>

namespace jc {

// 自研 DEFLATE 压缩器（RFC 1951 算法），零第三方依赖，纯 C++20 标准库。
// 输出裸 DEFLATE 流（无 zlib/gzip 外壳、无校验和），仅供 JC2 自用格式（.jcw/.jcb）使用，
// 不保证与任何外部工具的格式兼容性。

// 压缩：src[0..n) → out（裸 DEFLATE 流）。返回 true 表示成功。
bool deflateCompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out);

// 解压：完整 DEFLATE 流 → 原始数据（append 到 out 末尾）。返回 true 表示成功。
bool deflateDecompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out);

} // namespace jc

#endif // JC2_DEFLATE_H
