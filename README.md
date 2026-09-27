# d99

Debian packaging toolchain in C99.

## Components

- `d99-deb`: archive manipulation (`dpkg-deb`)
- `d99-query`: package database queries (`dpkg-query`)
- `d99-inst`: package installer and transaction coordinator (`dpkg`)
- `d99-solve`: dependency solver and repository fetcher (`apt` / `apt-get`)
- `d99-build`: manifest-based package authoring tool

## Installation

### Stable Release

Install latest stable release and activate system swap in one step:

```sh
curl -fsSL https://raw.githubusercontent.com/abit-foggy/d99/main/scripts/stable.sh | sudo sh
```

Or run modular steps individually:

```sh
# install binaries only into /usr/local/bin
curl -fsSL https://raw.githubusercontent.com/abit-foggy/d99/main/scripts/stable/install.sh | sudo sh

# activate system swap
curl -fsSL https://raw.githubusercontent.com/abit-foggy/d99/main/scripts/stable/swap.sh | sudo sh
```

### Nightly Build

Install latest nightly build and activate system swap:

```sh
curl -fsSL https://raw.githubusercontent.com/abit-foggy/d99/main/scripts/nightly.sh | sudo sh
```

Or run modular steps individually:

```sh
# install nightly binaries (or build from source if prebuilt asset is not yet available)
curl -fsSL https://raw.githubusercontent.com/abit-foggy/d99/main/scripts/nightly/install.sh | sudo sh

# activate system swap
curl -fsSL https://raw.githubusercontent.com/abit-foggy/d99/main/scripts/nightly/swap.sh | sudo sh
```

Pre-built release tarballs are also directly downloadable:

```sh
curl -fsSL https://github.com/abit-foggy/d99/releases/latest/download/d99-linux-amd64.tar.gz | sudo tar -xz -C /usr/local/bin
```

## Building

```sh
make
make check
sudo make install
```

Requires a C99 compiler and POSIX libc.
Optional compression libraries: `zlib`, `liblzma`, `libzstd`.

## Usage

```sh
# build package from manifest
d99-build --manifest d99.ini

# inspect and extract .deb archives
d99-deb -c package.deb
d99-deb -I package.deb
d99-deb -x package.deb ./out

# install / query packages
d99-inst -i package.deb
d99-query -l
d99-query -s package-name
d99-query -L package-name
d99-query -S /path/to/file

# repository operations
d99-solve update
d99-solve search <term>
d99-solve install <package>
d99-solve remove <package>
```

## System Swap

To swap system tools with d99 binaries:

```sh
# using runner
sudo scripts/stable.sh --swap-only          # or scripts/nightly.sh --swap-only
sudo scripts/stable.sh --revert             # restore system tools
sudo scripts/stable.sh --swap-only --dry-run # preview changes without applying

# or using modular swap script
sudo scripts/stable/swap.sh                 # divert and symlink over system tools
sudo scripts/stable/swap.sh --revert        # restore system tools
sudo scripts/stable/swap.sh --dry-run       # preview changes without applying
```

## System Purge

> [!WARNING]
> Running `purge` permanently deletes the diverted upstream tool binaries (`/usr/bin/*.upstream`) and upstream Perl packaging directories (`/usr/share/dpkg`). This action is irreversible and prevents reverting back to upstream APT/dpkg via `--revert`. Only apply this if you intend to run `d99` completely standalone without upstream fallback.

```sh
sudo scripts/stable.sh --purge --dry-run    # preview files to purge
sudo scripts/stable.sh --purge              # purge with confirmation prompt
sudo scripts/stable.sh --purge -y           # purge without confirmation prompt
```

## License

This project is licensed under the BSD 2-Clause Simplified License. See [LICENSE](LICENSE) for details.
