# Upstream provenance

PUSE-PSP vendors the portable C++ save engines and data catalogs from:

- Project: [Zannael/PUSE](https://github.com/Zannael/PUSE)
- Revision: `eab18a7baacec0db27ea19422d0e7e5d7ba44fab`
- Revision date: 2026-10-15
- License: MIT (`LICENSE-PUSE`)

Vendored paths originate from PUSE's `switch-homebrew/include/puse/core`,
`switch-homebrew/source/core`, and `switch-homebrew/romfs/data` trees. PSP-specific
changes are limited to portability fixes, asset lookup, RTC candidate output paths,
the native user interface, Memory Stick discovery, backups, and packaging.

PUSE-PSP currently exposes the Pokémon Unbound profile. The vendored engine also
contains PUSE's Radical Red profile data, but that profile is not enabled in the
PSP interface until it receives its own device-level round-trip testing.
