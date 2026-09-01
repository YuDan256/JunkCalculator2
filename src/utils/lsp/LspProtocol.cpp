#include "LspProtocol.h"
#include <cctype>
#include <sstream>
#include <iomanip>

namespace jc {
namespace lsp {

    // ========================================================================
    // LSP 结构体转换实现
    // ========================================================================
    Position Position::fromJson(const Json& j) {
        Position p;
        if (j.has("line") && j["line"].isNumber()) p.line = static_cast<int>(j["line"].numVal);
        if (j.has("character") && j["character"].isNumber()) p.character = static_cast<int>(j["character"].numVal);
        return p;
    }

    Json Position::toJson() const {
        Json j;
        j.type = JsonType::Object;
        j["line"] = Json(line);
        j["character"] = Json(character);
        return j;
    }

    Range Range::fromJson(const Json& j) {
        Range r;
        if (j.has("start")) r.start = Position::fromJson(j["start"]);
        if (j.has("end")) r.end = Position::fromJson(j["end"]);
        return r;
    }

    Json Range::toJson() const {
        Json j;
        j.type = JsonType::Object;
        j["start"] = start.toJson();
        j["end"] = end.toJson();
        return j;
    }

    Json TextEdit::toJson() const {
        Json j;
        j.type = JsonType::Object;
        j["range"] = range.toJson();
        j["newText"] = Json(newText);
        return j;
    }

    Json DocumentSymbol::toJson() const {
        Json j;
        j.type = JsonType::Object;
        j["name"] = Json(name);
        if (!detail.empty()) j["detail"] = Json(detail);
        j["kind"] = Json(kind);
        j["range"] = range.toJson();
        j["selectionRange"] = selectionRange.toJson();
        if (!children.empty()) {
            std::vector<Json> arr;
            for (const auto& c : children) arr.push_back(c.toJson());
            j["children"] = Json(arr);
        }
        return j;
    }

    // ========================================================================
    // JSON-RPC 2.0 消息结构实现
    // ========================================================================
    RequestMessage RequestMessage::fromJson(const Json& j) {
        RequestMessage req;
        if (j.has("id")) req.id = j["id"];
        if (j.has("method") && j["method"].isString()) req.method = j["method"].strVal;
        if (j.has("params")) req.params = j["params"];
        return req;
    }

    Json ResponseMessage::toJson() const {
        Json j;
        j.type = JsonType::Object;
        j["jsonrpc"] = Json("2.0");
        j["id"] = id;
        if (!error.isNull()) {
            j["error"] = error;
        } else {
            j["result"] = result;
        }
        return j;
    }

    NotificationMessage NotificationMessage::fromJson(const Json& j) {
        NotificationMessage notif;
        if (j.has("method") && j["method"].isString()) notif.method = j["method"].strVal;
        if (j.has("params")) notif.params = j["params"];
        return notif;
    }

    Json NotificationMessage::toJson() const {
        Json j;
        j.type = JsonType::Object;
        j["jsonrpc"] = Json("2.0");
        j["method"] = Json(method);
        if (!params.isNull()) j["params"] = params;
        return j;
    }

    // ========================================================================
    // LSP 初始化相关结构实现
    // ========================================================================
    InitializeParams InitializeParams::fromJson(const Json& j) {
        InitializeParams params;
        if (j.has("processId")) params.processId = j["processId"];
        if (j.has("rootUri") && j["rootUri"].isString()) params.rootUri = j["rootUri"].strVal;
        if (j.has("capabilities")) params.capabilities = j["capabilities"];
        return params;
    }

    Json ServerCapabilities::SignatureHelpOptions::toJson() const {
        Json j;
        j.type = JsonType::Object;
        std::vector<Json> triggers;
        for (const auto& c : triggerCharacters) triggers.push_back(Json(c));
        j["triggerCharacters"] = Json(triggers);
        return j;
    }

    Json ServerCapabilities::SemanticTokensOptions::Legend::toJson() const {
        Json j;
        j.type = JsonType::Object;
        std::vector<Json> types;
        for (const auto& t : tokenTypes) types.push_back(Json(t));
        j["tokenTypes"] = Json(types);
        std::vector<Json> modifiers;
        for (const auto& m : tokenModifiers) modifiers.push_back(Json(m));
        j["tokenModifiers"] = Json(modifiers);
        return j;
    }

    Json ServerCapabilities::SemanticTokensOptions::Full::toJson() const {
        Json j;
        j.type = JsonType::Object;
        j["delta"] = Json(delta);
        return j;
    }

    Json ServerCapabilities::SemanticTokensOptions::toJson() const {
        Json j;
        j.type = JsonType::Object;
        j["legend"] = legend.toJson();
        j["range"] = Json(range);
        j["full"] = full.toJson();
        return j;
    }

    Json ServerCapabilities::CompletionOptions::toJson() const {
        Json j;
        j.type = JsonType::Object;
        j["resolveProvider"] = Json(resolveProvider);
        std::vector<Json> triggers;
        for (const auto& c : triggerCharacters) triggers.push_back(Json(c));
        j["triggerCharacters"] = Json(triggers);
        return j;
    }

    Json ServerCapabilities::toJson() const {
        Json j;
        j.type = JsonType::Object;
        j["textDocumentSync"] = Json(textDocumentSync);
        j["hoverProvider"] = Json(hoverProvider);
        j["definitionProvider"] = Json(definitionProvider);
        j["documentFormattingProvider"] = Json(documentFormattingProvider);
        j["documentSymbolProvider"] = Json(documentSymbolProvider);
        j["signatureHelpProvider"] = signatureHelpProvider.toJson();
        j["completionProvider"] = completionProvider.toJson();
        j["semanticTokensProvider"] = semanticTokensProvider.toJson();
        return j;
    }

    Json InitializeResult::ServerInfo::toJson() const {
        Json j;
        j.type = JsonType::Object;
        j["name"] = Json(name);
        j["version"] = Json(version);
        return j;
    }

    Json InitializeResult::toJson() const {
        Json j;
        j.type = JsonType::Object;
        j["capabilities"] = capabilities.toJson();
        j["serverInfo"] = serverInfo.toJson();
        return j;
    }

} // namespace lsp
} // namespace jc
