# SPDX-License-Identifier: GPL-3.0-only
"""Build isShelf's cooperative console-tile template without console contact."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from bootstrap_ps5_toolchain import environment, prepare as prepare_compiler  # noqa: E402
from fetch_ps5_deps import prepare as prepare_dependencies  # noqa: E402
from ps5_common import CACHE, run, sha256  # noqa: E402

OUTPUT = CACHE / "console-tile-forwarder-v2"
SOURCE = ROOT / "native/ps5/tile_forwarder"
RUNTIME = ROOT / "native/third_party/ps5-runtime/tooling/native"
LIBC_SHA256 = "e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036"


def need(value: bool, reason: str) -> None:
    if not value:
        raise RuntimeError(reason)


def literal(data: bytes) -> str:
    return ",\n".join(",".join(f"0x{byte:02x}" for byte in data[offset:offset + 24])
                       for offset in range(0, len(data), 24))


def main() -> Path:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    prefix = prepare_compiler()
    sdk, _, _ = prepare_dependencies()
    env = environment(prefix)
    tool = CACHE / "build/ps5-native-tool"
    tool_stamp = tool.with_name(tool.name + ".build.json")
    need(tool.is_file() and tool_stamp.is_file(),
         "Build isShelf first so the reviewed native SELF tool is available")
    recorded_tool = json.loads(tool_stamp.read_text(encoding="utf-8"))
    need(recorded_tool.get("sha256") == sha256(tool),
         "Cached native SELF tool differs from its build record")
    # The parent build has just produced and independently compared this
    # clean-room runtime twice.  Use that reviewed output directly so a fresh
    # checkout does not depend on a separately cloned Forwarder Format cache.
    libc = CACHE / "build/libc-a.prx"
    need(libc.is_file() and sha256(libc) == LIBC_SHA256,
         "Pinned clean-room tile libc from the parent build is unavailable")

    compiler = prefix / "usr/bin/clang-18"
    cxx = prefix / "usr/bin/clang++-18"
    common = ["-target", "x86_64-sie-ps5", "-isysroot", sdk,
              "-isystem", sdk / "target/include/c++/v1",
              "-isystem", sdk / "target/include",
              "-fvisibility-nodllstorageclass=default", "-fno-stack-protector",
              "-fno-plt", "-femulated-tls", "-O2", "-fno-omit-frame-pointer",
              "-ffunction-sections", "-fdata-sections", "-DSHELF_PS5",
              "-DNDEBUG", "-I", ROOT / "native/include",
              "-I", ROOT / "native/third_party"]

    relay_object = OUTPUT / "tile-relay.o"
    relay_json_object = OUTPUT / "tile-relay-json.o"
    relay_elf = OUTPUT / "tile-relay.elf"
    run([cxx, *common, "-std=c++17", "-fno-exceptions", "-fno-rtti",
         "-Wall", "-Wextra", "-Werror", "-c", SOURCE / "relay.cpp",
         "-o", relay_object], env=env)
    run([compiler, *common, "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-c", ROOT / "native/src/json.c", "-o", relay_json_object], env=env)
    run([prefix / "usr/bin/ld.lld-18", "-m", "elf_x86_64", "-pie", "-T",
         sdk / "ldscripts/elf_x86_64.x", "--eh-frame-hdr", "-z",
         "max-page-size=0x4000", "-mllvm", "-emulated-tls", "--hash-style=gnu",
         "--gc-sections", "-o", relay_elf, relay_object, relay_json_object,
         sdk / "target/lib/crt1.o", "-L", sdk / "target/lib", "-lc",
         "-lkernel_web", "-lSceLibcInternal", "-lSceNet",
         "-lSceSystemService", "-lSceUserService"], env=env)
    relay = relay_elf.read_bytes()
    need(relay.startswith(b"\x7fELF\x02\x01\x01\x00\x00") and
         b"isshelf-main=PPSA99541;handoff=shadowmount-unmount-assisted-retry5s" in relay and
         b"/api/v1/games/info" in relay and
         b"/api/v1/games/unmount" in relay and
         b"coalesced duplicate origin=" in relay and
         b"tile-relay-v3.log" in relay and
         b"isshelf-console-tile.json" in relay,
         "Relay payload identity or ABI differs")
    generated = OUTPUT / "tile_relay_payload.inc"
    generated.write_text(
        "// Generated from the reviewed one-shot cooperative relay.\n"
        "static const unsigned char kTileRelayPayload[] = {\n" + literal(relay) +
        "\n};\n", encoding="utf-8")

    sources = [
        (SOURCE / "main.cpp", cxx, ["-std=c++17", "-fno-exceptions", "-fno-rtti"]),
        (ROOT / "native/src/json.c", compiler, ["-std=c11"]),
        (RUNTIME / "app_crt.cpp", cxx, ["-std=c++17", "-fno-exceptions", "-fno-rtti"]),
        (RUNTIME / "app_cpp_runtime.cpp", cxx, ["-std=c++17", "-fno-exceptions", "-fno-rtti"]),
    ]
    objects = []
    for source, frontend, language in sources:
        target = OUTPUT / ("app-" + source.stem + ".o")
        run([frontend, *common, *language, "-Wall", "-Wextra", "-Werror",
             "-I", OUTPUT, "-c", source, "-o", target], env=env)
        objects.append(target)

    pie = OUTPUT / "tile-forwarder-pie.elf"
    run([prefix / "usr/bin/ld.lld-18", "-m", "elf_x86_64", "-pie", "-z",
         "max-page-size=0x4000", "-mllvm", "-emulated-tls", "--hash-style=gnu",
         "--eh-frame-hdr", "-T", RUNTIME / "ps5-pie.ld", "--version-script",
         RUNTIME / "app-symbols.map", "-e", "_start", "-o", pie, *objects,
         "--as-needed", *sorted((sdk / "target/lib").glob("*.so"))], env=env)
    elf = OUTPUT / "eboot.elf"
    signed = OUTPUT / "template/eboot.bin"
    signed.parent.mkdir(parents=True, exist_ok=True)
    run([tool, "link", "--in", pie, "--out", elf, "--stub-dir",
         sdk / "target/lib", "--module-sdk", "0x02000009", "--companion-sdk",
         "0x08050001", "--file-name", "eboot.elf"], env=env)
    run([tool, "self", "--sign", "--in", elf, "--out", signed,
         "--magic", "0x1D3D154F"], env=env)
    raw_elf = elf.read_bytes()
    need(raw_elf.count(relay) == 1 and
         b"isshelf-cooperative-forwarder/3" in raw_elf and
         b"ISSHELF_CLEAN_TILE_CLOSE_V1" in raw_elf and
         b"forwarder.json" in raw_elf and signed.stat().st_size < 512 * 1024,
         "Cooperative tile SELF structure differs")
    module = OUTPUT / "template/sce_module/libc.prx"
    module.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(libc, module)

    inputs = {path.relative_to(ROOT).as_posix(): sha256(path) for path in [
        SOURCE / "main.cpp", SOURCE / "relay.cpp",
        ROOT / "native/include/shelf/console_tile_relay.hpp",
        ROOT / "native/include/shelf/console_tile_forwarder_config.hpp",
        ROOT / "native/include/shelf/console_tile_intent.hpp",
        ROOT / "native/include/shelf/console_tile_job_pin.hpp",
        ROOT / "native/include/shelf/launch_protocol.hpp",
        ROOT / "native/include/shelf/catalog.generated.hpp",
        ROOT / "native/src/json.c", RUNTIME / "app_crt.cpp",
        RUNTIME / "app_cpp_runtime.cpp", RUNTIME / "ps5-pie.ld",
        RUNTIME / "app-symbols.map",
    ]}
    report = {
        "format": "isshelf-cooperative-console-forwarder/3",
        "status": "offline-built",
        "consoleContact": False,
        "target": "PPSA99541",
        "relayPort": 10202,
        "relayLifecycle": "one-shot-with-identical-request-coalescing",
        "stableHomeObservations": 5,
        "stableHomeIntervalMs": 250,
        "maximumWaitMs": 30000,
        "shadowMountReleaseGate": "one-unmount-assist-one-retry-after-5s-then-paced-game-info",
        "shadowMountUnmountAttempts": 2,
        "shadowMountUnmountRetryDelayMs": 5000,
        "shadowMountInfoIntervalMs": 1000,
        "shadowMountReleaseMaxWaitMs": 40000,
        "launchAttempts": 1,
        "tileExit": "clean-shell-close-no-crt-return",
        "emulatorHandoff": "direct-system-menu-or-isshelf-library",
        "payloadAutoloadRequired": False,
        "eboot": {"bytes": signed.stat().st_size, "sha256": sha256(signed)},
        "relay": {"bytes": len(relay), "sha256": sha256(relay_elf)},
        "libc": {"bytes": module.stat().st_size, "sha256": sha256(module)},
        "nativeTool": {"sha256": sha256(tool), "buildKey": recorded_tool.get("key")},
        "inputs": inputs,
    }
    (OUTPUT / "build.json").write_text(json.dumps(report, indent=2) + "\n",
                                        encoding="utf-8")
    print(json.dumps(report, indent=2))
    return signed


if __name__ == "__main__":
    main()
