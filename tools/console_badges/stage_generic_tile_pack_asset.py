# SPDX-License-Identifier: GPL-3.0-only
"""Copy the reviewed 51-title pack into an app bundle, create-only.

The source pack is tracked so a clean release checkout has every package input.
This is host packaging only. It does not enable an installer or contact a PS5.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat

ROOT = Path(__file__).resolve().parents[2]
PACK_LEAF = "pack-v1"
DEFAULT_SOURCE = ROOT / "assets/console-tiles" / PACK_LEAF
PACK_BYTES = 8_694_623
PACK_SHA256 = "505fbfa7f4c6a57e7a66b594cb5881f152268b4953c2182139f6a7ab55d080bd"
INDEX_BYTES = 57_357
INDEX_SHA256 = "66cc20f6018449462fc228d8d9de5966dfc9d12998bf2da9734973b6f7ed8044"


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _read_exact(path: Path, size: int) -> bytes:
    """Reject an oversized or nonregular input before a bounded read."""
    metadata = path.lstat()
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_size != size:
        raise ValueError(f"Generic tile pack file has wrong type or size: {path.name}")
    with path.open("rb") as input_file:
        raw = input_file.read(size + 1)
        if len(raw) != size or input_file.read(1):
            raise ValueError(f"Generic tile pack file changed during read: {path.name}")
    return raw


def _fsync_directory(path: Path) -> None:
    """Persist directory entries where the host OS exposes directory fsync.

    Windows does not support opening a directory through os.open for fsync;
    file fsync and package ZIP readback remain in force there.
    """
    if os.name == "nt":
        return
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    descriptor = os.open(path, flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def verify(source: Path) -> dict:
    """Verify the complete tracked pack and its closed inventory."""
    source = Path(source)
    if not source.is_dir() or source.is_symlink():
        raise ValueError("Exact existing generic tile pack source required")
    entries = list(source.iterdir())
    if ({entry.name for entry in entries} != {"pack.bin", "index.json"} or
            any(not entry.is_file() or entry.is_symlink() for entry in entries)):
        raise ValueError("Generic tile pack source inventory changed")
    pack = _read_exact(source / "pack.bin", PACK_BYTES)
    index_raw = _read_exact(source / "index.json", INDEX_BYTES)
    if _sha(pack) != PACK_SHA256 or _sha(index_raw) != INDEX_SHA256:
        raise ValueError("Reviewed generic tile pack identity differs")
    index = json.loads(index_raw)
    if (index.get("format") != "isshelf-tile-blob-pack/1" or
            index.get("packBytes") != PACK_BYTES or
            index.get("packSha256") != PACK_SHA256 or
            len(index.get("blobs", [])) != 207 or len(index.get("titles", [])) != 52 or
            sum(not row.get("adopted", False) for row in index["titles"]) != 51 or
            sum(row.get("adopted", False) for row in index["titles"]) != 1):
        raise ValueError("Reviewed generic tile pack index differs")
    return {
        "packBytes": PACK_BYTES, "packSha256": PACK_SHA256,
        "indexBytes": INDEX_BYTES, "indexSha256": INDEX_SHA256,
        "uniqueBlobs": 207, "preparedTitles": 51, "adoptedTitles": 1,
    }


def stage(asset_root: Path, source: Path = DEFAULT_SOURCE) -> dict:
    """Stage only exact tracked bytes below an existing bundle.

    A failed copy leaves its partial child for inspection; a later invocation
    cannot silently replace that child. A changed source is rejected before
    creating any destination path.
    """
    source = Path(source)
    asset_root = Path(asset_root)
    if not source.is_dir() or source.is_symlink():
        raise ValueError("Exact existing generic tile pack source required")
    observed = verify(source)
    expected = {
        "packBytes": PACK_BYTES, "packSha256": PACK_SHA256,
        "indexBytes": INDEX_BYTES, "indexSha256": INDEX_SHA256,
        "uniqueBlobs": 207, "preparedTitles": 51, "adoptedTitles": 1,
    }
    if observed != expected:
        raise ValueError("Reviewed generic tile pack identity differs")
    if not asset_root.is_dir() or asset_root.is_symlink():
        raise ValueError("Exact existing asset bundle root required")
    prepared = []
    for name, size, digest in (
        ("pack.bin", PACK_BYTES, PACK_SHA256),
        ("index.json", INDEX_BYTES, INDEX_SHA256),
    ):
        raw = _read_exact(source / name, size)
        if _sha(raw) != digest:
            raise ValueError("Generic tile pack changed after source check")
        prepared.append((name, raw, digest))
    target = asset_root / PACK_LEAF
    target.mkdir(mode=0o755, exist_ok=False)
    _fsync_directory(asset_root)
    for name, raw, digest in prepared:
        file = target / name
        with file.open("xb") as output:
            output.write(raw)
            output.flush()
            os.fsync(output.fileno())
        copied = _read_exact(file, len(raw))
        if copied != raw or _sha(copied) != digest:
            raise ValueError("Copied generic tile pack differs on readback")
    _fsync_directory(target)
    return {"relativeRoot": f"assets/console-tiles/{PACK_LEAF}", **expected}
