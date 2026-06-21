#ifndef JC2_TENSOR_MODULE_H
#define JC2_TENSOR_MODULE_H

#include "Module.h"
#include "../math/Tensor.h"
#include "../memory/Value.h"

namespace jc {

    JC2_MODULE(tensor) {
        // ====================================================================
        // 1. 注册 Tensor 类
        // ====================================================================
        auto tensorClass = GcHeap::get().allocate<ObjClass>();
        tensorClass->name = "Tensor";
        env["Tensor"] = Value(tensorClass);

        // ====================================================================
        // 2. 辅助函数
        // ====================================================================
        auto getTensor = [](const Value& val) -> std::shared_ptr<Tensor> {
            if (!val.isInstance()) throw std::runtime_error("TypeError: Expected a Tensor instance.");
            auto inst = val.asInstance();
            if (!inst->nativeData.has_value() || inst->nativeData.type() != typeid(std::shared_ptr<Tensor>)) {
                throw std::runtime_error("TypeError: Instance is not a Tensor.");
            }
            return std::any_cast<std::shared_ptr<Tensor>>(inst->nativeData);
        };

        auto wrapTensor = [tensorClass](const Tensor& t) -> Value {
            auto inst = GcHeap::get().allocate<ObjInstance>();
            inst->classDef = tensorClass;
            inst->nativeData = std::make_shared<Tensor>(t);
            return Value(inst);
        };

        auto isTensor = [](const Value& val) -> bool {
            if (!val.isInstance()) return false;
            auto inst = val.asInstance();
            return inst->nativeData.has_value() && inst->nativeData.type() == typeid(std::shared_ptr<Tensor>);
        };

        // 辅助：从 list 提取 shape
        auto listToShape = [](const Value& val) -> std::vector<int> {
            if (!val.isObjType(ObjType::LIST)) throw std::runtime_error("TypeError: shape must be a list.");
            auto list = static_cast<ObjList*>(val.asObj());
            std::vector<int> shape;
            for (const auto& v : list->vec) shape.push_back(static_cast<int>(v.asDouble()));
            return shape;
        };

        // 辅助：从 list 提取 double 数据
        auto listToDoubles = [](const Value& val) -> std::vector<double> {
            if (!val.isObjType(ObjType::LIST)) throw std::runtime_error("TypeError: data must be a list.");
            auto list = static_cast<ObjList*>(val.asObj());
            std::vector<double> data;
            for (const auto& v : list->vec) data.push_back(v.asDouble());
            return data;
        };

        // 辅助：创建 dunder 闭包
        auto makeDunder = [&](const std::string& name, int param_count, NativeCallable fn) {
            std::vector<std::string> params;
            std::vector<bool> refs;
            params.push_back("self"); refs.push_back(false);
            for (int i = 1; i < param_count; ++i) {
                params.push_back("_p" + std::to_string(i));
                refs.push_back(false);
            }
            auto closure = GcHeap::get().allocate<ObjClosure>(params, refs, name, nullptr);
            closure->nativeFn = std::make_any<NativeCallable>(std::move(fn));
            tensorClass->methods[name] = closure;
        };

        // ====================================================================
        // 3. Dunder Methods
        // ====================================================================

        // __str__
        makeDunder("__str__", 1, [getTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            return Value(t->toString());
        });

        // __add__ : T + T
        makeDunder("__add__", 2, [getTensor, isTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t1 = getTensor(args[0]);
            if (isTensor(args[1])) {
                auto t2 = getTensor(args[1]);
                return wrapTensor(tensor_add(*t1, *t2));
            }
            // 标量加法
            Tensor scalar = tensor_scalar(args[1].asDouble());
            return wrapTensor(tensor_add(*t1, scalar));
        });

        // __radd__ : scalar + T
        makeDunder("__radd__", 2, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t1 = getTensor(args[0]);
            Tensor scalar = tensor_scalar(args[1].asDouble());
            return wrapTensor(tensor_add(scalar, *t1));
        });

