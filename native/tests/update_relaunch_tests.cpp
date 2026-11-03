// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/update_relaunch.hpp"
#include <cassert>
#include <cstdint>
#include <vector>

namespace {
struct Fixture {
    std::vector<shelf::UpdateRelaunchState> states;
    std::vector<int> launches;
    std::size_t state_at = 0;
    std::size_t launch_at = 0;
    unsigned waited = 0;
    unsigned state_calls = 0;
    unsigned target_at_waited = ~0u;
    unsigned other_at_waited = ~0u;
};

shelf::UpdateRelaunchState state(void* raw) {
    auto& fixture = *static_cast<Fixture*>(raw);
    ++fixture.state_calls;
    if (fixture.state_at < fixture.states.size())
        return fixture.states[fixture.state_at++];
    if (fixture.launch_at && fixture.waited >= fixture.target_at_waited)
        return shelf::UpdateRelaunchState::target;
    if (fixture.launch_at && fixture.waited >= fixture.other_at_waited)
        return shelf::UpdateRelaunchState::other_app;
    return shelf::UpdateRelaunchState::idle;
}

int launch(void* raw) {
    auto& fixture = *static_cast<Fixture*>(raw);
    if (fixture.launches.empty())
        return -1;
    const auto at = fixture.launch_at < fixture.launches.size()
        ? fixture.launch_at : fixture.launches.size() - 1;
    ++fixture.launch_at;
    return fixture.launches[at];
}

void wait(void* raw, unsigned milliseconds) {
    static_cast<Fixture*>(raw)->waited += milliseconds;
}

shelf::UpdateRelaunchOutcome run(Fixture& fixture) {
    return shelf::relaunch_updated_shelf({&fixture, state, launch, wait});
}
} // namespace

int main() {
    {
        Fixture fixture;
        fixture.launches = {0};
        fixture.target_at_waited = shelf::update_relaunch_settle_ms;
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::launched);
        assert(outcome.attempts == 1 && fixture.launch_at == 1);
        assert(fixture.waited == shelf::update_relaunch_settle_ms);
        assert(fixture.state_calls >= shelf::update_relaunch_settle_ms /
                                      shelf::update_relaunch_poll_ms + 2);
    }
    {
        Fixture fixture;
        fixture.launches = {static_cast<int>(0x80aa001au)};
        fixture.target_at_waited = shelf::update_relaunch_settle_ms + 1200;
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::launched);
        assert(outcome.attempts == 1 && fixture.launch_at == 1);
        assert(static_cast<std::uint32_t>(outcome.last_code) == 0x80aa001au);
        assert(fixture.waited == shelf::update_relaunch_settle_ms + 1200);
    }
    {
        Fixture fixture{{shelf::UpdateRelaunchState::other_app}, {0}};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::foreground_changed);
        assert(outcome.attempts == 0 && fixture.launch_at == 0);
    }
    {
        Fixture fixture{{shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::target}, {0}};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::launched);
        assert(outcome.attempts == 0);
        assert(fixture.waited == shelf::update_relaunch_poll_ms * 2);
    }
    {
        Fixture fixture{{shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::other_app}, {0}};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::foreground_changed);
        assert(outcome.attempts == 0);
        assert(fixture.waited == shelf::update_relaunch_poll_ms * 2);
    }
    {
        Fixture fixture;
        fixture.launches = {-1};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::refused);
        assert(outcome.attempts == 1 && fixture.launch_at == 1);
        assert(fixture.waited == shelf::update_relaunch_settle_ms +
                                 shelf::update_relaunch_observe_ms);
    }
    {
        Fixture fixture;
        fixture.launches = {0};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::not_observed);
        assert(outcome.attempts == 1 && fixture.launch_at == 1);
        assert(fixture.waited == shelf::update_relaunch_settle_ms +
                                 shelf::update_relaunch_observe_ms);
    }
    {
        Fixture fixture;
        fixture.launches = {
            static_cast<int>(shelf::update_relaunch_already_running)};
        fixture.target_at_waited = shelf::update_relaunch_settle_ms + 100;
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::launched);
        assert(outcome.attempts == 1);
    }
    {
        Fixture fixture;
        fixture.launches = {0};
        fixture.other_at_waited = shelf::update_relaunch_settle_ms + 100;
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::foreground_changed);
        assert(outcome.attempts == 1 && fixture.launch_at == 1);
    }
    {
        Fixture fixture{{shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::unknown}, {0}};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::service_unavailable);
        assert(outcome.attempts == 0);
        assert(fixture.waited == shelf::update_relaunch_poll_ms);
    }
    {
        Fixture fixture{{shelf::UpdateRelaunchState::unknown}, {0}};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::service_unavailable);
        assert(outcome.attempts == 0);
    }
    assert(shelf::relaunch_updated_shelf({}).result == shelf::UpdateRelaunchResult::invalid);
    return 0;
}
