#ifndef JC2_HELP_ROUTER_H
#define JC2_HELP_ROUTER_H

#include <string>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <map>
#include <any>
#include <vector>
#include "GeneratedHelpText.h"
#include "../frontend/Highlight.h"

namespace jc {
    inline std::map<std::string, std::string> DynamicHelp;

    class HelpRouter {
    private:
        struct NativeJson {
            enum Type { N_NULL, N_STRING, N_ARRAY, N_OBJECT };
            Type type = N_NULL;
            std::string str;
            std::vector<NativeJson> arr;
            std::map<std::string, NativeJson> obj;

            bool isNull() const { return type == N_NULL; }
            bool isString() const { return type == N_STRING; }
            bool isArray() const { return type == N_ARRAY; }
            bool isObject() const { return type == N_OBJECT; }
        };

        class NativeJsonParser {
            std::string s;
            size_t pos = 0;

            void skipWS() {
                while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) pos++;
            }

            std::string parseString() {
                std::string res;
                pos++; // skip "
                while (pos < s.size() && s[pos] != '"') {
                    if (s[pos] == '\\' && pos + 1 < s.size()) {
                        pos++;
                        if (s[pos] == 'n') res += '\n';
                        else if (s[pos] == 't') res += '\t';
                        else if (s[pos] == 'r') res += '\r';
                        else if (s[pos] == '"') res += '"';
                        else if (s[pos] == '\\') res += '\\';
                        else res += s[pos];
                    } else {
                        res += s[pos];
                    }
                    pos++;
                }
                if (pos < s.size()) pos++; // skip "
                return res;
            }

            NativeJson parseArray() {
                NativeJson node;
                node.type = NativeJson::N_ARRAY;
                pos++; // skip [
                skipWS();
                if (pos < s.size() && s[pos] == ']') {
                    pos++;
                    return node;
                }
                while (pos < s.size()) {
                    node.arr.push_back(parseValue());
                    skipWS();
                    if (pos < s.size() && s[pos] == ',') {
                        pos++;
                        skipWS();
                    } else if (pos < s.size() && s[pos] == ']') {
                        pos++;
                        break;
                    } else {
                        break; // error
                    }
                }
                return node;
            }

            NativeJson parseObject() {
                NativeJson node;
                node.type = NativeJson::N_OBJECT;
                pos++; // skip {
                skipWS();
                if (pos < s.size() && s[pos] == '}') {
                    pos++;
                    return node;
                }
                while (pos < s.size()) {
                    skipWS();
                    if (pos >= s.size() || s[pos] != '"') break;
                    std::string key = parseString();
                    skipWS();
                    if (pos < s.size() && s[pos] == ':') pos++;
                    skipWS();
                    node.obj[key] = parseValue();
                    skipWS();
                    if (pos < s.size() && s[pos] == ',') {
                        pos++;
                        skipWS();
                    } else if (pos < s.size() && s[pos] == '}') {
                        pos++;
                        break;
                    } else {
                        break; // error
                    }
                }
                return node;
            }

        public:
            NativeJsonParser(const std::string& str) : s(str), pos(0) {}

            NativeJson parseValue() {
                skipWS();
                if (pos >= s.size()) return NativeJson();
                if (s[pos] == '"') {
                    NativeJson node;
                    node.type = NativeJson::N_STRING;
                    node.str = parseString();
                    return node;
                }
                if (s[pos] == '[') return parseArray();
                if (s[pos] == '{') return parseObject();
                
                // skip other values like true, false, numbers, null
                while (pos < s.size() && !std::isspace(static_cast<unsigned char>(s[pos])) && s[pos] != ',' && s[pos] != ']' && s[pos] != '}') {
                    pos++;
                }
                return NativeJson();
            }
        };

        static inline NativeJson helpAst;
        static inline bool initialized = false;

