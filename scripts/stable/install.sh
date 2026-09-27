#!/bin/sh
# d99 stable installer
# Downloads and installs the latest stable release of d99 into /usr/local/bin.
set -eu

REPO="abit-foggy/d99"
D99_BINDIR="${D99_BINDIR:-/usr/local/bin}"
TARBALL_URL="https://github.com/${REPO}/releases/latest/download/d99-linux-amd64.tar.gz"

usage() {
    echo "Usage: $0 [--bin-dir=<dir>]"
    exit 2
}

for arg in "$@"; do
    case "$arg" in
        --bin-dir=*) D99_BINDIR="${arg#*=}" ;;
        -h|--help) usage ;;
        *) echo "unknown option: $arg" >&2; usage ;;
    esac
done

if [ "$(id -u)" -ne 0 ] && [ "$D99_BINDIR" = "/usr/local/bin" ]; then
    echo "d99 stable install: root required to install to ${D99_BINDIR}" >&2
    exit 1
fi

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64) ;;
    *)
        echo "d99: release binaries currently built for x86_64 only (detected: $ARCH)" >&2
        exit 1
        ;;
esac

TMPDIR="$(mktemp -d /tmp/d99-stable.XXXXXX)"
cleanup() {
    rm -rf "$TMPDIR"
}
trap cleanup EXIT INT TERM

echo "d99: downloading latest stable release from ${TARBALL_URL}..."
if command -v curl >/dev/null 2>&1; then
    curl -fsSL --retry 3 "$TARBALL_URL" -o "${TMPDIR}/d99.tar.gz"
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "${TMPDIR}/d99.tar.gz" "$TARBALL_URL"
else
    echo "d99: neither curl nor wget found; please install one to continue" >&2
    exit 1
fi

mkdir -p "$D99_BINDIR"
tar -xzf "${TMPDIR}/d99.tar.gz" -C "$D99_BINDIR"

for bin in d99-deb d99-query d99-inst d99-solve d99-build; do
    if [ -f "${D99_BINDIR}/${bin}" ]; then
        chmod 0755 "${D99_BINDIR}/${bin}"
    fi
done

echo "d99: successfully installed stable binaries into ${D99_BINDIR}"
if [ -x "${D99_BINDIR}/d99-solve" ]; then
    "${D99_BINDIR}/d99-solve" --version
fi
