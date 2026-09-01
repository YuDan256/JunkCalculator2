#ifndef JC2_LSP_BUILTIN_INDEX_H
#define JC2_LSP_BUILTIN_INDEX_H

#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace jc {
namespace lsp {

    // 内置符号类别
    enum class BuiltinKind {
        Type,          // 类型对象（int/double/matrix/...），callable
        Function,      // 全局内置函数（sin/print/...）
        Module,        // 模块（sys/cas/math/random）
        ModuleMember,  // 模块成员函数（sys.xxx）
        Method,        // 方法（A.row()/v.push()）
        Keyword        // 关键字
    };

    // 单个内置符号的完整信息
    struct BuiltinSymbol {
        std::string name;                 // 名字（模块成员/方法用短名）
        BuiltinKind kind = BuiltinKind::Function;
        std::set<int> arity;              // 精确参数个数集合（BuiltinRegistry 权威）
        std::vector<std::string> paramNames;
        std::string restName;             // 空 = 无 rest
        std::vector<std::string> kwargNames;
        std::string kwargsName;           // 空 = 无 **kwargs
        int kwargDefaultCount = 0;
        std::string signatureText;        // 文档签名文本（hover 用）
        std::string desc;
        std::string owner;                // 所属模块名 / 类型名，空 = 全局
    };

    // 内置符号库：单一权威索引，从 BuiltinRegistry(VM) + documentation.json 合成
    class BuiltinIndex {
    public:
        BuiltinIndex();

        // 查询
        const BuiltinSymbol* findGlobal(const std::string& name) const;
        const BuiltinSymbol* findModuleMember(const std::string& module, const std::string& name) const;
        const BuiltinSymbol* findMethod(const std::string& typeName, const std::string& method) const;
        bool isKeyword(const std::string& name) const;
        bool isTypeName(const std::string& name) const;
        bool isMethodName(const std::string& name) const;              // 任意类型的方法名
        const std::unordered_set<std::string>& keywords() const { return keywordSet; }
        const std::unordered_set<std::string>& typeNames() const { return typeNameSet; }
        const std::unordered_set<std::string>& methodNames() const { return methodNameSet; }
        const std::unordered_map<std::string, BuiltinSymbol>& allGlobals() const { return globals; }

    private:
        std::unordered_map<std::string, BuiltinSymbol> globals;        // 全局函数 + 类型（含别名）
        std::unordered_map<std::string, std::unordered_map<std::string, BuiltinSymbol>> moduleMembers; // module -> name -> sym
        std::unordered_map<std::string, std::unordered_map<std::string, BuiltinSymbol>> methods;       // type -> method -> sym
        std::unordered_set<std::string> keywordSet;
        std::unordered_set<std::string> typeNameSet;
        std::unordered_set<std::string> methodNameSet;

        void buildGlobals();    // VM 精确签名 + helpAst 文档
        void buildModules();    // VM builtinModules（namespace 闭包）+ helpAst 文档
        void buildMethods();    // helpAst matrix/list/string/dict/set_methods
        void buildTypes();      // Value.h BuiltinType 枚举名 + complex（去 bigint）
        void buildKeywords();   // helpAst keywords
    };

} // namespace lsp
} // namespace jc

#endif // JC2_LSP_BUILTIN_INDEX_H
