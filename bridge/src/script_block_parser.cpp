// script_block_parser.cpp — see script_block_parser.h for purpose / scope.

#include "script_block_parser.h"

#include <cstdlib>
#include <cstring>

namespace bridge {

namespace {

inline bool is_pdx_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == ':' || c == '-';
}

inline void skip_ws(const char*& p) {
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        } else if (*p == '#') {
            while (*p && *p != '\n') p++;
        } else {
            break;
        }
    }
}

bool read_ident(const char*& p, std::string& out) {
    skip_ws(p);
    const char* start = p;
    while (*p && is_pdx_ident_char(*p)) p++;
    if (p == start) return false;
    out.assign(start, static_cast<std::size_t>(p - start));
    return true;
}

bool read_value(const char*& p, std::string& out) {
    skip_ws(p);
    if (*p == '"') {
        p++;
        const char* start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        if (*p != '"') return false;
        out.assign(start, static_cast<std::size_t>(p - start));
        p++;
        return true;
    }
    if (*p == '{') {
        int depth = 1;
        p++;
        const char* start = p;
        while (*p && depth > 0) {
            if (*p == '{') {
                depth++;
            } else if (*p == '}') {
                depth--;
                if (depth == 0) break;
            }
            p++;
        }
        if (depth != 0) return false;
        out.assign(start, static_cast<std::size_t>(p - start));
        p++;
        return true;
    }
    return read_ident(p, out);
}

}  // namespace

ScriptBlock parse_script_block(const char* text) {
    ScriptBlock r;
    if (!text) {
        r.error = "null_text";
        return r;
    }
    const char* base = text;
    const char* p = text;

    if (!read_ident(p, r.effect_name)) {
        r.error = "expected_effect_name";
        r.error_offset = static_cast<std::size_t>(p - base);
        return r;
    }
    skip_ws(p);
    if (*p != '=') {
        r.error = "expected_equals_after_effect_name";
        r.error_offset = static_cast<std::size_t>(p - base);
        return r;
    }
    p++;
    skip_ws(p);
    if (*p != '{') {
        // Parameterless invocation form `<effect> = yes` (the canonical PDX
        // way to fire a scripted_effect with no args). Accept it as a valid
        // block with an empty params map; anything else here is malformed.
        std::string bare;
        if (read_ident(p, bare) && bare == "yes") {
            skip_ws(p);
            if (*p != '\0') {
                r.error = "trailing_content_after_block";
                r.error_offset = static_cast<std::size_t>(p - base);
                return r;
            }
            r.valid = true;  // params left empty
            return r;
        }
        r.error = "expected_open_brace";
        r.error_offset = static_cast<std::size_t>(p - base);
        return r;
    }
    p++;

    while (true) {
        skip_ws(p);
        if (*p == '}') {
            p++;
            skip_ws(p);
            if (*p != '\0') {
                r.error = "trailing_content_after_block";
                r.error_offset = static_cast<std::size_t>(p - base);
                return r;
            }
            r.valid = true;
            return r;
        }
        if (!*p) {
            r.error = "unexpected_eof_in_block";
            r.error_offset = static_cast<std::size_t>(p - base);
            return r;
        }
        std::string key;
        if (!read_ident(p, key)) {
            r.error = "expected_key";
            r.error_offset = static_cast<std::size_t>(p - base);
            return r;
        }
        skip_ws(p);
        if (*p != '=') {
            r.error = "expected_equals_after_key";
            r.error_offset = static_cast<std::size_t>(p - base);
            return r;
        }
        p++;
        std::string val;
        if (!read_value(p, val)) {
            r.error = "expected_value";
            r.error_offset = static_cast<std::size_t>(p - base);
            return r;
        }
        r.params[std::move(key)] = std::move(val);
    }
}

std::string strip_ref_prefix(const std::string& s, const char* prefix) {
    if (!prefix) return s;
    std::size_t plen = std::strlen(prefix);
    if (s.size() > plen && std::strncmp(s.c_str(), prefix, plen) == 0) {
        return s.substr(plen);
    }
    return s;
}

bool parse_int_strict(const std::string& s, int* out) {
    if (s.empty() || !out) return false;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    *out = static_cast<int>(v);
    return true;
}

}  // namespace bridge
