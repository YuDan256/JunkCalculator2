#ifndef JC2_TENSOR_H
#define JC2_TENSOR_H

#include <vector>
#include <memory>
#include <stdexcept>
#include <numeric>
#include <string>
#include <functional>
#include <iostream>
#include <cmath>
#include <sstream>
#include <algorithm>
#include <cassert>
#include <unordered_set>
#include <random>
#include <future>

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

    inline std::string dtypeToString(DType d) {
        switch (d) {
            case DType::Float32: return "float32";
            case DType::Float64: return "float64";
            case DType::Int32:   return "int32";
            case DType::Int64:   return "int64";
        }
        return "unknown";
    }

    inline DType stringToDType(const std::string& s) {
        if (s == "float32" || s == "f32") return DType::Float32;
        if (s == "float64" || s == "f64" || s == "double") return DType::Float64;
        if (s == "int32" || s == "i32") return DType::Int32;
        if (s == "int64" || s == "i64") return DType::Int64;
        throw std::runtime_error("Tensor Error: Unknown dtype string '" + s + "'.");
    }

    // ========================================================================
    // 2. 类型擦除的底层存储 (Type-erased Storage)
    // ========================================================================
    struct TensorStorageBase {
        virtual ~TensorStorageBase() = default;
        virtual DType dtype() const = 0;
        virtual void* data_ptr() = 0;
        virtual const void* data_ptr() const = 0;
        virtual size_t element_size() const = 0;
        virtual std::shared_ptr<TensorStorageBase> clone() const = 0;
        virtual size_t size() const = 0;
    };

    template <typename T>
    struct TensorStorage : public TensorStorageBase {
        std::vector<T> data;

        explicit TensorStorage(size_t sz) : data(sz, T(0)) {}
        explicit TensorStorage(const std::vector<T>& d) : data(d) {}
        TensorStorage(size_t sz, T fill_val) : data(sz, fill_val) {}

        DType dtype() const override {
            if constexpr (std::is_same_v<T, float>) return DType::Float32;
            else if constexpr (std::is_same_v<T, double>) return DType::Float64;
            else if constexpr (std::is_same_v<T, int32_t>) return DType::Int32;
            else if constexpr (std::is_same_v<T, int64_t>) return DType::Int64;
            else throw std::runtime_error("Tensor Error: Unsupported DType.");
        }

        void* data_ptr() override { return data.data(); }
        const void* data_ptr() const override { return data.data(); }
        size_t element_size() const override { return sizeof(T); }
        size_t size() const override { return data.size(); }
        std::shared_ptr<TensorStorageBase> clone() const override {
            return std::make_shared<TensorStorage<T>>(data);
        }
    };

    // ========================================================================
    // 3. 前向声明
    // ========================================================================
    class Tensor;

    struct BackwardNode {
        virtual ~BackwardNode() = default;
        virtual void apply() = 0;
        virtual std::vector<Tensor> get_dependencies() const { return {}; }
    };

    struct TensorImpl {
        std::shared_ptr<TensorStorageBase> storage;
        bool requires_grad = false;
        bool is_leaf = true;
        std::shared_ptr<Tensor> grad;
        std::shared_ptr<BackwardNode> grad_fn;
    };

    // ========================================================================
    // 4. 辅助工具
    // ========================================================================
    inline bool& grad_enabled() {
        static thread_local bool enabled = true;
        return enabled;
    }

    struct AutogradGuard {
        bool prev;
        AutogradGuard(bool enable) : prev(grad_enabled()) { grad_enabled() = enable; }
        ~AutogradGuard() { grad_enabled() = prev; }
    };

    inline std::vector<int> calcStrides(const std::vector<int>& s) {
        if (s.empty()) return {};
        std::vector<int> st(s.size(), 1);
        for (int i = static_cast<int>(s.size()) - 2; i >= 0; --i) {
            st[i] = st[i + 1] * s[i + 1];
        }
        return st;
    }

    inline size_t shapeToNumel(const std::vector<int>& shape) {
        if (shape.empty()) return 0;
        size_t n = 1;
        for (int s : shape) n *= static_cast<size_t>(s);
        return n;
    }

    // 线程安全的随机数引擎（mt19937，替代低质量 std::rand）
    inline std::mt19937& tensor_rng() {
        static thread_local std::mt19937 gen(std::random_device{}());
        return gen;
    }

    // ========================================================================
    // 5. 张量句柄 (Tensor Handle)
    // ========================================================================
    class Tensor {
    public:
        std::shared_ptr<TensorImpl> impl;
        size_t offset = 0;
        std::vector<int> shape;
        std::vector<int> strides;

        Tensor() = default;

        // 基础构造函数：创建全零张量
        Tensor(std::vector<int> s, DType type = DType::Float64, bool req_grad = false)
            : shape(std::move(s)) {
            strides = calcStrides(shape);
            size_t total_elements = numel();
            impl = std::make_shared<TensorImpl>();
            impl->requires_grad = req_grad && grad_enabled();
            switch (type) {
                case DType::Float64: impl->storage = std::make_shared<TensorStorage<double>>(total_elements); break;
                case DType::Float32: impl->storage = std::make_shared<TensorStorage<float>>(total_elements); break;
                case DType::Int32:   impl->storage = std::make_shared<TensorStorage<int32_t>>(total_elements); break;
                case DType::Int64:   impl->storage = std::make_shared<TensorStorage<int64_t>>(total_elements); break;
            }
        }

        int dim() const { return static_cast<int>(shape.size()); }

        size_t numel() const { return shapeToNumel(shape); }

        DType dtype() const { return (impl && impl->storage) ? impl->storage->dtype() : DType::Float64; }

        bool is_contiguous() const {
            auto expected = calcStrides(shape);
            return strides == expected && offset == 0;
        }

        // ====================================================================
        // 元素读写（仅支持 contiguous 的 double 快速路径；其他类型走通用路径）
        // ====================================================================
        double item() const {
            if (numel() != 1) throw std::runtime_error("Tensor Error: item() requires exactly 1 element.");
            return getFlat(0);
        }

        double getFlat(size_t linear_idx) const {
            if (is_contiguous()) return getByAbsIdx(offset + linear_idx);
            size_t idx = offset;
            size_t remain = linear_idx;
            for (int i = static_cast<int>(shape.size()) - 1; i >= 0; --i) {
                size_t coord = remain % shape[i];
                remain /= shape[i];
                idx += coord * strides[i];
            }
            return getByAbsIdx(idx);
        }

        void setFlat(size_t linear_idx, double val) {
            if (is_contiguous()) { setByAbsIdx(offset + linear_idx, val); return; }
            size_t idx = offset;
            size_t remain = linear_idx;
            for (int i = static_cast<int>(shape.size()) - 1; i >= 0; --i) {
                size_t coord = remain % shape[i];
                remain /= shape[i];
                idx += coord * strides[i];
            }
            setByAbsIdx(idx, val);
        }

        double getByAbsIdx(size_t idx) const {
            switch (impl->storage->dtype()) {
                case DType::Float64: return static_cast<const double*>(impl->storage->data_ptr())[idx];
                case DType::Float32: return static_cast<double>(static_cast<const float*>(impl->storage->data_ptr())[idx]);
                case DType::Int32:   return static_cast<double>(static_cast<const int32_t*>(impl->storage->data_ptr())[idx]);
                case DType::Int64:   return static_cast<double>(static_cast<const int64_t*>(impl->storage->data_ptr())[idx]);
            }
            return 0.0;
        }

        void setByAbsIdx(size_t idx, double val) {
            switch (impl->storage->dtype()) {
                case DType::Float64: static_cast<double*>(impl->storage->data_ptr())[idx] = val; break;
                case DType::Float32: static_cast<float*>(impl->storage->data_ptr())[idx] = static_cast<float>(val); break;
                case DType::Int32:   static_cast<int32_t*>(impl->storage->data_ptr())[idx] = static_cast<int32_t>(val); break;
                case DType::Int64:   static_cast<int64_t*>(impl->storage->data_ptr())[idx] = static_cast<int64_t>(val); break;
            }
        }

        // ====================================================================
        // 填充
        // ====================================================================
        void fill_(double val) {
            size_t n = numel();
            for (size_t i = 0; i < n; ++i) setFlat(i, val);
        }

        // ====================================================================
        // 深拷贝（解耦存储）
        // ====================================================================
        Tensor clone() const {
            Tensor t(shape, dtype(), impl->requires_grad);
            size_t n = numel();
            for (size_t i = 0; i < n; ++i) t.setFlat(i, getFlat(i));
            t.impl->is_leaf = true;
            return t;
        }

        Tensor to(DType target_dtype) const {
            Tensor t(shape, target_dtype, impl->requires_grad);
            size_t n = numel();
            for (size_t i = 0; i < n; ++i) t.setFlat(i, getFlat(i));
            t.impl->is_leaf = true;
            return t;
        }

        // 使张量连续化
        Tensor contiguous() const {
            if (is_contiguous()) return *this;
            return clone();
        }

        // ====================================================================
        // 广播机制核心 (Broadcasting Core)
        // ====================================================================
        static std::vector<int> broadcastShapes(const std::vector<int>& shapeA, const std::vector<int>& shapeB) {
            int ndimA = static_cast<int>(shapeA.size());
            int ndimB = static_cast<int>(shapeB.size());
            int ndimOut = std::max(ndimA, ndimB);
            std::vector<int> out_shape(ndimOut);

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
                    }
                    // else: orig_shape[orig_idx] == 1 → stride stays 0 (broadcast!)
                }
            }
            return new_strides;
        }

        static size_t getFlatIndex(size_t linear_idx, const std::vector<int>& out_shape, const std::vector<int>& bc_strides) {
            size_t flat_idx = 0;
            size_t remain = linear_idx;
            for (int i = static_cast<int>(out_shape.size()) - 1; i >= 0; --i) {
                size_t coord = remain % out_shape[i];
                remain /= out_shape[i];
                flat_idx += coord * bc_strides[i];
            }
            return flat_idx;
        }

        // ====================================================================
        // 零拷贝视图 (Zero-copy View)
        // ====================================================================
        Tensor make_view() const {
            Tensor t;
            t.impl = std::make_shared<TensorImpl>();
            t.impl->storage = impl->storage;
            t.impl->requires_grad = impl->requires_grad && grad_enabled();
            t.impl->is_leaf = false;
            t.offset = offset;
            t.shape = shape;
            t.strides = strides;
            return t;
        }

        Tensor view(const std::vector<int>& new_shape) const {
            if (!is_contiguous()) throw std::runtime_error("Tensor Error: view() requires a contiguous tensor.");
            size_t new_numel = shapeToNumel(new_shape);
            if (new_numel != numel()) throw std::runtime_error("Tensor Error: Shape mismatch in view().");
            Tensor t = make_view();
            t.shape = new_shape;
            t.strides = calcStrides(new_shape);
            return t;
        }

        // ====================================================================
        // 转置 (Transpose) — 零拷贝
        // ====================================================================
        Tensor transpose(int dim0, int dim1) const {
            if (dim0 < 0 || dim0 >= dim() || dim1 < 0 || dim1 >= dim())
                throw std::runtime_error("Tensor Error: transpose dimension out of range.");
            Tensor t = make_view();
            std::swap(t.shape[dim0], t.shape[dim1]);
            std::swap(t.strides[dim0], t.strides[dim1]);
            return t;
        }

        Tensor T() const {
            if (dim() < 2) throw std::runtime_error("Tensor Error: T() requires at least 2 dimensions.");
            return transpose(dim() - 2, dim() - 1);
        }

        static constexpr int SLICE_NONE = -2147483647 - 1;

        // ====================================================================
        // 切片 — 返回子视图
        // ====================================================================
        Tensor slice_dim(int dimension, int start, int end, int step) const {
            if (dimension < 0 || dimension >= dim())
                throw std::runtime_error("Tensor Error: slice dimension out of range.");
            if (step == 0) throw std::runtime_error("Tensor Error: slice step cannot be zero.");

            int dim_size = shape[dimension];
            int st = start == SLICE_NONE ? (step > 0 ? 0 : dim_size - 1) : start;
            int en = end == SLICE_NONE ? (step > 0 ? dim_size : -1) : end;

            if (st < 0 && start != SLICE_NONE) st += dim_size;
            if (en < 0 && end != SLICE_NONE) en += dim_size;

            if (step > 0) {
                st = std::max(0, std::min(dim_size, st));
                en = std::max(0, std::min(dim_size, en));
            } else {
                st = std::max(-1, std::min(dim_size - 1, st));
                en = std::max(-1, std::min(dim_size - 1, en));
            }

            int count = 0;
            if (step > 0) {
                if (en > st) count = (en - st + step - 1) / step;
            } else {
                if (en < st) count = (st - en - step - 1) / (-step);
            }

            Tensor t = make_view();
            t.offset = offset + st * strides[dimension];
            t.shape[dimension] = count;
            t.strides[dimension] *= step;
            return t;
        }

        Tensor select(int dimension, int index) const {
            if (dimension < 0 || dimension >= dim())
                throw std::runtime_error("Tensor Error: select dimension out of range.");
            if (index < 0) index += shape[dimension];
            if (index < 0 || index >= shape[dimension])
                throw std::runtime_error("Tensor Error: select index out of range.");
            Tensor t = make_view();
            t.offset = offset + index * strides[dimension];
            t.shape.clear();
            t.strides.clear();
            for (int i = 0; i < dim(); ++i) {
                if (i != dimension) {
                    t.shape.push_back(shape[i]);
                    t.strides.push_back(strides[i]);
                }
            }
            return t;
        }

        // ====================================================================
        // Squeeze / Unsqueeze
        // ====================================================================
        Tensor unsqueeze(int dimension) const {
            if (dimension < 0) dimension += dim() + 1;
            if (dimension < 0 || dimension > dim())
                throw std::runtime_error("Tensor Error: unsqueeze dimension out of range.");
            Tensor t = make_view();
            t.shape.insert(t.shape.begin() + dimension, 1);
            int stride_val = (dimension < static_cast<int>(strides.size())) ? strides[dimension] : 1;
            t.strides.insert(t.strides.begin() + dimension, stride_val);
            return t;
        }

        Tensor squeeze(int dimension = -1) const {
            Tensor t = make_view();
            t.shape.clear();
            t.strides.clear();
            if (dimension >= 0) {
                if (dimension >= dim()) throw std::runtime_error("Tensor Error: squeeze dimension out of range.");
                if (shape[dimension] == 1) {
                    for (int i = 0; i < dim(); ++i) {
                        if (i != dimension) {
                            t.shape.push_back(shape[i]);
                            t.strides.push_back(strides[i]);
                        }
                    }
                } else {
                    t.shape = shape;
                    t.strides = strides;
                }
            } else {
                for (int i = 0; i < dim(); ++i) {
                    if (shape[i] != 1) {
                        t.shape.push_back(shape[i]);
                        t.strides.push_back(strides[i]);
                    }
                }
            }
            if (t.shape.empty()) { t.shape = {1}; t.strides = {1}; }
            return t;
        }

        // ====================================================================
        // 触发反向传播
        // ====================================================================
        void backward() {
            if (!impl->requires_grad) throw std::runtime_error("Tensor Error: Tensor does not require grad.");
            if (!impl->grad) {
                impl->grad = std::make_shared<Tensor>(shape, dtype(), false);
                impl->grad->fill_(1.0);
            }
            
            // 拓扑排序 (DFS)
            std::vector<std::shared_ptr<BackwardNode>> topo;
            std::unordered_set<BackwardNode*> visited;
            std::function<void(const std::shared_ptr<BackwardNode>&)> build_topo = [&](const std::shared_ptr<BackwardNode>& fn) {
                if (!fn) return;
                if (visited.count(fn.get())) return;
                visited.insert(fn.get());
                for (const auto& dep : fn->get_dependencies()) {
                    if (dep.impl && dep.impl->grad_fn) {
                        build_topo(dep.impl->grad_fn);
                    }
                }
                topo.push_back(fn);
            };
            
            build_topo(impl->grad_fn);
            
            // 反向遍历拓扑序
            AutogradGuard guard(false);
            for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
                (*it)->apply();
            }
        }

        // ====================================================================
        // 漂亮打印
        // ====================================================================
        std::string toString() const {
            std::ostringstream oss;
            oss << "Tensor(";
            printRecursive(oss, 0, 0, 0);
            oss << ", shape=[";
            for (size_t i = 0; i < shape.size(); ++i) {
                oss << shape[i];
                if (i + 1 < shape.size()) oss << ", ";
            }
            oss << "], dtype=" << dtypeToString(dtype());
            if (impl && impl->requires_grad) oss << ", requires_grad=true";
            oss << ")";
            return oss.str();
        }

    private:
        void printRecursive(std::ostringstream& oss, int cur_dim, size_t base_offset, int indent) const {
            if (cur_dim == dim()) {
                // 标量位置
                oss << getByAbsIdx(base_offset);
                return;
            }
            oss << "[";
            for (int i = 0; i < shape[cur_dim]; ++i) {
                if (i > 0) {
                    oss << ", ";
                    if (cur_dim < dim() - 1) {
                        oss << "\n";
                        for (int j = 0; j < indent + cur_dim + 1; ++j) oss << " ";
                    }
                }
                printRecursive(oss, cur_dim + 1, base_offset + i * strides[cur_dim], indent);
            }
            oss << "]";
        }
    };

    // ========================================================================
    // 6. Autograd 节点 (Backward Nodes)
    // ========================================================================

    class WeakTensor {
    public:
        std::weak_ptr<TensorImpl> impl;
        size_t offset = 0;
        std::vector<int> shape;
        std::vector<int> strides;

        WeakTensor() = default;
        WeakTensor(const Tensor& t) : impl(t.impl), offset(t.offset), shape(t.shape), strides(t.strides) {}

        Tensor lock() const {
            Tensor t;
            t.impl = impl.lock();
            t.offset = offset;
            t.shape = shape;
            t.strides = strides;
            return t;
        }
    };

    // ---- 辅助：将梯度从 broadcast 形状 reduce 回原始形状 ----
    inline Tensor reduce_grad_for_broadcast(const Tensor& grad_out, const std::vector<int>& orig_shape) {
        // grad_out 的 shape 可能比 orig_shape 维度多（因广播扩展），需要 sum 掉多出的维度
        if (grad_out.shape == orig_shape) return grad_out;

        int ndimOut = static_cast<int>(grad_out.shape.size());
        int ndimOrig = static_cast<int>(orig_shape.size());

        // 对齐到右侧
        std::vector<int> axes_to_sum;
        for (int i = 0; i < ndimOut; ++i) {
            int orig_idx = i - (ndimOut - ndimOrig);
            if (orig_idx < 0) {
                axes_to_sum.push_back(i); // 多出的前导维度
            } else if (orig_shape[orig_idx] == 1 && grad_out.shape[i] != 1) {
                axes_to_sum.push_back(i); // 被广播的维度
            }
        }

        // 逐轴 reduce-sum（从高轴到低轴以保持索引正确）
        Tensor result = grad_out;
        for (int ax_i = static_cast<int>(axes_to_sum.size()) - 1; ax_i >= 0; --ax_i) {
            int axis = axes_to_sum[ax_i];
            // sum along 'axis'
            std::vector<int> new_shape;
            for (int d = 0; d < result.dim(); ++d) {
                if (d != axis) new_shape.push_back(result.shape[d]);
            }
            if (new_shape.empty()) new_shape = {1};
            Tensor summed(new_shape, result.dtype(), false);
            
            // 优化：使用 N 维计数器避免内层循环的坐标换算和内存分配
            int axis_size = result.shape[axis];
            int inner_stride = result.strides[axis];
            size_t out_n = summed.numel();
            std::vector<size_t> coord(summed.dim(), 0);
            size_t base_idx = result.offset;

            for (size_t oi = 0; oi < out_n; ++oi) {
                double acc = 0.0;
                size_t cur_idx = base_idx;
                for (int k = 0; k < axis_size; ++k) {
                    acc += result.getByAbsIdx(cur_idx);
                    cur_idx += inner_stride;
                }
                summed.setFlat(oi, acc);

                // 推进 N 维计数器
                for (int d = summed.dim() - 1; d >= 0; --d) {
                    coord[d]++;
                    if (coord[d] < static_cast<size_t>(summed.shape[d])) {
                        int orig_d = (d >= axis) ? d + 1 : d;
                        base_idx += result.strides[orig_d];
                        break;
                    }
                    coord[d] = 0;
                    int orig_d = (d >= axis) ? d + 1 : d;
                    base_idx -= result.strides[orig_d] * (summed.shape[d] - 1);
                }
            }
            result = summed;
        }

        // reshape back to orig_shape if needed
        if (result.shape != orig_shape) {
            result = result.view(orig_shape);
        }
        return result;
    }

    // ---- 辅助：张量逐元素累加 ----
    inline void tensor_accumulate_grad(Tensor& target, const Tensor& source) {
        size_t n = target.numel();
        if (target.is_contiguous() && source.is_contiguous()) {
            size_t idxT = target.offset;
            size_t idxS = source.offset;
            for (size_t i = 0; i < n; ++i) {
                target.setByAbsIdx(idxT + i, target.getByAbsIdx(idxT + i) + source.getByAbsIdx(idxS + i));
            }
            return;
        }
        // 优化：非连续张量使用 N 维计数器，避免昂贵的 getFlat 取模运算
        int ndim = target.dim();
        std::vector<size_t> coord(ndim, 0);
        size_t idxT = target.offset;
        size_t idxS = source.offset;
        for (size_t i = 0; i < n; ++i) {
            target.setByAbsIdx(idxT, target.getByAbsIdx(idxT) + source.getByAbsIdx(idxS));
            for (int d = ndim - 1; d >= 0; --d) {
                coord[d]++;
                if (coord[d] < static_cast<size_t>(target.shape[d])) {
                    idxT += target.strides[d];
                    idxS += source.strides[d];
                    break;
                }
                coord[d] = 0;
                idxT -= target.strides[d] * (target.shape[d] - 1);
                idxS -= source.strides[d] * (target.shape[d] - 1);
            }
        }
    }

    // ---- AddBackward ----
    struct AddBackward : public BackwardNode {
        Tensor a, b;
        WeakTensor out;
        AddBackward(Tensor a, Tensor b, Tensor out)
            : a(std::move(a)), b(std::move(b)), out(std::move(out)) {}
        std::vector<Tensor> get_dependencies() const override { return {a, b}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad) return;
            if (a.impl->requires_grad) {
                Tensor g = reduce_grad_for_broadcast(*out_locked.impl->grad, a.shape);
                if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
                tensor_accumulate_grad(*a.impl->grad, g);
            }
            if (b.impl->requires_grad) {
                Tensor g = reduce_grad_for_broadcast(*out_locked.impl->grad, b.shape);
                if (!b.impl->grad) b.impl->grad = std::make_shared<Tensor>(b.shape, b.dtype(), false);
                tensor_accumulate_grad(*b.impl->grad, g);
            }
        }
    };

    // ---- SubBackward ----
    struct SubBackward : public BackwardNode {
        Tensor a, b;
        WeakTensor out;
        SubBackward(Tensor a, Tensor b, Tensor out)
            : a(std::move(a)), b(std::move(b)), out(std::move(out)) {}
        std::vector<Tensor> get_dependencies() const override { return {a, b}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad) return;
            if (a.impl->requires_grad) {
                Tensor g = reduce_grad_for_broadcast(*out_locked.impl->grad, a.shape);
                if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
                tensor_accumulate_grad(*a.impl->grad, g);
            }
            if (b.impl->requires_grad) {
                Tensor g = reduce_grad_for_broadcast(*out_locked.impl->grad, b.shape);
                // negate
                Tensor neg_g(g.shape, g.dtype(), false);
                for (size_t i = 0; i < neg_g.numel(); ++i) neg_g.setFlat(i, -g.getFlat(i));
                if (!b.impl->grad) b.impl->grad = std::make_shared<Tensor>(b.shape, b.dtype(), false);
                tensor_accumulate_grad(*b.impl->grad, neg_g);
            }
        }
    };

    // ---- MulBackward (element-wise) ----
    struct MulBackward : public BackwardNode {
        Tensor a, b;
        WeakTensor out;
        MulBackward(Tensor a, Tensor b, Tensor out)
            : a(std::move(a)), b(std::move(b)), out(std::move(out)) {}
        std::vector<Tensor> get_dependencies() const override { return {a, b}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad) return;
            if (a.impl->requires_grad) {
                // da = grad_out * b
                Tensor da(out_locked.impl->grad->shape, out_locked.impl->grad->dtype(), false);
                auto stridesB = Tensor::broadcastStrides(b.shape, b.strides, out_locked.impl->grad->shape);
                for (size_t i = 0; i < da.numel(); ++i) {
                    size_t idxB = Tensor::getFlatIndex(i, out_locked.impl->grad->shape, stridesB);
                    da.setFlat(i, out_locked.impl->grad->getFlat(i) * b.getByAbsIdx(b.offset + idxB));
                }
                Tensor g = reduce_grad_for_broadcast(da, a.shape);
                if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
                tensor_accumulate_grad(*a.impl->grad, g);
            }
            if (b.impl->requires_grad) {
                // db = grad_out * a
                Tensor db(out_locked.impl->grad->shape, out_locked.impl->grad->dtype(), false);
                auto stridesA = Tensor::broadcastStrides(a.shape, a.strides, out_locked.impl->grad->shape);
                for (size_t i = 0; i < db.numel(); ++i) {
                    size_t idxA = Tensor::getFlatIndex(i, out_locked.impl->grad->shape, stridesA);
                    db.setFlat(i, out_locked.impl->grad->getFlat(i) * a.getByAbsIdx(a.offset + idxA));
                }
                Tensor g = reduce_grad_for_broadcast(db, b.shape);
                if (!b.impl->grad) b.impl->grad = std::make_shared<Tensor>(b.shape, b.dtype(), false);
                tensor_accumulate_grad(*b.impl->grad, g);
            }
        }
    };

    // ---- DivBackward (element-wise a/b) ----
    struct DivBackward : public BackwardNode {
        Tensor a, b;
        WeakTensor out;
        DivBackward(Tensor a, Tensor b, Tensor out)
            : a(std::move(a)), b(std::move(b)), out(std::move(out)) {}
        std::vector<Tensor> get_dependencies() const override { return {a, b}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad) return;
            if (a.impl->requires_grad) {
                // da = grad / b
                Tensor da(out_locked.impl->grad->shape, out_locked.impl->grad->dtype(), false);
                auto stridesB = Tensor::broadcastStrides(b.shape, b.strides, out_locked.impl->grad->shape);
                for (size_t i = 0; i < da.numel(); ++i) {
                    size_t idxB = Tensor::getFlatIndex(i, out_locked.impl->grad->shape, stridesB);
                    da.setFlat(i, out_locked.impl->grad->getFlat(i) / b.getByAbsIdx(b.offset + idxB));
                }
                Tensor g = reduce_grad_for_broadcast(da, a.shape);
                if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
                tensor_accumulate_grad(*a.impl->grad, g);
            }
            if (b.impl->requires_grad) {
                // db = -grad * a / b^2
                Tensor db(out_locked.impl->grad->shape, out_locked.impl->grad->dtype(), false);
                auto stridesA = Tensor::broadcastStrides(a.shape, a.strides, out_locked.impl->grad->shape);
                auto stridesB = Tensor::broadcastStrides(b.shape, b.strides, out_locked.impl->grad->shape);
                for (size_t i = 0; i < db.numel(); ++i) {
                    size_t idxA = Tensor::getFlatIndex(i, out_locked.impl->grad->shape, stridesA);
                    size_t idxB = Tensor::getFlatIndex(i, out_locked.impl->grad->shape, stridesB);
                    double bv = b.getByAbsIdx(b.offset + idxB);
                    double av = a.getByAbsIdx(a.offset + idxA);
                    db.setFlat(i, -out_locked.impl->grad->getFlat(i) * av / (bv * bv));
                }
                Tensor g = reduce_grad_for_broadcast(db, b.shape);
                if (!b.impl->grad) b.impl->grad = std::make_shared<Tensor>(b.shape, b.dtype(), false);
                tensor_accumulate_grad(*b.impl->grad, g);
            }
        }
    };

    // ---- NegBackward ----
    struct NegBackward : public BackwardNode {
        Tensor a;
        WeakTensor out;
        NegBackward(Tensor a, Tensor out) : a(std::move(a)), out(std::move(out)) {}
        std::vector<Tensor> get_dependencies() const override { return {a}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad || !a.impl->requires_grad) return;
            Tensor neg_g(out_locked.impl->grad->shape, out_locked.impl->grad->dtype(), false);
            for (size_t i = 0; i < neg_g.numel(); ++i) neg_g.setFlat(i, -out_locked.impl->grad->getFlat(i));
            if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
            tensor_accumulate_grad(*a.impl->grad, neg_g);
        }
    };

    // ---- PowScalarBackward (element-wise a^exponent, exponent is scalar) ----
    struct PowScalarBackward : public BackwardNode {
        Tensor a;
        WeakTensor out;
        double exponent;
        PowScalarBackward(Tensor a, double exp, Tensor out)
            : a(std::move(a)), out(std::move(out)), exponent(exp) {}
        std::vector<Tensor> get_dependencies() const override { return {a}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad || !a.impl->requires_grad) return;
            Tensor da(a.shape, a.dtype(), false);
            for (size_t i = 0; i < da.numel(); ++i) {
                da.setFlat(i, out_locked.impl->grad->getFlat(i) * exponent * std::pow(a.getFlat(i), exponent - 1.0));
            }
            if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
            tensor_accumulate_grad(*a.impl->grad, da);
        }
    };

    // ---- ExpBackward ----
    struct ExpBackward : public BackwardNode {
        Tensor a;
        WeakTensor out;
        ExpBackward(Tensor a, Tensor out) : a(std::move(a)), out(std::move(out)) {}
        std::vector<Tensor> get_dependencies() const override { return {a}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad || !a.impl->requires_grad) return;
            Tensor da(a.shape, a.dtype(), false);
            for (size_t i = 0; i < da.numel(); ++i) {
                da.setFlat(i, out_locked.impl->grad->getFlat(i) * out_locked.getFlat(i)); // d(exp(x))/dx = exp(x)
            }
            if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
            tensor_accumulate_grad(*a.impl->grad, da);
        }
    };

    // ---- LogBackward ----
    struct LogBackward : public BackwardNode {
        Tensor a;
        WeakTensor out;
        LogBackward(Tensor a, Tensor out) : a(std::move(a)), out(std::move(out)) {}
        std::vector<Tensor> get_dependencies() const override { return {a}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad || !a.impl->requires_grad) return;
            Tensor da(a.shape, a.dtype(), false);
            for (size_t i = 0; i < da.numel(); ++i) {
                da.setFlat(i, out_locked.impl->grad->getFlat(i) / a.getFlat(i));
            }
            if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
            tensor_accumulate_grad(*a.impl->grad, da);
        }
    };

    // ---- SumBackward ----
    struct SumBackward : public BackwardNode {
        Tensor a;
        WeakTensor out;
        SumBackward(Tensor a, Tensor out) : a(std::move(a)), out(std::move(out)) {}
        std::vector<Tensor> get_dependencies() const override { return {a}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad || !a.impl->requires_grad) return;
            double g = out_locked.impl->grad->getFlat(0); // scalar grad
            Tensor da(a.shape, a.dtype(), false);
            da.fill_(g);
            if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
            tensor_accumulate_grad(*a.impl->grad, da);
        }
    };

    // ---- MeanBackward ----
    struct MeanBackward : public BackwardNode {
        Tensor a;
        WeakTensor out;
        MeanBackward(Tensor a, Tensor out) : a(std::move(a)), out(std::move(out)) {}
        std::vector<Tensor> get_dependencies() const override { return {a}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad || !a.impl->requires_grad) return;
            double g = out_locked.impl->grad->getFlat(0) / static_cast<double>(a.numel());
            Tensor da(a.shape, a.dtype(), false);
            da.fill_(g);
            if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
            tensor_accumulate_grad(*a.impl->grad, da);
        }
    };

    // ---- SumAxisBackward（沿 axis 规约，梯度广播回 axis 维）----
    struct SumAxisBackward : public BackwardNode {
        Tensor a;
        WeakTensor out;
        int axis;
        SumAxisBackward(Tensor a, Tensor out, int axis)
            : a(std::move(a)), out(std::move(out)), axis(axis) {}
        std::vector<Tensor> get_dependencies() const override { return {a}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad || !a.impl->requires_grad) return;
            Tensor da(a.shape, a.dtype(), false);
            int axis_size = a.shape[axis];
            size_t outer = 1, inner = 1;
            for (int d = 0; d < axis; ++d) outer *= a.shape[d];
            for (int d = axis + 1; d < a.dim(); ++d) inner *= a.shape[d];
            for (size_t o = 0; o < outer; ++o) {
                for (size_t in = 0; in < inner; ++in) {
                    double g = out_locked.impl->grad->getFlat(o * inner + in);
                    for (int k = 0; k < axis_size; ++k) da.setFlat((o * axis_size + k) * inner + in, g);
                }
            }
            if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
            tensor_accumulate_grad(*a.impl->grad, da);
        }
    };

    // ---- MeanAxisBackward ----
    struct MeanAxisBackward : public BackwardNode {
        Tensor a;
        WeakTensor out;
        int axis;
        MeanAxisBackward(Tensor a, Tensor out, int axis)
            : a(std::move(a)), out(std::move(out)), axis(axis) {}
        std::vector<Tensor> get_dependencies() const override { return {a}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad || !a.impl->requires_grad) return;
            Tensor da(a.shape, a.dtype(), false);
            int axis_size = a.shape[axis];
            size_t outer = 1, inner = 1;
            for (int d = 0; d < axis; ++d) outer *= a.shape[d];
            for (int d = axis + 1; d < a.dim(); ++d) inner *= a.shape[d];
            for (size_t o = 0; o < outer; ++o) {
                for (size_t in = 0; in < inner; ++in) {
                    double g = out_locked.impl->grad->getFlat(o * inner + in) / static_cast<double>(axis_size);
                    for (int k = 0; k < axis_size; ++k) da.setFlat((o * axis_size + k) * inner + in, g);
                }
            }
            if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
            tensor_accumulate_grad(*a.impl->grad, da);
        }
    };

    // ---- ClampBackward（梯度仅在 [min, max] 区间内传播）----
    struct ClampBackward : public BackwardNode {
        Tensor a;
        WeakTensor out;
        double min_val, max_val;
        ClampBackward(Tensor a, Tensor out, double min_val, double max_val)
            : a(std::move(a)), out(std::move(out)), min_val(min_val), max_val(max_val) {}
        std::vector<Tensor> get_dependencies() const override { return {a}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad || !a.impl->requires_grad) return;
            Tensor da(a.shape, a.dtype(), false);
            for (size_t i = 0; i < da.numel(); ++i) {
                double x = a.getFlat(i);
                da.setFlat(i, (x >= min_val && x <= max_val) ? out_locked.impl->grad->getFlat(i) : 0.0);
            }
            if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
            tensor_accumulate_grad(*a.impl->grad, da);
        }
    };

    // ---- MatMulBackward ----
    struct MatMulBackward : public BackwardNode {
        Tensor a, b;
        WeakTensor out;
        MatMulBackward(Tensor a, Tensor b, Tensor out)
            : a(std::move(a)), b(std::move(b)), out(std::move(out)) {}
        std::vector<Tensor> get_dependencies() const override { return {a, b}; }
        void apply() override;  // 定义在 matmul 之后
    };

    // ---- ReluBackward ----
    struct ReluBackward : public BackwardNode {
        Tensor a;
        WeakTensor out;
        ReluBackward(Tensor a, Tensor out) : a(std::move(a)), out(std::move(out)) {}
        std::vector<Tensor> get_dependencies() const override { return {a}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad || !a.impl->requires_grad) return;
            Tensor da(a.shape, a.dtype(), false);
            for (size_t i = 0; i < da.numel(); ++i) {
                da.setFlat(i, a.getFlat(i) > 0.0 ? out_locked.impl->grad->getFlat(i) : 0.0);
            }
            if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
            tensor_accumulate_grad(*a.impl->grad, da);
        }
    };

    // ---- SigmoidBackward ----
    struct SigmoidBackward : public BackwardNode {
        Tensor a;
        WeakTensor out;
        SigmoidBackward(Tensor a, Tensor out) : a(std::move(a)), out(std::move(out)) {}
        std::vector<Tensor> get_dependencies() const override { return {a}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad || !a.impl->requires_grad) return;
            Tensor da(a.shape, a.dtype(), false);
            for (size_t i = 0; i < da.numel(); ++i) {
                double s = out_locked.getFlat(i); // sigmoid value
                da.setFlat(i, out_locked.impl->grad->getFlat(i) * s * (1.0 - s));
            }
            if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
            tensor_accumulate_grad(*a.impl->grad, da);
        }
    };

    // ---- TanhBackward ----
    struct TanhBackward : public BackwardNode {
        Tensor a;
        WeakTensor out;
        TanhBackward(Tensor a, Tensor out) : a(std::move(a)), out(std::move(out)) {}
        std::vector<Tensor> get_dependencies() const override { return {a}; }
        void apply() override {
            Tensor out_locked = out.lock();
            if (!out_locked.impl || !out_locked.impl->grad || !a.impl->requires_grad) return;
            Tensor da(a.shape, a.dtype(), false);
            for (size_t i = 0; i < da.numel(); ++i) {
                double t = out_locked.getFlat(i);
                da.setFlat(i, out_locked.impl->grad->getFlat(i) * (1.0 - t * t));
            }
            if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
            tensor_accumulate_grad(*a.impl->grad, da);
        }
    };

    // ========================================================================
    // 7. 前向算子 (Forward Operators)
    // ========================================================================

    // ---- 模板化逐元素运算核心（dtype 分派一次，内层 raw pointer 快速路径） ----

    // 二元逐元素：模板化 + dtype 分派
    template <typename T, typename Op>
    inline void apply_binary_impl(const Tensor& a, const Tensor& b, Tensor& out, Op op) {
        const T* pa = static_cast<const T*>(a.impl->storage->data_ptr()) + a.offset;
        const T* pb = static_cast<const T*>(b.impl->storage->data_ptr()) + b.offset;
        T* po = static_cast<T*>(out.impl->storage->data_ptr());
        size_t total = out.numel();

        if (a.shape == b.shape && a.is_contiguous() && b.is_contiguous()) {
            for (size_t i = 0; i < total; ++i) po[i] = static_cast<T>(op(pa[i], pb[i]));
            return;
        }
        // 广播/非连续：预计算 broadcast strides + 多维计数器指针累加（每元素 O(1)）
        auto stridesA = Tensor::broadcastStrides(a.shape, a.strides, out.shape);
        auto stridesB = Tensor::broadcastStrides(b.shape, b.strides, out.shape);
        int ndim = out.dim();
        std::vector<size_t> coord(ndim, 0);
        size_t idxA = 0, idxB = 0;
        for (size_t i = 0; i < total; ++i) {
            po[i] = static_cast<T>(op(pa[idxA], pb[idxB]));
            for (int d = ndim - 1; d >= 0; --d) {
                coord[d]++;
                if (coord[d] < static_cast<size_t>(out.shape[d])) {
                    idxA += stridesA[d];
                    idxB += stridesB[d];
                    break;
                }
                coord[d] = 0;
                idxA -= stridesA[d] * (out.shape[d] - 1);
                idxB -= stridesB[d] * (out.shape[d] - 1);
            }
        }
    }

    template <typename Op>
    inline void dispatch_binary(const Tensor& a, const Tensor& b, Tensor& out, Op op) {
        switch (a.dtype()) {
            case DType::Float64: apply_binary_impl<double>(a, b, out, op); break;
            case DType::Float32: apply_binary_impl<float>(a, b, out, op); break;
            case DType::Int32:   apply_binary_impl<int32_t>(a, b, out, op); break;
            case DType::Int64:   apply_binary_impl<int64_t>(a, b, out, op); break;
        }
    }

    // 一元逐元素：模板化 + dtype 分派
    template <typename T, typename Op>
    inline void apply_unary_impl(const Tensor& a, Tensor& out, Op op) {
        const T* pa = static_cast<const T*>(a.impl->storage->data_ptr()) + a.offset;
        T* po = static_cast<T*>(out.impl->storage->data_ptr());
        size_t total = out.numel();
        if (a.is_contiguous()) {
            for (size_t i = 0; i < total; ++i) po[i] = static_cast<T>(op(pa[i]));
        } else {
            int ndim = out.dim();
            std::vector<size_t> coord(ndim, 0);
            size_t idxA = 0;
            for (size_t i = 0; i < total; ++i) {
                po[i] = static_cast<T>(op(pa[idxA]));
                for (int d = ndim - 1; d >= 0; --d) {
                    coord[d]++;
                    if (coord[d] < static_cast<size_t>(out.shape[d])) {
                        idxA += a.strides[d];
                        break;
                    }
                    coord[d] = 0;
                    idxA -= a.strides[d] * (out.shape[d] - 1);
                }
            }
        }
    }

    template <typename Op>
    inline void dispatch_unary(const Tensor& a, Tensor& out, Op op) {
        switch (a.dtype()) {
            case DType::Float64: apply_unary_impl<double>(a, out, op); break;
            case DType::Float32: apply_unary_impl<float>(a, out, op); break;
            case DType::Int32:   apply_unary_impl<int32_t>(a, out, op); break;
            case DType::Int64:   apply_unary_impl<int64_t>(a, out, op); break;
        }
    }

    // ---- Strassen 辅助函数 ----
    template <typename T>
    inline void tensor_add_view(const T* A, int strideA, const T* B, int strideB, T* C, int strideC, int m, int n) {
        for (int i = 0; i < m; ++i)
            for (int j = 0; j < n; ++j)
                C[i * strideC + j] = A[i * strideA + j] + B[i * strideB + j];
    }

    template <typename T>
    inline void tensor_sub_view(const T* A, int strideA, const T* B, int strideB, T* C, int strideC, int m, int n) {
        for (int i = 0; i < m; ++i)
            for (int j = 0; j < n; ++j)
                C[i * strideC + j] = A[i * strideA + j] - B[i * strideB + j];
    }

    template <typename T>
    inline void tensor_mul_base_view(const T* A, int strideA, const T* B, int strideB, T* C, int strideC, int m, int k_dim, int n) {
        for (int i = 0; i < m; ++i) {
            for (int j = 0; j < n; ++j) C[i * strideC + j] = T(0);
        }
        for (int i = 0; i < m; ++i) {
            for (int k = 0; k < k_dim; ++k) {
                T r = A[i * strideA + k];
                if (r == T(0)) continue;
                for (int j = 0; j < n; ++j) {
                    C[i * strideC + j] += r * B[k * strideB + j];
                }
            }
        }
    }

    template <typename T>
    void tensor_strassen_view(const T* A, int strideA, const T* B, int strideB, T* C, int strideC, int n, int depth) {
        if (n <= 64) {
            tensor_mul_base_view(A, strideA, B, strideB, C, strideC, n, n, n);
            return;
        }

        int odd = n % 2;
        int even_n = n - odd;
        int half = even_n / 2;

        std::vector<T> workspace(9 * half * half);
        T* M1 = workspace.data();
        T* M2 = M1 + half * half;
        T* M3 = M2 + half * half;
        T* M4 = M3 + half * half;
        T* M5 = M4 + half * half;
        T* M6 = M5 + half * half;
        T* M7 = M6 + half * half;
        T* T1 = M7 + half * half;
        T* T2 = T1 + half * half;

        const T* A11 = A;
        const T* A12 = A + half;
        const T* A21 = A + half * strideA;
        const T* A22 = A + half * strideA + half;

        const T* B11 = B;
        const T* B12 = B + half;
        const T* B21 = B + half * strideB;
        const T* B22 = B + half * strideB + half;

        T* C11 = C;
        T* C12 = C + half;
        T* C21 = C + half * strideC;
        T* C22 = C + half * strideC + half;

        if (depth < 2) {
            auto f1 = std::async(std::launch::async, [&]() {
                std::vector<T> localT(2 * half * half);
                T* lT1 = localT.data(); T* lT2 = lT1 + half * half;
                tensor_add_view(A11, strideA, A22, strideA, lT1, half, half, half);
                tensor_add_view(B11, strideB, B22, strideB, lT2, half, half, half);
                tensor_strassen_view(lT1, half, lT2, half, M1, half, half, depth + 1);
            });
            auto f2 = std::async(std::launch::async, [&]() {
                std::vector<T> localT(half * half);
                T* lT1 = localT.data();
                tensor_add_view(A21, strideA, A22, strideA, lT1, half, half, half);
                tensor_strassen_view(lT1, half, B11, strideB, M2, half, half, depth + 1);
            });
            auto f3 = std::async(std::launch::async, [&]() {
                std::vector<T> localT(half * half);
                T* lT2 = localT.data();
                tensor_sub_view(B12, strideB, B22, strideB, lT2, half, half, half);
                tensor_strassen_view(A11, strideA, lT2, half, M3, half, half, depth + 1);
            });
            auto f4 = std::async(std::launch::async, [&]() {
                std::vector<T> localT(half * half);
                T* lT2 = localT.data();
                tensor_sub_view(B21, strideB, B11, strideB, lT2, half, half, half);
                tensor_strassen_view(A22, strideA, lT2, half, M4, half, half, depth + 1);
            });
            auto f5 = std::async(std::launch::async, [&]() {
                std::vector<T> localT(half * half);
                T* lT1 = localT.data();
                tensor_add_view(A11, strideA, A12, strideA, lT1, half, half, half);
                tensor_strassen_view(lT1, half, B22, strideB, M5, half, half, depth + 1);
            });
            auto f6 = std::async(std::launch::async, [&]() {
                std::vector<T> localT(2 * half * half);
                T* lT1 = localT.data(); T* lT2 = lT1 + half * half;
                tensor_sub_view(A21, strideA, A11, strideA, lT1, half, half, half);
                tensor_add_view(B11, strideB, B12, strideB, lT2, half, half, half);
                tensor_strassen_view(lT1, half, lT2, half, M6, half, half, depth + 1);
            });
            
            std::vector<T> localT(2 * half * half);
            T* lT1 = localT.data(); T* lT2 = lT1 + half * half;
            tensor_sub_view(A12, strideA, A22, strideA, lT1, half, half, half);
            tensor_add_view(B21, strideB, B22, strideB, lT2, half, half, half);
            tensor_strassen_view(lT1, half, lT2, half, M7, half, half, depth + 1);

            f1.get(); f2.get(); f3.get(); f4.get(); f5.get(); f6.get();
        } else {
            tensor_add_view(A11, strideA, A22, strideA, T1, half, half, half);
            tensor_add_view(B11, strideB, B22, strideB, T2, half, half, half);
            tensor_strassen_view(T1, half, T2, half, M1, half, half, depth + 1);

            tensor_add_view(A21, strideA, A22, strideA, T1, half, half, half);
            tensor_strassen_view(T1, half, B11, strideB, M2, half, half, depth + 1);

            tensor_sub_view(B12, strideB, B22, strideB, T2, half, half, half);
            tensor_strassen_view(A11, strideA, T2, half, M3, half, half, depth + 1);

            tensor_sub_view(B21, strideB, B11, strideB, T2, half, half, half);
            tensor_strassen_view(A22, strideA, T2, half, M4, half, half, depth + 1);

            tensor_add_view(A11, strideA, A12, strideA, T1, half, half, half);
            tensor_strassen_view(T1, half, B22, strideB, M5, half, half, depth + 1);

            tensor_sub_view(A21, strideA, A11, strideA, T1, half, half, half);
            tensor_add_view(B11, strideB, B12, strideB, T2, half, half, half);
            tensor_strassen_view(T1, half, T2, half, M6, half, half, depth + 1);

            tensor_sub_view(A12, strideA, A22, strideA, T1, half, half, half);
            tensor_add_view(B21, strideB, B22, strideB, T2, half, half, half);
            tensor_strassen_view(T1, half, T2, half, M7, half, half, depth + 1);
        }

        for (int i = 0; i < half; ++i) {
            for (int j = 0; j < half; ++j) {
                C11[i * strideC + j] = M1[i * half + j] + M4[i * half + j] - M5[i * half + j] + M7[i * half + j];
                C12[i * strideC + j] = M3[i * half + j] + M5[i * half + j];
                C21[i * strideC + j] = M2[i * half + j] + M4[i * half + j];
                C22[i * strideC + j] = M1[i * half + j] - M2[i * half + j] + M3[i * half + j] + M6[i * half + j];
            }
        }

        if (odd) {
            for (int i = 0; i < even_n; ++i) {
                T sum = T(0);
                for (int k = 0; k < n; ++k) sum += A[i * strideA + k] * B[k * strideB + even_n];
                C[i * strideC + even_n] = sum;
            }
            for (int j = 0; j < even_n; ++j) {
                T sum = T(0);
                for (int k = 0; k < n; ++k) sum += A[even_n * strideA + k] * B[k * strideB + j];
                C[even_n * strideC + j] = sum;
            }
            T sum = T(0);
            for (int k = 0; k < n; ++k) sum += A[even_n * strideA + k] * B[k * strideB + even_n];
            C[even_n * strideC + even_n] = sum;

            for (int i = 0; i < even_n; ++i) {
                T a_edge = A[i * strideA + even_n];
                if (a_edge != T(0)) {
                    for (int j = 0; j < even_n; ++j) {
                        C[i * strideC + j] += a_edge * B[even_n * strideB + j];
                    }
                }
            }
        }
    }

    // matmul：智能分派 (Strassen vs Cache Blocking)
    template <typename T>
    inline void matmul_impl(const Tensor& a, const Tensor& b, Tensor& out) {
        const T* pa = static_cast<const T*>(a.impl->storage->data_ptr()) + a.offset;
        const T* pb = static_cast<const T*>(b.impl->storage->data_ptr()) + b.offset;
        T* po = static_cast<T*>(out.impl->storage->data_ptr()) + out.offset;
        int M = a.shape[0], K = a.shape[1], N = b.shape[1];
        int sa0 = a.strides[0], sa1 = a.strides[1];
        int sb0 = b.strides[0], sb1 = b.strides[1];
        
        int minDim = std::min({M, K, N});
        int maxDim = std::max({M, K, N});

        // 智能路由：大方阵走多线程 Strassen，狭长矩阵/小矩阵走 Cache Blocking
        if (minDim > 64 && maxDim < minDim * 4) {
            int n = maxDim;
            bool needPadA = (M != n || K != n);
            bool needPadB = (K != n || N != n);

            std::vector<T> padA, padB;
            const T* ptrA = pa;
            const T* ptrB = pb;
            int strideA = sa0;
            int strideB = sb0;

            // Strassen 假设内层连续 (col_stride == 1)，如果是非连续视图则先物化
            if (needPadA || sa1 != 1) {
                padA.assign(n * n, T(0));
                for (int i = 0; i < M; ++i) {
                    for (int j = 0; j < K; ++j) padA[i * n + j] = pa[i * sa0 + j * sa1];
                }
                ptrA = padA.data();
                strideA = n;
            }
            if (needPadB || sb1 != 1) {
                padB.assign(n * n, T(0));
                for (int i = 0; i < K; ++i) {
                    for (int j = 0; j < N; ++j) padB[i * n + j] = pb[i * sb0 + j * sb1];
                }
                ptrB = padB.data();
                strideB = n;
            }

            std::vector<T> padC(n * n, T(0));
            tensor_strassen_view(ptrA, strideA, ptrB, strideB, padC.data(), n, n, 0);

            for (int i = 0; i < M; ++i) {
                for (int j = 0; j < N; ++j) {
                    po[i * N + j] = padC[i * n + j];
                }
            }
            return;
        }

        // L1 Cache 友好的分块大小
        constexpr int BLOCK_SIZE = 64;

        for (int i0 = 0; i0 < M; i0 += BLOCK_SIZE) {
            int i_end = std::min(i0 + BLOCK_SIZE, M);
            for (int k0 = 0; k0 < K; k0 += BLOCK_SIZE) {
                int k_end = std::min(k0 + BLOCK_SIZE, K);
                for (int j0 = 0; j0 < N; j0 += BLOCK_SIZE) {
                    int j_end = std::min(j0 + BLOCK_SIZE, N);

                    for (int i = i0; i < i_end; ++i) {
                        T* row = po + i * N;
                        const T* arow = pa + i * sa0;
                        for (int k = k0; k < k_end; ++k) {
                            T va = arow[k * sa1];
                            if (va == T(0)) continue;
                            const T* brow = pb + k * sb0;
                            for (int j = j0; j < j_end; ++j) {
                                row[j] += va * brow[j * sb1];
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- 通用二元逐元素运算 ----

    template <typename Op>
    inline Tensor tensor_binary_op(const Tensor& a, const Tensor& b, Op op,
                                   bool req_grad, std::shared_ptr<BackwardNode> grad_fn_node = nullptr) {
        if (a.dtype() != b.dtype())
            throw std::runtime_error("Tensor Error: DType mismatch.");

        std::vector<int> out_shape = Tensor::broadcastShapes(a.shape, b.shape);
        Tensor out(out_shape, a.dtype(), req_grad);
        out.impl->is_leaf = false;

        dispatch_binary(a, b, out, op);

        if (req_grad && grad_fn_node) out.impl->grad_fn = grad_fn_node;
        return out;
    }

    // ---- 加法 ----
    inline Tensor tensor_add(const Tensor& a, const Tensor& b) {
        bool rg = (a.impl->requires_grad || b.impl->requires_grad) && grad_enabled();
        Tensor out = tensor_binary_op(a, b, [](auto x, auto y) { return x + y; }, rg);
        if (rg) {
            out.impl->grad_fn = std::make_shared<AddBackward>(a, b, out);
        }
        return out;
    }

    // ---- 减法 ----
    inline Tensor tensor_sub(const Tensor& a, const Tensor& b) {
        bool rg = (a.impl->requires_grad || b.impl->requires_grad) && grad_enabled();
        Tensor out = tensor_binary_op(a, b, [](auto x, auto y) { return x - y; }, rg);
        if (rg) {
            out.impl->grad_fn = std::make_shared<SubBackward>(a, b, out);
        }
        return out;
    }

    // ---- 逐元素乘法 ----
    inline Tensor tensor_mul(const Tensor& a, const Tensor& b) {
        bool rg = (a.impl->requires_grad || b.impl->requires_grad) && grad_enabled();
        Tensor out = tensor_binary_op(a, b, [](auto x, auto y) { return x * y; }, rg);
        if (rg) {
            out.impl->grad_fn = std::make_shared<MulBackward>(a, b, out);
        }
        return out;
    }

    // ---- 逐元素除法 ----
    inline Tensor tensor_div(const Tensor& a, const Tensor& b) {
        bool rg = (a.impl->requires_grad || b.impl->requires_grad) && grad_enabled();
        Tensor out = tensor_binary_op(a, b, [](auto x, auto y) { return x / y; }, rg);
        if (rg) {
            out.impl->grad_fn = std::make_shared<DivBackward>(a, b, out);
        }
        return out;
    }

    // ---- 取负 ----
    inline Tensor tensor_neg(const Tensor& a) {
        bool rg = a.impl->requires_grad && grad_enabled();
        Tensor out(a.shape, a.dtype(), rg);
        out.impl->is_leaf = false;
        dispatch_unary(a, out, [](auto x) { return -x; });
        if (rg) {
            out.impl->grad_fn = std::make_shared<NegBackward>(a, out);
        }
        return out;
    }

    // ---- 逐元素乘方（标量指数）----
    inline Tensor tensor_pow_scalar(const Tensor& a, double exponent) {
        bool rg = a.impl->requires_grad && grad_enabled();
        Tensor out(a.shape, a.dtype(), rg);
        out.impl->is_leaf = false;
        dispatch_unary(a, out, [exponent](auto x) { return std::pow(static_cast<double>(x), exponent); });
        if (rg) {
            out.impl->grad_fn = std::make_shared<PowScalarBackward>(a, exponent, out);
        }
        return out;
    }

    // 前向声明（tensor_matmul 依赖 tensor_sum / matmul_batched）
    inline Tensor tensor_sum(const Tensor& a, int axis = -1, bool keepdim = false);
    inline Tensor matmul_batched(const Tensor& a, const Tensor& b);

    // ---- 矩阵乘法 (1D / 2D / batched) ----
    inline Tensor tensor_matmul(const Tensor& a, const Tensor& b) {
        if (a.dim() < 1 || b.dim() < 1)
            throw std::runtime_error("Tensor Error: matmul requires at least 1D tensors.");

        // 1D @ 1D：点积 → 标量
        if (a.dim() == 1 && b.dim() == 1) {
            if (a.shape[0] != b.shape[0])
                throw std::runtime_error("Tensor Error: matmul shape mismatch.");
            return tensor_sum(tensor_mul(a, b));
        }

        // 1D @ 2D：a[n] @ b[n,p] → [p]
        if (a.dim() == 1 && b.dim() == 2) {
            return tensor_matmul(a.unsqueeze(0), b).squeeze(0);
        }

        // 2D @ 1D：a[m,n] @ b[n] → [m]
        if (a.dim() == 2 && b.dim() == 1) {
            return tensor_matmul(a, b.unsqueeze(1)).squeeze(1);
        }

        // batched：>2D
        if (a.dim() > 2 || b.dim() > 2) {
            return matmul_batched(a, b);
        }

        int M = a.shape[0], K = a.shape[1], K2 = b.shape[0], N = b.shape[1];
        if (K != K2) throw std::runtime_error("Tensor Error: matmul shape mismatch (" +
            std::to_string(K) + " vs " + std::to_string(K2) + ").");

        bool rg = (a.impl->requires_grad || b.impl->requires_grad) && grad_enabled();
        Tensor out({M, N}, a.dtype(), rg);
        out.impl->is_leaf = false;

        switch (a.dtype()) {
            case DType::Float64: matmul_impl<double>(a, b, out); break;
            case DType::Float32: matmul_impl<float>(a, b, out); break;
            case DType::Int32:   matmul_impl<int32_t>(a, b, out); break;
            case DType::Int64:   matmul_impl<int64_t>(a, b, out); break;
        }

        if (rg) {
            out.impl->grad_fn = std::make_shared<MatMulBackward>(a, b, out);
        }
        return out;
    }

    // batched matmul：[..., m, n] @ [..., n, p] → [..., m, p]（batch 维度右对齐广播）
    inline Tensor matmul_batched(const Tensor& a, const Tensor& b) {
        int m = a.shape[a.dim()-2], n = a.shape[a.dim()-1];
        int n2 = b.shape[b.dim()-2], p = b.shape[b.dim()-1];
        if (n != n2) throw std::runtime_error("Tensor Error: matmul shape mismatch.");

        std::vector<int> batchA(a.shape.begin(), a.shape.end()-2);
        std::vector<int> batchB(b.shape.begin(), b.shape.end()-2);
        std::vector<int> batchOut = Tensor::broadcastShapes(batchA, batchB);

        std::vector<int> out_shape = batchOut;
        out_shape.push_back(m);
        out_shape.push_back(p);

        bool rg = (a.impl->requires_grad || b.impl->requires_grad) && grad_enabled();
        Tensor out(out_shape, a.dtype(), rg);
        out.impl->is_leaf = false;

        size_t num_batch = shapeToNumel(batchOut);
        for (size_t bi = 0; bi < num_batch; ++bi) {
            Tensor a_slice = a, b_slice = b, out_slice = out;
            size_t remain = bi;
            for (int d = static_cast<int>(batchOut.size()) - 1; d >= 0; --d) {
                size_t coord = remain % batchOut[d];
                remain /= batchOut[d];
                int a_d = d - (static_cast<int>(batchOut.size()) - static_cast<int>(batchA.size()));
                if (a_d >= 0) a_slice = a_slice.select(a_d, (batchA[a_d] == 1) ? 0 : static_cast<int>(coord));
                int b_d = d - (static_cast<int>(batchOut.size()) - static_cast<int>(batchB.size()));
                if (b_d >= 0) b_slice = b_slice.select(b_d, (batchB[b_d] == 1) ? 0 : static_cast<int>(coord));
                out_slice = out_slice.select(d, static_cast<int>(coord));
            }
            switch (a.dtype()) {
                case DType::Float64: matmul_impl<double>(a_slice, b_slice, out_slice); break;
                case DType::Float32: matmul_impl<float>(a_slice, b_slice, out_slice); break;
                case DType::Int32:   matmul_impl<int32_t>(a_slice, b_slice, out_slice); break;
                case DType::Int64:   matmul_impl<int64_t>(a_slice, b_slice, out_slice); break;
            }
        }

        if (rg) {
            out.impl->grad_fn = std::make_shared<MatMulBackward>(a, b, out);
        }
        return out;
    }

    // MatMulBackward::apply 实现
    inline void MatMulBackward::apply() {
        Tensor out_locked = out.lock();
        if (!out_locked.impl || !out_locked.impl->grad) return;
        // C = A @ B → dA = dC @ B^T, dB = A^T @ dC
        if (a.impl->requires_grad) {
            Tensor bT = b.T().contiguous();
            Tensor da = tensor_matmul(*out_locked.impl->grad, bT);
            da.impl->requires_grad = false;
            if (!a.impl->grad) a.impl->grad = std::make_shared<Tensor>(a.shape, a.dtype(), false);
            tensor_accumulate_grad(*a.impl->grad, da);
        }
        if (b.impl->requires_grad) {
            Tensor aT = a.T().contiguous();
            Tensor db = tensor_matmul(aT, *out_locked.impl->grad);
            db.impl->requires_grad = false;
            if (!b.impl->grad) b.impl->grad = std::make_shared<Tensor>(b.shape, b.dtype(), false);
            tensor_accumulate_grad(*b.impl->grad, db);
        }
    }

    // ---- sum (全局规约) ----
    inline Tensor tensor_sum(const Tensor& a, int axis, bool keepdim) {
        bool rg = a.impl->requires_grad && grad_enabled();
        if (axis < 0) {
            Tensor out({1}, a.dtype(), rg);
            out.impl->is_leaf = false;
            double s = 0.0;
            for (size_t i = 0; i < a.numel(); ++i) s += a.getFlat(i);
            out.setFlat(0, s);
            if (rg) {
                out.impl->grad_fn = std::make_shared<SumBackward>(a, out);
            }
            return out;
        }
        if (axis >= a.dim()) throw std::runtime_error("Tensor Error: sum axis out of range.");
        std::vector<int> out_shape;
        for (int d = 0; d < a.dim(); ++d) {
            if (d == axis) { if (keepdim) out_shape.push_back(1); }
            else out_shape.push_back(a.shape[d]);
        }
        Tensor out(out_shape, a.dtype(), rg);
        out.impl->is_leaf = false;
        int axis_size = a.shape[axis];
        size_t outer = 1, inner = 1;
        for (int d = 0; d < axis; ++d) outer *= a.shape[d];
        for (int d = axis + 1; d < a.dim(); ++d) inner *= a.shape[d];
        for (size_t o = 0; o < outer; ++o) {
            for (size_t in = 0; in < inner; ++in) {
                double s = 0.0;
                for (int k = 0; k < axis_size; ++k) s += a.getFlat((o * axis_size + k) * inner + in);
                out.setFlat(o * inner + in, s);
            }
        }
        if (rg) {
            out.impl->grad_fn = std::make_shared<SumAxisBackward>(a, out, axis);
        }
        return out;
    }

    // ---- mean (全局或沿 axis) ----
    inline Tensor tensor_mean(const Tensor& a, int axis = -1, bool keepdim = false) {
        bool rg = a.impl->requires_grad && grad_enabled();
        if (axis < 0) {
            Tensor out({1}, a.dtype(), rg);
            out.impl->is_leaf = false;
            double s = 0.0;
            for (size_t i = 0; i < a.numel(); ++i) s += a.getFlat(i);
            out.setFlat(0, s / static_cast<double>(a.numel()));
            if (rg) {
                out.impl->grad_fn = std::make_shared<MeanBackward>(a, out);
            }
            return out;
        }
        if (axis >= a.dim()) throw std::runtime_error("Tensor Error: mean axis out of range.");
        std::vector<int> out_shape;
        for (int d = 0; d < a.dim(); ++d) {
            if (d == axis) { if (keepdim) out_shape.push_back(1); }
            else out_shape.push_back(a.shape[d]);
        }
        Tensor out(out_shape, a.dtype(), rg);
        out.impl->is_leaf = false;
        int axis_size = a.shape[axis];
        size_t outer = 1, inner = 1;
        for (int d = 0; d < axis; ++d) outer *= a.shape[d];
        for (int d = axis + 1; d < a.dim(); ++d) inner *= a.shape[d];
        for (size_t o = 0; o < outer; ++o) {
            for (size_t in = 0; in < inner; ++in) {
                double s = 0.0;
                for (int k = 0; k < axis_size; ++k) s += a.getFlat((o * axis_size + k) * inner + in);
                out.setFlat(o * inner + in, s / static_cast<double>(axis_size));
            }
        }
        if (rg) {
            out.impl->grad_fn = std::make_shared<MeanAxisBackward>(a, out, axis);
        }
        return out;
    }

    // ---- max / min (全局规约，不带梯度) ----
    inline Tensor tensor_max(const Tensor& a) {
        Tensor out({1}, a.dtype(), false);
        double m = a.getFlat(0);
        for (size_t i = 1; i < a.numel(); ++i) { double v = a.getFlat(i); if (v > m) m = v; }
        out.setFlat(0, m);
        return out;
    }

    inline Tensor tensor_min(const Tensor& a) {
        Tensor out({1}, a.dtype(), false);
        double m = a.getFlat(0);
        for (size_t i = 1; i < a.numel(); ++i) { double v = a.getFlat(i); if (v < m) m = v; }
        out.setFlat(0, m);
        return out;
    }

    // ---- clamp：裁剪到 [min, max]（带梯度）----
    inline Tensor tensor_clamp(const Tensor& a, double min_val, double max_val) {
        bool rg = a.impl->requires_grad && grad_enabled();
        Tensor out(a.shape, a.dtype(), rg);
        out.impl->is_leaf = false;
        dispatch_unary(a, out, [min_val, max_val](auto x) { return x < min_val ? min_val : (x > max_val ? max_val : x); });
        if (rg) {
            out.impl->grad_fn = std::make_shared<ClampBackward>(a, out, min_val, max_val);
        }
        return out;
    }

    // ---- argmax / argmin（返回线性索引，不带梯度）----
    inline int64_t tensor_argmax(const Tensor& a) {
        if (a.numel() == 0) throw std::runtime_error("Tensor Error: argmax on empty tensor.");
        size_t best = 0;
        double best_v = a.getFlat(0);
        for (size_t i = 1; i < a.numel(); ++i) { double v = a.getFlat(i); if (v > best_v) { best_v = v; best = i; } }
        return static_cast<int64_t>(best);
    }

    inline int64_t tensor_argmin(const Tensor& a) {
        if (a.numel() == 0) throw std::runtime_error("Tensor Error: argmin on empty tensor.");
        size_t best = 0;
        double best_v = a.getFlat(0);
        for (size_t i = 1; i < a.numel(); ++i) { double v = a.getFlat(i); if (v < best_v) { best_v = v; best = i; } }
        return static_cast<int64_t>(best);
    }

    // ---- 逐元素一元函数 ----
    inline Tensor tensor_exp(const Tensor& a) {
        bool rg = a.impl->requires_grad && grad_enabled();
        Tensor out(a.shape, a.dtype(), rg);
        out.impl->is_leaf = false;
        dispatch_unary(a, out, [](auto x) { return std::exp(static_cast<double>(x)); });
        if (rg) {
            out.impl->grad_fn = std::make_shared<ExpBackward>(a, out);
        }
        return out;
    }

    inline Tensor tensor_log(const Tensor& a) {
        bool rg = a.impl->requires_grad && grad_enabled();
        Tensor out(a.shape, a.dtype(), rg);
        out.impl->is_leaf = false;
        dispatch_unary(a, out, [](auto x) { return std::log(static_cast<double>(x)); });
        if (rg) {
            out.impl->grad_fn = std::make_shared<LogBackward>(a, out);
        }
        return out;
    }

    inline Tensor tensor_sqrt(const Tensor& a) {
        return tensor_pow_scalar(a, 0.5);
    }

    inline Tensor tensor_abs(const Tensor& a) {
        Tensor out(a.shape, a.dtype(), false); // abs 的梯度在 0 处不可微，简化处理
        out.impl->is_leaf = false;
        dispatch_unary(a, out, [](auto x) { return std::abs(x); });
        return out;
    }

    // ---- 激活函数 ----
    inline Tensor tensor_relu(const Tensor& a) {
        bool rg = a.impl->requires_grad && grad_enabled();
        Tensor out(a.shape, a.dtype(), rg);
        out.impl->is_leaf = false;
        dispatch_unary(a, out, [](auto v) { return v > 0 ? v : 0; });
        if (rg) {
            out.impl->grad_fn = std::make_shared<ReluBackward>(a, out);
        }
        return out;
    }

    inline Tensor tensor_sigmoid(const Tensor& a) {
        bool rg = a.impl->requires_grad && grad_enabled();
        Tensor out(a.shape, a.dtype(), rg);
        out.impl->is_leaf = false;
        dispatch_unary(a, out, [](auto x) { return 1.0 / (1.0 + std::exp(static_cast<double>(-x))); });
        if (rg) {
            out.impl->grad_fn = std::make_shared<SigmoidBackward>(a, out);
        }
        return out;
    }

    inline Tensor tensor_tanh(const Tensor& a) {
        bool rg = a.impl->requires_grad && grad_enabled();
        Tensor out(a.shape, a.dtype(), rg);
        out.impl->is_leaf = false;
        dispatch_unary(a, out, [](auto x) { return std::tanh(static_cast<double>(x)); });
        if (rg) {
            out.impl->grad_fn = std::make_shared<TanhBackward>(a, out);
        }
        return out;
    }

    // ---- 比较运算（不带梯度）----
    inline Tensor tensor_eq(const Tensor& a, const Tensor& b) {
        return tensor_binary_op(a, b, [](auto x, auto y) { return x == y ? 1 : 0; }, false);
    }
    inline Tensor tensor_neq(const Tensor& a, const Tensor& b) {
        return tensor_binary_op(a, b, [](auto x, auto y) { return x != y ? 1 : 0; }, false);
    }
    inline Tensor tensor_lt(const Tensor& a, const Tensor& b) {
        return tensor_binary_op(a, b, [](auto x, auto y) { return x < y ? 1 : 0; }, false);
    }
    inline Tensor tensor_gt(const Tensor& a, const Tensor& b) {
        return tensor_binary_op(a, b, [](auto x, auto y) { return x > y ? 1 : 0; }, false);
    }
    inline Tensor tensor_le(const Tensor& a, const Tensor& b) {
        return tensor_binary_op(a, b, [](auto x, auto y) { return x <= y ? 1 : 0; }, false);
    }
    inline Tensor tensor_ge(const Tensor& a, const Tensor& b) {
        return tensor_binary_op(a, b, [](auto x, auto y) { return x >= y ? 1 : 0; }, false);
    }

    // ---- 工厂函数 ----
    inline Tensor tensor_full(const std::vector<int>& shape, double val, DType dt = DType::Float64, bool req_grad = false) {
        Tensor t(shape, dt, req_grad);
        t.fill_(val);
        return t;
    }

    inline Tensor tensor_ones(const std::vector<int>& shape, DType dt = DType::Float64, bool req_grad = false) {
        return tensor_full(shape, 1.0, dt, req_grad);
    }

    inline Tensor tensor_zeros(const std::vector<int>& shape, DType dt = DType::Float64, bool req_grad = false) {
        return Tensor(shape, dt, req_grad);
    }

    inline Tensor tensor_arange(double start, double end, double step = 1.0, DType dt = DType::Float64) {
        std::vector<int> s;
        int n = static_cast<int>(std::ceil((end - start) / step));
        if (n <= 0) n = 0;
        s.push_back(n);
        Tensor t(s, dt, false);
        for (int i = 0; i < n; ++i) t.setFlat(i, start + i * step);
        return t;
    }

    inline Tensor tensor_linspace(double start, double end, int steps, DType dt = DType::Float64) {
        if (steps < 1) throw std::runtime_error("Tensor Error: linspace requires steps >= 1.");
        Tensor t({steps}, dt, false);
        if (steps == 1) { t.setFlat(0, start); return t; }
        for (int i = 0; i < steps; ++i) {
            t.setFlat(i, start + i * (end - start) / (steps - 1));
        }
        return t;
    }

    inline Tensor tensor_eye(int n, DType dt = DType::Float64) {
        Tensor t({n, n}, dt, false);
        for (int i = 0; i < n; ++i) t.setFlat(i * n + i, 1.0);
        return t;
    }

    // 从 double 数组创建
    inline Tensor tensor_from_data(const std::vector<double>& data, const std::vector<int>& shape, DType dt = DType::Float64, bool req_grad = false) {
        size_t expected = shapeToNumel(shape);
        if (data.size() != expected) throw std::runtime_error("Tensor Error: data size mismatch for shape.");
        Tensor t(shape, dt, req_grad);
        for (size_t i = 0; i < data.size(); ++i) t.setFlat(i, data[i]);
        return t;
    }

    // 标量张量
    inline Tensor tensor_scalar(double val, DType dt = DType::Float64, bool req_grad = false) {
        Tensor t({1}, dt, req_grad);
        t.setFlat(0, val);
        return t;
    }

    // ---- Concatenation ----
    inline Tensor tensor_cat(const std::vector<Tensor>& tensors, int axis = 0) {
        if (tensors.empty()) throw std::runtime_error("Tensor Error: cat requires at least one tensor.");
        int ndim = tensors[0].dim();
        if (axis < 0) axis += ndim;
        if (axis < 0 || axis >= ndim) throw std::runtime_error("Tensor Error: cat axis out of range.");

        // Validate shapes
        for (size_t ti = 1; ti < tensors.size(); ++ti) {
            if (tensors[ti].dim() != ndim) throw std::runtime_error("Tensor Error: cat dimension mismatch.");
            for (int d = 0; d < ndim; ++d) {
                if (d != axis && tensors[ti].shape[d] != tensors[0].shape[d])
                    throw std::runtime_error("Tensor Error: cat shape mismatch on dim " + std::to_string(d) + ".");
            }
        }

        std::vector<int> out_shape = tensors[0].shape;
        int total_along_axis = 0;
        for (const auto& t : tensors) total_along_axis += t.shape[axis];
        out_shape[axis] = total_along_axis;

        Tensor out(out_shape, tensors[0].dtype(), false);

        // Copy data
        size_t outer = 1, inner = 1;
        for (int d = 0; d < axis; ++d) outer *= out_shape[d];
        for (int d = axis + 1; d < ndim; ++d) inner *= out_shape[d];

        size_t write_idx = 0;
        for (size_t o = 0; o < outer; ++o) {
            for (const auto& t : tensors) {
                size_t chunk = t.shape[axis] * inner;
                for (size_t c = 0; c < chunk; ++c) {
                    size_t src_linear = o * t.shape[axis] * inner + c;
                    out.setFlat(write_idx++, t.getFlat(src_linear));
                }
            }
        }
        return out;
    }

    // ---- Stack ----
    inline Tensor tensor_stack(const std::vector<Tensor>& tensors, int axis = 0) {
        if (tensors.empty()) throw std::runtime_error("Tensor Error: stack requires at least one tensor.");
        // unsqueeze each tensor at axis, then cat
        std::vector<Tensor> expanded;
        for (const auto& t : tensors) expanded.push_back(t.unsqueeze(axis));
        return tensor_cat(expanded, axis);
    }

    // ---- 行列操作（Matrix-style Row/Col Operations）----
    
    // getRow: 返回第 row 行作为 1D tensor
    inline Tensor tensor_getrow(const Tensor& t, int row) {
        if (t.dim() < 2) throw std::runtime_error("Tensor Error: getrow requires at least 2D tensor.");
        if (row < 0) row += t.shape[0];
        if (row < 0 || row >= t.shape[0])
            throw std::runtime_error("Tensor Error: getrow index out of range.");
        
        std::vector<int> row_shape(t.shape.begin() + 1, t.shape.end());
        Tensor result(row_shape, t.dtype(), false);
        size_t row_numel = t.numel() / t.shape[0];
        for (size_t i = 0; i < row_numel; ++i) {
            result.setFlat(i, t.getFlat(row * row_numel + i));
        }
        return result;
    }

    // getCol: 返回第 col 列作为 Tensor（仅对 2D）
    inline Tensor tensor_getcol(const Tensor& t, int col) {
        if (t.dim() != 2) throw std::runtime_error("Tensor Error: getcol requires 2D tensor.");
        int rows = t.shape[0], cols = t.shape[1];
        if (col < 0) col += cols;
        if (col < 0 || col >= cols)
            throw std::runtime_error("Tensor Error: getcol index out of range.");
        
        Tensor result({rows}, t.dtype(), false);
        for (int i = 0; i < rows; ++i) {
            result.setFlat(i, t.getFlat(i * cols + col));
        }
        return result;
    }

    // deleteRow: 删除第 row 行，返回新 tensor（仅对 2D）
    inline Tensor tensor_deleterow(const Tensor& t, int row) {
        if (t.dim() != 2) throw std::runtime_error("Tensor Error: deleterow requires 2D tensor.");
        if (t.shape[0] <= 1) throw std::runtime_error("Tensor Error: Cannot delete row from 1-row tensor.");
        
        if (row < 0) row += t.shape[0];
        if (row < 0 || row >= t.shape[0])
            throw std::runtime_error("Tensor Error: deleterow index out of range.");
        
        int rows = t.shape[0], cols = t.shape[1];
        Tensor result({rows - 1, cols}, t.dtype(), false);
        int wr = 0;
        for (int i = 0; i < rows; ++i) {
            if (i == row) continue;
            for (int j = 0; j < cols; ++j) {
                result.setFlat(wr * cols + j, t.getFlat(i * cols + j));
            }
            wr++;
        }
        return result;
    }

    // deleteCol: 删除第 col 列，返回新 tensor（仅对 2D）
    inline Tensor tensor_deletecol(const Tensor& t, int col) {
        if (t.dim() != 2) throw std::runtime_error("Tensor Error: deletecol requires 2D tensor.");
        if (t.shape[1] <= 1) throw std::runtime_error("Tensor Error: Cannot delete col from 1-col tensor.");
        
        if (col < 0) col += t.shape[1];
        if (col < 0 || col >= t.shape[1])
            throw std::runtime_error("Tensor Error: deletecol index out of range.");
        
        int rows = t.shape[0], cols = t.shape[1];
        Tensor result({rows, cols - 1}, t.dtype(), false);
        for (int i = 0; i < rows; ++i) {
            int wj = 0;
            for (int j = 0; j < cols; ++j) {
                if (j == col) continue;
                result.setFlat(i * (cols - 1) + wj, t.getFlat(i * cols + j));
                wj++;
            }
        }
        return result;
    }

    // swapRows: 交换两行（仅对 2D）
    inline Tensor tensor_swaprows(const Tensor& t, int r1, int r2) {
        if (t.dim() != 2) throw std::runtime_error("Tensor Error: swaprows requires 2D tensor.");
        int rows = t.shape[0], cols = t.shape[1];
        if (r1 < 0) r1 += rows;
        if (r2 < 0) r2 += rows;
        if (r1 < 0 || r1 >= rows || r2 < 0 || r2 >= rows)
            throw std::runtime_error("Tensor Error: swaprows index out of range.");
        
        Tensor result = t.clone();
        for (int j = 0; j < cols; ++j) {
            double tmp = result.getFlat(r1 * cols + j);
            result.setFlat(r1 * cols + j, result.getFlat(r2 * cols + j));
            result.setFlat(r2 * cols + j, tmp);
        }
        return result;
    }

    // ---- 矩阵连接（Block concatenation）----
    inline Tensor tensor_hstack(const std::vector<Tensor>& tensors) {
        // 水平拼接：假设都是 2D，按列连接
        if (tensors.empty()) throw std::runtime_error("Tensor Error: hstack requires at least one tensor.");
        return tensor_cat(tensors, 1);  // axis=1 表示列方向
    }

    inline Tensor tensor_vstack(const std::vector<Tensor>& tensors) {
        // 垂直拼接：假设都是 2D，按行连接
        if (tensors.empty()) throw std::runtime_error("Tensor Error: vstack requires at least one tensor.");
        return tensor_cat(tensors, 0);  // axis=0 表示行方向
    }

    // ---- Random ----
    inline Tensor tensor_rand(const std::vector<int>& shape, DType dt = DType::Float64, bool req_grad = false) {
        Tensor t(shape, dt, req_grad);
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        auto& gen = tensor_rng();
        for (size_t i = 0; i < t.numel(); ++i) t.setFlat(i, dist(gen));
        return t;
    }

    inline Tensor tensor_randn(const std::vector<int>& shape, DType dt = DType::Float64, bool req_grad = false) {
        Tensor t(shape, dt, req_grad);
        std::normal_distribution<double> dist(0.0, 1.0);
        auto& gen = tensor_rng();
        for (size_t i = 0; i < t.numel(); ++i) t.setFlat(i, dist(gen));
        return t;
    }

    // ---- Softmax (不跟踪梯度的简化版) ----
    inline Tensor tensor_softmax(const Tensor& a, int axis = -1) {
        if (a.dim() == 0) throw std::runtime_error("Tensor Error: softmax requires at least 1D.");
        if (axis < 0) axis += a.dim();

        Tensor out = a.clone();
        out.impl->requires_grad = false;
        out.impl->is_leaf = true;

        // 沿 axis 做 softmax
        size_t outer = 1, inner = 1, axis_size = a.shape[axis];
        for (int d = 0; d < axis; ++d) outer *= a.shape[d];
        for (int d = axis + 1; d < a.dim(); ++d) inner *= a.shape[d];

        for (size_t o = 0; o < outer; ++o) {
            for (size_t in = 0; in < inner; ++in) {
                // Find max for numerical stability
                double mx = -1e300;
                for (size_t k = 0; k < axis_size; ++k) {
                    size_t idx = o * axis_size * inner + k * inner + in;
                    double v = a.getFlat(idx);
                    if (v > mx) mx = v;
                }
                double sum_exp = 0.0;
                for (size_t k = 0; k < axis_size; ++k) {
                    size_t idx = o * axis_size * inner + k * inner + in;
                    double e = std::exp(a.getFlat(idx) - mx);
                    out.setFlat(idx, e);
                    sum_exp += e;
                }
                for (size_t k = 0; k < axis_size; ++k) {
                    size_t idx = o * axis_size * inner + k * inner + in;
                    out.setFlat(idx, out.getFlat(idx) / sum_exp);
                }
            }
        }
        return out;
    }

    // ---- MSE Loss ----
    inline Tensor tensor_mse_loss(const Tensor& pred, const Tensor& target) {
        Tensor diff = tensor_sub(pred, target);
        Tensor sq = tensor_mul(diff, diff);
        return tensor_mean(sq);
    }

    // ---- Cross Entropy Loss (logits, integer targets) ----
    // Simplified: pred is [N, C], target is [N] of class indices → scalar loss
    // Not differentiable through this implementation; use for evaluation.

    // ---- Zero gradients ----
    inline void tensor_zero_grad(Tensor& t) {
        if (t.impl->grad) t.impl->grad->fill_(0.0);
    }

    // ---- SGD step (in-place) ----
    inline void tensor_sgd_step(Tensor& param, double lr) {
        if (!param.impl->grad) return;
        for (size_t i = 0; i < param.numel(); ++i) {
            param.setFlat(i, param.getFlat(i) - lr * param.impl->grad->getFlat(i));
        }
    }

} // namespace jc

#endif // JC2_TENSOR_H
