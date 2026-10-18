// speed_vector_probe.cpp — THE C++ HALF, DRIVEN BY THE SHARED VECTOR TABLE
// (ck-aqio).
//
// A rule stated in two languages cannot be derived; the only thing that can
// cross that boundary is a case list. So this probe exists to let the SAME FILE
// —  scripts/fixtures/registered_speed_vectors.tsv — reach
// bridge/include/registered_speed.h without a bridge, a game, or a socket.
//
// It links NOTHING: the resolver is a header-only inline, which is what makes
// the shared-table design cheap enough to be run on every gate rather than
// described in a comment.
//
// IT ECHOES THE INPUT IT ACTUALLY RECEIVED — the byte length and a rendered
// form — and that is not decoration. If this file's unescape disagreed with the
// acceptance's, the two halves would be answering DIFFERENT QUESTIONS and could
// agree by coincidence on every row. A harness that cannot prove it ran the
// bytes it was handed has tested nothing.
//
// Build:  g++ -std=c++17 -I bridge/include -o speed_vector_probe \
//             bridge/test/speed_vector_probe.cpp
// Usage:  ./speed_vector_probe scripts/fixtures/registered_speed_vectors.tsv

#include "registered_speed.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// The table's escaping, and it must match the reader on the other side:
//   \t \n \r \\ \s (space) \e (the empty string), UNSET as a bare token.
std::string unescape(const std::string& in, bool* is_unset) {
    *is_unset = (in == "UNSET");
    if (*is_unset) return std::string();
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\' || i + 1 >= in.size()) { out += in[i]; continue; }
        switch (in[++i]) {
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 's': out += ' ';  break;
            case 'e':              break;   // the empty string
            case '\\': out += '\\'; break;
            default:  out += '\\'; out += in[i]; break;
        }
    }
    return out;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream ss(line);
    while (std::getline(ss, cur, '\t')) out.push_back(cur);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: speed_vector_probe <vectors.tsv>\n");
        return 2;
    }
    std::ifstream f(argv[1]);
    if (!f) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    // id \t declared \t speed \t present \t refused \t raw_len \t raw_rendered
    printf("id\tdeclared\tspeed\tpresent\trefused\traw_len\traw_rendered\n");
    std::string line;
    int rows = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> cols = split_tabs(line);
        if (cols.size() < 2) continue;
        bool is_unset = false;
        const std::string raw = unescape(cols[1], &is_unset);
        const bridge::GameSpeedDeclaration d =
            is_unset ? bridge::resolve_game_speed(nullptr)
                     : bridge::resolve_game_speed(raw.c_str());
        printf("%s\t%d\t%d\t%d\t%s\t%zu\t%s\n",
               cols[0].c_str(), d.declared ? 1 : 0, d.speed, d.present ? 1 : 0,
               bridge::render_bytes(d.refused).c_str(),
               is_unset ? static_cast<size_t>(0) : raw.size(),
               is_unset ? "UNSET" : bridge::render_bytes(raw).c_str());
        ++rows;
    }
    if (rows == 0) {
        fprintf(stderr, "the table yielded NO rows — refusing to report a "
                        "green from an empty case list\n");
        return 3;
    }
    return 0;
}
