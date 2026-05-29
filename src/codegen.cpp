#include "../include/codegen.h"
#include "../include/ast.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/CGSCCPassManager.h"

#include <map>
#include <set>
#include "../include/error.h"
#include "../include/lexer.h"
#include <iostream>

using namespace llvm;

// ── Sistema de warnings ───────────────────────────────────────────────────────
#define ANSI_WARN   "\033[1;33m"
#define ANSI_BOLD   "\033[1m"
#define ANSI_RESET  "\033[0m"
#define ANSI_GRAY   "\033[0;90m"

static void emitWarning(const std::string& file, int line, int col,
                        const std::string& msg, const std::string& srcLine,
                        int tokenLen = 1) {
    std::cerr << ANSI_BOLD << file << ":" << line << ":" << col << ": "
              << ANSI_WARN << "warning: " << ANSI_RESET << ANSI_BOLD << msg << ANSI_RESET << "\n";
    if (!srcLine.empty()) {
        std::cerr << "  " << line << " | " << srcLine << "\n";
        std::string pad(std::to_string(line).size() + 3, ' ');
        std::cerr << pad << "| ";
        for (int i = 1; i < col; i++) std::cerr << ' ';
        std::cerr << ANSI_WARN;
        for (int i = 0; i < tokenLen; i++) std::cerr << '^';
        std::cerr << ANSI_RESET << "\n";
    }
}

static LLVMContext ctx;
static IRBuilder<> builder(ctx);
static std::unique_ptr<Module> llvmModule;

// Ativa fast-math flags globalmente no IRBuilder:
//   - reassoc: permite reordenar ops FP para melhor instrução scheduling
//   - contract: permite FMA (fused multiply-add) automaticamente
//   - afn: permite aproximações para funções transcendentais (sin, cos, sqrt)
//   - arcp: permite usar recíproco em vez de divisão FP
// Nota: NaN/Inf safety ainda é preservada (não usamos 'fast' completo sem
// consulta ao usuário). Para habilitar o modo totalmente unsafe, setar FastMath.
static void setupFastMath() {
    FastMathFlags fmf;
    fmf.setAllowReassoc(true);
    fmf.setAllowContract(true);
    fmf.setAllowReciprocal(true);
    fmf.setApproxFunc(true);
    // Não setamos NoNaNs/NoInfs por padrão para não quebrar programas que dependem desses comportamentos
    builder.setFastMathFlags(fmf);
}
static std::string sourceFile;

// Variáveis locais (dentro de funções): limpo a cada função
static std::map<std::string, AllocaInst*> localValues;
// Variáveis globais: nunca limpo, acessível de qualquer função
static std::map<std::string, GlobalVariable*> globalValues;
// Arrays locais: nome -> (AllocaInst*, tamanho, tipo elemento)
static std::map<std::string, std::pair<AllocaInst*, Type*>> localArrays;
// Arrays globais: nome -> (GlobalVariable*, tipo elemento)
static std::map<std::string, std::pair<GlobalVariable*, Type*>> globalArrays;

// Structs: nome do tipo -> StructType* LLVM
static std::map<std::string, StructType*> structTypes;
// Structs: nome do tipo -> lista de campos (para calcular índice)
static std::map<std::string, std::vector<StructField>> structFields;
// Variáveis locais do tipo struct: nome -> Value* (pode ser alloca ou ponteiro bruto)
static std::map<std::string, std::pair<Value*, std::string>> localStructs; // varName -> (ptr, typeName)
// Variáveis globais do tipo struct: nome -> (GlobalVariable*, typeName)
static std::map<std::string, std::pair<GlobalVariable*, std::string>> globalStructs;

// Enums: nome do tipo -> lista de (nome, valor discriminante)
// Enums são representados como i32 em LLVM
static std::map<std::string, std::vector<std::pair<std::string, int>>> enumValues;

// Funções que retornam struct: nome da função → nome do tipo struct
// A convenção é: hidden first param (sret pointer), return void
static std::map<std::string, std::string> structReturnFuncs;

// Parâmetros de struct de cada função: funcName -> lista de (índice do param, nome do tipo struct)
// Usado para saber quais params passar por ponteiro
static std::map<std::string, std::vector<std::pair<int,std::string>>> structParamFuncs;

// ── Tracking de uso para warnings ────────────────────────────────────────────
// Variáveis locais: nome -> {linha, col, usado}
struct VarInfo { int line; int col; bool used; };
static std::map<std::string, VarInfo> localVarUsage;
// Funções chamadas — acumulado entre TODOS os arquivos compilados na mesma invocação
static std::set<std::string> calledFunctions;
// Funções declaradas (nome -> arquivo onde foram definidas)
static std::map<std::string, std::string> declaredFunctions;

// true quando o arquivo sendo compilado não tem main (é uma lib).
// Suprime warnings de variável/função não usada nesse arquivo.
static bool isLibraryFile = false;

// Builtins declarados via .nh — só esses nomes podem ser chamados com namespace::
// Ex: "std::printf" após #include <stdio.nh> com namespace std;
static std::set<std::string> nhDeclaredBuiltins;

// ── Registro de sobrecarga de funções ─────────────────────────────────────────
// Para cada nome de função, guarda lista de (tipos dos params → nome mangled no LLVM)
struct OverloadEntry {
    std::vector<Type*> paramTypes;
    std::string mangledName;
};
static std::map<std::string, std::vector<OverloadEntry>> overloadTable;

// Sufixo de mangling: int→i, float→f, double→d, long→l, string→s
static std::string mangleSuffix(const std::vector<Type*>& types) {
    std::string s;
    for (auto* t : types) {
        if      (t->isIntegerTy(8))  s += "c";  // char
        else if (t->isIntegerTy(32)) s += "i";
        else if (t->isIntegerTy(64)) s += "l";
        else if (t->isFloatTy())     s += "f";
        else if (t->isDoubleTy())    s += "d";
        else if (t->isPointerTy())   s += "s";
        else                         s += "x";
    }
    return s.empty() ? "v" : s;
}

// Resolve overload dado nome do usuário e valores dos argumentos já gerados.
// Tenta match exato primeiro; depois aceita promoções numéricas (int→float/double, float→double).
// Também ajusta (coerce) os args para os tipos esperados.
static Function* resolveOverload(const std::string& name, std::vector<Value*>& args,
                                  int line, int col) {
    auto it = overloadTable.find(name);
    if (it == overloadTable.end()) return nullptr;
    auto& entries = it->second;

    // 1ª: match exato
    for (auto& e : entries) {
        if (e.paramTypes.size() != args.size()) continue;
        bool match = true;
        for (size_t i = 0; i < args.size(); i++)
            if (args[i]->getType() != e.paramTypes[i]) { match = false; break; }
        if (match) return llvmModule->getFunction(e.mangledName);
    }

    // 2ª: match com promoção — int32→float/double, float→double
    for (auto& e : entries) {
        if (e.paramTypes.size() != args.size()) continue;
        bool match = true;
        for (size_t i = 0; i < args.size(); i++) {
            Type* got  = args[i]->getType();
            Type* want = e.paramTypes[i];
            if (got == want) continue;
            if (got->isIntegerTy(32) && (want->isFloatTy() || want->isDoubleTy())) continue;
            if (got->isFloatTy()     && want->isDoubleTy()) continue;
            match = false; break;
        }
        if (!match) continue;
        // Aplica coerções nos args
        Function* fn = llvmModule->getFunction(e.mangledName);
        for (size_t i = 0; i < args.size(); i++) {
            Type* got  = args[i]->getType();
            Type* want = e.paramTypes[i];
            if (got == want) continue;
            if (got->isIntegerTy(32) && want->isFloatTy())
                args[i] = builder.CreateSIToFP(args[i], want, "coerce");
            else if (got->isIntegerTy(32) && want->isDoubleTy())
                args[i] = builder.CreateSIToFP(args[i], want, "coerce");
            else if (got->isFloatTy() && want->isDoubleTy())
                args[i] = builder.CreateFPExt(args[i], want, "coerce");
        }
        return fn;
    }

    return nullptr;
}

// Chamado pelo main após compilar TODOS os arquivos.
// Só emite warnings se ao menos um arquivo tinha main (não é build de lib pura).
void flushFunctionWarnings(bool projectHasMain) {
    if (projectHasMain) {
        for (auto& [name, file] : declaredFunctions) {
            if (calledFunctions.find(name) == calledFunctions.end())
                emitWarning(file, 0, 0,
                    "function '" + name + "' is defined but never called", "");
        }
    }
    calledFunctions.clear();
    declaredFunctions.clear();
}

static Type* llvmType(DataType t) {
    switch (t) {
        case DataType::Int:      return Type::getInt32Ty(ctx);
        case DataType::Long:     return Type::getInt64Ty(ctx);
        case DataType::LongLong: return Type::getInt64Ty(ctx);
        case DataType::Float:    return Type::getFloatTy(ctx);
        case DataType::Double:   return Type::getDoubleTy(ctx);
        case DataType::String:   return PointerType::getUnqual(ctx);
        case DataType::Char:     return Type::getInt8Ty(ctx);
        case DataType::Bool:     return Type::getInt1Ty(ctx);   // ← ADICIONAR
        case DataType::Void:     return Type::getVoidTy(ctx);
        case DataType::Custom:   return PointerType::getUnqual(ctx);
    }
    return Type::getInt32Ty(ctx);
}

