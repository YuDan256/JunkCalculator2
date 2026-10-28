#include "Emitter.h"
#include <stdexcept>
#include <unordered_map>

namespace jc {

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
    if (val > 0xFFFFFF) JC2_THROW(EmitterError, "Operand exceeds 24-bit limit.");
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
    if (bx > 0xFFFFFF) JC2_THROW(EmitterError, "Bx exceeds 24-bit limit.");

    std::vector<uint32_t> words;
    words.push_back(CREATE_ABx(op, encA, encBx));
    if (extA) words.push_back(CREATE_Ax(OpCode::EXTRAARG, a));
    if (extBx) words.push_back(CREATE_Ax(OpCode::EXTRAARG, bx));
    return words;
}

static std::vector<uint32_t> buildInstAsBx(OpCode op, int a, int sbx) {
    int encA; bool extA;
    encodeOperand(a, OpType::NORMAL, encA, extA);

    if (sbx < -32767 || sbx > 32767) JC2_THROW(EmitterError, "sBx out of bounds.");
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

static int packArgs(std::vector<uint32_t>& words, const std::vector<IRNode*>& args, Chunk& chunk, int dynamicSpillBase,
                    bool allowZeroMove = false) {
    // ★ 零搬运快路径：参数块已经连续躺在 r..r+n-1 时直接以 r 为基址。
    //   只对 CALL/TAIL_CALL 开放：INVOKE 会把解析出的方法闭包写回基址槽，
    //   低基址会覆盖调用方仍存活的值；CALL 对基址只读。
    if (allowZeroMove && !args.empty()) {
        int r = args[0]->physicalReg;
        bool contiguous = r >= 0;
        for (size_t i = 0; i < args.size() && contiguous; ++i) {
            bool unmaterializedConst = (args[i]->op == IROp::Constant && args[i]->physicalReg == -1);
            if (unmaterializedConst || args[i]->physicalReg != r + static_cast<int>(i)) {
                contiguous = false;
            }
        }
        if (contiguous) return r;
    }
    int base = dynamicSpillBase;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i]->op == IROp::Constant && args[i]->physicalReg == -1) {
            int idx = chunk.addConstant(args[i]->constVal);
            auto loadk = buildInstABx(OpCode::LOADK, base + static_cast<int>(i), idx);
            words.insert(words.end(), loadk.begin(), loadk.end());
        } else {
            int reg = ensureReg(args[i], words, chunk, 124);
            if (base + static_cast<int>(i) != reg) {
                auto move = buildInstAB(OpCode::MOVE, base + static_cast<int>(i), reg);
                words.insert(words.end(), move.begin(), move.end());
            }
        }
    }
    return base;
}

// 清掉不产生字节码的空跳板块（Merge / IfTrue / IfFalse）：前驱直接接到唯一后继。
// 这类块是 if/for/match 布局里 JMP 1 + 死 JMP 的根源；只处理无 Phi 的
// 安全情形，带 Phi 的汇合块留给布局后 DCE。
static bool blockEmitsCode(BasicBlock* bb) {
    if (!bb->controlNode) return true;
    const IROp ctrl = bb->controlNode->op;
    if (ctrl != IROp::Merge && ctrl != IROp::IfTrue && ctrl != IROp::IfFalse) return true;
    for (IRNode* node : bb->instructions) {
        if (node->op == IROp::Merge || node->op == IROp::IfTrue || node->op == IROp::IfFalse) continue;
        if (node->op == IROp::Phi) return true;   // 有 Phi，不在这里拆
        if (node->op == IROp::Constant && node->physicalReg == -1) continue;
        return true;
    }
    return false;
}

static bool blockHasPhi(BasicBlock* bb) {
    for (IRNode* node : bb->instructions) {
        if (node->op == IROp::Phi) return true;
    }
    return false;
}

