#ifndef JC2_SYMEVAL_H
#define JC2_SYMEVAL_H

#include "Symbolic.h"
#include "../memory/Value.h"
#include <map>
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace jc {
    using SymbolicFuncResolver = std::function<Value(const std::string&, const std::vector<Value>&)>;

    double fastEval(SymNode* node, const std::map<std::string, double>& env, const SymbolicFuncResolver& resolver = nullptr);
    Value evalUniversal(SymNode* node, const std::map<std::string, Value>& env, const SymbolicFuncResolver& resolver = nullptr);
}

#endif // JC2_SYMEVAL_H
