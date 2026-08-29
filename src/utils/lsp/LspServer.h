#ifndef JC2_LSP_SERVER_H
#define JC2_LSP_SERVER_H

#include <string>
#include <iostream>
#include "LspProtocol.h"

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

        bool isRunning = false;
        ServerState state = ServerState::Uninitialized;
    };

} // namespace lsp
} // namespace jc

#endif // JC2_LSP_SERVER_H