        static void init() {
            if (initialized) return;
            // 1. 将 CMake 生成的字节数组转为标准 C++ 字符串
            // 使用 sizeof 确保即使中间有 \0 也不会被截断
            std::string jsonStr(reinterpret_cast<const char*>(RAW_HELP_JSON), sizeof(RAW_HELP_JSON) - 1);
            
            // 2. 擦除所有的 UTF-8 BOM 头（如果存在）
            size_t pos = 0;
            while ((pos = jsonStr.find("\xEF\xBB\xBF", pos)) != std::string::npos) {
                jsonStr.erase(pos, 3);
            }
            
            // 3. 净化所有的控制字符 (解决纯手写 JSON 解析器对不可见字符处理不严格的 Bug)
            // JSON 规范允许在内部任意添加空白，将 \r, \t, \0 等替换为空格绝对安全
            for (char& c : jsonStr) {
                unsigned char uc = static_cast<unsigned char>(c);
                if (uc < 32 && uc != '\n') {
                    c = ' ';
                }
            }
            try {
                // 将极其干净的字符串喂给你的 Parser
                NativeJsonParser parser(jsonStr);
                helpAst = parser.parseValue();
            }
            catch (const std::exception& e) {
                std::cerr << "\n[HelpRouter] Failed to parse documentation.json: " << e.what() << "\n";
            }
            initialized = true;
        }

        static int levenshtein(const std::string& s1, const std::string& s2) {
            int len1 = static_cast<int>(s1.size()), len2 = static_cast<int>(s2.size());
            std::vector<int> col(len2 + 1), prevCol(len2 + 1);
            for (int i = 0; i <= len2; i++) prevCol[i] = i;
            for (int i = 0; i < len1; i++) {
                col[0] = i + 1;
                for (int j = 0; j < len2; j++) {
                    col[j + 1] = std::min({ prevCol[1 + j] + 1, col[j] + 1, prevCol[j] + (s1[i] == s2[j] ? 0 : 1) });
                }
                prevCol = col;
            }
            return prevCol[len2];
        }

        static bool printEntry(const NativeJson& entry) {
            if (!entry.isObject()) return false;
            std::cout << "\n";
            auto sigIt = entry.obj.find("signature");
            if (sigIt != entry.obj.end() && sigIt->second.isString()) {
                std::cout << col(Ansi::BRIGHT_GREEN) << sigIt->second.str << col(Ansi::RESET) << "\n";
            }
            auto descIt = entry.obj.find("desc");
            if (descIt != entry.obj.end()) {
                if (descIt->second.isString()) {
                    std::cout << "  " << descIt->second.str << "\n";
                } else if (descIt->second.isArray()) {
                    for (const auto& line : descIt->second.arr) {
                        if (line.isString()) {
                            std::cout << "  " << line.str << "\n";
                        }
                    }
                }
            }
            auto exIt = entry.obj.find("examples");
            if (exIt != entry.obj.end() && exIt->second.isArray()) {
                std::cout << "\n  Examples:\n";
                for (const auto& ex : exIt->second.arr) {
                    if (ex.isString()) {
                        std::cout << col(Ansi::BRIGHT_YELLOW) << "    " << ex.str << col(Ansi::RESET) << "\n";
                    }
                }
            }
            std::cout << std::endl;
            return true;
        }

    public:
        static void printHelpTopic(const std::string& topic) {
            init();

            std::string key = topic;
            size_t s = key.find_first_not_of(" \t");
            size_t e = key.find_last_not_of(" \t");
            if (s != std::string::npos) key = key.substr(s, e - s + 1);
            else key.clear();

            bool forceFunction = false;
            if (key.length() > 2 && key.substr(key.length() - 2) == "()") {
                forceFunction = true;
                key = key.substr(0, key.length() - 2);
            }

            // 1. Dynamic Help (Scripts)
            auto itDyn = DynamicHelp.find(key);
            if (itDyn != DynamicHelp.end()) {
                std::cout << "\n" << itDyn->second << std::endl;
                return;
            }

            if (helpAst.isNull() || !helpAst.isObject()) {
                std::cout << "\n  [System] Documentation data is unavailable.\n";
                return;
            }

            const auto& root = helpAst.obj;

            std::string lowerKey = key;
            std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(),
                [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });

