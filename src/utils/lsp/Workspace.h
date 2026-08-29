#ifndef JC2_LSP_WORKSPACE_H
#define JC2_LSP_WORKSPACE_H

#include <string>
#include <unordered_map>
#include <vector>
#include "LspProtocol.h"

namespace jc {
namespace lsp {

    class Document {
    public:
        std::string uri;
        std::string content;
        int version = 0;

        Document() = default;
        Document(std::string uri, std::string content, int version);

        // 全量更新文档内容
        void updateContent(std::string newContent, int newVersion);

        // 坐标转换引擎
        // 将 LSP 的 (line, character) 转换为 JC2 AST 的绝对字节偏移量
        int positionToOffset(const Position& pos) const;
        
        // 将 JC2 AST 的绝对字节偏移量转换为 LSP 的 (line, character)
        Position offsetToPosition(int offset) const;

    private:
        std::vector<int> lineOffsets; // 记录每一行开头的绝对字节偏移量
        void computeLineOffsets();
    };

    class Workspace {
    public:
        void openDocument(const std::string& uri, const std::string& text, int version);
        void updateDocument(const std::string& uri, const std::string& text, int version);
        void closeDocument(const std::string& uri);

        Document* getDocument(const std::string& uri);

    private:
        std::unordered_map<std::string, Document> documents;
    };

} // namespace lsp
} // namespace jc

#endif // JC2_LSP_WORKSPACE_H
