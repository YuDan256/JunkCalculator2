#ifndef JC2_JIT_BYTECODE_TO_HIR_H
#define JC2_JIT_BYTECODE_TO_HIR_H

#include "../ir/HIRBuilder.h"
#include "BytecodeCFG.h"
#include "../../vm/Bytecode.h"
#include <vector>
#include <stdexcept>

namespace jc {
namespace jit {

// ============================================================================
// 字节码到 HIR 转换器 (抽象解释器) (Step 33)
// ============================================================================
class BytecodeToHIR {
public:
    BytecodeToHIR(const Chunk& chunk, HIRBuilder& builder)
        : chunk_(chunk), builder_(builder) {}

    void build() {
        // 1. 构建字节码控制流图
        cfg_.build(chunk_);

        // 2. 初始化入口状态
        builder_.createStart();

        // 3. 抽象解释主循环：遍历基本块
        for (const auto& block : cfg_.blocks) {
            if (block.startIp >= static_cast<int>(chunk_.code.size())) continue;

            // Step 55: 乐观 Phi 插入 (Optimistic Phi Insertion)
            if (block.isLoopHeader) {
                std::vector<HIRNode*> forwardControls = { builder_.currentControl() };
                HIRNode* loopBegin = builder_.createLoopBegin(forwardControls);
                loopHeaderControls_[block.id] = loopBegin;
                
                std::vector<HIRNode*> phis(256, nullptr);
                for (size_t i = 0; i < 256; ++i) {
                    HIRNode* val = builder_.getLocal(i);
                    if (val) {
                        // 预创建 Phi 节点，初始只包含前向边的数据
                        HIRNode* phi = builder_.createPhi(val->type(), {val});
                        builder_.setLocal(i, phi);
                        phis[i] = phi;
                    }
                }
                loopHeaderPhis_[block.id] = phis;
            }

            int ip = block.startIp;
            while (ip < block.endIp) {
                int currentIp = ip;
                Instruction inst = chunk_.code[ip++];
                OpCode op = GET_OPCODE(inst);

                int a = GET_A(inst);
                int b = GET_B(inst);
                int c = GET_C(inst);
                int bx = GET_Bx(inst);
                int sbx = GET_sBx(inst);
                int sax = GET_sAx(inst);
                int ax = GET_Ax(inst);
                (void)sbx;
                (void)sax;
                (void)ax;

                auto fetchExtra = [&]() { return chunk_.code[ip++] >> 8; };

                // 模拟解释器的操作数获取逻辑，确保 IP 正确推进并获取扩展操作数
                switch (op) {
                    case OpCode::BUILD_LIST: case OpCode::BUILD_DICT: case OpCode::BUILD_SET:
                    case OpCode::CONCAT_STRINGS: case OpCode::DICT_REST: case OpCode::BUILD_MATRIX:
                    case OpCode::INDEX_GET: case OpCode::ITER_INIT: case OpCode::IN:
                    case OpCode::DICT_APPEND: case OpCode::FORMAT_STRING: case OpCode::BUILD_NAMESPACE:
                    case OpCode::ASSERT_PARAM_TYPE: case OpCode::GET_PROP: case OpCode::GET_PRIVATE:
                    case OpCode::TRY_GET_PROP: case OpCode::SET_PROP: case OpCode::SET_PRIVATE:
                    case OpCode::DEFINE_PRIVATE: case OpCode::DEFINE_PRIVATE_CONST:
                    case OpCode::DEFINE_PROP: case OpCode::DEFINE_PROP_CONST: case OpCode::INVOKE:
                    case OpCode::TAIL_INVOKE: case OpCode::INVOKE_PRIVATE: case OpCode::TAIL_INVOKE_PRIVATE:
                    case OpCode::INVOKE_FALLBACK: case OpCode::TAIL_INVOKE_FALLBACK:
                    case OpCode::GET_SUPER: case OpCode::SUPER_INVOKE: case OpCode::TAIL_SUPER_INVOKE:
                    case OpCode::METHOD: case OpCode::METHOD_PRIVATE: case OpCode::METHOD_CONST:
                    case OpCode::METHOD_PRIVATE_CONST: case OpCode::CALL: case OpCode::TAIL_CALL:
                    case OpCode::MATCH_SHAPE: case OpCode::MATCH_TYPE:
                        if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                        if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                        if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                        break;

                    case OpCode::INDEX_SET:
                        if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                        if (c == ESCAPE_NORMAL_8) c = fetchExtra();
                        break;

                    case OpCode::MOVE: case OpCode::IS_UNINIT: case OpCode::UNM: case OpCode::NOT:
                    case OpCode::BNOT: case OpCode::TO_BOOL: case OpCode::INHERIT: case OpCode::LIST_APPEND:
                    case OpCode::SET_APPEND: case OpCode::STRINGIFY: case OpCode::ITER_NEXT:
                    case OpCode::IMPORT: case OpCode::GET_UPVAL: case OpCode::SET_UPVAL:
                    case OpCode::BUILD_SLICE: case OpCode::MATCH_INIT:
                        if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                        if (b == ESCAPE_NORMAL_8) b = fetchExtra();
                        break;

                    case OpCode::LOAD_NIL: case OpCode::LOAD_BOOL: case OpCode::ASSERT_RETURN_TYPE:
                    case OpCode::RETURN: case OpCode::GET_SELF: case OpCode::GET_CURRENT_CLOSURE:
                    case OpCode::LIST_INIT: case OpCode::LIST_COMP_END: case OpCode::SET_INIT:
                    case OpCode::DICT_INIT: case OpCode::THROW: case OpCode::DEFER: case OpCode::RUN_DEFERS:
                        if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                        break;

                    case OpCode::LOADK: case OpCode::GET_GLOBAL: case OpCode::SET_GLOBAL:
                    case OpCode::SET_GLOBAL_REF: case OpCode::DEFINE_CONST_GLOBAL: case OpCode::CLASS:
                    case OpCode::CLOSURE: case OpCode::GET_REF_PARAM: case OpCode::SET_REF_PARAM:
                        if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                        if (bx == ESCAPE_NORMAL_16) bx = fetchExtra();
                        break;

                    case OpCode::DELETE_GLOBAL: case OpCode::PASS_REFS:
                        if (bx == ESCAPE_NORMAL_16) bx = fetchExtra();
                        break;

                    case OpCode::ADD: case OpCode::SUB: case OpCode::MUL: case OpCode::DIV:
                    case OpCode::IDIV: case OpCode::MOD: case OpCode::POW: case OpCode::LDIV:
                    case OpCode::BAND: case OpCode::BOR: case OpCode::BXOR: case OpCode::SHL:
                    case OpCode::SHR: case OpCode::EQ: case OpCode::NEQ: case OpCode::LT:
                    case OpCode::LE: case OpCode::GT: case OpCode::GE: case OpCode::IS:
                        if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                        if (b == ESCAPE_KBIT_CONST) b = RKASK(fetchExtra());
                        else if (b == ESCAPE_KBIT_REG) b = fetchExtra();
                        if (c == ESCAPE_KBIT_CONST) c = RKASK(fetchExtra());
                        else if (c == ESCAPE_KBIT_REG) c = fetchExtra();
                        break;

                    case OpCode::JMP_TRUE: case OpCode::JMP_FALSE: case OpCode::TRY_BEGIN:
                        if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                        break;

                    case OpCode::JMP:
                    case OpCode::TRY_END: case OpCode::EXTRAARG: case OpCode::SET_KW_ARGC:
                        break;

                    default:
                        break;
                }

                // 辅助函数：解析 K-Bit 操作数（寄存器或常量）
                auto getRKNode = [&](int rk) -> HIRNode* {
                    if (ISK(rk)) {
                        int idx = INDEXK(rk);
                        const Value& kst = chunk_.constants[idx];
                        if (kst.isInt32()) return builder_.createBoxInt32(builder_.createInt32Constant(kst.asInt32()));
                        if (kst.isDouble()) return builder_.createBoxDouble(builder_.createDoubleConstant(kst.asDoubleRaw()));
                        if (kst.isBool()) return builder_.createBoxBool(builder_.createBoolConstant(kst.asBool()));
                        if (kst.isString()) return builder_.createStringConstant(kst.asString());
                        return builder_.createNoneConstant();
                    }
                    HIRNode* node = builder_.getLocal(rk);
                    if (!node) {
                        node = builder_.createLoadRegister(rk);
                        builder_.setLocal(rk, node);
                    }
                    return node;
                };

                // Eager Sync: 写入虚拟寄存器时，同步写回 VM::registers 以保证 GC 安全
                auto setLocalSync = [&](int reg, HIRNode* node) {
                    builder_.setLocal(reg, node);
                    HIRNode* boxedNode = node;
                    if (node->type() == JITType::Int32) boxedNode = builder_.createBoxInt32(node);
                    else if (node->type() == JITType::Double) boxedNode = builder_.createBoxDouble(node);
                    else if (node->type() == JITType::Bool) boxedNode = builder_.createBoxBool(node);
                    builder_.createStoreRegister(reg, boxedNode);
                };

                // 抽象解释：根据操作码更新 HIRBuilder 的虚拟寄存器状态
                switch (op) {
                    case OpCode::MOVE: {
                        // 复写传播 (Copy Propagation)：直接复用 HIR 节点指针，不生成新节点
                        HIRNode* val = getRKNode(b);
                        setLocalSync(a, val);
                        break;
                    }
                    case OpCode::LOADK: {
                        const Value& kst = chunk_.constants[bx];
                        HIRNode* node = nullptr;
                        if (kst.isInt32()) {
                            node = builder_.createBoxInt32(builder_.createInt32Constant(kst.asInt32()));
                        } else if (kst.isDouble()) {
                            node = builder_.createBoxDouble(builder_.createDoubleConstant(kst.asDoubleRaw()));
                        } else if (kst.isBool()) {
                            node = builder_.createBoxBool(builder_.createBoolConstant(kst.asBool()));
                        } else if (kst.isString()) {
                            node = builder_.createStringConstant(kst.asString());
                        } else if (kst.isNone()) {
                            node = builder_.createNoneConstant();
                        }
                        // TODO: 其他复杂常量类型（如 BigInt, Matrix）的加载
                        if (node) {
                            setLocalSync(a, node);
                        }
                        break;
                    }
                    case OpCode::LOAD_NIL: {
                        setLocalSync(a, builder_.createNoneConstant());
                        break;
                    }
                    case OpCode::LOAD_BOOL: {
                        setLocalSync(a, builder_.createBoxBool(builder_.createBoolConstant(b != 0)));
                        break;
                    }
                    case OpCode::GET_GLOBAL: {
                        InlineCache& ic = const_cast<InlineCache&>(chunk_.inlineCaches[bx]);
                        if (ic.cachedGlobalSlot >= 0) {
                            auto node = builder_.createLoadGlobal(ic.cachedGlobalSlot);
                            setLocalSync(a, node);
                        } else {
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            builder_.createDeoptimize(fs);
                        }
                        break;
                    }
                    case OpCode::SET_GLOBAL: {
                        InlineCache& ic = const_cast<InlineCache&>(chunk_.inlineCaches[bx]);
                        if (ic.cachedGlobalSlot >= 0) {
                            builder_.createStoreGlobal(ic.cachedGlobalSlot, builder_.getLocal(a));
                        } else {
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            builder_.createDeoptimize(fs);
                        }
                        break;
                    }
                    case OpCode::ADD:
                    case OpCode::SUB:
                    case OpCode::MUL:
                    case OpCode::DIV:
                    case OpCode::IDIV:
                    case OpCode::MOD: {
                        uint8_t fb = chunk_.typeFeedback[currentIp];
                        HIRNode* lhs = getRKNode(b);
                        HIRNode* rhs = getRKNode(c);
                        
                        if (fb == 0x01) { // Monomorphic Int32 (纯 32 位整数)
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            auto guardL = builder_.createGuardIsInt32(lhs, fs);
                            auto guardR = builder_.createGuardIsInt32(rhs, fs);
                            auto unboxL = builder_.createUnboxInt32(lhs, guardL);
                            auto unboxR = builder_.createUnboxInt32(rhs, guardR);
                            
                            HIRNode* opNode = nullptr;
                            if (op == OpCode::ADD) opNode = builder_.createAddI32(unboxL, unboxR);
                            else if (op == OpCode::SUB) opNode = builder_.createSubI32(unboxL, unboxR);
                            else if (op == OpCode::MUL) opNode = builder_.createMulI32(unboxL, unboxR);
                            else if (op == OpCode::DIV) opNode = builder_.createDivI32(unboxL, unboxR);
                            else if (op == OpCode::IDIV) opNode = builder_.createIDivI32(unboxL, unboxR);
                            else if (op == OpCode::MOD) opNode = builder_.createModI32(unboxL, unboxR);
                            
                            setLocalSync(a, builder_.createBoxInt32(opNode));
                            
                        } else if (fb == 0x02) { // Monomorphic Double (纯 64 位浮点数)
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            auto guardL = builder_.createGuardIsDouble(lhs, fs);
                            auto guardR = builder_.createGuardIsDouble(rhs, fs);
                            auto unboxL = builder_.createUnboxDouble(lhs, guardL);
                            auto unboxR = builder_.createUnboxDouble(rhs, guardR);
                            
                            HIRNode* opNode = nullptr;
                            if (op == OpCode::ADD) opNode = builder_.createAddF64(unboxL, unboxR);
                            else if (op == OpCode::SUB) opNode = builder_.createSubF64(unboxL, unboxR);
                            else if (op == OpCode::MUL) opNode = builder_.createMulF64(unboxL, unboxR);
                            else if (op == OpCode::DIV) opNode = builder_.createDivF64(unboxL, unboxR);
                            else if (op == OpCode::IDIV) opNode = builder_.createIDivF64(unboxL, unboxR);
                            else if (op == OpCode::MOD) opNode = builder_.createModF64(unboxL, unboxR);
                            
                            setLocalSync(a, builder_.createBoxDouble(opNode));
                        }
                        break;
                    }
                    case OpCode::BAND:
                    case OpCode::BOR:
                    case OpCode::BXOR:
                    case OpCode::SHL:
                    case OpCode::SHR: {
                        uint8_t fb = chunk_.typeFeedback[currentIp];
                        HIRNode* lhs = getRKNode(b);
                        HIRNode* rhs = getRKNode(c);
                        
                        if (fb == 0x01) {
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            auto guardL = builder_.createGuardIsInt32(lhs, fs);
                            auto guardR = builder_.createGuardIsInt32(rhs, fs);
                            auto unboxL = builder_.createUnboxInt32(lhs, guardL);
                            auto unboxR = builder_.createUnboxInt32(rhs, guardR);
                            
                            HIRNode* opNode = nullptr;
                            if (op == OpCode::BAND) opNode = builder_.createBitAndI32(unboxL, unboxR);
                            else if (op == OpCode::BOR) opNode = builder_.createBitOrI32(unboxL, unboxR);
                            else if (op == OpCode::BXOR) opNode = builder_.createBitXorI32(unboxL, unboxR);
                            else if (op == OpCode::SHL) opNode = builder_.createShlI32(unboxL, unboxR);
                            else if (op == OpCode::SHR) opNode = builder_.createShrI32(unboxL, unboxR);
                            
                            setLocalSync(a, builder_.createBoxInt32(opNode));
                        }
                        break;
                    }
                    case OpCode::IS:
                    case OpCode::EQ:
                    case OpCode::NEQ:
                    case OpCode::LT:
                    case OpCode::LE:
                    case OpCode::GT:
                    case OpCode::GE: {
                        uint8_t fb = chunk_.typeFeedback[currentIp];
                        HIRNode* lhs = getRKNode(b);
                        HIRNode* rhs = getRKNode(c);
                        
                        if (fb == 0x01) {
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            auto guardL = builder_.createGuardIsInt32(lhs, fs);
                            auto guardR = builder_.createGuardIsInt32(rhs, fs);
                            auto unboxL = builder_.createUnboxInt32(lhs, guardL);
                            auto unboxR = builder_.createUnboxInt32(rhs, guardR);
                            
                            HIRNode* opNode = nullptr;
                            if (op == OpCode::EQ || op == OpCode::IS) opNode = builder_.createCmpEqI32(unboxL, unboxR);
                            else if (op == OpCode::NEQ) opNode = builder_.createCmpNeqI32(unboxL, unboxR);
                            else if (op == OpCode::LT) opNode = builder_.createCmpLtI32(unboxL, unboxR);
                            else if (op == OpCode::LE) opNode = builder_.createCmpLeI32(unboxL, unboxR);
                            else if (op == OpCode::GT) opNode = builder_.createCmpGtI32(unboxL, unboxR);
                            else if (op == OpCode::GE) opNode = builder_.createCmpGeI32(unboxL, unboxR);
                            
                            setLocalSync(a, builder_.createBoxBool(opNode));
                        } else if (fb == 0x02) {
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            auto guardL = builder_.createGuardIsDouble(lhs, fs);
                            auto guardR = builder_.createGuardIsDouble(rhs, fs);
                            auto unboxL = builder_.createUnboxDouble(lhs, guardL);
                            auto unboxR = builder_.createUnboxDouble(rhs, guardR);
                            
                            HIRNode* opNode = nullptr;
                            if (op == OpCode::EQ) opNode = builder_.createCmpEqF64(unboxL, unboxR);
                            else if (op == OpCode::NEQ) opNode = builder_.createCmpNeqF64(unboxL, unboxR);
                            else if (op == OpCode::LT) opNode = builder_.createCmpLtF64(unboxL, unboxR);
                            else if (op == OpCode::LE) opNode = builder_.createCmpLeF64(unboxL, unboxR);
                            else if (op == OpCode::GT) opNode = builder_.createCmpGtF64(unboxL, unboxR);
                            else if (op == OpCode::GE) opNode = builder_.createCmpGeF64(unboxL, unboxR);
                            else if (op == OpCode::IS) opNode = builder_.createCmpEqTagged(lhs, rhs); // IS is strict bitwise equality
                            
                            setLocalSync(a, builder_.createBoxBool(opNode));
                        } else if (op == OpCode::IS) {
                            setLocalSync(a, builder_.createBoxBool(builder_.createCmpEqTagged(lhs, rhs)));
                        }
                        break;
                    }
                    case OpCode::UNM:
                    case OpCode::BNOT:
                    case OpCode::NOT:
                    case OpCode::TO_BOOL: {
                        uint8_t fb = chunk_.typeFeedback[currentIp];
                        HIRNode* val = builder_.getLocal(b);
                        
                        if (fb == 0x01) {
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            auto guard = builder_.createGuardIsInt32(val, fs);
                            auto unbox = builder_.createUnboxInt32(val, guard);
                            
                            if (op == OpCode::UNM) {
                                setLocalSync(a, builder_.createBoxInt32(builder_.createNegI32(unbox)));
                            } else if (op == OpCode::BNOT) {
                                setLocalSync(a, builder_.createBoxInt32(builder_.createNotI32(unbox)));
                            } else if (op == OpCode::NOT) {
                                auto zero = builder_.createInt32Constant(0);
                                setLocalSync(a, builder_.createBoxBool(builder_.createCmpEqI32(unbox, zero)));
                            } else if (op == OpCode::TO_BOOL) {
                                auto zero = builder_.createInt32Constant(0);
                                setLocalSync(a, builder_.createBoxBool(builder_.createCmpNeqI32(unbox, zero)));
                            }
                        } else if (fb == 0x02 && op == OpCode::UNM) {
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            auto guard = builder_.createGuardIsDouble(val, fs);
                            auto unbox = builder_.createUnboxDouble(val, guard);
                            setLocalSync(a, builder_.createBoxDouble(builder_.createNegF64(unbox)));
                        } else if (fb == 0x08 && (op == OpCode::NOT || op == OpCode::TO_BOOL)) {
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            auto guard = builder_.createGuardIsBool(val, fs);
                            auto unbox = builder_.createUnboxBool(val, guard);
                            if (op == OpCode::NOT) {
                                auto falseNode = builder_.createBoolConstant(false);
                                setLocalSync(a, builder_.createBoxBool(builder_.createCmpEqI32(unbox, falseNode)));
                            } else {
                                setLocalSync(a, val);
                            }
                        }
                        break;
                    }
                    case OpCode::RETURN: {
                        builder_.createReturn(getRKNode(a));
                        break;
                    }
                    case OpCode::GET_PROP: {
                        InlineCache& ic = const_cast<InlineCache&>(chunk_.inlineCaches[c]);
                        if (ic.cachedClassId != 0 && ic.cachedFieldIndex >= 0) {
                            HIRNode* obj = getRKNode(b);
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            builder_.createGuardIsClass(obj, fs, ic.cachedClassId);
                            auto offset = builder_.createInt32Constant(ic.cachedFieldIndex);
                            auto loadField = builder_.createLoadField(obj, offset);
                            setLocalSync(a, loadField);
                        } else {
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            builder_.createDeoptimize(fs);
                        }
                        break;
                    }
                    default:
                        // 尚未实现的指令，暂时跳过
                        break;
                }

                if (!builder_.currentControl()) break; // 控制流已终止，跳过基本块剩余指令
            }

            // Step 56: 回边数据流绑定 (Back-edge Data Flow Binding)
            if (builder_.currentControl()) {
                for (int succId : block.successors) {
                    const auto& succBlock = cfg_.blocks[succId];
                    if (succBlock.isLoopHeader) {
                        if (std::find(succBlock.backEdges.begin(), succBlock.backEdges.end(), block.id) != succBlock.backEdges.end()) {
                            // 1. 绑定控制流回边
                            if (loopHeaderControls_.count(succId)) {
                                loopHeaderControls_[succId]->addInput(builder_.currentControl());
                            }
                            
                            // 2. 绑定数据流回边
                            if (loopHeaderPhis_.count(succId)) {
                                auto& phis = loopHeaderPhis_[succId];
                                for (size_t i = 0; i < 256; ++i) {
                                    if (phis[i]) {
                                        HIRNode* backEdgeVal = builder_.getLocal(i);
                                        if (backEdgeVal) {
                                            phis[i]->addInput(backEdgeVal);
                                        } else {
                                            phis[i]->addInput(builder_.createNoneConstant());
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

private:
    const Chunk& chunk_;
    HIRBuilder& builder_;
    BytecodeCFG cfg_;
    std::map<int, std::vector<HIRNode*>> loopHeaderPhis_;
    std::map<int, HIRNode*> loopHeaderControls_;
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_BYTECODE_TO_HIR_H