            // 2. Search in "topics" (Case-insensitive)
            if (!forceFunction) {
                auto topicsIt = root.find("topics");
                if (topicsIt != root.end() && topicsIt->second.isObject()) {
                    const auto& topics = topicsIt->second.obj;
                    auto topicIt = topics.find(lowerKey);
                    if (topicIt != topics.end()) {
                        const auto& topicVal = topicIt->second;
                        if (topicVal.isString()) {
                            std::cout << "\n" << topicVal.str << std::endl;
                        } else if (topicVal.isArray()) {
                            std::cout << "\n";
                            for (const auto& line : topicVal.arr) {
                                if (line.isString()) {
                                    std::cout << line.str << "\n";
                                }
                            }
                            std::cout << std::endl;
                        }
                        
                        // Check if a function with the same name exists to provide a hint
                        auto funcsIt = root.find("functions");
                        if (funcsIt != root.end() && funcsIt->second.isObject()) {
                            if (funcsIt->second.obj.count(key)) {
                                std::cout << col(Ansi::BRIGHT_YELLOW) << "  Tip: '" << key << "' is also a built-in function. Type '/help " << key << "()' to see its documentation.\n" << col(Ansi::RESET) << std::endl;
                            }
                        }
                        return;
                    }
                }
            }

            // 3. Search in "functions" (Exact match or alias)
            auto funcsIt = root.find("functions");
            if (funcsIt != root.end() && funcsIt->second.isObject()) {
                const auto& funcs = funcsIt->second.obj;
                
                const NativeJson* targetFn = nullptr;
                auto fnIt = funcs.find(key);
                if (fnIt != funcs.end()) {
                    targetFn = &fnIt->second;
                } else {
                    for (const auto& [k, v] : funcs) {
                        if (v.isObject()) {
                            auto aliasesIt = v.obj.find("aliases");
                            if (aliasesIt != v.obj.end() && aliasesIt->second.isArray()) {
                                for (const auto& alias : aliasesIt->second.arr) {
                                    if (alias.isString() && alias.str == key) {
                                        targetFn = &v;
                                        break;
                                    }
                                }
                            }
                        }
                        if (targetFn) break;
                    }
                }

                if (targetFn && printEntry(*targetFn)) return;
            }

            // 3.5 Search in "keywords" (case-insensitive)
            auto kwsIt = root.find("keywords");
            if (kwsIt != root.end() && kwsIt->second.isObject()) {
                const auto& kws = kwsIt->second.obj;
                auto kwIt = kws.find(lowerKey);
                if (kwIt != kws.end()) {
                    if (printEntry(kwIt->second)) return;
                }
            }