static AllocaInst* createEntryAlloca(Function* fn, const std::string& name, Type* type) {
    IRBuilder<> tmp(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    return tmp.CreateAlloca(type, nullptr, name);
}

static int getFieldIndex(const std::string& typeName, const std::string& fieldName, int line, int col);

// Retorna ponteiro (Value*) para o struct (local ou global)
static Value* getStructPtrValue(const std::string& varName, std::string& outTypeName) {
    auto lit = localStructs.find(varName);
    if (lit != localStructs.end()) {
        outTypeName = lit->second.second;
        return lit->second.first;
    }
    auto git = globalStructs.find(varName);
    if (git != globalStructs.end()) {
        outTypeName = git->second.second;
        return git->second.first;
    }
    return nullptr;
}

// Copia todos os campos de srcPtr para dstPtr (struct do tipo typeName)
// Suporta campos nested struct recursivamente.
static void copyStruct(Value* dstPtr, Value* srcPtr, const std::string& typeName) {
    StructType* st = structTypes[typeName];
    auto& fields = structFields[typeName];
    for (int i = 0; i < (int)fields.size(); i++) {
        Value* srcGep = builder.CreateStructGEP(st, srcPtr, i, "src." + fields[i].name);
        Value* dstGep = builder.CreateStructGEP(st, dstPtr, i, "dst." + fields[i].name);
        if (fields[i].type == DataType::Custom && !fields[i].structTypeName.empty()) {
            // Campo nested struct — copia recursivamente
            copyStruct(dstGep, srcGep, fields[i].structTypeName);
        } else {
            Type* ft = llvmType(fields[i].type);
            Value* val = builder.CreateLoad(ft, srcGep, fields[i].name);
            builder.CreateStore(val, dstGep);
        }
    }
}

// Chama uma função que retorna struct usando convenção sret:
//   - Aloca espaço local para o resultado
//   - Passa o ponteiro como primeiro argumento (sret)
//   - Retorna o Value* (alloca) com o resultado
static Value* callStructReturningFunc(Function* parentFn,
                                       const std::string& funcName,
                                       const std::string& structTypeName,
                                       std::vector<Value*>& userArgs,
                                       int line, int col) {
    std::string llvmFuncName = funcName;
    // Tenta resolver overload
    auto oit = overloadTable.find(funcName);
    if (oit != overloadTable.end() && !oit->second.empty())
        llvmFuncName = oit->second[0].mangledName;

    Function* fn = llvmModule->getFunction(llvmFuncName);
    if (!fn)
        reportError(sourceFile, line, col,
                    "function '" + funcName + "' was not declared",
                    getSourceLine(line), (int)funcName.size());

    StructType* st = structTypes[structTypeName];
    if (!st)
        reportError(sourceFile, line, col,
                    "struct type '" + structTypeName + "' was not declared",
                    getSourceLine(line), (int)structTypeName.size());

    // Aloca espaço para o resultado
    Value* resultAlloca = createEntryAlloca(parentFn, "sret." + funcName, st);

    // Monta args: sret pointer primeiro, depois user args
    std::vector<Value*> callArgs;
    callArgs.push_back(resultAlloca);
    for (auto* v : userArgs) callArgs.push_back(v);

    builder.CreateCall(fn, callArgs);
    return resultAlloca;
}


// ─────────────────────────────────────────────────────────────────────────────
// nova_printf — implementação de printf sem libc, em LLVM IR puro.
//
// Arquitetura:
//   nova__write_int(i64 val, i1 isSigned)  — converte inteiro → decimal → write
//   nova__write_dbl(double val)             — converte double → decimal.frac → write
//   nova_printf(i8* fmt, ...)               — loop sobre fmt, despacha por especificador
//
// Todos usam write(2) do sistema via chamada externa — sem InlineAsm,
// portanto funciona em qualquer target LLVM independente do asm parser.
//
// Especificadores suportados: %d %i %u %ld %li %lu %f %g %c %s %%
// ─────────────────────────────────────────────────────────────────────────────

// Declara write(int fd, const void* buf, size_t count) como simbolo externo.
// Presente em qualquer Linux — sem InlineAsm, sem dependencia de asm parser.
static Function* getOrDeclareWrite() {
    if (Function* f = llvmModule->getFunction("write")) return f;
    Type* i32   = Type::getInt32Ty(ctx);
    Type* i64   = Type::getInt64Ty(ctx);
    Type* ptrTy = PointerType::getUnqual(ctx);
    // ssize_t write(int fd, const void* buf, size_t count)
    FunctionType* ft = FunctionType::get(i64, {i32, ptrTy, i64}, false);
    return Function::Create(ft, Function::ExternalLinkage, "write", llvmModule.get());
}

// Emite write(1, buf, len) — funciona em qualquer target LLVM.
template<typename BuilderT>
static void emitWrite(BuilderT& b, Value* buf, Value* len) {
    Function* writeFn = getOrDeclareWrite();
    Value* len64 = len->getType()->isIntegerTy(64)
        ? len
        : b.CreateSExt(len, Type::getInt64Ty(ctx), "len64");
    b.CreateCall(writeFn,
        {ConstantInt::get(Type::getInt32Ty(ctx), 1), buf, len64});
}

// ── nova__write_int ───────────────────────────────────────────────────────────
// Converte um i64 para string decimal e escreve no stdout via syscall write.
// isSigned=true → interpreta como signed (emite '-' se negativo).
static Function* getOrBuildWriteInt() {
    if (Function* f = llvmModule->getFunction("nova__write_int")) return f;

    Type* i1    = Type::getInt1Ty(ctx);
    Type* i8    = Type::getInt8Ty(ctx);
    Type* i32   = Type::getInt32Ty(ctx);
    Type* i64   = Type::getInt64Ty(ctx);

    FunctionType* ft = FunctionType::get(
        Type::getVoidTy(ctx), {i64, i1}, false);
    Function* fn = Function::Create(ft, Function::InternalLinkage,
                                    "nova__write_int", llvmModule.get());
    fn->addFnAttr(Attribute::NoUnwind);

    auto ai = fn->arg_begin();
    Argument* argVal    = &*ai++;  argVal->setName("val");
    Argument* argSigned = &*ai;    argSigned->setName("isSigned");

    // Blocos
    BasicBlock* bbEntry  = BasicBlock::Create(ctx, "entry",    fn);
    BasicBlock* bbNeg    = BasicBlock::Create(ctx, "neg",      fn);
    BasicBlock* bbLoop   = BasicBlock::Create(ctx, "loop",     fn);
    BasicBlock* bbLoopB  = BasicBlock::Create(ctx, "loop_body",fn);
    BasicBlock* bbDone   = BasicBlock::Create(ctx, "done",     fn);
    BasicBlock* bbSign   = BasicBlock::Create(ctx, "sign",     fn);
    BasicBlock* bbSignY  = BasicBlock::Create(ctx, "sign_yes", fn);
    BasicBlock* bbEmit   = BasicBlock::Create(ctx, "emit",     fn);

    IRBuilder<> b(bbEntry);

    // Buffer de 22 chars na stack (preenchido da direita para a esquerda)
    // idx começa em 22; cada dígito: idx--, buf[idx] = digit
    ArrayType* bufTy = ArrayType::get(i8, 22);
    Value* buf    = b.CreateAlloca(bufTy, nullptr, "buf");
    Value* iIdx   = b.CreateAlloca(i32, nullptr, "idx");
    Value* iVal   = b.CreateAlloca(i64, nullptr, "val");
    Value* iNeg   = b.CreateAlloca(i1,  nullptr, "neg");

    b.CreateStore(ConstantInt::get(i32, 22), iIdx);
    b.CreateStore(argVal, iVal);
    b.CreateStore(ConstantInt::getFalse(ctx), iNeg);

    // Se signed e val < 0: nega e marca flag
    Value* isNeg = b.CreateAnd(argSigned,
        b.CreateICmpSLT(argVal, ConstantInt::get(i64, 0)), "is_neg");
    b.CreateCondBr(isNeg, bbNeg, bbLoop);

    b.SetInsertPoint(bbNeg);
    b.CreateStore(b.CreateNeg(argVal, "negv"), iVal);
    b.CreateStore(ConstantInt::getTrue(ctx), iNeg);
    b.CreateBr(bbLoop);

    // Loop: while (val != 0) { buf[--idx] = '0' + val%10; val /= 10; }
    b.SetInsertPoint(bbLoop);
    Value* cv = b.CreateLoad(i64, iVal, "cv");
    b.CreateCondBr(b.CreateICmpEQ(cv, ConstantInt::get(i64, 0)),
                   bbDone, bbLoopB);

    b.SetInsertPoint(bbLoopB);
    Value* cv2   = b.CreateLoad(i64, iVal, "cv2");
    Value* rem   = b.CreateURem(cv2, ConstantInt::get(i64, 10), "rem");
    Value* quot  = b.CreateUDiv(cv2, ConstantInt::get(i64, 10), "quot");
    Value* digit = b.CreateAdd(b.CreateTrunc(rem, i8),
                               ConstantInt::get(i8, (uint64_t)'0'), "digit");
    Value* curIdx= b.CreateLoad(i32, iIdx, "ci");
    Value* newIdx= b.CreateSub(curIdx, ConstantInt::get(i32, 1), "ni");
    Value* gep   = b.CreateGEP(bufTy, buf,
        {ConstantInt::get(i32, 0), newIdx}, "gep");
    b.CreateStore(digit, gep);
    b.CreateStore(newIdx, iIdx);
    b.CreateStore(quot, iVal);
    b.CreateBr(bbLoop);

    // Done: se nenhum dígito foi escrito (val era 0), escreve '0'
    b.SetInsertPoint(bbDone);
    Value* idxD   = b.CreateLoad(i32, iIdx, "idxD");
    Value* noDigit= b.CreateICmpEQ(idxD, ConstantInt::get(i32, 22));
    BasicBlock* bbZero = BasicBlock::Create(ctx, "zero", fn);
    b.CreateCondBr(noDigit, bbZero, bbSign);

    b.SetInsertPoint(bbZero);
    Value* idxZ  = b.CreateLoad(i32, iIdx, "idxZ");
    Value* idxZ1 = b.CreateSub(idxZ, ConstantInt::get(i32, 1));
    Value* gepZ  = b.CreateGEP(bufTy, buf,
        {ConstantInt::get(i32, 0), idxZ1});
    b.CreateStore(ConstantInt::get(i8, (uint64_t)'0'), gepZ);
    b.CreateStore(idxZ1, iIdx);
    b.CreateBr(bbSign);

    // Sign: se negativo, coloca '-'
    b.SetInsertPoint(bbSign);
    Value* negFlag = b.CreateLoad(i1, iNeg, "negflag");
    b.CreateCondBr(negFlag, bbSignY, bbEmit);

    b.SetInsertPoint(bbSignY);
    Value* idxS  = b.CreateLoad(i32, iIdx, "idxS");
    Value* idxS1 = b.CreateSub(idxS, ConstantInt::get(i32, 1));
    Value* gepS  = b.CreateGEP(bufTy, buf,
        {ConstantInt::get(i32, 0), idxS1});
    b.CreateStore(ConstantInt::get(i8, (uint64_t)'-'), gepS);
    b.CreateStore(idxS1, iIdx);
    b.CreateBr(bbEmit);

    // Emit: calcula start ptr e len, chama write
    b.SetInsertPoint(bbEmit);
    Value* startIdx = b.CreateLoad(i32, iIdx, "startIdx");
    Value* startPtr = b.CreateGEP(bufTy, buf,
        {ConstantInt::get(i32, 0), startIdx}, "startPtr");
    // len = 22 - startIdx
    Value* lenV = b.CreateSub(ConstantInt::get(i32, 22), startIdx, "lenV");
    emitWrite(b, startPtr, lenV);
    b.CreateRetVoid();

    verifyFunction(*fn);
    return fn;
}

// ── nova__write_dbl ───────────────────────────────────────────────────────────
// Converte um double para "inteiro.fracionário" (6 casas) e escreve no stdout.
static Function* getOrBuildWriteDbl() {
    if (Function* f = llvmModule->getFunction("nova__write_dbl")) return f;

    Type* i8    = Type::getInt8Ty(ctx);
    Type* i32   = Type::getInt32Ty(ctx);
    Type* i64   = Type::getInt64Ty(ctx);
    Type* dbl   = Type::getDoubleTy(ctx);
    Type* ptrTy = PointerType::getUnqual(ctx);

    Function* writeInt = getOrBuildWriteInt();

    FunctionType* ft = FunctionType::get(Type::getVoidTy(ctx), {dbl}, false);
    Function* fn = Function::Create(ft, Function::InternalLinkage,
                                    "nova__write_dbl", llvmModule.get());
    fn->addFnAttr(Attribute::NoUnwind);

    Argument* argVal = &*fn->arg_begin();
    argVal->setName("val");

    BasicBlock* bbEntry = BasicBlock::Create(ctx, "entry",    fn);
    BasicBlock* bbNeg   = BasicBlock::Create(ctx, "neg",      fn);
    BasicBlock* bbPos   = BasicBlock::Create(ctx, "pos",      fn);
    BasicBlock* bbFrac  = BasicBlock::Create(ctx, "frac",     fn);
    BasicBlock* bbRet   = BasicBlock::Create(ctx, "ret",      fn);

    IRBuilder<> b(bbEntry);

    // Checa negativo
    Value* isNeg = b.CreateFCmpOLT(argVal, ConstantFP::get(dbl, 0.0));
    b.CreateCondBr(isNeg, bbNeg, bbPos);

    // Escreve '-' e pega abs
    b.SetInsertPoint(bbNeg);
    {
        Value* minusBuf = b.CreateAlloca(i8, nullptr, "mbuf");
        b.CreateStore(ConstantInt::get(i8, (uint64_t)'-'), minusBuf);
        emitWrite(b, minusBuf, ConstantInt::get(i32, 1));
    }
    Value* absVal = b.CreateFNeg(argVal, "absval");
    b.CreateBr(bbPos);

    // PHI para o valor positivo
    b.SetInsertPoint(bbPos);
    PHINode* posVal = b.CreatePHI(dbl, 2, "posval");
    posVal->addIncoming(argVal, bbEntry);
    posVal->addIncoming(absVal, bbNeg);

    // Parte inteira → i64
    Value* intPart = b.CreateFPToUI(posVal, i64, "intpart");
    b.CreateCall(writeInt, {intPart, ConstantInt::getFalse(ctx)});

    // Escreve '.'
    Value* dotBuf = b.CreateAlloca(i8, nullptr, "dotbuf");
    b.CreateStore(ConstantInt::get(i8, (uint64_t)'.'), dotBuf);
    emitWrite(b, dotBuf, ConstantInt::get(i32, 1));
    b.CreateBr(bbFrac);

    // Parte fracionária — 6 dígitos, zero-padded, sem arredondamento
    b.SetInsertPoint(bbFrac);
    Value* intF   = b.CreateUIToFP(intPart, dbl, "intf");
    Value* frac   = b.CreateFSub(posVal, intF, "frac");
    Value* scaled = b.CreateFMul(frac, ConstantFP::get(dbl, 1000000.0), "scaled");
    Value* fracI  = b.CreateFPToUI(scaled, i64, "fraci");

    // Buffer de 6 bytes para os dígitos fracionários
    ArrayType* fbufTy = ArrayType::get(i8, 6);
    Value* fbuf = b.CreateAlloca(fbufTy, nullptr, "fbuf");
    Value* fv   = fracI;
    // Preenche da direita para a esquerda
    for (int di = 5; di >= 0; di--) {
        Value* rem   = b.CreateURem(fv, ConstantInt::get(i64, 10));
        Value* digit = b.CreateAdd(b.CreateTrunc(rem, i8),
                                   ConstantInt::get(i8, (uint64_t)'0'));
        Value* gepF  = b.CreateGEP(fbufTy, fbuf,
            {ConstantInt::get(i32, 0), ConstantInt::get(i32, di)});
        b.CreateStore(digit, gepF);
        fv = b.CreateUDiv(fv, ConstantInt::get(i64, 10));
    }
    Value* fbufPtr = b.CreateBitCast(fbuf, ptrTy);
    emitWrite(b, fbufPtr, ConstantInt::get(i32, 6));
    b.CreateBr(bbRet);

    b.SetInsertPoint(bbRet);
    b.CreateRetVoid();

    verifyFunction(*fn);
    return fn;
}

// ── nova_printf ───────────────────────────────────────────────────────────────
// Loop principal: percorre fmt char a char, escreve literais e despacha
// especificadores para as funções auxiliares acima.
static Function* getOrBuildNovaPrintf() {
    if (Function* f = llvmModule->getFunction("nova_printf")) return f;

    Type* i8    = Type::getInt8Ty(ctx);
    Type* i32   = Type::getInt32Ty(ctx);
    Type* i64   = Type::getInt64Ty(ctx);
    Type* dbl   = Type::getDoubleTy(ctx);
    Type* ptrTy = PointerType::getUnqual(ctx);

    Function* writeInt = getOrBuildWriteInt();
    Function* writeDbl = getOrBuildWriteDbl();

    // nova_printf(i8* fmt, ...) → i32
    FunctionType* ft = FunctionType::get(i32, {ptrTy}, true);
    Function* fn = Function::Create(ft, Function::InternalLinkage,
                                    "nova_printf", llvmModule.get());
    fn->addFnAttr(Attribute::NoUnwind);

    Argument* fmtArg = &*fn->arg_begin();
    fmtArg->setName("fmt");

    // ── Blocos ──────────────────────────────────────────────────────────────
    BasicBlock* bbEntry   = BasicBlock::Create(ctx, "entry",    fn);
    BasicBlock* bbLoop    = BasicBlock::Create(ctx, "loop",     fn);
    BasicBlock* bbCheck   = BasicBlock::Create(ctx, "check",    fn);
    BasicBlock* bbLit     = BasicBlock::Create(ctx, "lit",      fn);  // char literal
    BasicBlock* bbReadSpc = BasicBlock::Create(ctx, "read_spc", fn);  // lê char após %
    BasicBlock* bbLong    = BasicBlock::Create(ctx, "long_pfx", fn);  // lê char após %l
    BasicBlock* bbFmtD    = BasicBlock::Create(ctx, "fmt_d",    fn);
    BasicBlock* bbFmtLD   = BasicBlock::Create(ctx, "fmt_ld",   fn);
    BasicBlock* bbFmtU    = BasicBlock::Create(ctx, "fmt_u",    fn);
    BasicBlock* bbFmtLU   = BasicBlock::Create(ctx, "fmt_lu",   fn);
    BasicBlock* bbFmtF    = BasicBlock::Create(ctx, "fmt_f",    fn);
    BasicBlock* bbFmtC    = BasicBlock::Create(ctx, "fmt_c",    fn);
    BasicBlock* bbFmtS    = BasicBlock::Create(ctx, "fmt_s",    fn);
    BasicBlock* bbFmtPct  = BasicBlock::Create(ctx, "fmt_pct",  fn);
    BasicBlock* bbFmtUnk  = BasicBlock::Create(ctx, "fmt_unk",  fn);
    BasicBlock* bbSlLoop  = BasicBlock::Create(ctx, "sl_loop",  fn);  // strlen loop
    BasicBlock* bbSlInc   = BasicBlock::Create(ctx, "sl_inc",   fn);
    BasicBlock* bbSlDone  = BasicBlock::Create(ctx, "sl_done",  fn);
    BasicBlock* bbNext    = BasicBlock::Create(ctx, "next",     fn);
    BasicBlock* bbRet     = BasicBlock::Create(ctx, "ret",      fn);

    IRBuilder<> b(bbEntry);

    // va_list como array de 24 i8 na stack
    ArrayType* vaTy = ArrayType::get(i8, 24);
    Value* vaList = b.CreateAlloca(vaTy, nullptr, "va_list");
    Value* vaPtr  = b.CreateBitCast(vaList, ptrTy, "va_ptr");

    // va_start
    Function* vaStartFn = Intrinsic::getDeclaration(
        llvmModule.get(), Intrinsic::vastart, {ptrTy});
    b.CreateCall(vaStartFn, {vaPtr});

    // idx = 0
    Value* iIdx = b.CreateAlloca(i32, nullptr, "idx");
    b.CreateStore(ConstantInt::get(i32, 0), iIdx);
    b.CreateBr(bbLoop);

    // ── bbLoop: carrega fmt[idx] ─────────────────────────────────────────
    b.SetInsertPoint(bbLoop);
    Value* idx    = b.CreateLoad(i32, iIdx, "idx");
    Value* chPtr  = b.CreateGEP(i8, fmtArg, idx, "ch_ptr");
    Value* ch     = b.CreateLoad(i8, chPtr, "ch");
    Value* isEnd  = b.CreateICmpEQ(ch, ConstantInt::get(i8, 0));
    b.CreateCondBr(isEnd, bbRet, bbCheck);

    // ── bbCheck: é '%'? ───────────────────────────────────────────────────
    b.SetInsertPoint(bbCheck);
    Value* isPct = b.CreateICmpEQ(ch, ConstantInt::get(i8, (uint64_t)'%'));
    b.CreateCondBr(isPct, bbReadSpc, bbLit);

    // ── bbLit: escreve o char literal ─────────────────────────────────────
    b.SetInsertPoint(bbLit);
    emitWrite(b, chPtr, ConstantInt::get(i32, 1));
    b.CreateBr(bbNext);

    // ── bbReadSpc: avança idx e lê especificador ──────────────────────────
    b.SetInsertPoint(bbReadSpc);
    Value* idxS   = b.CreateLoad(i32, iIdx, "idxS");
    Value* idxS1  = b.CreateAdd(idxS, ConstantInt::get(i32, 1));
    b.CreateStore(idxS1, iIdx);
    Value* spcPtr = b.CreateGEP(i8, fmtArg, idxS1, "spc_ptr");
    Value* spc    = b.CreateLoad(i8, spcPtr, "spc");

    SwitchInst* sw = b.CreateSwitch(spc, bbFmtUnk, 9);
    sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'d')), bbFmtD);
    sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'i')), bbFmtD);
    sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'u')), bbFmtU);
    sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'f')), bbFmtF);
    sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'g')), bbFmtF);
    sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'e')), bbFmtF);
    sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'c')), bbFmtC);
    sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'s')), bbFmtS);
    sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'%')), bbFmtPct);
    sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'l')), bbLong);

    // ── bbLong: lê char após %l ───────────────────────────────────────────
    b.SetInsertPoint(bbLong);
    Value* idxL   = b.CreateLoad(i32, iIdx, "idxL");
    Value* idxL1  = b.CreateAdd(idxL, ConstantInt::get(i32, 1));
    b.CreateStore(idxL1, iIdx);
    Value* lPtr   = b.CreateGEP(i8, fmtArg, idxL1, "l_ptr");
    Value* lSpc   = b.CreateLoad(i8, lPtr, "l_spc");
    SwitchInst* sw2 = b.CreateSwitch(lSpc, bbFmtUnk, 3);
    sw2->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'d')), bbFmtLD);
    sw2->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'i')), bbFmtLD);
    sw2->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'u')), bbFmtLU);

    // ── %d/%i ─────────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtD);
    {
        Value* v32  = b.CreateVAArg(vaPtr, i32, "va_d");
        Value* v64  = b.CreateSExt(v32, i64, "v64");
        b.CreateCall(writeInt, {v64, ConstantInt::getTrue(ctx)});
    }
    b.CreateBr(bbNext);

    // ── %ld/%li ───────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtLD);
    {
        Value* v64 = b.CreateVAArg(vaPtr, i64, "va_ld");
        b.CreateCall(writeInt, {v64, ConstantInt::getTrue(ctx)});
    }
    b.CreateBr(bbNext);

    // ── %u ────────────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtU);
    {
        Value* v32  = b.CreateVAArg(vaPtr, i32, "va_u");
        Value* v64  = b.CreateZExt(v32, i64, "v64u");
        b.CreateCall(writeInt, {v64, ConstantInt::getFalse(ctx)});
    }
    b.CreateBr(bbNext);

    // ── %lu ───────────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtLU);
    {
        Value* v64 = b.CreateVAArg(vaPtr, i64, "va_lu");
        b.CreateCall(writeInt, {v64, ConstantInt::getFalse(ctx)});
    }
    b.CreateBr(bbNext);

    // ── %f/%g/%e ──────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtF);
    {
        Value* vd = b.CreateVAArg(vaPtr, dbl, "va_f");
        b.CreateCall(writeDbl, {vd});
    }
    b.CreateBr(bbNext);

    // ── %c ────────────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtC);
    {
        // char é promovido para i32 na ABI variádica
        Value* v32  = b.CreateVAArg(vaPtr, i32, "va_c");
        Value* v8   = b.CreateTrunc(v32, i8, "c8");
        Value* cbuf = b.CreateAlloca(i8, nullptr, "cbuf");
        b.CreateStore(v8, cbuf);
        emitWrite(b, cbuf, ConstantInt::get(i32, 1));
    }
    b.CreateBr(bbNext);

    // ── %s ────────────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtS);
    {
        Value* strPtr   = b.CreateVAArg(vaPtr, ptrTy, "va_s");
        Value* iLenAlloc= b.CreateAlloca(i32, nullptr, "slen");
        b.CreateStore(ConstantInt::get(i32, 0), iLenAlloc);
        b.CreateBr(bbSlLoop);

        // strlen loop
        b.SetInsertPoint(bbSlLoop);
        Value* li    = b.CreateLoad(i32, iLenAlloc, "li");
        Value* slGep = b.CreateGEP(i8, strPtr, li, "sl_gep");
        Value* slCh  = b.CreateLoad(i8, slGep, "sl_ch");
        b.CreateCondBr(
            b.CreateICmpEQ(slCh, ConstantInt::get(i8, 0)),
            bbSlDone, bbSlInc);

        b.SetInsertPoint(bbSlInc);
        Value* li2 = b.CreateLoad(i32, iLenAlloc, "li2");
        b.CreateStore(b.CreateAdd(li2, ConstantInt::get(i32, 1)), iLenAlloc);
        b.CreateBr(bbSlLoop);

        b.SetInsertPoint(bbSlDone);
        Value* slen = b.CreateLoad(i32, iLenAlloc, "slen_v");
        emitWrite(b, strPtr, slen);
    }
    b.CreateBr(bbNext);

    // ── %% ────────────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtPct);
    {
        Value* pctIdx = b.CreateLoad(i32, iIdx, "pctIdx");
        Value* pctPtr = b.CreateGEP(i8, fmtArg, pctIdx, "pct_ptr");
        emitWrite(b, pctPtr, ConstantInt::get(i32, 1));
    }
    b.CreateBr(bbNext);

    // ── especificador desconhecido: escreve %X literalmente ───────────────
    b.SetInsertPoint(bbFmtUnk);
    {
        Value* unkIdx = b.CreateLoad(i32, iIdx, "unkIdx");
        // O '%' está em unkIdx-1, o char desconhecido em unkIdx
        Value* unkM1  = b.CreateSub(unkIdx, ConstantInt::get(i32, 1));
        Value* unkPtr = b.CreateGEP(i8, fmtArg, unkM1, "unk_ptr");
        emitWrite(b, unkPtr, ConstantInt::get(i32, 2));
    }
    b.CreateBr(bbNext);

    // ── bbNext: incrementa idx e volta ao loop ────────────────────────────
    b.SetInsertPoint(bbNext);
    Value* idxN = b.CreateLoad(i32, iIdx, "idxN");
    b.CreateStore(b.CreateAdd(idxN, ConstantInt::get(i32, 1)), iIdx);
    b.CreateBr(bbLoop);

    // ── bbRet: va_end e retorna 0 ─────────────────────────────────────────
    b.SetInsertPoint(bbRet);
    Function* vaEndFn = Intrinsic::getDeclaration(
        llvmModule.get(), Intrinsic::vaend, {ptrTy});
    b.CreateCall(vaEndFn, {vaPtr});
    b.CreateRet(ConstantInt::get(i32, 0));

    verifyFunction(*fn);
    return fn;
}

// ── nova_scanf ────────────────────────────────────────────────────────────────
// Implementação de scanf sem libc, usando read(2) via chamada externa.
// Especificadores: %d %i %u %ld %li %lu %f %lf %c %s %%
// Retorna número de conversões bem-sucedidas (i32).
// ─────────────────────────────────────────────────────────────────────────────

static Function* getOrDeclareRead() {
    if (Function* f = llvmModule->getFunction("read")) return f;
    Type* i32   = Type::getInt32Ty(ctx);
    Type* i64   = Type::getInt64Ty(ctx);
    Type* ptrTy = PointerType::getUnqual(ctx);
    FunctionType* ft = FunctionType::get(i64, {i32, ptrTy, i64}, false);
    return Function::Create(ft, Function::ExternalLinkage, "read", llvmModule.get());
}

template<typename BuilderT>
static Value* emitReadByte(BuilderT& b) {
    Function* readFn = getOrDeclareRead();
    Type* i8  = Type::getInt8Ty(ctx);
    Type* i32 = Type::getInt32Ty(ctx);
    Type* i64 = Type::getInt64Ty(ctx);
    Value* buf = b.CreateAlloca(i8, nullptr, "rdbuf");
    Value* n   = b.CreateCall(readFn,
        {ConstantInt::get(i32, 0), buf, ConstantInt::get(i64, 1)});
    Value* ok  = b.CreateICmpEQ(n, ConstantInt::get(i64, 1), "read_ok");
    Value* ch  = b.CreateLoad(i8, buf, "rd_ch");
    return b.CreateSelect(ok, ch, ConstantInt::get(i8, 0), "rd_byte");
}

// Emite loop de skip de whitespace em bbSkip; salta para bbAfter quando
// encontra o primeiro char não-ws, deixando-o em laAlloc.
static void emitSkipWS(IRBuilder<>& b, Function* fn,
                       BasicBlock* bbSkip, BasicBlock* bbAfter,
                       Value* laAlloc) {
    b.SetInsertPoint(bbSkip);
    Type* i8 = Type::getInt8Ty(ctx);
    Value* c    = emitReadByte(b);
    b.CreateStore(c, laAlloc);
    Value* isWs = b.CreateOr(
        b.CreateOr(b.CreateICmpEQ(c, ConstantInt::get(i8,  9)),
                   b.CreateICmpEQ(c, ConstantInt::get(i8, 10))),
        b.CreateOr(b.CreateICmpEQ(c, ConstantInt::get(i8, 13)),
                   b.CreateICmpEQ(c, ConstantInt::get(i8, 32))));
    b.CreateCondBr(isWs, bbSkip, bbAfter);
}

// Emite loop de acumulação de dígitos decimais em i64.
// bbDigLoop: checa se laAlloc é dígito; se sim, vai p/ bbDigAcc, senão para bbDone.
// bbDigAcc: acumula acc = acc*10 + dig, lê próximo byte, volta p/ bbDigLoop.
static void emitDigitLoop(IRBuilder<>& b, Function* fn,
                          BasicBlock* bbDigLoop, BasicBlock* bbDigAcc,
                          BasicBlock* bbDone,
                          Value* laAlloc, Value* accAlloc) {
    Type* i8  = Type::getInt8Ty(ctx);
    Type* i64 = Type::getInt64Ty(ctx);

    b.SetInsertPoint(bbDigLoop);
    {
        Value* c = b.CreateLoad(i8, laAlloc, "la");
        b.CreateCondBr(
            b.CreateAnd(b.CreateICmpUGE(c, ConstantInt::get(i8, (uint64_t)'0')),
                        b.CreateICmpULE(c, ConstantInt::get(i8, (uint64_t)'9'))),
            bbDigAcc, bbDone);
    }
    b.SetInsertPoint(bbDigAcc);
    {
        Value* c   = b.CreateLoad(i8, laAlloc, "la");
        Value* dig = b.CreateSub(b.CreateZExt(c, i64), ConstantInt::get(i64, '0'));
        Value* acc = b.CreateLoad(i64, accAlloc, "acc");
        b.CreateStore(
            b.CreateAdd(b.CreateMul(acc, ConstantInt::get(i64, 10)), dig),
            accAlloc);
        b.CreateStore(emitReadByte(b), laAlloc);
        b.CreateBr(bbDigLoop);
    }
}

