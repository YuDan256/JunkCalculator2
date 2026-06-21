#ifndef JC2_TENSOR_MODULE_H
#define JC2_TENSOR_MODULE_H

#include "Module.h"
#include "../math/Tensor.h"
#include "../memory/Value.h"

#include <utility>

namespace jc {

    JC2_MODULE(tensor) {
        // ====================================================================
        // 1. 注册 Tensor 类
        // ====================================================================
        auto tensorClass = GcHeap::get().allocate<ObjClass>();
        tensorClass->name = "Tensor";
        env["Tensor"] = Value(tensorClass);

        // ====================================================================
        // 类方法绑定辅助函数
        // ====================================================================
        auto addTensorMethod = [&](const std::string& name, int param_count, NativeCallable fn) {
            std::vector<std::string> params;
            std::vector<bool> refs;
            for (int i = 0; i < param_count; ++i) {
                params.push_back("_p" + std::to_string(i));
                refs.push_back(false);
            }
            auto closure = GcHeap::get().allocate<ObjClosure>(params, refs, name, nullptr);
            closure->nativeFn = jc::VM::makeNativeFn(fn);
            closure->defaultValues.resize(param_count, Value::none());
            tensorClass->methods[name] = closure;
        };

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

        auto parseTensorOptions = [](const std::vector<Value>& args, size_t start,
                                     DType defaultDt = DType::Float64,
                                     bool defaultRequiresGrad = false) -> std::pair<DType, bool> {
            DType dt = defaultDt;
            bool req_grad = defaultRequiresGrad;
            for (size_t i = start; i < args.size(); ++i) {
                if (args[i].isString()) {
                    dt = stringToDType(args[i].asString());
                } else {
                    req_grad = args[i].truthy();
                }
            }
            return {dt, req_grad};
        };

        // __str__
        addTensorMethod("__str__", 0, [getTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return Value(t->toString());
        });

        // __add__ : T + T
        addTensorMethod("__add__", 1, [getTensor, isTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t1 = getTensor(selfVal);
            if (isTensor(args[0])) {
                auto t2 = getTensor(args[0]);
                return wrapTensor(tensor_add(*t1, *t2));
            }
            Tensor scalar = tensor_scalar(args[0].asDouble());
            return wrapTensor(tensor_add(*t1, scalar));
        });

        // __radd__ : scalar + T
        addTensorMethod("__radd__", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t1 = getTensor(selfVal);
            Tensor scalar = tensor_scalar(args[0].asDouble());
            return wrapTensor(tensor_add(scalar, *t1));
        });

        // __sub__
        addTensorMethod("__sub__", 1, [getTensor, isTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t1 = getTensor(selfVal);
            if (isTensor(args[0])) {
                return wrapTensor(tensor_sub(*t1, *getTensor(args[0])));
            }
            Tensor scalar = tensor_scalar(args[0].asDouble());
            return wrapTensor(tensor_sub(*t1, scalar));
        });

