#!/bin/sh
set -u

D99ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${D99ROOT}/output/bin"
WORK="${TMPDIR:-/tmp}/d99-e2e.$$"
FAILS=0
PASS=0

say()  { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS + 1)); }
bad()  { FAILS=$((FAILS + 1)); printf 'FAIL: %s\n' "$*" >&2; }
check() { # check <description> <command...>
    desc="$1"; shift
    if "$@" >/dev/null 2>&1; then ok; else bad "${desc}"; fi
}

cleanup() {
    rm -rf "${WORK}"
}
trap cleanup EXIT INT TERM

say "== d99 end-to-end test suite =="

# ---------------------------------------------------------------- fixtures
mkdir -p "${WORK}"
R="${WORK}/root"
M="${WORK}/mirror"
S1="${WORK}/stage-lib"
S2="${WORK}/stage-base"
S3="${WORK}/stage-meta"

for d in "${R}/var/lib/dpkg/info" "${R}/var/lib/d99" "${R}/tmp" \
         "${R}/etc/apt" "${M}" \
         "${S1}/DEBIAN" "${S1}/usr/lib" \
         "${S2}/DEBIAN" "${S2}/usr/bin" \
         "${S3}/DEBIAN"; do
    mkdir -p "$d"
done
touch "${R}/var/lib/dpkg/status"

printf 'Package: libhello\nVersion: 2.0-1\nArchitecture: all\nDescription: hello library\n' \
    > "${S1}/DEBIAN/control"
printf 'fake-lib-content\n' > "${S1}/usr/lib/libhello.so"
check "build libhello package" "${BIN}/d99-deb" -Z gz -b "${S1}" "${M}/libhello_2.0-1_all.deb"

printf 'Package: hello-d99\nVersion: 1.0-1\nArchitecture: all\nDepends: libhello (>= 1.0)\nMaintainer: Test <t@example.org>\nDescription: greeter, needs libhello\n' \
    > "${S2}/DEBIAN/control"
cat > "${S2}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
mkdir -p "$DPKG_ROOT/tmp"
echo "postinst: $* DPKG_ROOT=$DPKG_ROOT SCRIPT=$DPKG_MAINTSCRIPT_NAME ARCH=$DPKG_MAINTSCRIPT_ARCH" \
    > "$DPKG_ROOT/tmp/postinst.out"
EOF
cat > "${S2}/DEBIAN/postrm" <<'EOF'
#!/bin/sh
mkdir -p "$DPKG_ROOT/tmp"
echo "postrm: $*" > "$DPKG_ROOT/tmp/postrm.out"
EOF
chmod 755 "${S2}/DEBIAN/postinst" "${S2}/DEBIAN/postrm"
printf '#!/bin/sh\necho hello\n' > "${S2}/usr/bin/hello-d99"
chmod 755 "${S2}/usr/bin/hello-d99"
check "build hello-d99 package" "${BIN}/d99-deb" -Z gz -b "${S2}" "${M}/hello-d99_1.0-1_all.deb"

printf 'Package: hello-meta\nVersion: 0.5\nArchitecture: all\nDepends: hello-d99\nDescription: metapackage\n' \
    > "${S3}/DEBIAN/control"
check "build hello-meta package" "${BIN}/d99-deb" -Z gz -b "${S3}" "${M}/hello-meta_0.5_all.deb"

# ------------------------------------------------------- d99-deb inspection
check "d99-deb -c lists package" sh -c \
    "\"${BIN}/d99-deb\" -c \"${M}/hello-d99_1.0-1_all.deb\" | grep -q usr/bin/hello-d99"
check "d99-deb -f reads fields" sh -c \
    "\"${BIN}/d99-deb\" -f \"${M}/hello-d99_1.0-1_all.deb\" Package Version | grep -q '1.0-1'"
check "d99-deb -x extracts payload" sh -c \
    "\"${BIN}/d99-deb\" -x \"${M}/hello-d99_1.0-1_all.deb\" \"${WORK}/out\" && test -x \"${WORK}/out/usr/bin/hello-d99\""

# ------------------------------------------------------- d99-inst lifecycle
check "install libhello into sandbox" "${BIN}/d99-inst" --root="${R}" -i "${M}/libhello_2.0-1_all.deb"
check "binary landed" test -f "${R}/usr/lib/libhello.so"
check "status stanza written" grep -q "Package: libhello" "${R}/var/lib/dpkg/status"

