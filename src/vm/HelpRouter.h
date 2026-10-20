#ifndef JC2_HELP_ROUTER_H
#define JC2_HELP_ROUTER_H

#include <string>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include "../frontend/Highlight.h"
#include "../utils/json/Json.h"

namespace jc {
    inline std::map<std::string, std::string> DynamicHelp;

    class HelpRouter {
    public:
        static inline Json helpAst;
        static inline bool initialized = false;

        static std::filesystem::path exeDirPath() {
#ifdef _WIN32
            wchar_t buf[MAX_PATH];
            DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                return std::filesystem::path(std::wstring(buf, n)).parent_path();
            }
#endif
            return std::filesystem::current_path();
        }

        static void init() {
            if (initialized) return;
            std::string jsonStr;
            std::filesystem::path docPath = exeDirPath() / "data" / "documentation.json";
            std::ifstream f(docPath);
            if (f.is_open()) {
                std::stringstream ss;
                ss << f.rdbuf();
                jsonStr = ss.str();
                f.close();
            }
            if (jsonStr.empty()) {
                initialized = true;
                return;
            }
            size_t pos = 0;
            while ((pos = jsonStr.find("\xEF\xBB\xBF", pos)) != std::string::npos) {
                jsonStr.erase(pos, 3);
            }
            for (char& c : jsonStr) {
                unsigned char uc = static_cast<unsigned char>(c);
                if (uc < 32 && c != '\n') c = ' ';
            }
            try {
                helpAst = Json::parse(jsonStr);
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

        // 对外：返回整段帮助文本（纯文本，不含 ANSI）
        static std::string helpText(const std::string& query, bool* found = nullptr) {
            init();
            if (found) *found = true;
            if (helpAst.isNull() || !helpAst.isObject()) {
                if (found) *found = false;
                return "  [System] Documentation data is unavailable.";
            }

            Index ix = buildIndex(helpAst);
            std::string q = normalize(query);
            if (q.empty()) return renderMain(ix);

            if (q.find('*') != std::string::npos || q.find('?') != std::string::npos) {
                return renderSearch(ix, q);
            }

            // 1. 目录（catalog 节点）
            if (ix.catByPath.count(q)) return renderCatalogDir(ix, q);

            // 2. 精确限定名
            auto exact = ix.nodes.find(q);
            if (exact != ix.nodes.end()) return renderNode(ix, exact->second);

            // topic.<name> 直接查询主题
            if (q.rfind("topic.", 0) == 0) {
                auto t = ix.nodes.find(q.substr(6));
                if (t != ix.nodes.end()) return renderNode(ix, t->second);
            }

            // 3. 主题短名
            auto topics = shortMatches(ix, q, "topic");
            if (!topics.empty()) {
                if (topics.size() == 1) return renderNode(ix, ix.nodes.find(topics[0])->second);
                return renderAmbiguous(ix, q, topics);
            }

            // 4. 全局函数
            auto globals = shortMatches(ix, q, "function");
            if (!globals.empty()) {
                if (globals.size() == 1) return renderNode(ix, ix.nodes.find(globals[0])->second);
                return renderAmbiguous(ix, q, globals);
            }

            // 5. 方法（含 dunder）
            auto methods = shortMatches(ix, q, "method");
            auto dunders = shortMatches(ix, q, "dunder");
            methods.insert(methods.end(), dunders.begin(), dunders.end());
            if (!methods.empty()) {
                if (methods.size() == 1) return renderNode(ix, ix.nodes.find(methods[0])->second);
                return renderAmbiguous(ix, q, methods);
            }

            // 6. 关键字
            auto kws = shortMatches(ix, q, "keyword");
            if (!kws.empty()) return renderNode(ix, ix.nodes.find(kws[0])->second);

            // 7. 前缀目录（有子节点）
            if (hasChildren(ix, q)) return renderPrefixDir(ix, q);

            // 8. 模糊建议
            if (found) *found = false;
            return renderNotFound(ix, q, query);
        }

        static void printHelpTopic(const std::string& topic) {
            std::cout << helpText(topic) << "\n";
        }

        static void printMainHelp() {
            std::cout << helpText("") << "\n";
        }

        static void addFunctionHelp(const std::string& name, const std::string& sig,
                                    const std::string& desc, const std::string& ex) {
            init();
            if (helpAst.isNull()) helpAst.type = JsonType::Object;
            if (helpAst.objVal.find("global_functions") == helpAst.objVal.end()) {
                Json funcs; funcs.type = JsonType::Object;
                helpAst.objVal["global_functions"] = funcs;
            }
            Json fnNode; fnNode.type = JsonType::Object;
            Json sigNode; sigNode.type = JsonType::String; sigNode.strVal = sig;
            fnNode.objVal["signature"] = sigNode;
            Json descNode; descNode.type = JsonType::String; descNode.strVal = desc;
            fnNode.objVal["desc"] = descNode;
            if (!ex.empty()) {
                Json exArr; exArr.type = JsonType::Array;
                Json exNode; exNode.type = JsonType::String; exNode.strVal = ex;
                exArr.arrVal.push_back(exNode);
                fnNode.objVal["examples"] = exArr;
            }
            helpAst.objVal["global_functions"].objVal[name] = fnNode;
        }

    private:
        struct HNode {
            std::string id;
            std::string display;
            std::string kind;   // topic / function / method / keyword / dunder
            const Json* payload = nullptr;
            std::string body;
            std::string summary;
            bool dynamic = false;
        };

        struct CNode {
            std::string path;
            std::string title;
            std::string summary;
            std::string ref;
            std::string autoCat;
            std::vector<std::string> children;
        };

        struct Index {
            std::map<std::string, HNode> nodes;
            std::multimap<std::string, std::string> shorts;
            std::map<std::string, CNode> cat;
            std::map<std::string, std::string> catByPath;
            std::vector<std::string> catRoots;
        };

        static std::string toLower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        static std::string trim(const std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return "";
            size_t b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        }

        static std::string normalize(const std::string& raw) {
            std::string q = trim(raw);
            if (q.size() > 2 && q.substr(q.size() - 2) == "()") q = trim(q.substr(0, q.size() - 2));
            return toLower(q);
        }

        static std::string lastSeg(const std::string& id) {
            size_t p = id.rfind('.');
            return p == std::string::npos ? id : id.substr(p + 1);
        }

        static std::string slugify(const std::string& s) {
            std::string out;
            for (char c : toLower(s)) {
                if (std::isalnum(static_cast<unsigned char>(c))) out += c;
                else if (c == ' ' || c == '_' || c == '-') out += '-';
            }
            while (!out.empty() && out.front() == '-') out.erase(out.begin());
            while (!out.empty() && out.back() == '-') out.pop_back();
            return out;
        }

        static std::string jsonText(const Json& v) {
            if (v.isString()) return v.strVal;
            if (v.isArray()) {
                std::ostringstream o;
                for (size_t i = 0; i < v.arrVal.size(); ++i) {
                    if (i) o << "\n";
                    if (v.arrVal[i].isString()) o << v.arrVal[i].strVal;
                }
                return o.str();
            }
            return "";
        }

        static std::vector<std::string> splitLines(const std::string& s) {
            std::vector<std::string> out;
            std::string cur;
            for (char c : s) {
                if (c == '\n') { out.push_back(cur); cur.clear(); }
                else if (c != '\r') cur += c;
            }
            out.push_back(cur);
            return out;
        }

        static std::string firstLine(const std::string& s) {
            for (const auto& line : splitLines(s)) {
                std::string t = trim(line);
                if (!t.empty()) return t;
            }
            return "";
        }

        static std::string truncate(const std::string& s, size_t n) {
            if (s.size() <= n) return s;
            size_t cut = n;
            size_t sp = s.find_last_of(" \t", cut);
            if (sp != std::string::npos && sp > n / 2) cut = sp;
            return s.substr(0, cut) + "...";
        }

        static std::string payloadStr(const Json* p, const char* key) {
            if (!p) return "";
            auto it = p->objVal.find(key);
            if (it == p->objVal.end()) return "";
            return jsonText(it->second);
        }

        static std::vector<std::string> payloadList(const Json* p, const char* key) {
            std::vector<std::string> out;
            if (!p) return out;
            auto it = p->objVal.find(key);
            if (it == p->objVal.end()) return out;
            if (it->second.isArray()) {
                for (const auto& v : it->second.arrVal) if (v.isString()) out.push_back(v.strVal);
            } else if (it->second.isString()) {
                out.push_back(it->second.strVal);
            }
            return out;
        }

        static std::string prefixForCategory(const std::string& cat) {
            if (cat == "global_functions") return "global";
            if (cat == "matrix_methods") return "matrix";
            if (cat == "list_methods") return "list";
            if (cat == "string_methods") return "string";
            if (cat == "dict_methods") return "dict";
            if (cat == "set_methods") return "set";
            if (cat == "sys_methods") return "sys";
            if (cat == "math_methods") return "math";
            if (cat == "cas_methods") return "cas";
            if (cat == "random_methods") return "random";
            if (cat == "keywords") return "keyword";
            if (cat == "dunders") return "dunder";
            return cat;
        }

        static bool isDunderName(const std::string& name) {
            return name.size() > 4 && name.rfind("__", 0) == 0 && name.substr(name.size() - 2) == "__";
        }

        static void addNode(Index& ix, const std::string& idRaw, const std::string& display,
                            const std::string& kind, const Json* payload,
                            const std::string& body, bool dynamic) {
            HNode n;
            n.id = toLower(idRaw);
            n.display = display;
            n.kind = kind;
            n.payload = payload;
            n.body = body;
            n.dynamic = dynamic;
            n.summary = payload ? firstLine(payloadStr(payload, "desc")) : firstLine(body);
            ix.nodes[n.id] = n;
            ix.shorts.emplace(lastSeg(n.id), n.id);
            for (const auto& a : payloadList(payload, "aliases")) {
                ix.shorts.emplace(toLower(a), n.id);
            }
        }

        static void addCatalogNode(Index& ix, const Json& v, const std::string& parent) {
            if (!v.isObject()) return;
            CNode n;
            n.title = payloadStr(&v, "title");
            n.summary = payloadStr(&v, "summary");
            n.ref = toLower(payloadStr(&v, "ref"));
            n.autoCat = payloadStr(&v, "auto");

            if (!n.ref.empty()) {
                n.path = (n.ref.rfind("topic.", 0) == 0) ? n.ref.substr(6) : n.ref;
            } else if (!n.autoCat.empty()) {
                n.path = prefixForCategory(n.autoCat);
            } else {
                n.path = slugify(n.title);
            }
            if (n.title.empty()) n.title = (!n.ref.empty()) ? lastSeg(n.ref) : n.path;

            if (ix.cat.count(n.path)) {
                CNode& old = ix.cat[n.path];
                if (!n.title.empty()) old.title = n.title;
                if (!n.summary.empty()) old.summary = n.summary;
                if (!n.ref.empty()) old.ref = n.ref;
                if (!n.autoCat.empty()) old.autoCat = n.autoCat;
            } else {
                ix.cat[n.path] = n;
                ix.catByPath[n.path] = n.path;
                if (parent.empty()) ix.catRoots.push_back(n.path);
                else {
                    auto& p = ix.cat[parent].children;
                    if (std::find(p.begin(), p.end(), n.path) == p.end()) p.push_back(n.path);
                }
            }
            auto it = v.objVal.find("children");
            if (it != v.objVal.end() && it->second.isArray()) {
                for (const auto& c : it->second.arrVal) addCatalogNode(ix, c, n.path);
            }
        }

        static Index buildIndex(const Json& root) {
            Index ix;
            const auto& obj = root.objVal;

            auto topicsIt = obj.find("topics");
            if (topicsIt != obj.end() && topicsIt->second.isObject()) {
                for (const auto& [k, v] : topicsIt->second.objVal) {
                    addNode(ix, "topic." + toLower(k), k, "topic", nullptr, jsonText(v), false);
                }
            }

            // 动态注册（脚本）
            for (const auto& [k, v] : DynamicHelp) {
                addNode(ix, toLower(k), k, "topic", nullptr, v, true);
            }

            static const std::pair<const char*, const char*> cats[] = {
                {"matrix_methods", "matrix"}, {"list_methods", "list"},
                {"string_methods", "string"}, {"dict_methods", "dict"},
                {"set_methods", "set"}, {"sys_methods", "sys"},
                {"math_methods", "math"}, {"cas_methods", "cas"},
                {"random_methods", "random"}
            };
            for (const auto& [cat, prefix] : cats) {
                auto it = obj.find(cat);
                if (it == obj.end() || !it->second.isObject()) continue;
                std::vector<std::string> keys;
                for (const auto& [k, v] : it->second.objVal) keys.push_back(k);
                std::sort(keys.begin(), keys.end());
                for (const auto& k : keys) {
                    const Json& v = it->second.objVal.at(k);
                    addNode(ix, std::string(prefix) + "." + toLower(k),
                            std::string(prefix) + "." + k, "method", &v, "", false);
                }
            }

            auto gfIt = obj.find("global_functions");
            if (gfIt != obj.end() && gfIt->second.isObject()) {
                std::vector<std::string> keys;
                for (const auto& [k, v] : gfIt->second.objVal) keys.push_back(k);
                std::sort(keys.begin(), keys.end());
                for (const auto& k : keys) {
                    const Json& v = gfIt->second.objVal.at(k);
                    if (isDunderName(k)) addNode(ix, "dunder." + toLower(k), k, "dunder", &v, "", false);
                    else addNode(ix, "global." + toLower(k), k, "function", &v, "", false);
                }
            }

            auto kwIt = obj.find("keywords");
            if (kwIt != obj.end() && kwIt->second.isObject()) {
                for (const auto& [k, v] : kwIt->second.objVal) {
                    addNode(ix, "keyword." + toLower(k), k, "keyword", &v, "", false);
                }
            }

            auto catIt = obj.find("catalog");
            if (catIt != obj.end() && catIt->second.isArray()) {
                for (const auto& v : catIt->second.arrVal) addCatalogNode(ix, v, "");
            }
            return ix;
        }

        static const HNode* findNode(const Index& ix, const std::string& id) {
            auto it = ix.nodes.find(toLower(id));
            return it == ix.nodes.end() ? nullptr : &it->second;
        }

        static std::vector<std::string> shortMatches(const Index& ix, const std::string& q, const std::string& kind) {
            std::vector<std::string> out;
            auto range = ix.shorts.equal_range(toLower(q));
            for (auto it = range.first; it != range.second; ++it) {
                auto n = ix.nodes.find(it->second);
                if (n == ix.nodes.end()) continue;
                if (!kind.empty() && n->second.kind != kind) continue;
                if (std::find(out.begin(), out.end(), it->second) == out.end()) out.push_back(it->second);
            }
            return out;
        }

        static bool hasChildren(const Index& ix, const std::string& id) {
            std::string prefix = toLower(id) + ".";
            auto it = ix.nodes.lower_bound(prefix);
            return it != ix.nodes.end() && it->first.rfind(prefix, 0) == 0;
        }

        static std::vector<std::string> directChildren(const Index& ix, const std::string& prefix) {
            std::vector<std::string> out;
            std::string p = toLower(prefix) + ".";
            auto it = ix.nodes.lower_bound(p);
            for (; it != ix.nodes.end() && it->first.rfind(p, 0) == 0; ++it) {
                std::string rest = it->first.substr(p.size());
                if (rest.find('.') == std::string::npos) out.push_back(it->first);
            }
            return out;
        }

        static std::string listingName(const HNode& n) {
            if (n.kind == "function") return lastSeg(n.id);
            return n.display;
        }

        static std::string breadcrumb(const std::string& display) {
            std::string out, cur;
            for (char c : display) {
                if (c == '.') { out += cur + " › "; cur.clear(); }
                else cur += c;
            }
            out += cur;
            return out;
        }

        static std::string renderNode(const Index& ix, const HNode& n) {
            std::ostringstream o;
            if (n.kind == "topic") {
                o << n.body;
                if (!n.body.empty() && n.body.back() != '\n') o << "\n";
                std::string g = lastSeg(n.id);
                if (!shortMatches(ix, g, "function").empty()) {
                    o << "\n  Tip: '" << g << "' is also a builtin function; help('global." << g << "').\n";
                }
                return o.str();
            }
            o << breadcrumb(n.display) << "\n";
            std::string sig = payloadStr(n.payload, "signature");
            if (!sig.empty()) o << "  " << sig << "\n";
            std::string desc = payloadStr(n.payload, "desc");
            if (!desc.empty()) {
                for (const auto& line : splitLines(desc)) o << "  " << line << "\n";
            }
            auto examples = payloadList(n.payload, "examples");
            if (!examples.empty()) {
                o << "\n  Examples:\n";
                for (const auto& ex : examples) o << "    " << ex << "\n";
            }
            auto aliases = payloadList(n.payload, "aliases");
            if (!aliases.empty()) {
                o << "\n  Aliases: ";
                for (size_t i = 0; i < aliases.size(); ++i) { if (i) o << ", "; o << aliases[i]; }
                o << "\n";
            }
            auto see = payloadList(n.payload, "see_also");
            if (!see.empty()) {
                o << "  See also: ";
                for (size_t i = 0; i < see.size(); ++i) { if (i) o << ", "; o << see[i]; }
                o << "\n";
            }
            return o.str();
        }

        static void renderMembers(std::ostringstream& o, const Index& ix, const std::string& prefix,
                                  const std::string& label) {
            auto ids = directChildren(ix, prefix);
            if (ids.empty()) return;
            o << "  " << label << " (" << ids.size() << ")\n";
            for (const auto& id : ids) {
                auto it = ix.nodes.find(id);
                if (it == ix.nodes.end()) continue;
                std::string name = listingName(it->second);
                o << "    " << name;
                o << (name.size() < 22 ? std::string(22 - name.size(), ' ') : "  ");
                if (!it->second.summary.empty()) o << truncate(it->second.summary, 62);
                o << "\n";
            }
            o << "\n";
        }

        static std::string childSummary(const Index& ix, const CNode& c) {
            if (!c.summary.empty()) return c.summary;
            if (!c.ref.empty()) {
                if (const HNode* n = findNode(ix, c.ref)) return n->summary;
            }
            if (const HNode* n = findNode(ix, "topic." + c.path)) return n->summary;
            if (hasChildren(ix, c.path)) return std::to_string(directChildren(ix, c.path).size()) + " entries";
            return "";
        }

        static std::string renderCatalogDir(const Index& ix, const std::string& path) {
            auto it = ix.cat.find(path);
            if (it == ix.cat.end()) return renderPrefixDir(ix, path);
            const CNode& n = it->second;
            if (n.children.empty() && n.autoCat.empty()) {
                const HNode* leaf = nullptr;
                if (!n.ref.empty()) leaf = findNode(ix, n.ref);
                if (!leaf) leaf = findNode(ix, "topic." + path);
                if (!leaf) leaf = findNode(ix, path);
                if (leaf) return renderNode(ix, *leaf);
            }
            std::ostringstream o;
            o << n.title;
            if (!n.summary.empty()) o << " — " << n.summary;
            o << "\n";

            const HNode* body = nullptr;
            if (!n.ref.empty()) body = findNode(ix, n.ref);
            if (!body) body = findNode(ix, "topic." + path);
            if (!body) body = findNode(ix, path);
            if (body && body->kind == "topic") {
                o << "\n" << body->body;
                if (body->body.empty() || body->body.back() != '\n') o << "\n";
            } else if (body) {
                o << "\n" << renderNode(ix, *body);
            }

            std::vector<const CNode*> subtopics;
            for (const auto& cid : n.children) {
                auto ci = ix.cat.find(cid);
                if (ci == ix.cat.end()) continue;
                subtopics.push_back(&ci->second);
            }

            if (!subtopics.empty()) {
                o << "  Subtopics\n";
                for (const CNode* c : subtopics) {
                    o << "    " << c->title;
                    o << (c->title.size() < 22 ? std::string(22 - c->title.size(), ' ') : "  ");
                    o << truncate(childSummary(ix, *c), 62) << "\n";
                }
                o << "\n";
            }

            if (!n.autoCat.empty()) {
                std::string prefix = prefixForCategory(n.autoCat);
                std::string label = (n.autoCat == "global_functions") ? "Functions"
                    : (n.autoCat == "keywords") ? "Keywords"
                    : (n.autoCat == "dunders") ? "Dunders" : "Members";
                renderMembers(o, ix, prefix, label);
            }

            if (!n.children.empty()) o << "Use help('<path>') for details.\n";
            else if (n.autoCat.empty() && !body) o << "  (no documentation registered)\n";
            return o.str();
        }

        static std::string renderPrefixDir(const Index& ix, const std::string& path) {
            std::ostringstream o;
            o << path << "\n";
            if (const HNode* tb = findNode(ix, "topic." + path)) {
                if (tb->kind == "topic") {
                    o << "\n" << tb->body;
                    if (tb->body.empty() || tb->body.back() != '\n') o << "\n";
                }
            }
            renderMembers(o, ix, path, "Members");
            if (!hasChildren(ix, path)) o << "  (no entries)\n";
            return o.str();
        }

        static std::string renderAmbiguous(const Index& ix, const std::string& q,
                                           const std::vector<std::string>& ids) {
            std::ostringstream o;
            o << "'" << q << "' matches " << ids.size() << " entries:\n";
            for (const auto& id : ids) {
                auto it = ix.nodes.find(id);
                if (it == ix.nodes.end()) continue;
                std::string name = it->second.display;
                o << "  " << name;
                o << (name.size() < 24 ? std::string(24 - name.size(), ' ') : "  ");
                o << truncate(it->second.summary, 60) << "\n";
            }
            o << "Use help('<qualified path>') for details.\n";
            return o.str();
        }

        static bool globMatch(const std::string& pat, const std::string& text) {
            size_t p = 0, t = 0, star = std::string::npos, mark = 0;
            while (t < text.size()) {
                if (p < pat.size() && (pat[p] == '?' || pat[p] == text[t])) { ++p; ++t; }
                else if (p < pat.size() && pat[p] == '*') { star = p++; mark = t; }
                else if (star != std::string::npos) { p = star + 1; t = ++mark; }
                else return false;
            }
            while (p < pat.size() && pat[p] == '*') ++p;
            return p == pat.size();
        }

        static std::string renderSearch(const Index& ix, const std::string& q) {
            struct Hit { int rank; std::string display; std::string summary; };
            std::vector<Hit> hits;
            size_t star = q.find('*');
            std::string prefixPart = (star == std::string::npos) ? q : q.substr(0, star);
            for (const auto& [id, n] : ix.nodes) {
                bool match = globMatch(q, id) || globMatch(q, toLower(n.display)) ||
                             globMatch(q, toLower(lastSeg(id))) || globMatch(q, toLower(n.summary));
                if (!match) continue;
                int rank = 2;
                if (globMatch(q, toLower(lastSeg(id)))) rank = 0;
                else if (!prefixPart.empty() && id.rfind(prefixPart, 0) == 0) rank = 1;
                hits.push_back({rank, n.display, n.summary});
            }
            std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
                if (a.rank != b.rank) return a.rank < b.rank;
                return a.display < b.display;
            });
            if (hits.empty()) {
                return "No help entry matches '" + q + "'.\nType help() for the main menu.\n";
            }
            std::ostringstream o;
            o << "Matches for '" << q << "' (" << hits.size() << "):\n";
            size_t shown = 0;
            for (const auto& h : hits) {
                if (shown >= 20) break;
                o << "  " << h.display;
                o << (h.display.size() < 24 ? std::string(24 - h.display.size(), ' ') : "  ");
                o << truncate(h.summary, 60) << "\n";
                ++shown;
            }
            if (hits.size() > shown) o << "  ... and " << (hits.size() - shown) << " more\n";
            return o.str();
        }

