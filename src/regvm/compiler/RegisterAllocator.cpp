#include "RegisterAllocator.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <stack>
#include <functional>

namespace jc {
namespace regvm {

// 判断节点是否属于控制流骨架 (Control Spine)
static bool isControlSpine(IROp op) {
    switch (op) {
        case IROp::Start: case IROp::Return: case IROp::If: case IROp::IfTrue: case IROp::IfFalse:
        case IROp::Merge: case IROp::Loop: case IROp::TryBegin: case IROp::Catch: case IROp::TryEnd: case IROp::Throw:
        case IROp::GetGlobal: case IROp::SetGlobal: case IROp::SetGlobalRef: case IROp::DefineConstGlobal: case IROp::DeleteGlobal:
        case IROp::StoreLocal: case IROp::SetUpvalue: case IROp::SetRefParam: case IROp::PassRefs: case IROp::UpdateCaptured:
        case IROp::Call: case IROp::TailCall: case IROp::Invoke: case IROp::TailInvoke:
        case IROp::InvokeFallback: case IROp::TailInvokeFallback: case IROp::SuperInvoke: case IROp::TailSuperInvoke:
        case IROp::IndexGet: case IROp::IndexSet: case IROp::SliceGet: case IROp::SliceSet: 
        case IROp::GetProperty: case IROp::TryGetProperty: case IROp::SetProperty:
        case IROp::AssertParamType: case IROp::AssertReturnType:
        case IROp::IterInit: case IROp::IterNext: case IROp::BuildList: case IROp::BuildDict:
        case IROp::DictRest: case IROp::BuildSet: case IROp::BuildMatrix: case IROp::BuildNamespace: case IROp::Class:
        case IROp::Method: case IROp::Inherit: case IROp::Import:
        case IROp::ListInit: case IROp::ListAppend: case IROp::ListCompEnd:
        case IROp::Stringify: case IROp::ConcatStrings: case IROp::FormatString:
            return true;
        default:
            return false;
    }
}

void RegisterAllocator::allocate(IRGraph* graph) {
    std::unordered_map<IRNode*, BasicBlock*> nodeToBB;
    graph->blocks.clear();
    auto& blocks = graph->blocks;
    int bbId = 0;

    // 1. 为每个控制流骨架节点创建基本块
    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        if (node->op == IROp::Nop) continue;
        if (isControlSpine(node->op)) {
            auto bb = std::make_unique<BasicBlock>();
            bb->id = bbId++;
            bb->controlNode = node;
            nodeToBB[node] = bb.get();
            blocks.push_back(std::move(bb));
        }
    }

    // 2. 构建 CFG 边
    for (auto& bb : blocks) {
        IRNode* c = bb->controlNode;
        if (c->op == IROp::Merge || c->op == IROp::Loop) {
            for (IRNode* predC : c->dataInputs) {
                if (predC && nodeToBB.count(predC)) {
                    BasicBlock* predBB = nodeToBB[predC];
                    bb->preds.push_back(predBB);
                    predBB->succs.push_back(bb.get());
                }
            }
        } else if (c->controlInput && nodeToBB.count(c->controlInput)) {
            BasicBlock* predBB = nodeToBB[c->controlInput];
            bb->preds.push_back(predBB);
            predBB->succs.push_back(bb.get());
        }
    }

    // 3. 将数据节点分配到基本块中
    std::unordered_map<BasicBlock*, std::vector<IRNode*>> blockDataNodes;
    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        if (node->op == IROp::Nop || isControlSpine(node->op) || node->op == IROp::Phi) continue;
        if (node->controlInput && nodeToBB.count(node->controlInput)) {
            blockDataNodes[nodeToBB[node->controlInput]].push_back(node);
        }
    }

    // 在每个基本块内对数据节点进行拓扑排序 (指令调度)
    for (auto& bb : blocks) {
        auto& dataNodes = blockDataNodes[bb.get()];
        std::unordered_set<IRNode*> visited;
        std::unordered_set<IRNode*> visiting;
        std::vector<IRNode*> sorted;

        std::function<void(IRNode*)> dfs = [&](IRNode* n) {
            if (visited.count(n)) return;
            if (visiting.count(n)) return;
            visiting.insert(n);
            for (IRNode* in : n->dataInputs) {
                if (in && !isControlSpine(in->op) && in->op != IROp::Phi && in->controlInput == bb->controlNode) {
                    dfs(in);
                }
            }
            visiting.erase(n);
            visited.insert(n);
            sorted.push_back(n);
        };

        for (IRNode* n : dataNodes) dfs(n);
        bb->instructions = sorted;
    }

    // 4. Phi 节点去结构化 (Phi Destruction)
    std::vector<IRNode*> phis;
    for (auto& nodePtr : graph->getNodes()) {
        if (nodePtr->op == IROp::Phi && nodePtr->virtualReg != -1) {
            phis.push_back(nodePtr.get());
        }
    }
    
