#include "SemanticAnalyzer.h"
#include "../../vm/HelpRouter.h"
#include <unordered_set>
#include <sstream>

namespace jc {
namespace lsp {

    static bool isPosInRange(const Position& pos, const Range& range) {
        if (pos.line < range.start.line || pos.line > range.end.line) return false;
        if (pos.line == range.start.line && pos.character < range.start.character) return false;
        if (pos.line == range.end.line && pos.character >= range.end.character) return false;
        return true;
    }

    static bool isTypeCompatible(const std::string& inferred, const std::string& hint) {
        if (inferred.empty() || hint.empty() || hint == "any") return true;
        if (inferred == hint) return true;

        auto trim = [](std::string s) {
            size_t start = s.find_first_not_of(" \t");
            size_t end = s.find_last_not_of(" \t");
            return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
        };

        std::stringstream ss(hint);
        std::string part;
        while (std::getline(ss, part, '|')) {
            std::string t = trim(part);
            if (t == inferred) return true;
            if (t == "matrix" && (inferred == "realmatrix" || inferred == "complexmatrix" || inferred == "symmatrix")) return true;
            if (t == "real" && (inferred == "int" || inferred == "double" || inferred == "fraction" || inferred == "bool")) return true;
            if (t == "number" && (inferred == "int" || inferred == "double" || inferred == "fraction" || inferred == "complex" || inferred == "bool")) return true;
            if (t == "exact" && (inferred == "int" || inferred == "fraction" || inferred == "bool" || inferred == "symbolic")) return true;
        }
        return false;
    }

    SemanticAnalyzer::SemanticAnalyzer(Document* doc, const std::vector<Token>& tokens)
        : doc(doc), tokens(tokens) {
        globalScope = std::make_unique<Scope>();
        globalScope->kind = ScopeKind::Global;
        
        Range globalRange;
        globalRange.start = Position{0, 0};
        globalRange.end = Position{999999, 0}; // 足够大
        globalScope->scopeRange = globalRange;
        
        currentScope = globalScope.get();
    }

    void SemanticAnalyzer::analyze(Expr* root) {
        if (root) {
            root->accept(*this);
        }
    }

    void SemanticAnalyzer::enterScope(const Range& range, ScopeKind kind) {
        auto newScope = std::make_unique<Scope>();
        newScope->kind = kind;
        newScope->parent = currentScope;
        newScope->scopeRange = range;
        Scope* ptr = newScope.get();
        currentScope->children.push_back(std::move(newScope));
        currentScope = ptr;
    }

    void SemanticAnalyzer::leaveScope() {
        if (currentScope->parent) {
            currentScope = currentScope->parent;
        }
    }

    void SemanticAnalyzer::declareSymbol(const std::string& name, SymbolKind kind, int startPos, int endPos, const std::string& typeHint, bool isLocal, const std::string& inferredType) {
        if (!currentScope) return;
        
        Scope* targetScope = currentScope;
        if (!isLocal) {
            // Auto-local: 提升到最近的函数、命名空间或全局作用域
            while (targetScope && targetScope->kind == ScopeKind::Block) {
                targetScope = targetScope->parent;
            }
            if (!targetScope) targetScope = currentScope;
        }
        
        auto sym = std::make_shared<Symbol>();
        sym->name = name;
        sym->kind = kind;
        sym->definitionRange.start = doc->offsetToPosition(startPos);
        sym->definitionRange.end = doc->offsetToPosition(endPos);
        sym->typeHint = typeHint;
        sym->inferredType = inferredType;
        sym->docstring = extractDocstring(startPos);
        
        targetScope->symbols[name] = sym;
        references.push_back({sym->definitionRange, sym});

        // 检查是否覆盖了内置变量/类型/函数，给出警告
        if (name != "_" && name.find("<") != 0) {
            bool isBuiltin = false;
            jc::HelpRouter::init();
            if (jc::HelpRouter::helpAst.isObject() && jc::HelpRouter::helpAst.has("functions")) {
                if (jc::HelpRouter::helpAst["functions"].has(name)) isBuiltin = true;
            }
            if (!isBuiltin) {
                static const std::unordered_set<std::string> builtins = {
                    "int", "double", "string", "bool", "list", "dict", "set", "fraction", "complex",
                    "realmatrix", "complexmatrix", "symmatrix", "function", "class_type", "instance",
                    "namespace_type", "type", "slice", "PI", "E", "i", "I", "ANS"
                };
                if (builtins.count(name)) isBuiltin = true;
            }
            if (isBuiltin) {
                diagnostics.push_back({"Warning: Shadowing built-in symbol '" + name + "'.", startPos, endPos, 2});
            }
        }
    }

