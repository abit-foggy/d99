#!/bin/sh
# Usage:
#   d99-purge.sh            list what would be removed (dry run)
#   d99-purge.sh --apply    actually delete
#
# Requires root. 
set -u

APPLY=0
for arg in "$@"; do
    case "$arg" in
        --apply) APPLY=1 ;;
        *) echo "usage: $0 [--apply]" >&2; exit 2 ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo "d99-purge: must run as root" >&2
    exit 1
fi

removed=0

for f in /usr/bin/dpkg-deb.upstream /usr/bin/dpkg-query.upstream \
         /usr/bin/dpkg.upstream /usr/bin/apt.upstream /usr/bin/apt-get.upstream \
         /usr/local/bin/apt.mint; do
    if [ -e "$f" ]; then
        if [ "$APPLY" -eq 1 ]; then
            rm -f "$f"
            echo "removed $f"
        else
            echo "would remove $f"
        fi
        removed=$((removed + 1))
    fi
done

# perl-based dpkg tooling
if [ -d /usr/share/dpkg ]; then
    if [ "$APPLY" -eq 1 ]; then
        rm -rf /usr/share/dpkg
        echo "removed /usr/share/dpkg"
    else
        echo "would remove /usr/share/dpkg"
    fi
    removed=$((removed + 1))
fi

if [ "$removed" -eq 0 ]; then
    echo "nothing to purge"
fi
if [ "$APPLY" -eq 0 ] && [ "$removed" -gt 0 ]; then
    echo
    echo "dry run only; pass --apply to delete"
    echo "note: diversion registrations are kept (updates still divert to *.upstream)"
fi
exit 0
