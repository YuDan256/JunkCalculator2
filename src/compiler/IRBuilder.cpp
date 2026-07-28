#include "IRBuilder.h"
#include "IROptimizer.h"
#include "RegisterAllocator.h"
#include "Emitter.h"

extern bool g_showIR;

namespace jc {

static void collectPatternVars(Pattern* pat, std::vector<std::tuple<std::string, ScopeModifier, bool, Pattern*>>& boundVars) {
    if (auto* dp = dynamic_cast<DefaultPattern*>(pat)) {
        collectPatternVars(dp->inner.get(), boundVars);
    } else if (auto* vp = dynamic_cast<VariablePattern*>(pat)) {
        if (vp->name.lexeme != "_") boundVars.push_back({vp->name.lexeme, vp->modifier, vp->isConst, vp});
    } else if (auto* rp = dynamic_cast<RestPattern*>(pat)) {
        if (rp->name.lexeme != "_") boundVars.push_back({rp->name.lexeme, rp->modifier, rp->isConst, rp});
    } else if (auto* lp = dynamic_cast<ListPattern*>(pat)) {
        for (auto& e : lp->elements) collectPatternVars(e.get(), boundVars);
        if (lp->rest) collectPatternVars(lp->rest.get(), boundVars);
    } else if (auto* mp = dynamic_cast<MatrixPattern*>(pat)) {
        for (auto& row : mp->rows) {
            for (auto& e : row) collectPatternVars(e.get(), boundVars);
        }
        if (mp->restRow) collectPatternVars(mp->restRow.get(), boundVars);
    } else if (auto* dictPat = dynamic_cast<DictPattern*>(pat)) {
        for (auto& e : dictPat->entries) collectPatternVars(e.second.get(), boundVars);
        if (dictPat->rest) collectPatternVars(dictPat->rest.get(), boundVars);
    }
}

IRNode* IRBuilder::readVariable(const std::string& name, ResolvedSym sym) {
    // 优先检查是否是递归调用当前闭包
    // 只有当它不是当前作用域的局部变量（即没有被 shadow）时，才使用 GetCurrentClosure
    if (currentFunction && currentFunction->name == name && sym.scope != VarScope::Local && sym.scope != VarScope::State && sym.scope != VarScope::RefParam) {
        IRNode* node = graph->createValueNode(IROp::GetCurrentClosure);
        node->name = name;
        node->setControl(currentControl);
        return node;
    }

    if (sym.scope == VarScope::Global) {
        IRNode* node = graph->createValueNode(IROp::GetGlobal);
        node->name = name;
        node->setControl(currentControl);
        currentControl = node;
        return node;
    } else if (sym.scope == VarScope::Upvalue || sym.scope == VarScope::CapturedState || sym.scope == VarScope::State) {
        int upvalIdx = -1;
        if (sym.scope == VarScope::State) {
            for (int i = 0; i < static_cast<int>(currentFunction->upvalues.size()); ++i) {
                if (currentFunction->upvalues[i].name == name && (currentFunction->upvalues[i].isExplicitState || currentFunction->upvalues[i].isCapturedState)) {
                    upvalIdx = i;
                    break;
                }
            }
            if (upvalIdx == -1) {
                upvalIdx = static_cast<int>(currentFunction->upvalues.size());
                CompiledFunction::UpvalueInfo uv;
                uv.name = name;
                uv.isLocal = false;
                uv.index = 0;
                uv.isRef = false;
                uv.isGlobal = false;
                uv.isExplicitState = true;
                uv.isRefParam = false;
                uv.isCapturedState = false;
                currentFunction->upvalues.push_back(uv);
            }
        } else {
            upvalIdx = resolveUpvalue(name, sym.scope == VarScope::CapturedState);
        }
        IRNode* node = graph->createValueNode(IROp::GetUpvalue);
        node->payload1 = static_cast<uint32_t>(upvalIdx);
        node->name = name;
        node->setControl(currentControl);
        return node;
    } else if (sym.scope == VarScope::RefParam) {
        IRNode* node = graph->createValueNode(IROp::GetRefParam);
        node->payload1 = refParams[name];
        node->name = name;
        node->setControl(currentControl);
        return node;
    } else {
        IRNode* localNode = getLocalNode(name);
        if (localNode) return localNode;
        IRNode* uninit = graph->createConstant(Value::uninit());
        uninit->setControl(currentControl);
        return uninit;
    }
}

void IRBuilder::writeVariable(const std::string& name, IRNode* value, ResolvedSym sym, bool isExplicitLocal, bool isExplicitConst) {
    if (name == "_") return;
    if (sym.scope == VarScope::Global) {
        IROp op = isExplicitConst ? IROp::DefineConstGlobal : IROp::SetGlobal;
        IRNode* node = graph->createNode(op);
        node->setControl(currentControl);
        node->addData(value);
        node->name = name;
        currentControl = node;
    } else if (sym.scope == VarScope::Upvalue || sym.scope == VarScope::CapturedState || sym.scope == VarScope::State) {
        int upvalIdx = -1;
        if (sym.scope == VarScope::State) {
            for (int i = 0; i < static_cast<int>(currentFunction->upvalues.size()); ++i) {
                if (currentFunction->upvalues[i].name == name && (currentFunction->upvalues[i].isExplicitState || currentFunction->upvalues[i].isCapturedState)) {
                    upvalIdx = i;
                    break;
                }
            }
            if (upvalIdx == -1) {
                upvalIdx = static_cast<int>(currentFunction->upvalues.size());
                CompiledFunction::UpvalueInfo uv;
                uv.name = name;
                uv.isLocal = false;
                uv.index = 0;
                uv.isRef = false;
                uv.isGlobal = false;
                uv.isExplicitState = true;
                uv.isRefParam = false;
                uv.isCapturedState = false;
                currentFunction->upvalues.push_back(uv);
            }
        } else {
            upvalIdx = resolveUpvalue(name, sym.scope == VarScope::CapturedState);
        }
        IRNode* node = graph->createNode(IROp::SetUpvalue);
        node->payload1 = static_cast<uint32_t>(upvalIdx);
        node->name = name;
        node->addData(value);
        node->setControl(currentControl);
        currentControl = node;
    } else if (sym.scope == VarScope::RefParam) {
        IRNode* node = graph->createNode(IROp::SetRefParam);
        node->payload1 = refParams[name];
        node->name = name;
        node->addData(value);
        node->setControl(currentControl);
        currentControl = node;
    } else {
        if (currentConstVars.count(name)) {
            if (!isExplicitConst) error("Runtime Error: Cannot modify const variable '" + name + "'.");
            else error("Runtime Error: Cannot redefine const variable '" + name + "'.");
        }
        
        if (isExplicitLocal) {
            declareVariable(name, value);
            currentLocalVars.insert(name);
            if (isExplicitConst) currentConstVars.insert(name);
        } else {
            bool found = false;
            IRNode* origNode = nullptr;
            for (int i = static_cast<int>(envStack.size()) - 1; i >= 0; --i) {
                if (envStack[i].count(name)) {
                    origNode = envStack[i][name];
                    found = true;
                    break;
                }
            }
            
            if (found && capturedLocals.count(name)) {
                IRNode* updateNode = graph->createNode(IROp::UpdateCaptured);
                updateNode->setControl(currentControl);
                updateNode->addData(origNode);
                updateNode->addData(value);
                currentControl = updateNode;
                return;
            }
            
            if (found) {
                for (int i = static_cast<int>(envStack.size()) - 1; i >= 0; --i) {
                    if (envStack[i].count(name)) {
                        envStack[i][name] = value;
                        break;
                    }
                }
            } else {
                if (namespaceScopeDepth != -1) envStack[namespaceScopeDepth][name] = value;
                else envStack[0][name] = value;
            }
            if (isExplicitConst) currentConstVars.insert(name);
        }
    }
}

void IRBuilder::declareVariable(const std::string& name, IRNode* value) {
    if (name == "_") return;
    if (envStack.empty()) return;
    envStack.back()[name] = value;
}

IRBuilder::IRBuilder(IRGraph* graph, std::vector<std::shared_ptr<CompiledFunction>>* compiledFunctions, IRBuilder* parent, CompiledFunction* currentFunction, const std::unordered_map<Expr*, ResolvedSym>* exprSymbols, const std::unordered_map<Pattern*, ResolvedSym>* patternSymbols) 
    : graph(graph), compiledFunctions(compiledFunctions), parent(parent), currentFunction(currentFunction), exprSymbols(exprSymbols), patternSymbols(patternSymbols), currentControl(graph->startNode), lastValue(nullptr) {
    pushScope(); // 压入顶层作用域
}

IRNode* IRBuilder::getLocalNode(const std::string& name) {
    for (int i = static_cast<int>(envStack.size()) - 1; i >= 0; --i) {
        auto it = envStack[i].find(name);
        if (it != envStack[i].end()) return it->second;
    }
    return nullptr;
}

int IRBuilder::resolveUpvalue(const std::string& name, bool isCapturedState) {
    if (!parent || !currentFunction) return -1;

    if (isCapturedState) {
        // 查找显式状态上值 (Explicit State) 或 捕获状态上值 (Captured State)
        for (int i = static_cast<int>(currentFunction->upvalues.size()) - 1; i >= 0; --i) {
            if (currentFunction->upvalues[i].name == name && (currentFunction->upvalues[i].isExplicitState || currentFunction->upvalues[i].isCapturedState)) {
                return i;
            }
        }
        // 如果是 CapturedState，在内部闭包中它可能只是一个普通的 upvalue
        for (int i = static_cast<int>(currentFunction->upvalues.size()) - 1; i >= 0; --i) {
            if (currentFunction->upvalues[i].name == name && !currentFunction->upvalues[i].isExplicitState && !currentFunction->upvalues[i].isCapturedState) {
                return i;
            }
        }
    } else {
        // 查找普通的捕获上值
        for (int i = static_cast<int>(currentFunction->upvalues.size()) - 1; i >= 0; --i) {
            if (currentFunction->upvalues[i].name == name && !currentFunction->upvalues[i].isExplicitState && !currentFunction->upvalues[i].isCapturedState) {
                return i;
            }
        }
    }

    // Check if parent already has this upvalue
    if (parent->currentFunction) {
        int foundIdx = -1;
        if (isCapturedState) {
            for (int i = static_cast<int>(parent->currentFunction->upvalues.size()) - 1; i >= 0; --i) {
                if (parent->currentFunction->upvalues[i].name == name && (parent->currentFunction->upvalues[i].isExplicitState || parent->currentFunction->upvalues[i].isCapturedState)) {
                    foundIdx = i;
                    break;
                }
            }
            if (foundIdx == -1) {
                for (int i = static_cast<int>(parent->currentFunction->upvalues.size()) - 1; i >= 0; --i) {
                    if (parent->currentFunction->upvalues[i].name == name && !parent->currentFunction->upvalues[i].isExplicitState && !parent->currentFunction->upvalues[i].isCapturedState) {
                        foundIdx = i;
                        break;
                    }
                }
            }
        } else {
            for (int i = static_cast<int>(parent->currentFunction->upvalues.size()) - 1; i >= 0; --i) {
                if (parent->currentFunction->upvalues[i].name == name && !parent->currentFunction->upvalues[i].isExplicitState && !parent->currentFunction->upvalues[i].isCapturedState) {
                    foundIdx = i;
                    break;
                }
            }
        }
        
        if (foundIdx != -1) {
            int upvalIdx = static_cast<int>(currentFunction->upvalues.size());
            CompiledFunction::UpvalueInfo uv;
            uv.name = name;
            uv.isLocal = false;
            uv.index = foundIdx;
            uv.isRef = parent->currentFunction->upvalues[foundIdx].isRef;
            uv.isGlobal = false;
            uv.isExplicitState = false;
            uv.isRefParam = false;
            uv.isCapturedState = false;
            currentFunction->upvalues.push_back(uv);
            upvalueTargets.push_back({upvalIdx, false, nullptr});
            return upvalIdx;
        }
    }

    if (!isCapturedState && parent->refParams.count(name)) {
        int upvalIdx = static_cast<int>(currentFunction->upvalues.size());
        CompiledFunction::UpvalueInfo uv;
        uv.name = name;
        uv.isLocal = true;
        uv.index = parent->refParams[name];
        uv.isRef = true;
        uv.isGlobal = false;
        uv.isExplicitState = false;
        uv.isRefParam = true;
        uv.isCapturedState = false;
        currentFunction->upvalues.push_back(uv);
        upvalueTargets.push_back({upvalIdx, true, nullptr});
        return upvalIdx;
    }

    IRNode* localNode = !isCapturedState ? parent->getLocalNode(name) : nullptr;
    if (!localNode && !isCapturedState && parent->currentFunction && parent->currentFunction->name == name) {
        // 如果父函数正是我们要捕获的递归函数本身，直接在父图中生成 GetCurrentClosure
        localNode = parent->graph->createValueNode(IROp::GetCurrentClosure);
        localNode->name = name;
        localNode->setControl(parent->currentControl);
    }
    if (localNode) {
        int upvalIdx = static_cast<int>(currentFunction->upvalues.size());
        CompiledFunction::UpvalueInfo uv;
        uv.name = name;
        uv.isLocal = true;
        uv.index = -1; // Will be resolved after RegisterAllocator
        uv.isRef = false;
        
        IRBuilder* currBuilder = this;
        while (currBuilder && currBuilder->currentFunction) {
            if (currBuilder->currentFunction->name == name) {
                uv.isRef = true;
                break;
            }
            currBuilder = currBuilder->parent;
        }
        
        if (!uv.isRef) {
            IRBuilder* p = parent;
            while (p) {
                if (p->namespaceScopeDepth != -1 && p->namespaceScopeDepth < static_cast<int>(p->envStack.size())) {
                    if (p->envStack[p->namespaceScopeDepth].count(name)) {
                        uv.isRef = true;
                        break;
                    }
                }
                p = p->parent;
            }
        }
        
        uv.isGlobal = false;
        uv.isExplicitState = false;
        uv.isRefParam = false;
        uv.isCapturedState = false;
        currentFunction->upvalues.push_back(uv);
        upvalueTargets.push_back({upvalIdx, true, localNode});
        parent->capturedLocals.insert(name);
        parent->capturedNodesToExtend.push_back(localNode);
        return upvalIdx;
    }

    int upvalue = parent->resolveUpvalue(name, isCapturedState);
    if (upvalue != -1) {
        int upvalIdx = static_cast<int>(currentFunction->upvalues.size());
        CompiledFunction::UpvalueInfo uv;
        uv.name = name;
        uv.isLocal = false;
        uv.index = upvalue;
        uv.isRef = parent->currentFunction->upvalues[upvalue].isRef;
        uv.isGlobal = false;
        uv.isExplicitState = false;
        uv.isRefParam = false;
        uv.isCapturedState = false;
        currentFunction->upvalues.push_back(uv);
        upvalueTargets.push_back({upvalIdx, false, nullptr});
        return upvalIdx;
    }

    return -1;
}

void IRBuilder::buildPatternMatch(Pattern* pat, IRNode* valNode, IRNode* failMerge, ScopeModifier globalMod, bool globalConst, bool isAssignment) {
    auto assignVar = [&](const std::string& name, IRNode* vNode, ResolvedSym sym, ScopeModifier mod, bool isConst) {
        bool isLocal = mod == ScopeModifier::Local;
        if (mod == ScopeModifier::State) {
            IRNode* getVal = readVariable(name, sym);
            IRNode* isUninit = graph->createValueNode(IROp::IsUninit);
            isUninit->addData(getVal);
            isUninit->setControl(currentControl);
            
            IRNode* ifNode = graph->createNode(IROp::If);
            ifNode->addData(isUninit);
            ifNode->setControl(currentControl);
            
            IRNode* ifTrue = graph->createNode(IROp::IfTrue);
            ifTrue->setControl(ifNode);
            
            IRNode* ifFalse = graph->createNode(IROp::IfFalse);
            ifFalse->setControl(ifNode);
            
            currentControl = ifTrue;
            writeVariable(name, vNode, sym, isLocal, isConst);
            IRNode* trueCtrl = currentControl;
            
            IRNode* merge = graph->createNode(IROp::Merge);
            merge->addData(trueCtrl);
            merge->addData(ifFalse);
            currentControl = merge;
        } else {
            writeVariable(name, vNode, sym, isLocal, isConst);
        }
    };

    if (auto* dp = dynamic_cast<DefaultPattern*>(pat)) {
        IRNode* isUninit = graph->createValueNode(IROp::IsUninit);
        isUninit->setControl(currentControl);
        isUninit->addData(valNode);
        
        IRNode* ifNode = graph->createNode(IROp::If);
        ifNode->setControl(currentControl);
        ifNode->addData(isUninit);
        
        IRNode* ifTrue = graph->createNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = graph->createNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        
        currentControl = ifTrue;
        dp->defaultExpr->accept(*this);
        IRNode* defVal = lastValue;
        IRNode* trueCtrl = currentControl;
        
        IRNode* merge = graph->createNode(IROp::Merge);
        merge->addData(trueCtrl);
        merge->addData(ifFalse);
        
        IRNode* phi = graph->createValueNode(IROp::Phi);
        phi->setControl(merge);
        phi->addData(defVal);
        phi->addData(valNode);
        
        currentControl = merge;
        buildPatternMatch(dp->inner.get(), phi, failMerge, globalMod, globalConst, isAssignment);
        return;
    }
    if (auto* vp = dynamic_cast<VariablePattern*>(pat)) {
        IRNode* isUninit = graph->createValueNode(IROp::IsUninit);
        isUninit->setControl(currentControl);
        isUninit->addData(valNode);
        
        IRNode* ifNode = graph->createNode(IROp::If);
        ifNode->setControl(currentControl);
        ifNode->addData(isUninit);
        
        IRNode* ifTrue = graph->createNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        failMerge->addData(ifTrue);
        
        IRNode* ifFalse = graph->createNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        currentControl = ifFalse;

        if (vp->name.lexeme != "_") {
            auto it = patternSymbols->find(pat);
            ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
            ScopeModifier mod = vp->modifier != ScopeModifier::None ? vp->modifier : globalMod;
            
            bool isExplicitConst = vp->isConst || globalConst;
            if (mod == ScopeModifier::Ref && currentFunction) {
                int upvalIdx = resolveUpvalue(vp->name.lexeme, false);
                if (upvalIdx == -1) {
                    CompiledFunction::UpvalueInfo uv;
                    uv.name = vp->name.lexeme;
                    uv.isLocal = false;
                    uv.index = 0;
                    uv.isRef = true;
                    uv.isGlobal = true;
                    uv.isExplicitState = false;
                    uv.isRefParam = false;
                    uv.isCapturedState = false;
                    currentFunction->upvalues.push_back(uv);
                } else {
                    currentFunction->upvalues[upvalIdx].isRef = true;
                }
            }
            assignVar(vp->name.lexeme, valNode, sym, mod, isExplicitConst);
        }
    } else if (auto* lit = dynamic_cast<LiteralPattern*>(pat)) {
        lit->literal->accept(*this);
        IRNode* eqNode = graph->createValueNode(IROp::Eq);
        eqNode->setControl(currentControl);
        eqNode->addData(valNode);
        eqNode->addData(lastValue);
        
        IRNode* ifNode = graph->createNode(IROp::If);
        ifNode->setControl(currentControl);
        ifNode->addData(eqNode);
        
        IRNode* ifTrue = graph->createNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = graph->createNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        failMerge->addData(ifFalse);
        
        currentControl = ifTrue;
    } else if (auto* exprPat = dynamic_cast<ExprPattern*>(pat)) {
        if (isAssignment) {
            if (auto* dot = dynamic_cast<DotAccess*>(exprPat->expr.get())) {
                graph->currentLine = dot->field.line;
                dot->object->accept(*this);
                IRNode* objNode = lastValue;
                
                IRNode* setProp = graph->createValueNode(IROp::SetProperty);
                setProp->setControl(currentControl);
                setProp->addData(objNode);
                setProp->addData(valNode);
                setProp->name = dot->field.lexeme;
                currentControl = setProp;
                
                if (auto* var = dynamic_cast<Variable*>(dot->object.get())) {
                    auto it = exprSymbols->find(var);
                    ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
                    writeVariable(var->name.lexeme, objNode, sym, false, false);
                }
            } else if (auto* idx = dynamic_cast<IndexAccess*>(exprPat->expr.get())) {
                std::vector<IndexAccess*> chain;
                IndexAccess* curr = idx;
                while (curr) {
                    chain.push_back(curr);
                    if (auto* next = dynamic_cast<IndexAccess*>(curr->object.get())) {
                        curr = next;
                    } else {
                        break;
                    }
                }
                std::reverse(chain.begin(), chain.end());
                
                IRNode* dotParentNode = nullptr;
                std::string dotPropName = "";
                IRNode* objNode = nullptr;
                
                if (auto* chainDot = dynamic_cast<DotAccess*>(chain[0]->object.get())) {
                    chainDot->object->accept(*this);
                    dotParentNode = lastValue;
                    dotPropName = chainDot->field.lexeme;
                    
                    IRNode* getProp = graph->createValueNode(IROp::GetProperty);
                    getProp->setControl(currentControl);
                    getProp->addData(dotParentNode);
                    getProp->name = dotPropName;
                    currentControl = getProp;
                    objNode = getProp;
                } else {
                    chain[0]->object->accept(*this);
                    objNode = lastValue;
                }
                
                bool hasSlice = false;
                if (chain.size() == 1) {
                    for (auto& idxExpr : chain[0]->indices) {
                        if (dynamic_cast<SliceExpr*>(idxExpr.get())) {
                            hasSlice = true;
                            break;
                        }
                    }
                }

                IRNode* finalNode = nullptr;
                if (hasSlice) {
                    std::vector<IRNode*> sliceArgs;
                    for (auto& idxExpr : chain[0]->indices) {
                        if (auto* slice = dynamic_cast<SliceExpr*>(idxExpr.get())) {
                            if (slice->start) { slice->start->accept(*this); sliceArgs.push_back(lastValue); }
                            else { IRNode* n = graph->createConstant(Value::none()); n->setControl(currentControl); sliceArgs.push_back(n); }
                            
                            if (slice->end) { slice->end->accept(*this); sliceArgs.push_back(lastValue); }
                            else { IRNode* n = graph->createConstant(Value::none()); n->setControl(currentControl); sliceArgs.push_back(n); }
                            
                            if (slice->step) { slice->step->accept(*this); sliceArgs.push_back(lastValue); }
                            else { IRNode* n = graph->createConstant(Value::none()); n->setControl(currentControl); sliceArgs.push_back(n); }
                        } else {
                            idxExpr->accept(*this); sliceArgs.push_back(lastValue);
                            IRNode* n1 = graph->createConstant(Value::none()); n1->setControl(currentControl); sliceArgs.push_back(n1);
                            IRNode* n2 = graph->createConstant(Value(0.0)); n2->setControl(currentControl); sliceArgs.push_back(n2);
                        }
                    }
                    IRNode* node = graph->createValueNode(IROp::SliceSet);
                    node->setControl(currentControl);
                    node->addData(objNode);
                    for (auto* arg : sliceArgs) node->addData(arg);
                    node->addData(valNode);
                    node->payload1 = static_cast<uint32_t>(chain[0]->indices.size());
                    currentControl = node;
                    finalNode = node;
                } else {
                    std::vector<std::vector<IRNode*>> allIndices;
                    for (size_t i = 0; i < chain.size(); ++i) {
                        std::vector<IRNode*> levelIndices;
                        for (auto& idxExpr : chain[i]->indices) {
                            idxExpr->accept(*this);
                            levelIndices.push_back(lastValue);
                        }
                        allIndices.push_back(levelIndices);
                    }
                    
                    int depth = static_cast<int>(chain.size());
                    if (depth == 1) {
                        IRNode* setIdx = graph->createValueNode(IROp::IndexSet);
                        setIdx->setControl(currentControl);
                        setIdx->addData(objNode);
                        for (auto* iNode : allIndices[0]) setIdx->addData(iNode);
                        setIdx->addData(valNode);
                        setIdx->payload1 = static_cast<uint32_t>(allIndices[0].size());
                        currentControl = setIdx;
                        finalNode = setIdx;
                    } else {
                        std::vector<IRNode*> chainObjs;
                        chainObjs.push_back(objNode);
                        
                        for (size_t i = 0; i < chain.size() - 1; ++i) {
                            IRNode* getNode = graph->createValueNode(IROp::IndexGet);
                            getNode->setControl(currentControl);
                            getNode->addData(chainObjs.back());
                            for (auto* idxNode : allIndices[i]) getNode->addData(idxNode);
                            getNode->payload1 = static_cast<uint32_t>(allIndices[i].size());
                            currentControl = getNode;
                            chainObjs.push_back(getNode);
                        }
                        
                        IRNode* setNode = graph->createValueNode(IROp::IndexSet);
                        setNode->setControl(currentControl);
                        setNode->addData(chainObjs.back());
                        for (auto* iNode : allIndices.back()) setNode->addData(iNode);
                        setNode->addData(valNode);
                        setNode->payload1 = static_cast<uint32_t>(allIndices.back().size());
                        currentControl = setNode;
                        finalNode = setNode;
                        
                        for (int level = depth - 2; level >= 0; --level) {
                            IRNode* backSetNode = graph->createValueNode(IROp::IndexSet);
                            backSetNode->setControl(currentControl);
                            backSetNode->addData(chainObjs[level]);
                            for (auto* iNode : allIndices[level]) backSetNode->addData(iNode);
                            backSetNode->addData(finalNode);
                            backSetNode->payload1 = static_cast<uint32_t>(allIndices[level].size());
                            currentControl = backSetNode;
                            finalNode = backSetNode;
                        }
                    }
                }
                
                if (auto* var = dynamic_cast<Variable*>(chain[0]->object.get())) {
                    auto it = exprSymbols->find(var);
                    ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
                    writeVariable(var->name.lexeme, finalNode, sym, false, false);
                } else if (dotParentNode) {
                    IRNode* setProp = graph->createValueNode(IROp::SetProperty);
                    setProp->setControl(currentControl);
                    setProp->addData(dotParentNode);
                    setProp->addData(finalNode);
                    setProp->name = dotPropName;
                    currentControl = setProp;
                }
            } else {
                error("Syntax Error: Invalid L-value in destructuring assignment.");
            }
        } else {
            exprPat->expr->accept(*this);
            IRNode* eqNode = graph->createValueNode(IROp::Eq);
            eqNode->setControl(currentControl);
            eqNode->addData(valNode);
            eqNode->addData(lastValue);
            
            IRNode* ifNode = graph->createNode(IROp::If);
            ifNode->setControl(currentControl);
            ifNode->addData(eqNode);
            
            IRNode* ifTrue = graph->createNode(IROp::IfTrue);
            ifTrue->setControl(ifNode);
            
            IRNode* ifFalse = graph->createNode(IROp::IfFalse);
            ifFalse->setControl(ifNode);
            failMerge->addData(ifFalse);
            
            currentControl = ifTrue;
        }
    } else if (auto* dap = dynamic_cast<DynamicAssertPattern*>(pat)) {
        dap->expr->accept(*this);
        IRNode* eqNode = graph->createValueNode(IROp::Eq);
        eqNode->setControl(currentControl);
        eqNode->addData(valNode);
        eqNode->addData(lastValue);
        
        IRNode* ifNode = graph->createNode(IROp::If);
        ifNode->setControl(currentControl);
        ifNode->addData(eqNode);
        
        IRNode* ifTrue = graph->createNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = graph->createNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        failMerge->addData(ifFalse);
        
        currentControl = ifTrue;
    } else if (auto* lp = dynamic_cast<ListPattern*>(pat)) {
        int restIndex = -1;
        for (size_t i = 0; i < lp->elements.size(); ++i) {
            if (dynamic_cast<RestPattern*>(lp->elements[i].get())) {
                restIndex = static_cast<int>(i);
                break;
            }
        }

        bool hasRest = (restIndex != -1) || (lp->rest != nullptr);
        uint32_t minCols = 0;
        if (restIndex != -1) {
            minCols = static_cast<uint32_t>(lp->elements.size() - 1);
        } else {
            int requiredCols = 0;
            for (size_t i = 0; i < lp->elements.size(); ++i) {
                if (!dynamic_cast<DefaultPattern*>(lp->elements[i].get())) {
                    requiredCols = static_cast<int>(i) + 1;
                }
            }
            minCols = static_cast<uint32_t>(requiredCols);
        }
        uint32_t maxCols = hasRest ? 0xFFFFFFFF : static_cast<uint32_t>(lp->elements.size());

        IRNode* shapeNode = graph->createValueNode(IROp::MatchShape);
        shapeNode->setControl(currentControl);
        shapeNode->addData(valNode);
        shapeNode->payload1 = 1; // minRows
        shapeNode->payload2 = 1; // maxRows
        shapeNode->payload3 = minCols;
        shapeNode->payload4 = maxCols;
        shapeNode->payload5 = 2; // exactMask = 2 (1D pattern)
        
        IRNode* ifNode = graph->createNode(IROp::If);
        ifNode->setControl(currentControl);
        ifNode->addData(shapeNode);
        
        IRNode* ifTrue = graph->createNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = graph->createNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        failMerge->addData(ifFalse);
        
        currentControl = ifTrue;

        for (size_t i = 0; i < lp->elements.size(); ++i) {
            if (static_cast<int>(i) == restIndex) {
                auto* restPat = static_cast<RestPattern*>(lp->elements[i].get());
                IRNode* startNode = graph->createConstant(Value(static_cast<double>(i)));
                startNode->setControl(currentControl);
                
                int tailCount = static_cast<int>(lp->elements.size()) - 1 - static_cast<int>(i);
                IRNode* endNode = tailCount > 0 ? graph->createConstant(Value(static_cast<double>(-tailCount))) : graph->createConstant(Value::none());
                endNode->setControl(currentControl);
                
                IRNode* stepNode = graph->createConstant(Value::none());
                stepNode->setControl(currentControl);
                
                IRNode* sliceNode = graph->createValueNode(IROp::SliceGet);
                sliceNode->setControl(currentControl);
                sliceNode->addData(valNode);
                sliceNode->addData(startNode);
                sliceNode->addData(endNode);
                sliceNode->addData(stepNode);
                sliceNode->payload1 = 1;
                currentControl = sliceNode;
                
                if (restPat->name.lexeme != "_") {
                    auto it = patternSymbols->find(restPat);
                    ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
                    ScopeModifier mod = restPat->modifier != ScopeModifier::None ? restPat->modifier : globalMod;
                    bool isExplicitConst = restPat->isConst || globalConst;
                    
                    assignVar(restPat->name.lexeme, sliceNode, sym, mod, isExplicitConst);
                }
                continue;
            }
            
            IRNode* idxNode = nullptr;
            if (restIndex != -1 && static_cast<int>(i) > restIndex) {
                int offsetFromEnd = static_cast<int>(lp->elements.size()) - static_cast<int>(i);
                idxNode = graph->createConstant(Value(static_cast<double>(-offsetFromEnd)));
            } else {
                idxNode = graph->createConstant(Value(static_cast<double>(i)));
            }
            idxNode->setControl(currentControl);
            
            IRNode* elemNode = graph->createValueNode(IROp::IndexGet);
            elemNode->setControl(currentControl);
            elemNode->addData(valNode);
            elemNode->addData(idxNode);
            elemNode->payload1 = 1;
            elemNode->payload2 = 1; // noThrow
            currentControl = elemNode;
            
            buildPatternMatch(lp->elements[i].get(), elemNode, failMerge, globalMod, globalConst, isAssignment);
        }

        if (lp->rest && lp->rest->name.lexeme != "_") {
            auto* restPat = lp->rest.get();
            IRNode* startNode = graph->createConstant(Value(static_cast<double>(lp->elements.size())));
            startNode->setControl(currentControl);
            
            IRNode* endNode = graph->createConstant(Value::none());
            endNode->setControl(currentControl);
            
            IRNode* stepNode = graph->createConstant(Value::none());
            stepNode->setControl(currentControl);
            
            IRNode* sliceNode = graph->createValueNode(IROp::SliceGet);
            sliceNode->setControl(currentControl);
            sliceNode->addData(valNode);
            sliceNode->addData(startNode);
            sliceNode->addData(endNode);
            sliceNode->addData(stepNode);
            sliceNode->payload1 = 1;
            currentControl = sliceNode;
            
            auto it = patternSymbols->find(restPat);
            ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
            ScopeModifier mod = restPat->modifier != ScopeModifier::None ? restPat->modifier : globalMod;
            bool isExplicitConst = restPat->isConst || globalConst;
            
            if (mod == ScopeModifier::Ref && currentFunction) {
                int upvalIdx = resolveUpvalue(restPat->name.lexeme, false);
                if (upvalIdx == -1) {
                    CompiledFunction::UpvalueInfo uv;
                    uv.name = restPat->name.lexeme;
                    uv.isLocal = false;
                    uv.index = 0;
                    uv.isRef = true;
                    uv.isGlobal = true;
                    uv.isExplicitState = false;
                    uv.isRefParam = false;
                    uv.isCapturedState = false;
                    currentFunction->upvalues.push_back(uv);
                } else {
                    currentFunction->upvalues[upvalIdx].isRef = true;
                }
            }
            assignVar(restPat->name.lexeme, sliceNode, sym, mod, isExplicitConst);
        }
    } else if (auto* mp = dynamic_cast<MatrixPattern*>(pat)) {
        uint32_t minRows = 0;
        uint32_t maxRows = 0;
        bool hasRestRow = mp->restRow != nullptr;
        
        int reqRows = 0;
        for (size_t i = 0; i < mp->rows.size(); ++i) {
            bool rowHasReq = false;
            for (size_t j = 0; j < mp->rows[i].size(); ++j) {
                if (!dynamic_cast<DefaultPattern*>(mp->rows[i][j].get()) && !dynamic_cast<RestPattern*>(mp->rows[i][j].get())) {
                    rowHasReq = true;
                    break;
                }
            }
            if (rowHasReq) reqRows = static_cast<int>(i) + 1;
        }
        minRows = static_cast<uint32_t>(reqRows);
        maxRows = hasRestRow ? 0xFFFFFFFF : static_cast<uint32_t>(mp->rows.size());

        uint32_t minCols = 0;
        uint32_t maxCols = 0;
        bool hasRestCol = false;
        if (!mp->rows.empty()) {
            int maxReqCols = 0;
            for (size_t i = 0; i < mp->rows.size(); ++i) {
                int reqCols = 0;
                bool rowHasRest = false;
                for (size_t j = 0; j < mp->rows[i].size(); ++j) {
                    if (dynamic_cast<RestPattern*>(mp->rows[i][j].get())) {
                        rowHasRest = true;
                    } else if (!dynamic_cast<DefaultPattern*>(mp->rows[i][j].get())) {
                        reqCols = static_cast<int>(j) + 1;
                    }
                }
                if (rowHasRest) {
                    hasRestCol = true;
                    reqCols = static_cast<int>(mp->rows[i].size() - 1);
                }
                if (reqCols > maxReqCols) maxReqCols = reqCols;
            }
            minCols = static_cast<uint32_t>(maxReqCols);
            maxCols = hasRestCol ? 0xFFFFFFFF : static_cast<uint32_t>(mp->rows[0].size());
        }

        IRNode* shapeNode = graph->createValueNode(IROp::MatchShape);
        shapeNode->setControl(currentControl);
        shapeNode->addData(valNode);
        shapeNode->payload1 = minRows;
        shapeNode->payload2 = maxRows;
        shapeNode->payload3 = minCols;
        shapeNode->payload4 = maxCols;
        shapeNode->payload5 = 0; // exactMask = 0 (2D pattern)
        
        IRNode* ifNode = graph->createNode(IROp::If);
        ifNode->setControl(currentControl);
        ifNode->addData(shapeNode);
        
        IRNode* ifTrue = graph->createNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = graph->createNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        failMerge->addData(ifFalse);
        
        currentControl = ifTrue;

        for (size_t i = 0; i < mp->rows.size(); ++i) {
            int rowRestColIndex = -1;
            for (size_t j = 0; j < mp->rows[i].size(); ++j) {
                if (dynamic_cast<RestPattern*>(mp->rows[i][j].get())) {
                    rowRestColIndex = static_cast<int>(j);
                    break;
                }
            }

            for (size_t j = 0; j < mp->rows[i].size(); ++j) {
                if (static_cast<int>(j) == rowRestColIndex) {
                    // Handle rest column
                    auto* restPat = static_cast<RestPattern*>(mp->rows[i][j].get());
                    if (restPat->name.lexeme != "_") {
                        IRNode* rStartNode = graph->createConstant(Value(static_cast<double>(i)));
                        rStartNode->setControl(currentControl);
                        IRNode* rEndNode = graph->createConstant(Value(static_cast<double>(i + 1)));
                        rEndNode->setControl(currentControl);
                        IRNode* rStepNode = graph->createConstant(Value::none());
                        rStepNode->setControl(currentControl);

                        IRNode* cStartNode = graph->createConstant(Value(static_cast<double>(j)));
                        cStartNode->setControl(currentControl);
                        int tailCount = static_cast<int>(mp->rows[i].size()) - 1 - static_cast<int>(j);
                        IRNode* cEndNode = tailCount > 0 ? graph->createConstant(Value(static_cast<double>(-tailCount))) : graph->createConstant(Value::none());
                        cEndNode->setControl(currentControl);
                        IRNode* cStepNode = graph->createConstant(Value::none());
                        cStepNode->setControl(currentControl);

                        IRNode* sliceNode = graph->createValueNode(IROp::SliceGet);
                        sliceNode->setControl(currentControl);
                        sliceNode->addData(valNode);
                        sliceNode->addData(rStartNode);
                        sliceNode->addData(rEndNode);
                        sliceNode->addData(rStepNode);
                        sliceNode->addData(cStartNode);
                        sliceNode->addData(cEndNode);
                        sliceNode->addData(cStepNode);
                        sliceNode->payload1 = 2;
                        currentControl = sliceNode;

                        auto it = patternSymbols->find(restPat);
                        ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
                        ScopeModifier mod = restPat->modifier != ScopeModifier::None ? restPat->modifier : globalMod;
                        bool isExplicitConst = restPat->isConst || globalConst;
            
                        if (mod == ScopeModifier::Ref && currentFunction) {
                            int upvalIdx = resolveUpvalue(restPat->name.lexeme, false);
                            if (upvalIdx == -1) {
                                CompiledFunction::UpvalueInfo uv;
                                uv.name = restPat->name.lexeme;
                                uv.isLocal = false;
                                uv.index = 0;
                                uv.isRef = true;
                                uv.isGlobal = true;
                                uv.isExplicitState = false;
                                uv.isRefParam = false;
                                uv.isCapturedState = false;
                                currentFunction->upvalues.push_back(uv);
                            } else {
                                currentFunction->upvalues[upvalIdx].isRef = true;
                            }
                        }
                        assignVar(restPat->name.lexeme, sliceNode, sym, mod, isExplicitConst);
                    }
                    continue;
                }

                IRNode* rIdxNode = graph->createConstant(Value(static_cast<double>(i)));
                rIdxNode->setControl(currentControl);

                IRNode* cIdxNode = nullptr;
                if (rowRestColIndex != -1 && static_cast<int>(j) > rowRestColIndex) {
                    int offsetFromEnd = static_cast<int>(mp->rows[i].size()) - static_cast<int>(j);
                    cIdxNode = graph->createConstant(Value(static_cast<double>(-offsetFromEnd)));
                } else {
                    cIdxNode = graph->createConstant(Value(static_cast<double>(j)));
                }
                cIdxNode->setControl(currentControl);

                IRNode* elemNode = graph->createValueNode(IROp::IndexGet);
                elemNode->setControl(currentControl);
                elemNode->addData(valNode);
                elemNode->addData(rIdxNode);
                elemNode->addData(cIdxNode);
                elemNode->payload1 = 2;
                elemNode->payload2 = 1; // noThrow
                currentControl = elemNode;

                buildPatternMatch(mp->rows[i][j].get(), elemNode, failMerge, globalMod, globalConst, isAssignment);
            }
        }

        if (hasRestRow && mp->restRow->name.lexeme != "_") {
            auto* restPat = mp->restRow.get();
            IRNode* rStartNode = graph->createConstant(Value(static_cast<double>(mp->rows.size())));
            rStartNode->setControl(currentControl);
            IRNode* rEndNode = graph->createConstant(Value::none());
            rEndNode->setControl(currentControl);
            IRNode* rStepNode = graph->createConstant(Value::none());
            rStepNode->setControl(currentControl);

            IRNode* cStartNode = graph->createConstant(Value(0.0));
            cStartNode->setControl(currentControl);
            IRNode* cEndNode = graph->createConstant(Value::none());
            cEndNode->setControl(currentControl);
            IRNode* cStepNode = graph->createConstant(Value::none());
            cStepNode->setControl(currentControl);

            IRNode* sliceNode = graph->createValueNode(IROp::SliceGet);
            sliceNode->setControl(currentControl);
            sliceNode->addData(valNode);
            sliceNode->addData(rStartNode);
            sliceNode->addData(rEndNode);
            sliceNode->addData(rStepNode);
            sliceNode->addData(cStartNode);
            sliceNode->addData(cEndNode);
            sliceNode->addData(cStepNode);
            sliceNode->payload1 = 2;
            currentControl = sliceNode;

            auto it = patternSymbols->find(restPat);
            ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
            ScopeModifier mod = restPat->modifier != ScopeModifier::None ? restPat->modifier : globalMod;
            bool isExplicitConst = restPat->isConst || globalConst;
            
            if (mod == ScopeModifier::Ref && currentFunction) {
                int upvalIdx = resolveUpvalue(restPat->name.lexeme, false);
                if (upvalIdx == -1) {
                    CompiledFunction::UpvalueInfo uv;
                    uv.name = restPat->name.lexeme;
                    uv.isLocal = false;
                    uv.index = 0;
                    uv.isRef = true;
                    uv.isGlobal = true;
                    uv.isExplicitState = false;
                    uv.isRefParam = false;
                    uv.isCapturedState = false;
                    currentFunction->upvalues.push_back(uv);
                } else {
                    currentFunction->upvalues[upvalIdx].isRef = true;
                }
            }
            assignVar(restPat->name.lexeme, sliceNode, sym, mod, isExplicitConst);
        }
    } else if (auto* dp = dynamic_cast<DictPattern*>(pat)) {
        auto createTypeCheck = [&](const std::string& typeName) {
            IRNode* typeNode = graph->createValueNode(IROp::GetGlobal);
            typeNode->name = typeName;
            typeNode->setControl(currentControl);
            currentControl = typeNode;
            
            IRNode* matchNode = graph->createValueNode(IROp::MatchType);
            matchNode->setControl(currentControl);
            matchNode->addData(valNode);
            matchNode->addData(typeNode);
            return matchNode;
        };

        IRNode* isDict = createTypeCheck("dict");
        IRNode* isInst = createTypeCheck("instance");
        IRNode* isNs = createTypeCheck("namespace_type");

        IRNode* or1 = graph->createValueNode(IROp::BitOr);
        or1->setControl(currentControl);
        or1->addData(isDict);
        or1->addData(isInst);

        IRNode* or2 = graph->createValueNode(IROp::BitOr);
        or2->setControl(currentControl);
        or2->addData(or1);
        or2->addData(isNs);

        IRNode* toBool = graph->createValueNode(IROp::ToBool);
        toBool->setControl(currentControl);
        toBool->addData(or2);

        IRNode* ifNode = graph->createNode(IROp::If);
        ifNode->setControl(currentControl);
        ifNode->addData(toBool);
        
        IRNode* ifTrue = graph->createNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = graph->createNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        failMerge->addData(ifFalse);
        
        currentControl = ifTrue;

        for (auto& entry : dp->entries) {
            IRNode* keyNode = graph->createConstant(Value(entry.first));
            keyNode->setControl(currentControl);
            
            IRNode* elemNode = graph->createValueNode(IROp::IndexGet);
            elemNode->setControl(currentControl);
            elemNode->addData(valNode);
            elemNode->addData(keyNode);
            elemNode->payload1 = 1;
            elemNode->payload2 = 1; // noThrow
            currentControl = elemNode;
            
            buildPatternMatch(entry.second.get(), elemNode, failMerge, globalMod, globalConst, isAssignment);
        }

        if (dp->rest && dp->rest->name.lexeme != "_") {
            auto* restPat = dp->rest.get();
            
            std::vector<IRNode*> keyNodes;
            for (auto& entry : dp->entries) {
                IRNode* kNode = graph->createConstant(Value(entry.first));
                kNode->setControl(currentControl);
                keyNodes.push_back(kNode);
            }
            
            IRNode* excludeList = graph->createValueNode(IROp::BuildList);
            excludeList->setControl(currentControl);
            for (auto* k : keyNodes) excludeList->addData(k);
            excludeList->payload1 = static_cast<uint32_t>(keyNodes.size());
            currentControl = excludeList;
            
            IRNode* restNode = graph->createValueNode(IROp::DictRest);
            restNode->setControl(currentControl);
            restNode->addData(valNode);
            restNode->addData(excludeList);
            currentControl = restNode;
            
            auto it = patternSymbols->find(restPat);
            ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
            ScopeModifier mod = restPat->modifier != ScopeModifier::None ? restPat->modifier : globalMod;
            bool isExplicitConst = restPat->isConst || globalConst;
            
            if (mod == ScopeModifier::Ref && currentFunction) {
                int upvalIdx = resolveUpvalue(restPat->name.lexeme, false);
                if (upvalIdx == -1) {
                    CompiledFunction::UpvalueInfo uv;
                    uv.name = restPat->name.lexeme;
                    uv.isLocal = false;
                    uv.index = 0;
                    uv.isRef = true;
                    uv.isGlobal = true;
                    uv.isExplicitState = false;
                    uv.isRefParam = false;
                    uv.isCapturedState = false;
                    currentFunction->upvalues.push_back(uv);
                } else {
                    currentFunction->upvalues[upvalIdx].isRef = true;
                }
            }
            assignVar(restPat->name.lexeme, restNode, sym, mod, isExplicitConst);
        }
    }
}

void IRBuilder::buildCompClause(ListCompExpr* expr, size_t clauseIdx, IRNode* listNode) {
    if (clauseIdx >= expr->clauses.size()) {
        expr->valueExpr->accept(*this);
        IRNode* valNode = lastValue;
        IRNode* appendNode = graph->createNode(IROp::ListAppend);
        appendNode->setControl(currentControl);
        appendNode->addData(listNode);
        appendNode->addData(valNode);
        currentControl = appendNode;
        return;
    }
    
    auto& clause = expr->clauses[clauseIdx];
    clause.iterable->accept(*this);
    IRNode* iterNode = graph->createValueNode(IROp::IterInit);
    iterNode->setControl(currentControl);
    iterNode->addData(lastValue);
    
    auto isDestructPattern = [](Pattern* p) {
        while (auto* dp = dynamic_cast<DefaultPattern*>(p)) p = dp->inner.get();
        return dynamic_cast<ListPattern*>(p) != nullptr || dynamic_cast<MatrixPattern*>(p) != nullptr;
    };
    iterNode->payload1 = isDestructPattern(clause.pattern.get()) ? 1 : 0;
    currentControl = iterNode;
    
    IRNode* loopNode = graph->createNode(IROp::Loop);
    loopNode->addData(currentControl);
    
    std::vector<std::unordered_map<std::string, IRNode*>> loopPhisStack(envStack.size());
    for (size_t i = 0; i < envStack.size(); ++i) {
        for (const auto& pair : envStack[i]) {
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(loopNode);
            phi->addData(pair.second);
            phi->name = pair.first;
            loopPhisStack[i][pair.first] = phi;
        }
    }
    envStack = loopPhisStack;
    currentControl = loopNode;
    
    IRNode* exitControl = graph->createNode(IROp::Merge);

    IRNode* nextNode = graph->createValueNode(IROp::IterNext);
    nextNode->setControl(currentControl);
    nextNode->addData(iterNode);
    currentControl = nextNode;
    
    IRNode* isUninitNode = graph->createValueNode(IROp::IsUninit);
    isUninitNode->setControl(currentControl);
    isUninitNode->addData(nextNode);
    
    IRNode* ifNode = graph->createNode(IROp::If);
    ifNode->setControl(currentControl);
    ifNode->addData(isUninitNode);
    
    IRNode* ifTrue = graph->createNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    exitControl->addData(ifTrue);
    auto exitEnvStack = envStack;
    
    IRNode* ifFalse = graph->createNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    currentControl = ifFalse;
    
    auto envBeforePattern = envStack;
    IRNode* failMerge = graph->createNode(IROp::Merge);
    buildPatternMatch(clause.pattern.get(), nextNode, failMerge, ScopeModifier::Local, false, true);
    
    auto envBeforeCond = envStack;
    IRNode* condFailMerge = graph->createNode(IROp::Merge);
    for (auto& cond : clause.conditions) {
        cond->accept(*this);
        IRNode* condIfNode = graph->createNode(IROp::If);
        condIfNode->setControl(currentControl);
        condIfNode->addData(lastValue);
        
        IRNode* condIfTrue = graph->createNode(IROp::IfTrue);
        condIfTrue->setControl(condIfNode);
        
        IRNode* condIfFalse = graph->createNode(IROp::IfFalse);
        condIfFalse->setControl(condIfNode);
        condFailMerge->addData(condIfFalse);
        
        currentControl = condIfTrue;
    }
    
    buildCompClause(expr, clauseIdx + 1, listNode);
    
    loopNode->addData(currentControl);
    if (!condFailMerge->dataInputs.empty()) loopNode->addData(condFailMerge);
    if (!failMerge->dataInputs.empty()) loopNode->addData(failMerge);
    
    for (size_t i = 0; i < loopPhisStack.size(); ++i) {
        for (auto& pair : loopPhisStack[i]) {
            ResolvedSym rs; rs.scope = VarScope::Local;
            pair.second->addData(readVariable(pair.first, rs));
            if (!condFailMerge->dataInputs.empty()) {
                pair.second->addData(envBeforeCond[i].count(pair.first) ? envBeforeCond[i].at(pair.first) : graph->createConstant(Value::none()));
            }
            if (!failMerge->dataInputs.empty()) {
                pair.second->addData(envBeforePattern[i].count(pair.first) ? envBeforePattern[i].at(pair.first) : graph->createConstant(Value::none()));
            }
        }
    }
    
    envStack = exitEnvStack;
    currentControl = exitControl;
}

void IRBuilder::build(Expr* ast) {
    if (isInitMethod && instanceFieldsToInit) {
        for (auto& f : *instanceFieldsToInit) {
            f.value->accept(*this);
            IRNode* valNode = lastValue;
            IRNode* selfNode = graph->createValueNode(IROp::GetSelf);
            selfNode->setControl(currentControl);
            
            IROp op;
            if (f.isLocal && f.isConst) op = IROp::DefinePrivateConst;
            else if (f.isLocal) op = IROp::DefinePrivate;
            else if (f.isConst) op = IROp::DefinePropConst;
            else op = IROp::DefineProp;
            
            IRNode* defNode = graph->createValueNode(op);
            defNode->setControl(currentControl);
            defNode->addData(selfNode);
            defNode->addData(valNode);
            defNode->name = f.name.lexeme;
            currentControl = defNode;
        }
    }

    ast->accept(*this);
    
    // 自动在末尾插入 Return 节点
    IRNode* retVal = nullptr;
    if (lastValue) {
        retVal = lastValue;
    } else {
        retVal = graph->createConstant(Value::none());
        retVal->setControl(currentControl);
    }
    
    if (currentReturnTypeHint) {
        IRNode* assertNode = graph->createNode(IROp::AssertReturnType);
        assertNode->setControl(currentControl);
        assertNode->addData(retVal);
        currentControl = assertNode;
    }

    if (currentControl->op != IROp::Return && currentControl->op != IROp::Throw) {
        IRNode* retNode = graph->createNode(IROp::Return);
        retNode->setControl(currentControl);
        retNode->addData(retVal);
        recordExitNode(retNode);
        currentControl = retNode;
    }

    // 为所有退出节点添加被捕获变量的伪依赖，延长其生命周期
    for (auto& info : exitNodes) {
        for (const auto& pair : info.activeVars) {
            if (capturedLocals.count(pair.first)) {
                info.node->addData(pair.second);
            }
        }
        for (IRNode* n : capturedNodesToExtend) {
            info.node->addData(n);
        }
    }
}

void IRBuilder::visitLiteral(Literal* expr) {
    Value val;
    if (expr->isKeyword) {
        if (expr->value == "true") val = Value(true);
        else if (expr->value == "false") val = Value(false);
        else val = Value::none();
    } else if (expr->isString) {
        val = Value(expr->value);
    } else if (expr->isImaginary) {
        const std::string& s = expr->value;
        double imagPart = 0.0;
        if (s.length() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B' || s[1] == 'o' || s[1] == 'O')) {
            int base = 10;
            if (s[1] == 'x' || s[1] == 'X') base = 16;
            else if (s[1] == 'b' || s[1] == 'B') base = 2;
            else if (s[1] == 'o' || s[1] == 'O') base = 8;
            std::string numPart = s.substr(2);
            try {
                imagPart = BaseNum::fromString(numPart, base).getValue().toDouble();
            } catch (...) {
                error("Syntax Error: Invalid imaginary literal '" + s + "'.");
            }
        } else {
            imagPart = std::stod(s);
        }
        val = Value(Complex(0.0, imagPart));
    } else {
        const std::string& s = expr->value;
        if (s.length() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B' || s[1] == 'o' || s[1] == 'O')) {
            int base = 10;
            if (s[1] == 'x' || s[1] == 'X') base = 16;
            else if (s[1] == 'b' || s[1] == 'B') base = 2;
            else if (s[1] == 'o' || s[1] == 'O') base = 8;
            std::string numPart = s.substr(2);
            try {
                val = BaseNum::fromString(numPart, base).getValue();
            } catch (...) {
                error("Syntax Error: Invalid integer literal '" + s + "'.");
            }
        } else if (s.find('.') == std::string::npos &&
            s.find('e') == std::string::npos &&
            s.find('E') == std::string::npos) {
            try { val = Value(jc::BigInt(s)); }
            catch (...) { val = Value(std::stod(s)); }
        } else {
            val = Value(std::stod(s));
        }
    }
    lastValue = graph->createConstant(val);
    lastValue->setControl(currentControl);
}

void IRBuilder::visitBinary(Binary* expr) {
    graph->currentLine = expr->op.line;
    if (expr->op.type == TokenType::PIPE) {
        expr->right->accept(*this);
        IRNode* calleeNode = lastValue;
        
        IRCallSignature sig;
        if (auto* var = dynamic_cast<Variable*>(expr->left.get())) {
            std::string name = var->name.lexeme;
            if (refParams.count(name)) {
                ResolvedSym rs; rs.scope = VarScope::RefParam;
                sig.refs.push_back({0, 4, name, -1, readVariable(name, rs)});
            } else {
                IRNode* localNode = getLocalNode(name);
                if (localNode) {
                    capturedLocals.insert(name);
                    capturedNodesToExtend.push_back(localNode);
                    sig.refs.push_back({0, 2, name, -1, localNode});
                } else {
                    int upvalIdx = -1;
                    if (currentFunction) {
                        for (int j = static_cast<int>(currentFunction->upvalues.size()) - 1; j >= 0; --j) {
                            if (currentFunction->upvalues[j].name == name) {
                                upvalIdx = j;
                                break;
                            }
                        }
                    }
                    if (upvalIdx == -1) {
                        auto varIt = exprSymbols->find(var);
                        ResolvedSym rs = varIt != exprSymbols->end() ? varIt->second : ResolvedSym{};
                        upvalIdx = resolveUpvalue(name, rs.scope == VarScope::CapturedState);
                    }
                    
                    if (upvalIdx != -1) {
                        sig.refs.push_back({0, 3, name, upvalIdx, nullptr});
                    } else {
                        sig.refs.push_back({0, 1, name, -1, nullptr});
                    }
                }
            }
        }
        
        expr->left->accept(*this);
        IRNode* argNode = lastValue;
        
        if (!sig.refs.empty()) {
            graph->callSignatures.push_back(sig);
            IRNode* passRefsNode = graph->createNode(IROp::PassRefs);
            passRefsNode->setControl(currentControl);
            passRefsNode->payload1 = static_cast<uint32_t>(graph->callSignatures.size() - 1);
            for (auto& ref : sig.refs) {
                if (ref.localNode) passRefsNode->addData(ref.localNode);
            }
            currentControl = passRefsNode;
        }
        
        IRNode* callNode = graph->createValueNode(IROp::Call);
        callNode->setControl(currentControl);
        callNode->addData(calleeNode);
        callNode->addData(argNode);
        callNode->payload1 = 1;
        
        currentControl = callNode;
        lastValue = callNode;
        return;
    }

    if (expr->op.type == TokenType::AND_AND || expr->op.type == TokenType::OR_OR) {
        expr->left->accept(*this);
        IRNode* leftVal = lastValue;
        
        IRNode* toBoolNode = graph->createValueNode(IROp::ToBool);
        toBoolNode->setControl(currentControl);
        toBoolNode->addData(leftVal);
        
        IRNode* ifNode = graph->createNode(IROp::If);
        ifNode->setControl(currentControl);
        ifNode->addData(toBoolNode);
        
        IRNode* ifTrue = graph->createNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = graph->createNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        
        auto baseEnv = envStack;
        
        IRNode* mergeNode = graph->createNode(IROp::Merge);
        IRNode* resultPhi = graph->createValueNode(IROp::Phi);
        resultPhi->setControl(mergeNode);
        
        if (expr->op.type == TokenType::AND_AND) {
            currentControl = ifTrue;
            expr->right->accept(*this);
            IRNode* rightToBool = graph->createValueNode(IROp::ToBool);
            rightToBool->setControl(currentControl);
            rightToBool->addData(lastValue);
            
            mergeNode->addData(currentControl);
            resultPhi->addData(rightToBool);
            auto rightEnv = envStack;
            
            envStack = baseEnv;
            currentControl = ifFalse;
            mergeNode->addData(currentControl);
            resultPhi->addData(toBoolNode);
            
            envStack = baseEnv;
            for (size_t i = 0; i < baseEnv.size(); ++i) {
                std::unordered_set<std::string> modifiedVars;
                for (const auto& pair : rightEnv[i]) {
                    if (baseEnv[i].count(pair.first) && baseEnv[i].at(pair.first) != pair.second) modifiedVars.insert(pair.first);
                }
                for (const auto& name : modifiedVars) {
                    IRNode* tNode = rightEnv[i].count(name) ? rightEnv[i].at(name) : baseEnv[i].at(name);
                    IRNode* eNode = baseEnv[i].at(name);
                    IRNode* phi = graph->createValueNode(IROp::Phi);
                    phi->setControl(mergeNode);
                    phi->addData(tNode);
                    phi->addData(eNode);
                    phi->name = name;
                    envStack[i][name] = phi;
                }
            }
        } else {
            currentControl = ifTrue;
            mergeNode->addData(currentControl);
            resultPhi->addData(toBoolNode);
            
            envStack = baseEnv;
            currentControl = ifFalse;
            expr->right->accept(*this);
            IRNode* rightToBool = graph->createValueNode(IROp::ToBool);
            rightToBool->setControl(currentControl);
            rightToBool->addData(lastValue);
            
            mergeNode->addData(currentControl);
            resultPhi->addData(rightToBool);
            auto rightEnv = envStack;
            
            envStack = baseEnv;
            for (size_t i = 0; i < baseEnv.size(); ++i) {
                std::unordered_set<std::string> modifiedVars;
                for (const auto& pair : rightEnv[i]) {
                    if (baseEnv[i].count(pair.first) && baseEnv[i].at(pair.first) != pair.second) modifiedVars.insert(pair.first);
                }
                for (const auto& name : modifiedVars) {
                    IRNode* tNode = baseEnv[i].at(name);
                    IRNode* eNode = rightEnv[i].count(name) ? rightEnv[i].at(name) : baseEnv[i].at(name);
                    IRNode* phi = graph->createValueNode(IROp::Phi);
                    phi->setControl(mergeNode);
                    phi->addData(tNode);
                    phi->addData(eNode);
                    phi->name = name;
                    envStack[i][name] = phi;
                }
            }
        }
        
        currentControl = mergeNode;
        lastValue = resultPhi;
        return;
    }

    // 1. 编译左子树
    expr->left->accept(*this);
    IRNode* leftVal = lastValue;
    
    // 2. 编译右子树
    expr->right->accept(*this);
    IRNode* rightVal = lastValue;

    // 3. 映射操作码
    IROp op = IROp::Add;
    switch (expr->op.type) {
        case TokenType::PLUS: op = IROp::Add; break;
        case TokenType::MINUS: op = IROp::Sub; break;
        case TokenType::STAR: op = IROp::Mul; break;
        case TokenType::SLASH: op = IROp::Div; break;
        case TokenType::PERCENT: op = IROp::Mod; break;
        case TokenType::CARET: op = IROp::Pow; break;
        case TokenType::BACKSLASH: op = IROp::LeftDivide; break;
        case TokenType::EQUAL: op = IROp::Eq; break;
        case TokenType::BANG_EQUAL: op = IROp::Neq; break;
        case TokenType::LESS: op = IROp::Lt; break;
        case TokenType::LESS_EQUAL: op = IROp::Le; break;
        case TokenType::GREATER: op = IROp::Gt; break;
        case TokenType::GREATER_EQUAL: op = IROp::Ge; break;
        case TokenType::IS: op = IROp::Is; break;
        case TokenType::IN: op = IROp::In; break;
        case TokenType::BIT_AND: op = IROp::BitAnd; break;
        case TokenType::BIT_OR: op = IROp::BitOr; break;
        case TokenType::BIT_XOR: op = IROp::BitXor; break;
        case TokenType::SHIFT_LEFT: op = IROp::Shl; break;
        case TokenType::SHIFT_RIGHT: op = IROp::Shr; break;
        default: error("Syntax Error: Unsupported binary operator.");
    }

    // 4. 创建运算节点并连接依赖边
    IRNode* node = graph->createValueNode(op);
    node->setControl(currentControl);
    node->addData(leftVal);
    node->addData(rightVal);
    lastValue = node;
}

void IRBuilder::visitVariable(Variable* expr) {
    graph->currentLine = expr->name.line;
    auto it = exprSymbols->find(expr);
    ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
    lastValue = readVariable(expr->name.lexeme, sym);
}

void IRBuilder::visitAssign(Assign* expr) {
    graph->currentLine = expr->name.line;
    auto it = exprSymbols->find(expr);
    ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};

