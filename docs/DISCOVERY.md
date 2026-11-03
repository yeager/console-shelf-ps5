# Read-only library discovery

The native PS5 beta inspects existing libraries and displays real game lists. The implementation is original isShelf code. Discovery has been tested inside the PS5 app on firmware 12.20, with the owner's existing `ftpsrv`; the [final hardware checks](verification/ui24-owner-final-checks-2026-10-26.json) include actual library/launch acceptance. Discovery alone does not establish that every listed game is compatible. For everyday use, see [adding games](BEGINNERS.md#5-add-more-games).

## Connection and scope

The PS5 build uses anonymous FTP at **127.0.0.1:2121**, connecting only to the console's already running ftpsrv. It does not start a payload, expose a new server, resolve hostnames, or change network settings. It contains no FTP upload, delete, rename, permission or server-stop operation. Passive data connections remain pinned to the control peer even if PASV advertises another address. Connection, reply, metadata, directory and recursion sizes are bounded.

The normal PC preview remains offline. A maintainer can opt in explicitly:

```powershell
.\.cache\native-host\shelf-host.exe --asset-root . --data-dir .cache\library-review --library-ftp <console-LAN-IPv4> --scan-only
```

Omit `--scan-only` to browse the resulting inventory in the native PC UI. `--library-root <local-export-root>` instead reads a deliberately supplied local tree containing virtual PS5 paths; it never scans the PC's drives automatically. Host-only `--library-port` supports a test fixture. Console launch arguments cannot override storage or the loopback endpoint.

## What is currently recognised

| Source | Discovery rule |
| --- | --- |
| Game artwork | Exact serial/filename matches from existing cover folders, RetroArch thumbnails, and registered game icons. Images are cached separately in Shelf's own data folder; optional artwork failures do not hide games. See [game artwork](GAME-ART.md). |
| Registered emulator apps | Known title identity, public `sce_sys/param.json` and a present program. A source folder alone is insufficient. |
| RetroArch | Existing `*_libretro.so` files provide core evidence. Per-system folders under the app's `content/` are scanned using explicit folder aliases and format lists. An `.info` file or suggested core name does not count as an installed core. |
| PSXS5 / SwanStationPS5 / PSSwanStation | Existing `/data/PSXS5/games`, `/data/SwanStationPS5/games`, `/data/homebrew/PPSA99248/games` and `/data/psswanstation/games`. PSSwanStation is detected as title `PPSA99248`; SwanStationPS5 remains a separate choice. CUE track references and M3U disc references must resolve within the existing tree. Valid multi-disc manifests suppress their individual discs; BIN tracks are not separate PS1 games. |
| PS5SX2 | Existing `/data/PCSX2/games`. ISO candidates must have the ISO9660 descriptor; CHD candidates require their header. |
| PS5SX3 | Existing `/data/homebrew/PPSA99303/rpcs3/games`. Game folders require `PS3_GAME/PARAM.SFO` and `PS3_GAME/USRDIR/EBOOT.BIN`; title/name come from the bounded SFO parser. Keys are not read. |
| Native titles | Registered `/user/app` entries with matching public JSON/SFO identity and a program/package/mount marker. Known utility/emulator slots are excluded. A registered tile can still have a broken mount; its presence does not prove playability. |

N64 dumps require a recognised byte-order signature. Other listed formats are discovery candidates, not full archive-integrity or emulator compatibility certifications. Source paths stay unchanged. BIOS, keys, firmware, saves, logs, core binaries and temporary download extensions do not become games.

## Current limits

- The scanner examines the configured locations above; “complete” means those reads completed, not that every storage device and emulator layout was searched. Missing optional folders are normal; unreadable roots, limits and read failures keep the scan partial.
- No automatic import of arbitrary folders or archives. No custom-root editor or persistent inventory cache yet. Reopen Shelf with ftpsrv running to rescan.
- Switch NSP/NSZ base/update/DLC parsing, dedicated non-RetroArch libraries, and PS4 package-only metadata need additional adapters. Their game lists are not inferred from filenames or dependency folders.
- Native apps without accessible public metadata are omitted. Existing per-game settings and account/token files are never consulted.
- Default emulator selection prefers a detected standalone app over a RetroArch core. The owner can cycle detected choices in console details; their choice uses a stable route ID and saves in Shelf's own settings.
- The launch panel distinguishes direct RetroArch requests from standalone emulator libraries. The owner has verified PSXS5/PS5SX2/PS5SX3 menus and selected RetroArch game routes; other routes remain experimental. Discovery alone does not prove direct launch.

## Checks

`npm run test:native` exercises path/UTF-8 validation, bounded JSON parsing, duplicate-key/NUL rejection, real core evidence, disc reference deduplication, partial scans, preference selection and library navigation. `npm run test:library-ftp` uses an ephemeral loopback-only fake server: it checks permitted commands, multiline replies, EPSV/PASV peer pinning, native metadata and malformed directory refusal. It never contacts a PS5. `npm run test:native-renderer` continues to exercise the shared UI.

Public screenshots show owner-selected app views and their game/emulator counts. Raw inventory snapshots, IP addresses, account data and game files are not published. Local development snapshots remain under ignored `.cache/console-readonly/`.
