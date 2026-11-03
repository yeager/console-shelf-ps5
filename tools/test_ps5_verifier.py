# SPDX-License-Identifier: GPL-3.0-only
"""Reject damaged/unexpected executables using copies of a real cross-build."""
import struct
import shutil
import uuid
from ps5_common import CACHE
from verify_ps5_build import verify, verify_recorded_hashes, public_nid

build = CACHE / "build"
elf = build / "eboot.elf"
self_file = build / "eboot.bin"
verify(elf, self_file)
folder = CACHE / "verifier-tests" / str(uuid.uuid4())
folder.mkdir(parents=True)
original = elf.read_bytes()


def reject(name, mutation):
    damaged = bytearray(original)
    mutation(damaged)
    file = folder / (name + ".elf")
    file.write_bytes(damaged)
    try:
        verify(file, self_file)
    except (ValueError, struct.error):
        return
    raise RuntimeError(f"Verifier accepted a damaged fixture: {name}")


reject("magic", lambda b: b.__setitem__(0, 0))
reject("architecture", lambda b: struct.pack_into("<H", b, 18, 3))
reject("entry-outside-code", lambda b: struct.pack_into("<Q", b, 24, 0xffffffffffffffff))
phoff = struct.unpack_from("<Q", original, 32)[0]
reject("writable-executable", lambda b: struct.pack_into("<I", b, phoff + 4, 7))
reject("load-exceeds-file", lambda b: struct.pack_into("<Q", b, phoff + 32, len(original) * 2))
module = original.index(b"libScePad.prx")
reject("foreign-module", lambda b: b.__setitem__(slice(module, module + 13), b"libBadPad.prx"))
headers = [struct.unpack_from("<IIQQQQQQ", original, phoff + i * 56)
           for i in range(struct.unpack_from("<H", original, 56)[0])]
dynamic = next(p for p in headers if p[0] == 2)
tags = dict(struct.unpack_from("<qQ", original, dynamic[2] + offset)
            for offset in range(0, dynamic[5], 16))
def file_offset(address):
    p = next(p for p in headers if p[0] == 1 and p[3] <= address < p[3] + p[5])
    return p[2] + address - p[3]
strings = file_offset(tags[5])
shell_name = original.index(b"JoBqSQt1yyA#", strings, strings + tags[10])
symbols = file_offset(tags[6])
shell_symbol = next(symbols + offset for offset in range(0, tags[0x6100003f], 24)
                    if struct.unpack_from("<I", original, symbols + offset)[0] == shell_name - strings)
reject("required-shell-import-weak", lambda b: b.__setitem__(shell_symbol + 4, 0x22))
reject("required-shell-import-missing", lambda b: b.__setitem__(shell_name, ord("X")))
shell_end = original.index(b"\0", shell_name)
reject("required-shell-import-wrong-provider", lambda b: b.__setitem__(shell_end - 1, ord("H")))
reject("required-shell-import-wrong-library", lambda b: b.__setitem__(shell_end - 3, ord("G")))
visibility_name = original.index((public_nid("sceSystemServiceGetStatus") + "#").encode(), strings, strings + tags[10])
visibility_symbol = next(symbols + offset for offset in range(0, tags[0x6100003f], 24)
                         if struct.unpack_from("<I", original, symbols + offset)[0] == visibility_name - strings)
reject("required-visibility-import-weak", lambda b: b.__setitem__(visibility_symbol + 4, 0x22))
reject("required-visibility-import-missing", lambda b: b.__setitem__(visibility_name, ord("X")))
self_close_marker = original.index(b"ISSHELF_EXACT_SELF_CLOSE_V1")
reject("exact-self-close-marker-missing", lambda b: b.__setitem__(self_close_marker, ord("X")))
update_marker = original.index(b"ISSHELF_IN_APP_UPDATE_CLIENT_V1")
reject("update-client-marker-missing", lambda b: b.__setitem__(update_marker, ord("X")))
owner_support_marker = original.index(b"ISSHELF_OWNER_SUPPORT_CONTROL_V1")
reject("owner-support-marker-missing", lambda b: b.__setitem__(owner_support_marker, ord("X")))
for update_name in ("aligned_alloc", "poll", "sceNetPoolCreate", "read", "write"):
    update_import = original.index((public_nid(update_name) + "#").encode(),
                                   strings, strings + tags[10])
    reject("update-client-" + update_name + "-missing",
           lambda b, offset=update_import: b.__setitem__(offset, ord("X")))
for static_name in ("__cxa_guard_acquire", "__cxa_guard_release", "strcasecmp",
                    "dlopen", "dlsym", "dlclose", "dlerror"):
    assert (public_nid(static_name) + "#").encode() not in original[strings:strings + tags[10]]
