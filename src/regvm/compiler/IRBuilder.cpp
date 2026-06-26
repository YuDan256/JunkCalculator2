#include "IRBuilder.h"
#include "IROptimizer.h"
#include "RegisterAllocator.h"
#include "Emitter.h"

namespace jc {
namespace regvm {

static void collectPatternVars(Pattern* pat, std::vector<std::tuple<std::string, ScopeModifier, bool>>& boundVars) {
    if (auto* dp = dynamic_cast<DefaultPattern*>(pat)) {
        collectPatternVars(dp->inner.get(), boundVars);
    } else if (auto* vp = dynamic_cast<VariablePattern*>(pat)) {
        if (vp->name.lexeme != "_") boundVars.push_back({vp->name.lexeme, vp->modifier, vp->isConst});
    } else if (auto* rp = dynamic_cast<RestPattern*>(pat)) {
        if (rp->name.lexeme != "_") boundVars.push_back({rp->name.lexeme, rp->modifier, rp->isConst});
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

IRNode* IRBuilder::readVariable(const std::string& name) {
    IRNode* localNode = getLocalNode(name);
    if (localNode) return localNode;

    if (currentFunction) {
        for (size_t i = 0; i < currentFunction->upvalues.size(); ++i) {
            if (currentFunction->upvalues[i].name == name && (currentFunction->upvalues[i].isExplicitState || currentFunction->upvalues[i].isRef)) {
                IRNode* node = graph->createValueNode(IROp::GetUpvalue);
                node->payload1 = static_cast<uint32_t>(i);
                node->name = name;
                node->setControl(currentControl);
                return node;
            }
        }
    }

    int upvalIdx = resolveUpvalue(name);
    if (upvalIdx != -1) {
        IRNode* node = graph->createValueNode(IROp::GetUpvalue);
        node->payload1 = static_cast<uint32_t>(upvalIdx);
        node->name = name;
        node->setControl(currentControl);
        return node;
    }

    // 如果没找到，生成一个 GetGlobal 节点
    IRNode* node = graph->createValueNode(IROp::GetGlobal);
    node->name = name;
    node->setControl(currentControl);
    currentControl = node;
    return node;
}

void IRBuilder::writeVariable(const std::string& name, IRNode* value, bool isConst, bool isGlobalRef) {
    if (envStack.empty()) return;
    // 查找变量在哪个作用域定义的
    for (int i = static_cast<int>(envStack.size()) - 1; i >= 0; --i) {
        auto it = envStack[i].find(name);
        if (it != envStack[i].end()) {
            envStack[i][name] = value;
            return;
        }
    }

    if (currentFunction) {
        for (size_t i = 0; i < currentFunction->upvalues.size(); ++i) {
            if (currentFunction->upvalues[i].name == name && (currentFunction->upvalues[i].isExplicitState || currentFunction->upvalues[i].isRef)) {
                IRNode* node = graph->createNode(IROp::SetUpvalue);
                node->payload1 = static_cast<uint32_t>(i);
                node->name = name;
                node->addData(value);
                node->setControl(currentControl);
                currentControl = node;
                return;
            }
        }
    }

    int upvalIdx = resolveUpvalue(name);
    if (upvalIdx != -1) {
        IRNode* node = graph->createNode(IROp::SetUpvalue);
        node->payload1 = static_cast<uint32_t>(upvalIdx);
        node->name = name;
        node->addData(value);
        node->setControl(currentControl);
        currentControl = node;
        return;
    }

    // 如果都没找到，说明是全局变量赋值
    IROp op = IROp::SetGlobal;
    if (isConst) op = IROp::DefineConstGlobal;
    else if (isGlobalRef) op = IROp::SetGlobalRef;

    IRNode* node = graph->createNode(op);
    node->setControl(currentControl);
    node->addData(value);
    node->name = name;
    currentControl = node;
}

void IRBuilder::declareVariable(const std::string& name, IRNode* value) {
    if (envStack.empty()) return;
    envStack.back()[name] = value;
}

IRBuilder::IRBuilder(IRGraph* graph, std::vector<std::shared_ptr<CompiledFunction>>* compiledFunctions, IRBuilder* parent, CompiledFunction* currentFunction) 
    : graph(graph), compiledFunctions(compiledFunctions), parent(parent), currentFunction(currentFunction), currentControl(nullptr), lastValue(nullptr) {
    envStack.emplace_back(); // 压入顶层作用域
}

IRNode* IRBuilder::getLocalNode(const std::string& name) {
    for (int i = static_cast<int>(envStack.size()) - 1; i >= 0; --i) {
        auto it = envStack[i].find(name);
        if (it != envStack[i].end()) return it->second;
    }
    return nullptr;
}

int IRBuilder::resolveUpvalue(const std::string& name) {
    if (!parent) return -1;

    // Check if parent already has this upvalue
    if (parent->currentFunction) {
        for (size_t i = 0; i < parent->currentFunction->upvalues.size(); ++i) {
            if (parent->currentFunction->upvalues[i].name == name) {
                int upvalIdx = static_cast<int>(currentFunction->upvalues.size());
                CompiledFunction::UpvalueInfo uv;
                uv.name = name;
                uv.isLocal = false;
                uv.index = static_cast<int>(i);
                uv.isRef = false;
                uv.isGlobal = false;
                uv.isExplicitState = false;
                uv.isRefParam = false;
                currentFunction->upvalues.push_back(uv);
                upvalueTargets.push_back({upvalIdx, false, nullptr});
                return upvalIdx;
            }
        }
    }

    IRNode* localNode = parent->getLocalNode(name);
    if (localNode) {
        int upvalIdx = static_cast<int>(currentFunction->upvalues.size());
        CompiledFunction::UpvalueInfo uv;
        uv.name = name;
        uv.isLocal = true;
        uv.index = -1; // Will be resolved after RegisterAllocator
        uv.isRef = false;
        uv.isGlobal = false;
        uv.isExplicitState = false;
        uv.isRefParam = false;
        currentFunction->upvalues.push_back(uv);
        upvalueTargets.push_back({upvalIdx, true, localNode});
        return upvalIdx;
    }

    int upvalue = parent->resolveUpvalue(name);
    if (upvalue != -1) {
        int upvalIdx = static_cast<int>(currentFunction->upvalues.size());
        CompiledFunction::UpvalueInfo uv;
        uv.name = name;
        uv.isLocal = false;
        uv.index = upvalue;
        uv.isRef = false;
        uv.isGlobal = false;
        uv.isExplicitState = false;
        uv.isRefParam = false;
        currentFunction->upvalues.push_back(uv);
        upvalueTargets.push_back({upvalIdx, false, nullptr});
        return upvalIdx;
    }

    return -1;
}

void IRBuilder::buildPatternMatch(Pattern* pat, IRNode* valNode, IRNode* failMerge, ScopeModifier globalMod, bool globalConst) {
    if (auto* dp = dynamic_cast<DefaultPattern*>(pat)) {
        buildPatternMatch(dp->inner.get(), valNode, failMerge, globalMod, globalConst);
        return;
    }
    if (auto* vp = dynamic_cast<VariablePattern*>(pat)) {
        if (vp->name.lexeme != "_") {
            ScopeModifier mod = vp->modifier;
            if (mod == ScopeModifier::None) mod = globalMod;
            bool isConst = vp->isConst || globalConst;

            if (mod == ScopeModifier::State) {
                if (!currentFunction) throw std::runtime_error("IRBuilder Error: 'state' modifier cannot be used at the top level.");
                bool found = false;
                for (auto& u : currentFunction->upvalues) {
                    if (u.name == vp->name.lexeme && u.isExplicitState) {
                        found = true; break;
                    }
                }
                if (!found) {
                    CompiledFunction::UpvalueInfo uv;
                    uv.name = vp->name.lexeme;
                    uv.isLocal = false;
                    uv.index = 0;
                    uv.isRef = false;
                    uv.isGlobal = false;
                    uv.isExplicitState = true;
                    uv.isRefParam = false;
                    currentFunction->upvalues.push_back(uv);
                }

                IRNode* getVal = readVariable(vp->name.lexeme);
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
                writeVariable(vp->name.lexeme, valNode, isConst, false);
                IRNode* trueCtrl = currentControl;

                IRNode* merge = graph->createNode(IROp::Merge);
                merge->addData(trueCtrl);
                merge->addData(ifFalse);
                currentControl = merge;
            } else {
                if (mod == ScopeModifier::Ref) {
                    if (currentFunction) {
                        int upvalIdx = resolveUpvalue(vp->name.lexeme);
                        if (upvalIdx == -1) {
                            CompiledFunction::UpvalueInfo uv;
                            uv.name = vp->name.lexeme;
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

                if (mod == ScopeModifier::Local) {
                    declareVariable(vp->name.lexeme, valNode);
                } else {
                    bool isGlobalRef = (mod == ScopeModifier::Ref) && !currentFunction;
                    writeVariable(vp->name.lexeme, valNode, isConst, isGlobalRef);
                }
            }
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
    } else if (auto* lp = dynamic_cast<ListPattern*>(pat)) {
        IRNode* shapeNode = graph->createValueNode(IROp::MatchShape);
        shapeNode->setControl(currentControl);
        shapeNode->addData(valNode);
        
        IRNode* ifNode = graph->createNode(IROp::If);
        ifNode->setControl(currentControl);
        ifNode->addData(shapeNode);
        
        IRNode* ifTrue = graph->createNode(IROp::IfTrue);
        ifTrue->setControl(ifNode);
        
        IRNode* ifFalse = graph->createNode(IROp::IfFalse);
        ifFalse->setControl(ifNode);
        failMerge->addData(ifFalse);
        
        currentControl = ifTrue;
        
        int restIndex = -1;
        for (size_t i = 0; i < lp->elements.size(); ++i) {
            if (dynamic_cast<RestPattern*>(lp->elements[i].get())) {
                restIndex = static_cast<int>(i);
                break;
            }
        }

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
                sliceNode->addData(stepNode);
                sliceNode->addData(endNode);
                sliceNode->addData(startNode);
                sliceNode->payload1 = 1;
                
                if (restPat->name.lexeme != "_") {
                    if (restPat->modifier == ScopeModifier::State) {
                        bool found = false;
                        for (auto& u : currentFunction->upvalues) {
                            if (u.name == restPat->name.lexeme && u.isExplicitState) {
                                found = true; break;
                            }
                        }
                        if (!found) {
                            CompiledFunction::UpvalueInfo uv;
                            uv.name = restPat->name.lexeme;
                            uv.isLocal = false;
                            uv.index = 0;
                            uv.isRef = false;
                            uv.isGlobal = false;
                            uv.isExplicitState = true;
                            uv.isRefParam = false;
                            currentFunction->upvalues.push_back(uv);
                        }

                        IRNode* getVal = readVariable(restPat->name.lexeme);
                        IRNode* isUninit = graph->createValueNode(IROp::IsUninit);
                        isUninit->addData(getVal);
                        isUninit->setControl(currentControl);

                        IRNode* stateIfNode = graph->createNode(IROp::If);
                        stateIfNode->addData(isUninit);
                        stateIfNode->setControl(currentControl);

                        IRNode* stateIfTrue = graph->createNode(IROp::IfTrue);
                        stateIfTrue->setControl(stateIfNode);

                        IRNode* stateIfFalse = graph->createNode(IROp::IfFalse);
                        stateIfFalse->setControl(stateIfNode);

                        currentControl = stateIfTrue;
                        writeVariable(restPat->name.lexeme, sliceNode);
                        IRNode* trueCtrl = currentControl;

                        IRNode* merge = graph->createNode(IROp::Merge);
                        merge->addData(trueCtrl);
                        merge->addData(stateIfFalse);
                        currentControl = merge;
                    } else {
                        ScopeModifier rmod = restPat->modifier;
                        if (rmod == ScopeModifier::None) rmod = globalMod;
                        bool rConst = restPat->isConst || globalConst;

                        if (rmod == ScopeModifier::Ref) {
                            if (currentFunction) {
                                int upvalIdx = resolveUpvalue(restPat->name.lexeme);
                                if (upvalIdx == -1) {
                                    CompiledFunction::UpvalueInfo uv;
                                    uv.name = restPat->name.lexeme;
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
                        if (rmod == ScopeModifier::Local) {
                            declareVariable(restPat->name.lexeme, sliceNode);
                        } else {
                            bool isGlobalRef = (rmod == ScopeModifier::Ref) && !currentFunction;
                            writeVariable(restPat->name.lexeme, sliceNode, rConst, isGlobalRef);
                        }
                    }
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
            
            buildPatternMatch(lp->elements[i].get(), elemNode, failMerge, globalMod, globalConst);
        }
    } else if (auto* dp = dynamic_cast<DictPattern*>(pat)) {
        IRNode* typeNode = graph->createValueNode(IROp::MatchType);
        typeNode->setControl(currentControl);
        typeNode->addData(valNode);
        
        IRNode* ifNode = graph->createNode(IROp::If);
        ifNode->setControl(currentControl);
        ifNode->addData(typeNode);
        
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
            
            buildPatternMatch(entry.second.get(), elemNode, failMerge, globalMod, globalConst);
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
    
    IRNode* loopNode = graph->createNode(IROp::Loop);
    loopNode->addData(currentControl);
    
    std::unordered_map<std::string, IRNode*> loopPhis;
    for (const auto& pair : envStack.back()) {
        IRNode* phi = graph->createValueNode(IROp::Phi);
        phi->setControl(loopNode);
        phi->addData(pair.second);
        phi->name = pair.first;
        loopPhis[pair.first] = phi;
    }
    envStack.back() = loopPhis;
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
    auto exitEnv = envStack.back();
    
    IRNode* ifFalse = graph->createNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    currentControl = ifFalse;
    
    IRNode* failMerge = graph->createNode(IROp::Merge);
    buildPatternMatch(clause.pattern.get(), nextNode, failMerge, ScopeModifier::Local, false);
    
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
    
    for (auto& pair : loopPhis) {
        pair.second->addData(readVariable(pair.first));
        if (!condFailMerge->dataInputs.empty()) pair.second->addData(readVariable(pair.first));
        if (!failMerge->dataInputs.empty()) pair.second->addData(readVariable(pair.first));
    }
    
    envStack.back() = exitEnv;
    currentControl = exitControl;
}

void IRBuilder::build(Expr* ast) {
    currentControl = graph->startNode;
    ast->accept(*this);
    
    // 自动在末尾插入 Return 节点
    IRNode* retNode = graph->createNode(IROp::Return);
    retNode->setControl(currentControl);
    if (lastValue) {
        retNode->addData(lastValue);
    } else {
        IRNode* noneNode = graph->createConstant(Value::none());
        noneNode->setControl(currentControl);
        retNode->addData(noneNode);
    }
    currentControl = retNode;
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
                throw std::runtime_error("IRBuilder Error: Invalid imaginary literal '" + s + "'.");
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
                throw std::runtime_error("IRBuilder Error: Invalid integer literal '" + s + "'.");
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
    if (expr->op.type == TokenType::PIPE) {
        expr->right->accept(*this);
        IRNode* calleeNode = lastValue;
        
        expr->left->accept(*this);
        IRNode* argNode = lastValue;
        
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
        
        auto baseEnv = envStack.back();
        
        IRNode* mergeNode = graph->createNode(IROp::Merge);
        IRNode* resultPhi = graph->createValueNode(IROp::Phi);
        resultPhi->setControl(mergeNode);
        
        if (expr->op.type == TokenType::AND_AND) {
            currentControl = ifTrue;
            expr->right->accept(*this);
            mergeNode->addData(currentControl);
            resultPhi->addData(lastValue);
            auto rightEnv = envStack.back();
            
            envStack.back() = baseEnv;
            currentControl = ifFalse;
            mergeNode->addData(currentControl);
            resultPhi->addData(leftVal);
            
            envStack.back() = baseEnv;
            std::unordered_set<std::string> modifiedVars;
            for (const auto& pair : rightEnv) {
                if (baseEnv[pair.first] != pair.second) modifiedVars.insert(pair.first);
            }
            for (const auto& name : modifiedVars) {
                IRNode* tNode = rightEnv.count(name) ? rightEnv[name] : baseEnv[name];
                IRNode* eNode = baseEnv[name];
                IRNode* phi = graph->createValueNode(IROp::Phi);
                phi->setControl(mergeNode);
                phi->addData(tNode);
                phi->addData(eNode);
                phi->name = name;
                envStack.back()[name] = phi;
            }
        } else {
            currentControl = ifTrue;
            mergeNode->addData(currentControl);
            resultPhi->addData(leftVal);
            
            envStack.back() = baseEnv;
            currentControl = ifFalse;
            expr->right->accept(*this);
            mergeNode->addData(currentControl);
            resultPhi->addData(lastValue);
            auto rightEnv = envStack.back();
            
            envStack.back() = baseEnv;
            std::unordered_set<std::string> modifiedVars;
            for (const auto& pair : rightEnv) {
                if (baseEnv[pair.first] != pair.second) modifiedVars.insert(pair.first);
            }
            for (const auto& name : modifiedVars) {
                IRNode* tNode = baseEnv[name];
                IRNode* eNode = rightEnv.count(name) ? rightEnv[name] : baseEnv[name];
                IRNode* phi = graph->createValueNode(IROp::Phi);
                phi->setControl(mergeNode);
                phi->addData(tNode);
                phi->addData(eNode);
                phi->name = name;
                envStack.back()[name] = phi;
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
        case TokenType::IN: op = IROp::In; break;
        case TokenType::BIT_AND: op = IROp::BitAnd; break;
        case TokenType::BIT_OR: op = IROp::BitOr; break;
        case TokenType::BIT_XOR: op = IROp::BitXor; break;
        case TokenType::SHIFT_LEFT: op = IROp::Shl; break;
        case TokenType::SHIFT_RIGHT: op = IROp::Shr; break;
        default: throw std::runtime_error("IRBuilder: Unsupported binary operator.");
    }

    // 4. 创建运算节点并连接依赖边
    IRNode* node = graph->createValueNode(op);
    node->setControl(currentControl);
    node->addData(leftVal);
    node->addData(rightVal);
    lastValue = node;
}

void IRBuilder::visitVariable(Variable* expr) {
    lastValue = readVariable(expr->name.lexeme);
}

void IRBuilder::visitAssign(Assign* expr) {
    if (expr->isState) {
        if (!currentFunction) throw std::runtime_error("IRBuilder Error: 'state' modifier cannot be used at the top level.");
        CompiledFunction::UpvalueInfo uv;
        uv.name = expr->name.lexeme;
        uv.isLocal = false;
        uv.index = 0;
        uv.isRef = false;
        uv.isGlobal = false;
        uv.isExplicitState = true;
        uv.isRefParam = false;
        currentFunction->upvalues.push_back(uv);

        IRNode* getVal = readVariable(expr->name.lexeme);
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
        
        std::string originalName = expr->name.lexeme;
        for (auto& u : currentFunction->upvalues) {
            if (u.name == originalName && u.isExplicitState) {
                u.name = "<hidden_state>";
            }
        }
        
        expr->value->accept(*this);
        IRNode* valNode = lastValue;
        
        for (auto& u : currentFunction->upvalues) {
            if (u.name == "<hidden_state>" && u.isExplicitState) {
                u.name = originalName;
            }
        }
        
        writeVariable(expr->name.lexeme, valNode, expr->isConst, false);
        IRNode* trueCtrl = currentControl;

        IRNode* merge = graph->createNode(IROp::Merge);
        merge->addData(trueCtrl);
        merge->addData(ifFalse);
        currentControl = merge;

        lastValue = valNode;
        return;
    }

    if (expr->isRef) {
        if (currentFunction) {
            int upvalIdx = resolveUpvalue(expr->name.lexeme);
            if (upvalIdx == -1) {
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
    }

    expr->value->accept(*this);
    IRNode* valNode = lastValue;
    
    if (expr->isLocal) {
        declareVariable(expr->name.lexeme, valNode);
    } else {
        bool isGlobalRef = expr->isRef && !currentFunction;
        writeVariable(expr->name.lexeme, valNode, expr->isConst, isGlobalRef);
    }
    lastValue = valNode;
}

void IRBuilder::visitBlock(Block* expr) {
    envStack.emplace_back(); // 进入新作用域
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
    envStack.pop_back(); // 离开作用域
}

void IRBuilder::visitGroupingExpr(GroupingExpr* expr) {
    expr->expression->accept(*this);
}

void IRBuilder::visitUnary(Unary* expr) {
    expr->right->accept(*this);
    IRNode* rightVal = lastValue;
    
    IROp op = IROp::Neg;
    switch (expr->op.type) {
        case TokenType::MINUS: op = IROp::Neg; break;
        case TokenType::BANG:  op = IROp::Not; break;
        case TokenType::TILDE: op = IROp::BitNot; break;
        case TokenType::PLUS:  lastValue = rightVal; return; // +x 等价于 x
        default: throw std::runtime_error("IRBuilder: Unsupported unary operator.");
    }
    
    IRNode* node = graph->createValueNode(op);
    node->setControl(currentControl);
    node->addData(rightVal);
    lastValue = node;
}

void IRBuilder::visitCall(Call* expr) {
    std::string calleeName = expr->callee.lexeme;
    IRNode* calleeNode = readVariable(calleeName);
    
    // 2. 编译参数
    std::vector<IRNode*> argNodes;
    for (auto& arg : expr->arguments) {
        arg->accept(*this);
        argNodes.push_back(lastValue);
    }
    
    // 3. 创建 Call 节点
    IRNode* callNode = graph->createValueNode(IROp::Call);
    callNode->setControl(currentControl);
    callNode->addData(calleeNode); // 第一个数据输入是 callee
    for (IRNode* arg : argNodes) {
        callNode->addData(arg);
    }
    callNode->payload1 = static_cast<uint32_t>(argNodes.size()); // 记录 argc
    
    // Call 节点可能会产生副作用，因此它也串联在控制流中
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
    auto baseEnv = envStack.back();

    // 4. 编译 True 分支
    currentControl = ifTrue;
    envStack.emplace_back(baseEnv); // ★ 自动创建块级作用域
    expr->thenBranch->accept(*this);
    IRNode* thenControl = currentControl;
    IRNode* thenVal = lastValue;
    auto thenEnv = envStack.back();
    envStack.pop_back();

    // 5. 编译 False 分支
    currentControl = ifFalse;
    envStack.emplace_back(baseEnv); // ★ 自动创建块级作用域
    IRNode* elseVal = nullptr;
    if (expr->elseBranch) {
        expr->elseBranch->accept(*this);
        elseVal = lastValue;
    } else {
        elseVal = graph->createConstant(Value::none());
        elseVal->setControl(currentControl);
    }
    IRNode* elseControl = currentControl;
    auto elseEnv = envStack.back();
    envStack.pop_back();

    // 6. 创建 Merge 节点汇合控制流
    IRNode* mergeNode = graph->createNode(IROp::Merge);
    mergeNode->addData(thenControl);
    mergeNode->addData(elseControl);
    currentControl = mergeNode;

    // 7. 合并环境 (生成 Phi 节点)
    std::unordered_set<std::string> modifiedVars;
    for (const auto& pair : thenEnv) {
        if (baseEnv.count(pair.first) && baseEnv[pair.first] != pair.second) modifiedVars.insert(pair.first);
    }
    for (const auto& pair : elseEnv) {
        if (baseEnv.count(pair.first) && baseEnv[pair.first] != pair.second) modifiedVars.insert(pair.first);
    }

    for (const auto& name : modifiedVars) {
        IRNode* tNode = thenEnv.count(name) ? thenEnv[name] : baseEnv[name];
        IRNode* eNode = elseEnv.count(name) ? elseEnv[name] : baseEnv[name];
        
        if (tNode != eNode) {
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(mergeNode);
            phi->addData(tNode);
            phi->addData(eNode);
            phi->name = name;
            envStack.back()[name] = phi; // 更新环境为 Phi 节点
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
    
    IRNode* retNode = graph->createNode(IROp::Return);
    retNode->setControl(currentControl);
    retNode->addData(retVal);
    
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
    for (auto& row : expr->elements) {
        for (auto& e : row) {
            e->accept(*this);
            elements.push_back(lastValue);
        }
    }
    IRNode* node = graph->createValueNode(IROp::BuildMatrix);
    node->setControl(currentControl);
    for (auto* e : elements) node->addData(e);
    node->payload1 = static_cast<uint32_t>(expr->elements.size());
    node->payload2 = static_cast<uint32_t>(expr->elements.empty() ? 0 : expr->elements[0].size());
    currentControl = node;
    lastValue = node;
}

void IRBuilder::visitWhileExpr(WhileExpr* expr) {
    envStack.emplace_back(envStack.back()); // ★ 自动创建块级作用域
    
    IRNode* loopNode = graph->createNode(IROp::Loop);
    loopNode->addData(currentControl);
    
    std::unordered_map<std::string, IRNode*> loopPhis;
    for (const auto& pair : envStack.back()) {
        IRNode* phi = graph->createValueNode(IROp::Phi);
        phi->setControl(loopNode);
        phi->addData(pair.second);
        phi->name = pair.first;
        loopPhis[pair.first] = phi;
    }
    envStack.back() = loopPhis;
    currentControl = loopNode;
    
    IRNode* breakMerge = graph->createNode(IROp::Merge);
    loopStack.push_back({loopNode, loopPhis, breakMerge, {}});
    
    expr->condition->accept(*this);
    IRNode* condVal = lastValue;
    
    IRNode* ifNode = graph->createNode(IROp::If);
    ifNode->setControl(currentControl);
    ifNode->addData(condVal);
    
    IRNode* ifTrue = graph->createNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    
    IRNode* ifFalse = graph->createNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    
    currentControl = ifTrue;
    expr->body->accept(*this);
    
    loopNode->addData(currentControl);
    for (auto& pair : loopPhis) {
        pair.second->addData(readVariable(pair.first));
    }
    
    breakMerge->addData(ifFalse);
    loopStack.back().breakEnvs.push_back(envStack.back());
    
    currentControl = breakMerge;
    
    auto& breakEnvs = loopStack.back().breakEnvs;
    std::unordered_map<std::string, IRNode*> exitEnv;
    for (const auto& pair : envStack.back()) {
        const std::string& name = pair.first;
        IRNode* phi = graph->createValueNode(IROp::Phi);
        phi->setControl(breakMerge);
        for (auto& env : breakEnvs) {
            phi->addData(env.count(name) ? env.at(name) : graph->createConstant(Value::none()));
        }
        phi->name = name;
        exitEnv[name] = phi;
    }
    
    loopStack.pop_back();
    
    auto finalEnv = exitEnv;
    envStack.pop_back();
    for (const auto& pair : finalEnv) {
        if (envStack.back().count(pair.first)) {
            envStack.back()[pair.first] = pair.second;
        }
    }
    
    lastValue = graph->createConstant(Value::none());
    lastValue->setControl(currentControl);
}

void IRBuilder::visitForExpr(ForExpr* expr) {
    envStack.emplace_back(envStack.back()); // ★ 自动创建块级作用域
    expr->initializer->accept(*this);
    
    IRNode* loopNode = graph->createNode(IROp::Loop);
    loopNode->addData(currentControl);
    
    std::unordered_map<std::string, IRNode*> loopPhis;
    for (const auto& pair : envStack.back()) {
        IRNode* phi = graph->createValueNode(IROp::Phi);
        phi->setControl(loopNode);
        phi->addData(pair.second);
        phi->name = pair.first;
        loopPhis[pair.first] = phi;
    }
    envStack.back() = loopPhis;
    currentControl = loopNode;
    
    IRNode* breakMerge = graph->createNode(IROp::Merge);
    loopStack.push_back({loopNode, loopPhis, breakMerge, {}});
    
    expr->condition->accept(*this);
    IRNode* condVal = lastValue;
    
    IRNode* ifNode = graph->createNode(IROp::If);
    ifNode->setControl(currentControl);
    ifNode->addData(condVal);
    
    IRNode* ifTrue = graph->createNode(IROp::IfTrue);
    ifTrue->setControl(ifNode);
    
    IRNode* ifFalse = graph->createNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    
    currentControl = ifTrue;
    expr->body->accept(*this);
    expr->update->accept(*this);
    
    loopNode->addData(currentControl);
    for (auto& pair : loopPhis) {
        pair.second->addData(readVariable(pair.first));
    }
    
    breakMerge->addData(ifFalse);
    loopStack.back().breakEnvs.push_back(loopPhis);
    
    currentControl = breakMerge;
    
    auto& breakEnvs = loopStack.back().breakEnvs;
    std::unordered_map<std::string, IRNode*> exitEnv;
    for (const auto& pair : envStack.back()) {
        const std::string& name = pair.first;
        IRNode* phi = graph->createValueNode(IROp::Phi);
        phi->setControl(breakMerge);
        for (auto& env : breakEnvs) {
            phi->addData(env.count(name) ? env.at(name) : graph->createConstant(Value::none()));
        }
        phi->name = name;
        exitEnv[name] = phi;
    }
    
    loopStack.pop_back();
    
    auto finalEnv = exitEnv;
    envStack.pop_back();
    for (const auto& pair : finalEnv) {
        if (envStack.back().count(pair.first)) {
            envStack.back()[pair.first] = pair.second;
        }
    }
    
    lastValue = graph->createConstant(Value::none());
    lastValue->setControl(currentControl);
}

void IRBuilder::visitBreakExpr(BreakExpr*) {
    if (loopStack.empty()) throw std::runtime_error("IRBuilder: break outside loop");
    auto& loop = loopStack.back();
    loop.breakMerge->addData(currentControl);
    loop.breakEnvs.push_back(envStack.back());
    
    currentControl = graph->createNode(IROp::Merge); // Dead code
}

void IRBuilder::visitContinueExpr(ContinueExpr*) {
    if (loopStack.empty()) throw std::runtime_error("IRBuilder: continue outside loop");
    auto& loop = loopStack.back();
    loop.loopNode->addData(currentControl);
    for (auto& pair : loop.loopPhis) {
        pair.second->addData(readVariable(pair.first));
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
    if (expr->hasObjectExpr()) {
        expr->objectExpr->accept(*this);
    } else {
        lastValue = readVariable(expr->name.lexeme);
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
            writeVariable(expr->name.lexeme, rootObjNode);
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
            writeVariable(expr->name.lexeme, rootObjNode);
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
        
        for (int level = depth - 2; level >= 0; --level) {
            IRNode* backSetNode = graph->createValueNode(IROp::IndexSet);
            backSetNode->setControl(currentControl);
            backSetNode->addData(chainObjs[level]);
            for (auto* idx : indicesTmp[level]) backSetNode->addData(idx);
            backSetNode->addData(chainObjs[level + 1]);
            backSetNode->payload1 = static_cast<uint32_t>(indicesTmp[level].size());
            currentControl = backSetNode;
        }
        
        if (!expr->hasObjectExpr()) {
            writeVariable(expr->name.lexeme, rootObjNode);
        }
        lastValue = valNode;
    }
}
    
void IRBuilder::visitLocalDecl(LocalDecl* expr) {
    IRNode* noneNode = graph->createConstant(Value::none());
    noneNode->setControl(currentControl);
    declareVariable(expr->name.lexeme, noneNode);
    lastValue = noneNode;
}

void IRBuilder::visitRefDecl(RefDecl* expr) {
    if (currentFunction) {
        int upvalIdx = resolveUpvalue(expr->name.lexeme);
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

    lastValue = graph->createConstant(Value::none());
    lastValue->setControl(currentControl);
}

void IRBuilder::visitStateDecl(StateDecl* expr) {
    if (!currentFunction) throw std::runtime_error("IRBuilder Error: 'state' modifier cannot be used at the top level.");
    CompiledFunction::UpvalueInfo uv;
    uv.name = expr->name.lexeme;
    uv.isLocal = false;
    uv.index = 0;
    uv.isRef = false;
    uv.isGlobal = false;
    uv.isExplicitState = false;
    uv.isRefParam = false;
    currentFunction->upvalues.push_back(uv);

    lastValue = graph->createConstant(Value::none());
    lastValue->setControl(currentControl);
}
    
void IRBuilder::visitConstDecl(ConstDecl* expr) {
    IRNode* noneNode = graph->createConstant(Value::none());
    noneNode->setControl(currentControl);
    declareVariable(expr->name.lexeme, noneNode);
    lastValue = noneNode;
}

void IRBuilder::visitDeleteExpr(DeleteExpr* expr) {
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

    if (auto* var = dynamic_cast<Variable*>(expr->target.get())) {
        targetVal = readVariable(var->name.lexeme);
    } else if (auto* dot = dynamic_cast<DotAccess*>(expr->target.get())) {
        dot->object->accept(*this);
        objNode = lastValue;
        propName = dot->field.lexeme;
        
        targetVal = graph->createValueNode(IROp::GetProperty);
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
        
        chain[0]->object->accept(*this);
        objNode = lastValue;
        
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
        throw std::runtime_error("IRBuilder: Unsupported compound assignment target.");
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
        default: throw std::runtime_error("IRBuilder: Unsupported compound operator.");
    }

    IRNode* opNode = graph->createValueNode(op);
    opNode->setControl(currentControl);
    opNode->addData(targetVal);
    opNode->addData(rightVal);

    if (auto* var = dynamic_cast<Variable*>(expr->target.get())) {
        if (expr->isState) {
            if (!currentFunction) throw std::runtime_error("IRBuilder Error: 'state' modifier cannot be used at the top level.");
            CompiledFunction::UpvalueInfo uv;
            uv.name = var->name.lexeme;
            uv.isLocal = false;
            uv.index = 0;
            uv.isRef = false;
            uv.isGlobal = false;
            uv.isExplicitState = false;
            uv.isRefParam = false;
            currentFunction->upvalues.push_back(uv);
        } else if (expr->isRef) {
            if (currentFunction) {
                int upvalIdx = resolveUpvalue(var->name.lexeme);
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

        if (expr->isLocal) declareVariable(var->name.lexeme, opNode);
        else {
            bool isGlobalRef = expr->isRef && !currentFunction;
            writeVariable(var->name.lexeme, opNode, false, isGlobalRef);
        }
    } else if (dynamic_cast<DotAccess*>(expr->target.get())) {
        IRNode* setProp = graph->createValueNode(IROp::SetProperty);
        setProp->setControl(currentControl);
        setProp->addData(objNode);
        setProp->addData(opNode);
        setProp->name = propName;
        currentControl = setProp;
    } else if (dynamic_cast<IndexAccess*>(expr->target.get())) {
        int depth = static_cast<int>(chain.size());
        if (depth == 1) {
            IRNode* setIdx = graph->createValueNode(IROp::IndexSet);
            setIdx->setControl(currentControl);
            setIdx->addData(objNode);
            for (auto* i : indices) setIdx->addData(i);
            setIdx->addData(opNode);
            setIdx->payload1 = static_cast<uint32_t>(indices.size());
            currentControl = setIdx;
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
            
            for (int level = depth - 2; level >= 0; --level) {
                IRNode* backSetNode = graph->createValueNode(IROp::IndexSet);
                backSetNode->setControl(currentControl);
                backSetNode->addData(chainObjs[level]);
                for (auto* idx : allIndices[level]) backSetNode->addData(idx);
                backSetNode->addData(chainObjs[level + 1]);
                backSetNode->payload1 = static_cast<uint32_t>(allIndices[level].size());
                currentControl = backSetNode;
            }
        }
    }

    lastValue = opNode;
}

void IRBuilder::visitLambdaExpr(LambdaExpr* expr) {
    if (compiledFunctions) {
        auto fnDef = std::make_shared<CompiledFunction>();
        fnDef->name = expr->name.empty() ? "lambda" : expr->name;
        fnDef->arity = static_cast<int>(expr->params.size()) - (expr->hasRestParam ? 1 : 0);
        fnDef->maxArity = static_cast<int>(expr->params.size());
        fnDef->hasRestParam = expr->hasRestParam;
        fnDef->paramIsRef = expr->paramIsRef;
        fnDef->paramIsConst = expr->paramIsConst;
        
        IRGraph fnGraph;
        IRBuilder fnBuilder(&fnGraph, compiledFunctions, this, fnDef.get());
        
        for (size_t i = 0; i < expr->params.size(); ++i) {
            IRNode* paramNode = fnGraph.createValueNode(IROp::Parameter);
            paramNode->payload1 = static_cast<uint32_t>(i);
            fnBuilder.declareVariable(expr->params[i].lexeme, paramNode);
            
            if (expr->defaultExprs[i]) {
                IRNode* isUninit = fnGraph.createValueNode(IROp::IsUninit);
                isUninit->setControl(fnBuilder.currentControl);
                isUninit->addData(paramNode);
                
                IRNode* ifNode = fnGraph.createNode(IROp::If);
                ifNode->setControl(fnBuilder.currentControl);
                ifNode->addData(isUninit);
                
                IRNode* ifTrue = fnGraph.createNode(IROp::IfTrue);
                ifTrue->setControl(ifNode);
                
                IRNode* ifFalse = fnGraph.createNode(IROp::IfFalse);
                ifFalse->setControl(ifNode);
                
                fnBuilder.currentControl = ifTrue;
                expr->defaultExprs[i]->accept(fnBuilder);
                IRNode* defVal = fnBuilder.lastValue;
                IRNode* trueCtrl = fnBuilder.currentControl;
                
                IRNode* merge = fnGraph.createNode(IROp::Merge);
                merge->addData(trueCtrl);
                merge->addData(ifFalse);
                
                IRNode* phi = fnGraph.createValueNode(IROp::Phi);
                phi->setControl(merge);
                phi->addData(defVal);
                phi->addData(paramNode);
                phi->name = expr->params[i].lexeme;
                
                fnBuilder.currentControl = merge;
                fnBuilder.writeVariable(expr->params[i].lexeme, phi);
            }
        }
        
        fnBuilder.build(expr->body.get());
        
        IROptimizer::optimize(&fnGraph);
        RegisterAllocator::allocate(&fnGraph);
        
        for (auto& target : fnBuilder.upvalueTargets) {
            if (target.isLocal && target.localNode) {
                fnDef->upvalues[target.index].index = target.localNode->physicalReg;
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
    for (auto& arg : expr->arguments) {
        arg->accept(*this);
        argNodes.push_back(lastValue);
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
    envStack.emplace_back(envStack.back()); // ★ 自动创建块级作用域
    expr->iterable->accept(*this);
    IRNode* iterNode = graph->createValueNode(IROp::IterInit);
    iterNode->setControl(currentControl);
    iterNode->addData(lastValue);
    currentControl = iterNode;
    
    IRNode* loopNode = graph->createNode(IROp::Loop);
    loopNode->addData(currentControl);
    
    std::unordered_map<std::string, IRNode*> loopPhis;
    for (const auto& pair : envStack.back()) {
        IRNode* phi = graph->createValueNode(IROp::Phi);
        phi->setControl(loopNode);
        phi->addData(pair.second);
        phi->name = pair.first;
        loopPhis[pair.first] = phi;
    }
    envStack.back() = loopPhis;
    currentControl = loopNode;
    
    IRNode* breakMerge = graph->createNode(IROp::Merge);
    loopStack.push_back({loopNode, loopPhis, breakMerge, {}});
    
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
    loopStack.back().breakEnvs.push_back(envStack.back());
    
    IRNode* ifFalse = graph->createNode(IROp::IfFalse);
    ifFalse->setControl(ifNode);
    currentControl = ifFalse;
    
    IRNode* failMerge = graph->createNode(IROp::Merge);
    ScopeModifier mod = expr->isLocal ? ScopeModifier::Local : ScopeModifier::None;
    buildPatternMatch(expr->pattern.get(), nextNode, failMerge, mod, expr->isConst);
    
    expr->body->accept(*this);
    
    loopNode->addData(currentControl);
    for (auto& pair : loopPhis) {
        pair.second->addData(readVariable(pair.first));
    }
    
    if (!failMerge->dataInputs.empty()) {
        breakMerge->addData(failMerge);
        loopStack.back().breakEnvs.push_back(envStack.back());
    }
    
    currentControl = breakMerge;
    
    auto& breakEnvs = loopStack.back().breakEnvs;
    std::unordered_map<std::string, IRNode*> exitEnv;
    for (const auto& pair : envStack.back()) {
        const std::string& name = pair.first;
        IRNode* phi = graph->createValueNode(IROp::Phi);
        phi->setControl(breakMerge);
        for (auto& env : breakEnvs) {
            phi->addData(env.count(name) ? env.at(name) : graph->createConstant(Value::none()));
        }
        phi->name = name;
        exitEnv[name] = phi;
    }
    
    loopStack.pop_back();
    
    auto finalEnv = exitEnv;
    envStack.pop_back();
    for (const auto& pair : finalEnv) {
        if (envStack.back().count(pair.first)) {
            envStack.back()[pair.first] = pair.second;
        }
    }
    
    lastValue = graph->createConstant(Value::none());
    lastValue->setControl(currentControl);
}

void IRBuilder::visitThrowExpr(ThrowExpr* expr) {
    expr->value->accept(*this);
    IRNode* throwNode = graph->createNode(IROp::Throw);
    throwNode->setControl(currentControl);
    throwNode->addData(lastValue);
    currentControl = throwNode;
    lastValue = throwNode;
}

void IRBuilder::visitTryCatchExpr(TryCatchExpr* expr) {
    IRNode* tryBegin = graph->createNode(IROp::TryBegin);
    tryBegin->setControl(currentControl);

    auto baseEnv = envStack.back();

    // 1. 正常分支
    currentControl = tryBegin;
    expr->tryBody->accept(*this);
    IRNode* tryEnd = graph->createNode(IROp::TryEnd);
    tryEnd->setControl(currentControl);
    IRNode* normalControl = tryEnd;
    IRNode* normalVal = lastValue;
    auto normalEnv = envStack.back();

    // 2. 异常分支
    envStack.back() = baseEnv;
    
    IRNode* catchNode = graph->createValueNode(IROp::Catch);
    catchNode->setControl(tryBegin);
    currentControl = catchNode;
    
    envStack.emplace_back(); // Catch scope

    IRNode* errVal = catchNode; // Catch 节点产生异常对象

    IRNode* failMerge = graph->createNode(IROp::Merge);
    buildPatternMatch(expr->catchPattern.get(), errVal, failMerge, ScopeModifier::Local, false);

    if (!failMerge->dataInputs.empty()) {
        IRNode* throwNode = graph->createNode(IROp::Throw);
        throwNode->setControl(failMerge);
        throwNode->addData(errVal);
    }

    expr->catchBody->accept(*this);
    IRNode* catchControl = currentControl;
    IRNode* catchVal = lastValue;
    
    auto catchScopeEnv = envStack.back();
    envStack.pop_back();
    for (const auto& pair : catchScopeEnv) {
        if (envStack.back().count(pair.first)) {
            envStack.back()[pair.first] = pair.second;
        }
    }
    auto catchEnv = envStack.back();

    // 3. 汇合
    IRNode* mergeNode = graph->createNode(IROp::Merge);
    mergeNode->addData(normalControl);
    mergeNode->addData(catchControl);
    currentControl = mergeNode;

    envStack.back() = baseEnv;
    std::unordered_set<std::string> modifiedVars;
    for (const auto& pair : normalEnv) {
        if (baseEnv[pair.first] != pair.second) modifiedVars.insert(pair.first);
    }
    for (const auto& pair : catchEnv) {
        if (baseEnv[pair.first] != pair.second) modifiedVars.insert(pair.first);
    }

    for (const auto& name : modifiedVars) {
        IRNode* nNode = normalEnv.count(name) ? normalEnv[name] : baseEnv[name];
        IRNode* cNode = catchEnv.count(name) ? catchEnv[name] : baseEnv[name];
        if (nNode != cNode) {
            IRNode* phi = graph->createValueNode(IROp::Phi);
            phi->setControl(mergeNode);
            phi->addData(nNode);
            phi->addData(cNode);
            phi->name = name;
            envStack.back()[name] = phi;
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
    
    for (auto& caseBranch : expr->cases) {
        currentControl = currentFailControl;
        
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
        
        currentFailControl = nextCaseMerge;
    }
    
    currentControl = currentFailControl;
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
    
    currentControl = endMerge;
    lastValue = resultPhi;
}

void IRBuilder::visitClassDefExpr(ClassDefExpr* expr) {
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
        
    for (auto& method : expr->methods) {
        if (compiledFunctions) {
            auto fnDef = std::make_shared<CompiledFunction>();
            fnDef->name = method.name.lexeme;
            fnDef->arity = static_cast<int>(method.params.size()) - (method.hasRestParam ? 1 : 0);
            fnDef->maxArity = static_cast<int>(method.params.size());
            fnDef->hasRestParam = method.hasRestParam;
            fnDef->paramIsRef = method.paramIsRef;
            fnDef->paramIsConst = method.paramIsConst;
            
            IRGraph fnGraph;
            IRBuilder fnBuilder(&fnGraph, compiledFunctions, this, fnDef.get());
            
            for (size_t i = 0; i < method.params.size(); ++i) {
                IRNode* paramNode = fnGraph.createValueNode(IROp::Parameter);
                paramNode->payload1 = static_cast<uint32_t>(i);
                fnBuilder.declareVariable(method.params[i].lexeme, paramNode);
                
                if (method.defaultExprs[i]) {
                    IRNode* isUninit = fnGraph.createValueNode(IROp::IsUninit);
                    isUninit->setControl(fnBuilder.currentControl);
                    isUninit->addData(paramNode);
                    
                    IRNode* ifNode = fnGraph.createNode(IROp::If);
                    ifNode->setControl(fnBuilder.currentControl);
                    ifNode->addData(isUninit);
                    
                    IRNode* ifTrue = fnGraph.createNode(IROp::IfTrue);
                    ifTrue->setControl(ifNode);
                    
                    IRNode* ifFalse = fnGraph.createNode(IROp::IfFalse);
                    ifFalse->setControl(ifNode);
                    
                    fnBuilder.currentControl = ifTrue;
                    method.defaultExprs[i]->accept(fnBuilder);
                    IRNode* defVal = fnBuilder.lastValue;
                    IRNode* trueCtrl = fnBuilder.currentControl;
                    
                    IRNode* merge = fnGraph.createNode(IROp::Merge);
                    merge->addData(trueCtrl);
                    merge->addData(ifFalse);
                    
                    IRNode* phi = fnGraph.createValueNode(IROp::Phi);
                    phi->setControl(merge);
                    phi->addData(defVal);
                    phi->addData(paramNode);
                    phi->name = method.params[i].lexeme;
                    
                    fnBuilder.currentControl = merge;
                    fnBuilder.writeVariable(method.params[i].lexeme, phi);
                }
            }
            
            fnBuilder.build(method.body.get());
            
            IROptimizer::optimize(&fnGraph);
            RegisterAllocator::allocate(&fnGraph);
            
            for (auto& target : fnBuilder.upvalueTargets) {
                if (target.isLocal && target.localNode) {
                    fnDef->upvalues[target.index].index = target.localNode->physicalReg;
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

            IRNode* methodNode = graph->createNode(IROp::Method);
            methodNode->setControl(currentControl);
            methodNode->addData(classNode);
            methodNode->addData(methodClosure);
            methodNode->name = method.name.lexeme;
            currentControl = methodNode;
        } else {
            IRNode* methodClosure = graph->createValueNode(IROp::Closure);
            methodClosure->setControl(currentControl);
            methodClosure->name = std::to_string(method.fnIdx);

            IRNode* methodNode = graph->createNode(IROp::Method);
            methodNode->setControl(currentControl);
            methodNode->addData(classNode);
            methodNode->addData(methodClosure);
            methodNode->name = method.name.lexeme;
            currentControl = methodNode;
        }
    }
        
    declareVariable(expr->name.lexeme, classNode);
    lastValue = classNode;
}

void IRBuilder::visitNamespaceDecl(NamespaceDecl* expr) {
    IRNode* nsNode = graph->createValueNode(IROp::BuildNamespace);
    nsNode->setControl(currentControl);
    nsNode->name = expr->name.lexeme;
    currentControl = nsNode;
    declareVariable(expr->name.lexeme, nsNode);
    lastValue = nsNode;
}
    
void IRBuilder::visitDotAccess(DotAccess* expr) {
    expr->object->accept(*this);
    IRNode* objNode = lastValue;
        
    IRNode* node = graph->createValueNode(IROp::GetProperty);
    node->setControl(currentControl);
    node->addData(objNode);
    node->name = expr->field.lexeme;
    currentControl = node;
    lastValue = node;
}

void IRBuilder::visitDotAssign(DotAssign* expr) {
    expr->object->accept(*this);
    IRNode* objNode = lastValue;
        
    expr->value->accept(*this);
    IRNode* valNode = lastValue;
        
    IRNode* node = graph->createValueNode(IROp::SetProperty);
    node->setControl(currentControl);
    node->addData(objNode);
    node->addData(valNode);
    node->name = expr->field.lexeme;
    currentControl = node;
    lastValue = valNode;
}

void IRBuilder::visitMethodCallExpr(MethodCallExpr* expr) {
    expr->object->accept(*this);
    IRNode* objNode = lastValue;
        
    std::vector<IRNode*> argNodes;
    for (auto& arg : expr->arguments) {
        arg->accept(*this);
        argNodes.push_back(lastValue);
    }
        
    IRNode* invokeNode = graph->createValueNode(IROp::Invoke);
    invokeNode->setControl(currentControl);
    invokeNode->addData(objNode);
    for (auto* arg : argNodes) {
        invokeNode->addData(arg);
    }
    invokeNode->payload1 = static_cast<uint32_t>(argNodes.size());
    invokeNode->name = expr->method.lexeme;
        
    currentControl = invokeNode;
    lastValue = invokeNode;
}

void IRBuilder::visitSuperExpr(SuperExpr*) {
    IRNode* node = graph->createValueNode(IROp::GetSuper);
    node->setControl(currentControl);
    lastValue = node;
}

void IRBuilder::visitSelfExpr(SelfExpr*) {
    IRNode* node = graph->createValueNode(IROp::GetSelf);
    node->setControl(currentControl);
    lastValue = node;
}

void IRBuilder::visitDestructAssign(DestructAssign* expr) {
    std::vector<std::tuple<std::string, ScopeModifier, bool>> boundVars;
    collectPatternVars(expr->pattern.get(), boundVars);
    
    std::string firstStateVar = "";
    if (expr->isState) {
        for (const auto& varTuple : boundVars) {
            if (std::get<0>(varTuple) != "_") {
                firstStateVar = std::get<0>(varTuple);
                break;
            }
        }
    }
    
    IRNode* skipMerge = nullptr;
    if (!firstStateVar.empty() && currentFunction) {
        bool found = false;
        for (auto& u : currentFunction->upvalues) {
            if (u.name == firstStateVar && u.isExplicitState) {
                found = true; break;
            }
        }
        if (!found) {
            CompiledFunction::UpvalueInfo uv;
            uv.name = firstStateVar;
            uv.isLocal = false;
            uv.index = 0;
            uv.isRef = false;
            uv.isGlobal = false;
            uv.isExplicitState = true;
            uv.isRefParam = false;
            currentFunction->upvalues.push_back(uv);
        }
        
        IRNode* getVal = readVariable(firstStateVar);
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
        
        for (const auto& varTuple : boundVars) {
            std::string name = std::get<0>(varTuple);
            if (name != "_") {
                for (auto& u : currentFunction->upvalues) {
                    if (u.name == name && u.isExplicitState) {
                        u.name = "<hidden_state_" + name + ">";
                    }
                }
            }
        }
    }
    
    expr->value->accept(*this);
    IRNode* valNode = lastValue;
    
    if (!firstStateVar.empty() && currentFunction) {
        for (const auto& varTuple : boundVars) {
            std::string name = std::get<0>(varTuple);
            if (name != "_") {
                for (auto& u : currentFunction->upvalues) {
                    if (u.name == "<hidden_state_" + name + ">" && u.isExplicitState) {
                        u.name = name;
                    }
                }
            }
        }
    }
    
    IRNode* failMerge = graph->createNode(IROp::Merge);
    
    ScopeModifier mod = ScopeModifier::None;
    if (expr->isLocal) mod = ScopeModifier::Local;
    else if (expr->isRef) mod = ScopeModifier::Ref;
    else if (expr->isState) mod = ScopeModifier::State;

    buildPatternMatch(expr->pattern.get(), valNode, failMerge, mod, expr->isConst);
    
    if (!failMerge->dataInputs.empty()) {
        IRNode* throwNode = graph->createNode(IROp::Throw);
        throwNode->setControl(failMerge);
        IRNode* errStr = graph->createConstant(Value("TypeError: Destructuring pattern match failed."));
        errStr->setControl(failMerge);
        throwNode->addData(errStr);
    }
    
    if (skipMerge) {
        skipMerge->addData(currentControl);
        currentControl = skipMerge;
        
        IRNode* phi = graph->createValueNode(IROp::Phi);
        phi->setControl(skipMerge);
        IRNode* noneNode = graph->createConstant(Value::none());
        noneNode->setControl(skipMerge);
        phi->addData(noneNode);
        phi->addData(valNode);
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
    envStack.emplace_back(); // Scope for comprehension
    
    IRNode* listNode = graph->createValueNode(IROp::ListInit);
    listNode->setControl(currentControl);
    currentControl = listNode;
    
    buildCompClause(expr, 0, listNode);
    
    IRNode* endNode = graph->createValueNode(IROp::ListCompEnd);
    endNode->setControl(currentControl);
    endNode->addData(listNode);
    currentControl = endNode;
    lastValue = endNode;
    
    envStack.pop_back();
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
    throw std::runtime_error("IRBuilder: Slice expression should be handled by visitIndexAccess.");
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

void IRBuilder::visitMatchExpr(MatchExpr* expr) {
    expr->subject->accept(*this);
    IRNode* subjectNode = lastValue;
    
    IRNode* endMerge = graph->createNode(IROp::Merge);
    IRNode* resultPhi = graph->createValueNode(IROp::Phi);
    resultPhi->setControl(endMerge);
    
    IRNode* currentFailControl = currentControl;
    
    for (auto& branch : expr->branches) {
        currentControl = currentFailControl;
        envStack.emplace_back(); // Scope for branch
        
        IRNode* branchSuccessMerge = graph->createNode(IROp::Merge);
        IRNode* nextBranchMerge = graph->createNode(IROp::Merge);
        
        for (auto& pat : branch.patterns) {
            IRNode* patFailMerge = graph->createNode(IROp::Merge);
            
            buildPatternMatch(pat.get(), subjectNode, patFailMerge, ScopeModifier::Local, false);
            
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
        
        envStack.pop_back();
        currentFailControl = nextBranchMerge;
    }
    
    currentControl = currentFailControl;
    IRNode* noneNode = graph->createConstant(Value::none());
    noneNode->setControl(currentControl);
    endMerge->addData(currentControl);
    resultPhi->addData(noneNode);
    
    currentControl = endMerge;
    lastValue = resultPhi;
}

} // namespace regvm
} // namespace jc
