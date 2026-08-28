#ifndef JC2_BYTECODE_SERIALIZER_H
#define JC2_BYTECODE_SERIALIZER_H

#include "Bytecode.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_set>

namespace jc {

class VM;

class BytecodeSerializer {
public:
    static constexpr uint32_t MAGIC_NUMBER = 0x4A434202; // JCB2
    static constexpr uint32_t VERSION = 4;

    // 将一组编译好的函数序列化为 .jcb 文件
    static void saveJCB(const std::string& path, VM* vm, int startIndex, int count, bool stripDebug = false);
    
    // 从 .jcb 文件反序列化，追加到 VM，并返回顶层模块函数
    static std::shared_ptr<CompiledFunction> loadJCB(const std::string& path, VM* vm);

private:
    enum class ConstTag : uint8_t {
        NONE = 0, BOOL_FALSE, BOOL_TRUE, INT32, DOUBLE, STRING,
        BIGINT, FRACTION, COMPLEX,
        REAL_MATRIX, COMPLEX_MATRIX,
        NAMESPACE, FNIDX, UNINIT
    };

    static void write8(std::ostream& os, uint8_t v);
    static void write16(std::ostream& os, uint16_t v);
    static void write32(std::ostream& os, uint32_t v);
    static void write64(std::ostream& os, uint64_t v);
    static void writeDouble(std::ostream& os, double v);
    static void writeString(std::ostream& os, const std::string& s);

    static uint8_t read8(std::istream& is);
    static uint16_t read16(std::istream& is);
    static uint32_t read32(std::istream& is);
    static uint64_t read64(std::istream& is);
    static double readDouble(std::istream& is);
    static std::string readString(std::istream& is);

    static void writeValue(std::ostream& os, const Value& val, const std::unordered_set<int>& fnIdxConstants, int constIdx, int startIndex);
    static Value readValue(std::istream& is, int baseIdx);

    static void writeChunk(std::ostream& os, const Chunk& chunk, int startIndex, bool stripDebug);
    static void readChunk(std::istream& is, Chunk& chunk, int baseIdx);

    static void writeFunction(std::ostream& os, const CompiledFunction* fn, int startIndex, bool stripDebug);
    static void readFunction(std::istream& is, CompiledFunction* fn, int baseIdx);
};

} // namespace jc

#endif // JC2_BYTECODE_SERIALIZER_H
