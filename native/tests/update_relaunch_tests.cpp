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
};

shelf::UpdateRelaunchState state(void* raw) {
    auto& fixture = *static_cast<Fixture*>(raw);
    if (fixture.states.empty())
        return shelf::UpdateRelaunchState::idle;
    const auto at = fixture.state_at < fixture.states.size()
        ? fixture.state_at++ : fixture.states.size() - 1;
    return fixture.states[at];
}

int launch(void* raw) {
    auto& fixture = *static_cast<Fixture*>(raw);
    if (fixture.launches.empty())
        return -1;
    const auto at = fixture.launch_at < fixture.launches.size()
        ? fixture.launch_at++ : fixture.launches.size() - 1;
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
        Fixture fixture{{shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::target}, {0}};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::launched);
        assert(outcome.attempts == 1 && fixture.launch_at == 1);
    }
    {
        Fixture fixture;
        fixture.launches = {static_cast<int>(0x80aa001au),
                            static_cast<int>(0x80aa001au), 17};
        fixture.states = {shelf::UpdateRelaunchState::idle,
                          shelf::UpdateRelaunchState::idle,
                          shelf::UpdateRelaunchState::idle,
                          shelf::UpdateRelaunchState::idle,
                          shelf::UpdateRelaunchState::idle,
                          shelf::UpdateRelaunchState::target};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::launched);
        assert(outcome.attempts == 3 && outcome.last_code == 17);
        assert(fixture.waited == shelf::update_relaunch_settle_ms +
                                 shelf::update_relaunch_retry_ms * 2);
    }
    {
        Fixture fixture{{shelf::UpdateRelaunchState::other_app}, {0}};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::foreground_changed);
        assert(outcome.attempts == 0 && fixture.launch_at == 0);
    }
    {
        Fixture fixture{{shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::other_app}, {0}};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::foreground_changed);
        assert(outcome.attempts == 0);
        assert(fixture.waited == shelf::update_relaunch_settle_ms);
    }
    {
        Fixture fixture{{shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::other_app}, {0}};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::foreground_changed);
        assert(outcome.attempts == 1);
    }
    {
        Fixture fixture;
        fixture.launches = {-1};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::refused);
        assert(outcome.attempts == shelf::update_relaunch_rounds *
                                   shelf::update_relaunch_attempts_per_round);
    }
    {
        Fixture fixture;
        fixture.launches = {0};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::not_observed);
        assert(outcome.attempts == shelf::update_relaunch_rounds);
        assert(fixture.waited == shelf::update_relaunch_settle_ms +
                                 shelf::update_relaunch_rounds *
                                     shelf::update_relaunch_observe_ms);
    }
    {
        Fixture fixture{{shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::idle,
                         shelf::UpdateRelaunchState::target},
                        {static_cast<int>(shelf::update_relaunch_already_running)}};
        const auto outcome = run(fixture);
        assert(outcome.result == shelf::UpdateRelaunchResult::launched);
        assert(outcome.attempts == 1);
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
