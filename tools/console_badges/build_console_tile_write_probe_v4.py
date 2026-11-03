# SPDX-License-Identifier: GPL-3.0-only
"""Build the offline v4 scratch write-capability probe; never deploy here."""

import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import zipfile

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from bootstrap_ps5_toolchain import environment, prepare as prepare_compiler  # noqa: E402
from fetch_ps5_deps import prepare as prepare_dependencies  # noqa: E402
from prepare_corresponding_source import prepare_sdk_source  # noqa: E402
from ps5_common import CACHE, ROOT, run, sha256  # noqa: E402
from verify_ps5_build import require  # noqa: E402


def verify_elf(elf: Path, crt1: Path, expected_imports: set[str] | None) -> dict:
    raw = elf.read_bytes()
    require(raw[:9] == crt1.read_bytes()[:9] == b"\x7fELF\x02\x01\x01\x00\x00",
            "Wrong pinned SDK payload ABI")
    kind, machine = struct.unpack_from("<HH", raw, 16)
    require((kind, machine) == (3, 62), "Expected x86-64 dynamic ELF payload")
    entry, phoff = struct.unpack_from("<QQ", raw, 24)
    phsize, phcount = struct.unpack_from("<HH", raw, 54)
    require(phsize == 56 and 1 <= phcount <= 16 and phoff + phsize * phcount <= len(raw),
            "Invalid program header table")
    segments = [struct.unpack_from("<IIQQQQQQ", raw, phoff + i * phsize) for i in range(phcount)]
    loads = [p for p in segments if p[0] == 1]
    require(any(p[1] & 1 and p[3] <= entry < p[3] + p[6] for p in loads),
            "Entry outside executable segment")
    for p in loads:
        require(p[5] <= p[6] and p[2] + p[5] <= len(raw) and p[7] == 0x4000 and
                p[2] % p[7] == p[3] % p[7], "Invalid load segment")
    dynamic = [p for p in segments if p[0] == 2]
    require(len(dynamic) == 1 and dynamic[0][2] + dynamic[0][5] <= len(raw) and
            dynamic[0][5] % 16 == 0, "Missing or invalid dynamic table")
    tags = []
    for at in range(dynamic[0][2], dynamic[0][2] + dynamic[0][5], 16):
        tag, value = struct.unpack_from("<QQ", raw, at)
        if tag == 0:
            break
        tags.append((tag, value))
    string_tables = [value for tag, value in tags if tag == 5]
    string_sizes = [value for tag, value in tags if tag == 10]
    require(len(string_tables) == len(string_sizes) == 1,
            "Missing dynamic string table or size")
    string_address, string_size = string_tables[0], string_sizes[0]
    string_segment = next((p for p in loads if p[3] <= string_address and
                           string_address + string_size <= p[3] + p[5]), None)
    require(string_segment is not None, "Dynamic strings outside file-backed segment")
    string_offset = string_segment[2] + string_address - string_segment[3]
    needed = []
    for tag, offset in tags:
        if tag == 1:
            require(offset < string_size, "Invalid needed library offset")
            start, end = string_offset + offset, string_offset + string_size
            require(b"\0" in raw[start:end], "Unterminated needed library name")
            needed.append(raw[start:raw.index(0, start, end)].decode("ascii"))
    require(sorted(needed) == ["libSceLibcInternal.sprx", "libkernel_web.sprx"],
            f"Unexpected v4 runtime dependencies: {needed}")
    shoff = struct.unpack_from("<Q", raw, 40)[0]
    shsize, shcount = struct.unpack_from("<HH", raw, 58)
    require(shsize == 64 and 1 <= shcount <= 256 and shoff + shsize * shcount <= len(raw),
            "Invalid section table")
    sections = [struct.unpack_from("<IIQQQQIIQQ", raw, shoff + i * shsize) for i in range(shcount)]
    symbols = {}
    for section in sections:
        if section[1] != 2:
            continue
        require(section[6] < shcount and section[9] == 24 and section[4] + section[5] <= len(raw),
                "Invalid symbol table")
        strings = sections[section[6]]
        require(strings[4] + strings[5] <= len(raw), "Invalid symbol strings")
        names = raw[strings[4]:strings[4] + strings[5]]
        for offset in range(0, section[5], 24):
            name, info, _, owner, address, size = struct.unpack_from("<IBBHQQ", raw, section[4] + offset)
            require(name < len(names) and b"\0" in names[name:], "Invalid symbol name")
            label = names[name:names.index(0, name)].decode("ascii")
            symbols[label] = (info, owner, address, size)
    for hook in ("__patch_init", "__rtld_so_init", "__rtld_so_new"):
        require(hook in symbols, "Missing unmodified standard SDK startup hook")
        info, owner, address, size = symbols[hook]
        require(info == 0x12 and owner != 0 and size > 3,
                "Standard SDK hook unexpectedly replaced by restricted stub")
        segment = next((p for p in loads if p[1] & 1 and p[3] <= address and
                        address + size <= p[3] + p[5]), None)
        require(segment is not None, "Standard SDK hook outside executable bytes")
        begin = segment[2] + address - segment[3]
        require(raw[begin:begin + size] != b"\x31\xc0\xc3", "Restricted hook found")
    require("klog_puts" in symbols and symbols["klog_puts"][1] != 0 and
            "klog_printf" in symbols and symbols["klog_printf"][1] != 0,
            "Missing SDK main-entry and failure log routines")
    checked_syscalls = {"fstatat": 493, "mkdirat": 496, "openat": 499,
                        "renameat": 501, "unlinkat": 503}
    adapter_evidence = {}
    for operation, number in checked_syscalls.items():
        require(operation not in symbols, f"Unsafe raw SDK {operation} wrapper linked")
        matches = [(name, item) for name, item in symbols.items()
                   if f"checked_{operation}" in name and item[1] != 0 and item[3] > 0]
        require(len(matches) == 1, f"Missing carry-normalizing {operation} adapter")
        name, (_, _, address, size) = matches[0]
        segment = next((p for p in loads if p[1] & 1 and p[3] <= address and
                        address + size <= p[3] + p[5]), None)
        require(segment is not None, f"{operation} adapter outside executable bytes")
        begin = segment[2] + address - segment[3]
        body = raw[begin:begin + size]
        number_at = body.find(b"\xb8" + struct.pack("<I", number))
        syscall_at = body.find(b"\x0f\x05", number_at + 5)
        carry_at = body.find(b"\x0f\x92", syscall_at + 2)
        require(0 <= number_at < syscall_at < carry_at,
                f"{operation} does not bind syscall {number} to carry capture")
        adapter_evidence[operation] = {"syscall": number, "symbol": name,
                                       "syscallOffset": syscall_at,
                                       "carryCaptureOffset": carry_at}
    imports = {name for name, (_, owner, _, _) in symbols.items() if name and owner == 0}
    if expected_imports is not None:
        require(imports == expected_imports, f"Unexpected external imports: {sorted(imports)}")
    require(not any(name.startswith(("sceNet", "socket", "connect", "send", "recv"))
                    for name in imports), "Unexpected network import")
    return {"bytes": len(raw), "sha256": sha256(elf), "programHeaders": phcount,
            "loadSegments": len(loads), "standardSdkHooksVerified": True,
            "needed": needed,
            "atSyscalls": ["fstatat", "mkdirat", "openat", "renameat", "unlinkat"],
            "atSyscallReturn": "freebsd-carry-normalized",
            "atAdapterEvidence": adapter_evidence,
            "mainEntryKlog": "ISSHELF_WRITE_PROBE_V4_MAIN_ENTERED",
            "externalImports": sorted(imports)}


