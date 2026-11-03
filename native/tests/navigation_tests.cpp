// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/navigation.hpp"
#include "shelf/presentation.hpp"
#include <cstdio>
#include <cstdlib>
namespace {
void check(bool value, const char* why) {
    if (!value) {
        std::fprintf(stderr, "FAIL navigation: %s\n", why);
        std::exit(1);
    }
}
} // namespace
void test_navigation() {
    using namespace shelf;
    NavigationInput nav;
    Action out{};
    check(!nav.axis(true, 3000, 7, 0, out), "resting stick drift does not move selection");
    check(nav.axis(true, 22000, 7, 0, out) && out == Action::right, "stick responds immediately");
    check(!nav.axis(false, 22500, 7, 10, out), "small diagonal wobble does not change direction");
    check(!nav.axis(true, 21000, 7, 20, out), "axis event frequency cannot accelerate navigation");
    check(nav.tick(220, out) && out == Action::right, "held gesture repeats after its short initial pause");
    check(nav.axis(false, 32000, 7, 240, out) && out == Action::down, "deliberate stronger direction turns");
    nav.axis(true, 0, 7, 250, out);
    nav.axis(false, 0, 7, 251, out);
    check(!nav.tick(1000, out), "neutral stops repeat");
    check(nav.button(Action::left, true, 7, 1100, out), "D-pad tap responds immediately");
    nav.button(Action::left, false, 8, 1101, out);
    check(nav.tick(1320, out) && out == Action::left, "another controller cannot release a held button");
    check(!nav.axis(true, 32000, 7, 1330, out), "stick does not steal an active D-pad hold");
    nav.cancel();
    check(!nav.tick(2000, out) && !nav.axis(true, 31000, 7, 2001, out),
          "opening a page cancels repeat and requires the stick to return to neutral");
    nav.axis(true, 0, 7, 2002, out);
    check(nav.axis(true, -24000, 7, 2003, out) && out == Action::left, "a new gesture works after neutral");
    nav.disconnected(7);
    check(!nav.tick(3000, out), "disconnect stops a held gesture");
    nav.button(Action::down, true, 9, 0xffffff80u, out);
    check(!nav.tick(0x5bu, out) && nav.tick(0x5cu, out), "repeat timing handles the clock wrapping");
    nav.cancel();
    nav.button(Action::right, true, 9, 5000, out);
    check(nav.tick(9000, out) && !nav.tick(9000, out) && !nav.tick(9059, out) && nav.tick(9060, out),
          "a delayed frame emits one move and sustained holds accelerate without a backlog");
    std::puts(
        "PASS: physical gesture repeat, diagonal hysteresis, drift, page changes, device ownership and clock wrap.");
    OpeningGate opening;
    opening.sample(10, 0, 2);
    check(!opening.ready(6000), "known background state cannot time out into the opening animation");
    opening.sample(6000, 0, 0);
    check(!opening.ready(6119) && opening.ready(6120), "opening waits for a stable foreground interval");
    opening.sample(6200, 0, 1);
    check(!opening.ready(9000), "a system overlay pauses the opening clock");
    opening.sample(9000, -1, 0);
    opening.sample(13999, -1, 0);
    check(!opening.ready(13999), "status errors have their own bounded fallback clock");
    opening.sample(14000, -1, 0);
    check(opening.used_fallback() && !opening.ready(14119) && opening.ready(14120),
          "an unavailable status API does not trap the user on the loading screen");
    opening.sample(15000, 0, 2);
    check(!opening.ready(20000) && !opening.used_fallback(), "known background cancels an earlier fallback");
    opening.sample(20000, 0, 0);
    check(!opening.ready(20119) && opening.ready(20120), "returning to the foreground restarts the stable interval");
    OpeningGate owner_escape;
    owner_escape.sample(100, 0, 2);
    owner_escape.allow_owner_input(500);
    owner_escape.sample(550, 0, 2);
    check(owner_escape.used_owner_input() && !owner_escape.ready(619) && owner_escape.ready(620),
          "physical owner input escapes a stale known-background opening without an immediate action");
    OpeningGate wrapped_opening;
    wrapped_opening.sample(0xffffffc0u, 0, 0);
    check(!wrapped_opening.ready(0x37u) && wrapped_opening.ready(0x38u), "opening gate handles timer wrap");
    std::puts("PASS: foreground opening gate, owner escape, overlay pause, unavailable-status fallback and timer wrap.");
    check(!opening_inventory_ready(false, opening_inventory_min_ms - 1) &&
              !opening_inventory_ready(true, opening_inventory_min_ms - 1),
          "opening keeps its minimum branded loading interval even when discovery is fast");
    check(opening_inventory_ready(true, opening_inventory_min_ms),
          "completed discovery reveals an already-populated Home after the minimum interval");
    check(!opening_inventory_ready(false, opening_inventory_timeout_ms - 1) &&
              opening_inventory_ready(false, opening_inventory_timeout_ms),
          "slow discovery keeps the loading screen before a bounded responsive fallback");
    std::puts("PASS: inventory-aware opening avoids an empty Home without allowing discovery to trap controls.");
    LaunchVisibilityGate launch_visibility;
    check(launch_visibility.sample(100, 0, 1) == LaunchVisibilityDecision::wait &&
              launch_visibility.sample(1000, 0, 0) == LaunchVisibilityDecision::wait &&
              launch_visibility.sample(1119, 0, 0) == LaunchVisibilityDecision::wait &&
              launch_visibility.sample(1120, 0, 0) == LaunchVisibilityDecision::commit,
          "launcher waits for a short stable foreground interval after an overlay");
    launch_visibility.reset();
    check(launch_visibility.sample(2000, -1, 0) == LaunchVisibilityDecision::wait &&
              launch_visibility.sample(6999, 0, 2) == LaunchVisibilityDecision::wait &&
              launch_visibility.sample(7000, 0, 2) == LaunchVisibilityDecision::cancel,
          "launcher visibility errors and background state have a bounded cancellation time");
    launch_visibility.reset();
    check(launch_visibility.sample(0xffffffc0u, 0, 0) == LaunchVisibilityDecision::wait &&
              launch_visibility.sample(0x37u, 0, 0) == LaunchVisibilityDecision::wait &&
              launch_visibility.sample(0x38u, 0, 0) == LaunchVisibilityDecision::commit,
          "launcher foreground stability handles timer wrap");
    std::puts("PASS: launch handoff tolerates brief overlays but remains foreground- and timeout-gated.");
    CloseGesture close;
    Presentation view;
    close.press(7, 100);
    close.update(1599, view);
    check(view.hold > .49f && view.hold < .51f && !close.closing(), "Circle shows actual hold progress");
    check(!close.release(7, 1600), "an interrupted long hold cancels without navigating back");
    close.update(1700, view);
    check(view.hold == 0 && !close.finished(5000), "releasing before three seconds does not close");
    close.press(7, 6000);
    check(close.release(7, 6100), "a short Circle tap retains normal Back behaviour");
    close.press(7, 7000);
    check(!close.release(8, 7100), "another controller cannot end a hold");
    close.update(9999, view);
    check(!close.closing(), "three-second threshold is not rounded down");
    close.update(10000, view);
    check(close.closing() && !close.finished(10000), "three seconds starts a separate closing animation");
    close.update(10120, view);
    check(view.close > .49f && view.close < .51f, "close transition has an intermediate frame");
    check(close.finished(10240), "close waits until the quicker fade completes");
    CloseGesture released_close;
    released_close.press(4, 10);
    check(!released_close.release(4, 3010) && released_close.closing(),
          "releasing exactly at three seconds still commits closure without an intermediate update");
    std::puts("PASS: three-second Circle hold, cancellation, ordinary Back taps and timed closing fade.");
    CloseHint hint;
    view = {};
    hint.show(100);
    hint.update(350, view);
    check(view.close_hint == 1 && view.close == 0, "a tap hint is visible without starting a close");
    hint.update(2900, view);
    check(view.close_hint > 0 && view.close_hint < 1, "the hint fades near its expiry");
    hint.update(3100, view);
    check(view.close_hint == 0, "the hint disappears after three seconds");
    hint.show(0xffffff80u);
    hint.update(0x80u, view);
    check(view.close_hint == 1, "hint expiry handles timer wrap");
    hint.dismiss();
    hint.update(0x90u, view);
    check(view.close_hint == 0, "another action can dismiss the hint immediately");
    ShelfPageMotion pages;
    pages.select(1, 1000, false);
    pages.select(1, 1080, false);
    check(pages.active && pages.outgoing() && pages.offset() < 0 && pages.opacity() < 1,
          "next page moves the outgoing consoles to the left");
    pages.select(1, 1110, false);
    check(!pages.outgoing() && pages.page() == 1 && pages.offset() > 0,
          "incoming consoles arrive from the right in the same shelf positions");
    pages.select(0, 1120, false);
    pages.select(0, 1200, false);
    check(pages.direction == -1 && pages.offset() > 0, "rapid reverse paging uses the latest requested direction");
    pages.select(0, 1380, false);
    check(!pages.active && pages.offset() == 0 && pages.opacity() == 1, "page motion settles with opaque stationary consoles");
    pages.select(2, 1700, true);
    check(!pages.active && pages.page() == 2, "reduced motion changes pages without sliding");
}
