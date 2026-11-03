"""Create a store-only isShelf ZIP from a verified full public release ZIP.

The source release keeps its root-level guides and evidence. Stores receive only
the PPSA99541 app folder, with every member checked against MANIFEST.json.
This tool never publishes or contacts a console.
"""

import argparse
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path, PurePosixPath


TITLE_ID = "PPSA99541"
REQUIRED = {
    "eboot.bin",
    "sce_sys/param.json",
    "sce_sys/icon0.png",
    "sce_sys/pic0.dds",
    "sce_sys/pic1.dds",
    "START-HERE.md",
    "LICENSE",
    "THIRD_PARTY.md",
}
PROGRAM_EXACT = {"eboot.bin", "LICENSE", "THIRD_PARTY.md", "START-HERE.md", "build-source.json"}
PROGRAM_PREFIXES = ("assets/", "licenses/", "native/assets/fonts/", "sce_module/", "sce_sys/")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    value = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def safe_member(name):
    if not isinstance(name, str):
        return False
    path = PurePosixPath(name)
    return (name == path.as_posix() and not path.is_absolute() and len(path.parts) > 0
            and all(part not in ("", ".", "..") for part in path.parts)
            and "\\" not in name and not name.endswith("/"))


def program_member(name):
    """The in-app updater may replace program material, never personal data."""
    return safe_member(name) and (name in PROGRAM_EXACT or any(
        name.startswith(prefix) and len(name) > len(prefix) for prefix in PROGRAM_PREFIXES))


def app_members(archive):
    names = archive.namelist()
    require(len(names) == len(set(names)), "Release ZIP has duplicate members")
    require("MANIFEST.json" in names, "Release ZIP lacks MANIFEST.json")
    manifest = json.loads(archive.read("MANIFEST.json"))
    require(manifest.get("schema") == 1 and manifest.get("titleId") == TITLE_ID,
            "Release manifest is not for isShelf")
    source_commit = manifest.get("compiledSourceCommit", "")
    require(re.fullmatch(r"[0-9a-f]{40}", source_commit) is not None,
            "Release manifest lacks compiled source commit")
    listed = manifest.get("files")
    require(isinstance(listed, list) and listed, "Release manifest has no app file list")

    expected = {}
    for entry in listed:
        require(isinstance(entry, dict), "Invalid app file entry")
        name = entry.get("path")
        require(safe_member(name), "Unsafe app file path")
        require(program_member(name), "Release app contains a non-program or personal-data path")
        require(name not in expected, "Duplicate app path in manifest")
        require(isinstance(entry.get("bytes"), int) and entry["bytes"] >= 0,
                "Invalid app file size")
        require(isinstance(entry.get("sha256"), str)
                and re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]) is not None,
                "Invalid app file digest")
        expected[name] = entry
    require(REQUIRED.issubset(expected), "Release app is missing required members")
    actual = {name[len(TITLE_ID) + 1:] for name in names
              if name.startswith(TITLE_ID + "/")}
    require(actual == set(expected), "Release app folder differs from manifest")

    data = {}
    for name, entry in expected.items():
        raw = archive.read(TITLE_ID + "/" + name)
        require(len(raw) == entry["bytes"] and digest(raw) == entry["sha256"],
                "Release app member failed manifest check: " + name)
        data[name] = raw
    param = json.loads(data["sce_sys/param.json"])
    require(param.get("titleId") == TITLE_ID, "App param.json has the wrong title ID")
    require(param.get("localizedParameters", {}).get("en-US", {}).get("titleName") == "isShelf",
            "App param.json is not branded isShelf")
    content_version = param.get("contentVersion", "")
    require(re.fullmatch(r"[0-9]{2}\.[0-9]{3}\.[0-9]{3}", content_version) is not None,
            "App param.json has an invalid content version")
    return data, source_commit, content_version


def create_store_zip(release_zip, output_zip):
    release_zip = Path(release_zip).resolve()
    output_zip = Path(output_zip).resolve()
    require(release_zip != output_zip, "Output must differ from the full release ZIP")
    require(not output_zip.exists(), "Output already exists: " + str(output_zip))
    with zipfile.ZipFile(release_zip, "r") as source:
        data, source_commit, content_version = app_members(source)

    try:
        with zipfile.ZipFile(output_zip, "x", compression=zipfile.ZIP_DEFLATED,
                             compresslevel=6) as target:
            for name, raw in sorted(data.items()):
                info = zipfile.ZipInfo(TITLE_ID + "/" + name, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                target.writestr(info, raw, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
        with zipfile.ZipFile(output_zip, "r") as completed:
            require(completed.testzip() is None, "Store ZIP CRC failure")
            require(set(completed.namelist()) == {TITLE_ID + "/" + name for name in data},
                    "Store ZIP contains unexpected members")
            for name, raw in data.items():
                require(completed.read(TITLE_ID + "/" + name) == raw,
                        "Store ZIP readback differs: " + name)
    except Exception:
        output_zip.unlink(missing_ok=True)
        raise

    return {
        "file": output_zip.name,
        "bytes": output_zip.stat().st_size,
        "sha256": file_digest(output_zip),
        "appFiles": len(data),
        "titleId": TITLE_ID,
        "contentVersion": content_version,
        "compiledSourceCommit": source_commit,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release_zip", help="Verified full public release ZIP")
    parser.add_argument("output_zip", help="New store-only PPSA99541.zip path")
    args = parser.parse_args(argv)
    try:
        result = create_store_zip(args.release_zip, args.output_zip)
    except (OSError, ValueError, zipfile.BadZipFile, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
