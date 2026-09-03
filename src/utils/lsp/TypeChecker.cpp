#include "TypeChecker.h"
#include "../../vm/HelpRouter.h"
#include "../../utils/json/Json.h"

namespace jc {
namespace lsp {

    static int positionalArgCount(const std::vector<std::unique_ptr<Expr>>& args) {
        int n = 0;
        for (auto& a : args) {
            if (a && dynamic_cast<KeywordArgExpr*>(a.get())) continue;
            n++;
        }
        return n;
    }

    static Type sigToType(const TypeSig& sig) {
        std::vector<TypeElem> elems;
        for (const auto& t : sig.types) elems.push_back(t);
        if (elems.empty()) return Type::any();
        return Type(std::move(elems));
    }

    TypeChecker::TypeChecker(Document* doc, NameResolver& resolver, BuiltinIndex& index, TypeInferrer& inferrer)
        : doc(doc), resolver(resolver), index(index), inferrer(inferrer) {}

    void TypeChecker::check(Expr* root) {
        if (root) root->accept(*this);
        // shadowing 警告（NameResolver 收集，排除 PI/E/I/ANS）
        for (const auto& sw : resolver.shadowWarnings()) {
            addDiag("Warning: Shadowing built-in symbol '" + sw.name + "'.", sw.pos, sw.pos + (int)sw.name.size());
        }
    }

    void TypeChecker::addDiag(const std::string& msg, int startPos, int endPos, int severity) {
        Diagnostic d;
        d.message = msg;
        d.startPos = startPos;
        d.endPos = endPos;
        d.severity = severity;
        diagnostics.push_back(std::move(d));
    }

    std::string TypeChecker::didYouMean(const std::string& name) {
        HelpRouter::init();
        const Json& helpAst = HelpRouter::helpAst;
        std::string best;
        int bestDist = 3;
        auto consider = [&](const std::string& cand) {
            int d = HelpRouter::levenshtein(name, cand);
            if (d < bestDist) { bestDist = d; best = cand; }
        };
        if (helpAst.isObject() && helpAst.has("global_functions") && helpAst["global_functions"].isObject()) {
            for (const auto& [k, v] : helpAst["global_functions"].objVal) consider(k);
        }
        for (const auto& [k, v] : index.allGlobals()) consider(k);
        for (const auto& m : index.methodNames()) consider(m);
        return best;
    }

    void TypeChecker::checkCallArity(const NameRes& res, int argCount, const Token& callee) {
        if (!res.builtin) return;
        const BuiltinSymbol& sym = *res.builtin;
        if (sym.arity.empty()) return;
        if (sym.arity.count(argCount)) return;
        bool hasRest = !sym.restName.empty() || !sym.kwargsName.empty();
        if (hasRest && argCount >= *sym.arity.begin()) return;
        std::string expect = sym.signatureText.empty() ? sym.name : sym.signatureText;
        addDiag("Warning: '" + callee.lexeme + "' expects " + expect + ", got " + std::to_string(argCount) + " argument(s).",
                callee.position, callee.position + (int)callee.lexeme.size());
    }

    void TypeChecker::checkMemberArity(const BuiltinSymbol& sym, int argCount, const Token& member) {
        if (sym.arity.empty()) return;
        if (sym.arity.count(argCount)) return;
        bool hasRest = !sym.restName.empty() || !sym.kwargsName.empty();
        if (hasRest && argCount >= *sym.arity.begin()) return;
        std::string expect = sym.signatureText.empty() ? sym.name : sym.signatureText;
        addDiag("Warning: '" + member.lexeme + "' expects " + expect + ", got " + std::to_string(argCount) + " argument(s).",
                member.position, member.position + (int)member.lexeme.size());
    }