    bool isFuncDef = dynamic_cast<LambdaExpr*>(expr->value.get()) != nullptr;
    bool hidden = false;
    IRNode* hiddenNode = nullptr;
    int hiddenDepth = -1;

    if (expr->name.lexeme != "_" && !isFuncDef && (expr->isLocal || expr->isState || expr->isConst)) {
        for (int i = static_cast<int>(envStack.size()) - 1; i >= 0; --i) {
            auto envIt = envStack[i].find(expr->name.lexeme);
            if (envIt != envStack[i].end()) {
                hiddenNode = envIt->second;
                hiddenDepth = i;
                envStack[i].erase(envIt);
                hidden = true;
                break;
            }
        }
    }

    if (expr->isState) {
        IRNode* getVal = readVariable(expr->name.lexeme, sym);
        IRNode* isUninit = graph->createValueNode(IROp::IsUninit);
        isUninit->addData(getVal);
        isUninit->setControl(currentControl);

        IRNode* ifNode = graph->createNode(IROp::If);
        ifNode->addData(isUninit);
        ifNode->setControl(currentControl);

        IRNode* ifTrue = graph->createNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);

        IRNode* ifFalse = graph->createNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);

        currentControl = ifTrue;
        
        expr->value->accept(*this);
        IRNode* valNode = lastValue;
        
        if (hidden) envStack[hiddenDepth][expr->name.lexeme] = hiddenNode;
        
        writeVariable(expr->name.lexeme, valNode, sym, false);
        IRNode* trueCtrl = currentControl;

        IRNode* merge = graph->createNode(IROp::Merge);
        merge->addData(trueCtrl);
        merge->addData(ifFalse);
        currentControl = merge;

        lastValue = valNode;
        return;
    }

    expr->value->accept(*this);
    IRNode* valNode = lastValue;
    
    if (valNode->op == IROp::Class || valNode->op == IROp::BuildNamespace) {
        if (valNode->name.empty() && expr->name.lexeme != "_") valNode->name = expr->name.lexeme;
    }
    
    if (hidden) envStack[hiddenDepth][expr->name.lexeme] = hiddenNode;
    
    writeVariable(expr->name.lexeme, valNode, sym, expr->isLocal, expr->isConst);
    lastValue = valNode;
}

