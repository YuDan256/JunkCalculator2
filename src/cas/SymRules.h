#ifndef JC2_SYMRULES_H
#define JC2_SYMRULES_H

#include "Symbolic.h"
#include <vector>
#include <utility>
#include <string>

namespace jc {

    // =================================================================
    // 积分规则字典表 (静态缓存，使用通配符 _x)
    // =================================================================
    inline const std::vector<std::pair<SymExpr, SymExpr>>& getStaticIntegRules() {
        static std::vector<std::pair<SymExpr, SymExpr>> rules;
        if (rules.empty()) {
            SymExpr x = SymExpr::makeVar("_x");
            SymExpr _n = SymExpr::makeVar("_n");
            SymExpr _a = SymExpr::makeVar("_a");

            auto func = [](const std::string& name, const SymExpr& arg) {
                return SymExpr(new SymFunc(name, std::vector<SymNode*>{arg.ptr}));
            };
            auto func2 = [](const std::string& name, const SymExpr& arg1, const SymExpr& arg2) {
                return SymExpr(new SymFunc(name, std::vector<SymNode*>{arg1.ptr, arg2.ptr}));
            };

            rules = {
            // 幂函数与对数
            { x ^ SymExpr(-1), func("log", x) },
            { x ^ _n, (SymExpr(1) / (_n + SymExpr(1))) * (x ^ (_n + SymExpr(1))) },
            { x, (SymExpr(1) / SymExpr(2)) * (x ^ SymExpr(2)) },
            
            // 指数函数
            { func("exp", x), func("exp", x) },
            { _a ^ x, (_a ^ x) / func("log", _a) },
            
            // 绝对值与符号函数
            { func("sgn", x), func("abs", x) },
            
            // 对数与反三角函数 (可由分部积分推导，此处作为查表加速)
            { func("log", x), x * func("log", x) - x },
            { func("asin", x), x * func("asin", x) + ((SymExpr(1) - (x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2))) },
            { func("acos", x), x * func("acos", x) - ((SymExpr(1) - (x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2))) },
            { func("atan", x), x * func("atan", x) - (SymExpr(1) / SymExpr(2)) * func("log", SymExpr(1) + (x ^ SymExpr(2))) },

            // 反双曲函数积分
            { func("asinh", x), x * func("asinh", x) - (((x ^ SymExpr(2)) + SymExpr(1)) ^ SymExpr(Fraction(1, 2))) },
            { func("acosh", x), x * func("acosh", x) - (((x ^ SymExpr(2)) - SymExpr(1)) ^ SymExpr(Fraction(1, 2))) },
            { func("atanh", x), x * func("atanh", x) + (SymExpr(1) / SymExpr(2)) * func("log", SymExpr(1) - (x ^ SymExpr(2))) },

            // 基础三角函数
            { func("sin", x), -func("cos", x) },
            { func("cos", x), func("sin", x) },
            { func("tan", x), -func("log", func("cos", x)) },
            { SymExpr(1) / func("tan", x), func("log", func("sin", x)) },
            { SymExpr(1) / func("cos", x), func("log", (SymExpr(1) + func("sin", x)) / func("cos", x)) },
            { SymExpr(1) / func("sin", x), func("log", func("tan", x / SymExpr(2))) },
            { SymExpr(1) / (func("cos", x) ^ SymExpr(2)), func("tan", x) },
            { SymExpr(1) / (func("sin", x) ^ SymExpr(2)), -SymExpr(1) / func("tan", x) },
            
            // 三角函数平方与倒数平方
            { func("tan", x) ^ SymExpr(2), func("tan", x) - x },
            { SymExpr(1) / (func("tan", x) ^ SymExpr(2)), -x - SymExpr(1) / func("tan", x) },
            
            // 反三角函数导数逆运算
            { SymExpr(1) / (SymExpr(1) + (x ^ SymExpr(2))), func("atan", x) },
            { SymExpr(1) / ((SymExpr(1) - (x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2))), func("asin", x) },
            
            // 反双曲函数导数逆运算
            { SymExpr(1) / ((SymExpr(1) + (x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2))), func("asinh", x) },
            { SymExpr(1) / (((x ^ SymExpr(2)) - SymExpr(1)) ^ SymExpr(Fraction(1, 2))), func("acosh", x) },
            
            // 双曲函数
            { func("sinh", x), func("cosh", x) },
            { func("cosh", x), func("sinh", x) },
            { func("tanh", x), func("log", func("cosh", x)) },
            { SymExpr(1) / (func("cosh", x) ^ SymExpr(2)), func("tanh", x) },
            { SymExpr(1) / (func("sinh", x) ^ SymExpr(2)), -SymExpr(1) / func("tanh", x) },
            { func("tanh", x) ^ SymExpr(2), x - func("tanh", x) },
            { SymExpr(1) / (func("tanh", x) ^ SymExpr(2)), x - SymExpr(1) / func("tanh", x) },
            
            // 常见无理函数积分 (避免错误的分部积分或换元)
            { (SymExpr(1) + (x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2)), 
              (x * ((SymExpr(1) + (x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2))) / SymExpr(2)) + 
              (SymExpr(1) / SymExpr(2)) * func("log", x + ((SymExpr(1) + (x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2)))) },
            { ((x ^ SymExpr(2)) + SymExpr(1)) ^ SymExpr(Fraction(1, 2)), 
              (x * (((x ^ SymExpr(2)) + SymExpr(1)) ^ SymExpr(Fraction(1, 2))) / SymExpr(2)) + 
              (SymExpr(1) / SymExpr(2)) * func("log", x + (((x ^ SymExpr(2)) + SymExpr(1)) ^ SymExpr(Fraction(1, 2)))) },
            { ((x ^ SymExpr(2)) - SymExpr(1)) ^ SymExpr(Fraction(1, 2)), 
              (x * (((x ^ SymExpr(2)) - SymExpr(1)) ^ SymExpr(Fraction(1, 2))) / SymExpr(2)) - 
              (SymExpr(1) / SymExpr(2)) * func("log", x + (((x ^ SymExpr(2)) - SymExpr(1)) ^ SymExpr(Fraction(1, 2)))) },
            { (SymExpr(1) - (x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2)), 
              (x * ((SymExpr(1) - (x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2))) / SymExpr(2)) + 
              (SymExpr(1) / SymExpr(2)) * func("asin", x) },

            // 椭圆积分 (Elliptic Integrals)
            { SymExpr(1) / (((SymExpr(1) - (x ^ SymExpr(2))) * (SymExpr(1) - _a * (x ^ SymExpr(2)))) ^ SymExpr(Fraction(1, 2))), func2("EllipticF", x, _a) },
            { ((SymExpr(1) - _a * (x ^ SymExpr(2))) / (SymExpr(1) - (x ^ SymExpr(2)))) ^ SymExpr(Fraction(1, 2)), func2("EllipticE", x, _a) },

            // 特殊函数 (误差函数与菲涅尔积分)
            { func("exp", -(x ^ SymExpr(2))), (SymExpr(Fraction(1, 2)) * (SymExpr::makeVar("PI") ^ SymExpr(Fraction(1, 2)))) * func("erf", x) },
            { func("sin", x ^ SymExpr(2)), ((SymExpr::makeVar("PI") / SymExpr(2)) ^ SymExpr(Fraction(1, 2))) * func("fresnel_s", ((SymExpr(2) / SymExpr::makeVar("PI")) ^ SymExpr(Fraction(1, 2))) * x) },
            { func("cos", x ^ SymExpr(2)), ((SymExpr::makeVar("PI") / SymExpr(2)) ^ SymExpr(Fraction(1, 2))) * func("fresnel_c", ((SymExpr(2) / SymExpr::makeVar("PI")) ^ SymExpr(Fraction(1, 2))) * x) },
            
            // 积分正弦、余弦与指数积分
            { func("sin", x) / x, func("Si", x) },
            { func("cos", x) / x, func("Ci", x) },
            { func("exp", x) / x, func("Ei", x) },
            { SymExpr(1) / func("log", x), func("Li", x) }
            };
        }
        return rules;
    }