    // ---------- ExprVisitor ----------
    void TypeChecker::visitCall(Call* e) {
        for (auto& arg : e->arguments) if (arg) arg->accept(*this);
        const NameRes* res = resolver.resolveAt(e);
        if (!res) return;
        if (res->origin == NameRes::Undefined) {
            std::string msg = "Warning: Undefined function '" + e->callee.lexeme + "'.";
            std::string dym = didYouMean(e->callee.lexeme);
            if (!dym.empty()) msg += " Did you mean '" + dym + "'?";
            addDiag(msg, e->callee.position, e->callee.position + (int)e->callee.lexeme.size());
        } else if (res->origin == NameRes::Builtin && res->builtin && res->builtin->kind == BuiltinKind::Function) {
            checkCallArity(*res, positionalArgCount(e->arguments), e->callee);
            // 参数类型检查（内置函数）
            const auto& pt = res->builtin->paramTypes;
            for (size_t i = 0; i < pt.size() && i < e->arguments.size(); ++i) {
                if (!e->arguments[i] || dynamic_cast<KeywordArgExpr*>(e->arguments[i].get())) continue;
                Type expected = sigToType(pt[i]);
                Type actual = inferrer.typeOf(e->arguments[i].get());
                if (!expected.isAny() && !actual.isAny() && !actual.isNever() && !Type::compatible(actual, expected)) {
                    addDiag("Warning: Argument " + std::to_string(i + 1) + " expects '" + expected.toString() + "', got '" + actual.toString() + "'.",
                            e->arguments[i]->startPos, e->arguments[i]->endPos);
                }
            }
        } else if (res->origin == NameRes::User && res->user && res->user->kind == UserSymbol::Function) {
            // 参数类型检查（用户函数）
            const auto& pt = res->user->paramTypes;
            for (size_t i = 0; i < pt.size() && i < e->arguments.size(); ++i) {
                if (!e->arguments[i]) continue;
                Type expected = pt[i];
                Type actual = inferrer.typeOf(e->arguments[i].get());
                if (!expected.isAny() && !actual.isAny() && !actual.isNever() && !Type::compatible(actual, expected)) {
                    addDiag("Warning: Argument " + std::to_string(i + 1) + " expects '" + expected.toString() + "', got '" + actual.toString() + "'.",
                            e->arguments[i]->startPos, e->arguments[i]->endPos);
                }
            }
        }
    }

    void TypeChecker::visitInvokeExpr(InvokeExpr* e) {
        if (e->callee) e->callee->accept(*this);
        for (auto& arg : e->arguments) if (arg) arg->accept(*this);
        // callee 是 DotAccess（如 sys.gc）时，检查成员参数数量
        if (auto* dot = dynamic_cast<DotAccess*>(e->callee.get())) {
            const NameRes* res = resolver.resolveAt(dot);
            if (res && res->member && res->member->kind == BuiltinKind::ModuleMember) {
                checkMemberArity(*res->member, positionalArgCount(e->arguments), dot->field);
            }
        }
    }

