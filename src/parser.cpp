#include "ast.h"
#include <cstdlib>
#include "lexer.h"
#include "../include/error.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <set>
#include <map>
#include <vector>
#include <algorithm>

static Token current;
static std::string sourceFile;

static std::set<std::string> includedHeaders;
static std::map<std::string, int> arraySizes;

// Rastreia nomes declarados (para "did you mean?")
static std::set<std::string> declaredFunctionNames;
static std::set<std::string> declaredVarNames;

// Conjunto de structs declarados — usado para detectar mutabilidade
static std::set<std::string> declaredStructNames;

// ── Mutabilidade em escopo: variáveis imutáveis ────────────────────────────────
// Registro de quais variáveis locais são imutáveis
static std::set<std::string> immutableVars;
static std::set<std::string> mutableReferences;
static DataType currentReturnType = DataType::Void;

// ── Assinaturas de funções: para validação nos call sites ─────────────────────
// Mapeia nome da função → lista de tipos de parâmetros como string.
// Exemplos: "&mut i32", "&i32", "i32", "str", etc.
// Usado para checar: (a) aridade correta, (b) &mut obrigatório quando requerido.
struct FnParamInfo {
    std::string typeName; // ex: "&mut i32", "i32", "&str"
    bool isRefMut;        // true se o parâmetro é &mut T
    bool isRef;           // true se o parâmetro é &T (imutável)
};
static std::map<std::string, std::vector<FnParamInfo>> fnSignatures;
static std::set<std::string> variadicFunctions; // funções com parâmetro variádico (...)
static std::map<std::string, std::string> declaredVarStructType;

// Constrói FnParamInfo a partir de um ParamNode já parseado.
static FnParamInfo buildParamInfo(const ParamNode& p) {
    FnParamInfo info;
    info.typeName = p.structTypeName.empty() ? "" : p.structTypeName;
    if (info.typeName.empty()) {
        // tipo primitivo — não é referência
        info.isRefMut = false;
        info.isRef    = false;
        return info;
    }
    // Referência: structTypeName é algo como "&mut i32" ou "&i32"
    info.isRefMut = (info.typeName.size() >= 5 && info.typeName.substr(0, 5) == "&mut ");
    info.isRef    = (!info.isRefMut && !info.typeName.empty() && info.typeName[0] == '&');
    return info;
}

// ── Levenshtein ───────────────────────────────────────────────────────────────
static int editDistance(const std::string& a, const std::string& b) {
    int m = (int)a.size(), n = (int)b.size();
    std::vector<std::vector<int>> dp(m+1, std::vector<int>(n+1, 0));
    for (int i = 0; i <= m; i++) dp[i][0] = i;
    for (int j = 0; j <= n; j++) dp[0][j] = j;
    for (int i = 1; i <= m; i++)
        for (int j = 1; j <= n; j++)
            dp[i][j] = (a[i-1] == b[j-1])
                ? dp[i-1][j-1]
                : 1 + std::min({dp[i-1][j], dp[i][j-1], dp[i-1][j-1]});
    return dp[m][n];
}

static std::string didYouMean(const std::string& name,
                               const std::set<std::string>& candidates) {
    std::string best;
    int bestDist = 3;
    for (auto& c : candidates) {
        int d = editDistance(name, c);
        if (d < bestDist) { bestDist = d; best = c; }
    }
    if (!best.empty()) return "did you mean '" + best + "'?";
    return "";
}

// ── Tipo de dado: nome: Type ──────────────────────────────────────────────────
// Quando retorna DataType::Custom, o caller lê lastCustomTypeName.
static std::string lastCustomTypeName;

// ── Nome legível de token ─────────────────────────────────────────────────────
static std::string dataTypeToString(DataType t) {
    switch (t) {
        case DataType::Int:      return "i32";
        case DataType::Long:     return "i64";
        case DataType::LongLong: return "i64";
        case DataType::Float:    return "f32";
        case DataType::Double:   return "f64";
        case DataType::String:   return "str";
        case DataType::Char:     return "char";
        case DataType::Void:     return "void";
        case DataType::Bool:     return "bool";
        case DataType::Custom:   return lastCustomTypeName;
    }
    return "unknown";
}

static std::string tokenTypeName(TokenType t) {
    switch (t) {
        case TOKEN_SEMI:      return "';'";
        case TOKEN_LPAREN:    return "'('";
        case TOKEN_RPAREN:    return "')'";
        case TOKEN_LBRACE:    return "'{'";
        case TOKEN_RBRACE:    return "'}'";
        case TOKEN_LBRACKET:  return "'['";
        case TOKEN_RBRACKET:  return "']'";
        case TOKEN_ASSIGN:    return "'='";
        case TOKEN_COMMA:     return "','";
        case TOKEN_COLON:     return "':'";
        case TOKEN_ARROW:     return "'->'";
        case TOKEN_AS:        return "'as'";
        case TOKEN_IDENT:     return "identifier";
        case TOKEN_INT_LIT:   return "integer literal";
        case TOKEN_FLOAT_LIT: return "float literal";
        case TOKEN_STRING_LIT:return "string literal";
        case TOKEN_CHAR_LIT:  return "char literal";
        case TOKEN_INT:       return "'i32'";
        case TOKEN_FLOAT:     return "'f32'";
        case TOKEN_STRING:    return "'str'";
        case TOKEN_CHAR:      return "'char'";
        case TOKEN_VOID:      return "'void'";
        case TOKEN_LONG:      return "'i64'";
        case TOKEN_DOUBLE:    return "'f64'";
        case TOKEN_BOOL:      return "'bool'";
        case TOKEN_FN:        return "'fn'";
        case TOKEN_LET:       return "'let'";
        case TOKEN_MUT:       return "'mut'";
        case TOKEN_IMPL:      return "'impl'";
        case TOKEN_IF:        return "'if'";
        case TOKEN_ELSE:      return "'else'";
        case TOKEN_WHILE:     return "'while'";
        case TOKEN_FOR:       return "'for'";
        case TOKEN_RETURN:    return "'return'";
        case TOKEN_STRUCT:    return "'struct'";
        case TOKEN_EOF:       return "end of file";
        default:              return "unknown token";
    }
}

// ── eat: consome token esperado ou emite erro detalhado ───────────────────────
static Token eat(TokenType expected) {
    if (current.type != expected) {
        std::string ln = getSourceLine(current.line);
        int tlen = std::max(1, (int)current.value.size());

        if (expected == TOKEN_SEMI) {
            reportError(sourceFile, current.line, current.col,
                        "missing ';' at end of statement",
                        ln, tlen,
                        "add ';' before '" + current.value + "'");
        } else if (expected == TOKEN_COLON) {
            reportError(sourceFile, current.line, current.col,
                        "expected ':' after variable name in declaration",
                        ln, tlen,
                        "syntax: let name: Type = value;");
        } else if (expected == TOKEN_LBRACE) {
            reportError(sourceFile, current.line, current.col,
                        "expected '{' to open block, but found '" + current.value + "'",
                        ln, tlen,
                        "every if/for/while/fn body must be wrapped in { }");
        } else if (expected == TOKEN_RBRACE) {
            reportError(sourceFile, current.line, current.col,
                        "expected '}' to close block, but found '" + current.value + "'",
                        ln, tlen,
                        "check for a missing '}' above this line");
        } else if (expected == TOKEN_RPAREN) {
            reportError(sourceFile, current.line, current.col,
                        "expected ')' to close expression, but found '" + current.value + "'",
                        ln, tlen,
                        "check for an unmatched '(' earlier");
        } else if (expected == TOKEN_LPAREN) {
            reportError(sourceFile, current.line, current.col,
                        "expected '(' here, but found '" + current.value + "'",
                        ln, tlen);
        } else if (expected == TOKEN_ASSIGN) {
            reportError(sourceFile, current.line, current.col,
                        "expected '=' in assignment, but found '" + current.value + "'",
                        ln, tlen);
        } else if (expected == TOKEN_ARROW) {
            reportError(sourceFile, current.line, current.col,
                        "expected '->' for return type annotation, but found '" + current.value + "'",
                        ln, tlen,
                        "syntax: fn name(params) -> RetType { ... }");
        } else if (expected == TOKEN_IDENT) {
            if (current.type == TOKEN_INT    || current.type == TOKEN_FLOAT  ||
                current.type == TOKEN_STRING || current.type == TOKEN_VOID   ||
                current.type == TOKEN_DOUBLE || current.type == TOKEN_LONG   ||
                current.type == TOKEN_CHAR   || current.type == TOKEN_BOOL) {
                reportError(sourceFile, current.line, current.col,
                            "'" + current.value + "' is a reserved type keyword and cannot be used as a name",
                            ln, tlen,
                            "choose a different name for your variable or function");
            } else if (current.type == TOKEN_FN   || current.type == TOKEN_LET ||
                       current.type == TOKEN_MUT   || current.type == TOKEN_IMPL||
                       current.type == TOKEN_STRUCT) {
                reportError(sourceFile, current.line, current.col,
                            "'" + current.value + "' is a reserved keyword and cannot be used as a name",
                            ln, tlen,
                            "choose a different name");
            } else {
                reportError(sourceFile, current.line, current.col,
                            "expected a name (identifier), but found '" + current.value + "'",
                            ln, tlen);
            }
        } else if (expected == TOKEN_RBRACKET) {
            reportError(sourceFile, current.line, current.col,
                        "expected ']' to close array index, but found '" + current.value + "'",
                        ln, tlen,
                        "check for a missing ']' in the array access");
        } else {
            reportError(sourceFile, current.line, current.col,
                        "expected " + tokenTypeName(expected) +
                        ", but found '" + current.value + "'",
                        ln, tlen);
        }
    }
    Token t = current;
    current = nextToken();
    return t;
}

// ── Verificação de mutabilidade ───────────────────────────────────────────────
static void checkMutable(const std::string& varName, int line, int col) {
    if (immutableVars.count(varName)) {
        std::string ln = getSourceLine(line);
        reportError(sourceFile, line, col,
                    "cannot assign to immutable variable '" + varName + "'",
                    ln, (int)varName.size(),
                    "declare with 'let mut " + varName + ": ...' to allow mutation");
    }
}

static DataType parseDataType() {
    // &T ou &mut T — tipo de referência/ponteiro
    if (current.type == TOKEN_AMPERSAND) {
        current = nextToken();
        bool isMut = false;
        if (current.type == TOKEN_MUT) {
            isMut = true;
            current = nextToken();
        }
        DataType base = parseDataType();
        std::string baseName = dataTypeToString(base);
        
        lastCustomTypeName = (isMut ? "&mut " : "&") + baseName;
        return DataType::Custom;
    }
    if (current.type == TOKEN_INT)    { current = nextToken(); return DataType::Int; }
    if (current.type == TOKEN_FLOAT)  { current = nextToken(); return DataType::Float; }
    if (current.type == TOKEN_STRING) { current = nextToken(); return DataType::String; }
    if (current.type == TOKEN_CHAR)   { current = nextToken(); return DataType::Char; }
    if (current.type == TOKEN_VOID)   { current = nextToken(); return DataType::Void; }
    if (current.type == TOKEN_DOUBLE) { current = nextToken(); return DataType::Double; }
    if (current.type == TOKEN_BOOL)   { current = nextToken(); return DataType::Bool; }
    if (current.type == TOKEN_LONG) {
        current = nextToken();
        if (current.type == TOKEN_LONG) { current = nextToken(); return DataType::LongLong; }
        return DataType::Long;
    }
    // Tipo struct ou qualificado: Point  |  geo::Point
    if (current.type == TOKEN_IDENT) {
        lastCustomTypeName = current.value;
        current = nextToken();
        if (current.type == TOKEN_COLONCOLON) {
            current = nextToken();
            std::string structName = eat(TOKEN_IDENT).value;
            lastCustomTypeName = lastCustomTypeName + "::" + structName;
        }
        return DataType::Custom;
    }

    // Mensagem de erro rica
    std::string ln = getSourceLine(current.line);
    static const std::set<std::string> types =
        {"i32","i64","f32","f64","str","bool","char","void","int","float","double","long","string"};
    std::string hint = didYouMean(current.value, types);
    if (hint.empty())
        hint = "valid types: i32, i64, f32, f64, str, char, bool, void, or a struct name";
    reportError(sourceFile, current.line, current.col,
                "'" + current.value + "' is not a valid type",
                ln, std::max(1,(int)current.value.size()), hint);
    return DataType::Int;
}

// ── Conversão segura ──────────────────────────────────────────────────────────
static long long safeStoll(const std::string& val, int ln, int co) {
    try { return std::stoll(val); }
    catch (const std::out_of_range&) {
        reportError(sourceFile, ln, co,
                    "integer literal '" + val + "' overflows — value is too large",
                    getSourceLine(ln), (int)val.size(),
                    "use 'i64' for large values — max i32 is 2147483647");
    } catch (const std::invalid_argument&) {
        reportError(sourceFile, ln, co,
                    "malformed integer literal '" + val + "'",
                    getSourceLine(ln), (int)val.size());
    }
    return 0;
}

