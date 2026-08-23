#ifndef JC2_JIT_BYTECODE_TO_HIR_H
#define JC2_JIT_BYTECODE_TO_HIR_H

#include "../ir/HIRBuilder.h"
#include "BytecodeCFG.h"
#include "../../vm/Bytecode.h"
#include "../../vm/VM.h"
#include <vector>
#include <stdexcept>
#include <atomic>
#include <set>
#include <map>
#include <algorithm>

namespace jc {
namespace jit {

// ============================================================================
// 字节码到 HIR 转换器 (抽象解释器) (Step 33)
// ============================================================================
class BytecodeToHIR {
public:
    BytecodeToHIR(const Chunk& chunk, HIRBuilder& builder, int maxRegs, int inlineDepth = 0, int registerOffset = 0)
        : chunk_(chunk), builder_(builder), maxRegs_(maxRegs), inlineDepth_(inlineDepth), registerOffset_(registerOffset) {}

    FrameStateNode* captureFrameState(int currentIp) {
        uint32_t bailoutId = builder_.generateBailoutId();
        return builder_.captureFrameState(bailoutId, currentIp);
    }

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

        // 2. 重新计算 RPO 和正确的循环回边 (基于 DFS)
        std::vector<int> rpo;
        std::vector<bool> visited(cfg_.blocks.size(), false);
        std::vector<bool> inStack(cfg_.blocks.size(), false);
        std::set<int> loopHeaders;
        std::map<int, std::vector<int>> backEdges; // succId -> list of predIds

        std::function<void(int)> dfs = [&](int blockId) {
            visited[blockId] = true;
            inStack[blockId] = true;
            for (int succId : cfg_.blocks[blockId].successors) {
                if (!visited[succId]) {
                    dfs(succId);
                } else if (inStack[succId] && cfg_.blocks[blockId].startIp > cfg_.blocks[succId].startIp) {
                    loopHeaders.insert(succId);
                    backEdges[succId].push_back(blockId);
                }
            }
            inStack[blockId] = false;
            rpo.push_back(blockId);
        };

        if (!cfg_.blocks.empty()) {
            int startBlockId = 0;
            if (isOSR_ && cfg_.ipToBlockId.count(osrLoopHeaderIp_)) {
                startBlockId = cfg_.ipToBlockId[osrLoopHeaderIp_];
            }
            dfs(startBlockId);
        }
        std::reverse(rpo.begin(), rpo.end());

        // 3. 初始化入口状态
        if (isInline_) {
            for (size_t i = 0; i < inlineArgs_.size(); ++i) {
                builder_.setLocal(registerOffset_ + i, inlineArgs_[i]);
            }
            blockEntryControls_[0].push_back(builder_.createStart());
            blockExitStates_[-1] = builder_.getRegisters(); // Dummy predecessor for inline
            blockExitEffects_[-1] = builder_.currentEffect();
        } else if (isOSR_) {
            auto osrEntry = builder_.createOSREntry(osrLoopHeaderIp_);
            for (int i = 0; i < maxRegs_; ++i) {
                auto load = builder_.createLoadRegister(registerOffset_ + i);
                builder_.setLocal(registerOffset_ + i, load);
            }
            int startBlockId = cfg_.ipToBlockId[osrLoopHeaderIp_];
            blockEntryControls_[startBlockId].push_back(osrEntry);
            blockExitStates_[-1] = builder_.getRegisters(); // Dummy predecessor for OSR
            blockExitEffects_[-1] = builder_.currentEffect();
        } else {
            auto startNode = builder_.createStart();
            for (int i = 0; i < maxRegs_; ++i) {
                auto load = builder_.createLoadRegister(registerOffset_ + i);
                builder_.setLocal(registerOffset_ + i, load);
            }
            blockEntryControls_[0].push_back(startNode);
            blockExitStates_[-1] = builder_.getRegisters(); // Dummy predecessor
            blockExitEffects_[-1] = builder_.currentEffect();
        }

        // 4. 抽象解释主循环：按 RPO 遍历基本块
        for (int blockId : rpo) {
            const auto& block = cfg_.blocks[blockId];
            if (block.startIp >= static_cast<int>(chunk_.code.size())) continue;
            if (isOSR_ && block.startIp < osrLoopHeaderIp_) continue;

            auto& entryControls = blockEntryControls_[block.id];
            if (entryControls.empty()) continue; // 不可达块

            bool isLoopHeader = loopHeaders.count(block.id);

            // 构建控制流汇合与 Phi 节点
            if (entryControls.size() > 1 || isLoopHeader) {
                HIRNode* merge = nullptr;
                if (isLoopHeader) {
                    merge = builder_.createLoopBegin(entryControls);
                    loopHeaderControls_[block.id] = merge;
                } else {
                    merge = builder_.createMerge(entryControls);
                }
                builder_.setCurrentControl(merge);

                std::vector<int> predsToCheck;
                if (block.id == 0 || (isOSR_ && block.startIp == osrLoopHeaderIp_)) {
                    predsToCheck.push_back(-1);
                }
                for (int p : block.predecessors) {
                    // OSR 编译时，位于 OSR 头之前的前驱边在 handleBranchTarget 中走 deopt 路径、
                    // 不会写入 entryControls，因此这里也要一并排除，保持 predsToCheck 与 entryControls 对齐。
                    if (isOSR_ && cfg_.blocks[p].startIp < osrLoopHeaderIp_) {
                        continue;
                    }
                    predsToCheck.push_back(p);
                }

                // 构建 Effect Phi
                std::vector<HIRNode*> effectInputs;
                bool needsEffectPhi = false;
                HIRNode* firstEffect = nullptr;
                for (int predId : predsToCheck) {
                    if (blockExitEffects_.count(predId)) {
                        HIRNode* eff = blockExitEffects_[predId];
                        if (!firstEffect) firstEffect = eff;
                        else if (firstEffect != eff) needsEffectPhi = true;
                        effectInputs.push_back(eff);
                    }
                }
                // 循环头始终需要 Effect Phi：回边 effect 会在 Step 56 里追加进来
                if (isLoopHeader) needsEffectPhi = true;
                if (needsEffectPhi) {
                    HIRNode* effectPhi = builder_.createPhi(JITType::Effect, effectInputs);
                    builder_.setCurrentEffect(effectPhi);
                    if (isLoopHeader) {
                        loopHeaderEffectPhis_[block.id] = effectPhi;
                    }
                } else if (firstEffect) {
                    builder_.setCurrentEffect(firstEffect);
                }

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
                            } else if (isLoopHeader) {
                                needsPhi = true; break;
                            }
                        }
                    };
                    
