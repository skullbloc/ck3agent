// pdx_parser.h — minimal Paradox-script brace tokenizer + a landed_titles
// walker specialized for de-jure hierarchy extraction.
//
// Handles: UTF-8 BOM, '#' line comments, @vars (skipped), nested { } blocks,
// hsv{ } and `{R G B}` color forms (with or without a space after the brace),
// and opaque non-title sub-blocks (can_create={…}, ai_primary_priority={…}).
//
// The walker only recurses into blocks whose key looks like a title
// (prefix e_/k_/d_/c_/b_); everything else with a `{` is skipped wholesale.

#ifndef PDX_PARSER_H
#define PDX_PARSER_H

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace pdx {

// A token stream over a script file. Tokens are: identifiers/values,
// '{', '}', '='. Comments and whitespace are stripped.
struct Lexer {
    const char* p;
    const char* end;

    explicit Lexer(const std::string& s) {
        p = s.data();
        end = s.data() + s.size();
        // skip UTF-8 BOM
        if (end - p >= 3 && (unsigned char)p[0] == 0xEF &&
            (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
            p += 3;
        }
    }

    void skip_ws_comments() {
        for (;;) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' ||
                               *p == '\n')) {
                ++p;
            }
            if (p < end && *p == '#') {
                while (p < end && *p != '\n') ++p;
                continue;
            }
            break;
        }
    }

    // Returns next token text; sets is_punct for { } =. Empty string at EOF.
    std::string next(char& punct) {
        punct = 0;
        skip_ws_comments();
        if (p >= end) return {};
        char c = *p;
        if (c == '{' || c == '}' || c == '=') {
            punct = c;
            ++p;
            return std::string(1, c);
        }
        if (c == '"') {
            ++p;
            const char* s = p;
            while (p < end && *p != '"') ++p;
            std::string tok(s, p - s);
            if (p < end) ++p; // closing quote
            return tok;
        }
        // bareword: until whitespace, punct, or comment
        const char* s = p;
        while (p < end) {
            char d = *p;
            if (d == ' ' || d == '\t' || d == '\r' || d == '\n' ||
                d == '{' || d == '}' || d == '=' || d == '#') {
                break;
            }
            ++p;
        }
        return std::string(s, p - s);
    }
};

inline bool is_title_key(const std::string& k) {
    if (k.size() < 3) return false;
    char t = k[0];
    return (t == 'e' || t == 'k' || t == 'd' || t == 'c' || t == 'b') &&
           k[1] == '_';
}

inline uint8_t tier_of(const std::string& k) {
    switch (k[0]) {
        case 'b': return 0;
        case 'c': return 1;
        case 'd': return 2;
        case 'k': return 3;
        case 'e': return 4;
    }
    return 255;
}

} // namespace pdx

#endif // PDX_PARSER_H
