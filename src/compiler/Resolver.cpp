#include "Resolver.h"
#include <set>

namespace jc {

void Resolver::checkExplicitDecl(void* node, const std::string& name) {
    if (name == "_") return;
    if (scopes.empty()) return;
    if (checkedDecls.count(node)) return;
    checkedDecls.insert(node);
    
    if (scopes.back().lexicalDecls.count(name)) {
        throw std::runtime_error("SyntaxError: Variable '" + name + "' has already been declared in this scope.");
    }
}

void Resolver::beginScope(bool isFunc, bool isNamespace) {
    Scope s;
    s.isFunctionScope = isFunc;
    s.isNamespaceScope = isNamespace;
    scopes.push_back(s);
}

void Resolver::endScope() {
    scopes.pop_back();
}

void Resolver::declareVariable(const std::string& name, VarScope scopeType, bool isConst, bool isExplicitLocal) {
    if (name == "_") return;
    if (scopes.empty()) return;
    
    scopes.back().lexicalDecls.insert(name);
    
    ResolvedSym sym;
    sym.scope = scopeType;
    sym.isConst = isConst;
    
    if (scopeType == VarScope::Local && !isExplicitLocal) {
        // Auto-local: 先检查当前函数/命名空间内是否已经有这个变量
        for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
            if (scopes[i].symbols.count(name)) return;
            if (scopes[i].isFunctionScope || scopes[i].isNamespaceScope || i == 0) break;
        }
        
        // 提升到最近的函数或命名空间作用域
        for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
            if (scopes[i].isFunctionScope || scopes[i].isNamespaceScope || i == 0) {
                if (i == 0 && !scopes[0].isFunctionScope && !scopes[0].isNamespaceScope) {
                    sym.scope = VarScope::Global;
                }
                if (scopes[i].symbols.find(name) == scopes[i].symbols.end()) {
                    sym.depth = i;
                    scopes[i].symbols[name] = sym;
                }
                return;
            }
        }
    } else if (scopeType == VarScope::State) {
        // State 绑定到最近的函数作用域
        for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
            if (scopes[i].isFunctionScope || i == 0) {
                if (scopes[i].symbols.find(name) == scopes[i].symbols.end()) {
                    sym.depth = i;
                    scopes[i].symbols[name] = sym;
                }
                return;
            }
        }
    }
    
    sym.depth = static_cast<int>(scopes.size()) - 1;
    scopes.back().symbols[name] = sym;
}

ResolvedSym Resolver::resolveName(const std::string& name) {
    for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
        if (scopes[i].symbols.count(name)) {
            ResolvedSym sym = scopes[i].symbols[name];
            // 检查是否跨越了函数边界（闭包捕获）
            bool crossedFunction = false;
            for (int j = static_cast<int>(scopes.size()) - 1; j > i; --j) {
                if (scopes[j].isFunctionScope) {
                    crossedFunction = true;
                    break;
                }
            }
            if (crossedFunction) {
                if (sym.scope == VarScope::State) {
                    sym.scope = VarScope::CapturedState;
                } else if (sym.scope == VarScope::Local || sym.scope == VarScope::RefParam) {
                    sym.scope = VarScope::Upvalue;
                }
            }
            return sym;
        }
    }
    return ResolvedSym{VarScope::Global, -1, -1, false};
}

void Resolver::resolve(Expr* expr) {
    if (expr) expr->accept(*this);
}

void Resolver::visitBlock(Block* expr) {
    beginScope();
    hoistBlock(expr);
    for (auto& stmt : expr->statements) {
        resolve(stmt.get());
    }
    endScope();
}

