# SPDX-License-Identifier: GPL-3.0-only
"""Prepare isShelf console forwarders offline. This program never contacts a PS5."""

from __future__ import annotations

import argparse
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM_COMMIT = "22e8762ad7dbd01e18bd8c0e95f86461fdf8aaf4"
UPSTREAM_URL = "https://github.com/heydemoura/ps5-forwarder-format"
COOPERATIVE_RUNTIME = "isshelf-cooperative-forwarder/3"
COOPERATIVE_TEMPLATE = ROOT / ".cache/ps5/console-tile-forwarder-v2"
COOPERATIVE_EBOOT_SHA256 = "8e43160e64a4f3fe9f84a2a7cd4b9175772ff9897c3bd8a25870f32b0d936638"
UPSTREAM_TEMPLATE_HASHES = {
    "template/eboot.bin": "3944ed19d258d348be0dda445959740919be4f727d6e6766e783b8a01c298331",
    "template/sce_module/libc.prx": "e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036",
    "forwarder/param.json": "b70d9efe0c435f2c0e0497e7753de30dff205d7786f9ce47eb7664d3cb0835c3",
    "LICENSE": "230184f60bae2feaf244f10a8bac053c8ff33a183bcc365b4d8b876d2b7f4809",
}
TEMPLATE_HASHES = {**UPSTREAM_TEMPLATE_HASHES,
                   "template/eboot.bin": COOPERATIVE_EBOOT_SHA256}
MAIN_TITLE_ID = "PPSA99541"
FIRST_TILE_NUMBER = 99700
FIRST_ALTERNATE_NUMBER = 99752
LAST_TILE_NUMBER = 99899
RESERVED_TITLE_IDS = {"PPSA99764"}  # Upstream target registry at the pinned commit.
TITLE_ID_PATTERN = re.compile(r"^(?:PPSA|CUSA|LAPY)[0-9]{5}$")
DESTINATIONS = {"library", "home", "emulator"}
TILE_STAGE = ROOT / "assets/console-tiles/isshelf-stage-v2.png"
TILE_STAGE_SHA256 = "eb0841fc2bc49ddf269ce9d59caac546f544005f5705696876ad713cfd31a47e"
TILE_STAGE_SIZE = (1254, 1254)
TILE_STAGE_CONTACT_Y = 420
ART_STABLE_ALPHA = 16
ART_CONTACT_ALPHA = 64
REVIEWED_ICON_ROOT = ROOT / "assets/console-tiles/icons"
REVIEWED_ICON_FORMAT = "isshelf-console-tile-icons/2"


def known_menu_titles() -> set[str]:
    """Keep offline emulator arguments inside the native launch allowlist."""
    source = (ROOT / "native/include/shelf/launch_protocol.hpp").read_text(encoding="utf-8")
    match = re.search(r"inline bool launch_menu_known\(.*?for \(auto known : \{(.*?)\}\)", source, re.S)
    if not match:
        raise ValueError("Cannot locate native emulator-menu allowlist")
    titles = set(re.findall(r'"((?:PPSA|CUSA)[0-9]{5})"', match.group(1)))
    if not titles:
        raise ValueError("Native emulator-menu allowlist is empty")
    return titles

