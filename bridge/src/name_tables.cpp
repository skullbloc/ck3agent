// Static name resolution tables for CK3 interactions and decisions.
// Pre-computes MurmurHash3-32 (seed=0) for all known vanilla+DLC names,
// enabling hash→name lookup without runtime string extraction from memory.
//
// Also provides a character display name cache. The game stores localization
// keys (e.g. "AmE_dE_e") in memory; resolving them to display names (e.g.
// "Amedee") requires calling a game function on the main thread. The cache
// is populated during on_tick (game thread) and read from the IPC reader
// thread via get_cached_character_name().

#include "name_tables.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "bridge.h"
#include "ck3_offsets.h"
#include "world_epoch.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_map>

namespace bridge {

// =============================================================================
// MurmurHash3-32 (same as action_dispatch.cpp / CK3 FUN_08213140)
// =============================================================================

static uint32_t murmur3_32(const char* data, uint32_t len) {
    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;
    const uint32_t seed = 0;

    uint32_t h = seed;
    const uint32_t nblocks = len / 4;

    const uint8_t* blocks = reinterpret_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t k;
        memcpy(&k, blocks + i * 4, 4);
        k *= c1;
        k = (k << 15) | (k >> 17);
        k *= c2;
        h ^= k;
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xe6546b64;
    }

    const uint8_t* tail = blocks + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
    case 3: k1 ^= static_cast<uint32_t>(tail[2]) << 16; // fallthrough
    case 2: k1 ^= static_cast<uint32_t>(tail[1]) << 8;  // fallthrough
    case 1: k1 ^= static_cast<uint32_t>(tail[0]);
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;
        h ^= k1;
    }

    h ^= len;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;

    return h;
}

// =============================================================================
// Name tables
// =============================================================================

static std::unordered_map<uint32_t, const char*> s_interaction_names CK3_EARLY_INIT;
static std::unordered_map<uint32_t, const char*> s_decision_names CK3_EARLY_INIT;

static void insert_interaction(const char* name) {
    uint32_t h = murmur3_32(name, static_cast<uint32_t>(strlen(name)));
    s_interaction_names[h] = name;
}

static void insert_decision(const char* name) {
    uint32_t h = murmur3_32(name, static_cast<uint32_t>(strlen(name)));
    s_decision_names[h] = name;
}

