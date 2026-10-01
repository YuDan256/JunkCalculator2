#include "Hostname.h"

#include <algorithm>
#include <cctype>

namespace jc2::crypto {

namespace {

std::string lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

// 去掉末尾的一个点（FQDN 写法），并转小写。
std::string normalize(const std::string& s) {
    std::string out = lower(s);
    if (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

size_t labelCount(const std::string& s) {
    if (s.empty()) return 0;
    return static_cast<size_t>(std::count(s.begin(), s.end(), '.')) + 1;
}

} // namespace

bool hostnameMatch(const std::string& pattern, const std::string& host) {
    const std::string p = normalize(pattern);
    const std::string h = normalize(host);
    if (p.empty() || h.empty()) return false;

    const size_t star = p.find('*');
    if (star == std::string::npos) {
        return p == h;
    }

    // 通配符只能出现在最左标签，且必须是整个标签
    if (star != 0 || p.size() < 2 || p[1] != '.') return false;
    if (p.find('*', 1) != std::string::npos) return false;
    // *.example.com 至少三个标签
    if (labelCount(p) < 3) return false;

    const std::string suffix = p.substr(1);   // ".example.com"
    if (h.size() <= suffix.size()) return false;
    if (h.compare(h.size() - suffix.size(), suffix.size(), suffix) != 0) return false;

    // 通配符只吃一个标签
    const std::string prefix = h.substr(0, h.size() - suffix.size());
    if (prefix.empty()) return false;
    return prefix.find('.') == std::string::npos;
}

bool hostnameMatchAny(const std::vector<std::string>& patterns, const std::string& host) {
    for (const auto& p : patterns) {
        if (hostnameMatch(p, host)) return true;
    }
    return false;
}

} // namespace jc2::crypto