check "remove libhello" "${BIN}/d99-inst" --root="${R}" -r libhello
check "state is config-files" grep -q "Status: deinstall ok config-files" "${R}/var/lib/dpkg/status"
check "file removed on remove" test ! -f "${R}/usr/lib/libhello.so"

check "purge libhello" "${BIN}/d99-inst" --root="${R}" -P libhello
check "stanza dropped on purge" sh -c "! grep -q 'Package: libhello' '${R}/var/lib/dpkg/status'"

check "install hello-d99 (dep on libhello, missing)" sh -c \
    "! \"${BIN}/d99-inst\" --root=\"${R}\" -i \"${M}/hello-d99_1.0-1_all.deb\" 2>/dev/null"
say "  (dependency enforcement refused the install; as designed)"
check "install both as a batch" "${BIN}/d99-inst" --root="${R}" -i \
    "${M}/libhello_2.0-1_all.deb" "${M}/hello-d99_1.0-1_all.deb"
check "postinst ran with dpkg env" grep -q "SCRIPT=postinst" "${R}/tmp/postinst.out"
check "postinst saw configure" grep -q "postinst: configure" "${R}/tmp/postinst.out"
check "hello-d99 binary works" "${R}/usr/bin/hello-d99"

if [ -x /bin/sleep ]; then
    cp /bin/sleep "${S2}/usr/bin/hello-d99"
    "${BIN}/d99-deb" -Z gz -b "${S2}" "${M}/hello-d99_1.0-2_all.deb" >/dev/null
    "${R}/usr/bin/hello-d99" 5 &
    RUNNING_PID=$!
    sleep 0.1
    check "upgrade running executable without ETXTBSY" "${BIN}/d99-inst" --root="${R}" -i "${M}/hello-d99_1.0-2_all.deb"
    kill "${RUNNING_PID}" 2>/dev/null || true
    wait "${RUNNING_PID}" 2>/dev/null || true
fi

# ------------------------------------------------------- d99-query
check "query -W works" sh -c \
    "\"${BIN}/d99-query\" --admindir \"${R}/var/lib/dpkg\" -W hello-d99 | grep -q '1.0-1'"
check "query -S finds owner" sh -c \
    "\"${BIN}/d99-query\" --admindir \"${R}/var/lib/dpkg\" -S /usr/bin/hello-d99 | grep -q 'hello-d99'"
check "query -L lists files" sh -c \
    "\"${BIN}/d99-query\" --admindir \"${R}/var/lib/dpkg\" -L hello-d99 | grep -q '/usr/bin/hello-d99'"
check "query -s dumps stanza" sh -c \
    "\"${BIN}/d99-query\" --admindir \"${R}/var/lib/dpkg\" -s hello-d99 | grep -q 'Status: install ok installed'"
check "index rebuild" "${BIN}/d99-query" --admindir "${R}/var/lib/dpkg" --rebuild
check "indexed -S still works" sh -c \
    "\"${BIN}/d99-query\" --admindir \"${R}/var/lib/dpkg\" -S /usr/bin/hello-d99 | grep -q 'hello-d99'"
check "indexed -W still works" sh -c \
    "\"${BIN}/d99-query\" --admindir \"${R}/var/lib/dpkg\" -W hello-d99 | grep -q '1.0-1'"

