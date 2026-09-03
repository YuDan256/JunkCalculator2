#include "Deflate.h"
#include <algorithm>
#include <cstring>
#include <queue>
#include <utility>

namespace jc {
namespace {

// ============================================================================
// 常量与码表（RFC 1951）
// ============================================================================
constexpr int MAX_BITS = 15;
constexpr int LITERAL_CODES = 286;   // literal/length 符号 0..285
constexpr int DISTANCE_SYMBOLS = 32; // distance 符号 0..31（30/31 为保留占位）
constexpr int END_OF_BLOCK = 256;
constexpr int CLEN_SYMBOLS = 19;

struct LengthInfo { int base; int extra; };
constexpr LengthInfo LENGTH_TABLE[29] = {
    {3,0},{4,0},{5,0},{6,0},{7,0},{8,0},{9,0},{10,0},
    {11,1},{13,1},{15,1},{17,1},
    {19,2},{23,2},{27,2},{31,2},
    {35,3},{43,3},{51,3},{59,3},
    {67,4},{83,4},{99,4},{115,4},
    {131,5},{163,5},{195,5},{227,5},
    {258,0}
};

struct DistInfo { int base; int extra; };
constexpr DistInfo DIST_TABLE[32] = {
    {1,0},{2,0},{3,0},{4,0},
    {5,1},{7,1},
    {9,2},{13,2},
    {17,3},{25,3},
    {33,4},{49,4},
    {65,5},{97,5},
    {129,6},{193,6},
    {257,7},{385,7},
    {513,8},{769,8},
    {1025,9},{1537,9},
    {2049,10},{3073,10},
    {4097,11},{6145,11},
    {8193,12},{12289,12},
    {16385,13},{24577,13},
    {0,0},{0,0}
};

constexpr int CLEN_ORDER[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

int lengthCode(int len) {
    for (int i = 0; i < 29; ++i) {
        const LengthInfo& li = LENGTH_TABLE[i];
        if (len >= li.base && len < li.base + (1 << li.extra)) return 257 + i;
    }
    return -1;
}

int distanceCode(int dist) {
    for (int i = 0; i < 30; ++i) {
        const DistInfo& di = DIST_TABLE[i];
        if (dist >= di.base && dist < di.base + (1 << di.extra)) return i;
    }
    return -1;
}

// ============================================================================
// 位流（LSB-first）
// ============================================================================
struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t bitBuf = 0;
    int bitCount = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}

    void writeBits(uint32_t value, int count) {
        if (count <= 0) return;
        uint32_t mask = (count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1);
        bitBuf |= (value & mask) << bitCount;
        bitCount += count;
        while (bitCount >= 8) {
            out.push_back(static_cast<uint8_t>(bitBuf & 0xFF));
            bitBuf >>= 8;
            bitCount -= 8;
        }
    }

    // Huffman 码字：MSB-first 逐位写
    void writeCode(uint32_t code, int len) {
        for (int i = len - 1; i >= 0; --i) writeBits((code >> i) & 1u, 1);
    }

    void align() {
        if (bitCount > 0) {
            out.push_back(static_cast<uint8_t>(bitBuf & 0xFF));
            bitBuf = 0;
            bitCount = 0;
        }
    }

    // 当前已写入的有效位数（含未 flush 的 bitCount）
    size_t totalBits() const { return out.size() * 8ull + static_cast<size_t>(bitCount); }
};

struct BitReader {
    const uint8_t* src;
    size_t n;
    size_t pos = 0;
    uint32_t bitBuf = 0;
    int bitCount = 0;
    BitReader(const uint8_t* s, size_t len) : src(s), n(len) {}

    bool readBits(int count, uint32_t& value) {
        value = 0;
        for (int i = 0; i < count; ++i) {
            if (bitCount == 0) {
                if (pos >= n) return false;
                bitBuf = src[pos++];
                bitCount = 8;
            }
            value |= (bitBuf & 1u) << i;
            bitBuf >>= 1;
            bitCount--;
        }
        return true;
    }

    void alignByte() { bitBuf = 0; bitCount = 0; }

