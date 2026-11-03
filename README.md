# isShelf

<p align="center">
  <a href="https://ko-fi.com/iswebro">
    <img src="assets/support/isshelf-support-banner.png" alt="A warm gaming room with a walnut console shelf and a welcoming coffee cup" />
  </a>
</p>

> **☕ Buy Iswebro a Coffee! Only if you’d like to.**
>
> If isShelf is useful to you, you can [support the next update on Ko-fi](https://ko-fi.com/iswebro). Tips help with development, testing, documentation and release work. There’s no pressure: sharing the project, trying it out and sending feedback help just as much.

<p align="center">
  <a href="https://ko-fi.com/iswebro">
    <img src="assets/support/isshelf-tip-decoration.png" width="180" alt="A coffee cup and game controller with a glowing heart" />
  </a>
</p>

**Your consoles. Your games. Your play room.** isShelf by **Iswebro** brings your existing PS5 homebrew libraries together in Grid, Swipe and a customisable Shelf room.

A native C++/SDL app, tested on PS5 firmware **12.20**, with a separate [browser design preview](index.html). Choose your preferred installed emulator, keep your games in their existing folders and make the room your own. The catalogue has 52 system categories; a console illustration does **not** mean an emulator is available for it.

![isShelf running on PS5: PlayStation consoles arranged on deep shelves, with a personal video on the room TV](docs/images/ps5/shelf-room.webp)

*Actual PS5 capture, 26 October 2026. Manufacturer startup videos visible on the TV are personal overrides, supplied separately. isShelf includes its own original intro animations.* [More PS5 screenshots and a short recording](docs/SCREENSHOTS.md).

## Download for PS5

| File | Purpose |
| --- | --- |
| [PPSA99541.zip](https://github.com/Iswebro/console-shelf-ps5/releases/download/v0.1.1/PPSA99541.zip) | Installable PS5 app. |
| [Complete source and relink kit](https://github.com/Iswebro/console-shelf-ps5/releases/download/v0.1.1/isShelf-v0.1.1-Source-and-Relink.zip) | Matching source, objects, libraries and build instructions. |
| [SHA256SUMS.txt](https://github.com/Iswebro/console-shelf-ps5/releases/download/v0.1.1/SHA256SUMS.txt) | Full SHA-256 values for every payload. |

Follow the [installation, recovery and removal guide](docs/PS5-INSTALL.md). Download [MANIFEST.json](https://github.com/Iswebro/console-shelf-ps5/releases/download/v0.1.1/MANIFEST.json) for per-file verification and [HARDWARE-CHECKS.json](https://github.com/Iswebro/console-shelf-ps5/releases/download/v0.1.1/HARDWARE-CHECKS.json) for tested scope. Stable **v0.1.1** is verified on **firmware 12.20** with the existing homebrew enabler, ShadowMountPlus, ELF loader and FTP service. No PC host is needed to run it. Emulators and games are supplied separately.

isShelf is also [listed on ProsperoStore](https://prosperostore.net/app/PPSA99541/). See [store submission status](docs/STORE-LISTINGS.md) for the other destinations.

## New to isShelf? Start here

| I want to… | Guide |
| --- | --- |
| Get started, learn the controls and choose an emulator | [Beginner's guide](docs/BEGINNERS.md) |
| Put my own videos or pictures on the room TV | [Personal TV media, step by step](docs/PERSONAL-TV.md) |
| Choose consoles, arrange a shelf, change logos or set the clock | [Room and customisation guide](docs/ROOM.md) |
| Install, update, back up or remove isShelf | [Installation and recovery](docs/PS5-INSTALL.md) |
| Find missing games or understand a launch message | [Troubleshooting](docs/BEGINNERS.md#troubleshooting) |

## Make it yours

- Browse 52 systems in Grid, a curved card-free Swipe carousel, or a lit display Shelf. Shelf places the console illustrations directly on deep walnut counters in one continuous gaming-lounge scene, with a subtle selection light. It pages through the collection instead of stretching a shelf photograph into each row. All layouts share sort, visibility and favourite choices. Grid can also use classic text cards.
- In Shelf, **L1/R1** (or Page Up/Down) swaps eight consoles at a time while the room stays still. **Sort & filter → Shelf pages** can instead show one curated display of up to eight consoles chosen in **Consoles**. All consoles, emulator/game availability, favourites, search and sorting apply to the collection. Small preview screens fit fewer systems per page.
- **Play room** settings customise the TV with selected-console artwork, original intro pictures and silent animations for all 52 systems, a personal PNG, a clock and Shelf logo, a personal message, or rotating favourite games. Personal intro clips and pictures take priority over the matching Shelf original. Mark a game with Triangle in the native library, or its star in the preview. TV content is clipped to the photographed screen with subtle glass reflections; the search bar ends before it. Intros can apply to installed emulators or all systems, and optional fine-grain static can mark missing emulators or games. [Room controls and personal media](docs/ROOM.md).
- Missing emulators use black-and-white console artwork by default. Appearance → Missing emulator artwork can keep everything in full colour instead. This does not hide systems or change launch support; the browser uses its dated demo inventory, while the native app uses discovery.
- In Swipe, the selected console sits forward in the centre; neighbours recede in size, depth and angle. Motion eases to a stop in 420 ms and retargets immediately when direction changes.
- Loop through Swipe using the stick/D-pad on a standard-mapped controller, keyboard arrows, mouse dragging or touch. Click a side console to centre it, then select the centre console to explore. The browser honours reduced-motion preferences and remembers the selected system. The final native navigation, settings, persistence and restart checks passed on firmware 12.20. Automated controller checks are simulated.
- Switch between **All consoles**, **Emulator or games**, and **Favourites**. Availability uses recorded emulator routes, built-in PS5/PS4 routes, or example games in the preview; the native app uses real discovery. Choices persist on the device.
- Search, filter by brand or evidence level, sort in several ways, and save favorites.
- Open the settings gear for Appearance, Sort & filter, Consoles and Play room. Search the console picker, toggle individual systems or whole brands, and show/hide search results in one click. These choices persist on the device.
- Open a console detail page, choose a preferred detected emulator and browse its discovered games. The separate browser demo uses an example inventory and never launches PS5 software.
- Create or update all detected emulator shortcuts from inside isShelf, using the matching console artwork and saved destination. PS4 and PS5 stay as isShelf/library destinations because those games already live on PS5 Home. Remove isShelf-owned shortcuts in one batch; the app explains that you must close isShelf so ShadowMount can refresh the Home screen.
- On an ordinary main-tile launch, check the official stable release feed and offer **Update now** or **Later**. Downloads resume after interruption, the staged program is hash-checked before installation, and the updater is structurally unable to replace settings, emulators, games, BIOS, saves or personal artwork.
- Press **L3 + R3** together to opt into bounded remote support; press the chord again or close isShelf to stop it. Stable builds expose menu navigation and isShelf-rendered frames only. Private probes and emulator launch control remain excluded.
- A persistent launch panel explains the action before selection: [Open emulator library](previews/launch-library-only.png) for menu-only routes, or [Play game](previews/launch-direct-play.png) for a direct RetroArch route. It follows the preferred emulator and stays on screen while browsing.
- Choose Vector, Gaming, Classic mint or Jade for the app logo. Vector is the default. The selected logo also appears on the room's clock/message screen and bundled intros; personal videos keep their own contents.

See the [mobile Swipe view](previews/carousel-mobile.png) and [Switch 2 icon](previews/switch2-3d-card.png). Nintendo's press photo informed the Switch 2 illustration but is **not** displayed or bundled.

## Inventory and compatibility

The [demo system catalogue](docs/SYSTEMS.md) comes from a dated PS5 setup snapshot. Its 46 RetroArch core mappings and example game names are **not** a live scan of another user's console. A mapped core or listed emulator does not guarantee a game runs. In the browser demo, “Emulator or games” shows 51 of 52 cards; Switch 2 is the hardware-reference-only card and has no launch route. The native app instead starts empty and can use its [read-only scanner](docs/DISCOVERY.md) for the owner's actual inventory.

The [stable native PS5 release](https://github.com/Iswebro/console-shelf-ps5/releases/tag/v0.1.1) renders the three layouts, discovers existing libraries, saves preferences, manages isShelf-owned Home shortcuts and explains each launch route before selection. Its public build retains physically enabled L3+R3 menu support and frame preview while excluding the development-only probes and remote emulator-launch extension. Hardware scope and exact download hashes are supplied with the release; emulator and native-title routes not listed there remain experimental.

![Nintendo systems in isShelf on a real PS5](docs/images/ps5/shelf-nintendo.webp)

![Choose which consoles to show individually in the native PS5 app](docs/images/ps5/settings-consoles.webp)

These are native PS5 captures. [The screenshot gallery](docs/SCREENSHOTS.md) also shows Appearance, Sort & filter and Play room settings. PC development previews remain under `previews/` and are labelled separately.

## PS5 home-screen artwork

[Glass and Light](assets/backgrounds/shelf-ps5-home-v2.png) is the new 3840 × 2160 selection background. A much larger Shelf mark sits directly over sculptural dark glass and restrained mint/cyan lighting, with a quiet left side and upper strip for the PS5 interface. The [logo is real SVG geometry with no background](assets/branding/shelf-gaming-mark.svg); a [transparent PNG](assets/branding/shelf-gaming-mark.png) is also included. The published app uses the Vector home tile; Gaming, Classic mint and Jade remain available as in-app choices.

The previous [static composition](assets/backgrounds/shelf-ps5-home-v1.png) is retained. The [prompt and composition notes](assets/backgrounds/shelf-ps5-home-v2.prompt.txt) record built-in image generation for the 1672 × 941 glass plate and direct 4K vector composition. Run `node tools/build-home-art.js` to reproduce the composition with Playwright and an available browser. The PS5-format `pic0.dds` and `pic1.dds` are included in the published app. The owner’s console captures confirm the Vector home tile and Glass and Light background.

## Development check

The preview itself needs no build step. With Node.js and npm, run `npm ci` and `npm test` for the browser interaction and icon-integrity check. The test uses an installed Microsoft Edge when available; otherwise install Playwright's Chromium with `npx playwright install chromium`. Tests leave the checked-in screenshots alone; set `SHELF_CAPTURE_PREVIEWS=1` to regenerate them intentionally. Run `npm run test:native` for the native model using an existing Visual Studio C++ toolset on Windows, or a C++17 compiler on Linux. See [native build notes](native/README.md).

## Credits and license

The application source in this repository is [GPL-3.0](LICENSE). The generated icons have separate [image credits and reuse terms](assets/CREDITS.md). [Third-party project credits](THIRD_PARTY.md) distinguish incorporated GPL platform/build source, rendering dependencies and emulator references. No emulator binaries, games, ROMs, BIOS, keys, proprietary firmware/runtime files or user credentials are included. Console and game names are used for identification; this project is not affiliated with their manufacturers.
