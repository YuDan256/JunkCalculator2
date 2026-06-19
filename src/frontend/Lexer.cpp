#include "Lexer.h"
#include <cctype>
#include <stdexcept>
#include <unordered_map>   // ★ 新增
#include <filesystem>
#include <string_view>

namespace jc {

    static const std::unordered_map<std::string_view, TokenType> keywords = {
        {"if",       TokenType::IF},
        {"else",     TokenType::ELSE},
        {"while",    TokenType::WHILE},
        {"for",      TokenType::FOR},
        {"break",    TokenType::BREAK},
        {"continue", TokenType::CONTINUE},
        {"return",   TokenType::RETURN},
        {"local",    TokenType::LOCAL},
        {"ref",      TokenType::REF},
        {"state",    TokenType::STATE},
        {"const",    TokenType::CONST},
        {"delete",   TokenType::DELETE},
        {"in",       TokenType::IN},
        {"throw",    TokenType::THROW},        // ★
        {"try",      TokenType::TRY},          // ★
        {"catch",    TokenType::CATCH},         // ★
        {"import",   TokenType::IMPORT},
        {"switch",   TokenType::SWITCH},       // ★
        {"case",     TokenType::CASE},         // ★
        {"default",  TokenType::DEFAULT},      // ★
        {"match",    TokenType::MATCH},        // ★
        {"class",    TokenType::CLASS},
        {"namespace",TokenType::NAMESPACE},
        {"super",    TokenType::SUPER},
        {"self",     TokenType::SELF},
        {"true",     TokenType::TRUE_KW},
        {"false",    TokenType::FALSE_KW},
        {"none",     TokenType::NONE_KW},
    };

    static bool isContinuationToken(TokenType t) {
        switch (t) {
            // 二元运算符
        case TokenType::PLUS: case TokenType::MINUS:
        case TokenType::STAR: case TokenType::SLASH:
        case TokenType::CARET: case TokenType::BACKSLASH:
        case TokenType::PERCENT:
            // 赋值
        case TokenType::ASSIGN:
        case TokenType::PLUS_ASSIGN: case TokenType::MINUS_ASSIGN:
        case TokenType::STAR_ASSIGN: case TokenType::SLASH_ASSIGN:
        case TokenType::PERCENT_ASSIGN: case TokenType::CARET_ASSIGN:
        case TokenType::BACKSLASH_ASSIGN:
        case TokenType::BIT_AND_ASSIGN: case TokenType::BIT_OR_ASSIGN: case TokenType::BIT_XOR_ASSIGN: // ★
        case TokenType::SHIFT_LEFT_ASSIGN: case TokenType::SHIFT_RIGHT_ASSIGN:
            // 比较
        case TokenType::EQUAL: case TokenType::BANG_EQUAL:
        case TokenType::LESS: case TokenType::LESS_EQUAL:
        case TokenType::GREATER: case TokenType::GREATER_EQUAL:
        case TokenType::SHIFT_LEFT: case TokenType::SHIFT_RIGHT:
            // 逻辑
        case TokenType::AND_AND: case TokenType::OR_OR:
        case TokenType::BIT_AND: case TokenType::BIT_OR: case TokenType::BIT_XOR:
        case TokenType::BANG: case TokenType::TILDE: // ★
            // 管道与箭头
        case TokenType::PIPE: case TokenType::ARROW: case TokenType::RIGHT_ARROW:
            // 标点
        case TokenType::COMMA: case TokenType::DOT:
        case TokenType::COLON: case TokenType::QUESTION:
        case TokenType::SEMICOLON: case TokenType::ELLIPSIS: // ★
            // 开括号
        case TokenType::LPAREN: case TokenType::LBRACKET:
        case TokenType::LBRACE:
            // 关键字 (期待后续表达式或块)
        case TokenType::IN:
            // 已有 NEWLINE 不重复发射
        case TokenType::NEWLINE:
            return true;
        default:
            return false;
        }
    }

    Lexer::Lexer(std::string source, std::string sourceFile)
        : source(std::move(source)), sourceFile(std::move(sourceFile)) {
    }

    void Lexer::throwError(const std::string& msg) {
        std::string fn = "Script";
        try { fn = std::filesystem::path(sourceFile).filename().string(); }
        catch (...) {}
        if (fn.empty()) fn = "Script";
        throw std::runtime_error("[" + fn + " : " + std::to_string(line) + "] Lexer Error: " + msg);
    }

