// RFC 6125 主机名匹配（只做 dNSName，不做 CN 兜底）。
#pragma once

#include <string>
#include <vector>

namespace jc2::crypto {

// 单个 dNSName 模式与主机名匹配：大小写不敏感；只允许最左标签是通配符，
// 且至少要有三个标签（*.example.com 可以，*.com 不可以）。
bool hostnameMatch(const std::string& pattern, const std::string& host);

// 任一 SAN 命中即算匹配。
bool hostnameMatchAny(const std::vector<std::string>& patterns, const std::string& host);

} // namespace jc2::crypto
