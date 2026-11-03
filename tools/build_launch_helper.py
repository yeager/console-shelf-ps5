# SPDX-License-Identifier: GPL-3.0-only
"""Build an isolated ELF-loader payload. Never sends or runs it on a PS5."""
import json
import struct
import tarfile
import hashlib
import zipfile
from bootstrap_ps5_toolchain import environment, prepare as prepare_compiler
from fetch_ps5_deps import prepare as prepare_dependencies
from ps5_common import CACHE, ROOT, run, sha256
from verify_ps5_build import require
from prepare_corresponding_source import prepare_sdk_source


def main(standard_runtime=False):
    prefix = prepare_compiler()
    sdk, _, _ = prepare_dependencies()
    env = environment(prefix)
    output = CACHE / ("launch-helper-standard" if standard_runtime else "launch-helper")
    output.mkdir(parents=True, exist_ok=True)
    source = ROOT / "native/ps5/helper/shelf_launch.cpp"
    update_source = ROOT / "native/ps5/helper/shelf_update.cpp"
    protocol = ROOT / "native/include/shelf/launch_protocol.hpp"
    update_helper = ROOT / "native/include/shelf/update_helper.hpp"
    update_relaunch = ROOT / "native/include/shelf/update_relaunch.hpp"
    update_protocol = ROOT / "native/include/shelf/update_protocol.hpp"
    sha256_header = ROOT / "native/include/shelf/sha256.hpp"
    residual_guard = ROOT / "native/include/shelf/tile_residual_guard.hpp"
    catalog = ROOT / "native/include/shelf/catalog.generated.hpp"
    limits = ROOT / "native/ps5/helper/runtime_limits.c"
    tracked_inputs = (source, update_source, protocol, update_helper, update_relaunch, update_protocol,
                      sha256_header, residual_guard, catalog, limits)
    inputs = {p.relative_to(ROOT).as_posix(): sha256(p) for p in tracked_inputs}
    archive, sdk_source = prepare_sdk_source()
    crt_source = output / "sdk-crt"
    crt_source.mkdir(exist_ok=True)
    crt_files = []
    with tarfile.open(archive) as upstream:
        for member in upstream:
            parts = member.name.split("/")
            if len(parts) != 3 or parts[1] != "crt" or not member.isfile():
                continue
            if not parts[2].endswith((".c", ".h", ".S")) or parts[2] == "patch.c":
                continue
            raw_source = upstream.extractfile(member).read()
            path = crt_source / parts[2]
            if path.exists():
                require(path.read_bytes() == raw_source, "Cached SDK CRT source differs")
            else:
                with path.open("xb") as target:
                    target.write(raw_source)
            if path.suffix in (".c", ".S") and path.name != "rtld_so.c":
                crt_files.append(path)
    crt_objects = []
    # Rebuild only the SDK CRT; its permission-patching unit is excluded.
    # Do not execute upstream build scripts or alter the original SDK release.
    for path in sorted(crt_files):
        target = output / ("sdk-" + path.stem + ".o")
        language = ["-std=gnu11", "-fms-extensions", "-Wno-microsoft-anon-tag"] if path.suffix == ".c" else ["-masm=intel"]
        run([prefix / "usr/bin/clang-18", "-target", "x86_64-sie-ps5", "-isysroot", sdk,
             "-isystem", sdk / "target/include", "-fvisibility-nodllstorageclass=default",
             "-fno-stack-protector", "-fno-plt", "-fPIC", "-ffreestanding", "-fno-builtin", "-nostdlib",
             "-ffunction-sections", "-fdata-sections", "-O1", "-Wall", "-Werror", *language,
             "-c", path, "-o", target], env=env)
        crt_objects.append(target)
    limits_object = output / "runtime-limits.o"
    run([prefix / "usr/bin/clang-18", "-target", "x86_64-sie-ps5", "-isysroot", sdk,
         "-isystem", sdk / "target/include", "-fvisibility-nodllstorageclass=default", "-fno-stack-protector",
         "-fPIC", "-ffunction-sections", "-fdata-sections", "-O2", "-Wall", "-Wextra", "-Werror",
         "-c", limits, "-o", limits_object], env=env)
    crt_objects.append(limits_object)
    sdk_links = {}
    with zipfile.ZipFile(CACHE / "downloads/ps5-payload-sdk-v0.42.zip") as original:
        for name in ("crt1.o", "libc.a", "libkernel_web.so", "libSceLibcInternal.so", "libSceNet.so",
                     "libSceSystemService.so", "libSceUserService.so"):
            data = original.read("ps5-payload-sdk/target/lib/" + name)
            require((sdk / "target/lib" / name).read_bytes() == data, "SDK link input differs from pinned release")
            sdk_links[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    elf = output / "shelf-launch.elf"
    helper_objects = []
    for helper_source in (source, update_source):
        obj = output / (helper_source.stem + ".o")
        run([prefix / "usr/bin/clang++-18", "-target", "x86_64-sie-ps5", "-isysroot", sdk,
             "-isystem", sdk / "target/include/c++/v1", "-isystem", sdk / "target/include",
             "-fvisibility-nodllstorageclass=default", "-fno-stack-protector", "-fno-plt", "-femulated-tls",
             "-std=c++17", "-O2", "-fno-exceptions", "-fno-rtti", "-DNDEBUG", "-Wall", "-Wextra", "-Werror",
             "-ffunction-sections", "-fdata-sections",
             *(["-DSHELF_STANDARD_SDK_RUNTIME"] if standard_runtime else []),
             "-I", ROOT / "native/include", "-c", helper_source, "-o", obj], env=env)
        helper_objects.append(obj)
    # SDK v0.42's ordinary ELF-payload contract, distinct from the native app's
    # converter/container. Do not apply the app's SELF verifier to this payload.
    run([prefix / "usr/bin/ld.lld-18", "-m", "elf_x86_64", "-pie", "-T", sdk / "ldscripts/elf_x86_64.x",
         "--eh-frame-hdr", "-z", "max-page-size=0x4000", "-mllvm", "-emulated-tls", "--hash-style=gnu",
         "--gc-sections", "-o", elf, *helper_objects,
         *([sdk / "target/lib/crt1.o"] if standard_runtime else crt_objects),
         "-L", sdk / "target/lib", "-lc", "-lkernel_web",
         "-lSceLibcInternal", "-lSceNet", "-lSceSystemService", "-lSceUserService"], env=env)
    raw = elf.read_bytes()
    # The pinned SDK's normal payload CRT uses System V ABI 0. Native app
    # conversion sets FreeBSD/Sony ABI 9 instead; that is a separate format.
    crt = (sdk / "target/lib/crt1.o").read_bytes()
    require(raw[:9] == crt[:9] == b"\x7fELF\x02\x01\x01\x00\x00", "Wrong SDK-payload ELF class/ABI")
    kind, machine = struct.unpack_from("<HH", raw, 16)
    require((kind, machine) == (3, 62), "Expected an x86-64 dynamic payload")
    entry, phoff = struct.unpack_from("<QQ", raw, 24)
    phsize, phcount = struct.unpack_from("<HH", raw, 54)
    require(phsize == 56 and 1 <= phcount <= 16 and phoff + phsize * phcount <= len(raw), "Invalid payload headers")
    segments = [struct.unpack_from("<IIQQQQQQ", raw, phoff + i * phsize) for i in range(phcount)]
    loads = [p for p in segments if p[0] == 1]
    require(any(p[1] & 1 and p[3] <= entry < p[3] + p[6] for p in loads), "Payload entry is outside code")
    for p in loads:
        require(p[5] <= p[6] and p[2] + p[5] <= len(raw) and p[7] == 0x4000 and p[2] % p[7] == p[3] % p[7],
                "Invalid SDK payload segment")
    shoff = struct.unpack_from("<Q", raw, 40)[0]
    shsize, shcount = struct.unpack_from("<HH", raw, 58)
    require(shsize == 64 and 1 <= shcount <= 256 and shoff + shsize * shcount <= len(raw), "Invalid symbol sections")
    sections = [struct.unpack_from("<IIQQQQIIQQ", raw, shoff + i * shsize) for i in range(shcount)]
    symbols = {}
    imports = set()
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        names = raw[strings[4]:strings[4] + strings[5]]
        require(section[9] == 24 and section[4] + section[5] <= len(raw), "Invalid helper symbol table")
        for index in range(0, section[5], 24):
            name, info, _, owner, address, size = struct.unpack_from("<IBBHQQ", raw, section[4] + index)
            label = names[name:names.index(0, name)].decode()
            if owner and info & 15 == 2:
                symbols[label] = (info, address, size)
            elif not owner and info == 0x12:
                imports.add(label)
    for hook in (() if standard_runtime else ("__patch_init", "__rtld_so_init", "__rtld_so_new")):
        require(hook in symbols, "Missing permission-preserving CRT hook")
        info, address, size = symbols[hook]
        segment = next(p for p in loads if p[1] & 1 and p[3] <= address and address + size <= p[3] + p[5])
        begin = segment[2] + address - segment[3]
        require(info == 0x12 and raw[begin:begin + size] == b"\x31\xc0\xc3", "CRT hook is not the expected return-zero body")
    forbidden = ("kernel_copyin", "kernel_proc_copyin", "mdbg_call", "patch_kernel_ucred", "patch_syscall_permissions")
    require(standard_runtime or not any(name.startswith(("kernel_set_", "mdbg_")) or name in forbidden for name in symbols),
            "Unexpected kernel write or credential-changing routine remains in helper")
    require(raw.count(b"ISSHELF_EXACT_ORIGIN_CLOSE_V1") == 1 and
            "sceLncUtilKillApp" in imports,
            "Exact-origin close marker or strong import differs")
    require(raw.count(b"ISSHELF_IN_APP_UPDATER_V1") == 1,
            "The exact in-app update transaction marker is missing")
    require(raw.count(b"ISSHELF_EXACT_UPDATE_RELAUNCH_V4") == 1 and
            "sceLncUtilLaunchApp" in imports,
            "The single-request exact-title updater relaunch marker or import differs")
    require(raw.count(b"ISSHELF_UPDATE_DEBUG_NOTICES_V1") == 1 and
            "sceKernelSendNotificationRequest" in imports,
            "The owner-visible updater notification marker or import differs")
    require(raw.count(b"Please don't open anything for %u seconds;") == 1,
            "The protected updater idle-window warning is missing")
    require(inputs == {p.relative_to(ROOT).as_posix(): sha256(p) for p in tracked_inputs}, "Build input drift")
    result = {"elf": {"bytes": len(raw), "sha256": sha256(elf)}, "inputs": inputs,
              "hardwareTest": "not run", "transport": "not connected", "deployed": False,
              "abi": "System V ABI 0, matching pinned SDK v0.42 crt1.o; not a native app/SELF",
              "sdkRuntimeSource": sdk_source,
              "sdkLinkInputs": sdk_links,
              "runtimeMode": "standard-sdk" if standard_runtime else "restricted",
              "permissionChanges": standard_runtime,
              "crtPatchHook": ("Unmodified pinned SDK startup; targets helper process capabilities and syscall bounds"
                               if standard_runtime else "verified return 0; SDK patch.c excluded; kernel write routines absent"),
              "policy": "cooperative Shelf closure; after five stuck committed seconds, one exact PPSA99541 origin close; one selected SystemService launch, one exact owner-approved journalled isShelf program update with SHA-256 revalidation, executable-last commit and rollback, owner-visible kernel debug notifications for verification, recovery, installation, rollback, finishing and relaunch, followed only after 35000 continuously observed no-app milliseconds (one full ShadowMount refresh cadence plus margin) by exactly one exact-PPSA99541 LncUtil request and up to 30000 ms of running-title observation even when the immediate launch return is negative; the request is never repeated because ShadowMount may already be completing it asynchronously; or after a 3 s post-close settle and a read-only fixed-loopback readiness window of at most 8 s, one fixed tile uninstall, one bounded owned-title removal/artwork batch, or the complete ordered PPSA99700..PPSA99751 residual sweep; readiness polling continuously proves no foreground app and sends no HTTP request; authorization is rechecked before the first mutation; residual cleanup first proves the manual list excludes every protected slot and preflights every global registration as absent or exact platform-owned isShelf identity and artwork before any uninstall; maintenance is paced at 1000 ms and followed by one scan; maintenance retries only the same title after an exact game_active or scanner_busy receipt, five continuous idle seconds, and no observed big app, with at most five retries per title; long maintenance may update only its exact nonce-and-issued status owner; read-only focus observation; no arbitrary process kill, arbitrary-title launch retry or emulator-file writes",
              "exactOriginClose": {"titleId": "PPSA99541", "delayMs": 5000,
                                     "attempts": 1, "marker": "ISSHELF_EXACT_ORIGIN_CLOSE_V1"},
              "exactUpdateRelaunch": {"titleId": "PPSA99541", "rounds": 1,
                                        "attemptsPerRound": 1, "settleMs": 35000,
                                        "retryMs": 0,
                                        "observeMs": 30000,
                                        "observeNegativeReturn": True,
                                        "marker": "ISSHELF_EXACT_UPDATE_RELAUNCH_V4"},
              "updateDebugNotifications": {"style": "kernel-text", "downloadPercent": [25, 50, 75],
                                             "helperStages": ["verify", "recover", "install", "restore", "finish", "relaunch"],
                                             "idleWarningSeconds": 35,
                                             "marker": "ISSHELF_UPDATE_DEBUG_NOTICES_V1"},
              "approvalForStandardRuntime": "required before console use" if standard_runtime else "not applicable"}
    (output / "build.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--standard-helper-runtime", action="store_true",
                        help="Build only: enable the SDK's process-permission startup; requires approval before console use")
    main(parser.parse_args().standard_helper_runtime)
