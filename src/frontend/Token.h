#ifndef JC2_TOKEN_H
#define JC2_TOKEN_H

#include <string>
#include <utility>

namespace jc {

    enum class TokenType {
        // --- 字面量与标识符 ---
        NUMBER, STRING, IDENTIFIER, FSTRING, RSTRING, IMAGINARY,           // ★ 3i, 2.5i, 1e3i

        // --- 基础数学运算符 ---
        PLUS, MINUS, STAR, SLASH, CARET, BACKSLASH, PERCENT,

        // --- 比较与赋值运算符 ---
        ASSIGN, EQUAL, BANG_EQUAL, GREATER, GREATER_EQUAL, LESS, LESS_EQUAL, 
        
        ARROW, PIPE, RIGHT_ARROW,
        SHIFT_LEFT, SHIFT_RIGHT,

        // --- 复合赋值运算符 ---     // ★ 新增
        PLUS_ASSIGN,         // ★ +=
        MINUS_ASSIGN,        // ★ -=
        STAR_ASSIGN,         // ★ *=
        SLASH_ASSIGN,        // ★ /=
        PERCENT_ASSIGN,      // ★ %=
        CARET_ASSIGN,        // ★ ^=
        BACKSLASH_ASSIGN,    // ★ \=
        BIT_AND_ASSIGN,      // ★ &=
        BIT_OR_ASSIGN,       // ★ |=
        SHIFT_LEFT_ASSIGN,   // ★ <<=
        SHIFT_RIGHT_ASSIGN,  // ★ >>=

        // --- 括号与标点符号 ---
        LPAREN, RPAREN, LBRACKET, RBRACKET,
        LBRACE, RBRACE,           // ★ 新增：{ }
        COMMA, SEMICOLON,
        QUESTION,            // ★ ?
        COLON,               // ★ :
        DOT, ELLIPSIS,
        AT,                  // ★ 新增：@

        // --- 控制流关键字 ---     // ★ 新增
        IF, ELSE, WHILE, FOR, IN,
        BREAK, CONTINUE,
        RETURN,
        LOCAL, REF, STATE,
        CONST,
        DELETE,
        THROW,               // ★ 新增
        TRY,                 // ★ 新增
        CATCH,               // ★ 新增
        IMPORT,
        SWITCH,              // ★
        CASE,                // ★
        DEFAULT,             // ★
        MATCH,               // ★ 新增
        CLASS,
        NAMESPACE,           // ★ 新增
        SUPER,
        SELF,                // ★ 新增
        TRUE_KW,             // ★ 新增
        FALSE_KW,            // ★ 新增
        NONE_KW,             // ★ 新增

        // --- 逻辑运算符 ---
        AND_AND,        // &&
        OR_OR,          // ||
        BANG,           // !  (单独的，不是 !=)
        TILDE,          // ~  (★ 新增)
        BIT_AND,        // &  (★ 新增)
        BIT_OR,         // |  (★ 新增)


        // --- 特殊标记 ---
        NEWLINE, END_OF_FILE, ERROR
    };

    struct Token {
        TokenType type;
        std::string lexeme;
        int position;
        int line;
        Token(TokenType type, std::string lexeme, int position = 0, int line = 0)
            : type(type), lexeme(std::move(lexeme)), position(position), line(line) {
        }
    };