void Resolver::hoistBlock(Block* block) {
    // 预扫描块内的变量声明，实现 Auto-Local 和显式声明的提升
    for (auto& stmt : block->statements) {
        if (auto* assign = dynamic_cast<Assign*>(stmt.get())) {
            if (assign->isState || assign->isRef || assign->isLocal || assign->isConst) {
                checkExplicitDecl(assign, assign->name.lexeme);
            }
            if (assign->isState) declareVariable(assign->name.lexeme, VarScope::State, assign->isConst);
            else if (assign->isRef) {
                ResolvedSym existing = resolveName(assign->name.lexeme);
                declareVariable(assign->name.lexeme, existing.scope, assign->isConst, true);
            }
            else if (assign->isLocal) declareVariable(assign->name.lexeme, VarScope::Local, assign->isConst, true);
            else declareVariable(assign->name.lexeme, VarScope::Local, assign->isConst, false);
        } else if (auto* locDecl = dynamic_cast<LocalDecl*>(stmt.get())) {
            checkExplicitDecl(locDecl, locDecl->name.lexeme);
            exprSymbols[locDecl] = resolveName(locDecl->name.lexeme);  // 捕获外层符号（declareVariable 之前）
            declareVariable(locDecl->name.lexeme, VarScope::Local, locDecl->isConst, true);
        } else if (auto* stateDecl = dynamic_cast<StateDecl*>(stmt.get())) {
            checkExplicitDecl(stateDecl, stateDecl->name.lexeme);
            declareVariable(stateDecl->name.lexeme, VarScope::State, stateDecl->isConst);
        } else if (auto* refDecl = dynamic_cast<RefDecl*>(stmt.get())) {
            checkExplicitDecl(refDecl, refDecl->name.lexeme);
            ResolvedSym existing = resolveName(refDecl->name.lexeme);
            declareVariable(refDecl->name.lexeme, existing.scope, refDecl->isConst, true);
        } else if (auto* constDecl = dynamic_cast<ConstDecl*>(stmt.get())) {
            checkExplicitDecl(constDecl, constDecl->name.lexeme);
            declareVariable(constDecl->name.lexeme, VarScope::Local, true, false);
        } else if (auto* destAssign = dynamic_cast<DestructAssign*>(stmt.get())) {
            ScopeModifier mod = ScopeModifier::None;
            if (destAssign->isLocal) mod = ScopeModifier::Local;
            else if (destAssign->isRef) mod = ScopeModifier::Ref;
            else if (destAssign->isState) mod = ScopeModifier::State;
            resolvePattern(destAssign->pattern.get(), true, mod, destAssign->isConst);
        }
    }
}

void Resolver::visitVariable(Variable* expr) {
    if (expr->name.lexeme == "_") {
        throw std::runtime_error("SyntaxError: '_' is a placeholder and cannot be read.");
    }
    exprSymbols[expr] = resolveName(expr->name.lexeme);
}

void Resolver::visitAssign(Assign* expr) {
    if (expr->isLocal || expr->isState || expr->isRef || expr->isConst) {
        checkExplicitDecl(expr, expr->name.lexeme);
    }
    bool isFuncDef = dynamic_cast<LambdaExpr*>(expr->value.get()) != nullptr;
    bool hidden = false;
    ResolvedSym hiddenSym;
    int hiddenDepth = -1;
    
    if (!isFuncDef && (expr->isLocal || expr->isState || expr->isConst)) {
        for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
            auto it = scopes[i].symbols.find(expr->name.lexeme);
            if (it != scopes[i].symbols.end()) {
                hiddenSym = it->second;
                hiddenDepth = i;
                scopes[i].symbols.erase(it);
                hidden = true;
                break;
            }
        }
    }

    if (expr->typeHint) resolve(expr->typeHint.get());
    resolve(expr->value.get());

    if (hidden) {
        scopes[hiddenDepth].symbols[expr->name.lexeme] = hiddenSym;
    }

    if (expr->isState) declareVariable(expr->name.lexeme, VarScope::State, expr->isConst);
    else if (expr->isRef) {
        ResolvedSym existing = resolveName(expr->name.lexeme);
        declareVariable(expr->name.lexeme, existing.scope, expr->isConst, true);
    }
    else if (expr->isLocal) declareVariable(expr->name.lexeme, VarScope::Local, expr->isConst, true);
    else declareVariable(expr->name.lexeme, VarScope::Local, expr->isConst, false);
    exprSymbols[expr] = resolveName(expr->name.lexeme);
}

