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

    Value makeExprList(const std::vector<std::unique_ptr<Expr>>& exprs) {
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

    void visitBlock(Block* expr) override {
        result = makeASTNode("Block", 0, {{"statements", makeExprList(expr->statements)}});
    }

    void visitCall(Call* expr) override {
        result = makeASTNode("Call", expr->callee.line, {
            {"callee", Value(expr->callee.lexeme)},
            {"arguments", makeExprList(expr->arguments)}
        });
    }
    void visitMatrixNode(MatrixNode* expr) override {
        ObjList* rows = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(rows);
        for (auto& row : expr->elements) rows->vec.push_back(makeExprList(row));
        result = makeASTNode("MatrixNode", 0, {{"elements", Value(rows)}, {"forceList", Value(expr->forceList)}});
    }
    void visitIfExpr(IfExpr* expr) override {
        expr->condition->accept(*this); Value cond = result;
        expr->thenBranch->accept(*this); Value thenB = result;
        Value elseB = Value::none();
        if (expr->elseBranch) { expr->elseBranch->accept(*this); elseB = result; }
        result = makeASTNode("IfExpr", 0, {{"condition", cond}, {"thenBranch", thenB}, {"elseBranch", elseB}});
    }
    void visitWhileExpr(WhileExpr* expr) override {
        expr->condition->accept(*this); Value cond = result;
        expr->body->accept(*this); Value body = result;
        result = makeASTNode("WhileExpr", 0, {{"condition", cond}, {"body", body}});
    }
    void visitForExpr(ForExpr* expr) override {
        expr->initializer->accept(*this); Value init = result;
        expr->condition->accept(*this); Value cond = result;
        expr->update->accept(*this); Value upd = result;
        expr->body->accept(*this); Value body = result;
        result = makeASTNode("ForExpr", 0, {{"initializer", init}, {"condition", cond}, {"update", upd}, {"body", body}});
    }
    void visitBreakExpr(BreakExpr*) override { result = makeASTNode("BreakExpr", 0, {}); }
    void visitContinueExpr(ContinueExpr*) override { result = makeASTNode("ContinueExpr", 0, {}); }
    void visitReturnExpr(ReturnExpr* expr) override {
        Value val = Value::none();
        if (expr->value) { expr->value->accept(*this); val = result; }
        result = makeASTNode("ReturnExpr", 0, {{"value", val}});
    }
    void visitIndexAccess(IndexAccess* expr) override {
        expr->object->accept(*this); Value obj = result;
        result = makeASTNode("IndexAccess", 0, {{"object", obj}, {"indices", makeExprList(expr->indices)}});
    }
    void visitIndexAssign(IndexAssign* expr) override {
        Value objExpr = Value::none();
        if (expr->objectExpr) { expr->objectExpr->accept(*this); objExpr = result; }
        ObjList* chain = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(chain);
        for (auto& level : expr->indexChain) chain->vec.push_back(makeExprList(level));
        expr->value->accept(*this); Value val = result;
        result = makeASTNode("IndexAssign", expr->name.line, {
            {"name", Value(expr->name.lexeme)}, {"objectExpr", objExpr}, {"indexChain", Value(chain)}, {"value", val}
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
        expr->value->accept(*this); Value val = result;
        result = makeASTNode("CompoundAssign", 0, {
            {"target", tgt}, {"op", Value(static_cast<double>(expr->op))}, {"value", val},
            {"isRef", Value(expr->isRef)}, {"isState", Value(expr->isState)}, {"isLocal", Value(expr->isLocal)}
        });
    }
    void visitInvokeExpr(InvokeExpr* expr) override {
        expr->callee->accept(*this); Value callee = result;
        result = makeASTNode("InvokeExpr", 0, {{"callee", callee}, {"arguments", makeExprList(expr->arguments)}});
    }
    void visitThrowExpr(ThrowExpr* expr) override {
        expr->value->accept(*this); Value val = result;
        result = makeASTNode("ThrowExpr", 0, {{"value", val}});
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
        expr->value->accept(*this); Value val = result;
        result = makeASTNode("DotAssign", expr->field.line, {{"object", obj}, {"field", Value(expr->field.lexeme)}, {"value", val}});
    }
    void visitMethodCallExpr(MethodCallExpr* expr) override {
        expr->object->accept(*this); Value obj = result;
        result = makeASTNode("MethodCallExpr", expr->method.line, {{"object", obj}, {"method", Value(expr->method.lexeme)}, {"arguments", makeExprList(expr->arguments)}});
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
        result = makeASTNode("FStringExpr", 0, {{"literals", Value(lits)}, {"exprs", makeExprList(expr->exprs)}, {"formatSpecs", Value(specs)}});
    }
    void visitDictLiteral(DictLiteral* expr) override {
        ObjList* entries = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(entries);
        for (auto& pair : expr->entries) {
            ObjList* kv = GcHeap::get().allocate<ObjList>();
            pair.first->accept(*this); kv->vec.push_back(result);
            pair.second->accept(*this); kv->vec.push_back(result);
            entries->vec.push_back(Value(kv));
        }
        result = makeASTNode("DictLiteral", 0, {{"entries", Value(entries)}});
    }
    void visitSetLiteral(SetLiteral* expr) override { result = makeASTNode("SetLiteral", 0, {{"elements", makeExprList(expr->elements)}}); }
    void visitSliceExpr(SliceExpr* expr) override {
        Value st = Value::none(), en = Value::none(), sp = Value::none();
        if (expr->start) { expr->start->accept(*this); st = result; }
        if (expr->end) { expr->end->accept(*this); en = result; }
        if (expr->step) { expr->step->accept(*this); sp = result; }
        result = makeASTNode("SliceExpr", 0, {{"start", st}, {"end", en}, {"step", sp}});
    }
    void visitSequenceExpr(SequenceExpr* expr) override { result = makeASTNode("SequenceExpr", 0, {{"expressions", makeExprList(expr->expressions)}}); }
    void visitGroupingExpr(GroupingExpr* expr) override {
        expr->expression->accept(*this); Value inner = result;
        result = makeASTNode("GroupingExpr", 0, {{"expression", inner}});
    }

    // Unimplemented complex nodes
    void visitLambdaExpr(LambdaExpr*) override { throw std::runtime_error("AST_to_JC2: LambdaExpr not implemented yet"); }
    void visitForInExpr(ForInExpr*) override { throw std::runtime_error("AST_to_JC2: ForInExpr not implemented yet"); }
    void visitTryCatchExpr(TryCatchExpr*) override { throw std::runtime_error("AST_to_JC2: TryCatchExpr not implemented yet"); }
    void visitSwitchExpr(SwitchExpr*) override { throw std::runtime_error("AST_to_JC2: SwitchExpr not implemented yet"); }
    void visitClassDefExpr(ClassDefExpr*) override { throw std::runtime_error("AST_to_JC2: ClassDefExpr not implemented yet"); }
    void visitNamespaceDecl(NamespaceDecl*) override { throw std::runtime_error("AST_to_JC2: NamespaceDecl not implemented yet"); }
    void visitDestructAssign(DestructAssign*) override { throw std::runtime_error("AST_to_JC2: DestructAssign not implemented yet"); }
    void visitListCompExpr(ListCompExpr*) override { throw std::runtime_error("AST_to_JC2: ListCompExpr not implemented yet"); }
    void visitMatchExpr(MatchExpr*) override { throw std::runtime_error("AST_to_JC2: MatchExpr not implemented yet"); }
    void visitMacroDefExpr(MacroDefExpr*) override { throw std::runtime_error("AST_to_JC2: MacroDefExpr not implemented yet"); }
    void visitMacroCallExpr(MacroCallExpr*) override { throw std::runtime_error("AST_to_JC2: MacroCallExpr not implemented yet"); }
    void visitQuoteExpr(QuoteExpr*) override { throw std::runtime_error("AST_to_JC2: QuoteExpr not implemented yet"); }
    void visitUnquoteExpr(UnquoteExpr*) override { throw std::runtime_error("AST_to_JC2: UnquoteExpr not implemented yet"); }
    void visitExprAssign(ExprAssign*) override { throw std::runtime_error("AST_to_JC2: ExprAssign not implemented yet"); }
};

Value AST_to_JC2(Expr* expr) {
    if (!expr) return Value::none();
    ASTToJC2Visitor visitor;
    expr->accept(visitor);
    return visitor.result;
}

std::unique_ptr<Expr> JC2_to_AST(const Value& val) {
    if (val.isNone()) return nullptr;
    if (!val.isInstance()) throw std::runtime_error("JC2_to_AST: Expected ASTNode instance");
    
    auto inst = val.asInstance();
    if (!inst->classDef || inst->classDef->name != "ASTNode") {
        throw std::runtime_error("JC2_to_AST: Expected ASTNode instance");
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
        Token op(TokenType::IDENTIFIER, getProp("op").asString(), line); // Fallback to IDENTIFIER for now
        return std::make_unique<Binary>(std::move(left), op, std::move(right));
    } else if (type == "Unary") {
        auto right = JC2_to_AST(getProp("right"));
        Token op(TokenType::IDENTIFIER, getProp("op").asString(), line);
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
        return std::make_unique<BreakExpr>();
    } else if (type == "ContinueExpr") {
        return std::make_unique<ContinueExpr>();
    } else if (type == "ReturnExpr") {
        return std::make_unique<ReturnExpr>(JC2_to_AST(getProp("value")));
    } else if (type == "IndexAccess") {
        return std::make_unique<IndexAccess>(JC2_to_AST(getProp("object")), getExprList(getProp("indices")));
    } else if (type == "IndexAssign") {
        Token name(TokenType::IDENTIFIER, getProp("name").asString(), line);
        auto objExpr = JC2_to_AST(getProp("objectExpr"));
        auto val = JC2_to_AST(getProp("value"));
        std::vector<std::vector<std::unique_ptr<Expr>>> chain;
        Value chainVal = getProp("indexChain");
        if (chainVal.isObjType(ObjType::LIST)) {
            for (const auto& levelVal : static_cast<ObjList*>(chainVal.asObj())->vec) chain.push_back(getExprList(levelVal));
        }
        if (objExpr) return std::make_unique<IndexAssign>(std::move(objExpr), std::move(chain), std::move(val));
        return std::make_unique<IndexAssign>(name, std::move(chain), std::move(val));
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
        return std::make_unique<ThrowExpr>(JC2_to_AST(getProp("value")));
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
    }

    throw std::runtime_error("JC2_to_AST: Unsupported node type '" + type + "'");
}

} // namespace jc
