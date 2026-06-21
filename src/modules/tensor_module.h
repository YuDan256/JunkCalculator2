#ifndef JC2_TENSOR_MODULE_H
#define JC2_TENSOR_MODULE_H

#include "Module.h"
#include "../math/Tensor.h"
#include "../memory/Value.h"

namespace jc {

    JC2_MODULE(tensor) {
        // 1. 在 JC2 环境中注册 Tensor 类
        auto tensorClass = GcHeap::get().allocate<ObjClass>();
        tensorClass->name = "Tensor";
        env["Tensor"] = Value(tensorClass);

        // 2. 辅助函数：从 JC2 Value 提取 C++ Tensor
        auto getTensor = [](const Value& val) -> std::shared_ptr<Tensor> {
            if (!val.isInstance()) throw std::runtime_error("TypeError: Expected a Tensor instance.");
            auto inst = val.asInstance();
            if (inst->nativeData.type() != typeid(std::shared_ptr<Tensor>)) {
                throw std::runtime_error("TypeError: Instance is not a Tensor.");
            }
            return std::any_cast<std::shared_ptr<Tensor>>(inst->nativeData);
        };

        // 3. 辅助函数：将 C++ Tensor 包装为 JC2 实例
        auto wrapTensor = [tensorClass](const Tensor& t) -> Value {
            auto inst = GcHeap::get().allocate<ObjInstance>();
            inst->classDef = tensorClass;
            inst->nativeData = std::make_shared<Tensor>(t);
            return Value(inst);
        };

        // ====================================================================
        // 绑定 Tensor 类的魔术方法 (Dunder Methods)
        // ====================================================================
        
        // __str__ : 打印 Tensor
        auto str_fn = GcHeap::get().allocate<ObjClosure>(
            std::vector<std::string>{"self"}, std::vector<bool>{false}, "__str__", nullptr
        );
        str_fn->nativeFn = std::make_any<NativeCallable>([getTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            std::string shape_str = "[";
            for (size_t i = 0; i < t->shape.size(); ++i) {
                shape_str += std::to_string(t->shape[i]);
                if (i < t->shape.size() - 1) shape_str += ", ";
            }
            shape_str += "]";
            return Value("Tensor(shape=" + shape_str + ", requires_grad=" + (t->requires_grad ? "true" : "false") + ")");
        });
        tensorClass->methods["__str__"] = str_fn;

        // __add__ : 运算符重载 T1 + T2
        auto add_fn = GcHeap::get().allocate<ObjClosure>(
            std::vector<std::string>{"self", "other"}, std::vector<bool>{false, false}, "__add__", nullptr
        );
        add_fn->nativeFn = std::make_any<NativeCallable>([getTensor, wrapTensor](const std::vector<Value>& args) -> Value {
            auto t1 = getTensor(args[0]);
            auto t2 = getTensor(args[1]);
            Tensor result = tensor_add(*t1, *t2);
            return wrapTensor(result);
        });
        tensorClass->methods["__add__"] = add_fn;

        // ====================================================================
        // 绑定 tensor 模块的全局工厂函数
        // ====================================================================

        // tensor.zeros(shape_list, requires_grad=false)
        ModuleReg reg(env, builtins, arity);
        
        reg.reg("zeros", {1, 2}, [wrapTensor](const std::vector<Value>& args) -> Value {
            if (!args[0].isObjType(ObjType::LIST)) throw std::runtime_error("TypeError: shape must be a list.");
            auto list = static_cast<ObjList*>(args[0].asObj());
            std::vector<int> shape;
            for (const auto& v : list->vec) shape.push_back(static_cast<int>(v.asDouble()));
            
            bool req_grad = false;
            if (args.size() == 2) req_grad = args[1].truthy();

            Tensor t(shape, DType::Float64, req_grad);
            return wrapTensor(t);
        });

        // tensor.backward(t)
        reg.reg("backward", {1}, [getTensor](const std::vector<Value>& args) -> Value {
            auto t = getTensor(args[0]);
            t->backward();
            return Value::none();
        });
    }

} // namespace jc

#endif // JC2_TENSOR_MODULE_H
