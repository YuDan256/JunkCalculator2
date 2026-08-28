#include "Formatter.h"
#include "../frontend/Lexer.h"
#include "../frontend/Parser.h"
#include <algorithm>

namespace jc {

namespace {
    class FormatterVisitor : public ExprVisitor {
    public:
        std::string out;
        int indentLevel = 0;
        bool needsIndent = true;
        const std::vector<Token>& tokens;
        const std::string& source;
        int cursor = 0;
        int pendingNewlines = 0;
        
        bool inElseIf = false;
        bool forceIfBraces = false;
        bool forceNextBlockBraces = false;

        FormatterVisitor(const std::vector<Token>& tokens, const std::string& source) 
            : tokens(tokens), source(source) {}

        bool checkIfBraces(IfExpr* expr) {
            if (!expr) return false;
            auto checkB = [&](Expr* e) {
                if (auto* b = dynamic_cast<Block*>(e)) {
                    return hasTokenBetween(TokenType::LBRACE, b->startPos, b->startPos + 1);
                }
                return false;
            };
            if (checkB(expr->thenBranch.get())) return true;
            if (auto* elif = dynamic_cast<IfExpr*>(expr->elseBranch.get())) {
                if (checkIfBraces(elif)) return true;
            } else {
                if (checkB(expr->elseBranch.get())) return true;
            }
            return false;
        }

        void flushNewlines() {
            if (pendingNewlines > 0) {
                out += "\n";
                if (pendingNewlines > 1) out += "\n";
                needsIndent = true;
                pendingNewlines = 0;
            }
        }

        void emitIndent() {
            if (needsIndent) {
                out += std::string(indentLevel * 4, ' ');
                needsIndent = false;
            }
        }

        void emit(const std::string& s) {
            if (s.empty()) return;
            flushNewlines();
            emitIndent();
            out += s;
        }

        void catchUpTrivia(int targetPos) {
            while (cursor < static_cast<int>(tokens.size()) && tokens[cursor].position < targetPos) {
                const auto& t = tokens[cursor];
                if (t.type == TokenType::COMMENT) {
                    flushNewlines();
                    if (!needsIndent && !out.empty() && out.back() != ' ' && out.back() != '\n') out += " ";
                    emitIndent();
                    out += t.lexeme;
                } else if (t.type == TokenType::NEWLINE) {
                    pendingNewlines++;
                }
                cursor++;
            }
        }

        void advanceCursor(int targetPos) {
            while (cursor < static_cast<int>(tokens.size()) && tokens[cursor].position < targetPos) {
                cursor++;
            }
        }

        void emitOriginal(int startPos, int endPos) {
            catchUpTrivia(startPos);
            emit(source.substr(startPos, endPos - startPos));
            advanceCursor(endPos);
        }

        bool hasTokenBetween(TokenType type, int startPos, int endPos) {
            for (int i = 0; i < static_cast<int>(tokens.size()); i++) {
                if (tokens[i].position >= startPos && tokens[i].position < endPos) {
                    if (tokens[i].type == type) return true;
                }
            }
            return false;
        }
        
        bool isMultiline(int startPos, int endPos) {
            return hasTokenBetween(TokenType::NEWLINE, startPos, endPos);
        }

        std::string getIdentStr(const Token& tok) {
            if (tok.position > 0 && source[tok.position - 1] == '`') {
                return "`" + tok.lexeme + "`";
            }
            return tok.lexeme;
        }