                    checkPreds(predsToCheck);

                    if (needsPhi) {
                        JITType type = JITType::Unknown;
                        std::vector<HIRNode*> phiInputs;
                        
                        auto addInputs = [&](const std::vector<int>& preds) {
                            // 第一次遍历：确定 Phi 节点的最终类型
                            for (int predId : preds) {
                                if (blockExitStates_.count(predId)) {
                                    HIRNode* val = blockExitStates_[predId][i];
                                    if (val) {
                                        if (type == JITType::TaggedValue) {
                                            // 已经是 TaggedValue，保持不变
                                        } else if (type == JITType::Unknown) {
                                            type = val->type();
                                        } else if (type != val->type()) {
                                            type = JITType::TaggedValue; // 类型冲突，提升为 TaggedValue
                                        }
                                    }
                                }
                            }
                            if (type == JITType::Unknown) type = JITType::TaggedValue;

                            // 第二次遍历：根据最终类型进行装箱
                            for (int predId : preds) {
                                if (blockExitStates_.count(predId)) {
                                    HIRNode* val = blockExitStates_[predId][i];
                                    if (val) {
                                        if (type == JITType::TaggedValue && val->type() != JITType::TaggedValue) {
                                            if (val->type() == JITType::Int32) val = builder_.createBoxInt32(val);
                                            else if (val->type() == JITType::Double) val = builder_.createBoxDouble(val);
                                            else if (val->type() == JITType::Bool) val = builder_.createBoxBool(val);
                                        }
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
                if (isLoopHeader) {
                    loopHeaderPhis_[block.id] = phis;
                }
            } else {
                builder_.setCurrentControl(entryControls[0]);
                int predId = -1;
                if (!block.predecessors.empty()) {
                    for (int p : block.predecessors) {
                        if (blockExitStates_.count(p)) { predId = p; break; }
                    }
                    if (predId == -1) predId = block.predecessors[0];
                }
                if (block.id == 0 || (isOSR_ && block.startIp == osrLoopHeaderIp_)) predId = -1;
                
                if (blockExitStates_.count(predId)) {
                    builder_.setRegisters(blockExitStates_[predId]);
                }
                if (blockExitEffects_.count(predId)) {
                    builder_.setCurrentEffect(blockExitEffects_[predId]);
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
                // 转义操作数的完整索引可能 >= 128（与 RK 的 0x80 标志位冲突），
                // 用 bit 24/25 作为转义标记，低 24 位承载完整索引，避免 INDEXK 截断。
                constexpr int RK_ESCAPED_CONST = 0x1000000;
                constexpr int RK_ESCAPED_REG  = 0x2000000;

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
                        if (b == ESCAPE_KBIT_CONST) b = fetchExtra() | RK_ESCAPED_CONST;
                        else if (b == ESCAPE_KBIT_REG) b = fetchExtra() | RK_ESCAPED_REG;
                        if (c == ESCAPE_KBIT_CONST) c = fetchExtra() | RK_ESCAPED_CONST;
                        else if (c == ESCAPE_KBIT_REG) c = fetchExtra() | RK_ESCAPED_REG;
                        break;

                    case OpCode::JMP_TRUE:
                    case OpCode::JMP_FALSE:
                        if (a == ESCAPE_NORMAL_8) a = fetchExtra();
                        break;

                    case OpCode::JMP:
                        break;

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
                    auto resolveConst = [&](int idx) -> HIRNode* {
                        const Value& kst = chunk_.constants[idx];
                        if (kst.isInt32()) return builder_.createBoxInt32(builder_.createInt32Constant(kst.asInt32()));
                        if (kst.isDouble()) return builder_.createBoxDouble(builder_.createDoubleConstant(kst.asDoubleRaw()));
                        if (kst.isBool()) return builder_.createBoxBool(builder_.createBoolConstant(kst.asBool()));
                        if (kst.isNone()) return builder_.createNoneConstant();
                        return builder_.createInt64Constant(kst.as_bits);
                    };
                    auto resolveReg = [&](int reg) -> HIRNode* {
                        HIRNode* node = builder_.getLocal(registerOffset_ + reg);
                        if (!node) {
                            node = builder_.createLoadRegister(registerOffset_ + reg);
                            builder_.setLocal(registerOffset_ + reg, node);
                        }
                        return node;
                    };
                    if (rk & RK_ESCAPED_CONST) return resolveConst(rk & 0xFFFFFF);
                    if (rk & RK_ESCAPED_REG) return resolveReg(rk & 0xFFFFFF);
                    if (ISK(rk)) return resolveConst(INDEXK(rk));
                    return resolveReg(rk);
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
                        } else if (kst.isNone()) {
                            node = builder_.createNoneConstant();
                        } else {
                            node = builder_.createInt64Constant(kst.as_bits);
                        }
                        
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
                            auto fs = captureFrameState(currentIp);
                            auto icIdxNode = builder_.createInt32Constant(bx);
                            auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                            auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_get_global), JITType::TaggedValue, 2, {icIdxNode, chunkNode}, fs);
                            setLocalSync(a, callout);
                            regFuncName_[a] = chunk_.constants[ic.nameIdx].asString();
                        }
                        break;
                    }
                    case OpCode::SET_GLOBAL: {
                        InlineCache& ic = const_cast<InlineCache&>(chunk_.inlineCaches[bx]);
                        HIRNode* val = builder_.getLocal(registerOffset_ + a);
                        if (val->type() != JITType::TaggedValue) {
                            if (val->type() == JITType::Int32) val = builder_.createBoxInt32(val);
                            else if (val->type() == JITType::Double) val = builder_.createBoxDouble(val);
                            else if (val->type() == JITType::Bool) val = builder_.createBoxBool(val);
                        }
                        if (ic.cachedGlobalSlot >= 0) {
                            builder_.createStoreGlobal(ic.cachedGlobalSlot, val);
                        } else {
                            auto fs = captureFrameState(currentIp);
                            auto icIdxNode = builder_.createInt32Constant(bx);
                            auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                            auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_set_global), JITType::Effect, 3, {icIdxNode, val, chunkNode}, fs);
                            builder_.setCurrentEffect(callout);
                        }
                        break;
                    }
                    case OpCode::ADD:
                    case OpCode::SUB:
                    case OpCode::MUL:
                    case OpCode::DIV:
                    case OpCode::IDIV:
                    case OpCode::MOD:
                    case OpCode::POW:
                    case OpCode::LDIV: {
                        uint8_t fb = chunk_.typeFeedback[currentIp];
                        HIRNode* lhs = getRKNode(b);
                        HIRNode* rhs = getRKNode(c);
                        
                        if (fb == 0x01 && op != OpCode::POW && op != OpCode::LDIV) { // Monomorphic Int32 (纯 32 位整数)
                            auto fs = captureFrameState(currentIp);
                            auto guardL = builder_.createGuardIsInt32(lhs, fs);
                            auto guardR = builder_.createGuardIsInt32(rhs, fs);
                            auto unboxL = builder_.createUnboxInt32(lhs, guardL);
                            auto unboxR = builder_.createUnboxInt32(rhs, guardR);
                            
                            HIRNode* opNode = nullptr;
                            if (op == OpCode::ADD) opNode = builder_.createAddI32(unboxL, unboxR, fs);
                            else if (op == OpCode::SUB) opNode = builder_.createSubI32(unboxL, unboxR, fs);
                            else if (op == OpCode::MUL) opNode = builder_.createMulI32(unboxL, unboxR, fs);
                            else if (op == OpCode::DIV) opNode = builder_.createDivI32(unboxL, unboxR, fs);
                            else if (op == OpCode::IDIV) opNode = builder_.createIDivI32(unboxL, unboxR, fs);
                            else if (op == OpCode::MOD) opNode = builder_.createModI32(unboxL, unboxR, fs);
                            
                            setLocalSync(a, builder_.createBoxInt32(opNode));
                            
                        } else if (fb == 0x02 && op != OpCode::POW && op != OpCode::LDIV) { // Monomorphic Double (纯 64 位浮点数)
                            auto fs = captureFrameState(currentIp);
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
                            auto fs = captureFrameState(currentIp);
                            void* fnPtr = nullptr;
                            if (op == OpCode::ADD) fnPtr = reinterpret_cast<void*>(jc2_jit_arith_add);
                            else if (op == OpCode::SUB) fnPtr = reinterpret_cast<void*>(jc2_jit_arith_sub);
                            else if (op == OpCode::MUL) fnPtr = reinterpret_cast<void*>(jc2_jit_arith_mul);
                            else if (op == OpCode::DIV) fnPtr = reinterpret_cast<void*>(jc2_jit_arith_div);
                            else if (op == OpCode::IDIV) fnPtr = reinterpret_cast<void*>(jc2_jit_arith_idiv);
                            else if (op == OpCode::MOD) fnPtr = reinterpret_cast<void*>(jc2_jit_arith_mod);
                            else if (op == OpCode::POW) fnPtr = reinterpret_cast<void*>(jc2_jit_arith_pow);
                            else if (op == OpCode::LDIV) fnPtr = reinterpret_cast<void*>(jc2_jit_arith_ldiv);
                            
                            auto callout = builder_.createCallout(fnPtr, JITType::TaggedValue, 2, {getBoxedRKNode(b), getBoxedRKNode(c)}, fs);
                            setLocalSync(a, callout);
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
                            auto fs = captureFrameState(currentIp);
                            auto guardL = builder_.createGuardIsInt32(lhs, fs);
                            auto guardR = builder_.createGuardIsInt32(rhs, fs);
                            auto unboxL = builder_.createUnboxInt32(lhs, guardL);
                            auto unboxR = builder_.createUnboxInt32(rhs, guardR);
                            
                            HIRNode* opNode = nullptr;
                            if (op == OpCode::BAND) opNode = builder_.createBitAndI32(unboxL, unboxR);
                            else if (op == OpCode::BOR) opNode = builder_.createBitOrI32(unboxL, unboxR);
                            else if (op == OpCode::BXOR) opNode = builder_.createBitXorI32(unboxL, unboxR);
                            else if (op == OpCode::SHL) opNode = builder_.createShlI32(unboxL, unboxR, fs);
                            else if (op == OpCode::SHR) opNode = builder_.createShrI32(unboxL, unboxR, fs);
                            
                            setLocalSync(a, builder_.createBoxInt32(opNode));
                        } else {
                            auto fs = captureFrameState(currentIp);
                            void* fnPtr = nullptr;
                            if (op == OpCode::BAND) fnPtr = reinterpret_cast<void*>(jc2_jit_bitwise_and);
                            else if (op == OpCode::BOR) fnPtr = reinterpret_cast<void*>(jc2_jit_bitwise_or);
                            else if (op == OpCode::BXOR) fnPtr = reinterpret_cast<void*>(jc2_jit_bitwise_xor);
                            else if (op == OpCode::SHL) fnPtr = reinterpret_cast<void*>(jc2_jit_bitwise_shl);
                            else if (op == OpCode::SHR) fnPtr = reinterpret_cast<void*>(jc2_jit_bitwise_shr);
                            
                            auto callout = builder_.createCallout(fnPtr, JITType::TaggedValue, 2, {getBoxedRKNode(b), getBoxedRKNode(c)}, fs);
                            setLocalSync(a, callout);
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
                            auto fs = captureFrameState(currentIp);
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
                            auto fs = captureFrameState(currentIp);
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
                            auto fs = captureFrameState(currentIp);
                            void* fnPtr = nullptr;
                            if (op == OpCode::EQ) fnPtr = reinterpret_cast<void*>(jc2_jit_cmp_eq);
                            else if (op == OpCode::NEQ) fnPtr = reinterpret_cast<void*>(jc2_jit_cmp_neq);
                            else if (op == OpCode::LT) fnPtr = reinterpret_cast<void*>(jc2_jit_cmp_lt);
                            else if (op == OpCode::LE) fnPtr = reinterpret_cast<void*>(jc2_jit_cmp_le);
                            else if (op == OpCode::GT) fnPtr = reinterpret_cast<void*>(jc2_jit_cmp_gt);
                            else if (op == OpCode::GE) fnPtr = reinterpret_cast<void*>(jc2_jit_cmp_ge);
                            
                            auto callout = builder_.createCallout(fnPtr, JITType::TaggedValue, 2, {getBoxedRKNode(b), getBoxedRKNode(c)}, fs);
                            setLocalSync(a, callout);
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
                            auto fs = captureFrameState(currentIp);
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
                            auto fs = captureFrameState(currentIp);
                            auto guard = builder_.createGuardIsDouble(val, fs);
                            auto unbox = builder_.createUnboxDouble(val, guard);
                            setLocalSync(a, builder_.createBoxDouble(builder_.createNegF64(unbox)));
                        } else if (fb == 0x08 && (op == OpCode::NOT || op == OpCode::TO_BOOL)) {
                            auto fs = captureFrameState(currentIp);
                            auto guard = builder_.createGuardIsBool(val, fs);
                            auto unbox = builder_.createUnboxBool(val, guard);
                            if (op == OpCode::NOT) {
                                auto falseNode = builder_.createBoolConstant(false);
                                setLocalSync(a, builder_.createBoxBool(builder_.createCmpEqI32(unbox, falseNode)));
                            } else {
                                setLocalSync(a, val);
                            }
                        } else if (op == OpCode::UNM || op == OpCode::BNOT) {
                            auto fs = captureFrameState(currentIp);
                            void* fnPtr = (op == OpCode::UNM) ? reinterpret_cast<void*>(jc2_jit_unary_unm) : reinterpret_cast<void*>(jc2_jit_unary_bnot);
                            auto callout = builder_.createCallout(fnPtr, JITType::TaggedValue, 1, {getBoxedRKNode(b)}, fs);
                            setLocalSync(a, callout);
                        } else {
                            auto fs = captureFrameState(currentIp);
                            auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_truthy), JITType::Bool, 1, {val}, fs);
                            if (op == OpCode::NOT) {
                                auto falseNode = builder_.createBoolConstant(false);
                                setLocalSync(a, builder_.createBoxBool(builder_.createCmpEqI32(callout, falseNode)));
                            } else {
                                setLocalSync(a, builder_.createBoxBool(callout));
                            }
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
                            builder_.createReturn(retVal);
                        }
                        break;
                    }
                    case OpCode::GET_PROP: {
                        InlineCache& ic = const_cast<InlineCache&>(chunk_.inlineCaches[c]);
                        auto fs = captureFrameState(currentIp);
                        HIRNode* objVal = getBoxedRKNode(b);
                        auto icIdxNode = builder_.createInt32Constant(c);
                        auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_get_prop), JITType::TaggedValue, 3, {objVal, icIdxNode, chunkNode}, fs);
                        setLocalSync(a, callout);
                        regFuncName_[a] = chunk_.constants[ic.nameIdx].asString();
                        break;
                    }
                    case OpCode::TRY_GET_PROP: {
                        InlineCache& ic = const_cast<InlineCache&>(chunk_.inlineCaches[c]);
                        auto fs = captureFrameState(currentIp);
                        HIRNode* objVal = getBoxedRKNode(b);
                        auto icIdxNode = builder_.createInt32Constant(c);
                        auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_try_get_prop), JITType::TaggedValue, 3, {objVal, icIdxNode, chunkNode}, fs);
                        setLocalSync(a, callout);
                        regFuncName_[a] = chunk_.constants[ic.nameIdx].asString();
                        break;
                    }
                    case OpCode::SET_PROP: {
                        auto fs = captureFrameState(currentIp);
                        HIRNode* objVal = getBoxedRKNode(a);
                        HIRNode* valVal = getBoxedRKNode(c);
                        auto icIdxNode = builder_.createInt32Constant(b);
                        auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_set_prop), JITType::Effect, 4, {objVal, valVal, icIdxNode, chunkNode}, fs);
                        builder_.setCurrentEffect(callout);
                        break;
                    }
                    case OpCode::JMP_TRUE:
                    case OpCode::JMP_FALSE: {
                        uint8_t fb = chunk_.typeFeedback[currentIp];
                        HIRNode* val = builder_.getLocal(registerOffset_ + a);
                        HIRNode* condNode = nullptr;
                        
                        if (fb == 0x01) {
                            auto fs = captureFrameState(currentIp);
                            auto guard = builder_.createGuardIsInt32(val, fs);
                            auto unbox = builder_.createUnboxInt32(val, guard);
                            auto zero = builder_.createInt32Constant(0);
                            condNode = builder_.createCmpNeqI32(unbox, zero);
                        } else if (fb == 0x08) {
                            auto fs = captureFrameState(currentIp);
                            auto guard = builder_.createGuardIsBool(val, fs);
                            condNode = builder_.createUnboxBool(val, guard);
                        } else {
                            auto fs = captureFrameState(currentIp);
                            condNode = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_truthy), JITType::Bool, 1, {val}, fs);
                        }
                        
                        auto branch = builder_.createBranch(condNode);
                        auto ifTrue = builder_.createIfTrue(branch);
                        auto ifFalse = builder_.createIfFalse(branch);
                        
                        int trueTargetIp = (op == OpCode::JMP_TRUE) ? (ip + sbx) : ip;
                        int falseTargetIp = (op == OpCode::JMP_FALSE) ? (ip + sbx) : ip;
                        
                        auto handleBranchTarget = [&](HIRNode* branchCtrl, int targetIp) {
                            if (isOSR_ && targetIp < osrLoopHeaderIp_) {
                                builder_.setCurrentControl(branchCtrl);
                                if (blockExitStates_.count(-1)) {
                                    builder_.setRegisters(blockExitStates_[-1]);
                                }
                                auto fs = captureFrameState(currentIp);
                                builder_.createDeoptimize(fs);
                            } else if (cfg_.ipToBlockId.count(targetIp)) {
                                blockEntryControls_[cfg_.ipToBlockId[targetIp]].push_back(branchCtrl);
                            }
                        };
                        
                        if (trueTargetIp == falseTargetIp) {
                            // 两个分支汇合到同一点（如 JMP_FALSE 偏移 0）：合并为单个 Merge 控制流，
                            // 保持 blockEntryControls_ 数量与 CFG predecessors（去重后）一致，避免 phi 与 Merge 输入数错位。
                            auto merge = builder_.createMerge({ifTrue, ifFalse});
                            handleBranchTarget(merge, trueTargetIp);
                        } else {
                            handleBranchTarget(ifTrue, trueTargetIp);
                            handleBranchTarget(ifFalse, falseTargetIp);
                        }
                        builder_.setCurrentControl(nullptr);
                        break;
                    }

                    case OpCode::JMP: {
                        int targetIp = ip + sax;
                        if (isOSR_ && targetIp < osrLoopHeaderIp_) {
                            // OSR 退出：deopt 点快照当前寄存器状态（数据流分析已保证正确性）。
                            // 不能用 OSR 入口快照（blockExitStates_[-1]），否则循环体内已更新的值
                            // （如 x += STRIP_WIDTH）会被回退成入口旧值，deopt 恢复后读到错误状态。
                            auto fs = captureFrameState(currentIp);
                            builder_.createDeoptimize(fs);
                        } else if (cfg_.ipToBlockId.count(targetIp)) {
                            blockEntryControls_[cfg_.ipToBlockId[targetIp]].push_back(builder_.currentControl());
                        }
                        builder_.setCurrentControl(nullptr);
                        break;
                    }

                    case OpCode::TRY_BEGIN:
                    case OpCode::TRY_END:
                    case OpCode::EXTRAARG:
                    case OpCode::SET_KW_ARGC:
                    case OpCode::PASS_REFS:
                        break;

                    case OpCode::BUILD_LIST: {
                        auto fs = captureFrameState(currentIp);
                        auto countNode = builder_.createInt32Constant(c);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_list), JITType::TaggedValue, 1, {countNode}, fs);
                        for (int i = 0; i < c; ++i) callout->addInput(getBoxedRKNode(b + i));
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::BUILD_DICT: {
                        auto fs = captureFrameState(currentIp);
                        auto countNode = builder_.createInt32Constant(c);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_dict), JITType::TaggedValue, 1, {countNode}, fs);
                        for (int i = 0; i < c * 2; ++i) callout->addInput(getBoxedRKNode(b + i));
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::BUILD_SET: {
                        auto fs = captureFrameState(currentIp);
                        auto countNode = builder_.createInt32Constant(c);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_set), JITType::TaggedValue, 1, {countNode}, fs);
                        for (int i = 0; i < c; ++i) callout->addInput(getBoxedRKNode(b + i));
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::INDEX_GET: {
                        bool noThrow = (c & 0x80) != 0;
                        int dims = c & 0x7F;
                        auto fs = captureFrameState(currentIp);
                        // 值 callout：直接传 obj 和索引的值，避免读 stale 的物理寄存器（同 IS_UNINIT 修复）
                        HIRNode* objVal = getBoxedRKNode(b);
                        HIRNode* a0Val = (dims > 0) ? getBoxedRKNode(b + 1) : builder_.createNoneConstant();
                        HIRNode* a1Val = (dims > 1) ? getBoxedRKNode(b + 2) : builder_.createNoneConstant();
                        auto dimsNode = builder_.createInt32Constant(dims);
                        auto noThrowNode = builder_.createInt32Constant(noThrow ? 1 : 0);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_index_get), JITType::TaggedValue, 5, {objVal, a0Val, a1Val, dimsNode, noThrowNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::INDEX_SET: {
                        int dims = c;
                        auto fs = captureFrameState(currentIp);
                        // 值 callout：直接传 obj、索引、val 的值，避免读 stale 的物理寄存器。
                        // copy-on-write 产生的新矩阵通过返回值 + setLocalSync 回传（callout 内部不再写回物理寄存器）。
                        HIRNode* objVal = getBoxedRKNode(a);
                        HIRNode* a0Val = (dims > 0) ? getBoxedRKNode(a + 1) : builder_.createNoneConstant();
                        HIRNode* a1Val = (dims > 1) ? getBoxedRKNode(a + 2) : builder_.createNoneConstant();
                        HIRNode* valVal = getBoxedRKNode(a + c + 1);
                        auto dimsNode = builder_.createInt32Constant(dims);
                        auto objRegNode = builder_.createInt32Constant(registerOffset_ + a);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_index_set), JITType::TaggedValue, 6, {objVal, a0Val, a1Val, valVal, dimsNode, objRegNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::BUILD_MATRIX: {
                        auto fs = captureFrameState(currentIp);
                        auto shapeIdxNode = builder_.createInt32Constant(c);
                        auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                        
                        const auto& shape = chunk_.matrixShapes[c];
                        int total = 0;
                        for (uint16_t cols : shape.rowCols) total += cols;
                        auto totalNode = builder_.createInt32Constant(total);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_matrix), JITType::TaggedValue, 3, {totalNode, shapeIdxNode, chunkNode}, fs);
                        for (int i = 0; i < total; ++i) callout->addInput(getBoxedRKNode(b + i));
                        
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::BUILD_SLICE: {
                        auto fs = captureFrameState(currentIp);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_slice), JITType::TaggedValue, 3, {getBoxedRKNode(b), getBoxedRKNode(b + 1), getBoxedRKNode(b + 2)}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::CLASS: {
                        auto fs = captureFrameState(currentIp);
                        auto nameIdxNode = builder_.createInt32Constant(bx);
                        auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_class), JITType::TaggedValue, 2, {nameIdxNode, chunkNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::BUILD_NAMESPACE: {
                        auto fs = captureFrameState(currentIp);
                        auto countNode = builder_.createInt32Constant(c);
                        auto nameIdxNode = builder_.createInt32Constant(b);
                        auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_build_namespace), JITType::TaggedValue, 3, {countNode, nameIdxNode, chunkNode}, fs);
                        for (int i = 0; i < c * 3; ++i) callout->addInput(getBoxedRKNode(a + 1 + i));
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::CONCAT_STRINGS: {
                        auto fs = captureFrameState(currentIp);
                        auto countNode = builder_.createInt32Constant(c);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_concat_strings), JITType::TaggedValue, 1, {countNode}, fs);
                        for (int i = 0; i < c; ++i) callout->addInput(getBoxedRKNode(b + i));
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::FORMAT_STRING: {
                        auto fs = captureFrameState(currentIp);
                        HIRNode* valVal = getBoxedRKNode(b);
                        auto specIdxNode = builder_.createInt32Constant(c);
                        auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_format_string), JITType::TaggedValue, 3, {valVal, specIdxNode, chunkNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::DICT_REST: {
                        auto fs = captureFrameState(currentIp);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_dict_rest), JITType::TaggedValue, 2, {getBoxedRKNode(b), getBoxedRKNode(c)}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::DICT_INIT: {
                        auto fs = captureFrameState(currentIp);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_dict_init), JITType::TaggedValue, 0, {}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::DICT_APPEND: {
                        auto fs = captureFrameState(currentIp);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_dict_append), JITType::Effect, 3, {getBoxedRKNode(a), getBoxedRKNode(b), getBoxedRKNode(c)}, fs);
                        builder_.setCurrentEffect(callout);
                        break;
                    }
                    case OpCode::GET_SELF: {
                        auto fs = captureFrameState(currentIp);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_get_self), JITType::TaggedValue, 0, {}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::GET_CURRENT_CLOSURE: {
                        auto fs = captureFrameState(currentIp);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_get_current_closure), JITType::TaggedValue, 0, {}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::GET_UPVAL: {
                        auto fs = captureFrameState(currentIp);
                        auto uvIdxNode = builder_.createInt32Constant(b);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_get_upval), JITType::TaggedValue, 1, {uvIdxNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::SET_UPVAL: {
                        auto fs = captureFrameState(currentIp);
                        auto uvIdxNode = builder_.createInt32Constant(b);
                        HIRNode* val = builder_.getLocal(registerOffset_ + a);
                        if (val->type() != JITType::TaggedValue) {
                            if (val->type() == JITType::Int32) val = builder_.createBoxInt32(val);
                            else if (val->type() == JITType::Double) val = builder_.createBoxDouble(val);
                            else if (val->type() == JITType::Bool) val = builder_.createBoxBool(val);
                        }
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_set_upval), JITType::Effect, 2, {uvIdxNode, val}, fs);
                        builder_.setCurrentEffect(callout);
                        break;
                    }
                    case OpCode::CLOSURE: {
                        auto fs = captureFrameState(currentIp);
                        int fnIdx = static_cast<int>(std::round(chunk_.constants[bx].asDouble()));
                        auto fnIdxNode = builder_.createInt32Constant(fnIdx);
                        auto offsetNode = builder_.createInt32Constant(registerOffset_);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_closure), JITType::TaggedValue, 2, {fnIdxNode, offsetNode}, fs);
                        
                        if (VM::activeVM && fnIdx >= 0 && fnIdx < static_cast<int>(VM::activeVM->getCompiledFunctions().size())) {
                            auto& fn = VM::activeVM->getCompiledFunctions()[fnIdx];
                            for (const auto& uv : fn->upvalues) {
                                if (uv.isLocal) {
                                    callout->addInput(getBoxedRKNode(uv.index));
                                }
                            }
                        }
                        
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
                            auto fs = captureFrameState(currentIp);
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
                            auto fs = captureFrameState(currentIp);
                            auto callNode = builder_.createCallBuiltin(callee, c, args, fs);
                            if (op == OpCode::CALL) {
                                setLocalSync(a, callNode);
                            } else {
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
                                builder_.createReturn(inlineRes);
                            }
                        } else {
                            auto fs = captureFrameState(currentIp);
                            auto callNode = builder_.createCall(callee, c, args, fs);
                            if (op == OpCode::CALL) {
                                setLocalSync(a, callNode);
                            } else {
                                builder_.createReturn(callNode);
                            }
                        }
                        break;
                    }
                    case OpCode::INVOKE:
                    case OpCode::TAIL_INVOKE:
                    case OpCode::INVOKE_PRIVATE:
                    case OpCode::TAIL_INVOKE_PRIVATE:
                    case OpCode::INVOKE_FALLBACK:
                    case OpCode::TAIL_INVOKE_FALLBACK: {
                        auto fs = captureFrameState(currentIp);
                        
                        int isPrivate = (op == OpCode::INVOKE_PRIVATE || op == OpCode::TAIL_INVOKE_PRIVATE) ? 1 : 0;
                        int fbType = (op == OpCode::INVOKE_FALLBACK || op == OpCode::TAIL_INVOKE_FALLBACK) ? 1 : -1;
                        
                        JitInvokeInfo info{ (uint32_t)(registerOffset_ + a), (uint32_t)b, (uint32_t)c, (uint32_t)isPrivate, fbType, &chunk_ };
                        jitInvokeInfos().push_back(info);
                        const JitInvokeInfo* infoPtr = &jitInvokeInfos().back();
                        auto infoPtrNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(infoPtr));
                        
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_invoke), JITType::TaggedValue, 1, {infoPtrNode}, fs);
                        
                        callout->addInput(getBoxedRKNode(a));
                        for (int i = 0; i < b; ++i) {
                            callout->addInput(getBoxedRKNode(a + 1 + i));
                        }
                        if (fbType == 1) {
                            callout->addInput(getBoxedRKNode(a + 1 + b)); // fallback value
                        }
                        
                        if (op == OpCode::INVOKE || op == OpCode::INVOKE_PRIVATE || op == OpCode::INVOKE_FALLBACK) {
                            setLocalSync(a, callout);
                        } else {
                            builder_.createReturn(callout);
                        }
                        break;
                    }
                    case OpCode::SUPER_INVOKE:
                    case OpCode::TAIL_SUPER_INVOKE: {
                        auto fs = captureFrameState(currentIp);
                        
                        JitSuperInvokeInfo info{ (uint32_t)(registerOffset_ + a), (uint32_t)b, (uint32_t)c, &chunk_ };
                        jitSuperInvokeInfos().push_back(info);
                        const JitSuperInvokeInfo* infoPtr = &jitSuperInvokeInfos().back();
                        auto infoPtrNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(infoPtr));
                        
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_super_invoke), JITType::TaggedValue, 1, {infoPtrNode}, fs);
                        
                        callout->addInput(getBoxedRKNode(a));
                        for (int i = 0; i < b; ++i) {
                            callout->addInput(getBoxedRKNode(a + 1 + i));
                        }
                        
                        if (op == OpCode::SUPER_INVOKE) {
                            setLocalSync(a, callout);
                        } else {
                            builder_.createReturn(callout);
                        }
                        break;
                    }
                    case OpCode::GET_SUPER: {
                        auto fs = captureFrameState(currentIp);
                        HIRNode* objVal = getBoxedRKNode(b);
                        auto nameIdxNode = builder_.createInt32Constant(c);
                        auto chunkNode = builder_.createInt64Constant(reinterpret_cast<uint64_t>(&chunk_));
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_get_super), JITType::TaggedValue, 3, {objVal, nameIdxNode, chunkNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::GET_REF_PARAM: {
                        auto fs = captureFrameState(currentIp);
                        auto bxNode = builder_.createInt32Constant(bx);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_get_ref_param), JITType::TaggedValue, 1, {bxNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::SET_REF_PARAM: {
                        auto fs = captureFrameState(currentIp);
                        auto bxNode = builder_.createInt32Constant(bx);
                        HIRNode* val = builder_.getLocal(registerOffset_ + a);
                        if (val->type() != JITType::TaggedValue) {
                            if (val->type() == JITType::Int32) val = builder_.createBoxInt32(val);
                            else if (val->type() == JITType::Double) val = builder_.createBoxDouble(val);
                            else if (val->type() == JITType::Bool) val = builder_.createBoxBool(val);
                        }
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_set_ref_param), JITType::Effect, 2, {bxNode, val}, fs);
                        builder_.setCurrentEffect(callout);
                        break;
                    }
                    case OpCode::IS_UNINIT: {
                        auto fs = captureFrameState(currentIp);
                        HIRNode* val = getBoxedRKNode(b);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_is_uninit), JITType::Bool, 1, {val}, fs);
                        setLocalSync(a, builder_.createBoxBool(callout));
                        break;
                    }
                    case OpCode::ITER_INIT: {
                        auto fs = captureFrameState(currentIp);
                        HIRNode* iterableVal = getBoxedRKNode(b);
                        auto cNode = builder_.createInt32Constant(c);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_iter_init), JITType::TaggedValue, 2, {iterableVal, cNode}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::ITER_NEXT: {
                        auto fs = captureFrameState(currentIp);
                        HIRNode* stateVal = getBoxedRKNode(b);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_iter_next), JITType::TaggedValue, 1, {stateVal}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::ASSERT_PARAM_TYPE: {
                        auto fs = captureFrameState(currentIp);
                        HIRNode* aVal = getBoxedRKNode(a);
                        auto bNode = builder_.createInt32Constant(b);
                        auto cNode = builder_.createInt32Constant(c);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_assert_param_type), JITType::Effect, 3, {aVal, bNode, cNode}, fs);
                        builder_.setCurrentEffect(callout);
                        break;
                    }
                    case OpCode::IN: {
                        auto fs = captureFrameState(currentIp);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_in), JITType::TaggedValue, 2, {getBoxedRKNode(b), getBoxedRKNode(c)}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::IMPORT: {
                        auto fs = captureFrameState(currentIp);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_import), JITType::TaggedValue, 1, {getBoxedRKNode(b)}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    case OpCode::MATCH_TYPE: {
                        auto fs = captureFrameState(currentIp);
                        auto callout = builder_.createCallout(reinterpret_cast<void*>(jc2_jit_match_type), JITType::TaggedValue, 2, {getBoxedRKNode(b), getBoxedRKNode(c)}, fs);
                        setLocalSync(a, callout);
                        break;
                    }
                    default:
                        throw std::runtime_error("JIT Error: Unsupported opcode " + std::to_string(static_cast<int>(op)));
                }

                if (!builder_.currentControl()) break; // 控制流已终止，跳过基本块剩余指令
            }

            // 保存当前块的出口状态
            blockExitStates_[block.id] = builder_.getRegisters();
            blockExitEffects_[block.id] = builder_.currentEffect();

            // 处理 Fallthrough 控制流
            if (builder_.currentControl()) {
                int nextIp = block.endIp;
                if (cfg_.ipToBlockId.count(nextIp)) {
                    blockEntryControls_[cfg_.ipToBlockId[nextIp]].push_back(builder_.currentControl());
                }
            }

            // Step 56: 回边数据流绑定 (Back-edge Data Flow Binding)
            for (int succId : block.successors) {
                if (loopHeaders.count(succId)) {
                    auto& edges = backEdges[succId];
                    if (std::find(edges.begin(), edges.end(), block.id) != edges.end()) {
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
                        
                        // 3. 绑定 Effect 回边
                        if (loopHeaderEffectPhis_.count(succId)) {
                            HIRNode* effectPhi = loopHeaderEffectPhis_[succId];
                            HIRNode* backEdgeEffect = blockExitEffects_[block.id];
                            if (backEdgeEffect) {
                                effectPhi->addInput(backEdgeEffect);
                            } else {
                                effectPhi->addInput(builder_.createStart()); // Fallback
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
        (void)fn;
        // 暂时完全禁用内联，直到实现完整的 Inline Deoptimization (CallFrame 重建)
        // 否则内联函数内部的去优化会导致 IP 损坏和重复执行副作用。
        // 此外，目前的内联器尚未正确映射 GET_REF_PARAM/SET_REF_PARAM 到调用方的实际参数。
        return false;
    }

    BytecodeCFG cfg_;
    std::map<int, std::vector<HIRNode*>> loopHeaderPhis_;
    std::map<int, HIRNode*> loopHeaderEffectPhis_;
    std::map<int, HIRNode*> loopHeaderControls_;
    std::map<int, std::string> regFuncName_;
    std::map<int, std::vector<HIRNode*>> blockEntryControls_;
    std::map<int, std::vector<HIRNode*>> blockExitStates_;
    std::map<int, HIRNode*> blockExitEffects_;

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