void IRBuilder::hoistBlock(Block* block) {
    auto hoistVar = [&](const std::string& name, bool isExplicitLocal) {
        if (name == "_") return;
        if (isExplicitLocal) {
            if (!envStack.back().count(name)) {
                IRNode* uninit = graph->createConstant(Value::uninit());
                uninit->setControl(currentControl);
                envStack.back()[name] = uninit;
            }
        } else {
            bool found = false;
            for (int i = static_cast<int>(envStack.size()) - 1; i >= 0; --i) {
                if (envStack[i].count(name)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                IRNode* uninit = graph->createConstant(Value::uninit());
                uninit->setControl(currentControl);
                if (namespaceScopeDepth != -1) envStack[namespaceScopeDepth][name] = uninit;
                else envStack[0][name] = uninit;
            }
        }
    };

    for (auto& stmt : block->statements) {
        if (auto* assign = dynamic_cast<Assign*>(stmt.get())) {
            if (!assign->isRef && !assign->isState) {
                auto it = exprSymbols->find(assign);
                ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
                if (sym.scope != VarScope::Global) {
                    hoistVar(assign->name.lexeme, assign->isLocal);
                }
            }
        } else if (auto* locDecl = dynamic_cast<LocalDecl*>(stmt.get())) {
            hoistVar(locDecl->name.lexeme, true);
        } else if (auto* constDecl = dynamic_cast<ConstDecl*>(stmt.get())) {
            hoistVar(constDecl->name.lexeme, true);
        } else if (auto* destAssign = dynamic_cast<DestructAssign*>(stmt.get())) {
            if (!destAssign->isRef && !destAssign->isState) {
                std::vector<std::tuple<std::string, ScopeModifier, bool, Pattern*>> boundVars;
                collectPatternVars(destAssign->pattern.get(), boundVars);
                for (const auto& varTuple : boundVars) {
                    const std::string& name = std::get<0>(varTuple);
                    ScopeModifier mod = std::get<1>(varTuple);
                    Pattern* p = std::get<3>(varTuple);
                    if (mod != ScopeModifier::Ref && mod != ScopeModifier::State) {
                        auto it = patternSymbols->find(p);
                        ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
                        if (sym.scope != VarScope::Global) {
                            hoistVar(name, mod == ScopeModifier::Local || destAssign->isLocal);
                        }
                    }
                }
            }
        }
    }
}

void IRBuilder::visitBlock(Block* expr) {
    pushScope(); // 进入新作用域
    hoistBlock(expr);
    if (expr->statements.empty()) {
        lastValue = graph->createConstant(Value::none());
        lastValue->setControl(currentControl);
    } else {
        for (size_t i = 0; i < expr->statements.size(); ++i) {
            expr->statements[i]->accept(*this);
            
            bool isTerminal = dynamic_cast<ReturnExpr*>(expr->statements[i].get()) ||
                              dynamic_cast<BreakExpr*>(expr->statements[i].get()) ||
                              dynamic_cast<ContinueExpr*>(expr->statements[i].get()) ||
                              dynamic_cast<ThrowExpr*>(expr->statements[i].get());
            if (i < expr->statements.size() - 1 && isTerminal) {
                break;
            }
        }
    }
    popScope(); // 离开作用域
}

void IRBuilder::visitGroupingExpr(GroupingExpr* expr) {
    expr->expression->accept(*this);
}

void IRBuilder::visitUnary(Unary* expr) {
    graph->currentLine = expr->op.line;
    expr->right->accept(*this);
    IRNode* rightVal = lastValue;
    
    IROp op = IROp::Neg;
    switch (expr->op.type) {
        case TokenType::MINUS: op = IROp::Neg; break;
        case TokenType::BANG:  op = IROp::Not; break;
        case TokenType::TILDE: op = IROp::BitNot; break;
        case TokenType::PLUS:  lastValue = rightVal; return; // +x 等价于 x
        default: error("Syntax Error: Unsupported unary operator.");
    }
    
    IRNode* node = graph->createValueNode(op);
    node->setControl(currentControl);
    node->addData(rightVal);
    lastValue = node;
}

void IRBuilder::visitCall(Call* expr) {
    graph->currentLine = expr->callee.line;
    std::string calleeName = expr->callee.lexeme;
    auto it = exprSymbols->find(expr);
    ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
    IRNode* calleeNode = readVariable(calleeName, sym);
    
    std::vector<IRNode*> argNodes;
    IRCallSignature sig;
    
    for (size_t i = 0; i < expr->arguments.size(); ++i) {
        auto& arg = expr->arguments[i];
        if (auto* var = dynamic_cast<Variable*>(arg.get())) {
            std::string name = var->name.lexeme;
            if (refParams.count(name)) {
                ResolvedSym rs; rs.scope = VarScope::RefParam;
                sig.refs.push_back({static_cast<uint8_t>(i), 4, name, -1, readVariable(name, rs)});
            } else {
                IRNode* localNode = getLocalNode(name);
                if (localNode) {
                    capturedLocals.insert(name);
                    capturedNodesToExtend.push_back(localNode);
                    sig.refs.push_back({static_cast<uint8_t>(i), 2, name, -1, localNode});
                } else {
                    int upvalIdx = -1;
                    if (currentFunction) {
                        for (int j = static_cast<int>(currentFunction->upvalues.size()) - 1; j >= 0; --j) {
                            if (currentFunction->upvalues[j].name == name) {
                                upvalIdx = j;
                                break;
                            }
                        }
                    }
                    if (upvalIdx == -1) {
                        auto varIt = exprSymbols->find(var);
                        ResolvedSym rs = varIt != exprSymbols->end() ? varIt->second : ResolvedSym{};
                        upvalIdx = resolveUpvalue(name, rs.scope == VarScope::CapturedState);
                    }
                    
                    if (upvalIdx != -1) {
                        sig.refs.push_back({static_cast<uint8_t>(i), 3, name, upvalIdx, nullptr});
                    } else {
                        sig.refs.push_back({static_cast<uint8_t>(i), 1, name, -1, nullptr});
                    }
                }
            }
        }
        arg->accept(*this);
        argNodes.push_back(lastValue);
    }
    
    if (!sig.refs.empty()) {
        graph->callSignatures.push_back(sig);
        IRNode* passRefsNode = graph->createNode(IROp::PassRefs);
        passRefsNode->setControl(currentControl);
        passRefsNode->payload1 = static_cast<uint32_t>(graph->callSignatures.size() - 1);
        for (auto& ref : sig.refs) {
            if (ref.localNode) passRefsNode->addData(ref.localNode);
        }
        currentControl = passRefsNode;
    }
    
    IRNode* callNode = graph->createValueNode(IROp::Call);
    callNode->setControl(currentControl);
    callNode->addData(calleeNode);
    for (IRNode* arg : argNodes) {
        callNode->addData(arg);
    }
    callNode->payload1 = static_cast<uint32_t>(argNodes.size());
    
    currentControl = callNode;
    lastValue = callNode;
}

void IRBuilder::visitIfExpr(IfExpr* expr) {
    // 1. 编译条件
    expr->condition->accept(*this);
    IRNode* condVal = lastValue;

    // 2. 创建 If 控制节点
    IRNode* ifNode = graph->createNode(IROp::If);
    ifNode->setControl(currentControl);
    ifNode->addData(condVal);

    IRNode* ifTrue = graph->createNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);

    IRNode* ifFalse = graph->createNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);

    // 3. 保存当前环境快照
    auto baseEnv = envStack;

    // 4. 编译 True 分支
    currentControl = ifTrue;
    pushScope(); // ★ 自动创建块级作用域
    expr->thenBranch->accept(*this);
    IRNode* thenControl = currentControl;
    IRNode* thenVal = lastValue;
    auto thenEnv = envStack;
    popScope();

    // 5. 编译 False 分支
    envStack = baseEnv;
    currentControl = ifFalse;
    pushScope(); // ★ 自动创建块级作用域
    IRNode* elseVal = nullptr;
    if (expr->elseBranch) {
        expr->elseBranch->accept(*this);
        elseVal = lastValue;
    } else {
        elseVal = graph->createConstant(Value::none());
        elseVal->setControl(currentControl);
    }
    IRNode* elseControl = currentControl;
    auto elseEnv = envStack;
    popScope();
    envStack = baseEnv;

    // 6. 创建 Merge 节点汇合控制流
    IRNode* mergeNode = graph->createNode(IROp::Merge);
    mergeNode->addData(thenControl);
    mergeNode->addData(elseControl);
    currentControl = mergeNode;

    // 7. 合并环境 (生成 Phi 节点)
    for (size_t i = 0; i < baseEnv.size(); ++i) {
        std::unordered_set<std::string> modifiedVars;
        for (const auto& pair : thenEnv[i]) {
            if (baseEnv[i].count(pair.first) && baseEnv[i][pair.first] != pair.second) modifiedVars.insert(pair.first);
        }
        for (const auto& pair : elseEnv[i]) {
            if (baseEnv[i].count(pair.first) && baseEnv[i][pair.first] != pair.second) modifiedVars.insert(pair.first);
        }

        for (const auto& name : modifiedVars) {
            IRNode* tNode = thenEnv[i].count(name) ? thenEnv[i].at(name) : baseEnv[i].at(name);
            IRNode* eNode = elseEnv[i].count(name) ? elseEnv[i].at(name) : baseEnv[i].at(name);
            
            if (tNode != eNode) {
                IRNode* phi = graph->createValueNode(IROp::Phi);
                phi->setControl(mergeNode);
                phi->addData(tNode);
                phi->addData(eNode);
                phi->name = name;
                envStack[i][name] = phi; // 更新环境为 Phi 节点
            }
        }
    }

    // 8. 表达式结果的 Phi (如果 If 被用作表达式)
    if (thenVal != elseVal) {
        IRNode* resultPhi = graph->createValueNode(IROp::Phi);
        resultPhi->setControl(mergeNode);
        resultPhi->addData(thenVal);
        resultPhi->addData(elseVal);
        lastValue = resultPhi;
    } else {
        lastValue = thenVal;
    }
}