        void visitPattern(Pattern* pat) {
            if (!pat) return;
            catchUpTrivia(pat->startPos);
            if (auto* lp = dynamic_cast<LiteralPattern*>(pat)) {
                if (lp->literal) lp->literal->accept(*this);
            } else if (auto* ep = dynamic_cast<ExprPattern*>(pat)) {
                if (ep->expr) ep->expr->accept(*this);
            } else if (auto* dp = dynamic_cast<DynamicAssertPattern*>(pat)) {
                emit("(");
                if (dp->expr) dp->expr->accept(*this);
                emit(")");
            } else if (auto* vp = dynamic_cast<VariablePattern*>(pat)) {
                if (vp->isConst) emit("const ");
                if (vp->modifier == ScopeModifier::Local) emit("local ");
                else if (vp->modifier == ScopeModifier::Ref) emit("ref ");
                else if (vp->modifier == ScopeModifier::State) emit("state ");
                emit(getIdentStr(vp->name));
                if (vp->typeHint) { emit(": "); vp->typeHint->accept(*this); }
            } else if (auto* rp = dynamic_cast<RestPattern*>(pat)) {
                if (rp->isConst) emit("const ");
                if (rp->modifier == ScopeModifier::Local) emit("local ");
                else if (rp->modifier == ScopeModifier::Ref) emit("ref ");
                else if (rp->modifier == ScopeModifier::State) emit("state ");
                emit("..." + getIdentStr(rp->name));
                if (rp->typeHint) { emit(": "); rp->typeHint->accept(*this); }
            } else if (auto* listp = dynamic_cast<ListPattern*>(pat)) {
                emit("[");
                for (size_t i = 0; i < listp->elements.size(); i++) {
                    visitPattern(listp->elements[i].get());
                    if (i < listp->elements.size() - 1 || listp->rest) emit(", ");
                }
                if (listp->rest) visitPattern(listp->rest.get());
                emit("]");
            } else if (auto* matp = dynamic_cast<MatrixPattern*>(pat)) {
                emit("[");
                for (size_t i = 0; i < matp->rows.size(); i++) {
                    for (size_t j = 0; j < matp->rows[i].size(); j++) {
                        visitPattern(matp->rows[i][j].get());
                        if (j < matp->rows[i].size() - 1) emit(", ");
                    }
                    if (i < matp->rows.size() - 1 || matp->restRow) emit("; ");
                }
                if (matp->restRow) visitPattern(matp->restRow.get());
                emit("]");
            } else if (auto* dictp = dynamic_cast<DictPattern*>(pat)) {
                emit("{");
                for (size_t i = 0; i < dictp->entries.size(); i++) {
                    emit(dictp->entries[i].first + ": ");
                    visitPattern(dictp->entries[i].second.get());
                    if (i < dictp->entries.size() - 1 || dictp->rest) emit(", ");
                }
                if (dictp->rest) visitPattern(dictp->rest.get());
                emit("}");
            } else if (auto* defp = dynamic_cast<DefaultPattern*>(pat)) {
                visitPattern(defp->inner.get());
                emit(" = ");
                if (defp->defaultExpr) defp->defaultExpr->accept(*this);
            }
            advanceCursor(pat->endPos);
        }

        void printLambdaParams(LambdaExpr* expr) {
            bool first = true;
            for (size_t i = 0; i < expr->params.size(); i++) {
                if (!first) emit(", ");
                first = false;
                if (expr->paramIsConst[i]) emit("const ");
                if (expr->paramIsRef[i]) emit("ref ");
                emit(getIdentStr(expr->params[i]));
                if (expr->paramTypes[i]) {
                    emit(": ");
                    expr->paramTypes[i]->accept(*this);
                }
                if (expr->defaultExprs[i]) {
                    emit(" = ");
                    expr->defaultExprs[i]->accept(*this);
                }
            }
            if (!expr->restName.empty()) {
                if (!first) emit(", ");
                first = false;
                emit("..." + expr->restName);
            }
            if (!expr->kwargParams.empty() || !expr->kwargsName.empty()) {
                if (!first) emit("; ");
                else emit(";");
                first = true;
                for (size_t i = 0; i < expr->kwargParams.size(); i++) {
                    if (!first) emit(", ");
                    first = false;
                    if (expr->kwargIsConst[i]) emit("const ");
                    if (expr->kwargIsRef[i]) emit("ref ");
                    emit(getIdentStr(expr->kwargParams[i]));
                    if (expr->kwargTypes[i]) {
                        emit(": ");
                        expr->kwargTypes[i]->accept(*this);
                    }
                    if (expr->kwargDefaultExprs[i]) {
                        emit(" = ");
                        expr->kwargDefaultExprs[i]->accept(*this);
                    }
                }
                if (!expr->kwargsName.empty()) {
                    if (!first) emit(", ");
                    emit("..." + expr->kwargsName);
                }
            }
        }

        void printMethodOrField(const std::string& name, Expr* value, bool isLocal, bool isConst, bool isStatic) {
            if (isStatic) emit("static ");
            if (isConst) emit("const ");
            if (isLocal) emit("local ");
            
            if (auto* lam = dynamic_cast<LambdaExpr*>(value)) {
                emit(name + "(");
                printLambdaParams(lam);
                emit(")");
                if (lam->returnType) {
                    emit(" -> ");
                    lam->returnType->accept(*this);
                }
                emit(" = ");
                if (lam->body) lam->body->accept(*this);
            } else {
                emit(name + " = ");
                if (value) value->accept(*this);
            }
        }