static int safeStoi(const std::string& val, int ln, int co) {
    try { return std::stoi(val); }
    catch (const std::out_of_range&) {
        reportError(sourceFile, ln, co,
                    "array size '" + val + "' is too large",
                    getSourceLine(ln), (int)val.size(),
                    "array sizes must fit in a 32-bit integer");
    } catch (const std::invalid_argument&) {
        reportError(sourceFile, ln, co,
                    "malformed array size literal '" + val + "'",
                    getSourceLine(ln), (int)val.size());
    }
    return 0;
}

// ── Forward declarations ──────────────────────────────────────────────────────
static NodePtr parseStatement();
static std::vector<NodePtr> parseBlock();
static NodePtr parseExpr();
static NodePtr parseAddSub();

// ═══════════════════════════════════════════════════════════════════════════════
// PARSING DE EXPRESSÕES
// ═══════════════════════════════════════════════════════════════════════════════

// ── Validação de call site ────────────────────────────────────────────────────
// Chama após parsear os args de uma chamada de função.
// Verifica: (1) aridade, (2) para cada param &mut T, o arg deve ser &mut expr.
// Para param &T, o arg pode ser &expr ou &mut expr — mas não um valor.
static void checkCallSite(const std::string& fnName,
                          const std::vector<NodePtr>& args,
                          int callLine, int callCol) {
    auto it = fnSignatures.find(fnName);
    if (it == fnSignatures.end()) return; // função desconhecida (externa etc.)

    const auto& sig = it->second;
    // Aridade — funções variádicas aceitam qualquer número de args >= params fixos
    bool isVariadic = variadicFunctions.count(fnName) > 0;
    if (!isVariadic && args.size() != sig.size()) {
        std::string ln = getSourceLine(callLine);
        std::string note = "function '" + fnName + "' expects " +
                           std::to_string(sig.size()) + " argument" +
                           (sig.size() == 1 ? "" : "s") +
                           ", but " + std::to_string(args.size()) + " " +
                           (args.size() == 1 ? "was" : "were") + " provided";
        std::string hint;
        if (sig.size() > 0) {
            hint = "signature: " + fnName + "(";
            for (size_t i = 0; i < sig.size(); i++) {
                if (i > 0) hint += ", ";
                hint += sig[i].typeName.empty() ? "T" : sig[i].typeName;
            }
            hint += ")";
        }
        reportError(sourceFile, callLine, callCol, note, ln,
                    std::max(1, (int)fnName.size()), hint);
    }

    // Verificação de &mut / & por parâmetro
    size_t n = std::min(args.size(), sig.size());
    for (size_t i = 0; i < n; i++) {
        const FnParamInfo& pi = sig[i];
        if (!pi.isRefMut && !pi.isRef) continue; // parâmetro por valor — sem restrição aqui

        const ASTNode* arg = args[i].get();
        // Determina se o argumento passado é &mut expr, &expr, ou valor puro
        const UnaryOpNode* unary = dynamic_cast<const UnaryOpNode*>(arg);
        bool argIsRefMut = unary && unary->op == "&mut";
        bool argIsRef    = unary && unary->op == "&";
        bool argIsValue  = !argIsRefMut && !argIsRef;

        std::string ln = getSourceLine(callLine);

        if (pi.isRefMut) {
            // O parâmetro exige &mut — o argumento DEVE ser &mut expr
            if (argIsRef) {
                // Passou &x quando precisa de &mut x
                std::string varName;
                if (auto* vn = dynamic_cast<const VarNode*>(unary->operand.get()))
                    varName = vn->name;
                std::string note = "argument " + std::to_string(i+1) + " of '" + fnName +
                                   "' requires a mutable reference '&mut', but '&" +
                                   varName + "' is an immutable reference";
                std::string hint = "change to: " + fnName + "(..., &mut " + varName + ", ...)";
                reportError(sourceFile, callLine, callCol, note, ln,
                            std::max(1,(int)fnName.size()), hint);
            } else if (argIsValue) {
                // Passou x (ou expr) quando precisa de &mut x
                std::string varName;
                if (auto* vn = dynamic_cast<const VarNode*>(arg)) varName = vn->name;
                std::string note = "argument " + std::to_string(i+1) + " of '" + fnName +
                                   "' requires a mutable reference '&mut " +
                                   (varName.empty() ? "expr" : varName) + "'";
                std::string hint = varName.empty()
                    ? "wrap the expression: &mut <variable>"
                    : "change to: " + fnName + "(..., &mut " + varName + ", ...)";
                reportError(sourceFile, callLine, callCol, note, ln,
                            std::max(1,(int)fnName.size()), hint);
            }
            // argIsRefMut → OK
        } else if (pi.isRef) {
            // O parâmetro exige & — aceita &expr ou &mut expr, recusa valor puro
            if (argIsValue) {
                std::string varName;
                if (auto* vn = dynamic_cast<const VarNode*>(arg)) varName = vn->name;
                std::string note = "argument " + std::to_string(i+1) + " of '" + fnName +
                                   "' requires a reference '&" +
                                   (varName.empty() ? "expr" : varName) + "'";
                std::string hint = varName.empty()
                    ? "wrap the expression: &<variable>"
                    : "change to: " + fnName + "(..., &" + varName + ", ...)";
                reportError(sourceFile, callLine, callCol, note, ln,
                            std::max(1,(int)fnName.size()), hint);
            }
        }
    }
}

static NodePtr parsePrimary() {
    if (current.type == TOKEN_AMPERSAND) {
        int tl = current.line, tc = current.col;
        current = nextToken();
        std::string op = "&";
        bool isMutBorrow = false;
        if (current.type == TOKEN_MUT) {
            op = "&mut";
            isMutBorrow = true;
            current = nextToken();
        }
        auto operand = parsePrimary();
        if (isMutBorrow) {
            std::string rootVar;
            if (auto* vn = dynamic_cast<VarNode*>(operand.get())) rootVar = vn->name;
            else if (auto* an = dynamic_cast<ArrayAccessNode*>(operand.get())) rootVar = an->name;
            else if (auto* fan = dynamic_cast<FieldAccessNode*>(operand.get())) {
                size_t dot = fan->varName.find('.');
                rootVar = (dot == std::string::npos) ? fan->varName : fan->varName.substr(0, dot);
            } else if (auto* dn = dynamic_cast<DerefNode*>(operand.get())) {
                if (auto* vn = dynamic_cast<VarNode*>(dn->operand.get())) rootVar = vn->name;
            }

            if (!rootVar.empty() && immutableVars.count(rootVar) && !mutableReferences.count(rootVar)) {
                reportError(sourceFile, tl, tc,
                            "cannot borrow immutable variable '" + rootVar + "' as mutable",
                            getSourceLine(tl), (int)op.size(),
                            "declare with 'let mut' to allow mutable references");
            }
        }
        return std::make_unique<UnaryOpNode>(op, std::move(operand), tl, tc);
    }
    // ──────────────────────────────────────────────

    // Operador unário ! (este já existe no seu código)
    if (current.type == TOKEN_NOT) {
        int tl = current.line, tc = current.col;
        current = nextToken();
        return std::make_unique<UnaryOpNode>("!", parsePrimary(), tl, tc);
    }
    if (current.type == TOKEN_STAR) {
        int l = current.line, c = current.col;
        current = nextToken(); // Consome o '*'
        // Chama parseUnaryExpression de novo para permitir algo como **ptr
        return std::make_unique<DerefNode>(parsePrimary(), DataType::Int, l, c);
    }
    // Negação unária -
    if (current.type == TOKEN_MINUS) {
        int tl = current.line, tc = current.col;
        current = nextToken();
        auto operand = parsePrimary();
        return std::make_unique<BinaryOpNode>("-",
            std::make_unique<IntLitNode>(0), std::move(operand), tl, tc);
    }
    // true / false
    if (current.type == TOKEN_TRUE) {
        current = nextToken();
        return std::make_unique<BoolLitNode>(true);
    }
    if (current.type == TOKEN_FALSE) {
        current = nextToken();
        return std::make_unique<BoolLitNode>(false);
    }
    // Inteiro literal
    if (current.type == TOKEN_INT_LIT) {
        int tl = current.line, tc = current.col;
        long long v = safeStoll(current.value, tl, tc);
        current = nextToken();
        if (v > 2147483647LL || v < -2147483648LL)
            return std::make_unique<LongLitNode>(v);
        return std::make_unique<IntLitNode>((int)v);
    }
    if (current.type == TOKEN_FLOAT_LIT) {
        std::string raw = current.value; current = nextToken();
        return std::make_unique<FloatLitNode>(raw);
    }
    if (current.type == TOKEN_STRING_LIT) {
        std::string v = current.value; current = nextToken();
        return std::make_unique<StringLitNode>(v);
    }
    if (current.type == TOKEN_CHAR_LIT) {
        char v = current.value.empty() ? '\0' : current.value[0];
        current = nextToken();
        return std::make_unique<CharLitNode>(v);
    }

    // Agrupamento: (expr)
    if (current.type == TOKEN_LPAREN) {
        current = nextToken();
        auto e = parseExpr();
        eat(TOKEN_RPAREN);
        return e;
    }

    // Identificador: var, func(), arr[i], obj.field, ns::func()
    // TOKEN_SELF: 'self' dentro de métodos impl — tratado como identificador normal
    if (current.type == TOKEN_IDENT || current.type == TOKEN_SELF) {
        std::string name = current.value;
        int tokLine = current.line, tokCol = current.col;
        current = nextToken();

        // Namespace qualificado: ns::member
        if (current.type == TOKEN_COLONCOLON) {
            current = nextToken();
            std::string memberName = eat(TOKEN_IDENT).value;
            std::string qualName = name + "::" + memberName;
            if (current.type == TOKEN_LPAREN) {
                current = nextToken();
                std::vector<NodePtr> args;
                while (current.type != TOKEN_RPAREN) {
                    if (current.type == TOKEN_EOF)
                        reportError(sourceFile, tokLine, tokCol,
                            "unterminated argument list for '" + qualName + "()'",
                            getSourceLine(tokLine), (int)qualName.size(), "add a closing ')'");
                    args.push_back(parseExpr());
                    if (current.type == TOKEN_COMMA) current = nextToken();
                }
                eat(TOKEN_RPAREN);
                checkCallSite(qualName, args, tokLine, tokCol);
                return std::make_unique<CallNode>(qualName, std::move(args), tokLine, tokCol);
            }
            return std::make_unique<VarNode>(qualName, tokLine, tokCol);
        }

        // Chamada de função: foo(...)
        if (current.type == TOKEN_LPAREN) {
            current = nextToken();
            std::vector<NodePtr> args;
            while (current.type != TOKEN_RPAREN) {
                if (current.type == TOKEN_EOF)
                    reportError(sourceFile, tokLine, tokCol,
                        "unterminated argument list for '" + name + "()'",
                        getSourceLine(tokLine), (int)name.size(), "add a closing ')'");
                args.push_back(parseExpr());
                if (current.type == TOKEN_COMMA) current = nextToken();
            }
            eat(TOKEN_RPAREN);
            checkCallSite(name, args, tokLine, tokCol);
            return std::make_unique<CallNode>(name, std::move(args), tokLine, tokCol);
        }

        // Índice de array: arr[i]
        if (current.type == TOKEN_LBRACKET) {
            current = nextToken();
            int idxLine = current.line, idxCol = current.col;
            auto index = parseExpr();
            eat(TOKEN_RBRACKET);
            if (auto* lit = dynamic_cast<IntLitNode*>(index.get())) {
                auto it = arraySizes.find(name);
                if (it != arraySizes.end()) {
                    int idx = lit->value;
                    if (idx < 0)
                        reportError(sourceFile, idxLine, idxCol,
                            "negative array index " + std::to_string(idx) + " for '" + name + "'",
                            getSourceLine(idxLine), (int)std::to_string(idx).size(),
                            "array indices start at 0");
                    if (idx >= it->second)
                        reportError(sourceFile, idxLine, idxCol,
                            "index " + std::to_string(idx) + " is out of bounds for array '"
                            + name + "' (size " + std::to_string(it->second) + ")",
                            getSourceLine(idxLine), (int)std::to_string(idx).size(),
                            "valid indices are 0 to " + std::to_string(it->second - 1));
                }
            }
            return std::make_unique<ArrayAccessNode>(name, std::move(index), tokLine, tokCol);
        }

        // Acesso de campo / chamada de método: obj.field  obj.method(...)
        if (current.type == TOKEN_DOT) {
            current = nextToken();
            std::string field = eat(TOKEN_IDENT).value;
            // Acesso encadeado: a.b.c
            std::string composedName = name;
            std::string composedField = field;
            while (current.type == TOKEN_DOT) {
                composedName = composedName + "." + composedField;
                current = nextToken();
                composedField = eat(TOKEN_IDENT).value;
            }
            // Chamada de método: obj.method(...)
            if (current.type == TOKEN_LPAREN) {
                current = nextToken();
                std::vector<NodePtr> args;
                while (current.type != TOKEN_RPAREN) {
                    if (current.type == TOKEN_EOF)
                        reportError(sourceFile, tokLine, tokCol,
                            "unterminated argument list for '" + composedName + "." + composedField + "()'",
                            getSourceLine(tokLine), (int)composedField.size(), "add ')'");
                    args.push_back(parseExpr());
                    if (current.type == TOKEN_COMMA) current = nextToken();
                }
                eat(TOKEN_RPAREN);
                // Resolve o tipo do objeto para validar assinatura do método
                {
                    auto typeIt = declaredVarStructType.find(composedName);
                    if (typeIt != declaredVarStructType.end())
                        checkCallSite(typeIt->second + "::" + composedField, args, tokLine, tokCol);
                }
                return std::make_unique<MethodCallNode>(composedName, composedField,
                                                        std::move(args), tokLine, tokCol);
            }
            return std::make_unique<FieldAccessNode>(composedName, composedField, tokLine, tokCol);
        }

        // Variável simples
        return std::make_unique<VarNode>(name, tokLine, tokCol);
    }

    // Erro: expressão inválida
    {
        std::string ln = getSourceLine(current.line);
        int tl = current.line, tc = current.col;
        std::string hint = didYouMean(current.value, declaredVarNames);
        if (hint.empty()) hint = didYouMean(current.value, declaredFunctionNames);
        if (hint.empty()) hint = "expected a value: literal, variable name, or function call";
        reportError(sourceFile, tl, tc,
                    "unexpected '" + current.value + "' in expression",
                    ln, std::max(1,(int)current.value.size()), hint);
    }
    return nullptr;
}

