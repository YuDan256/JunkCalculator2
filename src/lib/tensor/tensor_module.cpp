#include "../jc2_extension_cpp.h"
#include "Tensor.h"
#include <memory>

static jc2::Class* g_tensorClass = nullptr;

static jc::Tensor* getTensor(const jc2::Value& val) {
    if (!val.is_instance()) jc2::throw_error("TypeError: Expected a Tensor instance.");
    auto ptr = val.get_native_data<jc::Tensor>();
    if (!ptr) jc2::throw_error("TypeError: Instance is not a Tensor.");
    return ptr;
}

static jc2::Value wrapTensor(const jc::Tensor& t) {
    jc2::Instance inst(*g_tensorClass);
    auto data = new jc::Tensor(t);
    inst.set_native_data(data, [](void* ptr) {
        delete static_cast<jc::Tensor*>(ptr);
    });
    return inst;
}

static bool isTensor(const jc2::Value& val) {
    if (!val.is_instance()) return false;
    return val.get_native_data<jc::Tensor>() != nullptr;
}

static std::vector<int> listToShape(const jc2::Value& val) {
    if (!val.is_list()) jc2::throw_error("TypeError: shape must be a list.");
    jc2::List list(val.get_handle());
    std::vector<int> shape;
    for (size_t i = 0; i < list.size(); ++i) {
        shape.push_back(static_cast<int>(list.get(i).as_double()));
    }
    return shape;
}

static std::vector<double> listToDoubles(const jc2::Value& val) {
    if (!val.is_list()) jc2::throw_error("TypeError: data must be a list.");
    jc2::List list(val.get_handle());
    std::vector<double> data;
    for (size_t i = 0; i < list.size(); ++i) {
        data.push_back(list.get(i).as_double());
    }
    return data;
}

static std::pair<jc::DType, bool> parseTensorOptions(int argc, JC2_ValueHandle* argv, size_t start, jc::DType defaultDt = jc::DType::Float64, bool defaultRequiresGrad = false) {
    jc::DType dt = defaultDt;
    bool req_grad = defaultRequiresGrad;
    for (size_t i = start; i < (size_t)argc; ++i) {
        jc2::Value arg(argv[i]);
        if (arg.is_string()) {
            dt = jc::stringToDType(arg.as_string());
        } else {
            req_grad = arg.as_bool();
        }
    }
    return {dt, req_grad};
}

// Macros for methods
#define METHOD(name) JC2_ValueHandle tensor_##name(JC2_VMContext, [[maybe_unused]] int argc, JC2_ValueHandle* argv, void*)
#define GET_SELF auto t1 = getTensor(jc2::Value(argv[0]))

METHOD(__str__) { GET_SELF; return jc2::Value(t1->toString()).get_handle(); }
METHOD(__add__) {
    GET_SELF; jc2::Value other(argv[1]);
    if (isTensor(other)) return wrapTensor(jc::tensor_add(*t1, *getTensor(other))).get_handle();
    return wrapTensor(jc::tensor_add(*t1, jc::tensor_scalar(other.as_double()))).get_handle();
}
METHOD(__radd__) {
    GET_SELF; jc2::Value other(argv[1]);
    return wrapTensor(jc::tensor_add(jc::tensor_scalar(other.as_double()), *t1)).get_handle();
}
METHOD(__sub__) {
    GET_SELF; jc2::Value other(argv[1]);
    if (isTensor(other)) return wrapTensor(jc::tensor_sub(*t1, *getTensor(other))).get_handle();
    return wrapTensor(jc::tensor_sub(*t1, jc::tensor_scalar(other.as_double()))).get_handle();
}
METHOD(__rsub__) {
    GET_SELF; jc2::Value other(argv[1]);
    return wrapTensor(jc::tensor_sub(jc::tensor_scalar(other.as_double()), *t1)).get_handle();
}
METHOD(__mul__) {
    GET_SELF; jc2::Value other(argv[1]);
    if (isTensor(other)) return wrapTensor(jc::tensor_mul(*t1, *getTensor(other))).get_handle();
    return wrapTensor(jc::tensor_mul(*t1, jc::tensor_scalar(other.as_double()))).get_handle();
}
METHOD(__rmul__) {
    GET_SELF; jc2::Value other(argv[1]);
    return wrapTensor(jc::tensor_mul(jc::tensor_scalar(other.as_double()), *t1)).get_handle();
}
METHOD(__div__) {
    GET_SELF; jc2::Value other(argv[1]);
    if (isTensor(other)) return wrapTensor(jc::tensor_div(*t1, *getTensor(other))).get_handle();
    return wrapTensor(jc::tensor_div(*t1, jc::tensor_scalar(other.as_double()))).get_handle();
}
METHOD(__rdiv__) {
    GET_SELF; jc2::Value other(argv[1]);
    return wrapTensor(jc::tensor_div(jc::tensor_scalar(other.as_double()), *t1)).get_handle();
}
METHOD(__pow__) {
    GET_SELF; return wrapTensor(jc::tensor_pow_scalar(*t1, jc2::Value(argv[1]).as_double())).get_handle();
}
METHOD(__neg__) { GET_SELF; return wrapTensor(jc::tensor_neg(*t1)).get_handle(); }
METHOD(__eq__) {
    GET_SELF; jc2::Value other(argv[1]);
    if (isTensor(other)) return wrapTensor(jc::tensor_eq(*t1, *getTensor(other))).get_handle();
    return wrapTensor(jc::tensor_eq(*t1, jc::tensor_scalar(other.as_double()))).get_handle();
}
METHOD(__neq__) {
    GET_SELF; jc2::Value other(argv[1]);
    if (isTensor(other)) return wrapTensor(jc::tensor_neq(*t1, *getTensor(other))).get_handle();
    return wrapTensor(jc::tensor_neq(*t1, jc::tensor_scalar(other.as_double()))).get_handle();
}
METHOD(__lt__) {
    GET_SELF; jc2::Value other(argv[1]);
    if (isTensor(other)) return wrapTensor(jc::tensor_lt(*t1, *getTensor(other))).get_handle();
    return wrapTensor(jc::tensor_lt(*t1, jc::tensor_scalar(other.as_double()))).get_handle();
}
METHOD(__le__) {
    GET_SELF; jc2::Value other(argv[1]);
    if (isTensor(other)) return wrapTensor(jc::tensor_le(*t1, *getTensor(other))).get_handle();
    return wrapTensor(jc::tensor_le(*t1, jc::tensor_scalar(other.as_double()))).get_handle();
}
METHOD(__gt__) {
    GET_SELF; jc2::Value other(argv[1]);
    if (isTensor(other)) return wrapTensor(jc::tensor_gt(*t1, *getTensor(other))).get_handle();
    return wrapTensor(jc::tensor_gt(*t1, jc::tensor_scalar(other.as_double()))).get_handle();
}
METHOD(__ge__) {
    GET_SELF; jc2::Value other(argv[1]);
    if (isTensor(other)) return wrapTensor(jc::tensor_ge(*t1, *getTensor(other))).get_handle();
    return wrapTensor(jc::tensor_ge(*t1, jc::tensor_scalar(other.as_double()))).get_handle();
}
METHOD(__getitem__) {
    GET_SELF;
    if (argc == 2 && isTensor(jc2::Value(argv[1]))) {
        auto idx_t = getTensor(jc2::Value(argv[1]));
        if (idx_t->dtype() == jc::DType::Bool) {
            return wrapTensor(jc::tensor_mask_get(*t1, *idx_t)).get_handle();
        } else if (idx_t->dtype() == jc::DType::Int32 || idx_t->dtype() == jc::DType::Int64) {
            return wrapTensor(jc::tensor_index_get(*t1, *idx_t)).get_handle();
        } else {
            jc2::throw_error("Tensor Error: Advanced indexing requires a Bool or Integer tensor.");
        }
    }

    jc::Tensor current = *t1;
    int dims_provided = argc - 1;
    if (dims_provided > current.dim()) jc2::throw_error("Tensor Error: Too many indices for tensor.");

    int current_dim = 0;
    for (int i = 0; i < dims_provided; ++i) {
        jc2::Value idx_val(argv[i + 1]);
        if (idx_val.is_slice()) {
            jc2::Slice sl(idx_val.get_handle());
            current = current.slice_dim(current_dim, sl.start(), sl.end(), sl.step());
            current_dim++;
        } else {
            int idx = static_cast<int>(idx_val.as_double());
            current = current.select(current_dim, idx);
        }
    }

    if (current.shape.empty()) {
        return jc2::Value(current.getByAbsIdx(current.offset)).get_handle();
    }
    return wrapTensor(current).get_handle();
}
METHOD(__setitem__) {
    GET_SELF;
    if (argc < 2) jc2::throw_error("Tensor Error: __setitem__ requires at least one index and a value.");
    jc2::Value val(argv[argc - 1]);

    if (argc == 3 && isTensor(jc2::Value(argv[1]))) {
        auto idx_t = getTensor(jc2::Value(argv[1]));
        if (idx_t->dtype() == jc::DType::Bool) {
            if (isTensor(val)) jc::tensor_mask_set(*t1, *idx_t, *getTensor(val));
            else jc::tensor_mask_set(*t1, *idx_t, val.as_double());
        } else if (idx_t->dtype() == jc::DType::Int32 || idx_t->dtype() == jc::DType::Int64) {
            if (isTensor(val)) jc::tensor_index_set(*t1, *idx_t, *getTensor(val));
            else jc::tensor_index_set(*t1, *idx_t, val.as_double());
        } else {
            jc2::throw_error("Tensor Error: Advanced indexing requires a Bool or Integer tensor.");
        }
        return jc2::Value().get_handle();
    }

    int dims_provided = argc - 2;
    jc::Tensor current = *t1;
    if (dims_provided > current.dim()) jc2::throw_error("Tensor Error: Too many indices for tensor.");

    int current_dim = 0;
    for (int i = 0; i < dims_provided; ++i) {
        jc2::Value idx_val(argv[i + 1]);
        if (idx_val.is_slice()) {
            jc2::Slice sl(idx_val.get_handle());
            current = current.slice_dim(current_dim, sl.start(), sl.end(), sl.step());
            current_dim++;
        } else {
            int idx = static_cast<int>(idx_val.as_double());
            current = current.select(current_dim, idx);
        }
    }

    if (current.shape.empty()) {
        current.setByAbsIdx(current.offset, val.as_double());
    } else {
        if (isTensor(val)) {
            auto val_t = getTensor(val);
            if (val_t->numel() == 1) {
                current.fill_(val_t->item());
            } else if (current.shape == val_t->shape) {
                for (size_t i = 0; i < current.numel(); ++i) {
                    current.setFlat(i, val_t->getFlat(i));
                }
            } else {
                jc2::throw_error("Tensor Error: Shape mismatch in __setitem__.");
            }
        } else {
            current.fill_(val.as_double());
        }
    }
    return jc2::Value().get_handle();
}
METHOD(__len__) { GET_SELF; return jc2::Value(t1->dim() > 0 ? static_cast<double>(t1->shape[0]) : 0.0).get_handle(); }
METHOD(__abs__) { GET_SELF; return wrapTensor(jc::tensor_abs(*t1)).get_handle(); }

