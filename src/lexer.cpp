#include "lexer.h"
#include "../include/error.h"
#include <cctype>
#include <sstream>

static std::string source;
static size_t pos = 0;
static int line = 1;
static int col  = 1;

void initLexer(const std::string& src) {
    source = src;
    pos  = 0;
    line = 1;
    col  = 1;
}

LexerState saveLexerState() {
    return { source, pos, line, col };
}

void restoreLexerState(const LexerState& s) {
    source = s.source;
    pos    = s.pos;
    line   = s.line;
    col    = s.col;
}

std::string getSourceLine(int lineNumber) {
    int cur = 1;
    std::string result;
    for (size_t i = 0; i < source.size(); i++) {
        if (cur == lineNumber) {
            if (source[i] == '\n') break;
            result += source[i];
        }
        if (source[i] == '\n') cur++;
    }
    return result;
}

static char peek() {
    if (pos >= source.size()) return '\0';
    return source[pos];
}

static char advance() {
    if (pos >= source.size()) return '\0';
    char c = source[pos++];
    if (c == '\n') { line++; col = 1; }
    else col++;
    return c;
}

static void skipWhiteSpace() {
    while (pos < source.size()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && pos + 1 < source.size() && source[pos+1] == '/') {
            while (pos < source.size() && peek() != '\n') advance();
        } else if (c == '/' && pos + 1 < source.size() && source[pos+1] == '*') {
            advance(); advance();
            while (pos < source.size()) {
                if (peek() == '*' && pos + 1 < source.size() && source[pos+1] == '/') {
                    advance(); advance();
                    break;
                }
                advance();
            }
        } else {
            break;
        }
    }
}

// ── Mapeamento de palavras-chave ──────────────────────────────────────────────
// Suporta tanto os nomes Rust (fn, let, mut, impl, i32, f64, str, bool…)
// quanto apelidos amigáveis (int, float, double, long, string, void, char).
static TokenType identifyKeyword(const std::string& word) {
    // ── Palavras-chave estruturais ────────────────────────────────────────────
    if (word == "fn")        return TOKEN_FN;
    if (word == "let")       return TOKEN_LET;
    if (word == "mut")       return TOKEN_MUT;
    if (word == "impl")      return TOKEN_IMPL;
    if (word == "self")      return TOKEN_SELF;
    if (word == "return")    return TOKEN_RETURN;
    if (word == "if")        return TOKEN_IF;
    if (word == "else")      return TOKEN_ELSE;
    if (word == "for")       return TOKEN_FOR;
    if (word == "while")     return TOKEN_WHILE;
    if (word == "in")        return TOKEN_IN;
    if (word == "struct")    return TOKEN_STRUCT;
    if (word == "namespace") return TOKEN_NAMESPACE;
    if (word == "include")   return TOKEN_INCLUDE;
    if (word == "asm")       return TOKEN_ASM;
    if (word == "ir")        return TOKEN_IR;
    if (word == "as")        return TOKEN_AS;
    if (word == "enum")      return TOKEN_ENUM;

    // ── Literais booleanos ────────────────────────────────────────────────────
    if (word == "true")      return TOKEN_TRUE;
    if (word == "false")     return TOKEN_FALSE;

    // ── Tipos: nomes Rust ─────────────────────────────────────────────────────
    if (word == "i32")       return TOKEN_INT;
    if (word == "i64")       return TOKEN_LONG;
    if (word == "f32")       return TOKEN_FLOAT;
    if (word == "f64")       return TOKEN_DOUBLE;
    if (word == "str")       return TOKEN_STRING;
    if (word == "bool")      return TOKEN_BOOL;

    // ── Tipos: apelidos amigáveis (retro-compatibilidade) ────────────────────
    if (word == "int")       return TOKEN_INT;
    if (word == "float")     return TOKEN_FLOAT;
    if (word == "string")    return TOKEN_STRING;
    if (word == "char")      return TOKEN_CHAR;
    if (word == "void")      return TOKEN_VOID;
    if (word == "double")    return TOKEN_DOUBLE;
    if (word == "long")      return TOKEN_LONG;

    return TOKEN_IDENT;
}