    bool readByte(uint8_t& b) {
        if (pos >= n) return false;
        b = src[pos++];
        return true;
    }

    bool readU16(uint16_t& v) {
        uint8_t lo, hi;
        if (!readByte(lo) || !readByte(hi)) return false;
        v = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
        return true;
    }
};

// ============================================================================
// Huffman：码长计算（含长度限制）+ canonical codes + 解码器
// ============================================================================
void computeLengths(const std::vector<uint32_t>& freq, int n, std::vector<int>& lengths) {
    lengths.assign(n, 0);

    std::vector<int> syms;
    for (int i = 0; i < n; ++i) if (freq[i] > 0) syms.push_back(i);
    int m = static_cast<int>(syms.size());
    if (m == 0) return;
    if (m == 1) { lengths[syms[0]] = 1; return; }

    struct Node { uint32_t freq; int sym; int l; int r; };
    std::vector<Node> nodes(2 * m - 1);
    using PQ = std::priority_queue<std::pair<uint32_t, int>, std::vector<std::pair<uint32_t, int>>,
                                   std::greater<std::pair<uint32_t, int>>>;
    PQ pq;
    for (int i = 0; i < m; ++i) {
        nodes[i] = {freq[syms[i]], syms[i], -1, -1};
        pq.push({freq[syms[i]], i});
    }
    int next = m;
    while (pq.size() >= 2) {
        auto a = pq.top(); pq.pop();
        auto b = pq.top(); pq.pop();
        nodes[next] = {a.first + b.first, -1, a.second, b.second};
        pq.push({a.first + b.first, next});
        next++;
    }

    int blCount[MAX_BITS + 1] = {0};
    int overflow = 0;
    std::vector<std::pair<int, int>> stack;
    stack.push_back({2 * m - 2, 0});
    while (!stack.empty()) {
        auto [idx, depth] = stack.back();
        stack.pop_back();
        const Node& nd = nodes[idx];
        if (nd.l == -1) {
            int bits = depth;
            if (bits > MAX_BITS) { bits = MAX_BITS; overflow++; }
            lengths[nd.sym] = bits;
            blCount[bits]++;
        } else {
            stack.push_back({nd.r, depth + 1});
            stack.push_back({nd.l, depth + 1});
        }
    }

    if (overflow > 0) {
        do {
            int bits = MAX_BITS - 1;
            while (bits >= 1 && blCount[bits] == 0) bits--;
            if (bits < 1) break;
            blCount[bits]--;
            blCount[bits + 1] += 2;
            blCount[MAX_BITS]--;
            overflow -= 2;
        } while (overflow > 0);

        std::sort(syms.begin(), syms.end(), [&](int a, int b) { return freq[a] < freq[b]; });
        int symIdx = 0;
        for (int bits = MAX_BITS; bits >= 1; bits--) {
            int cnt = blCount[bits];
            while (cnt-- > 0 && symIdx < m) lengths[syms[symIdx++]] = bits;
        }
    }
}

void canonicalCodes(const std::vector<int>& lengths, int n, std::vector<uint32_t>& codes) {
    codes.assign(n, 0);
    int blCount[MAX_BITS + 1] = {0};
    for (int i = 0; i < n; ++i) if (lengths[i] > 0) blCount[lengths[i]]++;
    uint32_t nextCode[MAX_BITS + 2] = {0};
    uint32_t code = 0;
    for (int bits = 1; bits <= MAX_BITS; ++bits) {
        code = (code + blCount[bits - 1]) << 1;
        nextCode[bits] = code;
    }
    for (int i = 0; i < n; ++i) {
        int len = lengths[i];
        if (len > 0) codes[i] = nextCode[len]++;
    }
}

struct HuffDecoder {
    uint32_t first[MAX_BITS + 1];
    int blCount[MAX_BITS + 1];
    int offs[MAX_BITS + 2];
    std::vector<int> symbols;

