#ifndef JC2_JIT_BYTECODE_TO_HIR_H
#define JC2_JIT_BYTECODE_TO_HIR_H

#include "../ir/HIRBuilder.h"
#include "BytecodeCFG.h"
#include "../../vm/Bytecode.h"
#include "../../vm/VM.h"
#include <vector>
#include <stdexcept>

namespace jc {
namespace jit {

// ============================================================================
// 字节码到 HIR 转换器 (抽象解释器) (Step 33)
// ============================================================================
class BytecodeToHIR {
public:
    BytecodeToHIR(const Chunk& chunk, HIRBuilder& builder, int maxRegs, int inlineDepth = 0, int registerOffset = 0)
        : chunk_(chunk), builder_(builder), maxRegs_(maxRegs), inlineDepth_(inlineDepth), registerOffset_(registerOffset) {}

    void setOSRMode(int loopHeaderIp) {
        isOSR_ = true;
        osrLoopHeaderIp_ = loopHeaderIp;
    }

    HIRNode* buildInline(const std::vector<HIRNode*>& args) {
        isInline_ = true;
        inlineArgs_ = args;
        build();
        return inlineResult_;
    }

    void build() {
        // 1. 构建字节码控制流图
        cfg_.build(chunk_);

        // 2. 初始化入口状态
        if (isInline_) {
            for (size_t i = 0; i < inlineArgs_.size(); ++i) {
                builder_.setLocal(registerOffset_ + i, inlineArgs_[i]);
            }
            blockEntryControls_[0].push_back(builder_.createStart());
            blockExitStates_[-1] = builder_.getRegisters(); // Dummy predecessor for inline
        } else if (isOSR_) {
            auto osrEntry = builder_.createOSREntry(osrLoopHeaderIp_);
            for (int i = 0; i < maxRegs_; ++i) {
                auto load = builder_.createLoadRegister(registerOffset_ + i);
                builder_.setLocal(registerOffset_ + i, load);
            }
            int startBlockId = cfg_.ipToBlockId[osrLoopHeaderIp_];
            blockEntryControls_[startBlockId].push_back(osrEntry);
            blockExitStates_[-1] = builder_.getRegisters(); // Dummy predecessor for OSR
        } else {
            blockEntryControls_[0].push_back(builder_.createStart());
            blockExitStates_[-1] = builder_.getRegisters(); // Dummy predecessor
        }

        // 3. 抽象解释主循环：遍历基本块
        for (const auto& block : cfg_.blocks) {
            if (block.startIp >= static_cast<int>(chunk_.code.size())) continue;
            if (isOSR_ && block.startIp < osrLoopHeaderIp_) continue;

            auto& entryControls = blockEntryControls_[block.id];
            if (entryControls.empty()) continue; // 不可达块

            // 构建控制流汇合与 Phi 节点
            if (entryControls.size() > 1 || block.isLoopHeader) {
                HIRNode* merge = nullptr;
                if (block.isLoopHeader) {
                    merge = builder_.createLoopBegin(entryControls);
                    loopHeaderControls_[block.id] = merge;
                } else {
                    merge = builder_.createMerge(entryControls);
                }
                builder_.setCurrentControl(merge);

                std::vector<HIRNode*> phis(maxRegs_, nullptr);
                for (int i = 0; i < maxRegs_; ++i) {
                    bool needsPhi = false;
                    HIRNode* firstVal = nullptr;
                    
                    auto checkPreds = [&](const std::vector<int>& preds) {
                        for (int predId : preds) {
                            if (blockExitStates_.count(predId)) {
                                HIRNode* val = blockExitStates_[predId][i];
                                if (!firstVal) firstVal = val;
                                else if (firstVal != val) { needsPhi = true; break; }
                            } else if (block.isLoopHeader) {
                                needsPhi = true; break;
                            }
                        }
                    };
                    
                    std::vector<int> predsToCheck;
                    if (block.id == 0 || (isOSR_ && block.startIp == osrLoopHeaderIp_)) {
                        predsToCheck.push_back(-1);
                    }
                    for (int p : block.predecessors) {
                        predsToCheck.push_back(p);
                    }
                    checkPreds(predsToCheck);

                    if (needsPhi) {
                        JITType type = JITType::TaggedValue;
                        std::vector<HIRNode*> phiInputs;
                        
                        auto addInputs = [&](const std::vector<int>& preds) {
                            for (int predId : preds) {
                                if (blockExitStates_.count(predId)) {
                                    HIRNode* val = blockExitStates_[predId][i];
                                    if (val) {
                                        type = val->type();
                                        phiInputs.push_back(val);
                                    } else {
                                        phiInputs.push_back(builder_.createNoneConstant());
                                    }
                                }
                            }
                        };
                        
                        addInputs(predsToCheck);
                        
                        HIRNode* phi = builder_.createPhi(type, phiInputs);
                        builder_.setLocal(registerOffset_ + i, phi);
                        phis[i] = phi;
                    } else if (firstVal) {
                        builder_.setLocal(registerOffset_ + i, firstVal);
                    }
                }
                if (block.isLoopHeader) {
                    loopHeaderPhis_[block.id] = phis;
                }
            } else {
                builder_.setCurrentControl(entryControls[0]);
                int predId = block.predecessors.empty() ? -1 : block.predecessors[0];
                if (block.id == 0 || (isOSR_ && block.startIp == osrLoopHeaderIp_)) predId = -1;
                
                if (blockExitStates_.count(predId)) {
                    builder_.setRegisters(blockExitStates_[predId]);
                }
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

                    case OpCode::JMP_TRUE:
                    case OpCode::JMP_FALSE: {
                        if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                        uint8_t fb = chunk_.typeFeedback[currentIp];
                        HIRNode* val = builder_.getLocal(registerOffset_ + a);
                        HIRNode* condNode = nullptr;
                        
                        if (fb == 0x01) {
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            auto guard = builder_.createGuardIsInt32(val, fs);
                            auto unbox = builder_.createUnboxInt32(val, guard);
                            auto zero = builder_.createInt32Constant(0);
                            condNode = builder_.createCmpNeqI32(unbox, zero);
                        } else if (fb == 0x08) {
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            auto guard = builder_.createGuardIsBool(val, fs);
                            condNode = builder_.createUnboxBool(val, guard);
                        } else {
                            throw std::runtime_error("JIT Error: Unsupported type feedback for branch.");
                        }
                        
                        auto branch = builder_.createBranch(condNode);
                        auto ifTrue = builder_.createIfTrue(branch);
                        auto ifFalse = builder_.createIfFalse(branch);
                        
                        int trueTargetIp = (op == OpCode::JMP_TRUE) ? (ip + sbx) : ip;
                        int falseTargetIp = (op == OpCode::JMP_FALSE) ? (ip + sbx) : ip;
                        
                        if (cfg_.ipToBlockId.count(trueTargetIp)) {
                            blockEntryControls_[cfg_.ipToBlockId[trueTargetIp]].push_back(ifTrue);
                        }
                        if (cfg_.ipToBlockId.count(falseTargetIp)) {
                            blockEntryControls_[cfg_.ipToBlockId[falseTargetIp]].push_back(ifFalse);
                        }
                        break;
                    }

                    case OpCode::JMP: {
                        int targetIp = ip + sax;
                        if (cfg_.ipToBlockId.count(targetIp)) {
                            blockEntryControls_[cfg_.ipToBlockId[targetIp]].push_back(builder_.currentControl());
                        }
                        builder_.setCurrentControl(nullptr);
                        break;
                    }

                    case OpCode::TRY_BEGIN:
                        if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                        break;

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
                    HIRNode* node = builder_.getLocal(registerOffset_ + rk);
                    if (!node) {
                        node = builder_.createLoadRegister(registerOffset_ + rk);
                        builder_.setLocal(registerOffset_ + rk, node);
                    }
                    return node;
                };

                auto getBoxedRKNode = [&](int rk) -> HIRNode* {
                    HIRNode* node = getRKNode(rk);
                    if (node->type() != JITType::TaggedValue) {
                        if (node->type() == JITType::Int32) return builder_.createBoxInt32(node);
                        if (node->type() == JITType::Double) return builder_.createBoxDouble(node);
                        if (node->type() == JITType::Bool) return builder_.createBoxBool(node);
                    }
                    return node;
                };

                // ★ Step 81: OSR 模式下，跳过循环头之前的 HIR 节点生成，但保留 regFuncName_ 追踪
                if (isOSR_ && currentIp < osrLoopHeaderIp_) {
                    if (op == OpCode::LOADK) {
                        const Value& kst = chunk_.constants[bx];
                        if (kst.isString()) regFuncName_[a] = kst.asString();
                    } else if (op == OpCode::GET_GLOBAL) {
                        InlineCache& ic = const_cast<InlineCache&>(chunk_.inlineCaches[bx]);
                        if (ic.cachedGlobalSlot >= 0) regFuncName_[a] = chunk_.constants[ic.nameIdx].asString();
                    } else if (op == OpCode::MOVE) {
                        regFuncName_[a] = regFuncName_[b];
                    }
                    continue;
                }

                // Eager Sync: 写入虚拟寄存器时，同步写回 VM::registers 以保证 GC 安全
                auto setLocalSync = [&](int reg, HIRNode* node) {
                    builder_.setLocal(registerOffset_ + reg, node);
                    regFuncName_.erase(reg); // 默认清空函数名追踪
                };

                auto syncAllRegisters = [&]() {
                    for (int i = 0; i < maxRegs_; ++i) {
                        HIRNode* val = builder_.getLocal(registerOffset_ + i);
                        if (val) {
                            if (val->opcode() == HIROp::LoadRegister) {
                                auto loadReg = static_cast<RegisterAccessNode*>(val);
                                if (loadReg->regIndex() == registerOffset_ + i) {
                                    continue;
                                }
                            }
                            HIRNode* boxedNode = val;
                            if (val->type() == JITType::Int32) boxedNode = builder_.createBoxInt32(val);
                            else if (val->type() == JITType::Double) boxedNode = builder_.createBoxDouble(val);
                            else if (val->type() == JITType::Bool) boxedNode = builder_.createBoxBool(val);
                            builder_.createStoreRegister(registerOffset_ + static_cast<int>(i), boxedNode);
                        }
                    }
                };

                // 抽象解释：根据操作码更新 HIRBuilder 的虚拟寄存器状态
                switch (op) {
                    case OpCode::MOVE: {
                        // 复写传播 (Copy Propagation)：直接复用 HIR 节点指针，不生成新节点
                        HIRNode* val = getRKNode(b);
                        std::string trackedName = regFuncName_[b];
                        setLocalSync(a, val);
                        if (!trackedName.empty()) regFuncName_[a] = trackedName;
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
                            if (kst.isString()) regFuncName_[a] = kst.asString();
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
                            regFuncName_[a] = chunk_.constants[ic.nameIdx].asString();
                        } else {
                            throw std::runtime_error("JIT Error: Unresolved global variable.");
                        }
                        break;
                    }
                    case OpCode::SET_GLOBAL: {
                        InlineCache& ic = const_cast<InlineCache&>(chunk_.inlineCaches[bx]);
                        if (ic.cachedGlobalSlot >= 0) {
                            HIRNode* val = builder_.getLocal(registerOffset_ + a);
                            if (val->type() != JITType::TaggedValue) {
                                if (val->type() == JITType::Int32) val = builder_.createBoxInt32(val);
                                else if (val->type() == JITType::Double) val = builder_.createBoxDouble(val);
                                else if (val->type() == JITType::Bool) val = builder_.createBoxBool(val);
                            }
                            builder_.createStoreGlobal(ic.cachedGlobalSlot, val);
                        } else {
                            throw std::runtime_error("JIT Error: Unresolved global variable.");
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
                            if (op == OpCode::ADD) opNode = builder_.createAddI32(unboxL, unboxR, fs);
                            else if (op == OpCode::SUB) opNode = builder_.createSubI32(unboxL, unboxR, fs);
                            else if (op == OpCode::MUL) opNode = builder_.createMulI32(unboxL, unboxR, fs);
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
                        } else {
                            throw std::runtime_error("JIT Error: Unsupported type feedback for arithmetic op.");
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
                        } else {
                            throw std::runtime_error("JIT Error: Unsupported type feedback for bitwise op.");
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
                        } else {
                            throw std::runtime_error("JIT Error: Unsupported type feedback for comparison op.");
                        }
                        break;
                    }
                    case OpCode::UNM:
                    case OpCode::BNOT:
                    case OpCode::NOT:
                    case OpCode::TO_BOOL: {
                        uint8_t fb = chunk_.typeFeedback[currentIp];
                        HIRNode* val = builder_.getLocal(registerOffset_ + b);
                        
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
                        } else {
                            throw std::runtime_error("JIT Error: Unsupported type feedback for unary op.");
                        }
                        break;
                    }
                    case OpCode::RETURN: {
                        HIRNode* retVal = getRKNode(a);
                        if (retVal->type() != JITType::TaggedValue) {
                            if (retVal->type() == JITType::Int32) retVal = builder_.createBoxInt32(retVal);
                            else if (retVal->type() == JITType::Double) retVal = builder_.createBoxDouble(retVal);
                            else if (retVal->type() == JITType::Bool) retVal = builder_.createBoxBool(retVal);
                        }
                        if (isInline_) {
                            returnValues_.push_back(retVal);
                            returnControls_.push_back(builder_.currentControl());
                            builder_.setCurrentControl(nullptr);
                        } else {
                            syncAllRegisters();
                            builder_.createReturn(retVal);
                        }
                        break;
                    }
                    case OpCode::GET_PROP: {
                        InlineCache& ic = const_cast<InlineCache&>(chunk_.inlineCaches[c]);
                        if (ic.cachedClassId != 0 && ic.cachedFieldIndex >= 0) {
                            HIRNode* obj = getBoxedRKNode(b);
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            builder_.createGuardIsClass(obj, fs, ic.cachedClassId);
                            auto offset = builder_.createInt32Constant(ic.cachedFieldIndex);
                            auto loadField = builder_.createLoadField(obj, offset);
                            setLocalSync(a, loadField);
                            regFuncName_[a] = chunk_.constants[ic.nameIdx].asString();
                        } else {
                            throw std::runtime_error("JIT Error: Unresolved property access.");
                        }
                        break;
                    }
                    case OpCode::SET_PROP: {
                        InlineCache& ic = const_cast<InlineCache&>(chunk_.inlineCaches[b]);
                        if (ic.cachedClassId != 0 && ic.cachedFieldIndex >= 0) {
                            HIRNode* obj = getBoxedRKNode(a);
                            HIRNode* val = getBoxedRKNode(c);
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            builder_.createGuardIsClass(obj, fs, ic.cachedClassId);
                            auto offset = builder_.createInt32Constant(ic.cachedFieldIndex);
                            builder_.createStoreField(obj, offset, val);
                        } else {
                            throw std::runtime_error("JIT Error: Unresolved property access.");
                        }
                        break;
                    }
                    case OpCode::JMP_TRUE:
                    case OpCode::JMP_FALSE:
                    case OpCode::JMP:
                    case OpCode::TRY_BEGIN:
                    case OpCode::TRY_END:
                    case OpCode::EXTRAARG:
                    case OpCode::SET_KW_ARGC:
                        break;

                    case OpCode::BUILD_LIST: {
                        auto fs = builder_.captureFrameState(currentIp, currentIp);
                        auto startRegNode = builder_.createInt32Constant(registerOffset_ + b);
                        auto countNode = builder_.createInt32Constant(c);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_list), JITType::TaggedValue, 2, {startRegNode, countNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::BUILD_DICT: {
                        auto fs = builder_.captureFrameState(currentIp, currentIp);
                        auto startRegNode = builder_.createInt32Constant(registerOffset_ + b);
                        auto countNode = builder_.createInt32Constant(c);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_dict), JITType::TaggedValue, 2, {startRegNode, countNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::BUILD_SET: {
                        auto fs = builder_.captureFrameState(currentIp, currentIp);
                        auto startRegNode = builder_.createInt32Constant(registerOffset_ + b);
                        auto countNode = builder_.createInt32Constant(c);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_set), JITType::TaggedValue, 2, {startRegNode, countNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::BUILD_MATRIX: {
                        auto fs = builder_.captureFrameState(currentIp, currentIp);
                        auto startRegNode = builder_.createInt32Constant(registerOffset_ + b);
                        auto shapeIdxNode = builder_.createInt32Constant(c);
                        auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_matrix), JITType::TaggedValue, 3, {startRegNode, shapeIdxNode, chunkNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::BUILD_SLICE: {
                        auto fs = builder_.captureFrameState(currentIp, currentIp);
                        auto startRegNode = builder_.createInt32Constant(registerOffset_ + b);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_slice), JITType::TaggedValue, 1, {startRegNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::CLASS: {
                        auto fs = builder_.captureFrameState(currentIp, currentIp);
                        auto nameIdxNode = builder_.createInt32Constant(bx);
                        auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_class), JITType::TaggedValue, 2, {nameIdxNode, chunkNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::BUILD_NAMESPACE: {
                        auto fs = builder_.captureFrameState(currentIp, currentIp);
                        auto startRegNode = builder_.createInt32Constant(registerOffset_ + a + 1);
                        auto countNode = builder_.createInt32Constant(c);
                        auto nameIdxNode = builder_.createInt32Constant(b);
                        auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                        auto offsetNode = builder_.createInt32Constant(registerOffset_);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_namespace), JITType::TaggedValue, 5, {startRegNode, countNode, nameIdxNode, chunkNode, offsetNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::CONCAT_STRINGS: {
                        auto fs = builder_.captureFrameState(currentIp, currentIp);
                        auto startRegNode = builder_.createInt32Constant(registerOffset_ + b);
                        auto countNode = builder_.createInt32Constant(c);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_concat_strings), JITType::TaggedValue, 2, {startRegNode, countNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::FORMAT_STRING: {
                        auto fs = builder_.captureFrameState(currentIp, currentIp);
                        auto valRegNode = builder_.createInt32Constant(registerOffset_ + b);
                        auto specIdxNode = builder_.createInt32Constant(c);
                        auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_format_string), JITType::TaggedValue, 3, {valRegNode, specIdxNode, chunkNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::DICT_REST: {
                        auto fs = builder_.captureFrameState(currentIp, currentIp);
                        auto objRegNode = builder_.createInt32Constant(registerOffset_ + b);
                        auto excludeKeysRegNode = builder_.createInt32Constant(registerOffset_ + c);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_dict_rest), JITType::TaggedValue, 2, {objRegNode, excludeKeysRegNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::CLOSURE: {
                        auto fs = builder_.captureFrameState(currentIp, currentIp);
                        int fnIdx = static_cast<int>(std::round(chunk_.constants[bx].asDouble()));
                        auto fnIdxNode = builder_.createInt32Constant(fnIdx);
                        auto offsetNode = builder_.createInt32Constant(registerOffset_);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_closure), JITType::TaggedValue, 2, {fnIdxNode, offsetNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }

                    case OpCode::CALL:
                    case OpCode::TAIL_CALL: {
                        HIRNode* callee = getBoxedRKNode(b);
                        std::vector<HIRNode*> args;
                        for (int i = 0; i < c; ++i) {
                            args.push_back(getBoxedRKNode(b + 1 + i));
                        }
                        
                        std::string funcName = regFuncName_[b];
                        if (funcName.empty() && callee->opcode() == HIROp::StringConstant) {
                            funcName = static_cast<StringConstantNode*>(callee)->value();
                        }
                        
                        bool isMathIntrinsic = false;
                        bool allowInt32Promotion = false;
                        
                        if (funcName == "sqrtD" || funcName == "sin" || funcName == "cos") {
                            isMathIntrinsic = true;
                            allowInt32Promotion = true; // 这些函数语义上保证返回 Double，允许 Int32 提升
                        } else if (funcName == "sqrt" || funcName == "abs") {
                            isMathIntrinsic = true;
                            allowInt32Promotion = false; // 这些函数对 Int32 输入可能返回 SymExpr 或 BigInt，禁止提升
                        }
                        // 注意：floor, ceil, round, trunc 被移出内联名单。
                        // 因为它们在 JC2 中保证返回精确的 Int32 或 BigInt，
                        // 而 JIT 的硬件指令 (roundsd) 返回的是 Double，会导致类型突变。
                        
                        uint8_t fb = chunk_.typeFeedback[currentIp];

                        // Step 76: 尝试获取目标函数并判断是否内联
                        CompiledFunction* targetFn = nullptr;
                        if (!funcName.empty() && VM::activeVM) {
                            Value globalVal = VM::activeVM->getGlobal(funcName);
                            if (globalVal.isFunctionClosure()) {
                                ObjClosure* closure = globalVal.asFunction();
                                if (closure->isBytecode() && closure->compiledFnIndex >= 0) {
                                    auto& fns = VM::activeVM->getCompiledFunctions();
                                    if (closure->compiledFnIndex < static_cast<int>(fns.size())) {
                                        targetFn = fns[closure->compiledFnIndex].get();
                                    }
                                }
                            }
                        }

                        bool canInline = shouldInline(targetFn);

                        // Step 68: 识别目标是否为已知的 math 内置函数
                        // Step 69: 结合 Profiling 数据，如果参数为 Double，则将内置函数调用直接替换为对应的 HIR 数学节点。
                        // Step 70: 仅对语义安全的函数（如 sqrtD, sin）实现 Int32 -> Double 的自动类型提升。
                        if (isMathIntrinsic && c == 1 && (fb == 0x02 || (fb == 0x01 && allowInt32Promotion))) {
                            auto fs = builder_.captureFrameState(currentIp, currentIp);
                            HIRNode* unbox = nullptr;
                            if (fb == 0x02) {
                                auto guard = builder_.createGuardIsDouble(args[0], fs);
                                unbox = builder_.createUnboxDouble(args[0], guard);
                            } else {
                                auto guard = builder_.createGuardIsInt32(args[0], fs);
                                auto unboxInt = builder_.createUnboxInt32(args[0], guard);
                                unbox = builder_.createInt32ToDouble(unboxInt);
                            }
                            
                            HIRNode* mathNode = nullptr;
                            if (funcName == "sqrt" || funcName == "sqrtD") mathNode = builder_.createSqrtF64(unbox);
                            else if (funcName == "sin") mathNode = builder_.createSinF64(unbox);
                            else if (funcName == "cos") mathNode = builder_.createCosF64(unbox);
                            else if (funcName == "abs") mathNode = builder_.createAbsF64(unbox);
                            
                            auto boxedRes = builder_.createBoxDouble(mathNode);
                            if (op == OpCode::CALL) {
                                setLocalSync(a, boxedRes);
                            } else {
                                builder_.createReturn(boxedRes);
                            }
                        } else if (isMathIntrinsic) {
                            auto callNode = builder_.createCallBuiltin(callee, c, args);
                            if (op == OpCode::CALL) {
                                setLocalSync(a, callNode);
                            } else {
                                syncAllRegisters();
                                builder_.createReturn(callNode);
                            }
                        } else if (canInline) {
                            // Step 77: 实现内联展开逻辑
                            int newOffset = registerOffset_ + maxRegs_;
                            BytecodeToHIR inliner(targetFn->chunk, builder_, targetFn->localCount + targetFn->refCount, inlineDepth_ + 1, newOffset);
                            HIRNode* inlineRes = inliner.buildInline(args);
                            if (op == OpCode::CALL) {
                                setLocalSync(a, inlineRes);
                            } else {
                                syncAllRegisters();
                                builder_.createReturn(inlineRes);
                            }
                        } else {
                            syncAllRegisters();
                            auto callNode = builder_.createCall(callee, c, args);
                            if (op == OpCode::CALL) {
                                setLocalSync(a, callNode);
                            } else {
                                builder_.createReturn(callNode);
                            }
                        }
                        break;
                    }
                    default:
                        throw std::runtime_error("JIT Error: Unsupported opcode " + std::to_string(static_cast<int>(op)));
                }

                if (!builder_.currentControl()) break; // 控制流已终止，跳过基本块剩余指令
            }

            // 保存当前块的出口状态
            blockExitStates_[block.id] = builder_.getRegisters();

            // 处理 Fallthrough 控制流
            if (builder_.currentControl()) {
                int nextIp = block.endIp;
                if (cfg_.ipToBlockId.count(nextIp)) {
                    blockEntryControls_[cfg_.ipToBlockId[nextIp]].push_back(builder_.currentControl());
                }
            }

            // Step 56: 回边数据流绑定 (Back-edge Data Flow Binding)
            for (int succId : block.successors) {
                const auto& succBlock = cfg_.blocks[succId];
                if (succBlock.isLoopHeader) {
                    if (std::find(succBlock.backEdges.begin(), succBlock.backEdges.end(), block.id) != succBlock.backEdges.end()) {
                        // 1. 绑定控制流回边
                        // 回边控制流由 JMP 或 Fallthrough 提供，已经在 blockEntryControls_ 中了
                        // 但对于 LoopBegin 节点，我们需要直接将回边控制流加到它的 inputs 中
                        if (loopHeaderControls_.count(succId)) {
                            // 找到从当前块跳向 succId 的控制流节点
                            HIRNode* backEdgeCtrl = nullptr;
                            auto& entries = blockEntryControls_[succId];
                            if (!entries.empty()) {
                                backEdgeCtrl = entries.back();
                                entries.pop_back(); // 移除它，因为它直接连到 LoopBegin
                            }
                            if (backEdgeCtrl) {
                                loopHeaderControls_[succId]->addInput(backEdgeCtrl);
                            }
                        }
                        
                        // 2. 绑定数据流回边
                        if (loopHeaderPhis_.count(succId)) {
                            auto& phis = loopHeaderPhis_[succId];
                            for (int i = 0; i < maxRegs_; ++i) {
                                if (phis[i]) {
                                    HIRNode* backEdgeVal = blockExitStates_[block.id][i];
                                    if (backEdgeVal) {
                                        if (phis[i]->type() == JITType::TaggedValue && backEdgeVal->type() != JITType::TaggedValue) {
                                            if (backEdgeVal->type() == JITType::Int32) backEdgeVal = builder_.createBoxInt32(backEdgeVal);
                                            else if (backEdgeVal->type() == JITType::Double) backEdgeVal = builder_.createBoxDouble(backEdgeVal);
                                            else if (backEdgeVal->type() == JITType::Bool) backEdgeVal = builder_.createBoxBool(backEdgeVal);
                                        }
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

        if (isInline_) {
            if (returnControls_.empty()) {
                inlineResult_ = builder_.createNoneConstant();
            } else if (returnControls_.size() == 1) {
                builder_.setCurrentControl(returnControls_[0]);
                inlineResult_ = returnValues_[0];
            } else {
                HIRNode* merge = builder_.createMerge(returnControls_);
                builder_.setCurrentControl(merge);
                inlineResult_ = builder_.createPhi(JITType::TaggedValue, returnValues_);
            }
        }
    }

private:
    const Chunk& chunk_;
    HIRBuilder& builder_;
    int inlineDepth_;
    static constexpr int MAX_INLINE_DEPTH = 3;
    static constexpr size_t MAX_INLINE_INSTS = 50;

    bool shouldInline(const CompiledFunction* fn) const {
        if (inlineDepth_ >= MAX_INLINE_DEPTH) return false;
        if (!fn) return false;
        if (fn->chunk.code.size() > MAX_INLINE_INSTS) return false;
        
        // Step 87: 调整内联启发式算法
        // 拒绝内联包含异常处理或延迟执行的函数 (TRY_BEGIN, DEFER)
        // 复杂指令 (如 BUILD_MATRIX, CLASS, CLOSURE) 现在已通过 Callout 支持，允许内联！
        for (Instruction inst : fn->chunk.code) {
            OpCode op = GET_OPCODE(inst);
            if (op == OpCode::TRY_BEGIN || op == OpCode::DEFER) {
                return false;
            }
        }
        return true;
    }

    BytecodeCFG cfg_;
    std::map<int, std::vector<HIRNode*>> loopHeaderPhis_;
    std::map<int, HIRNode*> loopHeaderControls_;
    std::map<int, std::string> regFuncName_;
    std::map<int, std::vector<HIRNode*>> blockEntryControls_;
    std::map<int, std::vector<HIRNode*>> blockExitStates_;

    bool isInline_ = false;
    bool isOSR_ = false;
    int osrLoopHeaderIp_ = -1;
    std::vector<HIRNode*> inlineArgs_;
    HIRNode* inlineResult_ = nullptr;
    std::vector<HIRNode*> returnValues_;
    std::vector<HIRNode*> returnControls_;
    int maxRegs_ = 0;
    int registerOffset_ = 0;
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_BYTECODE_TO_HIR_H
