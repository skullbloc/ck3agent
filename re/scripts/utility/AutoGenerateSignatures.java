// AutoGenerateSignatures.java
// Generates byte-pattern signatures for CK3 bridge functions.
// For each function address: read 32 bytes, disassemble, wildcard RIP-relative
// and CALL/JMP displacements, test uniqueness in .text section.
//
// Usage (headless):
//   analyzeHeadless <project_dir> <project_name> -process <program> -noanalysis \
//     -postScript AutoGenerateSignatures.java [<output_dir> [<functions.csv>]]
//
//   output_dir     default: $CK3_RE_OUT, else the working directory (headless)
//                  or a directory prompt (GUI). Writes auto_signatures.txt
//                  (C++ ready for signatures.h) and auto_signatures_summary.txt.
//   functions.csv  optional. One `NAME,0xOFFSET` per line (# comments allowed);
//                  OFFSET is base-relative (Ghidra VA minus the program's image
//                  base). Replaces the built-in table below, which is the Linux
//                  1.18.3.1 list, and turns off the already-have-a-signature
//                  skip. On a new binary, pass your own candidates here.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.lang.*;
import java.io.*;
import java.util.*;

public class AutoGenerateSignatures extends GhidraScript {

    // All function addresses from ck3_offsets.h namespace funcs
    // Format: { file_offset, "NAME" }
    // file_offset = ELF VA (add 0x100000 for Ghidra VA in PIE binary)
    // The built-in table uses the Linux convention (Ghidra VA = offset + 0x100000);
    // run() takes the base from the program, which is 0x100000 for that ELF.