    bool build(const std::vector<int>& lengths, int n) {
        symbols.clear();
        memset(blCount, 0, sizeof(blCount));
        for (int i = 0; i < n; ++i) {
            int len = lengths[i];
            if (len < 0 || len > MAX_BITS) return false;
            if (len > 0) blCount[len]++;
        }
        blCount[0] = 0;
        uint32_t code = 0;
        offs[1] = 0;
        for (int bits = 1; bits <= MAX_BITS; ++bits) {
            code = (code + blCount[bits - 1]) << 1;
            first[bits] = code;
            offs[bits + 1] = offs[bits] + blCount[bits];
        }
        symbols.resize(offs[MAX_BITS + 1]);
        int nextIdx[MAX_BITS + 1];
        for (int b = 1; b <= MAX_BITS; ++b) nextIdx[b] = offs[b];
        for (int sym = 0; sym < n; ++sym) {
            int len = lengths[sym];
            if (len > 0) symbols[nextIdx[len]++] = sym;
        }
        return true;
    }

    int decode(BitReader& br) const {
        uint32_t code = 0;
        for (int len = 1; len <= MAX_BITS; ++len) {
            uint32_t bit;
            if (!br.readBits(1, bit)) return -1;
            code = (code << 1) | bit;
            int count = blCount[len];
            if (count > 0 && code - first[len] < static_cast<uint32_t>(count)) {
                return symbols[offs[len] + static_cast<int>(code - first[len])];
            }
        }
        return -1;
    }
};

// ============================================================================
// LZ77 匹配器
// ============================================================================
struct Matcher {
    static const int WINDOW_SIZE = 32768;
    static const int MIN_MATCH = 3;
    static const int MAX_MATCH = 258;
    static const int NICE_MATCH = 258;
    static const int GOOD_MATCH = 32;
    static const int MAX_CHAIN = 128;

    const uint8_t* data;
    size_t n;
    int head[65536];
    std::vector<int> prev;

    void init(const uint8_t* d, size_t len) {
        data = d;
        n = len;
        memset(head, -1, sizeof(head));
        prev.assign(len, -1);
    }

    // zlib 风格滚动 hash：(((c0 << 5) ^ c1) << 5) ^ c2，减少冲突
    int hash3(size_t p) const {
        uint32_t h = (static_cast<uint32_t>(data[p]) << 5) ^ data[p + 1];
        h = (h << 5) ^ data[p + 2];
        return static_cast<int>(h & 0xFFFF);
    }

    // 在 pos 处找最长匹配（不修改任何状态）。返回长度（>= MIN_MATCH 才有效），dist 输出。
    int tryMatchAt(size_t pos, int& dist) {
        if (pos + MIN_MATCH > n) return 0;
        int h = hash3(pos);
        int bestLen = 0, bestDist = 0;
        int cand = head[h];
        int chain = 0;
        int chainLimit = MAX_CHAIN;
        int maxLen = static_cast<int>(std::min<size_t>(MAX_MATCH, n - pos));
        while (cand >= 0 && chain < chainLimit) {
            int d = static_cast<int>(pos) - cand;
            if (d > WINDOW_SIZE) break;
            // 先快速比较前 3 字节，跳过 hash 冲突的假候选
            if (data[cand] != data[pos] || data[cand + 1] != data[pos + 1] || data[cand + 2] != data[pos + 2]) {
                cand = prev[cand];
                chain++;
                continue;
            }
            int len = 3;
            while (len < maxLen && data[cand + len] == data[pos + len]) len++;
            if (len > bestLen) {
                bestLen = len;
                bestDist = d;
                if (len >= NICE_MATCH) break;
                // 找到 good match 后，剩余链长减到 1/4（zlib 风格）
                if (len >= GOOD_MATCH) chainLimit = chain + ((chainLimit - chain) >> 2);
            }
            cand = prev[cand];
            chain++;
        }
        if (bestLen >= MIN_MATCH) {
            dist = bestDist;
            return bestLen;
        }
        return 0;
    }

    // 把 pos 插入 hash 链（供后续位置匹配使用）。
    void insertAt(size_t pos) {
        if (pos + 2 < n) {
            int h = hash3(pos);
            prev[pos] = head[h];
            head[h] = static_cast<int>(pos);
        }
    }
};

// ============================================================================
// 压缩
// ============================================================================
struct Token {
    bool isMatch;
    uint8_t literal;
    int length;
    int distance;

