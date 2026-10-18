// named_trigger_eval.cpp — closes ck-fo7x.4 (ADR-0001 §); D2 cure (ck-ay9o)
// per docs/reports/bridge-investigation-20260801/ probes-round3.md §P2 +
// verify-round3.md §4.1 (round-4 borrowed-head root).
//
// The trigger catalogue (re/rtti/trigger_subclasses.csv, 2049 rows) maps every
// CJominiTrigger subclass to its vtable and evaluator address. This module
// loads the catalogue on first use and exposes a single deferred query
// (evaluate_named_trigger) that lets a Python caller ask
//   "evaluate trigger X against scope {character: Y}"
// without going through an interaction definition.
//
// Evaluation path (game-thread):
//   1. Resolve trigger name → vtable_vfunc_rva via the catalogue.
//   2. Reuse the existing TLS pipeline (INTERACTION_TLS_CONSTRUCT +
//      INTERACTION_CTX_INIT/RESET/SET_TARGET) with a sentinel interaction to
//      build a scope frame whose scope_object is the requested character.
//      Probe P2 (round 3) proved this frame sound: real interaction trigger
//      blocks evaluate correctly against it, in both directions.
//   3. Build a stub LEAF node for the requested trigger. Its vptr points at a
//      shadow COPY of the class vtable in which the evaluate slots (0x28 and
//      0xd0) forward through recording thunks — a positive signal that the
//      evaluate vfunc actually dispatched.
//   4. BORROW a real compiled trigger-block head: byte-copy the sentinel
//      interaction's embedded 0xE0 head (the exact objects the proven
//      process_trigger_query path hands to the evaluator — it passes
//      interaction_def+slot_offset directly) and graft the stub leaf as its
//      only child in BOTH child lists. The copy carries the compiled
//      instance state (and_trigger::SCOPE_CTX_PTR/SCOPE_TYPE/SCOPE_DATA_PTR
//      at +0x10/+0x18/+0x28, subtype byte, allocator vtable, diag/label
//      pointers) that the compound machinery uses to type the sub-scope for
//      its children. Falls back to the round-3 synthetic CAndTrigger root
//      (known insufficient, kept so replies stay typed) when no head passes
//      the sanity gate.
//   5. Call FUN_07628500(root_node, scope_ctx) under crash protection.
//   6. Report `evaluated:true` + result ONLY when the leaf's evaluate vfunc
//      actually ran; otherwise a typed `scope_rejected` error. A pre-dispatch
//      short-circuit 0 can never read as a real `false` again. The reply
//      names the root used (`root_source`/`root_class`) so live probes can
//      tell the cure path from the fallback at a glance.
//
// History (defect D2 — born broken 9d82709 05-20; envelope honesty 08-01;
// cure attempt 2 08-06): this module originally passed the bare stub leaf as
// the evaluation ROOT and stamped `evaluated:true` unconditionally after the
// evaluator returned. FUN_07628790 rejected the leaf's scope pre-dispatch
// every time (evaluator RVAs 0x656cd30 AND 0x675cd10 both affected — shared
// path, not per-evaluator), so every call returned result:false for every
// character while CK3's error.log accumulated "untyped trigger [ Wrong scope
// for trigger: none, expected character ] / Script location: Unknown" lines.
// Known since ck-09q4u.4; see surface-audit.md D2 and probes-round3.md P2.
//
// Round 3 (08-01) wrapped the leaf in a SYNTHETIC CAndTrigger root built from
// the RTTI class vtable alone. Engine still rejected (verify-round3 §4.1:
// same two script_engine FAILs, replies honestly typed scope_rejected).
// Postmortem: the class vtable carries the compound's CODE but none of the
// compiled INSTANCE state — a real head's +0x10/+0x18/+0x28 scope-typing
// fields (HIGH, core-dump traversal, ck3_offsets.h and_trigger) are what let
// the compound type the sub-scope its leaf children are validated against.
// A zeroed root types nothing, so the child leaf still saw an untyped frame
// and was rejected pre-dispatch — P2.4's mechanism, one level deeper. Hence
// round 4: borrow the whole head, not just its class.
//
// Limitations (recorded in re/findings/re_evaluate_named_trigger.md):
//   - Stateful triggers that read members from the node (has_trait wants a
//     trait_id) get zeroed state and may return a wrong-but-survivable
//     result. Stateless character predicates are the supported surface.
//   - Only character-scope triggers route via the sentinel interaction's
//     actor slot; war/title scopes are not yet wired up.

#include "action_dispatch_internal.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "named_trigger_eval.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bridge {

// Crash-protection TLS is declared `extern thread_local` in
// action_dispatch_internal.h. lookup_character / lookup_interaction come
// from bridge.h via the same include chain — no redundant `extern` here.

// TLS pipeline typedefs (mirror ai_eval_hook.cpp). Hidden in an anonymous
// namespace so we don't conflict with the originals.
namespace {
using fn_ctx_init        = void(*)(uintptr_t* tls_ctx, void* interaction, void* character);
using fn_ctx_reset       = void(*)(uintptr_t* tls_ctx);
using fn_ctx_set_target  = void(*)(uintptr_t* tls_ctx, uint32_t target_char_id, char do_setup);
using fn_tls_construct   = void(*)(void* tls_ctx);
using fn_tls_destructor  = void(*)(void* tls_ctx);
using fn_trigger_eval    = bool (*)(uintptr_t trigger_node, uintptr_t scope_ctx);
}  // namespace

// TLS offsets — kept in sync with ai_eval_hook.cpp via runtime discovery.
// We can't reach into that file's `static`s, but the defaults are correct
// for the current binary and the worst case (mismatch) is a clean error.
static constexpr uintptr_t kTlsGuardOffsetDefault   = 0x6e8;
static constexpr uintptr_t kTlsContextOffsetDefault = 0x3a78;

NamedTriggerQuery g_named_trigger_query = {};

// =============================================================================
// Catalogue storage
// =============================================================================

static std::mutex                                       g_cat_mutex;
static std::atomic<bool>                                g_cat_loaded{false};
static std::unordered_map<std::string,
                          TriggerCatalogueEntry>        g_cat_by_class CK3_EARLY_INIT;
static std::unordered_map<std::string, std::string>     g_cat_by_keyword CK3_EARLY_INIT;

