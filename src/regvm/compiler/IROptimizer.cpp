#include "IROptimizer.h"
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <cmath>

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
        changed |= eliminateCommonSubexpressions(graph);
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
    oldNode->forwarding = newNode;
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

struct StrictValueHasher {
    size_t operator()(const Value& v) const {
        size_t h = ValueHasher{}(v);
        int typeTag = 0;
        if (v.isInt32()) typeTag = 1;
        else if (v.isBool()) typeTag = 2;
        else if (v.isDouble()) typeTag = 3;
        else if (v.isObj()) typeTag = 4 + static_cast<int>(v.asObj()->type);
        return h ^ (typeTag * 0x9e3779b9);
    }
};

struct StrictValueEqual {
    bool operator()(const Value& lhs, const Value& rhs) const {
        if (lhs.as_bits == rhs.as_bits) return true;
        if (lhs.isInt32() != rhs.isInt32()) return false;
        if (lhs.isBool() != rhs.isBool()) return false;
        if (lhs.isDouble() != rhs.isDouble()) return false;
        if (lhs.isObj() != rhs.isObj()) return false;
        if (lhs.isObj()) {
            if (lhs.asObj()->type != rhs.asObj()->type) return false;
            if (lhs.isString()) return lhs.asString() == rhs.asString();
            return Value::equals(lhs, rhs);
        }
        return false;
    }
};

