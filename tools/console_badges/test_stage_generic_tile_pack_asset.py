# SPDX-License-Identifier: GPL-3.0-only
"""Release package pack staging preserves exact tracked bytes."""

from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
import tempfile

import stage_generic_tile_pack_asset as asset_stager
from stage_generic_tile_pack_asset import (
    DEFAULT_SOURCE, INDEX_BYTES, INDEX_SHA256, PACK_BYTES, PACK_SHA256,
    stage,
)


def expect_reject(call):
    try:
        call()
    except (ValueError, FileExistsError):
        return
    raise AssertionError("Tampered or replayed pack was accepted")


with tempfile.TemporaryDirectory(prefix="isshelf-pack-asset-") as temporary:
    root = Path(temporary)
    bundle = root / "bundle"
    bundle.mkdir()
    synced_directories = []
    original_sync = asset_stager._fsync_directory

    def tracked_sync(path):
        synced_directories.append(path)
        original_sync(path)

    asset_stager._fsync_directory = tracked_sync
    try:
        result = stage(bundle)
    finally:
        asset_stager._fsync_directory = original_sync
    assert synced_directories == [bundle, bundle / "pack-v1"]
    assert result == {
        "relativeRoot": "assets/console-tiles/pack-v1",
        "packBytes": PACK_BYTES, "packSha256": PACK_SHA256,
        "indexBytes": INDEX_BYTES, "indexSha256": INDEX_SHA256,
        "uniqueBlobs": 207, "preparedTitles": 51, "adoptedTitles": 1,
    }
    for name, size, digest in (
        ("pack.bin", PACK_BYTES, PACK_SHA256),
        ("index.json", INDEX_BYTES, INDEX_SHA256),
    ):
        copied = (bundle / "pack-v1" / name).read_bytes()
        assert len(copied) == size
        assert hashlib.sha256(copied).hexdigest() == digest
        assert copied == (DEFAULT_SOURCE / name).read_bytes()
    assert {p.name for p in (bundle / "pack-v1").iterdir()} == {"pack.bin", "index.json"}
    expect_reject(lambda: stage(bundle))

    for corrupt in ("pack.bin", "index.json"):
        source = root / ("changed-" + corrupt)
        source.mkdir()
        for name in ("pack.bin", "index.json"):
            shutil.copyfile(DEFAULT_SOURCE / name, source / name)
        raw = bytearray((source / corrupt).read_bytes())
        raw[-1] ^= 1
        (source / corrupt).write_bytes(raw)
        target = root / ("rejected-" + corrupt)
        target.mkdir()
        expect_reject(lambda source=source, target=target: stage(target, source))
        assert list(target.iterdir()) == []

    # A sparse oversized input is rejected on its metadata before the pack
    # verifier can allocate it or construct any destination path.
    for oversized in ("pack.bin", "index.json"):
        source = root / ("oversized-" + oversized)
        source.mkdir()
        for name in ("pack.bin", "index.json"):
            shutil.copyfile(DEFAULT_SOURCE / name, source / name)
        with (source / oversized).open("r+b") as file:
            file.truncate((PACK_BYTES if oversized == "pack.bin" else INDEX_BYTES) + 1)
        target = root / ("oversized-target-" + oversized)
        target.mkdir()
        expect_reject(lambda source=source, target=target: stage(target, source))
        assert list(target.iterdir()) == []

    # A failed earlier write must remain intact; rerunning cannot complete or
    # replace a partial child under the create-only contract.
    partial_root = root / "partial-replay"
    partial_root.mkdir()
    partial = partial_root / "pack-v1"
    partial.mkdir()
    (partial / "pack.bin").write_bytes(b"partial write")
    expect_reject(lambda: stage(partial_root))
    assert (partial / "pack.bin").read_bytes() == b"partial write"
    assert {p.name for p in partial.iterdir()} == {"pack.bin"}

    absent = root / "absent"
    expect_reject(lambda: stage(absent))

print("PASS: exact pack, bounded oversized inputs, partial replay and tamper holds")