static const char* const k_catalogue_search_paths[] = {
    // Operator override.
    nullptr,  // filled from env at runtime
    // Canonical worktree layouts. The bridge runs from /tmp/libck3_bridge.so
    // and CK3's CWD varies, so we search a handful of well-known prefixes.
    "./re/rtti/trigger_subclasses.csv",
    "../re/rtti/trigger_subclasses.csv",
    "/home/admin/gc/ck3proj/re/rtti/trigger_subclasses.csv",
    "/home/admin/workspace/ck3proj/re/rtti/trigger_subclasses.csv",
};

// Curated alias table for the ADR smoke-test keywords plus a few obvious
// neighbours. The catalogue contains mangled class names like
// `27CScopedBoolConditionTriggerI17CIsAliveConditionE`; modders write
// `is_alive`. Hand-curating the well-known mappings is cheaper and more
// correct than guessing.
//
// HIGH-confidence alias rows. Keep this small — the algorithmic fallback in
// resolve_keyword_to_class handles the long tail.
static const struct {
    const char* keyword;
    const char* class_name;   // mangled, exact match to catalogue
} k_alias_table[] = {
    // ADR smoke test (re_trigger_subclass_catalogue.md §"is_alive" search).
    {"is_alive",
     "27CScopedBoolConditionTriggerI17CIsAliveConditionE"},
    // CScopedBoolConditionTrigger family — same template pattern as is_alive,
    // mechanical fallback skips template parameters so curated mapping is
    // required (ck-yh5d.1: live-tested keyword returned trigger_not_in_catalogue).
    {"is_ruler",
     "27CScopedBoolConditionTriggerI17CIsRulerConditionE"},
    // Target existence — mechanical fallback yields "jomini_target_exists",
    // not the script keyword `exists` (ck-yh5d.1).
    {"exists",                   "26CJominiTargetExistsTrigger"},
    // War-side predicates from re_scope_getters_catalogue.md (HIGH).
    {"is_at_war",                "15CIsAtWarTrigger"},
    {"is_at_war_with_liege",     "24CIsAtWarWithLiegeTrigger"},
    {"is_in_civil_war",          "20CIsInCivilWarTrigger"},
    {"is_civil_war",             "17IsCivilWarTrigger"},
    {"is_at_war_as_attacker",    "17CIsAtWarAsTriggerILb1EE"},
    {"is_at_war_as_defender",    "17CIsAtWarAsTriggerILb0EE"},
    {"has_raised_armies",        "23CHasRaisedArmiesTrigger"},
    {"has_valid_casus_belli",    "25HasValidCasusBelliTrigger"},
    {"is_white_peace_possible",  "27IsWhitePeacePossibleTrigger"},
    {"is_war_leader",            "19CIsWarLeaderTrigger"},
    {"is_participant",           "21CIsParticipantTrigger"},
};

// Note (ck-yh5d.1): `ai_has_no_lord` was on the live-test failure list but
// is *not* a built-in CJominiTrigger subclass — string scan of the core dump
// shows no occurrence, and the 2049-row catalogue has no matching class. It
// is either a script-defined wrapper (parsed from common/scripted_triggers/
// at game load) or a test-side composition. Built-in equivalent: combine
// `is_ai` + `is_independent_ruler` (both already resolvable: CIsAITrigger
// -> is_ai and CIsIndependentRulerTrigger -> is_independent_ruler via the
// mechanical fallback). Script-defined-trigger evaluation would need a
// separate handler that walks the runtime trigger registry.

// =============================================================================
// CSV / keyword resolver
// =============================================================================

// Strip the Itanium length prefix from a mangled class name in-place.
// "27CScopedBoolConditionTriggerI17CIsAliveConditionE" → "CScoped..."
// (Only used for diagnostics; lookup keys keep the prefix.)
static std::string strip_length_prefix(const std::string& mangled) {
    size_t i = 0;
    while (i < mangled.size() && isdigit(static_cast<unsigned char>(mangled[i]))) ++i;
    return mangled.substr(i);
}

// Best-effort CamelCase → snake_case, stripping a leading 'C' and a trailing
// 'Trigger' / 'Condition' suffix. Mirrors the rule documented in
// re_scope_getters_catalogue.md §"Script-keyword mapping confidence" and only
// applies to *simple* class names (no template specializations).
static std::string mechanical_keyword(const std::string& mangled_or_demangled) {
    std::string s = strip_length_prefix(mangled_or_demangled);
    // Templated classes (presence of 'I' followed by digits or 'L') are too
    // ambiguous for a mechanical conversion. Bail.
    for (size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] == 'I' &&
            (isdigit(static_cast<unsigned char>(s[i+1])) ||
             s[i+1] == 'L')) {
            return "";
        }
    }
    // Drop leading 'C' (matches catalogue convention; three classes lack it
    // — IsCivilWarTrigger etc. — but the alias table covers them).
    if (!s.empty() && s.front() == 'C' && s.size() > 1 &&
        isupper(static_cast<unsigned char>(s[1]))) {
        s.erase(0, 1);
    }
    // Drop trailing "Trigger" or "Condition".
    auto try_drop_suffix = [&](const char* suf) {
        size_t n = strlen(suf);
        if (s.size() >= n &&
            s.compare(s.size() - n, n, suf) == 0) {
            s.erase(s.size() - n);
        }
    };
    try_drop_suffix("Trigger");
    try_drop_suffix("Condition");
    if (s.empty()) return "";
    // CamelCase → snake_case.
    std::string out;
    out.reserve(s.size() + 4);
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (i > 0 && isupper(static_cast<unsigned char>(c)) &&
            !isupper(static_cast<unsigned char>(s[i-1]))) {
            out += '_';
        }
        out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Parse one hex column ("0x..."). Returns 0 on empty / parse failure.
static uint64_t parse_hex(const std::string& s) {
    if (s.empty()) return 0;
    return strtoull(s.c_str(), nullptr, 16);
}

// One CSV row, *very* lightly parsed — no embedded commas, no quoting. The
// catalogue is written by re/coredump/scan_trigger_subclasses.py and the
// format is stable.
static bool parse_row(const std::string& line, TriggerCatalogueEntry& out) {
    std::vector<std::string> cols;
    cols.reserve(14);
    std::string cur;
    for (char c : line) {
        if (c == ',') { cols.push_back(std::move(cur)); cur.clear(); }
        else cur += c;
    }
    cols.push_back(std::move(cur));
    if (cols.size() < 14) return false;
    out.class_name        = cols[0];
    out.demangled         = cols[1];
    out.vtable_vfunc_rva  = parse_hex(cols[6]);
    out.evaluator_rva     = parse_hex(cols[8]);
    out.evaluator_in_text = (cols[10] == "yes");
    if (cols[13] == "HIGH")        out.confidence = 'H';
    else if (cols[13] == "MEDIUM") out.confidence = 'M';
    else                           out.confidence = 'L';
    return true;
}