static Function* getOrBuildNovaScanf() {
    if (Function* f = llvmModule->getFunction("nova_scanf")) return f;

    Type* i1    = Type::getInt1Ty(ctx);
    Type* i8    = Type::getInt8Ty(ctx);
    Type* i32   = Type::getInt32Ty(ctx);
    Type* i64   = Type::getInt64Ty(ctx);
    Type* dbl   = Type::getDoubleTy(ctx);
    Type* flt   = Type::getFloatTy(ctx);
    Type* ptrTy = PointerType::getUnqual(ctx);

    FunctionType* ft = FunctionType::get(i32, {ptrTy}, true);
    Function* fn = Function::Create(ft, Function::InternalLinkage,
                                    "nova_scanf", llvmModule.get());
    fn->addFnAttr(Attribute::NoUnwind);
    Argument* fmtArg = &*fn->arg_begin();
    fmtArg->setName("fmt");

    // ── BasicBlocks ──────────────────────────────────────────────────────
    BasicBlock* bbEntry   = BasicBlock::Create(ctx, "entry",     fn);
    BasicBlock* bbLoop    = BasicBlock::Create(ctx, "loop",      fn);
    BasicBlock* bbCheck   = BasicBlock::Create(ctx, "check",     fn);
    BasicBlock* bbLitSkip = BasicBlock::Create(ctx, "lit_skip",  fn);
    BasicBlock* bbReadSpc = BasicBlock::Create(ctx, "read_spc",  fn);
    BasicBlock* bbLong    = BasicBlock::Create(ctx, "long_pfx",  fn);
    BasicBlock* bbFmtD    = BasicBlock::Create(ctx, "fmt_d",     fn);
    BasicBlock* bbFmtLD   = BasicBlock::Create(ctx, "fmt_ld",    fn);
    BasicBlock* bbFmtU    = BasicBlock::Create(ctx, "fmt_u",     fn);
    BasicBlock* bbFmtLU   = BasicBlock::Create(ctx, "fmt_lu",    fn);
    BasicBlock* bbFmtF    = BasicBlock::Create(ctx, "fmt_f",     fn);
    BasicBlock* bbFmtLF   = BasicBlock::Create(ctx, "fmt_lf",    fn);
    BasicBlock* bbFmtC    = BasicBlock::Create(ctx, "fmt_c",     fn);
    BasicBlock* bbFmtS    = BasicBlock::Create(ctx, "fmt_s",     fn);
    BasicBlock* bbFmtPct  = BasicBlock::Create(ctx, "fmt_pct",   fn);
    BasicBlock* bbFmtUnk  = BasicBlock::Create(ctx, "fmt_unk",   fn);
    BasicBlock* bbNext    = BasicBlock::Create(ctx, "next",      fn);
    BasicBlock* bbRet     = BasicBlock::Create(ctx, "ret",       fn);

    // %d/%i sub-blocks
    BasicBlock* bbDSkip   = BasicBlock::Create(ctx, "d_skip",    fn);
    BasicBlock* bbDSign   = BasicBlock::Create(ctx, "d_sign",    fn);
    BasicBlock* bbDSignCs = BasicBlock::Create(ctx, "d_sign_cs", fn);
    BasicBlock* bbDLoop   = BasicBlock::Create(ctx, "d_loop",    fn);
    BasicBlock* bbDAccum  = BasicBlock::Create(ctx, "d_accum",   fn);
    BasicBlock* bbDStore  = BasicBlock::Create(ctx, "d_store",   fn);

    // %ld/%li sub-blocks
    BasicBlock* bbLDSkip  = BasicBlock::Create(ctx, "ld_skip",   fn);
    BasicBlock* bbLDSign  = BasicBlock::Create(ctx, "ld_sign",   fn);
    BasicBlock* bbLDSignCs= BasicBlock::Create(ctx, "ld_sign_cs",fn);
    BasicBlock* bbLDLoop  = BasicBlock::Create(ctx, "ld_loop",   fn);
    BasicBlock* bbLDAccum = BasicBlock::Create(ctx, "ld_accum",  fn);
    BasicBlock* bbLDStore = BasicBlock::Create(ctx, "ld_store",  fn);

    // %u sub-blocks
    BasicBlock* bbUSkip   = BasicBlock::Create(ctx, "u_skip",    fn);
    BasicBlock* bbULoop   = BasicBlock::Create(ctx, "u_loop",    fn);
    BasicBlock* bbUAccum  = BasicBlock::Create(ctx, "u_accum",   fn);
    BasicBlock* bbUStore  = BasicBlock::Create(ctx, "u_store",   fn);

    // %lu sub-blocks
    BasicBlock* bbLUSkip  = BasicBlock::Create(ctx, "lu_skip",   fn);
    BasicBlock* bbLULoop  = BasicBlock::Create(ctx, "lu_loop",   fn);
    BasicBlock* bbLUAccum = BasicBlock::Create(ctx, "lu_accum",  fn);
    BasicBlock* bbLUStore = BasicBlock::Create(ctx, "lu_store",  fn);

    // %f/%lf sub-blocks
    BasicBlock* bbFSkip   = BasicBlock::Create(ctx, "f_skip",      fn);
    BasicBlock* bbFSign   = BasicBlock::Create(ctx, "f_sign",      fn);
    BasicBlock* bbFSignCs = BasicBlock::Create(ctx, "f_sign_cs",   fn);
    BasicBlock* bbFILoop  = BasicBlock::Create(ctx, "f_int_loop",  fn);
    BasicBlock* bbFIAccum = BasicBlock::Create(ctx, "f_int_accum", fn);
    BasicBlock* bbFDot    = BasicBlock::Create(ctx, "f_dot",       fn);
    BasicBlock* bbFDotCon = BasicBlock::Create(ctx, "f_dot_con",   fn);
    BasicBlock* bbFFLoop  = BasicBlock::Create(ctx, "f_frac_loop", fn);
    BasicBlock* bbFFAccum = BasicBlock::Create(ctx, "f_frac_accum",fn);
    BasicBlock* bbFStFlt  = BasicBlock::Create(ctx, "f_st_flt",    fn);
    BasicBlock* bbFStDbl  = BasicBlock::Create(ctx, "f_st_dbl",    fn);

    // %s sub-blocks
    BasicBlock* bbSSkip   = BasicBlock::Create(ctx, "s_skip",    fn);
    BasicBlock* bbSLoop   = BasicBlock::Create(ctx, "s_loop",    fn);
    BasicBlock* bbSAccum  = BasicBlock::Create(ctx, "s_accum",   fn);
    BasicBlock* bbSStore  = BasicBlock::Create(ctx, "s_store",   fn);

    // ── Entry ────────────────────────────────────────────────────────────
    IRBuilder<> b(bbEntry);

    ArrayType* vaTy = ArrayType::get(i8, 24);
    Value* vaList = b.CreateAlloca(vaTy, nullptr, "va_list");
    Value* vaPtr  = b.CreateBitCast(vaList, ptrTy, "va_ptr");
    b.CreateCall(Intrinsic::getDeclaration(llvmModule.get(), Intrinsic::vastart, {ptrTy}),
                 {vaPtr});

    Value* convAlloc = b.CreateAlloca(i32,  nullptr, "conv");
    Value* iIdx      = b.CreateAlloca(i32,  nullptr, "idx");
    Value* laAlloc   = b.CreateAlloca(i8,   nullptr, "la");

    Value* dVal   = b.CreateAlloca(i64,   nullptr, "d_val");
    Value* dNeg   = b.CreateAlloca(i1,    nullptr, "d_neg");
    Value* dDest  = b.CreateAlloca(ptrTy, nullptr, "d_dest");

    Value* ldVal  = b.CreateAlloca(i64,   nullptr, "ld_val");
    Value* ldNeg  = b.CreateAlloca(i1,    nullptr, "ld_neg");
    Value* ldDest = b.CreateAlloca(ptrTy, nullptr, "ld_dest");

    Value* uVal   = b.CreateAlloca(i64,   nullptr, "u_val");
    Value* uDest  = b.CreateAlloca(ptrTy, nullptr, "u_dest");

    Value* luVal  = b.CreateAlloca(i64,   nullptr, "lu_val");
    Value* luDest = b.CreateAlloca(ptrTy, nullptr, "lu_dest");

    Value* fIVal  = b.CreateAlloca(i64,   nullptr, "f_ival");  // parte inteira (i64)
    Value* fFrac  = b.CreateAlloca(dbl,   nullptr, "f_frac");
    Value* fFDiv  = b.CreateAlloca(dbl,   nullptr, "f_fdiv");
    Value* fNeg   = b.CreateAlloca(i1,    nullptr, "f_neg");
    Value* fDest  = b.CreateAlloca(ptrTy, nullptr, "f_dest");
    Value* fIsLf  = b.CreateAlloca(i1,    nullptr, "f_islf");

    Value* sDest  = b.CreateAlloca(ptrTy, nullptr, "s_dest");
    Value* sIdx   = b.CreateAlloca(i32,   nullptr, "s_idx");

    b.CreateStore(ConstantInt::get(i32, 0), convAlloc);
    b.CreateStore(ConstantInt::get(i32, 0), iIdx);
    b.CreateStore(ConstantInt::get(i8,  0), laAlloc);
    b.CreateBr(bbLoop);

    // ── Loop principal ────────────────────────────────────────────────────
    b.SetInsertPoint(bbLoop);
    {
        Value* idx = b.CreateLoad(i32, iIdx, "idx");
        Value* gep = b.CreateGEP(i8, fmtArg, idx, "gep");
        Value* ch  = b.CreateLoad(i8, gep, "ch");
        b.CreateCondBr(b.CreateICmpEQ(ch, ConstantInt::get(i8, 0)), bbRet, bbCheck);
    }
    b.SetInsertPoint(bbCheck);
    {
        Value* idx = b.CreateLoad(i32, iIdx, "idx");
        Value* gep = b.CreateGEP(i8, fmtArg, idx, "gep2");
        Value* ch  = b.CreateLoad(i8, gep, "ch2");
        b.CreateCondBr(
            b.CreateICmpEQ(ch, ConstantInt::get(i8, (uint64_t)'%')),
            bbReadSpc, bbLitSkip);
    }
    b.SetInsertPoint(bbLitSkip);
    b.CreateBr(bbNext);

    b.SetInsertPoint(bbReadSpc);
    {
        Value* idx  = b.CreateLoad(i32, iIdx, "idx");
        Value* idx1 = b.CreateAdd(idx, ConstantInt::get(i32, 1));
        b.CreateStore(idx1, iIdx);
        Value* gep  = b.CreateGEP(i8, fmtArg, idx1, "spc_gep");
        Value* spc  = b.CreateLoad(i8, gep, "spc");
        SwitchInst* sw = b.CreateSwitch(spc, bbFmtUnk, 10);
        sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'d')), bbFmtD);
        sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'i')), bbFmtD);
        sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'u')), bbFmtU);
        sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'f')), bbFmtF);
        sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'g')), bbFmtF);
        sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'e')), bbFmtF);
        sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'c')), bbFmtC);
        sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'s')), bbFmtS);
        sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'%')), bbFmtPct);
        sw->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'l')), bbLong);
    }
    b.SetInsertPoint(bbLong);
    {
        Value* idx  = b.CreateLoad(i32, iIdx, "idx");
        Value* idx1 = b.CreateAdd(idx, ConstantInt::get(i32, 1));
        b.CreateStore(idx1, iIdx);
        Value* gep  = b.CreateGEP(i8, fmtArg, idx1, "l_gep");
        Value* lspc = b.CreateLoad(i8, gep, "l_spc");
        SwitchInst* sw2 = b.CreateSwitch(lspc, bbFmtUnk, 4);
        sw2->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'d')), bbFmtLD);
        sw2->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'i')), bbFmtLD);
        sw2->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'u')), bbFmtLU);
        sw2->addCase(cast<ConstantInt>(ConstantInt::get(i8, (uint64_t)'f')), bbFmtLF);
    }
    b.SetInsertPoint(bbNext);
    {
        Value* idx = b.CreateLoad(i32, iIdx, "idx");
        b.CreateStore(b.CreateAdd(idx, ConstantInt::get(i32, 1)), iIdx);
        b.CreateBr(bbLoop);
    }

    // ── %d/%i ─────────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtD);
    b.CreateStore(b.CreateVAArg(vaPtr, ptrTy, "va_d"), dDest);
    b.CreateStore(ConstantInt::get(i64, 0), dVal);
    b.CreateStore(ConstantInt::getFalse(ctx), dNeg);
    b.CreateBr(bbDSkip);

    emitSkipWS(b, fn, bbDSkip, bbDSign, laAlloc);

    b.SetInsertPoint(bbDSign);
    {
        Value* c = b.CreateLoad(i8, laAlloc, "la");
        Value* isMinus = b.CreateICmpEQ(c, ConstantInt::get(i8, (uint64_t)'-'));
        b.CreateCondBr(
            b.CreateOr(isMinus, b.CreateICmpEQ(c, ConstantInt::get(i8, (uint64_t)'+'))),
            bbDSignCs, bbDLoop);
    }
    b.SetInsertPoint(bbDSignCs);
    {
        Value* c = b.CreateLoad(i8, laAlloc, "la");
        b.CreateStore(b.CreateICmpEQ(c, ConstantInt::get(i8, (uint64_t)'-')), dNeg);
        b.CreateStore(emitReadByte(b), laAlloc);
        b.CreateBr(bbDLoop);
    }
    emitDigitLoop(b, fn, bbDLoop, bbDAccum, bbDStore, laAlloc, dVal);
    b.SetInsertPoint(bbDStore);
    {
        Value* v   = b.CreateLoad(i64, dVal, "v");
        Value* neg = b.CreateLoad(i1,  dNeg, "neg");
        Value* fin = b.CreateSelect(neg, b.CreateNeg(v), v, "fin");
        b.CreateStore(b.CreateTrunc(fin, i32, "v32"),
                      b.CreateLoad(ptrTy, dDest, "dst"));
        b.CreateStore(b.CreateAdd(b.CreateLoad(i32, convAlloc), ConstantInt::get(i32, 1)), convAlloc);
        b.CreateBr(bbNext);
    }

    // ── %ld/%li ───────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtLD);
    b.CreateStore(b.CreateVAArg(vaPtr, ptrTy, "va_ld"), ldDest);
    b.CreateStore(ConstantInt::get(i64, 0), ldVal);
    b.CreateStore(ConstantInt::getFalse(ctx), ldNeg);
    b.CreateBr(bbLDSkip);

    emitSkipWS(b, fn, bbLDSkip, bbLDSign, laAlloc);

    b.SetInsertPoint(bbLDSign);
    {
        Value* c = b.CreateLoad(i8, laAlloc, "la");
        Value* isMinus = b.CreateICmpEQ(c, ConstantInt::get(i8, (uint64_t)'-'));
        b.CreateCondBr(
            b.CreateOr(isMinus, b.CreateICmpEQ(c, ConstantInt::get(i8, (uint64_t)'+'))),
            bbLDSignCs, bbLDLoop);
    }
    b.SetInsertPoint(bbLDSignCs);
    {
        Value* c = b.CreateLoad(i8, laAlloc, "la");
        b.CreateStore(b.CreateICmpEQ(c, ConstantInt::get(i8, (uint64_t)'-')), ldNeg);
        b.CreateStore(emitReadByte(b), laAlloc);
        b.CreateBr(bbLDLoop);
    }
    emitDigitLoop(b, fn, bbLDLoop, bbLDAccum, bbLDStore, laAlloc, ldVal);
    b.SetInsertPoint(bbLDStore);
    {
        Value* v   = b.CreateLoad(i64, ldVal, "v");
        Value* neg = b.CreateLoad(i1,  ldNeg, "neg");
        Value* fin = b.CreateSelect(neg, b.CreateNeg(v), v, "fin");
        b.CreateStore(fin, b.CreateLoad(ptrTy, ldDest, "dst"));
        b.CreateStore(b.CreateAdd(b.CreateLoad(i32, convAlloc), ConstantInt::get(i32, 1)), convAlloc);
        b.CreateBr(bbNext);
    }

    // ── %u ────────────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtU);
    b.CreateStore(b.CreateVAArg(vaPtr, ptrTy, "va_u"), uDest);
    b.CreateStore(ConstantInt::get(i64, 0), uVal);
    b.CreateBr(bbUSkip);

    emitSkipWS(b, fn, bbUSkip, bbULoop, laAlloc);
    emitDigitLoop(b, fn, bbULoop, bbUAccum, bbUStore, laAlloc, uVal);
    b.SetInsertPoint(bbUStore);
    {
        Value* v = b.CreateLoad(i64, uVal, "v");
        b.CreateStore(b.CreateTrunc(v, i32, "v32"),
                      b.CreateLoad(ptrTy, uDest, "dst"));
        b.CreateStore(b.CreateAdd(b.CreateLoad(i32, convAlloc), ConstantInt::get(i32, 1)), convAlloc);
        b.CreateBr(bbNext);
    }

    // ── %lu ───────────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtLU);
    b.CreateStore(b.CreateVAArg(vaPtr, ptrTy, "va_lu"), luDest);
    b.CreateStore(ConstantInt::get(i64, 0), luVal);
    b.CreateBr(bbLUSkip);

    emitSkipWS(b, fn, bbLUSkip, bbLULoop, laAlloc);
    emitDigitLoop(b, fn, bbLULoop, bbLUAccum, bbLUStore, laAlloc, luVal);
    b.SetInsertPoint(bbLUStore);
    {
        Value* v = b.CreateLoad(i64, luVal, "v");
        b.CreateStore(v, b.CreateLoad(ptrTy, luDest, "dst"));
        b.CreateStore(b.CreateAdd(b.CreateLoad(i32, convAlloc), ConstantInt::get(i32, 1)), convAlloc);
        b.CreateBr(bbNext);
    }

    // ── %f / %lf ──────────────────────────────────────────────────────────
    // Ambos compartilham os mesmos sub-blocos de parse. fIsLf decide o store.
    b.SetInsertPoint(bbFmtF);
    b.CreateStore(b.CreateVAArg(vaPtr, ptrTy, "va_f"), fDest);
    b.CreateStore(ConstantInt::get(i64, 0), fIVal);
    b.CreateStore(ConstantFP::get(dbl, 0.0), fFrac);
    b.CreateStore(ConstantFP::get(dbl, 10.0), fFDiv);
    b.CreateStore(ConstantInt::getFalse(ctx), fNeg);
    b.CreateStore(ConstantInt::getFalse(ctx), fIsLf);
    b.CreateBr(bbFSkip);

    b.SetInsertPoint(bbFmtLF);
    b.CreateStore(b.CreateVAArg(vaPtr, ptrTy, "va_lf"), fDest);
    b.CreateStore(ConstantInt::get(i64, 0), fIVal);
    b.CreateStore(ConstantFP::get(dbl, 0.0), fFrac);
    b.CreateStore(ConstantFP::get(dbl, 10.0), fFDiv);
    b.CreateStore(ConstantInt::getFalse(ctx), fNeg);
    b.CreateStore(ConstantInt::getTrue(ctx), fIsLf);
    b.CreateBr(bbFSkip);

    emitSkipWS(b, fn, bbFSkip, bbFSign, laAlloc);

    b.SetInsertPoint(bbFSign);
    {
        Value* c = b.CreateLoad(i8, laAlloc, "la");
        Value* isMinus = b.CreateICmpEQ(c, ConstantInt::get(i8, (uint64_t)'-'));
        b.CreateCondBr(
            b.CreateOr(isMinus, b.CreateICmpEQ(c, ConstantInt::get(i8, (uint64_t)'+'))),
            bbFSignCs, bbFILoop);
    }
    b.SetInsertPoint(bbFSignCs);
    {
        Value* c = b.CreateLoad(i8, laAlloc, "la");
        b.CreateStore(b.CreateICmpEQ(c, ConstantInt::get(i8, (uint64_t)'-')), fNeg);
        b.CreateStore(emitReadByte(b), laAlloc);
        b.CreateBr(bbFILoop);
    }
    // Parte inteira do float (acumula em fIVal como i64, converte depois)
    emitDigitLoop(b, fn, bbFILoop, bbFIAccum, bbFDot, laAlloc, fIVal);
    b.SetInsertPoint(bbFDot);
    {
        Value* c = b.CreateLoad(i8, laAlloc, "la");
        b.CreateCondBr(
            b.CreateICmpEQ(c, ConstantInt::get(i8, (uint64_t)'.')),
            bbFDotCon, bbFStFlt);
    }
    b.SetInsertPoint(bbFDotCon);
    b.CreateStore(emitReadByte(b), laAlloc);
    b.CreateBr(bbFFLoop);
    b.SetInsertPoint(bbFFLoop);
    {
        Value* c = b.CreateLoad(i8, laAlloc, "la");
        b.CreateCondBr(
            b.CreateAnd(b.CreateICmpUGE(c, ConstantInt::get(i8, (uint64_t)'0')),
                        b.CreateICmpULE(c, ConstantInt::get(i8, (uint64_t)'9'))),
            bbFFAccum, bbFStFlt);
    }
    b.SetInsertPoint(bbFFAccum);
    {
        Value* c   = b.CreateLoad(i8, laAlloc, "la");
        Value* dig = b.CreateUIToFP(
            b.CreateSub(b.CreateZExt(c, i32), ConstantInt::get(i32, '0')), dbl);
        Value* div = b.CreateLoad(dbl, fFDiv, "div");
        Value* fr  = b.CreateLoad(dbl, fFrac, "fr");
        b.CreateStore(b.CreateFAdd(fr, b.CreateFDiv(dig, div)), fFrac);
        b.CreateStore(b.CreateFMul(div, ConstantFP::get(dbl, 10.0)), fFDiv);
        b.CreateStore(emitReadByte(b), laAlloc);
        b.CreateBr(bbFFLoop);
    }
    // bbFStFlt: calcula valor final e bifurca para store float ou double
    {
        BasicBlock* bbDoStoreFlt = BasicBlock::Create(ctx, "do_st_flt", fn);
        BasicBlock* bbDoStoreDbl = BasicBlock::Create(ctx, "do_st_dbl", fn);

        b.SetInsertPoint(bbFStFlt);
        Value* ival  = b.CreateSIToFP(b.CreateLoad(i64, fIVal, "iv"), dbl, "iv_f");
        Value* frac  = b.CreateLoad(dbl, fFrac, "fr");
        Value* total = b.CreateFAdd(ival, frac, "tot");
        Value* neg   = b.CreateLoad(i1,  fNeg,  "neg");
        Value* final = b.CreateSelect(neg, b.CreateFNeg(total), total, "fin");
        Value* islf  = b.CreateLoad(i1,  fIsLf, "islf");
        Value* dst   = b.CreateLoad(ptrTy, fDest, "dst");
        Value* asF   = b.CreateFPTrunc(final, flt, "asF");
        b.CreateCondBr(islf, bbDoStoreDbl, bbDoStoreFlt);

        IRBuilder<> bF(bbDoStoreFlt);
        bF.CreateStore(asF, dst);
        bF.CreateStore(bF.CreateAdd(bF.CreateLoad(i32, convAlloc), ConstantInt::get(i32, 1)), convAlloc);
        bF.CreateBr(bbNext);

        IRBuilder<> bD(bbDoStoreDbl);
        bD.CreateStore(final, dst);
        bD.CreateStore(bD.CreateAdd(bD.CreateLoad(i32, convAlloc), ConstantInt::get(i32, 1)), convAlloc);
        bD.CreateBr(bbNext);
    }
    // bbFStDbl não é alcançado pelo fluxo acima, mas precisa de terminador
    b.SetInsertPoint(bbFStDbl);
    b.CreateBr(bbNext);

    // ── %c ────────────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtC);
    {
        Value* dst = b.CreateVAArg(vaPtr, ptrTy, "va_c");
        b.CreateStore(emitReadByte(b), dst);
        b.CreateStore(b.CreateAdd(b.CreateLoad(i32, convAlloc), ConstantInt::get(i32, 1)), convAlloc);
        b.CreateBr(bbNext);
    }

    // ── %s ────────────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtS);
    b.CreateStore(b.CreateVAArg(vaPtr, ptrTy, "va_s"), sDest);
    b.CreateStore(ConstantInt::get(i32, 0), sIdx);
    b.CreateBr(bbSSkip);

    emitSkipWS(b, fn, bbSSkip, bbSLoop, laAlloc);

    b.SetInsertPoint(bbSLoop);
    {
        Value* c = b.CreateLoad(i8, laAlloc, "la");
        Value* isEnd = b.CreateOr(
            b.CreateOr(b.CreateICmpEQ(c, ConstantInt::get(i8,  0)),
                       b.CreateICmpEQ(c, ConstantInt::get(i8, 32))),
            b.CreateOr(b.CreateICmpEQ(c, ConstantInt::get(i8,  9)),
                       b.CreateOr(b.CreateICmpEQ(c, ConstantInt::get(i8, 10)),
                                  b.CreateICmpEQ(c, ConstantInt::get(i8, 13)))));
        b.CreateCondBr(isEnd, bbSStore, bbSAccum);
    }
    b.SetInsertPoint(bbSAccum);
    {
        Value* c   = b.CreateLoad(i8, laAlloc, "la");
        Value* dst = b.CreateLoad(ptrTy, sDest, "dst");
        Value* si  = b.CreateLoad(i32, sIdx, "si");
        b.CreateStore(c, b.CreateGEP(i8, dst, si, "s_gep"));
        b.CreateStore(b.CreateAdd(si, ConstantInt::get(i32, 1)), sIdx);
        b.CreateStore(emitReadByte(b), laAlloc);
        b.CreateBr(bbSLoop);
    }
    b.SetInsertPoint(bbSStore);
    {
        Value* dst = b.CreateLoad(ptrTy, sDest, "dst");
        Value* si  = b.CreateLoad(i32, sIdx, "si");
        b.CreateStore(ConstantInt::get(i8, 0), b.CreateGEP(i8, dst, si, "null_gep"));
        b.CreateStore(b.CreateAdd(b.CreateLoad(i32, convAlloc), ConstantInt::get(i32, 1)), convAlloc);
        b.CreateBr(bbNext);
    }

    // ── %% ───────────────────────────────────────────────────────────────
    b.SetInsertPoint(bbFmtPct);
    emitReadByte(b); // consome um '%' do stdin
    b.CreateBr(bbNext);

    // ── especificador desconhecido ─────────────────────────────────────────
    b.SetInsertPoint(bbFmtUnk);
    b.CreateBr(bbNext);

    // ── ret ───────────────────────────────────────────────────────────────
    b.SetInsertPoint(bbRet);
    b.CreateCall(Intrinsic::getDeclaration(llvmModule.get(), Intrinsic::vaend, {ptrTy}),
                 {vaPtr});
    b.CreateRet(b.CreateLoad(i32, convAlloc, "result"));

    verifyFunction(*fn);
    return fn;
}