# Legacy short labels remain in bundle metadata for compatibility with the
# already-reviewed source preparation tools. They are deliberately not drawn
# into icon0.png: PS5 Home already presents the full console title.
BADGES = {
    "ps5": "PS5", "ps4": "PS4", "ps3": "PS3", "ps2": "PS2", "ps1": "PS1", "psp": "PSP",
    "switch2": "SWITCH 2", "switch": "SWITCH", "wiiu": "WII U", "wii": "WII",
    "gamecube": "GAMECUBE", "n64": "N64", "3ds": "3DS", "ds": "DS", "gba": "GBA",
    "gbc": "GBC", "gb": "GB", "snes": "SNES", "nes": "NES", "fds": "FDS",
    "virtualboy": "VIRTUAL BOY", "pokemonmini": "POKÉMON MINI",
    "dreamcast": "DREAMCAST", "saturn": "SATURN", "naomi": "NAOMI",
    "genesis": "MEGA DRIVE", "segacd": "SEGA CD", "sega32x": "32X",
    "mastersystem": "MASTER SYSTEM", "gamegear": "GAME GEAR", "sg1000": "SG-1000",
    "atomiswave": "ATOMISWAVE", "atarijaguar": "JAGUAR", "atarilynx": "LYNX",
    "atari7800": "ATARI 7800", "atari5200": "ATARI 5200", "atari2600": "ATARI 2600",
    "pcengine": "PC ENGINE", "supergrafx": "SUPERGRAFX", "pcfx": "PC-FX",
    "neogeocd": "NEO GEO CD", "neogeopocketcolor": "NGPC", "neogeopocket": "NGP",
    "wonderswancolor": "WONDERSWAN C", "wonderswan": "WONDERSWAN",
    "threedo": "3DO", "arcadefbneo": "FINALBURN NEO", "arcademame": "MAME",
    "amiga": "AMIGA", "c64": "C64", "dos": "DOS", "scummvm": "SCUMMVM",
}


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def catalog() -> list[dict[str, str]]:
    """Read the generated native catalog and cross-check every pinned illustration."""
    source = (ROOT / "native/include/shelf/catalog.generated.hpp").read_text(encoding="utf-8")
    rows = []
    for line in source.splitlines():
        match = re.match(r'^\s*\{("(?:\\.|[^"\\])*"), ("(?:\\.|[^"\\])*"),', line)
        if match:
            rows.append({"id": json.loads(match[1]), "name": json.loads(match[2])})
    manifest = json.loads((ROOT / "assets/3d-icons/manifest.json").read_text(encoding="utf-8"))
    if len(rows) != 52 or len(manifest) != 52 or len({row["id"] for row in rows}) != 52:
        raise ValueError("Expected 52 distinct native catalog systems and art records")
    art = {item["id"]: item for item in manifest}
    if set(art) != {row["id"] for row in rows} or set(BADGES) != set(art):
        raise ValueError("Catalog, art manifest and badge labels disagree")
    for index, row in enumerate(rows):
        item = art[row["id"]]
        expected = f'assets/3d-icons/{row["id"]}.png'
        image = ROOT / expected
        if item["local"] != expected or image.stat().st_size != item["bytes"] or digest(image).upper() != item["sha256"].upper():
            raise ValueError(f"Illustration pin mismatch: {row['id']}")
        row.update({"art": expected, "badge": BADGES[row["id"]],
                    "preferredTitleId": f"PPSA{FIRST_TILE_NUMBER + index:05d}"})
    return rows


def resolve_system(system_id: str) -> dict[str, str]:
    for row in catalog():
        if row["id"] == system_id:
            return row
    raise ValueError(f"Unknown catalog console: {system_id}")


@lru_cache(maxsize=4)
def _system_menu_titles(catalog_source: str, library_source: str,
                        launch_source: str) -> dict[str, frozenset[str]]:
    """Mirror native app/core discovery, without assuming a title serves every console."""
    systems = {row["id"] for row in catalog()}
    cores: dict[str, str] = {}
    for line in catalog_source.splitlines():
        if not re.match(r'^\s*\{"', line):
            continue
        strings = re.findall(r'"(?:\\.|[^"\\])*"', line)
        if len(strings) < 5:
            continue
        system, core = json.loads(strings[0]), json.loads(strings[4])
        if system in cores:
            raise ValueError("Native catalog has a duplicate system")
        cores[system] = core
    if set(cores) != systems:
        raise ValueError("Native core suggestions differ from the 52-console catalog")
    match = re.search(r"static const App apps\[\] = \{(.*?)\};", library_source, re.S)
    if not match:
        raise ValueError("Cannot locate native emulator app table")
    apps = re.findall(r'\{\s*"(PPSA[0-9]{5})"\s*,\s*"[^\"]+"\s*,\s*"([^\"]*)"\s*\}',
                      match.group(1))
    if len(apps) != 11 or len({title for title, _ in apps}) != 11:
        raise ValueError("Native emulator app table shape changed")
    allowed = re.search(r"inline bool launch_menu_known\(.*?for \(auto known : \{(.*?)\}\)",
                        launch_source, re.S)
    if not allowed:
        raise ValueError("Cannot locate native emulator-menu allowlist")
    allowlist = set(re.findall(r'"((?:PPSA|CUSA)[0-9]{5})"', allowed.group(1)))
    if not allowlist:
        raise ValueError("Native emulator-menu allowlist is empty")
    result: dict[str, set[str]] = {system: set() for system in systems}
    for title, names in apps:
        if title not in allowlist:
            raise ValueError("Native emulator app left launch allowlist")
        for system in filter(None, names.split("|")):
            if system not in result:
                raise ValueError("Native emulator app lists unknown console")
            result[system].add(title)
    if "PPSA99169" not in allowlist:
        raise ValueError("RetroArch left native emulator-menu allowlist")
    for system, core in cores.items():
        if core:
            result[system].add("PPSA99169")
    return {system: frozenset(titles) for system, titles in result.items()}


