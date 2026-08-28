#include "Formatter.h"
#include "../frontend/Lexer.h"
#include <algorithm>

namespace jc {

    std::string Formatter::format(const std::string& source) {
        Lexer lexer(source);
        lexer.keepComments = true; // ★ 开启注释保留
        auto tokens = lexer.tokenize();

        std::string out;
        int indent = 0;
        bool isNewLine = true;
        int emptyLines = 0;

        auto getIndent = [&]() { return std::string(indent * 4, ' '); };

        auto isOp = [](TokenType type) {
            switch (type) {
            case TokenType::PLUS: case TokenType::MINUS: case TokenType::STAR: case TokenType::SLASH:
            case TokenType::CARET: case TokenType::BACKSLASH: case TokenType::PERCENT:
            case TokenType::ASSIGN: case TokenType::EQUAL: case TokenType::BANG_EQUAL:
            case TokenType::GREATER: case TokenType::GREATER_EQUAL: case TokenType::LESS: case TokenType::LESS_EQUAL: case TokenType::SUBSET:
            case TokenType::ARROW: case TokenType::PIPE: case TokenType::RIGHT_ARROW:
            case TokenType::SHIFT_LEFT: case TokenType::SHIFT_RIGHT:
            case TokenType::PLUS_ASSIGN: case TokenType::MINUS_ASSIGN: case TokenType::STAR_ASSIGN:
            case TokenType::SLASH_ASSIGN: case TokenType::TILDE_SLASH_ASSIGN: case TokenType::PERCENT_ASSIGN:
            case TokenType::CARET_ASSIGN: case TokenType::BACKSLASH_ASSIGN: case TokenType::BIT_AND_ASSIGN:
            case TokenType::BIT_OR_ASSIGN: case TokenType::BIT_XOR_ASSIGN: case TokenType::SHIFT_LEFT_ASSIGN:
            case TokenType::SHIFT_RIGHT_ASSIGN:
            case TokenType::AND_AND: case TokenType::OR_OR: case TokenType::BIT_AND: case TokenType::BIT_OR: case TokenType::BIT_XOR:
            case TokenType::TILDE_SLASH:
                return true;
            default:
                return false;
            }
        };

        for (size_t i = 0; i < tokens.size(); ++i) {
            const auto& t = tokens[i];
            if (t.type == TokenType::END_OF_FILE) break;

            if (t.type == TokenType::NEWLINE) {
                if (isNewLine) {
                    emptyLines++;
                    // 最多保留 1 个空行
                    if (emptyLines <= 1 && !out.empty()) {
                        out += "\n";
                    }
                }
                else {
                    out += "\n";
                    isNewLine = true;
                    emptyLines = 0;
                }
                continue;
            }

            if (t.type == TokenType::RBRACE || t.type == TokenType::RBRACKET) {
                indent = std::max(0, indent - 1);
            }

            if (isNewLine) {
                if (t.type != TokenType::NEWLINE) {
                    out += getIndent();
                    isNewLine = false;
                }
            }
            else {
                bool spaceBefore = false;
                TokenType prev = i > 0 ? tokens[i - 1].type : TokenType::ERROR;

                // 1. 关键字后加空格
                if (prev >= TokenType::IF && prev <= TokenType::EXTENDS) spaceBefore = true;

                // 2. 二元运算符和赋值号两端加空格
                if (isOp(t.type) || isOp(prev)) spaceBefore = true;

                // 3. 逗号、分号、冒号后加空格
                if (prev == TokenType::COMMA || prev == TokenType::SEMICOLON || prev == TokenType::COLON) spaceBefore = true;

                // 4. 左大括号前加空格
                if (t.type == TokenType::LBRACE) spaceBefore = true;

                // 5. 注释前加空格
                if (t.type == TokenType::COMMENT) spaceBefore = true;

                // --- 例外情况 (取消空格) ---

                // 逗号、分号、冒号前不加空格
                if (t.type == TokenType::COMMA || t.type == TokenType::SEMICOLON || t.type == TokenType::COLON) spaceBefore = false;

                // 右括号前不加空格
                if (t.type == TokenType::RPAREN || t.type == TokenType::RBRACKET) spaceBefore = false;

                // 左括号后不加空格
                if (prev == TokenType::LPAREN || prev == TokenType::LBRACKET) spaceBefore = false;

                // 函数调用时，左括号前不加空格
                if (t.type == TokenType::LPAREN && (prev == TokenType::IDENTIFIER || prev == TokenType::RPAREN || prev == TokenType::RBRACKET || prev == TokenType::SUPER || prev == TokenType::SELF)) spaceBefore = false;

                // 点号两端不加空格
                if (t.type == TokenType::DOT || prev == TokenType::DOT) spaceBefore = false;

                // 智能识别一元运算符 (+, -, !, ~, $, @, ...)
                bool isUnary = false;
                if (t.type == TokenType::PLUS || t.type == TokenType::MINUS || t.type == TokenType::BANG || t.type == TokenType::TILDE || t.type == TokenType::DOLLAR || t.type == TokenType::AT || t.type == TokenType::ELLIPSIS) {
                    if (prev == TokenType::ASSIGN || prev == TokenType::LPAREN || prev == TokenType::LBRACKET || prev == TokenType::LBRACE || prev == TokenType::COMMA || prev == TokenType::COLON || prev == TokenType::SEMICOLON || isOp(prev) || prev == TokenType::RETURN || prev == TokenType::THROW || prev == TokenType::IF || prev == TokenType::WHILE || prev == TokenType::FOR || prev == TokenType::ERROR) {
                        isUnary = true;
                    }
                }
                if (isUnary) spaceBefore = true; // 一元运算符前需要与前面的 token 隔开
                
                // 一元运算符后不加空格
                bool prevIsUnary = false;
                if (prev == TokenType::PLUS || prev == TokenType::MINUS || prev == TokenType::BANG || prev == TokenType::TILDE || prev == TokenType::DOLLAR || prev == TokenType::AT || prev == TokenType::ELLIPSIS) {
                    TokenType prevPrev = i > 1 ? tokens[i - 2].type : TokenType::ERROR;
                    if (prevPrev == TokenType::ASSIGN || prevPrev == TokenType::LPAREN || prevPrev == TokenType::LBRACKET || prevPrev == TokenType::LBRACE || prevPrev == TokenType::COMMA || prevPrev == TokenType::COLON || prevPrev == TokenType::SEMICOLON || isOp(prevPrev) || prevPrev == TokenType::RETURN || prevPrev == TokenType::THROW || prevPrev == TokenType::IF || prevPrev == TokenType::WHILE || prevPrev == TokenType::FOR || prevPrev == TokenType::ERROR) {
                        prevIsUnary = true;
                    }
                }
                if (prevIsUnary) spaceBefore = false;

                if (spaceBefore && !out.empty() && out.back() != ' ') out += " ";
            }

            out += t.lexeme;

            if (t.type == TokenType::LBRACE || t.type == TokenType::LBRACKET) {
                indent++;
            }
        }

        // 确保文件以单个换行符结尾
        if (!out.empty() && out.back() != '\n') {
            out += "\n";
        }

        return out;
    }

} // namespace jc