static void simplifyEmptyMergeBlocks(IRGraph* graph) {
    bool changed = true;
    while (changed) {
        changed = false;

        // 先重建 preds，避免上一轮删除留下的旧边。
        for (auto& bbPtr : graph->blocks) bbPtr->preds.clear();
        for (auto& bbPtr : graph->blocks) {
            for (BasicBlock* succ : bbPtr->succs) {
                if (succ) succ->preds.push_back(bbPtr.get());
            }
        }

        // 一轮只计算映射，不边删边改，避免后继又被删除后留下悬空 target。
        std::unordered_map<BasicBlock*, BasicBlock*> redirect;
        for (size_t i = 1; i < graph->blocks.size(); ++i) {
            BasicBlock* bb = graph->blocks[i].get();
            if (bb->succs.size() != 1) continue;
            BasicBlock* target = bb->succs[0];
            if (!target || target == bb) continue;
            if (blockEmitsCode(bb)) continue;
            if (blockHasPhi(target)) continue;   // 目标 Phi 的边序会被打乱，保守跳过
            redirect[bb] = target;
        }
        if (redirect.empty()) break;

        auto resolve = [&](BasicBlock* bb) -> BasicBlock* {
            BasicBlock* cur = bb;
            for (size_t guard = 0; guard <= graph->blocks.size(); ++guard) {
                auto it = redirect.find(cur);
                if (it == redirect.end()) return cur;
                cur = it->second;
            }
            return nullptr;   // 环：不重定向，也不删除
        };

        // 所有前驱边先指向最终目标，再统一删除跳板块。
        for (auto& bbPtr : graph->blocks) {
            for (BasicBlock*& succ : bbPtr->succs) {
                if (BasicBlock* resolved = resolve(succ)) succ = resolved;
            }
        }

        std::vector<std::unique_ptr<BasicBlock>> kept;
        kept.reserve(graph->blocks.size());
        for (auto& bbPtr : graph->blocks) {
            if (redirect.count(bbPtr.get()) && resolve(bbPtr.get())) {
                changed = true;
                continue;
            }
            kept.push_back(std::move(bbPtr));
        }
        graph->blocks = std::move(kept);
    }
}

