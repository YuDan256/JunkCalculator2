#ifndef JC2_VM_EXCEPTION_H
#define JC2_VM_EXCEPTION_H

// 解释器内部的值异常（`throw expr` / `ValueException` 包装）。
// 单独成文件是为了让 BuiltinRegistry.h 也能用，而不会反向依赖 VM.h（与 VM.h 循环包含）。

#include "../memory/Value.h"
#include <exception>
#include <string>
#include <utility>

namespace jc {

struct ValueException : public std::exception {
    Value val;
    mutable std::string whatBuffer;
    explicit ValueException(Value v) : val(std::move(v)) {}
    const char* what() const noexcept override {
        if (whatBuffer.empty()) {
            whatBuffer = val.isString() ? val.asString() : val.toString();
        }
        return whatBuffer.c_str();
    }
};

} // namespace jc

#endif // JC2_VM_EXCEPTION_H
