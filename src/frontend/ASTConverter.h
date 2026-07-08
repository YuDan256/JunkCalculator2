#ifndef JC2_AST_CONVERTER_H
#define JC2_AST_CONVERTER_H

#include "Expr.h"
#include "../memory/Value.h"
#include <memory>

namespace jc {
    Value AST_to_JC2(Expr* expr);
    std::unique_ptr<Expr> JC2_to_AST(const Value& val);
}

#endif // JC2_AST_CONVERTER_H
