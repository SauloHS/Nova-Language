#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
# Nova Build System — Linux e Windows/MSYS2
# ─────────────────────────────────────────────────────────────────────────────

VERSION_FILE=".build_no"

# ── Detecta OS ────────────────────────────────────────────────────────────────
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "mingw"* || "$OSTYPE" == "cygwin" ]]; then
    IS_WINDOWS=true
    MAKE_LIBS='LIBS="-lncursesw"'
    BIN_NAME="n++.exe"
else
    IS_WINDOWS=false
    MAKE_LIBS=""
    BIN_NAME="n++"
fi

# ── Versão ────────────────────────────────────────────────────────────────────
if [ ! -f "$VERSION_FILE" ]; then
    echo 0 > "$VERSION_FILE"
fi
OLD_VERSION=$(cat "$VERSION_FILE")
NEW_VERSION=$((OLD_VERSION + 1))
echo $NEW_VERSION > "$VERSION_FILE"

echo "----------------------------------------"
echo "🛠️  Nova Build System | Build #$NEW_VERSION"
$IS_WINDOWS && echo "    Platform: Windows/MSYS2" || echo "    Platform: Linux"
echo "----------------------------------------"

# ── Compilar n++ ──────────────────────────────────────────────────────────────
echo "[1/5] Compiling n++..."
make -s $MAKE_LIBS 2>&1 | grep -i "error:"
MAKE_RET=${PIPESTATUS[0]}
if [ $MAKE_RET -ne 0 ]; then
    echo "❌ n++ build failed!"
    exit 1
fi

# ── Compilar orbit ────────────────────────────────────────────────────────────
echo "[2/5] Compiling orbit..."
make -s -C orbit $MAKE_LIBS 2>&1 | grep -i "error:"
ORBIT_RET=${PIPESTATUS[0]}
if [ $ORBIT_RET -ne 0 ]; then
    echo "⚠️  orbit build failed (skipping)"
fi

# ── Instalar ──────────────────────────────────────────────────────────────────
echo "[3/5] Installing binaries..."
./install.sh > /dev/null 2>&1

# ── Atualizar ambiente ────────────────────────────────────────────────────────
echo "[4/5] Updating shell..."
source ~/.bashrc 2>/dev/null || true

# ── Validar ───────────────────────────────────────────────────────────────────
echo -n "[5/5] n++ version:    "
$BIN_NAME --version 2>/dev/null || n++ --version 2>/dev/null || echo "(not in PATH yet — restart terminal)"

if ! $IS_WINDOWS; then
    echo -n "      orbit version:  "
    orbit version 2>/dev/null || echo "(not in PATH yet — restart terminal)"
fi

echo "----------------------------------------"
echo "✅ Build #$NEW_VERSION complete."