    void SemanticAnalyzer::declarePattern(Pattern* pat, bool isLocal, bool isConst) {
        if (!pat) return;
        if (auto* vp = dynamic_cast<VariablePattern*>(pat)) {
            if (vp->name.lexeme != "_") {
                bool effLocal = isLocal || vp->modifier == ScopeModifier::Local || vp->modifier == ScopeModifier::State;
                std::string hint = exprToString(vp->typeHint.get());
                declareSymbol(vp->name.lexeme, SymbolKind::Variable, vp->name.position, vp->name.position + static_cast<int>(vp->name.lexeme.length()), hint, effLocal);
            }
        } else if (auto* rp = dynamic_cast<RestPattern*>(pat)) {
            if (rp->name.lexeme != "_") {
                bool effLocal = isLocal || rp->modifier == ScopeModifier::Local || rp->modifier == ScopeModifier::State;
                std::string hint = exprToString(rp->typeHint.get());
                declareSymbol(rp->name.lexeme, SymbolKind::Variable, rp->name.position, rp->name.position + static_cast<int>(rp->name.lexeme.length()), hint, effLocal);
            }
        } else if (auto* lp = dynamic_cast<ListPattern*>(pat)) {
            for (auto& e : lp->elements) declarePattern(e.get(), isLocal, isConst);
            if (lp->rest) declarePattern(lp->rest.get(), isLocal, isConst);
        } else if (auto* mp = dynamic_cast<MatrixPattern*>(pat)) {
            for (auto& row : mp->rows) for (auto& e : row) declarePattern(e.get(), isLocal, isConst);
            if (mp->restRow) declarePattern(mp->restRow.get(), isLocal, isConst);
        } else if (auto* dp = dynamic_cast<DictPattern*>(pat)) {
            for (auto& e : dp->entries) declarePattern(e.second.get(), isLocal, isConst);
            if (dp->rest) declarePattern(dp->rest.get(), isLocal, isConst);
        } else if (auto* defp = dynamic_cast<DefaultPattern*>(pat)) {
            declarePattern(defp->inner.get(), isLocal, isConst);
            if (defp->defaultExpr) defp->defaultExpr->accept(*this);
        } else if (auto* ep = dynamic_cast<ExprPattern*>(pat)) {
            if (ep->expr) ep->expr->accept(*this);
        } else if (auto* dap = dynamic_cast<DynamicAssertPattern*>(pat)) {
            if (dap->expr) dap->expr->accept(*this);
        }
    }

    void SemanticAnalyzer::hoistBlock(Block* expr) {
        for (auto& stmt : expr->statements) {
            if (auto* assign = dynamic_cast<Assign*>(stmt.get())) {
                bool isLoc = assign->isLocal || assign->isState || assign->isConst;
                std::string hint = exprToString(assign->typeHint.get());
                std::string inferred = inferType(assign->value.get());
                declareSymbol(assign->name.lexeme, SymbolKind::Variable, assign->name.position, assign->name.position + static_cast<int>(assign->name.lexeme.length()), hint, isLoc, inferred);
            } else if (auto* locDecl = dynamic_cast<LocalDecl*>(stmt.get())) {
                std::string hint = exprToString(locDecl->typeHint.get());
                declareSymbol(locDecl->name.lexeme, SymbolKind::Variable, locDecl->name.position, locDecl->name.position + static_cast<int>(locDecl->name.lexeme.length()), hint, true);
            } else if (auto* stateDecl = dynamic_cast<StateDecl*>(stmt.get())) {
                std::string hint = exprToString(stateDecl->typeHint.get());
                declareSymbol(stateDecl->name.lexeme, SymbolKind::Variable, stateDecl->name.position, stateDecl->name.position + static_cast<int>(stateDecl->name.lexeme.length()), hint, true);
            } else if (auto* refDecl = dynamic_cast<RefDecl*>(stmt.get())) {
                std::string hint = exprToString(refDecl->typeHint.get());
                declareSymbol(refDecl->name.lexeme, SymbolKind::Variable, refDecl->name.position, refDecl->name.position + static_cast<int>(refDecl->name.lexeme.length()), hint, true);
            } else if (auto* constDecl = dynamic_cast<ConstDecl*>(stmt.get())) {
                declareSymbol(constDecl->name.lexeme, SymbolKind::Variable, constDecl->name.position, constDecl->name.position + static_cast<int>(constDecl->name.lexeme.length()), "", true);
            } else if (auto* destAssign = dynamic_cast<DestructAssign*>(stmt.get())) {
                declarePattern(destAssign->pattern.get(), destAssign->isLocal || destAssign->isState || destAssign->isConst, destAssign->isConst);
            } else if (auto* cls = dynamic_cast<ClassDefExpr*>(stmt.get())) {
                if (!cls->name.lexeme.empty() && cls->name.lexeme.find("<") != 0) {
                    declareSymbol(cls->name.lexeme, SymbolKind::Class, cls->name.position, cls->name.position + static_cast<int>(cls->name.lexeme.length()), "", true, "class_type");
                }
            } else if (auto* ns = dynamic_cast<NamespaceDecl*>(stmt.get())) {
                if (!ns->name.lexeme.empty() && ns->name.lexeme.find("<") != 0) {
                    declareSymbol(ns->name.lexeme, SymbolKind::Namespace, ns->name.position, ns->name.position + static_cast<int>(ns->name.lexeme.length()), "", true, "namespace_type");
                }
            } else if (auto* mac = dynamic_cast<MacroDefExpr*>(stmt.get())) {
                declareSymbol(mac->name.lexeme, SymbolKind::Function, mac->name.position, mac->name.position + static_cast<int>(mac->name.lexeme.length()), "", true, "macro");
            }
        }
    }