        // __rsub__
        addTensorMethod("__rsub__", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t1 = getTensor(selfVal);
            Tensor scalar = tensor_scalar(args[0].asDouble());
            return wrapTensor(tensor_sub(scalar, *t1));
        });

        // __mul__
        addTensorMethod("__mul__", 1, [getTensor, isTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t1 = getTensor(selfVal);
            if (isTensor(args[0])) {
                return wrapTensor(tensor_mul(*t1, *getTensor(args[0])));
            }
            Tensor scalar = tensor_scalar(args[0].asDouble());
            return wrapTensor(tensor_mul(*t1, scalar));
        });

        // __rmul__
        addTensorMethod("__rmul__", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t1 = getTensor(selfVal);
            Tensor scalar = tensor_scalar(args[0].asDouble());
            return wrapTensor(tensor_mul(scalar, *t1));
        });

        // __div__
        addTensorMethod("__div__", 1, [getTensor, isTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t1 = getTensor(selfVal);
            if (isTensor(args[0])) {
                return wrapTensor(tensor_div(*t1, *getTensor(args[0])));
            }
            Tensor scalar = tensor_scalar(args[0].asDouble());
            return wrapTensor(tensor_div(*t1, scalar));
        });

        // __rdiv__
        addTensorMethod("__rdiv__", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t1 = getTensor(selfVal);
            Tensor scalar = tensor_scalar(args[0].asDouble());
            return wrapTensor(tensor_div(scalar, *t1));
        });

        // __pow__
        addTensorMethod("__pow__", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t1 = getTensor(selfVal);
            double exponent = args[0].asDouble();
            return wrapTensor(tensor_pow_scalar(*t1, exponent));
        });

        // __neg__
        addTensorMethod("__neg__", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            return wrapTensor(tensor_neg(*getTensor(selfVal)));
        });

        // __eq__
        addTensorMethod("__eq__", 1, [getTensor, isTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            if (!isTensor(args[0])) return Value(false);
            auto t1 = getTensor(selfVal);
            auto t2 = getTensor(args[0]);
            if (t1->shape != t2->shape) return Value(false);
            for (size_t i = 0; i < t1->numel(); ++i) {
                if (t1->getFlat(i) != t2->getFlat(i)) return Value(false);
            }
            return Value(true);
        });

        // __getitem__ : t[i] — select along dim 0
        addTensorMethod("__getitem__", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            int idx = static_cast<int>(args[0].asDouble());
            if (t->dim() == 1) {
                return Value::fromDouble(t->getFlat(idx < 0 ? idx + t->shape[0] : idx));
            }
            return wrapTensor(t->select(0, idx));
        });

        // __setitem__ : t[i] = val (仅支持 1D 标量赋值)
        addTensorMethod("__setitem__", 2, [getTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            int idx = static_cast<int>(args[0].asDouble());
            if (idx < 0) idx += t->shape[0];
            if (t->dim() == 1) {
                t->setFlat(idx, args[1].asDouble());
            } else {
                throw std::runtime_error("Tensor Error: __setitem__ only supports 1D scalar assignment. Use setFlat() for N-D.");
            }
            return Value::none();
        });

        // __len__
        addTensorMethod("__len__", 0, [getTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return Value::fromDouble(t->dim() > 0 ? static_cast<double>(t->shape[0]) : 0.0);
        });

        // __abs__
        addTensorMethod("__abs__", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            return wrapTensor(tensor_abs(*getTensor(selfVal)));
        });

        // ====================================================================
        // 4. 实例方法（通过 Tensor.method_name 调用，第一个参数是 self）
        // ====================================================================

        // .item() — 提取标量
        addTensorMethod("item", 0, [getTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return Value::fromDouble(t->item());
        });

        // .shape() — 返回形状列表
        addTensorMethod("shape", 0, [getTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            auto list = GcHeap::get().allocate<ObjList>();
            for (int s : t->shape) list->vec.push_back(Value::fromDouble(static_cast<double>(s)));
            return Value(list);
        });

        // .dim() — 返回维度数
        addTensorMethod("dim", 0, [getTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return Value::fromDouble(static_cast<double>(t->dim()));
        });

        // .numel() — 返回元素总数
        addTensorMethod("numel", 0, [getTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return Value::fromDouble(static_cast<double>(t->numel()));
        });

        // .dtype() — 返回类型字符串
        addTensorMethod("dtype", 0, [getTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return Value(dtypeToString(t->dtype()));
        });

        // .to(dtype) — dtype 转换
        addTensorMethod("to", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            if (!args[0].isString()) throw std::runtime_error("TypeError: to(dtype) expects a dtype string.");
            return wrapTensor(t->to(stringToDType(args[0].asString())));
        });

        // .clone()
        addTensorMethod("clone", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(t->clone());
        });

        // .contiguous()
        addTensorMethod("contiguous", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(t->contiguous());
        });

        // .view(shape_list)
        addTensorMethod("view", 1, [getTensor, wrapTensor, listToShape](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            auto shape = listToShape(args[0]);
            return wrapTensor(t->view(shape));
        });

        // .reshape(shape_list)
        addTensorMethod("reshape", 1, [getTensor, wrapTensor, listToShape](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            auto shape = listToShape(args[0]);
            return wrapTensor(t->contiguous().view(shape));
        });

        // .T() — transpose last two dims
        addTensorMethod("T", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(t->T());
        });

        // .transpose(dim0, dim1)
        addTensorMethod("transpose", 2, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            int d0 = static_cast<int>(args[0].asDouble());
            int d1 = static_cast<int>(args[1].asDouble());
            return wrapTensor(t->transpose(d0, d1));
        });

        // .unsqueeze(dim)
        addTensorMethod("unsqueeze", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(t->unsqueeze(static_cast<int>(args[0].asDouble())));
        });

        // .squeeze([dim])
        addTensorMethod("squeeze", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(t->squeeze());
        });

        // .fill_(val)
        addTensorMethod("fill_", 1, [getTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            t->fill_(args[0].asDouble());
            return selfVal;
        });

        // .sum()
        addTensorMethod("sum", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(tensor_sum(*t));
        });

        // .mean()
        addTensorMethod("mean", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(tensor_mean(*t));
        });

        // .max()
        addTensorMethod("max", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(tensor_max(*t));
        });

        // .min()
        addTensorMethod("min", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(tensor_min(*t));
        });

        // .exp()
        addTensorMethod("exp", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(tensor_exp(*t));
        });

        // .log()
        addTensorMethod("log", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(tensor_log(*t));
        });

        // .sqrt()
        addTensorMethod("sqrt", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(tensor_sqrt(*t));
        });

        // .relu()
        addTensorMethod("relu", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(tensor_relu(*t));
        });

        // .sigmoid()
        addTensorMethod("sigmoid", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(tensor_sigmoid(*t));
        });

        // .tanh()
        addTensorMethod("tanh", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(tensor_tanh(*t));
        });

        // .softmax([axis])
        addTensorMethod("softmax", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(tensor_softmax(*t, -1));
        });

        // .matmul(other)
        addTensorMethod("matmul", 1, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return wrapTensor(tensor_matmul(*t, *getTensor(args[0])));
        });

        // .backward()
        addTensorMethod("backward", 0, [getTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            t->backward();
            return Value::none();
        });

        // .grad — 作为方法调用返回梯度张量
        addTensorMethod("grad", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            if (!t->grad) return Value::none();
            return wrapTensor(*t->grad);
        });

        // .requires_grad — 返回 bool
        addTensorMethod("requires_grad", 0, [getTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            return Value(t->requires_grad ? true : false);
        });

        // .detach() — 返回不跟踪梯度的副本
        addTensorMethod("detach", 0, [getTensor, wrapTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto orig = getTensor(selfVal);
            Tensor t = orig->clone();
            t.requires_grad = false;
            t.grad_fn = nullptr;
            t.grad = nullptr;
            t.is_leaf = true;
            return wrapTensor(t);
        });

        // .zero_grad()
        addTensorMethod("zero_grad", 0, [getTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            tensor_zero_grad(*t);
            return selfVal;
        });

        // .tolist() — 转换为 JC2 list
        addTensorMethod("tolist", 0, [getTensor](const std::vector<Value>&) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            auto list = GcHeap::get().allocate<ObjList>();
            for (size_t i = 0; i < t->numel(); ++i) {
                list->vec.push_back(Value::fromDouble(t->getFlat(i)));
            }
            return Value(list);
        });

        // .getFlat(idx) — 按线性索引读取
        addTensorMethod("getFlat", 1, [getTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            size_t idx = static_cast<size_t>(args[0].asDouble());
            return Value::fromDouble(t->getFlat(idx));
        });

        // .setFlat(idx, val) — 按线性索引写入
        addTensorMethod("setFlat", 2, [getTensor](const std::vector<Value>& args) -> Value {
            auto selfVal = jc::helpers::getGlobalCallback("self");
            auto t = getTensor(selfVal);
            size_t idx = static_cast<size_t>(args[0].asDouble());
            t->setFlat(idx, args[1].asDouble());
            return selfVal;
        });

        // ====================================================================
        // 5. 模块级全局工厂函数 (tensor.xxx)
        // ====================================================================
        ModuleReg reg(env, builtins, arity);

        // tensor.tensor(data_list, shape_list, [dtype], [requires_grad])
        reg.reg("tensor", {2, 3, 4}, [wrapTensor, listToDoubles, listToShape, parseTensorOptions](const std::vector<Value>& args) -> Value {
            auto data = listToDoubles(args[0]);
            auto shape = listToShape(args[1]);
            auto [dt, req_grad] = parseTensorOptions(args, 2);
            return wrapTensor(tensor_from_data(data, shape, dt, req_grad));
        });

        // tensor.scalar(val, [dtype], [requires_grad])
        reg.reg("scalar", {1, 2, 3}, [wrapTensor, parseTensorOptions](const std::vector<Value>& args) -> Value {
            auto [dt, rg] = parseTensorOptions(args, 1);
            return wrapTensor(tensor_scalar(args[0].asDouble(), dt, rg));
        });

        // tensor.zeros(shape_list, [dtype], [requires_grad])
        reg.reg("zeros", {1, 2, 3}, [wrapTensor, listToShape, parseTensorOptions](const std::vector<Value>& args) -> Value {
            auto shape = listToShape(args[0]);
            auto [dt, rg] = parseTensorOptions(args, 1);
            return wrapTensor(tensor_zeros(shape, dt, rg));
        });

        // tensor.ones(shape_list, [dtype], [requires_grad])
        reg.reg("ones", {1, 2, 3}, [wrapTensor, listToShape, parseTensorOptions](const std::vector<Value>& args) -> Value {
            auto shape = listToShape(args[0]);
            auto [dt, rg] = parseTensorOptions(args, 1);
            return wrapTensor(tensor_ones(shape, dt, rg));
        });

        // tensor.full(shape_list, fill_value, [dtype], [requires_grad])
        reg.reg("full", {2, 3, 4}, [wrapTensor, listToShape, parseTensorOptions](const std::vector<Value>& args) -> Value {
            auto shape = listToShape(args[0]);
            double val = args[1].asDouble();
            auto [dt, rg] = parseTensorOptions(args, 2);
            return wrapTensor(tensor_full(shape, val, dt, rg));
        });

        // tensor.eye(n, [dtype])
        reg.reg("eye", {1, 2}, [wrapTensor, parseTensorOptions](const std::vector<Value>& args) -> Value {
            auto [dt, rg] = parseTensorOptions(args, 1);
            return wrapTensor(tensor_eye(static_cast<int>(args[0].asDouble()), dt));
        });

        // tensor.arange(start, end, [step], [dtype])
        reg.reg("arange", {2, 3, 4}, [wrapTensor](const std::vector<Value>& args) -> Value {
            double start = args[0].asDouble();
            double end = args[1].asDouble();
            double step = 1.0;
            DType dt = DType::Float64;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i].isString()) dt = stringToDType(args[i].asString());
                else step = args[i].asDouble();
            }
            return wrapTensor(tensor_arange(start, end, step, dt));
        });

        // tensor.linspace(start, end, steps, [dtype])
        reg.reg("linspace", {3, 4}, [wrapTensor, parseTensorOptions](const std::vector<Value>& args) -> Value {
            auto [dt, rg] = parseTensorOptions(args, 3);
            return wrapTensor(tensor_linspace(args[0].asDouble(), args[1].asDouble(), static_cast<int>(args[2].asDouble()), dt));
        });

        // tensor.rand(shape_list, [dtype], [requires_grad])
        reg.reg("rand", {1, 2, 3}, [wrapTensor, listToShape, parseTensorOptions](const std::vector<Value>& args) -> Value {
            auto [dt, rg] = parseTensorOptions(args, 1);
            return wrapTensor(tensor_rand(listToShape(args[0]), dt, rg));
        });

        // tensor.randn(shape_list, [dtype], [requires_grad])
        reg.reg("randn", {1, 2, 3}, [wrapTensor, listToShape, parseTensorOptions](const std::vector<Value>& args) -> Value {
            auto [dt, rg] = parseTensorOptions(args, 1);
            return wrapTensor(tensor_randn(listToShape(args[0]), dt, rg));
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

        // tensor.to(t, dtype) — dtype 转换
        reg.reg("to", {2}, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            if (!args[1].isString()) throw std::runtime_error("TypeError: to(tensor, dtype) expects a dtype string.");
            return wrapTensor(getTensor(args[0])->to(stringToDType(args[1].asString())));
        });

        // tensor.from_matrix(matrix, [requires_grad]) — 从矩阵创建张量
        reg.reg("from_matrix", {1, 2}, [wrapTensor](const std::vector<Value>& args) -> Value {
            if (!args[0].isObjType(ObjType::REAL_MATRIX)) 
                throw std::runtime_error("TypeError: from_matrix expects a RealMatrix.");
            const RealMatrix& mat = static_cast<ObjRealMatrix*>(args[0].asObj())->mat;
            bool rg = (args.size() >= 2) ? args[1].truthy() : false;
            return wrapTensor(tensor_from_matrix(mat, rg));
        });

        // tensor.to_matrix(tensor) — 张量转换为矩阵
        reg.reg("to_matrix", {1}, [getTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            RealMatrix mat = tensor_to_matrix(*t);
            return Value(mat);
        });

        // tensor.getrow(t, row) — 获取第 row 行
        reg.reg("getrow", {2}, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            int row = static_cast<int>(args[1].asDouble());
            return wrapTensor(tensor_getrow(*t, row));
        });

        // tensor.getcol(t, col) — 获取第 col 列
        reg.reg("getcol", {2}, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            int col = static_cast<int>(args[1].asDouble());
            return wrapTensor(tensor_getcol(*t, col));
        });

        // tensor.deleterow(t, row) — 删除第 row 行
        reg.reg("deleterow", {2}, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            int row = static_cast<int>(args[1].asDouble());
            return wrapTensor(tensor_deleterow(*t, row));
        });

        // tensor.deletecol(t, col) — 删除第 col 列
        reg.reg("deletecol", {2}, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            int col = static_cast<int>(args[1].asDouble());
            return wrapTensor(tensor_deletecol(*t, col));
        });

        // tensor.swaprows(t, r1, r2) — 交换两行
        reg.reg("swaprows", {3}, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            int r1 = static_cast<int>(args[1].asDouble());
            int r2 = static_cast<int>(args[2].asDouble());
            return wrapTensor(tensor_swaprows(*t, r1, r2));
        });

        // tensor.hstack(list_of_tensors) — 水平拼接
        reg.reg("hstack", {1}, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            if (!args[0].isObjType(ObjType::LIST)) 
                throw std::runtime_error("TypeError: hstack expects a list of tensors.");
            auto list = static_cast<ObjList*>(args[0].asObj());
            std::vector<Tensor> tensors;
            for (const auto& v : list->vec) tensors.push_back(*getTensor(v));
            return wrapTensor(tensor_hstack(tensors));
        });

        // tensor.vstack(list_of_tensors) — 垂直拼接
        reg.reg("vstack", {1}, [getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            if (!args[0].isObjType(ObjType::LIST)) 
                throw std::runtime_error("TypeError: vstack expects a list of tensors.");
            auto list = static_cast<ObjList*>(args[0].asObj());
            std::vector<Tensor> tensors;
            for (const auto& v : list->vec) tensors.push_back(*getTensor(v));
            return wrapTensor(tensor_vstack(tensors));
        });
    }

} // namespace jc

#endif // JC2_TENSOR_MODULE_H
