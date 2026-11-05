#pragma once

#include "../cas/Symbolic.h"
#include "../memory/Value.h"
#include <string>
#include <vector>

namespace jc {

class VM;

// Lower a symbolic expression directly to a JC2 bytecode closure.
// This path deliberately does not create Tokens or AST nodes: SymNode maps
// straight to IR nodes, then goes through the normal optimizer/register
// allocator/emitter pipeline.
Value compileSymbolicFunction(VM& vm, const SymExpr& expr,
                              const std::vector<std::string>& varNames);

} // namespace jc