    std::shared_ptr<Symbol> SemanticAnalyzer::resolveSymbol(const std::string& name) {
        Scope* scope = currentScope;
        while (scope) {
            auto it = scope->symbols.find(name);
            if (it != scope->symbols.end()) {
                return it->second;
            }
            scope = scope->parent;
        }
        return nullptr;
    }

    std::shared_ptr<Symbol> SemanticAnalyzer::resolveSymbolAt(const std::string& name, const Position& pos) {
        Scope* scope = getScopeAt(globalScope.get(), pos);
        if (!scope && isPosInRange(pos, globalScope->scopeRange)) {
            scope = globalScope.get();
        }
        while (scope) {
            auto it = scope->symbols.find(name);
            if (it != scope->symbols.end()) {
                return it->second;
            }
            scope = scope->parent;
        }
        return nullptr;
    }

    std::string SemanticAnalyzer::exprToString(Expr* expr) {
        if (!expr) return "";
        if (auto* v = dynamic_cast<Variable*>(expr)) return v->name.lexeme;
        if (auto* b = dynamic_cast<Binary*>(expr)) {
            if (b->op.lexeme == "|" || b->op.lexeme == "&") {
                return exprToString(b->left.get()) + " " + b->op.lexeme + " " + exprToString(b->right.get());
            }
        }
        if (auto* g = dynamic_cast<GroupingExpr*>(expr)) return "(" + exprToString(g->expression.get()) + ")";
        if (auto* d = dynamic_cast<DotAccess*>(expr)) return exprToString(d->object.get()) + "." + d->field.lexeme;
        return "";
    }

    std::string SemanticAnalyzer::inferType(Expr* expr) {
        if (!expr) return "";
        if (auto* ta = dynamic_cast<TypeAssertExpr*>(expr)) return exprToString(ta->typeHint.get());
        if (auto* ea = dynamic_cast<ExprAssign*>(expr)) return inferType(ea->value.get());
        if (auto* v = dynamic_cast<Variable*>(expr)) {
            auto sym = resolveSymbolAt(v->name.lexeme, doc->offsetToPosition(v->startPos));
            if (sym) {
                if (!sym->typeHint.empty()) return sym->typeHint;
                if (!sym->inferredType.empty()) return sym->inferredType;
                if (sym->kind == SymbolKind::Class) return v->name.lexeme; // Class itself
            }
            return "";
        }
        if (auto* b = dynamic_cast<Binary*>(expr)) {
            std::string op = b->op.lexeme;
            if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=" || op == "in" || op == "is" || op == "<:" || op == "&&" || op == "||") {
                return "bool";
            }
            
            std::string lt = inferType(b->left.get());
            std::string rt = inferType(b->right.get());
            
            if (lt.empty() && !rt.empty()) lt = rt;
            if (rt.empty() && !lt.empty()) rt = lt;
            if (lt.empty() && rt.empty()) return "";

            if (op == "|") {
                if (lt == "type" || rt == "type" || lt == "class_type" || rt == "class_type") return "type";
                if (lt == "set" || rt == "set") return "set";
                if (lt == "int" && rt == "int") return "int";
                return "";
            }
            if (op == "&" || op == "^^") {
                if (lt == "set" || rt == "set") return "set";
                if (lt == "int" && rt == "int") return "int";
                return "";
            }
            if (op == "<<" || op == ">>") {
                if (lt == "int" && rt == "int") return "int";
                return "";
            }

            if (op == "+") {
                if (lt == "string" || rt == "string") return "string";
                if (lt == "list" || rt == "list") return "list";
                if (lt == "dict" || rt == "dict") return "dict";
            }
            if (op == "-") {
                if (lt == "dict") return "dict";
                if (lt == "set" && rt == "set") return "set";
            }
            if (op == "*") {
                if (lt == "string" || rt == "string") return "string";
                if (lt == "set" && rt == "set") return "set";
            }
            if (op == "/") {
                if (lt == "int" && rt == "int") return "fraction";
            }
            if (op == "~/") {
                if (lt == "int" && rt == "int") return "int";
                if (lt == "double" || rt == "double") return "double";
            }
            if (op == "%") {
                if (lt == "int" && rt == "int") return "int";
                if (lt == "double" || rt == "double") return "double";
            }
            if (op == "^") {
                if (lt == "int" && rt == "int") return "int";
            }

            bool lMat = (lt == "realmatrix" || lt == "complexmatrix" || lt == "symmatrix" || lt == "matrix");
            bool rMat = (rt == "realmatrix" || rt == "complexmatrix" || rt == "symmatrix" || rt == "matrix");
            if (lMat || rMat) {
                if (lt == "symmatrix" || rt == "symmatrix" || lt == "symbolic" || rt == "symbolic") return "symmatrix";
                if (lt == "complexmatrix" || rt == "complexmatrix" || lt == "complex" || rt == "complex") return "complexmatrix";
                if (lt == "realmatrix" || rt == "realmatrix") return "realmatrix";
                return "matrix";
            }

            if (lt == "symbolic" || rt == "symbolic") return "symbolic";
            if (lt == "complex" || rt == "complex") return "complex";
            if (lt == "double" || rt == "double") return "double";
            if (lt == "fraction" || rt == "fraction") return "fraction";
            if (lt == "int" && rt == "int") return "int";

            return lt;
        }
        if (auto* u = dynamic_cast<Unary*>(expr)) {
            if (u->op.lexeme == "!") return "bool";
            if (u->op.lexeme == "~") {
                std::string rt = inferType(u->right.get());
                if (rt == "int") return "int";
                return "";
            }
            return inferType(u->right.get());
        }
        if (dynamic_cast<MatrixNode*>(expr)) return "matrix";
        if (dynamic_cast<ListNode*>(expr) || dynamic_cast<ListCompExpr*>(expr)) return "list";
        if (dynamic_cast<DictLiteral*>(expr) || dynamic_cast<DictCompExpr*>(expr)) return "dict";
        if (dynamic_cast<SetLiteral*>(expr) || dynamic_cast<SetCompExpr*>(expr)) return "set";
        if (dynamic_cast<LambdaExpr*>(expr)) return "function";
        if (dynamic_cast<MacroDefExpr*>(expr)) return "macro";
        if (dynamic_cast<ClassDefExpr*>(expr)) return "class_type";
        if (dynamic_cast<NamespaceDecl*>(expr)) return "namespace_type";
        if (auto* lit = dynamic_cast<Literal*>(expr)) {
            if (lit->isString) return "string";
            if (lit->isImaginary) return "complex";
            if (lit->isKeyword) {
                if (lit->value == "true" || lit->value == "false") return "bool";
                if (lit->value == "none") return "none_type";
            }
            if (lit->value.find('.') != std::string::npos || lit->value.find('e') != std::string::npos || lit->value.find('E') != std::string::npos) return "double";
            return "int";
        }
        if (auto* call = dynamic_cast<Call*>(expr)) {
            std::string calleeName = call->callee.lexeme;
            if (calleeName == "matrix" || calleeName == "symmatrix" || calleeName == "ones" || calleeName == "zeros" || calleeName == "id") return "matrix";
            if (calleeName == "list") return "list";
            if (calleeName == "dict") return "dict";
            if (calleeName == "set") return "set";
            if (calleeName == "complex") return "complex";
            if (calleeName == "frac" || calleeName == "toFrac") return "fraction";
            if (calleeName == "string" || calleeName == "str") return "string";
            
            // 检查是否是类实例化
            auto sym = resolveSymbolAt(calleeName, doc->offsetToPosition(call->startPos));
            if (sym && sym->kind == SymbolKind::Class) {
                return calleeName; // 推导为该类的实例
            }
        }
        return "";
    }