    static final Object[][] ALL_FUNCS = {
        {0x04ceb5e0L, "AI_MANAGER_MAIN_TICK"},
        {0x04cedfd0L, "AI_MANAGER_POST_EXECUTE"},
        {0x04ce72e0L, "AI_MANAGER_DESTRUCTOR"},
        {0x04d1f190L, "AI_CHAR_TOGGLE"},
        {0x04d1f6b0L, "AI_CHAR_EVAL"},
        {0x04d22cb0L, "AI_CHAR_EVAL_SECONDARY"},
        {0x04c88fe0L, "AI_MARRIAGES_DISPATCH"},
        {0x04c89360L, "AI_INTERACTIONS_DISPATCH"},
        {0x04c89110L, "AI_LIFESTYLE_DISPATCH"},
        {0x04c89240L, "AI_MARRIAGE_WAR_DISPATCH"},
        {0x04c892d0L, "AI_WAR_COUNCIL_DISPATCH"},
        {0x04c891e0L, "AI_CALL_ALLIES_DISPATCH"},
        {0x04c88eb0L, "AI_COURT_ACTIVITY_DISPATCH"},
        {0x04c89530L, "AI_GRANT_REVOKE_1"},
        {0x04c89580L, "AI_GRANT_REVOKE_2"},
        {0x04c895d0L, "AI_TAX_DISPATCH"},
        {0x0664dca0L, "AI_PERSONALITY_HONOR"},
        {0x0664dcc0L, "AI_PERSONALITY_GREED"},
        {0x0664dce0L, "AI_PERSONALITY_RATIONALITY"},
        {0x0664dd00L, "AI_PERSONALITY_ENERGY"},
        {0x0664dd20L, "AI_PERSONALITY_BOLDNESS"},
        {0x0664dd40L, "AI_PERSONALITY_ZEAL"},
        {0x0664dd60L, "AI_PERSONALITY_VENGEFULNESS"},
        {0x0664dd80L, "AI_PERSONALITY_COMPASSION"},
        {0x0664dda0L, "AI_PERSONALITY_SOCIABILITY"},
        {0x065b6020L, "WAR_SCORE_UPDATE"},
        {0x065ba0d0L, "WAR_COMPUTE_SCORE"},
        {0x04cc4b90L, "AI_INTERACTION_DECISION"},
        {0x04d22820L, "AI_WAR_TARGET_EVAL"},
        {0x04d50660L, "AI_WAR_DECLARE_EXEC"},
        {0x04d55c10L, "AI_WAR_POST_DECL"},
        {0x04100770L, "CMD_AI_ENABLE"},
        {0x041010d0L, "CMD_AI_DISABLE"},
        {0x04122760L, "CMD_AI_DECLARE_WAR"},
        {0x04127e90L, "CMD_AI_TRY_INTERACTION"},
        {0x04128300L, "CMD_AI_TRY_DECISION"},
        {0x0411fc40L, "CMD_AI_INTERACTION_VALUES"},
        {0x04134500L, "CMD_AI_TOGGLE_ALL"},
        {0x04105500L, "CMD_SET_IS_AI"},
        {0x040fce10L, "CMD_YESMEN"},
        {0x040fd020L, "CMD_NOMEN"},
        {0x04d03b80L, "CMD_PRINT_AI_TIMINGS"},
        {0x0664b2c0L, "CHAR_IS_ALIVE"},
        {0x0664f390L, "CHAR_IS_RULER"},
        {0x06689a10L, "GET_FACTION_LIST"},
        {0x06680040L, "CHAR_GET_AI_CONTROLLER"},
        {0x05f5a1f0L, "CHAR_ITERATOR_START"},
        {0x06708010L, "CHAR_ITERATOR_NEXT"},
        {0x08113140L, "STRING_HASH"},
        {0x04d64160L, "STRATEGY_GOAL_PROCESS"},
        {0x04d66220L, "STRATEGY_EXECUTE"},
        {0x037df190L, "CHAR_ID_TO_PTR"},
        {0x061642e0L, "GET_PLAYER_CHAR_ID"},
        {0x061b8cf0L, "AI_PERSONALITY_EVAL"},
        {0x06688310L, "AI_PERSONALITY_PARAMETRIC"},
        {0x04cd2190L, "AI_TRY_DECISION_CORE"},
        {0x07660390L, "GAME_COMMAND_DISPATCH"},
        {0x05b7e6d0L, "RESOLVE_CHAR_SCOPE"},
        {0x06655990L, "GOLD_GET"},
        {0x0664b8c0L, "PIETY_GET"},
        {0x0664b8e0L, "PRESTIGE_GET"},
        {0x0666c7c0L, "SKILL_GET_BASE"},
        {0x0666c800L, "SKILL_GET_CALC"},
        {0x0666c720L, "SKILL_SET"},
        {0x0664beb0L, "MIL_CURRENT"},
        {0x0664bed0L, "MIL_MAX"},
        {0x075fd240L, "PAUSE_SET"},
        {0x075fd2d0L, "PAUSE_TOGGLE"},
        {0x03883f10L, "IS_MANUALLY_PAUSED"},
        {0x066c88c0L, "ADD_TRAIT"},
        {0x0664f4f0L, "TRAIT_GET_LIST"},
        {0x066746a0L, "TRAIT_ADD"},
        {0x06676150L, "TRAIT_REMOVE"},
        {0x037fe8c0L, "TRAIT_BSEARCH"},
        {0x03d725b0L, "CLAIM_GET_STATE"},
        {0x03f25c10L, "CLAIM_BUILD_LIST"},
        {0x06684b40L, "CLAIM_REMOVE"},
        {0x06684e60L, "CLAIM_CHANGE_STRENGTH"},
        {0x05e5ace0L, "EVENT_QUEUE_ENUM"},
        {0x05ecf3f0L, "EVENT_OPTION_COUNT"},
        {0x05e4d570L, "AI_EVENT_EVAL"},
        {0x0601c600L, "BUILDING_TOTAL_SLOTS"},
        {0x0601c610L, "BUILDING_FREE_SLOTS"},
        {0x0601c6b0L, "BUILDING_USED_SLOTS"},
        {0x066ef340L, "INTERACTION_RESOLVE"},
        {0x04ec3860L, "ADD_OPINION_CMD"},
        {0x05eb6df0L, "OPINION_TRIGGER"},
        {0x06678ab0L, "GET_OPINION_OF"},
        {0x061ca540L, "OPINION_BASE_CALC"},
        {0x067e3700L, "OPINION_REALM_MOD"},
        {0x05bc7c00L, "TRAIT_GET_DESCRIPTOR"},
        {0x064f9380L, "TRAIT_INDEX_TO_PTR"},
        {0x065f2960L, "TRAIT_PARSE_FIELD"},
        {0x066e8bd0L, "MOD_COLLECTION_PARSE"},
        {0x066e6ac0L, "MOD_COLLECTION_GET_SLOT"},
        {0x066e2230L, "MOD_DEF_LOOKUP_BY_TOKEN"},
        {0x066e21c0L, "MOD_DEF_GET_BY_INDEX"},
        {0x068710e0L, "RECALCULATE_MODIFIERS"},
        {0x06775b40L, "APPLY_TRAIT_RULER_EFFECTS"},
        {0x060c3310L, "TITLE_GET_COUNTY"},
        {0x060c58f0L, "TITLE_GET_TIER"},
        {0x060c3ed0L, "TITLE_GET_HOLDER"},
        {0x0411b7e0L, "FIRE_EVENT"},
        {0x02745900L, "EVENT_OPTION_SELECTION"},
        {0x05dba4d0L, "HOUSE_GET_NAME"},
        {0x05dbe2f0L, "DYNASTY_GET_FROM_HOUSE"},
        {0x05daa120L, "DYNASTY_GET_NAME"},
        {0x0666ada0L, "SET_CULTURE"},
        {0x0666bc40L, "SET_FAITH"},
        {0x06678dd0L, "WAR_PARTICIPATION_SET"},
        {0x065d10a0L, "WAR_CONSTRUCTOR"},
        {0x065be2d0L, "WAR_PARTICIPANT_SETUP"},
        {0x05a5ab70L, "WAR_CB_SET"},
        {0x05a5a820L, "WAR_CB_CONSTRUCTOR"},
        {0x065c4740L, "WAR_SIDE_INSERT"},
        {0x038d77a0L, "WAR_PARTICIPANT_SEARCH"},
        {0x065b6700L, "WAR_SET_NAME"},
        {0x065b6290L, "WAR_COPY_SCORES"},
        {0x04124030L, "CMD_SET_CULTURE"},
        {0x04123790L, "CMD_ADD_REALM_LAW"},
        {0x0410f110L, "CMD_FACTION_SPAWN"},
        {0x06672bf0L, "ADD_REALM_LAW"},
        {0x06b3d920L, "BUILD_SEND_INTERACTION_CMD"},
        {0x06b3ca50L, "INTERACTION_LOOKUP_BY_IDX"},
        {0x06b45300L, "INTERACTION_CTX_INIT"},
        {0x06b40000L, "INTERACTION_CTX_RESET"},
        {0x06b3f140L, "INTERACTION_CTX_SET_TARGET"},
        {0x06b3f500L, "INTERACTION_CTX_SET_RNG"},
        {0x06b41880L, "INTERACTION_CTX_VALIDATE"},
        {0x04dc5ea0L, "INTERACTION_FINALIZE"},
        {0x06b43eb0L, "INTERACTION_TRY_SEND"},
        {0x06b441c0L, "INTERACTION_COMPUTE_ACCEPT"},
        {0x04d980d0L, "WAR_BUILD_STATE"},
        {0x04d20b60L, "WAR_CHECK_FEASIBILITY"},
        {0x065dbcb0L, "WAR_ATTACH_STATE"},
        {0x065dbcc0L, "WAR_ATTACH_CB"},
        {0x0658fce0L, "CREATE_MAA_REGIMENT"},
        {0x06685330L, "CREATE_SECRET"},
        {0x05b83370L, "COUNCIL_TASK_CMD"},
        {0x05b86ef0L, "COUNCIL_PROVINCE_TASK"},
        {0x0677baf0L, "GET_COUNCIL_CONFIG"},
        {0x0677bdf0L, "GET_COUNCIL_INSTANCE"},
        {0x06d332c0L, "LAW_LOOKUP_BY_NAME"},
        {0x05c188b0L, "CULTURE_GET_NAME"},
        {0x05c2c530L, "CULTURE_GET_ID"},
        {0x06659c30L, "CHAR_DISPLAY_NAME"},
        {0x0675b4c0L, "CHAR_FIRST_NAME_RESOLVED"},
        {0x07fd3810L, "LOC_KEY_CHECK"},
        {0x07fbc5e0L, "LOC_RESOLVE"},
        {0x0619b410L, "BUILD_SEND_CMD_FROM_BUF"},
        {0x065891b0L, "ARMY_DISBAND"},
        {0x03de3e40L, "ARMY_RAISE_ALL"},
        {0x03de4b20L, "ARMY_RAISE_ALL_WRAPPER"},
        {0x05abe070L, "ARMY_IS_RETREATING"},
        {0x05ac2dd0L, "ARMY_IN_COMBAT"},
        {0x05ac2e20L, "ARMY_GET_OWNER_CHAR_ID"},
        {0x05ac4cc0L, "ARMY_GET_COMMANDER"},
        {0x05f61870L, "ARMY_ENUM_SUB"},
        {0x065d55a0L, "ARMY_CHECK_ENEMIES_NEARBY"},
        {0x065d5320L, "ARMY_CHECK_WAR_LEADER"},
        {0x065d5b60L, "ARMY_CHECK_HOSTILE_AREA"},
        {0x05ab5b00L, "SET_COUNCIL_TASK"},
        {0x0667bdf0L, "GET_COUNCIL_DATA"},
        {0x04df21f0L, "COUNCIL_TASK_EXECUTE"},
        {0x04d29140L, "CHECK_CONTRACT_VALID"},
        {0x04d29130L, "APPLY_CONTRACT_CHANGE"},
        {0x04d29690L, "GET_OBLIGATION_LEVEL"},
        {0x04d4c980L, "CREATE_OBLIGATION"},
        {0x04d46630L, "GET_NEXT_OBLIGATION"},
        {0x04d47180L, "GET_PREV_OBLIGATION"},
        {0x04d29ec0L, "GET_OBLIGATION_REALM"},
        {0x04d29100L, "ASSIGN_OBLIGATION"},
        {0x06670d30L, "GET_AGE"},
        {0x0664ddc0L, "IS_ADULT"},
        {0x0664b790L, "GET_STRESS"},
        {0x06679080L, "ADD_STRESS"},
        {0x0664ba90L, "GET_DREAD"},
        {0x066891b0L, "ADD_DREAD"},
        {0x06689160L, "SET_DREAD"},
        {0x067e27b0L, "FOCUS_GETTER"},
        {0x067e27d0L, "FOCUS_SETTER_CORE"},
        {0x038b58f0L, "FOCUS_LOOKUP_BY_KEY"},
        {0x066f5eb0L, "GET_TOTAL_PERK_POINTS"},
        {0x066f5ee0L, "GET_PERK_POINTS"},
        {0x066f60c0L, "GET_PERK_POINTS_USED"},
        {0x066f6300L, "GET_LIFESTYLE_XP"},
        {0x066f6320L, "HAS_PERK"},
        {0x05f5a230L, "GET_SCHEME_MANAGER"},
        {0x062a9000L, "SCHEME_KNOW_ALL"},
        {0x062a9010L, "SCHEME_END_ALL"},
        {0x062a7dd0L, "CREATE_ACTIVE_SCHEME"},
        {0x062a8810L, "INITIALIZE_SCHEME"},
        {0x062a8c50L, "INVALIDATE_SCHEME"},
        {0x062a6e30L, "REMOVE_ACTIVE_SCHEME"},
        {0x062a70f0L, "PROCESS_SCHEME_TICK"},
        {0x062a75a0L, "UPDATE_SCHEME_PROGRESS"},
        {0x062a86e0L, "VALIDATE_SCHEME_TARGET"},
        {0x03784840L, "SCHEME_REFDB_LOOKUP"},
        {0x05bb16e0L, "SCHEME_TYPE_NAME_ACCESSOR"},
        {0x05f60090L, "GET_RELATION_MANAGER"},
        {0x061daaf0L, "ADD_RELATION"},
        {0x061db2f0L, "REMOVE_RELATION"},
        {0x066f4f80L, "ADD_PERK"},
        {0x066f4b00L, "ADD_LIFESTYLE_XP"},
        {0x06678b10L, "GET_OPINION_BREAKDOWN"},
        {0x061c5750L, "ACTIVE_OPINION_SUM"},
        {0x06086c10L, "INCOME_CALC_BUDGET"},
        {0x0607b210L, "INCOME_CALC_SUBJECT_TAX"},
        {0x06087be0L, "INCOME_CALC_BARTER_GOODS"},
        {0x060870a0L, "INCOME_OPINION_MODIFIER"},
        {0x030ffdb0L, "SET_FOCUS_CMD"},
        {0x067f53a0L, "SET_FOCUS_APPLY"},
        {0x060874d0L, "INCOME_DOMAIN_CALC"},
        {0x060878d0L, "INCOME_VASSAL_CALC"},
        {0x060887e0L, "INCOME_TRIBUTARY_CALC"},
        {0x06088af0L, "INCOME_MODIFIER_CALC"},
        {0x06674290L, "MONTHLY_INCOME_GET"},
        {0x0577c640L, "LEGITIMACY_GET"},
        {0x06cceb70L, "CB_PREFILTER"},
        {0x06cce6d0L, "CB_VALIDITY_CHECK"},
        {0x06cce220L, "CB_VALIDITY_CORE"},
        {0x0665a030L, "TRIGGER_EVAL"},
        {0x068ba9c0L, "COMPUTE_DOMAIN_LIMIT"},
        {0x06879780L, "COLLECT_REALM_TITLES"},
        {0x061cf380L, "COUNTS_TOWARD_DOMAIN"},
        {0x06088ca0L, "INCOME_EXPENSE_WRAPPER"},
        {0x067327a0L, "INCOME_EXPENSE_COMPUTE"},
        {0x06088cf0L, "INCOME_NET_CALC"},
        {0x0665e550L, "INCOME_BALANCE_WRITER"},
        {0x04cd39f0L, "AI_INCOME_CALC"},
        {0x06681d70L, "ARMY_MAINTENANCE_NOBD"},
        {0x06681f10L, "ARMY_MAINTENANCE_BD"},
        {0x06087be0L, "VASSAL_CONTRIB_CALC"},
        {0x06022330L, "HOLDING_INCOME_CALC"},
        {0x066fceb0L, "YEARLY_STATS_SERIALIZER"},
        {0x0625b0d0L, "REGIMENT_ALIVE_COUNT"},
        {0x0625cfc0L, "REGIMENT_MAINT_COST"},
        {0x0625cf70L, "REGIMENT_BASE_COST"},
        {0x0619fed0L, "MILITARY_STATS_COMPUTE"},
        {0x067c3ac0L, "MILITARY_STRENGTH_GET"},
        {0x0619f380L, "MIL_SUM_MAX_SOLDIERS"},
        {0x0619f3b0L, "MIL_SUM_CURRENT_SOLDIERS"},
        {0x061a0b10L, "REGIMENT_ACCUMULATE_STATS"},
        {0x061a0cb0L, "REGIMENT_PROCESS_MERC"},
        {0x061a0ef0L, "REGIMENT_PROCESS_HOLY"},
        {0x06688570L, "GET_PERSONAL_REGIMENTS"},
        {0x06688d00L, "GET_TITLE_REGIMENTS"},
        {0x0668a910L, "GET_MERC_REGIMENTS"},
        {0x0668a990L, "GET_HOLY_ORDER_REGIMENTS"},
        {0x066885f0L, "GET_MAA_OWNER_LINKS"},
        {0x05750c50L, "ALLIANCE_IS_ALLIED_TO"},
        {0x05752240L, "ALLIANCE_GET_REASONS"},
        {0x05269820L, "ALLIANCE_CREATE"},
        {0x0576df20L, "TRUCE_LIST_ACCESSOR"},
        {0x05660fb0L, "GET_DIPLOMATIC_RELATION"},
        {0x05678a40L, "GET_OR_CREATE_RELATION"},
        {0x057e4390L, "RELATION_BINARY_SEARCH"},
        {0x05269460L, "TRUCE_ADD_ENTRY"},
        {0x052694d0L, "TRUCE_REMOVE_ENTRY"},
        {0x05268b70L, "TRUCE_EXPIRE"},
        {0x05264690L, "TRUCE_REASON_LOOKUP"},
        {0x052646c0L, "TRUCE_REASON_REVERSE"},
        {0x052701a0L, "RELATION_CREATE"},
        {0x052701e0L, "RELATION_ALLOC"},
        {0x05266850L, "RELATION_INIT"},
        {0x052668c0L, "RELATION_POST_INIT"},
        {0x04ec6000L, "TRUCE_ADD_ONEWAY"},
        {0x04ec6b90L, "TRUCE_ADD_BOTHWAYS"},
        {0x04ec7240L, "TRUCE_CANCEL_ONEWAY"},
        {0x04ec75a0L, "TRUCE_CANCEL_BOTHWAYS"},
        {0x0201ed60L, "HOOK_TRIGGER_INIT"},
        {0x020ce6b0L, "HOOK_EFFECT_INIT"},
        {0x0388e340L, "HOOK_GET_HOOK"},
        {0x0388e380L, "HOOK_GET_CHARACTER"},
        {0x0388eef0L, "HOOK_GET_HOOKS_ON_ME"},
        {0x05a6fdd0L, "HOOK_STRENGTH_STATE"},
        {0x05a6fee0L, "HOOK_HAS_EXPIRY"},
        {0x05a6f560L, "HOOK_GET_EXPIRY"},
        {0x05a6fe40L, "HOOK_GET_FRAME"},
        {0x0388f020L, "HOOK_STRONG_COUNT"},
        {0x02fbc260L, "HOOK_DETAIL_INIT"},
        {0x04cb8d40L, "AI_HOOK_EVALUATOR"},
        {0x06cce190L, "CB_VALIDITY_BY_ID"},
        {0x06ccec00L, "CB_EXTENDED_TRIGGER"},
        {0x06cc9920L, "CB_TITLE_VALIDATION"},
        {0x06ccee80L, "CB_SCOPE_BUILDER"},
        {0x065d5490L, "CB_ALREADY_AT_WAR"},
        {0x044969a0L, "CB_UI_POPULATE"},
        {0x06cd0010L, "CB_ENUMERATE_CLAIMS"},
        {0x06cca050L, "CB_TITLE_STANDARD_VALID"},
        {0x043082e0L, "HAS_CB_ON_TRIGGER"},
        {0x065c9360L, "WAR_CREATE_CORE"},
        {0x04d99190L, "AI_POPULATE_CB_CANDIDATES"},
        {0x06731920L, "ARE_AT_WAR"},
        {0x06677aa0L, "ADD_WAR_TO_CHAR"},
        {0x065b6870L, "ADD_WAR_PARTICIPANT"},
        {0x0553d490L, "BUILD_SUCCESSION_ORDER"},
        {0x063f7e40L, "EXECUTE_SUCCESSION"},
        {0x0553b180L, "BUILD_INHERITANCE_SUCC"},
        {0x05527450L, "BUILD_ELECTION_SUCC"},
        {0x0553c660L, "BUILD_APPOINTMENT_SUCC"},
        {0x06401880L, "BUILD_TITLE_HEIR_LIST"},
        {0x0574dbe0L, "GET_HEIR_TITLE_FOR_CHAR"},
        {0x04a63750L, "GET_SUCCESSION_LAW_FROM_TITLE"},
        {0x0578d970L, "GET_DESIGNATED_HEIR"},
        {0x0574a380L, "GET_DESIGNATED_HEIR_REF"},
        {0x05435520L, "COMPARE_SUCCESSION_RULES"},
        {0x054356f0L, "INIT_SUCCESSION_RULE_FROM_TITLE"},
        {0x05401880L, "BUILD_TITLE_HEIR_LIST_R8V6"},
        {0x06734d80L, "REGIMENT_QUERY"},
        {0x06570af0L, "MIL_QUALITY_GLOBAL"},
        {0x060bde00L, "GET_ALLY_CHARACTERS"},
        {0x0676c3d0L, "GET_VASSAL_TITLES"},
        {0x06164f00L, "GARRISON_TROOP_COUNT"},
        {0x06773040L, "IS_LANDED_CHECK"},
        {0x0664adf0L, "IS_IMPRISONED"},
        {0x0664a2e0L, "GET_EMPLOYER_RELATION"},
        {0x0619f3e0L, "MIL_MAX_WITHOUT_HIRES"},
        {0x0619f430L, "MIL_TOTAL_QUALITY_CURRENT"},
        {0x0619f400L, "MIL_TOTAL_QUALITY_MAX"},
        {0x042c54a0L, "AIWATCH_CONSTRUCTOR"},
        {0x042c5b60L, "AIWATCH_DESTRUCTOR"},
        {0x042c3670L, "AIWATCH_POPULATE"},
        {0x042c62a0L, "AIWATCH_BIG_UPDATE"},
        {0x042ca1a0L, "AIWATCH_DISPATCH_CB"},
        {0x042c33a0L, "AIWATCH_ACTIVITY_BUILD"},
        {0x04da0130L, "AI_EVAL_ACTIVITY"},
        {0x04da1aa0L, "AI_EVAL_STRATEGY"},
        {0x05ba8430L, "AI_EVAL_CB"},
        {0x05ba3d60L, "AI_SCOPE_FROM_CHAR"},
        {0x05ba8240L, "AI_WAR_DATA"},
    };

