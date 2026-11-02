// latch_guarded_rows.h — a list of engine asks under the fake random scope, one
// row at a time, with the scope latch tested before every row.
//
// construction_check's list mode (action_handlers.cpp do_construction_check) asks
// the construction validity and price of each listed building under the fake
// random scope. A row whose restore trips the scope latch (scope_slot_policy.h)
// must leave every later row unarmed: the latch is tested here before each row is
// asked, and a row asked while latched is refused rng_scope_latched instead
// (counted in arm_refused_latched), so arm_called_while_latched stays 0. The
// row's own ask (evaluate_build) tests the latch again before its arm, so a single
// check refuses the same way.
//
// Header-only and engine-free: the asks come in as callables, so
// bridge/test/latch_guarded_rows_probe.cpp drives this loop with a row that trips
// the latch mid-list.
#pragma once

#include <cstddef>

#include "scope_slot_policy.h"

namespace bridge {

// For i in [0, n): ask(i) while the latch is clear; latched(i) for every row from
// the first one that finds it set.
template <class Ask, class Latched>
inline void for_each_row_under_latch(std::size_t n, Ask&& ask, Latched&& latched) {
    for (std::size_t i = 0; i < n; ++i) {
        if (scope_latched()) {
            g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
            latched(i);
        } else {
            ask(i);
        }
    }
}

}  // namespace bridge
