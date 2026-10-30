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
        link(node);
        return node;
    }

    if (sym.scope == VarScope::Global) {
        IRNode* node = graph->createValueNode(IROp::GetGlobal);
        node->name = name;
        link(node);
        currentSeq = node;
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
        link(node);
        return node;
    } else if (sym.scope == VarScope::RefParam) {
        IRNode* node = graph->createValueNode(IROp::GetRefParam);
        node->payload1 = refParams[name];
        node->name = name;
        link(node);
        return node;
    } else {
        IRNode* localNode = getLocalNode(name);
        if (localNode) return localNode;
        IRNode* uninit = graph->createConstant(Value::uninit());
        link(uninit);
        return uninit;
    }
}

void IRBuilder::writeVariable(const std::string& name, IRNode* value, ResolvedSym sym, bool isExplicitLocal, bool isExplicitConst) {
    if (name == "_") return;
    namedValueNodes[name] = value;
    if (sym.scope == VarScope::Global) {
        IROp op = isExplicitConst ? IROp::DefineConstGlobal : IROp::SetGlobal;
        IRNode* node = graph->createNode(op);
        link(node);
        node->addData(value);
        node->name = name;
        currentSeq = node;
        globalValueNodes[name] = value;
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
        link(node);
        currentSeq = node;
    } else if (sym.scope == VarScope::RefParam) {
        IRNode* node = graph->createNode(IROp::SetRefParam);
        node->payload1 = refParams[name];
        node->name = name;
        node->addData(value);
        link(node);
        currentSeq = node;
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
                link(updateNode);
                updateNode->addData(origNode);
                updateNode->addData(value);
                currentSeq = updateNode;
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

IRBuilder::IRBuilder(IRGraph* graph, std::vector<std::shared_ptr<CompiledFunction>>* compiledFunctions, IRBuilder* parent, CompiledFunction* currentFunction, const std::unordered_map<Expr*, ResolvedSym>* exprSymbols, const std::unordered_map<Pattern*, ResolvedSym>* patternSymbols, const std::unordered_map<DeleteExpr*, std::vector<ResolvedSym>>* deleteSyms) 
    : graph(graph), compiledFunctions(compiledFunctions), parent(parent), currentFunction(currentFunction), exprSymbols(exprSymbols), patternSymbols(patternSymbols), deleteSyms(deleteSyms), currentSeq(graph->startNode), currentControl(graph->startNode), seqCursor(graph->startNode), lastValue(nullptr) {
    if (parent) {
        classStack = parent->classStack;
        globalEnumFields = parent->globalEnumFields;
        isInitMethod = parent->isInitMethod;
        instancePropertiesToInit = parent->instancePropertiesToInit;
    }
    pushScope(); // 压入顶层作用域
}

void IRBuilder::link(IRNode* node) {
    node->seqPrev = seqCursor;
    seqCursor = node;
    node->setControl(currentSeq); // 保持既有控制/生命周期行为
}

IRNode* IRBuilder::createCFGNode(IROp op) {
    IRNode* node = graph->createNode(op);
    currentControl = node;
    return node;
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
        localNode->setControl(parent->currentSeq);
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
        uv.isRef = parent->currentFunction ? parent->currentFunction->upvalues[upvalue].isRef : false;
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
        if (!allowInternalNames && isReservedInternalName(name)) error("Compile Error: Illegal use of reserved internal name '" + name + "'.");
        bool isLocal = mod == ScopeModifier::Local;
        if (mod == ScopeModifier::State) {
            IRNode* getVal = readVariable(name, sym);
            IRNode* isUninit = graph->createValueNode(IROp::IsUninit);
            isUninit->addData(getVal);
            link(isUninit);
            
            IRNode* ifNode = createCFGNode(IROp::If);
            ifNode->addData(isUninit);
            link(ifNode);
            
            IRNode* ifTrue = createCFGNode(IROp::IfTrue);
            ifTrue->setControl(ifNode);
            
            IRNode* ifFalse = createCFGNode(IROp::IfFalse);
            ifFalse->setControl(ifNode);
            
            auto baseEnv = envStack;
            
            currentSeq = ifTrue;
            writeVariable(name, vNode, sym, isLocal, isConst);
            IRNode* trueCtrl = currentSeq;
            auto trueEnv = envStack;
            
            currentSeq = ifFalse;
            auto falseEnv = baseEnv;
            
            IRNode* merge = createCFGNode(IROp::Merge);
            merge->addData(trueCtrl);
            merge->addData(ifFalse);
            
            envStack = baseEnv;
            for (size_t i = 0; i < baseEnv.size(); ++i) {
                std::unordered_set<std::string> modifiedVars;
                for (const auto& pair : trueEnv[i]) {
                    if (baseEnv[i].count(pair.first) && baseEnv[i].at(pair.first) != pair.second) modifiedVars.insert(pair.first);
                }
                for (const auto& nameVar : modifiedVars) {
                    IRNode* tNode = trueEnv[i].count(nameVar) ? trueEnv[i].at(nameVar) : baseEnv[i].at(nameVar);
                    IRNode* fNode = falseEnv[i].count(nameVar) ? falseEnv[i].at(nameVar) : baseEnv[i].at(nameVar);
                    if (tNode != fNode) {
                        IRNode* phi = graph->createValueNode(IROp::Phi);
                        phi->setControl(merge);
                        phi->addData(tNode);
                        phi->addData(fNode);
                        phi->name = nameVar;
                        envStack[i][nameVar] = phi;
                    }
                }
            }
            
            currentSeq = merge;
        } else {
            writeVariable(name, vNode, sym, isLocal, isConst);
            if (mod == ScopeModifier::Ref && sym.scope == VarScope::Global && currentSeq->op == IROp::SetGlobal) {
                currentSeq->op = IROp::SetGlobalRef;
            }
        }
    };

    auto emitTypeAssert = [&](IRNode* valueNode, const std::shared_ptr<Expr>& typeHint, const std::string& name) {
        if (!typeHint) return;
        typeHint->accept(*this);
        IRNode* typeNode = lastValue;

        if (!isAssignment) {
            // match 模式：类型守卫，类型不匹配时跳 failMerge（试下一个分支），而非抛错
            IRNode* matchNode = graph->createValueNode(IROp::MatchType);
            link(matchNode);
            matchNode->addData(valueNode);
            matchNode->addData(typeNode);

            IRNode* ifNode = createCFGNode(IROp::If);
            link(ifNode);
            ifNode->addData(matchNode);

            IRNode* ifTrue = createCFGNode(IROp::IfTrue);
            ifTrue->setControl(ifNode);

            IRNode* ifFalse = createCFGNode(IROp::IfFalse);
            ifFalse->setControl(ifNode);
            failMerge->addData(ifFalse);

            currentSeq = ifTrue;
            return;
        }

        IRNode* assertNode = graph->createNode(IROp::AssertType);
        link(assertNode);
        assertNode->addData(valueNode);
        assertNode->addData(typeNode);
        assertNode->name = name;
        currentSeq = assertNode;
    };

    if (auto* dp = dynamic_cast<DefaultPattern*>(pat)) {
        IRNode* isUninit = graph->createValueNode(IROp::IsUninit);
        link(isUninit);
        isUninit->addData(valNode);
        
        IRNode* ifNode = createCFGNode(IROp::If);
        link(ifNode);
        ifNode->addData(isUninit);
        
        IRNode* ifTrue = createCFGNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = createCFGNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        
        currentSeq = ifTrue;
        dp->defaultExpr->accept(*this);
        IRNode* defVal = lastValue;
        IRNode* trueCtrl = currentSeq;
        
        IRNode* merge = createCFGNode(IROp::Merge);
        merge->addData(trueCtrl);
        merge->addData(ifFalse);
        
        IRNode* phi = graph->createValueNode(IROp::Phi);
        phi->setControl(merge);
        phi->addData(defVal);
        phi->addData(valNode);
        
        currentSeq = merge;
        buildPatternMatch(dp->inner.get(), phi, failMerge, globalMod, globalConst, isAssignment);
        return;
    }
    if (auto* vp = dynamic_cast<VariablePattern*>(pat)) {
        IRNode* isUninit = graph->createValueNode(IROp::IsUninit);
        link(isUninit);
        isUninit->addData(valNode);
        
        IRNode* ifNode = createCFGNode(IROp::If);
        link(ifNode);
        ifNode->addData(isUninit);
        
        IRNode* ifTrue = createCFGNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        failMerge->addData(ifTrue);
        
        IRNode* ifFalse = createCFGNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        currentSeq = ifFalse;

        if (vp->name.lexeme != "_") {
            auto it = patternSymbols->find(pat);
            ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
            ScopeModifier mod = vp->modifier != ScopeModifier::None ? vp->modifier : globalMod;
            
            bool isExplicitConst = vp->isConst || globalConst;
            if (mod == ScopeModifier::Ref && currentFunction && (sym.scope == VarScope::Upvalue || sym.scope == VarScope::CapturedState)) {
                int upvalIdx = resolveUpvalue(vp->name.lexeme, sym.scope == VarScope::CapturedState);
                if (upvalIdx != -1) {
                    currentFunction->upvalues[upvalIdx].isRef = true;
                }
            }
            emitTypeAssert(valNode, vp->typeHint, vp->name.lexeme);
            assignVar(vp->name.lexeme, valNode, sym, mod, isExplicitConst);
        } else {
            // _ 丢弃符：不绑定变量，但类型注解（如 _: int）仍需生效
            emitTypeAssert(valNode, vp->typeHint, vp->name.lexeme);
        }
    } else if (auto* lit = dynamic_cast<LiteralPattern*>(pat)) {
        IRNode* matchInit = graph->createValueNode(IROp::MatchInit);
        link(matchInit);
        matchInit->addData(valNode);
        currentSeq = matchInit;
        valNode = matchInit;

        lit->literal->accept(*this);
        IRNode* eqNode = graph->createValueNode(IROp::Eq);
        link(eqNode);
        eqNode->addData(valNode);
        eqNode->addData(lastValue);
        
        IRNode* ifNode = createCFGNode(IROp::If);
        link(ifNode);
        ifNode->addData(eqNode);
        
        IRNode* ifTrue = createCFGNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = createCFGNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        failMerge->addData(ifFalse);
        
        currentSeq = ifTrue;
    } else if (auto* exprPat = dynamic_cast<ExprPattern*>(pat)) {
        if (isAssignment) {
            if (auto* dot = dynamic_cast<DotAccess*>(exprPat->expr.get())) {
                graph->currentLine = dot->field.line;
                dot->object->accept(*this);
                IRNode* objNode = lastValue;
                
                IRNode* setProp = graph->createValueNode(IROp::SetProperty);
                link(setProp);
                setProp->addData(objNode);
                setProp->addData(valNode);
                setProp->name = dot->field.lexeme;
                currentSeq = setProp;
                
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
                    link(getProp);
                    getProp->addData(dotParentNode);
                    getProp->name = dotPropName;
                    currentSeq = getProp;
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

                {
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
                        link(setIdx);
                        setIdx->addData(objNode);
                        for (auto* iNode : allIndices[0]) setIdx->addData(iNode);
                        setIdx->addData(valNode);
                        setIdx->payload1 = static_cast<uint32_t>(allIndices[0].size());
                        currentSeq = setIdx;
                    } else {
                        std::vector<IRNode*> chainObjs;
                        chainObjs.push_back(objNode);
                    
                        for (size_t i = 0; i < chain.size() - 1; ++i) {
                            IRNode* getNode = graph->createValueNode(IROp::IndexGet);
                            link(getNode);
                            getNode->addData(chainObjs.back());
                            for (auto* idxNode : allIndices[i]) getNode->addData(idxNode);
                            getNode->payload1 = static_cast<uint32_t>(allIndices[i].size());
                            currentSeq = getNode;
                            chainObjs.push_back(getNode);
                        }
                    
                        IRNode* setNode = graph->createValueNode(IROp::IndexSet);
                        link(setNode);
                        setNode->addData(chainObjs.back());
                        for (auto* iNode : allIndices.back()) setNode->addData(iNode);
                        setNode->addData(valNode);
                        setNode->payload1 = static_cast<uint32_t>(allIndices.back().size());
                        currentSeq = setNode;
                    }
                }
            } else {
                error("Syntax Error: Invalid L-value in destructuring assignment.");
            }
        } else {
            IRNode* matchInit = graph->createValueNode(IROp::MatchInit);
            link(matchInit);
            matchInit->addData(valNode);
            currentSeq = matchInit;
            valNode = matchInit;

            exprPat->expr->accept(*this);
            IRNode* eqNode = graph->createValueNode(IROp::Eq);
            link(eqNode);
            eqNode->addData(valNode);
            eqNode->addData(lastValue);
            
            IRNode* ifNode = createCFGNode(IROp::If);
            link(ifNode);
            ifNode->addData(eqNode);
            
            IRNode* ifTrue = createCFGNode(IROp::IfTrue);
            ifTrue->setControl(ifNode);
            
            IRNode* ifFalse = createCFGNode(IROp::IfFalse);
            ifFalse->setControl(ifNode);
            failMerge->addData(ifFalse);
            
            currentSeq = ifTrue;
        }
    } else if (auto* dap = dynamic_cast<DynamicAssertPattern*>(pat)) {
        IRNode* matchInit = graph->createValueNode(IROp::MatchInit);
        link(matchInit);
        matchInit->addData(valNode);
        currentSeq = matchInit;
        valNode = matchInit;

        dap->expr->accept(*this);
        IRNode* eqNode = graph->createValueNode(IROp::Eq);
        link(eqNode);
        eqNode->addData(valNode);
        eqNode->addData(lastValue);
        
        IRNode* ifNode = createCFGNode(IROp::If);
        link(ifNode);
        ifNode->addData(eqNode);
        
        IRNode* ifTrue = createCFGNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = createCFGNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        failMerge->addData(ifFalse);
        
        currentSeq = ifTrue;
    } else if (auto* lp = dynamic_cast<ListPattern*>(pat)) {
        IRNode* matchInit = graph->createValueNode(IROp::MatchInit);
        link(matchInit);
        matchInit->addData(valNode);
        currentSeq = matchInit;
        valNode = matchInit;

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
        link(shapeNode);
        shapeNode->addData(valNode);
        shapeNode->payload1 = 1; // minRows
        shapeNode->payload2 = 1; // maxRows
        shapeNode->payload3 = minCols;
        shapeNode->payload4 = maxCols;
        shapeNode->payload5 = 2; // exactMask = 2 (1D pattern)
        
        IRNode* ifNode = createCFGNode(IROp::If);
        link(ifNode);
        ifNode->addData(shapeNode);
        
        IRNode* ifTrue = createCFGNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = createCFGNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        failMerge->addData(ifFalse);
        
        currentSeq = ifTrue;

        for (size_t i = 0; i < lp->elements.size(); ++i) {
            if (static_cast<int>(i) == restIndex) {
                auto* restPat = static_cast<RestPattern*>(lp->elements[i].get());
                IRNode* startNode = graph->createConstant(Value(static_cast<double>(i)));
                link(startNode);
                
                int tailCount = static_cast<int>(lp->elements.size()) - 1 - static_cast<int>(i);
                IRNode* endNode = tailCount > 0 ? graph->createConstant(Value(static_cast<double>(-tailCount))) : graph->createConstant(Value::none());
                link(endNode);
                
                IRNode* stepNode = graph->createConstant(Value::none());
                link(stepNode);
                
                IRNode* buildSliceNode = graph->createValueNode(IROp::BuildSlice);
                link(buildSliceNode);
                buildSliceNode->addData(startNode);
                buildSliceNode->addData(endNode);
                buildSliceNode->addData(stepNode);
                currentSeq = buildSliceNode;

                IRNode* sliceNode = graph->createValueNode(IROp::IndexGet);
                link(sliceNode);
                sliceNode->addData(valNode);
                sliceNode->addData(buildSliceNode);
                sliceNode->payload1 = 1;
                sliceNode->payload2 = 0;
                currentSeq = sliceNode;
                
                if (restPat->name.lexeme != "_") {
                    auto it = patternSymbols->find(restPat);
                    ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
                    ScopeModifier mod = restPat->modifier != ScopeModifier::None ? restPat->modifier : globalMod;
                    bool isExplicitConst = restPat->isConst || globalConst;
                    
                    if (mod == ScopeModifier::Ref && currentFunction && (sym.scope == VarScope::Upvalue || sym.scope == VarScope::CapturedState)) {
                        int upvalIdx = resolveUpvalue(restPat->name.lexeme, sym.scope == VarScope::CapturedState);
                        if (upvalIdx != -1) {
                            currentFunction->upvalues[upvalIdx].isRef = true;
                        }
                    }
                    emitTypeAssert(sliceNode, restPat->typeHint, restPat->name.lexeme);
                    assignVar(restPat->name.lexeme, sliceNode, sym, mod, isExplicitConst);
                } else {
                    // _ 丢弃符：不绑定变量，但类型注解（如 ..._: int）仍需生效
                    emitTypeAssert(sliceNode, restPat->typeHint, restPat->name.lexeme);
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
            link(idxNode);
            
            IRNode* elemNode = graph->createValueNode(IROp::IndexGet);
            link(elemNode);
            elemNode->addData(valNode);
            elemNode->addData(idxNode);
            elemNode->payload1 = 1;
            elemNode->payload2 = 1; // noThrow
            currentSeq = elemNode;
            
            buildPatternMatch(lp->elements[i].get(), elemNode, failMerge, globalMod, globalConst, isAssignment);
        }

        if (lp->rest) {
            auto* restPat = lp->rest.get();
            IRNode* startNode = graph->createConstant(Value(static_cast<double>(lp->elements.size())));
            link(startNode);
            
            IRNode* endNode = graph->createConstant(Value::none());
            link(endNode);
            
            IRNode* stepNode = graph->createConstant(Value::none());
            link(stepNode);
            
            IRNode* buildSliceNode = graph->createValueNode(IROp::BuildSlice);
            link(buildSliceNode);
            buildSliceNode->addData(startNode);
            buildSliceNode->addData(endNode);
            buildSliceNode->addData(stepNode);
            currentSeq = buildSliceNode;

            IRNode* sliceNode = graph->createValueNode(IROp::IndexGet);
            link(sliceNode);
            sliceNode->addData(valNode);
            sliceNode->addData(buildSliceNode);
            sliceNode->payload1 = 1;
            sliceNode->payload2 = 0;
            currentSeq = sliceNode;
            
            if (restPat->name.lexeme != "_") {
                auto it = patternSymbols->find(restPat);
                ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
                ScopeModifier mod = restPat->modifier != ScopeModifier::None ? restPat->modifier : globalMod;
                bool isExplicitConst = restPat->isConst || globalConst;
                
                if (mod == ScopeModifier::Ref && currentFunction && (sym.scope == VarScope::Upvalue || sym.scope == VarScope::CapturedState)) {
                    int upvalIdx = resolveUpvalue(restPat->name.lexeme, sym.scope == VarScope::CapturedState);
                    if (upvalIdx != -1) {
                        currentFunction->upvalues[upvalIdx].isRef = true;
                    }
                }
                emitTypeAssert(sliceNode, restPat->typeHint, restPat->name.lexeme);
                assignVar(restPat->name.lexeme, sliceNode, sym, mod, isExplicitConst);
            } else {
                // _ 丢弃符：不绑定变量，但类型注解（如 ..._: int）仍需生效
                emitTypeAssert(sliceNode, restPat->typeHint, restPat->name.lexeme);
            }
        }
    } else if (auto* mp = dynamic_cast<MatrixPattern*>(pat)) {
        IRNode* matchInit = graph->createValueNode(IROp::MatchInit);
        link(matchInit);
        matchInit->addData(valNode);
        currentSeq = matchInit;
        valNode = matchInit;

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
        link(shapeNode);
        shapeNode->addData(valNode);
        shapeNode->payload1 = minRows;
        shapeNode->payload2 = maxRows;
        shapeNode->payload3 = minCols;
        shapeNode->payload4 = maxCols;
        shapeNode->payload5 = 0; // exactMask = 0 (2D pattern)
        
        IRNode* ifNode = createCFGNode(IROp::If);
        link(ifNode);
        ifNode->addData(shapeNode);
        
        IRNode* ifTrue = createCFGNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = createCFGNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        failMerge->addData(ifFalse);
        
        currentSeq = ifTrue;

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
                        link(rStartNode);
                        IRNode* rEndNode = graph->createConstant(Value(static_cast<double>(i + 1)));
                        link(rEndNode);
                        IRNode* rStepNode = graph->createConstant(Value::none());
                        link(rStepNode);

                        IRNode* cStartNode = graph->createConstant(Value(static_cast<double>(j)));
                        link(cStartNode);
                        int tailCount = static_cast<int>(mp->rows[i].size()) - 1 - static_cast<int>(j);
                        IRNode* cEndNode = tailCount > 0 ? graph->createConstant(Value(static_cast<double>(-tailCount))) : graph->createConstant(Value::none());
                        link(cEndNode);
                        IRNode* cStepNode = graph->createConstant(Value::none());
                        link(cStepNode);

                        IRNode* rSliceNode = graph->createValueNode(IROp::BuildSlice);
                        link(rSliceNode);
                        rSliceNode->addData(rStartNode);
                        rSliceNode->addData(rEndNode);
                        rSliceNode->addData(rStepNode);
                        currentSeq = rSliceNode;

                        IRNode* cSliceNode = graph->createValueNode(IROp::BuildSlice);
                        link(cSliceNode);
                        cSliceNode->addData(cStartNode);
                        cSliceNode->addData(cEndNode);
                        cSliceNode->addData(cStepNode);
                        currentSeq = cSliceNode;

                        IRNode* sliceNode = graph->createValueNode(IROp::IndexGet);
                        link(sliceNode);
                        sliceNode->addData(valNode);
                        sliceNode->addData(rSliceNode);
                        sliceNode->addData(cSliceNode);
                        sliceNode->payload1 = 2;
                        sliceNode->payload2 = 0;
                        currentSeq = sliceNode;

                        auto it = patternSymbols->find(restPat);
                        ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
                        ScopeModifier mod = restPat->modifier != ScopeModifier::None ? restPat->modifier : globalMod;
                        bool isExplicitConst = restPat->isConst || globalConst;
            
                        if (mod == ScopeModifier::Ref && currentFunction && (sym.scope == VarScope::Upvalue || sym.scope == VarScope::CapturedState)) {
                            int upvalIdx = resolveUpvalue(restPat->name.lexeme, sym.scope == VarScope::CapturedState);
                            if (upvalIdx != -1) {
                                currentFunction->upvalues[upvalIdx].isRef = true;
                            }
                        }
                        emitTypeAssert(sliceNode, restPat->typeHint, restPat->name.lexeme);
                        assignVar(restPat->name.lexeme, sliceNode, sym, mod, isExplicitConst);
                    }
                    continue;
                }

                IRNode* rIdxNode = graph->createConstant(Value(static_cast<double>(i)));
                link(rIdxNode);

                IRNode* cIdxNode = nullptr;
                if (rowRestColIndex != -1 && static_cast<int>(j) > rowRestColIndex) {
                    int offsetFromEnd = static_cast<int>(mp->rows[i].size()) - static_cast<int>(j);
                    cIdxNode = graph->createConstant(Value(static_cast<double>(-offsetFromEnd)));
                } else {
                    cIdxNode = graph->createConstant(Value(static_cast<double>(j)));
                }
                link(cIdxNode);

                IRNode* elemNode = graph->createValueNode(IROp::IndexGet);
                link(elemNode);
                elemNode->addData(valNode);
                elemNode->addData(rIdxNode);
                elemNode->addData(cIdxNode);
                elemNode->payload1 = 2;
                elemNode->payload2 = 1; // noThrow
                currentSeq = elemNode;

                buildPatternMatch(mp->rows[i][j].get(), elemNode, failMerge, globalMod, globalConst, isAssignment);
            }
        }

        if (hasRestRow && mp->restRow->name.lexeme != "_") {
            auto* restPat = mp->restRow.get();
            IRNode* rStartNode = graph->createConstant(Value(static_cast<double>(mp->rows.size())));
            link(rStartNode);
            IRNode* rEndNode = graph->createConstant(Value::none());
            link(rEndNode);
            IRNode* rStepNode = graph->createConstant(Value::none());
            link(rStepNode);

            IRNode* cStartNode = graph->createConstant(Value(0.0));
            link(cStartNode);
            IRNode* cEndNode = graph->createConstant(Value::none());
            link(cEndNode);
            IRNode* cStepNode = graph->createConstant(Value::none());
            link(cStepNode);

            IRNode* rSliceNode = graph->createValueNode(IROp::BuildSlice);
            link(rSliceNode);
            rSliceNode->addData(rStartNode);
            rSliceNode->addData(rEndNode);
            rSliceNode->addData(rStepNode);
            currentSeq = rSliceNode;

            IRNode* cSliceNode = graph->createValueNode(IROp::BuildSlice);
            link(cSliceNode);
            cSliceNode->addData(cStartNode);
            cSliceNode->addData(cEndNode);
            cSliceNode->addData(cStepNode);
            currentSeq = cSliceNode;

            IRNode* sliceNode = graph->createValueNode(IROp::IndexGet);
            link(sliceNode);
            sliceNode->addData(valNode);
            sliceNode->addData(rSliceNode);
            sliceNode->addData(cSliceNode);
            sliceNode->payload1 = 2;
            sliceNode->payload2 = 0;
            currentSeq = sliceNode;

            auto it = patternSymbols->find(restPat);
            ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
            ScopeModifier mod = restPat->modifier != ScopeModifier::None ? restPat->modifier : globalMod;
            bool isExplicitConst = restPat->isConst || globalConst;
            
            if (mod == ScopeModifier::Ref && currentFunction && (sym.scope == VarScope::Upvalue || sym.scope == VarScope::CapturedState)) {
                int upvalIdx = resolveUpvalue(restPat->name.lexeme, sym.scope == VarScope::CapturedState);
                if (upvalIdx != -1) {
                    currentFunction->upvalues[upvalIdx].isRef = true;
                }
            }
            emitTypeAssert(sliceNode, restPat->typeHint, restPat->name.lexeme);
            assignVar(restPat->name.lexeme, sliceNode, sym, mod, isExplicitConst);
        }
    } else if (auto* dp = dynamic_cast<DictPattern*>(pat)) {
        IRNode* matchInit = graph->createValueNode(IROp::MatchInit);
        link(matchInit);
        matchInit->addData(valNode);
        currentSeq = matchInit;
        valNode = matchInit;

        auto createTypeCheck = [&](const std::string& typeName) {
            IRNode* typeNode = graph->createValueNode(IROp::GetGlobal);
            typeNode->name = typeName;
            link(typeNode);
            currentSeq = typeNode;
            
            IRNode* matchNode = graph->createValueNode(IROp::MatchType);
            link(matchNode);
            matchNode->addData(valNode);
            matchNode->addData(typeNode);
            return matchNode;
        };

        IRNode* isDict = createTypeCheck("dict");
        IRNode* isInst = createTypeCheck("instance");
        IRNode* isNs = createTypeCheck("namespace_type");

        IRNode* or1 = graph->createValueNode(IROp::BitOr);
        link(or1);
        or1->addData(isDict);
        or1->addData(isInst);

        IRNode* or2 = graph->createValueNode(IROp::BitOr);
        link(or2);
        or2->addData(or1);
        or2->addData(isNs);

        IRNode* toBool = graph->createValueNode(IROp::ToBool);
        link(toBool);
        toBool->addData(or2);

        IRNode* ifNode = createCFGNode(IROp::If);
        link(ifNode);
        ifNode->addData(toBool);
        
        IRNode* ifTrue = createCFGNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = createCFGNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        failMerge->addData(ifFalse);
        
        currentSeq = ifTrue;

        for (auto& entry : dp->entries) {
            IRNode* keyNode = graph->createConstant(Value(entry.first));
            link(keyNode);
            
            IRNode* elemNode = graph->createValueNode(IROp::IndexGet);
            link(elemNode);
            elemNode->addData(valNode);
            elemNode->addData(keyNode);
            elemNode->payload1 = 1;
            elemNode->payload2 = 1; // noThrow
            currentSeq = elemNode;
            
            buildPatternMatch(entry.second.get(), elemNode, failMerge, globalMod, globalConst, isAssignment);
        }

        if (dp->rest && dp->rest->name.lexeme != "_") {
            auto* restPat = dp->rest.get();
            
            std::vector<IRNode*> keyNodes;
            for (auto& entry : dp->entries) {
                IRNode* kNode = graph->createConstant(Value(entry.first));
                link(kNode);
                keyNodes.push_back(kNode);
            }
            
            IRNode* excludeList = graph->createValueNode(IROp::BuildList);
            link(excludeList);
            for (auto* k : keyNodes) excludeList->addData(k);
            excludeList->payload1 = static_cast<uint32_t>(keyNodes.size());
            currentSeq = excludeList;
            
            IRNode* restNode = graph->createValueNode(IROp::DictRest);
            link(restNode);
            restNode->addData(valNode);
            restNode->addData(excludeList);
            currentSeq = restNode;
            
            auto it = patternSymbols->find(restPat);
            ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
            ScopeModifier mod = restPat->modifier != ScopeModifier::None ? restPat->modifier : globalMod;
            bool isExplicitConst = restPat->isConst || globalConst;
            
            if (mod == ScopeModifier::Ref && currentFunction && (sym.scope == VarScope::Upvalue || sym.scope == VarScope::CapturedState)) {
                int upvalIdx = resolveUpvalue(restPat->name.lexeme, sym.scope == VarScope::CapturedState);
                if (upvalIdx != -1) {
                    currentFunction->upvalues[upvalIdx].isRef = true;
                }
            }
            emitTypeAssert(restNode, restPat->typeHint, restPat->name.lexeme);
            assignVar(restPat->name.lexeme, restNode, sym, mod, isExplicitConst);
        }
    }
}

