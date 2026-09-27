# d99

Debian packaging toolchain in C99.

## Components

- `d99-deb`: archive manipulation (`dpkg-deb`)
- `d99-query`: package database queries (`dpkg-query`)
- `d99-inst`: package installer and transaction coordinator (`dpkg`)
- `d99-solve`: dependency solver and repository fetcher (`apt` / `apt-get`)
- `d99-build`: manifest-based package authoring tool

## Installation

Install pre-built release binaries:

```sh
curl -fsSL https://github.com/abit-foggy/d99/releases/latest/download/d99-linux-amd64.tar.gz | sudo tar -xz -C /usr/local/bin
```

System swap (divert upstream tools and activate d99):

```sh
curl -fsSL https://raw.githubusercontent.com/abit-foggy/d99/main/scripts/d99-swap.sh | sudo sh
```

System purge (permanently remove diverted upstream binaries):

```sh
curl -fsSL https://raw.githubusercontent.com/abit-foggy/d99/main/scripts/d99-purge.sh | sudo sh
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

```sh
sudo scripts/d99-swap.sh            # divert and symlink over system tools
sudo scripts/d99-swap.sh --revert   # restore system tools
sudo scripts/d99-swap.sh --dry-run  # preview changes without applying
```

## System Purge

> [!WARNING]
> Running `d99-purge.sh --apply` permanently deletes the diverted upstream tool binaries (`/usr/bin/*.upstream`) and upstream Perl packaging directories (`/usr/share/dpkg`). This action is irreversible and prevents reverting back to upstream APT/dpkg via `d99-swap.sh --revert`. Only apply this if you intend to run `d99` completely standalone without upstream fallback.

```sh
sudo scripts/d99-purge.sh           # dry run
sudo scripts/d99-purge.sh --apply   # replace system tools
```

## License

This project is licensed under the BSD 2-Clause Simplified License. See [LICENSE](LICENSE) for details.
