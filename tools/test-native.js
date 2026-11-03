// SPDX-License-Identifier: GPL-3.0-only
// Compile the native model on the host. No PS5 build, deployment or global settings.
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const build = path.join(root, '.cache/native-tests');
fs.mkdirSync(build, { recursive: true });
const source = path.join(root, 'native/src/model.cpp');
const tests = path.join(root, 'native/tests/model_tests.cpp');
const settings = path.join(root, 'native/src/settings.cpp');
const settingsTests = path.join(root, 'native/tests/settings_tests.cpp');
const ui = path.join(root, 'native/src/ui.cpp');
const tileManager = path.join(root, 'native/src/console_tile_manager.cpp');
const uiTests = path.join(root, 'native/tests/ui_tests.cpp');
const pairingEntryTests = path.join(root, 'native/tests/pairing_code_entry_tests.cpp');
const quarantineTests = path.join(root, 'native/tests/console_tile_quarantine_ps5_tests.cpp');
const uiBridge = path.join(root, 'native/src/ui_test_bridge.cpp');
const uiBridgeTests = path.join(root, 'native/tests/ui_test_bridge_tests.cpp');
const uiTestPreviewTests = path.join(root, 'native/tests/ui_test_preview_tests.cpp');
const navigationTests = path.join(root, 'native/tests/navigation_tests.cpp');
const launchTests = path.join(root, 'native/tests/launch_tests.cpp');
const consoleTileTests = path.join(root, 'native/tests/console_tile_intent_tests.cpp');
const routePreferenceTests = path.join(root, 'native/tests/console_tile_route_preference_tests.cpp');
const settingsRouteCodecTests = path.join(root, 'native/tests/console_tile_settings_route_codec_tests.cpp');
const routeStorePosixTests = path.join(root, 'native/tests/console_tile_route_store_posix_tests.cpp');
const consoleTileJobTests = path.join(root, 'native/tests/console_tile_job_tests.cpp');
const consoleTileJobDispatchTests = path.join(root, 'native/tests/console_tile_job_dispatch_tests.cpp');
const consoleTileJobBridgeTests = path.join(root, 'native/tests/console_tile_job_bridge_tests.cpp');
const consoleTileJobDirectoryTests = path.join(root, 'native/tests/console_tile_job_directory_tests.cpp');
const genericTileCreateStateTests = path.join(root, 'native/tests/generic_tile_create_state_tests.cpp');
const consoleTileStartupTests = path.join(root, 'native/tests/console_tile_startup_tests.cpp');
const consoleTileRelayTests = path.join(root, 'native/tests/console_tile_relay_tests.cpp');
const consoleTileForwarderConfigTests = path.join(root, 'native/tests/console_tile_forwarder_config_tests.cpp');
const includes = path.join(root, 'native/include');
const librarySources = ['library.cpp','artwork_sources.cpp','filesystem.cpp','metadata.cpp','json.c'].map(file=>path.join(root,'native/src',file));
const libraryTests = path.join(root, 'native/tests/library_tests.cpp');
const output = path.join(build, process.platform === 'win32' ? 'model-tests.exe' : 'model-tests');
const consoleTileOutput = path.join(build, process.platform === 'win32' ? 'console-tile-tests.exe' : 'console-tile-tests');
const routePreferenceOutput = path.join(build, process.platform === 'win32' ? 'console-tile-route-preference-tests.exe' : 'console-tile-route-preference-tests');
const settingsRouteCodecOutput = path.join(build, process.platform === 'win32' ? 'console-tile-settings-route-codec-tests.exe' : 'console-tile-settings-route-codec-tests');
const routeStorePosixOutput = path.join(build, 'console-tile-route-store-posix-tests');
const consoleTileJobOutput = path.join(build, process.platform === 'win32' ? 'console-tile-job-tests.exe' : 'console-tile-job-tests');
const consoleTileJobDispatchOutput = path.join(build, process.platform === 'win32' ? 'console-tile-job-dispatch-tests.exe' : 'console-tile-job-dispatch-tests');
const consoleTileJobBridgeOutput = path.join(build, process.platform === 'win32' ? 'console-tile-job-bridge-tests.exe' : 'console-tile-job-bridge-tests');
const consoleTileJobDirectoryOutput = path.join(build, process.platform === 'win32' ? 'console-tile-job-directory-tests.exe' : 'console-tile-job-directory-tests');
const genericTileCreateStateOutput = path.join(build, process.platform === 'win32' ? 'generic-tile-create-state-tests.exe' : 'generic-tile-create-state-tests');
const quarantineOutput = path.join(build, process.platform === 'win32' ? 'ps5-shortcut-quarantine-tests.exe' : 'ps5-shortcut-quarantine-tests');
function run(command, args, options = {}) {
  const result = spawnSync(command, args, { cwd: build, stdio: 'inherit', windowsHide: true, ...options });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`Native check command failed with exit code ${result.status}`);
}
if (process.platform === 'win32') {
  const vswhere = path.join(process.env['ProgramFiles(x86)'] || '', 'Microsoft Visual Studio/Installer/vswhere.exe');
  const result = spawnSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8', windowsHide: true });
  const installation = result.stdout?.trim();
  if (result.error || result.status !== 0 || !installation) throw new Error('An existing Visual Studio C++ toolset is required for the Windows host check.');
  const developerCommand = path.join(installation, 'Common7/Tools/VsDevCmd.bat');
  const quoted = value => {
    if (/["%\r\n]/.test(value)) throw new Error('Unsupported character in compiler input path.');
    return `"${value}"`;
  };
  const command = `call ${quoted(developerCommand)} -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /W4 /WX /I${quoted(includes)} /I${quoted(path.join(root,'native/third_party'))} ${[source,settings,ui,uiBridge,tests,settingsTests,uiTests,pairingEntryTests,uiBridgeTests,uiTestPreviewTests,consoleTileStartupTests,consoleTileRelayTests,consoleTileForwarderConfigTests,navigationTests,launchTests,...librarySources,libraryTests].map(quoted).join(' ')} /Fe:model-tests.exe`;
  run(process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c', `"${command}"`], { windowsVerbatimArguments: true });
  const consoleTileCommand = `call ${quoted(developerCommand)} -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /W4 /WX /I${quoted(includes)} ${quoted(consoleTileTests)} /Fe:console-tile-tests.exe`;
  run(process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c', `"${consoleTileCommand}"`], { windowsVerbatimArguments: true });
  const routePreferenceCommand = `call ${quoted(developerCommand)} -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /W4 /WX /I${quoted(includes)} ${quoted(routePreferenceTests)} /Fe:console-tile-route-preference-tests.exe`;
  run(process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c', `"${routePreferenceCommand}"`], { windowsVerbatimArguments: true });
  const settingsRouteCodecCommand = `call ${quoted(developerCommand)} -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /W4 /WX /I${quoted(includes)} ${quoted(settingsRouteCodecTests)} /Fe:console-tile-settings-route-codec-tests.exe`;
  run(process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c', `"${settingsRouteCodecCommand}"`], { windowsVerbatimArguments: true });
  const quarantineCommand = `call ${quoted(developerCommand)} -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /DSHELF_TILE_WRITE_QUARANTINE_TEST /W4 /WX /I${quoted(includes)} /I${quoted(path.join(root,'native/third_party'))} ${[source,settings,ui,tileManager,...librarySources,quarantineTests].map(quoted).join(' ')} /Fe:ps5-shortcut-quarantine-tests.exe Ws2_32.lib`;
  run(process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c', `"${quarantineCommand}"`], { windowsVerbatimArguments: true });
  const dispatchCommand = `call ${quoted(developerCommand)} -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /W4 /WX /I${quoted(includes)} ${quoted(consoleTileJobDispatchTests)} /Fe:console-tile-job-dispatch-tests.exe`;
  run(process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c', `"${dispatchCommand}"`], { windowsVerbatimArguments: true });
  const bridgeCommand = `call ${quoted(developerCommand)} -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /W4 /WX /I${quoted(includes)} ${quoted(consoleTileJobBridgeTests)} /Fe:console-tile-job-bridge-tests.exe`;
  run(process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c', `"${bridgeCommand}"`], { windowsVerbatimArguments: true });
  const jobCommand = `call ${quoted(developerCommand)} -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /W4 /WX /I${quoted(includes)} ${quoted(consoleTileJobTests)} /Fe:console-tile-job-tests.exe`;
  run(process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c', `"${jobCommand}"`], { windowsVerbatimArguments: true });
  const directoryCommand = `call ${quoted(developerCommand)} -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /W4 /WX /I${quoted(includes)} ${quoted(consoleTileJobDirectoryTests)} /Fe:console-tile-job-directory-tests.exe`;
  run(process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c', `"${directoryCommand}"`], { windowsVerbatimArguments: true });
  const createStateCommand = `call ${quoted(developerCommand)} -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /W4 /WX /I${quoted(includes)} ${quoted(genericTileCreateStateTests)} /Fe:generic-tile-create-state-tests.exe`;
  run(process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c', `"${createStateCommand}"`], { windowsVerbatimArguments: true });
} else {
  run('c++', ['-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror', '-I', includes, '-I',path.join(root,'native/third_party'),source, settings, ui, uiBridge, tests, settingsTests, uiTests,pairingEntryTests,uiBridgeTests,uiTestPreviewTests,consoleTileStartupTests,consoleTileRelayTests,consoleTileForwarderConfigTests,navigationTests,launchTests,...librarySources,libraryTests,'-o', output]);
  run('c++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', includes, consoleTileTests, '-o', consoleTileOutput]);
  run('c++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', includes,
              routePreferenceTests, '-o', routePreferenceOutput]);
  run('c++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', includes,
              settingsRouteCodecTests, '-o', settingsRouteCodecOutput]);
  run('c++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-DISSHELF_PS1_WRITER_HOST_TEST', '-I', includes,
              routeStorePosixTests, '-o', routeStorePosixOutput]);
  run('c++', ['-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror', '-DSHELF_TILE_WRITE_QUARANTINE_TEST', '-I', includes,
              '-I', path.join(root,'native/third_party'), source, settings, ui, tileManager, ...librarySources,
              quarantineTests, '-o', quarantineOutput]);
  run('c++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', includes,
              consoleTileJobDispatchTests, '-o', consoleTileJobDispatchOutput]);
  run('c++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', includes,
              consoleTileJobBridgeTests, '-o', consoleTileJobBridgeOutput]);
  run('c++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', includes,
              consoleTileJobTests, '-o', consoleTileJobOutput]);
  run('c++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', includes,
              consoleTileJobDirectoryTests, '-o', consoleTileJobDirectoryOutput]);
  run('c++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', includes,
              genericTileCreateStateTests, '-o', genericTileCreateStateOutput]);
}
run(output, []);
run(consoleTileOutput, []);
run(routePreferenceOutput, []);
run(settingsRouteCodecOutput, []);
if (process.platform !== 'win32') run(routeStorePosixOutput, []);
run(quarantineOutput, []);
run(consoleTileJobDispatchOutput, []);
run(consoleTileJobBridgeOutput, []);
run(consoleTileJobOutput, []);
run(consoleTileJobDirectoryOutput, []);
run(genericTileCreateStateOutput, []);
