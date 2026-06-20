#ifndef JC2_BIGINT_H
#define JC2_BIGINT_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>

// 引入复数以支持与复数的隐式混合运算提升
#include "Complex.h"

namespace jc {

    class BigInt {
    private:
        std::vector<uint32_t> data; // 小端序：data[0] 存最低的 32 位
        bool negative = false;

        // 清理前导零
        void trim() {
            while (data.size() > 1 && data.back() == 0) data.pop_back();
            if (data.size() == 1 && data[0] == 0) negative = false;
        }

        // 绝对值比较：|this| vs |other|  返回 -1 (小于), 0 (等于), 1 (大于)
        int absCompare(const BigInt& other) const {
            if (data.size() != other.data.size())
                return data.size() < other.data.size() ? -1 : 1;
            for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
                if (data[i] != other.data[i]) return data[i] < other.data[i] ? -1 : 1;
            }
            return 0;
        }

        // 绝对值加法
        static BigInt absAdd(const BigInt& a, const BigInt& b) {
            BigInt result;
            size_t n = std::max(a.data.size(), b.data.size());
            result.data.resize(n, 0);
            uint64_t carry = 0;
            for (size_t i = 0; i < n; ++i) {
                uint64_t sum = carry;
                if (i < a.data.size()) sum += a.data[i];
                if (i < b.data.size()) sum += b.data[i];
                result.data[i] = static_cast<uint32_t>(sum);
                carry = sum >> 32;
            }
            if (carry > 0) result.data.push_back(static_cast<uint32_t>(carry));
            return result;
        }

        // 绝对值减法
        static BigInt absSub(const BigInt& a, const BigInt& b) {
            BigInt result;
            result.data.resize(a.data.size(), 0);
            uint64_t borrow = 0;
            for (size_t i = 0; i < a.data.size(); ++i) {
                uint64_t diff = static_cast<uint64_t>(a.data[i]) - borrow;
                if (i < b.data.size()) diff -= b.data[i];
                result.data[i] = static_cast<uint32_t>(diff);
                borrow = (diff >> 32) & 1;
            }
            result.trim();
            return result;
        }

        // 基础 O(N^2) 乘法
        static BigInt mul_basecase(const BigInt& a, const BigInt& b) {
            size_t n = a.data.size(), m = b.data.size();
            BigInt result;
            result.data.assign(n + m, 0);
            for (size_t i = 0; i < n; ++i) {
                if (a.data[i] == 0) continue;
                uint64_t carry = 0;
                uint64_t d_i = a.data[i];
                for (size_t j = 0; j < m; ++j) {
                    uint64_t prod = d_i * b.data[j] + result.data[i + j] + carry;
                    result.data[i + j] = static_cast<uint32_t>(prod);
                    carry = prod >> 32;
                }
                if (carry > 0) result.data[i + m] += static_cast<uint32_t>(carry);
            }
            result.trim();
            return result;
        }

        // Karatsuba O(N^1.585) 乘法
        static BigInt karatsuba(const BigInt& a, const BigInt& b) {
            size_t n = a.data.size();
            size_t m = b.data.size();
            // 当 limb 数量较小时，使用基础乘法更快（32 limbs 约 288 位十进制）
            if (n < 32 || m < 32) return mul_basecase(a, b);

            size_t half = std::max(n, m) / 2;

            auto split = [half](const BigInt& num, BigInt& low, BigInt& high) {
                if (num.data.size() <= half) {
                    low = num;
                    high = BigInt(0);
                } else {
                    low.data.assign(num.data.begin(), num.data.begin() + half);
                    high.data.assign(num.data.begin() + half, num.data.end());
                    low.trim();
                    high.trim();
                }
            };

            BigInt a0, a1, b0, b1;
            split(a, a0, a1);
            split(b, b0, b1);

            BigInt z0 = karatsuba(a0, b0);
            BigInt z2 = karatsuba(a1, b1);
            BigInt z1 = karatsuba(absAdd(a0, a1), absAdd(b0, b1));
            z1 = absSub(absSub(z1, z2), z0);

            BigInt result = z0;
            if (!z1.isZero()) {
                BigInt z1_shifted = z1;
                z1_shifted.data.insert(z1_shifted.data.begin(), half, 0);
                result = absAdd(result, z1_shifted);
            }
            if (!z2.isZero()) {
                BigInt z2_shifted = z2;
                z2_shifted.data.insert(z2_shifted.data.begin(), 2 * half, 0);
                result = absAdd(result, z2_shifted);
            }

            result.trim();
            return result;
        }

