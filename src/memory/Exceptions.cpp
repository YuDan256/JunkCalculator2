// Exceptions.cpp
// 定义异常类指针全局（err::XxxClass）。
// 运行时错误的指针由 PredefinedClasses 注册时赋值；编译期/内部错误恒为 nullptr。

#include "Exceptions.h"

namespace jc {
    namespace err {
        // 运行时错误（PredefinedClasses 注册时赋值）
        ObjClass* TypeErrorClass = nullptr;
        ObjClass* ValueErrorClass = nullptr;
        ObjClass* MathErrorClass = nullptr;
        ObjClass* MatchErrorClass = nullptr;
        ObjClass* IOErrorClass = nullptr;
        ObjClass* RuntimeErrorClass = nullptr;
        ObjClass* OverflowErrorClass = nullptr;
        ObjClass* CalculusErrorClass = nullptr;
        ObjClass* SymbolicErrorClass = nullptr;
        // 编译期/内部错误（恒为 nullptr，不进运行时类注册）
        ObjClass* SyntaxErrorClass = nullptr;
        ObjClass* ParserErrorClass = nullptr;
        ObjClass* LexerErrorClass = nullptr;
        ObjClass* EmitterErrorClass = nullptr;
        ObjClass* InternalErrorClass = nullptr;
    } // namespace err
} // namespace jc
