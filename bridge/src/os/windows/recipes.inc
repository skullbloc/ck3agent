// recipes.inc — game functions the Windows bridge supplies itself (ADR 0001 P5).
//
// ONE list, two readers: re/scripts/windows/gen_win_offsets.py marks each name's
// RVA in the generated Windows ck3_offsets.h as CK3_WIN_RECIPE(<line order>), and
// os/windows/recipes.cpp builds the table bridge::game_fn() returns from, in the
// same order. A recipe is used ONLY when the function has no Windows anchor (the
// generator never marks a win_rva). Each recipe is the Linux function's body,
// read off the decompile, over offsets measured on the Windows build; the
// evidence is beside it in recipes.cpp. Append only (the index is the order).
RECIPE(GET_AGE)
RECIPE(GET_AI_HONOR)
RECIPE(GET_AI_GREED)
RECIPE(GET_AI_RATIONALITY)
RECIPE(GET_AI_ENERGY)
RECIPE(GET_AI_BOLDNESS)
RECIPE(GET_AI_ZEAL)
RECIPE(GET_AI_VENGEFULNESS)
RECIPE(GET_AI_COMPASSION)
RECIPE(GET_AI_SOCIABILITY)
RECIPE(ARMY_IS_RETREATING)
RECIPE(GET_TRUCE_LIST)
RECIPE(STRING_HASH)
RECIPE(CHAR_GET_AI_CONTROLLER)
RECIPE(SIMD_FIND_LONG)
RECIPE(ARMY_ENUM_SUB)
RECIPE(GET_OPINION_OF)
RECIPE(ARMY_IN_COMBAT)
RECIPE(AI_CHAR_TOGGLE)
RECIPE(CB_VALIDITY_CHECK)
RECIPE(WAR_SCORE_LANDLESS_THUNK)
RECIPE(INTERACTION_CTX_RESET)
RECIPE(INTERACTION_CTX_SET_TARGET)
RECIPE(BUILD_SEND_CMD_FROM_BUF)
