#include "ASTConverter.h"
#include "../vm/VM.h"
#include <stdexcept>

namespace jc {

class ASTToJC2Visitor : public ExprVisitor {
public:
    Value result;

    Value makeASTNode(const std::string& type, int line, const std::vector<std::pair<std::string, Value>>& props) {
        auto clsVal = VM::activeVM->getBuiltinValue("ASTNode");
        if (!clsVal.isClass()) throw std::runtime_error("ASTNode class not found");
        
        ObjInstance* inst = GcHeap::get().allocate<ObjInstance>();
        GcObjGuard instGuard(inst);
        inst->classDef = static_cast<ObjClass*>(clsVal.asObj());
        inst->fields = GcHeap::get().allocate<ObjDict>();
        
        inst->fields->set(Value("type"), Value(type));
        inst->fields->set(Value("line"), Value(line));
        
        for (const auto& p : props) {
            inst->fields->set(Value(p.first), p.second);
        }
        return Value(inst);
    }

    void visitBinary(Binary* expr) override {
        expr->left->accept(*this); Value l = result;
        GcValueGuard lGuard(l);
        expr->right->accept(*this); Value r = result;
        result = makeASTNode("Binary", expr->op.line, {
            {"op", Value(expr->op.lexeme)},
            {"left", l},
            {"right", r}
        });
    }

    void visitUnary(Unary* expr) override {
        expr->right->accept(*this); Value r = result;
        result = makeASTNode("Unary", expr->op.line, {
            {"op", Value(expr->op.lexeme)},
            {"right", r}
        });
    }

    void visitLiteral(Literal* expr) override {
        result = makeASTNode("Literal", 0, {
            {"value", Value(expr->value)},
            {"isString", Value(expr->isString)},
            {"isImaginary", Value(expr->isImaginary)},
            {"isKeyword", Value(expr->isKeyword)}
        });
    }

    void visitVariable(Variable* expr) override {
        result = makeASTNode("Variable", expr->name.line, {
            {"name", Value(expr->name.lexeme)}
        });
    }

    void visitAssign(Assign* expr) override {
        expr->value->accept(*this); Value v = result;
        result = makeASTNode("Assign", expr->name.line, {
            {"name", Value(expr->name.lexeme)},
            {"value", v},
            {"isRef", Value(expr->isRef)},
            {"isState", Value(expr->isState)},
            {"isLocal", Value(expr->isLocal)},
            {"isConst", Value(expr->isConst)}
        });
    }

    template<typename T>
    Value makeExprListT(const std::vector<T>& exprs) {
        ObjList* list = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(list);
        for (const auto& e : exprs) {
            if (e) {
                e->accept(*this);
                list->vec.push_back(result);
            } else {
                list->vec.push_back(Value::none());
            }
        }
        return Value(list);
    }

    Value patternToJC2(Pattern* pat) {
        if (!pat) return Value::none();
        if (auto* lp = dynamic_cast<LiteralPattern*>(pat)) {
            lp->literal->accept(*this);
            return makeASTNode("LiteralPattern", 0, {{"literal", result}});
        }
        if (auto* ep = dynamic_cast<ExprPattern*>(pat)) {
            ep->expr->accept(*this);
            return makeASTNode("ExprPattern", 0, {{"expr", result}});
        }
        if (auto* dp = dynamic_cast<DynamicAssertPattern*>(pat)) {
            dp->expr->accept(*this);
            return makeASTNode("DynamicAssertPattern", 0, {{"expr", result}});
        }
        if (auto* vp = dynamic_cast<VariablePattern*>(pat)) {
            return makeASTNode("VariablePattern", vp->name.line, {
                {"name", Value(vp->name.lexeme)},
                {"modifier", Value(static_cast<double>(vp->modifier))},
                {"isConst", Value(vp->isConst)}
            });
        }
        if (auto* rp = dynamic_cast<RestPattern*>(pat)) {
            return makeASTNode("RestPattern", rp->name.line, {
                {"name", Value(rp->name.lexeme)},
                {"modifier", Value(static_cast<double>(rp->modifier))},
                {"isConst", Value(rp->isConst)}
            });
        }
        if (auto* listp = dynamic_cast<ListPattern*>(pat)) {
            ObjList* elems = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(elems);
            for (auto& e : listp->elements) elems->vec.push_back(patternToJC2(e.get()));
            return makeASTNode("ListPattern", 0, {
                {"elements", Value(elems)},
                {"rest", patternToJC2(listp->rest.get())}
            });
        }
        if (auto* matp = dynamic_cast<MatrixPattern*>(pat)) {
            ObjList* rows = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(rows);
            for (auto& row : matp->rows) {
                ObjList* r = GcHeap::get().allocate<ObjList>();
                GcObjGuard rGuard(r);
                for (auto& e : row) r->vec.push_back(patternToJC2(e.get()));
                rows->vec.push_back(Value(r));
            }
            return makeASTNode("MatrixPattern", 0, {
                {"rows", Value(rows)},
                {"restRow", patternToJC2(matp->restRow.get())}
            });
        }
        if (auto* dictp = dynamic_cast<DictPattern*>(pat)) {
            ObjList* entries = GcHeap::get().allocate<ObjList>();
            GcObjGuard guard(entries);
            for (auto& e : dictp->entries) {
                ObjList* kv = GcHeap::get().allocate<ObjList>();
                GcObjGuard kvGuard(kv);
                kv->vec.push_back(Value(e.first));
                kv->vec.push_back(patternToJC2(e.second.get()));
                entries->vec.push_back(Value(kv));
            }
            return makeASTNode("DictPattern", 0, {
                {"entries", Value(entries)},
                {"rest", patternToJC2(dictp->rest.get())}
            });
        }
        if (auto* defp = dynamic_cast<DefaultPattern*>(pat)) {
            defp->defaultExpr->accept(*this); Value defVal = result;
            GcValueGuard defGuard(defVal);
            return makeASTNode("DefaultPattern", 0, {
                {"inner", patternToJC2(defp->inner.get())},
                {"defaultExpr", defVal}
            });
        }
        throw std::runtime_error("Macro Error: Unsupported pattern type");
    }

    void visitBlock(Block* expr) override {
        result = makeASTNode("Block", 0, {{"statements", makeExprListT(expr->statements)}});
    }

