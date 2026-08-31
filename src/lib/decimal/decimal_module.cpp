#include "../jc2_extension_cpp.h"
#include "Decimal.h"
#include <string>
#include <memory>
#include <algorithm>
#include <cmath>
#include <complex>

using jc::Decimal;
using jc::DecInt;

static jc2::Class* g_decimalClass = nullptr;



static std::shared_ptr<Decimal> getDecimal(const jc2::Value& val) {
    if (!val.is_instance()) jc2::throw_error("TypeError: Expected a Decimal instance.");
    auto ptr = val.get_native_data<std::shared_ptr<Decimal>>();
    if (!ptr) jc2::throw_error("TypeError: Instance is not a Decimal.");
    return *ptr;
}

static jc2::Value wrapDecimal(const Decimal& d) {
    jc2::Instance inst(*g_decimalClass);
    auto data = new std::shared_ptr<Decimal>(std::make_shared<Decimal>(d));
    inst.set_native_data(data, [](void* ptr) {
        delete static_cast<std::shared_ptr<Decimal>*>(ptr);
    });
    inst.freeze();
    return inst;
}

static bool canConvertToDecimal(const jc2::Value& val) {
    if (val.is_instance() && val.get_native_data<std::shared_ptr<Decimal>>()) return true;
    if (val.is_string() || val.is_int() || val.is_double()) return true;
    return false;
}

static Decimal parseDecimalArg(const jc2::Value& val) {
    if (val.is_instance() && val.get_native_data<std::shared_ptr<Decimal>>()) {
        return *(*val.get_native_data<std::shared_ptr<Decimal>>());
    }
    if (val.is_string()) {
        return Decimal::from_string(val.as_string());
    }
    if (val.is_int() || val.is_double()) {
        return Decimal::from_string(val.to_string());
    }
    jc2::throw_error("TypeError: Cannot convert to Decimal.");
}

#define METHOD(name) JC2_ValueHandle decimal_##name(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*)
#define GET_SELF (void)argc; auto d1 = getDecimal(jc2::Value(argv[0]))

