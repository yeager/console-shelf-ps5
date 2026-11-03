# SPDX-License-Identifier: GPL-3.0-only
"""Verify complete linked-library sources in the project cache. Never runs them."""
import gzip
import hashlib
import json
import lzma
import bz2
from pathlib import PurePosixPath
import posixpath
import tarfile
from ps5_common import ROOT, CACHE, fetch, member_path, sha256
from verify_ps5_build import require

SOURCE_CACHE = CACHE / "corresponding-source"


def git_hash(kind, data):
    return hashlib.sha1(kind.encode() + b" " + str(len(data)).encode() + b"\0" + data).hexdigest()


def git_tree(files):
    root = {}
    for path, value in files.items():
        node = root
        parts = path.split("/")
        for part in parts[:-1]:
            require(isinstance(node.setdefault(part, {}), dict), "Source file/directory collision")
            node = node[part]
        require(parts[-1] not in node, "Duplicate source path")
        node[parts[-1]] = value

    def encode(node):
        entries = []
        for name, value in node.items():
            directory = isinstance(value, dict)
            mode, digest = ("40000", encode(value)) if directory else value
            key = name.encode("utf-8")
            entries.append((key + (b"/" if directory else b""),
                            mode.encode() + b" " + key + b"\0" + bytes.fromhex(digest)))
        return git_hash("tree", b"".join(raw for _, raw in sorted(entries)))
    return encode(root)


def inspect_archive(path, expected_tree=None):
    # Read past the tar terminator to verify the compression trailer as well.
    expanded = 0
    opener = lzma.open if path.name.endswith(".xz") else bz2.open if path.name.endswith(".bz2") else gzip.open
    with opener(path, "rb") as stream:
        while block := stream.read(1024 * 1024):
            expanded += len(block)
            require(expanded <= 512 * 1024 * 1024, "Source archive expansion limit")
    files, root = {}, None
    with tarfile.open(path, "r:*") as archive:
        for item in archive:
            name = member_path(item.name)
            require(bool(name.parts), "Empty source member")
            if root is None:
                root = name.parts[0]
            require(name.parts[0] == root, "Source archive has multiple top-level folders")
            if item.isdir():
                continue
            require(item.isfile() or item.issym() or item.islnk(), "Special source archive member")
            relative = "/".join(name.parts[1:])
            require(relative and relative not in files, "Duplicate source member")
            require(len(files) < 50000 and item.size <= 64 * 1024 * 1024, "Source member limit")
            if item.issym() or item.islnk():
                require(not PurePosixPath(item.linkname).is_absolute() and "\\" not in item.linkname and
                        ":" not in item.linkname, "Invalid source link")
                target = posixpath.normpath(posixpath.join(posixpath.dirname(item.name), item.linkname)
                                           if item.issym() else item.linkname)
                require(PurePosixPath(target).parts[0] == root, "Source link leaves archive")
            if expected_tree:
                # Git records symlink target bytes, not the symlink's executable permission bits.
                raw = item.linkname.encode() if item.issym() else archive.extractfile(item).read()
                mode = "120000" if item.issym() else "100755" if item.mode & 0o111 else "100644"
                files[relative] = (mode, git_hash("blob", raw))
            else:
                files[relative] = ("", "")
    require(files, "Empty source archive")
    root_hash = git_tree(files) if expected_tree else None
    require(not expected_tree or root_hash == expected_tree, "Source content does not match pinned Git tree")
    return {"members": len(files), "expandedBytes": expanded, "gitTree": root_hash}


def download(record, target):
    # Preserve an interrupted attempt for inspection rather than silently replacing it.
    require(target.exists() or not target.with_name(target.name + ".download").exists(),
            "Interrupted source download exists; inspect it before retrying")
    return fetch(record["url"], target, record["sha256"], record.get("bytes"))


def prepare_sdk_source():
    sdk = json.loads((ROOT / "tools/linked-sources.json").read_text())["sdkRuntime"]
    source = sdk["source"]
    archive = download(source, SOURCE_CACHE / source["file"])
    inspected = inspect_archive(archive, sdk["gitTree"])
    record = {"id": sdk["id"], "version": sdk["version"], "archive": source["file"],
              "bytes": archive.stat().st_size, "sha256": sha256(archive), **inspected}
    return archive, record


def prepare():
    pins = json.loads((ROOT / "tools/linked-sources.json").read_text())
    SOURCE_CACHE.mkdir(parents=True, exist_ok=True)
    results = []
    for library in pins["libraries"]:
        source = library["source"]
        archive = download(source, SOURCE_CACHE / source["file"])
        inspected = inspect_archive(archive, library.get("gitTree"))
        recipes = library.get("recipes")
        if recipes is None and "recipe" in library:
            recipes = [{"id": library["id"], **library["recipe"]}]
        recipe_results = []
        for recipe in recipes or []:
            recipe_name = library["id"] + "-" + recipe["id"] + "-PKGBUILD"
            recipe_record = {**recipe, "url":
                "https://raw.githubusercontent.com/ps5-payload-dev/pacbrew-repo/" +
                pins["pacbrewRelease"] + "/" + recipe["id"] + "/PKGBUILD"}
            recipe_file = download(recipe_record, SOURCE_CACHE / recipe_name)
            require(git_hash("blob", recipe_file.read_bytes()) == recipe["gitBlob"],
                    "PacBrew recipe Git blob mismatch")
            recipe_results.append({"id": recipe["id"], "file": recipe_name,
                                   "sha256": sha256(recipe_file)})
        result = {"id": library["id"], "version": library["version"],
                  "archive": source["file"], "bytes": archive.stat().st_size,
                  "sha256": sha256(archive), **inspected}
        if len(recipe_results) == 1:
            result.update({"recipe": recipe_results[0]["file"],
                           "recipeSHA256": recipe_results[0]["sha256"]})
        elif recipe_results:
            result["recipes"] = recipe_results
        results.append(result)
        print("Verified complete source:", library["id"], inspected["members"], "members", flush=True)
    _, sdk_record = prepare_sdk_source()
    results.append(sdk_record)
    result = {"schema": 1, "sources": results, "upstreamScriptsExecuted": False,
              "settingsOrConsoleChanged": False}
    (SOURCE_CACHE / "source-verification.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


if __name__ == "__main__":
    prepare()
