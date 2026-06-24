#include "../jc2_extension_cpp.h"
#include <sstream>
#include <iomanip>
#include <cctype>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <stdexcept>
#include <vector>
#include <string>

std::string valueToLatex(const jc2::Value& val) {
    if (val.is_double()) {
        std::ostringstream oss; oss << std::defaultfloat << std::setprecision(6) << val.as_double();
        return oss.str();
    }
    else if (val.is_int()) return std::to_string(val.as_int());
    else if (val.is_bigint()) return jc2::BigInt(val.get_handle()).to_string();
    else if (val.is_fraction()) {
        jc2::Fraction f(val.get_handle());
        return "\\frac{" + f.num().to_string() + "}{" + f.den().to_string() + "}";
    }
    else if (val.is_complex()) {
        jc2::Complex c(val.get_handle());
        std::ostringstream oss; oss << std::defaultfloat << std::setprecision(4);
        if (c.real() == 0.0 && c.imag() == 0.0) return "0";
        if (c.real() != 0.0) oss << c.real();
        if (c.imag() != 0.0) {
            if (c.imag() > 0 && c.real() != 0.0) oss << "+";
            if (std::abs(c.imag() - 1.0) < 1e-9) oss << "i";
            else if (std::abs(c.imag() - -1.0) < 1e-9) oss << "-i";
            else oss << c.imag() << "i";
        }
        return oss.str();
    }
    else if (val.is_real_matrix()) {
        jc2::RealMatrix m(val.get_handle());
        std::ostringstream oss; oss << "\\begin{pmatrix}\n";
        for (int i = 0; i < m.rows(); ++i) {
            for (int j = 0; j < m.cols(); ++j) {
                oss << std::defaultfloat << std::setprecision(4) << m.get(i, j);
                if (j < m.cols() - 1) oss << " & ";
            }
            oss << (i < m.rows() - 1 ? " \\\\\n" : "\n");
        }
        oss << "\\end{pmatrix}";
        return oss.str();
    }
    else if (val.is_list()) {
        jc2::List l(val.get_handle());
        std::string s = "\\left[ ";
        for (size_t i = 0; i < l.size(); ++i) {
            s += valueToLatex(l.get(i));
            if (i < l.size() - 1) s += ", ";
        }
        return s + " \\right]";
    }
    return val.to_string();
}

class LatexParser {
    std::string src; size_t pos = 0;

    void skipSpace() { while (pos < src.size() && std::isspace(src[pos])) pos++; }
    bool match(char c) { skipSpace(); if (pos < src.size() && src[pos] == c) { pos++; return true; } return false; }

    std::string peekCmd() {
        skipSpace();
        if (pos < src.size() && src[pos] == '\\') {
            size_t p = pos + 1;
            if (p < src.size() && src[p] == '\\') return "\\\\";
            while (p < src.size() && std::isalpha(src[p])) p++;
            return src.substr(pos, p - pos);
        }
        return "";
    }

    bool matchCmd(const std::string& cmd) {
        if (peekCmd() == cmd) { pos += cmd.size(); return true; }
        return false;
    }

    std::string parseMatrix() {
        std::string envName;
        if (matchCmd("\\begin")) {
            if (!match('{')) jc2::throw_error("Expected '{' after \\begin");
            size_t start = pos;
            while (pos < src.size() && src[pos] != '}') pos++;
            envName = src.substr(start, pos - start);
            if (!match('}')) jc2::throw_error("Expected '}' after \\begin{...");
        } else {
            return "";
        }

        std::string code = "[";

        while (pos < src.size()) {
            skipSpace();
            if (matchCmd("\\end")) {
                if (!match('{')) jc2::throw_error("Expected '{' after \\end");
                size_t start = pos;
                while (pos < src.size() && src[pos] != '}') pos++;
                std::string endName = src.substr(start, pos - start);
                if (!match('}')) jc2::throw_error("Expected '}' after \\end{...");
                if (endName != envName) jc2::throw_error("Mismatched \\begin{" + envName + "} and \\end{" + endName + "}");
                break;
            }
            if (matchCmd("\\\\")) {
                code += "; ";
                continue;
            }
            if (match('&')) {
                code += ", ";
                continue;
            }
            code += parseExpr();
        }
        code += "]";
        return code;
    }

