# Game artwork

Private build `00.100.027` presents front covers in console-specific 3D cases, preserves existing rendered case artwork, and uses the same presentation in list thumbnails and the room TV's favourite-game mode. A game without an image uses its console illustration. The public `v0.1.0-beta.1` download predates this feature.

## Console cases

The first collection checks focus on PS1, PS2 and Nintendo 64. PS1 prefers the existing `covers/3d` images, including their original transparent jewel-case edges, labels and special editions. Those complete images are displayed directly rather than placed inside a second case. PS2 front covers sit inside tall black plastic DVD cases for every region, as requested by the owner; matching `_spine` images from the emulator's cover folder supply the visible side. N64 uses a wide cardboard box for western releases and portrait packaging for Japanese releases.

Additional case profiles cover NES, SNES, Game Boy/Color/Advance, DS/3DS, GameCube, Wii/Wii U, Switch, PSP, PS3/4/5, Mega Drive/Master System, Sega CD/Saturn/Dreamcast, PC Engine and Atari. The profile controls the case proportions, depth and material independently of the source image. Region and original scan orientation resolve common packaging differences. Computer packaging follows the scan's own proportions; arcade artwork remains a poster. A generic spine title is used when separate original spine art is unavailable, so this is not a claim that every edition's spine is an exact scanned reproduction.

Case rendering adds perspective, distinct front/side/top faces, contact shadows and a soft floor reflection. Plastic cases have a rounded solid shell; surface normals control the small glossy highlights while PS2's flat plastic remains deep black. This replaces the earlier flat gray rim. Large cases are drawn at twice the display resolution into a bounded two-entry texture cache for smooth edges and inexpensive subsequent frames. Actual app icons and screenshots keep their original proportions until a real retail front cover is available.

## Existing emulator artwork

isShelf finds images when it scans the library. Games, emulator folders and saves stay in place.

| Source | Matching rule |
| --- | --- |
| PSXS5 | The exact game path in `library.txt` supplies its serial. Complete case images in `covers/3d` take priority, followed by front images in `covers/default` or `covers/boxart`. Filename-matched covers work too. Conflicting serial rows are ignored. |
| PS5SX2 | An exact game path and matching file size in `chd-serials.txt` supply the serial used in `/data/PCSX2/covers`. A complete case in `covers/3d` takes priority if available. Otherwise, the front and optional `_spine` images become distinct case faces; back/spine images are never substituted for the front. |
| PSSwanStation / SwanStationPS5 | Existing `covers` folders, matched by game filename or a known serial. |
| RetroArch | The system's `thumbnails` database folder, using `Named_Boxarts`, then `Named_Titles` or `Named_Snaps`. Matching uses the game filename or displayed name. |
| PS3 | The discovered game's `PS3_GAME/ICON0.PNG`. |
| PS4 / PS5 | The registered game's `sce_sys/icon0.png`. |
| A game's own folder | `cover.png`, `cover.jpg`, `front.png` or `front.jpg`, including upper-case extensions. |

PNG and JPEG sources are supported. These images help identify games; they do not establish that a game or emulator is compatible.

## Cache and privacy

The app reads artwork through the console's existing ftpsrv and keeps resized PNG copies inside `/data/console-shelf/covers`. The cache uses a stable key derived from each game's system and path, with separate `.spine.png` and `.spine.tag` entries for original spine artwork. Its private `index.json` records the artwork source, presentation kind, cover readiness and spine readiness. Artwork is read in the background discovery worker, and unchanged cache entries are reused on the next opening. Spine transfers share the same per-opening budget as covers.

Transfers are limited to known artwork locations, 8 MiB per source image and 64 MiB per opening. Decoding is bounded to 16 million pixels, and cached images have a maximum side of 960 pixels. Very large collections can fill the cache over several openings. Damaged or oversized art leaves the game available with its console illustration. Cancellation still interrupts the FTP waits.

The artwork reader uses only the two named game-to-serial indexes, public game metadata, and image files. It does not read emulator account settings. Cover files from an owner's console are personal cache data, not public release assets. The app does not automatically download covers from the Internet.

## Owner's collection check

The 26 October 2026 check found 92 library entries. Eighty images came from the existing PS1, PS2, PS3 and native PS5 libraries. Ten matching N64 front covers from the [Libretro thumbnail collection](https://github.com/libretro-thumbnails/Nintendo_-_Nintendo_64) were added to the owner's private cache. The two GameShark utilities had no exact image in the checked sources and use the PS1 fallback.

The owner's startup check of build `00.100.025` failed with `CE-108255-1`. Console diagnostics identified a stack overflow in the artwork serial-index reader. Build `00.100.026` moves that bounded buffer to the heap, gives the discovery worker an explicit 256 KiB stack, and rejects scan functions with stack frames larger than 16 KiB during PS5 compilation. A 64 KiB worker regression reproduces the old failure and passes with the fix, including maximum-size, oversized and short-read indexes.

The replacement build is installed and its five changed program records were read back. Settings are unchanged byte for byte; the private intro and artwork folder listings are unchanged. On 27 October 2026, the owner confirmed that isShelf opens and stays responsive, and that selecting Castlevania: Symphony of the Night launches it successfully through PSSwanStation. Fresh console logs confirm a complete scan of 92 games, 90 cached covers, no unreadable artwork, and a foreground handoff to `PPSA99248`. Artwork appearance still awaits a visual check. [Artwork record](verification/emulator-artwork-2026-10-26.json); [startup fix and hardware record](verification/ui26-startup-stack-fix-2026-10-27.json).
