#pragma once
#include <string>

enum TokenType{
    // Literais
    TOKEN_INT_LIT,
    TOKEN_FLOAT_LIT,
    TOKEN_STRING_LIT,
    TOKEN_CHAR_LIT,

    // Tipos primitivos
    TOKEN_INT,          // i32 ou int
    TOKEN_FLOAT,        // f32 ou float
    TOKEN_STRING,       // str
    TOKEN_CHAR,         // char
    TOKEN_VOID,         // void
    TOKEN_LONG,         // i64 ou long
    TOKEN_LONGLONG,     // (compatibilidade interna)
    TOKEN_DOUBLE,       // f64 ou double
    TOKEN_BOOL,         // bool
    TOKEN_TRUE,         // true
    TOKEN_FALSE,        // false

    // Palavras-chave
    TOKEN_LET,          // let
    TOKEN_MUT,          // mut
    TOKEN_FN,           // fn
    TOKEN_IMPL,         // impl
    TOKEN_SELF,         // self (receiver de método)
    TOKEN_RETURN,       // return
    TOKEN_IF,           // if
    TOKEN_ELSE,         // else
    TOKEN_FOR,          // for
    TOKEN_WHILE,        // while
    TOKEN_IN,           // in (reservado para futuros iteradores)
    TOKEN_STRUCT,       // struct
    TOKEN_NAMESPACE,    // namespace
    TOKEN_INCLUDE,      // include
    TOKEN_ASM,          // asm
    TOKEN_IR,           // ir
    TOKEN_AS,           // as  (cast: expr as Type)

    // Identificador genérico
    TOKEN_IDENT,

    // Operadores aritméticos
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,

    // Operadores de comparação
    TOKEN_EQ,           // ==
    TOKEN_NEQ,          // !=
    TOKEN_LT,           // <
    TOKEN_GT,           // >
    TOKEN_LEQ,          // <=
    TOKEN_GEQ,          // >=

    // Operadores de atribuição
    TOKEN_ASSIGN,       // =

    // Pontuação
    TOKEN_LPAREN,       // (
    TOKEN_RPAREN,       // )
    TOKEN_LBRACE,       // {
    TOKEN_RBRACE,       // }
    TOKEN_LBRACKET,     // [
    TOKEN_RBRACKET,     // ]
    TOKEN_COMMA,        // ,
    TOKEN_SEMI,         // ;
    TOKEN_COLON,        // :
    TOKEN_COLONCOLON,   // ::
    TOKEN_DOT,          // .
    TOKEN_ARROW,        // -> (tipo de retorno de função)
    TOKEN_AMPERSAND,    // & (referência / &self)

    // Operadores lógicos
    TOKEN_AND,          // &&
    TOKEN_OR,           // ||
    TOKEN_NOT,          // !

    // Pré-processador
    TOKEN_HASH,
    TOKEN_HEADER_PATH,

    // Variádico
    TOKEN_ELLIPSIS,     // ...

    // Controle
    TOKEN_EOF,
    TOKEN_UNKNOWN,
    // ─── ENUM SUPPORT ────────────────────────────────────────────────────────
    TOKEN_ENUM,           // enum

    // ─── Legacy (mantidos para compatibilidade interna) ───────────────────────
    TOKEN_THEN          // nunca mais emitido, mantido para não quebrar switches
};

struct Token {
    TokenType type;
    std::string value;
    int line;
    int col;
};

struct LexerState {
    std::string source;
    size_t pos;
    int line;
    int col;
};

void initLexer(const std::string& src);
LexerState saveLexerState();
void restoreLexerState(const LexerState& s);
Token nextToken();
Token peekToken();
std::string getSourceLine(int lineNumber);