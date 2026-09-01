#include "NameResolver.h"
#include "../../vm/VM.h"
#include "../../vm/BuiltinRegistry.h"
#include "../../memory/Value.h"
#include "../../vm/HelpRouter.h"
#include "../../utils/json/Json.h"
#include <filesystem>
#include <climits>
#include <fstream>
#include <sstream>

namespace jc {
namespace lsp {

    static std::string objectNameOf(Expr* obj) {
        if (auto* v = dynamic_cast<Variable*>(obj)) return v->name.lexeme;
        return "";
    }

    NameResolver::NameResolver(Document* doc, BuiltinIndex& index) : doc(doc), index(index) {
        globalScope.name = "<global>";
        current = &globalScope;
        // 可覆盖的预置全局变量（当普通变量，不报 shadowing）
        declare("PI", UserSymbol::Variable, 0, 0, "", "double");
        declare("E", UserSymbol::Variable, 0, 0, "", "double");
        declare("i", UserSymbol::Variable, 0, 0, "", "complex");
        declare("I", UserSymbol::Variable, 0, 0, "", "complex");
        declare("ANS", UserSymbol::Variable, 0, 0, "", "any");
    }

    void NameResolver::resolve(Expr* root) {
        if (root) root->accept(*this);
    }

    const NameRes* NameResolver::resolveAt(Expr* node) const {
        auto it = nameRes.find(node);
        return it != nameRes.end() ? &it->second : nullptr;
    }

    const NameRes* NameResolver::resolveAtPos(const Position& pos) const {
        int offset = doc->positionToOffset(pos);
        const NameRes* best = nullptr;
        int bestLen = INT_MAX;
        for (const auto& [node, res] : nameRes) {
            if (node->startPos <= offset && offset < node->endPos) {
                int len = node->endPos - node->startPos;
                if (len < bestLen) { bestLen = len; best = &res; }
            }
        }
        return best;
    }

    // ---------- 作用域 ----------
    void NameResolver::enterScope(Scope* parent, Range range, bool isFunction, bool isNamespace, bool isClass) {
        auto s = std::make_unique<Scope>();
        s->parent = parent;
        s->range = range;
        s->isFunction = isFunction;
        s->isNamespace = isNamespace;
        s->isClass = isClass;
        Scope* ptr = s.get();
        parent->children.push_back(std::move(s));
        current = ptr;
    }

    void NameResolver::leaveScope() {
        if (current->parent) current = current->parent;
    }

    UserSymbol* NameResolver::declare(const std::string& name, UserSymbol::Kind kind, int startPos, int endPos,
                                      const std::string& typeHint, const std::string& inferredType) {
        if (!current) return nullptr;
        // shadowing 检查（排除可覆盖的预置变量 PI/E/I/ANS，排除内部名 <...>）
        static const std::unordered_set<std::string> overridable = {"PI", "E", "i", "I", "ANS"};
        if (!overridable.count(name) && name.find("<") != 0) {
            if (index.findGlobal(name)) {
                shadowWarn.push_back({ name, startPos });
            }
        }
        UserSymbol sym;
        sym.name = name;
        sym.kind = kind;
        sym.defPos = doc->offsetToPosition(startPos);
        sym.defEndPos = doc->offsetToPosition(endPos);
        sym.typeHint = typeHint;
        sym.inferredType = inferredType;
        auto [it, inserted] = current->symbols.emplace(name, std::move(sym));
        return &it->second;
    }

    UserSymbol* NameResolver::findUser(const std::string& name) const {
        Scope* s = current;
        while (s) {
            auto it = s->symbols.find(name);
            if (it != s->symbols.end()) return const_cast<UserSymbol*>(&it->second);
            s = s->parent;
        }
        return nullptr;
    }

    NameRes NameResolver::resolveName(const std::string& name) {
        NameRes res;
        UserSymbol* user = findUser(name);
        if (user) {
            res.origin = NameRes::User;
            res.user = user;
            return res;
        }
        const BuiltinSymbol* b = index.findGlobal(name);
        if (b) {
            res.origin = NameRes::Builtin;
            res.builtin = b;
            return res;
        }
        res.origin = NameRes::Undefined;
        return res;
    }

