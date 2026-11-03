# SPDX-License-Identifier: GPL-3.0-only
"""Assemble a native hardware-test candidate. Never connects to, installs on or launches a PS5."""
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import zipfile

from ps5_common import CACHE, ROOT, run, sha256
from console_badges.stage_generic_tile_pack_asset import stage as stage_generic_tile_pack_asset
from fetch_ps5_deps import prepare_update_ca
from verify_ps5_build import require, source_fingerprint, verify_recorded_hashes
from verify_ps5_presentation import verify as verify_presentation


def root_git(*arguments: str) -> str:
    """Run Git for ROOT even when a Windows worktree is packaged through WSL."""
    marker = ROOT / ".git"
    command = ["git"]
    if marker.is_file():
        match = re.fullmatch(r"gitdir:\s*(.+)\s*", marker.read_text(encoding="utf-8"))
        require(match is not None, "Invalid Git worktree pointer")
        raw = match.group(1).replace("\\", "/")
        if os.name != "nt" and re.match(r"^[A-Za-z]:/", raw):
            raw = "/mnt/" + raw[0].lower() + raw[2:]
        git_dir = Path(raw)
        if not git_dir.is_absolute():
            git_dir = (ROOT / git_dir).resolve()
        command.extend(["--git-dir", str(git_dir), "--work-tree", str(ROOT)])
    return subprocess.check_output([*command, *arguments], cwd=ROOT, text=True)


def verify_compiled_console_tile_pins(executable: Path, bundle: Path) -> dict:
    """Bind the compiled manager to the exact catalog and forwarder it packages."""
    raw = executable.read_bytes()
    pins = {}
    for relative in ("catalog.json", "template/eboot.bin"):
        path = bundle / relative
        require(path.is_file(), "Console-tile bundle pin input is missing: " + relative)
        digest = sha256(path)
        require(digest.encode("ascii") in raw,
                "Compiled console-tile pin differs from packaged " + relative)
        pins[relative] = {"bytes": path.stat().st_size, "sha256": digest}
    return pins


def stage_tracked_console_tile_bundle(destination: Path) -> dict:
    """Copy the closed reviewed bundle without a hidden upstream checkout."""
    source = ROOT / "assets/console-tiles"
    require(source.is_dir() and not source.is_symlink(),
            "Tracked console-tile bundle is unavailable")
    require(not any(path.is_symlink() for path in source.rglob("*")),
            "Tracked console-tile bundle contains a symlink")
    catalog_path = source / "catalog.json"
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    require(set(catalog) == {"format", "forwarderCommit", "forwarderSource", "runtime",
                             "systems", "target", "templateSha256"} and
            catalog["format"] == "isshelf-console-tile-assets/1" and
            catalog["target"] == "PPSA99541" and
            catalog["runtime"] == "isshelf-cooperative-forwarder/3" and
            catalog["forwarderSource"] == "https://github.com/heydemoura/ps5-forwarder-format" and
            catalog["forwarderCommit"] == "22e8762ad7dbd01e18bd8c0e95f86461fdf8aaf4",
            "Tracked console-tile catalog identity differs")
    template_hashes = {
        "template/eboot.bin": "8e43160e64a4f3fe9f84a2a7cd4b9175772ff9897c3bd8a25870f32b0d936638",
        "template/param.json": "b70d9efe0c435f2c0e0497e7753de30dff205d7786f9ce47eb7664d3cb0835c3",
        "template/sce_module/libc.prx": "e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036",
    }
    require(catalog["templateSha256"] == template_hashes,
            "Tracked console-tile template pins differ")
    package_files = {"catalog.json", "LICENSE-forwarder.txt", *template_hashes}
    systems = catalog["systems"]
    require(isinstance(systems, list) and len(systems) == 52, "Expected 52 tracked console tiles")
    seen = set()
    for row in systems:
        require(set(row) == {"badge", "iconBytes", "iconSha256", "id", "name", "preferredTitleId"} and
                re.fullmatch(r"[a-z0-9]+", row["id"]) is not None and row["id"] not in seen and
                re.fullmatch(r"PPSA99[0-9]{3}", row["preferredTitleId"]) is not None,
                "Invalid tracked console-tile catalog row")
        seen.add(row["id"])
        relative = "icons/" + row["id"] + ".png"
        icon = source / relative
        require(icon.is_file() and icon.stat().st_size == row["iconBytes"] and
                sha256(icon) == row["iconSha256"], "Tracked console-tile icon differs: " + row["id"])
        package_files.add(relative)
    for relative, expected in template_hashes.items():
        path = source / relative
        require(path.is_file() and sha256(path) == expected,
                "Tracked console-tile template differs: " + relative)
    require(sha256(source / "LICENSE-forwarder.txt") ==
            "230184f60bae2feaf244f10a8bac053c8ff33a183bcc365b4d8b876d2b7f4809",
            "Tracked console-tile license differs")
    source_only = {"isshelf-stage-v1.json", "isshelf-stage-v1.png",
                   "isshelf-stage-v2.json", "isshelf-stage-v2.png", "icons/manifest.json",
                   "pack-v1/index.json", "pack-v1/pack.bin"}
    actual = {path.relative_to(source).as_posix() for path in source.rglob("*") if path.is_file()}
    require(actual == package_files | source_only,
            "Tracked console-tile source inventory changed")
    require(not destination.exists(), "Refusing to replace an existing console-tile bundle")
    for relative in sorted(package_files):
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        with (source / relative).open("rb") as incoming, target.open("xb") as outgoing:
            shutil.copyfileobj(incoming, outgoing)
        require(target.read_bytes() == (source / relative).read_bytes(),
                "Staged console-tile file differs: " + relative)
    return {"trackedBaseFiles": len(package_files), "systems": len(systems)}