for self_close_name in ("sceLncUtilInitialize", "sceLncUtilGetAppIdOfRunningBigApp",
                        "sceLncUtilGetAppTitleId", "sceLncUtilKillApp"):
    self_close_import = original.index((public_nid(self_close_name) + "#").encode(),
                                       strings, strings + tags[10])
    self_close_symbol = next(symbols + offset for offset in range(0, tags[0x6100003f], 24)
                             if struct.unpack_from("<I", original, symbols + offset)[0] ==
                             self_close_import - strings)
    reject("exact-self-close-" + self_close_name + "-weak",
           lambda b, offset=self_close_symbol: b.__setitem__(offset + 4, 0x22))
    reject("exact-self-close-" + self_close_name + "-missing",
           lambda b, offset=self_close_import: b.__setitem__(offset, ord("X")))
private_marker = original.find(b"ISSHELF_PRIVATE_UI_TEST_BRIDGE_V1")
private_import_fixtures = 0
for private_name in ("open", "read", "write", "fchmod"):
    encoded = (public_nid(private_name) + "#").encode()
    if private_marker >= 0:
        private_import = original.index(encoded, strings, strings + tags[10])
        reject("private-" + private_name + "-missing",
               lambda b, offset=private_import: b.__setitem__(offset, ord("X")))
        private_import_fixtures += 1
    elif private_name in ("open", "read", "write"):
        assert encoded in original[strings:strings + tags[10]]
    else:
        assert encoded not in original[strings:strings + tags[10]]
owned_marker = original.find(b"ISSHELF_OWNED_TILE_WRITES_V1")
if owned_marker >= 0:
    reject("owned-tile-marker-missing", lambda b: b.__setitem__(owned_marker, ord("X")))
    for write_name in ("rmdir", "chmod"):
        write_import = original.index((public_nid(write_name) + "#").encode(),
                                      strings, strings + tags[10])
        reject("owned-tile-" + write_name + "-missing",
               lambda b, offset=write_import: b.__setitem__(offset, ord("X")))
assert (public_nid("fdopendir") + "#").encode() not in original[strings:strings + tags[10]]
assert (public_nid("getdirentries") + "#").encode() not in original[strings:strings + tags[10]]
ps2_action = original.find(b"probe-ps2-source\x00")
identity_import_fixtures = 0
if ps2_action >= 0:
    reject("protected-source-action-missing", lambda b: b.__setitem__(ps2_action, ord("X")))
    for identity_name in ("getuid", "geteuid"):
        identity_import = original.index((public_nid(identity_name) + "#").encode(), strings, strings + tags[10])
        reject("protected-source-" + identity_name + "-missing",
               lambda b, offset=identity_import: b.__setitem__(offset, ord("X")))
        identity_import_fixtures += 1
else:
    for identity_name in ("getuid", "geteuid"):
        assert (public_nid(identity_name) + "#").encode() not in original[strings:strings + tags[10]]
direct_action = original.find(b"probe-ps2-direct\x00")
direct_result = original.find(b"ps2-direct-matched\x00")
assert (direct_action >= 0) == (direct_result >= 0)
if direct_action >= 0:
    reject("protected-source-direct-action-missing",
           lambda b: b.__setitem__(direct_action, ord("X")))
    reject("protected-source-direct-result-missing",
           lambda b: b.__setitem__(direct_result, ord("X")))
bad_self = folder / "bad-wrapper.bin"
bad_self.write_bytes(b"BAD!" + self_file.read_bytes()[4:])
try:
    verify(elf, bad_self)
except ValueError:
    pass
else:
    raise RuntimeError("Verifier accepted a foreign SELF wrapper")
recorded = folder / "artifact-hash"
recorded.mkdir()
for name in ("eboot.elf", "eboot.bin", "structural-check.json"):
    shutil.copyfile(build / name, recorded / name)
verify_recorded_hashes(recorded)
damaged = bytearray(self_file.read_bytes())
damaged[-1] ^= 1
(recorded / "eboot.bin").write_bytes(damaged)
try:
    verify_recorded_hashes(recorded)
except ValueError:
    pass
else:
    raise RuntimeError("Verifier accepted a SELF changed after container validation")
fixture_count = (29 + private_import_fixtures + identity_import_fixtures +
                 (1 if ps2_action >= 0 else 0) + (2 if direct_action >= 0 else 0) +
                 (3 if owned_marker >= 0 else 0))
print(f"PASS: PS5 verifier rejects {fixture_count} damaged/foreign fixtures and enforces the selected public/private import contract without modifying the original executable.")