int Emitter::emit(IRGraph* graph, Chunk& chunk) {
    simplifyEmptyMergeBlocks(graph);

    for (auto& cb : graph->postAllocCallbacks) {
        cb();
    }

    int maxReg = -1;
    for (auto& nodePtr : graph->getNodes()) {
        if (nodePtr->physicalReg > maxReg) maxReg = nodePtr->physicalReg;
    }
    int dynamicSpillBase = maxReg + 1;

    // ★ 该 IR 指令是否会把自己的 physicalReg 写成结果（用于判定死写）。
    //   只列纯取值类指令：控制流 / 存储 / 调用一律不算——调用的结果虽写 A，
    //   但 nil 写可能承担"调用前释放引用"的职责，不能当死写删。
    auto writesResultReg = [](IROp op) -> bool {
        switch (op) {
            case IROp::Constant: case IROp::Move: case IROp::LoadLocal:
            case IROp::GetGlobal: case IROp::GetUpvalue: case IROp::GetRefParam:
            case IROp::GetSelf: case IROp::GetCurrentClosure: case IROp::Closure:
            case IROp::GetProperty: case IROp::GetPrivate: case IROp::TryGetProperty:
            case IROp::GetSuper: case IROp::Import:
            case IROp::Add: case IROp::Sub: case IROp::Mul: case IROp::Div:
            case IROp::IDiv: case IROp::Mod: case IROp::Pow: case IROp::LeftDivide:
            case IROp::Eq: case IROp::Neq: case IROp::Lt: case IROp::Le:
            case IROp::Gt: case IROp::Ge: case IROp::Cmp: case IROp::Is: case IROp::IsSubset:
            case IROp::Not: case IROp::Neg: case IROp::ToBool:
            case IROp::BitAnd: case IROp::BitOr: case IROp::BitXor: case IROp::BitNot:
            case IROp::Shl: case IROp::Shr: case IROp::IsUninit:
            case IROp::BuildList: case IROp::BuildDict: case IROp::BuildSet:
            case IROp::BuildMatrix: case IROp::BuildNamespace: case IROp::BuildSlice:
            case IROp::IndexGet: case IROp::In: case IROp::Stringify:
            case IROp::ConcatStrings: case IROp::FormatString:
            case IROp::IterInit: case IROp::IterNext: case IROp::Class:
                return true;
            default:
                return false;
        }
    };

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
                    case IROp::IDiv: inst.words = buildBinary(OpCode::IDIV); break;
                    case IROp::Mod: inst.words = buildBinary(OpCode::MOD); break;
                    case IROp::Pow: inst.words = buildBinary(OpCode::POW); break;
                    case IROp::LeftDivide: inst.words = buildBinary(OpCode::LDIV); break;
                    case IROp::Eq: inst.words = buildBinary(OpCode::EQ); break;
                    case IROp::Neq: inst.words = buildBinary(OpCode::NEQ); break;
                    case IROp::Lt: inst.words = buildBinary(OpCode::LT); break;
                    case IROp::Le: inst.words = buildBinary(OpCode::LE); break;
                    case IROp::Gt: inst.words = buildBinary(OpCode::GT); break;
                    case IROp::Ge: inst.words = buildBinary(OpCode::GE); break;
                    case IROp::Cmp: inst.words = buildBinary(OpCode::CMP); break;
                    case IROp::Is: inst.words = buildBinary(OpCode::IS); break;
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
                        if (node->dataInputs[0]->op == IROp::Constant && node->dataInputs[0]->physicalReg == -1) {
                            int idx = chunk.addConstant(node->dataInputs[0]->constVal);
                            auto w = buildInstABx(OpCode::LOADK, node->physicalReg, idx);
                            inst.words.insert(inst.words.end(), w.begin(), w.end());
                        } else {
                            int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                            if (node->physicalReg != b) {
                                auto w = buildInstAB(OpCode::MOVE, node->physicalReg, b);
                                inst.words.insert(inst.words.end(), w.begin(), w.end());
                            }
                        }
                        break;
                    }
                    case IROp::FreeReg: {
                        auto w = buildInstA(OpCode::LOAD_NIL, node->physicalReg);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
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
                            src.isConst = ref.isConst;
                            if (ref.sourceType == 1) {
                                src.sourceRef = chunk.addConstant(Value(ref.name));
                            } else if (ref.sourceType == 2) {
                                src.sourceRef = ref.localNode->getResolved()->physicalReg;
                            } else if (ref.sourceType == 4) {
                                src.sourceRef = ref.localNode->getResolved()->payload1;
                            } else if (ref.sourceType == 3) {
                                src.sourceRef = ref.upvalIdx;
                            }
                            if (!ref.kwName.empty()) {
                                src.kwNameIdx = chunk.addConstant(Value(ref.kwName));
                            } else {
                                src.kwNameIdx = 0;
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
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase, true);
                        if (node->payload2 > 0) {
                            inst.words.push_back(CREATE_Ax(OpCode::SET_KW_ARGC, node->payload2));
                        }
                        auto call = buildInstABC(node->op == IROp::Call ? OpCode::CALL : OpCode::TAIL_CALL, node->physicalReg, spillBase, node->payload1);
                        inst.words.insert(inst.words.end(), call.begin(), call.end());
                        break;
                    }
                    case IROp::Invoke: case IROp::TailInvoke: case IROp::InvokePrivate: case IROp::TailInvokePrivate: {
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase);
                        if (node->payload2 > 0) {
                            inst.words.push_back(CREATE_Ax(OpCode::SET_KW_ARGC, node->payload2));
                        }
                        uint32_t icIdx = chunk.addInlineCache(chunk.addConstant(Value(node->name)));
                        OpCode op = OpCode::INVOKE;
                        if (node->op == IROp::TailInvoke) op = OpCode::TAIL_INVOKE;
                        else if (node->op == IROp::InvokePrivate) op = OpCode::INVOKE_PRIVATE;
                        else if (node->op == IROp::TailInvokePrivate) op = OpCode::TAIL_INVOKE_PRIVATE;
                        auto inv = buildInstABC(op, spillBase, node->payload1, icIdx);
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
                        if (node->payload2 > 0) {
                            inst.words.push_back(CREATE_Ax(OpCode::SET_KW_ARGC, node->payload2));
                        }
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
                        if (node->payload2 > 0) {
                            inst.words.push_back(CREATE_Ax(OpCode::SET_KW_ARGC, node->payload2));
                        }
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
                        std::vector<uint16_t> rowCols;
                        rowCols.reserve(node->rowCols.size());
                        for (uint32_t rc : node->rowCols) rowCols.push_back(static_cast<uint16_t>(rc));
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
                    case IROp::NamespaceBegin: {
                        auto w = buildInstA(OpCode::NAMESPACE_BEGIN, 0);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::BuildNamespace: {
                        int fieldCount = static_cast<int>(node->payload1) * 3;
                        std::vector<IRNode*> argsToPack;
                        for (uint32_t i = 0; i < node->payload1; ++i) {
                            IRNode* key = node->dataInputs[i * 3];
                            IRNode* target = node->dataInputs[fieldCount + static_cast<int>(i)];
                            IRNode* isConst = node->dataInputs[i * 3 + 2];
                            if (target && target->op == IROp::Constant) target->physicalReg = -1;
                            argsToPack.push_back(key);
                            argsToPack.push_back(target);
                            argsToPack.push_back(isConst);
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
                    case IROp::MatrixCompInit: {
                        auto w = buildInstA(OpCode::MATRIX_COMP_INIT, node->physicalReg);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::MatrixCompAppend: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int b = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        auto w = buildInstAB(OpCode::MATRIX_COMP_APPEND, a, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::MatrixCompEnd: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstA(OpCode::MATRIX_COMP_END, a);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        if (node->physicalReg != a) {
                            auto loadRes = buildInstAB(OpCode::MOVE, node->physicalReg, a);
                            inst.words.insert(inst.words.end(), loadRes.begin(), loadRes.end());
                        }
                        break;
                    }
                    case IROp::SetInit: {
                        auto w = buildInstA(OpCode::SET_INIT, node->physicalReg);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::SetAppend: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int b = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        auto w = buildInstAB(OpCode::SET_APPEND, a, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::DictInit: {
                        auto w = buildInstA(OpCode::DICT_INIT, node->physicalReg);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::DictAppend: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int b = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        int c = ensureReg(node->dataInputs[2], inst.words, chunk, 126);
                        auto w = buildInstABC(OpCode::DICT_APPEND, a, b, c);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
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
                    case IROp::IndexSet: {
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase);
                        auto set = buildInstABC(OpCode::INDEX_SET, spillBase, 0, static_cast<int>(node->payload1));
                        inst.words.insert(inst.words.end(), set.begin(), set.end());
                        if (node->physicalReg != spillBase) {
                            auto loadRes = buildInstAB(OpCode::MOVE, node->physicalReg, spillBase);
                            inst.words.insert(inst.words.end(), loadRes.begin(), loadRes.end());
                        }
                        break;
                    }
                    case IROp::BuildSlice: {
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase);
                        auto build = buildInstABC(OpCode::BUILD_SLICE, node->physicalReg, spillBase, 0);
                        inst.words.insert(inst.words.end(), build.begin(), build.end());
                        break;
                    }
                    case IROp::MakeSpread: {
                        int spillBase = packArgs(inst.words, node->dataInputs, chunk, dynamicSpillBase);
                        auto build = buildInstABC(OpCode::MAKE_SPREAD, node->physicalReg, spillBase, node->payload1);
                        inst.words.insert(inst.words.end(), build.begin(), build.end());
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
                        OpCode op = node->payload1 ? OpCode::TRAIT : OpCode::CLASS;
                        auto w = buildInstABx(op, node->physicalReg, nameIdx);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Method: case IROp::MethodPrivate: case IROp::MethodConst: case IROp::MethodPrivateConst: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int c = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        uint32_t nameIdx = chunk.addConstant(Value(node->name));
                        OpCode op;
                        if (node->payload1) {
                            op = (node->op == IROp::MethodPrivate) ? OpCode::METHOD_ABSTRACT_PRIVATE : OpCode::METHOD_ABSTRACT;
                        } else {
                            op = OpCode::METHOD;
                            if (node->op == IROp::MethodPrivate) op = OpCode::METHOD_PRIVATE;
                            else if (node->op == IROp::MethodConst) op = OpCode::METHOD_CONST;
                            else if (node->op == IROp::MethodPrivateConst) op = OpCode::METHOD_PRIVATE_CONST;
                        }
                        auto w = buildInstABC(op, a, nameIdx, c, OpType::NORMAL, OpType::NORMAL);
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
                    case IROp::WithTrait: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int b = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        auto w = buildInstAB(OpCode::WITH_TRAIT, a, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::FreezeClass: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstA(OpCode::FREEZE_CLASS, a);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::GetProperty: case IROp::GetPrivate: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        uint32_t icIdx = chunk.addInlineCache(chunk.addConstant(Value(node->name)));
                        OpCode op = node->op == IROp::GetPrivate ? OpCode::GET_PRIVATE : OpCode::GET_PROP;
                        auto w = buildInstABC(op, node->physicalReg, b, icIdx, OpType::NORMAL, OpType::NORMAL);
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
                    case IROp::SetProperty: case IROp::SetPrivate: case IROp::DefinePrivate: case IROp::DefinePrivateConst: case IROp::DefineProp: case IROp::DefinePropConst: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int c = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        uint32_t icIdx = chunk.addInlineCache(chunk.addConstant(Value(node->name)));
                        OpCode op = OpCode::SET_PROP;
                        if (node->op == IROp::SetPrivate) op = OpCode::SET_PRIVATE;
                        else if (node->op == IROp::DefinePrivate) op = OpCode::DEFINE_PRIVATE;
                        else if (node->op == IROp::DefinePrivateConst) op = OpCode::DEFINE_PRIVATE_CONST;
                        else if (node->op == IROp::DefineProp) op = OpCode::DEFINE_PROP;
                        else if (node->op == IROp::DefinePropConst) op = OpCode::DEFINE_PROP_CONST;
                        auto w = buildInstABC(op, a, icIdx, c, OpType::NORMAL, OpType::NORMAL);
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
                    case IROp::GetCurrentClosure: {
                        auto w = buildInstA(OpCode::GET_CURRENT_CLOSURE, node->physicalReg);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Import: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstABC(OpCode::IMPORT, node->physicalReg, b, node->payload1 ? 1 : 0);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Export: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstAB(OpCode::EXPORT, node->physicalReg, b);
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
                        auto w = buildInstA(OpCode::ASSERT_RETURN_TYPE, a);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::AssertType: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int b = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        uint32_t nameIdx = chunk.addConstant(Value(node->name));
                        auto w = buildInstABC(OpCode::ASSERT_TYPE, a, b, nameIdx, OpType::NORMAL, OpType::NORMAL);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::MatchType: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int c = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        auto w = buildInstABC(OpCode::MATCH_TYPE, node->physicalReg, b, c, OpType::NORMAL, OpType::NORMAL);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::IsSubset: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int c = ensureReg(node->dataInputs[1], inst.words, chunk, 125);
                        auto w = buildInstABC(OpCode::IS_SUBSET, node->physicalReg, b, c, OpType::NORMAL, OpType::NORMAL);
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
                    case IROp::MatchInit: {
                        int b = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstAB(OpCode::MATCH_INIT, node->physicalReg, b);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Return: {
                        if (node->dataInputs[0]->op == IROp::Constant && node->dataInputs[0]->physicalReg == -1) {
                            int idx = chunk.addConstant(node->dataInputs[0]->constVal);
                            auto loadk = buildInstABx(OpCode::LOADK, 124, idx);
                            inst.words.insert(inst.words.end(), loadk.begin(), loadk.end());
                            auto ret = buildInstA(OpCode::RETURN, 124);
                            inst.words.insert(inst.words.end(), ret.begin(), ret.end());
                        } else {
                            int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                            auto ret = buildInstA(OpCode::RETURN, a);
                            inst.words.insert(inst.words.end(), ret.begin(), ret.end());
                        }
                        break;
                    }
                    case IROp::Throw: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto thr = buildInstA(OpCode::THROW, a);
                        inst.words.insert(inst.words.end(), thr.begin(), thr.end());
                        break;
                    }
                    case IROp::ThrowTyped: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        int typeIdx = chunk.addConstant(node->dataInputs[1]->constVal);
                        auto thr = buildInstABx(OpCode::THROW_TYPED, a, typeIdx);
                        inst.words.insert(inst.words.end(), thr.begin(), thr.end());
                        break;
                    }
                    case IROp::TryEnd: {
                        auto w = buildInstA(OpCode::TRY_END, 0);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::Defer: {
                        int a = ensureReg(node->dataInputs[0], inst.words, chunk, 124);
                        auto w = buildInstA(OpCode::DEFER, a);
                        inst.words.insert(inst.words.end(), w.begin(), w.end());
                        break;
                    }
                    case IROp::RunDefers: {
                        auto w = buildInstA(OpCode::RUN_DEFERS, node->payload1);
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
                for (size_t si = 0; si < bb->succs.size(); ++si) {
                    BasicBlock* succ = bb->succs[si];
                    int kind = (si < bb->succKind.size()) ? bb->succKind[si] : 0;
                    if (kind == 1 || (!kind && succ->controlNode->op == IROp::IfTrue)) trueBlock = succ;
                    if (kind == 2 || (!kind && succ->controlNode->op == IROp::IfFalse)) falseBlock = succ;
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
                // A 的最高位是 silent 标记；低 7 位仍是 errReg（寄存器最大 126）。
                tryInst.jumpA = errReg | (cNode->payload1 ? 0x80 : 0);
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
            else if (cNode->op != IROp::Return && cNode->op != IROp::Throw && cNode->op != IROp::ThrowTyped) {
                if (!bb->succs.empty() && bb->succs[0] != nextBb) {
                    EncodedInst jmpInst;
                    jmpInst.isJump = true;
                    jmpInst.jumpOp = OpCode::JMP;
                    jmpInst.jumpTarget = bb->succs[0];
                    insts.push_back(jmpInst);
                }
            }
        }

    // 1a. FreeReg 的死写消除
    //
    //   FreeReg 在块尾为"已死寄存器"发一条 LOAD_NIL，让 GC 能提前回收。若紧接着的下一条
    //   指令（跳过 label）就把同一个寄存器写成结果，这条 nil 写立刻被覆盖，属于死写：
    //   删掉它不会改变任何可观察行为，GC 的差异也只有一条指令。
    //   调用类指令不算覆盖（见 writesResultReg 的说明）。
    for (size_t i = 0; i + 1 < insts.size(); ++i) {
        auto& inst = insts[i];
        if (!inst.node || inst.node->op != IROp::FreeReg) continue;
        int freedReg = inst.node->physicalReg;
        if (freedReg < 0) continue;
        size_t j = i + 1;
        while (j < insts.size() && insts[j].words.empty() && !insts[j].isJump) ++j;
        if (j >= insts.size()) continue;
        auto* next = insts[j].node;
        if (!next || next->physicalReg != freedReg) continue;
        if (!writesResultReg(next->op)) continue;
        inst.words.clear();
    }

    // 1b. 删除布局上不可达的块
    //
    //   判据（三条同时成立）：
    //     1) 布局中的前一个块以无条件 JMP / 返回 / 抛出结束（顺序执行过不来）；
    //     2) 没有任何跳转（含 TRY_BEGIN 的 handler）指向本块；
    //     3) 不是函数入口。
    //   这类块通常是空 else、return 之后的代码等，删掉可以省下整块指令（常见是一条 JMP）。
    //   删除一个块可能让后面的块变成不可达，因此迭代到不动点。
    {
        std::vector<std::pair<int, int>> ranges;   // 每块在 insts 中的 [start, end)
        ranges.reserve(graph->blocks.size());
        for (size_t bbIdx = 0; bbIdx < graph->blocks.size(); ++bbIdx) {
            int start = blockToInstIdx[graph->blocks[bbIdx].get()];
            int end = (bbIdx + 1 < graph->blocks.size())
                          ? blockToInstIdx[graph->blocks[bbIdx + 1].get()]
                          : static_cast<int>(insts.size());
            ranges.push_back({start, end});
        }

        bool removedAny = true;
        while (removedAny) {
            removedAny = false;
            std::unordered_set<BasicBlock*> targeted;
            for (const auto& inst : insts) {
                if (inst.isJump && inst.jumpTarget) targeted.insert(inst.jumpTarget);
            }
            for (size_t k = 1; k < ranges.size(); ++k) {
                BasicBlock* bb = graph->blocks[k].get();
                if (targeted.count(bb)) continue;

                auto [ps, pe] = ranges[k - 1];
                int last = -1;
                for (int i = pe - 1; i >= ps; --i) {
                    if (!insts[i].words.empty() || insts[i].isJump) { last = i; break; }
                }
                if (last < 0) continue;   // 前块是空的：保守起见不删

                bool noFallThrough = false;
                const auto& pi = insts[last];
                if (pi.isJump && pi.jumpOp == OpCode::JMP) {
                    noFallThrough = true;
                } else if (pi.node) {
                    switch (pi.node->op) {
                        case IROp::Return: case IROp::Throw: case IROp::ThrowTyped:
                        case IROp::TailCall: case IROp::TailInvoke: case IROp::TailInvokePrivate:
                        case IROp::TailInvokeFallback: case IROp::TailSuperInvoke:
                            noFallThrough = true;
                            break;
                        default: break;
                    }
                }
                if (!noFallThrough) continue;

                auto [s, e] = ranges[k];
                bool cleared = false;
                for (int i = s; i < e; ++i) {
                    if (!insts[i].words.empty() || insts[i].isJump) cleared = true;
                    insts[i].words.clear();
                    insts[i].isJump = false;
                    insts[i].isTrampoline = false;
                }
                if (cleared) removedAny = true;
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
        for (size_t i = 0; i < insts.size(); ++i) {
            auto& inst = insts[i];
            if (inst.isJump) {
                if ((inst.jumpOp == OpCode::JMP_FALSE || inst.jumpOp == OpCode::JMP_TRUE) && i + 1 < insts.size()) {
                    auto& nextInst = insts[i + 1];
                    if (nextInst.isJump && nextInst.jumpOp == OpCode::JMP) {
                        int targetIdx = blockToInstIdx[inst.jumpTarget];
                        int actualNext = static_cast<int>(i) + 2;
                        while (actualNext < static_cast<int>(insts.size()) && insts[actualNext].words.empty() && !insts[actualNext].isJump) {
                            actualNext++;
                        }
                        int actualTarget = targetIdx;
                        while (actualTarget < static_cast<int>(insts.size()) && insts[actualTarget].words.empty() && !insts[actualTarget].isJump) {
                            actualTarget++;
                        }
                        if (actualTarget == actualNext) {
                            inst.jumpOp = (inst.jumpOp == OpCode::JMP_FALSE) ? OpCode::JMP_TRUE : OpCode::JMP_FALSE;
                            inst.jumpTarget = nextInst.jumpTarget;
                            nextInst.isJump = false;
                            nextInst.words.clear();
                            changed = true;
                            continue;
                        }
                    }
                }

                // Jump Threading
                int targetIdx = blockToInstIdx[inst.jumpTarget];
                std::unordered_set<int> visited;
                while (targetIdx < static_cast<int>(insts.size())) {
                    if (visited.count(targetIdx)) break;
                    visited.insert(targetIdx);
                    
                    int nextIdx = targetIdx;
                    while (nextIdx < static_cast<int>(insts.size()) && insts[nextIdx].words.empty() && !insts[nextIdx].isJump) {
                        nextIdx++;
                    }
                    if (nextIdx < static_cast<int>(insts.size()) && insts[nextIdx].isJump && insts[nextIdx].jumpOp == OpCode::JMP) {
                        if (inst.jumpTarget != insts[nextIdx].jumpTarget) {
                            inst.jumpTarget = insts[nextIdx].jumpTarget;
                            targetIdx = blockToInstIdx[inst.jumpTarget];
                            changed = true;
                            continue;
                        }
                    }
                    break;
                }

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
                        if (relOffset < -8388607 || relOffset > 8388607) JC2_THROW(EmitterError, "Jump too large even for 24-bit!");
                        auto newWords = std::vector<uint32_t>{ CREATE_sAx(OpCode::JMP, relOffset) };
                        if (inst.words != newWords) {
                            inst.words = newWords;
                            changed = true;
                        }
                    }
                } else {
                    // ★ 条件跳转的目标就是紧接着的下一条指令：控制流无论如何都落到那里，
                    //   跳转本身不改变语义（条件值在跳转之前已经算好），直接删掉。
                    //   TRY_BEGIN 例外——它的"跳转"承载的是异常处理范围，不能删。
                    if (relOffset == 0 && inst.jumpOp != OpCode::TRY_BEGIN) {
                        if (!inst.words.empty()) {
                            inst.words.clear();
                            inst.isJump = false;
                            inst.isTrampoline = false;
                            changed = true;
                        }
                        continue;
                    }
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
                          inst.node->op == IROp::InvokePrivate || inst.node->op == IROp::TailInvokePrivate ||
                          inst.node->op == IROp::InvokeFallback || inst.node->op == IROp::TailInvokeFallback ||
                          inst.node->op == IROp::SuperInvoke || inst.node->op == IROp::TailSuperInvoke ||
                          inst.node->op == IROp::BuildList || inst.node->op == IROp::BuildDict ||
                          inst.node->op == IROp::BuildSet || inst.node->op == IROp::ConcatStrings ||
                          inst.node->op == IROp::BuildMatrix || inst.node->op == IROp::IndexGet ||
                          inst.node->op == IROp::IndexSet || inst.node->op == IROp::BuildSlice ||
                          inst.node->op == IROp::BuildNamespace ||
                          inst.node->op == IROp::MatchInit)) {
            int argsCount = static_cast<int>(inst.node->dataInputs.size());
            if (inst.node->op == IROp::BuildNamespace) argsCount += 1; // +1 for spillBase offset
            if (dynamicSpillBase + argsCount > absoluteMaxReg) {
                absoluteMaxReg = dynamicSpillBase + argsCount;
            }
        }
    }
    return absoluteMaxReg;
}

} // namespace jc