    // Already have signatures for these (from signatures.h)
    static final Set<String> ALREADY_HAVE_SIG = new HashSet<>(Arrays.asList(
        "AI_MANAGER_MAIN_TICK", "AI_MANAGER_POST_EXECUTE", "AI_INTERACTION_DECISION",
        "AI_WAR_TARGET_EVAL", "AI_CHAR_TOGGLE", "STRING_HASH",
        "GET_PLAYER_CHAR_ID", "GAME_COMMAND_DISPATCH", "BUILD_SEND_INTERACTION_CMD",
        "CHAR_IS_ALIVE", "CHAR_IS_RULER", "GET_STRESS", "GET_DREAD",
        "PIETY_GET", "PRESTIGE_GET", "GOLD_GET", "SKILL_GET_BASE",
        "TRAIT_GET_LIST", "GET_FACTION_LIST", "MIL_CURRENT", "MIL_MAX",
        "AI_PERSONALITY_HONOR", "PAUSE_SET", "GET_OPINION_OF",
        "WAR_COMPUTE_SCORE", "INCOME_MODIFIER_CALC", "STRATEGY_EXECUTE",
        "IS_ADULT", "AI_MARRIAGES_DISPATCH", "AI_COURT_ACTIVITY_DISPATCH",
        "AI_GRANT_REVOKE_1", "AI_GRANT_REVOKE_2", "AI_TAX_DISPATCH"
    ));