    void visitCall(Call* expr) override {
        result = makeASTNode("Call", expr->callee.line, {
            {"callee", Value(expr->callee.lexeme)},
            {"arguments", makeExprListT(expr->arguments)}
        });
    }
    void visitMatrixNode(MatrixNode* expr) override {
        ObjList* rows = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(rows);
        for (auto& row : expr->elements) rows->vec.push_back(makeExprListT(row));
        result = makeASTNode("MatrixNode", 0, {{"elements", Value(rows)}, {"forceList", Value(expr->forceList)}});
    }
    void visitIfExpr(IfExpr* expr) override {
        expr->condition->accept(*this); Value cond = result;
        GcValueGuard condGuard(cond);
        expr->thenBranch->accept(*this); Value thenB = result;
        GcValueGuard thenBGuard(thenB);
        Value elseB = Value::none();
        if (expr->elseBranch) { expr->elseBranch->accept(*this); elseB = result; }
        result = makeASTNode("IfExpr", 0, {{"condition", cond}, {"thenBranch", thenB}, {"elseBranch", elseB}});
    }
    void visitWhileExpr(WhileExpr* expr) override {
        expr->condition->accept(*this); Value cond = result;
        GcValueGuard condGuard(cond);
        expr->body->accept(*this); Value body = result;
        result = makeASTNode("WhileExpr", 0, {{"condition", cond}, {"body", body}});
    }
    void visitForExpr(ForExpr* expr) override {
        expr->initializer->accept(*this); Value init = result;
        GcValueGuard initGuard(init);
        expr->condition->accept(*this); Value cond = result;
        GcValueGuard condGuard(cond);
        expr->update->accept(*this); Value upd = result;
        GcValueGuard updGuard(upd);
        expr->body->accept(*this); Value body = result;
        result = makeASTNode("ForExpr", 0, {{"initializer", init}, {"condition", cond}, {"update", upd}, {"body", body}});
    }
    void visitBreakExpr(BreakExpr* expr) override { result = makeASTNode("BreakExpr", expr->keyword.line, {}); }
    void visitContinueExpr(ContinueExpr* expr) override { result = makeASTNode("ContinueExpr", expr->keyword.line, {}); }
    void visitReturnExpr(ReturnExpr* expr) override {
        Value val = Value::none();
        if (expr->value) { expr->value->accept(*this); val = result; }
        result = makeASTNode("ReturnExpr", expr->keyword.line, {{"value", val}});
    }
    void visitIndexAccess(IndexAccess* expr) override {
        expr->object->accept(*this); Value obj = result;
        result = makeASTNode("IndexAccess", 0, {{"object", obj}, {"indices", makeExprListT(expr->indices)}});
    }
    void visitIndexAssign(IndexAssign* expr) override {
        Value objExpr = Value::none();
        if (expr->objectExpr) { expr->objectExpr->accept(*this); objExpr = result; }
        GcValueGuard objGuard(objExpr);
        ObjList* chain = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(chain);
        for (auto& level : expr->indexChain) chain->vec.push_back(makeExprListT(level));
        expr->value->accept(*this); Value valExpr = result;
        result = makeASTNode("IndexAssign", expr->name.line, {
            {"name", Value(expr->name.lexeme)}, {"objectExpr", objExpr}, {"indexChain", Value(chain)}, {"value", valExpr}
        });
    }
    void visitLocalDecl(LocalDecl* expr) override { result = makeASTNode("LocalDecl", expr->name.line, {{"name", Value(expr->name.lexeme)}, {"isConst", Value(expr->isConst)}}); }
    void visitRefDecl(RefDecl* expr) override { result = makeASTNode("RefDecl", expr->name.line, {{"name", Value(expr->name.lexeme)}, {"isConst", Value(expr->isConst)}}); }
    void visitStateDecl(StateDecl* expr) override { result = makeASTNode("StateDecl", expr->name.line, {{"name", Value(expr->name.lexeme)}, {"isConst", Value(expr->isConst)}}); }
    void visitConstDecl(ConstDecl* expr) override { result = makeASTNode("ConstDecl", expr->name.line, {{"name", Value(expr->name.lexeme)}}); }
    void visitDeleteExpr(DeleteExpr* expr) override {
        ObjList* names = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(names);
        for (auto& n : expr->names) names->vec.push_back(Value(n.lexeme));
        result = makeASTNode("DeleteExpr", 0, {{"names", Value(names)}});
    }
    void visitCompoundAssign(CompoundAssign* expr) override {
        expr->target->accept(*this); Value tgt = result;
        GcValueGuard tgtGuard(tgt);
        expr->value->accept(*this); Value val = result;
        result = makeASTNode("CompoundAssign", 0, {
            {"target", tgt}, {"op", Value(static_cast<double>(expr->op))}, {"value", val},
            {"isRef", Value(expr->isRef)}, {"isState", Value(expr->isState)}, {"isLocal", Value(expr->isLocal)}
        });
    }
    void visitInvokeExpr(InvokeExpr* expr) override {
        expr->callee->accept(*this); Value callee = result;
        result = makeASTNode("InvokeExpr", 0, {{"callee", callee}, {"arguments", makeExprListT(expr->arguments)}});
    }
    void visitThrowExpr(ThrowExpr* expr) override {
        expr->value->accept(*this); Value val = result;
        result = makeASTNode("ThrowExpr", expr->keyword.line, {{"value", val}});
    }
    void visitImportExpr(ImportExpr* expr) override {
        expr->path->accept(*this); Value path = result;
        result = makeASTNode("ImportExpr", 0, {{"path", path}});
    }
    void visitDotAccess(DotAccess* expr) override {
        expr->object->accept(*this); Value obj = result;
        result = makeASTNode("DotAccess", expr->field.line, {{"object", obj}, {"field", Value(expr->field.lexeme)}});
    }
    void visitDotAssign(DotAssign* expr) override {
        expr->object->accept(*this); Value obj = result;
        GcValueGuard objGuard(obj);
        expr->value->accept(*this); Value val = result;
        result = makeASTNode("DotAssign", expr->field.line, {{"object", obj}, {"field", Value(expr->field.lexeme)}, {"value", val}});
    }
    void visitMethodCallExpr(MethodCallExpr* expr) override {
        expr->object->accept(*this); Value obj = result;
        result = makeASTNode("MethodCallExpr", expr->method.line, {{"object", obj}, {"method", Value(expr->method.lexeme)}, {"arguments", makeExprListT(expr->arguments)}});
    }
    void visitSuperExpr(SuperExpr*) override { result = makeASTNode("SuperExpr", 0, {}); }
    void visitSelfExpr(SelfExpr*) override { result = makeASTNode("SelfExpr", 0, {}); }
    void visitFStringExpr(FStringExpr* expr) override {
        ObjList* lits = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard1(lits);
        for (auto& s : expr->literals) lits->vec.push_back(Value(s));
        ObjList* specs = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard2(specs);
        for (auto& s : expr->formatSpecs) specs->vec.push_back(Value(s));
        result = makeASTNode("FStringExpr", 0, {{"literals", Value(lits)}, {"exprs", makeExprListT(expr->exprs)}, {"formatSpecs", Value(specs)}});
    }
    void visitDictLiteral(DictLiteral* expr) override {
        ObjList* entries = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(entries);
        for (auto& pair : expr->entries) {
            ObjList* kv = GcHeap::get().allocate<ObjList>();
            GcObjGuard kvGuard(kv);
            pair.first->accept(*this); kv->vec.push_back(result);
            pair.second->accept(*this); kv->vec.push_back(result);
            entries->vec.push_back(Value(kv));
        }
        result = makeASTNode("DictLiteral", 0, {{"entries", Value(entries)}});
    }
    void visitSetLiteral(SetLiteral* expr) override { result = makeASTNode("SetLiteral", 0, {{"elements", makeExprListT(expr->elements)}}); }
    void visitSliceExpr(SliceExpr* expr) override {
        Value st = Value::none(), en = Value::none(), sp = Value::none();
        if (expr->start) { expr->start->accept(*this); st = result; }
        GcValueGuard stGuard(st);
        if (expr->end) { expr->end->accept(*this); en = result; }
        GcValueGuard enGuard(en);
        if (expr->step) { expr->step->accept(*this); sp = result; }
        result = makeASTNode("SliceExpr", 0, {{"start", st}, {"end", en}, {"step", sp}});
    }
    void visitSequenceExpr(SequenceExpr* expr) override { result = makeASTNode("SequenceExpr", 0, {{"expressions", makeExprListT(expr->expressions)}}); }
    void visitGroupingExpr(GroupingExpr* expr) override {
        expr->expression->accept(*this); Value inner = result;
        result = makeASTNode("GroupingExpr", 0, {{"expression", inner}});
    }