void IRBuilder::buildCompClause(CompExprBase* expr, size_t clauseIdx, IRNode* accNode, IROp appendOp) {
    if (clauseIdx >= expr->clauses.size()) {
        expr->valueExpr->accept(*this);
        IRNode* valNode = lastValue;
        IRNode* appendNode = graph->createNode(appendOp);
        link(appendNode);
        appendNode->addData(accNode);
        appendNode->addData(valNode);
        currentSeq = appendNode;
        return;
    }
    
    auto& clause = expr->clauses[clauseIdx];
    clause.iterable->accept(*this);
    IRNode* iterNode = graph->createValueNode(IROp::IterInit);
    link(iterNode);
    iterNode->addData(lastValue);
    
    auto isDestructPattern = [](Pattern* p) {
        while (auto* dp = dynamic_cast<DefaultPattern*>(p)) p = dp->inner.get();
        return dynamic_cast<ListPattern*>(p) != nullptr || dynamic_cast<MatrixPattern*>(p) != nullptr;
    };
    iterNode->payload1 = isDestructPattern(clause.pattern.get()) ? 1 : 0;
    currentSeq = iterNode;
    
    IRNode* loopNode = createCFGNode(IROp::Loop);
    loopNode->addData(currentSeq);
    
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
    currentSeq = loopNode;
    
    IRNode* exitControl = createCFGNode(IROp::Merge);

    IRNode* nextNode = graph->createValueNode(IROp::IterNext);
    link(nextNode);
    nextNode->addData(iterNode);
    currentSeq = nextNode;
    
    IRNode* isUninitNode = graph->createValueNode(IROp::IsUninit);
    link(isUninitNode);
    isUninitNode->addData(nextNode);
    
    IRNode* ifNode = createCFGNode(IROp::If);
    link(ifNode);
    ifNode->addData(isUninitNode);
    
    IRNode* ifTrue = createCFGNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    exitControl->addData(ifTrue);
    auto exitEnvStack = envStack;
    
    IRNode* ifFalse = createCFGNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    currentSeq = ifFalse;
    
    auto envBeforePattern = envStack;
    IRNode* failMerge = createCFGNode(IROp::Merge);
    buildPatternMatch(clause.pattern.get(), nextNode, failMerge, ScopeModifier::Local, false, true);
    
    auto envBeforeCond = envStack;
    IRNode* condFailMerge = createCFGNode(IROp::Merge);
    for (auto& cond : clause.conditions) {
        cond->accept(*this);
        IRNode* condIfNode = createCFGNode(IROp::If);
        link(condIfNode);
        condIfNode->addData(lastValue);
        
        IRNode* condIfTrue = createCFGNode(IROp::IfTrue);
        condIfTrue->setControl(condIfNode);
        
        IRNode* condIfFalse = createCFGNode(IROp::IfFalse);
        condIfFalse->setControl(condIfNode);
        condFailMerge->addData(condIfFalse);
        
        currentSeq = condIfTrue;
    }
    
    buildCompClause(expr, clauseIdx + 1, accNode, appendOp);
    
    loopNode->addData(currentSeq);
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
    currentSeq = exitControl;
}

