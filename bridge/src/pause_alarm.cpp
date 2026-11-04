// pause_alarm.cpp — see pause_alarm.h (L20-A).
#include "pause_alarm.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "game_date.h"

namespace bridge {
namespace pause_alarm {

namespace {
std::mutex g_mu;
Snapshot g_s;
std::atomic<bool> g_armed{false};
}  // namespace

Arm arm(int32_t target_day, int32_t now_day) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (target_day <= now_day) return Arm::NotInFuture;   // the previous alarm, if any, stands
    g_s.state = State::Armed;
    g_s.target_day = target_day;
    g_s.armed_at_day = now_day;
    g_s.paused_at_day = 0;
    g_s.cleared_by = "";
    g_s.arms++;
    g_armed.store(true, std::memory_order_release);
    return Arm::Armed;
}

bool armed() { return g_armed.load(std::memory_order_acquire); }

bool take_due(int32_t now_day) {
    if (!armed()) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_s.state != State::Armed || now_day < g_s.target_day) return false;
    g_s.state = State::Fired;
    g_s.paused_at_day = now_day;
    g_s.fires++;
    g_armed.store(false, std::memory_order_release);
    return true;
}

void clear(const char* why) {
    if (!armed()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_s.state != State::Armed) return;
    g_s.state = State::Cleared;
    g_s.cleared_by = why ? why : "";
    g_armed.store(false, std::memory_order_release);
}

Snapshot snapshot() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_s;
}

const char* state_name(State s) {
    switch (s) {
        case State::None:    return "none";
        case State::Armed:   return "armed";
        case State::Fired:   return "fired";
        case State::Cleared: return "cleared";
    }
    return "none";
}

bool parse_date(const char* s, int32_t* total_days) {
    if (!s || !*s) return false;
    int part[3] = {0, 0, 0};
    const char* p = s;
    for (int i = 0; i < 3; ++i) {
        if (*p < '0' || *p > '9') return false;
        char* end = nullptr;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > 100000) return false;
        part[i] = static_cast<int>(v);
        p = end;
        if (i < 2) {
            if (*p != '.') return false;
            ++p;
        }
    }
    if (*p != '\0') return false;
    const int y = part[0], m = part[1], d = part[2];
    if (m < 1 || m > 12) return false;
    if (d < 1 || d > ck3::game_date::days_in_month(m)) return false;
    if (total_days) *total_days = ck3::game_date::ymd_to_total_days(y, m, d);
    return true;
}

void reset_for_test() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_s = Snapshot{};
    g_armed.store(false, std::memory_order_release);
}

}  // namespace pause_alarm
}  // namespace bridge
