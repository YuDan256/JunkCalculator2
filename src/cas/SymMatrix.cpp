#include "SymMatrix.h"
#include "Groebner.h"
#include "../math/Matrix.h" // 借用 g_printMatrix2D 标志
#include <sstream>
#include <algorithm>

namespace jc {

    // ==========================================
    // 构造函数与基础访问
    // ==========================================
    SymMatrix::SymMatrix(int r, int c) : rows(r), cols(c), data(r * c, SymExpr(BigInt(0))) {}

    SymMatrix::SymMatrix(int r, int c, const std::vector<SymExpr>& flat_data)
        : rows(r), cols(c), data(flat_data) {
        if (flat_data.size() != static_cast<size_t>(r * c)) {
            throw std::invalid_argument("SymMatrix Error: Data size does not match dimensions.");
        }
    }

    SymMatrix::SymMatrix(const SymExpr& num) : rows(1), cols(1), data(1, num) {}
    SymMatrix::SymMatrix() : rows(0), cols(0), data() {}

    SymExpr& SymMatrix::operator()(int row, int col) {
        if (row >= 0 && row < rows && col >= 0 && col < cols) return data[row * cols + col];
        throw std::out_of_range("SymMatrix Error: Index out of bounds.");
    }

    const SymExpr& SymMatrix::operator()(int row, int col) const {
        if (row >= 0 && row < rows && col >= 0 && col < cols) return data[row * cols + col];
        throw std::out_of_range("SymMatrix Error: Index out of bounds.");
    }

    // ==========================================
    // 零等价探测器 (Zero Equivalence)
    // ==========================================
    bool SymMatrix::isSymZero(const SymExpr& expr) {
        if (!expr.ptr) return true;
        if (expr.isZero()) return true;
        
        // 浅层探测失败，使用轻量级展开化简进行零等价判定，避免调用极度耗时的 full_simplify
        try {
            SymExpr simplified = simplifyCore(expand_core(expr, SymConfig::maxExpandTerms));
            return simplified.isZero();
        } catch (...) {
            // 如果化简过程抛出异常（如超时或中断），保守返回 false
            return false;
        }
    }

