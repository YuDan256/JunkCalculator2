#include "Workspace.h"
#include "../../frontend/Utf8.h"
#include <algorithm>

namespace jc {
namespace lsp {

    Document::Document(std::string uri, std::string content, int version)
        : uri(std::move(uri)), content(std::move(content)), version(version) {
        computeLineOffsets();
    }

    void Document::updateContent(std::string newContent, int newVersion) {
        content = std::move(newContent);
        version = newVersion;
        computeLineOffsets();
    }

    void Document::computeLineOffsets() {
        lineOffsets.clear();
        lineOffsets.push_back(0); // 第 0 行的起始偏移量始终为 0
        for (size_t i = 0; i < content.length(); ++i) {
            if (content[i] == '\n') {
                lineOffsets.push_back(static_cast<int>(i + 1));
            }
        }
    }

    int Document::positionToOffset(const Position& pos) const {
        if (pos.line < 0 || pos.line >= static_cast<int>(lineOffsets.size())) {
            return -1; // 行号越界
        }
        int lineStartOffset = lineOffsets[pos.line];
        int lineEndOffset = (pos.line + 1 < static_cast<int>(lineOffsets.size())) 
                            ? lineOffsets[pos.line + 1] 
                            : static_cast<int>(content.length());

        std::string lineText = content.substr(lineStartOffset, lineEndOffset - lineStartOffset);
        
        // LSP 的 character 是基于 UTF-16 编码单元的，但为了与 JC2 内部统一，
        // 我们这里将其视为 UTF-8 字符索引（Codepoint）。
        // 使用 jc::utf8::byteOffset 进行安全的字符到字节偏移转换。
        size_t byteOff = jc::utf8::byteOffset(lineText, pos.character);
        if (byteOff == std::string::npos) {
            // 如果 character 超出了该行的长度，则返回该行的末尾（不含换行符）
            return lineEndOffset > lineStartOffset && content[lineEndOffset - 1] == '\n' 
                   ? lineEndOffset - 1 : lineEndOffset;
        }
        return lineStartOffset + static_cast<int>(byteOff);
    }

    Position Document::offsetToPosition(int offset) const {
        Position pos;
        if (offset < 0) return pos;
        if (offset > static_cast<int>(content.length())) {
            offset = static_cast<int>(content.length());
        }

        // 工业级实现：使用二分查找 (O(log N)) 快速定位 offset 所在的行
        auto it = std::upper_bound(lineOffsets.begin(), lineOffsets.end(), offset);
        pos.line = static_cast<int>(std::distance(lineOffsets.begin(), it)) - 1;

        int lineStartOffset = lineOffsets[pos.line];
        std::string lineText = content.substr(lineStartOffset, offset - lineStartOffset);
        
        // 计算从行首到 offset 的 UTF-8 字符数
        pos.character = static_cast<int>(jc::utf8::charIndex(lineText, lineText.length()));
        return pos;
    }

    void Workspace::openDocument(const std::string& uri, const std::string& text, int version) {
        documents[uri] = Document(uri, text, version);
    }

    void Workspace::updateDocument(const std::string& uri, const std::string& text, int version) {
        auto it = documents.find(uri);
        if (it != documents.end()) {
            it->second.updateContent(text, version);
        } else {
            // 容错：如果未打开却收到了更新，直接打开
            openDocument(uri, text, version);
        }
    }

    void Workspace::closeDocument(const std::string& uri) {
        documents.erase(uri);
    }

    Document* Workspace::getDocument(const std::string& uri) {
        auto it = documents.find(uri);
        if (it != documents.end()) {
            return &(it->second);
        }
        return nullptr;
    }

} // namespace lsp
} // namespace jc
