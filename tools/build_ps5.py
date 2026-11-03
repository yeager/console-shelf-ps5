# SPDX-License-Identifier: GPL-3.0-only
"""Cross-compile Shelf in the project cache. Does not connect to a console."""
import json
import hashlib
import re
from bootstrap_ps5_toolchain import environment, host_options, prepare as prepare_compiler
from fetch_ps5_deps import prepare as prepare_dependencies, prepare_mbedtls
from ps5_common import CACHE, ROOT, run, sha256
from verify_ps5_build import (PATH_PROBE_ELF_SHA256, PATH_PROBE_SOURCE_SHA256,
                              PATH_PROBE_V1_ELF_BYTES, PATH_PROBE_V1_ELF_SHA256,
                              WRITE_PROBE_ELF_SHA256, WRITE_PROBE_SOURCE_SHA256,
                              WRITE_PROBE_V3_ELF_BYTES, WRITE_PROBE_V3_ELF_SHA256,
                              embedded_elf_digest_occurrences, require, source_fingerprint, verify)
from build_launch_helper import main as build_launch_helper
from console_tile_forwarder_v2.build import main as build_console_tile_forwarder
from console_badges.build_console_tile_path_probe_v2 import main as build_console_tile_path_probe
from console_badges.build_console_tile_write_probe_v4 import main as build_console_tile_write_probe


def cached_run(args, inputs, output, env):
    record = {"args": [str(a) for a in args], "inputs": {str(p): sha256(p) for p in sorted(inputs)}}
    key = hashlib.sha256(json.dumps(record, sort_keys=True).encode()).hexdigest()
    stamp = output.with_name(output.name + ".build.json")
    if output.is_file() and stamp.is_file():
        old = json.loads(stamp.read_text())
        if old.get("key") == key and old.get("sha256") == sha256(output):
            return
    run(args, env=env)
    stamp.write_text(json.dumps({"key": key, "sha256": sha256(output)}, indent=2) + "\n")


