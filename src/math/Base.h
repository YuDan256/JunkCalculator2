#ifndef JC2_BASE_H
#define JC2_BASE_H

#include "BigInt.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace jc {

    class BaseNum {
    private:
        BigInt data;  // 底层真实值，永远存十进制 BigInt
        int radix;    // 进制基数 (>= 2)

        // 内部打印助手：将单个数值变成字符
        std::string getDigitChar(int val) const {
            if (val < 10) return std::to_string(val);
            if (val < 36) return std::string(1, static_cast<char>('A' + val - 10));
            return std::to_string(val);
        }

        // 内部位运算助手 (兼容 JC1：按位除 2 取模，无视极其庞大数字的性能，保证绝对准确)
        static BigInt doBitwise(BigInt a, BigInt b, char op) {
            a = a.abs();
            b = b.abs();

            // =============================================
            // 29-bit 分块：2^29 = 536870912 < 10^9
            // 恰好落入 BigInt 单 limb，走 divmod_small 快速路径
            // 相比逐位处理，迭代次数减少 29 倍
            // =============================================
            constexpr int64_t CHUNK = (1LL << 29);  // 536870912
            const BigInt CHUNK_BI(CHUNK);

            // 第一步：提取 29-bit 块（小端序）
            auto extractChunks = [&CHUNK_BI](BigInt n) -> std::vector<uint32_t> {
                std::vector<uint32_t> chunks;
                if (n.isZero()) {
                    chunks.push_back(0);
                    return chunks;
                }
                while (!n.isZero()) {
                    BigInt rem = n % CHUNK_BI;
                    chunks.push_back(static_cast<uint32_t>(std::abs(rem.toDouble())));
                    n = n / CHUNK_BI;
                }
                return chunks;
                };

            auto chunksA = extractChunks(a);
            auto chunksB = extractChunks(b);

            // 第二步：对齐长度
            size_t maxLen = std::max(chunksA.size(), chunksB.size());
            chunksA.resize(maxLen, 0);
            chunksB.resize(maxLen, 0);

            // 第三步：原生 32-bit 位运算 —— O(maxLen)，零开销
            std::vector<uint32_t> chunksR(maxLen);
            for (size_t i = 0; i < maxLen; ++i) {
                switch (op) {
                case '&': chunksR[i] = chunksA[i] & chunksB[i]; break;
                case '|': chunksR[i] = chunksA[i] | chunksB[i]; break;
                case '^': chunksR[i] = chunksA[i] ^ chunksB[i]; break;
                default:  chunksR[i] = 0; break;
                }
            }

            // 第四步：Horner 法则合并（从高位到低位）
            //   result = (...((chunk[m-1]) * BASE + chunk[m-2]) * BASE + ...) + chunk[0]
            //   每步乘法：BigInt × 单 limb BigInt = O(当前 limb 数)
            BigInt result(0);
            for (int i = static_cast<int>(maxLen) - 1; i >= 0; --i) {
                result = result * CHUNK_BI + BigInt(static_cast<int64_t>(chunksR[i]));
            }

            return result;
        }

    public:
        // --- 构造函数 ---
        BaseNum(BigInt val, int r) : data(std::move(val)), radix(r) {
            if (r < 2) throw std::invalid_argument("Math Error: Base radix must be >= 2.");
        }

        BaseNum(double val, int r) : radix(r) {
            if (r < 2) throw std::invalid_argument("Math Error: Base radix must be >= 2.");
            data = BigInt(static_cast<int64_t>(std::round(val)));
        }

        BigInt getValue() const { return data; }
        int getRadix() const { return radix; }

        // =================================================================================
        // 字符串解析引擎：完美复刻 JC1 的 enterBase1 / 2 / 3
        // =================================================================================
        static BaseNum fromString(std::string numStr, int r) {
            if (r < 2) throw std::runtime_error("Math Error: Radix must be >= 2.");
            bool isNeg = false;

            // 处理正负号
            if (!numStr.empty() && numStr[0] == '-') { isNeg = true; numStr.erase(0, 1); }
            else if (!numStr.empty() && numStr[0] == '+') { numStr.erase(0, 1); }

            if (numStr.empty()) throw std::invalid_argument("Base Error: Empty number string.");

            BigInt result(0);
            BigInt baseBI(r);

            // 模式 1 & 2：传统的数字和字母混合 ( <= 36 进制 )
            if (r <= 36 && numStr.find('_') == std::string::npos) {
                for (char c : numStr) {
                    int digit = 0;
                    if (c >= '0' && c <= '9') digit = c - '0';
                    else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
                    else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
                    else throw std::invalid_argument("Base Error: Invalid character '" + std::string(1, c) + "'.");

                    if (digit >= r) throw std::invalid_argument("Base Error: Digit exceeds radix.");
                    result = result * baseBI + BigInt(digit);
                }
            }
            // 模式 3：下划线分块法
            else {
                size_t pos = 0;
                while (pos < numStr.length()) {
                    size_t nextPos = numStr.find('_', pos);
                    std::string block = numStr.substr(pos, nextPos - pos);
                    if (block.empty()) {
                        if (nextPos == std::string::npos) break; // <--- 补充这行
                        pos = nextPos + 1;
                        continue;
                    }

                    // ★ 校验：块内只允许数字字符
                    for (char c : block) {
                        if (c < '0' || c > '9') {
                            throw std::invalid_argument("Base Error: Invalid character '" + std::string(1, c) + "' in digit block.");
                        }
                    }

                    // ★ 安全解析：用 BigInt 解析任意长度的数字块
                    int64_t digit = 0;
                    bool overflow = false;
                    try {
                        // 先尝试 stoll（快速路径，覆盖绝大多数正常输入）
                        size_t parsedLen = 0;
                        digit = std::stoll(block, &parsedLen);
                        if (parsedLen != block.length()) overflow = true;
                    }
                    catch (const std::out_of_range&) {
                        overflow = true;
                    }
                    catch (const std::invalid_argument&) {
                        throw std::invalid_argument("Base Error: Invalid digit block '" + block + "'.");
                    }

                    if (overflow) {
                        // ★ 降级路径：块太大无法放入 int64_t，用 BigInt 解析
                        BigInt digitBI(block);
                        BigInt radixBI(r);
                        if (digitBI.isNegative() || digitBI >= radixBI) {
                            throw std::invalid_argument(
                                "Base Error: Digit block '" + block + "' exceeds radix " + std::to_string(r) + ".");
                        }
                        result = result * baseBI + digitBI;
                    }
                    else {
                        if (digit < 0 || digit >= static_cast<int64_t>(r)) {
                            throw std::invalid_argument(
                                "Base Error: Digit block '" + block + "' exceeds radix " + std::to_string(r) + ".");
                        }
                        result = result * baseBI + BigInt(digit);
                    }

                    if (nextPos == std::string::npos) break;
                    pos = nextPos + 1;
                }
            }
            if (isNeg) result = -result;
            return BaseNum(result, r);
        }

        // =================================================================================
        // 同化四则运算 (Base Assimilation)
        // =================================================================================
        int determineRadix(const BaseNum& other) const {
            if (radix == 10) return other.radix;
            if (other.radix == 10) return radix;
            if (radix != other.radix) throw std::invalid_argument("Base Error: Radix mismatched (and neither is 10).");
            return radix;
        }

        BaseNum operator+(const BaseNum& b) const { return BaseNum(data + b.data, determineRadix(b)); }
        BaseNum operator-(const BaseNum& b) const { return BaseNum(data - b.data, determineRadix(b)); }
        BaseNum operator*(const BaseNum& b) const { return BaseNum(data * b.data, determineRadix(b)); }
        BaseNum operator/(const BaseNum& b) const { return BaseNum(data / b.data, determineRadix(b)); }
        BaseNum operator%(const BaseNum& b) const { return BaseNum(data % b.data, determineRadix(b)); }
        BaseNum operator^(const BaseNum& b) const { return BaseNum(data.pow(static_cast<int64_t>(b.data.toDouble())), determineRadix(b)); }
        BaseNum operator-() const { return BaseNum(-data, radix); }

        // =================================================================================
        // 原汁原味的二进制位运算 (Strict Binary Bitwise)
        // =================================================================================
        void assertBinary(const BaseNum& b, const std::string& op) const {
            if (radix != 2 || b.radix != 2) throw std::runtime_error("Base Error: Bitwise " + op + " only defined for base 2.");
        }

        BaseNum bitAnd(const BaseNum& b) const { assertBinary(b, "AND"); return BaseNum(doBitwise(data, b.data, '&'), 2); }
        BaseNum bitOr(const BaseNum& b)  const { assertBinary(b, "OR"); return BaseNum(doBitwise(data, b.data, '|'), 2); }
        BaseNum bitXor(const BaseNum& b) const { assertBinary(b, "XOR"); return BaseNum(doBitwise(data, b.data, '^'), 2); }

        int bitLength() const {
            if (data.isZero()) return 0;
            BigInt temp = data.abs();

            // 粗算：用 29-bit 块快速定位大致范围
            constexpr int64_t CHUNK = (1LL << 29);
            const BigInt CHUNK_BI(CHUNK);
            int bits = 0;
            while (temp >= CHUNK_BI) {
                temp = temp / CHUNK_BI;
                bits += 29;
            }

            // 精算：剩余不超过 29 位，用原生 int 逐位
            int64_t remaining = static_cast<int64_t>(temp.toDouble());
            while (remaining > 0) {
                remaining >>= 1;
                bits++;
            }

            return bits;
        }

        static int alignToBytes(int bits) {
            if (bits == 0) return 8;
            return ((bits + 7) / 8) * 8;
        }

        BaseNum bitNot(int width = 0) const {
            if (radix != 2) throw std::runtime_error("Base Error: Bitwise NOT only defined for base 2.");
            if (data.isNegative()) throw std::runtime_error("Base Error: Bitwise NOT on negative numbers requires explicit width.");
            // 确定位宽
            int w = width;
            if (w <= 0) {
                w = alignToBytes(bitLength());
                if (w == 0) w = 8; // 零值默认 8 位
            }
            // 构造全 1 掩码：mask = 2^w - 1
            BigInt mask = BigInt(2).pow(w) - BigInt(1);
            // NOT = XOR with all-1s mask
            return BaseNum(doBitwise(data, mask, '^'), 2);
        }

        BaseNum shiftLeft(int shift) const {
            if (shift < 0) throw std::runtime_error("Base Error: Negative shift count.");
            return BaseNum(data * BigInt(radix).pow(shift), radix);
        }
        BaseNum shiftRight(int shift) const {
            if (shift < 0) throw std::runtime_error("Base Error: Negative shift count.");
            return BaseNum(data / BigInt(radix).pow(shift), radix);
        }

        int digitCount() const {
            if (data.isZero()) return 0;
            if (radix == 10) return data.digitCount();
            BigInt temp = data.abs();
            uint32_t r = static_cast<uint32_t>(radix);
            int count = 0;
            while (!temp.isZero()) {
                temp = temp.divmod_small(r).first;
                count++;
            }
            return count;
        }

        // =================================================================================
        // 逆天打印输出
        // =================================================================================
        std::string toString() const {
            if (radix == 10) return data.toString(); // 10 进制不带方括号
            if (data.isZero()) return "[0]_" + std::to_string(radix);

            BigInt temp = data.abs();
            BigInt baseBI(radix);
            std::vector<int> digits;

            while (!temp.isZero()) {
                BigInt rem = temp % baseBI;
                digits.push_back(static_cast<int>(rem.toDouble()));
                temp = temp / baseBI;
            }

            std::string res = "[";
            if (data.isNegative()) res += "-";

            if (radix <= 10) {
                for (int i = static_cast<int>(digits.size()) - 1; i >= 0; --i) {
                    res += std::to_string(digits[i]);
                }
            }
            else if (radix <= 36) {
                for (int i = static_cast<int>(digits.size()) - 1; i >= 0; --i) {
                    res += getDigitChar(digits[i]);
                }
            }
            else {
                for (int i = static_cast<int>(digits.size()) - 1; i >= 0; --i) {
                    res += std::to_string(digits[i]);
                    if (i > 0) res += "_";
                }
            }
            res += "]_" + std::to_string(radix);
            return res;
        }

        friend std::ostream& operator<<(std::ostream& os, const BaseNum& b) {
            return os << b.toString();
        }
    };

} // namespace jc
#endif // JC2_BASE_H
