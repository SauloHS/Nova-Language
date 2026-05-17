#pragma once
#include <string>
#include <memory>
#include <vector>

// ── Tipos de dados ────────────────────────────────────────────────────────────
enum class DataType { Int, Float, String, Void, Long, LongLong, Double, Char, Bool, Custom };

// ── Nó base ───────────────────────────────────────────────────────────────────
struct ASTNode {
    virtual ~ASTNode() = default;
};

using NodePtr = std::unique_ptr<ASTNode>;

// ═══════════════════════════════════════════════════════════════
// LITERAIS
// ═══════════════════════════════════════════════════════════════

struct IntLitNode : ASTNode {
    int value;
    IntLitNode(int v) : value(v) {}
};

struct LongLitNode : ASTNode {
    long long value;
    LongLitNode(long long v) : value(v) {}
};

struct FloatLitNode : ASTNode {
    std::string raw;
    FloatLitNode(const std::string& v) : raw(v) {}
};

struct StringLitNode : ASTNode {
    std::string value;
    StringLitNode(const std::string& v) : value(v) {}
};

struct CharLitNode : ASTNode {
    char value;
    CharLitNode(char v) : value(v) {}
};

struct BoolLitNode : ASTNode {
    bool value;
    BoolLitNode(bool v) : value(v) {}
};

// ═══════════════════════════════════════════════════════════════
// EXPRESSÕES
// ═══════════════════════════════════════════════════════════════

// Referência a variável: a, b, x
struct VarNode : ASTNode {
    std::string name;
    int line, col;
    VarNode(const std::string& n, int l, int c) : name(n), line(l), col(c) {}
};

// Operação binária
struct BinaryOpNode : ASTNode {
    std::string op;
    NodePtr left, right;
    int line, col;
    BinaryOpNode(const std::string& op, NodePtr l, NodePtr r, int ln = 0, int c = 0)
        : op(op), left(std::move(l)), right(std::move(r)), line(ln), col(c) {}
};

// Operação unária: !expr, -expr
struct UnaryOpNode : ASTNode {
    std::string op;
    NodePtr operand;
    int line, col;
    UnaryOpNode(const std::string& o, NodePtr e, int l, int c)
        : op(o), operand(std::move(e)), line(l), col(c) {}
};

// Chamada de função: foo(a, b)
struct CallNode : ASTNode {
    std::string name;
    std::vector<NodePtr> args;
    int line, col;
    CallNode(const std::string& n, std::vector<NodePtr> a, int l, int c)
        : name(n), args(std::move(a)), line(l), col(c) {}
};

// Cast explícito: (i32)x, (f64)y
struct CastNode : ASTNode {
    DataType targetType;
    NodePtr expr;
    int line, col;
    CastNode(DataType t, NodePtr e, int l, int c)
        : targetType(t), expr(std::move(e)), line(l), col(c) {}
};

// Acesso de campo: p.x
struct FieldAccessNode : ASTNode {
    std::string varName;
    std::string fieldName;
    int line, col;
    FieldAccessNode(const std::string& v, const std::string& f, int l, int c)
        : varName(v), fieldName(f), line(l), col(c) {}
};

// Leitura de array: arr[i]
struct ArrayAccessNode : ASTNode {
    std::string name;
    NodePtr index;
    int line, col;
    ArrayAccessNode(const std::string& n, NodePtr i, int l, int c)
        : name(n), index(std::move(i)), line(l), col(c) {}
};

// ═══════════════════════════════════════════════════════════════
// STATEMENTS
// ═══════════════════════════════════════════════════════════════

// Declaração: let x: i32 = 5;  /  let mut x: i32 = 5;
struct VarDeclNode : ASTNode {
    DataType type;
    std::string typeName;   // preenchido se type == Custom
    std::string name;
    NodePtr init;
    bool isMutable;         // true se declarado com `let mut`
    int line, col;
    VarDeclNode(DataType t, const std::string& n, NodePtr i,
                bool mut = false, int l = 0, int c = 0)
        : type(t), name(n), init(std::move(i)), isMutable(mut), line(l), col(c) {}
};

