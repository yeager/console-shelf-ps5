# SPDX-License-Identifier: GPL-3.0-only
"""Host fixture for the isolated v4 scratch transaction and refusal cases."""

import errno
import json
from pathlib import Path
import stat
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from bootstrap_ps5_toolchain import environment, host_options, prepare  # noqa: E402
from ps5_common import ROOT  # noqa: E402


def fields(path: Path) -> dict[str, str]:
    lines = path.read_text().splitlines()
    assert lines[-1] == "record_complete=1", lines[-1]
    parsed = dict(line.split("=", 1) for line in lines)
    assert len(parsed) == len(lines), "duplicate receipt field"
    return parsed


def main() -> None:
    prefix = prepare()
    env = environment(prefix)
    compile_options, link_options = host_options(prefix)
    source = ROOT / "tools/console_badges/console_tile_write_probe_v4.cpp"
    with tempfile.TemporaryDirectory(prefix="isshelf-write-probe-") as folder_name:
        folder = Path(folder_name)
        output = folder / "console-shelf"
        homebrew = folder / "homebrew"
        output.mkdir()
        homebrew.mkdir()
        owned = homebrew / "PPSA99541"
        owned.mkdir()
        (owned / "sentinel").write_bytes(b"existing title bytes\n")
        request = output / "console-tile-write-request-v4.txt"
        executable = folder / "probe"
        subprocess.run([prefix / "usr/bin/clang++-18", "-std=c++17", "-O2", "-Wall", "-Wextra",
                        "-Werror", *compile_options, "-DISSHELF_WRITE_PROBE_HOST_TEST",
                        "-DISSHELF_WRITE_PROBE_TEST_RECEIPT_ROOT=" + json.dumps(str(output)),
                        "-DISSHELF_WRITE_PROBE_TEST_HOMEBREW_ROOT=" + json.dumps(str(homebrew)),
                        source, "-o", executable, *link_options], check=True, env=env)

        def set_request(nonce: str) -> None:
            request.write_text("format=isshelf-console-tile-write-probe/4\nnonce=" + nonce + "\n")
            request.chmod(0o644)

        first_nonce = "0123456789abcdef0123456789abcdef"
        set_request(first_nonce)
        run = subprocess.run([executable], check=False, env=env)
        assert run.returncode == 0, run.returncode
        first_receipt = output / f"console-tile-write-v4-{first_nonce}.txt"
        first = fields(first_receipt)
        assert first["format"] == "isshelf-console-tile-write-probe/4"
        assert first["nonce"] == first_nonce
        assert first["runtime"] == "standard-sdk-v0.42"
        assert first["scratch_remaining"] == "absent"
        assert first["transaction_complete"] == "1"
        assert first["valid_title_id_write_attempted"] == "0"
        assert first["homebrew_scratch_write_attempted"] == "1"
        assert first["homebrew_scratch_created"] == "1"
        assert first["first_failure"] == "none"
        for field in ("scratch_absence", "scratch_mkdirat", "scratch_openat",
                      "scratch_identity", "file_openat", "file_write", "file_fsync",
                      "file_seek", "file_readback", "renameat", "directory_fsync",
                      "final_openat", "final_readback", "file_unlinkat", "scratch_unlinkat"):
            assert first[f"stage_{field}_errno"] == "0", field
        assert stat.S_IMODE(first_receipt.stat().st_mode) == 0o644
        assert (owned / "sentinel").read_bytes() == b"existing title bytes\n"
        assert not list(homebrew.glob(".isshelf-write-probe-v4-*"))

        injected = folder / "probe-injected-failure"
        subprocess.run([prefix / "usr/bin/clang++-18", "-std=c++17", "-O2", "-Wall", "-Wextra",
                        "-Werror", *compile_options, "-DISSHELF_WRITE_PROBE_HOST_TEST",
                        "-DISSHELF_WRITE_PROBE_TEST_FAIL_AFTER_TEMP",
                        "-DISSHELF_WRITE_PROBE_TEST_RECEIPT_ROOT=" + json.dumps(str(output)),
                        "-DISSHELF_WRITE_PROBE_TEST_HOMEBREW_ROOT=" + json.dumps(str(homebrew)),
                        source, "-o", injected, *link_options], check=True, env=env)
        failed_nonce = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        set_request(failed_nonce)
        assert subprocess.run([injected], check=False, env=env).returncode == 54
        failed = fields(output / f"console-tile-write-v4-{failed_nonce}.txt")
        assert failed["stage_file_write_errno"] == str(errno.EIO)
        assert failed["stage_file_unlinkat_errno"] == "0"
        assert failed["stage_scratch_unlinkat_errno"] == "0"
        assert failed["scratch_remaining"] == "absent"
        assert failed["transaction_complete"] == "0"
        assert failed["homebrew_scratch_write_attempted"] == "1"
        assert failed["homebrew_scratch_created"] == "1"
        assert not list(homebrew.glob(".isshelf-write-probe-v4-*"))

        set_request(first_nonce)

        before = first_receipt.read_bytes()
        assert subprocess.run([executable], check=False, env=env).returncode == 42
        assert first_receipt.read_bytes() == before

        second_nonce = "fedcba9876543210fedcba9876543210"
        set_request(second_nonce)
        scratch = homebrew / f".isshelf-write-probe-v4-{second_nonce}"
        scratch.mkdir()
        (scratch / "sentinel").write_bytes(b"preexisting scratch\n")
        assert subprocess.run([executable], check=False, env=env).returncode == 48
        collision = fields(output / f"console-tile-write-v4-{second_nonce}.txt")
        assert collision["stage_scratch_absence_errno"] == str(errno.EEXIST)
        assert collision["scratch_remaining"] == "present"
        assert collision["transaction_complete"] == "0"
        assert collision["homebrew_scratch_write_attempted"] == "0"
        assert collision["homebrew_scratch_created"] == "0"
        assert (scratch / "sentinel").read_bytes() == b"preexisting scratch\n"

        third_nonce = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        set_request(third_nonce)
        symlink = homebrew / f".isshelf-write-probe-v4-{third_nonce}"
        symlink.symlink_to(owned, target_is_directory=True)
        assert subprocess.run([executable], check=False, env=env).returncode == 48
        blocked = fields(output / f"console-tile-write-v4-{third_nonce}.txt")
        assert blocked["stage_scratch_absence_errno"] == str(errno.EEXIST)
        assert symlink.is_symlink()
        assert (owned / "sentinel").read_bytes() == b"existing title bytes\n"

        request.write_text("format=isshelf-console-tile-write-probe/4\nnonce=BAD\n")
        assert subprocess.run([executable], check=False, env=env).returncode == 41
        set_request("cccccccccccccccccccccccccccccccc")
        request.chmod(0o600)
        assert subprocess.run([executable], check=False, env=env).returncode == 41
        assert len(list(output.glob("console-tile-write-v4-*.txt"))) == 4
    print("PASS: v4 transaction, success/failure cleanup, create-only receipt, collisions and symlink")


if __name__ == "__main__":
    main()
