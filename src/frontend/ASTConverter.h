#ifndef JC2_AST_CONVERTER_H
#define JC2_AST_CONVERTER_H

#include "Expr.h"
#include "../memory/Value.h"
#include <memory>
#include <functional>

namespace jc {
    using MacroExpandFunc = std::function<std::unique_ptr<Expr>(const std::string&, std::vector<std::unique_ptr<Expr>>&)>;
    Value AST_to_JC2(Expr* expr);
    std::unique_ptr<Expr> JC2_to_AST(const Value& val, MacroExpandFunc expander = nullptr, int quoteDepth = 0);
}

#endif // JC2_AST_CONVERTER_H
