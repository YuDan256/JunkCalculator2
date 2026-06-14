#include "Compiler.h"
#include "../vm/VM.h"
#include <functional>

namespace jc {

    void Compiler::emit(OpCode op, int line) {
        chunk()->write(op, line);
        if (line > 0) lastLine = line; // ★ 新增同步点
    }
    void Compiler::emit(uint8_t byte, int line) {
        chunk()->write(byte, line);
        if (line > 0) lastLine = line; // ★ 新增同步点
    }
    void Compiler::emit16(uint16_t val, int line) {
        chunk()->write16(val, line);
        if (line > 0) lastLine = line; // ★ 新增同步点
    }
    uint16_t Compiler::makeConstant(const Value& val) { return chunk()->addConstant(val); }
    uint16_t Compiler::identifierConstant(const std::string& name) { return makeConstant(Value(name)); }
    void Compiler::compileNode(Expr* expr) { 
        bool isCall = dynamic_cast<Call*>(expr) || dynamic_cast<InvokeExpr*>(expr) || dynamic_cast<MethodCallExpr*>(expr) || dynamic_cast<GroupingExpr*>(expr);
        bool prevTail = inTailPosition;
        if (!isCall) inTailPosition = false;
        expr->accept(*this); 
        inTailPosition = prevTail;
    }

    void Compiler::initCompiler(CompiledFunction* fn) {
        CompilerState state;
        state.function = fn;
        state.scopeDepth = 0;
        state.maxLocals = 0;  // ★ 初始化峰值
        stateStack.push_back(state);
    }

    void Compiler::beginScope() { current().scopeDepth++; }

    void Compiler::endScope() {
        current().scopeDepth--;
        
        // 1. 遍历所有变量，将超出作用域的变量匿名化
        for (auto& local : current().locals) {
            if (local.depth > current().scopeDepth) {
                local.name = ""; 
            }
        }
        
        // 2. 从后往前清理可以安全回收的 slot
        while (!current().locals.empty()) {
            auto& back = current().locals.back();
            // 如果末尾的变量是匿名的（说明它超出了作用域），且没有被捕获，就可以安全弹出
            if (back.name == "" && !back.isCaptured) {
                current().locals.pop_back();
            } else {
                break;
            }
        }
    }

    void Compiler::emitDefaultPreamble(
        const std::vector<std::shared_ptr<Expr>>& defaultExprs,
        int paramCount)
    {
        for (int i = 0; i < paramCount; ++i) {
            if (i < static_cast<int>(defaultExprs.size()) && defaultExprs[i]) {
                if (current().locals[i].isRefParam) {
                    emit(OpCode::OP_GET_REF_PARAM, lastLine);
                    emit16(static_cast<uint16_t>(current().locals[i].refParamIndex), lastLine);
                } else {
                    emit(OpCode::OP_GET_LOCAL, lastLine);
                    emit16(static_cast<uint16_t>(i), lastLine);
                }
                emit(OpCode::OP_NONE, lastLine);
                emit(OpCode::OP_EQUAL, lastLine);

                int skipJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
                emit(OpCode::OP_POP, lastLine);

                compileNode(defaultExprs[i].get());
                if (current().locals[i].isRefParam) {
                    emit(OpCode::OP_SET_REF_PARAM, lastLine);
                    emit16(static_cast<uint16_t>(current().locals[i].refParamIndex), lastLine);
                } else {
                    emit(OpCode::OP_SET_LOCAL, lastLine);
                    emit16(static_cast<uint16_t>(i), lastLine);
                }
                emit(OpCode::OP_POP, lastLine);

                int endJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                chunk()->patchJump(skipJump);
                emit(OpCode::OP_POP, lastLine);
                chunk()->patchJump(endJump);
            }
        }
    }

    void Compiler::emitStoreTarget(Expr* target, bool isConst) {
        if (auto* var = dynamic_cast<Variable*>(target)) {
            const std::string& name = var->name.lexeme;
            int slot = resolveLocal(name);

            if (slot != -1 && current().captures.count(name) == 0) {
                if (current().locals[slot].isRefParam) {
                    emit(OpCode::OP_SET_REF_PARAM, lastLine);
                    emit16(static_cast<uint16_t>(current().locals[slot].refParamIndex), lastLine);
                } else {
                    emit(OpCode::OP_SET_LOCAL, lastLine);
                    emit16(static_cast<uint16_t>(slot), lastLine);
                }
            }
            else {
                int upvalue = resolveUpvalue(name);
                if (upvalue != -1) {
                    emit(OpCode::OP_SET_UPVALUE, lastLine);
                    emit16(static_cast<uint16_t>(upvalue), lastLine);
                }
                else {
                    uint16_t nameIdx = identifierConstant(name);
                    auto it = current().captures.find(name);
                    if (it != current().captures.end() && it->second.type == CaptureType::Ref) {
                        emit(OpCode::OP_SET_GLOBAL_REF, lastLine);
                    } else if (isConst) {
                        emit(OpCode::OP_DEFINE_CONST_GLOBAL, lastLine);
                    } else {
                        emit(OpCode::OP_SET_GLOBAL, lastLine);
                    }
                    emit16(nameIdx, lastLine);
                }
            }
            return;
        }

        addLocal("", current().scopeDepth);
        int valTmpIdx = static_cast<int>(current().locals.size()) - 1;
        emit(OpCode::OP_SET_LOCAL, lastLine);
        emit16(static_cast<uint16_t>(valTmpIdx), lastLine);

        if (auto* dot = dynamic_cast<DotAccess*>(target)) {
            compileNode(dot->object.get());

            emit(OpCode::OP_GET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(valTmpIdx), lastLine);

            uint16_t fieldIdx = identifierConstant(dot->field.lexeme);
            emit(OpCode::OP_SET_PROPERTY, lastLine);
            emit16(fieldIdx, lastLine);
            emit16(chunk()->addInlineCache(), lastLine);

            emit(OpCode::OP_POP, lastLine);
            current().locals.pop_back();
            return;
        }

        if (auto* idx = dynamic_cast<IndexAccess*>(target)) {
            for (auto& i : idx->indices) {
                if (dynamic_cast<SliceExpr*>(i.get()))
                    throw std::runtime_error(
                        "Compiler Error: Slice compound assignment not supported in VM.");
            }
            uint8_t dimCount = static_cast<uint8_t>(idx->indices.size());

            compileNode(idx->object.get());
            for (auto& i : idx->indices)
                compileNode(i.get());

            emit(OpCode::OP_GET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(valTmpIdx), lastLine);

            emit(OpCode::OP_INDEX_SET, lastLine);
            emit(dimCount, lastLine);

            emitStoreTarget(idx->object.get());
            emit(OpCode::OP_POP, lastLine);
            emit(OpCode::OP_POP, lastLine);

            current().locals.pop_back();
            return;
        }

        // 如果不是支持的左值，直接丢弃
        emit(OpCode::OP_POP, lastLine);
        current().locals.pop_back();
    }