def system_menu_titles() -> dict[str, frozenset[str]]:
    return dict(_system_menu_titles(
        (ROOT / "native/include/shelf/catalog.generated.hpp").read_text(encoding="utf-8"),
        (ROOT / "native/src/library.cpp").read_text(encoding="utf-8"),
        (ROOT / "native/include/shelf/launch_protocol.hpp").read_text(encoding="utf-8"),
    ))


def validate_intent(system_id: str, destination: str, emulator: str | None) -> str:
    menus = system_menu_titles()
    if system_id not in menus:
        raise ValueError("Unknown catalog console")
    if destination not in DESTINATIONS:
        raise ValueError("Unknown shortcut destination")
    if destination == "emulator":
        if not emulator or emulator not in known_menu_titles() or emulator == MAIN_TITLE_ID:
            raise ValueError("Emulator choice needs a supported native menu title ID")
        if emulator not in menus[system_id]:
            raise ValueError("Emulator menu does not serve the selected console")
    elif emulator:
        raise ValueError("Only the emulator destination accepts an emulator title ID")
    token = f"--isshelf-console-tile={system_id},{destination},{emulator or '-'}"
    if len(token.encode("ascii")) > 127:
        raise ValueError("Console intent exceeds PS5 argument bound")
    return token


def validate_creation_intent(system_id: str, destination: str,
                             emulator: str | None) -> str:
    """Validate a newly created route while retaining legacy parser coverage."""
    token = validate_intent(system_id, destination, emulator)
    if destination == "emulator" and emulator == "PPSA99169":
        raise ValueError(
            "RetroArch cannot select a console menu without a game; use the isShelf library route")
    return token


def occupied_ids(path: Path) -> set[str]:
    """Accept ShadowMountPlus list_games JSON or a list of checked title IDs."""
    data = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(data, dict) and isinstance(data.get("games"), list):
        values = [item.get("title_id") for item in data["games"]]
        if data.get("count") != len(values):
            raise ValueError("Incomplete ShadowMountPlus title inventory")
    elif isinstance(data, list):
        values = data
    else:
        raise ValueError("Inventory must be list_games JSON or an ID array")
    if any(not isinstance(v, str) or not TITLE_ID_PATTERN.fullmatch(v) for v in values):
        raise ValueError("Inventory contains an invalid title ID")
    if len(set(values)) != len(values):
        raise ValueError("Inventory contains duplicate title IDs")
    return set(values)


def choose_title_id(row: dict[str, str], occupied: set[str], explicit: str | None = None) -> str:
    if explicit is not None:
        candidates = [explicit]
    else:
        candidates = [row["preferredTitleId"]] + [
            f"PPSA{number:05d}" for number in range(FIRST_ALTERNATE_NUMBER, LAST_TILE_NUMBER + 1)]
    for candidate in candidates:
        if not (re.fullmatch(r"PPSA[0-9]{5}", candidate) and
                FIRST_TILE_NUMBER <= int(candidate[4:]) <= LAST_TILE_NUMBER):
            raise ValueError("Tile ID must be in isShelf's PPSA99700..PPSA99899 range")
        if candidate in RESERVED_TITLE_IDS:
            if explicit:
                raise ValueError("Tile ID is reserved by the pinned forwarder target registry")
            continue
        if candidate not in occupied and candidate != MAIN_TITLE_ID:
            return candidate
    raise ValueError("No unoccupied isShelf console tile title ID is available")


def verify_template(template_root: Path) -> None:
    result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=template_root,
                            capture_output=True, text=True, check=True)
    if result.stdout.strip() != UPSTREAM_COMMIT:
        raise ValueError("Forwarder source commit differs from reviewed pin")
    for relative, expected in UPSTREAM_TEMPLATE_HASHES.items():
        path = template_root / relative
        if not path.is_file() or digest(path) != expected:
            raise ValueError(f"Forwarder template pin mismatch: {relative}")


