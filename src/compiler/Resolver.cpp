#include "Resolver.h"
#include "../memory/Exceptions.h"
#include <set>
#include <algorithm>

namespace jc {

// ---------------------------------------------------------------------------
// Phase 1 definite-assignment analysis for function-level auto-locals.
//
// This is intentionally separate from name resolution: it answers only
// "can this auto-local be unbound on some path?" and rejects the definite
// case before IR generation. Reads that are merely *possibly* unbound are
// left for the runtime check phase.
// ---------------------------------------------------------------------------
namespace {

struct DAState {
    std::unordered_set<std::string> must;
    std::unordered_set<std::string> may;
};

static void daMerge(const DAState& a, const DAState& b, DAState& out) {
    DAState result;
    for (const auto& name : a.must) {
        if (b.must.count(name)) result.must.insert(name);
    }
    result.may = a.may;
    result.may.insert(b.may.begin(), b.may.end());
    out = std::move(result);
}

class AutoLocalDefiniteAssignment {
public:
    AutoLocalDefiniteAssignment(const std::unordered_set<std::string>& autoLocals,
                                std::unordered_map<Expr*, ResolvedSym>* symbols,
                                const std::vector<Token>& params,
                                const std::vector<Token>& kwargParams,
                                const std::string& restName,
                                const std::string& kwargsName)
        : autoLocals_(autoLocals), symbols_(symbols) {
        auto markParam = [&](const std::string& name) {
            if (!name.empty() && name != "_") {
                state_.must.insert(name);
                state_.may.insert(name);
            }
        };
        for (const auto& p : params) markParam(p.lexeme);
        for (const auto& p : kwargParams) markParam(p.lexeme);
        markParam(restName);
        markParam(kwargsName);
    }

    void analyze(Expr* body) { walk(body); }

private:
    const std::unordered_set<std::string>& autoLocals_;
    std::unordered_map<Expr*, ResolvedSym>* symbols_;
    DAState state_;

    void checkRead(Variable* var) {
        const std::string& name = var->name.lexeme;
        if (!autoLocals_.count(name)) return;
        if (!state_.may.count(name)) {
            JC2_THROW(SyntaxError, "[Line " + std::to_string(var->name.line) +
                                    "] Variable '" + name + "' is used before its assignment.");
        }
        if (!state_.must.count(name) && symbols_) {
            auto it = symbols_->find(var);
            if (it != symbols_->end()) it->second.needsInitCheck = true;
        }
    }

    void markBound(const std::string& name) {
        if (!autoLocals_.count(name)) return;
        state_.must.insert(name);
        state_.may.insert(name);
    }

    void markPattern(Pattern* pat) {
        if (!pat) return;
        if (auto* dp = dynamic_cast<DefaultPattern*>(pat)) {
            markPattern(dp->inner.get());
            return;
        }
        if (auto* vp = dynamic_cast<VariablePattern*>(pat)) {
            if (vp->name.lexeme != "_") markBound(vp->name.lexeme);
            return;
        }
        if (auto* rp = dynamic_cast<RestPattern*>(pat)) {
            if (rp->name.lexeme != "_") markBound(rp->name.lexeme);
            return;
        }
        if (auto* lp = dynamic_cast<ListPattern*>(pat)) {
            for (auto& e : lp->elements) markPattern(e.get());
            markPattern(lp->rest.get());
            return;
        }
        if (auto* mp = dynamic_cast<MatrixPattern*>(pat)) {
            for (auto& row : mp->rows) for (auto& e : row) markPattern(e.get());
            markPattern(mp->restRow.get());
            return;
        }
        if (auto* dp = dynamic_cast<DictPattern*>(pat)) {
            for (auto& e : dp->entries) markPattern(e.second.get());
            markPattern(dp->rest.get());
        }
    }

