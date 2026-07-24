#!/usr/bin/env bash
set -euo pipefail

# Compatibility entry point. Installation behavior belongs in the canonical
# install.sh at the repository root.
echo "note: scripts/setup.sh is deprecated; using canonical install.sh" >&2

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_INSTALLER="$SCRIPT_DIR/../install.sh"
if [[ -f "$ROOT_INSTALLER" ]]; then
    exec bash "$ROOT_INSTALLER" "$@"
fi

TMP_INSTALLER=$(mktemp "${TMPDIR:-/tmp}/cbm-install.XXXXXX")
cleanup() {
    rm -f "$TMP_INSTALLER"
}
trap cleanup EXIT

URL="https://raw.githubusercontent.com/0ctacity/codebase-memory-mcp/main/install.sh"
if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$URL" -o "$TMP_INSTALLER"
elif command -v wget >/dev/null 2>&1; then
    wget -qO "$TMP_INSTALLER" "$URL"
else
    echo "error: curl or wget is required" >&2
    exit 1
fi

bash "$TMP_INSTALLER" "$@"
