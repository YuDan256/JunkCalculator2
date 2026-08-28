#ifndef JC2_HIGHLIGHT_H
#define JC2_HIGHLIGHT_H

#include "Lexer.h"
#include "Token.h"
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#undef IN
#undef CONST
#undef DELETE
#undef ERROR
#undef VOID
#undef OUT
#undef NEAR
#undef FAR
#undef min
#undef max
#endif

namespace jc {

    // ═══════════════════════════════════════════
    // ANSI 颜色码
    // ═══════════════════════════════════════════
    namespace Ansi {
        inline const char* RESET = "\033[0m";
        inline const char* BOLD = "\033[1m";
        inline const char* DIM = "\033[2m";
        inline const char* RED = "\033[31m";
        inline const char* GREEN = "\033[32m";
        inline const char* YELLOW = "\033[33m";
        inline const char* BLUE = "\033[34m";
        inline const char* MAGENTA = "\033[35m";
        inline const char* CYAN = "\033[36m";
        inline const char* GRAY = "\033[90m";
        inline const char* BRIGHT_RED = "\033[91m";
        inline const char* BRIGHT_GREEN = "\033[92m";
        inline const char* BRIGHT_YELLOW = "\033[93m";
        inline const char* BRIGHT_BLUE = "\033[94m";
        inline const char* BRIGHT_MAGENTA = "\033[95m";
        inline const char* BRIGHT_CYAN = "\033[96m";
        inline const char* WHITE = "\033[97m";
    }

    // ═══════════════════════════════════════════
    // 全局开关
    // ═══════════════════════════════════════════
    inline bool colorsEnabled = true;

    inline std::string col(const char* color) {
        return colorsEnabled ? std::string(color) : "";
    }

    // ═══════════════════════════════════════════
    // Windows ANSI 启用
    // ═══════════════════════════════════════════
    inline void enableAnsiColors() {
#ifdef _WIN32
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(hOut, &mode)) {
                mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                SetConsoleMode(hOut, mode);
            }
        }
        HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
        if (hErr != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(hErr, &mode)) {
                mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                SetConsoleMode(hErr, mode);
            }
        }
        SetConsoleOutputCP(CP_UTF8);