        void printMatrixOrList(const std::vector<std::vector<std::unique_ptr<Expr>>>& elements, int startPos, int endPos, bool isList) {
            catchUpTrivia(startPos);
            emit(isList ? "@[" : "[");
            bool multi = isMultiline(startPos, endPos);
            if (multi) { pendingNewlines++; indentLevel++; }
            
            for (size_t i = 0; i < elements.size(); i++) {
                auto& row = elements[i];
                for (size_t j = 0; j < row.size(); j++) {
                    if (row[j]) row[j]->accept(*this);
                    if (j < row.size() - 1) emit(", ");
                }
                if (i < elements.size() - 1) {
                    emit(";");
                    if (multi) pendingNewlines++;
                    else emit(" ");
                }
            }
            
            if (multi) { indentLevel--; pendingNewlines++; }
            catchUpTrivia(endPos - 1);
            emit("]");
            advanceCursor(endPos);
        }

        void printCompClauses(const std::vector<CompClause>& clauses) {
            for (auto& c : clauses) {
                emit(" for (");
                visitPattern(c.pattern.get());
                emit(" in ");
                if (c.iterable) c.iterable->accept(*this);
                emit(")");
                for (auto& cond : c.conditions) {
                    emit(" if (");
                    if (cond) cond->accept(*this);
                    emit(")");
                }
            }
        }

        void visitBinary(Binary* expr) override {
            if (expr->left) expr->left->accept(*this);
            catchUpTrivia(expr->op.position);
            
            bool hasNewline = isMultiline(expr->left->endPos, expr->right->startPos);
            bool isLong = (expr->endPos - expr->startPos) > 80;
            bool isLogical = (expr->op.type == TokenType::AND_AND || expr->op.type == TokenType::OR_OR);
            
            if (expr->op.type == TokenType::PIPE) {
                if (hasNewline || isLong) {
                    emit(" |>");
                    pendingNewlines++;
                    indentLevel++;
                    if (expr->right) expr->right->accept(*this);
                    indentLevel--;
                } else {
                    emit(" |> ");
                    if (expr->right) expr->right->accept(*this);
                }
            } else {
                if (hasNewline || (isLong && isLogical)) {
                    emit(" " + expr->op.lexeme);
                    pendingNewlines++;
                    indentLevel++;
                    if (expr->right) expr->right->accept(*this);
                    indentLevel--;
                } else {
                    emit(" " + expr->op.lexeme + " ");
                    if (expr->right) expr->right->accept(*this);
                }
            }
            advanceCursor(expr->endPos);
        }

        void visitUnary(Unary* expr) override {
            catchUpTrivia(expr->startPos);
            emit(expr->op.lexeme);
            if (expr->right) expr->right->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitLiteral(Literal* expr) override { emitOriginal(expr->startPos, expr->endPos); }
        void visitVariable(Variable* expr) override { emitOriginal(expr->startPos, expr->endPos); }
        void visitContextKeywordExpr(ContextKeywordExpr* expr) override { emitOriginal(expr->startPos, expr->endPos); }
        void visitSuperExpr(SuperExpr* expr) override { emitOriginal(expr->startPos, expr->endPos); }
        void visitSelfExpr(SelfExpr* expr) override { emitOriginal(expr->startPos, expr->endPos); }
        void visitBreakExpr(BreakExpr* expr) override { emitOriginal(expr->startPos, expr->endPos); }
        void visitContinueExpr(ContinueExpr* expr) override { emitOriginal(expr->startPos, expr->endPos); }
        void visitFStringExpr(FStringExpr* expr) override { emitOriginal(expr->startPos, expr->endPos); }

        void visitAssign(Assign* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->isConst) emit("const ");
            if (expr->isLocal) emit("local ");
            if (expr->isRef) emit("ref ");
            if (expr->isState) emit("state ");
            
            if (auto* lam = dynamic_cast<LambdaExpr*>(expr->value.get())) {
                emit(getIdentStr(expr->name) + "(");
                printLambdaParams(lam);
                emit(")");
                if (lam->returnType) {
                    emit(" -> ");
                    lam->returnType->accept(*this);
                }
                emit(" = ");
                if (lam->body) lam->body->accept(*this);
            } else {
                emit(getIdentStr(expr->name));
                if (expr->typeHint) {
                    emit(": ");
                    expr->typeHint->accept(*this);
                }
                emit(" = ");
                if (expr->value) expr->value->accept(*this);
            }
            advanceCursor(expr->endPos);
        }

        void visitCall(Call* expr) override {
            catchUpTrivia(expr->startPos);
            emit(expr->callee.lexeme + "(");
            for (size_t i = 0; i < expr->arguments.size(); i++) {
                if (expr->arguments[i]) expr->arguments[i]->accept(*this);
                if (i < expr->arguments.size() - 1) emit(", ");
            }
            emit(")");
            advanceCursor(expr->endPos);
        }

