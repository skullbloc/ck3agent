#pragma once
// L13 D2: the bridge goes DORMANT -> ACTIVE on the first game-thread drain.
//
// Before L13 the transition happened at the top of on_tick, i.e. on the first
// AI tick, so a save loaded paused stayed DORMANT until something unpaused it —
// which is why the bridge used to unpause the game itself at load (the
// ck-w809/ck-7kbi auto-unpause, deleted). The drain runs on every session pass,
// paused or running, so the transition needs no tick.
//
// The decision is this one function, called by drain_game_thread_work
// (bridge_init.cpp) and driven on the host by bridge/test/test_game_thread_reads.cpp.
// It reads nothing about the clock or the tick count: that absence is the
// property. Returns true exactly once: on the call that made the transition.
#include <atomic>

namespace bridge {

template <class State>
inline bool activate_on_first_drain(std::atomic<State>& state) {
    State expected = State::DORMANT;
    return state.compare_exchange_strong(expected, State::ACTIVE,
                                         std::memory_order_acq_rel);
}

}  // namespace bridge
