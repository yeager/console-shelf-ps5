// SPDX-License-Identifier: GPL-3.0-only
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const quarantined = process.argv.length === 3 && process.argv[2] === '--tile-write-quarantine';
if (process.argv.length > (quarantined ? 3 : 2)) throw new Error('Unknown native host build option.');
const root = path.resolve(__dirname, '..');
const build = path.join(root, '.cache/native-host');
fs.mkdirSync(build, { recursive: true });
function run(command,args,options={}) { const result=spawnSync(command,args,{cwd:build,stdio:'inherit',windowsHide:true,...options});if(result.error)throw result.error;if(result.status!==0)throw new Error(`Build command failed (${result.status})`); }
const sources=['model.cpp','localization.cpp','settings.cpp','ui.cpp','ui_test_bridge.cpp','ui_test_video.cpp','graphics.cpp','codecs.cpp','movie.cpp','library.cpp','artwork_sources.cpp','artwork.cpp','filesystem.cpp','metadata.cpp','ftp.cpp','launch_client.cpp','update.cpp','update_client.cpp','json.c','host_main.cpp'].map(file=>path.join(root,'native/src',file));
const includes=[path.join(root,'native/include'),path.join(root,'native/third_party')];
if(process.platform==='win32'){
  run(process.execPath,[path.join(root,'tools/fetch-native-deps.js')]);
  const sdl=path.join(root,'.cache/native-deps/SDL2-2.32.10');
  includes.push(path.join(sdl,'include'));
  const vswhere=path.join(process.env['ProgramFiles(x86)']||'','Microsoft Visual Studio/Installer/vswhere.exe');
  const result=spawnSync(vswhere,['-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],{encoding:'utf8',windowsHide:true});
  if(result.error||result.status!==0||!result.stdout.trim())throw new Error('Existing Visual Studio C++ tools are required.');
  const quoted=value=>{if(/["%\r\n]/.test(value))throw new Error('Unsupported compiler path character.');return `"${value}"`;};
  const developer=path.join(result.stdout.trim(),'Common7/Tools/VsDevCmd.bat');
  const command=`call ${quoted(developer)} -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /O2 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /W4 /WX ${quarantined?'/DSHELF_TILE_WRITE_QUARANTINE_TEST ':''}${includes.map(value=>'/I'+quoted(value)).join(' ')} ${sources.map(quoted).join(' ')} /Fe:${quarantined?'shelf-host-quarantine.exe':'shelf-host.exe'} /link /LIBPATH:${quoted(path.join(sdl,'lib/x64'))} SDL2.lib ws2_32.lib`;
  run(process.env.ComSpec||'cmd.exe',['/d','/s','/c',`"${command}"`],{windowsVerbatimArguments:true});
  fs.copyFileSync(path.join(sdl,'lib/x64/SDL2.dll'),path.join(build,'SDL2.dll'));
}else{
  const flags=spawnSync('sdl2-config',['--cflags','--libs'],{encoding:'utf8'});if(flags.status!==0)throw new Error('An existing SDL2 development installation is required.');
  run('c++',['-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-error=deprecated','-Wno-error=missing-field-initializers','-Wno-error=unused-variable','-Wno-unused-function',...(quarantined?['-DSHELF_TILE_WRITE_QUARANTINE_TEST']:[]),...includes.flatMap(value=>['-I',value]),...sources,...flags.stdout.trim().split(/\s+/),'-o',quarantined?'shelf-host-quarantine':'shelf-host']);
}
console.log(`Built ${quarantined?'quarantined ':''}native host renderer in .cache/native-host.`);
