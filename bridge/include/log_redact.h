// log_redact.h — what a raw wire frame may put into the bridge log.
//
// 2026.12.5 (the draft smoke, smoke5): ipc_server.cpp logged every handshake
// verbatim ("Handshake received (slot=N): <line>"), and a handshake carries the
// RESUME TOKEN — server-minted, "an authorisation over a claim set" (session.h),
// the one credential that lets a client take a seat's rulers. The house rule is
// that a token never reaches a log. The Python side already withholds it from
// the ack it logs and keeps (bridge_client.HANDSHAKE_ACK_WITHHELD); this is the
// bridge side, used at every raw-frame log site (ipc_server.cpp "Handshake
// received", dispatch_router.cpp "Action: ... params:").
//
// THE RULE (round 4, after the r3 witness's adversarial frames):
//   * A SENSITIVE KEY is one whose DECODED name (\u escapes and the other JSON
//     escapes resolved, as RapidJSON resolves them), lower-cased with `_`, `-`
//     and spaces dropped, is "auth", or contains "authoriz", "token", "secret",
//     "passw", "apikey" or "credential". So resume_token, resumeToken,
//     RESUME_TOKEN, "resume_token", Authorization and api_key all are.
//   * Its WHOLE VALUE, whatever its type (string, number, bool, null, array,
//     object), is replaced by "<redacted>".
//   * A string value that is itself JSON (an object or array encoded in a
//     string) is redacted whole when the same rule finds a secret inside it.
//   * Everything else is kept byte for byte, so the log still says who
//     handshook and how.
//   * A frame RapidJSON does not accept (truncated, a missing colon, a comment,
//     a NUL) is not scanned by guesswork: it is CUT before its first
//     sensitive-looking byte (any sensitive stem above, in any case, or a `\u`
//     escape that could spell one) and marked "<redacted: unparsable frame>".
//     A malformed frame with nothing sensitive-looking in it is logged as is.
// Some keys that match are not secrets (usage counts such as input_tokens):
// their values leave the LOG too. The rule errs that way on purpose.
#pragma once

#include <rapidjson/document.h>

#include <cctype>
#include <cstdint>
#include <string>

namespace bridge {

constexpr const char kLogRedacted[] = "<redacted>";
constexpr const char kLogRedactedFrame[] = "<redacted: unparsable frame>";

namespace log_redact_detail {

inline std::string fold(const std::string& s) {
    std::string k;
    k.reserve(s.size());
    for (char c : s) {
        if (c == '_' || c == '-' || c == ' ') continue;
        k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return k;
}

inline bool sensitive_key(const std::string& decoded) {
    const std::string k = fold(decoded);
    return k == "auth" || k.find("authoriz") != std::string::npos ||
           k.find("token") != std::string::npos || k.find("secret") != std::string::npos ||
           k.find("passw") != std::string::npos || k.find("apikey") != std::string::npos ||
           k.find("credential") != std::string::npos;
}

inline int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline void put_utf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) { out->push_back(static_cast<char>(cp)); return; }
    if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    }
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
}

// `s[i]` is '"'. Returns the index just past the closing quote (npos when the
// string runs off the end) and the decoded body in *dec.
inline size_t scan_string(const std::string& s, size_t i, std::string* dec) {
    size_t j = i + 1;
    const size_t n = s.size();
    while (j < n) {
        const char c = s[j];
        if (c == '"') return j + 1;
        if (c != '\\') { dec->push_back(c); ++j; continue; }
        if (j + 1 >= n) return std::string::npos;
        const char e = s[j + 1];
        switch (e) {
            case 'b': dec->push_back('\b'); break;
            case 'f': dec->push_back('\f'); break;
            case 'n': dec->push_back('\n'); break;
            case 'r': dec->push_back('\r'); break;
            case 't': dec->push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                for (int k = 2; k < 6; ++k) {
                    const int h = (j + k < n) ? hexval(s[j + k]) : -1;
                    if (h < 0) return std::string::npos;
                    cp = (cp << 4) | static_cast<uint32_t>(h);
                }
                j += 6;
                if (cp >= 0xD800 && cp < 0xDC00 && j + 5 < n && s[j] == '\\' && s[j + 1] == 'u') {
                    uint32_t lo = 0;
                    bool ok = true;
                    for (int k = 2; k < 6; ++k) {
                        const int h = hexval(s[j + k]);
                        if (h < 0) { ok = false; break; }
                        lo = (lo << 4) | static_cast<uint32_t>(h);
                    }
                    if (ok && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        j += 6;
                    }
                }
                put_utf8(dec, cp);
                continue;
            }
            default: dec->push_back(e); break;     // \" \\ \/ and anything else literal
        }
        j += 2;
    }
    return std::string::npos;
}

