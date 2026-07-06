#include "Emitter.h"
#include <stdexcept>
#include <unordered_map>

namespace jc {
namespace regvm {

struct EncodedInst {
    IRNode* node = nullptr;
    std::vector<uint32_t> words;
    
    bool isJump = false;
    OpCode jumpOp = OpCode::MOVE;
    int jumpA = 0;
    BasicBlock* jumpTarget = nullptr;
    bool isTrampoline = false;
    int offset = 0;
};

enum class OpType {
    NORMAL,
    KBIT_REG,
    KBIT_KST
};

static void encodeOperand(int val, OpType type, int& enc, bool& ext) {
    if (val > 0xFFFFFF) throw std::runtime_error("Emitter Error: Operand exceeds 24-bit limit.");
    if (type == OpType::NORMAL) {
        if (val >= 255) { enc = 255; ext = true; }
        else { enc = val; ext = false; }
    } else if (type == OpType::KBIT_REG) {
        if (val >= 127) { enc = 127; ext = true; }
        else { enc = val; ext = false; }
    } else if (type == OpType::KBIT_KST) {
        if (val >= 127) { enc = 255; ext = true; } // 255 is 0xFF, which is 127 | 0x80
        else { enc = val | 0x80; ext = false; }
    }
}

static std::vector<uint32_t> buildInstABC(OpCode op, int a, int b, int c, OpType bType = OpType::NORMAL, OpType cType = OpType::NORMAL) {
    int encA, encB, encC;
    bool extA, extB, extC;
    encodeOperand(a, OpType::NORMAL, encA, extA);
    encodeOperand(b, bType, encB, extB);
    encodeOperand(c, cType, encC, extC);

    std::vector<uint32_t> words;
    words.push_back(CREATE_ABC(op, encA, encB, encC));
    if (extA) words.push_back(CREATE_Ax(OpCode::EXTRAARG, a));
    if (extB) words.push_back(CREATE_Ax(OpCode::EXTRAARG, b));
    if (extC) words.push_back(CREATE_Ax(OpCode::EXTRAARG, c));
    return words;
}

static std::vector<uint32_t> buildInstAB(OpCode op, int a, int b, OpType bType = OpType::NORMAL) {
    return buildInstABC(op, a, b, 0, bType, OpType::NORMAL);
}

static std::vector<uint32_t> buildInstA(OpCode op, int a) {
    return buildInstABC(op, a, 0, 0, OpType::NORMAL, OpType::NORMAL);
}

static std::vector<uint32_t> buildInstABx(OpCode op, int a, int bx) {
    int encA; bool extA;
    encodeOperand(a, OpType::NORMAL, encA, extA);
    
    int encBx = bx; bool extBx = false;
    if (bx >= 0xFFFF) { encBx = 0xFFFF; extBx = true; }
    if (bx > 0xFFFFFF) throw std::runtime_error("Emitter Error: Bx exceeds 24-bit limit.");

    std::vector<uint32_t> words;
    words.push_back(CREATE_ABx(op, encA, encBx));
    if (extA) words.push_back(CREATE_Ax(OpCode::EXTRAARG, a));
    if (extBx) words.push_back(CREATE_Ax(OpCode::EXTRAARG, bx));
    return words;
}

static std::vector<uint32_t> buildInstAsBx(OpCode op, int a, int sbx) {
    int encA; bool extA;
    encodeOperand(a, OpType::NORMAL, encA, extA);

    if (sbx < -32767 || sbx > 32767) throw std::runtime_error("Emitter Error: sBx out of bounds.");
    int encBx = sbx + 0x7FFF;

    std::vector<uint32_t> words;
    words.push_back(CREATE_ABx(op, encA, encBx));
    if (extA) words.push_back(CREATE_Ax(OpCode::EXTRAARG, a));
    return words;
}

static int ensureReg(IRNode* in, std::vector<uint32_t>& preWords, Chunk& chunk, int scratchReg) {
    if (in->op == IROp::Constant && in->physicalReg == -1) {
        int idx = chunk.addConstant(in->constVal);
        auto loadk = buildInstABx(OpCode::LOADK, scratchReg, idx);
        preWords.insert(preWords.end(), loadk.begin(), loadk.end());
        return scratchReg;
    }
    return in->physicalReg;
}

static int packArgs(std::vector<uint32_t>& words, const std::vector<IRNode*>& args, Chunk& chunk, int dynamicSpillBase) {
    int base = dynamicSpillBase;
    for (size_t i = 0; i < args.size(); ++i) {
        int reg = ensureReg(args[i], words, chunk, 124);
        auto move = buildInstAB(OpCode::MOVE, base + static_cast<int>(i), reg);
        words.insert(words.end(), move.begin(), move.end());
    }
    return base;
}

int Emitter::emit(IRGraph* graph, Chunk& chunk) {
    for (auto& cb : graph->postAllocCallbacks) {
        cb();
    }

    int maxReg = 127;
    for (auto& nodePtr : graph->getNodes()) {
        if (nodePtr->physicalReg > maxReg) maxReg = nodePtr->physicalReg;
    }
    int dynamicSpillBase = maxReg + 1;

    std::vector<EncodedInst> insts;
    std::unordered_map<BasicBlock*, int> blockToInstIdx;

    // 1. Lowering: IRNode -> EncodedInst
    for (size_t bbIdx = 0; bbIdx < graph->blocks.size(); ++bbIdx) {
        BasicBlock* bb = graph->blocks[bbIdx].get();
        BasicBlock* nextBb = (bbIdx + 1 < graph->blocks.size()) ? graph->blocks[bbIdx + 1].get() : nullptr;
        blockToInstIdx[bb] = static_cast<int>(insts.size());
        
        EncodedInst labelInst;
        labelInst.node = nullptr;
        insts.push_back(labelInst); // Dummy label

        for (IRNode* node : bb->instructions) {
            EncodedInst inst;
            inst.node = node;
            
            if (node->op == IROp::Constant && node->physicalReg != -1) {
                int idx = chunk.addConstant(node->constVal);
                auto w = buildInstABx(OpCode::LOADK, node->physicalReg, idx);
                inst.words.insert(inst.words.end(), w.begin(), w.end());
            } else {
                auto buildBinary = [&](OpCode op) {
                    int a = node->physicalReg;
                    int b = 0, c = 0;
                    OpType bType = OpType::KBIT_REG;
                    OpType cType = OpType::KBIT_REG;
                    if (node->dataInputs[0]->op == IROp::Constant && node->dataInputs[0]->physicalReg == -1) {
                        b = chunk.addConstant(node->dataInputs[0]->constVal); bType = OpType::KBIT_KST;
                    } else b = node->dataInputs[0]->physicalReg;
                    if (node->dataInputs[1]->op == IROp::Constant && node->dataInputs[1]->physicalReg == -1) {
                        c = chunk.addConstant(node->dataInputs[1]->constVal); cType = OpType::KBIT_KST;
                    } else c = node->dataInputs[1]->physicalReg;
                    return buildInstABC(op, a, b, c, bType, cType);
                };

                switch (node->op) {
                    case IROp::Add: inst.words = buildBinary(OpCode::ADD); break;
                    case IROp::Sub: inst.words = buildBinary(OpCode::SUB); break;
                    case IROp::Mul: inst.words = buildBinary(OpCode::MUL); break;
                    case IROp::Div: inst.words = buildBinary(OpCode::DIV); break;
                    case IROp::Mod: inst.words = buildBinary(OpCode::MOD); break;
                    case IROp::Pow: inst.words = buildBinary(OpCode::POW); break;
                    case IROp::LeftDivide: inst.words = buildBinary(OpCode::LDIV); break;
                    case IROp::Eq: inst.words = buildBinary(OpCode::EQ); break;
                    case IROp::Neq: inst.words = buildBinary(OpCode::NEQ); break;
                    case IROp::Lt: inst.words = buildBinary(OpCode::LT); break;
                    case IROp::Le: inst.words = buildBinary(OpCode::LE); break;
                    case IROp::Gt: inst.words = buildBinary(OpCode::GT); break;
                    case IROp::Ge: inst.words = buildBinary(OpCode::GE); break;
                    case IROp::BitAnd: inst.words = buildBinary(OpCode::BAND); break;
                    case IROp::BitOr: inst.words = buildBinary(OpCode::BOR); break;
                    case IROp::BitXor: inst.words = buildBinary(OpCode::BXOR); break;
                    case IROp::Shl: inst.words = buildBinary(OpCode::SHL); break;
                    case IROp::Shr: inst.words = buildBinary(OpCode::SHR); break;
                    
                    case IROp::Neg: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstAB(OpCode::UNM, node->physicalReg, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Not: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstAB(OpCode::NOT, node->physicalReg, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::BitNot: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstAB(OpCode::BNOT, node->physicalReg, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::ToBool: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstAB(OpCode::TO_BOOL, node->physicalReg, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Move: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        if (node->physicalReg != b) {
                            auto w = buildInstAB(OpCode::MOVE, node->physicalReg, b);
                            inst.words.insert(inst.words.end(), w.begin(), w.end());
                        }
                        break;
                    }
                    case IROp::UpdateCaptured: {
                        int dst = node->dataInputs[0]->physicalReg;
                        int src = ensureReg(node->dataInputs[1], inst.words, chunk, 124);
                        if (dst != src) {
                            auto w = buildInstAB(OpCode::MOVE, dst, src);
                            inst.words.insert(inst.words.end(), w.begin(), w.end());
                        }
                        break;
                    }
                    case IROp::GetGlobal: {
                        uint32_t icIdx = chunk.addInlineCache(chunk.addConstant(Value(node->name)));
                        auto w = buildInstABx(OpCode::GET_GLOBAL, node->physicalReg, icIdx);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::SetGlobal: {
                        uint32_t icIdx = chunk.addInlineCache(chunk.addConstant(Value(node->name)));
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstABx(OpCode::SET_GLOBAL, a, icIdx);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::SetGlobalRef: {
                        uint32_t icIdx = chunk.addInlineCache(chunk.addConstant(Value(node->name)));
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstABx(OpCode::SET_GLOBAL_REF, a, icIdx);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::DefineConstGlobal: {
                        uint32_t icIdx = chunk.addInlineCache(chunk.addConstant(Value(node->name)));
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstABx(OpCode::DEFINE_CONST_GLOBAL, a, icIdx);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::DeleteGlobal: {
                        uint32_t nameIdx = chunk.addConstant(Value(node->name));
                        auto w = buildInstABx(OpCode::DELETE_GLOBAL, 0, nameIdx);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::IsUninit: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstAB(OpCode::IS_UNINIT, node->physicalReg, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Closure: {
                        uint32_t fnIdx = chunk.addConstant(Value(std::stod(node->name)));
                        auto w = buildInstABx(OpCode::CLOSURE, node->physicalReg, fnIdx);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::GetUpvalue: {
                        auto w = buildInstAB(OpCode::GET_UPVAL, node->physicalReg, node->payload1);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::SetUpvalue: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstAB(OpCode::SET_UPVAL, a, node->payload1);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::GetRefParam: {
                        auto w = buildInstABx(OpCode::GET_REF_PARAM, node->physicalReg, node->payload1);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::SetRefParam: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstABx(OpCode::SET_REF_PARAM, a, node->payload1);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::PassRefs: {
                        const auto& irSig = graph->callSignatures[node->payload1];
                        std::vector<ArgSource> chunkRefs;
                        for (const auto& ref : irSig.refs) {
                            ArgSource src;
                            src.argIndex = ref.argIndex;
                            src.sourceType = ref.sourceType;
                            if (ref.sourceType == 1) {
                                src.sourceRef = chunk.addConstant(Value(ref.name));
                            } else if (ref.sourceType == 2) {
                                src.sourceRef = ref.localNode->getResolved()->physicalReg;
                            } else if (ref.sourceType == 4) {
                                src.sourceRef = ref.localNode->getResolved()->payload1;
                            } else if (ref.sourceType == 3) {
                                src.sourceRef = ref.upvalIdx;
                            }
                            chunkRefs.push_back(src);
                        }
                        uint32_t sigIdx = chunk.addCallSignature(chunkRefs);
                        auto w = buildInstABx(OpCode::PASS_REFS, 0, sigIdx);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Call:
                    case IROp::TailCall: {
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase);
                        auto call = buildInstAB(node->op == IROp::Call ? OpCode::CALL : OpCode::TAIL_CALL, spillBase, node->payload1);
                        inst.words.insert(inst.words.end(), call.begin(), call.end());
                        if (node->physicalReg != spillBase) {
                            auto loadRes = buildInstAB(OpCode::MOVE, node->physicalReg, spillBase);
                            inst.words.insert(inst.words.end(), loadRes.begin(), loadRes.end());
                        }
                        break;
                    }
                    case IROp::Invoke:
                    case IROp::TailInvoke: {
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase);
                        uint32_t icIdx = chunk.addInlineCache(chunk.addConstant(Value(node->name)));
                        auto inv = buildInstABC(node->op == IROp::Invoke ? OpCode::INVOKE : OpCode::TAIL_INVOKE, spillBase, node->payload1, icIdx);
                        inst.words.insert(inst.words.end(), inv.begin(), inv.end());
                        if (node->physicalReg != spillBase) {
                            auto loadRes = buildInstAB(OpCode::MOVE, node->physicalReg, spillBase);
                            inst.words.insert(inst.words.end(), loadRes.begin(), loadRes.end());
                        }
                        break;
                    }
                    case IROp::InvokeFallback:
                    case IROp::TailInvokeFallback: {
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase);
                        uint32_t icIdx = chunk.addInlineCache(chunk.addConstant(Value(node->name)));
                        auto inv = buildInstABC(node->op == IROp::InvokeFallback ? OpCode::INVOKE_FALLBACK : OpCode::TAIL_INVOKE_FALLBACK, spillBase, node->payload1, icIdx);
                        inst.words.insert(inst.words.end(), inv.begin(), inv.end());
                        if (node->physicalReg != spillBase) {
                            auto loadRes = buildInstAB(OpCode::MOVE, node->physicalReg, spillBase);
                            inst.words.insert(inst.words.end(), loadRes.begin(), loadRes.end());
                        }
                        break;
                    }
                    case IROp::SuperInvoke:
                    case IROp::TailSuperInvoke: {
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase);
                        uint32_t nameIdx = chunk.addConstant(Value(node->name));
                        auto inv = buildInstABC(node->op == IROp::SuperInvoke ? OpCode::SUPER_INVOKE : OpCode::TAIL_SUPER_INVOKE, spillBase, node->payload1, nameIdx);
                        inst.words.insert(inst.words.end(), inv.begin(), inv.end());
                        if (node->physicalReg != spillBase) {
                            auto loadRes = buildInstAB(OpCode::MOVE, node->physicalReg, spillBase);
                            inst.words.insert(inst.words.end(), loadRes.begin(), loadRes.end());
                        }
                        break;
                    }
                    case IROp::BuildList:
                    case IROp::BuildDict:
                    case IROp::BuildSet:
                    case IROp::ConcatStrings: {
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase);
                        OpCode op = OpCode::BUILD_LIST;
                        if (node->op == IROp::BuildDict) op = OpCode::BUILD_DICT;
                        else if (node->op == IROp::BuildSet) op = OpCode::BUILD_SET;
                        else if (node->op == IROp::ConcatStrings) op = OpCode::CONCAT_STRINGS;
                        auto build = buildInstABC(op, node->physicalReg, spillBase, node->payload1);
                        inst.words.insert(inst.words.end(), build.begin(), build.end());
                        break;
                    }
                    case IROp::BuildMatrix: {
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase);
                        std::vector<uint16_t> rowCols(node->payload1, static_cast<uint16_t>(node->payload2));
                        uint32_t shapeIdx = chunk.addMatrixShape(static_cast<uint16_t>(node->payload1), rowCols);
                        auto build = buildInstABC(OpCode::BUILD_MATRIX, node->physicalReg, spillBase, shapeIdx);
                        inst.words.insert(inst.words.end(), build.begin(), build.end());
                        break;
                    }
                    case IROp::DictRest: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int c = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        auto w = buildInstABC(OpCode::DICT_REST, node->physicalReg, b, c);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::BuildNamespace: {
                        std::vector<IRNode*> argsToPack;
                        for (uint32_t i = 0; i < node->payload1 * 3; ++i) {
                            argsToPack.push_back(node->dataInputs[i]);
                        }
                        int spillBase = packArgs(inst.words, argsToPack, chunk, dynamicSpillBase + 1) - 1;
                        uint32_t nameIdx = chunk.addConstant(Value(node->name));
                        auto w = buildInstABC(OpCode::BUILD_NAMESPACE, spillBase, nameIdx, node->payload1);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        if (node->physicalReg != spillBase) {
                            auto loadRes = buildInstAB(OpCode::MOVE, node->physicalReg, spillBase);
                            inst.words.insert(inst.words.end(), loadRes.begin(), loadRes.end());
                        }
                        break;
                    }
                    case IROp::ListInit: {
                        auto w = buildInstA(OpCode::LIST_INIT, node->physicalReg);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::ListAppend: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int b = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        auto w = buildInstAB(OpCode::LIST_APPEND, a, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::ListCompEnd: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstA(OpCode::LIST_COMP_END, a);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        if (node->physicalReg != a) {
                            auto loadRes = buildInstAB(OpCode::MOVE, node->physicalReg, a);
                            inst.words.insert(inst.words.end(), loadRes.begin(), loadRes.end());
                        }
                        break;
                    }
                    case IROp::IndexGet: {
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase);
                        int cVal = node->payload1;
                        if (node->payload2) cVal |= 0x80; // 使用最高位标记 noThrow
                        auto get = buildInstABC(OpCode::INDEX_GET, node->physicalReg, spillBase, cVal);
                        inst.words.insert(inst.words.end(), get.begin(), get.end());
                        break;
                    }
                    case IROp::SliceGet: {
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase);
                        auto get = buildInstABC(OpCode::SLICE_GET, node->physicalReg, spillBase, node->payload1);
                        inst.words.insert(inst.words.end(), get.begin(), get.end());
                        break;
                    }
                    case IROp::IndexSet:
                    case IROp::SliceSet: {
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase);
                        auto set = buildInstABC(node->op == IROp::IndexSet ? OpCode::INDEX_SET : OpCode::SLICE_SET, spillBase, 0, static_cast<int>(node->payload1));
                        inst.words.insert(inst.words.end(), set.begin(), set.end());
                        if (node->physicalReg != spillBase) {
                            auto loadRes = buildInstAB(OpCode::MOVE, node->physicalReg, spillBase);
                            inst.words.insert(inst.words.end(), loadRes.begin(), loadRes.end());
                        }
                        break;
                    }
                    case IROp::IterInit: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstABC(OpCode::ITER_INIT, node->physicalReg, b, node->payload1);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::IterNext: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstAB(OpCode::ITER_NEXT, node->physicalReg, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::In: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int c = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        auto w = buildInstABC(OpCode::IN, node->physicalReg, b, c);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Stringify: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstAB(OpCode::STRINGIFY, node->physicalReg, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::FormatString: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int c = chunk.addConstant(node->dataInputs[1]->constVal);
                        auto w = buildInstABC(OpCode::FORMAT_STRING, node->physicalReg, b, c, OpType::NORMAL, OpType::NORMAL);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Class: {
                        uint32_t nameIdx = chunk.addConstant(Value(node->name));
                        auto w = buildInstABx(OpCode::CLASS, node->physicalReg, nameIdx);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Method: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int c = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        uint32_t nameIdx = chunk.addConstant(Value(node->name));
                        auto w = buildInstABC(OpCode::METHOD, a, nameIdx, c, OpType::NORMAL, OpType::NORMAL);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Inherit: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int b = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        auto w = buildInstAB(OpCode::INHERIT, a, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::GetProperty: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        uint32_t icIdx = chunk.addInlineCache(chunk.addConstant(Value(node->name)));
                        auto w = buildInstABC(OpCode::GET_PROP, node->physicalReg, b, icIdx, OpType::NORMAL, OpType::NORMAL);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::TryGetProperty: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        uint32_t icIdx = chunk.addInlineCache(chunk.addConstant(Value(node->name)));
                        auto w = buildInstABC(OpCode::TRY_GET_PROP, node->physicalReg, b, icIdx, OpType::NORMAL, OpType::NORMAL);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::SetProperty: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int c = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        uint32_t icIdx = chunk.addInlineCache(chunk.addConstant(Value(node->name)));
                        auto w = buildInstABC(OpCode::SET_PROP, a, icIdx, c, OpType::NORMAL, OpType::NORMAL);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::GetSuper: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        uint32_t nameIdx = chunk.addConstant(Value(node->name));
                        auto w = buildInstABC(OpCode::GET_SUPER, node->physicalReg, b, nameIdx, OpType::NORMAL, OpType::NORMAL);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::GetSelf: {
                        auto w = buildInstA(OpCode::GET_SELF, node->physicalReg);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Import: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstAB(OpCode::IMPORT, node->physicalReg, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::AssertParamType: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstABC(OpCode::ASSERT_PARAM_TYPE, a, node->payload1, node->payload2, OpType::NORMAL, OpType::NORMAL);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::AssertReturnType: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstAB(OpCode::ASSERT_RETURN_TYPE, a, node->payload1, OpType::NORMAL);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::MatchType: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        uint32_t icIdx = chunk.addInlineCache(chunk.addConstant(Value(node->name)));
                        auto w = buildInstABC(OpCode::MATCH_TYPE, node->physicalReg, b, icIdx, OpType::NORMAL, OpType::NORMAL);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::MatchShape: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        uint32_t shapeIdx = chunk.addShapePattern(node->payload1, node->payload2, node->payload3, node->payload4, static_cast<uint8_t>(node->payload5));
                        auto w = buildInstABC(OpCode::MATCH_SHAPE, node->physicalReg, b, shapeIdx, OpType::NORMAL, OpType::NORMAL);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Return: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto ret = buildInstA(OpCode::RETURN, a);
                        inst.words.insert(inst.words.end(), ret.begin(), ret.end());
                        break;
                    }
                    case IROp::Throw: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto thr = buildInstA(OpCode::THROW, a);
                        inst.words.insert(inst.words.end(), thr.begin(), thr.end());
                        break;
                    }
                    case IROp::TryEnd: {
                        auto w = buildInstA(OpCode::TRY_END, 0);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    default: break;
                }
            }

            if (!inst.words.empty()) {
                insts.push_back(inst);
            }
            }

            IRNode* cNode = bb->controlNode;
            if (cNode->op == IROp::If) {
                BasicBlock* trueBlock = nullptr;
                BasicBlock* falseBlock = nullptr;
                for (auto* succ : bb->succs) {
                    if (succ->controlNode->op == IROp::IfTrue) trueBlock = succ;
                    if (succ->controlNode->op == IROp::IfFalse) falseBlock = succ;
                }
                EncodedInst dummy;
                int condReg = ensureReg(cNode->dataInputs[0], dummy.words, chunk, 124);
                if (!dummy.words.empty()) insts.push_back(dummy);
                
                if (falseBlock == nextBb) {
                    EncodedInst jmpTrue;
                    jmpTrue.isJump = true;
                    jmpTrue.jumpOp = OpCode::JMP_TRUE;
                    jmpTrue.jumpA = condReg;
                    jmpTrue.jumpTarget = trueBlock;
                    insts.push_back(jmpTrue);
                } else if (trueBlock == nextBb) {
                    EncodedInst jmpFalse;
                    jmpFalse.isJump = true;
                    jmpFalse.jumpOp = OpCode::JMP_FALSE;
                    jmpFalse.jumpA = condReg;
                    jmpFalse.jumpTarget = falseBlock;
                    insts.push_back(jmpFalse);
                } else {
                    EncodedInst jmpFalse;
                    jmpFalse.isJump = true;
                    jmpFalse.jumpOp = OpCode::JMP_FALSE;
                    jmpFalse.jumpA = condReg;
                    jmpFalse.jumpTarget = falseBlock;
                    insts.push_back(jmpFalse);
                    
                    EncodedInst jmpTrue;
                    jmpTrue.isJump = true;
                    jmpTrue.jumpOp = OpCode::JMP;
                    jmpTrue.jumpTarget = trueBlock;
                    insts.push_back(jmpTrue);
                }
            }
            else if (cNode->op == IROp::TryBegin) {
                BasicBlock* catchBlock = nullptr;
                BasicBlock* tryBlock = nullptr;
                for (auto* succ : bb->succs) {
                    if (succ->controlNode->op == IROp::Catch) catchBlock = succ;
                    else tryBlock = succ;
                }
                int errReg = catchBlock->controlNode->physicalReg;
                
                EncodedInst tryInst;
                tryInst.isJump = true;
                tryInst.jumpOp = OpCode::TRY_BEGIN;
                tryInst.jumpA = errReg;
                tryInst.jumpTarget = catchBlock;
                insts.push_back(tryInst);
                
                if (tryBlock != nextBb) {
                    EncodedInst jmpInst;
                    jmpInst.isJump = true;
                    jmpInst.jumpOp = OpCode::JMP;
                    jmpInst.jumpTarget = tryBlock;
                    insts.push_back(jmpInst);
                }
            }
            else if (cNode->op != IROp::Return && cNode->op != IROp::Throw) {
                if (!bb->succs.empty() && bb->succs[0] != nextBb) {
                    EncodedInst jmpInst;
                    jmpInst.isJump = true;
                    jmpInst.jumpOp = OpCode::JMP;
                    jmpInst.jumpTarget = bb->succs[0];
                    insts.push_back(jmpInst);
                }
            }
        }

    // 2. Branch Relaxation
    bool changed = true;
    while (changed) {
        changed = false;
        int offset = 0;
        for (auto& inst : insts) {
            inst.offset = offset;
            offset += static_cast<int>(inst.words.size());
        }
        for (auto& inst : insts) {
            if (inst.isJump) {
                int targetOffset = insts[blockToInstIdx[inst.jumpTarget]].offset;
                int relOffset = targetOffset - (inst.offset + static_cast<int>(inst.words.size()));
                
                if (inst.jumpOp == OpCode::JMP) {
                    if (relOffset == 0) {
                        if (!inst.words.empty()) {
                            inst.words.clear();
                            inst.isJump = false;
                            changed = true;
                        }
                    } else {
                        if (relOffset < -8388607 || relOffset > 8388607) throw std::runtime_error("Emitter Error: Jump too large even for 24-bit!");
                        auto newWords = std::vector<uint32_t>{ CREATE_sAx(OpCode::JMP, relOffset) };
                        if (inst.words != newWords) {
                            inst.words = newWords;
                            changed = true;
                        }
                    }
                } else {
                    if (relOffset < -32767 || relOffset > 32767) {
                        if (!inst.isTrampoline) {
                            inst.isTrampoline = true;
                            changed = true;
                            if (inst.jumpOp == OpCode::JMP_TRUE) {
                                inst.words = buildInstAsBx(OpCode::JMP_FALSE, inst.jumpA, 1);
                                inst.words.push_back(CREATE_sAx(OpCode::JMP, 0));
                            } else if (inst.jumpOp == OpCode::JMP_FALSE) {
                                inst.words = buildInstAsBx(OpCode::JMP_TRUE, inst.jumpA, 1);
                                inst.words.push_back(CREATE_sAx(OpCode::JMP, 0));
                            } else if (inst.jumpOp == OpCode::TRY_BEGIN) {
                                inst.words = buildInstAsBx(OpCode::TRY_BEGIN, inst.jumpA, 1);
                                inst.words.push_back(CREATE_sAx(OpCode::JMP, 1));
                                inst.words.push_back(CREATE_sAx(OpCode::JMP, 0));
                            }
                        }
                        if (inst.jumpOp == OpCode::TRY_BEGIN) {
                            int catchRel = targetOffset - (inst.offset + static_cast<int>(inst.words.size()));
                            if (GET_sAx(inst.words.back()) != catchRel) {
                                inst.words.back() = CREATE_sAx(OpCode::JMP, catchRel);
                                changed = true;
                            }
                        } else {
                            int farRel = targetOffset - (inst.offset + static_cast<int>(inst.words.size()));
                            if (GET_sAx(inst.words.back()) != farRel) {
                                inst.words.back() = CREATE_sAx(OpCode::JMP, farRel);
                                changed = true;
                            }
                        }
                    } else {
                        auto newWords = buildInstAsBx(inst.jumpOp, inst.jumpA, relOffset);
                        if (inst.words != newWords) {
                            inst.words = newWords;
                            inst.isTrampoline = false;
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    // 3. Binary Emission
    int lastLine = 0;
    for (const auto& inst : insts) {
        if (inst.node && inst.node->line > 0) lastLine = inst.node->line;
        for (uint32_t word : inst.words) {
            chunk.write(word, lastLine);
        }
    }
    
    // Return the total number of registers used (localCount)
    int absoluteMaxReg = dynamicSpillBase;
    for (const auto& inst : insts) {
        if (inst.node && (inst.node->op == IROp::Call || inst.node->op == IROp::TailCall || 
                          inst.node->op == IROp::Invoke || inst.node->op == IROp::TailInvoke ||
                          inst.node->op == IROp::InvokeFallback || inst.node->op == IROp::TailInvokeFallback ||
                          inst.node->op == IROp::SuperInvoke || inst.node->op == IROp::TailSuperInvoke ||
                          inst.node->op == IROp::BuildList || inst.node->op == IROp::BuildDict ||
                          inst.node->op == IROp::BuildSet || inst.node->op == IROp::ConcatStrings ||
                          inst.node->op == IROp::BuildMatrix || inst.node->op == IROp::IndexGet ||
                          inst.node->op == IROp::IndexSet || inst.node->op == IROp::SliceGet ||
                          inst.node->op == IROp::SliceSet || inst.node->op == IROp::BuildNamespace)) {
            int argsCount = static_cast<int>(inst.node->dataInputs.size());
            if (inst.node->op == IROp::BuildNamespace) argsCount += 1; // +1 for spillBase offset
            if (dynamicSpillBase + argsCount > absoluteMaxReg) {
                absoluteMaxReg = dynamicSpillBase + argsCount;
            }
        }
    }
    return absoluteMaxReg;
}

} // namespace regvm
} // namespace jc
