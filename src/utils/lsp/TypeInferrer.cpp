#include "TypeInferrer.h"

namespace jc {
namespace lsp {

    // 类型名 → Type（对应运行时 builtinValues 的类型对象）
    static Type builtinTypeObject(const std::string& name) {
        if (name == "int") return Type(BuiltinType::INT);
        if (name == "double") return Type(BuiltinType::FLOAT);
        if (name == "string") return Type(BuiltinType::STRING);
        if (name == "bool") return Type(BuiltinType::BOOL);
        if (name == "none_type") return Type(BuiltinType::NONE_TYPE);
        if (name == "list") return Type(BuiltinType::LIST);
        if (name == "dict") return Type(BuiltinType::DICT);
        if (name == "set") return Type(BuiltinType::SET);
        if (name == "fraction") return Type(BuiltinType::FRACTION);
        if (name == "complex") return Type(BuiltinType::COMPLEX);
        if (name == "symbolic") return Type(BuiltinType::SYMBOLIC);
        if (name == "matrix") return Type(std::vector<TypeElem>{BuiltinType::REALMAT, BuiltinType::COMPLEXMAT, BuiltinType::SYMMAT});
        if (name == "function") return Type(BuiltinType::FUNC);
        if (name == "class_type") return Type(BuiltinType::CLASS);
        if (name == "instance") return Type(BuiltinType::INSTANCE);
        if (name == "namespace_type") return Type(BuiltinType::NAMESPACE);
        if (name == "type") return Type(BuiltinType::TYPE_DEF);
        if (name == "slice") return Type(BuiltinType::SLICE);
        if (name == "any") return Type::any();
        return Type::any();
    }

    // TypeSig → Type（含 sameAsParam 依赖参数）
    static Type sigToType(const TypeSig& sig, const std::vector<Type>& argTypes) {
        if (sig.sameAsParam >= 0 && sig.sameAsParam < static_cast<int>(argTypes.size())) {
            return argTypes[sig.sameAsParam];
        }
        std::vector<TypeElem> elems;
        for (const auto& t : sig.types) elems.push_back(t);
        if (elems.empty()) return Type::any();
        return Type(std::move(elems));
    }

    TypeInferrer::TypeInferrer(Document* doc, NameResolver& resolver, BuiltinIndex& index)
        : doc(doc), resolver(resolver), index(index) {}

    void TypeInferrer::infer(Expr* root) {
        if (!root) return;
        collectDecls(root);   // 收集声明目标
        runFixpoint();        // 迭代推导变量类型到不动点
        inferExpr(root);      // 递归推导所有节点类型（缓存）
    }

    Type TypeInferrer::typeOf(Expr* node) const {
        auto it = exprTypes.find(node);
        return it != exprTypes.end() ? it->second : Type::any();
    }

    // ---------- 收集声明 ----------
    void TypeInferrer::collectDecls(Expr* root) {
        root->accept(*this);
    }

    void TypeInferrer::runFixpoint() {
        // 1. 类型别名轮（迭代到不动点）
        bool changed = true;
        int guard = 0;
        while (changed && guard++ < 100) {
            changed = false;
            for (const auto& [node, sym] : declTargets) {
                if (!sym) continue;
                if (auto* assign = dynamic_cast<Assign*>(node)) {
                    Type rhs = inferExpr(assign->value.get());
                    bool isTypeObject = false;
                    for (const auto& t : rhs.types) {
                        if (std::holds_alternative<BuiltinType>(t) &&
                            (std::get<BuiltinType>(t) == BuiltinType::TYPE_DEF || std::get<BuiltinType>(t) == BuiltinType::CLASS)) {
                            isTypeObject = true;
                            break;
                        }
                    }
                    if (isTypeObject) {
                        Type t = inferTypeObject(assign->value.get());
                        auto& cur = typeAliases[sym->name];
                        if (cur.types != t.types) { cur = t; changed = true; }
                    }
                }
            }
        }
        // 2. 函数签名推导（参数类型 + 返回类型）
        for (const auto& [node, sym] : declTargets) {
            if (!sym || sym->kind != UserSymbol::Function) continue;
            if (auto* assign = dynamic_cast<Assign*>(node)) {
                if (auto* lam = dynamic_cast<LambdaExpr*>(assign->value.get())) {
                    sym->paramTypes.clear();
                    for (auto& pt : lam->paramTypes) {
                        sym->paramTypes.push_back(pt ? inferTypeObject(pt.get()) : Type::any());
                    }
                    if (lam->returnType) {
                        sym->returnType = inferTypeObject(lam->returnType.get());
                        sym->hasReturnType = true;
                    } else {
                        sym->returnType = lam->body ? inferExpr(lam->body.get()) : Type::any();
                        sym->hasReturnType = false;
                    }
                }
            }
        }
        // 3. 变量类型轮（迭代到不动点）
        changed = true;
        guard = 0;
        while (changed && guard++ < 200) {
            changed = false;
            exprTypes.clear();  // 每轮清空缓存：变量类型在变化，缓存会导致 fixpoint 失效
            for (const auto& [node, sym] : declTargets) {
                if (!sym) continue;
                Type t = inferExpr(node);
                Type unified = Type::unify(sym->inferredType, t);
                if (unified.types != sym->inferredType.types) {
                    sym->inferredType = unified;
                    changed = true;
                }
            }
        }
    }