        static std::string renderNotFound(const Index& ix, const std::string& q, const std::string& raw) {
            struct Cand { int dist; std::string display; };
            std::vector<Cand> cands;
            for (const auto& [id, n] : ix.nodes) {
                std::string seg = lastSeg(id);
                int d = levenshtein(q, seg);
                bool prefix = seg.rfind(q, 0) == 0;
                bool sub = seg.find(q) != std::string::npos;
                if (d <= 2 || prefix || sub) cands.push_back({d, n.display});
            }
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
                if (a.dist != b.dist) return a.dist < b.dist;
                return a.display < b.display;
            });
            std::ostringstream o;
            o << "No help entry for '" << trim(raw) << "'.\n";
            if (!cands.empty()) {
                o << "  Did you mean: ";
                int count = 0;
                std::set<std::string> seen;
                for (const auto& c : cands) {
                    if (count >= 5) break;
                    if (!seen.insert(c.display).second) continue;
                    if (count) o << ", ";
                    o << c.display;
                    ++count;
                }
                o << "?\n";
            }
            o << "Type help() for the main menu.\n";
            return o.str();
        }

        static std::string renderMain(const Index& ix) {
            std::ostringstream o;
            o << "JC2 Help\n";
            if (ix.catRoots.empty()) {
                o << "  Topics: ";
                std::vector<std::string> topics;
                for (const auto& [id, n] : ix.nodes) {
                    if (n.kind == "topic" && id.rfind("topic.", 0) == 0) topics.push_back(n.display);
                }
                std::sort(topics.begin(), topics.end());
                for (size_t i = 0; i < topics.size(); ++i) { if (i) o << ", "; o << topics[i]; }
                o << "\n";
            } else {
                for (const auto& str : ix.catRoots) {
                    auto it = ix.cat.find(str);
                    if (it == ix.cat.end()) continue;
                    const CNode& n = it->second;
                    o << "  " << n.title;
                    o << (n.title.size() < 20 ? std::string(20 - n.title.size(), ' ') : "  ");
                    std::vector<std::string> labels;
                    for (const auto& cid : n.children) {
                        auto ci = ix.cat.find(cid);
                        if (ci == ix.cat.end()) continue;
                        labels.push_back(ci->second.title);
                    }
                    for (size_t i = 0; i < labels.size(); ++i) { if (i) o << ", "; o << labels[i]; }
                    o << "\n";
                }
            }
            o << "\nType help('<path>') for details, or help('*term*') to search.\n";
            return o.str();
        }
    };
} // namespace jc

#endif // JC2_HELP_ROUTER_H
