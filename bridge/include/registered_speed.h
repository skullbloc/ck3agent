// registered_speed.h — THE REGISTERED GAME SPEED, DECLARED ONCE.
//
// An episode DECLARES its speed in the registered-conditions block, beside
// strictness, the budget pair, the exclusion fraction, the horizon endpoints,
// the start save and the tool surface.
//
// WHY THIS FILE EXISTS, and it is not hygiene: the value had TWO HOMES and no
// contract between them.
//
//     scripts/episode_validity.py   PINNED_SPEED = 2      the declared pin
//     bridge/include/flush_scheduler.h  resting_speed = 3 the bridge's own idea
//
// Nothing connected them, and `resting_speed` was UNREACHABLE FROM
// CONFIGURATION — the default, three reads in the planner, and one assignment
// in a unit test, so nothing on the live path could ever SET it. READ THAT
// PRECISELY: it was unreachable from config and extremely reachable in
// EXECUTION, which is the whole point. A reader who takes it for dead code
// concludes the field did nothing and this fix was unnecessary; it ran on every
// flush that touched the clock, at a value nobody could change.
// So the flush planner's restore step re-asserted 3 over a pin of 2 on the
// first flush that carried clock work, ONCE per world (flush_scheduler.cpp:399
// updates the planner's own model, so every later plan saw agreement), and the
// episode then ran the whole horizon at a speed the experiment design does not
// declare while the validator recorded `speed_mismatch` for it.
//
// THE RULE APPLIED HERE is this project's one-contract rule: WHERE TWO
// IMPLEMENTATIONS MUST AGREE ON A VALUE, THE VALUE LIVES IN ONE DECLARATION AND
// BOTH ANSWER TO IT, and neither party owns the contract. The declaration is
// this header and `scripts/registered_speed.py`, which MIRROR each other across
// the language boundary the way `bridge/include/speed_read.h` and
// `episode_validity.STATUS_*` already do — and, exactly as there, the mirror is
// not trusted: `scripts/registered_speed.audit_references()` READS both and
// fails a host test if the name, the spelling of the environment variable or
// the value drift apart. A mirror nobody checks is two homes wearing one name.
//
// THE RESTORE STEP IS NOT THE HAZARD AND IS NOT REMOVED. It is what makes a null-mode
// episode free-run at a FIXED speed at all (pipeline.cpp's `pipeline_start`
// comment says so in as many words). The hard-coded value was the hazard: with
// the declaration carried here the planner rests AT the pin and its
// re-assertion is CORRECT rather than destructive.
//
// PURE. No engine, no globals, no allocation beyond the refusal string. Every
// function here is a total function of its argument, so a host test can ask it
// anything without a game.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace bridge {

// ═══════════════════════════════════════════════════════════════════════════
// THE DECLARATION
// ═══════════════════════════════════════════════════════════════════════════

//: THE declared CK3 game speed — the same for every
//: arm AND for null mode. Mirrored by `scripts/registered_speed.REGISTERED_GAME_SPEED`.
//: This is the ONLY integer literal in the tree that means "the pinned speed";
//: every other site reads it, and the audit fails on a second one.
constexpr int kRegisteredGameSpeed = 2;

//: HOW AN EPISODE DECLARES IT (§9.4). The harness establishes this variable at
//: launch the same way it establishes CK3_EPISODE_HORIZON_START/_END; the
//: bridge READS it here and rests at it. Mirrored by
//: `scripts/registered_speed.ENV_VAR`, and the audit compares the two strings.
constexpr const char* kGameSpeedEnv = "CK3_EPISODE_GAME_SPEED";

//: The value space. CK3 has five speeds; the bridge emits `speed_raw + 1` from
//: `game_state + GAME_SPEED`, so a READING is 1..5 and so is a DECLARATION.
constexpr int kGameSpeedMin = 1;
constexpr int kGameSpeedMax = 5;

//: The speed primitive under clock_write (event_handler.cpp) is the single
//: chokepoint every speed write funnels through, and it caps a write at 3
//: (at 4-5 a FREE-RUNNING agent loop lets deaths and wars
//: resolve between its cycles). The cap is 3 UNLESS the episode
//: DECLARED a faster speed (clamp_speed_write below) — a stepped episode
//: pauses on a game-day alarm, so nothing resolves unseen between its turns.
//: An undeclared world, a refused declaration and every declaration of 1..3
//: (the declared pin is 2) cap at 3 exactly as before. The game's own
//: limit is 5 (g_MaxGameSpeed, re/findings/re_game_speed.md).
constexpr int kGameSpeedSinkCap = 3;

//: Is `s` a speed at all? (Not "is it honourable" — see the cap above.)
constexpr bool game_speed_in_range(int s) {
    return s >= kGameSpeedMin && s <= kGameSpeedMax;
}

// ═══════════════════════════════════════════════════════════════════════════
// RESOLUTION
// ═══════════════════════════════════════════════════════════════════════════

