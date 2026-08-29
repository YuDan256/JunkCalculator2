#ifndef JC2_FORMATTER_H
#define JC2_FORMATTER_H

#include <string>

namespace jc {

    class Formatter {
    public:
        // 格式化 JC2 源代码
        // 自动处理缩进、空格、空行压缩，并完美保留注释
        static std::string format(const std::string& source);
    };

} // namespace jc

#endif // JC2_FORMATTER_H
