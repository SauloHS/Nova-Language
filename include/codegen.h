#pragma once
#include "ast.h"
#include <string>

void codegenProgram(const ProgramNode& program, const std::string& outputFile,
                    const std::string& filename, int optLevel = 0,
                    bool targetWindows = false);

// Emite warnings de funções definidas mas nunca chamadas em todo o projeto.
// projectHasMain: true se ao menos um arquivo compilado tinha uma função main.
// Se false (build de lib pura), nenhum warning é emitido.
// Deve ser chamado pelo main após compilar todos os arquivos.
void flushFunctionWarnings(bool projectHasMain);