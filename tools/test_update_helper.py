# SPDX-License-Identifier: GPL-3.0-only
"""Exercise updater success, rollback and recovery without touching a console."""

import hashlib
import os
import shutil
import subprocess
from pathlib import Path

from bootstrap_ps5_toolchain import environment, host_options, prepare
from ps5_common import CACHE, ROOT, run


TAG = "v9.8.7"
PROGRAM_OLD = {
    "assets/app.dat": b"old packaged asset\n",
    "eboot.bin": b"old executable\n",
    "sce_sys/param.json": b'{"old":true}\n',
}
PROGRAM_NEW = {
    "assets/app.dat": b"new packaged asset\n",
    "eboot.bin": b"new executable\n",
    "sce_sys/param.json": b'{"new":true}\n',
}
PLACEMENT_ORDER = [
    *sorted(relative for relative in PROGRAM_NEW if relative != "eboot.bin"),
    "eboot.bin",
]
SENTINELS = {
    "data/console-shelf/settings.ini": b"theme=precious\n",
    "data/console-shelf/routes/selected.ini": b"route=precious\n",
    "data/console-shelf/artwork/custom.png": b"precious custom artwork\x00\xff",
    "data/homebrew/PPSA99541/settings.ini": b"adjacent app setting\n",
    "data/homebrew/PPSA99541/games/title.iso": b"precious adjacent game\x00",
    "data/homebrew/PPSA99541/assets/user-custom.png": b"precious app artwork\n",
    "data/homebrew/PPSA99541/sce_sys/user-note.txt": b"precious note\n",
    "emulators/PS5SX2/config.ini": b"renderer=precious\n",
    "games/library/game.iso": b"precious game bytes\x00\x01",
    "bios/console/firmware.bin": b"precious bios bytes\x02",
    "saves/profile/card.bin": b"precious save bytes\x03",
}


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def write(path, raw):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw)


def fixture(root):
    data = root / "data/console-shelf"
    app = root / "data/homebrew/PPSA99541"
    stage = data / "update/stage/PPSA99541"
    for relative, raw in PROGRAM_OLD.items():
        write(app / relative, raw)
    for relative, raw in PROGRAM_NEW.items():
        write(stage / relative, raw)
    for relative, raw in SENTINELS.items():
        write(root / relative, raw)
    records = [
        f"file={relative}\tbytes={len(raw)}\tsha256={digest(raw)}\n"
        for relative, raw in sorted(PROGRAM_NEW.items())
    ]
    manifest = ("isshelf-update 1\n" + f"tag={TAG}\ncount={len(records)}\n" + "".join(records)).encode()
    manifest_path = data / "update/ready.manifest"
    write(manifest_path, manifest)
    return data, app, digest(manifest)


def sentinel_snapshot(root):
    return {relative: digest((root / relative).read_bytes()) for relative in SENTINELS}


def assert_sentinels(root, expected):
    observed = sentinel_snapshot(root)
    if observed != expected:
        changed = sorted(set(observed) | set(expected))
        changed = [name for name in changed if observed.get(name) != expected.get(name)]
        raise AssertionError("Updater changed protected user data: " + ", ".join(changed))


def assert_program(app, expected):
    for relative, raw in expected.items():
        if (app / relative).read_bytes() != raw:
            raise AssertionError("Unexpected installed program byte: " + relative)


def execute(executable, manifest_sha, fail_before=None):
    env = os.environ.copy()
    if fail_before is None:
        env.pop("ISSHELF_TEST_FAIL_BEFORE_PLACEMENT", None)
    else:
        env["ISSHELF_TEST_FAIL_BEFORE_PLACEMENT"] = str(fail_before)
    completed = subprocess.run([executable, TAG, manifest_sha], check=True, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    try:
        return int(completed.stdout.strip())
    except ValueError as error:
        raise AssertionError("Updater harness returned an invalid result") from error


def reset(root):
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)


def move_replacement(data, app, relative, place=True):
    backup = data / "update/backup/PPSA99541" / relative
    backup.parent.mkdir(parents=True, exist_ok=True)
    (app / relative).replace(backup)
    if place:
        (data / "update/stage/PPSA99541" / relative).replace(app / relative)


def emulate_begin(data, app, manifest_sha, placed=1, backup_only=None):
    for relative in PLACEMENT_ORDER[:placed]:
        move_replacement(data, app, relative)
    if backup_only is not None:
        move_replacement(data, app, PLACEMENT_ORDER[backup_only], place=False)
    write(data / "update/journal.txt",
          f"isshelf-update-journal 1\nmanifest={manifest_sha}\nstate=BEGIN\n".encode())


def emulate_commit(data, app, manifest_sha):
    for relative in PLACEMENT_ORDER:
        move_replacement(data, app, relative)
    write(data / "update/journal.txt",
          f"isshelf-update-journal 1\nmanifest={manifest_sha}\nstate=COMMIT\n".encode())


def emulate_committed_without_journal(data, app, manifest_sha):
    emulate_commit(data, app, manifest_sha)
    shutil.copyfile(data / "update/ready.manifest", data / "update/installed.manifest")
    shutil.rmtree(data / "update/backup")
    shutil.rmtree(data / "update/stage")
    (data / "update/journal.txt").unlink()