    void walk(Expr* expr) {
        if (!expr) return;

        if (auto* b = dynamic_cast<Block*>(expr)) {
            for (auto& stmt : b->statements) walk(stmt.get());
            return;
        }
        if (auto* seq = dynamic_cast<SequenceExpr*>(expr)) {
            for (auto& e : seq->expressions) walk(e.get());
            return;
        }
        if (auto* a = dynamic_cast<Assign*>(expr)) {
            walk(a->value.get());
            if (!a->isState && !a->isRef) markBound(a->name.lexeme);
            return;
        }
        if (auto* ea = dynamic_cast<ExprAssign*>(expr)) {
            walk(ea->value.get());
            if (auto* v = dynamic_cast<Variable*>(ea->target.get())) {
                if (!ea->isState && !ea->isRef) markBound(v->name.lexeme);
            } else if (auto* da = dynamic_cast<DestructAssign*>(ea->target.get())) {
                markPattern(da->pattern.get());
            }
            return;
        }
        if (auto* del = dynamic_cast<DeleteExpr*>(expr)) {
            for (auto& tok : del->names) {
                if (autoLocals_.count(tok.lexeme)) {
                    state_.must.erase(tok.lexeme);
                    state_.may.erase(tok.lexeme);
                }
            }
            return;
        }
        if (auto* c = dynamic_cast<CompoundAssign*>(expr)) {
            if (auto* v = dynamic_cast<Variable*>(c->target.get())) checkRead(v);
            else walk(c->target.get());
            walk(c->value.get());
            if (auto* v = dynamic_cast<Variable*>(c->target.get())) markBound(v->name.lexeme);
            return;
        }
        if (auto* da = dynamic_cast<DestructAssign*>(expr)) {
            walk(da->value.get());
            markPattern(da->pattern.get());
            return;
        }
        if (auto* ld = dynamic_cast<LocalDecl*>(expr)) {
            markBound(ld->name.lexeme);
            return;
        }
        if (auto* cd = dynamic_cast<ConstDecl*>(expr)) {
            markBound(cd->name.lexeme);
            return;
        }
        if (auto* v = dynamic_cast<Variable*>(expr)) {
            checkRead(v);
            return;
        }
        if (auto* g = dynamic_cast<GroupingExpr*>(expr)) {
            walk(g->expression.get());
            return;
        }
        if (auto* u = dynamic_cast<Unary*>(expr)) {
            walk(u->right.get());
            return;
        }
        if (auto* b = dynamic_cast<Binary*>(expr)) {
            walk(b->left.get());
            walk(b->right.get());
            return;
        }
        if (auto* call = dynamic_cast<Call*>(expr)) {
            for (auto& arg : call->arguments) walk(arg.get());
            return;
        }
        if (auto* inv = dynamic_cast<InvokeExpr*>(expr)) {
            walk(inv->callee.get());
            for (auto& arg : inv->arguments) walk(arg.get());
            return;
        }
        if (auto* mc = dynamic_cast<MethodCallExpr*>(expr)) {
            walk(mc->object.get());
            for (auto& arg : mc->arguments) walk(arg.get());
            return;
        }
        if (auto* ia = dynamic_cast<IndexAccess*>(expr)) {
            walk(ia->object.get());
            for (auto& i : ia->indices) walk(i.get());
            return;
        }
        if (auto* ias = dynamic_cast<IndexAssign*>(expr)) {
            if (ias->objectExpr) walk(ias->objectExpr.get());
            for (auto& chain : ias->indexChain) for (auto& i : chain) walk(i.get());
            walk(ias->value.get());
            return;
        }
        if (auto* da = dynamic_cast<DotAccess*>(expr)) {
            walk(da->object.get());
            return;
        }
        if (auto* das = dynamic_cast<DotAssign*>(expr)) {
            walk(das->object.get());
            walk(das->value.get());
            return;
        }
        if (auto* ife = dynamic_cast<IfExpr*>(expr)) {
            walk(ife->condition.get());
            DAState before = state_;
            DAState thenState = state_;
            state_ = thenState;
            walk(ife->thenBranch.get());
            thenState = state_;
            DAState elseState = before;
            if (ife->elseBranch) {
                state_ = elseState;
                walk(ife->elseBranch.get());
                elseState = state_;
            }
            daMerge(thenState, elseState, state_);
            return;
        }
        if (auto* wh = dynamic_cast<WhileExpr*>(expr)) {
            walk(wh->condition.get());
            DAState before = state_;
            state_ = before;
            walk(wh->body.get());
            DAState bodyState = state_;
            daMerge(before, bodyState, state_);
            return;
        }
        if (auto* fr = dynamic_cast<ForExpr*>(expr)) {
            walk(fr->initializer.get());
            DAState afterInit = state_;
            walk(fr->condition.get());
            state_ = afterInit;
            walk(fr->body.get());
            DAState bodyState = state_;
            daMerge(afterInit, bodyState, state_);
            walk(fr->update.get());
            return;
        }
        if (auto* fi = dynamic_cast<ForInExpr*>(expr)) {
            walk(fi->iterable.get());
            DAState before = state_;
            markPattern(fi->pattern.get());
            walk(fi->body.get());
            DAState bodyState = state_;
            daMerge(before, bodyState, state_);
            return;
        }
        if (auto* m = dynamic_cast<MatchExpr*>(expr)) {
            walk(m->subject.get());
            DAState before = state_;
            bool first = true;
            DAState merged;
            for (auto& branch : m->branches) {
                state_ = before;
                for (auto& pattern : branch.patterns) markPattern(pattern.get());
                if (branch.guard) walk(branch.guard.get());
                walk(branch.body.get());
                if (first) { merged = state_; first = false; }
                else daMerge(merged, state_, merged);
            }
            state_ = first ? before : merged;
            return;
        }
        if (auto* tc = dynamic_cast<TryCatchExpr*>(expr)) {
            DAState before = state_;
            walk(tc->tryBody.get());
            DAState merged = state_;
            for (auto& branch : tc->catchBranches) {
                state_ = before;
                for (auto& pattern : branch.patterns) markPattern(pattern.get());
                if (branch.guard) walk(branch.guard.get());
                walk(branch.body.get());
                daMerge(merged, state_, merged);
            }
            state_ = merged;
            return;
        }
        if (auto* ret = dynamic_cast<ReturnExpr*>(expr)) { walk(ret->value.get()); return; }
        if (auto* th = dynamic_cast<ThrowExpr*>(expr)) { walk(th->value.get()); return; }
        if (auto* dl = dynamic_cast<DictLiteral*>(expr)) {
            for (auto& [k, v] : dl->entries) { walk(k.get()); walk(v.get()); }
            return;
        }
        if (auto* sl = dynamic_cast<SetLiteral*>(expr)) {
            for (auto& e : sl->elements) walk(e.get());
            return;
        }
        if (auto* ln = dynamic_cast<ListNode*>(expr)) {
            for (auto& row : ln->elements) for (auto& e : row) walk(e.get());
            return;
        }
        if (auto* mn = dynamic_cast<MatrixNode*>(expr)) {
            for (auto& row : mn->elements) for (auto& e : row) walk(e.get());
            return;
        }
        if (auto* fs = dynamic_cast<FStringExpr*>(expr)) {
            for (auto& e : fs->exprs) walk(e.get());
            return;
        }
        if (auto* slice = dynamic_cast<SliceExpr*>(expr)) {
            walk(slice->start.get()); walk(slice->end.get()); walk(slice->step.get());
            return;
        }
        // Lambda/Class bodies are separate call-time scopes and are handled
        // when their own IRBuilder compiles them.
    }
};

} // namespace


void Resolver::checkExplicitDecl(void* node, const std::string& name) {
    if (name == "_") return;
    if (scopes.empty()) return;
    if (checkedDecls.count(node)) return;
    checkedDecls.insert(node);
    
    if (scopes.back().lexicalDecls.count(name)) {
        JC2_THROW(SyntaxError, "Variable '" + name + "' has already been declared in this scope.");
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

void Resolver::declareVariable(const std::string& name, VarScope scopeType, bool isConst, bool isExplicitLocal, bool isAutoLocal) {
    if (name == "_") return;
    if (scopes.empty()) return;
    
    scopes.back().lexicalDecls.insert(name);
    
    ResolvedSym sym;
    sym.scope = scopeType;
    sym.isConst = isConst;
    sym.isAutoLocal = isAutoLocal;
    
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
    if (declSerial >= 0) {               // ★ 记录声明位置（由 hoistBlock 逐语句设置）
        sym.declBlock = declSerial;
        sym.declStmt = declStmt;
    }
    scopes.back().symbols[name] = sym;
}

// ★ 诊断辅助：若符号声明于当前语句之后（同一块内），标记 declaredLater
static void markDeclaredLater(ResolvedSym& sym, const std::vector<std::pair<int, int>>& frames) {
    if (sym.declBlock < 0) return;
    // Only diagnose when the read is in the declaration's own block. A read
    // inside a nested branch may happen after an earlier assignment in that
    // branch; comparing it against a later statement of the parent block would
    // produce a false "used before declaration" error.
    if (frames.empty() || frames.back().first != sym.declBlock) return;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].first == sym.declBlock) {
            if (sym.declStmt > frames[i].second) sym.declaredLater = true;
            return;
        }
    }
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
                // A nested function/method resolves outer names at call time.
                // A later module-level declaration is not a source-order error.
            } else {
                markDeclaredLater(sym, blockFrames);
            }
            return sym;
        }
    }
    ResolvedSym globalSym;
    globalSym.scope = VarScope::Global;
    globalSym.isConst = knownConstGlobals && knownConstGlobals->count(name);
    return globalSym;
}