// Return
struct ReturnNode : ASTNode {
    NodePtr expr;           // nullptr para `return;`
    ReturnNode(NodePtr e) : expr(std::move(e)) {}
};

// Print (builtin)
struct PrintNode : ASTNode {
    NodePtr expr;
    PrintNode(NodePtr e) : expr(std::move(e)) {}
};

// if cond { ... } else { ... }
struct IfNode : ASTNode {
    NodePtr condition;
    std::vector<NodePtr> thenBlock;
    std::vector<NodePtr> elseBlock;
    IfNode(NodePtr c, std::vector<NodePtr> t, std::vector<NodePtr> e)
        : condition(std::move(c)), thenBlock(std::move(t)), elseBlock(std::move(e)) {}
};

// while cond { ... }
struct WhileNode : ASTNode {
    NodePtr condition;
    std::vector<NodePtr> body;
    WhileNode(NodePtr c, std::vector<NodePtr> b)
        : condition(std::move(c)), body(std::move(b)) {}
};

// for (init; cond; step) { ... }
struct ForNode : ASTNode {
    NodePtr init;
    NodePtr condition;
    NodePtr step;
    std::vector<NodePtr> body;
    ForNode(NodePtr i, NodePtr c, NodePtr s, std::vector<NodePtr> b)
        : init(std::move(i)), condition(std::move(c)), step(std::move(s)), body(std::move(b)) {}
};

// Atribuição: x = expr  |  x += expr  |  x++  |  x--
struct VarAssignNode : ASTNode {
    std::string name;
    std::string op;
    NodePtr expr;           // nullptr para ++ e --
    int line, col;
    VarAssignNode(const std::string& n, const std::string& o, NodePtr e, int l, int c)
        : name(n), op(o), expr(std::move(e)), line(l), col(c) {}
};

// ═══════════════════════════════════════════════════════════════
// FUNÇÕES
// ═══════════════════════════════════════════════════════════════

// Parâmetro: name: Type  (tipo pode ser primitivo ou struct)
struct ParamNode {
    DataType type;
    std::string name;
    std::string structTypeName;   // preenchido se type == Custom
};

// fn nome(params) -> RetType { body }
struct FunctionNode : ASTNode {
    DataType returnType;
    std::string returnStructType; // preenchido se returnType == Custom
    std::string name;
    std::vector<ParamNode> params;
    std::vector<NodePtr> body;
    bool isVariadic;
    // Pertence a um impl? Se sim, implStructName != ""
    std::string implStructName;

    FunctionNode(DataType rt, const std::string& rst, const std::string& n,
                 std::vector<ParamNode> p, std::vector<NodePtr> b,
                 bool variadic = false, const std::string& implStruct = "")
        : returnType(rt), returnStructType(rst), name(n), params(std::move(p)),
          body(std::move(b)), isVariadic(variadic), implStructName(implStruct) {}
};

// Forward declaration (de .nh)
struct FuncDeclNode : ASTNode {
    DataType returnType;
    std::string returnStructType;
    std::string name;
    std::vector<ParamNode> params;
    bool isVariadic;
    FuncDeclNode(DataType rt, const std::string& n, std::vector<ParamNode> p,
                 bool variadic = false)
        : returnType(rt), returnStructType(""), name(n), params(std::move(p)),
          isVariadic(variadic) {}
    FuncDeclNode(DataType rt, const std::string& rst, const std::string& n,
                 std::vector<ParamNode> p, bool variadic = false)
        : returnType(rt), returnStructType(rst), name(n), params(std::move(p)),
          isVariadic(variadic) {}
};

// FuncDeclNode extendido (compatibilidade interna)
struct StructParamInfo {
    std::string structTypeName;
    std::string paramName;
};