        void visitMatrixNode(MatrixNode* expr) override { printMatrixOrList(expr->elements, expr->startPos, expr->endPos, false); }
        void visitListNode(ListNode* expr) override { printMatrixOrList(expr->elements, expr->startPos, expr->endPos, true); }

        void visitBlock(Block* expr) override {
            catchUpTrivia(expr->startPos);
            bool hasBraces = false;
            for (int i = 0; i < static_cast<int>(tokens.size()); i++) {
                if (tokens[i].position == expr->startPos) {
                    if (tokens[i].type == TokenType::LBRACE) hasBraces = true;
                    break;
                }
            }
            
            bool useBraces = hasBraces || forceNextBlockBraces;
            forceNextBlockBraces = false;

            if (useBraces) {
                if (expr->statements.empty()) {
                    emit("{}");
                } else {
                    emit("{");
                    pendingNewlines++;
                    indentLevel++;
                    for (auto& stmt : expr->statements) {
                        if (stmt) {
                            stmt->accept(*this);
                            pendingNewlines++;
                        }
                    }
                    indentLevel--;
                    if (hasBraces) {
                        catchUpTrivia(expr->endPos - 1);
                    }
                    emit("}");
                }
            } else {
                if (!expr->statements.empty() && expr->statements[0]) {
                    expr->statements[0]->accept(*this);
                }
            }
            advanceCursor(expr->endPos);
        }

        void visitIfExpr(IfExpr* expr) override {
            bool isTopLevel = !inElseIf;
            bool prevInElseIf = inElseIf;
            inElseIf = false;

            if (isTopLevel) {
                forceIfBraces = checkIfBraces(expr);
            }

            catchUpTrivia(expr->startPos);
            emit("if (");
            if (expr->condition) expr->condition->accept(*this);
            emit(")");
            
            bool hasBraces = false;
            if (auto* b = dynamic_cast<Block*>(expr->thenBranch.get())) {
                hasBraces = hasTokenBetween(TokenType::LBRACE, b->startPos, b->startPos + 1);
            }
            bool useBraces = hasBraces || forceIfBraces;

            if (useBraces) emit(" ");
            else { pendingNewlines++; indentLevel++; }
            
            forceNextBlockBraces = useBraces && !hasBraces;
            if (expr->thenBranch) expr->thenBranch->accept(*this);
            forceNextBlockBraces = false;
            
            if (!useBraces) indentLevel--;
            
            if (expr->elseBranch) {
                catchUpTrivia(expr->elseBranch->startPos - 4);
                if (useBraces) emit(" else ");
                else { pendingNewlines++; emit("else "); }
                
                bool isElseIf = dynamic_cast<IfExpr*>(expr->elseBranch.get()) != nullptr;
                
                bool elseHasBraces = false;
                if (auto* b = dynamic_cast<Block*>(expr->elseBranch.get())) {
                    elseHasBraces = hasTokenBetween(TokenType::LBRACE, b->startPos, b->startPos + 1);
                }
                bool useElseBraces = elseHasBraces || forceIfBraces;

                if (!isElseIf && !useElseBraces) {
                    pendingNewlines++; indentLevel++;
                }
                
                if (isElseIf) {
                    inElseIf = true;
                } else {
                    forceNextBlockBraces = useElseBraces && !elseHasBraces;
                }

                expr->elseBranch->accept(*this);
                forceNextBlockBraces = false;
                
                if (!isElseIf && !useElseBraces) indentLevel--;
            }
            advanceCursor(expr->endPos);
            inElseIf = prevInElseIf;
        }

        void visitWhileExpr(WhileExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("while (");
            if (expr->condition) expr->condition->accept(*this);
            emit(")");
            
            bool hasBraces = false;
            if (auto* b = dynamic_cast<Block*>(expr->body.get())) {
                hasBraces = hasTokenBetween(TokenType::LBRACE, b->startPos, b->startPos + 1);
            }
            if (hasBraces) emit(" ");
            else { pendingNewlines++; indentLevel++; }
            
            if (expr->body) expr->body->accept(*this);
            
            if (!hasBraces) indentLevel--;
            advanceCursor(expr->endPos);
        }

        void visitForExpr(ForExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("for (");
            if (expr->initializer) expr->initializer->accept(*this);
            emit("; ");
            if (expr->condition) expr->condition->accept(*this);
            emit("; ");
            if (expr->update) expr->update->accept(*this);
            emit(")");
            
            bool hasBraces = false;
            if (auto* b = dynamic_cast<Block*>(expr->body.get())) {
                hasBraces = hasTokenBetween(TokenType::LBRACE, b->startPos, b->startPos + 1);
            }
            if (hasBraces) emit(" ");
            else { pendingNewlines++; indentLevel++; }
            
            if (expr->body) expr->body->accept(*this);
            
            if (!hasBraces) indentLevel--;
            advanceCursor(expr->endPos);
        }

