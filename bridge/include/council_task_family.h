// council_task_family.h — which task type a named council task is ON A GIVEN SEAT
// (L20-A, smoke17 row B5).
//
// The engine defines a task once and lets other seats CLONE it: game/common/
// council_tasks/01_ministry_tasks.txt declares
//     task_fabricate_claim_minister_of_rites = { position = minister_justice
//                                                clone = task_fabricate_claim }
// and the chaplain's task_fabricate_claim is hidden for a ruler with ministries
// (00_court_chaplain_tasks.txt is_shown: has_council_position = minister_justice).
// In memory each clone is its own CCouncilTaskType whose +0x14d8 points at the
// template (ck3::council_task_type::CLONE_TEMPLATE_PTR, HIGH: decompile of
// FUN_05cb9ed0 and the 867 dump, 5 of 72 types set, each at its data template).
//
// The handler found the type by the asked name and compared the caller's `seat`
// with that type's one position, so B5 (Cui 13191, seat minister_justice,
// task_fabricate_claim) was refused seat_cannot_take_task / task_seat
// councillor_court_chaplain before the engine was asked. The rule now:
//   family(T) = root + every type whose template is root, root = T's template
//               when T is a clone, else T;
//   no seat (or a numeric one)   -> T itself (unchanged);
//   a seat                       -> the family member on that seat, or none
//                                   (the refusal lists the family's seats).
// PURE: rows are read by the caller. Host-tested by bridge/test/
// test_council_task_family.cpp on rows from the 867 dump.
#ifndef CK3_BRIDGE_COUNCIL_TASK_FAMILY_H
#define CK3_BRIDGE_COUNCIL_TASK_FAMILY_H

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace bridge {
namespace council_task_family {

struct Row {
    uintptr_t obj = 0;          // the CCouncilTaskType
    uintptr_t clone_of = 0;     // *(obj + CLONE_TEMPLATE_PTR), 0 = not a clone
    std::string name;           // key, e.g. "task_fabricate_claim"
    std::string seat;           // its position type's key, e.g. "councillor_court_chaplain"
};

// Seat names compare case-blind and without the "councillor_" prefix, so the
// server's "court_chaplain" and the engine's "councillor_court_chaplain" agree.
inline std::string norm_seat(std::string v) {
    for (auto& ch : v) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    const std::string pre = "councillor_";
    if (v.compare(0, pre.size(), pre) == 0) v = v.substr(pre.size());
    return v;
}

struct Pick {
    const Row* asked = nullptr;           // the row named `want` (nullptr: no such task)
    const Row* chosen = nullptr;          // the row to assign (nullptr: refuse)
    std::vector<const Row*> family;       // root first, then its clones in row order
};

inline Pick resolve(const std::vector<Row>& rows, const std::string& want,
                    const std::string& want_seat) {
    Pick p;
    for (const auto& r : rows) if (r.name == want) { p.asked = &r; break; }
    if (!p.asked) return p;
    uintptr_t root = p.asked->clone_of ? p.asked->clone_of : p.asked->obj;
    for (const auto& r : rows) if (r.obj == root) { p.family.push_back(&r); break; }
    if (p.family.empty()) p.family.push_back(p.asked);   // template not in the DB: the asked row alone
    for (const auto& r : rows)
        if (r.clone_of == root && &r != p.family.front()) p.family.push_back(&r);
    const bool numeric = !want_seat.empty() &&
        want_seat.find_first_not_of("0123456789") == std::string::npos;
    if (want_seat.empty() || numeric) { p.chosen = p.asked; return p; }
    const std::string ws = norm_seat(want_seat);
    if (norm_seat(p.asked->seat) == ws) { p.chosen = p.asked; return p; }
    for (const Row* r : p.family)
        if (norm_seat(r->seat) == ws) { p.chosen = r; return p; }
    return p;
}

}  // namespace council_task_family
}  // namespace bridge

#endif  // CK3_BRIDGE_COUNCIL_TASK_FAMILY_H
