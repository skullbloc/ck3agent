// pulse_restore_probe.cpp — ck-cyhr. The FIRST coverage deferred_drain.cpp has had.
//
// Driven by scripts/acceptance_pulse_restore.py. This TU links the REAL
// bridge/src/deferred_drain.cpp against a FAKE engine, so what is exercised is
// the shipping coordinator and not a model of it. The same TU is compiled a
// second time against the PRE-FIX deferred_drain.cpp (staged from a pinned blob
// sha) to produce the negative control — the control is therefore a BINARY that
// does what the bridge used to do, not a paragraph asserting it.
//
// THE DEFECT (ck-cyhr, P0, blocked the whole E0 set): PulseRestore captured the
// game speed at pulse open and re-asserted it at pulse close if it had changed.
// The pre-registered E0 speed pin (display 2) landed INSIDE a window opened at
// display 3, so on close the guard called it drift and wrote 3 back. The meter
// lane pauses to read and unpauses after, continuously, so that window opened on
// every sample. All eight E0 episodes would have sealed `speed_mismatch`.
//
// THE FIX: the pulse restores THE PAUSE FLAG ONLY. It never writes speed. The
// speed is observed at open and close and the divergence LOGGED.
//
// WHAT EACH SCENARIO IS FOR — and note that a read-back at the instant of the
// write is exactly the check that already passed on the broken tree, so no
// scenario here judges on one:
//
//   pin_straddled       the log's own shape, minimal. Window opens at display 3,
//                       another client pins display 2 inside it, window closes.
//                       Does the pin SURVIVE the close?
//   pin_before_window   the pin precedes the window, so nothing drifts. NOT
//                       discriminating (it passes pre-fix too) and declared as
//                       such; it is here because the ordinary path has to keep
//                       working.
//   meter_lane          the E0 shape end to end: lane cycle, then a cycle whose
//                       window straddles the pin, then two more lane cycles, then
//                       read the speed. Persistence across later windows, which
//                       is the thing a read-back cannot tell you.
//   throw_path          the pause half is load-bearing (the I15 hang). Service
//                       throws mid-window: is the game re-paused anyway, and is
//                       the pin still intact?
//   drain_unavailable   the sim never ticks: re-paused, typed status, closure not
//                       run, speed untouched.
//   hot_path            already unpaused: the coordinator must touch nothing.
//   nested_pulse        two callers share one window via g_pulse_depth; only the
//                       last out re-pauses, and neither writes speed.
//
// Emits one JSON object on stdout. The python driver judges; this probe only
// observes, so both trees can be asked the identical question.

#include "bridge.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════
// THE FAKE ENGINE
// ═══════════════════════════════════════════════════════════════════════════
// Every symbol deferred_drain.cpp needs from the engine, and nothing else. The
// real ones live in event_handler.cpp and touch CK3 memory; these keep the same
// CONTRACT: game_get_speed() returns the internal 0-indexed speed, and
// game_set_speed() takes the 1-indexed display speed (event_handler.cpp:272).
// Pause is a SEPARATE FLAG and not speed 0 — which is the whole reason the pulse
// cannot itself disturb the speed.

namespace {

struct TraceEvent {
    std::string phase;   // who was acting: coordinator / setter / lane / service
    std::string op;      // pause | unpause | set_speed | get_speed | is_paused
    int arg = -1;        // display speed for set_speed, else -1
};

std::mutex              g_fake_mx;
std::vector<TraceEvent> g_trace;
std::atomic<bool>       g_paused{true};
std::atomic<int>        g_internal_speed{2};    // display 3
std::atomic<bool>       g_ticks_enabled{true};  // false => the sim never ticks
std::atomic<bool>       g_ticker_stop{false};

// The acting phase. The coordinator never sets this; every OTHER actor does,
// around its own calls, so the trace can say who issued each write. Without it
// "a set_speed happened" cannot be attributed, and attribution is the question.
thread_local const char* t_phase = "coordinator";

struct PhaseScope {
    const char* prev;
    explicit PhaseScope(const char* p) : prev(t_phase) { t_phase = p; }
    ~PhaseScope() { t_phase = prev; }
};

void record(const char* op, int arg = -1) {
    std::lock_guard<std::mutex> lk(g_fake_mx);
    g_trace.push_back(TraceEvent{t_phase, op, arg});
}

} // namespace

namespace bridge {

FILE*                 g_logfile = nullptr;
std::atomic<uint64_t> g_tick_id{0};

bool is_paused() {
    record("is_paused");
    return g_paused.load(std::memory_order_acquire);
}

int game_get_speed() {
    record("get_speed");
    return g_internal_speed.load(std::memory_order_acquire);
}

void game_set_speed(int display_speed) {
    if (display_speed < 1) display_speed = 1;
    if (display_speed > 5) display_speed = 5;
    record("set_speed", display_speed);
    g_internal_speed.store(display_speed - 1, std::memory_order_release);
}

void game_pause() {
    record("pause");
    g_paused.store(true, std::memory_order_release);
}

void game_unpause() {
    record("unpause");
    g_paused.store(false, std::memory_order_release);
}

} // namespace bridge