            // 4. Did you mean? (Fuzzy matching)
            std::vector<std::pair<std::string, std::string>> candidates;
            if (funcsIt != root.end() && funcsIt->second.isObject()) {
                for (const auto& [funcName, v] : funcsIt->second.obj) {
                    if (v.isObject()) {
                        std::string desc = "";
                        auto descIt = v.obj.find("desc");
                        if (descIt != v.obj.end()) {
                            if (descIt->second.isString()) {
                                desc = descIt->second.str;
                            } else if (descIt->second.isArray() && !descIt->second.arr.empty()) {
                                if (descIt->second.arr[0].isString()) {
                                    desc = descIt->second.arr[0].str;
                                }
                            }
                        }
                        
                        std::string lowerFuncName = funcName;
                        std::transform(lowerFuncName.begin(), lowerFuncName.end(), lowerFuncName.begin(),
                            [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
                        
                        bool isPrefix = lowerFuncName.find(lowerKey) == 0;
                        bool isSubstr = lowerFuncName.find(lowerKey) != std::string::npos;
                        int dist = levenshtein(lowerKey, lowerFuncName);
                        
                        if (isPrefix || isSubstr || dist <= 2) {
                            candidates.push_back({funcName, desc});
                        }
                    }
                }
            }

            if (kwsIt != root.end() && kwsIt->second.isObject()) {
                for (const auto& [kwName, v] : kwsIt->second.obj) {
                    if (v.isObject()) {
                        std::string desc = "";
                        auto descIt = v.obj.find("desc");
                        if (descIt != v.obj.end()) {
                            if (descIt->second.isString()) {
                                desc = descIt->second.str;
                            } else if (descIt->second.isArray() && !descIt->second.arr.empty()) {
                                if (descIt->second.arr[0].isString()) {
                                    desc = descIt->second.arr[0].str;
                                }
                            }
                        }
                        std::string lowerKwName = kwName;
                        std::transform(lowerKwName.begin(), lowerKwName.end(), lowerKwName.begin(),
                            [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
                        bool isPrefix = lowerKwName.find(lowerKey) == 0;
                        bool isSubstr = lowerKwName.find(lowerKey) != std::string::npos;
                        int dist = levenshtein(lowerKey, lowerKwName);
                        if (isPrefix || isSubstr || dist <= 2) {
                            candidates.push_back({kwName + " (keyword)", desc});
                        }
                    }
                }
            }

            if (!candidates.empty()) {
                std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
                    std::string la = a.first, lb = b.first;
                    std::transform(la.begin(), la.end(), la.begin(), [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
                    std::transform(lb.begin(), lb.end(), lb.begin(), [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
                    
                    bool aPref = la.find(lowerKey) == 0;
                    bool bPref = lb.find(lowerKey) == 0;
                    if (aPref != bPref) return aPref;
                    
                    bool aSub = la.find(lowerKey) != std::string::npos;
                    bool bSub = lb.find(lowerKey) != std::string::npos;
                    if (aSub != bSub) return aSub;
                    
                    return levenshtein(lowerKey, la) < levenshtein(lowerKey, lb);
                });
                
                std::cout << "\n  No exact match found for '" << topic << "'.\n\n";
                std::cout << "  Did you mean?\n";
                int count = 0;
                for (const auto& cand : candidates) {
                    if (count++ >= 5) break;
                    std::cout << col(Ansi::BRIGHT_GREEN) << "    > " << cand.first << col(Ansi::RESET);
                    if (!cand.second.empty()) {
                        std::string d = cand.second;
                        if (d.length() > 60) {
                            size_t cutPos = 57;
                            size_t lastSpace = d.find_last_of(" \t", 57);
                            if (lastSpace != std::string::npos && lastSpace > 30) {
                                cutPos = lastSpace;
                            }
                            d = d.substr(0, cutPos) + "...";
                        }
                        int padding = std::max(1, 15 - static_cast<int>(cand.first.length()));
                        std::cout << std::string(padding, ' ') << "(" << d << ")";
                    }
                    std::cout << "\n";
                }
                std::cout << std::endl;
                return;
            }

            std::cout << "\n  No detailed help available for topic: '" << topic << "'\n";
        }

        static void printMainHelp() {
            printHelpTopic("main");
        }

        static void addFunctionHelp(const std::string& name, const std::string& sig, const std::string& desc, const std::string& ex) {
            init();
            if (helpAst.isNull()) {
                helpAst.type = NativeJson::N_OBJECT;
            }
            if (helpAst.obj.find("functions") == helpAst.obj.end()) {
                NativeJson funcs; funcs.type = NativeJson::N_OBJECT;
                helpAst.obj["functions"] = funcs;
            }
            NativeJson fnNode;
            fnNode.type = NativeJson::N_OBJECT;
            
            NativeJson sigNode; sigNode.type = NativeJson::N_STRING; sigNode.str = sig;
            fnNode.obj["signature"] = sigNode;
            
            NativeJson descNode; descNode.type = NativeJson::N_STRING; descNode.str = desc;
            fnNode.obj["desc"] = descNode;
            
            if (!ex.empty()) {
                NativeJson exArr; exArr.type = NativeJson::N_ARRAY;
                NativeJson exNode; exNode.type = NativeJson::N_STRING; exNode.str = ex;
                exArr.arr.push_back(exNode);
                fnNode.obj["examples"] = exArr;
            }
            
            helpAst.obj["functions"].obj[name] = fnNode;
        }
    };
} // namespace jc

#endif // JC2_HELP_ROUTER_H
