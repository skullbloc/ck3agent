// speed_read_probe.cpp — ck-k57a. The BRIDGE's own emission, on a host.
//
// WHY THIS EXISTS. The python acceptance needs `get_tick_info` payloads for a
// faulted speed read, and CK3 is not running on the host lane. A python model
// of what the bridge emits would be testing the model. This probe links the
// SAME `bridge::speed::emit` the live handler calls, so the fixture it prints
// is the handler's own bytes.
//
// Built TWICE. Without `CK3_SPEED_PREFIX_CONTROL` it is the shipping handler.
// With it, the emission reverts to the pre-fix arithmetic restored verbatim
// from query_handlers.cpp @ 899d328 — the negative control, as a binary. A
// control that fails to COMPILE is a control doing its job.
//
// Usage:  speed_read_probe <root_ok:0|1> <faulted:0|1> <raw>
// Prints one JSON object per argument triple, one per line.

#include "speed_read.h"

#include <cstdio>
#include <cstdlib>
#include <string>

struct StubBuilder {
    std::string buf;
    void key(const char* k)          { buf += '"'; buf += k; buf += "\":"; }
    void value_int(long long v)      { buf += std::to_string(v); buf += ','; }
    void value_null()                { buf += "null,"; }
    void value_string(const char* v) { buf += '"'; buf += v; buf += "\","; }
    std::string take() const {
        std::string s = buf;
        if (!s.empty() && s.back() == ',') s.pop_back();
        return "{" + s + "}";
    }
};

int main(int argc, char** argv) {
    if (argc < 4 || ((argc - 1) % 3) != 0) {
        fprintf(stderr, "usage: %s <root_ok> <faulted> <raw> [...]\n", argv[0]);
        return 2;
    }
    for (int i = 1; i + 2 < argc; i += 3) {
        const bool    root_ok = atoi(argv[i]) != 0;
        const bool    faulted = atoi(argv[i + 1]) != 0;
        const int32_t raw     = static_cast<int32_t>(atoi(argv[i + 2]));
        StubBuilder b;
#ifdef CK3_SPEED_PREFIX_CONTROL
        // THE PRE-FIX HANDLER. Note what it does NOT look at: `faulted`. That
        // is the defect, in one line — the fault information existed (the
        // counter was already being bumped) and the emission site never
        // consulted it.
        (void)faulted;
        b.key("game_speed");
        if (root_ok) b.value_int(raw + 1);
        else         b.value_int(0);
#else
        bridge::speed::emit(b, bridge::speed::classify(root_ok, faulted, raw));
#endif
        printf("%s\n", b.take().c_str());
    }
    return 0;
}