    Type TypeInferrer::returnTypeOf(const BuiltinSymbol& sym, const std::vector<Type>& argTypes) {
        return sigToType(sym.returnType, argTypes);
    }

    // ---------- 值推导 ----------
    Type TypeInferrer::literalType(const Literal* lit) {
        if (lit->isString) return Type(BuiltinType::STRING);
        if (lit->isImaginary) return Type(BuiltinType::COMPLEX);
        if (lit->isKeyword) {
            if (lit->value == "true" || lit->value == "false") return Type(BuiltinType::BOOL);
            if (lit->value == "none") return Type(BuiltinType::NONE_TYPE);
        }
        const std::string& v = lit->value;
        if (v.find('.') != std::string::npos || v.find('e') != std::string::npos || v.find('E') != std::string::npos) {
            return Type(BuiltinType::FLOAT);
        }
        return Type(BuiltinType::INT);
    }

    Type TypeInferrer::inferExpr(Expr* e) {
        if (!e) return Type::any();
        auto it = exprTypes.find(e);
        if (it != exprTypes.end()) return it->second;

        Type result;
        if (auto* lit = dynamic_cast<Literal*>(e)) {
            result = literalType(lit);
        } else if (auto* v = dynamic_cast<Variable*>(e)) {
            const NameRes* nr = resolver.resolveAt(e);
            if (nr && nr->origin == NameRes::User && nr->user) {
                result = nr->user->inferredType;
            } else if (nr && nr->origin == NameRes::Builtin && nr->builtin) {
                if (nr->builtin->kind == BuiltinKind::Type) result = Type(BuiltinType::TYPE_DEF);
                else if (nr->builtin->kind == BuiltinKind::Function) result = Type(BuiltinType::FUNC);
                else result = Type::any();
            } else {
                result = Type::any();
            }
        } else if (auto* assign = dynamic_cast<Assign*>(e)) {
            result = assign->value ? inferExpr(assign->value.get()) : Type::any();
        } else if (auto* ld = dynamic_cast<LocalDecl*>(e)) {
            result = ld->typeHint ? inferTypeObject(ld->typeHint.get()) : Type::any();
        } else if (auto* rd = dynamic_cast<RefDecl*>(e)) {
            result = rd->typeHint ? inferTypeObject(rd->typeHint.get()) : Type::any();
        } else if (auto* sd = dynamic_cast<StateDecl*>(e)) {
            result = sd->typeHint ? inferTypeObject(sd->typeHint.get()) : Type::any();
        } else if (dynamic_cast<ConstDecl*>(e)) {
            result = Type::any();
        } else if (dynamic_cast<MatrixNode*>(e)) {
            result = Type(BuiltinType::REALMAT);
        } else if (dynamic_cast<ListNode*>(e) || dynamic_cast<ListCompExpr*>(e)) {
            result = Type(BuiltinType::LIST);
        } else if (dynamic_cast<DictLiteral*>(e) || dynamic_cast<DictCompExpr*>(e)) {
            result = Type(BuiltinType::DICT);
        } else if (dynamic_cast<SetLiteral*>(e) || dynamic_cast<SetCompExpr*>(e)) {
            result = Type(BuiltinType::SET);
        } else if (dynamic_cast<LambdaExpr*>(e)) {
            result = Type(BuiltinType::FUNC);
        } else if (dynamic_cast<ClassDefExpr*>(e)) {
            result = Type(BuiltinType::CLASS);
        } else if (dynamic_cast<NamespaceDecl*>(e) || dynamic_cast<ImportExpr*>(e)) {
            result = Type(BuiltinType::NAMESPACE);
        } else if (auto* b = dynamic_cast<Binary*>(e)) {
            Type lt = inferExpr(b->left.get());
            Type rt = inferExpr(b->right.get());
            std::string op = b->op.lexeme;
            if (op == "|" || op == "&") {
                result = Type(BuiltinType::TYPE_DEF);  // 类型对象运算
            } else if (op == "/" && lt.types.size() == 1 && rt.types.size() == 1 &&
                       std::holds_alternative<BuiltinType>(lt.types[0]) && std::get<BuiltinType>(lt.types[0]) == BuiltinType::INT &&
                       std::holds_alternative<BuiltinType>(rt.types[0]) && std::get<BuiltinType>(rt.types[0]) == BuiltinType::INT) {
                result = Type(BuiltinType::FRACTION);  // int / int → fraction
            } else {
                result = Type::unify(lt, rt);
                if (result.isAny()) result = Type::any();
                // 数字提升：若单边是 FLOAT 则 FLOAT
                bool hasFloat = false;
                for (const auto& t : lt.types) if (std::holds_alternative<BuiltinType>(t) && std::get<BuiltinType>(t) == BuiltinType::FLOAT) hasFloat = true;
                for (const auto& t : rt.types) if (std::holds_alternative<BuiltinType>(t) && std::get<BuiltinType>(t) == BuiltinType::FLOAT) hasFloat = true;
                if (hasFloat) result = Type(BuiltinType::FLOAT);
            }
        } else if (auto* u = dynamic_cast<Unary*>(e)) {
            if (u->op.lexeme == "!") result = Type(BuiltinType::BOOL);
            else result = inferExpr(u->right.get());
        } else if (auto* c = dynamic_cast<Call*>(e)) {
            const NameRes* nr = resolver.resolveAt(e);
            if (nr && nr->origin == NameRes::Builtin && nr->builtin) {
                std::vector<Type> argTypes;
                for (auto& a : c->arguments) argTypes.push_back(inferExpr(a.get()));
                result = returnTypeOf(*nr->builtin, argTypes);
            } else if (nr && nr->origin == NameRes::User && nr->user && nr->user->kind == UserSymbol::Function) {
                for (auto& a : c->arguments) if (a) inferExpr(a.get());  // 推导实参（供参数类型检查）
                result = nr->user->returnType;  // 用户函数返回类型（含覆盖内置）
            } else {
                result = Type::any();
            }
        } else if (auto* g = dynamic_cast<GroupingExpr*>(e)) {
            result = inferExpr(g->expression.get());
        } else if (auto* ife = dynamic_cast<IfExpr*>(e)) {
            result = Type::unify(inferExpr(ife->thenBranch.get()), inferExpr(ife->elseBranch.get()));
        } else if (auto* seq = dynamic_cast<SequenceExpr*>(e)) {
            result = seq->expressions.empty() ? Type::any() : inferExpr(seq->expressions.back().get());
        } else if (auto* blk = dynamic_cast<Block*>(e)) {
            Type r = Type::any();
            for (auto& s : blk->statements) if (s) r = inferExpr(s.get());
            result = r;
        } else {
            result = Type::any();
        }

        exprTypes[e] = result;
        return result;
    }