// Resolve um nome possivelmente encadeado (ex: "L.start" ou "L.start.inner")
// para o ponteiro do struct e seu tipo, navegando pelos campos via GEP.
// Retorna {ponteiro, typeName} do struct final, ou {nullptr, ""} se não encontrado.
static std::pair<Value*, std::string> resolveNestedStructPtr(const std::string& varName, int line, int col) {
    // Divide o nome pelos '.'
    std::vector<std::string> parts;
    {
        std::string cur;
        for (char c : varName) {
            if (c == '.') { if (!cur.empty()) parts.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) parts.push_back(cur);
    }

    if (parts.empty()) return std::make_pair((Value*)nullptr, std::string(""));

    // Resolve a raiz (primeiro componente)
    Value* ptr = nullptr;
    std::string typeName;
    {
        auto lit = localStructs.find(parts[0]);
        if (lit != localStructs.end()) {
            ptr      = lit->second.first;
            typeName = lit->second.second;
        } else {
            auto git = globalStructs.find(parts[0]);
            if (git != globalStructs.end()) {
                ptr      = git->second.first;
                typeName = git->second.second;
            }
        }
    }
    if (!ptr) return std::make_pair((Value*)nullptr, std::string(""));

    // Navega pelos componentes restantes via GEP
    for (size_t i = 1; i < parts.size(); i++) {
        const std::string& fieldName = parts[i];
        auto fit = structFields.find(typeName);
        if (fit == structFields.end()) return std::make_pair((Value*)nullptr, std::string(""));
        auto& fields = fit->second;
        int idx = -1;
        for (int fi = 0; fi < (int)fields.size(); fi++)
            if (fields[fi].name == fieldName) { idx = fi; break; }
        if (idx < 0) return std::make_pair((Value*)nullptr, std::string(""));

        StructType* st = structTypes[typeName];
        ptr = builder.CreateStructGEP(st, ptr, idx, fieldName);

        if (fields[idx].type == DataType::Custom && !fields[idx].structTypeName.empty()) {
            typeName = fields[idx].structTypeName;
        } else {
            // Chegamos num campo primitivo — só faz sentido se for o último componente
            if (i + 1 < parts.size()) return std::make_pair((Value*)nullptr, std::string(""));
            return std::make_pair(ptr, std::string(""));
        }
    }
    return std::make_pair(ptr, typeName);
}

static Value* codegenExpr(const ASTNode* node) {
    if (auto* n = dynamic_cast<const IntLitNode*>(node))
        return ConstantInt::get(Type::getInt32Ty(ctx), n->value);

    if (auto* n = dynamic_cast<const LongLitNode*>(node))
        return ConstantInt::get(Type::getInt64Ty(ctx), (uint64_t)n->value);

    if (auto* n = dynamic_cast<const FloatLitNode*>(node)) {
        // Usa double por padrão para preservar precisão máxima do literal.
        // O tipo final é resolvido pelo contexto (VarDeclNode, cast, etc.)
        // Se o valor for usado em contexto float, o store fará a conversão.
        APFloat apVal(APFloat::IEEEdouble(), n->raw);
        return ConstantFP::get(Type::getDoubleTy(ctx), apVal);
    }

    if (auto* n = dynamic_cast<const StringLitNode*>(node))
        return builder.CreateGlobalStringPtr(n->value);

    // char literal — emite i8
    if (auto* n = dynamic_cast<const CharLitNode*>(node))
        return ConstantInt::get(Type::getInt8Ty(ctx), (unsigned char)n->value);

    // Busca local primeiro, depois global
    // Busca local primeiro, depois global
    if (auto* n = dynamic_cast<const VarNode*>(node)) {
        // Array local passado como argumento: retorna ponteiro para o primeiro elemento
        auto arrIt = localArrays.find(n->name);
        if (arrIt != localArrays.end()) {
            Type* arrType = arrIt->second.second;
            Value* arrPtr = arrIt->second.first;
            return builder.CreateGEP(arrType, arrPtr,
                {ConstantInt::get(Type::getInt32Ty(ctx), 0),
                 ConstantInt::get(Type::getInt32Ty(ctx), 0)}, n->name + ".ptr");
        }
        auto localIt = localValues.find(n->name);
        if (localIt != localValues.end()) {
            auto usageIt = localVarUsage.find(n->name);
            if (usageIt != localVarUsage.end()) usageIt->second.used = true;
            AllocaInst* alloca = localIt->second;
            return builder.CreateLoad(alloca->getAllocatedType(), alloca, n->name);
        }
        auto globalIt = globalValues.find(n->name);
        if (globalIt != globalValues.end()) {
            GlobalVariable* gv = globalIt->second;
            return builder.CreateLoad(gv->getValueType(), gv, n->name);
        }
        // Tenta também sem namespace: "geo::PI" → "PI"
        {
            auto sep = n->name.rfind("::");
            if (sep != std::string::npos) {
                std::string shortName = n->name.substr(sep + 2);
                auto shortIt = globalValues.find(shortName);
                if (shortIt != globalValues.end()) {
                    GlobalVariable* gv = shortIt->second;
                    return builder.CreateLoad(gv->getValueType(), gv, shortName);
                }
            }
        }
        // Check for enum variable
        {
            auto sep = n->name.rfind("::");
            std::string baseName = (sep != std::string::npos) ? n->name.substr(sep + 2) : n->name;
            std::string enumName = (sep != std::string::npos) ? n->name.substr(0, sep) : "";

            // Look for enum value in enumValues map
            std::string lookupName = n->name;
            if (!enumName.empty() && enumValues.count(lookupName)) {
                // Fully qualified enum value (e.g., Color::RED)
                auto& enumEntries = enumValues[lookupName];
                // Find the matching enum entry
                for (auto& [enumValueName, enumValue] : enumEntries) {
                    // This is simplified - we need to match the varName against enumValueName
                    // For now, we'll assume the varName matches the enum value name
                    // A proper implementation would need to parse the enum access
                    return ConstantInt::get(Type::getInt32Ty(ctx), enumValue);
                }
            } else if (!enumName.empty()) {
                // Try to find as enum.TYPE or just check if it's an enum value in current namespace
                // For simplicity in this implementation, we'll treat unknown vars as potential enum values
                // A proper implementation would need better enum scoping rules
                if (enumValues.count(enumName)) {
                    auto& enumEntries = enumValues[enumName];
                    // Look for the enum value by name (n->name without namespace)
                    for (auto& [enumValueName, enumValue] : enumEntries) {
                        if (enumValueName == baseName) {
                            return ConstantInt::get(Type::getInt32Ty(ctx), enumValue);
                        }
                    }
                }
            }
        }

        reportError(sourceFile, n->line, n->col,
                    "The variable '" + n->name + "' was not declared in this scope",
                    getSourceLine(n->line), (int)n->name.size());
        return nullptr;
    }

    if (auto* n = dynamic_cast<const UnaryOpNode*>(node)) {
        // '&' / '&mut' geram um ponteiro bruto como inteiro (i64).
        if (n->op == "&" || n->op == "&mut") {
            Value* ptr = nullptr;
            if (auto* vn = dynamic_cast<const VarNode*>(n->operand.get())) {
                auto lit = localValues.find(vn->name);
                if (lit != localValues.end()) ptr = lit->second;
                else {
                    auto git = globalValues.find(vn->name);
                    if (git != globalValues.end()) ptr = git->second;
                }
            } else if (auto* an = dynamic_cast<const ArrayAccessNode*>(n->operand.get())) {
                Value* idx = codegenExpr(an->index.get());
                auto lit = localArrays.find(an->name);
                if (lit != localArrays.end())
                    ptr = builder.CreateGEP(lit->second.second, lit->second.first,
                                            {ConstantInt::get(Type::getInt32Ty(ctx), 0), idx});
                else {
                    auto git = globalArrays.find(an->name);
                    if (git != globalArrays.end())
                        ptr = builder.CreateGEP(git->second.second, git->second.first,
                                                {ConstantInt::get(Type::getInt32Ty(ctx), 0), idx});
                }
            } else if (auto* fan = dynamic_cast<const FieldAccessNode*>(n->operand.get())) {
                auto [sPtr, tName] = resolveNestedStructPtr(fan->varName, n->line, n->col);
                if (sPtr) {
                    int idx = getFieldIndex(tName, fan->fieldName, n->line, n->col);
                    ptr = builder.CreateStructGEP(structTypes[tName], sPtr, idx, fan->fieldName);
                }
            } else if (auto* dn = dynamic_cast<const DerefNode*>(n->operand.get())) {
                ptr = codegenExpr(dn->operand.get());
                if (ptr->getType()->isIntegerTy(64))
                    ptr = builder.CreateIntToPtr(ptr, PointerType::getUnqual(llvmType(dn->type)));
            }

            if (ptr)
                return builder.CreatePtrToInt(ptr, Type::getInt64Ty(ctx), "addrof");

            reportError(sourceFile, n->line, n->col,
                        "cannot take address of this expression",
                        getSourceLine(n->line), 1,
                        "you can only take the address of variables, array elements, or struct fields");
            return nullptr;
        }
        Value* val = codegenExpr(n->operand.get());
        // Converte para i1: val != 0
        Value* asBool = val->getType()->isFloatTy()
            ? builder.CreateFCmpONE(val, ConstantFP::get(val->getType(), 0.0))
            : builder.CreateICmpNE(val, ConstantInt::get(val->getType(), 0));
        // !: inverte o i1 e estende para i32
        Value* notVal = builder.CreateNot(asBool);
        return builder.CreateZExt(notVal, Type::getInt32Ty(ctx));
    }

    if (auto* n = dynamic_cast<const BinaryOpNode*>(node)) {
        // Curto-circuito para && e ||
        if (n->op == "&&" || n->op == "||") {
            Function* fn = builder.GetInsertBlock()->getParent();
            Value* lVal = codegenExpr(n->left.get());
            Value* lBool = lVal->getType()->isFloatTy()
                ? builder.CreateFCmpONE(lVal, ConstantFP::get(lVal->getType(), 0.0))
                : builder.CreateICmpNE(lVal, ConstantInt::get(lVal->getType(), 0));

            BasicBlock* evalRhsBB = BasicBlock::Create(ctx, "logic.rhs", fn);
            BasicBlock* mergeBB   = BasicBlock::Create(ctx, "logic.merge", fn);
            BasicBlock* entryBB   = builder.GetInsertBlock();

            if (n->op == "&&") {
                // se lhs é falso, pula direto pro merge com 0
                builder.CreateCondBr(lBool, evalRhsBB, mergeBB);
            } else {
                // se lhs é verdadeiro, pula direto pro merge com 1
                builder.CreateCondBr(lBool, mergeBB, evalRhsBB);
            }

            builder.SetInsertPoint(evalRhsBB);
            Value* rVal = codegenExpr(n->right.get());
            Value* rBool = rVal->getType()->isFloatTy()
                ? builder.CreateFCmpONE(rVal, ConstantFP::get(rVal->getType(), 0.0))
                : builder.CreateICmpNE(rVal, ConstantInt::get(rVal->getType(), 0));
            Value* rExt = builder.CreateZExt(rBool, Type::getInt32Ty(ctx));
            BasicBlock* rhsDoneBB = builder.GetInsertBlock();
            builder.CreateBr(mergeBB);

            builder.SetInsertPoint(mergeBB);
            PHINode* phi = builder.CreatePHI(Type::getInt32Ty(ctx), 2, "logic.result");
            int shortVal = (n->op == "&&") ? 0 : 1;
            phi->addIncoming(ConstantInt::get(Type::getInt32Ty(ctx), shortVal), entryBB);
            phi->addIncoming(rExt, rhsDoneBB);
            return phi;
        }

        Value* l = codegenExpr(n->left.get());
        Value* r = codegenExpr(n->right.get());

        // ── Harmonização de tipos inteiros ────────────────────────────────
        // Garante que l e r tenham o mesmo tipo antes de qualquer operação.
        // Casos comuns: char[i] (i8) comparado com literal 0 (i32),
        //               long (i64) operado com int (i32), etc.
        if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy()) {
            unsigned lBits = l->getType()->getIntegerBitWidth();
            unsigned rBits = r->getType()->getIntegerBitWidth();
            if (lBits < rBits)
                l = builder.CreateSExt(l, r->getType(), "promote");
            else if (rBits < lBits)
                r = builder.CreateSExt(r, l->getType(), "promote");
        }
        // float/double: promove o menor para double
        if (l->getType()->isFloatTy() && r->getType()->isDoubleTy())
            l = builder.CreateFPExt(l, Type::getDoubleTy(ctx), "promote");
        if (l->getType()->isDoubleTy() && r->getType()->isFloatTy())
            r = builder.CreateFPExt(r, Type::getDoubleTy(ctx), "promote");

        bool lIsFloat  = l->getType()->isFloatTy() || l->getType()->isDoubleTy();
        bool rIsFloat  = r->getType()->isFloatTy() || r->getType()->isDoubleTy();
        bool lIsInt    = l->getType()->isIntegerTy();
        bool rIsInt    = r->getType()->isIntegerTy();
        bool lIsString = l->getType()->isPointerTy();
        bool rIsString = r->getType()->isPointerTy();

        // Verificação de tipos em operações aritméticas
        if (n->op == "+" || n->op == "-" || n->op == "*" ||
            n->op == "/" || n->op == "%") {

            if (lIsString || rIsString)
                reportError(sourceFile, n->line, n->col,
                    "operator '" + n->op + "' cannot be applied to string values",
                    getSourceLine(n->line), (int)n->op.size());

            if (lIsFloat && rIsInt)
                reportError(sourceFile, n->line, n->col,
                    "type mismatch: cannot apply '" + n->op + "' to 'float' and 'int'",
                    getSourceLine(n->line), (int)n->op.size());

            if (lIsInt && rIsFloat)
                reportError(sourceFile, n->line, n->col,
                    "type mismatch: cannot apply '" + n->op + "' to 'int' and 'float'",
                    getSourceLine(n->line), (int)n->op.size());
        }

        if (n->op == "%" && (lIsFloat || rIsFloat))
            reportError(sourceFile, n->line, n->col,
                "operator '%' cannot be applied to float values",
                getSourceLine(n->line), (int)n->op.size());

        bool isFloat = l->getType()->isFloatingPointTy() || r->getType()->isFloatingPointTy();

        if (n->op == "+") return isFloat ? builder.CreateFAdd(l, r) : builder.CreateAdd(l, r);
        if (n->op == "-") return isFloat ? builder.CreateFSub(l, r) : builder.CreateSub(l, r);
        if (n->op == "*") return isFloat ? builder.CreateFMul(l, r) : builder.CreateMul(l, r);
        if (n->op == "/") return isFloat ? builder.CreateFDiv(l, r) : builder.CreateSDiv(l, r);
        if (n->op == "%") return isFloat ? builder.CreateFRem(l, r) : builder.CreateSRem(l, r);

        Value* cmp = nullptr;
        if (isFloat) {
            if (n->op == "==") cmp = builder.CreateFCmpOEQ(l, r);
            if (n->op == "!=") cmp = builder.CreateFCmpONE(l, r);
            if (n->op == "<")  cmp = builder.CreateFCmpOLT(l, r);
            if (n->op == ">")  cmp = builder.CreateFCmpOGT(l, r);
            if (n->op == "<=") cmp = builder.CreateFCmpOLE(l, r);
            if (n->op == ">=") cmp = builder.CreateFCmpOGE(l, r);
        } else {
            if (n->op == "==") cmp = builder.CreateICmpEQ(l, r);
            if (n->op == "!=") cmp = builder.CreateICmpNE(l, r);
            if (n->op == "<")  cmp = builder.CreateICmpSLT(l, r);
            if (n->op == ">")  cmp = builder.CreateICmpSGT(l, r);
            if (n->op == "<=") cmp = builder.CreateICmpSLE(l, r);
            if (n->op == ">=") cmp = builder.CreateICmpSGE(l, r);
        }
        if (cmp)
            return builder.CreateZExt(cmp, Type::getInt32Ty(ctx));

        reportError(sourceFile, 0, 0, "The operator '" + n->op + "' is not supported", "");
        return nullptr;
    }
    if (auto* n = dynamic_cast<const DerefNode*>(node)) {
        Value* raw = codegenExpr(n->operand.get());
        if (!raw) return nullptr;

        Type* targetType = llvmType(n->type);
        Type* targetPtrTy = PointerType::getUnqual(targetType);
        Value* ptr = nullptr;

        // Caso comum atual da linguagem: '&x' vira i64 (endereco bruto).
        if (raw->getType()->isIntegerTy(64)) {
            ptr = builder.CreateIntToPtr(raw, targetPtrTy, "deref_ptr");
        } else if (raw->getType()->isPointerTy()) {
            ptr = raw;
            if (ptr->getType() != targetPtrTy)
                ptr = builder.CreateBitCast(ptr, targetPtrTy, "deref_cast");
        } else {
            reportError(sourceFile, n->line, n->col,
                        "cannot dereference a non-pointer value",
                        getSourceLine(n->line), 1,
                        "use '&value' to create a pointer before using '*'");
            return nullptr;
        }

        return builder.CreateLoad(targetType, ptr, "deref_tmp");
    }

    if (auto* n = dynamic_cast<const CallNode*>(node)) {
        calledFunctions.insert(n->name);

        bool isPrintf = nhDeclaredBuiltins.count(n->name) > 0 &&
            (n->name == "printf" ||
             (n->name.size() >= 8 && n->name.substr(n->name.size()-8) == "::printf") ||
             n->name == "std::printf");
        bool isScanf  = nhDeclaredBuiltins.count(n->name) > 0 &&
            (n->name == "scanf" ||
             (n->name.size() >= 7 && n->name.substr(n->name.size()-7) == "::scanf") ||
             n->name == "std::scanf");

        if (isPrintf) {
            Function* npf = getOrBuildNovaPrintf();
            std::vector<Value*> args;
            for (auto& arg : n->args) {
                Value* v = codegenExpr(arg.get());
                if (v->getType()->isFloatTy())
                    v = builder.CreateFPExt(v, Type::getDoubleTy(ctx), "va_promote");
                else if (v->getType()->isIntegerTy() &&
                         v->getType()->getIntegerBitWidth() < 32)
                    v = builder.CreateSExt(v, Type::getInt32Ty(ctx), "va_promote");
                args.push_back(v);
            }
            return builder.CreateCall(npf->getFunctionType(), npf, args);
        }

        if (isScanf) {
            Function* nsf = getOrBuildNovaScanf();
            std::vector<Value*> args;
            for (auto& arg : n->args) {
                Value* v = codegenExpr(arg.get());
                args.push_back(v);
            }
            return builder.CreateCall(nsf->getFunctionType(), nsf, args);
        }

        // Verifica se a função retorna struct — se sim, não pode ser usada como expressão diretamente
        // (deve ser usada via StructVarDeclNode ou StructAssignNode)
        auto sretIt = structReturnFuncs.find(n->name);
        if (sretIt != structReturnFuncs.end()) {
            reportError(sourceFile, n->line, n->col,
                "function '" + n->name + "' returns a struct — assign it: '" +
                sretIt->second + " result = " + n->name + "(...);'",
                getSourceLine(n->line), (int)n->name.size(),
                "struct-returning functions cannot be used inside expressions");
            return nullptr;
        }

        // Gera os argumentos, substituindo struct args por ponteiros
        std::vector<Value*> args;
        auto spIt = structParamFuncs.find(n->name);

        for (size_t i = 0; i < n->args.size(); i++) {
            // Verifica se este param é do tipo struct ou referência
            bool isStructParam = false;
            std::string paramTypeName;
            if (spIt != structParamFuncs.end()) {
                for (auto& entry : spIt->second) {
                    if ((size_t)entry.first == i) { 
                        isStructParam = true; 
                        paramTypeName = entry.second;
                        break; 
                    }
                }
            }

            if (isStructParam) {
                // Se for referência (&T ou &mut T), avalia a expressão diretamente
                if (!paramTypeName.empty() && paramTypeName[0] == '&') {
                    Value* val = codegenExpr(n->args[i].get());
                    // Se o resultado for i64 (ponteiro), converte para ptr do LLVM se necessário
                    if (val->getType()->isIntegerTy(64))
                        val = builder.CreateIntToPtr(val, PointerType::getUnqual(ctx));
                    args.push_back(val);
                } else {
                    // Parâmetro de struct por valor: o argumento deve ser um nome de variável
                    if (auto* varN = dynamic_cast<const VarNode*>(n->args[i].get())) {
                        std::string sType;
                        Value* ptr = getStructPtrValue(varN->name, sType);
                        if (!ptr)
                            reportError(sourceFile, n->line, n->col,
                                        "struct variable '" + varN->name + "' was not declared",
                                        getSourceLine(n->line), (int)varN->name.size());
                        args.push_back(ptr);
                    } else {
                        reportError(sourceFile, n->line, n->col,
                                    "struct argument must be a struct variable name",
                                    getSourceLine(n->line), (int)n->name.size(),
                                    "pass a named struct variable: " + n->name + "(myStruct, ...)");
                        return nullptr;
                    }
                }
            } else {
                args.push_back(codegenExpr(n->args[i].get()));
            }
        }

        // Tenta resolver via tabela de overloads
        Function* fn = resolveOverload(n->name, args, n->line, n->col);
        if (!fn) fn = llvmModule->getFunction(n->name);

        if (!fn)
            reportError(sourceFile, n->line, n->col,
                        "function '" + n->name + "' was not declared",
                        getSourceLine(n->line), (int)n->name.size());

        bool isVariadic = fn->getFunctionType()->isVarArg();

        // Coerção dos parâmetros fixos
        size_t pi = 0;
        for (auto& param : fn->args()) {
            if (pi >= args.size()) break;
            Type* got  = args[pi]->getType();
            Type* want = param.getType();
            if (got != want) {
                if (got->isIntegerTy() && want->isFloatingPointTy())
                    args[pi] = builder.CreateSIToFP(args[pi], want, "coerce");
                else if (got->isFloatTy() && want->isDoubleTy())
                    args[pi] = builder.CreateFPExt(args[pi], want, "coerce");
                else if (got->isDoubleTy() && want->isFloatTy())
                    args[pi] = builder.CreateFPTrunc(args[pi], want, "coerce");
            }
            pi++;
        }

        if (isVariadic) {
            for (size_t vi = pi; vi < args.size(); vi++) {
                Type* t = args[vi]->getType();
                if (t->isFloatTy())
                    args[vi] = builder.CreateFPExt(args[vi], Type::getDoubleTy(ctx), "va_promote");
                else if (t->isIntegerTy() && t->getIntegerBitWidth() < 32)
                    args[vi] = builder.CreateSExt(args[vi], Type::getInt32Ty(ctx), "va_promote");
            }
            return builder.CreateCall(fn->getFunctionType(), fn, args);
        }

        return builder.CreateCall(fn, args);
    }


    if (auto* n = dynamic_cast<const ArrayAccessNode*>(node)) {
        Value* idx = codegenExpr(n->index.get());
        // local primeiro
        auto lit = localArrays.find(n->name);
        if (lit != localArrays.end()) {
            Type* elemType = lit->second.second->getArrayElementType();
            Value* gep = builder.CreateGEP(lit->second.second,
                                           lit->second.first,
                                           {ConstantInt::get(Type::getInt32Ty(ctx), 0), idx});
            // Se elemento é struct, retorna ponteiro (para acesso de campo)
            if (elemType->isStructTy()) return gep;
            return builder.CreateLoad(elemType, gep, n->name);
        }
        auto git = globalArrays.find(n->name);
        if (git != globalArrays.end()) {
            Type* elemType = git->second.second->getArrayElementType();
            Value* gep = builder.CreateGEP(git->second.second,
                                           git->second.first,
                                           {ConstantInt::get(Type::getInt32Ty(ctx), 0), idx});
            // Se elemento é struct, retorna ponteiro (para acesso de campo)
            if (elemType->isStructTy()) return gep;
            return builder.CreateLoad(elemType, gep, n->name);
        }
        reportError(sourceFile, n->line, n->col,
                    "array '" + n->name + "' was not declared in this scope",
                    getSourceLine(n->line), (int)n->name.size());
        return nullptr;
    }

    if (auto* n = dynamic_cast<const FieldAccessNode*>(node)) {
        // Resolve o varName (pode ser "p", "p.inner", "p.a.b", etc.)
        auto [structPtr, typeName] = resolveNestedStructPtr(n->varName, n->line, n->col);
        if (!structPtr)
            reportError(sourceFile, n->line, n->col,
                        "variable '" + n->varName + "' was not declared in this scope",
                        getSourceLine(n->line), (int)n->varName.size());

        int idx = getFieldIndex(typeName, n->fieldName, n->line, n->col);
        StructType* st = structTypes[typeName];
        Value* gep = builder.CreateStructGEP(st, structPtr, idx, n->fieldName);

        // Se o campo é um struct aninhado, retorna o ponteiro (não carrega)
        // para que acessos encadeados adicionais funcionem via resolveNestedStructPtr
        auto& fields = structFields[typeName];
        if (fields[idx].type == DataType::Custom && !fields[idx].structTypeName.empty()) {
            // Registra também em localStructs para compatibilidade com outros caminhos
            std::string nestedKey = n->varName + "." + n->fieldName;
            localStructs[nestedKey] = std::make_pair(gep, fields[idx].structTypeName);
            return gep;
        }

        Type* elemType = st->getElementType(idx);
        return builder.CreateLoad(elemType, gep, n->fieldName);
    }

    if (auto* n = dynamic_cast<const MethodCallNode*>(node)) {
        // Resolve o ponteiro para o struct (local ou global)
        Value* selfPtr = nullptr;
        std::string typeName;
        auto lit = localStructs.find(n->varName);
        if (lit != localStructs.end()) {
            selfPtr  = lit->second.first;
            typeName = lit->second.second;
        } else {
            auto git = globalStructs.find(n->varName);
            if (git != globalStructs.end()) {
                selfPtr  = git->second.first;
                typeName = git->second.second;
            }
        }
        if (!selfPtr)
            reportError(sourceFile, n->line, n->col,
                "variable '" + n->varName + "' was not declared in this scope",
                getSourceLine(n->line), (int)n->varName.size());

        std::string funcName = typeName + "__" + n->methodName;
        Function* fn = llvmModule->getFunction(funcName);
        if (!fn)
            reportError(sourceFile, n->line, n->col,
                "struct '" + typeName + "' has no method '" + n->methodName + "'",
                getSourceLine(n->line), (int)n->methodName.size());

        std::vector<Value*> args;
        args.push_back(selfPtr); // self
        for (auto& arg : n->args)
            args.push_back(codegenExpr(arg.get()));
        return builder.CreateCall(fn, args);
    }

    if (auto* n = dynamic_cast<const CastNode*>(node)) {
        Value* val = codegenExpr(n->expr.get());
        Type* srcType = val->getType();
        Type* dstType = llvmType(n->targetType);

        if (srcType == dstType) return val; // cast no-op

        // ── Warnings de perda de precisão ────────────────────────────────
        bool srcIsDouble = srcType->isDoubleTy();
        bool srcIsFloat  = srcType->isFloatTy();
        bool srcIs64     = srcType->isIntegerTy(64);
        bool dstIsFloat  = dstType->isFloatTy();
        bool dstIs32     = dstType->isIntegerTy(32);

        if (srcIsDouble && dstIsFloat)
            emitWarning(sourceFile, n->line, n->col,
                "cast from 'double' to 'float' loses precision",
                getSourceLine(n->line), 1);
        if (srcIsDouble && (dstIs32 || dstType->isIntegerTy(64)))
            emitWarning(sourceFile, n->line, n->col,
                "cast from 'double' to integer truncates fractional part",
                getSourceLine(n->line), 1);
        if (srcIsFloat && dstIs32)
            emitWarning(sourceFile, n->line, n->col,
                "cast from 'float' to 'int' truncates fractional part",
                getSourceLine(n->line), 1);
        if (srcIs64 && dstIs32)
            emitWarning(sourceFile, n->line, n->col,
                "cast from 'long' to 'int' may truncate value",
                getSourceLine(n->line), 1);
                // ── Gera a instrução de cast correta ─────────────────────────────
            // bool (i1) → int/long/longlong: ZExt (0 ou 1)
            if (srcType->isIntegerTy(1) && dstType->isIntegerTy() && !dstType->isIntegerTy(1))
                return builder.CreateZExt(val, dstType, "cast");
            // bool (i1) → float/double
            if (srcType->isIntegerTy(1) && dstType->isFloatingPointTy())
                return builder.CreateUIToFP(val, dstType, "cast");
            // int/long → bool (i1): val != 0
            if (srcType->isIntegerTy() && !srcType->isIntegerTy(1) && dstType->isIntegerTy(1))
                return builder.CreateICmpNE(val, ConstantInt::get(srcType, 0), "cast");
            // float/double → bool (i1): val != 0.0
            if (srcType->isFloatingPointTy() && dstType->isIntegerTy(1))
                return builder.CreateFCmpONE(val, ConstantFP::get(srcType, 0.0), "cast");


        // ── Gera a instrução de cast correta ─────────────────────────────
        // int/long/longlong → float/double
        if (srcType->isIntegerTy() && dstType->isFloatingPointTy())
            return builder.CreateSIToFP(val, dstType, "cast");
        // float/double → int/long/longlong
        if (srcType->isFloatingPointTy() && dstType->isIntegerTy())
            return builder.CreateFPToSI(val, dstType, "cast");
        // float → double (widening)
        if (srcIsFloat && dstType->isDoubleTy())
            return builder.CreateFPExt(val, dstType, "cast");
        // double → float (narrowing)
        if (srcIsDouble && dstIsFloat)
            return builder.CreateFPTrunc(val, dstType, "cast");
        // int widening (int → long)
        if (srcType->isIntegerTy() && dstType->isIntegerTy() &&
            srcType->getIntegerBitWidth() < dstType->getIntegerBitWidth())
            return builder.CreateSExt(val, dstType, "cast");
        // int narrowing (long → int)
        if (srcType->isIntegerTy() && dstType->isIntegerTy() &&
            srcType->getIntegerBitWidth() > dstType->getIntegerBitWidth())
            return builder.CreateTrunc(val, dstType, "cast");

        // Fallback — tipos iguais ou ponteiros
        return val;
    }

    reportError(sourceFile, 0, 0, "Internal error: unknown node type in codegen", "");
    return nullptr;
}

