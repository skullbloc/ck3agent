// war_stance_record.h — which AI coordinator record answers query_war_stance's
// row for a character: the one definition.
//
// L22 (smoke19 A3): at 0868.01.09 Charles 12015, war 13's primary_attacker with
// occupation 8 and a won siege, read `is_attacker false, stance
// defender_defensive`. The handler kept ONE record per war,
//
//     rec_stance[rec_war_id & 0xFFFFFF] = { idx, is_atk };
//
// but the AI manager holds one record per war PER SIDE (the side is the record's
// own bit rec+0x68 & 4, the bit FUN_04e60920 branches on to return that side's
// primary character), so the record walked last overwrote the other and the row
// took both its side and its stance from whichever side the array listed last.
// Census of ck3_20260421_195552.core (1097, same BuildID): 174 active records,
// 87 wars, exactly one attacker and one defender record each; in 39 of the 87
// the defender's record comes last.
//
// The rule now: keep both sides' records; the row's side comes from the war
// record (war_side::of: the participant arrays, then the primary refs), and the
// stance from the record of THAT side. Header-only and engine-free:
// bridge/test/test_war_stance_record.cpp compiles it on a fixture from that dump.
#ifndef CK3_BRIDGE_WAR_STANCE_RECORD_H
#define CK3_BRIDGE_WAR_STANCE_RECORD_H

namespace bridge {
namespace war_stance_record {

// One side's coordinator record, as the walk saw it.
struct SideRec {
    bool present = false;
    int stance_idx = -1;   // index into the 7 stance slots; -1 = matched no slot
};

// Both sides' records of one war.
struct WarRecs {
    SideRec attacker, defender;
#ifdef WAR_STANCE_RECORD_PREFIX_CONTROL
    SideRec last; bool last_atk = true;   // the pre-fix map: the last record wins
#endif
};

// The walk files a record under its own side (rec+0x68 & 4).
inline void note(WarRecs& w, bool rec_is_attacker, int stance_idx) {
    SideRec& s = rec_is_attacker ? w.attacker : w.defender;
    s.present = true;
    s.stance_idx = stance_idx;
#ifdef WAR_STANCE_RECORD_PREFIX_CONTROL
    w.last = s; w.last_atk = rec_is_attacker;
#endif
}

// The record a row on `side_is_attacker` reads (the side from war_side::of).
inline const SideRec& for_side(const WarRecs& w, bool side_is_attacker) {
#ifdef WAR_STANCE_RECORD_PREFIX_CONTROL
    (void)side_is_attacker;
    return w.last;
#else
    return side_is_attacker ? w.attacker : w.defender;
#endif
}

// The side a row reports. Pre-fix it was the last record's bit.
inline bool row_is_attacker(const WarRecs& w, bool side_is_attacker) {
#ifdef WAR_STANCE_RECORD_PREFIX_CONTROL
    (void)side_is_attacker;
    return w.last_atk;
#else
    (void)w;
    return side_is_attacker;
#endif
}

}  // namespace war_stance_record
}  // namespace bridge

#endif  // CK3_BRIDGE_WAR_STANCE_RECORD_H
