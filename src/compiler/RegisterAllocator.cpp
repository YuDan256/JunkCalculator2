#include "RegisterAllocator.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <stack>
#include <functional>

namespace jc {

// 判断节点是否属于控制流骨架 (Control Spine)
static bool isControlSpine(IROp op) {
    switch (op) {
        case IROp::Start: case IROp::Return: case IROp::If: case IROp::IfTrue: case IROp::IfFalse:
        case IROp::Merge: case IROp::Loop: case IROp::TryBegin: case IROp::Catch: case IROp::TryEnd: case IROp::Throw:
        case IROp::GetGlobal: case IROp::SetGlobal: case IROp::SetGlobalRef: case IROp::DefineConstGlobal: case IROp::DeleteGlobal:
        case IROp::StoreLocal: case IROp::SetUpvalue: case IROp::SetRefParam: case IROp::PassRefs: case IROp::UpdateCaptured:
        case IROp::Call: case IROp::TailCall: case IROp::Invoke: case IROp::TailInvoke:
        case IROp::InvokePrivate: case IROp::TailInvokePrivate:
        case IROp::InvokeFallback: case IROp::TailInvokeFallback: case IROp::SuperInvoke: case IROp::TailSuperInvoke:
        case IROp::IndexGet: case IROp::IndexSet: case IROp::BuildSlice: case IROp::MakeSpread: 
        case IROp::GetProperty: case IROp::GetPrivate: case IROp::TryGetProperty: 
        case IROp::SetProperty: case IROp::SetPrivate: case IROp::DefinePrivate: case IROp::DefinePrivateConst:
        case IROp::DefineProp: case IROp::DefinePropConst:
        case IROp::GetSuper:
        case IROp::AssertParamType: case IROp::AssertReturnType: case IROp::AssertType:
        case IROp::MatchInit:
        case IROp::IterInit: case IROp::IterNext: case IROp::BuildList: case IROp::BuildDict:
        case IROp::DictRest: case IROp::BuildSet: case IROp::BuildMatrix: case IROp::BuildNamespace: case IROp::Class:
        case IROp::Method: case IROp::MethodPrivate: case IROp::MethodConst: case IROp::MethodPrivateConst:
        case IROp::Inherit: case IROp::Import:
        case IROp::Defer: case IROp::RunDefers:
        case IROp::ListInit: case IROp::ListAppend: case IROp::MatrixCompInit: case IROp::MatrixCompAppend: case IROp::MatrixCompEnd:
        case IROp::SetInit: case IROp::SetAppend: case IROp::DictInit: case IROp::DictAppend:
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

    // 记录每个基本块在 Phi 去结构化前的指令数量
    std::unordered_map<BasicBlock*, size_t> prePhiInstCount;
    for (auto& bb : blocks) {
        prePhiInstCount[bb.get()] = bb->instructions.size();
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
            if (!src) continue;
            // 注意：不要跳过 op == Nop 的节点，因为它可能是刚刚被去结构化的另一个 Phi 节点！
            // 只要它有 virtualReg，它就是一个有效的数据源。
            if (src->op == IROp::Nop && src->virtualReg == -1) continue;
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

    // 对每个基本块末尾追加的 Move 指令进行依赖排序 (Sequentializing Parallel Copies)
    for (auto& bb : blocks) {
        size_t startIdx = prePhiInstCount[bb.get()];
        if (startIdx >= bb->instructions.size()) continue;

        std::vector<IRNode*> moves(bb->instructions.begin() + startIdx, bb->instructions.end());
        std::vector<IRNode*> sortedMoves;
        std::unordered_set<IRNode*> visited;
        std::unordered_set<IRNode*> visiting;

        std::function<void(IRNode*)> dfsMove = [&](IRNode* m) {
            if (visited.count(m)) return;
            if (visiting.count(m)) return; // 简单打破循环依赖
            visiting.insert(m);

            int m_def = m->virtualReg;
            if (m_def != -1) {
                for (IRNode* other : moves) {
                    if (other == m) continue;
                    if (other->dataInputs.size() == 1 && other->dataInputs[0] && other->dataInputs[0]->virtualReg == m_def) {
                        // other 读取了 m 写入的寄存器，必须在 m 之前执行
                        dfsMove(other);
                    }
                }
            }

            visiting.erase(m);
            visited.insert(m);
            sortedMoves.push_back(m);
        };

        for (IRNode* m : moves) {
            dfsMove(m);
        }

        for (size_t i = 0; i < sortedMoves.size(); ++i) {
            bb->instructions[startIdx + i] = sortedMoves[i];
        }
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
        } else if (nodePtr->op == IROp::BuildNamespace) {
            for (uint32_t i = nodePtr->payload1 * 3; i < nodePtr->dataInputs.size(); ++i) {
                if (nodePtr->dataInputs[i]) capturedNodes.insert(nodePtr->dataInputs[i]);
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

    std::vector<std::vector<bool>> liveIn(blocks.size(), std::vector<bool>(numVRegs, false));
    std::vector<std::vector<bool>> liveOut(blocks.size(), std::vector<bool>(numVRegs, false));
    std::vector<std::vector<bool>> def(blocks.size(), std::vector<bool>(numVRegs, false));
    std::vector<std::vector<bool>> use(blocks.size(), std::vector<bool>(numVRegs, false));

    for (size_t i = 0; i < blocks.size(); ++i) {
        auto& bb = blocks[i];
        for (IRNode* inst : bb->instructions) {
            if (inst->virtualReg != -1 && isUncoloredConst[inst->virtualReg]) continue;
            for (IRNode* src : inst->dataInputs) {
                if (src && src->virtualReg != -1 && !isUncoloredConst[src->virtualReg] && !def[i][src->virtualReg]) {
                    use[i][src->virtualReg] = true;
                }
            }
            if (inst->virtualReg != -1) {
                def[i][inst->virtualReg] = true;
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = static_cast<int>(blocks.size()) - 1; i >= 0; --i) {
            auto& bb = blocks[i];
            
            for (BasicBlock* succ : bb->succs) {
                int sIdx = succ->id;
                for (int v = 0; v < numVRegs; ++v) {
                    if (liveIn[sIdx][v] && !liveOut[i][v]) {
                        liveOut[i][v] = true;
                        changed = true;
                    }
                }
            }

            for (int v = 0; v < numVRegs; ++v) {
                bool newVal = use[i][v] || (liveOut[i][v] && !def[i][v]);
                if (newVal != liveIn[i][v]) {
                    liveIn[i][v] = newVal;
                    changed = true;
                }
            }
        }
    }

    // 6. 构建冲突图 (Interference Graph) 与 Move 偏好
    std::vector<std::vector<bool>> adjMatrix(numVRegs, std::vector<bool>(numVRegs, false));
    std::vector<std::vector<int>> adjList(numVRegs);
    std::vector<int> degree(numVRegs, 0);
    std::vector<std::vector<int>> moveHints(numVRegs);

    auto addEdge = [&](int u, int v) {
        if (u != v && !adjMatrix[u][v]) {
            adjMatrix[u][v] = true;
            adjMatrix[v][u] = true;
            adjList[u].push_back(v);
            adjList[v].push_back(u);
            degree[u]++;
            degree[v]++;
        }
    };

    for (size_t i = 0; i < blocks.size(); ++i) {
        auto& bb = blocks[i];
        std::vector<bool> live = liveOut[i];
        for (auto it = bb->instructions.rbegin(); it != bb->instructions.rend(); ++it) {
            IRNode* inst = *it;
            
            if (inst->op == IROp::Move && inst->dataInputs.size() == 1 && inst->dataInputs[0]) {
                int srcReg = inst->dataInputs[0]->virtualReg;
                int dstReg = inst->virtualReg;
                if (srcReg != -1 && dstReg != -1) {
                    moveHints[dstReg].push_back(srcReg);
                    moveHints[srcReg].push_back(dstReg);
                }
            }

            if (inst->virtualReg != -1) {
                live[inst->virtualReg] = false;
                for (int l = 0; l < numVRegs; ++l) {
                    if (live[l]) {
                        if (inst->op == IROp::Move && inst->dataInputs.size() == 1 && 
                            inst->dataInputs[0] && inst->dataInputs[0]->virtualReg == l) {
                            continue;
                        }
                        addEdge(inst->virtualReg, l);
                    }
                }
            }
            for (IRNode* src : inst->dataInputs) {
                if (src && src->virtualReg != -1 && !isUncoloredConst[src->virtualReg]) {
                    live[src->virtualReg] = true;
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
        } else if (nodePtr->op == IROp::Constant && nodePtr->virtualReg != -1) {
            if (isUncoloredConst[nodePtr->virtualReg]) {
                // 常量节点不参与着色，它们将在生成字节码时使用 K-Bit 或被加载到暂存器
                removed[nodePtr->virtualReg] = true;
            }
        }
    }

    // 7.1 贪心寄存器合并 (Greedy Register Coalescing)
    std::vector<int> alias(numVRegs);
    for (int i = 0; i < numVRegs; ++i) alias[i] = i;

    auto getAlias = [&](auto& self, int x) -> int {
        if (alias[x] == x) return x;
        return alias[x] = self(self, alias[x]);
    };

    bool coalesced = false;
    for (auto& bb : blocks) {
        for (IRNode* inst : bb->instructions) {
            if (inst->op == IROp::Move && inst->dataInputs.size() == 1 && inst->dataInputs[0]) {
                int u = inst->virtualReg;
                int v = inst->dataInputs[0]->virtualReg;
                if (u != -1 && v != -1 && !isUncoloredConst[u] && !isUncoloredConst[v]) {
                    int rootU = getAlias(getAlias, u);
                    int rootV = getAlias(getAlias, v);
                    if (rootU != rootV && !adjMatrix[rootU][rootV]) {
                        int cU = color[rootU];
                        int cV = color[rootV];
                        if (cU != -1 && cV != -1 && cU != cV) continue; // 预着色冲突，不能合并
                        
                        if (cV != -1 && cU == -1) std::swap(rootU, rootV);
                        
                        alias[rootV] = rootU;
                        for (int neighbor : adjList[rootV]) {
                            int rootN = getAlias(getAlias, neighbor);
                            if (rootN != rootU) {
                                addEdge(rootU, rootN);
                            }
                        }
                        coalesced = true;
                    }
                }
            }
        }
    }

    if (coalesced) {
        for (int i = 0; i < numVRegs; ++i) {
            if (getAlias(getAlias, i) != i) {
                removed[i] = true;
            } else {
                degree[i] = 0;
                std::vector<int> newAdj;
                for (int neighbor : adjList[i]) {
                    int rootN = getAlias(getAlias, neighbor);
                    if (rootN != i && std::find(newAdj.begin(), newAdj.end(), rootN) == newAdj.end()) {
                        newAdj.push_back(rootN);
                    }
                }
                adjList[i] = newAdj;
                degree[i] = static_cast<int>(newAdj.size());
            }
        }
    }

    // 标记预着色节点为 removed
    for (int i = 0; i < numVRegs; ++i) {
        if (color[i] != -1) removed[i] = true;
    }

    int nodesRemaining = 0;
    for (int i = 0; i < numVRegs; ++i) {
        if (removed[i]) continue;
        bool isUsed = false;
        for (size_t bIdx = 0; bIdx < blocks.size(); ++bIdx) {
            if (def[bIdx][i] || use[bIdx][i]) { isUsed = true; break; }
        }
        if (isUsed) nodesRemaining++;
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

        for (int neighbor : adjList[pick]) {
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
        for (int neighbor : adjList[v]) {
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
    for (int i = 0; i < numVRegs; ++i) {
        int root = getAlias(getAlias, i);
        if (color[root] != -1) {
            color[i] = color[root];
        }
    }

    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        if (node->virtualReg != -1 && color[node->virtualReg] != -1) {
            node->physicalReg = color[node->virtualReg];
        }
    }

    // 9. 插入 FreeReg 指令以帮助 GC 及时回收死对象
    for (size_t i = 0; i < blocks.size(); ++i) {
        auto& bb = blocks[i];
        
        bool hasTerminal = false;
        for (IRNode* inst : bb->instructions) {
            if (inst->op == IROp::Return || inst->op == IROp::Throw || 
                inst->op == IROp::TailCall || inst->op == IROp::TailInvoke || 
                inst->op == IROp::TailInvokeFallback || inst->op == IROp::TailSuperInvoke) {
                hasTerminal = true;
                break;
            }
        }
        if (hasTerminal) continue; // 包含终端指令的块，VM 会在退出时自动清空整个调用帧
        
        std::unordered_set<int> physLiveOut;
        std::unordered_set<int> physUsed;
        
        for (int v = 0; v < numVRegs; ++v) {
            if (color[v] != -1) {
                if (liveOut[i][v]) physLiveOut.insert(color[v]);
                if (liveIn[i][v] || def[i][v]) physUsed.insert(color[v]);
            }
        }
        
        std::vector<int> deadRegs;
        for (int p : physUsed) {
            if (physLiveOut.find(p) == physLiveOut.end()) {
                deadRegs.push_back(p);
            }
        }
        
        IRNode* cNode = bb->controlNode;
        if (cNode) {
            if (cNode->op == IROp::If && cNode->dataInputs.size() > 0 && cNode->dataInputs[0]) {
                int p = cNode->dataInputs[0]->physicalReg;
                if (p != -1) {
                    auto it = std::find(deadRegs.begin(), deadRegs.end(), p);
                    if (it != deadRegs.end()) deadRegs.erase(it);
                }
            } else if (cNode->op == IROp::TryBegin) {
                BasicBlock* catchBlock = nullptr;
                for (auto* succ : bb->succs) {
                    if (succ->controlNode->op == IROp::Catch) {
                        catchBlock = succ;
                        break;
                    }
                }
                if (catchBlock && catchBlock->controlNode) {
                    int p = catchBlock->controlNode->physicalReg;
                    if (p != -1) {
                        auto it = std::find(deadRegs.begin(), deadRegs.end(), p);
                        if (it != deadRegs.end()) deadRegs.erase(it);
                    }
                }
            }
        }
        
        for (int p : deadRegs) {
            IRNode* freeNode = graph->createNode(IROp::FreeReg);
            freeNode->physicalReg = p;
            freeNode->setControl(cNode);
            bb->instructions.push_back(freeNode);
        }
    }
}

} // namespace jc