    static Token lit(uint8_t b) {
        Token t;
        t.isMatch = false;
        t.literal = b;
        t.length = 0;
        t.distance = 0;
        return t;
    }
    static Token match(int len, int dist) {
        Token t;
        t.isMatch = true;
        t.literal = 0;
        t.length = len;
        t.distance = dist;
        return t;
    }
};

void writeDynamicTables(BitWriter& bw, const std::vector<int>& litLens, const std::vector<int>& distLens,
                        int litCount, int distCount) {
    bw.writeBits(static_cast<uint32_t>(litCount - 257), 5);
    bw.writeBits(static_cast<uint32_t>(distCount - 1), 5);

    // 拼接码长序列
    std::vector<int> allLens;
    allLens.reserve(litCount + distCount);
    for (int i = 0; i < litCount; ++i) allLens.push_back(litLens[i]);
    for (int i = 0; i < distCount; ++i) allLens.push_back(distLens[i]);

    // RLE 编码 → clen 符号序列 + extra
    std::vector<int> rleSym, rleExtra, rleExtraBits;
    size_t i = 0;
    int total = static_cast<int>(allLens.size());
    while (i < static_cast<size_t>(total)) {
        int len = allLens[i];
        size_t j = i;
        while (j < static_cast<size_t>(total) && allLens[j] == len && j - i < 138) j++;
        int run = static_cast<int>(j - i);
        if (len == 0) {
            while (run >= 11) {
                int take = std::min(run, 138);
                rleSym.push_back(18);
                rleExtra.push_back(take - 11);
                rleExtraBits.push_back(7);
                run -= take;
            }
            if (run >= 3) {
                rleSym.push_back(17);
                rleExtra.push_back(run - 3);
                rleExtraBits.push_back(3);
                run = 0;
            }
            while (run > 0) {
                rleSym.push_back(0);
                rleExtra.push_back(-1);
                rleExtraBits.push_back(0);
                run--;
            }
        } else {
            rleSym.push_back(len);
            rleExtra.push_back(-1);
            rleExtraBits.push_back(0);
            run--;
            while (run >= 3) {
                int take = std::min(run, 6);
                rleSym.push_back(16);
                rleExtra.push_back(take - 3);
                rleExtraBits.push_back(2);
                run -= take;
            }
            while (run > 0) {
                rleSym.push_back(len);
                rleExtra.push_back(-1);
                rleExtraBits.push_back(0);
                run--;
            }
        }
        i = j;
    }

    // clen 码长
    std::vector<uint32_t> clenFreq(CLEN_SYMBOLS, 0);
    for (int s : rleSym) clenFreq[s]++;
    std::vector<int> clenLens;
    computeLengths(clenFreq, CLEN_SYMBOLS, clenLens);

    int clenCount = CLEN_SYMBOLS;
    while (clenCount > 4 && clenLens[CLEN_ORDER[clenCount - 1]] == 0) clenCount--;
    bw.writeBits(static_cast<uint32_t>(clenCount - 4), 4);
    for (int k = 0; k < clenCount; ++k) {
        bw.writeBits(static_cast<uint32_t>(clenLens[CLEN_ORDER[k]]), 3);
    }

    std::vector<uint32_t> clenCodes;
    canonicalCodes(clenLens, CLEN_SYMBOLS, clenCodes);
    for (size_t k = 0; k < rleSym.size(); ++k) {
        int s = rleSym[k];
        bw.writeCode(clenCodes[s], clenLens[s]);
        if (rleExtraBits[k] > 0) bw.writeBits(static_cast<uint32_t>(rleExtra[k]), rleExtraBits[k]);
    }
}

void writeTokens(BitWriter& bw, const std::vector<Token>& tokens,
                 const std::vector<uint32_t>& litCodes, const std::vector<int>& litLens,
                 const std::vector<uint32_t>& distCodes, const std::vector<int>& distLens) {
    for (const Token& t : tokens) {
        if (t.isMatch) {
            int lc = lengthCode(t.length);
            int dc = distanceCode(t.distance);
            bw.writeCode(litCodes[lc], litLens[lc]);
            const LengthInfo& li = LENGTH_TABLE[lc - 257];
            if (li.extra > 0) bw.writeBits(static_cast<uint32_t>(t.length - li.base), li.extra);
            bw.writeCode(distCodes[dc], distLens[dc]);
            const DistInfo& di = DIST_TABLE[dc];
            if (di.extra > 0) bw.writeBits(static_cast<uint32_t>(t.distance - di.base), di.extra);
        } else {
            bw.writeCode(litCodes[t.literal], litLens[t.literal]);
        }
    }
    bw.writeCode(litCodes[END_OF_BLOCK], litLens[END_OF_BLOCK]);
}

void compressBlock(BitWriter& bw, const uint8_t* data, size_t n, bool isFinal) {
    std::vector<Token> tokens;
    tokens.reserve(n / 2 + 16);
    Matcher matcher;
    matcher.init(data, n);
    // 懒匹配（zlib deflate_slow 风格）：找到匹配后，比较下一位置是否有更长的匹配。
    size_t pos = 0;
    while (pos < n) {
        int dist;
        int len = matcher.tryMatchAt(pos, dist);
        if (len >= Matcher::MIN_MATCH) {
            int dist2 = 0;
            int len2 = (pos + 1 < n) ? matcher.tryMatchAt(pos + 1, dist2) : 0;
            if (len2 > len) {
                // 下一位置匹配更长：当前位置输出 literal，前进 1，下一位置的匹配留给下一轮。
                tokens.push_back(Token::lit(data[pos]));
                matcher.insertAt(pos);
                pos += 1;
            } else {
                tokens.push_back(Token::match(len, dist));
                for (int i = 0; i < len; ++i) matcher.insertAt(pos + i);
                pos += len;
            }
        } else {
            tokens.push_back(Token::lit(data[pos]));
            matcher.insertAt(pos);
            pos += 1;
        }
    }

    std::vector<uint32_t> litFreq(LITERAL_CODES, 0);
    std::vector<uint32_t> distFreq(DISTANCE_SYMBOLS, 0);
    for (const Token& t : tokens) {
        if (t.isMatch) {
            litFreq[lengthCode(t.length)]++;
            distFreq[distanceCode(t.distance)]++;
        } else {
            litFreq[t.literal]++;
        }
    }
    litFreq[END_OF_BLOCK]++;

    std::vector<int> litLens, distLens;
    computeLengths(litFreq, LITERAL_CODES, litLens);
    computeLengths(distFreq, DISTANCE_SYMBOLS, distLens);

    int litCount = LITERAL_CODES;
    while (litCount > 257 && litLens[litCount - 1] == 0) litCount--;
    int distCount = 1;
    for (int i = DISTANCE_SYMBOLS - 1; i >= 0; --i) {
        if (distLens[i] > 0) { distCount = i + 1; break; }
    }

    std::vector<uint32_t> litCodes, distCodes;
    canonicalCodes(litLens, LITERAL_CODES, litCodes);
    canonicalCodes(distLens, DISTANCE_SYMBOLS, distCodes);

    // 固定 Huffman 码表（RFC 1951），供 fixed 块使用
    std::vector<int> fLitLens(288), fDistLens(DISTANCE_SYMBOLS);
    for (int i = 0; i < 144; ++i) fLitLens[i] = 8;
    for (int i = 144; i < 256; ++i) fLitLens[i] = 9;
    for (int i = 256; i < 280; ++i) fLitLens[i] = 7;
    for (int i = 280; i < 288; ++i) fLitLens[i] = 8;
    for (int i = 0; i < DISTANCE_SYMBOLS; ++i) fDistLens[i] = 5;
    std::vector<uint32_t> fLitCodes, fDistCodes;
    canonicalCodes(fLitLens, 288, fLitCodes);
    canonicalCodes(fDistLens, DISTANCE_SYMBOLS, fDistCodes);

    // 分别测 dynamic / fixed 的有效位数，stored 用上界估算
    size_t dynBits = 0, fixBits = 0;
    {
        std::vector<uint8_t> tmp;
        BitWriter t(tmp);
        t.writeBits(isFinal ? 1 : 0, 1);
        t.writeBits(2, 2);
        writeDynamicTables(t, litLens, distLens, litCount, distCount);
        writeTokens(t, tokens, litCodes, litLens, distCodes, distLens);
        dynBits = t.totalBits();
    }
    {
        std::vector<uint8_t> tmp;
        BitWriter t(tmp);
        t.writeBits(isFinal ? 1 : 0, 1);
        t.writeBits(1, 2);
        writeTokens(t, tokens, fLitCodes, fLitLens, fDistCodes, fDistLens);
        fixBits = t.totalBits();
    }
    size_t storedBits = 3 + 7 + 8 * (4 + n);

    // 三种块类型选最小，写入连续位流（块之间不额外对齐，仅 stored 块自身对齐）
    if (dynBits <= fixBits && dynBits <= storedBits) {
        bw.writeBits(isFinal ? 1 : 0, 1);
        bw.writeBits(2, 2);
        writeDynamicTables(bw, litLens, distLens, litCount, distCount);
        writeTokens(bw, tokens, litCodes, litLens, distCodes, distLens);
    } else if (fixBits <= storedBits) {
        bw.writeBits(isFinal ? 1 : 0, 1);
        bw.writeBits(1, 2);
        writeTokens(bw, tokens, fLitCodes, fLitLens, fDistCodes, fDistLens);
    } else {
        bw.writeBits(isFinal ? 1 : 0, 1);
        bw.writeBits(0, 2);
        bw.align();
        bw.out.push_back(static_cast<uint8_t>(n & 0xFF));
        bw.out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        uint16_t nlen = static_cast<uint16_t>(~n);
        bw.out.push_back(static_cast<uint8_t>(nlen & 0xFF));
        bw.out.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFF));
        bw.out.insert(bw.out.end(), data, data + n);
    }
}