void IRBuilder::build(Expr* ast) {
    if (isInitMethod && instancePropertiesToInit) {
        for (auto* f : *instancePropertiesToInit) {
            f->value->accept(*this);
            IRNode* valNode = lastValue;
            IRNode* selfNode = graph->createValueNode(IROp::GetSelf);
            link(selfNode);
            
            IROp op;
            if (f->isLocal && f->isConst) op = IROp::DefinePrivateConst;
            else if (f->isLocal) op = IROp::DefinePrivate;
            else if (f->isConst) op = IROp::DefinePropConst;
            else op = IROp::DefineProp;
            
            IRNode* defNode = graph->createValueNode(op);
            link(defNode);
            defNode->addData(selfNode);
            defNode->addData(valNode);
            defNode->name = f->name.lexeme;
            currentSeq = defNode;
        }
    }

    ast->accept(*this);
    
    // 自动在末尾插入 Return 节点
    IRNode* retVal = nullptr;
    if (lastValue) {
        retVal = lastValue;
    } else {
        retVal = graph->createConstant(Value::none());
        link(retVal);
    }
    
    if (currentReturnTypeHint) {
        IRNode* assertNode = graph->createNode(IROp::AssertReturnType);
        link(assertNode);
        assertNode->addData(retVal);
        currentSeq = assertNode;
    }

    if (currentSeq->op != IROp::Return && currentSeq->op != IROp::Throw && currentSeq->op != IROp::ThrowTyped) {
        IRNode* retNode = createCFGNode(IROp::Return);
        link(retNode);
        retNode->addData(retVal);
        recordExitNode(retNode);
        currentSeq = retNode;
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
                imagPart = BaseNum::fromString(numPart, base).getValue().toFloat();
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
    link(lastValue);
}

void IRBuilder::visitBinary(Binary* expr) {
    graph->currentLine = expr->op.line;
    if (expr->op.type == TokenType::PIPE) {
        expr->right->accept(*this);
        IRNode* calleeNode = lastValue;
        
        IRCallSignature sig;
        if (auto* var = dynamic_cast<Variable*>(expr->left.get())) {
            std::string name = var->name.lexeme;
            auto argVarIt = exprSymbols->find(var);
            ResolvedSym argSym = argVarIt != exprSymbols->end() ? argVarIt->second : ResolvedSym{};
            if (refParams.count(name)) {
                ResolvedSym rs; rs.scope = VarScope::RefParam;
                sig.refs.push_back({0, 4, name, -1, readVariable(name, rs), "", argSym.isConst});
            } else {
                IRNode* localNode = getLocalNode(name);
                if (localNode) {
                    capturedLocals.insert(name);
                    capturedNodesToExtend.push_back(localNode);
                    sig.refs.push_back({0, 2, name, -1, localNode, "", argSym.isConst});
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
                        sig.refs.push_back({0, 3, name, upvalIdx, nullptr, "", argSym.isConst});
                    } else {
                        sig.refs.push_back({0, 1, name, -1, nullptr, "", argSym.isConst});
                    }
                }
            }
        }
        
        expr->left->accept(*this);
        IRNode* argNode = lastValue;
        
        if (!sig.refs.empty()) {
            graph->callSignatures.push_back(sig);
            IRNode* passRefsNode = graph->createNode(IROp::PassRefs);
            link(passRefsNode);
            passRefsNode->payload1 = static_cast<uint32_t>(graph->callSignatures.size() - 1);
            for (auto& ref : sig.refs) {
                if (ref.localNode) passRefsNode->addData(ref.localNode);
            }
            currentSeq = passRefsNode;
        }
        
        IRNode* callNode = graph->createValueNode(IROp::Call);
        link(callNode);
        callNode->addData(calleeNode);
        callNode->addData(argNode);
        callNode->payload1 = 1;
        callNode->payload2 = 0;
        
        currentSeq = callNode;
        lastValue = callNode;
        return;
    }

    if (expr->op.type == TokenType::AND_AND || expr->op.type == TokenType::OR_OR) {
        expr->left->accept(*this);
        IRNode* leftVal = lastValue;
        
        IRNode* toBoolNode = graph->createValueNode(IROp::ToBool);
        link(toBoolNode);
        toBoolNode->addData(leftVal);
        
        IRNode* ifNode = createCFGNode(IROp::If);
        link(ifNode);
        ifNode->addData(toBoolNode);
        
        IRNode* ifTrue = createCFGNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = createCFGNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        
        auto baseEnv = envStack;
        
        IRNode* mergeNode = createCFGNode(IROp::Merge);
        IRNode* resultPhi = graph->createValueNode(IROp::Phi);
        resultPhi->setControl(mergeNode);
        
        if (expr->op.type == TokenType::AND_AND) {
            currentSeq = ifTrue;
            expr->right->accept(*this);
            IRNode* rightToBool = graph->createValueNode(IROp::ToBool);
            link(rightToBool);
            rightToBool->addData(lastValue);
            
            mergeNode->addData(currentSeq);
            resultPhi->addData(rightToBool);
            auto rightEnv = envStack;
            
            envStack = baseEnv;
            currentSeq = ifFalse;
            mergeNode->addData(currentSeq);
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
            currentSeq = ifTrue;
            mergeNode->addData(currentSeq);
            resultPhi->addData(toBoolNode);
            
            envStack = baseEnv;
            currentSeq = ifFalse;
            expr->right->accept(*this);
            IRNode* rightToBool = graph->createValueNode(IROp::ToBool);
            link(rightToBool);
            rightToBool->addData(lastValue);
            
            mergeNode->addData(currentSeq);
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
        
        currentSeq = mergeNode;
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
        case TokenType::TILDE_SLASH: op = IROp::IDiv; break;
        case TokenType::PERCENT: op = IROp::Mod; break;
        case TokenType::CARET: op = IROp::Pow; break;
        case TokenType::BACKSLASH: op = IROp::LeftDivide; break;
        case TokenType::EQUAL: op = IROp::Eq; break;
        case TokenType::BANG_EQUAL: op = IROp::Neq; break;
        case TokenType::LESS: op = IROp::Lt; break;
        case TokenType::LESS_EQUAL: op = IROp::Le; break;
        case TokenType::GREATER: op = IROp::Gt; break;
        case TokenType::GREATER_EQUAL: op = IROp::Ge; break;
        case TokenType::CMP: op = IROp::Cmp; break;
        case TokenType::IS: op = IROp::Is; break;
        case TokenType::SUBSET: op = IROp::IsSubset; break;
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
    link(node);
    node->addData(leftVal);
    node->addData(rightVal);
    lastValue = node;
}

void IRBuilder::visitVariable(Variable* expr) {
    if (!allowInternalNames && isReservedInternalName(expr->name.lexeme)) {
        error(expr->name.line, "Compile Error: Illegal use of reserved internal name '" + expr->name.lexeme + "'.");
    }
    graph->currentLine = expr->name.line;
    auto it = exprSymbols->find(expr);
    ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
    // ★ 诊断：引用出现在其声明的初始化语句之前（语义不变，仅报错更达意）
    if (sym.declaredLater) {
        error(expr->name.line, "'" + expr->name.lexeme + "' is used before its declaration is initialized.");
    }
    lastValue = readVariable(expr->name.lexeme, sym);
}

void IRBuilder::visitTypeAssertExpr(TypeAssertExpr* expr) {
    expr->value->accept(*this);
    IRNode* valueNode = lastValue;

    expr->typeHint->accept(*this);
    IRNode* typeNode = lastValue;

    IRNode* assertNode = graph->createNode(IROp::AssertType);
    link(assertNode);
    assertNode->addData(valueNode);
    assertNode->addData(typeNode);
    assertNode->name = expr->name.lexeme;
    currentSeq = assertNode;

    lastValue = valueNode;  // 断言通过后，表达式值 = x
}

void IRBuilder::visitAssign(Assign* expr) {
    if (!allowInternalNames && isReservedInternalName(expr->name.lexeme)) {
        error(expr->name.line, "Compile Error: Illegal use of reserved internal name '" + expr->name.lexeme + "'.");
    }
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
        // ★ state 永远向上绑定到闭包，闭包外没有可绑定的状态槽。
        //   顶层 `state y = 2` 曾一路走到 readVariable，那里对
        //   currentFunction->upvalues 解引用 → 空指针（0xC0000005）。
        //   在 -e 模式下更是静默无输出，只有崩溃码。
        //   与 visitStateDecl / visitAssign 的同名检查保持同一措辞。
        if (!currentFunction) error(expr->name.line, "Syntax Error: 'state' modifier cannot be used at the top level.");
        IRNode* getVal = readVariable(expr->name.lexeme, sym);
        IRNode* isUninit = graph->createValueNode(IROp::IsUninit);
        isUninit->addData(getVal);
        link(isUninit);

        IRNode* ifNode = createCFGNode(IROp::If);
        ifNode->addData(isUninit);
        link(ifNode);

        IRNode* ifTrue = createCFGNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);

        IRNode* ifFalse = createCFGNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);

        auto baseEnv = envStack;

        currentSeq = ifTrue;
        
        expr->value->accept(*this);
        IRNode* valNode = lastValue;
        
        if (hidden) envStack[hiddenDepth][expr->name.lexeme] = hiddenNode;
        
        writeVariable(expr->name.lexeme, valNode, sym, false);
        IRNode* trueCtrl = currentSeq;
        auto trueEnv = envStack;

        currentSeq = ifFalse;
        auto falseEnv = baseEnv;

        IRNode* merge = createCFGNode(IROp::Merge);
        merge->addData(trueCtrl);
        merge->addData(ifFalse);
        
        envStack = baseEnv;
        for (size_t i = 0; i < baseEnv.size(); ++i) {
            std::unordered_set<std::string> modifiedVars;
            for (const auto& pair : trueEnv[i]) {
                if (baseEnv[i].count(pair.first) && baseEnv[i].at(pair.first) != pair.second) modifiedVars.insert(pair.first);
            }
            for (const auto& nameVar : modifiedVars) {
                IRNode* tNode = trueEnv[i].count(nameVar) ? trueEnv[i].at(nameVar) : baseEnv[i].at(nameVar);
                IRNode* fNode = falseEnv[i].count(nameVar) ? falseEnv[i].at(nameVar) : baseEnv[i].at(nameVar);
                if (tNode != fNode) {
                    IRNode* phi = graph->createValueNode(IROp::Phi);
                    phi->setControl(merge);
                    phi->addData(tNode);
                    phi->addData(fNode);
                    phi->name = nameVar;
                    envStack[i][nameVar] = phi;
                }
            }
        }
        
        currentSeq = merge;

        IRNode* phiVal = graph->createValueNode(IROp::Phi);
        phiVal->setControl(merge);
        phiVal->addData(valNode);
        IRNode* noneNode = graph->createConstant(Value::none());
        noneNode->setControl(merge);
        phiVal->addData(noneNode);
        lastValue = phiVal;
        return;
    }

    if (auto* named = dynamic_cast<NamespaceDecl*>(expr->value.get())) {
        named->isLocalBinding = expr->isLocal;
    }
    expr->value->accept(*this);
    IRNode* valNode = lastValue;

    if (sym.scope == VarScope::Global && dynamic_cast<EnumDefExpr*>(expr->value.get())
        && valNode->op == IROp::Constant && valNode->constVal.isObjType(ObjType::NAMESPACE)) {
        auto* enumNs = static_cast<ObjNamespace*>(valNode->constVal.asObj());
        auto& fields = globalEnumFields[expr->name.lexeme];
        for (const auto& [fieldName, field] : enumNs->fields) {
            if (field.isConst && field.upval) fields[fieldName] = field.upval->closed;
        }
    }
    
    // ★ 匿名 class / trait / namespace 一律不继承变量名。
    //   曾在此把赋值目标名写进 Class / BuildNamespace 节点，于是 A = class {}
    //   打印 <class A>、换绑给别的变量就改名（b = a 仍显示 <class a>）——
    //   名字成了绑定的属性，而不是对象自身的属性。
    //   匿名就该保持空名：Parser 给的本来就是空名，打印层也有
    //   name.empty() → <anonymous class> / <anonymous namespace> 的分支，
    //   与匿名 enum 的既有做法一致。具名形式（class Name {}）由 Parser 显式赋名，
    //   不经过这里。
    
    if (expr->typeHint) {
        expr->typeHint->accept(*this);
        IRNode* typeNode = lastValue;
        
        IRNode* assertNode = graph->createNode(IROp::AssertType);
        link(assertNode);
        assertNode->addData(valNode);
        assertNode->addData(typeNode);
        assertNode->name = expr->name.lexeme;
        currentSeq = assertNode;
    }
    
    if (hidden) envStack[hiddenDepth][expr->name.lexeme] = hiddenNode;
    
    writeVariable(expr->name.lexeme, valNode, sym, expr->isLocal, expr->isConst);
    if (expr->isRef && sym.scope == VarScope::Global && currentSeq->op == IROp::SetGlobal) {
        currentSeq->op = IROp::SetGlobalRef;
    }
    lastValue = valNode;
}

void IRBuilder::hoistBlock(Block* block) {
    auto hoistVar = [&](const std::string& name, bool isExplicitLocal) {
        if (name == "_") return;
        if (isExplicitLocal) {
            if (!envStack.back().count(name)) {
                IRNode* uninit = graph->createConstant(Value::uninit());
                link(uninit);
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
                link(uninit);
                if (namespaceScopeDepth != -1) envStack[namespaceScopeDepth][name] = uninit;
                else envStack[0][name] = uninit;
            }
        }
    };

    // ★ 递归下探控制流体内的语句。裸赋值（auto-local）按语义属于【函数级】
    //   （documentation.json 的 Variable Routing：ordinary assignments inside blocks
    //   bypass the block and attach to the function/global scope）。若只在块自己那层
    //   hoist，绑定会落在分支帧里；而 visitIfExpr 在分支前就把 baseEnv 拷走、分支后
    //   `envStack = baseEnv` 覆盖回去 ⇒ 绑定丢失、汇合看不到它 ⇒ 不生成 Phi，
    //   分支外的读取退化成 <uninit> 并被常量折叠掉整个函数。见 local/FINDINGS.md F14。
    std::function<void(Expr*)> hoistStmt;
    std::function<void(Expr*)> hoistExpr;

    // 普通表达式里也可能藏 auto-local 赋值：`(x = 1)`、`f(x = 1)`、
    // `return (x = 1)`。hoist 必须递归到这些子表达式；Lambda/Namespace/
    // Class/Enum/Quote 是独立作用域或编译期 AST，遇到即停。
    hoistExpr = [&](Expr* e) {
        if (!e) return;

        if (auto* g = dynamic_cast<GroupingExpr*>(e)) { hoistExpr(g->expression.get()); return; }
        if (auto* seq = dynamic_cast<SequenceExpr*>(e)) {
            for (auto& x : seq->expressions) hoistExpr(x.get());
            return;
        }
        if (auto* b = dynamic_cast<Binary*>(e)) { hoistExpr(b->left.get()); hoistExpr(b->right.get()); return; }
        if (auto* u = dynamic_cast<Unary*>(e)) { hoistExpr(u->right.get()); return; }
        if (auto* ta = dynamic_cast<TypeAssertExpr*>(e)) {
            hoistExpr(ta->value.get());
            if (ta->typeHint) hoistExpr(ta->typeHint.get());
            return;
        }
        if (auto* c = dynamic_cast<Call*>(e)) {
            for (auto& a : c->arguments) hoistExpr(a.get());
            return;
        }
        if (auto* inv = dynamic_cast<InvokeExpr*>(e)) {
            hoistExpr(inv->callee.get());
            for (auto& a : inv->arguments) hoistExpr(a.get());
            return;
        }
        if (auto* mc = dynamic_cast<MethodCallExpr*>(e)) {
            hoistExpr(mc->object.get());
            for (auto& a : mc->arguments) hoistExpr(a.get());
            return;
        }
        if (auto* ia = dynamic_cast<IndexAccess*>(e)) {
            hoistExpr(ia->object.get());
            for (auto& i : ia->indices) hoistExpr(i.get());
            return;
        }
        if (auto* ias = dynamic_cast<IndexAssign*>(e)) {
            if (ias->objectExpr) hoistExpr(ias->objectExpr.get());
            for (auto& chain : ias->indexChain) for (auto& i : chain) hoistExpr(i.get());
            hoistExpr(ias->value.get());
            if (ias->typeHint) hoistExpr(ias->typeHint.get());
            return;
        }
        if (auto* da = dynamic_cast<DotAccess*>(e)) { hoistExpr(da->object.get()); return; }
        if (auto* das = dynamic_cast<DotAssign*>(e)) {
            hoistExpr(das->object.get());
            hoistExpr(das->value.get());
            if (das->typeHint) hoistExpr(das->typeHint.get());
            return;
        }
        if (auto* ca = dynamic_cast<CompoundAssign*>(e)) {
            hoistExpr(ca->target.get());
            hoistExpr(ca->value.get());
            return;
        }
        if (auto* ea = dynamic_cast<ExprAssign*>(e)) {
            hoistExpr(ea->target.get());
            hoistExpr(ea->value.get());
            return;
        }
        if (auto* comp = dynamic_cast<CompExprBase*>(e)) {
            hoistExpr(comp->valueExpr.get());
            for (auto& clause : comp->clauses) {
                hoistExpr(clause.iterable.get());
                for (auto& cond : clause.conditions) hoistExpr(cond.get());
            }
            return;
        }
        if (auto* sc = dynamic_cast<SetCompExpr*>(e)) {
            hoistExpr(sc->valueExpr.get());
            for (auto& clause : sc->clauses) {
                hoistExpr(clause.iterable.get());
                for (auto& cond : clause.conditions) hoistExpr(cond.get());
            }
            return;
        }
        if (auto* dc = dynamic_cast<DictCompExpr*>(e)) {
            hoistExpr(dc->keyExpr.get());
            hoistExpr(dc->valueExpr.get());
            for (auto& clause : dc->clauses) {
                hoistExpr(clause.iterable.get());
                for (auto& cond : clause.conditions) hoistExpr(cond.get());
            }
            return;
        }
        if (auto* ln = dynamic_cast<ListNode*>(e)) {
            for (auto& row : ln->elements) for (auto& x : row) hoistExpr(x.get());
            return;
        }
        if (auto* mn = dynamic_cast<MatrixNode*>(e)) {
            for (auto& row : mn->elements) for (auto& x : row) hoistExpr(x.get());
            return;
        }
        if (auto* dl = dynamic_cast<DictLiteral*>(e)) {
            for (auto& [k, v] : dl->entries) { hoistExpr(k.get()); hoistExpr(v.get()); }
            return;
        }
        if (auto* sl = dynamic_cast<SetLiteral*>(e)) {
            for (auto& x : sl->elements) hoistExpr(x.get());
            return;
        }
        if (auto* slice = dynamic_cast<SliceExpr*>(e)) {
            hoistExpr(slice->start.get());
            hoistExpr(slice->end.get());
            hoistExpr(slice->step.get());
            return;
        }
        if (auto* fs = dynamic_cast<FStringExpr*>(e)) {
            for (auto& x : fs->exprs) hoistExpr(x.get());
            return;
        }
        if (auto* ret = dynamic_cast<ReturnExpr*>(e)) { hoistExpr(ret->value.get()); return; }
        if (auto* th = dynamic_cast<ThrowExpr*>(e)) { hoistExpr(th->value.get()); return; }
        if (auto* df = dynamic_cast<DeferExpr*>(e)) { hoistExpr(df->body.get()); return; }
        if (auto* kw = dynamic_cast<KeywordArgExpr*>(e)) { hoistExpr(kw->value.get()); return; }
        if (auto* sp = dynamic_cast<SpreadExpr*>(e)) { hoistExpr(sp->value.get()); return; }
        if (auto* imp = dynamic_cast<ImportExpr*>(e)) { hoistExpr(imp->path.get()); return; }
        if (auto* inc = dynamic_cast<IncludeExpr*>(e)) { hoistExpr(inc->value.get()); return; }
        if (auto* mcall = dynamic_cast<MacroCallExpr*>(e)) {
            for (auto& a : mcall->arguments) hoistExpr(a.get());
            return;
        }

        // 语句/控制流由 hoistStmt 处理；赋值与声明也在这里集中处理。
        if (dynamic_cast<Block*>(e) || dynamic_cast<IfExpr*>(e) ||
            dynamic_cast<WhileExpr*>(e) || dynamic_cast<ForExpr*>(e) ||
            dynamic_cast<ForInExpr*>(e) || dynamic_cast<MatchExpr*>(e) ||
            dynamic_cast<TryCatchExpr*>(e)) {
            hoistStmt(e);
            return;
        }

        if (auto* a = dynamic_cast<Assign*>(e)) {
            if (!a->isRef && !a->isState) {
                auto it = exprSymbols->find(a);
                ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
                if (sym.scope != VarScope::Global && !a->isLocal) hoistVar(a->name.lexeme, false);
            }
            hoistExpr(a->value.get());
            if (a->typeHint) hoistExpr(a->typeHint.get());
            return;
        }
        if (auto* ld = dynamic_cast<LocalDecl*>(e)) {
            // explicit local stays in its block scope
            if (ld->typeHint) hoistExpr(ld->typeHint.get());
            return;
        }
        if (dynamic_cast<ConstDecl*>(e)) return;
        if (auto* rd = dynamic_cast<RefDecl*>(e)) { if (rd->typeHint) hoistExpr(rd->typeHint.get()); return; }
        if (auto* sd = dynamic_cast<StateDecl*>(e)) { if (sd->typeHint) hoistExpr(sd->typeHint.get()); return; }
        if (auto* da = dynamic_cast<DestructAssign*>(e)) {
            if (!da->isRef && !da->isState) {
                std::vector<std::tuple<std::string, ScopeModifier, bool, Pattern*>> boundVars;
                collectPatternVars(da->pattern.get(), boundVars);
                for (const auto& varTuple : boundVars) {
                    const std::string& name = std::get<0>(varTuple);
                    ScopeModifier mod = std::get<1>(varTuple);
                    Pattern* p = std::get<3>(varTuple);
                    if (mod != ScopeModifier::Ref && mod != ScopeModifier::State) {
                        auto it = patternSymbols->find(p);
                        ResolvedSym sym = it != patternSymbols->end() ? it->second : ResolvedSym{};
                        if (sym.scope != VarScope::Global && mod != ScopeModifier::Local && !da->isLocal) hoistVar(name, false);
                    }
                }
            }
            hoistExpr(da->value.get());
            return;
        }
        // Lambda / Namespace / Class / Enum / Quote / Unquote: leave untouched.
    };

    hoistStmt = [&](Expr* e) {
        if (!e) return;
        if (auto* blk = dynamic_cast<Block*>(e)) {
            for (auto& st : blk->statements) hoistStmt(st.get());
            return;
        }
        if (auto* i = dynamic_cast<IfExpr*>(e)) {
            hoistExpr(i->condition.get());
            hoistStmt(i->thenBranch.get());
            hoistStmt(i->elseBranch.get());
            return;
        }
        if (auto* w = dynamic_cast<WhileExpr*>(e)) {
            hoistExpr(w->condition.get());
            hoistStmt(w->body.get());
            return;
        }
        if (auto* f = dynamic_cast<ForExpr*>(e)) {
            hoistExpr(f->initializer.get());
            hoistExpr(f->condition.get());
            hoistExpr(f->update.get());
            hoistStmt(f->body.get());
            return;
        }
        if (auto* fi = dynamic_cast<ForInExpr*>(e)) {
            hoistExpr(fi->iterable.get());
            hoistStmt(fi->body.get());
            return;
        }
        if (auto* m = dynamic_cast<MatchExpr*>(e)) {
            hoistExpr(m->subject.get());
            for (auto& c : m->branches) {
                hoistExpr(c.guard.get());
                hoistStmt(c.body.get());
            }
            return;
        }
        if (auto* tc = dynamic_cast<TryCatchExpr*>(e)) {
            hoistStmt(tc->tryBody.get());
            for (auto& c : tc->catchBranches) {
                hoistExpr(c.guard.get());
                hoistStmt(c.body.get());
            }
            return;
        }
        hoistExpr(e);
    };

    for (auto& stmt : block->statements) hoistStmt(stmt.get());

    for (auto& stmt : block->statements) hoistStmt(stmt.get());
}