void init_name_tables() {
    // --- Interaction names (433 vanilla + DLC) ---
    static const char* const interactions[] = {
        "abandon_hook_interaction",
        "accuse_of_decandence_interaction",
        "acknowledge_governor_interaction",
        "add_artifact_claim_interaction",
        "add_artifact_interaction",
        "add_glory_interaction",
        "add_hook_interaction",
        "add_to_court_interaction",
        "admin_liege_modify_vassal_contract_interaction",
        "admin_vassal_modify_vassal_contract_interaction",
        "adopt_interaction",
        "adventurer_adopt_interaction",
        "ai_cultural_hybridization_interaction",
        "ai_make_concubine_interaction",
        "ai_only_hegemonic_tributary_modify_tributary_contract_interaction",
        "ai_only_liege_modify_vassal_contract_admin_province_interaction",
        "ai_only_liege_modify_vassal_contract_interaction",
        "ai_only_vassal_modify_vassal_contract_interaction",
        "appoint_co_emperor_interaction",
        "appoint_co_monarch_interaction",
        "appoint_junior_emperor_interaction",
        "appoint_podesta_interaction",
        "appoint_vizier_interaction",
        "arrange_marriage_interaction",
        "ask_for_conversion_courtier_interaction",
        "ask_for_conversion_interaction",
        "ask_for_pardon_interaction",
        "assume_house_bloc_leadership_interaction",
        "attempt_conversion_of_local_ruler_interaction",
        "bargain_fealty_interaction",
        "become_tributary_interaction",
        "befriend_interaction",
        "blind_interaction",
        "boost_efficiency_interaction",
        "break_betrothal_interaction",
        "break_up_with_lover_interaction",
        "buy_claim_interaction",
        "cabinet_of_curiosities_interaction",
        "call_ally_interaction",
        "call_dynasty_member_to_war_interaction",
        "call_house_member_to_war_interaction",
        "castrate_child_interaction",
        "castrate_interaction",
        "ce1_ask_to_promote_legend_interaction",
        "cease_paying_tribute_interaction",
        "cease_tribute_payments_interaction",
        "celestial_ask_to_switch_movements_interaction",
        "celestial_demand_treasury_interaction",
        "celestial_extinguish_noble_family_interaction",
        "celestial_give_treasury_interaction",
        "celestial_movement_power_promote_interaction",
        "celestial_movement_power_slander_interaction",
        "celestial_request_commander_interaction",
        "celestial_request_elders_support_interaction",
        "celestial_request_examination_support_interaction",
        "celestial_request_military_aid_interaction",
        "celestial_request_treasury_interaction",
        "celestial_send_military_aid_interaction",
        "celestial_siphon_treasury_interaction",
        "censor_find_secrets_interaction",
        "challenge_for_artifact_interaction",
        "challenge_to_2p_chess_interaction",
        "challenge_to_single_combat_interaction",
        "challenge_to_trial_by_combat_interaction",
        "change_career_path_for_family_member_interaction",
        "change_character_weight_interaction",
        "change_cultural_acceptance_debug_interaction",
        "choose_favorite_interaction",
        "claim_all_interaction",
        "claim_throne_interaction",
        "common_interaction",
        "complete_task_contract_interaction",
        "concubine_interaction",
        "contact_list_request_interaction",
        "contest_appointment_interaction",
        "contract_assistance_interaction",
        "convert_at_war_interaction",
        "convert_to_religion_adventurer_interaction",
        "convert_to_religion_interaction",
        "convert_to_witchcraft_interaction",
        "council_task_interaction",
        "coup_ceremonial_liege_interaction",
        "court_interaction",
        "court_task_interaction",
        "create_betrothal_interaction",
        "create_ceremonial_claimant_faction_against_interaction",
        "create_claimant_faction_against_interaction",
        "create_task_contract_interaction",
        "damage_efficiency_interaction",
        "debug_add_to_contact_list_interaction",
        "debug_add_truce_interaction",
        "debug_alliance_interaction",
        "debug_change_council_task_interaction",
        "debug_change_opinion_interaction",
        "debug_coronation_add_to_guest_subset_interaction",
        "debug_destroy_artifact_interaction",
        "debug_give_animal_interaction",
        "debug_imprison_simple_interaction",
        "debug_move_domicile_interaction",
        "debug_progress_all_active_schemes_interaction",
        "debug_secrets_interaction",
        "debug_sex_interaction",
        "debug_start_era_of_great_holy_wars_interaction",
        "debug_travel_to_ruler_interaction",
        "debug_trigger_localization_testing_interaction",
        "declaration_of_repentance_interaction",
        "declare_me_regent_interaction",
        "declare_war_interaction",
        "decommission_mandala_temple_interaction",
        "demand_admin_interaction",
        "demand_artifact_interaction",
        "demand_aspiration_change_interaction",
        "demand_concubine_interaction",
        "demand_conversion_interaction",
        "demand_conversion_minister_rites_interaction",
        "demand_conversion_player_ruler_interaction",
        "demand_conversion_vassal_ruler_interaction",
        "demand_courtier_interaction",
        "demand_dynasty_banner_interaction",
        "demand_hostage_interaction",
        "demand_payment_interaction",
        "demand_tributary_interaction",
        "denounce_interaction",
        "designate_diarch_interaction",
        "designate_gruesome_festivals_sacrifice_interaction",
        "designate_heir_interaction",
        "designate_shieldmaiden_interaction",
        "destroy_artifact_interaction",
        "destroy_noble_family_interaction",
        "destroy_title_interaction",
        "diarch_adjust_bureaucracy_interaction",
        "diarch_coup_liege_interaction",
        "diarch_demand_despotate_interaction",
        "diarch_entrench_regency_interaction",
        "diarch_imprison_interaction",
        "diarch_interaction",
        "diarch_legal_meddling_interaction",
        "diarch_regent_appoints_self_co_emperor_interaction",
        "diarch_request_imperial_expedition_interaction",
        "diarch_retract_vassal_interaction",
        "diarch_revoke_title_interaction",
        "diarch_set_own_mandate_interaction",
        "diarch_shift_privileges_interaction",
        "diarch_subsidise_crown_authority_interaction",
        "diarch_swing_scales_currency_interaction",
        "diarch_syphon_treasury_interaction",
        "diarch_train_knight_interaction",
        "disinherit_children_interaction",
        "disinherit_interaction",
        "dismiss_concubine_interaction",
        "dismiss_strong_hook_interaction",
        "dismiss_weak_hook_interaction",
        "divorce_character_dynast_request_interaction",
        "divorce_character_dynast_request_rel_head_interaction",
        "divorce_character_house_head_interaction",
        "divorce_character_interaction",
        "divorce_character_rel_head_interaction",
        "do_a_crime_embezzle_interaction",
        "dynast_claim_title_interaction",
        "dynast_end_dynasty_wars_interaction",
        "dynast_legitimize_bastard_interaction",
        "educate_child_interaction",
        "elope_interaction",
        "end_diarchy_interaction",
        "end_war_attacker_defeat_interaction",
        "end_war_attacker_victory_interaction",
        "end_war_attacker_white_peace_interaction",
        "enforce_truce_interaction",
        "ep3_governor_request_war_permission_interaction",
        "ep3_hof_ask_for_influence_interaction",
        "ep3_join_faction_war_interaction",
        "ep3_mass_arrests_interaction",
        "ep3_switch_faction_war_sides_interaction",
        "evict_adventurer_interaction",
        "exact_tribute_interaction",
        "exact_tribute_player_interaction",
        "exchange_hostage_interaction",
        "excommunicate_interaction",
        "execute_prisoner_interaction",
        "expose_secret_interaction",
        "extoll_virtuousness_interaction",
        "fabricate_hook_interaction",
        "feudalize_herder_holding_interaction",
        "feudalize_nomadic_holding_interaction",
        "find_elder_interaction",
        "force_abdication_interaction",
        "force_duel_anyone_interaction",
        "force_duel_criminals_interaction",
        "force_governor_removal_interaction",
        "force_join_faction_interaction",
        "force_onto_council_interaction",
        "force_step_down_interaction",
        "force_vote_in_succession_election_interaction",
        "forgive_interaction",
        "fp3_challenge_house_head_interaction",
        "fp3_demand_submission_interaction",
        "fp3_request_honorary_titles_interaction",
        "fp3_request_house_unity_change_interaction",
        "fp3_request_turkic_invasion_interaction",
        "frontier_influence_war_interaction",
        "fund_inspiration_interaction",
        "generate_artifact_with_history_interaction",
        "get_claim_interaction",
        "get_claim_on_all_creatable_interaction",
        "gift_artifact_interaction",
        "gift_interaction",
        "give_criminal_trait_interaction",
        "give_infinite_schemes_of_type_interaction",
        "give_vassal_directive_interaction",
        "governor_removal_interaction",
        "grant_governorship_interaction",
        "grant_house_bloc_leadership_interaction",
        "grant_independence_interaction",
        "grant_titles_interaction",
        "grant_title_to_new_random_character_interaction",
        "grant_vassal_interaction",
        "harm_candidacy_interaction",
        "hire_laamp_mercenaries_interaction",
        "hof_ask_for_claim_interaction",
        "hof_ask_for_gold_interaction",
        "hof_redirect_great_holy_war_interaction",
        "host_honored_guest_interaction",
        "house_head_request_interaction",
        "improve_house_relation_in_bloc_interaction",
        "incite_revolts_interaction",
        "indebt_guest_interaction",
        "influence_candidacy_interaction",
        "influence_child_commander_interaction",
        "influence_child_personality_interaction",
        "influence_meritorious_career_interaction",
        "inspire_conversion_interaction",
        "inspire_interaction",
        "interfere_in_war_interaction",
        "interrogate_interaction",
        "invest_appointment_interaction",
        "invite_agent_to_scheme_interaction",
        "invite_interaction",
        "invite_to_council_interaction",
        "invite_to_council_position_interaction",
        "invite_to_court_interaction",
        "invite_to_house_bloc_interaction",
        "invite_to_scheme_interaction",
        "join_confederation_interaction",
        "join_house_bloc_interaction",
        "join_independence_faction_interaction",
        "join_vassal_war_interaction",
        "join_war_interaction",
        "kick_from_court_interaction",
        "kick_from_house_bloc_interaction",
        "laamp_leave_war_interaction",
        "laamp_war_change_sides_interaction",
        "landless_admin_switch_non_admin_top_liege_interaction",
        "learn_language_debug_interaction",
        "learn_language_interaction",
        "learn_secrets_interaction",
        "leave_house_bloc_interaction",
        "legitimize_bastard_interaction",
        "liege_dismiss_entrenched_regency_interaction",
        "liege_dismiss_temporary_regency_interaction",
        "liege_dismiss_vizier_interaction",
        "lift_excommunication_interaction",
        "literalist_debate_interaction",
        "make_child_learn_language_interaction",
        "make_concubine_interaction",
        "make_dynasty_house_head_interaction",
        "make_friend_interaction",
        "make_independent_interaction",
        "make_lover_interaction",
        "make_nemesis_interaction",
        "make_noble_family_interaction",
        "make_rival_interaction",
        "make_soulmate_interaction",
        "marry_off_interaction",
        "migration_interaction",
        "mongol_demand_subjugation_interaction",
        "move_to_dungeon_interaction",
        "move_to_house_arrest_interaction",
        "mpo_ask_for_herd_interaction",
        "mpo_demand_obedience_interaction",
        "mpo_gift_herd_interaction",
        "mpo_negotiate_obedience_interaction",
        "mpo_retrieve_land_from_herder_interaction",
        "mpo_vassal_to_tributary_interaction",
        "mulct_vizier_interaction",
        "negotiate_alliance_interaction",
        "negotiate_settlement_interaction",
        "noble_family_adoption_interaction",
        "nomad_liege_modify_vassal_contract_interaction",
        "nomad_vassal_modify_vassal_contract_interaction",
        "offer_courtier_interaction",
        "offer_eunuch_interaction",
        "offer_fealty_interaction",
        "offer_guardianship_interaction",
        "offer_hostage_interaction",
        "offer_peace_interaction",
        "offer_tributary_status_interaction",
        "offer_vassalization_interaction",
        "offer_ward_interaction",
        "overthrow_regent_scheme_interaction",
        "pardon_interaction",
        "pawn_gear_interaction",
        "pay_ransom_interaction",
        "pay_treasury_debt_interaction",
        "perk_alliance_interaction",
        "prison_break_contract_interaction",
        "procure_estate_interaction",
        "promote_divergent_or_hybrid_culture_interaction",
        "propose_best_friendship_interaction",
        "purchase_land_interaction",
        "purchase_truce_interaction",
        "ransom_interaction",
        "ransom_me_interaction",
        "reaffirm_best_friendship_interaction",
        "reassert_tributary_interaction",
        "recall_hostage_interaction",
        "refill_maa_interaction",
        "refill_maa_nomad_interaction",
        "reinforce_soldiers_interaction",
        "release_as_tributary_interaction",
        "release_from_prison_interaction",
        "release_hostage_interaction",
        "release_tributary_interaction",
        "relocate_camp_to_interaction",
        "remove_designated_gruesome_festivals_sacrifice_interaction",
        "remove_favorite_interaction",
        "remove_guardian_interaction",
        "remove_rival_interaction",
        "request_ally_title_troops_interaction",
        "request_co_emperor_interaction",
        "request_contract_assistance_interaction",
        "request_conversion_interaction",
        "request_council_interaction",
        "request_culture_conversion_interaction",
        "request_emperor_title_troops_interaction",
        "request_excommunication_interaction",
        "request_governorship_interaction",
        "request_great_project_contribution_interaction",
        "request_laamp_invasion_sponsorship_interaction",
        "request_mentoring_interaction",
        "request_raid_remit_interaction",
        "request_title_troops_back_interaction",
        "restore_inheritance_interaction",
        "retract_vassal_interaction",
        "return_hostage_interaction",
        "reveal_as_witch_interaction",
        "revel_in_pain_interaction",
        "revoke_title_interaction",
        "scapegoat_counterpart_interaction",
        "seduce_interaction",
        "seek_indulgences_interaction",
        "seize_realm_interaction",
        "select_ghw_beneficiary_interaction",
        "send_child_to_examination_interaction",
        "send_poem_interaction",
        "send_to_holy_order_interaction",
        "send_to_varangian_guard_interaction",
        "set_favored_minister_interaction",
        "set_primary_spouse_interaction",
        "set_relation_interaction",
        "share_known_secrets_interaction",
        "slay_character_interaction",
        "spawn_courtier_interaction",
        "spawn_epidemic_interaction",
        "spawn_legend_interaction",
        "special_ai_interaction",
        "special_interaction",
        "start_bout_interaction",
        "start_challenge_status_interaction",
        "start_coerce_contribution_interaction",
        "start_coerce_tributary_interaction",
        "start_damage_legitimacy_interaction",
        "start_depose_interaction",
        "start_diarchy_interaction",
        "start_disbelieve_mandala_interaction",
        "start_dispute_border_interaction",
        "start_expand_power_base_interaction",
        "start_foster_legitimacy_interaction",
        "start_found_despotate_interaction",
        "start_generate_claim_interaction",
        "start_independence_faction_interaction",
        "start_ingratiate_family_interaction",
        "start_leverage_contribution_interaction",
        "start_murder_interaction",
        "start_pregnancy_interaction",
        "start_promote_interaction",
        "start_raid_estate_interaction",
        "start_slander_interaction",
        "start_subsume_province_interaction",
        "start_teach_governor_interaction",
        "steal_herd_interaction",
        "stop_attacker_vassal_war_interaction",
        "stop_defender_vassal_war_interaction",
        "subject_modify_tributary_contract_interaction",
        "sumptuary_law_debate_interaction",
        "sun_trial_interaction",
        "support_candidacy_interaction",
        "support_feudalize_tribal_holding_interaction",
        "suzerain_modify_tributary_contract_interaction",
        "sway_interaction",
        "swing_scales_currency_interaction",
        "synergise_friendship_interaction",
        "systematically_maim_character_interaction",
        "take_artifact_interaction",
        "take_domain_interaction",
        "take_hostage_interaction",
        "take_realm_interaction",
        "take_title_interaction",
        "take_vassal_interaction",
        "take_vows_interaction",
        "task_contract_train_knight_interaction",
        "temporal_condemnation_interaction",
        "test_dynasty_prestige_interaction",
        "test_effect_localization_interaction",
        "tgp_mandala_send_missionary_interaction",
        "torture_interaction",
        "trade_oath_interaction",
        "transfer_county_interaction",
        "transfer_huge_gold_amount_to_treasury_interaction",
        "transfer_large_gold_amount_to_treasury_interaction",
        "transfer_small_gold_amount_to_treasury_interaction",
        "transfer_title_army_maa_interaction",
        "transfer_vassal_interaction",
        "tribalize_holding_interaction",
        "tribute_mission_artifact_interaction",
        "tribute_mission_bunga_mas_interaction",
        "tribute_mission_concubine_interaction",
        "tribute_mission_eunuch_interaction",
        "tribute_mission_gold_interaction",
        "tribute_mission_herd_interaction",
        "turn_subject_into_mandala_interaction",
        "vassal_claim_liege_title_interaction",
        "vassal_modify_vassal_contract_interaction",
        "view_war_interaction",
    };

    for (const char* name : interactions) {
        insert_interaction(name);
    }

    // --- Decision names (394 vanilla + DLC) ---
    static const char* const decisions[] = {
        "abandon_celibacy_decision",
        "abandon_crypto_faith_decision",
        "accept_cannibalism_decision",
        "accuse_krstjani_of_heresy_decision",
        "activity_is_valid_tournament_contestant_training_decision",
        "add_to_steppe_decision",
        "adjust_confederation_cohesion_decision",
        "admin_confirmation_decision",
        "adopt_a_new_faith_for_persia_decision",
        "adopt_domicile_culture_decision",
        "adopt_domicile_faith_decision",
        "adopt_nomadic_ways_decision",
        "adopt_puppy_decision",
        "adopt_special_succession_decision",
        "ai_create_head_of_faith_decision",
        "ai_movement_petition_change_laws_decision",
        "appoint_a_righteous_caliph_decision",
        "ask_western_help_decision",
        "assimilate_local_traditions_decision",
        "assimilate_to_mandala_decision",
        "author_book_decision",
        "avenge_the_battle_of_nahrawan_decision",
        "avenge_the_battle_of_tours_decision",
        "bandit_king_decision",
        "become_chakravarti_decision",
        "become_conqueror_decision",
        "become_greatest_of_khans_decision",
        "become_landless_adventurer_decision",
        "become_leading_house_of_the_confederation_decision",
        "become_nomadic_decision",
        "become_saoshyant_decision",
        "blend_traditions_decision",
        "borrow_from_holy_order_decision",
        "break_with_the_old_ways_decision",
        "build_holy_pilgrim_roads_decision",
        "butcher_animals_decision",
        "call_for_acclaimed_knight_candidates_decision",
        "call_for_confederation_decision",
        "cancel_holy_order_lease_decision",
        "change_all_house_relations_decision",
        "change_election_candidacy_status_decision",
        "change_imperial_aspirations_decision",
        "chinese_concubines_decision",
        "choose_designated_terrain_decision",
        "choose_master_terrain_decision",
        "clear_our_confederations_leading_house_decision",
        "commission_artifact_decision",
        "commission_epic_decision",
        "commission_icon_decision",
        "commission_legend_artifact_decision",
        "commission_silk_regalia_decision",
        "commit_suicide_decision",
        "confederation_kingdom_decision",
        "consolidate_rule_decision",
        "convert_clan_to_feudalism_through_liege_decision",
        "convert_feudalism_to_clan_through_liege_decision",
        "convert_to_administrative_decision",
        "convert_to_feudalism_from_administrative_decision",
        "convert_to_feudalism_liege_converted_decision",
        "convert_to_local_culture_decision",
        "convert_to_mandala_government_decision",
        "convert_to_meritocratic_decision",
        "convert_to_tribalism_decision",
        "convert_whole_realm_to_feudalism_decision",
        "convert_whole_realm_to_feudalism_west_african_pagan_decision",
        "council_of_toledo_decision",
        "create_armenian_empire_decision",
        "create_bunga_mas_decision",
        "create_cadet_branch_decision",
        "create_dar_al_islam_decision",
        "create_holy_order_decision",
        "create_israel_kingdom_decision",
        "create_outremer_empire_decision",
        "deceitful_progress_hostile_schemes_decision",
        "declare_bloodline_holy_decision",
        "declare_canarias_decision",
        "defenders_of_highgod_decision",
        "destroy_the_magi_decision",
        "develop_fertility_decision",
        "diligent_development_focus_decision",
        "diplomacy_oath_1_decision",
        "diplomacy_oath_2_decision",
        "dismantle_byz_pretender_decision",
        "dismantle_holy_pretender_decision",
        "dismantle_papacy_decision",
        "divine_the_stars_decision",
        "draw_up_new_charts_decision",
        "dynasty_of_many_crowns_decision",
        "eat_cheese_decision",
        "elevate_mann_and_the_isles_decision",
        "embrace_celibacy_decision",
        "embrace_english_culture_decision",
        "embrace_hebrew_language_decision",
        "embrace_heresy_decision",
        "embrace_outremer_culture_decision",
        "embrace_responsibilities_decision",
        "embrace_tengriism_decision",
        "empower_sicilian_parliament_decision",
        "enact_legacy_of_adventurer_decision",
        "encourage_confession_of_false_conversions_decision",
        "end_investments_minor_decision",
        "end_the_struggle_oath_decision",
        "enforce_traditions_decision",
        "england_request_laudabiliter_decision",
        "escape_from_prison_decision",
        "establish_bactrian_supremacy_decision",
        "establish_iconoclast_patriarchate_decision",
        "establish_norman_culture_decision",
        "establish_silk_production_decision",
        "establish_the_electorate_of_saxony_decision",
        "establish_yamagate_in_samarkand_decision",
        "estate_bolster_security_decision",
        "estate_governor_patrols_decision",
        "estate_granary_decision",
        "evangelize_the_slavs_decision",
        "exoticise_a_grand_hall_decision",
        "expand_duchy_decision",
        "expand_the_steppe_decision",
        "expunge_latin_dominion_decision",
        "extort_subjects_decision",
        "favor_dynastic_cycle_era_decision",
        "favored_movement_consults_heaven_decision",
        "favor_movement_decision",
        "favour_skilled_outsiders_decision",
        "favour_the_countryside_basques_decision",
        "forge_the_jomsvikings_decision",
        "formalise_the_daneland_decision",
        "form_austria_kingdom_decision",
        "form_bene_israel_decision",
        "form_bosporan_kingdom_decision",
        "form_brunei_empire_decision",
        "form_brunei_kingdom_decision",
        "form_dai_viet_empire_decision",
        "form_majapahit_empire_decision",
        "form_portugal_decision",
        "form_rum_sultanate_decision",
        "form_ryukyu_empire_decision",
        "form_siam_kingdom_decision",
        "form_switzerland_kingdom_decision",
        "form_the_kingdom_of_aragon_decision",
        "foster_integration_decision",
        "found_capital_of_the_rus_decision",
        "found_duchy_decision",
        "found_empire_decision",
        "found_empire_of_beth_nahrain_decision",
        "found_kingdom_decision",
        "found_kingdom_of_bosnia_decision",
        "found_kingdom_of_livonia_decision",
        "found_kingdom_toledo_decision",
        "found_salon_decision",
        "found_the_empire_of_hindustan_decision",
        "found_university_decision",
        "found_varangian_guard_decision",
        "found_witch_coven_decision",
        "fp3_become_clan_government_decision",
        "fp3_embellish_capital_decision",
        "fp3_embrace_zoroastrianism_decision",
        "fp3_name_read_in_friday_prayer_decision",
        "fp3_play_with_buzina_decision",
        "fp3_promote_persian_scripture_decision",
        "fp3_sponsor_persian_scholars_decision",
        "fp3_sponsor_research_project_decision",
        "fp3_sunder_the_caliphate_decision",
        "gain_weight_decision",
        "gather_faction_support_decision",
        "gather_provisions_decision",
        "give_sky_burial_decision",
        "go_fishing_decision",
        "golden_age_jewish_science_in_iberia_decision",
        "greatest_hunter_oath_decision",
        "heir_preparation_oath_decision",
        "hold_court_decision",
        "hold_mystical_communion_decision",
        "hold_triumph_decision",
        "house_head_consults_heaven_decision",
        "humiliate_the_stooge_decision",
        "impatient_rushing_all_schemes_decision",
        "improve_city_province_decision",
        "initiate_consolamentum_decision",
        "install_claimants_oath_decision",
        "introduce_a_new_fashion_at_court_decision",
        "invite_claimants_decision",
        "invite_knights_decision",
        "invite_poets_decision",
        "invite_religious_exiles_decision",
        "isolate_capital_decision",
        "isolate_family_decision",
        "jester_entertainment_decision",
        "jewish_restore_high_priesthood_decision",
        "kindle_royal_fire_decision",
        "knight_errant_decision",
        "launch_hungarian_migration_decision",
        "learning_oath_1_decision",
        "leave_confederation_decision",
        "legitimize_latin_dominion_decision",
        "local_arbitration_decision",
        "lose_weight_decision",
        "lunatic_building_decision",
        "mandala_adopt_clan_government_decision",
        "martial_oath_1_decision",
        "martial_oath_2_decision",
        "meditate_in_seclusion_decision",
        "mend_the_fracture_oath_decision",
        "mend_the_great_schism_decision",
        "merge_aquitaine_decision",
        "movement_petition_change_laws_decision",
        "movement_petition_decision",
        "move_shogunate_court_decision",
        "mozarabic_bind_the_faith_to_rome_decision",
        "mozarabic_break_with_rome_decision",
        "mpo_abuse_authority_paiza_decision",
        "mpo_become_greatest_of_khans_decision",
        "mpo_call_for_merchants_decision",
        "mpo_call_for_merchants_small_decision",
        "mpo_desperate_stance_decision",
        "mpo_divination_decision",
        "mpo_establish_paiza_system_decision",
        "mpo_gok_world_conquest_decision",
        "mpo_pax_mongolica_decision",
        "mpo_pleasure_dome_decision",
        "negotiate_the_danelaw_decision",
        "negotiate_the_danelaw_integrate_duchies_decision",
        "nomad_higher_tier_title_decision",
        "order_mass_eviction_decision",
        "pay_homage_decision",
        "pet_cat_decision",
        "pet_dog_decision",
        "petition_ceremonial_liege_decision",
        "petition_liege_decision",
        "pick_legendary_barony_decision",
        "pledge_loyalty_to_liege_decision",
        "pray_to_protective_amulet_decision",
        "prepare_greek_fire_dromons_decision",
        "promote_gothic_innovations_decision",
        "promote_hungarian_settlement_decision",
        "promote_other_culture_development_decision",
        "proselytize_to_your_people_decision",
        "provide_for_the_faithful_oath_decision",
        "provide_for_the_poor_oath_decision",
        "provide_for_the_warriors_oath_decision",
        "purge_pretenders_oath_decision",
        "raise_runestone_decision",
        "raise_stele_decision",
        "reclaim_britannia_decision",
        "reconfirm_grain_dole_decision",
        "recreate_byzantine_empire_decision",
        "recruit_court_position_decision",
        "recruit_from_nomadic_capital_decision",
        "recruit_terrain_specialist_decision",
        "reform_carolingian_empire_decision",
        "refound_pandidakterion_decision",
        "reinstitute_grain_dole_decision",
        "reject_inheritance_decision",
        "remove_from_steppe_decision",
        "renounce_governorship_decision",
        "renounce_noble_family_title_decision",
        "renounce_vow_of_poverty_decision",
        "renounce_vows_decision",
        "repay_loan_decision",
        "repent_oath_decision",
        "restore_backwater_counties_decision",
        "restore_badd_fortress_decision",
        "restore_dumnonia_decision",
        "restore_ecumenical_patriarchate_decision",
        "restore_holy_roman_empire_decision",
        "restore_mandala_capital_decision",
        "restore_old_vasconia_decision",
        "restore_papacy_decision",
        "restore_pope_in_rome_decision",
        "restore_roman_empire_decision",
        "restore_roman_empire_holy_decision",
        "restore_roman_empire_italian_decision",
        "restore_sunni_caliphate_decision",
        "retake_eastern_provinces_decision",
        "return_rome_decision",
        "return_to_the_steppe_decision",
        "revive_magyar_paganism_decision",
        "revoke_false_conversion_decision",
        "rites_of_passage_decision",
        "sacrifice_bear_decision",
        "sale_of_titles_decision",
        "scrape_the_barrel_decision",
        "seasoned_visitor_decision",
        "secede_from_empire_decision",
        "secure_high_kingdom_north_sea_decision",
        "secure_iberian_foothold_decision",
        "secure_mediterranean_decision",
        "seek_aid_of_the_spirits_decision",
        "seize_mandala_capital_decision",
        "select_personal_deity_germanic_decision",
        "select_personal_deity_hinduism_decision",
        "separatist_uprising_decision",
        "set_capital_constantinople_decision",
        "set_capital_rome_decision",
        "set_de_jure_capital_decision",
        "settle_people_decision",
        "settle_the_steppe_decision",
        "situation_dynastic_cycle_choose_movement_decision",
        "situation_dynastic_cycle_claim_mandate_decision",
        "split_roman_empire_decision",
        "spread_theravada_faith_decision",
        "start_city_construction_decision",
        "step_away_from_the_steppe_decision",
        "stewardship_oath_1_decision",
        "stewardship_oath_2_decision",
        "stop_gain_weight_decision",
        "stop_lose_weight_decision",
        "strengthen_bloodline_decision",
        "stress_loss_athletic_decision",
        "stress_loss_comfort_eater_decision",
        "stress_loss_confider_decision",
        "stress_loss_contrite_decision",
        "stress_loss_drunkard_decision",
        "stress_loss_flagellant_decision",
        "stress_loss_hashishiyah_decision",
        "stress_loss_improvident_decision",
        "stress_loss_inappetetic_decision",
        "stress_loss_irritable_decision",
        "stress_loss_journaller_decision",
        "stress_loss_profligate_decision",
        "stress_loss_rakish_decision",
        "stress_loss_reclusive_decision",
        "struggle_iberia_ending_compromise_decision",
        "struggle_iberia_ending_conciliation_decision",
        "struggle_iberia_ending_hostility_decision",
        "struggle_persia_ending_assertion_decision",
        "struggle_persia_ending_foundation_decision",
        "struggle_persia_ending_rekindle_iran_decision",
        "study_commander_trait_decision",
        "submit_to_the_great_khan_decision",
        "summon_fair_courtiers_decision",
        "take_stewardship_of_the_sacred_river_decision",
        "take_vow_of_poverty_decision",
        "tgp_china_attend_child_exam_decision",
        "tgp_china_study_confucian_classics_decision",
        "tgp_choose_career_path_decision",
        "tgp_claim_the_mantle_of_maitreya_decision",
        "tgp_command_family_to_take_exams_decision",
        "tgp_divine_reincarnation_decision",
        "tgp_japan_assert_regional_dominion_government_decision",
        "tgp_japan_become_a_monk_decision",
        "tgp_japan_become_a_poet_decision",
        "tgp_japan_become_shogun_decision",
        "tgp_japan_emperor_restoration_decision",
        "tgp_japan_emperor_war_decision",
        "tgp_japan_establish_house_fief_decision",
        "tgp_japan_establish_soryo_administration_decision",
        "tgp_japan_establish_soryo_manor_decision",
        "tgp_japan_imperial_branch_decision",
        "tgp_japan_restore_japanese_government_decision",
        "tgp_japan_restore_japanese_monarchy_decision",
        "tgp_japan_train_with_troops_decision",
        "tgp_korea_unify_goryeo_decision",
        "tgp_minister_recruit_courtiers_decision",
        "tgp_minister_request_bestowal_of_honors_decision",
        "tgp_refuse_appointments_decision",
        "tgp_resume_appointments_decision",
        "tgp_suggest_budget_change_decision",
        "tgp_tea_ceremony_decision",
        "train_for_tournament_decision",
        "treasure_hunting_contract_decision",
        "treasure_map_contract_decision",
        "tribal_challenge_ruler_decision",
        "unify_burgundy_kingdom_decision",
        "unify_italian_empire_decision",
        "unify_region_decision",
        "unite_africa_decision",
        "unite_the_slavs_decision",
        "unite_the_southern_slavs_decision",
        "unite_the_spanish_thrones_decision",
        "unite_the_western_slavs_decision",
        "unity_consult_house_decision",
        "unity_educate_youth_in_madrasas_decision",
        "unity_encourage_house_economy_decision",
        "unity_establish_futuwaa_lodges_decision",
        "unity_establish_network_decision",
        "unity_improve_logistics_decision",
        "unity_improve_taxation_decision",
        "unity_organize_house_for_war_decision",
        "unity_placate_vassals_decision",
        "unity_promote_development_decision",
        "unity_reinforce_army_decision",
        "unity_search_for_administrators_decision",
        "unity_send_bailiffs_decision",
        "unity_steer_house_direction_decision",
        "visit_local_settlement_decision",
        "visit_local_shrine_decision",
        "visit_silk_road_market_decision",
        "wild_goose_chase_decision",
        "wipe_slate_decision",
        "write_muwashshah_poem_decision",
        "zealous_missionary_prep_decision",
        "zealous_missionary_start_decision",
        "zoroastrian_restore_high_priesthood_decision",
    };

    for (const char* name : decisions) {
        insert_decision(name);
    }
}

