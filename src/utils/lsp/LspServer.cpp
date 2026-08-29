#include "LspServer.h"
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
        } catch (const std::exception& e) {
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
    }

    void LspServer::handleNotification(const NotificationMessage& notif) {
        if (notif.method == "exit") {
            isRunning = false;
        }
    }

} // namespace lsp
} // namespace jc