static int getFieldIndex(const std::string& typeName, const std::string& fieldName, int line, int col) {
    auto it = structFields.find(typeName);
    if (it == structFields.end())
        reportError(sourceFile, line, col, "struct type '" + typeName + "' was not declared", getSourceLine(line), (int)typeName.size());
    auto& fields = it->second;
    for (int i = 0; i < (int)fields.size(); i++)
        if (fields[i].name == fieldName) return i;
    reportError(sourceFile, line, col, "struct '" + typeName + "' has no field '" + fieldName + "'", getSourceLine(line), (int)fieldName.size());
    return -1;
}

// Resolve um argumento de chamada: se for VarNode em localStructs/globalStructs,
// retorna o ponteiro do struct. Caso contrário chama codegenExpr normalmente.
static Value* resolveCallArg(const ASTNode* argNode, const std::string& funcName,
                              int paramIdx, int line, int col) {
    auto spIt = structParamFuncs.find(funcName);
    bool isStructParam = false;
    if (spIt != structParamFuncs.end())
        for (auto& [idx, typeName] : spIt->second)
            if (idx == paramIdx) { isStructParam = true; break; }

    if (isStructParam) {
        if (auto* vn = dynamic_cast<const VarNode*>(argNode)) {
            std::string sType;
            Value* ptr = getStructPtrValue(vn->name, sType);
            if (ptr) return ptr;
            reportError(sourceFile, line, col,
                "struct variable '" + vn->name + "' was not declared in this scope",
                getSourceLine(line), (int)vn->name.size());
        }
        reportError(sourceFile, line, col,
            "argument " + std::to_string(paramIdx+1) + " of '" + funcName +
            "' must be a struct variable",
            getSourceLine(line), (int)funcName.size());
    }
    return codegenExpr(argNode);
}

