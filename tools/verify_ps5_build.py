# SPDX-License-Identifier: GPL-3.0-only
"""Independent structural checks; a PASS here is not a PS5 hardware test."""
import json
import struct
import base64
import hashlib
from ps5_common import CACHE, ROOT, sha256

PATH_PROBE_SOURCE_SHA256 = "5562cc7fd6d0e029601923d0281a3380dfaf7e3ef00866f904e525429a71197f"
PATH_PROBE_ELF_SHA256 = "46b5b8a259cf525c67e1efc3af9245ad90c07858cf726a1e1254fbd9524ee3ea"
PATH_PROBE_V1_ELF_SHA256 = "a267920271488c532df1d4e3d7afc3ce71d59cc5320d2ed83508f01bbc6f43e2"
PATH_PROBE_V1_ELF_BYTES = 109312
WRITE_PROBE_SOURCE_SHA256 = "1fce504c464262e2f8060e7d0fa3735013de79648448011ad25843e79e8eb84d"
WRITE_PROBE_ELF_SHA256 = "8e50ecb74c6421dcb2caae7a770a2465569f81cef279d85749b20894cc109c26"
WRITE_PROBE_V3_ELF_BYTES = 107368
WRITE_PROBE_V3_ELF_SHA256 = "d1a03e770cdd7a97cc4c3614a01337a4a822d9fe62dc5f5f4080b85616b716de"
# Extracted offline with the pinned native tool from the private UI36 package
# (source commit 2f8b6c41a0624b1d7f16793701bf52b6c98a1a04). The three
# extra imports below are solely for the fixed create-only probe request and
# no-follow receipt read. The UI37 app-side openat import resolved to a null
# GOT entry on hardware; UI38 forbids it. The separately built probe payload
# keeps its verified SDK runtime and unchanged pinned ELF.
UI36_BASELINE_SELF_SHA256 = "2f429cc25740a9332f8639191c056596aa243092484f5dbfdd34d3e597960cce"
UI36_BASELINE_ELF_SHA256 = "831556b5195d2c4d054c3dfc5563ae70638c78e92b1cec1e47bcf57084427255"
UI36_IMPORTS_SHA256 = "056a731e1b3901d2b0c6a92f90ef161b4d0ad2a2d494621fd52594c0e8fbdb3b"
# UI42 quarantines the old in-app console-tile writer on PS5. Relative to
# UI36, only libkernel rmdir and chmod disappear; open/read/write remain the
# three app-side imports for the private diagnostic bridge. This is the
# independently extracted, full 242-row NID/provider/library/binding set.
# The subsequent private job-store bridge adds exactly fdopendir from
# libSceLibcInternal and geteuid from libkernel, both with binding 18. The
# fixed private app job-root credential diagnostic adds getuid from libkernel.
UI42_QUARANTINED_IMPORTS_SHA256 = "fbffe89dc9b89bb2584f8a0621fde2b3f71ae4098cdddc5c052b2808f6d70990"
RELEASE_BASE_IMPORT_COUNT = 243
RELEASE_BASE_IMPORTS_SHA256 = "0e7c291fe32a28757f9af029026475805663919abc1ff3f76de4052c2ba89091"
OWNED_TILE_MARKER = b"ISSHELF_OWNED_TILE_WRITES_V1"
UPDATE_CLIENT_MARKER = b"ISSHELF_IN_APP_UPDATE_CLIENT_V1"
OWNER_SUPPORT_MARKER = b"ISSHELF_OWNER_SUPPORT_CONTROL_V1"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def embedded_elf_digest_occurrences(raw, size, digest):
    """Find an exact embedded ELF by length/digest without requiring old cache files."""
    magic = b"\x7fELF\x02\x01\x01\x00\x00"
    count = 0
    at = 0
    while (at := raw.find(magic, at)) >= 0:
        if at + size <= len(raw) and hashlib.sha256(raw[at:at + size]).hexdigest() == digest:
            count += 1
        at += 1
    return count


def public_nid(name):
    # Public SDK ABI symbol identifier, also documented by the pinned native
    # converter. This is a name hash, not a signing or decryption key.
    suffix = bytes.fromhex("518d64a635ded8c1e6b039b1c3e55230")
    digest = hashlib.sha1(name.encode("ascii") + suffix).digest()[:8][::-1]
    return base64.b64encode(digest).decode("ascii").rstrip("=").replace("/", "-")