void Resolver::resolve(Expr* expr) {
    if (expr) expr->accept(*this);
}

void Resolver::visitBlock(Block* expr) {
    beginScope();
    // ★ 诊断：先压本块帧（含块序号），hoistBlock 才能把声明记到本块与正确语句序号上
    int serial = nextBlockSerial++;
    blockFrames.push_back({serial, 0});
    hoistBlock(expr);
    for (size_t k = 0; k < expr->statements.size(); ++k) {
        blockFrames.back().second = static_cast<int>(k);
        resolve(expr->statements[k].get());
    }
    blockFrames.pop_back();
    endScope();
}

void Resolver::hoistBlock(Block* block) {
    // 预扫描块内的变量声明，实现 Auto-Local 和显式声明的提升
    int serial = blockFrames.empty() ? -1 : blockFrames.back().first;
    int stmtIndex = 0;
    for (auto& stmt : block->statements) {
        declSerial = serial;
        declStmt = stmtIndex++;

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
            else {
                bool isDefinition = dynamic_cast<LambdaExpr*>(assign->value.get()) != nullptr ||
                                    dynamic_cast<ClassDefExpr*>(assign->value.get()) != nullptr;
                declareVariable(assign->name.lexeme, VarScope::Local, assign->isConst, false, !isDefinition);
            }
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
    declSerial = -1;
    declStmt = -1;
}

void Resolver::visitVariable(Variable* expr) {
    if (expr->name.lexeme == "_") {
        JC2_THROW(SyntaxError, "'_' is a placeholder and cannot be read.");
    }
    ResolvedSym sym = resolveName(expr->name.lexeme);
    if (sym.scope == VarScope::Upvalue && sym.isAutoLocal) sym.needsInitCheck = true;
    if (sym.scope == VarScope::Local && sym.deletedLocal) sym.needsInitCheck = true;
    exprSymbols[expr] = sym;
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
    else {
        bool isDefinition = dynamic_cast<LambdaExpr*>(expr->value.get()) != nullptr ||
                            dynamic_cast<ClassDefExpr*>(expr->value.get()) != nullptr;
        declareVariable(expr->name.lexeme, VarScope::Local, expr->isConst, false, !isDefinition);
    }
    if (!expr->isState && !expr->isRef) {
        for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
            auto it = scopes[i].symbols.find(expr->name.lexeme);
            if (it != scopes[i].symbols.end()) { it->second.deletedLocal = false; break; }
        }
    }
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
    ResolvedSym sym = resolveName(expr->callee.lexeme);
    if (sym.scope == VarScope::Upvalue && sym.isAutoLocal) sym.needsInitCheck = true;
    if (sym.scope == VarScope::Local && sym.deletedLocal) sym.needsInitCheck = true;
    exprSymbols[expr] = sym;
    for (auto& arg : expr->arguments) resolve(arg.get());
}