        static std::pair<BigInt, BigInt> divmod(const BigInt& a, const BigInt& b) {
            if (b.isZero()) throw std::runtime_error("Math Error: Division by zero.");

            BigInt absA = a.abs(), absB = b.abs();

            if (absA < absB) {
                return { BigInt(0), a };
            }

            // 对于小除数，走快速路径
            if (absB.data.size() == 1) {
                auto [q, r] = a.divmod_small(absB.data[0]);
                q.negative = (a.negative != b.negative);
                if (q.isZero()) q.negative = false;
                BigInt remBI(r);
                remBI.negative = a.negative;
                if (remBI.isZero()) remBI.negative = false;
                return { q, remBI };
            }

            // =============================================
            // Knuth D 算法 (In-place Long Division)
            // 避免了内层循环中频繁的 BigInt 对象创建和拷贝
            // =============================================
            int n_orig = static_cast<int>(absA.data.size());
            int m = static_cast<int>(absB.data.size());

            // 归一化因子 d，使得除数最高位 >= BASE / 2
            uint32_t d = static_cast<uint32_t>((1ULL << 32) / (static_cast<uint64_t>(absB.data.back()) + 1));

            auto mul_scalar = [](const BigInt& num, uint32_t scalar) {
                if (scalar == 1) return num;
                BigInt res;
                res.data.resize(num.data.size(), 0);
                uint64_t carry = 0;
                for (size_t i = 0; i < num.data.size(); ++i) {
                    uint64_t prod = static_cast<uint64_t>(num.data[i]) * scalar + carry;
                    res.data[i] = static_cast<uint32_t>(prod);
                    carry = prod >> 32;
                }
                if (carry > 0) res.data.push_back(static_cast<uint32_t>(carry));
                return res;
            };

            BigInt u = mul_scalar(absA, d);
            BigInt v = mul_scalar(absB, d);

            // 确保 u 有 n_orig + 1 个 limb
            u.data.resize(n_orig + 1, 0);

            BigInt quotient;
            quotient.data.resize(n_orig - m + 1, 0);

            for (int j = n_orig - m; j >= 0; --j) {
                // 估算商 q_hat
                uint64_t num = (static_cast<uint64_t>(u.data[j + m]) << 32) | u.data[j + m - 1];
                uint64_t v_m1 = v.data[m - 1];
                uint64_t q_hat = num / v_m1;
                uint64_t r_hat = num % v_m1;

                // 修正 q_hat
                if (m >= 2) {
                    uint64_t v_m2 = v.data[m - 2];
                    uint64_t u_jm2 = u.data[j + m - 2];
                    while (q_hat == (1ULL << 32) || q_hat * v_m2 > (r_hat << 32) + u_jm2) {
                        q_hat--;
                        r_hat += v_m1;
                        if (r_hat >= (1ULL << 32)) break;
                    }
                } else {
                    if (q_hat == (1ULL << 32)) q_hat--;
                }

                if (q_hat == 0) {
                    quotient.data[j] = 0;
                    continue;
                }

                // 乘法并减去 (u[j..j+m] -= q_hat * v)
                uint64_t carry = 0;
                for (int i = 0; i < m; ++i) {
                    uint64_t prod = q_hat * v.data[i] + carry;
                    uint32_t p_digit = static_cast<uint32_t>(prod);
                    carry = prod >> 32;
                    
                    if (u.data[j + i] < p_digit) {
                        u.data[j + i] -= p_digit;
                        carry++;
                    } else {
                        u.data[j + i] -= p_digit;
                    }
                }
                
                bool is_borrow = u.data[j + m] < carry;
                u.data[j + m] -= static_cast<uint32_t>(carry);

                quotient.data[j] = static_cast<uint32_t>(q_hat);

                // 如果减多了，加回来 (极少发生)
                if (is_borrow) {
                    quotient.data[j]--;
                    uint64_t carry_add = 0;
                    for (int i = 0; i < m; ++i) {
                        uint64_t sum = static_cast<uint64_t>(u.data[j + i]) + v.data[i] + carry_add;
                        u.data[j + i] = static_cast<uint32_t>(sum);
                        carry_add = sum >> 32;
                    }
                    u.data[j + m] += static_cast<uint32_t>(carry_add);
                }
            }

            quotient.negative = (a.negative != b.negative);
            quotient.trim();

            // 还原余数 (r = u / d)
            BigInt remainder;
            remainder.data.resize(m, 0);
            uint64_t rem = 0;
            for (int i = m - 1; i >= 0; --i) {
                uint64_t cur = (rem << 32) | u.data[i];
                remainder.data[i] = static_cast<uint32_t>(cur / d);
                rem = cur % d;
            }
            remainder.negative = a.negative;
            remainder.trim();

            return { quotient, remainder };
        }

    public:
        // =================================================================================
        // 纯流式外存与分页缓冲引擎 (Streaming I/O & Paged Cache Engine)
        // =================================================================================
        struct PrimeIndex {
            int64_t indexNumber;
            int64_t primeValue;
            std::streampos offset;
        };

        inline static std::vector<PrimeIndex> primeFileIndex;
        inline static bool fileIndexed = false;
        inline static int64_t totalPrimesInFile = 0;
        inline static int64_t largestPrimeInFile = 0;
        static constexpr int64_t BLOCK_SIZE = 10000;

        inline static std::string customPrimePath = "";
        static std::string getPrimeFilePath() {
            return customPrimePath;
        }

        // --- 外部更换挂载路径接口 ---
        static void setPrimeFilePath(const std::string& newPath) {
            // 先检查更换的文件存不存在
            namespace fs = std::filesystem;
            if (!fs::exists(newPath) && newPath != "default") {
                throw std::runtime_error("IO Error: Prime table file not found at " + newPath);
            }

            if (newPath == "default") customPrimePath = "";
            else customPrimePath = newPath;
            // 路径变了，以前的旧锚点、旧索引全部作废，必须清空！
            primeFileIndex.clear();
            fileIndexed = false;
            totalPrimesInFile = 0;
            largestPrimeInFile = 0;

            if (customPrimePath.empty()) {
                std::cout << "[System] Prime engine remounted to dynamic computation." << std::endl;
            } else {
                std::cout << "[System] Prime engine remounted to: " << getPrimeFilePath() << std::endl;
                buildFileIndex();
            }
        }

        // --- (可选加速) 极速扫描建立文件索引 (使用堆内存 buffer 防栈溢出) ---
        static void buildFileIndex() {
            if (fileIndexed) return;

            std::string filepath = getPrimeFilePath();
            if (filepath.empty()) return;

            std::ifstream file(filepath, std::ios::binary);
            if (!file.is_open()) {
                fileIndexed = true;
                std::cout << "[System] Notice: Prime table not found at " << filepath << ". Using dynamic computation." << std::endl;
                return;
            }

            std::cout << "[System] Building prime index tree from " << filepath << "..." << std::endl;

            primeFileIndex.clear();
            int64_t count = 0;

            constexpr size_t BUFFER_SIZE = 4194304; // 4MB
            std::vector<char> buffer(BUFFER_SIZE);

            std::streampos absolutePos = 0;
            int64_t currentPrime = 0;
            bool readingNumber = false;
            std::streampos numberStartPos = 0;  // ★ 新增：正向记录每个数字的起始文件偏移

            while (file.read(buffer.data(), BUFFER_SIZE) || file.gcount() > 0) {
                size_t bytesRead = static_cast<size_t>(file.gcount());
                for (size_t i = 0; i < bytesRead; ++i) {
                    char c = buffer[i];
                    if (c >= '0' && c <= '9') {
                        if (!readingNumber) {
                            // ★ 第一个数字字符出现时，立刻锁定绝对起始位置
                            numberStartPos = absolutePos + static_cast<std::streampos>(i);
                        }
                        currentPrime = currentPrime * 10 + (c - '0');
                        readingNumber = true;
                    }
                    else if (c == '\n' || c == '\r') {
                        if (readingNumber) {
                            count++;
                            if (count % BLOCK_SIZE == 1 || count == 1) {
                                // ★ 直接使用正向记录的位置，不再反算
                                primeFileIndex.push_back({ count - 1, currentPrime, numberStartPos });
                            }
                            largestPrimeInFile = currentPrime;
                            currentPrime = 0;
                            readingNumber = false;
                        }
                    }
                }
                absolutePos += bytesRead;
            }

            // 处理文件末尾没有换行符的最后一个数字
            if (readingNumber) {
                count++;
                if (count % BLOCK_SIZE == 1 || count == 1) {
                    // ★ 同样使用正向记录的位置
                    primeFileIndex.push_back({ count - 1, currentPrime, numberStartPos });
                }
                largestPrimeInFile = currentPrime;
            }

            totalPrimesInFile = count;
            fileIndexed = true;
            file.close();
            if (totalPrimesInFile > 0) {
                std::cout << "[System] Successfully indexed " << totalPrimesInFile << " primes." << std::endl;
            }
        }

