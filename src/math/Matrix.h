#ifndef JC2_MATRIX_H
#define JC2_MATRIX_H

#include "Complex.h"
#include "Tolerance.h"
#include <cmath>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "../vm/EngineInterrupt.h"

namespace jc {

    inline bool g_printMatrix2D = false;

    template <typename T>
    class Matrix {
    private:
        int rows;
        int cols;
        std::vector<T> data; // 极致连续的一维内存，装 T

    public:
        // --- 构造函数 ---
        Matrix(int r, int c) : rows(r), cols(c), data(r* c, T(0)) {}

        Matrix(int r, int c, const std::vector<T>& flat_data)
            : rows(r), cols(c), data(flat_data) {
            if (flat_data.size() != static_cast<size_t>(r * c)) {
                throw std::invalid_argument("Matrix Error: Data size does not match dimensions.");
            }
        }

        Matrix(T num) : rows(1), cols(1), data(1, num) {}
        Matrix() : rows(0), cols(0), data() {}

        // --- 基础访问 ---
        int getRows() const { return rows; }
        int getCols() const { return cols; }
        bool isNumber() const { return rows == 1 && cols == 1; }

        T& operator()(int row, int col) {
            if (row >= 0 && row < rows && col >= 0 && col < cols) return data[row * cols + col];
            throw std::out_of_range("Matrix Error: Index out of bounds.");
        }

        const T& operator()(int row, int col) const {
            if (row >= 0 && row < rows && col >= 0 && col < cols) return data[row * cols + col];
            throw std::out_of_range("Matrix Error: Index out of bounds.");
        }

        // ==== 惊艳的高速数学运算 (纯模板推导) ====

        Matrix operator+(const Matrix& other) const {
            if (isNumber() && other.isNumber()) return Matrix(data[0] + other.data[0]);
            if (rows != other.rows || cols != other.cols) throw std::invalid_argument("Matrix Error: Dimensions mismatch (+).");

            Matrix result(rows, cols);
            for (size_t i = 0; i < data.size(); ++i) result.data[i] = data[i] + other.data[i];
            return result;
        }

        Matrix operator-(const Matrix& other) const {
            if (isNumber() && other.isNumber()) return Matrix(data[0] - other.data[0]);
            if (rows != other.rows || cols != other.cols) throw std::invalid_argument("Matrix Error: Dimensions mismatch (-).");

            Matrix result(rows, cols);
            for (size_t i = 0; i < data.size(); ++i) result.data[i] = data[i] - other.data[i];
            return result;
        }

        Matrix operator-() const {
            Matrix result(rows, cols);
            for (size_t i = 0; i < data.size(); ++i) result.data[i] = -data[i];
            return result;
        }

        Matrix operator*(T scalar) const {
            Matrix result(rows, cols);
            for (size_t i = 0; i < data.size(); ++i) result.data[i] = data[i] * scalar;
            return result;
        }

        friend Matrix operator*(T scalar, const Matrix& rhs) {
            return rhs * scalar;
        }

        Matrix operator/(T scalar) const {
            // 对于泛型 T，如果 T 是 double，比较 0.0；如果是 Complex，比较它是否为零 (需确认 Complex 有 == 重载)
            if (scalar == T(0)) throw std::runtime_error("Matrix Error: Division by zero.");
            Matrix result(rows, cols);
            for (size_t i = 0; i < data.size(); ++i) result.data[i] = data[i] / scalar;
            return result;
        }

        // 基础 O(N^3) 乘法 (i-k-j 缓存优化 + 稀疏跳跃)
        static Matrix multiplyBase(const Matrix& A, const Matrix& B) {
            Matrix result(A.rows, B.cols);
            for (int i = 0; i < A.rows; ++i) {
                checkInterrupt();
                for (int k = 0; k < A.cols; ++k) {
                    T r = A(i, k);
                    if (isEssentiallyZero(r)) continue; // ★ 稀疏优化：若乘数为0，直接跳过整行遍历，大幅加速稀疏/对角矩阵
                    for (int j = 0; j < B.cols; ++j) {
                        result(i, j) = result(i, j) + r * B(k, j);
                    }
                }
            }
            return result;
        }

        // Strassen 递归核心 (要求 A, B 是 2^k x 2^k 的方阵)
        static Matrix strassenCore(const Matrix& A, const Matrix& B, int depth = 0) {
            int n = A.rows;
            if (n <= 64) return multiplyBase(A, B); // 阈值：小矩阵使用基础乘法

            int half = n / 2;
            Matrix A11(half, half), A12(half, half), A21(half, half), A22(half, half);
            Matrix B11(half, half), B12(half, half), B21(half, half), B22(half, half);

            for (int i = 0; i < half; ++i) {
                for (int j = 0; j < half; ++j) {
                    A11(i, j) = A(i, j);
                    A12(i, j) = A(i, j + half);
                    A21(i, j) = A(i + half, j);
                    A22(i, j) = A(i + half, j + half);

                    B11(i, j) = B(i, j);
                    B12(i, j) = B(i, j + half);
                    B21(i, j) = B(i + half, j);
                    B22(i, j) = B(i + half, j + half);
                }
            }

            Matrix M1, M2, M3, M4, M5, M6, M7;

            // 限制并发深度，防止线程爆炸 (最大深度设为 2，最多产生 1 + 7 + 49 = 57 个任务)
            if (depth < 2) {
                auto f1 = std::async(std::launch::async, [&]() { return strassenCore(A11 + A22, B11 + B22, depth + 1); });
                auto f2 = std::async(std::launch::async, [&]() { return strassenCore(A21 + A22, B11, depth + 1); });
                auto f3 = std::async(std::launch::async, [&]() { return strassenCore(A11, B12 - B22, depth + 1); });
                auto f4 = std::async(std::launch::async, [&]() { return strassenCore(A22, B21 - B11, depth + 1); });
                auto f5 = std::async(std::launch::async, [&]() { return strassenCore(A11 + A12, B22, depth + 1); });
                auto f6 = std::async(std::launch::async, [&]() { return strassenCore(A21 - A11, B11 + B12, depth + 1); });
                
                // 最后一个任务直接在当前线程执行，充分利用资源
                M7 = strassenCore(A12 - A22, B21 + B22, depth + 1);

                M1 = f1.get(); M2 = f2.get(); M3 = f3.get();
                M4 = f4.get(); M5 = f5.get(); M6 = f6.get();
            } else {
                M1 = strassenCore(A11 + A22, B11 + B22, depth + 1);
                M2 = strassenCore(A21 + A22, B11, depth + 1);
                M3 = strassenCore(A11, B12 - B22, depth + 1);
                M4 = strassenCore(A22, B21 - B11, depth + 1);
                M5 = strassenCore(A11 + A12, B22, depth + 1);
                M6 = strassenCore(A21 - A11, B11 + B12, depth + 1);
                M7 = strassenCore(A12 - A22, B21 + B22, depth + 1);
            }

            Matrix C11 = M1 + M4 - M5 + M7;
            Matrix C12 = M3 + M5;
            Matrix C21 = M2 + M4;
            Matrix C22 = M1 - M2 + M3 + M6;

            Matrix C(n, n);
            for (int i = 0; i < half; ++i) {
                for (int j = 0; j < half; ++j) {
                    C(i, j) = C11(i, j);
                    C(i, j + half) = C12(i, j);
                    C(i + half, j) = C21(i, j);
                    C(i + half, j + half) = C22(i, j);
                }
            }
            return C;
        }

        // 智能矩阵乘法入口
        Matrix operator*(const Matrix& other) const {
            if (rows == 1 && cols == 1) return other * data[0];
            if (other.rows == 1 && other.cols == 1) return (*this) * other.data[0];
            if (cols != other.rows) throw std::invalid_argument("Matrix Error: Cols must equal rows (*).");

            int maxDim = std::max({rows, cols, other.cols});
            // 当矩阵维度较大时，启用 Strassen 算法 (O(N^2.81))
            if (maxDim > 64) {
                int m = 1;
                while (m < maxDim) m *= 2;

                Matrix A_pad(m, m), B_pad(m, m);
                for (int i = 0; i < rows; ++i)
                    for (int j = 0; j < cols; ++j)
                        A_pad(i, j) = (*this)(i, j);

                for (int i = 0; i < other.rows; ++i)
                    for (int j = 0; j < other.cols; ++j)
                        B_pad(i, j) = other(i, j);

                Matrix C_pad = strassenCore(A_pad, B_pad);

                Matrix result(rows, other.cols);
                for (int i = 0; i < rows; ++i)
                    for (int j = 0; j < other.cols; ++j)
                        result(i, j) = C_pad(i, j);

                return result;
            }

            return multiplyBase(*this, other);
        }

        // ==== 纯数学流派：标量加减变成 A ± c*I ====
        Matrix operator+(T scalar) const {
            if (rows != cols) throw std::invalid_argument("Math Error: Scalar addition requires square matrix.");
            Matrix result(*this);
            for (int i = 0; i < rows; ++i) result(i, i) = result(i, i) + scalar;
            return result;
        }