    std::string parseFactor() {
        skipSpace();
        if (peekCmd() == "\\begin") {
            return parseMatrix();
        }
        if (matchCmd("\\frac")) {
            if (!match('{')) jc2::throw_error("Expected '{' after \\frac");
            auto num = parseExpr();
            if (!match('}')) jc2::throw_error("Expected '}' after numerator");
            if (!match('{')) jc2::throw_error("Expected '{' for denominator");
            auto den = parseExpr();
            if (!match('}')) jc2::throw_error("Expected '}' after denominator");
            return "((" + num + ") / (" + den + "))";
        }

        std::string cmd = peekCmd();
        if (cmd == "\\sin" || cmd == "\\cos" || cmd == "\\tan" || cmd == "\\sqrt" ||
            cmd == "\\ln" || cmd == "\\log" || cmd == "\\exp" || cmd == "\\abs") {
            pos += cmd.size();
            skipSpace();

            std::string arg;
            if (match('(')) {
                arg = parseExpr();
                if (!match(')')) jc2::throw_error("Missing ')' for " + cmd);
            }
            else if (match('{')) {
                arg = parseExpr();
                if (!match('}')) jc2::throw_error("Missing '}' for " + cmd);
            }
            else {
                arg = parsePower();
            }
            std::string fname = cmd.substr(1);
            if (fname == "ln") fname = "log";
            return fname + "(" + arg + ")";
        }

        if (match('(')) {
            auto expr = parseExpr();
            if (!match(')')) jc2::throw_error("Missing closing ')'");
            return "(" + expr + ")";
        }
        if (match('{')) {
            auto expr = parseExpr();
            if (!match('}')) jc2::throw_error("Missing closing '}'");
            return "(" + expr + ")";
        }
        if (match('[')) {
            auto expr = parseExpr();
            if (!match(']')) jc2::throw_error("Missing closing ']'");
            return "(" + expr + ")";
        }

        size_t start = pos;
        while (pos < src.size() && (std::isdigit(src[pos]) || src[pos] == '.')) pos++;
        if (pos > start) return src.substr(start, pos - start);

        if (cmd != "") {
            pos += cmd.size();
            if (cmd == "\\pi") return "PI";
            return cmd.substr(1);
        }
        if (pos < src.size() && std::isalpha(src[pos])) {
            std::string varStr(1, src[pos++]);
            return varStr;
        }

        jc2::throw_error("LaTeX Parse Error: Unexpected token at '" + src.substr(pos, 5) + "...'");
        return "";
    }

    std::string parsePower() {
        auto left = parseFactor();
        skipSpace();
        if (match('^')) {
            auto right = parseFactor();
            return "(" + left + ") ^ (" + right + ")";
        }
        return left;
    }

    std::string parseTerm() {
        auto left = parsePower();
        while (true) {
            skipSpace();
            if (match('*') || matchCmd("\\cdot") || matchCmd("\\times")) {
                left = "(" + left + ") * (" + parsePower() + ")";
            }
            else if (match('/')) {
                left = "(" + left + ") / (" + parsePower() + ")";
            }
            else {
                if (pos < src.size() && (src[pos] == '(' || std::isalpha(src[pos]))) {
                    left = "(" + left + ") * (" + parsePower() + ")";
                }
                else if (pos < src.size() && src[pos] == '\\') {
                    std::string cmd = peekCmd();
                    if (cmd == "\\\\" || cmd == "\\end") break;
                    left = "(" + left + ") * (" + parsePower() + ")";
                }
                else break;
            }
        }
        return left;
    }

public:
    std::string parseExpr() {
        skipSpace();
        std::string left;
        bool negate = false;
        if (match('-')) negate = true;
        else match('+');

        left = parseTerm();
        if (negate) left = "-(" + left + ")";

        while (true) {
            skipSpace();
            if (match('+')) left = "(" + left + ") + (" + parseTerm() + ")";
            else if (match('-')) left = "(" + left + ") - (" + parseTerm() + ")";
            else break;
        }
        return left;
    }

