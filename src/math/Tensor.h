#ifndef JC2_TENSOR_H
#define JC2_TENSOR_H

#include <vector>
#include <memory>
#include <stdexcept>
#include <numeric>
#include <string>
#include <functional>
#include <iostream>

namespace jc {

    // ========================================================================
    // 1. 数据类型 (DType)
    // ========================================================================
    enum class DType {
        Float32,
        Float64,
        Int32,
        Int64
    };

    // ========================================================================
    // 2. 类型擦除的底层存储 (Type-erased Storage)
    // ========================================================================
    struct TensorStorageBase {
        virtual ~TensorStorageBase() = default;
        virtual DType dtype() const = 0;
        virtual void* data_ptr() = 0;
        virtual size_t element_size() const = 0;
    };

    template <typename T>
    struct TensorStorage : public TensorStorageBase {
        std::vector<T> data;

        explicit TensorStorage(size_t size) : data(size, T(0)) {}
        explicit TensorStorage(const std::vector<T>& d) : data(d) {}

        DType dtype() const override {
            if constexpr (std::is_same_v<T, float>) return DType::Float32;
            else if constexpr (std::is_same_v<T, double>) return DType::Float64;
            else if constexpr (std::is_same_v<T, int32_t>) return DType::Int32;
            else if constexpr (std::is_same_v<T, int64_t>) return DType::Int64;
            throw std::runtime_error("Tensor Error: Unsupported DType.");
        }

        void* data_ptr() override { return data.data(); }
        size_t element_size() const override { return sizeof(T); }
    };

    // ========================================================================
    // 3. 动态计算图节点 (Autograd Node)
    // ========================================================================
    class Tensor; // 前向声明

    struct BackwardNode {
        virtual ~BackwardNode() = default;
        // 反向传播接口：接收上游传来的梯度，计算当前节点的梯度并分发给子节点
        virtual void apply() = 0; 
    };

    // ========================================================================
    // 4. 张量句柄 (Tensor Handle) - 包含 Shape, Strides 和 Autograd 元数据
    // ========================================================================
    class Tensor {
    public:
        std::shared_ptr<TensorStorageBase> storage;
        size_t offset = 0;
        std::vector<int> shape;
        std::vector<int> strides;

        // Autograd 字段
        bool requires_grad = false;
        bool is_leaf = true;
        std::shared_ptr<Tensor> grad;
        std::shared_ptr<BackwardNode> grad_fn;

        Tensor() = default;

        // 基础构造函数
        Tensor(std::vector<int> s, DType type = DType::Float64, bool req_grad = false) 
            : shape(std::move(s)), requires_grad(req_grad) {
            strides = calcStrides(shape);
            size_t total_elements = numel();
            
            if (type == DType::Float64) storage = std::make_shared<TensorStorage<double>>(total_elements);
            else if (type == DType::Float32) storage = std::make_shared<TensorStorage<float>>(total_elements);
            else if (type == DType::Int32) storage = std::make_shared<TensorStorage<int32_t>>(total_elements);
            else if (type == DType::Int64) storage = std::make_shared<TensorStorage<int64_t>>(total_elements);
            else throw std::runtime_error("Tensor Error: Unknown DType.");
        }

        int dim() const { return static_cast<int>(shape.size()); }
        
        size_t numel() const {
            if (shape.empty()) return 0;
            size_t n = 1;
            for (int s : shape) n *= s;
            return n;
        }

        bool is_contiguous() const {
            auto expected = calcStrides(shape);
            return strides == expected && offset == 0;
        }

        // ====================================================================
        // 广播机制核心 (Broadcasting Core)
        // ====================================================================
        static std::vector<int> broadcastShapes(const std::vector<int>& shapeA, const std::vector<int>& shapeB) {
            std::vector<int> out_shape;
            int ndimA = static_cast<int>(shapeA.size());
            int ndimB = static_cast<int>(shapeB.size());
            int ndimOut = std::max(ndimA, ndimB);
            out_shape.resize(ndimOut);
            
            for (int i = 0; i < ndimOut; ++i) {
                int dimA = (i < ndimOut - ndimA) ? 1 : shapeA[i - (ndimOut - ndimA)];
                int dimB = (i < ndimOut - ndimB) ? 1 : shapeB[i - (ndimOut - ndimB)];
                
                if (dimA != dimB && dimA != 1 && dimB != 1) {
                    throw std::runtime_error("Tensor Error: Shapes are not broadcastable.");
                }
                out_shape[i] = std::max(dimA, dimB);
            }
            return out_shape;
        }

        static std::vector<int> broadcastStrides(const std::vector<int>& orig_shape, const std::vector<int>& orig_strides, const std::vector<int>& out_shape) {
            int ndimOrig = static_cast<int>(orig_shape.size());
            int ndimOut = static_cast<int>(out_shape.size());
            std::vector<int> new_strides(ndimOut, 0);
            
            for (int i = 0; i < ndimOut; ++i) {
                int orig_idx = i - (ndimOut - ndimOrig);
                if (orig_idx >= 0) {
                    if (orig_shape[orig_idx] == out_shape[i]) {
                        new_strides[i] = orig_strides[orig_idx];
                    } else if (orig_shape[orig_idx] == 1) {
                        new_strides[i] = 0; // ★ 核心魔法：广播维度的步长设为 0，实现零拷贝原地踏步！
                    }
                }
            }
            return new_strides;
        }

