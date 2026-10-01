// SHA-384 / SHA-512（FIPS 180-4）。SHA-384 就是换了初值并截断输出的 SHA-512。
#pragma once

#include <cstddef>
#include <cstdint>

namespace jc2::crypto {

class Sha512 {
public:
    static constexpr size_t kBlockSize = 128;

    explicit Sha512(bool sha384) : is384_(sha384) { reset(); }

    void reset();
    void update(const void* data, size_t len);
    void finish(uint8_t* out);   // SHA-384 写 48 字节，SHA-512 写 64 字节

    size_t digestSize() const { return is384_ ? 48 : 64; }

private:
    void compress(const uint8_t* block);

    uint64_t h_[8];
    uint64_t total_[2] = {0, 0};   // 消息长度（比特），高低 64 位
    uint8_t buffer_[kBlockSize];
    size_t buffered_ = 0;
    bool is384_;
};

void sha384(const void* data, size_t len, uint8_t out[48]);
void sha512(const void* data, size_t len, uint8_t out[64]);

} // namespace jc2::crypto