def source_fingerprint():
    inputs = []
    for directory in ("native/src", "native/include", "native/ps5", "native/third_party/stb",
                      "native/third_party/ps5sx2_https",
                      "native/third_party/ps5-runtime", "native/third_party/cjson", "native/third_party/pl_mpeg",
                      "assets/backgrounds", "assets/branding", "assets/room"):
        inputs.extend(p for p in (ROOT / directory).rglob("*") if p.is_file())
    inputs += [ROOT / "tools" / name for name in ("build_ps5.py", "bootstrap_ps5_toolchain.py", "fetch_ps5_deps.py",
                                                   "ps5_common.py", "verify_ps5_build.py", "build_launch_helper.py",
                                                   "prepare_corresponding_source.py", "linked-sources.json", "build-home-art.js")]
    # package.json supplies ISSHELF_VERSION to the native compiler. Bind it as
    # a real build input so a version bump can never reuse an older fingerprint.
    inputs.append(ROOT / "package.json")
    inputs += [ROOT / "tools/console_badges" / name for name in (
        "generic_tile_pack_native.hpp", "generic_tile_catalog.generated.hpp",
        "generic_tile_pack_index.generated.hpp", "sha256_stream.hpp")]
    inputs += [ROOT / "tools/console_badges" / name for name in (
        "build_console_tile_path_probe_v2.py", "console_tile_path_probe_v2.cpp",
        "test_console_tile_path_probe_v2.py", "build_console_tile_write_probe_v4.py",
        "console_tile_write_probe_v4.cpp", "test_console_tile_write_probe_v4.py")]
    return {p.relative_to(ROOT).as_posix(): sha256(p) for p in sorted(inputs)}