        void visitReturnExpr(ReturnExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("return");
            if (expr->value) {
                emit(" ");
                expr->value->accept(*this);
            }
            advanceCursor(expr->endPos);
        }

        void visitIndexAccess(IndexAccess* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->object) expr->object->accept(*this);
            emit("[");
            for (size_t i = 0; i < expr->indices.size(); i++) {
                if (expr->indices[i]) expr->indices[i]->accept(*this);
                if (i < expr->indices.size() - 1) emit(", ");
            }
            emit("]");
            advanceCursor(expr->endPos);
        }

        void visitIndexAssign(IndexAssign* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->objectExpr) expr->objectExpr->accept(*this);
            else emit(getIdentStr(expr->name));
            
            for (auto& chain : expr->indexChain) {
                emit("[");
                for (size_t i = 0; i < chain.size(); i++) {
                    if (chain[i]) chain[i]->accept(*this);
                    if (i < chain.size() - 1) emit(", ");
                }
                emit("]");
            }
            
            if (auto* lam = dynamic_cast<LambdaExpr*>(expr->value.get())) {
                emit("(");
                printLambdaParams(lam);
                emit(")");
                if (lam->returnType) {
                    emit(" -> ");
                    lam->returnType->accept(*this);
                }
                emit(" = ");
                if (lam->body) lam->body->accept(*this);
            } else {
                if (expr->typeHint) {
                    emit(": ");
                    expr->typeHint->accept(*this);
                }
                emit(" = ");
                if (expr->value) expr->value->accept(*this);
            }
            advanceCursor(expr->endPos);
        }

