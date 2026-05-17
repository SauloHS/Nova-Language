#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Nova Language Packager — Linux (.tar.gz) e Windows/MSYS2 (.zip)
# ─────────────────────────────────────────────────────────────────────────────

NAME="nova-lang-v2.2.0beta"
DIST_DIR="dist/$NAME"

# ── Detecta OS ────────────────────────────────────────────────────────────────
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "mingw"* || "$OSTYPE" == "cygwin" ]]; then
    IS_WINDOWS=true
    BIN_NAME="n++.exe"
else
    IS_WINDOWS=false
    BIN_NAME="n++"
fi

echo "📦 Packaging Nova ($($IS_WINDOWS && echo 'Windows' || echo 'Linux'))..."

# ── Limpa builds antigos ──────────────────────────────────────────────────────
rm -rf dist
mkdir -p "$DIST_DIR"

# ── Copia arquivos essenciais ─────────────────────────────────────────────────
if [[ ! -f "$BIN_NAME" ]]; then
    echo "❌ Binary '$BIN_NAME' not found. Run make or make.sh first."
    exit 1
fi

cp "$BIN_NAME" "$DIST_DIR/"
cp install.sh "$DIST_DIR/"
cp -r stdlib "$DIST_DIR/"

chmod +x "$DIST_DIR/$BIN_NAME"
chmod +x "$DIST_DIR/install.sh"

# ── Comprime ──────────────────────────────────────────────────────────────────
cd dist

if $IS_WINDOWS; then
    if command -v zip &>/dev/null; then
        zip -r "${NAME}-windows.zip" "$NAME"
        echo "✅ Done! Package created at: dist/${NAME}-windows.zip"
    else
        echo "⚠️  'zip' not found. Installing..."
        pacman -S --noconfirm zip && zip -r "${NAME}-windows.zip" "$NAME"
        echo "✅ Done! Package created at: dist/${NAME}-windows.zip"
    fi
else
    tar -czvf "${NAME}.tar.gz" "$NAME"
    echo "✅ Done! Package created at: dist/${NAME}.tar.gz"
fi