// ── Multiplicação / divisão / módulo ─────────────────────────────────────────
static NodePtr parseMulDiv() {
    auto left = parsePrimary();
    while (current.type == TOKEN_STAR  ||
           current.type == TOKEN_SLASH ||
           current.type == TOKEN_PERCENT) {
        std::string op = current.value;
        int tl = current.line, tc = current.col;
        current = nextToken();
        left = std::make_unique<BinaryOpNode>(op, std::move(left), parsePrimary(), tl, tc);
    }
    return left;
}

// ── Cast: expr as Type ────────────────────────────────────────────────────────
// Tem precedência maior que aritmética (4 + x as f64 == 4 + (x as f64))
static NodePtr parseCast() {
    auto left = parseMulDiv();
    while (current.type == TOKEN_AS) {
        int tl = current.line, tc = current.col;
        current = nextToken(); // consome 'as'
        // Deve vir um tipo primitivo — structs não são castáveis
        DataType targetType;
        if      (current.type == TOKEN_INT)    { targetType = DataType::Int;    current = nextToken(); }
        else if (current.type == TOKEN_LONG)   { targetType = DataType::Long;   current = nextToken(); }
        else if (current.type == TOKEN_FLOAT)  { targetType = DataType::Float;  current = nextToken(); }
        else if (current.type == TOKEN_DOUBLE) { targetType = DataType::Double; current = nextToken(); }
        else if (current.type == TOKEN_CHAR)   { targetType = DataType::Char;   current = nextToken(); }
        else if (current.type == TOKEN_BOOL)   { targetType = DataType::Int;    current = nextToken(); } // bool → i32
        else {
            std::string ln = getSourceLine(current.line);
            reportError(sourceFile, current.line, current.col,
                "'as' must be followed by a primitive type",
                ln, std::max(1,(int)current.value.size()),
                "valid cast targets: i32, i64, f32, f64, char\n"
                "    example: x as f64,  value as i32");
            return left;
        }
        left = std::make_unique<CastNode>(targetType, std::move(left), tl, tc);
    }
    return left;
}

// ── Adição / subtração ────────────────────────────────────────────────────────
static NodePtr parseAddSub() {
    auto left = parseCast();
    while (current.type == TOKEN_PLUS || current.type == TOKEN_MINUS) {
        std::string op = current.value;
        int tl = current.line, tc = current.col;
        current = nextToken();
        left = std::make_unique<BinaryOpNode>(op, std::move(left), parseCast(), tl, tc);
    }
    return left;
}

// ── Comparação ────────────────────────────────────────────────────────────────
static NodePtr parseComparison() {
    auto left = parseAddSub();
    while (current.type == TOKEN_LT  || current.type == TOKEN_GT  ||
           current.type == TOKEN_LEQ || current.type == TOKEN_GEQ ||
           current.type == TOKEN_EQ  || current.type == TOKEN_NEQ) {
        std::string op = current.value;
        int tl = current.line, tc = current.col;
        current = nextToken();
        left = std::make_unique<BinaryOpNode>(op, std::move(left), parseAddSub(), tl, tc);
    }
    return left;
}