    inline std::string tokenTypeToString(TokenType type) {
        switch (type) {
        case TokenType::NUMBER:        return "NUMBER";
        case TokenType::IMAGINARY:     return "IMAGINARY";
        case TokenType::IDENTIFIER:    return "IDENTIFIER";
        case TokenType::STRING:        return "STRING";
        case TokenType::FSTRING:       return "FSTRING";
        case TokenType::RSTRING:       return "RSTRING";
        case TokenType::PLUS:          return "PLUS(+)";
        case TokenType::MINUS:         return "MINUS(-)";
        case TokenType::STAR:          return "STAR(*)";
        case TokenType::SLASH:         return "SLASH(/)";
        case TokenType::CARET:         return "CARET(^)";
        case TokenType::BACKSLASH:     return "BACKSLASH(\\)";
        case TokenType::PERCENT:       return "PERCENT(%)";
        case TokenType::ASSIGN:        return "ASSIGN(=)";
        case TokenType::EQUAL:         return "EQUAL(==)";
        case TokenType::ARROW:         return "ARROW(=>)";
        case TokenType::RIGHT_ARROW:   return "RIGHT_ARROW(->)";
        case TokenType::PIPE:          return "PIPE(|>)";
        case TokenType::BANG_EQUAL:    return "BANG_EQUAL(!=)";
        case TokenType::PLUS_ASSIGN:    return "PLUS_ASSIGN(+=)";
        case TokenType::MINUS_ASSIGN:   return "MINUS_ASSIGN(-=)";
        case TokenType::STAR_ASSIGN:    return "STAR_ASSIGN(*=)";
        case TokenType::SLASH_ASSIGN:   return "SLASH_ASSIGN(/=)";
        case TokenType::PERCENT_ASSIGN: return "PERCENT_ASSIGN(%=)";
        case TokenType::CARET_ASSIGN:   return "CARET_ASSIGN(^=)";
        case TokenType::BACKSLASH_ASSIGN: return "BACKSLASH_ASSIGN(\\=)";
        case TokenType::BIT_AND_ASSIGN: return "BIT_AND_ASSIGN(&=)";   // ★
        case TokenType::BIT_OR_ASSIGN:  return "BIT_OR_ASSIGN(|=)";    // ★
        case TokenType::SHIFT_LEFT_ASSIGN: return "SHIFT_LEFT_ASSIGN(<<=)";
        case TokenType::SHIFT_RIGHT_ASSIGN: return "SHIFT_RIGHT_ASSIGN(>>=)";
        case TokenType::AND_AND:       return "AND_AND(&&)";    // ★
        case TokenType::OR_OR:         return "OR_OR(||)";      // ★
        case TokenType::BANG:          return "BANG(!)";         // ★
        case TokenType::TILDE:         return "TILDE(~)";        // ★
        case TokenType::BIT_AND:       return "BIT_AND(&)";
        case TokenType::BIT_OR:        return "BIT_OR(|)";
        case TokenType::SHIFT_LEFT:    return "SHIFT_LEFT(<<)";
        case TokenType::SHIFT_RIGHT:   return "SHIFT_RIGHT(>>)";
        case TokenType::GREATER:       return "GREATER(>)";
        case TokenType::GREATER_EQUAL: return "GREATER_EQUAL(>=)";
        case TokenType::LESS:          return "LESS(<)";
        case TokenType::LESS_EQUAL:    return "LESS_EQUAL(<=)";
        case TokenType::LPAREN:        return "LPAREN( ( )";
        case TokenType::RPAREN:        return "RPAREN( ) )";
        case TokenType::LBRACKET:      return "LBRACKET( [ )";
        case TokenType::RBRACKET:      return "RBRACKET( ] )";
        case TokenType::LBRACE:        return "LBRACE( { )";     // ★
        case TokenType::RBRACE:        return "RBRACE( } )";     // ★
        case TokenType::COMMA:         return "COMMA(,)";
        case TokenType::SEMICOLON:     return "SEMICOLON(;)";
        case TokenType::QUESTION:      return "QUESTION(?)";
        case TokenType::COLON:         return "COLON(:)";
        case TokenType::DOT:           return "DOT(.)";
        case TokenType::ELLIPSIS:      return "ELLIPSIS(...)";
        case TokenType::AT:            return "AT(@)";
        case TokenType::CLASS:         return "CLASS";
        case TokenType::NAMESPACE:     return "NAMESPACE";
        case TokenType::SUPER:         return "SUPER";
        case TokenType::SELF:          return "SELF";
        case TokenType::TRUE_KW:       return "TRUE";
        case TokenType::FALSE_KW:      return "FALSE";
        case TokenType::NONE_KW:       return "NONE";
        case TokenType::IF:            return "IF";               // ★
        case TokenType::ELSE:          return "ELSE";             // ★
        case TokenType::WHILE:         return "WHILE";            // ★
        case TokenType::FOR:           return "FOR";              // ★
        case TokenType::IN:            return "IN";
        case TokenType::BREAK:         return "BREAK";            // ★
        case TokenType::CONTINUE:      return "CONTINUE";         // ★
        case TokenType::RETURN:        return "RETURN";           // ★
        case TokenType::SWITCH:        return "SWITCH";
        case TokenType::CASE:          return "CASE";
        case TokenType::DEFAULT:       return "DEFAULT";
        case TokenType::MATCH:         return "MATCH";
        case TokenType::LOCAL:         return "LOCAL";
        case TokenType::IMPORT:        return "IMPORT";
        case TokenType::REF:           return "REF";
        case TokenType::STATE:         return "STATE";
        case TokenType::CONST:         return "CONST";
        case TokenType::DELETE:        return "DELETE";
        case TokenType::NEWLINE:       return "NEWLINE";
        case TokenType::END_OF_FILE:   return "EOF";
        case TokenType::ERROR:         return "ERROR";
        case TokenType::THROW:         return "THROW";
        case TokenType::TRY:           return "TRY";
        case TokenType::CATCH:         return "CATCH";
        default:                       return "UNKNOWN";
        }
    }

} // namespace jc
#endif // JC2_TOKEN_H
