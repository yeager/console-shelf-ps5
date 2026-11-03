# isShelf console tile assets and offline forwarders

This tool prepares one opt-in tile per console or the reusable artwork/template
bundle for the isShelf app. It never connects to a PS5, registers a title, or
changes the shell database. Runtime creation/edit/removal is a separate feature.

## Source and contract

The small tiles use the public [PS5 Forwarder Format v1](https://github.com/heydemoura/ps5-forwarder-format/blob/main/SPEC.md).
Their metadata/runtime template and license come from upstream commit
`22e8762ad7dbd01e18bd8c0e95f86461fdf8aaf4`; their executable is isShelf's
reviewed cooperative v3 forwarder. The generator checks the upstream commit,
the local build record and every SHA-256 pin before copying those files. The
GPL-3.0-or-later license is copied with the output.
The target is isShelf `PPSA99541`; the only forwarded argument is
`--isshelf-console-tile=<catalog-id>,<library|home|emulator>,<emulator-title-id|->`.
The v3 cooperative runtime validates this bounded argument while the shortcut
is foreground, requests a clean shell close, waits for stable Home, and opens
an emulator route directly at its pinned menu. Library and Home routes still
open the matching isShelf build with the requested console. An older installed
isShelf build will just open normally.

Every tile uses its own 512×512 icon composed from the catalog's reviewed
console illustration and the pinned isShelf product-stage artwork. There is no
extra console-name tag inside the picture: PS5 Home already presents the full
title. The generated stage follows isShelf's midnight navy, mint, cyan and
periwinkle visual language, while the accurate console cutout remains
unchanged. The builder has pinned artwork for all 52 catalog consoles. Icon
generation needs Pillow.

## Preparing resources

Clone the reviewed upstream source into the ignored `.cache` area and check
out the pinned commit. The generator will refuse any other revision or changed
template bytes:

```powershell
git clone https://github.com/heydemoura/ps5-forwarder-format.git .cache/ps5-forwarder-format
git -C .cache/ps5-forwarder-format checkout --detach 22e8762ad7dbd01e18bd8c0e95f86461fdf8aaf4
python tools/console_tiles/build.py bundle --output .cache/console-tiles/bundle
```

The complete reviewed artwork set is versioned under
`assets/console-tiles/icons/`. To reproduce it byte-for-byte into a new folder:

```powershell
python tools/console_tiles/build.py reviewed-icons `
  --output .cache/console-tiles/reviewed-icons
```

The redesigned bundle is about 17.3 MiB with `template/eboot.bin`,
`template/sce_module/libc.prx`, `template/param.json`, 52 `icons/<id>.png`,
`catalog.json` with hashes and preferred title IDs, and the forwarder license.
For in-app tile creation, package it into
`/app0/assets/console-tiles/` in the main isShelf title. A bundle alone does
not add console tiles.

## Offline single-tile proof

Supply a fresh, complete installed-title inventory from the console. The
builder accepts the JSON shape returned by ShadowMountPlus `list_games` or a
checked array of title IDs. It also needs an independent folder listing at
installation time, because an unregistered `/data/homebrew` folder may be
absent from `list_games`.

```powershell
python tools/console_tiles/build.py forwarder `
  --system ps1 --destination library `
  --inventory-json .cache/console-tiles/installed-titles.json `
  --output .cache/console-tiles/one-tile
```

For the emulator-menu destination, use `--destination emulator` and
`--emulator PPSA99248`, with that title ID present in the inventory. The selected emulator
must also be a route isShelf associates with that console. `home` and `library`
must omit `--emulator`.

The preferred title ID is `PPSA99700 + catalog index`. If occupied, the
builder selects the first free alternate from `PPSA99752..PPSA99899`, excluding
reserved `PPSA99764`. A checked `--title-id` can choose a different free ID
in that range. This is only an offline collision check: recheck registered
titles **and** `/data/homebrew` and `/user/app` folders immediately before
installation. Never overwrite a title another app owns. Edit an existing
isShelf tile by replacing its `forwarder.json` atomically after verifying its
immutable `isshelf-console-tile.json` owner marker; its title ID and icon stay
the same. Registration is required when the icon/name changes, not for a
destination-only edit under the format specification.

Tile output is create-only and includes:

```text
PPSA99704/
  eboot.bin
  sce_module/libc.prx
  forwarder.json
  sce_sys/param.json
  sce_sys/icon0.png
  isshelf-console-tile.json
  LICENSE-forwarder.txt
```

The marker holds immutable owner, system, title and source identity. The live
destination is stored only in `forwarder.json`, so a destination edit is one
atomic file replacement. Missing emulator and failed launch behavior must be
verified on the owner's PS5 before this output is offered as an installed
feature. These forwarders have no native source-console platform tag; a
separate reversible and tested shell-metadata path would be needed for that.
If the forwarder files are ever distributed, include the matching upstream
source at the pinned commit in the corresponding source delivery.

## Local checks

```powershell
python tools/console_tiles/test_build.py
```

The tests verify 52 distinct IDs, argument validation, inventory rejection,
preferred/alternate allocation, one generated forwarder, metadata, artwork,
create-only output, and collisions. They do not prove PS5 registration or
startup. The integration test needs Pillow and the pinned checkout.
