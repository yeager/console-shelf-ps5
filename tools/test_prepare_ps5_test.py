# SPDX-License-Identifier: GPL-3.0-only
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import prepare_ps5_test as prepare


class CompiledTilePinTests(unittest.TestCase):
    def test_exact_packaged_pins_must_exist_in_compiled_app(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle = root / "bundle"
            (bundle / "template").mkdir(parents=True)
            (bundle / "catalog.json").write_bytes(b"catalog-v3")
            (bundle / "template/eboot.bin").write_bytes(b"forwarder-v3")
            digests = [prepare.sha256(bundle / relative) for relative in
                       ("catalog.json", "template/eboot.bin")]
            executable = root / "eboot.elf"
            executable.write_bytes(("prefix" + "|".join(digests) + "suffix").encode())
            pins = prepare.verify_compiled_console_tile_pins(executable, bundle)
            self.assertEqual(set(pins), {"catalog.json", "template/eboot.bin"})

            (bundle / "catalog.json").write_bytes(b"catalog-drift")
            with self.assertRaisesRegex(ValueError, "Compiled console-tile pin differs"):
                prepare.verify_compiled_console_tile_pins(executable, bundle)


if __name__ == "__main__":
    unittest.main()