    std::string SemanticAnalyzer::extractDocstring(int nodeStartPos) {
        // 往前找紧挨着的 COMMENT token
        std::string docstring = "";
        for (int i = static_cast<int>(tokens.size()) - 1; i >= 0; --i) {
            if (tokens[i].position >= nodeStartPos) continue;
            
            if (tokens[i].type == TokenType::COMMENT) {
                // 简单提取注释内容
                std::string comment = tokens[i].lexeme;
                if (comment.find("//") == 0) {
                    docstring = comment.substr(2) + "\n" + docstring;
                } else if (comment.find("/*") == 0) {
                    docstring = comment.substr(2, comment.length() - 4) + "\n" + docstring;
                }
            } else if (tokens[i].type != TokenType::NEWLINE) {
                break; // 遇到非空白/非注释 token，停止提取
            }
        }
        
        // 简单 trim
        size_t first = docstring.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = docstring.find_last_not_of(" \t\r\n");
        return docstring.substr(first, (last - first + 1));
    }

    std::shared_ptr<Symbol> SemanticAnalyzer::getSymbolAt(const Position& pos) {
        std::shared_ptr<Symbol> bestMatch = nullptr;
        // 寻找包含该位置的最小 Range（由于 AST 遍历顺序，后加入的通常是更深层的节点）
        for (const auto& ref : references) {
            if (isPosInRange(pos, ref.range)) {
                bestMatch = ref.symbol;
            }
        }
        return bestMatch;
    }

    Scope* SemanticAnalyzer::getScopeAt(Scope* scope, const Position& pos) {
        if (!scope) return nullptr;
        for (auto& child : scope->children) {
            if (isPosInRange(pos, child->scopeRange)) {
                if (auto found = getScopeAt(child.get(), pos)) {
                    return found;
                }
                return child.get();
            }
        }
        return scope;
    }

    std::vector<std::shared_ptr<Symbol>> SemanticAnalyzer::getVisibleSymbolsAt(const Position& pos) {
        std::vector<std::shared_ptr<Symbol>> visible;
        Scope* scope = getScopeAt(globalScope.get(), pos);
        if (!scope && isPosInRange(pos, globalScope->scopeRange)) {
            scope = globalScope.get();
        }
        while (scope) {
            for (const auto& pair : scope->symbols) {
                visible.push_back(pair.second);
            }
            scope = scope->parent;
        }
        return visible;
    }

