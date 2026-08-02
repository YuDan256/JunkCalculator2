#ifndef JC2_SYMMATRIX_H
#define JC2_SYMMATRIX_H

#include "Symbolic.h"
#include <vector>
#include <stdexcept>
#include <iostream>
#include <string>

namespace jc {

    class SymMatrix {
    private:
        int rows;
        int cols;
        std::vector<SymExpr> data; // 极致连续的一维内存，装载 SymExpr 代理类

    public:
        // --- 构造函数 ---
        SymMatrix(int r, int c);
        SymMatrix(int r, int c, const std::vector<SymExpr>& flat_data);
        explicit SymMatrix(const SymExpr& num);
        SymMatrix();

        // --- 基础访问 ---
        int getRows() const { return rows; }
        int getCols() const { return cols; }
        bool isScalar() const { return rows == 1 && cols == 1; }
        const std::vector<SymExpr>& rawData() const { return data; }

        SymExpr& operator()(int row, int col);
        const SymExpr& operator()(int row, int col) const;

        // --- 零等价探测器 ---
        // 专门用于矩阵消元、求逆时的安全零判定
        static bool isSymZero(const SymExpr& expr);

        // --- 基础算术 ---
        SymMatrix operator+(const SymMatrix& other) const;
        SymMatrix operator-(const SymMatrix& other) const;
        SymMatrix operator-() const;
        SymMatrix operator*(const SymExpr& scalar) const;
        friend SymMatrix operator*(const SymExpr& scalar, const SymMatrix& rhs);
        SymMatrix operator/(const SymExpr& scalar) const;

        // 矩阵乘法 (内置防膨胀机制)
        SymMatrix operator*(const SymMatrix& other) const;

        // 标量加减 (A ± c*I)
        SymMatrix operator+(const SymExpr& scalar) const;
        friend SymMatrix operator+(const SymExpr& scalar, const SymMatrix& rhs);
        SymMatrix operator-(const SymExpr& scalar) const;
        friend SymMatrix operator-(const SymExpr& scalar, const SymMatrix& rhs);

        // --- 结构操作 ---
        SymMatrix transpose() const;
        SymMatrix subMatrix(int excludeRow, int excludeCol) const;
        void swapRows(int row1, int row2);
        void swapCols(int col1, int col2);
        SymMatrix getRow(int r) const;
        SymMatrix getCol(int c) const;
        SymMatrix deleteRow(int r) const;
        SymMatrix deleteCol(int c) const;
        SymMatrix integR(const SymMatrix& other) const;
        SymMatrix integC(const SymMatrix& other) const;
        SymMatrix integD(const SymMatrix& other) const;
        SymMatrix reshape(int newRows, int newCols) const;

        static SymMatrix identity(int n);
        static SymMatrix zeros(int r, int c);
        static SymMatrix ones(int r, int c);

        // --- 高级线性代数 (将在后续步骤实现) ---
        SymExpr determinant() const;
        SymExpr cofactor(int row, int col) const;
        SymExpr algebraicCofactor(int row, int col) const;
        SymMatrix adjugate() const;
        SymMatrix inverse() const;
        SymMatrix power(int n) const;
        SymExpr trace() const;
        
        SymExpr sum() const;
        SymExpr product() const;
        SymMatrix conjugateTranspose() const;
        SymMatrix nullSpace() const;
        SymMatrix orthogonalize() const;
        int rank() const;
        SymExpr norm() const;
        SymExpr condition() const;
        SymExpr permanent() const;
        
        SymExpr charPoly(const std::string& var) const;
        std::vector<SymExpr> eigenvalues() const;
        std::vector<std::pair<SymExpr, SymMatrix>> eigenvectors() const;
        SymMatrix solve(const SymMatrix& b) const;
        
        std::pair<SymMatrix, SymMatrix> lu() const;
        std::pair<SymMatrix, SymMatrix> qr() const;
        std::pair<SymMatrix, SymMatrix> diagonalize() const;

        // --- CAS 深度联动 (将在后续步骤实现) ---
        SymMatrix diff(const std::string& var) const;
        SymMatrix integ(const std::string& var) const;
        SymMatrix limit(const std::string& var, const SymExpr& val, const std::string& dir = "") const;
        SymMatrix subs(const std::string& var, const SymExpr& val) const;
        SymMatrix simplify() const;
        SymMatrix expand() const;
        SymMatrix evalFloat() const;
        SymMatrix evalValue() const;

        // --- 格式化输出 ---
        friend std::ostream& operator<<(std::ostream& out, const SymMatrix& m);
    };

} // namespace jc

#endif // JC2_SYMMATRIX_H