        friend Matrix operator+(T scalar, const Matrix& rhs) {
            return rhs + scalar;
        }

        Matrix operator-(T scalar) const {
            if (rows != cols) throw std::invalid_argument("Math Error: Scalar subtraction requires square matrix.");
            Matrix result(*this);
            for (int i = 0; i < rows; ++i) result(i, i) = result(i, i) - scalar;
            return result;
        }

        friend Matrix operator-(T scalar, const Matrix& rhs) {
            if (rhs.getRows() != rhs.getCols()) throw std::invalid_argument("Math Error: Scalar subtraction requires square matrix.");
            Matrix result = -rhs;
            for (int i = 0; i < rhs.getRows(); ++i) result(i, i) = result(i, i) + scalar;
            return result;
        }

        // ==== 格式化输出 ====
        friend std::ostream& operator<<(std::ostream& out, const Matrix& m) {
            std::vector<std::vector<std::string>> strs(m.rows, std::vector<std::string>(m.cols));
            std::vector<size_t> colWidths(m.cols, 0);

            // ★ 修复：按行独立提取参考容差 (Row-based scaling) 解决显示遮蔽
            std::vector<double> rowScale(m.rows, 0.0);
            for (int i = 0; i < m.rows; ++i) {
                for (int j = 0; j < m.cols; ++j) {
                    rowScale[i] = std::max(rowScale[i], magnitudeOf(m(i, j)));
                }
                rowScale[i] = std::max(rowScale[i], 1e-30); // 兜底
            }

            for (int i = 0; i < m.rows; ++i) {
                for (int j = 0; j < m.cols; ++j) {
                    std::ostringstream oss;
                    T val = m(i, j);

                    if constexpr (std::is_same_v<T, double>) {
                        // ★ 把本行的规模传给清洗引擎，消除浮点噪音
                        val = Tol::clean(val, rowScale[i]);

                        std::ostringstream temp;
                        temp << val;
                        std::string s = temp.str();
                        if (std::isfinite(val) && s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
                            s += ".0";
                        }
                        oss << s;
                    }
                    else if constexpr (std::is_same_v<T, Complex>) {
                        Complex cv = Complex::cleaned(val);
                        // 对于复数，传它的模长进清洗引擎，消除浮点噪音
                        cv.real = Tol::clean(cv.real, rowScale[i]);
                        cv.imag = Tol::clean(cv.imag, rowScale[i]);

                        oss << cv; // Complex 的 operator<< 已经处理了 .0 的逻辑
                    }
                    else {
                        if (isEssentiallyZero(val)) oss << 0;
                        else oss << val;
                    }

                    strs[i][j] = oss.str();
                    if (strs[i][j].length() > colWidths[j]) {
                        colWidths[j] = strs[i][j].length();
                    }
                }
            }

            if (jc::g_printMatrix2D) {
                // 保持排版输出不变 (2D)
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
                // 嵌套时的一维紧凑输出 (1D)
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

        void swapRows(int row1, int row2) {
            if (row1 < 0 || row1 >= rows || row2 < 0 || row2 >= rows) throw std::out_of_range("Matrix Error: Row index out of bounds.");
            for (int j = 0; j < cols; ++j) {
                T temp = (*this)(row1, j);
                (*this)(row1, j) = (*this)(row2, j);
                (*this)(row2, j) = temp;
            }
        }
        // [2] 初等行变换：某行乘以非零常数
        void multiplyRow(int row, T scalar, int startCol = 0) {
            if (row < 0 || row >= rows) throw std::out_of_range("Matrix Error: Row index out of bounds.");
            // 利用标准库判定 0 的通用写法 (兼容 double, Complex 等类型的比较)
            if (scalar == T(0)) throw std::invalid_argument("Matrix Error: Cannot multiply a row by zero.");
            for (int j = startCol; j < cols; ++j) {
                (*this)(row, j) = (*this)(row, j) * scalar;
            }
        }
        // [3] 初等行变换：将 row2 的 scalar 倍加到 row1 上 ( row1 = row1 + scalar * row2 )
        void addRows(int row1, int row2, T scalar, int startCol = 0) {
            if (row1 < 0 || row1 >= rows || row2 < 0 || row2 >= rows) throw std::out_of_range("Matrix Error: Row index out of bounds.");
            for (int j = startCol; j < cols; ++j) {
                (*this)(row1, j) = (*this)(row1, j) + scalar * (*this)(row2, j);
            }
        }

        // [4] 判断元素是否为零
        static bool isEssentiallyZero(const T& val) {
            if constexpr (std::is_same_v<T, double>) return val == 0.0;
            else if constexpr (std::is_same_v<T, Complex>) return val.real == 0.0 && val.imag == 0.0;
            else return val == T(0);
        }

        // ================== 高阶算法 ==================
        // 获取某个元素的模长大小 (用于高斯消元找绝对值最大的主元，避免除以微小浮点数造成误差爆炸)
        static double magnitudeOf(const T& val) {
            if constexpr (std::is_same_v<T, double>) return std::abs(val);
            else if constexpr (std::is_same_v<T, Complex>) return val.modulus();
            else return 0.0;
        }
        // [5] 终极兵器：高斯-约当消元法 (Gaussian-Jordan Elimination) -> 转化为简化阶梯型矩阵
        // 返回值对：<消元后的矩阵, 交换行的次数 (用于算行列式的正负号)>
        std::pair<Matrix<T>, int> gaussianElimination() const {
            Matrix<T> result(*this);
            int swapCount = 0;
            int currentRow = 0;

            // ★ 提取行级容差参照
            std::vector<double> rowScale(rows, 0.0);
            for (int i = 0; i < rows; ++i) {
                for (int j = 0; j < cols; ++j) {
                    rowScale[i] = std::max(rowScale[i], magnitudeOf(result(i, j)));
                }
                rowScale[i] = std::max(rowScale[i], 1e-30);
            }

            for (int j = 0; j < cols && currentRow < rows; ++j) {
                checkInterrupt();
                int maxRow = currentRow;
                double maxVal = magnitudeOf(result(currentRow, j));

                for (int i = currentRow + 1; i < rows; ++i) {
                    double currentVal = magnitudeOf(result(i, j));
                    if (currentVal > maxVal) {
                        maxVal = currentVal;
                        maxRow = i;
                    }
                }

                // ★ 相比以前比较 globalMax，现在只比较它本身行的 scale
                if (Tol::clean(maxVal, rowScale[maxRow]) == 0.0) {
                    for (int i = currentRow; i < rows; ++i) result(i, j) = T(0);
                    continue;
                }

                if (maxRow != currentRow) {
                    result.swapRows(currentRow, maxRow);
                    std::swap(rowScale[currentRow], rowScale[maxRow]); // ★ 容差表跟着一起换行！
                    swapCount++;
                }
                T pivot = result(currentRow, j);
                // ★ 优化：左侧已经是 0，无需参与乘法，直接从 j 开始
                result.multiplyRow(currentRow, T(1) / pivot, j);
                for (int i = 0; i < rows; ++i) {
                    if (i != currentRow) {
                        T factor = result(i, j);
                        // ★ 浮点清扫同样只参考本行
                        if (Tol::clean(magnitudeOf(factor), rowScale[i]) != 0.0) {
                            result.addRows(i, currentRow, -factor, j + 1);
                            result(i, j) = T(0); // ★ 显式置零，消除浮点误差并砍掉冗余计算
                        }
                        else {
                            result(i, j) = T(0);
                        }
                    }
                }
                currentRow++;
            }

            // 最终扫描清扫
            for (int i = 0; i < result.rows; i++) {
                for (int j = 0; j < result.cols; j++) {
                    if (Tol::clean(magnitudeOf(result(i, j)), rowScale[i]) == 0.0) result(i, j) = T(0);
                }
            }
            return { result, swapCount };
        }

        // [6] 行列式 (Determinant)
        T determinant() const {
            if (rows != cols) throw std::invalid_argument("Math Error: Determinant is only defined for square matrices.");
            Matrix<T> temp(*this);
            int swapCount = 0;

            std::vector<double> rowScale(rows, 0.0);
            for (int i = 0; i < rows; ++i) {
                for (int j = 0; j < cols; ++j) {
                    rowScale[i] = std::max(rowScale[i], magnitudeOf(temp(i, j)));
                }
                rowScale[i] = std::max(rowScale[i], 1e-30);
            }

            for (int i = 0; i < cols; ++i) {
                checkInterrupt();
                int maxRow = i;
                double maxVal = magnitudeOf(temp(i, i));
                for (int k = i + 1; k < rows; ++k) {
                    double currentVal = magnitudeOf(temp(k, i));
                    if (currentVal > maxVal) {
                        maxVal = currentVal;
                        maxRow = k;
                    }
                }

                // ★ 当选出的最强主元，在它自身行中都算微小时，果断认零
                if (Tol::clean(maxVal, rowScale[maxRow]) == 0.0) return T(0);

                if (maxRow != i) {
                    temp.swapRows(i, maxRow);
                    std::swap(rowScale[i], rowScale[maxRow]);
                    swapCount++;
                }
                T pivot = temp(i, i);
                for (int j = i + 1; j < rows; ++j) {
                    T factor = temp(j, i) / pivot;
                    temp(j, i) = T(0); // ★ 显式置零，跳过无意义计算
                    for (int k = i + 1; k < cols; ++k) {
                        temp(j, k) = temp(j, k) - factor * temp(i, k);
                    }
                }
            }
            T det = T(1);
            for (int i = 0; i < rows; ++i) {
                det = det * temp(i, i);
            }
            return (swapCount % 2 == 1) ? -det : det;
        }

        // [7] 矩阵转置 (Transpose)
        Matrix<T> transpose() const {
            Matrix<T> result(cols, rows);
            for (int i = 0; i < rows; i++) {
                for (int j = 0; j < cols; j++) {
                    result(j, i) = (*this)(i, j);
                }
            }
            return result;
        }
        // [8] 逆矩阵 (Inverse) - 拼凑满配高斯-约当算子的最经典算法！
        Matrix<T> inverse() const {
            if (rows != cols) throw std::invalid_argument("Math Error: Inverse is only defined for square matrices.");

            // 把我们要的单位矩阵 I 直接拼贴到原矩阵 A 的右侧，变成 [A | I]
            Matrix<T> augmented(rows, cols * 2);
            for (int i = 0; i < rows; ++i) {
                for (int j = 0; j < cols; ++j) {
                    augmented(i, j) = (*this)(i, j);
                }
                augmented(i, i + cols) = T(1); // 拼接上单位矩阵的对角线 1
            }

            // 无情地对它使用高斯约当消元法削到死！
            auto [eliminated, swaps] = augmented.gaussianElimination();

            // 削完之后查证：左半边有没有成功变成单位矩阵？如果有哪怕一行对角线为 0，说明不可逆！
            for (int i = 0; i < rows; ++i) {
                if (isEssentiallyZero(eliminated(i, i))) {
                    throw std::runtime_error("Math Error: Matrix is singular and cannot be inverted.");
                }
            }

            // 如果左边顺利削成了 I，那么右半边现在的样子，就是完美的 A^{-1} !
            Matrix<T> inv(rows, cols);
            for (int i = 0; i < rows; ++i) {
                for (int j = 0; j < cols; ++j) {
                    inv(i, j) = eliminated(i, j + cols);
                }
            }
            return inv;
        }

        // =================================================================================
// 扩展线性代数工具箱 (Extended Linear Algebra Toolkit)
// =================================================================================

// [秩] Rank
        int rank() const {
            auto [reduced, swaps] = gaussianElimination();
            int r = 0;
            for (int i = 0; i < rows; ++i) {
                bool nonZero = false;
                for (int j = 0; j < cols; ++j) {
                    if (!isEssentiallyZero(reduced(i, j))) { nonZero = true; break; }
                }
                if (nonZero) r++;
            }
            return r;
        }

        // [迹] Trace
        T trace() const {
            if (rows != cols) throw std::invalid_argument("Math Error: Trace is only defined for square matrices.");
            T result = T(0);
            for (int i = 0; i < rows; ++i) result = result + (*this)(i, i);
            return result;
        }

        // [单位矩阵] Identity
        static Matrix<T> identity(int n) {
            Matrix<T> I(n, n);
            for (int i = 0; i < n; ++i) I(i, i) = T(1);
            return I;
        }

        // [Frobenius 范数] Norm
        double norm() const {
            double result = 0;
            for (size_t i = 0; i < data.size(); ++i) {
                double m = magnitudeOf(data[i]);
                result += m * m;
            }
            return std::sqrt(result);
        }

        // [子矩阵] 删除指定行和列后的余子矩阵
        Matrix<T> subMatrix(int excludeRow, int excludeCol) const {
            if (rows <= 1 || cols <= 1) throw std::invalid_argument("Matrix Error: Matrix too small for subMatrix.");
            Matrix<T> result(rows - 1, cols - 1);
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

        // [整数次幂] A^n（支持负整数幂 = 逆矩阵重复乘）
        Matrix<T> power(int n) const {
            if (rows != cols) throw std::invalid_argument("Math Error: Matrix power requires a square matrix.");
            if (n == 0) return identity(rows);
            Matrix<T> base = (n > 0) ? *this : inverse();
            int exp = std::abs(n);
            Matrix<T> result = identity(rows);
            while (exp > 0) {
                if (exp & 1) result = result * base;
                base = base * base;
                exp >>= 1;
            }
            return result;
        }

        // =================================================================================
        // 矩阵拼接 (Matrix Concatenation)
        // =================================================================================

        // [水平拼接] [A | B]
        Matrix<T> integR(const Matrix<T>& other) const {
            if (rows != other.rows) throw std::invalid_argument("Matrix Error: Row counts must match for horizontal concatenation.");
            Matrix<T> result(rows, cols + other.cols);
            for (int i = 0; i < rows; ++i) {
                for (int j = 0; j < cols; ++j) result(i, j) = (*this)(i, j);
                for (int j = 0; j < other.cols; ++j) result(i, j + cols) = other(i, j);
            }
            return result;
        }

        // [垂直拼接] [A; B]
        Matrix<T> integC(const Matrix<T>& other) const {
            if (cols != other.cols) throw std::invalid_argument("Matrix Error: Column counts must match for vertical concatenation.");
            Matrix<T> result(rows + other.rows, cols);
            for (int i = 0; i < rows; ++i)
                for (int j = 0; j < cols; ++j) result(i, j) = (*this)(i, j);
            for (int i = 0; i < other.rows; ++i)
                for (int j = 0; j < cols; ++j) result(i + rows, j) = other(i, j);
            return result;
        }

        // [对角拼接] diag(A, B)
        Matrix<T> integD(const Matrix<T>& other) const {
            Matrix<T> result(rows + other.rows, cols + other.cols);
            for (int i = 0; i < rows; ++i)
                for (int j = 0; j < cols; ++j) result(i, j) = (*this)(i, j);
            for (int i = 0; i < other.rows; ++i)
                for (int j = 0; j < other.cols; ++j) result(i + rows, j + cols) = other(i, j);
            return result;
        }

        // =================================================================================
        // 行列操作 (Row / Column Operations)
        // =================================================================================

        // [交换列]
        void swapCols(int col1, int col2) {
            if (col1 < 0 || col1 >= cols || col2 < 0 || col2 >= cols) throw std::out_of_range("Matrix Error: Column index out of bounds.");
            for (int i = 0; i < rows; ++i) {
                T temp = (*this)(i, col1);
                (*this)(i, col1) = (*this)(i, col2);
                (*this)(i, col2) = temp;
            }
        }

        // [获取某一行] 返回 1×cols 矩阵
        Matrix<T> getRow(int row) const {
            if (row < 0 || row >= rows) throw std::out_of_range("Matrix Error: Row index out of bounds.");
            Matrix<T> result(1, cols);
            for (int j = 0; j < cols; ++j) result(0, j) = (*this)(row, j);
            return result;
        }

        // [获取某一列] 返回 rows×1 矩阵
        Matrix<T> getCol(int col) const {
            if (col < 0 || col >= cols) throw std::out_of_range("Matrix Error: Column index out of bounds.");
            Matrix<T> result(rows, 1);
            for (int i = 0; i < rows; ++i) result(i, 0) = (*this)(i, col);
            return result;
        }

        // [删除某一行] 返回 (rows-1)×cols 矩阵
        Matrix<T> deleteRow(int row) const {
            if (rows <= 1) throw std::invalid_argument("Matrix Error: Cannot delete row from single-row matrix.");
            if (row < 0 || row >= rows) throw std::out_of_range("Matrix Error: Row index out of bounds.");
            Matrix<T> result(rows - 1, cols);
            int ri = 0;
            for (int i = 0; i < rows; ++i) {
                if (i == row) continue;
                for (int j = 0; j < cols; ++j) result(ri, j) = (*this)(i, j);
                ri++;
            }
            return result;
        }

        // [删除某一列] 返回 rows×(cols-1) 矩阵
        Matrix<T> deleteCol(int col) const {
            if (cols <= 1) throw std::invalid_argument("Matrix Error: Cannot delete col from single-column matrix.");
            if (col < 0 || col >= cols) throw std::out_of_range("Matrix Error: Column index out of bounds.");
            Matrix<T> result(rows, cols - 1);
            for (int i = 0; i < rows; ++i) {
                int rj = 0;
                for (int j = 0; j < cols; ++j) {
                    if (j == col) continue;
                    result(i, rj++) = (*this)(i, j);
                }
            }
            return result;
        }

        // [变形] Reshape：保持元素总数不变，重新分配行列
        Matrix<T> reshape(int newRows, int newCols) const {
            if (newRows * newCols != rows * cols) throw std::invalid_argument("Matrix Error: Element count mismatch in reshape.");
            Matrix<T> result(newRows, newCols);
            for (int idx = 0; idx < rows * cols; ++idx) {
                result.data[idx] = data[idx]; // 一维底层直接拷贝！极速！
            }
            return result;
        }

        // =================================================================================
        // 矩阵生成器 (Matrix Generators) - 均为 static
        // =================================================================================

        // [全 1 矩阵]
        static Matrix<T> ones(int r, int c) {
            Matrix<T> result(r, c);
            for (auto& val : result.data) val = T(1);
            return result;
        }

        // [全 0 矩阵]  (构造函数已经默认全0，这里只是语义糖)
        static Matrix<T> zeros(int r, int c) {
            return Matrix<T>(r, c);
        }

        // [元素求和]
        T sum() const {
            T result = T(0);
            for (const auto& val : data) result = result + val;
            return result;
        }

        // [元素求积]
        T product() const {
            T result = T(1);
            for (const auto& val : data) result = result * val;
            return result;
        }

        // [余子式] cofactor(i,j) = det(subMatrix(i,j))
        T cofactor(int row, int col) const {
            return subMatrix(row, col).determinant();
        }

        // [代数余子式] Acofactor(i,j) = (-1)^(i+j) * cofactor(i,j)
        T algebraicCofactor(int row, int col) const {
            T sign = ((row + col) % 2 == 0) ? T(1) : T(-1);
            return sign * cofactor(row, col);
        }

        // [伴随矩阵] Adjugate = transpose of cofactor matrix
        Matrix<T> adjugate() const {
            if (rows != cols) throw std::invalid_argument("Math Error: Adjugate requires a square matrix.");
            if (rows == 1) { Matrix<T> r(1, 1); r(0, 0) = T(1); return r; }
            
            // ★ 算法级降维打击：利用 adj(A) = det(A) * A^-1，将复杂度从 O(N^5) 降至 O(N^3)
            T det = determinant();
            if (!isEssentiallyZero(det)) {
                try {
                    return inverse() * det;
                } catch (...) {
                    // 浮点误差导致 inverse 判定为奇异矩阵时，静默退化为余子式法
                }
            }

            // 仅当矩阵不可逆 (det == 0) 时，才退化为 O(N^5) 的代数余子式定义法
            Matrix<T> result(rows, cols);
            for (int i = 0; i < rows; ++i)
                for (int j = 0; j < cols; ++j)
                    result(j, i) = algebraicCofactor(i, j); // 注意转置：result(j,i)
            return result;
        }

        // [永久式] Permanent (递归展开，和行列式类似但不带符号)
        T permanent() const {
            if (rows != cols) throw std::invalid_argument("Math Error: Permanent requires a square matrix.");
            if (rows > 20)
                throw std::runtime_error("Math Error: Permanent limited to 20x20 (combinatorial complexity).");
            if (rows == 1) return (*this)(0, 0);
            if (rows == 2) return (*this)(0, 0) * (*this)(1, 1) + (*this)(0, 1) * (*this)(1, 0);

            // ★ Ryser 公式：O(2^n * n) 代替 O(n!) 递归展开
            // perm(A) = (-1)^n * Σ_{S⊆{1..n}} (-1)^|S| * Π_{i=1}^{n} Σ_{j∈S} a_{ij}
            int n = rows;
            T result = T(0);
            int64_t totalSubsets = 1LL << n; // 2^n

            for (int64_t s = 1; s < totalSubsets; ++s) {
                // 计算 Π_{i=0}^{n-1} (Σ_{j∈S} a_{i,j})
                T product = T(1);
                for (int i = 0; i < n; ++i) {
                    T rowSum = T(0);
                    for (int j = 0; j < n; ++j) {
                        if (s & (1LL << j)) {
                            rowSum = rowSum + (*this)(i, j);
                        }
                    }
                    product = product * rowSum;
                }

                // (-1)^(n - |S|) 的符号
                int bits = 0;
                int64_t tmp = s;
                while (tmp) { bits += (tmp & 1); tmp >>= 1; }

                if ((n - bits) % 2 == 0)
                    result = result + product;
                else
                    result = result - product;
            }

            return result;
        }

        // [条件数] Condition number = norm(A) * norm(A^-1)
        double condition() const {
            if (rows != cols) throw std::invalid_argument("Math Error: Condition number requires a square matrix.");
            return norm() * inverse().norm();
        }

        // 用于外部访问底层数据的只读接口（方便序列化等）
        const std::vector<T>& rawData() const { return data; }

        // =================================================================================
// 矩阵泛函引擎 (Matrix Transcendental Functions)
// 基于泰勒级数：纯模板实现，自动兼容 double / Complex
// =================================================================================


        // [矩阵指数] Scaling and Squaring + Taylor 级数
        // 替代原来的纯 Taylor 展开，对任意范数矩阵稳定收敛
        Matrix<T> matExp() const {
            if (rows != cols) throw std::invalid_argument("Math Error: Matrix exp requires a square matrix.");

            int n = rows;
            double normA = norm();

            // 零矩阵直接返回单位阵
            if (normA == 0.0) return identity(n);
            int s = 0;
            double scaledNorm = normA;
            while (scaledNorm > 0.5) {
                scaledNorm /= 2.0;
                s++;
            }

            // B = A / 2^s，此时 ||B|| <= 0.5
            Matrix<T> B = (s > 0) ? (*this) / T(std::pow(2.0, s)) : *this;
            Matrix<T> result = identity(n);
            Matrix<T> term = identity(n);
            for (int k = 1; k <= 64; ++k) {
                checkInterrupt();
                term = term * B / T(static_cast<double>(k));
                result = result + term;
                // 相对收敛判定：当前项相对于结果可忽略
                if (Tol::clean(term.norm(), result.norm(), 1e3) == 0.0) break;
            }
            for (int i = 0; i < s; ++i) {
                result = result * result;
            }

            return result;
        }

        // [矩阵正弦] sin(A) = A - A³/3! + A⁵/5! - ...
        // 递推：term_k = term_{k-1} * (-A²) / ((2k)(2k+1))
        Matrix<T> matSin() const {
            if (rows != cols) throw std::invalid_argument("Math Error: Matrix sin requires a square matrix.");
            Matrix<T> negA2 = -((*this) * (*this));
            Matrix<T> term = *this;
            Matrix<T> result = *this;
            for (int k = 1; k <= 1000; ++k) {
                checkInterrupt();
                term = term * negA2 / T(static_cast<double>((2 * k) * (2 * k + 1)));
                result = result + term;
                if (Tol::clean(term.norm(), result.norm(), 1e3) == 0.0) return result;
            }
            throw std::runtime_error("Math Error: Matrix sin did not converge.");
        }

        // [矩阵余弦] cos(A) = I - A²/2! + A⁴/4! - ...
        // 递推：term_k = term_{k-1} * (-A²) / ((2k-1)(2k))
        Matrix<T> matCos() const {
            if (rows != cols) throw std::invalid_argument("Math Error: Matrix cos requires a square matrix.");
            Matrix<T> negA2 = -((*this) * (*this));
            Matrix<T> term = identity(rows);
            Matrix<T> result = identity(rows);
            for (int k = 1; k <= 1000; ++k) {
                checkInterrupt();
                term = term * negA2 / T(static_cast<double>((2 * k - 1) * (2 * k)));
                result = result + term;
                if (Tol::clean(term.norm(), result.norm(), 1e3) == 0.0) return result;
            }
            throw std::runtime_error("Math Error: Matrix cos did not converge.");
        }

        // [矩阵正切] tan(A) = sin(A) * cos(A)^{-1}
        Matrix<T> matTan() const {
            Matrix<T> c = matCos();
            if (c.rank() < rows)
                throw std::runtime_error("Math Error: Matrix cos(A) is singular, tan(A) undefined.");
            return matSin() * c.inverse();
        }

        // [矩阵双曲正弦] sinh(A) = (exp(A) - exp(-A)) / 2
        Matrix<T> matSinh() const {
            return (matExp() - (-(*this)).matExp()) / T(2.0);
        }

        // [矩阵双曲余弦] cosh(A) = (exp(A) + exp(-A)) / 2
        Matrix<T> matCosh() const {
            return (matExp() + (-(*this)).matExp()) / T(2.0);
        }

        // [矩阵双曲正切] tanh(A) = sinh(A) * cosh(A)^{-1}
        Matrix<T> matTanh() const {
            Matrix<T> c = matCosh();
            if (c.rank() < rows)
                throw std::runtime_error("Math Error: Matrix cosh(A) is singular, tanh(A) undefined.");
            return matSinh() * c.inverse();
        }

        // [矩阵对数 - 泰勒级数法] ln(I + X) = X - X²/2 + X³/3 - ...
        // 仅当 ||A - I|| < 1 时收敛。其他情况需要用对角化方法（见下方自由函数）
        Matrix<T> matLogSeries() const {
            if (rows != cols) throw std::invalid_argument("Math Error: Matrix log requires a square matrix.");
            Matrix<T> X = (*this) - identity(rows);
            if (X.norm() >= 1.0)
                throw std::runtime_error("Math Error: Matrix log series requires ||A - I|| < 1. Try diagonalization.");

            Matrix<T> term = X;
            Matrix<T> result = X;
            for (int k = 2; k <= 1000; ++k) {
                checkInterrupt();
                term = term * X * T(-1.0) * T(static_cast<double>(k - 1)) / T(static_cast<double>(k));
                result = result + term;
                if (Tol::clean(term.norm(), result.norm(), 1e3) == 0.0) return result;  // ★ 相对阈值
            }
            throw std::runtime_error("Math Error: Matrix log series did not converge.");
        }

        // =================================================================================
// 高等代数核心引擎 v2.0 (Advanced Linear Algebra Engine)
// =================================================================================

// --- 辅助工具 ---

// 共轭：double 返回自身，Complex 返回共轭复数
        static T conjugateOf(const T& val) {
            if constexpr (std::is_same_v<T, Complex>) return val.conjugate();
            else return val;
        }

        // 共轭转置 (Hermitian Transpose)
        Matrix<T> conjugateTranspose() const {
            Matrix<T> result(cols, rows);
            for (int i = 0; i < rows; ++i)
                for (int j = 0; j < cols; ++j)
                    result(j, i) = conjugateOf((*this)(i, j));
            return result;
        }

        // 转换为复数矩阵（无论 T 是 double 还是 Complex）
        Matrix<Complex> toComplexMatrix() const {
            Matrix<Complex> result(rows, cols);
            for (int i = 0; i < rows; ++i)
                for (int j = 0; j < cols; ++j) {
                    if constexpr (std::is_same_v<T, Complex>)
                        result(i, j) = (*this)(i, j);
                    else
                        result(i, j) = Complex(static_cast<double>((*this)(i, j)));
                }
            return result;
        }

        // =================================================================================
        // [QR 分解] Modified Gram-Schmidt (比你旧版的 Classical GS 数值稳定性强得多！)
        // 返回 {Q, R}，其中 Q 的列为正交规范化向量，R 为上三角
        // =================================================================================
        std::pair<Matrix<T>, Matrix<T>> qrDecomposition() const {
            if (rows < cols) throw std::invalid_argument("Math Error: QR decomposition requires rows >= cols.");
            int m = rows, n = cols;

            // 工作副本：V 的各列将被逐步正交化成 Q 的列
            Matrix<T> V(*this);
            Matrix<T> R(n, n);

            for (int j = 0; j < n; ++j) {
                checkInterrupt();
                // 计算第 j 列的范数
                double colNorm = 0;
                for (int i = 0; i < m; ++i) {
                    double mag = magnitudeOf(V(i, j));
                    colNorm += mag * mag;
                }
                colNorm = std::sqrt(colNorm);

                R(j, j) = T(colNorm);

                // 归一化第 j 列
                if (colNorm > 1e-14) {
                    for (int i = 0; i < m; ++i)
                        V(i, j) = V(i, j) / T(colNorm);
                }

                // 将后续各列对第 j 列做正交化投影（Modified GS 的核心！）
                for (int k = j + 1; k < n; ++k) {
                    // R(j,k) = <V_j, V_k> (共轭内积)
                    T dot = T(0);
                    for (int i = 0; i < m; ++i)
                        dot = dot + conjugateOf(V(i, j)) * V(i, k);
                    R(j, k) = dot;

                    // V_k = V_k - R(j,k) * V_j
                    for (int i = 0; i < m; ++i)
                        V(i, k) = V(i, k) - dot * V(i, j);
                }
            }
            // V 现在就是正交矩阵 Q (m × n)
            return { V, R };
        }

        // =================================================================================
        // [LU 分解] Doolittle 算法 + 列主元偏序选取 (Partial Pivoting)
        // 返回 {L, U, P}，满足 PA = LU
        // =================================================================================
        struct LUResult { Matrix<T> L; Matrix<T> U; Matrix<T> P; };

        // [LU 分解] Doolittle 算法 + 列主元偏序选取 (Partial Pivoting)
        LUResult luDecomposition() const {
            if (rows != cols) throw std::invalid_argument("Math Error: LU decomposition requires a square matrix.");
            int n = rows;

            Matrix<T> U(*this);
            Matrix<T> L = identity(n);
            Matrix<T> P = identity(n);

            std::vector<double> rowScale(rows, 0.0);
            for (int i = 0; i < rows; ++i) {
                for (int j = 0; j < cols; ++j) {
                    rowScale[i] = std::max(rowScale[i], magnitudeOf(U(i, j)));
                }
                rowScale[i] = std::max(rowScale[i], 1e-30);
            }

            for (int k = 0; k < n; ++k) {
                checkInterrupt();
                // 列主元选取
                int maxRow = k;
                double maxVal = magnitudeOf(U(k, k));
                for (int i = k + 1; i < n; ++i) {
                    double val = magnitudeOf(U(i, k));
                    if (val > maxVal) { maxVal = val; maxRow = i; }
                }

                if (Tol::clean(maxVal, rowScale[maxRow]) == 0.0) continue;

                // 交换 U, P 的行，以及 L 的已处理列
                if (maxRow != k) {
                    U.swapRows(k, maxRow);
                    P.swapRows(k, maxRow);
                    std::swap(rowScale[k], rowScale[maxRow]); // ★ 容差同步交换
                    for (int j = 0; j < k; ++j) {
                        T temp = L(k, j);
                        L(k, j) = L(maxRow, j);
                        L(maxRow, j) = temp;
                    }
                }

                // 消元
                T pivot = U(k, k);
                for (int i = k + 1; i < n; ++i) {
                    T factor = U(i, k) / pivot;
                    L(i, k) = factor;
                    U(i, k) = T(0); // ★ 显式置零，跳过无意义计算
                    for (int j = k + 1; j < n; ++j)
                        U(i, j) = U(i, j) - factor * U(k, j);
                }
            }

            // 清扫浮点残渣
            for (int i = 0; i < n; ++i) {
                for (int j = 0; j < n; ++j) {
                    if (Tol::clean(magnitudeOf(U(i, j)), rowScale[i]) == 0.0) U(i, j) = T(0);
                    // L 都是比例因子，最大不超过1，因此使用绝对常数容差即可
                    if (i != j && Tol::clean(magnitudeOf(L(i, j)), 1.0) == 0.0) L(i, j) = T(0);
                }
            }

            return { L, U, P };
        }

        // =================================================================================
        // [零空间 / 核] Null Space：返回一个矩阵，其列向量构成 Ax=0 的基础解系
        // =================================================================================
        Matrix<T> nullSpace() const {
            auto [rref, swaps] = gaussianElimination();

            // 找出主元列和自由列
            std::vector<int> pivotCols;
            std::vector<bool> isPivot(cols, false);

            for (int i = 0; i < rows; ++i) {
                for (int j = 0; j < cols; ++j) {
                    if (!isEssentiallyZero(rref(i, j))) {
                        pivotCols.push_back(j);
                        isPivot[j] = true;
                        break;
                    }
                }
            }

            std::vector<int> freeCols;
            for (int j = 0; j < cols; ++j)
                if (!isPivot[j]) freeCols.push_back(j);

            int nullity = static_cast<int>(freeCols.size());
            if (nullity == 0) return Matrix<T>(cols, 0); // 零空间只有零向量

            Matrix<T> result(cols, nullity);

            for (int k = 0; k < nullity; ++k) {
                int freeCol = freeCols[k];
                result(freeCol, k) = T(1); // 自由变量设为 1

                // 回代：从 RREF 的主元行中读取系数
                for (int p = 0; p < static_cast<int>(pivotCols.size()); ++p) {
                    result(pivotCols[p], k) = -rref(p, freeCol);
                }
            }
            return result;
        }

        // =================================================================================
        // [Schmidt 正交化] 返回列向量正交规范化后的矩阵（本质上就是 QR 的 Q）
        // =================================================================================
        Matrix<T> orthogonalize() const {
            if (rank() < cols) throw std::invalid_argument("Math Error: Columns must be linearly independent.");
            auto [Q, R] = qrDecomposition();
            return Q;
        }

        static Matrix<T> magic(int n) {
            if (n < 3) throw std::invalid_argument("Math Error: Magic square size must be >= 3.");
            Matrix<T> result(n, n);
            if (n % 2 == 1) {
                // === 奇数阶：罗伯法 (Siamese method) ===
                int num = 1;
                int i = 0, j = n / 2;
                result(i, j) = T(num++);
                while (num <= n * n) {
                    int newi = (i - 1 + n) % n;
                    int newj = (j + 1) % n;
                    if (!isEssentiallyZero(result(newi, newj))) {
                        newi = (i + 1) % n;
                        newj = j;
                    }
                    result(newi, newj) = T(num++);
                    i = newi;
                    j = newj;
                }
            }
            else if (n % 4 == 0) {
                // === 双偶数阶 (4k)：对称交叉翻转法 ===
                int num = 1;
                for (int i = 0; i < n; ++i)
                    for (int j = 0; j < n; ++j)
                        result(i, j) = T(num++);
                for (int i = 0; i < n; i++) {
                    for (int j = 0; j < n / 2; j++) {
                        if (i % 4 == j % 4 || i % 4 + j % 4 == 3) {
                            // 交换 (i,j) 和 (n-1-i, n-1-j)
                            T temp = result(i, j);
                            result(i, j) = result(n - 1 - i, n - 1 - j);
                            result(n - 1 - i, n - 1 - j) = temp;
                        }
                    }
                }
            }
            else {
                // === 单偶数阶 (4k+2)：斯特雷奇法 (Strachey method) ===
                int u = n / 2;
                int v = n * n / 4;
                int t = (n - 2) / 4;
                Matrix<T> sub = magic(u); // 递归生成 u 阶奇数幻方
                // 填充四个象限
                for (int i = 0; i < u; i++) {
                    for (int j = 0; j < u; j++) {
                        result(i, j) = sub(i, j);                      // 左上
                        result(i + u, j + u) = sub(i, j) + T(v);       // 右下
                        result(i + u, j) = sub(i, j) + T(3 * v);       // 左下
                        result(i, j + u) = sub(i, j) + T(2 * v);       // 右上
                    }
                }
                // 左侧列交换
                for (int i = 0; i < u; i++) {
                    for (int j = 0; j < t; j++) {
                        int col = (i == (u - 1) / 2) ? (u - 1) / 2 + j : j;
                        T temp = result(i, col);
                        result(i, col) = result(u + i, col);
                        result(u + i, col) = temp;
                    }
                }
                // 右侧列交换
                for (int j = 0; j < t - 1; j++) {
                    int col = u + (u - 1) / 2 - j;
                    for (int i = 0; i < u; i++) {
                        T temp = result(i, col);
                        result(i, col) = result(u + i, col);
                        result(u + i, col) = temp;
                    }
                }
            }
            return result;
        }
    };

    // ==========================================
    // 预定义类型别名：为我们的“动态升维”铺垫
    // ==========================================
    using RealMatrix = Matrix<double>;
    using ComplexMatrix = Matrix<Complex>;

    // =================================================================================
    // Householder 约化为上 Hessenberg 形式
    // H = Q^H * A * Q（相似变换，保持特征值不变）
    // 一次性 O(n³) 开销，使后续 QR 迭代从 O(n³)/步 降到 O(n²)/步
    // =================================================================================
    inline ComplexMatrix toHessenberg(const ComplexMatrix& A) {
        int n = A.getRows();
        if (n != A.getCols()) throw std::invalid_argument("Math Error: Hessenberg reduction requires a square matrix.");
        if (n <= 2) return A; // 2×2 以下本身就是 Hessenberg

        ComplexMatrix H(A);

        for (int k = 0; k < n - 2; ++k) {
            checkInterrupt();
            int m = n - k - 1;
            double xnorm = 0;
            for (int i = k + 1; i < n; ++i) {
                double mag = H(i, k).modulus();
                xnorm += mag * mag;
            }
            xnorm = std::sqrt(xnorm);
            if (xnorm == 0.0) continue;
            Complex alpha = H(k + 1, k);
            Complex signAlpha = (alpha.real == 0.0 && alpha.imag == 0.0)
                ? Complex(1.0)
                : alpha / Complex(alpha.modulus());

            std::vector<Complex> v(m);
            for (int i = 0; i < m; ++i)
                v[i] = H(k + 1 + i, k);
            v[0] = v[0] + signAlpha * Complex(xnorm);
            double vnorm = 0;
            for (int i = 0; i < m; ++i)
                vnorm += v[i].modulus() * v[i].modulus();
            vnorm = std::sqrt(vnorm);
            if (vnorm == 0.0) continue;
            for (int i = 0; i < m; ++i)
                v[i] = v[i] / Complex(vnorm);
            for (int j = k; j < n; ++j) {
                Complex dot(0);
                for (int i = 0; i < m; ++i)
                    dot = dot + v[i].conjugate() * H(k + 1 + i, j);
                for (int i = 0; i < m; ++i)
                    H(k + 1 + i, j) = H(k + 1 + i, j) - 2.0 * v[i] * dot;
            }
            for (int i = 0; i < n; ++i) {
                Complex dot(0);
                for (int j = 0; j < m; ++j)
                    dot = dot + H(i, k + 1 + j) * v[j];
                for (int j = 0; j < m; ++j)
                    H(i, k + 1 + j) = H(i, k + 1 + j) - 2.0 * dot * v[j].conjugate();
            }
        }
        for (int i = 2; i < n; ++i)
            for (int j = 0; j < i - 1; ++j) {
                H(i, j).real = Tol::clean(H(i, j).real, H.norm());
                H(i, j).imag = Tol::clean(H(i, j).imag, H.norm());
            }

        return H;
    }

    inline Complex eigenShift(const ComplexMatrix& A, int n) {
        Complex a = A(n - 2, n - 2), b = A(n - 2, n - 1);
        Complex c = A(n - 1, n - 2), d = A(n - 1, n - 1);
        Complex tr = a + d;
        Complex det = a * d - b * c;
        Complex disc = sqrt(tr * tr / 4.0 - det);
        Complex e1 = tr / 2.0 + disc;
        Complex e2 = tr / 2.0 - disc;
        return ((e1 - d).modulus() < (e2 - d).modulus()) ? e1 : e2;
    }
    // 核心：QR 迭代法求特征值
    // =================================================================================
// 核心：Hessenberg + Givens QR 迭代法求特征值
// 总复杂度 O(n³)，相比原版 O(n⁴~n⁵) 提升巨大
// =================================================================================
    inline std::vector<Complex> computeEigenvalues(ComplexMatrix A) {
        int n = A.getRows();
        if (n != A.getCols()) throw std::invalid_argument("Math Error: Eigenvalues require a square matrix.");

        // =============================================
        // 第一步：Householder 约化为上 Hessenberg 形式
        // 一次性 O(n³)，后续所有迭代都在此结构上进行
        // =============================================
        A = toHessenberg(A);

        std::vector<Complex> eigenvals;
        int maxIter = 10000;

        while (n > 0) {
            checkInterrupt();
            if (n == 1) {
                eigenvals.push_back(A(0, 0));
                break;
            }

            // 检查是否有已经收敛的子对角线元素（可以直接摘取）
            // 这在矩阵本身接近三角时能大幅加速
            bool deflated = false;
            for (int i = n - 1; i >= 1; --i) {
                double offDiag = A(i, i - 1).modulus();
                double scale = A(i, i).modulus() + A(i - 1, i - 1).modulus();
                if (Tol::clean(offDiag, scale, 1e7) == 0.0) {
                    A(i, i - 1) = Complex(0);
                    if (i == n - 1) {
                        // 最后一行已解耦，直接摘取特征值
                        eigenvals.push_back(A(n - 1, n - 1));
                        ComplexMatrix sub(n - 1, n - 1);
                        for (int r = 0; r < n - 1; ++r)
                            for (int c = 0; c < n - 1; ++c)
                                sub(r, c) = A(r, c);
                        A = sub;
                        n--;
                        deflated = true;
                        break;
                    }
                }
            }
            if (deflated) continue;

            bool converged = false;
            for (int iter = 0; iter < maxIter; ++iter) {
                Complex shift = eigenShift(A, n);
                for (int i = 0; i < n; ++i)
                    A(i, i) = A(i, i) - shift;
                std::vector<Complex> cs(n - 1), ss(n - 1);
                for (int i = 0; i < n - 1; ++i) {
                    Complex a = A(i, i), b = A(i + 1, i);
                    double r = std::sqrt(a.modulus() * a.modulus() + b.modulus() * b.modulus());

                    if (r == 0.0) {
                        cs[i] = Complex(1);
                        ss[i] = Complex(0);
                        continue;
                    }
                    cs[i] = a / Complex(r);
                    ss[i] = b / Complex(r);
                    for (int j = i; j < n; ++j) {
                        Complex t1 = cs[i].conjugate() * A(i, j) + ss[i].conjugate() * A(i + 1, j);
                        Complex t2 = -(ss[i]) * A(i, j) + cs[i] * A(i + 1, j);
                        A(i, j) = t1;
                        A(i + 1, j) = t2;
                    }
                }
                for (int i = 0; i < n - 1; ++i) {
                    int rowLimit = std::min(i + 2, n - 1);
                    for (int j = 0; j <= rowLimit; ++j) {
                        Complex t1 = A(j, i) * cs[i] + A(j, i + 1) * ss[i];
                        Complex t2 = A(j, i + 1) * cs[i].conjugate() - A(j, i) * ss[i].conjugate();
                        A(j, i) = t1;
                        A(j, i + 1) = t2;
                    }
                }
                for (int i = 0; i < n; ++i)
                    A(i, i) = A(i, i) + shift;
                double offDiag = A(n - 1, n - 2).modulus();
                double scale = A(n - 1, n - 1).modulus() + A(n - 2, n - 2).modulus();
                if (Tol::clean(offDiag, scale, 1e7) == 0.0) {
                    eigenvals.push_back(A(n - 1, n - 1));
                    ComplexMatrix deflatedMat(n - 1, n - 1);
                    for (int i = 0; i < n - 1; ++i)
                        for (int j = 0; j < n - 1; ++j)
                            deflatedMat(i, j) = A(i, j);
                    A = deflatedMat;
                    n--;
                    converged = true;
                    break;
                }
            }
            if (!converged)
                throw std::runtime_error("Math Error: QR algorithm failed to converge.");
        }
        return eigenvals;
    }
    // 便利重载：实数矩阵自动升维到复数域再求解
    inline std::vector<Complex> computeEigenvalues(const RealMatrix& A) {
        return computeEigenvalues(A.toComplexMatrix());
    }
    // 特征向量计算：对每个特征值 λ，求 (A - λI) 的零空间
    // 返回 {特征向量矩阵 P, 对齐后的特征值列表}
    inline std::pair<ComplexMatrix, std::vector<Complex>> computeEigenvectorsAligned(const ComplexMatrix& A, const std::vector<Complex>& eigenvals) {
        int n = A.getRows();
        ComplexMatrix I = ComplexMatrix::identity(n);
        
        double maxEvMod = 0.0;
        for (const auto& ev : eigenvals) {
            maxEvMod = std::max(maxEvMod, ev.modulus());
        }

        // 去重特征值（相同特征值只求一次零空间）
        std::vector<Complex> unique;
        for (const auto& ev : eigenvals) {
            bool found = false;
            for (const auto& u : unique) {
                // 结合相对容差与绝对底噪容差：
                // 1. 对于常规大小的特征值，参考它们自身的模长
                // 2. 对于极微小的特征值，使用 maxEvMod * 1e-5 作为底噪参考
                // 配合 1e6 的清洗因子，最终的绝对底噪容差为 maxEvMod * 1e-11
                double ref = std::max({ev.modulus(), u.modulus(), maxEvMod * 1e-5});
                if (Tol::clean((ev - u).modulus(), ref, 1e6) == 0.0) { 
                    found = true; 
                    break; 
                }
            }
            if (!found) unique.push_back(ev);
        }
        
        std::vector<ComplexMatrix> nullSpaces;
        std::vector<Complex> alignedEigenvals;
        int totalCols = 0;
        for (const auto& lambda : unique) {
            ComplexMatrix B = A - lambda * I;
            ComplexMatrix ns = B.nullSpace();
            int cols = ns.getCols();
            nullSpaces.push_back(ns);
            for (int j = 0; j < cols; ++j) {
                alignedEigenvals.push_back(lambda);
            }
            totalCols += cols;
        }
        
        if (totalCols == 0) throw std::runtime_error("Math Error: No eigenvectors found.");
        
        ComplexMatrix P(n, totalCols);
        int currentCol = 0;
        for (const auto& ns : nullSpaces) {
            int cols = ns.getCols();
            for (int j = 0; j < cols; ++j) {
                for (int i = 0; i < n; ++i) {
                    P(i, currentCol) = ns(i, j);
                }
                currentCol++;
            }
        }
        return { P, alignedEigenvals };
    }

    // 兼容旧版接口：仅返回特征向量矩阵 P
    inline ComplexMatrix computeEigenvectors(const ComplexMatrix& A, const std::vector<Complex>& eigenvals) {
        return computeEigenvectorsAligned(A, eigenvals).first;
    }

    // 对角化：返回 {P, D}，满足 A = P * D * P^(-1)
    inline std::pair<ComplexMatrix, ComplexMatrix> diagonalize(const ComplexMatrix& A) {
        int n = A.getRows();
        if (n != A.getCols()) throw std::invalid_argument("Math Error: Diagonalization requires a square matrix.");
        auto eigenvals = computeEigenvalues(A);
        auto [P, alignedEigenvals] = computeEigenvectorsAligned(A, eigenvals);
        if (P.getCols() != n)
            throw std::runtime_error("Math Error: Matrix is not diagonalizable (insufficient eigenvectors).");
        ComplexMatrix D(n, n);
        for (int i = 0; i < n; ++i)
            D(i, i) = alignedEigenvals[i];
        return { P, D };
    }


    inline ComplexMatrix matSqrtIterative(const ComplexMatrix& A) {
        if (A.getRows() != A.getCols())
            throw std::invalid_argument("Math Error: Matrix sqrt requires a square matrix.");
        int n = A.getRows();
        if (A.rank() < n)
            throw std::runtime_error("Math Error: Iterative matrix sqrt requires a non-singular matrix.");
        ComplexMatrix Y = A;
        ComplexMatrix Z = ComplexMatrix::identity(n);
        for (int iter = 0; iter < 100; ++iter) {
            checkInterrupt();
            ComplexMatrix Yinv, Zinv;
            try {
                Yinv = Y.inverse();
                Zinv = Z.inverse();
            }
            catch (...) {
                throw std::runtime_error(
                    "Math Error: Matrix square root failed (singular intermediate matrix).");
            }
            ComplexMatrix Ynew = (Y + Zinv) * Complex(0.5);
            ComplexMatrix Znew = (Z + Yinv) * Complex(0.5);
            double diff = (Ynew - Y).norm();
            Y = Ynew;
            Z = Znew;
            if (Tol::clean(diff, Y.norm(), 1e4) == 0.0)
                return Y;
        }
        throw std::runtime_error(
            "Math Error: Matrix square root iteration did not converge.");
    }


    inline ComplexMatrix matSqrt(const ComplexMatrix& A);
    // =================================================================================
// 矩阵对数 (对角化法)：ln(A) = P * diag(ln(λ_i)) * P^{-1}
// =================================================================================
    inline ComplexMatrix matLog(const ComplexMatrix& A) {
        if (A.getRows() != A.getCols())
            throw std::invalid_argument("Math Error: Matrix log requires a square matrix.");

        int n = A.getRows();
        if (A.rank() < n)
            throw std::runtime_error("Math Error: Matrix logarithm undefined for singular matrices.");

        ComplexMatrix I = ComplexMatrix::identity(n);

        // 路径 1：||A - I|| < 1 时直接用 Taylor 级数（最快最精确）
        if ((A - I).norm() < 1.0) {
            return A.matLogSeries();
        }

        // 路径 2：尝试对角化
        try {
            auto [P, D] = diagonalize(A);

            ComplexMatrix logD(n, n);
            for (int i = 0; i < n; ++i) {
                Complex eigenval = D(i, i);
                if (eigenval.real == 0.0 && eigenval.imag == 0.0)
                    throw std::runtime_error("Math Error: Matrix logarithm undefined (zero eigenvalue).");
                logD(i, i) = log(eigenval);
            }

            return P * logD * P.inverse();
        }
        catch (const std::runtime_error& e) {
            std::string msg = e.what();

            // 零特征值错误，直接向上传递（这个信息本身是准确的）
            if (msg.find("zero eigenvalue") != std::string::npos) throw;

            // 路径 3：对角化失败，尝试 Schur 逼近法
            // 反复开方使矩阵靠近 I，然后用 Taylor 级数
            try {
                ComplexMatrix B = A;
                int sqrtCount = 0;
                const int maxSqrtSteps = 64;

                while ((B - I).norm() >= 1.0 && sqrtCount < maxSqrtSteps) {
                    checkInterrupt();
                    B = matSqrtIterative(B);  // ★ 自闭合迭代，不会回调 matLog
                    sqrtCount++;
                }

                if ((B - I).norm() >= 1.0)
                    throw std::runtime_error("convergence failed");

                // log(A) = 2^sqrtCount * log(B)
                ComplexMatrix logB = B.matLogSeries();
                return logB * Complex(std::pow(2.0, sqrtCount));
            }
            catch (const EngineInterruptError&) {
                throw;
            }
            catch (...) {
                // 所有路径都失败，给出清晰的最终诊断
                throw std::runtime_error(
                    "Math Error: Matrix logarithm failed. "
                    "The matrix may be non-diagonalizable or singular. "
                    "log(A) requires A to be invertible and diagonalizable, "
                    "or sufficiently close to the identity matrix."
                );
            }
        }
    }

    inline ComplexMatrix matLog(const RealMatrix& A) {
        return matLog(A.toComplexMatrix());
    }

    // =================================================================================
    // 矩阵开方 (对角化法)：sqrt(A) = P * diag(sqrt(λ_i)) * P^{-1}
    // =================================================================================
    inline ComplexMatrix matSqrt(const ComplexMatrix& A) {
        if (A.getRows() != A.getCols())
            throw std::invalid_argument("Math Error: Matrix sqrt requires a square matrix.");

        int n = A.getRows();

        // 路径 1：对角化（最快最精确）
        try {
            auto [P, D] = diagonalize(A);

            ComplexMatrix sqrtD(n, n);
            for (int i = 0; i < n; ++i) {
                sqrtD(i, i) = sqrt(D(i, i));
            }

            return P * sqrtD * P.inverse();
        }
        catch (const std::runtime_error& e) {
            std::string msg = e.what();
            if (msg.find("singular") != std::string::npos) throw;

            // 路径 2：Denman-Beavers 迭代（自闭合，不依赖 matLog）
            try {
                return matSqrtIterative(A);
            }
            catch (const EngineInterruptError&) {
                throw;
            }
            catch (...) {}

            // 路径 3：exp(log(A) * 0.5)
            // ★ 此处安全：matLog 路径 3 调用的是 matSqrtIterative 而非 matSqrt，无循环
            try {
                return (matLog(A) * Complex(0.5)).matExp();
            }
            catch (const EngineInterruptError&) {
                throw;
            }
            catch (...) {
                throw std::runtime_error(
                    "Math Error: Matrix square root failed. "
                    "The matrix may be non-diagonalizable or singular. "
                    "sqrt(A) requires A to be invertible."
                );
            }
        }
    }

    inline ComplexMatrix matSqrt(const RealMatrix& A) {
        return matSqrt(A.toComplexMatrix());
    }

    // =================================================================================
    // 矩阵乘方 (通用)：A^B = exp(B * ln(A))
    // =================================================================================
    inline ComplexMatrix matPow(const ComplexMatrix& A, const ComplexMatrix& B) {
        try {
            return (B * matLog(A)).matExp();
        }
        catch (const EngineInterruptError&) {
            throw;
        }
        catch (const std::runtime_error& e) {
            std::string msg = e.what();
            // 如果 matLog 已经给出了清晰的诊断，直接传递
            if (msg.find("Matrix logarithm") != std::string::npos) throw;
            // 否则包装一层上下文
            throw std::runtime_error(
                "Math Error: Matrix power A^B failed. " + std::string(e.what())
            );
        }
    }

    // =================================================================================
// StringMatrix — 字符串矩阵（独立实现，不继承数值模板）
// =================================================================================
    class StringMatrix {
    private:
        int rows;
        int cols;
        std::vector<std::string> data;

    public:
        StringMatrix() : rows(0), cols(0) {}
        StringMatrix(int r, int c) : rows(r), cols(c), data(r* c, "") {}
        StringMatrix(int r, int c, const std::vector<std::string>& flat)
            : rows(r), cols(c), data(flat) {
            if (static_cast<int>(flat.size()) != r * c)
                throw std::invalid_argument("StringMatrix Error: Data size does not match dimensions.");
        }
        explicit StringMatrix(const std::string& s) : rows(1), cols(1), data(1, s) {}

        int getRows() const { return rows; }
        int getCols() const { return cols; }
        bool isScalar() const { return rows == 1 && cols == 1; }
        const std::vector<std::string>& rawData() const { return data; }

        std::string& operator()(int r, int c) {
            if (r < 0 || r >= rows || c < 0 || c >= cols)
                throw std::out_of_range("StringMatrix Error: Index out of bounds.");
            return data[r * cols + c];
        }
        const std::string& operator()(int r, int c) const {
            if (r < 0 || r >= rows || c < 0 || c >= cols)
                throw std::out_of_range("StringMatrix Error: Index out of bounds.");
            return data[r * cols + c];
        }

        StringMatrix transpose() const {
            StringMatrix result(cols, rows);
            for (int i = 0; i < rows; ++i)
                for (int j = 0; j < cols; ++j)
                    result(j, i) = (*this)(i, j);
            return result;
        }

        StringMatrix getRow(int r) const {
            if (r < 0 || r >= rows) throw std::out_of_range("StringMatrix Error: Row index out of bounds.");
            StringMatrix result(1, cols);
            for (int j = 0; j < cols; ++j) result(0, j) = (*this)(r, j);
            return result;
        }

        StringMatrix getCol(int c) const {
            if (c < 0 || c >= cols) throw std::out_of_range("StringMatrix Error: Column index out of bounds.");
            StringMatrix result(rows, 1);
            for (int i = 0; i < rows; ++i) result(i, 0) = (*this)(i, c);
            return result;
        }

        StringMatrix deleteRow(int r) const {
            if (rows <= 1) throw std::invalid_argument("StringMatrix Error: Cannot delete row from single-row matrix.");
            if (r < 0 || r >= rows) throw std::out_of_range("StringMatrix Error: Row index out of bounds.");
            StringMatrix result(rows - 1, cols);
            int ri = 0;
            for (int i = 0; i < rows; ++i) {
                if (i == r) continue;
                for (int j = 0; j < cols; ++j) result(ri, j) = (*this)(i, j);
                ri++;
            }
            return result;
        }

        StringMatrix deleteCol(int c) const {
            if (cols <= 1) throw std::invalid_argument("StringMatrix Error: Cannot delete col from single-column matrix.");
            if (c < 0 || c >= cols) throw std::out_of_range("StringMatrix Error: Column index out of bounds.");
            StringMatrix result(rows, cols - 1);
            for (int i = 0; i < rows; ++i) {
                int rj = 0;
                for (int j = 0; j < cols; ++j) {
                    if (j == c) continue;
                    result(i, rj++) = (*this)(i, j);
                }
            }
            return result;
        }

        StringMatrix integR(const StringMatrix& other) const {
            if (rows != other.rows)
                throw std::invalid_argument("StringMatrix Error: Row counts must match for horizontal concatenation.");
            StringMatrix result(rows, cols + other.cols);
            for (int i = 0; i < rows; ++i) {
                for (int j = 0; j < cols; ++j) result(i, j) = (*this)(i, j);
                for (int j = 0; j < other.cols; ++j) result(i, j + cols) = other(i, j);
            }
            return result;
        }

        StringMatrix integC(const StringMatrix& other) const {
            if (cols != other.cols)
                throw std::invalid_argument("StringMatrix Error: Column counts must match for vertical concatenation.");
            StringMatrix result(rows + other.rows, cols);
            for (int i = 0; i < rows; ++i)
                for (int j = 0; j < cols; ++j) result(i, j) = (*this)(i, j);
            for (int i = 0; i < other.rows; ++i)
                for (int j = 0; j < cols; ++j) result(i + rows, j) = other(i, j);
            return result;
        }

        StringMatrix reshape(int newRows, int newCols) const {
            if (newRows * newCols != rows * cols)
                throw std::invalid_argument("StringMatrix Error: Element count mismatch in reshape.");
            return StringMatrix(newRows, newCols, data);
        }

        void swapRows(int r1, int r2) {
            if (r1 < 0 || r1 >= rows || r2 < 0 || r2 >= rows)
                throw std::out_of_range("StringMatrix Error: Row index out of bounds.");
            for (int j = 0; j < cols; ++j) std::swap((*this)(r1, j), (*this)(r2, j));
        }

        void swapCols(int c1, int c2) {
            if (c1 < 0 || c1 >= cols || c2 < 0 || c2 >= cols)
                throw std::out_of_range("StringMatrix Error: Column index out of bounds.");
            for (int i = 0; i < rows; ++i) std::swap((*this)(i, c1), (*this)(i, c2));
        }

        friend std::ostream& operator<<(std::ostream& out, const StringMatrix& m) {
            if (m.rows == 0 || m.cols == 0) { out << "[empty]"; return out; }

            // 计算列宽
            std::vector<size_t> colWidths(m.cols, 0);
            for (int i = 0; i < m.rows; ++i)
                for (int j = 0; j < m.cols; ++j) {
                    size_t w = m(i, j).size() + 2; // +2 for quotes
                    if (w > colWidths[j]) colWidths[j] = w;
                }

            if (jc::g_printMatrix2D) {
                for (int i = 0; i < m.rows; ++i) {
                    out << "[";
                    for (int j = 0; j < m.cols; ++j) {
                        std::string cell = "\"" + m(i, j) + "\"";
                        size_t padding = colWidths[j] - cell.size();
                        for (size_t p = 0; p < padding; ++p) out << ' ';
                        out << cell;
                        if (j < m.cols - 1) out << ", ";
                    }
                    out << "]";
                    if (i < m.rows - 1) out << "\n";
                }
            } else {
                out << "[";
                for (int i = 0; i < m.rows; ++i) {
                    for (int j = 0; j < m.cols; ++j) {
                        out << "\"" << m(i, j) << "\"";
                        if (j < m.cols - 1) out << ", ";
                    }
                    if (i < m.rows - 1) out << "; ";
                }
                out << "]";
            }
            return out;
        }
    };
} // namespace jc
#endif // JC2_MATRIX_H
