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

    void visitBlock(Block* expr) override {
        ObjList* stmts = GcHeap::get().allocate<ObjList>();
        GcObjGuard guard(stmts);
        for (auto& stmt : expr->statements) {
            stmt->accept(*this);
            stmts->vec.push_back(result);
        }
        result = makeASTNode("Block", 0, {
            {"statements", Value(stmts)}
        });
    }

    // TODO: Implement other visitor methods as needed for macros
    void visitCall(Call*) override { throw std::runtime_error("AST_to_JC2: Call not implemented yet"); }
    void visitMatrixNode(MatrixNode*) override { throw std::runtime_error("AST_to_JC2: MatrixNode not implemented yet"); }
    void visitIfExpr(IfExpr*) override { throw std::runtime_error("AST_to_JC2: IfExpr not implemented yet"); }
    void visitWhileExpr(WhileExpr*) override { throw std::runtime_error("AST_to_JC2: WhileExpr not implemented yet"); }
    void visitForExpr(ForExpr*) override { throw std::runtime_error("AST_to_JC2: ForExpr not implemented yet"); }
    void visitBreakExpr(BreakExpr*) override { throw std::runtime_error("AST_to_JC2: BreakExpr not implemented yet"); }
    void visitContinueExpr(ContinueExpr*) override { throw std::runtime_error("AST_to_JC2: ContinueExpr not implemented yet"); }
    void visitReturnExpr(ReturnExpr*) override { throw std::runtime_error("AST_to_JC2: ReturnExpr not implemented yet"); }
    void visitIndexAccess(IndexAccess*) override { throw std::runtime_error("AST_to_JC2: IndexAccess not implemented yet"); }
    void visitIndexAssign(IndexAssign*) override { throw std::runtime_error("AST_to_JC2: IndexAssign not implemented yet"); }
    void visitLocalDecl(LocalDecl*) override { throw std::runtime_error("AST_to_JC2: LocalDecl not implemented yet"); }
    void visitRefDecl(RefDecl*) override { throw std::runtime_error("AST_to_JC2: RefDecl not implemented yet"); }
    void visitStateDecl(StateDecl*) override { throw std::runtime_error("AST_to_JC2: StateDecl not implemented yet"); }
    void visitConstDecl(ConstDecl*) override { throw std::runtime_error("AST_to_JC2: ConstDecl not implemented yet"); }
    void visitDeleteExpr(DeleteExpr*) override { throw std::runtime_error("AST_to_JC2: DeleteExpr not implemented yet"); }
    void visitCompoundAssign(CompoundAssign*) override { throw std::runtime_error("AST_to_JC2: CompoundAssign not implemented yet"); }
    void visitLambdaExpr(LambdaExpr*) override { throw std::runtime_error("AST_to_JC2: LambdaExpr not implemented yet"); }
    void visitInvokeExpr(InvokeExpr*) override { throw std::runtime_error("AST_to_JC2: InvokeExpr not implemented yet"); }
    void visitForInExpr(ForInExpr*) override { throw std::runtime_error("AST_to_JC2: ForInExpr not implemented yet"); }
    void visitThrowExpr(ThrowExpr*) override { throw std::runtime_error("AST_to_JC2: ThrowExpr not implemented yet"); }
    void visitTryCatchExpr(TryCatchExpr*) override { throw std::runtime_error("AST_to_JC2: TryCatchExpr not implemented yet"); }
    void visitImportExpr(ImportExpr*) override { throw std::runtime_error("AST_to_JC2: ImportExpr not implemented yet"); }
    void visitSwitchExpr(SwitchExpr*) override { throw std::runtime_error("AST_to_JC2: SwitchExpr not implemented yet"); }
    void visitClassDefExpr(ClassDefExpr*) override { throw std::runtime_error("AST_to_JC2: ClassDefExpr not implemented yet"); }
    void visitNamespaceDecl(NamespaceDecl*) override { throw std::runtime_error("AST_to_JC2: NamespaceDecl not implemented yet"); }
    void visitDotAccess(DotAccess*) override { throw std::runtime_error("AST_to_JC2: DotAccess not implemented yet"); }
    void visitDotAssign(DotAssign*) override { throw std::runtime_error("AST_to_JC2: DotAssign not implemented yet"); }
    void visitMethodCallExpr(MethodCallExpr*) override { throw std::runtime_error("AST_to_JC2: MethodCallExpr not implemented yet"); }
    void visitSuperExpr(SuperExpr*) override { throw std::runtime_error("AST_to_JC2: SuperExpr not implemented yet"); }
    void visitSelfExpr(SelfExpr*) override { throw std::runtime_error("AST_to_JC2: SelfExpr not implemented yet"); }
    void visitDestructAssign(DestructAssign*) override { throw std::runtime_error("AST_to_JC2: DestructAssign not implemented yet"); }
    void visitFStringExpr(FStringExpr*) override { throw std::runtime_error("AST_to_JC2: FStringExpr not implemented yet"); }
    void visitListCompExpr(ListCompExpr*) override { throw std::runtime_error("AST_to_JC2: ListCompExpr not implemented yet"); }
    void visitDictLiteral(DictLiteral*) override { throw std::runtime_error("AST_to_JC2: DictLiteral not implemented yet"); }
    void visitSetLiteral(SetLiteral*) override { throw std::runtime_error("AST_to_JC2: SetLiteral not implemented yet"); }
    void visitSliceExpr(SliceExpr*) override { throw std::runtime_error("AST_to_JC2: SliceExpr not implemented yet"); }
    void visitSequenceExpr(SequenceExpr*) override { throw std::runtime_error("AST_to_JC2: SequenceExpr not implemented yet"); }
    void visitMatchExpr(MatchExpr*) override { throw std::runtime_error("AST_to_JC2: MatchExpr not implemented yet"); }
    void visitGroupingExpr(GroupingExpr*) override { throw std::runtime_error("AST_to_JC2: GroupingExpr not implemented yet"); }
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
        std::vector<std::unique_ptr<Expr>> stmts;
        Value stmtsVal = getProp("statements");
        if (stmtsVal.isObjType(ObjType::LIST)) {
            auto list = static_cast<ObjList*>(stmtsVal.asObj());
            for (const auto& v : list->vec) {
                stmts.push_back(JC2_to_AST(v));
            }
        }
        return std::make_unique<Block>(std::move(stmts));
    }

    throw std::runtime_error("JC2_to_AST: Unsupported node type '" + type + "'");
}

} // namespace jc
