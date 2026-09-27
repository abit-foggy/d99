#!/bin/sh
# Usage:
#   d99-purge.sh            permanently delete diverted upstream binaries (prompts for confirmation)
#   d99-purge.sh -y         permanently delete without prompting
#   d99-purge.sh --dry-run  list what would be removed
#
# Requires root.
set -u

DRY_RUN=0
CONFIRMED=0

for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        -y|--yes|-f|--force) CONFIRMED=1 ;;
        --apply) ;;
        *) echo "usage: $0 [--dry-run] [-y|--yes]" >&2; exit 2 ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo "d99-purge: must run as root" >&2
    exit 1
fi

CANDIDATES="/usr/bin/dpkg-deb.upstream /usr/bin/dpkg-query.upstream \
            /usr/bin/dpkg.upstream /usr/bin/apt.upstream /usr/bin/apt-get.upstream \
            /usr/local/bin/apt.mint"

TO_REMOVE=""
for f in $CANDIDATES; do
    if [ -e "$f" ]; then
        TO_REMOVE="$TO_REMOVE $f"
    fi
done

if [ -d /usr/share/dpkg ]; then
    TO_REMOVE="$TO_REMOVE /usr/share/dpkg"
fi

if [ -z "$TO_REMOVE" ]; then
    echo "nothing to purge"
    exit 0
fi

if [ "$DRY_RUN" -eq 1 ]; then
    for item in $TO_REMOVE; do
        echo "would remove $item"
    done
    echo
    echo "dry run only; omit --dry-run to purge"
    echo "note: diversion registrations are kept (updates still divert to *.upstream)"
    exit 0
fi

echo "The following upstream files and directories will be permanently removed:"
for item in $TO_REMOVE; do
    echo "  $item"
done
echo

if [ "$CONFIRMED" -eq 0 ]; then
    printf "Do you really want to permanently purge these files? [y/N] "
    reply=""
    if [ -t 0 ]; then
        read -r reply
    elif [ -r /dev/tty ]; then
        read -r reply < /dev/tty
    else
        echo >&2
        echo "d99-purge: interactive input not available; pass -y to confirm" >&2
        exit 1
    fi

    case "$reply" in
        [yY]|[yY][eE][sS]) ;;
        *)
            echo "Purge cancelled."
            exit 0
            ;;
    esac
fi

for item in $TO_REMOVE; do
    rm -rf "$item"
    echo "removed $item"
done

echo
echo "note: diversion registrations are kept (updates still divert to *.upstream)"
exit 0