    // ---------- 类型对象推导 ----------
    Type TypeInferrer::inferTypeObject(Expr* e) {
        if (!e) return Type::any();
        if (auto* v = dynamic_cast<Variable*>(e)) {
            std::string name = v->name.lexeme;
            auto it = typeAliases.find(name);
            if (it != typeAliases.end()) return it->second;  // 类型别名
            if (index.isTypeName(name)) return builtinTypeObject(name);  // 内置类型名
            const NameRes* nr = resolver.resolveAt(e);
            if (nr && nr->origin == NameRes::User && nr->user && nr->user->kind == UserSymbol::Class) {
                return Type(name);  // 用户类
            }
            return Type::any();
        }
        if (auto* b = dynamic_cast<Binary*>(e)) {
            if (b->op.lexeme == "|") return Type::unify(inferTypeObject(b->left.get()), inferTypeObject(b->right.get()));
            if (b->op.lexeme == "&") return Type::intersect(inferTypeObject(b->left.get()), inferTypeObject(b->right.get()));
        }
        if (auto* g = dynamic_cast<GroupingExpr*>(e)) return inferTypeObject(g->expression.get());
        return Type::any();
    }

    // ---------- ExprVisitor（收集声明 + 遍历） ----------
    void TypeInferrer::visitAssign(Assign* e) {
        if (e->value) e->value->accept(*this);
        Position pos = doc->offsetToPosition(e->name.position);
        const NameRes* nr = resolver.resolveAtPos(pos);
        declTargets[e] = (nr && nr->origin == NameRes::User) ? nr->user : nullptr;
    }

