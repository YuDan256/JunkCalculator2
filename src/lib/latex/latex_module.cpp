#include "../jc2_extension_cpp.h"

#include <cctype>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

static jc2::Class* g_latexErrorClass = nullptr;

[[noreturn]] inline void throwLatexError(const std::string& msg) {
    jc2::throw_error_class(*g_latexErrorClass, msg);
}

// ─────────────────────────────────────────────────────────────────────────────
// AST
// ─────────────────────────────────────────────────────────────────────────────
namespace latex {

struct Expr {
    virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

struct NumberExpr : Expr {
    std::string text;
    explicit NumberExpr(std::string t) : text(std::move(t)) {}
};

struct SymbolExpr : Expr {
    std::string name;
    explicit SymbolExpr(std::string n) : name(std::move(n)) {}
};

struct UnaryExpr : Expr {
    std::string op;
    ExprPtr child;
    UnaryExpr(std::string o, ExprPtr c) : op(std::move(o)), child(std::move(c)) {}
};

struct BinaryExpr : Expr {
    std::string op;
    ExprPtr left;
    ExprPtr right;
    BinaryExpr(std::string o, ExprPtr l, ExprPtr r)
        : op(std::move(o)), left(std::move(l)), right(std::move(r)) {}
};

struct FractionExpr : Expr {
    ExprPtr num;
    ExprPtr den;
    FractionExpr(ExprPtr n, ExprPtr d) : num(std::move(n)), den(std::move(d)) {}
};

struct RootExpr : Expr {
    ExprPtr index;
    ExprPtr body;
    RootExpr(ExprPtr i, ExprPtr b) : index(std::move(i)), body(std::move(b)) {}
};

struct FunctionExpr : Expr {
    std::string name;
    ExprPtr arg;
    FunctionExpr(std::string n, ExprPtr a) : name(std::move(n)), arg(std::move(a)) {}
};

struct MatrixExpr : Expr {
    std::string env;
    std::vector<std::vector<ExprPtr>> rows;
    explicit MatrixExpr(std::string e) : env(std::move(e)) {}
};

struct ParseError : std::runtime_error {
    int line;
    int column;
    ParseError(std::string msg, int l, int c)
        : std::runtime_error(std::move(msg)), line(l), column(c) {}
};

struct LatexError : std::runtime_error {
    explicit LatexError(std::string msg) : std::runtime_error(std::move(msg)) {}
};

} // namespace latex

// ─────────────────────────────────────────────────────────────────────────────
// Lexer
// ─────────────────────────────────────────────────────────────────────────────
namespace latex {

enum class Tok {
    Number, Ident, Command,
    LBrace, RBrace, LParen, RParen, LBracket, RBracket,
    Plus, Minus, Star, Slash, Caret, Underscore,
    Amp, RowBreak, Comma, Equals, Less, Greater,
    End
};

struct Token {
    Tok type;
    std::string text;
    int line = 1;
    int column = 1;
};

class Lexer {
public:
    explicit Lexer(const std::string& source) : src(source) {}

    std::vector<Token> scan() {
        std::vector<Token> out;
        while (!atEnd()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(); continue; }
            if (c == '%') {
                while (!atEnd() && peek() != '\n') advance();
                continue;
            }
            int line = curLine;
            int col = curCol;
            if (c == '\\') {
                advance();
                if (atEnd()) throw ParseError("dangling '\\'", line, col);
                if (peek() == '\\') {
                    advance();
                    out.push_back({Tok::RowBreak, "\\\\", line, col});
                    continue;
                }
                std::string name = "\\";
                if (std::isalpha(static_cast<unsigned char>(peek()))) {
                    while (!atEnd() && std::isalpha(static_cast<unsigned char>(peek()))) {
                        name += advance();
                    }
                } else {
                    name += advance();
                }
                out.push_back({Tok::Command, name, line, col});
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '.' && pos + 1 < src.size() &&
                 std::isdigit(static_cast<unsigned char>(src[pos + 1])))) {
                out.push_back({Tok::Number, scanNumber(), line, col});
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c))) {
                // A single math variable. \mathrm{...} is used for long names.
                out.push_back({Tok::Ident, std::string(1, advance()), line, col});
                continue;
            }
            advance();
            switch (c) {
                case '{': out.push_back({Tok::LBrace, "{", line, col}); break;
                case '}': out.push_back({Tok::RBrace, "}", line, col}); break;
                case '(': out.push_back({Tok::LParen, "(", line, col}); break;
                case ')': out.push_back({Tok::RParen, ")", line, col}); break;
                case '[': out.push_back({Tok::LBracket, "[", line, col}); break;
                case ']': out.push_back({Tok::RBracket, "]", line, col}); break;
                case '+': out.push_back({Tok::Plus, "+", line, col}); break;
                case '-': out.push_back({Tok::Minus, "-", line, col}); break;
                case '*': out.push_back({Tok::Star, "*", line, col}); break;
                case '/': out.push_back({Tok::Slash, "/", line, col}); break;
                case '^': out.push_back({Tok::Caret, "^", line, col}); break;
                case '_': out.push_back({Tok::Underscore, "_", line, col}); break;
                case '&': out.push_back({Tok::Amp, "&", line, col}); break;
                case ',': out.push_back({Tok::Comma, ",", line, col}); break;
                case '=': out.push_back({Tok::Equals, "=", line, col}); break;
                case '<': out.push_back({Tok::Less, "<", line, col}); break;
                case '>': out.push_back({Tok::Greater, ">", line, col}); break;
                default:
                    throw ParseError(std::string("unexpected character '") + c + "'", line, col);
            }
        }
        out.push_back({Tok::End, "", curLine, curCol});
        return out;
    }