    void NameResolver::record(Expr* node, const NameRes& res) {
        nameRes[node] = res;
    }

    // ---------- import：读 sidecar json（不 LoadLibrary） ----------
    void NameResolver::handleImport(ImportExpr* expr) {
        std::string moduleName;
        if (auto* lit = dynamic_cast<Literal*>(expr->path.get())) {
            moduleName = lit->value;
        } else if (auto* v = dynamic_cast<Variable*>(expr->path.get())) {
            moduleName = v->name.lexeme;
        }
        if (moduleName.empty()) return;
        if (importedModules.count(moduleName)) return;

        // 找 sidecar json（lib/xxx.json，与 xxx.dll 成对）
        std::string jsonPath = helpers::safeResolvePath(moduleName + ".json");
        std::filesystem::path p(jsonPath);
        if (!std::filesystem::is_regular_file(p)) {
            importedModules[moduleName] = {};  // 无 sidecar（.jc2 脚本 / 老 dll），符号未知
            return;
        }

        std::ifstream f(jsonPath);
        if (!f.is_open()) { importedModules[moduleName] = {}; return; }
        std::stringstream ss;
        ss << f.rdbuf();
        f.close();

        Json root;
        try {
            root = Json::parse(ss.str());
        } catch (...) {
            importedModules[moduleName] = {};
            return;
        }
        if (!root.isObject()) { importedModules[moduleName] = {}; return; }

        std::unordered_map<std::string, BuiltinSymbol> members;

        // 模块函数
        if (root.has("functions") && root["functions"].isArray()) {
            for (const auto& fn : root["functions"].arrVal) {
                if (!fn.isObject() || !fn.has("name") || !fn["name"].isString()) continue;
                BuiltinSymbol sym;
                sym.name = fn["name"].strVal;
                sym.kind = BuiltinKind::ModuleMember;
                sym.owner = moduleName;
                int minA = fn.has("minArity") && fn["minArity"].isNumber() ? static_cast<int>(fn["minArity"].numVal) : 0;
                int maxA = fn.has("maxArity") && fn["maxArity"].isNumber() ? static_cast<int>(fn["maxArity"].numVal) : 0;
                for (int i = minA; i <= maxA; ++i) sym.arity.insert(i);
                if (fn.has("params") && fn["params"].isArray()) {
                    for (const auto& prm : fn["params"].arrVal) if (prm.isString()) sym.paramNames.push_back(prm.strVal);
                }
                if (fn.has("rest") && fn["rest"].isString()) sym.restName = fn["rest"].strVal;
                members[sym.name] = std::move(sym);
            }
        }

        // 类（类名是模块成员）+ 方法名（收集到 importedMethods，不报未知方法）
        if (root.has("classes") && root["classes"].isArray()) {
            for (const auto& cls : root["classes"].arrVal) {
                if (!cls.isObject() || !cls.has("name") || !cls["name"].isString()) continue;
                std::string clsName = cls["name"].strVal;
                BuiltinSymbol clsSym;
                clsSym.name = clsName;
                clsSym.kind = BuiltinKind::ModuleMember;
                clsSym.owner = moduleName;
                members[clsName] = std::move(clsSym);
                if (cls.has("methods") && cls["methods"].isArray()) {
                    for (const auto& m : cls["methods"].arrVal) {
                        if (m.isObject() && m.has("name") && m["name"].isString()) importedMethods.insert(m["name"].strVal);
                    }
                }
            }
        }

        importedModules[moduleName] = std::move(members);
    }

    std::string NameResolver::extractDocstring(int) { return ""; }

    std::vector<UserSymbol*> NameResolver::visibleSymbolsAt(const Position& pos) const {
        std::vector<UserSymbol*> out;
        // 找到包含 pos 的最深作用域，收集其及祖先的符号
        std::function<void(Scope*)> walk = [&](Scope* s) {
            for (auto& [n, sym] : s->symbols) out.push_back(const_cast<UserSymbol*>(&sym));
            for (auto& c : s->children) {
                if (pos.line >= c->range.start.line && pos.line <= c->range.end.line) {
                    walk(c.get());
                    break;
                }
            }
        };
        walk(const_cast<Scope*>(&globalScope));
        return out;
    }

