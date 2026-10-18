// observer_declaration_probe.cpp — the episode-observer declaration policy (ck-obsv).
//
// Drives the REAL predicates from bridge/src/session.cpp. They live there rather
// than in pipeline.cpp for a reason that is also why this probe is cheap: they
// are facts about SESSIONS and about POLICY, with no engine behind them, so a
// host test can ask them without a socket, a pipeline or a game.
//
// Prints TSV; scripts/acceptance_observer_declaration.py asserts on values.

#include "session.h"
#include "bridge.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

// The stub set the other host probes carry, for the same reason: session.cpp
// pulls in world_identity.cpp and world_epoch.cpp, whose guarded reads need
// these. Nothing here is exercised — the policies under test touch none of it.
namespace bridge {
FILE* g_logfile = nullptr;
uintptr_t             g_base_addr = 0;
std::vector<MemRange> g_valid_ranges;
std::shared_mutex     g_ranges_mutex;
std::mutex g_controlled_mutex;
std::vector<uint32_t> g_controlled_chars;
std::atomic<uint32_t> g_controlled_primary{0};
std::atomic<uint32_t> g_controlled_heir{0};
std::atomic<bool> g_agent_mode{false};
}  // namespace bridge

using namespace bridge;

int main() {
    printf("case\tvalue\n");
    // THE FLOOR: who may declare at all.
    printf("may_declare_control\t%d\n",
           episode_observer_may_declare(Session::Mode::CONTROL) ? 1 : 0);
    printf("may_declare_observe\t%d\n",
           episode_observer_may_declare(Session::Mode::OBSERVE) ? 1 : 0);

    // FIRST-WINS, as a pure policy rather than through the plumbing.
    printf("winner_from_empty\t%s\n",
           episode_observer_winner("", "sess_meter").c_str());
    printf("winner_second_claimant\t%s\n",
           episode_observer_winner("sess_meter", "sess_other").c_str());
    printf("winner_same_twice\t%s\n",
           episode_observer_winner("sess_meter", "sess_meter").c_str());

    // ── ck-m3n5: THE ENFORCEMENT, DRIVEN ────────────────────────────────────
    // The rows above are the two POLICIES. This is the DECISION the bridge acts
    // on — the one pipeline_declare_episode_observer switches on and the only
    // thing standing between a declaration and set_privileged_reader. Driving it
    // makes the floor's enforcement BEHAVIOURALLY witnessed instead of witnessed
    // by the spelling of a call site, which is what ck-t5bc's confer-anyway
    // mutant walked through while the lane read 9/9.
    auto name = [](ObserverDecision d) {
        switch (d) {
        case ObserverDecision::Confer:             return "confer";
        case ObserverDecision::RefusedByMode:      return "refused_by_mode";
        case ObserverDecision::RefusedByFirstWins: return "refused_by_first_wins";
        }
        return "?";
    };
    // A CONTROL session, with the privilege UNCLAIMED. The dangerous case: there
    // is no other holder, so first-wins alone would confer it.
    printf("decide_control_unclaimed\t%s\n",
           name(episode_observer_decision("", "sess_driver",
                                          Session::Mode::CONTROL)));
    // A CONTROL session that ALREADY holds it — a floor that only guards the
    // transition would let this one through.
    printf("decide_control_holding\t%s\n",
           name(episode_observer_decision("sess_driver", "sess_driver",
                                          Session::Mode::CONTROL)));
    // A CONTROL session that is ALSO a second claimant. THIS is the case that
    // makes the order load-bearing: both refusals apply, and the answer must be
    // the MODE one. Without this row the floor could be asked second and every
    // other row would be unchanged — which is exactly what a control caught.
    printf("decide_control_second\t%s\n",
           name(episode_observer_decision("sess_meter", "sess_driver",
                                          Session::Mode::CONTROL)));
    printf("decide_observe_unclaimed\t%s\n",
           name(episode_observer_decision("", "sess_meter",
                                          Session::Mode::OBSERVE)));
    printf("decide_observe_second\t%s\n",
           name(episode_observer_decision("sess_meter", "sess_other",
                                          Session::Mode::OBSERVE)));
    printf("decide_observe_redeclare\t%s\n",
           name(episode_observer_decision("sess_meter", "sess_meter",
                                          Session::Mode::OBSERVE)));
    return 0;
}
