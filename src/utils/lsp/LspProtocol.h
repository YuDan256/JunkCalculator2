#ifndef JC2_LSP_PROTOCOL_H
#define JC2_LSP_PROTOCOL_H

#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <optional>

#include "../json/Json.h"

namespace jc {
namespace lsp {

    // ========================================================================
    // 2. LSP 基础数据结构 (Base Types)
    // ========================================================================
    
    // 表示文档中的一个位置 (基于 0 的行号和字符偏移)
    struct Position {
        int line = 0;
        int character = 0;

        static Position fromJson(const Json& j);
        Json toJson() const;
    };

    // 表示文档中的一段范围
    struct Range {
        Position start;
        Position end;

        static Range fromJson(const Json& j);
        Json toJson() const;
    };

    // ========================================================================
    // 3. JSON-RPC 2.0 消息结构
    // ========================================================================
    
    struct RequestMessage {
        std::string jsonrpc = "2.0";
        Json id; // Number or String
        std::string method;
        Json params;

        static RequestMessage fromJson(const Json& j);
    };

    struct ResponseMessage {
        std::string jsonrpc = "2.0";
        Json id;
        Json result;
        Json error;

        Json toJson() const;
    };

    struct NotificationMessage {
        std::string jsonrpc = "2.0";
        std::string method;
        Json params;

        static NotificationMessage fromJson(const Json& j);
        Json toJson() const;
    };

    // ========================================================================
    // 4. LSP 初始化相关结构 (Initialize)
    // ========================================================================
    
    struct InitializeParams {
        Json processId;
        std::string rootUri;
        Json capabilities; // 客户端能力，暂存为原始 JSON 以保持轻量

        static InitializeParams fromJson(const Json& j);
    };

    struct ServerCapabilities {
        int textDocumentSync = 1; // 1 = Full Sync (全量同步)
        bool hoverProvider = true;
        bool definitionProvider = true;
        bool documentFormattingProvider = true;
        
        // 补全提供者配置
        struct CompletionOptions {
            bool resolveProvider = false;
            std::vector<std::string> triggerCharacters = { ".", ":" };
            Json toJson() const;
        } completionProvider;

        Json toJson() const;
    };

    struct InitializeResult {
        ServerCapabilities capabilities;
        
        struct ServerInfo {
            std::string name = "jc2-lsp";
            std::string version = "1.0.0";
            Json toJson() const;
        } serverInfo;

        Json toJson() const;
    };

} // namespace lsp
} // namespace jc

#endif // JC2_LSP_PROTOCOL_H