static bool try_open(const char* path, std::ifstream& f) {
    if (!path || !*path) return false;
    f.open(path);
    return static_cast<bool>(f);
}

// Load the catalogue. Thread-safe; idempotent. Returns the number of rows
// added (0 on failure). Failures are non-fatal — handlers report
// `catalogue_not_loaded` to the caller.
static size_t load_catalogue_locked() {
    if (g_cat_loaded.load(std::memory_order_acquire)) {
        return g_cat_by_class.size();
    }
    std::ifstream f;
    const char* env_path = std::getenv("CK3_TRIGGER_CATALOGUE_CSV");
    bool opened = try_open(env_path, f);
    if (!opened) {
        for (size_t i = 1;  // skip the env-var slot
             i < sizeof(k_catalogue_search_paths) / sizeof(*k_catalogue_search_paths);
             ++i) {
            if (try_open(k_catalogue_search_paths[i], f)) {
                ADLOG("NAMED_TRIGGER: loaded catalogue from %s\n",
                      k_catalogue_search_paths[i]);
                opened = true;
                break;
            }
        }
    } else {
        ADLOG("NAMED_TRIGGER: loaded catalogue from CK3_TRIGGER_CATALOGUE_CSV=%s\n", env_path);
    }
    if (!opened) {
        ADLOG("NAMED_TRIGGER: catalogue CSV not found in any search path\n");
        return 0;
    }
    std::string line;
    // Skip header.
    if (!std::getline(f, line)) return 0;
    size_t n_rows = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        TriggerCatalogueEntry entry;
        if (!parse_row(line, entry)) continue;
        // Keep only HIGH/MEDIUM rows that have a usable vtable. LOW rows
        // have no primary vtable in the binary — invoking them would be
        // a wild jump.
        if (entry.vtable_vfunc_rva == 0) continue;
        g_cat_by_class[entry.class_name] = std::move(entry);
        ++n_rows;
    }
    // Seed the keyword index from the curated alias table.
    for (size_t i = 0; i < sizeof(k_alias_table) / sizeof(*k_alias_table); ++i) {
        auto it = g_cat_by_class.find(k_alias_table[i].class_name);
        if (it != g_cat_by_class.end()) {
            g_cat_by_keyword[k_alias_table[i].keyword] = it->first;
        } else {
            ADLOG("NAMED_TRIGGER: alias '%s' -> '%s' not in catalogue (alias skipped)\n",
                  k_alias_table[i].keyword, k_alias_table[i].class_name);
        }
    }
    // Add mechanical keywords for HIGH-confidence simple classes — only when
    // the slot isn't already populated by the curated alias table (to keep
    // hand-validated mappings authoritative).
    for (const auto& kv : g_cat_by_class) {
        if (kv.second.confidence != 'H') continue;
        if (!kv.second.evaluator_in_text) continue;
        std::string kw = mechanical_keyword(kv.second.demangled);
        if (kw.empty()) continue;
        // Don't clobber curated entries.
        if (g_cat_by_keyword.count(kw)) continue;
        g_cat_by_keyword[kw] = kv.first;
    }
    g_cat_loaded.store(true, std::memory_order_release);
    ADLOG("NAMED_TRIGGER: catalogue loaded — %zu rows, %zu keywords\n",
          g_cat_by_class.size(), g_cat_by_keyword.size());
    return n_rows;
}

static size_t ensure_loaded() {
    if (g_cat_loaded.load(std::memory_order_acquire)) {
        return g_cat_by_class.size();
    }
    std::lock_guard<std::mutex> lock(g_cat_mutex);
    return load_catalogue_locked();
}

size_t trigger_catalogue_size() { return g_cat_by_class.size(); }
bool   trigger_catalogue_loaded() { return g_cat_loaded.load(std::memory_order_acquire); }

const TriggerCatalogueEntry* lookup_trigger_entry(const char* name) {
    if (!name || !*name) return nullptr;
    ensure_loaded();
    // 1. Exact mangled-class match.
    auto it = g_cat_by_class.find(name);
    if (it != g_cat_by_class.end()) return &it->second;
    // 2. Script-keyword alias.
    if (const char* canonical = resolve_keyword_to_class(name)) {
        auto it2 = g_cat_by_class.find(canonical);
        if (it2 != g_cat_by_class.end()) return &it2->second;
    }
    return nullptr;
}

const char* resolve_keyword_to_class(const char* keyword) {
    if (!keyword || !*keyword) return nullptr;
    ensure_loaded();
    auto it = g_cat_by_keyword.find(keyword);
    if (it == g_cat_by_keyword.end()) return nullptr;
    return it->second.c_str();
}

// =============================================================================
// Game-thread evaluation
// =============================================================================

// The stub leaf node only needs the vtable pointer at +0, but we pad it out
// to the full CJominiTrigger footprint (ck3::trigger_struct::TOTAL_SIZE) so
// any member reads — subtype byte +0x20, diag ptr +0x48, child lists
// +0x50/+0x98 (zero count = empty), per re_trigger_evaluator_07628500_deep.md
// — land on safe zero bytes rather than past the end.
static constexpr size_t TRIGGER_NODE_STUB_SIZE = ck3::trigger_struct::TOTAL_SIZE;  // 0xE0

// Evaluation-root buffer size (0xE0 — the embedded trigger-block head size,
// live-proven). The PREFERRED root is a byte-copied real head (see the
// borrowed-head section below); kAndRootClass names the catalogue row for
// the round-3 synthetic FALLBACK root (RTTI 11CAndTrigger, vtable 0xbe147b0
// — see ck3_offsets.h decision_type comments), which is known insufficient
// on its own: the class vtable carries the compound's code but not the
// compiled scope-typing instance state (verify-round3 §4.1 postmortem).
static constexpr const char* kAndRootClass = "11CAndTrigger";
static constexpr size_t AND_ROOT_NODE_SIZE = ck3::trigger_struct::TOTAL_SIZE;  // 0xE0

