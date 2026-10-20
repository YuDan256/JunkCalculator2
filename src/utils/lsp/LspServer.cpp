#include "LspServer.h"
#include "../../frontend/Lexer.h"
#include "../../frontend/Parser.h"
#include "NameResolver.h"
#include "TypeChecker.h"
#include "TypeInferrer.h"
#include "../../utils/fmt/Formatter.h"
#include "../../vm/HelpRouter.h"
#include "../../frontend/Utf8.h"
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace jc {
namespace lsp {

    // 在 doc 文本里找 `var = <rhs>`，返回最后一个赋值右侧（用于内置类实例的类型推断兜底）
    static std::string assignedRhs(const std::string& text, const std::string& var, int beforeOffset) {
        std::string best;
        size_t pos = 0;
        while ((pos = text.find(var, pos)) != std::string::npos) {
            if (pos >= static_cast<size_t>(beforeOffset)) break;
            bool leftOk = (pos == 0) || !(std::isalnum(static_cast<unsigned char>(text[pos - 1])) || text[pos - 1] == '_');
            size_t after = pos + var.size();
            bool rightOk = (after >= text.size()) || !(std::isalnum(static_cast<unsigned char>(text[after])) || text[after] == '_');
            if (leftOk && rightOk) {
                size_t p = after;
                while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) p++;
                if (p < text.size() && text[p] == '=' && (p + 1 >= text.size() || text[p + 1] != '=')) {
                    size_t e = text.find('\n', p);
                    best = text.substr(p + 1, (e == std::string::npos ? text.size() : e) - (p + 1));
                }
            }
            pos += var.size();
        }
        return best;
    }

    // 从右值扫描 `Class(`，返回在 BuiltinIndex 中确实有类方法的类名
    static std::string classFromRhs(const std::string& rhs, const BuiltinIndex& index) {
        for (size_t i = 0; i < rhs.size();) {
            if (std::isalpha(static_cast<unsigned char>(rhs[i])) || rhs[i] == '_') {
                size_t j = i;
                while (j < rhs.size() && (std::isalnum(static_cast<unsigned char>(rhs[j])) || rhs[j] == '_')) j++;
                std::string id = rhs.substr(i, j - i);
                size_t k = j;
                while (k < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[k]))) k++;
                if (k < rhs.size() && rhs[k] == '(' && index.hasClassMethods(id)) return id;
                i = j;
            } else {
                i++;
            }
        }
        return "";
    }

    LspServer::LspServer() : isRunning(false) {
        // 确保标准输入输出以二进制模式运行，避免 \r\n 转换导致 Content-Length 计算错误
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
    }

    LspServer::~LspServer() = default;

    void LspServer::run() {
        isRunning = true;
        std::string content;
        
        while (isRunning) {
            if (readMessage(content)) {
                handleMessage(content);
            } else {
                // 读取失败或遇到 EOF，退出循环
                break;
            }
        }
    }

    bool LspServer::readMessage(std::string& outContent) {
        outContent.clear();
        int contentLength = -1;
        std::string line;

        // 1. 读取 HTTP 头部
        while (true) {
            char c;
            line.clear();
            while (std::cin.get(c)) {
                line += c;
                if (c == '\n') break;
            }

            if (line.empty()) {
                return false; // EOF
            }

            // 头部结束标志 \r\n 或 \n
            if (line == "\r\n" || line == "\n") {
                break;
            }

            // 解析 Content-Length
            const std::string clPrefix = "Content-Length: ";
            if (line.compare(0, clPrefix.length(), clPrefix) == 0) {
                try {
                    contentLength = std::stoi(line.substr(clPrefix.length()));
                } catch (...) {
                    return false; // 解析长度失败
                }
            }
        }

        if (contentLength < 0) {
            return false;
        }

        // 2. 读取指定长度的 JSON 内容
        outContent.resize(contentLength);
        std::cin.read(&outContent[0], contentLength);

        return std::cin.good() || std::cin.eof();
    }

    void LspServer::sendMessage(const std::string& content) {
        std::cout << "Content-Length: " << content.length() << "\r\n\r\n" << content;
        std::cout.flush();
    }

    void LspServer::handleMessage(const std::string& content) {
        try {
            Json j = Json::parse(content);
            
            // JSON-RPC 路由逻辑
            if (j.has("method")) {
                if (j.has("id")) {
                    // 有 id 和 method，是 Request
                    handleRequest(RequestMessage::fromJson(j));
                } else {
                    // 有 method 无 id，是 Notification
                    handleNotification(NotificationMessage::fromJson(j));
                }
            }
        } catch (const std::exception&) {
            // 工业级实现：解析失败时应静默忽略或记录到日志文件，绝不能崩溃或污染 stdout
        }
    }

    void LspServer::handleRequest(const RequestMessage& req) {
        if (req.method == "initialize") {
            // 1. 解析客户端参数 (暂不深度使用，但保留扩展性)
            InitializeParams params = InitializeParams::fromJson(req.params);
            
            // 2. 构造服务器能力响应
            InitializeResult result;
            result.capabilities.textDocumentSync = 1; // 1 = Full Sync
            result.capabilities.hoverProvider = true;
            result.capabilities.definitionProvider = true;
            result.capabilities.documentFormattingProvider = true;
            result.capabilities.completionProvider.resolveProvider = false;
            result.capabilities.completionProvider.triggerCharacters = { ".", ":" };

            result.capabilities.semanticTokensProvider.legend.tokenTypes = {
                "namespace", "type", "class", "enum", "interface", "struct", "typeParameter", "parameter", "variable", "property", "enumMember", "event", "function", "method", "macro", "controlFlow", "keyword", "comment", "string", "number", "regexp", "operator", "decorator"
            };
            result.capabilities.semanticTokensProvider.legend.tokenModifiers = {
                "declaration", "definition", "readonly", "static", "deprecated", "abstract", "async", "modification", "documentation", "defaultLibrary"
            };
            result.capabilities.semanticTokensProvider.full.delta = false;
            result.capabilities.semanticTokensProvider.range = false;

            // 3. 发送响应
            ResponseMessage res;
            res.id = req.id;
            res.result = result.toJson();
            sendMessage(res.toJson().serialize());
            
            state = ServerState::Initialized;
        }
        else if (req.method == "shutdown") {
            state = ServerState::ShuttingDown;
            ResponseMessage res;
            res.id = req.id;
            res.result = Json(nullptr);
            sendMessage(res.toJson().serialize());
        }
        else if (req.method == "textDocument/hover") {
            handleHover(req);
        }
        else if (req.method == "textDocument/definition") {
            handleDefinition(req);
        }
        else if (req.method == "textDocument/completion") {
            handleCompletion(req);
        }
        else if (req.method == "textDocument/formatting") {
            handleFormatting(req);
        }
        else if (req.method == "textDocument/signatureHelp") {
            handleSignatureHelp(req);
        }
        else if (req.method == "textDocument/documentSymbol") {
            handleDocumentSymbol(req);
        }
        else if (req.method == "textDocument/semanticTokens/full") {
            handleSemanticTokens(req);
        }
    }

    void LspServer::handleNotification(const NotificationMessage& notif) {
        if (notif.method == "exit") {
            isRunning = false;
        } else if (notif.method == "textDocument/didOpen") {
            handleDidOpen(notif);
        } else if (notif.method == "textDocument/didChange") {
            handleDidChange(notif);
        } else if (notif.method == "textDocument/didClose") {
            handleDidClose(notif);
        }
    }

    void LspServer::handleDidOpen(const NotificationMessage& notif) {
        if (notif.params.has("textDocument")) {
            const Json& doc = notif.params["textDocument"];
            if (doc.has("uri") && doc.has("text") && doc.has("version")) {
                std::string uri = doc["uri"].strVal;
                std::string text = doc["text"].strVal;
                int version = static_cast<int>(doc["version"].numVal);
                workspace.openDocument(uri, text, version);
                publishDiagnostics(uri, workspace.getDocument(uri));
            }
        }
    }

    void LspServer::handleDidChange(const NotificationMessage& notif) {
        if (notif.params.has("textDocument") && notif.params.has("contentChanges")) {
            const Json& doc = notif.params["textDocument"];
            if (doc.has("uri") && doc.has("version")) {
                std::string uri = doc["uri"].strVal;
                int version = static_cast<int>(doc["version"].numVal);
                
                const Json& changes = notif.params["contentChanges"];
                if (changes.isArray() && !changes.arrVal.empty()) {
                    // 因为我们在 initialize 中声明了 textDocumentSync = 1 (Full Sync)
                    // 所以编辑器每次都会把完整的最新代码发过来，直接取 text 即可
                    if (changes.arrVal[0].has("text")) {
                        std::string text = changes.arrVal[0]["text"].strVal;
                        workspace.updateDocument(uri, text, version);
                        publishDiagnostics(uri, workspace.getDocument(uri));
                    }
                }
            }
        }
    }

    void LspServer::handleDidClose(const NotificationMessage& notif) {
        if (notif.params.has("textDocument")) {
            const Json& doc = notif.params["textDocument"];
            if (doc.has("uri")) {
                std::string uri = doc["uri"].strVal;
                workspace.closeDocument(uri);
            }
        }
    }

    void LspServer::handleHover(const RequestMessage& req) {
        ResponseMessage res;
        res.id = req.id;
        res.result = Json(nullptr);

        if (req.params.has("textDocument") && req.params.has("position")) {
            std::string uri = req.params["textDocument"]["uri"].strVal;
            Position pos = Position::fromJson(req.params["position"]);
            
            Document* doc = workspace.getDocument(uri);
            if (doc) {
                try {
                    Lexer lexer(doc->content, uri);
                    lexer.keepComments = true;
                    auto tokens = lexer.tokenize();
                    
                    std::vector<Token> parserTokens;
                    for (const auto& t : tokens) {
                        if (t.type != TokenType::COMMENT) parserTokens.push_back(t);
                    }
                    
                    Parser parser(parserTokens, uri);
                    parser.isLspMode = true;
                    auto ast = parser.parse();
                    
                    NameResolver resolver(doc, builtinIndex);
                    resolver.resolve(ast.get());
                    TypeInferrer inferrer(doc, resolver, builtinIndex);
                    inferrer.infer(ast.get());
                    
                    // 先处理类方法（math.BaseNum.*）与类实例方法，避免解析器把它们当普通属性
                    {
                        int offset = doc->positionToOffset(pos);
                        int start = offset, end = offset;
                        while (start > 0 && (std::isalnum(static_cast<unsigned char>(doc->content[start - 1])) || doc->content[start - 1] == '_')) start--;
                        while (end < static_cast<int>(doc->content.length()) && (std::isalnum(static_cast<unsigned char>(doc->content[end])) || doc->content[end] == '_')) end++;
                        std::string word, qualifier;
                        if (start < end) {
                            word = doc->content.substr(start, end - start);
                            int qpos = start - 1;
                            while (qpos >= 0 && std::isspace(static_cast<unsigned char>(doc->content[qpos]))) qpos--;
                            if (qpos >= 0 && doc->content[qpos] == '.') {
                                int qe = qpos - 1;
                                while (qe >= 0 && std::isspace(static_cast<unsigned char>(doc->content[qe]))) qe--;
                                int qs = qe;
                                while (qs >= 0 && (std::isalnum(static_cast<unsigned char>(doc->content[qs])) || doc->content[qs] == '_')) qs--;
                                if (qs < qe) qualifier = doc->content.substr(qs + 1, qe - qs);
                            }
                        }
                        std::string owner;
                        if (!qualifier.empty()) {
                            owner = qualifier;
                            for (auto* sym : resolver.visibleSymbolsAt(pos)) {
                                if (sym->name == qualifier) {
                                    std::string t = sym->inferredType.toString();
                                    if (t.empty()) t = sym->typeHint;
                                    if (!t.empty()) owner = t;
                                    break;
                                }
                            }
                            if (!builtinIndex.hasClassMethods(owner)) {
                                std::string guessed = classFromRhs(assignedRhs(doc->content, qualifier, offset), builtinIndex);
                                if (!guessed.empty()) owner = guessed;
                            }
                        }
                        if (!owner.empty() && builtinIndex.hasClassMethods(owner)) {
                            if (const BuiltinSymbol* b = builtinIndex.findClassMethod(owner, word)) {
                                std::string md = "```jc2\n(module_member) " + (b->signatureText.empty() ? owner + "." + word : b->signatureText) + "\n```";
                                if (!b->desc.empty()) md += "\n---\n" + b->desc;
                                if (!b->examples.empty()) {
                                    md += "\n\n**Examples:**\n```jc2\n";
                                    for (const auto& ex : b->examples) md += ex + "\n";
                                    md += "```";
                                }
                                Json hover;
                                hover.type = JsonType::Object;
                                Json contents;
                                contents.type = JsonType::Object;
                                contents["kind"] = Json("markdown");
                                contents["value"] = Json(md);
                                hover["contents"] = contents;
                                res.result = hover;
                                sendMessage(res.toJson().serialize());
                                return;
                            }
                        }
                    }

                    const NameRes* nr = resolver.resolveAtPos(pos);
                    if (nr) {
                        Json hover;
                        hover.type = JsonType::Object;
                        Json contents;
                        contents.type = JsonType::Object;
                        contents["kind"] = Json("markdown");
                        
                        std::string md;
                        if (nr->origin == NameRes::User && nr->user) {
                            std::string kindStr = "variable";
                            if (nr->user->kind == UserSymbol::Function) kindStr = "function";
                            else if (nr->user->kind == UserSymbol::Class) kindStr = "class";
                            else if (nr->user->kind == UserSymbol::Trait) kindStr = "interface";
                            else if (nr->user->kind == UserSymbol::Parameter) kindStr = "parameter";
                            else if (nr->user->kind == UserSymbol::Property) kindStr = "property";
                            else if (nr->user->kind == UserSymbol::Namespace) kindStr = "namespace";
                            md = "```jc2\n(" + kindStr + ") " + nr->user->name;
                            if (!nr->user->typeHint.empty()) md += ": " + nr->user->typeHint;
                            else if (!nr->user->inferredType.isNever() && !nr->user->inferredType.isAny()) {
                                md += ": " + nr->user->inferredType.toString();
                            }
                            md += "\n```";
                        } else if (nr->origin == NameRes::Builtin && nr->builtin) {
                            const BuiltinSymbol& b = *nr->builtin;
                            std::string kindStr = (b.kind == BuiltinKind::Type) ? "type" : (b.kind == BuiltinKind::Class) ? "class" : "builtin_function";
                            md = "```jc2\n(" + kindStr + ") " + (b.signatureText.empty() ? b.name : b.signatureText) + "\n```";
                            if (!b.desc.empty()) md += "\n---\n" + b.desc;
                            if (!b.examples.empty()) {
                                md += "\n\n**Examples:**\n```jc2\n";
                                for (const auto& ex : b.examples) md += ex + "\n";
                                md += "```";
                            }
                        } else if (nr->origin == NameRes::Imported && nr->member) {
                            md = "```jc2\n(module_member) " + nr->moduleName + "." + nr->member->name + "\n```";
                            if (!nr->member->desc.empty()) md += "\n---\n" + nr->member->desc;
                        }
                        if (!md.empty()) {
                            contents["value"] = Json(md);
                            hover["contents"] = contents;
                            res.result = hover;
                        }
                    } else {
                        // Fallback: 检查是否是内置函数或关键字
                        int offset = doc->positionToOffset(pos);
                        std::string word;
                        int start = offset;
                        while (start > 0 && (std::isalnum(static_cast<unsigned char>(doc->content[start - 1])) || doc->content[start - 1] == '_')) {
                            start--;
                        }
                        int end = offset;
                        while (end < static_cast<int>(doc->content.length()) && (std::isalnum(static_cast<unsigned char>(doc->content[end])) || doc->content[end] == '_')) {
                            end++;
                        }
                        if (start < end) {
                            word = doc->content.substr(start, end - start);

                            std::string qualifier;
                            int qpos = start - 1;
                            while (qpos >= 0 && std::isspace(static_cast<unsigned char>(doc->content[qpos]))) qpos--;
                            if (qpos >= 0 && doc->content[qpos] == '.') {
                                int qe = qpos - 1;
                                while (qe >= 0 && std::isspace(static_cast<unsigned char>(doc->content[qe]))) qe--;
                                int qs = qe;
                                while (qs >= 0 && (std::isalnum(static_cast<unsigned char>(doc->content[qs])) || doc->content[qs] == '_')) qs--;
                                if (qs < qe) qualifier = doc->content.substr(qs + 1, qe - qs);
                            }

                            jc::HelpRouter::init();
                            const Json& helpAst = jc::HelpRouter::helpAst;
                            if (!qualifier.empty()) {
                                if (const BuiltinSymbol* b = builtinIndex.findClassMethod(qualifier, word)) {
                                    std::string md = "```jc2\n(module_member) " + qualifier + "." + word + "\n```";
                                    if (!b->signatureText.empty()) md = "```jc2\n(module_member) " + b->signatureText + "\n```";
                                    if (!b->desc.empty()) md += "\n---\n" + b->desc;
                                    if (!b->examples.empty()) {
                                        md += "\n\n**Examples:**\n```jc2\n";
                                        for (const auto& ex : b->examples) md += ex + "\n";
                                        md += "```";
                                    }
                                    Json hover;
                                    hover.type = JsonType::Object;
                                    Json contents;
                                    contents.type = JsonType::Object;
                                    contents["kind"] = Json("markdown");
                                    contents["value"] = Json(md);
                                    hover["contents"] = contents;
                                    res.result = hover;
                                    sendMessage(res.toJson().serialize());
                                    return;
                                }
                            }
                            if (helpAst.isObject()) {
                                Json targetData;
                                std::string kindStr;
                                
                                std::vector<std::string> categories = {
                                    "global_functions", "matrix_methods", "list_methods", 
                                    "string_methods", "dict_methods", "set_methods",
                                    "sys_methods", "math_methods", "cas_methods", "random_methods"
                                };
                                
                                for (const auto& cat : categories) {
                                    if (helpAst.has(cat) && helpAst[cat].has(word)) {
                                        targetData = helpAst[cat][word];
                                        kindStr = (cat == "global_functions") ? "builtin_function" : "builtin_method";
                                        break;
                                    }
                                }
                                if (targetData.isNull()) {
                                    for (const auto& cat : categories) {
                                        if (!helpAst.has(cat) || !helpAst[cat].isObject()) continue;
                                        for (const auto& [key, entry] : helpAst[cat].objVal) {
                                            if (key.size() > word.size() + 1 && key.compare(key.size() - word.size(), word.size(), word) == 0
                                                && key[key.size() - word.size() - 1] == '.') {
                                                targetData = entry;
                                                kindStr = "builtin_method";
                                                break;
                                            }
                                        }
                                        if (!targetData.isNull()) break;
                                    }
                                }
                                
                                if (targetData.isNull() && helpAst.has("keywords") && helpAst["keywords"].has(word)) {
                                    targetData = helpAst["keywords"][word];
                                    kindStr = "keyword";
                                }
                                
                                if (!targetData.isNull()) {
                                    Json hover;
                                    hover.type = JsonType::Object;
                                    Json contents;
                                    contents.type = JsonType::Object;
                                    contents["kind"] = Json("markdown");
                                    
                                    std::string md = "```jc2\n(" + kindStr + ") ";
                                    if (targetData.has("signature")) md += targetData["signature"].strVal;
                                    else md += word;
                                    md += "\n```";
                                    
                                    if (targetData.has("desc")) {
                                        md += "\n---\n";
                                        if (targetData["desc"].isString()) md += targetData["desc"].strVal;
                                        else if (targetData["desc"].isArray()) {
                                            for (const auto& line : targetData["desc"].arrVal) md += line.strVal + "\n";
                                        }
                                    }
                                    if (targetData.has("examples") && targetData["examples"].isArray()) {
                                        md += "\n\n**Examples:**\n```jc2\n";
                                        for (const auto& ex : targetData["examples"].arrVal) md += ex.strVal + "\n";
                                        md += "```";
                                    }
                                    
                                    contents["value"] = Json(md);
                                    hover["contents"] = contents;
                                    res.result = hover;
                                }
                            }
                        }
                    }
                } catch (...) {}
            }
        }
        sendMessage(res.toJson().serialize());
    }

    void LspServer::handleDefinition(const RequestMessage& req) {
        ResponseMessage res;
        res.id = req.id;
        res.result = Json(nullptr);

        if (req.params.has("textDocument") && req.params.has("position")) {
            std::string uri = req.params["textDocument"]["uri"].strVal;
            Position pos = Position::fromJson(req.params["position"]);
            
            Document* doc = workspace.getDocument(uri);
            if (doc) {
                try {
                    Lexer lexer(doc->content, uri);
                    lexer.keepComments = true;
                    auto tokens = lexer.tokenize();
                    
                    std::vector<Token> parserTokens;
                    for (const auto& t : tokens) {
                        if (t.type != TokenType::COMMENT) parserTokens.push_back(t);
                    }
                    
                    Parser parser(parserTokens, uri);
                    parser.isLspMode = true;
                    auto ast = parser.parse();
                    
                    NameResolver resolver(doc, builtinIndex);
                    resolver.resolve(ast.get());
                    
                    const NameRes* nr = resolver.resolveAtPos(pos);
                    if (nr && nr->origin == NameRes::User && nr->user) {
                        Json location;
                        location.type = JsonType::Object;
                        location["uri"] = Json(uri);
                        Range range;
                        range.start = nr->user->defPos;
                        range.end = nr->user->defEndPos;
                        location["range"] = range.toJson();
                        res.result = location;
                    }
                } catch (...) {}
            }
        }
        sendMessage(res.toJson().serialize());
    }

    void LspServer::handleCompletion(const RequestMessage& req) {
        ResponseMessage res;
        res.id = req.id;
        res.result = Json(nullptr);

        if (req.params.has("textDocument") && req.params.has("position")) {
            std::string uri = req.params["textDocument"]["uri"].strVal;
            Position pos = Position::fromJson(req.params["position"]);
            
            Document* doc = workspace.getDocument(uri);
            if (doc) {
                try {
                    Lexer lexer(doc->content, uri);
                    lexer.keepComments = true;
                    auto tokens = lexer.tokenize();
                    
                    std::vector<Token> parserTokens;
                    for (const auto& t : tokens) {
                        if (t.type != TokenType::COMMENT) parserTokens.push_back(t);
                    }
                    
                    Parser parser(parserTokens, uri);
                    parser.isLspMode = true;
                    auto ast = parser.parse();
                    
                    NameResolver resolver(doc, builtinIndex);
                    resolver.resolve(ast.get());
                    TypeInferrer inferrer(doc, resolver, builtinIndex);
                    inferrer.infer(ast.get());
                    
                    std::vector<Json> items;
                    
                    // ★ 上下文感知：检测是否是属性访问 (DotAccess)
                    int offset = doc->positionToOffset(pos);
                    bool isDotAccess = false;
                    std::string objectName = "";
                    
                    int tempOffset = offset - 1;
                    while (tempOffset >= 0 && std::isspace(static_cast<unsigned char>(doc->content[tempOffset]))) tempOffset--;
                    
                    // 如果光标前是一个标识符，再往前看是不是点
                    int wordStart = tempOffset;
                    while (wordStart >= 0 && (std::isalnum(static_cast<unsigned char>(doc->content[wordStart])) || doc->content[wordStart] == '_')) {
                        wordStart--;
                    }
                    
                    int dotCheckOffset = wordStart;
                    while (dotCheckOffset >= 0 && std::isspace(static_cast<unsigned char>(doc->content[dotCheckOffset]))) dotCheckOffset--;
                    
                    if (dotCheckOffset >= 0 && doc->content[dotCheckOffset] == '.') {
                        isDotAccess = true;
                        int objEnd = dotCheckOffset - 1;
                        while (objEnd >= 0 && std::isspace(static_cast<unsigned char>(doc->content[objEnd]))) objEnd--;
                        int objStart = objEnd;
                        while (objStart >= 0 && (std::isalnum(static_cast<unsigned char>(doc->content[objStart])) || doc->content[objStart] == '_')) {
                            objStart--;
                        }
                        if (objStart < objEnd) {
                            objectName = doc->content.substr(objStart + 1, objEnd - objStart);
                        }
                    } else if (tempOffset >= 0 && doc->content[tempOffset] == '.') {
                        isDotAccess = true;
                        int objEnd = tempOffset - 1;
                        while (objEnd >= 0 && std::isspace(static_cast<unsigned char>(doc->content[objEnd]))) objEnd--;
                        int objStart = objEnd;
                        while (objStart >= 0 && (std::isalnum(static_cast<unsigned char>(doc->content[objStart])) || doc->content[objStart] == '_')) {
                            objStart--;
                        }
                        if (objStart < objEnd) {
                            objectName = doc->content.substr(objStart + 1, objEnd - objStart);
                        }
                    }

                    jc::HelpRouter::init();
                    const Json& helpAst = jc::HelpRouter::helpAst;

                    if (isDotAccess) {
                        // 属性访问补全
                        std::string targetType = "";
                        if (!objectName.empty()) {
                            for (auto* s : resolver.visibleSymbolsAt(pos)) {
                                if (s->name == objectName) {
                                    targetType = s->inferredType.toString();
                                    if (targetType.empty() && !s->typeHint.empty()) targetType = s->typeHint;
                                    break;
                                }
                            }
                        }
                        
                        // 类方法（math.BaseNum.* 这类带点的文档条目）
                        std::string classOwner;
                        if (builtinIndex.hasClassMethods(objectName)) classOwner = objectName;
                        else if (!targetType.empty() && builtinIndex.hasClassMethods(targetType)) classOwner = targetType;
                        if (classOwner.empty() && !objectName.empty()) {
                            int cursor = doc->positionToOffset(pos);
                            classOwner = classFromRhs(assignedRhs(doc->content, objectName, cursor), builtinIndex);
                        }
                        if (!classOwner.empty()) {
                            for (const auto& [name, sym] : builtinIndex.classMethodsOf(classOwner)) {
                                Json item;
                                item.type = JsonType::Object;
                                item["label"] = Json(name);
                                item["kind"] = Json(2); // Method
                                if (!sym.signatureText.empty()) item["detail"] = Json(sym.signatureText);
                                if (!sym.desc.empty()) {
                                    Json docJson;
                                    docJson.type = JsonType::Object;
                                    docJson["kind"] = Json("markdown");
                                    std::string descStr = sym.desc;
                                    if (!sym.examples.empty()) {
                                        descStr += "\n\n**Examples:**\n```jc2\n";
                                        for (const auto& ex : sym.examples) descStr += ex + "\n";
                                        descStr += "```";
                                    }
                                    docJson["value"] = Json(descStr);
                                    item["documentation"] = docJson;
                                }
                                items.push_back(item);
                            }
                        }
                        // 根据推导出的类型，直接读取对应的内置方法分类
                        else if (helpAst.isObject()) {
                            std::vector<std::string> targetCategories;
                            if (targetType == "matrix" || targetType == "realmatrix" || targetType == "complexmatrix" || targetType == "symmatrix") {
                                targetCategories.push_back("matrix_methods");
                            } else if (targetType == "list") {
                                targetCategories.push_back("list_methods");
                            } else if (targetType == "string") {
                                targetCategories.push_back("string_methods");
                            } else if (targetType == "dict") {
                                targetCategories.push_back("dict_methods");
                            } else if (targetType == "set") {
                                targetCategories.push_back("set_methods");
                            } else if (objectName == "sys") {
                                targetCategories.push_back("sys_methods");
                            } else if (objectName == "math") {
                                targetCategories.push_back("math_methods");
                            } else if (objectName == "cas") {
                                targetCategories.push_back("cas_methods");
                            } else if (objectName == "random") {
                                targetCategories.push_back("random_methods");
                            } else if (targetType.empty() || targetType == "any") {
                                targetCategories = {"matrix_methods", "list_methods", "string_methods", "dict_methods", "set_methods", "sys_methods", "math_methods", "cas_methods", "random_methods"};
                            }
                            
                            for (const auto& cat : targetCategories) {
                                if (helpAst.has(cat)) {
                                    const Json& funcs = helpAst[cat];
                                    for (const auto& pair : funcs.objVal) {
                                        const std::string& funcName = pair.first;
                                        const Json& funcData = pair.second;
                                        
                                        if (funcName.length() > 4 && funcName.substr(0, 2) == "__" && funcName.substr(funcName.length()-2) == "__") continue;
                                        if (funcName.find('.') != std::string::npos) continue;
                                        
                                        Json item;
                                        item.type = JsonType::Object;
                                        item["label"] = Json(funcName);
                                        item["kind"] = Json(2); // Method
                                        if (funcData.has("signature")) item["detail"] = Json(funcData["signature"].strVal);
                                        if (funcData.has("desc")) {
                                            Json docJson;
                                            docJson.type = JsonType::Object;
                                            docJson["kind"] = Json("markdown");
                                            std::string descStr;
                                            if (funcData["desc"].isString()) descStr = funcData["desc"].strVal;
                                            else if (funcData["desc"].isArray()) {
                                                for (const auto& line : funcData["desc"].arrVal) descStr += line.strVal + "\n";
                                            }
                                            if (funcData.has("examples") && funcData["examples"].isArray()) {
                                                descStr += "\n\n**Examples:**\n```jc2\n";
                                                for (const auto& ex : funcData["examples"].arrVal) descStr += ex.strVal + "\n";
                                                descStr += "```";
                                            }
                                            docJson["value"] = Json(descStr);
                                            item["documentation"] = docJson;
                                        }
                                        items.push_back(item);
                                    }
                                }
                            }
                        }
                    } else {
                        // 普通全局/局部补全
                        auto visibleSymbols = resolver.visibleSymbolsAt(pos);
                        
                        std::unordered_set<std::string> visibleNames;
                        // 1. 添加当前作用域可见的符号
                        for (auto* sym : visibleSymbols) {
                            visibleNames.insert(sym->name);
                            Json item;
                            item.type = JsonType::Object;
                            item["label"] = Json(sym->name);
                            
                            int kind = 6; // Variable
                            if (sym->kind == UserSymbol::Function) kind = 3; // Function
                            else if (sym->kind == UserSymbol::Class) kind = 7; // Class
                            else if (sym->kind == UserSymbol::Trait) kind = 11; // Interface
                            else if (sym->kind == UserSymbol::Property) kind = 10; // Property
                            else if (sym->kind == UserSymbol::Namespace) kind = 9; // Module
                            
                            item["kind"] = Json(kind);
                            
                            if (!sym->typeHint.empty()) {
                                item["detail"] = Json(sym->typeHint);
                            } else if (!sym->inferredType.isNever()) {
                                item["detail"] = Json(sym->inferredType.toString());
                            }
                            
                            if (!sym->docstring.empty()) {
                                Json docJson;
                                docJson.type = JsonType::Object;
                                docJson["kind"] = Json("markdown");
                                docJson["value"] = Json(sym->docstring);
                                item["documentation"] = docJson;
                            }
                            
                            items.push_back(item);
                        }
                        
                        // 2. 添加 JC2 语言关键字 & 内置函数 (从 HelpRouter 获取)
                        if (helpAst.isObject() && helpAst.has("keywords")) {
                            for (const auto& pair : helpAst["keywords"].objVal) {
                                Json item;
                                item.type = JsonType::Object;
                                item["label"] = Json(pair.first);
                                item["kind"] = Json(14); // Keyword
                                items.push_back(item);
                            }
                        }
                        if (helpAst.isObject() && helpAst.has("global_functions")) {
                            const Json& funcs = helpAst["global_functions"];
                            for (const auto& pair : funcs.objVal) {
                                const std::string& funcName = pair.first;
                                const Json& funcData = pair.second;
                                
                                // 忽略 dunder methods (如 __add__)，除非用户主动输入
                                if (funcName.length() > 4 && funcName.substr(0, 2) == "__" && funcName.substr(funcName.length()-2) == "__") {
                                    continue;
                                }
                                
                                // 如果该内置函数已被用户局部变量覆盖，则不再提示内置函数
                                if (visibleNames.count(funcName)) {
                                    continue;
                                }
                                
                                Json item;
                                item.type = JsonType::Object;
                                item["label"] = Json(funcName);
                                item["kind"] = Json(3); // Function
                                
                                if (funcData.has("signature")) {
                                    item["detail"] = Json(funcData["signature"].strVal);
                                }
                                
                                if (funcData.has("desc")) {
                                    Json docJson;
                                    docJson.type = JsonType::Object;
                                    docJson["kind"] = Json("markdown");
                                    std::string descStr;
                                    if (funcData["desc"].isString()) descStr = funcData["desc"].strVal;
                                    else if (funcData["desc"].isArray()) {
                                        for (const auto& line : funcData["desc"].arrVal) descStr += line.strVal + "\n";
                                    }
                                    if (funcData.has("examples") && funcData["examples"].isArray()) {
                                        descStr += "\n\n**Examples:**\n```jc2\n";
                                        for (const auto& ex : funcData["examples"].arrVal) descStr += ex.strVal + "\n";
                                        descStr += "```";
                                    }
                                    docJson["value"] = Json(descStr);
                                    item["documentation"] = docJson;
                                }
                                items.push_back(item);
                            }
                        }
                    }
                    
                    res.result = Json(items);
                } catch (...) {}
            }
        }
        sendMessage(res.toJson().serialize());
    }

    void LspServer::handleFormatting(const RequestMessage& req) {
        ResponseMessage res;
        res.id = req.id;
        res.result = Json(nullptr);

        if (req.params.has("textDocument")) {
            std::string uri = req.params["textDocument"]["uri"].strVal;
            Document* doc = workspace.getDocument(uri);
            if (doc) {
                try {
                    std::string formatted = jc::Formatter::format(doc->content);
                    if (formatted != doc->content) {
                        TextEdit edit;
                        edit.range.start = {0, 0};
                        edit.range.end = {999999, 0}; // 覆盖整个文档
                        edit.newText = formatted;
                        
                        std::vector<Json> edits;
                        edits.push_back(edit.toJson());
                        res.result = Json(edits);
                    } else {
                        res.result = Json(std::vector<Json>()); // 无需修改
                    }
                } catch (...) {}
            }
        }
        sendMessage(res.toJson().serialize());
    }

    void LspServer::handleSignatureHelp(const RequestMessage& req) {
        ResponseMessage res;
        res.id = req.id;
        res.result = Json(nullptr);

        if (req.params.has("textDocument") && req.params.has("position")) {
            std::string uri = req.params["textDocument"]["uri"].strVal;
            Position pos = Position::fromJson(req.params["position"]);
            
            Document* doc = workspace.getDocument(uri);
            if (doc) {
                try {
                    int offset = doc->positionToOffset(pos);
                    if (offset > 0 && offset <= static_cast<int>(doc->content.length())) {
                        int openParenIdx = -1;
                        int parenCount = 0;
                        int activeParam = 0;
                        
                        // 向前扫描寻找未闭合的左括号 '('
                        for (int i = offset - 1; i >= 0; --i) {
                            char c = doc->content[i];
                            if (c == ')') parenCount++;
                            else if (c == '(') {
                                if (parenCount == 0) {
                                    openParenIdx = i;
                                    break;
                                }
                                parenCount--;
                            } else if (c == ',' && parenCount == 0) {
                                activeParam++;
                            }
                        }
                        
                        if (openParenIdx > 0) {
                            int nameEnd = openParenIdx - 1;
                            while (nameEnd >= 0 && std::isspace(static_cast<unsigned char>(doc->content[nameEnd]))) nameEnd--;
                            int nameStart = nameEnd;
                            while (nameStart >= 0 && (std::isalnum(static_cast<unsigned char>(doc->content[nameStart])) || doc->content[nameStart] == '_')) {
                                nameStart--;
                            }
                            nameStart++;
                            
                            if (nameStart <= nameEnd) {
                                std::string funcName = doc->content.substr(nameStart, nameEnd - nameStart + 1);
                                
                                jc::HelpRouter::init();
                                const Json& helpAst = jc::HelpRouter::helpAst;
                                if (helpAst.isObject() && helpAst.has("functions")) {
                                    const Json& funcs = helpAst["functions"];
                                    if (funcs.has(funcName)) {
                                        const Json& funcData = funcs[funcName];
                                        if (funcData.has("signature")) {
                                            std::string sig = funcData["signature"].strVal;
                                            
                                            Json sigInfo;
                                            sigInfo.type = JsonType::Object;
                                            sigInfo["label"] = Json(sig);
                                            
                                            if (funcData.has("desc")) {
                                                Json docJson;
                                                docJson.type = JsonType::Object;
                                                docJson["kind"] = Json("markdown");
                                                
                                                std::string descStr = funcData["desc"].strVal;
                                                if (funcData.has("examples") && funcData["examples"].isArray()) {
                                                    descStr += "\n\n**Examples:**\n```jc2\n";
                                                    for (const auto& ex : funcData["examples"].arrVal) {
                                                        descStr += ex.strVal + "\n";
                                                    }
                                                    descStr += "\n```";
                                                }
                                                docJson["value"] = Json(descStr);
                                                sigInfo["documentation"] = docJson;
                                            }
                                            
                                            std::vector<Json> params;
                                            size_t pStart = sig.find('(');
                                            size_t pEnd = sig.find(')');
                                            if (pStart != std::string::npos && pEnd != std::string::npos && pEnd > pStart) {
                                                std::string pStr = sig.substr(pStart + 1, pEnd - pStart - 1);
                                                std::stringstream ss(pStr);
                                                std::string item;
                                                while (std::getline(ss, item, ',')) {
                                                    size_t s = item.find_first_not_of(" \t");
                                                    size_t e = item.find_last_not_of(" \t");
                                                    if (s != std::string::npos) item = item.substr(s, e - s + 1);
                                                    
                                                    Json pInfo;
                                                    pInfo.type = JsonType::Object;
                                                    pInfo["label"] = Json(item);
                                                    params.push_back(pInfo);
                                                }
                                            }
                                            sigInfo["parameters"] = Json(params);
                                            
                                            Json sigHelp;
                                            sigHelp.type = JsonType::Object;
                                            std::vector<Json> signatures;
                                            signatures.push_back(sigInfo);
                                            sigHelp["signatures"] = Json(signatures);
                                            sigHelp["activeSignature"] = Json(0);
                                            sigHelp["activeParameter"] = Json(activeParam);
                                            
                                            res.result = sigHelp;
                                        }
                                    }
                                }
                            }
                        }
                    }
                } catch (...) {}
            }
        }
        sendMessage(res.toJson().serialize());
    }

    void LspServer::handleDocumentSymbol(const RequestMessage& req) {
        ResponseMessage res;
        res.id = req.id;
        res.result = Json(nullptr);

        if (req.params.has("textDocument")) {
            std::string uri = req.params["textDocument"]["uri"].strVal;
            Document* doc = workspace.getDocument(uri);
            if (doc) {
                try {
                    Lexer lexer(doc->content, uri);
                    lexer.keepComments = true;
                    auto tokens = lexer.tokenize();
                    
                    std::vector<Token> parserTokens;
                    for (const auto& t : tokens) {
                        if (t.type != TokenType::COMMENT) parserTokens.push_back(t);
                    }
                    
                    Parser parser(parserTokens, uri);
                    parser.isLspMode = true;
                    auto ast = parser.parse();
                    
                    NameResolver resolver(doc, builtinIndex);
                    resolver.resolve(ast.get());
                    
                    std::vector<Json> symJsons;
                    for (const UserSymbol* sym : resolver.documentSymbols()) {
                        Json ds;
                        ds.type = JsonType::Object;
                        ds["name"] = Json(sym->name);
                        ds["detail"] = Json(sym->typeHint);
                        int kind = 13; // Variable
                        if (sym->kind == UserSymbol::Function) kind = 12;
                        else if (sym->kind == UserSymbol::Class) kind = 5;
                        else if (sym->kind == UserSymbol::Trait) kind = 11;
                        else if (sym->kind == UserSymbol::Namespace) kind = 2;
                        ds["kind"] = Json((double)kind);
                        Range range;
                        range.start = sym->defPos;
                        range.end = sym->defEndPos;
                        ds["range"] = range.toJson();
                        ds["selectionRange"] = range.toJson();
                        symJsons.push_back(ds);
                    }
                    res.result = Json(symJsons);
                } catch (...) {}
            }
        }
        sendMessage(res.toJson().serialize());
    }

    void LspServer::handleSemanticTokens(const RequestMessage& req) {
        ResponseMessage res;
        res.id = req.id;
        res.result = Json(nullptr);

        if (req.params.has("textDocument")) {
            std::string uri = req.params["textDocument"]["uri"].strVal;
            Document* doc = workspace.getDocument(uri);
            if (doc) {
                try {
                    Lexer lexer(doc->content, uri);
                    lexer.keepComments = true;
                    auto tokens = lexer.tokenize();
                    
                    std::vector<uint32_t> data;
                    int prevLine = 0;
                    int prevChar = 0;

                    auto addToken = [&](int line, int startChar, int length, int tokenType, int tokenModifiers) {
                        int deltaLine = line - prevLine;
                        int deltaStartChar = (deltaLine == 0) ? (startChar - prevChar) : startChar;
                        data.push_back(deltaLine);
                        data.push_back(deltaStartChar);
                        data.push_back(length);
                        data.push_back(tokenType);
                        data.push_back(tokenModifiers);
                        prevLine = line;
                        prevChar = startChar;
                    };

                    // ★ f-string：字面段发 string，{...} 插值段再走一遍 Lexer 独立着色。
                    //   依据源文本切片（token.rawLength），与整段染成字符串的老做法不同。
                    auto emitSpan = [&](const std::string& seg, int segStartOffset, int spanType) {
                        size_t st = 0;
                        while (st < seg.size()) {
                            size_t nl = seg.find('\n', st);
                            std::string lineStr = (nl == std::string::npos) ? seg.substr(st)
                                                                            : seg.substr(st, nl - st);
                            int len = static_cast<int>(jc::utf8::charIndex(lineStr, lineStr.length()));
                            if (len > 0) {
                                Position sp = doc->offsetToPosition(segStartOffset + static_cast<int>(st));
                                addToken(sp.line, sp.character, len, spanType, 0);
                            }
                            if (nl == std::string::npos) break;
                            st = nl + 1;
                        }
                    };
                    auto emitFString = [&](const std::string& src, int basePos) {
                        size_t i = 0;
                        while (i < src.size() && (src[i] == 'f' || src[i] == 'F' || src[i] == 'r' || src[i] == 'R')) i++;
                        size_t q = 0;
                        while (i + q < src.size() && src[i + q] == '"') q++;
                        if (q == 0) { while (i + q < src.size() && src[i + q] == '\'') q++; }
                        if (q == 0) { emitSpan(src, basePos, 18); return; }
                        size_t qn = (q >= 3) ? 3 : 1;
                        size_t bodyA = i + qn;
                        size_t bodyB = (src.size() >= qn) ? src.size() - qn : bodyA;
                        emitSpan(src.substr(0, bodyA), basePos, 18);          // 前缀：f" 或 f"""
                        size_t seg = bodyA, k = bodyA;
                        while (k < bodyB) {
                            if (src[k] == '{' && k + 1 < bodyB && src[k + 1] == '{') { k += 2; continue; }
                            if (src[k] == '}' && k + 1 < bodyB && src[k + 1] == '}') { k += 2; continue; }
                            if (src[k] != '{') { ++k; continue; }
                            emitSpan(src.substr(seg, k - seg), basePos + static_cast<int>(seg), 18);
                            int depth = 0; char inq = 0; size_t j = k;
                            for (; j < bodyB; ++j) {
                                char c = src[j];
                                if (inq) {
                                    if (c == '\\') { ++j; continue; }
                                    if (c == inq) inq = 0;
                                    continue;
                                }
                                if (c == '"' || c == '\'') { inq = c; continue; }
                                if (c == '{') depth++;
                                else if (c == '}') { depth--; if (depth == 0) break; }
                            }
                            size_t innerA = k + 1, innerB = (j < bodyB) ? j : bodyB;
                            std::string inner = src.substr(innerA, innerB - innerA);
                            if (!inner.empty()) {
                                Lexer innerLexer(inner, uri);
                                auto innerToks = innerLexer.tokenize();
                                for (const auto& it : innerToks) {
                                    int tt = -1;
                                    switch (it.type) {
                                        case TokenType::IDENTIFIER: tt = 8;  break; // variable
                                        case TokenType::NUMBER:
                                        case TokenType::IMAGINARY:  tt = 19; break; // number
                                        case TokenType::STRING:
                                        case TokenType::RSTRING:
                                        case TokenType::FSTRING:    tt = 18; break; // string
                                        case TokenType::TRUE_KW: case TokenType::FALSE_KW:
                                        case TokenType::NONE_KW:    tt = 15; break; // keyword
                                        default:                    tt = 21; break; // operator
                                    }
                                    const int rawLen = (it.rawLength >= 0) ? it.rawLength
                                                                          : static_cast<int>(it.lexeme.size());
                                    std::string piece = inner.substr(static_cast<size_t>(it.position),
                                                                     static_cast<size_t>(rawLen));
                                    emitSpan(piece, basePos + static_cast<int>(innerA) + it.position, tt);
                                }
                            }
                            k = (j < bodyB) ? j + 1 : bodyB;
                            seg = k;
                        }
                        emitSpan(src.substr(seg, bodyB - seg), basePos + static_cast<int>(seg), 18);
                        emitSpan(src.substr(bodyB), basePos + static_cast<int>(bodyB), 18);  // 收尾引号
                    };

                    // 结合 NameResolver 获取更精确的符号类型
                    NameResolver resolver(doc, builtinIndex);
                    std::vector<Token> parserTokens;
                    for (const auto& t : tokens) {
                        if (t.type != TokenType::COMMENT) parserTokens.push_back(t);
                    }
                    Parser parser(parserTokens, uri);
                    parser.isLspMode = true;
                    auto ast = parser.parse();
                    resolver.resolve(ast.get());

                    int braceDepth = 0;
                    int enumBraceDepth = -1;
                    bool expectEnumBrace = false;

                    for (size_t i = 0; i < tokens.size(); ++i) {
                        const auto& t = tokens[i];
                        int tokenType = -1;
                        int tokenModifiers = 0;

                        if (t.type == TokenType::LBRACE) {
                            braceDepth++;
                            if (expectEnumBrace) {
                                enumBraceDepth = braceDepth;
                                expectEnumBrace = false;
                            }
                        } else if (t.type == TokenType::RBRACE) {
                            if (braceDepth == enumBraceDepth) {
                                enumBraceDepth = -1;
                            }
                            braceDepth--;
                        } else if (t.type == TokenType::ENUM) {
                            expectEnumBrace = true;
                        } else if (t.type != TokenType::COMMENT && t.type != TokenType::NEWLINE && t.type != TokenType::IDENTIFIER) {
                            expectEnumBrace = false;
                        }

                        bool isAfterDot = false;
                        if (i > 0) {
                            int prevIdx = static_cast<int>(i) - 1;
                            while (prevIdx >= 0 && (tokens[prevIdx].type == TokenType::COMMENT || tokens[prevIdx].type == TokenType::NEWLINE)) {
                                prevIdx--;
                            }
                            if (prevIdx >= 0 && tokens[prevIdx].type == TokenType::DOT) {
                                isAfterDot = true;
                            }
                        }

                        bool isKeyword = false;
                        bool isControlFlow = false;

                        switch (t.type) {
                            // 控制流关键字（controlFlow，紫）
                            case TokenType::IF: case TokenType::ELSE: case TokenType::WHILE:
                            case TokenType::FOR: case TokenType::BREAK: case TokenType::CONTINUE:
                            case TokenType::RETURN: case TokenType::THROW: case TokenType::TRY:
                            case TokenType::CATCH: case TokenType::MATCH: case TokenType::DEFER:
                                isControlFlow = true;
                                break;
                            // 其余关键字 + 常量 + 声明（keyword，蓝）
                            case TokenType::IN: case TokenType::IS: case TokenType::AS:
                            case TokenType::TRUE_KW: case TokenType::FALSE_KW: case TokenType::NONE_KW:
                            case TokenType::CLASS: case TokenType::ENUM: case TokenType::NAMESPACE:
                            case TokenType::TRAIT: case TokenType::WITH:
                            case TokenType::IMPORT: case TokenType::MACRO: case TokenType::SYNTAX:
                            case TokenType::QUOTE:
                            case TokenType::STATIC: case TokenType::LOCAL: case TokenType::CONST:
                            case TokenType::REF: case TokenType::STATE: case TokenType::DELETE:
                            case TokenType::EXTENDS:
                                isKeyword = true;
                                break;
                            default:
                                break;
                        }

                        if (isControlFlow || isKeyword || t.type == TokenType::IDENTIFIER) {
                            if (isAfterDot) {
                                tokenType = 9; // property（默认）
                                Position dotPos = doc->offsetToPosition(t.position);
                                const NameRes* dotNr = resolver.resolveAtPos(dotPos);
                                if (dotNr) {
                                    if (dotNr->origin == NameRes::Imported && dotNr->member) tokenType = 12; // 模块成员 function
                                    else if (dotNr->isMethod) tokenType = 13; // method
                                }
                            } else if (enumBraceDepth != -1) {
                                // 在 enum 块内，判断是否是枚举成员定义
                                bool isEnumMember = false;
                                int nextIdx = static_cast<int>(i) + 1;
                                while (nextIdx < static_cast<int>(tokens.size()) && tokens[nextIdx].type == TokenType::COMMENT) {
                                    nextIdx++;
                                }
                                if (nextIdx < static_cast<int>(tokens.size())) {
                                    auto nt = tokens[nextIdx].type;
                                    if (nt == TokenType::COMMA || nt == TokenType::ASSIGN || nt == TokenType::RBRACE || nt == TokenType::NEWLINE) {
                                        isEnumMember = true;
                                    }
                                }
                                if (isEnumMember) {
                                    tokenType = 10; // enumMember
                                } else {
                                    tokenType = isControlFlow ? 15 : (isKeyword ? 16 : 8);
                                }
                            } else if (isControlFlow) {
                                tokenType = 15; // controlFlow
                            } else if (isKeyword) {
                                tokenType = 16; // keyword
                            } else {
                                tokenType = 8; // variable
                                Position pos = doc->offsetToPosition(t.position);
                                const NameRes* nr = resolver.resolveAtPos(pos);
                                if (nr) {
                                    if (nr->origin == NameRes::Builtin && nr->builtin) {
                                        if (nr->builtin->kind == BuiltinKind::Type) tokenType = 1; // type
                                        else if (nr->builtin->kind == BuiltinKind::Class) tokenType = 2; // class
                                        else tokenType = 12; // function
                                    } else if (nr->origin == NameRes::Imported) {
                                        tokenType = nr->isMethod ? 13 : 12; // method / function
                                    } else if (nr->origin == NameRes::User && nr->user) {
                                        if (nr->user->kind == UserSymbol::Function) tokenType = 12;
                                        else if (nr->user->kind == UserSymbol::Class) tokenType = 2;
                                        else if (nr->user->kind == UserSymbol::Trait) tokenType = 2;
                                        else if (nr->user->kind == UserSymbol::Parameter) tokenType = 7;
                                        else if (nr->user->kind == UserSymbol::Property) tokenType = 9;
                                        else if (nr->user->kind == UserSymbol::Namespace) tokenType = 0;
                                    }
                                }
                            }
                        } else {
                            switch (t.type) {
                                case TokenType::NUMBER:
                                case TokenType::IMAGINARY:
                                    tokenType = 19; // number
                                    break;
                                case TokenType::STRING:
                                case TokenType::RSTRING:
                                case TokenType::FSTRING:
                                    tokenType = 18; // string
                                    break;
                                // FSTRING 不标记 semantic token：交给 tmLanguage 的 fstrings/fstring-interpolation，
                                // 否则整个 f-string（含 {} 内部）被整体标为 string，覆盖插值高亮。
                                case TokenType::COMMENT:
                                    tokenType = 17; // comment
                                    break;
                                case TokenType::PLUS:
                                case TokenType::MINUS:
                                case TokenType::STAR:
                                case TokenType::SLASH:
                                case TokenType::CARET:
                                case TokenType::PERCENT:
                                case TokenType::ASSIGN:
                                case TokenType::EQUAL:
                                case TokenType::BANG_EQUAL:
                                case TokenType::LESS:
                                case TokenType::LESS_EQUAL:
                                case TokenType::GREATER:
                                case TokenType::GREATER_EQUAL:
                                case TokenType::AND_AND:
                                case TokenType::OR_OR:
                                case TokenType::BANG:
                                case TokenType::BIT_AND:
                                case TokenType::BIT_OR:
                                case TokenType::BIT_XOR:
                                case TokenType::TILDE:
                                case TokenType::SHIFT_LEFT:
                                case TokenType::SHIFT_RIGHT:
                                case TokenType::PLUS_ASSIGN:
                                case TokenType::MINUS_ASSIGN:
                                case TokenType::STAR_ASSIGN:
                                case TokenType::SLASH_ASSIGN:
                                case TokenType::CARET_ASSIGN:
                                case TokenType::PERCENT_ASSIGN:
                                case TokenType::BIT_AND_ASSIGN:
                                case TokenType::BIT_OR_ASSIGN:
                                case TokenType::BIT_XOR_ASSIGN:
                                case TokenType::SHIFT_LEFT_ASSIGN:
                                case TokenType::SHIFT_RIGHT_ASSIGN:
                                case TokenType::TILDE_SLASH:
                                case TokenType::TILDE_SLASH_ASSIGN:
                                case TokenType::BACKSLASH:
                                case TokenType::BACKSLASH_ASSIGN:
                                case TokenType::PIPE:
                                case TokenType::ARROW:
                                case TokenType::ELLIPSIS:
                                case TokenType::QUESTION:
                                case TokenType::SUBSET:
                                    tokenType = 21; // operator
                                    break;
                                default:
                                    break;
                            }
                        }

                        if (tokenType != -1) {
                            Position pos = doc->offsetToPosition(t.position);
                            // ★ span 取自**源文本**：t.lexeme 对字符串/注释是解码后的值
                            //   （引号被去掉、转义还原、多行内容含真实换行），按它算长度会把
                            //   多行字符串错误地落到下一行、单行字符串只染开头引号。
                            const int rawLen = (t.rawLength >= 0) ? t.rawLength
                                                                  : static_cast<int>(t.lexeme.size());
                            std::string lexeme = doc->content.substr(static_cast<size_t>(t.position),
                                                                    static_cast<size_t>(rawLen));
                            if (t.type == TokenType::FSTRING) {
                                emitFString(lexeme, t.position);
                                continue;
                            }
                            int currentLine = pos.line;
                            int currentChar = pos.character;
                            
                            size_t startIdx = 0;
                            while (startIdx < lexeme.length()) {
                                size_t nlIdx = lexeme.find('\n', startIdx);
                                std::string lineStr;
                                if (nlIdx == std::string::npos) {
                                    lineStr = lexeme.substr(startIdx);
                                } else {
                                    lineStr = lexeme.substr(startIdx, nlIdx - startIdx);
                                }
                                
                                int len = static_cast<int>(jc::utf8::charIndex(lineStr, lineStr.length()));
                                if (len > 0) {
                                    addToken(currentLine, currentChar, len, tokenType, tokenModifiers);
                                }
                                
                                if (nlIdx == std::string::npos) break;
                                
                                currentLine++;
                                currentChar = 0;
                                startIdx = nlIdx + 1;
                            }
                        }
                    }

                    Json result;
                    result.type = JsonType::Object;
                    std::vector<Json> dataJson;
                    for (uint32_t v : data) {
                        dataJson.push_back(Json(static_cast<double>(v)));
                    }
                    result["data"] = Json(dataJson);
                    res.result = result;
                } catch (const std::exception& e) {
                    std::cerr << "[LSP] semanticTokens error: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "[LSP] semanticTokens error: unknown" << std::endl;
                }
            }
        }
        sendMessage(res.toJson().serialize());
    }

    void LspServer::publishDiagnostics(const std::string& uri, Document* doc) {
        if (!doc) return;

        std::vector<Json> diagnosticsJson;

        try {
            // 1. 词法分析
            Lexer lexer(doc->content, uri);
            lexer.keepComments = true;
            auto tokens = lexer.tokenize();

            // 收集词法错误
            std::vector<Token> parserTokens;
            for (const auto& tok : tokens) {
                if (tok.type == TokenType::COMMENT) continue;
                parserTokens.push_back(tok);
                
                if (tok.type == TokenType::ERROR) {
                    Json diag;
                    diag.type = JsonType::Object;
                    
                    Range range;
                    range.start = doc->offsetToPosition(tok.position);
                    range.end = doc->offsetToPosition(tok.position + 1); // 词法错误通常标记单个字符
                    
                    diag["range"] = range.toJson();
                    diag["severity"] = Json(1); // 1 = Error
                    diag["message"] = Json(tok.lexeme); // Lexer 将错误信息存在 lexeme 中
                    
                    diagnosticsJson.push_back(diag);
                }
            }

            // 2. 语法分析 (开启 LSP 容错模式)
            Parser parser(parserTokens, uri);
            parser.isLspMode = true;
            auto ast = parser.parse();

            // 3. 语义分析（NameResolver + TypeInferrer + TypeChecker）
            NameResolver resolver(doc, builtinIndex);
            resolver.resolve(ast.get());
            TypeInferrer inferrer(doc, resolver, builtinIndex);
            inferrer.infer(ast.get());
            TypeChecker checker(doc, resolver, builtinIndex, inferrer);
            checker.check(ast.get());

            // 收集语法错误
            for (const auto& err : parser.diagnostics) {
                Json diag;
                diag.type = JsonType::Object;
                
                Range range;
                range.start = doc->offsetToPosition(err.startPos);
                range.end = doc->offsetToPosition(err.endPos > err.startPos ? err.endPos : err.startPos + 1);
                
                diag["range"] = range.toJson();
                diag["severity"] = Json(1); // 1 = Error
                diag["message"] = Json(err.message);
                
                diagnosticsJson.push_back(diag);
            }

            // 收集语义警告
            for (const auto& diag : checker.diagnostics) {
                Json d;
                d.type = JsonType::Object;
                
                Range range;
                range.start = doc->offsetToPosition(diag.startPos);
                range.end = doc->offsetToPosition(diag.endPos > diag.startPos ? diag.endPos : diag.startPos + 1);
                
                d["range"] = range.toJson();
                d["severity"] = Json(diag.severity);
                d["message"] = Json(diag.message);
                
                diagnosticsJson.push_back(d);
            }

        } catch (...) {
            // 工业级实现：如果发生致命的解析崩溃，静默忽略，不中断 LSP 服务
        }

        // 3. 发送诊断通知给编辑器
        NotificationMessage notif;
        notif.method = "textDocument/publishDiagnostics";
        notif.params.type = JsonType::Object;
        notif.params["uri"] = Json(uri);
        notif.params["diagnostics"] = Json(diagnosticsJson);

        sendMessage(notif.toJson().serialize());
    }

} // namespace lsp
} // namespace jc
