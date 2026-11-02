// engine_text.h — the game's rendered tooltip text, made readable (cleanup 2026.11, F16).
//
// The engine's reason strings (FACTION_CAN_CREATE and its siblings append them to a
// std::string; re/findings/re_factions.md §1) are RENDERED text with the game's
// markup still in it, read live on the 867 save:
//
//   \x15<code> ... \x15!   a format span; <code> runs to the first space
//                           (X, L, high, E;, ONCLICK:CHARACTER,12139,
//                           TOOLTIP:GAME_CONCEPT,crown_authority, ...)
//   \x15indent_newline:N    a line break in the tooltip
//   \x16<icon>!             an inline icon
//
// clean_engine_text keeps the words, turns each line break (indent_newline, and
// the "\n" the game puts before each appended reason) into "; " and drops
// the rest. Two rules on the lines (L12, read on the node's 867 save):
//   - a compound header ("All of these:", any line ending in ':') is kept only
//     when a deeper line follows it. The game renders a compound trigger's
//     failing children through the trigger's own tooltip key (FUN_0763e6b0); a
//     child with no key or no localisation renders nothing, so the reason string
//     the bridge receives can be the header alone (the imprison check's
//     "All of these:", w-l10-factions-6; imperial_policy_faction's
//     "...; All of these:"). A header with nothing under it says nothing.
//   - a line that repeats an earlier one is dropped: FACTION_CAN_CREATE_AS_COMMAND
//     (FUN_05fa3460) runs three checks into one string and two of them test the
//     Ceremonial Monarch condition, so restore_ceremonial_liege_faction's reason
//     read "X has a Ceremonial Monarch; X has a Ceremonial Monarch".
// An empty result means the game gave no reason; the caller says so ("the game
// gives no detail"). Pure, no engine dependency: ck3agent/server/test_engine_text.py
// compiles it against the recorded live strings.
#pragma once

#include <string>
#include <vector>

namespace bridge {

inline std::string clean_engine_text(const std::string& in) {
    struct Line { int level; std::string text; };
    std::vector<Line> lines;
    std::string out;            // the line being read
    int level = 0, next_level = 0;
    auto finish = [&]() {
        while (!out.empty() && out.back() == ' ') out.pop_back();
        size_t s0 = 0;
        while (s0 < out.size() && out[s0] == ' ') ++s0;
        if (s0 < out.size()) lines.push_back({level, out.substr(s0)});
        out.clear();
        level = next_level;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '\x15') {                       // format span
            if (i + 1 < in.size() && in[i + 1] == '!') { ++i; continue; }
            // the code runs to the first space (dropped with it) or to the next
            // markup byte (kept), whichever comes first
            size_t end = in.find_first_of(std::string(" \x15\x16", 3), i + 1);
            if (end == std::string::npos) end = in.size();
            if (in.compare(i + 1, 14, "indent_newline") == 0) {
                next_level = 0;
                if (i + 16 <= end && in[i + 15] == ':')
                    for (size_t k = i + 16; k < end && in[k] >= '0' && in[k] <= '9'; ++k)
                        next_level = next_level * 10 + (in[k] - '0');
                finish();
            }
            i = (end < in.size() && in[end] == ' ') ? end : end - 1;
            continue;
        }
        if (c == '\x16') {                       // inline icon
            size_t end = in.find('!', i + 1);
            i = (end == std::string::npos) ? in.size() : end;
            continue;
        }
        if (c == '\n') { next_level = 0; finish(); continue; }  // the game's own reason
        // break: CAN_FIRE_ULTIMATUM and its siblings append each reason after "\n"
        char d = (c == '\r' || c == '\t') ? ' ' : c;
        if (static_cast<unsigned char>(d) < 0x20) continue;
        if (d == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += d;
    }
    finish();
    // A line's own trailing/leading ';' are separators the game left in the text.
    for (auto& l : lines) {
        while (!l.text.empty() && (l.text.back() == ' ' || l.text.back() == ';')) l.text.pop_back();
        size_t s0 = 0;
        while (s0 < l.text.size() && (l.text[s0] == ' ' || l.text[s0] == ';')) ++s0;
        l.text.erase(0, s0);
    }
    // From the last line back: a header is kept only when a kept line sits under
    // it (deeper, before the next line at its own level or shallower).
    std::vector<char> keep(lines.size(), 0);
    for (size_t k = lines.size(); k-- > 0;) {
        const Line& l = lines[k];
        if (l.text.empty()) continue;
        if (l.text.back() != ':') { keep[k] = 1; continue; }
        for (size_t n = k + 1; n < lines.size() && lines[n].level > l.level; ++n)
            if (keep[n]) { keep[k] = 1; break; }
    }
    std::string joined;
    std::vector<std::string> kept;
    for (size_t k = 0; k < lines.size(); ++k) {
        const Line& l = lines[k];
        if (!keep[k]) continue;
        bool seen = false;
        for (const auto& t : kept) if (t == l.text) { seen = true; break; }
        if (seen) continue;
        kept.push_back(l.text);
        if (!joined.empty()) joined += "; ";
        joined += l.text;
    }
    return joined;
}

}  // namespace bridge