void IRBuilder::visitBlock(Block* expr) {
    pushScope(); // 进入新作用域
    hoistBlock(expr);
    if (expr->statements.empty()) {
        lastValue = graph->createConstant(Value::none());
        link(lastValue);
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
    link(node);
    node->addData(rightVal);
    lastValue = node;
}

void IRBuilder::visitCall(Call* expr) {
    // 本次调用是否处于尾位置（由 visitReturnExpr 设置）；立刻清掉，避免嵌套调用误判。
    bool isTailCall = tailCallPosition;
    tailCallPosition = false;
    graph->currentLine = expr->callee.line;
    std::string calleeName = expr->callee.lexeme;
    auto it = exprSymbols->find(expr);
    ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
    // ★ 诊断：被调函数声明于本引用之后（local 前向引用）——语义不变，只把报错说清楚
    if (sym.declaredLater) {
        error(expr->callee.line, "'" + calleeName + "' is used before its declaration is initialized.");
    }
    IRNode* calleeNode = readVariable(calleeName, sym);
    
    std::vector<IRNode*> argNodes;
    IRCallSignature sig;
    
    int kwArgc = 0;
    for (size_t i = 0; i < expr->arguments.size(); ++i) {
        auto& arg = expr->arguments[i];
        std::string kwName = "";
        Expr* actualArg = arg.get();
        
        if (auto* kwArg = dynamic_cast<KeywordArgExpr*>(arg.get())) {
            kwName = kwArg->name.lexeme;
            actualArg = kwArg->value.get();
            
            IRNode* nameNode = graph->createConstant(Value(kwName));
            link(nameNode);
            argNodes.push_back(nameNode);
            kwArgc++;
        }
        else if (auto* spread = dynamic_cast<SpreadExpr*>(arg.get())) {
            if (spread->isKeyword) {
                IRNode* noneNode = graph->createConstant(Value::none());
                link(noneNode);
                IRNode* markNode = graph->createValueNode(IROp::MakeSpread);
                link(markNode);
                markNode->addData(noneNode);
                markNode->payload1 = 1;
                argNodes.push_back(markNode);
                currentSeq = markNode;
                kwArgc++;
                actualArg = spread->value.get();
            }
        }
        
        if (auto* var = dynamic_cast<Variable*>(actualArg)) {
            std::string name = var->name.lexeme;
            auto argVarIt = exprSymbols->find(var);
            ResolvedSym argSym = argVarIt != exprSymbols->end() ? argVarIt->second : ResolvedSym{};
            if (refParams.count(name)) {
                ResolvedSym rs; rs.scope = VarScope::RefParam;
                sig.refs.push_back({static_cast<uint8_t>(i), 4, name, -1, readVariable(name, rs), kwName, argSym.isConst});
            } else {
                IRNode* localNode = getLocalNode(name);
                if (localNode) {
                    capturedLocals.insert(name);
                    capturedNodesToExtend.push_back(localNode);
                    sig.refs.push_back({static_cast<uint8_t>(i), 2, name, -1, localNode, kwName, argSym.isConst});
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
                        sig.refs.push_back({static_cast<uint8_t>(i), 3, name, upvalIdx, nullptr, kwName, argSym.isConst});
                    } else {
                        sig.refs.push_back({static_cast<uint8_t>(i), 1, name, -1, nullptr, kwName, argSym.isConst});
                    }
                }
            }
        }
        actualArg->accept(*this);
        argNodes.push_back(lastValue);
    }
    
    if (!sig.refs.empty()) {
        graph->callSignatures.push_back(sig);
        IRNode* passRefsNode = graph->createNode(IROp::PassRefs);
        link(passRefsNode);
        passRefsNode->payload1 = static_cast<uint32_t>(graph->callSignatures.size() - 1);
        for (auto& ref : sig.refs) {
            if (ref.localNode) passRefsNode->addData(ref.localNode);
        }
        currentSeq = passRefsNode;
    }
    
    IRNode* callNode = graph->createValueNode(IROp::Call);
    link(callNode);
    callNode->addData(calleeNode);
    for (IRNode* arg : argNodes) {
        callNode->addData(arg);
    }
    callNode->payload1 = static_cast<uint32_t>(argNodes.size());
    callNode->payload2 = static_cast<uint32_t>(kwArgc);
    if (isTailCall) callNode->op = IROp::TailCall;

    currentSeq = callNode;
    lastValue = callNode;
}

void IRBuilder::visitIfExpr(IfExpr* expr) {
    // 1. 编译条件
    expr->condition->accept(*this);
    IRNode* condVal = lastValue;

    // 2. 创建 If 控制节点
    IRNode* ifNode = createCFGNode(IROp::If);
    link(ifNode);
    ifNode->addData(condVal);

    IRNode* ifTrue = createCFGNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);

    IRNode* ifFalse = createCFGNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);

    // 3. 保存当前环境快照
    auto baseEnv = envStack;

    // 4. 编译 True 分支
    currentSeq = ifTrue;
    pushScope(); // ★ 自动创建块级作用域
    expr->thenBranch->accept(*this);
    IRNode* thenControl = currentSeq;
    IRNode* thenVal = lastValue;
    auto thenEnv = envStack;
    popScope();

    // 5. 编译 False 分支
    envStack = baseEnv;
    currentSeq = ifFalse;
    pushScope(); // ★ 自动创建块级作用域
    IRNode* elseVal = nullptr;
    if (expr->elseBranch) {
        expr->elseBranch->accept(*this);
        elseVal = lastValue;
    } else {
        elseVal = graph->createConstant(Value::none());
        link(elseVal);
    }
    IRNode* elseControl = currentSeq;
    auto elseEnv = envStack;
    popScope();
    envStack = baseEnv;

    // 6. 创建 Merge 节点汇合控制流
    IRNode* mergeNode = createCFGNode(IROp::Merge);
    mergeNode->addData(thenControl);
    mergeNode->addData(elseControl);
    currentSeq = mergeNode;

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
        // ★ 尾调用：`return f(args)` 且不在 try 内、没有返回类型断言时，把这次调用标成
        //   TailCall。VM 的解释器路径会复用当前帧（真正的 TCO）；JIT 把 TAIL_CALL 当普通
        //   调用处理，随后的 RETURN 仍然在（本函数照常生成），因此两条执行路径语义一致。
        if (activeTryCount == 0 && !currentReturnTypeHint &&
            dynamic_cast<Call*>(expr->value.get())) {
            tailCallPosition = true;
        }
        expr->value->accept(*this);
        retVal = lastValue;
        tailCallPosition = false;
    } else {
        retVal = graph->createConstant(Value::none());
        link(retVal);
    }
    
    graph->currentLine = expr->keyword.line;
    
    if (currentReturnTypeHint) {
        IRNode* assertNode = graph->createNode(IROp::AssertReturnType);
        link(assertNode);
        assertNode->addData(retVal);
        currentSeq = assertNode;
    }

    IRNode* retNode = createCFGNode(IROp::Return);
    link(retNode);
    retNode->addData(retVal);
    
    recordExitNode(retNode);
    
    currentSeq = retNode;
    lastValue = retVal;
}

void IRBuilder::visitMatrixNode(MatrixNode* expr) {
    int rows = static_cast<int>(expr->elements.size());
    if (rows == 0) {
        lastValue = graph->createConstant(Value(RealMatrix(0, 0)));
        link(lastValue);
        return;
    }

    std::vector<IRNode*> elements;
    for (auto& row : expr->elements) {
        for (auto& e : row) {
            e->accept(*this);
            elements.push_back(lastValue);
        }
    }
    IRNode* node = graph->createValueNode(IROp::BuildMatrix);
    link(node);
    for (auto* e : elements) node->addData(e);
    node->payload1 = static_cast<uint32_t>(expr->elements.size());
    for (auto& row : expr->elements) node->rowCols.push_back(static_cast<uint32_t>(row.size()));
    currentSeq = node;
    lastValue = node;
}

void IRBuilder::visitListNode(ListNode* expr) {
    int rows = static_cast<int>(expr->elements.size());
    if (rows == 0) {
        IRNode* node = graph->createValueNode(IROp::ListInit);
        link(node);
        currentSeq = node;
        lastValue = node;
        return;
    }

    if (rows == 1) {
        std::vector<IRNode*> elements;
        for (auto& e : expr->elements[0]) {
            e->accept(*this);
            elements.push_back(lastValue);
        }
        IRNode* node = graph->createValueNode(IROp::BuildList);
        link(node);
        for (auto* e : elements) node->addData(e);
        node->payload1 = static_cast<uint32_t>(elements.size());
        currentSeq = node;
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
            link(rowNode);
            for (auto* e : elements) rowNode->addData(e);
            rowNode->payload1 = static_cast<uint32_t>(elements.size());
            currentSeq = rowNode;
            rowNodes.push_back(rowNode);
        }
        IRNode* node = graph->createValueNode(IROp::BuildList);
        link(node);
        for (auto* r : rowNodes) node->addData(r);
        node->payload1 = static_cast<uint32_t>(rowNodes.size());
        currentSeq = node;
        lastValue = node;
    }
}

void IRBuilder::visitWhileExpr(WhileExpr* expr) {
    pushScope(); // ★ 自动创建块级作用域
    
    IRNode* loopNode = createCFGNode(IROp::Loop);
    loopNode->addData(currentSeq);
    
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
    currentSeq = loopNode;
    
    IRNode* breakMerge = createCFGNode(IROp::Merge);
    IRNode* continueMerge = createCFGNode(IROp::Merge);
    loopStack.push_back({loopNode, loopPhisStack, breakMerge, {}, continueMerge, {}, activeTryCount});
    
    expr->condition->accept(*this);
    IRNode* condVal = lastValue;
    
    IRNode* ifNode = createCFGNode(IROp::If);
    link(ifNode);
    ifNode->addData(condVal);
    
    IRNode* ifTrue = createCFGNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    
    IRNode* ifFalse = createCFGNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    
    auto envBeforeBody = envStack;
    
    currentSeq = ifTrue;
    expr->body->accept(*this);
    
    continueMerge->addData(currentSeq);
    loopStack.back().continueEnvs.push_back(envStack);
    
    currentSeq = continueMerge;
    
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
    
    loopNode->addData(currentSeq);
    for (size_t i = 0; i < loopPhisStack.size(); ++i) {
        for (auto& pair : loopPhisStack[i]) {
            ResolvedSym rs; rs.scope = VarScope::Local;
            pair.second->addData(readVariable(pair.first, rs));
        }
    }
    
    breakMerge->addData(ifFalse);
    loopStack.back().breakEnvs.push_back(envBeforeBody);
    
    currentSeq = breakMerge;
    
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
    link(lastValue);
}

void IRBuilder::visitForExpr(ForExpr* expr) {
    pushScope(); // ★ 自动创建块级作用域
    expr->initializer->accept(*this);
    
    IRNode* loopNode = createCFGNode(IROp::Loop);
    loopNode->addData(currentSeq);
    
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
    currentSeq = loopNode;
    
    IRNode* breakMerge = createCFGNode(IROp::Merge);
    IRNode* continueMerge = createCFGNode(IROp::Merge);
    loopStack.push_back({loopNode, loopPhisStack, breakMerge, {}, continueMerge, {}, activeTryCount});
    
    expr->condition->accept(*this);
    IRNode* condVal = lastValue;
    
    IRNode* ifNode = createCFGNode(IROp::If);
    link(ifNode);
    ifNode->addData(condVal);
    
    IRNode* ifTrue = createCFGNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    
    IRNode* ifFalse = createCFGNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    
    auto envBeforeBody = envStack;
    
    currentSeq = ifTrue;
    expr->body->accept(*this);
    
    continueMerge->addData(currentSeq);
    loopStack.back().continueEnvs.push_back(envStack);
    
    currentSeq = continueMerge;
    
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
    
    loopNode->addData(currentSeq);
    for (size_t i = 0; i < loopPhisStack.size(); ++i) {
        for (auto& pair : loopPhisStack[i]) {
            ResolvedSym rs; rs.scope = VarScope::Local;
            pair.second->addData(readVariable(pair.first, rs));
        }
    }
    
    breakMerge->addData(ifFalse);
    loopStack.back().breakEnvs.push_back(envBeforeBody);
    
    currentSeq = breakMerge;
    
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
    link(lastValue);
}

void IRBuilder::visitBreakExpr(BreakExpr* expr) {
    graph->currentLine = expr->keyword.line;
    if (loopStack.empty()) error("Syntax Error: 'break' outside loop.");
    auto& loop = loopStack.back();
    
    int loopDepth = static_cast<int>(loop.loopPhisStack.size());
    int triesToPop = activeTryCount - loop.tryDepthAtLoopStart;
    for (int i = 0; i < triesToPop; ++i) {
        IRNode* tryEnd = createCFGNode(IROp::TryEnd);
        link(tryEnd);
        currentSeq = tryEnd;
    }

    int defersToRun = 0;
    for (size_t i = loopDepth - 1; i < deferCounts.size(); ++i) {
        defersToRun += deferCounts[i];
    }
    if (defersToRun > 0) {
        IRNode* runNode = graph->createNode(IROp::RunDefers);
        runNode->payload1 = defersToRun;
        link(runNode);
        currentSeq = runNode;
    }
    
    loop.breakMerge->addData(currentSeq);
    loop.breakEnvs.push_back(envStack);
    
    currentSeq = createCFGNode(IROp::Merge); // Dead code
}

void IRBuilder::visitContinueExpr(ContinueExpr* expr) {
    graph->currentLine = expr->keyword.line;
    if (loopStack.empty()) error("Syntax Error: 'continue' outside loop.");
    auto& loop = loopStack.back();
    
    int loopDepth = static_cast<int>(loop.loopPhisStack.size());
    int triesToPop = activeTryCount - loop.tryDepthAtLoopStart;
    for (int i = 0; i < triesToPop; ++i) {
        IRNode* tryEnd = createCFGNode(IROp::TryEnd);
        link(tryEnd);
        currentSeq = tryEnd;
    }

    int defersToRun = 0;
    for (size_t i = loopDepth; i < deferCounts.size(); ++i) {
        defersToRun += deferCounts[i];
    }
    if (defersToRun > 0) {
        IRNode* runNode = graph->createNode(IROp::RunDefers);
        runNode->payload1 = defersToRun;
        link(runNode);
        currentSeq = runNode;
    }
    
    if (loop.continueMerge) {
        loop.continueMerge->addData(currentSeq);
        loop.continueEnvs.push_back(envStack);
    } else {
        loop.loopNode->addData(currentSeq);
        for (size_t i = 0; i < loop.loopPhisStack.size(); ++i) {
            for (auto& pair : loop.loopPhisStack[i]) {
                ResolvedSym rs; rs.scope = VarScope::Local;
                pair.second->addData(readVariable(pair.first, rs));
            }
        }
    }
    
    currentSeq = createCFGNode(IROp::Merge); // Dead code
}
    
void IRBuilder::visitIndexAccess(IndexAccess* expr) {
    expr->object->accept(*this);
    IRNode* objNode = lastValue;

    std::vector<IRNode*> indices;
    for (auto& idx : expr->indices) {
        idx->accept(*this);
        indices.push_back(lastValue);
    }
        
    IRNode* node = graph->createValueNode(IROp::IndexGet);
    link(node);
    node->addData(objNode);
    for (auto* idx : indices) node->addData(idx);
    node->payload1 = static_cast<uint32_t>(indices.size());
    currentSeq = node;
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
            link(getProp);
            getProp->addData(dotParentNode);
            getProp->name = dotPropName;
            currentSeq = getProp;
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

    expr->value->accept(*this);
    IRNode* valNode = lastValue;

    // ★ 匿名 class / trait / namespace 一律不继承变量名，理由见 visitVarDecl 处的说明。

    std::vector<std::vector<IRNode*>> indicesTmp(expr->indexChain.size());
    for (size_t i = 0; i < expr->indexChain.size(); ++i) {
        for (size_t j = 0; j < expr->indexChain[i].size(); ++j) {
            expr->indexChain[i][j]->accept(*this);
            indicesTmp[i].push_back(lastValue);
        }
    }

    int depth = static_cast<int>(expr->indexChain.size());
    if (depth == 1) {
        if (dotParentNode) {
            // 字段索引赋值：GetProperty 取字段 + IndexSet 原地改（引用类型原地改，矩阵报错）
            IRNode* getProp = graph->createValueNode(IROp::GetProperty);
            link(getProp);
            getProp->addData(dotParentNode);
            getProp->name = dotPropName;
            currentSeq = getProp;
            
            IRNode* node = graph->createValueNode(IROp::IndexSet);
            link(node);
            node->addData(getProp);
            for (auto* idx : indicesTmp[0]) node->addData(idx);
            node->addData(valNode);
            node->payload1 = static_cast<uint32_t>(indicesTmp[0].size());
            currentSeq = node;
        } else {
            IRNode* node = graph->createValueNode(IROp::IndexSet);
            link(node);
            node->addData(rootObjNode);
            for (auto* idx : indicesTmp[0]) node->addData(idx);
            node->addData(valNode);
            node->payload1 = static_cast<uint32_t>(indicesTmp[0].size());
            currentSeq = node;
        }
        lastValue = valNode;
    } else {
        std::vector<IRNode*> chainObjs;
        chainObjs.push_back(rootObjNode);
        
        for (int level = 0; level < depth - 1; ++level) {
            IRNode* getNode = graph->createValueNode(IROp::IndexGet);
            link(getNode);
            getNode->addData(chainObjs.back());
            for (auto* idx : indicesTmp[level]) getNode->addData(idx);
            getNode->payload1 = static_cast<uint32_t>(indicesTmp[level].size());
            currentSeq = getNode;
            chainObjs.push_back(getNode);
        }
        
        IRNode* setNode = graph->createValueNode(IROp::IndexSet);
        link(setNode);
        setNode->addData(chainObjs.back());
        for (auto* idx : indicesTmp[depth - 1]) setNode->addData(idx);
        setNode->addData(valNode);
        setNode->payload1 = static_cast<uint32_t>(indicesTmp[depth - 1].size());
        currentSeq = setNode;
        
        lastValue = valNode;
    }

    if (expr->typeHint) {
        expr->typeHint->accept(*this);
        IRNode* typeNode = lastValue;
        IRNode* assertNode = graph->createNode(IROp::AssertType);
        link(assertNode);
        assertNode->addData(valNode);
        assertNode->addData(typeNode);
        assertNode->name = expr->name.lexeme;
        currentSeq = assertNode;
        lastValue = valNode;
    }
}
    
void IRBuilder::visitLocalDecl(LocalDecl* expr) {
    if (!allowInternalNames && isReservedInternalName(expr->name.lexeme)) error(expr->name.line, "Compile Error: Illegal use of reserved internal name '" + expr->name.lexeme + "'.");
    graph->currentLine = expr->name.line;
    
    // 捕获外层 x（作为初值），外层符号由 Resolver 存入 exprSymbols；无外层时 readVariable 落到 GetGlobal，运行时报 undefined
    IRNode* capturedNode;
    if (expr->name.lexeme == "_") {
        capturedNode = graph->createConstant(Value::uninit());
        link(capturedNode);
    } else {
        auto it = exprSymbols->find(expr);
        ResolvedSym outerSym = it != exprSymbols->end() ? it->second : ResolvedSym{};
        capturedNode = readVariable(expr->name.lexeme, outerSym);
    }
    declareVariable(expr->name.lexeme, capturedNode);
    
    if (expr->name.lexeme != "_") {
        if (expr->typeHint) {
            expr->typeHint->accept(*this);
            IRNode* typeNode = lastValue;
            IRNode* assertNode = graph->createNode(IROp::AssertType);
            link(assertNode);
            assertNode->addData(capturedNode);
            assertNode->addData(typeNode);
            assertNode->name = expr->name.lexeme;
            currentSeq = assertNode;
        }
        currentLocalVars.insert(expr->name.lexeme);
        if (expr->isConst) currentConstVars.insert(expr->name.lexeme);
    }
    lastValue = graph->createConstant(Value::none());
    link(lastValue);
}

