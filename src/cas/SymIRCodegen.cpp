#include "SymIRCodegen.h"

#include "../compiler/Emitter.h"
#include "../compiler/IR.h"
#include "../compiler/IROptimizer.h"
#include "../compiler/RegisterAllocator.h"
#include "../memory/GcHeap.h"
#include "../vm/VM.h"

#include <unordered_map>
#include <utility>

namespace jc {
namespace {

class SymbolicIRBuilder {
public:
    SymbolicIRBuilder() {
        seq_ = graph_.startNode;
        currentSeq_ = graph_.startNode;
    }

    IRGraph& graph() { return graph_; }

    void declareParameter(const std::string& name, int index) {
        IRNode* node = graph_.createValueNode(IROp::Parameter);
        node->payload1 = static_cast<uint32_t>(index);
        node->name = name;
        append(node, false);
        params_[name] = node;
    }

    IRNode* emit(SymNode* node) {
        if (!node) JC2_THROW(RuntimeError, "toFunc(): empty symbolic expression.");
        switch (node->getType()) {
        case SymType::NUM:
            return constant(casValue(static_cast<SymNum*>(node)->value));
        case SymType::VAR: {
            const std::string& name = static_cast<SymVar*>(node)->name;
            auto it = params_.find(name);
            if (it == params_.end()) {
                JC2_THROW(RuntimeError, "toFunc(): unknown variable '" + name + "'.");
            }
            return it->second;
        }
        case SymType::CONST:
            return constant(constantValue(static_cast<SymConst*>(node)->id));
        case SymType::ADD: {
            auto* add = static_cast<SymAdd*>(node);
            if (add->args.empty()) return constant(Value(0));
            IRNode* out = emit(add->args[0]);
            for (size_t i = 1; i < add->args.size(); ++i) {
                IRNode* right = emit(add->args[i]);
                out = binary(IROp::Add, out, right);
            }
            return out;
        }
        case SymType::MUL: {
            auto* mul = static_cast<SymMul*>(node);
            if (mul->args.empty()) return constant(Value(1));
            IRNode* out = emit(mul->args[0]);
            for (size_t i = 1; i < mul->args.size(); ++i) {
                IRNode* right = emit(mul->args[i]);
                out = binary(IROp::Mul, out, right);
            }
            return out;
        }
        case SymType::POW: {
            auto* pw = static_cast<SymPow*>(node);
            return binary(IROp::Pow, emit(pw->base), emit(pw->exp));
        }
        case SymType::FUNC: {
            auto* fn = static_cast<SymFunc*>(node);
            IRNode* callee = getGlobal(fn->name);
            std::vector<IRNode*> args;
            args.reserve(fn->args.size());
            for (auto* arg : fn->args) args.push_back(emit(arg));
            return call(callee, args);
        }
        }
        JC2_THROW(RuntimeError, "toFunc(): unsupported symbolic node.");
    }

    // Finish the straight-line function with a return of `value`.
    void finish(IRNode* value) {
        IRNode* ret = graph_.createNode(IROp::Return);
        append(ret, true);
        ret->addData(value);
    }

    void optimizeAndEmit(Chunk& chunk, int& localCount) {
        IROptimizer::optimize(&graph_);
        RegisterAllocator::allocate(&graph_);
        localCount = Emitter::emit(&graph_, chunk);
    }

private:
    IRGraph graph_;
    IRNode* seq_ = nullptr;
    IRNode* currentSeq_ = nullptr;
    std::unordered_map<std::string, IRNode*> params_;

    IRNode* append(IRNode* node, bool effect) {
        node->seqPrev = seq_;
        seq_ = node;
        node->setControl(currentSeq_);
        if (effect) currentSeq_ = node;
        return node;
    }

    IRNode* constant(const Value& value) {
        IRNode* node = graph_.createConstant(value);
        append(node, false);
        return node;
    }

    IRNode* getGlobal(const std::string& name) {
        IRNode* node = graph_.createValueNode(IROp::GetGlobal);
        node->name = name;
        append(node, true);
        return node;
    }

    IRNode* binary(IROp op, IRNode* left, IRNode* right) {
        IRNode* node = graph_.createValueNode(op);
        append(node, false);
        node->addData(left);
        node->addData(right);
        return node;
    }

    IRNode* call(IRNode* callee, const std::vector<IRNode*>& args) {
        IRNode* node = graph_.createValueNode(IROp::Call);
        append(node, true);
        node->addData(callee);
        for (IRNode* arg : args) node->addData(arg);
        node->payload1 = static_cast<uint32_t>(args.size());
        node->payload2 = 0;
        return node;
    }

    static Value casValue(const CASVal& value) {
        if (std::holds_alternative<int32_t>(value)) return Value(std::get<int32_t>(value));
        if (std::holds_alternative<BigInt>(value)) return Value(std::get<BigInt>(value));
        if (std::holds_alternative<Fraction>(value)) return Value(std::get<Fraction>(value));
        return Value(std::get<double>(value));
    }

    static Value constantValue(SymConstId id) {
        switch (id) {
        case SymConstId::Pi: return Value(3.14159265358979323846);
        case SymConstId::E: return Value(2.71828182845904523536);
        case SymConstId::I: return Value(Complex(0.0, 1.0));
        }
        return Value::none();
    }
};

} // namespace

Value compileSymbolicFunction(VM& vm, const SymExpr& expr,
                              const std::vector<std::string>& varNames) {
    if (!expr.ptr) JC2_THROW(RuntimeError, "toFunc(): expected a symbolic expression.");

    SymbolicIRBuilder builder;
    for (size_t i = 0; i < varNames.size(); ++i) {
        builder.declareParameter(varNames[i], static_cast<int>(i));
    }
    IRNode* result = builder.emit(expr.ptr);
    builder.finish(result);

    auto fn = std::make_shared<CompiledFunction>();
    fn->name = "<sym_to_func>";
    fn->paramNames = varNames;
    fn->arity = static_cast<int>(varNames.size());
    fn->maxArity = fn->arity;
    fn->paramIsRef.assign(varNames.size(), false);
    fn->paramIsConst.assign(varNames.size(), false);

    int localCount = 0;
    Chunk chunk;
    builder.optimizeAndEmit(chunk, localCount);
    fn->chunk = std::move(chunk);
    fn->localCount = localCount;

    auto& functions = vm.getCompiledFunctions();
    const int fnIndex = static_cast<int>(functions.size());
    functions.push_back(fn);

    std::vector<bool> refs(varNames.size(), false);
    auto* closure = GcHeap::get().allocate<ObjClosure>(
        varNames, refs, "<sym_to_func>", nullptr);
    closure->compiledFnIndex = fnIndex;
    closure->owner_class = nullptr;
    return Value(closure);
}

} // namespace jc