static void codegenStmt(const ASTNode* node, Function* fn) {
    // ── Declaração de variável de struct local: Point p;  ou  Point p = func(); ──
    if (auto* n = dynamic_cast<const StructVarDeclNode*>(node)) {
        auto it = structTypes.find(n->typeName);
        if (it == structTypes.end())
            reportError(sourceFile, n->line, n->col,
                        "struct type '" + n->typeName + "' was not declared",
                        getSourceLine(n->line), (int)n->typeName.size());
        StructType* st = it->second;
        AllocaInst* alloca = createEntryAlloca(fn, n->varName, st);
        localStructs[n->varName] = std::make_pair(alloca, n->typeName);

        if (!n->initFuncName.empty()) {
            // Point p = @copy:outraVar  — cópia de struct
            if (n->initFuncName.substr(0, 6) == "@copy:") {
                std::string srcName = n->initFuncName.substr(6);
                std::string srcType;
                Value* srcPtr = getStructPtrValue(srcName, srcType);
                if (!srcPtr)
                    reportError(sourceFile, n->line, n->col,
                                "struct variable '" + srcName + "' was not declared in this scope",
                                getSourceLine(n->line), (int)srcName.size());
                copyStruct(alloca, srcPtr, n->typeName);
            } else if (n->initFuncName.substr(0, 9) == "@literal:") {
                // let mut p: Point = { x: 1, y: 2 }  — inicialização literal por campo
                // initFuncName = "@literal:x,y"  initFuncArgs = [expr_x, expr_y]
                std::string fieldList = n->initFuncName.substr(9);
                // Divide os nomes de campos pelo ','
                std::vector<std::string> fieldNames;
                {
                    std::string cur;
                    for (char c : fieldList) {
                        if (c == ',') { if (!cur.empty()) fieldNames.push_back(cur); cur.clear(); }
                        else cur += c;
                    }
                    if (!cur.empty()) fieldNames.push_back(cur);
                }
                auto& fields = structFields[n->typeName];
                for (int i = 0; i < (int)fieldNames.size(); i++) {
                    if (i >= (int)n->initFuncArgs.size()) break;
                    // Acha o índice do campo no struct
                    int fi = -1;
                    for (int k = 0; k < (int)fields.size(); k++)
                        if (fields[k].name == fieldNames[i]) { fi = k; break; }
                    if (fi < 0)
                        reportError(sourceFile, n->line, n->col,
                                    "struct '" + n->typeName + "' has no field '" + fieldNames[i] + "'",
                                    getSourceLine(n->line), (int)fieldNames[i].size());
                    Value* val = codegenExpr(n->initFuncArgs[i].get());
                    Value* gep = builder.CreateStructGEP(st, alloca, fi, fieldNames[i]);
                    Type* fieldType = st->getElementType(fi);
                    // Coerção automática de tipo
                    if (val->getType() != fieldType) {
                        if (val->getType()->isIntegerTy() && fieldType->isFloatingPointTy())
                            val = builder.CreateSIToFP(val, fieldType, "coerce");
                        else if (val->getType()->isFloatTy() && fieldType->isDoubleTy())
                            val = builder.CreateFPExt(val, fieldType, "coerce");
                        else if (val->getType()->isDoubleTy() && fieldType->isFloatTy())
                            val = builder.CreateFPTrunc(val, fieldType, "coerce");
                        else if (val->getType()->isIntegerTy() && fieldType->isIntegerTy()) {
                            if (val->getType()->getIntegerBitWidth() > fieldType->getIntegerBitWidth())
                                val = builder.CreateTrunc(val, fieldType, "trunc");
                            else
                                val = builder.CreateSExt(val, fieldType, "sext");
                        }
                    }
                    builder.CreateStore(val, gep);
                }
            } else {
                // Point p = func(args...)  — função retornando struct
                std::vector<Value*> args;
                for (int i = 0; i < (int)n->initFuncArgs.size(); i++)
                    args.push_back(resolveCallArg(n->initFuncArgs[i].get(), n->initFuncName, i, n->line, n->col));
                Value* result = callStructReturningFunc(
                    fn, n->initFuncName, n->typeName, args, n->line, n->col);
                copyStruct(alloca, result, n->typeName);
            }
        }
        return;
    }

    // ── Atribuição de struct completo: p = func(...) ou p = outraVar ─────────
    if (auto* n = dynamic_cast<const StructAssignNode*>(node)) {
        std::string dstType;
        Value* dstPtr = getStructPtrValue(n->varName, dstType);
        if (!dstPtr)
            reportError(sourceFile, n->line, n->col,
                        "struct variable '" + n->varName + "' was not declared in this scope",
                        getSourceLine(n->line), (int)n->varName.size());

        if (!n->srcVar.empty()) {
            // p = outraVar
            std::string srcType;
            Value* srcPtr = getStructPtrValue(n->srcVar, srcType);
            if (!srcPtr)
                reportError(sourceFile, n->line, n->col,
                            "struct variable '" + n->srcVar + "' was not declared in this scope",
                            getSourceLine(n->line), (int)n->srcVar.size());
            copyStruct(dstPtr, srcPtr, dstType);
        } else {
            // p = func(args...)
            std::vector<Value*> args;
            for (int i = 0; i < (int)n->funcArgs.size(); i++)
                args.push_back(resolveCallArg(n->funcArgs[i].get(), n->funcName, i, n->line, n->col));
            Value* result = callStructReturningFunc(
                fn, n->funcName, dstType, args, n->line, n->col);
            copyStruct(dstPtr, result, dstType);
        }
        return;
    }

    // ── Atribuição de campo: p.x = expr;  ou  p.inner.x = expr; ─────────────
    if (auto* n = dynamic_cast<const FieldAssignNode*>(node)) {
        Value* val = codegenExpr(n->value.get());

        // Resolve o varName (pode ser "p" ou "p.inner" ou "p.a.b")
        auto [structPtr, typeName] = resolveNestedStructPtr(n->varName, n->line, n->col);
        if (!structPtr)
            reportError(sourceFile, n->line, n->col,
                        "variable '" + n->varName + "' was not declared in this scope",
                        getSourceLine(n->line), (int)n->varName.size());

        int idx = getFieldIndex(typeName, n->fieldName, n->line, n->col);
        StructType* st = structTypes[typeName];
        Value* gep = builder.CreateStructGEP(st, structPtr, idx, n->fieldName);

        // Coerção de tipo automática
        Type* fieldType = st->getElementType(idx);
        if (val->getType() != fieldType) {
            if (val->getType()->isIntegerTy() && fieldType->isFloatingPointTy())
                val = builder.CreateSIToFP(val, fieldType, "coerce");
            else if (val->getType()->isFloatTy() && fieldType->isDoubleTy())
                val = builder.CreateFPExt(val, fieldType, "coerce");
            else if (val->getType()->isDoubleTy() && fieldType->isFloatTy())
                val = builder.CreateFPTrunc(val, fieldType, "coerce");
            else if (val->getType()->isIntegerTy() && fieldType->isIntegerTy()) {
                if (val->getType()->getIntegerBitWidth() > fieldType->getIntegerBitWidth())
                    val = builder.CreateTrunc(val, fieldType, "trunc");
                else
                    val = builder.CreateSExt(val, fieldType, "sext");
            }
        }
        builder.CreateStore(val, gep);
        return;
    }

    if (auto* n = dynamic_cast<const ArrayDeclNode*>(node)) {
        // Array de struct: aloca array de StructType
        if (n->type == DataType::Custom && !n->structTypeName.empty()) {
            auto sit = structTypes.find(n->structTypeName);
            if (sit == structTypes.end())
                reportError(sourceFile, n->line, n->col,
                            "struct type '" + n->structTypeName + "' was not declared",
                            getSourceLine(n->line), (int)n->structTypeName.size());
            StructType* st = sit->second;
            ArrayType* arrType = ArrayType::get(st, n->size);
            AllocaInst* alloca = createEntryAlloca(fn, n->name, arrType);
            localArrays[n->name] = {alloca, arrType};
            // Inicialização de struct arrays não é suportada ainda (init deve ser vazio)
            return;
        }
        Type* elemType = llvmType(n->type);
        ArrayType* arrType = ArrayType::get(elemType, n->size);
        AllocaInst* alloca = createEntryAlloca(fn, n->name, arrType);
        localArrays[n->name] = {alloca, arrType};
        for (int i = 0; i < (int)n->init.size(); i++) {
            Value* val = codegenExpr(n->init[i].get());
            // Coerção: ajusta o valor ao tipo do elemento do array
            Type* valType = val->getType();
            if (valType != elemType) {
                // int/long/char → float/double
                if (valType->isIntegerTy() && elemType->isFloatingPointTy())
                    val = builder.CreateSIToFP(val, elemType, "arr_coerce");
                // float → double
                else if (valType->isFloatTy() && elemType->isDoubleTy())
                    val = builder.CreateFPExt(val, elemType, "arr_coerce");
                // double → float
                else if (valType->isDoubleTy() && elemType->isFloatTy())
                    val = builder.CreateFPTrunc(val, elemType, "arr_coerce");
                // int widening: int32 → int64
                else if (valType->isIntegerTy() && elemType->isIntegerTy() &&
                         valType->getIntegerBitWidth() < elemType->getIntegerBitWidth())
                    val = builder.CreateSExt(val, elemType, "arr_coerce");
                // int narrowing: int64 → int8 (long → char)
                else if (valType->isIntegerTy() && elemType->isIntegerTy() &&
                         valType->getIntegerBitWidth() > elemType->getIntegerBitWidth())
                    val = builder.CreateTrunc(val, elemType, "arr_coerce");
            }
            Value* gep = builder.CreateGEP(arrType, alloca,
                {ConstantInt::get(Type::getInt32Ty(ctx), 0),
                 ConstantInt::get(Type::getInt32Ty(ctx), i)});
            builder.CreateStore(val, gep);
        }
        return;
    }

    // arr[i] = func(...)  ou  arr[i] = outraVar  — elemento de array de struct
    if (auto* n = dynamic_cast<const ArrayStructAssignNode*>(node)) {
        Value* idx = codegenExpr(n->index.get());

        // Localiza o array
        Type* arrType = nullptr;
        Value* arrPtr = nullptr;
        auto lit = localArrays.find(n->arrayName);
        if (lit != localArrays.end()) { arrType = lit->second.second; arrPtr = lit->second.first; }
        else {
            auto git = globalArrays.find(n->arrayName);
            if (git != globalArrays.end()) { arrType = git->second.second; arrPtr = git->second.first; }
        }
        if (!arrPtr)
            reportError(sourceFile, n->line, n->col,
                "array '" + n->arrayName + "' was not declared in this scope",
                getSourceLine(n->line), (int)n->arrayName.size());

        Type* elemType = arrType->getArrayElementType();
        if (!elemType->isStructTy())
            reportError(sourceFile, n->line, n->col,
                "array '" + n->arrayName + "' is not an array of structs — "
                "use plain assignment for primitive arrays",
                getSourceLine(n->line), (int)n->arrayName.size());

        // GEP para o elemento destino
        Value* elemPtr = builder.CreateGEP(arrType, arrPtr,
            {ConstantInt::get(Type::getInt32Ty(ctx), 0), idx}, "arr.elem");

        // Descobre o nome do tipo struct
        StructType* st = cast<StructType>(elemType);
        std::string typeName;
        for (auto& [tname, stype] : structTypes)
            if (stype == st) { typeName = tname; break; }

        if (n->funcName.substr(0, 6) == "@copy:") {
            // arr[i] = outraVar;  — cópia de struct
            std::string srcName = n->funcName.substr(6);
            std::string srcType;
            Value* srcPtr = getStructPtrValue(srcName, srcType);
            if (!srcPtr)
                reportError(sourceFile, n->line, n->col,
                    "struct variable '" + srcName + "' was not declared in this scope",
                    getSourceLine(n->line), (int)srcName.size());
            copyStruct(elemPtr, srcPtr, typeName);
        } else {
            // arr[i] = func(...);  — função retornando struct
            std::vector<Value*> args;
            for (int i = 0; i < (int)n->args.size(); i++)
                args.push_back(resolveCallArg(n->args[i].get(), n->funcName, i, n->line, n->col));
            // Chama a função com sret apontando direto para o elemento do array
            std::string llvmFuncName = n->funcName;
            auto oit = overloadTable.find(n->funcName);
            if (oit != overloadTable.end() && !oit->second.empty())
                llvmFuncName = oit->second[0].mangledName;
            Function* fn = llvmModule->getFunction(llvmFuncName);
            if (!fn)
                reportError(sourceFile, n->line, n->col,
                    "function '" + n->funcName + "' was not declared",
                    getSourceLine(n->line), (int)n->funcName.size());
            std::vector<Value*> callArgs;
            callArgs.push_back(elemPtr); // sret direto no elemento do array
            for (auto* v : args) callArgs.push_back(v);
            builder.CreateCall(fn, callArgs);
        }
        return;
    }

    // arr[i].field = val;
    if (auto* n = dynamic_cast<const ArrayFieldAssignNode*>(node)) {
        Value* idx = codegenExpr(n->index.get());
        Value* val = codegenExpr(n->value.get());
        Type* arrType = nullptr; Value* arrPtr = nullptr;
        auto lit = localArrays.find(n->arrayName);
        if (lit != localArrays.end()) { arrType = lit->second.second; arrPtr = lit->second.first; }
        else {
            auto git = globalArrays.find(n->arrayName);
            if (git != globalArrays.end()) { arrType = git->second.second; arrPtr = git->second.first; }
        }
        if (!arrPtr)
            reportError(sourceFile, n->line, n->col,
                "array '" + n->arrayName + "' was not declared",
                getSourceLine(n->line), (int)n->arrayName.size());
        Type* elemType = arrType->getArrayElementType();
        if (!elemType->isStructTy())
            reportError(sourceFile, n->line, n->col,
                "'" + n->arrayName + "' is not an array of structs",
                getSourceLine(n->line), (int)n->arrayName.size());
        StructType* st = cast<StructType>(elemType);
        std::string typeName;
        for (auto& [tname, stype] : structTypes)
            if (stype == st) { typeName = tname; break; }
        int fieldIdx = getFieldIndex(typeName, n->fieldName, n->line, n->col);
        Value* elemPtr = builder.CreateGEP(arrType, arrPtr,
            {ConstantInt::get(Type::getInt32Ty(ctx), 0), idx}, "elem");
        Value* fieldPtr = builder.CreateStructGEP(st, elemPtr, fieldIdx, n->fieldName);
        builder.CreateStore(val, fieldPtr);
        return;
    }

    if (auto* n = dynamic_cast<const ArrayAssignNode*>(node)) {
        Value* idx = codegenExpr(n->index.get());
        Value* val = codegenExpr(n->value.get());

        // Helper: coerce val para elemType
        auto coerce = [&](Type* elemType) -> Value* {
            Type* valType = val->getType();
            if (valType == elemType) return val;
            if (valType->isIntegerTy() && elemType->isFloatingPointTy())
                return builder.CreateSIToFP(val, elemType, "arr_coerce");
            if (valType->isFloatTy() && elemType->isDoubleTy())
                return builder.CreateFPExt(val, elemType, "arr_coerce");
            if (valType->isDoubleTy() && elemType->isFloatTy())
                return builder.CreateFPTrunc(val, elemType, "arr_coerce");
            if (valType->isIntegerTy() && elemType->isIntegerTy()) {
                if (valType->getIntegerBitWidth() < elemType->getIntegerBitWidth())
                    return builder.CreateSExt(val, elemType, "arr_coerce");
                if (valType->getIntegerBitWidth() > elemType->getIntegerBitWidth())
                    return builder.CreateTrunc(val, elemType, "arr_coerce");
            }
            return val;
        };

        auto lit = localArrays.find(n->name);
        if (lit != localArrays.end()) {
            Type* elemType = lit->second.second->getArrayElementType();
            Value* gep = builder.CreateGEP(lit->second.second, lit->second.first,
                {ConstantInt::get(Type::getInt32Ty(ctx), 0), idx});
            builder.CreateStore(coerce(elemType), gep);
            return;
        }
        auto git = globalArrays.find(n->name);
        if (git != globalArrays.end()) {
            Type* elemType = git->second.second->getArrayElementType();
            Value* gep = builder.CreateGEP(git->second.second, git->second.first,
                {ConstantInt::get(Type::getInt32Ty(ctx), 0), idx});
            builder.CreateStore(coerce(elemType), gep);
            return;
        }
        reportError(sourceFile, n->line, n->col,
                    "array '" + n->name + "' was not declared in this scope",
                    getSourceLine(n->line), (int)n->name.size());
        return;
    }

    if (auto* n = dynamic_cast<const DerefAssignNode*>(node)) {
        // n->target é um DerefNode (*p). Precisamos do endereço bruto (p como i64),
        // não do valor desreferenciado — então avaliamos o operando interno diretamente.
        Value* rawTarget = nullptr;
        if (auto* dn = dynamic_cast<const DerefNode*>(n->target.get())) {
            rawTarget = codegenExpr(dn->operand.get());
        } else {
            rawTarget = codegenExpr(n->target.get());
        }
        Value* val = codegenExpr(n->value.get());
        if (!rawTarget || !val) return;

        Type* targetType = llvmType(n->targetType);
        Type* targetPtrTy = PointerType::getUnqual(targetType);
        Value* ptr = nullptr;

        if (rawTarget->getType()->isIntegerTy(64)) {
            ptr = builder.CreateIntToPtr(rawTarget, targetPtrTy, "deref_store_ptr");
        } else if (rawTarget->getType()->isIntegerTy(32)) {
            // i32 guardado como endereço (ex: parâmetro &mut i32 em função com i32 param)
            Value* ext = builder.CreateSExt(rawTarget, Type::getInt64Ty(ctx), "addr_ext");
            ptr = builder.CreateIntToPtr(ext, targetPtrTy, "deref_store_ptr");
        } else if (rawTarget->getType()->isPointerTy()) {
            ptr = rawTarget;
            if (ptr->getType() != targetPtrTy)
                ptr = builder.CreateBitCast(ptr, targetPtrTy, "deref_store_cast");
        } else {
            reportError(sourceFile, n->line, n->col,
                        "cannot assign through a non-pointer value",
                        getSourceLine(n->line), 1,
                        "use '&value' to create a pointer before '*ptr = ...'");
            return;
        }

        if (val->getType() != targetType) {
            if (val->getType()->isIntegerTy() && targetType->isIntegerTy()) {
                unsigned srcBits = val->getType()->getIntegerBitWidth();
                unsigned dstBits = targetType->getIntegerBitWidth();
                if (srcBits > dstBits) val = builder.CreateTrunc(val, targetType, "deref_trunc");
                else if (srcBits < dstBits) val = builder.CreateSExt(val, targetType, "deref_sext");
            } else if (val->getType()->isIntegerTy() && targetType->isFloatingPointTy()) {
                val = builder.CreateSIToFP(val, targetType, "deref_to_fp");
            } else if (val->getType()->isDoubleTy() && targetType->isFloatTy()) {
                val = builder.CreateFPTrunc(val, targetType, "deref_fp_trunc");
            } else if (val->getType()->isFloatTy() && targetType->isDoubleTy()) {
                val = builder.CreateFPExt(val, targetType, "deref_fp_ext");
            }
        }

        builder.CreateStore(val, ptr);
        return;
    }

    if (auto* n = dynamic_cast<const VarAssignNode*>(node)) {
        // Resolve o alloca/global da variável (local tem prioridade)
        AllocaInst*     localAlloca = nullptr;
        GlobalVariable* globalVar   = nullptr;
        auto localIt  = localValues.find(n->name);
        auto globalIt = globalValues.find(n->name);
        if (localIt != localValues.end())
            localAlloca = localIt->second;
        else if (globalIt != globalValues.end())
            globalVar = globalIt->second;
        else
            reportError(sourceFile, n->line, n->col,
                        "The variable '" + n->name + "' has not been declared (use 'int', 'float', or 'string' to declare it)",
                        getSourceLine(n->line), (int)n->name.size());

        Type* varType = localAlloca ? localAlloca->getAllocatedType()
                                    : globalVar->getValueType();
        bool isFloat  = varType->isFloatingPointTy();

        auto loadCurrent = [&]() -> Value* {
            return localAlloca ? builder.CreateLoad(varType, localAlloca, n->name)
                               : builder.CreateLoad(varType, globalVar,   n->name);
        };
        // store com conversão automática se necessário
        auto store = [&](Value* v) {
            if (v->getType() != varType) {
                if (v->getType()->isIntegerTy() && varType->isIntegerTy()) {
                    unsigned srcBits = v->getType()->getIntegerBitWidth();
                    unsigned dstBits = varType->getIntegerBitWidth();
                    if (srcBits > dstBits)
                        v = builder.CreateTrunc(v, varType, "trunc");
                    else
                        v = builder.CreateSExt(v, varType, "sext");
                } else if (v->getType()->isDoubleTy() && varType->isFloatTy())
                    v = builder.CreateFPTrunc(v, varType, "to_float");
                else if (v->getType()->isFloatTy() && varType->isDoubleTy())
                    v = builder.CreateFPExt(v, varType, "to_double");
            }
            if (localAlloca) builder.CreateStore(v, localAlloca);
            else             builder.CreateStore(v, globalVar);
        };

        Value* newVal = nullptr;
        if (n->op == "=") {
            newVal = codegenExpr(n->expr.get());
            // Harmoniza: ex. atribuir i32 a variável char (i8)
            if (newVal->getType() != varType) {
                if (newVal->getType()->isIntegerTy() && varType->isIntegerTy()) {
                    unsigned srcBits = newVal->getType()->getIntegerBitWidth();
                    unsigned dstBits = varType->getIntegerBitWidth();
                    if (srcBits > dstBits)
                        newVal = builder.CreateTrunc(newVal, varType, "trunc");
                    else
                        newVal = builder.CreateSExt(newVal, varType, "sext");
                } else if (newVal->getType()->isDoubleTy() && varType->isFloatTy())
                    newVal = builder.CreateFPTrunc(newVal, varType, "to_float");
                else if (newVal->getType()->isFloatTy() && varType->isDoubleTy())
                    newVal = builder.CreateFPExt(newVal, varType, "to_double");
                else if (newVal->getType()->isIntegerTy() && varType->isFloatingPointTy())
                    newVal = builder.CreateSIToFP(newVal, varType, "to_fp");
            }
        } else if (n->op == "++") {
            Value* cur = loadCurrent();
            newVal = isFloat ? builder.CreateFAdd(cur, ConstantFP::get(varType, 1.0))
                             : builder.CreateAdd(cur,  ConstantInt::get(varType, 1));
        } else if (n->op == "--") {
            Value* cur = loadCurrent();
            newVal = isFloat ? builder.CreateFSub(cur, ConstantFP::get(varType, 1.0))
                             : builder.CreateSub(cur,  ConstantInt::get(varType, 1));
        } else {
            Value* cur = loadCurrent();
            Value* rhs = codegenExpr(n->expr.get());
            // Harmoniza rhs para o tipo da variável
            if (rhs->getType() != varType) {
                if (rhs->getType()->isIntegerTy() && varType->isIntegerTy()) {
                    unsigned srcBits = rhs->getType()->getIntegerBitWidth();
                    unsigned dstBits = varType->getIntegerBitWidth();
                    if (srcBits > dstBits)
                        rhs = builder.CreateTrunc(rhs, varType, "trunc");
                    else
                        rhs = builder.CreateSExt(rhs, varType, "sext");
                } else if (rhs->getType()->isDoubleTy() && varType->isFloatTy())
                    rhs = builder.CreateFPTrunc(rhs, varType, "rhs_to_float");
                else if (rhs->getType()->isFloatTy() && varType->isDoubleTy())
                    rhs = builder.CreateFPExt(rhs, varType, "rhs_to_double");
                else if (rhs->getType()->isIntegerTy() && varType->isFloatingPointTy())
                    rhs = builder.CreateSIToFP(rhs, varType, "rhs_to_fp");
            }
            if (n->op == "+=") newVal = isFloat ? builder.CreateFAdd(cur, rhs) : builder.CreateAdd(cur, rhs);
            if (n->op == "-=") newVal = isFloat ? builder.CreateFSub(cur, rhs) : builder.CreateSub(cur, rhs);
            if (n->op == "*=") newVal = isFloat ? builder.CreateFMul(cur, rhs) : builder.CreateMul(cur, rhs);
            if (n->op == "/=") newVal = isFloat ? builder.CreateFDiv(cur, rhs) : builder.CreateSDiv(cur, rhs);
        }
        store(newVal);
        return;
    }

    if (auto* n = dynamic_cast<const VarDeclNode*>(node)) {
        Type* type = llvmType(n->type);
        AllocaInst* alloca = createEntryAlloca(fn, n->name, type);
        localValues[n->name] = alloca;
        // Registra para tracking de uso
        localVarUsage[n->name] = {n->line, n->col, false};
        if (n->init) {
            bool declaredFloat  = (n->type == DataType::Float);
            bool declaredDouble = (n->type == DataType::Double);
            bool declaredInt    = (n->type == DataType::Int);
            bool declaredLong   = (n->type == DataType::Long || n->type == DataType::LongLong);
            bool initIsInt      = dynamic_cast<const IntLitNode*>(n->init.get()) != nullptr;
            bool initIsFloat    = dynamic_cast<const FloatLitNode*>(n->init.get()) != nullptr;

            if (declaredFloat && initIsInt)
                reportError(sourceFile, n->line, n->col,
                    "type mismatch: variable '" + n->name + "' is declared as 'float' "
                    "but assigned an 'int' literal — use '" + n->name + " = " +
                    std::to_string(dynamic_cast<const IntLitNode*>(n->init.get())->value) + ".0' instead",
                    getSourceLine(n->line), (int)n->name.size());

            if (declaredDouble && initIsInt)
                reportError(sourceFile, n->line, n->col,
                    "type mismatch: variable '" + n->name + "' is declared as 'double' "
                    "but assigned an 'int' literal — use '" + n->name + " = " +
                    std::to_string(dynamic_cast<const IntLitNode*>(n->init.get())->value) + ".0' instead",
                    getSourceLine(n->line), (int)n->name.size());

            if (declaredInt && initIsFloat)
                reportError(sourceFile, n->line, n->col,
                    "type mismatch: variable '" + n->name + "' is declared as 'int' "
                    "but assigned a 'float' literal — use 'float " + n->name + "' instead",
                    getSourceLine(n->line), (int)n->name.size());

            if (declaredLong && initIsFloat)
                reportError(sourceFile, n->line, n->col,
                    "type mismatch: variable '" + n->name + "' is declared as 'long' "
                    "but assigned a 'float' literal — use 'double " + n->name + "' instead",
                    getSourceLine(n->line), (int)n->name.size());

            Value* val = codegenExpr(n->init.get());
            
            // ── CORREÇÃO: Harmonização completa de tipos no VarDeclNode ──
            if (val->getType() != type) {
                if (val->getType()->isIntegerTy() && type->isIntegerTy()) {
                    unsigned srcBits = val->getType()->getIntegerBitWidth();
                    unsigned dstBits = type->getIntegerBitWidth();
                    if (srcBits > dstBits)
                        val = builder.CreateTrunc(val, type, "trunc");
                    else
                        val = builder.CreateSExt(val, type, "sext");
                } else if (val->getType()->isDoubleTy() && type->isFloatTy()) {
                    val = builder.CreateFPTrunc(val, type, "to_float");
                } else if (val->getType()->isFloatTy() && type->isDoubleTy()) {
                    val = builder.CreateFPExt(val, type, "to_double");
                }
            }
            // ─────────────────────────────────────────────────────────────
            
            builder.CreateStore(val, alloca);
        }
        return;
    }

    if (auto* n = dynamic_cast<const ReturnNode*>(node)) {
        if (!n->expr) {
            builder.CreateRetVoid();
        } else {
            // Verifica se é retorno de struct (varName em localStructs/globalStructs)
            // Caso típico: return p;  onde p é um struct
            if (auto* varN = dynamic_cast<const VarNode*>(n->expr.get())) {
                std::string retType;
                Value* srcPtr = getStructPtrValue(varN->name, retType);
                if (srcPtr) {
                    // Função retorna struct via sret: copia para o primeiro parâmetro (sret)
                    // O primeiro argumento da função é sempre o sret pointer quando retorna struct
                    Function* curFn = builder.GetInsertBlock()->getParent();
                    Value* sretPtr = &*curFn->arg_begin();
                    copyStruct(sretPtr, srcPtr, retType);
                    builder.CreateRetVoid();
                    return;
                }
            }
            Value* val = codegenExpr(n->expr.get());
            builder.CreateRet(val);
        }
        return;
    }


    if (auto* n = dynamic_cast<const IfNode*>(node)) {
        Value* cond = codegenExpr(n->condition.get());
        cond = builder.CreateICmpNE(cond, ConstantInt::get(Type::getInt32Ty(ctx), 0));

        BasicBlock* thenBB  = BasicBlock::Create(ctx, "then", fn);
        BasicBlock* elseBB  = BasicBlock::Create(ctx, "else", fn);
        BasicBlock* mergeBB = BasicBlock::Create(ctx, "merge", fn);

        builder.CreateCondBr(cond, thenBB, elseBB);

        builder.SetInsertPoint(thenBB);
        for (auto& stmt : n->thenBlock)
            codegenStmt(stmt.get(), fn);
        if (!builder.GetInsertBlock()->getTerminator())
            builder.CreateBr(mergeBB);

        builder.SetInsertPoint(elseBB);
        for (auto& stmt : n->elseBlock)
            codegenStmt(stmt.get(), fn);
        if (!builder.GetInsertBlock()->getTerminator())
            builder.CreateBr(mergeBB);

        builder.SetInsertPoint(mergeBB);
        return;
    }

    if (auto* n = dynamic_cast<const WhileNode*>(node)) {
        BasicBlock* condBB  = BasicBlock::Create(ctx, "while.cond", fn);
        BasicBlock* bodyBB  = BasicBlock::Create(ctx, "while.body", fn);
        BasicBlock* afterBB = BasicBlock::Create(ctx, "while.after", fn);

        builder.CreateBr(condBB);

        builder.SetInsertPoint(condBB);
        Value* cond = codegenExpr(n->condition.get());
        cond = builder.CreateICmpNE(cond, ConstantInt::get(Type::getInt32Ty(ctx), 0));
        builder.CreateCondBr(cond, bodyBB, afterBB);

        builder.SetInsertPoint(bodyBB);
        for (auto& stmt : n->body)
            codegenStmt(stmt.get(), fn);
        if (!builder.GetInsertBlock()->getTerminator())
            builder.CreateBr(condBB);

        builder.SetInsertPoint(afterBB);
        return;
    }

    if (auto* n = dynamic_cast<const ForNode*>(node)) {
        // init (pode declarar variável nova no escopo local)
        if (n->init) codegenStmt(n->init.get(), fn);

        BasicBlock* condBB  = BasicBlock::Create(ctx, "for.cond", fn);
        BasicBlock* bodyBB  = BasicBlock::Create(ctx, "for.body", fn);
        BasicBlock* stepBB  = BasicBlock::Create(ctx, "for.step", fn);
        BasicBlock* afterBB = BasicBlock::Create(ctx, "for.after", fn);

        builder.CreateBr(condBB);

        builder.SetInsertPoint(condBB);
        Value* cond = codegenExpr(n->condition.get());
        cond = builder.CreateICmpNE(cond, ConstantInt::get(Type::getInt32Ty(ctx), 0));
        builder.CreateCondBr(cond, bodyBB, afterBB);

        builder.SetInsertPoint(bodyBB);
        for (auto& stmt : n->body)
            codegenStmt(stmt.get(), fn);
        if (!builder.GetInsertBlock()->getTerminator())
            builder.CreateBr(stepBB);

        builder.SetInsertPoint(stepBB);
        if (n->step) codegenStmt(n->step.get(), fn);
        builder.CreateBr(condBB);

        builder.SetInsertPoint(afterBB);
        return;
    }

    // ── asm { ... } — inline assembly com variáveis Nova ────────────────
    if (auto* n = dynamic_cast<const AsmNode*>(node)) {
        // Carrega as variáveis capturadas e monta constraints GCC-style
        // Cada $varname vira um input constraint "r" com o valor carregado
        std::vector<Value*> inputVals;
        std::string asmCode = n->code;
        std::string constraints;

        for (size_t i = 0; i < n->capturedVars.size(); i++) {
            const std::string& varName = n->capturedVars[i];
            Value* val = nullptr;

            // Busca local depois global
            auto lit = localValues.find(varName);
            if (lit != localValues.end())
                val = builder.CreateLoad(lit->second->getAllocatedType(), lit->second, varName);
            else {
                auto git = globalValues.find(varName);
                if (git != globalValues.end())
                    val = builder.CreateLoad(git->second->getValueType(), git->second, varName);
            }

            if (val) {
                inputVals.push_back(val);
                // Substitui $varname por ${i} no código asm
                std::string placeholder = "$" + varName;
                std::string repl = "${" + std::to_string(i) + "}";
                size_t pos = 0;
                while ((pos = asmCode.find(placeholder, pos)) != std::string::npos) {
                    asmCode.replace(pos, placeholder.size(), repl);
                    pos += repl.size();
                }
                if (!constraints.empty()) constraints += ",";
                // Float/double precisam de constraint "x" (XMM), inteiros "r"
                if (val->getType()->isFloatingPointTy())
                    constraints += "x";
                else
                    constraints += "r";
            }
        }

        // Monta o FunctionType com os tipos dos inputs
        std::vector<Type*> inputTypes;
        for (auto* v : inputVals) inputTypes.push_back(v->getType());
        FunctionType* asmFt = FunctionType::get(Type::getVoidTy(ctx), inputTypes, false);
        InlineAsm* inlineAsm = InlineAsm::get(
            asmFt, asmCode, constraints,
            /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);
        builder.CreateCall(inlineAsm, inputVals);
        return;
    }

    // ── ir { ... } — LLVM IR inline com substituição de variáveis Nova ───
    // $varname no texto IR é substituído pelo valor SSA carregado da variável.
    // O IR completo é emitido como InlineAsm void com os valores como inputs,
    // ou — para chamadas de função conhecidas — geramos a call diretamente.
    if (auto* n = dynamic_cast<const IrNode*>(node)) {
        // Estratégia: substituir $varname por referências LLVM temporárias
        // e gerar as instruções diretamente via IRBuilder quando possível.
        // Para IR genérico, fazemos substituição textual e parseamos.

        // Carrega todos os valores capturados
        std::map<std::string, Value*> captures;
        for (auto& varName : n->capturedVars) {
            auto lit = localValues.find(varName);
            if (lit != localValues.end()) {
                captures[varName] = builder.CreateLoad(
                    lit->second->getAllocatedType(), lit->second, varName);
                continue;
            }
            auto git = globalValues.find(varName);
            if (git != globalValues.end()) {
                captures[varName] = builder.CreateLoad(
                    git->second->getValueType(), git->second, varName);
                continue;
            }
            // Pode ser uma string literal global criada pelo codegen
            // Tenta encontrar por nome no módulo
            if (auto* gv = llvmModule->getNamedGlobal(varName))
                captures[varName] = builder.CreateConstGEP2_32(
                    gv->getValueType(), gv, 0, 0, varName);
        }

        // Substitui $varname por um marcador numérico e constrói lista de args
        std::string irText = n->code;
        std::vector<Value*> argVals;
        std::vector<std::string> ordered; // ordem dos $vars no texto

        // Percorre o texto e coleta $vars na ordem de aparição
        for (size_t i = 0; i < irText.size(); i++) {
            if (irText[i] == '$' && i + 1 < irText.size() &&
                (std::isalpha((unsigned char)irText[i+1]) || irText[i+1] == '_')) {
                std::string vname;
                size_t j = i + 1;
                while (j < irText.size() &&
                       (std::isalnum((unsigned char)irText[j]) || irText[j] == '_'))
                    vname += irText[j++];
                // Só adiciona se não já está na lista
                bool found = false;
                for (auto& ov : ordered) if (ov == vname) { found = true; break; }
                if (!found) ordered.push_back(vname);
            }
        }

        for (auto& vname : ordered)
            if (captures.count(vname)) argVals.push_back(captures[vname]);

        // Substitui $varname pelo tipo LLVM correto no texto IR
        // para gerar constraints de InlineAsm
        std::string asmConstraints;
        for (size_t i = 0; i < ordered.size(); i++) {
            std::string placeholder = "$" + ordered[i];
            std::string repl = "${" + std::to_string(i) + "}";
            size_t pos = 0;
            while ((pos = irText.find(placeholder, pos)) != std::string::npos) {
                irText.replace(pos, placeholder.size(), repl);
                pos += repl.size();
            }
            if (i > 0) asmConstraints += ",";
            if (captures.count(ordered[i]) &&
                captures[ordered[i]]->getType()->isFloatingPointTy())
                asmConstraints += "x";
            else
                asmConstraints += "r";
        }

        // Emite como InlineAsm void com os valores substituídos
        std::vector<Type*> argTypes;
        for (auto* v : argVals) argTypes.push_back(v->getType());
        FunctionType* irFt = FunctionType::get(Type::getVoidTy(ctx), argTypes, false);
        InlineAsm* ia = InlineAsm::get(
            irFt, irText, asmConstraints,
            /*hasSideEffects=*/true, /*isAlignStack=*/false,
            InlineAsm::AD_ATT);  // AT&T — para IR real use AD_Intel
        builder.CreateCall(ia, argVals);
        return;
    }

    codegenExpr(node);
}