// =============================================================================
// Shadow-vtable dispatch detection (D2: scope_rejected vs genuine false)
// =============================================================================
//
// FIX 4.1 requires `evaluated:true` ONLY when the leaf's evaluate vfunc
// actually ran. FUN_07628790's scope validation returns 0 WITHOUT dispatching
// when it rejects — indistinguishable from a genuine false at the call site,
// which is exactly the mislabelling that WAS defect D2. So we detect dispatch
// positively: the stub leaf's vptr points at a shadow copy of its class
// vtable whose evaluate slots forward through recording thunks. The two head
// slots (offset-to-top, typeinfo) are copied too, so vptr[-1]/[-2] reads
// (RTTI / read_rtti_name) still resolve to the real class.
//
// The thunks are signature-agnostic: six integer register args forwarded
// untouched, both integer return registers (RAX:RDX) forwarded via a 16-byte
// POD return. Single-slot query serviced only by the game thread → plain
// statics are safe.

struct VfuncRet2 { uint64_t rax; uint64_t rdx; };
using fn_vfunc_passthru = VfuncRet2 (*)(uint64_t, uint64_t, uint64_t,
                                        uint64_t, uint64_t, uint64_t);

static constexpr size_t SHADOW_HEAD_SLOTS  = 2;  // offset-to-top + typeinfo ptr
static constexpr size_t SHADOW_VFUNC_SLOTS =
    ck3::trigger_vtable::TYPE_EVALUATE / sizeof(uintptr_t) + 1;  // 0x00..0xd0
alignas(16) static uintptr_t g_shadow_vtable[SHADOW_HEAD_SLOTS + SHADOW_VFUNC_SLOTS];

static fn_vfunc_passthru g_leaf_real_type_eval = nullptr;  // real vtable[0xd0]
static fn_vfunc_passthru g_leaf_real_eval      = nullptr;  // real vtable[0x28]
static volatile bool     g_leaf_eval_ran       = false;

// vtable[0xd0] — the dispatch FUN_07628790 performs after scope validation
// (re_trigger_evaluator_07628500_deep.md, HIGH).
static VfuncRet2 leaf_type_eval_thunk(uint64_t a, uint64_t b, uint64_t c,
                                      uint64_t d, uint64_t e, uint64_t f) {
    g_leaf_eval_ran = true;
    return g_leaf_real_type_eval(a, b, c, d, e, f);
}

// vtable[0x28] — documented as a second leaf-evaluation entry
// (ck3::trigger_vtable::EVALUATE, from the decision pipeline RE). Thunked
// best-effort so dispatch through either slot is observed.
static VfuncRet2 leaf_eval_thunk(uint64_t a, uint64_t b, uint64_t c,
                                 uint64_t d, uint64_t e, uint64_t f) {
    g_leaf_eval_ran = true;
    return g_leaf_real_eval(a, b, c, d, e, f);
}

// Pick a sentinel interaction whose trigger blocks we can borrow for scope
// construction. declare_war_interaction is guaranteed to exist in the
// shipping binary (re_declare_war_pdxscript_source.md), has a CCharacter
// actor slot, and accepts any target — exactly the shape we need to build a
// character-scoped scope frame.
static constexpr const char* kSentinelInteraction = "declare_war_interaction";

// =============================================================================
// Borrowed-head root (D2 cure, ck-ay9o)
// =============================================================================
//
// The evaluation roots the engine demonstrably accepts against our frame are
// the 0xE0 trigger-block heads EMBEDDED in CCharacterInteractionType:
// process_trigger_query (leg B, P2.2/P2.3) passes interaction_def+offset
// straight to SCRIPT_TRIGGER_EVALUATOR and gets real verdicts 7/7. We
// byte-copy one of those heads and swap its child lists for our stub leaf —
// identical to leg B in everything but the children.

// Interaction trigger-slot offsets — mirror of the file-static
// g_trigger_slots in ai_eval_hook.cpp (keep in sync; canonical rows live in
// ck3_offsets.h interaction_def, e.g. IS_SHOWN_TRIGGER=0xBC0). Order is the
// borrow preference: slot 0 first, same as query_trigger_result reports.
static const struct {
    const char* name;
    uint32_t    offset;
} k_borrow_slots[] = {
    { "is_valid",                       0x0BC0 },
    { "is_shown",                       0x0D80 },
    { "is_valid_showing_failures_only", 0x0F40 },
    { "can_be_sent_despite_rejection",  0x12C0 },
    { "can_send",                       0x1100 },
    { "auto_accept",                    0x0E60 },
    { "on_auto_accept",                 0x1020 },
};
static constexpr size_t NUM_BORROW_SLOTS =
    sizeof(k_borrow_slots) / sizeof(*k_borrow_slots);

// Read the Itanium RTTI class name behind a vtable pointer (vtable[-1] →
// typeinfo, typeinfo+8 → mangled name, digit length-prefix kept so it
// compares against catalogue keys like "11CAndTrigger"). Local mirror of
// query_handlers.cpp read_rtti_name (file-static there).
static bool rtti_class_name(uintptr_t vptr, char* out, size_t out_size) {
    if (!vptr || !is_mapped(vptr) || !out || out_size == 0) return false;
    uintptr_t typeinfo = safe_read<uintptr_t>(vptr - sizeof(uintptr_t));
    if (!typeinfo || !is_mapped(typeinfo)) return false;
    uintptr_t name_ptr = safe_read<uintptr_t>(typeinfo + sizeof(uintptr_t));
    if (!name_ptr || !is_mapped(name_ptr)) return false;
    size_t n = 0;
    while (n + 1 < out_size) {
        uint8_t ch = safe_read<uint8_t>(name_ptr + n);
        if (ch == 0 || ch < 0x20 || ch > 0x7e) break;
        out[n++] = static_cast<char>(ch);
    }
    out[n] = '\0';
    return n > 0;
}

