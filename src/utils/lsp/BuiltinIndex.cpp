#include "BuiltinIndex.h"
#include "../../vm/VM.h"
#include "../../vm/HelpRouter.h"
#include "../../memory/Value.h"
#include "../../utils/json/Json.h"

namespace jc {
namespace lsp {

    // ---------- 文档读取辅助 ----------
    static std::string docStr(const Json& entry, const char* key) {
        if (!entry.isObject() || !entry.has(key)) return "";
        const Json& v = entry[key];
        if (v.isString()) return v.strVal;
        if (v.isArray() && !v.arrVal.empty() && v.arrVal[0].isString()) return v.arrVal[0].strVal;
        return "";
    }

    static std::vector<std::string> docAliases(const Json& entry) {
        std::vector<std::string> aliases;
        if (!entry.isObject() || !entry.has("aliases") || !entry["aliases"].isArray()) return aliases;
        for (const auto& a : entry["aliases"].arrVal) {
            if (a.isString()) aliases.push_back(a.strVal);
        }
        return aliases;
    }

    BuiltinIndex::BuiltinIndex() {
        buildGlobals();
        buildModules();
        buildMethods();
        buildTypes();
        buildKeywords();
    }

    void BuiltinIndex::buildGlobals() {
        HelpRouter::init();
        const Json& helpAst = HelpRouter::helpAst;
        const Json* funcsNode = nullptr;
        if (helpAst.isObject() && helpAst.has("global_functions")) funcsNode = &helpAst["global_functions"];

        if (!VM::activeVM) return;
        const auto& arities = VM::activeVM->getBuiltinArity();
        const auto& paramNames = VM::activeVM->getBuiltinParamNames();
        const auto& restNames = VM::activeVM->getBuiltinRestName();
        const auto& kwargNames = VM::activeVM->getBuiltinKwargNames();
        const auto& kwargsNames = VM::activeVM->getBuiltinKwargsName();
        const auto& kwargDefaults = VM::activeVM->getBuiltinKwargDefaultCount();

        for (const auto& [name, arity] : arities) {
            BuiltinSymbol sym;
            sym.name = name;
            sym.kind = BuiltinKind::Function;
            sym.arity = arity;
            auto itP = paramNames.find(name);
            if (itP != paramNames.end()) sym.paramNames = itP->second;
            auto itR = restNames.find(name);
            if (itR != restNames.end()) sym.restName = itR->second;
            auto itK = kwargNames.find(name);
            if (itK != kwargNames.end()) sym.kwargNames = itK->second;
            auto itKs = kwargsNames.find(name);
            if (itKs != kwargsNames.end()) sym.kwargsName = itKs->second;
            auto itD = kwargDefaults.find(name);
            if (itD != kwargDefaults.end()) sym.kwargDefaultCount = itD->second;

            if (funcsNode && funcsNode->isObject() && funcsNode->has(name)) {
                const Json& e = (*funcsNode)[name];
                sym.signatureText = docStr(e, "signature");
                sym.desc = docStr(e, "desc");
                for (auto& alias : docAliases(e)) {
                    BuiltinSymbol aliasSym = sym;
                    aliasSym.name = alias;
                    globals[alias] = std::move(aliasSym);
                }
            }
            globals[name] = std::move(sym);
        }
    }

    void BuiltinIndex::buildModules() {
        HelpRouter::init();
        const Json& helpAst = HelpRouter::helpAst;

        // 模块名 → documentation.json 分类
        static const std::unordered_map<std::string, std::string> modToCat = {
            {"sys", "sys_methods"}, {"math", "math_methods"},
            {"cas", "cas_methods"}, {"random", "random_methods"}
        };

        if (!VM::activeVM) return;
        const auto& modules = VM::activeVM->getBuiltinModules();

        auto readDoc = [&](const std::string& mod, const std::string& fnName) -> std::pair<std::string, std::string> {
            auto itC = modToCat.find(mod);
            if (itC == modToCat.end()) return {};
            if (!helpAst.isObject() || !helpAst.has(itC->second)) return {};
            const Json& cat = helpAst[itC->second];
            if (!cat.isObject() || !cat.has(fnName)) return {};
            const Json& e = cat[fnName];
            return { docStr(e, "signature"), docStr(e, "desc") };
        };

        for (const auto& [modName, modVal] : modules) {
            if (!modVal.isObjType(ObjType::NAMESPACE)) continue;
            auto* ns = static_cast<ObjNamespace*>(modVal.asObj());
            for (const auto& [fnName, field] : ns->fields) {
                if (!field.upval) continue;
                Value fnVal = field.upval->closed;
                if (!fnVal.isFunctionClosure()) continue;
                auto* cl = fnVal.asFunction();

                BuiltinSymbol sym;
                sym.name = fnName;
                sym.kind = BuiltinKind::ModuleMember;
                sym.owner = modName;
                int minA = cl->minArgs();
                int maxA = cl->maxArgs();
                for (int i = minA; i <= maxA; ++i) sym.arity.insert(i);
                sym.paramNames = cl->paramNames;
                if (!cl->restName.empty()) sym.restName = cl->restName;
                sym.kwargNames = cl->kwargNames;
                sym.kwargsName = cl->kwargsName;

                auto [sig, desc] = readDoc(modName, fnName);
                sym.signatureText = sig;
                sym.desc = desc;

                moduleMembers[modName][fnName] = std::move(sym);
            }
        }
    }