struct FuncDeclNodeEx : ASTNode {
    DataType returnType;
    std::string returnStructType;
    std::string name;
    std::vector<ParamNode>       primitiveParams;
    std::vector<StructParamInfo> structParams;
    std::vector<std::pair<bool,int>> paramOrder;
    FuncDeclNodeEx(DataType rt, const std::string& rst, const std::string& n)
        : returnType(rt), returnStructType(rst), name(n) {}
};

// ═══════════════════════════════════════════════════════════════
// ARRAYS
// ═══════════════════════════════════════════════════════════════

struct ArrayDeclNode : ASTNode {
    DataType type;
    std::string name;
    int size;
    std::vector<NodePtr> init;
    std::string structTypeName;
    bool isMutable;
    int line, col;
    ArrayDeclNode(DataType t, const std::string& n, int s,
                  std::vector<NodePtr> i, int l, int c, bool mut = true)
        : type(t), name(n), size(s), init(std::move(i)), isMutable(mut), line(l), col(c) {}
    ArrayDeclNode(DataType t, const std::string& stn, const std::string& n, int s,
                  std::vector<NodePtr> i, int l, int c, bool mut = true)
        : type(t), name(n), size(s), init(std::move(i)), structTypeName(stn),
          isMutable(mut), line(l), col(c) {}
};

struct ArrayFieldAssignNode : ASTNode {
    std::string arrayName;
    NodePtr index;
    std::string fieldName;
    NodePtr value;
    int line, col;
    ArrayFieldAssignNode(const std::string& a, NodePtr i, const std::string& f,
                         NodePtr v, int l, int c)
        : arrayName(a), index(std::move(i)), fieldName(f), value(std::move(v)), line(l), col(c) {}
};

struct ArrayStructAssignNode : ASTNode {
    std::string arrayName;
    NodePtr index;
    std::string funcName;
    std::vector<NodePtr> args;
    int line, col;
    ArrayStructAssignNode(const std::string& a, NodePtr i, const std::string& fn,
                          std::vector<NodePtr> fargs, int l, int c)
        : arrayName(a), index(std::move(i)), funcName(fn), args(std::move(fargs)), line(l), col(c) {}
};

struct ArrayAssignNode : ASTNode {
    std::string name;
    NodePtr index;
    NodePtr value;
    int line, col;
    ArrayAssignNode(const std::string& n, NodePtr i, NodePtr v, int l, int c)
        : name(n), index(std::move(i)), value(std::move(v)), line(l), col(c) {}
};

// ═══════════════════════════════════════════════════════════════
// STRUCTS
// ═══════════════════════════════════════════════════════════════

struct StructField {
    DataType type;
    std::string name;
    std::string structTypeName;
};

// Método dentro do struct (usado apenas pelo codegen via ImplNode)
struct StructMethod {
    DataType returnType;
    std::string returnStructType;
    std::string name;
    std::vector<ParamNode> params;
    std::vector<NodePtr> body;
    bool hasRefSelf;   // true se o primeiro param é &self / &mut self
};

// Definição de struct — apenas campos (sem métodos inline)
// A sintaxe Rust-style proíbe métodos dentro de `struct { }`.
// Métodos vêm em `impl NomeStruct { fn ... }` → ImplNode.
struct StructDefNode : ASTNode {
    std::string name;
    std::vector<StructField> fields;
    // `methods` mantido vazio para structs, preenchido pelo codegen via ImplNode.
    std::vector<StructMethod> methods;
    int line, col;
    StructDefNode(const std::string& n, std::vector<StructField> f,
                  std::vector<StructMethod> m, int l, int c)
        : name(n), fields(std::move(f)), methods(std::move(m)), line(l), col(c) {}
};

// impl NomeStruct { fn método(&self, ...) -> RetType { ... } }
struct ImplNode : ASTNode {
    std::string structName;
    std::vector<StructMethod> methods;
    int line, col;
    ImplNode(const std::string& n, std::vector<StructMethod> m, int l, int c)
        : structName(n), methods(std::move(m)), line(l), col(c) {}
};