// A borrowable head must look like a real compiled trigger node: fully
// mapped span, a mapped vtable whose evaluate slot (vtable[0xd0] — the one
// FUN_07628790 dispatches) is mapped code, RTTI naming a *Trigger class, and
// a populated child list in the live-proven layout (data at +0x50, packed
// [count:hi32|capacity:lo32] at +0x58). An EMPTY head is refused: it gives
// no evidence the object is a populated compiled block, and grafting into
// junk is exactly the wild-jump class the quality bar forbids.
static bool head_looks_sane(uintptr_t head, char* cls_out, size_t cls_size) {
    if (!head || !is_mapped(head) ||
        !is_mapped(head + ck3::trigger_struct::TOTAL_SIZE - sizeof(uintptr_t))) {
        return false;
    }
    uintptr_t vptr = safe_read<uintptr_t>(head + ck3::trigger_struct::VTABLE);
    if (!vptr || !is_mapped(vptr)) return false;
    uintptr_t eval_fn = safe_read<uintptr_t>(vptr + ck3::trigger_vtable::TYPE_EVALUATE);
    if (!eval_fn || !is_mapped(eval_fn)) return false;
    if (!rtti_class_name(vptr, cls_out, cls_size) ||
        strstr(cls_out, "Trigger") == nullptr) {
        return false;
    }
    uint64_t packed = safe_read<uint64_t>(head + ck3::trigger_struct::CHILDREN_PACKED);
    uint32_t count    = static_cast<uint32_t>(packed >> 32);
    uint32_t capacity = static_cast<uint32_t>(packed & 0xFFFFFFFFu);
    if (count == 0 || count > capacity || capacity > 4096) return false;
    uintptr_t data = safe_read<uintptr_t>(head + ck3::trigger_struct::CHILDREN_DATA);
    if (!data || !is_mapped(data) ||
        !is_mapped(data + (count - 1) * sizeof(uintptr_t))) {
        return false;
    }
    uintptr_t child0 = safe_read<uintptr_t>(data);
    if (!child0 || !is_mapped(child0)) return false;
    return true;
}

static void set_error(const char* tag) {
    snprintf(g_named_trigger_query.error,
             sizeof(g_named_trigger_query.error), "%s", tag);
}

