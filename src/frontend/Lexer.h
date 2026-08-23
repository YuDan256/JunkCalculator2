#ifndef JC2_LEXER_H
#define JC2_LEXER_H

#include "Token.h"
#include <string>
#include <vector>

namespace jc {

    struct Directive {
        std::string name;  // 指令名（如 "!" 或 "pragma"）
        std::string args;  // 指令名后的原始内容（到行尾）
        int line = 0;
    };

    class Lexer {
    private:
        std::string source;         // 要解析的原始字符串
        std::vector<Token> tokens;  // 存放切分好的词法单元

        int start = 0;              // 当前正在扫描的单词的起始位置
        int current = 0;            // 当前扫描到的字符位置
        int parenBracketDepth = 0;
        int line = 1;

        std::string sourceFile;

        // --- 内部核心扫描逻辑 ---
        void scanToken();           // 识别下一个单词
        void identifier();          // 处理字母开头的标识符 (变量或函数，如 sin, x_1)
        void number();              // 处理数字 (如 123, 3.14)

        // --- 游标控制工具函数 ---
        bool isAtEnd() const;       // 判断是否读到了字符串末尾
        char advance();             // 游标前进一步，并返回上一个字符（消费字符）
        char peek() const;          // 看一眼当前字符，但不前进（只看不吃）
        char peekNext() const;      // 往后多看一眼（用于判断小数点后的数字等）
        char peekNextNext() const;
        bool match(char expected);  // 条件前进：如果当前字符匹配，则吃掉它并返回 true

        // --- 添加 Token 工具 ---
        void addToken(TokenType type);

        void stringLiteral(char quoteChar);
        void multilineStringLiteral(char quoteChar);
        void fstringLiteral(char quoteChar);  // ★
        void fmultilineStringLiteral(char quoteChar);
        void rstringLiteral(char quoteChar);  // ★
        void rmultilineStringLiteral(char quoteChar);

        void multilineComment();

        void emitError(const std::string& msg);

    public:
        std::vector<Directive> directives;  // 收集到的编译器/VM 指令（行首 # 开头）

        explicit Lexer(std::string source, std::string sourceFile = "");

        // 启动词法分析的主入口
        std::vector<Token> tokenize();
    };

} // namespace jc

#endif // JC2_LEXER_H
