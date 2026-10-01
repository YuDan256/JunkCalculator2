#ifndef JC2_INTERNAL_CODEC_GZIP_H
#define JC2_INTERNAL_CODEC_GZIP_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// gzip (RFC 1952) / zlib (RFC 1950) 容器外壳 + 校验和。
//
// 本层只负责**容器**：解析头、定位裸 DEFLATE 载荷、校验尾部校验和。
// 真正的 DEFLATE 解码一律复用 src/utils/deflate/Deflate.cpp 的
// jc::deflateDecompress（RFC 1951），这里不再实现任何 Huffman / LZ77 逻辑。
//
// 未支持（与 Deflate.cpp 的能力边界一致，明确列出而不是静默忽略）：
//   - 多个 gzip 成员拼接（RFC 1952 §2.2）：只解第一个成员；
//   - zlib 预设字典（FDICT）：直接报错，不调用 inflateSetDictionary；
//   - gzip FHCRC：字段会被正确跳过，但头 CRC 值本身不重算校验；
//   - 流式/增量解压：一次性整块进出。
//
// 相对 zlib 参考实现更严格的一点：流结束后**不允许**有多余尾随字节
// （gzip 尾部固定取最后 8 字节、zlib 校验和固定取最后 4 字节，多出来的字节会让
// 校验和字段错位从而失败）。RFC 1951 §3.2.3 允许解码器忽略尾随数据，但作为
// "喂进来就必须是一个完整容器"的接口，这里选择报错。

namespace jc {
namespace codec {

using Bytes = std::vector<uint8_t>;

// 解压输出上限（防解压炸弹）：默认 64 MiB。超限直接失败，不继续分配内存。
inline constexpr size_t kDefaultMaxOutput = 64u * 1024u * 1024u;

// gzip 解压：头（magic 1f 8b、CM=8、FLG 的 FTEXT/FHCRC/FEXTRA/FNAME/FCOMMENT）
// → 裸 DEFLATE → 校验 CRC32 与 ISIZE。任一步不符返回 false 并写 err。
bool gzipDecode(const uint8_t* src, size_t n, Bytes& out, std::string& err,
                size_t maxOutput = kDefaultMaxOutput);

// zlib 解压：2 字节头（CM=8，且 (CMF<<8|FLG) % 31 == 0、FDICT=0）
// → 裸 DEFLATE → 校验 Adler-32。任一步不符返回 false 并写 err。
bool zlibDecode(const uint8_t* src, size_t n, Bytes& out, std::string& err,
                size_t maxOutput = kDefaultMaxOutput);

// 裸 DEFLATE（RFC 1951）：与 deflateDecompress 的唯一区别是带输出上限。
bool rawDecode(const uint8_t* src, size_t n, Bytes& out, std::string& err,
               size_t maxOutput = kDefaultMaxOutput);

// 校验和（RFC 1952 §8 / RFC 1950 §9）。
uint32_t crc32(const uint8_t* data, size_t n);
uint32_t adler32(const uint8_t* data, size_t n);

} // namespace codec
} // namespace jc

#endif // JC2_INTERNAL_CODEC_GZIP_H