// ── Lógica &&  || ─────────────────────────────────────────────────────────────
static NodePtr parseExpr() {
    auto left = parseComparison();
    while (current.type == TOKEN_AND || current.type == TOKEN_OR) {
        std::string op = current.value;
        int tl = current.line, tc = current.col;
        current = nextToken();
        left = std::make_unique<BinaryOpNode>(op, std::move(left), parseComparison(), tl, tc);
    }
    return left;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PARSING DE STATEMENTS
// ═══════════════════════════════════════════════════════════════════════════════

static std::vector<NodePtr> parseBlock() {
    eat(TOKEN_LBRACE);
    std::vector<NodePtr> stmts;
    while (current.type != TOKEN_RBRACE && current.type != TOKEN_EOF)
        stmts.push_back(parseStatement());
    if (current.type == TOKEN_EOF)
        reportError(sourceFile, current.line, current.col,
                    "unexpected end of file — missing '}' to close block",
                    getSourceLine(current.line), 1,
                    "add '}' to close the open block");
    eat(TOKEN_RBRACE);
    return stmts;
}

// ── `let [mut] name: Type [= expr];` ─────────────────────────────────────────
static NodePtr parseLetDecl(int tokLine, int tokCol) {
    bool isMut = false;
    if (current.type == TOKEN_MUT) {
        isMut = true;
        current = nextToken();
    }

    std::string name = eat(TOKEN_IDENT).value;
    eat(TOKEN_COLON);
    DataType type = parseDataType();
    std::string customTypeName = (type == DataType::Custom) ? lastCustomTypeName : "";

    // Verificação de array: let [mut] arr: [i32; N]
    // Sintaxe alternativa suportada: `let arr: TypeName[N]` (usando colchete direto após o tipo)
    if (current.type == TOKEN_LBRACKET) {
        current = nextToken();
        if (current.type != TOKEN_INT_LIT)
            reportError(sourceFile, current.line, current.col,
                "array size must be a constant integer",
                getSourceLine(current.line), std::max(1,(int)current.value.size()),
                "example: let " + name + ": i32[10];");
        int sizeLine = current.line, sizeCol = current.col;
        int size = safeStoi(eat(TOKEN_INT_LIT).value, sizeLine, sizeCol);
        if (size <= 0)
            reportError(sourceFile, sizeLine, sizeCol,
                "array size must be positive", getSourceLine(sizeLine), 1);
        eat(TOKEN_RBRACKET);
        std::vector<NodePtr> init;
        if (current.type == TOKEN_ASSIGN) {
            current = nextToken();
            eat(TOKEN_LBRACE);
            while (current.type != TOKEN_RBRACE) {
                if (current.type == TOKEN_EOF)
                    reportError(sourceFile, tokLine, tokCol,
                        "unterminated initializer for array '" + name + "'",
                        getSourceLine(tokLine), (int)name.size(), "add '}'");
                init.push_back(parseExpr());
                if (current.type == TOKEN_COMMA) current = nextToken();
            }
            eat(TOKEN_RBRACE);
        }
        eat(TOKEN_SEMI);
        arraySizes[name] = size;
        if (!isMut) immutableVars.insert(name);
        declaredVarNames.insert(name);
        if (type == DataType::Custom)
            return std::make_unique<ArrayDeclNode>(type, customTypeName, name, size,
                                                   std::move(init), tokLine, tokCol, isMut);
        return std::make_unique<ArrayDeclNode>(type, name, size,
                                               std::move(init), tokLine, tokCol, isMut);
    }

    // Struct var: let [mut] p: Point = ...;
    if (type == DataType::Custom) {
        if (!isMut) immutableVars.insert(name);
        if (customTypeName.substr(0, 5) == "&mut ") {
            mutableReferences.insert(name);
        }
        declaredVarNames.insert(name);
        if (current.type == TOKEN_ASSIGN) {
            current = nextToken();
            // Inicialização literal: let mut p: Point = { x: 1, y: 2 }
            if (current.type == TOKEN_LBRACE) {
                current = nextToken();
                std::vector<NodePtr> args;
                std::string fieldNames; // "x,y,z" — codegen vai usar pra mapear
                while (current.type != TOKEN_RBRACE) {
                    if (current.type == TOKEN_EOF)
                        reportError(sourceFile, tokLine, tokCol,
                            "unterminated struct initializer for '" + name + "'",
                            getSourceLine(tokLine), (int)name.size(), "add '}'");
                    std::string fieldName = eat(TOKEN_IDENT).value;
                    eat(TOKEN_COLON);
                    args.push_back(parseExpr());
                    if (!fieldNames.empty()) fieldNames += ",";
                    fieldNames += fieldName;
                    if (current.type == TOKEN_COMMA) current = nextToken();
                }
                eat(TOKEN_RBRACE);
                eat(TOKEN_SEMI);
                declaredVarStructType[name] = customTypeName;
                return std::make_unique<StructVarDeclNode>(
                    customTypeName, name, "@literal:" + fieldNames,
                    std::move(args), isMut, tokLine, tokCol);
            }
            if (current.type == TOKEN_IDENT) {
                std::string callName = current.value;
                current = nextToken();
                if (current.type == TOKEN_LPAREN) {
                    current = nextToken();
                    std::vector<NodePtr> args;
                    while (current.type != TOKEN_RPAREN) {
                        if (current.type == TOKEN_EOF)
                            reportError(sourceFile, tokLine, tokCol,
                                "unterminated argument list for '" + callName + "()'",
                                getSourceLine(tokLine), (int)callName.size(), "add ')'");
                        args.push_back(parseExpr());
                        if (current.type == TOKEN_COMMA) current = nextToken();
                    }
                    eat(TOKEN_RPAREN);
                    eat(TOKEN_SEMI);
                    declaredVarStructType[name] = customTypeName;
                    return std::make_unique<StructVarDeclNode>(
                        customTypeName, name, callName, std::move(args), isMut, tokLine, tokCol);
                }
                eat(TOKEN_SEMI);
                declaredVarStructType[name] = customTypeName;
                return std::make_unique<StructVarDeclNode>(
                    customTypeName, name, "@copy:" + callName,
                    std::vector<NodePtr>{}, isMut, tokLine, tokCol);
            }
            reportError(sourceFile, current.line, current.col,
                "struct variable '" + name + "' must be initialized with a function call, another variable, or a literal { field: value, ... }",
                getSourceLine(current.line), std::max(1,(int)current.value.size()),
                "example: let p: Point = { x: 1, y: 2 };");
        }
        eat(TOKEN_SEMI);
        declaredVarStructType[name] = customTypeName;
        return std::make_unique<StructVarDeclNode>(customTypeName, name, isMut, tokLine, tokCol);
    }

    // Variável primitiva
    NodePtr init = nullptr;
    if (current.type == TOKEN_ASSIGN) {
        current = nextToken();
        init = parseExpr();
    } else {
        // Sem inicializador — erro rigoroso como Rust
        reportError(sourceFile, current.line, current.col,
                    "variable '" + name + "' must be initialized",
                    getSourceLine(current.line), std::max(1,(int)current.value.size()),
                    "add '= value' before the semicolon, or use 'let mut " +
                    name + ": " + (type == DataType::Int ? "i32" : "Type") + " = 0;'");
    }
    eat(TOKEN_SEMI);
    if (!isMut) immutableVars.insert(name);
    declaredVarNames.insert(name);
    // Se já existia algum registro antigo de struct (por exemplo vindo de um
    // #include) com o mesmo nome, uma declaração local primitiva deve
    // sobrescrevê-lo para evitar que o parser trate `x = func(...)` como
    // StructAssignNode indevidamente.
    if (type != DataType::Custom)
        declaredVarStructType.erase(name);
    return std::make_unique<VarDeclNode>(type, name, std::move(init), isMut, tokLine, tokCol);
}

// ── Parsing de statement ──────────────────────────────────────────────────────
static NodePtr parseStatement() {
    int tokLine = current.line, tokCol = current.col;

    // ── *ptr = value; ─────────────────────────────────────────────────────────
    if (current.type == TOKEN_STAR) {
        int dl = current.line, dc = current.col;
        current = nextToken(); // consome '*'

        auto operand = parsePrimary();
        
        // Check if dereference mutation is allowed (only for &mut references)
        if (auto* vn = dynamic_cast<VarNode*>(operand.get())) {
            if (!mutableReferences.count(vn->name)) {
                reportError(sourceFile, dl, dc,
                            "cannot assign to value through immutable reference '" + vn->name + "'",
                            getSourceLine(dl), 1,
                            "the reference must be '&mut' to allow mutation through it");
            }
        }

        auto target = std::make_unique<DerefNode>(std::move(operand), DataType::Int, dl, dc);

        if (current.type != TOKEN_ASSIGN)
            reportError(sourceFile, current.line, current.col,
                        "expected '=' after dereference target",
                        getSourceLine(current.line), std::max(1, (int)current.value.size()),
                        "syntax: *ptr = value;");
        eat(TOKEN_ASSIGN);
        auto value = parseExpr();
        eat(TOKEN_SEMI);
        return std::make_unique<DerefAssignNode>(
            std::move(target), std::move(value), DataType::Int, tokLine, tokCol);
    }

    // ── return ────────────────────────────────────────────────────────────────
    if (current.type == TOKEN_RETURN) {
        int rl = current.line, rc = current.col;
        current = nextToken();
        if (current.type == TOKEN_SEMI) {
            current = nextToken();
            if (currentReturnType != DataType::Void) {
                reportError(sourceFile, rl, rc,
                    "missing return value in function expected to return '" + dataTypeToString(currentReturnType) + "'",
                    getSourceLine(rl), 6, "return an expression of the correct type");
            }
            return std::make_unique<ReturnNode>(nullptr);
        }
        auto e = parseExpr();
        if (currentReturnType == DataType::Void) {
            reportError(sourceFile, rl, rc,
                "function with no return type cannot return a value",
                getSourceLine(rl), 6, "remove the return value or add '-> Type' to the function signature");
        }
        eat(TOKEN_SEMI);
        return std::make_unique<ReturnNode>(std::move(e));
    }

    // ── let ───────────────────────────────────────────────────────────────────
    if (current.type == TOKEN_LET) {
        current = nextToken();
        return parseLetDecl(tokLine, tokCol);
    }

    // ── if ────────────────────────────────────────────────────────────────────
    // Sintaxe: if cond { } [else { }]
    // Parênteses opcionais ao redor da condição (mantidos para compatibilidade)
    if (current.type == TOKEN_IF) {
        current = nextToken();
        bool hasParen = (current.type == TOKEN_LPAREN);
        if (hasParen) current = nextToken();
        auto cond = parseExpr();
        if (hasParen) eat(TOKEN_RPAREN);
        // Erro explícito se vier `then` (sintaxe antiga)
        if (current.type == TOKEN_THEN) {
            std::string ln = getSourceLine(current.line);
            reportError(sourceFile, current.line, current.col,
                        "unexpected 'then'",
                        ln, 4,
                        "use: if condition { ... }");
        }
        auto thenBlock = parseBlock();
        std::vector<NodePtr> elseBlock;
        if (current.type == TOKEN_ELSE) {
            current = nextToken();
            if (current.type == TOKEN_IF) {
                // else if encadeado
                elseBlock.push_back(parseStatement());
            } else {
                elseBlock = parseBlock();
            }
        }
        return std::make_unique<IfNode>(std::move(cond), std::move(thenBlock), std::move(elseBlock));
    }

    // ── while ─────────────────────────────────────────────────────────────────
    // Sintaxe: while cond { }
    if (current.type == TOKEN_WHILE) {
        current = nextToken();
        if (current.type != TOKEN_LPAREN && current.type != TOKEN_IDENT &&
            current.type != TOKEN_INT_LIT && current.type != TOKEN_NOT &&
            current.type != TOKEN_TRUE && current.type != TOKEN_FALSE &&
            current.type != TOKEN_MINUS)
            reportError(sourceFile, current.line, current.col,
                        "expected condition after 'while', but found '" + current.value + "'",
                        getSourceLine(current.line), std::max(1,(int)current.value.size()),
                        "syntax: while condition { ... }");
        bool hasParen = (current.type == TOKEN_LPAREN);
        if (hasParen) current = nextToken();
        auto cond = parseExpr();
        if (hasParen) eat(TOKEN_RPAREN);
        return std::make_unique<WhileNode>(std::move(cond), parseBlock());
    }

    // ── for ───────────────────────────────────────────────────────────────────
    // Sintaxe: for (init; cond; step) { }
    if (current.type == TOKEN_FOR) {
        current = nextToken();
        if (current.type != TOKEN_LPAREN)
            reportError(sourceFile, current.line, current.col,
                        "expected '(' after 'for', but found '" + current.value + "'",
                        getSourceLine(current.line), std::max(1,(int)current.value.size()),
                        "syntax: for (init; condition; step) { ... }");
        eat(TOKEN_LPAREN);

        NodePtr init;
        // Inicializador: `let [mut] i: i32 = 0` (sem ;) ou `i = 0`
        if (current.type == TOKEN_LET) {
            current = nextToken();
            bool letMut = false;
            if (current.type == TOKEN_MUT) { letMut = true; current = nextToken(); }
            std::string nm = eat(TOKEN_IDENT).value;
            DataType type = DataType::Int;
            if (current.type == TOKEN_COLON) {
                current = nextToken();
                type = parseDataType();
            }
            NodePtr iv = nullptr;
            if (current.type == TOKEN_ASSIGN) { current = nextToken(); iv = parseExpr(); }
            // Em for-loop, a variável de iteração é sempre tratada como mutável
            init = std::make_unique<VarDeclNode>(type, nm, std::move(iv), true, tokLine, tokCol);
            declaredVarNames.insert(nm);
        } else if (current.type == TOKEN_IDENT) {
            std::string nm = current.value;
            int tl = current.line, tc = current.col;
            current = nextToken();
            eat(TOKEN_ASSIGN);
            init = std::make_unique<VarAssignNode>(nm, "=", parseExpr(), tl, tc);
        } else if (current.type != TOKEN_SEMI) {
            reportError(sourceFile, current.line, current.col,
                        "invalid for-loop initializer — expected 'let' or an assignment",
                        getSourceLine(current.line), std::max(1,(int)current.value.size()),
                        "example: for (let mut i: i32 = 0; i < 10; i++) { ... }");
        }
        eat(TOKEN_SEMI);

        auto cond = parseExpr();
        eat(TOKEN_SEMI);

        // Step
        NodePtr step;
        if (current.type != TOKEN_IDENT)
            reportError(sourceFile, current.line, current.col,
                        "expected variable name for for-loop step, but found '" + current.value + "'",
                        getSourceLine(current.line), std::max(1,(int)current.value.size()),
                        "valid steps: i++,  i--,  i += 2,  i = i + 1");
        {
            std::string nm = eat(TOKEN_IDENT).value;
            int tl = current.line, tc = current.col;
            if (current.type == TOKEN_PLUS && peekToken().type == TOKEN_PLUS)
                { eat(TOKEN_PLUS); eat(TOKEN_PLUS); step = std::make_unique<VarAssignNode>(nm, "++", nullptr, tl, tc); }
            else if (current.type == TOKEN_MINUS && peekToken().type == TOKEN_MINUS)
                { eat(TOKEN_MINUS); eat(TOKEN_MINUS); step = std::make_unique<VarAssignNode>(nm, "--", nullptr, tl, tc); }
            else {
                auto tryC = [&](TokenType opTok, const std::string& opStr) -> NodePtr {
                    if (current.type == opTok && peekToken().type == TOKEN_ASSIGN) {
                        eat(opTok); eat(TOKEN_ASSIGN);
                        return std::make_unique<VarAssignNode>(nm, opStr, parseExpr(), tl, tc);
                    }
                    return nullptr;
                };
                if      (auto s = tryC(TOKEN_PLUS,  "+=")) step = std::move(s);
                else if (auto s = tryC(TOKEN_MINUS, "-=")) step = std::move(s);
                else if (auto s = tryC(TOKEN_STAR,  "*=")) step = std::move(s);
                else if (auto s = tryC(TOKEN_SLASH, "/=")) step = std::move(s);
                else {
                    if (current.type != TOKEN_ASSIGN)
                        reportError(sourceFile, current.line, current.col,
                                    "invalid for-loop step after '" + nm + "'",
                                    getSourceLine(current.line), 1,
                                    "valid: " + nm + "++,  " + nm + "--,  " + nm + " += expr");
                    eat(TOKEN_ASSIGN);
                    step = std::make_unique<VarAssignNode>(nm, "=", parseExpr(), tl, tc);
                }
            }
        }
        eat(TOKEN_RPAREN);
        return std::make_unique<ForNode>(std::move(init), std::move(cond), std::move(step), parseBlock());
    }

    // ── asm / ir ──────────────────────────────────────────────────────────────
    auto extractVarRefs = [](const std::string& raw) {
        std::vector<std::string> vars; std::set<std::string> seen;
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] == '$' && i+1 < raw.size() &&
                (std::isalpha((unsigned char)raw[i+1]) || raw[i+1] == '_')) {
                std::string nm; size_t j = i+1;
                while (j < raw.size() && (std::isalnum((unsigned char)raw[j]) || raw[j] == '_'))
                    nm += raw[j++];
                if (!seen.count(nm)) { vars.push_back(nm); seen.insert(nm); }
            }
        }
        return vars;
    };

    if (current.type == TOKEN_ASM || current.type == TOKEN_IR) {
        bool isAsm = (current.type == TOKEN_ASM);
        int tl = current.line, tc = current.col;
        current = nextToken();
        if (current.type != TOKEN_LBRACE)
            reportError(sourceFile, current.line, current.col,
                        std::string("expected '{' after '") + (isAsm ? "asm" : "ir") + "'",
                        getSourceLine(current.line), 1,
                        isAsm ? "syntax: asm { \"instruction\" }"
                              : "syntax: ir { llvm_instruction }");
        current = nextToken();
        std::string code; int depth = 1;
        while (current.type != TOKEN_EOF && depth > 0) {
            if      (current.type == TOKEN_LBRACE) { depth++; code += "{ "; current = nextToken(); }
            else if (current.type == TOKEN_RBRACE) {
                depth--;
                if (depth > 0) { code += "} "; current = nextToken(); }
                else current = nextToken();
            } else if (current.type == TOKEN_STRING_LIT)
                { code += "\"" + current.value + "\" "; current = nextToken(); }
            else if (!isAsm && current.type == TOKEN_PERCENT)
                { code += "% "; current = nextToken(); }
            else { code += current.value + " "; current = nextToken(); }
        }
        if (depth > 0)
            reportError(sourceFile, tl, tc,
                        std::string("unterminated '") + (isAsm ? "asm" : "ir") + "' block — missing '}'",
                        getSourceLine(tl), isAsm ? 3 : 2);
        while (!code.empty() && code.back() == ' ') code.pop_back();
        auto vars = extractVarRefs(code);
        if (isAsm) return std::make_unique<AsmNode>(code, "", vars, tl, tc);
        return std::make_unique<IrNode>(code, vars, tl, tc);
    }

    // ── Statement que começa com IDENT: atribuição, chamada, acesso etc. ──────
    if (current.type == TOKEN_IDENT) {
        std::string firstName = current.value;
        current = nextToken();

        // namespace::... (statement qualificado)
        if (current.type == TOKEN_COLONCOLON) {
            current = nextToken();
            std::string qualPart = eat(TOKEN_IDENT).value;
            std::string qualName = firstName + "::" + qualPart;

            // Declaração de variável struct qualificada: let p: ns::Point = ...
            // — já tratada no parseLetDecl; aqui chegamos apenas se não houver `let`.
            if (current.type == TOKEN_IDENT) {
                std::string varName = current.value;
                current = nextToken();
                if (current.type == TOKEN_ASSIGN) {
                    current = nextToken();
                    if (current.type == TOKEN_IDENT) {
                        std::string callName = current.value;
                        current = nextToken();
                        if (current.type == TOKEN_LPAREN) {
                            current = nextToken();
                            std::vector<NodePtr> args;
                            while (current.type != TOKEN_RPAREN) {
                                if (current.type == TOKEN_EOF)
                                    reportError(sourceFile, tokLine, tokCol,
                                        "unterminated argument list for '" + callName + "()'",
                                        getSourceLine(tokLine), (int)callName.size(), "add ')'");
                                args.push_back(parseExpr());
                                if (current.type == TOKEN_COMMA) current = nextToken();
                            }
                            eat(TOKEN_RPAREN);
                            eat(TOKEN_SEMI);
                            declaredVarStructType[varName] = qualName;
                            return std::make_unique<StructVarDeclNode>(
                                qualName, varName, callName, std::move(args), true, tokLine, tokCol);
                        }
                        eat(TOKEN_SEMI);
                        declaredVarStructType[varName] = qualName;
                        return std::make_unique<StructVarDeclNode>(
                            qualName, varName, "@copy:" + callName,
                            std::vector<NodePtr>{}, true, tokLine, tokCol);
                    }
                }
                eat(TOKEN_SEMI);
                declaredVarStructType[varName] = qualName;
                return std::make_unique<StructVarDeclNode>(qualName, varName, true, tokLine, tokCol);
            }

            if (current.type == TOKEN_ASSIGN) {
                checkMutable(qualName, tokLine, tokCol);
                current = nextToken();
                auto val = parseExpr();
                eat(TOKEN_SEMI);
                return std::make_unique<VarAssignNode>(qualName, "=", std::move(val), tokLine, tokCol);
            }
            eat(TOKEN_LPAREN);
            std::vector<NodePtr> args;
            while (current.type != TOKEN_RPAREN) {
                if (current.type == TOKEN_EOF)
                    reportError(sourceFile, tokLine, tokCol,
                        "unterminated argument list for '" + qualName + "()'",
                        getSourceLine(tokLine), (int)qualName.size(), "add ')'");
                args.push_back(parseExpr());
                if (current.type == TOKEN_COMMA) current = nextToken();
            }
            eat(TOKEN_RPAREN);
            eat(TOKEN_SEMI);
            checkCallSite(qualName, args, tokLine, tokCol);
            return std::make_unique<CallNode>(qualName, std::move(args), tokLine, tokCol);
        }

        // Struct var sem `let`: Point p;  ou  Point p = func();
        // (mantido para compatibilidade — mas emite aviso sobre preferir `let`)
        if (current.type == TOKEN_IDENT && declaredStructNames.count(firstName)) {
            std::string varName = current.value;
            current = nextToken();
            if (current.type == TOKEN_ASSIGN) {
                current = nextToken();
                if (current.type == TOKEN_IDENT) {
                    std::string callName = current.value;
                    current = nextToken();
                    if (current.type == TOKEN_LPAREN) {
                        current = nextToken();
                        std::vector<NodePtr> args;
                        while (current.type != TOKEN_RPAREN) {
                            if (current.type == TOKEN_EOF)
                                reportError(sourceFile, tokLine, tokCol,
                                    "unterminated argument list for '" + callName + "()'",
                                    getSourceLine(tokLine), (int)callName.size(), "add ')'");
                            args.push_back(parseExpr());
                            if (current.type == TOKEN_COMMA) current = nextToken();
                        }
                        eat(TOKEN_RPAREN);
                        eat(TOKEN_SEMI);
                        declaredVarStructType[varName] = firstName;
                        return std::make_unique<StructVarDeclNode>(
                            firstName, varName, callName, std::move(args), true, tokLine, tokCol);
                    }
                    eat(TOKEN_SEMI);
                    declaredVarStructType[varName] = firstName;
                    return std::make_unique<StructVarDeclNode>(
                        firstName, varName, "@copy:" + callName,
                        std::vector<NodePtr>{}, true, tokLine, tokCol);
                }
            }
            if (current.type == TOKEN_LBRACKET) {
                current = nextToken();
                if (current.type != TOKEN_INT_LIT)
                    reportError(sourceFile, current.line, current.col,
                        "array size must be a constant integer",
                        getSourceLine(current.line), std::max(1,(int)current.value.size()),
                        "example: let arr: " + firstName + "[10];");
                int sizeLine = current.line, sizeCol = current.col;
                int size = safeStoi(eat(TOKEN_INT_LIT).value, sizeLine, sizeCol);
                if (size <= 0)
                    reportError(sourceFile, sizeLine, sizeCol,
                        "array size must be positive", getSourceLine(sizeLine), 1);
                eat(TOKEN_RBRACKET);
                eat(TOKEN_SEMI);
                arraySizes[varName] = size;
                return std::make_unique<ArrayDeclNode>(DataType::Custom, firstName, varName, size,
                                                       std::vector<NodePtr>{}, tokLine, tokCol, true);
            }
            eat(TOKEN_SEMI);
            declaredVarStructType[varName] = firstName;
            return std::make_unique<StructVarDeclNode>(firstName, varName, true, tokLine, tokCol);
        }

        // Acesso a array: arr[i] = ...
        if (current.type == TOKEN_LBRACKET) {
            current = nextToken();
            int idxLine = current.line, idxCol = current.col;
            auto index = parseExpr();
            eat(TOKEN_RBRACKET);
            if (auto* lit = dynamic_cast<IntLitNode*>(index.get())) {
                auto it = arraySizes.find(firstName);
                if (it != arraySizes.end()) {
                    int idx = lit->value;
                    if (idx < 0)
                        reportError(sourceFile, idxLine, idxCol,
                            "negative array index " + std::to_string(idx) + " for '" + firstName + "'",
                            getSourceLine(idxLine), (int)std::to_string(idx).size(),
                            "array indices start at 0");
                    if (idx >= it->second)
                        reportError(sourceFile, idxLine, idxCol,
                            "index " + std::to_string(idx) + " is out of bounds for array '"
                            + firstName + "' (size " + std::to_string(it->second) + ")",
                            getSourceLine(idxLine), (int)std::to_string(idx).size(),
                            "valid indices are 0 to " + std::to_string(it->second - 1));
                }
            }
            // arr[i].field = val;
            if (current.type == TOKEN_DOT) {
                checkMutable(firstName, tokLine, tokCol);
                current = nextToken();
                std::string field = eat(TOKEN_IDENT).value;
                if (current.type != TOKEN_ASSIGN)
                    reportError(sourceFile, current.line, current.col,
                        "expected '=' after '" + firstName + "[index]." + field + "'",
                        getSourceLine(current.line), 1,
                        "syntax: " + firstName + "[index]." + field + " = value;");
                eat(TOKEN_ASSIGN);
                auto val = parseExpr();
                eat(TOKEN_SEMI);
                return std::make_unique<ArrayFieldAssignNode>(
                    firstName, std::move(index), field, std::move(val), tokLine, tokCol);
            }
            if (current.type != TOKEN_ASSIGN)
                reportError(sourceFile, current.line, current.col,
                    "expected '=' after array index in assignment",
                    getSourceLine(current.line), 1,
                    "syntax: " + firstName + "[index] = value;");
            checkMutable(firstName, tokLine, tokCol);
            eat(TOKEN_ASSIGN);
            if (current.type == TOKEN_IDENT) {
                LexerState st2 = saveLexerState();
                Token saved2 = current;
                std::string rhsName = current.value;
                current = nextToken();
                if (current.type == TOKEN_LPAREN) {
                    current = nextToken();
                    std::vector<NodePtr> callArgs;
                    while (current.type != TOKEN_RPAREN) {
                        if (current.type == TOKEN_EOF)
                            reportError(sourceFile, tokLine, tokCol,
                                "unterminated argument list for '" + rhsName + "()'",
                                getSourceLine(tokLine), (int)rhsName.size(), "add ')'");
                        callArgs.push_back(parseExpr());
                        if (current.type == TOKEN_COMMA) current = nextToken();
                    }
                    eat(TOKEN_RPAREN);
                    eat(TOKEN_SEMI);
                    return std::make_unique<ArrayStructAssignNode>(
                        firstName, std::move(index), rhsName, std::move(callArgs), tokLine, tokCol);
                }
                if (current.type == TOKEN_SEMI) {
                    eat(TOKEN_SEMI);
                    return std::make_unique<ArrayStructAssignNode>(
                        firstName, std::move(index), "@copy:" + rhsName,
                        std::vector<NodePtr>{}, tokLine, tokCol);
                }
                restoreLexerState(st2);
                current = saved2;
            }
            auto val = parseExpr();
            eat(TOKEN_SEMI);
            return std::make_unique<ArrayAssignNode>(firstName, std::move(index), std::move(val), tokLine, tokCol);
        }

        // Acesso de campo / chamada de método como statement
        if (current.type == TOKEN_DOT) {
            if (immutableVars.count(firstName) && !mutableReferences.count(firstName)) {
                reportError(sourceFile, tokLine, tokCol,
                            "cannot mutate fields of immutable variable '" + firstName + "'",
                            getSourceLine(tokLine), (int)firstName.size(),
                            "declare with 'let mut' or use a '&mut' reference to allow field mutation");
            }
            current = nextToken();
            std::string member = eat(TOKEN_IDENT).value;
            std::string composedName = firstName;
            std::string composedMember = member;
            while (current.type == TOKEN_DOT) {
                composedName = composedName + "." + composedMember;
                current = nextToken();
                composedMember = eat(TOKEN_IDENT).value;
            }
            if (current.type == TOKEN_LPAREN) {
                current = nextToken();
                std::vector<NodePtr> args;
                while (current.type != TOKEN_RPAREN) {
                    if (current.type == TOKEN_EOF)
                        reportError(sourceFile, tokLine, tokCol,
                            "unterminated argument list for '" + composedName + "." + composedMember + "()'",
                            getSourceLine(tokLine), (int)composedMember.size(), "add ')'");
                    args.push_back(parseExpr());
                    if (current.type == TOKEN_COMMA) current = nextToken();
                }
                eat(TOKEN_RPAREN);
                eat(TOKEN_SEMI);
                // Resolve o tipo do objeto para validar assinatura do método
                {
                    auto typeIt = declaredVarStructType.find(composedName);
                    if (typeIt != declaredVarStructType.end())
                        checkCallSite(typeIt->second + "::" + composedMember, args, tokLine, tokCol);
                }
                return std::make_unique<MethodCallNode>(composedName, composedMember, std::move(args), tokLine, tokCol);
            }
            if (current.type != TOKEN_ASSIGN)
                reportError(sourceFile, current.line, current.col,
                    "expected '=' after field access '" + composedName + "." + composedMember + "'",
                    getSourceLine(current.line), 1,
                    "syntax: " + composedName + "." + composedMember + " = value;");
            eat(TOKEN_ASSIGN);
            auto val = parseExpr();
            eat(TOKEN_SEMI);
            return std::make_unique<FieldAssignNode>(composedName, composedMember, std::move(val), tokLine, tokCol);
        }

        // i++  /  i--
        if (current.type == TOKEN_PLUS && peekToken().type == TOKEN_PLUS) {
            checkMutable(firstName, tokLine, tokCol);
            eat(TOKEN_PLUS); eat(TOKEN_PLUS); eat(TOKEN_SEMI);
            return std::make_unique<VarAssignNode>(firstName, "++", nullptr, tokLine, tokCol);
        }
        if (current.type == TOKEN_MINUS && peekToken().type == TOKEN_MINUS) {
            checkMutable(firstName, tokLine, tokCol);
            eat(TOKEN_MINUS); eat(TOKEN_MINUS); eat(TOKEN_SEMI);
            return std::make_unique<VarAssignNode>(firstName, "--", nullptr, tokLine, tokCol);
        }

        // Compound assignment: +=, -=, *=, /=
        auto tryCompound = [&](TokenType opTok, const std::string& opStr) -> NodePtr {
            if (current.type == opTok && peekToken().type == TOKEN_ASSIGN) {
                checkMutable(firstName, tokLine, tokCol);
                eat(opTok); eat(TOKEN_ASSIGN);
                auto val = parseExpr(); eat(TOKEN_SEMI);
                return std::make_unique<VarAssignNode>(firstName, opStr, std::move(val), tokLine, tokCol);
            }
            return nullptr;
        };
        if (auto nd = tryCompound(TOKEN_PLUS,  "+=")) return nd;
        if (auto nd = tryCompound(TOKEN_MINUS, "-=")) return nd;
        if (auto nd = tryCompound(TOKEN_STAR,  "*=")) return nd;
        if (auto nd = tryCompound(TOKEN_SLASH, "/=")) return nd;

        // Atribuição simples: x = expr
        if (current.type == TOKEN_ASSIGN) {
            checkMutable(firstName, tokLine, tokCol);
            eat(TOKEN_ASSIGN);
            if (current.type == TOKEN_IDENT) {
                LexerState st = saveLexerState();
                Token saved = current;
                std::string rhsName = current.value;
                current = nextToken();
                if (current.type == TOKEN_COLONCOLON) {
                    current = nextToken();
                    rhsName = rhsName + "::" + eat(TOKEN_IDENT).value;
                }
                if (current.type == TOKEN_LPAREN) {
                    current = nextToken();
                    std::vector<NodePtr> args;
                    while (current.type != TOKEN_RPAREN) {
                        if (current.type == TOKEN_EOF)
                            reportError(sourceFile, tokLine, tokCol,
                                "unterminated argument list for '" + rhsName + "()'",
                                getSourceLine(tokLine), (int)rhsName.size(), "add ')'");
                        args.push_back(parseExpr());
                        if (current.type == TOKEN_COMMA) current = nextToken();
                    }
                    eat(TOKEN_RPAREN);
                    eat(TOKEN_SEMI);
                    // Se o destino foi declarado como struct, então a atribuição
                    // completa precisa ser tratada como StructAssignNode.
                    // Caso contrário (x é primitivo), trate como VarAssignNode
                    // atribuindo o resultado da chamada de função.
                    if (declaredVarStructType.find(firstName) != declaredVarStructType.end()) {
                        return std::make_unique<StructAssignNode>(
                            firstName, rhsName, std::move(args), tokLine, tokCol);
                    }
                    return std::make_unique<VarAssignNode>(
                        firstName, "=", std::make_unique<CallNode>(rhsName, std::move(args), tokLine, tokCol),
                        tokLine, tokCol);
                }
                restoreLexerState(st);
                current = saved;
            }
            auto val = parseExpr(); eat(TOKEN_SEMI);
            return std::make_unique<VarAssignNode>(firstName, "=", std::move(val), tokLine, tokCol);
        }

        // Chamada de função como statement: foo(...)
        if (current.type == TOKEN_LPAREN) {
            current = nextToken();
            std::vector<NodePtr> args;
            while (current.type != TOKEN_RPAREN) {
                if (current.type == TOKEN_EOF)
                    reportError(sourceFile, tokLine, tokCol,
                        "unterminated argument list for '" + firstName + "()'",
                        getSourceLine(tokLine), (int)firstName.size(), "add ')'");
                args.push_back(parseExpr());
                if (current.type == TOKEN_COMMA) current = nextToken();
            }
            eat(TOKEN_RPAREN); eat(TOKEN_SEMI);
            checkCallSite(firstName, args, tokLine, tokCol);
            return std::make_unique<CallNode>(firstName, std::move(args), tokLine, tokCol);
        }

        // Erro: statement inválido com IDENT
        {
            std::string ln = getSourceLine(current.line);
            std::string hint = didYouMean(firstName, declaredVarNames);
            if (hint.empty()) hint = didYouMean(firstName, declaredFunctionNames);

            // Detecta padrão de declaração sem `let`
            if (current.type == TOKEN_IDENT) {
                reportError(sourceFile, tokLine, tokCol,
                            "missing 'let' before variable declaration",
                            ln, (int)firstName.size(),
                            "use: let " + firstName + ": Type = value;  or  let mut " + firstName + ": Type = value;");
            }
            if (hint.empty())
                hint = "statements must be: let, return, if, for, while, an assignment, or a function call";
            reportError(sourceFile, current.line, current.col,
                        "invalid statement after '" + firstName + "'",
                        ln, std::max(1,(int)current.value.size()), hint);
        }
    }

    // ── Erro final ────────────────────────────────────────────────────────────
    {
        std::string ln = getSourceLine(current.line);
        int tl = current.line, tc = current.col;
        int tlen = std::max(1,(int)current.value.size());

        if (current.type == TOKEN_RBRACE)
            reportError(sourceFile, tl, tc,
                        "unexpected '}' — too many closing braces",
                        ln, 1, "check for an extra '}' or a missing '{'");
        if (current.type == TOKEN_EOF)
            reportError(sourceFile, tl, tc,
                        "unexpected end of file inside function body",
                        ln, 1, "add '}' to close the open function");

        // Detecta declaração legada de variável: `int x = ...` sem `let`
        if (current.type == TOKEN_INT    || current.type == TOKEN_FLOAT  ||
            current.type == TOKEN_STRING || current.type == TOKEN_VOID   ||
            current.type == TOKEN_DOUBLE || current.type == TOKEN_LONG   ||
            current.type == TOKEN_CHAR   || current.type == TOKEN_BOOL) {
            std::string typeName = current.value;
            current = nextToken();
            std::string varName = (current.type == TOKEN_IDENT) ? current.value : "name";
            reportError(sourceFile, tl, tc,
                        "variable declarations require 'let'",
                        ln, (int)typeName.size(),
                        "use: let " + varName + ": " + typeName + " = value;  "
                        "or: let mut " + varName + ": " + typeName + " = value;");
        }

        std::string hint = didYouMean(current.value, declaredVarNames);
        if (hint.empty()) hint = didYouMean(current.value, declaredFunctionNames);
        if (hint.empty())
            hint = "statements must start with: let, return, if, for, while, or a variable/function name";
        reportError(sourceFile, tl, tc,
                    "invalid statement starting with '" + current.value + "'",
                    ln, tlen, hint);
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PARSING DE FUNÇÕES (fn)
// ═══════════════════════════════════════════════════════════════════════════════

// Parseia parâmetros: `(name: Type, name: Type, ...)`
// Também suporta `&self` e `&mut self` como primeiro parâmetro (impl methods)
static std::vector<ParamNode> parseFnParams(const std::string& fnName,
                                             int tokLine, int tokCol,
                                             bool* hasRefSelf = nullptr,
                                             bool* outIsVariadic = nullptr) {
    eat(TOKEN_LPAREN);
    std::vector<ParamNode> params;
    if (hasRefSelf) *hasRefSelf = false;
    if (outIsVariadic) *outIsVariadic = false;

    while (current.type != TOKEN_RPAREN) {
        if (current.type == TOKEN_EOF)
            reportError(sourceFile, tokLine, tokCol,
                "unterminated parameter list for '" + fnName + "'",
                getSourceLine(tokLine), (int)fnName.size(),
                "add closing ')' and the function body");

        // Variádico: ...
        if (current.type == TOKEN_ELLIPSIS) {
            if (outIsVariadic) *outIsVariadic = true;
            current = nextToken();
            break;
        }

        // &self, &mut self ou & [mut] name: Type
        bool isRef = false;
        bool isMut = false;
        if (current.type == TOKEN_AMPERSAND) {
            isRef = true;
            current = nextToken();
            if (current.type == TOKEN_MUT) {
                isMut = true;
                current = nextToken();
            }
            if (current.type == TOKEN_SELF) {
                if (hasRefSelf) *hasRefSelf = true;
                if (isMut) mutableReferences.insert("self");
                current = nextToken();
                if (current.type == TOKEN_COMMA) current = nextToken();
                continue;
            }
        }

        // self (sem &) — não suportado, erro explícito
        if (current.type == TOKEN_SELF) {
            std::string ln = getSourceLine(current.line);
            reportError(sourceFile, current.line, current.col,
                "bare 'self' receiver is not supported — use '&self' or '&mut self'",
                ln, 4,
                "example: fn method(&self, x: i32) -> void { ... }");
        }

        // name: Type  (Rust-style)
        std::string pname = eat(TOKEN_IDENT).value;
        eat(TOKEN_COLON);
        DataType ptype = parseDataType();
        std::string pStructType = (ptype == DataType::Custom) ? lastCustomTypeName : "";

        // Se começamos com &, injetamos no pStructType se já não for referência
        if (isRef && pStructType.find('&') == std::string::npos) {
            std::string baseName = dataTypeToString(ptype);
            pStructType = (isMut ? "&mut " : "&") + baseName;
            ptype = DataType::Custom;
        }

        // Suporte a arrays como parâmetros: name: Type[N] ou name: Type[]
        // Codifica como "__arr__N__name" para o codegen registrar em localArrays
        if (current.type == TOKEN_LBRACKET) {
            current = nextToken();
            int arrSize = 0;
            if (current.type == TOKEN_INT_LIT) {
                arrSize = std::stoi(current.value);
                current = nextToken();
            }
            eat(TOKEN_RBRACKET);
            // Codifica: pname vira "__arr__N__originalName", tipo permanece igual
            pname = "__arr__" + std::to_string(arrSize) + "__" + pname;
        }

        params.push_back({ptype, pname, pStructType});
        if (current.type == TOKEN_COMMA) current = nextToken();
    }
    eat(TOKEN_RPAREN);
    return params;
}

// Parseia tipo de retorno: `-> Type` ou nada (implícito void)
static DataType parseFnReturn() {
    if (current.type != TOKEN_ARROW) return DataType::Void;
    current = nextToken();
    return parseDataType();
}

// ═══════════════════════════════════════════════════════════════════════════════
// PARSING DE .nh (HEADER FILES)
// ═══════════════════════════════════════════════════════════════════════════════

static std::vector<NodePtr> parseNhFile(const std::string& nhPath,
                                         const std::string& includedFrom) {
    if (includedHeaders.count(nhPath)) return {};
    includedHeaders.insert(nhPath);

    std::ifstream f(nhPath);
    if (!f) {
        std::cerr << "\n" << BOLD << RED << "error: " << RESET << BOLD
                  << "cannot open header file '" << nhPath << "'\n" << RESET;
        std::cerr << BOLD << "  included from: " << RESET << includedFrom << "\n";
        bool isSystem = (nhPath.find("/usr/local/lib/nova") != std::string::npos) ||
                        (std::getenv("NOVA_STDLIB_PATH") &&
                         nhPath.find(std::getenv("NOVA_STDLIB_PATH")) != std::string::npos);
        if (isSystem) {
            std::cerr << CYAN << "  hint: " << RESET
                      << "system header not found — is NOVA_STDLIB_PATH set correctly?\n";
            std::cerr << CYAN << "  hint: " << RESET
                      << "example: export NOVA_STDLIB_PATH=/usr/local/lib/nova\n";
        } else {
            std::cerr << CYAN << "  hint: " << RESET
                      << "check that the path is relative to '" << includedFrom << "'\n";
            std::cerr << CYAN << "  hint: " << RESET
                      << "header files must have the .nh extension\n";
        }
        std::cerr << "\n";
        exit(1);
    }

    std::stringstream buf; buf << f.rdbuf();
    std::string nhSource = buf.str();

    LexerState savedLexer = saveLexerState();
    Token savedCurrent    = current;
    std::string savedSrc  = sourceFile;

    initLexer(nhSource);
    sourceFile = nhPath;
    current = nextToken();

    std::string currentNhNamespace;
    std::vector<NodePtr> decls;

    while (current.type != TOKEN_EOF) {
        if (current.type == TOKEN_NAMESPACE) {
            current = nextToken();
            currentNhNamespace = eat(TOKEN_IDENT).value;
            eat(TOKEN_SEMI);
            continue;
        }
        if (current.type == TOKEN_STRUCT) {
            int tl = current.line, tc = current.col;
            current = nextToken();
            std::string sName = eat(TOKEN_IDENT).value;
            std::string qualSName = currentNhNamespace.empty() ? sName : currentNhNamespace + "::" + sName;
            eat(TOKEN_LBRACE);
            std::vector<StructField> fields;
            while (current.type != TOKEN_RBRACE && current.type != TOKEN_EOF) {
                if (current.type == TOKEN_INT    || current.type == TOKEN_FLOAT  ||
                    current.type == TOKEN_STRING  || current.type == TOKEN_DOUBLE ||
                    current.type == TOKEN_LONG    || current.type == TOKEN_VOID   ||
                    current.type == TOKEN_CHAR    || current.type == TOKEN_BOOL   ||
                    current.type == TOKEN_IDENT) {
                    // Campo Rust-style: name: Type,  ou  Type name;  (retro-compat)
                    // Detecção: se o próximo após o IDENT for ':', é name: Type
                    if (current.type == TOKEN_IDENT) {
                        LexerState st = saveLexerState();
                        Token savedTok = current;
                        std::string maybeName = current.value;
                        current = nextToken();
                        if (current.type == TOKEN_COLON) {
                            // Rust-style: name: Type,
                            current = nextToken();
                            DataType ft = parseDataType();
                            std::string ftStruct = (ft == DataType::Custom) ? lastCustomTypeName : "";
                            if (current.type == TOKEN_COMMA) current = nextToken();
                            else if (current.type == TOKEN_SEMI) current = nextToken();
                            fields.push_back({ft, maybeName, ftStruct});
                            continue;
                        }
                        // Retro-compat: Type name;
                        restoreLexerState(st);
                        current = savedTok;
                    }
                    DataType ft = parseDataType();
                    std::string ftStruct = (ft == DataType::Custom) ? lastCustomTypeName : "";
                    std::string fn = eat(TOKEN_IDENT).value;
                    if (current.type == TOKEN_SEMI) current = nextToken();
                    else if (current.type == TOKEN_COMMA) current = nextToken();
                    fields.push_back({ft, fn, ftStruct});
                } else {
                    reportError(nhPath, current.line, current.col,
                                "unexpected token in struct field declaration",
                                getSourceLine(current.line), std::max(1,(int)current.value.size()),
                                "field syntax: name: Type,  (e.g. x: i32, y: f32)");
                }
            }
            if (current.type == TOKEN_EOF)
                reportError(nhPath, current.line, current.col,
                            "unterminated struct '" + sName + "' — missing '}'",
                            getSourceLine(current.line), 1);
            eat(TOKEN_RBRACE);
            if (current.type == TOKEN_SEMI) current = nextToken();
            decls.push_back(std::make_unique<StructDefNode>(
                qualSName, std::move(fields), std::vector<StructMethod>{}, tl, tc));
            continue;
        }

        // fn declaração de função (header .nh)
        if (current.type == TOKEN_FN) {
            int tl = current.line, tc = current.col;
            current = nextToken();
            std::string name = eat(TOKEN_IDENT).value;
            bool isVariadic = false;
            auto params = parseFnParams(name, tl, tc, nullptr, &isVariadic);
            DataType retType = parseFnReturn();
            std::string retStructTypeName = (retType == DataType::Custom) ? lastCustomTypeName : "";
            eat(TOKEN_SEMI);
            std::string qualName = currentNhNamespace.empty() ? name : currentNhNamespace + "::" + name;
            // Registra assinatura para validação de call sites
            {
                std::vector<FnParamInfo> sig;
                for (const auto& p : params) sig.push_back(buildParamInfo(p));
                fnSignatures[qualName] = sig;
                if (qualName != name) fnSignatures[name] = sig;
                if (isVariadic) {
                    variadicFunctions.insert(qualName);
                    if (qualName != name) variadicFunctions.insert(name);
                }
            }
            decls.push_back(std::make_unique<FuncDeclNode>(
                retType, retStructTypeName, qualName, std::move(params), isVariadic));
            continue;
        }

        // Variável global: let NAME: TYPE = value;  (em header)
        if (current.type == TOKEN_LET) {
            int tl = current.line, tc = current.col;
            current = nextToken();
            if (current.type == TOKEN_MUT) current = nextToken();
            std::string name = eat(TOKEN_IDENT).value;
            eat(TOKEN_COLON);
            DataType retType = parseDataType();
            std::string retStructTypeName = (retType == DataType::Custom) ? lastCustomTypeName : "";
            NodePtr init = nullptr;
            if (current.type == TOKEN_ASSIGN) {
                current = nextToken();
                if (current.type == TOKEN_INT_LIT) {
                    long long vl = safeStoll(current.value, current.line, current.col);
                    current = nextToken();
                    init = (vl > 2147483647LL || vl < -2147483648LL)
                        ? (NodePtr)std::make_unique<LongLitNode>(vl)
                        : (NodePtr)std::make_unique<IntLitNode>((int)vl);
                } else if (current.type == TOKEN_FLOAT_LIT) {
                    init = std::make_unique<FloatLitNode>(current.value); current = nextToken();
                } else if (current.type == TOKEN_STRING_LIT) {
                    init = std::make_unique<StringLitNode>(current.value); current = nextToken();
                } else {
                    reportError(nhPath, current.line, current.col,
                                "only literal values are allowed as initializers in .nh files",
                                getSourceLine(current.line), std::max(1,(int)current.value.size()),
                                "expressions and function calls are not permitted here");
                }
            }
            eat(TOKEN_SEMI);
            std::string qn = currentNhNamespace.empty() ? name : currentNhNamespace + "::" + name;
            decls.push_back(std::make_unique<VarDeclNode>(retType, qn, std::move(init), false, tl, tc));
            continue;
        }

        // Sintaxe legada em .nh: Type name(...);  ou  Type name;
        if (current.type == TOKEN_INT    || current.type == TOKEN_FLOAT  ||
            current.type == TOKEN_STRING || current.type == TOKEN_VOID   ||
            current.type == TOKEN_DOUBLE || current.type == TOKEN_LONG   ||
            current.type == TOKEN_CHAR   || current.type == TOKEN_BOOL   ||
            current.type == TOKEN_IDENT) {
            int tl = current.line, tc = current.col;
            DataType retType = parseDataType();
            std::string retStructTypeName = (retType == DataType::Custom) ? lastCustomTypeName : "";
            std::string name = eat(TOKEN_IDENT).value;
            if (current.type == TOKEN_LPAREN) {
                current = nextToken();
                std::vector<ParamNode> params;
                bool isVariadic = false;
                while (current.type != TOKEN_RPAREN && current.type != TOKEN_EOF) {
                    if (current.type == TOKEN_ELLIPSIS) { isVariadic = true; current = nextToken(); break; }
                    DataType ptype = parseDataType();
                    std::string pStructType = (ptype == DataType::Custom) ? lastCustomTypeName : "";
                    std::string pname = "_";
                    if (current.type == TOKEN_IDENT) pname = eat(TOKEN_IDENT).value;
                    if (current.type == TOKEN_LBRACKET) {
                        current = nextToken();
                        if (current.type == TOKEN_INT_LIT) current = nextToken();
                        eat(TOKEN_RBRACKET);
                    }
                    if (ptype == DataType::Custom)
                        params.push_back({DataType::Void, "__struct__" + pStructType + "::" + pname, pStructType});
                    else
                        params.push_back({ptype, pname, ""});
                    if (current.type == TOKEN_COMMA) current = nextToken();
                }
                if (current.type == TOKEN_EOF)
                    reportError(nhPath, tl, tc,
                                "unterminated parameter list for '" + name + "' in header",
                                getSourceLine(tl), (int)name.size(),
                                "add ')' and ';' to complete the declaration");
                eat(TOKEN_RPAREN);
                eat(TOKEN_SEMI);
                std::string qualName = currentNhNamespace.empty() ? name : currentNhNamespace + "::" + name;
                // Registra assinatura para validação de call sites
                {
                    std::vector<FnParamInfo> sig;
                    for (const auto& p : params) sig.push_back(buildParamInfo(p));
                    fnSignatures[qualName] = sig;
                    if (qualName != name) fnSignatures[name] = sig;
                    if (isVariadic) {
                        variadicFunctions.insert(qualName);
                        if (qualName != name) variadicFunctions.insert(name);
                    }
                }
                decls.push_back(std::make_unique<FuncDeclNode>(
                    retType, retStructTypeName, qualName, std::move(params), isVariadic));
            } else {
                NodePtr init = nullptr;
                if (current.type == TOKEN_ASSIGN) {
                    current = nextToken();
                    if (current.type == TOKEN_INT_LIT) {
                        long long vl = safeStoll(current.value, current.line, current.col);
                        current = nextToken();
                        init = (vl > 2147483647LL || vl < -2147483648LL)
                            ? (NodePtr)std::make_unique<LongLitNode>(vl)
                            : (NodePtr)std::make_unique<IntLitNode>((int)vl);
                    } else if (current.type == TOKEN_FLOAT_LIT) {
                        init = std::make_unique<FloatLitNode>(current.value); current = nextToken();
                    } else if (current.type == TOKEN_STRING_LIT) {
                        init = std::make_unique<StringLitNode>(current.value); current = nextToken();
                    } else {
                        reportError(nhPath, current.line, current.col,
                                    "only literal values are allowed as initializers in .nh files",
                                    getSourceLine(current.line), std::max(1,(int)current.value.size()),
                                    "expressions and function calls are not permitted here");
                    }
                }
                eat(TOKEN_SEMI);
                std::string qn = currentNhNamespace.empty() ? name : currentNhNamespace + "::" + name;
                decls.push_back(std::make_unique<VarDeclNode>(retType, qn, std::move(init), false, tl, tc));
            }
            continue;
        }

        reportError(nhPath, current.line, current.col,
                    "unexpected '" + current.value + "' in header file",
                    getSourceLine(current.line), std::max(1,(int)current.value.size()),
                    ".nh files may only contain fn signatures, struct definitions, "
                    "and global variable declarations");
    }

    restoreLexerState(savedLexer);
    sourceFile = savedSrc;
    current    = savedCurrent;
    return decls;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PARSING DE `impl StructName { fn ... }`
// Os métodos são injetados diretamente no StructDefNode já na lista de
// declarações do programa — assim o codegen existente os encontra normalmente
// sem precisar conhecer ImplNode.
// ═══════════════════════════════════════════════════════════════════════════════

static void parseImpl(ProgramNode& program, int tokLine, int tokCol) {
    std::string structName = eat(TOKEN_IDENT).value;

    if (!declaredStructNames.count(structName)) {
        std::string ln = getSourceLine(tokLine);
        std::string hint = didYouMean(structName, declaredStructNames);
        if (hint.empty())
            hint = "'" + structName + "' was not declared as a struct — declare it before impl";
        reportError(sourceFile, tokLine, tokCol,
                    "cannot impl unknown type '" + structName + "'",
                    ln, (int)structName.size(), hint);
    }

    // Encontra o StructDefNode correspondente para injetar os métodos
    StructDefNode* targetStruct = nullptr;
    for (auto& decl : program.declarations) {
        if (auto* sd = dynamic_cast<StructDefNode*>(decl.get())) {
            if (sd->name == structName) { targetStruct = sd; break; }
        }
    }
    // Não deve acontecer pois declaredStructNames foi populado ao parsear o struct
    if (!targetStruct) {
        reportError(sourceFile, tokLine, tokCol,
                    "internal error: struct '" + structName + "' not found in declarations",
                    getSourceLine(tokLine), (int)structName.size());
    }

    eat(TOKEN_LBRACE);

    while (current.type != TOKEN_RBRACE) {
        if (current.type == TOKEN_EOF)
            reportError(sourceFile, tokLine, tokCol,
                        "unterminated impl '" + structName + "' — missing '}'",
                        getSourceLine(tokLine), (int)structName.size());

        if (current.type != TOKEN_FN)
            reportError(sourceFile, current.line, current.col,
                        "expected 'fn' inside impl block, but found '" + current.value + "'",
                        getSourceLine(current.line), std::max(1,(int)current.value.size()),
                        "impl blocks may only contain fn definitions");

        int fnLine = current.line, fnCol = current.col;
        current = nextToken();
        std::string methodName = eat(TOKEN_IDENT).value;

        // Verifica duplicata de método no mesmo impl
        for (auto& existing : targetStruct->methods) {
            if (existing.name == methodName) {
                reportError(sourceFile, fnLine, fnCol,
                            "method '" + methodName + "' is defined more than once in impl '" + structName + "'",
                            getSourceLine(fnLine), (int)methodName.size(),
                            "each method name must be unique within an impl block");
            }
        }

        immutableVars.clear(); // novo escopo de função
        mutableReferences.clear();

        bool hasRefSelf = false;
        auto params = parseFnParams(methodName, fnLine, fnCol, &hasRefSelf);
        DataType retType = parseFnReturn();
        std::string retStructType = (retType == DataType::Custom) ? lastCustomTypeName : "";

        if (current.type == TOKEN_SEMI)
            reportError(sourceFile, current.line, current.col,
                        "method '" + methodName + "' has no body",
                        getSourceLine(current.line), 1,
                        "add the method body: { ... }");

        declaredFunctionNames.insert(structName + "::" + methodName);
        currentReturnType = retType;

        // Registra assinatura para validação de call sites (método: StructName::methodName)
        {
            std::vector<FnParamInfo> sig;
            for (const auto& p : params) sig.push_back(buildParamInfo(p));
            fnSignatures[structName + "::" + methodName] = sig;
        }
        if (hasRefSelf) immutableVars.insert("self");

        // Parameters are immutable by default
        for (const auto& p : params) {
            std::string pName = p.name;
            if (pName.substr(0, 7) == "__arr__") {
                auto sep = pName.find("__", 7);
                if (sep != std::string::npos) pName = pName.substr(sep + 2);
            }
            if (p.type == DataType::Custom && p.structTypeName.substr(0, 5) == "&mut ") {
                mutableReferences.insert(pName);
            }
            immutableVars.insert(pName);
        }

        auto body = parseBlock();
        targetStruct->methods.push_back({retType, retStructType, methodName,
                                         std::move(params), std::move(body), hasRefSelf});
        
        currentReturnType = DataType::Void;
    }
    eat(TOKEN_RBRACE);
}

// ═══════════════════════════════════════════════════════════════════════════════
// PROGRAMA PRINCIPAL
// ═══════════════════════════════════════════════════════════════════════════════

ProgramNode parseProgram(const std::string& source, const std::string& filename) {
    sourceFile = filename;
    includedHeaders.clear();
    arraySizes.clear();
    declaredFunctionNames.clear();
    declaredVarNames.clear();
    declaredStructNames.clear();
    immutableVars.clear();
    fnSignatures.clear();
    variadicFunctions.clear();
    declaredVarStructType.clear();
    initLexer(source);
    current = nextToken();
    ProgramNode program;

    while (current.type != TOKEN_EOF) {

        // ── #include ──────────────────────────────────────────────────────────
        if (current.type == TOKEN_HASH) {
            int tl = current.line, tc = current.col;
            current = nextToken();
            if (current.type != TOKEN_INCLUDE)
                reportError(sourceFile, tl, tc,
                            "expected 'include' after '#', but found '" + current.value + "'",
                            getSourceLine(tl), 1,
                            "only '#include' directives are supported");
            current = nextToken();
            std::string headerPath; bool isSystem = false;
            if (current.type == TOKEN_STRING_LIT)
                { headerPath = current.value; current = nextToken(); }
            else if (current.type == TOKEN_HEADER_PATH)
                { headerPath = current.value; isSystem = true; current = nextToken(); }
            else
                reportError(sourceFile, current.line, current.col,
                            "expected a filename after '#include', but found '" + current.value + "'",
                            getSourceLine(current.line), std::max(1,(int)current.value.size()),
                            "use quotes for local: #include \"file.nh\"  "
                            "or angle brackets for stdlib: #include <stdio.nh>");
            std::string fullPath;
            if (isSystem) {
                const char* ev = std::getenv("NOVA_STDLIB_PATH");
                fullPath = (ev ? std::string(ev) : "/usr/local/lib/nova") + "/" + headerPath;
            } else {
                std::string base;
                auto sl = sourceFile.rfind('/');
                if (sl != std::string::npos) base = sourceFile.substr(0, sl + 1);
                fullPath = base + headerPath;
            }
            for (auto& d : parseNhFile(fullPath, sourceFile))
                program.declarations.push_back(std::move(d));
            continue;
        }

        // ── struct Foo { ... } ────────────────────────────────────────────────
        // Apenas campos são permitidos dentro de struct.
        // Sintaxe dos campos: name: Type,  (Rust-style)
        if (current.type == TOKEN_STRUCT) {
            int tokLine = current.line, tokCol = current.col;
            current = nextToken();
            std::string sName = eat(TOKEN_IDENT).value;
            declaredStructNames.insert(sName);
            eat(TOKEN_LBRACE);
            std::vector<StructField> fields;
            while (current.type != TOKEN_RBRACE) {
                if (current.type == TOKEN_EOF)
                    reportError(sourceFile, tokLine, tokCol,
                                "unterminated struct '" + sName + "' — missing '}'",
                                getSourceLine(tokLine), (int)sName.size());

                // Detectar método inline → erro explícito
                if (current.type == TOKEN_FN) {
                    std::string ln = getSourceLine(current.line);
                    reportError(sourceFile, current.line, current.col,
                                "methods are not allowed inside struct definitions",
                                ln, 2,
                                "define methods in a separate impl block:\n"
                                "  impl " + sName + " {\n"
                                "      fn method(&self) -> void { ... }\n"
                                "  }");
                }

                // Campo: name: Type,
                // Também aceita sintaxe legada `Type name;` para compatibilidade
                if (current.type == TOKEN_IDENT) {
                    LexerState st = saveLexerState();
                    Token savedTok = current;
                    std::string maybeName = current.value;
                    current = nextToken();
                    if (current.type == TOKEN_COLON) {
                        // Rust-style: name: Type,
                        current = nextToken();
                        DataType ft = parseDataType();
                        std::string ftStructName = (ft == DataType::Custom) ? lastCustomTypeName : "";
                        if (current.type == TOKEN_COMMA) current = nextToken();
                        else if (current.type == TOKEN_SEMI) current = nextToken();
                        fields.push_back({ft, maybeName, ftStructName});
                        continue;
                    }
                    // Pode ser tipo legado (ex: `Vec2 pos;`)
                    if (current.type == TOKEN_IDENT) {
                        std::string fieldName = current.value;
                        current = nextToken();
                        DataType ft = DataType::Custom;
                        std::string ftStructName = maybeName;
                        if (current.type == TOKEN_SEMI) current = nextToken();
                        else if (current.type == TOKEN_COMMA) current = nextToken();
                        fields.push_back({ft, fieldName, ftStructName});
                        continue;
                    }
                    restoreLexerState(st);
                    current = savedTok;
                }

                // Tipos primitivos em estilo legado: `int x;`
                if (current.type == TOKEN_INT    || current.type == TOKEN_FLOAT  ||
                    current.type == TOKEN_STRING  || current.type == TOKEN_DOUBLE ||
                    current.type == TOKEN_LONG    || current.type == TOKEN_CHAR   ||
                    current.type == TOKEN_BOOL    || current.type == TOKEN_VOID) {
                    DataType ft = parseDataType();
                    std::string ftStructName = (ft == DataType::Custom) ? lastCustomTypeName : "";
                    std::string mName = eat(TOKEN_IDENT).value;
                    if (current.type == TOKEN_SEMI) current = nextToken();
                    else if (current.type == TOKEN_COMMA) current = nextToken();
                    fields.push_back({ft, mName, ftStructName});
                    continue;
                }

                reportError(sourceFile, current.line, current.col,
                            "unexpected '" + current.value + "' in struct '" + sName + "'",
                            getSourceLine(current.line), std::max(1,(int)current.value.size()),
                            "struct fields use: name: Type,  (e.g.  x: i32,)");
            }
            eat(TOKEN_RBRACE);
            program.declarations.push_back(
                std::make_unique<StructDefNode>(sName, std::move(fields), std::vector<StructMethod>{}, tokLine, tokCol));
            continue;
        }

        // ── impl ──────────────────────────────────────────────────────────────
        if (current.type == TOKEN_IMPL) {
            int tokLine = current.line, tokCol = current.col;
            current = nextToken();
            parseImpl(program, tokLine, tokCol);
            continue;
        }

        // ── fn (função top-level) ─────────────────────────────────────────────
        // Sintaxe: fn name(params) -> RetType { body }
        if (current.type == TOKEN_FN) {
            int tokLine = current.line, tokCol = current.col;
            current = nextToken();
            std::string name = eat(TOKEN_IDENT).value;
            
            // Setup function context before parsing params and body
            immutableVars.clear();
            mutableReferences.clear();

            auto params = parseFnParams(name, tokLine, tokCol);
            DataType retType = parseFnReturn();
            std::string retStructType = (retType == DataType::Custom) ? lastCustomTypeName : "";

            if (current.type == TOKEN_SEMI)
                reportError(sourceFile, current.line, current.col,
                            "function '" + name + "' has no body",
                            getSourceLine(current.line), 1,
                            "add the function body: { ... }  — "
                            "for forward declarations, use a .nh header file");

            declaredFunctionNames.insert(name);
            currentReturnType = retType;

            // Registra assinatura para validação de call sites
            {
                std::vector<FnParamInfo> sig;
                for (const auto& p : params) sig.push_back(buildParamInfo(p));
                fnSignatures[name] = std::move(sig);
            }
            
            // Parameters are immutable by default
            for (const auto& p : params) {
                std::string pName = p.name;
                if (pName.substr(0, 7) == "__arr__") {
                    auto sep = pName.find("__", 7);
                    if (sep != std::string::npos) pName = pName.substr(sep + 2);
                }
                if (p.type == DataType::Custom && p.structTypeName.substr(0, 5) == "&mut ") {
                    mutableReferences.insert(pName);
                }
                immutableVars.insert(pName);
            }

            program.declarations.push_back(
                std::make_unique<FunctionNode>(retType, retStructType, name,
                                               std::move(params), parseBlock()));
            
            currentReturnType = DataType::Void;
            continue;
        }

        // ── Variável global: let [mut] name: Type = value; ────────────────────
        if (current.type == TOKEN_LET) {
            int tokLine = current.line, tokCol = current.col;
            current = nextToken();
            bool isMut = false;
            if (current.type == TOKEN_MUT) { isMut = true; current = nextToken(); }
            std::string name = eat(TOKEN_IDENT).value;
            eat(TOKEN_COLON);
            DataType type = parseDataType();
            std::string customTypeName = (type == DataType::Custom) ? lastCustomTypeName : "";

            if (type == DataType::Custom) {
                // Global struct var
                if (!isMut) immutableVars.insert(name);
                declaredVarNames.insert(name);
                if (current.type == TOKEN_ASSIGN) {
                    current = nextToken();
                    if (current.type == TOKEN_IDENT) {
                        std::string callName = current.value;
                        current = nextToken();
                        if (current.type == TOKEN_LPAREN) {
                            current = nextToken();
                            std::vector<NodePtr> args;
                            while (current.type != TOKEN_RPAREN) {
                                if (current.type == TOKEN_EOF)
                                    reportError(sourceFile, tokLine, tokCol,
                                        "unterminated argument list for '" + callName + "()'",
                                        getSourceLine(tokLine), (int)callName.size(), "add ')'");
                                args.push_back(parseExpr());
                                if (current.type == TOKEN_COMMA) current = nextToken();
                            }
                            eat(TOKEN_RPAREN); eat(TOKEN_SEMI);
                            declaredVarStructType[name] = customTypeName;
                            program.declarations.push_back(std::make_unique<StructVarDeclNode>(
                                customTypeName, name, callName, std::move(args), isMut, tokLine, tokCol));
                            continue;
                        }
                        eat(TOKEN_SEMI);
                        declaredVarStructType[name] = customTypeName;
                        program.declarations.push_back(std::make_unique<StructVarDeclNode>(
                            customTypeName, name, "@copy:" + callName,
                            std::vector<NodePtr>{}, isMut, tokLine, tokCol));
                        continue;
                    }
                }
                eat(TOKEN_SEMI);
                declaredVarStructType[name] = customTypeName;
                program.declarations.push_back(
                    std::make_unique<StructVarDeclNode>(customTypeName, name, isMut, tokLine, tokCol));
                continue;
            }

            if (current.type == TOKEN_LBRACKET) {
                current = nextToken();
                int sizeLine = current.line, sizeCol = current.col;
                int size = safeStoi(eat(TOKEN_INT_LIT).value, sizeLine, sizeCol);
                if (size <= 0)
                    reportError(sourceFile, sizeLine, sizeCol,
                                "global array '" + name + "' size must be positive",
                                getSourceLine(sizeLine), 1);
                eat(TOKEN_RBRACKET);
                std::vector<NodePtr> init;
                if (current.type == TOKEN_ASSIGN) {
                    current = nextToken(); eat(TOKEN_LBRACE);
                    while (current.type != TOKEN_RBRACE) {
                        if (current.type == TOKEN_EOF)
                            reportError(sourceFile, tokLine, tokCol,
                                "unterminated initializer for global array '" + name + "'",
                                getSourceLine(tokLine), (int)name.size(), "add '}'");
                        init.push_back(parseExpr());
                        if (current.type == TOKEN_COMMA) current = nextToken();
                    }
                    eat(TOKEN_RBRACE);
                }
                eat(TOKEN_SEMI);
                arraySizes[name] = size;
                if (!isMut) immutableVars.insert(name);
                declaredVarNames.insert(name);
                program.declarations.push_back(
                    std::make_unique<ArrayDeclNode>(type, name, size, std::move(init), tokLine, tokCol, isMut));
                continue;
            }

            NodePtr init = nullptr;
            if (current.type == TOKEN_ASSIGN) { current = nextToken(); init = parseExpr(); }
            eat(TOKEN_SEMI);
            if (!isMut) immutableVars.insert(name);
            declaredVarNames.insert(name);
            program.declarations.push_back(
                std::make_unique<VarDeclNode>(type, name, std::move(init), isMut, tokLine, tokCol));
            continue;
        }

        // ── namespace (apenas em top-level, para compatibilidade) ────────────
        if (current.type == TOKEN_NAMESPACE) {
            current = nextToken();
            // Ignorado a nível de AST — usado só em headers .nh
            if (current.type == TOKEN_IDENT) current = nextToken();
            if (current.type == TOKEN_SEMI)  current = nextToken();
            continue;
        }

        // ── Tipos primitivos no top-level são ERRO — obriga uso de fn ──────────
        if (current.type == TOKEN_INT    || current.type == TOKEN_FLOAT  ||
            current.type == TOKEN_STRING || current.type == TOKEN_VOID   ||
            current.type == TOKEN_DOUBLE || current.type == TOKEN_LONG   ||
            current.type == TOKEN_CHAR   || current.type == TOKEN_BOOL) {
            std::string typeName = current.value;
            int tl = current.line, tc = current.col;
            current = nextToken();
            std::string name = (current.type == TOKEN_IDENT) ? current.value : "name";
            std::string ln = getSourceLine(tl);
            reportError(sourceFile, tl, tc,
                "function declarations require the 'fn' keyword",
                ln, (int)typeName.size(),
                "use: fn " + name + "(params) -> " + typeName + " { ... }");
        }

        // ── Erro top-level ────────────────────────────────────────────────────
        {
            std::string hint = didYouMean(current.value, declaredFunctionNames);
            if (hint.empty())
                hint = "top-level declarations must be: fn, struct, impl, let, or #include";
            reportError(sourceFile, current.line, current.col,
                        "unexpected '" + current.value + "' at top level",
                        getSourceLine(current.line), std::max(1,(int)current.value.size()), hint);
        }
    }
    return program;
}