        // __sub__
        makeDunder("__sub__", 2, [getTensor, isTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t1 = getTensor(args[0]);
            if (isTensor(args[1])) {
                return wrapTensor(tensor_sub(*t1, *getTensor(args[1])));
            }
            Tensor scalar = tensor_scalar(args[1].asDouble());
            return wrapTensor(tensor_sub(*t1, scalar));
        });

        // __rsub__
        makeDunder("__rsub__", 2, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t1 = getTensor(args[0]);
            Tensor scalar = tensor_scalar(args[1].asDouble());
            return wrapTensor(tensor_sub(scalar, *t1));
        });

        // __mul__
        makeDunder("__mul__", 2, [getTensor, isTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t1 = getTensor(args[0]);
            if (isTensor(args[1])) {
                return wrapTensor(tensor_mul(*t1, *getTensor(args[1])));
            }
            Tensor scalar = tensor_scalar(args[1].asDouble());
            return wrapTensor(tensor_mul(*t1, scalar));
        });

        // __rmul__
        makeDunder("__rmul__", 2, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t1 = getTensor(args[0]);
            Tensor scalar = tensor_scalar(args[1].asDouble());
            return wrapTensor(tensor_mul(scalar, *t1));
        });

        // __div__
        makeDunder("__div__", 2, [getTensor, isTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t1 = getTensor(args[0]);
            if (isTensor(args[1])) {
                return wrapTensor(tensor_div(*t1, *getTensor(args[1])));
            }
            Tensor scalar = tensor_scalar(args[1].asDouble());
            return wrapTensor(tensor_div(*t1, scalar));
        });

        // __rdiv__
        makeDunder("__rdiv__", 2, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t1 = getTensor(args[0]);
            Tensor scalar = tensor_scalar(args[1].asDouble());
            return wrapTensor(tensor_div(scalar, *t1));
        });

        // __pow__
        makeDunder("__pow__", 2, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t1 = getTensor(args[0]);
            double exponent = args[1].asDouble();
            return wrapTensor(tensor_pow_scalar(*t1, exponent));
        });

        // __neg__
        makeDunder("__neg__", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_neg(*getTensor(args[0])));
        });

        // __eq__
        makeDunder("__eq__", 2, [getTensor, isTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            if (!isTensor(args[1])) return Value(false);
            auto t1 = getTensor(args[0]);
            auto t2 = getTensor(args[1]);
            if (t1->shape != t2->shape) return Value(false);
            for (size_t i = 0; i < t1->numel(); ++i) {
                if (t1->getFlat(i) != t2->getFlat(i)) return Value(false);
            }
            return Value(true);
        });

        // __getitem__ : t[i] — select along dim 0
        makeDunder("__getitem__", 2, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            int idx = static_cast<int>(args[1].asDouble());
            if (t->dim() == 1) {
                // 返回标量
                return Value::fromDouble(t->getFlat(idx < 0 ? idx + t->shape[0] : idx));
            }
            return wrapTensor(t->select(0, idx));
        });

        // __setitem__ : t[i] = val (仅支持 1D 标量赋值)
        makeDunder("__setitem__", 3, [getTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            int idx = static_cast<int>(args[1].asDouble());
            if (idx < 0) idx += t->shape[0];
            if (t->dim() == 1) {
                t->setFlat(idx, args[2].asDouble());
            } else {
                throw std::runtime_error("Tensor Error: __setitem__ only supports 1D scalar assignment. Use setFlat() for N-D.");
            }
            return Value::none();
        });

        // __len__
        makeDunder("__len__", 1, [getTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            return Value::fromDouble(t->dim() > 0 ? static_cast<double>(t->shape[0]) : 0.0);
        });

        // __abs__
        makeDunder("__abs__", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_abs(*getTensor(args[0])));
        });

        // ====================================================================
        // 4. 实例方法（通过 Tensor.method_name 调用，第一个参数是 self）
        // ====================================================================

        // .item() — 提取标量
        makeDunder("item", 1, [getTensor](const std::vector<Value>& args) -> Value {
            return Value::fromDouble(getTensor(args[0])->item());
        });

        // .shape() — 返回形状列表
        makeDunder("shape", 1, [getTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            auto list = GcHeap::get().allocate<ObjList>();
            for (int s : t->shape) list->vec.push_back(Value::fromDouble(static_cast<double>(s)));
            return Value(list);
        });

        // .dim() — 返回维度数
        makeDunder("dim", 1, [getTensor](const std::vector<Value>& args) -> Value {
            return Value::fromDouble(static_cast<double>(getTensor(args[0])->dim()));
        });

        // .numel() — 返回元素总数
        makeDunder("numel", 1, [getTensor](const std::vector<Value>& args) -> Value {
            return Value::fromDouble(static_cast<double>(getTensor(args[0])->numel()));
        });

        // .dtype() — 返回类型字符串
        makeDunder("dtype", 1, [getTensor](const std::vector<Value>& args) -> Value {
            return Value(dtypeToString(getTensor(args[0])->dtype()));
        });

        // .clone()
        makeDunder("clone", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(getTensor(args[0])->clone());
        });

        // .contiguous()
        makeDunder("contiguous", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(getTensor(args[0])->contiguous());
        });

        // .view(shape_list)
        makeDunder("view", 2, [getTensor, wrapTensor, listToShape](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            auto shape = listToShape(args[1]);
            return wrapTensor(t->view(shape));
        });

        // .reshape(shape_list) — alias for contiguous().view()
        makeDunder("reshape", 2, [getTensor, wrapTensor, listToShape](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            auto shape = listToShape(args[1]);
            return wrapTensor(t->contiguous().view(shape));
        });

        // .T() — transpose last two dims
        makeDunder("T", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(getTensor(args[0])->T());
        });

        // .transpose(dim0, dim1)
        makeDunder("transpose", 3, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            int d0 = static_cast<int>(args[1].asDouble());
            int d1 = static_cast<int>(args[2].asDouble());
            return wrapTensor(t->transpose(d0, d1));
        });

        // .unsqueeze(dim)
        makeDunder("unsqueeze", 2, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(getTensor(args[0])->unsqueeze(static_cast<int>(args[1].asDouble())));
        });

        // .squeeze([dim])
        makeDunder("squeeze", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(getTensor(args[0])->squeeze());
        });

        // .fill_(val)
        makeDunder("fill_", 2, [getTensor](const std::vector<Value>& args) -> Value {
            getTensor(args[0])->fill_(args[1].asDouble());
            return args[0]; // return self
        });

        // .sum()
        makeDunder("sum", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_sum(*getTensor(args[0])));
        });

        // .mean()
        makeDunder("mean", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_mean(*getTensor(args[0])));
        });

        // .max()
        makeDunder("max", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_max(*getTensor(args[0])));
        });

        // .min()
        makeDunder("min", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_min(*getTensor(args[0])));
        });

        // .exp()
        makeDunder("exp", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_exp(*getTensor(args[0])));
        });

        // .log()
        makeDunder("log", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_log(*getTensor(args[0])));
        });

        // .sqrt()
        makeDunder("sqrt", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_sqrt(*getTensor(args[0])));
        });

        // .relu()
        makeDunder("relu", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_relu(*getTensor(args[0])));
        });

        // .sigmoid()
        makeDunder("sigmoid", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_sigmoid(*getTensor(args[0])));
        });

        // .tanh()
        makeDunder("tanh", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_tanh(*getTensor(args[0])));
        });

        // .softmax([axis])
        makeDunder("softmax", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_softmax(*getTensor(args[0]), -1));
        });

        // .matmul(other)
        makeDunder("matmul", 2, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_matmul(*getTensor(args[0]), *getTensor(args[1])));
        });

        // .backward()
        makeDunder("backward", 1, [getTensor](const std::vector<Value>& args) -> Value {
            getTensor(args[0])->backward();
            return Value::none();
        });

        // .grad — 作为方法调用返回梯度张量
        makeDunder("grad", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            if (!t->grad) return Value::none();
            return wrapTensor(*t->grad);
        });

        // .requires_grad — 返回 bool
        makeDunder("requires_grad", 1, [getTensor](const std::vector<Value>& args) -> Value {
            return Value(getTensor(args[0])->requires_grad ? true : false);
        });

        // .detach() — 返回不跟踪梯度的副本
        makeDunder("detach", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            Tensor t = getTensor(args[0])->clone();
            t.requires_grad = false;
            t.grad_fn = nullptr;
            t.grad = nullptr;
            t.is_leaf = true;
            return wrapTensor(t);
        });

        // .zero_grad()
        makeDunder("zero_grad", 1, [getTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            tensor_zero_grad(*t);
            return args[0]; // return self
        });

        // .tolist() — 转换为 JC2 list
        makeDunder("tolist", 1, [getTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            auto list = GcHeap::get().allocate<ObjList>();
            for (size_t i = 0; i < t->numel(); ++i) {
                list->vec.push_back(Value::fromDouble(t->getFlat(i)));
            }
            return Value(list);
        });

        // .getFlat(idx) — 按线性索引读取
        makeDunder("getFlat", 2, [getTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            size_t idx = static_cast<size_t>(args[1].asDouble());
            return Value::fromDouble(t->getFlat(idx));
        });

        // .setFlat(idx, val) — 按线性索引写入
        makeDunder("setFlat", 3, [getTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            size_t idx = static_cast<size_t>(args[1].asDouble());
            t->setFlat(idx, args[2].asDouble());
            return args[0];
        });

        // ====================================================================
        // 5. 模块级全局工厂函数 (tensor.xxx)
        // ====================================================================
        ModuleReg reg(env, builtins, arity);

        // tensor.tensor(data_list, shape_list, [requires_grad])
        reg.reg("tensor", {2, 3}, [wrapTensor, listToDoubles, listToShape](const std::vector<Value>& args) -> Value {
            auto data = listToDoubles(args[0]);
            auto shape = listToShape(args[1]);
            bool req_grad = (args.size() >= 3) ? args[2].truthy() : false;
            return wrapTensor(tensor_from_data(data, shape, req_grad));
        });

        // tensor.scalar(val, [requires_grad])
        reg.reg("scalar", {1, 2}, [wrapTensor](const std::vector<Value>& args) -> Value {
            bool rg = (args.size() >= 2) ? args[1].truthy() : false;
            return wrapTensor(tensor_scalar(args[0].asDouble(), rg));
        });

        // tensor.zeros(shape_list, [requires_grad])
        reg.reg("zeros", {1, 2}, [wrapTensor, listToShape](const std::vector<Value>& args) -> Value {
            auto shape = listToShape(args[0]);
            bool rg = (args.size() >= 2) ? args[1].truthy() : false;
            return wrapTensor(tensor_zeros(shape, DType::Float64, rg));
        });

        // tensor.ones(shape_list, [requires_grad])
        reg.reg("ones", {1, 2}, [wrapTensor, listToShape](const std::vector<Value>& args) -> Value {
            auto shape = listToShape(args[0]);
            bool rg = (args.size() >= 2) ? args[1].truthy() : false;
            return wrapTensor(tensor_ones(shape, DType::Float64, rg));
        });

        // tensor.full(shape_list, fill_value, [requires_grad])
        reg.reg("full", {2, 3}, [wrapTensor, listToShape](const std::vector<Value>& args) -> Value {
            auto shape = listToShape(args[0]);
            double val = args[1].asDouble();
            bool rg = (args.size() >= 3) ? args[2].truthy() : false;
            return wrapTensor(tensor_full(shape, val, DType::Float64, rg));
        });

        // tensor.eye(n)
        reg.reg("eye", {1}, [wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_eye(static_cast<int>(args[0].asDouble())));
        });

        // tensor.arange(start, end, [step])
        reg.reg("arange", {2, 3}, [wrapTensor](const std::vector<Value>& args) -> Value {
            double start = args[0].asDouble();
            double end = args[1].asDouble();
            double step = (args.size() >= 3) ? args[2].asDouble() : 1.0;
            return wrapTensor(tensor_arange(start, end, step));
        });

        // tensor.linspace(start, end, steps)
        reg.reg("linspace", {3}, [wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_linspace(args[0].asDouble(), args[1].asDouble(), static_cast<int>(args[2].asDouble())));
        });

        // tensor.rand(shape_list)
        reg.reg("rand", {1}, [wrapTensor, listToShape](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_rand(listToShape(args[0])));
        });

        // tensor.randn(shape_list)
        reg.reg("randn", {1}, [wrapTensor, listToShape](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_randn(listToShape(args[0])));
        });

        // tensor.matmul(a, b)
        reg.reg("matmul", {2}, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_matmul(*getTensor(args[0]), *getTensor(args[1])));
        });

        // tensor.cat(list_of_tensors, [axis])
        reg.reg("cat", {1, 2}, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            if (!args[0].isObjType(ObjType::LIST)) throw std::runtime_error("TypeError: cat expects a list of tensors.");
            auto list = static_cast<ObjList*>(args[0].asObj());
            std::vector<Tensor> tensors;
            for (const auto& v : list->vec) tensors.push_back(*getTensor(v));
            int axis = (args.size() >= 2) ? static_cast<int>(args[1].asDouble()) : 0;
            return wrapTensor(tensor_cat(tensors, axis));
        });

        // tensor.stack(list_of_tensors, [axis])
        reg.reg("stack", {1, 2}, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            if (!args[0].isObjType(ObjType::LIST)) throw std::runtime_error("TypeError: stack expects a list of tensors.");
            auto list = static_cast<ObjList*>(args[0].asObj());
            std::vector<Tensor> tensors;
            for (const auto& v : list->vec) tensors.push_back(*getTensor(v));
            int axis = (args.size() >= 2) ? static_cast<int>(args[1].asDouble()) : 0;
            return wrapTensor(tensor_stack(tensors, axis));
        });

        // tensor.mse_loss(pred, target)
        reg.reg("mse_loss", {2}, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            return wrapTensor(tensor_mse_loss(*getTensor(args[0]), *getTensor(args[1])));
        });

        // tensor.softmax(t, [axis])
        reg.reg("softmax", {1, 2}, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            int axis = (args.size() >= 2) ? static_cast<int>(args[1].asDouble()) : -1;
            return wrapTensor(tensor_softmax(*getTensor(args[0]), axis));
        });

        // tensor.backward(t)
        reg.reg("backward", {1}, [getTensor](const std::vector<Value>& args) -> Value {
            getTensor(args[0])->backward();
            return Value::none();
        });

        // tensor.zero_grad(t)
        reg.reg("zero_grad", {1}, [getTensor](const std::vector<Value>& args) -> Value {
            tensor_zero_grad(*getTensor(args[0]));
            return Value::none();
        });

        // tensor.sgd_step(t, lr) — 原地 SGD 更新
        reg.reg("sgd_step", {2}, [getTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            double lr = args[1].asDouble();
            tensor_sgd_step(*t, lr);
            return Value::none();
        });

        // tensor.no_grad(fn) — 执行函数，禁用梯度追踪（简化版：仅标注）
        reg.reg("no_grad", {1}, [](const std::vector<Value>& args) -> Value {
            // 简化实现：直接调用函数，不做任何梯度追踪管理
            // 实际应用中应使用 detach()
            if (!args[0].isObjType(ObjType::CLOSURE)) throw std::runtime_error("TypeError: no_grad expects a function.");
            auto closure = static_cast<ObjClosure*>(args[0].asObj());
            if (jc::helpers::callFunctionCallback) {
                return jc::helpers::callFunctionCallback(closure, {});
            }
            throw std::runtime_error("Tensor Error: no_grad requires VM callback.");
        });

        // tensor.isTensor(val) — 类型判断
        reg.reg("isTensor", {1}, [isTensor](const std::vector<Value>& args) -> Value {
            return Value(isTensor(args[0]));
        });
    }

} // namespace jc

#endif // JC2_TENSOR_MODULE_H