    for (IRNode* phi : phis) {
        IRNode* merge = phi->controlInput;
        if (!merge || (merge->op != IROp::Merge && merge->op != IROp::Loop)) continue;

        for (size_t i = 0; i < phi->dataInputs.size(); ++i) {
            IRNode* src = phi->dataInputs[i];
            if (!src || src->op == IROp::Nop) continue;
            if (i >= merge->dataInputs.size()) continue;
            IRNode* predC = merge->dataInputs[i];
            if (!predC || !nodeToBB.count(predC)) continue;

            BasicBlock* predBB = nodeToBB[predC];
            IRNode* moveNode = graph->createNode(IROp::Move);
            moveNode->virtualReg = phi->virtualReg;
            moveNode->addData(src);
            moveNode->setControl(predC);
            
            predBB->instructions.push_back(moveNode);
        }
        phi->op = IROp::Nop; // 销毁 Phi 节点
    }

    // 将控制节点插入到基本块指令流的开头
    for (auto& bb : blocks) {
        bb->instructions.insert(bb->instructions.begin(), bb->controlNode);
    }

    // 5. 活跃变量分析 (Liveness Analysis)
    int numVRegs = 0;
    for (auto& nodePtr : graph->getNodes()) {
        if (nodePtr->virtualReg >= numVRegs) numVRegs = nodePtr->virtualReg + 1;
    }

    if (numVRegs == 0) return;

    std::unordered_set<IRNode*> capturedNodes;
    for (auto& nodePtr : graph->getNodes()) {
        if (nodePtr->op == IROp::Closure) {
            for (IRNode* in : nodePtr->dataInputs) {
                if (in) capturedNodes.insert(in);
            }
        } else if (nodePtr->op == IROp::UpdateCaptured) {
            if (nodePtr->dataInputs.size() > 0 && nodePtr->dataInputs[0]) {
                capturedNodes.insert(nodePtr->dataInputs[0]);
            }
        } else if (nodePtr->op == IROp::PassRefs) {
            for (IRNode* in : nodePtr->dataInputs) {
                if (in) capturedNodes.insert(in);
            }
        }
    }

    std::vector<bool> isUncoloredConst(numVRegs, false);
    for (auto& nodePtr : graph->getNodes()) {
        if (nodePtr->op == IROp::Constant && nodePtr->virtualReg != -1) {
            if (!capturedNodes.count(nodePtr.get())) {
                isUncoloredConst[nodePtr->virtualReg] = true;
            }
        }
    }