void IRBuilder::visitReturnExpr(ReturnExpr* expr) {
    IRNode* retVal = nullptr;
    if (expr->value) {
        expr->value->accept(*this);
        retVal = lastValue;
    } else {
        retVal = graph->createConstant(Value::none());
        retVal->setControl(currentControl);
    }
    
    graph->currentLine = expr->keyword.line;
    
    if (currentReturnTypeHint) {
        IRNode* assertNode = graph->createNode(IROp::AssertReturnType);
        assertNode->setControl(currentControl);
        assertNode->addData(retVal);
        currentControl = assertNode;
    }

    IRNode* retNode = graph->createNode(IROp::Return);
    retNode->setControl(currentControl);
    retNode->addData(retVal);
    
    recordExitNode(retNode);
    
    currentControl = retNode;
    lastValue = retVal;
}

void IRBuilder::visitMatrixNode(MatrixNode* expr) {
    int rows = static_cast<int>(expr->elements.size());
    if (rows == 0) {
        if (expr->forceList) {
            IRNode* node = graph->createValueNode(IROp::ListInit);
            node->setControl(currentControl);
            currentControl = node;
            lastValue = node;
        } else {
            lastValue = graph->createConstant(Value(RealMatrix(0, 0)));
            lastValue->setControl(currentControl);
        }
        return;
    }

    if (expr->forceList) {
        if (rows == 1) {
            std::vector<IRNode*> elements;
            for (auto& e : expr->elements[0]) {
                e->accept(*this);
                elements.push_back(lastValue);
            }
            IRNode* node = graph->createValueNode(IROp::BuildList);
            node->setControl(currentControl);
            for (auto* e : elements) node->addData(e);
            node->payload1 = static_cast<uint32_t>(elements.size());
            currentControl = node;
            lastValue = node;
        } else {
            std::vector<IRNode*> rowNodes;
            for (auto& row : expr->elements) {
                std::vector<IRNode*> elements;
                for (auto& e : row) {
                    e->accept(*this);
                    elements.push_back(lastValue);
                }
                IRNode* rowNode = graph->createValueNode(IROp::BuildList);
                rowNode->setControl(currentControl);
                for (auto* e : elements) rowNode->addData(e);
                rowNode->payload1 = static_cast<uint32_t>(elements.size());
                currentControl = rowNode;
                rowNodes.push_back(rowNode);
            }
            IRNode* node = graph->createValueNode(IROp::BuildList);
            node->setControl(currentControl);
            for (auto* r : rowNodes) node->addData(r);
            node->payload1 = static_cast<uint32_t>(rowNodes.size());
            currentControl = node;
            lastValue = node;
        }
        return;
    }

    std::vector<IRNode*> elements;
    size_t expectedCols = expr->elements.empty() ? 0 : expr->elements[0].size();
    for (auto& row : expr->elements) {
        if (row.size() != expectedCols) {
            error("Syntax Error: Matrix rows must have the same number of columns.");
        }
        for (auto& e : row) {
            e->accept(*this);
            elements.push_back(lastValue);
        }
    }
    IRNode* node = graph->createValueNode(IROp::BuildMatrix);
    node->setControl(currentControl);
    for (auto* e : elements) node->addData(e);
    node->payload1 = static_cast<uint32_t>(expr->elements.size());
    node->payload2 = static_cast<uint32_t>(expectedCols);
    currentControl = node;
    lastValue = node;
}

void IRBuilder::visitWhileExpr(WhileExpr* expr) {
    pushScope(); // ★ 自动创建块级作用域
    
    IRNode* loopNode = graph->createNode(IROp::Loop);
    loopNode->addData(currentControl);
    
    std::vector<std::unordered_map<std::string, IRNode*>> loopPhisStack(envStack.size());
    for (size_t i = 0; i < envStack.size(); ++i) {
        for (const auto& pair : envStack[i]) {
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(loopNode);
            phi->addData(pair.second);
            phi->name = pair.first;
            loopPhisStack[i][pair.first] = phi;
        }
    }
    envStack = loopPhisStack;
    currentControl = loopNode;
    
    IRNode* breakMerge = graph->createNode(IROp::Merge);
    IRNode* continueMerge = graph->createNode(IROp::Merge);
    loopStack.push_back({loopNode, loopPhisStack, breakMerge, {}, continueMerge, {}, activeTryCount});
    
    expr->condition->accept(*this);
    IRNode* condVal = lastValue;
    
    IRNode* ifNode = graph->createNode(IROp::If);
    ifNode->setControl(currentControl);
    ifNode->addData(condVal);
    
    IRNode* ifTrue = graph->createNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    
    IRNode* ifFalse = graph->createNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    
    auto envBeforeBody = envStack;
    
    currentControl = ifTrue;
    expr->body->accept(*this);
    
    continueMerge->addData(currentControl);
    loopStack.back().continueEnvs.push_back(envStack);
    
    currentControl = continueMerge;
    
    auto& continueEnvs = loopStack.back().continueEnvs;
    for (size_t i = 0; i < envStack.size(); ++i) {
        for (const auto& pair : envStack[i]) {
            const std::string& name = pair.first;
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(continueMerge);
            for (auto& env : continueEnvs) {
                phi->addData(env[i].count(name) ? env[i].at(name) : graph->createConstant(Value::none()));
            }
            phi->name = name;
            envStack[i][name] = phi;
        }
    }
    
    loopNode->addData(currentControl);
    for (size_t i = 0; i < loopPhisStack.size(); ++i) {
        for (auto& pair : loopPhisStack[i]) {
            ResolvedSym rs; rs.scope = VarScope::Local;
            pair.second->addData(readVariable(pair.first, rs));
        }
    }
    
    breakMerge->addData(ifFalse);
    loopStack.back().breakEnvs.push_back(envBeforeBody);
    
    currentControl = breakMerge;
    
    auto& breakEnvs = loopStack.back().breakEnvs;
    std::vector<std::unordered_map<std::string, IRNode*>> exitEnvStack(envStack.size());
    for (size_t i = 0; i < envStack.size(); ++i) {
        for (const auto& pair : envStack[i]) {
            const std::string& name = pair.first;
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(breakMerge);
            for (auto& env : breakEnvs) {
                phi->addData(env[i].count(name) ? env[i].at(name) : graph->createConstant(Value::none()));
            }
            phi->name = name;
            exitEnvStack[i][name] = phi;
        }
    }
    
    loopStack.pop_back();
    
    envStack = exitEnvStack;
    popScope(); // 离开块级作用域
    
    lastValue = graph->createConstant(Value::none());
    lastValue->setControl(currentControl);
}

void IRBuilder::visitForExpr(ForExpr* expr) {
    pushScope(); // ★ 自动创建块级作用域
    expr->initializer->accept(*this);
    
    IRNode* loopNode = graph->createNode(IROp::Loop);
    loopNode->addData(currentControl);
    
    std::vector<std::unordered_map<std::string, IRNode*>> loopPhisStack(envStack.size());
    for (size_t i = 0; i < envStack.size(); ++i) {
        for (const auto& pair : envStack[i]) {
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(loopNode);
            phi->addData(pair.second);
            phi->name = pair.first;
            loopPhisStack[i][pair.first] = phi;
        }
    }
    envStack = loopPhisStack;
    currentControl = loopNode;
    
    IRNode* breakMerge = graph->createNode(IROp::Merge);
    IRNode* continueMerge = graph->createNode(IROp::Merge);
    loopStack.push_back({loopNode, loopPhisStack, breakMerge, {}, continueMerge, {}, activeTryCount});
    
    expr->condition->accept(*this);
    IRNode* condVal = lastValue;
    
    IRNode* ifNode = graph->createNode(IROp::If);
    ifNode->setControl(currentControl);
    ifNode->addData(condVal);
    
    IRNode* ifTrue = graph->createNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    
    IRNode* ifFalse = graph->createNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    
    auto envBeforeBody = envStack;
    
    currentControl = ifTrue;
    expr->body->accept(*this);
    
    continueMerge->addData(currentControl);
    loopStack.back().continueEnvs.push_back(envStack);
    
    currentControl = continueMerge;
    
    auto& continueEnvs = loopStack.back().continueEnvs;
    for (size_t i = 0; i < envStack.size(); ++i) {
        for (const auto& pair : envStack[i]) {
            const std::string& name = pair.first;
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(continueMerge);
            for (auto& env : continueEnvs) {
                phi->addData(env[i].count(name) ? env[i].at(name) : graph->createConstant(Value::none()));
            }
            phi->name = name;
            envStack[i][name] = phi;
        }
    }
    
    expr->update->accept(*this);
    
    loopNode->addData(currentControl);
    for (size_t i = 0; i < loopPhisStack.size(); ++i) {
        for (auto& pair : loopPhisStack[i]) {
            ResolvedSym rs; rs.scope = VarScope::Local;
            pair.second->addData(readVariable(pair.first, rs));
        }
    }
    
    breakMerge->addData(ifFalse);
    loopStack.back().breakEnvs.push_back(envBeforeBody);
    
    currentControl = breakMerge;
    
    auto& breakEnvs = loopStack.back().breakEnvs;
    std::vector<std::unordered_map<std::string, IRNode*>> exitEnvStack(envStack.size());
    for (size_t i = 0; i < envStack.size(); ++i) {
        for (const auto& pair : envStack[i]) {
            const std::string& name = pair.first;
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(breakMerge);
            for (auto& env : breakEnvs) {
                phi->addData(env[i].count(name) ? env[i].at(name) : graph->createConstant(Value::none()));
            }
            phi->name = name;
            exitEnvStack[i][name] = phi;
        }
    }
    
    loopStack.pop_back();
    
    envStack = exitEnvStack;
    popScope(); // 离开块级作用域
    
    lastValue = graph->createConstant(Value::none());
    lastValue->setControl(currentControl);
}

void IRBuilder::visitBreakExpr(BreakExpr* expr) {
    graph->currentLine = expr->keyword.line;
    if (loopStack.empty()) error("Syntax Error: 'break' outside loop.");
    auto& loop = loopStack.back();
    
    int loopDepth = static_cast<int>(loop.loopPhisStack.size());
    int triesToPop = activeTryCount - loop.tryDepthAtLoopStart;
    for (int i = 0; i < triesToPop; ++i) {
        IRNode* tryEnd = graph->createNode(IROp::TryEnd);
        tryEnd->setControl(currentControl);
        currentControl = tryEnd;
    }

    int defersToRun = 0;
    for (size_t i = loopDepth - 1; i < deferCounts.size(); ++i) {
        defersToRun += deferCounts[i];
    }
    if (defersToRun > 0) {
        IRNode* runNode = graph->createNode(IROp::RunDefers);
        runNode->payload1 = defersToRun;
        runNode->setControl(currentControl);
        currentControl = runNode;
    }
    
    loop.breakMerge->addData(currentControl);
    loop.breakEnvs.push_back(envStack);
    
    currentControl = graph->createNode(IROp::Merge); // Dead code
}

void IRBuilder::visitContinueExpr(ContinueExpr* expr) {
    graph->currentLine = expr->keyword.line;
    if (loopStack.empty()) error("Syntax Error: 'continue' outside loop.");
    auto& loop = loopStack.back();
    
    int loopDepth = static_cast<int>(loop.loopPhisStack.size());
    int triesToPop = activeTryCount - loop.tryDepthAtLoopStart;
    for (int i = 0; i < triesToPop; ++i) {
        IRNode* tryEnd = graph->createNode(IROp::TryEnd);
        tryEnd->setControl(currentControl);
        currentControl = tryEnd;
    }

    int defersToRun = 0;
    for (size_t i = loopDepth; i < deferCounts.size(); ++i) {
        defersToRun += deferCounts[i];
    }
    if (defersToRun > 0) {
        IRNode* runNode = graph->createNode(IROp::RunDefers);
        runNode->payload1 = defersToRun;
        runNode->setControl(currentControl);
        currentControl = runNode;
    }
    
    if (loop.continueMerge) {
        loop.continueMerge->addData(currentControl);
        loop.continueEnvs.push_back(envStack);
    } else {
        loop.loopNode->addData(currentControl);
        for (size_t i = 0; i < loop.loopPhisStack.size(); ++i) {
            for (auto& pair : loop.loopPhisStack[i]) {
                ResolvedSym rs; rs.scope = VarScope::Local;
                pair.second->addData(readVariable(pair.first, rs));
            }
        }
    }
    
    currentControl = graph->createNode(IROp::Merge); // Dead code
}
    
void IRBuilder::visitIndexAccess(IndexAccess* expr) {
    bool hasSlice = false;
    for (auto& idx : expr->indices) {
        if (dynamic_cast<SliceExpr*>(idx.get())) {
            hasSlice = true;
            break;
        }
    }

    expr->object->accept(*this);
    IRNode* objNode = lastValue;

    if (hasSlice) {
        std::vector<IRNode*> sliceArgs;
        for (auto& idx : expr->indices) {
            if (auto* slice = dynamic_cast<SliceExpr*>(idx.get())) {
                if (slice->start) { slice->start->accept(*this); sliceArgs.push_back(lastValue); }
                else { IRNode* n = graph->createConstant(Value::none()); n->setControl(currentControl); sliceArgs.push_back(n); }
                
                if (slice->end) { slice->end->accept(*this); sliceArgs.push_back(lastValue); }
                else { IRNode* n = graph->createConstant(Value::none()); n->setControl(currentControl); sliceArgs.push_back(n); }
                
                if (slice->step) { slice->step->accept(*this); sliceArgs.push_back(lastValue); }
                else { IRNode* n = graph->createConstant(Value::none()); n->setControl(currentControl); sliceArgs.push_back(n); }
            } else {
                idx->accept(*this); sliceArgs.push_back(lastValue);
                IRNode* n1 = graph->createConstant(Value::none()); n1->setControl(currentControl); sliceArgs.push_back(n1);
                IRNode* n2 = graph->createConstant(Value(0.0)); n2->setControl(currentControl); sliceArgs.push_back(n2);
            }
        }
        IRNode* node = graph->createValueNode(IROp::SliceGet);
        node->setControl(currentControl);
        node->addData(objNode);
        for (auto* arg : sliceArgs) node->addData(arg);
        node->payload1 = static_cast<uint32_t>(expr->indices.size());
        currentControl = node;
        lastValue = node;
        return;
    }

    std::vector<IRNode*> indices;
    for (auto& idx : expr->indices) {
        idx->accept(*this);
        indices.push_back(lastValue);
    }
        
    IRNode* node = graph->createValueNode(IROp::IndexGet);
    node->setControl(currentControl);
    node->addData(objNode);
    for (auto* idx : indices) node->addData(idx);
    node->payload1 = static_cast<uint32_t>(indices.size());
    currentControl = node;
    lastValue = node;
}

