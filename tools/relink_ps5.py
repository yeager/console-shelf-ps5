# SPDX-License-Identifier: GPL-3.0-only
"""Retain application objects and test relinking with replaceable static libraries.

All outputs stay in .cache/ps5. Does not deploy or execute the resulting PS5 code.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import uuid
import zipfile
from bootstrap_ps5_toolchain import environment, host_options, prepare as prepare_compiler
from build_ps5 import cached_run
from ps5_common import ROOT, CACHE, fetch, prepare_archive, run, sha256
from verify_ps5_build import require, source_fingerprint, verify, verify_recorded_hashes

OBJECTS = ("model", "settings", "ui", "graphics", "codecs", "movie", "library", "filesystem", "metadata", "ftp", "launch_client", "console_tile_manager",
           "host_main", "json", "alloc", "vout", "shims", "lifecycle", "app_crt", "app_cpp_runtime")
LIBRARIES = ("libSDL2.a", "libiconv.a", "libsamplerate.a")
INPUTS = CACHE / "relink-inputs"


def file_record(path):
    return {"bytes": path.stat().st_size, "sha256": sha256(path)}


def checked(path, record):
    require(path.is_file() and file_record(path) == record, "Relink input differs: " + path.name)
    return path


def sdk_stubs(sdk, archive):
    prefix = "ps5-payload-sdk/target/lib/"
    result = {}
    with zipfile.ZipFile(archive) as source:
        for item in source.infolist():
            if item.filename.startswith(prefix) and item.filename.endswith(".so"):
                relative = item.filename[len(prefix):]
                require("/" not in relative, "Unexpected nested SDK stub")
                raw = source.read(item)
                record = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
                checked(sdk / "target/lib" / relative, record)
                result[relative] = record
    require(result and set(result) == {p.name for p in (sdk / "target/lib").glob("*.so")},
            "SDK stub set differs from the pinned release")
    return result


def capture(sdk, pins, inputs):
    require(not inputs.exists(), "Relink snapshot already exists; use --inputs for a new build snapshot")
    build = CACHE / "build"
    verified = verify_recorded_hashes(build)
    require(source_fingerprint() == json.loads((build / "build-source.json").read_text()),
            "Application sources differ from the recorded build")
    ports_archive = fetch(pins["ports"]["url"], CACHE / "downloads/pacbrew-v0.40.2.tar.gz",
                          pins["ports"]["sha256"], pins["ports"]["bytes"])
    libraries = {}
    with tarfile.open(ports_archive, "r|gz") as archive:
        for member in archive:
            name = "opt/ps5-payload-sdk/target/user/homebrew/lib/"
            if member.name.startswith(name) and member.name[len(name):] in LIBRARIES:
                require(member.isfile(), "Library is not a regular archive member")
                library = member.name[len(name):]
                digest = hashlib.sha256()
                with archive.extractfile(member) as stream:
                    while block := stream.read(1024 * 1024):
                        digest.update(block)
                record = {"bytes": member.size, "sha256": digest.hexdigest()}
                checked(CACHE / "ports-v0.40.2/lib" / library, record)
                libraries[library] = record
    require(set(libraries) == set(LIBRARIES), "Pinned ports archive lacks a linked library")
    stubs = sdk_stubs(sdk, CACHE / "downloads/ps5-payload-sdk-v0.42.zip")
    objects = {name + ".o": file_record(build / (name + ".o")) for name in OBJECTS}
    inputs.mkdir(parents=True)
    (inputs / "objects").mkdir()
    (inputs / "libraries").mkdir()
    for name, record in objects.items():
        shutil.copyfile(build / name, inputs / "objects" / name)
        checked(inputs / "objects" / name, record)
    for name, record in libraries.items():
        shutil.copyfile(CACHE / "ports-v0.40.2/lib" / name, inputs / "libraries" / name)
        checked(inputs / "libraries" / name, record)
    shutil.copyfile(build / "build-source.json", inputs / "build-source.json")
    report = json.loads((build / "structural-check.json").read_text())
    record = {"schema": 1, "objects": objects, "libraries": libraries, "sdkStubs": stubs,
              "objectOrder": [name + ".o" for name in OBJECTS], "referenceElf": report["elf"],
              "referenceSelf": report["self"], "sourceFingerprint": file_record(inputs / "build-source.json"),
              "scope": "Application object relink inputs; no firmware compatibility claim"}
    if "embeddedLaunchHelper" in verified:
        helper = verified["embeddedLaunchHelper"]
        record["launcherRuntime"] = {key: helper[key] for key in
                                     ("bytes", "sha256", "runtimeMode", "permissionChanges")}
    (inputs / "link-inputs.json").write_text(json.dumps(record, indent=2) + "\n")
    print("Retained and verified", len(objects), "application objects and three replaceable libraries.", flush=True)


def converter(prefix, env, zlib):
    # Same pinned converter source and host options as build_ps5.py; no app recompile.
    output = CACHE / "relink-tools"
    output.mkdir(parents=True, exist_ok=True)
    compiler = prefix / "usr/bin/clang-18"
    cxx = prefix / "usr/bin/clang++-18"
    host_flags, host_link = host_options(prefix)
    names = ("adler32", "crc32", "deflate", "infback", "inffast", "inflate", "inftrees", "trees", "zutil",
             "compress", "uncompr", "gzclose", "gzlib", "gzread", "gzwrite")
    objects = []
    for name in names:
        obj = output / (name + ".o")
        cached_run([compiler, *host_flags, "-O2", "-DHAVE_UNISTD_H", "-I", zlib, "-c", zlib / (name + ".c"),
                    "-o", obj], [zlib / (name + ".c"), *zlib.glob("*.h")], obj, env)
        objects.append(obj)
    run(["ar", "rcs", output / "libz.a", *objects], env=env)
    runtime = ROOT / "native/third_party/ps5-runtime/tooling/native"
    tool = output / "ps5-native-tool"
    cached_run([cxx, *host_flags, *host_link, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-I", zlib,
                *(runtime / (name + ".cpp") for name in ("native_app_builder", "elf_object", "self_container",
                                                        "sce_module_writer")), output / "libz.a", "-o", tool],
               [*runtime.glob("*.cpp"), *runtime.glob("*.hpp"), zlib / "zlib.h", zlib / "zconf.h",
                output / "libz.a"], tool, env)
    return tool


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture-inputs", action="store_true", help="Retain inputs from the current verified build")
    parser.add_argument("--inputs", type=Path, default=INPUTS, help="Object/library snapshot folder")
    parser.add_argument("--sdl", type=Path, help="Replacement PS5 SDL2 static library")
    parser.add_argument("--libiconv", type=Path, help="Replacement PS5 libiconv static library")
    parser.add_argument("--libsamplerate", type=Path, help="Replacement PS5 libsamplerate static library")
    args = parser.parse_args()
    inputs = args.inputs.resolve()
    pins = json.loads((ROOT / "native/ps5/dependencies.json").read_text())
    sdk = prepare_archive(pins["sdk"], "ps5-payload-sdk-v0.42.zip", CACHE / "sdk-v0.42", "ps5-payload-sdk/")
    zlib = prepare_archive(pins["zlib"], "zlib-1.3.2.tar.gz", CACHE / "zlib-1.3.2", "zlib-1.3.2/")
    if args.capture_inputs:
        capture(sdk, pins, inputs)
    record = json.loads((inputs / "link-inputs.json").read_text())
    checked(inputs / "build-source.json", record["sourceFingerprint"])
    require(source_fingerprint() == json.loads((inputs / "build-source.json").read_text()),
            "Relink source tree differs from the retained build; use a matching source kit")
    require(record["objectOrder"] == [name + ".o" for name in OBJECTS], "Unexpected object link order")
    objects = [checked(inputs / "objects" / name, record["objects"][name]) for name in record["objectOrder"]]
    stubs = sdk_stubs(sdk, CACHE / "downloads/ps5-payload-sdk-v0.42.zip")
    require(stubs == record["sdkStubs"], "SDK differs from the retained link inputs")
    replacements = (args.sdl, args.libiconv, args.libsamplerate)
    libraries = [replacement.resolve() if replacement else checked(inputs / "libraries" / name,
                                                                  record["libraries"][name])
                 for name, replacement in zip(LIBRARIES, replacements)]
    for path in libraries:
        with path.open("rb") as source:
            require(source.read(8) == b"!<arch>\n", "Expected a PS5 static archive")
    prefix = prepare_compiler()
    env = environment(prefix)
    tool = converter(prefix, env, zlib)
    output = CACHE / "relink-checks" / (datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ-") +
                                       uuid.uuid4().hex[:8])
    output.mkdir(parents=True)
    runtime = ROOT / "native/third_party/ps5-runtime/tooling/native"
    pie, elf, signed = (output / name for name in ("shelf-pie.elf", "eboot.elf", "eboot.bin"))
    run([prefix / "usr/bin/ld.lld-18", "-m", "elf_x86_64", "-pie", "-z", "max-page-size=0x4000", "-mllvm",
         "-emulated-tls", "--hash-style=gnu", "--eh-frame-hdr", "-T", runtime / "ps5-pie.ld", "--version-script",
         runtime / "app-symbols.map", "-e", "_start", "-o", pie, *objects, "--start-group", *libraries,
         "--end-group", "--as-needed", *(sdk / "target/lib" / name for name in sorted(stubs))], env=env)
    run([tool, "link", "--in", pie, "--out", elf, "--stub-dir", sdk / "target/lib", "--module-sdk", "0x02000009",
         "--companion-sdk", "0x08050001", "--file-name", "eboot.elf"], env=env)
    run([tool, "self", "--sign", "--in", elf, "--out", signed, "--magic", "0x1D3D154F"], env=env)
    result = verify(elf, signed)
    modified = any(replacement is not None for replacement in replacements)
    exact = result["self"] == record["referenceSelf"] and file_record(elf) == {
        "bytes": record["referenceElf"]["bytes"], "sha256": record["referenceElf"]["sha256"]}
    require(modified or exact, "Unmodified relink did not reproduce the verified executable")
    result.update({"relinkedOnly": True, "applicationRecompiled": False, "replacementSupplied": modified,
                   "exactReferenceMatch": exact, "libraries": {name: file_record(path)
                                                               for name, path in zip(LIBRARIES, libraries)}})
    if "launcherRuntime" in record:
        result["launcherRuntime"] = record["launcherRuntime"]
    (output / "relink-verification.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Relink verified:", output, "exact reference match:" + str(exact), flush=True)


if __name__ == "__main__":
    main()