// ============================================================================
// 解压
// ============================================================================
bool decodeBlockData(BitReader& br, std::vector<uint8_t>& out,
                     const HuffDecoder& litDec, const HuffDecoder& distDec) {
    while (true) {
        int sym = litDec.decode(br);
        if (sym < 0) return false;
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == END_OF_BLOCK) {
            return true;
        } else {
            int lenIdx = sym - 257;
            if (lenIdx < 0 || lenIdx >= 29) return false;
            const LengthInfo& li = LENGTH_TABLE[lenIdx];
            uint32_t extra = 0;
            if (li.extra > 0 && !br.readBits(li.extra, extra)) return false;
            int length = li.base + static_cast<int>(extra);

            int dsym = distDec.decode(br);
            if (dsym < 0 || dsym >= 30) return false;
            const DistInfo& di = DIST_TABLE[dsym];
            extra = 0;
            if (di.extra > 0 && !br.readBits(di.extra, extra)) return false;
            int dist = di.base + static_cast<int>(extra);
            if (dist > static_cast<int>(out.size())) return false;

            for (int i = 0; i < length; ++i) {
                out.push_back(out[out.size() - dist]);
            }
        }
    }
}

bool decodeFixedBlock(BitReader& br, std::vector<uint8_t>& out) {
    std::vector<int> litLens(288);
    for (int i = 0; i < 144; ++i) litLens[i] = 8;
    for (int i = 144; i < 256; ++i) litLens[i] = 9;
    for (int i = 256; i < 280; ++i) litLens[i] = 7;
    for (int i = 280; i < 288; ++i) litLens[i] = 8;
    std::vector<int> distLens(DISTANCE_SYMBOLS, 5);

    HuffDecoder litDec, distDec;
    if (!litDec.build(litLens, 288)) return false;
    if (!distDec.build(distLens, DISTANCE_SYMBOLS)) return false;
    return decodeBlockData(br, out, litDec, distDec);
}