    for (auto& bb : blocks) {
        for (IRNode* inst : bb->instructions) {
            if (inst->virtualReg != -1 && isUncoloredConst[inst->virtualReg]) continue; // 未捕获的常量不占用物理寄存器
            for (IRNode* src : inst->dataInputs) {
                if (src && src->virtualReg != -1 && !isUncoloredConst[src->virtualReg] && !bb->def.count(src->virtualReg)) {
                    bb->use.insert(src->virtualReg);
                }
            }
            if (inst->virtualReg != -1) {
                bb->def.insert(inst->virtualReg);
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
            auto& bb = *it;
            size_t oldOut = bb->liveOut.size();
            size_t oldIn = bb->liveIn.size();

            for (BasicBlock* succ : bb->succs) {
                bb->liveOut.insert(succ->liveIn.begin(), succ->liveIn.end());
            }

            bb->liveIn = bb->use;
            for (int v : bb->liveOut) {
                if (!bb->def.count(v)) bb->liveIn.insert(v);
            }

            if (bb->liveOut.size() != oldOut || bb->liveIn.size() != oldIn) {
                changed = true;
            }
        }
    }

    // 6. 构建冲突图 (Interference Graph) 与 Move 偏好
    std::vector<std::unordered_set<int>> adj(numVRegs);
    std::vector<int> degree(numVRegs, 0);
    std::unordered_map<int, std::unordered_set<int>> moveHints; // 记录 Move 指令的源和目标，用于着色偏好

    auto addEdge = [&](int u, int v) {
        if (u != v && !adj[u].count(v)) {
            adj[u].insert(v);
            adj[v].insert(u);
            degree[u]++;
            degree[v]++;
        }
    };

    for (auto& bb : blocks) {
        std::unordered_set<int> live = bb->liveOut;
        for (auto it = bb->instructions.rbegin(); it != bb->instructions.rend(); ++it) {
            IRNode* inst = *it;
            
            // 收集 Move 偏好
            if (inst->op == IROp::Move && inst->dataInputs.size() == 1 && inst->dataInputs[0]) {
                int srcReg = inst->dataInputs[0]->virtualReg;
                int dstReg = inst->virtualReg;
                if (srcReg != -1 && dstReg != -1) {
                    moveHints[dstReg].insert(srcReg);
                    moveHints[srcReg].insert(dstReg);
                }
            }

            if (inst->virtualReg != -1) {
                live.erase(inst->virtualReg);
                for (int l : live) {
                    // ★ 核心优化：Move 指令的目标寄存器不与源寄存器产生冲突
                    if (inst->op == IROp::Move && inst->dataInputs.size() == 1 && 
                        inst->dataInputs[0] && inst->dataInputs[0]->virtualReg == l) {
                        continue;
                    }
                    addEdge(inst->virtualReg, l);
                }
            }
            for (IRNode* src : inst->dataInputs) {
                if (src && src->virtualReg != -1 && !isUncoloredConst[src->virtualReg]) {
                    live.insert(src->virtualReg);
                }
            }
        }
    }

    // 7. Chaitin 图着色算法
    // 根据 K-Bit 设计，B/C 操作数最高位为 1 时表示常量池索引 (0~127)
    // 且 127 (0x7F) 被保留作为 EXTRAARG 的转义标志。
    // 因此物理寄存器被严格限制在 0~126。
    // 我们保留 3 个寄存器 (124, 125, 126) 用于极端溢出恢复，因此 K = 124。
    const int K = 124;
    std::vector<int> color(numVRegs, -1);
    std::vector<bool> removed(numVRegs, false);
    std::stack<int> selectStack;

    // 预着色：函数参数必须分配到指定的连续物理寄存器
    for (auto& nodePtr : graph->getNodes()) {
        if (nodePtr->op == IROp::Parameter && nodePtr->virtualReg != -1) {
            color[nodePtr->virtualReg] = nodePtr->payload1;
            removed[nodePtr->virtualReg] = true;
        } else if (nodePtr->op == IROp::Constant && nodePtr->virtualReg != -1) {
            if (isUncoloredConst[nodePtr->virtualReg]) {
                // 常量节点不参与着色，它们将在生成字节码时使用 K-Bit 或被加载到暂存器
                removed[nodePtr->virtualReg] = true;
            }
        }
    }

    int nodesRemaining = 0;
    for (int i = 0; i < numVRegs; ++i) {
        if (removed[i]) continue;
        bool used = false;
        for (auto& bb : blocks) {
            if (bb->def.count(i) || bb->use.count(i)) { used = true; break; }
        }
        if (used) nodesRemaining++;
        else removed[i] = true;
    }

    // 简化与溢出选择
    while (nodesRemaining > 0) {
        int pick = -1;
        for (int i = 0; i < numVRegs; ++i) {
            if (!removed[i] && degree[i] < K) {
                pick = i;
                break;
            }
        }

        if (pick == -1) {
            // 必须溢出：选择度数最大的节点作为牺牲者
            int maxDeg = -1;
            for (int i = 0; i < numVRegs; ++i) {
                if (!removed[i] && degree[i] > maxDeg) {
                    maxDeg = degree[i];
                    pick = i;
                }
            }
        }

        removed[pick] = true;
        selectStack.push(pick);
        nodesRemaining--;

        for (int neighbor : adj[pick]) {
            if (!removed[neighbor]) degree[neighbor]--;
        }
    }

    // 着色与溢出槽分配
    int maxPreColor = 127;
    for (int i = 0; i < numVRegs; ++i) {
        if (removed[i] && color[i] > maxPreColor) {
            maxPreColor = color[i];
        }
    }
    int spillCount = maxPreColor - 127;

    while (!selectStack.empty()) {
        int v = selectStack.top();
        selectStack.pop();

        std::vector<bool> usedColors(K, false);
        for (int neighbor : adj[v]) {
            if (color[neighbor] != -1 && color[neighbor] < K) {
                usedColors[color[neighbor]] = true;
            }
        }

        int c = -1;
        
        // ★ 优先尝试使用 Move Hint 偏好的颜色 (寄存器合并)
        for (int hintReg : moveHints[v]) {
            int hintColor = color[hintReg];
            if (hintColor != -1 && hintColor < K && !usedColors[hintColor]) {
                c = hintColor;
                break;
            }
        }

        // 如果偏好颜色不可用，则分配编号最小的可用颜色
        if (c == -1) {
            for (int i = 0; i < K; ++i) {
                if (!usedColors[i]) {
                    c = i;
                    break;
                }
            }
        }

        if (c != -1) {
            color[v] = c;
        } else {
            // 分配到溢出槽 (>= 128)
            color[v] = 128 + spillCount++;
        }
    }

    // 8. 回填物理寄存器分配结果
    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        if (node->virtualReg != -1 && color[node->virtualReg] != -1) {
            node->physicalReg = color[node->virtualReg];
        }
    }
}

} // namespace regvm
} // namespace jc