bool IROptimizer::eliminateCommonSubexpressions(IRGraph* graph) {
    bool changed = false;
    
    struct NodeHasher {
        size_t operator()(const IRNode* n) const {
            size_t h = static_cast<size_t>(n->op);
            for (auto* in : n->dataInputs) {
                h ^= reinterpret_cast<size_t>(in) + 0x9e3779b9 + (h << 6) + (h >> 2);
            }
            h ^= reinterpret_cast<size_t>(n->controlInput) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= static_cast<size_t>(n->payload1) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    
    struct NodeEqual {
        bool operator()(const IRNode* a, const IRNode* b) const {
            if (a->op != b->op) return false;
            if (a->payload1 != b->payload1 || a->payload2 != b->payload2 || 
                a->payload3 != b->payload3 || a->payload4 != b->payload4 || 
                a->payload5 != b->payload5) return false;
            if (a->dataInputs.size() != b->dataInputs.size()) return false;
            for (size_t i = 0; i < a->dataInputs.size(); ++i) {
                if (a->dataInputs[i] != b->dataInputs[i]) return false;
            }
            if (a->controlInput != b->controlInput) return false;
            return true;
        }
    };

    std::unordered_map<IRNode*, IRNode*, NodeHasher, NodeEqual> seen;
    
    auto isPure = [](IROp op) {
        switch (op) {
            case IROp::Add: case IROp::Sub: case IROp::Mul: case IROp::Div:
            case IROp::Mod: case IROp::Pow: case IROp::LeftDivide:
            case IROp::Eq: case IROp::Neq: case IROp::Lt: case IROp::Le:
            case IROp::Gt: case IROp::Ge: case IROp::Not: case IROp::Neg:
            case IROp::BitAnd: case IROp::BitOr: case IROp::BitXor: case IROp::BitNot:
            case IROp::Shl: case IROp::Shr: case IROp::ToBool: case IROp::IsUninit:
                return true;
            default: return false;
        }
    };

    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        if (node->op == IROp::Nop) continue;
        if (isPure(node->op)) {
            auto it = seen.find(node);
            if (it != seen.end()) {
                replaceNode(graph, node, it->second);
                changed = true;
            } else {
                seen[node] = node;
            }
        }
    }
    return changed;
}

bool IROptimizer::deduplicateConstants(IRGraph* graph) {
    bool changed = false;
    std::unordered_map<Value, IRNode*, StrictValueHasher, StrictValueEqual> constMap;
    
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
            auto it = constMap.find(node->constVal);
            if (it != constMap.end()) {
                replaceNode(graph, node, it->second);
                changed = true;
            } else {
                constMap[node->constVal] = node;
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
                else if (node->op == IROp::Stringify) {
                    Value resVal;
                    if (val.isString()) {
                        resVal = val;
                    } else {
                        std::ostringstream oss;
                        if (val.isUninit()) oss << "Uninitialized";
                        else oss << val;
                        resVal = Value(oss.str());
                    }
                    IRNode* constNode = graph->createConstant(resVal);
                    constNode->setControl(node->controlInput);
                    for (auto& userNodePtr : graph->getNodes()) {
                        IRNode* user = userNodePtr.get();
                        if (user->controlInput == node) user->controlInput = node->controlInput;
                        for (auto& din : user->dataInputs) {
                            if (din == node) din = constNode;
                        }
                    }
                    node->op = IROp::Nop; node->dataInputs.clear(); node->controlInput = nullptr; changed = true;
                }
            } catch (...) {} // 忽略除零等运行时错误，留给 VM 抛出
        }
        // 字符串格式化折叠
        else if (node->op == IROp::FormatString && node->dataInputs.size() == 2) {
            IRNode* valNode = node->dataInputs[0];
            IRNode* specNode = node->dataInputs[1];
            if (valNode && valNode->op == IROp::Constant && !capturedNodes.count(valNode) &&
                specNode && specNode->op == IROp::Constant && !capturedNodes.count(specNode)) {
                Value val = valNode->constVal;
                const std::string& spec = specNode->constVal.asString();
                
                char align = '\0';
                int width = 0;
                int precision = -1;
                char type = '\0';
                size_t si = 0;
                if (si < spec.size() && (spec[si] == '<' || spec[si] == '>' || spec[si] == '^'))
                    align = spec[si++];
                while (si < spec.size() && spec[si] >= '0' && spec[si] <= '9')
                    width = width * 10 + (spec[si++] - '0');
                if (si < spec.size() && spec[si] == '.') {
                    si++; precision = 0;
                    while (si < spec.size() && spec[si] >= '0' && spec[si] <= '9')
                        precision = precision * 10 + (spec[si++] - '0');
                }
                if (si < spec.size()) type = spec[si++];

                std::ostringstream oss;
                if (type == 'f' || type == 'e') {
                    if (precision >= 0) oss << std::fixed << std::setprecision(precision);
                    if (type == 'e') oss << std::scientific;
                    try { oss << val.asDouble(); } catch (...) { oss << val; }
                }
                else if (type == 'd') { try { oss << static_cast<int64_t>(std::round(val.asDouble())); } catch (...) { oss << val; } }
                else if (type == 'x') { try { oss << std::hex << static_cast<int64_t>(std::round(val.asDouble())); } catch (...) { oss << val; } }
                else { oss << val; }

                std::string result = oss.str();
                if (width > 0 && static_cast<int>(result.size()) < width) {
                    int pad = width - static_cast<int>(result.size());
                    if (align == '<') result += std::string(pad, ' ');
                    else if (align == '^') {
                        int l = pad / 2, r = pad - l;
                        result = std::string(l, ' ') + result + std::string(r, ' ');
                    }
                    else result = std::string(pad, ' ') + result;
                }
                
                IRNode* constNode = graph->createConstant(Value(result));
                constNode->setControl(node->controlInput);
                for (auto& userNodePtr : graph->getNodes()) {
                    IRNode* user = userNodePtr.get();
                    if (user->controlInput == node) user->controlInput = node->controlInput;
                    for (auto& din : user->dataInputs) {
                        if (din == node) din = constNode;
                    }
                }
                node->op = IROp::Nop; node->dataInputs.clear(); node->controlInput = nullptr; changed = true;
            }
        }
        // 字符串拼接折叠
        else if (node->op == IROp::ConcatStrings) {
            bool allConst = true;
            for (IRNode* in : node->dataInputs) {
                if (!in || in->op != IROp::Constant || capturedNodes.count(in)) {
                    allConst = false;
                    break;
                }
            }
            if (allConst && !node->dataInputs.empty()) {
                std::string res;
                for (IRNode* in : node->dataInputs) {
                    Value val = in->constVal;
                    if (val.isString()) res += val.asString();
                    else {
                        std::ostringstream oss;
                        if (val.isUninit()) oss << "Uninitialized";
                        else oss << val;
                        res += oss.str();
                    }
                }
                
                IRNode* constNode = graph->createConstant(Value(res));
                constNode->setControl(node->controlInput);
                for (auto& userNodePtr : graph->getNodes()) {
                    IRNode* user = userNodePtr.get();
                    if (user->controlInput == node) user->controlInput = node->controlInput;
                    for (auto& din : user->dataInputs) {
                        if (din == node) din = constNode;
                    }
                }
                node->op = IROp::Nop; node->dataInputs.clear(); node->controlInput = nullptr; changed = true;
            }
        }
        // 二元运算折叠与代数化简
        else if (node->dataInputs.size() == 2 && node->dataInputs[0] && node->dataInputs[1]) {
            IRNode* leftNode = node->dataInputs[0];
            IRNode* rightNode = node->dataInputs[1];
            
            bool leftIsConst = leftNode->op == IROp::Constant && !capturedNodes.count(leftNode);
            bool rightIsConst = rightNode->op == IROp::Constant && !capturedNodes.count(rightNode);
            
            if (leftIsConst && rightIsConst) {
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
                    else if (node->op == IROp::Eq) { 
                        bool eq = (left.isString() && right.isString()) ? (left.asString() == right.asString()) : Value::equals(left, right);
                        node->constVal = Value(eq); node->op = IROp::Constant; node->dataInputs.clear(); changed = true; 
                    }
                    else if (node->op == IROp::Neq) { 
                        bool eq = (left.isString() && right.isString()) ? (left.asString() == right.asString()) : Value::equals(left, right);
                        node->constVal = Value(!eq); node->op = IROp::Constant; node->dataInputs.clear(); changed = true; 
                    }
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

    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        
        if (node->op == IROp::Merge) {
            if (node->dataInputs.empty()) continue;
            bool allSame = true;
            IRNode* firstValid = nullptr;
            int validIndex = -1;
            for (size_t i = 0; i < node->dataInputs.size(); ++i) {
                IRNode* in = node->dataInputs[i];
                if (!in || in->op == IROp::Nop) continue;
                if (in == node) continue;
                if (!firstValid) {
                    firstValid = in;
                    validIndex = static_cast<int>(i);
                }
                else if (in != firstValid) { allSame = false; break; }
            }
            if (allSame && firstValid) {
                std::vector<IRNode*> dependentPhis;
                bool canSimplify = true;
                for (auto& nPtr : graph->getNodes()) {
                    if (nPtr->op == IROp::Phi && nPtr->controlInput == node) {
                        if (capturedNodes.count(nPtr.get())) {
                            canSimplify = false;
                            break;
                        }
                        dependentPhis.push_back(nPtr.get());
                    }
                }
                if (canSimplify) {
                    for (IRNode* phi : dependentPhis) {
                        IRNode* validData = nullptr;
                        if (validIndex != -1 && validIndex < static_cast<int>(phi->dataInputs.size())) {
                            validData = phi->dataInputs[validIndex];
                        }
                        if (validData) {
                            replaceNode(graph, phi, validData);
                        }
                    }
                    replaceNode(graph, node, firstValid);
                    changed = true;
                }
            }
        } 
        else if (node->op == IROp::Phi) {
            if (capturedNodes.count(node)) continue;
            
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
    
    int maxId = 0;
    for (auto& nodePtr : graph->getNodes()) {
        if (nodePtr->id > maxId) maxId = nodePtr->id;
    }
    
    // 1. 正向传播控制流可达性 (Forward Control Flow Reachability)
    std::vector<bool> reachableControl(maxId + 1, false);
    std::vector<IRNode*> ctrlWorklist;
    
    if (graph->startNode) {
        reachableControl[graph->startNode->id] = true;
        ctrlWorklist.push_back(graph->startNode);
    }
    
    std::vector<std::vector<IRNode*>> ctrlUses(maxId + 1);
    for (auto& nodePtr : graph->getNodes()) {
        IRNode* n = nodePtr.get();
        if (n->controlInput) {
            ctrlUses[n->controlInput->id].push_back(n);
        }
        if (n->op == IROp::Merge || n->op == IROp::Loop) {
            for (IRNode* din : n->dataInputs) {
                if (din) ctrlUses[din->id].push_back(n);
            }
        }
    }
    
    while (!ctrlWorklist.empty()) {
        IRNode* curr = ctrlWorklist.back();
        ctrlWorklist.pop_back();
        for (IRNode* use : ctrlUses[curr->id]) {
            if (!reachableControl[use->id]) {
                reachableControl[use->id] = true;
                // 任何被控制流边连接的节点都属于控制流图的一部分，继续传播
                ctrlWorklist.push_back(use);
            }
        }
    }
    
    // 2. 标记所有可达的副作用节点为存活 (Roots)
    std::vector<bool> used(maxId + 1, false);
    std::vector<IRNode*> worklist;
    
    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        if (node->op == IROp::Nop) continue;
        if (hasSideEffects(node->op)) {
            // 只要节点在控制流可达集合中，它就是存活的 Root
            if (reachableControl[node->id]) {
                used[node->id] = true;
                worklist.push_back(node);
            }
        }
    }
    
    // 3. 逆向传播存活标记 (Backward Mark)
    while (!worklist.empty()) {
        IRNode* curr = worklist.back();
        worklist.pop_back();
        
        if (curr->controlInput && !used[curr->controlInput->id]) {
            used[curr->controlInput->id] = true;
            worklist.push_back(curr->controlInput);
        }
        
        if (curr->op == IROp::Merge || curr->op == IROp::Loop) {
            for (IRNode* in : curr->dataInputs) {
                if (in && reachableControl[in->id] && !used[in->id]) {
                    used[in->id] = true;
                    worklist.push_back(in);
                }
            }
        } else if (curr->op == IROp::Phi) {
            IRNode* merge = curr->controlInput;
            if (merge && (merge->op == IROp::Merge || merge->op == IROp::Loop)) {
                for (size_t i = 0; i < curr->dataInputs.size(); ++i) {
                    IRNode* in = curr->dataInputs[i];
                    if (in && !used[in->id]) {
                        if (i < merge->dataInputs.size()) {
                            IRNode* ctrlIn = merge->dataInputs[i];
                            if (ctrlIn && reachableControl[ctrlIn->id]) {
                                used[in->id] = true;
                                worklist.push_back(in);
                            }
                        }
                    }
                }
            } else {
                for (IRNode* in : curr->dataInputs) {
                    if (in && !used[in->id]) {
                        used[in->id] = true;
                        worklist.push_back(in);
                    }
                }
            }
        } else {
            for (IRNode* in : curr->dataInputs) {
                if (in && !used[in->id]) {
                    used[in->id] = true;
                    worklist.push_back(in);
                }
            }
        }
    }
    
    // 4. 清除未被标记的节点 (Sweep)
    for (auto& nodePtr : graph->getNodes()) {
        IRNode* node = nodePtr.get();
        if (node->op != IROp::Nop && !used[node->id]) {
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