    void BuiltinIndex::buildMethods() {
        HelpRouter::init();
        const Json& helpAst = HelpRouter::helpAst;

        static const std::unordered_map<std::string, std::string> typeToCat = {
            {"matrix", "matrix_methods"}, {"list", "list_methods"},
            {"string", "string_methods"}, {"dict", "dict_methods"}, {"set", "set_methods"}
        };

        for (const auto& [typeName, catName] : typeToCat) {
            if (!helpAst.isObject() || !helpAst.has(catName)) continue;
            const Json& cat = helpAst[catName];
            if (!cat.isObject()) continue;
            for (const auto& [methodName, entry] : cat.objVal) {
                if (!entry.isObject()) continue;
                BuiltinSymbol sym;
                sym.name = methodName;
                sym.kind = BuiltinKind::Method;
                sym.owner = typeName;
                sym.signatureText = docStr(entry, "signature");
                sym.desc = docStr(entry, "desc");
                methodNameSet.insert(methodName);
                methods[typeName][methodName] = sym;
                for (auto& alias : docAliases(entry)) {
                    methodNameSet.insert(alias);
                    BuiltinSymbol aliasSym = sym;
                    aliasSym.name = alias;
                    methods[typeName][alias] = std::move(aliasSym);
                }
            }
        }
    }

    void BuiltinIndex::buildTypes() {
        // Value.h BuiltinType 枚举名 + complex（去 bigint）。表面名与内部名一并纳入。
        static const std::vector<std::string> names = {
            "any", "int", "double", "string", "bool", "none_type", "list", "dict", "set",
            "fraction", "symbolic", "realmatrix", "complexmatrix", "symmatrix", "function",
            "class_type", "instance", "namespace_type", "custom_class", "type", "slice",
            "complex", "matrix"
        };
        for (const auto& n : names) {
            typeNameSet.insert(n);
            BuiltinSymbol sym;
            sym.name = n;
            sym.kind = BuiltinKind::Type;
            globals[n] = std::move(sym);
        }
    }

    void BuiltinIndex::buildKeywords() {
        HelpRouter::init();
        const Json& helpAst = HelpRouter::helpAst;
        if (helpAst.isObject() && helpAst.has("keywords") && helpAst["keywords"].isObject()) {
            for (const auto& [kw, v] : helpAst["keywords"].objVal) {
                keywordSet.insert(kw);
            }
        }
    }

    const BuiltinSymbol* BuiltinIndex::findGlobal(const std::string& name) const {
        auto it = globals.find(name);
        return it != globals.end() ? &it->second : nullptr;
    }

    const BuiltinSymbol* BuiltinIndex::findModuleMember(const std::string& module, const std::string& name) const {
        auto itM = moduleMembers.find(module);
        if (itM == moduleMembers.end()) return nullptr;
        auto it = itM->second.find(name);
        return it != itM->second.end() ? &it->second : nullptr;
    }

    const BuiltinSymbol* BuiltinIndex::findMethod(const std::string& typeName, const std::string& method) const {
        auto itT = methods.find(typeName);
        if (itT == methods.end()) return nullptr;
        auto it = itT->second.find(method);
        return it != itT->second.end() ? &it->second : nullptr;
    }

    bool BuiltinIndex::isKeyword(const std::string& name) const {
        return keywordSet.count(name) > 0;
    }

    bool BuiltinIndex::isTypeName(const std::string& name) const {
        return typeNameSet.count(name) > 0;
    }

    bool BuiltinIndex::isMethodName(const std::string& name) const {
        return methodNameSet.count(name) > 0;
    }

} // namespace lsp
} // namespace jc
