// fault_location_probe.cpp — IS THE FAULTING INSTRUCTION ACTUALLY RECOVERABLE
// FROM A ucontext ON THIS PLATFORM? (ck-duv5)
//
// ck-duv5 makes every fatal path log `rip` and `rip_off = rip - base`, so a
// crash names the instruction instead of only the address it touched. THAT
// RESTS ON ONE PLATFORM ASSUMPTION: that the third argument to an SA_SIGINFO
// handler carries a ucontext whose gregs[REG_RIP] holds the faulting
// instruction pointer. If that is ever false — different libc, different arch,
// a sigaltstack subtlety — the shipped handler logs `rip=(nil) rip_off=0x0` and
// the fix silently buys nothing.
//
// SO IT IS DRIVEN RATHER THAN ASSUMED: install a real handler, take a real
// SIGSEGV at a known instruction, and check what comes back.
//
// WHAT THIS WITNESSES AND WHAT IT DOES NOT, said here rather than left to be
// discovered. It witnesses THE TECHNIQUE on this platform. It does NOT exercise
// bridge_init.cpp's handler — that TU needs the engine — so that the shipped
// handler USES the technique on both fatal paths is a READ, not a run. The two
// claims are different and only one of them is driven here.

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ucontext.h>
#include <csetjmp>
#include <string>
#include "../include/fault_site.h"
#include "../include/thread_registry.h"
#include <thread>
#include <vector>

namespace {

volatile void* g_rip = nullptr;
volatile void* g_fault_addr = nullptr;
volatile int   g_ran = 0;
sigjmp_buf     g_back;
char           g_regs[512] = {0};

// The address of the faulting function, so the probe can assert the recovered
// RIP lands INSIDE it rather than merely being non-null — a handler that
// reported a constant would pass a non-null check.
void (*g_expected_fn)() = nullptr;

// Rows are printed AND counted. The first cut printed a table and returned 0
// unconditionally — a probe that cannot exit non-zero is a report, not a gate,
// and a gate runner would have called it green no matter what it observed.
int g_failed = 0;
void row(const char* name, bool ok) {
    if (!ok) ++g_failed;
    std::printf("%s\t%d\n", name, ok ? 1 : 0);
}

void handler(int, siginfo_t* info, void* ctx) {
    g_ran = 1;
    g_fault_addr = info ? info->si_addr : nullptr;
    if (ctx) format_fault_regs(g_regs, sizeof g_regs, ctx);
    if (ctx) {
        auto* uc = static_cast<ucontext_t*>(ctx);
        g_rip = reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RIP]);
    }
    siglongjmp(g_back, 1);
}

// Deliberately fault at a null + small offset, the shape the arm-A crash had.
volatile int g_sink = 0;

// The object pointer the engine would be holding. volatile + extern-ish so the
// compiler cannot fold it: it must be LOADED INTO A REGISTER and dereferenced,
// which is the shape both arm-A crashes had (a field read off a null object).
//
// NOT null, though the real crashes were: a null object puts 0 in the register,
// and 0 is in half the register file at any moment, so the row could not tell a
// captured value from a coincidence. A distinctive unmapped object makes the
// claim discriminating — and the property under test ("the register names the
// object") is the same one either way.
volatile uintptr_t g_object = 0xdead0000UL;

__attribute__((noinline)) void fault_here() {
    // A FEW REAL INSTRUCTIONS BEFORE THE FAULT, ON PURPOSE. My first cut faulted
    // on the function's FIRST instruction, so rip - fn was 0 — and the row
    // asserting a NON-ZERO offset failed while the code was correct. A fixture
    // that sits exactly on the boundary tests the boundary, not the claim.
    for (int i = 0; i < 3; ++i) g_sink += i;

    // AND THE FAULT ITSELF MUST GO THROUGH A REGISTER. My second cut wrote
    // `*(uint8_t*)0x1a`, which -O1 compiled to `movzbl 26, %eax` — the constant
    // lives in the INSTRUCTION ENCODING and no register ever holds it. The
    // register row then failed against correct code, because the fixture could
    // not exhibit the property being asserted. A null OBJECT with a field read
    // at +0x1a is both the real shape and register-bearing.
    volatile uint8_t* p =
        reinterpret_cast<volatile uint8_t*>(g_object) + 0x1a;
    (void)*p;                       // SIGSEGV at object+0x1a, object in a GPR
}

}  // namespace

