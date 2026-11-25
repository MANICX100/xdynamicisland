#!/usr/bin/env sh
set -eu

REPO="MANICX100/xdynamicisland"
BIN_NAME="xdynamicisland"
INSTALL_DIR="${INSTALL_DIR:-/usr/local/bin}"
ASSET="xdynamicisland-linux-x86_64"
BASE_URL="https://github.com/${REPO}/releases/latest/download"

ARCH="$(uname -m)"
if [ "$ARCH" != "x86_64" ]; then
    echo "Unsupported architecture: ${ARCH} (prebuilt binary is x86_64; build from source with 'make install')." >&2
    exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "Downloading ${BIN_NAME} from ${BASE_URL}/${ASSET}..."
curl -fsSL "${BASE_URL}/${ASSET}" -o "${TMP_DIR}/${BIN_NAME}"

if curl -fsSL "${BASE_URL}/SHA256SUMS" -o "${TMP_DIR}/SHA256SUMS" 2>/dev/null; then
    EXPECTED="$(grep " ${ASSET}\$" "${TMP_DIR}/SHA256SUMS" | awk '{print $1}')"
    if [ -n "$EXPECTED" ] && command -v sha256sum >/dev/null 2>&1; then
        ACTUAL="$(sha256sum "${TMP_DIR}/${BIN_NAME}" | awk '{print $1}')"
        if [ "$EXPECTED" != "$ACTUAL" ]; then
            echo "Checksum verification failed!" >&2
            exit 1
        fi
        echo "SHA256 checksum verified."
    fi
fi

chmod +x "${TMP_DIR}/${BIN_NAME}"

if [ -w "$INSTALL_DIR" ]; then
    install -m 755 "${TMP_DIR}/${BIN_NAME}" "${INSTALL_DIR}/${BIN_NAME}"
elif command -v doas >/dev/null 2>&1; then
    doas install -m 755 "${TMP_DIR}/${BIN_NAME}" "${INSTALL_DIR}/${BIN_NAME}"
elif command -v sudo >/dev/null 2>&1; then
    sudo install -m 755 "${TMP_DIR}/${BIN_NAME}" "${INSTALL_DIR}/${BIN_NAME}"
else
    echo "Neither 'doas' nor 'sudo' found; re-run as root to install to ${INSTALL_DIR}." >&2
    exit 1
fi

echo "Installed ${BIN_NAME} to ${INSTALL_DIR}/${BIN_NAME}"
"${INSTALL_DIR}/${BIN_NAME}" --version