def build(standard_runtime=True, ui_test_bridge=False, owned_tile_writes=False):
    identity = source_fingerprint()
    sword_manifest = json.loads((ROOT / "native/third_party/ps5sx2_https/SOURCE.json").read_text())
    for entry in sword_manifest["files"]:
        source = ROOT / entry["local"]
        require(source.is_file() and source.stat().st_size == entry["bytes"] and
                sha256(source) == entry["sha256"],
                "Adapted PS5SX2 updater source differs from its recorded pin: " + entry["local"])
    package_version = json.loads((ROOT / "package.json").read_text()).get("version", "")
    require(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", package_version) is not None,
            "package.json must contain a canonical production version")
    version_tag = "v" + package_version
    video_sources = ROOT / "native/third_party/pl_mpeg"
    for entry in json.loads((video_sources / "SOURCE.json").read_text())["files"]:
        source = video_sources / entry["path"]
        require(source.stat().st_size == entry["bytes"] and sha256(source) == entry["sha256"],
                "PL_MPEG source differs from the recorded upstream pin")
    prefix = prepare_compiler()
    env = environment(prefix)
    sdk, ports, zlib = prepare_dependencies()
    mbedtls = prepare_mbedtls()
    output = CACHE / "build"
    output.mkdir(parents=True, exist_ok=True)
    build_launch_helper(standard_runtime)
    helper_folder = CACHE / ("launch-helper-standard" if standard_runtime else "launch-helper")
    helper = helper_folder / "shelf-launch.elf"
    helper_record = json.loads((helper_folder / "build.json").read_text())
    require(helper_record["elf"] == {"bytes": helper.stat().st_size, "sha256": sha256(helper)}, "Helper build changed")
    payload = helper.read_bytes()
    literal = ",\n".join(",".join(f"0x{byte:02x}" for byte in payload[start:start + 24]) for start in range(0, len(payload), 24))
    (output / "shelf_launch_asset.hpp").write_text(
        "// Generated from the verified owned helper; never read from a console or external request.\n"
        "static const unsigned char shelf_launch_payload[] = {\n" + literal + "\n};\n")
    probe_record = None
    probe_payload = None
    write_probe_record = None
    write_probe_payload = None
    if ui_test_bridge:
        probe_source = ROOT / "tools/console_badges/console_tile_path_probe_v2.cpp"
        require(sha256(probe_source) == PATH_PROBE_SOURCE_SHA256,
                "Read-only path probe source differs from the reviewed pin")
        build_console_tile_path_probe()
        probe_folder = CACHE / "console-tile-path-probe-v2"
        probe_elf = probe_folder / "console-tile-path-probe-v2.elf"
        probe_record = json.loads((probe_folder / "build.json").read_text())
        probe_identity = probe_record["elf"]
        require(probe_record["inputs"] == {probe_source.relative_to(ROOT).as_posix(): PATH_PROBE_SOURCE_SHA256} and
                probe_identity["bytes"] == probe_elf.stat().st_size and
                probe_identity["sha256"] == sha256(probe_elf) == PATH_PROBE_ELF_SHA256 and
                sorted(probe_identity["needed"]) == ["libSceLibcInternal.sprx", "libkernel_web.sprx"] and
                probe_identity["openatPresent"] is False and
                probe_identity["standardSdkHooksVerified"] is True and
                probe_identity["mainEntryKlog"] == "ISSHELF_PATH_PROBE_V2_MAIN_ENTERED" and
                probe_record["request"] == "/data/console-shelf/console-tile-path-request-v2.txt" and
                probe_record["onConsoleReceipt"] == "/data/console-shelf/console-tile-path-v2-<nonce>.txt" and
                probe_record["requestMode"] == probe_record["receiptMode"] == "0644" and
                probe_record["runtimeMode"] == "standard-sdk" and
                probe_record["permissionChanges"] is True and
                probe_record["titleRootWriteAttempted"] is False and
                probe_record["databaseOpenAttempted"] is False and
                probe_record["networkAttempted"] is False,
                "Read-only path probe build changed")
        probe_payload = probe_elf.read_bytes()
        probe_literal = ",\n".join(",".join(f"0x{byte:02x}" for byte in probe_payload[start:start + 24])
                                   for start in range(0, len(probe_payload), 24))
        (output / "console_tile_path_probe_asset.hpp").write_text(
            "// Private UI test only: exact verified v2 read-only standard-SDK path probe.\n"
            "#include <cstddef>\n"
            "static const unsigned char console_tile_path_probe_payload[] = {\n" + probe_literal + "\n};\n"
            "static constexpr std::size_t console_tile_path_probe_payload_bytes = "
            "sizeof(console_tile_path_probe_payload);\n")
        write_source = ROOT / "tools/console_badges/console_tile_write_probe_v4.cpp"
        require(sha256(write_source) == WRITE_PROBE_SOURCE_SHA256,
                "Scratch write probe source differs from the reviewed pin")
        build_console_tile_write_probe()
        write_folder = CACHE / "console-tile-write-probe-v4"
        write_elf = write_folder / "console-tile-write-probe-v4.elf"
        write_probe_record = json.loads((write_folder / "build.json").read_text())
        write_identity = write_probe_record["elf"]
        require(write_probe_record["inputs"] == {write_source.relative_to(ROOT).as_posix(): WRITE_PROBE_SOURCE_SHA256} and
                write_identity["bytes"] == write_elf.stat().st_size and
                write_identity["sha256"] == sha256(write_elf) == WRITE_PROBE_ELF_SHA256 and
                sorted(write_identity["needed"]) == ["libSceLibcInternal.sprx", "libkernel_web.sprx"] and
                write_identity["atSyscalls"] == ["fstatat", "mkdirat", "openat", "renameat", "unlinkat"] and
                write_identity["atSyscallReturn"] == "freebsd-carry-normalized" and
                set(write_identity["atAdapterEvidence"]) ==
                    {"fstatat", "mkdirat", "openat", "renameat", "unlinkat"} and
                write_probe_record["errnoMacros"] ==
                    {"ENOENT": 2, "EEXIST": 17, "EIO": 5, "EINVAL": 22} and
                write_identity["standardSdkHooksVerified"] is True and
                write_identity["mainEntryKlog"] == "ISSHELF_WRITE_PROBE_V4_MAIN_ENTERED" and
                write_probe_record["request"] == "/data/console-shelf/console-tile-write-request-v4.txt" and
                write_probe_record["onConsoleReceipt"] == "/data/console-shelf/console-tile-write-v4-<nonce>.txt" and
                write_probe_record["scratch"] == "/data/homebrew/.isshelf-write-probe-v4-<nonce>" and
                write_probe_record["requestMode"] == write_probe_record["receiptMode"] == "0644" and
                write_probe_record["runtimeMode"] == "standard-sdk" and
                write_probe_record["permissionChanges"] is True and
                write_probe_record["validTitleIdWriteAttempted"] is False and
                write_probe_record["homebrewScratchWritePossible"] is True and
                write_probe_record["databaseOpenAttempted"] is False and
                write_probe_record["networkAttempted"] is False,
                "Scratch write probe build changed")
        write_probe_payload = write_elf.read_bytes()
        write_literal = ",\n".join(",".join(f"0x{byte:02x}" for byte in write_probe_payload[start:start + 24])
                                   for start in range(0, len(write_probe_payload), 24))
        (output / "console_tile_write_probe_v4_asset.hpp").write_text(
            "// Private UI test only: exact verified v3 scratch write probe.\n"
            "#include <cstddef>\n"
            "static const unsigned char console_tile_write_probe_v4_payload[] = {\n" + write_literal + "\n};\n"
            "static constexpr std::size_t console_tile_write_probe_v4_payload_bytes = "
            "sizeof(console_tile_write_probe_v4_payload);\n")
    runtime = ROOT / "native/third_party/ps5-runtime/tooling/native"
    manifest = json.loads((runtime.parent.parent / "SOURCE.json").read_text())
    for entry in manifest["files"]:
        source = runtime.parent.parent / entry["path"]
        if source.stat().st_size != entry["bytes"] or sha256(source) != entry["sha256"]:
            raise RuntimeError(f"Imported runtime source fails its pin: {entry['path']}")
    compiler = prefix / "usr/bin/clang-18"
    cxx = prefix / "usr/bin/clang++-18"
    host_flags, host_link = host_options(prefix)
    # Build the host's zlib locally from its pinned source; never use a target lib.
    local_zlib = output / "zlib"
    local_zlib.mkdir(exist_ok=True)
    z_sources = ["adler32", "crc32", "deflate", "infback", "inffast", "inflate", "inftrees", "trees",
                 "zutil", "compress", "uncompr", "gzclose", "gzlib", "gzread", "gzwrite"]
    z_objects = []
    for name in z_sources:
        obj = local_zlib / (name + ".o")
        cached_run([compiler, *host_flags, "-O2", "-DHAVE_UNISTD_H", "-I", zlib, "-c", zlib / (name + ".c"), "-o", obj],
                   [zlib / (name + ".c"), *zlib.glob("*.h")], obj, env)
        z_objects.append(obj)
    run(["ar", "rcs", local_zlib / "libz.a", *z_objects], env=env)
    tool = output / "ps5-native-tool"
    cached_run([cxx, *host_flags, *host_link, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-I", zlib,
         *(runtime / (name + ".cpp") for name in ("native_app_builder", "elf_object", "self_container", "sce_module_writer")),
         local_zlib / "libz.a", "-o", tool], [*runtime.glob("*.cpp"), *runtime.glob("*.hpp"),
                                             zlib / "zlib.h", zlib / "zconf.h", local_zlib / "libz.a"], tool, env)
    libc_builder = output / "libc-builder"
    cached_run([cxx, *host_flags, *host_link, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                runtime / "libc_builder.cpp", "-o", libc_builder], [runtime / "libc_builder.cpp"], libc_builder, env)
    # Reproduce, rather than copy, the reference's clean-room runtime.
    for copy in ("a", "b"):
        raw = output / f"libc-{copy}.elf"
        signed = output / f"libc-{copy}.prx"
        run([libc_builder, runtime / "runtime/api-surface.txt", runtime / "runtime/imports.txt", raw], env=env)
        if sha256(raw) != "8ee6e124993e1af26420cb455890fd002f5d6c7e78883c860ce45734e7d002bb":
            raise RuntimeError("Rebuilt raw clean-room runtime does not match the reference")
        run([tool, "self", "--sign", "--in", raw, "--out", signed], env=env)
        if sha256(signed) != "e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036":
            raise RuntimeError("Rebuilt signed clean-room runtime does not match the reference")
    print("Clean-room runtime rebuilt twice with the exact reference SHA-256.", flush=True)
    common = ["-target", "x86_64-sie-ps5", "-fvisibility-nodllstorageclass=default", "-isysroot", sdk,
              "-isystem", sdk / "target/include/c++/v1", "-isystem", sdk / "target/include",
              "-fno-stack-protector", "-fno-plt", "-femulated-tls", "-O2", "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
              "-DSHELF_PS5", "-DGS_NATIVE", "-DNDEBUG", "-D_REENTRANT", "-D_THREAD_SAFE",
              '-DMBEDTLS_CONFIG_FILE="ps5sx2_mbedtls_config.h"', f'-DISSHELF_VERSION="{version_tag}"',
              "-I", ROOT / "native/include", "-I", ROOT / "native/ps5", "-I", ROOT / "native/third_party", "-I", output,
              "-I", ROOT / "native/third_party/ps5sx2_https", "-I", mbedtls / "include", "-I", mbedtls / "library",
              "-I", ports / "include/SDL2", "-I", ports / "include"]
    # L3+R3 owner support is a stable user feature. The separate private flag
    # adds development probes, but public builds retain bounded menu control
    # and asynchronous app-frame previews after physical opt-in.
    common.append("-DSHELF_OWNER_SUPPORT_CONTROL")
    if ui_test_bridge:
        common.append("-DSHELF_UI_TEST_BRIDGE")
    if owned_tile_writes:
        common.append("-DSHELF_PS5_OWNED_TILE_WRITES")
    mbedtls_sources = [
        "aes.c", "aesni.c", "asn1parse.c", "asn1write.c", "base64.c", "bignum.c",
        "bignum_core.c", "chacha20.c", "chachapoly.c", "cipher.c", "cipher_wrap.c",
        "constant_time.c", "ctr_drbg.c", "ecdh.c", "ecdsa.c", "ecp.c", "ecp_curves.c",
        "entropy.c", "error.c", "gcm.c", "md.c", "oid.c", "pem.c", "pk.c", "pk_ecc.c",
        "pk_wrap.c", "pkparse.c", "platform_util.c", "poly1305.c", "rsa.c",
        "rsa_alt_helpers.c", "sha1.c", "sha256.c", "sha512.c", "ssl_ciphersuites.c",
        "ssl_client.c", "ssl_msg.c", "ssl_tls.c", "ssl_tls12_client.c", "x509.c", "x509_crt.c",
    ]
    sources = [ROOT / "native/src" / (name + ".cpp") for name in ("model", "settings", "ui", "graphics", "codecs", "movie", "library", "artwork_sources", "artwork", "filesystem", "metadata", "ftp", "launch_client", "update", "update_client", "console_tile_manager", "managed_tile_api_v1", "managed_tile_app_client", "host_main")]
    sources += [ROOT / "native/src/json.c"]
    sources += [ROOT / "native/third_party/ps5sx2_https/fe_https.cpp",
                ROOT / "native/third_party/ps5sx2_https/ps5sx2_mbedtls_platform.c",
                ROOT / "native/ps5/platform/update_https_ps5.cpp"]
    sources += [mbedtls / "library" / name for name in mbedtls_sources]
    sources.append(ROOT / "native/src/ui_test_bridge.cpp")
    sources.append(ROOT / "native/src/ui_test_video.cpp")
    sources += [ROOT / "native/ps5/platform" / name for name in
                ("alloc.c", "vout.c", "shims.c", "lifecycle.c")]
    sources += [runtime / "app_crt.cpp", runtime / "app_cpp_runtime.cpp"]
    objects = []
    for source in sources:
        obj = output / (source.stem + ".o")
        language = ["-std=c++17", "-fno-exceptions", "-fno-rtti"] if source.suffix == ".cpp" else ["-std=c11"]
        warnings = ["-Wall", "-Wextra", "-Werror", "-Wno-unused-function"]
        if source.stem in {"library", "artwork_sources", "artwork", "filesystem", "metadata", "ftp", "ui_test_video", "console_tile_manager"}:
            # These units run on the bounded library worker stack. Reject a single
            # oversized frame before a desktop's larger default stack hides it.
            warnings.append("-Wframe-larger-than=16384")
        if source.stem == "codecs":
            warnings = ["-w"]  # Unmodified third-party implementations; owned units remain strict.
        print("Compile PS5:", source.name, flush=True)
        run([compiler, *common, *language, *warnings, "-fstack-usage", "-c", source, "-o", obj], env=env)
        objects.append(obj)
    # Equivalent to SDK v0.42's prospero-lld wrapper, using the pinned linker
    # directly so no global llvm-config symlink or installation is needed.
    pie = output / "shelf-pie.elf"
    run([prefix / "usr/bin/ld.lld-18", "-m", "elf_x86_64", "-pie", "-z", "max-page-size=0x4000",
         "--no-dynamic-linker",
         "-mllvm", "-emulated-tls", "--hash-style=gnu", "--eh-frame-hdr", "-T", runtime / "ps5-pie.ld",
         "-T", ROOT / "native/third_party/ps5sx2_https/ehframe.ld",
         "--version-script", runtime / "app-symbols.map", "-e", "_start", "-o", pie, *objects,
         "-L", sdk / "target/lib",
         "--start-group", *(ports / "lib" / name for name in
                             ("libSDL2.a", "libiconv.a", "libsamplerate.a", "libminizip.a")),
         local_zlib / "libz.a",
         *(sdk / "target/lib" / name for name in
           ("libc++.a", "libc++abi.a", "libunwind.a", "libc.a", "libpthread.a")),
         "--end-group", "--as-needed", *sorted((sdk / "target/lib").glob("*.so"))], env=env)
    elf = output / "eboot.elf"
    run([tool, "link", "--in", pie, "--out", elf, "--stub-dir", sdk / "target/lib",
         "--module-sdk", "0x02000009", "--companion-sdk", "0x08050001", "--file-name", "eboot.elf"], env=env)
    signed = output / "eboot.bin"
    run([tool, "self", "--sign", "--in", elf, "--out", signed, "--magic", "0x1D3D154F"], env=env)
    run([tool, "self", "--inspect", "--file", signed], env=env)
    report = verify(elf, signed)
    require(report["ownerSupportControl"] is True,
            "Stable PS5 builds must retain physical L3+R3 owner support")
    require(report["uiTestBridge"] == ui_test_bridge, "Private UI test build marker differs from requested options")
    require(report["ownedTileWrites"] == owned_tile_writes,
            "Private owned-tile build marker differs from requested options")
    require(elf.read_bytes().count(payload) == 1, "The verified helper is not embedded exactly once")
    report["embeddedLaunchHelper"] = {**helper_record["elf"], "offset": elf.read_bytes().index(payload),
                                     "runtimeMode": helper_record["runtimeMode"],
                                     "permissionChanges": helper_record["permissionChanges"],
                                     "exactOriginClose": helper_record["exactOriginClose"]}
    if ui_test_bridge:
        raw_elf = elf.read_bytes()
        require(probe_payload is not None and probe_record is not None and raw_elf.count(probe_payload) == 1,
                "The verified v2 read-only path probe is not embedded exactly once")
        require(embedded_elf_digest_occurrences(raw_elf, PATH_PROBE_V1_ELF_BYTES,
                                                PATH_PROBE_V1_ELF_SHA256) == 0,
                "Private build still embeds the v1 path probe")
        report["embeddedConsoleTilePathProbe"] = {
            **probe_record["elf"], "version": 2, "offset": raw_elf.index(probe_payload),
            "runtimeMode": probe_record["runtimeMode"],
            "permissionChanges": probe_record["permissionChanges"],
            "request": probe_record["request"], "onConsoleReceipt": probe_record["onConsoleReceipt"],
            "requestMode": probe_record["requestMode"], "receiptMode": probe_record["receiptMode"],
            "titleRootWriteAttempted": probe_record["titleRootWriteAttempted"],
            "databaseOpenAttempted": probe_record["databaseOpenAttempted"],
            "networkAttempted": probe_record["networkAttempted"],
        }
        require(write_probe_payload is not None and write_probe_record is not None and
                raw_elf.count(write_probe_payload) == 1 and
                embedded_elf_digest_occurrences(raw_elf, WRITE_PROBE_V3_ELF_BYTES,
                                                WRITE_PROBE_V3_ELF_SHA256) == 0,
                "The verified v4 scratch write probe is not embedded exactly once")
        report["embeddedConsoleTileWriteProbe"] = {
            **write_probe_record["elf"], "version": 4, "offset": raw_elf.index(write_probe_payload),
            "runtimeMode": write_probe_record["runtimeMode"],
            "permissionChanges": write_probe_record["permissionChanges"],
            "request": write_probe_record["request"],
            "onConsoleReceipt": write_probe_record["onConsoleReceipt"],
            "scratch": write_probe_record["scratch"],
            "requestMode": write_probe_record["requestMode"],
            "receiptMode": write_probe_record["receiptMode"],
            "validTitleIdWriteAttempted": write_probe_record["validTitleIdWriteAttempted"],
            "homebrewScratchWritePossible": write_probe_record["homebrewScratchWritePossible"],
            "databaseOpenAttempted": write_probe_record["databaseOpenAttempted"],
            "networkAttempted": write_probe_record["networkAttempted"],
        }
    require(identity == source_fingerprint(), "Build inputs changed during compilation; rebuild before verification")
    (output / "build-source.json").write_text(json.dumps(identity, indent=2) + "\n")
    (output / "structural-check.json").write_text(json.dumps(report, indent=2) + "\n")
    build_console_tile_forwarder()
    print(f"PS5 executable compiled at {signed}; NOT a validated console package.", flush=True)


def parse_options(arguments=None):
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    helper_runtime = parser.add_mutually_exclusive_group()
    helper_runtime.add_argument("--standard-helper-runtime", dest="standard_helper_runtime", action="store_true",
                                help="Use the accepted SDK helper runtime required for emulator launching (default)")
    helper_runtime.add_argument("--restricted-helper-runtime", dest="standard_helper_runtime", action="store_false",
                                help="Build the diagnostic helper that cannot launch on the owner's console")
    parser.set_defaults(standard_helper_runtime=True)
    parser.add_argument("--ui-test-bridge", action="store_true",
                        help="Private only: add diagnostic probes to the always-present owner support control; blocks public release")
    parser.add_argument("--owned-tile-writes", action="store_true",
                        help="Compile the exact-owned-source tile add/edit/remove manager")
    return parser.parse_args(arguments)


if __name__ == "__main__":
    options = parse_options()
    build(options.standard_helper_runtime, options.ui_test_bridge, options.owned_tile_writes)