private:
    const std::string& src;
    size_t pos = 0;
    int curLine = 1;
    int curCol = 1;

    bool atEnd() const { return pos >= src.size(); }
    char peek() const { return src[pos]; }

    char advance() {
        char c = src[pos++];
        if (c == '\n') { ++curLine; curCol = 1; }
        else { ++curCol; }
        return c;
    }

    std::string scanNumber() {
        size_t start = pos;
        bool dot = false;
        while (!atEnd()) {
            char c = peek();
            if (std::isdigit(static_cast<unsigned char>(c))) { advance(); continue; }
            if (c == '.' && !dot) { dot = true; advance(); continue; }
            break;
        }
        // Accept scientific notation only when it is a complete exponent.
        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            size_t save = pos;
            int saveLine = curLine;
            int saveCol = curCol;
            advance();
            if (!atEnd() && (peek() == '+' || peek() == '-')) advance();
            size_t digits = pos;
            while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
            if (pos == digits) {
                pos = save;
                curLine = saveLine;
                curCol = saveCol;
            }
        }
        return src.substr(start, pos - start);
    }
};

} // namespace latex

// ─────────────────────────────────────────────────────────────────────────────
// Pratt parser
// ─────────────────────────────────────────────────────────────────────────────
namespace latex {

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : tokens(std::move(toks)) {}

    ExprPtr parseAll() {
        ExprPtr expr = parseExpr(0);
        if (!check(Tok::End)) {
            const Token& t = peek();
            throw ParseError("unexpected trailing token '" + t.text + "'", t.line, t.column);
        }
        return expr;
    }

    ExprPtr parseExpr(int minPrec) {
        ExprPtr left = parsePrefix();
        for (;;) {
            int prec = precedence();
            if (prec <= minPrec) break;
            bool implicit = startsPrimary(peek().type, peek().text);
            Token op;
            if (implicit) {
                op = Token{Tok::Star, "*", peek().line, peek().column};
            } else {
                op = advance();
            }
            ExprPtr right = parseExpr(prec + (op.type == Tok::Caret ? -1 : 0));
            if (op.type == Tok::Caret) {
                left = std::make_unique<BinaryExpr>("^", std::move(left), std::move(right));
            } else if (op.type == Tok::Underscore) {
                left = std::make_unique<BinaryExpr>("_", std::move(left), std::move(right));
            } else {
                left = std::make_unique<BinaryExpr>(op.text, std::move(left), std::move(right));
            }
        }
        return left;
    }

private:
    std::vector<Token> tokens;
    size_t pos = 0;

    const Token& peek(size_t n = 0) const {
        static Token eof{Tok::End, "", 0, 0};
        size_t i = pos + n;
        return i < tokens.size() ? tokens[i] : eof;
    }
    bool check(Tok t) const { return peek().type == t; }
    Token advance() { return tokens[pos++]; }
    bool match(Tok t) { if (!check(t)) return false; ++pos; return true; }

    void expect(Tok t, const std::string& msg) {
        if (!check(t)) {
            const Token& tok = peek();
            throw ParseError(msg + " (got '" + tok.text + "')", tok.line, tok.column);
        }
        ++pos;
    }

    static bool startsPrimary(Tok t, const std::string& text = "") {
        if (t == Tok::Command) {
            if (text == "\\end" || text == "\\right" ||
                text == "\\cdot" || text == "\\times" ||
                text == "\\le" || text == "\\ge" || text == "\\ne" ||
                text == "\\approx" || text == "\\equiv") {
                return false;
            }
            return true;
        }
        return t == Tok::Number || t == Tok::Ident ||
               t == Tok::LParen || t == Tok::LBracket || t == Tok::LBrace;
    }

    bool isFunctionCommand(const std::string& c) const {
        static const std::unordered_map<std::string, std::string> funcs = {
            {"\\sin", "sin"}, {"\\cos", "cos"}, {"\\tan", "tan"},
            {"\\cot", "cot"}, {"\\sec", "sec"}, {"\\csc", "csc"},
            {"\\arcsin", "asin"}, {"\\arccos", "acos"}, {"\\arctan", "atan"},
            {"\\sinh", "sinh"}, {"\\cosh", "cosh"}, {"\\tanh", "tanh"},
            {"\\ln", "log"}, {"\\log", "log"}, {"\\exp", "exp"},
            {"\\abs", "abs"}, {"\\min", "min"}, {"\\max", "max"},
            {"\\floor", "floor"}, {"\\ceil", "ceil"}
        };
        return funcs.count(c) != 0;
    }