    void visitLambdaExpr(LambdaExpr* expr) override {
        ObjList* params = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard1(params);
        for (auto& p : expr->params) params->vec.push_back(Value(p.lexeme));
        
        ObjList* paramIsRef = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard2(paramIsRef);
        for (bool b : expr->paramIsRef) paramIsRef->vec.push_back(Value(b));
        
        ObjList* paramIsConst = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard3(paramIsConst);
        for (bool b : expr->paramIsConst) paramIsConst->vec.push_back(Value(b));
        
        Value retTypeVal = Value::none();
        if (expr->returnType) {
            expr->returnType->accept(*this);
            retTypeVal = result;
        }
        GcValueGuard retGuard(retTypeVal);

        expr->body->accept(*this); Value bodyVal = result;
        
        result = makeASTNode("LambdaExpr", 0, {
            {"name", Value(expr->name)},
            {"params", Value(params)},
            {"paramIsRef", Value(paramIsRef)},
            {"paramIsConst", Value(paramIsConst)},
            {"defaultExprs", makeExprListT(expr->defaultExprs)},
            {"hasRestParam", Value(expr->hasRestParam)},
            {"paramTypes", makeExprListT(expr->paramTypes)},
            {"returnType", retTypeVal},
            {"rawBody", Value(expr->rawBody)},
            {"body", bodyVal}
        });
    }

    void visitForInExpr(ForInExpr* expr) override {
        expr->iterable->accept(*this); Value iter = result;
        GcValueGuard iterGuard(iter);
        expr->body->accept(*this); Value body = result;
        result = makeASTNode("ForInExpr", 0, {
            {"pattern", patternToJC2(expr->pattern.get())},
            {"iterable", iter},
            {"body", body},
            {"isLocal", Value(expr->isLocal)},
            {"isConst", Value(expr->isConst)}
        });
    }

    void visitTryCatchExpr(TryCatchExpr* expr) override {
        expr->tryBody->accept(*this); Value tryB = result;
        GcValueGuard tryBGuard(tryB);
        expr->catchBody->accept(*this); Value catchB = result;
        result = makeASTNode("TryCatchExpr", 0, {
            {"tryBody", tryB},
            {"catchPattern", patternToJC2(expr->catchPattern.get())},
            {"catchBody", catchB}
        });
    }

    void visitSwitchExpr(SwitchExpr* expr) override {
        expr->subject->accept(*this); Value subj = result;
        ObjList* cases = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(cases);
        for (auto& c : expr->cases) {
            ObjList* casePair = GcHeap::get().allocate<ObjList>();
            GcObjGuard cpGuard(casePair);
            casePair->vec.push_back(makeExprListT(c.first));
            c.second->accept(*this); casePair->vec.push_back(result);
            cases->vec.push_back(Value(casePair));
        }
        Value defB = Value::none();
        if (expr->defaultBody) { expr->defaultBody->accept(*this); defB = result; }
        result = makeASTNode("SwitchExpr", 0, {
            {"subject", subj},
            {"cases", Value(cases)},
            {"defaultBody", defB}
        });
    }

    void visitClassDefExpr(ClassDefExpr* expr) override {
        Value sup = Value::none();
        if (expr->superClassExpr) { expr->superClassExpr->accept(*this); sup = result; }
        GcValueGuard supGuard(sup);
        
        auto serializeMethods = [&](const std::vector<ClassDefExpr::MethodDef>& methodsList) {
            ObjList* methods = GcHeap::get().allocate<ObjList>();
            for (auto& m : methodsList) {
                ObjList* params = GcHeap::get().allocate<ObjList>();
                GcObjGuard pGuard(params);
                for (auto& p : m.params) params->vec.push_back(Value(p.lexeme));
                
                ObjList* paramIsRef = GcHeap::get().allocate<ObjList>();
                GcObjGuard prGuard(paramIsRef);
                for (bool b : m.paramIsRef) paramIsRef->vec.push_back(Value(b));
                
                ObjList* paramIsConst = GcHeap::get().allocate<ObjList>();
                GcObjGuard pcGuard(paramIsConst);
                for (bool b : m.paramIsConst) paramIsConst->vec.push_back(Value(b));
                
                Value retTypeVal = Value::none();
                if (m.returnType) {
                    m.returnType->accept(*this);
                    retTypeVal = result;
                }
                GcValueGuard retGuard(retTypeVal);

                m.body->accept(*this); Value bodyVal = result;
                
                Value methodNode = makeASTNode("MethodDef", m.name.line, {
                    {"name", Value(m.name.lexeme)},
                    {"params", Value(params)},
                    {"paramIsRef", Value(paramIsRef)},
                    {"paramIsConst", Value(paramIsConst)},
                    {"defaultExprs", makeExprListT(m.defaultExprs)},
                    {"hasRestParam", Value(m.hasRestParam)},
                    {"paramTypes", makeExprListT(m.paramTypes)},
                    {"returnType", retTypeVal},
                    {"rawBody", Value(m.rawBody)},
                    {"body", bodyVal}
                });
                methods->vec.push_back(methodNode);
            }
            return methods;
        };

        ObjList* methods = serializeMethods(expr->methods);
        GcObjGuard guard(methods);
        ObjList* staticMethods = serializeMethods(expr->staticMethods);
        GcObjGuard smGuard(staticMethods);

        ObjList* staticFields = GcHeap::get().allocate<ObjList>();
        GcObjGuard sfGuard(staticFields);
        for (auto& f : expr->staticFields) {
            ObjList* pair = GcHeap::get().allocate<ObjList>();
            GcObjGuard pGuard(pair);
            pair->vec.push_back(Value(f.name.lexeme));
            f.value->accept(*this);
            pair->vec.push_back(result);
            staticFields->vec.push_back(Value(pair));
        }
        
        result = makeASTNode("ClassDefExpr", expr->name.line, {
            {"name", Value(expr->name.lexeme)},
            {"superClassExpr", sup},
            {"methods", Value(methods)},
            {"staticMethods", Value(staticMethods)},
            {"staticFields", Value(staticFields)}
        });
    }

    void visitNamespaceDecl(NamespaceDecl* expr) override {
        expr->body->accept(*this); Value body = result;
        result = makeASTNode("NamespaceDecl", expr->name.line, {
            {"name", Value(expr->name.lexeme)},
            {"body", body}
        });
    }

    void visitEnumDefExpr(EnumDefExpr* expr) override {
        ObjList* members = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(members);
        for (auto& m : expr->members) {
            ObjList* pair = GcHeap::get().allocate<ObjList>();
            GcObjGuard pGuard(pair);
            pair->vec.push_back(Value(m.first.lexeme));
            if (m.second) {
                m.second->accept(*this);
                pair->vec.push_back(result);
            } else {
                pair->vec.push_back(Value::none());
            }
            members->vec.push_back(Value(pair));
        }
        result = makeASTNode("EnumDefExpr", expr->name.line, {
            {"name", Value(expr->name.lexeme)},
            {"members", Value(members)}
        });
    }