def main() -> None:
    source = ROOT / "tools/console_badges/console_tile_write_probe_v4.cpp"
    original = {source.relative_to(ROOT).as_posix(): sha256(source)}
    prefix = prepare_compiler()
    sdk, _, _ = prepare_dependencies()
    _, sdk_source = prepare_sdk_source()
    env = environment(prefix)
    macros = run([prefix / "usr/bin/clang++-18", "-target", "x86_64-sie-ps5",
                  "-isysroot", sdk, "-isystem", sdk / "target/include/c++/v1",
                  "-isystem", sdk / "target/include", "-std=c++17", "-E", "-dM", source],
                 env=env, capture_output=True, text=True).stdout
    errno_macros = {name: number for name, number in
                    (("ENOENT", 2), ("EEXIST", 17), ("EIO", 5), ("EINVAL", 22))}
    for name, number in errno_macros.items():
        require(re.search(rf"^#define {name}\s+{number}$", macros, re.MULTILINE) is not None,
                f"Pinned target preprocessor changed {name}")
    output = CACHE / "console-tile-write-probe-v4"
    output.mkdir(parents=True, exist_ok=True)
    pinned = {}
    with zipfile.ZipFile(CACHE / "downloads/ps5-payload-sdk-v0.42.zip") as archive:
        for name in ("crt1.o", "libc.a", "libkernel_web.so", "libSceLibcInternal.so"):
            data = archive.read("ps5-payload-sdk/target/lib/" + name)
            require((sdk / "target/lib" / name).read_bytes() == data,
                    "SDK link input differs from pinned release")
            pinned[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    obj = output / "console_tile_write_probe_v4.o"
    elf = output / "console-tile-write-probe-v4.elf"
    run([prefix / "usr/bin/clang++-18", "-target", "x86_64-sie-ps5", "-isysroot", sdk,
         "-isystem", sdk / "target/include/c++/v1", "-isystem", sdk / "target/include",
         "-fvisibility-nodllstorageclass=default", "-fno-stack-protector", "-fno-plt",
         "-femulated-tls", "-std=c++17", "-O2", "-fno-exceptions", "-fno-rtti",
         "-DNDEBUG", "-Wall", "-Wextra", "-Werror", "-c", source, "-o", obj], env=env)
    run([prefix / "usr/bin/ld.lld-18", "-m", "elf_x86_64", "-pie", "-T",
         sdk / "ldscripts/elf_x86_64.x", "--eh-frame-hdr", "-z", "max-page-size=0x4000",
         "-mllvm", "-emulated-tls", "--hash-style=gnu", "--gc-sections", "-o", elf,
         obj, sdk / "target/lib/crt1.o", "-L", sdk / "target/lib", "-lc",
         "-lkernel_web", "-lSceLibcInternal"], env=env)
    require(original == {source.relative_to(ROOT).as_posix(): sha256(source)},
            "Probe source changed during build")
    expected_imports = {"__error", "close", "fchmod", "fstat", "fsync", "lseek", "lstat",
                        "memcmp", "memset", "open", "read", "snprintf", "vsnprintf", "write"}
    checked = verify_elf(elf, sdk / "target/lib/crt1.o", expected_imports)
    receipt = {"elf": checked, "inputs": original, "errnoMacros": errno_macros,
               "sdkRuntimeSource": sdk_source,
               "sdkLinkInputs": pinned, "runtimeMode": "standard-sdk",
               "permissionChanges": True,
               "crtPatchHook": "Unmodified SDK v0.42 crt1.o patches only this payload process",
               "request": "/data/console-shelf/console-tile-write-request-v4.txt",
               "onConsoleReceipt": "/data/console-shelf/console-tile-write-v4-<nonce>.txt",
               "scratch": "/data/homebrew/.isshelf-write-probe-v4-<nonce>",
               "requestMode": "0644", "receiptMode": "0644",
               "phaseExitCodes": list(range(40, 77)),
               "validTitleIdWriteAttempted": False, "homebrewScratchWritePossible": True,
               "databaseOpenAttempted": False,
               "networkAttempted": False, "hardwareTest": "not run",
               "transport": "not connected", "deployed": False}
    (output / "build.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"elf": str(elf), **checked, "hardwareTest": "not run"}, indent=2))


if __name__ == "__main__":
    main()
