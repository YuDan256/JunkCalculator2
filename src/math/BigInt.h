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

// 引入复数以支持与复数的隐式混合运算提升
#include "Complex.h"

namespace jc {

    class BigInt {
    private:
        static constexpr int64_t BASE = 1000000000LL; // 10^9 压位表示法
        static constexpr int BASE_DIGITS = 9;

        std::vector<int32_t> data; // 小端序：data[0] 存最低的 9 位数字
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
            int64_t carry = 0;
            for (size_t i = 0; i < n; ++i) {
                int64_t sum = carry;
                if (i < a.data.size()) sum += a.data[i];
                if (i < b.data.size()) sum += b.data[i];
                if (sum >= BASE) {
                    sum -= BASE;
                    carry = 1;
                } else {
                    carry = 0;
                }
                result.data[i] = static_cast<int32_t>(sum);
            }
            if (carry > 0) result.data.push_back(static_cast<int32_t>(carry));
            return result;
        }

        // 绝对值减法
        static BigInt absSub(const BigInt& a, const BigInt& b) {
            BigInt result;
            result.data.resize(a.data.size(), 0);
            int64_t borrow = 0;
            for (size_t i = 0; i < a.data.size(); ++i) {
                int64_t diff = static_cast<int64_t>(a.data[i]) - borrow;
                if (i < b.data.size()) diff -= b.data[i];
                if (diff < 0) { diff += BASE; borrow = 1; }
                else borrow = 0;
                result.data[i] = static_cast<int32_t>(diff);
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
                int64_t carry = 0;
                int64_t d_i = a.data[i];
                for (size_t j = 0; j < m; ++j) {
                    int64_t prod = d_i * b.data[j] + result.data[i + j] + carry;
                    carry = prod / BASE;
                    result.data[i + j] = static_cast<int32_t>(prod - carry * BASE);
                }
                if (carry > 0) result.data[i + m] += static_cast<int32_t>(carry);
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

        // 单个 limb(块) 的除法/取模
        std::pair<BigInt, int64_t> divmod_small(int64_t divisor) const {
            if (divisor == 0) throw std::runtime_error("Math Error: Division by zero.");

            // ★ INT64_MIN 的绝对值无法用 int64_t 表示
            // 且超出短除法的安全运算范围，拒绝处理
            if (divisor == std::numeric_limits<int64_t>::min()) {
                throw std::runtime_error("Math Error: Divisor magnitude too large for fast division. Use general division.");
            }

            bool divNeg = (divisor < 0);
            uint64_t d = divNeg ? static_cast<uint64_t>(-divisor) : static_cast<uint64_t>(divisor);

            BigInt q;
            q.data.resize(data.size(), 0);
            uint64_t rem = 0;
            for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
                uint64_t cur = rem * BASE + data[i];
                q.data[i] = static_cast<int32_t>(cur / d);
                rem = cur % d;
            }
            q.negative = (negative != divNeg);
            q.trim();
            int64_t final_rem = static_cast<int64_t>(rem);
            if (negative) final_rem = -final_rem;
            return { q, final_rem };
        }

        static std::pair<BigInt, BigInt> divmod(const BigInt& a, const BigInt& b) {
            if (b.isZero()) throw std::runtime_error("Math Error: Division by zero.");

            BigInt absA = a.abs(), absB = b.abs();

            if (absA < absB) {
                return { BigInt(0), a };
            }

            // 对于小除数，走快速路径
            if (absB.data.size() == 1) {
                auto [q, r] = a.divmod_small(
                    b.negative ? -static_cast<int64_t>(b.data[0]) : b.data[0]);
                return { q, BigInt(r) };
            }

            // =============================================
            // Knuth D 算法 (In-place Long Division)
            // 避免了内层循环中频繁的 BigInt 对象创建和拷贝
            // =============================================
            int n_orig = static_cast<int>(absA.data.size());
            int m = static_cast<int>(absB.data.size());

            // 归一化因子 d，使得除数最高位 >= BASE / 2
            uint64_t d = BASE / (static_cast<uint64_t>(absB.data.back()) + 1);

            auto mul_scalar = [](const BigInt& num, uint64_t scalar) {
                if (scalar == 1) return num;
                BigInt res;
                res.data.resize(num.data.size(), 0);
                uint64_t carry = 0;
                for (size_t i = 0; i < num.data.size(); ++i) {
                    uint64_t prod = static_cast<uint64_t>(num.data[i]) * scalar + carry;
                    carry = prod / BASE;
                    res.data[i] = static_cast<int32_t>(prod - carry * BASE);
                }
                if (carry > 0) res.data.push_back(static_cast<int32_t>(carry));
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
                uint64_t num = static_cast<uint64_t>(u.data[j + m]) * BASE + u.data[j + m - 1];
                uint64_t v_m1 = v.data[m - 1];
                uint64_t q_hat = num / v_m1;
                uint64_t r_hat = num % v_m1;

                // 修正 q_hat
                if (m >= 2) {
                    uint64_t v_m2 = v.data[m - 2];
                    uint64_t u_jm2 = u.data[j + m - 2];
                    while (q_hat == BASE || q_hat * v_m2 > BASE * r_hat + u_jm2) {
                        q_hat--;
                        r_hat += v_m1;
                        if (r_hat >= BASE) break;
                    }
                } else {
                    if (q_hat == BASE) q_hat--;
                }

                if (q_hat == 0) {
                    quotient.data[j] = 0;
                    continue;
                }

                // 乘法并减去 (u[j..j+m] -= q_hat * v)
                // 优化：将乘法进位和减法借位合并为无符号运算，消除分支和有符号开销
                uint64_t carry = 0;
                for (int i = 0; i < m; ++i) {
                    uint64_t prod = q_hat * v.data[i] + carry;
                    uint64_t carry_prod = prod / BASE;
                    uint64_t p_digit = prod - carry_prod * BASE;
                    
                    if (static_cast<uint64_t>(u.data[j + i]) < p_digit) {
                        u.data[j + i] = static_cast<int32_t>(u.data[j + i] + BASE - p_digit);
                        carry = carry_prod + 1;
                    } else {
                        u.data[j + i] = static_cast<int32_t>(u.data[j + i] - p_digit);
                        carry = carry_prod;
                    }
                }
                
                bool is_borrow = static_cast<uint64_t>(u.data[j + m]) < carry;
                if (is_borrow) {
                    u.data[j + m] = static_cast<int32_t>(u.data[j + m] + BASE - carry);
                } else {
                    u.data[j + m] = static_cast<int32_t>(u.data[j + m] - carry);
                }

                quotient.data[j] = static_cast<int32_t>(q_hat);

                // 如果减多了，加回来 (极少发生)
                if (is_borrow) {
                    quotient.data[j]--;
                    uint64_t carry_add = 0;
                    for (int i = 0; i < m; ++i) {
                        uint64_t sum = static_cast<uint64_t>(u.data[j + i]) + v.data[i] + carry_add;
                        carry_add = sum / BASE;
                        u.data[j + i] = static_cast<int32_t>(sum - carry_add * BASE);
                    }
                    u.data[j + m] = static_cast<int32_t>(u.data[j + m] + carry_add - ((u.data[j + m] + carry_add >= BASE) ? BASE : 0));
                }
            }

            quotient.negative = (a.negative != b.negative);
            quotient.trim();

            // 还原余数 (r = u / d)
            BigInt remainder;
            remainder.data.resize(m, 0);
            uint64_t rem = 0;
            uint64_t ud = static_cast<uint64_t>(d);
            for (int i = m - 1; i >= 0; --i) {
                uint64_t cur = rem * BASE + u.data[i];
                remainder.data[i] = static_cast<int32_t>(cur / ud);
                rem = cur % ud;
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
                data.push_back(static_cast<int32_t>(v % BASE));
                v /= BASE;
            }
        }

        explicit BigInt(const std::string& s) {
            if (s.empty()) throw std::invalid_argument("BigInt Error: Empty string.");
            size_t start = 0;
            negative = false;
            if (s[0] == '-') { negative = true; start = 1; }
            else if (s[0] == '+') { start = 1; }
            if (start == s.size()) throw std::invalid_argument("BigInt Error: No digits found.");

            for (int i = static_cast<int>(s.size()); i > static_cast<int>(start); i -= BASE_DIGITS) {
                int from = std::max(static_cast<int>(start), i - BASE_DIGITS);
                std::string chunk = s.substr(from, i - from);
                data.push_back(std::stoi(chunk));
            }
            trim();
        }

        bool isZero() const { return data.size() == 1 && data[0] == 0; }
        bool isNegative() const { return negative; }

        double toDouble() const {
            double result = 0.0;
            for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
                result = result * static_cast<double>(BASE) + static_cast<double>(data[i]);
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

            // 如果两者都在 double 安全范围内 (34 * 9 = 306 位十进制)，直接计算
            if (n_size <= 34 && d_size <= 34) {
                return num.toDouble() / den.toDouble();
            }

            // 否则，提取最高 3 个 limb (约 27 位十进制，足以覆盖 double 的 53 bits 精度)
            auto extract = [](const BigInt& b) -> std::pair<double, int> {
                if (b.isZero()) return {0.0, 0};
                int sz = static_cast<int>(b.data.size());
                double res = 0.0;
                int start = std::max(0, sz - 3);
                for (int i = sz - 1; i >= start; --i) {
                    res = res * BASE + b.data[i];
                }
                if (b.negative) res = -res;
                return {res, start};
            };

            auto [n_val, n_exp] = extract(num);
            auto [d_val, d_exp] = extract(den);

            double ratio = n_val / d_val;
            int exp_diff = n_exp - d_exp;

            if (exp_diff != 0) {
                ratio *= std::pow(static_cast<double>(BASE), exp_diff);
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
            constexpr uint64_t UBASE = static_cast<uint64_t>(BASE);
            constexpr uint64_t LIMIT = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

            for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
                if (result > (LIMIT - static_cast<uint64_t>(data[i])) / UBASE + 1)
                    if (!(negative && i == 0 && result * UBASE + static_cast<uint64_t>(data[i])
                        == static_cast<uint64_t>(LIMIT) + 1ULL))
                        throw std::runtime_error("Overflow: BigInt too large for int64.");
                result = result * UBASE + static_cast<uint64_t>(data[i]);
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
            std::string result;
            if (negative) result += '-';
            result += std::to_string(data.back());
            for (int i = static_cast<int>(data.size()) - 2; i >= 0; --i) {
                std::ostringstream oss;
                oss << std::setw(BASE_DIGITS) << std::setfill('0') << data[i];
                result += oss.str();
            }
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
            while (!b.isZero()) { BigInt temp = b; b = a % b; a = temp; }
            return a;
        }

        static BigInt lcm(const BigInt& a, const BigInt& b) {
            if (a.isZero() || b.isZero()) return BigInt(0);
            return (a.abs() / gcd(a, b)) * b.abs();
        }

        static BigInt factorial(int64_t n) {
            if (n < 0) throw std::runtime_error("Math Error: Factorial undefined for negative numbers.");
            BigInt result(1);
            for (int64_t i = 2; i <= n; ++i) result = result * BigInt(i);
            return result;
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
                exp = exp.divmod_small(2).first;
                base = mathMod(base * base, mod);
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
            std::vector<int64_t> witnesses = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37 };
            BigInt nMinus1 = n - BigInt(1);
            for (int64_t a : witnesses) {
                BigInt aBI(a);
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
                BigInt i = lastP > 0 ? (lastP == 2 ? BigInt(3) : BigInt(lastP) + BigInt(2)) : BigInt(3);
                if (lastP == 0) {
                    int count = 0;
                    while (true) {
                        auto [q, rem] = divmod(n, BigInt(2));
                        if (!rem.isZero()) break;
                        n = q;
                        count++;
                    }
                    if (count > 0) factors.push_back({ BigInt(2), count });
                }

                while (true) {
                    BigInt sq = i * i;
                    if (sq > n) break;
                    int count = 0;
                    while (true) {
                        auto [q, rem] = divmod(n, i);
                        if (!rem.isZero()) break;
                        n = q;
                        count++;
                    }
                    if (count > 0) factors.push_back({ i, count });
                    i = i + BigInt(2);
                }
                if (n > BigInt(1)) factors.push_back({ n, 1 });
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