    void Compiler::compileCompClause(ListCompExpr* expr, size_t clauseIdx) {
        if (clauseIdx >= expr->clauses.size()) {
            uint16_t depth = static_cast<uint16_t>(2 * expr->clauses.size());

            compileNode(expr->valueExpr.get());
            emit(OpCode::OP_LIST_APPEND, lastLine);
            emit16(depth, lastLine);
            return;
        }

        auto& clause = expr->clauses[clauseIdx];
        compileNode(clause.iterable.get());
        emit(OpCode::OP_ITER_INIT, lastLine);
        emit(static_cast<uint8_t>(clause.isDestruct() ? 1 : 0), lastLine);

        int loopStart = static_cast<int>(chunk()->code.size());
        int exitJump = chunk()->emitJump(OpCode::OP_ITER_NEXT, lastLine);

        int skipMatchJump = -1;
        if (clause.isDestruct()) {
            addLocal("<comp_val>", current().scopeDepth);
            int valSlot = static_cast<int>(current().locals.size()) - 1;
            emit(OpCode::OP_SET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(valSlot), lastLine);
            emit(OpCode::OP_POP, lastLine);

            std::vector<std::tuple<std::string, ScopeModifier, bool>> boundVars;
            collectPatternVars(clause.pattern.get(), boundVars);
            for (const auto& varTuple : boundVars) {
                const std::string& name = std::get<0>(varTuple);
                if (name == "_") continue;
                int slot = resolveLocal(name);
                if (slot == -1 || current().locals[slot].depth < current().scopeDepth) {
                    addLocal(name, current().scopeDepth);
                }
            }

            std::vector<int> failJumps;
            compilePatternMatch(clause.pattern.get(), valSlot, failJumps, false);

            if (!failJumps.empty()) {
                int successJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                for (int fj : failJumps) chunk()->patchJump(fj);
                emit(OpCode::OP_POP, lastLine); // pop boolean
                skipMatchJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                chunk()->patchJump(successJump);
            }
        }
        else {
            const std::string& varName = clause.varName.lexeme;
            int slot = resolveLocal(varName);
            if (slot == -1 || current().locals[slot].depth < current().scopeDepth) {
                addLocal(varName, current().scopeDepth);
                slot = resolveLocal(varName);
            }
            emit(OpCode::OP_SET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(slot), lastLine);
            emit(OpCode::OP_POP, lastLine);
        }

        std::vector<int> condJumps;
        for (auto& cond : clause.conditions) {
            compileNode(cond.get());
            condJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine));
            emit(OpCode::OP_POP, lastLine); // pop true boolean
        }

        compileCompClause(expr, clauseIdx + 1);

        if (!condJumps.empty()) {
            int successSkip = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
            for (int cj : condJumps) {
                chunk()->patchJump(cj);
            }
            emit(OpCode::OP_POP, lastLine); // pop false boolean
            chunk()->patchJump(successSkip);
        }

        if (skipMatchJump != -1) {
            chunk()->patchJump(skipMatchJump);
        }

        chunk()->emitLoop(loopStart, lastLine);
        chunk()->patchJump(exitJump);

        emit(OpCode::OP_POP, lastLine);
        emit(OpCode::OP_POP, lastLine);
    }

    void Compiler::addLocal(const std::string& name, int depth, bool isConst) {
        current().locals.push_back({ name, depth, false, isConst });
        // ★ 跟踪峰值容量
        if (static_cast<int>(current().locals.size()) > current().maxLocals) {
            current().maxLocals = static_cast<int>(current().locals.size());
        }
    }

    void Compiler::declareVariable(const std::string& name) {
        if (current().scopeDepth == 0) return;
        addLocal(name, current().scopeDepth);
    }

    int Compiler::resolveLocal(const std::string& name) {
        auto& locals = current().locals;
        for (int i = static_cast<int>(locals.size()) - 1; i >= 0; --i) {
            if (locals[i].name == name) return i;
        }
        return -1;
    }

    void Compiler::beginLoop(int loopStart) {
        loopStack.push_back({ loopStart, {}, {}, current().scopeDepth, current().tryDepth });
    }

    void Compiler::endLoop() {
        loopStack.pop_back();
    }

    void Compiler::emitBreakJumps() {
        for (int offset : loopStack.back().breakJumps) {
            chunk()->patchJump(offset);
        }
    }

    Chunk Compiler::compile(Expr* ast, const std::string& sourceFile) {
        currentSourceFile = sourceFile; // 记住
        auto mainFn = std::make_shared<CompiledFunction>();
        mainFn->name = "<script>";
        mainFn->sourceFile = sourceFile; // ★ 打上文件烙印

        // ★ 核心修复：不要把顶层 <script> 函数塞进 compiledFunctions！
        // 否则 REPL 每敲一行代码，它的 AST、字节码和常量池（包含变量名的 ObjString）
        // 就会被永久驻留在内存中，导致 GC 追踪的对象数量无限增长。
        initCompiler(mainFn.get());
        compileNode(ast);
        emit(OpCode::OP_RETURN, lastLine);
        mainFn->localCount = current().maxLocals;
        topLevelLocalCount = current().maxLocals;
        stateStack.pop_back();
        return mainFn->chunk;
    }

    Chunk Compiler::compileModule(Expr* ast, const std::string& sourceFile, const std::string& moduleName) {
        currentSourceFile = sourceFile;
        
        // ★ 压入一个虚拟的全局状态，使得模块内部的 stateStack.size() > 1
        // 这样模块顶层的变量就会被正确识别为 Auto-locals (depth 0) 而不是全局变量
        CompilerState dummyGlobal;
        stateStack.push_back(dummyGlobal);
        
        auto fn = std::make_shared<CompiledFunction>();
        fn->name = "<module " + moduleName + ">";
        fn->sourceFile = sourceFile;
        
        initCompiler(fn.get());
        beginScope(); // depth 1 (用于隔离 local 声明的私有变量)
        
        if (auto* block = dynamic_cast<Block*>(ast)) {
            for (size_t i = 0; i < block->statements.size(); ++i) {
                compileNode(block->statements[i].get());
                emit(OpCode::OP_POP, lastLine);
            }
        } else {
            compileNode(ast);
            emit(OpCode::OP_POP, lastLine);
        }
        
        int count = 0;
        for (auto& local : current().locals) {
            // 仅导出 depth == 0 的 Auto-locals，完美实现 local 关键字的私有化封装！
            if (local.depth == 0 && !local.name.empty() && local.name[0] != '<') {
                uint16_t keyIdx = identifierConstant(local.name);
                emit(OpCode::OP_CONSTANT, lastLine);
                emit16(keyIdx, lastLine);

                int slot = resolveLocal(local.name);
                chunk()->emitConstant(Value(static_cast<double>(slot)), lastLine);

                bool isConst = local.isConst;
                chunk()->emitConstant(Value(isConst ? 1.0 : 0.0), lastLine);

                count++;
            }
        }
        
        uint16_t nsNameIdx = identifierConstant(moduleName);
        emit(OpCode::OP_BUILD_NAMESPACE, lastLine);
        emit16(nsNameIdx, lastLine);
        emit16(static_cast<uint16_t>(count), lastLine);
        emit(OpCode::OP_RETURN, lastLine);
        
        topLevelLocalCount = current().maxLocals;
        endScope();
        stateStack.pop_back(); // pop module state
        stateStack.pop_back(); // pop dummy global state
        return fn->chunk;
    }


    void Compiler::visitLiteral(Literal* expr) {
        if (expr->isKeyword) {
            if (expr->value == "true") emit(OpCode::OP_TRUE, lastLine);
            else if (expr->value == "false") emit(OpCode::OP_FALSE, lastLine);
            else if (expr->value == "none") emit(OpCode::OP_NONE, lastLine);
        }
        else if (expr->isString) {
            chunk()->emitConstant(Value(expr->value), lastLine);
        }
        else if (expr->isImaginary) {
            const std::string& s = expr->value;
            double imagPart = 0.0;
            if (s.length() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B' || s[1] == 'o' || s[1] == 'O')) {
                int base = 10;
                if (s[1] == 'x' || s[1] == 'X') base = 16;
                else if (s[1] == 'b' || s[1] == 'B') base = 2;
                else if (s[1] == 'o' || s[1] == 'O') base = 8;
                std::string numPart = s.substr(2); // 剔除前缀
                try {
                    imagPart = BaseNum::fromString(numPart, base).getValue().toDouble();
                } catch (...) {
                    throw std::runtime_error("Compiler Error: Invalid imaginary literal '" + s + "'.");
                }
            } else {
                imagPart = std::stod(s);
            }
            chunk()->emitConstant(Value(Complex(0.0, imagPart)), lastLine);
        }
        else {
            const std::string& s = expr->value;
            if (s.length() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B' || s[1] == 'o' || s[1] == 'O')) {
                int base = 10;
                if (s[1] == 'x' || s[1] == 'X') base = 16;
                else if (s[1] == 'b' || s[1] == 'B') base = 2;
                else if (s[1] == 'o' || s[1] == 'O') base = 8;
                std::string numPart = s.substr(2);
                try {
                    chunk()->emitConstant(Value(BaseNum::fromString(numPart, base).getValue()), lastLine);
                } catch (...) {
                    throw std::runtime_error("Compiler Error: Invalid integer literal '" + s + "'.");
                }
            }
            else if (s.find('.') == std::string::npos &&
                s.find('e') == std::string::npos &&
                s.find('E') == std::string::npos) {
                try { chunk()->emitConstant(Value(BigInt(s)), lastLine); }
                catch (...) { chunk()->emitConstant(Value(std::stod(s)), lastLine); }
            }
            else {
                chunk()->emitConstant(Value(std::stod(s)), lastLine);
            }
        }
        return;
    }

    void Compiler::visitVariable(Variable* expr) {
        lastLine = expr->name.line;
        const std::string& name = expr->name.lexeme;
        int slot = resolveLocal(name);
        if (slot != -1 && current().captures.count(name) == 0) {
            if (current().locals[slot].isRefParam) {
                emit(OpCode::OP_GET_REF_PARAM, expr->name.line);
                emit16(static_cast<uint16_t>(current().locals[slot].refParamIndex), expr->name.line);
            } else {
                emit(OpCode::OP_GET_LOCAL, expr->name.line);
                emit16(static_cast<uint16_t>(slot), expr->name.line);
            }
        }
        else {
            int upvalue = resolveUpvalue(name);
            if (upvalue != -1) {
                emit(OpCode::OP_GET_UPVALUE, expr->name.line);
                emit16(static_cast<uint16_t>(upvalue), expr->name.line);
            }
            else {
                uint16_t idx = identifierConstant(name);
                emit(OpCode::OP_GET_GLOBAL, expr->name.line);
                emit16(idx, expr->name.line);
            }
        }
        return;
    }

    void Compiler::visitAssign(Assign* expr) {
        lastLine = expr->name.line;
        const std::string& name = expr->name.lexeme;

        int existingSlot = resolveLocal(name);
        if (existingSlot != -1 && current().locals[existingSlot].isConst) {
            if (!expr->isLocal || current().locals[existingSlot].depth == current().scopeDepth) {
                uint16_t msgIdx = identifierConstant("Runtime Error: Cannot modify const variable '" + name + "'.");
                emit(OpCode::OP_CONSTANT, lastLine);
                emit16(msgIdx, lastLine);
                emit(OpCode::OP_THROW, lastLine);
                return;
            }
        }
        auto capIt = current().captures.find(name);
        if (capIt != current().captures.end() && capIt->second.isConst) {
            if (!expr->isLocal) {
                uint16_t msgIdx = identifierConstant("Runtime Error: Cannot modify const variable '" + name + "'.");
                emit(OpCode::OP_CONSTANT, lastLine);
                emit16(msgIdx, lastLine);
                emit(OpCode::OP_THROW, lastLine);
                return;
            }
        }

        // ★ Pre-register ref/state BEFORE compiling RHS so variable reads resolve to upvalue
        if (expr->isLocal) {
            if (current().captures.count(name) > 0) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'local' and 'ref'/'state'.");
        } else if (expr->isRef) {
            if (current().captures.count(name) > 0 && current().captures[name].type != CaptureType::Ref) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'ref' and 'state'.");
            current().captures[name] = {CaptureType::Ref, expr->isConst, false};
        } else if (expr->isState) {
            if (current().captures.count(name) > 0 && current().captures[name].type != CaptureType::State) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'ref' and 'state'.");
            current().captures[name] = {CaptureType::State, expr->isConst, true};
        }

        // ★ Pre-declare local variable if RHS is a Lambda, to support local recursion
        if (dynamic_cast<LambdaExpr*>(expr->value.get())) {
            int slot = resolveLocal(name);
            if (expr->isLocal) {
                if (slot == -1 || current().locals[slot].depth < current().scopeDepth) addLocal(name, current().scopeDepth, expr->isConst);
            } else if (!expr->isRef && !expr->isState && stateStack.size() > 1 && slot == -1 && current().captures.count(name) == 0) {
                addLocal(name, 0, expr->isConst); // Auto-locals go to function scope
            }
        }

        // ★ State initialization: only assign if the upvalue is currently UNINIT
        if (expr->isState) {
            int upvalue = resolveUpvalue(name);
            if (upvalue != -1) {
                emit(OpCode::OP_GET_UPVALUE, expr->name.line);
                emit16(static_cast<uint16_t>(upvalue), expr->name.line);
                emit(OpCode::OP_IS_UNINIT, expr->name.line);
                
                int skipJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, expr->name.line);
                emit(OpCode::OP_POP, expr->name.line); // pop boolean
                
                // ★ Temporarily unregister so RHS can capture the outer variable if it references the same name
                CaptureModifier tempMod = current().captures[name];
                current().captures.erase(name);
                
                compileNode(expr->value.get());
                
                // ★ Re-register
                current().captures[name] = tempMod;

                emit(OpCode::OP_SET_UPVALUE, expr->name.line);
                emit16(static_cast<uint16_t>(upvalue), expr->name.line);
                
                int endJump = chunk()->emitJump(OpCode::OP_JUMP, expr->name.line);
                
                chunk()->patchJump(skipJump);
                emit(OpCode::OP_POP, expr->name.line); // pop boolean
                emit(OpCode::OP_GET_UPVALUE, expr->name.line);
                emit16(static_cast<uint16_t>(upvalue), expr->name.line);
                
                chunk()->patchJump(endJump);
                return;
            }
        }

        compileNode(expr->value.get());

        if (stateStack.size() == 1 && current().scopeDepth == 0) {
            knownGlobals.insert(name);
        }

        int slot = resolveLocal(name);
        int upvalue = -1;

        if (expr->isLocal) {
            if (slot == -1 || current().locals[slot].depth < current().scopeDepth) {
                addLocal(name, current().scopeDepth, expr->isConst);
                slot = resolveLocal(name);
            } else {
                current().locals[slot].isConst = expr->isConst;
            }
        } else if (expr->isRef || expr->isState) {
            upvalue = resolveUpvalue(name);
        } else {
            // ★ Auto-local Write (Shadowing)
            if (stateStack.size() > 1 && slot == -1 && current().captures.count(name) == 0) {
                addLocal(name, 0, expr->isConst); // Auto-locals go to function scope
                slot = resolveLocal(name);
            } else if (slot != -1) {
                current().locals[slot].isConst = expr->isConst;
            }
        }

        if (slot != -1 && current().captures.count(name) == 0) {
            if (current().locals[slot].isRefParam) {
                emit(OpCode::OP_SET_REF_PARAM, expr->name.line);
                emit16(static_cast<uint16_t>(current().locals[slot].refParamIndex), expr->name.line);
            } else {
                emit(OpCode::OP_SET_LOCAL, expr->name.line);
                emit16(static_cast<uint16_t>(slot), expr->name.line);
            }
        }
        else {
            if (upvalue == -1) upvalue = resolveUpvalue(name);
            if (upvalue != -1) {
                emit(OpCode::OP_SET_UPVALUE, expr->name.line);
                emit16(static_cast<uint16_t>(upvalue), expr->name.line);
            }
            else {
                uint16_t idx = identifierConstant(name);
                auto it = current().captures.find(name);
                if (expr->isRef || (it != current().captures.end() && it->second.type == CaptureType::Ref)) {
                    emit(OpCode::OP_SET_GLOBAL_REF, expr->name.line);
                } else if (expr->isConst) {
                    emit(OpCode::OP_DEFINE_CONST_GLOBAL, expr->name.line);
                } else {
                    emit(OpCode::OP_SET_GLOBAL, expr->name.line);
                }
                emit16(idx, expr->name.line);
            }
        }
        return;
    }

    std::optional<Value> Compiler::tryFoldConstant(Expr* expr) {
        if (auto* lit = dynamic_cast<Literal*>(expr)) {
            if (lit->isKeyword) {
                if (lit->value == "true") return Value(true);
                if (lit->value == "false") return Value(false);
                if (lit->value == "none") return Value::none();
            }
            else if (lit->isString) {
                return Value(lit->value);
            }
            else if (lit->isImaginary) {
                const std::string& s = lit->value;
                double imagPart = 0.0;
                if (s.length() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B' || s[1] == 'o' || s[1] == 'O')) {
                    int base = 10;
                    if (s[1] == 'x' || s[1] == 'X') base = 16;
                    else if (s[1] == 'b' || s[1] == 'B') base = 2;
                    else if (s[1] == 'o' || s[1] == 'O') base = 8;
                    std::string numPart = s.substr(2); // 剔除前缀
                    try {
                        imagPart = BaseNum::fromString(numPart, base).getValue().toDouble();
                    } catch (...) {
                        return std::nullopt;
                    }
                } else {
                    imagPart = std::stod(s);
                }
                return Value(Complex(0.0, imagPart));
            }
            else {
                const std::string& s = lit->value;
                if (s.length() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B' || s[1] == 'o' || s[1] == 'O')) {
                    int base = 10;
                    if (s[1] == 'x' || s[1] == 'X') base = 16;
                    else if (s[1] == 'b' || s[1] == 'B') base = 2;
                    else if (s[1] == 'o' || s[1] == 'O') base = 8;
                    std::string numPart = s.substr(2);
                    try {
                        return Value(BaseNum::fromString(numPart, base).getValue());
                    } catch (...) {
                        return std::nullopt;
                    }
                }
                else if (s.find('.') == std::string::npos &&
                    s.find('e') == std::string::npos &&
                    s.find('E') == std::string::npos) {
                    try { return Value(BigInt(s)); }
                    catch (...) { return Value(std::stod(s)); }
                }
                else {
                    return Value(std::stod(s));
                }
            }
        }
        else if (auto* un = dynamic_cast<Unary*>(expr)) {
            auto rightVal = tryFoldConstant(un->right.get());
            if (!rightVal) return std::nullopt;
            try {
                switch (un->op.type) {
                case TokenType::MINUS: return -(*rightVal);
                case TokenType::BANG:  return Value(!rightVal->truthy());
                case TokenType::TILDE: return ~(*rightVal);
                case TokenType::PLUS:  return rightVal;
                default: return std::nullopt;
                }
            }
            catch (...) { return std::nullopt; }
        }
        else if (auto* group = dynamic_cast<GroupingExpr*>(expr)) {
            return tryFoldConstant(group->expression.get());
        }
        else if (auto* bin = dynamic_cast<Binary*>(expr)) {
            if (bin->op.type == TokenType::AND_AND) {
                auto leftVal = tryFoldConstant(bin->left.get());
                if (leftVal && !leftVal->truthy()) return Value(false);
                if (leftVal && leftVal->truthy()) {
                    auto rightVal = tryFoldConstant(bin->right.get());
                    if (rightVal) return Value(rightVal->truthy());
                }
                return std::nullopt;
            }
            if (bin->op.type == TokenType::OR_OR) {
                auto leftVal = tryFoldConstant(bin->left.get());
                if (leftVal && leftVal->truthy()) return Value(true);
                if (leftVal && !leftVal->truthy()) {
                    auto rightVal = tryFoldConstant(bin->right.get());
                    if (rightVal) return Value(rightVal->truthy());
                }
                return std::nullopt;
            }

            auto leftVal = tryFoldConstant(bin->left.get());
            auto rightVal = tryFoldConstant(bin->right.get());
            if (!leftVal || !rightVal) return std::nullopt;

            try {
                switch (bin->op.type) {
                case TokenType::PLUS:          return (*leftVal) + (*rightVal);
                case TokenType::MINUS:         return (*leftVal) - (*rightVal);
                case TokenType::STAR:          return (*leftVal) * (*rightVal);
                case TokenType::SLASH:         return (*leftVal) / (*rightVal);
                case TokenType::PERCENT:       return (*leftVal) % (*rightVal);
                case TokenType::CARET:         return (*leftVal) ^ (*rightVal);
                case TokenType::EQUAL:         return Value(Value::equals(*leftVal, *rightVal));
                case TokenType::BANG_EQUAL:    return Value(!Value::equals(*leftVal, *rightVal));
                case TokenType::BIT_AND:       return (*leftVal) & (*rightVal);
                case TokenType::BIT_OR:        return (*leftVal) | (*rightVal);
                case TokenType::BIT_XOR:       return bitXor(*leftVal, *rightVal);
                case TokenType::SHIFT_LEFT:    return (*leftVal) << (*rightVal);
                case TokenType::SHIFT_RIGHT:   return (*leftVal) >> (*rightVal);
                case TokenType::LESS: {
                    if ((leftVal->isBigInt() || leftVal->isInt32() || leftVal->isBool()) && (rightVal->isBigInt() || rightVal->isInt32() || rightVal->isBool())) return Value(leftVal->asBigInt() < rightVal->asBigInt());
                    if (leftVal->isObjType(ObjType::FRACTION) && rightVal->isObjType(ObjType::FRACTION)) return Value(static_cast<ObjFraction*>(leftVal->asObj())->frac < static_cast<ObjFraction*>(rightVal->asObj())->frac);
                    if (leftVal->isString() && rightVal->isString()) return Value(leftVal->asString() < rightVal->asString());
                    return Value(leftVal->asDouble() < rightVal->asDouble());
                }
                case TokenType::LESS_EQUAL: {
                    if ((leftVal->isBigInt() || leftVal->isInt32() || leftVal->isBool()) && (rightVal->isBigInt() || rightVal->isInt32() || rightVal->isBool())) return Value(leftVal->asBigInt() <= rightVal->asBigInt());
                    if (leftVal->isObjType(ObjType::FRACTION) && rightVal->isObjType(ObjType::FRACTION)) return Value(static_cast<ObjFraction*>(leftVal->asObj())->frac <= static_cast<ObjFraction*>(rightVal->asObj())->frac);
                    if (leftVal->isString() && rightVal->isString()) return Value(leftVal->asString() <= rightVal->asString());
                    return Value(leftVal->asDouble() <= rightVal->asDouble());
                }
                case TokenType::GREATER: {
                    if ((leftVal->isBigInt() || leftVal->isInt32() || leftVal->isBool()) && (rightVal->isBigInt() || rightVal->isInt32() || rightVal->isBool())) return Value(leftVal->asBigInt() > rightVal->asBigInt());
                    if (leftVal->isObjType(ObjType::FRACTION) && rightVal->isObjType(ObjType::FRACTION)) return Value(static_cast<ObjFraction*>(leftVal->asObj())->frac > static_cast<ObjFraction*>(rightVal->asObj())->frac);
                    if (leftVal->isString() && rightVal->isString()) return Value(leftVal->asString() > rightVal->asString());
                    return Value(leftVal->asDouble() > rightVal->asDouble());
                }
                case TokenType::GREATER_EQUAL: {
                    if ((leftVal->isBigInt() || leftVal->isInt32() || leftVal->isBool()) && (rightVal->isBigInt() || rightVal->isInt32() || rightVal->isBool())) return Value(leftVal->asBigInt() >= rightVal->asBigInt());
                    if (leftVal->isObjType(ObjType::FRACTION) && rightVal->isObjType(ObjType::FRACTION)) return Value(static_cast<ObjFraction*>(leftVal->asObj())->frac >= static_cast<ObjFraction*>(rightVal->asObj())->frac);
                    if (leftVal->isString() && rightVal->isString()) return Value(leftVal->asString() >= rightVal->asString());
                    return Value(leftVal->asDouble() >= rightVal->asDouble());
                }
                default: return std::nullopt;
                }
            }
            catch (...) { return std::nullopt; }
        }
        return std::nullopt;
    }

    void Compiler::visitUnary(Unary* expr) {
        lastLine = expr->op.line;
        if (auto folded = tryFoldConstant(expr)) {
            uint16_t idx = makeConstant(*folded);
            emit(OpCode::OP_CONSTANT, lastLine);
            emit16(idx, lastLine);
            return;
        }

        compileNode(expr->right.get());
        switch (expr->op.type) {
        case TokenType::MINUS: emit(OpCode::OP_NEGATE, expr->op.line); break;
        case TokenType::BANG:  emit(OpCode::OP_NOT, expr->op.line); break;
        case TokenType::TILDE: emit(OpCode::OP_BIT_NOT, expr->op.line); break;
        case TokenType::PLUS:  break;
        default: throw std::runtime_error("Compiler Error: Unknown unary operator.");
        }
        return;
    }

    void Compiler::visitBinary(Binary* expr) {
        lastLine = expr->op.line;
        if (auto folded = tryFoldConstant(expr)) {
            uint16_t idx = makeConstant(*folded);
            emit(OpCode::OP_CONSTANT, lastLine);
            emit16(idx, lastLine);
            return;
        }

        if (expr->op.type == TokenType::AND_AND) {
            compileNode(expr->left.get());
            int jump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, expr->op.line);
            emit(OpCode::OP_POP, expr->op.line);
            compileNode(expr->right.get());
            chunk()->patchJump(jump);
            return;
        }
        if (expr->op.type == TokenType::OR_OR) {
            compileNode(expr->left.get());
            int elseJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, expr->op.line);
            int endJump = chunk()->emitJump(OpCode::OP_JUMP, expr->op.line);
            chunk()->patchJump(elseJump);
            emit(OpCode::OP_POP, expr->op.line);
            compileNode(expr->right.get());
            chunk()->patchJump(endJump);
            return;
        }

        if (expr->op.type == TokenType::PIPE) {
            compileNode(expr->right.get());
            compileNode(expr->left.get());
            emit(OpCode::OP_CALL, expr->op.line);
            emit(1, expr->op.line);
            return;
        }

        compileNode(expr->left.get());
        compileNode(expr->right.get());

        int line = expr->op.line;
        switch (expr->op.type) {
        case TokenType::PLUS:          emit(OpCode::OP_ADD, line); break;
        case TokenType::MINUS:         emit(OpCode::OP_SUBTRACT, line); break;
        case TokenType::STAR:          emit(OpCode::OP_MULTIPLY, line); break;
        case TokenType::SLASH:         emit(OpCode::OP_DIVIDE, line); break;
        case TokenType::PERCENT:       emit(OpCode::OP_MODULO, line); break;
        case TokenType::CARET:         emit(OpCode::OP_POWER, line); break;
        case TokenType::BACKSLASH:     emit(OpCode::OP_LEFT_DIVIDE, line); break;
        case TokenType::EQUAL:         emit(OpCode::OP_EQUAL, line); break;
        case TokenType::BANG_EQUAL:    emit(OpCode::OP_NOT_EQUAL, line); break;
        case TokenType::LESS:          emit(OpCode::OP_LESS, line); break;
        case TokenType::LESS_EQUAL:    emit(OpCode::OP_LESS_EQUAL, line); break;
        case TokenType::GREATER:       emit(OpCode::OP_GREATER, line); break;
        case TokenType::GREATER_EQUAL: emit(OpCode::OP_GREATER_EQUAL, line); break;
        case TokenType::IN:            emit(OpCode::OP_IN, line); break;
        case TokenType::BIT_AND:       emit(OpCode::OP_BIT_AND, line); break;  // ★
        case TokenType::BIT_OR:        emit(OpCode::OP_BIT_OR, line); break;   // ★
        case TokenType::BIT_XOR:       emit(OpCode::OP_BIT_XOR, line); break;  // ★
        case TokenType::SHIFT_LEFT:    emit(OpCode::OP_BIT_SHIFT_LEFT, line); break;
        case TokenType::SHIFT_RIGHT:   emit(OpCode::OP_BIT_SHIFT_RIGHT, line); break;
        default:
            throw std::runtime_error("Compiler Error: Unsupported binary operator '" +
                expr->op.lexeme + "'.");
        }
        return;
    }

    void Compiler::visitCall(Call* expr) {
        lastLine = expr->callee.line;
        const std::string& name = expr->callee.lexeme;

        bool isBuiltin = false;
        if (VM::activeVM) {
            isBuiltin = VM::activeVM->getNativeBuiltins().count(name) > 0;
        }

        int slot = resolveLocal(name);
        if (slot != -1 && !isBuiltin) {
            emit(OpCode::OP_GET_LOCAL, expr->callee.line);
            emit16(static_cast<uint16_t>(slot), expr->callee.line);
        }
        else {
            int upvalue = resolveUpvalue(name);
            if (upvalue != -1 && !isBuiltin) {
                emit(OpCode::OP_GET_UPVALUE, expr->callee.line);
                emit16(static_cast<uint16_t>(upvalue), expr->callee.line);
            }
            else {
                // ★ 关键重构：将全局级别调用的目标变成字符串文字，把解析交接给 VM 的 OP_CALL 晚绑定操作
                uint16_t idx = identifierConstant(name);
                emit(OpCode::OP_CONSTANT, expr->callee.line);
                emit16(idx, expr->callee.line);
            }
        }
        for (auto& argExpr : expr->arguments) {
            compileNode(argExpr.get());
        }
        
        bool hasVariableArgs = false;
        for (auto& argExpr : expr->arguments) {
            if (dynamic_cast<Variable*>(argExpr.get())) {
                hasVariableArgs = true;
                break;
            }
        }

        bool mayHaveRef = false;
        if (hasVariableArgs) {
            bool foundDef = false;
            for (auto& fn : compiledFunctions) {
                if (fn->name == name) {
                    foundDef = true;
                    for (bool r : fn->paramIsRef) {
                        if (r) { mayHaveRef = true; break; }
                    }
                    break;
                }
            }
            if (!foundDef) mayHaveRef = true;
        }

        struct ArgSource { uint8_t argIndex; uint8_t sourceType; uint16_t sourceRef; };
        std::vector<ArgSource> sources;

        if (mayHaveRef) {
            for (int i = 0; i < static_cast<int>(expr->arguments.size()); ++i) {
                if (auto* varExpr = dynamic_cast<Variable*>(expr->arguments[i].get())) {
                    int localSlot = resolveLocal(varExpr->name.lexeme);
                    if (localSlot != -1) {
                        if (current().locals[localSlot].isRefParam) {
                            sources.push_back({ static_cast<uint8_t>(i), 4, static_cast<uint16_t>(current().locals[localSlot].refParamIndex) });
                        } else {
                            sources.push_back({ static_cast<uint8_t>(i), 2, static_cast<uint16_t>(localSlot) });
                        }
                    }
                    else {
                        int uv = resolveUpvalue(varExpr->name.lexeme);
                        if (uv != -1) {
                            sources.push_back({ static_cast<uint8_t>(i), 3, static_cast<uint16_t>(uv) });
                        }
                        else {
                            uint16_t nameIdx = identifierConstant(varExpr->name.lexeme);
                            sources.push_back({ static_cast<uint8_t>(i), 1, nameIdx });
                        }
                    }
                }
            }
        }

        bool actualTailCall = inTailPosition && sources.empty();

        if (!sources.empty()) {
            emit(OpCode::OP_PASS_REFS, expr->callee.line);
            emit(static_cast<uint8_t>(sources.size()), expr->callee.line);
            for (auto& s : sources) {
                emit(s.argIndex, expr->callee.line);
                emit(s.sourceType, expr->callee.line);
                emit16(s.sourceRef, expr->callee.line);
            }
        }

        if (actualTailCall) {
            emit(OpCode::OP_TAIL_CALL, expr->callee.line);
            emit(static_cast<uint8_t>(expr->arguments.size()), expr->callee.line);
            tailCallEmitted = true;
        } else {
            emit(OpCode::OP_CALL, expr->callee.line);
            emit(static_cast<uint8_t>(expr->arguments.size()), expr->callee.line);
        }
        return;
    }

    void Compiler::visitBlock(Block* expr) {
        beginScope();
        if (expr->statements.empty()) {
            emit(OpCode::OP_NONE, lastLine);
        }
        else {
            for (size_t i = 0; i < expr->statements.size(); ++i) {
                compileNode(expr->statements[i].get());
                if (i < expr->statements.size() - 1) {
                    emit(OpCode::OP_POP, lastLine);
                }
            }
        }

        endScope();
        return;
    }

    void Compiler::visitIfExpr(IfExpr* expr) {
        if (auto condVal = tryFoldConstant(expr->condition.get())) {
            beginScope();
            if (condVal->truthy()) {
                compileNode(expr->thenBranch.get());
            }
            else if (expr->elseBranch) {
                compileNode(expr->elseBranch.get());
            }
            else {
                emit(OpCode::OP_NONE, lastLine);
            }
            endScope();
            return;
        }

        beginScope(); // ★ 自动创建块级作用域
        compileNode(expr->condition.get());
        int thenJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
        emit(OpCode::OP_POP, lastLine);
        compileNode(expr->thenBranch.get());
        int elseJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
        chunk()->patchJump(thenJump);
        emit(OpCode::OP_POP, lastLine);
        if (expr->elseBranch) {
            compileNode(expr->elseBranch.get());
        }
        else {
            emit(OpCode::OP_NONE, lastLine);
        }
        chunk()->patchJump(elseJump);
        endScope(); // ★
        return;
    }

    void Compiler::visitWhileExpr(WhileExpr* expr) {
        auto condVal = tryFoldConstant(expr->condition.get());
        if (condVal && !condVal->truthy()) {
            emit(OpCode::OP_NONE, lastLine);
            return;
        }

        beginScope(); // ★ 自动创建块级作用域
        int loopStart = static_cast<int>(chunk()->code.size());
        beginLoop(loopStart);

        int exitJump = -1;
        if (condVal && condVal->truthy()) {
            // Infinite loop, no condition check needed
        } else {
            compileNode(expr->condition.get());
            exitJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
            emit(OpCode::OP_POP, lastLine);
        }

        compileNode(expr->body.get());

        // ★ continue 跳转目标：就在 POP body result 之前
        for (int offset : loopStack.back().continueJumps) {
            chunk()->patchJump(offset);
        }

        emit(OpCode::OP_POP, lastLine);   // POP body result（或 continue 填充的 NONE）
        chunk()->emitLoop(loopStart, lastLine);

        if (exitJump != -1) {
            chunk()->patchJump(exitJump);
            emit(OpCode::OP_POP, lastLine);
        }

        emitBreakJumps();
        endLoop();

        emit(OpCode::OP_NONE, lastLine);
        endScope(); // ★
        return;
    }

    void Compiler::visitForExpr(ForExpr* expr) {
        beginScope();
        compileNode(expr->initializer.get());
        emit(OpCode::OP_POP, lastLine);

        int loopStart = static_cast<int>(chunk()->code.size());
        beginLoop(loopStart);

        compileNode(expr->condition.get());
        int exitJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
        emit(OpCode::OP_POP, lastLine);
        compileNode(expr->body.get());

        // ★ continue 跳转目标：POP body result，然后执行 update
        for (int offset : loopStack.back().continueJumps) {
            chunk()->patchJump(offset);
        }

        emit(OpCode::OP_POP, lastLine);   // POP body result
        compileNode(expr->update.get());
        emit(OpCode::OP_POP, lastLine);   // POP update result
        chunk()->emitLoop(loopStart, lastLine);

        chunk()->patchJump(exitJump);
        emit(OpCode::OP_POP, lastLine);

        emitBreakJumps();
        endLoop();

        emit(OpCode::OP_NONE, lastLine);
        endScope();
        return;
    }

    void Compiler::visitLambdaExpr(LambdaExpr* expr) {
        auto fn = std::make_shared<CompiledFunction>();
        fn->name = expr->name;
        fn->maxArity = static_cast<int>(expr->params.size());
        fn->hasRestParam = expr->hasRestParam;

        int requiredParams = 0;
        for (size_t i = 0; i < expr->params.size(); ++i) {
            if (i >= expr->defaultExprs.size() || !expr->defaultExprs[i]) {
                if (expr->hasRestParam && i == expr->params.size() - 1) break;
                requiredParams++;
            }
            else break;
        }
        fn->arity = requiredParams;

        compiledFunctions.push_back(fn);

        int thisFnIndex = functionIndexOffset +
            static_cast<int>(compiledFunctions.size()) - 1;

        initCompiler(fn.get());
        beginScope();

        int refIdx = 0;
        for (size_t i = 0; i < expr->params.size(); ++i) {
            addLocal(expr->params[i].lexeme, current().scopeDepth);
            if (expr->paramIsRef[i]) {
                current().locals.back().isRefParam = true;
                current().locals.back().refParamIndex = refIdx++;
            }
        }
        fn->paramIsRef = expr->paramIsRef; // ★ Transfer ref info
        emitDefaultPreamble(expr->defaultExprs, fn->maxArity);


        for (size_t i = 0; i < expr->params.size(); ++i) {
            if (i < expr->paramTypes.size() && !expr->paramTypes[i].empty()) {
                int slot = resolveLocal(expr->params[i].lexeme);
                if (current().locals[slot].isRefParam) {
                    emit(OpCode::OP_GET_REF_PARAM, lastLine); 
                    emit16(static_cast<uint16_t>(current().locals[slot].refParamIndex), lastLine);
                } else {
                    emit(OpCode::OP_GET_LOCAL, lastLine); 
                    emit16(static_cast<uint16_t>(slot), lastLine);
                }

                uint16_t typeIdx = identifierConstant(expr->paramTypes[i]);
                uint16_t nameIdx = identifierConstant(expr->params[i].lexeme);
                emit(OpCode::OP_ASSERT_PARAM_TYPE, lastLine);
                emit16(typeIdx, lastLine); emit16(nameIdx, lastLine);
            }
        }
        current().expectedReturnType = expr->returnType;
        compileNode(expr->body.get());
        if (!expr->returnType.empty()) {
            uint16_t typeIdx = identifierConstant(expr->returnType);
            emit(OpCode::OP_ASSERT_RETURN_TYPE, lastLine);
            emit16(typeIdx, lastLine);
        }
        emit(OpCode::OP_RETURN, lastLine);

        // ★ 修改：提取峰值
        fn->localCount = current().maxLocals;

        endScope();
        stateStack.pop_back();

        uint16_t fnIdx = makeConstant(Value(static_cast<double>(thisFnIndex)));
        emit(OpCode::OP_CLOSURE, lastLine);
        emit16(fnIdx, lastLine);

        return;
    }

    int Compiler::addUpvalue(int level, const std::string& name,
        bool isLocal, int index, bool isRef, bool isGlobal, bool isExplicitState, bool isRefParam) {
        auto* fn = stateStack[level].function;
        for (int j = 0; j < static_cast<int>(fn->upvalues.size()); ++j) {
            if (fn->upvalues[j].name == name && fn->upvalues[j].isExplicitState == isExplicitState) {
                if (isRef) fn->upvalues[j].isRef = true; // ★ 升级为引用捕获
                if (isGlobal) fn->upvalues[j].isGlobal = true;
                return j;
            }
        }
        fn->upvalues.push_back({ name, isLocal, index, isRef, isGlobal, isExplicitState, isRefParam });
        return static_cast<int>(fn->upvalues.size()) - 1;
    }

    int Compiler::resolveUpvalueAt(int level, const std::string& name, bool isRef, bool isState) {
        if (level <= 0) {
            if (isState) return -2;
            return -1;
        }
        int enclosingLevel = level - 1;
        auto& enclosing = stateStack[enclosingLevel];

        for (int i = static_cast<int>(enclosing.locals.size()) - 1; i >= 0; --i) {
            if (enclosing.locals[i].name == name) {
                enclosing.locals[i].isCaptured = true; // ★ 标记为被捕获，防止其物理 slot 被复用
                if (enclosing.locals[i].isRefParam) {
                    return addUpvalue(level, name, true, enclosing.locals[i].refParamIndex, isRef, false, false, true);
                } else {
                    return addUpvalue(level, name, true, i, isRef, false, false, false);
                }
            }
        }

        auto it = enclosing.captures.find(name);
        bool isStateVar = it != enclosing.captures.end() && it->second.type == CaptureType::State;
        bool isRefVar = it != enclosing.captures.end() && it->second.type == CaptureType::Ref;
        bool isExplicitStateVar = isStateVar && it->second.isExplicitState;

        if (isExplicitStateVar) {
            int enclosingUv = addUpvalue(enclosingLevel, name, false, -1, isRefVar, true, true);
            return addUpvalue(level, name, false, enclosingUv, isRef, false, false);
        }

        bool enclosingIsState = isState || isStateVar;
        bool enclosingIsRef = isRef || isRefVar;

        int upvalueInEnclosing = resolveUpvalueAt(enclosingLevel, name, enclosingIsRef, enclosingIsState);
        if (upvalueInEnclosing != -1) {
            if (upvalueInEnclosing == -2) {
                if (enclosingLevel == 0) {
                    return -2;
                }
                // ★ 核心修复：如果外层是全局捕获，必须在外层添加 isGlobal=true 的 Upvalue，
                // 然后当前层添加一个普通的 Upvalue 指向外层！保证内外层物理内存共享！
                int enclosingUv = addUpvalue(enclosingLevel, name, false, -1, enclosingIsRef, true, false);
                return addUpvalue(level, name, false, enclosingUv, isRef, false, false);
            }
            
            return addUpvalue(level, name, false, upvalueInEnclosing, isRef, false, false);
        }

        return -1;
    }

    int Compiler::resolveUpvalue(const std::string& name) {
        int currentLevel = static_cast<int>(stateStack.size()) - 1;
        auto it = stateStack[currentLevel].captures.find(name);
        bool isRef = it != stateStack[currentLevel].captures.end() && it->second.type == CaptureType::Ref;
        bool isState = it != stateStack[currentLevel].captures.end() && it->second.type == CaptureType::State;
        bool isExplicitState = isState && it->second.isExplicitState;
        
        // ★ 规避副作用：仅在闭包捕获自身函数名时，临时强制按引用捕获。
        // 这样既支持了递归，又不会污染 refNames 导致无法创建同名局部变量。
        if (stateStack[currentLevel].function && name == stateStack[currentLevel].function->name) {
            isRef = true;
        }
        
        if (isState && isExplicitState) {
            // 显式初始化的 state，直接在当前层添加一个 isGlobal=true 的 upvalue，不往外找！
            // 这样它在 OP_CLOSURE 时必定被初始化为 uninit
            return addUpvalue(currentLevel, name, false, -1, isRef, true, true);
        }
        
        int uv = resolveUpvalueAt(currentLevel, name, isRef, isState);
        if (uv == -2) {
            if (isState) {
                return addUpvalue(currentLevel, name, false, -1, isRef, true, false);
            }
            return -1;
        }
        return uv;
    }

    void Compiler::visitReturnExpr(ReturnExpr* expr) {
        bool hasRefParams = false;
        for (bool isRef : current().function->paramIsRef) {
            if (isRef) { hasRefParams = true; break; }
        }
        bool isInit = current().function->name == "init";
        bool canTailCall = current().expectedReturnType.empty() && current().tryDepth == 0 && !hasRefParams && !isInit;

        if (expr->value) {
            bool prevTail = inTailPosition;
            bool prevEmitted = tailCallEmitted;
            inTailPosition = canTailCall;
            tailCallEmitted = false;
            
            compileNode(expr->value.get());
            
            bool emitted = tailCallEmitted;
            inTailPosition = prevTail;
            tailCallEmitted = prevEmitted;

            if (emitted) {
                return; // 尾调用指令已经包含了 return 语义
            }
        } else {
            emit(OpCode::OP_NONE, lastLine);
        }

        // ★ 幽灵注入：手工书写的返回值检查
        if (!current().expectedReturnType.empty()) {
            uint16_t typeIdx = identifierConstant(current().expectedReturnType);
            emit(OpCode::OP_ASSERT_RETURN_TYPE, lastLine);
            emit16(typeIdx, lastLine);
        }

        for (int i = 0; i < current().tryDepth; ++i) emit(OpCode::OP_TRY_END, lastLine);
        emit(OpCode::OP_RETURN, lastLine);
        return;
    }

    void Compiler::visitBreakExpr(BreakExpr*) {
        if (loopStack.empty()) throw std::runtime_error("Compiler Error: 'break' outside loop.");

        // ★ 智能发散机制：清理掉在这层跳出沿途中遇到的所有 Try 处理器
        int diff = current().tryDepth - loopStack.back().tryDepth;
        for (int i = 0; i < diff; ++i) emit(OpCode::OP_TRY_END, lastLine);
        int jump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
        loopStack.back().breakJumps.push_back(jump);
        return;
    }

    void Compiler::visitContinueExpr(ContinueExpr*) {
        if (loopStack.empty()) throw std::runtime_error("Compiler Error: 'continue' outside loop.");
        emit(OpCode::OP_NONE, lastLine);
        // ★ 同样智能清理
        int diff = current().tryDepth - loopStack.back().tryDepth;
        for (int i = 0; i < diff; ++i) emit(OpCode::OP_TRY_END, lastLine);
        int jump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
        loopStack.back().continueJumps.push_back(jump);
        return;
    }

    void Compiler::visitCompoundAssign(CompoundAssign* expr) {
        auto emitOp = [this](TokenType op) {
            switch (op) {
            case TokenType::PLUS:    emit(OpCode::OP_ADD, lastLine); break;
            case TokenType::MINUS:   emit(OpCode::OP_SUBTRACT, lastLine); break;
            case TokenType::STAR:    emit(OpCode::OP_MULTIPLY, lastLine); break;
            case TokenType::SLASH:   emit(OpCode::OP_DIVIDE, lastLine); break;
            case TokenType::PERCENT: emit(OpCode::OP_MODULO, lastLine); break;
            case TokenType::CARET:   emit(OpCode::OP_POWER, lastLine); break;
            case TokenType::BACKSLASH: emit(OpCode::OP_LEFT_DIVIDE, lastLine); break;
            case TokenType::BIT_AND: emit(OpCode::OP_BIT_AND, lastLine); break; // ★
            case TokenType::BIT_OR:  emit(OpCode::OP_BIT_OR, lastLine); break;  // ★
            case TokenType::BIT_XOR: emit(OpCode::OP_BIT_XOR, lastLine); break; // ★
            case TokenType::SHIFT_LEFT: emit(OpCode::OP_BIT_SHIFT_LEFT, lastLine); break;
            case TokenType::SHIFT_RIGHT: emit(OpCode::OP_BIT_SHIFT_RIGHT, lastLine); break;
            default: throw std::runtime_error("Compiler Error: Unknown compound operator.");
            }
            };

        if (auto* var = dynamic_cast<Variable*>(expr->target.get())) {
            const std::string& name = var->name.lexeme;
            
            int existingSlot = resolveLocal(name);
            if (existingSlot != -1 && current().locals[existingSlot].isConst) {
                if (!expr->isLocal || current().locals[existingSlot].depth == current().scopeDepth) {
                    uint16_t msgIdx = identifierConstant("Runtime Error: Cannot modify const variable '" + name + "'.");
                    emit(OpCode::OP_CONSTANT, lastLine);
                    emit16(msgIdx, lastLine);
                    emit(OpCode::OP_THROW, lastLine);
                    return;
                }
            }
            auto capIt = current().captures.find(name);
            if (capIt != current().captures.end() && capIt->second.isConst) {
                if (!expr->isLocal) {
                    uint16_t msgIdx = identifierConstant("Runtime Error: Cannot modify const variable '" + name + "'.");
                    emit(OpCode::OP_CONSTANT, lastLine);
                    emit16(msgIdx, lastLine);
                    emit(OpCode::OP_THROW, lastLine);
                    return;
                }
            }

            if (stateStack.size() == 1) {
                knownGlobals.insert(name);
            }

            int slot = resolveLocal(name);
            int upvalue = -1;

            if (expr->isLocal) {
                if (current().captures.count(name) > 0) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'local' and 'ref'/'state'.");
                if (slot == -1 || current().locals[slot].depth < current().scopeDepth) {
                    addLocal(name, current().scopeDepth);
                    slot = resolveLocal(name);
                }
            } else if (expr->isRef) {
                if (current().captures.count(name) > 0 && current().captures[name].type != CaptureType::Ref) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'ref' and 'state'.");
                current().captures[name] = {CaptureType::Ref, false, false}; // ★ 提前注册
                upvalue = resolveUpvalue(name);
            } else if (expr->isState) {
                if (current().captures.count(name) > 0 && current().captures[name].type != CaptureType::State) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'ref' and 'state'.");
                current().captures[name] = {CaptureType::State, false, false};
                // DO NOT set explicitState for compound assignment!
                upvalue = resolveUpvalue(name);
            }

            // 读取当前值
            if (slot != -1 && current().captures.count(name) == 0) {
                if (current().locals[slot].isRefParam) {
                    emit(OpCode::OP_GET_REF_PARAM, lastLine);
                    emit16(static_cast<uint16_t>(current().locals[slot].refParamIndex), lastLine);
                } else {
                    emit(OpCode::OP_GET_LOCAL, lastLine);
                    emit16(static_cast<uint16_t>(slot), lastLine);
                }
            }
            else {
                if (upvalue == -1) upvalue = resolveUpvalue(name);
                if (upvalue != -1) {
                    emit(OpCode::OP_GET_UPVALUE, lastLine);
                    emit16(static_cast<uint16_t>(upvalue), lastLine);
                }
                else {
                    uint16_t idx = identifierConstant(name);
                    emit(OpCode::OP_GET_GLOBAL, lastLine);
                    emit16(idx, lastLine);
                }
            }

            compileNode(expr->value.get());
            emitOp(expr->op);

            if (slot != -1 && current().captures.count(name) == 0) {
                if (current().locals[slot].isRefParam) {
                    emit(OpCode::OP_SET_REF_PARAM, lastLine);
                    emit16(static_cast<uint16_t>(current().locals[slot].refParamIndex), lastLine);
                } else {
                    emit(OpCode::OP_SET_LOCAL, lastLine);
                    emit16(static_cast<uint16_t>(slot), lastLine);
                }
            }
            else {
                if (upvalue == -1) upvalue = resolveUpvalue(name);
                if (upvalue != -1) {
                    emit(OpCode::OP_SET_UPVALUE, lastLine);
                    emit16(static_cast<uint16_t>(upvalue), lastLine);
                }
                else {
                    uint16_t idx = identifierConstant(name);
                    emit(OpCode::OP_SET_GLOBAL, lastLine);
                    emit16(idx, lastLine);
                }
            }
            return;
        }

        if (auto* dot = dynamic_cast<DotAccess*>(expr->target.get())) {
            compileNode(dot->object.get());
            addLocal("", current().scopeDepth);
            int objTmpIdx = static_cast<int>(current().locals.size()) - 1;
            emit(OpCode::OP_SET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(objTmpIdx), lastLine);
            emit(OpCode::OP_POP, lastLine);
            
            emit(OpCode::OP_GET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(objTmpIdx), lastLine);
            uint16_t nameIdx = identifierConstant(dot->field.lexeme);
            emit(OpCode::OP_GET_PROPERTY, lastLine);
            emit16(nameIdx, lastLine);
            emit16(chunk()->addInlineCache(), lastLine);
            
            compileNode(expr->value.get());
            emitOp(expr->op);
            
            addLocal("", current().scopeDepth);
            int valTmpIdx = static_cast<int>(current().locals.size()) - 1;
            emit(OpCode::OP_SET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(valTmpIdx), lastLine);
            emit(OpCode::OP_POP, lastLine);
            
            emit(OpCode::OP_GET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(objTmpIdx), lastLine);
            emit(OpCode::OP_GET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(valTmpIdx), lastLine);
            emit(OpCode::OP_SET_PROPERTY, lastLine);
            emit16(nameIdx, lastLine);
            emit16(chunk()->addInlineCache(), lastLine);
            
            current().locals.pop_back();
            current().locals.pop_back();
            return;
        }

        if (auto* idx = dynamic_cast<IndexAccess*>(expr->target.get())) {
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
            
            for (auto* c : chain) {
                for (auto& i : c->indices) {
                    if (dynamic_cast<SliceExpr*>(i.get())) {
                        throw std::runtime_error("Compiler Error: Slice compound assignment not supported in VM.");
                    }
                }
            }
            
            compileNode(chain[0]->object.get());
            addLocal("", current().scopeDepth);
            int rootTmpIdx = static_cast<int>(current().locals.size()) - 1;
            emit(OpCode::OP_SET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(rootTmpIdx), lastLine);
            emit(OpCode::OP_POP, lastLine);
            
            std::vector<std::vector<int>> indicesTmp(chain.size());
            for (int i = 0; i < static_cast<int>(chain.size()); ++i) {
                for (auto& indexExpr : chain[i]->indices) {
                    compileNode(indexExpr.get());
                    addLocal("", current().scopeDepth);
                    int tmpIdx = static_cast<int>(current().locals.size()) - 1;
                    emit(OpCode::OP_SET_LOCAL, lastLine);
                    emit16(static_cast<uint16_t>(tmpIdx), lastLine);
                    emit(OpCode::OP_POP, lastLine);
                    indicesTmp[i].push_back(tmpIdx);
                }
            }
            
            auto emitLoadRoot = [&]() {
                emit(OpCode::OP_GET_LOCAL, lastLine);
                emit16(static_cast<uint16_t>(rootTmpIdx), lastLine);
            };
            
            auto emitLoadIndices = [&](int level) {
                for (int tmpIdx : indicesTmp[level]) {
                    emit(OpCode::OP_GET_LOCAL, lastLine);
                    emit16(static_cast<uint16_t>(tmpIdx), lastLine);
                }
            };
            
            emitLoadRoot();
            for (int i = 0; i < static_cast<int>(chain.size()); ++i) {
                emitLoadIndices(i);
                emit(OpCode::OP_INDEX_GET, lastLine);
                emit(static_cast<uint8_t>(indicesTmp[i].size()), lastLine);
            }
            
            compileNode(expr->value.get());
            emitOp(expr->op);
            
            addLocal("", current().scopeDepth);
            int valTmpIdx = static_cast<int>(current().locals.size()) - 1;
            emit(OpCode::OP_SET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(valTmpIdx), lastLine);
            emit(OpCode::OP_POP, lastLine);
            
            int depth = static_cast<int>(chain.size());
            if (depth == 1) {
                emitLoadRoot();
                emitLoadIndices(0);
                emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valTmpIdx), lastLine);
                emit(OpCode::OP_INDEX_SET, lastLine);
                emit(static_cast<uint8_t>(chain[0]->indices.size()), lastLine);
                
                emitStoreTarget(chain[0]->object.get());
                emit(OpCode::OP_POP, lastLine);
                emit(OpCode::OP_POP, lastLine); // pop val
            } else {
                emit(OpCode::OP_NONE, lastLine);
                addLocal("", current().scopeDepth);
                int chainTmpIdx = static_cast<int>(current().locals.size()) - 1;
                emit(OpCode::OP_SET_LOCAL, lastLine);
                emit16(static_cast<uint16_t>(chainTmpIdx), lastLine);
                emit(OpCode::OP_POP, lastLine);

                emitLoadRoot();
                for (int level = 0; level < depth - 1; ++level) {
                    emitLoadIndices(level);
                    emit(OpCode::OP_INDEX_GET, lastLine);
                    emit(static_cast<uint8_t>(chain[level]->indices.size()), lastLine);
                }
                emitLoadIndices(depth - 1);
                emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valTmpIdx), lastLine);
                emit(OpCode::OP_INDEX_SET, lastLine);
                emit(static_cast<uint8_t>(chain[depth - 1]->indices.size()), lastLine);

                for (int level = depth - 2; level >= 0; --level) {
                    emit(OpCode::OP_SET_LOCAL, lastLine); emit16(static_cast<uint16_t>(chainTmpIdx), lastLine); 
                    emit(OpCode::OP_POP, lastLine);
                    emit(OpCode::OP_POP, lastLine);
                    
                    emitLoadRoot();
                    for (int l = 0; l < level; ++l) {
                        emitLoadIndices(l);
                        emit(OpCode::OP_INDEX_GET, lastLine); emit(static_cast<uint8_t>(chain[l]->indices.size()), lastLine);
                    }
                    emitLoadIndices(level);
                    emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(chainTmpIdx), lastLine);
                    emit(OpCode::OP_INDEX_SET, lastLine); emit(static_cast<uint8_t>(chain[level]->indices.size()), lastLine);
                }
                emitStoreTarget(chain[0]->object.get());
                emit(OpCode::OP_POP, lastLine);
                emit(OpCode::OP_POP, lastLine); // pop chainTmpIdx
                
                current().locals.pop_back();
            }
            
            emit(OpCode::OP_GET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(valTmpIdx), lastLine);
            
            current().locals.pop_back();
            for (auto it = indicesTmp.rbegin(); it != indicesTmp.rend(); ++it) {
                for (size_t j = 0; j < it->size(); ++j) {
                    current().locals.pop_back();
                }
            }
            current().locals.pop_back();
            
            return;
        }

        throw std::runtime_error("Compiler Error: Compound assignment target not supported in VM.");
    }

    void Compiler::visitForInExpr(ForInExpr* expr) {
        beginScope(); // ★ 自动创建块级作用域
        compileNode(expr->iterable.get());
        emit(OpCode::OP_ITER_INIT, lastLine);
        emit(static_cast<uint8_t>(expr->isDestruct() ? 1 : 0), lastLine);

        int loopStart = static_cast<int>(chunk()->code.size());
        beginLoop(loopStart);

        int exitJump = chunk()->emitJump(OpCode::OP_ITER_NEXT, lastLine);

        if (expr->isDestruct()) {
            addLocal("<forin_val>", current().scopeDepth);
            int valSlot = static_cast<int>(current().locals.size()) - 1;
            emit(OpCode::OP_SET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(valSlot), lastLine);
            emit(OpCode::OP_POP, lastLine);

            std::vector<std::tuple<std::string, ScopeModifier, bool>> boundVars;
            collectPatternVars(expr->pattern.get(), boundVars);

            std::vector<std::string> tempStateNames;
            for (auto& varTuple : boundVars) {
                const std::string& name = std::get<0>(varTuple);
                ScopeModifier& mod = std::get<1>(varTuple);
                bool isConst = std::get<2>(varTuple) || expr->isConst;
                if (name == "_") continue;
                
                if (mod == ScopeModifier::None && expr->isLocal) mod = ScopeModifier::Local;

                if (mod == ScopeModifier::Local) {
                    if (current().captures.count(name) > 0) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'local' and 'ref'/'state'.");
                } else if (mod == ScopeModifier::Ref) {
                    if (current().captures.count(name) > 0 && current().captures[name].type != CaptureType::Ref) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'ref' and 'state'.");
                    current().captures[name] = {CaptureType::Ref, isConst, false};
                } else if (mod == ScopeModifier::State) {
                    if (current().captures.count(name) > 0 && current().captures[name].type != CaptureType::State) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'ref' and 'state'.");
                    tempStateNames.push_back(name);
                }
            }

            for (auto& varTuple : boundVars) {
                const std::string& name = std::get<0>(varTuple);
                bool isConst = std::get<2>(varTuple) || expr->isConst;
                if (std::find(tempStateNames.begin(), tempStateNames.end(), name) != tempStateNames.end()) {
                    current().captures[name] = {CaptureType::State, isConst, true};
                }
            }

            for (const auto& varTuple : boundVars) {
                const std::string& name = std::get<0>(varTuple);
                ScopeModifier mod = std::get<1>(varTuple);
                bool isConst = std::get<2>(varTuple) || expr->isConst;
                if (name == "_") continue;
                
                int existingSlot = resolveLocal(name);
                if (existingSlot != -1 && current().locals[existingSlot].isConst) {
                    if (mod != ScopeModifier::Local || current().locals[existingSlot].depth == current().scopeDepth) {
                        uint16_t msgIdx = identifierConstant("Runtime Error: Cannot modify const variable '" + name + "'.");
                        emit(OpCode::OP_CONSTANT, lastLine);
                        emit16(msgIdx, lastLine);
                        emit(OpCode::OP_THROW, lastLine);
                        continue;
                    }
                }
                auto capIt = current().captures.find(name);
                if (capIt != current().captures.end() && capIt->second.isConst) {
                    if (mod != ScopeModifier::Local) {
                        uint16_t msgIdx = identifierConstant("Runtime Error: Cannot modify const variable '" + name + "'.");
                        emit(OpCode::OP_CONSTANT, lastLine);
                        emit16(msgIdx, lastLine);
                        emit(OpCode::OP_THROW, lastLine);
                        continue;
                    }
                }
                if (stateStack.size() == 1) knownGlobals.insert(name);

                int slot = resolveLocal(name);
                if (mod == ScopeModifier::Local) {
                    if (slot == -1 || current().locals[slot].depth < current().scopeDepth) {
                        addLocal(name, current().scopeDepth, isConst);
                    } else {
                        current().locals[slot].isConst = isConst;
                    }
                } else if (mod == ScopeModifier::None) {
                    if (stateStack.size() > 1 && slot == -1 && current().captures.count(name) == 0) {
                        addLocal(name, 0, isConst);
                    } else if (slot != -1) {
                        current().locals[slot].isConst = isConst;
                    }
                }
            }

            std::vector<int> failJumps;
            compilePatternMatch(expr->pattern.get(), valSlot, failJumps, expr->isConst);

            if (!failJumps.empty()) {
                int successJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                for (int fj : failJumps) chunk()->patchJump(fj);
                emit(OpCode::OP_POP, lastLine); // pop boolean
                emit(OpCode::OP_NONE, lastLine); // push dummy body result for the loop end POP
                loopStack.back().continueJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP, lastLine));
                chunk()->patchJump(successJump);
            }
        }
        else {
            const std::string& varName = expr->varName.lexeme;
            int existingSlot = resolveLocal(varName);
            if (existingSlot != -1 && current().locals[existingSlot].isConst) {
                if (!expr->isLocal || current().locals[existingSlot].depth == current().scopeDepth) {
                    uint16_t msgIdx = identifierConstant("Runtime Error: Cannot modify const variable '" + varName + "'.");
                    emit(OpCode::OP_CONSTANT, lastLine);
                    emit16(msgIdx, lastLine);
                    emit(OpCode::OP_THROW, lastLine);
                }
            }
            auto capIt = current().captures.find(varName);
            if (capIt != current().captures.end() && capIt->second.isConst) {
                if (!expr->isLocal) {
                    uint16_t msgIdx = identifierConstant("Runtime Error: Cannot modify const variable '" + varName + "'.");
                    emit(OpCode::OP_CONSTANT, lastLine);
                    emit16(msgIdx, lastLine);
                    emit(OpCode::OP_THROW, lastLine);
                }
            }
            
            if (stateStack.size() == 1) knownGlobals.insert(varName);

            int slot = resolveLocal(varName);
            if (expr->isLocal) {
                if (slot == -1 || current().locals[slot].depth < current().scopeDepth) {
                    addLocal(varName, current().scopeDepth, expr->isConst); slot = resolveLocal(varName);
                } else {
                    current().locals[slot].isConst = expr->isConst;
                }
            } else {
                if (stateStack.size() > 1 && slot == -1 && current().captures.count(varName) == 0) {
                    addLocal(varName, 0, expr->isConst); slot = resolveLocal(varName);
                } else if (slot != -1) {
                    current().locals[slot].isConst = expr->isConst;
                }
            }
            if (slot != -1) { emit(OpCode::OP_SET_LOCAL, lastLine); emit16(static_cast<uint16_t>(slot), lastLine); }
            else { 
                uint16_t idx = identifierConstant(varName); 
                auto it = current().captures.find(varName);
                if (it != current().captures.end() && it->second.type == CaptureType::Ref) {
                    emit(OpCode::OP_SET_GLOBAL_REF, lastLine);
                } else if (expr->isConst) {
                    emit(OpCode::OP_DEFINE_CONST_GLOBAL, lastLine);
                } else {
                    emit(OpCode::OP_SET_GLOBAL, lastLine); 
                }
                emit16(idx, lastLine); 
            }
            emit(OpCode::OP_POP, lastLine);
        }

        compileNode(expr->body.get());

        // ★ continue 跳转目标：POP body result
        for (int offset : loopStack.back().continueJumps) {
            chunk()->patchJump(offset);
        }

        emit(OpCode::OP_POP, lastLine);   // POP body result
        chunk()->emitLoop(loopStart, lastLine);
        chunk()->patchJump(exitJump);

        emitBreakJumps();
        endLoop();

        emit(OpCode::OP_POP, lastLine);   // pop iterator element
        emit(OpCode::OP_POP, lastLine);   // pop List
        emit(OpCode::OP_NONE, lastLine);
        endScope(); // ★
        return;
    }

    void Compiler::visitMatrixNode(MatrixNode* expr) {
        int rows = static_cast<int>(expr->elements.size());
        if (rows == 0) {
            if (expr->forceList) {
                emit(OpCode::OP_LIST_INIT, lastLine);
            } else {
                chunk()->emitConstant(Value(RealMatrix(0, 0)), lastLine);
            }
            return;
        }
        
        if (expr->forceList) {
            if (rows == 1) {
                int cols = static_cast<int>(expr->elements[0].size());
                for (int j = 0; j < cols; ++j) {
                    compileNode(expr->elements[0][j].get());
                }
                emit(OpCode::OP_BUILD_LIST, lastLine);
                emit16(static_cast<uint16_t>(cols), lastLine);
            } else {
                for (int i = 0; i < rows; ++i) {
                    int cols = static_cast<int>(expr->elements[i].size());
                    for (int j = 0; j < cols; ++j) {
                        compileNode(expr->elements[i][j].get());
                    }
                    emit(OpCode::OP_BUILD_LIST, lastLine);
                    emit16(static_cast<uint16_t>(cols), lastLine);
                }
                emit(OpCode::OP_BUILD_LIST, lastLine);
                emit16(static_cast<uint16_t>(rows), lastLine);
            }
            return;
        }

        std::vector<int> rowCols;
        for (int i = 0; i < rows; ++i) {
            int cols = static_cast<int>(expr->elements[i].size());
            rowCols.push_back(cols);
            for (int j = 0; j < cols; ++j) {
                compileNode(expr->elements[i][j].get());
            }
        }
        emit(OpCode::OP_BUILD_MATRIX, lastLine);
        emit16(static_cast<uint16_t>(rows), lastLine);
        for (int c : rowCols) {
            emit16(static_cast<uint16_t>(c), lastLine);
        }
        return;
    }

    void Compiler::visitIndexAccess(IndexAccess* expr) {
        bool hasSlice = false;
        for (auto& idx : expr->indices) {
            if (dynamic_cast<SliceExpr*>(idx.get())) {
                hasSlice = true;
                break;
            }
        }

        if (hasSlice) {
            compileNode(expr->object.get());
            for (auto& idx : expr->indices) {
                if (auto* slice = dynamic_cast<SliceExpr*>(idx.get())) {
                    if (slice->start) compileNode(slice->start.get()); else emit(OpCode::OP_NONE, lastLine);
                    if (slice->end) compileNode(slice->end.get()); else emit(OpCode::OP_NONE, lastLine);
                    if (slice->step) compileNode(slice->step.get()); else emit(OpCode::OP_NONE, lastLine);
                }
                else {
                    compileNode(idx.get());
                    emit(OpCode::OP_NONE, lastLine);       // end = none
                    chunk()->emitConstant(Value(0.0), lastLine);  // ★ step = 0 (点索引标记)
                }
            }
            emit(OpCode::OP_SLICE_GET, lastLine);
            emit(static_cast<uint8_t>(expr->indices.size()), lastLine);
            return;
        }
        compileNode(expr->object.get());
        for (auto& idx : expr->indices) compileNode(idx.get());
        emit(OpCode::OP_INDEX_GET, lastLine);
        emit(static_cast<uint8_t>(expr->indices.size()), lastLine);
        return;
    }

    void Compiler::visitIndexAssign(IndexAssign* expr) {
        if (!expr->hasObjectExpr()) {
            int existingSlot = resolveLocal(expr->name.lexeme);
            if (existingSlot != -1 && current().locals[existingSlot].isConst) {
                uint16_t msgIdx = identifierConstant("Runtime Error: Cannot modify const variable '" + expr->name.lexeme + "'.");
                emit(OpCode::OP_CONSTANT, lastLine);
                emit16(msgIdx, lastLine);
                emit(OpCode::OP_THROW, lastLine);
                return;
            }
            auto capIt = current().captures.find(expr->name.lexeme);
            if (capIt != current().captures.end() && capIt->second.isConst) {
                uint16_t msgIdx = identifierConstant("Runtime Error: Cannot modify const variable '" + expr->name.lexeme + "'.");
                emit(OpCode::OP_CONSTANT, lastLine);
                emit16(msgIdx, lastLine);
                emit(OpCode::OP_THROW, lastLine);
                return;
            }
        }

        bool hasSlice = false;
        if (expr->indexChain.size() == 1) {
            for (auto& idx : expr->indexChain[0]) {
                if (dynamic_cast<SliceExpr*>(idx.get())) {
                    hasSlice = true;
                    break;
                }
            }
        }

        // 1. 编译右值并保存到临时变量
        compileNode(expr->value.get());
        addLocal("", current().scopeDepth);
        int valTmpIdx = static_cast<int>(current().locals.size()) - 1;
        emit(OpCode::OP_SET_LOCAL, lastLine);
        emit16(static_cast<uint16_t>(valTmpIdx), lastLine);
        emit(OpCode::OP_POP, lastLine);

        // 2. 编译 root object 并保存到临时变量
        int rootTmpIdx = -1;
        if (expr->hasObjectExpr()) {
            compileNode(expr->objectExpr.get());
            addLocal("", current().scopeDepth);
            rootTmpIdx = static_cast<int>(current().locals.size()) - 1;
            emit(OpCode::OP_SET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(rootTmpIdx), lastLine);
            emit(OpCode::OP_POP, lastLine);
        }

        // 3. 编译所有的 indices 并保存到临时变量
        struct IndexTmp {
            int normalIdx = -1;
            int sliceStartIdx = -1;
            int sliceEndIdx = -1;
            int sliceStepIdx = -1;
        };
        std::vector<std::vector<IndexTmp>> indicesTmp(expr->indexChain.size());
        for (size_t i = 0; i < expr->indexChain.size(); ++i) {
            for (size_t j = 0; j < expr->indexChain[i].size(); ++j) {
                auto& idxExpr = expr->indexChain[i][j];
                IndexTmp tmp;
                if (auto* slice = dynamic_cast<SliceExpr*>(idxExpr.get())) {
                    if (slice->start) {
                        compileNode(slice->start.get());
                        addLocal("", current().scopeDepth);
                        tmp.sliceStartIdx = static_cast<int>(current().locals.size()) - 1;
                        emit(OpCode::OP_SET_LOCAL, lastLine);
                        emit16(static_cast<uint16_t>(tmp.sliceStartIdx), lastLine);
                        emit(OpCode::OP_POP, lastLine);
                    }
                    if (slice->end) {
                        compileNode(slice->end.get());
                        addLocal("", current().scopeDepth);
                        tmp.sliceEndIdx = static_cast<int>(current().locals.size()) - 1;
                        emit(OpCode::OP_SET_LOCAL, lastLine);
                        emit16(static_cast<uint16_t>(tmp.sliceEndIdx), lastLine);
                        emit(OpCode::OP_POP, lastLine);
                    }
                    if (slice->step) {
                        compileNode(slice->step.get());
                        addLocal("", current().scopeDepth);
                        tmp.sliceStepIdx = static_cast<int>(current().locals.size()) - 1;
                        emit(OpCode::OP_SET_LOCAL, lastLine);
                        emit16(static_cast<uint16_t>(tmp.sliceStepIdx), lastLine);
                        emit(OpCode::OP_POP, lastLine);
                    }
                } else {
                    compileNode(idxExpr.get());
                    addLocal("", current().scopeDepth);
                    tmp.normalIdx = static_cast<int>(current().locals.size()) - 1;
                    emit(OpCode::OP_SET_LOCAL, lastLine);
                    emit16(static_cast<uint16_t>(tmp.normalIdx), lastLine);
                    emit(OpCode::OP_POP, lastLine);
                }
                indicesTmp[i].push_back(tmp);
            }
        }

        auto emitLoadRoot = [&]() {
            if (expr->hasObjectExpr()) {
                emit(OpCode::OP_GET_LOCAL, lastLine);
                emit16(static_cast<uint16_t>(rootTmpIdx), lastLine);
            }
            else {
                int slot = resolveLocal(expr->name.lexeme);
                if (slot != -1) { emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(slot), lastLine); }
                else {
                    int upvalue = resolveUpvalue(expr->name.lexeme);
                    if (upvalue != -1) { emit(OpCode::OP_GET_UPVALUE, lastLine); emit16(static_cast<uint16_t>(upvalue), lastLine); }
                    else { uint16_t nameIdx = identifierConstant(expr->name.lexeme); emit(OpCode::OP_GET_GLOBAL, lastLine); emit16(nameIdx, lastLine); }
                }
            }
        };

        auto emitStoreRoot = [&]() {
            if (!expr->hasObjectExpr()) {
                int slot = resolveLocal(expr->name.lexeme);
                if (slot != -1) { emit(OpCode::OP_SET_LOCAL, lastLine); emit16(static_cast<uint16_t>(slot), lastLine); }
                else {
                    int upvalue = resolveUpvalue(expr->name.lexeme);
                    if (upvalue != -1) { emit(OpCode::OP_SET_UPVALUE, lastLine); emit16(static_cast<uint16_t>(upvalue), lastLine); }
                    else { 
                        uint16_t nameIdx = identifierConstant(expr->name.lexeme); 
                        auto it = current().captures.find(expr->name.lexeme);
                        if (it != current().captures.end() && it->second.type == CaptureType::Ref) {
                            emit(OpCode::OP_SET_GLOBAL_REF, lastLine);
                        } else {
                            emit(OpCode::OP_SET_GLOBAL, lastLine); 
                        }
                        emit16(nameIdx, lastLine); 
                    }
                }
            }
            else {
                if (auto* dot = dynamic_cast<DotAccess*>(expr->objectExpr.get())) {
                    emitStoreTarget(expr->objectExpr.get());
                } else {
                    emitStoreTarget(expr->objectExpr.get());
                }
            }
        };

        auto emitLoadIndices = [&](int level) {
            for (size_t j = 0; j < expr->indexChain[level].size(); ++j) {
                auto& tmp = indicesTmp[level][j];
                if (tmp.normalIdx == -1) {
                    if (tmp.sliceStartIdx != -1) {
                        emit(OpCode::OP_GET_LOCAL, lastLine);
                        emit16(static_cast<uint16_t>(tmp.sliceStartIdx), lastLine);
                    } else emit(OpCode::OP_NONE, lastLine);
                    
                    if (tmp.sliceEndIdx != -1) {
                        emit(OpCode::OP_GET_LOCAL, lastLine);
                        emit16(static_cast<uint16_t>(tmp.sliceEndIdx), lastLine);
                    } else emit(OpCode::OP_NONE, lastLine);
                    
                    if (tmp.sliceStepIdx != -1) {
                        emit(OpCode::OP_GET_LOCAL, lastLine);
                        emit16(static_cast<uint16_t>(tmp.sliceStepIdx), lastLine);
                    } else emit(OpCode::OP_NONE, lastLine);
                } else {
                    emit(OpCode::OP_GET_LOCAL, lastLine);
                    emit16(static_cast<uint16_t>(tmp.normalIdx), lastLine);
                    if (hasSlice) {
                        emit(OpCode::OP_NONE, lastLine);
                        chunk()->emitConstant(Value(0.0), lastLine);
                    }
                }
            }
        };

        if (hasSlice) {
            emitLoadRoot();
            emitLoadIndices(0);

            emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valTmpIdx), lastLine);
            emit(OpCode::OP_SLICE_SET, lastLine);
            emit(static_cast<uint8_t>(expr->indexChain[0].size()), lastLine);
            
            emitStoreRoot();
            emit(OpCode::OP_POP, lastLine); // pop root_new
            emit(OpCode::OP_POP, lastLine); // pop val
        } else {
            int depth = static_cast<int>(expr->indexChain.size());
            if (depth == 1) {
                emitLoadRoot();
                emitLoadIndices(0);
                emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valTmpIdx), lastLine);
                emit(OpCode::OP_INDEX_SET, lastLine);
                emit(static_cast<uint8_t>(expr->indexChain[0].size()), lastLine);
                
                emitStoreRoot();
                emit(OpCode::OP_POP, lastLine); // pop root_new
                emit(OpCode::OP_POP, lastLine); // pop val
            }
            else {
                emit(OpCode::OP_NONE, lastLine);
                addLocal("", current().scopeDepth);
                int chainTmpIdx = static_cast<int>(current().locals.size()) - 1;
                emit(OpCode::OP_SET_LOCAL, lastLine);
                emit16(static_cast<uint16_t>(chainTmpIdx), lastLine);
                emit(OpCode::OP_POP, lastLine);

                emitLoadRoot();
                for (int level = 0; level < depth - 1; ++level) {
                    emitLoadIndices(level);
                    emit(OpCode::OP_INDEX_GET, lastLine);
                    emit(static_cast<uint8_t>(expr->indexChain[level].size()), lastLine);
                }
                emitLoadIndices(depth - 1);
                emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valTmpIdx), lastLine);
                emit(OpCode::OP_INDEX_SET, lastLine);
                emit(static_cast<uint8_t>(expr->indexChain[depth - 1].size()), lastLine);

                for (int level = depth - 2; level >= 0; --level) {
                    emit(OpCode::OP_SET_LOCAL, lastLine); emit16(static_cast<uint16_t>(chainTmpIdx), lastLine); 
                    emit(OpCode::OP_POP, lastLine); // pop obj_new
                    emit(OpCode::OP_POP, lastLine); // pop VAL
                    
                    emitLoadRoot();
                    for (int l = 0; l < level; ++l) {
                        emitLoadIndices(l);
                        emit(OpCode::OP_INDEX_GET, lastLine); emit(static_cast<uint8_t>(expr->indexChain[l].size()), lastLine);
                    }
                    emitLoadIndices(level);
                    emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(chainTmpIdx), lastLine);
                    emit(OpCode::OP_INDEX_SET, lastLine); emit(static_cast<uint8_t>(expr->indexChain[level].size()), lastLine);
                }
                emitStoreRoot();
                emit(OpCode::OP_POP, lastLine); // pop root_new
                emit(OpCode::OP_POP, lastLine); // pop chainTmpIdx
                
                current().locals.pop_back(); // untrack chainTmpIdx
            }
        }

        // 恢复 val 到栈顶
        emit(OpCode::OP_GET_LOCAL, lastLine);
        emit16(static_cast<uint16_t>(valTmpIdx), lastLine);

        // 清理所有的临时变量
        for (auto it = indicesTmp.rbegin(); it != indicesTmp.rend(); ++it) {
            for (auto jt = it->rbegin(); jt != it->rend(); ++jt) {
                if (jt->sliceStepIdx != -1) current().locals.pop_back();
                if (jt->sliceEndIdx != -1) current().locals.pop_back();
                if (jt->sliceStartIdx != -1) current().locals.pop_back();
                if (jt->normalIdx != -1) current().locals.pop_back();
            }
        }
        if (rootTmpIdx != -1) current().locals.pop_back();
        current().locals.pop_back(); // untrack valTmpIdx

        return;
    }

    void Compiler::visitInvokeExpr(InvokeExpr* expr) {
        compileNode(expr->callee.get());
        for (auto& argExpr : expr->arguments) compileNode(argExpr.get());
        
        bool hasVariableArgs = false;
        for (auto& argExpr : expr->arguments) {
            if (dynamic_cast<Variable*>(argExpr.get())) { hasVariableArgs = true; break; }
        }

        struct ArgSource { uint8_t argIndex; uint8_t sourceType; uint16_t sourceRef; };
        std::vector<ArgSource> sources;

        if (hasVariableArgs) {
            for (int i = 0; i < static_cast<int>(expr->arguments.size()); ++i) {
                if (auto* varExpr = dynamic_cast<Variable*>(expr->arguments[i].get())) {
                    int localSlot = resolveLocal(varExpr->name.lexeme);
                    if (localSlot != -1) {
                        if (current().locals[localSlot].isRefParam) {
                            sources.push_back({ static_cast<uint8_t>(i), 4, static_cast<uint16_t>(current().locals[localSlot].refParamIndex) });
                        } else {
                            sources.push_back({ static_cast<uint8_t>(i), 2, static_cast<uint16_t>(localSlot) });
                        }
                    }
                    else {
                        int uv = resolveUpvalue(varExpr->name.lexeme);
                        if (uv != -1) sources.push_back({ static_cast<uint8_t>(i), 3, static_cast<uint16_t>(uv) });
                        else sources.push_back({ static_cast<uint8_t>(i), 1, identifierConstant(varExpr->name.lexeme) });
                    }
                }
            }
        }

        bool actualTailCall = inTailPosition && sources.empty();

        if (!sources.empty()) {
            emit(OpCode::OP_PASS_REFS, lastLine);
            emit(static_cast<uint8_t>(sources.size()), lastLine);
            for (auto& s : sources) { emit(s.argIndex, lastLine); emit(s.sourceType, lastLine); emit16(s.sourceRef, lastLine); }
        }

        if (actualTailCall) {
            emit(OpCode::OP_TAIL_CALL, lastLine);
            emit(static_cast<uint8_t>(expr->arguments.size()), lastLine);
            tailCallEmitted = true;
        } else {
            emit(OpCode::OP_CALL, lastLine);
            emit(static_cast<uint8_t>(expr->arguments.size()), lastLine);
        }
        return;
    }

    void Compiler::collectPatternVars(Pattern* pat, std::vector<std::tuple<std::string, ScopeModifier, bool>>& boundVars) {
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

    void Compiler::compilePatternMatch(Pattern* p, int valSlot, std::vector<int>& failJumps, bool isConst) {
        if (auto* dp = dynamic_cast<DefaultPattern*>(p)) {
            // 如果在顶层直接遇到 DefaultPattern，说明它没有被容器解构剥离。
            // 此时 valSlot 里的值已经是确定的（不会是缺失），所以默认值永远不会触发。
            // 直接匹配内部模式即可。
            compilePatternMatch(dp->inner.get(), valSlot, failJumps, isConst);
            return;
        }
        if (auto* lit = dynamic_cast<LiteralPattern*>(p)) {
            if (!tryFoldConstant(lit->literal.get())) {
                throw std::runtime_error("Compiler Error: Dynamic expression assertions must be enclosed in parentheses '()'.");
            }
            compileNode(lit->literal.get());
            emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
            emit(OpCode::OP_EQUAL, lastLine);
            failJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine));
            emit(OpCode::OP_POP, lastLine);
        } else if (auto* dynPat = dynamic_cast<DynamicAssertPattern*>(p)) {
            compileNode(dynPat->expr.get());
            emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
            emit(OpCode::OP_EQUAL, lastLine);
            failJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine));
            emit(OpCode::OP_POP, lastLine);
        } else if (auto* var = dynamic_cast<VariablePattern*>(p)) {
            if (var->name.lexeme != "_") {
                emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
                
                bool isExplicitState = false;
                auto it = current().captures.find(var->name.lexeme);
                if (it != current().captures.end() && it->second.type == CaptureType::State && it->second.isExplicitState) {
                    isExplicitState = true;
                }

                if (var->modifier == ScopeModifier::State || isExplicitState) {
                    int upvalue = resolveUpvalue(var->name.lexeme);
                    if (upvalue != -1) {
                        emit(OpCode::OP_GET_UPVALUE, lastLine);
                        emit16(static_cast<uint16_t>(upvalue), lastLine);
                        emit(OpCode::OP_IS_UNINIT, lastLine);
                        int skipJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
                        emit(OpCode::OP_POP, lastLine); // pop boolean
                        
                        emit(OpCode::OP_SET_UPVALUE, lastLine);
                        emit16(static_cast<uint16_t>(upvalue), lastLine);
                        
                        int endJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                        chunk()->patchJump(skipJump);
                        emit(OpCode::OP_POP, lastLine); // pop boolean
                        chunk()->patchJump(endJump);
                    }
                } else {
                    Variable v(var->name);
                    emitStoreTarget(&v, isConst || var->isConst);
                }
                emit(OpCode::OP_POP, lastLine);
            }
        } else if (auto* exprPat = dynamic_cast<ExprPattern*>(p)) {
            emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
            emitStoreTarget(exprPat->expr.get(), isConst);
            emit(OpCode::OP_POP, lastLine);
        } else if (auto* lp = dynamic_cast<ListPattern*>(p)) {
            int minCols = 0;
            bool hasRest = lp->rest != nullptr;
            bool hasRestInMiddle = false;
            bool hasDefault = false;
            for (size_t i = 0; i < lp->elements.size(); ++i) {
                auto& e = lp->elements[i];
                if (dynamic_cast<RestPattern*>(e.get())) {
                    hasRest = true;
                    if (i < lp->elements.size() - 1) hasRestInMiddle = true;
                }
                else if (dynamic_cast<DefaultPattern*>(e.get())) {
                    hasDefault = true;
                }
                else {
                    minCols++;
                }
            }
            if (hasRestInMiddle && hasDefault) {
                throw std::runtime_error("Compiler Error: Default values are not allowed when a rest pattern ('...') is used in the middle of a list.");
            }
            int maxCols = hasRest ? 0xFFFF : static_cast<int>(lp->elements.size());
            uint8_t exactMask = 2; // 1D pattern

            emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
            emit(OpCode::OP_MATCH_SHAPE, lastLine);
            emit16(1, lastLine); // minRows
            emit16(1, lastLine); // maxRows
            emit16(static_cast<uint16_t>(minCols), lastLine);
            emit16(static_cast<uint16_t>(maxCols), lastLine);
            emit(exactMask, lastLine);
            failJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine));
            emit(OpCode::OP_POP, lastLine);

            int c_idx = 0;
            bool afterRest = false;
            int rightOffset = 0;
            for (size_t i = 0; i < lp->elements.size(); ++i) {
                if (dynamic_cast<RestPattern*>(lp->elements[i].get())) afterRest = true;
                else if (afterRest) rightOffset++;
            }
            
            afterRest = false;
            int currentRightOffset = rightOffset;

            for (size_t i = 0; i < lp->elements.size(); ++i) {
                if (auto* restPat = dynamic_cast<RestPattern*>(lp->elements[i].get())) {
                    afterRest = true;
                    if (restPat->name.lexeme != "_") {
                        emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
                        emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(static_cast<double>(c_idx))), lastLine);
                        if (rightOffset > 0) {
                            emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(static_cast<double>(-rightOffset))), lastLine);
                        } else {
                            emit(OpCode::OP_NONE, lastLine);
                        }
                        emit(OpCode::OP_NONE, lastLine);
                        emit(OpCode::OP_SLICE_GET, lastLine); emit(1, lastLine);
                        
                        bool isExplicitState = false;
                        auto it = current().captures.find(restPat->name.lexeme);
                        if (it != current().captures.end() && it->second.type == CaptureType::State && it->second.isExplicitState) {
                            isExplicitState = true;
                        }
                        if (restPat->modifier == ScopeModifier::State || isExplicitState) {
                            int upvalue = resolveUpvalue(restPat->name.lexeme);
                            if (upvalue != -1) {
                                emit(OpCode::OP_GET_UPVALUE, lastLine);
                                emit16(static_cast<uint16_t>(upvalue), lastLine);
                                emit(OpCode::OP_IS_UNINIT, lastLine);
                                int skipJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
                                emit(OpCode::OP_POP, lastLine);
                                emit(OpCode::OP_SET_UPVALUE, lastLine);
                                emit16(static_cast<uint16_t>(upvalue), lastLine);
                                int endJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                                chunk()->patchJump(skipJump);
                                emit(OpCode::OP_POP, lastLine);
                                chunk()->patchJump(endJump);
                            }
                        } else {
                            Variable v(restPat->name);
                            emitStoreTarget(&v, isConst || restPat->isConst);
                        }
                        emit(OpCode::OP_POP, lastLine);
                    }
                } else {
                    Pattern* actualPat = lp->elements[i].get();
                    Expr* defExpr = nullptr;
                    if (auto* dp = dynamic_cast<DefaultPattern*>(actualPat)) {
                        actualPat = dp->inner.get();
                        defExpr = dp->defaultExpr.get();
                    }

                    emit(OpCode::OP_TRY_BEGIN, lastLine);
                    int catchOffsetSlot = static_cast<int>(chunk()->code.size());
                    emit16(0, lastLine);
                    emit16(identifierConstant(""), lastLine);
                    current().tryDepth++;

                    emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
                    if (afterRest) {
                        emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(static_cast<double>(-currentRightOffset))), lastLine);
                        currentRightOffset--;
                    } else {
                        emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(static_cast<double>(c_idx))), lastLine);
                        c_idx++;
                    }
                    emit(OpCode::OP_INDEX_GET, lastLine); emit(1, lastLine);

                    current().tryDepth--;
                    emit(OpCode::OP_TRY_END, lastLine);
                    emit(OpCode::OP_TRUE, lastLine);
                    int skipCatch = chunk()->emitJump(OpCode::OP_JUMP, lastLine);

                    int catchAddr = static_cast<int>(chunk()->code.size());
                    int relOffset = catchAddr - (catchOffsetSlot + 4);
                    chunk()->code[catchOffsetSlot] = static_cast<uint8_t>((relOffset >> 8) & 0xFF);
                    chunk()->code[catchOffsetSlot + 1] = static_cast<uint8_t>(relOffset & 0xFF);

                    emit(OpCode::OP_POP, lastLine); // pop error
                    emit(OpCode::OP_NONE, lastLine); // dummy value
                    emit(OpCode::OP_FALSE, lastLine);

                    chunk()->patchJump(skipCatch);

                    if (defExpr) {
                        int hasValJump = chunk()->emitJump(OpCode::OP_JUMP_IF_TRUE, lastLine);
                        emit(OpCode::OP_POP, lastLine); // pop false
                        emit(OpCode::OP_POP, lastLine); // pop none
                        compileNode(defExpr);
                        int endDefJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);

                        chunk()->patchJump(hasValJump);
                        emit(OpCode::OP_POP, lastLine); // pop true

                        chunk()->patchJump(endDefJump);
                    } else {
                        failJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine));
                        emit(OpCode::OP_POP, lastLine); // pop true
                    }

                    addLocal("<pat_tmp>", current().scopeDepth);
                    int tmpSlot = static_cast<int>(current().locals.size()) - 1;
                    emit(OpCode::OP_SET_LOCAL, lastLine); emit16(static_cast<uint16_t>(tmpSlot), lastLine);
                    emit(OpCode::OP_POP, lastLine);
                    
                    compilePatternMatch(actualPat, tmpSlot, failJumps, isConst);
                    current().locals.pop_back();
                }
            }

            if (lp->rest && lp->rest->name.lexeme != "_") {
                emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
                emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(static_cast<double>(c_idx))), lastLine);
                emit(OpCode::OP_NONE, lastLine);
                emit(OpCode::OP_NONE, lastLine);
                emit(OpCode::OP_SLICE_GET, lastLine); emit(1, lastLine);
                
                bool isExplicitState = false;
                auto it = current().captures.find(lp->rest->name.lexeme);
                if (it != current().captures.end() && it->second.type == CaptureType::State && it->second.isExplicitState) {
                    isExplicitState = true;
                }
                if (lp->rest->modifier == ScopeModifier::State || isExplicitState) {
                    int upvalue = resolveUpvalue(lp->rest->name.lexeme);
                    if (upvalue != -1) {
                        emit(OpCode::OP_GET_UPVALUE, lastLine);
                        emit16(static_cast<uint16_t>(upvalue), lastLine);
                        emit(OpCode::OP_IS_UNINIT, lastLine);
                        int skipJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
                        emit(OpCode::OP_POP, lastLine);
                        emit(OpCode::OP_SET_UPVALUE, lastLine);
                        emit16(static_cast<uint16_t>(upvalue), lastLine);
                        int endJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                        chunk()->patchJump(skipJump);
                        emit(OpCode::OP_POP, lastLine);
                        chunk()->patchJump(endJump);
                    }
                } else {
                    Variable v(lp->rest->name);
                    emitStoreTarget(&v, isConst || lp->rest->isConst);
                }
                emit(OpCode::OP_POP, lastLine);
            }
        } else if (auto* mp = dynamic_cast<MatrixPattern*>(p)) {
            int rows = static_cast<int>(mp->rows.size());
            int exactTotalCols = -1;
            bool anyRowNoRest = false;
            
            for (const auto& row : mp->rows) {
                bool hasRest = false;
                bool hasRestInMiddle = false;
                bool hasDefault = false;
                int total = 0;
                for (size_t i = 0; i < row.size(); ++i) {
                    auto& e = row[i];
                    if (dynamic_cast<RestPattern*>(e.get())) {
                        hasRest = true;
                        if (i < row.size() - 1) hasRestInMiddle = true;
                    }
                    else if (dynamic_cast<DefaultPattern*>(e.get())) {
                        hasDefault = true;
                        total++;
                    }
                    else {
                        total++;
                    }
                }
                if (hasRestInMiddle && hasDefault) {
                    throw std::runtime_error("Compiler Error: Default values are not allowed when a rest pattern ('...') is used in the middle of a matrix row.");
                }
                if (!hasRest) {
                    anyRowNoRest = true;
                    if (exactTotalCols == -1) exactTotalCols = total;
                    else if (exactTotalCols != total) {
                        throw std::runtime_error("Compiler Error: Matrix pattern rows without '...' must have the same number of columns.");
                    }
                }
            }
            
            if (anyRowNoRest) {
                for (const auto& row : mp->rows) {
                    bool hasRest = false;
                    int total = 0;
                    for (const auto& e : row) {
                        if (dynamic_cast<RestPattern*>(e.get())) hasRest = true;
                        else total++;
                    }
                    if (hasRest && total > exactTotalCols) {
                        throw std::runtime_error("Compiler Error: Matrix pattern row with '...' has more fixed elements than the exact column count.");
                    }
                }
            }
            
            int minCols = 0;
            for (const auto& row : mp->rows) {
                int required = 0;
                for (const auto& e : row) {
                    if (!dynamic_cast<RestPattern*>(e.get()) && !dynamic_cast<DefaultPattern*>(e.get())) {
                        required++;
                    }
                }
                if (required > minCols) minCols = required;
            }
            
            int maxCols = 0xFFFF;
            if (anyRowNoRest || (mp->rows.empty() && !mp->restRow)) {
                maxCols = 0;
                for (const auto& row : mp->rows) {
                    if (static_cast<int>(row.size()) > maxCols) maxCols = static_cast<int>(row.size());
                }
            }

            int minRows = 0;
            for (int r = 0; r < rows; ++r) {
                bool hasRequired = false;
                for (const auto& e : mp->rows[r]) {
                    if (!dynamic_cast<DefaultPattern*>(e.get()) && !dynamic_cast<RestPattern*>(e.get())) {
                        hasRequired = true;
                        break;
                    }
                }
                if (hasRequired) minRows = r + 1;
            }

            int maxRows = mp->restRow ? 0xFFFF : rows;

            uint8_t exactMask = 0;
            
            emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
            emit(OpCode::OP_MATCH_SHAPE, lastLine);
            emit16(static_cast<uint16_t>(minRows), lastLine);
            emit16(static_cast<uint16_t>(maxRows), lastLine);
            emit16(static_cast<uint16_t>(minCols), lastLine);
            emit16(static_cast<uint16_t>(maxCols), lastLine);
            emit(exactMask, lastLine);
            failJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine));
            emit(OpCode::OP_POP, lastLine);

            for (int r = 0; r < rows; ++r) {
                int c_idx = 0;
                bool afterRest = false;
                int rightOffset = 0;
                for (size_t i = 0; i < mp->rows[r].size(); ++i) {
                    if (dynamic_cast<RestPattern*>(mp->rows[r][i].get())) afterRest = true;
                    else if (afterRest) rightOffset++;
                }
                
                afterRest = false;
                int currentRightOffset = rightOffset;

                for (size_t i = 0; i < mp->rows[r].size(); ++i) {
                    auto& e = mp->rows[r][i];
                    if (auto* restPat = dynamic_cast<RestPattern*>(e.get())) {
                        afterRest = true;
                        if (restPat->name.lexeme != "_") {
                            emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
                            emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(static_cast<double>(r))), lastLine);
                            emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(static_cast<double>(r + 1))), lastLine);
                            emit(OpCode::OP_NONE, lastLine);
                            emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(static_cast<double>(c_idx))), lastLine);
                            if (rightOffset > 0) {
                                emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(static_cast<double>(-rightOffset))), lastLine);
                            } else {
                                emit(OpCode::OP_NONE, lastLine);
                            }
                            emit(OpCode::OP_NONE, lastLine);
                            emit(OpCode::OP_SLICE_GET, lastLine); emit(2, lastLine);
                            
                            bool isExplicitState = false;
                            auto it = current().captures.find(restPat->name.lexeme);
                            if (it != current().captures.end() && it->second.type == CaptureType::State && it->second.isExplicitState) {
                                isExplicitState = true;
                            }
                            if (restPat->modifier == ScopeModifier::State || isExplicitState) {
                                int upvalue = resolveUpvalue(restPat->name.lexeme);
                                if (upvalue != -1) {
                                    emit(OpCode::OP_GET_UPVALUE, lastLine);
                                    emit16(static_cast<uint16_t>(upvalue), lastLine);
                                    emit(OpCode::OP_IS_UNINIT, lastLine);
                                    int skipJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
                                    emit(OpCode::OP_POP, lastLine);
                                    emit(OpCode::OP_SET_UPVALUE, lastLine);
                                    emit16(static_cast<uint16_t>(upvalue), lastLine);
                                    int endJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                                    chunk()->patchJump(skipJump);
                                    emit(OpCode::OP_POP, lastLine);
                                    chunk()->patchJump(endJump);
                                }
                            } else {
                                Variable v(restPat->name);
                                emitStoreTarget(&v, isConst || restPat->isConst);
                            }
                            emit(OpCode::OP_POP, lastLine);
                        }
                    } else {
                        Pattern* actualPat = e.get();
                        Expr* defExpr = nullptr;
                        if (auto* dp = dynamic_cast<DefaultPattern*>(actualPat)) {
                            actualPat = dp->inner.get();
                            defExpr = dp->defaultExpr.get();
                        }

                        emit(OpCode::OP_TRY_BEGIN, lastLine);
                        int catchOffsetSlot = static_cast<int>(chunk()->code.size());
                        emit16(0, lastLine);
                        emit16(identifierConstant(""), lastLine);
                        current().tryDepth++;

                        emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
                        emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(static_cast<double>(r))), lastLine);
                        if (afterRest) {
                            emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(static_cast<double>(-currentRightOffset))), lastLine);
                            currentRightOffset--;
                        } else {
                            emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(static_cast<double>(c_idx))), lastLine);
                            c_idx++;
                        }
                        emit(OpCode::OP_INDEX_GET, lastLine); emit(2, lastLine);

                        current().tryDepth--;
                        emit(OpCode::OP_TRY_END, lastLine);
                        emit(OpCode::OP_TRUE, lastLine);
                        int skipCatch = chunk()->emitJump(OpCode::OP_JUMP, lastLine);

                        int catchAddr = static_cast<int>(chunk()->code.size());
                        int relOffset = catchAddr - (catchOffsetSlot + 4);
                        chunk()->code[catchOffsetSlot] = static_cast<uint8_t>((relOffset >> 8) & 0xFF);
                        chunk()->code[catchOffsetSlot + 1] = static_cast<uint8_t>(relOffset & 0xFF);

                        emit(OpCode::OP_POP, lastLine); // pop error
                        emit(OpCode::OP_NONE, lastLine); // dummy value
                        emit(OpCode::OP_FALSE, lastLine);

                        chunk()->patchJump(skipCatch);

                        if (defExpr) {
                            int hasValJump = chunk()->emitJump(OpCode::OP_JUMP_IF_TRUE, lastLine);
                            emit(OpCode::OP_POP, lastLine); // pop false
                            emit(OpCode::OP_POP, lastLine); // pop none
                            compileNode(defExpr);
                            int endDefJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);

                            chunk()->patchJump(hasValJump);
                            emit(OpCode::OP_POP, lastLine); // pop true

                            chunk()->patchJump(endDefJump);
                        } else {
                            failJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine));
                            emit(OpCode::OP_POP, lastLine); // pop true
                        }

                        addLocal("<pat_tmp>", current().scopeDepth);
                        int tmpSlot = static_cast<int>(current().locals.size()) - 1;
                        emit(OpCode::OP_SET_LOCAL, lastLine); emit16(static_cast<uint16_t>(tmpSlot), lastLine);
                        emit(OpCode::OP_POP, lastLine);
                        
                        compilePatternMatch(actualPat, tmpSlot, failJumps, isConst);
                        current().locals.pop_back();
                    }
                }
            }

            if (mp->restRow && mp->restRow->name.lexeme != "_") {
                emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
                emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(static_cast<double>(rows))), lastLine);
                emit(OpCode::OP_NONE, lastLine);
                emit(OpCode::OP_NONE, lastLine);
                emit(OpCode::OP_NONE, lastLine);
                emit(OpCode::OP_NONE, lastLine);
                emit(OpCode::OP_NONE, lastLine);
                emit(OpCode::OP_SLICE_GET, lastLine); emit(2, lastLine);
                
                bool isExplicitState = false;
                auto it = current().captures.find(mp->restRow->name.lexeme);
                if (it != current().captures.end() && it->second.type == CaptureType::State && it->second.isExplicitState) {
                    isExplicitState = true;
                }
                if (mp->restRow->modifier == ScopeModifier::State || isExplicitState) {
                    int upvalue = resolveUpvalue(mp->restRow->name.lexeme);
                    if (upvalue != -1) {
                        emit(OpCode::OP_GET_UPVALUE, lastLine);
                        emit16(static_cast<uint16_t>(upvalue), lastLine);
                        emit(OpCode::OP_IS_UNINIT, lastLine);
                        int skipJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
                        emit(OpCode::OP_POP, lastLine);
                        emit(OpCode::OP_SET_UPVALUE, lastLine);
                        emit16(static_cast<uint16_t>(upvalue), lastLine);
                        int endJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                        chunk()->patchJump(skipJump);
                        emit(OpCode::OP_POP, lastLine);
                        chunk()->patchJump(endJump);
                    }
                } else {
                    Variable v(mp->restRow->name);
                    emitStoreTarget(&v, isConst || mp->restRow->isConst);
                }
                emit(OpCode::OP_POP, lastLine);
            }
        } else if (auto* dictPat = dynamic_cast<DictPattern*>(p)) {
            emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
            emit(OpCode::OP_MATCH_TYPE, lastLine); emit16(identifierConstant("dict"), lastLine);
            
            emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
            emit(OpCode::OP_MATCH_TYPE, lastLine); emit16(identifierConstant("instance"), lastLine);
            emit(OpCode::OP_BIT_OR, lastLine);

            emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
            emit(OpCode::OP_MATCH_TYPE, lastLine); emit16(identifierConstant("namespace"), lastLine);
            emit(OpCode::OP_BIT_OR, lastLine);
            
            failJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine));
            emit(OpCode::OP_POP, lastLine);

            for (auto& entry : dictPat->entries) {
                Pattern* actualPat = entry.second.get();
                Expr* defExpr = nullptr;
                if (auto* dp_inner = dynamic_cast<DefaultPattern*>(actualPat)) {
                    actualPat = dp_inner->inner.get();
                    defExpr = dp_inner->defaultExpr.get();
                }

                emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
                emit(OpCode::OP_TRY_GET_PROPERTY, lastLine); emit16(identifierConstant(entry.first), lastLine);
                emit16(chunk()->addInlineCache(), lastLine);
                
                if (defExpr) {
                    int hasValJump = chunk()->emitJump(OpCode::OP_JUMP_IF_TRUE, lastLine);
                    emit(OpCode::OP_POP, lastLine); // pop false
                    emit(OpCode::OP_POP, lastLine); // pop none
                    compileNode(defExpr);
                    int endDefJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                    
                    chunk()->patchJump(hasValJump);
                    emit(OpCode::OP_POP, lastLine); // pop true
                    
                    chunk()->patchJump(endDefJump);
                    
                    addLocal("<pat_tmp>", current().scopeDepth);
                    int tmpSlot = static_cast<int>(current().locals.size()) - 1;
                    emit(OpCode::OP_SET_LOCAL, lastLine); emit16(static_cast<uint16_t>(tmpSlot), lastLine);
                    emit(OpCode::OP_POP, lastLine);
                    
                    compilePatternMatch(actualPat, tmpSlot, failJumps, isConst);
                    current().locals.pop_back();
                } else {
                    int failJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
                    
                    // Success path:
                    emit(OpCode::OP_POP, lastLine); // pop true
                    addLocal("<pat_tmp>", current().scopeDepth);
                    int tmpSlot = static_cast<int>(current().locals.size()) - 1;
                    emit(OpCode::OP_SET_LOCAL, lastLine); emit16(static_cast<uint16_t>(tmpSlot), lastLine);
                    emit(OpCode::OP_POP, lastLine); // pop the value
                    
                    compilePatternMatch(actualPat, tmpSlot, failJumps, isConst);
                    current().locals.pop_back();
                    
                    int endJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                    
                    // Fail path:
                    chunk()->patchJump(failJump);
                    emit(OpCode::OP_POP, lastLine); // pop false
                    emit(OpCode::OP_POP, lastLine); // pop none
                    emit(OpCode::OP_FALSE, lastLine); // push false for the outer failJumps handler
                    failJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP, lastLine));
                    
                    chunk()->patchJump(endJump);
                }
            }

            if (dictPat->rest && dictPat->rest->name.lexeme != "_") {
                emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(valSlot), lastLine);
                for (auto& entry : dictPat->entries) {
                    emit(OpCode::OP_CONSTANT, lastLine); emit16(makeConstant(Value(entry.first)), lastLine);
                }
                emit(OpCode::OP_DICT_REST, lastLine);
                emit16(static_cast<uint16_t>(dictPat->entries.size()), lastLine);
                
                bool isExplicitState = false;
                auto it = current().captures.find(dictPat->rest->name.lexeme);
                if (it != current().captures.end() && it->second.type == CaptureType::State && it->second.isExplicitState) {
                    isExplicitState = true;
                }
                if (dictPat->rest->modifier == ScopeModifier::State || isExplicitState) {
                    int upvalue = resolveUpvalue(dictPat->rest->name.lexeme);
                    if (upvalue != -1) {
                        emit(OpCode::OP_GET_UPVALUE, lastLine);
                        emit16(static_cast<uint16_t>(upvalue), lastLine);
                        emit(OpCode::OP_IS_UNINIT, lastLine);
                        int skipJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
                        emit(OpCode::OP_POP, lastLine);
                        emit(OpCode::OP_SET_UPVALUE, lastLine);
                        emit16(static_cast<uint16_t>(upvalue), lastLine);
                        int endJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                        chunk()->patchJump(skipJump);
                        emit(OpCode::OP_POP, lastLine);
                        chunk()->patchJump(endJump);
                    }
                } else {
                    Variable v(dictPat->rest->name);
                    emitStoreTarget(&v, isConst || dictPat->rest->isConst);
                }
                emit(OpCode::OP_POP, lastLine);
            }
        }
    }

    void Compiler::visitDestructAssign(DestructAssign* expr) {
        // 1. Pre-register ref/state names
        std::vector<std::tuple<std::string, ScopeModifier, bool>> boundVars;
        collectPatternVars(expr->pattern.get(), boundVars);

        std::vector<std::string> tempStateNames;
        for (const auto& varTuple : boundVars) {
            const std::string& name = std::get<0>(varTuple);
            ScopeModifier mod = std::get<1>(varTuple);
            bool isConst = std::get<2>(varTuple) || expr->isConst;
            if (mod == ScopeModifier::None) {
                if (expr->isLocal) mod = ScopeModifier::Local;
                else if (expr->isRef) mod = ScopeModifier::Ref;
                else if (expr->isState) mod = ScopeModifier::State;
            }
            
            if (mod == ScopeModifier::Local) {
                if (current().captures.count(name) > 0) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'local' and 'ref'/'state'.");
            } else if (mod == ScopeModifier::Ref) {
                if (current().captures.count(name) > 0 && current().captures[name].type != CaptureType::Ref) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'ref' and 'state'.");
                current().captures[name] = {CaptureType::Ref, isConst, false};
            } else if (mod == ScopeModifier::State) {
                if (current().captures.count(name) > 0 && current().captures[name].type != CaptureType::State) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'ref' and 'state'.");
                tempStateNames.push_back(name);
                current().captures[name] = {CaptureType::State, isConst, true};
            }
        }

        int skipAllJump = -1;
        if (expr->isState && !tempStateNames.empty()) {
            int upvalue = resolveUpvalue(tempStateNames[0]);
            if (upvalue != -1) {
                emit(OpCode::OP_GET_UPVALUE, lastLine);
                emit16(static_cast<uint16_t>(upvalue), lastLine);
                emit(OpCode::OP_IS_UNINIT, lastLine);
                skipAllJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
                emit(OpCode::OP_POP, lastLine); // pop boolean
            }
        }

        std::vector<CaptureModifier> tempMods;
        for (const auto& name : tempStateNames) {
            tempMods.push_back(current().captures[name]);
            current().captures.erase(name);
        }

        // 2. Compile RHS
        compileNode(expr->value.get());

        for (size_t i = 0; i < tempStateNames.size(); ++i) {
            current().captures[tempStateNames[i]] = tempMods[i];
        }

        // 3. Save RHS to a temporary local variable
        addLocal("<destruct_val>", current().scopeDepth);
        int valSlot = static_cast<int>(current().locals.size()) - 1;
        emit(OpCode::OP_SET_LOCAL, lastLine);
        emit16(static_cast<uint16_t>(valSlot), lastLine);
        emit(OpCode::OP_POP, lastLine);

        // 4. Register locals for bound variables
        for (const auto& varTuple : boundVars) {
            const std::string& name = std::get<0>(varTuple);
            ScopeModifier mod = std::get<1>(varTuple);
            bool isConst = std::get<2>(varTuple) || expr->isConst;
            if (mod == ScopeModifier::None) {
                if (expr->isLocal) mod = ScopeModifier::Local;
                else if (expr->isRef) mod = ScopeModifier::Ref;
                else if (expr->isState) mod = ScopeModifier::State;
            }
            
            int existingSlot = resolveLocal(name);
            if (existingSlot != -1 && current().locals[existingSlot].isConst) {
                if (mod != ScopeModifier::Local || current().locals[existingSlot].depth == current().scopeDepth) {
                    uint16_t msgIdx = identifierConstant("Runtime Error: Cannot modify const variable '" + name + "'.");
                    emit(OpCode::OP_CONSTANT, lastLine);
                    emit16(msgIdx, lastLine);
                    emit(OpCode::OP_THROW, lastLine);
                    continue;
                }
            }
            auto capIt = current().captures.find(name);
            if (capIt != current().captures.end() && capIt->second.isConst) {
                if (mod != ScopeModifier::Local) {
                    uint16_t msgIdx = identifierConstant("Runtime Error: Cannot modify const variable '" + name + "'.");
                    emit(OpCode::OP_CONSTANT, lastLine);
                    emit16(msgIdx, lastLine);
                    emit(OpCode::OP_THROW, lastLine);
                    continue;
                }
            }
            if (stateStack.size() == 1) knownGlobals.insert(name);

            int slot = resolveLocal(name);
            if (mod == ScopeModifier::Local) {
                if (slot == -1 || current().locals[slot].depth < current().scopeDepth) {
                    addLocal(name, current().scopeDepth, isConst);
                } else {
                    current().locals[slot].isConst = isConst;
                }
            } else if (mod == ScopeModifier::None) {
                if (stateStack.size() > 1 && slot == -1 && current().captures.count(name) == 0) {
                    addLocal(name, 0, isConst);
                } else if (slot != -1) {
                    current().locals[slot].isConst = isConst;
                }
            }
        }

        // 5. Compile pattern match
        std::vector<int> failJumps;
        compilePatternMatch(expr->pattern.get(), valSlot, failJumps, expr->isConst);

        // 6. Handle match failure
        if (!failJumps.empty()) {
            int successJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
            for (int fj : failJumps) {
                chunk()->patchJump(fj);
            }
            emit(OpCode::OP_POP, lastLine); // pop the boolean from match failure
            uint16_t msgIdx = identifierConstant("TypeError: Destructuring pattern match failed.");
            emit(OpCode::OP_CONSTANT, lastLine);
            emit16(msgIdx, lastLine);
            emit(OpCode::OP_THROW, lastLine);
            chunk()->patchJump(successJump);
        }

        // 7. Restore RHS value to stack top
        emit(OpCode::OP_GET_LOCAL, lastLine);
        emit16(static_cast<uint16_t>(valSlot), lastLine);

        if (skipAllJump != -1) {
            int endJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
            chunk()->patchJump(skipAllJump);
            emit(OpCode::OP_POP, lastLine); // pop boolean
            emit(OpCode::OP_NONE, lastLine); // push none when skipped
            chunk()->patchJump(endJump);
        }

        return;
    }

    void Compiler::visitSwitchExpr(SwitchExpr* expr) {
        compileNode(expr->subject.get());
        std::vector<int> endJumps;

        for (auto& [values, body] : expr->cases) {
            std::vector<int> bodyJumps;
            int noMatchJump = -1;

            for (size_t vi = 0; vi < values.size(); ++vi) {
                emit(OpCode::OP_DUP, lastLine);
                compileNode(values[vi].get());
                emit(OpCode::OP_EQUAL, lastLine);
                int matchJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
                emit(OpCode::OP_POP, lastLine);
                int toBody = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                bodyJumps.push_back(toBody);
                chunk()->patchJump(matchJump);
                emit(OpCode::OP_POP, lastLine);
            }

            noMatchJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
            for (int bj : bodyJumps) chunk()->patchJump(bj);
            emit(OpCode::OP_POP, lastLine); // 弹出 subject
            compileNode(body.get()); // 执行 body，压入 body 结果
            endJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP, lastLine));
            chunk()->patchJump(noMatchJump);
        }
        emit(OpCode::OP_POP, lastLine); // 弹出 subject
        if (expr->defaultBody) compileNode(expr->defaultBody.get());
        else emit(OpCode::OP_NONE, lastLine);

        for (int ej : endJumps) chunk()->patchJump(ej);
        return;
    }

    void Compiler::visitThrowExpr(ThrowExpr* expr) {
        compileNode(expr->value.get());
        emit(OpCode::OP_THROW, lastLine);
        return;
    }

    void Compiler::visitTryCatchExpr(TryCatchExpr* expr) {
        uint16_t catchNameIdx = identifierConstant(expr->catchName.lexeme);
        emit(OpCode::OP_TRY_BEGIN, lastLine);
        int offsetSlot = static_cast<int>(chunk()->code.size());
        emit16(0, lastLine);
        emit16(catchNameIdx, lastLine);
        current().tryDepth++;                 // ★ 进入 try 块
        compileNode(expr->tryBody.get());
        current().tryDepth--;                 // ★ 离开 try 块
        emit(OpCode::OP_TRY_END, lastLine);
        int skipCatch = chunk()->emitJump(OpCode::OP_JUMP, lastLine);

        int catchAddr = static_cast<int>(chunk()->code.size());
        int relOffset = catchAddr - (offsetSlot + 4);
        chunk()->code[offsetSlot] = static_cast<uint8_t>((relOffset >> 8) & 0xFF);
        chunk()->code[offsetSlot + 1] = static_cast<uint8_t>(relOffset & 0xFF);

        int existingSlot = resolveLocal(expr->catchName.lexeme);
        if (existingSlot != -1 && current().locals[existingSlot].isConst && current().locals[existingSlot].depth == current().scopeDepth) {
            uint16_t msgIdx = identifierConstant("Runtime Error: Cannot modify const variable '" + expr->catchName.lexeme + "'.");
            emit(OpCode::OP_CONSTANT, lastLine);
            emit16(msgIdx, lastLine);
            emit(OpCode::OP_THROW, lastLine);
            return;
        }

        if (stateStack.size() == 1) knownGlobals.insert(expr->catchName.lexeme);

        int slot = resolveLocal(expr->catchName.lexeme);
        if (stateStack.size() > 1 && slot == -1 && current().captures.count(expr->catchName.lexeme) == 0) {
            addLocal(expr->catchName.lexeme, current().scopeDepth); // catch 变量严格块级
            slot = resolveLocal(expr->catchName.lexeme);
        }
        if (slot != -1) {
            emit(OpCode::OP_SET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(slot), lastLine);
        }
        else {
            // ★ 修复：在这里补充未定义的 nameIdx
            uint16_t nameIdx = identifierConstant(expr->catchName.lexeme);
            auto it = current().captures.find(expr->catchName.lexeme);
            if (it != current().captures.end() && it->second.type == CaptureType::Ref) {
                emit(OpCode::OP_SET_GLOBAL_REF, lastLine);
            } else {
                emit(OpCode::OP_SET_GLOBAL, lastLine);
            }
            emit16(nameIdx, lastLine);
        }
        emit(OpCode::OP_POP, lastLine);

        compileNode(expr->catchBody.get());
        chunk()->patchJump(skipCatch);
        return;
    }

    void Compiler::visitDictLiteral(DictLiteral* expr) {
        for (size_t i = 0; i < expr->entries.size(); ++i) {
            auto& [keyExpr, valExpr] = expr->entries[i];
            if (!keyExpr) {
                throw std::runtime_error("Compiler Error: Dictionary spread ('...') is not supported in normal dictionary literals.");
            }
            compileNode(keyExpr.get());
            compileNode(valExpr.get());
        }
        emit(OpCode::OP_BUILD_DICT, lastLine);
        emit16(static_cast<uint16_t>(expr->entries.size()), lastLine);
        return;
    }

    void Compiler::visitSetLiteral(SetLiteral* expr) {
        for (auto& elemExpr : expr->elements) {
            compileNode(elemExpr.get());
        }
        emit(OpCode::OP_BUILD_SET, lastLine);
        emit16(static_cast<uint16_t>(expr->elements.size()), lastLine);
        return;
    }

    void Compiler::visitRefDecl(RefDecl* expr) {
        lastLine = expr->name.line;
        const std::string& name = expr->name.lexeme;
        if (current().captures.count(name) > 0 && current().captures[name].type != CaptureType::Ref) {
            throw std::runtime_error("Compiler Error: Cannot declare variable as both 'ref' and 'state'.");
        }
        current().captures[name] = {CaptureType::Ref, expr->isConst, false};
        int upvalue = resolveUpvalue(name);
        
        if (upvalue != -1) {
            emit(OpCode::OP_GET_UPVALUE, lastLine);
            emit16(static_cast<uint16_t>(upvalue), lastLine);
        } else {
            uint16_t idx = identifierConstant(name);
            emit(OpCode::OP_GET_GLOBAL, lastLine);
            emit16(idx, lastLine);
        }
        return;
    }

    void Compiler::visitStateDecl(StateDecl* expr) {
        lastLine = expr->name.line;
        const std::string& name = expr->name.lexeme;
        if (current().captures.count(name) > 0 && current().captures[name].type != CaptureType::State) {
            throw std::runtime_error("Compiler Error: Cannot declare variable as both 'ref' and 'state'.");
        }
        current().captures[name] = {CaptureType::State, expr->isConst, false};
        int upvalue = resolveUpvalue(name);
        
        if (upvalue != -1) {
            emit(OpCode::OP_GET_UPVALUE, lastLine);
            emit16(static_cast<uint16_t>(upvalue), lastLine);
        } else {
            uint16_t idx = identifierConstant(name);
            emit(OpCode::OP_GET_GLOBAL, lastLine);
            emit16(idx, lastLine);
        }
        return;
    }

    void Compiler::visitLocalDecl(LocalDecl* expr) {
        lastLine = expr->name.line;
        const std::string& name = expr->name.lexeme;
        if (current().captures.count(name) > 0) {
            throw std::runtime_error("Compiler Error: Cannot declare variable as both 'local' and 'ref'/'state'.");
        }
        if (expr->isConst) {
            throw std::runtime_error("Compiler Error: 'const' declaration requires '= value'.");
        }
        int slot = resolveLocal(name);
        if (slot == -1 || current().locals[slot].depth < current().scopeDepth) {
            addLocal(name, current().scopeDepth, expr->isConst);
            slot = resolveLocal(name);
        }
        emit(OpCode::OP_NONE, lastLine);
        emit(OpCode::OP_SET_LOCAL, lastLine);
        emit16(static_cast<uint16_t>(slot), lastLine);
        return;
    }

    void Compiler::visitConstDecl(ConstDecl*) {
        throw std::runtime_error("Compiler Error: 'const' declaration requires '= value'.");
    }

    void Compiler::visitDeleteExpr(DeleteExpr* expr) {
        for (auto& tok : expr->names) {
            uint16_t fnIdx = identifierConstant("__vm_delete__");
            emit(OpCode::OP_CONSTANT, lastLine);
            emit16(fnIdx, lastLine);
            chunk()->emitConstant(Value(tok.lexeme), lastLine);
            emit(OpCode::OP_CALL, lastLine);
            emit(static_cast<uint8_t>(1), lastLine);
            emit(OpCode::OP_POP, lastLine);
        }
        emit(OpCode::OP_NONE, lastLine);
        return;
    }

    void Compiler::visitFStringExpr(FStringExpr* expr) {
        int partCount = 0;
        for (size_t i = 0; i < expr->exprs.size(); ++i) {
            if (!expr->literals[i].empty()) {
                chunk()->emitConstant(Value(expr->literals[i]), lastLine);
                partCount++;
            }
            compileNode(expr->exprs[i].get());
            if (!expr->formatSpecs[i].empty()) {
                uint16_t specIdx = makeConstant(Value(expr->formatSpecs[i]));
                emit(OpCode::OP_FORMAT_STRING, lastLine);
                emit16(specIdx, lastLine);
            }
            else {
                emit(OpCode::OP_STRINGIFY, lastLine);
            }
            partCount++;
        }
        if (!expr->literals.back().empty()) {
            chunk()->emitConstant(Value(expr->literals.back()), lastLine);
            partCount++;
        }
        emit(OpCode::OP_CONCAT_STRINGS, lastLine);
        emit16(static_cast<uint16_t>(partCount), lastLine);
        return;
    }

    void Compiler::visitListCompExpr(ListCompExpr* expr) {
        beginScope(); // ★ 列表推导式自带块级作用域
        emit(OpCode::OP_LIST_INIT, lastLine);
        compileCompClause(expr, 0);
        if (!expr->forceList) {
            emit(OpCode::OP_LIST_COMP_END, lastLine); // ★ 底层指令降维
        }
        endScope();
        return;
    }

    void Compiler::visitImportExpr(ImportExpr* expr) {
        compileNode(expr->path.get());
        emit(OpCode::OP_IMPORT, lastLine);
        return;
    }

    void Compiler::visitNamespaceDecl(NamespaceDecl* expr) {
        auto fn = std::make_shared<CompiledFunction>();
        fn->name = "<namespace " + expr->name.lexeme + ">";
        fn->maxArity = 0;
        fn->arity = 0;
        fn->hasRestParam = false;

        compiledFunctions.push_back(fn);
        int thisFnIndex = functionIndexOffset + static_cast<int>(compiledFunctions.size()) - 1;

        initCompiler(fn.get());
        beginScope(); // depth 1

        auto block = static_cast<Block*>(expr->body.get());
        for (size_t i = 0; i < block->statements.size(); ++i) {
            compileNode(block->statements[i].get());
            emit(OpCode::OP_POP, lastLine);
        }

        int count = 0;
        for (auto& local : current().locals) {
            // 仅导出在命名空间中定义的 auto-local 变量 (depth == 0)
            // depth == 1 的 local 变量将作为私有变量被丢弃，不会被导出！
            if (local.depth == 0 && !local.name.empty() && local.name[0] != '<') {
                uint16_t keyIdx = identifierConstant(local.name);
                emit(OpCode::OP_CONSTANT, lastLine);
                emit16(keyIdx, lastLine);

                int slot = resolveLocal(local.name);
                chunk()->emitConstant(Value(static_cast<double>(slot)), lastLine);

                bool isConst = local.isConst;
                chunk()->emitConstant(Value(isConst ? 1.0 : 0.0), lastLine);

                count++;
            }
        }

        uint16_t nsNameIdx = identifierConstant(expr->name.lexeme);
        emit(OpCode::OP_BUILD_NAMESPACE, lastLine);
        emit16(nsNameIdx, lastLine);
        emit16(static_cast<uint16_t>(count), lastLine);
        emit(OpCode::OP_RETURN, lastLine);

        fn->localCount = current().maxLocals;
        endScope();
        stateStack.pop_back();

        uint16_t fnIdx = makeConstant(Value(static_cast<double>(thisFnIndex)));
        emit(OpCode::OP_CLOSURE, lastLine);
        emit16(fnIdx, lastLine);

        emit(OpCode::OP_CALL, lastLine);
        emit(0, lastLine);

        const std::string& name = expr->name.lexeme;
        if (stateStack.size() == 1) knownGlobals.insert(name);

        int slot = resolveLocal(name);
        if (stateStack.size() > 1 && slot == -1 && current().captures.count(name) == 0) {
            addLocal(name, 0);
            slot = resolveLocal(name);
        }

        if (slot != -1) {
            emit(OpCode::OP_SET_LOCAL, lastLine);
            emit16(static_cast<uint16_t>(slot), lastLine);
        } else {
            int upvalue = resolveUpvalue(name);
            if (upvalue != -1) {
                emit(OpCode::OP_SET_UPVALUE, lastLine);
                emit16(static_cast<uint16_t>(upvalue), lastLine);
            } else {
                uint16_t nameIdx = identifierConstant(name);
                auto it = current().captures.find(name);
                if (it != current().captures.end() && it->second.type == CaptureType::Ref) {
                    emit(OpCode::OP_SET_GLOBAL_REF, lastLine);
                } else {
                    emit(OpCode::OP_SET_GLOBAL, lastLine);
                }
                emit16(nameIdx, lastLine);
            }
        }

        return;
    }

    void Compiler::visitClassDefExpr(ClassDefExpr* expr) {
        lastLine = expr->name.line;
        const std::string& className = expr->name.lexeme;
        uint16_t nameIdx = identifierConstant(className);

        // 1. 生成空的类定义对象
        emit(OpCode::OP_CLASS, lastLine);
        emit16(nameIdx, lastLine);

        // 2. 将类保存进环境（局部/闭包/全局 安全判定）
        int slot = resolveLocal(className);
        if (stateStack.size() > 1 && slot == -1 && current().captures.count(className) == 0) {
            addLocal(className, 0); // 类名自动溢出到函数作用域
            slot = resolveLocal(className);
        }
        if (slot != -1) {
            emit(OpCode::OP_SET_LOCAL, lastLine); emit16(static_cast<uint16_t>(slot), lastLine);
        }
        else {
            int upvalue = resolveUpvalue(className);
            if (upvalue != -1) { emit(OpCode::OP_SET_UPVALUE, lastLine); emit16(static_cast<uint16_t>(upvalue), lastLine); }
            else { 
                auto it = current().captures.find(className);
                if (it != current().captures.end() && it->second.type == CaptureType::Ref) {
                    emit(OpCode::OP_SET_GLOBAL_REF, lastLine);
                } else {
                    emit(OpCode::OP_SET_GLOBAL, lastLine); 
                }
                emit16(nameIdx, lastLine); 
            }
        }

        // ★ 智能加载宏：无论这个类在何处，精准找到它
        auto emitLoadClass = [&]() {
            if (slot != -1) { emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(slot), lastLine); }
            else {
                int upvalue = resolveUpvalue(className);
                if (upvalue != -1) { emit(OpCode::OP_GET_UPVALUE, lastLine); emit16(static_cast<uint16_t>(upvalue), lastLine); }
                else { emit(OpCode::OP_GET_GLOBAL, lastLine); emit16(nameIdx, lastLine); }
            }
            };

        // 3. 继承逻辑（修复：彻底兼容局部/闭包父类）
        if (!expr->superClassName.empty()) {
            uint16_t superIdx = identifierConstant(expr->superClassName);
            emitLoadClass(); // 取出当前子类

            int sSlot = resolveLocal(expr->superClassName);
            if (sSlot != -1) { emit(OpCode::OP_GET_LOCAL, lastLine); emit16(static_cast<uint16_t>(sSlot), lastLine); }
            else {
                int sUpvalue = resolveUpvalue(expr->superClassName);
                if (sUpvalue != -1) { emit(OpCode::OP_GET_UPVALUE, lastLine); emit16(static_cast<uint16_t>(sUpvalue), lastLine); }
                else { emit(OpCode::OP_GET_GLOBAL, lastLine); emit16(superIdx, lastLine); }
            }
            emit(OpCode::OP_INHERIT, lastLine);
        }

        // 4. 方法注册逻辑
        for (auto& md : expr->methods) {
            auto fn = std::make_shared<CompiledFunction>();
            fn->name = md.name.lexeme;
            fn->maxArity = static_cast<int>(md.params.size());
            fn->hasRestParam = md.hasRestParam;

            int requiredParams = 0;
            for (size_t i = 0; i < md.params.size(); ++i) {
                if (i >= md.defaultExprs.size() || !md.defaultExprs[i]) {
                    if (md.hasRestParam && i == md.params.size() - 1) break;
                    requiredParams++;
                }
                else break;
            }
            fn->arity = requiredParams;

            compiledFunctions.push_back(fn);

            int thisFnIndex = functionIndexOffset + static_cast<int>(compiledFunctions.size()) - 1;

            initCompiler(fn.get());
            beginScope();
            int refIdx = 0;
            for (size_t i = 0; i < md.params.size(); ++i) {
                addLocal(md.params[i].lexeme, current().scopeDepth);
                if (md.paramIsRef[i]) {
                    current().locals.back().isRefParam = true;
                    current().locals.back().refParamIndex = refIdx++;
                }
            }
            fn->paramIsRef = md.paramIsRef;

            emitDefaultPreamble(md.defaultExprs, fn->maxArity);

            // ★ 幽灵注入：参数类型检查 (换用专属变量名防遮蔽)
            for (size_t i = 0; i < md.params.size(); ++i) {
                if (i < md.paramTypes.size() && !md.paramTypes[i].empty()) {
                    int paramSlot = resolveLocal(md.params[i].lexeme);
                    if (current().locals[paramSlot].isRefParam) {
                        emit(OpCode::OP_GET_REF_PARAM, lastLine);
                        emit16(static_cast<uint16_t>(current().locals[paramSlot].refParamIndex), lastLine);
                    } else {
                        emit(OpCode::OP_GET_LOCAL, lastLine);
                        emit16(static_cast<uint16_t>(paramSlot), lastLine);
                    }

                    uint16_t paramTypeIdx = identifierConstant(md.paramTypes[i]);
                    uint16_t paramNameIdx = identifierConstant(md.params[i].lexeme);
                    emit(OpCode::OP_ASSERT_PARAM_TYPE, lastLine);
                    emit16(paramTypeIdx, lastLine);
                    emit16(paramNameIdx, lastLine);
                }
            }
            current().expectedReturnType = md.returnType;
            compileNode(md.body.get());
            if (!md.returnType.empty()) {
                uint16_t retTypeIdx = identifierConstant(md.returnType);
                emit(OpCode::OP_ASSERT_RETURN_TYPE, lastLine);
                emit16(retTypeIdx, lastLine);
            }
            emit(OpCode::OP_RETURN, lastLine); // (不用动)

            fn->localCount = current().maxLocals;
            endScope();
            stateStack.pop_back();

            // ★ 修复：放弃无脑 GET_GLOBAL，智能获取正处于挂载态的方法主类
            emitLoadClass();

            uint16_t fnIdx = makeConstant(Value(static_cast<double>(thisFnIndex)));
            emit(OpCode::OP_CLOSURE, lastLine); emit16(fnIdx, lastLine);

            uint16_t methodNameIdx = identifierConstant(md.name.lexeme);
            emit(OpCode::OP_METHOD, lastLine); emit16(methodNameIdx, lastLine);

            emit(OpCode::OP_POP, lastLine);
        }
        return;
    }

    void Compiler::visitDotAccess(DotAccess* expr) {
        lastLine = expr->field.line;
        if (dynamic_cast<SuperExpr*>(expr->object.get())) {
            emit(OpCode::OP_GET_SELF, lastLine);
            uint16_t nameIdx = identifierConstant(expr->field.lexeme);
            emit(OpCode::OP_GET_SUPER, lastLine); emit16(nameIdx, lastLine);
            return;
        }

        compileNode(expr->object.get());
        uint16_t nameIdx = identifierConstant(expr->field.lexeme);
        emit(OpCode::OP_GET_PROPERTY, lastLine);
        emit16(nameIdx, lastLine);
        emit16(chunk()->addInlineCache(), lastLine);
        return;
    }

    void Compiler::visitDotAssign(DotAssign* expr) {
        lastLine = expr->field.line;
        
        compileNode(expr->object.get());
        compileNode(expr->value.get());
        
        uint16_t nameIdx = identifierConstant(expr->field.lexeme);
        emit(OpCode::OP_SET_PROPERTY, lastLine);
        emit16(nameIdx, lastLine);
        emit16(chunk()->addInlineCache(), lastLine);
        
        return;
    }

    void Compiler::visitMethodCallExpr(MethodCallExpr* expr) {
        lastLine = expr->method.line;
        if (dynamic_cast<SuperExpr*>(expr->object.get())) {
            emit(OpCode::OP_GET_SELF, lastLine);
            for (auto& arg : expr->arguments) compileNode(arg.get());
            uint16_t nameIdx = identifierConstant(expr->method.lexeme);

            bool hasVariableArgs = false;
            for (auto& argExpr : expr->arguments) {
                if (dynamic_cast<Variable*>(argExpr.get())) { hasVariableArgs = true; break; }
            }

            struct ArgSource { uint8_t argIndex; uint8_t sourceType; uint16_t sourceRef; };
            std::vector<ArgSource> sources;

            if (hasVariableArgs) {
                for (int i = 0; i < static_cast<int>(expr->arguments.size()); ++i) {
                    if (auto* varExpr = dynamic_cast<Variable*>(expr->arguments[i].get())) {
                        int localSlot = resolveLocal(varExpr->name.lexeme);
                        if (localSlot != -1) {
                            if (current().locals[localSlot].isRefParam) {
                                sources.push_back({ static_cast<uint8_t>(i), 4, static_cast<uint16_t>(current().locals[localSlot].refParamIndex) });
                            } else {
                                sources.push_back({ static_cast<uint8_t>(i), 2, static_cast<uint16_t>(localSlot) });
                            }
                        }
                        else {
                            int uv = resolveUpvalue(varExpr->name.lexeme);
                            if (uv != -1) sources.push_back({ static_cast<uint8_t>(i), 3, static_cast<uint16_t>(uv) });
                            else sources.push_back({ static_cast<uint8_t>(i), 1, identifierConstant(varExpr->name.lexeme) });
                        }
                    }
                }
            }

            bool actualTailCall = inTailPosition && sources.empty();

            if (!sources.empty()) {
                emit(OpCode::OP_PASS_REFS, lastLine);
                emit(static_cast<uint8_t>(sources.size()), lastLine);
                for (auto& s : sources) { emit(s.argIndex, lastLine); emit(s.sourceType, lastLine); emit16(s.sourceRef, lastLine); }
            }

            if (actualTailCall) {
                emit(OpCode::OP_TAIL_SUPER_INVOKE, lastLine);
                emit16(nameIdx, lastLine);
                emit(static_cast<uint8_t>(expr->arguments.size()), lastLine);
                tailCallEmitted = true;
            } else {
                emit(OpCode::OP_SUPER_INVOKE, lastLine);
                emit16(nameIdx, lastLine);
                emit(static_cast<uint8_t>(expr->arguments.size()), lastLine);
            }
            return;
        }

        compileNode(expr->object.get());
        for (auto& arg : expr->arguments) compileNode(arg.get());
        uint16_t nameIdx = identifierConstant(expr->method.lexeme);

        bool hasVariableArgs = false;
        for (auto& argExpr : expr->arguments) {
            if (dynamic_cast<Variable*>(argExpr.get())) { hasVariableArgs = true; break; }
        }

        struct ArgSource { uint8_t argIndex; uint8_t sourceType; uint16_t sourceRef; };
        std::vector<ArgSource> sources;

        if (hasVariableArgs) {
            for (int i = 0; i < static_cast<int>(expr->arguments.size()); ++i) {
                if (auto* varExpr = dynamic_cast<Variable*>(expr->arguments[i].get())) {
                    int localSlot = resolveLocal(varExpr->name.lexeme);
                    if (localSlot != -1) {
                        if (current().locals[localSlot].isRefParam) {
                            sources.push_back({ static_cast<uint8_t>(i), 4, static_cast<uint16_t>(current().locals[localSlot].refParamIndex) });
                        } else {
                            sources.push_back({ static_cast<uint8_t>(i), 2, static_cast<uint16_t>(localSlot) });
                        }
                    }
                    else {
                        int uv = resolveUpvalue(varExpr->name.lexeme);
                        if (uv != -1) sources.push_back({ static_cast<uint8_t>(i), 3, static_cast<uint16_t>(uv) });
                        else sources.push_back({ static_cast<uint8_t>(i), 1, identifierConstant(varExpr->name.lexeme) });
                    }
                }
            }
        }

        bool actualTailCall = inTailPosition && sources.empty();

        if (!sources.empty()) {
            emit(OpCode::OP_PASS_REFS, lastLine);
            emit(static_cast<uint8_t>(sources.size()), lastLine);
            for (auto& s : sources) { emit(s.argIndex, lastLine); emit(s.sourceType, lastLine); emit16(s.sourceRef, lastLine); }
        }

        if (actualTailCall) {
            emit(OpCode::OP_TAIL_INVOKE, lastLine);
            emit16(nameIdx, lastLine);
            emit(static_cast<uint8_t>(expr->arguments.size()), lastLine);
            emit16(chunk()->addInlineCache(), lastLine);
            tailCallEmitted = true;
        } else {
            emit(OpCode::OP_INVOKE, lastLine);
            emit16(nameIdx, lastLine);
            emit(static_cast<uint8_t>(expr->arguments.size()), lastLine);
            emit16(chunk()->addInlineCache(), lastLine);
        }
        return;
    }

    void Compiler::visitSuperExpr(SuperExpr*) {
        throw std::runtime_error("Compiler Error: 'super' must be followed by '.method()'.");
    }

    void Compiler::visitSelfExpr(SelfExpr*) {
        emit(OpCode::OP_GET_SELF, lastLine);
        return;
    }

    void Compiler::visitSliceExpr(SliceExpr*) {
        throw std::runtime_error("Compiler Error: Slice expression should be handled by visitIndexAccess.");
    }

    void Compiler::visitSequenceExpr(SequenceExpr* expr) {
        for (size_t i = 0; i < expr->expressions.size(); ++i) {
            compileNode(expr->expressions[i].get());

            // 除了最后一个表达式，其余的执行完后都要清理栈（丢弃结果）
            if (i < expr->expressions.size() - 1) {
                emit(OpCode::OP_POP, lastLine);
            }
        }
        // 最后一个表达式的结果自然留在栈顶供上层读取
        return;
    }

    void Compiler::visitGroupingExpr(GroupingExpr* expr) {
        compileNode(expr->expression.get());
        return;
    }

    void Compiler::visitMatchExpr(MatchExpr* expr) {
        compileNode(expr->subject.get());
        
        addLocal("<match_subject>", current().scopeDepth);
        int subjectSlot = static_cast<int>(current().locals.size()) - 1;
        emit(OpCode::OP_SET_LOCAL, lastLine);
        emit16(static_cast<uint16_t>(subjectSlot), lastLine);
        emit(OpCode::OP_POP, lastLine);

        std::vector<int> endJumps;

        for (auto& branch : expr->branches) {
            beginScope();

            std::vector<std::tuple<std::string, ScopeModifier, bool>> boundVars;
            for (auto& pat : branch.patterns) collectPatternVars(pat.get(), boundVars);

            for (const auto& varTuple : boundVars) {
                const std::string& var = std::get<0>(varTuple);
                ScopeModifier mod = std::get<1>(varTuple);
                bool isConst = std::get<2>(varTuple);
                
                if (mod == ScopeModifier::Local) {
                    if (current().captures.count(var) > 0) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'local' and 'ref'/'state'.");
                } else if (mod == ScopeModifier::Ref) {
                    if (current().captures.count(var) > 0 && current().captures[var].type != CaptureType::Ref) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'ref' and 'state'.");
                    current().captures[var] = {CaptureType::Ref, isConst, false};
                } else if (mod == ScopeModifier::State) {
                    if (current().captures.count(var) > 0 && current().captures[var].type != CaptureType::State) throw std::runtime_error("Compiler Error: Cannot declare variable as both 'ref' and 'state'.");
                    current().captures[var] = {CaptureType::State, isConst, true};
                }

                int slot = resolveLocal(var);
                if (mod == ScopeModifier::Local) {
                    if (slot == -1 || current().locals[slot].depth < current().scopeDepth) {
                        addLocal(var, current().scopeDepth, isConst);
                    }
                } else if (mod == ScopeModifier::None) {
                    if (slot == -1 || current().locals[slot].depth < current().scopeDepth) {
                        addLocal(var, current().scopeDepth, isConst);
                    }
                }
            }

            int nextBranchJump = -1;
            std::vector<int> bodyJumps;

            for (size_t pi = 0; pi < branch.patterns.size(); ++pi) {
                auto& pat = branch.patterns[pi];
                std::vector<int> failJumps;

                compilePatternMatch(pat.get(), subjectSlot, failJumps, false);

                bodyJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP, lastLine));

                for (int fj : failJumps) {
                    chunk()->patchJump(fj);
                }
                if (!failJumps.empty()) {
                    emit(OpCode::OP_POP, lastLine);
                }
            }

            nextBranchJump = chunk()->emitJump(OpCode::OP_JUMP, lastLine);

            for (int bj : bodyJumps) {
                chunk()->patchJump(bj);
            }

            if (branch.guard) {
                compileNode(branch.guard.get());
                int guardFailJump = chunk()->emitJump(OpCode::OP_JUMP_IF_FALSE, lastLine);
                emit(OpCode::OP_POP, lastLine);
                
                compileNode(branch.body.get());
                endJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP, lastLine));

                chunk()->patchJump(guardFailJump);
                emit(OpCode::OP_POP, lastLine);
                
                int guardToNextBranch = chunk()->emitJump(OpCode::OP_JUMP, lastLine);
                chunk()->patchJump(nextBranchJump);
                chunk()->patchJump(guardToNextBranch);
            } else {
                compileNode(branch.body.get());
                endJumps.push_back(chunk()->emitJump(OpCode::OP_JUMP, lastLine));
                chunk()->patchJump(nextBranchJump);
            }

            endScope();
        }

        emit(OpCode::OP_NONE, lastLine);

        for (int ej : endJumps) {
            chunk()->patchJump(ej);
        }

        return;
    }

}
