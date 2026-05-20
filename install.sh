#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Nova Language Installer
# Suporta Linux (LLVM 19) e Windows/MSYS2 (LLVM 21)
# ─────────────────────────────────────────────────────────────────────────────

set -o pipefail

BOLD="\033[1m"
CYAN="\033[1;36m"
GREEN="\033[1;32m"
RED="\033[1;31m"
YELLOW="\033[1;33m"
GRAY="\033[0;90m"
RESET="\033[0m"

info()    { echo -e "${CYAN}  →${RESET} $1"; }
success() { echo -e "${GREEN}  ✓${RESET} ${BOLD}$1${RESET}"; }
warn()    { echo -e "${YELLOW}  ⚠${RESET} $1"; }
fail()    { echo -e "${RED}  ✗ error:${RESET} ${BOLD}$1${RESET}"; exit 1; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Detecta OS ────────────────────────────────────────────────────────────────
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "mingw"* || "$OSTYPE" == "cygwin" ]]; then
    IS_WINDOWS=true
else
    IS_WINDOWS=false
fi

# ── Dependency Check ──────────────────────────────────────────────────────────
if $IS_WINDOWS; then
    info "Checking for LLVM (Windows/MSYS2)..."
    if ! command -v llvm-config &>/dev/null; then
        warn "LLVM not found. Installing via pacman..."
        pacman -S --noconfirm mingw-w64-x86_64-llvm || fail "Failed to install LLVM. Run: pacman -S mingw-w64-x86_64-llvm"
        success "LLVM installed."
    else
        LLVM_VER=$(llvm-config --version | cut -d. -f1)
        success "LLVM $LLVM_VER detected."
    fi
else
    info "Checking for LLVM 19..."
    if ! command -v llvm-config-19 &>/dev/null && ! llvm-config --version | grep -q "19" 2>/dev/null; then
        warn "LLVM 19 not found. This is required for Nova to compile code."
        echo -ne "${YELLOW}  ?${RESET} Would you like to install llvm-19-dev now? (sudo required) [y/N]: "
        read -r response
        if [[ "$response" =~ ^([yY][eE][sS]|[yY])$ ]]; then
            info "Updating packages and installing LLVM 19..."
            sudo apt update && sudo apt install -y llvm-19-dev clang-19 libncurses-dev
            success "LLVM 19 installed successfully."
        else
            fail "LLVM 19 is required. Please install it manually: sudo apt install llvm-19-dev"
        fi
    else
        success "LLVM 19 detected."
    fi
fi

# ── Arguments ─────────────────────────────────────────────────────────────────
if $IS_WINDOWS; then
    PREFIX="$HOME/.local"
else
    PREFIX="/usr/local"
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --help)
            echo "Usage: ./install.sh [--prefix /path]"
            echo "  --prefix   Installation base directory (default: /usr/local on Linux, ~/.local on Windows)"
            exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done

BIN_DIR="$PREFIX/bin"
LIB_DIR="$PREFIX/lib/nova"

# ── Detecta nome do binário ───────────────────────────────────────────────────
if $IS_WINDOWS; then
    BIN_NAME="n++.exe"
    BIN_SRC="n++.exe"
else
    BIN_NAME="n++"
    BIN_SRC="n++"
fi

echo ""
echo -e "${BOLD}Nova Language Installer${RESET}"
echo -e "${GRAY}  repository: $REPO${RESET}"
echo -e "${GRAY}  binary:     $BIN_DIR/$BIN_NAME${RESET}"
echo -e "${GRAY}  stdlib:     $LIB_DIR/${RESET}"
echo -e "${GRAY}  platform:   $($IS_WINDOWS && echo 'Windows/MSYS2' || echo 'Linux')${RESET}"
echo ""

# ── Check package structure ───────────────────────────────────────────────────
if [[ ! -f "$REPO/$BIN_SRC" ]]; then
    warn "Binary '$BIN_SRC' not found. Trying to build it..."
    if [[ -f "$REPO/Makefile" ]]; then
        if $IS_WINDOWS; then
            make -C "$REPO" LIBS="-lncursesw" || fail "Failed to build $BIN_NAME. Run 'make LIBS=\"-lncursesw\"' manually."
        else
            make -C "$REPO" || fail "Failed to build $BIN_NAME. Run 'make' manually."
        fi
    else
        fail "Neither binary '$BIN_SRC' nor 'Makefile' found."
    fi
fi
[[ -d "$REPO/stdlib" ]] || fail "'stdlib/' directory not found in package"

# ── Install binary (sem sudo no Windows) ─────────────────────────────────────
info "Installing binary..."
mkdir -p "$BIN_DIR"

if $IS_WINDOWS; then
    cp "$REPO/$BIN_SRC" "$BIN_DIR/$BIN_NAME"
    chmod +x "$BIN_DIR/$BIN_NAME"
else
    sudo mkdir -p "$BIN_DIR"
    sudo cp "$REPO/$BIN_SRC" "$BIN_DIR/$BIN_NAME"
    sudo chmod +x "$BIN_DIR/$BIN_NAME"
fi
success "Binary installed at $BIN_DIR/$BIN_NAME"