void IRBuilder::visitRefDecl(RefDecl* expr) {
    if (!allowInternalNames && isReservedInternalName(expr->name.lexeme)) error(expr->name.line, "Compile Error: Illegal use of reserved internal name '" + expr->name.lexeme + "'.");
    graph->currentLine = expr->name.line;
    if (expr->name.lexeme != "_") {
        auto it = exprSymbols->find(expr);
        ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
        if (currentFunction && (sym.scope == VarScope::Upvalue || sym.scope == VarScope::CapturedState)) {
            int upvalIdx = resolveUpvalue(expr->name.lexeme, sym.scope == VarScope::CapturedState);
            if (upvalIdx != -1) {
                currentFunction->upvalues[upvalIdx].isRef = true;
            }
        } else if (sym.scope == VarScope::Global) {
            IRNode* checkNode = graph->createValueNode(IROp::GetGlobal);
            checkNode->name = expr->name.lexeme;
            link(checkNode);
            currentSeq = checkNode;
        }
        
        if (expr->typeHint) {
            IRNode* outerVal = readVariable(expr->name.lexeme, sym);
            expr->typeHint->accept(*this);
            IRNode* typeNode = lastValue;
            IRNode* assertNode = graph->createNode(IROp::AssertType);
            link(assertNode);
            assertNode->addData(outerVal);
            assertNode->addData(typeNode);
            assertNode->name = expr->name.lexeme;
            currentSeq = assertNode;
        }
        
        if (expr->isConst) currentConstVars.insert(expr->name.lexeme);
    }
    lastValue = graph->createConstant(Value::none());
    link(lastValue);
}

void IRBuilder::visitStateDecl(StateDecl* expr) {
    if (!allowInternalNames && isReservedInternalName(expr->name.lexeme)) error(expr->name.line, "Compile Error: Illegal use of reserved internal name '" + expr->name.lexeme + "'.");
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
        if (expr->typeHint) {
            ResolvedSym stateSym;
            stateSym.scope = VarScope::State;
            IRNode* stateVal = readVariable(expr->name.lexeme, stateSym);
            expr->typeHint->accept(*this);
            IRNode* typeNode = lastValue;
            IRNode* assertNode = graph->createNode(IROp::AssertType);
            link(assertNode);
            assertNode->addData(stateVal);
            assertNode->addData(typeNode);
            assertNode->name = expr->name.lexeme;
            currentSeq = assertNode;
        }
        if (expr->isConst) currentConstVars.insert(expr->name.lexeme);
    }
    lastValue = graph->createConstant(Value::none());
    link(lastValue);
}
    
void IRBuilder::visitConstDecl(ConstDecl* expr) {
    if (!allowInternalNames && isReservedInternalName(expr->name.lexeme)) error(expr->name.line, "Compile Error: Illegal use of reserved internal name '" + expr->name.lexeme + "'.");
    graph->currentLine = expr->name.line;
    IRNode* uninitNode = graph->createConstant(Value::uninit());
    link(uninitNode);
    declareVariable(expr->name.lexeme, uninitNode);
    if (expr->name.lexeme != "_") {
        currentConstVars.insert(expr->name.lexeme);
    }
    lastValue = graph->createConstant(Value::none());
    link(lastValue);
}

void IRBuilder::visitDeleteExpr(DeleteExpr* expr) {
    if (!expr->names.empty()) graph->currentLine = expr->names[0].line;
    std::vector<ResolvedSym> syms;
    if (deleteSyms) {
        auto it = deleteSyms->find(expr);
        if (it != deleteSyms->end()) syms = it->second;
    }
    for (size_t i = 0; i < expr->names.size(); ++i) {
        const std::string& name = expr->names[i].lexeme;
        ResolvedSym sym = (i < syms.size()) ? syms[i] : ResolvedSym{};
        if (sym.scope == VarScope::Global) {
            IRNode* delNode = graph->createNode(IROp::DeleteGlobal);
            link(delNode);
            delNode->name = name;
            currentSeq = delNode;
        } else {
            // 局部遮蔽：编译期已在 Resolver 回落外层；这里把 envStack 的局部标记为 uninit，防闭包/后续误引用
            IRNode* uninit = graph->createConstant(Value::uninit());
            link(uninit);
            for (int j = static_cast<int>(envStack.size()) - 1; j >= 0; --j) {
                auto envIt = envStack[j].find(name);
                if (envIt != envStack[j].end()) {
                    envStack[j][name] = uninit;
                    break;
                }
            }
        }
    }
    lastValue = graph->createConstant(Value::none());
    link(lastValue);
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
            auto it = exprSymbols->find(var);
            ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
            if (currentFunction && (sym.scope == VarScope::Upvalue || sym.scope == VarScope::CapturedState)) {
                int upvalIdx = resolveUpvalue(var->name.lexeme, sym.scope == VarScope::CapturedState);
                if (upvalIdx != -1) {
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
        if (!classStack.empty()) {
            bool isSelf = dynamic_cast<SelfExpr*>(dot->object.get()) != nullptr;
            bool isClassVar = false;
            if (auto* ck = dynamic_cast<ContextKeywordExpr*>(dot->object.get())) {
                if (ck->kind == ContextKeywordExpr::Kind::Class) isClassVar = true;
            } else if (auto* objVar = dynamic_cast<Variable*>(dot->object.get())) {
                if (!classStack.back().name.empty() && objVar->name.lexeme == classStack.back().name) {
                    isClassVar = true;
                }
            }
            if ((isSelf || isClassVar) && classStack.back().privateMembers.count(propName)) {
                getOp = IROp::GetPrivate;
            }
        }

        targetVal = graph->createValueNode(getOp);
        link(targetVal);
        targetVal->addData(objNode);
        targetVal->name = propName;
        currentSeq = targetVal;
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
            if (!classStack.empty()) {
                bool isSelf = dynamic_cast<SelfExpr*>(chainDot->object.get()) != nullptr;
                bool isClassVar = false;
                if (auto* ck = dynamic_cast<ContextKeywordExpr*>(chainDot->object.get())) {
                    if (ck->kind == ContextKeywordExpr::Kind::Class) isClassVar = true;
                } else if (auto* objVar = dynamic_cast<Variable*>(chainDot->object.get())) {
                    if (!classStack.back().name.empty() && objVar->name.lexeme == classStack.back().name) {
                        isClassVar = true;
                    }
                }
                if ((isSelf || isClassVar) && classStack.back().privateMembers.count(dotPropName)) {
                    getOp = IROp::GetPrivate;
                }
            }

            IRNode* getProp = graph->createValueNode(getOp);
            link(getProp);
            getProp->addData(dotParentNode);
            getProp->name = dotPropName;
            currentSeq = getProp;
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
            link(getNode);
            getNode->addData(currObj);
            for (auto* idxNode : levelIndices) getNode->addData(idxNode);
            getNode->payload1 = static_cast<uint32_t>(levelIndices.size());
            currentSeq = getNode;
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
        case TokenType::TILDE_SLASH:
        case TokenType::TILDE_SLASH_ASSIGN: op = IROp::IDiv; break;
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
    link(opNode);
    opNode->addData(targetVal);
    opNode->addData(rightVal);

    if (auto* var = dynamic_cast<Variable*>(expr->target.get())) {
        auto it = exprSymbols->find(var);
        ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
        writeVariable(var->name.lexeme, opNode, sym, expr->isLocal, false);
        if (expr->isRef && sym.scope == VarScope::Global && currentSeq->op == IROp::SetGlobal) {
            currentSeq->op = IROp::SetGlobalRef;
        }
    } else if (dynamic_cast<DotAccess*>(expr->target.get())) {
        IROp setOp = IROp::SetProperty;
        if (!classStack.empty()) {
            auto* dotTarget = static_cast<DotAccess*>(expr->target.get());
            bool isSelf = dynamic_cast<SelfExpr*>(dotTarget->object.get()) != nullptr;
            bool isClassVar = false;
            if (auto* ck = dynamic_cast<ContextKeywordExpr*>(dotTarget->object.get())) {
                if (ck->kind == ContextKeywordExpr::Kind::Class) isClassVar = true;
            } else if (auto* objVar = dynamic_cast<Variable*>(dotTarget->object.get())) {
                if (!classStack.back().name.empty() && objVar->name.lexeme == classStack.back().name) {
                    isClassVar = true;
                }
            }
            if ((isSelf || isClassVar) && classStack.back().privateMembers.count(propName)) {
                setOp = IROp::SetPrivate;
            }
        }

        IRNode* setProp = graph->createValueNode(setOp);
        link(setProp);
        setProp->addData(objNode);
        setProp->addData(opNode);
        setProp->name = propName;
        currentSeq = setProp;
    } else if (dynamic_cast<IndexAccess*>(expr->target.get())) {
        int depth = static_cast<int>(chain.size());
        if (depth == 1) {
            IRNode* setIdx = graph->createValueNode(IROp::IndexSet);
            link(setIdx);
            setIdx->addData(objNode);
            for (auto* i : indices) setIdx->addData(i);
            setIdx->addData(opNode);
            setIdx->payload1 = static_cast<uint32_t>(indices.size());
            currentSeq = setIdx;
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
                    link(getNode);
                    getNode->addData(chainObjs.back());
                    for (auto* idxNode : levelIndices) getNode->addData(idxNode);
                    getNode->payload1 = static_cast<uint32_t>(levelIndices.size());
                    currentSeq = getNode;
                    chainObjs.push_back(getNode);
                }
            }
            
            IRNode* setNode = graph->createValueNode(IROp::IndexSet);
            link(setNode);
            setNode->addData(chainObjs.back());
            for (auto* idx : allIndices.back()) setNode->addData(idx);
            setNode->addData(opNode);
            setNode->payload1 = static_cast<uint32_t>(allIndices.back().size());
            currentSeq = setNode;
        }
    }

    lastValue = opNode;
}

void IRBuilder::buildFunctionParams(const std::vector<Token>& params, const std::vector<std::shared_ptr<Expr>>& defaultExprs, 
    const std::string& restName,
    const std::vector<bool>& paramIsRef, const std::vector<bool>& paramIsConst, const std::vector<std::shared_ptr<Expr>>& typeHints,
    const std::vector<Token>& kwargParams, const std::vector<bool>& kwargIsRef, const std::vector<bool>& kwargIsConst,
    const std::vector<std::shared_ptr<Expr>>& kwargDefaultExprs, const std::vector<std::shared_ptr<Expr>>& kwargTypes,
    const std::string& kwargsName) {
    int requiredArgs = 0;
    bool seenDefault = false;
    for (size_t i = 0; i < params.size(); ++i) {
        if (defaultExprs[i]) {
            seenDefault = true;
        } else if (seenDefault) {
            error(params[i].line, "Syntax Error: non-default argument follows default argument.");
        }
        if (!defaultExprs[i]) requiredArgs++;
    }
    currentFunction->arity = requiredArgs;
    currentFunction->maxArity = static_cast<int>(params.size());
    currentFunction->restName = restName;
    currentFunction->kwargsName = kwargsName;
    currentFunction->paramIsRef = paramIsRef;
    currentFunction->paramIsConst = paramIsConst;
    currentFunction->kwargIsRef = kwargIsRef;
    currentFunction->kwargIsConst = kwargIsConst;
    for (const auto& de : kwargDefaultExprs) currentFunction->kwargHasDefault.push_back(de != nullptr);
    for (const auto& p : params) {
        currentFunction->paramNames.push_back(p.lexeme);
    }
    for (const auto& p : kwargParams) {
        currentFunction->kwargNames.push_back(p.lexeme);
    }
    int refCount = 0;
    for (bool isRef : paramIsRef) if (isRef) refCount++;
    for (bool isRef : kwargIsRef) if (isRef) refCount++;
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
                link(paramNode);
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
            link(isUninit);
            isUninit->addData(paramNode);
            
            IRNode* ifNode = createCFGNode(IROp::If);
            link(ifNode);
            ifNode->addData(isUninit);
            
            IRNode* ifTrue = createCFGNode(IROp::IfTrue);
            ifTrue->setControl(ifNode);
            
            IRNode* ifFalse = createCFGNode(IROp::IfFalse);
            ifFalse->setControl(ifNode);
            
            auto baseEnv = envStack;
            
            currentSeq = ifTrue;
            envStack.emplace_back();
            defaultExprs[i]->accept(*this);
            IRNode* defVal = lastValue;
            IRNode* trueCtrl = currentSeq;
            auto trueEnv = envStack;
            envStack.pop_back();
            
            envStack = baseEnv;
            currentSeq = ifFalse;
            envStack.emplace_back();
            IRNode* falseCtrl = currentSeq;
            auto falseEnv = envStack;
            envStack.pop_back();
            
            envStack = baseEnv;
            
            IRNode* merge = createCFGNode(IROp::Merge);
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
            
            currentSeq = merge;
            ResolvedSym sym;
            sym.scope = paramIsRef[i] ? VarScope::RefParam : VarScope::Local;
            sym.isConst = paramIsConst[i];
            writeVariable(params[i].lexeme, phi, sym, true, paramIsConst[i]);
        }
        
        if (i < typeHints.size() && typeHints[i]) {
            IRNode* assertNode = graph->createNode(IROp::AssertParamType);
            link(assertNode);
            ResolvedSym rs; rs.scope = paramIsRef[i] ? VarScope::RefParam : VarScope::Local;
            assertNode->addData(readVariable(params[i].lexeme, rs));
            
            uint32_t paramNameIdx = currentFunction->chunk.addConstant(Value(params[i].lexeme));
            
            assertNode->payload1 = static_cast<uint32_t>(i);
            assertNode->payload2 = paramNameIdx;
            
            currentSeq = assertNode;
        }
    }

    // ★ rest 参数（位置收集，绑定到 params.size() 寄存器）
    if (!restName.empty()) {
        IRNode* restNode = graph->createValueNode(IROp::Parameter);
        restNode->payload1 = static_cast<uint32_t>(params.size());
        declareVariable(restName, restNode);
    }

    // ★ 仅关键字参数（绑定到 params.size()+1 起的寄存器）
    size_t kwBase = params.size() + (restName.empty() ? 0 : 1);
    for (size_t j = 0; j < kwargParams.size(); ++j) {
        IRNode* paramNode = nullptr;
        if (j < kwargIsRef.size() && kwargIsRef[j]) {
            if (kwargParams[j].lexeme != "_") refParams[kwargParams[j].lexeme] = refIdx;
            refIdx++;
            if (kwargParams[j].lexeme != "_") {
                ResolvedSym rs; rs.scope = VarScope::RefParam;
                paramNode = readVariable(kwargParams[j].lexeme, rs);
            } else {
                paramNode = graph->createConstant(Value::none());
                link(paramNode);
            }
        } else {
            paramNode = graph->createValueNode(IROp::Parameter);
            paramNode->payload1 = static_cast<uint32_t>(kwBase + j);
            declareVariable(kwargParams[j].lexeme, paramNode);
        }
        if (kwargParams[j].lexeme != "_" && j < kwargIsConst.size() && kwargIsConst[j]) {
            currentConstVars.insert(kwargParams[j].lexeme);
        }
        if (j < kwargDefaultExprs.size() && kwargDefaultExprs[j]) {
            IRNode* isUninit = graph->createValueNode(IROp::IsUninit);
            link(isUninit);
            isUninit->addData(paramNode);
            IRNode* ifNode = createCFGNode(IROp::If);
            link(ifNode);
            ifNode->addData(isUninit);
            IRNode* ifTrue = createCFGNode(IROp::IfTrue);
            ifTrue->setControl(ifNode);
            IRNode* ifFalse = createCFGNode(IROp::IfFalse);
            ifFalse->setControl(ifNode);
            auto baseEnv = envStack;
            currentSeq = ifTrue;
            envStack.emplace_back();
            kwargDefaultExprs[j]->accept(*this);
            IRNode* defVal = lastValue;
            IRNode* trueCtrl = currentSeq;
            auto trueEnv = envStack;
            envStack.pop_back();
            envStack = baseEnv;
            currentSeq = ifFalse;
            envStack.emplace_back();
            IRNode* falseCtrl = currentSeq;
            auto falseEnv = envStack;
            envStack.pop_back();
            envStack = baseEnv;
            IRNode* merge = createCFGNode(IROp::Merge);
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
            phi->name = kwargParams[j].lexeme;
            currentSeq = merge;
            ResolvedSym sym;
            sym.scope = VarScope::Local;
            sym.isConst = j < kwargIsConst.size() && kwargIsConst[j];
            writeVariable(kwargParams[j].lexeme, phi, sym, true, sym.isConst);
        }
        if (j < kwargTypes.size() && kwargTypes[j]) {
            IRNode* assertNode = graph->createNode(IROp::AssertParamType);
            link(assertNode);
            ResolvedSym rs; rs.scope = VarScope::Local;
            assertNode->addData(readVariable(kwargParams[j].lexeme, rs));
            uint32_t paramNameIdx = currentFunction->chunk.addConstant(Value(kwargParams[j].lexeme));
            assertNode->payload1 = static_cast<uint32_t>(kwBase + j);
            assertNode->payload2 = paramNameIdx;
            currentSeq = assertNode;
        }
    }

    // ★ kwargs 参数（关键字收集，绑定到 kwBase + kwargParams.size() 寄存器）
    if (!kwargsName.empty()) {
        IRNode* kwNode = graph->createValueNode(IROp::Parameter);
        kwNode->payload1 = static_cast<uint32_t>(kwBase + kwargParams.size());
        declareVariable(kwargsName, kwNode);
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
        IRBuilder fnBuilder(&fnGraph, compiledFunctions, this, fnDef.get(), exprSymbols, patternSymbols, deleteSyms);
        fnBuilder.allowInternalNames = this->allowInternalNames;
        
        fnBuilder.currentReturnTypeHint = expr->returnType;
        fnBuilder.buildFunctionParams(expr->params, expr->defaultExprs, expr->restName, expr->paramIsRef, expr->paramIsConst, expr->paramTypes,
            expr->kwargParams, expr->kwargIsRef, expr->kwargIsConst, expr->kwargDefaultExprs, expr->kwargTypes, expr->kwargsName);
        
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
        link(closureNode);
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
        link(closureNode);
        closureNode->name = std::to_string(expr->fnIdx);
        lastValue = closureNode;
    }
}