void process_named_trigger_query() {
    int expected = 1;
    if (!g_named_trigger_query.state.compare_exchange_strong(
            expected, 4, std::memory_order_acq_rel)) {
        return;  // not pending
    }

    // Snapshot inputs locally so subsequent IPC-thread writes can't race.
    volatile uint32_t actor_id    = g_named_trigger_query.character_id;
    volatile uint32_t target_id   = g_named_trigger_query.target_id;
    if (target_id == 0) target_id = actor_id;
    const std::string trigger_name = g_named_trigger_query.trigger_name;

    g_named_trigger_query.resolved        = false;
    g_named_trigger_query.evaluated       = false;
    g_named_trigger_query.result          = false;
    g_named_trigger_query.error[0]        = '\0';
    g_named_trigger_query.resolved_class[0] = '\0';
    g_named_trigger_query.root_source[0]  = '\0';
    g_named_trigger_query.root_class[0]   = '\0';
    g_named_trigger_query.evaluator_rva   = 0;
    g_named_trigger_query.vtable_vfunc_rva = 0;
    g_named_trigger_query.confidence      = '?';

    // --- 1. Catalogue lookup ---
    const TriggerCatalogueEntry* entry = lookup_trigger_entry(trigger_name.c_str());
    if (!entry) {
        set_error("trigger_not_in_catalogue");
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }
    snprintf(g_named_trigger_query.resolved_class,
             sizeof(g_named_trigger_query.resolved_class),
             "%s", entry->class_name.c_str());
    g_named_trigger_query.evaluator_rva    = entry->evaluator_rva;
    g_named_trigger_query.vtable_vfunc_rva = entry->vtable_vfunc_rva;
    g_named_trigger_query.confidence       = entry->confidence;
    g_named_trigger_query.resolved         = true;

    if (!entry->evaluator_in_text) {
        // We could still attempt the call, but a MEDIUM/LOW row means the
        // evaluator resolves outside .text (libpops_api thunk or pure-virtual).
        // Bail out rather than risk a wild jump.
        set_error("evaluator_outside_text_skipped");
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }

    uintptr_t vtable_runtime = g_base_addr + entry->vtable_vfunc_rva;
    if (!is_mapped(vtable_runtime)) {
        set_error("vtable_not_mapped");
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }

    // --- 1b. Synthetic-root vtable (round-3 FALLBACK only) ---
    // The cure path borrows a real head at step 4; this catalogue row is only
    // needed when no head passes the sanity gate, so a miss here is NOT fatal.
    // `volatile` pointer: read again after the sigsetjmp regions below
    // (same -Wclobbered discipline as `character` / `interaction`).
    const TriggerCatalogueEntry* volatile root_entry =
        lookup_trigger_entry(kAndRootClass);

    // --- 1c. Shadow copy of the leaf vtable, evaluate slots thunked ---
    uintptr_t vt_head = vtable_runtime - SHADOW_HEAD_SLOTS * sizeof(uintptr_t);
    if (!is_mapped(vt_head) ||
        !is_mapped(vtable_runtime + ck3::trigger_vtable::TYPE_EVALUATE)) {
        set_error("leaf_vtable_span_not_mapped");
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }
    memcpy(g_shadow_vtable, reinterpret_cast<const void*>(vt_head),
           sizeof(g_shadow_vtable));

    constexpr size_t kSlotTypeEval =
        SHADOW_HEAD_SLOTS + ck3::trigger_vtable::TYPE_EVALUATE / sizeof(uintptr_t);
    constexpr size_t kSlotEval =
        SHADOW_HEAD_SLOTS + ck3::trigger_vtable::EVALUATE / sizeof(uintptr_t);

    g_leaf_real_type_eval =
        reinterpret_cast<fn_vfunc_passthru>(g_shadow_vtable[kSlotTypeEval]);
    if (!g_leaf_real_type_eval ||
        !is_mapped(reinterpret_cast<uintptr_t>(g_leaf_real_type_eval))) {
        set_error("leaf_evaluate_vfunc_not_mapped");
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }
    if (entry->evaluator_rva != 0 &&
        reinterpret_cast<uintptr_t>(g_leaf_real_type_eval) !=
            g_base_addr + entry->evaluator_rva) {
        // Informational only — the live vtable is authoritative.
        ADLOG("NAMED_TRIGGER: vtable[0xd0]=0x%lx disagrees with catalogue "
              "evaluator 0x%lx (using vtable)\n",
              reinterpret_cast<uintptr_t>(g_leaf_real_type_eval) - g_base_addr,
              (unsigned long)entry->evaluator_rva);
    }
    g_shadow_vtable[kSlotTypeEval] =
        reinterpret_cast<uintptr_t>(&leaf_type_eval_thunk);

    g_leaf_real_eval = reinterpret_cast<fn_vfunc_passthru>(g_shadow_vtable[kSlotEval]);
    if (g_leaf_real_eval &&
        is_mapped(reinterpret_cast<uintptr_t>(g_leaf_real_eval))) {
        g_shadow_vtable[kSlotEval] = reinterpret_cast<uintptr_t>(&leaf_eval_thunk);
    }

    // --- 2. Character + sentinel interaction lookup ---
    // `volatile` to silence gcc's -Wclobbered: locals used after a
    // sigsetjmp/longjmp must be volatile so the compiler doesn't keep them
    // in registers across the jump.
    void* volatile character   = lookup_character(actor_id);
    if (!character) {
        set_error("actor_not_found");
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }
    void* volatile interaction = lookup_interaction(kSentinelInteraction);
    if (!interaction) {
        set_error("sentinel_interaction_not_found");
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }

    // --- 3. TLS scope construction (mirror process_trigger_query) ---
    uintptr_t fs_base = 0;
    syscall(SYS_arch_prctl, ARCH_GET_FS, &fs_base);
    if (fs_base == 0) {
        set_error("tls_fs_base_zero");
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }
    uint8_t* guard_byte = reinterpret_cast<uint8_t*>(fs_base - kTlsGuardOffsetDefault);
    void*    tls_ctx    = reinterpret_cast<void*>(fs_base - kTlsContextOffsetDefault);

    auto tls_construct  = reinterpret_cast<fn_tls_construct>(
        g_base_addr + ck3::funcs::INTERACTION_TLS_CONSTRUCT);
    auto ctx_init       = reinterpret_cast<fn_ctx_init>(
        g_base_addr + ck3::funcs::INTERACTION_CTX_INIT);
    auto ctx_reset      = reinterpret_cast<fn_ctx_reset>(
        g_base_addr + ck3::funcs::INTERACTION_CTX_RESET);
    auto ctx_set_target = reinterpret_cast<fn_ctx_set_target>(
        g_base_addr + ck3::funcs::INTERACTION_CTX_SET_TARGET);

    if (!is_mapped(reinterpret_cast<uintptr_t>(tls_construct)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(ctx_init)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(ctx_reset)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(ctx_set_target))) {
        set_error("pipeline_funcs_not_mapped");
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }

    if (*guard_byte == 0) {
        watchdog_arm(3);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false;
            watchdog_disarm();
            set_error("tls_construct_crash");
            g_named_trigger_query.state.store(3, std::memory_order_release);
            return;
        }
        try {
            tls_construct(tls_ctx);
        } catch (...) {
            g_in_protected_region = false;
            watchdog_disarm();
            set_error("tls_construct_exception");
            g_named_trigger_query.state.store(3, std::memory_order_release);
            return;
        }
        g_in_protected_region = false;
        watchdog_disarm();
        *guard_byte = 1;
    }

    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        const char* sig_name = (sig == 1) ? "SIGSEGV" :
                              (sig == 2) ? "SIGALRM" : "UNKNOWN";
        char buf[64];
        snprintf(buf, sizeof(buf), "scope_setup_%s", sig_name);
        set_error(buf);
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }
    try {
        ctx_init(reinterpret_cast<uintptr_t*>(tls_ctx), interaction, character);
        ctx_reset(reinterpret_cast<uintptr_t*>(tls_ctx));
        ctx_set_target(reinterpret_cast<uintptr_t*>(tls_ctx), target_id, 1);
    } catch (...) {
        g_in_protected_region = false;
        watchdog_disarm();
        set_error("scope_setup_exception");
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    uintptr_t ctx_addr  = reinterpret_cast<uintptr_t>(tls_ctx);
    uintptr_t scope_ctx = ctx_addr + ck3::interaction_ctx::SCOPE_LIST;

    // --- 4. Stub leaf grafted under a BORROWED real trigger-block head ---
    //
    // A bare leaf at the evaluation root is rejected by FUN_07628790's
    // scope-type pre-check (P2), and round 3 proved a synthetic CAndTrigger
    // root built from the class vtable alone is rejected the same way one
    // level deeper: the zeroed instance carries none of the compiled
    // scope-typing state (+0x10/+0x18/+0x28) the compound machinery uses to
    // type the sub-scope for its children (verify-round3 §4.1 postmortem).
    // So byte-copy a head the engine PROVABLY accepts against this frame —
    // one of the sentinel interaction's embedded 0xE0 blocks, the exact
    // addresses leg B (process_trigger_query) hands to the evaluator — and
    // replace only its child lists with our single stub leaf.
    alignas(16) uint8_t leaf[TRIGGER_NODE_STUB_SIZE];
    memset(leaf, 0, sizeof(leaf));
    *reinterpret_cast<uintptr_t*>(leaf) =
        reinterpret_cast<uintptr_t>(&g_shadow_vtable[SHADOW_HEAD_SLOTS]);

    alignas(16) uint8_t root_node[AND_ROOT_NODE_SIZE];
    memset(root_node, 0, sizeof(root_node));

    bool root_borrowed = false;
    {
        // The interaction definition pointer the ctx recorded at CTX_INIT —
        // same read process_trigger_query performs. Fall back to the
        // lookup_interaction pointer if the ctx read looks wrong; the
        // per-head sanity gate protects against either being unexpected.
        uintptr_t def_candidates[2] = {
            safe_read<uintptr_t>(ctx_addr + ck3::interaction_ctx::INTERACTION_TYPE_PTR),
            reinterpret_cast<uintptr_t>(interaction),
        };
        for (int d = 0; d < 2 && !root_borrowed; ++d) {
            uintptr_t def = def_candidates[d];
            if (!def || !is_mapped(def)) continue;
            for (size_t s = 0; s < NUM_BORROW_SLOTS; ++s) {
                uintptr_t head = def + k_borrow_slots[s].offset;
                char cls[96];
                if (!head_looks_sane(head, cls, sizeof(cls))) continue;
                memcpy(root_node, reinterpret_cast<const void*>(head),
                       AND_ROOT_NODE_SIZE);
                snprintf(g_named_trigger_query.root_source,
                         sizeof(g_named_trigger_query.root_source),
                         "borrowed:%s", k_borrow_slots[s].name);
                snprintf(g_named_trigger_query.root_class,
                         sizeof(g_named_trigger_query.root_class), "%s", cls);
                root_borrowed = true;
                ADLOG("NAMED_TRIGGER: borrowed head slot=%s def=0x%lx "
                      "head=0x%lx class=%s\n",
                      k_borrow_slots[s].name, def, head, cls);
                break;
            }
        }
    }

    if (!root_borrowed) {
        // FALLBACK: round-3 synthetic CAndTrigger root. Known insufficient
        // on its own (the reply will come back scope_rejected), but it keeps
        // the surface typed and honest instead of failing dark when no head
        // passes the sanity gate.
        if (!root_entry) {
            set_error("no_evaluation_root_available");
            g_named_trigger_query.state.store(3, std::memory_order_release);
            return;
        }
        uintptr_t root_vtable = g_base_addr + root_entry->vtable_vfunc_rva;
        if (!is_mapped(root_vtable)) {
            set_error("and_root_vtable_not_mapped");
            g_named_trigger_query.state.store(3, std::memory_order_release);
            return;
        }
        *reinterpret_cast<uintptr_t*>(root_node + ck3::trigger_struct::VTABLE) =
            root_vtable;
        snprintf(g_named_trigger_query.root_source,
                 sizeof(g_named_trigger_query.root_source), "synthetic_and_root");
        snprintf(g_named_trigger_query.root_class,
                 sizeof(g_named_trigger_query.root_class), "%s", kAndRootClass);
        ADLOG("NAMED_TRIGGER: no borrowable head — synthetic root "
              "vt_rva=0x%lx (expect scope_rejected)\n",
              (unsigned long)root_entry->vtable_vfunc_rva);
    }

    // Graft: single-entry child array parked in the root's own inline (SSO)
    // buffer at +0x70, exactly where the script compiler puts ≤4 children.
    // Layout per ck3::trigger_struct (live-proven by query_trigger_slots'
    // walker): data qword at +0x50, packed [count:upper32|capacity:lower32]
    // at +0x58. NOTE: and_trigger::CHILDREN_CAPACITY=0x54 is NOT written —
    // it would overlap the +0x50 data pointer's upper dword (stale RE row).
    // Optimizer-built nodes carry the same children in BOTH orderings
    // ("from" unique_ptr list at +0x50, "to" raw list at +0x98,
    // re_trigger_evaluator_07628500_deep.md); mirror that so whichever phase
    // the compound evaluator runs (subtype byte +0x20 routes from/to/both)
    // it sees the leaf, and the from/to comparison can never disagree. On
    // the borrowed path everything else — scope-typing state, subtype,
    // allocator vtable, diag/label pointers — is the real head's, verbatim.
    // Evaluation only READS the child arrays (count == capacity, never
    // grows), so the copied allocator is never invoked.
    uintptr_t child_slot =
        reinterpret_cast<uintptr_t>(root_node) + ck3::and_trigger::CHILDREN_INLINE;
    *reinterpret_cast<uintptr_t*>(child_slot) = reinterpret_cast<uintptr_t>(leaf);
    *reinterpret_cast<uintptr_t*>(root_node + ck3::trigger_struct::CHILDREN_DATA) =
        child_slot;
    *reinterpret_cast<uint64_t*>(root_node + ck3::trigger_struct::CHILDREN_PACKED) =
        (uint64_t(1) << 32) | uint64_t(1);   // count=1, capacity=1
    *reinterpret_cast<uintptr_t*>(root_node + ck3::trigger_struct::RAW_PTR_DATA) =
        child_slot;
    *reinterpret_cast<uint64_t*>(root_node + ck3::trigger_struct::RAW_PTR_PACKED) =
        (uint64_t(1) << 32) | uint64_t(1);   // count=1, capacity=1

    ADLOG("NAMED_TRIGGER: root=%p (%s) leaf=%p shadow_vt=%p leaf_d0=0x%lx\n",
          static_cast<void*>(root_node), g_named_trigger_query.root_source,
          static_cast<void*>(leaf),
          static_cast<void*>(&g_shadow_vtable[SHADOW_HEAD_SLOTS]),
          reinterpret_cast<uintptr_t>(g_leaf_real_type_eval) - g_base_addr);

    // --- 5. Resolve evaluator and call ---
    auto trigger_eval = reinterpret_cast<fn_trigger_eval>(
        g_base_addr + ck3::trigger_eval_funcs::SCRIPT_TRIGGER_EVALUATOR);
    if (!is_mapped(reinterpret_cast<uintptr_t>(trigger_eval))) {
        set_error("trigger_evaluator_not_mapped");
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }

    g_leaf_eval_ran = false;

    watchdog_arm(3);
    g_in_protected_region = true;
    sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        // Crash during evaluation — record the signal but keep `resolved`
        // true so the caller can see WHICH class crashed.
        const char* sig_name = (sig == 1) ? "SIGSEGV" :
                              (sig == 2) ? "SIGALRM" : "UNKNOWN";
        char buf[64];
        snprintf(buf, sizeof(buf), "evaluation_%s", sig_name);
        set_error(buf);
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }
    bool eval_result = false;
    try {
        eval_result = trigger_eval(reinterpret_cast<uintptr_t>(root_node),
                                   scope_ctx);
    } catch (...) {
        g_in_protected_region = false;
        watchdog_disarm();
        set_error("evaluation_exception");
        g_named_trigger_query.state.store(3, std::memory_order_release);
        return;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    // --- 6. Report — a verdict ONLY if the leaf's evaluate vfunc ran ---
    if (g_leaf_eval_ran) {
        g_named_trigger_query.evaluated = true;
        g_named_trigger_query.result    = eval_result;
        g_named_trigger_query.error[0]  = '\0';
        g_named_trigger_query.state.store(2, std::memory_order_release);
    } else if (!eval_result) {
        // The engine short-circuited before dispatch (FUN_07628790 scope
        // validation) — the exact D2 mechanism. Loud typed error, never a
        // fake `false`.
        set_error("scope_rejected");
        g_named_trigger_query.state.store(3, std::memory_order_release);
    } else {
        // Tree passed but neither thunk fired: dispatch went through a path
        // we don't instrument. Refuse to guess a verdict.
        set_error("leaf_dispatch_undetected");
        g_named_trigger_query.state.store(3, std::memory_order_release);
    }

    ADLOG("NAMED_TRIGGER: %s actor=%u target=%u -> tree=%s leaf_ran=%d\n",
          g_named_trigger_query.resolved_class,
          actor_id & 0xFFFFFF, target_id & 0xFFFFFF,
          eval_result ? "true" : "false",
          g_leaf_eval_ran ? 1 : 0);
}