    std::string functionName(const std::string& c) const {
        static const std::unordered_map<std::string, std::string> funcs = {
            {"\\sin", "sin"}, {"\\cos", "cos"}, {"\\tan", "tan"},
            {"\\cot", "cot"}, {"\\sec", "sec"}, {"\\csc", "csc"},
            {"\\arcsin", "asin"}, {"\\arccos", "acos"}, {"\\arctan", "atan"},
            {"\\sinh", "sinh"}, {"\\cosh", "cosh"}, {"\\tanh", "tanh"},
            {"\\ln", "log"}, {"\\log", "log"}, {"\\exp", "exp"},
            {"\\abs", "abs"}, {"\\min", "min"}, {"\\max", "max"},
            {"\\floor", "floor"}, {"\\ceil", "ceil"}
        };
        auto it = funcs.find(c);
        return it == funcs.end() ? c.substr(1) : it->second;
    }

    int precedence() const {
        const Token& t = peek();
        switch (t.type) {
            case Tok::Equals: case Tok::Less: case Tok::Greater: return 10;
            case Tok::Plus: case Tok::Minus: return 20;
            case Tok::Star: case Tok::Slash: return 30;
            case Tok::Caret: case Tok::Underscore: return 40;
            case Tok::Command:
                if (t.text == "\\cdot" || t.text == "\\times") return 30;
                if (t.text == "\\le" || t.text == "\\ge" || t.text == "\\ne" ||
                    t.text == "\\approx" || t.text == "\\equiv") return 10;
                break;
            default: break;
        }
        // Implicit multiplication: 2x, 2\sin(x), xy.
        if (startsPrimary(t.type, t.text)) return 30;
        return 0;
    }

    ExprPtr parsePrefix() {
        if (match(Tok::Plus)) return parseExpr(35);
        if (match(Tok::Minus)) {
            return std::make_unique<UnaryExpr>("-", parseExpr(35));
        }
        return parsePrimary();
    }

    ExprPtr parsePrimary() {
        const Token& t = peek();
        if (t.type == Tok::Number) {
            advance();
            return std::make_unique<NumberExpr>(t.text);
        }
        if (t.type == Tok::Ident) {
            advance();
            return std::make_unique<SymbolExpr>(t.text);
        }
        if (t.type == Tok::LParen || t.type == Tok::LBracket) {
            Tok close = (t.type == Tok::LParen) ? Tok::RParen : Tok::RBracket;
            advance();
            ExprPtr inner = parseExpr(0);
            expect(close, "missing closing delimiter");
            return inner;
        }
        if (t.type == Tok::LBrace) {
            advance();
            ExprPtr inner = parseExpr(0);
            expect(Tok::RBrace, "missing closing '}'");
            return inner;
        }
        if (t.type != Tok::Command) {
            throw ParseError("expected an expression, got '" + t.text + "'", t.line, t.column);
        }

        std::string cmd = t.text;
        advance();
        if (cmd == "\\frac") {
            ExprPtr num = parseBracedArg("\\frac numerator");
            ExprPtr den = parseBracedArg("\\frac denominator");
            return std::make_unique<FractionExpr>(std::move(num), std::move(den));
        }
        if (cmd == "\\sqrt") {
            ExprPtr index;
            if (match(Tok::LBracket)) {
                index = parseExpr(0);
                expect(Tok::RBracket, "missing closing ']' after root index");
            }
            ExprPtr body = parseBracedArg("\\sqrt body");
            return std::make_unique<RootExpr>(std::move(index), std::move(body));
        }
        if (isFunctionCommand(cmd)) {
            ExprPtr arg;
            if (check(Tok::LParen)) {
                advance();
                arg = parseExpr(0);
                expect(Tok::RParen, "missing ')' after " + cmd);
            } else if (check(Tok::LBrace)) {
                arg = parseBracedArg(cmd + " argument");
            } else {
                arg = parseExpr(35);
            }
            return std::make_unique<FunctionExpr>(functionName(cmd), std::move(arg));
        }
        if (cmd == "\\mathrm" || cmd == "\\operatorname") {
            expect(Tok::LBrace, "expected '{' after " + cmd);
            std::string name;
            while (!check(Tok::RBrace) && !check(Tok::End)) {
                const Token& part = advance();
                if (part.type != Tok::Ident && part.type != Tok::Number) {
                    throw ParseError("invalid name in " + cmd, part.line, part.column);
                }
                name += part.text;
            }
            expect(Tok::RBrace, "missing '}' after " + cmd);
            if (name.empty()) throw ParseError("empty name in " + cmd, t.line, t.column);
            return std::make_unique<SymbolExpr>(name);
        }
        if (cmd == "\\left") {
            if (check(Tok::End)) throw ParseError("expected delimiter after \\left", t.line, t.column);
            advance(); // delimiter
            ExprPtr inner = parseExpr(0);
            if (!check(Tok::Command) || peek().text != "\\right") {
                throw ParseError("missing \\right", t.line, t.column);
            }
            advance();
            if (!check(Tok::End)) advance(); // closing delimiter
            return inner;
        }
        if (cmd == "\\right") {
            throw ParseError("\\right without matching \\left", t.line, t.column);
        }
        if (cmd == "\\begin") {
            std::string env = readEnvName("\\begin");
            return parseMatrix(env, t.line, t.column);
        }
        if (cmd == "\\pi") return std::make_unique<SymbolExpr>("pi");
        if (cmd == "\\infty") throw ParseError("\\infty is not supported in evaluable formulas", t.line, t.column);
        static const std::unordered_map<std::string, std::string> symbols = {
            {"\\alpha", "alpha"}, {"\\beta", "beta"}, {"\\gamma", "gamma"},
            {"\\delta", "delta"}, {"\\epsilon", "epsilon"}, {"\\varepsilon", "varepsilon"},
            {"\\zeta", "zeta"}, {"\\eta", "eta"}, {"\\theta", "theta"},
            {"\\vartheta", "vartheta"}, {"\\iota", "iota"}, {"\\kappa", "kappa"},
            {"\\lambda", "lambda"}, {"\\mu", "mu"}, {"\\nu", "nu"},
            {"\\xi", "xi"}, {"\\rho", "rho"}, {"\\sigma", "sigma"},
            {"\\tau", "tau"}, {"\\phi", "phi"}, {"\\varphi", "varphi"},
            {"\\chi", "chi"}, {"\\psi", "psi"}, {"\\omega", "omega"},
            {"\\Gamma", "Gamma"}, {"\\Delta", "Delta"}, {"\\Theta", "Theta"},
            {"\\Lambda", "Lambda"}, {"\\Xi", "Xi"}, {"\\Pi", "Pi"},
            {"\\Sigma", "Sigma"}, {"\\Phi", "Phi"}, {"\\Psi", "Psi"},
            {"\\Omega", "Omega"}
        };
        auto sit = symbols.find(cmd);
        if (sit != symbols.end()) return std::make_unique<SymbolExpr>(sit->second);
        throw ParseError("unknown LaTeX command '" + cmd + "'", t.line, t.column);
    }