void Resolver::visitTypeAssertExpr(TypeAssertExpr* expr) {
    resolve(expr->value.get());      // value 是 Variable，走 visitVariable → resolveName（含 upvalue 检测）
    resolve(expr->typeHint.get());
}

void Resolver::visitBinary(Binary* expr) {
    resolve(expr->left.get());
    resolve(expr->right.get());
}

void Resolver::visitUnary(Unary* expr) {
    resolve(expr->right.get());
}

void Resolver::visitLiteral(Literal* /*expr*/) {}

void Resolver::visitCall(Call* expr) {
    exprSymbols[expr] = resolveName(expr->callee.lexeme);
    for (auto& arg : expr->arguments) resolve(arg.get());
}

void Resolver::visitMatrixNode(MatrixNode* expr) {
    for (auto& row : expr->elements) {
        for (auto& e : row) resolve(e.get());
    }
}

void Resolver::visitIfExpr(IfExpr* expr) {
    resolve(expr->condition.get());
    resolve(expr->thenBranch.get());
    if (expr->elseBranch) resolve(expr->elseBranch.get());
}

void Resolver::visitWhileExpr(WhileExpr* expr) {
    resolve(expr->condition.get());
    resolve(expr->body.get());
}

void Resolver::visitForExpr(ForExpr* expr) {
    beginScope();
    resolve(expr->initializer.get());
    resolve(expr->condition.get());
    resolve(expr->update.get());
    resolve(expr->body.get());
    endScope();
}

void Resolver::visitBreakExpr(BreakExpr* /*expr*/) {}
void Resolver::visitContinueExpr(ContinueExpr* /*expr*/) {}

void Resolver::visitReturnExpr(ReturnExpr* expr) {
    if (expr->value) resolve(expr->value.get());
}

void Resolver::visitIndexAccess(IndexAccess* expr) {
    resolve(expr->object.get());
    for (auto& idx : expr->indices) resolve(idx.get());
}

void Resolver::visitIndexAssign(IndexAssign* expr) {
    if (expr->objectExpr) resolve(expr->objectExpr.get());
    else exprSymbols[expr] = resolveName(expr->name.lexeme);
    for (auto& chain : expr->indexChain) {
        for (auto& idx : chain) resolve(idx.get());
    }
    resolve(expr->value.get());
    if (expr->typeHint) resolve(expr->typeHint.get());
}

void Resolver::visitLocalDecl(LocalDecl* expr) {
    checkExplicitDecl(expr, expr->name.lexeme);
    // 外层符号已在 hoistBlock 中存入 exprSymbols（在 declareVariable 之前），此处不再重复解析
    declareVariable(expr->name.lexeme, VarScope::Local, expr->isConst, true);
    if (expr->typeHint) resolve(expr->typeHint.get());
}
void Resolver::visitRefDecl(RefDecl* expr) {
    checkExplicitDecl(expr, expr->name.lexeme);
    ResolvedSym existing = resolveName(expr->name.lexeme);
    declareVariable(expr->name.lexeme, existing.scope, expr->isConst, true);
    exprSymbols[expr] = resolveName(expr->name.lexeme);
    if (expr->typeHint) resolve(expr->typeHint.get());
}
void Resolver::visitStateDecl(StateDecl* expr) {
    checkExplicitDecl(expr, expr->name.lexeme);
    declareVariable(expr->name.lexeme, VarScope::State, expr->isConst);
    if (expr->typeHint) resolve(expr->typeHint.get());
}
void Resolver::visitConstDecl(ConstDecl* expr) { checkExplicitDecl(expr, expr->name.lexeme); declareVariable(expr->name.lexeme, VarScope::Local, true, false); }

