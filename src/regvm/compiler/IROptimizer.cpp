#include "IROptimizer.h"
#include <unordered_map>
#include <unordered_set>

namespace jc {
namespace regvm {

void IROptimizer::optimize(IRGraph* graph) {
    bool changed;
    do {
        changed = false;
        changed |= foldConstants(graph);
        changed |= foldControlFlow(graph);
        changed |= simplifyPhis(graph);
        changed |= deduplicateConstants(graph);
        changed |= eliminateDeadCode(graph);
    } while (changed);
}

void IROptimizer::replaceNode(IRGraph* graph, IRNode* oldNode, IRNode* newNode) {
    if (oldNode == newNode) return;
    for (auto& nodePtr : graph->getNodes()) {
        IRNode* n = nodePtr.get();
        if (n->controlInput == oldNode) n->controlInput = newNode;
        for (auto& din : n->dataInputs) {
            if (din == oldNode) din = newNode;
        }
    }
    oldNode->op = IROp::Nop;
    oldNode->dataInputs.clear();
    oldNode->controlInput = nullptr;
}

bool IROptimizer::hasSideEffects(IROp op) {
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

bool IROptimizer::deduplicateConstants(IRGraph* graph) {
    bool changed = false;
    std::unordered_map<size_t, IRNode*> constMap;
    
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
            // 保护 slotNode 不被常量去重（它们的值将在寄存器分配后由回调填入）
            for (uint32_t i = 0; i < nodePtr->payload1; ++i) {
                if (nodePtr->dataInputs[i * 3 + 1]) capturedNodes.insert(nodePtr->dataInputs[i * 3 + 1]);
            }
            for (uint32_t i = nodePtr->payload1 * 3; i < nodePtr->dataInputs.size(); ++i) {
                if (nodePtr->dataInputs[i]) capturedNodes.insert(nodePtr->dataInputs[i]);
            }
        }
    }

    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        if (node->op == IROp::Constant && !capturedNodes.count(node)) {
            size_t hash = ValueHasher{}(node->constVal);
            auto it = constMap.find(hash);
            if (it != constMap.end() && Value::equals(it->second->constVal, node->constVal)) {
                replaceNode(graph, node, it->second);
                changed = true;
            } else {
                constMap[hash] = node;
            }
        }
    }
    return changed;
}

