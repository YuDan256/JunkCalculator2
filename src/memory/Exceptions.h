#ifndef JC2_EXCEPTIONS_H
#define JC2_EXCEPTIONS_H

// 统一异常基础设施（独立于 Value，供任何需要抛类型化错误的文件 include）：
//   - err:: 类型常量表
//   - Jc2Error 轻量异常（type + message 都是 std::string）
//   - JC2_THROW 宏（类型与消息显式分离）
//   - err* 重复错误 helper

#include <string>
#include <stdexcept>

namespace jc {

    // ★ 统一异常类型标签（显式类型化，取代字符串前缀编码）
    namespace err {
        // 运行时错误（可被 try/catch 捕获）
        constexpr const char* TypeError = "TypeError";
        constexpr const char* ValueError = "ValueError";
        constexpr const char* MathError = "MathError";
        constexpr const char* MatchError = "MatchError";
        constexpr const char* IOError = "IOError";
        constexpr const char* RuntimeError = "RuntimeError";
        constexpr const char* OverflowError = "OverflowError";
        constexpr const char* FFIError = "FFIError";
        constexpr const char* TensorError = "TensorError";
        constexpr const char* CalculusError = "CalculusError";
        constexpr const char* SymbolicError = "SymbolicError";
        // 编译期错误（不进运行时）
        constexpr const char* SyntaxError = "SyntaxError";
        constexpr const char* ParserError = "ParserError";
        constexpr const char* LexerError = "LexerError";
        constexpr const char* EmitterError = "EmitterError";
        // 内部错误（不变量破坏，用户不该触发）
        constexpr const char* InternalError = "InternalError";
    }

    // ★ 轻量异常：type + message 都是 std::string，不依赖 Value 完整定义，
    // 因此任何文件（含 Value 类的内联方法）都能抛出；VM 层 catch 后包装成 Exception 对象。
    struct Jc2Error : public std::runtime_error {
        std::string type;
        std::string message;
        mutable std::string whatBuffer;
        Jc2Error(std::string t, std::string msg) : std::runtime_error(msg), type(std::move(t)), message(std::move(msg)) {}
        const char* what() const noexcept override {
            if (whatBuffer.empty()) {
                whatBuffer = type.empty() ? message : type + ": " + message;
            }
            return whatBuffer.c_str();
        }
    };

    // ★ 统一抛出入口：类型与消息显式分离，替代 throw std::runtime_error("前缀: 消息")
    #define JC2_THROW(T, msg) throw ::jc::Jc2Error(::jc::err::T, (msg))

    // ★ 重复错误 helper：消除同一错误在多处重复抛出（后续新代码也直接复用）
    [[noreturn]] inline void errDivByZero() { JC2_THROW(MathError, "Division by zero."); }
    [[noreturn]] inline void errModByZero() { JC2_THROW(MathError, "Modulo by zero."); }
    [[noreturn]] inline void errMatScalarAddSquare() { JC2_THROW(MathError, "Matrix-scalar addition requires a square matrix."); }
    [[noreturn]] inline void errMatScalarSubSquare() { JC2_THROW(MathError, "Matrix-scalar subtraction requires a square matrix."); }
    [[noreturn]] inline void errNegShift() { JC2_THROW(MathError, "Negative shift count."); }
    [[noreturn]] inline void errExpectMatrix() { JC2_THROW(TypeError, "Expected a matrix."); }
    [[noreturn]] inline void errKeyNotFound(const std::string& key) { JC2_THROW(RuntimeError, "Key '" + key + "' not found."); }
    [[noreturn]] inline void errIndexAbsTooLarge() { JC2_THROW(ValueError, "Index absolute value exceeds 2^31-1."); }
    [[noreturn]] inline void errStrRepeatNeg() { JC2_THROW(TypeError, "String repeat count must be non-negative."); }
    [[noreturn]] inline void errUnhashable() { JC2_THROW(TypeError, "unhashable type."); }
    [[noreturn]] inline void errDeleteConstProp(const std::string& key) { JC2_THROW(RuntimeError, "Cannot delete const property '" + key + "'."); }
    [[noreturn]] inline void errModifyConstProp(const std::string& key) { JC2_THROW(RuntimeError, "Cannot modify const property '" + key + "'."); }
    [[noreturn]] inline void errModifyPrivateProp(const std::string& key) { JC2_THROW(RuntimeError, "Cannot modify private property '" + key + "'."); }
    [[noreturn]] inline void errAccessPrivateDynamic() { JC2_THROW(RuntimeError, "Cannot access private or lifecycle properties dynamically."); }
    [[noreturn]] inline void errMatImmutableSetItem() { JC2_THROW(RuntimeError, "Matrices are immutable. Use setItem(i, x) / setSlice(sr, sc, x) to get a new matrix."); }
    [[noreturn]] inline void errMatImmutableSetElement() { JC2_THROW(RuntimeError, "Matrices are immutable. Use setElement(r, c, x) / setSlice(sr, sc, x) to get a new matrix."); }
    [[noreturn]] inline void errAccessPrivateOutsideClass() { JC2_THROW(RuntimeError, "Cannot access private property outside of class context."); }
    [[noreturn]] inline void errCallFrameOverflow() { JC2_THROW(RuntimeError, "CallFrame stack overflow."); }
    [[noreturn]] inline void errMatrixElementType() { JC2_THROW(RuntimeError, "Matrix elements must be numeric, complex, or symbolic. Use @[...] for lists."); }
    [[noreturn]] inline void errInvalidRefParamIndex() { JC2_THROW(RuntimeError, "Invalid ref param index."); }
    [[noreturn]] inline void errInvalidUpvalueIndex() { JC2_THROW(RuntimeError, "Invalid upvalue index."); }
    [[noreturn]] inline void errSetPrivateOnType() { JC2_THROW(RuntimeError, "Cannot set private property on this type."); }
    [[noreturn]] inline void errUnsupportedIndexDim() { JC2_THROW(RuntimeError, "Unsupported index dimensionality."); }
    [[noreturn]] inline void errSuperInstanceContext() { JC2_THROW(RuntimeError, "'super' requires an instance context."); }
    [[noreturn]] inline void errSuperClassContext() { JC2_THROW(RuntimeError, "'super' requires class context."); }
    [[noreturn]] inline void errNoParentClass() { JC2_THROW(RuntimeError, "No parent class."); }
    [[noreturn]] inline void errUndefinedGlobal(const std::string& name) { JC2_THROW(RuntimeError, "Undefined global variable '" + name + "'."); }
    [[noreturn]] inline void errRequiresAtLeast(const std::string& name, const std::string& arity) { JC2_THROW(RuntimeError, "'" + name + "' requires at least " + arity + " arguments."); }
    [[noreturn]] inline void errExpectsAtMost(const std::string& name, const std::string& arity) { JC2_THROW(RuntimeError, "'" + name + "' expects at most " + arity + " arguments."); }
    [[noreturn]] inline void errCannotIterate() { JC2_THROW(RuntimeError, "Cannot iterate over this type."); }
    [[noreturn]] inline void errTargetNotCallable() { JC2_THROW(RuntimeError, "Target is not callable."); }

} // namespace jc

#endif // JC2_EXCEPTIONS_H