        // --- (可选加速) 锚点空降 ---
        static int64_t getPrimeAt(int64_t index) {
            if (!fileIndexed) return -1; // 没建索引拒绝服务
            if (index < 0 || index >= totalPrimesInFile) return -1;

            auto it = std::upper_bound(primeFileIndex.begin(), primeFileIndex.end(), index,
                [](int64_t val, const PrimeIndex& anchor) { return val < anchor.indexNumber; });
            if (it == primeFileIndex.begin()) it = primeFileIndex.begin();
            else --it;

            std::ifstream file(getPrimeFilePath(), std::ios::binary);
            file.seekg(it->offset);

            int64_t currentIdx = it->indexNumber;
            std::string line;
            while (currentIdx <= index && std::getline(file, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                if (currentIdx == index) return std::stoll(line);
                currentIdx++;
            }
            return -1;
        }

        // =================================================================================
        // 核心构造函数与基础操作
        // =================================================================================
        BigInt() : data(1, 0), negative(false) {}

        BigInt(int64_t val) {
            negative = (val < 0);
            uint64_t v = static_cast<uint64_t>(val);
            if (negative) v = 0ULL - v;
            if (v == 0) { data.push_back(0); return; }
            while (v > 0) {
                data.push_back(static_cast<uint32_t>(v & 0xFFFFFFFFULL));
                v >>= 32;
            }
        }

        BigInt mul_small(uint32_t v) const {
            if (v == 0) return BigInt(0);
            if (v == 1) return *this;
            BigInt res;
            res.data.resize(data.size(), 0);
            uint64_t carry = 0;
            for (size_t i = 0; i < data.size(); ++i) {
                uint64_t prod = static_cast<uint64_t>(data[i]) * v + carry;
                res.data[i] = static_cast<uint32_t>(prod);
                carry = prod >> 32;
            }
            if (carry > 0) res.data.push_back(static_cast<uint32_t>(carry));
            res.negative = negative;
            return res;
        }

        BigInt add_small(uint32_t v) const {
            if (v == 0) return *this;
            BigInt res = *this;
            uint64_t carry = v;
            for (size_t i = 0; i < res.data.size() && carry > 0; ++i) {
                uint64_t sum = static_cast<uint64_t>(res.data[i]) + carry;
                res.data[i] = static_cast<uint32_t>(sum);
                carry = sum >> 32;
            }
            if (carry > 0) res.data.push_back(static_cast<uint32_t>(carry));
            return res;
        }

        // 单个 limb(块) 的除法/取模
        std::pair<BigInt, uint32_t> divmod_small(uint32_t divisor) const {
            if (divisor == 0) throw std::runtime_error("Math Error: Division by zero.");

            BigInt q;
            q.data.resize(data.size(), 0);
            uint64_t rem = 0;
            for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
                uint64_t cur = (rem << 32) | data[i];
                q.data[i] = static_cast<uint32_t>(cur / divisor);
                rem = cur % divisor;
            }
            q.negative = negative;
            q.trim();
            return { q, static_cast<uint32_t>(rem) };
        }

        explicit BigInt(const std::string& s) {
            if (s.empty()) throw std::invalid_argument("BigInt Error: Empty string.");
            size_t start = 0;
            negative = false;
            if (s[0] == '-') { negative = true; start = 1; }
            else if (s[0] == '+') { start = 1; }
            if (start == s.size()) throw std::invalid_argument("BigInt Error: No digits found.");

            data.push_back(0);
            for (size_t i = start; i < s.size(); i += 9) {
                size_t len = std::min<size_t>(9, s.size() - i);
                uint32_t chunk = std::stoul(s.substr(i, len));
                uint32_t multiplier = 1;
                for (size_t j = 0; j < len; ++j) multiplier *= 10;
                
                *this = mul_small(multiplier);
                *this = add_small(chunk);
            }
            trim();
        }

        bool isZero() const { return data.size() == 1 && data[0] == 0; }
        bool isNegative() const { return negative; }