    // =================================================================
    // 三角化简规则字典表 (静态缓存，使用通配符 _x, _c)
    // =================================================================
    inline const std::vector<std::pair<SymExpr, SymExpr>>& getTrigRules() {
        static std::vector<std::pair<SymExpr, SymExpr>> rules;
        if (rules.empty()) {
            SymExpr _x = SymExpr::makeVar("_x");
            SymExpr _c = SymExpr::makeVar("_c");

            auto func = [](const std::string& name, const SymExpr& arg) {
                return SymExpr(new SymFunc(name, std::vector<SymNode*>{arg.ptr}));
            };

            SymExpr sin_x = func("sin", _x);
            SymExpr cos_x = func("cos", _x);
            SymExpr tan_x = func("tan", _x);
            SymExpr sin_2x = func("sin", SymExpr(2) * _x);
            SymExpr cos_2x = func("cos", SymExpr(2) * _x);

            rules = {
                // 倒数关系 (将 cot, sec, csc 统一化归为 tan, cos, sin)
                { func("cot", _x), SymExpr(1) / tan_x },
                { func("sec", _x), SymExpr(1) / cos_x },
                { func("csc", _x), SymExpr(1) / sin_x },

                // 平方和恒等式
                { (sin_x ^ SymExpr(2)) + (cos_x ^ SymExpr(2)), SymExpr(1) },
                { _c * (sin_x ^ SymExpr(2)) + _c * (cos_x ^ SymExpr(2)), _c },
                
                // 平方差转换
                { SymExpr(1) - (sin_x ^ SymExpr(2)), cos_x ^ SymExpr(2) },
                { SymExpr(1) - (cos_x ^ SymExpr(2)), sin_x ^ SymExpr(2) },
                { _c - _c * (sin_x ^ SymExpr(2)), _c * (cos_x ^ SymExpr(2)) },
                { _c - _c * (cos_x ^ SymExpr(2)), _c * (sin_x ^ SymExpr(2)) },
                
                // 商数关系
                { sin_x / cos_x, tan_x },
                { cos_x / sin_x, SymExpr(1) / tan_x },
                { (sin_x ^ SymExpr(2)) / (cos_x ^ SymExpr(2)), tan_x ^ SymExpr(2) },
                { (cos_x ^ SymExpr(2)) / (sin_x ^ SymExpr(2)), SymExpr(1) / (tan_x ^ SymExpr(2)) },
                
                // 乘积与消去关系
                { tan_x * cos_x, sin_x },
                { cos_x * tan_x, sin_x },
                { sin_x / tan_x, cos_x },
                { tan_x / sin_x, SymExpr(1) / cos_x },
                
                // 割线与正切的平方关系 (1 + tan^2 = sec^2)
                { SymExpr(1) + (tan_x ^ SymExpr(2)), SymExpr(1) / (cos_x ^ SymExpr(2)) },
                { _c + _c * (tan_x ^ SymExpr(2)), _c / (cos_x ^ SymExpr(2)) },
                { (SymExpr(1) / (cos_x ^ SymExpr(2))) - (tan_x ^ SymExpr(2)), SymExpr(1) },
                { (SymExpr(1) / (cos_x ^ SymExpr(2))) - SymExpr(1), tan_x ^ SymExpr(2) },

                // 倍角公式逆向化简
                { sin_x * cos_x, (SymExpr(1) / SymExpr(2)) * sin_2x },
                { SymExpr(2) * sin_x * cos_x, sin_2x },
                { _c * sin_x * cos_x, (_c / SymExpr(2)) * sin_2x },
                { (cos_x ^ SymExpr(2)) - (sin_x ^ SymExpr(2)), cos_2x },
                { _c * (cos_x ^ SymExpr(2)) - _c * (sin_x ^ SymExpr(2)), _c * cos_2x },
                { SymExpr(1) - SymExpr(2) * (sin_x ^ SymExpr(2)), cos_2x },
                { SymExpr(2) * (cos_x ^ SymExpr(2)) - SymExpr(1), cos_2x },

                // 倍角公式正向约分
                { sin_2x / cos_x, SymExpr(2) * sin_x },
                { sin_2x / sin_x, SymExpr(2) * cos_x },

                // 负角化简
                { func("sin", SymExpr(-1) * _x), -sin_x },
                { func("cos", SymExpr(-1) * _x), cos_x },
                { func("tan", SymExpr(-1) * _x), -tan_x },
                { func("cot", SymExpr(-1) * _x), -func("cot", _x) },
                { func("sec", SymExpr(-1) * _x), func("sec", _x) },
                { func("csc", SymExpr(-1) * _x), -func("csc", _x) },
                { func("sinh", SymExpr(-1) * _x), -func("sinh", _x) },
                { func("cosh", SymExpr(-1) * _x), func("cosh", _x) },
                { func("tanh", SymExpr(-1) * _x), -func("tanh", _x) },

                // 双曲函数平方关系
                { (func("cosh", _x) ^ SymExpr(2)) - (func("sinh", _x) ^ SymExpr(2)), SymExpr(1) },
                { _c * (func("cosh", _x) ^ SymExpr(2)) - _c * (func("sinh", _x) ^ SymExpr(2)), _c },
                { (func("sinh", _x) ^ SymExpr(2)) + SymExpr(1), func("cosh", _x) ^ SymExpr(2) },
                { (func("cosh", _x) ^ SymExpr(2)) - SymExpr(1), func("sinh", _x) ^ SymExpr(2) },
                { _c * (func("sinh", _x) ^ SymExpr(2)) + _c, _c * (func("cosh", _x) ^ SymExpr(2)) },
                { _c * (func("cosh", _x) ^ SymExpr(2)) - _c, _c * (func("sinh", _x) ^ SymExpr(2)) },

                // 半角反三角函数化简
                { func("tan", func("atan", _x) / SymExpr(2)), (((_x ^ SymExpr(2)) + SymExpr(1)) ^ SymExpr(Fraction(1, 2)) - SymExpr(1)) / _x },
                { func("tan", func("asin", _x) / SymExpr(2)), _x / (SymExpr(1) + ((SymExpr(1) - (_x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2)))) },
                { func("tan", func("acos", _x) / SymExpr(2)), ((SymExpr(1) - _x) ^ SymExpr(Fraction(1, 2))) / ((SymExpr(1) + _x) ^ SymExpr(Fraction(1, 2))) },

                // 反三角函数嵌套化简
                { func("sin", func("asin", _x)), _x },
                { func("cos", func("acos", _x)), _x },
                { func("tan", func("atan", _x)), _x },
                { func("asin", func("sin", _x)), _x },
                { func("acos", func("cos", _x)), _x },
                { func("atan", func("tan", _x)), _x },
                { func("cos", func("asin", _x)), (SymExpr(1) - (_x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2)) },
                { func("sin", func("acos", _x)), (SymExpr(1) - (_x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2)) },
                { func("sin", func("atan", _x)), _x / ((SymExpr(1) + (_x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2))) },
                { func("cos", func("atan", _x)), SymExpr(1) / ((SymExpr(1) + (_x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2))) },
                { func("tan", func("asin", _x)), _x / ((SymExpr(1) - (_x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2))) },
                { func("tan", func("acos", _x)), ((SymExpr(1) - (_x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2))) / _x },

                // 双曲-反双曲嵌套化简
                { func("sinh", func("asinh", _x)), _x },
                { func("cosh", func("acosh", _x)), _x },
                { func("tanh", func("atanh", _x)), _x },
                { func("asinh", func("sinh", _x)), _x },
                { func("acosh", func("cosh", _x)), _x },
                { func("atanh", func("tanh", _x)), _x },
                { func("cosh", func("asinh", _x)), ((_x ^ SymExpr(2)) + SymExpr(1)) ^ SymExpr(Fraction(1, 2)) },
                { func("sinh", func("acosh", _x)), ((_x ^ SymExpr(2)) - SymExpr(1)) ^ SymExpr(Fraction(1, 2)) },
                { func("tanh", func("asinh", _x)), _x / (((_x ^ SymExpr(2)) + SymExpr(1)) ^ SymExpr(Fraction(1, 2))) },
                { func("tanh", func("acosh", _x)), (((_x ^ SymExpr(2)) - SymExpr(1)) ^ SymExpr(Fraction(1, 2))) / _x },
                { func("sinh", func("atanh", _x)), _x / ((SymExpr(1) - (_x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2))) },
                { func("cosh", func("atanh", _x)), SymExpr(1) / ((SymExpr(1) - (_x ^ SymExpr(2))) ^ SymExpr(Fraction(1, 2))) },

                // 反双曲函数与根式嵌套化简
                { func("asinh", ((_x ^ SymExpr(2)) - SymExpr(1)) ^ SymExpr(Fraction(1, 2))), func("acosh", _x) },
                { func("acosh", ((_x ^ SymExpr(2)) + SymExpr(1)) ^ SymExpr(Fraction(1, 2))), func("asinh", _x) }
            };
        }
        return rules;
    }

} // namespace jc

#endif // JC2_SYMRULES_H