        void visitLocalDecl(LocalDecl* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->isConst) emit("const ");
            emit("local ");
            emit(getIdentStr(expr->name));
            if (expr->typeHint) { emit(": "); expr->typeHint->accept(*this); }
            advanceCursor(expr->endPos);
        }

        void visitRefDecl(RefDecl* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->isConst) emit("const ");
            emit("ref ");
            emit(getIdentStr(expr->name));
            if (expr->typeHint) { emit(": "); expr->typeHint->accept(*this); }
            advanceCursor(expr->endPos);
        }

        void visitStateDecl(StateDecl* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->isConst) emit("const ");
            emit("state ");
            emit(getIdentStr(expr->name));
            if (expr->typeHint) { emit(": "); expr->typeHint->accept(*this); }
            advanceCursor(expr->endPos);
        }

        void visitConstDecl(ConstDecl* expr) override {
            catchUpTrivia(expr->startPos);
            emit("const " + getIdentStr(expr->name));
            advanceCursor(expr->endPos);
        }

        void visitDeleteExpr(DeleteExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("delete ");
            for (size_t i = 0; i < expr->names.size(); i++) {
                emit(getIdentStr(expr->names[i]));
                if (i < expr->names.size() - 1) emit(", ");
            }
            advanceCursor(expr->endPos);
        }

        void visitCompoundAssign(CompoundAssign* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->isLocal) emit("local ");
            if (expr->isRef) emit("ref ");
            if (expr->isState) emit("state ");
            if (expr->target) expr->target->accept(*this);
            
            std::string opStr;
            switch (expr->op) {
                case TokenType::PLUS: opStr = "+="; break;
                case TokenType::MINUS: opStr = "-="; break;
                case TokenType::STAR: opStr = "*="; break;
                case TokenType::SLASH: opStr = "/="; break;
                case TokenType::TILDE_SLASH: opStr = "~/="; break;
                case TokenType::PERCENT: opStr = "%="; break;
                case TokenType::CARET: opStr = "^="; break;
                case TokenType::BACKSLASH: opStr = "\\="; break;
                case TokenType::BIT_AND: opStr = "&="; break;
                case TokenType::BIT_OR: opStr = "|="; break;
                case TokenType::BIT_XOR: opStr = "^^="; break;
                case TokenType::SHIFT_LEFT: opStr = "<<="; break;
                case TokenType::SHIFT_RIGHT: opStr = ">>="; break;
                default: opStr = "+="; break;
            }
            emit(" " + opStr + " ");
            if (expr->value) expr->value->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitLambdaExpr(LambdaExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("(");
            printLambdaParams(expr);
            emit(") => ");
            if (expr->body) expr->body->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitInvokeExpr(InvokeExpr* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->callee) expr->callee->accept(*this);
            emit("(");
            for (size_t i = 0; i < expr->arguments.size(); i++) {
                if (expr->arguments[i]) expr->arguments[i]->accept(*this);
                if (i < expr->arguments.size() - 1) emit(", ");
            }
            emit(")");
            advanceCursor(expr->endPos);
        }

        void visitForInExpr(ForInExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("for (");
            if (expr->isConst) emit("const ");
            if (expr->isLocal) emit("local ");
            visitPattern(expr->pattern.get());
            emit(" in ");
            if (expr->iterable) expr->iterable->accept(*this);
            emit(")");
            
            bool hasBraces = false;
            if (auto* b = dynamic_cast<Block*>(expr->body.get())) {
                hasBraces = hasTokenBetween(TokenType::LBRACE, b->startPos, b->startPos + 1);
            }
            if (hasBraces) emit(" ");
            else { pendingNewlines++; indentLevel++; }
            
            if (expr->body) expr->body->accept(*this);
            
            if (!hasBraces) indentLevel--;
            advanceCursor(expr->endPos);
        }

        void visitThrowExpr(ThrowExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("throw ");
            if (expr->value) expr->value->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitTryCatchExpr(TryCatchExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("try ");
            
            bool tryHasBraces = false;
            if (auto* b = dynamic_cast<Block*>(expr->tryBody.get())) {
                tryHasBraces = hasTokenBetween(TokenType::LBRACE, b->startPos, b->startPos + 1);
            }
            if (expr->tryBody) expr->tryBody->accept(*this);
            
            if (expr->catchPattern) {
                catchUpTrivia(expr->catchPattern->startPos - 6);
                if (tryHasBraces) emit(" catch (");
                else { pendingNewlines++; emit("catch ("); }
                
                visitPattern(expr->catchPattern.get());
                emit(") ");
                if (expr->catchBody) expr->catchBody->accept(*this);
            }
            advanceCursor(expr->endPos);
        }

        void visitImportExpr(ImportExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("import ");
            if (expr->path) expr->path->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitSwitchExpr(SwitchExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("switch (");
            if (expr->subject) expr->subject->accept(*this);
            emit(") {");
            pendingNewlines++;
            
            for (auto& c : expr->cases) {
                indentLevel++;
                emit("case ");
                for (size_t i = 0; i < c.first.size(); i++) {
                    c.first[i]->accept(*this);
                    if (i < c.first.size() - 1) emit(", ");
                }
                emit(":");
                pendingNewlines++;
                
                indentLevel++;
                if (c.second) c.second->accept(*this);
                indentLevel -= 2;
            }
            
            if (expr->defaultBody) {
                indentLevel++;
                emit("default:");
                pendingNewlines++;
                indentLevel++;
                expr->defaultBody->accept(*this);
                indentLevel -= 2;
            }
            
            catchUpTrivia(expr->endPos - 1);
            emit("}");
            advanceCursor(expr->endPos);
        }

        void visitClassDefExpr(ClassDefExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("class");
            if (!expr->name.lexeme.empty()) emit(" " + getIdentStr(expr->name));
            if (expr->superClassExpr) {
                emit(" extends ");
                expr->superClassExpr->accept(*this);
            }
            emit(" {");
            pendingNewlines++;
            indentLevel++;
            
            for (auto& p : expr->staticProperties) {
                printMethodOrField(getIdentStr(p.name), p.value.get(), p.isLocal, p.isConst, true);
                pendingNewlines++;
            }
            for (auto& p : expr->instanceProperties) {
                printMethodOrField(getIdentStr(p.name), p.value.get(), p.isLocal, p.isConst, false);
                pendingNewlines++;
            }
            
            indentLevel--;
            catchUpTrivia(expr->endPos - 1);
            emit("}");
            advanceCursor(expr->endPos);
        }

        void visitNamespaceDecl(NamespaceDecl* expr) override {
            catchUpTrivia(expr->startPos);
            emit("namespace");
            if (!expr->name.lexeme.empty()) emit(" " + getIdentStr(expr->name));
            emit(" ");
            if (expr->body) expr->body->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitEnumDefExpr(EnumDefExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("enum");
            if (!expr->name.lexeme.empty()) emit(" " + getIdentStr(expr->name));
            emit(" {");
            pendingNewlines++;
            indentLevel++;
            
            for (size_t i = 0; i < expr->members.size(); i++) {
                emit(getIdentStr(expr->members[i].first));
                if (expr->members[i].second) {
                    emit(" = ");
                    expr->members[i].second->accept(*this);
                }
                if (i < expr->members.size() - 1) emit(",");
                pendingNewlines++;
            }
            
            indentLevel--;
            catchUpTrivia(expr->endPos - 1);
            emit("}");
            advanceCursor(expr->endPos);
        }

        void visitDotAccess(DotAccess* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->object) expr->object->accept(*this);
            emit("." + getIdentStr(expr->field));
            advanceCursor(expr->endPos);
        }

        void visitDotAssign(DotAssign* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->object) expr->object->accept(*this);
            emit("." + getIdentStr(expr->field));
            
            if (auto* lam = dynamic_cast<LambdaExpr*>(expr->value.get())) {
                emit("(");
                printLambdaParams(lam);
                emit(")");
                if (lam->returnType) {
                    emit(" -> ");
                    lam->returnType->accept(*this);
                }
                emit(" = ");
                if (lam->body) lam->body->accept(*this);
            } else {
                if (expr->typeHint) {
                    emit(": ");
                    expr->typeHint->accept(*this);
                }
                emit(" = ");
                if (expr->value) expr->value->accept(*this);
            }
            advanceCursor(expr->endPos);
        }

        void visitMethodCallExpr(MethodCallExpr* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->object) expr->object->accept(*this);
            emit("." + getIdentStr(expr->method) + "(");
            for (size_t i = 0; i < expr->arguments.size(); i++) {
                if (expr->arguments[i]) expr->arguments[i]->accept(*this);
                if (i < expr->arguments.size() - 1) emit(", ");
            }
            emit(")");
            advanceCursor(expr->endPos);
        }

        void visitDestructAssign(DestructAssign* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->isConst) emit("const ");
            if (expr->isLocal) emit("local ");
            if (expr->isRef) emit("ref ");
            if (expr->isState) emit("state ");
            visitPattern(expr->pattern.get());
            emit(" = ");
            if (expr->value) expr->value->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitMatrixCompExpr(MatrixCompExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("[");
            if (expr->valueExpr) expr->valueExpr->accept(*this);
            printCompClauses(expr->clauses);
            emit("]");
            advanceCursor(expr->endPos);
        }

        void visitListCompExpr(ListCompExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("@[");
            if (expr->valueExpr) expr->valueExpr->accept(*this);
            printCompClauses(expr->clauses);
            emit("]");
            advanceCursor(expr->endPos);
        }

        void visitSetCompExpr(SetCompExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("@{");
            if (expr->valueExpr) expr->valueExpr->accept(*this);
            printCompClauses(expr->clauses);
            emit("}");
            advanceCursor(expr->endPos);
        }

        void visitDictCompExpr(DictCompExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("{");
            if (expr->keyExpr) expr->keyExpr->accept(*this);
            emit(": ");
            if (expr->valueExpr) expr->valueExpr->accept(*this);
            printCompClauses(expr->clauses);
            emit("}");
            advanceCursor(expr->endPos);
        }

        void visitDictLiteral(DictLiteral* expr) override {
            catchUpTrivia(expr->startPos);
            emit("{");
            bool multi = isMultiline(expr->startPos, expr->endPos);
            if (multi) { pendingNewlines++; indentLevel++; }
            
            for (size_t i = 0; i < expr->entries.size(); i++) {
                if (auto* lit = dynamic_cast<Literal*>(expr->entries[i].first.get())) {
                    if (auto* var = dynamic_cast<Variable*>(expr->entries[i].second.get())) {
                        if (lit->value == var->name.lexeme) {
                            emit(getIdentStr(var->name));
                            goto next_entry;
                        }
                    }
                }
                if (expr->entries[i].first) expr->entries[i].first->accept(*this);
                emit(": ");
                if (expr->entries[i].second) expr->entries[i].second->accept(*this);
                
            next_entry:
                if (i < expr->entries.size() - 1) {
                    emit(",");
                    if (multi) pendingNewlines++;
                    else emit(" ");
                }
            }
            
            if (multi) { indentLevel--; pendingNewlines++; }
            catchUpTrivia(expr->endPos - 1);
            emit("}");
            advanceCursor(expr->endPos);
        }

        void visitSetLiteral(SetLiteral* expr) override {
            catchUpTrivia(expr->startPos);
            emit("@{");
            bool multi = isMultiline(expr->startPos, expr->endPos);
            if (multi) { pendingNewlines++; indentLevel++; }
            
            for (size_t i = 0; i < expr->elements.size(); i++) {
                if (expr->elements[i]) expr->elements[i]->accept(*this);
                if (i < expr->elements.size() - 1) {
                    emit(",");
                    if (multi) pendingNewlines++;
                    else emit(" ");
                }
            }
            
            if (multi) { indentLevel--; pendingNewlines++; }
            catchUpTrivia(expr->endPos - 1);
            emit("}");
            advanceCursor(expr->endPos);
        }

        void visitSliceExpr(SliceExpr* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->start) expr->start->accept(*this);
            emit(":");
            if (expr->end) expr->end->accept(*this);
            if (expr->step) {
                emit(":");
                expr->step->accept(*this);
            }
            advanceCursor(expr->endPos);
        }

        void visitSequenceExpr(SequenceExpr* expr) override {
            catchUpTrivia(expr->startPos);
            for (size_t i = 0; i < expr->expressions.size(); i++) {
                if (expr->expressions[i]) expr->expressions[i]->accept(*this);
                if (i < expr->expressions.size() - 1) emit(", ");
            }
            advanceCursor(expr->endPos);
        }

        void visitMatchExpr(MatchExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("match (");
            if (expr->subject) expr->subject->accept(*this);
            emit(") {");
            pendingNewlines++;
            indentLevel++;
            
            for (size_t i = 0; i < expr->branches.size(); i++) {
                auto& b = expr->branches[i];
                for (size_t j = 0; j < b.patterns.size(); j++) {
                    visitPattern(b.patterns[j].get());
                    if (j < b.patterns.size() - 1) emit(", ");
                }
                if (b.guard) {
                    emit(" if (");
                    b.guard->accept(*this);
                    emit(")");
                }
                emit(" => ");
                
                bool hasBraces = false;
                if (auto* blk = dynamic_cast<Block*>(b.body.get())) {
                    hasBraces = hasTokenBetween(TokenType::LBRACE, blk->startPos, blk->startPos + 1);
                }
                
                if (hasBraces) {
                    if (b.body) b.body->accept(*this);
                } else {
                    if (b.body) b.body->accept(*this);
                }
                
                if (i < expr->branches.size() - 1) {
                    emit(",");
                    pendingNewlines++;
                }
            }
            
            indentLevel--;
            pendingNewlines++;
            catchUpTrivia(expr->endPos - 1);
            emit("}");
            advanceCursor(expr->endPos);
        }

        void visitGroupingExpr(GroupingExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("(");
            if (expr->expression) expr->expression->accept(*this);
            catchUpTrivia(expr->endPos - 1);
            emit(")");
            advanceCursor(expr->endPos);
        }

        void visitMacroDefExpr(MacroDefExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit(expr->isTokenMacro ? "syntax " : "macro ");
            emit(getIdentStr(expr->name) + "(");
            for (size_t i = 0; i < expr->params.size(); i++) {
                emit(getIdentStr(expr->params[i]));
                if (i < expr->params.size() - 1 || !expr->restName.empty()) emit(", ");
            }
            if (!expr->restName.empty()) emit("..." + expr->restName);
            emit(") = ");
            if (expr->body) expr->body->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitMacroCallExpr(MacroCallExpr* expr) override {
            emitOriginal(expr->startPos, expr->endPos);
        }

        void visitQuoteExpr(QuoteExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("quote ");
            if (expr->body) expr->body->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitUnquoteExpr(UnquoteExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("$");
            if (expr->expr) expr->expr->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitExprAssign(ExprAssign* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->isConst) emit("const ");
            if (expr->isLocal) emit("local ");
            if (expr->isRef) emit("ref ");
            if (expr->isState) emit("state ");
            if (expr->target) expr->target->accept(*this);
            emit(" = ");
            if (expr->value) expr->value->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitDeferExpr(DeferExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("defer ");
            if (expr->body) expr->body->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitKeywordArgExpr(KeywordArgExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit(getIdentStr(expr->name) + " = ");
            if (expr->value) expr->value->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitSpreadExpr(SpreadExpr* expr) override {
            catchUpTrivia(expr->startPos);
            emit("...");
            if (expr->value) expr->value->accept(*this);
            advanceCursor(expr->endPos);
        }

        void visitTypeAssertExpr(TypeAssertExpr* expr) override {
            catchUpTrivia(expr->startPos);
            if (expr->value) expr->value->accept(*this);
            emit(" as ");
            if (expr->typeHint) expr->typeHint->accept(*this);
            advanceCursor(expr->endPos);
        }
    };
}

    std::string Formatter::format(const std::string& source) {
        Lexer lexer(source);
        lexer.keepComments = true;
        auto tokens = lexer.tokenize();

        try {
            Lexer parserLexer(source);
            auto parserTokens = parserLexer.tokenize();
            Parser parser(parserTokens);
            parser.disableMacroExpansion = true;
            auto ast = parser.parse();
            
            FormatterVisitor visitor(tokens, source);
            ast->accept(visitor);
            visitor.catchUpTrivia(static_cast<int>(source.length()));
            visitor.flushNewlines();
            
            std::string out = visitor.out;
            if (!out.empty() && out.back() != '\n') out += "\n";
            return out;
        } catch (...) {
            return source;
        }
    }

} // namespace jc
