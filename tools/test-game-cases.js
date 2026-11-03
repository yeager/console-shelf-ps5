// SPDX-License-Identifier: GPL-3.0-only
// Synthetic covers and non-executable game headers; no personal media or console access.
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const region = require('./png-regions');
const root = path.resolve(__dirname, '..');
const folder = path.join(root, '.cache/game-case-tests', String(Date.now()));
const tree = path.join(folder, 'tree'), data = path.join(folder, 'data'), captures = path.join(folder, 'captures');
for (const name of [tree, data, captures]) fs.mkdirSync(name, { recursive: true });
function crc(bytes) {
  let value = 0xffffffff;
  for (const byte of bytes) { value ^= byte; for (let bit=0; bit<8; bit++) value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0); }
  return (value ^ 0xffffffff) >>> 0;
}
function png(w, h, rgb) {
  function chunk(type, bytes) {
    const tag = Buffer.from(type), size = Buffer.alloc(4), checksum = Buffer.alloc(4);
    size.writeUInt32BE(bytes.length); checksum.writeUInt32BE(crc(Buffer.concat([tag, bytes])));
    return Buffer.concat([size, tag, bytes, checksum]);
  }
  const header = Buffer.alloc(13); header.writeUInt32BE(w); header.writeUInt32BE(h,4); header[8]=8; header[9]=2;
  const pixels = Buffer.alloc(h*(w*3+1));
  for (let y=0; y<h; y++) for(let x=0; x<w; x++) for(let c=0;c<3;c++) pixels[y*(w*3+1)+1+x*3+c]=rgb[c];
  return Buffer.concat([Buffer.from('89504e470d0a1a0a','hex'),chunk('IHDR',header),chunk('IDAT',zlib.deflateSync(pixels)),chunk('IEND',Buffer.alloc(0))]);
}
function file(name, bytes) {
  const destination = path.join(tree, name);
  fs.mkdirSync(path.dirname(destination), { recursive: true }); fs.writeFileSync(destination, bytes);
}
const red = [246,38,61], green = [27,236,69];
const iso = Buffer.alloc(0x8008); iso.write('CD001',0x8001);
file('data/PCSX2/games/Case fixture.iso',iso);
file('data/PCSX2/chd-serials.txt',`/data/PCSX2/games/Case fixture.iso\t${iso.length}\t1\tSLES-12345\n`);
file('data/PCSX2/covers/SLES-12345.png',png(135,190,red));
file('data/PCSX2/covers/SLES-12345_spine.png',png(14,190,green));
file('user/app/PPSA99169/eboot.bin','NONEXECUTABLE EMULATOR FIXTURE');
file('user/app/PPSA99169/sce_sys/param.json',JSON.stringify({titleId:'PPSA99169',titleName:'RetroArch'}));
file('data/homebrew/PPSA99169/cores/mupen64plus_next_libretro.so','NONEXECUTABLE CORE FIXTURE');
file('data/homebrew/PPSA99169/content/n64/Case fixture.z64',Buffer.from('8037124000000000','hex'));
file('data/homebrew/PPSA99169/thumbnails/Nintendo - Nintendo 64/Named_Boxarts/Case fixture.png',png(195,137,red));
file('data/PSXS5/games/Case fixture.cue','FILE "Case fixture.bin" BINARY\n');
file('data/PSXS5/games/Case fixture.bin','NONEXECUTABLE TRACK FIXTURE');
file('data/PSXS5/library.txt','Case fixture\tSLES-54321\t1\t/data/PSXS5/games/Case fixture.cue\tCase fixture\n');
file('data/PSXS5/covers/3d/SLES-54321.png',png(226,230,red));
file('user/app/PPSA12345/eboot.bin','NONEXECUTABLE APP FIXTURE');
file('user/app/PPSA12345/sce_sys/param.json',JSON.stringify({titleId:'PPSA12345',titleName:'Icon fixture'}));
file('user/app/PPSA12345/sce_sys/icon0.png',png(128,128,red));
const executable = path.join(root, '.cache/native-host', process.platform==='win32'?'shelf-host.exe':'shelf-host');
function run(name, system) {
  const result = spawnSync(executable,['--asset-root',root,'--data-dir',data,'--capture-dir',captures,'--library-root',tree,'--headless',
    '--script',`search,text:${system},back,confirm,right,confirm,shot:${name}.png,report:${name}.json`],
    {cwd:root,encoding:'utf8',windowsHide:true,timeout:30000});
  assert.equal(result.error,undefined); assert.equal(result.status,0,result.stdout+result.stderr);
  return JSON.parse(fs.readFileSync(path.join(captures,name+'.json')));
}
function bounds(name, predicate) {
  const pixels = region(path.join(captures,name+'.png'),1190,327,610,550);
  let minx=610,maxx=-1,miny=550,maxy=-1,count=0;
  for(let y=0;y<550;y++)for(let x=0;x<610;x++){
    const at=(y*610+x)*4;
    if(predicate(pixels[at],pixels[at+1],pixels[at+2])){minx=Math.min(minx,x);maxx=Math.max(maxx,x);miny=Math.min(miny,y);maxy=Math.max(maxy,y);count++;}
  }
  assert.ok(count>100,'the synthetic art must appear on the rendered case');
  return {minx,maxx,miny,maxy,width:maxx-minx+1,height:maxy-miny+1,count};
}
const isRed=(r,g,b)=>r>200&&g<90&&b<130;
const isGreen=(r,g,b)=>g>150&&r<80&&b<110;
run('ps2','PlayStation 2');
const dvd=bounds('ps2',isRed),spine=bounds('ps2',isGreen);
assert.ok(dvd.height>dvd.width*1.3,'PS2 uses a tall DVD case');
assert.ok(spine.height>spine.width*5,'the existing spine is a distinct narrow side face');
const ps2Pixels=region(path.join(captures,'ps2.png'),1190,327,610,550);
const middle=Math.floor((dvd.minx+dvd.maxx)/2);
let paperTop=0;
while(paperTop<550&&!isRed(...ps2Pixels.subarray((paperTop*610+middle)*4,(paperTop*610+middle)*4+3)))paperTop++;
assert.ok(paperTop>3&&paperTop<550,'the PS2 front paper is visible below its plastic edge');
const plastic=ps2Pixels.subarray(((paperTop-2)*610+middle)*4,((paperTop-2)*610+middle)*4+3);
assert.ok([...plastic].every(channel=>channel<20),'rendered PS2 plastic stays deep black instead of a gray decorative frame');
run('n64','Nintendo 64');
const n64=bounds('n64',isRed);
assert.ok(n64.width>n64.height*1.2,'N64 remains a wide cardboard box');
run('ps1','PlayStation 1');
const jewel=bounds('ps1',isRed);
assert.ok(Math.abs(jewel.width/jewel.height-226/230)<.02,'pre-rendered PS1 case artwork keeps its original silhouette and proportions');
run('icon','PlayStation 5');
const icon=bounds('icon',isRed);
assert.ok(Math.abs(icon.width/icon.height-1)<.02,'square app icons are never stretched into retail cover art');
const index=JSON.parse(fs.readFileSync(path.join(data,'covers/index.json')));
assert.equal(index.games.find(g=>g.system==='ps1').kind,1);
assert.equal(index.games.find(g=>g.system==='ps2').spineReady,true);
assert.equal(index.games.find(g=>g.system==='ps5').kind,2);
fs.writeFileSync(path.join(root,'.cache/game-case-tests/latest.json'),JSON.stringify({folder,captures,processClosed:true,checks:{dvd,spine,n64,jewel,icon}},null,2)+'\n');
console.log('PASS: PS1 case preservation, separate PS2 spine texture, N64 box proportions, icon preservation and repeatable cache reuse.');