    ExprPtr parseBracedArg(const std::string& what) {
        expect(Tok::LBrace, "expected '{' for " + what);
        ExprPtr e = parseExpr(0);
        expect(Tok::RBrace, "missing '}' for " + what);
        return e;
    }

    std::string readEnvName(const std::string& cmd) {
        expect(Tok::LBrace, "expected '{' after " + cmd);
        std::string name;
        while (!check(Tok::RBrace) && !check(Tok::End)) {
            const Token& part = advance();
            if (part.type != Tok::Ident) {
                throw ParseError("invalid environment name", part.line, part.column);
            }
            name += part.text;
        }
        expect(Tok::RBrace, "missing '}' after environment name");
        return name;
    }

    ExprPtr parseMatrix(const std::string& env, int line, int col) {
        auto matrix = std::make_unique<MatrixExpr>(env);
        std::vector<ExprPtr> row;
        for (;;) {
            if (check(Tok::End)) {
                throw ParseError("missing \\end{" + env + "}", line, col);
            }
            if (check(Tok::Command) && peek().text == "\\end") {
                advance();
                std::string endName = readEnvName("\\end");
                if (endName != env) {
                    throw ParseError("\\begin{" + env + "} closed by \\end{" + endName + "}", line, col);
                }
                if (!row.empty()) matrix->rows.push_back(std::move(row));
                break;
            }
            if (check(Tok::RowBreak)) {
                advance();
                if (row.empty()) throw ParseError("empty matrix row", line, col);
                matrix->rows.push_back(std::move(row));
                row.clear();
                continue;
            }
            row.push_back(parseExpr(0));
            if (check(Tok::Amp)) { advance(); continue; }
            if (check(Tok::RowBreak)) continue;
            if (check(Tok::Command) && peek().text == "\\end") continue;
            const Token& t = peek();
            throw ParseError("expected '&', '\\\\' or \\end in matrix", t.line, t.column);
        }
        if (matrix->rows.empty()) throw ParseError("empty matrix", line, col);
        for (const auto& r : matrix->rows) {
            if (r.empty()) throw ParseError("empty matrix row", line, col);
        }
        return matrix;
    }
};

} // namespace latex

// ─────────────────────────────────────────────────────────────────────────────
// Validator
// ─────────────────────────────────────────────────────────────────────────────
namespace latex {

static bool isIdentifier(const std::string& s) {
    if (s.empty()) return false;
    unsigned char c = static_cast<unsigned char>(s[0]);
    if (!(std::isalpha(c) || c == '_')) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        c = static_cast<unsigned char>(s[i]);
        if (!(std::isalnum(c) || c == '_')) return false;
    }
    return s[0] != '<';
}

static bool isKnownConstant(const std::string& s) {
    return s == "pi" || s == "e" || s == "i" || s == "j";
}