    void SemanticAnalyzer::buildDocumentSymbols(Scope* scope, std::vector<DocumentSymbol>& outSymbols) {
        if (!scope) return;
        
        // 将当前作用域的符号转换为 DocumentSymbol
        for (const auto& [name, sym] : scope->symbols) {
            // 过滤掉不需要在大纲中显示的局部变量和参数，保持大纲清晰
            if (sym->kind == SymbolKind::Parameter) continue;
            if (scope->kind == ScopeKind::Block && sym->kind == SymbolKind::Variable) continue;

            DocumentSymbol ds;
            ds.name = sym->name;
            ds.detail = sym->typeHint;
            
            if (sym->kind == SymbolKind::Function) ds.kind = 12; // Function
            else if (sym->kind == SymbolKind::Class) ds.kind = 5; // Class
            else if (sym->kind == SymbolKind::Variable) ds.kind = 13; // Variable
            else if (sym->kind == SymbolKind::Property) ds.kind = 7; // Property
            else if (sym->kind == SymbolKind::Namespace) ds.kind = 2; // Module
            else ds.kind = 13;

            ds.selectionRange = sym->definitionRange;
            ds.range = sym->definitionRange; // 默认范围
            
            // 尝试从子作用域中找到与该符号对应的完整范围（例如函数体、类体）
            for (const auto& child : scope->children) {
                if ((child->kind == ScopeKind::Function && sym->kind == SymbolKind::Function) ||
                    (child->kind == ScopeKind::Class && sym->kind == SymbolKind::Class) ||
                    (child->kind == ScopeKind::Namespace && sym->kind == SymbolKind::Namespace)) {
                    
                    // 启发式匹配：子作用域的起始位置紧跟在符号定义之后
                    if (child->scopeRange.start.line >= sym->definitionRange.start.line &&
                        child->scopeRange.start.line <= sym->definitionRange.end.line + 1) {
                        ds.range = child->scopeRange;
                        // 递归构建嵌套的子符号（如类的方法）
                        buildDocumentSymbols(child.get(), ds.children);
                        break;
                    }
                }
            }
            
            outSymbols.push_back(ds);
        }
        
        // 对于没有关联到特定符号的子作用域（如普通的 Block），直接将其内部符号提升到当前层级
        for (const auto& child : scope->children) {
            if (child->kind == ScopeKind::Block) {
                buildDocumentSymbols(child.get(), outSymbols);
            }
        }
    }

    std::vector<DocumentSymbol> SemanticAnalyzer::getDocumentSymbols() {
        std::vector<DocumentSymbol> symbols;
        buildDocumentSymbols(globalScope.get(), symbols);
        return symbols;
    }

    // ========================================================================
    // ExprVisitor 实现 (核心节点)
    // ========================================================================

    void SemanticAnalyzer::visitBlock(Block* expr) {
        Range range;
        range.start = doc->offsetToPosition(expr->startPos);
        range.end = doc->offsetToPosition(expr->endPos);
        
        enterScope(range, ScopeKind::Block);
        hoistBlock(expr);
        for (auto& stmt : expr->statements) {
            if (stmt) stmt->accept(*this);
        }
        leaveScope();
    }

    void SemanticAnalyzer::visitAssign(Assign* expr) {
        if (expr->value) {
            expr->value->accept(*this);
        }
        
        SymbolKind kind = SymbolKind::Variable;
        if (dynamic_cast<LambdaExpr*>(expr->value.get()) || dynamic_cast<ClassDefExpr*>(expr->value.get())) {
            // 如果右侧是 Lambda 或 Class，已经在它们自己的 visit 中处理了声明
            return;
        }
        
        std::string inferred = inferType(expr->value.get());
        std::string hint = exprToString(expr->typeHint.get());
        
        if (!isTypeCompatible(inferred, hint)) {
            diagnostics.push_back({"Warning: Type mismatch. Expected '" + hint + "', but inferred '" + inferred + "'.", expr->value->startPos, expr->value->endPos, 2});
        }
        
        declareSymbol(expr->name.lexeme, kind, expr->name.position, expr->name.position + static_cast<int>(expr->name.lexeme.length()), hint, expr->isLocal || expr->isState || expr->isConst, inferred);
    }

    void SemanticAnalyzer::visitVariable(Variable* expr) {
        // 变量引用，记录到引用表中以供查询
        auto sym = resolveSymbol(expr->name.lexeme);
        if (sym) {
            Range r;
            r.start = doc->offsetToPosition(expr->startPos);
            r.end = doc->offsetToPosition(expr->endPos);
            references.push_back({r, sym});
        }
    }

    void SemanticAnalyzer::visitLambdaExpr(LambdaExpr* expr) {
        // 声明函数本身
        if (!expr->name.empty() && expr->name.find("<") != 0) {
            declareSymbol(expr->name, SymbolKind::Function, expr->startPos, expr->startPos + static_cast<int>(expr->name.length()), "", true, "function");
        }
        
        Range range;
        range.start = doc->offsetToPosition(expr->startPos);
        range.end = doc->offsetToPosition(expr->endPos);
        
        enterScope(range, ScopeKind::Function);
        
        // 声明参数
        for (size_t i = 0; i < expr->params.size(); ++i) {
            std::string hint = i < expr->paramTypes.size() ? exprToString(expr->paramTypes[i].get()) : "";
            declareSymbol(expr->params[i].lexeme, SymbolKind::Parameter, expr->params[i].position, expr->params[i].position + static_cast<int>(expr->params[i].lexeme.length()), hint);
        }
        for (size_t i = 0; i < expr->kwargParams.size(); ++i) {
            std::string hint = i < expr->kwargTypes.size() ? exprToString(expr->kwargTypes[i].get()) : "";
            declareSymbol(expr->kwargParams[i].lexeme, SymbolKind::Parameter, expr->kwargParams[i].position, expr->kwargParams[i].position + static_cast<int>(expr->kwargParams[i].lexeme.length()), hint);
        }
        if (!expr->restName.empty()) {
            declareSymbol(expr->restName, SymbolKind::Parameter, expr->startPos, expr->startPos, "list"); // 简化位置
        }
        if (!expr->kwargsName.empty()) {
            declareSymbol(expr->kwargsName, SymbolKind::Parameter, expr->startPos, expr->startPos, "dict");
        }
        
        if (expr->body) {
            expr->body->accept(*this);
        }
        
        leaveScope();
    }