void Resolver::visitMatrixNode(MatrixNode* expr) {
    for (auto& row : expr->elements) {
        for (auto& e : row) resolve(e.get());
    }
}

void Resolver::visitListNode(ListNode* expr) {
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

void Resolver::visitBreakExpr(BreakExpr* expr) {
    if (expr->value) resolve(expr->value.get());
}
void Resolver::visitContinueExpr(ContinueExpr* /*expr*/) {}

void Resolver::visitReturnExpr(ReturnExpr* expr) {
    if (expr->value) resolve(expr->value.get());
}

void Resolver::visitYieldExpr(YieldExpr* expr) {
    if (expr->value) resolve(expr->value.get());
}

void Resolver::visitSpawnExpr(SpawnExpr* expr) {
    beginScope(true, false);
    resolve(expr->body.get());
    endScope();
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

void Resolver::visitDeleteExpr(DeleteExpr* expr) {
    std::vector<ResolvedSym> syms;
    syms.reserve(expr->names.size());
    for (auto& tok : expr->names) {
        ResolvedSym sym = resolveName(tok.lexeme);
        syms.push_back(sym);
        if (sym.scope == VarScope::Upvalue || sym.scope == VarScope::CapturedState) {
            JC2_THROW(SyntaxError, "Cannot delete captured variable '" + tok.lexeme + "'.");
        }
        if (sym.scope == VarScope::State) {
            JC2_THROW(SyntaxError, "Cannot delete state variable '" + tok.lexeme + "'.");
        }
        if (sym.isConst && sym.scope != VarScope::Global) {
            JC2_THROW(SyntaxError, "Cannot delete const variable '" + tok.lexeme + "'.");
        }
        // Python-style delete: a local binding stays in scope but becomes
        // unbound. It must not fall back to an outer binding of the same name.
        if (sym.scope == VarScope::Local || sym.scope == VarScope::RefParam) {
            for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
                auto it = scopes[i].symbols.find(tok.lexeme);
                if (it != scopes[i].symbols.end()) {
                    it->second.deletedLocal = true;
                    syms.back().deletedLocal = true;
                    break;
                }
            }
        }
    }
    deleteSyms[expr] = std::move(syms);
}

void Resolver::visitCompoundAssign(CompoundAssign* expr) {
    resolve(expr->target.get());
    resolve(expr->value.get());
    if (!expr->isState && !expr->isRef) {
        if (auto* v = dynamic_cast<Variable*>(expr->target.get())) {
            for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
                auto it = scopes[i].symbols.find(v->name.lexeme);
                if (it != scopes[i].symbols.end()) { it->second.deletedLocal = false; break; }
            }
        }
    }
}

