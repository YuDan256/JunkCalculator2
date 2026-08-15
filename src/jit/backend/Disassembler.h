#ifndef JC2_JIT_DISASSEMBLER_H
#define JC2_JIT_DISASSEMBLER_H

// ============================================================================
// 轻量级 x86-64 反汇编器
// 用于把 MacroAssembler 发射的机器码解码回助记符，便于人工审查。
// 只覆盖 JIT 实际用到的指令子集，遇到未知字节以 "db 0xXX" 兜底（绝不越界/崩溃）。
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <string>
#include <sstream>
#include <iomanip>
#include <ostream>

namespace jc {
namespace jit {

namespace disasm_detail {

const char* gpr64(int r) {
    static const char* n[] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                              "r8","r9","r10","r11","r12","r13","r14","r15"};
    return (r >= 0 && r < 16) ? n[r] : "?";
}
const char* gpr32(int r) {
    static const char* n[] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi",
                              "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
    return (r >= 0 && r < 16) ? n[r] : "?";
}
const char* xmm(int r) {
    static const char* n[] = {"xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
                              "xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15"};
    return (r >= 0 && r < 16) ? n[r] : "?";
}
const char* condName(int c) {
    static const char* n[] = {"o","no","b","ae","e","ne","be","a","s","ns","p","np","l","ge","le","g"};
    return (c >= 0 && c < 16) ? n[c] : "?";
}

struct State {
    const uint8_t* p;
    size_t size;
    bool p66 = false, pf2 = false, pf3 = false;
    bool rexW = false, rexR = false, rexX = false, rexB = false;
    bool hasRex = false;
};

inline uint8_t rd8(State& s, bool& ok) {
    if (s.size < 1) { ok = false; return 0; }
    s.size--; return *s.p++;
}
inline uint16_t rd16(State& s, bool& ok) {
    if (s.size < 2) { ok = false; return 0; }
    uint16_t v = (uint16_t)s.p[0] | ((uint16_t)s.p[1] << 8);
    s.p += 2; s.size -= 2; return v;
}
inline uint32_t rd32(State& s, bool& ok) {
    if (s.size < 4) { ok = false; return 0; }
    uint32_t v = (uint32_t)s.p[0] | ((uint32_t)s.p[1] << 8) | ((uint32_t)s.p[2] << 16) | ((uint32_t)s.p[3] << 24);
    s.p += 4; s.size -= 4; return v;
}
inline uint64_t rd64(State& s, bool& ok) {
    if (s.size < 8) { ok = false; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= ((uint64_t)s.p[i]) << (8 * i);
    s.p += 8; s.size -= 8; return v;
}

// 解析 ModRM / SIB / disp，返回 r/m 操作数字符串（寄存器或内存）。
// regIdx: 传入 /r 字段的寄存器编号（已含 REX.R 扩展）。
struct ModRM {
    int mod, reg, rm;
};
inline ModRM readModRM(State& s, bool& ok) {
    uint8_t b = rd8(s, ok);
    ModRM m;
    m.mod = (b >> 6) & 3;
    m.reg = (b >> 3) & 7;
    m.rm = b & 7;
    return m;
}

inline std::string formatRm(State& s, const ModRM& m, bool& ok, bool isXmmReg) {
    if (m.mod == 3) {
        return isXmmReg ? xmm(m.rm | (s.rexB ? 8 : 0)) : gpr64(m.rm | (s.rexB ? 8 : 0));
    }
    // 内存操作数
    int base = -1, index = -1, scale = 1;
    int64_t disp = 0;
    bool hasDisp = false;
    bool ripRel = false;

    if (m.rm == 4) { // SIB
        uint8_t sib = rd8(s, ok);
        int sc = (sib >> 6) & 3;
        int idx = (sib >> 3) & 7;
        int bs = sib & 7;
        scale = 1 << sc;
        index = idx | (s.rexX ? 8 : 0);
        if (idx == 4 && !s.rexX) index = -1; // 无 index
        if (m.mod == 0 && bs == 5) {
            // [index*scale + disp32]，无 base
            base = -1;
            disp = (int32_t)rd32(s, ok);
            hasDisp = true;
        } else {
            base = bs | (s.rexB ? 8 : 0);
            if (m.mod == 1) { disp = (int8_t)rd8(s, ok); hasDisp = true; }
            else if (m.mod == 2) { disp = (int32_t)rd32(s, ok); hasDisp = true; }
        }
    } else {
        if (m.mod == 0 && m.rm == 5) {
            // RIP 相对
            ripRel = true;
            disp = (int32_t)rd32(s, ok);
            hasDisp = true;
        } else {
            base = m.rm | (s.rexB ? 8 : 0);
            if (m.mod == 1) { disp = (int8_t)rd8(s, ok); hasDisp = true; }
            else if (m.mod == 2) { disp = (int32_t)rd32(s, ok); hasDisp = true; }
        }
    }

    std::ostringstream os;
    if (ripRel) {
        char buf[32];
        snprintf(buf, sizeof(buf), "[rip%+d]", (int)disp);
        os << buf;
        return os.str();
    }
    os << "[";
    if (base >= 0) os << gpr64(base);
    if (index >= 0) {
        if (base >= 0) os << "+";
        os << gpr64(index);
        if (scale > 1) os << "*" << scale;
    }
    if (hasDisp) {
        char buf[32];
        if (disp < 0) snprintf(buf, sizeof(buf), "-0x%llx", (unsigned long long)(-disp));
        else snprintf(buf, sizeof(buf), "+0x%llx", (unsigned long long)disp);
        os << buf;
    }
    os << "]";
    return os.str();
}

// 返回解码后的指令长度；out 填助记符；hexOut 填字节。
inline int disassembleOne(const uint8_t* code, size_t size, std::string& out, std::string& hexOut) {
    State s{ code, size };
    bool ok = true;
    const uint8_t* start = code;

    // 前缀
    for (;;) {
        if (s.size < 1) break;
        uint8_t b = *s.p;
        if (b == 0x66) { s.p66 = true; s.p++; s.size--; }
        else if (b == 0x67) { s.p++; s.size--; }
        else if (b == 0xF2) { s.pf2 = true; s.p++; s.size--; }
        else if (b == 0xF3) { s.pf3 = true; s.p++; s.size--; }
        else if ((b & 0xF0) == 0x40) { // REX
            s.hasRex = true;
            s.rexW = (b & 0x08) != 0;
            s.rexR = (b & 0x04) != 0;
            s.rexX = (b & 0x02) != 0;
            s.rexB = (b & 0x01) != 0;
            s.p++; s.size--;
        }
        else break;
    }

    uint8_t op = rd8(s, ok);
    std::ostringstream o;

    auto gprReg = [&](int r) { return s.rexW ? gpr64(r) : gpr32(r); };

    // ================= 两字节 0F =================
    if (op == 0x0F) {
        uint8_t op2 = rd8(s, ok);
        if (op2 == 0x3A && s.p66) { // roundsd (66 0F 3A 0B)
            uint8_t op3 = rd8(s, ok);
            if (op3 == 0x0B) {
                ModRM m = readModRM(s, ok);
                std::string dst = xmm(m.reg | (s.rexR ? 8 : 0));
                std::string src = formatRm(s, m, ok, true);
                uint8_t imm = rd8(s, ok);
                o << "roundsd " << dst << ", " << src << ", " << (int)imm;
            } else { o << "db 0x" << std::hex << (int)op3 << std::dec; }
        }
        else if (op2 >= 0x80 && op2 <= 0x8F) { // jcc rel32
            uint32_t rel = rd32(s, ok);
            o << "j" << condName(op2 & 0xF) << " " << "+0x" << std::hex << rel << std::dec;
        }
        else if (op2 >= 0x90 && op2 <= 0x9F) { // setcc r/m8
            ModRM m = readModRM(s, ok);
            o << "set" << condName(op2 & 0xF) << " " << formatRm(s, m, ok, false);
        }
        else if (op2 == 0xAF) { // imul r, r/m
            ModRM m = readModRM(s, ok);
            o << "imul " << gprReg(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, false);
        }
        else if (op2 == 0xB6) { // movzx r, r/m8
            ModRM m = readModRM(s, ok);
            o << "movzx " << gprReg(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, false);
        }
        else if (op2 == 0xBE || op2 == 0xBF) { // movsx r, r/m
            ModRM m = readModRM(s, ok);
            o << "movsx " << gprReg(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, false);
        }
        else if (op2 == 0x1F) { // nop
            ModRM m = readModRM(s, ok);
            if (m.mod == 3) { /* reg form */ }
            else formatRm(s, m, ok, false);
            o << "nop";
        }
        else if (op2 == 0x28) { // movapd (66)
            ModRM m = readModRM(s, ok);
            o << "movapd " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0x29) { // movapd (66)
            ModRM m = readModRM(s, ok);
            o << "movapd " << formatRm(s, m, ok, true) << ", " << xmm(m.reg | (s.rexR ? 8 : 0));
        }
        else if (op2 == 0x54) { // andpd (66)
            ModRM m = readModRM(s, ok);
            o << "andpd " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0x57) { // xorpd (66)
            ModRM m = readModRM(s, ok);
            o << "xorpd " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0xEF) { // pxor (66)
            ModRM m = readModRM(s, ok);
            o << "pxor " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0xDB) { // pand (66)
            ModRM m = readModRM(s, ok);
            o << "pand " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0xEB) { // por (66)
            ModRM m = readModRM(s, ok);
            o << "por " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0x2E) { // ucomisd (66)
            ModRM m = readModRM(s, ok);
            o << "ucomisd " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0x6E) { // movd/movq xmm, r/m (66)
            ModRM m = readModRM(s, ok);
            const char* mnem = s.rexW ? "movq" : "movd";
            o << mnem << " " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, false);
        }
        else if (op2 == 0x7E) { // movd/movq r/m, xmm (66)
            ModRM m = readModRM(s, ok);
            const char* mnem = s.rexW ? "movq" : "movd";
            o << mnem << " " << formatRm(s, m, ok, false) << ", " << xmm(m.reg | (s.rexR ? 8 : 0));
        }
        else if (op2 == 0x10) { // movsd (F2) / movss (F3)
            ModRM m = readModRM(s, ok);
            const char* mnem = s.pf3 ? "movss" : "movsd";
            o << mnem << " " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0x11) { // movsd (F2) / movss (F3)
            ModRM m = readModRM(s, ok);
            const char* mnem = s.pf3 ? "movss" : "movsd";
            o << mnem << " " << formatRm(s, m, ok, true) << ", " << xmm(m.reg | (s.rexR ? 8 : 0));
        }
        else if (op2 == 0x58) { // addsd (F2)
            ModRM m = readModRM(s, ok);
            o << "addsd " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0x5C) { // subsd (F2)
            ModRM m = readModRM(s, ok);
            o << "subsd " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0x59) { // mulsd (F2)
            ModRM m = readModRM(s, ok);
            o << "mulsd " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0x5E) { // divsd (F2)
            ModRM m = readModRM(s, ok);
            o << "divsd " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0x51) { // sqrtsd (F2)
            ModRM m = readModRM(s, ok);
            o << "sqrtsd " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0x2A) { // cvtsi2sd (F2)
            ModRM m = readModRM(s, ok);
            o << "cvtsi2sd " << xmm(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, false);
        }
        else if (op2 == 0x2C) { // cvttsd2si (F2)
            ModRM m = readModRM(s, ok);
            o << "cvttsd2si " << gprReg(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, true);
        }
        else if (op2 == 0xD6) { // movq r/m, xmm (66)
            ModRM m = readModRM(s, ok);
            o << "movq " << formatRm(s, m, ok, false) << ", " << xmm(m.reg | (s.rexR ? 8 : 0));
        }
        else {
            o << "db 0x0F 0x" << std::hex << (int)op2 << std::dec;
        }
    }
    // ================= 单字节 =================
    else if (op >= 0x50 && op <= 0x57) { // push reg
        o << "push " << gpr64(op & 7 | (s.rexB ? 8 : 0));
    }
    else if (op >= 0x58 && op <= 0x5F) { // pop reg
        o << "pop " << gpr64(op & 7 | (s.rexB ? 8 : 0));
    }
    else if (op == 0x68) { o << "push 0x" << std::hex << rd32(s, ok) << std::dec; }
    else if (op == 0x6A) { o << "push " << (int)(int8_t)rd8(s, ok); }
    else if (op == 0x63) { // movsxd
        ModRM m = readModRM(s, ok);
        o << "movsxd " << gpr64(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, false);
    }
    else if (op == 0x69 || op == 0x6B) { // imul r, r/m, imm
        ModRM m = readModRM(s, ok);
        std::string src = formatRm(s, m, ok, false);
        if (op == 0x69) { o << "imul " << gprReg(m.reg | (s.rexR ? 8 : 0)) << ", " << src << ", 0x" << std::hex << rd32(s, ok) << std::dec; }
        else { o << "imul " << gprReg(m.reg | (s.rexR ? 8 : 0)) << ", " << src << ", " << (int)(int8_t)rd8(s, ok); }
    }
    else if (((op & 7) == 1 || (op & 7) == 3) && op >= 0x01 && op <= 0x3B) { // ALU reg-reg
        static const char* an[] = {"add","or","adc","sbb","and","sub","xor","cmp"};
        int opIdx = (op >> 3) & 7;
        ModRM m = readModRM(s, ok);
        const char* mn = (opIdx < 8) ? an[opIdx] : "?";
        if (op & 2) { // op r, r/m
            o << mn << " " << gprReg(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, false);
        } else { // op r/m, r
            o << mn << " " << formatRm(s, m, ok, false) << ", " << gprReg(m.reg | (s.rexR ? 8 : 0));
        }
    }
    else if (op == 0x81 || op == 0x83) { // group1 r/m, imm
        ModRM m = readModRM(s, ok);
        static const char* gn[] = {"add","or","adc","sbb","and","sub","xor","cmp"};
        const char* mn = (m.reg < 8) ? gn[m.reg] : "?";
        std::string rm = formatRm(s, m, ok, false);
        if (op == 0x81) { o << mn << " " << rm << ", 0x" << std::hex << rd32(s, ok) << std::dec; }
        else { o << mn << " " << rm << ", " << (int)(int8_t)rd8(s, ok); }
    }
    else if (op == 0x85) { // test r/m, r
        ModRM m = readModRM(s, ok);
        o << "test " << formatRm(s, m, ok, false) << ", " << gprReg(m.reg | (s.rexR ? 8 : 0));
    }
    else if (op == 0x89) { // mov r/m, r
        ModRM m = readModRM(s, ok);
        o << (s.rexW ? "movq" : "mov") << " " << formatRm(s, m, ok, false) << ", " << gprReg(m.reg | (s.rexR ? 8 : 0));
    }
    else if (op == 0x8B) { // mov r, r/m
        ModRM m = readModRM(s, ok);
        o << (s.rexW ? "movq" : "mov") << " " << gprReg(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, false);
    }
    else if (op == 0x8D) { // lea
        ModRM m = readModRM(s, ok);
        o << "lea " << gprReg(m.reg | (s.rexR ? 8 : 0)) << ", " << formatRm(s, m, ok, false);
    }
    else if (op == 0x90) { o << "nop"; }
    else if (op == 0x99) { o << (s.rexW ? "cqo" : "cdq"); }
    else if (op >= 0xB8 && op <= 0xBF) { // mov r, imm
        if (s.rexW) {
            uint64_t imm = rd64(s, ok);
            o << "movabs " << gpr64(op & 7 | (s.rexB ? 8 : 0)) << ", 0x" << std::hex << imm << std::dec;
        } else { o << "mov " << gpr32(op & 7 | (s.rexB ? 8 : 0)) << ", 0x" << std::hex << rd32(s, ok) << std::dec; }
    }
    else if (op == 0xC1 || op == 0xD1) { // group2 shift
        ModRM m = readModRM(s, ok);
        static const char* gn[] = {"rol","ror","rcl","rcr","shl","shr","?","sar"};
        const char* mn = (m.reg < 8) ? gn[m.reg] : "?";
        std::string rm = formatRm(s, m, ok, false);
        if (op == 0xC1) { o << mn << " " << rm << ", " << (int)rd8(s, ok); }
        else { o << mn << " " << rm << ", 1"; }
    }
    else if (op == 0xC7) { // mov r/m, imm32
        ModRM m = readModRM(s, ok);
        o << (s.rexW ? "movq" : "mov") << " " << formatRm(s, m, ok, false) << ", 0x" << std::hex << rd32(s, ok) << std::dec;
    }
    else if (op == 0xC3) { o << "ret"; }
    else if (op == 0xE8) { uint32_t rel = rd32(s, ok); o << "call +0x" << std::hex << rel << std::dec; }
    else if (op == 0xE9) { uint32_t rel = rd32(s, ok); o << "jmp +0x" << std::hex << rel << std::dec; }
    else if (op == 0xEB) { int8_t rel = (int8_t)rd8(s, ok); o << "jmp " << (int)rel; }
    else if (op == 0xF7) { // group3
        ModRM m = readModRM(s, ok);
        static const char* gn[] = {"test","?","not","neg","mul","imul","div","idiv"};
        const char* mn = (m.reg < 8) ? gn[m.reg] : "?";
        std::string rm = formatRm(s, m, ok, false);
        if (m.reg == 0) { o << "test " << rm << ", 0x" << std::hex << rd32(s, ok) << std::dec; }
        else { o << mn << " " << rm; }
    }
    else if (op == 0xFF) { // group5
        ModRM m = readModRM(s, ok);
        std::string rm = formatRm(s, m, ok, false);
        if (m.reg == 2) o << "call " << rm;
        else if (m.reg == 4) o << "jmp " << rm;
        else if (m.reg == 6) o << "push " << rm;
        else o << "group5/" << m.reg << " " << rm;
    }
    else {
        o << "db 0x" << std::hex << (int)op << std::dec;
    }

    if (!ok) {
        out = "db (truncated)";
        hexOut.clear();
        return (int)(s.p - start);
    }
    out = o.str();

    // 生成 hex
    std::ostringstream h;
    int n = (int)(s.p - start);
    for (int i = 0; i < n; ++i) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X ", start[i]);
        h << buf;
    }
    hexOut = h.str();
    return n;
}

inline void disassemble(const uint8_t* code, size_t size, std::ostream& os, uint64_t base = 0) {
    (void)base;
    size_t off = 0;
    while (off < size) {
        std::string out, hex;
        int n = disassembleOne(code + off, size - off, out, hex);
        if (n <= 0) {
            // 单字节 db 兜底
            char buf[32];
            snprintf(buf, sizeof(buf), "%04zx: %02X                    db 0x%02X\n", off, code[off], code[off]);
            os << buf;
            off += 1;
            continue;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%04zx: ", off);
        os << buf;
        os << std::left << std::setw(24) << hex << " " << out << "\n";
        off += n;
    }
}

} // namespace disasm_detail

using disasm_detail::disassemble;

} // namespace jit
} // namespace jc

#endif // JC2_JIT_DISASSEMBLER_H