METHOD(item) { GET_SELF; return jc2::Value(t1->item()).get_handle(); }
METHOD(shape) {
    GET_SELF; jc2::List list;
    for (int s : t1->shape) list.push_back(jc2::Value(static_cast<double>(s)));
    return list.get_handle();
}
METHOD(dim) { GET_SELF; return jc2::Value(static_cast<double>(t1->dim())).get_handle(); }
METHOD(numel) { GET_SELF; return jc2::Value(static_cast<double>(t1->numel())).get_handle(); }
METHOD(dtype) { GET_SELF; return jc2::Value(jc::dtypeToString(t1->dtype())).get_handle(); }
METHOD(to) { GET_SELF; return wrapTensor(t1->to(jc::stringToDType(jc2::Value(argv[1]).as_string()))).get_handle(); }
METHOD(clone) { GET_SELF; return wrapTensor(t1->clone()).get_handle(); }
METHOD(contiguous) { GET_SELF; return wrapTensor(t1->contiguous()).get_handle(); }
METHOD(view) { GET_SELF; return wrapTensor(t1->view(listToShape(jc2::Value(argv[1])))).get_handle(); }
METHOD(reshape) { GET_SELF; return wrapTensor(t1->contiguous().view(listToShape(jc2::Value(argv[1])))).get_handle(); }
METHOD(T) { GET_SELF; return wrapTensor(t1->T()).get_handle(); }
METHOD(transpose) { GET_SELF; return wrapTensor(t1->transpose(static_cast<int>(jc2::Value(argv[1]).as_double()), static_cast<int>(jc2::Value(argv[2]).as_double()))).get_handle(); }
METHOD(unsqueeze) { GET_SELF; return wrapTensor(t1->unsqueeze(static_cast<int>(jc2::Value(argv[1]).as_double()))).get_handle(); }
METHOD(squeeze) { GET_SELF; return wrapTensor(t1->squeeze()).get_handle(); }
METHOD(fill_) { GET_SELF; t1->fill_(jc2::Value(argv[1]).as_double()); return argv[0]; }
METHOD(sum) {
    GET_SELF;
    int axis = -1;
    bool keepdim = false;
    if (argc > 1) axis = static_cast<int>(jc2::Value(argv[1]).as_double());
    if (argc > 2) keepdim = jc2::Value(argv[2]).as_bool();
    return wrapTensor(jc::tensor_sum(*t1, axis, keepdim)).get_handle();
}
METHOD(mean) {
    GET_SELF;
    int axis = -1;
    bool keepdim = false;
    if (argc > 1) axis = static_cast<int>(jc2::Value(argv[1]).as_double());
    if (argc > 2) keepdim = jc2::Value(argv[2]).as_bool();
    return wrapTensor(jc::tensor_mean(*t1, axis, keepdim)).get_handle();
}
METHOD(max) { GET_SELF; return wrapTensor(jc::tensor_max(*t1)).get_handle(); }
METHOD(min) { GET_SELF; return wrapTensor(jc::tensor_min(*t1)).get_handle(); }
METHOD(clamp) { GET_SELF; return wrapTensor(jc::tensor_clamp(*t1, jc2::Value(argv[1]).as_double(), jc2::Value(argv[2]).as_double())).get_handle(); }
METHOD(argmax) { GET_SELF; return jc2::Value(static_cast<double>(jc::tensor_argmax(*t1))).get_handle(); }
METHOD(argmin) { GET_SELF; return jc2::Value(static_cast<double>(jc::tensor_argmin(*t1))).get_handle(); }
METHOD(exp) { GET_SELF; return wrapTensor(jc::tensor_exp(*t1)).get_handle(); }
METHOD(log) { GET_SELF; return wrapTensor(jc::tensor_log(*t1)).get_handle(); }
METHOD(sqrt) { GET_SELF; return wrapTensor(jc::tensor_sqrt(*t1)).get_handle(); }
METHOD(relu) { GET_SELF; return wrapTensor(jc::tensor_relu(*t1)).get_handle(); }
METHOD(sigmoid) { GET_SELF; return wrapTensor(jc::tensor_sigmoid(*t1)).get_handle(); }
METHOD(tanh) { GET_SELF; return wrapTensor(jc::tensor_tanh(*t1)).get_handle(); }
METHOD(softmax) { GET_SELF; return wrapTensor(jc::tensor_softmax(*t1, -1)).get_handle(); }
METHOD(matmul) { GET_SELF; return wrapTensor(jc::tensor_matmul(*t1, *getTensor(jc2::Value(argv[1])))).get_handle(); }
METHOD(backward) { GET_SELF; t1->backward(); return jc2::Value().get_handle(); }
METHOD(grad) { GET_SELF; if (!t1->impl->grad) return jc2::Value().get_handle(); return wrapTensor(*t1->impl->grad).get_handle(); }
METHOD(requires_grad) { GET_SELF; return jc2::Value(t1->impl->requires_grad ? true : false).get_handle(); }
METHOD(detach) {
    GET_SELF; jc::Tensor t = t1->clone();
    t.impl->requires_grad = false; t.impl->grad_fn = nullptr; t.impl->grad = nullptr; t.impl->is_leaf = true;
    return wrapTensor(t).get_handle();
}
METHOD(zero_grad) { GET_SELF; jc::tensor_zero_grad(*t1); return argv[0]; }
METHOD(tolist) {
    GET_SELF; jc2::List list;
    for (size_t i = 0; i < t1->numel(); ++i) list.push_back(jc2::Value(t1->getFlat(i)));
    return list.get_handle();
}
METHOD(getFlat) { GET_SELF; return jc2::Value(t1->getFlat(static_cast<size_t>(jc2::Value(argv[1]).as_double()))).get_handle(); }
METHOD(setFlat) { GET_SELF; t1->setFlat(static_cast<size_t>(jc2::Value(argv[1]).as_double()), jc2::Value(argv[2]).as_double()); return argv[0]; }

