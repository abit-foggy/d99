#!/bin/sh
# d99 nightly installer
# Downloads the nightly build (or builds latest git main) and installs into /usr/local/bin.
set -eu

REPO="abit-foggy/d99"
D99_BINDIR="${D99_BINDIR:-/usr/local/bin}"
BUILD_FROM_SOURCE=0
NIGHTLY_URL="https://github.com/${REPO}/releases/download/nightly/d99-nightly-linux-amd64.tar.gz"
FALLBACK_URL="https://github.com/${REPO}/releases/download/nightly/d99-linux-amd64.tar.gz"

usage() {
    echo "Usage: $0 [--build] [--bin-dir=<dir>]"
    echo "  --build         build directly from git source instead of downloading binary"
    echo "  --bin-dir=<dir> target installation directory (default: /usr/local/bin)"
    exit 2
}

for arg in "$@"; do
    case "$arg" in
        --build) BUILD_FROM_SOURCE=1 ;;
        --bin-dir=*) D99_BINDIR="${arg#*=}" ;;
        -h|--help) usage ;;
        *) echo "unknown option: $arg" >&2; usage ;;
    esac
done

if [ "$(id -u)" -ne 0 ] && [ "$D99_BINDIR" = "/usr/local/bin" ]; then
    echo "d99 nightly install: root required to install to ${D99_BINDIR}" >&2
    exit 1
fi

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64) ;;
    *)
        echo "d99: prebuilt nightly binaries are x86_64 only; falling back to source build"
        BUILD_FROM_SOURCE=1
        ;;
esac

TMPDIR="$(mktemp -d /tmp/d99-nightly.XXXXXX)"
cleanup() {
    rm -rf "$TMPDIR"
}
trap cleanup EXIT INT TERM

download_file() {
    url="$1"
    out="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 2 "$url" -o "$out"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$out" "$url"
    else
        return 1
    fi
}

installed=0

if [ "$BUILD_FROM_SOURCE" -eq 0 ]; then
    echo "d99: attempting to download nightly release from GitHub..."
    if download_file "$NIGHTLY_URL" "${TMPDIR}/d99.tar.gz" || \
       download_file "$FALLBACK_URL" "${TMPDIR}/d99.tar.gz"; then
        mkdir -p "$D99_BINDIR"
        tar -xzf "${TMPDIR}/d99.tar.gz" -C "$D99_BINDIR"
        installed=1
    else
        echo "d99: nightly release asset not yet available; falling back to source build..."
        BUILD_FROM_SOURCE=1
    fi
fi

if [ "$BUILD_FROM_SOURCE" -eq 1 ]; then
    echo "d99: building from latest git source..."
    SRCDIR=""
    # If currently inside a d99 git tree with Makefile
    if [ -f "./Makefile" ] && [ -f "./src/solve/main.c" ]; then
        SRCDIR="$(pwd)"
        make -C "$SRCDIR" -j"$(nproc 2>/dev/null || echo 2)"
        make -C "$SRCDIR" install PREFIX="/usr/local"
        installed=1
    else
        if ! command -v git >/dev/null 2>&1 || ! command -v cc >/dev/null 2>&1; then
            echo "d99: git and a C compiler (gcc/clang) are required to build from source" >&2
            exit 1
        fi
        git clone --depth 1 "https://github.com/${REPO}.git" "${TMPDIR}/d99-src"
        make -C "${TMPDIR}/d99-src" -j"$(nproc 2>/dev/null || echo 2)"
        make -C "${TMPDIR}/d99-src" install PREFIX="/usr/local"
        installed=1
    fi
fi

if [ "$installed" -eq 1 ]; then
    for bin in d99-deb d99-query d99-inst d99-solve d99-build; do
        if [ -f "${D99_BINDIR}/${bin}" ]; then
            chmod 0755 "${D99_BINDIR}/${bin}"
        fi
    done
    echo "d99: successfully installed nightly build into ${D99_BINDIR}"
    if [ -x "${D99_BINDIR}/d99-solve" ]; then
        "${D99_BINDIR}/d99-solve" --version
    fi
else
    echo "d99: nightly installation failed" >&2
    exit 1
fi
