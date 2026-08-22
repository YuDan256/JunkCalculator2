#include "BytecodeSerializer.h"
#include "VM.h"
#include "../jit/frontend/BytecodeCFG.h"
#include <fstream>
#include <cstring>
#include <stdexcept>

namespace jc {

void BytecodeSerializer::write8(std::ostream& os, uint8_t v) { os.put(static_cast<char>(v)); }
void BytecodeSerializer::write16(std::ostream& os, uint16_t v) {
    uint8_t buf[2] = { static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF) };
    os.write(reinterpret_cast<char*>(buf), 2);
}
void BytecodeSerializer::write32(std::ostream& os, uint32_t v) {
    uint8_t buf[4] = {
        static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
        static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 24) & 0xFF)
    };
    os.write(reinterpret_cast<char*>(buf), 4);
}
void BytecodeSerializer::write64(std::ostream& os, uint64_t v) {
    uint8_t buf[8];
    for (int i = 0; i < 8; ++i) buf[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
    os.write(reinterpret_cast<char*>(buf), 8);
}
void BytecodeSerializer::writeDouble(std::ostream& os, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    write64(os, bits);
}
void BytecodeSerializer::writeString(std::ostream& os, const std::string& s) {
    write32(os, static_cast<uint32_t>(s.size()));
    os.write(s.data(), s.size());
}

uint8_t BytecodeSerializer::read8(std::istream& is) { return static_cast<uint8_t>(is.get()); }
uint16_t BytecodeSerializer::read16(std::istream& is) {
    uint8_t buf[2];
    if (!is.read(reinterpret_cast<char*>(buf), 2)) throw std::runtime_error("JCB Read Error");
    return buf[0] | (buf[1] << 8);
}
uint32_t BytecodeSerializer::read32(std::istream& is) {
    uint8_t buf[4];
    if (!is.read(reinterpret_cast<char*>(buf), 4)) throw std::runtime_error("JCB Read Error");
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}
uint64_t BytecodeSerializer::read64(std::istream& is) {
    uint8_t buf[8];
    if (!is.read(reinterpret_cast<char*>(buf), 8)) throw std::runtime_error("JCB Read Error");
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(buf[i]) << (i * 8);
    return v;
}
double BytecodeSerializer::readDouble(std::istream& is) {
    uint64_t bits = read64(is);
    double v;
    std::memcpy(&v, &bits, 8);
    return v;
}
std::string BytecodeSerializer::readString(std::istream& is) {
    uint32_t len = read32(is);
    std::string s(len, '\0');
    if (len > 0 && !is.read(s.data(), len)) throw std::runtime_error("JCB Read Error");
    return s;
}

void BytecodeSerializer::writeValue(std::ostream& os, const Value& val, const std::unordered_set<int>& fnIdxConstants, int constIdx, int startIndex) {
    if (fnIdxConstants.count(constIdx)) {
        write8(os, static_cast<uint8_t>(ConstTag::FNIDX));
        write32(os, static_cast<uint32_t>(val.asDouble() - startIndex));
        return;
    }
    if (val.isNone()) { write8(os, static_cast<uint8_t>(ConstTag::NONE)); return; }
    if (val.isBool()) { write8(os, static_cast<uint8_t>(val.asBool() ? ConstTag::BOOL_TRUE : ConstTag::BOOL_FALSE)); return; }
    if (val.isInt32()) { write8(os, static_cast<uint8_t>(ConstTag::INT32)); write32(os, val.asInt32()); return; }
    if (val.isDouble()) { write8(os, static_cast<uint8_t>(ConstTag::DOUBLE)); writeDouble(os, val.asDoubleRaw()); return; }
    if (val.isString()) {
        write8(os, static_cast<uint8_t>(ConstTag::STRING));
        writeString(os, val.asString());
        return;
    }
    if (val.isBigInt()) {
        write8(os, static_cast<uint8_t>(ConstTag::BIGINT));
        BigInt b = val.asBigInt();
        write8(os, b.getSign() ? 1 : 0);
        const auto& raw = b.getRawData();
        write32(os, static_cast<uint32_t>(raw.size()));
        for (uint32_t v : raw) write32(os, v);
        return;
    }
    if (val.isObjType(ObjType::FRACTION)) {
        write8(os, static_cast<uint8_t>(ConstTag::FRACTION));
        Fraction f = static_cast<ObjFraction*>(val.asObj())->frac;
        write8(os, f.getNum().getSign() ? 1 : 0);
        const auto& numRaw = f.getNum().getRawData();
        write32(os, static_cast<uint32_t>(numRaw.size()));
        for (uint32_t v : numRaw) write32(os, v);
        write8(os, f.getDen().getSign() ? 1 : 0);
        const auto& denRaw = f.getDen().getRawData();
        write32(os, static_cast<uint32_t>(denRaw.size()));
        for (uint32_t v : denRaw) write32(os, v);
        return;
    }
    if (val.isComplex()) {
        write8(os, static_cast<uint8_t>(ConstTag::COMPLEX));
        Complex c = val.asComplex();
        writeDouble(os, c.real);
        writeDouble(os, c.imag);
        return;
    }
    if (val.isObjType(ObjType::BASENUM)) {
        write8(os, static_cast<uint8_t>(ConstTag::BASENUM));
        BaseNum b = static_cast<ObjBaseNum*>(val.asObj())->base;
        write8(os, static_cast<uint8_t>(b.getRadix()));
        write8(os, b.getValue().getSign() ? 1 : 0);
        const auto& raw = b.getValue().getRawData();
        write32(os, static_cast<uint32_t>(raw.size()));
        for (uint32_t v : raw) write32(os, v);
        return;
    }
    if (val.isObjType(ObjType::REAL_MATRIX)) {
        write8(os, static_cast<uint8_t>(ConstTag::REAL_MATRIX));
        RealMatrix m = val.asRealMatrix();
        write16(os, static_cast<uint16_t>(m.getRows()));
        write16(os, static_cast<uint16_t>(m.getCols()));
        const auto& raw = m.rawData();
        for (double d : raw) writeDouble(os, d);
        return;
    }
    if (val.isObjType(ObjType::COMPLEX_MATRIX)) {
        write8(os, static_cast<uint8_t>(ConstTag::COMPLEX_MATRIX));
        ComplexMatrix m = val.asComplexMatrix();
        write16(os, static_cast<uint16_t>(m.getRows()));
        write16(os, static_cast<uint16_t>(m.getCols()));
        const auto& raw = m.rawData();
        for (Complex c : raw) { writeDouble(os, c.real); writeDouble(os, c.imag); }
        return;
    }
    if (val.isObjType(ObjType::NAMESPACE)) {
        ObjNamespace* ns = static_cast<ObjNamespace*>(val.asObj());
        write8(os, static_cast<uint8_t>(ConstTag::NAMESPACE));
        writeString(os, ns->name);
        write32(os, static_cast<uint32_t>(ns->fields.size()));
        for (const auto& [k, f] : ns->fields) {
            writeString(os, k);
            write8(os, f.isConst ? 1 : 0);
            writeValue(os, *(f.upval->location), fnIdxConstants, -1, startIndex);
        }
        return;
    }
    throw std::runtime_error("BytecodeSerializer Error: Unsupported constant type for serialization.");
}

Value BytecodeSerializer::readValue(std::istream& is, int baseIdx) {
    ConstTag tag = static_cast<ConstTag>(read8(is));
    switch (tag) {
        case ConstTag::NONE: return Value::none();
        case ConstTag::BOOL_FALSE: return Value(false);
        case ConstTag::BOOL_TRUE: return Value(true);
        case ConstTag::INT32: return Value(static_cast<int32_t>(read32(is)));
        case ConstTag::DOUBLE: return Value(readDouble(is));
        case ConstTag::STRING: return Value(readString(is));
        case ConstTag::BIGINT: {
            bool sign = read8(is) != 0;
            uint32_t size = read32(is);
            std::vector<uint32_t> raw(size);
            for (uint32_t i = 0; i < size; ++i) raw[i] = read32(is);
            return Value(BigInt::fromRawData(sign, raw));
        }
        case ConstTag::FRACTION: {
            bool nSign = read8(is) != 0;
            uint32_t nSize = read32(is);
            std::vector<uint32_t> nRaw(nSize);
            for (uint32_t i = 0; i < nSize; ++i) nRaw[i] = read32(is);
            bool dSign = read8(is) != 0;
            uint32_t dSize = read32(is);
            std::vector<uint32_t> dRaw(dSize);
            for (uint32_t i = 0; i < dSize; ++i) dRaw[i] = read32(is);
            return Value(Fraction(BigInt::fromRawData(nSign, nRaw), BigInt::fromRawData(dSign, dRaw)));
        }
        case ConstTag::COMPLEX: {
            double r = readDouble(is);
            double i = readDouble(is);
            return Value(Complex(r, i));
        }
        case ConstTag::BASENUM: {
            int radix = read8(is);
            bool sign = read8(is) != 0;
            uint32_t size = read32(is);
            std::vector<uint32_t> raw(size);
            for (uint32_t i = 0; i < size; ++i) raw[i] = read32(is);
            return Value(BaseNum(BigInt::fromRawData(sign, raw), radix));
        }
        case ConstTag::REAL_MATRIX: {
            uint16_t r = read16(is);
            uint16_t c = read16(is);
            std::vector<double> raw(r * c);
            for (int i = 0; i < r * c; ++i) raw[i] = readDouble(is);
            return Value(RealMatrix(r, c, raw));
        }
        case ConstTag::COMPLEX_MATRIX: {
            uint16_t r = read16(is);
            uint16_t c = read16(is);
            std::vector<Complex> raw(r * c);
            for (int i = 0; i < r * c; ++i) {
                double re = readDouble(is);
                double im = readDouble(is);
                raw[i] = Complex(re, im);
            }
            return Value(ComplexMatrix(r, c, raw));
        }
        case ConstTag::NAMESPACE: {
            ObjNamespace* ns = GcHeap::get().allocate<ObjNamespace>();
            ns->name = readString(is);
            uint32_t count = read32(is);
            for (uint32_t i = 0; i < count; ++i) {
                std::string key = readString(is);
                bool isConst = read8(is) != 0;
                Value val = readValue(is, baseIdx);
                ObjUpVal* uv = GcHeap::get().allocate<ObjUpVal>();
                uv->closed = val;
                uv->location = &uv->closed;
                ns->fields[key] = { uv, isConst };
            }
            ns->is_frozen = true;
            return Value(ns);
        }
        case ConstTag::FNIDX: {
            uint32_t offset = read32(is);
            return Value(static_cast<double>(baseIdx + offset));
        }
        default: throw std::runtime_error("JCB Read Error: Unknown constant tag.");
    }
}

void BytecodeSerializer::writeChunk(std::ostream& os, const Chunk& chunk, int startIndex, bool stripDebug) {
    write32(os, static_cast<uint32_t>(chunk.code.size()));
    for (Instruction inst : chunk.code) write32(os, inst);

    if (stripDebug) {
        write32(os, 0);
    } else {
        write32(os, static_cast<uint32_t>(chunk.lines.size()));
        for (int line : chunk.lines) write32(os, static_cast<uint32_t>(line));
    }

    std::unordered_set<int> fnIdxConstants;
    for (size_t ip = 0; ip < chunk.code.size(); ++ip) {
        Instruction inst = chunk.code[ip];
        OpCode op = GET_OPCODE(inst);
        if (op == OpCode::CLOSURE) {
            int bx = GET_Bx(inst);
            if (bx == ESCAPE_NORMAL_16) bx = chunk.code[ip + 1] >> 8;
            fnIdxConstants.insert(bx);
        }
    }

    write32(os, static_cast<uint32_t>(chunk.constants.size()));
    for (size_t i = 0; i < chunk.constants.size(); ++i) {
        writeValue(os, chunk.constants[i], fnIdxConstants, static_cast<int>(i), startIndex);
    }

    write32(os, static_cast<uint32_t>(chunk.inlineCaches.size()));
    for (const auto& ic : chunk.inlineCaches) write32(os, ic.nameIdx);

    write32(os, static_cast<uint32_t>(chunk.shapePatterns.size()));
    for (const auto& sp : chunk.shapePatterns) {
        write32(os, sp.minRows); write32(os, sp.maxRows);
        write32(os, sp.minCols); write32(os, sp.maxCols);
        write8(os, sp.exactMask);
    }

    write32(os, static_cast<uint32_t>(chunk.matrixShapes.size()));
    for (const auto& ms : chunk.matrixShapes) {
        write16(os, ms.rows);
        write16(os, static_cast<uint16_t>(ms.rowCols.size()));
        for (uint16_t c : ms.rowCols) write16(os, c);
    }

    write32(os, static_cast<uint32_t>(chunk.callSignatures.size()));
    for (const auto& sig : chunk.callSignatures) {
        write16(os, static_cast<uint16_t>(sig.refs.size()));
        for (const auto& ref : sig.refs) {
            write8(os, ref.argIndex);
            write8(os, ref.sourceType);
            write32(os, ref.sourceRef);
        }
    }
}

void BytecodeSerializer::readChunk(std::istream& is, Chunk& chunk, int baseIdx) {
    uint32_t codeSize = read32(is);
    chunk.code.resize(codeSize);
    chunk.typeFeedback.assign(codeSize, 0); // ★ JIT Tier 0 Profiling: 初始化为 0
    chunk.osrCounters.assign(codeSize, 0);  // ★ JIT OSR Profiling: 初始化为 0
    chunk.osrLoopHeaderFlags.assign(codeSize, 0);
    for (uint32_t i = 0; i < codeSize; ++i) chunk.code[i] = read32(is);

    // ★ 基于 CFG 识别真正的循环头（dominator 回边），填充 osrLoopHeaderFlags。
    // 这样 VM 的 OSR 触发检测只对真正的循环回边生效，不会被循环体内部的向后跳转误导。
    {
        jit::BytecodeCFG cfg;
        cfg.build(chunk);
        for (const auto& blk : cfg.blocks) {
            if (blk.isLoopHeader && blk.startIp >= 0 && blk.startIp < static_cast<int>(codeSize)) {
                chunk.osrLoopHeaderFlags[blk.startIp] = 1;
            }
        }
        chunk.osrLoopHeadersComputed = true;
    }

    uint32_t linesSize = read32(is);
    chunk.lines.resize(linesSize);
    for (uint32_t i = 0; i < linesSize; ++i) chunk.lines[i] = static_cast<int>(read32(is));

    uint32_t constSize = read32(is);
    chunk.constants.resize(constSize);
    for (uint32_t i = 0; i < constSize; ++i) chunk.constants[i] = readValue(is, baseIdx);

    uint32_t icSize = read32(is);
    chunk.inlineCaches.resize(icSize);
    for (uint32_t i = 0; i < icSize; ++i) {
        chunk.inlineCaches[i].nameIdx = read32(is);
        chunk.inlineCaches[i].cachedGlobalSlot = -1;
        chunk.inlineCaches[i].cachedClassId = 0;
        chunk.inlineCaches[i].cachedMethod = nullptr;
        chunk.inlineCaches[i].cachedClass = nullptr;
        chunk.inlineCaches[i].cachedFieldIndex = -1;
        chunk.inlineCaches[i].cachedBuiltinType = BuiltinType::UNKNOWN;
        chunk.inlineCaches[i].cachedNativeFn.reset();
    }

    uint32_t spSize = read32(is);
    chunk.shapePatterns.resize(spSize);
    for (uint32_t i = 0; i < spSize; ++i) {
        chunk.shapePatterns[i].minRows = read32(is);
        chunk.shapePatterns[i].maxRows = read32(is);
        chunk.shapePatterns[i].minCols = read32(is);
        chunk.shapePatterns[i].maxCols = read32(is);
        chunk.shapePatterns[i].exactMask = read8(is);
    }

    uint32_t msSize = read32(is);
    chunk.matrixShapes.resize(msSize);
    for (uint32_t i = 0; i < msSize; ++i) {
        chunk.matrixShapes[i].rows = read16(is);
        uint16_t rcSize = read16(is);
        chunk.matrixShapes[i].rowCols.resize(rcSize);
        for (uint16_t j = 0; j < rcSize; ++j) chunk.matrixShapes[i].rowCols[j] = read16(is);
    }

    uint32_t csSize = read32(is);
    chunk.callSignatures.resize(csSize);
    for (uint32_t i = 0; i < csSize; ++i) {
        uint16_t refSize = read16(is);
        chunk.callSignatures[i].refs.resize(refSize);
        for (uint16_t j = 0; j < refSize; ++j) {
            chunk.callSignatures[i].refs[j].argIndex = read8(is);
            chunk.callSignatures[i].refs[j].sourceType = read8(is);
            chunk.callSignatures[i].refs[j].sourceRef = read32(is);
        }
    }
}

void BytecodeSerializer::writeFunction(std::ostream& os, const CompiledFunction* fn, int startIndex, bool stripDebug) {
    writeString(os, fn->name);
    writeString(os, stripDebug ? "" : fn->sourceFile);
    write32(os, fn->arity);
    write32(os, fn->maxArity);
    write32(os, fn->localCount);
    write8(os, fn->hasRestParam ? 1 : 0);

    writeChunk(os, fn->chunk, startIndex, stripDebug);

    write16(os, static_cast<uint16_t>(fn->upvalues.size()));
    for (const auto& uv : fn->upvalues) {
        writeString(os, stripDebug ? "" : uv.name);
        uint8_t flags = 0;
        if (uv.isLocal) flags |= 1;
        if (uv.isRef) flags |= 2;
        if (uv.isGlobal) flags |= 4;
        if (uv.isExplicitState) flags |= 8;
        if (uv.isRefParam) flags |= 16;
        if (uv.isCapturedState) flags |= 32;
        write8(os, flags);
        write32(os, static_cast<uint32_t>(uv.index));
    }

    write16(os, static_cast<uint16_t>(fn->paramIsRef.size()));
    for (bool b : fn->paramIsRef) write8(os, b ? 1 : 0);

    write16(os, static_cast<uint16_t>(fn->paramIsConst.size()));
    for (bool b : fn->paramIsConst) write8(os, b ? 1 : 0);

    write16(os, static_cast<uint16_t>(fn->paramNames.size()));
    for (const auto& name : fn->paramNames) writeString(os, name);

    write32(os, static_cast<uint32_t>(fn->refCount));

    write16(os, static_cast<uint16_t>(fn->paramTypeRegs.size()));
    for (int reg : fn->paramTypeRegs) write32(os, static_cast<uint32_t>(reg));

    write32(os, static_cast<uint32_t>(fn->returnTypeReg));
}

void BytecodeSerializer::readFunction(std::istream& is, CompiledFunction* fn, int baseIdx) {
    fn->name = readString(is);
    fn->sourceFile = readString(is);
    fn->arity = static_cast<int>(read32(is));
    fn->maxArity = static_cast<int>(read32(is));
    fn->localCount = static_cast<int>(read32(is));
    fn->hasRestParam = read8(is) != 0;

    readChunk(is, fn->chunk, baseIdx);

    uint16_t uvSize = read16(is);
    fn->upvalues.resize(uvSize);
    for (uint16_t i = 0; i < uvSize; ++i) {
        fn->upvalues[i].name = readString(is);
        uint8_t flags = read8(is);
        fn->upvalues[i].isLocal = (flags & 1) != 0;
        fn->upvalues[i].isRef = (flags & 2) != 0;
        fn->upvalues[i].isGlobal = (flags & 4) != 0;
        fn->upvalues[i].isExplicitState = (flags & 8) != 0;
        fn->upvalues[i].isRefParam = (flags & 16) != 0;
        fn->upvalues[i].isCapturedState = (flags & 32) != 0;
        fn->upvalues[i].index = static_cast<int>(read32(is));
    }

    uint16_t prSize = read16(is);
    fn->paramIsRef.resize(prSize);
    for (uint16_t i = 0; i < prSize; ++i) fn->paramIsRef[i] = read8(is) != 0;

    uint16_t pcSize = read16(is);
    fn->paramIsConst.resize(pcSize);
    for (uint16_t i = 0; i < pcSize; ++i) fn->paramIsConst[i] = read8(is) != 0;

    uint16_t pnSize = read16(is);
    fn->paramNames.resize(pnSize);
    for (uint16_t i = 0; i < pnSize; ++i) fn->paramNames[i] = readString(is);

    fn->refCount = static_cast<int>(read32(is));

    uint16_t ptrSize = read16(is);
    fn->paramTypeRegs.resize(ptrSize);
    for (uint16_t i = 0; i < ptrSize; ++i) fn->paramTypeRegs[i] = static_cast<int>(read32(is));

    fn->returnTypeReg = static_cast<int>(read32(is));
}

void BytecodeSerializer::saveJCB(const std::string& path, VM* vm, int startIndex, int count, bool stripDebug) {
    std::ofstream os(path, std::ios::binary);
    if (!os) throw std::runtime_error("IO Error: Cannot open file for writing: " + path);

    write32(os, MAGIC_NUMBER);
    write32(os, VERSION);
    write32(os, static_cast<uint32_t>(count));

    for (int i = 0; i < count; ++i) {
        auto fn = vm->getCompiledFunctions()[startIndex + i];
        writeFunction(os, fn.get(), startIndex, stripDebug);
    }
}

std::shared_ptr<CompiledFunction> BytecodeSerializer::loadJCB(const std::string& path, VM* vm) {
    std::ifstream is(path, std::ios::binary);
    if (!is) throw std::runtime_error("IO Error: Cannot open file for reading: " + path);

    uint32_t magic = read32(is);
    if (magic != MAGIC_NUMBER) throw std::runtime_error("JCB_MAGIC_MISMATCH");
    uint32_t version = read32(is);
    if (version != VERSION) throw std::runtime_error("JCB_VERSION_MISMATCH");

    uint32_t count = read32(is);
    int baseIdx = static_cast<int>(vm->getCompiledFunctions().size());

    std::shared_ptr<CompiledFunction> lastFn;
    for (uint32_t i = 0; i < count; ++i) {
        auto fn = std::make_shared<CompiledFunction>();
        readFunction(is, fn.get(), baseIdx);
        vm->getCompiledFunctions().push_back(fn);
        lastFn = fn;
    }
    return lastFn;
}

} // namespace jc
