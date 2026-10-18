#pragma once
// personality_locks.h — the agent-set personality lock table, split out of
// action_dispatch_internal.h so its WORLD-RESET half can be compiled with no
// engine dependencies at all (ck-hxh7, ck-9fc3).
//
// WHY THIS TABLE IS DIFFERENT FROM EVERY OTHER WORLD-SCOPED CACHE
// --------------------------------------------------------------
// Every other cache the ck-hxh7 registry clears is pure DERIVED data: a
// spurious clear costs one recompute. This one WRITES. apply_personality_locks()
// runs three times per on_tick (bridge_init.cpp pre-tick, post-tick and
// end-of-tick) and pokes int8 personality axes straight into a character's
// personality array. The table is keyed by CHARACTER ID, and character ids are
// REUSED across saves. So after a save is loaded into a live process, every
// entry names a DIFFERENT PERSON, and the bridge rewrites that person's
// personality several times a second for the rest of the process — silently,
// permanently, with values belonging to a character from a world that no longer
// exists. That is ck-9fc3, filed P1 off the ck-hxh7 sibling sweep.
//
// WHY THE FIX IS QUARANTINE AND NOT A PLAIN CLEAR
// ----------------------------------------------
// ck-9fc3's objection to registering this table with the world-reset registry
// is correct and still stands: the world fingerprint detects THAT the world
// changed, not WHICH save was loaded, so it cannot tell "the same save was
// reloaded" (the ck-6kvq.5 resume path, where the agent's steering SHOULD
// survive) from "a different save was loaded" (where it must not). Discarding
// steering on every reload would break resume, which is an R7 hard requirement.
//
// So the write set is emptied — which is the entire P1 hazard, and is not
// optional — and the entries are MOVED to a quarantine keyed by the epoch they
// were set in, rather than destroyed. After a reload the bridge writes NOTHING
// until a client sets locks against the new world, and nothing the agent asked
// for has been lost.
//
// ck-9fc3 CLOSED THE OTHER HALF. Quarantined locks are still never
// AUTO-re-applied — nothing in the tick path restores them. They are restored
// on exactly one trigger: a harness DECLARING the save identity of the world
// now loaded (`declare_save_identity`), and only when that declaration proves
// the same save. See world_identity.h for the rule and for why the identity is
// declared rather than inferred. Nothing here reads a character id to decide.
//
// /tmp/ck3-personality-locks.json — the cross-process carrier, and the way this
// hazard could outlive the process — is likewise no longer identity-free: it
// carries a "world" header and is loaded only through the same decision. See
// the persistence block at the bottom of this header.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "world_identity.h"

namespace bridge {

struct PersonalityLock {
    int8_t axes[10];       // Desired values for all 10 axes
    bool locked[10];       // Which axes are locked (vs. left to engine)
};

// shared_mutex: shared_lock for reads (hot path), unique_lock for writes.
// Guards BOTH tables below.
extern std::shared_mutex g_personality_lock_mutex;

// The live write set. apply_personality_locks() iterates exactly this.
extern std::unordered_map<uint32_t, PersonalityLock> g_personality_locks;

// Entries carried over from a world that no longer exists. Never applied.
extern std::unordered_map<uint32_t, PersonalityLock> g_quarantined_personality_locks;

// Registered with the ck-hxh7 world-reset registry. Moves g_personality_locks
// into the quarantine and leaves the live write set EMPTY.
void quarantine_personality_locks_on_world_reset();

// ck-9fc3: the other direction. Moves the quarantine back into the live write
// set and empties the quarantine; returns how many entries moved. Entries the
// new world has ALREADY set are left alone — an explicit set against the world
// now loaded is newer evidence than anything carried over, so it wins.
//
// CALL THIS FROM ONE PLACE ONLY: world_identity_declare(), after
// judge_same_save() has returned YES. It is deliberately not guarded here —
// putting the guard inside would put the rule in two places.
size_t restore_quarantined_personality_locks();

// Diagnostics / acceptance readback (each takes the lock itself).
size_t   personality_lock_count();
size_t   personality_quarantine_count();
uint64_t personality_quarantine_epoch();   // 0 = nothing has ever been quarantined

// ---------------------------------------------------------------------------
// The CROSS-PROCESS carrier (personality_persist.cpp) — ck-9fc3.
// ---------------------------------------------------------------------------
// /tmp/ck3-personality-locks.json is how agent steering escapes the process. It
// used to carry a bare id->axes map, which is an identity-free payload: wire
// load_personality_locks() anywhere and the previous run's steering lands on
// whatever world happens to be loaded next — the ck-9fc3 hazard, promoted from
// "for the life of the process" to "forever".
//
// So the file now carries a "world" header (episode_id, save_sha256,
// start_date_year) written from the identity declared at the time of the save,
// and the reader takes the SAME judge_same_save() decision the in-process
// quarantine takes. A headerless file — anything written before this change —
// names no save, so it can never match and is never loaded. That is the
// conservative default and it is what makes the wiring safe.
//
// The disk carrier additionally requires the SAME EPISODE. Two episodes may
// legitimately replay one save (so `same_save_as` excludes episode_id), but
// they share one host and one /tmp path, and a second episode inheriting the
// first's steering is cross-world contamination by another name.

// The identity the WRITER stamps into the file. Pushed by
// world_identity_declare() rather than pulled by the writer, and the reason is
// lock order: save_personality_locks_locked() is called with
// g_personality_lock_mutex HELD, while world_identity_declare() holds the
// identity module's own mutex and then takes g_personality_lock_mutex to
// restore. A writer that reached back for the identity would close that cycle.
// personality_persist.cpp's copy is a LEAF: it takes nothing else.
void set_personality_locks_identity(const WorldIdentity& id);

// Writes the table plus the identity header. Call while holding
// g_personality_lock_mutex (name kept from the pre-ck-9fc3 signature).
void save_personality_locks_locked();

// Reads the header only. Returns false when the file is absent or carries no
// header. Never touches the tables.
bool peek_personality_locks_identity(WorldIdentity* out);

// Loads the file into the live write set. The caller has already judged the
// header; this only reads. Returns how many entries were loaded.
size_t load_personality_locks_from_disk();

// Path of the carrier, so a test can point it somewhere else. Passing nullptr
// or "" restores the default (/tmp/ck3-personality-locks.json).
void        set_personality_locks_path(const char* path);
const char* personality_locks_path();

} // namespace bridge