void IRBuilder::visitInvokeExpr(InvokeExpr* expr) {
    expr->callee->accept(*this);
    IRNode* calleeNode = lastValue;
        
    std::vector<IRNode*> argNodes;
    IRCallSignature sig;
    
    int kwArgc = 0;
    for (size_t i = 0; i < expr->arguments.size(); ++i) {
        auto& arg = expr->arguments[i];
        std::string kwName = "";
        Expr* actualArg = arg.get();
        
        if (auto* kwArg = dynamic_cast<KeywordArgExpr*>(arg.get())) {
            kwName = kwArg->name.lexeme;
            actualArg = kwArg->value.get();
            
            IRNode* nameNode = graph->createConstant(Value(kwName));
            link(nameNode);
            argNodes.push_back(nameNode);
            kwArgc++;
        }
        else if (auto* spread = dynamic_cast<SpreadExpr*>(arg.get())) {
            if (spread->isKeyword) {
                IRNode* noneNode = graph->createConstant(Value::none());
                link(noneNode);
                IRNode* markNode = graph->createValueNode(IROp::MakeSpread);
                link(markNode);
                markNode->addData(noneNode);
                markNode->payload1 = 1;
                argNodes.push_back(markNode);
                currentSeq = markNode;
                kwArgc++;
                actualArg = spread->value.get();
            }
        }
        
        if (auto* var = dynamic_cast<Variable*>(actualArg)) {
            std::string name = var->name.lexeme;
            auto argVarIt = exprSymbols->find(var);
            ResolvedSym argSym = argVarIt != exprSymbols->end() ? argVarIt->second : ResolvedSym{};
            if (refParams.count(name)) {
                ResolvedSym rs; rs.scope = VarScope::RefParam;
                sig.refs.push_back({static_cast<uint8_t>(i), 4, name, -1, readVariable(name, rs), kwName, argSym.isConst});
            } else {
                IRNode* localNode = getLocalNode(name);
                if (localNode) {
                    capturedLocals.insert(name);
                    capturedNodesToExtend.push_back(localNode);
                    sig.refs.push_back({static_cast<uint8_t>(i), 2, name, -1, localNode, kwName, argSym.isConst});
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
                        sig.refs.push_back({static_cast<uint8_t>(i), 3, name, upvalIdx, nullptr, kwName, argSym.isConst});
                    } else {
                        sig.refs.push_back({static_cast<uint8_t>(i), 1, name, -1, nullptr, kwName, argSym.isConst});
                    }
                }
            }
        }
        actualArg->accept(*this);
        argNodes.push_back(lastValue);
    }
    
    if (!sig.refs.empty()) {
        graph->callSignatures.push_back(sig);
        IRNode* passRefsNode = graph->createNode(IROp::PassRefs);
        link(passRefsNode);
        passRefsNode->payload1 = static_cast<uint32_t>(graph->callSignatures.size() - 1);
        for (auto& ref : sig.refs) {
            if (ref.localNode) passRefsNode->addData(ref.localNode);
        }
        currentSeq = passRefsNode;
    }
        
    IRNode* invokeNode = graph->createValueNode(IROp::Call);
    link(invokeNode);
    invokeNode->addData(calleeNode);
    for (auto* arg : argNodes) invokeNode->addData(arg);
    invokeNode->payload1 = static_cast<uint32_t>(argNodes.size());
    invokeNode->payload2 = static_cast<uint32_t>(kwArgc);
        
    currentSeq = invokeNode;
    lastValue = invokeNode;
}

void IRBuilder::visitForInExpr(ForInExpr* expr) {
    pushScope(); // ★ 自动创建块级作用域
    expr->iterable->accept(*this);
    IRNode* iterNode = graph->createValueNode(IROp::IterInit);
    link(iterNode);
    iterNode->addData(lastValue);
    
    auto isDestructPattern = [](Pattern* p) {
        while (auto* dp = dynamic_cast<DefaultPattern*>(p)) p = dp->inner.get();
        return dynamic_cast<ListPattern*>(p) != nullptr || dynamic_cast<MatrixPattern*>(p) != nullptr;
    };
    iterNode->payload1 = isDestructPattern(expr->pattern.get()) ? 1 : 0;
    
    currentSeq = iterNode;
    
    IRNode* loopNode = createCFGNode(IROp::Loop);
    loopNode->addData(currentSeq);
    
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
    currentSeq = loopNode;
    
    IRNode* breakMerge = createCFGNode(IROp::Merge);
    IRNode* continueMerge = createCFGNode(IROp::Merge);
    loopStack.push_back({loopNode, loopPhisStack, breakMerge, {}, continueMerge, {}, activeTryCount});
    
    IRNode* nextNode = graph->createValueNode(IROp::IterNext);
    link(nextNode);
    nextNode->addData(iterNode);
    currentSeq = nextNode;
    
    IRNode* isUninitNode = graph->createValueNode(IROp::IsUninit);
    link(isUninitNode);
    isUninitNode->addData(nextNode);
    
    IRNode* ifNode = createCFGNode(IROp::If);
    link(ifNode);
    ifNode->addData(isUninitNode);
    
    IRNode* ifTrue = createCFGNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    breakMerge->addData(ifTrue);
    loopStack.back().breakEnvs.push_back(envStack);
    
    IRNode* ifFalse = createCFGNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    currentSeq = ifFalse;
    
    auto envBeforeBody = envStack;
    IRNode* failMerge = createCFGNode(IROp::Merge);
    ScopeModifier mod = expr->isLocal ? ScopeModifier::Local : ScopeModifier::None;
    buildPatternMatch(expr->pattern.get(), nextNode, failMerge, mod, expr->isConst, true);
    
    expr->body->accept(*this);
    
    continueMerge->addData(currentSeq);
    loopStack.back().continueEnvs.push_back(envStack);
    
    currentSeq = continueMerge;
    
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
    
    loopNode->addData(currentSeq);
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
    
    currentSeq = breakMerge;
    
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
    link(lastValue);
}

void IRBuilder::visitThrowExpr(ThrowExpr* expr) {
    expr->value->accept(*this);
    graph->currentLine = expr->keyword.line;
    IRNode* throwNode = createCFGNode(IROp::Throw);
    link(throwNode);
    throwNode->addData(lastValue);
    recordExitNode(throwNode);
    currentSeq = throwNode;
    lastValue = throwNode;
}

