# SPDX-License-Identifier: GPL-3.0-only
"""Host fixture for separate, web-runtime-only v2 path diagnostic."""

import errno
import json
import stat
from pathlib import Path
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from bootstrap_ps5_toolchain import environment, host_options, prepare  # noqa: E402
from ps5_common import ROOT  # noqa: E402


def parse_receipt(path: Path) -> dict[str, str]:
    lines = path.read_text().splitlines()
    assert lines[-1] == "record_complete=1", lines[-1]
    fields = dict(line.split("=", 1) for line in lines)
    assert len(fields) == len(lines), "duplicate field"
    return fields


def main() -> None:
    prefix = prepare()
    env = environment(prefix)
    compile_options, link_options = host_options(prefix)
    source = ROOT / "tools/console_badges/console_tile_path_probe_v2.cpp"
    with tempfile.TemporaryDirectory(prefix="isshelf-path-probe-") as folder_name:
        folder = Path(folder_name)
        output = folder / "console-shelf"
        homebrew = folder / "homebrew"
        app = folder / "app"
        appmeta = folder / "appmeta"
        for root in (output, homebrew, app, appmeta):
            root.mkdir()
        original = b"owned fixture only\n"
        owned = homebrew / "PPSA99541"
        owned.mkdir()
        (owned / "sentinel").write_bytes(original)
        tile = homebrew / "PPSA99704"
        tile.mkdir()
        (tile / "sentinel").write_bytes(original)
        (app / "PPSA99704").symlink_to(tile, target_is_directory=True)
        (appmeta / "PPSA99704").mkdir()
        request = output / "console-tile-path-request-v2.txt"
        nonce = "0123456789abcdef0123456789abcdef"
        request_bytes = ("format=isshelf-console-tile-path-probe/2\nnonce=" + nonce + "\n").encode()
        request.write_bytes(request_bytes)
        request.chmod(0o644)
        executable = folder / "probe"
        subprocess.run([prefix / "usr/bin/clang++-18", "-std=c++17", "-O2", "-Wall", "-Wextra",
                        "-Werror", *compile_options, "-DISSHELF_PATH_PROBE_HOST_TEST",
                        "-DISSHELF_PATH_PROBE_TEST_RECEIPT_ROOT=" + json.dumps(str(output)),
                        "-DISSHELF_PATH_PROBE_TEST_HOMEBREW_ROOT=" + json.dumps(str(homebrew)),
                        "-DISSHELF_PATH_PROBE_TEST_APP_ROOT=" + json.dumps(str(app)),
                        "-DISSHELF_PATH_PROBE_TEST_APPMETA_ROOT=" + json.dumps(str(appmeta)),
                        source, "-o", executable, *link_options], check=True, env=env)
        subprocess.run([executable], check=True, env=env)
        receipt = output / ("console-tile-path-v2-" + nonce + ".txt")
        first = receipt.read_bytes()
        fields = parse_receipt(receipt)
        assert fields["format"] == "isshelf-console-tile-path-probe/2"
        assert stat.S_IMODE(receipt.stat().st_mode) == 0o644
        assert fields["runtime"] == "standard-sdk-v0.42"
        assert fields["record_complete"] == "1"
        assert fields["request_unlinked"] == "0"
        assert fields["title_root_write_attempted"] == "0"
        assert fields["database_open_attempted"] == "0"
        assert fields["network_attempted"] == "0"
        assert fields["root_0_complete"] == "1"
        assert fields["root_0_seen_PPSA99541"] == "1"
        assert fields["root_0_seen_PPSA99704"] == "1"
        assert fields["root_1_seen_PPSA99704"] == "1"
        assert fields["root_2_seen_PPSA99704"] == "1"
        assert fields["path_0_lstat_errno"] == fields["path_0_stat_errno"] == "0"
        assert fields["path_0_open_errno"] == fields["path_0_fstat_errno"] == "0"
        assert fields["path_0_fstat_same_lstat"] == "1"
        assert int(fields["path_2_lstat_type"]) != int(fields["path_2_stat_type"])
        assert int(fields["path_2_open_errno"]) == errno.ELOOP
        for index in (4, 5, 6):
            assert int(fields[f"path_{index}_lstat_errno"]) == errno.ENOENT
            assert int(fields[f"path_{index}_stat_errno"]) == errno.ENOENT
            assert int(fields[f"path_{index}_open_errno"]) == errno.ENOENT
        assert (owned / "sentinel").read_bytes() == original
        assert (tile / "sentinel").read_bytes() == original
        assert request.read_bytes() == request_bytes
        duplicate = subprocess.run([executable], check=False, env=env)
        assert duplicate.returncode == 29 and receipt.read_bytes() == first
        second_nonce = "fedcba9876543210fedcba9876543210"
        request.write_bytes(("format=isshelf-console-tile-path-probe/2\nnonce=" +
                             second_nonce + "\n").encode())
        appmeta.rename(folder / "appmeta-hidden")
        subprocess.run([executable], check=True, env=env)
        second_receipt = output / ("console-tile-path-v2-" + second_nonce + ".txt")
        failed_listing = parse_receipt(second_receipt)
        assert failed_listing["record_complete"] == "1"
        assert failed_listing["root_2_complete"] == "0"
        assert int(failed_listing["root_2_dir_open_errno"]) == errno.ENOENT
        (folder / "appmeta-hidden").rename(appmeta)
        request.write_bytes(b"format=isshelf-console-tile-path-probe/2\nnonce=BAD\n")
        assert subprocess.run([executable], check=False, env=env).returncode == 23
        assert set(output.glob("console-tile-path-v2-*.txt")) == {receipt, second_receipt}
        request.write_bytes(request_bytes)
        request.chmod(0o600)
        assert subprocess.run([executable], check=False, env=env).returncode == 23
        request.chmod(0o644)
        request.write_bytes(request_bytes)
        output.rename(folder / "output-real")
        output.symlink_to(folder / "output-real", target_is_directory=True)
        assert subprocess.run([executable], check=False, env=env).returncode == 21
        assert receipt.read_bytes() == first
    print("PASS: exact path observations, symlink and missing-title behavior, create-only receipt")


if __name__ == "__main__":
    main()
