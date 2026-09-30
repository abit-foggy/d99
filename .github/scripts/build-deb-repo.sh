#!/bin/sh
set -eu

# Script to build d99 Debian packages and APT repository layout
SITE_DIR="${1:-site}"
REPO_DIR="${SITE_DIR}/repo"
GIT_SHA="$(git rev-parse HEAD 2>/dev/null || echo "unknown")"
SHORT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")"
REPO_ROOT="$(pwd)"

echo "==> Preparing APT repository in ${REPO_DIR}..."
rm -rf "${REPO_DIR}"
mkdir -p "${REPO_DIR}/pool/main/d/d99"
mkdir -p "${REPO_DIR}/dists/stable/main/binary-amd64"
mkdir -p "${REPO_DIR}/dists/stable/main/binary-all"
mkdir -p "${REPO_DIR}/dists/nightly/main/binary-amd64"
mkdir -p "${REPO_DIR}/dists/nightly/main/binary-all"

# 1. Build and package Stable (v0.2.0-1)
STABLE_VER="0.2.0"
echo "==> Building stable package (v${STABLE_VER}-1)..."
make clean
make VERSION="${STABLE_VER}" -j"$(nproc 2>/dev/null || echo 2)"
STAGE_DIR="$(mktemp -d /tmp/d99-deb-stable.XXXXXX)"
mkdir -p "${STAGE_DIR}/DEBIAN" "${STAGE_DIR}/usr/bin"
cp "${REPO_ROOT}/output/bin/"d99-* "${STAGE_DIR}/usr/bin/"
chmod 0755 "${STAGE_DIR}/usr/bin/"*
cat > "${STAGE_DIR}/DEBIAN/control" <<EOF
Package: d99
Version: ${STABLE_VER}-1
Section: utils
Priority: optional
Architecture: amd64
Maintainer: abit-foggy <https://github.com/abit-foggy/d99>
Depends: libc6, zlib1g, liblzma5, libzstd1
Provides: dpkg (= 99:999.0.0), apt (= 99:999.0.0), dpkg-dev (= 99:999.0.0), apt-utils (= 99:999.0.0)
Replaces: d99-nightly, dpkg, apt, dpkg-dev, apt-utils
Conflicts: d99-nightly
Description: Debian packaging toolchain in C99 (Stable)
 d99 is a lightweight Debian packaging toolchain implemented in C99,
 superseding standard dpkg, apt, dpkg-dev, and apt-utils.
EOF
dpkg-deb -b "${STAGE_DIR}" "${REPO_DIR}/pool/main/d/d99/d99_${STABLE_VER}-1_amd64.deb"
rm -rf "${STAGE_DIR}"

# 2. Build and package Nightly (d99-nightly: 999.0.0+nightly.<sha>)
NIGHTLY_VER="999.0.0+nightly.${SHORT_SHA}"
echo "==> Building nightly package (${NIGHTLY_VER})..."
make clean
make VERSION="${NIGHTLY_VER}" -j"$(nproc 2>/dev/null || echo 2)"
STAGE_DIR="$(mktemp -d /tmp/d99-deb-nightly.XXXXXX)"
mkdir -p "${STAGE_DIR}/DEBIAN" "${STAGE_DIR}/usr/bin"
cp "${REPO_ROOT}/output/bin/"d99-* "${STAGE_DIR}/usr/bin/"
chmod 0755 "${STAGE_DIR}/usr/bin/"*
cat > "${STAGE_DIR}/DEBIAN/control" <<EOF
Package: d99-nightly
Version: ${NIGHTLY_VER}
Section: utils
Priority: optional
Architecture: amd64
Maintainer: abit-foggy <https://github.com/abit-foggy/d99>
Depends: libc6, zlib1g, liblzma5, libzstd1
Provides: d99 (= ${NIGHTLY_VER}), dpkg (= 99:999.0.0), apt (= 99:999.0.0), dpkg-dev (= 99:999.0.0), apt-utils (= 99:999.0.0)
Replaces: d99, dpkg, apt, dpkg-dev, apt-utils
Conflicts: d99
Description: Debian packaging toolchain in C99 (Nightly)
 d99 is a lightweight Debian packaging toolchain implemented in C99,
 superseding standard dpkg, apt, dpkg-dev, and apt-utils.
 Nightly builds track main development snapshots and out-version stable.
EOF
dpkg-deb -b "${STAGE_DIR}" "${REPO_DIR}/pool/main/d/d99/d99-nightly_${NIGHTLY_VER}_amd64.deb"
rm -rf "${STAGE_DIR}"

# Copy deb files to repo root as well so flat path './' works directly
cp "${REPO_DIR}/pool/main/d/d99/"*.deb "${REPO_DIR}/"

# 3. Generate APT repository indexes
echo "==> Generating APT indexes..."
# dists/stable (includes both d99 and d99-nightly)
(cd "${REPO_DIR}" && apt-ftparchive packages pool > dists/stable/main/binary-amd64/Packages)
gzip -9k -f "${REPO_DIR}/dists/stable/main/binary-amd64/Packages"
touch "${REPO_DIR}/dists/stable/main/binary-all/Packages"
gzip -9k -f "${REPO_DIR}/dists/stable/main/binary-all/Packages"
(cd "${REPO_DIR}" && apt-ftparchive \
    -o APT::FTPArchive::Release::Origin="d99" \
    -o APT::FTPArchive::Release::Label="d99" \
    -o APT::FTPArchive::Release::Suite="stable" \
    -o APT::FTPArchive::Release::Codename="stable" \
    -o APT::FTPArchive::Release::Components="main" \
    -o APT::FTPArchive::Release::Architectures="amd64 all" \
    release dists/stable > dists/stable/Release)

# dists/nightly (includes both d99 and d99-nightly)
cp "${REPO_DIR}/dists/stable/main/binary-amd64/Packages" "${REPO_DIR}/dists/nightly/main/binary-amd64/Packages"
cp "${REPO_DIR}/dists/stable/main/binary-amd64/Packages.gz" "${REPO_DIR}/dists/nightly/main/binary-amd64/Packages.gz"
touch "${REPO_DIR}/dists/nightly/main/binary-all/Packages"
gzip -9k -f "${REPO_DIR}/dists/nightly/main/binary-all/Packages"
(cd "${REPO_DIR}" && apt-ftparchive \
    -o APT::FTPArchive::Release::Origin="d99" \
    -o APT::FTPArchive::Release::Label="d99" \
    -o APT::FTPArchive::Release::Suite="nightly" \
    -o APT::FTPArchive::Release::Codename="nightly" \
    -o APT::FTPArchive::Release::Components="main" \
    -o APT::FTPArchive::Release::Architectures="amd64 all" \
    release dists/nightly > dists/nightly/Release)

# Flat index (for: deb https://abit-foggy.github.io/d99/repo/ ./)
(cd "${REPO_DIR}" && apt-ftparchive packages pool > Packages)
gzip -9k -f "${REPO_DIR}/Packages"
(cd "${REPO_DIR}" && apt-ftparchive \
    -o APT::FTPArchive::Release::Origin="d99" \
    -o APT::FTPArchive::Release::Label="d99" \
    -o APT::FTPArchive::Release::Suite="repo" \
    -o APT::FTPArchive::Release::Architectures="amd64 all" \
    release . > Release)

# 4. Generate dead simple file explorer indexes
echo "==> Generating file explorer directory indexes..."
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
"${SCRIPT_DIR}/make-file-indexes.py" "${SITE_DIR}"

echo "==> APT repository successfully generated in ${REPO_DIR}."