    std::vector<UserSymbol*> NameResolver::documentSymbols() const {
        std::vector<UserSymbol*> out;
        for (auto& [n, sym] : globalScope.symbols) out.push_back(const_cast<UserSymbol*>(&sym));
        return out;
    }

    // ---------- hoist ----------
    static void hoistDecls(NameResolver* nr, Block* block);

    // ---------- ExprVisitor ----------
    void NameResolver::visitBlock(Block* e) {
        Range range;
        range.start = doc->offsetToPosition(e->startPos);
        range.end = doc->offsetToPosition(e->endPos);
        enterScope(current, range);
        // hoist 块内声明
        for (auto& stmt : e->statements) {
            if (auto* a = dynamic_cast<Assign*>(stmt.get())) {
                if (!a->isLocal && !a->isState && !a->isConst) {
                    // 全局/函数级赋值：hoist 到最近的函数/命名空间/全局
                    // 简化：声明到当前（块）作用域，由 findUser 向上可见
                }
            }
        }
        // 先声明函数/类/命名空间/局部声明（支持先引用后声明）
        for (auto& stmt : e->statements) {
            if (auto* a = dynamic_cast<Assign*>(stmt.get())) {
                if (dynamic_cast<LambdaExpr*>(a->value.get()) || dynamic_cast<ClassDefExpr*>(a->value.get())) {
                    declare(a->name.lexeme, UserSymbol::Function, a->name.position, a->name.position + (int)a->name.lexeme.size());
                }
            } else if (auto* ld = dynamic_cast<LocalDecl*>(stmt.get())) {
                declare(ld->name.lexeme, UserSymbol::Variable, ld->name.position, ld->name.position + (int)ld->name.lexeme.size());
            } else if (auto* sd = dynamic_cast<StateDecl*>(stmt.get())) {
                declare(sd->name.lexeme, UserSymbol::Variable, sd->name.position, sd->name.position + (int)sd->name.lexeme.size());
            } else if (auto* rd = dynamic_cast<RefDecl*>(stmt.get())) {
                declare(rd->name.lexeme, UserSymbol::Variable, rd->name.position, rd->name.position + (int)rd->name.lexeme.size());
            } else if (auto* cd = dynamic_cast<ConstDecl*>(stmt.get())) {
                declare(cd->name.lexeme, UserSymbol::Variable, cd->name.position, cd->name.position + (int)cd->name.lexeme.size());
            } else if (auto* cls = dynamic_cast<ClassDefExpr*>(stmt.get())) {
                declare(cls->name.lexeme, UserSymbol::Class, cls->name.position, cls->name.position + (int)cls->name.lexeme.size());
            } else if (auto* ns = dynamic_cast<NamespaceDecl*>(stmt.get())) {
                declare(ns->name.lexeme, UserSymbol::Namespace, ns->name.position, ns->name.position + (int)ns->name.lexeme.size());
            }
        }
        for (auto& stmt : e->statements) if (stmt) stmt->accept(*this);
        leaveScope();
    }

    void NameResolver::visitAssign(Assign* e) {
        if (e->value) e->value->accept(*this);
        // 如果右侧是 Lambda/Class，已在 hoist 里声明过，这里不重复
        if (dynamic_cast<LambdaExpr*>(e->value.get()) || dynamic_cast<ClassDefExpr*>(e->value.get())) return;
        declare(e->name.lexeme, UserSymbol::Variable, e->name.position, e->name.position + (int)e->name.lexeme.size());
    }

    void NameResolver::visitVariable(Variable* e) {
        record(e, resolveName(e->name.lexeme));
    }

    void NameResolver::visitCall(Call* e) {
        for (auto& arg : e->arguments) if (arg) arg->accept(*this);
        NameRes res = resolveName(e->callee.lexeme);
        res.isCallTarget = true;
        record(e, res);
    }

    void NameResolver::visitDotAccess(DotAccess* e) {
        if (e->object) e->object->accept(*this);
        NameRes res;
        res.isMethod = true;
        res.memberName = e->field.lexeme;
        std::string objName = objectNameOf(e->object.get());
        if (!objName.empty()) {
            const BuiltinSymbol* member = index.findModuleMember(objName, e->field.lexeme);
            if (member) {
                res.origin = NameRes::Imported;
                res.moduleName = objName;
                res.member = member;
                res.isMethod = false;
            } else if (importedModules.count(objName)) {
                auto& mm = importedModules[objName];
                auto it = mm.find(e->field.lexeme);
                if (it != mm.end()) {
                    res.origin = NameRes::Imported;
                    res.moduleName = objName;
                    res.member = &it->second;
                    res.isMethod = false;
                }
            }
        }
        record(e, res);
    }