    void TypeChecker::visitMethodCallExpr(MethodCallExpr* e) {
        if (e->object) e->object->accept(*this);
        for (auto& arg : e->arguments) if (arg) arg->accept(*this);
        const NameRes* res = resolver.resolveAt(e);
        // self.xxx 等已解析为用户符号（类方法）的方法调用，不再报 Unknown method。
        if (res && res->origin == NameRes::User) return;
        if (res && res->origin == NameRes::Imported && res->member) {
            checkMemberArity(*res->member, positionalArgCount(e->arguments), e->method);
            return;
        }
        // UFCS：x.f(y) 等价 f(x, y)，method 是全局函数时检查参数类型（接收者为第 1 参）
        const BuiltinSymbol* gf = index.findGlobal(e->method.lexeme);
        if (gf && (gf->kind == BuiltinKind::Function || gf->kind == BuiltinKind::Type || gf->kind == BuiltinKind::Class)) {
            std::vector<Type> argTypes;
            if (e->object) argTypes.push_back(inferrer.typeOf(e->object.get()));
            for (auto& a : e->arguments) if (a) argTypes.push_back(inferrer.typeOf(a.get()));
            for (size_t i = 0; i < gf->paramTypes.size() && i < argTypes.size(); ++i) {
                Type expected = sigToType(gf->paramTypes[i]);
                Type actual = argTypes[i];
                if (!expected.isAny() && !actual.isAny() && !actual.isNever() && !Type::compatible(actual, expected)) {
                    addDiag("Warning: UFCS argument " + std::to_string(i + 1) + " expects '" + expected.toString() + "', got '" + actual.toString() + "'.",
                            e->method.position, e->method.position + (int)e->method.lexeme.size());
                }
            }
            return;
        }
        if (!index.isMethodName(e->method.lexeme) && !resolver.isImportedMethod(e->method.lexeme)) {
            // 只对 self.xxx 报 Unknown method（self 的类型已知，方法在类里定义，typo 可报）。
            // 其它对象（参数/属性访问/调用结果）类型动态，静态无法判断方法是否存在，不报。
            if (e->object && dynamic_cast<SelfExpr*>(e->object.get())) {
                std::string msg = "Warning: Unknown method '" + e->method.lexeme + "'.";
                std::string dym = didYouMean(e->method.lexeme);
                if (!dym.empty()) msg += " Did you mean '" + dym + "'?";
                addDiag(msg, e->method.position, e->method.position + (int)e->method.lexeme.size());
            }
        }
    }

    void TypeChecker::visitDotAccess(DotAccess* e) {
        if (e->object) e->object->accept(*this);
        const NameRes* res = resolver.resolveAt(e);
        if (!res) return;
        // object 是模块，但 field 没解析到成员 → 未知模块成员
        std::string objName;
        if (auto* v = dynamic_cast<Variable*>(e->object.get())) objName = v->name.lexeme;
        if (!objName.empty() && res->origin == NameRes::Undefined) {
            bool isModule = index.findModuleMember(objName, "__none__") != nullptr ||
                            resolver.resolveAt(e->object.get()) != nullptr &&
                            (resolver.resolveAt(e->object.get())->origin == NameRes::Imported);
            // 简化：如果 object 名字是内置模块或已 import 的模块，且 field 未解析，报未知成员
            (void)isModule;
        }
    }

    void TypeChecker::visitVariable(Variable* e) {
        (void)e;
        // 第一版不报未定义变量（避免 REPL 全局/动态误报）
    }