    void visitDestructAssign(DestructAssign* expr) override {
        expr->value->accept(*this); Value val = result;
        GcValueGuard valGuard(val);
        result = makeASTNode("DestructAssign", 0, {
            {"pattern", patternToJC2(expr->pattern.get())},
            {"value", val},
            {"isRef", Value(expr->isRef)},
            {"isState", Value(expr->isState)},
            {"isLocal", Value(expr->isLocal)},
            {"isConst", Value(expr->isConst)}
        });
    }

    void visitListCompExpr(ListCompExpr* expr) override {
        expr->valueExpr->accept(*this); Value valExpr = result;
        GcValueGuard valGuard(valExpr);
        ObjList* clauses = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(clauses);
        for (auto& c : expr->clauses) {
            c.iterable->accept(*this); Value iter = result;
            GcValueGuard iterGuard(iter);
            Value pat = patternToJC2(c.pattern.get());
            GcValueGuard patGuard(pat);
            Value conds = makeExprListT(c.conditions);
            Value clauseNode = makeASTNode("CompClause", 0, {
                {"pattern", pat},
                {"iterable", iter},
                {"conditions", conds}
            });
            clauses->vec.push_back(clauseNode);
        }
        result = makeASTNode("ListCompExpr", 0, {
            {"valueExpr", valExpr},
            {"clauses", Value(clauses)},
            {"forceList", Value(expr->forceList)}
        });
    }

    void visitSetCompExpr(SetCompExpr* expr) override {
        expr->valueExpr->accept(*this); Value valExpr = result;
        GcValueGuard valGuard(valExpr);
        ObjList* clauses = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(clauses);
        for (auto& c : expr->clauses) {
            c.iterable->accept(*this); Value iter = result;
            GcValueGuard iterGuard(iter);
            Value pat = patternToJC2(c.pattern.get());
            GcValueGuard patGuard(pat);
            Value conds = makeExprListT(c.conditions);
            Value clauseNode = makeASTNode("CompClause", 0, {
                {"pattern", pat},
                {"iterable", iter},
                {"conditions", conds}
            });
            clauses->vec.push_back(clauseNode);
        }
        result = makeASTNode("SetCompExpr", 0, {
            {"valueExpr", valExpr},
            {"clauses", Value(clauses)}
        });
    }

    void visitDictCompExpr(DictCompExpr* expr) override {
        expr->keyExpr->accept(*this); Value keyExpr = result;
        GcValueGuard keyGuard(keyExpr);
        expr->valueExpr->accept(*this); Value valExpr = result;
        GcValueGuard valGuard(valExpr);
        ObjList* clauses = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(clauses);
        for (auto& c : expr->clauses) {
            c.iterable->accept(*this); Value iter = result;
            GcValueGuard iterGuard(iter);
            Value pat = patternToJC2(c.pattern.get());
            GcValueGuard patGuard(pat);
            Value conds = makeExprListT(c.conditions);
            Value clauseNode = makeASTNode("CompClause", 0, {
                {"pattern", pat},
                {"iterable", iter},
                {"conditions", conds}
            });
            clauses->vec.push_back(clauseNode);
        }
        result = makeASTNode("DictCompExpr", 0, {
            {"keyExpr", keyExpr},
            {"valueExpr", valExpr},
            {"clauses", Value(clauses)}
        });
    }

    void visitMatchExpr(MatchExpr* expr) override {
        expr->subject->accept(*this); Value subj = result;
        GcValueGuard subjGuard(subj);
        ObjList* branches = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(branches);
        for (auto& b : expr->branches) {
            ObjList* pats = GcHeap::get().allocate<ObjList>();
            GcObjGuard patsGuard(pats);
            for (auto& p : b.patterns) pats->vec.push_back(patternToJC2(p.get()));
            
            Value guardVal = Value::none();
            if (b.guard) { b.guard->accept(*this); guardVal = result; }
            GcValueGuard gGuard(guardVal);
            
            b.body->accept(*this); Value bodyVal = result;
            
            Value branchNode = makeASTNode("MatchBranch", 0, {
                {"patterns", Value(pats)},
                {"guard", guardVal},
                {"body", bodyVal}
            });
            branches->vec.push_back(branchNode);
        }
        result = makeASTNode("MatchExpr", 0, {
            {"subject", subj},
            {"branches", Value(branches)}
        });
    }

    void visitMacroDefExpr(MacroDefExpr* expr) override {
        ObjList* params = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(params);
        for (auto& p : expr->params) params->vec.push_back(Value(p.lexeme));
        
        expr->body->accept(*this); Value body = result;
        result = makeASTNode("MacroDefExpr", expr->name.line, {
            {"name", Value(expr->name.lexeme)},
            {"params", Value(params)},
            {"hasRestParam", Value(expr->hasRestParam)},
            {"isTokenMacro", Value(expr->isTokenMacro)},
            {"body", body}
        });
    }

    void visitMacroCallExpr(MacroCallExpr* expr) override {
        result = makeASTNode("MacroCallExpr", expr->macroName.line, {
            {"macroName", Value(expr->macroName.lexeme)},
            {"arguments", makeExprListT(expr->arguments)}
        });
    }

    void visitQuoteExpr(QuoteExpr* expr) override {
        expr->body->accept(*this); Value body = result;
        result = makeASTNode("QuoteExpr", 0, {
            {"body", body}
        });
    }

    void visitUnquoteExpr(UnquoteExpr* expr) override {
        expr->expr->accept(*this); Value inner = result;
        result = makeASTNode("UnquoteExpr", 0, {
            {"expr", inner}
        });
    }

    void visitExprAssign(ExprAssign* expr) override {
        expr->target->accept(*this); Value tgt = result;
        GcValueGuard tgtGuard(tgt);
        expr->value->accept(*this); Value val = result;
        result = makeASTNode("ExprAssign", 0, {
            {"target", tgt},
            {"value", val},
            {"isRef", Value(expr->isRef)},
            {"isState", Value(expr->isState)},
            {"isLocal", Value(expr->isLocal)},
            {"isConst", Value(expr->isConst)}
        });
    }

