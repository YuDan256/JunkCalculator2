#include "../jc2_extension_cpp.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <stdexcept>
#include <cctype>

static int g_max_steps = 10000000;

static uint32_t decodeUTF8(const std::string& str, size_t& pos) {
    if (pos >= str.length()) return 0;
    unsigned char c = str[pos];
    if (c < 0x80) {
        pos++;
        return c;
    }
    uint32_t cp = 0;
    int bytes = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; bytes = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; bytes = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; bytes = 3; }
    else { pos++; return c; }
    
    pos++;
    for (int i = 0; i < bytes; ++i) {
        if (pos >= str.length()) break;
        unsigned char next = str[pos];
        if ((next & 0xC0) != 0x80) break;
        cp = (cp << 6) | (next & 0x3F);
        pos++;
    }
    return cp;
}

enum class Opcode {
    CHAR, CLASS, ANY, SPLIT, JMP, SAVE, ASSERT_START, ASSERT_END, BACKREF, MATCH
};

struct CharClass {
    bool negate = false;
    std::vector<uint32_t> chars;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::vector<char> shorthands;
    
    bool match(uint32_t cp) const {
        bool found = false;
        for (uint32_t c : chars) {
            if (cp == c) { found = true; break; }
        }
        if (!found) {
            for (const auto& r : ranges) {
                if (cp >= r.first && cp <= r.second) { found = true; break; }
            }
        }
        if (!found) {
            for (char sh : shorthands) {
                bool m = false;
                switch (sh) {
                    case 'd': m = (cp >= '0' && cp <= '9'); break;
                    case 'D': m = !(cp >= '0' && cp <= '9'); break;
                    case 'w': m = (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_'; break;
                    case 'W': m = !((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_'); break;
                    case 's': m = (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\v' || cp == '\f'); break;
                    case 'S': m = !(cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\v' || cp == '\f'); break;
                }
                if (m) { found = true; break; }
            }
        }
        return negate ? !found : found;
    }
};

struct Instruction {
    Opcode op;
    size_t op1 = 0;
    size_t op2 = 0;
};

struct ASTNode {
    std::string type;
    uint32_t ch = 0;
    std::string kind;
    int idx = 0;
    int min = 0;
    int max = 0;
    bool lazy = false;
    bool neg = false;
    std::vector<std::shared_ptr<ASTNode>> inner_list;
    std::shared_ptr<ASTNode> a, b, inner;
    
    struct ClassSpec {
        std::string t;
        uint32_t v = 0;
        uint32_t lo = 0, hi = 0;
        char sh = 0;
    };
    std::vector<ClassSpec> specs;
};

class Parser {
    std::string s;
    size_t p = 0;
    size_t n;
public:
    int gc = 0;

    Parser(const std::string& pat) : s(pat), n(pat.length()) {}

    uint32_t pk() {
        if (p >= n) return 0;
        size_t temp = p;
        return decodeUTF8(s, temp);
    }

    uint32_t nx() {
        if (p >= n) return 0;
        return decodeUTF8(s, p);
    }

    bool ok() const { return p < n; }

    std::vector<std::shared_ptr<ASTNode>> parse() {
        auto nodes = alt();
        if (ok()) jc2::throw_error("Regex Error: Unexpected character at position " + std::to_string(p));
        return nodes;
    }

    std::vector<std::shared_ptr<ASTNode>> alt() {
        auto left = seq();
        if (ok() && pk() == '|') {
            nx();
            auto right = alt();
            auto node = std::make_shared<ASTNode>();
            node->type = "alt";
            
            auto leftNode = std::make_shared<ASTNode>();
            leftNode->type = "group";
            leftNode->inner_list = left;
            leftNode->idx = -1;
            
            auto rightNode = std::make_shared<ASTNode>();
            rightNode->type = "group";
            rightNode->inner_list = right;
            rightNode->idx = -1;
            
            node->a = leftNode;
            node->b = rightNode;
            return {node};
        }
        return left;
    }

    std::vector<std::shared_ptr<ASTNode>> seq() {
        std::vector<std::shared_ptr<ASTNode>> nodes;
        while (ok() && pk() != ')' && pk() != '|') {
            nodes.push_back(quant());
        }
        return nodes;
    }

    std::shared_ptr<ASTNode> quant() {
        auto node = atom();
        if (ok()) {
            bool is_quant = false;
            uint32_t c = pk();
            if (c == '*') {
                nx();
                auto q = std::make_shared<ASTNode>();
                q->type = "star";
                q->inner = node;
                node = q;
                is_quant = true;
            } else if (c == '+') {
                nx();
                auto q = std::make_shared<ASTNode>();
                q->type = "plus";
                q->inner = node;
                node = q;
                is_quant = true;
            } else if (c == '?') {
                nx();
                auto q = std::make_shared<ASTNode>();
                q->type = "opt";
                q->inner = node;
                node = q;
                is_quant = true;
            } else if (c == '{') {
                size_t saved_p = p;
                nx();
                std::string min_str = "";
                while (ok() && isdigit(pk())) { min_str += (char)nx(); }
                
                bool has_comma = false;
                std::string max_str = "";
                if (ok() && pk() == ',') {
                    has_comma = true; nx();
                    while (ok() && isdigit(pk())) { max_str += (char)nx(); }
                }
                
                if (ok() && pk() == '}') {
                    nx();
                    int min_val = min_str.length() > 0 ? std::stoi(min_str) : 0;
                    int max_val = -1;
                    if (has_comma) {
                        if (max_str.length() > 0) max_val = std::stoi(max_str);
                    } else {
                        max_val = min_str.length() > 0 ? min_val : 0;
                    }
                    auto q = std::make_shared<ASTNode>();
                    q->type = "bound";
                    q->inner = node;
                    q->min = min_val;
                    q->max = max_val;
                    node = q;
                    is_quant = true;
                } else {
                    p = saved_p;
                }
            }
            if (is_quant) {
                if (ok() && pk() == '?') {
                    nx();
                    node->lazy = true;
                } else {
                    node->lazy = false;
                }
            }
        }
        return node;
    }

    std::shared_ptr<ASTNode> atom() {
        if (!ok()) jc2::throw_error("Regex Error: Unexpected end of pattern");
        uint32_t c = pk();
        if (c == '(') {
            nx();
            gc++;
            int idx = gc;
            auto inner = alt();
            if (!ok() || pk() != ')') jc2::throw_error("Regex Error: Unmatched '('");
            nx();
            auto node = std::make_shared<ASTNode>();
            node->type = "group";
            node->inner_list = inner;
            node->idx = idx;
            return node;
        } else if (c == '[') {
            return cls();
        } else if (c == '.') {
            nx();
            auto node = std::make_shared<ASTNode>();
            node->type = "dot";
            return node;
        } else if (c == '^') {
            nx();
            auto node = std::make_shared<ASTNode>();
            node->type = "start";
            return node;
        } else if (c == '$') {
            nx();
            auto node = std::make_shared<ASTNode>();
            node->type = "end";
            return node;
        } else if (c == '\\') {
            nx();
            if (!ok()) jc2::throw_error("Regex Error: Trailing backslash");
            uint32_t e = nx();
            if (isdigit(e) && e != '0') {
                std::string num_str = "";
                num_str += (char)e;
                while (ok() && isdigit(pk())) { num_str += (char)nx(); }
                auto node = std::make_shared<ASTNode>();
                node->type = "backref";
                node->idx = std::stoi(num_str);
                return node;
            }
            if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
                auto node = std::make_shared<ASTNode>();
                node->type = "esc";
                node->kind = std::string(1, (char)e);
                return node;
            }
            auto node = std::make_shared<ASTNode>();
            node->type = "lit";
            node->ch = e;
            return node;
        } else {
            if (c == ')' || c == '*' || c == '+' || c == '?') {
                jc2::throw_error("Regex Error: Unexpected character");
            }
            nx();
            auto node = std::make_shared<ASTNode>();
            node->type = "lit";
            node->ch = c;
            return node;
        }
    }

    std::shared_ptr<ASTNode> cls() {
        nx();
        bool neg = false;
        if (ok() && pk() == '^') { neg = true; nx(); }
        auto node = std::make_shared<ASTNode>();
        node->type = "class";
        node->neg = neg;
        
        if (ok() && pk() == ']') {
            ASTNode::ClassSpec sp; sp.t = "c"; sp.v = ']';
            node->specs.push_back(sp);
            nx();
        }
        
        while (ok() && pk() != ']') {
            uint32_t c = nx();
            if (c == '\\' && ok()) {
                uint32_t e = nx();
                if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
                    ASTNode::ClassSpec sp; sp.t = "sh"; sp.sh = (char)e;
                    node->specs.push_back(sp);
                } else {
                    ASTNode::ClassSpec sp; sp.t = "c"; sp.v = e;
                    node->specs.push_back(sp);
                }
            } else if (ok() && pk() == '-') {
                nx();
                if (ok() && pk() != ']') {
                    ASTNode::ClassSpec sp; sp.t = "r"; sp.lo = c; sp.hi = nx();
                    node->specs.push_back(sp);
                } else {
                    ASTNode::ClassSpec sp1; sp1.t = "c"; sp1.v = c;
                    ASTNode::ClassSpec sp2; sp2.t = "c"; sp2.v = '-';
                    node->specs.push_back(sp1);
                    node->specs.push_back(sp2);
                }
            } else {
                ASTNode::ClassSpec sp; sp.t = "c"; sp.v = c;
                node->specs.push_back(sp);
            }
        }
        if (!ok()) jc2::throw_error("Regex Error: Unmatched '['");
        nx();
        return node;
    }
};

class BytecodeCompiler {
public:
    std::vector<Instruction> insts;
    std::vector<CharClass> classes;
    bool hasBackref = false;

    size_t emit(Opcode op, size_t op1 = 0, size_t op2 = 0) {
        insts.push_back({op, op1, op2});
        return insts.size() - 1;
    }

    void compile(std::shared_ptr<ASTNode> node) {
        if (!node) return;
        if (node->type == "lit") {
            emit(Opcode::CHAR, node->ch);
        } else if (node->type == "dot") {
            emit(Opcode::ANY);
        } else if (node->type == "start") {
            emit(Opcode::ASSERT_START);
        } else if (node->type == "end") {
            emit(Opcode::ASSERT_END);
        } else if (node->type == "esc") {
            CharClass cls;
            cls.shorthands.push_back(node->kind[0]);
            classes.push_back(cls);
            emit(Opcode::CLASS, classes.size() - 1);
        } else if (node->type == "class") {
            CharClass cls;
            cls.negate = node->neg;
            for (auto& sp : node->specs) {
                if (sp.t == "c") cls.chars.push_back(sp.v);
                else if (sp.t == "r") cls.ranges.push_back({sp.lo, sp.hi});
                else if (sp.t == "sh") cls.shorthands.push_back(sp.sh);
            }
            classes.push_back(cls);
            emit(Opcode::CLASS, classes.size() - 1);
        } else if (node->type == "group") {
            if (node->idx != -1) emit(Opcode::SAVE, node->idx * 2);
            for (auto& n : node->inner_list) compile(n);
            if (node->idx != -1) emit(Opcode::SAVE, node->idx * 2 + 1);
        } else if (node->type == "alt") {
            size_t splitIdx = emit(Opcode::SPLIT, 0, 0);
            size_t l1 = insts.size();
            compile(node->a);
            size_t jmpIdx = emit(Opcode::JMP, 0);
            size_t l2 = insts.size();
            compile(node->b);
            size_t l3 = insts.size();
            insts[splitIdx].op1 = l1;
            insts[splitIdx].op2 = l2;
            insts[jmpIdx].op1 = l3;
        } else if (node->type == "star") {
            size_t l1 = insts.size();
            size_t splitIdx = emit(Opcode::SPLIT, 0, 0);
            size_t l2 = insts.size();
            compile(node->inner);
            emit(Opcode::JMP, l1);
            size_t l3 = insts.size();
            if (node->lazy) {
                insts[splitIdx].op1 = l3;
                insts[splitIdx].op2 = l2;
            } else {
                insts[splitIdx].op1 = l2;
                insts[splitIdx].op2 = l3;
            }
        } else if (node->type == "plus") {
            size_t l1 = insts.size();
            compile(node->inner);
            size_t splitIdx = emit(Opcode::SPLIT, 0, 0);
            size_t l2 = insts.size();
            if (node->lazy) {
                insts[splitIdx].op1 = l2;
                insts[splitIdx].op2 = l1;
            } else {
                insts[splitIdx].op1 = l1;
                insts[splitIdx].op2 = l2;
            }
        } else if (node->type == "opt") {
            size_t splitIdx = emit(Opcode::SPLIT, 0, 0);
            size_t l1 = insts.size();
            compile(node->inner);
            size_t l2 = insts.size();
            if (node->lazy) {
                insts[splitIdx].op1 = l2;
                insts[splitIdx].op2 = l1;
            } else {
                insts[splitIdx].op1 = l1;
                insts[splitIdx].op2 = l2;
            }
        } else if (node->type == "bound") {
            for (int i = 0; i < node->min; ++i) {
                compile(node->inner);
            }
            if (node->max == -1) {
                size_t l1 = insts.size();
                size_t splitIdx = emit(Opcode::SPLIT, 0, 0);
                size_t l2 = insts.size();
                compile(node->inner);
                emit(Opcode::JMP, l1);
                size_t l3 = insts.size();
                if (node->lazy) {
                    insts[splitIdx].op1 = l3;
                    insts[splitIdx].op2 = l2;
                } else {
                    insts[splitIdx].op1 = l2;
                    insts[splitIdx].op2 = l3;
                }
            } else {
                std::vector<size_t> splits;
                for (int i = node->min; i < node->max; ++i) {
                    size_t splitIdx = emit(Opcode::SPLIT, 0, 0);
                    splits.push_back(splitIdx);
                    size_t l1 = insts.size();
                    compile(node->inner);
                    if (node->lazy) {
                        insts[splitIdx].op2 = l1;
                    } else {
                        insts[splitIdx].op1 = l1;
                    }
                }
                size_t endIdx = insts.size();
                for (size_t splitIdx : splits) {
                    if (node->lazy) {
                        insts[splitIdx].op1 = endIdx;
                    } else {
                        insts[splitIdx].op2 = endIdx;
                    }
                }
            }
        } else if (node->type == "backref") {
            emit(Opcode::BACKREF, node->idx);
            hasBackref = true;
        }
    }
};

struct ThreadState {
    size_t pc = 0;
    size_t sp = 0;
    std::vector<size_t> caps;
};

class RegexVM {
public:
    std::vector<Instruction> insts;
    std::vector<CharClass> classes;
    int groupCount;
    bool hasBackref;

    RegexVM(const std::string& pat) {
        Parser p(pat);
        auto ast = p.parse();
        groupCount = p.gc;
        
        BytecodeCompiler c;
        c.emit(Opcode::SAVE, 0);
        for (auto& n : ast) c.compile(n);
        c.emit(Opcode::SAVE, 1);
        c.emit(Opcode::MATCH);
        
        insts = std::move(c.insts);
        classes = std::move(c.classes);
        hasBackref = c.hasBackref;
    }

    bool execute(const std::string& text, size_t startSp, std::vector<size_t>& outCaps) {
        std::vector<ThreadState> stack;
        
        ThreadState current;
        current.pc = 0;
        current.sp = startSp;
        current.caps.assign((groupCount + 1) * 2, (size_t)-1);
        
        size_t textLen = text.length();
        
        std::vector<bool> visited;
        if (!hasBackref) {
            visited.assign(insts.size() * (textLen + 1), false);
        }
        
        int stepCount = 0;

        while (true) {
            if (current.pc >= insts.size()) goto backtrack;
            
            if (!hasBackref) {
                size_t vIdx = current.pc * (textLen + 1) + current.sp;
                if (visited[vIdx]) goto backtrack;
                visited[vIdx] = true;
            } else {
                stepCount++;
                if (g_max_steps != -1 && stepCount > g_max_steps) {
                    jc2::throw_error("RegexError: Catastrophic backtracking detected (step limit exceeded).");
                }
            }
            
            {
                const Instruction& inst = insts[current.pc];
                switch (inst.op) {
                    case Opcode::CHAR: {
                        if (current.sp >= textLen) goto backtrack;
                        size_t nextSp = current.sp;
                        uint32_t cp = decodeUTF8(text, nextSp);
                        if (cp == static_cast<uint32_t>(inst.op1)) {
                            current.sp = nextSp;
                            current.pc++;
                        } else {
                            goto backtrack;
                        }
                        break;
                    }
                    case Opcode::CLASS: {
                        if (current.sp >= textLen) goto backtrack;
                        size_t nextSp = current.sp;
                        uint32_t cp = decodeUTF8(text, nextSp);
                        if (classes[inst.op1].match(cp)) {
                            current.sp = nextSp;
                            current.pc++;
                        } else {
                            goto backtrack;
                        }
                        break;
                    }
                    case Opcode::ANY: {
                        if (current.sp >= textLen) goto backtrack;
                        size_t nextSp = current.sp;
                        uint32_t cp = decodeUTF8(text, nextSp);
                        if (cp != '\n') {
                            current.sp = nextSp;
                            current.pc++;
                        } else {
                            goto backtrack;
                        }
                        break;
                    }
                    case Opcode::SPLIT: {
                        ThreadState splitState = current;
                        splitState.pc = inst.op2;
                        stack.push_back(splitState);
                        current.pc = inst.op1;
                        break;
                    }
                    case Opcode::JMP: {
                        current.pc = inst.op1;
                        break;
                    }
                    case Opcode::SAVE: {
                        current.caps[inst.op1] = current.sp;
                        current.pc++;
                        break;
                    }
                    case Opcode::ASSERT_START: {
                        if (current.sp == 0) current.pc++;
                        else goto backtrack;
                        break;
                    }
                    case Opcode::ASSERT_END: {
                        if (current.sp == textLen) current.pc++;
                        else goto backtrack;
                        break;
                    }
                    case Opcode::BACKREF: {
                        int idx = static_cast<int>(inst.op1);
                        size_t s = current.caps[idx * 2];
                        size_t e = current.caps[idx * 2 + 1];
                        if (s == (size_t)-1 || e == (size_t)-1) goto backtrack;
                        size_t len = e - s;
                        if (current.sp + len > textLen) goto backtrack;
                        if (text.compare(current.sp, len, text, s, len) == 0) {
                            current.sp += len;
                            current.pc++;
                        } else {
                            goto backtrack;
                        }
                        break;
                    }
                    case Opcode::MATCH: {
                        outCaps = current.caps;
                        return true;
                    }
                }
            }
            continue;
            
        backtrack:
            if (stack.empty()) return false;
            current = stack.back();
            stack.pop_back();
        }
    }
};

static jc2::Class* g_regexClass = nullptr;
static jc2::Class* g_reMatchClass = nullptr;

static std::shared_ptr<RegexVM> getRegex(const jc2::Value& val) {
    if (!val.is_instance()) {
        jc2::throw_error("TypeError: Expected a Regex instance.");
    }
    auto ptr = val.get_native_data<std::shared_ptr<RegexVM>>();
    if (!ptr) {
        jc2::throw_error("TypeError: Instance is not a Regex.");
    }
    return *ptr;
}

static jc2::Value wrapRegex(std::shared_ptr<RegexVM> vm, const std::string& pat) {
    jc2::Instance inst(*g_regexClass);
    auto data = new std::shared_ptr<RegexVM>(vm);
    inst.set_native_data(data, [](void* ptr) {
        delete static_cast<std::shared_ptr<RegexVM>*>(ptr);
    });
    inst.set("pattern", jc2::Value(pat));
    inst.freeze();
    return inst;
}

jc2::Value make_rematch(const std::string& text, const std::vector<size_t>& caps) {
    size_t start = caps[0];
    size_t end = caps[1];
    std::string matched = text.substr(start, end - start);
    jc2::List groups;
    for (size_t i = 2; i < caps.size(); i += 2) {
        size_t s = caps[i];
        size_t e = caps[i+1];
        if (s != (size_t)-1 && e != (size_t)-1) {
            groups.push_back(jc2::Value(text.substr(s, e - s)));
        } else {
            groups.push_back(jc2::Value(""));
        }
    }
    
    jc2::Instance inst(*g_reMatchClass);
    inst.set("matched", jc2::Value(matched));
    inst.set("start", jc2::Value((int32_t)start));
    inst.set("end", jc2::Value((int32_t)end));
    inst.set("groups", groups);
    return inst;
}

JC2_ValueHandle rematch_str(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Instance inst(argv[0]);
    std::string matched = inst.get("matched").as_string();
    int start = inst.get("start").as_int();
    int end = inst.get("end").as_int();
    std::string res = "<ReMatch \"" + matched + "\" span=" + std::to_string(start) + ":" + std::to_string(end) + ">";
    return jc2::Value(res).get_handle();
}

JC2_ValueHandle regex_init(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 1) jc2::throw_error("TypeError: Regex() takes 1 argument.");
    std::string pat = jc2::Value(argv[0]).as_string();
    try {
        auto vm = std::make_shared<RegexVM>(pat);
        return wrapRegex(vm, pat).get_handle();
    } catch (const std::exception& e) {
        jc2::throw_error(e.what());
    }
}

JC2_ValueHandle regex_search(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    auto vm = getRegex(jc2::Value(argv[0]));
    std::string text = jc2::Value(argv[1]).as_string();
    std::vector<size_t> caps;
    for (size_t i = 0; i <= text.length(); ++i) {
        if (vm->execute(text, i, caps)) {
            return make_rematch(text, caps).get_handle();
        }
    }
    return jc2::Value(0).get_handle();
}

JC2_ValueHandle regex_match(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    auto vm = getRegex(jc2::Value(argv[0]));
    std::string text = jc2::Value(argv[1]).as_string();
    std::vector<size_t> caps;
    if (vm->execute(text, 0, caps)) {
        return make_rematch(text, caps).get_handle();
    }
    return jc2::Value(0).get_handle();
}

JC2_ValueHandle regex_test(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    auto vm = getRegex(jc2::Value(argv[0]));
    std::string text = jc2::Value(argv[1]).as_string();
    std::vector<size_t> caps;
    for (size_t i = 0; i <= text.length(); ++i) {
        if (vm->execute(text, i, caps)) {
            return jc2::Value(true).get_handle();
        }
    }
    return jc2::Value(false).get_handle();
}

JC2_ValueHandle regex_findall(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    auto vm = getRegex(jc2::Value(argv[0]));
    std::string text = jc2::Value(argv[1]).as_string();
    jc2::List res;
    std::vector<size_t> caps;
    size_t i = 0;
    while (i <= text.length()) {
        if (vm->execute(text, i, caps)) {
            size_t start = caps[0];
            size_t end = caps[1];
            res.push_back(jc2::Value(text.substr(start, end - start)));
            i = (end == start) ? end + 1 : end;
        } else {
            i++;
        }
    }
    return res.get_handle();
}

JC2_ValueHandle regex_split(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    auto vm = getRegex(jc2::Value(argv[0]));
    std::string text = jc2::Value(argv[1]).as_string();
    jc2::List res;
    std::vector<size_t> caps;
    size_t i = 0;
    size_t lastEnd = 0;
    while (i <= text.length()) {
        if (vm->execute(text, i, caps)) {
            size_t start = caps[0];
            size_t end = caps[1];
            if (end > lastEnd) {
                res.push_back(jc2::Value(text.substr(lastEnd, start - lastEnd)));
                lastEnd = end;
                i = (end == start) ? end + 1 : end;
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
    res.push_back(jc2::Value(text.substr(lastEnd)));
    return res.get_handle();
}

JC2_ValueHandle regex_subst(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    auto vm = getRegex(jc2::Value(argv[0]));
    std::string text = jc2::Value(argv[1]).as_string();
    std::string repl = jc2::Value(argv[2]).as_string();
    std::string res = "";
    std::vector<size_t> caps;
    size_t i = 0;
    while (i <= text.length()) {
        if (vm->execute(text, i, caps)) {
            size_t start = caps[0];
            size_t end = caps[1];
            std::string matched = text.substr(start, end - start);
            
            std::string r = "";
            size_t j = 0;
            while (j < repl.length()) {
                if (repl[j] == '$' && j + 1 < repl.length()) {
                    char nc = repl[j+1];
                    if (isdigit(nc)) {
                        int gi = nc - '0';
                        if (gi >= 1 && gi <= vm->groupCount) {
                            size_t gs = caps[gi*2];
                            size_t ge = caps[gi*2+1];
                            if (gs != (size_t)-1 && ge != (size_t)-1) {
                                r += text.substr(gs, ge - gs);
                            }
                        }
                        j += 2; continue;
                    }
                    if (nc == '&') {
                        r += matched;
                        j += 2; continue;
                    }
                }
                r += repl[j];
                j++;
            }
            res += r;
            i = (end == start) ? end + 1 : end;
            if (end == start && i - 1 < text.length()) {
                res += text[i - 1];
            }
        } else {
            if (i < text.length()) res += text[i];
            i++;
        }
    }
    return jc2::Value(res).get_handle();
}

JC2_ValueHandle regex_str(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Instance inst(argv[0]);
    std::string pat = inst.get("pattern").as_string();
    std::string escaped = "";
    for (char c : pat) {
        if (c == '\\') escaped += "\\\\";
        else if (c == '"') escaped += "\\\"";
        else escaped += c;
    }
    return jc2::Value("Regex(\"" + escaped + "\")").get_handle();
}

std::shared_ptr<RegexVM> ensureRegex(const jc2::Value& val) {
    if (val.is_instance() && val.get_native_data<std::shared_ptr<RegexVM>>()) {
        return *val.get_native_data<std::shared_ptr<RegexVM>>();
    }
    return std::make_shared<RegexVM>(val.to_string());
}

JC2_ValueHandle global_getcontext(JC2_VMContext, int, JC2_ValueHandle*, void*) {
    jc2::Dict ctx;
    ctx.set(jc2::Value("max_steps"), jc2::Value(g_max_steps));
    return ctx.get_handle();
}

JC2_ValueHandle global_setcontext(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc > 0) {
        int steps = static_cast<int>(jc2::Value(argv[0]).as_double());
        if (steps < -1) {
            jc2::throw_error("ValueError: max_steps cannot be less than -1.");
        }
        g_max_steps = steps;
    }
    return jc2::Value().get_handle();
}

JC2_ValueHandle global_compile(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    std::string pat = jc2::Value(argv[0]).as_string();
    try {
        auto vm = std::make_shared<RegexVM>(pat);
        return wrapRegex(vm, pat).get_handle();
    } catch (const std::exception& e) {
        jc2::throw_error(e.what());
    }
}

JC2_ValueHandle global_test(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    auto vm = ensureRegex(jc2::Value(argv[0]));
    std::string text = jc2::Value(argv[1]).as_string();
    std::vector<size_t> caps;
    for (size_t i = 0; i <= text.length(); ++i) {
        if (vm->execute(text, i, caps)) return jc2::Value(true).get_handle();
    }
    return jc2::Value(false).get_handle();
}

JC2_ValueHandle global_match(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    auto vm = ensureRegex(jc2::Value(argv[0]));
    std::string text = jc2::Value(argv[1]).as_string();
    std::vector<size_t> caps;
    if (vm->execute(text, 0, caps)) return make_rematch(text, caps).get_handle();
    return jc2::Value(0).get_handle();
}

JC2_ValueHandle global_search(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    auto vm = ensureRegex(jc2::Value(argv[0]));
    std::string text = jc2::Value(argv[1]).as_string();
    std::vector<size_t> caps;
    for (size_t i = 0; i <= text.length(); ++i) {
        if (vm->execute(text, i, caps)) return make_rematch(text, caps).get_handle();
    }
    return jc2::Value(0).get_handle();
}

JC2_ValueHandle global_findall(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    auto vm = ensureRegex(jc2::Value(argv[0]));
    std::string text = jc2::Value(argv[1]).as_string();
    jc2::List res;
    std::vector<size_t> caps;
    size_t i = 0;
    while (i <= text.length()) {
        if (vm->execute(text, i, caps)) {
            size_t start = caps[0];
            size_t end = caps[1];
            res.push_back(jc2::Value(text.substr(start, end - start)));
            i = (end == start) ? end + 1 : end;
        } else {
            i++;
        }
    }
    return res.get_handle();
}

JC2_ValueHandle global_split(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    auto vm = ensureRegex(jc2::Value(argv[0]));
    std::string text = jc2::Value(argv[1]).as_string();
    jc2::List res;
    std::vector<size_t> caps;
    size_t i = 0;
    size_t lastEnd = 0;
    while (i <= text.length()) {
        if (vm->execute(text, i, caps)) {
            size_t start = caps[0];
            size_t end = caps[1];
            if (end > lastEnd) {
                res.push_back(jc2::Value(text.substr(lastEnd, start - lastEnd)));
                lastEnd = end;
                i = (end == start) ? end + 1 : end;
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
    res.push_back(jc2::Value(text.substr(lastEnd)));
    return res.get_handle();
}

JC2_ValueHandle global_subst(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    auto vm = ensureRegex(jc2::Value(argv[0]));
    std::string text = jc2::Value(argv[1]).as_string();
    std::string repl = jc2::Value(argv[2]).as_string();
    std::string res = "";
    std::vector<size_t> caps;
    size_t i = 0;
    while (i <= text.length()) {
        if (vm->execute(text, i, caps)) {
            size_t start = caps[0];
            size_t end = caps[1];
            std::string matched = text.substr(start, end - start);
            
            std::string r = "";
            size_t j = 0;
            while (j < repl.length()) {
                if (repl[j] == '$' && j + 1 < repl.length()) {
                    char nc = repl[j+1];
                    if (isdigit(nc)) {
                        int gi = nc - '0';
                        if (gi >= 1 && gi <= vm->groupCount) {
                            size_t gs = caps[gi*2];
                            size_t ge = caps[gi*2+1];
                            if (gs != (size_t)-1 && ge != (size_t)-1) {
                                r += text.substr(gs, ge - gs);
                            }
                        }
                        j += 2; continue;
                    }
                    if (nc == '&') {
                        r += matched;
                        j += 2; continue;
                    }
                }
                r += repl[j];
                j++;
            }
            res += r;
            i = (end == start) ? end + 1 : end;
            if (end == start && i - 1 < text.length()) {
                res += text[i - 1];
            }
        } else {
            if (i < text.length()) res += text[i];
            i++;
        }
    }
    return jc2::Value(res).get_handle();
}

int jc2_init(jc2::Module& mod) {
    g_reMatchClass = new jc2::Class("ReMatch");
    g_reMatchClass->bind_method("__str__", rematch_str, 0, 0);
    mod.register_value("ReMatch", *g_reMatchClass);

    g_regexClass = new jc2::Class("Regex");
    g_regexClass->set_allocator(regex_init);
    g_regexClass->bind_method("search", regex_search, 1, 1, {"text"});
    g_regexClass->bind_method("match", regex_match, 1, 1, {"text"});
    g_regexClass->bind_method("test", regex_test, 1, 1, {"text"});
    g_regexClass->bind_method("findall", regex_findall, 1, 1, {"text"});
    g_regexClass->bind_method("split", regex_split, 1, 1, {"text"});
    g_regexClass->bind_method("subst", regex_subst, 2, 2, {"text", "repl"});
    g_regexClass->bind_method("__str__", regex_str, 0, 0);
    g_regexClass->bind_method("__call__", regex_search, 1, 1, {"text"});
    mod.register_value("Regex", *g_regexClass);

    mod.register_function("getcontext", global_getcontext, 0, 0);
    mod.register_function("setcontext", global_setcontext, 1, 1, {"steps"});
    mod.register_function("compile", global_compile, 1, 1, {"pat"});
    mod.register_function("test", global_test, 2, 2, {"pat", "text"});
    mod.register_function("match", global_match, 2, 2, {"pat", "text"});
    mod.register_function("search", global_search, 2, 2, {"pat", "text"});
    mod.register_function("findall", global_findall, 2, 2, {"pat", "text"});
    mod.register_function("split", global_split, 2, 2, {"pat", "text"});
    mod.register_function("subst", global_subst, 3, 3, {"pat", "text", "repl"});

    mod.register_help("regex",
        "═══ Regular Expressions (Native Module) ═══\n\n"
        "  Requires: import regex\n\n"
        "  A high-performance Regex engine based on a Bytecode VM (Thompson NFA).\n"
        "  Supports capture groups, alternation, character classes, and greedy/lazy quantifiers.\n\n"
        "  Pattern Compilation (Object-Oriented API)\n"
        "  ──────────────────────\n"
        "    It is highly recommended to instantiate a Regex object when reusing patterns,\n"
        "    as it caches the compiled bytecode.\n"
        "    \n"
        "    r = regex.Regex(\"(\\\\w+) (\\\\d+)\") // Instantiate Regex object\n"
        "    r.test(\"Alice 30\")             // → true\n"
        "    m = r.search(\"Bob 25\")         // Returns a ReMatch object\n"
        "    r(\"Bob 25\")                    // Callable syntax sugar for search()\n"
        "    m.matched                      // → \"Bob 25\"\n"
        "    m.groups[0]                    // → \"Bob\"\n"
        "    m.start, m.end                 // → Matching span indices\n"
        "    \n"
        "    r.findall(\"A 1 B 2\")           // → [\"A 1\", \"B 2\"]\n"
        "    r.subst(\"A 1\", \"$2-$1\")        // → \"1-A\"\n"
        "    r.split(\"A 1, B 2\")            // Splits by match\n\n"
        "  Global Functional API (via namespace)\n"
        "  ──────────────────────\n"
        "    regex.getcontext()            Returns a dict with current context settings (max_steps).\n"
        "    regex.setcontext(steps)       Sets the global step limit for backreference backtracking (-1 for no limit).\n"
        "    regex.compile(pat)            Returns a Regex object.\n"
        "    regex.test(pat, text)         Returns true if pattern matches anywhere, else false.\n"
        "    regex.match(pat, text)        Matches exactly at the start of text.\n"
        "    regex.search(pat, text)       Searches for first match anywhere. Returns ReMatch or 0.\n"
        "    regex.findall(pat, text)      Returns a List of all matched substrings.\n"
        "    regex.split(pat, text)        Splits text by pattern, returns a List.\n"
        "    regex.subst(pat, text, rep)   Replaces matches with `rep` (supports $1, $2, &).\n\n"
        "  Supported Syntax\n"
        "  ──────────────────────\n"
        "    .             Any character (except newline)\n"
        "    \\d \\w \\s      Digit, word character (alphanumeric + _), whitespace\n"
        "    \\D \\W \\S      Negated shorthands\n"
        "    [abc], [a-z]  Character class and ranges\n"
        "    [^abc]        Negated character class\n"
        "    ^  $          Start / End anchors\n"
        "    *  +  ?       Greedy quantifiers\n"
        "    *? +? ??      Lazy (non-greedy) quantifiers\n"
        "    {m,n}         Bounded quantifiers (append ? for lazy)\n"
        "    (...)         Capturing group\n"
        "    \\1, \\2        Backreferences\n"
        "    |             Alternation (OR)\n"
    );

    return 0;
}

JC2_EXTENSION_INIT
