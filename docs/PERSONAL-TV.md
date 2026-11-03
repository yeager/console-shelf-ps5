# Put your own videos on the TV

Your room's TV can play a short personal video each time you select a console. The file lives on the PS5: no PC streaming server is needed afterward.

This guide applies to the native **v0.1.0** app. The separate browser preview has its own file pickers; choosing a file there does not upload it to the PS5.

## Try the included intros first

1. Open isShelf and press **Square** until you reach **Shelf** layout. The TV belongs to this room; Grid and Swipe do not show it.
2. Open **Options → Play room** using L1/R1 to change tabs.
3. Set **TV display → Console intro video**.
4. Choose **Console intros → Installed emulators and built-in systems**, or **All consoles** if you also want intros for systems without an emulator.
5. Press Circle to leave Settings, then select a different console.

![Where to choose TV display, intro scope and static on PS5](images/ps5/settings-play-room.webp)

All 52 catalogue systems have an original, silent isShelf animation. They are not the manufacturers' startup videos. The manufacturer's intros visible in some [owner screenshots](SCREENSHOTS.md) are personal overrides and are not included in the app or source download.

## What you need for a personal video

- A local video you have permission to use, such as `C:\My clips\my-ps2-intro.mp4`.
- A Windows PC with [Python 3.11 or newer](https://www.python.org/downloads/windows/).
- A portable Windows FFmpeg build linked from [FFmpeg's download page](https://ffmpeg.org/download.html). Keep **ffmpeg.exe** and **ffprobe.exe** together in its `bin` folder.
- The **Source-and-Relink.zip** from the [same isShelf release](https://github.com/Iswebro/console-shelf-ps5/releases/tag/v0.1.0), extracted on the PC. It contains `tools/prepare-intro.py` and needs no compiler for this task.
- Your existing local FTP connection to the PS5. [FileZilla Client](https://filezilla-project.org/) is one option; its [connection/transfer guide](https://wiki.filezilla-project.org/Using) explains the two panes.

The converter works from a local file and does not download YouTube videos, contact the PS5 or modify the original. You can use a short portion of your own longer recording with `--start` and `--seconds`.

## 1. Convert a copy on your PC

1. Extract the source ZIP. Open its **isShelf** folder: it should contain a `tools` folder.
2. Open PowerShell in that folder, or use `Set-Location -LiteralPath 'C:\isShelf-Source\isShelf'`, replacing that example with your extracted folder.
3. Run `python --version`. If Python is installed under `py` instead, use `py` in the commands below.
4. Paste this example, replacing the two file paths with your own:

```powershell
python .\tools\prepare-intro.py "C:\My clips\my-ps2-intro.mp4" --console ps2 --start 0 --seconds 10 --ffmpeg "C:\Tools\ffmpeg\bin\ffmpeg.exe"
```

`--console ps2` names the console. `--start 0` begins at the start of the source; `--start 35` would begin 35 seconds in. `--seconds 10` keeps ten seconds. Start with a short clip; the converter allows up to 60 seconds.

When it finishes, find the new folder printed after **prepared**. By default it is inside the source folder's `.cache/room-intros/`. Copy that printed path into File Explorer's address bar. The folder contains:

| File | Use |
| --- | --- |
| **ps2.mpg** | The converted video to copy onto the PS5 |
| **ps2.mp4** | A version for the browser preview; do not use it as the native PS5 override |
| **manifest.json** | Sizes/hashes and the expected PS5 destination |

The recommended output is silent MPEG-1 at **640 × 360, 25 fps**. The app accepts bounded MPEG-1 program streams up to 640 × 368, 30 fps and 32 MiB. Renaming `.mp4` to `.mpg` does not convert a file. Existing output folders are refused, so your previous conversion is not overwritten.

## 2. Copy the MPEG onto your PS5

1. Close isShelf and any game/download. Start the PS5's existing **ftpsrv** in your usual Payload Manager.
2. Connect your FTP client to **your PS5's LAN IP**, port **2121**, using your existing connection details. Use your trusted home network. This is FTP, not SFTP. No router port forwarding is needed.
3. On the PC side, open the converter's output folder. On the PS5 side, open **`/data/console-shelf`**.
4. Open its **`intros`** folder. If it is missing, create that one folder inside `/data/console-shelf`.
5. If `ps2.mpg` is already there, download a backup to your PC before replacing your personal clip.
6. Upload **ps2.mpg** and wait until the transfer completes. Confirm the remote filename and byte count match the prepared file/manifest.

For this example, the full PS5 path must be:

```text
/data/console-shelf/intros/ps2.mpg
```

Use the lowercase console ID as the filename, not its display name. For another system, replace `ps2` in both the conversion command and destination:

| Console | ID | Upload filename |
| --- | --- | --- |
| PlayStation 1 | `ps1` | `ps1.mpg` |
| PlayStation 2 | `ps2` | `ps2.mpg` |
| PlayStation 3 | `ps3` | `ps3.mpg` |
| Nintendo 64 | `n64` | `n64.mpg` |
| Super Nintendo | `snes` | `snes.mpg` |
| GameCube | `gamecube` | `gamecube.mpg` |
| Dreamcast | `dreamcast` | `dreamcast.mpg` |
| Mega Drive / Genesis | `genesis` | `genesis.mpg` |
| Nintendo Switch | `switch` | `switch.mpg` |
| Nintendo Switch 2 | `switch2` | `switch2.mpg` |

[Full console ID list](CONSOLE-IDS.md). An ID names artwork/TV media; it does not promise an emulator for that system.

These are **personal data** paths. Leave the app's `PPSA99541` program files and bundled `assets/room/intros` alone. Keeping overrides under `/data/console-shelf` lets ordinary program upgrades preserve them.

## 3. See it on the TV

Reopen isShelf, choose Shelf layout and **Play room → TV display → Console intro video**. Select your console. A valid personal `.mpg` takes priority over its bundled animation. It plays once per console selection, without audio. Select another console and come back to replay it.

To return to the bundled animation, close isShelf and move your personal override back to the PC, keeping a backup. Reopen the app. You do not need to reinstall isShelf.

## Pictures work too

For a personal picture, use a PNG up to **2048 × 2048 pixels**, preferably below **8 MB**.

| Desired result | PS5 file | TV display setting |
| --- | --- | --- |
| The same picture for the room | `/data/console-shelf/tv-image.png` | **My picture** |
| A different still for one console | `/data/console-shelf/intros/ps2.png` (PS2 example) | **Console intro picture** |

Close isShelf before copying/replacing files. A picture override and video override are selected by different TV modes. The general picture is cropped to fill the TV; intro videos keep their aspect ratio. [All room options](ROOM.md).

## Troubleshooting

| Problem | Check |
| --- | --- |
| No TV is visible | Use **Shelf** layout. |
| isShelf's own animation plays | Check the exact lowercase ID, `.mpg` extension, folder and completed upload. Run the converter instead of renaming an MP4. |
| Only a picture or clock appears | Change **TV display** to **Console intro video**. |
| The TV shows static | Set **TV static for missing content → Off** for the test. Static deliberately overrides the selected display when its condition matches. |
| Intros work for some systems only | Change **Console intros → All consoles**, or install/use a supported emulator route. The intro-scope option does not create emulator support. |
| Video stays on one frame | **Appearance → Motion → Reduced** freezes intro motion. Choose smooth motion to play it. |
| Video does not restart | Move to a different console and return; entering the same console's library does not replay it. Reopen isShelf after replacing a file. |
| There is no sound | Native TV intros are currently silent by design. |
| Converter cannot find Python/FFmpeg | Check `python --version` (or `py --version`), the quoted `--ffmpeg` path and that `ffprobe.exe` is beside `ffmpeg.exe`. |
| Converter says its output exists | Choose a new `--out` directory, or let a later run create a new timestamped folder. Keep your existing output. |

Back up `/data/console-shelf` with your preferences and personal media. Public isShelf releases do not collect or redistribute these files.