        static size_t getFlatIndex(size_t linear_idx, const std::vector<int>& out_shape, const std::vector<int>& strides) {
            size_t flat_idx = 0;
            size_t remain = linear_idx;
            for (int i = static_cast<int>(out_shape.size()) - 1; i >= 0; --i) {
                size_t coord = remain % out_shape[i];
                remain /= out_shape[i];
                flat_idx += coord * strides[i];
            }
            return flat_idx;
        }

        // 零拷贝视图 (Zero-copy View)
        Tensor view(const std::vector<int>& new_shape) const {
            if (!is_contiguous()) throw std::runtime_error("Tensor Error: view() requires a contiguous tensor.");
            Tensor t = *this; // 浅拷贝句柄
            t.shape = new_shape;
            t.strides = calcStrides(new_shape);
            if (t.numel() != numel()) throw std::runtime_error("Tensor Error: Shape mismatch in view().");
            return t;
        }

        // 触发反向传播
        void backward() {
            if (!requires_grad) throw std::runtime_error("Tensor Error: Tensor does not require grad.");
            if (!grad) {
                // 默认初始梯度为 1.0
                grad = std::make_shared<Tensor>(shape, storage->dtype(), false);
                // TODO: Fill grad with 1.0
            }
            if (grad_fn) {
                grad_fn->apply();
            }
        }

    private:
        static std::vector<int> calcStrides(const std::vector<int>& s) {
            if (s.empty()) return {};
            std::vector<int> st(s.size(), 1);
            for (int i = static_cast<int>(s.size()) - 2; i >= 0; --i) {
                st[i] = st[i + 1] * s[i + 1];
            }
            return st;
        }
    };

    // ========================================================================
    // 5. 算子示例：加法 (Forward + Backward)
    // ========================================================================
    struct AddBackward : public BackwardNode {
        Tensor a, b, out;
        AddBackward(Tensor a, Tensor b, Tensor out) : a(std::move(a)), b(std::move(b)), out(std::move(out)) {}
        
        void apply() override {
            // 链式法则：out 的梯度直接传递给 a 和 b
            if (a.requires_grad) {
                if (!a.grad) a.grad = std::make_shared<Tensor>(a.shape, a.storage->dtype());
                // TODO: a.grad += out.grad (需要处理广播的逆运算 sum)
            }
            if (b.requires_grad) {
                if (!b.grad) b.grad = std::make_shared<Tensor>(b.shape, b.storage->dtype());
                // TODO: b.grad += out.grad
            }
            // 继续向上传播
            if (a.grad_fn) a.grad_fn->apply();
            if (b.grad_fn) b.grad_fn->apply();
        }
    };

    template <typename T>
    void tensor_add_impl(const Tensor& a, const Tensor& b, Tensor& out) {
        const T* ptrA = static_cast<const T*>(a.storage->data_ptr()) + a.offset;
        const T* ptrB = static_cast<const T*>(b.storage->data_ptr()) + b.offset;
        T* ptrOut = static_cast<T*>(out.storage->data_ptr()) + out.offset;
        
        auto stridesA = Tensor::broadcastStrides(a.shape, a.strides, out.shape);
        auto stridesB = Tensor::broadcastStrides(b.shape, b.strides, out.shape);
        
        size_t total = out.numel();
        for (size_t i = 0; i < total; ++i) {
            size_t idxA = Tensor::getFlatIndex(i, out.shape, stridesA);
            size_t idxB = Tensor::getFlatIndex(i, out.shape, stridesB);
            ptrOut[i] = ptrA[idxA] + ptrB[idxB];
        }
    }

    inline Tensor tensor_add(const Tensor& a, const Tensor& b) {
        if (a.storage->dtype() != b.storage->dtype()) {
            throw std::runtime_error("Tensor Error: DType mismatch in add.");
        }

        std::vector<int> out_shape = Tensor::broadcastShapes(a.shape, b.shape);
        Tensor out(out_shape, a.storage->dtype(), a.requires_grad || b.requires_grad);
        out.is_leaf = false;

        if (a.storage->dtype() == DType::Float64) tensor_add_impl<double>(a, b, out);
        else if (a.storage->dtype() == DType::Float32) tensor_add_impl<float>(a, b, out);
        else if (a.storage->dtype() == DType::Int32) tensor_add_impl<int32_t>(a, b, out);
        else if (a.storage->dtype() == DType::Int64) tensor_add_impl<int64_t>(a, b, out);

        // 构建计算图
        if (out.requires_grad) {
            out.grad_fn = std::make_shared<AddBackward>(a, b, out);
        }
        return out;
    }

} // namespace jc

#endif // JC2_TENSOR_H