    // ---------- 遍历 ----------
    void TypeChecker::visitBinary(Binary* e) {
        if (e->left) e->left->accept(*this);
        if (e->right) e->right->accept(*this);
    }
    void TypeChecker::visitUnary(Unary* e) {
        if (e->right) e->right->accept(*this);
    }
    void TypeChecker::visitAssign(Assign* e) {
        if (e->value) e->value->accept(*this);
        // 类型注解检查：x: T = expr，检查 expr 类型 compatible T
        if (e->typeHint) {
            Type expected = inferrer.inferTypeObject(e->typeHint.get());
            Type actual = inferrer.typeOf(e->value.get());
            if (!expected.isAny() && !actual.isNever() && !actual.isAny()) {
                if (!Type::compatible(actual, expected)) {
                    addDiag("Warning: Type mismatch. Expected '" + expected.toString() + "', got '" + actual.toString() + "'.",
                            e->value->startPos, e->value->endPos);
                }
            }
        }
    }
    void TypeChecker::visitMatrixNode(MatrixNode* e) {
        for (auto& row : e->elements) for (auto& c : row) if (c) c->accept(*this);
    }
    void TypeChecker::visitListNode(ListNode* e) {
        for (auto& row : e->elements) for (auto& c : row) if (c) c->accept(*this);
    }
    void TypeChecker::visitBlock(Block* e) {
        for (auto& s : e->statements) if (s) s->accept(*this);
    }
    void TypeChecker::visitIfExpr(IfExpr* e) {
        if (e->condition) e->condition->accept(*this);
        if (e->thenBranch) e->thenBranch->accept(*this);
        if (e->elseBranch) e->elseBranch->accept(*this);
    }
    void TypeChecker::visitWhileExpr(WhileExpr* e) {
        if (e->condition) e->condition->accept(*this);
        if (e->body) e->body->accept(*this);
    }
    void TypeChecker::visitForExpr(ForExpr* e) {
        if (e->initializer) e->initializer->accept(*this);
        if (e->condition) e->condition->accept(*this);
        if (e->update) e->update->accept(*this);
        if (e->body) e->body->accept(*this);
    }
    void TypeChecker::visitReturnExpr(ReturnExpr* e) {
        if (e->value) e->value->accept(*this);
    }
    void TypeChecker::visitIndexAccess(IndexAccess* e) {
        if (e->object) e->object->accept(*this);
        for (auto& idx : e->indices) if (idx) idx->accept(*this);
    }
    void TypeChecker::visitIndexAssign(IndexAssign* e) {
        if (e->objectExpr) e->objectExpr->accept(*this);
        for (auto& chain : e->indexChain) for (auto& idx : chain) if (idx) idx->accept(*this);
        if (e->value) e->value->accept(*this);
    }
    void TypeChecker::visitLocalDecl(LocalDecl* e) {
        (void)e;
    }
    void TypeChecker::visitRefDecl(RefDecl* e) {
        (void)e;
    }
    void TypeChecker::visitStateDecl(StateDecl* e) {
        (void)e;
    }
    void TypeChecker::visitConstDecl(ConstDecl* e) {
        (void)e;
    }
    void TypeChecker::visitCompoundAssign(CompoundAssign* e) {
        if (e->target) e->target->accept(*this);
        if (e->value) e->value->accept(*this);
    }
    void TypeChecker::visitLambdaExpr(LambdaExpr* e) {
        if (e->body) e->body->accept(*this);
    }
    void TypeChecker::visitForInExpr(ForInExpr* e) {
        if (e->iterable) e->iterable->accept(*this);
        if (e->body) e->body->accept(*this);
    }
    void TypeChecker::visitThrowExpr(ThrowExpr* e) {
        if (e->value) e->value->accept(*this);
    }
    void TypeChecker::visitTryCatchExpr(TryCatchExpr* e) {
        if (e->tryBody) e->tryBody->accept(*this);
        if (e->catchBody) e->catchBody->accept(*this);
    }
    void TypeChecker::visitSwitchExpr(SwitchExpr* e) {
        if (e->subject) e->subject->accept(*this);
        for (auto& c : e->cases) {
            for (auto& v : c.first) if (v) v->accept(*this);
            if (c.second) c.second->accept(*this);
        }
        if (e->defaultBody) e->defaultBody->accept(*this);
    }
    void TypeChecker::visitClassDefExpr(ClassDefExpr* e) {
        for (auto& p : e->staticProperties) if (p.value) p.value->accept(*this);
        for (auto& p : e->instanceProperties) if (p.value) p.value->accept(*this);
    }
    void TypeChecker::visitNamespaceDecl(NamespaceDecl* e) {
        if (e->body) e->body->accept(*this);
    }
    void TypeChecker::visitEnumDefExpr(EnumDefExpr* e) {
        for (auto& m : e->members) if (m.second) m.second->accept(*this);
    }
    void TypeChecker::visitDotAssign(DotAssign* e) {
        if (e->object) e->object->accept(*this);
        if (e->value) e->value->accept(*this);
    }
    void TypeChecker::visitDestructAssign(DestructAssign* e) {
        if (e->value) e->value->accept(*this);
    }
    void TypeChecker::visitFStringExpr(FStringExpr* e) {
        for (auto& x : e->exprs) if (x) x->accept(*this);
    }
    void TypeChecker::visitMatrixCompExpr(MatrixCompExpr* e) {
        for (auto& c : e->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (e->valueExpr) e->valueExpr->accept(*this);
    }
    void TypeChecker::visitListCompExpr(ListCompExpr* e) {
        for (auto& c : e->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (e->valueExpr) e->valueExpr->accept(*this);
    }
    void TypeChecker::visitSetCompExpr(SetCompExpr* e) {
        for (auto& c : e->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (e->valueExpr) e->valueExpr->accept(*this);
    }
    void TypeChecker::visitDictCompExpr(DictCompExpr* e) {
        for (auto& c : e->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (e->keyExpr) e->keyExpr->accept(*this);
        if (e->valueExpr) e->valueExpr->accept(*this);
    }
    void TypeChecker::visitDictLiteral(DictLiteral* e) {
        for (auto& p : e->entries) {
            if (p.first) p.first->accept(*this);
            if (p.second) p.second->accept(*this);
        }
    }
    void TypeChecker::visitSetLiteral(SetLiteral* e) {
        for (auto& x : e->elements) if (x) x->accept(*this);
    }
    void TypeChecker::visitSliceExpr(SliceExpr* e) {
        if (e->start) e->start->accept(*this);
        if (e->end) e->end->accept(*this);
        if (e->step) e->step->accept(*this);
    }
    void TypeChecker::visitSequenceExpr(SequenceExpr* e) {
        for (auto& x : e->expressions) if (x) x->accept(*this);
    }
    void TypeChecker::visitMatchExpr(MatchExpr* e) {
        if (e->subject) e->subject->accept(*this);
        for (auto& b : e->branches) {
            if (b.guard) b.guard->accept(*this);
            if (b.body) b.body->accept(*this);
        }
    }
    void TypeChecker::visitGroupingExpr(GroupingExpr* e) {
        if (e->expression) e->expression->accept(*this);
    }
    void TypeChecker::visitMacroDefExpr(MacroDefExpr* e) {
        if (e->body) e->body->accept(*this);
    }
    void TypeChecker::visitMacroCallExpr(MacroCallExpr* e) {
        for (auto& arg : e->arguments) if (arg) arg->accept(*this);
        const NameRes* res = resolver.resolveAt(e);
        if (res && res->origin == NameRes::Undefined) {
            std::string msg = "Warning: Undefined macro '" + e->macroName.lexeme + "'.";
            std::string dym = didYouMean(e->macroName.lexeme);
            if (!dym.empty()) msg += " Did you mean '" + dym + "'?";
            addDiag(msg, e->macroName.position, e->macroName.position + (int)e->macroName.lexeme.size());
        }
    }
    void TypeChecker::visitQuoteExpr(QuoteExpr* e) {
        if (e->body) e->body->accept(*this);
    }
    void TypeChecker::visitUnquoteExpr(UnquoteExpr* e) {
        if (e->expr) e->expr->accept(*this);
    }
    void TypeChecker::visitExprAssign(ExprAssign* e) {
        if (e->target) e->target->accept(*this);
        if (e->value) e->value->accept(*this);
    }
    void TypeChecker::visitDeferExpr(DeferExpr* e) {
        if (e->body) e->body->accept(*this);
    }
    void TypeChecker::visitKeywordArgExpr(KeywordArgExpr* e) {
        if (e->value) e->value->accept(*this);
    }
    void TypeChecker::visitSpreadExpr(SpreadExpr* e) {
        if (e->value) e->value->accept(*this);
    }
    void TypeChecker::visitTypeAssertExpr(TypeAssertExpr* e) {
        if (e->value) e->value->accept(*this);
        if (e->typeHint) {
            Type expected = inferrer.inferTypeObject(e->typeHint.get());
            Type actual = inferrer.typeOf(e->value.get());
            if (!expected.isAny() && !actual.isNever() && !actual.isAny()) {
                if (!Type::compatible(actual, expected)) {
                    addDiag("Warning: Type assertion may fail. Inferred '" + actual.toString() + "' does not match '" + expected.toString() + "'.",
                            e->value->startPos, e->value->endPos);
                }
            }
        }
    }

} // namespace lsp
} // namespace jc