# ------------------------------------------------------- d99-solve (file://)
{
    for f in "${M}"/*.deb; do
        "${BIN}/d99-deb" -I "$f" control | tail -n +4
        echo "Filename: $(basename "$f")"
        echo "Size: $(stat -c%s "$f")"
        echo "SHA256: $(sha256sum "$f" | cut -d' ' -f1)"
        echo ""
    done
} > "${M}/Packages"
printf 'deb file://%s ./\n' "${M}" > "${R}/etc/apt/sources.list"

check "solve update" "${BIN}/d99-solve" --root="${R}" update
check "solve search" sh -c \
    "\"${BIN}/d99-solve\" --root=\"${R}\" search hello-meta | grep -q metapackage"
check "solve show" sh -c \
    "\"${BIN}/d99-solve\" --root=\"${R}\" show libhello | grep -q 'Version: 2.0-1'"
check "query --json -l" sh -c \
    "\"${BIN}/d99-query\" --admindir \"${R}/var/lib/dpkg\" --json -l hello-d99 | grep -q '\"package\":\"hello-d99\"'"
check "solve api info" sh -c \
    "\"${BIN}/d99-solve\" --root=\"${R}\" api info | grep -q '\"status\":\"ok\"'"
check "solve api search" sh -c \
    "\"${BIN}/d99-solve\" --root=\"${R}\" api search hello-meta | grep -q '\"status\":\"ok\"'"
check "solve api show" sh -c \
    "\"${BIN}/d99-solve\" --root=\"${R}\" api show libhello | grep -q '\"candidate_version\":\"2.0-1\"'"
check "solve api plan install" sh -c \
    "\"${BIN}/d99-solve\" --root=\"${R}\" api plan install hello-meta | grep -q '\"action\":\"install\"'"
check "solve api rpc" sh -c \
    "printf '{\"id\":42,\"method\":\"info\"}\n' | \"${BIN}/d99-solve\" --root=\"${R}\" api rpc | grep -q '\"id\":42'"
check "solve install --simulate --json" sh -c \
    "\"${BIN}/d99-solve\" --root=\"${R}\" -y install hello-meta --simulate --json | grep -q '\"simulated\":true'"
check "libd99.so exists" test -f "${D99ROOT}/lib/libd99.so"

# clean the db so the SAT solver has real work to do
rm -rf "${R}/var/lib/dpkg" "${R}/var/cache" "${R}/var/lib/d99/lists"
mkdir -p "${R}/var/lib/dpkg/info" "${R}/var/lib/d99" "${R}/tmp"
touch "${R}/var/lib/dpkg/status"
"${BIN}/d99-solve" --root="${R}" update >/dev/null 2>&1

check "SAT-resolved install of hello-meta" "${BIN}/d99-solve" --root="${R}" -y install hello-meta
check "dependency libhello pulled in" sh -c \
    "\"${BIN}/d99-query\" --admindir \"${R}/var/lib/dpkg\" -W libhello | grep -q '2.0-1'"
check "dependency hello-d99 pulled in" sh -c \
    "\"${BIN}/d99-query\" --admindir \"${R}/var/lib/dpkg\" -W hello-d99 | grep -q '1.0-1'"
check "metapackage installed" sh -c \
    "\"${BIN}/d99-query\" --admindir \"${R}/var/lib/dpkg\" -W hello-meta | grep -q '0.5'"

check "extended_states recorded auto-install for dependencies" sh -c \
    "grep -A2 'Package: hello-d99' '${R}/var/lib/apt/extended_states' | grep -q 'Auto-Installed: 1'"
check "extended_states recorded manual-install for target" sh -c \
    "grep -A2 'Package: hello-meta' '${R}/var/lib/apt/extended_states' | grep -q 'Auto-Installed: 0'"
check "history log recorded install transaction" sh -c \
    "grep -q 'Install: hello-meta' '${R}/var/log/d99/history.log'"
check "solve upgrade when up to date" "${BIN}/d99-solve" --root="${R}" -y upgrade
ln -sf d99-solve "${BIN}/apt-mark"
check "apt-mark showauto shows dependencies" sh -c \
    "\"${BIN}/apt-mark\" --root=\"${R}\" showauto | grep -q hello-d99"
check "apt-mark hold works" "${BIN}/apt-mark" --root="${R}" hold hello-meta
check "apt-mark showhold shows hold" sh -c \
    "\"${BIN}/apt-mark\" --root=\"${R}\" showhold | grep -q hello-meta"
check "apt-mark unhold works" "${BIN}/apt-mark" --root="${R}" unhold hello-meta

check "solve remove hello-meta" "${BIN}/d99-solve" --root="${R}" -y remove hello-meta
check "autoremove removes orphaned hello-d99 and libhello" "${BIN}/d99-solve" --root="${R}" -y autoremove
check "orphans deinstalled" sh -c \
    "grep -A2 'Package: hello-d99' '${R}/var/lib/dpkg/status' | grep -q 'Status: deinstall ok config-files'"

check "solve clean clears cache" "${BIN}/d99-solve" --root="${R}" clean
check "solve autoclean runs" "${BIN}/d99-solve" --root="${R}" autoclean

mkdir -p "${R}/var/backups/d99/libhello"
cp "${M}/libhello_2.0-1_all.deb" "${R}/var/backups/d99/libhello/"
check "solve rollback restores package" "${BIN}/d99-solve" --root="${R}" rollback libhello
check "package installed after rollback" sh -c \
    "\"${BIN}/d99-query\" --admindir \"${R}/var/lib/dpkg\" -W libhello | grep -q '2.0-1'"

# ------------------------------------------------------- dpkg-divert interop
# A registered local diversion (as written by `dpkg-divert --local`)
# must make unpacking a package that ships /usr/bin/dpkg leave our
# symlink alone and write the payload to /usr/bin/dpkg.upstream.
DVIS="${WORK}/div"
mkdir -p "${DVIS}/root/var/lib/dpkg/info" "${DVIS}/root/var/lib/d99" \
         "${DVIS}/root/tmp" "${DVIS}/root/usr/bin" \
         "${DVIS}/stage/DEBIAN" "${DVIS}/stage/usr/bin"
touch "${DVIS}/root/var/lib/dpkg/status"
printf '/usr/bin/dpkg.upstream\n/usr/bin/dpkg\n:\n' \
    > "${DVIS}/root/var/lib/dpkg/diversions"
ln -s /usr/local/bin/d99-inst "${DVIS}/root/usr/bin/dpkg"
printf 'Package: dpkg\nVersion: 1.22-1\nArchitecture: amd64\nDescription: upstream dpkg\n' \
    > "${DVIS}/stage/DEBIAN/control"
printf 'upstream\n' > "${DVIS}/stage/usr/bin/dpkg"
"${BIN}/d99-deb" -Z gz -b "${DVIS}/stage" \
    "${DVIS}/dpkg_1.22-1_amd64.deb" >/dev/null 2>&1
"${BIN}/d99-inst" --root="${DVIS}/root" -i \
    "${DVIS}/dpkg_1.22-1_amd64.deb" >/dev/null 2>&1
check "diversions: symlink survives unpack" test -L "${DVIS}/root/usr/bin/dpkg"
check "diversions: payload diverted to .upstream" \
    test -f "${DVIS}/root/usr/bin/dpkg.upstream"
check "diversions: .list records diverted path" sh -c \
    "grep -q '/usr/bin/dpkg.upstream' '${DVIS}/root/var/lib/dpkg/info/dpkg.list'"
check "diversions: purge removes the diverted payload" sh -c \
    "\"${BIN}/d99-inst\" --root=\"${DVIS}/root\" -P dpkg >/dev/null 2>&1 && test ! -f \"${DVIS}/root/usr/bin/dpkg.upstream\""
check "diversions: symlink survives purge too" test -L "${DVIS}/root/usr/bin/dpkg"

# ------------------------------------------------------- d99-build
BT="${WORK}/build"
mkdir -p "${BT}/src" "${BT}/payload" "${BT}/out"
cat > "${BT}/src/prog.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("hi\n"); return 0; }
EOF
if command -v cc >/dev/null 2>&1; then
    cc -O2 -o "${BT}/payload/prog" "${BT}/src/prog.c" 2>/dev/null
    cat > "${BT}/d99.ini" <<EOF
[package]
name = built-by-d99
version = 3.1-4
architecture = auto
maintainer = Test <t@example.org>
description = built from a manifest
 continuation line here

[files]
/usr/bin/built-by-d99 = payload/prog
EOF
    ( cd "${BT}" && "${BIN}/d99-build" --manifest d99.ini --output out/pkg.deb ) >/dev/null 2>&1
    if [ -f "${BT}/out/pkg.deb" ]; then
        ok
        "${BIN}/d99-deb" -f "${BT}/out/pkg.deb" Depends 2>/dev/null | grep -q "libc6" \
            && ok || bad "ELF auto-depends (libc6)"
    else
        bad "d99-build produced a package"
    fi
else
    say "  (no compiler available; skipping d99-build ELF test)"
fi

# ------------------------------------------------------- fallback mechanics
if [ -x /usr/bin/dpkg-deb.upstream ] || [ -x /usr/bin/dpkg.upstream ]; then
    say "  (upstream backups present; fallback test would run here)"
else
    check "fallback dies cleanly without upstream" sh -c \
        "! \"${BIN}/d99-deb\" --definitely-unknown-flag 2>/dev/null"
fi

say
say "passed: ${PASS}, failed: ${FAILS}"
[ "${FAILS}" -eq 0 ] || exit 1
exit 0