    std::string compile(const std::string& latex) {
        src = latex; pos = 0;
        return parseExpr();
    }
};

JC2_ValueHandle global_to_latex(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    return jc2::Value(valueToLatex(jc2::Value(argv[0]))).get_handle();
}

JC2_ValueHandle global_eval(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Value arg(argv[0]);
    if (!arg.is_string()) jc2::throw_error("eval() requires a LaTeX string.");
    LatexParser parser;
    std::string code = parser.compile(arg.as_string());
    
    jc2::Value evalFunc(jc2::Env::api->get_global(jc2::Env::ctx, "eval"));
    if (!evalFunc.is_function()) jc2::throw_error("Internal Error: 'eval' function not found.");
    
    JC2_ValueHandle codeHandle = jc2::Value(code).get_handle();
    jc2::Value res(jc2::Env::api->call_function(jc2::Env::ctx, evalFunc.get_handle(), 1, &codeHandle));
    
    if (res.is_complex() && jc2::Complex(res.get_handle()).imag() == 0.0) return jc2::Value(jc2::Complex(res.get_handle()).real()).get_handle();
    return res.get_handle();
}

JC2_ValueHandle global_compile(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Value arg0(argv[0]);
    jc2::Value arg1(argv[1]);
    if (!arg0.is_string()) jc2::throw_error("compile(string, vars): Requires formula and variable names.");

    std::string latex_str = arg0.as_string();
    std::vector<std::string> varNames;

    if (arg1.is_list()) {
        jc2::List l(arg1.get_handle());
        for (size_t i = 0; i < l.size(); ++i) {
            jc2::Value v = l.get(i);
            if (!v.is_string()) jc2::throw_error("Variable names must be strings.");
            varNames.push_back(v.as_string());
        }
    }
    else if (arg1.is_string_matrix()) {
        jc2::StringMatrix m(arg1.get_handle());
        for (int i = 0; i < m.rows(); ++i) {
            for (int j = 0; j < m.cols(); ++j) {
                varNames.push_back(m.get(i, j));
            }
        }
    }
    else jc2::throw_error("compile_latex(): 2nd argument must be a List or Matrix of variable strings.");

    LatexParser parser;
    std::string exprCode = parser.compile(latex_str);

    std::string funcCode = "(";
    for (size_t i = 0; i < varNames.size(); ++i) {
        std::string v = varNames[i];
        if (!v.empty() && v[0] == '\\') v = v.substr(1);
        funcCode += v;
        if (i < varNames.size() - 1) funcCode += ", ";
    }
    funcCode += ") => " + exprCode;

    jc2::Value evalFunc(jc2::Env::api->get_global(jc2::Env::ctx, "eval"));
    if (!evalFunc.is_function()) jc2::throw_error("Internal Error: 'eval' function not found.");
    
    JC2_ValueHandle codeHandle = jc2::Value(funcCode).get_handle();
    return jc2::Env::api->call_function(jc2::Env::ctx, evalFunc.get_handle(), 1, &codeHandle);
}