static void validate(const Expr& e, const std::vector<std::string>& vars) {
    if (auto* n = dynamic_cast<const SymbolExpr*>(&e)) {
        if (isKnownConstant(n->name)) return;
        for (const auto& v : vars) if (v == n->name) return;
        throw LatexError("unknown symbol '" + n->name + "'");
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) { validate(*u->child, vars); return; }
    if (auto* b = dynamic_cast<const BinaryExpr*>(&e)) {
        validate(*b->left, vars); validate(*b->right, vars); return;
    }
    if (auto* f = dynamic_cast<const FractionExpr*>(&e)) {
        validate(*f->num, vars); validate(*f->den, vars); return;
    }
    if (auto* r = dynamic_cast<const RootExpr*>(&e)) {
        if (r->index) validate(*r->index, vars);
        validate(*r->body, vars); return;
    }
    if (auto* f = dynamic_cast<const FunctionExpr*>(&e)) { validate(*f->arg, vars); return; }
    if (auto* m = dynamic_cast<const MatrixExpr*>(&e)) {
        for (const auto& row : m->rows) for (const auto& cell : row) validate(*cell, vars);
        return;
    }
}

} // namespace latex

// ─────────────────────────────────────────────────────────────────────────────
// JC2 lowering
// ─────────────────────────────────────────────────────────────────────────────
namespace latex {

static std::string lower(const Expr& e);

static std::string lowerSymbol(const std::string& name) {
    if (name == "pi") return "math.pi()";
    if (name == "e") return "math.e()";
    if (name == "i" || name == "j") return "1i";
    if (!isIdentifier(name)) throw LatexError("invalid variable name '" + name + "'");
    return name;
}

static std::string lower(const Expr& e) {
    if (auto* n = dynamic_cast<const NumberExpr*>(&e)) return n->text;
    if (auto* s = dynamic_cast<const SymbolExpr*>(&e)) return lowerSymbol(s->name);
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
        return "(-(" + lower(*u->child) + "))";
    }
    if (auto* b = dynamic_cast<const BinaryExpr*>(&e)) {
        std::string op = b->op;
        if (op == "=") op = "==";
        else if (op == "\\le") op = "<=";
        else if (op == "\\ge") op = ">=";
        else if (op == "\\ne") op = "!=";
        else if (op == "\\approx" || op == "\\equiv") op = "==";
        else if (op == "_") {
            // x_1 is a legal JC2 identifier; keep it as one symbol.
            auto* ls = dynamic_cast<const SymbolExpr*>(b->left.get());
            auto* rs = dynamic_cast<const NumberExpr*>(b->right.get());
            if (!ls || !rs) throw LatexError("subscript requires a symbol and a number");
            return ls->name + "_" + rs->text;
        }
        return "(" + lower(*b->left) + " " + op + " " + lower(*b->right) + ")";
    }
    if (auto* f = dynamic_cast<const FractionExpr*>(&e)) {
        return "((" + lower(*f->num) + ") / (" + lower(*f->den) + "))";
    }
    if (auto* r = dynamic_cast<const RootExpr*>(&e)) {
        if (!r->index) return "sqrt(" + lower(*r->body) + ")";
        return "root(" + lower(*r->body) + ", " + lower(*r->index) + ")";
    }
    if (auto* f = dynamic_cast<const FunctionExpr*>(&e)) {
        return f->name + "(" + lower(*f->arg) + ")";
    }
    if (auto* m = dynamic_cast<const MatrixExpr*>(&e)) {
        if (m->rows.empty()) throw LatexError("empty matrix");
        size_t cols = m->rows[0].size();
        std::string out = "[";
        for (size_t i = 0; i < m->rows.size(); ++i) {
            if (m->rows[i].size() != cols) throw LatexError("matrix rows have different lengths");
            for (size_t j = 0; j < m->rows[i].size(); ++j) {
                if (j) out += ", ";
                out += lower(*m->rows[i][j]);
            }
            if (i + 1 < m->rows.size()) out += "; ";
        }
        return out + "]";
    }
    throw LatexError("unsupported expression");
}

static void collectSymbols(const Expr& e, std::vector<std::string>& out) {
    if (auto* s = dynamic_cast<const SymbolExpr*>(&e)) {
        if (!isKnownConstant(s->name)) out.push_back(s->name);
        return;
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) { collectSymbols(*u->child, out); return; }
    if (auto* b = dynamic_cast<const BinaryExpr*>(&e)) {
        collectSymbols(*b->left, out); collectSymbols(*b->right, out); return;
    }
    if (auto* f = dynamic_cast<const FractionExpr*>(&e)) {
        collectSymbols(*f->num, out); collectSymbols(*f->den, out); return;
    }
    if (auto* r = dynamic_cast<const RootExpr*>(&e)) {
        if (r->index) collectSymbols(*r->index, out);
        collectSymbols(*r->body, out); return;
    }
    if (auto* f = dynamic_cast<const FunctionExpr*>(&e)) { collectSymbols(*f->arg, out); return; }
    if (auto* m = dynamic_cast<const MatrixExpr*>(&e)) {
        for (const auto& row : m->rows) for (const auto& cell : row) collectSymbols(*cell, out);
    }
}

} // namespace latex

