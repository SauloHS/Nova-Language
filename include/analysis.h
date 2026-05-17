#pragma once
#include "ast.h"
#include <string>

// Roda todas as análises estáticas sobre o programa já parseado.
// Emite warnings e erros diretamente no stderr (mesmo formato do resto do compilador).
// Termina o processo via reportError() se encontrar um erro fatal.
// Deve ser chamado APÓS parseProgram() e ANTES de codegenProgram().
void runAnalysis(const ProgramNode& program, const std::string& sourceFile);