METHOD(__str__) { GET_SELF; return jc2::Value(d1->to_string()).get_handle(); }
METHOD(__add__) { GET_SELF; return wrapDecimal(d1->add(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__radd__) { GET_SELF; return wrapDecimal(parseDecimalArg(jc2::Value(argv[1])).add(*d1)).get_handle(); }
METHOD(__sub__) { GET_SELF; return wrapDecimal(d1->sub(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__rsub__) { GET_SELF; return wrapDecimal(parseDecimalArg(jc2::Value(argv[1])).sub(*d1)).get_handle(); }
METHOD(__mul__) { GET_SELF; return wrapDecimal(d1->mul(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__rmul__) { GET_SELF; return wrapDecimal(parseDecimalArg(jc2::Value(argv[1])).mul(*d1)).get_handle(); }
METHOD(__div__) { GET_SELF; return wrapDecimal(d1->div(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__rdiv__) { GET_SELF; return wrapDecimal(parseDecimalArg(jc2::Value(argv[1])).div(*d1)).get_handle(); }
METHOD(__eq__) { 
    GET_SELF; 
    jc2::Value rhs(argv[1]);
    if (!canConvertToDecimal(rhs)) return jc2::Value(false).get_handle();
    return jc2::Value(d1->eq(parseDecimalArg(rhs))).get_handle(); 
}
METHOD(__lt__) { GET_SELF; return jc2::Value(d1->lt(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__gt__) { GET_SELF; return jc2::Value(d1->gt(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__le__) { GET_SELF; return jc2::Value(d1->le(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__ge__) { GET_SELF; return jc2::Value(d1->ge(parseDecimalArg(jc2::Value(argv[1])))).get_handle(); }
METHOD(__neq__) { 
    GET_SELF; 
    jc2::Value rhs(argv[1]);
    if (!canConvertToDecimal(rhs)) return jc2::Value(true).get_handle();
    return jc2::Value(d1->neq(parseDecimalArg(rhs))).get_handle(); 
}
METHOD(__neg__) { GET_SELF; return wrapDecimal(Decimal(DecInt(0), 0).sub(*d1)).get_handle(); }
METHOD(__bool__) { GET_SELF; return jc2::Value(!d1->eq(Decimal(DecInt(0), 0))).get_handle(); }
METHOD(__abs__) { GET_SELF; return wrapDecimal(d1->abs()).get_handle(); }
METHOD(sqrt) { GET_SELF; return wrapDecimal(d1->sqrt()).get_handle(); }
METHOD(exp) { GET_SELF; return wrapDecimal(d1->exp_val()).get_handle(); }
METHOD(sin) { GET_SELF; return wrapDecimal(d1->sin_val()).get_handle(); }
METHOD(cos) { GET_SELF; return wrapDecimal(d1->cos_val()).get_handle(); }
METHOD(tan) { GET_SELF; return wrapDecimal(d1->tan_val()).get_handle(); }
METHOD(ln) { GET_SELF; return wrapDecimal(d1->ln_val()).get_handle(); }
METHOD(log10) { GET_SELF; return wrapDecimal(d1->log10_val()).get_handle(); }
METHOD(asin) { GET_SELF; return wrapDecimal(d1->asin_val()).get_handle(); }
METHOD(acos) { GET_SELF; return wrapDecimal(d1->acos_val()).get_handle(); }
METHOD(atan) { GET_SELF; return wrapDecimal(d1->atan_val()).get_handle(); }
METHOD(sinh) { GET_SELF; return wrapDecimal(d1->sinh_val()).get_handle(); }
METHOD(cosh) { GET_SELF; return wrapDecimal(d1->cosh_val()).get_handle(); }
METHOD(tanh) { GET_SELF; return wrapDecimal(d1->tanh_val()).get_handle(); }

METHOD(__hash__) { 
    GET_SELF; 
    return jc2::Value(d1->to_string()).get_handle(); 
}

JC2_ValueHandle global_Decimal(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 1) jc2::throw_error("TypeError: Decimal() takes exactly 1 argument (0 given).");
    return wrapDecimal(parseDecimalArg(jc2::Value(argv[0]))).get_handle();
}

JC2_ValueHandle global_getcontext(JC2_VMContext, int, JC2_ValueHandle*, void*) {
    jc2::Dict ctx;
    ctx.set(jc2::Value("prec"), jc2::Value(Decimal::g_prec));
    return ctx.get_handle();
}

JC2_ValueHandle global_setcontext(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc > 0) {
        Decimal::g_prec = static_cast<int>(jc2::Value(argv[0]).as_double());
    }
    return jc2::Value().get_handle();
}

JC2_ValueHandle global_pi(JC2_VMContext, int argc, JC2_ValueHandle*, void*) {
    (void)argc;
    return wrapDecimal(Decimal::pi()).get_handle();
}

JC2_ValueHandle global_e(JC2_VMContext, int argc, JC2_ValueHandle*, void*) {
    (void)argc;
    return wrapDecimal(Decimal::e()).get_handle();
}

JC2_ValueHandle global_ln2(JC2_VMContext, int argc, JC2_ValueHandle*, void*) {
    (void)argc;
    return wrapDecimal(Decimal::ln2()).get_handle();
}

JC2_ValueHandle global_ln10(JC2_VMContext, int argc, JC2_ValueHandle*, void*) {
    (void)argc;
    return wrapDecimal(Decimal::ln10()).get_handle();
}

JC2_ValueHandle global_sqrt2(JC2_VMContext, int argc, JC2_ValueHandle*, void*) {
    (void)argc;
    return wrapDecimal(Decimal::sqrt2()).get_handle();
}

int jc2_init(jc2::Module& mod) {
    g_decimalClass = new jc2::Class("Decimal");
    mod.register_value("Decimal", *g_decimalClass);

    g_decimalClass->bind_method("__str__", decimal___str__, 0, 0);
    g_decimalClass->bind_method("__add__", decimal___add__, 1, 1, {"other"});
    g_decimalClass->bind_method("__radd__", decimal___radd__, 1, 1, {"other"});
    g_decimalClass->bind_method("__sub__", decimal___sub__, 1, 1, {"other"});
    g_decimalClass->bind_method("__rsub__", decimal___rsub__, 1, 1, {"other"});
    g_decimalClass->bind_method("__mul__", decimal___mul__, 1, 1, {"other"});
    g_decimalClass->bind_method("__rmul__", decimal___rmul__, 1, 1, {"other"});
    g_decimalClass->bind_method("__div__", decimal___div__, 1, 1, {"other"});
    g_decimalClass->bind_method("__rdiv__", decimal___rdiv__, 1, 1, {"other"});
    g_decimalClass->bind_method("__eq__", decimal___eq__, 1, 1, {"other"});
    g_decimalClass->bind_method("__lt__", decimal___lt__, 1, 1, {"other"});
    g_decimalClass->bind_method("__gt__", decimal___gt__, 1, 1, {"other"});
    g_decimalClass->bind_method("__le__", decimal___le__, 1, 1, {"other"});
    g_decimalClass->bind_method("__ge__", decimal___ge__, 1, 1, {"other"});
    g_decimalClass->bind_method("__neq__", decimal___neq__, 1, 1, {"other"});
    g_decimalClass->bind_method("__hash__", decimal___hash__, 0, 0);
    g_decimalClass->bind_method("__neg__", decimal___neg__, 0, 0);
    g_decimalClass->bind_method("__bool__", decimal___bool__, 0, 0);
    g_decimalClass->bind_method("__abs__", decimal___abs__, 0, 0);
    g_decimalClass->bind_method("sqrt", decimal_sqrt, 0, 0);
    g_decimalClass->bind_method("exp", decimal_exp, 0, 0);
    g_decimalClass->bind_method("sin", decimal_sin, 0, 0);
    g_decimalClass->bind_method("cos", decimal_cos, 0, 0);
    g_decimalClass->bind_method("tan", decimal_tan, 0, 0);
    g_decimalClass->bind_method("ln", decimal_ln, 0, 0);
    g_decimalClass->bind_method("log10", decimal_log10, 0, 0);
    g_decimalClass->bind_method("asin", decimal_asin, 0, 0);
    g_decimalClass->bind_method("acos", decimal_acos, 0, 0);
    g_decimalClass->bind_method("atan", decimal_atan, 0, 0);
    g_decimalClass->bind_method("sinh", decimal_sinh, 0, 0);
    g_decimalClass->bind_method("cosh", decimal_cosh, 0, 0);
    g_decimalClass->bind_method("tanh", decimal_tanh, 0, 0);

    g_decimalClass->set_allocator(global_Decimal);

    mod.register_function("getcontext", global_getcontext, 0, 0);
    mod.register_function("setcontext", global_setcontext, 1, 1, {"prec"});
    mod.register_function("pi", global_pi, 0, 0);
    mod.register_function("e", global_e, 0, 0);
    mod.register_function("ln2", global_ln2, 0, 0);
    mod.register_function("ln10", global_ln10, 0, 0);
    mod.register_function("sqrt2", global_sqrt2, 0, 0);

    mod.register_help("decimal",
        "═══ Arbitrary-Precision Decimal Arithmetic — Native Module ═══\n\n"
        "  Requires: import decimal\n\n"
        "  The `decimal` module provides a Decimal data type for fast correctly-rounded\n"
        "  decimal floating point arithmetic. It avoids the precision issues of standard\n"
        "  binary floating point (double).\n\n"
        "  Construction\n"
        "  ──────────────────────\n"
        "    decimal.Decimal(\"0.1\")        Create from string (Recommended)\n"
        "    decimal.Decimal(42)           Create from integer\n"
        "    decimal.Decimal(3.14)         Create from double\n\n"
        "  Context Management\n"
        "  ──────────────────────\n"
        "    decimal.getcontext()          Returns a dict with current context settings\n"
        "    decimal.setcontext(prec)      Sets the global precision (number of digits)\n\n"
        "  Math Functions\n"
        "  ──────────────────────\n"
        "    decimal.pi() / e()            Returns Pi or Euler's number (e)\n"
        "    decimal.ln2() / ln10()        Returns natural log of 2 or 10\n"
        "    decimal.sqrt2()               Returns square root of 2\n"
        "    d.sqrt()                      Square root\n"
        "    d.exp()                       Exponential (e^x)\n"
        "    d.ln() / d.log10()            Natural and Base-10 Logarithm\n"
        "    d.sin() / d.cos() / d.tan()   Trigonometric functions\n"
        "    d.asin() / d.acos() / d.atan()Inverse Trigonometric functions\n"
        "    d.sinh() / d.cosh() / d.tanh()Hyperbolic functions\n"
        "    abs(d)                        Absolute value\n\n"
        "  Example\n"
        "  ──────────────────────\n"
        "    import decimal\n"
        "    decimal.setcontext(50)\n"
        "    a = decimal.Decimal(\"0.1\")\n"
        "    b = decimal.Decimal(\"0.2\")\n"
        "    print(a + b)                  // → 0.3 (Exact!)\n"
    );

    mod.register_function_help("decimal.Decimal", "decimal.Decimal(value)", "Creates a new Decimal object from a string, integer, or double.", "d = decimal.Decimal(\"123.45\")");
    mod.register_function_help("decimal.getcontext", "decimal.getcontext()", "Returns the current decimal context (precision).", "ctx = decimal.getcontext()");
    mod.register_function_help("decimal.setcontext", "decimal.setcontext(prec)", "Sets the global precision for decimal division.", "decimal.setcontext(50)");

    return 0;
}

JC2_EXTENSION_INIT