void IRBuilder::visitIndexAssign(IndexAssign* expr) {
    IRNode* dotParentNode = nullptr;
    std::string dotPropName = "";

    if (expr->hasObjectExpr()) {
        if (auto* dot = dynamic_cast<DotAccess*>(expr->objectExpr.get())) {
            dot->object->accept(*this);
            dotParentNode = lastValue;
            dotPropName = dot->field.lexeme;
            
            IRNode* getProp = graph->createValueNode(IROp::GetProperty);
            getProp->setControl(currentControl);
            getProp->addData(dotParentNode);
            getProp->name = dotPropName;
            currentControl = getProp;
            lastValue = getProp;
        } else {
            expr->objectExpr->accept(*this);
        }
    } else {
        auto it = exprSymbols->find(expr);
        ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
        lastValue = readVariable(expr->name.lexeme, sym);
    }
    IRNode* rootObjNode = lastValue;

    bool hasSlice = false;
    if (expr->indexChain.size() == 1) {
        for (auto& idx : expr->indexChain[0]) {
            if (dynamic_cast<SliceExpr*>(idx.get())) {
                hasSlice = true;
                break;
            }
        }
    }

    expr->value->accept(*this);
    IRNode* valNode = lastValue;

    if (valNode->op == IROp::Class || valNode->op == IROp::BuildNamespace) {
        if (valNode->name.empty()) valNode->name = expr->name.lexeme;
    }

    if (hasSlice) {
        std::vector<IRNode*> sliceArgs;
        for (auto& idx : expr->indexChain[0]) {
            if (auto* slice = dynamic_cast<SliceExpr*>(idx.get())) {
                if (slice->start) { slice->start->accept(*this); sliceArgs.push_back(lastValue); }
                else { IRNode* n = graph->createConstant(Value::none()); n->setControl(currentControl); sliceArgs.push_back(n); }
                
                if (slice->end) { slice->end->accept(*this); sliceArgs.push_back(lastValue); }
                else { IRNode* n = graph->createConstant(Value::none()); n->setControl(currentControl); sliceArgs.push_back(n); }
                
                if (slice->step) { slice->step->accept(*this); sliceArgs.push_back(lastValue); }
                else { IRNode* n = graph->createConstant(Value::none()); n->setControl(currentControl); sliceArgs.push_back(n); }
            } else {
                idx->accept(*this); sliceArgs.push_back(lastValue);
                IRNode* n1 = graph->createConstant(Value::none()); n1->setControl(currentControl); sliceArgs.push_back(n1);
                IRNode* n2 = graph->createConstant(Value(0.0)); n2->setControl(currentControl); sliceArgs.push_back(n2);
            }
        }
        IRNode* node = graph->createValueNode(IROp::SliceSet);
        node->setControl(currentControl);
        node->addData(rootObjNode);
        for (auto* arg : sliceArgs) node->addData(arg);
        node->addData(valNode);
        node->payload1 = static_cast<uint32_t>(expr->indexChain[0].size());
        currentControl = node;
        
        if (!expr->hasObjectExpr()) {
            auto it = exprSymbols->find(expr);
            ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
            writeVariable(expr->name.lexeme, node, sym, false, false);
        } else if (dotParentNode) {
            IROp setOp = IROp::SetProperty;
            if (dynamic_cast<SelfExpr*>(static_cast<DotAccess*>(expr->objectExpr.get())->object.get()) && !classStack.empty()) {
                if (classStack.back().privateMembers.count(dotPropName)) {
                    setOp = IROp::SetPrivate;
                }
            }

            IRNode* setProp = graph->createValueNode(setOp);
            setProp->setControl(currentControl);
            setProp->addData(dotParentNode);
            setProp->addData(node);
            setProp->name = dotPropName;
            currentControl = setProp;
        }
        lastValue = valNode;
        return;
    }

    std::vector<std::vector<IRNode*>> indicesTmp(expr->indexChain.size());
    for (size_t i = 0; i < expr->indexChain.size(); ++i) {
        for (size_t j = 0; j < expr->indexChain[i].size(); ++j) {
            expr->indexChain[i][j]->accept(*this);
            indicesTmp[i].push_back(lastValue);
        }
    }

    int depth = static_cast<int>(expr->indexChain.size());
    if (depth == 1) {
        IRNode* node = graph->createValueNode(IROp::IndexSet);
        node->setControl(currentControl);
        node->addData(rootObjNode);
        for (auto* idx : indicesTmp[0]) node->addData(idx);
        node->addData(valNode);
        node->payload1 = static_cast<uint32_t>(indicesTmp[0].size());
        currentControl = node;
        
        if (!expr->hasObjectExpr()) {
            auto it = exprSymbols->find(expr);
            ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
            writeVariable(expr->name.lexeme, node, sym, false, false);
        } else if (dotParentNode) {
            IRNode* setProp = graph->createValueNode(IROp::SetProperty);
            setProp->setControl(currentControl);
            setProp->addData(dotParentNode);
            setProp->addData(node);
            setProp->name = dotPropName;
            currentControl = setProp;
        }
        lastValue = valNode;
    } else {
        std::vector<IRNode*> chainObjs;
        chainObjs.push_back(rootObjNode);
        
        for (int level = 0; level < depth - 1; ++level) {
            IRNode* getNode = graph->createValueNode(IROp::IndexGet);
            getNode->setControl(currentControl);
            getNode->addData(chainObjs.back());
            for (auto* idx : indicesTmp[level]) getNode->addData(idx);
            getNode->payload1 = static_cast<uint32_t>(indicesTmp[level].size());
            currentControl = getNode;
            chainObjs.push_back(getNode);
        }
        
        IRNode* setNode = graph->createValueNode(IROp::IndexSet);
        setNode->setControl(currentControl);
        setNode->addData(chainObjs.back());
        for (auto* idx : indicesTmp[depth - 1]) setNode->addData(idx);
        setNode->addData(valNode);
        setNode->payload1 = static_cast<uint32_t>(indicesTmp[depth - 1].size());
        currentControl = setNode;
        
        IRNode* finalNode = setNode;
        for (int level = depth - 2; level >= 0; --level) {
            IRNode* backSetNode = graph->createValueNode(IROp::IndexSet);
            backSetNode->setControl(currentControl);
            backSetNode->addData(chainObjs[level]);
            for (auto* idx : indicesTmp[level]) backSetNode->addData(idx);
            backSetNode->addData(finalNode);
            backSetNode->payload1 = static_cast<uint32_t>(indicesTmp[level].size());
            currentControl = backSetNode;
            finalNode = backSetNode;
        }
        
        if (!expr->hasObjectExpr()) {
            auto it = exprSymbols->find(expr);
            ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
            writeVariable(expr->name.lexeme, finalNode, sym, false, false);
        } else if (dotParentNode) {
            IROp setOp = IROp::SetProperty;
            if (dynamic_cast<SelfExpr*>(static_cast<DotAccess*>(expr->objectExpr.get())->object.get()) && !classStack.empty()) {
                if (classStack.back().privateMembers.count(dotPropName)) {
                    setOp = IROp::SetPrivate;
                }
            }

            IRNode* setProp = graph->createValueNode(setOp);
            setProp->setControl(currentControl);
            setProp->addData(dotParentNode);
            setProp->addData(finalNode);
            setProp->name = dotPropName;
            currentControl = setProp;
        }
        lastValue = valNode;
    }
}
    
void IRBuilder::visitLocalDecl(LocalDecl* expr) {
    graph->currentLine = expr->name.line;
    IRNode* uninitNode = graph->createConstant(Value::uninit());
    uninitNode->setControl(currentControl);
    declareVariable(expr->name.lexeme, uninitNode);
    if (expr->name.lexeme != "_") {
        currentLocalVars.insert(expr->name.lexeme);
        if (expr->isConst) currentConstVars.insert(expr->name.lexeme);
    }
    lastValue = uninitNode;
}

void IRBuilder::visitRefDecl(RefDecl* expr) {
    graph->currentLine = expr->name.line;
    if (expr->name.lexeme != "_") {
        if (currentFunction) {
            int upvalIdx = resolveUpvalue(expr->name.lexeme, false);
            if (upvalIdx == -1) {
                // If not found in parent, it's a global ref
                CompiledFunction::UpvalueInfo uv;
                uv.name = expr->name.lexeme;
                uv.isLocal = false;
                uv.index = 0;
                uv.isRef = true;
                uv.isGlobal = true;
                uv.isExplicitState = false;
                uv.isRefParam = false;
                currentFunction->upvalues.push_back(uv);
            } else {
                currentFunction->upvalues[upvalIdx].isRef = true;
            }
        }
        if (expr->isConst) currentConstVars.insert(expr->name.lexeme);
    }
    lastValue = graph->createConstant(Value::uninit());
    lastValue->setControl(currentControl);
}

void IRBuilder::visitStateDecl(StateDecl* expr) {
    graph->currentLine = expr->name.line;
    if (!currentFunction) error("Syntax Error: 'state' modifier cannot be used at the top level.");
    
    if (expr->name.lexeme != "_") {
        int upvalIdx = resolveUpvalue(expr->name.lexeme, false);
        if (upvalIdx == -1) {
            CompiledFunction::UpvalueInfo uv;
            uv.name = expr->name.lexeme;
            uv.isLocal = false;
            uv.index = 0;
            uv.isRef = false;
            uv.isGlobal = true;
            uv.isExplicitState = false;
            uv.isRefParam = false;
            uv.isCapturedState = true;
            currentFunction->upvalues.push_back(uv);
        } else {
            currentFunction->upvalues[upvalIdx].isCapturedState = true;
        }
        if (expr->isConst) currentConstVars.insert(expr->name.lexeme);
    }
    lastValue = graph->createConstant(Value::uninit());
    lastValue->setControl(currentControl);
}
    
void IRBuilder::visitConstDecl(ConstDecl* expr) {
    graph->currentLine = expr->name.line;
    IRNode* uninitNode = graph->createConstant(Value::uninit());
    uninitNode->setControl(currentControl);
    declareVariable(expr->name.lexeme, uninitNode);
    if (expr->name.lexeme != "_") {
        currentConstVars.insert(expr->name.lexeme);
    }
    lastValue = uninitNode;
}

void IRBuilder::visitDeleteExpr(DeleteExpr* expr) {
    if (!expr->names.empty()) graph->currentLine = expr->names[0].line;
    for (auto& tok : expr->names) {
        IRNode* delNode = graph->createNode(IROp::DeleteGlobal);
        delNode->setControl(currentControl);
        delNode->name = tok.lexeme;
        currentControl = delNode;
    }
    lastValue = graph->createConstant(Value::none());
    lastValue->setControl(currentControl);
}

void IRBuilder::visitCompoundAssign(CompoundAssign* expr) {
    IRNode* targetVal = nullptr;
    IRNode* objNode = nullptr;
    std::vector<IRNode*> indices;
    std::string propName;
    
    std::vector<IndexAccess*> chain;
    IRNode* dotParentNode = nullptr;
    std::string dotPropName = "";

    if (auto* var = dynamic_cast<Variable*>(expr->target.get())) {
        graph->currentLine = var->name.line;
        if (expr->isState) {
            if (!currentFunction) error("Syntax Error: 'state' modifier cannot be used at the top level.");
            int upvalIdx = resolveUpvalue(var->name.lexeme, false);
            if (upvalIdx == -1) {
                CompiledFunction::UpvalueInfo uv;
                uv.name = var->name.lexeme;
                uv.isLocal = false;
                uv.index = 0;
                uv.isRef = false;
                uv.isGlobal = true;
                uv.isExplicitState = false;
                uv.isRefParam = false;
                uv.isCapturedState = true;
                currentFunction->upvalues.push_back(uv);
            } else {
                currentFunction->upvalues[upvalIdx].isCapturedState = true;
            }
        } else if (expr->isRef) {
            if (currentFunction) {
                int upvalIdx = resolveUpvalue(var->name.lexeme, false);
                if (upvalIdx == -1) {
                    CompiledFunction::UpvalueInfo uv;
                    uv.name = var->name.lexeme;
                    uv.isLocal = false;
                    uv.index = 0;
                    uv.isRef = true;
                    uv.isGlobal = true;
                    uv.isExplicitState = false;
                    uv.isRefParam = false;
                    currentFunction->upvalues.push_back(uv);
                } else {
                    currentFunction->upvalues[upvalIdx].isRef = true;
                }
            }
        }
        auto it = exprSymbols->find(var);
        ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
        targetVal = readVariable(var->name.lexeme, sym);
    } else if (auto* dot = dynamic_cast<DotAccess*>(expr->target.get())) {
        graph->currentLine = dot->field.line;
        dot->object->accept(*this);
        objNode = lastValue;
        propName = dot->field.lexeme;
        
        IROp getOp = IROp::GetProperty;
        if (dynamic_cast<SelfExpr*>(dot->object.get()) && !classStack.empty()) {
            if (classStack.back().privateMembers.count(propName)) {
                getOp = IROp::GetPrivate;
            }
        }

        targetVal = graph->createValueNode(getOp);
        targetVal->setControl(currentControl);
        targetVal->addData(objNode);
        targetVal->name = propName;
        currentControl = targetVal;
    } else if (auto* idx = dynamic_cast<IndexAccess*>(expr->target.get())) {
        IndexAccess* curr = idx;
        while (curr) {
            chain.push_back(curr);
            if (auto* next = dynamic_cast<IndexAccess*>(curr->object.get())) {
                curr = next;
            } else {
                break;
            }
        }
        std::reverse(chain.begin(), chain.end());
        
        if (auto* chainDot = dynamic_cast<DotAccess*>(chain[0]->object.get())) {
            chainDot->object->accept(*this);
            dotParentNode = lastValue;
            dotPropName = chainDot->field.lexeme;
            
            IROp getOp = IROp::GetProperty;
            if (dynamic_cast<SelfExpr*>(chainDot->object.get()) && !classStack.empty()) {
                if (classStack.back().privateMembers.count(dotPropName)) {
                    getOp = IROp::GetPrivate;
                }
            }

            IRNode* getProp = graph->createValueNode(getOp);
            getProp->setControl(currentControl);
            getProp->addData(dotParentNode);
            getProp->name = dotPropName;
            currentControl = getProp;
            objNode = getProp;
        } else {
            chain[0]->object->accept(*this);
            objNode = lastValue;
        }
        
        IRNode* currObj = objNode;
        for (size_t i = 0; i < chain.size(); ++i) {
            std::vector<IRNode*> levelIndices;
            for (auto& idxExpr : chain[i]->indices) {
                idxExpr->accept(*this);
                levelIndices.push_back(lastValue);
            }
            if (i == chain.size() - 1) {
                indices = levelIndices;
            }
            
            IRNode* getNode = graph->createValueNode(IROp::IndexGet);
            getNode->setControl(currentControl);
            getNode->addData(currObj);
            for (auto* idxNode : levelIndices) getNode->addData(idxNode);
            getNode->payload1 = static_cast<uint32_t>(levelIndices.size());
            currentControl = getNode;
            currObj = getNode;
        }
        targetVal = currObj;
    } else {
        error("Syntax Error: Unsupported compound assignment target.");
    }

    expr->value->accept(*this);
    IRNode* rightVal = lastValue;

    IROp op = IROp::Add;
    switch (expr->op) {
        case TokenType::PLUS:
        case TokenType::PLUS_ASSIGN: op = IROp::Add; break;
        case TokenType::MINUS:
        case TokenType::MINUS_ASSIGN: op = IROp::Sub; break;
        case TokenType::STAR:
        case TokenType::STAR_ASSIGN: op = IROp::Mul; break;
        case TokenType::SLASH:
        case TokenType::SLASH_ASSIGN: op = IROp::Div; break;
        case TokenType::PERCENT:
        case TokenType::PERCENT_ASSIGN: op = IROp::Mod; break;
        case TokenType::CARET:
        case TokenType::CARET_ASSIGN: op = IROp::Pow; break;
        case TokenType::BACKSLASH:
        case TokenType::BACKSLASH_ASSIGN: op = IROp::LeftDivide; break;
        case TokenType::BIT_AND:
        case TokenType::BIT_AND_ASSIGN: op = IROp::BitAnd; break;
        case TokenType::BIT_OR:
        case TokenType::BIT_OR_ASSIGN: op = IROp::BitOr; break;
        case TokenType::BIT_XOR:
        case TokenType::BIT_XOR_ASSIGN: op = IROp::BitXor; break;
        case TokenType::SHIFT_LEFT:
        case TokenType::SHIFT_LEFT_ASSIGN: op = IROp::Shl; break;
        case TokenType::SHIFT_RIGHT:
        case TokenType::SHIFT_RIGHT_ASSIGN: op = IROp::Shr; break;
        default: error("Syntax Error: Unsupported compound operator.");
    }

    IRNode* opNode = graph->createValueNode(op);
    opNode->setControl(currentControl);
    opNode->addData(targetVal);
    opNode->addData(rightVal);

    if (auto* var = dynamic_cast<Variable*>(expr->target.get())) {
        auto it = exprSymbols->find(var);
        ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
        writeVariable(var->name.lexeme, opNode, sym, expr->isLocal, false);
    } else if (dynamic_cast<DotAccess*>(expr->target.get())) {
        IROp setOp = IROp::SetProperty;
        if (dynamic_cast<SelfExpr*>(static_cast<DotAccess*>(expr->target.get())->object.get()) && !classStack.empty()) {
            if (classStack.back().privateMembers.count(propName)) {
                setOp = IROp::SetPrivate;
            }
        }

        IRNode* setProp = graph->createValueNode(setOp);
        setProp->setControl(currentControl);
        setProp->addData(objNode);
        setProp->addData(opNode);
        setProp->name = propName;
        currentControl = setProp;
    } else if (dynamic_cast<IndexAccess*>(expr->target.get())) {
        int depth = static_cast<int>(chain.size());
        IRNode* finalNode = nullptr;
        if (depth == 1) {
            IRNode* setIdx = graph->createValueNode(IROp::IndexSet);
            setIdx->setControl(currentControl);
            setIdx->addData(objNode);
            for (auto* i : indices) setIdx->addData(i);
            setIdx->addData(opNode);
            setIdx->payload1 = static_cast<uint32_t>(indices.size());
            currentControl = setIdx;
            finalNode = setIdx;
        } else {
            std::vector<IRNode*> chainObjs;
            chainObjs.push_back(objNode);
            
            std::vector<std::vector<IRNode*>> allIndices;
            for (size_t i = 0; i < chain.size(); ++i) {
                std::vector<IRNode*> levelIndices;
                for (auto& idxExpr : chain[i]->indices) {
                    idxExpr->accept(*this);
                    levelIndices.push_back(lastValue);
                }
                allIndices.push_back(levelIndices);
                
                if (i < chain.size() - 1) {
                    IRNode* getNode = graph->createValueNode(IROp::IndexGet);
                    getNode->setControl(currentControl);
                    getNode->addData(chainObjs.back());
                    for (auto* idxNode : levelIndices) getNode->addData(idxNode);
                    getNode->payload1 = static_cast<uint32_t>(levelIndices.size());
                    currentControl = getNode;
                    chainObjs.push_back(getNode);
                }
            }
            
            IRNode* setNode = graph->createValueNode(IROp::IndexSet);
            setNode->setControl(currentControl);
            setNode->addData(chainObjs.back());
            for (auto* idx : allIndices.back()) setNode->addData(idx);
            setNode->addData(opNode);
            setNode->payload1 = static_cast<uint32_t>(allIndices.back().size());
            currentControl = setNode;
            finalNode = setNode;
            
            for (int level = depth - 2; level >= 0; --level) {
                IRNode* backSetNode = graph->createValueNode(IROp::IndexSet);
                backSetNode->setControl(currentControl);
                backSetNode->addData(chainObjs[level]);
                for (auto* idx : allIndices[level]) backSetNode->addData(idx);
                backSetNode->addData(finalNode);
                backSetNode->payload1 = static_cast<uint32_t>(allIndices[level].size());
                currentControl = backSetNode;
                finalNode = backSetNode;
            }
        }
        
        if (auto* rootVar = dynamic_cast<Variable*>(chain[0]->object.get())) {
            auto it = exprSymbols->find(rootVar);
            ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
            writeVariable(rootVar->name.lexeme, finalNode, sym, expr->isLocal, false);
        } else if (dotParentNode) {
            IRNode* setProp = graph->createValueNode(IROp::SetProperty);
            setProp->setControl(currentControl);
            setProp->addData(dotParentNode);
            setProp->addData(finalNode);
            setProp->name = dotPropName;
            currentControl = setProp;
        }
    }

    lastValue = opNode;
}

void IRBuilder::buildFunctionParams(const std::vector<Token>& params, const std::vector<std::shared_ptr<Expr>>& defaultExprs, bool hasRestParam, const std::vector<bool>& paramIsRef, const std::vector<bool>& paramIsConst, const std::vector<std::shared_ptr<Expr>>& typeHints) {
    int requiredArgs = 0;
    bool seenDefault = false;
    for (size_t i = 0; i < params.size(); ++i) {
        if (hasRestParam && i == params.size() - 1) continue;
        if (defaultExprs[i]) {
            seenDefault = true;
        } else if (seenDefault) {
            error(params[i].line, "Syntax Error: non-default argument follows default argument.");
        }
        if (!defaultExprs[i]) requiredArgs++;
    }
    currentFunction->arity = requiredArgs;
    currentFunction->maxArity = static_cast<int>(params.size());
    currentFunction->hasRestParam = hasRestParam;
    currentFunction->paramIsRef = paramIsRef;
    currentFunction->paramIsConst = paramIsConst;
    int refCount = 0;
    for (bool isRef : paramIsRef) if (isRef) refCount++;
    currentFunction->refCount = refCount;
    
    int refIdx = 0;
    for (size_t i = 0; i < params.size(); ++i) {
        IRNode* paramNode = nullptr;
        if (i < paramIsRef.size() && paramIsRef[i]) {
            if (params[i].lexeme != "_") refParams[params[i].lexeme] = refIdx;
            refIdx++;
            if (params[i].lexeme != "_") {
                ResolvedSym rs; rs.scope = VarScope::RefParam;
                paramNode = readVariable(params[i].lexeme, rs);
            } else {
                paramNode = graph->createConstant(Value::none());
                paramNode->setControl(currentControl);
            }
        } else {
            paramNode = graph->createValueNode(IROp::Parameter);
            paramNode->payload1 = static_cast<uint32_t>(i);
            declareVariable(params[i].lexeme, paramNode);
        }
        if (params[i].lexeme != "_" && i < paramIsConst.size() && paramIsConst[i]) {
            currentConstVars.insert(params[i].lexeme);
        }
        
        if (defaultExprs[i]) {
            IRNode* isUninit = graph->createValueNode(IROp::IsUninit);
            isUninit->setControl(currentControl);
            isUninit->addData(paramNode);
            
            IRNode* ifNode = graph->createNode(IROp::If);
            ifNode->setControl(currentControl);
            ifNode->addData(isUninit);
            
            IRNode* ifTrue = graph->createNode(IROp::IfTrue);
            ifTrue->setControl(ifNode);
            
            IRNode* ifFalse = graph->createNode(IROp::IfFalse);
            ifFalse->setControl(ifNode);
            
            auto baseEnv = envStack;
            
            currentControl = ifTrue;
            envStack.emplace_back();
            defaultExprs[i]->accept(*this);
            IRNode* defVal = lastValue;
            IRNode* trueCtrl = currentControl;
            auto trueEnv = envStack;
            envStack.pop_back();
            
            envStack = baseEnv;
            currentControl = ifFalse;
            envStack.emplace_back();
            IRNode* falseCtrl = currentControl;
            auto falseEnv = envStack;
            envStack.pop_back();
            
            envStack = baseEnv;
            
            IRNode* merge = graph->createNode(IROp::Merge);
            merge->addData(trueCtrl);
            merge->addData(falseCtrl);
            
            for (size_t envIdx = 0; envIdx < baseEnv.size(); ++envIdx) {
                std::unordered_set<std::string> modifiedVars;
                for (const auto& pair : trueEnv[envIdx]) {
                    if (baseEnv[envIdx].count(pair.first) && baseEnv[envIdx].at(pair.first) != pair.second) modifiedVars.insert(pair.first);
                }
                for (const auto& pair : falseEnv[envIdx]) {
                    if (baseEnv[envIdx].count(pair.first) && baseEnv[envIdx].at(pair.first) != pair.second) modifiedVars.insert(pair.first);
                }
                for (const auto& name : modifiedVars) {
                    if (capturedLocals.count(name)) continue;
                    IRNode* tNode = trueEnv[envIdx].count(name) ? trueEnv[envIdx].at(name) : baseEnv[envIdx].at(name);
                    IRNode* fNode = falseEnv[envIdx].count(name) ? falseEnv[envIdx].at(name) : baseEnv[envIdx].at(name);
                    if (tNode != fNode) {
                        IRNode* phi = graph->createValueNode(IROp::Phi);
                        phi->setControl(merge);
                        phi->addData(tNode);
                        phi->addData(fNode);
                        phi->name = name;
                        envStack[envIdx][name] = phi;
                    }
                }
            }
            
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(merge);
            phi->addData(defVal);
            phi->addData(paramNode);
            phi->name = params[i].lexeme;
            
            currentControl = merge;
            ResolvedSym sym;
            sym.scope = paramIsRef[i] ? VarScope::RefParam : VarScope::Local;
            sym.isConst = paramIsConst[i];
            writeVariable(params[i].lexeme, phi, sym, true, paramIsConst[i]);
        }
        
        if (i < typeHints.size() && typeHints[i]) {
            IRNode* assertNode = graph->createNode(IROp::AssertParamType);
            assertNode->setControl(currentControl);
            ResolvedSym rs; rs.scope = paramIsRef[i] ? VarScope::RefParam : VarScope::Local;
            assertNode->addData(readVariable(params[i].lexeme, rs));
            
            uint32_t paramNameIdx = currentFunction->chunk.addConstant(Value(params[i].lexeme));
            
            assertNode->payload1 = static_cast<uint32_t>(i);
            assertNode->payload2 = paramNameIdx;
            
            currentControl = assertNode;
        }
    }
}

