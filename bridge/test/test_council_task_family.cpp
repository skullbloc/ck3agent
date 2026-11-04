// test_council_task_family.cpp — L20-A, smoke17 row B5: which council task type
// a named task is on a given seat (council_task_family.h), on EVERY task type of
// the 867 dump.
//
// Fixture: the core dump ck3_20260625_152901_ivar867 (CLAUDE.md, "Runtime probing") (BuildID
// e61ff264...), CCouncilTaskTypeDatabase *(base+0x0bed76d8), the flat map at
// inst+0x1b0 (mask inst+0x1bc, stride 0x18, ctrl +0x04, value +0x10) exactly as
// do_set_council_task walks it; per type: name std::string +0x10, clone template
// +0x14d8, seat = *(+0x30)'s name +0x10. Read 2026-11-04 with re/coredump/
// core_reader.py. Built twice: plain (must pass) and -DFAMILY_PREFIX_CONTROL,
// the pre-fix rule (the asked type's own seat or refuse), registered WILL_FAIL:
// it refuses B5 with task_seat councillor_court_chaplain.
#include <cstdio>
#include <string>
#include <vector>
#include "council_task_family.h"

using bridge::council_task_family::Row;
static const std::vector<Row> kRows = {
    {0x7f9006a52300, 0x0, "task_increase_control", "councillor_marshal"},
    {0x7f9006a67100, 0x0, "task_vizier_default", "councillor_vizier"},
    {0x7f9006b52ce0, 0x7f90068e1600, "task_bestow_royal_favor_minister_of_personnel", "minister_personnel"},
    {0x7f90068e7e60, 0x0, "task_kurultai_fertility_1", "councillor_kurultai_1"},
    {0x7f9006a50e20, 0x0, "task_train_commanders", "councillor_marshal"},
    {0x7f9006a5ca00, 0x0, "task_disrupt_schemes", "councillor_spymaster"},
    {0x7f9006a64740, 0x0, "task_accept_culture", "councillor_steward"},
    {0x7f9006b541c0, 0x7f90068efba0, "task_culture_minister", "councillor_court_chaplain"},
    {0x7f90068ebd00, 0x0, "task_kurultai_domestic_affairs_1", "councillor_kurultai_1"},
    {0x7f9006b5bf00, 0x7f9006a50e20, "task_train_commanders_grand_marshal", "minister_grand_marshal"},
    {0x7f90068e3fc0, 0x0, "task_conversion", "councillor_court_chaplain"},
    {0x7f9006a4baa0, 0x0, "task_kurultai_exploit_area_4", "councillor_kurultai_4"},
    {0x7f90068f8dc0, 0x0, "task_kurultai_raider_2", "councillor_kurultai_2"},
    {0x7f90068e9340, 0x0, "task_kurultai_control_1", "councillor_kurultai_1"},
    {0x7f9006a4e460, 0x0, "task_kurultai_culture_4", "councillor_kurultai_4"},
    {0x7f9006a685e0, 0x0, "task_vizier_court_politics", "councillor_vizier"},
    {0x7f9006b556a0, 0x0, "task_justify_rule", "minister_justice"},
    {0x7f90068e0120, 0x0, "task_integrate_title", "councillor_chancellor"},
    {0x7f90068ff620, 0x0, "task_kurultai_court_astrologer_3", "councillor_kurultai_3"},
    {0x7f9006905e80, 0x0, "task_kurultai_default_4", "councillor_kurultai_4"},
    {0x7f90068efba0, 0x0, "task_kurultai_culture_1", "councillor_kurultai_1"},
    {0x7f9006a5a040, 0x0, "task_court_intrigue", "councillor_spouse"},
    {0x7f9006b56b80, 0x7f90068e54a0, "task_fabricate_claim_minister_of_rites", "minister_justice"},
    {0x7f9006a65c20, 0x0, "task_convince_dejure", "councillor_steward"},
    {0x7f90068fcc60, 0x0, "task_kurultai_fertility_3", "councillor_kurultai_3"},
    {0x7f9006a537e0, 0x0, "task_manage_guards", "councillor_marshal"},
    {0x7f90068f1080, 0x0, "task_kurultai_default_2", "councillor_kurultai_2"},
    {0x7f9006a58b60, 0x0, "task_manage_domain", "councillor_spouse"},
    {0x7f90068e2ae0, 0x0, "task_religious_relations", "councillor_court_chaplain"},
    {0x7f9006a5f3c0, 0x0, "task_find_secrets", "councillor_spymaster"},
    {0x7f90068dec40, 0x0, "task_domestic_affairs", "councillor_chancellor"},
    {0x7f9006a61d80, 0x0, "task_develop_county", "councillor_steward"},
    {0x7f90068e1600, 0x0, "task_bestow_royal_favor", "councillor_chancellor"},
    {0x7f9006a6c480, 0x0, "task_vizier_court_intrigue", "councillor_vizier"},
    {0x7f90068ee6c0, 0x0, "task_kurultai_raider_1", "councillor_kurultai_1"},
    {0x7f90069049a0, 0x0, "task_kurultai_culture_3", "councillor_kurultai_3"},
    {0x7f9006a57680, 0x0, "task_chivalry", "councillor_spouse"},
    {0x7f9006b5aa20, 0x0, "task_lead_troops", "minister_grand_marshal"},
    {0x7f90068f78e0, 0x0, "task_kurultai_exploit_area_2", "councillor_kurultai_2"},
    {0x7f90068ed1e0, 0x0, "task_kurultai_exploit_area_1", "councillor_kurultai_1"},
    {0x7f90068f6400, 0x0, "task_kurultai_domestic_affairs_2", "councillor_kurultai_2"},
    {0x7f90068fb780, 0x0, "task_kurultai_default_3", "councillor_kurultai_3"},
    {0x7f9006b58060, 0x0, "task_great_projects", "minister_works"},
    {0x7f90068f2560, 0x0, "task_kurultai_fertility_2", "councillor_kurultai_2"},
    {0x7f9006a5dee0, 0x0, "task_support_schemes", "councillor_spymaster"},
    {0x7f9006901fe0, 0x0, "task_kurultai_exploit_area_3", "councillor_kurultai_3"},
    {0x7f90068e54a0, 0x0, "task_fabricate_claim", "councillor_court_chaplain"},
    {0x7f9006a47c00, 0x0, "task_kurultai_control_4", "councillor_kurultai_4"},
    {0x7f9006a69ac0, 0x0, "task_vizier_chivalry", "councillor_vizier"},
    {0x7f9006a46720, 0x0, "task_kurultai_fertility_4", "councillor_kurultai_4"},
    {0x7f9006a6ee40, 0x0, "task_manage_talent", "minister_personnel"},
    {0x7f90068f3a40, 0x0, "task_kurultai_control_2", "councillor_kurultai_2"},
    {0x7f9006900b00, 0x0, "task_kurultai_domestic_affairs_3", "councillor_kurultai_3"},
    {0x7f90068f4f20, 0x0, "task_kurultai_court_astrologer_2", "councillor_kurultai_2"},
    {0x7f9006a6afa0, 0x0, "task_vizier_manage_domain", "councillor_vizier"},
    {0x7f9006b59540, 0x7f9006a61d80, "task_develop_county_minister_of_works", "minister_works"},
    {0x7f90068fa2a0, 0x0, "task_kurultai_culture_2", "councillor_kurultai_2"},
    {0x7f90068dd760, 0x0, "task_foreign_affairs", "councillor_chancellor"},
    {0x7f9006a5b520, 0x0, "task_patronage", "councillor_spouse"},
    {0x7f9006a63260, 0x0, "task_promote_culture", "councillor_steward"},
    {0x7f9006a4f940, 0x0, "task_organize_levies", "councillor_marshal"},
    {0x7f9006a54cc0, 0x0, "task_spouse_default", "councillor_spouse"},
    {0x7f9006a6d960, 0x0, "task_vizier_patronage", "councillor_vizier"},
    {0x7f9006a4cf80, 0x0, "task_kurultai_raider_4", "councillor_kurultai_4"},
    {0x7f9006a608a0, 0x0, "task_collect_taxes", "councillor_steward"},
    {0x7f9006a561a0, 0x0, "task_court_politics", "councillor_spouse"},
    {0x7f90069034c0, 0x0, "task_kurultai_raider_3", "councillor_kurultai_3"},
    {0x7f9006a4a5c0, 0x0, "task_kurultai_domestic_affairs_4", "councillor_kurultai_4"},
    {0x7f9006a490e0, 0x0, "task_kurultai_court_astrologer_4", "councillor_kurultai_4"},
    {0x7f90068ea820, 0x0, "task_kurultai_court_astrologer_1", "councillor_kurultai_1"},
    {0x7f90068e6980, 0x0, "task_kurultai_default_1", "councillor_kurultai_1"},
    {0x7f90068fe140, 0x0, "task_kurultai_control_3", "councillor_kurultai_3"},
};

