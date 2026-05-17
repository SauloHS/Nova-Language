#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Orbit Installer
# ─────────────────────────────────────────────────────────────────────────────

set -e

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
PREFIX="${1:-/usr/local}"
BIN_DIR="$PREFIX/bin"

echo ""
echo -e "${BOLD}Orbit Installer${RESET}"
echo -e "${GRAY}  binary: $BIN_DIR/orbit${RESET}"
echo ""

# Build if binary missing
if [[ ! -f "$REPO/orbit" ]]; then
    warn "Binary 'orbit' not found. Building..."
    make -C "$REPO" || fail "Build failed. Run 'make' in the orbit directory."
fi

info "Installing orbit binary..."
sudo mkdir -p "$BIN_DIR"
sudo cp "$REPO/orbit" "$BIN_DIR/orbit"
sudo chmod +x "$BIN_DIR/orbit"
success "orbit installed at $BIN_DIR/orbit"

echo ""
echo -e "${BOLD}${GREEN}Orbit installation complete!${RESET}"
echo -e "  Try: ${CYAN}orbit help${RESET}"
echo ""
