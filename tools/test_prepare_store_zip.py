"""Check that the store archive keeps exactly the manifest-verified app bytes."""

import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from prepare_ps5_release import public_guides
from prepare_store_zip import REQUIRED, TITLE_ID, create_store_zip, digest


ROOT = Path(__file__).resolve().parents[1]


def fixture(directory, tamper=False, extra_name=None):
    param = {
        "titleId": TITLE_ID,
        "contentVersion": "00.100.022",
        "localizedParameters": {"en-US": {"titleName": "isShelf"}},
    }
    data = {name: (json.dumps(param).encode() if name == "sce_sys/param.json"
                   else ("fixture:" + name).encode()) for name in REQUIRED}
    if extra_name:
        data[extra_name] = b"personal fixture"
    manifest = {
        "schema": 1,
        "titleId": TITLE_ID,
        "compiledSourceCommit": "a" * 40,
        "files": [{"path": name, "bytes": len(raw), "sha256": digest(raw)}
                  for name, raw in sorted(data.items())],
    }
    source = directory / "full.zip"
    with zipfile.ZipFile(source, "x") as archive:
        archive.writestr("MANIFEST.json", json.dumps(manifest))
        archive.writestr("PS5-INSTALL.md", "Full release guide")
        for name, raw in data.items():
            archive.writestr(TITLE_ID + "/" + name,
                             b"wrong bytes" if tamper and name == "eboot.bin" else raw)
    return source, data


class StoreZipTest(unittest.TestCase):
    def test_installed_guide_links_to_tagged_room_document(self):
        guides = public_guides("See [room](ROOM.md).", b"Room", "v0.1.0-beta.1")
        installed = guides["PPSA99541/START-HERE.md"].decode()
        self.assertIn("https://github.com/Iswebro/console-shelf-ps5/blob/v0.1.0-beta.1/docs/ROOM.md",
                      installed)
        self.assertNotIn("../ROOM.md", installed)

    def test_preserves_only_verified_app_folder(self):
        with tempfile.TemporaryDirectory(dir=ROOT / ".cache") as temporary:
            directory = Path(temporary)
            source, data = fixture(directory)
            output = directory / "PPSA99541.zip"
            receipt = create_store_zip(source, output)
            self.assertEqual(receipt["appFiles"], len(data))
            self.assertEqual(receipt["contentVersion"], "00.100.022")
            self.assertEqual(receipt["sha256"], digest(output.read_bytes()))
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(set(archive.namelist()),
                                 {TITLE_ID + "/" + name for name in data})
                for name, raw in data.items():
                    self.assertEqual(archive.read(TITLE_ID + "/" + name), raw)

    def test_rejects_changed_app_byte_without_output(self):
        with tempfile.TemporaryDirectory(dir=ROOT / ".cache") as temporary:
            directory = Path(temporary)
            source, _ = fixture(directory, tamper=True)
            output = directory / "PPSA99541.zip"
            with self.assertRaisesRegex(ValueError, "manifest check"):
                create_store_zip(source, output)
            self.assertFalse(output.exists())

    def test_rejects_personal_data_path_without_output(self):
        for personal in ("settings.ini", "emulators/PS5SX2/config.ini",
                         "games/title.iso", "roms/title.bin", "bios/firmware.bin",
                         "saves/card.bin", "artwork/custom.png", "keys/account.key",
                         "userdata/profile.json"):
            with self.subTest(personal=personal), \
                    tempfile.TemporaryDirectory(dir=ROOT / ".cache") as temporary:
                directory = Path(temporary)
                source, _ = fixture(directory, extra_name=personal)
                output = directory / "PPSA99541.zip"
                with self.assertRaisesRegex(ValueError, "personal-data path"):
                    create_store_zip(source, output)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
