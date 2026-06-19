#ifndef JC2_PARSER_H
#define JC2_PARSER_H

#include "Expr.h"
#include "Token.h"
#include <memory>
#include <stdexcept>
#include <vector>

namespace jc {

    class Parser {
    private:
        std::vector<Token> tokens;
        int current = 0;
        std::string sourceFile;

        // --- 文法规则 (优先级从低到高) ---
        std::unique_ptr<Expr> expression();
        std::unique_ptr<Expr> assignment();
        std::unique_ptr<Expr> logicalOr();
        std::unique_ptr<Expr> logicalAnd();
        std::unique_ptr<Expr> comparison();
        std::unique_ptr<Expr> bitwiseOr();
        std::unique_ptr<Expr> bitwiseXor();
        std::unique_ptr<Expr> bitwiseAnd();
        std::unique_ptr<Expr> shift();
        std::unique_ptr<Expr> addition();
        std::unique_ptr<Expr> multiplication();
        std::unique_ptr<Expr> power();
        std::unique_ptr<Expr> unary();
        std::unique_ptr<Expr> call();
        std::unique_ptr<Expr> primary();
        std::unique_ptr<Expr> ternary();

        // ★ 新增：控制流解析
        std::unique_ptr<Expr> parseBlock();
        std::unique_ptr<Expr> parseStatementOrBlock();
        std::unique_ptr<Expr> ifExpr();
        std::unique_ptr<Expr> whileExpr();
        std::unique_ptr<Expr> forExpr();
        std::unique_ptr<Expr> forInExpr(Token varName);
        std::unique_ptr<Expr> switchExpr();
        std::unique_ptr<Expr> matchExpr();     // ★
        std::unique_ptr<Pattern> parsePrimaryPattern(); // ★
        std::unique_ptr<Pattern> parsePattern(); // ★
        std::unique_ptr<Expr> parseMatchBody();  // ★
        std::unique_ptr<Expr> classDefExpr();  // ★
        std::unique_ptr<Expr> namespaceExpr(); // ★ 新增
        std::unique_ptr<Expr> parseFString(const std::string& raw);  // ★
        std::unique_ptr<Expr> parseListComp(std::unique_ptr<Expr> valueExpr);  // ★
        std::unique_ptr<Expr> pipe();
        std::unique_ptr<Expr> parseDictLiteral();  // ★
        std::unique_ptr<Expr> parseSetLiteral();   // ★ 新增

        bool isDictLiteralLookahead(int startPos);
        bool replacePlaceholders(std::unique_ptr<Expr>& expr, std::vector<Token>& phParams, std::vector<std::shared_ptr<Expr>>& phDefaults, int& phCount);

        // --- 游标工具 ---
        inline bool match(std::initializer_list<TokenType> types) { for (auto t : types) if (check(t)) { advance(); return true; } return false; }
        inline bool check(TokenType type) const { if (isAtEnd()) return false; return peek().type == type; }
        inline bool isAtEnd() const { return peek().type == TokenType::END_OF_FILE; }
        inline Token advance() { if (!isAtEnd()) current++; return previous(); }
        inline Token peek() const { return tokens[current]; }
        inline Token previous() const { return tokens[current - 1]; }
        Token consume(TokenType type, const std::string& message);

    public:
        explicit Parser(std::vector<Token> tokens, std::string sourceFile = "")
            : tokens(std::move(tokens)), sourceFile(std::move(sourceFile)) {}
        std::unique_ptr<Expr> parse();
    };

} // namespace jc
#endif // JC2_PARSER_H
