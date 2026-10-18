// rollback_state_probe.cpp — DOES AN UNDONE CLAIM DESTROY A REAL ONE? (ck-qv51)
//
// do_set_controlled_character CLAIMS a character, then checks the roster; if the
// roster diverged it calls release_character to undo the claim. The seat never steered
// that character. ck-3ru8's census of all ten release_character call sites found this
// path reaching the succession handoff with ReleaseSubjectState::Unknown — but the
// worse half is that it also overwrote last_steered_character_id with the UNDONE id.
//
// That field is set only on the way out of primary and is never cleared (session.h),
// so a genuine earlier value was destroyed and a later real succession could be
// blinded: the handoff looks for the seat's last subject and finds a character the
// agent never steered, whose state is Unknown, and recovers nothing.
//
// SEVERITY IS BOUNDED AT "SEAT STAYS EMPTY" — measured by ck-zgh4, and worth stating
// because it is what ck-qv51 and ck-cwal negotiate over. The id and the state are
// written as a PAIR inside one session->mutex and one `primary == char_id` guard, so
// they cannot be torn: there is no composite where a never-steered id is paired with
// an older Corpse state, which would be a WRONG SEIZURE and strictly worse than the
// bug ck-7yrp fixed. A blinded handoff returns 0. It never recovers the wrong subject.
//
// AND ck-sozz'S LEGACY FLAG CANNOT SEPARATE THIS, which is the trap: commit_take does
// move between the two builds, so reaching for that flag is the natural instinct. But
// claim_character sets primary_character_id unconditionally on both its branches and
// runs BEFORE the roster check in BOTH builds, so the recording guard fires either
// way. A run of the flag comes back showing identical behaviour, which reads as "the
// flag is broken" or "the defect is not real" rather than "this defect is upstream of
// what the flag moves."

#include "bridge.h"
#include "session.h"
#include "skip_held_heir.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace bridge {
FILE* g_logfile = nullptr;
uintptr_t g_base_addr = 0;
std::vector<MemRange> g_valid_ranges;
std::shared_mutex g_ranges_mutex;
std::mutex g_controlled_mutex;
std::vector<uint32_t> g_controlled_chars;
std::atomic<uint32_t> g_controlled_primary{0};
std::atomic<uint32_t> g_controlled_heir{0};
std::atomic<bool> g_agent_mode{false};
}
using namespace bridge;

static int g_failures = 0;
static void row(const char* n, bool ok, const std::string& d) {
    printf("%-58s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) ++g_failures;
}
static uint32_t last_steered(Session* s) {
    std::lock_guard<std::mutex> lk(s->mutex);
    return s->last_steered_character_id;
}
static ReleaseSubjectState last_state(Session* s) {
    std::lock_guard<std::mutex> lk(s->mutex);
    return s->last_release_state;
}

int main() {
    const uint32_t kReal = 4000;    // a character the seat really steered
    const uint32_t kOther = 4001;   // so the seat is not left empty
    const uint32_t kUndone = 4002;  // the claim the roster refuses

    Session* s = g_session_registry.create("agent-a");
    std::string why;

    // ── A GENUINE RELEASE FIRST, so there is a real value to destroy ─────────
    // ORDER MATTERS AND MY FIRST VERSION HAD IT WRONG. release_character records only
    // when the released id IS the primary, and claim_character sets the primary
    // unconditionally — so claiming kOther AFTER kReal moved the primary off kReal and
    // the setup release recorded nothing. kOther first, then kReal, so kReal is the
    // primary when it is released. (That same unconditional set is why the rollback's
    // recording guard fires at all, in both builds — see the header note.)
    (void)g_session_registry.claim_character(s, kOther, &why);
    if (!g_session_registry.claim_character(s, kReal, &why)) {
        printf("NOT EVALUABLE: setup claim failed: %s\n", why.c_str());
        return 3;
    }
    g_session_registry.release_character(s, kReal, ReleaseSubjectState::Corpse);
    row("setup_a_real_release_is_recorded",
        last_steered(s) == kReal && last_state(s) == ReleaseSubjectState::Corpse,
        "the seat released its own corpse; this is the value the rollback used to "
        "destroy");

    // ── NOW A CLAIM AND ITS ROLLBACK ─────────────────────────────────────────
    // This is the shape do_set_controlled_character takes: claim, then the roster
    // refuses, then undo. claim_character sets primary unconditionally, so at the
    // undo the recording guard `primary == char_id` is TRUE.
    if (!g_session_registry.claim_character(s, kUndone, &why)) {
        printf("NOT EVALUABLE: rollback-setup claim failed: %s\n", why.c_str());
        return 3;
    }
    g_session_registry.release_character(s, kUndone,
                                        ReleaseSubjectState::UndoneClaim);

    // ── THE KNOWN-POSITIVE e2 ASKED FOR ──────────────────────────────────────
    row("a_prior_last_steered_SURVIVES_an_undone_claim",
        last_steered(s) == kReal,
        "still " + std::to_string(last_steered(s)) + ", not the undone " +
        std::to_string(kUndone) + " — the seat never steered that character, so "
        "remembering it would destroy the only record of one it did");
    row("and_the_recorded_STATE_survives_too",
        last_state(s) == ReleaseSubjectState::Corpse,
        "still corpse, not unknown — the pair is preserved together or not at all");

    // ── AND THE CLAIM REALLY IS GONE, which the fix must not break ───────────
    {
        std::lock_guard<std::mutex> lk(s->mutex);
        row("the_undone_claim_is_actually_released",
            s->claimed_character_ids.count(kUndone) == 0
            && s->primary_character_id != kUndone,
            "recording nothing must not mean releasing nothing — the roster refused "
            "it and the registry must not keep ownership");
    }

    // ── THE HANDOFF IS NOT BLINDED ───────────────────────────────────────────
    // The end-to-end consequence, stated as the thing a reader cares about: after an
    // undone claim the seat can still be recovered on its real subject.
    {
        std::lock_guard<std::mutex> lk(s->mutex);
        s->primary_character_id = 0;   //: as a death would leave it
    }
    row("a_later_succession_can_still_recover_the_REAL_subject",
        handoff_subject(0, last_steered(s), last_state(s)) == kReal,
        "the blinding this bead exists to stop: with the undone id and Unknown in "
        "place, this returned 0 and the seat stayed empty");

    // ── AND UndoneClaim ITSELF NEVER RECOVERS, if one ever reaches the handoff ─
    row("UndoneClaim_never_recovers_if_it_reaches_the_handoff",
        handoff_subject(0, kUndone, ReleaseSubjectState::UndoneClaim) == 0,
        "defensive: the state is not stored any more, but a fourth enumerator must "
        "not fall through to a recovery");

    printf("\n%s (%d failing rows)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