// ─────────────────────────────────────────────────────────────────────────────
// Renderer
// ─────────────────────────────────────────────────────────────────────────────
namespace latex {

static std::string formatDouble(double v) {
    if (std::isnan(v)) return "\\mathrm{NaN}";
    if (std::isinf(v)) return v < 0 ? "-\\infty" : "\\infty";
    if (v == 0.0) return "0";
    std::ostringstream oss;
    oss << std::setprecision(15) << std::defaultfloat << v;
    std::string s = oss.str();
    // Avoid ugly trailing zeroes from defaultfloat.
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

static std::string renderSymbolic(const jc2::Value& val, int parentPrecedence = 0);

static std::string renderValue(const jc2::Value& val) {
    if (val.is_symbolic()) return renderSymbolic(val);
    if (val.is_int()) return std::to_string(val.as_int());
    if (val.is_bigint()) return jc2::BigInt(val.get_handle()).to_string();
    if (val.is_float()) return formatDouble(val.as_float());
    if (val.is_fraction()) {
        jc2::Fraction f(val.get_handle());
        std::string num = renderValue(f.num());
        std::string den = renderValue(f.den());
        if (num.rfind('-', 0) == 0) return "-\\frac{" + num.substr(1) + "}{" + den + "}";
        return "\\frac{" + num + "}{" + den + "}";
    }
    if (val.is_complex()) {
        jc2::Complex complexValue(val.get_handle());
        double re = complexValue.real();
        double im = complexValue.imag();
        if (std::abs(im) < 1e-15) return formatDouble(re);
        std::string out;
        if (std::abs(re) >= 1e-15) out += formatDouble(re);
        if (im > 0 && !out.empty()) out += "+";
        if (std::abs(im - 1.0) < 1e-15) out += "i";
        else if (std::abs(im + 1.0) < 1e-15) out += "-i";
        else out += formatDouble(im) + "i";
        return out;
    }
    if (val.is_real_matrix()) {
        jc2::RealMatrix m(val.get_handle());
        std::ostringstream oss;
        oss << "\\begin{pmatrix}\n";
        for (int i = 0; i < m.rows(); ++i) {
            for (int j = 0; j < m.cols(); ++j) {
                if (j) oss << " & ";
                oss << formatDouble(m.get(i, j));
            }
            oss << (i + 1 < m.rows() ? " \\\\\n" : "\n");
        }
        oss << "\\end{pmatrix}";
        return oss.str();
    }
    if (val.is_sym_matrix()) {
        jc2::SymMatrix m(val.get_handle());
        std::ostringstream oss;
        oss << "\\begin{pmatrix}\n";
        for (int i = 0; i < m.rows(); ++i) {
            for (int j = 0; j < m.cols(); ++j) {
                if (j) oss << " & ";
                oss << renderValue(m.get(i, j));
            }
            oss << (i + 1 < m.rows() ? " \\\\\n" : "\n");
        }
        oss << "\\end{pmatrix}";
        return oss.str();
    }
    if (val.is_complex_matrix()) {
        jc2::ComplexMatrix m(val.get_handle());
        std::ostringstream oss;
        oss << "\\begin{pmatrix}\n";
        for (int i = 0; i < m.rows(); ++i) {
            for (int j = 0; j < m.cols(); ++j) {
                if (j) oss << " & ";
                double re = m.get_real(i, j);
                double im = m.get_imag(i, j);
                if (std::abs(im) < 1e-15) oss << formatDouble(re);
                else {
                    std::string reS = formatDouble(re);
                    std::string imS = formatDouble(im);
                    if (std::abs(re) >= 1e-15) oss << reS << (im >= 0 ? "+" : "") << imS << "i";
                    else oss << imS << "i";
                }
            }
            oss << (i + 1 < m.rows() ? " \\\\\n" : "\n");
        }
        oss << "\\end{pmatrix}";
        return oss.str();
    }
    if (val.is_list()) {
        jc2::List l(val.get_handle());
        std::string out = "\\left[ ";
        for (size_t i = 0; i < l.size(); ++i) {
            if (i) out += ", ";
            out += renderValue(l.get(i));
        }
        return out + " \\right]";
    }
    if (val.is_string()) {
        std::string s = val.as_string();
        std::string out = "\\text{";
        for (char c : s) {
            if (c == '\\' || c == '{' || c == '}') out += '\\';
            out += c;
        }
        return out + "}";
    }
    return val.to_string();
}

static int symbolicPrecedence(int kind) {
    switch (kind) {
        case JC2_SYM_ADD: return 10;
        case JC2_SYM_MUL: return 20;
        case JC2_SYM_POW: return 30;
        default: return 100;
    }
}

static std::string escapeSymbolicName(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (c == '_' || c == '%' || c == '#' || c == '{' || c == '}') out += '\\';
        out += c;
    }
    return out;
}

static std::string renderSymbolicFunction(const jc2::Value& val) {
    const std::string name = val.symbolic_name();
    const int count = val.symbolic_child_count();
    if (name == "exp" && count == 1) {
        return "e^{" + renderSymbolic(val.symbolic_child(0)) + "}";
    }
    if (name == "sqrt" && count == 1) {
        return "\\sqrt{" + renderSymbolic(val.symbolic_child(0)) + "}";
    }
    if (name == "abs" && count == 1) {
        return "\\left|" + renderSymbolic(val.symbolic_child(0)) + "\\right|";
    }
    static const std::unordered_map<std::string, std::string> known = {
        {"sin", "\\sin"}, {"cos", "\\cos"}, {"tan", "\\tan"},
        {"cot", "\\cot"}, {"sec", "\\sec"}, {"csc", "\\csc"},
        {"asin", "\\arcsin"}, {"acos", "\\arccos"}, {"atan", "\\arctan"},
        {"sinh", "\\sinh"}, {"cosh", "\\cosh"}, {"tanh", "\\tanh"},
        {"ln", "\\ln"}, {"log", "\\log"}, {"lg", "\\lg"},
        {"det", "\\det"}, {"min", "\\min"}, {"max", "\\max"},
    };
    auto it = known.find(name);
    std::string out = (it != known.end()) ? it->second
        : "\\operatorname{" + escapeSymbolicName(name) + "}";
    out += "\\left(";
    for (int i = 0; i < count; ++i) {
        if (i) out += ", ";
        out += renderSymbolic(val.symbolic_child(i));
    }
    return out + "\\right)";
}

static std::string renderSymbolic(const jc2::Value& val, int parentPrecedence) {
    const int kind = val.symbolic_kind();
    std::string body;
    switch (kind) {
        case JC2_SYM_NUM:
            body = renderValue(val.symbolic_number());
            break;
        case JC2_SYM_VAR: {
            const std::string name = val.symbolic_name();
            if (name.size() == 1 && std::isalpha(static_cast<unsigned char>(name[0]))) {
                body = name;
            } else {
                body = "\\mathrm{" + escapeSymbolicName(name) + "}";
            }
            break;
        }
        case JC2_SYM_CONST:
            switch (val.symbolic_const_id()) {
                case 0: body = "\\pi"; break;
                case 1: body = "e"; break;
                case 2: body = "i"; break;
                default: body = "?"; break;
            }
            break;
        case JC2_SYM_ADD: {
            bool first = true;
            for (int i = 0; i < val.symbolic_child_count(); ++i) {
                std::string term = renderSymbolic(val.symbolic_child(i), 11);
                if (term.empty()) continue;
                const bool negative = term[0] == '-';
                if (first) {
                    body += term;
                    first = false;
                } else if (negative) {
                    body += " - " + term.substr(1);
                } else {
                    body += " + " + term;
                }
            }
            if (first) body = "0";
            break;
        }
        case JC2_SYM_MUL:
            for (int i = 0; i < val.symbolic_child_count(); ++i) {
                const jc2::Value child = val.symbolic_child(i);
                std::string factor = renderSymbolic(child, 20);
                if (factor.empty()) continue;
                if (!body.empty()) {
                    // Adjacent numeric factors need \cdot; symbol/function
                    // factors read better with a thin space.
                    const bool prevNumeric = (i > 0 &&
                        val.symbolic_child(i - 1).symbolic_kind() == JC2_SYM_NUM);
                    const bool curNumeric = child.symbolic_kind() == JC2_SYM_NUM;
                    body += (prevNumeric && curNumeric) ? " \\cdot " : "\\,";
                }
                if (!body.empty() && factor[0] == '-') {
                    body += "\\left(" + factor + "\\right)";
                } else {
                    body += factor;
                }
            }
            if (body.empty()) body = "1";
            break;
        case JC2_SYM_POW: {
            std::string base = renderSymbolic(val.symbolic_child(0), 30);
            if (!base.empty() && base[0] == '-') base = "\\left(" + base + "\\right)";
            body = base + "^{" + renderSymbolic(val.symbolic_child(1)) + "}";
            break;
        }
        case JC2_SYM_FUNC:
            body = renderSymbolicFunction(val);
            break;
        default:
            body = val.to_string();
            break;
    }
    if (symbolicPrecedence(kind) < parentPrecedence) {
        return "\\left(" + body + "\\right)";
    }
    return body;
}

} // namespace latex

