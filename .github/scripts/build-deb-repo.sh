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

# 1. Build and package Stable (v0.1.0-1)
echo "==> Building stable package (v0.1.0-1)..."
make clean
make VERSION="0.1.0" -j"$(nproc 2>/dev/null || echo 2)"
STAGE_DIR="$(mktemp -d /tmp/d99-deb-stable.XXXXXX)"
mkdir -p "${STAGE_DIR}/DEBIAN" "${STAGE_DIR}/usr/bin"
cp "${REPO_ROOT}/output/bin/"d99-* "${STAGE_DIR}/usr/bin/"
chmod 0755 "${STAGE_DIR}/usr/bin/"*
cat > "${STAGE_DIR}/DEBIAN/control" <<EOF
Package: d99
Version: 0.1.0-1
Section: utils
Priority: optional
Architecture: amd64
Maintainer: abit-foggy <https://github.com/abit-foggy/d99>
Depends: libc6, zlib1g, liblzma5, libzstd1
Description: Debian packaging toolchain in C99
 d99 is a lightweight Debian packaging toolchain implemented in C99,
 providing d99-deb, d99-query, d99-inst, d99-solve, and d99-build.
EOF
dpkg-deb -b "${STAGE_DIR}" "${REPO_DIR}/pool/main/d/d99/d99_0.1.0-1_amd64.deb"
rm -rf "${STAGE_DIR}"

# 2. Build and package Nightly (999.0.0+nightly.<sha>)
NIGHTLY_VER="999.0.0+nightly.${SHORT_SHA}"
echo "==> Building nightly package (${NIGHTLY_VER})..."
make clean
make VERSION="${NIGHTLY_VER}" -j"$(nproc 2>/dev/null || echo 2)"
STAGE_DIR="$(mktemp -d /tmp/d99-deb-nightly.XXXXXX)"
mkdir -p "${STAGE_DIR}/DEBIAN" "${STAGE_DIR}/usr/bin"
cp "${REPO_ROOT}/output/bin/"d99-* "${STAGE_DIR}/usr/bin/"
chmod 0755 "${STAGE_DIR}/usr/bin/"*
cat > "${STAGE_DIR}/DEBIAN/control" <<EOF
Package: d99
Version: ${NIGHTLY_VER}
Section: utils
Priority: optional
Architecture: amd64
Maintainer: abit-foggy <https://github.com/abit-foggy/d99>
Depends: libc6, zlib1g, liblzma5, libzstd1
Description: Debian packaging toolchain in C99 (Nightly snapshot)
 d99 is a lightweight Debian packaging toolchain implemented in C99,
 providing d99-deb, d99-query, d99-inst, d99-solve, and d99-build.
 Nightly builds track main development snapshots and out-version stable.
EOF
dpkg-deb -b "${STAGE_DIR}" "${REPO_DIR}/pool/main/d/d99/d99_${NIGHTLY_VER}_amd64.deb"
rm -rf "${STAGE_DIR}"

# Copy deb files to repo root as well so flat path './' works directly
cp "${REPO_DIR}/pool/main/d/d99/"*.deb "${REPO_DIR}/"

# 3. Generate APT repository indexes
echo "==> Generating APT indexes..."
# dists/stable (includes both stable & nightly so updating 'stable' picks up nightly if present, or stable)
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

# dists/nightly
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

