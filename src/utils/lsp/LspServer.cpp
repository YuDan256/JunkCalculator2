#include "LspServer.h"
#include "../../frontend/Lexer.h"
#include "../../frontend/Parser.h"
#include "SemanticAnalyzer.h"
#include <iostream>
#include <string>
#include <vector>

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
                    
                    Parser parser(tokens, uri);
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
                    
                    Parser parser(tokens, uri);
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
                    
                    Parser parser(tokens, uri);
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
                    
                    // 2. 添加 JC2 语言关键字
                    const char* keywords[] = {
                        "if", "else", "while", "for", "in", "is", "as", "break", "continue", "return",
                        "class", "extends", "super", "self", "namespace", "enum", "ref", "state", "const", "local", "static", "delete",
                        "try", "catch", "throw", "import", "true", "false", "none", "macro", "syntax", "quote", "match", "case", "default", "defer"
                    };
                    for (const char* kw : keywords) {
                        Json item;
                        item.type = JsonType::Object;
                        item["label"] = Json(kw);
                        item["kind"] = Json(14); // Keyword
                        items.push_back(item);
                    }
                    
                    res.result = Json(items);
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
            for (const auto& tok : tokens) {
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
            Parser parser(tokens, uri);
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