def verify_recorded_hashes(folder):
    recorded = json.loads((folder / "structural-check.json").read_text())
    for filename, key in (("eboot.elf", "elf"), ("eboot.bin", "self")):
        file = folder / filename
        expected = recorded[key]
        require(file.stat().st_size == expected["bytes"] and sha256(file) == expected["sha256"],
                f"{filename} changed after its build/container validation; rebuild first")
    bridge = b"ISSHELF_PRIVATE_UI_TEST_BRIDGE_V1" in (folder / "eboot.elf").read_bytes()
    require(bridge == bool(recorded.get("uiTestBridge", False)), "Private UI test marker differs from build record")
    owner_support = OWNER_SUPPORT_MARKER in (folder / "eboot.elf").read_bytes()
    require(owner_support == bool(recorded.get("ownerSupportControl", False)),
            "Owner support marker differs from build record")
    owned_tiles = OWNED_TILE_MARKER in (folder / "eboot.elf").read_bytes()
    require(owned_tiles == bool(recorded.get("ownedTileWrites", False)),
            "Owned-tile marker differs from build record")
    exact_self_close = b"ISSHELF_EXACT_SELF_CLOSE_V1" in (folder / "eboot.elf").read_bytes()
    require(exact_self_close == bool(recorded.get("exactSelfCloseFallback", False)),
            "Exact self-close marker differs from build record")
    if "embeddedLaunchHelper" in recorded:
        helper = recorded["embeddedLaunchHelper"]
        raw = (folder / "eboot.elf").read_bytes()
        begin, size = helper["offset"], helper["bytes"]
        require(0 <= begin < len(raw) and 64 <= size <= 1048576 and begin + size <= len(raw), "Invalid helper extent")
        data = raw[begin:begin + size]
        require(data[:8] == b"\x7fELF\x02\x01\x01\x00" and hashlib.sha256(data).hexdigest() == helper["sha256"],
                "Embedded helper differs from its verified build")
        require(helper.get("exactOriginClose") == {
                    "titleId": "PPSA99541", "delayMs": 5000, "attempts": 1,
                    "marker": "ISSHELF_EXACT_ORIGIN_CLOSE_V1"},
                "Embedded helper exact-origin close contract differs")
        require(helper.get("exactUpdateRelaunch") == {
                    "titleId": "PPSA99541", "rounds": 1, "attemptsPerRound": 1,
                    "settleMs": 35000, "retryMs": 0, "observeMs": 30000,
                    "observeNegativeReturn": True,
                    "marker": "ISSHELF_EXACT_UPDATE_RELAUNCH_V4"},
                "Embedded helper update-relaunch contract differs")
        require(helper.get("updateDebugNotifications") == {
                    "style": "kernel-text", "downloadPercent": [25, 50, 75],
                    "helperStages": ["verify", "recover", "install", "restore", "finish", "relaunch"],
                    "idleWarningSeconds": 35,
                    "marker": "ISSHELF_UPDATE_DEBUG_NOTICES_V1"},
                "Embedded helper update-notification contract differs")
        require(data.count(b"ISSHELF_EXACT_UPDATE_RELAUNCH_V4") == 1 and
                data.count(b"Please don't open anything for %u seconds;") == 1,
                "Embedded helper lacks the protected 35-second update-relaunch evidence")
    private_probe = recorded.get("embeddedConsoleTilePathProbe")
    require((private_probe is not None) == bridge,
            "Read-only path probe must be embedded only in the private UI test build")
    if private_probe is not None:
        probe_folder = CACHE / "console-tile-path-probe-v2"
        probe_build = json.loads((probe_folder / "build.json").read_text())
        probe_file = probe_folder / "console-tile-path-probe-v2.elf"
        probe = probe_file.read_bytes()
        source = ROOT / "tools/console_badges/console_tile_path_probe_v2.cpp"
        require(sha256(source) == PATH_PROBE_SOURCE_SHA256 and
                probe_build["inputs"] == {source.relative_to(ROOT).as_posix(): PATH_PROBE_SOURCE_SHA256},
                "Read-only path probe source differs from the reviewed pin")
        require(probe_build["elf"]["bytes"] == len(probe) and
                probe_build["elf"]["sha256"] == hashlib.sha256(probe).hexdigest() == PATH_PROBE_ELF_SHA256 and
                sorted(probe_build["elf"]["needed"]) == ["libSceLibcInternal.sprx", "libkernel_web.sprx"] and
                probe_build["elf"]["openatPresent"] is False and
                probe_build["elf"]["standardSdkHooksVerified"] is True and
                probe_build["elf"]["mainEntryKlog"] == "ISSHELF_PATH_PROBE_V2_MAIN_ENTERED" and
                private_probe["bytes"] == len(probe) and
                private_probe["sha256"] == probe_build["elf"]["sha256"] and
                private_probe["version"] == 2 and
                sorted(private_probe["needed"]) == ["libSceLibcInternal.sprx", "libkernel_web.sprx"] and
                private_probe["openatPresent"] is False and
                private_probe["request"] == probe_build["request"] ==
                    "/data/console-shelf/console-tile-path-request-v2.txt" and
                private_probe["onConsoleReceipt"] == probe_build["onConsoleReceipt"] ==
                    "/data/console-shelf/console-tile-path-v2-<nonce>.txt" and
                private_probe["requestMode"] == private_probe["receiptMode"] ==
                    probe_build["requestMode"] == probe_build["receiptMode"] == "0644" and
                private_probe["runtimeMode"] == "standard-sdk" and
                private_probe["permissionChanges"] is True and
                private_probe["titleRootWriteAttempted"] is False and
                private_probe["databaseOpenAttempted"] is False and
                private_probe["networkAttempted"] is False,
                "Read-only path probe identity or scope differs from its build")
        raw = (folder / "eboot.elf").read_bytes()
        begin = private_probe["offset"]
        require(0 <= begin < len(raw) and 64 <= len(probe) <= 1048576 and
                begin + len(probe) <= len(raw) and raw[begin:begin + len(probe)] == probe and
                raw.count(probe) == 1 and
                embedded_elf_digest_occurrences(raw, PATH_PROBE_V1_ELF_BYTES, PATH_PROBE_V1_ELF_SHA256) == 0,
                "V2 path probe must be embedded exactly once, without the v1 payload")
    private_write = recorded.get("embeddedConsoleTileWriteProbe")
    require((private_write is not None) == bridge,
            "Scratch write probe must be embedded only in the private UI test build")
    if private_write is not None:
        folder_v4 = CACHE / "console-tile-write-probe-v4"
        build_v4 = json.loads((folder_v4 / "build.json").read_text())
        probe_v4 = (folder_v4 / "console-tile-write-probe-v4.elf").read_bytes()
        source_v4 = ROOT / "tools/console_badges/console_tile_write_probe_v4.cpp"
        require(sha256(source_v4) == WRITE_PROBE_SOURCE_SHA256 and
                build_v4["inputs"] == {source_v4.relative_to(ROOT).as_posix(): WRITE_PROBE_SOURCE_SHA256} and
                build_v4["elf"]["bytes"] == len(probe_v4) == private_write["bytes"] and
                build_v4["elf"]["sha256"] == hashlib.sha256(probe_v4).hexdigest() ==
                    private_write["sha256"] == WRITE_PROBE_ELF_SHA256 and
                sorted(private_write["needed"]) == ["libSceLibcInternal.sprx", "libkernel_web.sprx"] and
                private_write["atSyscalls"] == ["fstatat", "mkdirat", "openat", "renameat", "unlinkat"] and
                private_write["atSyscallReturn"] == "freebsd-carry-normalized" and
                set(private_write["atAdapterEvidence"]) ==
                    {"fstatat", "mkdirat", "openat", "renameat", "unlinkat"} and
                build_v4["errnoMacros"] ==
                    {"ENOENT": 2, "EEXIST": 17, "EIO": 5, "EINVAL": 22} and
                private_write["standardSdkHooksVerified"] is True and
                private_write["mainEntryKlog"] == "ISSHELF_WRITE_PROBE_V4_MAIN_ENTERED" and
                private_write["version"] == 4 and
                private_write["request"] == build_v4["request"] ==
                    "/data/console-shelf/console-tile-write-request-v4.txt" and
                private_write["onConsoleReceipt"] == build_v4["onConsoleReceipt"] ==
                    "/data/console-shelf/console-tile-write-v4-<nonce>.txt" and
                private_write["scratch"] == build_v4["scratch"] ==
                    "/data/homebrew/.isshelf-write-probe-v4-<nonce>" and
                private_write["requestMode"] == private_write["receiptMode"] == "0644" and
                private_write["validTitleIdWriteAttempted"] is False and
                private_write["homebrewScratchWritePossible"] is True and
                private_write["databaseOpenAttempted"] is False and
                private_write["networkAttempted"] is False,
                "V4 scratch write probe identity or scope differs from its build")
        raw = (folder / "eboot.elf").read_bytes()
        at = private_write["offset"]
        require(0 <= at < len(raw) and at + len(probe_v4) <= len(raw) and
                raw[at:at + len(probe_v4)] == probe_v4 and raw.count(probe_v4) == 1 and
                embedded_elf_digest_occurrences(raw, WRITE_PROBE_V3_ELF_BYTES,
                                                WRITE_PROBE_V3_ELF_SHA256) == 0,
                "V4 scratch write probe must be embedded exactly once")
    return recorded