Token nextToken() {
    skipWhiteSpace();

    if (pos >= source.size())
        return {TOKEN_EOF, "", line, col};

    char c = peek();
    int tokLine = line;
    int tokCol  = col;

    // ── Números ───────────────────────────────────────────────────────────────
    if (std::isdigit((unsigned char)c)) {
        std::string num;
        bool isFloat = false;
        bool dotSeen = false;

        while (pos < source.size()) {
            char ch = peek();
            if (std::isdigit((unsigned char)ch)) {
                num += advance();
            } else if (ch == '.' && !dotSeen) {
                // Distingue 1.method() de 1.0 — só é float se vier dígito depois do ponto
                if (pos + 1 < source.size() && std::isdigit((unsigned char)source[pos+1])) {
                    dotSeen = true;
                    isFloat = true;
                    num += advance();
                } else {
                    break;
                }
            } else {
                break;
            }
        }
        // Sufixo opcional: 1i32, 3.14f64 — consome e ignora (para compatibilidade futura)
        if (pos < source.size() && (peek() == 'i' || peek() == 'f' || peek() == 'u')) {
            while (pos < source.size() && std::isalnum((unsigned char)peek()))
                advance();
        }
        return {isFloat ? TOKEN_FLOAT_LIT : TOKEN_INT_LIT, num, tokLine, tokCol};
    }

    // ── Identificadores e palavras-chave ──────────────────────────────────────
    if (std::isalpha((unsigned char)c) || c == '_') {
        std::string word;
        while (pos < source.size() &&
               (std::isalnum((unsigned char)peek()) || peek() == '_'))
            word += advance();
        return {identifyKeyword(word), word, tokLine, tokCol};
    }

    // ── String literal ────────────────────────────────────────────────────────
    if (c == '"') {
        advance();
        std::string str;
        while (pos < source.size() && peek() != '"') {
            char sc = advance();
            if (sc == '\\' && pos < source.size()) {
                char esc = advance();
                switch (esc) {
                    case 'n':  str += '\n'; break;
                    case 't':  str += '\t'; break;
                    case 'r':  str += '\r'; break;
                    case '0':  str += '\0'; break;
                    case '\\': str += '\\'; break;
                    case '"':  str += '"';  break;
                    case '\'': str += '\''; break;
                    default:   str += '\\'; str += esc; break;
                }
            } else {
                str += sc;
            }
        }
        if (pos >= source.size()) {
            std::string srcLine = getSourceLine(tokLine);
            reportError("<source>", tokLine, tokCol,
                        "unterminated string literal",
                        srcLine, 1,
                        "add a closing '\"' to end the string");
        }
        advance();
        return {TOKEN_STRING_LIT, str, tokLine, tokCol};
    }

    // ── Char literal ──────────────────────────────────────────────────────────
    if (c == '\'') {
        advance();
        char ch = '\0';
        if (peek() == '\\') {
            advance();
            char esc = advance();
            switch (esc) {
                case 'n':  ch = '\n'; break;
                case 't':  ch = '\t'; break;
                case 'r':  ch = '\r'; break;
                case '0':  ch = '\0'; break;
                case '\\': ch = '\\'; break;
                case '\'': ch = '\''; break;
                case '"':  ch = '"';  break;
                default:   ch = esc;  break;
            }
        } else {
            ch = advance();
        }
        if (peek() == '\'') advance();
        return {TOKEN_CHAR_LIT, std::string(1, ch), tokLine, tokCol};
    }

    // ── Operadores e pontuação ────────────────────────────────────────────────
    advance();
    switch (c) {
        case '+': return {TOKEN_PLUS,    "+", tokLine, tokCol};
        case '*': return {TOKEN_STAR,    "*", tokLine, tokCol};
        case '/': return {TOKEN_SLASH,   "/", tokLine, tokCol};
        case '%': return {TOKEN_PERCENT, "%", tokLine, tokCol};
        case '(': return {TOKEN_LPAREN,  "(", tokLine, tokCol};
        case ')': return {TOKEN_RPAREN,  ")", tokLine, tokCol};
        case '{': return {TOKEN_LBRACE,  "{", tokLine, tokCol};
        case '}': return {TOKEN_RBRACE,  "}", tokLine, tokCol};
        case '[': return {TOKEN_LBRACKET,"[", tokLine, tokCol};
        case ']': return {TOKEN_RBRACKET,"]", tokLine, tokCol};
        case ',': return {TOKEN_COMMA,   ",", tokLine, tokCol};
        case ';': return {TOKEN_SEMI,    ";", tokLine, tokCol};
        case '#': return {TOKEN_HASH,    "#", tokLine, tokCol};

        case '-':
            // -> (tipo de retorno)
            if (peek() == '>') { advance(); return {TOKEN_ARROW, "->", tokLine, tokCol}; }
            return {TOKEN_MINUS, "-", tokLine, tokCol};

        case ':':
            if (peek() == ':') { advance(); return {TOKEN_COLONCOLON, "::", tokLine, tokCol}; }
            return {TOKEN_COLON, ":", tokLine, tokCol};

        case '.':
            if (peek() == '.' && pos + 1 < source.size() && source[pos+1] == '.') {
                advance(); advance();
                return {TOKEN_ELLIPSIS, "...", tokLine, tokCol};
            }
            return {TOKEN_DOT, ".", tokLine, tokCol};

        case '=':
            if (peek() == '=') { advance(); return {TOKEN_EQ, "==", tokLine, tokCol}; }
            return {TOKEN_ASSIGN, "=", tokLine, tokCol};

        case '!':
            if (peek() == '=') { advance(); return {TOKEN_NEQ, "!=", tokLine, tokCol}; }
            return {TOKEN_NOT, "!", tokLine, tokCol};

        case '&':
            if (peek() == '&') { advance(); return {TOKEN_AND, "&&", tokLine, tokCol}; }
            // & sozinho é referência válida em Rust-style (&self, &mut)
            return {TOKEN_AMPERSAND, "&", tokLine, tokCol};

        case '|':
            if (peek() == '|') { advance(); return {TOKEN_OR, "||", tokLine, tokCol}; }
            {
                std::string srcLine = getSourceLine(tokLine);
                reportError("<source>", tokLine, tokCol,
                    "bitwise operator '|' is not supported — did you mean '||'?",
                    srcLine, 1,
                    "use '||' for logical OR");
            }
            break;

        case '<': {
            // Tenta ler <header.nh> — caminho de include
            size_t savePos2  = pos;
            int    saveLine2 = line;
            int    saveCol2  = col;
            std::string hdr;
            while (peek() != '>' && peek() != '\n' && pos < source.size())
                hdr += advance();
            if (peek() == '>' && !hdr.empty() &&
                (std::isalpha((unsigned char)hdr[0]) || hdr[0] == '_' || hdr[0] == '.')) {
                advance();
                return {TOKEN_HEADER_PATH, hdr, tokLine, tokCol};
            }
            pos  = savePos2;
            line = saveLine2;
            col  = saveCol2;
            if (peek() == '=') { advance(); return {TOKEN_LEQ, "<=", tokLine, tokCol}; }
            return {TOKEN_LT, "<", tokLine, tokCol};
        }

        case '>':
            if (peek() == '=') { advance(); return {TOKEN_GEQ, ">=", tokLine, tokCol}; }
            return {TOKEN_GT, ">", tokLine, tokCol};
    }

    return {TOKEN_UNKNOWN, std::string(1, c), tokLine, tokCol};
}

Token peekToken() {
    LexerState saved = saveLexerState();
    Token t = nextToken();
    restoreLexerState(saved);
    return t;
}