// ─────────────────────────────────────────────────────────────────────────────
// JC2 module bridge
// ─────────────────────────────────────────────────────────────────────────────
static latex::ExprPtr parseLatex(const std::string& source) {
    latex::Lexer lexer(source);
    latex::Parser parser(lexer.scan());
    try {
        return parser.parseAll();
    } catch (const latex::ParseError& e) {
        throwLatexError("latex parse error at line " + std::to_string(e.line) +
                        ", column " + std::to_string(e.column) + ": " + e.what());
    }
}

JC2_ValueHandle global_to_latex(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    try {
        jc2::Value value(argv[0]);
        return jc2::Value(latex::renderValue(value)).get_handle();
    } catch (const std::exception& e) {
        throwLatexError(std::string("latex render error: ") + e.what());
    }
}

JC2_ValueHandle global_eval(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Value arg(argv[0]);
    if (!arg.is_string()) throwLatexError("latex.eval() requires a formula string.");
    auto expr = parseLatex(arg.as_string());
    try {
        // eval is for constant formulas; free symbols need compile().
        latex::validate(*expr, {});
    } catch (const latex::LatexError& e) {
        throwLatexError(std::string("latex eval error: ") + e.what());
    }
    std::string code = "import math\n" + latex::lower(*expr);
    jc2::Value evalFunc(jc2::Env::api->get_global(jc2::Env::ctx, "eval"));
    if (!evalFunc.is_function()) throwLatexError("'eval' function not found.");
    JC2_ValueHandle h = jc2::Value(code).get_handle();
    jc2::Value res(jc2::Env::api->call_function(jc2::Env::ctx, evalFunc.get_handle(), 1, &h));
    if (res.is_complex() && jc2::Complex(res.get_handle()).imag() == 0.0) {
        return jc2::Value(jc2::Complex(res.get_handle()).real()).get_handle();
    }
    return res.get_handle();
}