int main() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);

    g_expected_fn = &fault_here;
    if (sigsetjmp(g_back, 1) == 0) {
        fault_here();
    }

    const uintptr_t rip = reinterpret_cast<uintptr_t>(const_cast<void*>(g_rip));
    const uintptr_t fn  = reinterpret_cast<uintptr_t>(
        reinterpret_cast<void*>(g_expected_fn));
    // A faulting instruction inside a small noinline function is within a few
    // hundred bytes of its entry. The window is deliberately loose — the claim
    // is "RIP points into the function that faulted", not an exact offset.
    const bool in_fn = rip >= fn && rip < fn + 4096;

    std::printf("case\tvalue\n");
    row("handler_ran", g_ran == 1);
    row("fault_addr_is_object_plus_field_offset",
        g_fault_addr == reinterpret_cast<void*>(0xdead001aUL));
    row("rip_recovered", rip != 0);
    row("rip_points_into_the_faulting_fn", in_fn);
    // AND THE OFFSET IS WHAT MAKES IT ACTIONABLE: rip - base is the number that
    // resolves in Ghidra. Here "base" is the faulting function; in the bridge it
    // is g_base_addr. A zero offset would mean the subtraction produced nothing.
    row("rip_minus_base_is_nonzero", in_fn && (rip - fn) != 0);

    // ── §B: THE SHARED FORMATTER, BOTH BRANCHES ─────────────────────────────
    // §A above witnesses the TECHNIQUE (ucontext carries the faulting IP on this
    // platform). These rows witness the SPELLING the handler logs — and in
    // particular that the branch which cannot compute an offset says so instead
    // of printing a plausible 0x0. That branch is unreachable on demand inside
    // the handler, which is the whole reason the formatter was lifted out.
    char b[192];

    // resolved: a real base, a real rip.
    const uintptr_t fake_base = 0x55000000UL;
    const void*     fake_rip  = reinterpret_cast<void*>(fake_base + 0x4c89360UL);
    std::string resolved = format_fault_site(b, sizeof b, fake_rip, fake_base);
    row("resolved_prints_the_offset",
        resolved.find("rip_off=0x4c89360") != std::string::npos);
    row("resolved_prints_the_ghidra_va",
        resolved.find("Ghidra ~0x4d89360") != std::string::npos);
    row("resolved_says_nothing_about_UNRESOLVED",
        resolved.find("UNRESOLVED") == std::string::npos);

    // no ucontext: rip is null.
    std::string no_ctx = format_fault_site(b, sizeof b, nullptr, fake_base);
    row("no_ucontext_refuses_rather_than_zero",
        no_ctx.find("rip_off=UNRESOLVED") != std::string::npos);
    row("no_ucontext_names_which_input_was_missing",
        no_ctx.find("no ucontext") != std::string::npos);

    // base unknown: rip is real, g_base_addr never got set.
    std::string no_base = format_fault_site(b, sizeof b, fake_rip, 0);
    row("no_base_refuses_rather_than_zero",
        no_base.find("rip_off=UNRESOLVED") != std::string::npos);
    row("no_base_names_which_input_was_missing",
        no_base.find("base address unknown") != std::string::npos);

    // THE ROW THAT MATTERS: the two refusals must not read like the answer for a
    // fault at base+0. If they did, a reader of a future crash log would be back
    // where arm-A left us — holding a number that cannot say if it was measured.
    std::string at_base_zero = format_fault_site(b, sizeof b,
                                   reinterpret_cast<void*>(fake_base), fake_base);
    row("a_real_fault_at_base_plus_0_is_distinguishable_from_a_refusal",
        at_base_zero.find("rip_off=0x0") != std::string::npos &&
        no_ctx.find("rip_off=0x0")       == std::string::npos &&
        no_base.find("rip_off=0x0")      == std::string::npos);

    // ── §D: THE REGISTERS, END TO END ───────────────────────────────────────
    // Not a synthetic mcontext: this is the register file from the REAL fault
    // above. fault_here() loads 0x1a into a register and dereferences it, so if
    // the capture works, some GPR still holds 0x1a at fault time. That is the
    // whole diagnostic claim — "the register names the pointer" — driven rather
    // than asserted.
    std::string regs = g_regs;
    row("regs_captured", !regs.empty() && regs.find("UNAVAILABLE") == std::string::npos);
    // THE DIAGNOSTIC CLAIM, DRIVEN: fault_addr says 0xdead001a — "something at
    // +0x1a off an object". The register file says WHICH object: 0xdead0000. In
    // a real crash that is the difference between "a null pointer somewhere" and
    // the identity of the structure that was null.
    row("a_register_names_the_object_the_fault_came_off",
        regs.find("=dead0000") != std::string::npos);
    char rb[512];
    row("no_ucontext_regs_say_so",
        std::string(format_fault_regs(rb, sizeof rb, nullptr)).find("UNAVAILABLE")
            != std::string::npos);

    // ── §E: WHICH THREAD FAULTED ────────────────────────────────────────────
    // The registry is a recorded fact, not an inference: each bridge thread
    // registers at its own entry point and the handler looks the tid up. That
    // is what makes it drivable here with no game — real threads, real tids.
    using namespace bridge;
    char tb[192];

    // This thread registers as the game thread and must read back as one.
    register_thread(ThreadRole::GameThread);
    const int me = current_tid();
    std::string mine = format_fault_thread(tb, sizeof tb, me);
    row("registered_thread_reports_its_role",
        mine.find("thread=game_thread") != std::string::npos);
    row("registered_thread_reports_its_tid",
        mine.find("tid=" + std::to_string(me)) != std::string::npos);

    // A REAL second thread with a REAL different tid, registered differently.
    // Two roles in one table is the case a single-thread test cannot reach.
    std::string other;
    int other_tid = 0;
    std::thread t([&]{
        register_thread(ThreadRole::IpcServer);
        other_tid = current_tid();
        char b2[192];
        other = format_fault_thread(b2, sizeof b2, other_tid);
    });
    t.join();
    row("a_second_thread_gets_its_own_distinct_role",
        other.find("thread=ipc_server") != std::string::npos && other_tid != me);
    // And the first thread's entry is not clobbered by the second's.
    row("registering_one_thread_does_not_overwrite_another",
        std::string(format_fault_thread(tb, sizeof tb, me))
            .find("thread=game_thread") != std::string::npos);

    // THE ROW ck-t5bc ASKED FOR: an unknown tid must REFUSE, not fall back.
    // Threads the bridge never created — the engine's AI workers above all —
    // land here, and a plausible-looking role would be indistinguishable from a
    // recorded one. Same rule as rip_off=UNRESOLVED.
    std::string unknown = format_fault_thread(tb, sizeof tb, 0x7FFFFFFF);
    row("unknown_tid_says_UNREGISTERED",
        unknown.find("thread=UNREGISTERED") != std::string::npos);
    row("unknown_tid_does_not_name_a_role",
        unknown.find("game_thread") == std::string::npos &&
        unknown.find("ipc_server") == std::string::npos &&
        unknown.find("engine")     == std::string::npos);
    // ... and it carries the table's own integrity witness, so a reader can
    // tell "not a bridge thread" from "the table overflowed and lost it".
    row("unknown_tid_reports_table_overflow_count",
        unknown.find("table_overflow=") != std::string::npos);
    row("table_did_not_overflow_during_this_run",
        thread_table_overflow().load() == 0);

    // ── ck-tdco: THE OVERFLOW COUNTER'S POSITIVE CASE ───────────────────────
    // ck-t5bc drove the mutants on real disk and found that killing the
    // fetch_add left this run GREEN. Both overflow rows passed on a dead
    // counter: one asserts only that the string "table_overflow=" is PRESENT,
    // the other asserts the count is ZERO. A counter that is only ever asserted
    // to be zero is indistinguishable from a counter that cannot count.
    //
    // THE WITNESS HAD NO WITNESS, and it matters here more than it usually
    // would: the whole UNREGISTERED argument is that an absent entry means "not
    // a bridge thread" — which holds ONLY if the table cannot silently have
    // lost one. The counter is what rules that out, so the counter has to be
    // shown to work.
    //
    // Driven, not simulated: real threads, really registering, really past the
    // table size. kMaxRegisteredThreads is 16 and two slots are already taken.
    {
        const int before = thread_table_overflow().load();
        std::vector<std::thread> fillers;
        for (size_t i = 0; i < kMaxRegisteredThreads + 4; ++i)
            fillers.emplace_back([]{ register_thread(ThreadRole::IpcReader); });
        for (auto& f : fillers) f.join();
        const int after = thread_table_overflow().load();
        row("overflowing_the_table_INCREMENTS_the_counter", after > before);

        // And an overflowed thread must still be reported honestly rather than
        // silently absent: its tid has no slot, so it reads UNREGISTERED — and
        // the line now carries a NON-ZERO overflow count, which is exactly the
        // signal that distinguishes "not a bridge thread" from "the table lost
        // one". Without this row the two cases print the same sentence.
        char ob[192];
        std::string overflowed = format_fault_thread(ob, sizeof ob, 0x7FFFFFFE);
        row("an_overflowed_table_says_so_in_the_line",
            overflowed.find("thread=UNREGISTERED") != std::string::npos &&
            overflowed.find("table_overflow=0)") == std::string::npos);
    }

    // A registered role must be distinguishable from a refusal, exactly as a
    // real fault at base+0 must be distinguishable from an unresolved offset.
    row("a_real_role_is_distinguishable_from_a_refusal",
        mine.find("UNREGISTERED") == std::string::npos &&
        unknown.find("UNREGISTERED") != std::string::npos);

    // ── §F: THE WORD AT [rsp] (ck-65lv) ─────────────────────────────────────
    // ck-duv5's first capture named GetPrestige and could not name its caller —
    // 16 candidates, identity in the return address, which at an entry ip is at
    // [rsp]. My first cut GATED the read on ip_is_at_entry and the only shipped
    // caller passes false, so it would have named nobody. It now always emits
    // the word, labelled as what it literally is.
    {
        char rb[192];
        const uintptr_t fake_base = 0x55000000UL;
        uintptr_t word = fake_base + 0x4dd2ca0UL;
        uintptr_t slot = reinterpret_cast<uintptr_t>(&word);
        auto good = [](uintptr_t a, void* out, size_t n) -> bool {
            std::memcpy(out, reinterpret_cast<const void*>(a), n); return true; };
        auto bad  = [](uintptr_t, void*, size_t) -> bool { return false; };

        std::string ok = format_frame_word(rb, sizeof rb, slot, good, fake_base, fake_base + 0x10000000UL);
        row("the_frame_word_is_ALWAYS_emitted",
            ok.find("[rsp]=0x") != std::string::npos);
        row("the_frame_word_is_rendered_as_a_Ghidra_VA",
            ok.find("[rsp]_off=0x4dd2ca0") != std::string::npos &&
            ok.find("Ghidra ~0x4ed2ca0") != std::string::npos);
        // It must NOT assert the interpretation, only state the rule — the
        // handler cannot know whether the ip is at an entry.
        row("it_states_the_rule_rather_than_asserting_a_return_address",
            ok.find("AT A FUNCTION ENTRY") != std::string::npos &&
            ok.find("ret=") == std::string::npos);

        std::string unread = format_frame_word(rb, sizeof rb, slot, bad, fake_base, fake_base + 0x10000000UL);
        row("an_unreadable_rsp_says_so_rather_than_printing_a_value",
            unread.find("[rsp]=UNREADABLE") != std::string::npos &&
            unread.find("[rsp]=0x") == std::string::npos);

        uintptr_t outside = 0x1000UL;
        std::string oo = format_frame_word(
            rb, sizeof rb, reinterpret_cast<uintptr_t>(&outside), good, fake_base, fake_base + 0x10000000UL);
        row("a_word_outside_the_image_refuses_the_offset",
            oo.find("[rsp]_off=UNRESOLVED") != std::string::npos &&
            oo.find("[rsp]=0x1000") != std::string::npos);

        row("the_frame_word_formatter_DISCRIMINATES",
            ok.find("[rsp]_off=0x") != std::string::npos &&
            unread.find("[rsp]_off=0x") == std::string::npos);
    }

    // ── §G: THE LADDER (ck-65lv, the dare) ───────────────────────────────────
    // Eight words above rsp, each probed independently, each labelled as what
    // it is. The dare's one-word line named the caller and could not name the
    // caller's caller; a reproduction on one word buys the same word again.
    {
        char lb[768];
        const uintptr_t fake_base = 0x55000000UL;
        // a fake stack: word 0 in-image (a return address), 1 outside, 2 in-image,
        // 3 will be REFUSED by the probe, 4..7 outside/zero
        uintptr_t stack[8] = { fake_base + 0x7748ebdUL, 0x3d0UL, fake_base + 0x123456UL,
                               fake_base + 0x999UL, 0UL, 0xaUL, 0x7f4ae19f9aa0UL, 0UL };
        const uintptr_t sp = reinterpret_cast<uintptr_t>(&stack[0]);
        static uintptr_t refuse_at = 0; refuse_at = sp + 3 * sizeof(uintptr_t);
        auto good = [](uintptr_t a, void* out, size_t n) -> bool {
            std::memcpy(out, reinterpret_cast<const void*>(a), n); return true; };
        auto refuse3 = [](uintptr_t a, void* out, size_t n) -> bool {
            if (a == refuse_at) return false;
            std::memcpy(out, reinterpret_cast<const void*>(a), n); return true; };
        auto bad = [](uintptr_t, void*, size_t) -> bool { return false; };

        std::string all = format_stack_ladder(lb, sizeof lb, sp, good, fake_base, fake_base + 0x10000000UL);
        row("the_ladder_prints_EIGHT_slots",
            [&]{ size_t c = 0, pos = 0; while ((pos = all.find("[rsp+0x", pos)) != std::string::npos) { ++c; ++pos; } return c == 8; }());
        row("an_in_image_word_carries_offset_and_Ghidra_VA",
            all.find("[rsp+0x00]=0x5c748ebd off=0x7748ebd ~0x7848ebd") != std::string::npos);
        row("a_word_outside_the_image_is_raw_with_off_UNRESOLVED",
            all.find("[rsp+0x08]=0x3d0 off=UNRESOLVED") != std::string::npos &&
            all.find("[rsp+0x28]=0xa off=UNRESOLVED") != std::string::npos);
        // THE COUNTEREXAMPLE THE FIXTURE HELD AND DID NOT LOOK AT (ck-760i's
        // review): a stack address is ABOVE the base and must not be an offset.
        row("a_STACK_address_above_the_base_is_UNRESOLVED_not_an_offset",
            all.find("[rsp+0x30]=0x7f4ae19f9aa0 off=UNRESOLVED") != std::string::npos);
        row("the_ladder_states_the_interpretation_rule_not_a_claim",
            all.find("RETURN-ADDRESS CANDIDATES") != std::string::npos &&
            all.find("ret=") == std::string::npos);

        std::string part = format_stack_ladder(lb, sizeof lb, sp, refuse3, fake_base, fake_base + 0x10000000UL);
        row("an_unreadable_slot_is_printed_UNREADABLE_beside_the_others",
            part.find("[rsp+0x18]=UNREADABLE") != std::string::npos);
        row("the_ladder_does_NOT_stop_at_the_first_unreadable_slot",
            part.find("[rsp+0x20]=0x0 off=UNRESOLVED") != std::string::npos &&
            part.find("[rsp+0x38]=") != std::string::npos);
        row("the_readable_slots_are_identical_with_and_without_the_refusal",
            part.find("[rsp+0x00]=0x5c748ebd off=0x7748ebd") != std::string::npos &&
            part.find("[rsp+0x10]=0x55123456 off=0x123456") != std::string::npos);

        std::string none = format_stack_ladder(lb, sizeof lb, sp, bad, fake_base, fake_base + 0x10000000UL);
        row("a_probe_that_refuses_everything_yields_eight_UNREADABLE_and_no_values",
            [&]{ size_t c = 0, pos = 0; while ((pos = none.find("=UNREADABLE", pos)) != std::string::npos) { ++c; ++pos; } return c == 8; }()
            && none.find("]=0x") == std::string::npos);
        std::string norsp = format_stack_ladder(lb, sizeof lb, 0, good, fake_base, fake_base + 0x10000000UL);
        row("no_rsp_says_UNREADABLE_rather_than_probing_address_zero",
            norsp.find("ladder=UNREADABLE") != std::string::npos);

        // bounded: a buffer too small is truncated, never overrun, still terminated
        char tiny[40];
        std::string t = format_stack_ladder(tiny, sizeof tiny, sp, good, fake_base, fake_base + 0x10000000UL);
        row("a_small_buffer_truncates_safely_and_stays_terminated",
            t.size() < sizeof tiny && std::strlen(tiny) == t.size());

        // the one-word line is UNCHANGED by the ladder's existence
        char rb[192];
        uintptr_t word = fake_base + 0x4dd2ca0UL;
        std::string one = format_frame_word(rb, sizeof rb, reinterpret_cast<uintptr_t>(&word), good, fake_base, fake_base + 0x10000000UL);
        row("the_one_word_line_still_prints_exactly_as_before",
            one.find("[rsp]_off=0x4dd2ca0 Ghidra ~0x4ed2ca0 (AT A FUNCTION ENTRY") != std::string::npos);
    }

    // ── §H: THE RANGE HAS TWO ENDS (ck-2ptx completed) ────────────────────────
    // Measured with the dare's REAL base and register values: three of eight
    // words rendered as offsets ~46 TB into a 170 MB image.
    {
        char lb[768], rb[192];
        const uintptr_t base = 0x55b6f324a000UL, end = base + 0xB000000UL;   // ~176 MB
        uintptr_t stack[8] = { 0x55b6fa992ebdUL, 0x7f4ae19f9aa0UL, 0x7f4ae19f9808UL, 0x3d0UL,
                               0xaUL, 0x7f4b14ff6ac0UL, 0x0UL, 0x55b6ff117570UL };
        const uintptr_t sp = reinterpret_cast<uintptr_t>(&stack[0]);
        auto good = [](uintptr_t a, void* out, size_t n) -> bool {
            std::memcpy(out, reinterpret_cast<const void*>(a), n); return true; };
        std::string lad = format_stack_ladder(lb, sizeof lb, sp, good, base, end);
        // 0x55b6ff117570 (rbx in the dare) is 0xbecd570 = ~200 MB past the base — PAST
        // a ~176 MB image, i.e. heap just beyond the end: exactly the "plausible
        // offset" the upper bound exists to refuse. My first expectation called it
        // in-image; the code was right and the expectation wrong.
        row("H1 the dare's return address carries its offset; the heap word just past the image does NOT",
            lad.find("[rsp+0x00]=0x55b6fa992ebd off=0x7748ebd ~0x7848ebd") != std::string::npos &&
            lad.find("[rsp+0x38]=0x55b6ff117570 off=UNRESOLVED") != std::string::npos);
        row("H2 the dare's three STACK/heap words are UNRESOLVED, not 46 TB offsets",
            lad.find("[rsp+0x08]=0x7f4ae19f9aa0 off=UNRESOLVED") != std::string::npos &&
            lad.find("[rsp+0x10]=0x7f4ae19f9808 off=UNRESOLVED") != std::string::npos &&
            lad.find("[rsp+0x28]=0x7f4b14ff6ac0 off=UNRESOLVED") != std::string::npos &&
            lad.find("~0x2993ee8afaa0") == std::string::npos);
        row("H3 the ladder header names the image range it classified against",
            lad.find("image [0x55b6f324a000,0x55b6fe24a000)") != std::string::npos &&
            lad.find("extent assumed") == std::string::npos);
        // the same hole in the ONE-WORD line, closed
        uintptr_t stackword = 0x7f4ae19f9aa0UL;
        std::string one = format_frame_word(rb, sizeof rb, reinterpret_cast<uintptr_t>(&stackword), good, base, end);
        row("H4 the one-word line refuses a stack address above the base",
            one.find("[rsp]_off=UNRESOLVED") != std::string::npos && one.find("Ghidra ~") == std::string::npos);
        uintptr_t inimg = 0x55b6fa992ebdUL;
        std::string one_in = format_frame_word(rb, sizeof rb, reinterpret_cast<uintptr_t>(&inimg), good, base, end);
        row("H5 the one-word line's in-image output is unchanged by the extent",
            one_in.find("[rsp]_off=0x7748ebd Ghidra ~0x7848ebd (AT A FUNCTION ENTRY THIS IS THE RETURN ADDRESS; elsewhere a frame word)") != std::string::npos &&
            one_in.find("extent assumed") == std::string::npos);
        // extent UNKNOWN (0): the fallback classifies conservatively and SAYS so
        std::string lad0 = format_stack_ladder(lb, sizeof lb, sp, good, base, 0);
        row("H6 with the extent unknown the fallback (base+512MB) still refuses the stack words and SAYS extent assumed",
            lad0.find("(extent assumed)") != std::string::npos &&
            lad0.find("[rsp+0x08]=0x7f4ae19f9aa0 off=UNRESOLVED") != std::string::npos &&
            lad0.find("[rsp+0x00]=0x55b6fa992ebd off=0x7748ebd") != std::string::npos);
        // the fallback's KNOWN weakness, stated as a row so nobody reads it as coverage:
        // a heap word within 512 MB of the base classifies in-image under the fallback
        row("H6b under the ASSUMED extent a near heap word IS rendered as an offset — which is why the real extent is captured at init",
            lad0.find("[rsp+0x38]=0x55b6ff117570 off=0xbecd570") != std::string::npos);
        std::string one0 = format_frame_word(rb, sizeof rb, reinterpret_cast<uintptr_t>(&inimg), good, base, 0);
        row("H7 the one-word line marks an assumed extent too",
            one0.find("(extent assumed)") != std::string::npos);
    }

    std::printf("\n%s: %d row(s) failed\n", g_failed ? "FAIL" : "PASS", g_failed);
    return g_failed ? 1 : 0;
}
