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
#include "../vm/EngineInterrupt.h"

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
        // 分块差分编码外存引擎 (Block-Differential Streaming Engine)
        // =================================================================================
        inline static bool fileIndexed = false;
        inline static int64_t totalPrimesInFile = 0;
        inline static int64_t largestPrimeInFile = 0;
        inline static std::vector<uint64_t> blockAnchors; // ★ 内存级块首质数索引 (仅占极小内存)
        static constexpr int PRIMES_PER_BLOCK = 4093;
        static constexpr int BLOCK_BYTES = 8192;

        struct PrimeHeader {
            char magic[4];
            uint32_t reserved;
            uint64_t totalPrimes;
            uint64_t largestPrime;
        };

        inline static std::string customPrimePath = "";
        static std::string getPrimeFilePath() {
            return customPrimePath;
        }

        // --- 外部更换挂载路径接口 ---
        static void setPrimeFilePath(const std::string& newPath) {
            if (newPath == "default") customPrimePath = "";
            else customPrimePath = newPath;
            
            fileIndexed = false;
            totalPrimesInFile = 0;
            largestPrimeInFile = 0;
            blockAnchors.clear();

            if (customPrimePath.empty()) {
                std::cout << "[System] Prime engine remounted to dynamic computation." << std::endl;
            } else {
                std::cout << "[System] Prime engine remounted to: " << getPrimeFilePath() << std::endl;
                buildFileIndex();
            }
        }

        // --- 极速扫描建立文件索引 (JCP1 差分格式) ---
        static void buildFileIndex() {
            if (fileIndexed) return;

            std::string filepath = getPrimeFilePath();
            if (filepath.empty()) return;

            std::ifstream file(filepath, std::ios::binary);
            if (!file.is_open()) {
                fileIndexed = true;
                std::cout << "[System] Notice: Prime table not found at " << filepath << ". Using dynamic computation. Call extendPrimes(N) to generate it." << std::endl;
                return;
            }

            PrimeHeader header = {};
            if (file.read(reinterpret_cast<char*>(&header), 24) && 
                header.magic[0] == 'J' && header.magic[1] == 'C' && 
                header.magic[2] == 'P' && header.magic[3] == '1') {
                totalPrimesInFile = header.totalPrimes;
                largestPrimeInFile = header.largestPrime;
                
                int64_t totalBlocks = (totalPrimesInFile + PRIMES_PER_BLOCK - 1) / PRIMES_PER_BLOCK;
                blockAnchors.resize(totalBlocks);
                for (int64_t i = 0; i < totalBlocks; ++i) {
                    file.seekg(24 + i * BLOCK_BYTES, std::ios::beg);
                    file.read(reinterpret_cast<char*>(&blockAnchors[i]), 8);
                }
                
                if (totalPrimesInFile > 0) {
                    std::cout << "[System] Successfully mounted JCP1 diff-encoded prime table: " << totalPrimesInFile << " primes." << std::endl;
                }
            } else {
                totalPrimesInFile = 0;
                largestPrimeInFile = 0;
                std::cout << "[System] Warning: Invalid or old prime table format. Please rebuild." << std::endl;
            }

            fileIndexed = true;
            file.close();
        }

        // --- O(1) 块级空降与差分解码 ---
        static int64_t getPrimeAt(int64_t index) {
            if (!fileIndexed || index < 0 || index >= totalPrimesInFile) return -1;
            std::ifstream file(getPrimeFilePath(), std::ios::binary);
            if (!file) return -1;
            
            int64_t blockIdx = index / PRIMES_PER_BLOCK;
            int offset = index % PRIMES_PER_BLOCK;
            
            file.seekg(24 + blockIdx * BLOCK_BYTES, std::ios::beg);
            uint64_t basePrime = 0;
            file.read(reinterpret_cast<char*>(&basePrime), 8);
            
            if (offset == 0) return basePrime;
            
            std::vector<uint16_t> gaps(offset);
            file.read(reinterpret_cast<char*>(gaps.data()), offset * 2);
            
            uint64_t p = basePrime;
            for (int i = 0; i < offset; ++i) p += gaps[i];
            return p;
        }

        static bool isPrimeFast(uint64_t n) {
            if (n < 2) return false;
            if (n == 2 || n == 3 || n == 5 || n == 7) return true;
            if (n % 2 == 0 || n % 3 == 0 || n % 5 == 0) return false;
            
            if (n < 4294967295ULL) {
                uint64_t d = n - 1;
                int s = 0;
                while ((d & 1) == 0) { d >>= 1; s++; }
                
                uint64_t bases[] = {2, 7, 61};
                for (uint64_t a : bases) {
                    if (n <= a) break;
                    uint64_t res = 1;
                    uint64_t base = a;
                    uint64_t exp = d;
                    while (exp > 0) {
                        if (exp & 1) res = (res * base) % n;
                        base = (base * base) % n;
                        exp >>= 1;
                    }
                    uint64_t x = res;
                    if (x == 1 || x == n - 1) continue;
                    bool composite = true;
                    for (int r = 1; r < s; ++r) {
                        x = (x * x) % n;
                        if (x == n - 1) { composite = false; break; }
                    }
                    if (composite) return false;
                }
                return true;
            }
            return BigInt(static_cast<int64_t>(n)).isPrime();
        }

        // --- 扩展质数表 (JCP1 差分编码 + 极速分段筛法) ---
        static void extendPrimeTable(int64_t count) {
            if (customPrimePath.empty()) {
                throw std::runtime_error("IO Error: No prime table mounted. Use mountPrimes() first.");
            }
            if (!fileIndexed) buildFileIndex();
            
            std::fstream file(customPrimePath, std::ios::binary | std::ios::in | std::ios::out);
            bool valid = false;
            PrimeHeader header = {};
            if (file.is_open()) {
                if (file.read(reinterpret_cast<char*>(&header), 24) && 
                    header.magic[0] == 'J' && header.magic[1] == 'C' && 
                    header.magic[2] == 'P' && header.magic[3] == '1') {
                    valid = true;
                }
            }
            if (!valid) {
                file.close();
                std::ofstream out(customPrimePath, std::ios::binary);
                header = {{'J', 'C', 'P', '1'}, 0, 0, 0};
                out.write(reinterpret_cast<char*>(&header), 24);
                out.close();
                file.open(customPrimePath, std::ios::binary | std::ios::in | std::ios::out);
            }
            
            int64_t currentTotal = header.totalPrimes;
            uint64_t currentP_val = currentTotal > 0 ? header.largestPrime : 1;
            uint64_t lastP_val = currentP_val;
            
            std::vector<char> blockBuf(BLOCK_BYTES, 0);
            int primesInLastBlock = 0;
            int64_t lastBlockIdx = 0;
            
            if (currentTotal > 0) {
                lastBlockIdx = (currentTotal - 1) / PRIMES_PER_BLOCK;
                primesInLastBlock = (currentTotal - 1) % PRIMES_PER_BLOCK + 1;
                file.seekg(24 + lastBlockIdx * BLOCK_BYTES, std::ios::beg);
                file.read(blockBuf.data(), BLOCK_BYTES);
            }
            
            file.seekp(24 + lastBlockIdx * BLOCK_BYTES, std::ios::beg);

            // =========================================================
            // 极速分段筛法 (Segmented Sieve of Eratosthenes)
            // =========================================================
            uint32_t max_sqrt = 1000000; // 覆盖到 10^12，足以生成 370 亿个质数
            std::vector<uint32_t> base_primes;
            std::vector<bool> is_p(max_sqrt + 1, true);
            for (uint32_t p = 2; p * p <= max_sqrt; ++p) {
                if (is_p[p]) {
                    for (uint32_t i = p * p; i <= max_sqrt; i += p) is_p[i] = false;
                }
            }
            for (uint32_t p = 2; p <= max_sqrt; ++p) {
                if (is_p[p]) base_primes.push_back(p);
            }

            const uint64_t S = 262144; // 256KB L2 Cache 友好
            std::vector<uint8_t> sieve(S);
            uint64_t low = currentP_val + 1;
            
            int64_t primes_found = 0;

            auto add_prime = [&](uint64_t p_val) {
                if (primesInLastBlock == 0) {
                    uint64_t base = p_val;
                    std::memcpy(blockBuf.data(), &base, 8);
                    std::memset(blockBuf.data() + 8, 0, BLOCK_BYTES - 8);
                    primesInLastBlock = 1;
                    blockAnchors.push_back(base);
                } else {
                    uint64_t gap = p_val - lastP_val;
                    if (gap > 65535) throw std::runtime_error("Math Error: Prime gap exceeds 65535. Differential encoding failed.");
                    uint16_t gap16 = static_cast<uint16_t>(gap);
                    std::memcpy(blockBuf.data() + 8 + (primesInLastBlock - 1) * 2, &gap16, 2);
                    primesInLastBlock++;
                }
                
                if (primesInLastBlock == PRIMES_PER_BLOCK) {
                    file.write(blockBuf.data(), BLOCK_BYTES);
                    primesInLastBlock = 0;
                    lastBlockIdx++;
                }
                
                lastP_val = p_val;
                currentTotal++;
                primes_found++;
            };

            if (low <= 2 && count > 0) {
                add_prime(2);
                currentP_val = 2;
                low = 3;
            }
            if (low % 2 == 0) low++;

            while (primes_found < count) {
                std::fill(sieve.begin(), sieve.end(), static_cast<uint8_t>(1));
                uint64_t high = low + S * 2 - 2;
                
                for (uint32_t p : base_primes) {
                    if (p == 2) continue;
                    uint64_t p2 = static_cast<uint64_t>(p) * p;
                    if (p2 > high) break;
                    
                    uint64_t start = (low / p) * p;
                    if (start < low) start += p;
                    if (start == p) start += p;
                    if (start % 2 == 0) start += p;
                    
                    for (uint64_t j = start; j <= high; j += p * 2) {
                        sieve[(j - low) / 2] = 0;
                    }
                }
                
                for (uint64_t i = 0; i < S && primes_found < count; ++i) {
                    if (sieve[i]) {
                        uint64_t p = low + i * 2;
                        add_prime(p);
                        currentP_val = p;
                    }
                }
                low += S * 2;
            }
            
            if (primesInLastBlock > 0) {
                file.write(blockBuf.data(), BLOCK_BYTES);
            }
            
            header.totalPrimes = currentTotal;
            header.largestPrime = currentP_val;
            file.seekp(0, std::ios::beg);
            file.write(reinterpret_cast<char*>(&header), 24);
            file.close();
            
            totalPrimesInFile = header.totalPrimes;
            largestPrimeInFile = header.largestPrime;
            std::cout << "[System] Extended prime table by " << count << " primes. New total: " << totalPrimesInFile << std::endl;
        }

        // --- 转换旧版 TXT 质数表为 JCP1 格式 ---
        static void convertTxtToJCP1(const std::string& txtPath, const std::string& binPath) {
            std::ifstream in(txtPath);
            if (!in.is_open()) throw std::runtime_error("IO Error: Cannot open source txt file '" + txtPath + "'.");
            
            std::ofstream out(binPath, std::ios::binary);
            if (!out.is_open()) throw std::runtime_error("IO Error: Cannot create target bin file '" + binPath + "'.");
            
            PrimeHeader header = {{'J', 'C', 'P', '1'}, 0, 0, 0};
            out.write(reinterpret_cast<char*>(&header), 24);
            
            std::vector<char> blockBuf(BLOCK_BYTES, 0);
            int primesInBlock = 0;
            uint64_t currentTotal = 0;
            uint64_t lastP = 0;
            
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                
                uint64_t p = 0;
                try { p = std::stoull(line); } catch (...) { continue; }
                
                if (primesInBlock == 0) {
                    std::memcpy(blockBuf.data(), &p, 8);
                    std::memset(blockBuf.data() + 8, 0, BLOCK_BYTES - 8);
                    primesInBlock = 1;
                } else {
                    uint64_t gap = p - lastP;
                    if (gap > 65535) throw std::runtime_error("Math Error: Prime gap exceeds 65535. Differential encoding failed.");
                    uint16_t gap16 = static_cast<uint16_t>(gap);
                    std::memcpy(blockBuf.data() + 8 + (primesInBlock - 1) * 2, &gap16, 2);
                    primesInBlock++;
                }
                
                lastP = p;
                currentTotal++;
                
                if (primesInBlock == PRIMES_PER_BLOCK) {
                    out.write(blockBuf.data(), BLOCK_BYTES);
                    primesInBlock = 0;
                }
            }
            
            if (primesInBlock > 0) {
                out.write(blockBuf.data(), BLOCK_BYTES);
            }
            
            header.totalPrimes = currentTotal;
            header.largestPrime = lastP;
            out.seekp(0, std::ios::beg);
            out.write(reinterpret_cast<char*>(&header), 24);
            
            in.close();
            out.close();
            std::cout << "[System] Successfully converted " << currentTotal << " primes to JCP1 format: " << binPath << std::endl;
        }

        // --- 校验 JCP1 质数表完整性与准确性 ---
        static bool verifyPrimeTable() {
            if (customPrimePath.empty() || !fileIndexed) {
                throw std::runtime_error("IO Error: No prime table mounted. Use mountPrimes() first.");
            }
            std::ifstream file(customPrimePath, std::ios::binary);
            if (!file.is_open()) {
                throw std::runtime_error("IO Error: Cannot open prime table for verification.");
            }

            PrimeHeader header = {};
            if (!file.read(reinterpret_cast<char*>(&header), 24) || 
                header.magic[0] != 'J' || header.magic[1] != 'C' || 
                header.magic[2] != 'P' || header.magic[3] != '1') {
                std::cout << "[Verify] Failed: Invalid JCP1 header magic." << std::endl;
                return false;
            }

            uint64_t count = 0;
            uint64_t lastP = 0;
            int64_t totalBlocks = (header.totalPrimes + PRIMES_PER_BLOCK - 1) / PRIMES_PER_BLOCK;
            std::vector<char> blockBuf(BLOCK_BYTES);

            std::cout << "[Verify] Starting verification of " << header.totalPrimes << " primes..." << std::endl;

            for (int64_t b = 0; b < totalBlocks; ++b) {
                jc::checkInterrupt();
                if (!file.read(blockBuf.data(), BLOCK_BYTES)) {
                    std::cout << "[Verify] Failed: Unexpected EOF at block " << b << "." << std::endl;
                    return false;
                }

                uint64_t p = 0;
                std::memcpy(&p, blockBuf.data(), 8);

                if (b > 0 && p <= lastP) {
                    std::cout << "[Verify] Failed: Primes not strictly increasing at block " << b << ". Last: " << lastP << ", Current: " << p << std::endl;
                    return false;
                }
                if (!isPrimeFast(p)) {
                    std::cout << "[Verify] Failed: Invalid prime " << p << " at block " << b << " (BasePrime)." << std::endl;
                    return false;
                }

                lastP = p;
                count++;

                int primesInThisBlock = (b == totalBlocks - 1) ? 
                    ((header.totalPrimes - 1) % PRIMES_PER_BLOCK + 1) : PRIMES_PER_BLOCK;

                for (int i = 0; i < primesInThisBlock - 1; ++i) {
                    uint16_t gap = 0;
                    std::memcpy(&gap, blockBuf.data() + 8 + i * 2, 2);
                    
                    if (gap == 0) {
                        std::cout << "[Verify] Failed: Zero gap found after prime " << p << " at block " << b << "." << std::endl;
                        return false;
                    }
                    
                    p += gap;
                    if (!isPrimeFast(p)) {
                        std::cout << "[Verify] Failed: Invalid prime " << p << " at block " << b << ", offset " << i + 1 << "." << std::endl;
                        return false;
                    }
                    if (p <= lastP) {
                        std::cout << "[Verify] Failed: Primes not strictly increasing at block " << b << ", offset " << i + 1 << "." << std::endl;
                        return false;
                    }
                    lastP = p;
                    count++;
                }
                
                if (count % 10000000 == 0) {
                    std::cout << "[Verify] Checked " << count << " primes..." << std::endl;
                }
            }

            if (count != header.totalPrimes) {
                std::cout << "[Verify] Failed: Count mismatch. Header: " << header.totalPrimes << ", Actual: " << count << std::endl;
                return false;
            }
            if (lastP != header.largestPrime) {
                std::cout << "[Verify] Failed: Largest prime mismatch. Header: " << header.largestPrime << ", Actual: " << lastP << std::endl;
                return false;
            }

            std::cout << "[Verify] Success: All " << count << " primes are valid and strictly increasing. Largest: " << lastP << std::endl;
            return true;
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
            // [极速外存探针] (内存索引 + 单次块读取)
            // =========================================================
            if (fileIndexed && n <= BigInt(largestPrimeInFile)) {
                int64_t val = -1;
                try { val = n.toInt64(); }
                catch (...) { /* 降级 */ }

                if (val >= 0) {
                    uint64_t uval = static_cast<uint64_t>(val);
                    auto it = std::upper_bound(blockAnchors.begin(), blockAnchors.end(), uval);
                    if (it != blockAnchors.begin()) {
                        int64_t targetBlock = std::distance(blockAnchors.begin(), it) - 1;
                        
                        std::ifstream file(getPrimeFilePath(), std::ios::binary);
                        file.seekg(24 + targetBlock * BLOCK_BYTES, std::ios::beg);
                        char blockBuf[BLOCK_BYTES];
                        file.read(blockBuf, BLOCK_BYTES);
                        uint64_t p = 0;
                        std::memcpy(&p, blockBuf, 8);
                        
                        if (p == uval) return true;
                        
                        int64_t totalBlocks = blockAnchors.size();
                        int primesInThisBlock = (targetBlock == totalBlocks - 1) ? 
                            ((totalPrimesInFile - 1) % PRIMES_PER_BLOCK + 1) : PRIMES_PER_BLOCK;
                            
                        for (int i = 0; i < primesInThisBlock - 1; ++i) {
                            uint16_t gap = 0;
                            std::memcpy(&gap, blockBuf + 8 + i * 2, 2);
                            p += gap;
                            if (p == uval) return true;
                            if (p > uval) return false;
                        }
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
            // [极速外存探针] (内存索引 + 单次块读取)
            // =========================================================
            if (fileIndexed && n < BigInt(largestPrimeInFile)) {
                int64_t val = -1;
                try { val = n.toInt64(); }
                catch (...) { /* 降级 */ }

                if (val >= 0) {
                    uint64_t uval = static_cast<uint64_t>(val);
                    auto it = std::upper_bound(blockAnchors.begin(), blockAnchors.end(), uval);
                    int64_t targetBlock = (it == blockAnchors.begin()) ? 0 : std::distance(blockAnchors.begin(), it) - 1;
                    
                    int64_t totalBlocks = blockAnchors.size();
                    std::ifstream file(getPrimeFilePath(), std::ios::binary);
                    
                    for (int64_t b = targetBlock; b < totalBlocks; ++b) {
                        file.seekg(24 + b * BLOCK_BYTES, std::ios::beg);
                        char blockBuf[BLOCK_BYTES];
                        file.read(blockBuf, BLOCK_BYTES);
                        uint64_t p = 0;
                        std::memcpy(&p, blockBuf, 8);
                        
                        if (p > uval) return BigInt(p);
                        
                        int primesInThisBlock = (b == totalBlocks - 1) ? 
                            ((totalPrimesInFile - 1) % PRIMES_PER_BLOCK + 1) : PRIMES_PER_BLOCK;
                            
                        for (int i = 0; i < primesInThisBlock - 1; ++i) {
                            uint16_t gap = 0;
                            std::memcpy(&gap, blockBuf + 8 + i * 2, 2);
                            p += gap;
                            if (p > uval) return BigInt(p);
                        }
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

        // --- O(1) 索引空降与动态接力 ---
        static BigInt nthPrime(int64_t n) {
            if (n < 1) throw std::runtime_error("Math Error: nthPrime requires n >= 1.");
            
            if (fileIndexed && n <= totalPrimesInFile) {
                int64_t p = getPrimeAt(n - 1);
                if (p >= 2) return BigInt(p);
            }

            int64_t count = fileIndexed ? totalPrimesInFile : 0;
            int64_t lastP = fileIndexed ? largestPrimeInFile : 0;

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

            int64_t n = -1;
            try { n = nBI.toInt64(); }
            catch (...) { /* 超出 int64 范围 */ }

            if (n >= 2 && fileIndexed) {
                if (n >= largestPrimeInFile) {
                    count = totalPrimesInFile;
                    lastP = largestPrimeInFile;
                } else {
                    uint64_t un = static_cast<uint64_t>(n);
                    auto it = std::upper_bound(blockAnchors.begin(), blockAnchors.end(), un);
                    int64_t targetBlock = (it == blockAnchors.begin()) ? 0 : std::distance(blockAnchors.begin(), it) - 1;
                    
                    std::ifstream file(getPrimeFilePath(), std::ios::binary);
                    file.seekg(24 + targetBlock * BLOCK_BYTES, std::ios::beg);
                    char blockBuf[BLOCK_BYTES];
                    file.read(blockBuf, BLOCK_BYTES);
                    uint64_t p = 0;
                    std::memcpy(&p, blockBuf, 8);
                    
                    int64_t ans = targetBlock * PRIMES_PER_BLOCK;
                    if (p <= un) ans++;
                    
                    int64_t totalBlocks = blockAnchors.size();
                    int primesInThisBlock = (targetBlock == totalBlocks - 1) ? 
                        ((totalPrimesInFile - 1) % PRIMES_PER_BLOCK + 1) : PRIMES_PER_BLOCK;
                        
                    for (int i = 0; i < primesInThisBlock - 1; ++i) {
                        uint16_t gap = 0;
                        std::memcpy(&gap, blockBuf + 8 + i * 2, 2);
                        p += gap;
                        if (p <= un) ans++;
                        else break;
                    }
                    return ans;
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
            if (!filepath.empty() && fileIndexed) {
                std::ifstream file(filepath, std::ios::binary);
                if (file.is_open()) {
                    file.seekg(24, std::ios::beg);
                    char blockBuf[BLOCK_BYTES];
                    bool done = false;
                    int64_t primesRead = 0;
                    
                    while (!done && primesRead < totalPrimesInFile && file.read(blockBuf, BLOCK_BYTES)) {
                        uint64_t p = 0;
                        std::memcpy(&p, blockBuf, 8);
                        
                        auto processPrime = [&](uint64_t primeVal) {
                            lastP = primeVal;
                            BigInt pBI(primeVal);
                            if (pBI * pBI > n) { done = true; return false; }
                            int count = 0;
                            while (true) {
                                auto [q, rem] = divmod(n, pBI);
                                if (!rem.isZero()) break;
                                n = q;
                                count++;
                            }
                            if (count > 0) factors.push_back({ pBI, count });
                            return true;
                        };
                        
                        if (!processPrime(p)) break;
                        primesRead++;
                        
                        int primesInThisBlock = (totalPrimesInFile - primesRead >= PRIMES_PER_BLOCK - 1) ? 
                            (PRIMES_PER_BLOCK - 1) : static_cast<int>(totalPrimesInFile - primesRead);
                            
                        for (int i = 0; i < primesInThisBlock; ++i) {
                            uint16_t gap = 0;
                            std::memcpy(&gap, blockBuf + 8 + i * 2, 2);
                            p += gap;
                            if (!processPrime(p)) break;
                            primesRead++;
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