    std::vector<Token> Lexer::tokenize() {
        while (!isAtEnd()) {
            start = current;
            scanToken();
        }
        tokens.emplace_back(TokenType::END_OF_FILE, "", current, line);
        return tokens;
    }

    void Lexer::scanToken() {
        char c = advance();
        switch (c) {
        case '(':
            parenBracketDepth++;
            addToken(TokenType::LPAREN);
            break;
        case ')':
            if (parenBracketDepth > 0) parenBracketDepth--;
            addToken(TokenType::RPAREN);
            break;
        case '[':
            parenBracketDepth++;
            addToken(TokenType::LBRACKET);
            break;
        case ']':
            if (parenBracketDepth > 0) parenBracketDepth--;
            addToken(TokenType::RBRACKET);
            break;
        case '{': addToken(TokenType::LBRACE); break;    // ★ 新增
        case '}': addToken(TokenType::RBRACE); break;    // ★ 新增
        case '~': addToken(TokenType::TILDE); break;     // ★ 新增
        case ',': addToken(TokenType::COMMA); break;
        case ';': addToken(TokenType::SEMICOLON); break;
        case '?': addToken(TokenType::QUESTION); break;    // ★
        case ':': addToken(TokenType::COLON); break;       // ★
        case '@': addToken(TokenType::AT); break;          // ★
        case '.':
            if (match('.') && match('.')) {
                addToken(TokenType::ELLIPSIS);
            }
            else {
                addToken(TokenType::DOT);
            }
            break;
        case '+':
            addToken(match('=') ? TokenType::PLUS_ASSIGN : TokenType::PLUS);
            break;
        case '-':
            if (match('>')) {
                addToken(TokenType::RIGHT_ARROW);           // ★ 匹配 ->
            }
            else if (match('=')) {
                addToken(TokenType::MINUS_ASSIGN);          // 匹配 -=
            }
            else {
                addToken(TokenType::MINUS);                 // 纯减号
            }
            break;
        case '*':
            addToken(match('=') ? TokenType::STAR_ASSIGN : TokenType::STAR);
            break;
        case '/':
            if (match('/')) {
                // 单行注释
                while (!isAtEnd() && peek() != '\n') advance();
            }
            else if (match('*')) {
                // 多行注释
                multilineComment();
            }
            else if (match('=')) {
                addToken(TokenType::SLASH_ASSIGN);
            }
            else {
                addToken(TokenType::SLASH);
            }
            break;
        case '%':
            addToken(match('=') ? TokenType::PERCENT_ASSIGN : TokenType::PERCENT);
            break;
        case '^':
            if (match('^')) {
                addToken(match('=') ? TokenType::BIT_XOR_ASSIGN : TokenType::BIT_XOR);
            } else {
                addToken(match('=') ? TokenType::CARET_ASSIGN : TokenType::CARET);
            }
            break;
        case '\\':
            addToken(match('=') ? TokenType::BACKSLASH_ASSIGN : TokenType::BACKSLASH);
            break;
        case '"':
            if (peek() == '"' && peekNext() == '"') {
                advance(); advance();
                multilineStringLiteral('"');
            } else {
                stringLiteral('"');
            }
            break;
        case '\'':
            if (peek() == '\'' && peekNext() == '\'') {
                advance(); advance();
                multilineStringLiteral('\'');
            } else {
                stringLiteral('\'');
            }
            break;
        case '=':
            if (match('=')) addToken(TokenType::EQUAL);
            else if (match('>')) addToken(TokenType::ARROW);      // ★
            else addToken(TokenType::ASSIGN);
            break;
        case '!':
            if (match('=')) addToken(TokenType::BANG_EQUAL);
            else addToken(TokenType::BANG);           // ★ 不再报错，作为逻辑 NOT
            break;
        case '&':
            if (match('=')) addToken(TokenType::BIT_AND_ASSIGN);       // ★
            else if (match('&')) addToken(TokenType::AND_AND);
            else addToken(TokenType::BIT_AND);
            break;
        case '|':
            if (match('=')) addToken(TokenType::BIT_OR_ASSIGN);        // ★
            else if (match('|')) addToken(TokenType::OR_OR);
            else if (match('>')) addToken(TokenType::PIPE);
            else addToken(TokenType::BIT_OR);
            break;
        case '<':
            if (match('<')) {
                addToken(match('=') ? TokenType::SHIFT_LEFT_ASSIGN : TokenType::SHIFT_LEFT);
            } else {
                addToken(match('=') ? TokenType::LESS_EQUAL : TokenType::LESS);
            }
            break;
        case '>':
            if (match('>')) {
                addToken(match('=') ? TokenType::SHIFT_RIGHT_ASSIGN : TokenType::SHIFT_RIGHT);
            } else {
                addToken(match('=') ? TokenType::GREATER_EQUAL : TokenType::GREATER);
            }
            break;
        case ' ': case '\r': case '\t':
            break;
        case '\n':
            line++;
            // ★ 智能换行符：当不在 () 或 [] 内部、且上一个 token 不是续行符时发射
            if (parenBracketDepth == 0 && !tokens.empty()) {
                TokenType lastType = tokens.back().type;
                if (!isContinuationToken(lastType)) {
                    tokens.emplace_back(TokenType::NEWLINE, "\\n", current - 1, line - 1);
                }
            }
            break;
        default:
            if (std::isdigit(c)) { number(); }
            else if (c == 'f' && (peek() == '"' || peek() == '\'')) { // ★ 支持 f" 和 f'
                char quote = advance(); // consume opening quote
                if (peek() == quote && peekNext() == quote) {
                    advance(); advance();
                    fmultilineStringLiteral(quote);
                } else {
                    fstringLiteral(quote);
                }
            }
            else if (c == 'r' && (peek() == '"' || peek() == '\'')) { // ★ 支持 r" 和 r'
                char quote = advance(); // consume opening quote
                if (peek() == quote && peekNext() == quote) {
                    advance(); advance();
                    rmultilineStringLiteral(quote);
                } else {
                    rstringLiteral(quote);
                }
            }
            else if (std::isalpha(c) || c == '_') { identifier(); }
            else {
                throwError("Unexpected character '" + std::string(1, c) + "'.");
            }
            break;
        }
    }