    private File resolveOutputDir(String[] args) throws Exception {
        String dir = (args.length > 0 && !args[0].isEmpty()) ? args[0] : System.getenv("CK3_RE_OUT");
        File f;
        if (dir != null && !dir.isEmpty()) {
            f = new File(dir);
        } else if (isRunningHeadless()) {
            f = new File(System.getProperty("user.dir"));
        } else {
            f = askDirectory("AutoGenerateSignatures output directory", "Select");
        }
        f.mkdirs();
        return f;
    }

    private static Object[][] readFunctionsCsv(File csv) throws IOException {
        List<Object[]> rows = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(csv))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] parts = line.split(",");
                if (parts.length < 2) throw new IOException("bad line (want NAME,0xOFFSET): " + line);
                String hex = parts[1].trim().toLowerCase();
                if (hex.startsWith("0x")) hex = hex.substring(2);
                rows.add(new Object[] { Long.parseLong(hex, 16), parts[0].trim() });
            }
        }
        return rows.toArray(new Object[0][]);
    }

    @Override
    public void run() throws Exception {
        println("=== AutoGenerateSignatures ===");

        String[] args = getScriptArgs();
        Object[][] funcs = ALL_FUNCS;
        Set<String> haveSig = ALREADY_HAVE_SIG;
        if (args.length > 1 && !args[1].isEmpty()) {
            funcs = readFunctionsCsv(new File(args[1]));
            haveSig = new HashSet<>();
            println("Functions from " + args[1] + ": " + funcs.length);
        }
        final long IMAGE_BASE = currentProgram.getImageBase().getOffset();
        println("Image base: 0x" + Long.toHexString(IMAGE_BASE));

        Memory memory = currentProgram.getMemory();
        Listing listing = currentProgram.getListing();
        AddressSpace addrSpace = currentProgram.getAddressFactory().getDefaultAddressSpace();

        // Find .text section
        MemoryBlock textBlock = null;
        for (MemoryBlock block : memory.getBlocks()) {
            if (block.getName().equals(".text")) {
                textBlock = block;
                break;
            }
        }
        if (textBlock == null) {
            println("ERROR: .text section not found!");
            return;
        }

        long textStart = textBlock.getStart().getOffset();
        long textEnd = textBlock.getEnd().getOffset();
        long textSize = textEnd - textStart + 1;
        println("Text section: 0x" + Long.toHexString(textStart) + " - 0x" + Long.toHexString(textEnd) + " (" + textSize + " bytes)");

        // Read entire .text section into byte array for fast scanning
        println("Reading .text section into memory for scanning...");
        byte[] textBytes = new byte[(int)textSize];
        memory.getBytes(textBlock.getStart(), textBytes);
        println("Text section loaded (" + textBytes.length + " bytes)");

        // Output files
        String outDir = resolveOutputDir(args).getPath() + File.separator;
        PrintWriter sigWriter = new PrintWriter(new FileWriter(outDir + "auto_signatures.txt"));
        PrintWriter summaryWriter = new PrintWriter(new FileWriter(outDir + "auto_signatures_summary.txt"));

        sigWriter.println("// Auto-generated signatures for CK3 bridge functions");
        sigWriter.println("// Generated by AutoGenerateSignatures.java");
        sigWriter.println("//");
        sigWriter.println("// Format: C++ ready for signatures.h");
        sigWriter.println("// Each entry shows: name, file_offset, pattern bytes, mask, match count");
        sigWriter.println("");

        summaryWriter.println("=== Auto Signature Generation Summary ===");
        summaryWriter.println("");

        int totalProcessed = 0;
        int uniqueCount = 0;
        int ambiguousCount = 0;
        int failedCount = 0;
        int skippedCount = 0;

        // Deduplicate by address (some functions share the same address)
        Set<Long> processedAddresses = new HashSet<>();

        for (Object[] entry : funcs) {
            long fileOffset = (Long) entry[0];
            String name = (String) entry[1];

            if (haveSig.contains(name)) {
                skippedCount++;
                continue;
            }

            if (processedAddresses.contains(fileOffset)) {
                skippedCount++;
                continue;
            }
            processedAddresses.add(fileOffset);

            long ghidraAddr = fileOffset + IMAGE_BASE;
            Address funcAddr = addrSpace.getAddress(ghidraAddr);

            // Check if address is in .text
            if (ghidraAddr < textStart || ghidraAddr > textEnd - 32) {
                println("SKIP " + name + " at 0x" + Long.toHexString(fileOffset) + " — outside .text");
                summaryWriter.println("SKIP " + name + " 0x" + Long.toHexString(fileOffset) + " — outside .text");
                failedCount++;
                continue;
            }

            // Read 32 bytes at function start
            byte[] rawBytes = new byte[32];
            try {
                memory.getBytes(funcAddr, rawBytes);
            } catch (Exception e) {
                println("ERROR reading " + name + " at 0x" + Long.toHexString(ghidraAddr) + ": " + e.getMessage());
                summaryWriter.println("ERROR " + name + " 0x" + Long.toHexString(fileOffset) + " — read failed");
                failedCount++;
                continue;
            }

            // Disassemble to find instruction boundaries and RIP-relative/CALL/JMP bytes
            boolean[] isWildcard = new boolean[32];
            int usableLen = 0;

            try {
                Address curAddr = funcAddr;
                while (curAddr.getOffset() < ghidraAddr + 32) {
                    Instruction insn = listing.getInstructionAt(curAddr);
                    if (insn == null) {
                        // Try to get the instruction containing this address
                        insn = listing.getInstructionContaining(curAddr);
                        if (insn == null) {
                            // Can't disassemble further; use what we have
                            break;
                        }
                    }

                    int insnLen = insn.getLength();
                    int insnStart = (int)(curAddr.getOffset() - ghidraAddr);
                    int insnEnd = insnStart + insnLen;

                    if (insnEnd > 32) {
                        // Instruction extends beyond our 32-byte window
                        // Truncate — don't include partial instructions
                        usableLen = insnStart;
                        break;
                    }

                    usableLen = insnEnd;

                    // Check for RIP-relative addressing or branch displacements
                    String mnemonic = insn.getMnemonicString();
                    byte[] insnBytes = insn.getBytes();

                    // Detect CALL rel32 (E8 xx xx xx xx)
                    if (insnLen == 5 && (rawBytes[insnStart] & 0xFF) == 0xE8) {
                        // Wildcard the 4-byte displacement
                        for (int i = insnStart + 1; i < insnStart + 5; i++) {
                            isWildcard[i] = true;
                        }
                    }
                    // Detect JMP rel32 (E9 xx xx xx xx)
                    else if (insnLen == 5 && (rawBytes[insnStart] & 0xFF) == 0xE9) {
                        for (int i = insnStart + 1; i < insnStart + 5; i++) {
                            isWildcard[i] = true;
                        }
                    }
                    // Detect conditional JMP rel32 (0F 8x xx xx xx xx, 6 bytes)
                    else if (insnLen == 6 && (rawBytes[insnStart] & 0xFF) == 0x0F
                             && ((rawBytes[insnStart+1] & 0xF0) == 0x80)) {
                        for (int i = insnStart + 2; i < insnStart + 6; i++) {
                            isWildcard[i] = true;
                        }
                    }
                    // Detect short conditional jumps (7x xx, 2 bytes) — wildcard the offset
                    else if (insnLen == 2 && ((rawBytes[insnStart] & 0xF0) == 0x70 || (rawBytes[insnStart] & 0xFF) == 0xEB)) {
                        isWildcard[insnStart + 1] = true;
                    }
                    // Detect RIP-relative addressing in longer instructions
                    // Look for [RIP+disp32] pattern in instruction operands
                    else if (insnLen >= 5) {
                        // Check if any operand references use RIP-relative addressing
                        // Heuristic: scan the instruction bytes for ModRM with RIP-relative (mod=00, rm=101)
                        // and wildcard the subsequent 4 bytes
                        wildcardRipRelative(rawBytes, insnStart, insnLen, isWildcard);
                    }

                    curAddr = curAddr.add(insnLen);
                }
            } catch (Exception e) {
                // If disassembly fails, just use raw bytes (no wildcards)
                usableLen = 32;
            }

            if (usableLen < 8) {
                println("SKIP " + name + " — too short after disassembly (" + usableLen + " bytes)");
                summaryWriter.println("SKIP " + name + " 0x" + Long.toHexString(fileOffset) + " — only " + usableLen + " bytes");
                failedCount++;
                continue;
            }

            // Cap at usableLen
            int patLen = Math.min(usableLen, 32);

            // Build pattern and mask
            byte[] pattern = new byte[patLen];
            byte[] mask = new byte[patLen];
            System.arraycopy(rawBytes, 0, pattern, 0, patLen);
            for (int i = 0; i < patLen; i++) {
                mask[i] = isWildcard[i] ? (byte)0x00 : (byte)0xFF;
            }

            // Test uniqueness: scan .text section
            int matchCount = scanForPattern(textBytes, pattern, mask, patLen);

            totalProcessed++;

            String status;
            if (matchCount == 1) {
                status = "UNIQUE";
                uniqueCount++;
            } else if (matchCount == 0) {
                status = "NOT_FOUND";
                failedCount++;
            } else {
                status = "AMBIGUOUS(" + matchCount + ")";
                ambiguousCount++;
            }

            // Try extending/shortening for ambiguous patterns
            if (matchCount > 1 && matchCount <= 10) {
                // Try with full 32 bytes (even if partial instruction at end)
                int extLen = 32;
                byte[] extMask = new byte[extLen];
                for (int i = 0; i < extLen; i++) {
                    extMask[i] = (i < patLen) ? mask[i] : (byte)0xFF;
                }
                int extMatch = scanForPattern(textBytes, rawBytes, extMask, extLen);
                if (extMatch == 1) {
                    // Use extended pattern
                    patLen = extLen;
                    pattern = rawBytes.clone();
                    mask = extMask;
                    matchCount = 1;
                    status = "UNIQUE(extended)";
                    uniqueCount++;
                    ambiguousCount--;
                }
            }

            // Output the signature
            println(String.format("%-40s 0x%08x  %s  matches=%d", name, fileOffset, status, matchCount));
            summaryWriter.println(String.format("%-40s 0x%08x  %s  matches=%d", name, fileOffset, status, matchCount));

            // Write C++ format
            sigWriter.println("// " + name + " (0x" + String.format("%08x", fileOffset) + ") — " + status + ", " + matchCount + " match(es)");
            sigWriter.print("static const uint8_t PAT_" + name + "[] = {");
            for (int i = 0; i < patLen; i++) {
                if (i > 0) sigWriter.print(",");
                if (i % 16 == 0) sigWriter.print("\n    ");
                sigWriter.print(String.format("0x%02X", pattern[i] & 0xFF));
            }
            sigWriter.println("\n};");

            sigWriter.print("static const uint8_t MSK_" + name + "[] = {");
            for (int i = 0; i < patLen; i++) {
                if (i > 0) sigWriter.print(",");
                if (i % 16 == 0) sigWriter.print("\n    ");
                sigWriter.print(String.format("0x%02X", mask[i] & 0xFF));
            }
            sigWriter.println("\n};");
            sigWriter.println("");
        }

        // Write summary
        summaryWriter.println("");
        summaryWriter.println("=== TOTALS ===");
        summaryWriter.println("Already have signature (skipped): " + skippedCount);
        summaryWriter.println("Processed:  " + totalProcessed);
        summaryWriter.println("Unique:     " + uniqueCount);
        summaryWriter.println("Ambiguous:  " + ambiguousCount);
        summaryWriter.println("Failed:     " + failedCount);
        summaryWriter.println("Total coverage: " + (uniqueCount + skippedCount - (int)haveSig.size() + (int)haveSig.size()) + " / " + funcs.length);
        summaryWriter.println("New unique sigs: " + uniqueCount);
        summaryWriter.println("Projected auto-recovery: " + String.format("%.1f%%", 100.0 * (uniqueCount + haveSig.size()) / funcs.length));

        sigWriter.close();
        summaryWriter.close();

        println("");
        println("=== SUMMARY ===");
        println("Already have: " + skippedCount);
        println("Processed:    " + totalProcessed);
        println("Unique:       " + uniqueCount);
        println("Ambiguous:    " + ambiguousCount);
        println("Failed:       " + failedCount);
        println("New unique:   " + uniqueCount);
        println("Output: " + outDir + "auto_signatures.txt");
        println("Summary: " + outDir + "auto_signatures_summary.txt");
    }

    /**
     * Scan textBytes for pattern with mask. Returns number of matches.
     * Stops counting at 20 to avoid slow scans.
     */
    private int scanForPattern(byte[] text, byte[] pattern, byte[] mask, int patLen) {
        int matches = 0;
        int limit = text.length - patLen;
        for (int i = 0; i <= limit; i++) {
            boolean match = true;
            for (int j = 0; j < patLen; j++) {
                if ((mask[j] & 0xFF) == 0xFF) {
                    if (text[i + j] != pattern[j]) {
                        match = false;
                        break;
                    }
                }
            }
            if (match) {
                matches++;
                if (matches >= 20) return matches; // early exit
            }
        }
        return matches;
    }

    /**
     * Heuristic: detect ModRM bytes with RIP-relative addressing (mod=00, rm=101)
     * and wildcard the 4-byte displacement that follows.
     *
     * In x86-64, ModRM byte with mod=00 and rm=101 (binary 00_xxx_101) means
     * [RIP + disp32]. The xxx is the reg field (doesn't matter for our purpose).
     */
    private void wildcardRipRelative(byte[] bytes, int insnStart, int insnLen, boolean[] isWildcard) {
        // Walk through instruction bytes looking for ModRM pattern
        // Skip prefix bytes (REX, etc.) and opcode bytes to find ModRM

        int pos = insnStart;
        int end = insnStart + insnLen;

        // Skip REX prefix (0x40-0x4F)
        if (pos < end && (bytes[pos] & 0xF0) == 0x40) {
            pos++;
        }

        // Skip other prefixes (66, F2, F3, 2E, etc.)
        while (pos < end) {
            int b = bytes[pos] & 0xFF;
            if (b == 0x66 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x3E ||
                b == 0x26 || b == 0x36 || b == 0x64 || b == 0x65) {
                pos++;
            } else {
                break;
            }
        }

        if (pos >= end) return;

        int opcodeStart = pos;
        int opcode = bytes[pos] & 0xFF;

        // Two-byte opcode (0F xx)
        if (opcode == 0x0F) {
            pos++;
            if (pos >= end) return;
            opcode = 0x0F00 | (bytes[pos] & 0xFF);
        }

        pos++; // past opcode

        // Check if this opcode has a ModRM byte
        // Most instructions with ModRM: mov, lea, cmp, add, sub, test, etc.
        // Rather than enumerate all, check if remaining bytes could be [RIP+disp32]
        if (pos < end) {
            int modrm = bytes[pos] & 0xFF;
            int mod = (modrm >> 6) & 0x03;
            int rm = modrm & 0x07;

            // RIP-relative: mod=00, rm=101
            if (mod == 0 && rm == 5) {
                // The next 4 bytes after ModRM are the disp32
                int dispStart = pos + 1;
                if (dispStart + 4 <= end && dispStart + 4 <= insnStart + 32) {
                    for (int i = dispStart; i < dispStart + 4; i++) {
                        if (i < 32) {
                            isWildcard[i] = true;
                        }
                    }
                }
            }
        }
    }
}