def main():
    prefix = prepare()
    env = environment(prefix)
    flags, link = host_options(prefix)
    output = CACHE / "update-helper-tests"
    root = output / "root"
    output.mkdir(parents=True, exist_ok=True)
    relaunch_executable = output / "update-relaunch-tests"
    run([prefix / "usr/bin/clang++-18", *flags, *link, "-std=c++17", "-O1",
         "-Wall", "-Wextra", "-Werror", "-I", ROOT / "native/include",
         ROOT / "native/tests/update_relaunch_tests.cpp", "-o", relaunch_executable], env=env)
    run([relaunch_executable], env=env)
    executable = output / "update-helper-tests"
    data = root / "data/console-shelf"
    app = root / "data/homebrew/PPSA99541"
    run([prefix / "usr/bin/clang++-18", *flags, *link, "-std=c++17", "-O1",
         "-Wall", "-Wextra", "-Werror", "-DSHELF_UPDATE_HOST_TEST",
         f'-DSHELF_UPDATE_DATA_ROOT="{data.as_posix()}"',
         f'-DSHELF_UPDATE_APP_ROOT="{app.as_posix()}"',
         "-I", ROOT / "native/include", ROOT / "native/ps5/helper/shelf_update.cpp",
         ROOT / "native/tests/update_helper_transaction_tests.cpp", "-o", executable], env=env)

    reset(root)
    data, app, manifest_sha = fixture(root)
    protected = sentinel_snapshot(root)
    if execute(executable, manifest_sha) != 0:
        raise AssertionError("Successful update transaction failed")
    assert_program(app, PROGRAM_NEW)
    assert_sentinels(root, protected)

    reset(root)
    data, app, manifest_sha = fixture(root)
    protected = sentinel_snapshot(root)
    (data / "update/backup/PPSA99541/assets").mkdir(parents=True)
    if execute(executable, manifest_sha) != 0:
        raise AssertionError("Pre-BEGIN recovery did not clear empty updater backup folders")
    assert_program(app, PROGRAM_NEW)
    assert_sentinels(root, protected)

    for fail_before in range(len(PLACEMENT_ORDER)):
        reset(root)
        data, app, manifest_sha = fixture(root)
        protected = sentinel_snapshot(root)
        if execute(executable, manifest_sha, fail_before=fail_before) >= 0:
            raise AssertionError(f"Injected update failure {fail_before} did not fail")
        assert_program(app, PROGRAM_OLD)
        assert_sentinels(root, protected)

    # Exercise power loss with a durable BEGIN journal before the first file,
    # after every completed replacement, and between each backup/place pair.
    for placed in range(len(PLACEMENT_ORDER) + 1):
        reset(root)
        data, app, manifest_sha = fixture(root)
        protected = sentinel_snapshot(root)
        emulate_begin(data, app, manifest_sha, placed=placed)
        if execute(executable, manifest_sha) != 0:
            raise AssertionError(f"BEGIN recovery after {placed} placements did not finish")
        assert_program(app, PROGRAM_NEW)
        assert_sentinels(root, protected)
    for backup_only in range(len(PLACEMENT_ORDER)):
        reset(root)
        data, app, manifest_sha = fixture(root)
        protected = sentinel_snapshot(root)
        emulate_begin(data, app, manifest_sha, placed=backup_only,
                      backup_only=backup_only)
        if execute(executable, manifest_sha) != 0:
            raise AssertionError(f"BEGIN recovery at backup boundary {backup_only} did not finish")
        assert_program(app, PROGRAM_NEW)
        assert_sentinels(root, protected)

    reset(root)
    data, app, manifest_sha = fixture(root)
    protected = sentinel_snapshot(root)
    emulate_commit(data, app, manifest_sha)
    if execute(executable, manifest_sha) != 0:
        raise AssertionError("COMMIT recovery did not finish cleanup")
    assert_program(app, PROGRAM_NEW)
    assert_sentinels(root, protected)

    # COMMIT recovery is idempotent at every cleanup boundary: after the
    # installed manifest is copied, after rollback bytes are removed, and
    # after the empty stage is removed.
    for cleanup_steps in range(3):
        reset(root)
        data, app, manifest_sha = fixture(root)
        protected = sentinel_snapshot(root)
        emulate_commit(data, app, manifest_sha)
        shutil.copyfile(data / "update/ready.manifest",
                        data / "update/installed.manifest")
        if cleanup_steps >= 1:
            shutil.rmtree(data / "update/backup")
        if cleanup_steps >= 2:
            shutil.rmtree(data / "update/stage")
        if execute(executable, manifest_sha) != 0:
            raise AssertionError(f"COMMIT cleanup boundary {cleanup_steps} did not finish")
        assert_program(app, PROGRAM_NEW)
        assert_sentinels(root, protected)

    reset(root)
    data, app, manifest_sha = fixture(root)
    protected = sentinel_snapshot(root)
    emulate_committed_without_journal(data, app, manifest_sha)
    if execute(executable, manifest_sha) != 0:
        raise AssertionError("Post-COMMIT two-unlink recovery did not finish cleanup")
    assert_program(app, PROGRAM_NEW)
    assert_sentinels(root, protected)

    print("PASS: updater recovery and single-request exact-title relaunch preserve every tested safety boundary.")


if __name__ == "__main__":
    main()