const char* resolve_interaction_name(uint32_t hash) {
    auto it = s_interaction_names.find(hash);
    return (it != s_interaction_names.end()) ? it->second : nullptr;
}

const char* resolve_decision_name(uint32_t hash) {
    auto it = s_decision_names.find(hash);
    return (it != s_decision_names.end()) ? it->second : nullptr;
}

// =============================================================================
// Character Display Name Cache
// =============================================================================
// The game stores localization keys at CCharacter+0x20 (e.g. "AmE_dE_e").
// Resolving to display names requires calling CHAR_FIRST_NAME_RESOLVED on the
// game thread. The reader thread (IPC queries) cannot safely call game functions.
//
// Flow:
//   1. Reader thread calls get_cached_character_name(id) — returns cached name
//      or empty string on miss, and adds id to g_needs_resolution set.
//   2. on_tick (game thread) calls populate_name_cache() which resolves any
//      pending IDs and caches the result.
//   3. Next reader query for the same id returns the resolved name.
//
// Pattern: game thread populates the cache, reader thread reads under a mutex.
//
// What a miss looks like to a client (lane-namekey): the reply carries NO
// `name`; it carries `name_key` (the key on the object, `E_thelred`) and
// `name_pending: true` (ad_read_char_name_full / emit_char_name in
// dispatch_router.cpp). A paused game runs no on_tick, so step 2 waits for
// time to move; the bridge never pulses the clock for a name read.

