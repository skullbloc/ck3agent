// declare_war_outcome.h -- the verdict of an AI-pipeline declare_war after its
// dispatch (action_handlers.cpp do_declare_war), in one place so a host test
// can read it.
//
// After the dispatch the war is the game's. The handler reads the actor's war
// count before and after; a count read that faulted cannot say whether a war
// was made, so the answer is "war_unverified" (war_created unknown), never
// "dispatched_but_no_war", which says the pipeline made none (witness #10
// minor 9).
#pragma once

namespace bridge {

struct DeclareWarOutcome {
    bool success;
    const char* status;   // timeout | war_created | war_unverified | dispatched_but_no_war | rejected
};

inline DeclareWarOutcome declare_war_outcome(bool found_result, bool intent_succeeded,
                                             bool war_created, bool count_unread) {
    if (!found_result) return {false, "timeout"};
    if (intent_succeeded && war_created) return {true, "war_created"};
    if (intent_succeeded && count_unread) return {false, "war_unverified"};
    if (intent_succeeded) return {false, "dispatched_but_no_war"};
    return {false, "rejected"};
}

}  // namespace bridge
