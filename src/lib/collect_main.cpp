// 符号收集器：离线运行某个 dll 模块的 jc2_init，收集符号到 sidecar json。
// 用法：collect_<module> <moduleName> <outputPath>
// 通过编译宏 JC2_MODULE_SOURCE 指定要 include 的模块源码（相对 src 目录）。

#include "collect_api.h"
#include "jc2_extension_cpp.h"

#ifndef JC2_MODULE_SOURCE
#error "JC2_MODULE_SOURCE must be defined (e.g. \"image/image_module.cpp\")"
#endif
#include JC2_MODULE_SOURCE

#include <string>

int main(int argc, char** argv) {
    if (argc < 3) return 1;
    std::string module = argv[1];
    std::string outPath = argv[2];

    JC2_HostAPI api = jcsym::makeCollectApi();
    jc2::Env::api = &api;
    jc2::Env::ctx = nullptr;

    jc2::Module mod(nullptr);
    int r = jc2_init(mod);
    (void)r;

    jcsym::writeSidecarJson(jcsym::g_collected, module, outPath);
    return 0;
}
