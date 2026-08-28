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
        ASSIGN, EQUAL, BANG_EQUAL, GREATER, GREATER_EQUAL, LESS, LESS_EQUAL, SUBSET, 
        
        ARROW, PIPE, RIGHT_ARROW,
        SHIFT_LEFT, SHIFT_RIGHT,

        // --- 复合赋值运算符 ---     // ★ 新增
        PLUS_ASSIGN,         // ★ +=
        MINUS_ASSIGN,        // ★ -=
        STAR_ASSIGN,         // ★ *=
        SLASH_ASSIGN,        // ★ /=
        TILDE_SLASH_ASSIGN,  // ★ ~/=
        PERCENT_ASSIGN,      // ★ %=
        CARET_ASSIGN,        // ★ ^=
        BACKSLASH_ASSIGN,    // ★ \=
        BIT_AND_ASSIGN,      // ★ &=
        BIT_OR_ASSIGN,       // ★ |=
        BIT_XOR_ASSIGN,      // ★ ^^=
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
        DOLLAR,              // ★ 新增：$

        // --- 控制流关键字 ---     // ★ 新增
        IF, ELSE, WHILE, FOR, IN, IS, AS,
        BREAK, CONTINUE,
        RETURN,
        LOCAL, REF, STATE,
        CONST,
        STATIC,              // ★ 新增
        DELETE,
        THROW,               // ★ 新增
        TRY,                 // ★ 新增
        CATCH,               // ★ 新增
        IMPORT,
        SWITCH,              // ★
        CASE,                // ★
        DEFAULT,             // ★
        MATCH,               // ★ 新增
        MACRO,               // ★ 新增
        SYNTAX,              // ★ 新增
        QUOTE,               // ★ 新增
        DEFER,               // ★ 新增
        CLASS,
        EXTENDS, 
        NAMESPACE,           // ★ 新增
        ENUM,                // ★ 新增
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
        TILDE_SLASH,    // ~/ (★ 新增)
        BIT_AND,        // &  (★ 新增)
        BIT_OR,         // |  (★ 新增)
        BIT_XOR,        // ^^ (★ 新增)


        // --- 特殊标记 ---
        NEWLINE, END_OF_FILE, ERROR, COMMENT
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
        case TokenType::TILDE_SLASH_ASSIGN: return "TILDE_SLASH_ASSIGN(~/=)";
        case TokenType::PERCENT_ASSIGN: return "PERCENT_ASSIGN(%=)";
        case TokenType::CARET_ASSIGN:   return "CARET_ASSIGN(^=)";
        case TokenType::BACKSLASH_ASSIGN: return "BACKSLASH_ASSIGN(\\=)";
        case TokenType::BIT_AND_ASSIGN: return "BIT_AND_ASSIGN(&=)";   // ★
        case TokenType::BIT_OR_ASSIGN:  return "BIT_OR_ASSIGN(|=)";    // ★
        case TokenType::BIT_XOR_ASSIGN: return "BIT_XOR_ASSIGN(^^=)";  // ★
        case TokenType::SHIFT_LEFT_ASSIGN: return "SHIFT_LEFT_ASSIGN(<<=)";
        case TokenType::SHIFT_RIGHT_ASSIGN: return "SHIFT_RIGHT_ASSIGN(>>=)";
        case TokenType::AND_AND:       return "AND_AND(&&)";    // ★
        case TokenType::OR_OR:         return "OR_OR(||)";      // ★
        case TokenType::BANG:          return "BANG(!)";         // ★
        case TokenType::TILDE:         return "TILDE(~)";        // ★
        case TokenType::TILDE_SLASH:   return "TILDE_SLASH(~/)"; // ★
        case TokenType::BIT_AND:       return "BIT_AND(&)";
        case TokenType::BIT_OR:        return "BIT_OR(|)";
        case TokenType::BIT_XOR:       return "BIT_XOR(^^)";
        case TokenType::SHIFT_LEFT:    return "SHIFT_LEFT(<<)";
        case TokenType::SHIFT_RIGHT:   return "SHIFT_RIGHT(>>)";
        case TokenType::GREATER:       return "GREATER(>)";
        case TokenType::GREATER_EQUAL: return "GREATER_EQUAL(>=)";
        case TokenType::LESS:          return "LESS(<)";
        case TokenType::LESS_EQUAL:    return "LESS_EQUAL(<=)";
        case TokenType::SUBSET:         return "SUBSET(<:)";
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
        case TokenType::DOLLAR:        return "DOLLAR($)";
        case TokenType::CLASS:         return "CLASS";
        case TokenType::NAMESPACE:     return "NAMESPACE";
        case TokenType::ENUM:          return "ENUM";
        case TokenType::SUPER:         return "SUPER";
        case TokenType::SELF:          return "SELF";
        case TokenType::TRUE_KW:       return "TRUE";
        case TokenType::FALSE_KW:      return "FALSE";
        case TokenType::NONE_KW:       return "NONE";
        case TokenType::EXTENDS:        return "EXTENDS";
        case TokenType::IF:            return "IF";               // ★
        case TokenType::ELSE:          return "ELSE";             // ★
        case TokenType::WHILE:         return "WHILE";            // ★
        case TokenType::FOR:           return "FOR";              // ★
        case TokenType::IN:            return "IN";
        case TokenType::IS:            return "IS";
        case TokenType::AS:            return "AS";
        case TokenType::BREAK:         return "BREAK";            // ★
        case TokenType::CONTINUE:      return "CONTINUE";         // ★
        case TokenType::RETURN:        return "RETURN";           // ★
        case TokenType::SWITCH:        return "SWITCH";
        case TokenType::CASE:          return "CASE";
        case TokenType::DEFAULT:       return "DEFAULT";
        case TokenType::MATCH:         return "MATCH";
        case TokenType::MACRO:         return "MACRO";
        case TokenType::SYNTAX:        return "SYNTAX";
        case TokenType::QUOTE:         return "QUOTE";
        case TokenType::DEFER:         return "DEFER";
        case TokenType::LOCAL:         return "LOCAL";
        case TokenType::IMPORT:        return "IMPORT";
        case TokenType::REF:           return "REF";
        case TokenType::STATE:         return "STATE";
        case TokenType::CONST:         return "CONST";
        case TokenType::STATIC:        return "STATIC";
        case TokenType::DELETE:        return "DELETE";
        case TokenType::NEWLINE:       return "NEWLINE";
        case TokenType::END_OF_FILE:   return "EOF";
        case TokenType::ERROR:         return "ERROR";
        case TokenType::COMMENT:       return "COMMENT";
        case TokenType::THROW:         return "THROW";
        case TokenType::TRY:           return "TRY";
        case TokenType::CATCH:         return "CATCH";
        default:                       return "UNKNOWN";
        }
    }

    inline TokenType stringToTokenType(const std::string& s) {
        if (s == "NUMBER") return TokenType::NUMBER;
        if (s == "IMAGINARY") return TokenType::IMAGINARY;
        if (s == "IDENTIFIER") return TokenType::IDENTIFIER;
        if (s == "STRING") return TokenType::STRING;
        if (s == "FSTRING") return TokenType::FSTRING;
        if (s == "RSTRING") return TokenType::RSTRING;
        if (s == "PLUS") return TokenType::PLUS;
        if (s == "MINUS") return TokenType::MINUS;
        if (s == "STAR") return TokenType::STAR;
        if (s == "SLASH") return TokenType::SLASH;
        if (s == "CARET") return TokenType::CARET;
        if (s == "BACKSLASH") return TokenType::BACKSLASH;
        if (s == "PERCENT") return TokenType::PERCENT;
        if (s == "ASSIGN") return TokenType::ASSIGN;
        if (s == "EQUAL") return TokenType::EQUAL;
        if (s == "ARROW") return TokenType::ARROW;
        if (s == "RIGHT_ARROW") return TokenType::RIGHT_ARROW;
        if (s == "PIPE") return TokenType::PIPE;
        if (s == "BANG_EQUAL") return TokenType::BANG_EQUAL;
        if (s == "PLUS_ASSIGN") return TokenType::PLUS_ASSIGN;
        if (s == "MINUS_ASSIGN") return TokenType::MINUS_ASSIGN;
        if (s == "STAR_ASSIGN") return TokenType::STAR_ASSIGN;
        if (s == "SLASH_ASSIGN") return TokenType::SLASH_ASSIGN;
        if (s == "PERCENT_ASSIGN") return TokenType::PERCENT_ASSIGN;
        if (s == "CARET_ASSIGN") return TokenType::CARET_ASSIGN;
        if (s == "BACKSLASH_ASSIGN") return TokenType::BACKSLASH_ASSIGN;
        if (s == "BIT_AND_ASSIGN") return TokenType::BIT_AND_ASSIGN;
        if (s == "BIT_OR_ASSIGN") return TokenType::BIT_OR_ASSIGN;
        if (s == "BIT_XOR_ASSIGN") return TokenType::BIT_XOR_ASSIGN;
        if (s == "SHIFT_LEFT_ASSIGN") return TokenType::SHIFT_LEFT_ASSIGN;
        if (s == "SHIFT_RIGHT_ASSIGN") return TokenType::SHIFT_RIGHT_ASSIGN;
        if (s == "AND_AND") return TokenType::AND_AND;
        if (s == "OR_OR") return TokenType::OR_OR;
        if (s == "BANG") return TokenType::BANG;
        if (s == "TILDE") return TokenType::TILDE;
        if (s == "BIT_AND") return TokenType::BIT_AND;
        if (s == "BIT_OR") return TokenType::BIT_OR;
        if (s == "BIT_XOR") return TokenType::BIT_XOR;
        if (s == "SHIFT_LEFT") return TokenType::SHIFT_LEFT;
        if (s == "SHIFT_RIGHT") return TokenType::SHIFT_RIGHT;
        if (s == "GREATER") return TokenType::GREATER;
        if (s == "GREATER_EQUAL") return TokenType::GREATER_EQUAL;
        if (s == "LESS") return TokenType::LESS;
        if (s == "LESS_EQUAL") return TokenType::LESS_EQUAL;
        if (s == "SUBSET") return TokenType::SUBSET;
        if (s == "LPAREN") return TokenType::LPAREN;
        if (s == "RPAREN") return TokenType::RPAREN;
        if (s == "LBRACKET") return TokenType::LBRACKET;
        if (s == "RBRACKET") return TokenType::RBRACKET;
        if (s == "LBRACE") return TokenType::LBRACE;
        if (s == "RBRACE") return TokenType::RBRACE;
        if (s == "COMMA") return TokenType::COMMA;
        if (s == "SEMICOLON") return TokenType::SEMICOLON;
        if (s == "QUESTION") return TokenType::QUESTION;
        if (s == "COLON") return TokenType::COLON;
        if (s == "DOT") return TokenType::DOT;
        if (s == "ELLIPSIS") return TokenType::ELLIPSIS;
        if (s == "AT") return TokenType::AT;
        if (s == "DOLLAR") return TokenType::DOLLAR;
        if (s == "CLASS") return TokenType::CLASS;
        if (s == "NAMESPACE") return TokenType::NAMESPACE;
        if (s == "ENUM") return TokenType::ENUM;
        if (s == "SUPER") return TokenType::SUPER;
        if (s == "SELF") return TokenType::SELF;
        if (s == "TRUE") return TokenType::TRUE_KW;
        if (s == "FALSE") return TokenType::FALSE_KW;
        if (s == "NONE") return TokenType::NONE_KW;
        if (s == "EXTENDS") return TokenType::EXTENDS;
        if (s == "IF") return TokenType::IF;
        if (s == "ELSE") return TokenType::ELSE;
        if (s == "WHILE") return TokenType::WHILE;
        if (s == "FOR") return TokenType::FOR;
        if (s == "IN") return TokenType::IN;
        if (s == "IS") return TokenType::IS;
        if (s == "AS") return TokenType::AS;
        if (s == "BREAK") return TokenType::BREAK;
        if (s == "CONTINUE") return TokenType::CONTINUE;
        if (s == "RETURN") return TokenType::RETURN;
        if (s == "SWITCH") return TokenType::SWITCH;
        if (s == "CASE") return TokenType::CASE;
        if (s == "DEFAULT") return TokenType::DEFAULT;
        if (s == "MATCH") return TokenType::MATCH;
        if (s == "MACRO") return TokenType::MACRO;
        if (s == "SYNTAX") return TokenType::SYNTAX;
        if (s == "QUOTE") return TokenType::QUOTE;
        if (s == "DEFER") return TokenType::DEFER;
        if (s == "LOCAL") return TokenType::LOCAL;
        if (s == "IMPORT") return TokenType::IMPORT;
        if (s == "REF") return TokenType::REF;
        if (s == "STATE") return TokenType::STATE;
        if (s == "CONST") return TokenType::CONST;
        if (s == "STATIC") return TokenType::STATIC;
        if (s == "DELETE") return TokenType::DELETE;
        if (s == "NEWLINE") return TokenType::NEWLINE;
        if (s == "EOF") return TokenType::END_OF_FILE;
        if (s == "ERROR") return TokenType::ERROR;
        if (s == "THROW") return TokenType::THROW;
        if (s == "TRY") return TokenType::TRY;
        if (s == "CATCH") return TokenType::CATCH;
        
        // Fallback for old ASTConverter stringToTokenType
        if (s == "+") return TokenType::PLUS;
        if (s == "-") return TokenType::MINUS;
        if (s == "*") return TokenType::STAR;
        if (s == "/") return TokenType::SLASH;
        if (s == "^") return TokenType::CARET;
        if (s == "\\") return TokenType::BACKSLASH;
        if (s == "%") return TokenType::PERCENT;
        if (s == "=") return TokenType::ASSIGN;
        if (s == "==") return TokenType::EQUAL;
        if (s == "=>") return TokenType::ARROW;
        if (s == "->") return TokenType::RIGHT_ARROW;
        if (s == "|>") return TokenType::PIPE;
        if (s == "!=") return TokenType::BANG_EQUAL;
        if (s == "+=") return TokenType::PLUS_ASSIGN;
        if (s == "-=") return TokenType::MINUS_ASSIGN;
        if (s == "*=") return TokenType::STAR_ASSIGN;
        if (s == "/=") return TokenType::SLASH_ASSIGN;
        if (s == "~/=") return TokenType::TILDE_SLASH_ASSIGN;
        if (s == "%=") return TokenType::PERCENT_ASSIGN;
        if (s == "^=") return TokenType::CARET_ASSIGN;
        if (s == "\\=") return TokenType::BACKSLASH_ASSIGN;
        if (s == "&=") return TokenType::BIT_AND_ASSIGN;
        if (s == "|=") return TokenType::BIT_OR_ASSIGN;
        if (s == "^^=") return TokenType::BIT_XOR_ASSIGN;
        if (s == "<<=") return TokenType::SHIFT_LEFT_ASSIGN;
        if (s == ">>=") return TokenType::SHIFT_RIGHT_ASSIGN;
        if (s == "&&") return TokenType::AND_AND;
        if (s == "||") return TokenType::OR_OR;
        if (s == "!") return TokenType::BANG;
        if (s == "~") return TokenType::TILDE;
        if (s == "~/") return TokenType::TILDE_SLASH;
        if (s == "&") return TokenType::BIT_AND;
        if (s == "|") return TokenType::BIT_OR;
        if (s == "^^") return TokenType::BIT_XOR;
        if (s == "<<") return TokenType::SHIFT_LEFT;
        if (s == ">>") return TokenType::SHIFT_RIGHT;
        if (s == ">") return TokenType::GREATER;
        if (s == ">=") return TokenType::GREATER_EQUAL;
        if (s == "<") return TokenType::LESS;
        if (s == "<=") return TokenType::LESS_EQUAL;
        if (s == "<:") return TokenType::SUBSET;
        if (s == "in") return TokenType::IN;
        if (s == "is") return TokenType::IS;
        if (s == "as") return TokenType::AS;
        if (s == "...") return TokenType::ELLIPSIS;
        if (s == "$") return TokenType::DOLLAR;
        return TokenType::IDENTIFIER;
    }

} // namespace jc
#endif // JC2_TOKEN_H