def cooperative_template() -> Path:
    manifest = COOPERATIVE_TEMPLATE / "build.json"
    eboot = COOPERATIVE_TEMPLATE / "template/eboot.bin"
    libc = COOPERATIVE_TEMPLATE / "template/sce_module/libc.prx"
    if not manifest.is_file() or not eboot.is_file() or not libc.is_file():
        raise ValueError("Build the cooperative PS5 tile forwarder before packaging")
    record = json.loads(manifest.read_text(encoding="utf-8"))
    if (record.get("format") != "isshelf-cooperative-console-forwarder/3" or
            record.get("status") != "offline-built" or
            record.get("consoleContact") is not False or
            record.get("target") != MAIN_TITLE_ID or
            record.get("relayLifecycle") !=
                "one-shot-with-identical-request-coalescing" or
            record.get("stableHomeObservations") != 5 or
            record.get("shadowMountReleaseGate") !=
                "one-unmount-assist-one-retry-after-5s-then-paced-game-info" or
            record.get("shadowMountUnmountAttempts") != 2 or
            record.get("shadowMountUnmountRetryDelayMs") != 5000 or
            record.get("shadowMountInfoIntervalMs") != 1000 or
            record.get("shadowMountReleaseMaxWaitMs") != 40000 or
            record.get("launchAttempts") != 1 or
            record.get("tileExit") != "clean-shell-close-no-crt-return" or
            record.get("emulatorHandoff") != "direct-system-menu-or-isshelf-library" or
            record.get("eboot") != {"bytes": eboot.stat().st_size,
                                     "sha256": COOPERATIVE_EBOOT_SHA256} or
            record.get("libc") != {"bytes": libc.stat().st_size,
                                    "sha256": TEMPLATE_HASHES["template/sce_module/libc.prx"]} or
            digest(eboot) != COOPERATIVE_EBOOT_SHA256 or
            digest(libc) != TEMPLATE_HASHES["template/sce_module/libc.prx"]):
        raise ValueError("Cooperative PS5 tile forwarder build differs from its pin")
    for relative, expected in record.get("inputs", {}).items():
        path = ROOT / relative
        if not path.is_file() or digest(path) != expected:
            raise ValueError(f"Cooperative tile source changed after build: {relative}")
    return COOPERATIVE_TEMPLATE / "template"


def seat_artwork(source):
    """Place real hardware on the shelf, ignoring faint source-shadow tails."""
    alpha = source.getchannel("A")
    stable = alpha.point(
        lambda value: 255 if value >= ART_STABLE_ALPHA else 0).getbbox()
    if stable is None:
        raise ValueError("Console illustration has no stable visible pixels")
    padding = 4
    stable = (max(0, stable[0] - padding), max(0, stable[1] - padding),
              min(source.width, stable[2] + padding),
              min(source.height, stable[3] + padding))
    source = source.crop(stable)
    contact = source.getchannel("A").point(
        lambda value: 255 if value >= ART_CONTACT_ALPHA else 0).getbbox()
    if contact is None:
        raise ValueError("Console illustration has no physical contact edge")
    x = (512 - source.width) // 2
    y = TILE_STAGE_CONTACT_Y - contact[3]
    if y < 18:
        raise ValueError("Console illustration cannot fit above the shelf contact line")
    return source, x, y, contact[3]