void IRBuilder::visitLambdaExpr(LambdaExpr* expr) {
    if (compiledFunctions) {
        std::vector<IRNode*> paramTypeNodes;
        for (auto& pt : expr->paramTypes) {
            if (pt) {
                pt->accept(*this);
                paramTypeNodes.push_back(lastValue);
            } else {
                paramTypeNodes.push_back(nullptr);
            }
        }
        IRNode* retTypeNode = nullptr;
        if (expr->returnType) {
            expr->returnType->accept(*this);
            retTypeNode = lastValue;
        }

        auto fnDef = std::make_shared<CompiledFunction>();
        fnDef->name = expr->name.empty() ? "lambda" : expr->name;
        
        IRGraph fnGraph;
        IRBuilder fnBuilder(&fnGraph, compiledFunctions, this, fnDef.get(), exprSymbols, patternSymbols);
        
        fnBuilder.currentReturnTypeHint = expr->returnType;
        fnBuilder.buildFunctionParams(expr->params, expr->defaultExprs, expr->hasRestParam, expr->paramIsRef, expr->paramIsConst, expr->paramTypes);
        
        fnBuilder.build(expr->body.get());
        
        if (g_showIR) fnGraph.print("Lambda Unoptimized");
        
        IROptimizer::optimize(&fnGraph);
        if (g_showIR) fnGraph.print("Lambda Optimized");
        
        RegisterAllocator::allocate(&fnGraph);
        if (g_showIR) fnGraph.print("Lambda Allocated");
        
        for (auto& target : fnBuilder.upvalueTargets) {
            if (target.isLocal && target.localNode) {
                IRNode* localNode = target.localNode;
                int upvalIdx = target.index;
                CompiledFunction* childFn = fnDef.get();
                this->graph->postAllocCallbacks.push_back([childFn, upvalIdx, localNode]() {
                    childFn->upvalues[upvalIdx].index = localNode->getResolved()->physicalReg;
                });
            }
        }
        
        fnDef->localCount = Emitter::emit(&fnGraph, fnDef->chunk);
        
        compiledFunctions->push_back(fnDef);
        expr->fnIdx = static_cast<int>(compiledFunctions->size()) - 1;

        IRNode* closureNode = graph->createValueNode(IROp::Closure);
        closureNode->setControl(currentControl);
        closureNode->name = std::to_string(expr->fnIdx);
        
        for (auto& target : fnBuilder.upvalueTargets) {
            if (target.isLocal && target.localNode) {
                closureNode->addData(target.localNode);
            }
        }

        CompiledFunction* childFn = fnDef.get();
        for (size_t i = 0; i < paramTypeNodes.size(); ++i) {
            IRNode* pNode = paramTypeNodes[i];
            this->graph->postAllocCallbacks.push_back([childFn, i, pNode]() {
                if (childFn->paramTypeRegs.size() <= i) childFn->paramTypeRegs.resize(i + 1, -1);
                childFn->paramTypeRegs[i] = pNode ? pNode->getResolved()->physicalReg : -1;
            });
            if (pNode) closureNode->addData(pNode);
        }
        this->graph->postAllocCallbacks.push_back([childFn, retTypeNode]() {
            childFn->returnTypeReg = retTypeNode ? retTypeNode->getResolved()->physicalReg : -1;
        });
        if (retTypeNode) closureNode->addData(retTypeNode);
        
        lastValue = closureNode;
    } else {
        IRNode* closureNode = graph->createValueNode(IROp::Closure);
        closureNode->setControl(currentControl);
        closureNode->name = std::to_string(expr->fnIdx);
        lastValue = closureNode;
    }
}

void IRBuilder::visitInvokeExpr(InvokeExpr* expr) {
    expr->callee->accept(*this);
    IRNode* calleeNode = lastValue;
        
    std::vector<IRNode*> argNodes;
    IRCallSignature sig;
    
    for (size_t i = 0; i < expr->arguments.size(); ++i) {
        auto& arg = expr->arguments[i];
        if (auto* var = dynamic_cast<Variable*>(arg.get())) {
            std::string name = var->name.lexeme;
            if (refParams.count(name)) {
                ResolvedSym rs; rs.scope = VarScope::RefParam;
                sig.refs.push_back({static_cast<uint8_t>(i), 4, name, -1, readVariable(name, rs)});
            } else {
                IRNode* localNode = getLocalNode(name);
                if (localNode) {
                    capturedLocals.insert(name);
                    capturedNodesToExtend.push_back(localNode);
                    sig.refs.push_back({static_cast<uint8_t>(i), 2, name, -1, localNode});
                } else {
                    int upvalIdx = -1;
                    if (currentFunction) {
                        for (int j = static_cast<int>(currentFunction->upvalues.size()) - 1; j >= 0; --j) {
                            if (currentFunction->upvalues[j].name == name) {
                                upvalIdx = j;
                                break;
                            }
                        }
                    }
                    if (upvalIdx == -1) {
                        auto varIt = exprSymbols->find(var);
                        ResolvedSym rs = varIt != exprSymbols->end() ? varIt->second : ResolvedSym{};
                        upvalIdx = resolveUpvalue(name, rs.scope == VarScope::CapturedState);
                    }
                    
                    if (upvalIdx != -1) {
                        sig.refs.push_back({static_cast<uint8_t>(i), 3, name, upvalIdx, nullptr});
                    } else {
                        sig.refs.push_back({static_cast<uint8_t>(i), 1, name, -1, nullptr});
                    }
                }
            }
        }
        arg->accept(*this);
        argNodes.push_back(lastValue);
    }
    
    if (!sig.refs.empty()) {
        graph->callSignatures.push_back(sig);
        IRNode* passRefsNode = graph->createNode(IROp::PassRefs);
        passRefsNode->setControl(currentControl);
        passRefsNode->payload1 = static_cast<uint32_t>(graph->callSignatures.size() - 1);
        for (auto& ref : sig.refs) {
            if (ref.localNode) passRefsNode->addData(ref.localNode);
        }
        currentControl = passRefsNode;
    }
        
    IRNode* invokeNode = graph->createValueNode(IROp::Call);
    invokeNode->setControl(currentControl);
    invokeNode->addData(calleeNode);
    for (auto* arg : argNodes) invokeNode->addData(arg);
    invokeNode->payload1 = static_cast<uint32_t>(argNodes.size());
        
    currentControl = invokeNode;
    lastValue = invokeNode;
}

void IRBuilder::visitForInExpr(ForInExpr* expr) {
    pushScope(); // ★ 自动创建块级作用域
    expr->iterable->accept(*this);
    IRNode* iterNode = graph->createValueNode(IROp::IterInit);
    iterNode->setControl(currentControl);
    iterNode->addData(lastValue);
    
    auto isDestructPattern = [](Pattern* p) {
        while (auto* dp = dynamic_cast<DefaultPattern*>(p)) p = dp->inner.get();
        return dynamic_cast<ListPattern*>(p) != nullptr || dynamic_cast<MatrixPattern*>(p) != nullptr;
    };
    iterNode->payload1 = isDestructPattern(expr->pattern.get()) ? 1 : 0;
    
    currentControl = iterNode;
    
    IRNode* loopNode = graph->createNode(IROp::Loop);
    loopNode->addData(currentControl);
    
    std::vector<std::unordered_map<std::string, IRNode*>> loopPhisStack(envStack.size());
    for (size_t i = 0; i < envStack.size(); ++i) {
        for (const auto& pair : envStack[i]) {
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(loopNode);
            phi->addData(pair.second);
            phi->name = pair.first;
            loopPhisStack[i][pair.first] = phi;
        }
    }
    envStack = loopPhisStack;
    currentControl = loopNode;
    
    IRNode* breakMerge = graph->createNode(IROp::Merge);
    IRNode* continueMerge = graph->createNode(IROp::Merge);
    loopStack.push_back({loopNode, loopPhisStack, breakMerge, {}, continueMerge, {}, activeTryCount});
    
    IRNode* nextNode = graph->createValueNode(IROp::IterNext);
    nextNode->setControl(currentControl);
    nextNode->addData(iterNode);
    currentControl = nextNode;
    
    IRNode* isUninitNode = graph->createValueNode(IROp::IsUninit);
    isUninitNode->setControl(currentControl);
    isUninitNode->addData(nextNode);
    
    IRNode* ifNode = graph->createNode(IROp::If);
    ifNode->setControl(currentControl);
    ifNode->addData(isUninitNode);
    
    IRNode* ifTrue = graph->createNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    breakMerge->addData(ifTrue);
    loopStack.back().breakEnvs.push_back(envStack);
    
    IRNode* ifFalse = graph->createNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    currentControl = ifFalse;
    
    auto envBeforeBody = envStack;
    IRNode* failMerge = graph->createNode(IROp::Merge);
    ScopeModifier mod = expr->isLocal ? ScopeModifier::Local : ScopeModifier::None;
    buildPatternMatch(expr->pattern.get(), nextNode, failMerge, mod, expr->isConst, true);
    
    expr->body->accept(*this);
    
    continueMerge->addData(currentControl);
    loopStack.back().continueEnvs.push_back(envStack);
    
    currentControl = continueMerge;
    
    auto& continueEnvs = loopStack.back().continueEnvs;
    for (size_t i = 0; i < envStack.size(); ++i) {
        for (const auto& pair : envStack[i]) {
            const std::string& name = pair.first;
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(continueMerge);
            for (auto& env : continueEnvs) {
                phi->addData(env[i].count(name) ? env[i].at(name) : graph->createConstant(Value::none()));
            }
            phi->name = name;
            envStack[i][name] = phi;
        }
    }
    
    loopNode->addData(currentControl);
    for (size_t i = 0; i < loopPhisStack.size(); ++i) {
        for (auto& pair : loopPhisStack[i]) {
            ResolvedSym rs; rs.scope = VarScope::Local;
            pair.second->addData(readVariable(pair.first, rs));
        }
    }
    
    if (!failMerge->dataInputs.empty()) {
        breakMerge->addData(failMerge);
        loopStack.back().breakEnvs.push_back(envBeforeBody);
    }
    
    currentControl = breakMerge;
    
    auto& breakEnvs = loopStack.back().breakEnvs;
    std::vector<std::unordered_map<std::string, IRNode*>> exitEnvStack(envStack.size());
    for (size_t i = 0; i < envStack.size(); ++i) {
        for (const auto& pair : envStack[i]) {
            const std::string& name = pair.first;
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(breakMerge);
            for (auto& env : breakEnvs) {
                phi->addData(env[i].count(name) ? env[i].at(name) : graph->createConstant(Value::none()));
            }
            phi->name = name;
            exitEnvStack[i][name] = phi;
        }
    }
    
    loopStack.pop_back();
    
    envStack = exitEnvStack;
    popScope(); // 离开块级作用域
    
    lastValue = graph->createConstant(Value::none());
    lastValue->setControl(currentControl);
}

void IRBuilder::visitThrowExpr(ThrowExpr* expr) {
    expr->value->accept(*this);
    graph->currentLine = expr->keyword.line;
    IRNode* throwNode = graph->createNode(IROp::Throw);
    throwNode->setControl(currentControl);
    throwNode->addData(lastValue);
    recordExitNode(throwNode);
    currentControl = throwNode;
    lastValue = throwNode;
}

void IRBuilder::visitTryCatchExpr(TryCatchExpr* expr) {
    IRNode* tryBegin = graph->createNode(IROp::TryBegin);
    tryBegin->setControl(currentControl);

    auto baseEnv = envStack;

    // 1. 正常分支
    IRNode* tryBodyStart = graph->createNode(IROp::Merge);
    tryBodyStart->addData(tryBegin);
    currentControl = tryBodyStart;

    activeTryCount++;
    expr->tryBody->accept(*this);
    activeTryCount--;
    
    IRNode* tryEnd = graph->createNode(IROp::TryEnd);
    tryEnd->setControl(currentControl);
    IRNode* normalControl = tryEnd;
    IRNode* normalVal = lastValue;
    auto normalEnv = envStack;

    // 2. 异常分支
    envStack = baseEnv;
    
    IRNode* catchNode = graph->createValueNode(IROp::Catch);
    catchNode->setControl(tryBegin);
    currentControl = catchNode;
    
    pushScope(); // Catch scope

    IRNode* errVal = catchNode; // Catch 节点产生异常对象

    IRNode* failMerge = graph->createNode(IROp::Merge);
    buildPatternMatch(expr->catchPattern.get(), errVal, failMerge, ScopeModifier::Local, false, true);

    if (!failMerge->dataInputs.empty()) {
        IRNode* throwNode = graph->createNode(IROp::Throw);
        throwNode->setControl(failMerge);
        throwNode->addData(errVal);
        recordExitNode(throwNode);
    }

    expr->catchBody->accept(*this);
    IRNode* catchControl = currentControl;
    IRNode* catchVal = lastValue;
    
    popScope();
    auto catchEnv = envStack;

    // 3. 汇合
    IRNode* mergeNode = graph->createNode(IROp::Merge);
    mergeNode->addData(normalControl);
    mergeNode->addData(catchControl);
    currentControl = mergeNode;

    envStack = baseEnv;
    for (size_t i = 0; i < baseEnv.size(); ++i) {
        std::unordered_set<std::string> modifiedVars;
        for (const auto& pair : normalEnv[i]) {
            if (baseEnv[i].count(pair.first) && baseEnv[i].at(pair.first) != pair.second) modifiedVars.insert(pair.first);
        }
        for (const auto& pair : catchEnv[i]) {
            if (baseEnv[i].count(pair.first) && baseEnv[i].at(pair.first) != pair.second) modifiedVars.insert(pair.first);
        }

        for (const auto& name : modifiedVars) {
            IRNode* nNode = normalEnv[i].count(name) ? normalEnv[i].at(name) : baseEnv[i].at(name);
            IRNode* cNode = catchEnv[i].count(name) ? catchEnv[i].at(name) : baseEnv[i].at(name);
            if (nNode != cNode) {
                IRNode* phi = graph->createValueNode(IROp::Phi);
                phi->setControl(mergeNode);
                phi->addData(nNode);
                phi->addData(cNode);
                phi->name = name;
                envStack[i][name] = phi;
            }
        }
    }

    if (normalVal != catchVal) {
        IRNode* resultPhi = graph->createValueNode(IROp::Phi);
        resultPhi->setControl(mergeNode);
        resultPhi->addData(normalVal);
        resultPhi->addData(catchVal);
        lastValue = resultPhi;
    } else {
        lastValue = normalVal;
    }
}

void IRBuilder::visitImportExpr(ImportExpr* expr) {
    expr->path->accept(*this);
    IRNode* importNode = graph->createValueNode(IROp::Import);
    importNode->setControl(currentControl);
    importNode->addData(lastValue);
    currentControl = importNode;
    lastValue = importNode;
}

void IRBuilder::visitSwitchExpr(SwitchExpr* expr) {
    expr->subject->accept(*this);
    IRNode* subjectNode = lastValue;
    
    IRNode* endMerge = graph->createNode(IROp::Merge);
    IRNode* resultPhi = graph->createValueNode(IROp::Phi);
    resultPhi->setControl(endMerge);
    
    IRNode* currentFailControl = currentControl;
    auto baseEnv = envStack;
    std::vector<std::vector<std::unordered_map<std::string, IRNode*>>> branchEnvs;
    
    for (auto& caseBranch : expr->cases) {
        currentControl = currentFailControl;
        envStack = baseEnv;
        pushScope(); // Scope for branch
        
        IRNode* caseSuccessMerge = graph->createNode(IROp::Merge);
        IRNode* nextCaseMerge = graph->createNode(IROp::Merge);
        
        for (auto& valExpr : caseBranch.first) {
            valExpr->accept(*this);
            IRNode* valNode = lastValue;
            
            IRNode* eqNode = graph->createValueNode(IROp::Eq);
            eqNode->setControl(currentControl);
            eqNode->addData(subjectNode);
            eqNode->addData(valNode);
            
            IRNode* ifNode = graph->createNode(IROp::If);
            ifNode->setControl(currentControl);
            ifNode->addData(eqNode);
            
            IRNode* ifTrue = graph->createNode(IROp::IfTrue);
            ifTrue->setControl(ifNode);
            caseSuccessMerge->addData(ifTrue);
            
            IRNode* ifFalse = graph->createNode(IROp::IfFalse);
            ifFalse->setControl(ifNode);
            currentControl = ifFalse;
        }
        nextCaseMerge->addData(currentControl);
        
        currentControl = caseSuccessMerge;
        caseBranch.second->accept(*this);
        endMerge->addData(currentControl);
        resultPhi->addData(lastValue);
        
        popScope();
        branchEnvs.push_back(envStack);
        currentFailControl = nextCaseMerge;
    }
    
    currentControl = currentFailControl;
    envStack = baseEnv;
    pushScope();
    if (expr->defaultBody) {
        expr->defaultBody->accept(*this);
        endMerge->addData(currentControl);
        resultPhi->addData(lastValue);
    } else {
        IRNode* noneNode = graph->createConstant(Value::none());
        noneNode->setControl(currentControl);
        endMerge->addData(currentControl);
        resultPhi->addData(noneNode);
    }
    popScope();
    branchEnvs.push_back(envStack);
    
    envStack = baseEnv;
    for (size_t i = 0; i < baseEnv.size(); ++i) {
        std::unordered_set<std::string> modifiedVars;
        for (auto& bEnv : branchEnvs) {
            for (const auto& pair : bEnv[i]) {
                if (baseEnv[i].count(pair.first) && baseEnv[i].at(pair.first) != pair.second) {
                    modifiedVars.insert(pair.first);
                }
            }
        }
        for (const auto& name : modifiedVars) {
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(endMerge);
            for (auto& bEnv : branchEnvs) {
                phi->addData(bEnv[i].count(name) ? bEnv[i].at(name) : baseEnv[i].at(name));
            }
            phi->name = name;
            envStack[i][name] = phi;
        }
    }
    
    currentControl = endMerge;
    lastValue = resultPhi;
}