static const Row* pick(const std::string& task, const std::string& seat) {
#ifdef FAMILY_PREFIX_CONTROL
    // the pre-fix rule (git show 85be8d78:bridge/src/action_handlers.cpp:1293-1296):
    // the asked type; refused only for a non-empty, non-numeric seat that differs
    // from the type's own seat (an empty task seat was admitted too)
    for (const auto& r : kRows)
        if (r.name == task) {
            const bool numeric = !seat.empty() &&
                seat.find_first_not_of("0123456789") == std::string::npos;
            if (seat.empty() || numeric || r.seat.empty() ||
                bridge::council_task_family::norm_seat(seat) ==
                    bridge::council_task_family::norm_seat(r.seat)) return &r;
            return nullptr;
        }
    return nullptr;
#else
    return bridge::council_task_family::resolve(kRows, task, seat).chosen;
#endif
}

static int failures = 0;
static void expect(const char* what, const Row* got, const char* want_name) {
    const bool ok = want_name ? (got && got->name == want_name) : (got == nullptr);
    printf("%s %-58s -> %s\n", ok ? "PASS" : "FAIL", what, got ? got->name.c_str() : "(refused)");
    if (!ok) failures++;
}

int main() {
    // B5: Cui 13191 (ministries), seat minister_justice
    expect("B5 task_fabricate_claim @ minister_justice",
           pick("task_fabricate_claim", "minister_justice"), "task_fabricate_claim_minister_of_rites");
    // the chaplain's own seat, both spellings, and no seat: unchanged
    expect("task_fabricate_claim @ court_chaplain", pick("task_fabricate_claim", "court_chaplain"), "task_fabricate_claim");
    expect("task_fabricate_claim @ councillor_court_chaplain",
           pick("task_fabricate_claim", "councillor_court_chaplain"), "task_fabricate_claim");
    expect("task_fabricate_claim, no seat", pick("task_fabricate_claim", ""), "task_fabricate_claim");
    // (not discriminating: the pre-fix rule admitted a numeric seat too; it pins
    // that the new rule keeps it)
    expect("task_fabricate_claim, numeric seat", pick("task_fabricate_claim", "1006"), "task_fabricate_claim");
    // a seat no family member sits on is still refused
    expect("task_fabricate_claim @ chancellor (refuse)", pick("task_fabricate_claim", "chancellor"), nullptr);
    // the other four clone families of the dump, both directions
    expect("task_develop_county @ minister_works",
           pick("task_develop_county", "minister_works"), "task_develop_county_minister_of_works");
    expect("task_train_commanders @ minister_grand_marshal",
           pick("task_train_commanders", "minister_grand_marshal"), "task_train_commanders_grand_marshal");
    expect("task_bestow_royal_favor @ minister_personnel",
           pick("task_bestow_royal_favor", "minister_personnel"), "task_bestow_royal_favor_minister_of_personnel");
    expect("clone asked on its template's seat",
           pick("task_fabricate_claim_minister_of_rites", "court_chaplain"), "task_fabricate_claim");
    // a task with no clone keeps the old refusal
    expect("task_increase_control @ chancellor (refuse)", pick("task_increase_control", "chancellor"), nullptr);
    expect("no such task", pick("task_does_not_exist", "minister_justice"), nullptr);
    // the family the refusal lists
    const auto fam = bridge::council_task_family::resolve(kRows, "task_fabricate_claim", "chancellor").family;
    const bool fam_ok = fam.size() == 2 && fam[0]->seat == "councillor_court_chaplain" &&
                        fam[1]->seat == "minister_justice";
    printf("%s family of task_fabricate_claim = %zu seat(s)\n", fam_ok ? "PASS" : "FAIL", fam.size());
    if (!fam_ok) failures++;
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