bool decodeDynamicBlock(BitReader& br, std::vector<uint8_t>& out) {
    uint32_t hlit, hdist, hclen;
    if (!br.readBits(5, hlit)) return false;
    if (!br.readBits(5, hdist)) return false;
    if (!br.readBits(4, hclen)) return false;
    int litCount = static_cast<int>(hlit) + 257;
    int distCount = static_cast<int>(hdist) + 1;
    int clenCount = static_cast<int>(hclen) + 4;
    if (litCount > 286 || distCount > 30 || clenCount > 19) return false;

    std::vector<int> clenLens(CLEN_SYMBOLS, 0);
    for (int i = 0; i < clenCount; ++i) {
        uint32_t len;
        if (!br.readBits(3, len)) return false;
        clenLens[CLEN_ORDER[i]] = static_cast<int>(len);
    }
    HuffDecoder clenDec;
    if (!clenDec.build(clenLens, CLEN_SYMBOLS)) return false;

    std::vector<int> allLens;
    allLens.reserve(litCount + distCount);
    while (static_cast<int>(allLens.size()) < litCount + distCount) {
        int sym = clenDec.decode(br);
        if (sym < 0) return false;
        if (sym <= 15) {
            allLens.push_back(sym);
        } else if (sym == 16) {
            if (allLens.empty()) return false;
            uint32_t extra;
            if (!br.readBits(2, extra)) return false;
            int repeat = 3 + static_cast<int>(extra);
            int prev = allLens.back();
            for (int i = 0; i < repeat; ++i) allLens.push_back(prev);
        } else if (sym == 17) {
            uint32_t extra;
            if (!br.readBits(3, extra)) return false;
            int repeat = 3 + static_cast<int>(extra);
            for (int i = 0; i < repeat; ++i) allLens.push_back(0);
        } else { // 18
            uint32_t extra;
            if (!br.readBits(7, extra)) return false;
            int repeat = 11 + static_cast<int>(extra);
            for (int i = 0; i < repeat; ++i) allLens.push_back(0);
        }
    }

    std::vector<int> litLens(allLens.begin(), allLens.begin() + litCount);
    std::vector<int> distLens(allLens.begin() + litCount, allLens.end());
    HuffDecoder litDec, distDec;
    if (!litDec.build(litLens, litCount)) return false;
    if (!distDec.build(distLens, distCount)) return false;
    return decodeBlockData(br, out, litDec, distDec);
}

} // namespace