void IRBuilder::visitClassDefExpr(ClassDefExpr* expr) {
    graph->currentLine = expr->name.line;
    
    std::unordered_set<std::string> instanceMembers;
    std::unordered_set<std::string> staticMembers;
    
    auto checkRedef = [&](const std::string& name, bool isStatic) {
        if (isStatic) {
            if (staticMembers.count(name)) error("Syntax Error: Static member '" + name + "' is redefined.");
            staticMembers.insert(name);
        } else {
            if (instanceMembers.count(name)) error("Syntax Error: Instance member '" + name + "' is redefined.");
            instanceMembers.insert(name);
        }
    };
    
    for (auto& m : expr->methods) checkRedef(m.name.lexeme, false);
    for (auto& f : expr->instanceFields) checkRedef(f.name.lexeme, false);
    for (auto& m : expr->staticMethods) checkRedef(m.name.lexeme, true);
    for (auto& f : expr->staticFields) checkRedef(f.name.lexeme, true);

    ClassContext ctx;
    ctx.name = expr->name.lexeme;
    for (auto& m : expr->methods) if (m.isLocal) ctx.privateMembers.insert(m.name.lexeme);
    for (auto& f : expr->instanceFields) if (f.isLocal) ctx.privateMembers.insert(f.name.lexeme);
    classStack.push_back(ctx);

    bool hasInit = false;
    for (auto& m : expr->methods) { if (m.name.lexeme == "init") hasInit = true; }
    if (!hasInit && !expr->instanceFields.empty()) {
        ClassDefExpr::MethodDef initDef = {
            Token(TokenType::IDENTIFIER, "init", expr->name.line),
            {}, {}, {}, {}, false, {}, nullptr, "",
            std::make_shared<Block>(std::vector<std::unique_ptr<Expr>>()),
            -1, false, false
        };
        expr->methods.push_back(std::move(initDef));
    }

    IRNode* classNode = graph->createValueNode(IROp::Class);
    classNode->setControl(currentControl);
    classNode->name = expr->name.lexeme;
    currentControl = classNode;
        
    if (expr->superClassExpr) {
        expr->superClassExpr->accept(*this);
        IRNode* superNode = lastValue;
        IRNode* inheritNode = graph->createNode(IROp::Inherit);
        inheritNode->setControl(currentControl);
        inheritNode->addData(classNode);
        inheritNode->addData(superNode);
        currentControl = inheritNode;
    }
        
    pushScope();
    declareVariable("<class>", classNode);
    currentLocalVars.insert("<class>");
    currentConstVars.insert("<class>");
    if (!expr->name.lexeme.empty()) {
        declareVariable(expr->name.lexeme, classNode);
        currentLocalVars.insert(expr->name.lexeme);
        currentConstVars.insert(expr->name.lexeme);
    }
        
    for (auto& method : expr->methods) {
        std::vector<IRNode*> paramTypeNodes;
        for (auto& pt : method.paramTypes) {
            if (pt) {
                pt->accept(*this);
                paramTypeNodes.push_back(lastValue);
            } else {
                paramTypeNodes.push_back(nullptr);
            }
        }
        IRNode* retTypeNode = nullptr;
        if (method.returnType) {
            method.returnType->accept(*this);
            retTypeNode = lastValue;
        }

        if (compiledFunctions) {
            auto fnDef = std::make_shared<CompiledFunction>();
            fnDef->name = method.name.lexeme;
            
            IRGraph fnGraph;
            IRBuilder fnBuilder(&fnGraph, compiledFunctions, this, fnDef.get(), exprSymbols, patternSymbols);
            if (method.name.lexeme == "init") {
                fnBuilder.isInitMethod = true;
                fnBuilder.instanceFieldsToInit = &expr->instanceFields;
            }
            
            fnBuilder.currentReturnTypeHint = method.returnType;
            fnBuilder.buildFunctionParams(method.params, method.defaultExprs, method.hasRestParam, method.paramIsRef, method.paramIsConst, method.paramTypes);
            
            fnBuilder.build(method.body.get());
            
            if (g_showIR) fnGraph.print("Method '" + method.name.lexeme + "' Unoptimized");
            
            IROptimizer::optimize(&fnGraph);
            if (g_showIR) fnGraph.print("Method '" + method.name.lexeme + "' Optimized");
            
            RegisterAllocator::allocate(&fnGraph);
            if (g_showIR) fnGraph.print("Method '" + method.name.lexeme + "' Allocated");
            
            for (auto& target : fnBuilder.upvalueTargets) {
                if (target.isLocal && target.localNode) {
                    IRNode* localNode = target.localNode;
                    int upvalIdx = target.index;
                    CompiledFunction* childFn = fnDef.get();
                    this->graph->postAllocCallbacks.push_back([childFn, upvalIdx, localNode]() {
                        childFn->upvalues[upvalIdx].index = localNode->getResolved()->physicalReg;
                    });
                }
            }
            
            fnDef->localCount = Emitter::emit(&fnGraph, fnDef->chunk);
            
            compiledFunctions->push_back(fnDef);
            method.fnIdx = static_cast<int>(compiledFunctions->size()) - 1;
            
            IRNode* methodClosure = graph->createValueNode(IROp::Closure);
            methodClosure->setControl(currentControl);
            methodClosure->name = std::to_string(method.fnIdx);
            
            for (auto& target : fnBuilder.upvalueTargets) {
                if (target.isLocal && target.localNode) {
                    methodClosure->addData(target.localNode);
                }
            }

            CompiledFunction* childFn = fnDef.get();
            for (size_t i = 0; i < paramTypeNodes.size(); ++i) {
                IRNode* pNode = paramTypeNodes[i];
                this->graph->postAllocCallbacks.push_back([childFn, i, pNode]() {
                    if (childFn->paramTypeRegs.size() <= i) childFn->paramTypeRegs.resize(i + 1, -1);
                    childFn->paramTypeRegs[i] = pNode ? pNode->getResolved()->physicalReg : -1;
                });
                if (pNode) methodClosure->addData(pNode);
            }
            this->graph->postAllocCallbacks.push_back([childFn, retTypeNode]() {
                childFn->returnTypeReg = retTypeNode ? retTypeNode->getResolved()->physicalReg : -1;
            });
            if (retTypeNode) methodClosure->addData(retTypeNode);

            IROp methodOp = IROp::Method;
            if (method.isLocal && method.isConst) methodOp = IROp::MethodPrivateConst;
            else if (method.isLocal) methodOp = IROp::MethodPrivate;
            else if (method.isConst) methodOp = IROp::MethodConst;

            IRNode* methodNode = graph->createNode(methodOp);
            methodNode->setControl(currentControl);
            methodNode->addData(classNode);
            methodNode->addData(methodClosure);
            methodNode->name = method.name.lexeme;
            currentControl = methodNode;
        } else {
            IRNode* methodClosure = graph->createValueNode(IROp::Closure);
            methodClosure->setControl(currentControl);
            methodClosure->name = std::to_string(method.fnIdx);

            IROp methodOp = IROp::Method;
            if (method.isLocal && method.isConst) methodOp = IROp::MethodPrivateConst;
            else if (method.isLocal) methodOp = IROp::MethodPrivate;
            else if (method.isConst) methodOp = IROp::MethodConst;

            IRNode* methodNode = graph->createNode(methodOp);
            methodNode->setControl(currentControl);
            methodNode->addData(classNode);
            methodNode->addData(methodClosure);
            methodNode->name = method.name.lexeme;
            currentControl = methodNode;
        }
    }

    for (auto& method : expr->staticMethods) {
        std::vector<IRNode*> paramTypeNodes;
        for (auto& pt : method.paramTypes) {
            if (pt) {
                pt->accept(*this);
                paramTypeNodes.push_back(lastValue);
            } else {
                paramTypeNodes.push_back(nullptr);
            }
        }
        IRNode* retTypeNode = nullptr;
        if (method.returnType) {
            method.returnType->accept(*this);
            retTypeNode = lastValue;
        }

        if (compiledFunctions) {
            auto fnDef = std::make_shared<CompiledFunction>();
            fnDef->name = method.name.lexeme;
            
            IRGraph fnGraph;
            IRBuilder fnBuilder(&fnGraph, compiledFunctions, this, fnDef.get(), exprSymbols, patternSymbols);
            
            fnBuilder.currentReturnTypeHint = method.returnType;
            fnBuilder.buildFunctionParams(method.params, method.defaultExprs, method.hasRestParam, method.paramIsRef, method.paramIsConst, method.paramTypes);
            
            fnBuilder.build(method.body.get());
            
            if (g_showIR) fnGraph.print("Static Method '" + method.name.lexeme + "' Unoptimized");
            
            IROptimizer::optimize(&fnGraph);
            if (g_showIR) fnGraph.print("Static Method '" + method.name.lexeme + "' Optimized");
            
            RegisterAllocator::allocate(&fnGraph);
            if (g_showIR) fnGraph.print("Static Method '" + method.name.lexeme + "' Allocated");
            
            for (auto& target : fnBuilder.upvalueTargets) {
                if (target.isLocal && target.localNode) {
                    IRNode* localNode = target.localNode;
                    int upvalIdx = target.index;
                    CompiledFunction* childFn = fnDef.get();
                    this->graph->postAllocCallbacks.push_back([childFn, upvalIdx, localNode]() {
                        childFn->upvalues[upvalIdx].index = localNode->getResolved()->physicalReg;
                    });
                }
            }
            
            fnDef->localCount = Emitter::emit(&fnGraph, fnDef->chunk);
            
            compiledFunctions->push_back(fnDef);
            method.fnIdx = static_cast<int>(compiledFunctions->size()) - 1;
            
            IRNode* methodClosure = graph->createValueNode(IROp::Closure);
            methodClosure->setControl(currentControl);
            methodClosure->name = std::to_string(method.fnIdx);
            
            for (auto& target : fnBuilder.upvalueTargets) {
                if (target.isLocal && target.localNode) {
                    methodClosure->addData(target.localNode);
                }
            }

            CompiledFunction* childFn = fnDef.get();
            for (size_t i = 0; i < paramTypeNodes.size(); ++i) {
                IRNode* pNode = paramTypeNodes[i];
                this->graph->postAllocCallbacks.push_back([childFn, i, pNode]() {
                    if (childFn->paramTypeRegs.size() <= i) childFn->paramTypeRegs.resize(i + 1, -1);
                    childFn->paramTypeRegs[i] = pNode ? pNode->getResolved()->physicalReg : -1;
                });
                if (pNode) methodClosure->addData(pNode);
            }
            this->graph->postAllocCallbacks.push_back([childFn, retTypeNode]() {
                childFn->returnTypeReg = retTypeNode ? retTypeNode->getResolved()->physicalReg : -1;
            });
            if (retTypeNode) methodClosure->addData(retTypeNode);

            IROp propOp = IROp::SetProperty;
            if (method.isLocal && method.isConst) propOp = IROp::DefinePrivateConst;
            else if (method.isLocal) propOp = IROp::DefinePrivate;
            else if (method.isConst) propOp = IROp::DefinePropConst;

            IRNode* setPropNode = graph->createValueNode(propOp);
            setPropNode->setControl(currentControl);
            setPropNode->addData(classNode);
            setPropNode->addData(methodClosure);
            setPropNode->name = method.name.lexeme;
            currentControl = setPropNode;
        } else {
            IRNode* methodClosure = graph->createValueNode(IROp::Closure);
            methodClosure->setControl(currentControl);
            methodClosure->name = std::to_string(method.fnIdx);

            IROp propOp = IROp::SetProperty;
            if (method.isLocal && method.isConst) propOp = IROp::DefinePrivateConst;
            else if (method.isLocal) propOp = IROp::DefinePrivate;
            else if (method.isConst) propOp = IROp::DefinePropConst;

            IRNode* setPropNode = graph->createValueNode(propOp);
            setPropNode->setControl(currentControl);
            setPropNode->addData(classNode);
            setPropNode->addData(methodClosure);
            setPropNode->name = method.name.lexeme;
            currentControl = setPropNode;
        }
    }

    for (auto& field : expr->staticFields) {
        field.value->accept(*this);
        IRNode* valNode = lastValue;

        IROp propOp = IROp::SetProperty;
        if (field.isLocal && field.isConst) propOp = IROp::DefinePrivateConst;
        else if (field.isLocal) propOp = IROp::DefinePrivate;
        else if (field.isConst) propOp = IROp::DefinePropConst;

        IRNode* setPropNode = graph->createValueNode(propOp);
        setPropNode->setControl(currentControl);
        setPropNode->addData(classNode);
        setPropNode->addData(valNode);
        setPropNode->name = field.name.lexeme;
        currentControl = setPropNode;
    }
        
    popScope();
    classStack.pop_back();
    lastValue = classNode;
}

static Value evaluateConstantExpr(Expr* expr) {
    if (!expr) return Value::none();
    if (auto* lit = dynamic_cast<Literal*>(expr)) {
        if (lit->isKeyword) {
            if (lit->value == "true") return Value(true);
            if (lit->value == "false") return Value(false);
            return Value::none();
        } else if (lit->isString) {
            return Value(lit->value);
        } else if (lit->isImaginary) {
            const std::string& s = lit->value;
            double imagPart = 0.0;
            if (s.length() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B' || s[1] == 'o' || s[1] == 'O')) {
                int base = 10;
                if (s[1] == 'x' || s[1] == 'X') base = 16;
                else if (s[1] == 'b' || s[1] == 'B') base = 2;
                else if (s[1] == 'o' || s[1] == 'O') base = 8;
                std::string numPart = s.substr(2);
                imagPart = BaseNum::fromString(numPart, base).getValue().toDouble();
            } else {
                imagPart = std::stod(s);
            }
            return Value(Complex(0.0, imagPart));
        } else {
            const std::string& s = lit->value;
            if (s.length() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B' || s[1] == 'o' || s[1] == 'O')) {
                int base = 10;
                if (s[1] == 'x' || s[1] == 'X') base = 16;
                else if (s[1] == 'b' || s[1] == 'B') base = 2;
                else if (s[1] == 'o' || s[1] == 'O') base = 8;
                std::string numPart = s.substr(2);
                return Value(BaseNum::fromString(numPart, base).getValue());
            } else if (s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
                try { return Value(jc::BigInt(s)); }
                catch (...) { return Value(std::stod(s)); }
            } else {
                return Value(std::stod(s));
            }
        }
    } else if (auto* un = dynamic_cast<Unary*>(expr)) {
        Value right = evaluateConstantExpr(un->right.get());
        GcValueGuard rightGuard(right);
        if (un->op.type == TokenType::MINUS) return -right;
        if (un->op.type == TokenType::PLUS) return right;
        if (un->op.type == TokenType::TILDE) return ~right;
        if (un->op.type == TokenType::BANG) return Value(!right.truthy());
    } else if (auto* bin = dynamic_cast<Binary*>(expr)) {
        Value left = evaluateConstantExpr(bin->left.get());
        GcValueGuard leftGuard(left);
        Value right = evaluateConstantExpr(bin->right.get());
        GcValueGuard rightGuard(right);
        if (bin->op.type == TokenType::PLUS) return left + right;
        if (bin->op.type == TokenType::MINUS) return left - right;
        if (bin->op.type == TokenType::STAR) return left * right;
        if (bin->op.type == TokenType::SLASH) return left / right;
        if (bin->op.type == TokenType::PERCENT) return left % right;
        if (bin->op.type == TokenType::SHIFT_LEFT) return left << right;
        if (bin->op.type == TokenType::SHIFT_RIGHT) return left >> right;
        if (bin->op.type == TokenType::BIT_AND) return left & right;
        if (bin->op.type == TokenType::BIT_OR) return left | right;
        if (bin->op.type == TokenType::BIT_XOR) return bitXor(left, right);
    }
    throw std::runtime_error("CompileError: Enum member value must be a compile-time constant.");
}

void IRBuilder::visitEnumDefExpr(EnumDefExpr* expr) {
    graph->currentLine = expr->name.line;
    
    ObjNamespace* ns = GcHeap::get().allocate<ObjNamespace>();
    Value nsVal(ns);
    GcValueGuard nsGuard(nsVal);
    
    ns->name = expr->name.lexeme;
    
    Value currentValue = Value::fromInt32(0);
    GcValueGuard cvGuard(currentValue);
    bool isIntEnum = true;
    
    for (auto& member : expr->members) {
        if (member.second) {
            try {
                currentValue = evaluateConstantExpr(member.second.get());
            } catch (const std::exception& e) {
                error(member.first.line, e.what());
            }
            isIntEnum = currentValue.isInt32() || currentValue.isBigInt();
        } else {
            if (!isIntEnum) {
                error(member.first.line, "CompileError: Cannot auto-increment enum value after a non-integer value.");
            }
        }
        
        ObjUpVal* upval = GcHeap::get().allocate<ObjUpVal>();
        upval->closed = currentValue;
        upval->location = &upval->closed;
        
        NamespaceField field;
        field.upval = upval;
        field.isConst = true;
        
        ns->fields[member.first.lexeme] = field;
        
        if (isIntEnum) {
            currentValue = currentValue + Value::fromInt32(1);
        }
    }
    
    ns->is_frozen = true;
    
    IRNode* node = graph->createConstant(nsVal);
    node->setControl(currentControl);
    lastValue = node;
}

void IRBuilder::visitNamespaceDecl(NamespaceDecl* expr) {
    graph->currentLine = expr->name.line;
    
    int prevNamespaceDepth = namespaceScopeDepth;
    namespaceScopeDepth = static_cast<int>(envStack.size());
    pushScope();
    
    currentLocalVars.clear();
    currentConstVars.clear();
    
    IRNode* nsSelfNode = graph->createNode(IROp::Nop);
    declareVariable("<namespace>", nsSelfNode);
    currentLocalVars.insert("<namespace>");
    currentConstVars.insert("<namespace>");
    if (!expr->name.lexeme.empty()) {
        declareVariable(expr->name.lexeme, nsSelfNode);
        currentLocalVars.insert(expr->name.lexeme);
        currentConstVars.insert(expr->name.lexeme);
    }
    
    if (auto* block = dynamic_cast<Block*>(expr->body.get())) {
        hoistBlock(block);
        for (auto& stmt : block->statements) {
            stmt->accept(*this);
        }
    } else {
        expr->body->accept(*this);
    }
    
    std::vector<std::string> exportedKeys;
    std::vector<IRNode*> exportedNodes;
    std::vector<bool> exportedConsts;
    
    for (const auto& [k, v] : envStack.back()) {
        if (currentLocalVars.count(k)) continue;
        exportedKeys.push_back(k);
        exportedNodes.push_back(v);
        exportedConsts.push_back(currentConstVars.count(k) > 0);
        capturedNodesToExtend.push_back(v);
    }
    
    popScope();
    namespaceScopeDepth = prevNamespaceDepth;
    
    IRNode* nsNode = graph->createValueNode(IROp::BuildNamespace);
    nsNode->setControl(currentControl);
    nsNode->name = expr->name.lexeme;
    
    for (size_t i = 0; i < exportedKeys.size(); ++i) {
        IRNode* keyNode = graph->createConstant(Value(exportedKeys[i]));
        keyNode->setControl(currentControl);
        nsNode->addData(keyNode);
        
        IRNode* slotNode = graph->createConstant(Value(0.0));
        slotNode->setControl(currentControl);
        nsNode->addData(slotNode);
        
        IRNode* targetNode = exportedNodes[i];
        this->graph->postAllocCallbacks.push_back([slotNode, targetNode]() {
            slotNode->constVal = Value(static_cast<double>(targetNode->getResolved()->physicalReg));
        });
        
        IRNode* constNode = graph->createConstant(Value(exportedConsts[i]));
        constNode->setControl(currentControl);
        nsNode->addData(constNode);
    }
    
    for (size_t i = 0; i < exportedNodes.size(); ++i) {
        nsNode->addData(exportedNodes[i]);
    }
    
    nsNode->payload1 = static_cast<uint32_t>(exportedKeys.size());
    currentControl = nsNode;
    
    nsSelfNode->forwarding = nsNode;
    for (auto& nPtr : graph->getNodes()) {
        IRNode* n = nPtr.get();
        for (auto& din : n->dataInputs) {
            if (din == nsSelfNode) din = nsNode;
        }
    }
    
    lastValue = nsNode;
}
    
void IRBuilder::visitDotAccess(DotAccess* expr) {
    graph->currentLine = expr->field.line;
    if (dynamic_cast<SuperExpr*>(expr->object.get())) {
        IRNode* selfNode = graph->createValueNode(IROp::GetSelf);
        selfNode->setControl(currentControl);
        
        IRNode* node = graph->createValueNode(IROp::GetSuper);
        node->setControl(currentControl);
        node->addData(selfNode);
        node->name = expr->field.lexeme;
        currentControl = node;
        lastValue = node;
        return;
    }
    
    expr->object->accept(*this);
    IRNode* objNode = lastValue;
        
    IROp op = IROp::GetProperty;
    if (dynamic_cast<SelfExpr*>(expr->object.get()) && !classStack.empty()) {
        if (classStack.back().privateMembers.count(expr->field.lexeme)) {
            op = IROp::GetPrivate;
        }
    }

    IRNode* node = graph->createValueNode(op);
    node->setControl(currentControl);
    node->addData(objNode);
    node->name = expr->field.lexeme;
    currentControl = node;
    lastValue = node;
}

void IRBuilder::visitDotAssign(DotAssign* expr) {
    graph->currentLine = expr->field.line;
    expr->object->accept(*this);
    IRNode* objNode = lastValue;
        
    expr->value->accept(*this);
    IRNode* valNode = lastValue;
        
    if (valNode->op == IROp::Class || valNode->op == IROp::BuildNamespace) {
        if (valNode->name.empty()) valNode->name = expr->field.lexeme;
    }
        
    IROp op = IROp::SetProperty;
    if (dynamic_cast<SelfExpr*>(expr->object.get()) && !classStack.empty()) {
        if (classStack.back().privateMembers.count(expr->field.lexeme)) {
            op = IROp::SetPrivate;
        }
    }

    IRNode* node = graph->createValueNode(op);
    node->setControl(currentControl);
    node->addData(objNode);
    node->addData(valNode);
    node->name = expr->field.lexeme;
    currentControl = node;
    lastValue = valNode;
}

void IRBuilder::visitMethodCallExpr(MethodCallExpr* expr) {
    graph->currentLine = expr->method.line;
    bool isSuper = dynamic_cast<SuperExpr*>(expr->object.get()) != nullptr;
    
    IRNode* objNode = nullptr;
    if (isSuper) {
        objNode = graph->createValueNode(IROp::GetSelf);
        objNode->setControl(currentControl);
    } else {
        expr->object->accept(*this);
        objNode = lastValue;
    }
        
    std::vector<IRNode*> argNodes;
    IRCallSignature sig;
    
    for (size_t i = 0; i < expr->arguments.size(); ++i) {
        auto& arg = expr->arguments[i];
        if (auto* var = dynamic_cast<Variable*>(arg.get())) {
            std::string name = var->name.lexeme;
            if (refParams.count(name)) {
                ResolvedSym rs; rs.scope = VarScope::RefParam;
                sig.refs.push_back({static_cast<uint8_t>(i), 4, name, -1, readVariable(name, rs)});
            } else {
                IRNode* localNode = getLocalNode(name);
                if (localNode) {
                    capturedLocals.insert(name);
                    capturedNodesToExtend.push_back(localNode);
                    sig.refs.push_back({static_cast<uint8_t>(i), 2, name, -1, localNode});
                } else {
                    int upvalIdx = -1;
                    if (currentFunction) {
                        for (int j = static_cast<int>(currentFunction->upvalues.size()) - 1; j >= 0; --j) {
                            if (currentFunction->upvalues[j].name == name) {
                                upvalIdx = j;
                                break;
                            }
                        }
                    }
                    if (upvalIdx == -1) {
                        auto varIt = exprSymbols->find(var);
                        ResolvedSym rs = varIt != exprSymbols->end() ? varIt->second : ResolvedSym{};
                        upvalIdx = resolveUpvalue(name, rs.scope == VarScope::CapturedState);
                    }
                    
                    if (upvalIdx != -1) {
                        sig.refs.push_back({static_cast<uint8_t>(i), 3, name, upvalIdx, nullptr});
                    } else {
                        sig.refs.push_back({static_cast<uint8_t>(i), 1, name, -1, nullptr});
                    }
                }
            }
        }
        arg->accept(*this);
        argNodes.push_back(lastValue);
    }
    
    if (!sig.refs.empty()) {
        graph->callSignatures.push_back(sig);
        IRNode* passRefsNode = graph->createNode(IROp::PassRefs);
        passRefsNode->setControl(currentControl);
        passRefsNode->payload1 = static_cast<uint32_t>(graph->callSignatures.size() - 1);
        for (auto& ref : sig.refs) {
            if (ref.localNode) passRefsNode->addData(ref.localNode);
        }
        currentControl = passRefsNode;
    }
        
    bool hasFallback = false;
    IRNode* fallbackNode = nullptr;
    
    if (!isSuper) {
        std::string name = expr->method.lexeme;
        if (refParams.count(name)) {
            hasFallback = true;
            ResolvedSym rs; rs.scope = VarScope::RefParam;
            fallbackNode = readVariable(name, rs);
        } else {
            IRNode* localNode = getLocalNode(name);
            if (localNode) {
                hasFallback = true;
                ResolvedSym rs; rs.scope = VarScope::Local;
                fallbackNode = readVariable(name, rs);
            } else {
                int upvalIdx = -1;
                if (currentFunction) {
                    for (int j = static_cast<int>(currentFunction->upvalues.size()) - 1; j >= 0; --j) {
                        if (currentFunction->upvalues[j].name == name) {
                            upvalIdx = j;
                            break;
                        }
                    }
                }
                if (upvalIdx == -1) {
                    // Fallback is always a normal upvalue, not a captured state
                    upvalIdx = resolveUpvalue(name, false);
                }
                
                if (upvalIdx != -1) {
                    hasFallback = true;
                    ResolvedSym rs; rs.scope = VarScope::Upvalue;
                    fallbackNode = readVariable(name, rs);
                }
            }
        }
    }

    bool isPrivate = false;
    if (!isSuper && dynamic_cast<SelfExpr*>(expr->object.get()) && !classStack.empty()) {
        if (classStack.back().privateMembers.count(expr->method.lexeme)) {
            isPrivate = true;
        }
    }

    IROp op;
    if (isSuper) op = IROp::SuperInvoke;
    else if (isPrivate) op = IROp::InvokePrivate;
    else if (hasFallback) op = IROp::InvokeFallback;
    else op = IROp::Invoke;

    IRNode* invokeNode = graph->createValueNode(op);
    invokeNode->setControl(currentControl);
    invokeNode->addData(objNode);
    for (auto* arg : argNodes) {
        invokeNode->addData(arg);
    }
    if (hasFallback) {
        invokeNode->addData(fallbackNode);
    }
    invokeNode->payload1 = static_cast<uint32_t>(argNodes.size());
    invokeNode->name = expr->method.lexeme;
        
    currentControl = invokeNode;
    lastValue = invokeNode;
}

void IRBuilder::visitSuperExpr(SuperExpr*) {
    error("Syntax Error: 'super' can only be used for method calls or property access.");
}

void IRBuilder::visitSelfExpr(SelfExpr*) {
    IRNode* node = graph->createValueNode(IROp::GetSelf);
    node->setControl(currentControl);
    lastValue = node;
}