// Global functions
#define FUNC(name) JC2_ValueHandle global_##name(JC2_VMContext, [[maybe_unused]] int argc, JC2_ValueHandle* argv, void*)

static void parseNestedList(const jc2::Value& val, std::vector<double>& out_data, std::vector<int>& out_shape, int current_depth) {
    if (val.is_list()) {
        jc2::List list(val.get_handle());
        int size = static_cast<int>(list.size());
        
        if (current_depth == static_cast<int>(out_shape.size())) {
            out_shape.push_back(size);
        } else if (current_depth < static_cast<int>(out_shape.size())) {
            if (out_shape[current_depth] != size) {
                jc2::throw_error("Tensor Error: Inconsistent sequence length in nested list.");
            }
        }
        
        for (size_t i = 0; i < static_cast<size_t>(size); ++i) {
            parseNestedList(list.get(i), out_data, out_shape, current_depth + 1);
        }
    } else {
        if (current_depth < static_cast<int>(out_shape.size())) {
            jc2::throw_error("Tensor Error: Jagged nested list detected.");
        }
        out_data.push_back(val.as_double());
    }
}

FUNC(tensor) {
    if (argc < 1) jc2::throw_error("TypeError: Tensor() takes at least 1 argument.");
    jc2::Value arg0(argv[0]);
    if (!arg0.is_list()) jc2::throw_error("TypeError: data must be a list.");

    std::vector<double> data;
    std::vector<int> shape;
    jc::DType dt = jc::DType::Float64;
    bool rg = false;

    jc2::Value shape_val = (argc >= 2) ? jc2::Value(argv[1]) : jc2::Value::none();
    
    if (shape_val.is_list()) {
        data = listToDoubles(arg0);
        shape = listToShape(shape_val);
    } else if (shape_val.is_none()) {
        parseNestedList(arg0, data, shape, 0);
        size_t expected_numel = shape.empty() ? 0 : 1;
        for (int s : shape) expected_numel *= s;
        if (data.size() != expected_numel) {
            jc2::throw_error("Tensor Error: Jagged nested list detected.");
        }
    } else {
        jc2::throw_error("TypeError: shape must be a list or none.");
    }

    if (argc >= 3) {
        jc2::Value dt_val(argv[2]);
        if (!dt_val.is_none()) {
            if (!dt_val.is_string()) jc2::throw_error("TypeError: dtype must be a string.");
            dt = jc::stringToDType(dt_val.as_string());
        }
    }
    if (argc >= 4) {
        jc2::Value rg_val(argv[3]);
        if (!rg_val.is_none()) {
            rg = rg_val.as_bool();
        }
    }

    return wrapTensor(jc::tensor_from_data(data, shape, dt, rg)).get_handle();
}
FUNC(scalar) {
    auto [dt, rg] = parseTensorOptions(argc, argv, 1);
    return wrapTensor(jc::tensor_scalar(jc2::Value(argv[0]).as_double(), dt, rg)).get_handle();
}
FUNC(zeros) {
    auto shape = listToShape(jc2::Value(argv[0]));
    auto [dt, rg] = parseTensorOptions(argc, argv, 1);
    return wrapTensor(jc::tensor_zeros(shape, dt, rg)).get_handle();
}
FUNC(ones) {
    auto shape = listToShape(jc2::Value(argv[0]));
    auto [dt, rg] = parseTensorOptions(argc, argv, 1);
    return wrapTensor(jc::tensor_ones(shape, dt, rg)).get_handle();
}
FUNC(full) {
    auto shape = listToShape(jc2::Value(argv[0]));
    double val = jc2::Value(argv[1]).as_double();
    auto [dt, rg] = parseTensorOptions(argc, argv, 2);
    return wrapTensor(jc::tensor_full(shape, val, dt, rg)).get_handle();
}
FUNC(eye) {
    auto [dt, rg] = parseTensorOptions(argc, argv, 1);
    return wrapTensor(jc::tensor_eye(static_cast<int>(jc2::Value(argv[0]).as_double()), dt)).get_handle();
}
FUNC(arange) {
    double start = jc2::Value(argv[0]).as_double();
    double end = jc2::Value(argv[1]).as_double();
    double step = 1.0;
    jc::DType dt = jc::DType::Float64;
    for (int i = 2; i < argc; ++i) {
        jc2::Value arg(argv[i]);
        if (arg.is_string()) dt = jc::stringToDType(arg.as_string());
        else step = arg.as_double();
    }
    return wrapTensor(jc::tensor_arange(start, end, step, dt)).get_handle();
}
FUNC(linspace) {
    auto [dt, rg] = parseTensorOptions(argc, argv, 3);
    return wrapTensor(jc::tensor_linspace(jc2::Value(argv[0]).as_double(), jc2::Value(argv[1]).as_double(), static_cast<int>(jc2::Value(argv[2]).as_double()), dt)).get_handle();
}
FUNC(rand) {
    auto [dt, rg] = parseTensorOptions(argc, argv, 1);
    return wrapTensor(jc::tensor_rand(listToShape(jc2::Value(argv[0])), dt, rg)).get_handle();
}
FUNC(randn) {
    auto [dt, rg] = parseTensorOptions(argc, argv, 1);
    return wrapTensor(jc::tensor_randn(listToShape(jc2::Value(argv[0])), dt, rg)).get_handle();
}
FUNC(matmul) { (void)argc; return wrapTensor(jc::tensor_matmul(*getTensor(jc2::Value(argv[0])), *getTensor(jc2::Value(argv[1])))).get_handle(); }
FUNC(cat) {
    jc2::Value listVal(argv[0]);
    if (!listVal.is_list()) jc2::throw_error("TypeError: cat expects a list of tensors.");
    jc2::List list(listVal.get_handle());
    std::vector<jc::Tensor> tensors;
    for (size_t i = 0; i < list.size(); ++i) tensors.push_back(*getTensor(list.get(i)));
    int axis = (argc >= 2) ? static_cast<int>(jc2::Value(argv[1]).as_double()) : 0;
    return wrapTensor(jc::tensor_cat(tensors, axis)).get_handle();
}
FUNC(stack) {
    jc2::Value listVal(argv[0]);
    if (!listVal.is_list()) jc2::throw_error("TypeError: stack expects a list of tensors.");
    jc2::List list(listVal.get_handle());
    std::vector<jc::Tensor> tensors;
    for (size_t i = 0; i < list.size(); ++i) tensors.push_back(*getTensor(list.get(i)));
    int axis = (argc >= 2) ? static_cast<int>(jc2::Value(argv[1]).as_double()) : 0;
    return wrapTensor(jc::tensor_stack(tensors, axis)).get_handle();
}
FUNC(mse_loss) { (void)argc; return wrapTensor(jc::tensor_mse_loss(*getTensor(jc2::Value(argv[0])), *getTensor(jc2::Value(argv[1])))).get_handle(); }
FUNC(softmax) {
    int axis = (argc >= 2) ? static_cast<int>(jc2::Value(argv[1]).as_double()) : -1;
    return wrapTensor(jc::tensor_softmax(*getTensor(jc2::Value(argv[0])), axis)).get_handle();
}
FUNC(backward) { (void)argc; getTensor(jc2::Value(argv[0]))->backward(); return jc2::Value().get_handle(); }
FUNC(zero_grad) { (void)argc; jc::tensor_zero_grad(*getTensor(jc2::Value(argv[0]))); return jc2::Value().get_handle(); }
FUNC(sgd_step) { (void)argc; jc::tensor_sgd_step(*getTensor(jc2::Value(argv[0])), jc2::Value(argv[1]).as_double()); return jc2::Value().get_handle(); }
FUNC(isTensor) { (void)argc; return jc2::Value(isTensor(jc2::Value(argv[0]))).get_handle(); }
FUNC(to) { (void)argc; return wrapTensor(getTensor(jc2::Value(argv[0]))->to(jc::stringToDType(jc2::Value(argv[1]).as_string()))).get_handle(); }
FUNC(getrow) { (void)argc; return wrapTensor(jc::tensor_getrow(*getTensor(jc2::Value(argv[0])), static_cast<int>(jc2::Value(argv[1]).as_double()))).get_handle(); }
FUNC(getcol) { (void)argc; return wrapTensor(jc::tensor_getcol(*getTensor(jc2::Value(argv[0])), static_cast<int>(jc2::Value(argv[1]).as_double()))).get_handle(); }
FUNC(deleterow) { (void)argc; return wrapTensor(jc::tensor_deleterow(*getTensor(jc2::Value(argv[0])), static_cast<int>(jc2::Value(argv[1]).as_double()))).get_handle(); }
FUNC(deletecol) { (void)argc; return wrapTensor(jc::tensor_deletecol(*getTensor(jc2::Value(argv[0])), static_cast<int>(jc2::Value(argv[1]).as_double()))).get_handle(); }
FUNC(swaprows) { (void)argc; return wrapTensor(jc::tensor_swaprows(*getTensor(jc2::Value(argv[0])), static_cast<int>(jc2::Value(argv[1]).as_double()), static_cast<int>(jc2::Value(argv[2]).as_double()))).get_handle(); }
FUNC(hstack) {
    (void)argc;
    jc2::Value listVal(argv[0]);
    if (!listVal.is_list()) jc2::throw_error("TypeError: hstack expects a list of tensors.");
    jc2::List list(listVal.get_handle());
    std::vector<jc::Tensor> tensors;
    for (size_t i = 0; i < list.size(); ++i) tensors.push_back(*getTensor(list.get(i)));
    return wrapTensor(jc::tensor_hstack(tensors)).get_handle();
}
FUNC(vstack) {
    (void)argc;
    jc2::Value listVal(argv[0]);
    if (!listVal.is_list()) jc2::throw_error("TypeError: vstack expects a list of tensors.");
    jc2::List list(listVal.get_handle());
    std::vector<jc::Tensor> tensors;
    for (size_t i = 0; i < list.size(); ++i) tensors.push_back(*getTensor(list.get(i)));
    return wrapTensor(jc::tensor_vstack(tensors)).get_handle();
}
FUNC(from_matrix) {
    jc2::Value matVal(argv[0]);
    if (!matVal.is_real_matrix()) jc2::throw_error("TypeError: from_matrix expects a RealMatrix.");
    jc2::RealMatrix mat(matVal.get_handle());
    bool rg = (argc >= 2) ? jc2::Value(argv[1]).as_bool() : false;
    
    int rows = mat.rows();
    int cols = mat.cols();
    jc::Tensor t({rows, cols}, jc::DType::Float64, rg);
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            t.setFlat(i * cols + j, mat.get(i, j));
        }
    }
    return wrapTensor(t).get_handle();
}
FUNC(to_matrix) {
    (void)argc;
    auto t = getTensor(jc2::Value(argv[0]));
    if (t->dim() != 2) jc2::throw_error("Tensor Error: to_matrix requires 2D tensor.");
    int rows = t->shape[0];
    int cols = t->shape[1];
    jc2::RealMatrix mat(rows, cols);
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            mat.set(i, j, t->getFlat(i * cols + j));
        }
    }
    return mat.get_handle();
}
FUNC(no_grad) {
    (void)argc;
    jc2::Value fnVal(argv[0]);
    if (!fnVal.is_function()) jc2::throw_error("TypeError: no_grad expects a function.");
    jc2::Function fn(fnVal.get_handle());
    jc::AutogradGuard guard(false);
    return fn.call({}).get_handle();
}

