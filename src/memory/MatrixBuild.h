#pragma once

#include "Value.h"

#include <cstdint>
#include <vector>

namespace jc {

// Shared BUILD_MATRIX semantics for the VM and the constant folder.
// Inputs are already evaluated cells; rows/rowCols describe the literal
// block structure. Throws RuntimeError for invalid concatenations.
inline Value buildMatrixValue(const std::vector<Value>& cells, uint16_t rows,
                              const std::vector<uint16_t>& rowCols) {
    int total = 0;
    for (uint16_t cols : rowCols) total += cols;
    if (static_cast<int>(cells.size()) < total) {
        JC2_THROW(RuntimeError, "Matrix literal has fewer cells than its shape.");
    }

    bool hasComplex = false;
    bool hasSymbolic = false;
    bool hasOther = false;

    auto canBeMatrixElement = [](const Value& v) -> bool {
        return v.isNumber() || v.isObjType(ObjType::BIGINT) || v.isObjType(ObjType::FRACTION) ||
               v.isObjType(ObjType::COMPLEX) ||
               v.isObjType(ObjType::SYMBOLIC) ||
               v.isObjType(ObjType::REAL_MATRIX) || v.isObjType(ObjType::COMPLEX_MATRIX) ||
               v.isObjType(ObjType::SYM_MATRIX);
    };

    for (int ii = 0; ii < total; ++ii) {
        const Value& v = cells[static_cast<size_t>(ii)];
        if (v.isObjType(ObjType::COMPLEX) || v.isObjType(ObjType::COMPLEX_MATRIX)) hasComplex = true;
        if (v.isSymbolic() || v.isObjType(ObjType::SYM_MATRIX)) hasSymbolic = true;
        if (!canBeMatrixElement(v)) {
            hasOther = true;
        } else if (v.isObjType(ObjType::BIGINT) || v.isObjType(ObjType::FRACTION)) {
            try { v.asFloat(); } catch (...) {
                if (!hasSymbolic) hasOther = true;
            }
        }
    }

    if (hasOther) {
        JC2_THROW(RuntimeError, "Matrix elements must be numeric, complex, or symbolic. Use @[...] for lists.");
    }

    auto extractCell = [&](Value& cell) {
        if (!cell.isObjType(ObjType::REAL_MATRIX) && !cell.isObjType(ObjType::COMPLEX_MATRIX) &&
            !cell.isObjType(ObjType::SYM_MATRIX)) {
            if (hasSymbolic) {
                cell = Value(SymMatrix(1, 1, { cell.asSymbolic() }));
            } else if (hasComplex) {
                cell = Value(ComplexMatrix(1, 1, { cell.asComplex() }));
            } else {
                cell = Value(RealMatrix(1, 1, { cell.asFloat() }));
            }
        }
        if (hasSymbolic) {
            if (cell.isObjType(ObjType::REAL_MATRIX) || cell.isObjType(ObjType::COMPLEX_MATRIX)) {
                cell = Value(cell.asSymMatrix());
            }
        } else if (hasComplex && cell.isObjType(ObjType::REAL_MATRIX)) {
            cell = Value(cell.asComplexMatrix());
        }
    };

    try {
        int idx = 0;
        Value matResult = Value::none();
        for (int i = 0; i < rows; ++i) {
            Value rowResult = Value::none();
            int cols = rowCols[static_cast<size_t>(i)];
            for (int j = 0; j < cols; ++j) {
                Value cell = cells[static_cast<size_t>(idx++)];
                extractCell(cell);
                if (rowResult.isNone()) {
                    rowResult = cell;
                } else {
                    if (hasSymbolic) rowResult = Value(static_cast<ObjSymMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjSymMatrix*>(cell.asObj())->mat));
                    else if (hasComplex) rowResult = Value(static_cast<ObjComplexMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjComplexMatrix*>(cell.asObj())->mat));
                    else rowResult = Value(static_cast<ObjRealMatrix*>(rowResult.asObj())->mat.integR(static_cast<ObjRealMatrix*>(cell.asObj())->mat));
                }
            }
            if (matResult.isNone()) {
                matResult = rowResult;
            } else {
                if (hasSymbolic) matResult = Value(static_cast<ObjSymMatrix*>(matResult.asObj())->mat.integC(static_cast<ObjSymMatrix*>(rowResult.asObj())->mat));
                else if (hasComplex) matResult = Value(static_cast<ObjComplexMatrix*>(matResult.asObj())->mat.integC(static_cast<ObjComplexMatrix*>(rowResult.asObj())->mat));
                else matResult = Value(static_cast<ObjRealMatrix*>(matResult.asObj())->mat.integC(static_cast<ObjRealMatrix*>(rowResult.asObj())->mat));
            }
        }
        return matResult;
    } catch (const Jc2Error& e) {
        JC2_THROW(RuntimeError, "Block matrix concatenation failed: " + e.message);
    } catch (...) {
        JC2_THROW(RuntimeError, "Block matrix concatenation failed.");
    }
}

} // namespace jc