JC2_ValueHandle global_compile(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Value arg0(argv[0]);
    jc2::Value arg1(argv[1]);
    if (!arg0.is_string()) throwLatexError("latex.compile() requires a formula string.");
    if (!arg1.is_list()) throwLatexError("latex.compile() second argument must be a list of variable names.");

    std::vector<std::string> vars;
    jc2::List l(arg1.get_handle());
    for (size_t i = 0; i < l.size(); ++i) {
        jc2::Value v = l.get(i);
        if (!v.is_string()) throwLatexError("latex.compile() variable names must be strings.");
        std::string name = v.as_string();
        if (!latex::isIdentifier(name)) {
            throwLatexError("latex.compile(): invalid variable name '" + name + "'");
        }
        for (const auto& old : vars) {
            if (old == name) throwLatexError("latex.compile(): duplicate variable '" + name + "'");
        }
        vars.push_back(name);
    }

    auto expr = parseLatex(arg0.as_string());
    try {
        latex::validate(*expr, vars);
    } catch (const latex::LatexError& e) {
        throwLatexError(std::string("latex compile error: ") + e.what());
    }

    std::string params;
    for (size_t i = 0; i < vars.size(); ++i) {
        if (i) params += ", ";
        params += vars[i];
    }
    std::string code = "(" + params + ") => { import math\n return " + latex::lower(*expr) + "\n}";

    jc2::Value evalFunc(jc2::Env::api->get_global(jc2::Env::ctx, "eval"));
    if (!evalFunc.is_function()) throwLatexError("'eval' function not found.");
    JC2_ValueHandle h = jc2::Value(code).get_handle();
    return jc2::Env::api->call_function(jc2::Env::ctx, evalFunc.get_handle(), 1, &h);
}

int jc2_init(jc2::Module& mod) {
    g_latexErrorClass = new jc2::Class("LatexError");
    jc2::Value exceptionCls = jc2::get_global("Exception");
    if (exceptionCls.get_handle() != 0) g_latexErrorClass->set_parent(jc2::Class(exceptionCls.get_handle()));
    mod.register_value("LatexError", *g_latexErrorClass);

    mod.register_function("to_latex", global_to_latex, 1, 1, {"obj"});
    mod.register_function("eval", global_eval, 1, 1, {"formula"});
    mod.register_function("compile", global_compile, 2, 2, {"formula", "vars"});

    mod.register_help("latex",
        "═══ LaTeX Mathematical Engine — Native Module ═══\n\n"
        "  LaTeX parses a documented mathematical subset, validates symbols,\n"
        "  lowers to JC2 source, and renders JC2 values back to LaTeX.\n\n"
        "  Serialization (JC2 → LaTeX)\n"
        "    latex.to_latex(obj)\n"
        "      Numbers, fractions, complex numbers, real/complex matrices,\n"
        "      lists and strings.\n\n"
        "  Evaluation (LaTeX → Value)\n"
        "    latex.eval(r\"\\frac{1+\\sqrt{5}}{2}\")\n"
        "    latex.eval(r\"\\begin{pmatrix} 1 & 2 \\\\ 3 & 4 \\end{pmatrix}\")\n\n"
        "  Compilation (LaTeX → JC2 closure)\n"
        "    f = latex.compile(r\"\\frac{\\sin(\\theta)}{x^2}\", [\"x\", \"theta\"])\n"
        "    f(2.0, math.PI/2)\n\n"
        "  Supported syntax\n"
        "    + - * / ^, {}, (), [], \\frac, \\sqrt, \\sqrt[n]{x}\n"
        "    \\sin \\cos \\tan \\cot \\sec \\csc \\ln \\log \\exp \\abs\n"
        "    \\min \\max \\floor \\ceil, Greek commands, \\mathrm{name}\n"
        "    pmatrix, bmatrix, vmatrix, matrix environments\n\n"
        "  Unknown commands, unmatched delimiters, non-rectangular matrices and\n"
        "  unknown symbols are hard errors.");
    mod.register_function_help("latex.to_latex", "latex.to_latex(obj)",
        "Renders a JC2 value as LaTeX.", "latex.to_latex(frac(1, 2))");
    mod.register_function_help("latex.eval", "latex.eval(formula)",
        "Parses and evaluates a constant LaTeX formula.", "latex.eval(r\"\\frac{1}{2}\")");
    mod.register_function_help("latex.compile", "latex.compile(formula, vars)",
        "Compiles a validated parameterized formula into a JC2 closure.",
        "latex.compile(r\"\\sin(x)\", [\"x\"])");

    return 0;
}

JC2_EXTENSION_INIT