# ── Install stdlib ────────────────────────────────────────────────────────────
info "Installing standard library..."
if $IS_WINDOWS; then
    mkdir -p "$LIB_DIR"
    cp "$REPO/stdlib/"*.nh  "$LIB_DIR/" 2>/dev/null || warn "No .nh files found"
    cp "$REPO/stdlib/"*.npp "$LIB_DIR/" 2>/dev/null || warn "No .npp files found"
else
    sudo mkdir -p "$LIB_DIR"
    sudo cp "$REPO/stdlib/"*.nh  "$LIB_DIR/" 2>/dev/null || warn "No .nh files found"
    sudo cp "$REPO/stdlib/"*.npp "$LIB_DIR/" 2>/dev/null || warn "No .npp files found"
fi

NH_COUNT=$(ls "$LIB_DIR"/*.nh  2>/dev/null | wc -l)
NPP_COUNT=$(ls "$LIB_DIR"/*.npp 2>/dev/null | wc -l)
success "Standard library installed ($NH_COUNT headers, $NPP_COUNT modules)"

info "Installing editor config..."
if $IS_WINDOWS; then
    if [[ ! -f "$LIB_DIR/nova.cfg" ]]; then
        cp "$REPO/stdlib/nova.cfg" "$LIB_DIR/nova.cfg"
        success "Config installed at $LIB_DIR/nova.cfg"
    else
        warn "nova.cfg already exists — skipping (your settings are preserved)"
    fi
else
    if [[ ! -f "$LIB_DIR/nova.cfg" ]]; then
        sudo cp "$REPO/stdlib/nova.cfg" "$LIB_DIR/nova.cfg"
        success "Config installed at $LIB_DIR/nova.cfg"
    else
        warn "nova.cfg already exists — skipping (your settings are preserved)"
    fi
fi

# ── Install orbit ─────────────────────────────────────────────────────────────
if $IS_WINDOWS; then
    warn "orbit installation is not supported on Windows yet. Skipping."
else
    info "Installing orbit..."
    if [[ -d "$REPO/orbit" ]]; then
        if [[ ! -f "$REPO/orbit/orbit" ]]; then
            info "Building orbit..."
            make -C "$REPO/orbit" --no-print-directory 2>&1 | grep -i "error:" || true
            MAKE_RET=${PIPESTATUS[0]}
            if [[ $MAKE_RET -ne 0 ]]; then
                warn "orbit build failed. Skipping orbit installation."
            fi
        fi
        if [[ -f "$REPO/orbit/orbit" ]]; then
            sudo cp "$REPO/orbit/orbit" "$BIN_DIR/orbit"
            sudo chmod +x "$BIN_DIR/orbit"
            success "orbit installed at $BIN_DIR/orbit"
        fi
    else
        warn "orbit/ directory not found. Skipping orbit installation."
    fi
fi
# ── Configure shell RC ────────────────────────────────────────────────────────
info "Configuring environment variables..."

if [ -n "$SUDO_USER" ]; then
    USER_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
else
    USER_HOME="$HOME"
fi

if   [[ -f "$USER_HOME/.zshrc" ]];   then SHELL_RC="$USER_HOME/.zshrc"
elif [[ -f "$USER_HOME/.bashrc" ]];  then SHELL_RC="$USER_HOME/.bashrc"
elif [[ -f "$USER_HOME/.profile" ]]; then SHELL_RC="$USER_HOME/.profile"
else SHELL_RC=""; fi

add_to_rc() {
    local line="$1"
    local comment="$2"
    if [[ -n "$SHELL_RC" ]] && ! grep -qF "$line" "$SHELL_RC"; then
        echo "" >> "$SHELL_RC"
        echo "# $comment" >> "$SHELL_RC"
        echo "$line" >> "$SHELL_RC"
        [ -n "$SUDO_USER" ] && chown "$SUDO_USER" "$SHELL_RC"
    fi
}

if [[ -n "$SHELL_RC" ]]; then
    add_to_rc "export NOVA_STDLIB_PATH=\"$LIB_DIR\""  "Nova standard library path"
    add_to_rc "export PATH=\"\$PATH:$BIN_DIR\""       "Nova compiler + orbit"
    if $IS_WINDOWS; then
        add_to_rc "alias n++='n++.exe'"  "Nova compiler alias (Windows)"
    fi
    success "Environment variables added to $SHELL_RC"
else
    warn "Could not detect shell RC file. Add these manually:"
    echo "    export NOVA_STDLIB_PATH=\"$LIB_DIR\""
    echo "    export PATH=\"\$PATH:$BIN_DIR\""
fi

# ── Final Message ─────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}${GREEN}Installation complete!${RESET}"
echo ""
if $IS_WINDOWS; then
    echo -e "  Nova compiler:  ${CYAN}n++.exe --version${RESET}"
    echo -e "  Reload shell:   ${CYAN}source ~/.bashrc${RESET}  (or restart MSYS2)"
else
    echo -e "  Nova compiler:  ${CYAN}n++ --version${RESET}"
    echo -e "  Orbit:          ${CYAN}orbit help${RESET}"
    echo -e "  Reload shell:   ${CYAN}source ~/.bashrc${RESET}  (or restart terminal)"
fi
echo ""