    // ★ 修改：扫描完标识符后查关键字表
    void Lexer::identifier() {
        while (std::isalnum(peek()) || peek() == '_') {
            advance();
        }
        std::string_view text(source.data() + start, current - start);
        auto it = keywords.find(text);
        if (it != keywords.end()) {
            addToken(it->second);   // ★ 命中关键字
        }
        else {
            addToken(TokenType::IDENTIFIER);
        }
    }

    void Lexer::number() {
        char firstDigit = source[current - 1];
        bool isHexOctBin = false;
        if (firstDigit == '0') {
            char next = peek();
            if (next == 'x' || next == 'X') {
                advance(); // consume 'x'
                if (!std::isxdigit(peek())) throwError("Invalid hex literal.");
                while (std::isxdigit(peek())) advance();
                isHexOctBin = true;
            } else if (next == 'b' || next == 'B') {
                advance(); // consume 'b'
                if (peek() != '0' && peek() != '1') throwError("Invalid binary literal.");
                while (peek() == '0' || peek() == '1') advance();
                isHexOctBin = true;
            } else if (next == 'o' || next == 'O') {
                advance(); // consume 'o'
                if (peek() < '0' || peek() > '7') throwError("Invalid octal literal.");
                while (peek() >= '0' && peek() <= '7') advance();
                isHexOctBin = true;
            }
        }

        if (!isHexOctBin) {
            while (std::isdigit(peek())) advance();
            if (peek() == '.' && std::isdigit(peekNext())) {
                advance();
                while (std::isdigit(peek())) advance();
            }
            if (peek() == 'e' || peek() == 'E') {
                char next = peekNext();
                bool hasSign = (next == '+' || next == '-');
                bool isValidScientific = false;
                if (hasSign) { if (std::isdigit(peekNextNext())) isValidScientific = true; }
                else if (std::isdigit(next)) isValidScientific = true;
                if (isValidScientific) {
                    advance();
                    if (hasSign) advance();
                    while (std::isdigit(peek())) advance();
                }
            }
        }

        // ★ 虚数后缀: 3i, 3.14i, 1e3i, 0x1Ai
        if (peek() == 'i') {
            char next = peekNext();
            // i 后面必须是"非标识符延续字符"
            bool validEnd = (next == '\0' || next == ' ' || next == '\t' ||
                next == '+' || next == '-' || next == '*' || next == '/' ||
                next == '^' || next == '%' || next == '\\' ||
                next == ')' || next == ']' || next == '}' ||
                next == ',' || next == ';' || next == '\n' || next == '\r' ||
                next == '|' || next == '&' || next == '!' ||
                next == '?' || next == ':' || next == '=' ||
                next == '<' || next == '>' || next == '"');
            if (validEnd) {
                advance(); // consume 'i'
                addToken(TokenType::IMAGINARY);
                return;
            }
        }
        
        if (std::isalnum(peek()) || peek() == '_') {
            throwError("Invalid character '" + std::string(1, peek()) + "' in number literal.");
        }

        addToken(TokenType::NUMBER);
    }