    void visitDeferExpr(DeferExpr* expr) override {
        expr->body->accept(*this); Value body = result;
        result = makeASTNode("DeferExpr", 0, {
            {"body", body}
        });
    }
};

Value AST_to_JC2(Expr* expr) {
    if (!expr) return Value::none();
    ASTToJC2Visitor visitor;
    expr->accept(visitor);
    return visitor.result;
}

std::unique_ptr<Pattern> jc2ToPattern(const Value& val) {
    if (val.isNone()) return nullptr;
    auto inst = val.asInstance();
    auto getProp = [&](const std::string& key) -> Value {
        Value kv(key);
        if (inst->fields && inst->fields->keyMap.count(kv)) {
            return inst->fields->elements[inst->fields->keyMap[kv]].second;
        }
        return Value::none();
    };
    std::string type = getProp("type").asString();
    
    if (type == "LiteralPattern") {
        return std::make_unique<LiteralPattern>(JC2_to_AST(getProp("literal")));
    } else if (type == "ExprPattern") {
        return std::make_unique<ExprPattern>(JC2_to_AST(getProp("expr")));
    } else if (type == "DynamicAssertPattern") {
        return std::make_unique<DynamicAssertPattern>(JC2_to_AST(getProp("expr")));
    } else if (type == "VariablePattern") {
        Token name(TokenType::IDENTIFIER, getProp("name").asString(), 0);
        return std::make_unique<VariablePattern>(name, static_cast<ScopeModifier>(getProp("modifier").asDouble()), getProp("isConst").truthy());
    } else if (type == "RestPattern") {
        Token name(TokenType::IDENTIFIER, getProp("name").asString(), 0);
        return std::make_unique<RestPattern>(name, static_cast<ScopeModifier>(getProp("modifier").asDouble()), getProp("isConst").truthy());
    } else if (type == "ListPattern") {
        std::vector<std::unique_ptr<Pattern>> elements;
        Value elemsVal = getProp("elements");
        if (elemsVal.isObjType(ObjType::LIST)) {
            for (const auto& v : static_cast<ObjList*>(elemsVal.asObj())->vec) elements.push_back(jc2ToPattern(v));
        }
        auto rest = jc2ToPattern(getProp("rest"));
        return std::make_unique<ListPattern>(std::move(elements), std::unique_ptr<RestPattern>(static_cast<RestPattern*>(rest.release())));
    } else if (type == "MatrixPattern") {
        std::vector<std::vector<std::unique_ptr<Pattern>>> rows;
        Value rowsVal = getProp("rows");
        if (rowsVal.isObjType(ObjType::LIST)) {
            for (const auto& rVal : static_cast<ObjList*>(rowsVal.asObj())->vec) {
                std::vector<std::unique_ptr<Pattern>> row;
                if (rVal.isObjType(ObjType::LIST)) {
                    for (const auto& v : static_cast<ObjList*>(rVal.asObj())->vec) row.push_back(jc2ToPattern(v));
                }
                rows.push_back(std::move(row));
            }
        }
        auto restRow = jc2ToPattern(getProp("restRow"));
        return std::make_unique<MatrixPattern>(std::move(rows), std::unique_ptr<RestPattern>(static_cast<RestPattern*>(restRow.release())));
    } else if (type == "DictPattern") {
        std::vector<std::pair<std::string, std::unique_ptr<Pattern>>> entries;
        Value entriesVal = getProp("entries");
        if (entriesVal.isObjType(ObjType::LIST)) {
            for (const auto& v : static_cast<ObjList*>(entriesVal.asObj())->vec) {
                if (v.isObjType(ObjType::LIST)) {
                    auto kv = static_cast<ObjList*>(v.asObj());
                    if (kv->vec.size() >= 2) entries.push_back({kv->vec[0].asString(), jc2ToPattern(kv->vec[1])});
                }
            }
        }
        auto rest = jc2ToPattern(getProp("rest"));
        return std::make_unique<DictPattern>(std::move(entries), std::unique_ptr<RestPattern>(static_cast<RestPattern*>(rest.release())));
    } else if (type == "DefaultPattern") {
        return std::make_unique<DefaultPattern>(jc2ToPattern(getProp("inner")), JC2_to_AST(getProp("defaultExpr")));
    }
    throw std::runtime_error("Macro Error: Unsupported pattern type '" + type + "'");
}

std::unique_ptr<Expr> JC2_to_AST(const Value& val) {
    if (val.isNone()) return nullptr;
    
    // ★ 核心机制：如果宏返回一个 List，将其转换为 SequenceExpr 以实现平铺（不引入作用域）
    if (val.isObjType(ObjType::LIST)) {
        auto objList = static_cast<ObjList*>(val.asObj());
        std::vector<std::unique_ptr<Expr>> exprs;
        for (const auto& v : objList->vec) {
            if (!v.isNone()) {
                exprs.push_back(JC2_to_AST(v));
            }
        }
        return std::make_unique<SequenceExpr>(std::move(exprs));
    }

    if (!val.isInstance()) throw std::runtime_error("Macro Error: Expected ASTNode instance or List of ASTNodes");
    
    auto inst = val.asInstance();
    if (!inst->classDef || inst->classDef->name != "ASTNode") {
        throw std::runtime_error("Macro Error: Expected ASTNode instance or List of ASTNodes");
    }
    
    auto getProp = [&](const std::string& key) -> Value {
        Value kv(key);
        if (inst->fields && inst->fields->keyMap.count(kv)) {
            return inst->fields->elements[inst->fields->keyMap[kv]].second;
        }
        return Value::none();
    };

    auto getExprList = [&](const Value& listVal) -> std::vector<std::unique_ptr<Expr>> {
        std::vector<std::unique_ptr<Expr>> list;
        if (listVal.isObjType(ObjType::LIST)) {
            auto objList = static_cast<ObjList*>(listVal.asObj());
            for (const auto& v : objList->vec) {
                if (v.isNone()) list.push_back(nullptr);
                else list.push_back(JC2_to_AST(v));
            }
        }
        return list;
    };

    std::string type = getProp("type").asString();
    int line = getProp("line").isInt32() ? getProp("line").asInt32() : static_cast<int>(getProp("line").asDouble());

    if (type == "Binary") {
        auto left = JC2_to_AST(getProp("left"));
        auto right = JC2_to_AST(getProp("right"));
        std::string opStr = getProp("op").asString();
        Token op(stringToTokenType(opStr), opStr, line);
        return std::make_unique<Binary>(std::move(left), op, std::move(right));
    } else if (type == "Unary") {
        auto right = JC2_to_AST(getProp("right"));
        std::string opStr = getProp("op").asString();
        Token op(stringToTokenType(opStr), opStr, line);
        return std::make_unique<Unary>(op, std::move(right));
    } else if (type == "Literal") {
        return std::make_unique<Literal>(
            getProp("value").asString(),
            getProp("isString").truthy(),
            getProp("isImaginary").truthy(),
            getProp("isKeyword").truthy()
        );
    } else if (type == "Variable") {
        return std::make_unique<Variable>(Token(TokenType::IDENTIFIER, getProp("name").asString(), line));
    } else if (type == "Assign") {
        auto value = JC2_to_AST(getProp("value"));
        return std::make_unique<Assign>(
            Token(TokenType::IDENTIFIER, getProp("name").asString(), line),
            std::move(value),
            getProp("isRef").truthy(),
            getProp("isState").truthy(),
            getProp("isLocal").truthy(),
            getProp("isConst").truthy()
        );
    } else if (type == "Block") {
        return std::make_unique<Block>(getExprList(getProp("statements")));
    } else if (type == "Call") {
        Token callee(TokenType::IDENTIFIER, getProp("callee").asString(), line);
        return std::make_unique<Call>(callee, getExprList(getProp("arguments")));
    } else if (type == "MatrixNode") {
        std::vector<std::vector<std::unique_ptr<Expr>>> elements;
        Value rowsVal = getProp("elements");
        if (rowsVal.isObjType(ObjType::LIST)) {
            for (const auto& rowVal : static_cast<ObjList*>(rowsVal.asObj())->vec) {
                elements.push_back(getExprList(rowVal));
            }
        }
        return std::make_unique<MatrixNode>(std::move(elements), getProp("forceList").truthy());
    } else if (type == "IfExpr") {
        return std::make_unique<IfExpr>(JC2_to_AST(getProp("condition")), JC2_to_AST(getProp("thenBranch")), JC2_to_AST(getProp("elseBranch")));
    } else if (type == "WhileExpr") {
        return std::make_unique<WhileExpr>(JC2_to_AST(getProp("condition")), JC2_to_AST(getProp("body")));
    } else if (type == "ForExpr") {
        return std::make_unique<ForExpr>(JC2_to_AST(getProp("initializer")), JC2_to_AST(getProp("condition")), JC2_to_AST(getProp("update")), JC2_to_AST(getProp("body")));
    } else if (type == "BreakExpr") {
        return std::make_unique<BreakExpr>(Token(TokenType::BREAK, "break", line));
    } else if (type == "ContinueExpr") {
        return std::make_unique<ContinueExpr>(Token(TokenType::CONTINUE, "continue", line));
    } else if (type == "ReturnExpr") {
        return std::make_unique<ReturnExpr>(Token(TokenType::RETURN, "return", line), JC2_to_AST(getProp("value")));
    } else if (type == "IndexAccess") {
        return std::make_unique<IndexAccess>(JC2_to_AST(getProp("object")), getExprList(getProp("indices")));
    } else if (type == "IndexAssign") {
        Token name(TokenType::IDENTIFIER, getProp("name").asString(), line);
        auto objExpr = JC2_to_AST(getProp("objectExpr"));
        auto valExpr = JC2_to_AST(getProp("value"));
        std::vector<std::vector<std::unique_ptr<Expr>>> chain;
        Value chainVal = getProp("indexChain");
        if (chainVal.isObjType(ObjType::LIST)) {
            for (const auto& levelVal : static_cast<ObjList*>(chainVal.asObj())->vec) chain.push_back(getExprList(levelVal));
        }
        if (objExpr) return std::make_unique<IndexAssign>(std::move(objExpr), std::move(chain), std::move(valExpr));
        return std::make_unique<IndexAssign>(name, std::move(chain), std::move(valExpr));
    } else if (type == "LocalDecl") {
        return std::make_unique<LocalDecl>(Token(TokenType::IDENTIFIER, getProp("name").asString(), line), getProp("isConst").truthy());
    } else if (type == "RefDecl") {
        return std::make_unique<RefDecl>(Token(TokenType::IDENTIFIER, getProp("name").asString(), line), getProp("isConst").truthy());
    } else if (type == "StateDecl") {
        return std::make_unique<StateDecl>(Token(TokenType::IDENTIFIER, getProp("name").asString(), line), getProp("isConst").truthy());
    } else if (type == "ConstDecl") {
        return std::make_unique<ConstDecl>(Token(TokenType::IDENTIFIER, getProp("name").asString(), line));
    } else if (type == "DeleteExpr") {
        std::vector<Token> names;
        Value namesVal = getProp("names");
        if (namesVal.isObjType(ObjType::LIST)) {
            for (const auto& v : static_cast<ObjList*>(namesVal.asObj())->vec) names.push_back(Token(TokenType::IDENTIFIER, v.asString(), line));
        }
        return std::make_unique<DeleteExpr>(std::move(names));
    } else if (type == "CompoundAssign") {
        return std::make_unique<CompoundAssign>(JC2_to_AST(getProp("target")), static_cast<TokenType>(getProp("op").asDouble()), JC2_to_AST(getProp("value")), getProp("isRef").truthy(), getProp("isState").truthy(), getProp("isLocal").truthy());
    } else if (type == "InvokeExpr") {
        return std::make_unique<InvokeExpr>(JC2_to_AST(getProp("callee")), getExprList(getProp("arguments")));
    } else if (type == "ThrowExpr") {
        return std::make_unique<ThrowExpr>(Token(TokenType::THROW, "throw", line), JC2_to_AST(getProp("value")));
    } else if (type == "ImportExpr") {
        return std::make_unique<ImportExpr>(JC2_to_AST(getProp("path")));
    } else if (type == "DotAccess") {
        return std::make_unique<DotAccess>(JC2_to_AST(getProp("object")), Token(TokenType::IDENTIFIER, getProp("field").asString(), line));
    } else if (type == "DotAssign") {
        return std::make_unique<DotAssign>(JC2_to_AST(getProp("object")), Token(TokenType::IDENTIFIER, getProp("field").asString(), line), JC2_to_AST(getProp("value")));
    } else if (type == "MethodCallExpr") {
        return std::make_unique<MethodCallExpr>(JC2_to_AST(getProp("object")), Token(TokenType::IDENTIFIER, getProp("method").asString(), line), getExprList(getProp("arguments")));
    } else if (type == "SuperExpr") {
        return std::make_unique<SuperExpr>();
    } else if (type == "SelfExpr") {
        return std::make_unique<SelfExpr>();
    } else if (type == "FStringExpr") {
        std::vector<std::string> literals, specs;
        Value litsVal = getProp("literals"), specsVal = getProp("formatSpecs");
        if (litsVal.isObjType(ObjType::LIST)) for (const auto& v : static_cast<ObjList*>(litsVal.asObj())->vec) literals.push_back(v.asString());
        if (specsVal.isObjType(ObjType::LIST)) for (const auto& v : static_cast<ObjList*>(specsVal.asObj())->vec) specs.push_back(v.asString());
        return std::make_unique<FStringExpr>(std::move(literals), getExprList(getProp("exprs")), std::move(specs));
    } else if (type == "DictLiteral") {
        std::vector<std::pair<std::unique_ptr<Expr>, std::unique_ptr<Expr>>> entries;
        Value entriesVal = getProp("entries");
        if (entriesVal.isObjType(ObjType::LIST)) {
            for (const auto& v : static_cast<ObjList*>(entriesVal.asObj())->vec) {
                if (v.isObjType(ObjType::LIST)) {
                    auto kv = static_cast<ObjList*>(v.asObj());
                    if (kv->vec.size() >= 2) entries.push_back({JC2_to_AST(kv->vec[0]), JC2_to_AST(kv->vec[1])});
                }
            }
        }
        return std::make_unique<DictLiteral>(std::move(entries));
    } else if (type == "SetLiteral") {
        return std::make_unique<SetLiteral>(getExprList(getProp("elements")));
    } else if (type == "SliceExpr") {
        return std::make_unique<SliceExpr>(JC2_to_AST(getProp("start")), JC2_to_AST(getProp("end")), JC2_to_AST(getProp("step")));
    } else if (type == "SequenceExpr") {
        return std::make_unique<SequenceExpr>(getExprList(getProp("expressions")));
    } else if (type == "GroupingExpr") {
        return std::make_unique<GroupingExpr>(JC2_to_AST(getProp("expression")));
    } else if (type == "LambdaExpr") {
        std::vector<Token> params;
        Value paramsVal = getProp("params");
        if (paramsVal.isObjType(ObjType::LIST)) {
            for (const auto& v : static_cast<ObjList*>(paramsVal.asObj())->vec) {
                params.push_back(Token(TokenType::IDENTIFIER, v.asString(), line));
            }
        }
        std::vector<bool> paramIsRef, paramIsConst;
        Value refVal = getProp("paramIsRef"), constVal = getProp("paramIsConst");
        if (refVal.isObjType(ObjType::LIST)) {
            for (const auto& v : static_cast<ObjList*>(refVal.asObj())->vec) paramIsRef.push_back(v.truthy());
        }
        if (constVal.isObjType(ObjType::LIST)) {
            for (const auto& v : static_cast<ObjList*>(constVal.asObj())->vec) paramIsConst.push_back(v.truthy());
        }
        std::vector<std::shared_ptr<Expr>> paramTypes;
        for (auto& e : getExprList(getProp("paramTypes"))) {
            paramTypes.push_back(std::shared_ptr<Expr>(e.release()));
        }
        std::vector<std::shared_ptr<Expr>> defaultExprs;
        for (auto& e : getExprList(getProp("defaultExprs"))) {
            defaultExprs.push_back(std::shared_ptr<Expr>(e.release()));
        }
        return std::make_unique<LambdaExpr>(
            getProp("name").asString(), std::move(params), std::move(paramIsRef), std::move(paramIsConst),
            std::move(defaultExprs), getProp("hasRestParam").truthy(),
            std::move(paramTypes), std::shared_ptr<Expr>(JC2_to_AST(getProp("returnType")).release()),
            getProp("rawBody").asString(), std::shared_ptr<Expr>(JC2_to_AST(getProp("body")).release())
        );
    } else if (type == "ForInExpr") {
        return std::make_unique<ForInExpr>(
            jc2ToPattern(getProp("pattern")),
            JC2_to_AST(getProp("iterable")),
            JC2_to_AST(getProp("body")),
            getProp("isLocal").truthy(),
            getProp("isConst").truthy()
        );
    } else if (type == "TryCatchExpr") {
        return std::make_unique<TryCatchExpr>(
            JC2_to_AST(getProp("tryBody")),
            jc2ToPattern(getProp("catchPattern")),
            JC2_to_AST(getProp("catchBody"))
        );
    } else if (type == "SwitchExpr") {
        std::vector<std::pair<std::vector<std::unique_ptr<Expr>>, std::unique_ptr<Expr>>> cases;
        Value casesVal = getProp("cases");
        if (casesVal.isObjType(ObjType::LIST)) {
            for (const auto& cVal : static_cast<ObjList*>(casesVal.asObj())->vec) {
                if (cVal.isObjType(ObjType::LIST)) {
                    auto cList = static_cast<ObjList*>(cVal.asObj());
                    if (cList->vec.size() >= 2) {
                        std::vector<std::unique_ptr<Expr>> vals;
                        if (cList->vec[0].isObjType(ObjType::LIST)) {
                            for (const auto& v : static_cast<ObjList*>(cList->vec[0].asObj())->vec) {
                                vals.push_back(JC2_to_AST(v));
                            }
                        }
                        cases.push_back({std::move(vals), JC2_to_AST(cList->vec[1])});
                    }
                }
            }
        }
        return std::make_unique<SwitchExpr>(
            JC2_to_AST(getProp("subject")),
            std::move(cases),
            JC2_to_AST(getProp("defaultBody"))
        );
    } else if (type == "ClassDefExpr") {
        auto parseMethods = [&](const Value& methodsVal) {
            std::vector<ClassDefExpr::MethodDef> methods;
            if (methodsVal.isObjType(ObjType::LIST)) {
                for (const auto& mVal : static_cast<ObjList*>(methodsVal.asObj())->vec) {
                    auto mInst = mVal.asInstance();
                    auto getMProp = [&](const std::string& key) -> Value {
                        Value kv(key);
                        if (mInst->fields && mInst->fields->keyMap.count(kv)) {
                            return mInst->fields->elements[mInst->fields->keyMap[kv]].second;
                        }
                        return Value::none();
                    };
                    ClassDefExpr::MethodDef method{
                        Token(TokenType::IDENTIFIER, getMProp("name").asString(), line),
                        {}, {}, {}, {}, false, {}, nullptr, "", nullptr, -1
                    };
                    Value paramsVal = getMProp("params");
                    if (paramsVal.isObjType(ObjType::LIST)) {
                        for (const auto& pVal : static_cast<ObjList*>(paramsVal.asObj())->vec) {
                            method.params.push_back(Token(TokenType::IDENTIFIER, pVal.asString(), line));
                        }
                    }
                    Value refVal = getMProp("paramIsRef");
                    if (refVal.isObjType(ObjType::LIST)) {
                        for (const auto& rVal : static_cast<ObjList*>(refVal.asObj())->vec) method.paramIsRef.push_back(rVal.truthy());
                    }
                    Value constVal = getMProp("paramIsConst");
                    if (constVal.isObjType(ObjType::LIST)) {
                        for (const auto& cVal : static_cast<ObjList*>(constVal.asObj())->vec) method.paramIsConst.push_back(cVal.truthy());
                    }
                    Value defsVal = getMProp("defaultExprs");
                    if (defsVal.isObjType(ObjType::LIST)) {
                        for (const auto& dVal : static_cast<ObjList*>(defsVal.asObj())->vec) {
                            method.defaultExprs.push_back(std::shared_ptr<Expr>(JC2_to_AST(dVal).release()));
                        }
                    }
                    method.hasRestParam = getMProp("hasRestParam").truthy();
                    for (auto& e : getExprList(getMProp("paramTypes"))) {
                        method.paramTypes.push_back(std::shared_ptr<Expr>(e.release()));
                    }
                    method.returnType = std::shared_ptr<Expr>(JC2_to_AST(getMProp("returnType")).release());
                    method.rawBody = getMProp("rawBody").asString();
                    method.body = std::shared_ptr<Expr>(JC2_to_AST(getMProp("body")).release());
                    methods.push_back(std::move(method));
                }
            }
            return methods;
        };

        std::vector<ClassDefExpr::MethodDef> methods = parseMethods(getProp("methods"));
        std::vector<ClassDefExpr::MethodDef> staticMethods = parseMethods(getProp("staticMethods"));
        
        std::vector<ClassDefExpr::StaticFieldDef> staticFields;
        Value sfVal = getProp("staticFields");
        if (sfVal.isObjType(ObjType::LIST)) {
            for (const auto& fVal : static_cast<ObjList*>(sfVal.asObj())->vec) {
                if (fVal.isObjType(ObjType::LIST)) {
                    auto fList = static_cast<ObjList*>(fVal.asObj());
                    if (fList->vec.size() >= 2) {
                        Token fName(TokenType::IDENTIFIER, fList->vec[0].asString(), line);
                        staticFields.push_back({fName, JC2_to_AST(fList->vec[1])});
                    }
                }
            }
        }

        std::string clsName = "";
        if (getProp("name").isString()) clsName = getProp("name").asString();
        return std::make_unique<ClassDefExpr>(
            Token(TokenType::IDENTIFIER, clsName, line),
            JC2_to_AST(getProp("superClassExpr")),
            std::move(methods),
            std::move(staticMethods),
            std::move(staticFields)
        );
    } else if (type == "NamespaceDecl") {
        std::string nsName = "";
        if (getProp("name").isString()) nsName = getProp("name").asString();
        return std::make_unique<NamespaceDecl>(
            Token(TokenType::IDENTIFIER, nsName, line),
            JC2_to_AST(getProp("body"))
        );
    } else if (type == "EnumDefExpr") {
        std::string enumName = "";
        if (getProp("name").isString()) enumName = getProp("name").asString();
        std::vector<std::pair<Token, std::unique_ptr<Expr>>> members;
        Value membersVal = getProp("members");
        if (membersVal.isObjType(ObjType::LIST)) {
            for (const auto& mVal : static_cast<ObjList*>(membersVal.asObj())->vec) {
                if (mVal.isObjType(ObjType::LIST)) {
                    auto mList = static_cast<ObjList*>(mVal.asObj());
                    if (mList->vec.size() >= 2) {
                        Token mName(TokenType::IDENTIFIER, mList->vec[0].asString(), line);
                        std::unique_ptr<Expr> mValExpr = nullptr;
                        if (!mList->vec[1].isNone()) {
                            mValExpr = JC2_to_AST(mList->vec[1]);
                        }
                        members.push_back({mName, std::move(mValExpr)});
                    }
                }
            }
        }
        return std::make_unique<EnumDefExpr>(Token(TokenType::IDENTIFIER, enumName, line), std::move(members));
    } else if (type == "DestructAssign") {
        return std::make_unique<DestructAssign>(
            jc2ToPattern(getProp("pattern")),
            JC2_to_AST(getProp("value")),
            getProp("isRef").truthy(),
            getProp("isState").truthy(),
            getProp("isLocal").truthy(),
            getProp("isConst").truthy()
        );
    } else if (type == "ListCompExpr") {
        std::vector<CompClause> clauses;
        Value clausesVal = getProp("clauses");
        if (clausesVal.isObjType(ObjType::LIST)) {
            for (const auto& cVal : static_cast<ObjList*>(clausesVal.asObj())->vec) {
                auto cInst = cVal.asInstance();
                auto getCProp = [&](const std::string& key) -> Value {
                    Value kv(key);
                    if (cInst->fields && cInst->fields->keyMap.count(kv)) {
                        return cInst->fields->elements[cInst->fields->keyMap[kv]].second;
                    }
                    return Value::none();
                };
                CompClause clause(
                    jc2ToPattern(getCProp("pattern")),
                    std::shared_ptr<Expr>(JC2_to_AST(getCProp("iterable")).release())
                );
                Value condsVal = getCProp("conditions");
                if (condsVal.isObjType(ObjType::LIST)) {
                    for (const auto& condVal : static_cast<ObjList*>(condsVal.asObj())->vec) {
                        clause.conditions.push_back(std::shared_ptr<Expr>(JC2_to_AST(condVal).release()));
                    }
                }
                clauses.push_back(std::move(clause));
            }
        }
        return std::make_unique<ListCompExpr>(
            JC2_to_AST(getProp("valueExpr")),
            std::move(clauses),
            getProp("forceList").truthy()
        );
    } else if (type == "SetCompExpr") {
        std::vector<CompClause> clauses;
        Value clausesVal = getProp("clauses");
        if (clausesVal.isObjType(ObjType::LIST)) {
            for (const auto& cVal : static_cast<ObjList*>(clausesVal.asObj())->vec) {
                auto cInst = cVal.asInstance();
                auto getCProp = [&](const std::string& key) -> Value {
                    Value kv(key);
                    if (cInst->fields && cInst->fields->keyMap.count(kv)) {
                        return cInst->fields->elements[cInst->fields->keyMap[kv]].second;
                    }
                    return Value::none();
                };
                CompClause clause(
                    jc2ToPattern(getCProp("pattern")),
                    std::shared_ptr<Expr>(JC2_to_AST(getCProp("iterable")).release())
                );
                Value condsVal = getCProp("conditions");
                if (condsVal.isObjType(ObjType::LIST)) {
                    for (const auto& condVal : static_cast<ObjList*>(condsVal.asObj())->vec) {
                        clause.conditions.push_back(std::shared_ptr<Expr>(JC2_to_AST(condVal).release()));
                    }
                }
                clauses.push_back(std::move(clause));
            }
        }
        return std::make_unique<SetCompExpr>(
            JC2_to_AST(getProp("valueExpr")),
            std::move(clauses)
        );
    } else if (type == "DictCompExpr") {
        std::vector<CompClause> clauses;
        Value clausesVal = getProp("clauses");
        if (clausesVal.isObjType(ObjType::LIST)) {
            for (const auto& cVal : static_cast<ObjList*>(clausesVal.asObj())->vec) {
                auto cInst = cVal.asInstance();
                auto getCProp = [&](const std::string& key) -> Value {
                    Value kv(key);
                    if (cInst->fields && cInst->fields->keyMap.count(kv)) {
                        return cInst->fields->elements[cInst->fields->keyMap[kv]].second;
                    }
                    return Value::none();
                };
                CompClause clause(
                    jc2ToPattern(getCProp("pattern")),
                    std::shared_ptr<Expr>(JC2_to_AST(getCProp("iterable")).release())
                );
                Value condsVal = getCProp("conditions");
                if (condsVal.isObjType(ObjType::LIST)) {
                    for (const auto& condVal : static_cast<ObjList*>(condsVal.asObj())->vec) {
                        clause.conditions.push_back(std::shared_ptr<Expr>(JC2_to_AST(condVal).release()));
                    }
                }
                clauses.push_back(std::move(clause));
            }
        }
        return std::make_unique<DictCompExpr>(
            JC2_to_AST(getProp("keyExpr")),
            JC2_to_AST(getProp("valueExpr")),
            std::move(clauses)
        );
    } else if (type == "MatchExpr") {
        std::vector<MatchBranch> branches;
        Value branchesVal = getProp("branches");
        if (branchesVal.isObjType(ObjType::LIST)) {
            for (const auto& bVal : static_cast<ObjList*>(branchesVal.asObj())->vec) {
                auto bInst = bVal.asInstance();
                auto getBProp = [&](const std::string& key) -> Value {
                    Value kv(key);
                    if (bInst->fields && bInst->fields->keyMap.count(kv)) {
                        return bInst->fields->elements[bInst->fields->keyMap[kv]].second;
                    }
                    return Value::none();
                };
                MatchBranch branch;
                Value patsVal = getBProp("patterns");
                if (patsVal.isObjType(ObjType::LIST)) {
                    for (const auto& pVal : static_cast<ObjList*>(patsVal.asObj())->vec) {
                        branch.patterns.push_back(jc2ToPattern(pVal));
                    }
                }
                branch.guard = JC2_to_AST(getBProp("guard"));
                branch.body = JC2_to_AST(getBProp("body"));
                branches.push_back(std::move(branch));
            }
        }
        return std::make_unique<MatchExpr>(
            JC2_to_AST(getProp("subject")),
            std::move(branches)
        );
    } else if (type == "MacroDefExpr") {
        std::vector<Token> params;
        Value paramsVal = getProp("params");
        if (paramsVal.isObjType(ObjType::LIST)) {
            for (const auto& v : static_cast<ObjList*>(paramsVal.asObj())->vec) {
                params.push_back(Token(TokenType::IDENTIFIER, v.asString(), line));
            }
        }
        return std::make_unique<MacroDefExpr>(
            Token(TokenType::IDENTIFIER, getProp("name").asString(), line),
            std::move(params),
            getProp("hasRestParam").truthy(),
            getProp("isTokenMacro").truthy(),
            JC2_to_AST(getProp("body"))
        );
    } else if (type == "MacroCallExpr") {
        return std::make_unique<MacroCallExpr>(
            Token(TokenType::IDENTIFIER, getProp("macroName").asString(), line),
            getExprList(getProp("arguments"))
        );
    } else if (type == "QuoteExpr") {
        return std::make_unique<QuoteExpr>(JC2_to_AST(getProp("body")));
    } else if (type == "UnquoteExpr") {
        return std::make_unique<UnquoteExpr>(JC2_to_AST(getProp("expr")));
    } else if (type == "ExprAssign") {
        return std::make_unique<ExprAssign>(
            JC2_to_AST(getProp("target")),
            JC2_to_AST(getProp("value")),
            getProp("isRef").truthy(),
            getProp("isState").truthy(),
            getProp("isLocal").truthy(),
            getProp("isConst").truthy()
        );
    } else if (type == "DeferExpr") {
        return std::make_unique<DeferExpr>(JC2_to_AST(getProp("body")));
    }

    throw std::runtime_error("Macro Error: Unsupported node type '" + type + "'");
}

} // namespace jc
