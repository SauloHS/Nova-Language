#!/bin/bash

# Script de testes para o compilador Nova
# Uso: ./test.sh [test_name]

# Configurações
COMPILER="../n++"
TEST_DIR="."
EXPECT_DIR="./expect"
TEMP_DIR="/tmp/nova_tests"
RESULTS_FILE="test_results.log"
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Cria diretórios temporários
mkdir -p "$TEMP_DIR"
mkdir -p "$TEST_DIR/expect"

# Limpa resultados anteriores
rm -f "$RESULTS_FILE"
touch "$RESULTS_FILE"

echo "========================================"
echo " Nova Compiler Test Suite"
echo "========================================"
echo ""

# Executa um teste específico
run_test() {
    local test_file="$1"
    local test_name="${test_file##*/}"
    test_name="${test_name%.*}"
    local expect_file="$EXPECT_DIR/${test_name}.expect"
    local output_file="$TEMP_DIR/${test_name}_out"
    local exec_file="$TEMP_DIR/${test_name}"
    local exit_code=0
    local diff_output=""

    echo -n "Testando $test_name... "

    # Compila
    "$COMPILER" "$test_file" -o "$exec_file" > /dev/null 2>&1
    exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo -e "${RED}FALHOU (compilação)${NC}" | tee -a "$RESULTS_FILE"
        echo "  Erro: compilação falhou" >> "$RESULTS_FILE"
        return 1
    fi

    # Executa
    "$exec_file" > "$output_file" 2>&1
    exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo -e "${RED}FALHOU (execução)${NC}" | tee -a "$RESULTS_FILE"
        echo "  Erro: programa retornou código $exit_code" >> "$RESULTS_FILE"
        return 1
    fi

    # Compara com arquivo de expectativa
    if [ -f "$expect_file" ]; then
        if ! diff -u <(cat "$expect_file"; echo) <(cat "$output_file"; echo) >> "$RESULTS_FILE" 2>&1; then
            echo -e "${RED}FALHOU (output)${NC}" | tee -a "$RESULTS_FILE"
            echo "  Diff: $expect_file vs $output_file" >> "$RESULTS_FILE"
            return 1
        fi
    fi

    # Sucesso
    echo -e "${GREEN}OK${NC}" | tee -a "$RESULTS_FILE"
    return 0
}

# Executa todos os testes
run_all_tests() {
    local total=0
    local passed=0
    local failed=0
    local test_files

    echo "Procurando testes em $TEST_DIR/*.npp"
    echo ""
    
    # Encontra todos os arquivos .npp
    test_files=($TEST_DIR/*.npp)
    total=${#test_files[@]}
    
    if [ $total -eq 0 ]; then
        echo "Nenhum teste encontrado em $TEST_DIR"
        exit 1
    fi
    
    # Executa cada teste
    for test_file in "${test_files[@]}"; do
        run_test "$test_file"
        if [ $? -eq 0 ]; then
            ((passed++))
        else
            ((failed++))
        fi
    done
    
    # Resumo
    echo ""
echo "========================================"
    echo " Resumo dos Testes"
    echo "========================================"
    echo " Total:  $total"
    echo " Passed: ${GREEN}$passed${NC}"
    echo " Failed: ${RED}$failed${NC}"
    echo "========================================"
    
    # Detalhes dos testes que falharam
    if [ $failed -gt 0 ]; then
        echo ""
        echo "Detalhes dos testes falhados:"
        echo "----------------------------------------"
        grep -A 10 "^Testando.*FALHOU" "$RESULTS_FILE" | sed 's/^Testando //;s/\.\.\..*//'
    fi
}

# Executa teste específico ou todos
if [ $# -eq 1 ]; then
    test_file="$TEST_DIR/$1.npp"
    if [ -f "$test_file" ]; then
        run_test "$test_file"
    else
        echo "Teste $1 não encontrado"
        exit 1
    fi
else
    run_all_tests
fi

# Limpeza
# rm -rf "$TEMP_DIR"

exit ${failed}