def verify(elf_path, self_path):
    raw = elf_path.read_bytes()
    require(raw[:9] == b"\x7fELF\x02\x01\x01\x09\x02", "Expected PS5/FreeBSD ELF64 little-endian ABI")
    kind, machine, version, entry, phoff = struct.unpack_from("<HHIQQ", raw, 16)
    require((kind, machine, version) == (0xfe10, 62, 1), "Expected native PS5 x86-64 executable")
    ehsize, phsize, phcount = struct.unpack_from("<HHH", raw, 52)
    require(ehsize == 64 and phsize == 56 and 1 <= phcount <= 64, "Invalid ELF header geometry")
    require(phoff + phsize * phcount <= len(raw), "Program headers exceed file")
    headers = [struct.unpack_from("<IIQQQQQQ", raw, phoff + i * phsize) for i in range(phcount)]
    loads = [p for p in headers if p[0] == 1]
    require(loads and any((p[1] & 1) and p[3] <= entry < p[3] + p[6] for p in loads), "Entry is outside executable memory")
    for _, flags, offset, va, _, filesz, memsz, align in loads:
        require(filesz <= memsz and offset + filesz <= len(raw), "Invalid LOAD range")
        require(align == 0x4000 and offset % align == va % align, "Invalid PS5 page alignment")
        require(flags & 3 != 3, "A LOAD segment is both writable and executable")
    def virtual(address, length):
        for p in loads:
            if p[3] <= address and address + length <= p[3] + p[5]:
                return p[2] + address - p[3]
        raise ValueError("Dynamic address is outside file-backed LOAD memory")
    dynamic = [p for p in headers if p[0] == 2]
    require(len(dynamic) == 1, "Expected one dynamic segment")
    p = dynamic[0]
    require(p[2] + p[5] <= len(raw) and p[5] % 16 == 0, "Invalid dynamic segment range")
    tags = [struct.unpack_from("<qQ", raw, p[2] + n) for n in range(0, p[5], 16)]
    require(tags and tags[-1] == (0, 0), "Dynamic segment lacks terminator")
    table = dict(tags)
    start = virtual(table[5], table[10])
    strings = raw[start:start + table[10]]
    def string_at(offset):
        require(offset < len(strings), "String offset is outside metadata")
        end = strings.find(b"\0", offset)
        require(end >= offset, "Unterminated metadata string")
        return strings[offset:end].decode("ascii")

    # The first device test exposed a required weak-only import returning null.
    # Check the final converted ELF rather than trusting library names alone.
    symbol_bytes = table.get(0x6100003f, 0)
    require(table.get(11) == 24 and 0 < symbol_bytes <= 65536 and symbol_bytes % 24 == 0,
            "Invalid native symbol table extent")
    symbol_start = virtual(table[6], symbol_bytes)
    modules, libraries = {}, {}
    alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
    for tag, value in tags:
        if tag in (0x61000045, 0x61000049):
            module_id = value >> 48
            require(module_id < 64, "Unexpected native module identity")
            destination = modules if tag == 0x61000045 else libraries
            require(alphabet[module_id] not in destination, "Duplicate native module/library identity")
            destination[alphabet[module_id]] = string_at(value & 0xffffffff)
    imported = set()
    imported_count = 0
    for offset in range(0, symbol_bytes, 24):
        name, info, other, section, value, size = struct.unpack_from("<IBBHQQ", raw, symbol_start + offset)
        if section != 0:
            continue
        parts = string_at(name).split("#")
        if len(parts) != 3:
            continue
        provider = modules.get(parts[2])
        library = libraries.get(parts[1])
        require(provider is not None and library is not None, "Import has an unknown module or library")
        imported_count += 1
        imported.add((parts[0], provider, library, str(info)))
    require(imported_count == len(imported), "Duplicate native import binding")
    require(not any(row[0] == public_nid("openat") for row in imported),
            "The app must not import openat from any provider")
    require(raw.count(UPDATE_CLIENT_MARKER) == 1,
            "The reviewed in-app update client marker must appear exactly once")
    updater_imports = {
        (public_nid(name), provider, provider, "18")
        for provider, names in {
            "libSceLibcInternal": (
                "aligned_alloc", "wcstoll", "wcstoul", "wcstod", "wcstof", "__udivti3",
                "wcstoull", "wcstold", "wmemcmp", "fputs", "strerror_r", "wcstol",
                "wmemchr", "swprintf", "strtold", "vfprintf", "strtof"),
            "libkernel": (
                "__inet_ntop", "read", "write", "pthread_rwlock_unlock",
                "pthread_mutexattr_destroy", "unlink", "pthread_once",
                "pthread_rwlock_rdlock", "poll", "pthread_rwlock_wrlock"),
            "libSceNet": (
                "sceNetInit", "sceNetPoolCreate", "sceNetPoolDestroy",
                "sceNetResolverCreate", "sceNetResolverStartNtoa",
                "sceNetResolverDestroy"),
        }.items()
        for name in names
    }
    updater_static_replacements = {
        (public_nid(name), provider, provider, "18")
        for provider, names in {
            "libSceLibcInternal": ("__cxa_guard_acquire", "__cxa_guard_release", "strcasecmp"),
            "libkernel": ("dlopen", "dlsym", "dlclose", "dlerror"),
        }.items()
        for name in names
    }
    missing_updater_imports = updater_imports - imported
    require(not missing_updater_imports,
            "The marked update client is missing an exact reviewed network/runtime import: " +
            ", ".join(sorted(row[0] for row in missing_updater_imports)))
    require(not (updater_static_replacements & imported),
            "A function statically supplied by the update runtime unexpectedly remains imported")
    private = b"ISSHELF_PRIVATE_UI_TEST_BRIDGE_V1" in raw
    owner_support = raw.count(OWNER_SUPPORT_MARKER) == 1
    require(raw.count(OWNER_SUPPORT_MARKER) == 1,
            "Physical L3+R3 owner support marker must appear exactly once")
    exact_self_close_marker = b"ISSHELF_EXACT_SELF_CLOSE_V1"
    require(raw.count(exact_self_close_marker) == 1,
            "Exact self-close contract marker must appear exactly once")
    self_close_import_names = (
        "sceLncUtilInitialize", "sceLncUtilGetAppIdOfRunningBigApp",
        "sceLncUtilGetAppTitleId", "sceLncUtilKillApp")
    self_close_imports = {
        (public_nid(name), "libSceSystemService", "libSceSystemService", "18")
        for name in self_close_import_names
    }
    require(self_close_imports <= imported,
            "Exact self-close imports are missing or bound to another provider")
    owned_tiles = raw.count(OWNED_TILE_MARKER) == 1
    require(raw.count(OWNED_TILE_MARKER) <= 1, "Owned-tile marker is duplicated")
    require(not private or embedded_elf_digest_occurrences(raw, PATH_PROBE_V1_ELF_BYTES,
                                                           PATH_PROBE_V1_ELF_SHA256) == 0,
            "Private build still embeds the v1 path probe")
    # Owned-tile writes need only permission repair and removal of known
    # directories. Cleanup deliberately avoids directory enumeration: it
    # deletes only pinned owned paths and lets rmdir retain unknown extras.
    tile_write_imports = {
        (public_nid("rmdir"), "libkernel", "libkernel", "18"),
        (public_nid("chmod"), "libkernel", "libkernel", "18"),
    }
    require(not any(row[0] == public_nid("getdirentries") for row in imported),
            "Owned-tile cleanup must not enumerate directory contents")
    require(not any(row[0] == public_nid("fdopendir") for row in imported),
            "Quarantined app job-root directory walker import remains")
    if owned_tiles:
        require(tile_write_imports <= imported,
                "Owned-tile imports are missing or rebound")
    else:
        require(not (tile_write_imports & imported),
                "Quarantined tile-writer imports remain in the PS5 app")

    bridge_imports = {
        (public_nid("open"), "libkernel", "libkernel", "18"),
        (public_nid("read"), "libkernel", "libkernel", "18"),
        (public_nid("write"), "libkernel", "libkernel", "18"),
        (public_nid("fchmod"), "libkernel", "libkernel", "18"),
    }
    bridge_only_imports = bridge_imports - {
        (public_nid("open"), "libkernel", "libkernel", "18"),
        (public_nid("read"), "libkernel", "libkernel", "18"),
        (public_nid("write"), "libkernel", "libkernel", "18"),
    }
    ps2_probe_marker = b"probe-ps2-source\x00"
    marker_count = raw.count(ps2_probe_marker)
    require(marker_count <= 1, "Protected-source probe action marker is duplicated")
    direct_action_count = raw.count(b"probe-ps2-direct\x00")
    direct_result_count = raw.count(b"ps2-direct-matched\x00")
    require(direct_action_count <= 1 and direct_result_count <= 1 and
            direct_action_count == direct_result_count and
            (not direct_action_count or marker_count == 1),
            "Protected-source direct action/result markers differ")
    ps2_probe_imports = ({
        (public_nid("getuid"), "libkernel", "libkernel", "18"),
        (public_nid("geteuid"), "libkernel", "libkernel", "18"),
    } if marker_count == 1 else set())

    private_import_contract = None
    public_import_contract = None
    if private:
        # UI48's private app-owned scratch source adds only fchmod to the
        # quarantined UI42 contract and its three existing bridge imports.
        # mkdir, fsync, lstat and fstat are already in the pinned baseline.
        require(bridge_imports <= imported,
                "Private path probe imports are missing or bound to another provider")
        # UI51's fixed, read-only protected-source diagnostic reports the app
        # process UID/EUID. Admit those two libkernel queries only when its
        # exact action marker is present; the old job-root directory walker
        # remains quarantined in every private build.
        require(ps2_probe_imports <= imported,
                "Protected-source UID queries are missing or rebound")
        if not marker_count:
            require(not any(row[0] in {public_nid("getuid"), public_nid("geteuid")}
                            for row in imported),
                    "UID queries require the fixed protected-source action")
        private_import_contract = {
            "baselineSourceCommit": "2f8b6c41a0624b1d7f16793701bf52b6c98a1a04",
            "baselineSelfSHA256": UI36_BASELINE_SELF_SHA256,
            "baselineExtractedElfSHA256": UI36_BASELINE_ELF_SHA256,
            "baselineImportCount": 244,
            "baselineImportSHA256": UI36_IMPORTS_SHA256,
            "quarantinedImportCount": 242,
            "quarantinedImportSHA256": UI42_QUARANTINED_IMPORTS_SHA256,
            "releaseBaselineImportCount": RELEASE_BASE_IMPORT_COUNT,
            "releaseBaselineImportSHA256": RELEASE_BASE_IMPORTS_SHA256,
            "removed": ([] if owned_tiles else
                        [{"name": name, "nid": public_nid(name), "provider": "libkernel"}
                         for name in ("rmdir", "chmod")]),
            "forbiddenAppImport": {"name": "openat", "nid": public_nid("openat")},
            "added": ([{"name": name, "nid": public_nid(name), "provider": "libkernel",
                        "library": "libkernel", "binding": "18"}
                       for name in (("read", "write", "fchmod") +
                                    (("getuid", "geteuid") if marker_count else ()) +
                                    (("rmdir", "chmod") if owned_tiles else ()))] +
                      [{"name": name, "nid": public_nid(name),
                        "provider": "libSceSystemService", "library": "libSceSystemService",
                        "binding": "18"} for name in self_close_import_names]),
            "appJobRootIo": "absent",
            "protectedSourceUidQueries": bool(marker_count),
            "protectedSourceDirectRead": bool(direct_action_count),
            "appSourceScratch": "private-create-only-no-title-registration",
            "ownedTileWrites": owned_tiles,
            "exactSelfCloseFallback": True,
            "selfCloseTitle": "PPSA99541",
            "selfCloseDelayMs": 3000,
            "selfCloseAttempts": 1,
            "inAppUpdater": {
                "marker": UPDATE_CLIENT_MARKER.decode("ascii"),
                "requiredImportCount": len(updater_imports),
                "staticReplacementCount": len(updater_static_replacements),
                "sharedPrivateImports": ["read", "write"],
            },
        }
    else:
        require(not (bridge_only_imports & imported),
                "Private UI bridge imports remain in the public PS5 app")
        require(marker_count == direct_action_count == direct_result_count == 0,
                "Private protected-source diagnostics remain in the public PS5 app")
        require(not any(row[0] in {public_nid("getuid"), public_nid("geteuid")}
                        for row in imported),
                "Private protected-source identity imports remain in the public PS5 app")

    baseline = ((imported - self_close_imports - updater_imports -
                (tile_write_imports if owned_tiles else set()) -
                (bridge_only_imports if private else set()) -
                (ps2_probe_imports if private else set())) |
                updater_static_replacements)
    canonical = "".join("\t".join(row) + "\n" for row in sorted(baseline)).encode("ascii")
    require(len(baseline) == RELEASE_BASE_IMPORT_COUNT and
            hashlib.sha256(canonical).hexdigest() == RELEASE_BASE_IMPORTS_SHA256,
            "Build differs from the reviewed application import set plus explicitly admitted features; " +
            f"observed count={len(baseline)} sha256={hashlib.sha256(canonical).hexdigest()}")
    if not private:
        public_import_contract = {
            "baselineSourceCommit": "2f8b6c41a0624b1d7f16793701bf52b6c98a1a04",
            "baselineImportCount": RELEASE_BASE_IMPORT_COUNT,
            "baselineImportSHA256": RELEASE_BASE_IMPORTS_SHA256,
            "added": ([{"name": name, "nid": public_nid(name), "provider": "libkernel",
                        "library": "libkernel", "binding": "18"}
                       for name in (("rmdir", "chmod") if owned_tiles else ())] +
                      [{"name": name, "nid": public_nid(name),
                        "provider": "libSceSystemService", "library": "libSceSystemService",
                        "binding": "18"} for name in self_close_import_names]),
            "forbiddenAppImports": [
                {"name": name, "nid": public_nid(name)}
                for name in ("openat", "getdirentries", "fdopendir", "getuid", "geteuid",
                             "fchmod")
            ],
            "inAppUpdater": {
                "marker": UPDATE_CLIENT_MARKER.decode("ascii"),
                "requiredImportCount": len(updater_imports),
                "staticReplacementCount": len(updater_static_replacements),
            },
            "ownedTileWrites": owned_tiles,
            "ownerSupportControl": owner_support,
            "uiTestBridge": False,
            "exactSelfCloseFallback": True,
        }
    required_imports = []
    for required in (("sceSystemServiceLoadExec", "sceSystemServiceGetStatus") +
                     self_close_import_names):
        prefix = public_nid(required) + "#"
        matches = []
        for offset in range(0, symbol_bytes, 24):
            name, info, other, section, value, size = struct.unpack_from("<IBBHQQ", raw, symbol_start + offset)
            label = string_at(name)
            if label.startswith(prefix):
                matches.append((label, info, section))
        require(len(matches) == 1, required + " import is missing or duplicated")
        label, info, section = matches[0]
        require(info >> 4 == 1 and info & 15 == 2 and section == 0,
                required + " import must be a strong undefined function")
        parts = label.split("#")
        require(len(parts) == 3 and modules.get(parts[2]) == "libSceSystemService" and
                libraries.get(parts[1]) == "libSceSystemService",
                required + " import is bound to the wrong system module")
        required_imports.append({"name": required, "binding": "global", "provider": modules[parts[2]],
                                 "library": libraries[parts[1]]})
    needed = []
    for tag, offset in tags:
        if tag == 1:
            require(offset < len(strings), "Module name is outside string table")
            end = strings.find(b"\0", offset)
            require(end > offset, "Unterminated module name")
            needed.append(strings[offset:end].decode("ascii"))
    allowed = {"libSceAudioOut.prx", "libSceLibcInternal.prx", "libSceNet.prx", "libScePad.prx", "libSceSystemService.prx",
               "libSceUserService.prx", "libSceVideoOut.prx", "libkernel.prx"}
    require(set(needed) == allowed, f"Unexpected system dependency set: {needed}")
    signed = self_path.read_bytes()
    require(signed[:4] == bytes.fromhex("4f153d1d"), "Expected the native plaintext homebrew FSELF format")
    require(b"\x7fELF\x02\x01\x01\x09\x02" in signed[:4096], "SELF lacks its native ELF header")
    return {"structuralCheck": "passed", "hardwareTest": "not run", "firmware12_20": "unverified",
            "ownerSupportControl": owner_support, "uiTestBridge": private, "ownedTileWrites": owned_tiles,
            "exactSelfCloseFallback": True, "selfCloseTitle": "PPSA99541",
            "selfCloseDelayMs": 3000, "selfCloseAttempts": 1,
            "privateImportContract": private_import_contract,
            "publicImportContract": public_import_contract,
            "elf": {"bytes": len(raw), "sha256": sha256(elf_path), "programHeaders": phcount,
                    "loadSegments": len(loads), "neededModules": needed,
                    "requiredShellImport": required_imports[0], "requiredVisibilityImport": required_imports[1],
                    "requiredSelfCloseImports": required_imports[2:]},
            "self": {"bytes": len(signed), "sha256": sha256(self_path)}}


if __name__ == "__main__":
    folder = CACHE / "build"
    identity = json.loads((folder / "build-source.json").read_text())
    require(identity == source_fingerprint(), "Sources changed since this executable was built; rebuild first")
    recorded = verify_recorded_hashes(folder)
    result = verify(folder / "eboot.elf", folder / "eboot.bin")
    if "embeddedLaunchHelper" in recorded:
        result["embeddedLaunchHelper"] = recorded["embeddedLaunchHelper"]
    if "embeddedConsoleTilePathProbe" in recorded:
        result["embeddedConsoleTilePathProbe"] = recorded["embeddedConsoleTilePathProbe"]
    if "embeddedConsoleTileWriteProbe" in recorded:
        result["embeddedConsoleTileWriteProbe"] = recorded["embeddedConsoleTileWriteProbe"]
    (folder / "structural-check.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS: independent PS5 ELF identity, segment bounds/alignment, executable entry, dependency set and SELF wrapper.")
    print("Firmware 12.20 and physical controller/display remain unverified.")
