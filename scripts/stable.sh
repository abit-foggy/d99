#!/bin/sh
# d99 stable runner
# One-shot script to install d99 stable release and activate system swap.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ACTION="all"
DRY_RUN=0
CONFIRMED=0
EXTRA_ARGS=""

usage() {
    code="${1:-2}"
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  (default)          install latest stable release and activate swap"
    echo "  --install-only     install binaries to /usr/local/bin without swapping"
    echo "  --swap-only        apply system tool swap only"
    echo "  --revert           revert system swap (restore upstream tools)"
    echo "  --purge            permanently purge diverted upstream tools"
    echo "  --dry-run          preview actions without making changes"
    echo "  -f, --force        force install even if nightly is present"
    echo "  -y, --yes          automatic yes to prompts (for purge)"
    echo "  -h, --help         show this help"
    exit "$code"
}

for arg in "$@"; do
    case "$arg" in
        --install-only) ACTION="install" ;;
        --swap-only)    ACTION="swap" ;;
        --revert)       ACTION="revert" ;;
        --purge)        ACTION="purge" ;;
        --dry-run)      DRY_RUN=1 ;;
        -f|--force)     EXTRA_ARGS="${EXTRA_ARGS} --force" ;;
        -y|--yes)       CONFIRMED=1 ;;
        -h|--help)      usage 0 ;;
        *)              EXTRA_ARGS="${EXTRA_ARGS} ${arg}" ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo "d99: root privileges required" >&2
    exit 1
fi

case "$ACTION" in
    revert)
        SWAP_CMD="${SCRIPT_DIR}/stable/swap.sh --revert"
        if [ "$DRY_RUN" -eq 1 ]; then SWAP_CMD="${SWAP_CMD} --dry-run"; fi
        $SWAP_CMD
        ;;
    purge)
        PURGE_CMD="${SCRIPT_DIR}/stable/purge.sh"
        if [ "$CONFIRMED" -eq 1 ]; then PURGE_CMD="${PURGE_CMD} -y"; fi
        if [ "$DRY_RUN" -eq 1 ]; then PURGE_CMD="${PURGE_CMD} --dry-run"; fi
        $PURGE_CMD
        ;;
    install)
        if [ "$DRY_RUN" -eq 1 ]; then
            echo "would download and install stable binaries into /usr/local/bin"
        else
            "${SCRIPT_DIR}/stable/install.sh" $EXTRA_ARGS
        fi
        ;;
    swap)
        SWAP_CMD="${SCRIPT_DIR}/stable/swap.sh"
        if [ "$DRY_RUN" -eq 1 ]; then SWAP_CMD="${SWAP_CMD} --dry-run"; fi
        $SWAP_CMD
        ;;
    all)
        if [ "$DRY_RUN" -eq 1 ]; then
            echo "would download and install stable binaries into /usr/local/bin"
            "${SCRIPT_DIR}/stable/swap.sh" --dry-run
        else
            "${SCRIPT_DIR}/stable/install.sh" $EXTRA_ARGS
            "${SCRIPT_DIR}/stable/swap.sh"
        fi
        ;;
esac
