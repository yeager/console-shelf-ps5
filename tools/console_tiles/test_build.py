# SPDX-License-Identifier: GPL-3.0-only
"""Contract checks for console forwarder identity and create-only output."""

import json
import inspect
from pathlib import Path
import tempfile
import unittest

from build import (COOPERATIVE_RUNTIME, COOPERATIVE_TEMPLATE, REVIEWED_ICON_FORMAT,
                   REVIEWED_ICON_ROOT, ROOT, TILE_STAGE, TILE_STAGE_SHA256,
                   TILE_STAGE_CONTACT_Y,
                   UPSTREAM_COMMIT, build_forwarder, build_reviewed_icons,
                   catalog, choose_title_id, digest, occupied_ids, render_icon,
                   resolve_system, seat_artwork, system_menu_titles, validate_creation_intent,
                   validate_intent)

try:
    import PIL  # noqa: F401
except ImportError:
    PIL = None


class ConsoleTileBuildTests(unittest.TestCase):
    def test_tile_stage_is_pinned_and_renderer_draws_no_badge(self):
        self.assertTrue(TILE_STAGE.is_file())
        self.assertEqual(digest(TILE_STAGE), TILE_STAGE_SHA256)
        renderer = inspect.getsource(render_icon)
        self.assertNotIn('row["badge"]', renderer)
        self.assertNotIn("ImageFont", renderer)
        self.assertNotIn("draw.text", renderer)

    def test_reviewed_icon_set_reproduces_all_52_assets(self):
        manifest = json.loads((REVIEWED_ICON_ROOT / "manifest.json").read_text(
            encoding="utf-8"))
        self.assertEqual(manifest["format"], REVIEWED_ICON_FORMAT)
        self.assertFalse(manifest["textTags"])
        self.assertEqual(manifest["stageSha256"], TILE_STAGE_SHA256)
        self.assertEqual(len(manifest["systems"]), 52)
        self.assertEqual([item["id"] for item in manifest["systems"]],
                         [row["id"] for row in catalog()])
        with tempfile.TemporaryDirectory() as temp:
            generated = build_reviewed_icons(Path(temp) / "icons")
            rebuilt = json.loads((generated / "manifest.json").read_text(
                encoding="utf-8"))
            self.assertEqual(rebuilt, manifest)
            for item in manifest["systems"]:
                reviewed = REVIEWED_ICON_ROOT / item["file"]
                self.assertTrue(reviewed.is_file())
                self.assertEqual(reviewed.stat().st_size, item["bytes"])
                self.assertEqual(digest(reviewed), item["sha256"])
                self.assertEqual(digest(generated / item["file"]), item["sha256"])

    @unittest.skipUnless(PIL, "Pillow is needed for artwork placement checks")
    def test_all_52_visible_hardware_edges_sit_on_the_stage(self):
        from PIL import Image
        for row in catalog():
            with self.subTest(system=row["id"]), Image.open(ROOT / row["art"]) as original:
                source = original.convert("RGBA")
                source = source.crop(source.getchannel("A").getbbox())
                source.thumbnail((438, 398), Image.Resampling.LANCZOS)
                _, _, y, contact_bottom = seat_artwork(source)
                self.assertEqual(y + contact_bottom, TILE_STAGE_CONTACT_Y)

    def test_all_52_console_ids_are_unique_and_stable(self):
        rows = catalog()
        self.assertEqual(len(rows), 52)
        self.assertEqual(len({r["id"] for r in rows}), 52)
        self.assertEqual(len({r["preferredTitleId"] for r in rows}), 52)
        self.assertEqual(rows[0]["preferredTitleId"], "PPSA99700")
        self.assertEqual(resolve_system("ps1")["preferredTitleId"], "PPSA99704")
        self.assertEqual(rows[-1]["preferredTitleId"], "PPSA99751")

    def test_preferred_and_alternate_title_id_allocation(self):
        row = resolve_system("ps1")
        self.assertEqual(choose_title_id(row, {"PPSA99541"}), "PPSA99704")
        self.assertEqual(choose_title_id(row, {"PPSA99704", "PPSA99752"}), "PPSA99753")
        self.assertEqual(choose_title_id(row, {"PPSA99704"}, "PPSA99899"), "PPSA99899")
        for explicit in ("PPSA99764", "PPSA99900", "PPSA99541"):
            with self.subTest(explicit=explicit), self.assertRaises(ValueError):
                choose_title_id(row, set(), explicit)

    def test_intent_rejects_cross_destination_and_invalid_targets(self):
        self.assertEqual(validate_intent("ps1", "library", None),
                         "--isshelf-console-tile=ps1,library,-")
        self.assertEqual(validate_intent("ps1", "emulator", "PPSA99248"),
                         "--isshelf-console-tile=ps1,emulator,PPSA99248")
        self.assertEqual(validate_creation_intent("ps1", "emulator", "PPSA99248"),
                         "--isshelf-console-tile=ps1,emulator,PPSA99248")
        with self.assertRaisesRegex(ValueError, "cannot select a console menu"):
            validate_creation_intent("psp", "emulator", "PPSA99169")
        for destination, emulator in (("home", "PPSA99248"), ("emulator", None),
                                      ("emulator", "PPSA99541"), ("emulator", "PPSA99999"),
                                      ("emulator", "LAPY12345"), ("emulator", "../foo"),
                                      ("unknown", None)):
            with self.subTest(destination=destination, emulator=emulator):
                with self.assertRaises(ValueError):
                    validate_intent("ps1", destination, emulator)

    def test_emulator_menu_is_system_matched(self):
        menus = system_menu_titles()
        self.assertEqual(len(menus), 52)
        self.assertEqual(menus["ps5"], frozenset())
        self.assertEqual(menus["ps3"], frozenset({"PPSA99303"}))
        self.assertEqual(menus["switch"], frozenset({"PPSA99008"}))
        self.assertEqual(menus["ps1"], frozenset({"PPSA97510", "PPSA98510",
                                                    "PPSA99248", "PPSA99169"}))
        self.assertEqual(menus["wii"], frozenset({"PPSA99764", "PPSA99169"}))
        for system, emulator in (("ps5", "PPSA99303"), ("ps1", "PPSA99303"),
                                 ("ps3", "PPSA99169"), ("switch2", "PPSA99008")):
            with self.subTest(system=system, emulator=emulator):
                with self.assertRaisesRegex(ValueError, "does not serve"):
                    validate_intent(system, "emulator", emulator)
        for destination, emulator in (("home", None), ("library", None),
                                      ("emulator", "PPSA99248")):
            with self.subTest(destination=destination):
                with self.assertRaisesRegex(ValueError, "Unknown catalog console"):
                    validate_intent("unknown", destination, emulator)
        for system in menus:
            self.assertEqual(validate_intent(system, "home", None).split(",")[0],
                             f"--isshelf-console-tile={system}")
            self.assertEqual(validate_intent(system, "library", None).split(",")[0],
                             f"--isshelf-console-tile={system}")

    def test_inventory_must_be_complete_and_well_formed(self):
        with tempfile.TemporaryDirectory() as temp:
            inventory = Path(temp) / "games.json"
            inventory.write_text(json.dumps({"count": 2, "games": [
                {"title_id": "PPSA99541"}, {"title_id": "PPSA99248"}]}))
            self.assertEqual(occupied_ids(inventory), {"PPSA99541", "PPSA99248"})
            inventory.write_text(json.dumps({"count": 3, "games": [{"title_id": "PPSA99541"}]}))
            with self.assertRaises(ValueError):
                occupied_ids(inventory)
            inventory.write_text(json.dumps(["PPSA99541", "PPSA99541"]))
            with self.assertRaises(ValueError):
                occupied_ids(inventory)

    @unittest.skipUnless(PIL and (ROOT / ".cache/ps5-forwarder-format/template/eboot.bin").is_file() and
                         (COOPERATIVE_TEMPLATE / "template/eboot.bin").is_file(),
                         "Pillow and the pinned forwarder checkout are needed for package check")
    def test_ps1_forwarder_shape_and_collision(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            inventory = root / "inventory.json"
            inventory.write_text(json.dumps(["PPSA99541", "PPSA99248"]))
            template = ROOT / ".cache/ps5-forwarder-format"
            tile = build_forwarder("ps1", "emulator", "PPSA99248", root / "out", template, inventory)
            self.assertEqual(tile.name, "PPSA99704")
            self.assertEqual({p.relative_to(tile).as_posix() for p in tile.rglob("*") if p.is_file()}, {
                "eboot.bin", "sce_module/libc.prx", "forwarder.json", "sce_sys/param.json",
                "sce_sys/icon0.png", "isshelf-console-tile.json", "LICENSE-forwarder.txt",
            })
            forwarder = json.loads((tile / "forwarder.json").read_text(encoding="utf-8"))
            self.assertEqual(forwarder["format"], "ps5-forwarder/1")
            self.assertEqual(forwarder["target"], "PPSA99541")
            self.assertEqual(forwarder["args"], ["--isshelf-console-tile=ps1,emulator,PPSA99248"])
            metadata = json.loads((tile / "sce_sys/param.json").read_text(encoding="utf-8"))
            self.assertEqual(metadata["titleId"], "PPSA99704")
            self.assertEqual(metadata["contentId"], "UP9000-PPSA99704_00-PS5FORWARDER0000")
            self.assertEqual(metadata["localizedParameters"]["en-US"]["titleName"], "PlayStation 1")
            owner = json.loads((tile / "isshelf-console-tile.json").read_text(encoding="utf-8"))
            self.assertEqual(owner["forwarderCommit"], UPSTREAM_COMMIT)
            self.assertEqual(owner["runtime"], COOPERATIVE_RUNTIME)
            from PIL import Image
            with Image.open(tile / "sce_sys/icon0.png") as icon:
                self.assertEqual(icon.size, (512, 512))
            with self.assertRaises(FileExistsError):
                build_forwarder("ps1", "library", None, root / "out", template, inventory)
            inventory.write_text(json.dumps(["PPSA99541", "PPSA99248", "PPSA99704"]))
            alternate = build_forwarder("ps1", "library", None, root / "another", template, inventory)
            self.assertEqual(alternate.name, "PPSA99752")
            with self.assertRaises(ValueError):
                build_forwarder("ps1", "library", None, root / "blocked", template,
                                inventory, explicit_title_id="PPSA99704")


if __name__ == "__main__":
    unittest.main()