static void codegenFunction(const FunctionNode* fn) {
    bool returnsStruct = (fn->returnType == DataType::Custom && !fn->returnStructType.empty());

    std::vector<Type*> paramTypes;

    // Se retorna struct: hidden sret pointer como primeiro param
    if (returnsStruct) {
        paramTypes.push_back(PointerType::getUnqual(ctx)); // sret
    }

    // Parâmetros normais — struct e array params são passados por ponteiro
    for (auto& p : fn->params) {
        if (p.type == DataType::Custom && !p.structTypeName.empty()) {
            paramTypes.push_back(PointerType::getUnqual(ctx)); // struct por ponteiro
        } else if (p.name.substr(0, 7) == "__arr__") {
            paramTypes.push_back(PointerType::getUnqual(ctx)); // array por ponteiro
        } else {
            paramTypes.push_back(llvmType(p.type));
        }
    }

    // Funções que retornam struct têm return type void (retorno via sret)
    Type* retType = returnsStruct ? Type::getVoidTy(ctx) : llvmType(fn->returnType);
    FunctionType* ft = FunctionType::get(retType, paramTypes, fn->isVariadic);

    // Resolve o nome real no LLVM (pode ser mangled se há overloads)
    std::string llvmName = fn->name;
    auto oit = overloadTable.find(fn->name);
    if (oit != overloadTable.end()) {
        for (auto& e : oit->second) {
            if (e.paramTypes == paramTypes) { llvmName = e.mangledName; break; }
        }
    }

    Function* func = llvmModule->getFunction(llvmName);
    if (!func)
        func = Function::Create(ft, Function::ExternalLinkage, llvmName, llvmModule.get());

    // ── Atributos de parâmetros ────────────────────────────────────────────
    // Parâmetros de string (i8*) recebem noalias + readonly para melhor alias analysis
    {
        size_t ai = 0;
        for (auto& arg : func->args()) {
            if (arg.getType()->isPointerTy()) {
                // sret pointer: writeonly (só escrevemos nele)
                if (returnsStruct && ai == 0) {
                    func->addParamAttr(ai, Attribute::WriteOnly);
                    func->addParamAttr(ai, Attribute::NoAlias);
                } else {
                    // Strings e struct params lidos: readonly + noalias
                    func->addParamAttr(ai, Attribute::ReadOnly);
                    func->addParamAttr(ai, Attribute::NoAlias);
                    #if LLVM_VERSION_MAJOR < 21
                        func->addParamAttr(ai, Attribute::NoCapture);
                    #endif
                }
            }
            ai++;
        }
    }
    // willreturn: a função sempre retorna (sem loops infinitos) → permite hoisting
    func->addFnAttr(Attribute::WillReturn);
    // nosync: não faz sync/atomic → permite reordenação livre
    func->addFnAttr(Attribute::NoSync);
    // mustprogress: garante que a função eventualmente termina → habilita mais otimizações de loop
    func->addFnAttr(Attribute::MustProgress);
    // uwtable não é necessário sem exceções

    // Nomeia os argumentos
    {
        size_t ai = 0;
        for (auto& arg : func->args()) {
            if (returnsStruct && ai == 0) {
                arg.setName("sret");
            } else {
                size_t pi = returnsStruct ? ai - 1 : ai;
                if (pi < fn->params.size())
                    arg.setName(fn->params[pi].name);
            }
            ai++;
        }
    }

    BasicBlock* bb = BasicBlock::Create(ctx, "entry", func);
    builder.SetInsertPoint(bb);

    // Limpa só locais — globais permanecem
    localValues.clear();
    localArrays.clear();
    localStructs.clear();
    localVarUsage.clear();

    // Registra parâmetros
    {
        size_t ai = 0;
        for (auto& arg : func->args()) {
            if (returnsStruct && ai == 0) { ai++; continue; } // pula sret
            size_t pi = returnsStruct ? ai - 1 : ai;
            if (pi >= fn->params.size()) { ai++; continue; }
            const auto& p = fn->params[pi];
            if (p.type == DataType::Custom && !p.structTypeName.empty()) {
                if (p.structTypeName[0] == '&') {
                    // Referência (&T ou &mut T): não copia, usa o ponteiro diretamente.
                    std::string realType = p.structTypeName;
                    size_t space = realType.find(' ');
                    if (space != std::string::npos) realType = realType.substr(space + 1);
                    else realType = realType.substr(1);
                    
                    // Se for referência a struct, registra em localStructs
                    if (structTypes.count(realType)) {
                        localStructs[p.name] = std::make_pair(&arg, realType);
                    } else {
                        // Referência a primitivo: guarda o ponteiro em localValues (precisa de alloca para o ponteiro)
                        AllocaInst* alloca = createEntryAlloca(func, p.name, arg.getType());
                        builder.CreateStore(&arg, alloca);
                        localValues[p.name] = alloca;
                    }
                } else {
                    // Parâmetro de struct (por valor no Nova, mas LLVM passa ponteiro): copia para local
                    StructType* st = structTypes[p.structTypeName];
                    if (!st)
                        reportError(sourceFile, 0, 0,
                                    "struct type '" + p.structTypeName + "' was not declared", "");
                    AllocaInst* alloca = createEntryAlloca(func, p.name, st);
                    copyStruct(alloca, &arg, p.structTypeName);
                    localStructs[p.name] = std::make_pair(alloca, p.structTypeName);
                }
            } else if (p.name.substr(0, 7) == "__arr__") {
                // Parâmetro de array: "__arr__N__realName"
                // Decodifica tamanho e nome real, recria alloca de ArrayType e copia do ponteiro
                std::string encoded = p.name.substr(7); // remove "__arr__"
                auto sep = encoded.find("__");
                int arrSize = (sep != std::string::npos) ? std::stoi(encoded.substr(0, sep)) : 0;
                std::string realName = (sep != std::string::npos) ? encoded.substr(sep + 2) : encoded;
                Type* elemType = llvmType(p.type);
                ArrayType* arrType = ArrayType::get(elemType, arrSize);
                AllocaInst* alloca = createEntryAlloca(func, realName, arrType);
                // arg é ponteiro para o primeiro elemento — copia cada elemento
                for (int ei = 0; ei < arrSize; ei++) {
                    Value* srcGep = builder.CreateGEP(elemType, &arg,
                        ConstantInt::get(Type::getInt32Ty(ctx), ei), "src." + std::to_string(ei));
                    Value* dstGep = builder.CreateGEP(arrType, alloca,
                        {ConstantInt::get(Type::getInt32Ty(ctx), 0),
                         ConstantInt::get(Type::getInt32Ty(ctx), ei)}, "dst." + std::to_string(ei));
                    Value* val = builder.CreateLoad(elemType, srcGep);
                    builder.CreateStore(val, dstGep);
                }
                localArrays[realName] = {alloca, arrType};
            } else {
                AllocaInst* alloca = createEntryAlloca(func, std::string(arg.getName()), arg.getType());
                builder.CreateStore(&arg, alloca);
                localValues[std::string(arg.getName())] = alloca;
            }
            ai++;
        }
    }

    for (auto& stmt : fn->body)
        codegenStmt(stmt.get(), func);

    // ── Warnings de variável local não usada (só em arquivos com main) ──────
    if (!isLibraryFile) {
        for (auto& [name, info] : localVarUsage) {
            if (!info.used)
                emitWarning(sourceFile, info.line, info.col,
                    "variable '" + name + "' is declared but never used",
                    getSourceLine(info.line), (int)name.size());
        }
    }

    if (!builder.GetInsertBlock()->getTerminator()) {
        if (returnsStruct || fn->returnType == DataType::Void)
            builder.CreateRetVoid();
        else
            builder.CreateRet(ConstantInt::get(Type::getInt32Ty(ctx), 0));
    }

    verifyFunction(*func);
}