    void SemanticAnalyzer::visitClassDefExpr(ClassDefExpr* expr) {
        if (!expr->name.lexeme.empty() && expr->name.lexeme.find("<") != 0) {
            declareSymbol(expr->name.lexeme, SymbolKind::Class, expr->name.position, expr->name.position + static_cast<int>(expr->name.lexeme.length()), "", true, "class_type");
        }
        
        Range range;
        range.start = doc->offsetToPosition(expr->startPos);
        range.end = doc->offsetToPosition(expr->endPos);
        
        enterScope(range, ScopeKind::Class);
        
        for (auto& prop : expr->staticProperties) {
            if (prop.value) prop.value->accept(*this);
            declareSymbol(prop.name.lexeme, SymbolKind::Property, prop.name.position, prop.name.position + static_cast<int>(prop.name.lexeme.length()));
        }
        for (auto& prop : expr->instanceProperties) {
            if (prop.value) prop.value->accept(*this);
            declareSymbol(prop.name.lexeme, SymbolKind::Property, prop.name.position, prop.name.position + static_cast<int>(prop.name.lexeme.length()));
        }
        
        leaveScope();
    }

    // ========================================================================
    // ExprVisitor 实现 (遍历子节点)
    // ========================================================================
    void SemanticAnalyzer::visitBinary(Binary* expr) {
        if (expr->left) expr->left->accept(*this);
        if (expr->right) expr->right->accept(*this);
    }
    void SemanticAnalyzer::visitUnary(Unary* expr) {
        if (expr->right) expr->right->accept(*this);
    }
    void SemanticAnalyzer::visitCall(Call* expr) {
        for (auto& arg : expr->arguments) if (arg) arg->accept(*this);
    }
    void SemanticAnalyzer::visitMatrixNode(MatrixNode* expr) {
        for (auto& row : expr->elements) {
            for (auto& e : row) if (e) e->accept(*this);
        }
    }
    void SemanticAnalyzer::visitListNode(ListNode* expr) {
        for (auto& row : expr->elements) {
            for (auto& e : row) if (e) e->accept(*this);
        }
    }
    void SemanticAnalyzer::visitIfExpr(IfExpr* expr) {
        if (expr->condition) expr->condition->accept(*this);
        if (expr->thenBranch) expr->thenBranch->accept(*this);
        if (expr->elseBranch) expr->elseBranch->accept(*this);
    }
    void SemanticAnalyzer::visitWhileExpr(WhileExpr* expr) {
        if (expr->condition) expr->condition->accept(*this);
        if (expr->body) expr->body->accept(*this);
    }
    void SemanticAnalyzer::visitForExpr(ForExpr* expr) {
        Range range;
        range.start = doc->offsetToPosition(expr->startPos);
        range.end = doc->offsetToPosition(expr->endPos);
        enterScope(range, ScopeKind::Block);
        if (expr->initializer) expr->initializer->accept(*this);
        if (expr->condition) expr->condition->accept(*this);
        if (expr->update) expr->update->accept(*this);
        if (expr->body) expr->body->accept(*this);
        leaveScope();
    }
    void SemanticAnalyzer::visitReturnExpr(ReturnExpr* expr) {
        if (expr->value) expr->value->accept(*this);
    }
    void SemanticAnalyzer::visitIndexAccess(IndexAccess* expr) {
        if (expr->object) expr->object->accept(*this);
        for (auto& idx : expr->indices) if (idx) idx->accept(*this);
    }
    void SemanticAnalyzer::visitIndexAssign(IndexAssign* expr) {
        if (expr->objectExpr) expr->objectExpr->accept(*this);
        for (auto& chain : expr->indexChain) {
            for (auto& idx : chain) if (idx) idx->accept(*this);
        }
        if (expr->value) expr->value->accept(*this);
        
        if (expr->typeHint) {
            std::string inferred = inferType(expr->value.get());
            std::string hint = exprToString(expr->typeHint.get());
            if (!isTypeCompatible(inferred, hint)) {
                diagnostics.push_back({"Warning: Type mismatch. Expected '" + hint + "', but inferred '" + inferred + "'.", expr->value->startPos, expr->value->endPos, 2});
            }
        }
    }
    void SemanticAnalyzer::visitLocalDecl(LocalDecl* expr) {
        std::string hint = exprToString(expr->typeHint.get());
        declareSymbol(expr->name.lexeme, SymbolKind::Variable, expr->name.position, expr->name.position + static_cast<int>(expr->name.lexeme.length()), hint, true);
    }
    void SemanticAnalyzer::visitRefDecl(RefDecl* expr) {
        std::string hint = exprToString(expr->typeHint.get());
        declareSymbol(expr->name.lexeme, SymbolKind::Variable, expr->name.position, expr->name.position + static_cast<int>(expr->name.lexeme.length()), hint, true);
    }
    void SemanticAnalyzer::visitStateDecl(StateDecl* expr) {
        std::string hint = exprToString(expr->typeHint.get());
        declareSymbol(expr->name.lexeme, SymbolKind::Variable, expr->name.position, expr->name.position + static_cast<int>(expr->name.lexeme.length()), hint, true);
    }
    void SemanticAnalyzer::visitConstDecl(ConstDecl* expr) {
        declareSymbol(expr->name.lexeme, SymbolKind::Variable, expr->name.position, expr->name.position + static_cast<int>(expr->name.lexeme.length()), "", true);
    }
    void SemanticAnalyzer::visitCompoundAssign(CompoundAssign* expr) {
        if (expr->target) expr->target->accept(*this);
        if (expr->value) expr->value->accept(*this);
    }
    void SemanticAnalyzer::visitInvokeExpr(InvokeExpr* expr) {
        if (expr->callee) expr->callee->accept(*this);
        for (auto& arg : expr->arguments) if (arg) arg->accept(*this);
    }
    void SemanticAnalyzer::visitForInExpr(ForInExpr* expr) {
        Range range;
        range.start = doc->offsetToPosition(expr->startPos);
        range.end = doc->offsetToPosition(expr->endPos);
        enterScope(range, ScopeKind::Block);
        if (expr->iterable) expr->iterable->accept(*this);
        declarePattern(expr->pattern.get(), true, expr->isConst);
        if (expr->body) expr->body->accept(*this);
        leaveScope();
    }
    void SemanticAnalyzer::visitThrowExpr(ThrowExpr* expr) {
        if (expr->value) expr->value->accept(*this);
    }
    void SemanticAnalyzer::visitTryCatchExpr(TryCatchExpr* expr) {
        if (expr->tryBody) expr->tryBody->accept(*this);
        Range range;
        range.start = doc->offsetToPosition(expr->catchBody ? expr->catchBody->startPos : expr->startPos);
        range.end = doc->offsetToPosition(expr->endPos);
        enterScope(range, ScopeKind::Block);
        declarePattern(expr->catchPattern.get(), true, false);
        if (expr->catchBody) expr->catchBody->accept(*this);
        leaveScope();
    }
    void SemanticAnalyzer::visitSwitchExpr(SwitchExpr* expr) {
        if (expr->subject) expr->subject->accept(*this);
        for (auto& c : expr->cases) {
            for (auto& v : c.first) if (v) v->accept(*this);
            if (c.second) c.second->accept(*this);
        }
        if (expr->defaultBody) expr->defaultBody->accept(*this);
    }
    void SemanticAnalyzer::visitNamespaceDecl(NamespaceDecl* expr) {
        declareSymbol(expr->name.lexeme, SymbolKind::Namespace, expr->name.position, expr->name.position + static_cast<int>(expr->name.lexeme.length()), "", true, "namespace_type");
        Range range;
        range.start = doc->offsetToPosition(expr->startPos);
        range.end = doc->offsetToPosition(expr->endPos);
        enterScope(range, ScopeKind::Namespace);
        if (expr->body) expr->body->accept(*this);
        leaveScope();
    }

