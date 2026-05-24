// analysis.cpp — Analisador estático da linguagem Nova
//
// Fases executadas (nesta ordem, sobre a AST):
//   1. VarInitChecker   — uso de variável não inicializada
//   2. DivZeroChecker   — divisão por zero literal em tempo de compilação
//   3. BoundsChecker    — acesso a array com índice literal fora dos bounds
//   4. DerefChecker     — valida desreferenciacao (*) em alvos invalidos
//   5. BorrowChecker    — borrow duplo (&mut simultâneo) e uso após move (NLL simplificado)
//   6. LeakChecker      — variável local nunca retornada/passada/atribuída para fora do escopo
//
// Cada fase é uma struct independente com método run(stmts, ...).
// A função pública runAnalysis() orquestra tudo.

#include "analysis.h"
#include "../include/error.h"
#include "../include/lexer.h"   // getSourceLine()

#include <map>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>

// ── Macros de cor (mesmas do codegen/main) ────────────────────────────────────
#define ANSI_WARN   "\033[1;33m"
#define ANSI_NOTE   "\033[1;36m"
#define ANSI_BOLD   "\033[1m"
#define ANSI_RESET  "\033[0m"

static std::string g_sourceFile;

// ── emitWarning / emitNote ────────────────────────────────────────────────────
static void emitWarning(int line, int col,
                        const std::string& msg,
                        const std::string& hint = "") {
    std::string src = (line > 0) ? getSourceLine(line) : "";
    int tlen = 1;
    std::cerr << ANSI_BOLD << g_sourceFile << ":" << line << ":" << col << ": "
              << ANSI_WARN << "warning: " << ANSI_RESET
              << ANSI_BOLD << msg << ANSI_RESET << "\n";
    if (!src.empty()) {
        std::cerr << "  " << line << " | " << src << "\n";
        std::string pad(std::to_string(line).size() + 3, ' ');
        std::cerr << pad << "| ";
        for (int i = 1; i < col; i++) std::cerr << ' ';
        std::cerr << ANSI_WARN << "^" << ANSI_RESET << "\n";
    }
    if (!hint.empty())
        std::cerr << ANSI_NOTE << "  note: " << ANSI_RESET << hint << "\n";
}

static void emitError(int line, int col,
                      const std::string& msg,
                      const std::string& hint = "") {
    std::string src = (line > 0) ? getSourceLine(line) : "";
    int tlen = std::max(1, col);
    reportError(g_sourceFile, line, col, msg, src, tlen, hint);
}

// ═════════════════════════════════════════════════════════════════════════════
// Utilitários de AST
// ═════════════════════════════════════════════════════════════════════════════

// Coleta todos os nomes de variáveis que aparecem como VarNode dentro de uma expr.
static void collectVarRefs(const ASTNode* node, std::set<std::string>& out) {
    if (!node) return;
    if (auto* v = dynamic_cast<const VarNode*>(node)) {
        out.insert(v->name);
        return;
    }
    if (auto* b = dynamic_cast<const BinaryOpNode*>(node)) {
        collectVarRefs(b->left.get(), out);
        collectVarRefs(b->right.get(), out);
        return;
    }
    if (auto* u = dynamic_cast<const UnaryOpNode*>(node)) {
        collectVarRefs(u->operand.get(), out);
        return;
    }
    if (auto* c = dynamic_cast<const CallNode*>(node)) {
        for (auto& a : c->args) collectVarRefs(a.get(), out);
        return;
    }
    if (auto* ca = dynamic_cast<const CastNode*>(node)) {
        collectVarRefs(ca->expr.get(), out);
        return;
    }
    if (auto* d = dynamic_cast<const DerefNode*>(node)) {
        collectVarRefs(d->operand.get(), out);
        return;
    }
    if (auto* fa = dynamic_cast<const FieldAccessNode*>(node)) {
        // "self.side" → extrai só a raiz (antes do primeiro '.')
        auto dot = fa->varName.find('.');
        out.insert(dot == std::string::npos ? fa->varName : fa->varName.substr(0, dot));
        return;
    }
    if (auto* mc = dynamic_cast<const MethodCallNode*>(node)) {
        // "rect.area()" → marca o receiver (raiz) como usado
        auto dot = mc->varName.find('.');
        out.insert(dot == std::string::npos ? mc->varName : mc->varName.substr(0, dot));
        for (auto& a : mc->args) collectVarRefs(a.get(), out);
        return;
    }
    if (auto* aa = dynamic_cast<const ArrayAccessNode*>(node)) {
        out.insert(aa->name);
        collectVarRefs(aa->index.get(), out);
        return;
    }
}

// Retorna true se a expressão é um IntLitNode com valor == 0.
static bool isZeroLit(const ASTNode* node) {
    if (auto* i = dynamic_cast<const IntLitNode*>(node)) return i->value == 0;
    return false;
}