#endif
    }

    // ═══════════════════════════════════════════
    // 代码语法高亮引擎 (单行处理核心)
    // ═══════════════════════════════════════════
    inline std::string highlightSingleLine(const std::string& code) {
        if (code.empty()) return code;

        static const std::set<std::string> keywords = {
            "if", "else", "while", "for", "in", "is", "as", "break", "continue", "return",
            "class", "extends", "super", "const", "static", "delete", "global", "ref",
            "throw", "try", "catch", "import", "switch", "case", "default", "match", "enum"
        };
        static const std::set<std::string> constants = {
            "PI", "E", "true", "false", "i", "I", "self", "none", "ANS"
        };

        // 用 Lexer 分词
        std::vector<Token> tokens;
        try {
            Lexer lexer(code);
            lexer.keepComments = true; // ★ 开启注释保留，使 REPL 支持注释高亮
            tokens = lexer.tokenize();
        }
        catch (...) {
            return code; // 解析失败则原样返回
        }

        std::string result;
        int lastPos = 0;

        for (const auto& tok : tokens) {
            if (tok.type == TokenType::END_OF_FILE) break;

            // 补上 token 之前的空白（保留原始格式）
            int tokStart = tok.position;

            // 找到 token 在原始代码中的实际结束位置
            int tokLen = static_cast<int>(tok.lexeme.size());

            // 确定原始源码中的这段 token (处理 string/fstring/rstring 有引号包围的情况)
            int srcStart = tokStart;
            int srcEnd = tokStart;

            // 根据 token 类型计算源码中的实际跨度
            if (tok.type == TokenType::STRING) {
                // "content" → 源码中比 lexeme 多 2 个引号
                srcEnd = srcStart + tokLen + 2;
            }
            else if (tok.type == TokenType::FSTRING) {
                // f"content" → 源码中比 lexeme 多 f + 2 引号 = 3
                srcEnd = srcStart + tokLen + 3;
            }
            else if (tok.type == TokenType::RSTRING) {
                // r"content" 或 r"TAG(content)TAG" → 从 srcStart 到代码中匹配的位置
                // 简化处理：直接搜索
                srcEnd = srcStart + tokLen + 3; // r + " + content + "
                // 自定义定界符时可能更长，但展示时取 lexeme 足够
            }
            else if (tok.type == TokenType::IMAGINARY) {
                srcEnd = srcStart + tokLen; // lexeme 已包含 'i' 后缀被剥除? 不，Lexer 记录的 lexeme 包含 i
                // Lexer addToken 时 lexeme = source.substr(start, current-start) 包含 i
                srcEnd = srcStart + tokLen;
            }
            else {
                srcEnd = srcStart + tokLen;
            }

            // 如果 srcStart 在代码范围内且大于 lastPos，补上中间空白
            if (srcStart > lastPos && srcStart <= static_cast<int>(code.size())) {
                result += code.substr(lastPos, srcStart - lastPos);
            }

            // 获取原始源码片段
            std::string srcText;
            if (srcEnd <= static_cast<int>(code.size())) {
                srcText = code.substr(srcStart, srcEnd - srcStart);
            }
            else {
                srcText = tok.lexeme;
            }

            // 根据 token 类型着色
            switch (tok.type) {
                // 关键字
            case TokenType::IF: case TokenType::ELSE: case TokenType::WHILE:
            case TokenType::FOR: case TokenType::IN: case TokenType::BREAK:
            case TokenType::CONTINUE: case TokenType::RETURN: case TokenType::CLASS:
            case TokenType::CONST: case TokenType::STATIC: case TokenType::DELETE: case TokenType::STATE:
            case TokenType::REF: case TokenType::THROW: case TokenType::TRY:
            case TokenType::CATCH: case TokenType::IMPORT: case TokenType::SWITCH:
            case TokenType::CASE: case TokenType::DEFAULT: case TokenType::SUPER:
            case TokenType::MATCH:
                result += col(Ansi::BRIGHT_MAGENTA) + srcText + col(Ansi::RESET);
                break;

                // 数字
            case TokenType::NUMBER:
                result += col(Ansi::BRIGHT_YELLOW) + srcText + col(Ansi::RESET);
                break;

                // 虚数
            case TokenType::IMAGINARY:
                result += col(Ansi::BRIGHT_YELLOW) + srcText + col(Ansi::RESET);
                break;

                // 字符串
            case TokenType::STRING:
            case TokenType::FSTRING:
            case TokenType::RSTRING:
                result += col(Ansi::BRIGHT_GREEN) + srcText + col(Ansi::RESET);
                break;

                // 运算符
            case TokenType::PLUS: case TokenType::MINUS: case TokenType::STAR:
            case TokenType::SLASH: case TokenType::TILDE_SLASH: case TokenType::CARET: case TokenType::PERCENT:
            case TokenType::BACKSLASH: case TokenType::PIPE:
            case TokenType::ASSIGN: case TokenType::ARROW:
            case TokenType::PLUS_ASSIGN: case TokenType::MINUS_ASSIGN:
            case TokenType::STAR_ASSIGN: case TokenType::SLASH_ASSIGN: case TokenType::TILDE_SLASH_ASSIGN:
            case TokenType::PERCENT_ASSIGN: case TokenType::CARET_ASSIGN:
            case TokenType::BACKSLASH_ASSIGN:
            case TokenType::BIT_AND_ASSIGN: case TokenType::BIT_OR_ASSIGN: // ★
            case TokenType::SHIFT_LEFT_ASSIGN: case TokenType::SHIFT_RIGHT_ASSIGN:
            case TokenType::AND_AND: case TokenType::OR_OR: case TokenType::BANG:
            case TokenType::BIT_AND: case TokenType::BIT_OR:
            case TokenType::SHIFT_LEFT: case TokenType::SHIFT_RIGHT:
                result += col(Ansi::CYAN) + srcText + col(Ansi::RESET);
                break;

                // 比较
            case TokenType::EQUAL: case TokenType::BANG_EQUAL:
            case TokenType::LESS: case TokenType::LESS_EQUAL:
            case TokenType::GREATER: case TokenType::GREATER_EQUAL:
                result += col(Ansi::CYAN) + srcText + col(Ansi::RESET);
                break;

                // 标识符
            case TokenType::IDENTIFIER:
                if (constants.count(tok.lexeme)) {
                    result += col(Ansi::BRIGHT_CYAN) + srcText + col(Ansi::RESET);
                }
                else {
                    result += srcText; // 普通标识符不着色
                }
                break;

                // 括号和标点
            case TokenType::LPAREN: case TokenType::RPAREN:
            case TokenType::LBRACKET: case TokenType::RBRACKET:
            case TokenType::LBRACE: case TokenType::RBRACE:
                result += col(Ansi::CYAN) + srcText + col(Ansi::RESET);
                break;

                // 注释
            case TokenType::COMMENT:
                result += col(Ansi::GRAY) + srcText + col(Ansi::RESET);
                break;

            default:
                result += srcText;
                break;
            }

            if (srcEnd > lastPos) lastPos = srcEnd;
        }

        // 补上尾部
        if (lastPos < static_cast<int>(code.size())) {
            result += code.substr(lastPos);
        }

        return result;
    }

    // ═══════════════════════════════════════════
    // 代码语法高亮引擎 (多行安全入口)
    // ═══════════════════════════════════════════
    inline std::string highlightCode(const std::string& code) {
        if (!colorsEnabled) return code;
        if (code.empty()) return code;

        std::string result;
        size_t start = 0;
        size_t end = code.find('\n');
        
        while (end != std::string::npos) {
            std::string line = code.substr(start, end - start);
            result += highlightSingleLine(line) + "\n";
            start = end + 1;
            end = code.find('\n', start);
        }
        
        if (start < code.size()) {
            result += highlightSingleLine(code.substr(start));
        }
        
        return result;
    }

    // ═══════════════════════════════════════════
    // 值类型着色（用于 REPL 输出）
    // ═══════════════════════════════════════════
    inline std::string colorizeType(const std::string& typeName) {
        if (!colorsEnabled) return "";
        if (typeName == "double" || typeName == "BigInt" || typeName == "Fraction")
            return col(Ansi::BRIGHT_YELLOW);
        if (typeName == "Complex")
            return col(Ansi::BRIGHT_MAGENTA);
        if (typeName == "String")
            return col(Ansi::BRIGHT_GREEN);
        if (typeName == "RealMatrix")
            return col(Ansi::BRIGHT_YELLOW);
        if (typeName == "ComplexMatrix")
            return col(Ansi::BRIGHT_MAGENTA);
        if (typeName == "Function")
            return col(Ansi::BRIGHT_BLUE);
        if (typeName == "Dict" || typeName == "List"|| typeName == "Set")
            return col(Ansi::CYAN);
        if (typeName == "SymExpr")
            return col(Ansi::WHITE);
        return "";
    }

} // namespace jc

#endif // JC2_HIGHLIGHT_H