    void TypeInferrer::visitLocalDecl(LocalDecl* e) {
        Position pos = doc->offsetToPosition(e->name.position);
        const NameRes* nr = resolver.resolveAtPos(pos);
        declTargets[e] = (nr && nr->origin == NameRes::User) ? nr->user : nullptr;
    }
    void TypeInferrer::visitRefDecl(RefDecl* e) {
        Position pos = doc->offsetToPosition(e->name.position);
        const NameRes* nr = resolver.resolveAtPos(pos);
        declTargets[e] = (nr && nr->origin == NameRes::User) ? nr->user : nullptr;
    }
    void TypeInferrer::visitStateDecl(StateDecl* e) {
        Position pos = doc->offsetToPosition(e->name.position);
        const NameRes* nr = resolver.resolveAtPos(pos);
        declTargets[e] = (nr && nr->origin == NameRes::User) ? nr->user : nullptr;
    }
    void TypeInferrer::visitConstDecl(ConstDecl* e) {
        Position pos = doc->offsetToPosition(e->name.position);
        const NameRes* nr = resolver.resolveAtPos(pos);
        declTargets[e] = (nr && nr->origin == NameRes::User) ? nr->user : nullptr;
    }

    void TypeInferrer::visitBlock(Block* e) {
        for (auto& s : e->statements) if (s) s->accept(*this);
    }
    void TypeInferrer::visitLambdaExpr(LambdaExpr* e) {
        if (e->body) e->body->accept(*this);
    }
    void TypeInferrer::visitClassDefExpr(ClassDefExpr* e) {
        for (auto& p : e->staticProperties) if (p.value) p.value->accept(*this);
        for (auto& p : e->instanceProperties) if (p.value) p.value->accept(*this);
    }
    void TypeInferrer::visitNamespaceDecl(NamespaceDecl* e) {
        if (e->body) e->body->accept(*this);
    }
    void TypeInferrer::visitBinary(Binary* e) {
        if (e->left) e->left->accept(*this);
        if (e->right) e->right->accept(*this);
    }
    void TypeInferrer::visitUnary(Unary* e) {
        if (e->right) e->right->accept(*this);
    }
    void TypeInferrer::visitVariable(Variable* e) {
        (void)e;
    }
    void TypeInferrer::visitCall(Call* e) {
        for (auto& a : e->arguments) if (a) a->accept(*this);
    }
    void TypeInferrer::visitMatrixNode(MatrixNode* e) {
        for (auto& row : e->elements) for (auto& c : row) if (c) c->accept(*this);
    }
    void TypeInferrer::visitListNode(ListNode* e) {
        for (auto& row : e->elements) for (auto& c : row) if (c) c->accept(*this);
    }
    void TypeInferrer::visitIfExpr(IfExpr* e) {
        if (e->condition) e->condition->accept(*this);
        if (e->thenBranch) e->thenBranch->accept(*this);
        if (e->elseBranch) e->elseBranch->accept(*this);
    }
    void TypeInferrer::visitWhileExpr(WhileExpr* e) {
        if (e->condition) e->condition->accept(*this);
        if (e->body) e->body->accept(*this);
    }
    void TypeInferrer::visitForExpr(ForExpr* e) {
        if (e->initializer) e->initializer->accept(*this);
        if (e->condition) e->condition->accept(*this);
        if (e->update) e->update->accept(*this);
        if (e->body) e->body->accept(*this);
    }
    void TypeInferrer::visitReturnExpr(ReturnExpr* e) {
        if (e->value) e->value->accept(*this);
    }
    void TypeInferrer::visitIndexAccess(IndexAccess* e) {
        if (e->object) e->object->accept(*this);
        for (auto& idx : e->indices) if (idx) idx->accept(*this);
    }
    void TypeInferrer::visitIndexAssign(IndexAssign* e) {
        if (e->objectExpr) e->objectExpr->accept(*this);
        for (auto& chain : e->indexChain) for (auto& idx : chain) if (idx) idx->accept(*this);
        if (e->value) e->value->accept(*this);
    }
    void TypeInferrer::visitCompoundAssign(CompoundAssign* e) {
        if (e->target) e->target->accept(*this);
        if (e->value) e->value->accept(*this);
    }
    void TypeInferrer::visitInvokeExpr(InvokeExpr* e) {
        if (e->callee) e->callee->accept(*this);
        for (auto& a : e->arguments) if (a) a->accept(*this);
    }
    void TypeInferrer::visitForInExpr(ForInExpr* e) {
        if (e->iterable) e->iterable->accept(*this);
        if (e->body) e->body->accept(*this);
    }
    void TypeInferrer::visitThrowExpr(ThrowExpr* e) {
        if (e->value) e->value->accept(*this);
    }
    void TypeInferrer::visitTryCatchExpr(TryCatchExpr* e) {
        if (e->tryBody) e->tryBody->accept(*this);
        if (e->catchBody) e->catchBody->accept(*this);
    }
    void TypeInferrer::visitImportExpr(ImportExpr* e) {
        (void)e;
    }
    void TypeInferrer::visitSwitchExpr(SwitchExpr* e) {
        if (e->subject) e->subject->accept(*this);
        for (auto& c : e->cases) {
            for (auto& v : c.first) if (v) v->accept(*this);
            if (c.second) c.second->accept(*this);
        }
        if (e->defaultBody) e->defaultBody->accept(*this);
    }
    void TypeInferrer::visitEnumDefExpr(EnumDefExpr* e) {
        for (auto& m : e->members) if (m.second) m.second->accept(*this);
    }
    void TypeInferrer::visitDotAccess(DotAccess* e) {
        if (e->object) e->object->accept(*this);
    }
    void TypeInferrer::visitDotAssign(DotAssign* e) {
        if (e->object) e->object->accept(*this);
        if (e->value) e->value->accept(*this);
    }
    void TypeInferrer::visitMethodCallExpr(MethodCallExpr* e) {
        if (e->object) e->object->accept(*this);
        for (auto& a : e->arguments) if (a) a->accept(*this);
    }
    void TypeInferrer::visitDestructAssign(DestructAssign* e) {
        if (e->value) e->value->accept(*this);
    }
    void TypeInferrer::visitFStringExpr(FStringExpr* e) {
        for (auto& x : e->exprs) if (x) x->accept(*this);
    }
    void TypeInferrer::visitMatrixCompExpr(MatrixCompExpr* e) {
        for (auto& c : e->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (e->valueExpr) e->valueExpr->accept(*this);
    }
    void TypeInferrer::visitListCompExpr(ListCompExpr* e) {
        for (auto& c : e->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (e->valueExpr) e->valueExpr->accept(*this);
    }
    void TypeInferrer::visitSetCompExpr(SetCompExpr* e) {
        for (auto& c : e->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (e->valueExpr) e->valueExpr->accept(*this);
    }
    void TypeInferrer::visitDictCompExpr(DictCompExpr* e) {
        for (auto& c : e->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (e->keyExpr) e->keyExpr->accept(*this);
        if (e->valueExpr) e->valueExpr->accept(*this);
    }
    void TypeInferrer::visitDictLiteral(DictLiteral* e) {
        for (auto& p : e->entries) {
            if (p.first) p.first->accept(*this);
            if (p.second) p.second->accept(*this);
        }
    }
    void TypeInferrer::visitSetLiteral(SetLiteral* e) {
        for (auto& x : e->elements) if (x) x->accept(*this);
    }
    void TypeInferrer::visitSliceExpr(SliceExpr* e) {
        if (e->start) e->start->accept(*this);
        if (e->end) e->end->accept(*this);
        if (e->step) e->step->accept(*this);
    }
    void TypeInferrer::visitSequenceExpr(SequenceExpr* e) {
        for (auto& x : e->expressions) if (x) x->accept(*this);
    }
    void TypeInferrer::visitMatchExpr(MatchExpr* e) {
        if (e->subject) e->subject->accept(*this);
        for (auto& b : e->branches) {
            if (b.guard) b.guard->accept(*this);
            if (b.body) b.body->accept(*this);
        }
    }
    void TypeInferrer::visitGroupingExpr(GroupingExpr* e) {
        if (e->expression) e->expression->accept(*this);
    }
    void TypeInferrer::visitMacroDefExpr(MacroDefExpr* e) {
        if (e->body) e->body->accept(*this);
    }
    void TypeInferrer::visitExprAssign(ExprAssign* e) {
        if (e->target) e->target->accept(*this);
        if (e->value) e->value->accept(*this);
    }
    void TypeInferrer::visitDeferExpr(DeferExpr* e) {
        if (e->body) e->body->accept(*this);
    }
    void TypeInferrer::visitKeywordArgExpr(KeywordArgExpr* e) {
        if (e->value) e->value->accept(*this);
    }
    void TypeInferrer::visitSpreadExpr(SpreadExpr* e) {
        if (e->value) e->value->accept(*this);
    }
    void TypeInferrer::visitTypeAssertExpr(TypeAssertExpr* e) {
        if (e->value) e->value->accept(*this);
        if (e->typeHint) e->typeHint->accept(*this);
    }

} // namespace lsp
} // namespace jc
