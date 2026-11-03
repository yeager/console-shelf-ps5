# SPDX-License-Identifier: GPL-3.0-only
"""Prepare stable public assets after explicit hardware checks. Never publishes or contacts a console."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid
import zipfile

from ps5_common import CACHE, ROOT, member_path, sha256
from prepare_store_zip import create_store_zip
from verify_ps5_build import require, source_fingerprint, verify_recorded_hashes

REQUIRED_CHECKS = ("ui_navigation", "ui_settings", "circle_close", "settings_persist", "restart_reopen",
                   "owner_support_l3_r3", "auto_update_install_relaunch", "update_preserves_settings",
                   "update_preserves_external_content",
                   "emulator_tile_batch_create", "emulator_tile_update", "emulator_tile_remove_all",
                   "shadowmount_close_refresh", "psxs5_menu", "ps5sx2_menu", "ps5sx3_menu",
                   "retroarch_direct_game", "shared_retroarch_tile_selected_library")
ROUTES = ("PSXS5: standalone emulator library", "PS5SX2: standalone emulator library",
          "PS5SX3: standalone emulator library", "RetroArch: selected Nintendo 64 game",
          "Shared RetroArch Home shortcut: matching console library in isShelf")


def require_public_build(build):
    require(build.get("ownerSupportControl", False) is True,
            "Stable release builds must retain physical L3+R3 owner support")
    require(build.get("uiTestBridge", False) is False,
            "Private UI test control builds cannot become public release inputs")
    require(build.get("ownedTileWrites", False) is True,
            "Stable release builds must include the exact-owned-source tile manager")
    contract = build.get("publicImportContract")
    require(isinstance(contract, dict) and contract.get("ownerSupportControl") is True and
            contract.get("uiTestBridge") is False and
            contract.get("ownedTileWrites") is True and contract.get("baselineImportCount") == 243,
            "Stable release build lacks the reviewed public import contract")


def validate_release_version(version):
    require(re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+(?:-(?:alpha|beta)\.[0-9]+)?", version) is not None,
            "Use an explicit semantic release version")
    return version


def cache_path(relative):
    normalized = str(relative).replace("\\", "/")
    member_path(normalized)
    path = (ROOT / normalized).resolve()
    require(path.is_relative_to((ROOT / ".cache").resolve()), "Artifact path must stay inside the project cache")
    return path


def digest(raw):
    return {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def check(raw, record, label):
    require(digest(raw) == {key: record[key] for key in ("bytes", "sha256")}, label + " changed")


def verify_console_manifest(files, transfer):
    records = transfer.get("files", [])
    for name in files:
        member_path(name)
    for entry in records:
        member_path(entry["path"])
    readback = {entry["path"]: {key: entry[key] for key in ("bytes", "sha256")} for entry in records}
    require(len(readback) == len(records) == len(files), "Console receipt lacks a unique full file ledger")
    require(readback == {name: {key: entry[key] for key in ("bytes", "sha256")} for name, entry in files.items()},
            "Console readback ledger differs from the packaged files")
    require(re.search(r"\bdisabled\b", transfer.get("selfMode", ""), re.IGNORECASE) is not None,
            "Console receipt must confirm raw SELF transfers")


def verify_source_members(kit_data, kit_manifest):
    entries = kit_manifest["files"]
    declared = {entry["path"] for entry in entries}
    require(len(declared) == len(entries) and declared == set(kit_data) - {"SOURCE-KIT-MANIFEST.json"},
            "Source manifest has duplicate, missing or undeclared members")
    for entry in entries:
        member_path(entry["path"])
        check(kit_data[entry["path"]], entry, "Source member " + entry["path"])


def verify_asset_sources(app_data, kit_data):
    mapped = {"sce_sys/param.json": "native/ps5/param.json",
              "sce_sys/pic0.dds": "native/ps5/sce_sys/pic0.dds",
              "sce_sys/pic1.dds": "native/ps5/sce_sys/pic1.dds",
              "licenses/cJSON-MIT.txt": "native/third_party/cjson/LICENSE",
              "licenses/pl_mpeg-source-with-MIT-license.h": "native/third_party/pl_mpeg/pl_mpeg.h"}
    for name in ("SDL2-PS5.txt", "libsamplerate-0.2.2.txt", "LGPL-2.1.txt", "LLVM-libcxx.txt",
                 "Mbed-TLS-3.6.7.txt", "PS5SX2-Swordpdf-NOTICE.txt"):
        mapped["licenses/" + name] = "native/third_party/licenses/" + name
    for name in ("stb_image", "stb_image_write", "stb_truetype"):
        mapped["licenses/" + name + "-source-with-license.h"] = "native/third_party/stb/" + name + ".h"
    generated = {"eboot.bin", "sce_module/libc.prx", "sce_sys/icon0.png", "build-source.json", "START-HERE.md"}
    compared, line_endings, pinned_external = [], [], []
    for name, raw in app_data.items():
        if name in generated:
            continue
        if name == "assets/update-ca.pem":
            dependency_path = "native/ps5/dependencies.json"
            provenance_path = "native/third_party/ps5sx2_https/SOURCE.json"
            require(dependency_path in kit_data and provenance_path in kit_data,
                    "Update CA provenance records are absent from the source kit")
            dependency = json.loads(kit_data[dependency_path]).get("updateCa", {})
            provenance = json.loads(kit_data[provenance_path])
            external = provenance.get("externalAsset", {})
            revision = provenance.get("revision")
            expected_url = ("https://raw.githubusercontent.com/Swordpdf/PS5SX2/" + str(revision) +
                            "/ps5/installer/ca/cacert.pem")
            observed = digest(raw)
            require(external.get("localAtBuild") == name and
                    external.get("upstream") == "ps5/installer/ca/cacert.pem" and
                    dependency.get("url") == expected_url and
                    dependency.get("bytes") == external.get("bytes") == observed["bytes"] and
                    dependency.get("sha256") == external.get("sha256") == observed["sha256"],
                    "Packaged update CA differs from its two committed provenance pins")
            pinned_external.append({"path": name, **observed, "url": expected_url,
                                    "provenance": [dependency_path, provenance_path]})
            continue
        source = mapped.get(name, name if name.startswith(("assets/", "native/assets/fonts/")) or
                            name in ("LICENSE", "THIRD_PARTY.md") else None)
        require(source is not None and source in kit_data, "Packaged file lacks corresponding source: " + name)
        if raw != kit_data[source]:
            # The existing Windows checkout carried LICENSE as CRLF; the Git
            # source snapshot stores LF. Keep both original hashes and report
            # this exact, bounded difference rather than rewriting either file.
            require(name == "LICENSE" and raw.replace(b"\r\n", b"\n") == kit_data[source].replace(b"\r\n", b"\n"),
                    "Packaged asset or notice differs from source: " + name)
            line_endings.append({"path": name, "difference": "CRLF versus LF only; content unchanged",
                                 "packaged": digest(raw), "source": digest(kit_data[source])})
        compared.append(name)
    return {"filesCompared": len(compared) + len(pinned_external),
            "byteExactFiles": len(compared) - len(line_endings),
            "pinnedExternalFiles": pinned_external,
            "lineEndingOnlyNotices": line_endings}


def packaged_icon_source(kit_data):
    manifest = json.loads(kit_data["native/ps5/presentation-assets.json"])
    source = manifest.get("icon", {}).get("source", "assets/shelf-icon.png")
    require(source in ("assets/shelf-icon.png", "assets/branding/isshelf-home-icon.png"),
            "Unexpected packaged icon source")
    return source


def verify_generated_icon(app_data, kit_data):
    source_name = packaged_icon_source(kit_data)
    source = ROOT / source_name
    require(source.read_bytes() == kit_data[source_name], "Logo generator input differs from source")
    for name in ("tools/prepare-package-art.cpp", "native/third_party/stb/stb_image.h",
                 "native/third_party/stb/stb_image_write.h"):
        require((ROOT / name).read_bytes() == kit_data[name], "Logo generator source differs from the source kit")
    directory = CACHE / "release-input-audits" / ("icon-" + uuid.uuid4().hex[:12])
    directory.mkdir(parents=True, exist_ok=False)
    output = directory / "icon0.png"
    tool = ROOT / ".cache/package-tools/prepare-package-art"
    arguments = [source, output]
    if tool.with_suffix(".exe").exists():
        tool = tool.with_suffix(".exe")
        if Path("/mnt/c").is_dir():
            arguments = [subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip() for path in arguments]
    require(tool.is_file(), "Build tools/build-package-art.js before checking the generated icon")
    subprocess.run([str(tool), *(str(path) for path in arguments)], check=True)
    require(output.read_bytes() == app_data["sce_sys/icon0.png"], "Generated tile icon differs from the console-tested icon")
    return directory


def public_install_notice(guide, version, installed_name):
    legacy = ("Preparation guide for the first prototype. A public binary has not been published yet; see the "
              "[acceptance status](PROTOTYPE-PLAN.md). New builds are named **isShelf (UI test)**; "
              "earlier console-tested builds used **Console Shelf (UI test)**.")
    published = re.compile(r"^Native PS5 prototype v[0-9]+\.[0-9]+\.[0-9]+(?:-(?:alpha|beta)\.[0-9]+)?, "
                           r"tested on firmware \*\*12\.20\*\*\. The app appears as \*\*[^*\r\n]+\*\*\.$", re.M)
    matches = list(published.finditer(guide))
    require(guide.count(legacy) + len(matches) == 1, "Exactly one install-guide release notice is required")
    old = legacy if legacy in guide else matches[0].group()
    notice = ("Native PS5 prototype " + version + ", tested on firmware **12.20**. "
              "The app appears as **" + installed_name + "**.")
    return guide.replace(old, notice, 1)


def public_guides(guide, room, version):
    room_url = ("https://github.com/Iswebro/console-shelf-ps5/blob/" +
                version + "/docs/ROOM.md")
    return {"README.md": guide.encode("utf-8"), "ROOM.md": room,
            "PPSA99541/START-HERE.md": guide.replace("](ROOM.md)", "](" + room_url + ")").encode("utf-8")}


def public_source_documents(kit_data, version):
    """Return publication-only documentation changes; leave frozen build inputs alone."""
    instructions = kit_data["SOURCE-KIT.txt"].decode("utf-8")
    old = ("This is a private preparation artifact. Publication still requires the full\n"
           "hardware and launch acceptance in docs/PROTOTYPE-PLAN.md.")
    require(old in instructions, "Source kit publication notice was not found")
    instructions = instructions.replace(old,
        "Published prototype " + version + ". The matching executable has the owner\n"
        "hardware checks listed in the release's HARDWARE-CHECKS.json. Other routes\n"
        "remain experimental. Rebuilding or changing the program needs new tests.")
    for name in ("Console Shelf", "isShelf"):
        instructions = instructions.replace(name + " source and relink kit (development build)",
                                            "isShelf " + version + " source and relink kit")
    instructions += ("\nProject documents retain the compiled-source snapshot. Publication notes\n"
                     "and any necessary README corrections, together with document hashes,\n"
                     "are recorded in SOURCE-KIT-MANIFEST.json.\n"
                     "The release hardware checks record later owner acceptance of the exact program.\n")
    readme = kit_data["README.md"].decode("utf-8")
    readme = readme.replace("This prototype uses an artwork fallback and does not play video.",
        "The prototype includes original Shelf stills and silent intro animations, with personal overrides.")
    note = ("> Source snapshot for isShelf " + version + ". The program code, build inputs and\n"
            "> bundled media are unchanged from the compiled revision. Current release hardware\n"
            "> checks are recorded in SOURCE-KIT-MANIFEST.json. The project status below is\n"
            "> historical; publication-only documentation changes are recorded in that manifest.\n\n")
    return {"SOURCE-KIT.txt": instructions.encode("utf-8"), "README.md": (note + readme).encode("utf-8")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--acceptance", type=Path, help="Reviewed owner hardware-check JSON")
    parser.add_argument("--verify-inputs-only", action="store_true", help="Audit existing app/source inputs; no release or hardware claim")
    package_version = json.loads((ROOT / "package.json").read_text(encoding="utf-8"))["version"]
    parser.add_argument("--version", default="v" + package_version,
                        help="Release tag (defaults to the exact package.json production version)")
    args = parser.parse_args()
    require(not (args.verify_inputs_only and args.acceptance), "Input auditing does not consume a hardware acceptance file")
    validate_release_version(args.version)
    if not args.verify_inputs_only:
        require(args.acceptance is not None, "Reviewed owner hardware-check JSON is required")
        accepted = json.loads(args.acceptance.read_text(encoding="utf-8"))
        require(accepted.get("schema") == 1 and accepted.get("firmware") == "12.20", "Missing target hardware evidence")
        checks = accepted.get("checks", {})
        pending = [name for name in REQUIRED_CHECKS if checks.get(name) is not True]
        require(not pending, "Hardware checks still pending: " + ", ".join(pending))
        require(not subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT).strip(),
                "Commit reviewed source and documentation before release preparation")

    package = json.loads((CACHE / "test-packages/installed-current.json").read_text(encoding="utf-8"))
    kit = json.loads((CACHE / "source-kits/latest.json").read_text(encoding="utf-8"))
    require(package["sourceCommit"] == kit["sourceCommit"], "App and complete source kit have different revisions")
    build = verify_recorded_hashes(CACHE / "build")
    require_public_build(build)
    require(build["self"]["sha256"] == kit["applicationSelf"]["sha256"], "Source kit refers to a different executable")
    if not args.verify_inputs_only:
        require(accepted.get("selfSHA256") == build["self"]["sha256"], "Hardware checks refer to a different executable")
    require(kit["coldRelinkTest"].startswith("passed;"), "Matching private source kit lacks an exact cold relink")
    cold = json.loads(cache_path(kit["coldRelinkProof"]).read_text(encoding="utf-8"))
    require(cold.get("status") == "passed" and cold.get("exitCode") == 0 and cold.get("self") == build["self"] and
            cold["sourceKit"]["archiveSHA256"] == kit["archiveSHA256"] and
            cold["sourceKit"]["sourceCommit"] == package["sourceCommit"] and cold.get("freshCompilerPrefix") is True and
            cold.get("converterRebuilt") is True and cold.get("applicationRecompiled") is False and
            cold.get("consoleContacted") is False, "Exact cold relink proof is not bound to this archive")
    source = cache_path(package["directory"])
    manifest = json.loads((source / "manifest.json").read_text(encoding="utf-8"))
    require(manifest["titleId"] == "PPSA99541" and manifest["sourceCommit"] == package["sourceCommit"],
            "Private app identity differs")
    if not args.verify_inputs_only:
        require(accepted.get("sourceCommit") == package["sourceCommit"] and
                accepted.get("packageManifestSHA256") == sha256(source / "manifest.json"),
                "Hardware checks refer to a different complete app manifest")
    files = {entry["path"]: entry for entry in manifest["files"]}
    require(len(files) == len(manifest["files"]) and "START-HERE.md" in files, "Invalid private app manifest")
    transfer = json.loads(cache_path(package["consoleReceipt"]).read_text(encoding="utf-8"))
    require(transfer["sourceCommit"] == package["sourceCommit"] and
            transfer.get("finalFullReadbackFiles") == len(files) and transfer.get("preferencesUnchanged") is True,
            "Matching complete console readback receipt is required")
    verify_console_manifest(files, transfer)
    app_data = {}
    for name, entry in files.items():
        member_path(name)
        path = (source / "PPSA99541" / name).resolve()
        require(path.is_relative_to((source / "PPSA99541").resolve()), "App member escapes its folder")
        raw = path.read_bytes()
        check(raw, entry, name)
        app_data[name] = raw
    check(app_data["eboot.bin"], build["self"], "Packaged program")
    require(hashlib.sha256(app_data["sce_module/libc.prx"]).hexdigest() ==
            "e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036", "Clean-room runtime changed")
    fingerprint = json.loads(app_data["build-source.json"])
    require(fingerprint == source_fingerprint(), "Compiled inputs differ from the release source tree")
    runtime = manifest["launcherRuntime"]
    require(runtime["sha256"] == kit["launcherRuntime"]["sha256"] == build["embeddedLaunchHelper"]["sha256"],
            "Embedded helper differs between app, source kit and build")
    require(runtime["runtimeMode"] == "standard-sdk" and runtime["permissionChanges"] is True,
            "This hardware acceptance is for the explicitly selected standard-SDK variant")

    source_archive = cache_path(kit["directory"]) / "Console-Shelf-Source-and-Relink.zip"
    require(source_archive.stat().st_size == kit["archiveBytes"] and sha256(source_archive) == kit["archiveSHA256"],
            "Complete source kit archive changed")
    with zipfile.ZipFile(source_archive) as archive:
        require(archive.testzip() is None, "Source kit ZIP integrity failed")
        require(len(archive.namelist()) == len(set(archive.namelist())) == kit["files"], "Duplicate source kit members")
        kit_data = {}
        for name in archive.namelist():
            member_path(name)
            require(name.startswith("Console-Shelf/"), "Unexpected source-kit prefix")
            kit_data[name.removeprefix("Console-Shelf/")] = archive.read(name)
    kit_manifest = json.loads(kit_data["SOURCE-KIT-MANIFEST.json"])
    require(kit_manifest["sourceCommit"] == package["sourceCommit"], "Source manifest revision differs")
    require(kit_manifest["applicationSelf"] == build["self"], "Source manifest program differs")
    verify_source_members(kit_data, kit_manifest)
    for name, expected in fingerprint.items():
        require(hashlib.sha256(kit_data[name]).hexdigest() == expected, "Corresponding compiled source differs")
    compared = verify_asset_sources(app_data, kit_data)
    audit_directory = verify_generated_icon(app_data, kit_data)
    if args.verify_inputs_only:
        audit = {"scope": "Artifact input audit only; physical hardware checks were not evaluated",
                 "sourceCommit": package["sourceCommit"], "applicationSelf": build["self"],
                 "consoleReadbackMembers": len(files), "sourceAssets": compared,
                 "sourceArchiveMembers": len(kit_data), "generatedIconReproduced": True,
                 "releaseAssetsWritten": False, "consoleContacted": False, "publicationAuthorizedByThisAudit": False}
        (audit_directory / "input-audit.json").write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({**audit, "auditDirectory": str(audit_directory.relative_to(ROOT))}, indent=2))
        return

    # Publish only closed, non-personal fields; never copy free-form owner logs or notes.
    evidence = {"schema": 1, "firmware": "12.20", "selfSHA256": build["self"]["sha256"],
                "compiledSourceCommit": package["sourceCommit"],
                "testedManifestSHA256": sha256(source / "manifest.json"),
                "ownerSupportControl": True, "uiTestBridge": False, "ownedTileWrites": True,
                "publicImportContract": build["publicImportContract"],
                "checks": {name: True for name in REQUIRED_CHECKS}, "verifiedRoutes": list(ROUTES),
                "basis": "Owner console reports; host checks and raw readbacks recorded separately",
                "limitations": ["Opening can show black before the grid", "Standalone emulators open their shelves",
                                "Other dedicated-emulator and native-title routes remain experimental",
                                "Discovery requires the existing console-local FTP service"]}
    # Preserve code, objects, libraries, media and upstream archives byte-for-byte.
    # Only publication documents and their manifest change from the cold-tested baseline.
    document_updates = public_source_documents(kit_data, args.version)
    document_changes = [{"path": name, "baseline": digest(kit_data[name]), "published": digest(raw)}
                        for name, raw in document_updates.items()]
    kit_data.update(document_updates)
    kit_manifest.update({"privatePreparation": False, "releaseVersion": args.version,
                         "hardwareAcceptance": evidence, "coldRelinkBaselineArchiveSHA256": kit["archiveSHA256"],
                         "publicationDocumentChanges": document_changes})
    for entry in kit_manifest["files"]:
        if entry["path"] in document_updates:
            entry.update(digest(kit_data[entry["path"]]))
    kit_data["SOURCE-KIT-MANIFEST.json"] = (json.dumps(kit_manifest, indent=2) + "\n").encode()

    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8]
    output = CACHE / "releases" / (args.version + "-" + stamp)
    output.mkdir(parents=True, exist_ok=False)
    source_zip = output / ("isShelf-" + args.version + "-Source-and-Relink.zip")
    with zipfile.ZipFile(source_zip, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=7) as target:
        for name, raw in kit_data.items():
            target.writestr("isShelf/" + name, raw)
    source_info = {"file": source_zip.name, "bytes": source_zip.stat().st_size, "sha256": sha256(source_zip)}

    guide = (ROOT / "docs/PS5-INSTALL.md").read_text(encoding="utf-8")
    installed_name = json.loads(app_data["sce_sys/param.json"])["localizedParameters"]["en-US"]["titleName"]
    guide = public_install_notice(guide, args.version, installed_name)
    guide = guide.replace("from this repository's release page when available", "from this release")
    guide += "\n## Verified launch routes\n\n" + "\n".join("- " + route for route in ROUTES)
    guide += ("\n\nProgram source: https://github.com/Iswebro/console-shelf-ps5/tree/" + package["sourceCommit"] +
              "\n\nThe matching complete source/object/relink kit is supplied beside this download. "
              "Licenses and credits are included. HARDWARE-CHECKS.json records this build's checked scope.\n")
    guides = public_guides(guide, (ROOT / "docs/ROOM.md").read_bytes(), args.version)
    app_data["START-HERE.md"] = guides.pop("PPSA99541/START-HERE.md")
    public_files = [{"path": name, **digest(raw)} for name, raw in sorted(app_data.items())]
    public_manifest = {"schema": 1, "version": args.version, "titleId": "PPSA99541",
                       "compiledSourceCommit": package["sourceCommit"], "launcherRuntime": runtime,
                       "ownerSupportControl": True, "uiTestBridge": False, "ownedTileWrites": True,
                       "publicImportContract": build["publicImportContract"],
                       "hardwareChecks": evidence, "sourceKit": source_info, "files": public_files,
                       "sourceAssetComparison": compared, "generatedIconReproduced": True,
                       "publicationToolsCommit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                       "programComparison": "All hardware-tested files unchanged except START-HERE.md documentation"}
    internal = output / "internal"
    internal.mkdir()
    app_zip = internal / ("isShelf-PS5-" + args.version + "-full.zip")
    app_extras = {**guides,
                  "MANIFEST.json": (json.dumps(public_manifest, indent=2) + "\n").encode("utf-8"),
                  "HARDWARE-CHECKS.json": (json.dumps(evidence, indent=2) + "\n").encode("utf-8")}
    with zipfile.ZipFile(app_zip, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as target:
        for name, raw in sorted(app_data.items()):
            target.writestr("PPSA99541/" + name, raw)
        for name, raw in app_extras.items():
            target.writestr(name, raw)
    for path, expected in ((source_zip, {"isShelf/" + name: raw for name, raw in kit_data.items()}),
                           (app_zip, {**{"PPSA99541/" + name: raw for name, raw in app_data.items()}, **app_extras})):
        with zipfile.ZipFile(path) as completed:
            require(completed.testzip() is None, "Public ZIP integrity failed")
            require(len(completed.namelist()) == len(set(completed.namelist())), "Duplicate public ZIP members")
            require(set(completed.namelist()) == set(expected), "Public ZIP has missing or undeclared members")
            for name, raw in expected.items():
                require(completed.read(name) == raw, "Public archive member changed")
    app_info = {"file": app_zip.name, "bytes": app_zip.stat().st_size, "sha256": sha256(app_zip)}
    store_zip = output / "PPSA99541.zip"
    store_info = create_store_zip(app_zip, store_zip)
    standalone = {
        "MANIFEST.json": app_extras["MANIFEST.json"],
        "HARDWARE-CHECKS.json": app_extras["HARDWARE-CHECKS.json"],
        "INSTALL.md": guides["README.md"],
        "ROOM.md": guides["ROOM.md"],
    }
    standalone_info = []
    for name, raw in standalone.items():
        path = output / name
        path.write_bytes(raw)
        standalone_info.append({"file": name, **digest(raw)})
    public_assets = [store_info, source_info, *standalone_info]
    sums = output / "SHA256SUMS.txt"
    sums.write_text("\n".join(info["sha256"] + "  " + info["file"] for info in public_assets) + "\n",
                    encoding="ascii")
    sums_info = {"file": sums.name, **digest(sums.read_bytes())}
    receipt = {"schema": 1, "version": args.version, "directory": str(output.relative_to(ROOT)),
               "compiledSourceCommit": package["sourceCommit"], "applicationSelf": build["self"],
               "assets": [*public_assets, sums_info], "internalFullRelease": app_info,
               "sourceKitColdRelink": "pending for the public ZIP; original retained-input baseline passed",
               "consoleDocumentReadback": "pending", "published": False}
    (output / "release-preparation.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    (CACHE / "releases/latest.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