using fn_resolve_name = std::string*(*)(std::string*, void*);

static std::mutex g_name_cache_mutex;
static std::unordered_map<uint32_t, std::string> g_name_cache CK3_EARLY_INIT;
static std::set<uint32_t> g_needs_resolution CK3_EARLY_INIT;

// ck-hxh7: this cache is keyed by CHARACTER ID, and character ids are REUSED
// across saves. Its lifetime is therefore the loaded WORLD, not the process.
// Register its clear function with the world-epoch detector so a save loaded
// into a live process drops it. (Before this, clear_name_cache() had no caller
// anywhere in the tree and the bridge served the previous world's names for the
// rest of the process — see world_epoch.h.)
[[maybe_unused]] static const WorldResetRegistrar g_name_cache_world_scoped CK3_EARLY_INIT([] {
    register_world_reset("character_name_cache", &clear_name_cache);
});

// Test seam (ck-hxh7 regression test). When non-null, populate_name_cache()
// resolves through this instead of the engine, so the cache's WORLD-LIFETIME
// behaviour can be exercised host-side with no game attached. Always null in a
// deployed bridge; the engine path below is untouched.
static character_name_resolver_fn g_name_resolver_override = nullptr;

void set_character_name_resolver(character_name_resolver_fn fn) {
    std::lock_guard<std::mutex> lock(g_name_cache_mutex);
    g_name_resolver_override = fn;
}