// =============================================================================
// IPC-thread entry point
// =============================================================================

static void emit_response(std::string* out, const char* trigger_name) {
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("evaluate_named_trigger");
    r.key("trigger"); r.value_string(trigger_name);
    r.key("character_id"); r.value_uint(g_named_trigger_query.character_id & 0xFFFFFF);
    r.key("target_id");    r.value_uint(g_named_trigger_query.target_id    & 0xFFFFFF);
    r.key("resolved");     r.value_bool(g_named_trigger_query.resolved);
    if (g_named_trigger_query.resolved) {
        r.key("class"); r.value_string(g_named_trigger_query.resolved_class);
        char hex[32];
        snprintf(hex, sizeof(hex), "0x%lx",
                 (unsigned long)g_named_trigger_query.evaluator_rva);
        r.key("evaluator_rva"); r.value_string(hex);
        snprintf(hex, sizeof(hex), "0x%lx",
                 (unsigned long)g_named_trigger_query.vtable_vfunc_rva);
        r.key("vtable_rva"); r.value_string(hex);
        char conf[2] = {g_named_trigger_query.confidence, '\0'};
        r.key("confidence"); r.value_string(conf);
    }
    // Which evaluation root serviced the call — "borrowed:<slot>" is the
    // cure path; "synthetic_and_root" is the round-3 fallback (expected to
    // scope-reject). Lets live probes and the suite tell the two apart.
    if (g_named_trigger_query.root_source[0]) {
        r.key("root_source"); r.value_string(g_named_trigger_query.root_source);
    }
    if (g_named_trigger_query.root_class[0]) {
        r.key("root_class"); r.value_string(g_named_trigger_query.root_class);
    }
    r.key("evaluated"); r.value_bool(g_named_trigger_query.evaluated);
    if (g_named_trigger_query.evaluated) {
        r.key("result"); r.value_bool(g_named_trigger_query.result);
        r.key("status"); r.value_string("ok");
    } else if (g_named_trigger_query.error[0]) {
        r.key("error"); r.value_string(g_named_trigger_query.error);
        if (strcmp(g_named_trigger_query.error, "scope_rejected") == 0) {
            r.key("hint"); r.value_string(
                "Engine rejected the trigger's scope pre-dispatch; the leaf "
                "evaluator never ran. This is NOT a false verdict.");
        }
        r.key("status"); r.value_string("error");
    } else {
        r.key("status"); r.value_string("pending");
    }
    r.key("deferred"); r.value_bool(true);
    r.end_object();
    if (out) *out = r.take();
}

