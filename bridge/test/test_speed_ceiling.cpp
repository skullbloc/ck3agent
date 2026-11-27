// test_speed_ceiling.cpp — the speed write's ceiling.
//
// The speed primitive under clock_write caps a write at 3, whoever asks (a
// free-running agent loop lets deaths and wars resolve between its cycles at 4-5).
// A stepped episode that DECLARES a faster speed through CK3_EPISODE_GAME_SPEED
// raises the ceiling to the speed it declared; nothing else moves: an undeclared world, a refused
// declaration and every declaration of 1..3 (the declared pin is 2) cap
// at 3 exactly as before.
//
// Two halves: the pure rule (registered_speed.h clamp_speed_write), and a
// structural row that the engine primitive calls it (it cannot run off the
// game; a live check proves the
// written speed reads back).

#include "registered_speed.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_fail = 0;
static int g_rows = 0;

static void expect(const char* what, int got, int want) {
    ++g_rows;
    if (got != want) {
        ++g_fail;
        std::printf("FAIL %s: got %d, want %d\n", what, got, want);
    }
}

int main() {
    using bridge::clamp_speed_write;
    using bridge::resolve_game_speed;

    const auto unset = resolve_game_speed(nullptr);
    for (int s = 1; s <= 3; ++s) expect("undeclared, at or below 3, passes", clamp_speed_write(s, unset), s);
    expect("undeclared 4 caps at 3", clamp_speed_write(4, unset), 3);
    expect("undeclared 5 caps at 3", clamp_speed_write(5, unset), 3);
    expect("below 1 is 1", clamp_speed_write(0, unset), 1);

    const auto pin = resolve_game_speed("2");     // the declared pin
    expect("declared 2: 2 passes", clamp_speed_write(2, pin), 2);
    expect("declared 2: 5 caps at 3 (unchanged)", clamp_speed_write(5, pin), 3);
    expect("declared 3: 5 caps at 3", clamp_speed_write(5, resolve_game_speed("3")), 3);

    const auto four = resolve_game_speed("4");
    expect("declared 4: 4 passes", clamp_speed_write(4, four), 4);
    expect("declared 4: 5 caps at 4", clamp_speed_write(5, four), 4);
    expect("declared 4: 3 passes", clamp_speed_write(3, four), 3);

    const auto five = resolve_game_speed("5");
    expect("declared 5: 5 passes", clamp_speed_write(5, five), 5);
    expect("declared 5: 4 passes", clamp_speed_write(4, five), 4);

    expect("refused declaration '9': 5 caps at 3", clamp_speed_write(5, resolve_game_speed("9")), 3);
    expect("refused declaration ' 5': 5 caps at 3", clamp_speed_write(5, resolve_game_speed(" 5")), 3);
    expect("empty declaration: 5 caps at 3", clamp_speed_write(5, resolve_game_speed("")), 3);

    // The primitive's write goes through the rule: the call statement is in
    // speed_primitive's body, and the old literal cap is not.
    std::ifstream f(SPEED_CEILING_EVENT_HANDLER);
    std::stringstream ss; ss << f.rdbuf();
    const std::string src = ss.str();
    const size_t a = src.find("static void speed_primitive(int speed) {");
    const size_t b = a == std::string::npos ? a : src.find("\n}\n", a);
    ++g_rows;
    if (a == std::string::npos || b == std::string::npos) {
        ++g_fail;
        std::printf("FAIL speed_primitive not found in %s\n", SPEED_CEILING_EVENT_HANDLER);
    } else {
        const std::string body = src.substr(a, b - a);
        ++g_rows;
        if (body.find("speed = bridge::clamp_speed_write(speed, bridge::resolve_game_speed_from_env());")
                == std::string::npos) {
            ++g_fail;
            std::printf("FAIL speed_primitive does not clamp through clamp_speed_write\n");
        }
        ++g_rows;
        if (body.find("if (speed > 3)") != std::string::npos) {
            ++g_fail;
            std::printf("FAIL speed_primitive still carries the literal cap\n");
        }
    }

    std::printf("%d rows, %d failed\n", g_rows, g_fail);
    return g_fail ? 1 : 0;
}