def render_icon(row: dict[str, str], target: Path) -> None:
    try:
        from PIL import Image, ImageFilter
    except ImportError as exc:
        raise RuntimeError("Pillow is required to render reviewed console art") from exc
    if not TILE_STAGE.is_file() or digest(TILE_STAGE) != TILE_STAGE_SHA256:
        raise ValueError("Pinned isShelf tile stage differs")
    with Image.open(TILE_STAGE) as stage:
        stage.load()
        if stage.size != TILE_STAGE_SIZE or stage.format != "PNG":
            raise ValueError("Unexpected isShelf tile stage geometry")
        canvas = stage.convert("RGBA").resize((512, 512), Image.Resampling.LANCZOS)
    with Image.open(ROOT / row["art"]) as source:
        source.load()
        if not (512 <= source.width <= 2048 and 512 <= source.height <= 2048):
            raise ValueError("Unexpected console illustration geometry")
        source = source.convert("RGBA")
    bounds = source.getchannel("A").getbbox()
    if bounds is None:
        raise ValueError("Console illustration is fully transparent")
    source = source.crop(bounds)
    # Normalize the visible silhouette rather than its transparent source
    # canvas. Every system therefore reads clearly at PS5 Home icon size.
    source.thumbnail((438, 398), Image.Resampling.LANCZOS)
    source, x, y, _ = seat_artwork(source)

    # A restrained shadow and brand-colour halo seat each existing, accurate
    # console cutout on the generated product stage without altering its art.
    alpha = source.getchannel("A")
    shadow_mask = Image.new("L", canvas.size)
    shadow_mask.paste(alpha, (x, min(511, y + 13)))
    shadow_mask = shadow_mask.filter(ImageFilter.GaussianBlur(17)).point(
        lambda value: value * 105 // 255)
    shadow = Image.new("RGBA", canvas.size, (0, 4, 10, 0))
    shadow.putalpha(shadow_mask)
    canvas.alpha_composite(shadow)

    halo_mask = Image.new("L", canvas.size)
    halo_mask.paste(alpha, (x, y))
    halo_mask = halo_mask.filter(ImageFilter.GaussianBlur(23)).point(
        lambda value: value * 58 // 255)
    halo = Image.new("RGBA", canvas.size, (53, 243, 227, 0))
    halo.putalpha(halo_mask)
    canvas.alpha_composite(halo)
    canvas.alpha_composite(source, (x, y))
    canvas.convert("RGB").save(target, "PNG", optimize=True)
    with Image.open(target) as check:
        if check.size != (512, 512) or check.format != "PNG":
            raise ValueError("Rendered PS5 icon is invalid")


def build_reviewed_icons(output: Path) -> Path:
    """Create the complete, reviewable 52-icon artwork set atomically."""
    rows = catalog()
    if output.exists():
        raise FileExistsError(f"Refusing to replace existing output: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".isshelf-reviewed-icons-",
                                     dir=output.parent) as temporary:
        icons = Path(temporary)
        systems = []
        for row in rows:
            icon = icons / (row["id"] + ".png")
            render_icon(row, icon)
            systems.append({
                "id": row["id"], "name": row["name"],
                "file": row["id"] + ".png",
                "bytes": icon.stat().st_size,
                "sha256": digest(icon),
            })
        write_json(icons / "manifest.json", {
            "format": REVIEWED_ICON_FORMAT,
            "stage": TILE_STAGE.relative_to(ROOT).as_posix(),
            "stageSha256": TILE_STAGE_SHA256,
            "textTags": False,
            "systems": systems,
        })
        os.rename(icons, output)
    return output


def metadata(template_root: Path, row: dict[str, str], title_id: str) -> dict:
    param = json.loads((template_root / "forwarder/param.json").read_text(encoding="utf-8"))
    if param.get("titleId") != "PPSA99100":
        raise ValueError("Unexpected forwarder metadata template")
    param["titleId"] = title_id
    param["conceptId"] = title_id[4:]
    param["contentId"] = f"UP9000-{title_id}_00-PS5FORWARDER0000"
    param["localizedParameters"] = {"defaultLanguage": "en-US", "en-US": {"titleName": row["name"]}}
    return param


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def build_forwarder(system: str, destination: str, emulator: str | None,
                    output: Path, template_root: Path, inventory: Path,
                    explicit_title_id: str | None = None) -> Path:
    row = resolve_system(system)
    token = validate_creation_intent(row["id"], destination, emulator)
    verify_template(template_root)
    runtime_template = cooperative_template()
    occupied = occupied_ids(inventory)
    title_id = choose_title_id(row, occupied, explicit_title_id)
    if destination == "emulator" and emulator not in occupied:
        raise ValueError("Chosen emulator is absent from the supplied installed-title inventory")
    output.mkdir(parents=True, exist_ok=True)
    final = output / title_id
    if final.exists():
        raise FileExistsError(f"Refusing to replace existing output: {final}")
    with tempfile.TemporaryDirectory(prefix=".isshelf-tile-", dir=output) as temporary:
        tile = Path(temporary)
        (tile / "sce_module").mkdir()
        (tile / "sce_sys").mkdir()
        shutil.copyfile(runtime_template / "eboot.bin", tile / "eboot.bin")
        shutil.copyfile(template_root / "template/sce_module/libc.prx", tile / "sce_module/libc.prx")
        write_json(tile / "forwarder.json", {
            "format": "ps5-forwarder/1", "target": MAIN_TITLE_ID,
            "args": [token], "creator": "isShelf console tiles",
        })
        write_json(tile / "sce_sys/param.json", metadata(template_root, row, title_id))
        render_icon(row, tile / "sce_sys/icon0.png")
        write_json(tile / "isshelf-console-tile.json", {
            "format": "isshelf-console-tile/1", "system": row["id"],
            "titleId": title_id, "target": MAIN_TITLE_ID,
            "forwarderSource": UPSTREAM_URL, "forwarderCommit": UPSTREAM_COMMIT,
            "runtime": COOPERATIVE_RUNTIME,
        })
        shutil.copyfile(template_root / "LICENSE", tile / "LICENSE-forwarder.txt")
        # No implicit overwrite, and everything stays on the same volume.
        if final.exists():
            raise FileExistsError(f"Refusing to replace existing output: {final}")
        os.rename(tile, final)
    return final


def build_bundle(output: Path, template_root: Path) -> Path:
    """Produce reusable template and all 52 icons for the main app's packaging."""
    verify_template(template_root)
    runtime_template = cooperative_template()
    rows = catalog()
    if output.exists():
        raise FileExistsError(f"Refusing to replace existing bundle: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".isshelf-bundle-", dir=output.parent) as temporary:
        bundle = Path(temporary)
        (bundle / "template/sce_module").mkdir(parents=True)
        (bundle / "icons").mkdir()
        shutil.copyfile(runtime_template / "eboot.bin", bundle / "template/eboot.bin")
        shutil.copyfile(template_root / "template/sce_module/libc.prx",
                        bundle / "template/sce_module/libc.prx")
        shutil.copyfile(template_root / "forwarder/param.json", bundle / "template/param.json")
        shutil.copyfile(template_root / "LICENSE", bundle / "LICENSE-forwarder.txt")
        for row in rows:
            render_icon(row, bundle / "icons" / (row["id"] + ".png"))
        write_json(bundle / "catalog.json", {
            "format": "isshelf-console-tile-assets/1", "target": MAIN_TITLE_ID,
            "forwarderSource": UPSTREAM_URL, "forwarderCommit": UPSTREAM_COMMIT,
            "runtime": COOPERATIVE_RUNTIME,
            "templateSha256": {
                "template/eboot.bin": TEMPLATE_HASHES["template/eboot.bin"],
                "template/sce_module/libc.prx": TEMPLATE_HASHES["template/sce_module/libc.prx"],
                "template/param.json": TEMPLATE_HASHES["forwarder/param.json"],
            },
            "systems": [dict({key: row[key] for key in ("id", "name", "badge", "preferredTitleId")},
                             iconSha256=digest(bundle / "icons" / (row["id"] + ".png")),
                             iconBytes=(bundle / "icons" / (row["id"] + ".png")).stat().st_size)
                        for row in rows],
        })
        os.rename(bundle, output)
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template-root", type=Path, default=ROOT / ".cache/ps5-forwarder-format")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="List all catalog consoles and candidate title IDs")
    bundle = commands.add_parser("bundle", help="Build all 52 icons and pinned forwarder resources")
    bundle.add_argument("--output", type=Path, required=True)
    reviewed = commands.add_parser("reviewed-icons", help="Build the complete reviewable 52-icon set")
    reviewed.add_argument("--output", type=Path, required=True)
    forwarder = commands.add_parser("forwarder", help="Build one opt-in Forwarder Format tile")
    forwarder.add_argument("--system", required=True)
    forwarder.add_argument("--destination", choices=sorted(DESTINATIONS), required=True)
    forwarder.add_argument("--emulator", help="Installed emulator title ID, for emulator destination only")
    forwarder.add_argument("--title-id", help="Explicit unused PPSA99700..PPSA99899 tile ID")
    forwarder.add_argument("--inventory-json", type=Path, required=True)
    forwarder.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "list":
        for row in catalog():
            print(f"{row['id']:20} {row['preferredTitleId']}  {row['name']}")
    elif args.command == "bundle":
        print(build_bundle(args.output, args.template_root))
    elif args.command == "reviewed-icons":
        print(build_reviewed_icons(args.output))
    else:
        print(build_forwarder(args.system, args.destination, args.emulator,
                              args.output, args.template_root, args.inventory_json, args.title_id))


if __name__ == "__main__":
    main()
