// SPDX-License-Identifier: GPL-3.0-only
// Isolated host test for console tile ownership, collision and scan lifecycle.
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const build = path.join(root, '.cache', 'console-tile-manager-tests');
fs.mkdirSync(build, { recursive: true });
const includes = path.join(root, 'native/include');
const thirdParty = path.join(root, 'native/third_party');
const sources = [
  'native/src/console_tile_manager.cpp',
  'native/src/model.cpp',
  'native/src/metadata.cpp',
  'native/src/json.c',
  'native/tests/console_tile_manager_tests.cpp',
].map(file => path.join(root, file));
const output = path.join(build, process.platform === 'win32' ? 'console-tile-manager-tests.exe' : 'console-tile-manager-tests');
function run(command, args, options = {}) {
  const result = spawnSync(command, args, { cwd: build, stdio: 'inherit', windowsHide: true, ...options });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`Console tile manager test command failed: ${result.status}`);
}
if (process.platform === 'win32') {
  const vswhere = path.join(process.env['ProgramFiles(x86)'] || '', 'Microsoft Visual Studio/Installer/vswhere.exe');
  const result = spawnSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8', windowsHide: true });
  const installation = result.stdout?.trim();
  if (result.error || result.status !== 0 || !installation) throw new Error('Existing Visual Studio C++ toolset required.');
  const developerCommand = path.join(installation, 'Common7/Tools/VsDevCmd.bat');
  const quoted = value => {
    if (/["%\r\n]/.test(value)) throw new Error('Unsupported compiler input path.');
    return `"${value}"`;
  };
  const command = `call ${quoted(developerCommand)} -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /W4 /WX /I${quoted(includes)} /I${quoted(thirdParty)} ${sources.map(quoted).join(' ')} /Fe:${quoted(output)} Ws2_32.lib`;
  run(process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c', `"${command}"`], { windowsVerbatimArguments: true });
} else {
  run('c++', ['-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror', '-I', includes, '-I', thirdParty,
              ...sources, '-o', output]);
}
run(output, []);