# 4. Generate Landing Pages (HTML)
echo "==> Generating HTML landing pages..."
cat > "${REPO_DIR}/index.html" <<'EOF'
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>d99 APT Repository</title>
  <style>
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      max-width: 820px;
      margin: 40px auto;
      padding: 0 20px;
      line-height: 1.6;
      color: #24292f;
      background: #f6f8fa;
    }
    .card {
      background: #ffffff;
      border: 1px solid #d0d7de;
      border-radius: 8px;
      padding: 24px 32px;
      box-shadow: 0 1px 3px rgba(31, 35, 40, 0.04);
    }
    h1 { margin-top: 0; color: #0969da; }
    h2 { border-bottom: 1px solid #d8dee4; padding-bottom: 8px; margin-top: 24px; }
    pre {
      background: #f6f8fa;
      border: 1px solid #d0d7de;
      border-radius: 6px;
      padding: 14px;
      overflow-x: auto;
      font-size: 14px;
    }
    code { font-family: ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, monospace; }
    a { color: #0969da; text-decoration: none; }
    a:hover { text-decoration: underline; }
    table { width: 100%; border-collapse: collapse; margin-top: 12px; }
    th, td { text-align: left; padding: 10px 12px; border-bottom: 1px solid #d0d7de; }
    th { background: #f6f8fa; }
    .badge {
      display: inline-block;
      padding: 2px 8px;
      font-size: 12px;
      font-weight: 600;
      border-radius: 12px;
      background: #ddf4ff;
      color: #0969da;
    }
  </style>
</head>
<body>
  <div class="card">
    <h1>d99 APT Repository</h1>
    <p>Debian packaging toolchain in C99. Fast, self-contained replacements and companions for dpkg, apt, and debian tools.</p>

    <h2>Quick Setup</h2>
    <p>Add this repository to your system APT sources:</p>
    <pre><code># Add repository source
echo "deb [trusted=yes] https://abit-foggy.github.io/d99/repo/ stable main" | sudo tee /etc/apt/sources.list.d/d99.list

# Update package index and install d99
sudo apt update
sudo apt install d99</code></pre>

    <p>Or using flat repository format:</p>
    <pre><code>echo "deb [trusted=yes] https://abit-foggy.github.io/d99/repo/ ./" | sudo tee /etc/apt/sources.list.d/d99.list
sudo apt update
sudo apt install d99</code></pre>

    <h2>Repository Tree</h2>
    <ul>
      <li><a href="dists/stable/Release">dists/stable/Release</a></li>
      <li><a href="dists/stable/main/binary-amd64/Packages">dists/stable/main/binary-amd64/Packages</a> (<a href="dists/stable/main/binary-amd64/Packages.gz">Packages.gz</a>)</li>
      <li><a href="Packages">Flat Packages index</a> (<a href="Packages.gz">Packages.gz</a>)</li>
      <li><a href="Release">Flat Release</a></li>
    </ul>

    <h2>Packages in Repository</h2>
    <table>
      <thead>
        <tr>
          <th>Package</th>
          <th>Version</th>
          <th>Architecture</th>
          <th>Download</th>
        </tr>
      </thead>
      <tbody>
        <tr>
          <td><strong>d99</strong> <span class="badge">nightly</span></td>
          <td>Nightly Snapshot</td>
          <td>amd64</td>
          <td><a href="pool/main/d/d99/">pool/main/d/d99/</a></td>
        </tr>
        <tr>
          <td><strong>d99</strong> <span class="badge">stable</span></td>
          <td>0.1.0-1</td>
          <td>amd64</td>
          <td><a href="pool/main/d/d99/d99_0.1.0-1_amd64.deb">d99_0.1.0-1_amd64.deb</a></td>
        </tr>
      </tbody>
    </table>

    <p style="margin-top: 24px; font-size: 14px; color: #57609a;">
      Project source repository: <a href="https://github.com/abit-foggy/d99">github.com/abit-foggy/d99</a>
    </p>
  </div>
</body>
</html>
EOF

# Root landing redirect
cat > "${SITE_DIR}/index.html" <<'EOF'
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>d99 Repository</title>
  <meta http-equiv="refresh" content="0; url=repo/">
  <script>window.location.replace("repo/");</script>
</head>
<body>
  <p>Redirecting to <a href="repo/">d99 APT repository</a>...</p>
</body>
</html>
EOF

echo "==> APT repository successfully generated in ${REPO_DIR}."