// ============================================================================
// 公开接口
// ============================================================================
bool deflateCompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    const size_t BLOCK_SIZE = 65535;
    BitWriter bw(out);
    if (n == 0) {
        bw.writeBits(1, 1);
        bw.writeBits(0, 2);
        bw.align();
        bw.out.push_back(0);
        bw.out.push_back(0);
        bw.out.push_back(0xFF);
        bw.out.push_back(0xFF);
        return true;
    }
    size_t pos = 0;
    while (pos < n) {
        size_t blockSize = std::min(BLOCK_SIZE, n - pos);
        bool isFinal = (pos + blockSize >= n);
        compressBlock(bw, src + pos, blockSize, isFinal);
        pos += blockSize;
    }
    bw.align();
    return true;
}

bool deflateDecompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    BitReader br(src, n);
    bool final = false;
    while (!final) {
        uint32_t bfinal, btype;
        if (!br.readBits(1, bfinal)) return false;
        if (!br.readBits(2, btype)) return false;
        final = (bfinal != 0);
        if (btype == 0) {
            br.alignByte();
            uint16_t len, nlen;
            if (!br.readU16(len) || !br.readU16(nlen)) return false;
            if ((len ^ 0xFFFF) != nlen) return false;
            for (uint32_t i = 0; i < len; ++i) {
                uint8_t b;
                if (!br.readByte(b)) return false;
                out.push_back(b);
            }
        } else if (btype == 1) {
            if (!decodeFixedBlock(br, out)) return false;
        } else if (btype == 2) {
            if (!decodeDynamicBlock(br, out)) return false;
        } else {
            return false;
        }
    }
    return true;
}

} // namespace jc