void IRBuilder::visitDestructAssign(DestructAssign* expr) {
    std::vector<std::tuple<std::string, ScopeModifier, bool, Pattern*>> boundVars;
    collectPatternVars(expr->pattern.get(), boundVars);
    
    std::vector<std::string> tempStateNames;
    std::vector<std::pair<std::string, std::pair<int, IRNode*>>> hiddenVars;

    if (expr->isLocal || expr->isState || expr->isConst) {
        std::vector<std::string> patVars;
        auto collectVars = [&](Pattern* p, auto& self) -> void {
            if (!p) return;
            if (auto* vp = dynamic_cast<VariablePattern*>(p)) {
                if (vp->name.lexeme != "_") patVars.push_back(vp->name.lexeme);
            } else if (auto* rp = dynamic_cast<RestPattern*>(p)) {
                if (rp->name.lexeme != "_") patVars.push_back(rp->name.lexeme);
            } else if (auto* lp = dynamic_cast<ListPattern*>(p)) {
                for (auto& e : lp->elements) self(e.get(), self);
                if (lp->rest) self(lp->rest.get(), self);
            } else if (auto* mp = dynamic_cast<MatrixPattern*>(p)) {
                for (auto& row : mp->rows) {
                    for (auto& e : row) self(e.get(), self);
                }
                if (mp->restRow) self(mp->restRow.get(), self);
            } else if (auto* dp = dynamic_cast<DictPattern*>(p)) {
                for (auto& e : dp->entries) self(e.second.get(), self);
                if (dp->rest) self(dp->rest.get(), self);
            } else if (auto* defp = dynamic_cast<DefaultPattern*>(p)) {
                self(defp->inner.get(), self);
            }
        };
        collectVars(expr->pattern.get(), collectVars);

        for (const auto& name : patVars) {
            for (int i = static_cast<int>(envStack.size()) - 1; i >= 0; --i) {
                auto it = envStack[i].find(name);
                if (it != envStack[i].end()) {
                    hiddenVars.push_back({name, {i, it->second}});
                    envStack[i].erase(it);
                    break;
                }
            }
        }
    }

    for (const auto& varTuple : boundVars) {
        const std::string& name = std::get<0>(varTuple);
        ScopeModifier mod = std::get<1>(varTuple);
        if (mod == ScopeModifier::None) {
            if (expr->isState) mod = ScopeModifier::State;
        }
        if (mod == ScopeModifier::State) {
            tempStateNames.push_back(name);
        }
    }
    
    IRNode* skipMerge = nullptr;
    auto baseEnv = envStack;
    
    if (expr->isState && !tempStateNames.empty() && currentFunction) {
        ResolvedSym sym; sym.scope = VarScope::State;
        IRNode* getVal = readVariable(tempStateNames[0], sym);
        IRNode* isUninit = graph->createValueNode(IROp::IsUninit);
        isUninit->addData(getVal);
        isUninit->setControl(currentControl);
            
        IRNode* ifNode = graph->createNode(IROp::If);
        ifNode->addData(isUninit);
        ifNode->setControl(currentControl);
            
        IRNode* ifTrue = graph->createNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
            
        IRNode* ifFalse = graph->createNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
            
        skipMerge = graph->createNode(IROp::Merge);
        skipMerge->addData(ifFalse);
            
        currentControl = ifTrue;
    }
        
    expr->value->accept(*this);
    IRNode* valNode = lastValue;
    
    for (const auto& hv : hiddenVars) {
        envStack[hv.second.first][hv.first] = hv.second.second;
    }

    IRNode* failMerge = graph->createNode(IROp::Merge);
    
    ScopeModifier mod = ScopeModifier::None;
    if (expr->isLocal) mod = ScopeModifier::Local;
    else if (expr->isRef) mod = ScopeModifier::Ref;
    else if (expr->isState) mod = ScopeModifier::State;

    buildPatternMatch(expr->pattern.get(), valNode, failMerge, mod, expr->isConst, true);
    
    if (!failMerge->dataInputs.empty()) {
        IRNode* throwNode = graph->createNode(IROp::Throw);
        throwNode->setControl(failMerge);
        IRNode* errStr = graph->createConstant(Value("TypeError: Destructuring pattern match failed."));
        errStr->setControl(failMerge);
        throwNode->addData(errStr);
        recordExitNode(throwNode);
    }
    
    if (skipMerge) {
        auto trueEnv = envStack;
        skipMerge->addData(currentControl);
        currentControl = skipMerge;
        
        envStack = baseEnv;
        for (size_t i = 0; i < baseEnv.size(); ++i) {
            std::unordered_set<std::string> modifiedVars;
            for (const auto& pair : trueEnv[i]) {
                if (baseEnv[i].count(pair.first) && baseEnv[i].at(pair.first) != pair.second) modifiedVars.insert(pair.first);
            }
            for (const auto& name : modifiedVars) {
                IRNode* tNode = trueEnv[i].count(name) ? trueEnv[i].at(name) : baseEnv[i].at(name);
                IRNode* fNode = baseEnv[i].at(name);
                if (tNode != fNode) {
                    IRNode* phi = graph->createValueNode(IROp::Phi);
                    phi->setControl(skipMerge);
                    phi->addData(tNode);
                    phi->addData(fNode);
                    phi->name = name;
                    envStack[i][name] = phi;
                }
            }
        }
        
        IRNode* phi = graph->createValueNode(IROp::Phi);
        phi->setControl(skipMerge);
        IRNode* noneNode = graph->createConstant(Value::none());
        noneNode->setControl(skipMerge);
        phi->addData(valNode);
        phi->addData(noneNode);
        lastValue = phi;
    } else {
        lastValue = valNode;
    }
}

void IRBuilder::visitFStringExpr(FStringExpr* expr) {
    std::vector<IRNode*> parts;
    for (size_t i = 0; i < expr->exprs.size(); ++i) {
        if (!expr->literals[i].empty()) {
            IRNode* litNode = graph->createConstant(Value(expr->literals[i]));
            litNode->setControl(currentControl);
            parts.push_back(litNode);
        }
        expr->exprs[i]->accept(*this);
        IRNode* exprVal = lastValue;
            
        if (!expr->formatSpecs[i].empty()) {
            IRNode* specNode = graph->createConstant(Value(expr->formatSpecs[i]));
            specNode->setControl(currentControl);
            IRNode* fmtNode = graph->createValueNode(IROp::FormatString);
            fmtNode->setControl(currentControl);
            fmtNode->addData(exprVal);
            fmtNode->addData(specNode);
            currentControl = fmtNode;
            parts.push_back(fmtNode);
        } else {
            IRNode* strNode = graph->createValueNode(IROp::Stringify);
            strNode->setControl(currentControl);
            strNode->addData(exprVal);
            currentControl = strNode;
            parts.push_back(strNode);
        }
    }
    if (!expr->literals.back().empty()) {
        IRNode* litNode = graph->createConstant(Value(expr->literals.back()));
        litNode->setControl(currentControl);
        parts.push_back(litNode);
    }
        
    IRNode* concatNode = graph->createValueNode(IROp::ConcatStrings);
    concatNode->setControl(currentControl);
    for (auto* p : parts) concatNode->addData(p);
    concatNode->payload1 = static_cast<uint32_t>(parts.size());
        
    currentControl = concatNode;
    lastValue = concatNode;
}

void IRBuilder::visitListCompExpr(ListCompExpr* expr) {
    pushScope(); // Scope for comprehension
    
    IRNode* listNode = graph->createValueNode(IROp::ListInit);
    listNode->setControl(currentControl);
    currentControl = listNode;
    
    buildCompClause(expr, 0, listNode);
    
    if (expr->forceList) {
        lastValue = listNode;
    } else {
        IRNode* endNode = graph->createValueNode(IROp::ListCompEnd);
        endNode->setControl(currentControl);
        endNode->addData(listNode);
        currentControl = endNode;
        lastValue = endNode;
    }
    
    popScope();
}

void IRBuilder::buildSetCompClause(SetCompExpr* expr, size_t clauseIdx, IRNode* setNode) {
    if (clauseIdx >= expr->clauses.size()) {
        expr->valueExpr->accept(*this);
        IRNode* valNode = lastValue;
        IRNode* appendNode = graph->createNode(IROp::SetAppend);
        appendNode->setControl(currentControl);
        appendNode->addData(setNode);
        appendNode->addData(valNode);
        currentControl = appendNode;
        return;
    }
    
    auto& clause = expr->clauses[clauseIdx];
    clause.iterable->accept(*this);
    IRNode* iterNode = graph->createValueNode(IROp::IterInit);
    iterNode->setControl(currentControl);
    iterNode->addData(lastValue);
    
    auto isDestructPattern = [](Pattern* p) {
        while (auto* dp = dynamic_cast<DefaultPattern*>(p)) p = dp->inner.get();
        return dynamic_cast<ListPattern*>(p) != nullptr || dynamic_cast<MatrixPattern*>(p) != nullptr;
    };
    iterNode->payload1 = isDestructPattern(clause.pattern.get()) ? 1 : 0;
    currentControl = iterNode;
    
    IRNode* loopNode = graph->createNode(IROp::Loop);
    loopNode->addData(currentControl);
    
    std::vector<std::unordered_map<std::string, IRNode*>> loopPhisStack(envStack.size());
    for (size_t i = 0; i < envStack.size(); ++i) {
        for (const auto& pair : envStack[i]) {
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(loopNode);
            phi->addData(pair.second);
            phi->name = pair.first;
            loopPhisStack[i][pair.first] = phi;
        }
    }
    envStack = loopPhisStack;
    currentControl = loopNode;
    
    IRNode* exitControl = graph->createNode(IROp::Merge);

    IRNode* nextNode = graph->createValueNode(IROp::IterNext);
    nextNode->setControl(currentControl);
    nextNode->addData(iterNode);
    currentControl = nextNode;
    
    IRNode* isUninitNode = graph->createValueNode(IROp::IsUninit);
    isUninitNode->setControl(currentControl);
    isUninitNode->addData(nextNode);
    
    IRNode* ifNode = graph->createNode(IROp::If);
    ifNode->setControl(currentControl);
    ifNode->addData(isUninitNode);
    
    IRNode* ifTrue = graph->createNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    exitControl->addData(ifTrue);
    auto exitEnvStack = envStack;
    
    IRNode* ifFalse = graph->createNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    currentControl = ifFalse;
    
    auto envBeforePattern = envStack;
    IRNode* failMerge = graph->createNode(IROp::Merge);
    buildPatternMatch(clause.pattern.get(), nextNode, failMerge, ScopeModifier::Local, false, true);
    
    auto envBeforeCond = envStack;
    IRNode* condFailMerge = graph->createNode(IROp::Merge);
    for (auto& cond : clause.conditions) {
        cond->accept(*this);
        IRNode* condIfNode = graph->createNode(IROp::If);
        condIfNode->setControl(currentControl);
        condIfNode->addData(lastValue);
        
        IRNode* condIfTrue = graph->createNode(IROp::IfTrue);
        condIfTrue->setControl(condIfNode);
        
        IRNode* condIfFalse = graph->createNode(IROp::IfFalse);
        condIfFalse->setControl(condIfNode);
        condFailMerge->addData(condIfFalse);
        
        currentControl = condIfTrue;
    }
    
    buildSetCompClause(expr, clauseIdx + 1, setNode);
    
    loopNode->addData(currentControl);
    if (!condFailMerge->dataInputs.empty()) loopNode->addData(condFailMerge);
    if (!failMerge->dataInputs.empty()) loopNode->addData(failMerge);
    
    for (size_t i = 0; i < loopPhisStack.size(); ++i) {
        for (auto& pair : loopPhisStack[i]) {
            ResolvedSym rs; rs.scope = VarScope::Local;
            pair.second->addData(readVariable(pair.first, rs));
            if (!condFailMerge->dataInputs.empty()) {
                pair.second->addData(envBeforeCond[i].count(pair.first) ? envBeforeCond[i].at(pair.first) : graph->createConstant(Value::none()));
            }
            if (!failMerge->dataInputs.empty()) {
                pair.second->addData(envBeforePattern[i].count(pair.first) ? envBeforePattern[i].at(pair.first) : graph->createConstant(Value::none()));
            }
        }
    }
    
    envStack = exitEnvStack;
    currentControl = exitControl;
}

void IRBuilder::visitSetCompExpr(SetCompExpr* expr) {
    pushScope();
    
    IRNode* setNode = graph->createValueNode(IROp::SetInit);
    setNode->setControl(currentControl);
    currentControl = setNode;
    
    buildSetCompClause(expr, 0, setNode);
    
    lastValue = setNode;
    popScope();
}

void IRBuilder::buildDictCompClause(DictCompExpr* expr, size_t clauseIdx, IRNode* dictNode) {
    if (clauseIdx >= expr->clauses.size()) {
        expr->keyExpr->accept(*this);
        IRNode* keyNode = lastValue;
        expr->valueExpr->accept(*this);
        IRNode* valNode = lastValue;
        IRNode* appendNode = graph->createNode(IROp::DictAppend);
        appendNode->setControl(currentControl);
        appendNode->addData(dictNode);
        appendNode->addData(keyNode);
        appendNode->addData(valNode);
        currentControl = appendNode;
        return;
    }
    
    auto& clause = expr->clauses[clauseIdx];
    clause.iterable->accept(*this);
    IRNode* iterNode = graph->createValueNode(IROp::IterInit);
    iterNode->setControl(currentControl);
    iterNode->addData(lastValue);
    
    auto isDestructPattern = [](Pattern* p) {
        while (auto* dp = dynamic_cast<DefaultPattern*>(p)) p = dp->inner.get();
        return dynamic_cast<ListPattern*>(p) != nullptr || dynamic_cast<MatrixPattern*>(p) != nullptr;
    };
    iterNode->payload1 = isDestructPattern(clause.pattern.get()) ? 1 : 0;
    currentControl = iterNode;
    
    IRNode* loopNode = graph->createNode(IROp::Loop);
    loopNode->addData(currentControl);
    
    std::vector<std::unordered_map<std::string, IRNode*>> loopPhisStack(envStack.size());
    for (size_t i = 0; i < envStack.size(); ++i) {
        for (const auto& pair : envStack[i]) {
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(loopNode);
            phi->addData(pair.second);
            phi->name = pair.first;
            loopPhisStack[i][pair.first] = phi;
        }
    }
    envStack = loopPhisStack;
    currentControl = loopNode;
    
    IRNode* exitControl = graph->createNode(IROp::Merge);

    IRNode* nextNode = graph->createValueNode(IROp::IterNext);
    nextNode->setControl(currentControl);
    nextNode->addData(iterNode);
    currentControl = nextNode;
    
    IRNode* isUninitNode = graph->createValueNode(IROp::IsUninit);
    isUninitNode->setControl(currentControl);
    isUninitNode->addData(nextNode);
    
    IRNode* ifNode = graph->createNode(IROp::If);
    ifNode->setControl(currentControl);
    ifNode->addData(isUninitNode);
    
    IRNode* ifTrue = graph->createNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    exitControl->addData(ifTrue);
    auto exitEnvStack = envStack;
    
    IRNode* ifFalse = graph->createNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    currentControl = ifFalse;
    
    auto envBeforePattern = envStack;
    IRNode* failMerge = graph->createNode(IROp::Merge);
    buildPatternMatch(clause.pattern.get(), nextNode, failMerge, ScopeModifier::Local, false, true);
    
    auto envBeforeCond = envStack;
    IRNode* condFailMerge = graph->createNode(IROp::Merge);
    for (auto& cond : clause.conditions) {
        cond->accept(*this);
        IRNode* condIfNode = graph->createNode(IROp::If);
        condIfNode->setControl(currentControl);
        condIfNode->addData(lastValue);
        
        IRNode* condIfTrue = graph->createNode(IROp::IfTrue);
        condIfTrue->setControl(condIfNode);
        
        IRNode* condIfFalse = graph->createNode(IROp::IfFalse);
        condIfFalse->setControl(condIfNode);
        condFailMerge->addData(condIfFalse);
        
        currentControl = condIfTrue;
    }
    
    buildDictCompClause(expr, clauseIdx + 1, dictNode);
    
    loopNode->addData(currentControl);
    if (!condFailMerge->dataInputs.empty()) loopNode->addData(condFailMerge);
    if (!failMerge->dataInputs.empty()) loopNode->addData(failMerge);
    
    for (size_t i = 0; i < loopPhisStack.size(); ++i) {
        for (auto& pair : loopPhisStack[i]) {
            ResolvedSym rs; rs.scope = VarScope::Local;
            pair.second->addData(readVariable(pair.first, rs));
            if (!condFailMerge->dataInputs.empty()) {
                pair.second->addData(envBeforeCond[i].count(pair.first) ? envBeforeCond[i].at(pair.first) : graph->createConstant(Value::none()));
            }
            if (!failMerge->dataInputs.empty()) {
                pair.second->addData(envBeforePattern[i].count(pair.first) ? envBeforePattern[i].at(pair.first) : graph->createConstant(Value::none()));
            }
        }
    }
    
    envStack = exitEnvStack;
    currentControl = exitControl;
}

void IRBuilder::visitDictCompExpr(DictCompExpr* expr) {
    pushScope();
    
    IRNode* dictNode = graph->createValueNode(IROp::DictInit);
    dictNode->setControl(currentControl);
    currentControl = dictNode;
    
    buildDictCompClause(expr, 0, dictNode);
    
    lastValue = dictNode;
    popScope();
}
    
void IRBuilder::visitDictLiteral(DictLiteral* expr) {
    std::vector<IRNode*> keysAndVals;
    for (auto& pair : expr->entries) {
        pair.first->accept(*this);
        keysAndVals.push_back(lastValue);
        pair.second->accept(*this);
        keysAndVals.push_back(lastValue);
    }
    IRNode* node = graph->createValueNode(IROp::BuildDict);
    node->setControl(currentControl);
    for (auto* kv : keysAndVals) node->addData(kv);
    node->payload1 = static_cast<uint32_t>(expr->entries.size());
    currentControl = node;
    lastValue = node;
}

void IRBuilder::visitSetLiteral(SetLiteral* expr) {
    std::vector<IRNode*> elements;
    for (auto& e : expr->elements) {
        e->accept(*this);
        elements.push_back(lastValue);
    }
    IRNode* node = graph->createValueNode(IROp::BuildSet);
    node->setControl(currentControl);
    for (auto* e : elements) node->addData(e);
    node->payload1 = static_cast<uint32_t>(expr->elements.size());
    currentControl = node;
    lastValue = node;
}

void IRBuilder::visitSliceExpr(SliceExpr*) {
    error("Syntax Error: Slice expression should be handled by visitIndexAccess.");
}
    
void IRBuilder::visitSequenceExpr(SequenceExpr* expr) {
    for (size_t i = 0; i < expr->expressions.size(); ++i) {
        expr->expressions[i]->accept(*this);
        
        bool isTerminal = dynamic_cast<ReturnExpr*>(expr->expressions[i].get()) ||
                          dynamic_cast<BreakExpr*>(expr->expressions[i].get()) ||
                          dynamic_cast<ContinueExpr*>(expr->expressions[i].get()) ||
                          dynamic_cast<ThrowExpr*>(expr->expressions[i].get());
        if (i < expr->expressions.size() - 1 && isTerminal) {
            break;
        }
    }
}

void IRBuilder::visitMacroDefExpr(MacroDefExpr*) {
    error("Syntax Error: Macro definition is not allowed here.");
}

void IRBuilder::visitMacroCallExpr(MacroCallExpr*) {
    error("Syntax Error: Macro call is not allowed here.");
}

void IRBuilder::visitQuoteExpr(QuoteExpr*) {
    error("Syntax Error: 'quote' block is not allowed here.");
}

void IRBuilder::visitUnquoteExpr(UnquoteExpr*) {
    error("Syntax Error: Unquote '$' can only be used inside a 'quote' block.");
}

void IRBuilder::visitExprAssign(ExprAssign*) {
    error("Syntax Error: Invalid assignment target.");
}

void IRBuilder::visitDeferExpr(DeferExpr* expr) {
    if (compiledFunctions) {
        auto fnDef = std::make_shared<CompiledFunction>();
        fnDef->name = "<defer>";
        
        IRGraph fnGraph;
        IRBuilder fnBuilder(&fnGraph, compiledFunctions, this, fnDef.get(), exprSymbols, patternSymbols);
        
        fnBuilder.buildFunctionParams({}, {}, false, {}, {}, {});
        fnBuilder.build(expr->body.get());
        
        IROptimizer::optimize(&fnGraph);
        RegisterAllocator::allocate(&fnGraph);
        
        for (auto& target : fnBuilder.upvalueTargets) {
            if (target.isLocal && target.localNode) {
                IRNode* localNode = target.localNode;
                int upvalIdx = target.index;
                CompiledFunction* childFn = fnDef.get();
                this->graph->postAllocCallbacks.push_back([childFn, upvalIdx, localNode]() {
                    childFn->upvalues[upvalIdx].index = localNode->getResolved()->physicalReg;
                });
            }
        }
        
        fnDef->localCount = Emitter::emit(&fnGraph, fnDef->chunk);
        compiledFunctions->push_back(fnDef);
        int fnIdx = static_cast<int>(compiledFunctions->size()) - 1;

        IRNode* closureNode = graph->createValueNode(IROp::Closure);
        closureNode->setControl(currentControl);
        closureNode->name = std::to_string(fnIdx);
        
        for (auto& target : fnBuilder.upvalueTargets) {
            if (target.isLocal && target.localNode) {
                closureNode->addData(target.localNode);
            }
        }
        
        IRNode* deferNode = graph->createNode(IROp::Defer);
        deferNode->setControl(currentControl);
        deferNode->addData(closureNode);
        currentControl = deferNode;
        
        deferCounts.back()++;
        lastValue = graph->createConstant(Value::none());
        lastValue->setControl(currentControl);
    } else {
        error("Syntax Error: 'defer' cannot be used at the top level without a compiled function context.");
    }
}

void IRBuilder::visitMatchExpr(MatchExpr* expr) {
    expr->subject->accept(*this);
    IRNode* subjectNode = lastValue;
    
    IRNode* endMerge = graph->createNode(IROp::Merge);
    IRNode* resultPhi = graph->createValueNode(IROp::Phi);
    resultPhi->setControl(endMerge);
    
    IRNode* currentFailControl = currentControl;
    auto baseEnv = envStack;
    std::vector<std::vector<std::unordered_map<std::string, IRNode*>>> branchEnvs;
    
    for (auto& branch : expr->branches) {
        currentControl = currentFailControl;
        envStack = baseEnv;
        pushScope(); // Scope for branch
        
        IRNode* branchSuccessMerge = graph->createNode(IROp::Merge);
        IRNode* nextBranchMerge = graph->createNode(IROp::Merge);
        
        for (auto& pat : branch.patterns) {
            IRNode* patFailMerge = graph->createNode(IROp::Merge);
            
            buildPatternMatch(pat.get(), subjectNode, patFailMerge, ScopeModifier::Local, false, false);
            
            branchSuccessMerge->addData(currentControl);
            currentControl = patFailMerge;
        }
        nextBranchMerge->addData(currentControl);
        
        currentControl = branchSuccessMerge;
        if (branch.guard) {
            branch.guard->accept(*this);
            IRNode* ifNode = graph->createNode(IROp::If);
            ifNode->setControl(currentControl);
            ifNode->addData(lastValue);
            
            IRNode* ifTrue = graph->createNode(IROp::IfTrue);
            ifTrue->setControl(ifNode);
            
            IRNode* ifFalse = graph->createNode(IROp::IfFalse);
            ifFalse->setControl(ifNode);
            nextBranchMerge->addData(ifFalse);
            
            currentControl = ifTrue;
        }
        
        branch.body->accept(*this);
        endMerge->addData(currentControl);
        resultPhi->addData(lastValue);
        
        popScope();
        branchEnvs.push_back(envStack);
        currentFailControl = nextBranchMerge;
    }
    
    currentControl = currentFailControl;
    envStack = baseEnv;
    pushScope();
    IRNode* noneNode = graph->createConstant(Value::none());
    noneNode->setControl(currentControl);
    endMerge->addData(currentControl);
    resultPhi->addData(noneNode);
    popScope();
    branchEnvs.push_back(envStack); // Fallback branch environment
    
    envStack = baseEnv;
    for (size_t i = 0; i < baseEnv.size(); ++i) {
        std::unordered_set<std::string> modifiedVars;
        for (auto& bEnv : branchEnvs) {
            for (const auto& pair : bEnv[i]) {
                if (baseEnv[i].count(pair.first) && baseEnv[i].at(pair.first) != pair.second) {
                    modifiedVars.insert(pair.first);
                }
            }
        }
        for (const auto& name : modifiedVars) {
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(endMerge);
            for (auto& bEnv : branchEnvs) {
                phi->addData(bEnv[i].count(name) ? bEnv[i].at(name) : baseEnv[i].at(name));
            }
            phi->name = name;
            envStack[i][name] = phi;
        }
    }
    
    currentControl = endMerge;
    lastValue = resultPhi;
}

} // namespace jc