// Helper: look up a CCharacter* by ID from the character database.
// Same pattern as ad_db_lookup in action_dispatch.cpp.
static uintptr_t name_cache_char_lookup(uint32_t id) {
    uintptr_t db = read_offset<uintptr_t>(ck3::globals::CHARACTER_DB);
    if (!db) return 0;
    uintptr_t table = read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
    if (!table) return 0;
    uint32_t capacity = read<uint32_t>(db + ck3::hash_map::CAPACITY);
    uint32_t masked = id & 0xFFFFFF;
    if (masked >= capacity) return 0;
    uintptr_t entry = read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                      + static_cast<uintptr_t>(masked) * ck3::hash_map::ENTRY_STRIDE);
    if (!entry) return 0;
    if ((read<uint32_t>(entry + ck3::character::ID) & 0xFFFFFF) != masked) return 0;
    return entry;
}

// Guard the input to the engine's name resolver (CHAR_FIRST_NAME_RESOLVED).
//
// The resolver reads the character's name fields (first-name std::string,
// override name, localization) and BUILDS a std::string inside the engine.
// If the character has been freed/reused or its name std::string is
// mid-mutation, the resolver over-reads a stale length/pointer and corrupts
// the heap (crash2 20260626). The slot-ID match in name_cache_char_lookup
// only proves the hash slot is occupied — it does NOT prove the object is a
// live character with sane name fields.
//
// Validate cheaply, per-tick, BEFORE handing the object to the engine:
//   1. ALIVE — DEATH_DATE (char+0x1c8) must be 0 (the codebase's canonical
//      liveness test; see game_state.cpp / observation.cpp). A dead or
//      being-destroyed character has a non-zero death-date pointer.
//   2. NAME-FIELD SANITY — the first-name std::string (char+0x20) must pass
//      safe_read_game_string's full validation (len 1..4096, mapped data
//      range). This is the same pre-validation we apply to direct game-string
//      reads; applying it to the resolver's INPUT object closes the hole.
// On any failure, skip the entry (leave it uncached → numeric-id fallback).
static bool name_resolve_input_is_safe(uintptr_t char_ptr) {
    if (char_ptr < 0x10000 || char_ptr > 0x7FFFFFFFFFFF) return false;
    if (!is_mapped(char_ptr)) return false;

    // 1. Liveness: DEATH_DATE == 0 means alive (canonical test).
    if (safe_read<uintptr_t>(char_ptr + ck3::character::DEATH_DATE) != 0) {
        return false;
    }

    // 2. First-name std::string must be readable & sane (len + mapped data).
    std::string probe;
    if (!safe_read_game_string(char_ptr + ck3::character::FIRST_NAME, probe)) {
        return false;
    }

    return true;
}