static void codegenGlobalVar(const VarDeclNode* node) {
    Type* type = llvmType(node->type);
    Constant* init = nullptr;

    if (node->init) {
        bool declaredFloat = (node->type == DataType::Float);
        bool declaredInt   = (node->type == DataType::Int);
        bool initIsInt     = dynamic_cast<const IntLitNode*>(node->init.get()) != nullptr;
        bool initIsFloat   = dynamic_cast<const FloatLitNode*>(node->init.get()) != nullptr;

        if (declaredFloat && initIsInt)
            reportError(sourceFile, node->line, node->col,
                "type mismatch: variable '" + node->name + "' is declared as 'float' "
                "but assigned an 'int' literal — use '" + node->name + " = " +
                std::to_string(dynamic_cast<const IntLitNode*>(node->init.get())->value) + ".0' instead",
                getSourceLine(node->line), (int)node->name.size());

        if (declaredInt && initIsFloat)
            reportError(sourceFile, node->line, node->col,
                "type mismatch: variable '" + node->name + "' is declared as 'int' "
                "but assigned a 'float' literal — use 'float " + node->name + "' instead",
                getSourceLine(node->line), (int)node->name.size());

        if (auto* n = dynamic_cast<const IntLitNode*>(node->init.get()))
            init = ConstantInt::get(type, n->value);
        else if (auto* n = dynamic_cast<const FloatLitNode*>(node->init.get())) {
            APFloat apVal(APFloat::IEEEdouble(), n->raw);
            if (type->isFloatTy()) {
                bool lossy = false;
                apVal.convert(APFloat::IEEEsingle(), APFloat::rmNearestTiesToEven, &lossy);
            }
            init = ConstantFP::get(type, apVal);
        }
        else if (auto* n = dynamic_cast<const StringLitNode*>(node->init.get())) {
            Constant* strConst = ConstantDataArray::getString(ctx, n->value, true);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
            auto* strGV = new GlobalVariable(*llvmModule, strConst->getType(), true,
                                              GlobalValue::PrivateLinkage, strConst,
                                              node->name + ".str");
#pragma GCC diagnostic pop
            strGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
            init = ConstantExpr::getPointerCast(strGV, PointerType::getUnqual(ctx));
        }
        else
            reportError(sourceFile, node->line, node->col,
                       "global variable '" + node->name + "' can only be initialized with a literal — expressions are not allowed in global scope",
                        getSourceLine(node->line), (int)node->name.size());
    }

    // Resolução de nomes para globais:
    // - Para "ns::X": mantemos compatibilidade com a implementação stdlib existente,
    //   mapeando para o símbolo LLVM "X" (strip do namespace).
    // - Para "X" (sem ::) definido fora da stdlib: evitamos colisão com o símbolo
    //   "X" que a stdlib define (ex: math.npp define "PI"). Então geramos um
    //   símbolo LLVM mangled apenas quando é definição (node->init != nullptr).
    std::string llvmSymbol = node->name;
    {
        auto sep = node->name.rfind("::");
        if (sep != std::string::npos) {
            llvmSymbol = node->name.substr(sep + 2);
        } else {
            const char* ev = std::getenv("NOVA_STDLIB_PATH");
            bool isStdlib = sourceFile.find("/usr/local/lib/nova") != std::string::npos ||
                            (ev && sourceFile.find(ev) != std::string::npos);
            bool isDefinition = (node->init != nullptr);
            if (!isStdlib && isDefinition) {
                llvmSymbol = "user__" + node->name;
            }
        }
    }

    // Se o símbolo já existe no módulo (criado como extern pelo .nh antes),
    // e agora temos um init real (definição do .npp), apenas atualiza o initializer.
    GlobalVariable* gv = llvmModule->getGlobalVariable(llvmSymbol);
    if (gv) {
        if (init && !gv->hasInitializer()) {
            // Promove de extern para definição com valor
            gv->setInitializer(init);
        }
        // Já registrado — só garante que os dois nomes apontam para ele
        globalValues[node->name] = gv;
        globalValues[llvmSymbol] = gv;
        return;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
    gv = new GlobalVariable(*llvmModule, type, false,
                             GlobalValue::ExternalLinkage, init, llvmSymbol);
#pragma GCC diagnostic pop

    globalValues[node->name] = gv;
    globalValues[llvmSymbol] = gv;
}

static void codegenGlobalArray(const ArrayDeclNode* node) {
    // Array global de struct
    if (node->type == DataType::Custom && !node->structTypeName.empty()) {
        auto sit = structTypes.find(node->structTypeName);
        if (sit == structTypes.end()) {
            std::cerr << "error: struct type '" << node->structTypeName << "' not declared\n";
            exit(1);
        }
        StructType* st = sit->second;
        ArrayType* arrType = ArrayType::get(st, node->size);
        Constant* init = ConstantAggregateZero::get(arrType);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
        auto* gv = new GlobalVariable(*llvmModule, arrType, false,
                                       GlobalValue::ExternalLinkage, init, node->name);
#pragma GCC diagnostic pop
        globalArrays[node->name] = {gv, arrType};
        return;
    }
    Type* elemType = llvmType(node->type);
    ArrayType* arrType = ArrayType::get(elemType, node->size);

    Constant* init = nullptr;
    if (!node->init.empty()) {
        std::vector<Constant*> vals;
        for (auto& e : node->init) {
            if (auto* n = dynamic_cast<const IntLitNode*>(e.get())) {
                // int literal → compatível com int, long, char
                if (elemType->isIntegerTy())
                    vals.push_back(ConstantInt::get(elemType, n->value, /*isSigned=*/true));
                else if (elemType->isFloatingPointTy())
                    vals.push_back(ConstantFP::get(elemType, (double)n->value));
                else
                    vals.push_back(ConstantInt::get(elemType, n->value, true));
            } else if (auto* n = dynamic_cast<const LongLitNode*>(e.get())) {
                if (elemType->isIntegerTy())
                    vals.push_back(ConstantInt::get(elemType, n->value, true));
                else if (elemType->isFloatingPointTy())
                    vals.push_back(ConstantFP::get(elemType, (double)n->value));
                else
                    vals.push_back(ConstantInt::get(elemType, n->value, true));
            } else if (auto* n = dynamic_cast<const FloatLitNode*>(e.get())) {
                APFloat apVal(APFloat::IEEEdouble(), n->raw);
                if (elemType->isFloatTy()) {
                    bool lossy = false;
                    apVal.convert(APFloat::IEEEsingle(), APFloat::rmNearestTiesToEven, &lossy);
                }
                vals.push_back(ConstantFP::get(elemType, apVal));
            } else if (auto* n = dynamic_cast<const CharLitNode*>(e.get())) {
                // char literal → i8
                vals.push_back(ConstantInt::get(elemType, (unsigned char)n->value, false));
            } else if (auto* n = dynamic_cast<const StringLitNode*>(e.get())) {
                // string literal → cria global i8* e usa como elemento
                Constant* strConst = ConstantDataArray::getString(ctx, n->value, true);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
                auto* strGV = new GlobalVariable(*llvmModule, strConst->getType(), true,
                                                  GlobalValue::PrivateLinkage, strConst,
                                                  node->name + ".str");
#pragma GCC diagnostic pop
                strGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
                vals.push_back(ConstantExpr::getPointerCast(strGV, PointerType::getUnqual(ctx)));
            } else {
                // Fallback: zero
                vals.push_back(Constant::getNullValue(elemType));
            }
        }
        // Preenche elementos restantes com zero
        while ((int)vals.size() < node->size)
            vals.push_back(Constant::getNullValue(elemType));
        init = ConstantArray::get(arrType, vals);
    } else {
        init = ConstantAggregateZero::get(arrType);
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
    auto* gv = new GlobalVariable(*llvmModule, arrType, false,
                                   GlobalValue::ExternalLinkage, init, node->name);
#pragma GCC diagnostic pop
    globalArrays[node->name] = {gv, arrType};
}

void codegenProgram(const ProgramNode& program, const std::string& outputFile,
                    const std::string& filename, int optLevel, bool targetWindows) {
    sourceFile = filename;
    llvmModule = std::make_unique<Module>("nova_module", ctx);
    overloadTable.clear();
    nhDeclaredBuiltins.clear();
    structReturnFuncs.clear();
    structParamFuncs.clear();
    setupFastMath();

    // Detecta se este arquivo tem main — se não, é uma lib e suprime warnings de "não usado"
    isLibraryFile = true;
    for (auto& decl : program.declarations)
        if (auto* fn = dynamic_cast<const FunctionNode*>(decl.get()))
            if (fn->name == "main") { isLibraryFile = false; break; }

    // Primeira passagem: registra structs, enums e declara funções
    for (auto& decl : program.declarations) {
        if (auto* ed = dynamic_cast<const EnumDefNode*>(decl.get())) {
            // Enums are represented as i32 in LLVM
            // We just need to store the enum definition for later use
            enumValues[ed->name] = {};
            int discriminante = 0;
            for (auto& [name, exprNode] : ed->values) {
                int value = discriminante; // default value
                if (exprNode) {
                    // Evaluate compile-time constant expression for enum discriminant
                    // For now we only support integer literals
                    if (auto* lit = dynamic_cast<const IntLitNode*>(exprNode.get())) {
                        value = lit->value;
                    } else {
                        // Fallback to discriminante counter for non-literal expressions
                        // TODO: implement proper compile-time expression evaluation
                        value = discriminante;
                    }
                }
                enumValues[ed->name].push_back({name, value});
                discriminante = value + 1; // next enum value
            }
            // Also register without namespace
            {
                auto sep = ed->name.rfind("::");
                if (sep != std::string::npos) {
                    std::string shortName = ed->name.substr(sep + 2);
                    if (!enumValues.count(shortName)) {
                        enumValues[shortName] = enumValues[ed->name];
                    }
                }
            }
            continue;
        }
        if (auto* sd = dynamic_cast<const StructDefNode*>(decl.get())) {
            std::vector<Type*> fieldTypes;
            for (auto& f : sd->fields) {
                if (f.type == DataType::Custom && !f.structTypeName.empty()) {
                    // Campo nested struct — usa StructType* real (inline, não ponteiro genérico)
                    auto sit = structTypes.find(f.structTypeName);
                    if (sit != structTypes.end())
                        fieldTypes.push_back(sit->second);
                    else
                        fieldTypes.push_back(PointerType::getUnqual(ctx)); // fallback
                } else {
                    fieldTypes.push_back(llvmType(f.type));
                }
            }
            StructType* st = StructType::create(ctx, fieldTypes, sd->name);
            structTypes[sd->name] = st;
            structFields[sd->name] = sd->fields;
            // Registra também pelo nome sem namespace (ex: "geo::Point" → também "Point")
            // e pelo nome com namespace se já houver outro com namespace registrado.
            // Isso permite que geo::Point no .nh e Point no .npp refiram ao mesmo tipo.
            {
                auto sep = sd->name.rfind("::");
                if (sep != std::string::npos) {
                    std::string shortName = sd->name.substr(sep + 2);
                    if (!structTypes.count(shortName)) {
                        structTypes[shortName]  = st;
                        structFields[shortName] = sd->fields;
                    }
                } else {
                    // Nome curto — registra também sob qualquer alias com namespace já existente
                    // Procura se já existe "ns::Name" para esse mesmo nome curto e unifica
                    for (auto& [existing, existingSt] : structTypes) {
                        auto esep = existing.rfind("::");
                        if (esep != std::string::npos && existing.substr(esep + 2) == sd->name) {
                            // já existe geo::Point — aponta para o mesmo StructType
                            structTypes[existing]  = st;
                            structFields[existing] = sd->fields;
                        }
                    }
                }
            }
            // Pré-declara métodos como NomeTipo__metodo(NomeTipo* self, ...)
            for (auto& m : sd->methods) {
                std::vector<Type*> paramTypes;
                paramTypes.push_back(PointerType::getUnqual(ctx)); // self
                for (auto& p : m.params)
                    paramTypes.push_back(llvmType(p.type));
                FunctionType* ft = FunctionType::get(llvmType(m.returnType), paramTypes, false);
                Function::Create(ft, Function::ExternalLinkage,
                                 sd->name + "__" + m.name, llvmModule.get());
            }
        }
        if (auto* fn = dynamic_cast<const FunctionNode*>(decl.get())) {
            bool returnsStruct = (fn->returnType == DataType::Custom && !fn->returnStructType.empty());

            std::vector<Type*> paramTypes;
            if (returnsStruct)
                paramTypes.push_back(PointerType::getUnqual(ctx)); // sret

            for (auto& p : fn->params) {
                if (p.type == DataType::Custom && !p.structTypeName.empty())
                    paramTypes.push_back(PointerType::getUnqual(ctx));
                else if (p.name.substr(0, 7) == "__arr__")
                    paramTypes.push_back(PointerType::getUnqual(ctx)); // array passa por ponteiro
                else
                    paramTypes.push_back(llvmType(p.type));
            }

            // Registra info de retorno/params de struct para uso em CallNode
            if (returnsStruct)
                structReturnFuncs[fn->name] = fn->returnStructType;

            // Registra posições de params de struct (índice no params[] do usuário)
            {
                std::vector<std::pair<int,std::string>> spi;
                for (int pi = 0; pi < (int)fn->params.size(); pi++)
                    if (fn->params[pi].type == DataType::Custom && !fn->params[pi].structTypeName.empty())
                        spi.push_back({pi, fn->params[pi].structTypeName});
                if (!spi.empty()) structParamFuncs[fn->name] = spi;
            }

            bool willBeOverloaded = false;
            int nameCount = 0;
            for (auto& d2 : program.declarations)
                if (auto* f2 = dynamic_cast<const FunctionNode*>(d2.get()))
                    if (f2->name == fn->name) nameCount++;
            willBeOverloaded = (nameCount > 1);

            std::string llvmName;
            if (willBeOverloaded) {
                llvmName = fn->name + "__" + mangleSuffix(paramTypes);
            } else {
                llvmName = fn->name;
            }

            // Funções que retornam struct têm return type void (sret)
            Type* retTy = returnsStruct ? Type::getVoidTy(ctx) : llvmType(fn->returnType);
            FunctionType* ft = FunctionType::get(retTy, paramTypes, fn->isVariadic);
            if (!llvmModule->getFunction(llvmName))
                Function::Create(ft, Function::ExternalLinkage, llvmName, llvmModule.get());

            overloadTable[fn->name].push_back({paramTypes, llvmName});

            if (fn->name != "main" && !isLibraryFile)
                declaredFunctions[fn->name] = filename;
        }
        // ── FuncDeclNode: assinatura externa vinda de .nh ─────────────────
        if (auto* fd = dynamic_cast<const FuncDeclNode*>(decl.get())) {
            // Se já existe um FunctionNode com o mesmo nome nesta unidade de compilação,
            // o FuncDeclNode é redundante — a definição local tem prioridade.
            {
                std::string fdBase = fd->name;
                auto sep = fdBase.rfind("::"); if (sep != std::string::npos) fdBase = fdBase.substr(sep+2);
                bool hasLocalDef = false;
                for (auto& d2 : program.declarations)
                    if (auto* f2 = dynamic_cast<const FunctionNode*>(d2.get()))
                        if (f2->name == fdBase) { hasLocalDef = true; break; }
                if (hasLocalDef) continue;
            }
            // Builtins interceptados pelo codegen não precisam de declaração externa.
            // Declarar "std::printf" como ExternalLinkage faria o linker tentar resolvê-lo.
            {
                const std::string& fn = fd->name;
                // Considera builtin qualquer declaração de "printf"/"scanf" com ou sem namespace
                bool isBuiltin = (fn == "printf") ||
                    (fn.size() >= 8 && fn.substr(fn.size() - 8) == "::printf") ||
                    fn == "std::printf" ||
                    (fn == "scanf") ||
                    (fn.size() >= 7 && fn.substr(fn.size() - 7) == "::scanf") ||
                    fn == "std::scanf";
                if (isBuiltin) {
                    // Apenas registra que este builtin foi declarado via .nh.
                    // A interceptação em CallNode usa esse set para validar a chamada.
                    // Não cria símbolo LLVM — o codegen gera nova_printf/nova_scanf diretamente.
                    nhDeclaredBuiltins.insert(fn);
                    continue;
                }
            }
            std::vector<Type*> paramTypes;
            for (auto& p : fd->params) {
                if (p.type == DataType::Void &&
                    p.name.substr(0, 10) == "__struct__") {
                    // Parâmetro de tipo struct — decodifica o nome
                    // formato: "__struct__TypeName::paramName"
                    std::string encoded = p.name.substr(10); // remove "__struct__"
                    auto sep = encoded.find("::");
                    std::string typeName = (sep != std::string::npos)
                        ? encoded.substr(0, sep) : encoded;
                    auto sit = structTypes.find(typeName);
                    if (sit != structTypes.end())
                        paramTypes.push_back(PointerType::getUnqual(ctx)); // passa por ponteiro
                    else
                        paramTypes.push_back(PointerType::getUnqual(ctx)); // fallback
                } else {
                    paramTypes.push_back(llvmType(p.type));
                }
            }

            // ── Nome sem namespace para o símbolo LLVM ──────────────────────
            // O .npp define funções SEM namespace (ex: sqrt, add).
            // O .nh declara COM namespace (ex: std::sqrt).
            // O llvmName deve usar o nome sem namespace para o linker encontrar o símbolo.
            // A overloadTable registra o nome COM namespace para resolver std::sqrt(x).
            std::string baseName = fd->name;
            {
                auto sep = fd->name.rfind("::");
                if (sep != std::string::npos)
                    baseName = fd->name.substr(sep + 2);
            }

            bool fdReturnsStruct = (fd->returnType == DataType::Custom && !fd->returnStructType.empty());

            // Se existe um FunctionNode com o mesmo baseName neste programa,
            // o FuncDeclNode deve usar exatamente o mesmo llvmName que o FunctionNode usará.
            // O FunctionNode conta só outros FunctionNodes → nameCount=1 → llvmName=baseName.
            // Portanto, quando há FunctionNode, sempre usamos baseName sem mangling.
            bool hasLocalImpl = false;
            for (auto& d2 : program.declarations)
                if (auto* f2 = dynamic_cast<const FunctionNode*>(d2.get()))
                    if (f2->name == baseName) { hasLocalImpl = true; break; }

            std::string llvmName;
            if (hasLocalImpl) {
                // Usa o mesmo nome que o FunctionNode vai gerar (sem mangling)
                llvmName = baseName;
            } else {
                // Só conta FuncDeclNodes para decidir mangling (sem FunctionNode local)
                int nameCount = 0;
                for (auto& d2 : program.declarations)
                    if (auto* f2 = dynamic_cast<const FuncDeclNode*>(d2.get())) {
                        std::string b2 = f2->name;
                        auto s2 = b2.rfind("::"); if (s2 != std::string::npos) b2 = b2.substr(s2+2);
                        if (b2 == baseName) nameCount++;
                    }
                llvmName = (nameCount > 1)
                    ? baseName + "__" + mangleSuffix(paramTypes)
                    : baseName;
            }

            if (!hasLocalImpl && !llvmModule->getFunction(llvmName)) {
                // Declaração externa: cria forward decl com assinatura correta.
                // Funções que retornam struct: sret pointer como primeiro param, return void.
                std::vector<Type*> declParamTypes = paramTypes;
                if (fdReturnsStruct)
                    declParamTypes.insert(declParamTypes.begin(), PointerType::getUnqual(ctx));
                Type* retTy = fdReturnsStruct ? Type::getVoidTy(ctx) : llvmType(fd->returnType);
                FunctionType* ft = FunctionType::get(retTy, declParamTypes, fd->isVariadic);
                Function::Create(ft, Function::ExternalLinkage, llvmName, llvmModule.get());
            }
            // Registra no overloadTable apenas se não há implementação local.
            // Se há FunctionNode local, ele já vai registrar com o llvmName correto.
            // Registrar aqui também causaria duplicatas com o entry errado em [0].
            if (!hasLocalImpl) {
                // Para forward decls externas, paramTypes no overloadTable não inclui sret
                // (sret é adicionado por callStructReturningFunc na hora da chamada)
                overloadTable[fd->name].push_back({paramTypes, llvmName});
                if (baseName != fd->name)
                    overloadTable[baseName].push_back({paramTypes, llvmName});
            } else {
                // Há implementação local: registra só o nome qualificado → baseName
                // para que chamadas via "geo::makePoint" resolvam para "makePoint"
                overloadTable[fd->name].push_back({paramTypes, llvmName});
            }

            // Retorno de struct
            if (fd->returnType == DataType::Custom && !fd->returnStructType.empty()) {
                structReturnFuncs[fd->name]  = fd->returnStructType;
                structReturnFuncs[baseName]  = fd->returnStructType;
            }

            // Registra como função "disponível" para o check Nova não reportar como faltando.
            // Funções de .nh são externas — o linker resolve, não o check Nova.
            calledFunctions.insert(baseName);
            calledFunctions.insert(fd->name);

            // Params de struct
            {
                std::vector<std::pair<int,std::string>> spi;
                int userIdx = 0;
                for (auto& p : fd->params) {
                    if (p.type == DataType::Void && p.name.size() >= 10 &&
                        p.name.substr(0,10) == "__struct__") {
                        std::string encoded = p.name.substr(10);
                        auto sep = encoded.find("::");
                        std::string typeName = (sep != std::string::npos)
                            ? encoded.substr(0, sep) : encoded;
                        spi.push_back({userIdx, typeName});
                    }
                    userIdx++;
                }
                if (!spi.empty()) {
                    structParamFuncs[fd->name] = spi;
                    structParamFuncs[baseName] = spi;
                }
            }
        }
    }

    // Segunda passagem: gera tudo
    for (auto& decl : program.declarations) {
        if (auto* fn = dynamic_cast<const FunctionNode*>(decl.get()))
            codegenFunction(fn);
        else if (auto* vd = dynamic_cast<const VarDeclNode*>(decl.get()))
            codegenGlobalVar(vd);
        else if (auto* ad = dynamic_cast<const ArrayDeclNode*>(decl.get()))
            codegenGlobalArray(ad);
        else if (auto* sd = dynamic_cast<const StructVarDeclNode*>(decl.get())) {
            // Variável global de struct
            auto it = structTypes.find(sd->typeName);
            if (it == structTypes.end()) {
                std::cerr << "error: struct type '" << sd->typeName << "' not declared\n";
                exit(1);
            }
            StructType* st = it->second;
            Constant* init = ConstantAggregateZero::get(st);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
            auto* gv = new GlobalVariable(*llvmModule, st, false,
                                          GlobalValue::ExternalLinkage, init, sd->varName);
#pragma GCC diagnostic pop
            globalStructs[sd->varName] = {gv, sd->typeName};
        }
        // StructDefNode: gera os corpos dos métodos
        else if (auto* sd = dynamic_cast<const StructDefNode*>(decl.get())) {
            StructType* st = structTypes[sd->name];
            for (auto& m : sd->methods) {
                std::string funcName = sd->name + "__" + m.name;
                Function* func = llvmModule->getFunction(funcName);

                // Nomeia os parâmetros: self + params do método
                size_t pi = 0;
                for (auto& arg : func->args()) {
                    if (pi == 0) arg.setName("self");
                    else         arg.setName(m.params[pi - 1].name);
                    pi++;
                }

                BasicBlock* bb = BasicBlock::Create(ctx, "entry", func);
                builder.SetInsertPoint(bb);

                localValues.clear();
                localArrays.clear();
                localStructs.clear();

                // self é ponteiro para o struct — expõe campos como variáveis locais via GEP
                Value* selfPtr = func->arg_begin();

                // Registra "self" em localStructs para que self.field funcione dentro do método
                localStructs["self"] = {selfPtr, sd->name};
                auto& fields = structFields[sd->name];
                for (int fi = 0; fi < (int)fields.size(); fi++) {
                    Value* gep = builder.CreateStructGEP(st, selfPtr, fi, fields[fi].name);
                    // Guarda o GEP como AllocaInst* falso não funciona —
                    // usamos um mapa separado para self fields
                    // Hack limpo: cria alloca local e copia o valor de entrada
                    AllocaInst* alloca = createEntryAlloca(func, fields[fi].name, llvmType(fields[fi].type));
                    Value* loaded = builder.CreateLoad(llvmType(fields[fi].type), gep, fields[fi].name);
                    builder.CreateStore(loaded, alloca);
                    localValues[fields[fi].name] = alloca;
                }

                // Parâmetros normais
                pi = 1;
                for (auto& arg : func->args()) {
                    if (pi == 1) { pi++; continue; } // pula self
                    AllocaInst* alloca = createEntryAlloca(func, std::string(arg.getName()), arg.getType());
                    builder.CreateStore(&arg, alloca);
                    localValues[std::string(arg.getName())] = alloca;
                    pi++;
                }

                // Gera corpo
                for (auto& stmt : m.body)
                    codegenStmt(stmt.get(), func);

                // Escreve campos modificados de volta no struct (self)
                for (int fi = 0; fi < (int)fields.size(); fi++) {
                    auto it = localValues.find(fields[fi].name);
                    if (it != localValues.end()) {
                        Value* gep = builder.CreateStructGEP(st, selfPtr, fi, fields[fi].name + ".ptr");
                        Value* val = builder.CreateLoad(llvmType(fields[fi].type), it->second, fields[fi].name);
                        builder.CreateStore(val, gep);
                    }
                }

                if (!builder.GetInsertBlock()->getTerminator()) {
                    if (m.returnType == DataType::Void)
                        builder.CreateRetVoid();
                    else
                        builder.CreateRet(ConstantInt::get(Type::getInt32Ty(ctx), 0));
                }
                verifyFunction(*func);
            }
        }
        // StructDefNode já foi processado na primeira passagem
    }

    InitializeAllTargetInfos();
    InitializeAllTargets();
    InitializeAllTargetMCs();
    InitializeAllAsmPrinters();

    // ── Seleção de target ─────────────────────────────────────────────────
    std::string targetTriple;
    if (targetWindows) {
        targetTriple = "x86_64-w64-windows-gnu";
    } else {
        targetTriple = sys::getDefaultTargetTriple();
        if (targetTriple.find("i386") != std::string::npos ||
            targetTriple.find("i686") != std::string::npos) {
            targetTriple = "x86_64-" + targetTriple.substr(targetTriple.find('-') + 1);
        }
    }
    #if LLVM_VERSION_MAJOR >= 21
    llvmModule->setTargetTriple(llvm::Triple(targetTriple));
#else
    llvmModule->setTargetTriple(targetTriple);
#endif

    std::string error;
    auto target = TargetRegistry::lookupTarget(targetTriple, error);
    if (!target) {
        std::cerr << "Target error: " << error << "\n";
        exit(1);
    }

    // ── CPU e features do host ─────────────────────────────────────────────
    // Para cross-compile Windows usamos generic; para host usamos o CPU real
    // para habilitar AVX2, AVX-512, BMI2, etc. automaticamente.
    std::string cpuName  = "x86-64";
    std::string features = "+sse2";
    if (!targetWindows) {
        cpuName = std::string(sys::getHostCPUName());
        if (cpuName.empty() || cpuName == "generic") cpuName = "x86-64";
        // LLVM 19: getHostCPUFeatures() retorna StringMap diretamente (sem parâmetro)
        StringMap<bool> hostFeatures = sys::getHostCPUFeatures();
        if (!hostFeatures.empty()) {
            features.clear();
            for (auto& kv : hostFeatures) {
                if (!features.empty()) features += ",";
                features += (kv.second ? "+" : "-");
                features += kv.first().str();
            }
        }
    }

    // ── Nível de otimização do code-generator ─────────────────────────────
    CodeGenOptLevel cgOptLevel = CodeGenOptLevel::None;
    if (optLevel == 1) cgOptLevel = CodeGenOptLevel::Less;
    if (optLevel == 2) cgOptLevel = CodeGenOptLevel::Default;
    if (optLevel >= 3) cgOptLevel = CodeGenOptLevel::Aggressive;

    TargetOptions opt;
    opt.AllowFPOpFusion  = FPOpFusion::Fast;
    opt.UnsafeFPMath     = (optLevel >= 2);

    Reloc::Model relocModel = Reloc::PIC_;
    auto* targetMachine = target->createTargetMachine(
        targetTriple, cpuName, features, opt, relocModel,
        std::nullopt, cgOptLevel);

    llvmModule->setDataLayout(targetMachine->createDataLayout());

    // ── Otimizações LLVM ──────────────────────────────────────────────────
    // Aplica em todos os níveis >= 1, escalando aggressividade.
    // O3 activa: inlining agressivo, vectorização, IPO, loop unrolling total,
    //            GVN, LICM, SLP vectorizer, inter-procedural constant prop.
    if (optLevel >= 1) {
        PipelineTuningOptions pto;
        pto.LoopUnrolling          = true;
        pto.LoopInterleaving       = true;
        pto.LoopVectorization      = (optLevel >= 2);
        pto.SLPVectorization       = (optLevel >= 2);
        pto.MergeFunctions         = (optLevel >= 3);
        pto.CallGraphProfile       = (optLevel >= 2);

        PassBuilder pb(targetMachine, pto);

        // Registra todas as análises padrão
        LoopAnalysisManager     lam;
        FunctionAnalysisManager fam;
        CGSCCAnalysisManager    cgam;
        ModuleAnalysisManager   mam;

        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);

        OptimizationLevel lvl;
        if      (optLevel == 1) lvl = OptimizationLevel::O1;
        else if (optLevel == 2) lvl = OptimizationLevel::O2;
        else                    lvl = OptimizationLevel::O3;

        // buildPerModuleDefaultPipeline inclui:
        //   - Inlining, SROA, mem2reg, GVN, LICM, loop-unroll,
        //     instcombine, dead-code elim, cfg simplification,
        //     vectorização (O2+), IPO (O3+), etc.
        ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(lvl);
        mpm.run(*llvmModule, mam);
    }

    // ── Emissão de código objeto ───────────────────────────────────────────
    std::error_code ec;
    raw_fd_ostream dest(outputFile, ec, sys::fs::OF_None);
    if (ec) {
        std::cerr << "The file could not be opened: " << ec.message() << "\n";
        exit(1);
    }

    legacy::PassManager pass;
    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr,
                                            CodeGenFileType::ObjectFile)) {
        std::cerr << "TargetMachine does not support object file output.\n";
        exit(1);
    }

    pass.run(*llvmModule);
    dest.flush();
}