// Tenta extrair valor literal inteiro de uma expressão (retorna false se não for literal).
static bool tryGetIntLit(const ASTNode* node, long long& out) {
    if (auto* i = dynamic_cast<const IntLitNode*>(node))  { out = i->value; return true; }
    if (auto* l = dynamic_cast<const LongLitNode*>(node)) { out = l->value; return true; }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// FASE 1 — VarInitChecker
// Detecta: uso de variável local declarada sem inicializador antes de qualquer atribuição.
//
// Estratégia NLL simplificada:
//   - Mantém um conjunto "uninit" de variáveis ainda não inicializadas.
//   - Quando encontra VarDeclNode sem init → adiciona ao conjunto.
//   - Quando encontra VarAssignNode (= ou +=etc) → remove do conjunto (marca init).
//   - Quando encontra qualquer VarNode numa expressão → checa se está em "uninit".
//   - Nos branches if/else: só remove de "uninit" se AMBOS os branches inicializam.
// ═════════════════════════════════════════════════════════════════════════════
struct VarInitChecker {
    // varName → {line, col} da declaração
    std::map<std::string, std::pair<int,int>> uninit;

    void checkExpr(const ASTNode* node) {
        if (!node) return;
        if (auto* v = dynamic_cast<const VarNode*>(node)) {
            auto it = uninit.find(v->name);
            if (it != uninit.end()) {
                emitError(v->line, v->col,
                    "use of possibly uninitialized variable '" + v->name + "'",
                    "assign a value before using it: let mut " + v->name + ": <type> = <value>;");
            }
            return;
        }
        if (auto* b = dynamic_cast<const BinaryOpNode*>(node)) {
            checkExpr(b->left.get()); checkExpr(b->right.get()); return;
        }
        if (auto* u = dynamic_cast<const UnaryOpNode*>(node)) {
            checkExpr(u->operand.get()); return;
        }
        if (auto* c = dynamic_cast<const CallNode*>(node)) {
            for (auto& a : c->args) checkExpr(a.get());
            return;
        }
        if (auto* ca = dynamic_cast<const CastNode*>(node)) {
            checkExpr(ca->expr.get()); return;
        }
        if (auto* fa = dynamic_cast<const FieldAccessNode*>(node)) {
            // Acesso a campo: a variável base deve estar init
            if (uninit.count(fa->varName))
                emitError(fa->line, fa->col,
                    "use of possibly uninitialized variable '" + fa->varName + "'",
                    "initialize '" + fa->varName + "' before accessing its fields");
            return;
        }
        if (auto* aa = dynamic_cast<const ArrayAccessNode*>(node)) {
            if (uninit.count(aa->name))
                emitError(aa->line, aa->col,
                    "use of possibly uninitialized array '" + aa->name + "'", "");
            checkExpr(aa->index.get());
            return;
        }
    }

    // Roda sobre uma lista de statements.
    // Retorna o conjunto de variáveis que foram inicializadas neste bloco
    // (usado para merge em if/else).
    std::set<std::string> run(const std::vector<NodePtr>& stmts) {
        std::set<std::string> initializedHere;

        for (auto& s : stmts) {
            ASTNode* node = s.get();

            // Declaração sem init → uninit
            if (auto* vd = dynamic_cast<const VarDeclNode*>(node)) {
                if (!vd->init) {
                    uninit[vd->name] = {vd->line, vd->col};
                } else {
                    checkExpr(vd->init.get());
                    uninit.erase(vd->name);
                    initializedHere.insert(vd->name);
                }
                continue;
            }

            // Atribuição → inicializa
            if (auto* va = dynamic_cast<const VarAssignNode*>(node)) {
                // O lado direito pode referenciar a própria var (x += 1) —
                // checa ANTES de marcar como init (x += 1 quando x não está init é erro)
                if (va->op != "=" && uninit.count(va->name))
                    emitError(va->line, va->col,
                        "use of uninitialized variable '" + va->name + "' in compound assignment",
                        "use '=' for the first assignment");
                checkExpr(va->expr.get());
                uninit.erase(va->name);
                initializedHere.insert(va->name);
                continue;
            }
            if (auto* da = dynamic_cast<const DerefAssignNode*>(node)) {
                checkExpr(da->target.get());
                checkExpr(da->value.get());
                continue;
            }

            // if/else — merge de ambos os lados
            if (auto* ifn = dynamic_cast<const IfNode*>(node)) {
                checkExpr(ifn->condition.get());

                // Salva snapshot e roda then
                auto savedUninit = uninit;
                auto thenInit = run(ifn->thenBlock);

                // Restaura e roda else
                auto afterThen = uninit;
                uninit = savedUninit;
                auto elseInit = run(ifn->elseBlock);

                // Só considera inicializada se AMBOS os branches inicializam
                uninit = afterThen; // começa com o estado after-then
                for (auto& name : elseInit) {
                    if (thenInit.count(name)) {
                        uninit.erase(name);           // ambos init → não é mais uninit
                        initializedHere.insert(name);
                    }
                }
                continue;
            }

            // while / for — não garantem execução, então não removemos uninit
            if (auto* wh = dynamic_cast<const WhileNode*>(node)) {
                checkExpr(wh->condition.get());
                auto saved = uninit;
                run(wh->body);
                uninit = saved; // conservador: loop pode não executar
                continue;
            }
            if (auto* fn = dynamic_cast<const ForNode*>(node)) {
                auto saved = uninit;
                if (fn->init) {
                    // O init do for pode ser um VarDeclNode ou assignment
                    std::vector<NodePtr> initVec;
                    // Não podemos mover o nó — criamos um view manual
                    run_single(fn->init.get(), initializedHere);
                }
                checkExpr(fn->condition.get());
                run(fn->body);
                checkExpr(fn->step.get());
                uninit = saved;
                continue;
            }

            // Return — checa a expr
            if (auto* ret = dynamic_cast<const ReturnNode*>(node)) {
                checkExpr(ret->expr.get()); continue;
            }

            // Print
            if (auto* pr = dynamic_cast<const PrintNode*>(node)) {
                checkExpr(pr->expr.get()); continue;
            }

            // Chamada de método: checa args
            if (auto* mc = dynamic_cast<const MethodCallNode*>(node)) {
                if (uninit.count(mc->varName))
                    emitError(mc->line, mc->col,
                        "use of uninitialized variable '" + mc->varName + "' as method receiver", "");
                for (auto& a : mc->args) checkExpr(a.get());
                continue;
            }

            // Atribuição de campo
            if (auto* fa = dynamic_cast<const FieldAssignNode*>(node)) {
                checkExpr(fa->value.get()); continue;
            }

            // Acesso genérico a expressão (inclui CallNode standalone, CastNode, etc.)
            checkExpr(node);
        }
        return initializedHere;
    }

    // Roda um único nó (usado para init do for)
    void run_single(const ASTNode* node, std::set<std::string>& initializedHere) {
        if (!node) return;
        if (auto* vd = dynamic_cast<const VarDeclNode*>(node)) {
            if (!vd->init) uninit[vd->name] = {vd->line, vd->col};
            else { checkExpr(vd->init.get()); uninit.erase(vd->name); initializedHere.insert(vd->name); }
            return;
        }
        if (auto* va = dynamic_cast<const VarAssignNode*>(node)) {
            checkExpr(va->expr.get());
            uninit.erase(va->name);
            initializedHere.insert(va->name);
            return;
        }
        checkExpr(node);
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// FASE 2 — DivZeroChecker
// Detecta divisão (/) ou módulo (%) onde o operando direito é o literal 0.
// ═════════════════════════════════════════════════════════════════════════════
struct DivZeroChecker {
    void checkExpr(const ASTNode* node) {
        if (!node) return;
        if (auto* b = dynamic_cast<const BinaryOpNode*>(node)) {
            if ((b->op == "/" || b->op == "%") && isZeroLit(b->right.get())) {
                emitError(b->line, b->col,
                    "division by zero",
                    b->op == "/" ? "the divisor is the literal 0"
                                 : "modulo by zero is undefined behavior");
            }
            checkExpr(b->left.get());
            checkExpr(b->right.get());
            return;
        }
        if (auto* u = dynamic_cast<const UnaryOpNode*>(node)) { checkExpr(u->operand.get()); return; }
        if (auto* c = dynamic_cast<const CallNode*>(node))    { for (auto& a : c->args) checkExpr(a.get()); return; }
        if (auto* ca = dynamic_cast<const CastNode*>(node))   { checkExpr(ca->expr.get()); return; }
        if (auto* aa = dynamic_cast<const ArrayAccessNode*>(node)) { checkExpr(aa->index.get()); return; }
    }

    void run(const std::vector<NodePtr>& stmts) {
        for (auto& s : stmts) {
            ASTNode* node = s.get();
            if (auto* vd = dynamic_cast<const VarDeclNode*>(node))    { checkExpr(vd->init.get()); continue; }
            if (auto* va = dynamic_cast<const VarAssignNode*>(node))  { checkExpr(va->expr.get()); continue; }
            if (auto* da = dynamic_cast<const DerefAssignNode*>(node)) {
                checkExpr(da->target.get()); checkExpr(da->value.get()); continue;
            }
            if (auto* ret = dynamic_cast<const ReturnNode*>(node))    { checkExpr(ret->expr.get()); continue; }
            if (auto* pr  = dynamic_cast<const PrintNode*>(node))     { checkExpr(pr->expr.get()); continue; }
            if (auto* ifn = dynamic_cast<const IfNode*>(node)) {
                checkExpr(ifn->condition.get());
                run(ifn->thenBlock); run(ifn->elseBlock); continue;
            }
            if (auto* wh = dynamic_cast<const WhileNode*>(node)) {
                checkExpr(wh->condition.get()); run(wh->body); continue;
            }
            if (auto* fn = dynamic_cast<const ForNode*>(node)) {
                checkExpr(fn->init.get()); checkExpr(fn->condition.get());
                checkExpr(fn->step.get()); run(fn->body); continue;
            }
            if (auto* mc = dynamic_cast<const MethodCallNode*>(node)) {
                for (auto& a : mc->args) checkExpr(a.get());
                continue;
            }
            if (auto* fa = dynamic_cast<const FieldAssignNode*>(node)) { checkExpr(fa->value.get()); continue; }
            checkExpr(node);
        }
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// FASE 3 — BoundsChecker
// Detecta acesso a array com índice literal fora dos bounds conhecidos.
// Os tamanhos são coletados das declarações ArrayDeclNode dentro da função.
// ═════════════════════════════════════════════════════════════════════════════
struct BoundsChecker {
    // nome → tamanho declarado
    std::map<std::string, int> arraySizes;

    void checkExpr(const ASTNode* node) {
        if (!node) return;
        if (auto* aa = dynamic_cast<const ArrayAccessNode*>(node)) {
            long long idx;
            if (tryGetIntLit(aa->index.get(), idx)) {
                auto it = arraySizes.find(aa->name);
                if (it != arraySizes.end()) {
                    if (idx < 0 || idx >= (long long)it->second) {
                        emitError(aa->line, aa->col,
                            "array index " + std::to_string(idx) +
                            " is out of bounds for '" + aa->name +
                            "' (size " + std::to_string(it->second) + ")",
                            "valid indices are 0.." + std::to_string(it->second - 1));
                    }
                }
            }
            checkExpr(aa->index.get());
            return;
        }
        if (auto* b = dynamic_cast<const BinaryOpNode*>(node))  { checkExpr(b->left.get()); checkExpr(b->right.get()); return; }
        if (auto* u = dynamic_cast<const UnaryOpNode*>(node))   { checkExpr(u->operand.get()); return; }
        if (auto* c = dynamic_cast<const CallNode*>(node))      { for (auto& a : c->args) checkExpr(a.get()); return; }
        if (auto* ca = dynamic_cast<const CastNode*>(node))     { checkExpr(ca->expr.get()); return; }
    }

    void run(const std::vector<NodePtr>& stmts) {
        for (auto& s : stmts) {
            ASTNode* node = s.get();
            if (auto* ad = dynamic_cast<const ArrayDeclNode*>(node)) {
                arraySizes[ad->name] = ad->size;
                for (auto& init : ad->init) checkExpr(init.get());
                continue;
            }
            if (auto* aa = dynamic_cast<const ArrayAssignNode*>(node)) {
                long long idx;
                if (tryGetIntLit(aa->index.get(), idx)) {
                    auto it = arraySizes.find(aa->name);
                    if (it != arraySizes.end() && (idx < 0 || idx >= (long long)it->second))
                        emitError(aa->line, aa->col,
                            "array index " + std::to_string(idx) +
                            " is out of bounds for '" + aa->name +
                            "' (size " + std::to_string(it->second) + ")",
                            "valid indices are 0.." + std::to_string(it->second - 1));
                }
                checkExpr(aa->value.get()); continue;
            }
            if (auto* vd = dynamic_cast<const VarDeclNode*>(node))    { checkExpr(vd->init.get()); continue; }
            if (auto* va = dynamic_cast<const VarAssignNode*>(node))  { checkExpr(va->expr.get()); continue; }
            if (auto* da = dynamic_cast<const DerefAssignNode*>(node)) {
                checkExpr(da->target.get()); checkExpr(da->value.get()); continue;
            }
            if (auto* ret = dynamic_cast<const ReturnNode*>(node))    { checkExpr(ret->expr.get()); continue; }
            if (auto* pr  = dynamic_cast<const PrintNode*>(node))     { checkExpr(pr->expr.get()); continue; }
            if (auto* ifn = dynamic_cast<const IfNode*>(node)) {
                checkExpr(ifn->condition.get());
                run(ifn->thenBlock); run(ifn->elseBlock); continue;
            }
            if (auto* wh = dynamic_cast<const WhileNode*>(node)) {
                checkExpr(wh->condition.get()); run(wh->body); continue;
            }
            if (auto* fn = dynamic_cast<const ForNode*>(node)) {
                checkExpr(fn->init.get()); checkExpr(fn->condition.get());
                checkExpr(fn->step.get()); run(fn->body); continue;
            }
            if (auto* mc = dynamic_cast<const MethodCallNode*>(node)) {
                for (auto& a : mc->args) checkExpr(a.get()); continue;
            }
            checkExpr(node);
        }
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// FASE 4 — DerefChecker
// Valida uso de '*' com regras conservadoras estilo Rust:
//   - proibe desreferenciar literais/expressoes arbitrarias
//   - proibe desreferenciar ponteiro nulo literal (0)
// ═════════════════════════════════════════════════════════════════════════════
struct DerefChecker {
    static bool isNullLiteral(const ASTNode* node) {
        if (auto* i = dynamic_cast<const IntLitNode*>(node))  return i->value == 0;
        if (auto* l = dynamic_cast<const LongLitNode*>(node)) return l->value == 0;
        return false;
    }

    static bool isBorrowExpr(const ASTNode* node) {
        auto* u = dynamic_cast<const UnaryOpNode*>(node);
        return u && (u->op == "&" || u->op == "&mut");
    }

    static bool isValidDerefOperand(const ASTNode* node) {
        // Regra conservadora: so aceita algo que "parece ponteiro"
        // (var, borrow, outro deref, cast) para evitar '*42', '*(a+b)', etc.
        return dynamic_cast<const VarNode*>(node)   != nullptr ||
               dynamic_cast<const DerefNode*>(node) != nullptr ||
               dynamic_cast<const CastNode*>(node)  != nullptr ||
               isBorrowExpr(node);
    }

    void checkExpr(const ASTNode* node) {
        if (!node) return;

        if (auto* d = dynamic_cast<const DerefNode*>(node)) {
            const ASTNode* op = d->operand.get();
            if (!op) return;

            if (isNullLiteral(op)) {
                emitError(d->line, d->col,
                    "cannot dereference a null pointer literal",
                    "0 is not a valid address for dereference");
            } else if (!isValidDerefOperand(op)) {
                emitError(d->line, d->col,
                    "invalid dereference target",
                    "only pointer-like values can be dereferenced (e.g. *ptr, *&value)");
            }

            checkExpr(op);
            return;
        }

        if (auto* b = dynamic_cast<const BinaryOpNode*>(node)) {
            checkExpr(b->left.get()); checkExpr(b->right.get()); return;
        }
        if (auto* u = dynamic_cast<const UnaryOpNode*>(node)) {
            checkExpr(u->operand.get()); return;
        }
        if (auto* c = dynamic_cast<const CallNode*>(node)) {
            for (auto& a : c->args) checkExpr(a.get());
            return;
        }
        if (auto* ca = dynamic_cast<const CastNode*>(node)) {
            checkExpr(ca->expr.get()); return;
        }
        if (auto* aa = dynamic_cast<const ArrayAccessNode*>(node)) {
            checkExpr(aa->index.get()); return;
        }
    }

    void run(const std::vector<NodePtr>& stmts) {
        for (auto& s : stmts) {
            ASTNode* node = s.get();
            if (auto* vd = dynamic_cast<const VarDeclNode*>(node))    { checkExpr(vd->init.get()); continue; }
            if (auto* va = dynamic_cast<const VarAssignNode*>(node))  { checkExpr(va->expr.get()); continue; }
            if (auto* da = dynamic_cast<const DerefAssignNode*>(node)) {
                checkExpr(da->target.get()); checkExpr(da->value.get()); continue;
            }
            if (auto* ret = dynamic_cast<const ReturnNode*>(node))    { checkExpr(ret->expr.get()); continue; }
            if (auto* pr  = dynamic_cast<const PrintNode*>(node))     { checkExpr(pr->expr.get()); continue; }
            if (auto* ifn = dynamic_cast<const IfNode*>(node)) {
                checkExpr(ifn->condition.get());
                run(ifn->thenBlock); run(ifn->elseBlock); continue;
            }
            if (auto* wh = dynamic_cast<const WhileNode*>(node)) {
                checkExpr(wh->condition.get()); run(wh->body); continue;
            }
            if (auto* fn = dynamic_cast<const ForNode*>(node)) {
                checkExpr(fn->init.get()); checkExpr(fn->condition.get());
                checkExpr(fn->step.get()); run(fn->body); continue;
            }
            if (auto* mc = dynamic_cast<const MethodCallNode*>(node)) {
                for (auto& a : mc->args) checkExpr(a.get());
                continue;
            }
            if (auto* fa = dynamic_cast<const FieldAssignNode*>(node)) { checkExpr(fa->value.get()); continue; }
            checkExpr(node);
        }
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// FASE 5 — BorrowChecker (NLL Real)
//
// Modelo de ownership da Nova:
//   - Toda variável tem exatamente um dono (owner).
//   - Pode existir qualquer número de borrows imutáveis (&) OU exatamente um borrow
//     mutável (&mut) — mas nunca os dois ao mesmo tempo.
//   - "Non-lexical" (NLL): O tempo de vida de uma referência termina 
//     exatamente na última linha de execução em que ela é utilizada.
// ═════════════════════════════════════════════════════════════════════════════
struct BorrowChecker {

    enum class BorrowKind { None, Shared, Mutable };

    struct Borrow {
        BorrowKind kind;
        int line, col;
        std::string borrower;
    };

    // varName (ex: 's') -> lista de empréstimos ativos sobre ela
    std::map<std::string, std::vector<Borrow>> activeBorrows;
    std::map<std::string, std::pair<int,int>> moved;
    std::set<std::string> structVarNames;

    // ── ESTADO NLL ───────────────────────────────────────────────────
    std::map<std::string, int> lastUseIndex;
    int globalIndex = 0;
    int execIndex = 0;

    // Passada 1: Mapeia o "futuro" para saber a última vez que cada variável aparece
    void recordUses(const ASTNode* node) {
        if (!node) return;
        
        // Coleta todas as variáveis lidas nesta expressão
        std::set<std::string> refs;
        collectVarRefs(node, refs);
        for (auto& r : refs) lastUseIndex[r] = globalIndex;

        // Registra nós de atribuição e blocos internos
        if (auto* ifn = dynamic_cast<const IfNode*>(node)) {
            prePass(ifn->thenBlock);
            prePass(ifn->elseBlock);
        } else if (auto* wh = dynamic_cast<const WhileNode*>(node)) {
            prePass(wh->body);
        } else if (auto* fn = dynamic_cast<const ForNode*>(node)) {
            recordUses(fn->init.get());
            recordUses(fn->step.get());
            prePass(fn->body);
        } else if (auto* va = dynamic_cast<const VarAssignNode*>(node)) {
            lastUseIndex[va->name] = globalIndex;
        } else if (auto* fa = dynamic_cast<const FieldAssignNode*>(node)) {
            lastUseIndex[fa->varName] = globalIndex;
        } else if (auto* aa = dynamic_cast<const ArrayAssignNode*>(node)) {
            lastUseIndex[aa->name] = globalIndex;
        } else if (auto* sa = dynamic_cast<const StructAssignNode*>(node)) {
            lastUseIndex[sa->varName] = globalIndex;
            if (!sa->srcVar.empty()) lastUseIndex[sa->srcVar] = globalIndex;
        } else if (auto* vd = dynamic_cast<const VarDeclNode*>(node)) {
            lastUseIndex[vd->name] = globalIndex;
            recordUses(vd->init.get());
        } else if (auto* svd = dynamic_cast<const StructVarDeclNode*>(node)) {
            lastUseIndex[svd->varName] = globalIndex;
        } else if (auto* evd = dynamic_cast<const EnumVarDeclNode*>(node)) {
            lastUseIndex[evd->varName] = globalIndex;
            // Enum variable declarations don't have initialization expressions to check
        } else if (auto* ad = dynamic_cast<const ArrayDeclNode*>(node)) {
            lastUseIndex[ad->name] = globalIndex;
        }
    }

    void prePass(const std::vector<NodePtr>& stmts) {
        for (auto& s : stmts) {
            globalIndex++;
            recordUses(s.get());
        }
    }
    // ─────────────────────────────────────────────────────────────────

    void registerStructVar(const std::string& name) {
        structVarNames.insert(name);
    }

    void checkUseAfterMove(const std::string& name, int line, int col) {
        auto it = moved.find(name);
        if (it != moved.end()) {
            emitError(line, col,
                "use of moved value '" + name + "'",
                "value was moved at line " + std::to_string(it->second.first) +
                " — you cannot use a value after it has been moved");
        }
    }

    void borrowShared(const std::string& varName, const std::string& borrowerName, int line, int col) {
        checkUseAfterMove(varName, line, col);
        auto& vec = activeBorrows[varName];
        
        for (auto& b : vec) {
            if (b.kind == BorrowKind::Mutable) {
                emitError(line, col,
                    "cannot borrow '" + varName + "' as immutable because it is also borrowed as mutable",
                    "the mutable borrow of '" + varName + "' (by '" + b.borrower +
                    "') is still active at line " + std::to_string(b.line));
                return;
            }
        }
        vec.push_back({BorrowKind::Shared, line, col, borrowerName});
    }

    void borrowMutable(const std::string& varName, const std::string& borrowerName, int line, int col) {
        checkUseAfterMove(varName, line, col);
        auto& vec = activeBorrows[varName];
        
        if (!vec.empty()) {
            if (vec.size() == 1 && vec[0].kind == BorrowKind::Mutable) {
                emitError(line, col,
                    "cannot borrow '" + varName + "' as mutable more than once at a time",
                    "the previous mutable borrow (by '" + vec[0].borrower +
                    "') is still active at line " + std::to_string(vec[0].line));
            } else {
                emitError(line, col,
                    "cannot borrow '" + varName + "' as mutable because it is also borrowed as immutable",
                    "there are " + std::to_string(vec.size()) + " active immutable borrow(s) of '" + varName + "'");
            }
            return;
        }
        vec.push_back({BorrowKind::Mutable, line, col, borrowerName});
    }

    void checkCallArgs(const CallNode* c) {
        for (auto& arg : c->args) {
            // Verifica move de struct
            if (auto* v = dynamic_cast<const VarNode*>(arg.get())) {
                if (structVarNames.count(v->name)) {
                    checkUseAfterMove(v->name, v->line, v->col);
                    moved[v->name] = {v->line, v->col};
                }
            }
            // Verifica &mut passado como argumento enquanto há borrow ativo
            if (auto* uo = dynamic_cast<const UnaryOpNode*>(arg.get())) {
                if (uo->op == "&mut") {
                    if (auto* target = dynamic_cast<const VarNode*>(uo->operand.get())) {
                        borrowMutable(target->name, "<argument>", c->line, c->col);
                        // Após checar, remove o borrow mutável que acabou de ser registrado
                        // (é um borrow temporário — dura só a chamada)
                        auto& vec = activeBorrows[target->name];
                        vec.erase(std::remove_if(vec.begin(), vec.end(), [](const Borrow& b){
                            return b.borrower == "<argument>";
                        }), vec.end());
                        if (vec.empty()) activeBorrows.erase(target->name);
                    }
                } else if (uo->op == "&") {
                    // borrow imutável como argumento: apenas checa use-after-move
                    if (auto* target = dynamic_cast<const VarNode*>(uo->operand.get())) {
                        checkUseAfterMove(target->name, c->line, c->col);
                    }
                }
            }
        }
    }

    void checkExpr(const ASTNode* node) {
        if (!node) return;
        if (auto* v = dynamic_cast<const VarNode*>(node)) {
            checkUseAfterMove(v->name, v->line, v->col);
            return;
        }
        if (auto* b = dynamic_cast<const BinaryOpNode*>(node)) {
            checkExpr(b->left.get()); checkExpr(b->right.get()); return;
        }
        if (auto* u = dynamic_cast<const UnaryOpNode*>(node)) {
            checkExpr(u->operand.get()); return;
        }
        if (auto* c = dynamic_cast<const CallNode*>(node)) {
            checkCallArgs(c);
            for (auto& a : c->args) checkExpr(a.get());
            return;
        }
        if (auto* ca = dynamic_cast<const CastNode*>(node)) {
            checkExpr(ca->expr.get()); return;
        }
        if (auto* fa = dynamic_cast<const FieldAccessNode*>(node)) {
            checkUseAfterMove(fa->varName, fa->line, fa->col); return;
        }
        if (auto* aa = dynamic_cast<const ArrayAccessNode*>(node)) {
            checkUseAfterMove(aa->name, aa->line, aa->col);
            checkExpr(aa->index.get()); return;
        }
    }

    // Passada 2: Valida os comandos usando o conhecimento do NLL
    void run(const std::vector<NodePtr>& stmts) {
        for (auto& s : stmts) {
            execIndex++; // Avança o relógio

            // ── MÁGICA NLL: Expirar borrows mortos ──
            for (auto it = activeBorrows.begin(); it != activeBorrows.end(); ) {
                auto& vec = it->second;
                vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const Borrow& b) {
                    if (b.borrower.empty()) return true; 
                    // Se o último uso do dono da referência já passou, o borrow morre!
                    return lastUseIndex[b.borrower] < execIndex; 
                }), vec.end());
                
                if (vec.empty()) it = activeBorrows.erase(it);
                else ++it;
            }
            // ────────────────────────────────────────

            ASTNode* node = s.get();

            if (auto* svd = dynamic_cast<const StructVarDeclNode*>(node)) {
                registerStructVar(svd->varName);
                continue;
            }

            if (auto* vd = dynamic_cast<const VarDeclNode*>(node)) {
                if (vd->init) {
                    if (auto* uo = dynamic_cast<const UnaryOpNode*>(vd->init.get())) {
                        if (uo->op == "&") {
                            if (auto* target = dynamic_cast<const VarNode*>(uo->operand.get()))
                                borrowShared(target->name, vd->name, vd->line, vd->col);
                        } else if (uo->op == "&mut") {
                            if (auto* target = dynamic_cast<const VarNode*>(uo->operand.get()))
                                borrowMutable(target->name, vd->name, vd->line, vd->col);
                        } else {
                            checkExpr(vd->init.get());
                        }
                    } else {
                        checkExpr(vd->init.get());
                    }
                }
                continue;
            }

            if (auto* va = dynamic_cast<const VarAssignNode*>(node)) {
                checkExpr(va->expr.get());
                // Mutação direta: não pode existir borrow ativo sobre esta variável
                auto it = activeBorrows.find(va->name);
                if (it != activeBorrows.end() && !it->second.empty()) {
                    auto& borrows = it->second;
                    for (auto& b : borrows) {
                        if (b.kind == BorrowKind::Shared) {
                            emitError(va->line, va->col,
                                "cannot assign to '" + va->name + "' because it is borrowed",
                                "immutable borrow ('" + b.borrower + "') is still active — last use is after this assignment");
                            break;
                        } else if (b.kind == BorrowKind::Mutable) {
                            emitError(va->line, va->col,
                                "cannot assign to '" + va->name + "' because it is mutably borrowed",
                                "mutable borrow ('" + b.borrower + "') is still active");
                            break;
                        }
                    }
                }
                moved.erase(va->name);
                continue;
            }
            if (auto* da = dynamic_cast<const DerefAssignNode*>(node)) {
                checkExpr(da->target.get());
                checkExpr(da->value.get());
                continue;
            }

            if (auto* ifn = dynamic_cast<const IfNode*>(node)) {
                checkExpr(ifn->condition.get());
                auto savedBorrows = activeBorrows;
                auto savedMoved   = moved;
                run(ifn->thenBlock);
                auto afterThenMoved = moved;
                activeBorrows = savedBorrows;
                moved   = savedMoved;
                run(ifn->elseBlock);
                for (auto& [name, pos] : afterThenMoved)
                    moved[name] = pos;
                continue;
            }

            if (auto* wh = dynamic_cast<const WhileNode*>(node)) {
                checkExpr(wh->condition.get());
                auto saved = activeBorrows;
                run(wh->body);
                activeBorrows = saved;
                continue;
            }

            if (auto* fn = dynamic_cast<const ForNode*>(node)) {
                checkExpr(fn->init.get());
                checkExpr(fn->condition.get());
                auto saved = activeBorrows;
                run(fn->body);
                checkExpr(fn->step.get());
                activeBorrows = saved;
                continue;
            }

            if (auto* ret = dynamic_cast<const ReturnNode*>(node)) {
                checkExpr(ret->expr.get()); continue;
            }
            if (auto* pr = dynamic_cast<const PrintNode*>(node)) {
                checkExpr(pr->expr.get()); continue;
            }
            if (auto* mc = dynamic_cast<const MethodCallNode*>(node)) {
                checkUseAfterMove(mc->varName, mc->line, mc->col);
                for (auto& a : mc->args) checkExpr(a.get());
                continue;
            }
            if (auto* fa = dynamic_cast<const FieldAssignNode*>(node)) {
                checkUseAfterMove(fa->varName, fa->line, fa->col);
                checkExpr(fa->value.get()); continue;
            }
            if (auto* sa = dynamic_cast<const StructAssignNode*>(node)) {
                if (!sa->srcVar.empty() && structVarNames.count(sa->srcVar)) {
                    checkUseAfterMove(sa->srcVar, sa->line, sa->col);
                    moved[sa->srcVar] = {sa->line, sa->col};
                }
                for (auto& a : sa->funcArgs) checkExpr(a.get());
                moved.erase(sa->varName);
                continue;
            }

            checkExpr(node);
        }
    }
};
// ═════════════════════════════════════════════════════════════════════════════
// FASE 6 — LeakChecker
//
// Detecta variáveis locais de tipo "pesado" (struct, array) que:
//   - São declaradas dentro do escopo da função
//   - Nunca são: retornadas, passadas como argumento, atribuídas para fora
//
// Emite WARNING (não erro) — é heurística, não prova de leak.
// ═════════════════════════════════════════════════════════════════════════════
struct LeakChecker {

    struct VarRecord {
        int line, col;
        bool escaped; // true se passou para fora do escopo
    };

    // Candidatos: structs e arrays locais que podem "vazar"
    std::map<std::string, VarRecord> candidates;

    // Coleta todas as variáveis referenciadas numa expressão como "escaped"
    void markEscaped(const ASTNode* node) {
        std::set<std::string> refs;
        collectVarRefs(node, refs);
        for (auto& r : refs) {
            auto it = candidates.find(r);
            if (it != candidates.end()) it->second.escaped = true;
        }
    }

    void run(const std::vector<NodePtr>& stmts) {
        for (auto& s : stmts) {
            ASTNode* node = s.get();

            // Declaração de struct → candidato
            if (auto* svd = dynamic_cast<const StructVarDeclNode*>(node)) {
                candidates[svd->varName] = {svd->line, svd->col, false};
                // Args do construtor não escapam o struct em si
                continue;
            }

            // Declaração de array → candidato
            if (auto* ad = dynamic_cast<const ArrayDeclNode*>(node)) {
                candidates[ad->name] = {ad->line, ad->col, false};
                continue;
            }

            // Return: qualquer var referenciada escapa
            if (auto* ret = dynamic_cast<const ReturnNode*>(node)) {
                markEscaped(ret->expr.get()); continue;
            }

            // Chamada de função: args escapam
            if (auto* c = dynamic_cast<const CallNode*>(node)) {
                for (auto& a : c->args) markEscaped(a.get()); continue;
            }

            // Atribuição de struct: origem pode estar escapando (sendo copiada para fora)
            if (auto* sa = dynamic_cast<const StructAssignNode*>(node)) {
                if (!sa->srcVar.empty()) {
                    auto it = candidates.find(sa->srcVar);
                    if (it != candidates.end()) it->second.escaped = true;
                }
                for (auto& a : sa->funcArgs) markEscaped(a.get());
                continue;
            }

            // Método: se a var é o receiver, considera escaped
            if (auto* mc = dynamic_cast<const MethodCallNode*>(node)) {
                auto it = candidates.find(mc->varName);
                if (it != candidates.end()) it->second.escaped = true;
                for (auto& a : mc->args) markEscaped(a.get());
                continue;
            }

            // Atribuição de campo: var base escapa (está sendo usada)
            if (auto* fa = dynamic_cast<const FieldAssignNode*>(node)) {
                auto it = candidates.find(fa->varName);
                if (it != candidates.end()) it->second.escaped = true;
                markEscaped(fa->value.get());
                continue;
            }

            // Acesso de campo em expressão
            if (auto* fa = dynamic_cast<const FieldAccessNode*>(node)) {
                auto it = candidates.find(fa->varName);
                if (it != candidates.end()) it->second.escaped = true;
                continue;
            }

            // Declaração de var primitiva com init referenciando candidato
            if (auto* vd = dynamic_cast<const VarDeclNode*>(node)) {
                markEscaped(vd->init.get()); continue;
            }

            if (auto* va = dynamic_cast<const VarAssignNode*>(node)) {
                markEscaped(va->expr.get()); continue;
            }
            if (auto* da = dynamic_cast<const DerefAssignNode*>(node)) {
                markEscaped(da->target.get());
                markEscaped(da->value.get());
                continue;
            }

            // if/else/while/for: percorre sub-blocos
            if (auto* ifn = dynamic_cast<const IfNode*>(node)) {
                markEscaped(ifn->condition.get());
                run(ifn->thenBlock); run(ifn->elseBlock); continue;
            }
            if (auto* wh = dynamic_cast<const WhileNode*>(node)) {
                markEscaped(wh->condition.get()); run(wh->body); continue;
            }
            if (auto* fn = dynamic_cast<const ForNode*>(node)) {
                markEscaped(fn->init.get()); markEscaped(fn->condition.get());
                markEscaped(fn->step.get()); run(fn->body); continue;
            }

            if (auto* pr = dynamic_cast<const PrintNode*>(node)) {
                markEscaped(pr->expr.get()); continue;
            }
        }
    }

    // Emite warnings para os candidatos que nunca escaparam.
    void flush() {
        for (auto& [name, rec] : candidates) {
            if (!rec.escaped) {
                emitWarning(rec.line, rec.col,
                    "value '" + name + "' is allocated but never used or returned",
                    "if this is intentional, consider removing the declaration; "
                    "otherwise make sure you return, pass, or read from '" + name + "'");
            }
        }
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// runAnalysis — ponto de entrada público
// ═════════════════════════════════════════════════════════════════════════════
void runAnalysis(const ProgramNode& program, const std::string& sourceFile) {
    g_sourceFile = sourceFile;

    for (auto& decl : program.declarations) {
        ASTNode* node = decl.get();

        // Só analisamos o corpo de funções (FunctionNode)
        const FunctionNode* fn = dynamic_cast<const FunctionNode*>(node);
        if (!fn) continue;

        const std::vector<NodePtr>& body = fn->body;

        // ── Fase 1: variáveis não inicializadas ───────────────────────────────
        {
            VarInitChecker checker;
            // Parâmetros da função já estão inicializados
            for (auto& p : fn->params)
                checker.uninit.erase(p.name); // garantia: params nunca entram no uninit
            checker.run(body);
        }

        // ── Fase 2: divisão por zero ──────────────────────────────────────────
        {
            DivZeroChecker checker;
            checker.run(body);
        }

        // ── Fase 3: bounds de array ───────────────────────────────────────────
        {
            BoundsChecker checker;
            checker.run(body);
        }

        // ── Fase 4: validação de desreferenciação ─────────────────────────────
        {
            DerefChecker checker;
            checker.run(body);
        }

        // ── Fase 5: borrow checker (NLL simplificado) ─────────────────────────
        {
            BorrowChecker checker;
            checker.prePass(body);
            checker.run(body);
        }

        // ── Fase 6: leak checker ──────────────────────────────────────────────
        {
            LeakChecker checker;
            checker.run(body);
            checker.flush();
        }
    }
}