void Resolver::visitLambdaExpr(LambdaExpr* expr) {
    for (auto& pt : expr->paramTypes) {
        if (pt) resolve(pt.get());
    }
    for (auto& pt : expr->kwargTypes) {
        if (pt) resolve(pt.get());
    }
    if (expr->returnType) resolve(expr->returnType.get());

    beginScope(true, false);
    for (size_t i = 0; i < expr->params.size(); ++i) {
        if (expr->params[i].lexeme != "_" && scopes.back().lexicalDecls.count(expr->params[i].lexeme)) {
            JC2_THROW(SyntaxError, "Parameter '" + expr->params[i].lexeme + "' has already been declared.");
        }
        
        VarScope scope = expr->paramIsRef[i] ? VarScope::RefParam : VarScope::Local;
        declareVariable(expr->params[i].lexeme, scope, expr->paramIsConst[i], true);
        if (expr->defaultExprs[i]) resolve(expr->defaultExprs[i].get());
    }
    // ★ rest 参数
    if (!expr->restName.empty() && expr->restName != "_") {
        // ★ 必须与位置参数段同口径做重复检查。此前只 declareVariable 不校验，
        //   于是 f(x, ...x) 静默通过且 rest 覆盖位置参数：f(1,2,3) 返回 @[2,3]
        //   而不是 1 —— 不报错的错误语义。
        if (scopes.back().lexicalDecls.count(expr->restName)) {
            JC2_THROW(SyntaxError, "Parameter '" + expr->restName + "' has already been declared.");
        }
        declareVariable(expr->restName, VarScope::Local, false, true);
    }
    // ★ 仅关键字参数
    for (size_t j = 0; j < expr->kwargParams.size(); ++j) {
        if (expr->kwargParams[j].lexeme != "_") {
            // ★ 同上：仅关键字段此前也没有重复检查，跨分区同名（f(x; x)）被放行。
            if (scopes.back().lexicalDecls.count(expr->kwargParams[j].lexeme)) {
                JC2_THROW(SyntaxError, "Parameter '" + expr->kwargParams[j].lexeme + "' has already been declared.");
            }
            VarScope scope = (j < expr->kwargIsRef.size() && expr->kwargIsRef[j]) ? VarScope::RefParam : VarScope::Local;
            declareVariable(expr->kwargParams[j].lexeme, scope, j < expr->kwargIsConst.size() && expr->kwargIsConst[j], true);
            if (j < expr->kwargDefaultExprs.size() && expr->kwargDefaultExprs[j]) resolve(expr->kwargDefaultExprs[j].get());
        }
    }
    // ★ kwargs 参数
    if (!expr->kwargsName.empty() && expr->kwargsName != "_") {
        // ★ 同上：kwargs 名此前也无重复检查。f(x; ...x) 会让 kwargs 字典在函数体内
        //   遮蔽同名形参（f(1) 返回 {} 而不是 1），同样是静默的错误语义。
        if (scopes.back().lexicalDecls.count(expr->kwargsName)) {
            JC2_THROW(SyntaxError, "Parameter '" + expr->kwargsName + "' has already been declared.");
        }
        declareVariable(expr->kwargsName, VarScope::Local, false, true);
    }

    // Mark the next Block as this lambda's direct body. Its direct statements
    resolve(expr->body.get());

    std::unordered_set<std::string> autoLocals;
    for (const auto& [name, sym] : scopes.back().symbols) {
        if (sym.scope == VarScope::Local && sym.isAutoLocal) {
            autoLocals.insert(name);
        }
    }
    AutoLocalDefiniteAssignment analyzer(autoLocals, &exprSymbols, expr->params, expr->kwargParams,
                                          expr->restName, expr->kwargsName);
    analyzer.analyze(expr->body.get());

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
    resolveMatchBranches(expr->catchBranches);
}

