#include "BytecodeSerializer.h"
#include "VM.h"
#include "../jit/frontend/BytecodeCFG.h"
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

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
    if (val.isUninit()) { write8(os, static_cast<uint8_t>(ConstTag::UNINIT)); return; }
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
        case ConstTag::UNINIT: return Value::uninit();
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
            write8(os, ref.isConst ? 1 : 0);
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
            chunk.callSignatures[i].refs[j].isConst = read8(is) != 0;
        }
    }
}

void BytecodeSerializer::writeFunction(std::ostream& os, const CompiledFunction* fn, int startIndex, bool stripDebug) {
    writeString(os, fn->name);
    writeString(os, stripDebug ? "" : fn->sourceFile);
    write32(os, fn->arity);
    write32(os, fn->maxArity);
    write32(os, fn->localCount);
    writeString(os, fn->restName);
    writeString(os, fn->kwargsName);
    write16(os, static_cast<uint16_t>(fn->kwargNames.size()));
    for (const auto& name : fn->kwargNames) writeString(os, name);

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

    write16(os, static_cast<uint16_t>(fn->kwargIsRef.size()));
    for (bool b : fn->kwargIsRef) write8(os, b ? 1 : 0);

    write16(os, static_cast<uint16_t>(fn->kwargIsConst.size()));
    for (bool b : fn->kwargIsConst) write8(os, b ? 1 : 0);

    write16(os, static_cast<uint16_t>(fn->kwargHasDefault.size()));
    for (bool b : fn->kwargHasDefault) write8(os, b ? 1 : 0);

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
    fn->restName = readString(is);
    fn->kwargsName = readString(is);
    uint16_t kwargCount = read16(is);
    fn->kwargNames.resize(kwargCount);
    for (uint16_t i = 0; i < kwargCount; ++i) fn->kwargNames[i] = readString(is);

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

    uint16_t kwrSize = read16(is);
    fn->kwargIsRef.resize(kwrSize);
    for (uint16_t i = 0; i < kwrSize; ++i) fn->kwargIsRef[i] = read8(is) != 0;

    uint16_t kwcSize = read16(is);
    fn->kwargIsConst.resize(kwcSize);
    for (uint16_t i = 0; i < kwcSize; ++i) fn->kwargIsConst[i] = read8(is) != 0;

    uint16_t kwhSize = read16(is);
    fn->kwargHasDefault.resize(kwhSize);
    for (uint16_t i = 0; i < kwhSize; ++i) fn->kwargHasDefault[i] = read8(is) != 0;

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

void BytecodeSerializer::saveJCW(const std::string& path, VM* vm) {
    std::ofstream os(path, std::ios::binary);
    if (!os) throw std::runtime_error("IO Error: Cannot open file for writing: " + path);

    write32(os, 0x4A435701); // JCW1
    write32(os, VERSION);

    auto& fns = vm->getCompiledFunctions();
    write32(os, static_cast<uint32_t>(fns.size()));
    for (const auto& fn : fns) {
        writeFunction(os, fn.get(), 0, false);
    }

    std::unordered_map<Obj*, uint32_t> objToId;
    uint32_t nextId = 1;

    auto writeRuntimeValue = [&](const Value& val, auto& self) -> void {
        if (val.isNone()) { write8(os, 0); return; }
        if (val.isUninit()) { write8(os, 1); return; }
        if (val.isBool()) { write8(os, val.asBool() ? 3 : 2); return; }
        if (val.isInt32()) { write8(os, 4); write32(os, val.asInt32()); return; }
        if (val.isDouble()) { write8(os, 5); writeDouble(os, val.asDoubleRaw()); return; }
        
        Obj* obj = val.asObj();
        if (objToId.count(obj)) {
            write8(os, 6);
            write32(os, objToId[obj]);
            return;
        }
        
        objToId[obj] = nextId++;
        
        if (obj->type == ObjType::INSTANCE) {
            auto inst = static_cast<ObjInstance*>(obj);
            if (inst->nativeData.has_value() || inst->c_nativeData != nullptr) {
                std::string cname = inst->classDef ? inst->classDef->name : "Unknown";
                std::cout << "   [Warning] Skipped serialization of native instance (Class: " << cname << ").\n";
                write8(os, 8);
                return;
            }
        } else if (obj->type == ObjType::CLOSURE) {
            auto cl = static_cast<ObjClosure*>(obj);
            if (cl->isNative() && !cl->isBytecode()) {
                write8(os, 9);
                writeString(os, cl->rawBody);
                return;
            }
        } else if (obj->type == ObjType::NAMESPACE) {
            for (const auto& [k, v] : vm->getLoadedModules()) {
                if (v.isObj() && v.asObj() == obj) {
                    std::cout << "   [Warning] Skipped imported module '" << k << "'. Please re-import it after loading.\n";
                    write8(os, 10);
                    writeString(os, k);
                    return;
                }
            }
        } else if (obj->type == ObjType::CLASS) {
            auto cls = static_cast<ObjClass*>(obj);
            if (cls->is_native || cls->native_allocator) {
                write8(os, 11);
                writeString(os, cls->name);
                return;
            }
            if (cls == vm->listProto) { write8(os, 11); writeString(os, "<ListProto>"); return; }
            if (cls == vm->dictProto) { write8(os, 11); writeString(os, "<DictProto>"); return; }
            if (cls == vm->setProto) { write8(os, 11); writeString(os, "<SetProto>"); return; }
            if (cls == vm->stringProto) { write8(os, 11); writeString(os, "<StringProto>"); return; }
            if (cls == vm->matrixProto) { write8(os, 11); writeString(os, "<MatrixProto>"); return; }
        }

        write8(os, 7);
        write8(os, static_cast<uint8_t>(obj->type));
        
        switch (obj->type) {
            case ObjType::STRING: writeString(os, static_cast<ObjString*>(obj)->str); break;
            case ObjType::BIGINT: {
                BigInt b = static_cast<ObjBigInt*>(obj)->num;
                write8(os, b.getSign() ? 1 : 0);
                const auto& raw = b.getRawData();
                write32(os, static_cast<uint32_t>(raw.size()));
                for (uint32_t v : raw) write32(os, v);
                break;
            }
            case ObjType::FRACTION: {
                Fraction f = static_cast<ObjFraction*>(obj)->frac;
                write8(os, f.getNum().getSign() ? 1 : 0);
                const auto& numRaw = f.getNum().getRawData();
                write32(os, static_cast<uint32_t>(numRaw.size()));
                for (uint32_t v : numRaw) write32(os, v);
                write8(os, f.getDen().getSign() ? 1 : 0);
                const auto& denRaw = f.getDen().getRawData();
                write32(os, static_cast<uint32_t>(denRaw.size()));
                for (uint32_t v : denRaw) write32(os, v);
                break;
            }
            case ObjType::COMPLEX: {
                Complex c = static_cast<ObjComplex*>(obj)->comp;
                writeDouble(os, c.real); writeDouble(os, c.imag);
                break;
            }
            case ObjType::REAL_MATRIX: {
                RealMatrix m = static_cast<ObjRealMatrix*>(obj)->mat;
                write16(os, static_cast<uint16_t>(m.getRows()));
                write16(os, static_cast<uint16_t>(m.getCols()));
                for (double d : m.rawData()) writeDouble(os, d);
                break;
            }
            case ObjType::COMPLEX_MATRIX: {
                ComplexMatrix m = static_cast<ObjComplexMatrix*>(obj)->mat;
                write16(os, static_cast<uint16_t>(m.getRows()));
                write16(os, static_cast<uint16_t>(m.getCols()));
                for (Complex c : m.rawData()) { writeDouble(os, c.real); writeDouble(os, c.imag); }
                break;
            }
            case ObjType::SYM_MATRIX: {
                SymMatrix m = static_cast<ObjSymMatrix*>(obj)->mat;
                write16(os, static_cast<uint16_t>(m.getRows()));
                write16(os, static_cast<uint16_t>(m.getCols()));
                for (const auto& s : m.rawData()) writeString(os, s.toString());
                break;
            }
            case ObjType::SYMBOLIC: {
                writeString(os, static_cast<ObjSym*>(obj)->sym.toString());
                break;
            }
            case ObjType::LIST: {
                auto l = static_cast<ObjList*>(obj);
                write8(os, l->is_frozen ? 1 : 0);
                write32(os, static_cast<uint32_t>(l->vec.size()));
                for (const auto& v : l->vec) self(v, self);
                break;
            }
            case ObjType::DICT: {
                auto d = static_cast<ObjDict*>(obj);
                write8(os, d->is_frozen ? 1 : 0);
                write32(os, static_cast<uint32_t>(d->elements.size()));
                for (const auto& [k, v] : d->elements) {
                    self(k, self); self(v, self);
                }
                break;
            }
            case ObjType::SET: {
                auto s = static_cast<ObjSet*>(obj);
                write8(os, s->is_frozen ? 1 : 0);
                write32(os, static_cast<uint32_t>(s->elements.size()));
                for (const auto& v : s->elements) self(v, self);
                break;
            }
            case ObjType::UPVALUE: {
                auto uv = static_cast<ObjUpVal*>(obj);
                self(uv->closed, self);
                break;
            }
            case ObjType::CLOSURE: {
                auto cl = static_cast<ObjClosure*>(obj);
                write32(os, cl->compiledFnIndex);
                write32(os, cl->upvalueCount);
                for (int i = 0; i < cl->upvalueCount; ++i) {
                    if (cl->upvalues[i]) {
                        write8(os, 1);
                        self(Value(cl->upvalues[i]), self);
                    } else {
                        write8(os, 0);
                    }
                }
                self(cl->boundSelf, self);
                self(cl->boundClass, self);
                write8(os, cl->isUFCS ? 1 : 0);
                write8(os, cl->isTokenMacro ? 1 : 0);
                write8(os, cl->is_local ? 1 : 0);
                self(cl->owner_class ? Value(cl->owner_class) : Value::none(), self);
                break;
            }
            case ObjType::CLASS: {
                auto cls = static_cast<ObjClass*>(obj);
                writeString(os, cls->name);
                self(cls->parent ? Value(cls->parent) : Value::none(), self);
                write32(os, static_cast<uint32_t>(cls->properties.size()));
                for (const auto& [k, p] : cls->properties) {
                    writeString(os, k);
                    self(p.val, self);
                    write8(os, p.is_const ? 1 : 0);
                    write8(os, p.is_local ? 1 : 0);
                }
                break;
            }
            case ObjType::INSTANCE: {
                auto inst = static_cast<ObjInstance*>(obj);
                self(inst->classDef ? Value(inst->classDef) : Value::none(), self);
                write8(os, inst->is_frozen ? 1 : 0);
                write32(os, static_cast<uint32_t>(inst->properties.size()));
                for (const auto& [k, p] : inst->properties) {
                    writeString(os, k);
                    self(p.val, self);
                    write8(os, p.is_const ? 1 : 0);
                    write8(os, p.is_local ? 1 : 0);
                }
                break;
            }
            case ObjType::NAMESPACE: {
                auto ns = static_cast<ObjNamespace*>(obj);
                writeString(os, ns->name);
                write8(os, ns->is_frozen ? 1 : 0);
                write32(os, static_cast<uint32_t>(ns->fields.size()));
                for (const auto& [k, f] : ns->fields) {
                    writeString(os, k);
                    self(Value(f.upval), self);
                    write8(os, f.isConst ? 1 : 0);
                }
                break;
            }
            case ObjType::TYPE_DEF: {
                auto td = static_cast<ObjTypeDef*>(obj);
                write32(os, static_cast<uint32_t>(td->types.size()));
                for (const auto& t : td->types) {
                    if (std::holds_alternative<BuiltinType>(t)) {
                        write8(os, 0);
                        write32(os, static_cast<uint32_t>(std::get<BuiltinType>(t)));
                    } else {
                        write8(os, 1);
                        self(Value(std::get<ObjClass*>(t)), self);
                    }
                }
                break;
            }
            case ObjType::SLICE: {
                auto sl = static_cast<ObjSlice*>(obj);
                write32(os, sl->start);
                write32(os, sl->end);
                write32(os, sl->step);
                break;
            }
            case ObjType::SPREAD: {
                auto sp = static_cast<ObjSpread*>(obj);
                self(sp->value, self);
                write8(os, sp->isKeyword ? 1 : 0);
                break;
            }
            case ObjType::SUPER_PROXY: {
                auto sp = static_cast<ObjSuper*>(obj);
                self(sp->instance ? Value(sp->instance) : Value::none(), self);
                self(sp->parentClass ? Value(sp->parentClass) : Value::none(), self);
                break;
            }
        }
    };

    auto globals = vm->getGlobals();
    std::vector<std::pair<std::string, Value>> filteredGlobals;
    for (const auto& [name, val] : globals) {
        if (name == "PI" || name == "E" || name == "i" || name == "I" || name == "ANS") continue;
        if (name.length() >= 2 && name.front() == '<' && name.back() == '>') continue;
        
        if (!vm->getBuiltinValue(name).isNone()) continue;
        if (vm->getNativeBuiltins().count(name)) continue;
        
        filteredGlobals.push_back({name, val});
    }

    write32(os, static_cast<uint32_t>(filteredGlobals.size()));
    for (const auto& [name, val] : filteredGlobals) {
        writeString(os, name);
        writeRuntimeValue(val, writeRuntimeValue);
    }
}

void BytecodeSerializer::loadJCW(const std::string& path, VM* vm) {
    std::ifstream is(path, std::ios::binary);
    if (!is) throw std::runtime_error("IO Error: Cannot open file for reading: " + path);

    uint32_t magic = read32(is);
    if (magic != 0x4A435701) throw std::runtime_error("JCW_MAGIC_MISMATCH");
    uint32_t version = read32(is);
    if (version != VERSION) throw std::runtime_error("JCW_VERSION_MISMATCH");

    uint32_t fnCount = read32(is);
    vm->getCompiledFunctions().clear();
    for (uint32_t i = 0; i < fnCount; ++i) {
        auto fn = std::make_shared<CompiledFunction>();
        readFunction(is, fn.get(), 0);
        vm->getCompiledFunctions().push_back(fn);
    }

    std::vector<Obj*> idToObj;
    idToObj.push_back(nullptr); // ID 0 is null

    auto readRuntimeValue = [&](auto& self) -> Value {
        uint8_t tag = read8(is);
        if (tag == 0) return Value::none();
        if (tag == 1) return Value::uninit();
        if (tag == 2) return Value(false);
        if (tag == 3) return Value(true);
        if (tag == 4) return Value(static_cast<int32_t>(read32(is)));
        if (tag == 5) return Value(readDouble(is));
        if (tag == 6) {
            uint32_t id = read32(is);
            return Value(idToObj[id]);
        }
        if (tag == 8) {
            idToObj.push_back(nullptr);
            return Value::none();
        }
        if (tag == 9) {
            std::string name = readString(is);
            Value cl = vm->getBuiltinClosure(name);
            idToObj.push_back(cl.asObj());
            return cl;
        }
        if (tag == 10) {
            std::string name = readString(is);
            idToObj.push_back(nullptr);
            return Value::none();
        }
        if (tag == 11) {
            std::string name = readString(is);
            Obj* clsObj = nullptr;
            if (name == "<ListProto>") clsObj = vm->listProto;
            else if (name == "<DictProto>") clsObj = vm->dictProto;
            else if (name == "<SetProto>") clsObj = vm->setProto;
            else if (name == "<StringProto>") clsObj = vm->stringProto;
            else if (name == "<MatrixProto>") clsObj = vm->matrixProto;
            else {
                Value g = vm->getGlobal(name);
                if (g.isClass()) clsObj = g.asObj();
            }
            idToObj.push_back(clsObj);
            return clsObj ? Value(clsObj) : Value::none();
        }
        
        if (tag == 7) {
            ObjType type = static_cast<ObjType>(read8(is));
            uint32_t id = static_cast<uint32_t>(idToObj.size());
            idToObj.push_back(nullptr); // placeholder
            
            Value result;
            switch (type) {
                case ObjType::STRING: {
                    result = Value(readString(is));
                    idToObj[id] = result.asObj();
                    break;
                }
                case ObjType::BIGINT: {
                    bool sign = read8(is) != 0;
                    uint32_t size = read32(is);
                    std::vector<uint32_t> raw(size);
                    for (uint32_t i = 0; i < size; ++i) raw[i] = read32(is);
                    result = Value(BigInt::fromRawData(sign, raw));
                    idToObj[id] = result.asObj();
                    break;
                }
                case ObjType::FRACTION: {
                    bool nSign = read8(is) != 0;
                    uint32_t nSize = read32(is);
                    std::vector<uint32_t> nRaw(nSize);
                    for (uint32_t i = 0; i < nSize; ++i) nRaw[i] = read32(is);
                    bool dSign = read8(is) != 0;
                    uint32_t dSize = read32(is);
                    std::vector<uint32_t> dRaw(dSize);
                    for (uint32_t i = 0; i < dSize; ++i) dRaw[i] = read32(is);
                    result = Value(Fraction(BigInt::fromRawData(nSign, nRaw), BigInt::fromRawData(dSign, dRaw)));
                    idToObj[id] = result.asObj();
                    break;
                }
                case ObjType::COMPLEX: {
                    double r = readDouble(is); double i = readDouble(is);
                    result = Value(Complex(r, i));
                    idToObj[id] = result.asObj();
                    break;
                }
                case ObjType::REAL_MATRIX: {
                    uint16_t r = read16(is); uint16_t c = read16(is);
                    std::vector<double> raw(r * c);
                    for (int i = 0; i < r * c; ++i) raw[i] = readDouble(is);
                    result = Value(RealMatrix(r, c, raw));
                    idToObj[id] = result.asObj();
                    break;
                }
                case ObjType::COMPLEX_MATRIX: {
                    uint16_t r = read16(is); uint16_t c = read16(is);
                    std::vector<Complex> raw(r * c);
                    for (int i = 0; i < r * c; ++i) {
                        double re = readDouble(is); double im = readDouble(is);
                        raw[i] = Complex(re, im);
                    }
                    result = Value(ComplexMatrix(r, c, raw));
                    idToObj[id] = result.asObj();
                    break;
                }
                case ObjType::SYM_MATRIX: {
                    uint16_t r = read16(is); uint16_t c = read16(is);
                    std::vector<SymExpr> raw(r * c);
                    for (int i = 0; i < r * c; ++i) {
                        std::string s = readString(is);
                        if (jc::helpers::evalCallback) {
                            try { raw[i] = jc::helpers::evalCallback(s).asSymbolic(); }
                            catch (...) { raw[i] = SymExpr(BigInt(0)); }
                        } else {
                            raw[i] = SymExpr(BigInt(0));
                        }
                    }
                    result = Value(SymMatrix(r, c, raw));
                    idToObj[id] = result.asObj();
                    break;
                }
                case ObjType::SYMBOLIC: {
                    std::string s = readString(is);
                    if (jc::helpers::evalCallback) {
                        try { result = jc::helpers::evalCallback(s); }
                        catch (...) { result = Value(SymExpr(BigInt(0))); }
                    } else {
                        result = Value(SymExpr(BigInt(0)));
                    }
                    idToObj[id] = result.asObj();
                    break;
                }
                case ObjType::LIST: {
                    ObjList* l = GcHeap::get().allocate<ObjList>();
                    result = Value(l);
                    idToObj[id] = l;
                    l->is_frozen = read8(is) != 0;
                    uint32_t size = read32(is);
                    for (uint32_t i = 0; i < size; ++i) l->vec.push_back(self(self));
                    break;
                }
                case ObjType::DICT: {
                    ObjDict* d = GcHeap::get().allocate<ObjDict>();
                    result = Value(d);
                    idToObj[id] = d;
                    d->is_frozen = read8(is) != 0;
                    uint32_t size = read32(is);
                    for (uint32_t i = 0; i < size; ++i) {
                        Value k = self(self); Value v = self(self);
                        d->keyMap[k] = d->elements.size();
                        d->elements.push_back({k, v});
                    }
                    break;
                }
                case ObjType::SET: {
                    ObjSet* s = GcHeap::get().allocate<ObjSet>();
                    result = Value(s);
                    idToObj[id] = s;
                    s->is_frozen = read8(is) != 0;
                    uint32_t size = read32(is);
                    for (uint32_t i = 0; i < size; ++i) {
                        Value v = self(self);
                        s->keys.insert(v);
                        s->elements.push_back(v);
                    }
                    break;
                }
                case ObjType::UPVALUE: {
                    ObjUpVal* uv = GcHeap::get().allocate<ObjUpVal>();
                    result = Value(uv);
                    idToObj[id] = uv;
                    uv->closed = self(self);
                    uv->location = &uv->closed;
                    break;
                }
                case ObjType::CLOSURE: {
                    ObjClosure* cl = GcHeap::get().allocate<ObjClosure>(
                        std::vector<std::string>{}, std::vector<bool>{}, "", nullptr
                    );
                    result = Value(cl);
                    idToObj[id] = cl;
                    cl->compiledFnIndex = read32(is);
                    cl->upvalueCount = read32(is);
                    if (cl->upvalueCount > 0) {
                        cl->upvalues = new ObjUpVal*[cl->upvalueCount];
                        for (int i = 0; i < cl->upvalueCount; ++i) {
                            if (read8(is) != 0) {
                                cl->upvalues[i] = static_cast<ObjUpVal*>(self(self).asObj());
                            } else {
                                cl->upvalues[i] = nullptr;
                            }
                        }
                    }
                    cl->boundSelf = self(self);
                    cl->boundClass = self(self);
                    cl->isUFCS = read8(is) != 0;
                    cl->isTokenMacro = read8(is) != 0;
                    cl->is_local = read8(is) != 0;
                    Value owner = self(self);
                    if (owner.isClass()) cl->owner_class = static_cast<ObjClass*>(owner.asObj());
                    
                    if (cl->compiledFnIndex >= 0 && cl->compiledFnIndex < static_cast<int>(vm->getCompiledFunctions().size())) {
                        auto fn = vm->getCompiledFunctions()[cl->compiledFnIndex];
                        cl->paramNames = fn->paramNames;
                        cl->isRef = fn->paramIsRef;
                        cl->isConst = fn->paramIsConst;
                        cl->restName = fn->restName;
                        cl->kwargNames = fn->kwargNames;
                        cl->kwargIsRef = fn->kwargIsRef;
                        cl->kwargIsConst = fn->kwargIsConst;
                        cl->kwargsName = fn->kwargsName;
                        cl->kwargHasDefault = fn->kwargHasDefault;
                        int defaultLimit = fn->maxArity;
                        for (int j = fn->arity; j < defaultLimit; ++j) cl->defaultValues.push_back(Value::uninit());
                        
                        if (!fn->paramTypeRegs.empty()) {
                            cl->paramTypesCount = static_cast<int>(fn->paramTypeRegs.size());
                            cl->paramTypes = new Value[cl->paramTypesCount];
                            for (int i = 0; i < cl->paramTypesCount; ++i) cl->paramTypes[i] = Value::none();
                        }
                    }
                    break;
                }
                case ObjType::CLASS: {
                    ObjClass* cls = GcHeap::get().allocate<ObjClass>();
                    result = Value(cls);
                    idToObj[id] = cls;
                    cls->name = readString(is);
                    Value parent = self(self);
                    if (parent.isClass()) cls->parent = static_cast<ObjClass*>(parent.asObj());
                    uint32_t propCount = read32(is);
                    for (uint32_t i = 0; i < propCount; ++i) {
                        std::string k = readString(is);
                        Value v = self(self);
                        bool isConst = read8(is) != 0;
                        bool isLocal = read8(is) != 0;
                        cls->properties[k] = {v, isConst, isLocal};
                    }
                    break;
                }
                case ObjType::INSTANCE: {
                    ObjInstance* inst = GcHeap::get().allocate<ObjInstance>();
                    result = Value(inst);
                    idToObj[id] = inst;
                    Value cls = self(self);
                    if (cls.isClass()) inst->classDef = static_cast<ObjClass*>(cls.asObj());
                    inst->is_frozen = read8(is) != 0;
                    uint32_t propCount = read32(is);
                    for (uint32_t i = 0; i < propCount; ++i) {
                        std::string k = readString(is);
                        Value v = self(self);
                        bool isConst = read8(is) != 0;
                        bool isLocal = read8(is) != 0;
                        inst->properties[k] = {v, isConst, isLocal};
                    }
                    break;
                }
                case ObjType::NAMESPACE: {
                    ObjNamespace* ns = GcHeap::get().allocate<ObjNamespace>();
                    result = Value(ns);
                    idToObj[id] = ns;
                    ns->name = readString(is);
                    ns->is_frozen = read8(is) != 0;
                    uint32_t fieldCount = read32(is);
                    for (uint32_t i = 0; i < fieldCount; ++i) {
                        std::string k = readString(is);
                        Value uv = self(self);
                        bool isConst = read8(is) != 0;
                        if (uv.isObjType(ObjType::UPVALUE)) {
                            ns->fields[k] = {static_cast<ObjUpVal*>(uv.asObj()), isConst};
                        }
                    }
                    break;
                }
                case ObjType::TYPE_DEF: {
                    uint32_t typeCount = read32(is);
                    std::vector<std::variant<BuiltinType, ObjClass*>> types;
                    for (uint32_t i = 0; i < typeCount; ++i) {
                        if (read8(is) == 0) {
                            types.push_back(static_cast<BuiltinType>(read32(is)));
                        } else {
                            Value cls = self(self);
                            if (cls.isClass()) types.push_back(static_cast<ObjClass*>(cls.asObj()));
                        }
                    }
                    result = Value(internType(std::move(types)));
                    idToObj[id] = result.asObj();
                    break;
                }
                case ObjType::SLICE: {
                    ObjSlice* sl = GcHeap::get().allocate<ObjSlice>();
                    result = Value(sl);
                    idToObj[id] = sl;
                    sl->start = read32(is);
                    sl->end = read32(is);
                    sl->step = read32(is);
                    break;
                }
                case ObjType::SPREAD: {
                    ObjSpread* sp = GcHeap::get().allocate<ObjSpread>();
                    result = Value(sp);
                    idToObj[id] = sp;
                    sp->value = self(self);
                    sp->isKeyword = read8(is) != 0;
                    break;
                }
                case ObjType::SUPER_PROXY: {
                    ObjSuper* sp = GcHeap::get().allocate<ObjSuper>();
                    result = Value(sp);
                    idToObj[id] = sp;
                    Value inst = self(self);
                    if (inst.isInstance()) sp->instance = inst.asInstance();
                    Value parent = self(self);
                    if (parent.isClass()) sp->parentClass = static_cast<ObjClass*>(parent.asObj());
                    break;
                }
                default:
                    result = Value::none();
                    break;
            }
            return result;
        }
        return Value::none();
    };

    vm->clearGlobals();
    uint32_t globalCount = read32(is);
    for (uint32_t i = 0; i < globalCount; ++i) {
        std::string name = readString(is);
        Value val = readRuntimeValue(readRuntimeValue);
        vm->setGlobal(name, val);
    }
}

} // namespace jc
