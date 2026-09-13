// Symbolic.cpp
#include "Symbolic.h"
#include "Factorization.h"
#include "../memory/Value.h"          // ★ 新增：统一走 Value 运算
#include "../math/Tolerance.h"
#include "SymEval.h"
#include "SymRules.h"
#include "Groebner.h"
#include <sstream>
#include <cmath>
#include <algorithm>
#include <functional>
#include <set>
#include <numeric>
#include <map>

#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <optional>
#include <complex>

namespace jc {

    std::atomic<bool> g_interruptRequested{false};

    // ==========================================
    // 哈希工具与全局表达式内存池 (DAG Interning Pool)
    // ==========================================
    static uint64_t hashCombine(uint64_t h1, uint64_t h2) {
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }

    static uint64_t hashString(const std::string& s) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (char c : s) {
            h ^= static_cast<uint64_t>(c);
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    static uint64_t hashCASVal(const CASVal& v) {
        return std::visit([](auto&& arg) -> uint64_t {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, int32_t>) return hashCombine(1, std::hash<int32_t>{}(arg));
            else if constexpr (std::is_same_v<T, double>) return hashCombine(2, std::hash<double>{}(arg));
            else if constexpr (std::is_same_v<T, BigInt>) return hashCombine(3, hashString(arg.toString()));
            else return hashCombine(4, hashCombine(hashString(arg.getNum().toString()), hashString(arg.getDen().toString())));
        }, v);
    }

    // ==========================================
    // Arena 内存池实现 (无锁极速分配)
    // ==========================================
    struct SymArena {
        struct Block {
            static constexpr size_t SIZE = 256 * 1024;
            char data[SIZE] = {0};
            size_t offset = 0;
            Block* next = nullptr;
        };
        Block* head = nullptr;

        void* allocate(size_t size) {
            size = (size + 7) & ~7; // 8字节对齐
            if (!head || head->offset + size > Block::SIZE) {
                Block* newBlock = new Block();
                newBlock->next = head;
                head = newBlock;
            }
            void* ptr = head->data + head->offset;
            head->offset += size;
            return ptr;
        }

        void deallocate_last(void* ptr, size_t size) {
            size = (size + 7) & ~7;
            if (head && ptr == head->data + head->offset - size) {
                head->offset -= size; // 极速回收刚刚分配的重复节点
            }
        }
    };

    static thread_local SymArena g_symArena;

    void* SymNode::operator new(size_t size) { return g_symArena.allocate(size); }
    void SymNode::operator delete(void* ptr, size_t size) { g_symArena.deallocate_last(ptr, size); }

    // ==========================================
    // 符号节点内部化池（扁平开放寻址哈希表）
    // ==========================================
    // 【动机】原实现是 std::unordered_map<uint64_t, std::vector<SymNode*>>。
    //   实测（2000 次 cas.simplify 后取样）buckets == entries，即每个桶只装
    //   一个节点，vector 那一层是纯浪费；而每个条目要摊上 map 结点(~48B) +
    //   vector 对象(24B) + 单元素堆缓冲(~32B) ≈ 110B 额外开销，比被池化的
    //   SymNode 本体(48~64B) 还大，等于让 CAS 常驻内存翻两倍以上。
    // 【原理】开放寻址 + 线性探测：每个条目只占 16B，查找是连续内存扫描而
    //   非指针追逐。池中节点只增不删（删除只发生在 intern 的临时节点上，
    //   那种节点从未入池），因此不需要墓碑标记。
    class SymPool {
        struct Slot {
            uint64_t hash = 0;
            SymNode* node = nullptr;
        };
        std::vector<Slot> slots;
        size_t mask = 0;
        size_t count = 0;

        // 用高位做雪崩混合后再取模。hashCombine 的低位在结构相关的表达式之间
        // 相关性较强（它把 h1<<6，低 6 位几乎不参与），直接拿低位取模会让
        // 同一族的节点挤进同一条探测链，线性探测退化成链表扫描。
        size_t indexOf(uint64_t h) const {
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33;
            return static_cast<size_t>(h) & mask;
        }

        void insertRaw(uint64_t h, SymNode* node) {
            size_t i = indexOf(h);
            while (slots[i].node) i = (i + 1) & mask;
            slots[i].hash = h;
            slots[i].node = node;
            ++count;
        }

        void grow() {
            std::vector<Slot> old;
            old.swap(slots);
            size_t newCap = old.empty() ? 1024 : old.size() * 2;
            slots.assign(newCap, Slot{});
            mask = newCap - 1;
            count = 0;
            for (const Slot& s : old) {
                if (s.node) insertRaw(s.hash, s.node);
            }
        }

    public:
        static constexpr size_t MAX_LOAD_NUM = 3;   // 负载因子上限 3/4
        static constexpr size_t MAX_LOAD_DEN = 4;

        // 按哈希探测第一个满足 pred 的节点；未命中返回 nullptr。不修改表。
        template <class Pred>
        SymNode* find(uint64_t h, Pred&& pred) const {
            if (slots.empty()) return nullptr;
            size_t i = indexOf(h);
            while (slots[i].node) {
                if (slots[i].hash == h && pred(slots[i].node)) return slots[i].node;
                i = (i + 1) & mask;
            }
            return nullptr;
        }

        void insert(uint64_t h, SymNode* node) {
            if ((count + 1) * MAX_LOAD_DEN > slots.size() * MAX_LOAD_NUM) grow();
            insertRaw(h, node);
        }

        // 先按结构等价去重；重复则回收刚分配的临时节点并返回池中那一个。
        SymNode* intern(uint64_t h, SymNode* node) {
            if ((count + 1) * MAX_LOAD_DEN > slots.size() * MAX_LOAD_NUM) grow();
            size_t i = indexOf(h);
            while (slots[i].node) {
                if (slots[i].hash == h && slots[i].node->getType() == node->getType()
                    && slots[i].node->equals(node)) {
                    if (slots[i].node != node) delete node; // 触发 deallocate_last，回收内存
                    return slots[i].node;
                }
                i = (i + 1) & mask;
            }
            slots[i].hash = h;
            slots[i].node = node;
            ++count;
            return node;
        }
    };

    static thread_local SymPool g_symPool;

    void SymExpr::cleanupPool() {
        // 池中节点只增不删（它们是所有 SymExpr 共享的规范表示），
        // 逐条回收需要与 Value/GC 协同，此处不做动作。
    }

    SymNode* SymExpr::intern(SymNode* node) {
        if (!node) return nullptr;
        return g_symPool.intern(node->hashValue, node);
    }

    // ==========================================
    // CASVal ↔ Value 桥接（全局单一权威）
    // ==========================================
    static Value casValToValue(const CASVal& v) {
        return std::visit([](auto&& arg) -> Value { return Value(arg); }, v);
    }

    static CASVal valueToCasVal(const Value& v) {
        if (v.isObjType(ObjType::BIGINT))    return static_cast<ObjBigInt*>(v.asObj())->num;
        if (v.isObjType(ObjType::FRACTION))  return static_cast<ObjFraction*>(v.asObj())->frac;
        if (v.isFloat())                    return v.asFloatRaw();
        // ★ 整数统一规范成 BigInt 存储。Value 侧把 int32 与 float 视为同一个数
        //   （1 == 1.0），但 CASVal 若同时存在 int32(2) 与 BigInt(2) 两种形态，
        //   则 SymNum 的哈希/指针都会不同：内部化失效、同类项合并不掉，
        //   cas.simplify 曾因此在展开式上崩溃或给出错误结果。
        //   在这里统一形态，下游的哈希、比较、池查找、terms 索引就自动一致。
        if (v.isInt32())                     return BigInt(v.asInt32());
        JC2_THROW(MathError, "Cannot convert value to CAS type.");
    }

    // ==========================================
    // 谓词与工具（全部委托 Value）
    // ==========================================
    bool isCasZero(const CASVal& v) {
        return !casValToValue(v).truthy();
    }

    bool isCasOne(const CASVal& v) {
        try { return casValToValue(v).asFloat() == 1.0; }
        catch (...) { return false; }
    }

    // ==========================================
// 底层探测器：精准识别 CASVal 符号
// ==========================================
    bool isCasNegative(const CASVal& v) {
        return std::visit([](auto&& arg) -> bool {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, BigInt>) return arg.isNegative();
            else if constexpr (std::is_same_v<T, Fraction>) return arg.getNum().isNegative() != arg.getDen().isNegative();
            else if constexpr (std::is_same_v<T, double>) return arg < 0.0;
            else return arg < 0;
            }, v);
    }

    std::string casToString(const CASVal& v) {
        Value val = casValToValue(v);
        std::ostringstream oss;
        oss << val;
        return oss.str();
    }

    CASVal casAdd(const CASVal& a, const CASVal& b) {
        return valueToCasVal(casValToValue(a) + casValToValue(b));
    }

    CASVal casMul(const CASVal& a, const CASVal& b) {
        return valueToCasVal(casValToValue(a) * casValToValue(b));
    }

    // 从 CASVal 提取整数（委托 Value::asFloat）
    static std::pair<bool, int> casToInt(const CASVal& v) {
        if (std::holds_alternative<int32_t>(v)) {
            int32_t val = std::get<int32_t>(v);
            if (std::abs(val) <= 1000) return { true, val };
        }
        try {
            double d = casValToValue(v).asFloat();
            if (d == std::round(d) && std::abs(d) <= 1000)
                return { true, static_cast<int>(std::round(d)) };
        }
        catch (...) {}
        return { false, 0 };
    }

    // ==========================================
    // toString 实现
    // ==========================================

    // ==========================================
    // 打印专用排序器 (人类可读排版)
    // ==========================================
    static int compareForPrint(SymNode* a, SymNode* b, bool isMul) {
        if (a == b) return 0;

        if (isMul) {
            // 乘法：常数排在最前面
            if (a->getType() == SymType::NUM && b->getType() != SymType::NUM) return -1;
            if (a->getType() != SymType::NUM && b->getType() == SymType::NUM) return 1;
        }

        // 虚数单位 i 是常数而非普通变量，恒排在普通符号之后
        // (否则字典序会把 i 插到 sqrt(2) 之前，得到 "2 * i * sqrt(2)")
        auto isImagUnit = [](SymNode* n) -> bool {
            return n->getType() == SymType::CONST && static_cast<SymConst*>(n)->id == SymConstId::I;
        };
        if (isImagUnit(a) && !isImagUnit(b)) return 1;
        if (!isImagUnit(a) && isImagUnit(b)) return -1;

        auto getCore = [](SymNode* n) -> std::tuple<SymNode*, double, double> {
            SymNode* core = n;
            if (n->getType() == SymType::MUL) {
                auto mul = static_cast<SymMul*>(n);
                std::vector<SymNode*> vars;
                for (auto& arg : mul->args) {
                    if (arg->getType() != SymType::NUM) vars.push_back(arg);
                }
                if (vars.empty()) return {nullptr, 0.0, 0.0}; // 纯常数
                if (vars.size() == 1) core = vars[0];
                else {
                    double totalExp = 0.0;
                    for (auto& v : vars) {
                        if (v->getType() == SymType::POW) {
                            auto p = static_cast<SymPow*>(v);
                            if (p->exp->getType() == SymType::NUM) {
                                try { totalExp += casValToValue(static_cast<SymNum*>(p->exp)->value).asFloat(); } catch(...) { totalExp += 1.0; }
                            } else totalExp += 1.0;
                        } else {
                            totalExp += 1.0;
                        }
                    }
                    return {n, 1.0, totalExp};
                }
            } else if (n->getType() == SymType::NUM) {
                return {nullptr, 0.0, 0.0};
            }

            if (core->getType() == SymType::POW) {
                auto p = static_cast<SymPow*>(core);
                if (p->exp->getType() == SymType::NUM) {
                    try {
                        double e = casValToValue(static_cast<SymNum*>(p->exp)->value).asFloat();
                        return {p->base, e, e};
                    } catch(...) {}
                }
            }
            return {core, 1.0, 1.0};
        };

        auto [coreA, expA, totA] = getCore(a);
        auto [coreB, expB, totB] = getCore(b);

        // 纯常数排在最后 (加法中)
        if (!isMul) {
            if (!coreA && coreB) return 1;
            if (coreA && !coreB) return -1;
        }
        if (!coreA && !coreB) {
            // 都是常数，按数值大小排
            if (a->getType() == SymType::NUM && b->getType() == SymType::NUM) {
                try {
                    double valA = casValToValue(static_cast<SymNum*>(a)->value).asFloat();
                    double valB = casValToValue(static_cast<SymNum*>(b)->value).asFloat();
                    if (valA != valB) return valA < valB ? -1 : 1;
                } catch(...) {}
            }
            return a->toString() < b->toString() ? -1 : 1;
        }

        // 总指数高的排在前面 (降幂排列)
        if (totA != totB) {
            return totA > totB ? -1 : 1;
        }

        // 核心不同时，按字典序排 (人类可读)
        if (coreA != coreB) {
            std::string strA = coreA->toString();
            std::string strB = coreB->toString();
            if (strA != strB) {
                return strA < strB ? -1 : 1;
            }
            return coreA < coreB ? -1 : 1;
        }

        // 单变量同底数，指数高的排在前面
        if (expA != expB) {
            return expA > expB ? -1 : 1;
        }

        return 0;
    }

    // ==========================================
    // 加法节点排版：原教旨安全版 
    // a + (-b) 自动识别首个字符转为 a - b
    // ==========================================
    std::string SymAdd::computeString() const {
        if (args.empty()) return "0";
        std::string res = "";

        std::vector<SymNode*> sortedArgs = args;
        std::sort(sortedArgs.begin(), sortedArgs.end(), [](SymNode* a, SymNode* b) {
            return compareForPrint(a, b, false) < 0;
        });

        for (size_t i = 0; i < sortedArgs.size(); ++i) {
            std::string termStr = sortedArgs[i]->toString();

            if (i > 0) {
                // 只要该项字符串以 '-' 开头，说明它是个纯正的负项 (如 "-3 * x" 或是 "-x")
                if (!termStr.empty() && termStr[0] == '-') {
                    res += " - ";
                    res += termStr.substr(1); // 仅仅切掉开头的负号，绝对安全！
                }
                else {
                    res += " + ";
                    res += termStr;
                }
            }
            else {
                res += termStr; // 第一项原样输出 (-x 还是 -x)
            }
        }
        return res;
    }

    // ==========================================
    // 乘法节点排版：原教旨安全版 
    // 自动拦截开头的负系数，且绝不误伤内部括号
    // ==========================================

    // 整数 k 次方根的下取整（二分）。仅用于排版时判断完美幂，v 不会很大。
    static BigInt nthRootFloor(const BigInt& v, int64_t k) {
        if (k <= 1) return v;
        if (v <= BigInt(1)) return BigInt(1);   // 调用点只处理 v >= 1
        BigInt lo(0), hi(v);
        while (lo < hi) {
            BigInt mid = (lo + hi + BigInt(1)) / BigInt(2);
            BigInt mp(1);
            bool over = false;
            for (int64_t i = 0; i < k; ++i) {
                mp = mp * mid;
                if (mp > v) { over = true; break; }
            }
            if (!over && mp <= v) lo = mid; else hi = mid - BigInt(1);
        }
        return lo;
    }

    std::string SymMul::computeString() const {
        if (args.empty()) return "1";
        std::string res;

        std::vector<SymNode*> sortedArgs = args;
        std::sort(sortedArgs.begin(), sortedArgs.end(), [](SymNode* a, SymNode* b) {
            return compareForPrint(a, b, true) < 0;
        });

        size_t startIdx = 0;

        // ★ 整数系数与负单位分数幂合并排版。
        //   指数归约会把 p^(k*m/n) 写成 p^(q+1) / p^((n-r)/n)，乘积于是长成
        //   "2 * 4^(-1/5)"（即 NUM 2 × POW(4, -1/5)），平铺出来是 "2 * 1 / root(4, 5)"。
        //   这里把整数系数折回根号内：c * b^(-1/n) = root(c^n / b, n)，
        //   于是 2^(3/5) → root(8, 5)、2^(4/7) → root(16, 7)。
        //   仅当「整数 × 负单位分数幂」这一形状成立时才改写，其余情形一律走原路径。
        {
            bool haveC = false;
            int64_t cInt = 0;
            SymNode* recipBase = nullptr;
            int64_t recipN = 0;
            bool onlyThese = true;
            int numConsts = 0, numRecips = 0;
            for (SymNode* a : args) {
                if (a->getType() == SymType::NUM) {
                    // ★ 只接受整数形态的 CASVal。e/pi 这类常量在表里是 double，
                    //   直接交给 extractExactInt 会在 Complex/double 上做无保护的
                    //   std::get，抛 bad variant access（factor 的打印路径曾因此崩溃）。
                    const CASVal& cv = static_cast<SymNum*>(a)->value;
                    if (!std::holds_alternative<int32_t>(cv) && !std::holds_alternative<BigInt>(cv)) {
                        onlyThese = false; break;
                    }
                    auto [isI, iv] = extractExactInt(cv);
                    if (!isI || iv < 1 || iv > 1000000) { onlyThese = false; break; }
                    cInt = iv;
                    haveC = true;
                    ++numConsts;
                } else if (a->getType() == SymType::POW) {
                    auto pw = static_cast<SymPow*>(a);
                    if (pw->exp->getType() != SymType::NUM) { onlyThese = false; break; }
                    const CASVal& ev = static_cast<SymNum*>(pw->exp)->value;
                    if (!std::holds_alternative<Fraction>(ev)) { onlyThese = false; break; }
                    Fraction ef = std::get<Fraction>(ev);
                    if (ef.getNum() != BigInt(-1) || ef.getDen() <= BigInt(1)) { onlyThese = false; break; }
                    if (pw->base->getType() != SymType::NUM) { onlyThese = false; break; }
                    const CASVal& bvv = static_cast<SymNum*>(pw->base)->value;
                    if (!std::holds_alternative<int32_t>(bvv) && !std::holds_alternative<BigInt>(bvv)) {
                        onlyThese = false; break;
                    }
                    auto [isIB, bv] = extractExactInt(bvv);
                    if (!isIB || bv < 1 || bv > 1000000) { onlyThese = false; break; }
                    recipBase = pw->base;
                    recipN = ef.getDen().toInt64();
                    ++numRecips;
                } else {
                    onlyThese = false;
                    break;
                }
            }
            if (onlyThese && haveC && numConsts == 1 && numRecips == 1 && recipBase != nullptr) {
                auto [isB, bInt] = extractExactInt(static_cast<SymNum*>(recipBase)->value);
                // c * b^(-1/n) = (c^n / b)^(1/n)
                BigInt cBig(cInt), bBig(bInt);
                BigInt powC(1);
                bool overflow = false;
                for (int64_t i = 0; i < recipN; ++i) {
                    powC = powC * cBig;
                    if (powC > BigInt(1000000000000LL)) { overflow = true; break; }
                }
                if (!overflow) {
                    BigInt numer = powC;
                    BigInt denom = bBig;
                    BigInt g = BigInt::gcd(numer, denom);   // 约分 c^n / b
                    if (g > BigInt(1)) { numer = numer / g; denom = denom / g; }
                    if (denom == BigInt(1)) {
                        // 分母已消失：整体 = numer^(1/n)。若是完美 n 次幂则直接给出整数。
                        BigInt root = nthRootFloor(numer, recipN);
                        BigInt chk(1);
                        bool over = false;
                        for (int64_t i = 0; i < recipN; ++i) {
                            chk = chk * root;
                            if (chk > numer) { over = true; break; }
                        }
                        if (!over && chk == numer) return root.toString();
                        std::string s = numer.toString();
                        if (recipN == 2) return "sqrt(" + s + ")";
                        if (recipN == 3) return "cbrt(" + s + ")";
                        return "root(" + s + ", " + std::to_string(recipN) + ")";
                    }
                    // 仍有分母：不是干净的单根式，交回原有排版路径
                }
            }
        }

        // 探查乘积的第一项是不是确凿的常数，且以 '-' 开头
        if (sortedArgs[0]->getType() == SymType::NUM) {
            std::string firstStr = sortedArgs[0]->toString();
            if (!firstStr.empty() && firstStr[0] == '-') {
                startIdx = 1; // 跨过第一项独立排版

                if (sortedArgs.size() == 1) return firstStr; // 单独的 "-3"

                if (firstStr == "-1") {
                    res += "-"; // -1 * x 简化为 -x
                }
                else {
                    res += "-" + firstStr.substr(1) + " * "; // -3 * x 简化为 -3 * x
                }
            }
        }

        // 把后面的乘积拼接上去
        for (size_t i = startIdx; i < sortedArgs.size(); ++i) {
            if (i > startIdx) res += " * ";

            std::string termStr = sortedArgs[i]->toString();

            // 只有加法节点 (a+b) 在乘法中需要套括号保护！！其他由于 CAS 分配律必然无需多余括号！
            if (sortedArgs[i]->getType() == SymType::ADD) {
                res += "(" + termStr + ")";
            }
            else {
                res += termStr;
            }
        }
        return res;
    }

    std::string SymPow::computeString() const {
        // ==========================================
        // 排版拦截：检测是否为纯分数指数 1/n
        // ==========================================
        if (exp->getType() == SymType::NUM) {
            auto expNum = static_cast<SymNum*>(exp);
            if (std::holds_alternative<Fraction>(expNum->value)) {
                Fraction f = std::get<Fraction>(expNum->value);
                // 仅当分子为 1，且分母大于 1 时触发根式简写
                if (f.getNum() == BigInt(1) && f.getDen() > BigInt(1)) {
                    int64_t n = 0;
                    try {
                        n = static_cast<int64_t>(f.getDen().toFloat());
                    }
                    catch (...) {}

                    if (n == 2) {
                        return "sqrt(" + base->toString() + ")";
                    }
                    else if (n == 3) {
                        return "cbrt(" + base->toString() + ")";
                    }
                    else if (n > 3) {
                        return "root(" + base->toString() + ", " + std::to_string(n) + ")";
                    }
                }
                // ★ 负分数指数排版：base^(-p/n) 排版为 1 / base^(p/n)。
                //   若写成 "base^(-p/n)"，负号与分数叠在一起难以阅读，重解析也易产生歧义，
                //   这里显式还原成除法。
                if (f.getDen() > BigInt(1) && f.getNum().isNegative()) {
                    SymExpr posPart(new SymPow(base, SymExpr(-f).ptr));
                    return "1 / " + posPart.toString();
                }
                // ★ 真分数指数排版：base^(p/n)（1 < p < n）排版为 [系数 *] root(被开方数, n)。
                //   原先此情形落到通用分支，分子 p 被整个丢掉，2^(2/3) 会被印成 cbrt(2)
                //   （数值其实是 1.5874 而非 1.2599），属于实实在在的错误输出。
                //   ★ 规范形式：base^(p/n) = (base^p)^(1/n)，故取被开方数 M = base^p；若 M 是
                //   完美 n 次幂（M = A^n）则整体可约成一个整数 A。于是
                //   2^(3/5) → root(8, 5)，2^(2/5) → root(4, 5)，2^(4/7) → root(16, 7)，
                //   都避免了 "2 * 1 / root(4, 5)" 这类带倒数的形状。
                if (f.getDen() > BigInt(1) && f.getNum() > BigInt(1) && f.getNum() < f.getDen()) {
                    try {
                        int64_t p = f.getNum().toInt64();
                        int64_t n = f.getDen().toInt64();
                        if (p > 0 && p < 1000000 && n > 1 && n < 1000000) {
                            std::string bStr = base->toString();
                            bool bParen = base->getType() == SymType::ADD || base->getType() == SymType::MUL;
                            std::string outer = bParen ? "(" + bStr + ")" : bStr;
                            std::string inner;
                            BigInt coeff(1);
                            bool folded = false;
                            if (!bParen && base->getType() == SymType::NUM) {
                                auto [isBInt, bInt] = extractExactInt(static_cast<SymNum*>(base)->value);
                                if (isBInt) {
                                    // M = base^p
                                    BigInt M(1);
                                    const BigInt bBig(bInt);
                                    for (int64_t i = 0; i < p; ++i) M = M * bBig;
                                    // 尝试把 M 化成完美 n 次幂
                                    BigInt A = nthRootFloor(M, n);
                                    BigInt chk(1);
                                    bool over = false;
                                    for (int64_t i = 0; i < n; ++i) {
                                        chk = chk * A;
                                        if (chk > M) { over = true; break; }
                                    }
                                    if (!over && chk == M) {
                                        coeff = A;      // 整体是整数
                                        inner = "1";
                                    } else {
                                        inner = M.toString();
                                    }
                                    folded = true;
                                }
                            }
                            if (!folded) inner = outer + "^" + std::to_string(p);
                            if (coeff == BigInt(1) && inner == "1") return "1";
                            std::string rad;
                            if (inner == "1") return coeff.toString();
                            if (n == 2) rad = "sqrt(" + inner + ")";
                            else if (n == 3) rad = "cbrt(" + inner + ")";
                            else rad = "root(" + inner + ", " + std::to_string(n) + ")";
                            if (coeff != BigInt(1)) return coeff.toString() + " * " + rad;
                            return rad;
                        }
                    }
                    catch (...) {}
                }
                // ★ 带分数指数拆解：base^(p/n)（p > n > 1）排版为 base^q * base^(r/n)，
                //   与 sqrt(12) → 2 * sqrt(3) 的形式保持一致；否则会打印成 2^(3/2) 这类
                //   虽正确但不直观的形式（sqrt(8) 的期望输出是 2 * sqrt(2)）。
                //   指数本身仍是精确分数（合并走 casAdd），这里只影响显示。
                if (f.getDen() > BigInt(1) && f.getNum() > f.getDen()) {
                    try {
                        int64_t q = f.getNum().toInt64() / f.getDen().toInt64();
                        int64_t n = f.getDen().toInt64();
                        Fraction frac(f.getNum() - BigInt(q) * f.getDen(), f.getDen());
                        if (q > 0 && n > 0 && frac.getNum() > BigInt(0) && q < 1000000) {
                            std::string bStr = base->toString();
                            bool bParen = base->getType() == SymType::ADD || base->getType() == SymType::MUL;
                            std::string outer = bParen ? "(" + bStr + ")" : bStr;
                            if (q > 1) outer = outer + "^" + std::to_string(q);
                            SymExpr fracPart(new SymPow(base, SymExpr(frac).ptr));
                            return outer + " * " + fracPart.toString();
                        }
                    }
                    catch (...) {}
                }
            }
        }

        // ==========================================
        // 常规幂次排版 (带安全括号的保护逻辑)
        // ==========================================
        std::string bStr = base->toString();
        bool baseParen = false;

        if (base->getType() == SymType::ADD || base->getType() == SymType::MUL) {
            baseParen = true;
        }
        else if (base->getType() == SymType::NUM) {
            if (bStr.find('/') != std::string::npos || (!bStr.empty() && bStr[0] == '-')) {
                baseParen = true;
            }
        }

        if (baseParen && !(bStr.front() == '(' && bStr.back() == ')')) {
            bStr = "(" + bStr + ")";
        }

        std::string eStr = exp->toString();
        bool expParen = false;

        if (exp->getType() == SymType::ADD ||
            exp->getType() == SymType::MUL ||
            exp->getType() == SymType::POW) {
            expParen = true;
        }
        else if (exp->getType() == SymType::NUM) {
            if (eStr.find('/') != std::string::npos || (!eStr.empty() && eStr[0] == '-')) {
                expParen = true;
            }
        }

        if (expParen && !(eStr.front() == '(' && eStr.back() == ')')) {
            eStr = "(" + eStr + ")";
        }

        return bStr + "^" + eStr;
    }

    std::string SymFunc::computeString() const {
        std::string res = name + "(";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) res += ", ";
            res += args[i]->toString();
        }
        return res + ")";
    }

    // ==========================================
    // 节点构造与哈希计算
    // ==========================================
    SymNum::SymNum(CASVal v) : value(std::move(v)) {
        hashValue = hashCombine(static_cast<uint64_t>(SymType::NUM), hashCASVal(value));
    }
    bool SymNum::equals(const SymNode* other) const {
        // ★ 类型守卫：equals 只允许同类型比较。
        //   节点内部字段按各自布局解释，静态转换到错误类型会读到别的成员——
        //   例如把 SymVar("x") 当成 SymPow 读 base/exp，0x10/0x18 落在 std::string name
        //   上，读出的"指针"其实是 'x' 的字节（0x78），随后解引用即访问越界。
        //   case cas.trigsimp(x^2 + 2*x) 曾因此崩溃。
        if (other->getType() != SymType::NUM) return false;
        return value == static_cast<const SymNum*>(other)->value;
    }

    SymVar::SymVar(std::string n) : name(std::move(n)) {
        hashValue = hashCombine(static_cast<uint64_t>(SymType::VAR), hashString(name));
    }
    bool SymVar::equals(const SymNode* other) const {
        if (other->getType() != SymType::VAR) return false;
        return name == static_cast<const SymVar*>(other)->name;
    }
    std::string SymVar::computeString() const {
        return unescapeConstVarName(name);
    }

    // ==========================================
    // ★ 符号常量：单一事实来源的查询入口
    // ==========================================
    // 只有规范名（pi/e/i）代表常量。历史上内部用大写 "PI"/"E"/"I"，那些名字
    // 现在一律当作普通变量（见 makeVar 的转义），否则用户定义的同名符号会被
    // 悄悄当成常数（log(sym("E")) 曾直接化简成 1）。
    bool lookupSymbolicConstant(const std::string& name, SymConstId& outId) {
        for (const auto& d : kSymConstDefs) {
            if (name == d.name) { outId = d.id; return true; }
        }
        return false;
    }

    // 用户符号若与常量规范名冲名，转义成内部名保留，显示时再还原。
    std::string escapeConstVarName(const std::string& name) {
        return "<var:" + name + ">";
    }
    std::string unescapeConstVarName(const std::string& name) {
        const std::string pre = "<var:", suf = ">";
        if (name.size() > pre.size() + suf.size() &&
            name.compare(0, pre.size(), pre) == 0 &&
            name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
            return name.substr(pre.size(), name.size() - pre.size() - suf.size());
        }
        return name;
    }

    static const SymConstDef& constDef(SymConstId id) {
        for (const auto& d : kSymConstDefs) if (d.id == id) return d;
        return kSymConstDefs[0];
    }

    Complex symbolicConstantValue(SymConstId id) {
        if (id == SymConstId::I) return Complex(0.0, 1.0);
        return Complex(constDef(id).value, 0.0);
    }

    SymConst::SymConst(SymConstId i) : id(i) {
        hashValue = hashCombine(static_cast<uint64_t>(SymType::CONST), static_cast<uint64_t>(id));
    }
    bool SymConst::equals(const SymNode* other) const {
        if (other->getType() != SymType::CONST) return false;
        return id == static_cast<const SymConst*>(other)->id;
    }
    std::string SymConst::computeString() const {
        return constDef(id).display;
    }

    SymAdd::SymAdd(std::vector<SymNode*> a) : args(std::move(a)) {
        uint64_t h = static_cast<uint64_t>(SymType::ADD);
        for (const auto& arg : args) h = hashCombine(h, arg->hashValue);
        hashValue = h;
    }
    bool SymAdd::equals(const SymNode* other) const {
        if (other->getType() != SymType::ADD) return false;
        const auto* o = static_cast<const SymAdd*>(other);
        if (args.size() != o->args.size()) return false;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == o->args[i]) continue;
            // ★ 递归结构比较，而不是比指针：内部化池只保证"同一构造路径"复用节点，
            //   expand 出来的 x^2 - 1 与字面写的 x^2 - 1 子节点并不共享指针。
            if (!args[i]->equals(o->args[i])) return false;
        }
        return true;
    }

    SymMul::SymMul(std::vector<SymNode*> a) : args(std::move(a)) {
        uint64_t h = static_cast<uint64_t>(SymType::MUL);
        for (const auto& arg : args) h = hashCombine(h, arg->hashValue);
        hashValue = h;
    }
    bool SymMul::equals(const SymNode* other) const {
        if (other->getType() != SymType::MUL) return false;
        const auto* o = static_cast<const SymMul*>(other);
        if (args.size() != o->args.size()) return false;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == o->args[i]) continue;
            if (!args[i]->equals(o->args[i])) return false;
        }
        return true;
    }

    SymPow::SymPow(SymNode* b, SymNode* e) : base(b), exp(e) {
        hashValue = hashCombine(static_cast<uint64_t>(SymType::POW), hashCombine(base->hashValue, exp->hashValue));
    }
    bool SymPow::equals(const SymNode* other) const {
        if (other->getType() != SymType::POW) return false;
        const auto* o = static_cast<const SymPow*>(other);
        if (base == o->base && exp == o->exp) return true;
        return base->equals(o->base) && exp->equals(o->exp);
    }

    SymFunc::SymFunc(std::string n, std::vector<SymNode*> a) : name(n == "ln" ? "log" : std::move(n)), args(std::move(a)) {
        uint64_t h = hashCombine(static_cast<uint64_t>(SymType::FUNC), hashString(name));
        for (const auto& arg : args) h = hashCombine(h, arg->hashValue);
        hashValue = h;
    }
    bool SymFunc::equals(const SymNode* other) const {
        if (other->getType() != SymType::FUNC) return false;
        const auto* o = static_cast<const SymFunc*>(other);
        if (name != o->name || args.size() != o->args.size()) return false;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == o->args[i]) continue;
            if (!args[i]->equals(o->args[i])) return false;
        }
        return true;
    }

    // =================================================================
// 整数类型的终极脱壳器：
// 无论它是 BigInt, Fraction(分母为1), double(形如2.0), Complex(形如2+0i)
// 只要它数学上是个精确整数，统统榨出其 int64_t 的灵魂！
// =================================================================
    std::pair<bool, int64_t> extractExactInt(const CASVal& cval) {
        return std::visit([](auto&& arg) -> std::pair<bool, int64_t> {
            using T = std::decay_t<decltype(arg)>;

            if constexpr (std::is_same_v<T, int32_t>) {
                return { true, static_cast<int64_t>(arg) };
            }
            else if constexpr (std::is_same_v<T, BigInt>) {
                try {
                    return { true, arg.toInt64() };
                } catch (const EngineInterruptError&) {
                    throw;
                } catch (...) {
                    return { false, 0 };
                }
            }
            else if constexpr (std::is_same_v<T, Fraction>) {
                if (arg.getDen() == BigInt(1)) {
                    try {
                        return { true, arg.getNum().toInt64() };
                    } catch (const EngineInterruptError&) {
                        throw;
                    } catch (...) {
                        return { false, 0 };
                    }
                }
                return { false, 0 };
            }
            else {
                if (std::isfinite(arg) && arg == std::round(arg)) {
                    // C++ 浮点转整防御，最多精准到 53 位 (±9e15)
                    if (std::abs(arg) < 9e15) {
                        return { true, static_cast<int64_t>(std::round(arg)) };
                    }
                }
                return { false, 0 };
            }
            }, cval);
    }

    // ==========================================
    // SymExpr 构造
    // ==========================================
    SymExpr::SymExpr() : ptr(makeNum(BigInt(0)).ptr) {}
    SymExpr::SymExpr(double v) : ptr(makeNum(v).ptr) {}
    SymExpr::SymExpr(const BigInt& v) : ptr(makeNum(v).ptr) {}
    SymExpr::SymExpr(const Fraction& v) : ptr(makeNum(v).ptr) {}
    SymExpr::SymExpr(const Complex& v) {
        if (v.imag == 0.0) {
            ptr = makeNum(v.real).ptr;
        } else if (v.real == 0.0) {
            SymExpr imagPart(v.imag);
            SymExpr iVar = SymExpr::makeConst(SymConstId::I);
            ptr = makeMul({imagPart.ptr, iVar.ptr}).ptr;
        } else {
            SymExpr realPart(v.real);
            SymExpr imagPart(v.imag);
            SymExpr iVar = SymExpr::makeConst(SymConstId::I);
            ptr = makeAdd({realPart.ptr, makeMul({imagPart.ptr, iVar.ptr}).ptr}).ptr;
        }
    }
    SymExpr::SymExpr(const CASVal& v) : ptr(makeNum(v).ptr) {}

    SymExpr SymExpr::makeNum(CASVal v) {
        uint64_t h = hashCombine(static_cast<uint64_t>(SymType::NUM), hashCASVal(v));
        if (SymNode* hit = g_symPool.find(h, [&](SymNode* n) {
                return n->getType() == SymType::NUM && static_cast<SymNum*>(n)->value == v;
            })) {
            return SymExpr::fromInterned(hit);
        }
        SymNode* newNode = new SymNum(std::move(v));
        g_symPool.insert(h, newNode);
        return SymExpr::fromInterned(newNode);
    }

    SymExpr SymExpr::makeConst(SymConstId id) {
        uint64_t h = hashCombine(static_cast<uint64_t>(SymType::CONST), static_cast<uint64_t>(id));
        if (SymNode* hit = g_symPool.find(h, [&](SymNode* n) {
                return n->getType() == SymType::CONST && static_cast<SymConst*>(n)->id == id;
            })) {
            return SymExpr::fromInterned(hit);
        }
        SymNode* newNode = new SymConst(id);
        g_symPool.insert(h, newNode);
        return SymExpr::fromInterned(newNode);
    }

    SymExpr SymExpr::makeVar(const std::string& name) {
        // ★ 与常量规范名冲名的用户符号一律转义后当普通变量处理。
        //   makeVar("pi") 曾经直接产出常量节点，等于让 sym("pi") 静默变成 π。
        //   常量节点只经 makeConst 显式构造。
        std::string actual = name;
        if (isSymbolicConstantName(name)) actual = escapeConstVarName(name);
        uint64_t h = hashCombine(static_cast<uint64_t>(SymType::VAR), hashString(actual));
        if (SymNode* hit = g_symPool.find(h, [&](SymNode* n) {
                return n->getType() == SymType::VAR && static_cast<SymVar*>(n)->name == actual;
            })) {
            return SymExpr::fromInterned(hit);
        }
        SymNode* newNode = new SymVar(actual);
        g_symPool.insert(h, newNode);
        return SymExpr::fromInterned(newNode);
    }

    // 比较两个同类型节点的参数列表是否逐指针相等
    static bool sameArgs(const std::vector<SymNode*>& a, const std::vector<SymNode*>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

    SymExpr SymExpr::makeAdd(std::vector<SymNode*> args) {
        uint64_t h = static_cast<uint64_t>(SymType::ADD);
        for (const auto& arg : args) h = hashCombine(h, arg->hashValue);
        if (SymNode* hit = g_symPool.find(h, [&](SymNode* n) {
                return n->getType() == SymType::ADD && sameArgs(static_cast<SymAdd*>(n)->args, args);
            })) {
            return SymExpr::fromInterned(hit);
        }
        SymNode* newNode = new SymAdd(std::move(args));
        g_symPool.insert(h, newNode);
        return SymExpr::fromInterned(newNode);
    }

    SymExpr SymExpr::makeMul(std::vector<SymNode*> args) {
        uint64_t h = static_cast<uint64_t>(SymType::MUL);
        for (const auto& arg : args) h = hashCombine(h, arg->hashValue);
        if (SymNode* hit = g_symPool.find(h, [&](SymNode* n) {
                return n->getType() == SymType::MUL && sameArgs(static_cast<SymMul*>(n)->args, args);
            })) {
            return SymExpr::fromInterned(hit);
        }
        SymNode* newNode = new SymMul(std::move(args));
        g_symPool.insert(h, newNode);
        return SymExpr::fromInterned(newNode);
    }

    SymExpr SymExpr::makePow(SymNode* base, SymNode* exp) {
        uint64_t h = hashCombine(static_cast<uint64_t>(SymType::POW), hashCombine(base->hashValue, exp->hashValue));
        if (SymNode* hit = g_symPool.find(h, [&](SymNode* n) {
                auto* o = static_cast<SymPow*>(n);
                return n->getType() == SymType::POW && o->base == base && o->exp == exp;
            })) {
            return SymExpr::fromInterned(hit);
        }
        SymNode* newNode = new SymPow(base, exp);
        g_symPool.insert(h, newNode);
        return SymExpr::fromInterned(newNode);
    }

    SymExpr SymExpr::makeFunc(std::string name, std::vector<SymNode*> args) {
        std::string n = (name == "ln") ? "log" : std::move(name);
        uint64_t h = hashCombine(static_cast<uint64_t>(SymType::FUNC), hashString(n));
        for (const auto& arg : args) h = hashCombine(h, arg->hashValue);
        if (SymNode* hit = g_symPool.find(h, [&](SymNode* e) {
                auto* o = static_cast<SymFunc*>(e);
                return e->getType() == SymType::FUNC && o->name == n && sameArgs(o->args, args);
            })) {
            return SymExpr::fromInterned(hit);
        }
        SymNode* newNode = new SymFunc(n, std::move(args));
        g_symPool.insert(h, newNode);
        return SymExpr::fromInterned(newNode);
    }
    std::string SymExpr::toString() const { return ptr ? ptr->toString() : "null"; }
    bool SymExpr::isZero() const { return ptr && ptr->isZero(); }
    bool SymExpr::isOne() const { return ptr && ptr->isOne(); }

    SymExpr operator+(const SymExpr& a, const SymExpr& b) {
        if (!a.ptr) return b;
        if (!b.ptr) return a;
        if (a.isZero()) return b;
        if (b.isZero()) return a;
        std::vector<SymNode*> flatArgs;
        if (a.ptr->getType() == SymType::ADD) {
            auto addA = static_cast<SymAdd*>(a.ptr);
            flatArgs.insert(flatArgs.end(), addA->args.begin(), addA->args.end());
        } else {
            flatArgs.push_back(a.ptr);
        }
        if (b.ptr->getType() == SymType::ADD) {
            auto addB = static_cast<SymAdd*>(b.ptr);
            flatArgs.insert(flatArgs.end(), addB->args.begin(), addB->args.end());
        } else {
            flatArgs.push_back(b.ptr);
        }
        CASVal sumConst = BigInt(0);
        struct TermData { CASVal coeff; SymNode* baseNode = nullptr; };
        
        constexpr int SMALL_CAP = 8;
        TermData smallTerms[SMALL_CAP];
        int smallCount = 0;
        std::unique_ptr<std::unordered_map<SymNode*, TermData>> symTerms;
        bool useMap = flatArgs.size() > SMALL_CAP;
        if (useMap) symTerms = std::make_unique<std::unordered_map<SymNode*, TermData>>();

        auto addTerm = [&](SymNode* key, const CASVal& coeff) {
            if (useMap) {
                auto it = symTerms->find(key);
                if (it != symTerms->end()) it->second.coeff = casAdd(it->second.coeff, coeff);
                else (*symTerms)[key] = {coeff, key};
            } else {
                for (int i = 0; i < smallCount; ++i) {
                    if (smallTerms[i].baseNode == key) {
                        smallTerms[i].coeff = casAdd(smallTerms[i].coeff, coeff);
                        return;
                    }
                }
                smallTerms[smallCount++] = {coeff, key};
            }
        };

        for (auto& node : flatArgs) {
            if (node->getType() == SymType::NUM) {
                sumConst = casAdd(sumConst, static_cast<SymNum*>(node)->value);
            }
            else if (node->getType() == SymType::MUL) {
                auto mul = static_cast<SymMul*>(node);
                CASVal coeff = BigInt(1);
                std::vector<SymNode*> symParts;
                for (auto& m_arg : mul->args) {
                    if (m_arg->getType() == SymType::NUM)
                        coeff = casMul(coeff, static_cast<SymNum*>(m_arg)->value);
                    else
                        symParts.push_back(m_arg);
                }
                if (symParts.empty()) {
                    sumConst = casAdd(sumConst, coeff);
                }
                else {
                    SymExpr rem = (symParts.size() == 1)
                        ? SymExpr::fromInterned(symParts[0])
                        : SymExpr::makeMul(symParts);
                    addTerm(rem.ptr, coeff);
                }
            }
            else {
                addTerm(node, BigInt(1));
            }
        }
        // ★ 纯净输出：不做任何负号提取，直接组装 ADD 节点
        std::vector<TermData> sortedTerms;
        if (useMap) {
            sortedTerms.reserve(symTerms->size());
            for (auto& kv : *symTerms) sortedTerms.push_back(kv.second);
        } else {
            sortedTerms.reserve(smallCount);
            for (int i = 0; i < smallCount; ++i) sortedTerms.push_back(smallTerms[i]);
        }
        std::sort(sortedTerms.begin(), sortedTerms.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.baseNode->hashValue > rhs.baseNode->hashValue;
        });

        std::vector<SymNode*> newArgs;
        for (auto& data : sortedTerms) {
            if (isCasZero(data.coeff)) continue;
            if (isCasOne(data.coeff)) {
                newArgs.push_back(data.baseNode);
            }
            else {
                std::vector<SymNode*> mArgs;
                mArgs.push_back(SymExpr(data.coeff).ptr);
                if (data.baseNode->getType() == SymType::MUL) {
                    auto inner = static_cast<SymMul*>(data.baseNode);
                    mArgs.insert(mArgs.end(), inner->args.begin(), inner->args.end());
                }
                else {
                    mArgs.push_back(data.baseNode);
                }
                newArgs.push_back(SymExpr::makeMul(mArgs).ptr);
            }
        }
        if (!isCasZero(sumConst))
            newArgs.push_back(SymExpr(sumConst).ptr);

        if (newArgs.empty()) return SymExpr(BigInt(0));
        if (newArgs.size() == 1) return SymExpr(newArgs[0]);
        return SymExpr::makeAdd(std::move(newArgs));
    }
    // ==========================================
    // operator*（乘法与同底数指数合并：带 ADD 基底正规化）
    // ==========================================
    SymExpr operator*(const SymExpr& a, const SymExpr& b) {
        if (!a.ptr || !b.ptr) return SymExpr(BigInt(0));
        if (a.isZero() || b.isZero()) return SymExpr(0);
        if (a.isOne()) return b;
        if (b.isOne()) return a;
        std::vector<SymNode*> flatArgs;
        if (a.ptr->getType() == SymType::MUL) {
            auto mulA = static_cast<SymMul*>(a.ptr);
            flatArgs.insert(flatArgs.end(), mulA->args.begin(), mulA->args.end());
        } else {
            flatArgs.push_back(a.ptr);
        }
        if (b.ptr->getType() == SymType::MUL) {
            auto mulB = static_cast<SymMul*>(b.ptr);
            flatArgs.insert(flatArgs.end(), mulB->args.begin(), mulB->args.end());
        } else {
            flatArgs.push_back(b.ptr);
        }
        CASVal prodConst = BigInt(1);
        struct FactorData { CASVal exp; SymNode* baseNode = nullptr; };
        
        constexpr int SMALL_CAP = 8;
        FactorData smallFactors[SMALL_CAP];
        int smallCount = 0;
        std::unique_ptr<std::unordered_map<SymNode*, FactorData>> symFactors;
        bool useMap = flatArgs.size() > SMALL_CAP;
        if (useMap) symFactors = std::make_unique<std::unordered_map<SymNode*, FactorData>>();

        // ★ 核心架构：ADD 基底首项负号正规化
        // 检测一个 ADD 节点的字典序最后一项（通常是最高次项）是否带负系数
        auto addLeadingNegative = [](SymNode* node) -> bool {
            if (node->getType() != SymType::ADD) return false;
            auto add = static_cast<SymAdd*>(node);
            if (add->args.empty()) return false;
            auto lastArg = add->args.back();
            if (lastArg->getType() == SymType::NUM) return false;
            if (lastArg->getType() == SymType::MUL) {
                auto mul = static_cast<SymMul*>(lastArg);
                if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                    return isCasNegative(static_cast<SymNum*>(mul->args[0])->value);
                }
            }
            return false; // VAR, POW 等隐含系数 +1
            };
        // 对 ADD 节点取反：(-y + z) → (y - z)
        auto negateAdd = [](SymNode* node) -> SymNode* {
            auto add = static_cast<SymAdd*>(node);
            SymExpr result(BigInt(0));
            for (auto& arg : add->args) {
                result = result + (SymExpr(BigInt(-1)) * SymExpr(arg));
            }
            return result.ptr;
            };
            
        auto addFactor = [&](SymNode* key, const CASVal& expVal) {
            if (useMap) {
                auto it = symFactors->find(key);
                if (it != symFactors->end()) it->second.exp = casAdd(it->second.exp, expVal);
                else (*symFactors)[key] = {expVal, key};
            } else {
                for (int i = 0; i < smallCount; ++i) {
                    if (smallFactors[i].baseNode == key) {
                        smallFactors[i].exp = casAdd(smallFactors[i].exp, expVal);
                        return;
                    }
                }
                smallFactors[smallCount++] = {expVal, key};
            }
        };

        // 将一个因子（base^exp）登记到 symFactors，同时正规化 ADD 基底
        auto registerFactor = [&](SymNode* base, CASVal expVal) {
            // ★ 如果 base 是 ADD 且首项为负，翻转基底，吸收符号到 prodConst
            if (addLeadingNegative(base)) {
                auto [isInt, n] = extractExactInt(expVal);
                if (isInt) {
                    base = negateAdd(base);
                    // (-1)^n：奇数幂贡献 -1，偶数幂贡献 1
                    if (n % 2 != 0) {
                        prodConst = casMul(prodConst, BigInt(-1));
                    }
                }
            }
            SymExpr internedBase(base);
            addFactor(internedBase.ptr, expVal);
            };
            
        for (auto& node : flatArgs) {
            if (node->getType() == SymType::NUM) {
                prodConst = casMul(prodConst, static_cast<SymNum*>(node)->value);
            }
            else if (node->getType() == SymType::POW) {
                auto powNode = static_cast<SymPow*>(node);
                if (powNode->exp->getType() == SymType::NUM) {
                    CASVal expVal = static_cast<SymNum*>(powNode->exp)->value;
                    registerFactor(powNode->base, expVal);
                }
                else {
                    addFactor(node, BigInt(1));
                }
            }
            else {
                // 普通因子视为 base^1
                registerFactor(node, BigInt(1));
            }
        }
        if (isCasZero(prodConst)) return SymExpr(0);

        // ★ 尝试将 prodConst 与 symFactors 中的 NUM base 合并 (例如 2 * 2^(-1/2) -> 2^(1/2))
        //   合并判据：仅当吸收后指数仍是「可读形式」才吸收。
        //   可读形式 = 整数，或真分数 p/n (0 < p < n)，二者都能由 computeString 直接排版。
        //   若吸收后指数变成假分数 (p > n)，系数会被 embed 进指数而失去与外层常数约分的机会，
        //   于是 1/2 * 2^(3/2) 这类结果无法再化回 sqrt(2)。此时宁可保留显式系数 2 * 2^(1/2)。
        if (!isCasOne(prodConst)) {
            // 指数 +1 后是否仍可读
            auto expPlusOneListable = [](const CASVal& e) -> bool {
                return std::visit([](auto&& arg) -> bool {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, Fraction>) {
                        // exp+1 仍为真分数 <=> (分子+分母) < 分母，即分子 < 0
                        return (arg.getNum() + arg.getDen()) < arg.getDen();
                    } else if constexpr (std::is_same_v<T, double>) {
                        return arg != std::floor(arg);
                    } else {
                        return true; // 整型指数 +1 仍是整数
                    }
                    }, e);
            };
            auto processMerge = [&](FactorData& data) {
                if (data.baseNode->getType() == SymType::NUM) {
                    CASVal bVal = static_cast<SymNum*>(data.baseNode)->value;
                    auto [isBInt, bInt] = extractExactInt(bVal);
                    if (isBInt && bInt != 0 && bInt != 1 && bInt != -1) {
                        while (true) {
                            // ★ 吸收前先确认吸收后的指数仍可读，否则停止（保留显式系数以便约分）
                            if (!expPlusOneListable(data.exp)) break;
                            bool extracted = false;
                            if (std::holds_alternative<int32_t>(prodConst)) {
                                int32_t p = std::get<int32_t>(prodConst);
                                if (p % bInt == 0) {
                                    prodConst = static_cast<int32_t>(p / bInt);
                                    data.exp = casAdd(data.exp, BigInt(1));
                                    extracted = true;
                                }
                            } else if (std::holds_alternative<BigInt>(prodConst)) {
                                BigInt p = std::get<BigInt>(prodConst);
                                if ((p % BigInt(bInt)).isZero()) {
                                    prodConst = p / BigInt(bInt);
                                    data.exp = casAdd(data.exp, BigInt(1));
                                    extracted = true;
                                }
                            } else if (std::holds_alternative<Fraction>(prodConst)) {
                                Fraction p = std::get<Fraction>(prodConst);
                                if ((p.getNum() % BigInt(bInt)).isZero()) {
                                    prodConst = Fraction(p.getNum() / BigInt(bInt), p.getDen());
                                    data.exp = casAdd(data.exp, BigInt(1));
                                    extracted = true;
                                }
                            }
                            if (!extracted) break;
                        }
                    }
                }
            };
            
            if (useMap) {
                for (auto& [key, data] : *symFactors) processMerge(data);
            } else {
                for (int i = 0; i < smallCount; ++i) processMerge(smallFactors[i]);
            }
        }

        std::vector<FactorData> sortedFactors;
        if (useMap) {
            sortedFactors.reserve(symFactors->size());
            for (auto& kv : *symFactors) sortedFactors.push_back(kv.second);
        } else {
            sortedFactors.reserve(smallCount);
            for (int i = 0; i < smallCount; ++i) sortedFactors.push_back(smallFactors[i]);
        }
        std::sort(sortedFactors.begin(), sortedFactors.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.baseNode->hashValue > rhs.baseNode->hashValue;
        });

        std::vector<SymNode*> newArgs;
        for (auto& data : sortedFactors) {
            if (isCasZero(data.exp)) continue;
            
            SymExpr term = SymExpr(data.baseNode) ^ SymExpr(data.exp);
            if (term.ptr->getType() == SymType::NUM) {
                prodConst = casMul(prodConst, static_cast<SymNum*>(term.ptr)->value);
            } else if (term.ptr->getType() == SymType::MUL) {
                auto innerMul = static_cast<SymMul*>(term.ptr);
                for (auto& arg : innerMul->args) {
                    if (arg->getType() == SymType::NUM) {
                        prodConst = casMul(prodConst, static_cast<SymNum*>(arg)->value);
                    } else {
                        newArgs.push_back(arg);
                    }
                }
            } else {
                newArgs.push_back(term.ptr);
            }
        }
        if (isCasZero(prodConst)) return SymExpr(BigInt(0));
        if (!isCasOne(prodConst) || newArgs.empty()) {
            newArgs.insert(newArgs.begin(), SymExpr(prodConst).ptr);
        }
        if (newArgs.size() == 1) return SymExpr(newArgs[0]);
        return SymExpr::makeMul(std::move(newArgs));
    }

    // ==========================================
    // 单目与二元减法
    // ==========================================
    SymExpr SymExpr::operator-() const { return (*this) * SymExpr(-1); }
    SymExpr operator-(const SymExpr& a, const SymExpr& b) { return a + (-b); }

    static std::pair<bool, Value> tryEvalConst(const SymExpr& expr);

    // ==========================================
    // operator/（除法 → 委托 Value 求倒数）
    // ==========================================
    SymExpr operator/(const SymExpr& a, const SymExpr& b) {
        if (!a.ptr) return SymExpr(BigInt(0));
        if (!b.ptr || b.isZero()) JC2_THROW(MathError, "Division by zero.");

        auto [bOk, bVal] = tryEvalConst(b);
        if (bOk && !bVal.truthy()) JC2_THROW(MathError, "Division by zero.");

        if (b.ptr->getType() == SymType::NUM) {
            auto numNode = static_cast<SymNum*>(b.ptr);
            try {
                Value reciprocal = Value(BigInt(1)) / casValToValue(numNode->value);
                return a * SymExpr::makeNum(valueToCasVal(reciprocal));
            }
            catch (...) {}
        }

        return a * (b ^ SymExpr(-1));
    }

    // ==========================================
    // operator^（乘方 → 委托 Value 常数折叠）
    // ==========================================
    SymExpr operator^(const SymExpr& a, const SymExpr& b) {
        // ★ 递归护栏：分解 p^(m/n) 时会递归求 p^(q) 与 p^(r/n)，随后用 operator* 重新合并，
        //   而乘法对同底幂会相加指数，可能又得到原来的 m/n（例如 2^(3/2) → 2^1 * 2^(1/2)
        //   → 重新合并回 2^(3/2)），造成无限递归直至栈溢出。这里限制同一线程内该分解的嵌套
        //   深度：达到上限时跳过「整数底 + 分数指数」的分解，直接构造幂节点，循环即被打断。
        static thread_local int fracDecompDepth = 0;
        if (!a.ptr) return SymExpr(BigInt(0));
        if (!b.ptr) return SymExpr(BigInt(1));

        auto [aOk, aVal] = tryEvalConst(a);
        auto [bOk, bVal] = tryEvalConst(b);
        bool aIsZero = a.isZero() || (aOk && !aVal.truthy());
        bool bIsZero = b.isZero() || (bOk && !bVal.truthy());

        if (aIsZero) {
            if (bIsZero) JC2_THROW(MathError, "0^0 is undefined.");
            bool bIsNeg = false;
            if (bOk) {
                try { bIsNeg = bVal.asFloat() < 0.0; } catch(...) {}
            } else if (b.ptr->getType() == SymType::NUM) {
                bIsNeg = isCasNegative(static_cast<SymNum*>(b.ptr)->value);
            }
            if (bIsNeg) JC2_THROW(MathError, "Division by zero.");
            return SymExpr(0);
        }
        if (a.isOne() || bIsZero) return SymExpr(BigInt(1));
        if (b.isOne()) return a;

        // 常数折叠: 尝试对可求值的常数进行折叠 (如 i^-1 -> -i)
        if (aOk && bOk) {
            // 仅当底数和指数都是精确类型 (非浮点) 时，才进行折叠，以防 PI^2 变成浮点数
            bool aExact = a.ptr->getType() == SymType::NUM ? !std::holds_alternative<double>(static_cast<SymNum*>(a.ptr)->value) : true;
            bool bExact = b.ptr->getType() == SymType::NUM ? !std::holds_alternative<double>(static_cast<SymNum*>(b.ptr)->value) : true;
            
            if (aExact && bExact) {
                // 避免无限递归：如果 a 和 b 都是 NUM，说明我们已经在处理最底层的常数了，
                // 此时不应该再调用 Value::operator^，因为 Value::operator^ 遇到算不尽的数会再次调用 SymExpr::operator^。
                // 实际上，如果 a 和 b 都是 NUM，下面的 NUM^NUM 逻辑会安全地处理它。
                if (!(a.ptr->getType() == SymType::NUM && b.ptr->getType() == SymType::NUM)) {
                    try {
                        Value result = aVal ^ bVal;
                        // 如果结果是精确的整数/分数，则接受折叠 (复数底层是 double，会引入浮点误差，拒绝折叠)
                        if (result.isObjType(ObjType::BIGINT) || result.isObjType(ObjType::FRACTION)) {
                            return result.asSymbolic();
                        } else if (result.isComplex()) {
                            Complex c = result.asComplex();
                            // 仅当复数是精确的高斯整数时才折叠，过滤掉浮点噪音
                            if (c.real == std::round(c.real) && c.imag == std::round(c.imag)) {
                                SymExpr res(BigInt(static_cast<int64_t>(std::round(c.real))));
                                if (std::round(c.imag) != 0.0) {
                                    res = res + SymExpr(BigInt(static_cast<int64_t>(std::round(c.imag)))) * SymExpr::makeConst(SymConstId::I);
                                }
                                return res;
                            }
                        }
                    } catch (const EngineInterruptError&) {
                        throw;
                    } catch (...) {}
                }
            }
        }

        // ★ i 的整数次幂精确折叠。i 是常量节点，取幂会落到复数浮点路径，
        //   i^2 于是得到 -1.0 + 1.22e-16i（浮点噪音），既约不掉也不好显示。
        //   这里直接按 i 的周期性给出精确结果。
        if (a.ptr->getType() == SymType::CONST && static_cast<SymConst*>(a.ptr)->id == SymConstId::I &&
            b.ptr->getType() == SymType::NUM) {
            auto& bv = static_cast<SymNum*>(b.ptr)->value;
            if (std::holds_alternative<int32_t>(bv) || std::holds_alternative<BigInt>(bv)) {
                BigInt e = std::holds_alternative<int32_t>(bv)
                    ? BigInt(std::get<int32_t>(bv)) : std::get<BigInt>(bv);
                bool negExp = e.isNegative();
                BigInt rem = (negExp ? e.abs() : e) % BigInt(4);
                int64_t r4 = 0;
                try { r4 = rem.toInt64(); } catch (...) { r4 = -1; }
                if (r4 >= 0) {
                    // ★ 负指数不能靠 1 / i^k 去求倒数：1/i 会走 operator/ → i^(-1)
                    //   回到本分支，而它要算的倒数正是它自己，递归原地打转直到栈溢出
                    //   （cas.integ(sin(x)*cos(x), x) 就是这样崩的）。
                    //   i 是高斯整数的单位，倒数直接来自周期：1/i^k = i^(-k)，
                    //   负指数只需把指数换成模 4 的相反数，无需任何递归。
                    //   i^0=1, i^1=i, i^2=-1, i^3=-i
                    int64_t k = negExp ? (4 - r4) % 4 : r4;
                    if (k == 0) return SymExpr(BigInt(1));
                    if (k == 1) return SymExpr::makeConst(SymConstId::I);
                    if (k == 2) return SymExpr(BigInt(-1));
                    return SymExpr(BigInt(0)) - SymExpr::makeConst(SymConstId::I);
                }
            }
        }

        // 常数折叠: NUM^NUM → 委托 Value（Value 内部会自动决定精确/符号/浮点）
        if (a.ptr->getType() == SymType::NUM && b.ptr->getType() == SymType::NUM) {
            auto baseNum = static_cast<SymNum*>(a.ptr);
            auto expNum = static_cast<SymNum*>(b.ptr);
            auto [isInt, n] = casToInt(expNum->value);
            if (isInt) {
                try {
                    Value result = casValToValue(baseNum->value) ^ casValToValue(expNum->value);
                    return result.asSymbolic();
                }
                catch (const jc::Jc2Error& e) {
                    if (e.errorClass == jc::err::MathErrorClass || e.errorClass == jc::err::CalculusErrorClass || e.errorClass == jc::err::SymbolicErrorClass)
                        throw;
                }
                catch (const std::runtime_error& e) {
                    std::string msg = e.what();
                    if (msg.find("Math Error") != std::string::npos) throw;
                    if (msg.find("CAS Error") != std::string::npos) throw;
                }
            } else if (std::holds_alternative<Fraction>(expNum->value)) {
                Fraction expF = std::get<Fraction>(expNum->value);
                BigInt m = expF.getNum();
                BigInt n_den = expF.getDen();
                
                auto processIntBase = [&](BigInt baseInt) -> SymExpr {
                    if (baseInt.isZero()) return SymExpr(BigInt(0));
                    if (baseInt == BigInt(1)) return SymExpr(BigInt(1));
                    // 已在一次分数幂分解中：不再进入分解，直接构造幂节点（见函数入口的护栏说明）
                    if (fracDecompDepth >= 8) {
                        SymExpr baseSym(baseInt);
                        SymExpr expFrac(Fraction(m, n_den));
                        return SymExpr(new SymPow(baseSym.ptr, expFrac.ptr));
                    }
                    struct DecompGuard {
                        int& d;
                        DecompGuard(int& x) : d(x) { ++d; }
                        ~DecompGuard() { --d; }
                    } dg(fracDecompDepth);
                    
                    bool isNeg = baseInt.isNegative();
                    if (isNeg) baseInt = baseInt.abs();
                    
                    SymExpr outside(BigInt(1));
                    
                    if (baseInt > BigInt(1)) {
                        auto factors = baseInt.factorize();
                        for (const auto& f : factors) {
                            BigInt p = f.first;
                            BigInt k(f.second);
                            BigInt totalPow = k * m;
                            
                            BigInt q = totalPow / n_den;
                            BigInt r = totalPow % n_den;
                            if (r.isNegative()) {
                                r = r + n_den;
                                q = q - BigInt(1);
                            }
                            // ★ 指数归约：余数 r > n/2 时把 p^(r/n) 改写为更小的 p^((n-r)/n)，
                            //   使分解始终朝更小的指数递归（配合入口护栏断开递归），并避免
                            //   2^(3/2) 这类非规范形式（sqrt(8) 应为 2*sqrt(2)）。
                            //   ★ 恒等式：p^(k*m/n) = p^(q+1) / p^((n-r)/n)，因为
                            //   k*m/n = (q+1) + (r-n)/n 且 r-n = -(n-r)。
                            //   换了余数就必须把 q 加一，并用倒数还原多出来的那一份指数；
                            //   漏掉加一（把商当 q）或去改 outside 都会破坏等式
                            //   （4^(1/3) 会错成 2^(1/3) 或 2*cbrt(2)）。
                            BigInt rEff = r;
                            bool useDiv = false;
                            if (r * BigInt(2) > n_den) {
                                rEff = n_den - r;
                                q = q + BigInt(1);
                                useDiv = true;
                            }
                            
                            if (!q.isZero()) {
                                SymExpr term = SymExpr(p) ^ SymExpr(q);
                                if (outside.isOne()) outside = term;
                                else outside = outside * term;
                            }
                            if (!rEff.isZero()) {
                                BigInt pr(1);
                                for(BigInt i(0); i < rEff; i = i + BigInt(1)) pr = pr * p;
                                SymExpr fracPart = SymExpr(pr) ^ SymExpr(Fraction(BigInt(1), n_den));
                                if (useDiv) {
                                    // 倒数：p^(-(n-r)/n) 用 1 / p^((n-r)/n) 表示，避免负指数
                                    SymExpr inv = SymExpr(BigInt(1)) / fracPart;
                                    if (outside.isOne()) outside = inv;
                                    else outside = outside * inv;
                                } else {
                                    if (outside.isOne()) outside = fracPart;
                                    else outside = outside * fracPart;
                                }
                            }
                        }
                    }
                    
                    // 剩余分数幂已在上面按因子折进 outside，这里直接用其结果
                    SymExpr res = outside;
                    
                    if (isNeg) {
                        BigInt q_neg = m / n_den;
                        BigInt r_neg = m % n_den;
                        if (r_neg.isNegative()) {
                            r_neg = r_neg + n_den;
                            q_neg = q_neg - BigInt(1);
                        }
                        
                        auto multiplyRes = [&](SymExpr factor) {
                            if (res.isOne()) res = factor;
                            else res = factor * res;
                        };

                        if (!(q_neg % BigInt(2)).isZero()) {
                            multiplyRes(SymExpr(BigInt(-1)));
                        }
                        
                        if (!r_neg.isZero()) {
                            if (!(n_den % BigInt(2)).isZero()) {
                                if (!(r_neg % BigInt(2)).isZero()) {
                                    multiplyRes(SymExpr(BigInt(-1)));
                                }
                            } else if (n_den == BigInt(2) && r_neg == BigInt(1)) {
                                multiplyRes(SymExpr::makeConst(SymConstId::I));
                            } else {
                                SymExpr minusOne(BigInt(-1));
                                SymExpr fracSym(Fraction(r_neg, n_den));
                                SymExpr powPart(new SymPow(minusOne.ptr, fracSym.ptr));
                                multiplyRes(powPart);
                            }
                        }
                    }
                    return res;
                };

                if (std::holds_alternative<int32_t>(baseNum->value)) {
                    return processIntBase(BigInt(std::get<int32_t>(baseNum->value)));
                } else if (std::holds_alternative<BigInt>(baseNum->value)) {
                    return processIntBase(std::get<BigInt>(baseNum->value));
                } else if (std::holds_alternative<Fraction>(baseNum->value)) {
                    Fraction baseF = std::get<Fraction>(baseNum->value);
                    SymExpr numRes = processIntBase(baseF.getNum());
                    SymExpr denRes = processIntBase(baseF.getDen());
                    return numRes / denRes;
                } else if (std::holds_alternative<double>(baseNum->value)) {
                    try {
                        Value baseVal = casValToValue(baseNum->value);
                        Value expVal = casValToValue(expNum->value);
                        Value result = baseVal ^ expVal;
                        
                        if (result.isFloat() && std::isnan(result.asFloatRaw())) {
                            std::complex<double> bc(baseVal.asFloat(), 0.0);
                            std::complex<double> ec(expVal.asFloat(), 0.0);
                            std::complex<double> cres = std::pow(bc, ec);
                            if (cres.imag() == 0.0) return SymExpr(cres.real());
                            return SymExpr(Complex(cres.real(), cres.imag()));
                        }
                        
                        return result.asSymbolic();
                    } catch (const EngineInterruptError&) {
                        throw;
                    } catch (...) {}
                }
            } else if (std::holds_alternative<double>(expNum->value)) {
                try {
                    Value baseVal = casValToValue(baseNum->value);
                    Value expVal = casValToValue(expNum->value);
                    Value result = baseVal ^ expVal;
                        
                    if (result.isFloat() && std::isnan(result.asFloatRaw())) {
                        std::complex<double> bc(baseVal.asFloat(), 0.0);
                        std::complex<double> ec(expVal.asFloat(), 0.0);
                        std::complex<double> cres = std::pow(bc, ec);
                        if (cres.imag() == 0.0) return SymExpr(cres.real());
                        return SymExpr(Complex(cres.real(), cres.imag()));
                    }
                        
                    return result.asSymbolic();
                } catch (const EngineInterruptError&) {
                    throw;
                } catch (...) {}
            }
            // 非整数指数保留符号形式（由 Value 的升维机制自动保障）
        }

        // 假分数指数拆分: x^(3/2) -> x * x^(1/2)
        if (b.ptr->getType() == SymType::NUM) {
            auto expNum = static_cast<SymNum*>(b.ptr);
            if (std::holds_alternative<Fraction>(expNum->value)) {
                Fraction expF = std::get<Fraction>(expNum->value);
                BigInt m = expF.getNum();
                BigInt n_den = expF.getDen();
                if (n_den > BigInt(1) && m.abs() > n_den) {
                    BigInt q = m / n_den;
                    BigInt r = m % n_den;
                    if (r.isNegative()) {
                        r = r + n_den;
                        q = q - BigInt(1);
                    }
                    if (!q.isZero() && !r.isZero() && fracDecompDepth < 8) {
                        // ★ 与整数底数分支同样的护栏：part1*part2 会把同底幂指数重新相加，
                        //   可能还原出原来的 m/n（x^(3/2) → x^1 * x^(1/2) → x^(3/2)），
                        //   造成无限递归；达到嵌套上限时直接落到下面的 makePow 构造幂节点。
                        struct VDecompGuard {
                            int& d;
                            VDecompGuard(int& x) : d(x) { ++d; }
                            ~VDecompGuard() { --d; }
                        } vdg(fracDecompDepth);
                        SymExpr part1 = a ^ SymExpr(q);
                        SymExpr part2 = a ^ SymExpr(Fraction(r, n_den));
                        return part1 * part2;
                    }
                }
            }
        }

        // 幂的幂法则: (a^m)^n = a^(m*n)
        if (a.ptr->getType() == SymType::POW) {
            auto powNode = static_cast<SymPow*>(a.ptr);
            SymExpr newExp = SymExpr(powNode->exp) * b;
            return SymExpr(powNode->base) ^ newExp;
        }

        // 乘积分配律: (a*b*c)^n = a^n * b^n * c^n
        if (a.ptr->getType() == SymType::MUL && b.ptr->getType() == SymType::NUM) {
            auto mulNode = static_cast<SymMul*>(a.ptr);
            SymExpr result(BigInt(1));
            for (auto& factor : mulNode->args)
                result = result * (SymExpr(factor) ^ b);
            return result;
        }

        return SymExpr::makePow(a.ptr, b.ptr);
    }

    
    // =================================================================
    // AST 体积计算器
    // =================================================================
    static int countNodes(SymNode* node, std::unordered_set<const SymNode*>& visited) {
        if (!node) return 0;
        if (!visited.insert(node).second) return 0;
        int count = 1;
        switch (node->getType()) {
        case SymType::ADD:
            for (auto& arg : static_cast<SymAdd*>(node)->args)
                count += countNodes(arg, visited);
            break;
        case SymType::MUL:
            for (auto& arg : static_cast<SymMul*>(node)->args)
                count += countNodes(arg, visited);
            break;
        case SymType::POW:
            count += countNodes(static_cast<SymPow*>(node)->base, visited);
            count += countNodes(static_cast<SymPow*>(node)->exp, visited);
            break;
        case SymType::FUNC:
            for (auto& arg : static_cast<SymFunc*>(node)->args)
                count += countNodes(arg, visited);
            break;
        default: break;
        }
        return count;
    }

    int getAstNodeCount(const SymExpr& expr) {
        std::unordered_set<const SymNode*> visited;
        return countNodes(expr.ptr, visited);
    }

    // =================================================================
    // AST 复杂度计算器 (用于多重宇宙最优解选择)
    // =================================================================
    // ★ 重复子表达式必须按出现次数重复计分。
    //   原实现用一个 visited 集合，第二次遇到同一个节点直接返回 0 分；而节点是
    //   内部化的 DAG，于是"把同一个分母写两遍"的形式反而显得更小：
    //       x*(x-1)^(-1) + (x-1)^(-1)   ← 共享 (x-1)^(-1)，旧算法给 62 分
    //       (x+1)*(x-1)^(-1)            ← 旧算法给 72 分
    //   多重宇宙因此选中前者，而前者再化简又会变成后者 —— cas.simplify 不幂等，
    //   同一个表达式走不同路径收敛到不同的树，下游算法就无法靠树形判等去重。
    //   改成用 memo 缓存"单个子树的分值"，重复引用时**再加一次**，得到的是把 DAG
    //   按树展开后的真实规模，时间仍是 O(DAG 节点数)。
    //   ★ 展开后的分值可能极大（(x+1)^n 展开的 DAG 展开成树是 2^n 量级），用
    //     int64 并在上限处饱和，避免有符号溢出（饱和只影响"都很大"的候选之间的
    //     相对次序，而那类表达式本来就不是多重宇宙要挑的对象）。
    static constexpr int64_t kComplexityCap = 1'000'000'000LL;

    static int64_t computeComplexity(SymNode* node, std::unordered_map<const SymNode*, int64_t>& memo, int depth) {
        if (!node || depth > 512) return 0;   // 纵深护栏（DAG 无环，这里只防意外）
        auto it = memo.find(node);
        if (it != memo.end()) return it->second;
        int64_t score = 10; // 基础分放大，便于微调
        switch (node->getType()) {
        case SymType::NUM: {
            auto num = static_cast<SymNum*>(node);
            if (std::holds_alternative<Fraction>(num->value)) score += 15; 
            else if (std::holds_alternative<double>(num->value)) score += 20; 
            break;
        }
        case SymType::VAR:
            break;
        case SymType::ADD:
            score += 5; 
            for (auto& arg : static_cast<SymAdd*>(node)->args)
                score += computeComplexity(arg, memo, depth + 1);
            break;
        case SymType::MUL:
            score += 2;
            for (auto& arg : static_cast<SymMul*>(node)->args)
                score += computeComplexity(arg, memo, depth + 1);
            break;
        case SymType::POW: {
            score += 10; 
            auto powNode = static_cast<SymPow*>(node);
            score += computeComplexity(powNode->base, memo, depth + 1);
            score += computeComplexity(powNode->exp, memo, depth + 1);
            if (powNode->exp->getType() == SymType::NUM) {
                auto numVal = static_cast<SymNum*>(powNode->exp)->value;
                if (isCasNegative(numVal)) score += 15; // 负指数（分母）惩罚
                if (std::holds_alternative<Fraction>(numVal)) score += 20; // 根式惩罚
            } else if (powNode->exp->getType() != SymType::VAR) {
                score += 20; // 复杂指数惩罚（表达式在幂次上方）
            }
            break;
        }
        case SymType::FUNC:
            score += 25; 
            for (auto& arg : static_cast<SymFunc*>(node)->args)
                score += computeComplexity(arg, memo, depth + 1);
            break;
        default: break;
        }
        if (score > kComplexityCap) score = kComplexityCap;
        memo.emplace(node, score);
        return score;
    }

    int getAstComplexity(const SymExpr& expr) {
        std::unordered_map<const SymNode*, int64_t> memo;
        int64_t v = computeComplexity(expr.ptr, memo, 0);
        return static_cast<int>(v > kComplexityCap ? kComplexityCap : v);
    }

    // =================================================================
    // 变量深度探测器 (Variable Submergence Check)
    // =================================================================
    static int calcVarDepth(SymNode* node, const std::string& var, int currentDepth) {
        if (!node) return -1;
        switch (node->getType()) {
            case SymType::NUM: return -1;
            case SymType::VAR: 
                if (static_cast<SymVar*>(node)->name == var) return currentDepth;
                return -1;
            case SymType::ADD: {
                int maxD = -1;
                for (auto& arg : static_cast<SymAdd*>(node)->args) {
                    maxD = std::max(maxD, calcVarDepth(arg, var, currentDepth + 1));
                }
                return maxD;
            }
            case SymType::MUL: {
                int maxD = -1;
                for (auto& arg : static_cast<SymMul*>(node)->args) {
                    maxD = std::max(maxD, calcVarDepth(arg, var, currentDepth + 1));
                }
                return maxD;
            }
            case SymType::POW: {
                auto p = static_cast<SymPow*>(node);
                return std::max(calcVarDepth(p->base, var, currentDepth + 1), calcVarDepth(p->exp, var, currentDepth + 1));
            }
            case SymType::FUNC: {
                int maxD = -1;
                for (auto& arg : static_cast<SymFunc*>(node)->args) {
                    maxD = std::max(maxD, calcVarDepth(arg, var, currentDepth + 1));
                }
                return maxD;
            }
        }
        return -1;
    }

    int getVarDepth(const SymExpr& expr, const std::string& var) {
        return calcVarDepth(expr.ptr, var, 0);
    }

    // =================================================================
    // 超越函数嵌套权重 (Transcendental Extension Penalty)
    // =================================================================
    static int calcTransWeight(SymNode* node, const std::string& var, int currentNesting) {
        if (!node) return 0;
        if (!containsVar(node, var)) return 0;

        switch (node->getType()) {
            case SymType::NUM:
            case SymType::VAR:
                return 0;
            case SymType::ADD: {
                int w = 0;
                for (auto& arg : static_cast<SymAdd*>(node)->args) w += calcTransWeight(arg, var, currentNesting);
                return w;
            }
            case SymType::MUL: {
                int w = 0;
                for (auto& arg : static_cast<SymMul*>(node)->args) w += calcTransWeight(arg, var, currentNesting);
                return w;
            }
            case SymType::POW: {
                auto p = static_cast<SymPow*>(node);
                int w = 0;
                if (containsVar(p->exp, var)) {
                    w += (currentNesting + 1) * 2;
                    w += calcTransWeight(p->base, var, currentNesting + 1);
                    w += calcTransWeight(p->exp, var, currentNesting + 1);
                } else {
                    w += calcTransWeight(p->base, var, currentNesting);
                    w += calcTransWeight(p->exp, var, currentNesting);
                }
                return w;
            }
            case SymType::FUNC: {
                auto f = static_cast<SymFunc*>(node);
                int w = (currentNesting + 1) * 2;
                for (auto& arg : f->args) {
                    w += calcTransWeight(arg, var, currentNesting + 1);
                }
                return w;
            }
        }
        return 0;
    }

    int getTranscendentalWeight(const SymExpr& expr, const std::string& var) {
        return calcTransWeight(expr.ptr, var, 0);
    }

    // =================================================================
    // 代数重写引擎 (Pattern Matching Engine)
    // =================================================================
    
    // 检查节点是否为万能通配符
    static bool isWildcard(SymNode* node, std::string& outName) {
        if (node && node->getType() == SymType::VAR) {
            std::string name = static_cast<SymVar*>(node)->name;
            if (!name.empty() && name[0] == '_') {
                outName = name;
                return true;
            }
        }
        return false;
    }

    // 检查节点子树中是否包含万能通配符
    static bool hasWildcard(SymNode* node) {
        if (!node) return false;
        if (node->getType() == SymType::VAR) {
            std::string name = static_cast<SymVar*>(node)->name;
            return !name.empty() && name[0] == '_';
        }
        switch (node->getType()) {
            case SymType::ADD:
                for (auto& a : static_cast<SymAdd*>(node)->args) if (hasWildcard(a)) return true;
                break;
            case SymType::MUL:
                for (auto& a : static_cast<SymMul*>(node)->args) if (hasWildcard(a)) return true;
                break;
            case SymType::POW:
                return hasWildcard(static_cast<SymPow*>(node)->base) || hasWildcard(static_cast<SymPow*>(node)->exp);
            case SymType::FUNC:
                for (auto& a : static_cast<SymFunc*>(node)->args) if (hasWildcard(a)) return true;
                break;
            default: break;
        }
        return false;
    }

    // 精确匹配 AST 并记录捕获变量 (支持 ADD/MUL 的全排列交换律检测)
    static bool matchASTImpl(SymNode* node, SymNode* pat, std::map<std::string, SymExpr>& captures, std::vector<std::string>& addedKeys) {
        checkInterrupt();
        if (!node || !pat) return false;
        
        std::string wcName;
        // 1. 若 pat 是通配符
        if (isWildcard(pat, wcName)) {
            auto it = captures.find(wcName);
            if (it != captures.end()) {
                // ★ 必须用结构比较，不能用 SymExpr::operator==。
                //   后者现在是完整代数等价判定（内部会 simplify + expand），
                //   而 simplify 又会调用 trigsimp → applyRule → matchASTImpl，
                //   形成重入：匹配过程中再次进入匹配与化简，破坏捕获表/节点状态，
                //   表现为 cas.trigsimp(x^2 + 2*x) 访问越界崩溃。
                //   模式匹配只需要"形状相同"，结构比较语义上也是正确选择。
                if (it->second.ptr == node) return true;
                return it->second.ptr->equals(node);
            } else {
                captures[wcName] = SymExpr(node);
                addedKeys.push_back(wcName);
                return true;
            }
        }
        
        // 2. 类型检查
        if (node->getType() != pat->getType()) return false;
        
        // 3 & 4 & 5. 常规分支与交换律分支
        switch (node->getType()) {
            case SymType::NUM:
                return node->toString() == pat->toString();
            case SymType::VAR:
                return static_cast<SymVar*>(node)->name == static_cast<SymVar*>(pat)->name;
            case SymType::POW: {
                auto pNode = static_cast<SymPow*>(node);
                auto pPat = static_cast<SymPow*>(pat);
                
                // 严格限制：如果模板的指数是常数，目标表达式的指数必须完全一致
                if (pPat->exp->getType() == SymType::NUM) {
                    if (pNode->exp->getType() != SymType::NUM) return false;
                    if (pNode->exp->toString() != pPat->exp->toString()) return false;
                }
                
                size_t initialAdded = addedKeys.size();
                if (matchASTImpl(pNode->base, pPat->base, captures, addedKeys)) {
                    if (matchASTImpl(pNode->exp, pPat->exp, captures, addedKeys)) {
                        return true;
                    }
                }
                while (addedKeys.size() > initialAdded) {
                    captures.erase(addedKeys.back());
                    addedKeys.pop_back();
                }
                return false;
            }
            case SymType::FUNC: {
                auto fNode = static_cast<SymFunc*>(node);
                auto fPat = static_cast<SymFunc*>(pat);
                if (fNode->name != fPat->name || fNode->args.size() != fPat->args.size()) return false;
                size_t initialAdded = addedKeys.size();
                for (size_t i = 0; i < fNode->args.size(); ++i) {
                    if (!matchASTImpl(fNode->args[i], fPat->args[i], captures, addedKeys)) {
                        while (addedKeys.size() > initialAdded) {
                            captures.erase(addedKeys.back());
                            addedKeys.pop_back();
                        }
                        return false;
                    }
                }
                return true;
            }
            case SymType::ADD:
            case SymType::MUL: {
                const auto& nArgs = (node->getType() == SymType::ADD) ? static_cast<SymAdd*>(node)->args : static_cast<SymMul*>(node)->args;
                const auto& pArgs = (pat->getType() == SymType::ADD) ? static_cast<SymAdd*>(pat)->args : static_cast<SymMul*>(pat)->args;
                
                if (nArgs.size() != pArgs.size()) return false;
                
                // 预处理：精确匹配抵消 (剔除不含通配符的相同项)
                std::vector<bool> nUsed(nArgs.size(), false);
                std::vector<bool> pUsed(pArgs.size(), false);
                int matchCount = 0;

                for (size_t j = 0; j < pArgs.size(); ++j) {
                    if (!hasWildcard(pArgs[j])) {
                        for (size_t i = 0; i < nArgs.size(); ++i) {
                            if (nUsed[i]) continue;
                            // 同 1 处：结构比较，避免重入完整等价判定
                            if (nArgs[i] == pArgs[j] || nArgs[i]->equals(pArgs[j])) {
                                nUsed[i] = true;
                                pUsed[j] = true;
                                matchCount++;
                                break;
                            }
                        }
                    }
                }

                if (matchCount == nArgs.size()) return true;

                // 提取剩余的待匹配项
                std::vector<SymNode*> remN, remP;
                for (size_t i = 0; i < nArgs.size(); ++i) if (!nUsed[i]) remN.push_back(nArgs[i]);
                for (size_t j = 0; j < pArgs.size(); ++j) if (!pUsed[j]) remP.push_back(pArgs[j]);

                // DFS 回溯匹配剩余项 (带剪枝)
                std::vector<bool> remNUsed(remN.size(), false);
                std::function<bool(size_t)> dfs = [&](size_t pIdx) -> bool {
                    if (pIdx == remP.size()) return true;
                    size_t initialAdded = addedKeys.size();
                    for (size_t i = 0; i < remN.size(); ++i) {
                        if (!remNUsed[i]) {
                            if (matchASTImpl(remN[i], remP[pIdx], captures, addedKeys)) {
                                remNUsed[i] = true;
                                if (dfs(pIdx + 1)) {
                                    return true;
                                }
                                remNUsed[i] = false;
                                while (addedKeys.size() > initialAdded) {
                                    captures.erase(addedKeys.back());
                                    addedKeys.pop_back();
                                }
                            }
                        }
                    }
                    return false;
                };

                return dfs(0);
            }
        }
        return false;
    }

    bool matchAST(SymNode* node, SymNode* pat, std::map<std::string, SymExpr>& captures) {
        std::vector<std::string> addedKeys;
        bool res = matchASTImpl(node, pat, captures, addedKeys);
        if (!res) {
            for (const auto& k : addedKeys) captures.erase(k);
        }
        return res;
    }

    // 将捕获的 AST 塞回目标模板中
    SymExpr substituteCaptures(const SymExpr& target, const std::map<std::string, SymExpr>& captures) {
        if (!target.ptr) return target;
        
        std::string wcName;
        if (isWildcard(target.ptr, wcName)) {
            auto it = captures.find(wcName);
            if (it != captures.end()) return it->second;
            return target;
        }
        
        switch (target.ptr->getType()) {
            case SymType::NUM:
            case SymType::VAR:
                return target;
            case SymType::ADD: {
                auto add = static_cast<SymAdd*>(target.ptr);
                SymExpr res(BigInt(0));
                for (auto& arg : add->args) res = res + substituteCaptures(SymExpr(arg), captures);
                return res;
            }
            case SymType::MUL: {
                auto mul = static_cast<SymMul*>(target.ptr);
                SymExpr res(BigInt(1));
                for (auto& arg : mul->args) res = res * substituteCaptures(SymExpr(arg), captures);
                return res;
            }
            case SymType::POW: {
                auto pow = static_cast<SymPow*>(target.ptr);
                return substituteCaptures(SymExpr(pow->base), captures) ^ substituteCaptures(SymExpr(pow->exp), captures);
            }
            case SymType::FUNC: {
                auto func = static_cast<SymFunc*>(target.ptr);
                std::vector<SymNode*> newArgs;
                for (auto& arg : func->args) newArgs.push_back(substituteCaptures(SymExpr(arg), captures).ptr);
                return SymExpr::makeFunc(func->name, std::move(newArgs));
            }
        }
        return target;
    }

    // 核心入口：后序遍历并尝试重写
    SymExpr applyRule(const SymExpr& expr, const SymExpr& pattern, const SymExpr& target) {
        checkInterrupt();
        if (!expr.ptr) return expr;
        
        // 1. 无情探底（Post-order traversal）
        SymExpr current = expr;
        bool childChanged = false;
        
        switch (expr.ptr->getType()) {
            case SymType::ADD: {
                auto add = static_cast<SymAdd*>(expr.ptr);
                std::vector<SymNode*> newArgs;
                for (auto& arg : add->args) {
                    SymExpr newArg = applyRule(SymExpr(arg), pattern, target);
                    newArgs.push_back(newArg.ptr);
                    if (newArg.ptr != arg) childChanged = true;
                }
                if (childChanged) {
                    SymExpr res(BigInt(0));
                    for (auto& arg : newArgs) res = res + SymExpr(arg);
                    current = res;
                }
                break;
            }
            case SymType::MUL: {
                auto mul = static_cast<SymMul*>(expr.ptr);
                std::vector<SymNode*> newArgs;
                for (auto& arg : mul->args) {
                    SymExpr newArg = applyRule(SymExpr(arg), pattern, target);
                    newArgs.push_back(newArg.ptr);
                    if (newArg.ptr != arg) childChanged = true;
                }
                if (childChanged) {
                    SymExpr res(BigInt(1));
                    for (auto& arg : newArgs) res = res * SymExpr(arg);
                    current = res;
                }
                break;
            }
            case SymType::POW: {
                auto pow = static_cast<SymPow*>(expr.ptr);
                SymExpr newBase = applyRule(SymExpr(pow->base), pattern, target);
                SymExpr newExp = applyRule(SymExpr(pow->exp), pattern, target);
                if (newBase.ptr != pow->base || newExp.ptr != pow->exp) {
                    current = newBase ^ newExp;
                }
                break;
            }
            case SymType::FUNC: {
                auto func = static_cast<SymFunc*>(expr.ptr);
                std::vector<SymNode*> newArgs;
                for (auto& arg : func->args) {
                    SymExpr newArg = applyRule(SymExpr(arg), pattern, target);
                    newArgs.push_back(newArg.ptr);
                    if (newArg.ptr != arg) childChanged = true;
                }
                if (childChanged) {
                    current = SymExpr::makeFunc(func->name, std::move(newArgs));
                }
                break;
            }
            default: break;
        }
        
        // 2. 尝试整体 matchAST
        std::map<std::string, SymExpr> captures;
        if (matchAST(current.ptr, pattern.ptr, captures)) {
            return substituteCaptures(target, captures);
        }
        
        // 3. 子集拦截匹配 (Subset Matching)
        if (current.ptr && pattern.ptr && 
            ((current.ptr->getType() == SymType::ADD && pattern.ptr->getType() == SymType::ADD) ||
             (current.ptr->getType() == SymType::MUL && pattern.ptr->getType() == SymType::MUL))) {
            
            const auto& cArgs = (current.ptr->getType() == SymType::ADD) ? static_cast<SymAdd*>(current.ptr)->args : static_cast<SymMul*>(current.ptr)->args;
            const auto& pArgs = (pattern.ptr->getType() == SymType::ADD) ? static_cast<SymAdd*>(pattern.ptr)->args : static_cast<SymMul*>(pattern.ptr)->args;
            
            size_t N = cArgs.size();
            size_t K = pArgs.size();
            
            if (N > K) {
                // 预处理：精确匹配抵消
                std::vector<bool> cUsed(N, false);
                std::vector<bool> pUsed(K, false);

                for (size_t j = 0; j < K; ++j) {
                    if (!hasWildcard(pArgs[j])) {
                        for (size_t i = 0; i < N; ++i) {
                            if (cUsed[i]) continue;
                            // 同 1 处：结构比较，避免重入完整等价判定
                            if (cArgs[i] == pArgs[j] || cArgs[i]->equals(pArgs[j])) {
                                cUsed[i] = true;
                                pUsed[j] = true;
                                break;
                            }
                        }
                    }
                }

                std::vector<SymNode*> remP;
                for (size_t j = 0; j < K; ++j) if (!pUsed[j]) remP.push_back(pArgs[j]);

                std::map<std::string, SymExpr> finalCaptures;
                std::vector<bool> finalCUsed;
                std::vector<std::string> addedKeys;

                // DFS 回溯寻找子集匹配
                std::function<bool(size_t, std::vector<bool>&)> dfsSubset = 
                    [&](size_t pIdx, std::vector<bool>& curCUsed) -> bool {
                    if (pIdx == remP.size()) {
                        finalCUsed = curCUsed;
                        return true;
                    }
                    size_t initialAdded = addedKeys.size();
                    for (size_t i = 0; i < N; ++i) {
                        if (!curCUsed[i]) {
                            if (matchASTImpl(cArgs[i], remP[pIdx], finalCaptures, addedKeys)) {
                                curCUsed[i] = true;
                                if (dfsSubset(pIdx + 1, curCUsed)) return true;
                                curCUsed[i] = false;
                                while (addedKeys.size() > initialAdded) {
                                    finalCaptures.erase(addedKeys.back());
                                    addedKeys.pop_back();
                                }
                            }
                        }
                    }
                    return false;
                };

                if (dfsSubset(0, cUsed)) {
                    SymExpr replaced = substituteCaptures(target, finalCaptures);
                    std::vector<SymNode*> remaining;
                    for (size_t i = 0; i < N; ++i) {
                        if (!finalCUsed[i]) remaining.push_back(cArgs[i]);
                    }
                    if (current.ptr->getType() == SymType::ADD) {
                        SymExpr res = replaced;
                        for (auto& rem : remaining) res = res + SymExpr(rem);
                        return res;
                    } else {
                        SymExpr res = replaced;
                        for (auto& rem : remaining) res = res * SymExpr(rem);
                        return res;
                    }
                }
            }
        }
        
        return current;
    }

    // =================================================================
// 符号展开：带有防爆截断额度 maxPowTerms
// =================================================================
    static SymExpr expand_internal(const SymExpr& expr, int64_t maxPowTerms, bool distributeNonIntPow) {
        checkInterrupt();
        if (!expr.ptr) return expr;
        if (maxPowTerms <= 0) maxPowTerms = SymConfig::maxExpandTerms;

        struct ExpandKey {
            SymNode* ptr;
            int64_t maxPowTerms;
            bool distributeNonIntPow;
            bool operator==(const ExpandKey& o) const {
                return ptr == o.ptr && maxPowTerms == o.maxPowTerms && distributeNonIntPow == o.distributeNonIntPow;
            }
        };
        struct ExpandKeyHash {
            size_t operator()(const ExpandKey& k) const {
                return hashCombine(reinterpret_cast<uint64_t>(k.ptr), hashCombine(static_cast<uint64_t>(k.maxPowTerms), k.distributeNonIntPow ? 1 : 0));
            }
        };

        static thread_local std::unordered_map<ExpandKey, SymExpr, ExpandKeyHash> cache;
        static thread_local int depth = 0;

        ExpandKey sig = {expr.ptr, maxPowTerms, distributeNonIntPow};
        if (depth > 0) {
            auto it = cache.find(sig);
            if (it != cache.end()) return it->second;
        } else {
            cache.clear();
        }

        struct DepthGuard {
            int& d;
            DepthGuard(int& depth_ref) : d(depth_ref) { d++; }
            ~DepthGuard() { d--; }
        } guard(depth);

        auto compute = [&]() -> SymExpr {
            switch (expr.ptr->getType()) {
        case SymType::NUM:
        case SymType::VAR:
            return expr;

        case SymType::ADD: {
            SymExpr result(BigInt(0));
            // 向下传递 maxPowTerms
            for (auto& arg : static_cast<SymAdd*>(expr.ptr)->args) {
                result = result + expand_internal(SymExpr(arg), maxPowTerms, distributeNonIntPow);
            }
            return result;
        }

        case SymType::MUL: {
            auto mulNode = static_cast<SymMul*>(expr.ptr);
            SymExpr result(BigInt(1));
            
            for (auto& arg : mulNode->args) {
                SymExpr factor = expand_internal(SymExpr(arg), maxPowTerms, distributeNonIntPow);
                
                std::vector<SymNode*> leftTerms;
                if (result.ptr->getType() == SymType::ADD) {
                    for (auto& a : static_cast<SymAdd*>(result.ptr)->args) leftTerms.push_back(a);
                } else if (!result.isZero()) {
                    leftTerms.push_back(result.ptr);
                }
                
                std::vector<SymNode*> rightTerms;
                if (factor.ptr->getType() == SymType::ADD) {
                    for (auto& a : static_cast<SymAdd*>(factor.ptr)->args) rightTerms.push_back(a);
                } else if (!factor.isZero()) {
                    rightTerms.push_back(factor.ptr);
                }
                
                if (leftTerms.empty() || rightTerms.empty()) {
                    result = SymExpr(BigInt(0));
                    continue;
                }
                
                std::vector<SymNode*> nextTerms;
                nextTerms.reserve(leftTerms.size() * rightTerms.size());
                for (auto& l : leftTerms) {
                    for (auto& r : rightTerms) {
                        nextTerms.push_back((SymExpr(l) * SymExpr(r)).ptr);
                    }
                }
                
                if (nextTerms.size() > static_cast<size_t>(maxPowTerms)) {
                    JC2_THROW(MathError, "Expansion exceeded max terms limit.");
                }
                
                // 过滤零项并利用 operator+ 的展平与合并机制，一次性合并所有项！
                std::vector<SymNode*> nonZeroTerms;
                nonZeroTerms.reserve(nextTerms.size());
                for (auto& t : nextTerms) {
                    if (!SymExpr(t).isZero()) nonZeroTerms.push_back(t);
                }
                
                if (nonZeroTerms.empty()) {
                    result = SymExpr(BigInt(0));
                } else if (nonZeroTerms.size() == 1) {
                    result = SymExpr(nonZeroTerms[0]);
                } else {
                    SymExpr firstTerm(nonZeroTerms[0]);
                    nonZeroTerms.erase(nonZeroTerms.begin());
                    SymExpr restAdd(new SymAdd(std::move(nonZeroTerms)));
                    // 强制触发 operator+ 的 O(N) 同类项合并
                    result = firstTerm + restAdd;
                }
            }
            return result;
        }
        // ─────────────────────────────────────────────
        // 幂次展开：多项式定理极限优化版 (去除一切多余冗余)
        // ─────────────────────────────────────────────
        case SymType::POW: {
            auto powNode = static_cast<SymPow*>(expr.ptr);
            SymExpr baseExp = expand_internal(SymExpr(powNode->base), maxPowTerms, distributeNonIntPow);
            SymExpr expExp = expand_internal(SymExpr(powNode->exp), maxPowTerms, distributeNonIntPow);

            if (expExp.ptr->getType() == SymType::NUM) {
                auto numNode = static_cast<SymNum*>(expExp.ptr);
                auto [isInt, n] = extractExactInt(numNode->value);

                if (isInt && n >= 0 && n <= 1000) {
                    if (baseExp.ptr->getType() == SymType::ADD) {
                        auto addNode = static_cast<SymAdd*>(baseExp.ptr);
                        int m = static_cast<int>(addNode->args.size());

                        BigInt T(1);
                        for (int i = 1; i <= m - 1; ++i) {
                            T = (T * BigInt(n + m - i)) / BigInt(i);
                        }

                        if (T <= BigInt(maxPowTerms)) {
                            // ★ 优化 1：使用容器集中暂存节点，彻底避开 operator+ 的 O(N^2) 合并风暴
                            std::vector<SymNode*> finalTerms;
                            finalTerms.reserve(static_cast<size_t>(T.toFloat()));

                            // ★ 优化 2：针对二项式的光速直通车 (m = 2)，0 次大数阶乘运算
                            if (m == 2) {
                                SymExpr A(addNode->args[0]);
                                SymExpr B(addNode->args[1]);
                                BigInt C(1);

                                for (int64_t k = 0; k <= n; ++k) {
                                    SymExpr term(C);
                                    if (n - k > 0) term = term * (A ^ SymExpr(BigInt(n - k)));
                                    if (k > 0)     term = term * (B ^ SymExpr(BigInt(k)));

                                    finalTerms.push_back(term.ptr); // 直接存入底层节点

                                    // O(1) 光速增量更新组合系数
                                    C = (C * BigInt(n - k)) / BigInt(k + 1);
                                }
                            }
                            // ★ 优化 3：高级多元多项式引擎 (m > 2)，引入预备阶乘查表优化
                            else {
                                // 一次性准备好所需的所有阶乘数据，彻底消灭重复劳动
                                std::vector<BigInt> facts(n + 1, BigInt(1));
                                for (int64_t i = 1; i <= n; ++i) {
                                    facts[i] = facts[i - 1] * BigInt(i);
                                }

                                std::vector<int64_t> ks;
                                ks.reserve(m);

                                std::function<void(int, int64_t)> generateMulti = [&](int varIdx, int64_t remainN) {
                                    if (varIdx == m - 1) {
                                        ks.push_back(remainN);

                                        // 查表级极速获取系数：n! / (k1! * k2! ...)
                                        BigInt coeff = facts[n];
                                        for (int64_t k : ks) {
                                            if (k > 1) coeff = coeff / facts[k];
                                        }

                                        SymExpr term(coeff);
                                        for (int i = 0; i < m; ++i) {
                                            if (ks[i] > 0) {
                                                term = term * (SymExpr(addNode->args[i]) ^ SymExpr(BigInt(ks[i])));
                                            }
                                        }

                                        finalTerms.push_back(term.ptr); // 直入节点池
                                        ks.pop_back();
                                        return;
                                    }

                                    for (int64_t i = 0; i <= remainN; ++i) {
                                        ks.push_back(i);
                                        generateMulti(varIdx + 1, remainN - i);
                                        ks.pop_back();
                                    }
                                    };
                                generateMulti(0, n);
                            }

                            // 极速组装：一口气将几十上百个节点封入一把加法树中，省去全部树并排开销！
                            std::sort(finalTerms.begin(), finalTerms.end(), [](const auto& a, const auto& b) {
                                return a->hashValue > b->hashValue;
                            });
                            if (finalTerms.size() == 1) return SymExpr(finalTerms[0]);
                            return SymExpr::makeAdd(std::move(finalTerms));
                        }
                    }

                    if (baseExp.ptr->getType() == SymType::MUL) {
                        if (isInt) {
                            auto mulNode = static_cast<SymMul*>(baseExp.ptr);
                            SymExpr result(BigInt(1));
                            for (auto& arg : mulNode->args) {
                                result = result * expand_internal(SymExpr(arg) ^ expExp, maxPowTerms, distributeNonIntPow);
                            }
                            return result;
                        }
                    }
                }
            }
            if (distributeNonIntPow && baseExp.ptr->getType() == SymType::MUL) {
                auto mulNode = static_cast<SymMul*>(baseExp.ptr);
                SymExpr result(BigInt(1));
                for (auto& arg : mulNode->args) {
                    result = result * expand_internal(SymExpr(arg) ^ expExp, maxPowTerms, distributeNonIntPow);
                }
                return result;
            }
            return baseExp ^ expExp;
        }

        case SymType::FUNC: {
            auto func = static_cast<SymFunc*>(expr.ptr);
            std::vector<SymNode*> expArgs;
            for (auto& arg : func->args) {
                expArgs.push_back(expand_internal(SymExpr(arg), maxPowTerms, distributeNonIntPow).ptr); // 传递
            }

            if ((func->name == "sin" || func->name == "cos") && expArgs.size() == 1) {
                SymExpr inner(expArgs[0]);
                
                // 1. 和差公式展开: sin(A + B) = sin(A)cos(B) + cos(A)sin(B)
                if (inner.ptr->getType() == SymType::ADD) {
                    auto add = static_cast<SymAdd*>(inner.ptr);
                    SymExpr A(add->args[0]);
                    std::vector<SymNode*> restArgs(add->args.begin() + 1, add->args.end());
                    SymExpr B = (restArgs.size() == 1) ? SymExpr(restArgs[0]) : SymExpr::makeAdd(restArgs);
                    
                    SymExpr sinA(new SymFunc("sin", std::vector<SymNode*>{A.ptr}));
                    SymExpr cosA(new SymFunc("cos", std::vector<SymNode*>{A.ptr}));
                    SymExpr sinB(new SymFunc("sin", std::vector<SymNode*>{B.ptr}));
                    SymExpr cosB(new SymFunc("cos", std::vector<SymNode*>{B.ptr}));
                    
                    if (func->name == "sin") return expand_internal(sinA * cosB + cosA * sinB, maxPowTerms, distributeNonIntPow);
                    if (func->name == "cos") return expand_internal(cosA * cosB - sinA * sinB, maxPowTerms, distributeNonIntPow);
                }
                
                // 2. 倍角公式展开: sin(n*x)
                if (inner.ptr->getType() == SymType::MUL) {
                    auto mul = static_cast<SymMul*>(inner.ptr);
                    if (mul->args[0]->getType() == SymType::NUM) {
                        auto [isInt, n] = extractExactInt(static_cast<SymNum*>(mul->args[0])->value);
                        if (isInt) {
                            if (n < 0) {
                                SymExpr posInner = inner / SymExpr(BigInt(-1));
                                if (func->name == "sin") return expand_internal(-SymExpr::makeFunc("sin", std::vector<SymNode*>{posInner.ptr}), maxPowTerms, distributeNonIntPow);
                                if (func->name == "cos") return expand_internal(SymExpr::makeFunc("cos", std::vector<SymNode*>{posInner.ptr}), maxPowTerms, distributeNonIntPow);
                            }
                            else if (n > 1) {
                                SymExpr X = inner / SymExpr(BigInt(n));
                                SymExpr A = SymExpr(BigInt(n - 1)) * X;
                                SymExpr B = X;
                                
                                SymExpr sinA(new SymFunc("sin", std::vector<SymNode*>{A.ptr}));
                                SymExpr cosA(new SymFunc("cos", std::vector<SymNode*>{A.ptr}));
                                SymExpr sinB(new SymFunc("sin", std::vector<SymNode*>{B.ptr}));
                                SymExpr cosB(new SymFunc("cos", std::vector<SymNode*>{B.ptr}));
                                
                                if (func->name == "sin") return expand_internal(sinA * cosB + cosA * sinB, maxPowTerms, distributeNonIntPow);
                                if (func->name == "cos") return expand_internal(cosA * cosB - sinA * sinB, maxPowTerms, distributeNonIntPow);
                            }
                        }
                    }
                }
            }

            if (func->name == "log" && expArgs.size() == 1) {
                SymExpr inner(expArgs[0]);

                if (inner.ptr->getType() == SymType::MUL) {
                    auto mul = static_cast<SymMul*>(inner.ptr);
                    SymExpr sum(BigInt(0));
                    for (auto& factor : mul->args) {
                        SymExpr subLog(new SymFunc(func->name, std::vector<SymNode*>{ factor }));
                        sum = sum + expand_internal(subLog, maxPowTerms, distributeNonIntPow); // 传递
                    }
                    return sum;
                }

                if (inner.ptr->getType() == SymType::POW) {
                    auto powN = static_cast<SymPow*>(inner.ptr);
                    SymExpr logA(new SymFunc(func->name, std::vector<SymNode*>{ powN->base }));
                    return expand_internal(SymExpr(powN->exp) * logA, maxPowTerms, distributeNonIntPow); // 传递
                }
            }
                return SymExpr::makeFunc(func->name, std::move(expArgs));
            }
            default:
                return expr;
            }
        };

        SymExpr result = compute();
        cache[sig] = result;
        return result;
    }

    SymExpr expand_core(const SymExpr& expr, int64_t maxPowTerms) {
        return expand_internal(expr, maxPowTerms, false);
    }

    SymExpr expand(const SymExpr& expr, int64_t maxPowTerms) {
        return expand_internal(expr, maxPowTerms, true);
    }

    // ★ 把和规范成"最高次项系数为正"：-x^2 + y^2 记成 -(x^2 - y^2)。
    //   恒等式 -(a - b) = b - a，只提取整体符号，不改值。排版层按降幂排列，
    //   最高次项在 args.back()，因此只看末项系数符号即可。
    //   缺这一步时 expand((x-y)*(x+y)) 给 x^2 - y^2、expand((y-x)*(x+y)) 给
    //   -x^2 + y^2，两者代数等价却判不出相等。只用于等价判定，不改变 expand 对外输出。
    static SymNode* normalizeLeadingSignForCompare(SymNode* node) {
        if (!node || node->getType() != SymType::ADD) return node;
        auto add = static_cast<SymAdd*>(node);
        if (add->args.empty()) return node;
        SymNode* last = add->args.back();
        bool leadingNeg = false;
        if (last->getType() == SymType::NUM) {
            leadingNeg = isCasNegative(static_cast<SymNum*>(last)->value);
        } else if (last->getType() == SymType::MUL) {
            auto mul = static_cast<SymMul*>(last);
            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                leadingNeg = isCasNegative(static_cast<SymNum*>(mul->args[0])->value);
            }
        }
        if (!leadingNeg) return node;
        SymExpr neg = SymExpr(node) * SymExpr(BigInt(-1));
        return neg.ptr;
    }

    static std::string canonicalCompareText(const SymExpr& e) {
        SymExpr ex = expand_core(e, 1024);
        SymNode* norm = normalizeLeadingSignForCompare(ex.ptr);
        SymExpr sm = simplifyCore(SymExpr(norm));
        SymExpr smx = expand_core(sm, 1024);
        SymNode* norm2 = normalizeLeadingSignForCompare(smx.ptr);
        return SymExpr(norm2).toString();
    }

    // =================================================================
    // 表达式等价判定
    //   1) 先做结构比较（节点自身的 equals，递归比对类型与子节点，按内部化身份比原子）
    //   2) 结构不等时回退到"展开成规范多项式"再比一次，覆盖分配律/交换律/结合律
    //      造成的等价写法（(x+1)(x-1) vs x^2-1、2(x+y) vs 2x+2y）。
    //   规模上限用于避免对超大表达式做代价失控的展开。
    // =================================================================
    // 表达式中是否含符号常量（i / pi / e）或浮点近似。
    // 这类表达式不能走"展开成规范多项式"的等价回退：i 的存在会让共轭根
    // 1/2(√3·i − 1) 与 1/2(−√3·i − 1) 在机械比较下被误判为相等，
    // 从而把 solve 的去重（用 == 判等）变成"吃掉不同的根"。
    static bool hasNonPolynomialAtom(SymNode* node, int depth = 0) {
        if (!node || depth > 64) return false;
        switch (node->getType()) {
        case SymType::CONST:
            return true;                       // pi / e / i
        case SymType::NUM:
            return std::holds_alternative<double>(static_cast<SymNum*>(node)->value);
        case SymType::VAR:
            return false;
        case SymType::ADD:
            for (auto& a : static_cast<SymAdd*>(node)->args)
                if (hasNonPolynomialAtom(a, depth + 1)) return true;
            return false;
        case SymType::MUL:
            for (auto& a : static_cast<SymMul*>(node)->args)
                if (hasNonPolynomialAtom(a, depth + 1)) return true;
            return false;
        case SymType::POW:
            return hasNonPolynomialAtom(static_cast<SymPow*>(node)->base, depth + 1) ||
                   hasNonPolynomialAtom(static_cast<SymPow*>(node)->exp, depth + 1);
        case SymType::FUNC:
            for (auto& a : static_cast<SymFunc*>(node)->args)
                if (hasNonPolynomialAtom(a, depth + 1)) return true;
            return false;
        }
        return true;
    }

    bool symEquivalent(const SymExpr& a, const SymExpr& b) {
        if (a.ptr == b.ptr) return true;
        if (!a.ptr || !b.ptr) return false;
        if (a.ptr->getType() == b.ptr->getType()) {
            if (a.ptr->equals(b.ptr)) return true;
        }
        // ★ 含 i/pi/e 或浮点近似时，只用结构比较，不做展开回退
        if (hasNonPolynomialAtom(a.ptr) || hasNonPolynomialAtom(b.ptr)) return false;
        constexpr int64_t kMaxNodesForExpand = 400;
        constexpr int64_t kMaxExpandTerms = 4000;
        if (getAstNodeCount(a) > kMaxNodesForExpand) return false;
        if (getAstNodeCount(b) > kMaxNodesForExpand) return false;
        try {
            SymExpr ea = expand_core(a, kMaxExpandTerms);
            SymExpr eb = expand_core(b, kMaxExpandTerms);
            if (ea.ptr == eb.ptr) return true;
            if (ea.ptr->equals(eb.ptr)) return true;
            // 展开后项序不同的情形（吸收交换律/结合律）
            if (ea.toString() == eb.toString()) return true;
            // ★ 规范文本比较：再叠加"最高次项系数为正"的符号归一
            //   （-x^2 + y^2 → -(x^2 - y^2)）与 simplify 级别的强归一。
            if (canonicalCompareText(a) == canonicalCompareText(b)) return true;
            return false;
        }
        catch (const EngineInterruptError&) {
            throw;
        }
        catch (...) {
            return false;
        }
    }

    static std::pair<bool, Value> tryEvalConst(const SymExpr& expr) {
        if (!expr.ptr) return {false, Value()};
        switch (expr.ptr->getType()) {
            case SymType::NUM:
                return {true, casValToValue(static_cast<SymNum*>(expr.ptr)->value)};
            case SymType::VAR: {
                auto name = static_cast<SymVar*>(expr.ptr)->name;
                return {false, Value()};
            }
            case SymType::CONST: {
                Complex _c = symbolicConstantValue(static_cast<SymConst*>(expr.ptr)->id);
                return {true, Value(_c)};
            }
            case SymType::ADD: {
                Value sum(0.0);
                for (auto& arg : static_cast<SymAdd*>(expr.ptr)->args) {
                    auto [ok, v] = tryEvalConst(SymExpr(arg));
                    if (!ok) return {false, Value()};
                    sum = sum + v;
                }
                if (!sum.isFloat() && !sum.isInt32() &&
                    !sum.isObjType(ObjType::BIGINT) &&
                    !sum.isObjType(ObjType::FRACTION) &&
                    !sum.isComplex()) {
                    return {false, Value()};
                }
                return {true, sum};
            }
            case SymType::MUL: {
                Value prod(1.0);
                for (auto& arg : static_cast<SymMul*>(expr.ptr)->args) {
                    auto [ok, v] = tryEvalConst(SymExpr(arg));
                    if (!ok) return {false, Value()};
                    prod = prod * v;
                }
                if (!prod.isFloat() && !prod.isInt32() &&
                    !prod.isObjType(ObjType::BIGINT) &&
                    !prod.isObjType(ObjType::FRACTION) &&
                    !prod.isComplex()) {
                    return {false, Value()};
                }
                return {true, prod};
            }
            case SymType::POW: {
                auto p = static_cast<SymPow*>(expr.ptr);
                auto [ok1, b] = tryEvalConst(SymExpr(p->base));
                if (!ok1) return {false, Value()};
                auto [ok2, e] = tryEvalConst(SymExpr(p->exp));
                if (!ok2) return {false, Value()};
                try {
                    Value res = b ^ e;
                    if (res.isFloat() && std::isnan(res.asFloatRaw())) {
                        double br = 0.0, bi = 0.0, er = 0.0, ei = 0.0;
                        bool ok_cast = true;
                        try {
                            if (b.isComplex()) { br = b.asComplex().real; bi = b.asComplex().imag; }
                            else { br = b.asFloat(); }
                            if (e.isComplex()) { er = e.asComplex().real; ei = e.asComplex().imag; }
                            else { er = e.asFloat(); }
                        } catch (const EngineInterruptError&) { throw; } catch (...) { ok_cast = false; }
                    
                        if (ok_cast) {
                            std::complex<double> bc(br, bi);
                            std::complex<double> ec(er, ei);
                            std::complex<double> cres = std::pow(bc, ec);
                            if (cres.imag() == 0.0) res = Value(cres.real());
                            else res = Value(Complex(cres.real(), cres.imag()));
                        }
                    }
                
                    if (!res.isFloat() && !res.isInt32() &&
                        !res.isObjType(ObjType::BIGINT) &&
                        !res.isObjType(ObjType::FRACTION) &&
                        !res.isComplex()) {
                        return {false, Value()};
                    }
                    return {true, res};
                } catch (const EngineInterruptError&) {
                    throw;
                } catch (...) {
                    return {false, Value()};
                }
            }
            case SymType::FUNC:
                return {false, Value()};
        }
        return {false, Value()};
    }

    // ==========================================
    // 代入引擎 (Substitution Engine)
    // ==========================================
    SymExpr subs(const SymExpr& expr, const std::string& var, const SymExpr& val) {
        checkInterrupt();
        if (!expr.ptr) return expr;

        switch (expr.ptr->getType()) {
        case SymType::NUM:
            return expr;

        case SymType::VAR: {
            auto v = static_cast<SymVar*>(expr.ptr);
            return (v->name == var) ? val : expr;
        }

        case SymType::ADD: {
            auto add = static_cast<SymAdd*>(expr.ptr);
            SymExpr result(BigInt(0));
            for (auto& arg : add->args)
                result = result + subs(SymExpr(arg), var, val);
            return result;
        }

        case SymType::MUL: {
            auto mul = static_cast<SymMul*>(expr.ptr);
            SymExpr result(BigInt(1));
            for (auto& arg : mul->args)
                result = result * subs(SymExpr(arg), var, val);
            return result;
        }

        case SymType::POW: {
            auto pow = static_cast<SymPow*>(expr.ptr);
            return subs(SymExpr(pow->base), var, val) ^ subs(SymExpr(pow->exp), var, val);
        }

        case SymType::FUNC: {
            auto func = static_cast<SymFunc*>(expr.ptr);
            if ((func->name == "RootOf" || func->name == "RootSum") && func->args.size() == 3) {
                if (func->args[1]->getType() == SymType::VAR) {
                    std::string dummy = static_cast<SymVar*>(func->args[1])->name;
                    if (var == dummy) return expr;
                }
                std::vector<SymNode*> newArgs;
                newArgs.push_back(subs(SymExpr(func->args[0]), var, val).ptr);
                newArgs.push_back(func->args[1]);
                if (func->name == "RootSum") {
                    newArgs.push_back(subs(SymExpr(func->args[2]), var, val).ptr);
                } else {
                    newArgs.push_back(func->args[2]);
                }
                return SymExpr::makeFunc(func->name, std::move(newArgs));
            }
            std::vector<SymNode*> newArgs;
            for (auto& arg : func->args)
                newArgs.push_back(subs(SymExpr(arg), var, val).ptr);
            return SymExpr::makeFunc(func->name, std::move(newArgs));
        }
        default:
            return expr;
        }
    }

    // ==========================================
    // 代数数节点数值求值引擎 (Durand-Kerner Method)
    // ==========================================
    static std::vector<std::complex<double>> findRootsNumeric(const std::vector<std::complex<double>>& numCoeffs) {
        int n = static_cast<int>(numCoeffs.size()) - 1;
        if (n < 1) JC2_THROW(MathError, "Degree < 1.");
        if (std::abs(numCoeffs[n]) < 1e-15) JC2_THROW(MathError, "Leading coefficient evaluated to zero.");

        std::vector<std::complex<double>> roots(n);
        std::complex<double> r(0.4, 0.9);
        std::complex<double> current(1.0, 0.0);
        for (int i = 0; i < n; ++i) {
            roots[i] = current;
            current *= r;
        }

        const int max_iter = 2000;
        for (int iter = 0; iter < max_iter; ++iter) {
            checkInterrupt();
            double max_diff = 0.0;
            for (int i = 0; i < n; ++i) {
                std::complex<double> p_val = 0.0;
                std::complex<double> z = roots[i];
                for (int j = n; j >= 0; --j) p_val = p_val * z + numCoeffs[j];
                if (std::abs(p_val) < 1e-15) continue;

                std::complex<double> den = numCoeffs[n];
                for (int j = 0; j < n; ++j) {
                    if (i != j) den *= (z - roots[j]);
                }
                if (std::abs(den) < 1e-15) continue;

                std::complex<double> diff = p_val / den;
                roots[i] -= diff;
                max_diff = std::max(max_diff, std::abs(diff));
            }
            if (max_diff < 1e-14) break;
        }

        std::sort(roots.begin(), roots.end(), [](const std::complex<double>& a, const std::complex<double>& b) {
            if (std::abs(a.real() - b.real()) > 1e-10) return a.real() < b.real();
            return a.imag() < b.imag();
        });
        return roots;
    }

    static Value evaluateRootNode(SymFunc* func, std::function<Value(const SymExpr&)> evalCb) {
        bool isRootOf = (func->name == "RootOf");
        SymExpr P = isRootOf ? SymExpr(func->args[0]) : SymExpr(func->args[2]);
        std::string dummy = static_cast<SymVar*>(func->args[1])->name;

        auto coeffs = extractCoeffs(P, dummy);
        if (coeffs.empty()) JC2_THROW(MathError, "Polynomial extraction failed for Root node.");

        std::vector<std::complex<double>> numCoeffs;
        for (const auto& c : coeffs) {
            Value cVal = evalCb(c);
            if (cVal.isComplex()) {
                auto cx = cVal.asComplex();
                numCoeffs.push_back({cx.real, cx.imag});
            } else {
                numCoeffs.push_back({cVal.asFloat(), 0.0});
            }
        }

        auto roots = findRootsNumeric(numCoeffs);
        int n = static_cast<int>(roots.size());

        if (isRootOf) {
            int k = 1;
            if (func->args[2]->getType() == SymType::NUM) {
                auto [isInt, val] = extractExactInt(static_cast<SymNum*>(func->args[2])->value);
                if (isInt) k = static_cast<int>(val);
            }
            if (k < 1 || k > n) JC2_THROW(MathError, "Root index " + std::to_string(k) + " out of bounds (1.." + std::to_string(n) + ").");
            auto root = roots[k - 1];
            if (std::abs(root.imag()) < 1e-12) return Value(root.real());
            return Value(Complex(root.real(), root.imag()));
        } else {
            SymExpr expr = SymExpr(func->args[0]);
            Value sum(0.0);
            for (int i = 0; i < n; ++i) {
                Value rootVal;
                if (std::abs(roots[i].imag()) < 1e-12) rootVal = Value(roots[i].real());
                else rootVal = Value(Complex(roots[i].real(), roots[i].imag()));
                
                auto subbed = subs(expr, dummy, rootVal.asSymbolic());
                sum = sum + evalCb(subbed);
            }
            return sum;
        }
    }

    // ==========================================
    // evalFloat: 全部强转 double（evalf 语义）
    // ==========================================
    SymExpr evalFloat(const SymExpr& expr) {
        if (!expr.ptr) return expr;

        auto [ok, val] = tryEvalConst(expr);
        if (ok) {
            try {
                if (val.isComplex()) {
                    Complex c = val.asComplex();
                    if (c.imag == 0.0) return SymExpr(c.real);
                    return SymExpr(c);
                } else {
                    return SymExpr(val.asFloat());
                }
            } catch (const EngineInterruptError&) {
                throw;
            } catch (...) {
                // 如果 asFloat 失败（例如 val 是 Symbolic 符号表达式），则回退到 AST 遍历
            }
        }

        switch (expr.ptr->getType()) {
        case SymType::NUM: {
            auto num = static_cast<SymNum*>(expr.ptr);
            return SymExpr(casValToValue(num->value).asFloat());
        }

        case SymType::VAR: {
            return expr;
        }
        case SymType::CONST: {
            return SymExpr(symbolicConstantValue(static_cast<SymConst*>(expr.ptr)->id));
        }

        case SymType::ADD: {
            auto add = static_cast<SymAdd*>(expr.ptr);
            SymExpr result(0.0);
            for (auto& arg : add->args)
                result = result + evalFloat(SymExpr(arg));
            return result;
        }

        case SymType::MUL: {
            auto mul = static_cast<SymMul*>(expr.ptr);
            SymExpr result(1.0);
            for (auto& arg : mul->args)
                result = result * evalFloat(SymExpr(arg));
            return result;
        }

        case SymType::POW: {
            auto pow = static_cast<SymPow*>(expr.ptr);
            SymExpr base = evalFloat(SymExpr(pow->base));
            SymExpr expn = evalFloat(SymExpr(pow->exp));
            return base ^ expn;
        }

        case SymType::FUNC: {
            auto func = static_cast<SymFunc*>(expr.ptr);
            if ((func->name == "RootOf" || func->name == "RootSum") && func->args.size() == 3) {
                bool isRootOf = (func->name == "RootOf");
                SymExpr P = isRootOf ? SymExpr(func->args[0]) : SymExpr(func->args[2]);
                std::string dummy = static_cast<SymVar*>(func->args[1])->name;
                auto coeffs = extractCoeffs(P, dummy);
                
                bool p_is_const = !coeffs.empty();
                std::vector<std::complex<double>> numCoeffs;
                if (p_is_const) {
                    for (const auto& c : coeffs) {
                        auto [c_ok, c_val] = tryEvalConst(c);
                        if (c_ok) {
                            if (c_val.isComplex()) numCoeffs.push_back({c_val.asComplex().real, c_val.asComplex().imag});
                            else numCoeffs.push_back({c_val.asFloat(), 0.0});
                        } else {
                            p_is_const = false;
                            break;
                        }
                    }
                }

                if (p_is_const) {
                    try {
                        auto roots = findRootsNumeric(numCoeffs);
                        int n = static_cast<int>(roots.size());
                        if (isRootOf) {
                            int k = 1;
                            if (func->args[2]->getType() == SymType::NUM) {
                                auto [isInt, k_val] = extractExactInt(static_cast<SymNum*>(func->args[2])->value);
                                if (isInt) k = static_cast<int>(k_val);
                            }
                            if (k >= 1 && k <= n) {
                                auto root = roots[k - 1];
                                if (std::abs(root.imag()) < 1e-12) return SymExpr(root.real());
                                return SymExpr(Complex(root.real(), root.imag()));
                            }
                        } else {
                            SymExpr exprInner = SymExpr(func->args[0]);
                            SymExpr sum(0.0);
                            for (int i = 0; i < n; ++i) {
                                SymExpr rootExpr;
                                if (std::abs(roots[i].imag()) < 1e-12) rootExpr = SymExpr(roots[i].real());
                                else rootExpr = SymExpr(Complex(roots[i].real(), roots[i].imag()));
                                
                                SymExpr subbed = subs(exprInner, dummy, rootExpr);
                                sum = sum + evalFloat(subbed);
                            }
                            return sum;
                        }
                    } catch (...) {}
                }
            }
            if (func->name == "sqrt" && func->args.size() == 1) {
                return evalFloat(SymExpr(func->args[0]) ^ SymExpr(Fraction(1, 2)));
            }
            if (func->name == "cbrt" && func->args.size() == 1) {
                return evalFloat(SymExpr(func->args[0]) ^ SymExpr(Fraction(1, 3)));
            }
            if (func->name == "root" && func->args.size() == 2) {
                return evalFloat(SymExpr(func->args[0]) ^ (SymExpr(BigInt(1)) / SymExpr(func->args[1])));
            }
            std::vector<SymNode*> newArgs;
            for (auto& arg : func->args)
                newArgs.push_back(evalFloat(SymExpr(arg)).ptr);
            return SymExpr::makeFunc(func->name, std::move(newArgs));
        }
        default:
            return expr;
        }
    }

    // ==========================================
    // evalValue: 保留完整类型（Complex 不丢失）
    // ==========================================
    SymExpr evalValue(const SymExpr& expr) {
        if (!expr.ptr) return expr;

        auto [ok, val] = tryEvalConst(expr);
        if (ok) {
            return val.asSymbolic();
        }

        switch (expr.ptr->getType()) {
        case SymType::NUM:
            return expr;  // 保留原始类型

        case SymType::VAR:
            return expr;

        case SymType::ADD: {
            auto add = static_cast<SymAdd*>(expr.ptr);
            SymExpr result(BigInt(0));
            for (auto& arg : add->args)
                result = result + evalValue(SymExpr(arg));
            return result;
        }

        case SymType::MUL: {
            auto mul = static_cast<SymMul*>(expr.ptr);
            SymExpr result(BigInt(1));
            for (auto& arg : mul->args)
                result = result * evalValue(SymExpr(arg));
            return result;
        }

        case SymType::POW: {
            auto pow = static_cast<SymPow*>(expr.ptr);
            SymExpr base = evalValue(SymExpr(pow->base));
            SymExpr expn = evalValue(SymExpr(pow->exp));
            return base ^ expn;
        }

        case SymType::FUNC: {
            auto func = static_cast<SymFunc*>(expr.ptr);
            if ((func->name == "RootOf" || func->name == "RootSum") && func->args.size() == 3) {
                bool isRootOf = (func->name == "RootOf");
                SymExpr P = isRootOf ? SymExpr(func->args[0]) : SymExpr(func->args[2]);
                std::string dummy = static_cast<SymVar*>(func->args[1])->name;
                auto coeffs = extractCoeffs(P, dummy);
                
                int deg = static_cast<int>(coeffs.size()) - 1;
                if (deg >= 1 && deg <= 4) {
                    auto exactRoots = getExactRoots(coeffs);
                    if (!exactRoots.empty()) {
                        if (isRootOf) {
                            int k = 1;
                            if (func->args[2]->getType() == SymType::NUM) {
                                auto [isInt, k_val] = extractExactInt(static_cast<SymNum*>(func->args[2])->value);
                                if (isInt) k = static_cast<int>(k_val);
                            }
                            if (k >= 1 && k <= deg) {
                                return evalValue(exactRoots[k - 1]);
                            }
                        } else {
                            SymExpr exprInner = SymExpr(func->args[0]);
                            SymExpr sum(BigInt(0));
                            for (int i = 0; i < deg; ++i) {
                                SymExpr subbed = subs(exprInner, dummy, exactRoots[i]);
                                sum = sum + evalValue(subbed);
                            }
                            return sum;
                        }
                    }
                }

                bool p_is_const = !coeffs.empty();
                std::vector<std::complex<double>> numCoeffs;
                if (p_is_const) {
                    for (const auto& c : coeffs) {
                        auto [c_ok, c_val] = tryEvalConst(c);
                        if (c_ok) {
                            if (c_val.isComplex()) numCoeffs.push_back({c_val.asComplex().real, c_val.asComplex().imag});
                            else numCoeffs.push_back({c_val.asFloat(), 0.0});
                        } else {
                            p_is_const = false;
                            break;
                        }
                    }
                }

                if (p_is_const) {
                    try {
                        auto roots = findRootsNumeric(numCoeffs);
                        int n = static_cast<int>(roots.size());
                        if (isRootOf) {
                            int k = 1;
                            if (func->args[2]->getType() == SymType::NUM) {
                                auto [isInt, k_val] = extractExactInt(static_cast<SymNum*>(func->args[2])->value);
                                if (isInt) k = static_cast<int>(k_val);
                            }
                            if (k >= 1 && k <= n) {
                                auto root = roots[k - 1];
                                if (std::abs(root.imag()) < 1e-12) return SymExpr(root.real());
                                return SymExpr(Complex(root.real(), root.imag()));
                            }
                        } else {
                            SymExpr exprInner = SymExpr(func->args[0]);
                            SymExpr sum(BigInt(0));
                            for (int i = 0; i < n; ++i) {
                                SymExpr rootExpr;
                                if (std::abs(roots[i].imag()) < 1e-12) rootExpr = SymExpr(roots[i].real());
                                else rootExpr = SymExpr(Complex(roots[i].real(), roots[i].imag()));
                                
                                SymExpr subbed = subs(exprInner, dummy, rootExpr);
                                sum = sum + evalValue(subbed);
                            }
                            return sum;
                        }
                    } catch (...) {}
                }
            }
            if (func->name == "sqrt" && func->args.size() == 1) {
                return evalValue(SymExpr(func->args[0]) ^ SymExpr(Fraction(1, 2)));
            }
            if (func->name == "cbrt" && func->args.size() == 1) {
                return evalValue(SymExpr(func->args[0]) ^ SymExpr(Fraction(1, 3)));
            }
            if (func->name == "root" && func->args.size() == 2) {
                return evalValue(SymExpr(func->args[0]) ^ (SymExpr(BigInt(1)) / SymExpr(func->args[1])));
            }
            std::vector<SymNode*> newArgs;
            for (auto& arg : func->args)
                newArgs.push_back(evalValue(SymExpr(arg)).ptr);
            return SymExpr::makeFunc(func->name, std::move(newArgs));
        }
        default:
            return expr;
        }
    }

    // =================================================================
// 微积分引擎 (Calculus Engine)
// =================================================================
    SymExpr diff(const SymExpr& expr, const std::string& var) {
        checkInterrupt();
        if (!expr.ptr) return expr;

        switch (expr.ptr->getType()) {
        case SymType::NUM:
            return SymExpr(BigInt(0)); // 常数导数为 0

        case SymType::VAR: {
            auto v = static_cast<SymVar*>(expr.ptr);
            // dx/dx = 1,  dy/dx = 0
            return (v->name == var) ? SymExpr(BigInt(1)) : SymExpr(BigInt(0));
        }

        case SymType::ADD: {
            auto add = static_cast<SymAdd*>(expr.ptr);
            SymExpr result(BigInt(0));
            // 和的导数：(u + v)' = u' + v'
            for (auto& arg : add->args) {
                result = result + diff(SymExpr(arg), var);
            }
            return result;
        }

        case SymType::MUL: {
            auto mul = static_cast<SymMul*>(expr.ptr);
            SymExpr result(BigInt(0));
            // 乘积法则扩展 (u v w)' = u'vw + uv'w + uvw'
            for (size_t i = 0; i < mul->args.size(); ++i) {
                SymExpr term(BigInt(1));
                for (size_t j = 0; j < mul->args.size(); ++j) {
                    if (i == j) term = term * diff(SymExpr(mul->args[j]), var);
                    else        term = term * SymExpr(mul->args[j]);
                }
                result = result + term;
            }
            return result;
        }

        case SymType::POW: {
            auto powNode = static_cast<SymPow*>(expr.ptr);
            SymExpr u = SymExpr(powNode->base);
            SymExpr v = SymExpr(powNode->exp);
            SymExpr du = diff(u, var);
            SymExpr dv = diff(v, var);

            // 特例 1：底数和指数都没有 var (即常数的常数次幂) -> 会在常数折叠被干掉，但以防万一
            if (du.isZero() && dv.isZero()) return SymExpr(BigInt(0));

            // 特例 2：指数为常数 (幂法则): (u^n)' = n * u^(n-1) * u'
            if (dv.isZero()) {
                return v * (u ^ (v - SymExpr(BigInt(1)))) * du;
            }

            // 一般情况 (广义指数法则): u^v = e^(v * log(u))
            // (u^v)' = u^v * (v' * log(u) + v * u' / u)
            SymExpr log_u(new SymFunc("log", std::vector<SymNode*>{u.ptr}));
            return (u ^ v) * (dv * log_u + v * du / u);
        }

        case SymType::FUNC: {
            auto func = static_cast<SymFunc*>(expr.ptr);
            if (func->args.empty()) return SymExpr(BigInt(0));

            std::string name = func->name;
            size_t arity = func->args.size();

            // =========================================================
            // 一元函数 f(u) 链式法则：f(u)' = f'(u) * u'
            // =========================================================
            if (arity == 1) {
                SymExpr u = SymExpr(func->args[0]);
                SymExpr du = diff(u, var);
                if (du.isZero()) return SymExpr(BigInt(0)); // 性能优化：内层导数为0，外层无须计算

                if (name == "sin") {
                    SymExpr cos_u(new SymFunc("cos", std::vector<SymNode*>{u.ptr}));
                    return cos_u * du;
                }
                if (name == "cos") {
                    SymExpr sin_u(new SymFunc("sin", std::vector<SymNode*>{u.ptr}));
                    return -sin_u * du;
                }
                if (name == "tan") {
                    SymExpr cos_u(new SymFunc("cos", std::vector<SymNode*>{u.ptr}));
                    return (SymExpr(BigInt(1)) / (cos_u * cos_u)) * du; // sec^2(u)
                }
                if (name == "cot") {
                    SymExpr sin_u(new SymFunc("sin", std::vector<SymNode*>{u.ptr}));
                    return -(SymExpr(BigInt(1)) / (sin_u * sin_u)) * du; // -csc^2(u)
                }
                if (name == "sec") {
                    SymExpr cos_u(new SymFunc("cos", std::vector<SymNode*>{u.ptr}));
                    SymExpr sin_u(new SymFunc("sin", std::vector<SymNode*>{u.ptr}));
                    return (sin_u / (cos_u * cos_u)) * du; // sec(u)tan(u)
                }
                if (name == "csc") {
                    SymExpr cos_u(new SymFunc("cos", std::vector<SymNode*>{u.ptr}));
                    SymExpr sin_u(new SymFunc("sin", std::vector<SymNode*>{u.ptr}));
                    return -(cos_u / (sin_u * sin_u)) * du; // -csc(u)cot(u)
                }
                if (name == "exp") {
                    return expr * du; // exp(u)' = exp(u) * u'
                }
                if (name == "log") {
                    return du / u; // log(u)' = u' / u
                }
                if (name == "sqrt") {
                    return du / (SymExpr(BigInt(2)) * expr); // sqrt(u)' = u' / (2*sqrt(u))
                }
                if (name == "abs") {
                    return (u / expr) * du; // |u|' = u/|u| * u' (当 u!=0)
                }
                if (name == "asin") {
                    return du / ((SymExpr(BigInt(1)) - u * u) ^ SymExpr(Fraction(1, 2))); // 1 / sqrt(1-u^2)
                }
                if (name == "acos") {
                    return -du / ((SymExpr(BigInt(1)) - u * u) ^ SymExpr(Fraction(1, 2))); // -1 / sqrt(1-u^2)
                }
                if (name == "atan") {
                    return du / (SymExpr(BigInt(1)) + u * u); // 1 / (1+u^2)
                }
                if (name == "asinh") {
                    return du / ((u * u + SymExpr(BigInt(1))) ^ SymExpr(Fraction(1, 2)));
                }
                if (name == "acosh") {
                    return du / ((u * u - SymExpr(BigInt(1))) ^ SymExpr(Fraction(1, 2)));
                }
                if (name == "atanh") {
                    return du / (SymExpr(BigInt(1)) - u * u);
                }
                if (name == "sinh") {
                    SymExpr cosh_u(new SymFunc("cosh", std::vector<SymNode*>{u.ptr}));
                    return cosh_u * du;
                }
                if (name == "cosh") {
                    SymExpr sinh_u(new SymFunc("sinh", std::vector<SymNode*>{u.ptr}));
                    return sinh_u * du;
                }
                if (name == "tanh") {
                    SymExpr cosh_u(new SymFunc("cosh", std::vector<SymNode*>{u.ptr}));
                    return (SymExpr(BigInt(1)) / (cosh_u * cosh_u)) * du;
                }
                if (name == "cbrt") {
                    return du / (SymExpr(BigInt(3)) * (expr ^ SymExpr(BigInt(2))));
                }
                if (name == "sgn" || name == "round" || name == "floor" || name == "ceil" || name == "trunc") {
                    return SymExpr(BigInt(0));
                }
                if (name == "deg") {
                    return (SymExpr(BigInt(180)) / SymExpr::makeConst(SymConstId::Pi)) * du;
                }
                if (name == "rad") {
                    return (SymExpr::makeConst(SymConstId::Pi) / SymExpr(BigInt(180))) * du;
                }
                if (name == "erf") {
                    SymExpr pi = SymExpr::makeConst(SymConstId::Pi);
                    SymExpr minus_u2 = -(u ^ SymExpr(BigInt(2)));
                    SymExpr exp_u(new SymFunc("exp", std::vector<SymNode*>{minus_u2.ptr}));
                    return (SymExpr(BigInt(2)) / (pi ^ SymExpr(Fraction(1, 2)))) * exp_u * du;
                }
                if (name == "fresnel_s") {
                    SymExpr pi = SymExpr::makeConst(SymConstId::Pi);
                    SymExpr arg = (pi / SymExpr(BigInt(2))) * (u ^ SymExpr(BigInt(2)));
                    SymExpr sin_u(new SymFunc("sin", std::vector<SymNode*>{arg.ptr}));
                    return sin_u * du;
                }
                if (name == "fresnel_c") {
                    SymExpr pi = SymExpr::makeConst(SymConstId::Pi);
                    SymExpr arg = (pi / SymExpr(BigInt(2))) * (u ^ SymExpr(BigInt(2)));
                    SymExpr cos_u(new SymFunc("cos", std::vector<SymNode*>{arg.ptr}));
                    return cos_u * du;
                }
                if (name == "Si") {
                    SymExpr sin_u(new SymFunc("sin", std::vector<SymNode*>{u.ptr}));
                    return (sin_u / u) * du;
                }
                if (name == "Ci") {
                    SymExpr cos_u(new SymFunc("cos", std::vector<SymNode*>{u.ptr}));
                    return (cos_u / u) * du;
                }
                if (name == "Ei") {
                    SymExpr exp_u(new SymFunc("exp", std::vector<SymNode*>{u.ptr}));
                    return (exp_u / u) * du;
                }
                if (name == "Li") {
                    SymExpr log_u(new SymFunc("log", std::vector<SymNode*>{u.ptr}));
                    return du / log_u;
                }
            }
            // =========================================================
            // 代数数节点求导 (RootOf / RootSum)
            // =========================================================
            else if (arity == 3 && (name == "RootOf" || name == "RootSum")) {
                if (func->args[1]->getType() == SymType::VAR) {
                    std::string dummy = static_cast<SymVar*>(func->args[1])->name;
                    if (name == "RootOf") {
                        SymExpr P(func->args[0]);
                        SymExpr dP_dv = diff(P, var);
                        if (dP_dv.isZero()) return SymExpr(BigInt(0));
                        SymExpr dP_dz = diff(P, dummy);
                        SymExpr res = -dP_dv / dP_dz;
                        return simplifyCore(subs(res, dummy, expr));
                    } else if (name == "RootSum") {
                        SymExpr E(func->args[0]);
                        SymExpr P(func->args[2]);
                        SymExpr dE_dv = diff(E, var);
                        SymExpr dE_da = diff(E, dummy);
                        SymExpr dP_dv = diff(P, var);
                        SymExpr dP_da = diff(P, dummy);
                        
                        SymExpr da_dv = -dP_dv / dP_da;
                        SymExpr inner_diff = simplifyCore(dE_dv + dE_da * da_dv);
                        
                        if (inner_diff.isZero()) return SymExpr(BigInt(0));
                        
                        return SymExpr::makeFunc("RootSum", std::vector<SymNode*>{
                            inner_diff.ptr, func->args[1], P.ptr
                        });
                    }
                }
            }
            // =========================================================
            // 二元函数 f(u, v) 偏导分配律：f_x = f_u * u_x + f_v * v_x
            // =========================================================
            else if (arity == 2) {
                SymExpr u = SymExpr(func->args[0]);
                SymExpr v = SymExpr(func->args[1]);
                SymExpr du = diff(u, var);
                SymExpr dv = diff(v, var);

                if (du.isZero() && dv.isZero()) return SymExpr(BigInt(0));

                if (name == "pow") {
                    // POW 作为函数：(u^v)' = u^v * (v'*log(u) + v*u'/u)
                    if (dv.isZero()) {
                        SymExpr power_down(new SymFunc("pow", std::vector<SymNode*>{u.ptr, (v - SymExpr(1)).ptr}));
                        return v * power_down * du;
                    }
                    SymExpr log_u(new SymFunc("log", std::vector<SymNode*>{u.ptr}));
                    return expr * (dv * log_u + v * du / u);
                }
                if (name == "root") {
                    // root(u, v) 实际上是 u^(1/v)。将其转换到底层幂节点然后递归求导即可！
                    SymExpr p = u ^ (SymExpr(BigInt(1)) / v);
                    return diff(p, var);
                }
                if (name == "log") {
                    // 指定底数的对数 log(u, v) = log(v) / log(u) (u 为底，v 为真数)
                    // 运用商的导数法则: (f/g)' = (f'g - fg')/g^2
                    SymExpr log_v(new SymFunc("log", std::vector<SymNode*>{v.ptr}));
                    SymExpr log_u(new SymFunc("log", std::vector<SymNode*>{u.ptr}));
                    SymExpr df = dv / v;
                    SymExpr dg = du / u;
                    return (df * log_u - log_v * dg) / (log_u * log_u);
                }
                if (name == "atan2") {
                    // atan2(y, x) => atan2(u, v)
                    // d/dx atan2(u, v) = (u'*v - u*v') / (u^2 + v^2)
                    return (du * v - u * dv) / (u * u + v * v);
                }
            }

            JC2_THROW(CalculusError, "Derivative of function '" + name + "' with " + std::to_string(arity) + " argument(s) is not implemented yet.");
        }
        default:
            return SymExpr(BigInt(0));
        }
    }

    // =================================================================
// 聚拢引擎 (Contraction Engine)
// log(x) + log(y) → log(x*y)
// c * log(x)       → log(x^c)
// exp(a) * exp(b)  → exp(a+b)
// =================================================================
    SymExpr contract(const SymExpr& expr) {
        checkInterrupt();
        if (!expr.ptr) return expr;

        static thread_local std::unordered_map<SymNode*, SymExpr> cache;
        static thread_local int depth = 0;

        SymNode* sig = expr.ptr;
        if (depth > 0) {
            auto it = cache.find(sig);
            if (it != cache.end()) return it->second;
        } else {
            cache.clear();
        }

        struct DepthGuard {
            int& d;
            DepthGuard(int& depth_ref) : d(depth_ref) { d++; }
            ~DepthGuard() { d--; }
        } guard(depth);

        auto compute = [&]() -> SymExpr {
            switch (expr.ptr->getType()) {

            // ─────────────────────────────────────────────
            // 加法节点：寻找所有 log 项，合并为一个 log
            // ─────────────────────────────────────────────
        case SymType::ADD: {
            auto add = static_cast<SymAdd*>(expr.ptr);
            SymExpr logInside(BigInt(1));
            SymExpr otherTerms(BigInt(0));
            int logCount = 0;

            for (auto& arg : add->args) {
                SymExpr term = contract(SymExpr(arg));
                SymExpr coeff(BigInt(1));
                SymFunc* logNode = nullptr;

                // 嗅探：纯 log(x)
                if (term.ptr->getType() == SymType::FUNC) {
                    auto fn = static_cast<SymFunc*>(term.ptr);
                    if (fn->name == "log" && fn->args.size() == 1) {
                        logNode = fn;
                    }
                }
                // 嗅探：c * log(x)（包括 -1 * log(x) 即减法）
                else if (term.ptr->getType() == SymType::MUL) {
                    auto mul = static_cast<SymMul*>(term.ptr);
                    SymExpr subCoeff(BigInt(1));
                    SymFunc* foundLog = nullptr;
                    for (auto& f : mul->args) {
                        if (!foundLog && f->getType() == SymType::FUNC) {
                            auto fn = static_cast<SymFunc*>(f);
                            if (fn->name == "log" && fn->args.size() == 1) {
                                foundLog = fn;
                                continue;
                            }
                        }
                        subCoeff = subCoeff * SymExpr(f);
                    }
                    if (foundLog) {
                        coeff = subCoeff;
                        logNode = foundLog;
                    }
                }

                if (logNode) {
                    logCount++;
                    SymExpr inside(logNode->args[0]);
                    // c * log(x) = log(x^c)，所以乘入 inside^coeff
                    logInside = logInside * (inside ^ coeff);
                }
                else {
                    otherTerms = otherTerms + term;
                }
            }

            if (logCount > 1) {
                SymExpr combinedLog(new SymFunc(
                    "log",
                    std::vector<SymNode*>{logInside.ptr}));
                return otherTerms + combinedLog;
            }

            // 没有足够的 log 可合并，重建加法节点
            SymExpr rebuilt(BigInt(0));
            for (auto& arg : add->args)
                rebuilt = rebuilt + contract(SymExpr(arg));
            return rebuilt;
        }

                         // ─────────────────────────────────────────────
                         // 乘法节点：寻找所有 exp 项，合并为一个 exp
                         // ─────────────────────────────────────────────
        case SymType::MUL: {
            auto mul = static_cast<SymMul*>(expr.ptr);
            SymExpr expArg(BigInt(0));
            int expCount = 0;
            
            std::map<SymNode*, std::vector<SymNode*>> powGroups;
            std::vector<SymNode*> otherFactorsList;

            for (auto& arg : mul->args) {
                SymExpr factor = contract(SymExpr(arg));
                SymFunc* expNode = nullptr;
                SymExpr coeff(BigInt(1));

                // 嗅探：纯 exp(x)
                if (factor.ptr->getType() == SymType::FUNC) {
                    auto fn = static_cast<SymFunc*>(factor.ptr);
                    if (fn->name == "exp" && fn->args.size() == 1)
                        expNode = fn;
                }
                // 嗅探：exp(x)^c（如 1/exp(x) = exp(x)^(-1)）
                else if (factor.ptr->getType() == SymType::POW) {
                    auto powN = static_cast<SymPow*>(factor.ptr);
                    if (powN->base->getType() == SymType::FUNC) {
                        auto fn = static_cast<SymFunc*>(powN->base);
                        if (fn->name == "exp" && fn->args.size() == 1) {
                            expNode = fn;
                            coeff = SymExpr(powN->exp);
                        }
                    }
                }

                if (expNode) {
                    expCount++;
                    expArg = expArg + (SymExpr(expNode->args[0]) * coeff);
                }
                else {
                    if (factor.ptr->getType() == SymType::POW) {
                        auto powN = static_cast<SymPow*>(factor.ptr);
                        powGroups[powN->exp].push_back(powN->base);
                    } else {
                        otherFactorsList.push_back(factor.ptr);
                    }
                }
            }

            SymExpr rebuilt(BigInt(1));
            if (expCount > 0) {
                SymExpr combinedExp(new SymFunc("exp", std::vector<SymNode*>{expArg.ptr}));
                rebuilt = combinedExp;
            }
            
            bool powCombined = false;
            for (auto& kv : powGroups) {
                if (kv.second.size() > 1) {
                    SymExpr combinedBase(BigInt(1));
                    for (auto& b : kv.second) combinedBase = combinedBase * SymExpr(b);
                    rebuilt = rebuilt * (combinedBase ^ SymExpr(kv.first));
                    powCombined = true;
                } else {
                    rebuilt = rebuilt * (SymExpr(kv.second[0]) ^ SymExpr(kv.first));
                }
            }
            
            for (auto& f : otherFactorsList) {
                rebuilt = rebuilt * SymExpr(f);
            }

            if (expCount > 1 || powCombined) {
                return rebuilt;
            }

            // 没有足够的项可合并，重建乘法节点
            SymExpr origRebuilt(BigInt(1));
            for (auto& arg : mul->args)
                origRebuilt = origRebuilt * contract(SymExpr(arg));
            return origRebuilt;
        }

        case SymType::POW: {
            auto p = static_cast<SymPow*>(expr.ptr);
            return contract(SymExpr(p->base)) ^ contract(SymExpr(p->exp));
        }

        case SymType::FUNC: {
            auto f = static_cast<SymFunc*>(expr.ptr);
            std::vector<SymNode*> nArgs;
            for (auto& a : f->args)
                nArgs.push_back(contract(SymExpr(a)).ptr);
            return SymExpr::makeFunc(f->name, std::move(nArgs));
        }

            default: break;
            }
            return expr;
        };

        SymExpr result = compute();
        cache[sig] = result;
        return result;
    }

    // =================================================================
// 检测 AST 中是否包含指定变量
// =================================================================
    static bool containsVarImpl(SymNode* node, const std::string& var, std::unordered_set<const SymNode*>& visited) {
        if (!node) return false;
        if (!visited.insert(node).second) return false;
        switch (node->getType()) {
        case SymType::NUM:  return false;
        case SymType::VAR:  return static_cast<SymVar*>(node)->name == var;
        case SymType::ADD:
            for (auto& a : static_cast<SymAdd*>(node)->args)
                if (containsVarImpl(a, var, visited)) return true;
            return false;
        case SymType::MUL:
            for (auto& a : static_cast<SymMul*>(node)->args)
                if (containsVarImpl(a, var, visited)) return true;
            return false;
        case SymType::POW:
            return containsVarImpl(static_cast<SymPow*>(node)->base, var, visited) ||
                containsVarImpl(static_cast<SymPow*>(node)->exp, var, visited);
        case SymType::FUNC: {
            auto func = static_cast<SymFunc*>(node);
            if ((func->name == "RootOf" || func->name == "RootSum") && func->args.size() == 3) {
                if (func->args[1]->getType() == SymType::VAR) {
                    std::string dummy = static_cast<SymVar*>(func->args[1])->name;
                    if (var == dummy) return false;
                }
                if (func->name == "RootOf") return containsVarImpl(func->args[0], var, visited);
                return containsVarImpl(func->args[0], var, visited) || containsVarImpl(func->args[2], var, visited);
            }
            for (auto& a : func->args)
                if (containsVarImpl(a, var, visited)) return true;
            return false;
        }
        }
        return false;
    }

    bool containsVar(SymNode* node, const std::string& var) {
        std::unordered_set<const SymNode*> visited;
        return containsVarImpl(node, var, visited);
    }

    // =================================================================
// 收集 AST 中出现的所有变量名
// =================================================================
    static void collectAllVarsImpl(SymNode* node, std::set<std::string>& vars, std::unordered_set<const SymNode*>& visited) {
        if (!node) return;
        if (!visited.insert(node).second) return;
        switch (node->getType()) {
        case SymType::NUM: break;
        case SymType::VAR:
            vars.insert(static_cast<SymVar*>(node)->name);
            break;
        case SymType::ADD:
            for (auto& a : static_cast<SymAdd*>(node)->args) collectAllVarsImpl(a, vars, visited);
            break;
        case SymType::MUL:
            for (auto& a : static_cast<SymMul*>(node)->args) collectAllVarsImpl(a, vars, visited);
            break;
        case SymType::POW:
            collectAllVarsImpl(static_cast<SymPow*>(node)->base, vars, visited);
            collectAllVarsImpl(static_cast<SymPow*>(node)->exp, vars, visited);
            break;
        case SymType::FUNC: {
            auto func = static_cast<SymFunc*>(node);
            if ((func->name == "RootOf" || func->name == "RootSum") && func->args.size() == 3) {
                std::set<std::string> subVars;
                collectAllVarsImpl(func->args[0], subVars, visited);
                if (func->name == "RootSum") collectAllVarsImpl(func->args[2], subVars, visited);
                if (func->args[1]->getType() == SymType::VAR) {
                    subVars.erase(static_cast<SymVar*>(func->args[1])->name);
                }
                vars.insert(subVars.begin(), subVars.end());
                break;
            }
            for (auto& a : func->args) collectAllVarsImpl(a, vars, visited);
            break;
        }
        }
    }

    void collectAllVars(SymNode* node, std::set<std::string>& vars) {
        std::unordered_set<const SymNode*> visited;
        collectAllVarsImpl(node, vars, visited);
    }

    // =================================================================
// 基础化简：身份吸收 + expand/contract 博弈
// ★ 不调用 factor，专门用于 factor 内部，防止循环递归
// =================================================================
    SymExpr simplifyCore(const SymExpr& expr) {
        checkInterrupt();
        if (!expr.ptr) return expr;

        static thread_local std::unordered_map<SymNode*, SymExpr> cache;
        static thread_local int depth = 0;

        SymNode* sig = expr.ptr;
        if (depth > 0) {
            auto it = cache.find(sig);
            if (it != cache.end()) return it->second;
        } else {
            cache.clear();
        }

        struct DepthGuard {
            int& d;
            DepthGuard(int& depth_ref) : d(depth_ref) { d++; }
            ~DepthGuard() { d--; }
        } guard(depth);

        auto compute = [&]() -> SymExpr {
            // 递归化简内层 + 身份吸收法则
            SymNode* newNode = expr.ptr;
        switch (expr.ptr->getType()) {
        case SymType::ADD: {
            SymExpr res(BigInt(0));
            for (auto& arg : static_cast<SymAdd*>(expr.ptr)->args)
                res = res + simplifyCore(SymExpr(arg));
            newNode = res.ptr;
            break;
        }
        case SymType::MUL: {
            SymExpr res(BigInt(1));
            for (auto& arg : static_cast<SymMul*>(expr.ptr)->args)
                res = res * simplifyCore(SymExpr(arg));
            newNode = res.ptr;
            break;
        }
        case SymType::POW: {
            auto pow = static_cast<SymPow*>(expr.ptr);
            SymExpr base = simplifyCore(SymExpr(pow->base));
            SymExpr exp = simplifyCore(SymExpr(pow->exp));
            
            // 代数数降幂 (Algebraic Number Power Reduction)
            if (base.ptr->getType() == SymType::FUNC && exp.ptr->getType() == SymType::NUM) {
                auto func = static_cast<SymFunc*>(base.ptr);
                if (func->name == "RootOf" && func->args.size() == 3) {
                    auto [isInt, n] = extractExactInt(static_cast<SymNum*>(exp.ptr)->value);
                    if (isInt && n > 0) {
                        if (func->args[1]->getType() == SymType::VAR) {
                            std::string dummy = static_cast<SymVar*>(func->args[1])->name;
                            SymExpr P(func->args[0]);
                            int deg = getDegree(P, dummy);
                            if (deg > 0 && n >= deg) {
                                SymExpr dummyVar = SymExpr::makeVar(dummy);
                                SymExpr targetPow = dummyVar ^ SymExpr(BigInt(n));
                                SymExpr rem = polyDiv(targetPow, P, dummy).second;
                                return simplifyCore(subs(rem, dummy, base));
                            }
                        }
                    }
                }
            }
            
            // 复分母有理化 (Complex Denominator Rationalization)
            if (exp.ptr->getType() == SymType::NUM) {
                auto [isInt, n] = extractExactInt(static_cast<SymNum*>(exp.ptr)->value);
                if (isInt && n < 0) {
                    if (isPolynomialIn(base, "i")) {
                        auto coeffs = extractCoeffs(base, "i");
                        if (coeffs.size() == 2 && !coeffs[1].isZero()) {
                            SymExpr A = coeffs[0];
                            SymExpr B = coeffs[1];
                            if (!containsVar(A.ptr, "i") && !containsVar(B.ptr, "i")) {
                                if (A.isZero()) {
                                    SymExpr inv = simplifyCore(-SymExpr::makeConst(SymConstId::I) * (B ^ SymExpr(BigInt(-1))));
                                    SymExpr res(BigInt(1));
                                    for (int64_t i = 0; i < -n; ++i) {
                                        res = simplifyCore(expand_core(res * inv, SymConfig::maxExpandTerms));
                                    }
                                    return res;
                                } else {
                                    SymExpr den = simplifyCore(A * A + B * B);
                                    if (!den.isZero()) {
                                        SymExpr conj = simplifyCore(A - SymExpr::makeConst(SymConstId::I) * B);
                                        SymExpr inv = simplifyCore(expand_core(conj * (den ^ SymExpr(BigInt(-1))), SymConfig::maxExpandTerms));
                                        SymExpr res(BigInt(1));
                                        for (int64_t i = 0; i < -n; ++i) {
                                            res = simplifyCore(expand_core(res * inv, SymConfig::maxExpandTerms));
                                        }
                                        return res;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            
            newNode = (base ^ exp).ptr;
            break;
        }
        case SymType::FUNC: {
            auto func = static_cast<SymFunc*>(expr.ptr);
            std::vector<SymNode*> nArgs;
            for (auto& arg : func->args)
                nArgs.push_back(simplifyCore(SymExpr(arg)).ptr);

            if (nArgs.size() == 1) {
                SymExpr inner(nArgs[0]);
                
                if (func->name == "log") {
                    if (inner.isOne()) return SymExpr(BigInt(0));
                    if (inner.ptr->getType() == SymType::CONST &&
                        static_cast<SymConst*>(inner.ptr)->id == SymConstId::E) {
                        return SymExpr(BigInt(1));
                    }
                    if (inner.ptr->getType() == SymType::FUNC) {
                        auto innerFn = static_cast<SymFunc*>(inner.ptr);
                        if (innerFn->name == "exp") return SymExpr(innerFn->args[0]);
                    }
                    if (inner.ptr->getType() == SymType::POW) {
                        auto powNode = static_cast<SymPow*>(inner.ptr);
                        if (powNode->base->getType() == SymType::CONST &&
                            static_cast<SymConst*>(powNode->base)->id == SymConstId::E) {
                            return SymExpr(powNode->exp);
                        }
                        SymExpr baseLog(new SymFunc("log", std::vector<SymNode*>{powNode->base}));
                        SymExpr simpBaseLog = simplifyCore(baseLog);
                        if (simpBaseLog.ptr != baseLog.ptr) {
                            return simplifyCore(SymExpr(powNode->exp) * simpBaseLog);
                        }
                    }
                    if (inner.ptr->getType() == SymType::MUL) {
                        auto mul = static_cast<SymMul*>(inner.ptr);
                        SymExpr res(BigInt(0));
                        bool simplified = false;
                        for (auto& arg : mul->args) {
                            SymExpr partLog(new SymFunc("log", std::vector<SymNode*>{arg}));
                            SymExpr simpPart = simplifyCore(partLog);
                            if (simpPart.ptr != partLog.ptr) simplified = true;
                            res = res + simpPart;
                        }
                        if (simplified) return res;
                    }
                }
                
                if (func->name == "exp") {
                    if (inner.isZero()) return SymExpr(BigInt(1));
                    if (inner.ptr->getType() == SymType::FUNC) {
                        auto innerFn = static_cast<SymFunc*>(inner.ptr);
                        if (innerFn->name == "log")
                            return SymExpr(innerFn->args[0]);
                    }
                    if (inner.ptr->getType() == SymType::MUL) {
                        auto mul = static_cast<SymMul*>(inner.ptr);
                        SymExpr coeff(BigInt(1));
                        SymNode* logArg = nullptr;
                        for (auto& arg : mul->args) {
                            if (!logArg && arg->getType() == SymType::FUNC) {
                                auto fn = static_cast<SymFunc*>(arg);
                                if (fn->name == "log" && fn->args.size() == 1) {
                                    logArg = fn->args[0];
                                    continue;
                                }
                            }
                            coeff = coeff * SymExpr(arg);
                        }
                        if (logArg) {
                            return SymExpr(logArg) ^ coeff;
                        }
                    }
                    if (inner.ptr->getType() == SymType::ADD) {
                        auto add = static_cast<SymAdd*>(inner.ptr);
                        SymExpr res(BigInt(1));
                        bool simplified = false;
                        for (auto& arg : add->args) {
                            SymExpr partExp(new SymFunc("exp", std::vector<SymNode*>{arg}));
                            SymExpr simpPart = simplifyCore(partExp);
                            if (simpPart.ptr != partExp.ptr) simplified = true;
                            res = res * simpPart;
                        }
                        if (simplified) return res;
                    }
                }
                
                auto isNegativeArg = [](const SymExpr& e) -> bool {
                    if (e.ptr->getType() == SymType::NUM) return isCasNegative(static_cast<SymNum*>(e.ptr)->value);
                    if (e.ptr->getType() == SymType::MUL) {
                        auto mul = static_cast<SymMul*>(e.ptr);
                        if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                            return isCasNegative(static_cast<SymNum*>(mul->args[0])->value);
                        }
                    }
                    return false;
                };

                if (func->name == "sin" || func->name == "cos" || func->name == "tan") {
                    if (inner.isZero()) {
                        if (func->name == "sin" || func->name == "tan") return SymExpr(BigInt(0));
                        if (func->name == "cos") return SymExpr(BigInt(1));
                    }
                    
                    if (isNegativeArg(inner)) {
                        SymExpr posInner = simplifyCore(-inner);
                        if (func->name == "sin" || func->name == "tan") {
                            return simplifyCore(-SymExpr::makeFunc(func->name, std::vector<SymNode*>{posInner.ptr}));
                        } else if (func->name == "cos") {
                            return simplifyCore(SymExpr::makeFunc(func->name, std::vector<SymNode*>{posInner.ptr}));
                        }
                    }
                    
                    auto isPiConst = [](SymNode* n) -> bool {
                        return n->getType() == SymType::CONST && static_cast<SymConst*>(n)->id == SymConstId::Pi;
                        };
                    auto getPiCoeff = [&](const SymExpr& e) -> std::pair<bool, Fraction> {
                        if (isPiConst(e.ptr)) {
                            return {true, Fraction(1)};
                        }
                        if (e.ptr->getType() == SymType::MUL) {
                            auto mul = static_cast<SymMul*>(e.ptr);
                            bool hasPi = false;
                            Fraction coeff(1);
                            bool valid = true;
                            for (auto& arg : mul->args) {
                                if (isPiConst(arg)) {
                                    hasPi = true;
                                } else if (arg->getType() == SymType::NUM) {
                                    auto num = static_cast<SymNum*>(arg);
                                    if (std::holds_alternative<int32_t>(num->value)) coeff = coeff * Fraction(BigInt(std::get<int32_t>(num->value)));
                                    else if (std::holds_alternative<BigInt>(num->value)) coeff = coeff * Fraction(std::get<BigInt>(num->value));
                                    else if (std::holds_alternative<Fraction>(num->value)) coeff = coeff * std::get<Fraction>(num->value);
                                    else valid = false;
                                } else {
                                    valid = false;
                                }
                            }
                            if (valid && hasPi) return {true, coeff};
                        }
                        return {false, Fraction(0)};
                    };
                    
                    auto [isPiMul, piCoeff] = getPiCoeff(inner);
                    if (isPiMul) {
                        Fraction two(2);
                        Fraction c = piCoeff;
                        while (c < Fraction(0)) c = c + two;
                        while (c >= two) c = c - two;
                        
                        if (func->name == "sin") {
                            if (c == Fraction(0) || c == Fraction(1)) return SymExpr(BigInt(0));
                            if (c == Fraction(BigInt(1), BigInt(2))) return SymExpr(BigInt(1));
                            if (c == Fraction(BigInt(3), BigInt(2))) return SymExpr(BigInt(-1));
                            if (c == Fraction(BigInt(1), BigInt(6)) || c == Fraction(BigInt(5), BigInt(6))) return SymExpr(Fraction(BigInt(1), BigInt(2)));
                            if (c == Fraction(BigInt(7), BigInt(6)) || c == Fraction(BigInt(11), BigInt(6))) return SymExpr(Fraction(BigInt(-1), BigInt(2)));
                            if (c == Fraction(BigInt(1), BigInt(4)) || c == Fraction(BigInt(3), BigInt(4))) return SymExpr(Fraction(BigInt(1), BigInt(2))) * (SymExpr(BigInt(2)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(5), BigInt(4)) || c == Fraction(BigInt(7), BigInt(4))) return SymExpr(Fraction(BigInt(-1), BigInt(2))) * (SymExpr(BigInt(2)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(1), BigInt(3)) || c == Fraction(BigInt(2), BigInt(3))) return SymExpr(Fraction(BigInt(1), BigInt(2))) * (SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(4), BigInt(3)) || c == Fraction(BigInt(5), BigInt(3))) return SymExpr(Fraction(BigInt(-1), BigInt(2))) * (SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                        } else if (func->name == "cos") {
                            if (c == Fraction(BigInt(1), BigInt(2)) || c == Fraction(BigInt(3), BigInt(2))) return SymExpr(BigInt(0));
                            if (c == Fraction(0)) return SymExpr(BigInt(1));
                            if (c == Fraction(1)) return SymExpr(BigInt(-1));
                            if (c == Fraction(BigInt(1), BigInt(3)) || c == Fraction(BigInt(5), BigInt(3))) return SymExpr(Fraction(BigInt(1), BigInt(2)));
                            if (c == Fraction(BigInt(2), BigInt(3)) || c == Fraction(BigInt(4), BigInt(3))) return SymExpr(Fraction(BigInt(-1), BigInt(2)));
                            if (c == Fraction(BigInt(1), BigInt(4)) || c == Fraction(BigInt(7), BigInt(4))) return SymExpr(Fraction(BigInt(1), BigInt(2))) * (SymExpr(BigInt(2)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(3), BigInt(4)) || c == Fraction(BigInt(5), BigInt(4))) return SymExpr(Fraction(BigInt(-1), BigInt(2))) * (SymExpr(BigInt(2)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(1), BigInt(6)) || c == Fraction(BigInt(11), BigInt(6))) return SymExpr(Fraction(BigInt(1), BigInt(2))) * (SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(5), BigInt(6)) || c == Fraction(BigInt(7), BigInt(6))) return SymExpr(Fraction(BigInt(-1), BigInt(2))) * (SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                        } else if (func->name == "tan") {
                            if (c == Fraction(0) || c == Fraction(1)) return SymExpr(BigInt(0));
                            if (c == Fraction(BigInt(1), BigInt(4)) || c == Fraction(BigInt(5), BigInt(4))) return SymExpr(BigInt(1));
                            if (c == Fraction(BigInt(3), BigInt(4)) || c == Fraction(BigInt(7), BigInt(4))) return SymExpr(BigInt(-1));
                            if (c == Fraction(BigInt(1), BigInt(6)) || c == Fraction(BigInt(7), BigInt(6))) return SymExpr(Fraction(BigInt(1), BigInt(3))) * (SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(5), BigInt(6)) || c == Fraction(BigInt(11), BigInt(6))) return SymExpr(Fraction(BigInt(-1), BigInt(3))) * (SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(1), BigInt(3)) || c == Fraction(BigInt(4), BigInt(3))) return SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2)));
                            if (c == Fraction(BigInt(2), BigInt(3)) || c == Fraction(BigInt(5), BigInt(3))) return -(SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                        } else if (func->name == "cot") {
                            if (c == Fraction(BigInt(1), BigInt(2)) || c == Fraction(BigInt(3), BigInt(2))) return SymExpr(BigInt(0));
                            if (c == Fraction(BigInt(1), BigInt(4)) || c == Fraction(BigInt(5), BigInt(4))) return SymExpr(BigInt(1));
                            if (c == Fraction(BigInt(3), BigInt(4)) || c == Fraction(BigInt(7), BigInt(4))) return SymExpr(BigInt(-1));
                            if (c == Fraction(BigInt(1), BigInt(6)) || c == Fraction(BigInt(7), BigInt(6))) return SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2)));
                            if (c == Fraction(BigInt(5), BigInt(6)) || c == Fraction(BigInt(11), BigInt(6))) return -(SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(1), BigInt(3)) || c == Fraction(BigInt(4), BigInt(3))) return SymExpr(Fraction(BigInt(1), BigInt(3))) * (SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(2), BigInt(3)) || c == Fraction(BigInt(5), BigInt(3))) return SymExpr(Fraction(BigInt(-1), BigInt(3))) * (SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                        } else if (func->name == "sec") {
                            if (c == Fraction(0)) return SymExpr(BigInt(1));
                            if (c == Fraction(1)) return SymExpr(BigInt(-1));
                            if (c == Fraction(BigInt(1), BigInt(3)) || c == Fraction(BigInt(5), BigInt(3))) return SymExpr(BigInt(2));
                            if (c == Fraction(BigInt(2), BigInt(3)) || c == Fraction(BigInt(4), BigInt(3))) return SymExpr(BigInt(-2));
                            if (c == Fraction(BigInt(1), BigInt(4)) || c == Fraction(BigInt(7), BigInt(4))) return SymExpr(BigInt(2)) ^ SymExpr(Fraction(BigInt(1), BigInt(2)));
                            if (c == Fraction(BigInt(3), BigInt(4)) || c == Fraction(BigInt(5), BigInt(4))) return -(SymExpr(BigInt(2)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(1), BigInt(6)) || c == Fraction(BigInt(11), BigInt(6))) return SymExpr(Fraction(BigInt(2), BigInt(3))) * (SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(5), BigInt(6)) || c == Fraction(BigInt(7), BigInt(6))) return SymExpr(Fraction(BigInt(-2), BigInt(3))) * (SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                        } else if (func->name == "csc") {
                            if (c == Fraction(BigInt(1), BigInt(2))) return SymExpr(BigInt(1));
                            if (c == Fraction(BigInt(3), BigInt(2))) return SymExpr(BigInt(-1));
                            if (c == Fraction(BigInt(1), BigInt(6)) || c == Fraction(BigInt(5), BigInt(6))) return SymExpr(BigInt(2));
                            if (c == Fraction(BigInt(7), BigInt(6)) || c == Fraction(BigInt(11), BigInt(6))) return SymExpr(BigInt(-2));
                            if (c == Fraction(BigInt(1), BigInt(4)) || c == Fraction(BigInt(3), BigInt(4))) return SymExpr(BigInt(2)) ^ SymExpr(Fraction(BigInt(1), BigInt(2)));
                            if (c == Fraction(BigInt(5), BigInt(4)) || c == Fraction(BigInt(7), BigInt(4))) return -(SymExpr(BigInt(2)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(1), BigInt(3)) || c == Fraction(BigInt(2), BigInt(3))) return SymExpr(Fraction(BigInt(2), BigInt(3))) * (SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                            if (c == Fraction(BigInt(4), BigInt(3)) || c == Fraction(BigInt(5), BigInt(3))) return SymExpr(Fraction(BigInt(-2), BigInt(3))) * (SymExpr(BigInt(3)) ^ SymExpr(Fraction(BigInt(1), BigInt(2))));
                        }
                    }
                }
                
                if (func->name == "sinh" || func->name == "cosh" || func->name == "tanh") {
                    if (inner.isZero()) {
                        if (func->name == "sinh" || func->name == "tanh") return SymExpr(BigInt(0));
                        if (func->name == "cosh") return SymExpr(BigInt(1));
                    }
                    
                    if (isNegativeArg(inner)) {
                        SymExpr posInner = simplifyCore(-inner);
                        if (func->name == "sinh" || func->name == "tanh") {
                            return simplifyCore(-SymExpr::makeFunc(func->name, std::vector<SymNode*>{posInner.ptr}));
                        } else if (func->name == "cosh") {
                            return simplifyCore(SymExpr::makeFunc(func->name, std::vector<SymNode*>{posInner.ptr}));
                        }
                    }
                }
                
                if (inner.isZero()) {
                    if (func->name == "erf" || func->name == "fresnel_s" || func->name == "fresnel_c" || func->name == "Si" || func->name == "Li") return SymExpr(BigInt(0));
                }
                if (func->name == "sqrt") {
                    return inner ^ SymExpr(Fraction(BigInt(1), BigInt(2)));
                }
                if (func->name == "cbrt") {
                    return inner ^ SymExpr(Fraction(BigInt(1), BigInt(3)));
                }
            }
            
            if (nArgs.size() == 3 && (func->name == "RootOf" || func->name == "RootSum")) {
                if (nArgs[1]->getType() == SymType::VAR) {
                    std::string dummy = static_cast<SymVar*>(nArgs[1])->name;
                    SymExpr P = (func->name == "RootOf") ? SymExpr(nArgs[0]) : SymExpr(nArgs[2]);
                    int deg = getDegree(P, dummy);
                    if (deg >= 1) {
                        auto coeffs = extractCoeffs(P, dummy);
                        if (deg == 1) {
                            SymExpr root = simplifyCore(-coeffs[0] / coeffs[1]);
                            if (func->name == "RootOf") return root;
                            if (func->name == "RootSum") return simplifyCore(subs(SymExpr(nArgs[0]), dummy, root));
                        } else if (func->name == "RootSum") {
                            SymExpr E(nArgs[0]);
                            // 1. 加法分配律: RootSum(A + B) -> RootSum(A) + RootSum(B)
                            if (E.ptr->getType() == SymType::ADD) {
                                SymExpr res(BigInt(0));
                                for (auto& arg : static_cast<SymAdd*>(E.ptr)->args) {
                                    res = res + SymExpr::makeFunc("RootSum", std::vector<SymNode*>{
                                        arg, nArgs[1], nArgs[2]
                                    });
                                }
                                return simplifyCore(res);
                            }
                            // 2. 常数提取: RootSum(c * A) -> c * RootSum(A)
                            if (E.ptr->getType() == SymType::MUL) {
                                SymExpr c(BigInt(1));
                                SymExpr rest(BigInt(1));
                                for (auto& arg : static_cast<SymMul*>(E.ptr)->args) {
                                    if (!containsVar(arg, dummy)) c = c * SymExpr(arg);
                                    else rest = rest * SymExpr(arg);
                                }
                                if (!c.isOne()) {
                                    SymExpr newSum(new SymFunc("RootSum", std::vector<SymNode*>{
                                        rest.ptr, nArgs[1], nArgs[2]
                                    }));
                                    return simplifyCore(c * newSum);
                                }
                            }
                            // 3. 韦达定理 (Vieta's formulas) 降维打击
                            if (!containsVar(E.ptr, dummy)) {
                                return simplifyCore(E * SymExpr(BigInt(deg)));
                            }
                            if (E.ptr->getType() == SymType::VAR && static_cast<SymVar*>(E.ptr)->name == dummy) {
                                return simplifyCore(-coeffs[deg - 1] / coeffs[deg]);
                            }
                            if (E.ptr->getType() == SymType::POW) {
                                auto powNode = static_cast<SymPow*>(E.ptr);
                                if (powNode->base->getType() == SymType::VAR && static_cast<SymVar*>(powNode->base)->name == dummy) {
                                    if (powNode->exp->getType() == SymType::NUM) {
                                        auto [isInt, p] = extractExactInt(static_cast<SymNum*>(powNode->exp)->value);
                                        if (isInt && p == 2 && deg >= 2) {
                                            SymExpr sum1 = -coeffs[deg - 1] / coeffs[deg];
                                            SymExpr sum2 = coeffs[deg - 2] / coeffs[deg];
                                            return simplifyCore(sum1 * sum1 - SymExpr(BigInt(2)) * sum2);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            
            newNode = SymExpr::makeFunc(func->name, std::move(nArgs)).ptr;
            break;
        }
        default: break;
        }

        SymExpr current(newNode);
        return current;
        };

        SymExpr result = compute();
        cache[sig] = result;
        return result;
    }


    // =================================================================
    // 轻量化多项式探针 (Polynomial Probe)
    // 机制说明：如果含有非积分变量 (如 y, z)，由于它们不等于 var，
    // 且 containsVar(..., var) 会返回 false，因此它们会被自然地
    // 视为常数 (0次多项式)，从而完美支持多元多项式的判定。
    // =================================================================
    bool isPolynomialIn(const SymExpr& expr, const std::string& var) {
        if (!expr.ptr) return false;
        switch (expr.ptr->getType()) {
            case SymType::NUM:
            case SymType::VAR:
            case SymType::CONST:   // ★ 常量（pi/e/i）是系数，不破坏多项式的性质
                return true; // 非 var 的其他变量 (如 y) 在这里返回 true，视为常数
            case SymType::ADD: {
                auto add = static_cast<SymAdd*>(expr.ptr);
                for (auto& arg : add->args) {
                    if (!isPolynomialIn(SymExpr(arg), var)) return false;
                }
                return true;
            }
            case SymType::MUL: {
                auto mul = static_cast<SymMul*>(expr.ptr);
                for (auto& arg : mul->args) {
                    if (!isPolynomialIn(SymExpr(arg), var)) return false;
                }
                return true;
            }
            case SymType::POW: {
                auto powNode = static_cast<SymPow*>(expr.ptr);
                if (containsVar(powNode->exp, var)) return false;
                if (containsVar(powNode->base, var)) {
                    if (powNode->exp->getType() != SymType::NUM) return false;
                    auto [isInt, n] = extractExactInt(static_cast<SymNum*>(powNode->exp)->value);
                    if (!isInt || n < 0) return false;
                }
                return isPolynomialIn(SymExpr(powNode->base), var);
            }
            case SymType::FUNC: {
                auto func = static_cast<SymFunc*>(expr.ptr);
                for (auto& arg : func->args) {
                    if (containsVar(arg, var)) return false;
                }
                return true;
            }
        }
        return false;
    }

    // =================================================================
    // 独立的多项式代数引擎 (SparsePoly)
    // =================================================================
    struct SparsePoly {
        std::map<int, SymExpr> coeffs;

        bool isZero() const {
            for (const auto& kv : coeffs) {
                if (!kv.second.isZero()) return false;
            }
            return true;
        }

        void clean() {
            for (auto it = coeffs.begin(); it != coeffs.end(); ) {
                if (it->second.isZero()) it = coeffs.erase(it);
                else ++it;
            }
        }

        SparsePoly operator+(const SparsePoly& other) const {
            SparsePoly res = *this;
            for (const auto& kv : other.coeffs) {
                res.coeffs[kv.first] = simplifyCore(res.coeffs[kv.first] + kv.second);
            }
            res.clean();
            return res;
        }

        SparsePoly operator*(const SparsePoly& other) const {
            SparsePoly res;
            for (const auto& kv1 : coeffs) {
                if (kv1.second.isZero()) continue;
                for (const auto& kv2 : other.coeffs) {
                    if (kv2.second.isZero()) continue;
                    int deg = kv1.first + kv2.first;
                    res.coeffs[deg] = simplifyCore(res.coeffs[deg] + kv1.second * kv2.second);
                }
            }
            res.clean();
            return res;
        }

        SparsePoly pow(int n) const {
            if (n == 0) {
                SparsePoly res;
                res.coeffs[0] = SymExpr(BigInt(1));
                return res;
            }
            if (n == 1) return *this;
            SparsePoly half = pow(n / 2);
            SparsePoly res = half * half;
            if (n % 2 != 0) res = res * (*this);
            return res;
        }
    };

    static std::optional<SparsePoly> toPolynomial(const SymExpr& expr, const std::string& var) {
        checkInterrupt();
        if (!expr.ptr) return std::nullopt;
        if (!containsVar(expr.ptr, var)) {
            SparsePoly p;
            p.coeffs[0] = expr;
            return p;
        }
        switch (expr.ptr->getType()) {
            case SymType::VAR: {
                if (static_cast<SymVar*>(expr.ptr)->name == var) {
                    SparsePoly p;
                    p.coeffs[1] = SymExpr(BigInt(1));
                    return p;
                }
                SparsePoly p;
                p.coeffs[0] = expr;
                return p;
            }
            case SymType::ADD: {
                SparsePoly res;
                for (auto& arg : static_cast<SymAdd*>(expr.ptr)->args) {
                    auto p = toPolynomial(SymExpr(arg), var);
                    if (!p) return std::nullopt;
                    res = res + *p;
                }
                return res;
            }
            case SymType::MUL: {
                SparsePoly res;
                res.coeffs[0] = SymExpr(BigInt(1));
                for (auto& arg : static_cast<SymMul*>(expr.ptr)->args) {
                    auto p = toPolynomial(SymExpr(arg), var);
                    if (!p) return std::nullopt;
                    res = res * *p;
                }
                return res;
            }
            case SymType::POW: {
                auto powNode = static_cast<SymPow*>(expr.ptr);
                if (containsVar(powNode->exp, var)) return std::nullopt;
                if (powNode->exp->getType() == SymType::NUM) {
                    auto [isInt, n] = extractExactInt(static_cast<SymNum*>(powNode->exp)->value);
                    if (isInt && n >= 0 && n <= 1000) {
                        auto baseP = toPolynomial(SymExpr(powNode->base), var);
                        if (!baseP) return std::nullopt;
                        return baseP->pow(static_cast<int>(n));
                    }
                }
                return std::nullopt;
            }
            default:
                return std::nullopt;
        }
    }

    // =================================================================
    // 动态多项式系数提取器：无限次 (消除 int64_t 转型警告修正版)
    // =================================================================
    std::vector<SymExpr> extractCoeffs(const SymExpr& expr, const std::string& var) {
        if (!isPolynomialIn(expr, var)) return {};
        auto polyOpt = toPolynomial(expr, var);
        if (!polyOpt) return {};
        
        int maxDeg = -1;
        for (const auto& kv : polyOpt->coeffs) {
            if (!kv.second.isZero()) {
                maxDeg = std::max(maxDeg, kv.first);
            }
        }
        if (maxDeg < 0) return {};
        
        std::vector<SymExpr> coeffs(static_cast<size_t>(maxDeg + 1), SymExpr(BigInt(0)));
        for (const auto& kv : polyOpt->coeffs) {
            coeffs[static_cast<size_t>(kv.first)] = kv.second;
        }
        return coeffs;
    }

    // =================================================================
    // 多项式代数底座 (Polynomial Algebra)
    // =================================================================
    static void trimCoeffs(std::vector<SymExpr>& a) {
        while (!a.empty() && a.back().isZero()) a.pop_back();
    }

    static SymExpr simplifyFrac(const SymExpr& expr) {
        auto [num, den] = getFraction(expr);
        if (den.isOne()) return simplifyCore(expand_core(num, SymConfig::maxExpandTerms));
        return simplifyCore(expand_core(num, SymConfig::maxExpandTerms)) / simplifyCore(expand_core(den, SymConfig::maxExpandTerms));
    }

    static std::pair<std::vector<SymExpr>, std::vector<SymExpr>> polyDivCoeffs(std::vector<SymExpr> A, const std::vector<SymExpr>& B) {
        trimCoeffs(A);
        if (B.empty()) JC2_THROW(MathError, "Division by zero polynomial.");
        int degA = static_cast<int>(A.size()) - 1;
        int degB = static_cast<int>(B.size()) - 1;
        
        if (degA < degB) return {{}, A};
        
        std::vector<SymExpr> Q(degA - degB + 1, SymExpr(BigInt(0)));
        SymExpr leadB = B.back();
        
        for (int i = degA - degB; i >= 0; --i) {
            checkInterrupt();
            if (A[i + degB].isZero()) continue;
            SymExpr q = simplifyFrac(A[i + degB] / leadB);
            Q[i] = q;
            for (int j = 0; j <= degB; ++j) {
                A[i + j] = simplifyFrac(A[i + j] - q * B[j]);
            }
        }
        trimCoeffs(Q);
        trimCoeffs(A);
        return {Q, A};
    }

    static std::vector<SymExpr> polyPseudoRemCoeffs(std::vector<SymExpr> A, const std::vector<SymExpr>& B) {
        trimCoeffs(A);
        if (B.empty()) JC2_THROW(MathError, "Division by zero polynomial.");
        int degA = static_cast<int>(A.size()) - 1;
        int degB = static_cast<int>(B.size()) - 1;
        
        if (degB == 0) return {}; // 任何多项式对常数的伪余数均为 0
        
        int d = degA - degB + 1;
        if (d <= 0) return A;
        
        SymExpr leadB = B.back();
        
        while (degA >= degB) {
            checkInterrupt();
            if (getAstNodeCount(A.back()) > SymConfig::maxAstNodes) {
                JC2_THROW(MathError, "polyPseudoRemCoeffs failed due to coefficient explosion.");
            }
            SymExpr leadA = A.back();
            
            for (int i = 0; i <= degA; ++i) {
                A[i] = simplifyFrac(A[i] * leadB);
            }
            
            int shift = degA - degB;
            for (int i = 0; i <= degB; ++i) {
                A[i + shift] = simplifyFrac(A[i + shift] - B[i] * leadA);
            }
            
            trimCoeffs(A);
            degA = static_cast<int>(A.size()) - 1;
            d--;
        }
        
        if (d > 0) {
            SymExpr multiplier = simplifyFrac(leadB ^ SymExpr(BigInt(d)));
            for (auto& c : A) c = simplifyFrac(c * multiplier);
        }
        
        return A;
    }

    int getDegree(const SymExpr& expr, const std::string& var) {
        auto coeffs = extractCoeffs(expr, var);
        if (coeffs.empty()) return -1;
        return static_cast<int>(coeffs.size()) - 1;
    }

    std::pair<SymExpr, SymExpr> polyDiv(const SymExpr& dividend, const SymExpr& divisor, const std::string& var) {
        auto coeffsA = extractCoeffs(dividend, var);
        auto coeffsB = extractCoeffs(divisor, var);
        
        if (coeffsB.empty()) JC2_THROW(MathError, "Divisor is not a polynomial in " + var);
        if (coeffsA.empty()) return {SymExpr(BigInt(0)), dividend};
        
        auto [coeffsQ, coeffsR] = polyDivCoeffs(coeffsA, coeffsB);
        
        auto toExpr = [&](const std::vector<SymExpr>& coeffs) {
            SymExpr res(BigInt(0));
            SymExpr X = SymExpr::makeVar(var);
            for (size_t i = 0; i < coeffs.size(); ++i) {
                if (!coeffs[i].isZero()) {
                    if (i == 0) res = res + coeffs[i];
                    else if (i == 1) res = res + coeffs[i] * X;
                    else res = res + coeffs[i] * (X ^ SymExpr(BigInt(i)));
                }
            }
            return res;
        };
        
        return {toExpr(coeffsQ), toExpr(coeffsR)};
    }

    static SymExpr exactDiv(const SymExpr& dividend, const SymExpr& divisor) {
        std::set<std::string> vars;
        collectAllVars(dividend.ptr, vars);
        collectAllVars(divisor.ptr, vars);
        if (vars.size() == 1) {
            return polyDiv(dividend, divisor, *vars.begin()).first;
        }
        return simplifyFrac(dividend / divisor);
    }

    // 专为 Bareiss 算法设计的纯多项式环精确除法器 (Fraction-Free)
    SymExpr bareissExactDiv(const SymExpr& dividend, const SymExpr& divisor) {
        if (divisor.isOne()) return dividend;
        if (divisor.isZero()) JC2_THROW(MathError, "Bareiss exact division by zero.");
        
        std::set<std::string> varsDivisor;
        collectAllVars(divisor.ptr, varsDivisor);
        
        if (varsDivisor.empty()) {
            return simplifyCore(expand_core(dividend / divisor, SymConfig::maxExpandTerms));
        }
        
        std::set<std::string> varsDividend;
        collectAllVars(dividend.ptr, varsDividend);
        
        std::string var = "";
        for (const auto& v : varsDivisor) {
            if (varsDividend.count(v)) {
                var = v;
                break;
            }
        }
        
        if (var.empty()) {
            return simplifyCore(expand_core(dividend / divisor, SymConfig::maxExpandTerms));
        }
        
        auto coeffsA = extractCoeffs(dividend, var);
        auto coeffsB = extractCoeffs(divisor, var);
        
        if (coeffsB.empty() || coeffsA.empty()) return simplifyCore(expand_core(dividend / divisor, SymConfig::maxExpandTerms));
        
        int degA = static_cast<int>(coeffsA.size()) - 1;
        int degB = static_cast<int>(coeffsB.size()) - 1;
        if (degA < degB) return SymExpr(BigInt(0));
        
        std::vector<SymExpr> Q(degA - degB + 1, SymExpr(BigInt(0)));
        SymExpr leadB = coeffsB.back();
        
        for (int i = degA - degB; i >= 0; --i) {
            checkInterrupt();
            if (coeffsA[i + degB].isZero()) continue;
            
            // Bareiss 保证这里的除法在环内是精确的，直接展开化简即可，绝不引入未化简的分数 AST
            SymExpr q = simplifyCore(expand_core(coeffsA[i + degB] / leadB, SymConfig::maxExpandTerms));
            Q[i] = q;
            for (int j = 0; j <= degB; ++j) {
                coeffsA[i + j] = simplifyCore(expand_core(coeffsA[i + j] - q * coeffsB[j], SymConfig::maxExpandTerms));
            }
        }
        
        SymExpr res(BigInt(0));
        SymExpr X = SymExpr::makeVar(var);
        for (size_t i = 0; i < Q.size(); ++i) {
            if (!Q[i].isZero()) {
                if (i == 0) res = res + Q[i];
                else if (i == 1) res = res + Q[i] * X;
                else res = res + Q[i] * (X ^ SymExpr(BigInt(i)));
            }
        }
        return res;
    }

    SymExpr polyPseudoRem(const SymExpr& dividend, const SymExpr& divisor, const std::string& var) {
        auto coeffsA = extractCoeffs(dividend, var);
        auto coeffsB = extractCoeffs(divisor, var);
        
        if (coeffsB.empty()) JC2_THROW(MathError, "Divisor is not a polynomial in " + var);
        if (coeffsA.empty()) return dividend;
        
        auto coeffsR = polyPseudoRemCoeffs(coeffsA, coeffsB);
        
        SymExpr res(BigInt(0));
        SymExpr X = SymExpr::makeVar(var);
        for (size_t i = 0; i < coeffsR.size(); ++i) {
            if (!coeffsR[i].isZero()) {
                if (i == 0) res = res + coeffsR[i];
                else if (i == 1) res = res + coeffsR[i] * X;
                else res = res + coeffsR[i] * (X ^ SymExpr(BigInt(i)));
            }
        }
        return res;
    }

    SymExpr polyGCD(const SymExpr& a, const SymExpr& b, const std::string& var) {
        if (a.isZero()) return b;
        if (b.isZero()) return a;

        auto coeffsA = extractCoeffs(a, var);
        auto coeffsB = extractCoeffs(b, var);
        
        if (coeffsA.empty() || coeffsB.empty()) return SymExpr(BigInt(1));
        
        if (coeffsA.size() < coeffsB.size()) {
            std::swap(coeffsA, coeffsB);
        }

        SymExpr g(BigInt(1));
        SymExpr h(BigInt(1));
        
        int iter = 0;
        while (!coeffsB.empty()) {
            checkInterrupt();
            if (++iter > SymConfig::maxIterations) {
                JC2_THROW(MathError, "polyGCD infinite loop detected.");
            }
            int degA = static_cast<int>(coeffsA.size()) - 1;
            int degB = static_cast<int>(coeffsB.size()) - 1;
            int delta = degA - degB;
            
            auto coeffsR = polyPseudoRemCoeffs(coeffsA, coeffsB);
            if (coeffsR.empty()) {
                coeffsA = coeffsB;
                break;
            }
            
            SymExpr leadB = coeffsB.back();
            
            SymExpr divisor = simplifyFrac(-(g * (h ^ SymExpr(BigInt(delta)))));
            if (delta % 2 == 0) {
                divisor = simplifyFrac(-divisor);
            }
            
            coeffsA = coeffsB;
            coeffsB = coeffsR;
            for (auto& c : coeffsB) c = simplifyFrac(c / divisor);
            trimCoeffs(coeffsB);
            
            g = leadB;
            if (delta > 0) {
                SymExpr h_pow = simplifyFrac(h ^ SymExpr(BigInt(delta - 1)));
                h = exactDiv(simplifyFrac(g ^ SymExpr(BigInt(delta))), h_pow);
            }
        }

        if (!coeffsA.empty()) {
            SymExpr lead = coeffsA.back();
            if (!lead.isZero() && !lead.isOne()) {
                if (lead.ptr->getType() == SymType::NUM) {
                    for (auto& c : coeffsA) c = simplifyFrac(c / lead);
                } else if (lead.ptr->getType() == SymType::MUL) {
                    auto mul = static_cast<SymMul*>(lead.ptr);
                    if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                        SymExpr numLead(mul->args[0]);
                        for (auto& c : coeffsA) c = simplifyFrac(c / numLead);
                    }
                }
            }
        }
        
        SymExpr res(BigInt(0));
        SymExpr X = SymExpr::makeVar(var);
        for (size_t i = 0; i < coeffsA.size(); ++i) {
            if (!coeffsA[i].isZero()) {
                if (i == 0) res = res + coeffsA[i];
                else if (i == 1) res = res + coeffsA[i] * X;
                else res = res + coeffsA[i] * (X ^ SymExpr(BigInt(i)));
            }
        }
        return res;
    }

    std::vector<std::pair<SymExpr, int>> polySquareFree(const SymExpr& p, const std::string& var) {
        std::vector<std::pair<SymExpr, int>> result;
        SymExpr P = simplifyFrac(p);
        if (P.isZero()) return result;
        int maxI = getDegree(P, var);
        if (maxI <= 0) {
            result.push_back({P, 1});
            return result;
        }

        auto coeffs = extractCoeffs(P, var);
        SymExpr lead = coeffs.back();
        SymExpr c(BigInt(1));
        if (lead.ptr->getType() == SymType::NUM) {
            c = lead;
        } else if (lead.ptr->getType() == SymType::MUL) {
            auto mul = static_cast<SymMul*>(lead.ptr);
            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                c = SymExpr(mul->args[0]);
            }
        }
        if (!c.isOne()) {
            P = simplifyFrac(P / c);
        }

        SymExpr dP = diff(P, var);
        SymExpr R = polyGCD(P, dP, var);
        if (SymConfig::debugIntegration) std::cout << "   [SQF] P: " << P.toString() << ", dP: " << dP.toString() << ", R(GCD): " << R.toString() << std::endl;
        // ★ gcd(f, f') 是不含 var 的非零常数 ⟺ f 平方自由。
        //   此时 Yun 迭代的输出必然是 V = f/R 本身、重数为 1：
        //   因为 d(f/R) = (f'·R − f·R')/R²，R' 对 var 求导为 0，故 W − V' = R'·V/R = 0，
        //   于是第一轮 gcd(V, 0) = V 就是它自己的平方自由部分。
        //   直接给出这一项，省掉每轮必做的 diff + simplifyFrac + polyGCD + 两次 polyDiv。
        //   cas.simplify 是自底向上对每个节点都调 factor 的，这里省下的是纯重复计算。
        if (!R.isZero() && getDegree(R, var) == 0) {
            SymExpr sf = simplifyFrac(polyDiv(P, R, var).first);
            if (!c.isOne()) sf = simplifyFrac(sf * c);
            result.push_back({sf, 1});
            return result;
        }
        SymExpr V = polyDiv(P, R, var).first;
        SymExpr W = polyDiv(dP, R, var).first;

        int i = 1;
        while (getDegree(V, var) > 0) {
            checkInterrupt();
            if (i > maxI + 2 || i > SymConfig::maxIterations) {
                JC2_THROW(MathError, "polySquareFree failed due to algebraic deadlock.");
            }
            SymExpr dV = diff(V, var);
            SymExpr W_minus_dV = simplifyFrac(W - dV);
            SymExpr Y = polyGCD(V, W_minus_dV, var);
            if (SymConfig::debugIntegration) std::cout << "   [SQF] i=" << i << ", V: " << V.toString() << ", W-dV: " << W_minus_dV.toString() << ", Y(GCD): " << Y.toString() << std::endl;
            
            if (getDegree(Y, var) > 0) {
                result.push_back({Y, i});
            }
            
            V = polyDiv(V, Y, var).first;
            W = polyDiv(W_minus_dV, Y, var).first;
            i++;
        }
        
        if (!c.isOne()) {
            if (!result.empty() && result[0].second == 1) {
                result[0].first = simplifyFrac(result[0].first * c);
            } else {
                result.push_back({c, 1});
            }
        }
        return result;
    }

    std::tuple<SymExpr, SymExpr, SymExpr> polyEGCD(const SymExpr& a, const SymExpr& b, const std::string& var) {
        SymExpr r0 = a, r1 = b;
        SymExpr s0(BigInt(1)), s1(BigInt(0));
        SymExpr t0(BigInt(0)), t1(BigInt(1));

        int iter = 0;
        while (!r1.isZero()) {
            checkInterrupt();
            if (++iter > SymConfig::maxIterations) {
                JC2_THROW(MathError, "polyEGCD infinite loop detected.");
            }
            auto [q, r] = polyDiv(r0, r1, var);
            r0 = r1; r1 = r;
            SymExpr s_temp = simplifyFrac(s0 - q * s1);
            s0 = s1; s1 = s_temp;
            SymExpr t_temp = simplifyFrac(t0 - q * t1);
            t0 = t1; t1 = t_temp;
            
            if (getAstNodeCount(s1) > SymConfig::maxAstNodes || getAstNodeCount(t1) > SymConfig::maxAstNodes) {
                JC2_THROW(MathError, "polyEGCD failed due to coefficient explosion.");
            }
        }

        auto coeffs = extractCoeffs(r0, var);
        if (!coeffs.empty()) {
            SymExpr lead = coeffs.back();
            if (!lead.isZero() && !lead.isOne()) {
                if (lead.ptr->getType() == SymType::NUM) {
                    r0 = simplifyFrac(r0 / lead);
                    s0 = simplifyFrac(s0 / lead);
                    t0 = simplifyFrac(t0 / lead);
                } else if (lead.ptr->getType() == SymType::MUL) {
                    auto mul = static_cast<SymMul*>(lead.ptr);
                    if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                        SymExpr numLead(mul->args[0]);
                        r0 = simplifyFrac(r0 / numLead);
                        s0 = simplifyFrac(s0 / numLead);
                        t0 = simplifyFrac(t0 / numLead);
                    }
                }
            }
        }
        
        // 最终清理，防止 Bezout 系数中残留未合并的代数数
        return { simplifyFrac(r0), simplifyFrac(s0), simplifyFrac(t0) };
    }

    SymExpr polyResultant(const SymExpr& a, const SymExpr& b, const std::string& var) {
        auto coeffsA = extractCoeffs(a, var);
        auto coeffsB = extractCoeffs(b, var);
        
        if (coeffsA.empty() || coeffsB.empty()) return SymExpr(BigInt(0));
        
        int degA = static_cast<int>(coeffsA.size()) - 1;
        int degB = static_cast<int>(coeffsB.size()) - 1;
        
        if (degA == 0 && degB == 0) return SymExpr(BigInt(1));
        if (degA == 0) return simplifyFrac(coeffsA[0] ^ SymExpr(BigInt(degB)));
        if (degB == 0) return simplifyFrac(coeffsB[0] ^ SymExpr(BigInt(degA)));

        int sign = 1;
        if (degA < degB) {
            std::swap(coeffsA, coeffsB);
            std::swap(degA, degB);
            if ((degA % 2 != 0) && (degB % 2 != 0)) sign = -1;
        }

        SymExpr g(BigInt(1));
        SymExpr h(BigInt(1));

        while (degB > 0) {
            checkInterrupt();
            int d = degA - degB;
            if ((degA % 2 != 0) && (degB % 2 != 0)) sign = -sign;

            auto coeffsR = polyPseudoRemCoeffs(coeffsA, coeffsB);
            if (coeffsR.empty()) return SymExpr(BigInt(0));

            int degR = static_cast<int>(coeffsR.size()) - 1;
            if (degR == 0) {
                SymExpr r = coeffsR[0];
                SymExpr h_pow = simplifyFrac(h ^ SymExpr(BigInt(degB - 1)));
                SymExpr g_pow = simplifyFrac(g ^ SymExpr(BigInt(degB - 1)));
                SymExpr num = simplifyFrac(r ^ SymExpr(BigInt(degB)));
                SymExpr den = simplifyFrac(g_pow * h_pow);
                SymExpr res = exactDiv(num, den);
                if (sign == -1) res = simplifyCore(-res);
                return res;
            }

            SymExpr divisor = simplifyFrac(g * (h ^ SymExpr(BigInt(d))));
            for (auto& c : coeffsR) c = simplifyFrac(c / divisor);
            trimCoeffs(coeffsR);

            coeffsA = coeffsB;
            coeffsB = coeffsR;
            degA = static_cast<int>(coeffsA.size()) - 1;
            degB = static_cast<int>(coeffsB.size()) - 1;

            g = coeffsA.back();
            if (d > 0) {
                SymExpr h_pow = simplifyFrac(h ^ SymExpr(BigInt(d - 1)));
                h = exactDiv(simplifyFrac(g ^ SymExpr(BigInt(d))), h_pow);
            }
        }

        return SymExpr(BigInt(0));
    }

    // =================================================================
// 尝试对表达式开精确平方根
// 仅处理 NUM, POW(偶数幂), MUL(逐因子开根) 三类结构
// 返回 {是否成功, 平方根表达式}
// =================================================================
    std::pair<bool, SymExpr> trySquareRoot(const SymExpr& expr, bool allowPartial) {
        if (!expr.ptr) return { false, expr };

        // 情况 1：纯常数
        if (expr.ptr->getType() == SymType::NUM) {
            auto numNode = static_cast<SymNum*>(expr.ptr);
            auto [isInt, n] = extractExactInt(numNode->value);
            if (isInt) {
                if (n < 0) return { false, expr }; // 负数无实平方根
                if (n == 0) return { true, SymExpr(BigInt(0)) };
                int64_t s = static_cast<int64_t>(std::round(std::sqrt(static_cast<double>(n))));
                // 精度保护：回代验证
                if (s * s == n) return { true, SymExpr(BigInt(s)) };
                
                if (allowPartial) {
                    int64_t outside = 1;
                    int64_t inside = n;
                    for (int64_t i = 2; i * i <= inside; ++i) {
                        while (inside % (i * i) == 0) {
                            outside *= i;
                            inside /= (i * i);
                        }
                    }
                    if (outside > 1) {
                        return { true, SymExpr(BigInt(outside)) * (SymExpr(BigInt(inside)) ^ SymExpr(Fraction(1, 2))) };
                    }
                }
            } else if (std::holds_alternative<Fraction>(numNode->value)) {
                Fraction f = std::get<Fraction>(numNode->value);
                if (f.getNum().isNegative()) return { false, expr };
                auto [numOk, numSqrt] = trySquareRoot(SymExpr(f.getNum()), allowPartial);
                auto [denOk, denSqrt] = trySquareRoot(SymExpr(f.getDen()), allowPartial);
                if (numOk && denOk) {
                    return { true, numSqrt / denSqrt };
                }
            }
            return { false, expr };
        }

        // 情况 2：var（单变量不能整数开根）
        if (expr.ptr->getType() == SymType::VAR) {
            return { false, expr };
        }

        // 情况 3：幂次 base^exp，若 exp 是偶数正整数，sqrt(base^exp) = base^(exp/2)
        if (expr.ptr->getType() == SymType::POW) {
            auto p = static_cast<SymPow*>(expr.ptr);
            if (p->exp->getType() == SymType::NUM) {
                auto [isInt, n] = extractExactInt(static_cast<SymNum*>(p->exp)->value);
                if (isInt && n > 0 && n % 2 == 0) {
                    return { true, SymExpr(p->base) ^ SymExpr(BigInt(n / 2)) };
                }
            }
            return { false, expr };
        }

        // 情况 4：乘积，逐因子开根（全部成功才算成功）
        if (expr.ptr->getType() == SymType::MUL) {
            auto mul = static_cast<SymMul*>(expr.ptr);
            SymExpr result(BigInt(1));
            for (auto& arg : mul->args) {
                auto [ok, sqrtArg] = trySquareRoot(SymExpr(arg), allowPartial);
                if (!ok) return { false, expr };
                result = result * sqrtArg;
            }
            return { true, result };
        }

        // 情况 5：加法或函数，结构太复杂，放弃
        return { false, expr };
    }

    // =================================================================
    // 提取有理分式的分子和分母 (Get Numerator and Denominator)
    // =================================================================
    std::pair<SymExpr, SymExpr> getFraction(const SymExpr& expr) {
        checkInterrupt();
        if (!expr.ptr) return {expr, SymExpr(BigInt(1))};
        switch (expr.ptr->getType()) {
            case SymType::NUM:
            case SymType::VAR:
            case SymType::CONST:   // 符号常量是"整块"的有理数意义上的原子，分母为 1
                return {expr, SymExpr(BigInt(1))};
            case SymType::ADD: {
                auto add = static_cast<SymAdd*>(expr.ptr);
                std::vector<std::pair<SymExpr, SymExpr>> nds;
                for (auto& arg : add->args) nds.push_back(getFraction(SymExpr(arg)));
                
                struct DenomData {
                    BigInt num_factor;
                    SymExpr sym_part;
                };
                std::vector<std::pair<SymExpr, DenomData>> parsed_nds;
                BigInt lcm_num(1);
                std::vector<SymExpr> unique_sym_dens;
                
                for (auto& nd : nds) {
                    SymExpr d = nd.second;
                    BigInt c(1);
                    SymExpr s(BigInt(1));
                    if (d.ptr->getType() == SymType::NUM) {
                        auto [isInt, val] = extractExactInt(static_cast<SymNum*>(d.ptr)->value);
                        if (isInt) c = BigInt(val);
                        else s = d;
                    } else if (d.ptr->getType() == SymType::MUL) {
                        auto mul = static_cast<SymMul*>(d.ptr);
                        std::vector<SymNode*> s_args;
                        for (auto& arg : mul->args) {
                            if (arg->getType() == SymType::NUM) {
                                auto [isInt, val] = extractExactInt(static_cast<SymNum*>(arg)->value);
                                if (isInt) c = c * BigInt(val);
                                else s_args.push_back(arg);
                            } else {
                                s_args.push_back(arg);
                            }
                        }
                        if (s_args.size() == 1) s = SymExpr(s_args[0]);
                        else if (s_args.size() > 1) s = SymExpr::makeMul(s_args);
                    } else {
                        s = d;
                    }
                    
                    if (c.isNegative()) {
                        c = -c;
                        nd.first = simplifyCore(-nd.first);
                    }
                    
                    if (c.isZero()) c = BigInt(1); // 防御性
                    lcm_num = BigInt::lcm(lcm_num, c);
                    
                    bool found = false;
                    for (auto& usd : unique_sym_dens) {
                        if (usd == s) { found = true; break; }
                    }
                    if (!found && !s.isOne()) unique_sym_dens.push_back(s);
                    
                    parsed_nds.push_back({nd.first, {c, s}});
                }
                
                SymExpr final_den = SymExpr(lcm_num);
                for (auto& usd : unique_sym_dens) final_den = simplifyCore(final_den * usd);
                
                SymExpr final_num(BigInt(0));
                for (auto& pnd : parsed_nds) {
                    SymExpr term_num = pnd.first;
                    BigInt missing_c = lcm_num / pnd.second.num_factor;
                    if (missing_c > BigInt(1)) term_num = simplifyCore(term_num * SymExpr(missing_c));
                    
                    for (auto& usd : unique_sym_dens) {
                        if (usd != pnd.second.sym_part) {
                            term_num = simplifyCore(expand_core(term_num * usd, SymConfig::maxExpandTerms));
                        }
                    }
                    final_num = simplifyCore(expand_core(final_num + term_num, SymConfig::maxExpandTerms));
                }
                
                return {final_num, final_den};
            }
            case SymType::MUL: {
                auto mul = static_cast<SymMul*>(expr.ptr);
                SymExpr num(BigInt(1));
                SymExpr den(BigInt(1));
                for (auto& arg : mul->args) {
                    auto nd = getFraction(SymExpr(arg));
                    num = simplifyCore(expand_core(num * nd.first, SymConfig::maxExpandTerms));
                    den = simplifyCore(den * nd.second);
                }
                return {num, den};
            }
            case SymType::POW: {
                auto powNode = static_cast<SymPow*>(expr.ptr);
                if (powNode->exp->getType() == SymType::NUM) {
                    auto [isInt, n] = extractExactInt(static_cast<SymNum*>(powNode->exp)->value);
                    if (isInt) {
                        auto nd = getFraction(SymExpr(powNode->base));
                        if (n >= 0) {
                            return {simplifyCore(expand_core(nd.first ^ SymExpr(BigInt(n)), SymConfig::maxExpandTerms)), simplifyCore(nd.second ^ SymExpr(BigInt(n)))};
                        } else {
                            return {simplifyCore(expand_core(nd.second ^ SymExpr(BigInt(-n)), SymConfig::maxExpandTerms)), simplifyCore(nd.first ^ SymExpr(BigInt(-n)))};
                        }
                    }
                }
                return {expr, SymExpr(BigInt(1))};
            }
            case SymType::FUNC:
                return {expr, SymExpr(BigInt(1))};
        }
        return {expr, SymExpr(BigInt(1))};
    }

    // =================================================================
    // 复杂多重无理式分母有理化 (Rationalize Denominator via Gröbner Basis)
    // =================================================================
    SymExpr rationalizeDenominator(const SymExpr& expr) {
        auto [A, D] = getFraction(expr);
        if (D.isOne()) return expr;

        std::map<SymNode*, std::string> exprToT;
        std::map<std::string, std::pair<SymExpr, SymExpr>> tToMinPoly;
        int t_counter = 0;

        std::function<SymExpr(const SymExpr&)> replaceRadicals = [&](const SymExpr& e) -> SymExpr {
            if (!e.ptr) return e;
            switch (e.ptr->getType()) {
                case SymType::ADD: {
                    SymExpr res(BigInt(0));
                    for (auto& arg : static_cast<SymAdd*>(e.ptr)->args) res = res + replaceRadicals(SymExpr(arg));
                    return res;
                }
                case SymType::MUL: {
                    SymExpr res(BigInt(1));
                    for (auto& arg : static_cast<SymMul*>(e.ptr)->args) res = res * replaceRadicals(SymExpr(arg));
                    return res;
                }
                case SymType::POW: {
                    auto powNode = static_cast<SymPow*>(e.ptr);
                    SymExpr base = replaceRadicals(SymExpr(powNode->base));
                    if (powNode->exp->getType() == SymType::NUM) {
                        auto numVal = static_cast<SymNum*>(powNode->exp)->value;
                        if (std::holds_alternative<Fraction>(numVal)) {
                            Fraction frac = std::get<Fraction>(numVal);
                            if (frac.getDen() > BigInt(1)) {
                                SymExpr newPow = base ^ SymExpr(frac);
                                SymNode* sig = newPow.ptr;
                                if (exprToT.count(sig)) return SymExpr::makeVar(exprToT[sig]);
                                // 使用 ~ 前缀确保在 Gröbner 基的字典序中优先级最高 (ASCII '~' > 'z' > 'x')
                                std::string t_name = "~t_rad_" + std::to_string(++t_counter);
                                exprToT[sig] = t_name;
                                SymExpr t_var = SymExpr::makeVar(t_name);
                                SymExpr minPoly = (t_var ^ SymExpr(frac.getDen())) - (base ^ SymExpr(frac.getNum()));
                                tToMinPoly[t_name] = {newPow, minPoly};
                                return t_var;
                            }
                        }
                    }
                    return base ^ replaceRadicals(SymExpr(powNode->exp));
                }
                case SymType::FUNC: {
                    auto f = static_cast<SymFunc*>(e.ptr);
                    if (f->name == "RootOf" && f->args.size() == 3) {
                        SymExpr poly = replaceRadicals(SymExpr(f->args[0]));
                        SymExpr dummy(f->args[1]);
                        SymNode* sig = e.ptr;
                        if (exprToT.count(sig)) return SymExpr::makeVar(exprToT[sig]);
                        std::string t_name = "~t_rad_" + std::to_string(++t_counter);
                        exprToT[sig] = t_name;
                        SymExpr t_var = SymExpr::makeVar(t_name);
                        SymExpr minPoly = subs(poly, static_cast<SymVar*>(dummy.ptr)->name, t_var);
                        tToMinPoly[t_name] = {e, minPoly};
                        return t_var;
                    }
                    std::vector<SymNode*> nArgs;
                    for (auto& arg : f->args) nArgs.push_back(replaceRadicals(SymExpr(arg)).ptr);
                    return SymExpr::makeFunc(f->name, std::move(nArgs));
                }
                default: return e;
            }
        };

        SymExpr D_prime = replaceRadicals(D);
        if (tToMinPoly.empty()) return expr;

        MultiPoly::clearRegistry();
        std::vector<MultiPoly> generators;
        SymExpr z_inv = SymExpr::makeVar("~z_inv");
        // 提前构造 MultiPoly 以确保 ~z_inv 获得最小的 ID (最高优先级)
        MultiPoly poly_z_inv(z_inv);
        for (const auto& kv : tToMinPoly) {
            MultiPoly poly_t(SymExpr::makeVar(kv.first));
        }

        generators.push_back(MultiPoly(simplifyCore(expand_core(z_inv * D_prime - SymExpr(BigInt(1)), SymConfig::maxExpandTerms))));

        for (const auto& kv : tToMinPoly) {
            generators.push_back(MultiPoly(simplifyCore(expand_core(kv.second.second, SymConfig::maxExpandTerms))));
        }

        std::vector<MultiPoly> rgb;
        try {
            rgb = computeGroebnerBasis(generators);
        } catch (const EngineInterruptError&) {
            throw;
        } catch (...) {
            return expr;
        }

        SymExpr invD(BigInt(0));
        bool found = false;

        for (const auto& poly : rgb) {
            if (poly.isZero()) continue;
            SymExpr polyExpr = poly.toSymExpr();
            if (containsVar(polyExpr.ptr, "~z_inv")) {
                auto coeffs = extractCoeffs(polyExpr, "~z_inv");
                if (coeffs.size() == 2) {
                    SymExpr c = coeffs[1];
                    bool c_has_t = false;
                    for (const auto& kv : tToMinPoly) {
                        if (containsVar(c.ptr, kv.first)) {
                            c_has_t = true;
                            break;
                        }
                    }
                    if (!c_has_t) {
                        SymExpr U = coeffs[0];
                        invD = simplifyCore(expand_core(-U / c, SymConfig::maxExpandTerms));
                        found = true;
                        break;
                    }
                }
            } else if (poly.terms.size() == 1 && poly.terms[0].mono.isOne()) {
                JC2_THROW(MathError, "Division by zero (denominator is algebraically zero).");
            }
        }

        if (!found) return expr;

        SymExpr result = simplifyCore(expand_core(A * invD, SymConfig::maxExpandTerms));
        for (auto it = tToMinPoly.rbegin(); it != tToMinPoly.rend(); ++it) {
            result = subs(result, it->first, it->second.first);
        }

        return simplifyCore(result);
    }

    // =================================================================
    // 有理分式化简 (Rational Fraction Simplification)
    // =================================================================
    // 加法项里是否含负指数幂。
    //   注意不能只查"加法节点的直接子节点是不是负幂"：x + 4*x^(-1) - 4 里的
    //   负幂是加法项 4*x^(-1)（一个 MUL）里的因子，直接子节点是 MUL 而不是 POW，
    //   只看一层会把这一整类漏掉（x + x^(-1) - 2 恰好是裸 POW，所以只测它会
    //   误以为判定正确）。这里顺着项本身的乘性因子与函数参数找。
    static bool termCarriesDenominator(SymNode* node) {
        if (!node) return false;
        switch (node->getType()) {
            case SymType::POW: {
                auto p = static_cast<SymPow*>(node);
                if (p->exp->getType() == SymType::NUM &&
                    isCasNegative(static_cast<SymNum*>(p->exp)->value)) return true;
                return termCarriesDenominator(p->base);
            }
            case SymType::MUL:
                for (SymNode* a : static_cast<SymMul*>(node)->args)
                    if (termCarriesDenominator(a)) return true;
                return false;
            case SymType::FUNC:
                for (SymNode* a : static_cast<SymFunc*>(node)->args)
                    if (termCarriesDenominator(a)) return true;
                return false;
            default: return false;   // 加法项不会嵌套加法（已展平）
        }
    }

    // =================================================================
    // 「分母被分配进和里」的形态探测。
    //   有理式的规范形态是单分式 —— 分母作为乘性因子整体待在外面：
    //       (x+1) * x^(-1)          ← 单分式
    //       x^(-1) + 1              ← 分母被逐项除进了和
    //   判据是「某个加法项里出现了负指数幂」。注意不能只看"表达式里有没有负
    //   指数"：x^2 * (x+1)^(-1) 里的 (x+1)^(-1) 是 MUL 的直接子节点，分母整体
    //   待在和外面，属于单分式，不能误判。
    // =================================================================
    static bool hasDistributedDenominator(SymNode* node, int depth) {
        if (!node || depth > 128) return false;
        switch (node->getType()) {
            case SymType::ADD:
                for (SymNode* a : static_cast<SymAdd*>(node)->args)
                    if (termCarriesDenominator(a) ||
                        hasDistributedDenominator(a, depth + 1)) return true;
                return false;
            case SymType::MUL:
                for (SymNode* a : static_cast<SymMul*>(node)->args)
                    if (hasDistributedDenominator(a, depth + 1)) return true;
                return false;
            case SymType::POW:
                return hasDistributedDenominator(static_cast<SymPow*>(node)->base, depth + 1);
            case SymType::FUNC:
                for (SymNode* a : static_cast<SymFunc*>(node)->args)
                    if (hasDistributedDenominator(a, depth + 1)) return true;
                return false;
            default: return false;
        }
    }

    // 候选筛选规则（在两处多重宇宙的 tryCandidate 里使用）：展开 / 约分 / 因式化
    // 这类候选，不允许把「分母整体待在和外面」的单分式改写成「分母逐项除进和里」
    // 的形态：
    //       (x+1) * x^(-1)                    ← 单分式
    //       x^(-1) + 1                        ← 分母被除进了和
    //       (x-1)^2 * x^(-1) * (x-2)^(-1)     ← 单分式
    //       (x + x^(-1) - 2) * (x-2)^(-1)     ← 分子被拆开
    //   两种写法数学值相同，而后者的节点更少，于是会赢下「按体积取最小」的多重宇宙
    //   —— simplify((x-1)^2/(x*(x-2))) 就是这么把已经分好的分子拆开的。
    //   规则只挡「由无到有」：输入本身就是若干倒数之和时（1/x + 1/(x+1)）current
    //   已经是分配形，直接放行 —— 既不干预多重宇宙原本要处理的情形，也不会把和
    //   强行并成一个大分式（那会让体积失控）。
    //   ★ 注意求值顺序：必须先算 after 再算 before。多数候选都不是分配形，
    //     after 一为假就短路返回，于是 before（即 current，通常是大表达式）的
    //     整棵遍历根本不会发生。反过来先把 current 的形态算好再逐个候选比对，
    //     会强制在每个节点都遍历一遍 current —— 实测那会让混合负载慢 12%
    //     （0.6% → 13.9%），是纯粹的反向优化。
    static bool introducesDistributedDenominator(const SymExpr& before, const SymExpr& after) {
        if (!hasDistributedDenominator(after.ptr, 0)) return false;
        return !hasDistributedDenominator(before.ptr, 0);
    }

    SymExpr simplifyRational(const SymExpr& expr) {
        if (!expr.ptr) return expr;
        
        SymExpr rationalized = rationalizeDenominator(expr);
        auto [num, den] = getFraction(rationalized);
        if (den.isOne()) {
            // 如果分母为 1，说明它本身就是多项式，但 getFraction 可能会展开它
            // 为了防止过度展开导致体积膨胀，我们比较一下体积
            if (getAstNodeCount(num) < getAstNodeCount(expr)) return num;
            return rationalized;
        }

        std::set<std::string> vars;
        collectAllVars(num.ptr, vars);
        collectAllVars(den.ptr, vars);
        
        if (!vars.empty()) {
            SymExpr numExp = simplifyCore(expand_core(num, SymConfig::maxExpandTerms));
            SymExpr denExp = simplifyCore(expand_core(den, SymConfig::maxExpandTerms));
            
            SymExpr currentNum = numExp;
            SymExpr currentDen = denExp;
            bool reduced = false;
            
            for (const std::string& var : vars) {
                if (getDegree(currentNum, var) >= 1 && getDegree(currentDen, var) >= 1) {
                    SymExpr g = polyGCD(currentNum, currentDen, var);
                    if (getDegree(g, var) >= 1) {
                        auto [qNum, rNum] = polyDiv(currentNum, g, var);
                        auto [qDen, rDen] = polyDiv(currentDen, g, var);
                        if (rNum.isZero() && rDen.isZero()) {
                            currentNum = qNum;
                            currentDen = qDen;
                            reduced = true;
                        }
                    }
                }
            }
            
            if (reduced) {
                SymExpr canceled = simplifyCore(currentNum / currentDen);
                if (canceled != rationalized) {
                    return canceled;
                }
            }
        }
        
        SymExpr factNum = factorReal(num);
        SymExpr factDen = factorReal(den);
        SymExpr canceled = simplifyCore(factNum / factDen);
        
        if (canceled != rationalized) {
            return canceled;
        }
        
        return rationalized;
    }

    // =================================================================
// 轻量级启发式化简：多重宇宙博弈（剥离 factor 和 rational）
// =================================================================
    SymExpr simplify(const SymExpr& expr) {
        checkInterrupt();
        if (!expr.ptr) return expr;

        static thread_local std::unordered_map<SymNode*, SymExpr> cache;
        static thread_local int depth = 0;

        SymNode* sig = expr.ptr;
        if (depth > 0) {
            auto it = cache.find(sig);
            if (it != cache.end()) return it->second;
        } else {
            cache.clear();
        }

        struct DepthGuard {
            int& d;
            DepthGuard(int& depth_ref) : d(depth_ref) { d++; }
            ~DepthGuard() { d--; }
        } guard(depth);

        auto compute = [&]() -> SymExpr {
            // 递归地对子节点调用 simplify (Bottom-up)
            SymExpr current = expr;
            switch (expr.ptr->getType()) {
                case SymType::ADD: {
                    SymExpr res(BigInt(0));
                    for (auto& arg : static_cast<SymAdd*>(expr.ptr)->args) res = res + simplify(SymExpr(arg));
                    current = res;
                    break;
                }
                case SymType::MUL: {
                    SymExpr res(BigInt(1));
                    for (auto& arg : static_cast<SymMul*>(expr.ptr)->args) res = res * simplify(SymExpr(arg));
                    current = res;
                    break;
                }
                case SymType::POW: {
                    auto p = static_cast<SymPow*>(expr.ptr);
                    current = simplify(SymExpr(p->base)) ^ simplify(SymExpr(p->exp));
                    break;
                }
                case SymType::FUNC: {
                    auto f = static_cast<SymFunc*>(expr.ptr);
                    std::vector<SymNode*> nArgs;
                    for (auto& arg : f->args) nArgs.push_back(simplify(SymExpr(arg)).ptr);
                    current = SymExpr::makeFunc(f->name, std::move(nArgs));
                    break;
                }
                default: break;
            }

            // 第一阶段：核心化简 + 身份吸收
            current = simplifyCore(current);
            
            // 强制进行一次三角化简，消除反三角嵌套等，将超越函数转化为代数式
            try { current = trigsimp(current); } catch (const EngineInterruptError&) { throw; } catch (...) {}

            // 第二阶段：多重宇宙博弈 (轻量级)
            SymExpr c_expand = current;
            SymExpr c_contract = current;
            SymExpr c_both = current;
            SymExpr c_rat_fast = current;
            SymExpr c_trig_expand = current;
            SymExpr c_trig_contract = current;

            try { c_expand = expand_core(current, 30); }
            catch (const EngineInterruptError&) { throw; }
            catch (const std::runtime_error&) {}

            try { c_contract = contract(current); }
            catch (const EngineInterruptError&) { throw; }
            catch (const std::runtime_error&) {}

            try {
                if (c_expand.ptr != current.ptr) {
                    c_both = contract(c_expand);
                    c_trig_expand = trigsimp(c_expand);
                }
            }
            catch (const EngineInterruptError&) { throw; }
            catch (const std::runtime_error&) {}
            
            try {
                if (c_contract.ptr != current.ptr) {
                    c_trig_contract = trigsimp(c_contract);
                }
            }
            catch (const EngineInterruptError&) { throw; }
            catch (const std::runtime_error&) {}

            try {
                auto [num, den] = getFraction(current);
                if (!den.isOne()) {
                    std::set<std::string> vars;
                    collectAllVars(num.ptr, vars);
                    collectAllVars(den.ptr, vars);
                    if (!vars.empty()) {
                        SymExpr numExp = simplifyCore(expand_core(num, 30));
                        SymExpr denExp = simplifyCore(expand_core(den, 30));
                        SymExpr currentNum = numExp;
                        SymExpr currentDen = denExp;
                        bool reduced = false;
                        
                        auto getIntContent = [](const SymExpr& e) -> BigInt {
                            if (e.ptr->getType() == SymType::NUM) {
                                auto [isInt, val] = extractExactInt(static_cast<SymNum*>(e.ptr)->value);
                                if (isInt) return BigInt(val).abs();
                                return BigInt(1);
                            }
                            if (e.ptr->getType() == SymType::ADD) {
                                BigInt gcd(0);
                                for (auto& arg : static_cast<SymAdd*>(e.ptr)->args) {
                                    BigInt c(1);
                                    if (arg->getType() == SymType::NUM) {
                                        auto [isInt, val] = extractExactInt(static_cast<SymNum*>(arg)->value);
                                        if (isInt) c = BigInt(val).abs();
                                    } else if (arg->getType() == SymType::MUL) {
                                        auto mul = static_cast<SymMul*>(arg);
                                        if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                            auto [isInt, val] = extractExactInt(static_cast<SymNum*>(mul->args[0])->value);
                                            if (isInt) c = BigInt(val).abs();
                                        }
                                    }
                                    if (gcd.isZero()) gcd = c;
                                    else gcd = BigInt::gcd(gcd, c);
                                    if (gcd == BigInt(1)) break;
                                }
                                return gcd.isZero() ? BigInt(1) : gcd;
                            }
                            if (e.ptr->getType() == SymType::MUL) {
                                auto mul = static_cast<SymMul*>(e.ptr);
                                if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                    auto [isInt, val] = extractExactInt(static_cast<SymNum*>(mul->args[0])->value);
                                    if (isInt) return BigInt(val).abs();
                                }
                            }
                            return BigInt(1);
                        };
                        
                        BigInt cNum = getIntContent(currentNum);
                        BigInt cDen = getIntContent(currentDen);
                        BigInt cGcd = BigInt::gcd(cNum, cDen);
                        if (cGcd > BigInt(1)) {
                            currentNum = simplifyCore(currentNum / SymExpr(cGcd));
                            currentDen = simplifyCore(currentDen / SymExpr(cGcd));
                            reduced = true;
                        }

                        for (const std::string& var : vars) {
                            if (getDegree(currentNum, var) >= 1 && getDegree(currentDen, var) >= 1) {
                                SymExpr g = polyGCD(currentNum, currentDen, var);
                                if (getDegree(g, var) >= 1) {
                                    auto [qNum, rNum] = polyDiv(currentNum, g, var);
                                    auto [qDen, rDen] = polyDiv(currentDen, g, var);
                                    if (rNum.isZero() && rDen.isZero()) {
                                        currentNum = qNum;
                                        currentDen = qDen;
                                        reduced = true;
                                    }
                                }
                            }
                        }
                        if (reduced) c_rat_fast = simplifyCore(currentNum / currentDen);
                    }
                }
            }
            catch (const EngineInterruptError&) { throw; }
            catch (const std::runtime_error&) {}

            // 选出体积最小的宇宙
            SymExpr best = current;
            int minSize = getAstComplexity(current);

            auto tryCandidate = [&](const SymExpr& cand) {
                if (introducesDistributedDenominator(current, cand)) return;
                int sz = getAstComplexity(cand);
                if (sz < minSize) {
                    minSize = sz;
                    best = cand;
                }
            };

            tryCandidate(c_expand);
            tryCandidate(c_contract);
            tryCandidate(c_both);
            tryCandidate(c_rat_fast);
            tryCandidate(c_trig_expand);
            tryCandidate(c_trig_contract);

            return best;
        };

        SymExpr result = compute();
        cache[sig] = result;
        return result;
    }

    // =================================================================
// 深度启发式化简：包含 factor 和 rational 的重型多重宇宙博弈
// =================================================================
    // 表达式里是否存在负指数（即分母）。用于决定 full_simplify 要不要跑第二轮：
    // 需要第二轮的是"有理分式"这一类 —— 重型候选 simplifyRational/factor 会造出
    // 一批新的子节点，只有再自底向上递归一次才能收敛；纯多项式/超越式第一轮就到不动点。
    static bool hasNegativePower(SymNode* node, int depth) {
        if (!node || depth > 64) return false;
        switch (node->getType()) {
            case SymType::POW: {
                auto p = static_cast<SymPow*>(node);
                if (p->exp->getType() == SymType::NUM &&
                    isCasNegative(static_cast<SymNum*>(p->exp)->value)) return true;
                return hasNegativePower(p->base, depth + 1) || hasNegativePower(p->exp, depth + 1);
            }
            case SymType::ADD:
                for (SymNode* a : static_cast<SymAdd*>(node)->args)
                    if (hasNegativePower(a, depth + 1)) return true;
                return false;
            case SymType::MUL:
                for (SymNode* a : static_cast<SymMul*>(node)->args)
                    if (hasNegativePower(a, depth + 1)) return true;
                return false;
            case SymType::FUNC:
                for (SymNode* a : static_cast<SymFunc*>(node)->args)
                    if (hasNegativePower(a, depth + 1)) return true;
                return false;
            default: return false;
        }
    }

    SymExpr full_simplify(const SymExpr& expr) {
        checkInterrupt();
        if (!expr.ptr) return expr;

        static thread_local std::unordered_map<SymNode*, SymExpr> cache;
        static thread_local int depth = 0;

        SymNode* sig = expr.ptr;
        if (depth > 0) {
            auto it = cache.find(sig);
            if (it != cache.end()) return it->second;
        } else {
            cache.clear();
            // ★ factor 记忆化与这个递归缓存同生命周期：只在一次顶层化简内复用，
            //   不跨调用累积。既吃到同一轮里的巨大重复率（实测 factor 调用数
            //   24800 → 5000、重负载化简快 9~17%），又不会像内部化池那样无界增长。
            clearFactorMemo();
        }

        struct DepthGuard {
            int& d;
            DepthGuard(int& depth_ref) : d(depth_ref) { d++; }
            ~DepthGuard() { d--; }
        } guard(depth);

        // 候选集档位。
        //   Full     —— 常规一轮：factor / simplifyRational / factor∘expand /
        //               factor∘simplifyRational 全试。
        //   Probe    —— 只做「递归子节点 + 轻量化简」，用于廉价判断是否已到不动点。
        //   FollowUp —— 收敛轮的裁剪候选集：只留 factor / simplifyRational /
        //               factor∘expand。实测 factor∘simplifyRational 在收敛轮里
        //               从不改变胜负，却要重跑一次最贵的 factor，去掉后第二轮
        //               的开销降到噪声水平（-0.3%）。
        enum class CandMode { Full, Probe, FollowUp };
        auto compute = [&](const SymExpr& input, CandMode mode) -> SymExpr {
            // 递归地对子节点调用 full_simplify (Bottom-up)
            SymExpr current = input;
            switch (input.ptr->getType()) {
                case SymType::ADD: {
                    SymExpr res(BigInt(0));
                    for (auto& arg : static_cast<SymAdd*>(input.ptr)->args)
                        res = res + full_simplify(SymExpr(arg));
                    current = res;
                    break;
                }
                case SymType::MUL: {
                    SymExpr res(BigInt(1));
                    for (auto& arg : static_cast<SymMul*>(input.ptr)->args) res = res * full_simplify(SymExpr(arg));
                    current = res;
                    break;
                }
                case SymType::POW: {
                    auto p = static_cast<SymPow*>(input.ptr);
                    current = full_simplify(SymExpr(p->base)) ^ full_simplify(SymExpr(p->exp));
                    break;
                }
                case SymType::FUNC: {
                    auto f = static_cast<SymFunc*>(input.ptr);
                    std::vector<SymNode*> nArgs;
                    for (auto& arg : f->args) nArgs.push_back(full_simplify(SymExpr(arg)).ptr);
                    current = SymExpr::makeFunc(f->name, std::move(nArgs));
                    break;
                }
                default: break;
            }

            // 第一阶段：先进行一次轻量级化简
            current = simplify(current);

            // 第二阶段：重型多重宇宙博弈
            SymExpr c_factor = current;
            SymExpr c_rational = current;
            SymExpr c_factor_expand = current;
            SymExpr c_rat_factor = current;

            if (mode == CandMode::Probe) return current;   // 只做递归 + 轻量化简

            // ★ simplifyRational 的前提是"这个表达式有分母"。
            //   对没有负指数的表达式（纯多项式、超越式）它进去只会被
            //   rationalizeDenominator / getFraction 展开一圈再原样退回：
            //   600 次顶层化简的采样里它自身耗时 73.6 ms，而 4310 次 Full 候选比较中
            //   胜出 0 次 —— 也就是说这部分开销是纯浪费。
            //   负指数是"存在分母"的充要条件：除法就是乘上负幂，开方给的是分数指数
            //   （不产生负指数）。所以用 hasNegativePower 当闸门。
            //   c_rat_factor（candidate 4）本来就以 c_rational != current 为前提，
            //   闸门一关它自动跳过，不必单独处理。
            //   验证：1990 例结构化语料（多项式 / 有理分式 / 幂 / 商 / 超越 / 多元）
            //   的 simplify 输出，闸门开与关逐字节一致。
            if (hasNegativePower(current.ptr, 0)) {
                try { c_rational = simplifyRational(current); }
                catch (const EngineInterruptError&) { throw; }
                catch (const std::runtime_error&) {}
            }

            try { c_factor = factor(current); }
            catch (const EngineInterruptError&) { throw; }
            catch (const std::runtime_error&) {}

            try {
                SymExpr c_expand = expand_core(current, 30);
                if (c_expand.ptr != current.ptr) {
                    c_factor_expand = factor(c_expand);
                }
            }
            catch (const EngineInterruptError&) { throw; }
            catch (const std::runtime_error&) {}

            if (mode == CandMode::Full) {
                try {
                    if (c_rational.ptr != current.ptr) {
                        c_rat_factor = factor(c_rational);
                    }
                }
                catch (const EngineInterruptError&) { throw; }
                catch (const std::runtime_error&) {}
            }

            // 选出体积最小的宇宙
            SymExpr best = current;
            int minSize = getAstComplexity(current);

            auto tryCandidate = [&](const SymExpr& cand) {
                if (introducesDistributedDenominator(current, cand)) return;
                int sz = getAstComplexity(cand);
                if (sz < minSize) {
                    minSize = sz;
                    best = cand;
                }
            };

            tryCandidate(c_factor);
            tryCandidate(c_rational);
            tryCandidate(c_factor_expand);
            tryCandidate(c_rat_factor);

            return best;
        };

        // ★ 迭代到不动点。
        //   compute() 会先自底向上递归化简子节点，再在结果上试
        //   factor / simplifyRational / expand。只跑一轮时，如果子节点的重写
        //   把一个"还能继续压缩"的形状交了上来，这一层就漏掉了：
        //     expand((x^2-y^2)/(x+y)^2) → -y^2*(x+y)^(-2) + x^2*(x+y)^(-2)
        //       → 第一轮停在 (x/y-1)*(x/y+1)^(-1)（cost 261）
        //       → 第二轮把子节点重写后才收敛到 (x-y)/(x+y)（cost 159）
        //   代价是几乎每次都要跑第二轮（第二轮一旦稳定就停），单项开销见提交说明；
        //   换来的是 simplify 真正幂等、且与路径无关 —— 下游算法可以靠树形判等去重。
        SymExpr result = compute(expr, CandMode::Full);
        if (depth == 1 && hasNegativePower(expr.ptr, 0)) {
            for (int round = 0; round < 3; ++round) {
                if (compute(result, CandMode::Probe).ptr == result.ptr) break;
                SymExpr next = compute(result, CandMode::FollowUp);
                if (next.ptr == result.ptr) break;
                result = next;
            }
        }
        cache[sig] = result;
        return result;
    }

    // =================================================================
    // 提取四次及以下多项式的精确根式解
    // =================================================================
    std::vector<SymExpr> getExactRoots(const std::vector<SymExpr>& coeffs) {
        int deg = static_cast<int>(coeffs.size()) - 1;
        std::vector<SymExpr> roots;
        if (deg == 1) {
            roots.push_back(simplifyCore(-coeffs[0] / coeffs[1]));
        } else if (deg == 2) {
            SymExpr a = coeffs[2], b = coeffs[1], c = coeffs[0];
            SymExpr delta = simplifyCore(b * b - SymExpr(BigInt(4)) * a * c);
            SymExpr sqrt_delta = delta ^ SymExpr(Fraction(1, 2));
            SymExpr twoA = SymExpr(BigInt(2)) * a;
            roots.push_back(simplifyCore((-b + sqrt_delta) / twoA));
            roots.push_back(simplifyCore((-b - sqrt_delta) / twoA));
        } else if (deg == 3) {
            SymExpr a = coeffs[3], b = coeffs[2], c = coeffs[1], d = coeffs[0];
            SymExpr p = simplifyCore((SymExpr(3) * a * c - b * b) / (SymExpr(3) * a * a));
            SymExpr q = simplifyCore((SymExpr(2) * b * b * b - SymExpr(9) * a * b * c + SymExpr(27) * a * a * d) / (SymExpr(27) * a * a * a));
            SymExpr delta = simplifyCore(((q / SymExpr(2)) ^ SymExpr(2)) + ((p / SymExpr(3)) ^ SymExpr(3)));
            SymExpr sqrt_delta = delta ^ SymExpr(Fraction(1, 2));
            
            SymExpr u, v_val;
            if (p.isZero()) {
                u = (-q) ^ SymExpr(Fraction(1, 3));
                v_val = SymExpr(0);
            } else {
                u = (-q / SymExpr(2) + sqrt_delta) ^ SymExpr(Fraction(1, 3));
                v_val = -p / (SymExpr(3) * u);
            }
            
            SymExpr I = SymExpr::makeConst(SymConstId::I);
            SymExpr sqrt3 = SymExpr(3) ^ SymExpr(Fraction(1, 2));
            SymExpr omega = (SymExpr(-1) + I * sqrt3) / SymExpr(2);
            SymExpr omega2 = (SymExpr(-1) - I * sqrt3) / SymExpr(2);
            
            SymExpr shift = b / (SymExpr(3) * a);
            roots.push_back(simplifyCore(u + v_val - shift));
            roots.push_back(simplifyCore(omega * u + omega2 * v_val - shift));
            roots.push_back(simplifyCore(omega2 * u + omega * v_val - shift));
        } else if (deg == 4) {
            SymExpr a = coeffs[4], b = coeffs[3], c = coeffs[2], d = coeffs[1], e = coeffs[0];
            SymExpr p = simplifyCore((SymExpr(8) * a * c - SymExpr(3) * b * b) / (SymExpr(8) * a * a));
            SymExpr q = simplifyCore((SymExpr(8) * a * a * d - SymExpr(4) * a * b * c + b * b * b) / (SymExpr(8) * a * a * a));
            SymExpr r_val = simplifyCore((SymExpr(256) * a * a * a * e - SymExpr(64) * a * a * b * d + SymExpr(16) * a * b * b * c - SymExpr(3) * b * b * b * b) / (SymExpr(256) * a * a * a * a));
            
            SymExpr shift = b / (SymExpr(4) * a);
            if (q.isZero()) {
                SymExpr sqrt_delta_bq = (p * p - SymExpr(4) * r_val) ^ SymExpr(Fraction(1, 2));
                SymExpr y1 = ((-p + sqrt_delta_bq) / SymExpr(2)) ^ SymExpr(Fraction(1, 2));
                SymExpr y2 = -y1;
                SymExpr y3 = ((-p - sqrt_delta_bq) / SymExpr(2)) ^ SymExpr(Fraction(1, 2));
                SymExpr y4 = -y3;
                roots.push_back(simplifyCore(y1 - shift));
                roots.push_back(simplifyCore(y2 - shift));
                roots.push_back(simplifyCore(y3 - shift));
                roots.push_back(simplifyCore(y4 - shift));
            } else {
                SymExpr A3 = SymExpr(1);
                SymExpr B3 = p;
                SymExpr C3 = p * p / SymExpr(4) - r_val;
                SymExpr D3 = -q * q / SymExpr(8);
                
                SymExpr p3 = simplifyCore((SymExpr(3) * A3 * C3 - B3 * B3) / (SymExpr(3) * A3 * A3));
                SymExpr q3 = simplifyCore((SymExpr(2) * B3 * B3 * B3 - SymExpr(9) * A3 * B3 * C3 + SymExpr(27) * A3 * A3 * D3) / (SymExpr(27) * A3 * A3 * A3));
                SymExpr delta3 = simplifyCore(((q3 / SymExpr(2)) ^ SymExpr(2)) + ((p3 / SymExpr(3)) ^ SymExpr(3)));
                SymExpr sqrt_delta3 = delta3 ^ SymExpr(Fraction(1, 2));
                
                SymExpr u3, v3;
                if (p3.isZero()) {
                    u3 = (-q3) ^ SymExpr(Fraction(1, 3));
                    v3 = SymExpr(0);
                } else {
                    u3 = (-q3 / SymExpr(2) + sqrt_delta3) ^ SymExpr(Fraction(1, 3));
                    v3 = -p3 / (SymExpr(3) * u3);
                }
                SymExpr m = simplifyCore(u3 + v3 - B3 / (SymExpr(3) * A3));
                
                SymExpr sqrt_2m = (SymExpr(2) * m) ^ SymExpr(Fraction(1, 2));
                SymExpr term1 = -(SymExpr(2) * p + SymExpr(2) * m + SymExpr(2) * q / sqrt_2m);
                SymExpr term2 = -(SymExpr(2) * p + SymExpr(2) * m - SymExpr(2) * q / sqrt_2m);
                
                SymExpr sqrt_term1 = term1 ^ SymExpr(Fraction(1, 2));
                SymExpr sqrt_term2 = term2 ^ SymExpr(Fraction(1, 2));
                
                roots.push_back(simplifyCore((sqrt_2m + sqrt_term1) / SymExpr(2) - shift));
                roots.push_back(simplifyCore((sqrt_2m - sqrt_term1) / SymExpr(2) - shift));
                roots.push_back(simplifyCore((-sqrt_2m + sqrt_term2) / SymExpr(2) - shift));
                roots.push_back(simplifyCore((-sqrt_2m - sqrt_term2) / SymExpr(2) - shift));
            }
        }
        return roots;
    }

    // =================================================================
    // 🚀 符号方程求解 (Symbolic Equation Solver)
    // 求解 expr == 0 关于 var 的根
    // =================================================================
    // 浅层数值因子分配：MUL 里若同时有"纯数值因子"和"ADD 因子"，把数值乘进去。
    //   二次公式给出的是 (-b ± √Δ)/(2A)，除以 2A 只会得到 1/2 * (2*i - 2) 这种
    //   没约分的形式，读起来完全不像 -1 + i。
    //   为什么不用 expand / full_simplify：两者都会递归进项的内部，在 Cardano
    //   嵌套根式上失控（实测 full_simplify 让 solve(x^3-3x+1) 从 0.9s 涨到 88s）。
    //   这里只做一层、且项数决定代价，天然有界。
    static SymExpr distributeNumericFactor(const SymExpr& e) {
        SymExpr cur = e;
        for (int round = 0; round < 4; ++round) {
            if (!cur.ptr || cur.ptr->getType() != SymType::MUL) break;
            auto mul = static_cast<SymMul*>(cur.ptr);
            SymNode* numNode = nullptr;
            SymNode* addNode = nullptr;
            int numCount = 0, addCount = 0;
            for (SymNode* a : mul->args) {
                if (a->getType() == SymType::NUM) { numNode = a; ++numCount; }
                else if (a->getType() == SymType::ADD) { addNode = a; ++addCount; }
            }
            if (numCount != 1 || addCount != 1) break;

            SymExpr rest(BigInt(1));
            for (SymNode* a : mul->args) {
                if (a != numNode && a != addNode) rest = rest * SymExpr(a);
            }
            SymExpr sum(BigInt(0));
            for (SymNode* term : static_cast<SymAdd*>(addNode)->args) {
                sum = sum + (SymExpr(numNode) * SymExpr(term) * rest);
            }
            if (sum.ptr == cur.ptr) break;
            cur = sum;
        }
        return cur;
    }

    // 前置声明：根排序要用它取数值键（定义在文件后段）
    static std::complex<double> fastEvalComplex(SymNode* node, const std::map<std::string, double>& env, const SymbolicFuncResolver& resolver);

    // ★ 根的规范化与排序（solveEq 出口）
    //   1) 化简：二次公式给出的是 (-b ± √Δ) / (2A)，只除不约，于是 x^4+4 的根
    //      会留下 1/2 * (2*i - 2) 这种形式（值对，但读不出是 -1 + i）。
    //      见 distributeNumericFactor。
    //   2) 排序：根的顺序原本取决于因式分解的遍历顺序，既任意又可能随实现漂移。
    //      "多项式的根集合"本身有确定的大小关系：按 (实部, 虚部) 升序，
    //      实根在前、共轭对相邻，与 Mathematica Root[] 以及 RootOf 自身的编号
    //      （findRootsNumeric 也是这个序）一致。
    //      无法数值化的根退化为按打印形式排序，保证结果仍然确定。
    static void normalizeAndSortRoots(std::vector<SymExpr>& roots) {
        // ① 化简 + 去重。
        //   ★ 化简只做"浅层数值因子分配"这一步，不能上 full_simplify / expand：
        //     它们会递归进项的内部控制不了代价，在 Cardano/Ferrari 的嵌套根式上
        //     爆炸（实测 full_simplify 让 solve(x^3-3x+1) 从 0.9s 涨到 88s，
        //     连 expand 都要 1.4s，且让积分慢 1.5 倍）。真正需要收拾的恰恰是
        //     二次公式留下的未约分形式（1/2 * (2*i - 2)），浅层分配就够。
        //   ★ 去重只比指针：所有节点都经过内部化池，规范形相同 ⟹ 同一个指针。
        //     用 SymExpr::operator== 会在"不相等"时回退到代数等价判定（expand +
        //     simplify + 规范化文本），而调用方已经去过重了，这里再来一遍纯属重复劳动。
        constexpr int64_t kMaxNodesToDistribute = 32;
        std::vector<SymExpr> stage;
        stage.reserve(roots.size());
        for (auto& r : roots) {
            SymExpr s = (getAstNodeCount(r) <= kMaxNodesToDistribute) ? distributeNumericFactor(r) : r;
            bool dup = false;
            for (auto& u : stage) {
                if (s.ptr == u.ptr) { dup = true; break; }
            }
            if (!dup) stage.push_back(s);
        }

        // ② 数值键：(实部, 虚部, 打印形式)。
        //   ★ 必须先量化再比较：共轭根的实部在数学上完全相等，而数值求根
        //     （findRootsNumeric 的 Durand–Kerner）只会给到 ~1e-16 的一致度，
        //     直接用 double 比大小就会由这点噪音决定先后 —— RootOf(f,x,1) 与
        //     RootOf(f,x,2) 正是这样被排反的。量化是纯函数，得到的仍是合法的
        //     严格弱序（在 (ok, 实部, 虚部, 文本) 上做字典序）。
        struct Key { bool ok = false; double re = 0.0; double im = 0.0; std::string text; };
        auto quantize = [](double v) {
            const double scale = std::max(1.0, std::abs(v));
            const double eps = scale * 1e-9;
            return std::round(v / eps) * eps;
        };
        std::vector<Key> keys(stage.size());
        for (size_t i = 0; i < stage.size(); ++i) {
            keys[i].text = stage[i].toString();
            try {
                std::complex<double> c = fastEvalComplex(stage[i].ptr, {}, SymbolicFuncResolver{});
                if (std::isfinite(c.real()) && std::isfinite(c.imag())) {
                    keys[i].ok = true;
                    keys[i].re = quantize(c.real());
                    keys[i].im = quantize(c.imag());
                }
            } catch (const EngineInterruptError&) {
                throw;
            } catch (...) {
                // 数值化失败：ok 保持 false，排到末尾，段内按文本排
            }
        }

        std::vector<size_t> order(stage.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (keys[a].ok != keys[b].ok) return keys[a].ok;
            if (keys[a].ok) {
                if (keys[a].re != keys[b].re) return keys[a].re < keys[b].re;
                if (keys[a].im != keys[b].im) return keys[a].im < keys[b].im;
            }
            return keys[a].text < keys[b].text;
        });

        std::vector<SymExpr> sorted;
        sorted.reserve(stage.size());
        for (size_t idx : order) sorted.push_back(stage[idx]);
        roots.swap(sorted);
    }

    std::vector<SymExpr> solveEq(const SymExpr& expr, const std::string& var) {
        SymExpr factored = factor(expr);
        std::vector<SymExpr> roots;

        std::function<void(const SymExpr&)> processFactor = [&](const SymExpr& f) {
            if (!containsVar(f.ptr, var)) return;

            if (f.ptr->getType() == SymType::MUL) {
                for (auto& arg : static_cast<SymMul*>(f.ptr)->args) {
                    processFactor(SymExpr(arg));
                }
                return;
            }
            if (f.ptr->getType() == SymType::POW) {
                auto powNode = static_cast<SymPow*>(f.ptr);
                if (containsVar(powNode->base, var) && !containsVar(powNode->exp, var)) {
                    processFactor(SymExpr(powNode->base));
                }
                return;
            }

            auto coeffs = extractCoeffs(f, var);
            if (coeffs.empty()) {
                if (containsVar(f.ptr, var)) {
                    if (f.ptr->getType() == SymType::ADD) {
                        auto add = static_cast<SymAdd*>(f.ptr);
                        if (add->args.size() == 2) {
                            SymExpr term1(add->args[0]);
                            SymExpr term2(add->args[1]);
                            
                            auto checkExpEq = [&](SymExpr t1, SymExpr t2) {
                                if (!containsVar(t2.ptr, var)) {
                                    if (t1.ptr->getType() == SymType::POW) {
                                        auto p = static_cast<SymPow*>(t1.ptr);
                                        if (!containsVar(p->base, var) && p->exp->getType() == SymType::VAR && static_cast<SymVar*>(p->exp)->name == var) {
                                            SymExpr a = SymExpr(p->base);
                                            SymExpr b = simplifyCore(-t2);
                                            SymExpr log_b(new SymFunc("log", std::vector<SymNode*>{b.ptr}));
                                            SymExpr log_a(new SymFunc("log", std::vector<SymNode*>{a.ptr}));
                                            roots.push_back(simplifyCore(log_b / log_a));
                                            return true;
                                        }
                                    } else if (t1.ptr->getType() == SymType::MUL) {
                                        auto mul = static_cast<SymMul*>(t1.ptr);
                                        if (mul->args.size() == 2) {
                                            SymExpr c(mul->args[0]);
                                            SymExpr p(mul->args[1]);
                                            if (containsVar(c.ptr, var)) std::swap(c, p);
                                            if (!containsVar(c.ptr, var) && p.ptr->getType() == SymType::POW) {
                                                auto powNode = static_cast<SymPow*>(p.ptr);
                                                if (!containsVar(powNode->base, var) && powNode->exp->getType() == SymType::VAR && static_cast<SymVar*>(powNode->exp)->name == var) {
                                                    SymExpr a = SymExpr(powNode->base);
                                                    SymExpr b = simplifyCore(-t2 / c);
                                                    SymExpr log_b(new SymFunc("log", std::vector<SymNode*>{b.ptr}));
                                                    SymExpr log_a(new SymFunc("log", std::vector<SymNode*>{a.ptr}));
                                                    roots.push_back(simplifyCore(log_b / log_a));
                                                    return true;
                                                }
                                            }
                                        }
                                    } else if (t1.ptr->getType() == SymType::FUNC) {
                                        auto func = static_cast<SymFunc*>(t1.ptr);
                                        if (func->name == "exp" && func->args.size() == 1 && func->args[0]->getType() == SymType::VAR && static_cast<SymVar*>(func->args[0])->name == var) {
                                            SymExpr b = simplifyCore(-t2);
                                            SymExpr log_b(new SymFunc("log", std::vector<SymNode*>{b.ptr}));
                                            roots.push_back(log_b);
                                            return true;
                                        } else if (func->name == "log" && func->args.size() == 1 && func->args[0]->getType() == SymType::VAR && static_cast<SymVar*>(func->args[0])->name == var) {
                                            SymExpr b = simplifyCore(-t2);
                                            SymExpr exp_b(new SymFunc("exp", std::vector<SymNode*>{b.ptr}));
                                            roots.push_back(exp_b);
                                            return true;
                                        }
                                    }
                                }
                                return false;
                            };
                            
                            if (checkExpEq(term1, term2)) return;
                            if (checkExpEq(term2, term1)) return;
                        }
                    }
                    JC2_THROW(RuntimeError, "Transcendental or non-polynomial equation is not supported yet.");
                }
                return;
            }

            int degree = static_cast<int>(coeffs.size()) - 1;
            if (degree == 1) {
                // ax + b = 0 => x = -b/a
                SymExpr a = coeffs[1];
                SymExpr b = coeffs[0];
                if (!a.isZero()) {
                    roots.push_back(simplifyCore(-b / a));
                }
            } else if (degree == 2) {
                // ax^2 + bx + c = 0
                SymExpr a = coeffs[2];
                SymExpr b = coeffs[1];
                SymExpr c = coeffs[0];
                if (!a.isZero()) {
                    SymExpr delta = simplifyCore(b * b - SymExpr(BigInt(4)) * a * c);
                    SymExpr twoA = SymExpr(BigInt(2)) * a;
                    auto [ok, sqrtDelta] = trySquareRoot(delta, true);
                    if (!ok) {
                        bool isNeg = false;
                        if (delta.ptr->getType() == SymType::NUM) {
                            isNeg = isCasNegative(static_cast<SymNum*>(delta.ptr)->value);
                        } else if (delta.ptr->getType() == SymType::MUL) {
                            auto mul = static_cast<SymMul*>(delta.ptr);
                            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                                isNeg = isCasNegative(static_cast<SymNum*>(mul->args[0])->value);
                            }
                        }
                        if (isNeg) {
                            SymExpr I = SymExpr::makeConst(SymConstId::I);
                            auto [ok2, sqrtPosDelta] = trySquareRoot(-delta, true);
                            if (ok2) {
                                sqrtDelta = simplifyCore(I * sqrtPosDelta);
                            } else {
                                sqrtDelta = simplifyCore(I * ((-delta) ^ SymExpr(Fraction(1, 2))));
                            }
                        } else {
                            sqrtDelta = delta ^ SymExpr(Fraction(1, 2));
                        }
                    }
                    roots.push_back(simplifyCore((-b + sqrtDelta) / twoA));
                    roots.push_back(simplifyCore((-b - sqrtDelta) / twoA));
                }
            } else if (degree == 3) {
                // ★ 3 次仍给根式解（卡尔丹/二项式），形式紧凑可读。
                //   4 次及以上改走 RootOf（见下），因为 Ferrari 展开会爆炸：
                //   Φ_5 = x^4+x^3+x^2+x+1 曾输出几十行嵌套根式。
                auto radicalRoots = getExactRoots(coeffs);
                for (auto& rr : radicalRoots) roots.push_back(rr);
                if (radicalRoots.empty()) {
                    SymExpr f_monic = f;
                    if (!coeffs[degree].isOne()) {
                        f_monic = simplifyCore(expand_core(f / coeffs[degree], SymConfig::maxExpandTerms));
                    }
                    for (int k = 1; k <= degree; ++k) {
                        roots.push_back(SymExpr::makeFunc("RootOf", std::vector<SymNode*>{
                            f_monic.ptr, SymExpr::makeVar(var).ptr, SymExpr(BigInt(k)).ptr
                        }));
                    }
                }
            } else if (degree >= 4) {
                // ★ 4 次及以上默认返回 RootOf：紧凑、精确、可数值化，
                //   避免 Ferrari 展开成不可读的嵌套根式。
                //   （根式解仍可通过 getExactRoots 在需要处取得，例如积分里的部分分式。）
                SymExpr f_monic = f;
                if (!coeffs[degree].isOne()) {
                    f_monic = simplifyCore(expand_core(f / coeffs[degree], SymConfig::maxExpandTerms));
                }
                for (int k = 1; k <= degree; ++k) {
                    roots.push_back(SymExpr::makeFunc("RootOf", std::vector<SymNode*>{
                        f_monic.ptr, SymExpr::makeVar(var).ptr, SymExpr(BigInt(k)).ptr
                    }));
                }
            }
        };

        processFactor(factored);

        // 去重
        std::vector<SymExpr> uniqueRoots;
        for (const auto& r : roots) {
            bool found = false;
            for (const auto& ur : uniqueRoots) {
                if (r == ur) {
                    found = true;
                    break;
                }
            }
            if (!found) uniqueRoots.push_back(r);
        }

        // ★ 出口统一规范化 + 定序。
        //   放在去重之后：浅层分配可能把两个写法不同的根收敛到同一个规范形，
        //   这一步会再按指针去一次重。
        normalizeAndSortRoots(uniqueRoots);

        return uniqueRoots;
    }

    // =================================================================
    // 静默代入 (Quiet Substitution) - 避免除零异常的控制流开销
    // =================================================================
    static std::optional<SymExpr> trySubsQuiet(const SymExpr& expr, const std::string& var, const SymExpr& val) {
        checkInterrupt();
        if (!expr.ptr) return expr;

        switch (expr.ptr->getType()) {
        case SymType::NUM:
            return expr;

        case SymType::VAR: {
            auto v = static_cast<SymVar*>(expr.ptr);
            return (v->name == var) ? val : expr;
        }

        case SymType::ADD: {
            auto add = static_cast<SymAdd*>(expr.ptr);
            SymExpr result(BigInt(0));
            for (auto& arg : add->args) {
                auto subArg = trySubsQuiet(SymExpr(arg), var, val);
                if (!subArg) return std::nullopt;
                result = result + *subArg;
            }
            return result;
        }

        case SymType::MUL: {
            auto mul = static_cast<SymMul*>(expr.ptr);
            SymExpr result(BigInt(1));
            for (auto& arg : mul->args) {
                auto subArg = trySubsQuiet(SymExpr(arg), var, val);
                if (!subArg) return std::nullopt;
                result = result * *subArg;
            }
            return result;
        }

        case SymType::POW: {
            auto pow = static_cast<SymPow*>(expr.ptr);
            auto baseSub = trySubsQuiet(SymExpr(pow->base), var, val);
            if (!baseSub) return std::nullopt;
            auto expSub = trySubsQuiet(SymExpr(pow->exp), var, val);
            if (!expSub) return std::nullopt;

            auto [bOk, bVal] = tryEvalConst(*baseSub);
            auto [eOk, eVal] = tryEvalConst(*expSub);
            bool bIsZero = baseSub->isZero() || (bOk && !bVal.truthy());
            bool eIsZero = expSub->isZero() || (eOk && !eVal.truthy());

            if (bIsZero) {
                if (eIsZero) return std::nullopt; // 0^0
                bool eIsNeg = false;
                if (eOk) {
                    try { eIsNeg = eVal.asFloat() < 0.0; } catch(...) {}
                } else if (expSub->ptr->getType() == SymType::NUM) {
                    eIsNeg = isCasNegative(static_cast<SymNum*>(expSub->ptr)->value);
                }
                if (eIsNeg) return std::nullopt; // Division by zero
            }
            
            try {
                return (*baseSub) ^ (*expSub);
            } catch (...) {
                return std::nullopt;
            }
        }

        case SymType::FUNC: {
            auto func = static_cast<SymFunc*>(expr.ptr);
            if ((func->name == "RootOf" || func->name == "RootSum") && func->args.size() == 3) {
                if (func->args[1]->getType() == SymType::VAR) {
                    std::string dummy = static_cast<SymVar*>(func->args[1])->name;
                    if (var == dummy) return expr;
                }
                auto sub0 = trySubsQuiet(SymExpr(func->args[0]), var, val);
                if (!sub0) return std::nullopt;
                
                std::vector<SymNode*> newArgs;
                newArgs.push_back(sub0->ptr);
                newArgs.push_back(func->args[1]);
                
                if (func->name == "RootSum") {
                    auto sub2 = trySubsQuiet(SymExpr(func->args[2]), var, val);
                    if (!sub2) return std::nullopt;
                    newArgs.push_back(sub2->ptr);
                } else {
                    newArgs.push_back(func->args[2]);
                }
                return SymExpr::makeFunc(func->name, std::move(newArgs));
            }
            std::vector<SymNode*> newArgs;
            for (auto& arg : func->args) {
                auto subArg = trySubsQuiet(SymExpr(arg), var, val);
                if (!subArg) return std::nullopt;
                newArgs.push_back(subArg->ptr);
            }
            return SymExpr::makeFunc(func->name, std::move(newArgs));
        }
        default:
            return expr;
        }
    }

    // =================================================================
    // 🚀 泰勒展开 (Taylor Series)
    // =================================================================
    SymExpr taylor(const SymExpr& expr, const std::string& var, const SymExpr& a, int order) {
        if (order < 0) JC2_THROW(MathError, "Taylor expansion order must be non-negative.");
        if (!expr.ptr) return SymExpr(BigInt(0));
        SymExpr result(BigInt(0));
        SymExpr current_deriv = expr;
        BigInt fact(1);
        SymExpr term_base = SymExpr::makeVar(var) - a;

        for (int n = 0; n <= order; ++n) {
            if (n > 0) {
                current_deriv = diff(current_deriv, var);
                fact = fact * BigInt(n);
            }
            SymExpr coeff;
            if (auto subbed = trySubsQuiet(current_deriv, var, a)) {
                try {
                    coeff = simplify(*subbed);
                } catch (const EngineInterruptError&) {
                    throw;
                } catch (...) {
                    JC2_THROW(MathError, "Cannot compute Taylor expansion (derivative undefined at expansion point).");
                }
            } else {
                JC2_THROW(MathError, "Cannot compute Taylor expansion (derivative undefined at expansion point).");
            }
            
            if (!coeff.isZero()) {
                SymExpr term = coeff / SymExpr(fact);
                if (n > 0) {
                    term = term * (term_base ^ SymExpr(BigInt(n)));
                }
                result = result + term;
            }
        }
        return simplify(result);
    }

    // =================================================================
    // 🚀 极限计算 (Limit) - 工业级 Gruntz 渐近线展开算法
    // =================================================================
    // 废弃洛必达法则，采用自底向上的渐近线展开 (Asymptotic Expansion)
    // 避免高阶导数导致的表达式膨胀 (Expression Swell) 和死锁
    // =================================================================
    
    // 辅助结构：渐近级数 (Asymptotic Series)
    struct AsympSeries {
        std::map<Fraction, SymExpr> terms; // exponent -> coefficient
        Fraction order;
        
        AsympSeries(Fraction ord = Fraction(6)) : order(ord) {}
        
        void addTerm(Fraction deg, SymExpr coeff) {
            if (deg > order) return;
            if (terms.count(deg)) terms[deg] = simplifyCore(terms[deg] + coeff);
            else terms[deg] = coeff;
            if (terms[deg].isZero()) terms.erase(deg);
        }
        
        AsympSeries operator+(const AsympSeries& other) const {
            AsympSeries res(std::max(order, other.order));
            for (auto& kv : terms) res.addTerm(kv.first, kv.second);
            for (auto& kv : other.terms) res.addTerm(kv.first, kv.second);
            return res;
        }
        
        AsympSeries operator*(const AsympSeries& other) const {
            AsympSeries res(std::min(order, other.order));
            for (auto& kv1 : terms) {
                for (auto& kv2 : other.terms) {
                    res.addTerm(kv1.first + kv2.first, simplifyCore(kv1.second * kv2.second));
                }
            }
            return res;
        }
        
        AsympSeries inverse() const {
            if (terms.empty()) JC2_THROW(MathError, "Division by zero in asymptotic expansion.");
            auto lead = *terms.begin();
            Fraction leadDeg = lead.first;
            SymExpr leadCoeff = lead.second;
            
            AsympSeries res(order - leadDeg);
            res.addTerm(-leadDeg, simplifyCore(SymExpr(BigInt(1)) / leadCoeff));
            
            AsympSeries rem = *this;
            rem.terms.erase(leadDeg);
            
            AsympSeries currentTerm = res;
            int max_i = std::max(5, static_cast<int>(order.toFloat()));
            for (int i = 1; i <= max_i; ++i) {
                if (rem.terms.empty()) break;
                currentTerm = currentTerm * rem * AsympSeries(order);
                if (currentTerm.terms.empty() || currentTerm.terms.begin()->first > order) break;
                // 乘以 -1/leadCoeff
                AsympSeries negLead(order);
                negLead.addTerm(-leadDeg, simplifyCore(SymExpr(BigInt(-1)) / leadCoeff));
                currentTerm = currentTerm * negLead;
                
                for (auto& kv : currentTerm.terms) res.addTerm(kv.first, kv.second);
            }
            return res;
        }
    };

    static AsympSeries computeGruntzSeries(const SymExpr& expr, const std::string& t_var, Fraction order) {
        if (!expr.ptr) return AsympSeries(order);
        
        switch (expr.ptr->getType()) {
            case SymType::NUM: {
                AsympSeries s(order);
                s.addTerm(Fraction(0), expr);
                return s;
            }
            case SymType::VAR: {
                AsympSeries s(order);
                if (static_cast<SymVar*>(expr.ptr)->name == t_var) {
                    s.addTerm(Fraction(1), SymExpr(BigInt(1)));
                } else {
                    s.addTerm(Fraction(0), expr);
                }
                return s;
            }
            case SymType::ADD: {
                AsympSeries s(order);
                for (auto& arg : static_cast<SymAdd*>(expr.ptr)->args) {
                    s = s + computeGruntzSeries(SymExpr(arg), t_var, order);
                }
                return s;
            }
            case SymType::MUL: {
                AsympSeries s(order);
                s.addTerm(Fraction(0), SymExpr(BigInt(1)));
                for (auto& arg : static_cast<SymMul*>(expr.ptr)->args) {
                    s = s * computeGruntzSeries(SymExpr(arg), t_var, order);
                }
                return s;
            }
            case SymType::POW: {
                auto powNode = static_cast<SymPow*>(expr.ptr);
                AsympSeries baseS = computeGruntzSeries(SymExpr(powNode->base), t_var, order);
                if (powNode->exp->getType() == SymType::NUM) {
                    auto [isInt, n] = extractExactInt(static_cast<SymNum*>(powNode->exp)->value);
                    if (isInt) {
                        if (n == 0) {
                            AsympSeries s(order);
                            s.addTerm(Fraction(0), SymExpr(BigInt(1)));
                            return s;
                        }
                        if (n > 0) {
                            AsympSeries s = baseS;
                            for (int i = 1; i < n; ++i) s = s * baseS;
                            return s;
                        }
                        if (n < 0) {
                            AsympSeries s = baseS;
                            for (int i = 1; i < -n; ++i) s = s * baseS;
                            return s.inverse();
                        }
                    } else if (std::holds_alternative<Fraction>(static_cast<SymNum*>(powNode->exp)->value)) {
                        Fraction f = std::get<Fraction>(static_cast<SymNum*>(powNode->exp)->value);
                        if (baseS.terms.empty()) return AsympSeries(order);
                        auto lead = *baseS.terms.begin();
                        Fraction leadDeg = lead.first;
                        SymExpr leadCoeff = lead.second;
                        
                        AsympSeries res(order);
                        Fraction newLeadDeg = leadDeg * f;
                        SymExpr newLeadCoeff = simplifyCore(leadCoeff ^ SymExpr(f));
                        res.addTerm(newLeadDeg, newLeadCoeff);
                        
                        // 二项式展开 (1 + x)^f = 1 + f*x + f*(f-1)/2 * x^2 + ...
                        AsympSeries rem = baseS;
                        rem.terms.erase(leadDeg);
                        if (!rem.terms.empty()) {
                            AsympSeries invLead(order);
                            invLead.addTerm(-leadDeg, simplifyCore(SymExpr(BigInt(1)) / leadCoeff));
                            AsympSeries x = rem * invLead;
                            
                            AsympSeries binom(order);
                            binom.addTerm(Fraction(0), SymExpr(BigInt(1)));
                            
                            AsympSeries currentX = x;
                            SymExpr coeffF(f);
                            SymExpr currentCoeff = coeffF;
                            BigInt fact(1);
                            
                            int max_i = std::max(3, static_cast<int>(order.toFloat()));
                            for (int i = 1; i <= max_i; ++i) {
                                if (currentX.terms.empty() || currentX.terms.begin()->first > order) break;
                                AsympSeries term(order);
                                for (auto& kv : currentX.terms) {
                                    term.addTerm(kv.first, simplifyCore(kv.second * currentCoeff / SymExpr(fact)));
                                }
                                binom = binom + term;
                                
                                currentX = currentX * x;
                                currentCoeff = simplifyCore(currentCoeff * (coeffF - SymExpr(BigInt(i))));
                                fact = fact * BigInt(i + 1);
                            }
                            
                            AsympSeries finalRes(order);
                            for (auto& kv : binom.terms) {
                                finalRes.addTerm(kv.first + newLeadDeg, simplifyCore(kv.second * newLeadCoeff));
                            }
                            return finalRes;
                        }
                        return res;
                    }
                }
                // Convert variable power to exp and compute series
                SymExpr log_base(new SymFunc("log", std::vector<SymNode*>{powNode->base}));
                SymExpr rewritten(new SymFunc("exp", std::vector<SymNode*>{(SymExpr(powNode->exp) * log_base).ptr}));
                return computeGruntzSeries(rewritten, t_var, order);
            }
            case SymType::FUNC: {
                auto func = static_cast<SymFunc*>(expr.ptr);
                if (func->name == "exp" && func->args.size() == 1) {
                    AsympSeries argS = computeGruntzSeries(SymExpr(func->args[0]), t_var, order);
                    if (argS.terms.empty()) {
                        AsympSeries s(order);
                        s.addTerm(Fraction(0), SymExpr(BigInt(1)));
                        return s;
                    }
                    auto lead = *argS.terms.begin();
                    if (lead.first < Fraction(0)) {
                        // Essential singularity, fallback
                        AsympSeries s(order);
                        s.addTerm(Fraction(0), expr);
                        return s;
                    }
                    SymExpr c0(BigInt(0));
                    if (lead.first == Fraction(0)) {
                        c0 = lead.second;
                        argS.terms.erase(Fraction(0));
                    }
                    
                    AsympSeries res(order);
                    res.addTerm(Fraction(0), SymExpr(BigInt(1)));
                    AsympSeries currentTerm(order);
                    currentTerm.addTerm(Fraction(0), SymExpr(BigInt(1)));
                    BigInt fact(1);
                    
                    int max_i = std::max(5, static_cast<int>(order.toFloat()));
                    for (int i = 1; i <= max_i; ++i) {
                        if (argS.terms.empty()) break;
                        currentTerm = currentTerm * argS;
                        if (currentTerm.terms.empty() || currentTerm.terms.begin()->first > order) break;
                        fact = fact * BigInt(i);
                        
                        AsympSeries termToAdd(order);
                        for (auto& kv : currentTerm.terms) {
                            termToAdd.addTerm(kv.first, simplifyCore(kv.second / SymExpr(fact)));
                        }
                        res = res + termToAdd;
                    }
                    
                    if (!c0.isZero()) {
                        AsympSeries expC0(order);
                        expC0.addTerm(Fraction(0), simplifyCore(SymExpr::makeFunc("exp", std::vector<SymNode*>{c0.ptr})));
                        res = res * expC0;
                    }
                    return res;
                }
                if ((func->name == "sin" || func->name == "cos" || func->name == "tan" || 
                     func->name == "sinh" || func->name == "cosh" || func->name == "tanh" ||
                     func->name == "asin" || func->name == "atan" || func->name == "asinh" || func->name == "atanh" ||
                     func->name == "log") && func->args.size() == 1) {
                    AsympSeries argS = computeGruntzSeries(SymExpr(func->args[0]), t_var, order);
                    if (argS.terms.empty() || argS.terms.begin()->first < Fraction(0)) {
                        AsympSeries s(order);
                        s.addTerm(Fraction(0), expr);
                        return s;
                    }
                    SymExpr c0(BigInt(0));
                    if (argS.terms.begin()->first == Fraction(0)) {
                        c0 = argS.terms.begin()->second;
                        argS.terms.erase(Fraction(0));
                    }
                    
                    AsympSeries res(order);
                    if (func->name == "sin" || func->name == "sinh" || func->name == "asin" || func->name == "asinh" || func->name == "tan" || func->name == "tanh" || func->name == "atan" || func->name == "atanh") {
                        if (c0.isZero()) {
                            res.addTerm(Fraction(0), SymExpr(BigInt(0)));
                            AsympSeries currentTerm = argS;
                            res = res + currentTerm;
                            currentTerm = currentTerm * argS * argS;
                            AsympSeries term3(order);
                            SymExpr coeff3(BigInt(0));
                            if (func->name == "sin") coeff3 = SymExpr(Fraction(-1, 6));
                            else if (func->name == "sinh") coeff3 = SymExpr(Fraction(1, 6));
                            else if (func->name == "asin") coeff3 = SymExpr(Fraction(1, 6));
                            else if (func->name == "asinh") coeff3 = SymExpr(Fraction(-1, 6));
                            else if (func->name == "tan") coeff3 = SymExpr(Fraction(1, 3));
                            else if (func->name == "tanh") coeff3 = SymExpr(Fraction(-1, 3));
                            else if (func->name == "atan") coeff3 = SymExpr(Fraction(-1, 3));
                            else if (func->name == "atanh") coeff3 = SymExpr(Fraction(1, 3));
                            for (auto& kv : currentTerm.terms) term3.addTerm(kv.first, simplifyCore(kv.second * coeff3));
                            res = res + term3;
                            
                            currentTerm = currentTerm * argS * argS;
                            AsympSeries term5(order);
                            SymExpr coeff5(BigInt(0));
                            if (func->name == "sin") coeff5 = SymExpr(Fraction(1, 120));
                            else if (func->name == "sinh") coeff5 = SymExpr(Fraction(1, 120));
                            else if (func->name == "asin") coeff5 = SymExpr(Fraction(3, 40));
                            else if (func->name == "asinh") coeff5 = SymExpr(Fraction(3, 40));
                            else if (func->name == "tan") coeff5 = SymExpr(Fraction(2, 15));
                            else if (func->name == "tanh") coeff5 = SymExpr(Fraction(2, 15));
                            else if (func->name == "atan") coeff5 = SymExpr(Fraction(1, 5));
                            else if (func->name == "atanh") coeff5 = SymExpr(Fraction(1, 5));
                            for (auto& kv : currentTerm.terms) term5.addTerm(kv.first, simplifyCore(kv.second * coeff5));
                            res = res + term5;
                        } else {
                            AsympSeries s(order);
                            s.addTerm(Fraction(0), expr);
                            return s;
                        }
                    } else if (func->name == "cos" || func->name == "cosh") {
                        if (c0.isZero()) {
                            res.addTerm(Fraction(0), SymExpr(BigInt(1)));
                            AsympSeries currentTerm = argS * argS;
                            AsympSeries term2(order);
                            SymExpr coeff2 = (func->name == "cos") ? SymExpr(Fraction(-1, 2)) : SymExpr(Fraction(1, 2));
                            for (auto& kv : currentTerm.terms) term2.addTerm(kv.first, simplifyCore(kv.second * coeff2));
                            res = res + term2;
                            
                            currentTerm = currentTerm * argS * argS;
                            AsympSeries term4(order);
                            SymExpr coeff4 = (func->name == "cos") ? SymExpr(Fraction(1, 24)) : SymExpr(Fraction(1, 24));
                            for (auto& kv : currentTerm.terms) term4.addTerm(kv.first, simplifyCore(kv.second * coeff4));
                            res = res + term4;
                        } else {
                            AsympSeries s(order);
                            s.addTerm(Fraction(0), expr);
                            return s;
                        }
                    } else if (func->name == "log") {
                        if (argS.terms.empty()) {
                            AsympSeries s(order);
                            s.addTerm(Fraction(0), expr);
                            return s;
                        }
                        auto lead = *argS.terms.begin();
                        Fraction leadDeg = lead.first;
                        SymExpr leadCoeff = lead.second;
                        
                        AsympSeries res_log(order);
                        SymExpr logC(new SymFunc("log", std::vector<SymNode*>{leadCoeff.ptr}));
                        SymExpr log_t(new SymFunc("log", std::vector<SymNode*>{SymExpr::makeVar(t_var).ptr}));
                        SymExpr k_log_t = SymExpr(leadDeg) * log_t;
                        res_log.addTerm(Fraction(0), simplifyCore(logC + k_log_t));
                        
                        AsympSeries rem = argS;
                        rem.terms.erase(leadDeg);
                        if (!rem.terms.empty()) {
                            AsympSeries invLead(order);
                            invLead.addTerm(-leadDeg, simplifyCore(SymExpr(BigInt(1)) / leadCoeff));
                            AsympSeries x = rem * invLead;
                            
                            AsympSeries currentTerm = x;
                            AsympSeries log_rem(order);
                            log_rem = log_rem + currentTerm;
                            
                            currentTerm = currentTerm * x;
                            AsympSeries term2(order);
                            for (auto& kv : currentTerm.terms) term2.addTerm(kv.first, simplifyCore(kv.second * SymExpr(Fraction(-1, 2))));
                            log_rem = log_rem + term2;
                            
                            currentTerm = currentTerm * x;
                            AsympSeries term3(order);
                            for (auto& kv : currentTerm.terms) term3.addTerm(kv.first, simplifyCore(kv.second * SymExpr(Fraction(1, 3))));
                            log_rem = log_rem + term3;
                            
                            currentTerm = currentTerm * x;
                            AsympSeries term4(order);
                            for (auto& kv : currentTerm.terms) term4.addTerm(kv.first, simplifyCore(kv.second * SymExpr(Fraction(-1, 4))));
                            log_rem = log_rem + term4;
                            
                            currentTerm = currentTerm * x;
                            AsympSeries term5(order);
                            for (auto& kv : currentTerm.terms) term5.addTerm(kv.first, simplifyCore(kv.second * SymExpr(Fraction(1, 5))));
                            log_rem = log_rem + term5;
                            
                            res_log = res_log + log_rem;
                        }
                        return res_log;
                    }
                    return res;
                }
                // Fallback for other functions
                AsympSeries s(order);
                s.addTerm(Fraction(0), expr);
                return s;
            }
        }
        AsympSeries s(order);
        s.addTerm(Fraction(0), expr);
        return s;
    }

    // =================================================================
    // 🚀 极限计算 (Limit) - 真正的工业级 Gruntz 算法 (True Gruntz Algorithm)
    // =================================================================
    
    static SymExpr rewritePowToExp(const SymExpr& expr, const std::string& var) {
        if (!expr.ptr) return expr;
        switch (expr.ptr->getType()) {
            case SymType::ADD: {
                SymExpr res(BigInt(0));
                for (auto& arg : static_cast<SymAdd*>(expr.ptr)->args) res = res + rewritePowToExp(SymExpr(arg), var);
                return res;
            }
            case SymType::MUL: {
                SymExpr res(BigInt(1));
                for (auto& arg : static_cast<SymMul*>(expr.ptr)->args) res = res * rewritePowToExp(SymExpr(arg), var);
                return res;
            }
            case SymType::POW: {
                auto p = static_cast<SymPow*>(expr.ptr);
                SymExpr base = rewritePowToExp(SymExpr(p->base), var);
                SymExpr exp = rewritePowToExp(SymExpr(p->exp), var);
                if (containsVar(exp.ptr, var)) {
                    SymExpr log_base(new SymFunc("log", std::vector<SymNode*>{base.ptr}));
                    return SymExpr::makeFunc("exp", std::vector<SymNode*>{(exp * log_base).ptr});
                }
                return base ^ exp;
            }
            case SymType::FUNC: {
                auto f = static_cast<SymFunc*>(expr.ptr);
                std::vector<SymNode*> newArgs;
                for (auto& arg : f->args) newArgs.push_back(rewritePowToExp(SymExpr(arg), var).ptr);
                return SymExpr::makeFunc(f->name, std::move(newArgs));
            }
            default: return expr;
        }
    }

    static SymExpr gruntzInf(SymExpr expr, const std::string& var, int depth);

    static int compareGrowth(SymExpr f, SymExpr g, const std::string& var, int depth) {
        SymExpr L = gruntzInf(simplifyCore(f / g), var, depth + 1);
        if (L.isZero()) return -1; // f < g
        if (L.ptr->getType() == SymType::VAR) {
            std::string n = static_cast<SymVar*>(L.ptr)->name;
            if (n == "inf" || n == "-inf") return 1; // f > g
        }
        if (L.ptr->getType() == SymType::MUL) {
            for (auto& arg : static_cast<SymMul*>(L.ptr)->args) {
                if (arg->getType() == SymType::VAR) {
                    std::string n = static_cast<SymVar*>(arg)->name;
                    if (n == "inf" || n == "-inf") return 1;
                }
            }
        }
        return 0; // f ~ g
    }

    static std::vector<SymExpr> mrvMax(const std::vector<SymExpr>& A, const std::vector<SymExpr>& B, const std::string& var, int depth) {
        if (A.empty()) return B;
        if (B.empty()) return A;
        SymExpr a = A[0], b = B[0];
        if (a.ptr->getType() == SymType::VAR && b.ptr->getType() == SymType::VAR) {
            std::vector<SymExpr> res = A;
            for (auto& x : B) {
                bool found = false;
                for (auto& y : res) if (x == y) found = true;
                if (!found) res.push_back(x);
            }
            return res;
        }
        if (a.ptr->getType() == SymType::VAR) return B;
        if (b.ptr->getType() == SymType::VAR) return A;
        
        SymExpr f = SymExpr(static_cast<SymFunc*>(a.ptr)->args[0]);
        SymExpr g = SymExpr(static_cast<SymFunc*>(b.ptr)->args[0]);
        int cmp = compareGrowth(f, g, var, depth);
        if (cmp == 1) return A;
        if (cmp == -1) return B;
        
        std::vector<SymExpr> res = A;
        for (auto& x : B) {
            bool found = false;
            for (auto& y : res) if (x == y) found = true;
            if (!found) res.push_back(x);
        }
        return res;
    }

    static std::vector<SymExpr> mrv(SymExpr expr, const std::string& var, int depth) {
        if (!containsVar(expr.ptr, var)) return {};
        if (expr.ptr->getType() == SymType::VAR && static_cast<SymVar*>(expr.ptr)->name == var) {
            return {expr};
        }
        if (expr.ptr->getType() == SymType::ADD) {
            std::vector<SymExpr> res;
            for (auto& arg : static_cast<SymAdd*>(expr.ptr)->args) {
                res = mrvMax(res, mrv(SymExpr(arg), var, depth), var, depth);
            }
            return res;
        }
        if (expr.ptr->getType() == SymType::MUL) {
            std::vector<SymExpr> res;
            for (auto& arg : static_cast<SymMul*>(expr.ptr)->args) {
                res = mrvMax(res, mrv(SymExpr(arg), var, depth), var, depth);
            }
            return res;
        }
        if (expr.ptr->getType() == SymType::POW) {
            auto p = static_cast<SymPow*>(expr.ptr);
            if (containsVar(p->exp, var)) {
                SymExpr base = SymExpr(p->base);
                SymExpr exp = SymExpr(p->exp);
                SymExpr log_base(new SymFunc("log", std::vector<SymNode*>{base.ptr}));
                SymExpr rewritten(new SymFunc("exp", std::vector<SymNode*>{(exp * log_base).ptr}));
                return mrv(rewritten, var, depth);
            } else {
                return mrv(SymExpr(p->base), var, depth);
            }
        }
        if (expr.ptr->getType() == SymType::FUNC) {
            auto f = static_cast<SymFunc*>(expr.ptr);
            if (f->name == "exp") {
                SymExpr arg(f->args[0]);
                std::vector<SymExpr> arg_mrv = mrv(arg, var, depth);
                SymExpr L = gruntzInf(arg, var, depth + 1);
                bool isInf = false;
                if (L.ptr->getType() == SymType::VAR) {
                    std::string n = static_cast<SymVar*>(L.ptr)->name;
                    if (n == "inf" || n == "-inf") isInf = true;
                } else if (L.ptr->getType() == SymType::MUL) {
                    for (auto& a : static_cast<SymMul*>(L.ptr)->args) {
                        if (a->getType() == SymType::VAR) {
                            std::string n = static_cast<SymVar*>(a)->name;
                            if (n == "inf" || n == "-inf") isInf = true;
                        }
                    }
                }
                
                if (isInf) {
                    return mrvMax(arg_mrv, {expr}, var, depth);
                } else {
                    return arg_mrv;
                }
            }
            std::vector<SymExpr> res;
            for (auto& arg : f->args) {
                res = mrvMax(res, mrv(SymExpr(arg), var, depth), var, depth);
            }
            return res;
        }
        return {};
    }

    static SymExpr rewriteMRV(SymExpr expr, const std::vector<SymExpr>& omega, const std::string& var, const std::string& w_var, SymExpr g, int depth) {
        if (!containsVar(expr.ptr, var)) return expr;
        if (expr.ptr->getType() == SymType::VAR && static_cast<SymVar*>(expr.ptr)->name == var) {
            return expr;
        }
        
        for (auto& o : omega) {
            if (expr == o) {
                if (expr.ptr->getType() == SymType::FUNC && static_cast<SymFunc*>(expr.ptr)->name == "exp") {
                    SymExpr f = SymExpr(static_cast<SymFunc*>(expr.ptr)->args[0]);
                    SymExpr c = gruntzInf(simplifyCore(f / g), var, depth + 1);
                    SymExpr w = SymExpr::makeVar(w_var);
                    SymExpr rem = simplifyCore(f - c * g);
                    SymExpr exp_rem(new SymFunc("exp", std::vector<SymNode*>{rem.ptr}));
                    return (w ^ simplifyCore(-c)) * rewriteMRV(exp_rem, omega, var, w_var, g, depth);
                }
            }
        }
        
        switch (expr.ptr->getType()) {
            case SymType::ADD: {
                SymExpr res(BigInt(0));
                for (auto& arg : static_cast<SymAdd*>(expr.ptr)->args) res = res + rewriteMRV(SymExpr(arg), omega, var, w_var, g, depth);
                return res;
            }
            case SymType::MUL: {
                SymExpr res(BigInt(1));
                for (auto& arg : static_cast<SymMul*>(expr.ptr)->args) res = res * rewriteMRV(SymExpr(arg), omega, var, w_var, g, depth);
                return res;
            }
            case SymType::POW: {
                auto p = static_cast<SymPow*>(expr.ptr);
                return rewriteMRV(SymExpr(p->base), omega, var, w_var, g, depth) ^ rewriteMRV(SymExpr(p->exp), omega, var, w_var, g, depth);
            }
            case SymType::FUNC: {
                auto f = static_cast<SymFunc*>(expr.ptr);
                std::vector<SymNode*> newArgs;
                for (auto& arg : f->args) newArgs.push_back(rewriteMRV(SymExpr(arg), omega, var, w_var, g, depth).ptr);
                return SymExpr::makeFunc(f->name, std::move(newArgs));
            }
            default: return expr;
        }
    }

    static SymExpr gruntzInf(SymExpr expr, const std::string& var, int depth) {
        if (depth > SymConfig::maxDepth * 3) JC2_THROW(MathError, "Gruntz limit depth exceeded.");
        if (!containsVar(expr.ptr, var)) return expr;
        
        expr = simplifyCore(rewritePowToExp(expr, var));
        std::vector<SymExpr> omega = mrv(expr, var, depth);
        if (omega.empty()) return expr;
        
        auto getInfSign = [&](const SymExpr& C) -> SymExpr {
            SymExpr C_lim = gruntzInf(C, var, depth + 1);
            bool isNeg = false;
            if (C_lim.ptr->getType() == SymType::NUM) {
                isNeg = isCasNegative(static_cast<SymNum*>(C_lim.ptr)->value);
            } else if (C_lim.ptr->getType() == SymType::MUL) {
                auto mul = static_cast<SymMul*>(C_lim.ptr);
                if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM) {
                    isNeg = isCasNegative(static_cast<SymNum*>(mul->args[0])->value);
                }
            }
            return isNeg ? SymExpr::makeVar("-inf") : SymExpr::makeVar("inf");
        };
        
        if (omega.size() == 1 && omega[0].ptr->getType() == SymType::VAR) {
            bool has_log = false;
            std::function<void(SymNode*)> check_log = [&](SymNode* node) {
                if (!node || has_log) return;
                if (node->getType() == SymType::FUNC && static_cast<SymFunc*>(node)->name == "log") {
                    has_log = true;
                    return;
                }
                if (node->getType() == SymType::ADD) for (auto& arg : static_cast<SymAdd*>(node)->args) check_log(arg);
                else if (node->getType() == SymType::MUL) for (auto& arg : static_cast<SymMul*>(node)->args) check_log(arg);
                else if (node->getType() == SymType::POW) {
                    auto powNode = static_cast<SymPow*>(node);
                    if (powNode->exp->getType() != SymType::NUM) {
                        has_log = true;
                        return;
                    }
                    check_log(powNode->base);
                    check_log(powNode->exp);
                }
                else if (node->getType() == SymType::FUNC) for (auto& arg : static_cast<SymFunc*>(node)->args) check_log(arg);
            };
            check_log(expr.ptr);

            if (has_log) {
                SymExpr exp_var(new SymFunc("exp", std::vector<SymNode*>{SymExpr::makeVar(var).ptr}));
                SymExpr new_expr = simplifyCore(subs(expr, var, exp_var));
                return gruntzInf(new_expr, var, depth + 1);
            }

            std::string t_var = "_t_inf";
            SymExpr t = SymExpr::makeVar(t_var);
            SymExpr expr_t = simplifyCore(subs(expr, var, SymExpr(BigInt(1)) / t));
            
            for (int order = 6; order <= 24; order += 6) {
                try {
                    AsympSeries s = computeGruntzSeries(expr_t, t_var, Fraction(order));
                    if (s.terms.empty()) return SymExpr(BigInt(0));
                    auto lead = *s.terms.begin();
                    if (lead.first >= Fraction(order)) continue;
                    
                    SymExpr leadCoeff = lead.second;
                    SymExpr log_t(new SymFunc("log", std::vector<SymNode*>{t.ptr}));
                    leadCoeff = simplifyCore(applyRule(leadCoeff, log_t, -SymExpr::makeFunc("log", std::vector<SymNode*>{SymExpr::makeVar(var).ptr})));
                    
                    if (lead.first > Fraction(0)) return SymExpr(BigInt(0));
                    if (lead.first < Fraction(0)) return getInfSign(leadCoeff);
                    return gruntzInf(leadCoeff, var, depth + 1);
                } catch (...) { break; }
            }
            return expr;
        }
        
        SymExpr g = SymExpr(static_cast<SymFunc*>(omega[0].ptr)->args[0]);
        
        // Ensure g -> +oo
        SymExpr L_g = gruntzInf(g, var, depth + 1);
        bool g_is_neg_inf = false;
        if (L_g.ptr->getType() == SymType::VAR && static_cast<SymVar*>(L_g.ptr)->name == "-inf") {
            g_is_neg_inf = true;
        } else if (L_g.ptr->getType() == SymType::MUL) {
            auto mul = static_cast<SymMul*>(L_g.ptr);
            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM && isCasNegative(static_cast<SymNum*>(mul->args[0])->value)) {
                for (auto& a : mul->args) {
                    if (a->getType() == SymType::VAR && static_cast<SymVar*>(a)->name == "inf") {
                        g_is_neg_inf = true;
                    }
                }
            }
        }
        if (g_is_neg_inf) {
            g = simplifyCore(-g);
        }
        
        std::string w_var = "_w";
        SymExpr w = SymExpr::makeVar(w_var);
        SymExpr rewritten = rewriteMRV(expr, omega, var, w_var, g, depth);
        
        for (int order = 6; order <= 24; order += 6) {
            try {
                AsympSeries s = computeGruntzSeries(rewritten, w_var, Fraction(order));
                if (s.terms.empty()) return SymExpr(BigInt(0));
                auto lead = *s.terms.begin();
                if (lead.first >= Fraction(order)) continue;
                
                SymExpr leadCoeff = lead.second;
                SymExpr log_w(new SymFunc("log", std::vector<SymNode*>{w.ptr}));
                leadCoeff = simplifyCore(applyRule(leadCoeff, log_w, -g));
                
                if (lead.first > Fraction(0)) return SymExpr(BigInt(0));
                if (lead.first < Fraction(0)) return getInfSign(leadCoeff);
                return gruntzInf(leadCoeff, var, depth + 1);
            } catch (...) { break; }
        }
        return expr;
    }

    static SymExpr limitCore(const SymExpr& expr, const std::string& var, const SymExpr& val, const std::string& dir, int depth) {
        if (depth > SymConfig::maxDepth) JC2_THROW(CalculusError, "Limit evaluation depth exceeded.");
        if (!expr.ptr) return expr;

        bool isInfLimit = false;
        bool isNegInfLimit = false;
        
        if (val.ptr->getType() == SymType::VAR) {
            std::string vName = static_cast<SymVar*>(val.ptr)->name;
            if (vName == "inf" || vName == "Infinity") isInfLimit = true;
            if (vName == "-inf") { isInfLimit = true; isNegInfLimit = true; }
        } else if (val.ptr->getType() == SymType::MUL) {
            auto mul = static_cast<SymMul*>(val.ptr);
            if (!mul->args.empty() && mul->args[0]->getType() == SymType::NUM && isCasNegative(static_cast<SymNum*>(mul->args[0])->value)) {
                for (auto& a : mul->args) {
                    if (a->getType() == SymType::VAR && static_cast<SymVar*>(a)->name == "inf") {
                        isInfLimit = true;
                        isNegInfLimit = true;
                    }
                }
            }
        }

        // 1. 尝试直接代入 (静默模式，不抛出除零异常)
        // 仅当极限点不是无穷大时才尝试直接代入
        if (!isInfLimit) {
            if (auto subbed = trySubsQuiet(expr, var, val)) {
                try {
                    SymExpr simp = simplify(*subbed);
                    if (!containsVar(simp.ptr, var)) return simp;
                } catch (...) {}
            }
        }

        // 2. 转换为 x -> oo 的标准 Gruntz 形式
        std::string t_var = "_t_inf";
        SymExpr t = SymExpr::makeVar(t_var);
        SymExpr expr_inf;
        
        if (isInfLimit) {
            if (isNegInfLimit) {
                expr_inf = subs(expr, var, -t);
            } else {
                expr_inf = subs(expr, var, t);
            }
        } else {
            // x -> val 转化为 t -> oo
            if (dir == "-") {
                expr_inf = subs(expr, var, val - SymExpr(BigInt(1)) / t);
            } else {
                expr_inf = subs(expr, var, val + SymExpr(BigInt(1)) / t);
            }
        }
        
        return gruntzInf(simplifyCore(expr_inf), t_var, 0);
    }

    SymExpr limit(const SymExpr& expr, const std::string& var, const SymExpr& val, const std::string& dir) {
        if (dir == "" || dir == "both") {
            SymExpr right, left;
            bool rightOk = false, leftOk = false;
            try { right = simplify(limitCore(expr, var, val, "+", 0)); rightOk = true; } catch (const EngineInterruptError&) { throw; } catch (...) {}
            try { left = simplify(limitCore(expr, var, val, "-", 0)); leftOk = true; } catch (const EngineInterruptError&) { throw; } catch (...) {}
            
            if (rightOk && leftOk) {
                if (right == left) return right;
                JC2_THROW(MathError, "Limit does not exist (left and right limits differ).");
            }
            
            JC2_THROW(MathError, "Limit does not exist or is undefined.");
        }
        return simplify(limitCore(expr, var, val, dir, 0));
    }

    // =================================================================
    // 🚀 三角化简 (Trigonometric Simplification)
    // =================================================================
    // 表达式里是否含三角函数节点（trigsimp 只对这类表达式可能有作用）
    static bool hasTrigFunc(SymNode* node, int depth = 0) {
        if (!node || depth > 256) return false;
        switch (node->getType()) {
        case SymType::FUNC: {
            auto f = static_cast<SymFunc*>(node);
            static const std::set<std::string> kTrig = {
                "sin", "cos", "tan", "cot", "sec", "csc",
                "asin", "acos", "atan", "sinh", "cosh", "tanh"
            };
            if (kTrig.count(f->name)) return true;
            for (auto& a : f->args) if (hasTrigFunc(a, depth + 1)) return true;
            return false;
        }
        case SymType::ADD:
            for (auto& a : static_cast<SymAdd*>(node)->args)
                if (hasTrigFunc(a, depth + 1)) return true;
            return false;
        case SymType::MUL:
            for (auto& a : static_cast<SymMul*>(node)->args)
                if (hasTrigFunc(a, depth + 1)) return true;
            return false;
        case SymType::POW:
            return hasTrigFunc(static_cast<SymPow*>(node)->base, depth + 1) ||
                   hasTrigFunc(static_cast<SymPow*>(node)->exp, depth + 1);
        default:
            return false;
        }
    }

    SymExpr trigsimp(const SymExpr& expr) {
        if (!expr.ptr) return expr;

        // ★ 无三角函数时直接返回：trigsimp 的规则库只处理三角恒等式，
        //   对纯多项式毫无作用，却仍要在"规则 × 迭代"的双层循环里反复
        //   做深拷贝、遍历与化简（实测纯多项式上快约 9%）。
        if (!hasTrigFunc(expr.ptr)) return expr;

        // 从规则库获取三角化简规则
        const std::vector<std::pair<SymExpr, SymExpr>>& rules = getTrigRules();

        SymExpr current = expr;
        bool changed = true;
        
        // 循环尝试应用规则，直到表达式不再发生变化
        int iter = 0;
        while (changed && iter++ < SymConfig::maxIterations) {
            changed = false;
            for (const auto& rule : rules) {
                SymExpr next = applyRule(current, rule.first, rule.second);
                if (next.ptr != current.ptr) {
                    SymExpr simplifiedNext = simplifyCore(next);
                    if (simplifiedNext.ptr != current.ptr) {
                        current = simplifiedNext;
                        changed = true;
                        break;
                    }
                }
            }
        }

        return current;
    }

    // =================================================================
    // 🚀 刘维尔域规范化 (Liouvillian Field Normalization)
    // =================================================================
    static Fraction gcdFraction(const Fraction& f1, const Fraction& f2) {
        if (f1.getNum().isZero()) return f2;
        if (f2.getNum().isZero()) return f1;
        BigInt num = BigInt::gcd(f1.getNum(), f2.getNum());
        BigInt den = BigInt::lcm(f1.getDen(), f2.getDen());
        return Fraction(num, den);
    }

    static std::pair<Fraction, SymExpr> extractRationalCoeff(const SymExpr& expr) {
        if (expr.ptr->getType() == SymType::NUM) {
            auto num = static_cast<SymNum*>(expr.ptr);
            if (std::holds_alternative<int32_t>(num->value)) {
                return {Fraction(BigInt(std::get<int32_t>(num->value)), BigInt(1)), SymExpr(BigInt(1))};
            } else if (std::holds_alternative<BigInt>(num->value)) {
                return {Fraction(std::get<BigInt>(num->value), BigInt(1)), SymExpr(BigInt(1))};
            } else if (std::holds_alternative<Fraction>(num->value)) {
                return {std::get<Fraction>(num->value), SymExpr(BigInt(1))};
            }
        } else if (expr.ptr->getType() == SymType::MUL) {
            auto mul = static_cast<SymMul*>(expr.ptr);
            BigInt num_val(1);
            BigInt den_val(1);
            std::vector<SymNode*> rest;
            for (auto& arg : mul->args) {
                if (arg->getType() == SymType::NUM) {
                    auto num = static_cast<SymNum*>(arg);
                    if (std::holds_alternative<int32_t>(num->value)) {
                        num_val = num_val * BigInt(std::get<int32_t>(num->value));
                    } else if (std::holds_alternative<BigInt>(num->value)) {
                        num_val = num_val * std::get<BigInt>(num->value);
                    } else if (std::holds_alternative<Fraction>(num->value)) {
                        Fraction f = std::get<Fraction>(num->value);
                        num_val = num_val * f.getNum();
                        den_val = den_val * f.getDen();
                    } else {
                        rest.push_back(arg);
                    }
                } else {
                    rest.push_back(arg);
                }
            }
            Fraction q(num_val, den_val);
            if (rest.empty()) return {q, SymExpr(BigInt(1))};
            if (rest.size() == 1) return {q, SymExpr(rest[0])};
            return {q, SymExpr::makeMul(rest)};
        }
        return {Fraction(BigInt(1), BigInt(1)), expr};
    }

    static SymExpr flattenLogExp(const SymExpr& expr) {
        checkInterrupt();
        if (!expr.ptr) return expr;
        switch (expr.ptr->getType()) {
            case SymType::ADD: {
                SymExpr res(BigInt(0));
                for (auto& arg : static_cast<SymAdd*>(expr.ptr)->args)
                    res = res + flattenLogExp(SymExpr(arg));
                return res;
            }
            case SymType::MUL: {
                SymExpr res(BigInt(1));
                for (auto& arg : static_cast<SymMul*>(expr.ptr)->args)
                    res = res * flattenLogExp(SymExpr(arg));
                return res;
            }
            case SymType::POW: {
                auto powNode = static_cast<SymPow*>(expr.ptr);
                return flattenLogExp(SymExpr(powNode->base)) ^ flattenLogExp(SymExpr(powNode->exp));
            }
            case SymType::FUNC: {
                auto func = static_cast<SymFunc*>(expr.ptr);
                if (func->name == "log" && func->args.size() == 1) {
                    SymExpr inner = flattenLogExp(SymExpr(func->args[0]));
                    if (inner.ptr->getType() == SymType::MUL) {
                        SymExpr res(BigInt(0));
                        for (auto& arg : static_cast<SymMul*>(inner.ptr)->args) {
                            res = res + flattenLogExp(SymExpr::makeFunc("log", std::vector<SymNode*>{arg}));
                        }
                        return res;
                    }
                    if (inner.ptr->getType() == SymType::POW) {
                        auto powNode = static_cast<SymPow*>(inner.ptr);
                        SymExpr baseLog = flattenLogExp(SymExpr::makeFunc("log", std::vector<SymNode*>{powNode->base}));
                        return SymExpr(powNode->exp) * baseLog;
                    }
                    return SymExpr::makeFunc("log", std::vector<SymNode*>{inner.ptr});
                }
                if (func->name == "exp" && func->args.size() == 1) {
                    SymExpr inner = flattenLogExp(SymExpr(func->args[0]));
                    if (inner.ptr->getType() == SymType::ADD) {
                        SymExpr res(BigInt(1));
                        for (auto& arg : static_cast<SymAdd*>(inner.ptr)->args) {
                            res = res * flattenLogExp(SymExpr::makeFunc("exp", std::vector<SymNode*>{arg}));
                        }
                        return res;
                    }
                    return SymExpr::makeFunc("exp", std::vector<SymNode*>{inner.ptr});
                }
                std::vector<SymNode*> newArgs;
                for (auto& arg : func->args) newArgs.push_back(flattenLogExp(SymExpr(arg)).ptr);
                return SymExpr::makeFunc(func->name, std::move(newArgs));
            }
            default: return expr;
        }
    }

    SymExpr rischNormalize(const SymExpr& expr) {
        checkInterrupt();
        SymExpr flat = flattenLogExp(expand_core(expr, SymConfig::maxExpandTerms));
        
        std::vector<SymExpr> allExps;
        std::function<void(const SymExpr&)> collectExps = [&](const SymExpr& e) {
            if (!e.ptr) return;
            if (e.ptr->getType() == SymType::FUNC) {
                auto func = static_cast<SymFunc*>(e.ptr);
                if (func->name == "exp" && func->args.size() == 1) {
                    allExps.push_back(e);
                }
                for (auto& arg : func->args) collectExps(SymExpr(arg));
            } else if (e.ptr->getType() == SymType::ADD) {
                for (auto& arg : static_cast<SymAdd*>(e.ptr)->args) collectExps(SymExpr(arg));
            } else if (e.ptr->getType() == SymType::MUL) {
                for (auto& arg : static_cast<SymMul*>(e.ptr)->args) collectExps(SymExpr(arg));
            } else if (e.ptr->getType() == SymType::POW) {
                auto powNode = static_cast<SymPow*>(e.ptr);
                collectExps(SymExpr(powNode->base));
                collectExps(SymExpr(powNode->exp));
            }
        };
        collectExps(flat);

        std::map<SymNode*, std::pair<SymExpr, Fraction>> expGroups;
        for (const auto& expNode : allExps) {
            auto func = static_cast<SymFunc*>(expNode.ptr);
            SymExpr inner(func->args[0]);
            auto [q, prim] = extractRationalCoeff(inner);
            SymNode* sig = prim.ptr;
            if (expGroups.count(sig)) {
                expGroups[sig].second = gcdFraction(expGroups[sig].second, q);
            } else {
                expGroups[sig] = {prim, q};
            }
        }

        std::function<SymExpr(const SymExpr&)> replaceExps = [&](const SymExpr& e) -> SymExpr {
            if (!e.ptr) return e;
            if (e.ptr->getType() == SymType::FUNC) {
                auto func = static_cast<SymFunc*>(e.ptr);
                if (func->name == "exp" && func->args.size() == 1) {
                    SymExpr inner = replaceExps(SymExpr(func->args[0]));
                    auto [q, prim] = extractRationalCoeff(inner);
                    SymNode* sig = prim.ptr;
                    if (expGroups.count(sig)) {
                        Fraction g = expGroups[sig].second;
                        if (g.getNum() > BigInt(0)) {
                            Fraction power(q.getNum() * g.getDen(), q.getDen() * g.getNum());
                            SymExpr baseExp(new SymFunc("exp", std::vector<SymNode*>{(SymExpr(g) * prim).ptr}));
                            if (power.getNum() == power.getDen()) return baseExp;
                            return baseExp ^ SymExpr(power);
                        }
                    }
                    return SymExpr::makeFunc("exp", std::vector<SymNode*>{inner.ptr});
                }
                std::vector<SymNode*> newArgs;
                for (auto& arg : func->args) newArgs.push_back(replaceExps(SymExpr(arg)).ptr);
                return SymExpr::makeFunc(func->name, std::move(newArgs));
            } else if (e.ptr->getType() == SymType::ADD) {
                SymExpr res(BigInt(0));
                for (auto& arg : static_cast<SymAdd*>(e.ptr)->args) res = res + replaceExps(SymExpr(arg));
                return res;
            } else if (e.ptr->getType() == SymType::MUL) {
                SymExpr res(BigInt(1));
                for (auto& arg : static_cast<SymMul*>(e.ptr)->args) res = res * replaceExps(SymExpr(arg));
                return res;
            } else if (e.ptr->getType() == SymType::POW) {
                auto powNode = static_cast<SymPow*>(e.ptr);
                return replaceExps(SymExpr(powNode->base)) ^ replaceExps(SymExpr(powNode->exp));
            }
            return e;
        };

        return replaceExps(flat);
    }

    // =================================================================
// 快速数值求值 (C++ 原生 Double 极限狂飙 + 依赖注入解耦)
// =================================================================
    static std::complex<double> fastEvalComplex(SymNode* node, const std::map<std::string, double>& env, const SymbolicFuncResolver& resolver) {
        if (!node) return 0.0;

        switch (node->getType()) {
        case SymType::NUM: {
            auto num = static_cast<SymNum*>(node);
            Value v = casValToValue(num->value);
            if (v.isComplex()) return std::complex<double>(v.asComplex().real, v.asComplex().imag);
            return v.asFloat();
        }
        case SymType::VAR: {
            auto varName = static_cast<SymVar*>(node)->name;
            auto it = env.find(varName);
            if (it != env.end()) return it->second;
            return 0.0;
        }
        case SymType::CONST: {
            Complex c = symbolicConstantValue(static_cast<SymConst*>(node)->id);
            return std::complex<double>(c.real, c.imag);
        }
        case SymType::ADD: {
            std::complex<double> sum = 0.0;
            for (auto& arg : static_cast<SymAdd*>(node)->args) sum += fastEvalComplex(arg, env, resolver);
            return sum;
        }
        case SymType::MUL: {
            std::complex<double> prod = 1.0;
            for (auto& arg : static_cast<SymMul*>(node)->args) prod *= fastEvalComplex(arg, env, resolver);
            return prod;
        }
        case SymType::POW: {
            auto p = static_cast<SymPow*>(node);
            return std::pow(fastEvalComplex(p->base, env, resolver), fastEvalComplex(p->exp, env, resolver));
        }
        case SymType::FUNC: {
            auto f = static_cast<SymFunc*>(node);
            if ((f->name == "RootOf" || f->name == "RootSum") && f->args.size() == 3) {
                Value v = evaluateRootNode(f, [&](const SymExpr& e) {
                    std::complex<double> c = fastEvalComplex(e.ptr, env, resolver);
                    if (c.imag() == 0.0) return Value(c.real());
                    return Value(Complex(c.real(), c.imag()));
                });
                if (v.isComplex()) return std::complex<double>(v.asComplex().real, v.asComplex().imag);
                return v.asFloat();
            }
            
            // 内置常见数学函数，避免跨界调用开销并支持复数
            if (f->args.size() == 1) {
                std::complex<double> arg = fastEvalComplex(f->args[0], env, resolver);
                if (f->name == "sqrt") return std::sqrt(arg);
                if (f->name == "cbrt") return std::pow(arg, 1.0/3.0);
                if (f->name == "exp") return std::exp(arg);
                if (f->name == "log") return std::log(arg);
                if (f->name == "sin") return std::sin(arg);
                if (f->name == "cos") return std::cos(arg);
                if (f->name == "tan") return std::tan(arg);
                if (f->name == "asin") return std::asin(arg);
                if (f->name == "acos") return std::acos(arg);
                if (f->name == "atan") return std::atan(arg);
                if (f->name == "sinh") return std::sinh(arg);
                if (f->name == "cosh") return std::cosh(arg);
                if (f->name == "tanh") return std::tanh(arg);
                if (f->name == "abs") return std::abs(arg);
            } else if (f->args.size() == 2) {
                if (f->name == "root") {
                    std::complex<double> base = fastEvalComplex(f->args[0], env, resolver);
                    std::complex<double> n = fastEvalComplex(f->args[1], env, resolver);
                    return std::pow(base, 1.0 / n);
                }
            }

            if (!resolver) JC2_THROW(InternalError, "No function resolver provided for '" + f->name + "'.");

            std::vector<Value> callArgs;
            callArgs.reserve(f->args.size());
            for (auto& arg : f->args) {
                std::complex<double> c = fastEvalComplex(arg, env, resolver);
                if (c.imag() == 0.0) callArgs.push_back(Value(c.real()));
                else callArgs.push_back(Value(Complex(c.real(), c.imag())));
            }
            Value res = resolver(f->name, callArgs);
            if (res.isComplex()) return std::complex<double>(res.asComplex().real, res.asComplex().imag);
            return res.asFloat();
        }
        }
        return 0.0;
    }

    double fastEval(SymNode* node, const std::map<std::string, double>& env, const SymbolicFuncResolver& resolver) {
        std::complex<double> res = fastEvalComplex(node, env, resolver);
        return res.real();
    }

    // =================================================================
// 万能多态求值 (高维张量/复平面支援 + 依赖注入解耦)
// =================================================================
    Value evalUniversal(SymNode* node, const std::map<std::string, Value>& env, const SymbolicFuncResolver& resolver) {
        if (!node) return Value(0.0);

        switch (node->getType()) {
        case SymType::NUM: {
            auto num = static_cast<SymNum*>(node);
            return casValToValue(num->value);
        }
        case SymType::VAR: {
            auto varName = static_cast<SymVar*>(node)->name;
            auto it = env.find(varName);
            if (it != env.end()) return it->second;
            return Value(0.0);
        }
        case SymType::CONST: {
            return Value(symbolicConstantValue(static_cast<SymConst*>(node)->id));
        }
        case SymType::ADD: {
            Value sum(0.0);
            for (auto& arg : static_cast<SymAdd*>(node)->args) sum = sum + evalUniversal(arg, env, resolver);
            return sum;
        }
        case SymType::MUL: {
            Value prod(1.0);
            for (auto& arg : static_cast<SymMul*>(node)->args) prod = prod * evalUniversal(arg, env, resolver);
            return prod;
        }
        case SymType::POW: {
            auto p = static_cast<SymPow*>(node);
            Value b = evalUniversal(p->base, env, resolver);
            Value e = evalUniversal(p->exp, env, resolver);
            Value res = b ^ e;
            
            if (res.isFloat() && std::isnan(res.asFloatRaw())) {
                double br = 0.0, bi = 0.0, er = 0.0, ei = 0.0;
                bool ok_cast = true;
                try {
                    if (b.isComplex()) { br = b.asComplex().real; bi = b.asComplex().imag; }
                    else { br = b.asFloat(); }
                    if (e.isComplex()) { er = e.asComplex().real; ei = e.asComplex().imag; }
                    else { er = e.asFloat(); }
                } catch (const EngineInterruptError&) { throw; } catch (...) { ok_cast = false; }
                
                if (ok_cast) {
                    std::complex<double> bc(br, bi);
                    std::complex<double> ec(er, ei);
                    std::complex<double> cres = std::pow(bc, ec);
                    if (cres.imag() == 0.0) return Value(cres.real());
                    return Value(Complex(cres.real(), cres.imag()));
                }
            }
            return res;
        }
        case SymType::FUNC: {
            auto f = static_cast<SymFunc*>(node);
            if ((f->name == "RootOf" || f->name == "RootSum") && f->args.size() == 3) {
                return evaluateRootNode(f, [&](const SymExpr& e) {
                    return evalUniversal(e.ptr, env, resolver);
                });
            }
            
            // 内置常见数学函数，避免跨界调用开销并支持复数
            if (f->args.size() == 1) {
                Value arg = evalUniversal(f->args[0], env, resolver);
                std::complex<double> c(0.0, 0.0);
                bool is_c = false;
                if (arg.isComplex()) { c = std::complex<double>(arg.asComplex().real, arg.asComplex().imag); is_c = true; }
                else if (arg.isFloat() || arg.isInt32() || arg.isObjType(ObjType::BIGINT) || arg.isObjType(ObjType::FRACTION)) {
                    c = std::complex<double>(arg.asFloat(), 0.0); is_c = true;
                }
                
                if (is_c) {
                    std::complex<double> res;
                    bool handled = true;
                    if (f->name == "sqrt") res = std::sqrt(c);
                    else if (f->name == "cbrt") res = std::pow(c, 1.0/3.0);
                    else if (f->name == "exp") res = std::exp(c);
                    else if (f->name == "log") res = std::log(c);
                    else if (f->name == "sin") res = std::sin(c);
                    else if (f->name == "cos") res = std::cos(c);
                    else if (f->name == "tan") res = std::tan(c);
                    else if (f->name == "asin") res = std::asin(c);
                    else if (f->name == "acos") res = std::acos(c);
                    else if (f->name == "atan") res = std::atan(c);
                    else if (f->name == "sinh") res = std::sinh(c);
                    else if (f->name == "cosh") res = std::cosh(c);
                    else if (f->name == "tanh") res = std::tanh(c);
                    else if (f->name == "abs") res = std::abs(c);
                    else handled = false;
                    
                    if (handled) {
                        if (res.imag() == 0.0) return Value(res.real());
                        return Value(Complex(res.real(), res.imag()));
                    }
                }
            } else if (f->args.size() == 2) {
                if (f->name == "root") {
                    Value base = evalUniversal(f->args[0], env, resolver);
                    Value n = evalUniversal(f->args[1], env, resolver);
                    std::complex<double> cb(base.isComplex() ? base.asComplex().real : base.asFloat(), base.isComplex() ? base.asComplex().imag : 0.0);
                    std::complex<double> cn(n.isComplex() ? n.asComplex().real : n.asFloat(), n.isComplex() ? n.asComplex().imag : 0.0);
                    std::complex<double> res = std::pow(cb, 1.0 / cn);
                    if (res.imag() == 0.0) return Value(res.real());
                    return Value(Complex(res.real(), res.imag()));
                }
            }

            if (!resolver) JC2_THROW(InternalError, "No function resolver provided for '" + f->name + "'.");

            // 同构打包发送给宿主环境处理
            std::vector<Value> callArgs;
            callArgs.reserve(f->args.size());
            for (auto& arg : f->args) {
                callArgs.push_back(evalUniversal(arg, env, resolver));
            }
            return resolver(f->name, callArgs);
        }
        }
        return Value(0.0);
    }
} // namespace jc
