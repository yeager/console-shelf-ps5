// SPDX-License-Identifier: GPL-3.0-only
// Render the real host UI with the PS5 tile-write quarantine branch selected.
// This does not make a PS5 package or contact a console.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const region = require('./png-regions');

const root = path.resolve(__dirname, '..');
const build = spawnSync(process.execPath,
  [path.join(__dirname, 'build-native-host.js'), '--tile-write-quarantine'],
  { cwd: root, encoding: 'utf8', windowsHide: true, timeout: 120000 });
assert.equal(build.error, undefined, 'quarantined host build completes');
assert.equal(build.status, 0, `quarantined host build failed: ${build.stdout}\n${build.stderr}`);

const parent = path.join(root, '.cache', 'native-quarantine-render-tests');
fs.mkdirSync(parent, { recursive: true });
const directory = path.join(parent, `${Date.now()}-${process.pid}`);
const data = path.join(directory, 'data');
const captures = path.join(directory, 'captures');
fs.mkdirSync(data, { recursive: true });
fs.mkdirSync(captures);
const executable = path.join(root, '.cache', 'native-host',
  process.platform === 'win32' ? 'shelf-host-quarantine.exe' : 'shelf-host-quarantine');
const script = [
  'search', 'text:PlayStation 2', 'back', 'confirm',
  'down', 'down', 'down', 'confirm',
  'shot:ps2-card.png', 'report:ps2-card.json',
  'mock-tile-status-error', 'shot:ps2-error.png',
  'confirm', 'shot:ps2-explained.png', 'report:ps2-explained.json',
].join(',');
const run = spawnSync(executable,
  ['--asset-root', root, '--data-dir', data, '--capture-dir', captures,
    '--headless', '--script', script],
  { cwd: root, encoding: 'utf8', windowsHide: true, timeout: 60000 });
assert.equal(run.error, undefined, 'quarantined host run completes');
assert.equal(run.status, 0, `quarantined host run failed: ${run.stdout}\n${run.stderr}`);

const card = JSON.parse(fs.readFileSync(path.join(captures, 'ps2-card.json')));
const explained = JSON.parse(fs.readFileSync(path.join(captures, 'ps2-explained.json')));
for (const report of [card, explained]) {
  assert.equal(report.page, 5, 'PS2 shortcut card is rendered');
  assert.equal(report.selected, 'ps2', 'PS2 remains selected');
  assert.equal(report.tileRequested, false, 'the read-only card did not request a tile write');
}
const cardPng = path.join(captures, 'ps2-card.png');
const errorPng = path.join(captures, 'ps2-error.png');
const explainedPng = path.join(captures, 'ps2-explained.png');
for (const filename of [cardPng, errorPng, explainedPng]) {
  const png = fs.readFileSync(filename);
  assert.equal(png.subarray(0, 8).toString('hex'), '89504e470d0a1a0a');
  assert.equal(png.readUInt32BE(16), 1920);
  assert.equal(png.readUInt32BE(20), 1080);
}
// Sample the card's empty right edge. The status copy now legitimately spans
// x=500 at this height, so that pixel tests text antialiasing rather than the
// opaque panel behind it.
const cardPixel = region(cardPng, 1050, 900, 1, 1);
assert.ok(cardPixel[0] < 40 && cardPixel[1] < 40 && cardPixel[2] < 40,
  'the status line stays on an opaque dark card over the wooden floor');
assert.ok(!region(cardPng, 112, 858, 940, 24).equals(region(errorPng, 112, 858, 940, 24)),
  'the ShadowMount error replaces the read-only status inline');
assert.ok(!region(errorPng, 112, 887, 940, 28).equals(region(explainedPng, 112, 887, 940, 28)),
  'Cross reports the route-save attempt inside the card');
assert.ok(!region(errorPng, 500, 934, 800, 70).equals(region(explainedPng, 500, 934, 800, 70)),
  'Cross shows the route-save progress toast without attempting a tile write');

const images = [cardPng, errorPng, explainedPng].map(filename => ({
  path: filename,
  sha256: crypto.createHash('sha256').update(fs.readFileSync(filename)).digest('hex'),
}));
fs.writeFileSync(path.join(parent, 'latest.json'),
  JSON.stringify({ directory, images, consoleContact: false, tileWrite: false }, null, 2) + '\n');
console.log(`PASS: quarantined PS2 shortcut card, inline ShadowMount error, and route-save feedback in ${captures}`);
