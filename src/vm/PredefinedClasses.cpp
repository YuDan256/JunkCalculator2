#include "PredefinedClasses.h"
#include "VM.h"
#include "../memory/GcHeap.h"
#include "../frontend/Highlight.h"
#include "../frontend/Lexer.h"
#include "../frontend/Parser.h"
#include "../frontend/ASTConverter.h"
#include <cmath>
#include <sstream>

namespace jc {

struct RangeData {
    double start;
    double end;
    double step;
    bool isInt;
    int64_t length;
};

struct RangeIterData {
    Value rangeObj;
    int64_t currentIndex;
};

void registerPredefinedClasses() {
    if (!VM::activeVM) return;

    // --- Range Class ---
    ObjClass* rangeClass = GcHeap::get().allocate<ObjClass>();
    GcObjGuard rcGuard(rangeClass);
    rangeClass->name = "Range";

    // __init__(*args)
    auto rangeInit = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"...args"}, std::vector<bool>{false}, "init", nullptr, true);
    GcObjGuard riGuard(rangeInit);
    rangeInit->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        
        double start = 0, end = 0, step = 1;
        if (args.size() == 1) {
            end = args[0].asDouble();
        } else if (args.size() == 2) {
            start = args[0].asDouble();
            end = args[1].asDouble();
        } else if (args.size() == 3) {
            start = args[0].asDouble();
            end = args[1].asDouble();
            step = args[2].asDouble();
        } else {
            throw std::runtime_error("TypeError: range expected 1 to 3 arguments.");
        }

        if (step == 0.0) throw std::runtime_error("ValueError: range() arg 3 must not be zero.");

        bool isInt = (std::floor(start) == start) && (std::floor(step) == step) && (std::floor(end) == end);
        
        int64_t len = 0;
        if (step > 0 && end > start) {
            len = static_cast<int64_t>(std::ceil((end - start) / step));
        } else if (step < 0 && end < start) {
            len = static_cast<int64_t>(std::ceil((start - end) / (-step)));
        }

        RangeData data{start, end, step, isInt, len};
        inst->nativeData = std::make_any<RangeData>(data);
        
        return self;
    });
    rangeClass->methods["init"] = rangeInit;

    // __len__()
    auto rangeLen = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__len__", nullptr);
    GcObjGuard rlGuard(rangeLen);
    rangeLen->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto& data = std::any_cast<RangeData&>(self.asInstance()->nativeData);
        return Value(BigInt(data.length));
    });
    rangeClass->methods["__len__"] = rangeLen;

    // __str__()
    auto rangeStr = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__str__", nullptr);
    GcObjGuard rsGuard(rangeStr);
    rangeStr->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto& data = std::any_cast<RangeData&>(self.asInstance()->nativeData);
        std::ostringstream oss;
        oss << "range(" << data.start << ", " << data.end << ", " << data.step << ")";
        return Value(oss.str());
    });
    rangeClass->methods["__str__"] = rangeStr;
    rangeClass->methods["__repr__"] = rangeStr;

    // __getitem__(idx)
    auto rangeGetItem = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"idx"}, std::vector<bool>{false}, "__getitem__", nullptr);
    GcObjGuard rgiGuard(rangeGetItem);
    rangeGetItem->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto& data = std::any_cast<RangeData&>(self.asInstance()->nativeData);
        int64_t idx = static_cast<int64_t>(std::round(args[0].asDouble()));
        if (idx < 0) idx += data.length;
        if (idx < 0 || idx >= data.length) throw std::out_of_range("IndexError: range object index out of range");
        
        double val = data.start + idx * data.step;
        if (data.isInt) return Value(BigInt(static_cast<int64_t>(val)));
        return Value(val);
    });
    rangeClass->methods["__getitem__"] = rangeGetItem;

    // --- RangeIterator Class ---
    ObjClass* rangeIterClass = GcHeap::get().allocate<ObjClass>();
    GcObjGuard ricGuard(rangeIterClass);
    rangeIterClass->name = "RangeIterator";
    rangeIterClass->is_native = true;

    // __iter__() for Range
    auto rangeIter = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__iter__", nullptr);
    GcObjGuard riterGuard(rangeIter);
    rangeIter->nativeFn = std::make_any<NativeCallable>([rangeIterClass](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        ObjInstance* iterInst = GcHeap::get().allocate<ObjInstance>();
        GcObjGuard guard(iterInst);
        iterInst->classDef = rangeIterClass;
        
        RangeIterData iterData{self, 0};
        iterInst->nativeData = std::make_any<RangeIterData>(iterData);
        
        iterInst->c_nativeNext = [](ObjInstance* inst) -> Value {
            auto& iterData = std::any_cast<RangeIterData&>(inst->nativeData);
            auto& rangeData = std::any_cast<RangeData&>(iterData.rangeObj.asInstance()->nativeData);
            if (iterData.currentIndex >= rangeData.length) return Value::uninit();
            double val = rangeData.start + iterData.currentIndex * rangeData.step;
            iterData.currentIndex++;
            if (rangeData.isInt) return Value(BigInt(static_cast<int64_t>(val)));
            return Value(val);
        };
        
        // 为了让 GC 追踪 rangeObj，我们把它也放到 fields 里
        iterInst->fields = GcHeap::get().allocate<ObjDict>();
        iterInst->fields->set(Value("range"), self);

        return Value(iterInst);
    });
    rangeClass->methods["__iter__"] = rangeIter;

    // __next__() for RangeIterator
    auto rangeIterNext = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__next__", nullptr);
    GcObjGuard rinGuard(rangeIterNext);
    rangeIterNext->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto& iterData = std::any_cast<RangeIterData&>(self.asInstance()->nativeData);
        auto& rangeData = std::any_cast<RangeData&>(iterData.rangeObj.asInstance()->nativeData);
        
        if (iterData.currentIndex >= rangeData.length) {
            return Value::none();
        }
        
        double val = rangeData.start + iterData.currentIndex * rangeData.step;
        iterData.currentIndex++;
        
        if (rangeData.isInt) return Value(BigInt(static_cast<int64_t>(val)));
        return Value(val);
    });
    rangeIterClass->methods["__next__"] = rangeIterNext;

    // --- ASTNode Class (For Macros) ---
    ObjClass* astNodeClass = GcHeap::get().allocate<ObjClass>();
    GcObjGuard astGuard(astNodeClass);
    astNodeClass->name = "ASTNode";

    auto astInit = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"type", "line", "props"}, std::vector<bool>{false, false, false}, "init", nullptr);
    astInit->defaultValues.push_back(Value("Unknown"));
    astInit->defaultValues.push_back(Value::fromInt32(0));
    astInit->defaultValues.push_back(Value::none());
    GcObjGuard astInitGuard(astInit);
    astInit->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        if (!inst->fields) inst->fields = GcHeap::get().allocate<ObjDict>();
        
        inst->fields->set(Value("type"), args.size() > 0 ? args[0] : Value("Unknown"));
        inst->fields->set(Value("line"), args.size() > 1 ? args[1] : Value::fromInt32(0));
        
        if (args.size() > 2 && args[2].isObjType(ObjType::DICT)) {
            auto props = static_cast<ObjDict*>(args[2].asObj());
            for (const auto& [k, v] : props->elements) {
                inst->fields->set(k, v);
            }
        }
        return self;
    });
    astNodeClass->methods["init"] = astInit;

    // --- Token Class (For Syntax Macros) ---
    ObjClass* tokenClass = GcHeap::get().allocate<ObjClass>();
    GcObjGuard tokenGuard(tokenClass);
    tokenClass->name = "Token";

    auto tokenInit = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"type", "lexeme", "line", "position"}, std::vector<bool>{false, false, false, false}, "init", nullptr);
    tokenInit->defaultValues.push_back(Value::fromInt32(0)); // line
    tokenInit->defaultValues.push_back(Value::fromInt32(0)); // position
    GcObjGuard tokenInitGuard(tokenInit);
    tokenInit->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        if (!inst->fields) inst->fields = GcHeap::get().allocate<ObjDict>();
        
        inst->fields->set(Value("type"), args.size() > 0 ? args[0] : Value("Unknown"));
        inst->fields->set(Value("lexeme"), args.size() > 1 ? args[1] : Value(""));
        inst->fields->set(Value("line"), args.size() > 2 ? args[2] : Value::fromInt32(0));
        inst->fields->set(Value("position"), args.size() > 3 ? args[3] : Value::fromInt32(0));
        
        return self;
    });
    tokenClass->methods["init"] = tokenInit;

    auto tokenRepr = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__repr__", nullptr);
    GcObjGuard tokenReprGuard(tokenRepr);
    tokenRepr->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        std::string typeStr = "Unknown";
        std::string lexeme = "";
        int line = 0;
        int position = 0;
        
        if (inst->fields) {
            auto itType = inst->fields->keyMap.find(Value("type"));
            if (itType != inst->fields->keyMap.end()) {
                Value tVal = inst->fields->elements[itType->second].second;
                typeStr = tVal.isString() ? tVal.asString() : tVal.toRepr();
            }
            
            auto itLex = inst->fields->keyMap.find(Value("lexeme"));
            if (itLex != inst->fields->keyMap.end()) {
                Value lVal = inst->fields->elements[itLex->second].second;
                lexeme = lVal.isString() ? lVal.asString() : lVal.toRepr();
            }
            
            auto itLine = inst->fields->keyMap.find(Value("line"));
            if (itLine != inst->fields->keyMap.end()) {
                Value lineVal = inst->fields->elements[itLine->second].second;
                if (lineVal.isNumber()) line = static_cast<int>(lineVal.asDouble());
            }
            
            auto itPos = inst->fields->keyMap.find(Value("position"));
            if (itPos != inst->fields->keyMap.end()) {
                Value posVal = inst->fields->elements[itPos->second].second;
                if (posVal.isNumber()) position = static_cast<int>(posVal.asDouble());
            }
        }
        
        std::ostringstream oss;
        oss << "Token(\"" << typeStr << "\", \"" << lexeme << "\", " << line << ", " << position << ")";
        return Value(oss.str());
    });
    tokenClass->methods["__repr__"] = tokenRepr;
    tokenClass->methods["__str__"] = tokenRepr;

    // --- TokenStream Class ---
    ObjClass* tokenStreamClass = GcHeap::get().allocate<ObjClass>();
    GcObjGuard tsGuard(tokenStreamClass);
    tokenStreamClass->name = "TokenStream";

    auto tsInit = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"tokens"}, std::vector<bool>{false}, "init", nullptr);
    GcObjGuard tsInitGuard(tsInit);
    tsInit->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        if (!inst->fields) inst->fields = GcHeap::get().allocate<ObjDict>();
        
        if (args.empty() || !args[0].isObjType(ObjType::LIST)) {
            throw std::runtime_error("TypeError: TokenStream init expects a list of Tokens.");
        }
        inst->fields->set(Value("tokens"), args[0]);
        inst->fields->set(Value("cursor"), Value::fromInt32(0));
        return self;
    });
    tokenStreamClass->methods["init"] = tsInit;

    auto tsTokens = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "tokens", nullptr);
    GcObjGuard tsTokensGuard(tsTokens);
    tsTokens->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        return inst->fields->elements[inst->fields->keyMap[Value("tokens")]].second;
    });
    tokenStreamClass->methods["tokens"] = tsTokens;

    auto tsPeek = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "peek", nullptr);
    GcObjGuard tsPeekGuard(tsPeek);
    tsPeek->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        Value tokensVal = inst->fields->elements[inst->fields->keyMap[Value("tokens")]].second;
        ObjList* list = static_cast<ObjList*>(tokensVal.asObj());
        int cursor = inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second.asInt32();
        
        if (cursor >= static_cast<int>(list->vec.size())) {
            Token eofTok(TokenType::END_OF_FILE, "", 0, 0);
            return VM::activeVM->makeTokenInstance(eofTok);
        }
        return list->vec[cursor];
    });
    tokenStreamClass->methods["peek"] = tsPeek;

    auto tsAdvance = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "advance", nullptr);
    GcObjGuard tsAdvanceGuard(tsAdvance);
    tsAdvance->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        Value tokensVal = inst->fields->elements[inst->fields->keyMap[Value("tokens")]].second;
        ObjList* list = static_cast<ObjList*>(tokensVal.asObj());
        int cursor = inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second.asInt32();
        
        if (cursor >= static_cast<int>(list->vec.size())) {
            Token eofTok(TokenType::END_OF_FILE, "", 0, 0);
            return VM::activeVM->makeTokenInstance(eofTok);
        }
        Value ret = list->vec[cursor];
        inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second = Value::fromInt32(cursor + 1);
        return ret;
    });
    tokenStreamClass->methods["advance"] = tsAdvance;

    auto tsPrevious = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "previous", nullptr);
    GcObjGuard tsPreviousGuard(tsPrevious);
    tsPrevious->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        Value tokensVal = inst->fields->elements[inst->fields->keyMap[Value("tokens")]].second;
        ObjList* list = static_cast<ObjList*>(tokensVal.asObj());
        int cursor = inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second.asInt32();
        
        if (cursor <= 0) {
            throw std::runtime_error("TokenStream Error: No previous token.");
        }
        return list->vec[cursor - 1];
    });
    tokenStreamClass->methods["previous"] = tsPrevious;

    auto tsIsAtEnd = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "isAtEnd", nullptr);
    GcObjGuard tsIsAtEndGuard(tsIsAtEnd);
    tsIsAtEnd->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        Value tokensVal = inst->fields->elements[inst->fields->keyMap[Value("tokens")]].second;
        ObjList* list = static_cast<ObjList*>(tokensVal.asObj());
        int cursor = inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second.asInt32();
        
        return Value(cursor >= static_cast<int>(list->vec.size()));
    });
    tokenStreamClass->methods["isAtEnd"] = tsIsAtEnd;

    auto tsMatch = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"type"}, std::vector<bool>{false}, "match", nullptr);
    GcObjGuard tsMatchGuard(tsMatch);
    tsMatch->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        Value tokensVal = inst->fields->elements[inst->fields->keyMap[Value("tokens")]].second;
        ObjList* list = static_cast<ObjList*>(tokensVal.asObj());
        int cursor = inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second.asInt32();
        
        if (cursor >= static_cast<int>(list->vec.size())) return Value(false);
        
        Value tokVal = list->vec[cursor];
        if (!tokVal.isInstance() || tokVal.asInstance()->classDef->name != "Token") return Value(false);
        
        auto tokInst = tokVal.asInstance();
        Value typeVal = tokInst->fields->elements[tokInst->fields->keyMap[Value("type")]].second;
        std::string typeStr = typeVal.isString() ? typeVal.asString() : typeVal.toRepr();
        
        std::string expectedType = args[0].isString() ? args[0].asString() : args[0].toRepr();
        
        if (typeStr == expectedType) {
            inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second = Value::fromInt32(cursor + 1);
            return Value(true);
        }
        return Value(false);
    });
    tokenStreamClass->methods["match"] = tsMatch;

    auto tsConsume = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"type", "msg"}, std::vector<bool>{false, false}, "consume", nullptr);
    GcObjGuard tsConsumeGuard(tsConsume);
    tsConsume->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        Value tokensVal = inst->fields->elements[inst->fields->keyMap[Value("tokens")]].second;
        ObjList* list = static_cast<ObjList*>(tokensVal.asObj());
        int cursor = inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second.asInt32();
        
        std::string expectedType = args[0].isString() ? args[0].asString() : args[0].toRepr();
        std::string errMsg = args[1].isString() ? args[1].asString() : args[1].toRepr();
        
        if (cursor >= static_cast<int>(list->vec.size())) {
            throw std::runtime_error(errMsg);
        }
        
        Value tokVal = list->vec[cursor];
        if (!tokVal.isInstance() || tokVal.asInstance()->classDef->name != "Token") {
            throw std::runtime_error(errMsg);
        }
        
        auto tokInst = tokVal.asInstance();
        Value typeVal = tokInst->fields->elements[tokInst->fields->keyMap[Value("type")]].second;
        std::string typeStr = typeVal.isString() ? typeVal.asString() : typeVal.toRepr();
        
        if (typeStr == expectedType) {
            inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second = Value::fromInt32(cursor + 1);
            return tokVal;
        }
        throw std::runtime_error(errMsg);
    });
    tokenStreamClass->methods["consume"] = tsConsume;

    auto tsInsert = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"idx", "token"}, std::vector<bool>{false, false}, "insert", nullptr);
    GcObjGuard tsInsertGuard(tsInsert);
    tsInsert->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        Value tokensVal = inst->fields->elements[inst->fields->keyMap[Value("tokens")]].second;
        ObjList* list = static_cast<ObjList*>(tokensVal.asObj());
        int cursor = inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second.asInt32();
        
        int idx = static_cast<int>(std::round(args[0].asDouble()));
        if (idx < 0) idx += static_cast<int>(list->vec.size());
        if (idx < 0 || idx > static_cast<int>(list->vec.size())) throw std::runtime_error("TokenStream Error: Insert index out of bounds.");
        
        Value tokVal = args[1];
        if (!tokVal.isInstance() || tokVal.asInstance()->classDef->name != "Token") {
            throw std::runtime_error("TypeError: Expected a Token instance.");
        }
        auto tokInst = tokVal.asInstance();
        std::string typeStr = tokInst->fields->elements[tokInst->fields->keyMap[Value("type")]].second.asString();
        std::string lexeme = tokInst->fields->elements[tokInst->fields->keyMap[Value("lexeme")]].second.asString();
        
        TokenType tType = stringToTokenType(typeStr);
        if (tType != TokenType::STRING && tType != TokenType::FSTRING && tType != TokenType::RSTRING &&
            tType != TokenType::NEWLINE && tType != TokenType::END_OF_FILE && tType != TokenType::ERROR) {
            jc::Lexer testLexer(lexeme, "");
            auto testTokens = testLexer.tokenize();
            if (testTokens.size() != 2 || testTokens[0].type != tType) {
                throw std::runtime_error("TypeError: Token type '" + typeStr + "' does not match its lexeme '" + lexeme + "'.");
            }
        }
        
        list->mut().insert(list->mut().begin() + idx, tokVal);
        
        if (idx <= cursor) {
            inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second = Value::fromInt32(cursor + 1);
        }
        return self;
    });
    tokenStreamClass->methods["insert"] = tsInsert;

    auto tsSet = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"idx", "token"}, std::vector<bool>{false, false}, "set", nullptr);
    GcObjGuard tsSetGuard(tsSet);
    tsSet->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        Value tokensVal = inst->fields->elements[inst->fields->keyMap[Value("tokens")]].second;
        ObjList* list = static_cast<ObjList*>(tokensVal.asObj());
        
        int idx = static_cast<int>(std::round(args[0].asDouble()));
        if (idx < 0) idx += static_cast<int>(list->vec.size());
        if (idx < 0 || idx >= static_cast<int>(list->vec.size())) throw std::runtime_error("TokenStream Error: Set index out of bounds.");
        
        Value tokVal = args[1];
        if (!tokVal.isInstance() || tokVal.asInstance()->classDef->name != "Token") {
            throw std::runtime_error("TypeError: Expected a Token instance.");
        }
        auto tokInst = tokVal.asInstance();
        std::string typeStr = tokInst->fields->elements[tokInst->fields->keyMap[Value("type")]].second.asString();
        std::string lexeme = tokInst->fields->elements[tokInst->fields->keyMap[Value("lexeme")]].second.asString();
        
        TokenType tType = stringToTokenType(typeStr);
        if (tType != TokenType::STRING && tType != TokenType::FSTRING && tType != TokenType::RSTRING &&
            tType != TokenType::NEWLINE && tType != TokenType::END_OF_FILE && tType != TokenType::ERROR) {
            jc::Lexer testLexer(lexeme, "");
            auto testTokens = testLexer.tokenize();
            if (testTokens.size() != 2 || testTokens[0].type != tType) {
                throw std::runtime_error("TypeError: Token type '" + typeStr + "' does not match its lexeme '" + lexeme + "'.");
            }
        }
        
        list->mut()[idx] = tokVal;
        return self;
    });
    tokenStreamClass->methods["set"] = tsSet;

    auto tsRemove = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"idx"}, std::vector<bool>{false}, "remove", nullptr);
    GcObjGuard tsRemoveGuard(tsRemove);
    tsRemove->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        Value tokensVal = inst->fields->elements[inst->fields->keyMap[Value("tokens")]].second;
        ObjList* list = static_cast<ObjList*>(tokensVal.asObj());
        int cursor = inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second.asInt32();
        
        int idx = static_cast<int>(std::round(args[0].asDouble()));
        if (idx < 0) idx += static_cast<int>(list->vec.size());
        if (idx < 0 || idx >= static_cast<int>(list->vec.size())) throw std::runtime_error("TokenStream Error: Remove index out of bounds.");
        
        list->mut().erase(list->mut().begin() + idx);
        
        if (idx < cursor) {
            inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second = Value::fromInt32(cursor - 1);
        }
        return self;
    });
    tokenStreamClass->methods["remove"] = tsRemove;

    auto tsParse = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "parse", nullptr);
    GcObjGuard tsParseGuard(tsParse);
    tsParse->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        Value tokensVal = inst->fields->elements[inst->fields->keyMap[Value("tokens")]].second;
        ObjList* list = static_cast<ObjList*>(tokensVal.asObj());
        int cursor = inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second.asInt32();
        
        std::vector<Token> tokens;
        for (size_t i = cursor; i < list->vec.size(); ++i) {
            const auto& v = list->vec[i];
            if (!v.isInstance() || v.asInstance()->classDef->name != "Token") throw std::runtime_error("TypeError: TokenStream contains non-Token elements.");
            auto tokInst = v.asInstance();
            std::string typeStr = tokInst->fields->elements[tokInst->fields->keyMap[Value("type")]].second.asString();
            std::string lexeme = tokInst->fields->elements[tokInst->fields->keyMap[Value("lexeme")]].second.asString();
            int line = tokInst->fields->elements[tokInst->fields->keyMap[Value("line")]].second.asInt32();
            int pos = tokInst->fields->elements[tokInst->fields->keyMap[Value("position")]].second.asInt32();
            
            TokenType tType = stringToTokenType(typeStr);
            tokens.emplace_back(tType, lexeme, pos, line);
        }
        tokens.emplace_back(TokenType::END_OF_FILE, "", 0, 0);
        
        Parser parser(tokens);
        auto ast = parser.parse();
        
        inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second = Value::fromInt32(static_cast<int32_t>(list->vec.size()));
        
        if (auto* block = dynamic_cast<Block*>(ast.get())) {
            if (block->statements.size() == 1) {
                return AST_to_JC2(block->statements[0].get());
            } else {
                ObjList* retList = GcHeap::get().allocate<ObjList>();
                GcObjGuard guard(retList);
                for (const auto& stmt : block->statements) {
                    retList->vec.push_back(AST_to_JC2(stmt.get()));
                }
                return Value(retList);
            }
        }
        return AST_to_JC2(ast.get());
    });
    tokenStreamClass->methods["parse"] = tsParse;

    auto tsParseOne = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "parseOne", nullptr);
    GcObjGuard tsParseOneGuard(tsParseOne);
    tsParseOne->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        Value tokensVal = inst->fields->elements[inst->fields->keyMap[Value("tokens")]].second;
        ObjList* list = static_cast<ObjList*>(tokensVal.asObj());
        int cursor = inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second.asInt32();
        
        if (cursor >= static_cast<int>(list->vec.size())) {
            throw std::runtime_error("TokenStream Error: Cannot parse past end of stream.");
        }

        std::vector<Token> tokens;
        for (size_t i = cursor; i < list->vec.size(); ++i) {
            const auto& v = list->vec[i];
            if (!v.isInstance() || v.asInstance()->classDef->name != "Token") throw std::runtime_error("TypeError: TokenStream contains non-Token elements.");
            auto tokInst = v.asInstance();
            std::string typeStr = tokInst->fields->elements[tokInst->fields->keyMap[Value("type")]].second.asString();
            std::string lexeme = tokInst->fields->elements[tokInst->fields->keyMap[Value("lexeme")]].second.asString();
            int line = tokInst->fields->elements[tokInst->fields->keyMap[Value("line")]].second.asInt32();
            int pos = tokInst->fields->elements[tokInst->fields->keyMap[Value("position")]].second.asInt32();
            
            TokenType tType = stringToTokenType(typeStr);
            tokens.emplace_back(tType, lexeme, pos, line);
        }
        tokens.emplace_back(TokenType::END_OF_FILE, "", 0, 0);
        
        Parser parser(tokens);
        auto ast = parser.parseStatementOrBlock();
        
        inst->fields->elements[inst->fields->keyMap[Value("cursor")]].second = Value::fromInt32(cursor + parser.getCurrent());
        
        return AST_to_JC2(ast.get());
    });
    tokenStreamClass->methods["parseOne"] = tsParseOne;

    // --- Exception Class ---
    ObjClass* exceptionClass = GcHeap::get().allocate<ObjClass>();
    GcObjGuard excGuard(exceptionClass);
    exceptionClass->name = "Exception";

    auto excInit = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{"type", "message"}, std::vector<bool>{false, false}, "init", nullptr);
    excInit->defaultValues.push_back(Value::none());
    GcObjGuard excInitGuard(excInit);
    excInit->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>& args) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        if (!inst->fields) inst->fields = GcHeap::get().allocate<ObjDict>();
        
        if (args.size() == 1) {
            inst->fields->set(Value("type"), Value("Exception"));
            inst->fields->set(Value("message"), args[0]);
        } else {
            inst->fields->set(Value("type"), args[0]);
            inst->fields->set(Value("message"), args[1]);
        }
        inst->fields->set(Value("traceback"), Value(""));
        inst->fields->set(Value("suppressed"), Value(GcHeap::get().allocate<ObjList>()));
        
        return self;
    });
    exceptionClass->methods["init"] = excInit;

    auto excRepl = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__repr__", nullptr);
    GcObjGuard excReplGuard(excRepl);
    excRepl->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        std::string typeStr = "Exception";
        std::string msg = "Unknown Error";
        if (inst->fields) {
            auto itType = inst->fields->keyMap.find(Value("type"));
            if (itType != inst->fields->keyMap.end()) {
                Value tVal = inst->fields->elements[itType->second].second;
                typeStr = tVal.isString() ? tVal.asString() : tVal.toRepr();
            }
            
            auto itMsg = inst->fields->keyMap.find(Value("message"));
            if (itMsg != inst->fields->keyMap.end()) {
                Value mVal = inst->fields->elements[itMsg->second].second;
                msg = mVal.isString() ? mVal.asString() : mVal.toRepr();
            }
        }
        if (typeStr == "Exception") {
            return Value("<Exception: " + msg + ">");
        } else {
            return Value("<Exception " + typeStr + ": " + msg + ">");
        }
    });
    exceptionClass->methods["__repr__"] = excRepl;

    auto excStr = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, "__str__", nullptr);
    GcObjGuard excStrGuard(excStr);
    excStr->nativeFn = std::make_any<NativeCallable>([](const std::vector<Value>&) -> Value {
        Value self = helpers::nativeSelfStack.back();
        auto inst = self.asInstance();
        std::string typeStr = "Exception";
        std::string msg = "Unknown Error";
        std::string tb = "";
        ObjList* supp = nullptr;
        
        if (inst->fields) {
            auto itType = inst->fields->keyMap.find(Value("type"));
            if (itType != inst->fields->keyMap.end()) {
                Value tVal = inst->fields->elements[itType->second].second;
                typeStr = tVal.isString() ? tVal.asString() : tVal.toRepr();
            }
            
            auto itMsg = inst->fields->keyMap.find(Value("message"));
            if (itMsg != inst->fields->keyMap.end()) {
                Value mVal = inst->fields->elements[itMsg->second].second;
                msg = mVal.isString() ? mVal.asString() : mVal.toRepr();
            }
            
            auto itTb = inst->fields->keyMap.find(Value("traceback"));
            if (itTb != inst->fields->keyMap.end()) {
                Value tbVal = inst->fields->elements[itTb->second].second;
                tb = tbVal.isString() ? tbVal.asString() : tbVal.toRepr();
            }
            
            auto itSupp = inst->fields->keyMap.find(Value("suppressed"));
            if (itSupp != inst->fields->keyMap.end()) {
                Value sVal = inst->fields->elements[itSupp->second].second;
                if (sVal.isObjType(ObjType::LIST)) supp = static_cast<ObjList*>(sVal.asObj());
            }
        }
        
        std::ostringstream oss;
        if (msg.find("Error:") != std::string::npos || msg.find("Exception:") != std::string::npos) {
            oss << msg;
        } else {
            oss << typeStr << ": " << msg;
        }
        if (!tb.empty()) {
            oss << tb;
        }
        if (supp && !supp->vec.empty()) {
            oss << "\n\n" << jc::col(jc::Ansi::BRIGHT_RED) << "Suppressed Exceptions (from defer):" << jc::col(jc::Ansi::RESET);
            for (const auto& s : supp->vec) {
                oss << "\n----------------------------------------\n";
                if (s.isInstance() && s.asInstance()->classDef->name == "Exception") {
                    auto dunderStr = VM::activeVM->findDunder(s, "__str__");
                    if (dunderStr) {
                        try {
                            oss << VM::activeVM->callDunder(s, dunderStr, {}).asString();
                        } catch (...) {
                            oss << s.toRepr();
                        }
                    } else {
                        oss << s.toRepr();
                    }
                } else {
                    oss << s.toRepr();
                }
            }
        }
        return Value(oss.str());
    });
    exceptionClass->methods["__str__"] = excStr;

    // 注册到全局
    VM::activeVM->registerBuiltinValue("range", Value(rangeClass));
    VM::activeVM->registerBuiltinValue("__range_iterator", Value(rangeIterClass));
    VM::activeVM->registerBuiltinValue("ASTNode", Value(astNodeClass));
    VM::activeVM->registerBuiltinValue("Token", Value(tokenClass));
    VM::activeVM->registerBuiltinValue("TokenStream", Value(tokenStreamClass));
    VM::activeVM->registerBuiltinValue("Exception", Value(exceptionClass));
}

} // namespace jc