// What one episode declared. THREE STATES, and collapsing any two of them is
// the hazard class this whole file is filed under:
//
//   declared=true,  refused=""     the harness declared a speed; `speed` is it.
//   declared=false, refused=""     NOTHING WAS DECLARED. `speed` is the
//                                  declared default, and `declared`
//                                  is what a bundle must publish beside it so a
//                                  reader can tell a declaration from a
//                                  fallback. The two are the same NUMBER today
//                                  and a different FACT always.
//   declared=false, refused="..."  a declaration was made and is NOT A SPEED.
//                                  The raw text is kept so the bridge can
//                                  publish the refusal instead of swallowing
//                                  it; the default stands, loudly.
//
// A FOURTH WAS ADDED, and it is the one `refused` alone cannot carry:
//
//   present=false                  THE VARIABLE WAS NOT SET. The establishing
//                                  step never ran, or never reached this
//                                  process.
//   present=true, refused=""       IT WAS SET TO NOTHING. The step RAN AND
//                                  COMPUTED NOTHING — a different file to
//                                  repair, and `getenv` distinguishes the two
//                                  at zero cost. Before this they were one
//                                  record.
struct GameSpeedDeclaration {
    int         speed    = kRegisteredGameSpeed;
    bool        declared = false;
    bool        present  = false;        // the variable existed in the env
    std::string refused;                 // the raw text, ONLY when refused
};

//: RENDER THE RAW TEXT FOR A HUMAN. `refused` exists so an operator sees their
//: own bytes back, and that is only true if the bytes they CANNOT SEE are the
//: ones shown most loudly. Under the strip-based behaviour every input that
//: reached `refused` was printable ('2x', 'fast', '9'); THE BYTE RULE ROUTES
//: INVISIBLE BYTES THERE FOR THE FIRST TIME, and a `\r` is the worst of them:
//: printed raw through the WARNING's %s it returns the cursor, the rest of the
//: line overwrites what came before, and the operator reads
//:     CK3_EPISODE_GAME_SPEED='2' is not a speed in 1..5
//: — a perfectly valid value, refused, with no visible reason. They retype 2,
//: get the same refusal, and have no way in. A rule that changes WHICH INPUTS
//: reach a message changes what that message has to be able to say.
inline std::string render_bytes(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (unsigned char c : raw) {
        switch (c) {
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\\': out += "\\\\"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char buf[5];
                    snprintf(buf, sizeof buf, "\\x%02x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Resolve a declaration from raw environment text (`nullptr`/"" = unset).
// TOTAL and pure: every input maps to one of the three states above.
//
// A trailing-garbage parse is a REFUSAL, not a silent prefix read: "2x" is a
// typo, and `strtol` reading it as 2 would make a typo indistinguishable from
// an intention.
// THE RULE: THE RAW BYTES MUST BE EXACTLY ONE OF
// "1".."5". Anything else is a refusal carrying the raw text.
//
// THIS HALF CHANGED TOO, and that is the point. It does not hand the bytes to
// `strtol`, WHICH SKIPS LEADING WHITESPACE — so it was never byte-equal, it was
// ASYMMETRIC: ' 2', '\t2', '02' and '+2' all DECLARED while ' 2 ', '2 ', '2\n'
// and '2\t' refused. Adopting it as the rule would have written "leading
// whitespace is fine, trailing whitespace is a refusal" into the
// experiment design — a sentence nobody would state deliberately, inherited from
// a library function, and contradicting §9.4.3's R4 byte-equality argument used
// to justify it. `'02'` and `'+2'` agreed with the Python half BY ACCIDENT.
//
// The forgiving alternative — strip both ends, then require a bare digit in
// range — is REJECTED ON THE RECORD rather than by omission: the establishing
// path reads the fence and exports it, so a conforming value never carries
// whitespace, and the cost is asymmetric in the same direction every time (a
// refusal costs a launch that dies in the first second; an acceptance costs 2.2
// hours of an episode judged against the wrong pin).
//
// NO strtol, NO strip, NO locale: one byte, in range, and nothing after it.
// `scripts/registered_speed.resolve` is the same rule in Python and
// `scripts/fixtures/registered_speed_vectors.tsv` is the ONE CASE LIST both
// answer to — a rule stated in two languages cannot be derived, and the only
// thing that crosses that boundary is a table.
inline GameSpeedDeclaration resolve_game_speed(const char* raw) {
    GameSpeedDeclaration d;
    if (!raw) return d;                          // UNSET: never established
    d.present = true;
    if (raw[0] >= '0' + kGameSpeedMin && raw[0] <= '0' + kGameSpeedMax
        && raw[1] == '\0') {
        d.speed = raw[0] - '0';
        d.declared = true;
        return d;
    }
    // Including the EMPTY string: set to nothing is a declaration that computed
    // nothing, and `present` is what tells it apart from unset.
    d.refused = raw;
    return d;
}

// The same question asked of the process environment.
inline GameSpeedDeclaration resolve_game_speed_from_env() {
    return resolve_game_speed(getenv(kGameSpeedEnv));
}

// THE SPEED A WRITE LANDS AS. Below 1 is 1; above the ceiling is
// the ceiling, which is kGameSpeedSinkCap, or the declared speed when the
// episode DECLARED one above it. Mirrored by scripts/registered_speed.py
// `write_ceiling`; host-tested by bridge/test/test_speed_ceiling.cpp.
inline int game_speed_write_ceiling(const GameSpeedDeclaration& d) {
    return (d.declared && d.speed > kGameSpeedSinkCap) ? d.speed : kGameSpeedSinkCap;
}

inline int clamp_speed_write(int requested, const GameSpeedDeclaration& d) {
    if (requested < 1) return 1;
    const int ceiling = game_speed_write_ceiling(d);
    return requested > ceiling ? ceiling : requested;
}

}  // namespace bridge