// Declaração de variável do tipo struct: let p: Point = makePoint(1, 2);
struct StructVarDeclNode : ASTNode {
    std::string typeName;
    std::string varName;
    std::string initFuncName;
    std::vector<NodePtr> initFuncArgs;
    bool isMutable;
    int line, col;
    StructVarDeclNode(const std::string& t, const std::string& v, bool mut, int l, int c)
        : typeName(t), varName(v), isMutable(mut), line(l), col(c) {}
    StructVarDeclNode(const std::string& t, const std::string& v,
                      const std::string& fn, std::vector<NodePtr> fa, bool mut, int l, int c)
        : typeName(t), varName(v), initFuncName(fn), initFuncArgs(std::move(fa)),
          isMutable(mut), line(l), col(c) {}
};

// Atribuição de struct completo: p = outraVar;  ou  p = funcao(...);
struct StructAssignNode : ASTNode {
    std::string varName;
    std::string srcVar;
    std::string funcName;
    std::vector<NodePtr> funcArgs;
    int line, col;
    StructAssignNode(const std::string& v, const std::string& src, int l, int c)
        : varName(v), srcVar(src), line(l), col(c) {}
    StructAssignNode(const std::string& v, const std::string& fn,
                     std::vector<NodePtr> fa, int l, int c)
        : varName(v), srcVar(""), funcName(fn), funcArgs(std::move(fa)), line(l), col(c) {}
};

// Atribuição de campo: p.x = expr;
struct FieldAssignNode : ASTNode {
    std::string varName;
    std::string fieldName;
    NodePtr value;
    int line, col;
    FieldAssignNode(const std::string& v, const std::string& f, NodePtr e, int l, int c)
        : varName(v), fieldName(f), value(std::move(e)), line(l), col(c) {}
};

// Chamada de método: p.move(10)
struct MethodCallNode : ASTNode {
    std::string varName;
    std::string methodName;
    std::vector<NodePtr> args;
    int line, col;
    MethodCallNode(const std::string& v, const std::string& m,
                   std::vector<NodePtr> a, int l, int c)
        : varName(v), methodName(m), args(std::move(a)), line(l), col(c) {}
};

// ═══════════════════════════════════════════════════════════════
// INLINE ASSEMBLY / IR
// ═══════════════════════════════════════════════════════════════

struct AsmNode : ASTNode {
    std::string code;
    std::string constraints;
    std::vector<std::string> capturedVars;
    int line, col;
    AsmNode(const std::string& c, const std::string& cst,
            const std::vector<std::string>& cv, int l, int co)
        : code(c), constraints(cst), capturedVars(cv), line(l), col(co) {}
};

struct IrNode : ASTNode {
    std::string code;
    std::vector<std::string> capturedVars;
    int line, col;
    IrNode(const std::string& c, const std::vector<std::string>& cv, int l, int co)
        : code(c), capturedVars(cv), line(l), col(co) {}
};
struct DerefNode : ASTNode {
    NodePtr operand;
    DataType type;
    int line, col;

    DerefNode(NodePtr op, DataType t, int l, int c)
        : operand(std::move(op)), type(t), line(l), col(c) {}
};

// Atribuição via ponteiro: *p = expr;
struct DerefAssignNode : ASTNode {
    NodePtr target;      // DerefNode (*p) ou expressão que resulta em ponteiro
    NodePtr value;       // valor a ser armazenado
    DataType targetType; // tipo do dado apontado
    int line, col;
    DerefAssignNode(NodePtr tgt, NodePtr val, DataType t, int l, int c)
        : target(std::move(tgt)), value(std::move(val)), targetType(t), line(l), col(c) {}
};

// ═══════════════════════════════════════════════════════════════
// PROGRAMA
// ═══════════════════════════════════════════════════════════════

struct ProgramNode {
    std::vector<NodePtr> declarations;
};

// ── Nó legado mantido apenas para compatibilidade com codegen ─────────────────
struct VarName : ASTNode {
    std::string name;
    VarName(const std::string& v) : name(v) {}
};