void IRBuilder::visitTryCatchExpr(TryCatchExpr* expr) {
    IRNode* tryBegin = createCFGNode(IROp::TryBegin);
    link(tryBegin);
    // 裸 try：不需要异常对象，只需要失败后跳到 catch 块并得到 none。
    tryBegin->payload1 = expr->catchBranches.empty() ? 1 : 0;

    auto baseEnv = envStack;

    // 1. 正常分支
    IRNode* tryBodyStart = createCFGNode(IROp::Merge);
    tryBodyStart->addData(tryBegin);
    currentSeq = tryBodyStart;

    activeTryCount++;
    expr->tryBody->accept(*this);
    activeTryCount--;
    
    IRNode* tryEnd = createCFGNode(IROp::TryEnd);
    link(tryEnd);
    IRNode* normalControl = tryEnd;
    IRNode* normalVal = lastValue;
    auto normalEnv = envStack;

    // 2. 异常分支（catch 链，复用 match 的分支选择）
    envStack = baseEnv;
    
    IRNode* catchNode = graph->createValueNode(IROp::Catch);
    catchNode->setControl(tryBegin);
    currentSeq = catchNode;
    
    IRNode* errVal = catchNode; // Catch 节点产生异常对象
    IRNode* subjectNode = errVal;
    
    IRNode* endMerge = createCFGNode(IROp::Merge);
    IRNode* catchResultPhi = graph->createValueNode(IROp::Phi);
    catchResultPhi->setControl(endMerge);
    
    IRNode* currentFailControl = currentSeq;
    std::vector<std::vector<std::unordered_map<std::string, IRNode*>>> branchEnvs;
    
    for (auto& branch : expr->catchBranches) {
        currentSeq = currentFailControl;
        envStack = baseEnv;
        pushScope(); // Branch scope
        
        IRNode* branchSuccessMerge = createCFGNode(IROp::Merge);
        IRNode* nextBranchMerge = createCFGNode(IROp::Merge);
        
        std::vector<std::vector<std::unordered_map<std::string, IRNode*>>> patEnvs;
        for (auto& pat : branch.patterns) {
            IRNode* patFailMerge = createCFGNode(IROp::Merge);
            auto envBefore = envStack;
            buildPatternMatch(pat.get(), subjectNode, patFailMerge, ScopeModifier::Local, false, false); // ★ 软匹配
            patEnvs.push_back(envStack);
            branchSuccessMerge->addData(currentSeq);
            currentSeq = patFailMerge;
            envStack = envBefore;
        }
        nextBranchMerge->addData(currentSeq);
        
        if (branch.patterns.size() > 1) {
            size_t branchScope = envStack.size() - 1;
            std::unordered_set<std::string> patVars;
            for (auto& pEnv : patEnvs) {
                for (auto& pair : pEnv[branchScope]) patVars.insert(pair.first);
            }
            for (auto& name : patVars) {
                IRNode* phi = graph->createValueNode(IROp::Phi);
                phi->setControl(branchSuccessMerge);
                for (auto& pEnv : patEnvs) {
                    phi->addData(pEnv[branchScope].count(name) ? pEnv[branchScope].at(name) : graph->createConstant(Value::none()));
                }
                phi->name = name;
                envStack[branchScope][name] = phi;
            }
        } else if (!patEnvs.empty()) {
            envStack = patEnvs[0];
        }
        
        currentSeq = branchSuccessMerge;
        if (branch.guard) {
            branch.guard->accept(*this);
            IRNode* ifNode = createCFGNode(IROp::If);
            link(ifNode);
            ifNode->addData(lastValue);
            IRNode* ifTrue = createCFGNode(IROp::IfTrue);
            ifTrue->setControl(ifNode);
            IRNode* ifFalse = createCFGNode(IROp::IfFalse);
            ifFalse->setControl(ifNode);
            nextBranchMerge->addData(ifFalse);
            currentSeq = ifTrue;
        }
        
        branch.body->accept(*this);
        endMerge->addData(currentSeq);
        catchResultPhi->addData(lastValue);
        
        popScope();
        branchEnvs.push_back(envStack);
        currentFailControl = nextBranchMerge;
    }
    
    // ★ 所有 catch 分支失败 → Throw errVal（异常向外传播）
    currentSeq = currentFailControl;
    envStack = baseEnv;
    pushScope();
    IRNode* throwNode = createCFGNode(IROp::Throw);
    link(throwNode);
    throwNode->addData(errVal);
    recordExitNode(throwNode);
    popScope();
    branchEnvs.push_back(envStack);
    
    // ★ 异常分支内部 env 汇合
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
    
    IRNode* catchControl = endMerge;
    IRNode* catchVal = catchResultPhi;
    auto catchEnv = envStack;

    // 3. 汇合
    IRNode* mergeNode = createCFGNode(IROp::Merge);
    mergeNode->addData(normalControl);
    mergeNode->addData(catchControl);
    currentSeq = mergeNode;

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

void IRBuilder::visitIncludeExpr(IncludeExpr* expr) {
    expr->value->accept(*this);
    IRNode* includeNode = graph->createValueNode(IROp::Include);
    link(includeNode);
    includeNode->addData(lastValue);
    currentSeq = includeNode;
    lastValue = includeNode;
}

void IRBuilder::visitImportExpr(ImportExpr* expr) {
    expr->path->accept(*this);
    IRNode* importNode = graph->createValueNode(IROp::Import);
    link(importNode);
    importNode->payload1 = expr->packagePath ? 1 : 0;
    importNode->addData(lastValue);
    currentSeq = importNode;
    lastValue = importNode;
    if (expr->isInclude) {
        IRNode* includeNode = graph->createValueNode(IROp::Include);
        link(includeNode);
        includeNode->addData(lastValue);
        currentSeq = includeNode;
        lastValue = includeNode;
    }
}

void IRBuilder::visitClassDefExpr(ClassDefExpr* expr) {
    graph->currentLine = expr->name.line;
    
    std::unordered_set<std::string> instanceMembers;
    std::unordered_set<std::string> staticMembers;
    
    auto checkRedef = [&](const std::string& name, bool isStatic) {
        if (!allowInternalNames && isReservedInternalName(name)) error("Compile Error: Illegal use of reserved internal name '" + name + "'.");
        if (isStatic) {
            if (staticMembers.count(name)) error("Syntax Error: Static member '" + name + "' is redefined.");
            staticMembers.insert(name);
        } else {
            if (instanceMembers.count(name)) error("Syntax Error: Instance member '" + name + "' is redefined.");
            instanceMembers.insert(name);
        }
    };
    
    for (auto& p : expr->staticProperties) checkRedef(p.name.lexeme, true);
    for (auto& p : expr->instanceProperties) checkRedef(p.name.lexeme, false);

    ClassContext ctx;
    ctx.name = expr->name.lexeme;
    for (auto& p : expr->staticProperties) if (p.isLocal) ctx.privateMembers.insert(p.name.lexeme);
    for (auto& p : expr->instanceProperties) if (p.isLocal) ctx.privateMembers.insert(p.name.lexeme);
    classStack.push_back(ctx);

    bool hasInit = false;
    for (auto& p : expr->instanceProperties) {
        if (p.name.lexeme == "init") hasInit = true;
    }
    
    // ★ 字段默认值初始化器：独立于用户 init，用保留名承载。
    // 原因：`<init>` 是按**名字**存进类成员表的，而 trait 组合时成员按名字复制，若字段初始化器
    // 也叫 `<init>`，后组合的 trait（或类自身的 init）会把前一个覆盖掉，导致部分字段默认值
    // 丢失（`with F1, F2` 两个 trait 同名私有字段会收敛成同一个；类自身写 init 时 trait 字段
    // 直接丢失）。改名后每个类/trait 各自持有一份自己的字段初始化器，实例化时按继承链与 trait
    // 顺序全部执行。
    bool hasFieldDefaults = false;
    // ★ 省略初值的成员（value == nullptr）没有默认值可注入：它们必须由构造期赋值。
    for (auto& p : expr->instanceProperties) {
        if (p.value && !dynamic_cast<LambdaExpr*>(p.value.get())) { hasFieldDefaults = true; break; }
    }
    if (hasFieldDefaults) {
        auto initBody = std::make_shared<Block>(std::vector<std::unique_ptr<Expr>>());
        auto initLambda = std::make_unique<LambdaExpr>(
            JC2_FIELD_INIT_NAME, std::vector<Token>{}, std::vector<bool>{}, std::vector<bool>{}, std::vector<std::shared_ptr<Expr>>{}, "", std::vector<std::shared_ptr<Expr>>{}, nullptr, "", initBody
        );
        expr->instanceProperties.push_back({Token(TokenType::IDENTIFIER, JC2_FIELD_INIT_NAME, expr->name.line), std::move(initLambda), false, false});

        // ★ 保留「父类有 <init>」这一既有可用性：用户没写 init 时补一个空的 <init>，
        // 使子类里的 `super.init()` 仍能解析到（字段默认值已由 <fieldinit> 完成，
        // 这里不需要再初始化字段，否则会与 <fieldinit> 重复写入）。
        if (!hasInit) {
            auto noopBody = std::make_shared<Block>(std::vector<std::unique_ptr<Expr>>());
            auto noopLambda = std::make_unique<LambdaExpr>(
                "init", std::vector<Token>{}, std::vector<bool>{}, std::vector<bool>{}, std::vector<std::shared_ptr<Expr>>{}, "", std::vector<std::shared_ptr<Expr>>{}, nullptr, "", noopBody
            );
            expr->instanceProperties.push_back({Token(TokenType::IDENTIFIER, "init", expr->name.line), std::move(noopLambda), false, false});
        }
    }

    // 字段默认值只由 <fieldinit> 负责；用户 init 不再承担（否则两处都会 DEFINE_PROP，
    // 触发 "Property 'x' already defined"）。因此 isInitMethod 只对 <fieldinit> 生效。
    std::vector<ClassDefExpr::PropertyDef*> fieldsToInit;
    for (auto& p : expr->instanceProperties) {
        if (p.value && !dynamic_cast<LambdaExpr*>(p.value.get())) {
            fieldsToInit.push_back(&p);
        }
    }

    IRNode* classNode = graph->createValueNode(IROp::Class);
    link(classNode);
    classNode->name = expr->name.lexeme;
    if (!expr->name.lexeme.empty()) classValueNodes[expr->name.lexeme] = classNode;
    classNode->payload1 = expr->isTrait ? 1 : 0;
    currentSeq = classNode;
        
    if (expr->superClassExpr) {
        expr->superClassExpr->accept(*this);
        IRNode* superNode = lastValue;
        IRNode* inheritNode = graph->createNode(IROp::Inherit);
        link(inheritNode);
        inheritNode->addData(classNode);
        inheritNode->addData(superNode);
        currentSeq = inheritNode;
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
        
    for (auto& p : expr->instanceProperties) {
        if (auto* lambda = dynamic_cast<LambdaExpr*>(p.value.get())) {
            // 仅 <fieldinit> 负责字段默认值；用户 init 只写用户逻辑（字段此时已就绪）
            bool isInit = (p.name.lexeme == JC2_FIELD_INIT_NAME);
            
            if (isInit) {
                this->isInitMethod = true;
                this->instancePropertiesToInit = &fieldsToInit;
            }
            
            lambda->accept(*this);
            IRNode* closureNode = lastValue;
            
            if (isInit) {
                this->isInitMethod = false;
                this->instancePropertiesToInit = nullptr;
            }
            
            IROp methodOp = IROp::Method;
            if (p.isLocal && p.isConst) methodOp = IROp::MethodPrivateConst;
            else if (p.isLocal) methodOp = IROp::MethodPrivate;
            else if (p.isConst) methodOp = IROp::MethodConst;

            std::string emitName = p.name.lexeme;
            if (emitName == "init") emitName = "<init>";
            else if (emitName == "finalize") emitName = "<finalize>";

            IRNode* methodNode = graph->createNode(methodOp);
            link(methodNode);
            methodNode->addData(classNode);
            methodNode->addData(closureNode);
            methodNode->name = emitName;
            methodNode->payload1 = p.is_abstract ? 1 : 0;
            currentSeq = methodNode;
        }
    }

    // ★ 字段声明登记：类体里声明的实例字段（非 lambda）以"只有标志、没有值"的形式
    //   记到类上，供后续「成员编号 + 扁平槽位」使用。值为 none 即"这是声明，不是赋值"，
    //   因此对类与实例都不可见（见 docs/OOP_MODEL_DESIGN.md §六 第 10 步）。
    for (auto& p : expr->instanceProperties) {
        if (dynamic_cast<LambdaExpr*>(p.value.get())) continue;
        IRNode* noneNode = graph->createValueNode(IROp::Constant);
        link(noneNode);
        noneNode->constVal = Value::none();
        IROp defOp = IROp::DefineProp;
        if (p.isLocal && p.isConst) defOp = IROp::DefinePrivateConst;
        else if (p.isLocal) defOp = IROp::DefinePrivate;
        else if (p.isConst) defOp = IROp::DefinePropConst;
        IRNode* defNode = graph->createValueNode(defOp);
        link(defNode);
        defNode->addData(classNode);
        defNode->addData(noneNode);
        defNode->name = p.name.lexeme;
        currentSeq = defNode;
    }

    for (auto& p : expr->staticProperties) {
        p.value->accept(*this);
        IRNode* valNode = lastValue;

        IROp propOp = IROp::SetProperty;
        if (p.isLocal && p.isConst) propOp = IROp::DefinePrivateConst;
        else if (p.isLocal) propOp = IROp::DefinePrivate;
        else if (p.isConst) propOp = IROp::DefinePropConst;

        std::string emitName = p.name.lexeme;
        if (emitName == "init") emitName = "<init>";
        else if (emitName == "finalize") emitName = "<finalize>";

        IRNode* setPropNode = graph->createValueNode(propOp);
        link(setPropNode);
        setPropNode->addData(classNode);
        setPropNode->addData(valNode);
        setPropNode->name = emitName;
        currentSeq = setPropNode;
    }

    for (auto& t : expr->traitExprs) {
        t->accept(*this);
        IRNode* traitNode = lastValue;
        IRNode* withNode = graph->createNode(IROp::WithTrait);
        link(withNode);
        withNode->addData(classNode);
        withNode->addData(traitNode);
        currentSeq = withNode;
    }

    if (expr->isTrait) {
        IRNode* freezeNode = graph->createNode(IROp::FreezeClass);
        link(freezeNode);
        freezeNode->addData(classNode);
        currentSeq = freezeNode;
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
                imagPart = BaseNum::fromString(numPart, base).getValue().toFloat();
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
    JC2_THROW(ParserError, "Enum member value must be a compile-time constant.");
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
    link(node);
    lastValue = node;
}

void IRBuilder::visitNamespaceDecl(NamespaceDecl* expr) {
    graph->currentLine = expr->name.line;
    
    int prevNamespaceDepth = namespaceScopeDepth;
    namespaceScopeDepth = static_cast<int>(envStack.size());
    pushScope();

    IRNode* nsBegin = graph->createValueNode(IROp::NamespaceBegin);
    link(nsBegin);

    auto savedLocalVars = currentLocalVars;
    auto savedConstVars = currentConstVars;
    currentLocalVars.clear();
    currentConstVars.clear();
    namespaceMemberScopes.emplace_back();
    
    declareVariable("<namespace>", nsBegin);
    currentLocalVars.insert("<namespace>");
    currentConstVars.insert("<namespace>");
    if (!expr->name.lexeme.empty()) {
        declareVariable(expr->name.lexeme, nsBegin);
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
    
    std::vector<std::string> fieldKeys;
    std::vector<IRNode*> fieldNodes;
    std::vector<bool> fieldConsts;
    
    // 1) 普通字段来自当前 namespace 作用域的 env，排除显式 local。
    std::unordered_set<std::string> collected;
    for (const auto& [k, v] : envStack.back()) {
        if (currentLocalVars.count(k)) continue;
        IRNode* target = v;
        if (target->op == IROp::Constant && target->constVal.isUninit()) {
            auto cit = classValueNodes.find(k);
            if (cit != classValueNodes.end()) {
                target = cit->second;
            } else {
                auto nit = namedValueNodes.find(k);
                if (nit != namedValueNodes.end()) target = nit->second;
                else {
                    auto git = globalValueNodes.find(k);
                    if (git != globalValueNodes.end()) target = git->second;
                }
            }
        }
        auto mit = namespaceMemberScopes.back().members.find(k);
        if (mit != namespaceMemberScopes.back().members.end()) target = mit->second;
        fieldKeys.push_back(k);
        fieldNodes.push_back(target);
        fieldConsts.push_back(currentConstVars.count(k) > 0);
        capturedNodesToExtend.push_back(target);
        collected.insert(k);
    }
    for (const auto& [k, target] : namespaceMemberScopes.back().members) {
        if (collected.count(k)) continue;
        fieldKeys.push_back(k);
        fieldNodes.push_back(target);
        fieldConsts.push_back(false);
        capturedNodesToExtend.push_back(target);
    }

    namespaceMemberScopes.pop_back();
    popScope();
    namespaceScopeDepth = prevNamespaceDepth;
    
    IRNode* nsNode = graph->createValueNode(IROp::BuildNamespace);
    link(nsNode);
    nsNode->name = expr->name.lexeme;
    
    for (size_t i = 0; i < fieldKeys.size(); ++i) {
        IRNode* keyNode = graph->createConstant(Value(fieldKeys[i]));
        link(keyNode);
        nsNode->addData(keyNode);
        
        IRNode* slotNode = graph->createConstant(Value(0.0));
        link(slotNode);
        nsNode->addData(slotNode);
        
        IRNode* targetNode = fieldNodes[i];
        this->graph->postAllocCallbacks.push_back([slotNode, targetNode]() {
            slotNode->constVal = Value(static_cast<double>(targetNode->getResolved()->physicalReg));
        });
        
        IRNode* constNode = graph->createConstant(Value(fieldConsts[i]));
        link(constNode);
        nsNode->addData(constNode);
    }
    
    for (size_t i = 0; i < fieldNodes.size(); ++i) {
        nsNode->addData(fieldNodes[i]);
    }
    
    nsNode->payload1 = static_cast<uint32_t>(fieldKeys.size());
    nsNode->dataInputs.insert(nsNode->dataInputs.begin(), nsBegin);
    currentSeq = nsNode;
    
    lastValue = nsNode;
    if (!namespaceMemberScopes.empty() && !expr->name.lexeme.empty() &&
        !expr->isLocalBinding && expr->bindsVariable) {
        namespaceMemberScopes.back().members[expr->name.lexeme] = nsNode;
    }
    currentLocalVars = std::move(savedLocalVars);
    currentConstVars = std::move(savedConstVars);
}
    
void IRBuilder::visitDotAccess(DotAccess* expr) {
    graph->currentLine = expr->field.line;
    if (!allowInternalNames && isReservedInternalName(expr->field.lexeme)) {
        error(expr->field.line, "Compile Error: Illegal use of reserved internal name '" + expr->field.lexeme + "'.");
    }
    std::string fieldName = expr->field.lexeme;

    if (dynamic_cast<SuperExpr*>(expr->object.get())) {
        if (fieldName == "init" || fieldName == "finalize") {
            if (!currentFunction || currentFunction->name != fieldName) {
                error(expr->field.line, "Syntax Error: Cannot access super." + fieldName + " outside of a " + fieldName + " method.");
            }
            if (!loopStack.empty()) {
                error(expr->field.line, "Compile Error: super." + fieldName + " cannot be accessed inside a loop.");
            }
            if (fieldName == "init") {
                if (hasCalledSuperInit) error(expr->field.line, "Compile Error: super.init can only be accessed once per constructor.");
                hasCalledSuperInit = true;
            } else {
                if (hasCalledSuperFinalize) error(expr->field.line, "Compile Error: super.finalize can only be accessed once per finalizer.");
                hasCalledSuperFinalize = true;
            }
            fieldName = "<" + fieldName + ">";
        }

        IRNode* selfNode = graph->createValueNode(IROp::GetSelf);
        link(selfNode);
        
        IRNode* node = graph->createValueNode(IROp::GetSuper);
        link(node);
        node->addData(selfNode);
        node->name = fieldName;
        currentSeq = node;
        lastValue = node;
        return;
    }
    
    // 顶层 const enum：G.A 直接折成常量，绕开 GetGlobal + GetProperty
    if (auto* gv = dynamic_cast<Variable*>(expr->object.get())) {
        auto sit = exprSymbols->find(gv);
        if (sit != exprSymbols->end() && sit->second.scope == VarScope::Global) {
            auto eit = globalEnumFields.find(gv->name.lexeme);
            if (eit != globalEnumFields.end()) {
                auto fit = eit->second.find(fieldName);
                if (fit != eit->second.end()) {
                    lastValue = graph->createConstant(fit->second);
                    link(lastValue);
                    return;
                }
            }
        }
    }

    expr->object->accept(*this);
    IRNode* objNode = lastValue;

    // 编译期枚举：const enum A { OK, ERROR } 的 A.OK 直接折成常量 0
    if (objNode && objNode->op == IROp::Constant && objNode->constVal.isObjType(ObjType::NAMESPACE)) {
        auto* enumNs = static_cast<ObjNamespace*>(objNode->constVal.asObj());
        auto fieldIt = enumNs->fields.find(fieldName);
        if (fieldIt != enumNs->fields.end() && fieldIt->second.isConst && fieldIt->second.upval) {
            IRNode* folded = graph->createConstant(fieldIt->second.upval->closed);
            link(folded);
            lastValue = folded;
            return;
        }
    }
        
    IROp op = IROp::GetProperty;
    if (!classStack.empty()) {
        bool isSelf = dynamic_cast<SelfExpr*>(expr->object.get()) != nullptr;
        bool isClassVar = false;
        if (auto* ck = dynamic_cast<ContextKeywordExpr*>(expr->object.get())) {
            if (ck->kind == ContextKeywordExpr::Kind::Class) isClassVar = true;
        } else if (auto* var = dynamic_cast<Variable*>(expr->object.get())) {
            if (!classStack.back().name.empty() && var->name.lexeme == classStack.back().name) {
                isClassVar = true;
            }
        }
        if ((isSelf || isClassVar) && classStack.back().privateMembers.count(expr->field.lexeme)) {
            op = IROp::GetPrivate;
        }
    }

    IRNode* node = graph->createValueNode(op);
    link(node);
    node->addData(objNode);
    node->name = fieldName;
    currentSeq = node;
    lastValue = node;
}

void IRBuilder::visitDotAssign(DotAssign* expr) {
    graph->currentLine = expr->field.line;
    if (!allowInternalNames && isReservedInternalName(expr->field.lexeme)) {
        error(expr->field.line, "Compile Error: Illegal use of reserved internal name '" + expr->field.lexeme + "'.");
    }
    expr->object->accept(*this);
    IRNode* objNode = lastValue;
        
    expr->value->accept(*this);
    IRNode* valNode = lastValue;
        
    if (valNode->op == IROp::Class || valNode->op == IROp::BuildNamespace) {
        if (valNode->name.empty()) valNode->name = expr->field.lexeme;
    }
        
    IROp op = IROp::SetProperty;
    if (!classStack.empty()) {
        bool isSelf = dynamic_cast<SelfExpr*>(expr->object.get()) != nullptr;
        bool isClassVar = false;
        if (auto* ck = dynamic_cast<ContextKeywordExpr*>(expr->object.get())) {
            if (ck->kind == ContextKeywordExpr::Kind::Class) isClassVar = true;
        } else if (auto* var = dynamic_cast<Variable*>(expr->object.get())) {
            if (!classStack.back().name.empty() && var->name.lexeme == classStack.back().name) {
                isClassVar = true;
            }
        }
        if ((isSelf || isClassVar) && classStack.back().privateMembers.count(expr->field.lexeme)) {
            op = IROp::SetPrivate;
        }
    }

    IRNode* node = graph->createValueNode(op);
    link(node);
    node->addData(objNode);
    node->addData(valNode);
    node->name = expr->field.lexeme;
    currentSeq = node;

    if (expr->typeHint) {
        expr->typeHint->accept(*this);
        IRNode* typeNode = lastValue;
        IRNode* assertNode = graph->createNode(IROp::AssertType);
        link(assertNode);
        assertNode->addData(valNode);
        assertNode->addData(typeNode);
        assertNode->name = expr->field.lexeme;
        currentSeq = assertNode;
    }

    lastValue = valNode;
}

void IRBuilder::visitMethodCallExpr(MethodCallExpr* expr) {
    graph->currentLine = expr->method.line;
    if (!allowInternalNames && isReservedInternalName(expr->method.lexeme)) {
        error(expr->method.line, "Compile Error: Illegal use of reserved internal name '" + expr->method.lexeme + "'.");
    }
    std::string methodName = expr->method.lexeme;
    bool isSuper = dynamic_cast<SuperExpr*>(expr->object.get()) != nullptr;
    
    if (isSuper) {
        if (methodName == "init" || methodName == "finalize") {
            if (!currentFunction || currentFunction->name != methodName) {
                error(expr->method.line, "Syntax Error: Cannot call super." + methodName + "() outside of a " + methodName + " method.");
            }
            if (!loopStack.empty()) {
                error(expr->method.line, "Compile Error: super." + methodName + "() cannot be called inside a loop.");
            }
            if (methodName == "init") {
                if (hasCalledSuperInit) error(expr->method.line, "Compile Error: super.init() can only be called once per constructor.");
                hasCalledSuperInit = true;
            } else {
                if (hasCalledSuperFinalize) error(expr->method.line, "Compile Error: super.finalize() can only be called once per finalizer.");
                hasCalledSuperFinalize = true;
            }
            methodName = "<" + methodName + ">";
        }
    }

    IRNode* objNode = nullptr;
    if (isSuper) {
        objNode = graph->createValueNode(IROp::GetSelf);
        link(objNode);
    } else {
        expr->object->accept(*this);
        objNode = lastValue;
    }
        
    std::vector<IRNode*> argNodes;
    IRCallSignature sig;
    
    int kwArgc = 0;
    for (size_t i = 0; i < expr->arguments.size(); ++i) {
        auto& arg = expr->arguments[i];
        std::string kwName = "";
        Expr* actualArg = arg.get();
        
        if (auto* kwArg = dynamic_cast<KeywordArgExpr*>(arg.get())) {
            kwName = kwArg->name.lexeme;
            actualArg = kwArg->value.get();
            
            IRNode* nameNode = graph->createConstant(Value(kwName));
            link(nameNode);
            argNodes.push_back(nameNode);
            kwArgc++;
        }
        else if (auto* spread = dynamic_cast<SpreadExpr*>(arg.get())) {
            if (spread->isKeyword) {
                IRNode* noneNode = graph->createConstant(Value::none());
                link(noneNode);
                IRNode* markNode = graph->createValueNode(IROp::MakeSpread);
                link(markNode);
                markNode->addData(noneNode);
                markNode->payload1 = 1;
                argNodes.push_back(markNode);
                currentSeq = markNode;
                kwArgc++;
                actualArg = spread->value.get();
            }
        }
        
        if (auto* var = dynamic_cast<Variable*>(actualArg)) {
            std::string name = var->name.lexeme;
            auto argVarIt = exprSymbols->find(var);
            ResolvedSym argSym = argVarIt != exprSymbols->end() ? argVarIt->second : ResolvedSym{};
            if (refParams.count(name)) {
                ResolvedSym rs; rs.scope = VarScope::RefParam;
                sig.refs.push_back({static_cast<uint8_t>(i), 4, name, -1, readVariable(name, rs), kwName, argSym.isConst});
            } else {
                IRNode* localNode = getLocalNode(name);
                if (localNode) {
                    capturedLocals.insert(name);
                    capturedNodesToExtend.push_back(localNode);
                    sig.refs.push_back({static_cast<uint8_t>(i), 2, name, -1, localNode, kwName, argSym.isConst});
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
                        sig.refs.push_back({static_cast<uint8_t>(i), 3, name, upvalIdx, nullptr, kwName, argSym.isConst});
                    } else {
                        sig.refs.push_back({static_cast<uint8_t>(i), 1, name, -1, nullptr, kwName, argSym.isConst});
                    }
                }
            }
        }
        actualArg->accept(*this);
        argNodes.push_back(lastValue);
    }
    
    if (!sig.refs.empty()) {
        graph->callSignatures.push_back(sig);
        IRNode* passRefsNode = graph->createNode(IROp::PassRefs);
        link(passRefsNode);
        passRefsNode->payload1 = static_cast<uint32_t>(graph->callSignatures.size() - 1);
        for (auto& ref : sig.refs) {
            if (ref.localNode) passRefsNode->addData(ref.localNode);
        }
        currentSeq = passRefsNode;
    }
        
    bool hasFallback = false;
    IRNode* fallbackNode = nullptr;
    
    if (!isSuper) {
        std::string name = methodName;
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
    if (!isSuper && !classStack.empty()) {
        bool isSelf = dynamic_cast<SelfExpr*>(expr->object.get()) != nullptr;
        bool isClassVar = false;
        if (auto* ck = dynamic_cast<ContextKeywordExpr*>(expr->object.get())) {
            if (ck->kind == ContextKeywordExpr::Kind::Class) isClassVar = true;
        } else if (auto* var = dynamic_cast<Variable*>(expr->object.get())) {
            if (!classStack.back().name.empty() && var->name.lexeme == classStack.back().name) {
                isClassVar = true;
            }
        }
        if ((isSelf || isClassVar) && classStack.back().privateMembers.count(methodName)) {
            isPrivate = true;
        }
    }

    IROp op;
    if (isSuper) op = IROp::SuperInvoke;
    else if (isPrivate) op = IROp::InvokePrivate;
    else if (hasFallback) op = IROp::InvokeFallback;
    else op = IROp::Invoke;

    IRNode* invokeNode = graph->createValueNode(op);
    link(invokeNode);
    invokeNode->addData(objNode);
    for (auto* arg : argNodes) {
        invokeNode->addData(arg);
    }
    if (hasFallback) {
        invokeNode->addData(fallbackNode);
    }
    invokeNode->payload1 = static_cast<uint32_t>(argNodes.size());
    invokeNode->payload2 = static_cast<uint32_t>(kwArgc);
    invokeNode->name = methodName;
        
    currentSeq = invokeNode;
    lastValue = invokeNode;
}

void IRBuilder::visitSuperExpr(SuperExpr*) {
    error("Syntax Error: 'super' can only be used for method calls or property access.");
}

void IRBuilder::visitSelfExpr(SelfExpr*) {
    IRNode* node = graph->createValueNode(IROp::GetSelf);
    link(node);
    lastValue = node;
}

void IRBuilder::visitContextKeywordExpr(ContextKeywordExpr* expr) {
    graph->currentLine = expr->keyword.line;
    const char* internal = (expr->kind == ContextKeywordExpr::Kind::Class) ? "<class>" : "<namespace>";
    auto it = exprSymbols->find(expr);
    ResolvedSym sym = it != exprSymbols->end() ? it->second : ResolvedSym{};
    lastValue = readVariable(internal, sym);
}

void IRBuilder::visitDestructAssign(DestructAssign* expr) {
    std::vector<std::tuple<std::string, ScopeModifier, bool, Pattern*>> boundVars;
    collectPatternVars(expr->pattern.get(), boundVars);

    // ★ state 永远向上绑定到闭包，闭包外没有可绑定的状态槽。
    //   顶层的 `[state a] = @[1]` 会在 buildPatternMatch 的 state 分支里
    //   解引用 currentFunction->upvalues → 空指针（0xC0000005）；
    //   下面那段 state 初始化还带 `&& currentFunction`，不加这条检查它会
    //   静默跳过、把畸形 IR 留到后面炸。与 visitVarDecl / visitAssign /
    //   visitStateDecl 的同名检查保持同一措辞。
    if (!currentFunction) {
        bool usesState = expr->isState;
        for (const auto& varTuple : boundVars) {
            if (std::get<1>(varTuple) == ScopeModifier::State) { usesState = true; break; }
        }
        if (usesState) error("Syntax Error: 'state' modifier cannot be used at the top level.");
    }

    std::vector<std::string> tempStateNames;
    std::vector<std::pair<std::string, std::pair<int, IRNode*>>> hiddenVars;

    std::vector<std::string> patVarsToHide;
    for (const auto& varTuple : boundVars) {
        const std::string& name = std::get<0>(varTuple);
        ScopeModifier mod = std::get<1>(varTuple);
        bool isConst = std::get<2>(varTuple);
        
        if (expr->isLocal || expr->isState || expr->isConst || 
            mod == ScopeModifier::Local || mod == ScopeModifier::State || isConst) {
            patVarsToHide.push_back(name);
        }
        
        if (mod == ScopeModifier::None) {
            if (expr->isState) mod = ScopeModifier::State;
        }
        if (mod == ScopeModifier::State) {
            tempStateNames.push_back(name);
        }
    }

    for (const auto& name : patVarsToHide) {
        for (int i = static_cast<int>(envStack.size()) - 1; i >= 0; --i) {
            auto it = envStack[i].find(name);
            if (it != envStack[i].end()) {
                hiddenVars.push_back({name, {i, it->second}});
                envStack[i].erase(it);
                break;
            }
        }
    }
    
    IRNode* skipMerge = nullptr;
    auto baseEnv = envStack;
    
    if (expr->isState && !tempStateNames.empty() && currentFunction) {
        ResolvedSym sym; sym.scope = VarScope::State;
        IRNode* getVal = readVariable(tempStateNames[0], sym);
        IRNode* isUninit = graph->createValueNode(IROp::IsUninit);
        isUninit->addData(getVal);
        link(isUninit);
            
        IRNode* ifNode = createCFGNode(IROp::If);
        ifNode->addData(isUninit);
        link(ifNode);
            
        IRNode* ifTrue = createCFGNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
            
        IRNode* ifFalse = createCFGNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
            
        skipMerge = createCFGNode(IROp::Merge);
        skipMerge->addData(ifFalse);
            
        currentSeq = ifTrue;
    }
        
    expr->value->accept(*this);
    IRNode* valNode = lastValue;
    
    for (const auto& hv : hiddenVars) {
        envStack[hv.second.first][hv.first] = hv.second.second;
    }

    IRNode* failMerge = createCFGNode(IROp::Merge);
    
    ScopeModifier mod = ScopeModifier::None;
    if (expr->isLocal) mod = ScopeModifier::Local;
    else if (expr->isRef) mod = ScopeModifier::Ref;
    else if (expr->isState) mod = ScopeModifier::State;

    buildPatternMatch(expr->pattern.get(), valNode, failMerge, mod, expr->isConst, true);
    
    if (!failMerge->dataInputs.empty()) {
        IRNode* throwNode = createCFGNode(IROp::ThrowTyped);
        throwNode->setControl(failMerge);
        IRNode* errStr = graph->createConstant(Value("Destructuring pattern match failed."));
        errStr->setControl(failMerge);
        throwNode->addData(errStr);
        IRNode* typeStr = graph->createConstant(Value(jc::err::MatchErrorClass));
        typeStr->setControl(failMerge);
        throwNode->addData(typeStr);
        recordExitNode(throwNode);
    }
    
    if (skipMerge) {
        auto trueEnv = envStack;
        skipMerge->addData(currentSeq);
        currentSeq = skipMerge;
        
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
                    phi->addData(fNode); // Index 0: ifFalse
                    phi->addData(tNode); // Index 1: ifTrue_end
                    phi->name = name;
                    envStack[i][name] = phi;
                }
            }
        }
        
        IRNode* phi = graph->createValueNode(IROp::Phi);
        phi->setControl(skipMerge);
        IRNode* noneNode = graph->createConstant(Value::none());
        noneNode->setControl(skipMerge);
        phi->addData(noneNode); // Index 0: ifFalse
        phi->addData(valNode);  // Index 1: ifTrue_end
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
            link(litNode);
            parts.push_back(litNode);
        }
        expr->exprs[i]->accept(*this);
        IRNode* exprVal = lastValue;
            
        if (!expr->formatSpecs[i].empty()) {
            IRNode* specNode = graph->createConstant(Value(expr->formatSpecs[i]));
            link(specNode);
            IRNode* fmtNode = graph->createValueNode(IROp::FormatString);
            link(fmtNode);
            fmtNode->addData(exprVal);
            fmtNode->addData(specNode);
            currentSeq = fmtNode;
            parts.push_back(fmtNode);
        } else {
            IRNode* strNode = graph->createValueNode(IROp::Stringify);
            link(strNode);
            strNode->addData(exprVal);
            currentSeq = strNode;
            parts.push_back(strNode);
        }
    }
    if (!expr->literals.back().empty()) {
        IRNode* litNode = graph->createConstant(Value(expr->literals.back()));
        link(litNode);
        parts.push_back(litNode);
    }
        
    IRNode* concatNode = graph->createValueNode(IROp::ConcatStrings);
    link(concatNode);
    for (auto* p : parts) concatNode->addData(p);
    concatNode->payload1 = static_cast<uint32_t>(parts.size());
        
    currentSeq = concatNode;
    lastValue = concatNode;
}