void Resolver::visitDeleteExpr(DeleteExpr* /*expr*/) {}

void Resolver::visitCompoundAssign(CompoundAssign* expr) {
    resolve(expr->target.get());
    resolve(expr->value.get());
}

void Resolver::visitLambdaExpr(LambdaExpr* expr) {
    for (auto& pt : expr->paramTypes) {
        if (pt) resolve(pt.get());
    }
    if (expr->returnType) resolve(expr->returnType.get());

    beginScope(true, false);
    for (size_t i = 0; i < expr->params.size(); ++i) {
        if (expr->params[i].lexeme != "_" && scopes.back().lexicalDecls.count(expr->params[i].lexeme)) {
            throw std::runtime_error("SyntaxError: Parameter '" + expr->params[i].lexeme + "' has already been declared.");
        }
        
        VarScope scope = expr->paramIsRef[i] ? VarScope::RefParam : VarScope::Local;
        declareVariable(expr->params[i].lexeme, scope, expr->paramIsConst[i], true);
        if (expr->defaultExprs[i]) resolve(expr->defaultExprs[i].get());
    }
    resolve(expr->body.get());
    endScope();
}

void Resolver::visitInvokeExpr(InvokeExpr* expr) {
    resolve(expr->callee.get());
    for (auto& arg : expr->arguments) resolve(arg.get());
}

void Resolver::visitForInExpr(ForInExpr* expr) {
    resolve(expr->iterable.get());
    beginScope();
    ScopeModifier mod = expr->isLocal ? ScopeModifier::Local : ScopeModifier::None;
    resolvePattern(expr->pattern.get(), true, mod, expr->isConst);
    resolve(expr->body.get());
    endScope();
}

void Resolver::visitThrowExpr(ThrowExpr* expr) {
    resolve(expr->value.get());
}

void Resolver::visitTryCatchExpr(TryCatchExpr* expr) {
    resolve(expr->tryBody.get());
    beginScope();
    resolvePattern(expr->catchPattern.get(), true, ScopeModifier::Local, false);
    resolve(expr->catchBody.get());
    endScope();
}

void Resolver::visitImportExpr(ImportExpr* expr) {
    resolve(expr->path.get());
}

void Resolver::visitSwitchExpr(SwitchExpr* expr) {
    resolve(expr->subject.get());
    for (auto& c : expr->cases) {
        for (auto& v : c.first) resolve(v.get());
        resolve(c.second.get());
    }
    if (expr->defaultBody) resolve(expr->defaultBody.get());
}

void Resolver::visitClassDefExpr(ClassDefExpr* expr) {
    if (expr->superClassExpr) resolve(expr->superClassExpr.get());
    
    beginScope(false, false);
    declareVariable("<class>", VarScope::Local, true, true);
    if (!expr->name.lexeme.empty()) {
        declareVariable(expr->name.lexeme, VarScope::Local, true, true);
    }
    
    for (auto& p : expr->staticProperties) {
        resolve(p.value.get());
    }
    for (auto& p : expr->instanceProperties) {
        resolve(p.value.get());
    }
    
    endScope();
}

void Resolver::visitNamespaceDecl(NamespaceDecl* expr) {
    beginScope(false, true);
    declareVariable("<namespace>", VarScope::Local, true, true);
    if (!expr->name.lexeme.empty()) {
        declareVariable(expr->name.lexeme, VarScope::Local, true, true);
    }
    resolve(expr->body.get());
    endScope();
}

void Resolver::visitEnumDefExpr(EnumDefExpr* expr) {
    for (auto& member : expr->members) {
        if (member.second) {
            resolve(member.second.get());
        }
    }
}

void Resolver::visitDotAccess(DotAccess* expr) {
    resolve(expr->object.get());
}

void Resolver::visitDotAssign(DotAssign* expr) {
    resolve(expr->object.get());
    resolve(expr->value.get());
    if (expr->typeHint) resolve(expr->typeHint.get());
}