    bool Lexer::isAtEnd() const { return current >= (int)source.length(); }
    char Lexer::advance() { return source[current++]; }
    char Lexer::peek() const { if (isAtEnd()) return '\0'; return source[current]; }
    char Lexer::peekNext() const { if (current + 1 >= (int)source.length()) return '\0'; return source[current + 1]; }
    char Lexer::peekNextNext() const { if (current + 2 >= (int)source.length()) return '\0'; return source[current + 2]; }
    bool Lexer::match(char expected) { if (isAtEnd() || source[current] != expected) return false; current++; return true; }
    void Lexer::addToken(TokenType type) { tokens.emplace_back(type, source.substr(start, current - start), start, line); }

    void Lexer::multilineStringLiteral(char quoteChar) {
        std::string value;
        while (!isAtEnd()) {
            if (peek() == quoteChar && peekNext() == quoteChar && peekNextNext() == quoteChar) {
                break;
            }
            if (peek() == '\n') line++;
            char c = advance();
            if (c == '\\' && !isAtEnd()) {
                char esc = advance();
                switch (esc) {
                case 'n':  value += '\n'; break;
                case 't':  value += '\t'; break;
                case 'r':  value += '\r'; break;
                case '\\': value += '\\'; break;
                case '"':  value += '"';  break;
                case '\'': value += '\''; break;
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    std::string octStr;
                    octStr += esc;
                    if (current < static_cast<int>(source.length()) && source[current] >= '0' && source[current] <= '7') {
                        octStr += source[current++];
                        if (current < static_cast<int>(source.length()) && source[current] >= '0' && source[current] <= '7') {
                            octStr += source[current++];
                        }
                    }
                    value += static_cast<char>(std::stoi(octStr, nullptr, 8));
                    break;
                }
                case 'a':  value += '\a'; break;
                case 'b':  value += '\b'; break;
                case 'f':  value += '\f'; break;
                case 'v':  value += '\v'; break;
                case 'x': {
                    if (current + 1 < static_cast<int>(source.length()) && std::isxdigit(source[current]) && std::isxdigit(source[current+1])) {
                        std::string hexStr = source.substr(current, 2);
                        value += static_cast<char>(std::stoi(hexStr, nullptr, 16));
                        current += 2;
                    } else {
                        value += "\\x";
                    }
                    break;
                }
                case '\n': line++; break; // 忽略反斜杠加换行
                default:
                    value += '\\';
                    value += esc;
                    break;
                }
            }
            else {
                value += c;
            }
        }
        if (isAtEnd()) {
            throwError("Unterminated multiline string.");
        }
        advance(); advance(); advance(); // 吃掉对应的三个引号
        tokens.emplace_back(TokenType::STRING, value, start);
    }

    void Lexer::stringLiteral(char quoteChar) {
        std::string value;
        while (peek() != quoteChar && !isAtEnd()) {
            if (peek() == '\n') line++;
            char c = advance();
            if (c == '\\' && !isAtEnd()) {
                // ★ 转义序列处理
                char esc = advance();
                switch (esc) {
                case 'n':  value += '\n'; break;
                case 't':  value += '\t'; break;
                case 'r':  value += '\r'; break;
                case '\\': value += '\\'; break;
                case '"':  value += '"';  break;
                case '\'': value += '\''; break; // ★ 支持单引号转义
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    std::string octStr;
                    octStr += esc;
                    if (current < static_cast<int>(source.length()) && source[current] >= '0' && source[current] <= '7') {
                        octStr += source[current++];
                        if (current < static_cast<int>(source.length()) && source[current] >= '0' && source[current] <= '7') {
                            octStr += source[current++];
                        }
                    }
                    value += static_cast<char>(std::stoi(octStr, nullptr, 8));
                    break;
                }
                case 'a':  value += '\a'; break;
                case 'b':  value += '\b'; break;
                case 'f':  value += '\f'; break;
                case 'v':  value += '\v'; break;
                case 'x': {
                    if (current + 1 < static_cast<int>(source.length()) && std::isxdigit(source[current]) && std::isxdigit(source[current+1])) {
                        std::string hexStr = source.substr(current, 2);
                        value += static_cast<char>(std::stoi(hexStr, nullptr, 16));
                        current += 2;
                    } else {
                        value += "\\x";
                    }
                    break;
                }
                default:
                    value += '\\';
                    value += esc;
                    break;
                }
            }
            else {
                value += c;
            }
        }
        if (isAtEnd()) {
            throwError("Unterminated string.");
        }
        advance(); // 吃掉对应的右引号
        tokens.emplace_back(TokenType::STRING, value, start);
    }

    void Lexer::fmultilineStringLiteral(char quoteChar) {
        std::string value;
        while (!isAtEnd()) {
            if (peek() == quoteChar && peekNext() == quoteChar && peekNextNext() == quoteChar) {
                break;
            }
            char c = peek();
            if (c == '{') {
                value += advance();
                int depth = 1;
                char inStrQuote = '\0';
                while (!isAtEnd() && depth > 0) {
                    c = peek();
                    if (inStrQuote != '\0') {
                        if (c == '\\' && !isAtEnd()) {
                            value += advance(); value += advance();
                        }
                        else if (c == inStrQuote) {
                            inStrQuote = '\0';
                            value += advance();
                        }
                        else {
                            value += advance();
                        }
                    }
                    else {
                        if (c == '"' || c == '\'') {
                            inStrQuote = c;
                            value += advance();
                        }
                        else if (c == '{') { depth++; value += advance(); }
                        else if (c == '}') { depth--; value += advance(); }
                        else { value += advance(); }
                    }
                }
                if (depth != 0)
                    throwError("Unmatched '{' in f-string.");
            }
            else if (c == '\\') {
                advance();
                if (isAtEnd()) break;
                char esc = advance();
                switch (esc) {
                case 'n':  value += '\n'; break;
                case 't':  value += '\t'; break;
                case 'r':  value += '\r'; break;
                case '\\': value += '\\'; break;
                case '"':  value += '"';  break;
                case '\'': value += '\''; break;
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    std::string octStr;
                    octStr += esc;
                    if (current < static_cast<int>(source.length()) && source[current] >= '0' && source[current] <= '7') {
                        octStr += source[current++];
                        if (current < static_cast<int>(source.length()) && source[current] >= '0' && source[current] <= '7') {
                            octStr += source[current++];
                        }
                    }
                    value += static_cast<char>(std::stoi(octStr, nullptr, 8));
                    break;
                }
                case 'a':  value += '\a'; break;
                case 'b':  value += '\b'; break;
                case 'f':  value += '\f'; break;
                case 'v':  value += '\v'; break;
                case 'x': {
                    if (current + 1 < static_cast<int>(source.length()) && std::isxdigit(source[current]) && std::isxdigit(source[current+1])) {
                        std::string hexStr = source.substr(current, 2);
                        value += static_cast<char>(std::stoi(hexStr, nullptr, 16));
                        current += 2;
                    } else {
                        value += "\\x";
                    }
                    break;
                }
                case '\n': line++; break;
                default:   value += '\\'; value += esc; break;
                }
            }
            else {
                if (c == '\n') line++;
                value += advance();
            }
        }
        if (isAtEnd())
            throwError("Unterminated multiline f-string.");
        advance(); advance(); advance(); // consume closing quotes
        tokens.emplace_back(TokenType::FSTRING, value, start);
    }

    void Lexer::fstringLiteral(char quoteChar) {
        std::string value;
        while (!isAtEnd() && peek() != quoteChar) {
            char c = peek();
            if (c == '{') {
                // ★ 表达式段：追踪花括号深度
                value += advance(); // consume and include '{'
                int depth = 1;
                char inStrQuote = '\0'; // ★ 动态跟踪内部嵌套的字符串界定符
                while (!isAtEnd() && depth > 0) {
                    c = peek();
                    if (inStrQuote != '\0') {
                        // ★ 在表达式内的字符串中
                        if (c == '\\' && !isAtEnd()) {
                            value += advance(); value += advance();
                        }
                        else if (c == inStrQuote) {
                            inStrQuote = '\0';
                            value += advance();
                        }
                        else {
                            value += advance();
                        }
                    }
                    else {
                        // ★ 在表达式中，但不在字符串内
                        if (c == '"' || c == '\'') {
                            inStrQuote = c;
                            value += advance();
                        }
                        else if (c == '{') { depth++; value += advance(); }
                        else if (c == '}') { depth--; value += advance(); }
                        else { value += advance(); }
                    }
                }
                if (depth != 0)
                    throwError("Unmatched '{' in f-string.");
            }
            else if (c == '\\') {
                // ★ 文本段的转义序列
                advance();
                if (isAtEnd()) break;
                char esc = advance();
                switch (esc) {
                case 'n':  value += '\n'; break;
                case 't':  value += '\t'; break;
                case 'r':  value += '\r'; break;
                case '\\': value += '\\'; break;
                case '"':  value += '"';  break;
                case '\'': value += '\''; break; // ★
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    std::string octStr;
                    octStr += esc;
                    if (current < static_cast<int>(source.length()) && source[current] >= '0' && source[current] <= '7') {
                        octStr += source[current++];
                        if (current < static_cast<int>(source.length()) && source[current] >= '0' && source[current] <= '7') {
                            octStr += source[current++];
                        }
                    }
                    value += static_cast<char>(std::stoi(octStr, nullptr, 8));
                    break;
                }
                case 'a':  value += '\a'; break;
                case 'b':  value += '\b'; break;
                case 'f':  value += '\f'; break;
                case 'v':  value += '\v'; break;
                case 'x': {
                    if (current + 1 < static_cast<int>(source.length()) && std::isxdigit(source[current]) && std::isxdigit(source[current+1])) {
                        std::string hexStr = source.substr(current, 2);
                        value += static_cast<char>(std::stoi(hexStr, nullptr, 16));
                        current += 2;
                    } else {
                        value += "\\x";
                    }
                    break;
                }
                default:   value += '\\'; value += esc; break;
                }
            }
            else {
                if (c == '\n') line++;
                value += advance();
            }
        }
        if (isAtEnd())
            throwError("Unterminated f-string.");
        advance(); // consume closing quote
        tokens.emplace_back(TokenType::FSTRING, value, start);
    }

    void Lexer::rmultilineStringLiteral(char quoteChar) {
        std::string value;
        while (!isAtEnd()) {
            if (peek() == quoteChar && peekNext() == quoteChar && peekNextNext() == quoteChar) {
                break;
            }
            if (peek() == '\n') line++;
            value += advance();
        }
        if (isAtEnd()) throwError("Unterminated multiline raw string.");
        advance(); advance(); advance();
        tokens.emplace_back(TokenType::RSTRING, value, start);
    }

    void Lexer::rstringLiteral(char quoteChar) {
        int probePos = current;
        std::string delimiter;
        bool hasDelimiter = false;

        while (probePos < static_cast<int>(source.size())) {
            char c = source[probePos];
            if (c == '(') {
                hasDelimiter = true;
                break;
            }
            else if (std::isalnum(c) || c == '_') {
                delimiter += c;
                probePos++;
            }
            else { break; }
        }

        if (hasDelimiter) {
            current = probePos + 1;
            // ★ 修改结束标记匹配符为当前的 quoteChar
            std::string endMarker = ")" + delimiter + quoteChar;
            size_t endLen = endMarker.size();

            std::string value;
            while (current < static_cast<int>(source.size())) {
                if (source[current] == '\n') line++;
                if (source[current] == ')' &&
                    current + static_cast<int>(endLen) <= static_cast<int>(source.size())) {
                    bool match = true;
                    for (size_t k = 0; k < endLen; ++k) {
                        if (source[current + k] != endMarker[k]) {
                            match = false; break;
                        }
                    }
                    if (match) {
                        current += static_cast<int>(endLen);
                        tokens.emplace_back(TokenType::RSTRING, value, start);
                        return;
                    }
                }
                value += source[current++];
            }
            throwError("Unterminated raw string (expected " + endMarker + ").");
        }
        else {
            std::string value;
            while (!isAtEnd() && peek() != quoteChar) {
                if (peek() == '\n') line++;
                value += advance();
            }
            if (isAtEnd()) throwError("Unterminated raw string.");
            advance();
            tokens.emplace_back(TokenType::RSTRING, value, start);
        }
    }

    void Lexer::multilineComment() {
        int nesting = 1;
        while (nesting > 0 && !isAtEnd()) {
            if (peek() == '\n') {
                line++;
                advance();
            } else if (peek() == '/' && peekNext() == '*') {
                advance();
                advance();
                nesting++;
            } else if (peek() == '*' && peekNext() == '/') {
                advance();
                advance();
                nesting--;
            } else {
                advance();
            }
        }
        if (nesting > 0) {
            throwError("Unterminated multiline comment.");
        }
    }
} // namespace jc