    void SemanticAnalyzer::visitMacroDefExpr(MacroDefExpr* expr) {
        declareSymbol(expr->name.lexeme, SymbolKind::Function, expr->name.position, expr->name.position + static_cast<int>(expr->name.lexeme.length()), "", true, "macro");
        Range range;
        range.start = doc->offsetToPosition(expr->startPos);
        range.end = doc->offsetToPosition(expr->endPos);
        enterScope(range, ScopeKind::Function);
        for (const auto& param : expr->params) {
            declareSymbol(param.lexeme, SymbolKind::Parameter, param.position, param.position + static_cast<int>(param.lexeme.length()), "ASTNode");
        }
        if (!expr->restName.empty()) {
            declareSymbol(expr->restName, SymbolKind::Parameter, expr->startPos, expr->startPos, "list");
        }
        if (expr->body) expr->body->accept(*this);
        leaveScope();
    }
    void SemanticAnalyzer::visitDotAccess(DotAccess* expr) {
        if (expr->object) expr->object->accept(*this);
    }
    void SemanticAnalyzer::visitDotAssign(DotAssign* expr) {
        if (expr->object) expr->object->accept(*this);
        if (expr->value) expr->value->accept(*this);
        
        if (expr->typeHint) {
            std::string inferred = inferType(expr->value.get());
            std::string hint = exprToString(expr->typeHint.get());
            if (!isTypeCompatible(inferred, hint)) {
                diagnostics.push_back({"Warning: Type mismatch. Expected '" + hint + "', but inferred '" + inferred + "'.", expr->value->startPos, expr->value->endPos, 2});
            }
        }
    }
    void SemanticAnalyzer::visitMethodCallExpr(MethodCallExpr* expr) {
        if (expr->object) expr->object->accept(*this);
        for (auto& arg : expr->arguments) if (arg) arg->accept(*this);
    }
    void SemanticAnalyzer::visitDestructAssign(DestructAssign* expr) {
        if (expr->value) expr->value->accept(*this);
        declarePattern(expr->pattern.get(), expr->isLocal || expr->isState || expr->isConst, expr->isConst);
    }
    void SemanticAnalyzer::visitFStringExpr(FStringExpr* expr) {
        for (auto& e : expr->exprs) if (e) e->accept(*this);
    }
    void SemanticAnalyzer::visitMatrixCompExpr(MatrixCompExpr* expr) {
        Range range;
        range.start = doc->offsetToPosition(expr->startPos);
        range.end = doc->offsetToPosition(expr->endPos);
        enterScope(range, ScopeKind::Block);
        for (auto& c : expr->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            declarePattern(c.pattern.get(), true, false);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (expr->valueExpr) expr->valueExpr->accept(*this);
        leaveScope();
    }
    void SemanticAnalyzer::visitListCompExpr(ListCompExpr* expr) {
        Range range;
        range.start = doc->offsetToPosition(expr->startPos);
        range.end = doc->offsetToPosition(expr->endPos);
        enterScope(range, ScopeKind::Block);
        for (auto& c : expr->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            declarePattern(c.pattern.get(), true, false);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (expr->valueExpr) expr->valueExpr->accept(*this);
        leaveScope();
    }
    void SemanticAnalyzer::visitSetCompExpr(SetCompExpr* expr) {
        Range range;
        range.start = doc->offsetToPosition(expr->startPos);
        range.end = doc->offsetToPosition(expr->endPos);
        enterScope(range, ScopeKind::Block);
        for (auto& c : expr->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            declarePattern(c.pattern.get(), true, false);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (expr->valueExpr) expr->valueExpr->accept(*this);
        leaveScope();
    }
    void SemanticAnalyzer::visitDictCompExpr(DictCompExpr* expr) {
        Range range;
        range.start = doc->offsetToPosition(expr->startPos);
        range.end = doc->offsetToPosition(expr->endPos);
        enterScope(range, ScopeKind::Block);
        for (auto& c : expr->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            declarePattern(c.pattern.get(), true, false);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (expr->keyExpr) expr->keyExpr->accept(*this);
        if (expr->valueExpr) expr->valueExpr->accept(*this);
        leaveScope();
    }
    void SemanticAnalyzer::visitDictLiteral(DictLiteral* expr) {
        for (auto& pair : expr->entries) {
            if (pair.first) pair.first->accept(*this);
            if (pair.second) pair.second->accept(*this);
        }
    }
    void SemanticAnalyzer::visitSetLiteral(SetLiteral* expr) {
        for (auto& e : expr->elements) if (e) e->accept(*this);
    }
    void SemanticAnalyzer::visitSliceExpr(SliceExpr* expr) {
        if (expr->start) expr->start->accept(*this);
        if (expr->end) expr->end->accept(*this);
        if (expr->step) expr->step->accept(*this);
    }
    void SemanticAnalyzer::visitSequenceExpr(SequenceExpr* expr) {
        for (auto& e : expr->expressions) if (e) e->accept(*this);
    }
    void SemanticAnalyzer::visitMatchExpr(MatchExpr* expr) {
        if (expr->subject) expr->subject->accept(*this);
        for (auto& b : expr->branches) {
            Range range;
            range.start = doc->offsetToPosition(b.body ? b.body->startPos : expr->startPos);
            range.end = doc->offsetToPosition(b.body ? b.body->endPos : expr->endPos);
            enterScope(range, ScopeKind::Block);
            for (auto& p : b.patterns) declarePattern(p.get(), true, false);
            if (b.guard) b.guard->accept(*this);
            if (b.body) b.body->accept(*this);
            leaveScope();
        }
    }
    void SemanticAnalyzer::visitGroupingExpr(GroupingExpr* expr) {
        if (expr->expression) expr->expression->accept(*this);
    }
    void SemanticAnalyzer::visitExprAssign(ExprAssign* expr) {
        if (expr->target) expr->target->accept(*this);
        if (expr->value) expr->value->accept(*this);
    }
    void SemanticAnalyzer::visitDeferExpr(DeferExpr* expr) {
        if (expr->body) expr->body->accept(*this);
    }
    void SemanticAnalyzer::visitKeywordArgExpr(KeywordArgExpr* expr) {
        if (expr->value) expr->value->accept(*this);
    }
    void SemanticAnalyzer::visitSpreadExpr(SpreadExpr* expr) {
        if (expr->value) expr->value->accept(*this);
    }
    void SemanticAnalyzer::visitTypeAssertExpr(TypeAssertExpr* expr) {
        if (expr->value) expr->value->accept(*this);
        if (expr->typeHint) expr->typeHint->accept(*this);
        
        std::string inferred = inferType(expr->value.get());
        std::string hint = exprToString(expr->typeHint.get());
        if (!isTypeCompatible(inferred, hint)) {
            diagnostics.push_back({"Warning: Type assertion may fail. Inferred type '" + inferred + "' does not match '" + hint + "'.", expr->startPos, expr->endPos, 2});
        }
    }

} // namespace lsp
} // namespace jc
