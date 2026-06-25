#include "VM.h"
#include <stdexcept>
#include <iostream>

namespace jc {
namespace regvm {

VM::VM() {
    registers = new Value[MAX_REGISTERS];
    frames = new CallFrame[MAX_FRAMES];
}

VM::~VM() {
    delete[] registers;
    delete[] frames;
}

Value VM::execute(const Chunk& mainChunk) {
    CallFrame mainFrame;
    mainFrame.chunk = &mainChunk;
    mainFrame.ip = 0;
    mainFrame.registerBase = 0;
    
    frames[0] = mainFrame;
    frameCount = 1;

    return run();
}

Value VM::run() {
    CallFrame* frame = &frames[frameCount - 1];
    const Chunk* chunk = frame->chunk;
    const Instruction* code = chunk->code.data();
    
    // 提取 EXTRAARG 扩展操作数 (24-bit)
    auto fetchExtra = [&]() -> int {
        Instruction ext = code[frame->ip++];
        return GET_Ax(ext);
    };

    // 获取物理寄存器或溢出槽 (Unified Address Space)
    auto getReg = [&](int idx) -> Value& {
        return registers[frame->registerBase + idx];
    };

    // K-Bit 机制：解析寄存器或常量池索引
    auto getRK = [&](int rk) -> Value {
        if (ISK(rk)) {
            int idx = INDEXK(rk);
            if (idx == ESCAPE_KBIT_CONST) idx = fetchExtra();
            return chunk->constants[idx];
        } else {
            if (rk == ESCAPE_KBIT_REG) rk = fetchExtra();
            return getReg(rk);
        }
    };

    while (true) {
        Instruction inst = code[frame->ip++];
        OpCode op = GET_OPCODE(inst);
        
        int a = GET_A(inst);
        int b = GET_B(inst);
        int c = GET_C(inst);
        int bx = GET_Bx(inst);
        int sbx = GET_sBx(inst);
        int ax = GET_Ax(inst);
        int sax = GET_sAx(inst);

        switch (op) {
            case OpCode::MOVE: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                getReg(a) = getReg(b);
                break;
            }
            case OpCode::LOADK: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                if (bx == ESCAPE_NORMAL_16) bx = fetchExtra();
                getReg(a) = chunk->constants[bx];
                break;
            }
            case OpCode::ADD: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value vb = getRK(b);
                Value vc = getRK(c);
                getReg(a) = vb + vc;
                break;
            }
            case OpCode::RETURN: {
                if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                Value res = getReg(a);
                frameCount--;
                if (frameCount == 0) {
                    return res;
                }
                // 恢复调用方帧状态
                frame = &frames[frameCount - 1];
                chunk = frame->chunk;
                code = chunk->code.data();
                break;
            }
            default:
                throw std::runtime_error("RegVM Error: Unimplemented opcode " + std::to_string(static_cast<int>(op)));
        }
    }
}

} // namespace regvm
} // namespace jc
