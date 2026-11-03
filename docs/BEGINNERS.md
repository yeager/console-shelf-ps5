# Start using isShelf

isShelf is a front door to the games and emulators already on your PS5. It organises the collection and opens the available launch route. It does not include or install games, emulators, BIOS files or a jailbreak.

This guide covers stable **v0.1.0**, tested on firmware **12.20**. Keep your working firmware and decline system software update prompts.

## 1. Install the app once

Use the [installation guide](PS5-INSTALL.md) if isShelf is not on your PS5 yet. Download **PPSA99541.zip** from the [stable release page](https://github.com/Iswebro/console-shelf-ps5/releases/tag/v0.1.0). The larger **Source-and-Relink.zip** is for source/tools; it is not the app to install.

You need an existing homebrew setup with ShadowMountPlus, the console's ELF loader on port **9021** and `ftpsrv` on **2121**. isShelf needs FTP for discovery even when running on the console. It does not start those services for you. Its own launch helper is included: there is no extra isShelf payload to add to autoload.

If isShelf is installed already, close any running game or download, use your normal loader workflow and open **isShelf**. Leave the existing FTP service available. A PC does not have to stay on to browse or launch games from isShelf.

## 2. Learn the controls

| Button | What it does |
| --- | --- |
| D-pad or left stick | Move between consoles, games and controls |
| Cross / X | Open or select the highlighted item |
| Circle | Go back; a tap on the home page shows a brief closing hint |
| Hold Circle for 3 seconds | Close isShelf from its home page with a fade |
| Square | Cycle **Grid → Swipe → Shelf** from the home page |
| Options or the gear | Open Settings |
| L1 / R1 in Shelf | Previous / next page of consoles; the room stays still |
| L1 / R1 in Settings | Change settings tab |
| Left / Right on a setting | Change its value; changes save automatically |
| Triangle on a console or game | Add/remove a favourite |

In Shelf layout, Up/Down moves between the two rows. Up from the top row reaches the toolbar; Up again reaches Settings. You do not need to page back to the first shelf.

## 3. Show the consoles you want

Press **Options**, then use **L1/R1** to reach **Consoles**.

- **All consoles** shows the catalogue, including systems without an emulator.
- **Emulator or games** focuses on systems where discovery found an emulator route or game.
- **Favourites only** shows consoles you starred with Triangle.
- Use the individual switches below to show/hide each console. Search by console name or brand to find one quickly.

![Individual console visibility switches on PS5](images/ps5/settings-consoles.webp)

These choices combine with the search, brand and availability filters. A missing console may simply be filtered out. To see the full catalogue again, choose All consoles, Show all, clear the search and reset the brand/availability filters.

Systems without a detected emulator use black-and-white artwork by default. Change **Appearance → Missing emulator artwork → Full colour** if you prefer. Colour is a visual choice, not a compatibility guarantee.

For one display of your favourites, choose **Sort & filter → Shelf pages → One curated shelf**, then select up to eight systems in **Consoles**. Choose **Browse collection** to bring paging back.

## 4. Choose an emulator and open a game

1. Highlight a console and press Cross.
2. Highlight **Preferred emulator**. Press Left/Right to choose a detected route. The choice is saved.
3. Choose **Show game library**.
4. Highlight a game and read the launch panel **before pressing Cross**.

| Launch panel | What happens |
| --- | --- |
| **Direct play / Play game** | isShelf asks the selected RetroArch core to open that game. |
| **Library only / Open emulator library** | The emulator opens its own shelf. Choose your game there. |
| Unavailable / no compatible route | The game can be listed, but isShelf has no usable launch route for that selection. |

For example, PSXS5 opens its own PlayStation library. With RetroArch/SwanStation selected instead, the PS1 direct-game route can open the selected file. The owner has tested Final Fantasy Tactics and CTR through that route. PS5SX2 and PS5SX3 currently open their own emulator libraries.

Close the previous emulator before starting another through isShelf. A discovered game or installed core is not a promise that every game is compatible. [Tested launch scope](INTEGRATION.md).

## 5. Add or remove PS5 Home shortcuts

Wait for isShelf's library scan to finish, open any console, then select **Add to PS5 Home Screen** (or **Edit PS5 Home Screen shortcut** if it already exists).

- **isShelf game library** opens that console's games inside isShelf.
- **Direct emulator menu** opens a supported standalone emulator with that console selected. Shared RetroArch systems use the isShelf library because RetroArch cannot select a console without a game.
- **isShelf home** opens isShelf with that console selected.
- PS4 and PS5 shortcuts always open in isShelf; they cannot be set to open an emulator.

Choose **Add or repair all emulator tiles** to create every shortcut supported by the completed scan. When it finishes, select **Close isShelf and update PS5 Home**. ShadowMount applies the queued registrations only after isShelf closes.

To clean up, open the same screen and choose **Remove this tile** or **Remove all isShelf tiles**. The app queues only shortcuts carrying isShelf's exact ownership record. Follow the on-screen **Close isShelf** action so ShadowMount can refresh Home. If a shortcut remains, reopen isShelf and use the verification/cleanup action; recovery data is deliberately kept until removal is confirmed.

### Return to an emulator's menu

Once a game is running, its controls belong to the emulator. isShelf cannot impose one menu shortcut across them. Use that emulator's configured Quick Menu/menu combination. For example, the owner's RetroArch setup uses Options + touchpad click; installing isShelf does not configure that combination on someone else's console.

## 6. Add more games

Install the game into its emulator's normal library first and confirm it works **inside the emulator**. Then close the emulator and reopen isShelf with `ftpsrv` available. isShelf rescans at startup; no second copy is needed just for isShelf.

Existing locations recognised by this release include:

| Library | Existing PS5 folder |
| --- | --- |
| PSXS5 | `/data/PSXS5/games` |
| SwanStationPS5 | `/data/SwanStationPS5/games` |
| PS5SX2 | `/data/PCSX2/games` |
| PS5SX3 | `/data/homebrew/PPSA99303/rpcs3/games` |
| RetroArch | Supported per-system folders inside that app's `content/` folder; the installed app location can vary |

Keep companion files together: a PS1 CUE needs its referenced tracks, and a multi-disc M3U needs its referenced discs. ZIPs, BIOS, keys, emulator dependencies and unfinished downloads are not games to import into isShelf. Some emulator formats and custom folders are not yet supported. [Discovery details and limits](DISCOVERY.md).

## 7. Personalise the room

Press Square until you reach **Shelf**, then open **Settings → Play room**. Try the clock, your name/message, rotating favourite games, or the bundled original intro videos.

![Play room settings on PS5](images/ps5/settings-play-room.webp)

Want your own startup videos? Follow [Personal TV media, step by step](PERSONAL-TV.md). It explains conversion, the exact FTP folder and common mistakes. [The room guide](ROOM.md) covers all display modes and the optional static screen.

## Troubleshooting

| What you see | First things to check |
| --- | --- |
| Empty library or incomplete scan | Start your existing `ftpsrv`, then close/reopen isShelf. Test that the emulator itself can see the games. |
| One console is missing | Clear search/brand/availability filters; check its visibility switch and whether One curated shelf is enabled. |
| Random dependencies or a missing ZIP | Use the emulator's real game folder and supported unpacked format. isShelf is not a general file browser or archive extractor. |
| Emulator opens, but not the selected game | Read the persistent launch panel. **Library only** is the current behaviour for standalone PSXS5/PS5SX2/PS5SX3. |
| A new or removed Home shortcut is not visible yet | Use the screen's **Close isShelf** action and wait for ShadowMount to finish refreshing PS5 Home. |
| Game also fails when opened inside the emulator | Check the dump, BIOS/companion files and emulator compatibility. A different isShelf setting cannot fix that game. |
| Launch does nothing | Close other apps; check your existing ELF loader is available. Preserve the launch logs before another attempt. |
| Opening background never reaches the controls | Hold Circle to close if input still works, then reopen after confirming FTP/ShadowMount services are ready. Preserve the startup log before retrying. |
| Personal TV video does not appear | [TV troubleshooting](PERSONAL-TV.md#troubleshooting): check the mode, filename, format, intro scope, static and motion settings. |

For a launch bug, copy `/data/console-shelf/startup.log`, `launch-helper.log` and `launch-status.txt` to your PC **before opening isShelf again**. Include the isShelf version, PS5 firmware, selected emulator/game and exact message when reporting. Review logs and captures for personal information before sharing; remove IP addresses, credentials and anything private. Do not attach games, saves, BIOS, keys or firmware files.

Back up `/data/console-shelf` before changing personal media or reinstalling. It contains isShelf preferences, favourites and personal TV files. Your emulators' saves remain in their own folders. [Upgrade/recovery instructions](PS5-INSTALL.md#upgrading-and-recovery).

The app is called **isShelf**, but its data folder deliberately keeps the older `console-shelf` name so existing settings still work. Keep that folder name when following these guides.