    // ==========================================
    // 基础算术
    // ==========================================
    SymMatrix SymMatrix::operator+(const SymMatrix& other) const {
        if (isScalar() && other.isScalar()) return SymMatrix(data[0] + other.data[0]);
        if (rows != other.rows || cols != other.cols) throw std::invalid_argument("SymMatrix Error: Dimensions mismatch (+).");

        SymMatrix result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = data[i] + other.data[i];
        }
        return result;
    }

    SymMatrix SymMatrix::operator-(const SymMatrix& other) const {
        if (isScalar() && other.isScalar()) return SymMatrix(data[0] - other.data[0]);
        if (rows != other.rows || cols != other.cols) throw std::invalid_argument("SymMatrix Error: Dimensions mismatch (-).");

        SymMatrix result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = data[i] - other.data[i];
        }
        return result;
    }

    SymMatrix SymMatrix::operator-() const {
        SymMatrix result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = -data[i];
        }
        return result;
    }

    SymMatrix SymMatrix::operator*(const SymExpr& scalar) const {
        SymMatrix result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = data[i] * scalar;
        }
        return result;
    }

    SymMatrix operator*(const SymExpr& scalar, const SymMatrix& rhs) {
        return rhs * scalar;
    }

    SymMatrix SymMatrix::operator/(const SymExpr& scalar) const {
        if (isSymZero(scalar)) throw std::runtime_error("SymMatrix Error: Division by zero.");
        SymMatrix result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = data[i] / scalar;
        }
        return result;
    }

    // ==========================================
    // 矩阵乘法 (带防膨胀机制)
    // ==========================================
    SymMatrix SymMatrix::operator*(const SymMatrix& other) const {
        if (rows == 1 && cols == 1) return other * data[0];
        if (other.rows == 1 && other.cols == 1) return (*this) * other.data[0];
        if (cols != other.rows) throw std::invalid_argument("SymMatrix Error: Cols must equal rows (*).");

        SymMatrix result(rows, other.cols);
        for (int i = 0; i < rows; ++i) {
            checkInterrupt();
            for (int k = 0; k < cols; ++k) {
                SymExpr r = (*this)(i, k);
                if (isSymZero(r)) continue; // 稀疏跳跃
                
                for (int j = 0; j < other.cols; ++j) {
                    SymExpr term = r * other(k, j);
                    if (!isSymZero(term)) {
                        // ★ 核心防膨胀：在累加时强制化简，防止 AST 树指数级爆炸
                        result(i, j) = simplifyCore(expand_core(result(i, j) + term, SymConfig::maxExpandTerms));
                    }
                }
            }
        }
        return result;
    }

    // ==========================================
    // 标量加减 (A ± c*I)
    // ==========================================
    SymMatrix SymMatrix::operator+(const SymExpr& scalar) const {
        if (rows != cols) throw std::invalid_argument("SymMatrix Error: Scalar addition requires square matrix.");
        SymMatrix result(*this);
        for (int i = 0; i < rows; ++i) result(i, i) = result(i, i) + scalar;
        return result;
    }

    SymMatrix operator+(const SymExpr& scalar, const SymMatrix& rhs) {
        return rhs + scalar;
    }

    SymMatrix SymMatrix::operator-(const SymExpr& scalar) const {
        if (rows != cols) throw std::invalid_argument("SymMatrix Error: Scalar subtraction requires square matrix.");
        SymMatrix result(*this);
        for (int i = 0; i < rows; ++i) result(i, i) = result(i, i) - scalar;
        return result;
    }

    SymMatrix operator-(const SymExpr& scalar, const SymMatrix& rhs) {
        if (rhs.getRows() != rhs.getCols()) throw std::invalid_argument("SymMatrix Error: Scalar subtraction requires square matrix.");
        SymMatrix result = -rhs;
        for (int i = 0; i < rhs.getRows(); ++i) result(i, i) = result(i, i) + scalar;
        return result;
    }

    // ==========================================
    // 结构操作
    // ==========================================
    SymMatrix SymMatrix::transpose() const {
        SymMatrix result(cols, rows);
        for (int i = 0; i < rows; i++) {
            for (int j = 0; j < cols; j++) {
                result(j, i) = (*this)(i, j);
            }
        }
        return result;
    }

    SymMatrix SymMatrix::subMatrix(int excludeRow, int excludeCol) const {
        if (rows <= 1 || cols <= 1) throw std::invalid_argument("SymMatrix Error: Matrix too small for subMatrix.");
        SymMatrix result(rows - 1, cols - 1);
        int ri = 0;
        for (int i = 0; i < rows; ++i) {
            if (i == excludeRow) continue;
            int rj = 0;
            for (int j = 0; j < cols; ++j) {
                if (j == excludeCol) continue;
                result(ri, rj) = (*this)(i, j);
                rj++;
            }
            ri++;
        }
        return result;
    }

    void SymMatrix::swapRows(int row1, int row2) {
        if (row1 < 0 || row1 >= rows || row2 < 0 || row2 >= rows) throw std::out_of_range("SymMatrix Error: Row index out of bounds.");
        for (int j = 0; j < cols; ++j) {
            SymExpr temp = (*this)(row1, j);
            (*this)(row1, j) = (*this)(row2, j);
            (*this)(row2, j) = temp;
        }
    }

    void SymMatrix::swapCols(int col1, int col2) {
        if (col1 < 0 || col1 >= cols || col2 < 0 || col2 >= cols) throw std::out_of_range("SymMatrix Error: Column index out of bounds.");
        for (int i = 0; i < rows; ++i) {
            SymExpr temp = (*this)(i, col1);
            (*this)(i, col1) = (*this)(i, col2);
            (*this)(i, col2) = temp;
        }
    }

    SymMatrix SymMatrix::getRow(int r) const {
        if (r < 0 || r >= rows) throw std::out_of_range("SymMatrix Error: Row index out of bounds.");
        SymMatrix result(1, cols);
        for (int j = 0; j < cols; ++j) result(0, j) = (*this)(r, j);
        return result;
    }

    SymMatrix SymMatrix::getCol(int c) const {
        if (c < 0 || c >= cols) throw std::out_of_range("SymMatrix Error: Column index out of bounds.");
        SymMatrix result(rows, 1);
        for (int i = 0; i < rows; ++i) result(i, 0) = (*this)(i, c);
        return result;
    }

    SymMatrix SymMatrix::deleteRow(int r) const {
        if (rows <= 1) throw std::invalid_argument("SymMatrix Error: Cannot delete row from single-row matrix.");
        if (r < 0 || r >= rows) throw std::out_of_range("SymMatrix Error: Row index out of bounds.");
        SymMatrix result(rows - 1, cols);
        int ri = 0;
        for (int i = 0; i < rows; ++i) {
            if (i == r) continue;
            for (int j = 0; j < cols; ++j) result(ri, j) = (*this)(i, j);
            ri++;
        }
        return result;
    }

    SymMatrix SymMatrix::deleteCol(int c) const {
        if (cols <= 1) throw std::invalid_argument("SymMatrix Error: Cannot delete col from single-column matrix.");
        if (c < 0 || c >= cols) throw std::out_of_range("SymMatrix Error: Column index out of bounds.");
        SymMatrix result(rows, cols - 1);
        for (int i = 0; i < rows; ++i) {
            int rj = 0;
            for (int j = 0; j < cols; ++j) {
                if (j == c) continue;
                result(i, rj++) = (*this)(i, j);
            }
        }
        return result;
    }

    SymMatrix SymMatrix::integR(const SymMatrix& other) const {
        if (rows != other.rows) throw std::invalid_argument("SymMatrix Error: Row counts must match for horizontal concatenation.");
        SymMatrix result(rows, cols + other.cols);
        for (int i = 0; i < rows; ++i) {
            for (int j = 0; j < cols; ++j) result(i, j) = (*this)(i, j);
            for (int j = 0; j < other.cols; ++j) result(i, j + cols) = other(i, j);
        }
        return result;
    }

    SymMatrix SymMatrix::integC(const SymMatrix& other) const {
        if (cols != other.cols) throw std::invalid_argument("SymMatrix Error: Column counts must match for vertical concatenation.");
        SymMatrix result(rows + other.rows, cols);
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < cols; ++j) result(i, j) = (*this)(i, j);
        for (int i = 0; i < other.rows; ++i)
            for (int j = 0; j < cols; ++j) result(i + rows, j) = other(i, j);
        return result;
    }

    SymMatrix SymMatrix::integD(const SymMatrix& other) const {
        SymMatrix result(rows + other.rows, cols + other.cols);
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < cols; ++j) result(i, j) = (*this)(i, j);
        for (int i = 0; i < other.rows; ++i)
            for (int j = 0; j < other.cols; ++j) result(i + rows, j + cols) = other(i, j);
        return result;
    }

    SymMatrix SymMatrix::reshape(int newRows, int newCols) const {
        if (newRows * newCols != rows * cols) throw std::invalid_argument("SymMatrix Error: Element count mismatch in reshape.");
        SymMatrix result(newRows, newCols);
        for (int idx = 0; idx < rows * cols; ++idx) {
            result.data[idx] = data[idx];
        }
        return result;
    }

    SymMatrix SymMatrix::identity(int n) {
        SymMatrix I(n, n);
        for (int i = 0; i < n; ++i) I(i, i) = SymExpr(BigInt(1));
        return I;
    }

    SymMatrix SymMatrix::zeros(int r, int c) {
        return SymMatrix(r, c);
    }

    SymMatrix SymMatrix::ones(int r, int c) {
        SymMatrix result(r, c);
        for (auto& val : result.data) val = SymExpr(BigInt(1));
        return result;
    }

    // ==========================================
    // 高级线性代数
    // ==========================================
    SymExpr SymMatrix::determinant() const {
        if (rows != cols) throw std::invalid_argument("SymMatrix Error: Determinant requires a square matrix.");
        if (rows == 0) return SymExpr(BigInt(1));
        if (rows == 1) return (*this)(0, 0);
        if (rows == 2) return jc::simplify(expand_core((*this)(0,0)*(*this)(1,1) - (*this)(0,1)*(*this)(1,0), SymConfig::maxExpandTerms));

        MultiPoly::clearRegistry();
        int n = rows;
        std::vector<std::vector<MultiPoly>> M(n, std::vector<MultiPoly>(n));
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                M[i][j] = MultiPoly((*this)(i, j));
            }
        }

        MultiPoly prev_pivot(SymExpr(BigInt(1)));
        int sign = 1;

        // Bareiss 算法 (无分母高斯消元法) - 纯多项式环极速版
        for (int k = 0; k < n - 1; ++k) {
            checkInterrupt();
            int pivot_row = k;
            while (pivot_row < n && M[pivot_row][k].isZero()) {
                pivot_row++;
            }
            if (pivot_row == n) return SymExpr(BigInt(0));

            if (pivot_row != k) {
                std::swap(M[k], M[pivot_row]);
                sign = -sign;
            }

            MultiPoly pivot = M[k][k];
            for (int i = k + 1; i < n; ++i) {
                for (int j = k + 1; j < n; ++j) {
                    MultiPoly diff = (M[i][j] * pivot) - (M[i][k] * M[k][j]);
                    M[i][j] = diff.exactDivide(prev_pivot);
                }
            }
            prev_pivot = pivot;
        }

        SymExpr det = M[n - 1][n - 1].toSymExpr();
        if (sign == -1) det = simplifyCore(-det);
        return jc::simplify(det);
    }

    SymExpr SymMatrix::cofactor(int row, int col) const {
        return subMatrix(row, col).determinant();
    }

    SymExpr SymMatrix::algebraicCofactor(int row, int col) const {
        SymExpr cof = cofactor(row, col);
        if ((row + col) % 2 != 0) return simplifyCore(-cof);
        return cof;
    }

    SymMatrix SymMatrix::adjugate() const {
        if (rows != cols) throw std::invalid_argument("SymMatrix Error: Adjugate requires a square matrix.");
        if (rows == 1) {
            SymMatrix res(1, 1);
            res(0, 0) = SymExpr(BigInt(1));
            return res;
        }
        SymMatrix result(rows, cols);
        for (int i = 0; i < rows; ++i) {
            checkInterrupt();
            for (int j = 0; j < cols; ++j) {
                result(j, i) = algebraicCofactor(i, j); // 注意这里的 (j, i) 已经包含了转置
            }
        }
        return result;
    }

    SymMatrix SymMatrix::inverse() const {
        if (rows != cols) throw std::invalid_argument("SymMatrix Error: Inverse requires a square matrix.");
        if (rows == 0) return SymMatrix();
        if (rows == 1) {
            if (isSymZero((*this)(0, 0))) throw std::runtime_error("SymMatrix Error: Matrix is singular and cannot be inverted.");
            SymMatrix res(1, 1);
            res(0, 0) = SymExpr(BigInt(1)) / (*this)(0, 0);
            return res;
        }
        if (rows == 2) {
            SymExpr det = simplifyCore(expand_core((*this)(0,0)*(*this)(1,1) - (*this)(0,1)*(*this)(1,0), SymConfig::maxExpandTerms));
            if (isSymZero(det)) throw std::runtime_error("SymMatrix Error: Matrix is singular and cannot be inverted.");
            SymMatrix res(2, 2);
            res(0, 0) = jc::simplify((*this)(1, 1) / det);
            res(0, 1) = jc::simplify(-(*this)(0, 1) / det);
            res(1, 0) = jc::simplify(-(*this)(1, 0) / det);
            res(1, 1) = jc::simplify((*this)(0, 0) / det);
            return res;
        }

        MultiPoly::clearRegistry();
        int n = rows;
        std::vector<std::vector<MultiPoly>> aug(n, std::vector<MultiPoly>(2 * n));
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                aug[i][j] = MultiPoly((*this)(i, j));
            }
            aug[i][n + i] = MultiPoly(SymExpr(BigInt(1)));
        }

        MultiPoly prev_pivot(SymExpr(BigInt(1)));

        // Bareiss 算法 (无分母高斯-若尔当消元法) - 纯多项式环极速版
        for (int k = 0; k < n; ++k) {
            checkInterrupt();
            int pivot_row = k;
            while (pivot_row < n && aug[pivot_row][k].isZero()) {
                pivot_row++;
            }
            if (pivot_row == n) throw std::runtime_error("SymMatrix Error: Matrix is singular and cannot be inverted.");

            if (pivot_row != k) {
                std::swap(aug[k], aug[pivot_row]);
            }

            MultiPoly pivot = aug[k][k];
            for (int i = 0; i < n; ++i) {
                if (i == k) continue;
                for (int j = k + 1; j < 2 * n; ++j) {
                    MultiPoly diff = (aug[i][j] * pivot) - (aug[i][k] * aug[k][j]);
                    aug[i][j] = diff.exactDivide(prev_pivot);
                }
            }
            prev_pivot = pivot;
        }

        SymExpr det_equiv = aug[n - 1][n - 1].toSymExpr();

        SymMatrix res(n, n);
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                res(i, j) = jc::simplify(aug[i][n + j].toSymExpr() / det_equiv);
            }
        }
        return res;
    }

    SymMatrix SymMatrix::power(int n) const {
        if (rows != cols) throw std::invalid_argument("SymMatrix Error: Matrix power requires a square matrix.");
        if (n == 0) return identity(rows);
        SymMatrix base = (n > 0) ? *this : inverse();
        int exp = std::abs(n);
        SymMatrix result = identity(rows);
        while (exp > 0) {
            checkInterrupt();
            if (exp & 1) result = result * base;
            base = base * base;
            exp >>= 1;
        }
        return result;
    }

    SymExpr SymMatrix::trace() const {
        if (rows != cols) throw std::invalid_argument("SymMatrix Error: Trace requires a square matrix.");
        SymExpr result(BigInt(0));
        for (int i = 0; i < rows; ++i) {
            result = result + (*this)(i, i);
        }
        return jc::simplify(result);
    }

    SymExpr SymMatrix::sum() const {
        SymExpr result(BigInt(0));
        for (const auto& val : data) result = result + val;
        return jc::simplify(result);
    }

    SymExpr SymMatrix::product() const {
        SymExpr result(BigInt(1));
        for (const auto& val : data) result = result * val;
        return jc::simplify(result);
    }

    SymMatrix SymMatrix::conjugateTranspose() const {
        return transpose();
    }

    SymMatrix SymMatrix::nullSpace() const {
        throw std::runtime_error("SymMatrix Error: nullSpace() is not implemented for symbolic matrices.");
    }

    SymMatrix SymMatrix::orthogonalize() const {
        throw std::runtime_error("SymMatrix Error: orthogonalize() is not implemented for symbolic matrices.");
    }

    int SymMatrix::rank() const {
        throw std::runtime_error("SymMatrix Error: rank() is not implemented for symbolic matrices.");
    }

    SymExpr SymMatrix::norm() const {
        throw std::runtime_error("SymMatrix Error: norm() is not implemented for symbolic matrices.");
    }

    SymExpr SymMatrix::condition() const {
        throw std::runtime_error("SymMatrix Error: condition() is not implemented for symbolic matrices.");
    }

    SymExpr SymMatrix::permanent() const {
        throw std::runtime_error("SymMatrix Error: permanent() is not implemented for symbolic matrices.");
    }

    // ==========================================
    // CAS 深度联动
    // ==========================================
    SymMatrix SymMatrix::diff(const std::string& var) const {
        SymMatrix result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = jc::diff(data[i], var);
        }
        return result;
    }

    SymMatrix SymMatrix::integ(const std::string& /*var*/) const {
        throw std::runtime_error("SymMatrix Error: Symbolic integration is not fully implemented yet.");
    }

    SymMatrix SymMatrix::limit(const std::string& var, const SymExpr& val, const std::string& dir) const {
        SymMatrix result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = jc::limit(data[i], var, val, dir);
        }
        return result;
    }

    SymMatrix SymMatrix::subs(const std::string& var, const SymExpr& val) const {
        SymMatrix result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = jc::subs(data[i], var, val);
        }
        return result;
    }

    SymMatrix SymMatrix::simplify() const {
        SymMatrix result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = jc::simplify(data[i]);
        }
        return result;
    }

    SymMatrix SymMatrix::expand() const {
        SymMatrix result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = jc::expand(data[i]);
        }
        return result;
    }

    SymMatrix SymMatrix::evalFloat() const {
        SymMatrix result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = jc::evalFloat(data[i]);
        }
        return result;
    }

    SymMatrix SymMatrix::evalValue() const {
        SymMatrix result(rows, cols);
        for (size_t i = 0; i < data.size(); ++i) {
            result.data[i] = jc::evalValue(data[i]);
        }
        return result;
    }

    // ==========================================
    // 格式化输出
    // ==========================================
    std::ostream& operator<<(std::ostream& out, const SymMatrix& m) {
        if (m.rows == 0 || m.cols == 0) {
            out << "[]";
            return out;
        }

        std::vector<std::vector<std::string>> strs(m.rows, std::vector<std::string>(m.cols));
        std::vector<size_t> colWidths(m.cols, 0);

        for (int i = 0; i < m.rows; ++i) {
            for (int j = 0; j < m.cols; ++j) {
                strs[i][j] = m(i, j).toString();
                if (strs[i][j].length() > colWidths[j]) {
                    colWidths[j] = strs[i][j].length();
                }
            }
        }

        if (jc::g_printMatrix2D) {
            for (int i = 0; i < m.rows; ++i) {
                out << "[";
                for (int j = 0; j < m.cols; ++j) {
                    size_t padding = colWidths[j] - strs[i][j].length();
                    for (size_t p = 0; p < padding; ++p) out << ' ';
                    out << strs[i][j];
                    if (j < m.cols - 1) out << ", ";
                }
                out << "]";
                if (i < m.rows - 1) out << "\n";
            }
        } else {
            out << "[";
            for (int i = 0; i < m.rows; ++i) {
                for (int j = 0; j < m.cols; ++j) {
                    out << strs[i][j];
                    if (j < m.cols - 1) out << ", ";
                }
                if (i < m.rows - 1) out << "; ";
            }
            out << "]";
        }
        return out;
    }

} // namespace jc
