#!/bin/sh
# Usage:
#   d99-swap.sh            divert upstream binaries and install symlinks
#   d99-swap.sh --dry-run  show what would be done (dry run)
#   d99-swap.sh --revert   remove symlinks and restore upstream binaries
#
# Requires root.  Run AFTER `make install` (tools live in /usr/local/bin).
set -u

# upstream:d99 for each swap point
TOOLS="dpkg-deb:d99-deb dpkg-query:d99-query dpkg:d99-inst apt:d99-solve apt-get:d99-solve"
APPLY=1
REVERT=0
DRY_RUN=0

for arg in "$@"; do
    case "$arg" in
        --apply)   APPLY=1 ;;
        --dry-run) APPLY=0; DRY_RUN=1 ;;
        --revert)  REVERT=1; APPLY=0 ;;
        *) echo "usage: $0 [--dry-run|--revert|--apply]" >&2; exit 2 ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo "d99-swap: must run as root to modify /usr/bin" >&2
    exit 1
fi

D99_BINDIR="${D99_BINDIR:-/usr/local/bin}"
DIVERT=/usr/bin/dpkg-divert   # the upstream tool itself; never diverted

if [ ! -x "$DIVERT" ]; then
    echo "d99-swap: $DIVERT not available" >&2
    exit 1
fi

have_diversion() {   # have_diversion <upstream-path>
    # output is e.g. "local diversion of /usr/bin/dpkg to /usr/bin/dpkg.upstream";
    # the trailing ' to ' avoids prefix-matching /usr/bin/dpkg-deb
    "$DIVERT" --list "$1" 2>/dev/null | grep -q "diversion of $1 to "
}

do_apply_one() {
    upstream="$1"; ours="$2"
    target="/usr/bin/${upstream}"
    diverted="${target}.upstream"

    if [ ! -x "${D99_BINDIR}/${ours}" ]; then
        echo "d99-swap: missing ${D99_BINDIR}/${ours}; run 'make install' first" >&2
        return 1
    fi

    if have_diversion "$target"; then
        echo "diversion already registered: ${target}"
    elif [ -e "$diverted" ]; then
        # legacy state from the old mv-based swap: the file already
        # lives at .upstream; adopt it by registering WITHOUT --rename.
        if [ "$APPLY" -eq 1 ]; then
            echo "adopting existing ${diverted} into the diversion database"
            "$DIVERT" --divert "$diverted" --local "$target" || return 1
        else
            echo "would register diversion ${target} -> ${diverted}"
            echo "  (file already present; no --rename needed)"
        fi
    elif [ -e "$target" ]; then
        if [ "$APPLY" -eq 1 ]; then
            echo "diverting ${target} -> ${diverted}"
            "$DIVERT" --divert "$diverted" --rename --local "$target" || return 1
        else
            echo "would divert ${target} -> ${diverted}"
        fi
    else
        echo "d99-swap: no upstream '${upstream}' installed; skipping" >&2
        return 0
    fi

    if [ "$APPLY" -eq 1 ]; then
        ln -sf "${D99_BINDIR}/${ours}" "$target"
        echo "linked ${target} -> ${D99_BINDIR}/${ours}"
    else
        echo "would link ${target} -> ${D99_BINDIR}/${ours}"
    fi
}

do_revert_one() {
    upstream="$1"
    target="/usr/bin/${upstream}"
    diverted="${target}.upstream"

    if [ -L "$target" ]; then
        if [ "$REVERT" -eq 1 ]; then
            rm -f "$target"
            echo "removed symlink ${target}"
        else
            echo "would remove symlink ${target}"
        fi
    fi
    # remove the symlink BEFORE dpkg-divert --rename --remove: the
    # original path must be free so the diverted file can move back.
    if have_diversion "$target"; then
        if [ "$REVERT" -eq 1 ]; then
            echo "removing diversion for ${target} (restoring upstream)"
            "$DIVERT" --rename --remove "$target" || return 1
        else
            echo "would run: dpkg-divert --rename --remove ${target}"
        fi
    elif [ -e "$diverted" ]; then
        if [ "$REVERT" -eq 1 ]; then
            echo "restoring legacy backup ${diverted}"
            mv "$diverted" "$target"
        else
            echo "would restore legacy backup ${diverted}"
        fi
    else
        echo "d99-swap: nothing to revert for ${upstream}" >&2
    fi
    return 0
}

do_apply_mint() {
    mint_apt="/usr/local/bin/apt"
    mint_diverted="/usr/local/bin/apt.mint"
    if [ ! -e "$mint_apt" ] && ! have_diversion "$mint_apt"; then
        return 0
    fi
    if [ -L "$mint_apt" ] && [ "$(readlink -f "$mint_apt")" = "${D99_BINDIR}/d99-solve" ]; then
        return 0
    fi
    if have_diversion "$mint_apt"; then
        echo "diversion already registered: ${mint_apt}"
    elif [ -e "$mint_apt" ] && [ ! -L "$mint_apt" ]; then
        if [ "$APPLY" -eq 1 ]; then
            echo "diverting wrapper ${mint_apt} -> ${mint_diverted}"
            "$DIVERT" --divert "$mint_diverted" --rename --local "$mint_apt" || return 1
        else
            echo "would divert wrapper ${mint_apt} -> ${mint_diverted}"
        fi
    fi
    if [ "$APPLY" -eq 1 ]; then
        ln -sf "${D99_BINDIR}/d99-solve" "$mint_apt"
        echo "linked ${mint_apt} -> ${D99_BINDIR}/d99-solve"
    else
        echo "would link ${mint_apt} -> ${D99_BINDIR}/d99-solve"
    fi
}

do_revert_mint() {
    mint_apt="/usr/local/bin/apt"
    if [ ! -e "$mint_apt" ] && ! have_diversion "$mint_apt"; then
        return 0
    fi
    if [ -L "$mint_apt" ]; then
        if [ "$REVERT" -eq 1 ]; then
            rm -f "$mint_apt"
            echo "removed symlink ${mint_apt}"
        else
            echo "would remove symlink ${mint_apt}"
        fi
    fi
    if have_diversion "$mint_apt"; then
        if [ "$REVERT" -eq 1 ]; then
            echo "removing diversion for ${mint_apt} (restoring wrapper)"
            "$DIVERT" --rename --remove "$mint_apt" || return 1
        else
            echo "would run: dpkg-divert --rename --remove ${mint_apt}"
        fi
    fi
}

status=0
if [ "$REVERT" -eq 1 ]; then
    do_revert_mint || status=1
    for pair in $TOOLS; do
        do_revert_one "${pair%%:*}" || status=1
    done
else
    for pair in $TOOLS; do
        do_apply_one "${pair%%:*}" "${pair##*:}" || status=1
    done
    do_apply_mint || status=1
fi

if [ "$DRY_RUN" -eq 1 ]; then
    echo
    echo "dry run only; omit --dry-run to perform the swap (or --revert to undo)"
fi
exit "$status"
