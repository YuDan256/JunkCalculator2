#include "jc2_extension_cpp.h"
#include "../math/Tensor.h"
#include <memory>

static jc2::Class* g_tensorClass = nullptr;

static std::shared_ptr<jc::Tensor> getTensor(const jc2::Value& val) {
    if (!val.is_instance()) jc2::throw_error("TypeError: Expected a Tensor instance.");
    auto ptr = val.get_native_data<std::shared_ptr<jc::Tensor>>();
    if (!ptr) jc2::throw_error("TypeError: Instance is not a Tensor.");
    return *ptr;
}

static jc2::Value wrapTensor(const jc::Tensor& t) {
    jc2::Instance inst(*g_tensorClass);
    auto data = new std::shared_ptr<jc::Tensor>(std::make_shared<jc::Tensor>(t));
    inst.set_native_data(data, [](void* ptr) {
        delete static_cast<std::shared_ptr<jc::Tensor>*>(ptr);
    });
    return inst;
}

static bool isTensor(const jc2::Value& val) {
    if (!val.is_instance()) return false;
    return val.get_native_data<std::shared_ptr<jc::Tensor>>() != nullptr;
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
#define METHOD(name) JC2_ValueHandle tensor_##name(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data)
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
    if (!isTensor(other)) return jc2::Value(false).get_handle();
    auto t2 = getTensor(other);
    if (t1->shape != t2->shape) return jc2::Value(false).get_handle();
    for (size_t i = 0; i < t1->numel(); ++i) {
        if (t1->getFlat(i) != t2->getFlat(i)) return jc2::Value(false).get_handle();
    }
    return jc2::Value(true).get_handle();
}
METHOD(__getitem__) {
    GET_SELF; int idx = static_cast<int>(jc2::Value(argv[1]).as_double());
    if (t1->dim() == 1) return jc2::Value(t1->getFlat(idx < 0 ? idx + t1->shape[0] : idx)).get_handle();
    return wrapTensor(t1->select(0, idx)).get_handle();
}
METHOD(__setitem__) {
    GET_SELF; int idx = static_cast<int>(jc2::Value(argv[1]).as_double());
    if (idx < 0) idx += t1->shape[0];
    if (t1->dim() == 1) t1->setFlat(idx, jc2::Value(argv[2]).as_double());
    else jc2::throw_error("Tensor Error: __setitem__ only supports 1D scalar assignment.");
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
METHOD(sum) { GET_SELF; return wrapTensor(jc::tensor_sum(*t1)).get_handle(); }
METHOD(mean) { GET_SELF; return wrapTensor(jc::tensor_mean(*t1)).get_handle(); }
METHOD(max) { GET_SELF; return wrapTensor(jc::tensor_max(*t1)).get_handle(); }
METHOD(min) { GET_SELF; return wrapTensor(jc::tensor_min(*t1)).get_handle(); }
METHOD(exp) { GET_SELF; return wrapTensor(jc::tensor_exp(*t1)).get_handle(); }
METHOD(log) { GET_SELF; return wrapTensor(jc::tensor_log(*t1)).get_handle(); }
METHOD(sqrt) { GET_SELF; return wrapTensor(jc::tensor_sqrt(*t1)).get_handle(); }
METHOD(relu) { GET_SELF; return wrapTensor(jc::tensor_relu(*t1)).get_handle(); }
METHOD(sigmoid) { GET_SELF; return wrapTensor(jc::tensor_sigmoid(*t1)).get_handle(); }
METHOD(tanh) { GET_SELF; return wrapTensor(jc::tensor_tanh(*t1)).get_handle(); }
METHOD(softmax) { GET_SELF; return wrapTensor(jc::tensor_softmax(*t1, -1)).get_handle(); }
METHOD(matmul) { GET_SELF; return wrapTensor(jc::tensor_matmul(*t1, *getTensor(jc2::Value(argv[1])))).get_handle(); }
METHOD(backward) { GET_SELF; t1->backward(); return jc2::Value().get_handle(); }
METHOD(grad) { GET_SELF; if (!t1->grad) return jc2::Value().get_handle(); return wrapTensor(*t1->grad).get_handle(); }
METHOD(requires_grad) { GET_SELF; return jc2::Value(t1->requires_grad ? true : false).get_handle(); }
METHOD(detach) {
    GET_SELF; jc::Tensor t = t1->clone();
    t.requires_grad = false; t.grad_fn = nullptr; t.grad = nullptr; t.is_leaf = true;
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
#define FUNC(name) JC2_ValueHandle global_##name(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data)

FUNC(tensor) {
    auto data = listToDoubles(jc2::Value(argv[0]));
    auto shape = listToShape(jc2::Value(argv[1]));
    auto [dt, rg] = parseTensorOptions(argc, argv, 2);
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
FUNC(matmul) { return wrapTensor(jc::tensor_matmul(*getTensor(jc2::Value(argv[0])), *getTensor(jc2::Value(argv[1])))).get_handle(); }
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
FUNC(mse_loss) { return wrapTensor(jc::tensor_mse_loss(*getTensor(jc2::Value(argv[0])), *getTensor(jc2::Value(argv[1])))).get_handle(); }
FUNC(softmax) {
    int axis = (argc >= 2) ? static_cast<int>(jc2::Value(argv[1]).as_double()) : -1;
    return wrapTensor(jc::tensor_softmax(*getTensor(jc2::Value(argv[0])), axis)).get_handle();
}
FUNC(backward) { getTensor(jc2::Value(argv[0]))->backward(); return jc2::Value().get_handle(); }
FUNC(zero_grad) { jc::tensor_zero_grad(*getTensor(jc2::Value(argv[0]))); return jc2::Value().get_handle(); }
FUNC(sgd_step) { jc::tensor_sgd_step(*getTensor(jc2::Value(argv[0])), jc2::Value(argv[1]).as_double()); return jc2::Value().get_handle(); }
FUNC(isTensor) { return jc2::Value(isTensor(jc2::Value(argv[0]))).get_handle(); }
FUNC(to) { return wrapTensor(getTensor(jc2::Value(argv[0]))->to(jc::stringToDType(jc2::Value(argv[1]).as_string()))).get_handle(); }
FUNC(getrow) { return wrapTensor(jc::tensor_getrow(*getTensor(jc2::Value(argv[0])), static_cast<int>(jc2::Value(argv[1]).as_double()))).get_handle(); }
FUNC(getcol) { return wrapTensor(jc::tensor_getcol(*getTensor(jc2::Value(argv[0])), static_cast<int>(jc2::Value(argv[1]).as_double()))).get_handle(); }
FUNC(deleterow) { return wrapTensor(jc::tensor_deleterow(*getTensor(jc2::Value(argv[0])), static_cast<int>(jc2::Value(argv[1]).as_double()))).get_handle(); }
FUNC(deletecol) { return wrapTensor(jc::tensor_deletecol(*getTensor(jc2::Value(argv[0])), static_cast<int>(jc2::Value(argv[1]).as_double()))).get_handle(); }
FUNC(swaprows) { return wrapTensor(jc::tensor_swaprows(*getTensor(jc2::Value(argv[0])), static_cast<int>(jc2::Value(argv[1]).as_double()), static_cast<int>(jc2::Value(argv[2]).as_double()))).get_handle(); }
FUNC(hstack) {
    jc2::Value listVal(argv[0]);
    if (!listVal.is_list()) jc2::throw_error("TypeError: hstack expects a list of tensors.");
    jc2::List list(listVal.get_handle());
    std::vector<jc::Tensor> tensors;
    for (size_t i = 0; i < list.size(); ++i) tensors.push_back(*getTensor(list.get(i)));
    return wrapTensor(jc::tensor_hstack(tensors)).get_handle();
}
FUNC(vstack) {
    jc2::Value listVal(argv[0]);
    if (!listVal.is_list()) jc2::throw_error("TypeError: vstack expects a list of tensors.");
    jc2::List list(listVal.get_handle());
    std::vector<jc::Tensor> tensors;
    for (size_t i = 0; i < list.size(); ++i) tensors.push_back(*getTensor(list.get(i)));
    return wrapTensor(jc::tensor_vstack(tensors)).get_handle();
}

int jc2_init(jc2::Module& mod) {
    g_tensorClass = new jc2::Class("Tensor");
    mod.register_value("Tensor", *g_tensorClass);

    g_tensorClass->bind_method("__str__", tensor___str__, 0, 0, false);
    g_tensorClass->bind_method("__add__", tensor___add__, 1, 1, false);
    g_tensorClass->bind_method("__radd__", tensor___radd__, 1, 1, false);
    g_tensorClass->bind_method("__sub__", tensor___sub__, 1, 1, false);
    g_tensorClass->bind_method("__rsub__", tensor___rsub__, 1, 1, false);
    g_tensorClass->bind_method("__mul__", tensor___mul__, 1, 1, false);
    g_tensorClass->bind_method("__rmul__", tensor___rmul__, 1, 1, false);
    g_tensorClass->bind_method("__div__", tensor___div__, 1, 1, false);
    g_tensorClass->bind_method("__rdiv__", tensor___rdiv__, 1, 1, false);
    g_tensorClass->bind_method("__pow__", tensor___pow__, 1, 1, false);
    g_tensorClass->bind_method("__neg__", tensor___neg__, 0, 0, false);
    g_tensorClass->bind_method("__eq__", tensor___eq__, 1, 1, false);
    g_tensorClass->bind_method("__getitem__", tensor___getitem__, 1, 1, false);
    g_tensorClass->bind_method("__setitem__", tensor___setitem__, 2, 2, false);
    g_tensorClass->bind_method("__len__", tensor___len__, 0, 0, false);
    g_tensorClass->bind_method("__abs__", tensor___abs__, 0, 0, false);

    g_tensorClass->bind_method("item", tensor_item, 0, 0, false);
    g_tensorClass->bind_method("shape", tensor_shape, 0, 0, false);
    g_tensorClass->bind_method("dim", tensor_dim, 0, 0, false);
    g_tensorClass->bind_method("numel", tensor_numel, 0, 0, false);
    g_tensorClass->bind_method("dtype", tensor_dtype, 0, 0, false);
    g_tensorClass->bind_method("to", tensor_to, 1, 1, false);
    g_tensorClass->bind_method("clone", tensor_clone, 0, 0, false);
    g_tensorClass->bind_method("contiguous", tensor_contiguous, 0, 0, false);
    g_tensorClass->bind_method("view", tensor_view, 1, 1, false);
    g_tensorClass->bind_method("reshape", tensor_reshape, 1, 1, false);
    g_tensorClass->bind_method("T", tensor_T, 0, 0, false);
    g_tensorClass->bind_method("transpose", tensor_transpose, 2, 2, false);
    g_tensorClass->bind_method("unsqueeze", tensor_unsqueeze, 1, 1, false);
    g_tensorClass->bind_method("squeeze", tensor_squeeze, 0, 0, false);
    g_tensorClass->bind_method("fill_", tensor_fill_, 1, 1, false);
    g_tensorClass->bind_method("sum", tensor_sum, 0, 0, false);
    g_tensorClass->bind_method("mean", tensor_mean, 0, 0, false);
    g_tensorClass->bind_method("max", tensor_max, 0, 0, false);
    g_tensorClass->bind_method("min", tensor_min, 0, 0, false);
    g_tensorClass->bind_method("exp", tensor_exp, 0, 0, false);
    g_tensorClass->bind_method("log", tensor_log, 0, 0, false);
    g_tensorClass->bind_method("sqrt", tensor_sqrt, 0, 0, false);
    g_tensorClass->bind_method("relu", tensor_relu, 0, 0, false);
    g_tensorClass->bind_method("sigmoid", tensor_sigmoid, 0, 0, false);
    g_tensorClass->bind_method("tanh", tensor_tanh, 0, 0, false);
    g_tensorClass->bind_method("softmax", tensor_softmax, 0, 1, false);
    g_tensorClass->bind_method("matmul", tensor_matmul, 1, 1, false);
    g_tensorClass->bind_method("backward", tensor_backward, 0, 0, false);
    g_tensorClass->bind_method("grad", tensor_grad, 0, 0, false);
    g_tensorClass->bind_method("requires_grad", tensor_requires_grad, 0, 0, false);
    g_tensorClass->bind_method("detach", tensor_detach, 0, 0, false);
    g_tensorClass->bind_method("zero_grad", tensor_zero_grad, 0, 0, false);
    g_tensorClass->bind_method("tolist", tensor_tolist, 0, 0, false);
    g_tensorClass->bind_method("getFlat", tensor_getFlat, 1, 1, false);
    g_tensorClass->bind_method("setFlat", tensor_setFlat, 2, 2, false);

    mod.register_function("tensor", global_tensor, 2, 4, false);
    mod.register_function("scalar", global_scalar, 1, 3, false);
    mod.register_function("zeros", global_zeros, 1, 3, false);
    mod.register_function("ones", global_ones, 1, 3, false);
    mod.register_function("full", global_full, 2, 4, false);
    mod.register_function("eye", global_eye, 1, 2, false);
    mod.register_function("arange", global_arange, 2, 4, false);
    mod.register_function("linspace", global_linspace, 3, 4, false);
    mod.register_function("rand", global_rand, 1, 3, false);
    mod.register_function("randn", global_randn, 1, 3, false);
    mod.register_function("matmul", global_matmul, 2, 2, false);
    mod.register_function("cat", global_cat, 1, 2, false);
    mod.register_function("stack", global_stack, 1, 2, false);
    mod.register_function("mse_loss", global_mse_loss, 2, 2, false);
    mod.register_function("softmax", global_softmax, 1, 2, false);
    mod.register_function("backward", global_backward, 1, 1, false);
    mod.register_function("zero_grad", global_zero_grad, 1, 1, false);
    mod.register_function("sgd_step", global_sgd_step, 2, 2, false);
    mod.register_function("isTensor", global_isTensor, 1, 1, false);
    mod.register_function("to", global_to, 2, 2, false);
    mod.register_function("getrow", global_getrow, 2, 2, false);
    mod.register_function("getcol", global_getcol, 2, 2, false);
    mod.register_function("deleterow", global_deleterow, 2, 2, false);
    mod.register_function("deletecol", global_deletecol, 2, 2, false);
    mod.register_function("swaprows", global_swaprows, 3, 3, false);
    mod.register_function("hstack", global_hstack, 1, 1, false);
    mod.register_function("vstack", global_vstack, 1, 1, false);

    return 0;
}

JC2_EXTENSION_INIT