int jc2_init(jc2::Module& mod) {
    g_tensorClass = new jc2::Class("Tensor");
    mod.register_value("Tensor", *g_tensorClass);

    g_tensorClass->bind_method("__str__", tensor___str__, 0, 0);
    g_tensorClass->bind_method("__add__", tensor___add__, 1, 1, {"other"});
    g_tensorClass->bind_method("__radd__", tensor___radd__, 1, 1, {"other"});
    g_tensorClass->bind_method("__sub__", tensor___sub__, 1, 1, {"other"});
    g_tensorClass->bind_method("__rsub__", tensor___rsub__, 1, 1, {"other"});
    g_tensorClass->bind_method("__mul__", tensor___mul__, 1, 1, {"other"});
    g_tensorClass->bind_method("__rmul__", tensor___rmul__, 1, 1, {"other"});
    g_tensorClass->bind_method("__div__", tensor___div__, 1, 1, {"other"});
    g_tensorClass->bind_method("__rdiv__", tensor___rdiv__, 1, 1, {"other"});
    g_tensorClass->bind_method("__pow__", tensor___pow__, 1, 1, {"exponent"});
    g_tensorClass->bind_method("__neg__", tensor___neg__, 0, 0);
    g_tensorClass->bind_method("__eq__", tensor___eq__, 1, 1, {"other"});
    g_tensorClass->bind_method("__neq__", tensor___neq__, 1, 1, {"other"});
    g_tensorClass->bind_method("__lt__", tensor___lt__, 1, 1, {"other"});
    g_tensorClass->bind_method("__le__", tensor___le__, 1, 1, {"other"});
    g_tensorClass->bind_method("__gt__", tensor___gt__, 1, 1, {"other"});
    g_tensorClass->bind_method("__ge__", tensor___ge__, 1, 1, {"other"});
    g_tensorClass->bind_method("__getitem__", tensor___getitem__, 1, 16777215, {}, "dims");
    g_tensorClass->bind_method("__setitem__", tensor___setitem__, 2, 16777215, {}, "dims_and_val");
    g_tensorClass->bind_method("__len__", tensor___len__, 0, 0);
    g_tensorClass->bind_method("__abs__", tensor___abs__, 0, 0);

    g_tensorClass->bind_method("item", tensor_item, 0, 0);
    g_tensorClass->bind_method("shape", tensor_shape, 0, 0);
    g_tensorClass->bind_method("dim", tensor_dim, 0, 0);
    g_tensorClass->bind_method("numel", tensor_numel, 0, 0);
    g_tensorClass->bind_method("dtype", tensor_dtype, 0, 0);
    g_tensorClass->bind_method("to", tensor_to, 1, 1, {"dtype"});
    g_tensorClass->bind_method("clone", tensor_clone, 0, 0);
    g_tensorClass->bind_method("contiguous", tensor_contiguous, 0, 0);
    g_tensorClass->bind_method("view", tensor_view, 1, 1, {"shape"});
    g_tensorClass->bind_method("reshape", tensor_reshape, 1, 1, {"shape"});
    g_tensorClass->bind_method("T", tensor_T, 0, 0);
    g_tensorClass->bind_method("transpose", tensor_transpose, 2, 2, {"dim0", "dim1"});
    g_tensorClass->bind_method("unsqueeze", tensor_unsqueeze, 1, 1, {"dim"});
    g_tensorClass->bind_method("squeeze", tensor_squeeze, 0, 0);
    g_tensorClass->bind_method("fill_", tensor_fill_, 1, 1, {"val"});
    g_tensorClass->bind_method("sum", tensor_sum, 0, 2, {"axis", "keepdim"});
    g_tensorClass->bind_method("mean", tensor_mean, 0, 2, {"axis", "keepdim"});
    g_tensorClass->bind_method("max", tensor_max, 0, 0);
    g_tensorClass->bind_method("min", tensor_min, 0, 0);
    g_tensorClass->bind_method("clamp", tensor_clamp, 2, 2, {"min", "max"});
    g_tensorClass->bind_method("argmax", tensor_argmax, 0, 0);
    g_tensorClass->bind_method("argmin", tensor_argmin, 0, 0);
    g_tensorClass->bind_method("exp", tensor_exp, 0, 0);
    g_tensorClass->bind_method("log", tensor_log, 0, 0);
    g_tensorClass->bind_method("sqrt", tensor_sqrt, 0, 0);
    g_tensorClass->bind_method("relu", tensor_relu, 0, 0);
    g_tensorClass->bind_method("sigmoid", tensor_sigmoid, 0, 0);
    g_tensorClass->bind_method("tanh", tensor_tanh, 0, 0);
    g_tensorClass->bind_method("softmax", tensor_softmax, 0, 1, {"axis"});
    g_tensorClass->bind_method("matmul", tensor_matmul, 1, 1, {"other"});
    g_tensorClass->bind_method("backward", tensor_backward, 0, 0);
    g_tensorClass->bind_method("grad", tensor_grad, 0, 0);
    g_tensorClass->bind_method("requires_grad", tensor_requires_grad, 0, 0);
    g_tensorClass->bind_method("detach", tensor_detach, 0, 0);
    g_tensorClass->bind_method("zero_grad", tensor_zero_grad, 0, 0);
    g_tensorClass->bind_method("tolist", tensor_tolist, 0, 0);
    g_tensorClass->bind_method("getFlat", tensor_getFlat, 1, 1, {"idx"});
    g_tensorClass->bind_method("setFlat", tensor_setFlat, 2, 2, {"idx", "val"});

    g_tensorClass->set_allocator(global_tensor);

    mod.register_function("tensor", global_tensor, 1, 4, {"data", "shape", "dtype", "requires_grad"});
    mod.register_function("scalar", global_scalar, 1, 3, {"val", "dtype", "requires_grad"});
    mod.register_function("zeros", global_zeros, 1, 3, {"shape", "dtype", "requires_grad"});
    mod.register_function("ones", global_ones, 1, 3, {"shape", "dtype", "requires_grad"});
    mod.register_function("full", global_full, 2, 4, {"shape", "val", "dtype", "requires_grad"});
    mod.register_function("eye", global_eye, 1, 2, {"n", "dtype"});
    mod.register_function("arange", global_arange, 2, 4, {"start", "end", "step", "dtype"});
    mod.register_function("linspace", global_linspace, 3, 4, {"start", "end", "steps", "dtype"});
    mod.register_function("rand", global_rand, 1, 3, {"shape", "dtype", "requires_grad"});
    mod.register_function("randn", global_randn, 1, 3, {"shape", "dtype", "requires_grad"});
    mod.register_function("matmul", global_matmul, 2, 2, {"a", "b"});
    mod.register_function("cat", global_cat, 1, 2, {"tensors", "axis"});
    mod.register_function("stack", global_stack, 1, 2, {"tensors", "axis"});
    mod.register_function("mse_loss", global_mse_loss, 2, 2, {"pred", "target"});
    mod.register_function("softmax", global_softmax, 1, 2, {"t", "axis"});
    mod.register_function("backward", global_backward, 1, 1, {"t"});
    mod.register_function("zero_grad", global_zero_grad, 1, 1, {"t"});
    mod.register_function("sgd_step", global_sgd_step, 2, 2, {"param", "lr"});
    mod.register_function("isTensor", global_isTensor, 1, 1, {"val"});
    mod.register_function("to", global_to, 2, 2, {"t", "dtype"});
    mod.register_function("getrow", global_getrow, 2, 2, {"t", "row"});
    mod.register_function("getcol", global_getcol, 2, 2, {"t", "col"});
    mod.register_function("deleterow", global_deleterow, 2, 2, {"t", "row"});
    mod.register_function("deletecol", global_deletecol, 2, 2, {"t", "col"});
    mod.register_function("swaprows", global_swaprows, 3, 3, {"t", "r1", "r2"});
    mod.register_function("hstack", global_hstack, 1, 1, {"tensors"});
    mod.register_function("vstack", global_vstack, 1, 1, {"tensors"});
    mod.register_function("from_matrix", global_from_matrix, 1, 2, {"mat", "requires_grad"});
    mod.register_function("to_matrix", global_to_matrix, 1, 1, {"t"});
    mod.register_function("no_grad", global_no_grad, 1, 1, {"fn"});

    mod.register_help("tensor",
        "═══ N-Dimensional Tensor Engine with Autograd — Native Module ═══\n\n"
        "  Requires: import tensor\n\n"
        "  The `tensor` module provides a PyTorch-inspired N-dimensional tensor computation\n"
        "  engine with full automatic differentiation (Autograd). It supports arbitrary-rank\n"
        "  tensors, NumPy-style broadcasting, zero-copy views, and a dynamic computation\n"
        "  graph that tracks gradients through all operations.\n\n"
        "  Construction & Factory Functions\n"
        "  ──────────────────────\n"
        "    import tensor\n\n"
        "    tensor.Tensor(data, [shape], [dtype], [requires_grad])\n"
        "    tensor.tensor(...)  // Legacy alias\n"
        "        Create a tensor from a nested list (leave shape as none) or a flat list + shape.\n"
        "        Optional dtype strings: \"float64\", \"float32\", \"int64\", \"int32\", \"bool\"\n"
        "        (aliases: \"f64\", \"f32\", \"i64\", \"i32\", \"double\").\n"
        "        tensor.tensor(@[@[1, 2], @[3, 4]])                 → 2×2 Tensor\n"
        "        tensor.tensor(@[1,2,3,4,5,6], @[2,3])              → 2×3 Tensor\n"
        "        tensor.tensor(@[1,2,3], @[3], \"int64\")             → int64 Tensor\n"
        "        tensor.tensor(@[1,2,3], @[3], true)                 → 1D with grad tracking\n"
        "        tensor.tensor(@[1,2,3], @[3], \"float32\", true)     → float32 with grad\n\n"
        "    tensor.scalar(val, [dtype], [requires_grad])\n"
        "        Create a scalar (1-element) tensor.\n"
        "        tensor.scalar(3.14)                         → Tensor([3.14])\n"
        "        tensor.scalar(1, \"int64\")                   → int64 scalar Tensor\n\n"
        "    tensor.zeros(shape, [dtype], [requires_grad])\n"
        "        Create an all-zeros tensor.\n"
        "        tensor.zeros(@[3, 3])                       → 3×3 zero Tensor\n"
        "        tensor.zeros(@[3, 3], \"float32\")            → float32 zero Tensor\n\n"
        "    tensor.ones(shape, [dtype], [requires_grad])\n"
        "        Create an all-ones tensor.\n"
        "        tensor.ones(@[2, 4])                        → 2×4 ones Tensor\n"
        "        tensor.ones(@[2, 4], \"int32\")               → int32 ones Tensor\n\n"
        "    tensor.full(shape, fill_value, [dtype], [requires_grad])\n"
        "        Create a tensor filled with a constant value.\n"
        "        tensor.full(@[2, 2], 7.0)                   → 2×2 Tensor of 7s\n"
        "        tensor.full(@[2, 2], 7, \"int64\")            → int64 Tensor of 7s\n\n"
        "    tensor.eye(n, [dtype])\n"
        "        Create an n×n identity matrix tensor.\n"
        "        tensor.eye(3)                               → 3×3 identity\n"
        "        tensor.eye(3, \"float32\")                   → float32 identity\n\n"
        "    tensor.arange(start, end, [step], [dtype])\n"
        "        Create a 1D tensor with values in [start, end) with given step.\n"
        "        tensor.arange(0, 10, 2)                     → Tensor([0,2,4,6,8])\n"
        "        tensor.arange(0, 10, 2, \"int64\")           → int64 Tensor([0,2,4,6,8])\n\n"
        "    tensor.linspace(start, end, steps, [dtype])\n"
        "        Create a 1D tensor with `steps` evenly spaced values.\n"
        "        tensor.linspace(0, 1, 5)                    → Tensor([0,.25,.5,.75,1])\n"
        "        tensor.linspace(0, 1, 5, \"float32\")        → float32 Tensor\n\n"
        "    tensor.rand(shape, [dtype], [requires_grad])\n"
        "        Create a tensor with uniform random values in [0, 1).\n"
        "        tensor.rand(@[3, 3])                        → 3×3 random Tensor\n"
        "        tensor.rand(@[3, 3], \"float32\", true)      → float32 random Tensor with grad\n\n"
        "    tensor.randn(shape, [dtype], [requires_grad])\n"
        "        Create a tensor with standard normal (Gaussian) random values.\n"
        "        tensor.randn(@[100])                        → 100 samples from N(0,1)\n"
        "        tensor.randn(@[100], \"float32\")            → float32 Gaussian samples\n\n"
        "  Tensor Properties (Instance Methods)\n"
        "  ──────────────────────\n"
        "    t.shape()             Returns the shape as a list: @[2, 3]\n"
        "    t.dim()               Number of dimensions (rank)\n"
        "    t.numel()             Total number of elements\n"
        "    t.dtype()             Data type string: \"float64\", \"float32\", \"int64\", or \"int32\"\n"
        "    t.to(dtype)           Return a new Tensor converted to dtype\n"
        "    tensor.to(t, dtype)   Module-level dtype conversion\n"
        "    t.item()              Extract the value of a single-element tensor as a number\n"
        "    t.requires_grad()     Returns true if gradient tracking is enabled\n\n"
        "  Arithmetic Operators (Support Broadcasting & Autograd)\n"
        "  ──────────────────────\n"
        "    t1 + t2               Element-wise addition\n"
        "    t1 - t2               Element-wise subtraction\n"
        "    t1 * t2               Element-wise multiplication (Hadamard product)\n"
        "    t1 / t2               Element-wise division\n"
        "    t ^ n                 Element-wise power (scalar exponent)\n"
        "    -t                    Element-wise negation\n"
        "    3.0 * t               Scalar-tensor operations (reverse operators supported)\n\n"
        "    All arithmetic operators fully support NumPy-style broadcasting:\n"
        "      tensor.ones(@[3, 1]) + tensor.ones(@[1, 4])  → 3×4 Tensor\n\n"
        "  Matrix Operations\n"
        "  ──────────────────────\n"
        "    t.matmul(other)                Matrix multiplication (1D / 2D / batched)\n"
        "    tensor.matmul(a, b)            Module-level matrix multiply\n"
        "    t.T()                          Transpose (swap last two dims, zero-copy)\n"
        "    t.transpose(dim0, dim1)        Swap two arbitrary dimensions (zero-copy)\n\n"
        "  Shape Manipulation (Zero-Copy Views)\n"
        "  ──────────────────────\n"
        "    t.view(shape_list)             Reshape (requires contiguous memory)\n"
        "    t.reshape(shape_list)          Reshape (auto-contiguous)\n"
        "    t.unsqueeze(dim)               Insert a size-1 dimension\n"
        "    t.squeeze([dim])               Remove size-1 dimensions\n"
        "    t[i]                           Select along dimension 0 (indexing)\n\n"
        "    tensor.cat(list, [axis])       Concatenate tensors along an axis\n"
        "    tensor.stack(list, [axis])     Stack tensors along a NEW axis\n\n"
        "  Reduction Operations (with Autograd support)\n"
        "  ──────────────────────\n"
        "    t.sum([axis], [keepdim])   Sum all elements, or along an axis → Tensor\n"
        "    t.mean([axis], [keepdim])  Mean of all elements, or along an axis → Tensor\n"
        "    t.max()               Maximum element → scalar Tensor (no grad)\n"
        "    t.min()               Minimum element → scalar Tensor (no grad)\n"
        "    t.clamp(min, max)     Clip elements to [min, max] (with grad)\n"
        "    t.argmax()            Linear index of the maximum element (no grad)\n"
        "    t.argmin()            Linear index of the minimum element (no grad)\n\n"
        "  Unary Math Functions (with Autograd support)\n"
        "  ──────────────────────\n"
        "    t.exp()               Element-wise exponential e^x\n"
        "    t.log()               Element-wise natural logarithm\n"
        "    t.sqrt()              Element-wise square root\n"
        "    abs(t)                Element-wise absolute value\n\n"
        "  Activation Functions (with Autograd support)\n"
        "  ──────────────────────\n"
        "    t.relu()              Rectified Linear Unit: max(0, x)\n"
        "    t.sigmoid()           Logistic sigmoid: 1 / (1 + e^(-x))\n"
        "    t.tanh()              Hyperbolic tangent\n"
        "    t.softmax([axis])     Softmax along axis (default: last dim)\n"
        "    tensor.softmax(t, axis)   Module-level softmax\n\n"
        "  Loss Functions\n"
        "  ──────────────────────\n"
        "    tensor.mse_loss(pred, target)\n"
        "        Mean Squared Error loss. Supports autograd.\n"
        "        loss = tensor.mse_loss(y_pred, y_true)\n"
        "        loss.backward()    // Gradients flow through!\n\n"
        "  Autograd (Automatic Differentiation)\n"
        "  ──────────────────────\n"
        "    JC2's tensor engine builds a dynamic computation graph on every forward\n"
        "    pass. Calling .backward() on a scalar tensor triggers reverse-mode\n"
        "    automatic differentiation, computing gradients for all leaf tensors\n"
        "    that have requires_grad=true.\n\n"
        "    x = tensor.tensor(@[2.0, 3.0], @[2], true)   // Leaf tensor, tracking grad\n"
        "    y = x * x                                      // y = x^2\n"
        "    z = y.sum()                                     // z = sum(x^2) = 13\n"
        "    z.backward()                                    // Compute gradients\n"
        "    x.grad()                                        // → Tensor([4.0, 6.0]) (dz/dx = 2x)\n\n"
        "    Supported autograd operations:\n"
        "      +, -, *, /, ^, neg, matmul, sum, mean,\n"
        "      exp, log, relu, sigmoid, tanh, clamp, mse_loss\n\n"
        "    t.backward()          Trigger reverse-mode AD from this scalar tensor\n"
        "    t.grad()              Access the accumulated gradient tensor (or none)\n"
        "    t.zero_grad()         Reset gradients to zero\n"
        "    t.detach()            Returns a copy detached from the computation graph\n"
        "    tensor.zero_grad(t)   Module-level zero_grad\n\n"
        "  Optimization\n"
        "  ──────────────────────\n"
        "    tensor.sgd_step(param, lr)\n"
        "        Performs an in-place SGD update: param -= lr * param.grad\n"
        "        Typical training loop:\n"
        "          for (epoch in range(100)) {\n"
        "              y_pred = x.matmul(W)   // Forward pass\n"
        "              loss = tensor.mse_loss(y_pred, y_true)\n"
        "              W.zero_grad()           // Clear old gradients\n"
        "              loss.backward()         // Compute new gradients\n"
        "              tensor.sgd_step(W, 0.01)// Update weights\n"
        "          }\n\n"
        "  Utility\n"
        "  ──────────────────────\n"
        "    t.clone()             Deep copy (new storage, detached)\n"
        "    t.to(dtype)           Convert dtype, e.g. t.to(\"float32\")\n"
        "    t.contiguous()        Returns a contiguous tensor (copy if needed)\n"
        "    t.fill_(val)          Fill all elements in-place\n"
        "    t.tolist()            Convert to a flat JC2 list\n"
        "    t.getFlat(idx)        Read element by linear index\n"
        "    t.setFlat(idx, val)   Write element by linear index\n"
        "    tensor.isTensor(val)  Returns true if val is a Tensor instance\n\n"
        "  Matrix Interoperability\n"
        "  ──────────────────────\n"
        "    tensor.from_matrix(matrix, [requires_grad])\n"
        "        Create a Tensor from a JC2 RealMatrix (2D).\n"
        "        A = [1, 2; 3, 4]\n"
        "        t = tensor.from_matrix(A)          → 2×2 Tensor\n"
        "        t = tensor.from_matrix(A, true)    → with gradient tracking\n\n"
        "    tensor.to_matrix(tensor)\n"
        "        Convert a 2D Tensor back to a JC2 RealMatrix.\n"
        "        M = tensor.to_matrix(t)            → [1, 2; 3, 4]\n\n"
        "    This allows seamless bridging between JC2's native matrix algebra\n"
        "    (det, inv, eig, lsolve, etc.) and the tensor autograd engine.\n\n"
        "  Row & Column Operations (2D Tensors)\n"
        "  ──────────────────────\n"
        "    tensor.getrow(t, row)       Extract row as a 1D Tensor\n"
        "    tensor.getcol(t, col)       Extract column as a 1D Tensor\n"
        "    tensor.deleterow(t, row)    Return new Tensor with row removed\n"
        "    tensor.deletecol(t, col)    Return new Tensor with column removed\n"
        "    tensor.swaprows(t, r1, r2)  Return new Tensor with two rows swapped\n\n"
        "    All indices support negative wrapping (e.g., -1 = last row/col).\n\n"
        "  Block Concatenation\n"
        "  ──────────────────────\n"
        "    tensor.hstack(list)         Horizontal concatenation (along columns)\n"
        "    tensor.vstack(list)         Vertical concatenation (along rows)\n\n"
        "    These are convenience wrappers around tensor.cat():\n"
        "      tensor.hstack(@[A, B])    ≡  tensor.cat(@[A, B], 1)\n"
        "      tensor.vstack(@[A, B])    ≡  tensor.cat(@[A, B], 0)\n\n"
        "  Example: Linear Regression\n"
        "  ──────────────────────\n"
        "    import tensor\n\n"
        "    // Training data: y = 2x + 1\n"
        "    x = tensor.tensor(@[1,2,3,4], @[4, 1], \"float64\")\n"
        "    y = tensor.tensor(@[3,5,7,9], @[4, 1], \"float64\")\n\n"
        "    // Parameters (requires_grad!)\n"
        "    W = tensor.randn(@[1, 1], \"float64\"); W = tensor.tensor(@[W.item()], @[1,1], \"float64\", true)\n"
        "    b = tensor.zeros(@[1, 1], \"float64\", true)\n\n"
        "    lr = 0.01\n"
        "    for (epoch in range(100)) {\n"
        "        pred = x.matmul(W) + b\n"
        "        loss = tensor.mse_loss(pred, y)\n"
        "        W.zero_grad(); b.zero_grad()\n"
        "        loss.backward()\n"
        "        tensor.sgd_step(W, lr)\n"
        "        tensor.sgd_step(b, lr)\n"
        "    }\n"
        "    println(f\"W = {W.item()}, b = {b.item()}\")\n"
        "    // → W ≈ 2.0, b ≈ 1.0"
    );

    mod.register_function_help("tensor.Tensor", "tensor.Tensor(data, [shape], [dtype], [requires_grad])", "Creates a Tensor from a nested list (leave shape as none) or a flat list and shape list. Optionally selects dtype and enables gradient tracking.", "tensor.Tensor(@[@[1, 2], @[3, 4]])");
    mod.register_function_help("tensor.tensor", "tensor.tensor(data, [shape], [dtype], [requires_grad])", "Legacy alias for tensor.Tensor.", "tensor.tensor(@[@[1, 2], @[3, 4]])");
    mod.register_function_help("tensor.scalar", "tensor.scalar(val, [dtype], [requires_grad])", "Creates a scalar (1-element) Tensor. Optionally selects dtype and enables gradient tracking.", "tensor.scalar(3.14)");
    mod.register_function_help("tensor.zeros", "tensor.zeros(shape_list, [dtype], [requires_grad])", "Creates an all-zeros Tensor with the given shape. Optionally selects dtype and enables gradient tracking.", "tensor.zeros(@[3, 3])");
    mod.register_function_help("tensor.ones", "tensor.ones(shape_list, [dtype], [requires_grad])", "Creates an all-ones Tensor with the given shape. Optionally selects dtype and enables gradient tracking.", "tensor.ones(@[2, 2])");
    mod.register_function_help("tensor.full", "tensor.full(shape_list, fill_value, [dtype], [requires_grad])", "Creates a Tensor filled with a constant value. Optionally selects dtype and enables gradient tracking.", "tensor.full(@[2, 3], 7.0)");
    mod.register_function_help("tensor.eye", "tensor.eye(n, [dtype])", "Creates an n×n identity matrix Tensor. Optionally selects dtype.", "tensor.eye(3)");
    mod.register_function_help("tensor.arange", "tensor.arange(start, end, [step], [dtype])", "Creates a 1D Tensor with values in [start, end) with the given step. Optionally selects dtype.", "tensor.arange(0, 10, 2)");
    mod.register_function_help("tensor.linspace", "tensor.linspace(start, end, steps, [dtype])", "Creates a 1D Tensor with `steps` evenly-spaced values from start to end. Optionally selects dtype.", "tensor.linspace(0, 1, 5)");
    mod.register_function_help("tensor.rand", "tensor.rand(shape_list, [dtype], [requires_grad])", "Creates a Tensor with uniform random values in [0, 1). Optionally selects dtype and enables gradient tracking.", "tensor.rand(@[3, 3])");
    mod.register_function_help("tensor.randn", "tensor.randn(shape_list, [dtype], [requires_grad])", "Creates a Tensor with standard normal (Gaussian) random values. Optionally selects dtype and enables gradient tracking.", "tensor.randn(@[100])");
    mod.register_function_help("tensor.to", "tensor.to(t, dtype) / t.to(dtype)", "Returns a new Tensor converted to the requested dtype. Supported dtype strings: \"float64\", \"float32\", \"int64\", \"int32\" and aliases \"f64\", \"f32\", \"i64\", \"i32\", \"double\".", "t.to(\"float32\")");
    mod.register_function_help("tensor.matmul", "tensor.matmul(a, b)", "Performs matrix multiplication: 1D (dot product), 2D, or batched (last two dims, batch broadcasting). Supports autograd.", "tensor.matmul(A, B)");
    mod.register_function_help("tensor.cat", "tensor.cat(tensor_list, [axis])", "Concatenates a list of Tensors along the given axis (default 0).", "tensor.cat(@[t1, t2], 0)");
    mod.register_function_help("tensor.stack", "tensor.stack(tensor_list, [axis])", "Stacks a list of Tensors along a new dimension at the given axis.", "tensor.stack(@[t1, t2], 0)");
    mod.register_function_help("tensor.mse_loss", "tensor.mse_loss(pred, target)", "Computes Mean Squared Error loss between predicted and target Tensors. Supports autograd.", "loss = tensor.mse_loss(y_pred, y_true)");
    mod.register_function_help("tensor.softmax", "tensor.softmax(t, [axis])", "Applies softmax normalization along the given axis (default: last dim).", "tensor.softmax(logits, -1)");
    mod.register_function_help("tensor.backward", "tensor.backward(t)", "Triggers reverse-mode automatic differentiation from a scalar Tensor.", "tensor.backward(loss)");
    mod.register_function_help("tensor.zero_grad", "tensor.zero_grad(t)", "Resets the gradient of a Tensor to zero.", "tensor.zero_grad(W)");
    mod.register_function_help("tensor.sgd_step", "tensor.sgd_step(param, lr)", "Performs an in-place SGD weight update: param -= lr * param.grad.", "tensor.sgd_step(W, 0.01)");
    mod.register_function_help("tensor.isTensor", "tensor.isTensor(val)", "Returns true if the value is a Tensor instance, false otherwise.", "tensor.isTensor(t)");
    mod.register_function_help("tensor.from_matrix", "tensor.from_matrix(matrix, [requires_grad])", "Creates a 2D Tensor from a JC2 RealMatrix. Optionally enables gradient tracking.", "tensor.from_matrix([1, 2; 3, 4])");
    mod.register_function_help("tensor.to_matrix", "tensor.to_matrix(tensor)", "Converts a 2D Tensor back to a JC2 RealMatrix for use with native linear algebra functions (det, inv, eig, etc.).", "M = tensor.to_matrix(t)");
    mod.register_function_help("tensor.no_grad", "tensor.no_grad(fn)", "Executes a function without tracking gradients.", "tensor.no_grad(() => { ... })");
    mod.register_function_help("tensor.getrow", "tensor.getrow(t, row)", "Extracts a row from a 2D Tensor, returning it as a 1D Tensor. Supports negative indexing.", "tensor.getrow(t, 0)");
    mod.register_function_help("tensor.getcol", "tensor.getcol(t, col)", "Extracts a column from a 2D Tensor, returning it as a 1D Tensor. Supports negative indexing.", "tensor.getcol(t, 0)");
    mod.register_function_help("tensor.deleterow", "tensor.deleterow(t, row)", "Returns a new 2D Tensor with the specified row removed. Supports negative indexing.", "tensor.deleterow(t, 0)");
    mod.register_function_help("tensor.deletecol", "tensor.deletecol(t, col)", "Returns a new 2D Tensor with the specified column removed. Supports negative indexing.", "tensor.deletecol(t, 1)");
    mod.register_function_help("tensor.swaprows", "tensor.swaprows(t, r1, r2)", "Returns a new 2D Tensor with two rows swapped. Supports negative indexing.", "tensor.swaprows(t, 0, 2)");
    mod.register_function_help("tensor.hstack", "tensor.hstack(tensor_list)", "Horizontally concatenates a list of 2D Tensors along the column axis (axis=1).", "tensor.hstack(@[A, B])");
    mod.register_function_help("tensor.vstack", "tensor.vstack(tensor_list)", "Vertically concatenates a list of 2D Tensors along the row axis (axis=0).", "tensor.vstack(@[A, B])");

    return 0;
}

JC2_EXTENSION_INIT
