const { chromium } = require('playwright');
const { pathToFileURL } = require('node:url');
const path = require('node:path');
const fs = require('node:fs');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const systems = require('../systems-data.js');
const manifest = require('../assets/3d-icons/manifest.json');

(async () => {
  const root = path.resolve(__dirname, '..');
  const capturePreviews = process.env.SHELF_CAPTURE_PREVIEWS === '1';
  assert.equal(manifest.length, 52);
  for (const entry of manifest) {
    const data = fs.readFileSync(path.join(root, entry.local));
    assert.equal(data.length, entry.bytes, `${entry.id} byte count`);
    assert.equal(crypto.createHash('sha256').update(data).digest('hex').toUpperCase(), entry.sha256, `${entry.id} SHA-256`);
  }
  const edge = process.platform === 'win32' && ['C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', 'C:/Program Files/Microsoft/Edge/Application/msedge.exe'].find(candidate => fs.existsSync(candidate));
  const browser = await chromium.launch(edge ? { headless: true, executablePath: edge } : { headless: true });
  try {
    const page = await browser.newPage({ viewport: { width: 1440, height: 1000 }, deviceScaleFactor: 1 });
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    const capture = async (target, name) => {
      if (!capturePreviews) return;
      await target.locator('img').evaluateAll(images => Promise.all(images.map(image => {
        image.loading = 'eager';
        return image.decode().catch(() => {});
      })));
      await target.screenshot({ path: path.join(root, 'previews', name), animations: 'disabled' });
    };
    const openPanel = async name => {
      if (!await page.locator('#settingsDialog').isVisible()) await page.click('#openAppearance');
      await page.click('#tab' + name);
    };
    const closePanel = () => page.click('#doneSettings');
    await page.goto(pathToFileURL(path.join(root, 'index.html')).href);
    await page.locator('.card').first().waitFor();
    assert.equal(await page.locator('#brandIcon').getAttribute('src'), 'assets/branding/shelf-gaming-mark.png', 'new profiles use the vector logo');
    await openPanel('Appearance');
    assert.deepEqual(await page.locator('#iconStyle option').evaluateAll(options => options.map(option => option.value)), ['vector', 'gaming', 'classic', 'jade']);
    for (const [choice, image] of Object.entries({vector: 'assets/branding/shelf-gaming-mark.png', gaming: 'assets/shelf-icon.png', classic: 'assets/shelf-icon-v1.png', jade: 'assets/shelf-icon-premium-v2.png'})) {
      await page.selectOption('#iconStyle', choice);
      await page.reload();
      await page.locator('.card').first().waitFor();
      assert.equal(await page.locator('#brandIcon').getAttribute('src'), image, choice + ' logo persists');
      assert.equal(await page.locator('#brandIcon').evaluate(image => image.complete && image.naturalWidth > 0), true);
      await openPanel('Appearance');
    }
    await page.selectOption('#iconStyle', 'vector');
    await closePanel();
    assert.equal(systems.length, 52);
    assert.equal(systems.filter(system => system.core).length, 46);
    assert.equal(await page.locator('.card').count(), 52);
    assert.equal(await page.locator('#settingsDialog').isVisible(), false);
    assert.equal(await page.locator('#sort').isVisible(), false, 'settings stay out of the shelf');
    assert.equal(await page.locator('#home [aria-haspopup="dialog"]').count(), 0, 'one header settings entry, no duplicate home settings buttons');
    const missingImage = page.locator('.card[data-system="switch2"] img');
    assert.match(await missingImage.evaluate(image => getComputedStyle(image).filter), /grayscale\(1\)/);
    assert.match(await page.locator('.card[data-system="ps5"] img').evaluate(image => getComputedStyle(image).filter), /grayscale\(0\)/);
    await openPanel('Appearance');
    await page.click('#missingColour');
    await closePanel();
    assert.match(await missingImage.evaluate(image => getComputedStyle(image).filter), /grayscale\(0\)/);
    await page.reload();
    assert.match(await missingImage.evaluate(image => getComputedStyle(image).filter), /grayscale\(0\)/, 'full colour choice persists');
    await openPanel('Appearance');
    assert.equal(await page.locator('#missingColour').getAttribute('aria-pressed'), 'true');
    await page.click('#missingMono');
    await closePanel();
    assert.equal(await page.locator('.card').count(), 52, 'colour preference never hides a console');
    await page.click('#toggleLayout');
    assert.equal(await page.locator('#systems').getAttribute('class'), 'systems-carousel');
    assert.equal(await page.locator('#settingsDialog').isVisible(), false, 'layout switches directly');
    await page.click('#toggleLayout');
    assert.equal(await page.locator('#systems').getAttribute('class'), 'systems-shelf');
    assert.match(await missingImage.evaluate(image => getComputedStyle(image).filter), /grayscale\(1\)/, 'Shelf preserves the missing-emulator cue');
    assert.equal(await page.locator('.display-tier').count(), 13);
    assert.equal(await page.locator('.display-tier .card').count(), 52);
    assert.equal(await page.locator('.display-tier:not([hidden])').count(), 2, 'one coherent room holds two visible tiers');
    assert.equal(await page.locator('.display-tier').first().evaluate(tier => getComputedStyle(tier).backgroundImage), 'none', 'there is no pasted shelf-panel photograph');
    const shelfPlacement = await page.locator('.display-tier:not([hidden]) .card').evaluateAll(cards => cards.map(card => {
      const art = shelfArtLayout[card.dataset.system], image = card.querySelector('img').getBoundingClientRect();
      const slot = card.getBoundingClientRect();
      const scene=window.shelfRoomScene, scale=Math.max(innerWidth/scene.width,innerHeight/scene.height);
      const row=scene.rows[Number(card.parentElement.dataset.roomTier)];
      const [x, y, w, h] = art.bounds;
      return { base: image.top + (art.supportY ?? y + h) * image.height / art.canvas[1], contact: (innerHeight-scene.height*scale)/2+row.feet*scale,
        left: image.left + x * image.width / art.canvas[0], right: image.left + (x + w) * image.width / art.canvas[0],
        slotLeft: slot.left, slotRight: slot.right };
    }));
    for (const pose of shelfPlacement) {
      assert.ok(Math.abs(pose.base - pose.contact) < 1, 'visible console base rests on the shelf surface');
      assert.ok(pose.left >= pose.slotLeft && pose.right <= pose.slotRight, 'console art stays inside its display slot');
    }
    await page.locator('.card-main').first().focus();
    await page.keyboard.press('ArrowDown');
    assert.equal(await page.evaluate(() => document.activeElement.closest('.card').dataset.system), systems[4].id);
    await page.keyboard.press('ArrowRight');
    assert.equal(await page.evaluate(() => document.activeElement.closest('.card').dataset.system), systems[5].id);
    await page.keyboard.press('PageDown');
    assert.equal(await page.locator('#shelfPageStatus').textContent(), '2 / 7');
    assert.equal(await page.evaluate(() => document.activeElement.closest('.card').dataset.system), systems[13].id);
    await page.keyboard.press('End');
    assert.equal(await page.locator('#shelfNext').isDisabled(), true);
    assert.equal(await page.evaluate(() => document.activeElement.closest('.card').dataset.system), systems.at(-1).id);
    await page.keyboard.press('Home');
    const reached=[];
    while(true){
      reached.push(...await page.locator('.display-tier:not([hidden]) .card').evaluateAll(cards=>cards.map(card=>card.dataset.system)));
      if(await page.locator('#shelfNext').isDisabled()) break;
      await page.click('#shelfNext');
    }
    assert.deepEqual(reached,systems.map(system=>system.id),'paging reaches every system once in order');
    await page.locator('.display-tier:not([hidden]) .card-main').first().focus();
    await page.keyboard.press('Home');
    await page.reload();
    assert.equal(await page.locator('#systems').getAttribute('class'), 'systems-shelf', 'third layout persists');
    await page.setViewportSize({ width: 1920, height: 1080 });
    await capture(page, 'display-shelf-desktop.png');
    await page.setViewportSize({ width: 1440, height: 1000 });
    await page.click('#toggleLayout');
    assert.equal(await page.locator('#systems').getAttribute('class'), 'systems-grid');

    // Automatic availability and individual choices compose and survive a reload.
    await openPanel('Consoles');
    assert.equal(await page.locator('input[data-console]').count(), 52);
    await page.click('#scopeAvailable');
    await page.locator('input[data-console="ps2"]').uncheck();
    await closePanel();
    assert.equal(await page.locator('.card').count(), 50);
    assert.equal(await page.locator('.card[data-system="switch2"]').count(), 0);
    assert.equal(await page.locator('.card[data-system="ps2"]').count(), 0);
    await page.reload();
    assert.equal(await page.locator('.card').count(), 50);
    await openPanel('Consoles');
    assert.equal(await page.locator('#scopeAvailable').getAttribute('aria-pressed'), 'true');
    assert.equal(await page.locator('input[data-console="ps2"]').isChecked(), false);
    await page.click('#scopeAll');
    await page.locator('input[data-console="ps2"]').check();
    await page.fill('#consoleSearch', 'Nintendo');
    assert.equal(await page.locator('input[data-console]').count(), 16);
    await page.locator('input[data-family="Nintendo"]').uncheck();
    assert.equal(await page.locator('.card').count(), 36);
    assert.equal(await page.locator('input[data-console]:checked').count(), 0);
    await page.click('#showAllConsoles');
    await page.locator('input[data-console="n64"]').uncheck();
    assert.equal(await page.locator('input[data-family="Nintendo"]').evaluate(input => input.indeterminate), true);
    await page.locator('input[data-console="n64"]').check();
    await page.fill('#consoleSearch', 'no such console');
    assert.equal(await page.locator('#emptyPicker').isVisible(), true);
    assert.equal(await page.locator('#hideAllConsoles').isDisabled(), true);
    await page.fill('#consoleSearch', '');
    await page.click('#hideAllConsoles');
    await closePanel();
    assert.equal(await page.locator('.card').count(), 0);
    assert.equal(await page.locator('#emptySearch').isVisible(), true);
    await page.click('#reviewSettings');
    await page.click('#showAllConsoles');
    await capture(page, 'settings-consoles.png');
    await closePanel();
    assert.equal(await page.locator('.card').count(), 52);

    // Modal keyboard navigation, Escape and focus return.
    await openPanel('Appearance');
    await page.locator('#tabAppearance').focus();
    await page.keyboard.press('ArrowRight');
    assert.equal(await page.locator('#tabBrowse').getAttribute('aria-selected'), 'true');
    await page.keyboard.press('End');
    assert.equal(await page.locator('#tabRoom').getAttribute('aria-selected'), 'true');
    await page.keyboard.press('Escape');
    assert.equal(await page.locator('#settingsDialog').isVisible(), false);
    assert.equal(await page.evaluate(() => document.activeElement.id), 'openAppearance');
    const broken = await page.locator('.console-art img').evaluateAll(async images => {
      const results = await Promise.all(images.map(image => new Promise(resolve => {
        const check = new Image(); check.onload = () => resolve(null); check.onerror = () => resolve(image.src); check.src = image.src;
      })));
      return results.filter(Boolean);
    });
    assert.deepEqual(broken, [], 'all system icons should load');
    await capture(page.locator('.card[data-system="switch2"]'), 'switch2-3d-card.png');
    await page.evaluate(() => window.scrollTo({ top: 0, behavior: 'instant' }));
    await capture(page, 'console-images-grid.png');

    await openPanel('Browse');
    await page.selectOption('#sort', 'az');
    assert.equal(await page.locator('.card-title').first().textContent(), '3DO Interactive Multiplayer');
    await page.selectOption('#sort', 'newest');
    assert.equal(await page.locator('.card-title').first().textContent(), 'Nintendo Switch 2');
    await page.selectOption('#sort', 'oldest');
    assert.equal(await page.locator('.card-title').first().textContent(), 'Atari 2600');
    await page.selectOption('#sort', 'featured');
    await closePanel();
    await page.locator('.card[data-system="ps2"] .favorite').click();
    await openPanel('Browse');
    await page.selectOption('#sort', 'favorites');
    assert.equal(await page.locator('.card-title').first().textContent(), 'PlayStation 2');
    await page.selectOption('#family', 'Nintendo');
    assert.equal(await page.locator('.card').count(), 16);
    await page.selectOption('#family', 'Atari');
    await page.selectOption('#availability', 'mapped');
    assert.equal(await page.locator('.card').count(), 5);
    assert.match(await page.locator('#shelfSummary').textContent(), /Atari/);
    await page.click('#clearFilters');
    assert.equal(await page.locator('.card').count(), 52);
    await closePanel();
    await page.fill('#search', 'mednafen_wswan');
    assert.equal(await page.locator('.card').count(), 2);
    await page.fill('#search', '');

    await openPanel('Appearance');
    await page.click('#lookCards');
    assert.equal(await page.locator('.classic-card').count(), 52);
    await page.click('#layoutCarousel');
    assert.equal(await page.locator('.icon-card').count(), 52);
    assert.equal(await page.locator('#lookCards').isDisabled(), true);
    await capture(page, 'settings-appearance.png');
    await closePanel();
    const floating = await page.locator('.systems-carousel .card').first().evaluate(element => {
      const card = getComputedStyle(element), art = getComputedStyle(element.querySelector('.console-art'));
      return { background: card.backgroundImage, border: card.borderTopWidth, shadow: card.boxShadow, artBackground: art.backgroundImage };
    });
    assert.deepEqual(floating, { background: 'none', border: '0px', shadow: 'none', artBackground: 'none' });
    await page.evaluate(() => window.scrollTo({ top: document.querySelector('.shelf-heading').offsetTop - 24, behavior: 'instant' }));
    await capture(page, 'console-images-swipe.png');
    const beforeMove = await page.locator('#systems').getAttribute('data-active-system');
    await page.click('#carouselNext');
    await page.waitForFunction(() => document.querySelector('#systems').dataset.animating === 'false');
    assert.notEqual(await page.locator('#systems').getAttribute('data-active-system'), beforeMove);
    const depth = await page.locator('#systems').evaluate(track => {
      const bounds = track.getBoundingClientRect(), center = bounds.x + bounds.width / 2;
      const cards = [...track.querySelectorAll('.card')].filter(card => getComputedStyle(card).visibility === 'visible').map(card => {
        const b = card.getBoundingClientRect(); return { active: card.classList.contains('is-selected'), x: b.x + b.width / 2, width: b.width };
      });
      return { center, cards };
    });
    const mainConsole = depth.cards.find(card => card.active);
    assert.ok(Math.abs(mainConsole.x - depth.center) < 2, 'selected console should settle exactly in the centre');
    assert.ok(depth.cards.every(card => card.active || card.width < mainConsole.width * .85), 'selected console should clearly dominate its neighbours');
    for (const direction of [-1, 1]) {
      const side = depth.cards.filter(card => (card.x - depth.center) * direction > 5).sort((a,b) => Math.abs(a.x-depth.center)-Math.abs(b.x-depth.center));
      assert.ok(side.length >= 2 && side[0].width > side[1].width, 'each side should step back in size');
    }
    await page.keyboard.press('Home');
    await page.keyboard.press('ArrowLeft');
    await page.waitForFunction(() => document.querySelector('#systems').dataset.animating === 'false');
    assert.equal(await page.locator('#carouselPosition').textContent(), '52 / 52', 'left from the first console loops to the last');
    await page.keyboard.press('ArrowRight');
    await page.waitForFunction(() => document.querySelector('#systems').dataset.animating === 'false');
    assert.equal(await page.locator('#carouselPosition').textContent(), '1 / 52');
    // Interrupted input retargets one animation rather than stacking a queue.
    await page.keyboard.press('ArrowRight'); await page.keyboard.press('ArrowRight'); await page.keyboard.press('ArrowLeft');
    await page.waitForFunction(() => document.querySelector('#systems').dataset.animating === 'false');
    assert.equal(await page.locator('#carouselPosition').textContent(), '2 / 52');
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await page.click('#carouselNext');
    assert.equal(await page.locator('#systems').getAttribute('data-animating'), 'false', 'reduced motion should settle immediately');
    await page.emulateMedia({ reducedMotion: 'no-preference' });
    const draggedFrom = await page.locator('#systems').getAttribute('data-active-system');
    const stage = await page.locator('#systems').boundingBox();
    await page.mouse.move(stage.x + stage.width / 2, stage.y + stage.height * .41);
    await page.mouse.down(); await page.mouse.move(stage.x + stage.width / 2 - 130, stage.y + stage.height * .41, {steps:6}); await page.mouse.up();
    await page.waitForFunction(() => document.querySelector('#systems').dataset.animating === 'false');
    assert.notEqual(await page.locator('#systems').getAttribute('data-active-system'), draggedFrom, 'drag should select and snap to a new console');
    // Exercise the actual preview polling path with a standard-mapped fake pad.
    await page.evaluate(() => {
      window.testShelfPad = {connected:true,mapping:'standard',axes:[.8,0],buttons:Array.from({length:17},()=>({pressed:false}))};
      Object.defineProperty(navigator,'getGamepads',{configurable:true,value:()=>[window.testShelfPad]});
      window.dispatchEvent(new Event('gamepadconnected'));
    });
    const beforePad = await page.locator('#systems').getAttribute('data-active-system');
    await page.waitForFunction(id => document.querySelector('#systems').dataset.activeSystem !== id, beforePad);
    await page.evaluate(() => { window.testShelfPad.axes[0]=0; window.testShelfPad.buttons[0].pressed=true; });
    await page.waitForFunction(() => document.querySelector('#home').hidden);
    await page.evaluate(() => { window.testShelfPad.buttons[0].pressed=false; window.testShelfPad.buttons[1].pressed=true; });
    await page.waitForFunction(() => !document.querySelector('#home').hidden);
    await page.evaluate(() => { delete navigator.getGamepads; delete window.testShelfPad; });
    await page.fill('#search', 'Switch 2');
    assert.equal(await page.locator('#carouselPrev').isDisabled(), true);
    await page.fill('#search', 'no console matches this');
    assert.equal(await page.locator('#carouselCaption').isVisible(), false);
    await page.fill('#search', '');
    await page.locator('#systems .is-selected img').dispatchEvent('error');
    await page.click('#carouselNext');
    await page.waitForFunction(() => document.querySelector('#systems').dataset.animating === 'false');
    assert.deepEqual(errors, [], 'a failed artwork load should retain navigation and the symbol fallback');
    await page.reload();
    assert.equal(await page.locator('.icon-card').count(), 52);
    await openPanel('Appearance');
    assert.equal(await page.locator('#layoutCarousel').getAttribute('aria-pressed'), 'true');
    assert.equal(await page.locator('#lookCards').getAttribute('aria-pressed'), 'true');
    await page.click('#layoutGrid');
    await page.click('#lookImages');
    await openPanel('Browse');
    assert.equal(await page.locator('#sort').inputValue(), 'favorites');
    await closePanel();
    await page.locator('.card[data-system="ps2"] .card-main').click();
    await page.click('#showLibrary');
    assert.equal(await page.locator('.game').count(), 3);
    assert.equal(await page.locator('#libraryLaunchAction').textContent(), 'Open emulator library');
    assert.match(await page.locator('#libraryLaunchDescription').textContent(), /Choose the game there/);
    await page.click('#back');
    await page.locator('.card[data-system="ps1"] .card-main').click();
    await page.selectOption('#emulator', 'PSXS5');
    await page.click('#showLibrary');
    assert.equal(await page.locator('#libraryLaunchHint').isVisible(), true, 'menu-only information is visible before choosing a game');
    assert.equal(await page.locator('#libraryLaunchAction').textContent(), 'Open emulator library');
    await page.selectOption('#emulator', 'RetroArch · SwanStation');
    assert.equal(await page.locator('#libraryLaunchAction').textContent(), 'Play game', 'the explanation follows a changed preferred emulator');
    await page.click('#back');
    await page.locator('.card[data-system="ps5"] .card-main').click();
    assert.equal(await page.locator('#emulatorLabel').isVisible(), false, 'native PS5 tiles do not offer an emulator choice');
    assert.equal(await page.locator('#openEmulator').isVisible(), false, 'native PS5 tiles expose only isShelf');
    assert.equal(await page.locator('#showLibrary').textContent(), 'Open in isShelf');
    await page.click('#back');
    await page.locator('.card[data-system="ps4"] .card-main').click();
    assert.equal(await page.locator('#emulatorLabel').isVisible(), false, 'native PS4 tiles do not offer an emulator choice');
    assert.equal(await page.locator('#openEmulator').isVisible(), false, 'native PS4 tiles expose only isShelf');
    assert.equal(await page.locator('#showLibrary').textContent(), 'Open in isShelf');
    await page.click('#back');
    await page.locator('.card[data-system="switch2"] .card-main').click();
    assert.ok((await page.locator('#heroImage').getAttribute('src')).endsWith('switch2.png'));
    assert.equal(await page.locator('#openEmulator').isDisabled(), true);
    assert.equal(await page.locator('#showLibrary').isDisabled(), true);
    await page.click('#back');

    // The mobile modal fits, scrolls internally, and keeps Done reachable.
    await page.setViewportSize({ width: 390, height: 844 });
    await openPanel('Consoles');
    await page.fill('#consoleSearch', 'Nintendo');
    await capture(page, 'settings-mobile.png');
    const modal = await page.locator('#settingsDialog').boundingBox();
    assert.ok(modal.x >= 0 && modal.x + modal.width <= 391 && modal.y >= 0 && modal.y + modal.height <= 845);
    assert.equal(await page.locator('#doneSettings').isVisible(), true);
    await page.locator('input[data-console="n64"]').uncheck();
    await page.locator('input[data-console="n64"]').check();
    await openPanel('Appearance');
    await page.click('#layoutShelf');
    await closePanel();
    assert.equal(await page.locator('.display-tier:not([hidden])').first().locator('.card').count(), 2);
    assert.equal(await page.locator('.display-tier').count(), 26);
    await capture(page, 'display-shelf-mobile.png');
    await openPanel('Appearance');
    await page.click('#layoutCarousel');
    await closePanel();
    await capture(page.locator('#systems'), 'mobile-swipe.png');
    const width = await page.evaluate(() => ({ page: document.documentElement.scrollWidth, viewport: innerWidth }));
    assert.ok(width.page <= width.viewport + 1, 'mobile page should not overflow horizontally');
    assert.deepEqual(errors, [], 'the page should have no JavaScript errors');
    console.log('PASS: settings, visibility/persistence, three layouts, shelf contact alignment/spatial input/responsive tiers, 52 icons, curved carousel motion, simulated standard gamepad and library routes.');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