    void NameResolver::visitMethodCallExpr(MethodCallExpr* e) {
        if (e->object) e->object->accept(*this);
        for (auto& arg : e->arguments) if (arg) arg->accept(*this);
        NameRes res;
        res.isMethod = true;
        res.isCallTarget = true;
        res.memberName = e->method.lexeme;
        std::string objName = objectNameOf(e->object.get());
        if (!objName.empty()) {
            const BuiltinSymbol* member = index.findModuleMember(objName, e->method.lexeme);
            if (member) {
                res.origin = NameRes::Imported;
                res.moduleName = objName;
                res.member = member;
                res.isMethod = false;  // 模块成员，非方法
            } else if (importedModules.count(objName)) {
                auto& mm = importedModules[objName];
                auto it = mm.find(e->method.lexeme);
                if (it != mm.end()) {
                    res.origin = NameRes::Imported;
                    res.moduleName = objName;
                    res.member = &it->second;
                    res.isMethod = false;
                }
            }
        }
        if (res.origin == NameRes::Undefined && index.isMethodName(e->method.lexeme)) {
            res.origin = NameRes::Builtin;  // 方法名已知
        }
        record(e, res);
    }

    void NameResolver::visitDotAssign(DotAssign* e) {
        if (e->object) e->object->accept(*this);
        if (e->value) e->value->accept(*this);
    }

    void NameResolver::visitImportExpr(ImportExpr* e) { handleImport(e); }

    void NameResolver::visitLambdaExpr(LambdaExpr* e) {
        if (!e->name.empty()) declare(e->name, UserSymbol::Function, e->startPos, e->startPos + (int)e->name.size());
        Range range;
        range.start = doc->offsetToPosition(e->startPos);
        range.end = doc->offsetToPosition(e->endPos);
        enterScope(current, range, true);
        for (auto& p : e->params) declare(p.lexeme, UserSymbol::Parameter, p.position, p.position + (int)p.lexeme.size());
        for (auto& p : e->kwargParams) declare(p.lexeme, UserSymbol::Parameter, p.position, p.position + (int)p.lexeme.size());
        if (!e->restName.empty()) declare(e->restName, UserSymbol::Parameter, e->startPos, e->startPos);
        if (e->body) e->body->accept(*this);
        leaveScope();
    }

    void NameResolver::visitClassDefExpr(ClassDefExpr* e) {
        Range range;
        range.start = doc->offsetToPosition(e->startPos);
        range.end = doc->offsetToPosition(e->endPos);
        enterScope(current, range, false, false, true);
        for (auto& p : e->staticProperties) {
            if (p.value) p.value->accept(*this);
            declare(p.name.lexeme, UserSymbol::Property, p.name.position, p.name.position + (int)p.name.lexeme.size());
        }
        for (auto& p : e->instanceProperties) {
            if (p.value) p.value->accept(*this);
            declare(p.name.lexeme, UserSymbol::Property, p.name.position, p.name.position + (int)p.name.lexeme.size());
        }
        leaveScope();
    }

    void NameResolver::visitNamespaceDecl(NamespaceDecl* e) {
        Range range;
        range.start = doc->offsetToPosition(e->startPos);
        range.end = doc->offsetToPosition(e->endPos);
        enterScope(current, range, false, true);
        if (e->body) e->body->accept(*this);
        leaveScope();
    }

    void NameResolver::visitEnumDefExpr(EnumDefExpr* e) {
        for (auto& m : e->members) {
            if (m.second) m.second->accept(*this);
        }
    }

