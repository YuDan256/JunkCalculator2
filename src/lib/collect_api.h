#ifndef JC2_SYMBOL_COLLECT_API_H
#define JC2_SYMBOL_COLLECT_API_H

#include "jc2_extension_api.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>

// 符号收集器：构建时离线运行 dll 模块的 jc2_init，收集符号到 sidecar json。
// 不 LoadLibrary、不触发 dll 副作用——只有「注册」类 host API 是真实收集，其余是安全桩。

namespace jcsym {

    struct Func {
        std::string name;
        int minArity = 0;
        int maxArity = 0;
        std::vector<std::string> params;
        std::string restName;
        std::vector<std::string> kwargNames;
        std::string kwargsName;
        std::string returnType;  // 返回类型名（空 = any）
    };

    struct Class {
        std::string name;
        std::vector<Func> methods;
    };

    struct Help {
        std::string name;        // 带前缀名，如 "image.Image"
        std::string signature;
        std::string desc;
    };

    struct Constant {
        std::string name;
        std::string type;        // "int" / "double" / "string" / "value"
        std::string value;
    };

    struct Collected {
        std::string module;
        std::vector<Func> functions;      // 模块函数
        std::vector<Class> classes;       // 类 + 方法
        std::vector<Help> help;           // help 条目
        std::vector<Constant> constants;  // 常量
        std::map<uint64_t, std::string> handleToClass;
        uint64_t nextHandle = 1;
    };

    // 全局收集器单例（host API 函数指针不能捕获，只能访问全局）
    extern Collected g_collected;

    // 构建「只收集」的 host API
    JC2_HostAPI makeCollectApi();

    // 把收集结果写成 sidecar json
    void writeSidecarJson(const Collected& c, const std::string& module, const std::string& outPath);

} // namespace jcsym

#endif // JC2_SYMBOL_COLLECT_API_H
