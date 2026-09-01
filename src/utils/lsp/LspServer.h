#ifndef JC2_LSP_SERVER_H
#define JC2_LSP_SERVER_H

#include <string>
#include <iostream>
#include "LspProtocol.h"
#include "Workspace.h"
#include "BuiltinIndex.h"

namespace jc {
namespace lsp {

    enum class ServerState {
        Uninitialized,
        Initialized,
        ShuttingDown
    };

    class LspServer {
    public:
        LspServer();
        ~LspServer();

        // 启动 LSP 服务器的主事件循环
        void run();

    private:
        // 从标准输入读取一条完整的 JSON-RPC 消息
        bool readMessage(std::string& outContent);
        
        // 向标准输出发送一条 JSON-RPC 消息
        void sendMessage(const std::string& content);

        // 处理接收到的 JSON 消息
        void handleMessage(const std::string& content);

        // 具体的请求与通知分发
        void handleRequest(const RequestMessage& req);
        void handleNotification(const NotificationMessage& notif);

        // 文档同步处理
        void handleDidOpen(const NotificationMessage& notif);
        void handleDidChange(const NotificationMessage& notif);
        void handleDidClose(const NotificationMessage& notif);

        // 语法诊断推送
        void publishDiagnostics(const std::string& uri, Document* doc);

        // 语言特性处理
        void handleHover(const RequestMessage& req);
        void handleDefinition(const RequestMessage& req);
        void handleCompletion(const RequestMessage& req);
        void handleFormatting(const RequestMessage& req);
        void handleSignatureHelp(const RequestMessage& req);
        void handleDocumentSymbol(const RequestMessage& req);
        void handleSemanticTokens(const RequestMessage& req);

        bool isRunning = false;
        ServerState state = ServerState::Uninitialized;
        Workspace workspace;
        BuiltinIndex builtinIndex;
    };

} // namespace lsp
} // namespace jc

#endif // JC2_LSP_SERVER_H