    // ---------- 简单遍历 ----------
    void NameResolver::visitBinary(Binary* e) {
        if (e->left) e->left->accept(*this);
        if (e->right) e->right->accept(*this);
    }
    void NameResolver::visitUnary(Unary* e) {
        if (e->right) e->right->accept(*this);
    }
    void NameResolver::visitMatrixNode(MatrixNode* e) {
        for (auto& row : e->elements) for (auto& c : row) if (c) c->accept(*this);
    }
    void NameResolver::visitListNode(ListNode* e) {
        for (auto& row : e->elements) for (auto& c : row) if (c) c->accept(*this);
    }
    void NameResolver::visitIfExpr(IfExpr* e) {
        if (e->condition) e->condition->accept(*this);
        if (e->thenBranch) e->thenBranch->accept(*this);
        if (e->elseBranch) e->elseBranch->accept(*this);
    }
    void NameResolver::visitWhileExpr(WhileExpr* e) {
        if (e->condition) e->condition->accept(*this);
        if (e->body) e->body->accept(*this);
    }
    void NameResolver::visitForExpr(ForExpr* e) {
        Range range;
        range.start = doc->offsetToPosition(e->startPos);
        range.end = doc->offsetToPosition(e->endPos);
        enterScope(current, range);
        if (e->initializer) e->initializer->accept(*this);
        if (e->condition) e->condition->accept(*this);
        if (e->update) e->update->accept(*this);
        if (e->body) e->body->accept(*this);
        leaveScope();
    }
    void NameResolver::visitReturnExpr(ReturnExpr* e) {
        if (e->value) e->value->accept(*this);
    }
    void NameResolver::visitIndexAccess(IndexAccess* e) {
        if (e->object) e->object->accept(*this);
        for (auto& idx : e->indices) if (idx) idx->accept(*this);
    }
    void NameResolver::visitIndexAssign(IndexAssign* e) {
        if (e->objectExpr) e->objectExpr->accept(*this);
        for (auto& chain : e->indexChain) for (auto& idx : chain) if (idx) idx->accept(*this);
        if (e->value) e->value->accept(*this);
    }
    void NameResolver::visitLocalDecl(LocalDecl* e) {
        declare(e->name.lexeme, UserSymbol::Variable, e->name.position, e->name.position + (int)e->name.lexeme.size());
    }
    void NameResolver::visitRefDecl(RefDecl* e) {
        declare(e->name.lexeme, UserSymbol::Variable, e->name.position, e->name.position + (int)e->name.lexeme.size());
    }
    void NameResolver::visitStateDecl(StateDecl* e) {
        declare(e->name.lexeme, UserSymbol::Variable, e->name.position, e->name.position + (int)e->name.lexeme.size());
    }
    void NameResolver::visitConstDecl(ConstDecl* e) {
        declare(e->name.lexeme, UserSymbol::Variable, e->name.position, e->name.position + (int)e->name.lexeme.size());
    }
    void NameResolver::visitCompoundAssign(CompoundAssign* e) {
        if (e->target) e->target->accept(*this);
        if (e->value) e->value->accept(*this);
    }
    void NameResolver::visitInvokeExpr(InvokeExpr* e) {
        if (e->callee) e->callee->accept(*this);
        for (auto& arg : e->arguments) if (arg) arg->accept(*this);
    }
    void NameResolver::visitForInExpr(ForInExpr* e) {
        Range range;
        range.start = doc->offsetToPosition(e->startPos);
        range.end = doc->offsetToPosition(e->endPos);
        enterScope(current, range);
        if (e->iterable) e->iterable->accept(*this);
        if (e->body) e->body->accept(*this);
        leaveScope();
    }
    void NameResolver::visitThrowExpr(ThrowExpr* e) {
        if (e->value) e->value->accept(*this);
    }
    void NameResolver::visitTryCatchExpr(TryCatchExpr* e) {
        if (e->tryBody) e->tryBody->accept(*this);
        if (e->catchBody) e->catchBody->accept(*this);
    }
    void NameResolver::visitSwitchExpr(SwitchExpr* e) {
        if (e->subject) e->subject->accept(*this);
        for (auto& c : e->cases) {
            for (auto& v : c.first) if (v) v->accept(*this);
            if (c.second) c.second->accept(*this);
        }
        if (e->defaultBody) e->defaultBody->accept(*this);
    }
    void NameResolver::visitDestructAssign(DestructAssign* e) {
        if (e->value) e->value->accept(*this);
    }
    void NameResolver::visitFStringExpr(FStringExpr* e) {
        for (auto& x : e->exprs) if (x) x->accept(*this);
    }
    void NameResolver::visitMatrixCompExpr(MatrixCompExpr* e) {
        Range range;
        range.start = doc->offsetToPosition(e->startPos);
        range.end = doc->offsetToPosition(e->endPos);
        enterScope(current, range);
        for (auto& c : e->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (e->valueExpr) e->valueExpr->accept(*this);
        leaveScope();
    }
    void NameResolver::visitListCompExpr(ListCompExpr* e) {
        Range range;
        range.start = doc->offsetToPosition(e->startPos);
        range.end = doc->offsetToPosition(e->endPos);
        enterScope(current, range);
        for (auto& c : e->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (e->valueExpr) e->valueExpr->accept(*this);
        leaveScope();
    }
    void NameResolver::visitSetCompExpr(SetCompExpr* e) {
        Range range;
        range.start = doc->offsetToPosition(e->startPos);
        range.end = doc->offsetToPosition(e->endPos);
        enterScope(current, range);
        for (auto& c : e->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (e->valueExpr) e->valueExpr->accept(*this);
        leaveScope();
    }
    void NameResolver::visitDictCompExpr(DictCompExpr* e) {
        Range range;
        range.start = doc->offsetToPosition(e->startPos);
        range.end = doc->offsetToPosition(e->endPos);
        enterScope(current, range);
        for (auto& c : e->clauses) {
            if (c.iterable) c.iterable->accept(*this);
            for (auto& cond : c.conditions) if (cond) cond->accept(*this);
        }
        if (e->keyExpr) e->keyExpr->accept(*this);
        if (e->valueExpr) e->valueExpr->accept(*this);
        leaveScope();
    }
    void NameResolver::visitDictLiteral(DictLiteral* e) {
        for (auto& p : e->entries) {
            if (p.first) p.first->accept(*this);
            if (p.second) p.second->accept(*this);
        }
    }
    void NameResolver::visitSetLiteral(SetLiteral* e) {
        for (auto& x : e->elements) if (x) x->accept(*this);
    }
    void NameResolver::visitSliceExpr(SliceExpr* e) {
        if (e->start) e->start->accept(*this);
        if (e->end) e->end->accept(*this);
        if (e->step) e->step->accept(*this);
    }
    void NameResolver::visitSequenceExpr(SequenceExpr* e) {
        for (auto& x : e->expressions) if (x) x->accept(*this);
    }
    void NameResolver::visitMatchExpr(MatchExpr* e) {
        if (e->subject) e->subject->accept(*this);
        for (auto& b : e->branches) {
            if (b.guard) b.guard->accept(*this);
            if (b.body) b.body->accept(*this);
        }
    }
    void NameResolver::visitGroupingExpr(GroupingExpr* e) {
        if (e->expression) e->expression->accept(*this);
    }
    void NameResolver::visitMacroDefExpr(MacroDefExpr* e) {
        declare(e->name.lexeme, UserSymbol::Function, e->name.position, e->name.position + (int)e->name.lexeme.size());
        Range range;
        range.start = doc->offsetToPosition(e->startPos);
        range.end = doc->offsetToPosition(e->endPos);
        enterScope(current, range, true);
        for (auto& p : e->params) declare(p.lexeme, UserSymbol::Parameter, p.position, p.position + (int)p.lexeme.size());
        if (e->body) e->body->accept(*this);
        leaveScope();
    }
    void NameResolver::visitExprAssign(ExprAssign* e) {
        if (e->target) e->target->accept(*this);
        if (e->value) e->value->accept(*this);
    }
    void NameResolver::visitDeferExpr(DeferExpr* e) {
        if (e->body) e->body->accept(*this);
    }
    void NameResolver::visitKeywordArgExpr(KeywordArgExpr* e) {
        if (e->value) e->value->accept(*this);
    }
    void NameResolver::visitSpreadExpr(SpreadExpr* e) {
        if (e->value) e->value->accept(*this);
    }
    void NameResolver::visitTypeAssertExpr(TypeAssertExpr* e) {
        if (e->value) e->value->accept(*this);
        if (e->typeHint) e->typeHint->accept(*this);
    }

} // namespace lsp
} // namespace jc