bool IROptimizer::foldConstants(IRGraph* graph) {
    bool changed = false;
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

    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        if (node->op == IROp::Nop || node->op == IROp::Constant) continue;

        // 一元运算折叠
        if (node->dataInputs.size() == 1 && node->dataInputs[0] && node->dataInputs[0]->op == IROp::Constant && !capturedNodes.count(node->dataInputs[0])) {
            Value val = node->dataInputs[0]->constVal;
            try {
                if (node->op == IROp::Neg) { node->constVal = -val; node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                else if (node->op == IROp::Not) { node->constVal = Value(!val.truthy()); node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                else if (node->op == IROp::BitNot) { node->constVal = ~val; node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                else if (node->op == IROp::ToBool) { node->constVal = Value(val.truthy()); node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
            } catch (...) {} // 忽略除零等运行时错误，留给 VM 抛出
        }
        // 二元运算折叠与代数化简
        else if (node->dataInputs.size() == 2 && node->dataInputs[0] && node->dataInputs[1]) {
            IRNode* leftNode = node->dataInputs[0];
            IRNode* rightNode = node->dataInputs[1];
            
            if (leftNode->op == IROp::Constant && rightNode->op == IROp::Constant && !capturedNodes.count(leftNode) && !capturedNodes.count(rightNode)) {
                Value left = leftNode->constVal;
                Value right = rightNode->constVal;
                try {
                    if (node->op == IROp::Add) { node->constVal = left + right; node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::Sub) { node->constVal = left - right; node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::Mul) { node->constVal = left * right; node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::Div) { node->constVal = left / right; node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::Mod) { node->constVal = left % right; node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::Pow) { node->constVal = left ^ right; node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::LeftDivide) { node->constVal = ldivide(left, right); node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::Eq) { node->constVal = Value(Value::equals(left, right)); node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::Neq) { node->constVal = Value(!Value::equals(left, right)); node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::Lt) { node->constVal = Value(left < right); node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::Le) { node->constVal = Value(left <= right); node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::Gt) { node->constVal = Value(left > right); node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::Ge) { node->constVal = Value(left >= right); node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::BitAnd) { node->constVal = left & right; node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::BitOr) { node->constVal = left | right; node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::BitXor) { node->constVal = bitXor(left, right); node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::Shl) { node->constVal = left << right; node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                    else if (node->op == IROp::Shr) { node->constVal = left >> right; node->op = IROp::Constant; node->dataInputs.clear(); changed = true; }
                } catch (...) {}
            }
        }
    }
    return changed;
}

bool IROptimizer::foldControlFlow(IRGraph* graph) {
    bool changed = false;
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

    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        if (node->op == IROp::If && node->dataInputs.size() == 1 && node->dataInputs[0] && node->dataInputs[0]->op == IROp::Constant && !capturedNodes.count(node->dataInputs[0])) {
            bool cond = node->dataInputs[0]->constVal.truthy();
            
            IRNode* ifTrueNode = nullptr;
            IRNode* ifFalseNode = nullptr;
            for (auto& n : graph->getNodes()) {
                if (n->controlInput == node) {
                    if (n->op == IROp::IfTrue) ifTrueNode = n.get();
                    if (n->op == IROp::IfFalse) ifFalseNode = n.get();
                }
            }
            
            if (ifTrueNode && ifFalseNode) {
                IRNode* taken = cond ? ifTrueNode : ifFalseNode;
                IRNode* untaken = cond ? ifFalseNode : ifTrueNode;
                
                replaceNode(graph, taken, node->controlInput);
                
                untaken->op = IROp::Nop;
                untaken->controlInput = nullptr;
                untaken->dataInputs.clear();
                
                node->op = IROp::Nop;
                node->controlInput = nullptr;
                node->dataInputs.clear();
                
                changed = true;
            }
        }
    }
    return changed;
}

bool IROptimizer::simplifyPhis(IRGraph* graph) {
    bool changed = false;
    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        
        if (node->op == IROp::Merge) {
            if (node->dataInputs.empty()) continue;
            bool allSame = true;
            IRNode* firstValid = nullptr;
            for (IRNode* in : node->dataInputs) {
                if (!in || in->op == IROp::Nop) continue;
                if (in == node) continue;
                if (!firstValid) firstValid = in;
                else if (in != firstValid) { allSame = false; break; }
            }
            if (allSame && firstValid) {
                replaceNode(graph, node, firstValid);
                changed = true;
            }
        } 
        else if (node->op == IROp::Phi) {
            if (node->dataInputs.empty()) continue;
            bool allSame = true;
            IRNode* firstValid = nullptr;
            
            IRNode* mergeNode = node->controlInput;
            bool hasMerge = (mergeNode && mergeNode->op == IROp::Merge);
            
            for (size_t i = 0; i < node->dataInputs.size(); ++i) {
                IRNode* in = node->dataInputs[i];
                if (!in || in->op == IROp::Nop) continue;
                
                // 如果对应的控制流分支已经死亡，则忽略该数据输入
                if (hasMerge && i < mergeNode->dataInputs.size()) {
                    IRNode* ctrlIn = mergeNode->dataInputs[i];
                    if (!ctrlIn || ctrlIn->op == IROp::Nop) continue;
                }
                
                if (in == node) continue;
                if (!firstValid) firstValid = in;
                else if (in != firstValid) { allSame = false; break; }
            }
            
            if (allSame && firstValid) {
                replaceNode(graph, node, firstValid);
                changed = true;
            }
        }
    }
    return changed;
}

bool IROptimizer::eliminateDeadCode(IRGraph* graph) {
    bool changed = false;
    
    // 1. 正向传播控制流可达性 (Forward Control Flow Reachability)
    std::unordered_set<IRNode*> reachableControl;
    std::vector<IRNode*> ctrlWorklist;
    
    if (graph->startNode) {
        reachableControl.insert(graph->startNode);
        ctrlWorklist.push_back(graph->startNode);
    }
    
    std::unordered_map<IRNode*, std::vector<IRNode*>> ctrlUses;
    for (auto& nodePtr : graph->getNodes()) {
        IRNode* n = nodePtr.get();
        if (n->controlInput) {
            ctrlUses[n->controlInput].push_back(n);
        }
        if (n->op == IROp::Merge || n->op == IROp::Loop) {
            for (IRNode* din : n->dataInputs) {
                if (din) ctrlUses[din].push_back(n);
            }
        }
    }
    
    while (!ctrlWorklist.empty()) {
        IRNode* curr = ctrlWorklist.back();
        ctrlWorklist.pop_back();
        for (IRNode* use : ctrlUses[curr]) {
            if (reachableControl.find(use) == reachableControl.end()) {
                reachableControl.insert(use);
                // 任何被控制流边连接的节点都属于控制流图的一部分，继续传播
                ctrlWorklist.push_back(use);
            }
        }
    }
    
    // 2. 标记所有可达的副作用节点为存活 (Roots)
    std::unordered_set<IRNode*> used;
    std::vector<IRNode*> worklist;
    
    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        if (node->op == IROp::Nop) continue;
        if (hasSideEffects(node->op)) {
            // 只要节点在控制流可达集合中，它就是存活的 Root
            if (reachableControl.find(node) != reachableControl.end()) {
                used.insert(node);
                worklist.push_back(node);
            }
        }
    }
    
    // 3. 逆向传播存活标记 (Backward Mark)
    while (!worklist.empty()) {
        IRNode* curr = worklist.back();
        worklist.pop_back();
        
        if (curr->controlInput && used.find(curr->controlInput) == used.end()) {
            used.insert(curr->controlInput);
            worklist.push_back(curr->controlInput);
        }
        for (IRNode* in : curr->dataInputs) {
            if (in && used.find(in) == used.end()) {
                used.insert(in);
                worklist.push_back(in);
            }
        }
    }
    
    // 4. 清除未被标记的节点 (Sweep)
    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        if (node->op != IROp::Nop && used.find(node) == used.end()) {
            node->op = IROp::Nop;
            node->dataInputs.clear();
            node->controlInput = nullptr;
            changed = true;
        }
    }
    
    return changed;
}

} // namespace regvm
} // namespace jc