def main():
    build = CACHE / "build"
    verified = verify_recorded_hashes(build)
    bridge_notice = ("Private menu control is compiled in. A normal Home launch starts with control off; click L3 + R3 together after discovery to enable it, including while the opening background is still visible. "
                     "The separate one-shot private opener can request only isShelf (PPSA99541) with a fresh arm argument. If accepted at startup, control begins enabled. "
                     "The header shows control status; click L3 + R3 to stop control, or close the app. The opener does not grant arbitrary app launch or general PS5 control. "
                     "While armed, a silent 960x540 JPEG preview targets two frames per second over the existing FTP service. "
                     "Run tools/ui_test_viewer.py from the matching source on the PC to view it. This preview ends when isShelf closes. "
                     "See docs/PRIVATE-UI-TEST.md in the matching source. This executable cannot be publicly released."
                     if verified.get("uiTestBridge") else "Remote menu test control is not compiled into this build.")
    launcher = verified.get("embeddedLaunchHelper", {})
    path_probe = verified.get("embeddedConsoleTilePathProbe", {})
    write_probe = verified.get("embeddedConsoleTileWriteProbe", {})
    permission_notice = ("Standard SDK helper runtime: changes process capabilities and syscall bounds in memory. "
                         "Owner approval is required before using this private candidate."
                         if launcher.get("permissionChanges") else
                         "Restricted helper runtime: SDK credential/syscall patches and custom SO loading are disabled.")
    probe_notice = ("This private build also embeds one exact v2 read-only console-tile path probe. "
                    "Its standard SDK startup changes only the probe process's credential and syscall bounds. "
                    "The probe observes fixed title paths and writes a create-only v2 receipt under /data/console-shelf; "
                    "it does not install a tile, open a database, or use the network."
                    if path_probe else "No console-tile path probe is embedded in this build.")
    if write_probe:
        probe_notice += (" It also embeds a distinct one-shot v4 scratch write probe. The owner-enabled "
                         "probe-writes-v4 action reserves a dot-prefixed nonce folder under /data/homebrew, tests "
                         "directory-handle writes/rename/readback/fsync, then attempts cleanup. Its create-only "
                         "receipt records each stage and any remaining scratch; this does not install a tile.")
    owned_tile_notice = (
        "This private qualification build enables isShelf's owned-source Home shortcut manager. "
        "It may create, edit, isolate, queue post-close removal and—after explicit TV confirmation—clean only a "
        "shortcut carrying isShelf's exact ownership record. Removal verifies ShadowMount manual "
        "tracking first so a retired source cannot be rediscovered. This path is blocked from public releases."
        if verified.get("ownedTileWrites") and verified.get("uiTestBridge") else
        "This production candidate enables isShelf's exact-owned-source Home shortcut manager. "
        "It may create, update and queue post-close removal only for shortcuts carrying isShelf's ownership record. "
        "Close isShelf after removal so ShadowMount can refresh the Home screen. The public binary verifier "
        "rejects private remote control, preview and diagnostic probe code."
        if verified.get("ownedTileWrites") else
        "Home shortcut creation and removal are disabled in this build; status remains read-only.")
    identity = json.loads((build / "build-source.json").read_text())
    require(identity == source_fingerprint(), "Rebuild the PS5 executable before assembling its test folder")
    status = root_git("status", "--porcelain")
    require(not status.strip(), "Commit reviewed sources before assembling the test folder")
    commit = root_git("rev-parse", "HEAD").strip()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    destination = CACHE / "test-packages" / ("ui-" + stamp + "-" + sha256(build / "eboot.bin")[:12])
    app = destination / "PPSA99541"
    app.mkdir(parents=True, exist_ok=False)

    def copy(source, relative):
        target = app / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as out, Path(source).open("rb") as incoming:
            shutil.copyfileobj(incoming, out)

    copy(build / "eboot.bin", "eboot.bin")
    copy(build / "libc-a.prx", "sce_module/libc.prx")
    require(sha256(app / "sce_module/libc.prx") == "e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036", "Wrong clean-room runtime")
    copy(ROOT / "native/ps5/param.json", "sce_sys/param.json")
    update_ca = prepare_update_ca()
    copy(update_ca, "assets/update-ca.pem")
    update_ca_pin = json.loads((ROOT / "native/ps5/dependencies.json").read_text())["updateCa"]
    require((app / "assets/update-ca.pem").stat().st_size == update_ca_pin["bytes"] and
            sha256(app / "assets/update-ca.pem") == update_ca_pin["sha256"],
            "Packaged update CA bundle differs from its reviewed pin")
    presentation = verify_presentation()
    for name in ("pic0.dds", "pic1.dds"):
        copy(ROOT / "native/ps5/sce_sys" / name, "sce_sys/" + name)
    metadata = json.loads((app / "sce_sys/param.json").read_text())
    require(metadata["titleId"] == "PPSA99541" and metadata["requiredSystemSoftwareVersion"] == "0x0000000000000000", "Wrong test identity or firmware requirement")
    images = sorted((ROOT / "assets/3d-icons").glob("*.png"))
    require(len(images) == 52, "Expected the 52 reviewed system illustrations")
    for image in images:
        copy(image, "assets/3d-icons/" + image.name)
    # The app creates only user-selected shortcuts. Bundle the pinned small
    # forwarder and one reviewed icon per catalog console for that workflow.
    tracked_console_tiles = stage_tracked_console_tile_bundle(app / "assets/console-tiles")
    console_tile_pins = verify_compiled_console_tile_pins(
        build / "eboot.elf", app / "assets/console-tiles")
    generic_tile_pack = stage_generic_tile_pack_asset(app / "assets/console-tiles")
    packaged_tile_assets = [path for path in sorted((app / "assets/console-tiles").rglob("*"))
                            if path.is_file()]
    require(len(packaged_tile_assets) == 59, "Unexpected packaged console-tile asset inventory")
    for packaged in packaged_tile_assets:
        relative = packaged.relative_to(app).as_posix()
        tracked = ROOT / relative
        require(tracked.is_file() and not tracked.is_symlink() and
                tracked.read_bytes() == packaged.read_bytes(),
                "Packaged console-tile asset differs from tracked release input: " + relative)
    generic_tile_pack["trackedBundleFiles"] = len(packaged_tile_assets)
    generic_tile_pack.update(tracked_console_tiles)
    for image in ("shelf-icon.png", "shelf-icon-v1.png", "shelf-icon-premium-v2.png"):
        copy(ROOT / "assets" / image, "assets/" + image)
    copy(ROOT / "assets/branding/shelf-gaming-mark.png", "assets/branding/shelf-gaming-mark.png")
    scenery = json.loads((ROOT / "assets/backgrounds/manifest.json").read_text(encoding="utf-8"))
    require(scenery.get("schema") == 1 and len(scenery.get("assets", [])) == 2, "Expected two reviewed scenery assets")
    require({item["local"] for item in scenery["assets"]} == {
        "assets/backgrounds/gamer-lounge-v1.png", "assets/backgrounds/gamer-lounge-deep-v1.png"}, "Unexpected scenery paths")
    for item in scenery["assets"]:
        source = ROOT / item["local"]
        raw = source.read_bytes()
        require(len(raw) == item["bytes"] and hashlib.sha256(raw).hexdigest() == item["sha256"], "Scenery digest mismatch")
        require(raw[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack_from(">II", raw, 16) == (item["width"], item["height"]), "Scenery dimensions mismatch")
        copy(source, item["local"])
    copy(ROOT / "assets/backgrounds/manifest.json", "assets/backgrounds/manifest.json")
    for name in ("Poppins-Regular.ttf", "Poppins-SemiBold.ttf", "OFL.txt"):
        copy(ROOT / "native/assets/fonts" / name, "native/assets/fonts/" + name)
    for name in ("SDL2-PS5.txt", "libsamplerate-0.2.2.txt", "LGPL-2.1.txt", "LLVM-libcxx.txt",
                 "Mbed-TLS-3.6.7.txt", "PS5SX2-Swordpdf-NOTICE.txt"):
        copy(ROOT / "native/third_party/licenses" / name, "licenses/" + name)
    for name in ("LICENSE", "THIRD_PARTY.md"):
        copy(ROOT / name, name)
    copy(ROOT / "native/third_party/cjson/LICENSE", "licenses/cJSON-MIT.txt")
    copy(ROOT / "native/third_party/stb/stb_image.h", "licenses/stb_image-source-with-license.h")
    copy(ROOT / "native/third_party/stb/stb_truetype.h", "licenses/stb_truetype-source-with-license.h")
    copy(ROOT / "native/third_party/stb/stb_image_write.h", "licenses/stb_image_write-source-with-license.h")
    copy(ROOT / "native/third_party/pl_mpeg/pl_mpeg.h", "licenses/pl_mpeg-source-with-MIT-license.h")
    room_media = json.loads((ROOT / "assets/room/manifest.json").read_text(encoding="utf-8"))
    require(room_media.get("officialConsoleIntro") is False and room_media.get("silent") is True,
            "Expected original silent Shelf demo")
    for item in room_media["assets"]:
        require(item["local"] in {"assets/room/shelf-intro.mpg", "assets/room/shelf-intro.mp4"}, "Unexpected demo path")
        source = ROOT / item["local"]
        require(source.stat().st_size == item["bytes"] and sha256(source) == item["sha256"], "Room demo hash mismatch")
        copy(source, item["local"])
    copy(ROOT / "assets/room/manifest.json", "assets/room/manifest.json")
    intro_root = ROOT / "assets/room/intros"
    intro_manifest = json.loads((intro_root / "manifest.json").read_text(encoding="utf-8"))
    require(intro_manifest.get("officialConsoleIntros") is False and intro_manifest.get("silent") is True,
            "Expected original silent per-console intros")
    require(len(intro_manifest["consoles"]) == len(images) and
            {item["id"] for item in intro_manifest["consoles"]} == {image.stem for image in images},
            "Original intro catalogue is incomplete")
    for console in intro_manifest["consoles"]:
        ident = console["id"]
        require({item["file"] for item in console["assets"]} == {ident + ext for ext in (".png", ".mpg", ".mp4")},
                "Unexpected intro media names")
        require(len(console["assets"]) == 3, "Duplicate intro media entry")
        for item in console["assets"]:
            source = intro_root / item["file"]
            require(source.stat().st_size == item["bytes"] and sha256(source) == item["sha256"], "Original intro digest mismatch")
            copy(source, "assets/room/intros/" + item["file"])
    copy(intro_root / "manifest.json", "assets/room/intros/manifest.json")
    copy(ROOT / "assets/CREDITS.md", "assets/CREDITS.md")
    copy(build / "build-source.json", "build-source.json")

    executable = ROOT / ".cache/package-tools/prepare-package-art"
    arguments = [ROOT / presentation.get("icon", {}).get("source", "assets/shelf-icon.png"), app / "sce_sys/icon0.png"]
    if executable.with_suffix(".exe").exists():
        executable = executable.with_suffix(".exe")
        if os.name != "nt":
            arguments = [subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip() for path in arguments]
    require(executable.is_file(), "Build tools/build-package-art.js first")
    original = sha256(ROOT / "assets/shelf-icon.png")
    run([executable, *arguments])
    require(original == sha256(ROOT / "assets/shelf-icon.png"), "Packaging changed the source artwork")
    icon = (app / "sce_sys/icon0.png").read_bytes()
    require(icon[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack_from(">II", icon, 16) == (512, 512), "Invalid tile icon")

    readme = f"""# isShelf UI110: premium seated console-tile artwork

Source: https://github.com/Iswebro/console-shelf-ps5/tree/{commit}
Unofficial test identity: PPSA99541. This build is not hardware-validated or a public release.

This private test displays Shelf's UI/settings and real library lists, and enables launch tests.
{bridge_notice}
The bundled original helper is embedded in eboot.bin and sent only to the console's existing
loopback ELF loader when you select a launch action. It waits for Shelf's explicit commitment
and cooperative closure. If that close stalls for five seconds, it rechecks the exact committed
isShelf title and app ID before one origin-close request; it never targets another title or writes emulator settings.
{permission_notice}
{probe_notice}
{owned_tile_notice}
The helper treats the PS5's exact-origin title-release gap as a bounded transition: it is
neither Home nor a foreign app, never counts as idle, and must be followed by a monitored
idle second after Shelf closes. A game action sends
one SystemService launch request and then only observes the running target and shell focus.
A normal return-Home request gets three seconds to finish; if it stalls, isShelf verifies
its own exact title ID and sends one self-close request. If isShelf is still the exact foreground
origin at five seconds, the committed helper sends one separately verified close request.
A tile-removal or artwork-refresh action passes only a validated, duplicate-free list of at most 52
isShelf allocation titles. After isShelf closes, the bounded helper gives ShadowMount three
extra seconds to clear its game-active lifecycle flag, then waits up to eight seconds for the
fixed loopback port using read-only connect-and-close probes while continuously proving no app
has opened. It rechecks authorization before sending one uninstall per exact stale title at a
queue-safe fixed one-second cadence and one final scan. Only the exact `game_active`
and `scanner_busy` transient receipts may retry that same title, after five seconds during which
SystemService continuously reports no big app; all other refusals stop immediately, and each title
has a hard limit of five such retries.
The helper's final result remains bound to the exact nonce and issue time it consumed, so this
bounded maintenance can report completion even when the full 52-title pass exceeds 30 seconds.
Observed shell focus is not proof of TV output; that result needs the owner's console test.
It expects the console's existing ftpsrv on 127.0.0.1:2121; no PC DNS host is needed for discovery.
The app stores preferences, launch state and diagnostics under /data/console-shelf. The private
probe records read-only path results for the preferred PS1 shortcut ID and one alternate ID. Its
receipt does not prove that a tile can be installed or shown on the home screen.

Before a first installation, list /data/homebrew and /user/app and stop if an unrelated app owns this identity.
For an update, use the established verified isShelf replacement and rollback procedure.
The isShelf destination is /data/homebrew/PPSA99541. Never replace another app.
Verify full raw readback hashes; ftpsrv must report SELF conversion disabled before reading eboot.bin.
Use the existing trusted ShadowMount procedure to expose this new app tile, with other apps closed.
Keep firmware 12.20 and decline system update prompts. This folder contains no firmware updater.

First test: open Shelf; check the picture, D-pad/stick, Cross/Circle and Options.
The Home controls reveal independently of the background FTP library scan. If a stale
visibility reading leaves only the opening image visible, any physical button releases
that gate after a short stability check; L3 + R3 can also enable private control there.
Square cycles Grid, Swipe and Shelf. Shelf shows eight consoles on the room's two deep display counters;
L1/R1 swaps pages with only the consoles moving; Down continues between tiers.
Sort & filter -> Shelf pages also offers one curated display of up to eight consoles,
chosen in Consoles. Appearance -> Motion offers Reduced motion.
Play room customises the TV: console artwork, personal text or PNG, clock, rotating
favourite games, intro pictures/videos (installed emulators or all consoles), or screen off.
All 52 consoles include original Shelf stills and silent animations. Your own PNG/MPEG
in the Shelf data folder takes priority. These are Shelf originals, not manufacturer boot videos.
Triangle stars games in Shelf's library. Personal PNGs stay in /data/console-shelf:
tv-image.png or intros/<console-id>.png, up to 2048 x 2048. Silent intro videos use
intros/<console-id>.mpg (MPEG-1); the PC preparation tool makes compatible copies.
The TV stays visible in console details and libraries. Static for missing emulators/games
is optional in Play room; reduced motion freezes it and videos on a still frame.
Static is drawn with fine screen-resolution grain. The room menu has its own quiet dark backboard.
Up/Down stays on the current Shelf page; Up from its top row reaches the toolbar.
Sort, visibility and favourite settings apply to all three views.
Appearance -> Missing emulator artwork chooses Black & white (default) or Full colour.
Only systems without a detected emulator are desaturated; PS4/PS5 stay in colour.
The choice is independent of console visibility and survives closing/reopening Shelf.
The selection background is confirmed; opening still shows black before the grid.
Check Nintendo 64's library and the PS1/PS2/PS3 counts, then save a layout/visibility choice.
Close and reopen Shelf to check persistence. Report any blank screen, crash or incorrect input.
UI110 enables the private Add to PS5 Home Screen workflow for isShelf-owned console shortcuts:
library, supported emulator menu, or isShelf Home with the console selected. Add and update
must verify the exact owned source and registration result. Remove first isolates the source,
keeps isShelf open so more removals can be queued, and explains that ShadowMount updates Home only
after isShelf closes. Remove all isShelf tiles queues every exact owned source in one action. The
separate Close isShelf and update PS5 Home button performs one cooperative close and one bounded
post-close ShadowMount batch. On a later opening, the final in-app action rechecks ShadowMount and
both PS5 registration roots before deleting only that owned recovery source.
When a full recovery pass confirms every tile is absent, isShelf immediately clears its cached
installed, removing and queued states so the normal Add to PS5 Home Screen action returns without
requiring another restart.
Sources without isShelf's exact
ownership record stay read-only. These write controls are excluded from public builds.
Each console page also offers Add or repair all emulator tiles. It preflights the complete supported
set before its first write, adds missing isShelf-owned shortcuts, gives dedicated standalone emulators
a direct menu route, and sends shared RetroArch systems to their matching isShelf game library because
RetroArch cannot select a console without a game. It leaves PS4, PS5 and Switch 2 untouched and saves
the exact committed routes in one durable settings update. When ShadowMount needs a post-close scan, the
completion screen tells the user why and provides one Close isShelf and update PS5 Home action.
The same repair action stays available on a pending-removal screen. After ShadowMount has removed a
title and both PS5 registration roots are absent, it safely cleans that exact owned recovery copy and
recreates the supported emulator shortcut; an unconfirmed removal is retained and stops the batch.
Each console page also offers Update all installed shortcuts. It preflights every isShelf-owned
source before changing any of them, upgrades only shortcuts already installed, preserves every
exact saved destination, and refreshes the pinned artwork and runtime. Because an ordinary scan
does not replace PS5 Home's existing icon copies, it compares both registration roots, prepares the
exact stale-title list, and closes cooperatively without sending a foreground uninstall burst. The
bounded helper then refreshes one exact title per second from Home, with limited same-title retries
only for ShadowMount's reviewed game-active or scanner-busy responses, followed by one scan.
It never deletes the retained owned source and is safe to run again after a reported mid-run stop.
The current 52 reviewed icons use accurate console cutouts on one isShelf stage with no repeated
console-name tag inside the picture; PS5 Home already presents the title.
New and explicitly updated shortcuts use the v3 cooperative single-flight runtime. The selected shortcut
queues one bounded route, requests a clean shell close instead of returning through the forbidden
process-exit path, and waits for a stable Home state. It then sends one exact-title request through
ShadowMount's supported unmount API. Only an exact successful receipt can release immediately. After an
exact busy receipt, it observes five continuous Home-idle seconds without polling, makes one final unmount
request, and only then falls back to one-per-second read-only game-info checks if still needed. Unavailable
or malformed replies also use that bounded read-only gate and cannot race the runtime release. Repeated
presses for the same title and route are coalesced into that original handoff,
so they cannot create competing relays or lose the selected console.
Dedicated emulator routes then launch their exact
pinned menu directly, without visibly opening isShelf or waiting for its library scan. Shared RetroArch
systems and explicit Library routes open isShelf with the requested console. It does not keep a resident forwarder, retry
a launch, or require an autoload entry. Existing
isShelf-owned legacy shortcuts are upgraded only when Update is selected and remain recoverable
if any atomic replacement step fails.
Validated Library and isShelf Home destinations select their saved console surface before the read-only
FTP library scan finishes, so the opening reveal already points at the requested console. The scan stays
in the background and direct-emulator destinations still require its complete pinned-route validation.
The package also carries an exact, hash-checked 51-title source pack under
assets/console-tiles/pack-v1 plus the adopted PS1 source. UI110 uses those pinned bytes only for
the private, owner-selected shortcut workflow; they are not cosmetic tag assets.
The game library shows a permanent launch panel before you select a game.
Library only / Open emulator library means choosing the game inside that emulator.
Direct play / Play game means the selected game is passed directly to RetroArch.
Changing Preferred emulator updates the panel; RetroArch route choices save across reopen.
Test PS2 -> Open emulator library first. Allow up to ten seconds after Shelf fades out.
Check that PS5SX2 appears on the TV, rather than remaining behind the PS5 home screen.
PS1/PS3 and other detected standalone emulators currently open their own libraries.
For N64 using RetroArch, a selected game uses the external --rom/--core forwarder contract.
All launch routes remain experimental until their console result is recorded. These checks and
the final source/license/relinking package are required before a public download.

Rollback: close Shelf and remove only its owned app folder using the established app-removal procedure.
Keep /data/console-shelf if you want to retain its preferences; existing emulator/game data is separate.

Licenses and credits are included. The exact complete library sources and relinking kit must still
be assembled before any public binary release. This local package is for the owner's private test only.
"""
    (app / "START-HERE.md").write_text(readme, encoding="utf8", newline="\n")
    files = [{"path": p.relative_to(app).as_posix(), "bytes": p.stat().st_size, "sha256": sha256(p)}
             for p in sorted(app.rglob("*")) if p.is_file()]
    purpose = ("private native launch test; not a public release" if verified.get("uiTestBridge") else
               "public-build hardware candidate; not yet a release")
    manifest = {"schema": 1, "purpose": purpose, "titleId": "PPSA99541", "sourceCommit": commit,
                "hardwareTest": "not run", "launcherRuntime": launcher, "pathProbe": path_probe,
                "writeProbe": write_probe, "genericTilePack": generic_tile_pack,
                "consoleTileBundlePins": console_tile_pins,
                "privateImportContract": verified.get("privateImportContract"),
                "publicImportContract": verified.get("publicImportContract"),
                "uiTestBridge": verified.get("uiTestBridge", False),
                "ownedTileWrites": verified.get("ownedTileWrites", False),
                "sourceFingerprintSHA256": hashlib.sha256((build / "build-source.json").read_bytes()).hexdigest(), "files": files}
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    archive = destination / "Console-Shelf-UI-test.zip"
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for item in files:
            output.write(app / item["path"], "PPSA99541/" + item["path"])
    with zipfile.ZipFile(archive) as completed:
        require(completed.testzip() is None, "Test package ZIP integrity failed")
        for item in files:
            raw = completed.read("PPSA99541/" + item["path"])
            require(len(raw) == item["bytes"] and hashlib.sha256(raw).hexdigest() == item["sha256"], "ZIP member changed")
    receipt = {"directory": str(destination.relative_to(ROOT)), "archiveBytes": archive.stat().st_size, "archiveSHA256": sha256(archive),
               "files": len(files), "appBytes": sum(item["bytes"] for item in files), "titleId": "PPSA99541", "sourceCommit": commit,
               "consoleInstall": "not performed", "publicRelease": "not published", "launcherRuntime": launcher,
               "pathProbe": path_probe, "writeProbe": write_probe,
               "genericTilePack": generic_tile_pack,
               "consoleTileBundlePins": console_tile_pins,
               "privateImportContract": verified.get("privateImportContract"),
               "publicImportContract": verified.get("publicImportContract"),
               "uiTestBridge": verified.get("uiTestBridge", False),
               "ownedTileWrites": verified.get("ownedTileWrites", False)}
    (CACHE / "test-packages/latest.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