void IRBuilder::visitMatrixCompExpr(MatrixCompExpr* expr) {
    pushScope(); // Scope for comprehension
    
    IRNode* accNode = graph->createValueNode(IROp::MatrixCompInit);
    link(accNode);
    currentSeq = accNode;
    
    buildCompClause(expr, 0, accNode, IROp::MatrixCompAppend);
    
    IRNode* endNode = graph->createValueNode(IROp::MatrixCompEnd);
    link(endNode);
    endNode->addData(accNode);
    currentSeq = endNode;
    lastValue = endNode;
    
    popScope();
}

void IRBuilder::visitListCompExpr(ListCompExpr* expr) {
    pushScope(); // Scope for comprehension
    
    IRNode* listNode = graph->createValueNode(IROp::ListInit);
    link(listNode);
    currentSeq = listNode;
    
    buildCompClause(expr, 0, listNode, IROp::ListAppend);
    
    lastValue = listNode;
    
    popScope();
}

void IRBuilder::buildSetCompClause(SetCompExpr* expr, size_t clauseIdx, IRNode* setNode) {
    if (clauseIdx >= expr->clauses.size()) {
        expr->valueExpr->accept(*this);
        IRNode* valNode = lastValue;
        IRNode* appendNode = graph->createNode(IROp::SetAppend);
        link(appendNode);
        appendNode->addData(setNode);
        appendNode->addData(valNode);
        currentSeq = appendNode;
        return;
    }
    
    auto& clause = expr->clauses[clauseIdx];
    clause.iterable->accept(*this);
    IRNode* iterNode = graph->createValueNode(IROp::IterInit);
    link(iterNode);
    iterNode->addData(lastValue);
    
    auto isDestructPattern = [](Pattern* p) {
        while (auto* dp = dynamic_cast<DefaultPattern*>(p)) p = dp->inner.get();
        return dynamic_cast<ListPattern*>(p) != nullptr || dynamic_cast<MatrixPattern*>(p) != nullptr;
    };
    iterNode->payload1 = isDestructPattern(clause.pattern.get()) ? 1 : 0;
    currentSeq = iterNode;
    
    IRNode* loopNode = createCFGNode(IROp::Loop);
    loopNode->addData(currentSeq);
    
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
    currentSeq = loopNode;
    
    IRNode* exitControl = createCFGNode(IROp::Merge);

    IRNode* nextNode = graph->createValueNode(IROp::IterNext);
    link(nextNode);
    nextNode->addData(iterNode);
    currentSeq = nextNode;
    
    IRNode* isUninitNode = graph->createValueNode(IROp::IsUninit);
    link(isUninitNode);
    isUninitNode->addData(nextNode);
    
    IRNode* ifNode = createCFGNode(IROp::If);
    link(ifNode);
    ifNode->addData(isUninitNode);
    
    IRNode* ifTrue = createCFGNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    exitControl->addData(ifTrue);
    auto exitEnvStack = envStack;
    
    IRNode* ifFalse = createCFGNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    currentSeq = ifFalse;
    
    auto envBeforePattern = envStack;
    IRNode* failMerge = createCFGNode(IROp::Merge);
    buildPatternMatch(clause.pattern.get(), nextNode, failMerge, ScopeModifier::Local, false, true);
    
    auto envBeforeCond = envStack;
    IRNode* condFailMerge = createCFGNode(IROp::Merge);
    for (auto& cond : clause.conditions) {
        cond->accept(*this);
        IRNode* condIfNode = createCFGNode(IROp::If);
        link(condIfNode);
        condIfNode->addData(lastValue);
        
        IRNode* condIfTrue = createCFGNode(IROp::IfTrue);
        condIfTrue->setControl(condIfNode);
        
        IRNode* condIfFalse = createCFGNode(IROp::IfFalse);
        condIfFalse->setControl(condIfNode);
        condFailMerge->addData(condIfFalse);
        
        currentSeq = condIfTrue;
    }
    
    buildSetCompClause(expr, clauseIdx + 1, setNode);
    
    loopNode->addData(currentSeq);
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
    currentSeq = exitControl;
}

void IRBuilder::visitSetCompExpr(SetCompExpr* expr) {
    pushScope();
    
    IRNode* setNode = graph->createValueNode(IROp::SetInit);
    link(setNode);
    currentSeq = setNode;
    
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
        link(appendNode);
        appendNode->addData(dictNode);
        appendNode->addData(keyNode);
        appendNode->addData(valNode);
        currentSeq = appendNode;
        return;
    }
    
    auto& clause = expr->clauses[clauseIdx];
    clause.iterable->accept(*this);
    IRNode* iterNode = graph->createValueNode(IROp::IterInit);
    link(iterNode);
    iterNode->addData(lastValue);
    
    auto isDestructPattern = [](Pattern* p) {
        while (auto* dp = dynamic_cast<DefaultPattern*>(p)) p = dp->inner.get();
        return dynamic_cast<ListPattern*>(p) != nullptr || dynamic_cast<MatrixPattern*>(p) != nullptr;
    };
    iterNode->payload1 = isDestructPattern(clause.pattern.get()) ? 1 : 0;
    currentSeq = iterNode;
    
    IRNode* loopNode = createCFGNode(IROp::Loop);
    loopNode->addData(currentSeq);
    
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
    currentSeq = loopNode;
    
    IRNode* exitControl = createCFGNode(IROp::Merge);

    IRNode* nextNode = graph->createValueNode(IROp::IterNext);
    link(nextNode);
    nextNode->addData(iterNode);
    currentSeq = nextNode;
    
    IRNode* isUninitNode = graph->createValueNode(IROp::IsUninit);
    link(isUninitNode);
    isUninitNode->addData(nextNode);
    
    IRNode* ifNode = createCFGNode(IROp::If);
    link(ifNode);
    ifNode->addData(isUninitNode);
    
    IRNode* ifTrue = createCFGNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    exitControl->addData(ifTrue);
    auto exitEnvStack = envStack;
    
    IRNode* ifFalse = createCFGNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    currentSeq = ifFalse;
    
    auto envBeforePattern = envStack;
    IRNode* failMerge = createCFGNode(IROp::Merge);
    buildPatternMatch(clause.pattern.get(), nextNode, failMerge, ScopeModifier::Local, false, true);
    
    auto envBeforeCond = envStack;
    IRNode* condFailMerge = createCFGNode(IROp::Merge);
    for (auto& cond : clause.conditions) {
        cond->accept(*this);
        IRNode* condIfNode = createCFGNode(IROp::If);
        link(condIfNode);
        condIfNode->addData(lastValue);
        
        IRNode* condIfTrue = createCFGNode(IROp::IfTrue);
        condIfTrue->setControl(condIfNode);
        
        IRNode* condIfFalse = createCFGNode(IROp::IfFalse);
        condIfFalse->setControl(condIfNode);
        condFailMerge->addData(condIfFalse);
        
        currentSeq = condIfTrue;
    }
    
    buildDictCompClause(expr, clauseIdx + 1, dictNode);
    
    loopNode->addData(currentSeq);
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
    currentSeq = exitControl;
}

void IRBuilder::visitDictCompExpr(DictCompExpr* expr) {
    pushScope();
    
    IRNode* dictNode = graph->createValueNode(IROp::DictInit);
    link(dictNode);
    currentSeq = dictNode;
    
    buildDictCompClause(expr, 0, dictNode);
    
    lastValue = dictNode;
    popScope();
}
    
void IRBuilder::visitDictLiteral(DictLiteral* expr) {
    std::vector<IRNode*> keysAndVals;
    for (auto& pair : expr->entries) {
        if (pair.first == nullptr) {
            // ★ rest entry：...dict 关键字解包，编译成 [ObjSpread(标记), dict]
            auto* spread = dynamic_cast<SpreadExpr*>(pair.second.get());
            IRNode* noneNode = graph->createConstant(Value::none());
            link(noneNode);
            IRNode* markNode = graph->createValueNode(IROp::MakeSpread);
            link(markNode);
            markNode->addData(noneNode);
            markNode->payload1 = 1;
            keysAndVals.push_back(markNode);
            currentSeq = markNode; // ★ 更新 control，保证标记先于后续求值
            spread->value->accept(*this);
            keysAndVals.push_back(lastValue);
        } else {
            pair.first->accept(*this);
            keysAndVals.push_back(lastValue);
            pair.second->accept(*this);
            keysAndVals.push_back(lastValue);
        }
    }
    IRNode* node = graph->createValueNode(IROp::BuildDict);
    link(node);
    for (auto* kv : keysAndVals) node->addData(kv);
    node->payload1 = static_cast<uint32_t>(expr->entries.size());
    currentSeq = node;
    lastValue = node;
}

void IRBuilder::visitSetLiteral(SetLiteral* expr) {
    std::vector<IRNode*> elements;
    for (auto& e : expr->elements) {
        e->accept(*this);
        elements.push_back(lastValue);
    }
    IRNode* node = graph->createValueNode(IROp::BuildSet);
    link(node);
    for (auto* e : elements) node->addData(e);
    node->payload1 = static_cast<uint32_t>(expr->elements.size());
    currentSeq = node;
    lastValue = node;
}

void IRBuilder::visitSliceExpr(SliceExpr* expr) {
    IRNode* startNode = nullptr;
    if (expr->start) {
        expr->start->accept(*this);
        startNode = lastValue;
    } else {
        startNode = graph->createConstant(Value::none());
        link(startNode);
    }

    IRNode* endNode = nullptr;
    if (expr->end) {
        expr->end->accept(*this);
        endNode = lastValue;
    } else {
        endNode = graph->createConstant(Value::none());
        link(endNode);
    }

    IRNode* stepNode = nullptr;
    if (expr->step) {
        expr->step->accept(*this);
        stepNode = lastValue;
    } else {
        stepNode = graph->createConstant(Value::none());
        link(stepNode);
    }

    IRNode* node = graph->createValueNode(IROp::BuildSlice);
    link(node);
    node->addData(startNode);
    node->addData(endNode);
    node->addData(stepNode);
    currentSeq = node;
    lastValue = node;
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

void IRBuilder::visitKeywordArgExpr(KeywordArgExpr* expr) {
    expr->value->accept(*this);
}

void IRBuilder::visitSpreadExpr(SpreadExpr* expr) {
    expr->value->accept(*this);
    IRNode* valNode = lastValue;
    IRNode* spreadNode = graph->createValueNode(IROp::MakeSpread);
    link(spreadNode);
    spreadNode->addData(valNode);
    spreadNode->payload1 = expr->isKeyword ? 1 : 0;
    currentSeq = spreadNode;
    lastValue = spreadNode;
}

void IRBuilder::visitDeferExpr(DeferExpr* expr) {
    if (compiledFunctions) {
        auto fnDef = std::make_shared<CompiledFunction>();
        fnDef->name = "<defer>";
        
        IRGraph fnGraph;
        IRBuilder fnBuilder(&fnGraph, compiledFunctions, this, fnDef.get(), exprSymbols, patternSymbols, deleteSyms);
        
        fnBuilder.buildFunctionParams({}, {}, "", {}, {}, {});
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
        link(closureNode);
        closureNode->name = std::to_string(fnIdx);
        
        for (auto& target : fnBuilder.upvalueTargets) {
            if (target.isLocal && target.localNode) {
                closureNode->addData(target.localNode);
            }
        }
        
        IRNode* deferNode = graph->createNode(IROp::Defer);
        link(deferNode);
        deferNode->addData(closureNode);
        currentSeq = deferNode;
        
        deferCounts.back()++;
        lastValue = graph->createConstant(Value::none());
        link(lastValue);
    } else {
        error("Syntax Error: 'defer' cannot be used at the top level without a compiled function context.");
    }
}

void IRBuilder::visitMatchExpr(MatchExpr* expr) {
    expr->subject->accept(*this);
    IRNode* subjectNode = lastValue;
    
    IRNode* endMerge = createCFGNode(IROp::Merge);
    IRNode* resultPhi = graph->createValueNode(IROp::Phi);
    resultPhi->setControl(endMerge);
    
    IRNode* currentFailControl = currentSeq;
    auto baseEnv = envStack;
    std::vector<std::vector<std::unordered_map<std::string, IRNode*>>> branchEnvs;
    
    for (auto& branch : expr->branches) {
        currentSeq = currentFailControl;
        envStack = baseEnv;
        pushScope(); // Scope for branch
        
        IRNode* branchSuccessMerge = createCFGNode(IROp::Merge);
        IRNode* nextBranchMerge = createCFGNode(IROp::Merge);
        
        std::vector<std::vector<std::unordered_map<std::string, IRNode*>>> patEnvs;
        for (auto& pat : branch.patterns) {
            IRNode* patFailMerge = createCFGNode(IROp::Merge);
            
            auto envBefore = envStack;
            buildPatternMatch(pat.get(), subjectNode, patFailMerge, ScopeModifier::Local, false, false);
            
            patEnvs.push_back(envStack);
            branchSuccessMerge->addData(currentSeq);
            currentSeq = patFailMerge;
            envStack = envBefore;
        }
        nextBranchMerge->addData(currentSeq);
        
        if (branch.patterns.size() > 1) {
            // 多 pattern（or-pattern）：同名变量的绑定可能来自不同 IR 节点，需 phi 合并
            size_t branchScope = envStack.size() - 1;
            std::unordered_set<std::string> patVars;
            for (auto& pEnv : patEnvs) {
                for (auto& pair : pEnv[branchScope]) patVars.insert(pair.first);
            }
            for (auto& name : patVars) {
                IRNode* phi = graph->createValueNode(IROp::Phi);
                phi->setControl(branchSuccessMerge);
                for (auto& pEnv : patEnvs) {
                    phi->addData(pEnv[branchScope].count(name) ? pEnv[branchScope].at(name) : graph->createConstant(Value::none()));
                }
                phi->name = name;
                envStack[branchScope][name] = phi;
            }
        } else if (!patEnvs.empty()) {
            envStack = patEnvs[0];
        }
        
        currentSeq = branchSuccessMerge;
        if (branch.guard) {
            branch.guard->accept(*this);
            IRNode* ifNode = createCFGNode(IROp::If);
            link(ifNode);
            ifNode->addData(lastValue);
            
            IRNode* ifTrue = createCFGNode(IROp::IfTrue);
            ifTrue->setControl(ifNode);
            
            IRNode* ifFalse = createCFGNode(IROp::IfFalse);
            ifFalse->setControl(ifNode);
            nextBranchMerge->addData(ifFalse);
            
            currentSeq = ifTrue;
        }
        
        branch.body->accept(*this);
        endMerge->addData(currentSeq);
        resultPhi->addData(lastValue);
        
        popScope();
        branchEnvs.push_back(envStack);
        currentFailControl = nextBranchMerge;
    }
    
    currentSeq = currentFailControl;
    envStack = baseEnv;
    pushScope();
    // ★ fallback：所有分支失败 → 抛 MatchError（消息带 subject 值）
    IRNode* strNode = graph->createValueNode(IROp::Stringify);
    link(strNode);
    strNode->addData(subjectNode);
    currentSeq = strNode;
    
    IRNode* prefixNode = graph->createConstant(Value("match: no branch matched for value "));
    link(prefixNode);
    
    IRNode* concatNode = graph->createValueNode(IROp::ConcatStrings);
    link(concatNode);
    concatNode->addData(prefixNode);
    concatNode->addData(strNode);
    concatNode->payload1 = 2;
    currentSeq = concatNode;
    
    IRNode* throwNode = createCFGNode(IROp::ThrowTyped);
    link(throwNode);
    throwNode->addData(concatNode);
    IRNode* typeStr = graph->createConstant(Value(jc::err::MatchErrorClass));
    link(typeStr);
    throwNode->addData(typeStr);
    recordExitNode(throwNode);
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
    
    currentSeq = endMerge;
    lastValue = resultPhi;
}

} // namespace jc
