# Installing isShelf

Native PS5 stable release v0.1.0, tested on firmware **12.20**. The app appears as **isShelf**.

For a guided first session, see [Start using isShelf](BEGINNERS.md). To add personal intro clips after installation, use the [step-by-step TV tutorial](PERSONAL-TV.md).

## What you need

- An existing working PS5 homebrew setup. The current hardware target is firmware **12.20**.
- Your usual enabler and ShadowMountPlus, plus the existing ELF loader on console-local port 9021.
- The console's existing `ftpsrv` on port 2121. Shelf currently uses it for library discovery, including over loopback. Keep it available while using Shelf, on a trusted local network.

Installing Shelf does not set up a jailbreak, update firmware, change DNS/firewall settings or edit your payload autoload list. It reads the emulator libraries where they already are.

Shelf includes its own small launcher helper. When you select a launch, the existing ELF loader runs that helper; it continues briefly after Shelf closes and asks the system to open the selected app. You do not manually load another Shelf payload or add one to autoload.

The normal, tested **standard-sdk** build retains the SDK's helper-process capability and syscall-bound initialization in RAM and is required for emulator launching. Read the manifest's runtime notice before installing it. The explicitly selected restricted diagnostic build keeps the loader's permissions, but its running-app query is refused on the owner's console and it must not be installed as a launchable Shelf build. Neither variant contains a firmware updater.

## First installation

Keep firmware 12.20 and decline any system software update prompt.

1. Download `PPSA99541.zip`, `MANIFEST.json` and `SHA256SUMS.txt` from the same [isShelf release](https://github.com/Iswebro/console-shelf-ps5/releases). Choose the version named at the top of this guide. On Windows, use `Get-FileHash -Algorithm SHA256 -LiteralPath 'C:\path\to\PPSA99541.zip'` and compare the complete value with `SHA256SUMS.txt`.
2. Extract the ZIP on your PC. The app folder is `PPSA99541`; keep all its subfolders together. You do not need Node.js, a compiler or the PC preview to use the installed app.
3. Close games, emulators and downloads on the PS5. Start your existing FTP service and connect using your established local FTP client.
4. Check both `/data/homebrew/PPSA99541` and `/user/app/PPSA99541` for an existing title. This is an unofficial ID, not a globally reserved one. If it belongs to another app or you cannot identify it, stop. For an existing Shelf installation, use the upgrade steps below.
5. Copy the complete `PPSA99541` folder into `/data/homebrew/`. Compare the copied files with the included size/SHA-256 manifest. For a raw readback of `eboot.bin`, first send `SELF` and confirm the FTP reply explicitly says conversion is **disabled**; otherwise the server can transform the downloaded file. Do not open Shelf during the transfer.
6. Let your existing ShadowMountPlus workflow discover the folder. If necessary, run ShadowMountPlus once through your usual Payload Manager dashboard and wait for its library-synchronised message. Open **isShelf**.

Shelf itself needs no PC host afterward; it uses the services on the console. A console illustration in All consoles does not mean an emulator exists for it; installed routes and real game counts come from discovery.

## Controls and first check

Browse with the D-pad or left stick; Cross selects and Circle goes back. Square cycles **Grid → Swipe → Shelf**. Shelf places eight systems on the living room's two deep counters. Up/Down moves between the two rows on the current page; Up from the top row reaches the toolbar, then Settings. Options or the gear also opens settings. A short Circle tap on Shelf's home page shows a small close hint; hold Circle for three seconds to close with a fade.

In Shelf, **L1/R1** changes pages with only the consoles moving. **Sort & filter → Shelf pages** also offers one curated display of up to eight consoles; choose its members in **Consoles**. There is a Favourites-only view and an optional Reduced motion setting. **Play room** controls the TV's pictures, clock, message, intro scope, favourite-game rotation and optional static for missing content. All 52 systems include an original Shelf picture and silent intro animation; your personal PNG or compatible MPEG takes priority. Triangle on a game in Shelf's library stars it. See [personal room setup](ROOM.md) for media paths and current limitations.

Appearance → Missing emulator artwork defaults to **Black & white** for systems without a detected emulator. Choose **Full colour** to turn that cue off. Native PS4/PS5 stay coloured. This setting changes the artwork, not which systems appear. Layout, sorting, favourites and individual console visibility save in Shelf's own data folder.

Open a console to choose a preferred detected emulator or show its game library. Standalone emulators can open their own shelves; a selected RetroArch game uses a separate direct-game route. Use the release's verified-route list to distinguish accepted behavior from experimental plans. Opening can still show black before the grid; the home-screen selection artwork is confirmed.

Highlight **Preferred emulator** and use Left/Right (or Cross) to cycle the detected choices. This selection saves in Shelf. For example, PlayStation 1 can offer PSXS5's own shelf and a RetroArch/SwanStation game route when both apps are detected. Both emulators can use the existing PS1 files: PSXS5 opens its own shelf, while the RetroArch game route receives the selected file. An emulator's own playlist is separate from Shelf's discovered library; switching the preference does not move games or convert saved data.

The release also detects the dedicated PSSwanStation app (`PPSA99248`, build 25). To show an existing PSXS5 collection in PSSwanStation's Network tab, add `path = ftp://127.0.0.1:2121/data/PSXS5/games` to that app's `network.cfg`, then reopen it or choose **Scan for games**. This reads the same files through the PS5's existing ftpsrv; the games stay in place. Choose **PSSwanStation** in isShelf's PS1 preferred-emulator control to use its selected-game launch.

Before selecting a game, read its launch panel. **Library only / Open emulator library** means you will choose the game inside that emulator after it opens. **Direct play / Play game** opens the selected game through RetroArch. This explanation stays visible while browsing; the Cross action in the footer matches it.

## PS5 Home shortcuts

After the library scan completes, open a console and choose **Add to PS5 Home Screen**. You can open its isShelf library, a supported standalone emulator menu, or isShelf with that console selected. PS4 and PS5 remain isShelf-only destinations. **Add or repair all emulator tiles** prepares every supported emulator shortcut in one batch.

Registrations and removals are applied after isShelf closes. Always use the on-screen **Close isShelf and update PS5 Home** action when the batch finishes. To clean up, choose **Remove all isShelf tiles**; isShelf queues only exact-owned shortcuts and tells ShadowMount to refresh after closure. Unknown or mismatched titles are left alone.

## Upgrading and recovery

Close Shelf and other apps before replacing files. Keep a PC backup of its program folder and `/data/console-shelf`, where its preferences live. When reading or backing up `eboot.bin` through `ftpsrv`, send the custom `SELF` command and confirm the reply explicitly says conversion is **disabled**; otherwise the server can transform the file during transfer and its hash will differ. This setting is per FTP connection.

Replace only Shelf's owned program files, putting `eboot.bin` last, and verify the complete new file manifest before reopening. Preserve `/data/console-shelf/settings.ini` and its recovery files. An ordinary program update does not require re-registering the app's artwork.

If a launch fails, preserve `/data/console-shelf/startup.log`, `launch-helper.log` and `launch-status.txt` before the next attempt. These are private diagnostics; review them before sharing. Restore the previous verified program folder to roll back. Other emulator folders, BIOS, games, saves and account settings are separate.

To remove Shelf, close it, retain your backup and use your established ShadowMountPlus homebrew-removal procedure for **PPSA99541 only**, including its owned source folder. Keep `/data/console-shelf` if you want its preferences for a future installation. Do not remove another title or a shared emulator/game directory.
