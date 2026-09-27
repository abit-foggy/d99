#!/bin/sh
# d99 nightly runner
# One-shot script to install d99 nightly build and activate system swap.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ACTION="all"
BUILD_SOURCE=0
DRY_RUN=0
CONFIRMED=0
EXTRA_ARGS=""

usage() {
    code="${1:-2}"
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  (default)          install nightly release and activate swap"
    echo "  --build            build from git main rather than downloading release"
    echo "  --install-only     install binaries to /usr/local/bin without swapping"
    echo "  --swap-only        apply system tool swap only"
    echo "  --revert           revert system swap (restore upstream tools)"
    echo "  --purge            permanently purge diverted upstream tools"
    echo "  --dry-run          preview actions without making changes"
    echo "  -y, --yes          automatic yes to prompts (for purge)"
    echo "  -h, --help         show this help"
    exit "$code"
}

for arg in "$@"; do
    case "$arg" in
        --build)        BUILD_SOURCE=1 ;;
        --install-only) ACTION="install" ;;
        --swap-only)    ACTION="swap" ;;
        --revert)       ACTION="revert" ;;
        --purge)        ACTION="purge" ;;
        --dry-run)      DRY_RUN=1 ;;
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
        SWAP_CMD="${SCRIPT_DIR}/nightly/swap.sh --revert"
        if [ "$DRY_RUN" -eq 1 ]; then SWAP_CMD="${SWAP_CMD} --dry-run"; fi
        $SWAP_CMD
        ;;
    purge)
        PURGE_CMD="${SCRIPT_DIR}/nightly/purge.sh"
        if [ "$CONFIRMED" -eq 1 ]; then PURGE_CMD="${PURGE_CMD} -y"; fi
        if [ "$DRY_RUN" -eq 1 ]; then PURGE_CMD="${PURGE_CMD} --dry-run"; fi
        $PURGE_CMD
        ;;
    install)
        if [ "$DRY_RUN" -eq 1 ]; then
            echo "would install nightly binaries into /usr/local/bin (build_source=${BUILD_SOURCE})"
        else
            INSTALL_ARGS=""
            if [ "$BUILD_SOURCE" -eq 1 ]; then INSTALL_ARGS="--build"; fi
            "${SCRIPT_DIR}/nightly/install.sh" $INSTALL_ARGS
        fi
        ;;
    swap)
        SWAP_CMD="${SCRIPT_DIR}/nightly/swap.sh"
        if [ "$DRY_RUN" -eq 1 ]; then SWAP_CMD="${SWAP_CMD} --dry-run"; fi
        $SWAP_CMD
        ;;
    all)
        if [ "$DRY_RUN" -eq 1 ]; then
            echo "would install nightly binaries into /usr/local/bin (build_source=${BUILD_SOURCE})"
            "${SCRIPT_DIR}/nightly/swap.sh" --dry-run
        else
            INSTALL_ARGS=""
            if [ "$BUILD_SOURCE" -eq 1 ]; then INSTALL_ARGS="--build"; fi
            "${SCRIPT_DIR}/nightly/install.sh" $INSTALL_ARGS
            "${SCRIPT_DIR}/nightly/swap.sh"
        fi
        ;;
esac