int jc2_init(jc2::Module& mod) {

    mod.register_function("to_latex", global_to_latex, 1, 1, false);
    mod.register_function("eval", global_eval, 1, 1, false);
    mod.register_function("compile", global_compile, 2, 2, false);

    mod.register_help("latex",
        "═══ LaTeX Mathematical Engine — Native Module ═══\n\n"
        "  Requires: import latex\n\n"
        "  The `latex` module provides bi-directional integration with standard LaTeX \n"
        "  math syntax. It can serialize JC2 matrices, fractions, and complex numbers \n"
        "  into beautiful LaTeX code, and conversely, it can parse, compile, and \n"
        "  evaluate raw LaTeX formulas into blazing-fast native JC2 closures.\n\n"
        "  Serialization (JC2 → LaTeX)\n"
        "  ──────────────────────\n"
        "    latex.to_latex(obj)\n"
        "        Converts a JC2 value into a clean LaTeX code string.\n"
        "        • fraction:  frac(1,2)      → \"\\frac{1}{2}\"\n"
        "        • complex:   3+4i           → \"3+4i\"\n"
        "        • matrix:    [1, 2; 3, 4]   → \"\\begin{pmatrix} 1 & 2 \\\\ ... \\end{pmatrix}\"\n"
        "        • list:      list(1, 2)     → \"\\left[ 1, 2 \\right]\"\n\n"
        "  Direct Evaluation (LaTeX → Number or Complex)\n"
        "  ──────────────────────\n"
        "    latex.eval(\"formula\")\n"
        "        Parses and evaluates a constant LaTeX math string directly.\n"
        "        The underlying engine fully supports the Complex Plane.\n"
        "        (Tip: Use r-strings `r\"...\"` so you don't have to double-escape backslashes!)\n"
        "        \n"
        "        latex.eval(r\"\\frac{1+\\sqrt{5}}{2}\")       → 1.618033\n"
        "        latex.eval(r\"\\frac{1+i}{2}\")              → 0.5 + 0.5i\n"
        "        latex.eval(r\"e^{i \\pi} + 1\")              → 0 + 0i  (Euler's Identity!)\n\n"
        "        // Multi-line matrices require raw multi-line strings (r\"\"\" ... \"\"\")\n"
        "        latex.eval(r\"\"\"\n"
        "        \\begin{pmatrix}\n"
        "            1 & 2 \\\\\n"
        "            3 & 4\n"
        "        \\end{pmatrix}\n"
        "        \"\"\")\n\n"
        "  JIT Compilation to JC2 Closures (LaTeX → JC2 Function)\n"
        "  ──────────────────────\n"
        "    latex.compile(\"formula\", variable_names)\n"
        "        Dynamically compiles a parameterized LaTeX formula into an executable \n"
        "        JC2 abstract syntax tree, returning a true JC2 function closure.\n"
        "        `variable_names` must be a list or stringmatrix.\n\n"
        "        // 1. Compile the LaTeX math into a native callable function f(x, \\theta)\n"
        "        f = latex.compile(r\"\\frac{\\sin(\\theta)}{x^2}\", [\"x\", r\"\\theta\"])\n"
        "        \n"
        "        // 2. Call it interactively! (Supports real and complex arguments)\n"
        "        f(2.0, PI/2)                 → 0.25\n"
        "        \n"
        "        // 3. Works seamlessly with all higher-order and calculus functions!\n"
        "        table(f, [1, 2, 3], fill(PI/2, 3)|>trans)    → Tabulates results over a vector\n"
        "        diff(f(_, PI/2), 2.0)        → Derivative w.r.t 'x' at x=2.0\n\n"
        "  Supported LaTeX Syntax (Parser Engine)\n"
        "  ──────────────────────\n"
        "    • Operations:   +, -, *, /, ^\n"
        "    • Grouping:     { }, ( ), [ ]\n"
        "    • Fractions:    \\frac{numerator}{denominator}\n"
        "    • Functions:    \\sin, \\cos, \\tan, \\sqrt, \\ln, \\log, \\exp, \\abs\n"
        "    • Constants:    \\pi, e, i, j   (i and j are intrinsically parsed as 0+1i)\n"
        "    \n"
        "    ★ Advanced Feature: Implicit Multiplication\n"
        "      The recursive descent parser fully understands implicit mathematical \n"
        "      multiplication just like a human reading a paper. \n"
        "      Expressions like \"2i\", \"xy\", and \"2 \\sin(x)\" act identically to \n"
        "      \"2*i\", \"x*y\", and \"2 * \\sin(x)\"."
    );

    mod.register_function_help("latex.to_latex", "latex.to_latex(obj)", "Serializes a JC2 value (matrix, fraction, complex) into LaTeX code.", "latex.to_latex(frac(1, 2))");
    mod.register_function_help("latex.eval", "latex.eval(formula)", "Parses and evaluates a LaTeX math string directly.", "latex.eval(r\"\\frac{1}{2}\")");
    mod.register_function_help("latex.compile", "latex.compile(formula, vars)", "Compiles a parameterized LaTeX formula into an executable JC2 closure.", "latex.compile(r\"\\sin(x)\", [\"x\"])");

    return 0;
}

JC2_EXTENSION_INIT
