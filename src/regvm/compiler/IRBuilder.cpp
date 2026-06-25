#include "IRBuilder.h"

namespace jc {
namespace regvm {

IRNode* IRBuilder::readVariable(const std::string& name) {
    for (int i = static_cast<int>(envStack.size()) - 1; i >= 0; --i) {
        auto it = envStack[i].find(name);
        if (it != envStack[i].end()) return it->second;
    }
    // 如果没找到，生成一个 GetGlobal 节点
    IRNode* node = graph->createValueNode(IROp::GetGlobal);
    node->name = name;
    node->setControl(currentControl);
    return node;
}

void IRBuilder::writeVariable(const std::string& name, IRNode* value) {
    if (envStack.empty()) return;
    // 查找变量在哪个作用域定义的
    for (int i = static_cast<int>(envStack.size()) - 1; i >= 0; --i) {
        auto it = envStack[i].find(name);
        if (it != envStack[i].end()) {
            envStack[i][name] = value;
            return;
        }
    }
    // 如果都没找到，说明是全局变量赋值
    IRNode* node = graph->createNode(IROp::SetGlobal);
    node->setControl(currentControl);
    node->addData(value);
    node->name = name;
    currentControl = node;
}

void IRBuilder::declareVariable(const std::string& name, IRNode* value) {
    if (envStack.empty()) return;
    envStack.back()[name] = value;
}

IRBuilder::IRBuilder(IRGraph* graph) : graph(graph), currentControl(nullptr), lastValue(nullptr) {
    envStack.emplace_back(); // 压入顶层作用域
}

void IRBuilder::buildPatternMatch(Pattern* pat, IRNode* valNode, IRNode* failMerge) {
    if (auto* vp = dynamic_cast<VariablePattern*>(pat)) {
        if (vp->name.lexeme != "_") {
            if (vp->modifier == ScopeModifier::Local) {
                declareVariable(vp->name.lexeme, valNode);
            } else {
                writeVariable(vp->name.lexeme, valNode);
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
        
        for (size_t i = 0; i < lp->elements.size(); ++i) {
            if (dynamic_cast<RestPattern*>(lp->elements[i].get())) continue;
            IRNode* idxNode = graph->createConstant(Value(static_cast<double>(i)));
            idxNode->setControl(currentControl);
            
            IRNode* elemNode = graph->createValueNode(IROp::IndexGet);
            elemNode->setControl(currentControl);
            elemNode->addData(valNode);
            elemNode->addData(idxNode);
            elemNode->payload1 = 1;
            
            buildPatternMatch(lp->elements[i].get(), elemNode, failMerge);
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
            
            buildPatternMatch(entry.second.get(), elemNode, failMerge);
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
    buildPatternMatch(clause.pattern.get(), nextNode, failMerge);
    
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
    } else {
        try { val = Value(std::stod(expr->value)); } 
        catch(...) { val = Value::none(); }
    }
    lastValue = graph->createConstant(val);
    lastValue->setControl(currentControl);
}

void IRBuilder::visitBinary(Binary* expr) {
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
        case TokenType::EQUAL_EQUAL: op = IROp::Eq; break;
        case TokenType::BANG_EQUAL: op = IROp::Neq; break;
        case TokenType::LESS: op = IROp::Lt; break;
        case TokenType::LESS_EQUAL: op = IROp::Le; break;
        case TokenType::GREATER: op = IROp::Gt; break;
        case TokenType::GREATER_EQUAL: op = IROp::Ge; break;
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
    expr->value->accept(*this);
    IRNode* valNode = lastValue;
    
    if (expr->isLocal) {
        declareVariable(expr->name.lexeme, valNode);
    } else {
        writeVariable(expr->name.lexeme, valNode);
    }
    lastValue = valNode;
}

void IRBuilder::visitBlock(Block* expr) {
    envStack.emplace_back(); // 进入新作用域
    for (auto& stmt : expr->statements) {
        stmt->accept(*this);
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
    expr->thenBranch->accept(*this);
    IRNode* thenControl = currentControl;
    IRNode* thenVal = lastValue;
    auto thenEnv = envStack.back();

    // 5. 编译 False 分支
    envStack.back() = baseEnv; // 恢复基础环境
    currentControl = ifFalse;
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

    // 6. 创建 Merge 节点汇合控制流
    IRNode* mergeNode = graph->createNode(IROp::Merge);
    mergeNode->addData(thenControl);
    mergeNode->addData(elseControl);
    currentControl = mergeNode;

    // 7. 合并环境 (生成 Phi 节点)
    envStack.back() = baseEnv; // 准备合并后的新环境
    std::unordered_set<std::string> modifiedVars;
    for (const auto& pair : thenEnv) {
        if (baseEnv[pair.first] != pair.second) modifiedVars.insert(pair.first);
    }
    for (const auto& pair : elseEnv) {
        if (baseEnv[pair.first] != pair.second) modifiedVars.insert(pair.first);
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
    envStack.back() = exitEnv;
    
    loopStack.pop_back();
    
    lastValue = graph->createConstant(Value::none());
    lastValue->setControl(currentControl);
}

void IRBuilder::visitForExpr(ForExpr* expr) {
    envStack.emplace_back();
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
    envStack.back() = exitEnv;
    
    loopStack.pop_back();
    envStack.pop_back();
    
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
    expr->object->accept(*this);
    IRNode* objNode = lastValue;
        
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
    IRNode* objNode = lastValue;
        
    std::vector<IRNode*> indices;
    for (auto& idx : expr->indexChain[0]) {
        idx->accept(*this);
        indices.push_back(lastValue);
    }
        
    expr->value->accept(*this);
    IRNode* valNode = lastValue;
        
    IRNode* node = graph->createValueNode(IROp::IndexSet);
    node->setControl(currentControl);
    node->addData(objNode);
    for (auto* idx : indices) node->addData(idx);
    node->addData(valNode);
    node->payload1 = static_cast<uint32_t>(indices.size());
        
    currentControl = node;
    lastValue = valNode;
}
    
void IRBuilder::visitLocalDecl(LocalDecl* expr) {
    IRNode* noneNode = graph->createConstant(Value::none());
    noneNode->setControl(currentControl);
    declareVariable(expr->name.lexeme, noneNode);
    lastValue = noneNode;
}

void IRBuilder::visitRefDecl(RefDecl* expr) {
    IRNode* noneNode = graph->createConstant(Value::none());
    noneNode->setControl(currentControl);
    declareVariable(expr->name.lexeme, noneNode);
    lastValue = noneNode;
}

void IRBuilder::visitStateDecl(StateDecl* expr) {
    IRNode* noneNode = graph->createConstant(Value::none());
    noneNode->setControl(currentControl);
    declareVariable(expr->name.lexeme, noneNode);
    lastValue = noneNode;
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
        idx->object->accept(*this);
        objNode = lastValue;
        for (auto& i : idx->indices) {
            i->accept(*this);
            indices.push_back(lastValue);
        }
        
        targetVal = graph->createValueNode(IROp::IndexGet);
        targetVal->setControl(currentControl);
        targetVal->addData(objNode);
        for (auto* i : indices) targetVal->addData(i);
        targetVal->payload1 = static_cast<uint32_t>(indices.size());
        currentControl = targetVal;
    } else {
        throw std::runtime_error("IRBuilder: Unsupported compound assignment target.");
    }

    expr->value->accept(*this);
    IRNode* rightVal = lastValue;

    IROp op = IROp::Add;
    switch (expr->op) {
        case TokenType::PLUS: op = IROp::Add; break;
        case TokenType::MINUS: op = IROp::Sub; break;
        case TokenType::STAR: op = IROp::Mul; break;
        case TokenType::SLASH: op = IROp::Div; break;
        case TokenType::PERCENT: op = IROp::Mod; break;
        case TokenType::CARET: op = IROp::Pow; break;
        case TokenType::BACKSLASH: op = IROp::LeftDivide; break;
        case TokenType::BIT_AND: op = IROp::BitAnd; break;
        case TokenType::BIT_OR: op = IROp::BitOr; break;
        case TokenType::BIT_XOR: op = IROp::BitXor; break;
        case TokenType::SHIFT_LEFT: op = IROp::Shl; break;
        case TokenType::SHIFT_RIGHT: op = IROp::Shr; break;
        default: throw std::runtime_error("IRBuilder: Unsupported compound operator.");
    }

    IRNode* opNode = graph->createValueNode(op);
    opNode->setControl(currentControl);
    opNode->addData(targetVal);
    opNode->addData(rightVal);
    currentControl = opNode;

    if (auto* var = dynamic_cast<Variable*>(expr->target.get())) {
        if (expr->isLocal) declareVariable(var->name.lexeme, opNode);
        else writeVariable(var->name.lexeme, opNode);
    } else if (dynamic_cast<DotAccess*>(expr->target.get())) {
        IRNode* setProp = graph->createValueNode(IROp::SetProperty);
        setProp->setControl(currentControl);
        setProp->addData(objNode);
        setProp->addData(opNode);
        setProp->name = propName;
        currentControl = setProp;
    } else if (dynamic_cast<IndexAccess*>(expr->target.get())) {
        IRNode* setIdx = graph->createValueNode(IROp::IndexSet);
        setIdx->setControl(currentControl);
        setIdx->addData(objNode);
        for (auto* i : indices) setIdx->addData(i);
        setIdx->addData(opNode);
        setIdx->payload1 = static_cast<uint32_t>(indices.size());
        currentControl = setIdx;
    }

    lastValue = opNode;
}

void IRBuilder::visitLambdaExpr(LambdaExpr* expr) {
    IRNode* closureNode = graph->createValueNode(IROp::Closure);
    closureNode->setControl(currentControl);
    closureNode->name = expr->name;
    currentControl = closureNode;
    lastValue = closureNode;
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
    envStack.emplace_back();
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
    buildPatternMatch(expr->pattern.get(), nextNode, failMerge);
    
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
    envStack.back() = exitEnv;
    
    loopStack.pop_back();
    envStack.pop_back();
    
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
    buildPatternMatch(expr->catchPattern.get(), errVal, failMerge);

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
        IRNode* methodClosure = graph->createValueNode(IROp::Closure);
        methodClosure->setControl(currentControl);
        methodClosure->name = method.name.lexeme;

        IRNode* methodNode = graph->createNode(IROp::Method);
        methodNode->setControl(currentControl);
        methodNode->addData(classNode);
        methodNode->addData(methodClosure);
        methodNode->name = method.name.lexeme;
        currentControl = methodNode;
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
    expr->value->accept(*this);
    IRNode* valNode = lastValue;
    
    IRNode* failMerge = graph->createNode(IROp::Merge);
    
    buildPatternMatch(expr->pattern.get(), valNode, failMerge);
    
    if (!failMerge->dataInputs.empty()) {
        IRNode* throwNode = graph->createNode(IROp::Throw);
        throwNode->setControl(failMerge);
        IRNode* errStr = graph->createConstant(Value("TypeError: Destructuring pattern match failed."));
        errStr->setControl(failMerge);
        throwNode->addData(errStr);
    }
    
    lastValue = valNode;
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

void IRBuilder::visitSliceExpr(SliceExpr* expr) {
    throw std::runtime_error("IRBuilder: Slice expression should be handled by visitIndexAccess.");
}
    
void IRBuilder::visitSequenceExpr(SequenceExpr* expr) {
    for (auto& e : expr->expressions) {
        e->accept(*this);
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
            
            buildPatternMatch(pat.get(), subjectNode, patFailMerge);
            
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
