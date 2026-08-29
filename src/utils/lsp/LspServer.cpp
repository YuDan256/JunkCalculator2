#include "LspServer.h"
#include "../../frontend/Lexer.h"
#include "../../frontend/Parser.h"
#include "SemanticAnalyzer.h"
#include "../../utils/fmt/Formatter.h"
#include "../../vm/HelpRouter.h"
#include <iostream>
#include <string>
#include <vector>
#include <sstream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace jc {
namespace lsp {

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
                    
                    SemanticAnalyzer analyzer(doc, tokens);
                    analyzer.analyze(ast.get());
                    
                    auto sym = analyzer.getSymbolAt(pos);
                    if (sym) {
                        Json hover;
                        hover.type = JsonType::Object;
                        
                        Json contents;
                        contents.type = JsonType::Object;
                        contents["kind"] = Json("markdown");
                        
                        std::string kindStr = "variable";
                        if (sym->kind == SymbolKind::Function) kindStr = "function";
                        else if (sym->kind == SymbolKind::Class) kindStr = "class";
                        else if (sym->kind == SymbolKind::Parameter) kindStr = "parameter";
                        else if (sym->kind == SymbolKind::Property) kindStr = "property";
                        else if (sym->kind == SymbolKind::Namespace) kindStr = "namespace";
                        
                        std::string md = "```jc2\n(" + kindStr + ") " + sym->name;
                        if (!sym->typeHint.empty()) md += ": " + sym->typeHint;
                        md += "\n```";
                        
                        if (!sym->docstring.empty()) {
                            md += "\n---\n" + sym->docstring;
                        }
                        
                        contents["value"] = Json(md);
                        hover["contents"] = contents;
                        res.result = hover;
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
                            
                            jc::HelpRouter::init();
                            const Json& helpAst = jc::HelpRouter::helpAst;
                            if (helpAst.isObject()) {
                                Json targetData;
                                std::string kindStr;
                                if (helpAst.has("functions") && helpAst["functions"].has(word)) {
                                    targetData = helpAst["functions"][word];
                                    kindStr = "built-in function";
                                } else if (helpAst.has("keywords") && helpAst["keywords"].has(word)) {
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
                    
                    SemanticAnalyzer analyzer(doc, tokens);
                    analyzer.analyze(ast.get());
                    
                    auto sym = analyzer.getSymbolAt(pos);
                    if (sym) {
                        Json location;
                        location.type = JsonType::Object;
                        location["uri"] = Json(uri);
                        location["range"] = sym->definitionRange.toJson();
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
                    
                    SemanticAnalyzer analyzer(doc, tokens);
                    analyzer.analyze(ast.get());
                    
                    auto visibleSymbols = analyzer.getVisibleSymbolsAt(pos);
                    
                    std::vector<Json> items;
                    
                    // 1. 添加当前作用域可见的符号
                    for (const auto& sym : visibleSymbols) {
                        Json item;
                        item.type = JsonType::Object;
                        item["label"] = Json(sym->name);
                        
                        int kind = 1; // Text
                        if (sym->kind == SymbolKind::Function) kind = 3; // Function
                        else if (sym->kind == SymbolKind::Class) kind = 7; // Class
                        else if (sym->kind == SymbolKind::Variable) kind = 6; // Variable
                        else if (sym->kind == SymbolKind::Parameter) kind = 6; // Variable
                        else if (sym->kind == SymbolKind::Property) kind = 10; // Property
                        else if (sym->kind == SymbolKind::Namespace) kind = 9; // Module
                        
                        item["kind"] = Json(kind);
                        
                        if (!sym->typeHint.empty()) {
                            item["detail"] = Json(sym->typeHint);
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
                    jc::HelpRouter::init();
                    const Json& helpAst = jc::HelpRouter::helpAst;
                    
                    if (helpAst.isObject() && helpAst.has("keywords")) {
                        for (const auto& pair : helpAst["keywords"].objVal) {
                            Json item;
                            item.type = JsonType::Object;
                            item["label"] = Json(pair.first);
                            item["kind"] = Json(14); // Keyword
                            items.push_back(item);
                        }
                    }
                    if (helpAst.isObject() && helpAst.has("functions")) {
                        const Json& funcs = helpAst["functions"];
                        for (const auto& pair : funcs.objVal) {
                            const std::string& funcName = pair.first;
                            const Json& funcData = pair.second;
                            
                            // 忽略 dunder methods (如 __add__)，除非用户主动输入
                            if (funcName.length() > 4 && funcName.substr(0, 2) == "__" && funcName.substr(funcName.length()-2) == "__") {
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
                    
                    SemanticAnalyzer analyzer(doc, tokens);
                    analyzer.analyze(ast.get());
                    
                    auto symbols = analyzer.getDocumentSymbols();
                    std::vector<Json> symJsons;
                    for (const auto& sym : symbols) {
                        symJsons.push_back(sym.toJson());
                    }
                    res.result = Json(symJsons);
                } catch (...) {}
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
            parser.parse(); // 此时不需要保存 AST，只需让 Parser 收集 diagnostics

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
