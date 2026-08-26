#ifndef JC2_JIT_BYTECODE_CFG_H
#define JC2_JIT_BYTECODE_CFG_H

#include "../../vm/Bytecode.h"
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <iostream>

namespace jc {
namespace jit {

// ============================================================================
// 字节码基本块 (Step 32)
// ============================================================================
struct BytecodeBlock {
    int id = 0;
    int startIp = 0;
    int endIp = 0; // 独占边界 (Exclusive)
    std::vector<int> successors;
    std::vector<int> predecessors;
    bool isLoopHeader = false;
    std::vector<int> backEdges; // 记录哪些前驱块是通过回边跳过来的
};

// ============================================================================
// 字节码控制流图分析器 (Step 32)
// ============================================================================
class BytecodeCFG {
public:
    std::vector<BytecodeBlock> blocks;
    std::map<int, int> ipToBlockId; // 映射 startIp 到 block ID

    void build(const Chunk& chunk) {
        std::set<int> leaders;
        leaders.insert(0); // 入口指令永远是 Leader

        const auto& code = chunk.code;
        int n = static_cast<int>(code.size());

        // 辅助函数：解析单条指令的长度、跳转目标以及是否为终端指令
        auto analyzeInstruction = [&](int startIp, int& nextIp, std::vector<int>& targets, bool& isTerminal) {
            int ip = startIp;
            Instruction inst = code[ip++];
            OpCode op = GET_OPCODE(inst);
            
            int a = GET_A(inst);
            int b = GET_B(inst);
            int c = GET_C(inst);
            int bx = GET_Bx(inst);
            int sbx = GET_sBx(inst);
            int sax = GET_sAx(inst);

            auto fetchExtra = [&]() { ip++; };

            isTerminal = false;

            // 模拟解释器的操作数获取逻辑，以确定指令的真实长度
            switch (op) {
                case OpCode::BUILD_LIST: case OpCode::BUILD_DICT: case OpCode::BUILD_SET:
                case OpCode::CONCAT_STRINGS: case OpCode::DICT_REST: case OpCode::BUILD_MATRIX:
                case OpCode::INDEX_GET: case OpCode::ITER_INIT: case OpCode::IN:
                case OpCode::DICT_APPEND: case OpCode::FORMAT_STRING: case OpCode::BUILD_NAMESPACE:
                case OpCode::ASSERT_PARAM_TYPE: case OpCode::ASSERT_TYPE: case OpCode::GET_PROP: case OpCode::GET_PRIVATE:
                case OpCode::TRY_GET_PROP: case OpCode::SET_PROP: case OpCode::SET_PRIVATE:
                case OpCode::DEFINE_PRIVATE: case OpCode::DEFINE_PRIVATE_CONST:
                case OpCode::DEFINE_PROP: case OpCode::DEFINE_PROP_CONST: case OpCode::INVOKE:
                case OpCode::TAIL_INVOKE: case OpCode::INVOKE_PRIVATE: case OpCode::TAIL_INVOKE_PRIVATE:
                case OpCode::INVOKE_FALLBACK: case OpCode::TAIL_INVOKE_FALLBACK:
                case OpCode::GET_SUPER: case OpCode::SUPER_INVOKE: case OpCode::TAIL_SUPER_INVOKE:
                case OpCode::METHOD: case OpCode::METHOD_PRIVATE: case OpCode::METHOD_CONST:
                case OpCode::METHOD_PRIVATE_CONST: case OpCode::CALL: case OpCode::TAIL_CALL:
                case OpCode::MATCH_SHAPE: case OpCode::MATCH_TYPE: case OpCode::IS_SUBSET:
                    if (a == ESCAPE_NORMAL_8) fetchExtra();
                    if (b == ESCAPE_NORMAL_8) fetchExtra();
                    if (c == ESCAPE_NORMAL_8) fetchExtra();
                    break;

                case OpCode::INDEX_SET:
                    if (a == ESCAPE_NORMAL_8) fetchExtra();
                    if (c == ESCAPE_NORMAL_8) fetchExtra();
                    break;

                case OpCode::MOVE: case OpCode::IS_UNINIT: case OpCode::UNM: case OpCode::NOT:
                case OpCode::BNOT: case OpCode::TO_BOOL: case OpCode::INHERIT: case OpCode::LIST_APPEND:
                case OpCode::MATRIX_COMP_APPEND:
                case OpCode::SET_APPEND: case OpCode::STRINGIFY: case OpCode::ITER_NEXT:
                case OpCode::IMPORT: case OpCode::GET_UPVAL: case OpCode::SET_UPVAL:
                case OpCode::BUILD_SLICE: case OpCode::MATCH_INIT:
                    if (a == ESCAPE_NORMAL_8) fetchExtra();
                    if (b == ESCAPE_NORMAL_8) fetchExtra();
                    break;

                case OpCode::LOAD_NIL: case OpCode::LOAD_BOOL: case OpCode::ASSERT_RETURN_TYPE:
                case OpCode::RETURN: case OpCode::GET_SELF: case OpCode::GET_CURRENT_CLOSURE:
                case OpCode::LIST_INIT: case OpCode::MATRIX_COMP_INIT: case OpCode::MATRIX_COMP_END: case OpCode::SET_INIT:
                case OpCode::DICT_INIT: case OpCode::THROW: case OpCode::DEFER: case OpCode::RUN_DEFERS:
                    if (a == ESCAPE_NORMAL_8) fetchExtra();
                    break;

                case OpCode::LOADK: case OpCode::GET_GLOBAL: case OpCode::SET_GLOBAL:
                case OpCode::SET_GLOBAL_REF: case OpCode::DEFINE_CONST_GLOBAL: case OpCode::CLASS:
                case OpCode::CLOSURE: case OpCode::GET_REF_PARAM: case OpCode::SET_REF_PARAM:
                    if (a == ESCAPE_NORMAL_8) fetchExtra();
                    if (bx == ESCAPE_NORMAL_16) fetchExtra();
                    break;

                case OpCode::DELETE_GLOBAL: case OpCode::PASS_REFS:
                    if (bx == ESCAPE_NORMAL_16) fetchExtra();
                    break;

                case OpCode::ADD: case OpCode::SUB: case OpCode::MUL: case OpCode::DIV:
                case OpCode::IDIV: case OpCode::MOD: case OpCode::POW: case OpCode::LDIV:
                case OpCode::BAND: case OpCode::BOR: case OpCode::BXOR: case OpCode::SHL:
                case OpCode::SHR: case OpCode::EQ: case OpCode::NEQ: case OpCode::LT:
                case OpCode::LE: case OpCode::GT: case OpCode::GE: case OpCode::IS:
                    if (a == ESCAPE_NORMAL_8) fetchExtra();
                    if (b == ESCAPE_KBIT_CONST || b == ESCAPE_KBIT_REG) fetchExtra();
                    if (c == ESCAPE_KBIT_CONST || c == ESCAPE_KBIT_REG) fetchExtra();
                    break;

                case OpCode::JMP_TRUE: case OpCode::JMP_FALSE: case OpCode::TRY_BEGIN:
                    if (a == ESCAPE_NORMAL_8) fetchExtra();
                    targets.push_back(ip + sbx);
                    break;

                case OpCode::JMP:
                    targets.push_back(ip + sax);
                    isTerminal = true;
                    break;

                case OpCode::TRY_END: case OpCode::EXTRAARG: case OpCode::SET_KW_ARGC:
                    break;

                default:
                    break;
            }

            if (op == OpCode::RETURN || op == OpCode::THROW || op == OpCode::TAIL_CALL || 
                op == OpCode::TAIL_INVOKE || op == OpCode::TAIL_INVOKE_PRIVATE || 
                op == OpCode::TAIL_INVOKE_FALLBACK || op == OpCode::TAIL_SUPER_INVOKE) {
                isTerminal = true;
            }

            nextIp = ip;
        };

        // 1. 扫描所有指令，找出所有的 Leader (基本块的起点)
        for (int ip = 0; ip < n; ) {
            int nextIp = ip;
            std::vector<int> targets;
            bool isTerminal = false;
            
            analyzeInstruction(ip, nextIp, targets, isTerminal);

            // 跳转目标是 Leader
            for (int target : targets) {
                if (target >= 0 && target < n) {
                    leaders.insert(target);
                }
            }

            // 终端指令或条件跳转指令的下一条指令也是 Leader
            if (isTerminal && nextIp < n) {
                leaders.insert(nextIp);
            } else if (!targets.empty() && nextIp < n) {
                leaders.insert(nextIp);
            }

            ip = nextIp;
        }

        // 2. 根据 Leaders 划分基本块
        std::vector<int> leaderList(leaders.begin(), leaders.end());
        for (size_t i = 0; i < leaderList.size(); ++i) {
            BytecodeBlock block;
            block.id = static_cast<int>(i);
            block.startIp = leaderList[i];
            block.endIp = (i + 1 < leaderList.size()) ? leaderList[i + 1] : n;
            blocks.push_back(block);
            ipToBlockId[block.startIp] = block.id;
        }

        // 3. 再次扫描，连接基本块之间的控制流边 (Edges)
        for (auto& block : blocks) {
            if (block.startIp >= n) continue;

            int ip = block.startIp;
            int nextIp = ip;
            std::vector<int> targets;
            bool isTerminal = false;

            // 找到该基本块的最后一条指令
            while (ip < block.endIp) {
                targets.clear();
                analyzeInstruction(ip, nextIp, targets, isTerminal);
                ip = nextIp;
            }

            // 添加跳转边
            for (int target : targets) {
                if (ipToBlockId.count(target)) {
                    int targetBlockId = ipToBlockId[target];
                    if (std::find(block.successors.begin(), block.successors.end(), targetBlockId) == block.successors.end()) {
                        block.successors.push_back(targetBlockId);
                        blocks[targetBlockId].predecessors.push_back(block.id);
                    }
                }
            }

            // 添加 Fallthrough 边 (如果最后一条指令不是终端指令)
            if (!isTerminal && block.endIp < n) {
                if (ipToBlockId.count(block.endIp)) {
                    int fallthroughId = ipToBlockId[block.endIp];
                    if (std::find(block.successors.begin(), block.successors.end(), fallthroughId) == block.successors.end()) {
                        block.successors.push_back(fallthroughId);
                        blocks[fallthroughId].predecessors.push_back(block.id);
                    }
                }
            }
        }

        // 4. 识别循环头和回边 (DFS)
        std::vector<int> state(blocks.size(), 0); // 0: 未访问, 1: 正在访问(在DFS栈中), 2: 已访问
        
        auto dfs = [&](auto& self, int u) -> void {
            state[u] = 1;
            for (int v : blocks[u].successors) {
                if (state[v] == 0) {
                    self(self, v);
                } else if (state[v] == 1) {
                    // 发现回边: u -> v
                    blocks[v].isLoopHeader = true;
                    blocks[v].backEdges.push_back(u);
                }
            }
            state[u] = 2;
        };
        
        if (!blocks.empty()) {
            dfs(dfs, 0);
        }
    }

    void print() const {
        std::cout << "=== Bytecode CFG ===" << std::endl;
        for (const auto& block : blocks) {
            std::cout << "Block " << block.id << " [" << block.startIp << ", " << block.endIp << ")";
            if (block.isLoopHeader) std::cout << " (Loop Header)";
            std::cout << std::endl;
            std::cout << "  Predecessors: ";
            for (int p : block.predecessors) std::cout << p << " ";
            std::cout << std::endl;
            std::cout << "  Successors: ";
            for (int s : block.successors) std::cout << s << " ";
            std::cout << std::endl;
            if (block.isLoopHeader) {
                std::cout << "  BackEdges: ";
                for (int b : block.backEdges) std::cout << b << " ";
                std::cout << std::endl;
            }
        }
        std::cout << "====================" << std::endl;
    }
};

} // namespace jit
} // namespace jc

#endif // JC2_JIT_BYTECODE_CFG_H
