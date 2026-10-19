#pragma once

#include <string>
#include <vector>

namespace jc {

// ★ 单一版本来源：main.cpp 的 version 输出与 sys.version() 共用。
inline constexpr const char* VERSION = "2.6.4.0";
inline constexpr const char* VERSION_FULL = "Junk Calculator 2.6.4.0";

// ★ `jc2 run script.jc2 -- a b` 中 `--` 之后的参数；由 main 填充，sys.args() 读取。
inline std::vector<std::string> g_scriptArgs;

} // namespace jc