void Resolver::visitMethodCallExpr(MethodCallExpr* expr) {
    resolve(expr->object.get());
    for (auto& arg : expr->arguments) resolve(arg.get());
}

void Resolver::visitSuperExpr(SuperExpr* /*expr*/) {}
void Resolver::visitSelfExpr(SelfExpr* /*expr*/) {}

void Resolver::visitContextKeywordExpr(ContextKeywordExpr* expr) {
    const char* internal = (expr->kind == ContextKeywordExpr::Kind::Class) ? "<class>" : "<namespace>";
    exprSymbols[expr] = resolveName(internal);
}

void Resolver::visitDestructAssign(DestructAssign* expr) {
    std::vector<std::pair<std::string, std::pair<int, ResolvedSym>>> hiddenVars;

    std::vector<std::string> patVarsToHide;
    auto collectVars = [&](Pattern* p, auto& self) -> void {
        if (!p) return;
        if (auto* vp = dynamic_cast<VariablePattern*>(p)) {
            if (vp->name.lexeme != "_") {
                if (expr->isLocal || expr->isState || expr->isConst || vp->modifier == ScopeModifier::Local || vp->modifier == ScopeModifier::State || vp->isConst) {
                    patVarsToHide.push_back(vp->name.lexeme);
                }
            }
        } else if (auto* rp = dynamic_cast<RestPattern*>(p)) {
            if (rp->name.lexeme != "_") {
                if (expr->isLocal || expr->isState || expr->isConst || rp->modifier == ScopeModifier::Local || rp->modifier == ScopeModifier::State || rp->isConst) {
                    patVarsToHide.push_back(rp->name.lexeme);
                }
            }
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

    for (const auto& name : patVarsToHide) {
        for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
            auto it = scopes[i].symbols.find(name);
            if (it != scopes[i].symbols.end()) {
                hiddenVars.push_back({name, {i, it->second}});
                scopes[i].symbols.erase(it);
                break;
            }
        }
    }

    resolve(expr->value.get());

    for (const auto& hv : hiddenVars) {
        scopes[hv.second.first].symbols[hv.first] = hv.second.second;
    }

    ScopeModifier mod = ScopeModifier::None;
    if (expr->isLocal) mod = ScopeModifier::Local;
    else if (expr->isRef) mod = ScopeModifier::Ref;
    else if (expr->isState) mod = ScopeModifier::State;
    resolvePattern(expr->pattern.get(), true, mod, expr->isConst);
}

void Resolver::visitFStringExpr(FStringExpr* expr) {
    for (auto& e : expr->exprs) resolve(e.get());
}

void Resolver::visitListCompExpr(ListCompExpr* expr) {
    beginScope();
    for (auto& c : expr->clauses) {
        resolve(c.iterable.get());
        resolvePattern(c.pattern.get(), true, ScopeModifier::Local, false);
        for (auto& cond : c.conditions) resolve(cond.get());
    }
    resolve(expr->valueExpr.get());
    endScope();
}

void Resolver::visitSetCompExpr(SetCompExpr* expr) {
    beginScope();
    for (auto& c : expr->clauses) {
        resolve(c.iterable.get());
        resolvePattern(c.pattern.get(), true, ScopeModifier::Local, false);
        for (auto& cond : c.conditions) resolve(cond.get());
    }
    resolve(expr->valueExpr.get());
    endScope();
}

void Resolver::visitDictCompExpr(DictCompExpr* expr) {
    beginScope();
    for (auto& c : expr->clauses) {
        resolve(c.iterable.get());
        resolvePattern(c.pattern.get(), true, ScopeModifier::Local, false);
        for (auto& cond : c.conditions) resolve(cond.get());
    }
    resolve(expr->keyExpr.get());
    resolve(expr->valueExpr.get());
    endScope();
}

void Resolver::visitDictLiteral(DictLiteral* expr) {
    for (auto& p : expr->entries) {
        resolve(p.first.get());
        resolve(p.second.get());
    }
}

void Resolver::visitSetLiteral(SetLiteral* expr) {
    for (auto& e : expr->elements) resolve(e.get());
}

void Resolver::visitSliceExpr(SliceExpr* expr) {
    if (expr->start) resolve(expr->start.get());
    if (expr->end) resolve(expr->end.get());
    if (expr->step) resolve(expr->step.get());
}

void Resolver::visitSequenceExpr(SequenceExpr* expr) {
    for (auto& e : expr->expressions) resolve(e.get());
}

void Resolver::visitMatchExpr(MatchExpr* expr) {
    resolve(expr->subject.get());
    for (auto& b : expr->branches) {
        if (b.patterns.size() > 1) {
            auto collectVars = [](Pattern* p, std::set<std::string>& out, auto& self) -> void {
                if (!p) return;
                if (auto* vp = dynamic_cast<VariablePattern*>(p)) {
                    if (vp->name.lexeme != "_" && !vp->name.lexeme.empty()) out.insert(vp->name.lexeme);
                } else if (auto* rp = dynamic_cast<RestPattern*>(p)) {
                    if (rp->name.lexeme != "_" && !rp->name.lexeme.empty()) out.insert(rp->name.lexeme);
                } else if (auto* lp = dynamic_cast<ListPattern*>(p)) {
                    for (auto& e : lp->elements) self(e.get(), out, self);
                    if (lp->rest) self(lp->rest.get(), out, self);
                } else if (auto* mp = dynamic_cast<MatrixPattern*>(p)) {
                    for (auto& row : mp->rows) for (auto& e : row) self(e.get(), out, self);
                    if (mp->restRow) self(mp->restRow.get(), out, self);
                } else if (auto* dp = dynamic_cast<DictPattern*>(p)) {
                    for (auto& e : dp->entries) self(e.second.get(), out, self);
                    if (dp->rest) self(dp->rest.get(), out, self);
                } else if (auto* defp = dynamic_cast<DefaultPattern*>(p)) {
                    self(defp->inner.get(), out, self);
                }
            };
            std::set<std::string> baseVars;
            bool first = true;
            for (auto& p : b.patterns) {
                std::set<std::string> vars;
                collectVars(p.get(), vars, collectVars);
                if (first) { baseVars = std::move(vars); first = false; }
                else if (vars != baseVars) {
                    throw std::runtime_error("Compile Error: In a comma-separated (or) pattern, every alternative must bind the same set of variables.");
                }
            }
        }
        beginScope();
        for (size_t i = 0; i < b.patterns.size(); ++i) {
            resolvePattern(b.patterns[i].get(), false, ScopeModifier::Local, false, i > 0);
        }
        if (b.guard) resolve(b.guard.get());
        resolve(b.body.get());
        endScope();
    }
}

void Resolver::visitGroupingExpr(GroupingExpr* expr) {
    resolve(expr->expression.get());
}

void Resolver::visitMacroDefExpr(MacroDefExpr* expr) {
    beginScope(true, false);
    for (auto& p : expr->params) {
        if (p.lexeme != "_" && scopes.back().lexicalDecls.count(p.lexeme)) {
            throw std::runtime_error("SyntaxError: Parameter '" + p.lexeme + "' has already been declared.");
        }
        declareVariable(p.lexeme, VarScope::Local, false, true);
    }
    resolve(expr->body.get());
    endScope();
}

void Resolver::visitMacroCallExpr(MacroCallExpr* expr) {
    for (auto& arg : expr->arguments) resolve(arg.get());
}

void Resolver::visitQuoteExpr(QuoteExpr* expr) {
    resolve(expr->body.get());
}

void Resolver::visitUnquoteExpr(UnquoteExpr* expr) {
    resolve(expr->expr.get());
}

void Resolver::visitExprAssign(ExprAssign* expr) {
    resolve(expr->target.get());
    resolve(expr->value.get());
}

void Resolver::visitDeferExpr(DeferExpr* expr) {
    beginScope(true, false);
    resolve(expr->body.get());
    endScope();
}

void Resolver::visitKeywordArgExpr(KeywordArgExpr* expr) {
    resolve(expr->value.get());
}

void Resolver::resolvePattern(Pattern* pat, bool isAssignment, ScopeModifier globalMod, bool globalConst, bool skipRedecl) {
    if (!pat) return;
    if (auto* vp = dynamic_cast<VariablePattern*>(pat)) {
        if (vp->typeHint) resolve(vp->typeHint.get());
        if (vp->name.lexeme != "_") {
            ScopeModifier mod = vp->modifier != ScopeModifier::None ? vp->modifier : globalMod;
            bool isConst = vp->isConst || globalConst;
            if ((mod != ScopeModifier::None || isConst) && !skipRedecl) {
                checkExplicitDecl(vp, vp->name.lexeme);
            }
            VarScope scope = VarScope::Local;
            if (mod == ScopeModifier::State) scope = VarScope::State;
            else if (mod == ScopeModifier::Ref) {
                ResolvedSym existing = resolveName(vp->name.lexeme);
                scope = existing.scope;
            }
            declareVariable(vp->name.lexeme, scope, isConst, mod == ScopeModifier::Local || mod == ScopeModifier::Ref);
            patternSymbols[pat] = resolveName(vp->name.lexeme);
        }
    } else if (auto* rp = dynamic_cast<RestPattern*>(pat)) {
        if (rp->typeHint) resolve(rp->typeHint.get());
        if (rp->name.lexeme != "_") {
            ScopeModifier mod = rp->modifier != ScopeModifier::None ? rp->modifier : globalMod;
            bool isConst = rp->isConst || globalConst;
            if ((mod != ScopeModifier::None || isConst) && !skipRedecl) {
                checkExplicitDecl(rp, rp->name.lexeme);
            }
            VarScope scope = VarScope::Local;
            if (mod == ScopeModifier::State) scope = VarScope::State;
            else if (mod == ScopeModifier::Ref) {
                ResolvedSym existing = resolveName(rp->name.lexeme);
                scope = existing.scope;
            }
            declareVariable(rp->name.lexeme, scope, isConst, mod == ScopeModifier::Local || mod == ScopeModifier::Ref);
            patternSymbols[pat] = resolveName(rp->name.lexeme);
        }
    } else if (auto* lp = dynamic_cast<ListPattern*>(pat)) {
        for (auto& e : lp->elements) resolvePattern(e.get(), isAssignment, globalMod, globalConst, skipRedecl);
        if (lp->rest) resolvePattern(lp->rest.get(), isAssignment, globalMod, globalConst, skipRedecl);
    } else if (auto* mp = dynamic_cast<MatrixPattern*>(pat)) {
        for (auto& row : mp->rows) {
            for (auto& e : row) resolvePattern(e.get(), isAssignment, globalMod, globalConst, skipRedecl);
        }
        if (mp->restRow) resolvePattern(mp->restRow.get(), isAssignment, globalMod, globalConst, skipRedecl);
    } else if (auto* dp = dynamic_cast<DictPattern*>(pat)) {
        for (auto& e : dp->entries) resolvePattern(e.second.get(), isAssignment, globalMod, globalConst, skipRedecl);
        if (dp->rest) resolvePattern(dp->rest.get(), isAssignment, globalMod, globalConst, skipRedecl);
    } else if (auto* defp = dynamic_cast<DefaultPattern*>(pat)) {
        resolvePattern(defp->inner.get(), isAssignment, globalMod, globalConst, skipRedecl);
        resolve(defp->defaultExpr.get());
    } else if (auto* ep = dynamic_cast<ExprPattern*>(pat)) {
        resolve(ep->expr.get());
    } else if (auto* dap = dynamic_cast<DynamicAssertPattern*>(pat)) {
        resolve(dap->expr.get());
    }
}

} // namespace jc