// ═══════════════════════════════════════════════════════════════════════════
// The ticker — the sim advancing while unpaused
// ═══════════════════════════════════════════════════════════════════════════
// await_tick_advance() polls g_tick_id, so a pulse only "works" if something
// ticks. That is the real precondition, so the fake honours it: g_tick_id moves
// only while unpaused AND ticking is enabled. drain_unavailable is produced by
// disabling ticking, exactly as a loading screen does.

namespace {

std::thread g_ticker;

void ticker_loop() {
    while (!g_ticker_stop.load(std::memory_order_acquire)) {
        if (!g_paused.load(std::memory_order_acquire) &&
            g_ticks_enabled.load(std::memory_order_acquire)) {
            bridge::g_tick_id.fetch_add(1, std::memory_order_acq_rel);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// A speed write from ANOTHER CLIENT, on ANOTHER THREAD — which is what the E0
// pin was (its own IPC session, closed straight after). Deterministic: the
// caller blocks until the write has landed, so the straddle is a fact of the
// run and not a race the test hopes for.
void pin_from_other_thread(int display_speed) {
    std::thread t([display_speed] {
        PhaseScope ps("setter");
        bridge::game_set_speed(display_speed);
    });
    t.join();
}

// ── JSON out ────────────────────────────────────────────────────────────────

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (static_cast<unsigned char>(c) < 0x20) {
            char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b;
        }
        else o += c;
    }
    return o;
}

std::string read_all(FILE* f) {
    if (!f) return "";
    fflush(f);
    long end = ftell(f);
    if (end < 0) return "";
    rewind(f);
    std::string out(static_cast<size_t>(end), '\0');
    size_t n = fread(&out[0], 1, static_cast<size_t>(end), f);
    out.resize(n);
    return out;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// The scenarios
// ═══════════════════════════════════════════════════════════════════════════

namespace {

struct Result {
    std::string scenario;
    std::string status = "ok";       // ok | drain_unavailable | busy_deferred | threw
    bool        service_ran = false;
    std::string service_out;         // whatever the coordinator wrote into `out`
    std::string threw_what;
};

const char* status_name(bridge::DeferredDrainStatus s) {
    switch (s) {
        case bridge::DeferredDrainStatus::Ok:               return "ok";
        case bridge::DeferredDrainStatus::BusyDeferred:     return "busy_deferred";
        case bridge::DeferredDrainStatus::DrainUnavailable: return "drain_unavailable";
        default:                                            return "unknown";
    }
}

// A plain lane-style deferred call with no straddle.
bridge::DeferredDrainStatus call(Result& r,
                                const std::function<void(std::string*)>& body) {
    std::string out;
    auto st = bridge::run_deferred_pause_transparent(
        [&](std::string* o) {
            r.service_ran = true;
            PhaseScope ps("service");
            body(o);
            if (o && o->empty()) *o = "{\"status\":\"ok\"}";
        }, &out);
    r.service_out = out;
    return st;
}

void scenario_pin_straddled(Result& r) {
    // The world as the log found it: paused by the lane, running at display 3.
    g_paused.store(true);
    g_internal_speed.store(2);           // display 3
    // The window opens at display 3; the pin lands INSIDE it; the window closes.
    r.status = status_name(call(r, [](std::string*) { pin_from_other_thread(2); }));
}

void scenario_pin_before_window(Result& r) {
    // NOT DISCRIMINATING, and declared so: nothing drifts, so the pre-fix branch
    // at :144 is never taken and both trees hold the pin. Here because the
    // ordinary path has to keep working after the fix.
    g_paused.store(true);
    g_internal_speed.store(1);           // display 2 — already pinned
    r.status = status_name(call(r, [](std::string*) {}));
}

void scenario_meter_lane(Result& r) {
    // The E0 shape, end to end. The lane pauses to read and unpauses after,
    // continuously; the pin arrives from its own short-lived session during one
    // of those windows. Then two more lane cycles run, and only THEN is the
    // speed read — a read-back at the instant of the write is the check that
    // already passed on the broken tree.
    g_paused.store(false);
    g_internal_speed.store(2);           // display 3, the world's resting speed

    for (int cycle = 0; cycle < 4; ++cycle) {
        { PhaseScope ps("lane"); bridge::game_pause(); }
        if (cycle == 1) {
            r.status = status_name(
                call(r, [](std::string*) { pin_from_other_thread(2); }));
        } else {
            (void)status_name(call(r, [](std::string*) {}));
        }
        { PhaseScope ps("lane"); bridge::game_unpause(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    { PhaseScope ps("lane"); bridge::game_pause(); }
}

void scenario_throw_path(Result& r) {
    // The pause half is load-bearing: the RAII guard exists because unclassified
    // handlers that fell through to g_msg_queue hung to the client's own timeout
    // (the I15 hang), and because of the cold-unpause bug. A throw out of the
    // service closure must still leave the game paused.
    g_paused.store(true);
    g_internal_speed.store(2);           // display 3
    std::string out;
    try {
        bridge::run_deferred_pause_transparent(
            [&](std::string*) {
                r.service_ran = true;
                PhaseScope ps("service");
                pin_from_other_thread(2);          // the pin, inside the window
                throw std::runtime_error("service blew up mid-window");
            }, &out);
        r.status = "no_throw_escaped";
    } catch (const std::exception& e) {
        r.status = "threw";
        r.threw_what = e.what();
    }
    r.service_out = out;
}

void scenario_drain_unavailable(Result& r) {
    // Loading screen / between sessions: the unpause silently no-ops and the sim
    // never ticks. Typed status, closure NOT run, re-paused, speed untouched.
    g_paused.store(true);
    g_internal_speed.store(2);           // display 3
    g_ticks_enabled.store(false);
    r.status = status_name(call(r, [](std::string*) {}));
    g_ticks_enabled.store(true);
}

void scenario_hot_path(Result& r) {
    // Already unpaused — the common agent-loop hot path. The coordinator must not
    // touch the clock at all.
    g_paused.store(false);
    g_internal_speed.store(1);           // display 2
    r.status = status_name(call(r, [](std::string*) {}));
}

void scenario_nested_pulse(Result& r) {
    // Two callers riding ONE window via g_pulse_depth: only the last out
    // re-pauses. The inner call runs while already unpaused, so it takes the hot
    // path; what this proves is that the shared window still ends paused and
    // still writes no speed, with the pin landing inside the OUTER window.
    g_paused.store(true);
    g_internal_speed.store(2);           // display 3
    r.status = status_name(call(r, [&](std::string*) {
        pin_from_other_thread(2);
        Result inner;
        (void)call(inner, [](std::string*) {});
    }));
}

struct Entry { const char* name; void (*fn)(Result&); };

const Entry kScenarios[] = {
    {"pin_straddled",     scenario_pin_straddled},
    {"pin_before_window", scenario_pin_before_window},
    {"meter_lane",        scenario_meter_lane},
    {"throw_path",        scenario_throw_path},
    {"drain_unavailable", scenario_drain_unavailable},
    {"hot_path",          scenario_hot_path},
    {"nested_pulse",      scenario_nested_pulse},
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: pulse_restore_probe <scenario>\nscenarios:");
        for (const auto& e : kScenarios) fprintf(stderr, " %s", e.name);
        fprintf(stderr, "\n");
        return 2;
    }
    const Entry* chosen = nullptr;
    for (const auto& e : kScenarios) if (strcmp(e.name, argv[1]) == 0) chosen = &e;
    if (!chosen) { fprintf(stderr, "unknown scenario: %s\n", argv[1]); return 2; }

    // The bridge log, captured. The fix's OBSERVATION lives here and nowhere
    // else, so the driver has to be able to read it.
    bridge::g_logfile = tmpfile();

    g_ticker = std::thread(ticker_loop);

    Result r;
    r.scenario = argv[1];
    chosen->fn(r);

    g_ticker_stop.store(true, std::memory_order_release);
    g_ticker.join();

    const std::string log = read_all(bridge::g_logfile);

    std::vector<TraceEvent> trace;
    { std::lock_guard<std::mutex> lk(g_fake_mx); trace = g_trace; }

    std::string coord_writes, other_writes, trace_json;
    bool first_c = true, first_o = true, first_t = true;
    for (const auto& ev : trace) {
        if (!first_t) trace_json += ",";
        first_t = false;
        trace_json += "{\"phase\":\"" + esc(ev.phase) + "\",\"op\":\"" + esc(ev.op) + "\"";
        if (ev.arg >= 0) trace_json += ",\"display\":" + std::to_string(ev.arg);
        trace_json += "}";
        if (ev.op != "set_speed") continue;
        std::string& bucket =
            (ev.phase == "coordinator") ? coord_writes : other_writes;
        bool& first = (ev.phase == "coordinator") ? first_c : first_o;
        if (!first) bucket += ",";
        first = false;
        bucket += std::to_string(ev.arg);
    }

    printf("{"
           "\"scenario\":\"%s\","
           "\"status\":\"%s\","
           "\"service_ran\":%s,"
           "\"service_out\":\"%s\","
           "\"threw_what\":\"%s\","
           "\"final_display_speed\":%d,"
           "\"final_paused\":%s,"
           "\"coordinator_speed_writes\":[%s],"
           "\"other_speed_writes\":[%s],"
           "\"trace\":[%s],"
           "\"log\":\"%s\""
           "}\n",
           esc(r.scenario).c_str(),
           esc(r.status).c_str(),
           r.service_ran ? "true" : "false",
           esc(r.service_out).c_str(),
           esc(r.threw_what).c_str(),
           g_internal_speed.load() + 1,
           g_paused.load() ? "true" : "false",
           coord_writes.c_str(),
           other_writes.c_str(),
           trace_json.c_str(),
           esc(log).c_str());
    return 0;
}
