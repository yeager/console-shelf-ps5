// SPDX-License-Identifier: GPL-3.0-only
// Build locally; never connect to or deploy onto a console.
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const selected = process.argv[2] || 'build_ps5.py';
if (!['build_ps5.py', 'test_ps5_platform.py', 'verify_ps5_build.py', 'test_ps5_verifier.py', 'test_ps5_release_inputs.py', 'test_normalize_legacy_console_tiles_private.py', 'prepare_ps5_test.py', 'prepare_corresponding_source.py', 'relink_ps5.py', 'prepare_ps5_source_kit.py', 'prepare_ps5_release.py', 'build_launch_helper.py', 'console_tile_forwarder_v2/build.py', 'remote_close_retroarch_v1/build.py', 'remote_close_emulator_targets_v1/build.py', 'foreground_title_probe_v1/build.py'].includes(selected)) throw new Error('Unknown PS5 build task.');
const script = path.resolve(__dirname, selected);
let command = 'python3', args = [script, ...process.argv.slice(3)];
if (process.platform === 'win32') {
  // Test packaging is platform-neutral and renders reviewed console artwork.
  // Prefer the managed Windows Python when it has Pillow so Windows worktree
  // paths and the package-art executable stay native. Compiler tasks continue
  // to use the pinned Ubuntu toolchain through WSL.
  const windowsPillow = selected === 'prepare_ps5_test.py'
    ? spawnSync('py.exe', ['-3', '-c', 'import PIL'], { encoding: 'utf8', windowsHide: true })
    : null;
  if (windowsPillow && !windowsPillow.error && windowsPillow.status === 0) {
    command = 'py.exe';
    args = ['-3', script, ...process.argv.slice(3)];
  } else {
    const translate = spawnSync('wsl.exe', ['-d', 'Ubuntu', '--exec', 'wslpath', '-a', script], { encoding: 'utf8', windowsHide: true });
    if (translate.error || translate.status !== 0) throw new Error('An existing WSL Ubuntu instance is required; this script does not install WSL.');
    command = 'wsl.exe';
    args = ['-d', 'Ubuntu', '--exec', 'python3', translate.stdout.trim(), ...process.argv.slice(3)];
  }
}
const result = spawnSync(command, args, { stdio: 'inherit', windowsHide: true });
if (result.error) throw result.error;
process.exitCode = result.status === 0 ? 0 : 1;
