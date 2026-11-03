# SPDX-License-Identifier: GPL-3.0-only
"""Small in-memory packaging fixtures. No console, hardware claims or release ZIPs."""
import copy
import json
import posixpath
import re
import unittest
from pathlib import Path
from build_ps5 import parse_options
from prepare_ps5_release import (digest, packaged_icon_source, public_guides, public_install_notice, public_source_documents, require_public_build, validate_release_version,
                                 verify_asset_sources, verify_console_manifest, verify_source_members)


class ReleaseInputTests(unittest.TestCase):
    def test_normal_build_commands_keep_the_launchable_helper_runtime(self):
        scripts = json.loads((Path(__file__).parents[1] / "package.json").read_text())["scripts"]
        self.assertIn("--standard-helper-runtime", scripts["build:ps5"])
        self.assertIn("--owned-tile-writes", scripts["build:ps5"])
        self.assertNotIn("--ui-test-bridge", scripts["build:ps5"])
        self.assertIn("--standard-helper-runtime", scripts["build:ps5-launch-helper"])
        self.assertIn("--restricted-helper-runtime", scripts["build:ps5-restricted"])
        self.assertTrue(parse_options([]).standard_helper_runtime)
        self.assertTrue(parse_options(["--standard-helper-runtime"]).standard_helper_runtime)
        self.assertFalse(parse_options(["--restricted-helper-runtime"]).standard_helper_runtime)
        private = parse_options(["--ui-test-bridge", "--owned-tile-writes"])
        self.assertTrue(private.ui_test_bridge and private.owned_tile_writes)

    def test_private_ui_test_build_cannot_be_released(self):
        public = {"ownerSupportControl": True, "uiTestBridge": False, "ownedTileWrites": True,
                  "publicImportContract": {"ownerSupportControl": True, "uiTestBridge": False, "ownedTileWrites": True,
                                           "baselineImportCount": 243}}
        require_public_build(public)
        for value in (True, 1, "false", None):
            with self.assertRaisesRegex(ValueError, "Private UI test"):
                require_public_build({**public, "uiTestBridge": value})

    def test_release_requires_owned_tiles_and_reviewed_public_contract(self):
        with self.assertRaisesRegex(ValueError, r"L3\+R3"):
            require_public_build({"ownerSupportControl": False, "uiTestBridge": False,
                                  "ownedTileWrites": True, "publicImportContract": {}})
        for value in (False, 0, "true", None):
            with self.assertRaisesRegex(ValueError, "tile manager"):
                require_public_build({"ownerSupportControl": True, "uiTestBridge": False,
                                      "ownedTileWrites": value})
        for contract in (None, {}, {"uiTestBridge": True, "ownedTileWrites": True,
                                    "baselineImportCount": 243},
                         {"uiTestBridge": False, "ownedTileWrites": True,
                          "baselineImportCount": 242}):
            with self.assertRaisesRegex(ValueError, "public import contract"):
                require_public_build({"ownerSupportControl": True, "uiTestBridge": False, "ownedTileWrites": True,
                                      "publicImportContract": contract})

    def test_stable_and_prerelease_versions_are_explicit(self):
        for version in ("v0.1.0", "v2.4.1-alpha.3", "v2.4.1-beta.9"):
            self.assertEqual(validate_release_version(version), version)
        for version in ("0.1.0", "v0.1", "v0.1.0-rc.1", "v0.1.0-beta"):
            with self.assertRaisesRegex(ValueError, "semantic release version"):
                validate_release_version(version)

    def test_release_notice_updates_a_previous_release_and_preserves_instructions(self):
        old = "Native PS5 prototype v0.1.0-alpha.1, tested on firmware **12.20**. The app appears as **isShelf**."
        instructions = "\n\n## Recovery\nPreserve existing settings and saves.\n"
        result = public_install_notice("# Installing isShelf\n\n" + old + instructions, "v0.1.0-beta.1", "isShelf")
        self.assertIn("Native PS5 prototype v0.1.0-beta.1, tested on firmware **12.20**.", result)
        self.assertNotIn("v0.1.0-alpha.1", result)
        self.assertTrue(result.endswith(instructions))
        stable = public_install_notice(result, "v0.1.0", "isShelf")
        self.assertIn("Native PS5 prototype v0.1.0, tested on firmware **12.20**.", stable)
        self.assertNotIn("v0.1.0-beta.1", stable)

    def test_release_notice_accepts_preparation_and_rejects_missing_or_ambiguous_notices(self):
        legacy = ("Preparation guide for the first prototype. A public binary has not been published yet; see the "
                  "[acceptance status](PROTOTYPE-PLAN.md). New builds are named **isShelf (UI test)**; "
                  "earlier console-tested builds used **Console Shelf (UI test)**.")
        result = public_install_notice(legacy, "v0.1.0-beta.1", "isShelf")
        self.assertIn("The app appears as **isShelf**.", result)
        self.assertNotIn("not been published", result)
        for text in ("unrecognized guide", legacy + "\n" + legacy, result + "\n" + result, legacy + "\n" + result):
            with self.assertRaisesRegex(ValueError, "notice"):
                public_install_notice(text, "v0.2.0-beta.1", "isShelf")

    def test_home_icon_master_follows_source_kit_without_breaking_legacy_kits(self):
        key = "native/ps5/presentation-assets.json"
        self.assertEqual(packaged_icon_source({key: b'{}'}), "assets/shelf-icon.png")
        source = "assets/branding/isshelf-home-icon.png"
        self.assertEqual(packaged_icon_source({key: json.dumps({"icon": {"source": source}}).encode()}), source)
        with self.assertRaisesRegex(ValueError, "Unexpected packaged icon"):
            packaged_icon_source({key: b'{"icon":{"source":"../../outside.png"}}'})

    def setUp(self):
        self.raw = {"fixture.bin": b"fixture program", "credits.txt": b"fixture notice"}
        self.files = {name: {"path": name, **digest(raw)} for name, raw in self.raw.items()}
        self.readback = {"files": list(self.files.values()), "selfMode": "226 SELF transfer mode disabled"}

    def test_complete_console_ledger(self):
        verify_console_manifest(self.files, self.readback)

    def test_partial_console_ledger_rejected(self):
        partial = copy.deepcopy(self.readback); partial["files"].pop()
        with self.assertRaisesRegex(ValueError, "full file ledger"):
            verify_console_manifest(self.files, partial)

    def test_changed_asset_in_console_ledger_rejected(self):
        changed = copy.deepcopy(self.readback); changed["files"][1].update(digest(b"different notice"))
        with self.assertRaisesRegex(ValueError, "differs"):
            verify_console_manifest(self.files, changed)

    def test_duplicate_console_member_rejected(self):
        duplicate = copy.deepcopy(self.readback); duplicate["files"].append(duplicate["files"][0])
        with self.assertRaisesRegex(ValueError, "unique full"):
            verify_console_manifest(self.files, duplicate)

    def test_transformed_self_mode_rejected(self):
        transformed = {**self.readback, "selfMode": "226 SELF transfer mode enabled"}
        with self.assertRaisesRegex(ValueError, "raw SELF"):
            verify_console_manifest(self.files, transformed)

    def test_source_member_set_is_exact(self):
        data = {"SOURCE-KIT-MANIFEST.json": b"fixture metadata", "source.cpp": b"fixture source"}
        manifest = {"files": [{"path": "source.cpp", **digest(data["source.cpp"])}]}
        verify_source_members(data, manifest)
        with self.assertRaisesRegex(ValueError, "undeclared"):
            verify_source_members({**data, "unlisted.bin": b"not source"}, manifest)
        duplicate = {"files": manifest["files"] * 2}
        with self.assertRaisesRegex(ValueError, "duplicate"):
            verify_source_members(data, duplicate)

    def test_assets_and_notices_match_corresponding_sources(self):
        app = {"assets/3d-icons/n64.png": b"fixture image", "licenses/cJSON-MIT.txt": b"fixture license"}
        source = {"assets/3d-icons/n64.png": b"fixture image", "native/third_party/cjson/LICENSE": b"fixture license"}
        result = verify_asset_sources(app, source)
        self.assertEqual(result["byteExactFiles"], 2)
        with self.assertRaisesRegex(ValueError, "differs"):
            verify_asset_sources({**app, "assets/3d-icons/n64.png": b"other image"}, source)
        with self.assertRaisesRegex(ValueError, "lacks corresponding"):
            verify_asset_sources(app, {"assets/3d-icons/n64.png": b"fixture image"})

    def test_update_ca_matches_both_committed_provenance_pins(self):
        raw = b"fixture certificate bundle\n"
        record = digest(raw)
        revision = "b2a886b48739395de675377b03ecce75ab3368a8"
        url = ("https://raw.githubusercontent.com/Swordpdf/PS5SX2/" + revision +
               "/ps5/installer/ca/cacert.pem")
        source = {
            "native/ps5/dependencies.json": json.dumps({
                "updateCa": {"url": url, **record},
            }).encode(),
            "native/third_party/ps5sx2_https/SOURCE.json": json.dumps({
                "revision": revision,
                "externalAsset": {
                    "localAtBuild": "assets/update-ca.pem",
                    "upstream": "ps5/installer/ca/cacert.pem",
                    **record,
                },
            }).encode(),
        }
        result = verify_asset_sources({"assets/update-ca.pem": raw}, source)
        self.assertEqual(result["filesCompared"], 1)
        self.assertEqual(result["byteExactFiles"], 0)
        self.assertEqual(result["pinnedExternalFiles"][0]["sha256"], record["sha256"])
        with self.assertRaisesRegex(ValueError, "two committed provenance pins"):
            verify_asset_sources({"assets/update-ca.pem": raw + b"changed"}, source)
        changed = copy.deepcopy(source)
        changed["native/ps5/dependencies.json"] = json.dumps({
            "updateCa": {"url": url, **digest(b"different")},
        }).encode()
        with self.assertRaisesRegex(ValueError, "two committed provenance pins"):
            verify_asset_sources({"assets/update-ca.pem": raw}, changed)
        with self.assertRaisesRegex(ValueError, "provenance records"):
            verify_asset_sources({"assets/update-ca.pem": raw}, {})

    def test_ps5_update_reader_opens_app_mount_before_bounded_read(self):
        source = (Path(__file__).parents[1] / "native/src/update_client.cpp").read_text()
        match = re.search(r"SmallRead read_small_state\(.*?\n\}\n(?=bool read_small)", source, re.S)
        self.assertIsNotNone(match)
        body = match.group(0)
        self.assertIn("open(name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)", body)
        self.assertIn("limit + 1 - used", body)
        self.assertIn("used > 0 && used <= limit", body)
        self.assertNotIn("regular(name", body)
        self.assertNotIn("lstat(", body)

    def test_update_recovery_reads_owned_files_without_metadata_only_gate(self):
        source = (Path(__file__).parents[1] / "native/src/update_client.cpp").read_text()
        recovery = re.search(r"bool update_recovery_pending\(.*?\n\}\n(?=\nbool take_update_result)",
                             source, re.S)
        self.assertIsNotNone(recovery)
        body = recovery.group(0)
        self.assertIn("read_small_state(manifest_path", body)
        self.assertIn("journal_state == SmallRead::missing", body)
        self.assertNotIn("regular(manifest_path)", body)
        self.assertNotIn("regular(journal)", body)

    def test_only_exact_license_newline_difference_is_recorded(self):
        result = verify_asset_sources({"LICENSE": b"fixture license\r\n"}, {"LICENSE": b"fixture license\n"})
        self.assertEqual(result["byteExactFiles"], 0)
        notice = result["lineEndingOnlyNotices"][0]
        self.assertEqual(notice["packaged"], digest(b"fixture license\r\n"))
        self.assertEqual(notice["source"], digest(b"fixture license\n"))
        with self.assertRaisesRegex(ValueError, "differs"):
            verify_asset_sources({"LICENSE": b"changed license\r\n"}, {"LICENSE": b"fixture license\n"})
        with self.assertRaisesRegex(ValueError, "differs"):
            verify_asset_sources({"assets/CREDITS.md": b"credits\r\n"}, {"assets/CREDITS.md": b"credits\n"})

    def test_unmapped_program_file_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "lacks corresponding"):
            verify_asset_sources({"unknown.dat": b"fixture"}, {"unknown.dat": b"fixture"})

    def test_room_links_work_in_full_release_and_app_only_zip(self):
        version = "v0.1.0-beta.1"
        members = public_guides("See [room setup](ROOM.md).", b"fixture room guide", version)
        self.assertEqual(set(members), {"README.md", "ROOM.md", "PPSA99541/START-HERE.md"})
        target = re.search(r"\]\(([^)]+)\)", members["README.md"].decode()).group(1)
        resolved = posixpath.normpath(target)
        self.assertIn(resolved, members)
        self.assertEqual(members[resolved], b"fixture room guide")
        app_target = re.search(r"\]\(([^)]+)\)", members["PPSA99541/START-HERE.md"].decode()).group(1)
        self.assertEqual(app_target, "https://github.com/Iswebro/console-shelf-ps5/blob/" + version + "/docs/ROOM.md")
        self.assertNotIn("../", app_target)

    def test_source_publication_corrects_docs_without_changing_program_inputs(self):
        private_notice = (b"Console Shelf source and relink kit (development build)\n"
                          b"This is a private preparation artifact. Publication still requires the full\n"
                          b"hardware and launch acceptance in docs/PROTOTYPE-PLAN.md.")
        original = {"SOURCE-KIT.txt": private_notice,
                    "README.md": b"# Shelf\nThis prototype uses an artwork fallback and does not play video.\n",
                    "source.cpp": b"frozen application source\x00\xff", "objects/main.o": b"frozen object",
                    "libraries/libSDL2.a": b"frozen library", "assets/room/intros/ps2.mpg": b"frozen media"}
        before = original.copy()
        documents = public_source_documents(original, "v0.1.0-alpha.1")
        self.assertEqual(original, before)
        self.assertEqual(set(documents), {"README.md", "SOURCE-KIT.txt"})
        self.assertNotIn(b"does not play video", documents["README.md"])
        self.assertIn(b"historical", documents["README.md"])
        published = {**original, **documents}
        for name in original.keys() - documents.keys():
            self.assertEqual(published[name], before[name])
        manifest = {"files": [{"path": name, **digest(raw)} for name, raw in published.items()]}
        verify_source_members({**published, "SOURCE-KIT-MANIFEST.json": b"fixture metadata"}, manifest)
        current = public_source_documents({**original, "README.md": b"# Shelf\nNative intro video is supported.\n"}, "v0.1.0-alpha.1")
        self.assertIn(b"Native intro video is supported.", current["README.md"])
        self.assertNotIn(b"stale no-video", current["SOURCE-KIT.txt"])
        with self.assertRaisesRegex(ValueError, "publication notice"):
            public_source_documents({**original, "SOURCE-KIT.txt": b"already published or unexpected"}, "v0.1.0-alpha.1")


if __name__ == "__main__":
    unittest.main()