bool do_evaluate_named_trigger(const char* params_json, std::string* out) {
    ensure_loaded();

    const char* raw_name = json_get_string(params_json, "trigger");
    if (!raw_name || !raw_name[0]) raw_name = json_get_string(params_json, "trigger_name");
    if (!raw_name || !raw_name[0]) {
        if (out) *out = R"({"error":"missing_trigger_name","hint":"Pass 'trigger' or 'trigger_name'"})";
        return false;
    }
    std::string name_str(raw_name);

    int character_raw = json_get_int(params_json, "character");
    if (character_raw <= 0) character_raw = json_get_int(params_json, "character_id");
    if (character_raw <= 0) character_raw = json_get_int(params_json, "actor_id");
    if (character_raw <= 0) {
        uint32_t ctrl = g_controlled_primary.load(std::memory_order_relaxed);
        if (ctrl == 0) ctrl = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
        character_raw = static_cast<int>(ctrl & 0xFFFFFF);
    }
    if (character_raw <= 0) {
        if (out) *out = R"({"error":"no_character_available"})";
        return false;
    }

    int target_raw = json_get_int(params_json, "target_id");
    if (target_raw < 0) target_raw = 0;

    // Reject if another query is in flight (single-slot design — same as
    // g_trigger_query). Caller can retry once the in-flight one drains.
    int current_state = g_named_trigger_query.state.load(std::memory_order_acquire);
    if (current_state == 1 || current_state == 4) {
        if (out) *out = R"({"error":"named_trigger_query_busy","hint":"Another evaluate_named_trigger is pending. Retry shortly."})";
        return true;
    }
    if (current_state == 2 || current_state == 3) {
        g_named_trigger_query.state.store(0, std::memory_order_release);
    }

    snprintf(g_named_trigger_query.trigger_name,
             sizeof(g_named_trigger_query.trigger_name),
             "%s", name_str.c_str());
    g_named_trigger_query.character_id = static_cast<uint32_t>(character_raw);
    g_named_trigger_query.target_id    = static_cast<uint32_t>(target_raw);
    g_named_trigger_query.resolved        = false;
    g_named_trigger_query.evaluated       = false;
    g_named_trigger_query.result          = false;
    g_named_trigger_query.error[0]        = '\0';
    g_named_trigger_query.resolved_class[0] = '\0';
    g_named_trigger_query.root_source[0]  = '\0';
    g_named_trigger_query.root_class[0]   = '\0';
    g_named_trigger_query.evaluator_rva   = 0;
    g_named_trigger_query.vtable_vfunc_rva = 0;
    g_named_trigger_query.confidence      = '?';

    g_named_trigger_query.state.store(1, std::memory_order_release);

    ADLOG("NAMED_TRIGGER: submitted '%s' actor=%d target=%d (catalogue %zu rows)\n",
          name_str.c_str(), character_raw, target_raw,
          g_cat_by_class.size());

    // Poll for completion (mirrors process_trigger_query timing — 60 * 50ms = ~3s).
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int st = g_named_trigger_query.state.load(std::memory_order_acquire);
        if (st == 2 || st == 3) {
            emit_response(out, name_str.c_str());
            g_named_trigger_query.state.store(0, std::memory_order_release);
            return true;
        }
    }

    // Timeout — return what we have without resetting state (lets a retry
    // re-claim the same in-flight query if it lands shortly).
    if (out) {
        ResponseBuilder r;
        r.begin_object();
        r.key("action"); r.value_string("evaluate_named_trigger");
        r.key("trigger"); r.value_string(name_str.c_str());
        r.key("character_id"); r.value_uint(static_cast<uint32_t>(character_raw) & 0xFFFFFF);
        r.key("error"); r.value_string("timeout_waiting_for_game_thread");
        r.key("hint"); r.value_string("Game must be unpaused for at least one tick. "
                                       "The query runs during drain_and_execute_intents.");
        r.key("status"); r.value_string("timeout");
        r.key("deferred"); r.value_bool(true);
        r.end_object();
        *out = r.take();
    }
    return true;
}

} // namespace bridge