void Resolver::visitIncludeExpr(IncludeExpr* expr) {
    if (expr->value) expr->value->accept(*this);
}

void Resolver::visitImportExpr(ImportExpr* expr) {
    resolve(expr->path.get());
}

void Resolver::visitClassDefExpr(ClassDefExpr* expr) {
    if (expr->superClassExpr) resolve(expr->superClassExpr.get());
    for (auto& t : expr->traitExprs) resolve(t.get());
    
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

void Resolver::visitMatrixCompExpr(MatrixCompExpr* expr) {
    beginScope();
    for (auto& c : expr->clauses) {
        resolve(c.iterable.get());
        resolvePattern(c.pattern.get(), true, ScopeModifier::Local, false);
        for (auto& cond : c.conditions) resolve(cond.get());
    }
    resolve(expr->valueExpr.get());
    endScope();
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
    resolveMatchBranches(expr->branches);
}

void Resolver::resolveMatchBranches(std::vector<MatchBranch>& branches) {
    for (auto& b : branches) {
        auto checkSelfRedecl = [](Pattern* p, std::set<std::string>& seen, auto& self) -> void {
            if (!p) return;
            if (auto* vp = dynamic_cast<VariablePattern*>(p)) {
                if (vp->name.lexeme != "_" && !vp->name.lexeme.empty()) {
                    if (!seen.insert(vp->name.lexeme).second) {
                        JC2_THROW(SyntaxError, "Variable '" + vp->name.lexeme + "' has already been declared in this pattern.");
                    }
                }
            } else if (auto* rp = dynamic_cast<RestPattern*>(p)) {
                if (rp->name.lexeme != "_" && !rp->name.lexeme.empty()) {
                    if (!seen.insert(rp->name.lexeme).second) {
                        JC2_THROW(SyntaxError, "Variable '" + rp->name.lexeme + "' has already been declared in this pattern.");
                    }
                }
            } else if (auto* lp = dynamic_cast<ListPattern*>(p)) {
                for (auto& e : lp->elements) self(e.get(), seen, self);
                if (lp->rest) self(lp->rest.get(), seen, self);
            } else if (auto* mp = dynamic_cast<MatrixPattern*>(p)) {
                for (auto& row : mp->rows) for (auto& e : row) self(e.get(), seen, self);
                if (mp->restRow) self(mp->restRow.get(), seen, self);
            } else if (auto* dp = dynamic_cast<DictPattern*>(p)) {
                for (auto& e : dp->entries) self(e.second.get(), seen, self);
                if (dp->rest) self(dp->rest.get(), seen, self);
            } else if (auto* defp = dynamic_cast<DefaultPattern*>(p)) {
                self(defp->inner.get(), seen, self);
            }
        };
        for (auto& p : b.patterns) {
            std::set<std::string> seen;
            checkSelfRedecl(p.get(), seen, checkSelfRedecl);
        }
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
                    JC2_THROW(ParserError, "In a comma-separated (or) pattern, every alternative must bind the same set of variables.");
                }
            }
        }
        beginScope();
        // Predeclare every branch target as an uninitialized local. A default
        // expression that references a later target must see that local slot,
        // not silently fall back to an outer variable of the same name.
        auto collectTargetNames = [](Pattern* p, std::set<std::string>& out, auto& self) -> void {
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
        std::set<std::string> branchTargets;
        if (!b.patterns.empty()) {
            collectTargetNames(b.patterns[0].get(), branchTargets, collectTargetNames);
        }
        for (const auto& name : branchTargets) {
            declareVariable(name, VarScope::Local, false, true);
        }
        for (size_t i = 0; i < b.patterns.size(); ++i) {
            resolvePattern(b.patterns[i].get(), false, ScopeModifier::Local, false, true);
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
            JC2_THROW(SyntaxError, "Parameter '" + p.lexeme + "' has already been declared.");
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

void Resolver::visitSpreadExpr(SpreadExpr* expr) {
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
                isConst = isConst || existing.isConst;
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
                isConst = isConst || existing.isConst;
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