inline size_t skip_ws(const std::string& s, size_t i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

// The end of the JSON value starting at s[i] (npos if it runs off the end).
// Iterative: a deep value cannot overflow the stack.
inline size_t skip_value(const std::string& s, size_t i) {
    const size_t n = s.size();
    if (i >= n) return std::string::npos;
    if (s[i] == '"') { std::string d; return scan_string(s, i, &d); }
    if (s[i] == '{' || s[i] == '[') {
        long depth = 0;
        size_t j = i;
        while (j < n) {
            const char c = s[j];
            if (c == '"') {
                std::string d;
                j = scan_string(s, j, &d);
                if (j == std::string::npos) return j;
                continue;
            }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (--depth == 0) return j + 1; }
            ++j;
        }
        return std::string::npos;
    }
    size_t j = i;
    while (j < n && s[j] != ',' && s[j] != '}' && s[j] != ']' &&
           !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
    return j;
}

inline bool parses(const std::string& s) {
    rapidjson::Document d;
    d.Parse<rapidjson::kParseIterativeFlag>(s.data(), s.size());
    return !d.HasParseError();
}

// The first byte of `s` that could begin a secret, npos when none.
inline size_t first_sensitive_looking(const std::string& s) {
    std::string low(s.size(), '\0');
    for (size_t i = 0; i < s.size(); ++i)
        low[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    size_t best = std::string::npos;
    for (const char* stem : {"auth", "token", "secret", "passw", "api_key", "apikey", "api-key",
                             "credential", "\\u"}) {
        const size_t p = low.find(stem);
        if (p < best) best = p;
    }
    return best;
}

inline std::string redact(const std::string& line, int depth);

// A frame RapidJSON refused: cut it before anything sensitive-looking.
inline std::string conservative(const std::string& line) {
    const size_t p = first_sensitive_looking(line);
    if (p == std::string::npos) return line;
    return line.substr(0, p) + kLogRedactedFrame;
}

// Scan a frame RapidJSON accepted. Byte-faithful except redacted values.
inline std::string scan(const std::string& line, int depth) {
    std::string out;
    out.reserve(line.size());
    const size_t n = line.size();
    size_t i = 0;
    while (i < n) {
        if (line[i] != '"') { out.push_back(line[i]); ++i; continue; }
        std::string dec;
        const size_t end = scan_string(line, i, &dec);
        if (end == std::string::npos) return out + kLogRedactedFrame;   // cannot happen on valid JSON
        const size_t k = skip_ws(line, end);
        if (k < n && line[k] == ':') {
            out.append(line, i, k + 1 - i);                 // the key, its spacing, the colon
            const size_t v = skip_ws(line, k + 1);
            out.append(line, k + 1, v - (k + 1));
            if (sensitive_key(dec)) {
                const size_t ve = skip_value(line, v);
                out += '"';
                out += kLogRedacted;
                out += '"';
                if (ve == std::string::npos) return out;   // runs off the end: drop the rest
                i = ve;
            } else {
                i = v;
            }
            continue;
        }
        // a string VALUE: redacted whole when it is JSON that hides a secret
        const size_t f = skip_ws(dec, 0);
        if (depth < 4 && f < dec.size() && (dec[f] == '{' || dec[f] == '[') &&
            redact(dec, depth + 1) != dec) {
            out += '"';
            out += kLogRedacted;
            out += '"';
        } else {
            out.append(line, i, end - i);
        }
        i = end;
    }
    return out;
}

inline std::string redact(const std::string& line, int depth) {
    return parses(line) ? scan(line, depth) : conservative(line);
}

}  // namespace log_redact_detail

inline std::string redact_for_log(const std::string& line) {
    return log_redact_detail::redact(line, 0);
}

}  // namespace bridge