std::string get_cached_character_name(uint32_t char_id) {
    std::lock_guard<std::mutex> lock(g_name_cache_mutex);
    auto it = g_name_cache.find(char_id);
    if (it != g_name_cache.end()) {
        return it->second;
    }
    // Cache miss — schedule resolution on the next on_tick
    g_needs_resolution.insert(char_id);
    return {};
}

void request_name_resolution(uint32_t char_id) {
    std::lock_guard<std::mutex> lock(g_name_cache_mutex);
    if (g_name_cache.find(char_id) == g_name_cache.end()) {
        g_needs_resolution.insert(char_id);
    }
}

void populate_name_cache() {
    // Collect IDs that need resolution under the lock, then release it
    // before calling game functions (which must not be called under our lock).
    std::set<uint32_t> pending;
    {
        std::lock_guard<std::mutex> lock(g_name_cache_mutex);
        if (g_needs_resolution.empty()) return;
        pending.swap(g_needs_resolution);
    }

    character_name_resolver_fn override_fn;
    {
        std::lock_guard<std::mutex> lock(g_name_cache_mutex);
        override_fn = g_name_resolver_override;
    }

    auto resolve_fn = reinterpret_cast<fn_resolve_name>(
        bridge::game_fn(ck3::funcs::CHAR_FIRST_NAME_RESOLVED));
    if (!resolve_fn && !override_fn) return;
    // No address for the engine's resolver in this build (Windows: poisoned until
    // mapped): names stay pending, as they are before the first tick, rather than
    // faulting once per tick (P6 full1).
    if (!override_fn && !is_mapped(reinterpret_cast<uintptr_t>(resolve_fn))) return;

    int resolved = 0;
    int skipped = 0;
    for (uint32_t id : pending) {
        std::string name;

        if (override_fn) {
            // Test seam only — never set in a deployed bridge.
            if (!override_fn(id, name)) { skipped++; continue; }
        } else {
            uintptr_t char_ptr = name_cache_char_lookup(id);
            if (!char_ptr) continue;

            // Never hand a stale/dead/being-destroyed character to the engine's
            // name resolver — it over-reads bad name fields and corrupts the heap
            // (crash2 20260626). Skip → numeric-id fallback at the read site.
            if (!name_resolve_input_is_safe(char_ptr)) {
                skipped++;
                continue;
            }

            bridge::game_first_name_resolved(reinterpret_cast<uintptr_t>(resolve_fn), &name,
                                             reinterpret_cast<void*>(char_ptr));
        }

        if (!name.empty()) {
            std::lock_guard<std::mutex> lock(g_name_cache_mutex);
            g_name_cache[id] = std::move(name);
            resolved++;
        }
    }

    if ((resolved > 0 || skipped > 0) && g_logfile) {
        fprintf(g_logfile,
                "[CK3-Bridge] Name cache: resolved %d names, skipped %d unsafe (%zu cached total)\n",
                resolved, skipped, g_name_cache.size());
        fflush(g_logfile);
    }
}

void clear_name_cache() {
    std::lock_guard<std::mutex> lock(g_name_cache_mutex);
    g_name_cache.clear();
    g_needs_resolution.clear();
    if (g_logfile) {
        fprintf(g_logfile, "[CK3-Bridge] Name cache cleared\n");
        fflush(g_logfile);
    }
}

} // namespace bridge
