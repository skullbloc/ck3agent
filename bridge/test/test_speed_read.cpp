// test_speed_read.cpp — ck-k57a. NO GAME. The classifier is a pure function of
// three facts, so every case including the memory fault is reachable host-side.
//
// THE CLAIM: a read that did not happen cannot leave this code wearing a legal
// speed. The `+ 1` that turned an abstention into a "speed 1" is applied on the
// OK path and nowhere else, and the negative control at the bottom — the
// pre-fix arithmetic, restored verbatim — is required to FAIL that claim.

#include "speed_read.h"

#include <cstdio>
#include <string>

using namespace bridge::speed;

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool ok, const char* what, const std::string& detail = "") {
    g_checks++;
    if (!ok) g_fails++;
    printf("  %-4s %s%s%s\n", ok ? "ok" : "FAIL", what,
           detail.empty() ? "" : " — ", detail.c_str());
}

// A twenty-line stand-in for ResponseBuilder, so `emit()` is exercised as the
// live handler exercises it rather than being inspected field by field.
struct StubBuilder {
    std::string buf;
    void key(const char* k)          { buf += '"'; buf += k; buf += "\":"; }
    void value_int(long long v)      { buf += std::to_string(v); buf += ','; }
    void value_null()                { buf += "null,"; }
    void value_string(const char* v) { buf += '"'; buf += v; buf += "\","; }
};

static std::string emitted(const Reading& r) {
    StubBuilder b;
    emit(b, r);
    return b.buf;
}

int main() {
    printf("test_speed_read (ck-k57a): absence is a different TYPE than a value\n");

    // ── the OK path is unchanged, which is half the bead ───────────────────
    printf("\nA READ THAT HAPPENED still reports exactly what it used to\n");
    for (int32_t raw = 0; raw <= 4; ++raw) {
        Reading r = classify(true, false, raw);
        char what[96];
        snprintf(what, sizeof(what), "raw %d -> speed %d, status ok", raw, raw + 1);
        check(r.ok() && r.has_speed && r.speed == raw + 1, what,
              "speed=" + std::to_string(r.speed));
    }
    check(std::string(status_name(classify(true, false, 1).status)) == "ok",
          "the OK status spells itself `ok`");
    check(emitted(classify(true, false, 1)) ==
              "\"game_speed\":2,\"speed_read_status\":\"ok\",\"game_speed_legacy\":2,",
          "a real speed 2 goes on the wire as the NUMBER 2",
          emitted(classify(true, false, 1)));

    // ── THE BEAD: a faulted read is not a speed ────────────────────────────
    printf("\nA FAULTED READ carries no speed, and cannot be mistaken for one\n");
    {
        // raw is 0 because that is what safe_read returns on failure — the
        // exact input that used to become a 1.
        Reading r = classify(true, /*speed_faulted=*/true, /*raw=*/0);
        check(!r.has_speed, "has_speed is FALSE — there is no speed to read");
        check(r.status == ReadStatus::ReadFaulted, "status is read_faulted");
        check(std::string(status_name(r.status)) == "read_faulted",
              "it spells itself `read_faulted`");
        check(!r.ok(), "ok() is false");
        const std::string wire = emitted(r);
        check(wire.find("\"game_speed\":null,") != std::string::npos,
              "game_speed goes on the wire as JSON null", wire);
        check(wire.find("\"game_speed\":1,") == std::string::npos,
              "and specifically NOT as 1 — the whole defect", wire);
        check(wire.find("\"game_speed\":0,") == std::string::npos,
              "and not as 0 either: 0 is an integer in the speeds' value space",
              wire);
        check(wire.find("\"speed_read_status\":\"read_faulted\"") != std::string::npos,
              "the status names WHICH absence this is", wire);
    }
    {
        // A fault does not stop at raw == 0: whatever junk safe_read left in
        // the destination, a read that did not happen yields no speed.
        for (int32_t raw : {0, 1, 4, -7, 99}) {
            Reading r = classify(true, true, raw);
            char what[96];
            snprintf(what, sizeof(what), "faulted with raw %d still yields no speed", raw);
            check(!r.has_speed && r.speed == 0, what);
        }
    }

    // ── the root case: a DIFFERENT absence, and it stays different ─────────
    printf("\nAN UNREADABLE ROOT is a second absence, not the same one\n");
    {
        Reading r = classify(false, false, 3);
        check(!r.has_speed, "no speed");
        check(std::string(status_name(r.status)) == "root_unreadable",
              "status is root_unreadable");
        check(status_name(classify(false, false, 0).status) !=
                  std::string(status_name(classify(true, true, 0).status)),
              "and it is NOT the same status as a fault — the bridge problem "
              "does not hide inside the world state");
        check(emitted(r).find("\"game_speed\":null,") != std::string::npos,
              "it is null on the wire too");
        // root_ok == false wins over a faulted flag: with no root there was
        // never an address to fault on.
        check(classify(false, true, 0).status == ReadStatus::RootUnreadable,
              "no root beats a fault flag — there was nothing to read from");
    }

    // ── the compat shape, stated once and checked ──────────────────────────
    printf("\nTHE COMPAT SHAPE: the old integer contract, with the bug removed\n");
    check(classify(true, false, 1).legacy_int() == 2,
          "a real speed still reads 2 on game_speed_legacy");
    check(classify(false, false, 0).legacy_int() == 0,
          "an unreadable root still reads 0 — byte-identical to pre-bead");
    check(classify(true, true, 0).legacy_int() == 0,
          "and a FAULT now reads 0 as well, which is the fix a legacy "
          "integer consumer gets for free");

    // ── NEGATIVE CONTROL. It runs, and it must fail the first claim. ───────
    printf("\nNEGATIVE CONTROL: the pre-fix arithmetic, restored verbatim\n");
    {
        // query_handlers.cpp @ 899d328, the two lines this bead deleted:
        //     int32_t speed_raw = safe_read<int32_t>(gs + GAME_SPEED);
        //     r.key("game_speed"); r.value_int(speed_raw + 1);
        // `safe_read` returned 0 on the fault, so:
        const int32_t speed_raw_on_fault = 0;      // what safe_read hands back
        const int prefix_emitted = speed_raw_on_fault + 1;
        check(prefix_emitted == 1,
              "the pre-fix handler emits 1 for a FAULTED read", 
              "game_speed=" + std::to_string(prefix_emitted));
        check(prefix_emitted >= 1 && prefix_emitted <= 5,
              "and 1 is a LEGAL speed, so §7.3 reads it as a wrong-speed "
              "sample and invalidates a healthy episode — the control fails "
              "the claim above, which is what makes the claim worth making");
        // The control's whole content: the two answers must DIFFER.
        check(!classify(true, true, 0).has_speed && prefix_emitted == 1,
              "fixed abstains where pre-fix asserted a speed — the two "
              "handlers are distinguishable on the same input");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    printf("%s\n", g_fails ? "FAIL" : "PASS");
    return g_fails ? 1 : 0;
}