        double toDouble() const {
            double result = 0.0;
            for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
                result = result * 4294967296.0 + static_cast<double>(data[i]);
                if (!std::isfinite(result))
                    throw std::runtime_error("Math Error: BigInt too large to convert to double.");
            }
            return negative ? -result : result;
        }

        static double toDoubleRatio(const BigInt& num, const BigInt& den) {
            if (den.isZero()) throw std::runtime_error("Math Error: Division by zero.");
            if (num.isZero()) return 0.0;

            int n_size = static_cast<int>(num.data.size());
            int d_size = static_cast<int>(den.data.size());

            // 如果两者都在 double 安全范围内 (32 * 9.6 = 307 位十进制)，直接计算
            if (n_size <= 32 && d_size <= 32) {
                return num.toDouble() / den.toDouble();
            }

            // 否则，提取最高 3 个 limb (约 96 bits，足以覆盖 double 的 53 bits 精度)
            auto extract = [](const BigInt& b) -> std::pair<double, int> {
                if (b.isZero()) return {0.0, 0};
                int sz = static_cast<int>(b.data.size());
                double res = 0.0;
                int start = std::max(0, sz - 3);
                for (int i = sz - 1; i >= start; --i) {
                    res = res * 4294967296.0 + b.data[i];
                }
                if (b.negative) res = -res;
                return {res, start};
            };

            auto [n_val, n_exp] = extract(num);
            auto [d_val, d_exp] = extract(den);

            double ratio = n_val / d_val;
            int exp_diff = n_exp - d_exp;

            if (exp_diff != 0) {
                ratio *= std::pow(4294967296.0, exp_diff);
            }
            
            if (!std::isfinite(ratio)) {
                throw std::runtime_error("Math Error: Fraction too large to convert to double.");
            }
            return ratio;
        }

        int64_t toInt64() const {
            if (data.size() > 3)
                throw std::runtime_error("Overflow: BigInt too large for int64.");

            // ★ 用 uint64_t 累加，避免中间步骤的有符号溢出
            uint64_t result = 0;
            constexpr uint64_t LIMIT = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

            for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
                if (result > (LIMIT - static_cast<uint64_t>(data[i])) >> 32)
                    if (!(negative && i == 0 && (result << 32) + static_cast<uint64_t>(data[i])
                        == static_cast<uint64_t>(LIMIT) + 1ULL))
                        throw std::runtime_error("Overflow: BigInt too large for int64.");
                result = (result << 32) + static_cast<uint64_t>(data[i]);
            }

            if (!negative) {
                if (result > LIMIT)
                    throw std::runtime_error("Overflow: BigInt too large for int64.");
                return static_cast<int64_t>(result);
            }
            else {
                if (result == static_cast<uint64_t>(LIMIT) + 1ULL)
                    return std::numeric_limits<int64_t>::min();
                if (result > LIMIT)
                    throw std::runtime_error("Overflow: BigInt too large for int64.");
                return -static_cast<int64_t>(result);
            }
        }

        std::string toString() const {
            if (isZero()) return "0";
            BigInt temp = this->abs();
            std::string result;
            while (!temp.isZero()) {
                auto [q, rem] = temp.divmod_small(1000000000);
                temp = q;
                std::string chunk = std::to_string(rem);
                if (!temp.isZero()) {
                    chunk = std::string(9 - chunk.length(), '0') + chunk;
                }
                result = chunk + result;
            }
            if (negative) result = "-" + result;
            return result;
        }

        // =================================================================================
        // 运算符重载 (大整数之间的计算)
        // =================================================================================
        bool operator==(const BigInt& other) const { return negative == other.negative && data == other.data; }
        bool operator!=(const BigInt& other) const { return !(*this == other); }
        bool operator<(const BigInt& other) const {
            if (negative != other.negative) return negative;
            int cmp = absCompare(other);
            return negative ? (cmp > 0) : (cmp < 0);
        }
        bool operator>(const BigInt& other) const { return other < *this; }
        bool operator<=(const BigInt& other) const { return !(other < *this); }
        bool operator>=(const BigInt& other) const { return !(*this < other); }

        BigInt operator-() const {
            BigInt result = *this;
            if (!isZero()) result.negative = !result.negative;
            return result;
        }

        BigInt operator+(const BigInt& other) const {
            if (negative == other.negative) {
                BigInt result = absAdd(*this, other);
                result.negative = negative;
                result.trim();
                return result;
            }
            int cmp = absCompare(other);
            if (cmp == 0) return BigInt(0);
            if (cmp > 0) { BigInt result = absSub(*this, other); result.negative = negative; return result; }
            BigInt result = absSub(other, *this);
            result.negative = other.negative;
            return result;
        }

        BigInt operator-(const BigInt& other) const { return *this + (-other); }

        BigInt operator*(const BigInt& other) const {
            BigInt result = karatsuba(*this, other);
            result.negative = (negative != other.negative);
            if (result.isZero()) result.negative = false;
            return result;
        }

        BigInt operator/(const BigInt& other) const { return divmod(*this, other).first; }
        BigInt operator%(const BigInt& other) const { return divmod(*this, other).second; }

        static BigInt mathMod(const BigInt& a, const BigInt& m) {
            if (m.isZero()) throw std::runtime_error("Math Error: Modulo by zero.");
            BigInt r = a % m;
            if (r.isNegative()) r = r + m.abs();
            return r;
        }

        BigInt abs() const {
            BigInt result = *this;
            result.negative = false;
            return result;
        }

        // 基础的 64 位整数幂
        BigInt pow(int64_t exp) const {
            if (exp < 0) throw std::runtime_error("Math Error: BigInt negative exponent not supported directly here.");
            BigInt result(1), base = *this;
            while (exp > 0) {
                if (exp & 1) result = result * base;
                base = base * base;
                exp >>= 1;
            }
            return result;
        }

        // 接受 BigInt 指数的高阶包装
        BigInt pow(const BigInt& exp) const {
            if (exp.isNegative()) throw std::runtime_error("Math Error: Positive exponent expected for BigInt return type.");
            // 安全截断：指数极大时转成 int64_t 肯定会溢出报错，但这是合理的，
            // 因为地球上没有计算机能算哪怕 2 甚至 10 的那么高次方的精确大数
            return this->pow(exp.toInt64());
        }

        int digitCount() const {
            if (isZero()) return 0;
            std::string s = toString();
            return static_cast<int>(negative ? s.size() - 1 : s.size());
        }

        // =================================================================================
        // 混合计算自动降阶 / 升维隐式友元接口 (为了迎合 Value.h)
        // =================================================================================
        bool operator==(double d) const {
            if (isZero()) return d == 0.0;
            try { return toDouble() == d; }
            catch (...) { return false; }
        }
        friend bool operator==(double d, const BigInt& b) { return b == d; }

        // BigInt <-> double
        friend double operator+(const BigInt& a, double b) { return a.toDouble() + b; }
        friend double operator+(double a, const BigInt& b) { return a + b.toDouble(); }
        friend double operator-(const BigInt& a, double b) { return a.toDouble() - b; }
        friend double operator-(double a, const BigInt& b) { return a - b.toDouble(); }
        friend double operator*(const BigInt& a, double b) { return a.toDouble() * b; }
        friend double operator*(double a, const BigInt& b) { return a * b.toDouble(); }
        friend double operator/(const BigInt& a, double b) {
            if (b == 0.0) throw std::runtime_error("Math Error: Division by zero.");
            return a.toDouble() / b;
        }
        friend double operator/(double a, const BigInt& b) {
            if (b.isZero()) throw std::runtime_error("Math Error: Division by zero.");
            return a / b.toDouble();
        }
        friend double operator%(const BigInt& a, double b) {
            if (b == 0.0) throw std::runtime_error("Math Error: Modulo by zero.");
            return std::fmod(a.toDouble(), b);
        }
        friend double operator%(double a, const BigInt& b) {
            if (b.isZero()) throw std::runtime_error("Math Error: Modulo by zero.");
            return std::fmod(a, b.toDouble());
        }

        // BigInt <-> Complex
        friend Complex operator+(const BigInt& a, const Complex& b) { return Complex(a.toDouble()) + b; }
        friend Complex operator+(const Complex& a, const BigInt& b) { return a + Complex(b.toDouble()); }
        friend Complex operator-(const BigInt& a, const Complex& b) { return Complex(a.toDouble()) - b; }
        friend Complex operator-(const Complex& a, const BigInt& b) { return a - Complex(b.toDouble()); }
        friend Complex operator*(const BigInt& a, const Complex& b) { return Complex(a.toDouble()) * b; }
        friend Complex operator*(const Complex& a, const BigInt& b) { return a * Complex(b.toDouble()); }
        friend Complex operator/(const BigInt& a, const Complex& b) { return Complex(a.toDouble()) / b; }
        friend Complex operator/(const Complex& a, const BigInt& b) { return a / Complex(b.toDouble()); }

        // =================================================================================
        // 工业级数论算法库 (Number Theory) 
        // =================================================================================

        static BigInt gcd(BigInt a, BigInt b) {
            a = a.abs(); b = b.abs();
            auto gcd_int = [](int64_t x, int64_t y) {
                while (y != 0) { int64_t t = y; y = x % y; x = t; }
                return x;
            };
            while (!b.isZero()) {
                if (b.data.size() == 1) {
                    uint32_t small_b = b.data[0];
                    if (small_b == 0) return a;
                    uint32_t rem = a.divmod_small(small_b).second;
                    return BigInt(gcd_int(small_b, rem));
                }
                BigInt temp = b; b = a % b; a = temp;
            }
            return a;
        }

        static BigInt lcm(const BigInt& a, const BigInt& b) {
            if (a.isZero() || b.isZero()) return BigInt(0);
            return (a.abs() / gcd(a, b)) * b.abs();
        }

    private:
        static BigInt factorialRange(int64_t left, int64_t right) {
            if (left > right) return BigInt(1);
            if (left == right) return BigInt(left);
            if (right - left == 1) return BigInt(left) * BigInt(right);
            int64_t mid = left + (right - left) / 2;
            return factorialRange(left, mid) * factorialRange(mid + 1, right);
        }

    public:
        static BigInt factorial(int64_t n) {
            if (n < 0) throw std::runtime_error("Math Error: Factorial undefined for negative numbers.");
            if (n == 0 || n == 1) return BigInt(1);
            return factorialRange(2, n);
        }

        static BigInt fibonacci(int64_t n) {
            if (n < 0) throw std::runtime_error("Math Error: Fibonacci undefined for negative.");
            if (n == 0) return BigInt(0);
            if (n == 1) return BigInt(1);
            BigInt a(0); 
            BigInt b(1); 
            int msb = 62;
            while (msb >= 0 && !((n >> msb) & 1)) msb--;          
            for (int i = msb; i >= 0; --i) {
                BigInt c = a * (b * BigInt(2) - a);
                BigInt d = (a * a) + (b * b);
                if ((n >> i) & 1) {
                    a = d;          
                    b = c + d;      
                }
                else {
                    a = c;         
                    b = d;          
                }
            }
            return a;
        }

        static BigInt modPow(BigInt base, BigInt exp, const BigInt& mod) {
            if (mod == BigInt(1)) return BigInt(0);
            if (mod.isNegative()) throw std::runtime_error("Math Error: Modulus must be positive.");

            BigInt result(1);
            base = mathMod(base, mod);

            while (!exp.isZero()) {
                if (exp.data[0] & 1)
                    result = mathMod(result * base, mod);
                
                // 原地除以 2，避免每次循环创建新的 BigInt 对象
                uint32_t rem = 0;
                for (int i = static_cast<int>(exp.data.size()) - 1; i >= 0; --i) {
                    uint64_t cur = (static_cast<uint64_t>(rem) << 32) | static_cast<uint64_t>(exp.data[i]);
                    exp.data[i] = static_cast<uint32_t>(cur >> 1);
                    rem = static_cast<uint32_t>(cur & 1);
                }
                exp.trim();

                if (!exp.isZero()) {
                    base = mathMod(base * base, mod);
                }
            }
            return result;
        }

        bool isPrime() const {
            BigInt n = this->abs();
            if (n < BigInt(2)) return false;
            if (n == BigInt(2) || n == BigInt(3) || n == BigInt(5) || n == BigInt(7)) return true;
            if (n.data[0] % 2 == 0) return false;

            // =========================================================
            // [极速外存探针]
            // =========================================================
            if (fileIndexed && n <= BigInt(largestPrimeInFile)) {
                int64_t val = -1;
                try { val = n.toInt64(); }
                catch (...) { /* 降级 */ }

                if (val >= 0) {
                    auto it = std::upper_bound(primeFileIndex.begin(), primeFileIndex.end(), val,
                        [](int64_t v, const PrimeIndex& anchor) { return v < anchor.primeValue; });
                    if (it == primeFileIndex.begin()) it = primeFileIndex.begin();
                    else --it;
                    std::ifstream file(getPrimeFilePath(), std::ios::binary);
                    file.seekg(it->offset);
                    int64_t currentIdx = it->indexNumber;
                    std::string line;
                    while (currentIdx <= totalPrimesInFile && std::getline(file, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line.empty()) continue;
                        int64_t p = std::stoll(line);
                        if (p == val) return true;
                        if (p > val) return false;
                        currentIdx++;
                    }
                    return false;
                }
            }

            // =========================================================
            // [动态降级]：Miller-Rabin
            // =========================================================
            BigInt d = n - BigInt(1);
            int r = 0;
            while (d.data[0] % 2 == 0) { d = d.divmod_small(2).first; r++; }
            
            // 前 12 个素数作为基，在数学上已证明对 3.18 * 10^24 (约 81 bits) 以内的数是 100% 准确的
            std::vector<BigInt> witnesses;
            for (int64_t p : { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37 }) {
                witnesses.push_back(BigInt(p));
            }
            
            BigInt nMinus1 = n - BigInt(1);
            
            // 如果数字超过了 81 bits (data.size() >= 3，因为 10^27 > 2^81)，
            // 我们追加 28 轮随机基测试，总计 40 轮，将误判率降低到密码学安全级别 (4^-40)
            if (n.data.size() >= 3) {
                uint64_t state = 0x123456789ABCDEFULL ^ static_cast<uint64_t>(n.data[0]);
                for (int k = 0; k < 28; ++k) {
                    BigInt randBase;
                    randBase.data.resize(n.data.size());
                    for (size_t i = 0; i < randBase.data.size(); ++i) {
                        state ^= state >> 12;
                        state ^= state << 25;
                        state ^= state >> 27;
                        randBase.data[i] = static_cast<uint32_t>(state * 2685821657736338717ULL);
                    }
                    randBase.trim();
                    if (randBase < BigInt(2)) randBase = randBase + BigInt(2);
                    if (randBase >= nMinus1) randBase = randBase % (nMinus1 - BigInt(2)) + BigInt(2);
                    witnesses.push_back(randBase);
                }
            }

            for (const BigInt& aBI : witnesses) {
                if (aBI >= n) continue;
                BigInt x = modPow(aBI, d, n);
                if (x == BigInt(1) || x == nMinus1) continue;
                bool found = false;
                for (int i = 0; i < r - 1; ++i) {
                    x = (x * x) % n;
                    if (x == nMinus1) { found = true; break; }
                }
                if (!found) return false;
            }
            return true;
        }

        BigInt nextPrime() const {
            BigInt n = this->abs();
            if (n < BigInt(2)) return BigInt(2);

            // =========================================================
            // [极速外存探针]
            // =========================================================
            if (fileIndexed && n < BigInt(largestPrimeInFile)) {
                int64_t val = -1;
                try { val = n.toInt64(); }
                catch (...) { /* 降级 */ }

                if (val >= 0) {
                    auto it = std::upper_bound(primeFileIndex.begin(), primeFileIndex.end(), val,
                        [](int64_t v, const PrimeIndex& anchor) { return v < anchor.primeValue; });
                    if (it == primeFileIndex.begin()) it = primeFileIndex.begin();
                    else --it;

                    std::ifstream file(getPrimeFilePath(), std::ios::binary);
                    file.seekg(it->offset);

                    int64_t currentIdx = it->indexNumber;
                    std::string line;
                    while (currentIdx <= totalPrimesInFile && std::getline(file, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line.empty()) continue;
                        int64_t p = std::stoll(line);
                        if (p > val) return BigInt(p);
                        currentIdx++;
                    }
                }
            }

            // =========================================================
            // [动态漫游寻找]
            // =========================================================
            BigInt candidate = n;
            if (candidate.data[0] % 2 == 0) candidate = candidate + BigInt(1);
            else candidate = candidate + BigInt(2);

            while (!candidate.isPrime()) {
                candidate = candidate + BigInt(2);
            }
            return candidate;
        }

        // --- 极速单向流读取版 (带堆内存防栈溢出缓冲) ---
        static BigInt nthPrime(int64_t n) {
            if (n < 1) throw std::runtime_error("Math Error: nthPrime requires n >= 1.");
            // 索引加速空降
            if (fileIndexed && n <= totalPrimesInFile) {
                int64_t p = getPrimeAt(n - 1);
                if (p >= 2) return BigInt(p);
                // ★ 空降失败，不直接报错，降级到流式扫描继续尝试
            }

            int64_t count = 0;
            int64_t lastP = 0;
            std::string filepath = getPrimeFilePath();

            if (!filepath.empty()) {
                std::ifstream file(filepath, std::ios::binary);
                if (file.is_open()) {
                constexpr size_t BUFFER_SIZE = 65536;
                std::vector<char> buffer(BUFFER_SIZE); // 使用堆内存防溢出
                int64_t currentPrime = 0;
                bool readingNumber = false;
                bool done = false;

                while (!done && (file.read(buffer.data(), BUFFER_SIZE) || file.gcount() > 0)) {
                    size_t bytesRead = static_cast<size_t>(file.gcount());
                    for (size_t i = 0; i < bytesRead; ++i) {
                        char c = buffer[i];
                        if (c >= '0' && c <= '9') {
                            currentPrime = currentPrime * 10 + (c - '0');
                            readingNumber = true;
                        }
                        else if (c == '\n' || c == '\r') {
                            if (readingNumber) {
                                count++;
                                lastP = currentPrime;
                                currentPrime = 0;
                                readingNumber = false;

                                if (count == n) {
                                    file.close();
                                    return BigInt(lastP);
                                }
                            }
                        }
                    }
                }
                    if (!done && readingNumber) {
                        count++;
                        lastP = currentPrime;
                        if (count == n) { file.close(); return BigInt(lastP); }
                    }
                    file.close();
                }
            }

            BigInt candidate = count > 0 ? (lastP == 2 ? BigInt(3) : BigInt(lastP) + BigInt(2)) : BigInt(3);
            if (count == 0) {
                if (n == 1) return BigInt(2);
                count = 1;
            }

            while (count < n) {
                if (candidate.isPrime()) count++;
                if (count < n) candidate = candidate + BigInt(2);
            }
            return candidate;
        }

        int64_t primePi() const {
            BigInt nBI = this->abs();
            if (nBI < BigInt(2)) return 0;

            int64_t count = 0;
            int64_t lastP = 0;

            // ★ 精确转换，失败则跳过文件扫描
            int64_t n = -1;
            try { n = nBI.toInt64(); }
            catch (...) { /* 超出 int64 范围 */ }

            if (n >= 2) {
                std::string filepath = getPrimeFilePath();
                if (!filepath.empty()) {
                    std::ifstream file(filepath, std::ios::binary);
                    if (file.is_open()) {
                    constexpr size_t BUFFER_SIZE = 65536;
                    std::vector<char> buffer(BUFFER_SIZE);
                    int64_t currentPrime = 0;
                    bool readingNumber = false;
                    bool done = false;

                    while (!done && (file.read(buffer.data(), BUFFER_SIZE) || file.gcount() > 0)) {
                        size_t bytesRead = static_cast<size_t>(file.gcount());
                        for (size_t i = 0; i < bytesRead; ++i) {
                            char c = buffer[i];
                            if (c >= '0' && c <= '9') {
                                currentPrime = currentPrime * 10 + (c - '0');
                                readingNumber = true;
                            }
                            else if (c == '\n' || c == '\r') {
                                if (readingNumber) {
                                    if (currentPrime > n) { done = true; break; }
                                    count++;
                                    lastP = currentPrime;
                                    currentPrime = 0;
                                    readingNumber = false;
                                }
                            }
                        }
                    }
                        if (!done && readingNumber && currentPrime <= n) {
                            count++;
                            lastP = currentPrime;
                        }
                        file.close();
                        if (done) return count;
                    }
                }
            }

            BigInt candidate = count > 0 ? (lastP == 2 ? BigInt(3) : BigInt(lastP) + BigInt(2)) : BigInt(2);
            if (count == 0) {
                if (nBI >= BigInt(2)) { count = 1; candidate = BigInt(3); }
            }
            while (candidate <= nBI) {
                if (candidate.isPrime()) count++;
                candidate = candidate + BigInt(2);
            }
            return count;
        }

    private:
        static BigInt pollardRho(const BigInt& n, int64_t c_val = 1, int64_t max_iter = -1) {
            if (n.data[0] % 2 == 0) return BigInt(2);
            if (n.data[0] % 3 == 0) return BigInt(3);
            if (n.data[0] % 5 == 0) return BigInt(5);

            BigInt x(2), y(2), d(1), c(c_val);
            BigInt prod(1);
            int64_t power = 1, lam = 1;
            
            auto f = [&](const BigInt& val) {
                return mathMod(val * val + c, n);
            };
            
            // Brent's cycle finding with GCD batching
            while (d == BigInt(1)) {
                if (power == lam) {
                    x = y;
                    power *= 2;
                    lam = 0;
                }
                y = f(y);
                lam++;
                
                BigInt diff = (x > y) ? x - y : y - x;
                prod = mathMod(prod * diff, n);
                
                if (lam % 64 == 0) {
                    d = gcd(prod, n);
                    if (d != BigInt(1)) break;
                }
                if (max_iter > 0 && lam > max_iter) return BigInt(1);
            }
            
            if (d == BigInt(1)) {
                d = gcd(prod, n);
            }
            
            if (d == n) {
                if (max_iter > 0) return BigInt(1);
                return pollardRho(n, c_val + 1, max_iter);
            }
            return d;
        }

        // Lenstra Elliptic Curve Factorization (ECM)
        static BigInt ecm(const BigInt& n, uint32_t B1, uint64_t seed) {
            BigInt X(2), Z(1);
            BigInt A24(seed % 10000 + 1); // A24 = (A+2)/4
            
            auto add = [&n](const BigInt& X1, const BigInt& Z1, const BigInt& X2, const BigInt& Z2, const BigInt& X_diff, const BigInt& Z_diff) -> std::pair<BigInt, BigInt> {
                BigInt u = mathMod((X1 - Z1) * (X2 + Z2), n);
                BigInt v = mathMod((X1 + Z1) * (X2 - Z2), n);
                BigInt X_plus = mathMod(Z_diff * mathMod((u + v) * (u + v), n), n);
                BigInt Z_plus = mathMod(X_diff * mathMod((u - v) * (u - v), n), n);
                return {X_plus, Z_plus};
            };

            auto double_pt = [&n, &A24](const BigInt& X1, const BigInt& Z1) -> std::pair<BigInt, BigInt> {
                BigInt u = mathMod(X1 + Z1, n);
                BigInt v = mathMod(X1 - Z1, n);
                u = mathMod(u * u, n);
                v = mathMod(v * v, n);
                BigInt diff = mathMod(u - v, n);
                BigInt X2 = mathMod(u * v, n);
                BigInt Z2 = mathMod(diff * mathMod(v + mathMod(A24 * diff, n), n), n);
                return {X2, Z2};
            };

            auto multiply = [&add, &double_pt](uint32_t k, BigInt X_in, BigInt Z_in) -> std::pair<BigInt, BigInt> {
                if (k == 0) return {BigInt(1), BigInt(0)};
                if (k == 1) return {X_in, Z_in};
                
                std::vector<int> bits;
                uint32_t temp = k;
                while (temp > 0) {
                    bits.push_back(temp & 1);
                    temp >>= 1;
                }
                
                BigInt x0 = X_in, z0 = Z_in;
                auto [x1, z1] = double_pt(X_in, Z_in);
                
                for (int i = static_cast<int>(bits.size()) - 2; i >= 0; --i) {
                    if (bits[i] == 0) {
                        auto next_x1_z1 = add(x0, z0, x1, z1, X_in, Z_in);
                        auto next_x0_z0 = double_pt(x0, z0);
                        x1 = next_x1_z1.first; z1 = next_x1_z1.second;
                        x0 = next_x0_z0.first; z0 = next_x0_z0.second;
                    } else {
                        auto next_x0_z0 = add(x0, z0, x1, z1, X_in, Z_in);
                        auto next_x1_z1 = double_pt(x1, z1);
                        x0 = next_x0_z0.first; z0 = next_x0_z0.second;
                        x1 = next_x1_z1.first; z1 = next_x1_z1.second;
                    }
                }
                return {x0, z0};
            };

            std::vector<uint32_t> primes;
            std::vector<bool> is_p(B1 + 1, true);
            for (uint32_t p = 2; p <= B1; ++p) {
                if (is_p[p]) {
                    primes.push_back(p);
                    for (uint64_t i = static_cast<uint64_t>(p) * p; i <= B1; i += p) is_p[i] = false;
                }
            }

            for (uint32_t p : primes) {
                uint32_t q = p;
                uint32_t max_q = B1;
                while (q <= max_q / p) q *= p;
                
                auto [nX, nZ] = multiply(q, X, Z);
                X = nX; Z = nZ;
                if (Z.isZero()) break;
            }
            
            BigInt g = gcd(Z, n);
            if (g > BigInt(1) && g < n) return g;
            return BigInt(1);
        }

        static void factorizeRecursive(BigInt n, std::map<BigInt, int>& factors) {
            if (n <= BigInt(1)) return;
            if (n.isPrime()) {
                factors[n]++;
                return;
            }
            
            BigInt divisor(1);
            
            // 1. Pollard's rho (fast for small factors up to ~15 digits)
            int64_t c = 1;
            while (divisor == BigInt(1) || divisor == n) {
                divisor = pollardRho(n, c++, 131072); // Limit iterations
                if (divisor != BigInt(1) && divisor != n) break;
                if (c > 3) break; // Try 3 different polynomials
            }
            
            // 2. ECM (Lenstra Elliptic Curve Method) for larger factors
            if (divisor == BigInt(1) || divisor == n) {
                uint64_t seed = 1;
                uint32_t B1 = 2000;
                while (divisor == BigInt(1) || divisor == n) {
                    divisor = ecm(n, B1, seed++);
                    if (seed % 10 == 0) B1 *= 2; // Increase B1 every 10 curves
                    if (B1 > 250000) {
                        // Fallback to unbounded Pollard's rho if ECM takes too long
                        divisor = pollardRho(n, c++, -1); 
                    }
                }
            }
            
            factorizeRecursive(divisor, factors);
            factorizeRecursive(n / divisor, factors);
        }

    public:
        std::vector<std::pair<BigInt, int>> factorize() const {
            BigInt n = this->abs();
            if (n <= BigInt(1)) throw std::runtime_error("Math Error: Factorization requires n > 1.");

            std::vector<std::pair<BigInt, int>> factors;
            int64_t lastP = 0;

            std::string filepath = getPrimeFilePath();
            if (!filepath.empty()) {
                std::ifstream file(filepath, std::ios::binary);
                if (file.is_open()) {
                constexpr size_t BUFFER_SIZE = 65536;
                std::vector<char> buffer(BUFFER_SIZE);
                int64_t currentPrime = 0;
                bool readingNumber = false;
                bool done = false;

                while (!done && (file.read(buffer.data(), BUFFER_SIZE) || file.gcount() > 0)) {
                    size_t bytesRead = static_cast<size_t>(file.gcount());
                    for (size_t i = 0; i < bytesRead; ++i) {
                        char c = buffer[i];
                        if (c >= '0' && c <= '9') {
                            currentPrime = currentPrime * 10 + (c - '0');
                            readingNumber = true;
                        }
                        else if (c == '\n' || c == '\r') {
                            if (readingNumber) {
                                int64_t p = currentPrime;
                                lastP = currentPrime;
                                currentPrime = 0;
                                readingNumber = false;

                                BigInt pBI(p);
                                if (pBI * pBI > n) { done = true; break; }

                                int count = 0;
                                while (true) {
                                    auto [q, rem] = divmod(n, pBI);
                                    if (!rem.isZero()) break;
                                    n = q;
                                    count++;
                                }
                                if (count > 0) factors.push_back({ pBI, count });
                            }
                        }
                    }
                }
                    if (!done && readingNumber) {
                        BigInt pBI(currentPrime);
                        lastP = currentPrime;
                        if (pBI * pBI <= n) {
                            int count = 0;
                            while (true) {
                                auto [q, rem] = divmod(n, pBI);
                                if (!rem.isZero()) break;
                                n = q;
                                count++;
                            }
                            if (count > 0) factors.push_back({ pBI, count });
                        }
                    }
                    file.close();
                }
            }

            if (n > BigInt(1)) {
                if (lastP == 0) {
                    const uint32_t small_primes[] = {
                        2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71,
                        73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151,
                        157, 163, 167, 173, 179, 181, 191, 193, 197, 199, 211, 223, 227, 229, 233,
                        239, 241, 251, 257, 263, 269, 271, 277, 281, 283, 293, 307, 311, 313, 317,
                        331, 337, 347, 349, 353, 359, 367, 373, 379, 383, 389, 397, 401, 409, 419,
                        421, 431, 433, 439, 443, 449, 457, 461, 463, 467, 479, 487, 491, 499, 503,
                        509, 521, 523, 541, 547, 557, 563, 569, 571, 577, 587, 593, 599, 601, 607,
                        613, 617, 619, 631, 641, 643, 647, 653, 659, 661, 673, 677, 683, 691, 701,
                        709, 719, 727, 733, 739, 743, 751, 757, 761, 769, 773, 787, 797, 809, 811,
                        821, 823, 827, 829, 839, 853, 857, 859, 863, 877, 881, 883, 887, 907, 911,
                        919, 929, 937, 941, 947, 953, 967, 971, 977, 983, 991, 997
                    };
                    for (uint32_t p : small_primes) {
                        if (n < BigInt(static_cast<int64_t>(p) * p)) {
                            if (n > BigInt(1)) {
                                factors.push_back({ n, 1 });
                                n = BigInt(1);
                            }
                            break;
                        }
                        int count = 0;
                        while (true) {
                            auto [q, rem] = n.divmod_small(p);
                            if (rem != 0) break;
                            n = q;
                            count++;
                        }
                        if (count > 0) factors.push_back({ BigInt(p), count });
                    }
                }

                std::map<BigInt, int> remainingFactors;
                factorizeRecursive(n, remainingFactors);
                for (const auto& [p, count] : remainingFactors) {
                    factors.push_back({ p, count });
                }

                std::sort(factors.begin(), factors.end(), [](const auto& a, const auto& b) {
                    return a.first < b.first;
                });

                std::vector<std::pair<BigInt, int>> mergedFactors;
                for (const auto& f : factors) {
                    if (!mergedFactors.empty() && mergedFactors.back().first == f.first) {
                        mergedFactors.back().second += f.second;
                    } else {
                        mergedFactors.push_back(f);
                    }
                }
                return mergedFactors;
            }
            return factors;
        }

        BigInt eulerPhi() const {
            BigInt n = this->abs();
            if (n <= BigInt(0)) throw std::runtime_error("Math Error: n > 0 required.");
            if (n == BigInt(1)) return BigInt(1);
            auto factors = n.factorize();
            BigInt result = n;
            for (const auto& [p, exp] : factors) result = result / p * (p - BigInt(1));
            return result;
        }

        BigInt divisorCount() const {
            BigInt n = this->abs();
            if (n <= BigInt(0)) throw std::runtime_error("Math Error: n > 0 required.");
            if (n == BigInt(1)) return BigInt(1);
            auto factors = n.factorize();
            BigInt result(1);
            for (const auto& [p, exp] : factors) result = result * BigInt(exp + 1);
            return result;
        }

        BigInt divisorSum(int64_t k = 1) const {
            BigInt n = this->abs();
            if (n <= BigInt(0)) throw std::runtime_error("Math Error: n > 0 required.");
            if (n == BigInt(1)) return BigInt(1);
            auto factors = n.factorize();
            BigInt result(1);
            for (const auto& [p, exp] : factors) {
                BigInt sum(0), pk(1);
                BigInt p_to_k = p.pow(k);
                for (int j = 0; j <= exp; ++j) {
                    sum = sum + pk;
                    pk = pk * p_to_k;
                }
                result = result * sum;
            }
            return result;
        }

        int omega() const { return static_cast<int>(this->abs().factorize().size()); }

        int bigOmega() const {
            auto factors = this->abs().factorize();
            int total = 0;
            for (const auto& [p, exp] : factors) total += exp;
            return total;
        }

        int mobius() const {
            BigInt n = this->abs();
            if (n == BigInt(1)) return 1;
            auto factors = n.factorize();
            for (const auto& [p, exp] : factors) if (exp > 1) return 0;
            return (factors.size() % 2 == 0) ? 1 : -1;
        }

        bool isPerfect() const {
            BigInt n = this->abs();
            if (n <= BigInt(1)) return false;
            return divisorSum(1) == n * BigInt(2);
        }

        // ====== 打印重载 ======
        friend std::ostream& operator<<(std::ostream& os, const BigInt& b) {
            os << b.toString();
            return os;
        }
    };

} // namespace jc
#endif // JC2_BIGINT_H
