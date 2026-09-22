#ifndef JC2_INTEGRATION_H
#define JC2_INTEGRATION_H

#include "Symbolic.h"
#include <string>

namespace jc {
    SymExpr integrate(const SymExpr& expr, const std::string& var, int depth = 0); // 符号积分
    SymExpr defint(const SymExpr& expr, const std::string& var, const SymExpr& a, const SymExpr& b); // 定积分
    SymExpr rischIntegrate(const SymExpr& expr, const std::string& var, int depth = 0); // Risch 算法入口

    // 原函数自检的三态结果。
    // Valid   : 残差恒零，或全部可求值测试点通过。
    // Invalid : 有确凿反证 —— 至少一个可求值的复数测试点上残差不可忽略。
    // Unknown : 残差非零，但一个测试点都求不出数值。
    // 极性由调用方决定：用户侧 cas.verifyInteg 只有 Valid 才算通过；
    // 推导过程中的守卫只在 Invalid（确凿反证）时拒绝，Unknown 保持原行为——
    // 实测有正确积分的化简残差并不恒零（如 sin(p)^3、1/sin(p)），
    // 若"求不出就拒绝"会误杀这些本来正确的解。
    enum class AntiderivCheck { Valid, Invalid, Unknown };

    // 判定 F 是否为 f 关于 var 的原函数：先看化简后的 dF/dv - f 是否恒零，
    // 否则取复数测试点做数值比较（复数点可避开实数域的定义域陷阱，如 log(-x)、sqrt(-x)）。
    AntiderivCheck checkAntiderivative(const SymExpr& f, const SymExpr& F, const std::string& var);
}

#endif // JC2_INTEGRATION_H
