// =============================================================================
// test_init_order.cpp — ck-22n3
//
// THE CONTRACT, asserted against the BUILT libck3_bridge.so, not against source:
//
//     every C++ dynamic initializer (_GLOBAL__sub_I*) runs BEFORE dll_init().
//
// WHY A BINARY TEST. dll_init is bridge_init.cpp's __attribute__((constructor)).
// It writes globals synchronously and it STARTS THREADS — server_loop and
// reader_loop via start_ipc_server(), conductor_loop via pipeline_start(), plus
// the detached game-ready poller. Without a priority its .init_array entry sat
// AHEAD of every dynamic initializer in the library, so all of that ran against
// raw .bss. Two shapes of damage, both observed:
//
//   * ck-xqol: the reader thread scanned a slot table that was still zeroed,
//     read fd 0 as a live client (zero is not negative), polled stdin, and
//     SIGSEGV'd clearing a std::string that had never been constructed.
//   * ck-22n3: bridge_init()'s own writes were RUN OVER by the constructors
//     that came after it — g_valid_ranges (the /proc/self/maps table built
//     during validate_offsets()) was emptied and its buffer leaked, and
//     instance_paths' five std::strings were default-constructed over the
//     resolved instance paths.
//
// Neither is visible in source review and neither is reproducible on demand:
// the evidence lives in .init_array. So the test reads .init_array.
//
// The ordering rule is enforced by include/init_order.h's CK3_EARLY_INIT
// (init_priority(101)) on every namespace-scope global in the library that
// needs dynamic initialization. A future global that forgets it lands in the
// UNPRIORITIZED .init_array, after dll_init, and fails check 3 below. That is
// the whole point of this file.
//
// Checks:
//   1. the .so parses, has a non-empty .init_array and a .symtab to name it
//   2. dll_init() is in .init_array
//   3. EVERY _GLOBAL__sub_I* entry has a lower index than dll_init()
//   4. EVERY _GLOBAL__sub_I* entry is a prioritized one (_GLOBAL__sub_I.00101_)
//   5. the translation units whose globals dll_init provably reaches are all
//      present and early: bridge_init, ipc_server, session, pipeline,
//      instance_paths
//   6. NEGATIVE CONTROL — the pure checker is re-run over a synthetic layout
//      with one initializer moved after dll_init, and MUST report a failure.
//      A check that cannot fail is not evidence.
//
// Host test: it opens the .so as a FILE and never loads it. dlopen()ing it here
// would run dll_init in the test process, which is the opposite of the point.
// =============================================================================

#include <elf.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef CK3_BRIDGE_SO_PATH
#define CK3_BRIDGE_SO_PATH "libck3_bridge.so"
#endif

namespace {

int g_checks = 0;
int g_failures = 0;

void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) {
        printf("  PASS  %s\n", what.c_str());
    } else {
        ++g_failures;
        printf("  FAIL  %s\n", what.c_str());
    }
}

// ---------------------------------------------------------------------------
// The PURE half: given .init_array in order, does it satisfy the contract?
// Kept separate from the ELF reader so check 6 can run it over a synthetic
// layout and watch it fail.
// ---------------------------------------------------------------------------
struct OrderVerdict {
    bool   ok = false;
    std::string reason;      // empty when ok
    int    dll_init_index = -1;
    int    initializers = 0;
    int    unprioritized = 0;
};

bool is_sub_i(const std::string& name) {
    return name.rfind("_GLOBAL__sub_I", 0) == 0;
}

bool is_prioritized_sub_i(const std::string& name) {
    // GCC emits _GLOBAL__sub_I.<priority>_<tu> for prioritized initializers and
    // _GLOBAL__sub_I_<tu> for unprioritized ones.
    return name.rfind("_GLOBAL__sub_I.", 0) == 0;
}

OrderVerdict verify_order(const std::vector<std::string>& entries) {
    OrderVerdict v;
    for (size_t i = 0; i < entries.size(); ++i) {
        // objdump/nm spell it "dll_init()" demangled and "_ZL8dll_initv" raw;
        // this reader hands over the raw name.
        if (entries[i].find("dll_init") != std::string::npos) v.dll_init_index = (int)i;
    }
    if (v.dll_init_index < 0) {
        v.reason = "dll_init is not in .init_array at all";
        return v;
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!is_sub_i(entries[i])) continue;
        ++v.initializers;
        if (!is_prioritized_sub_i(entries[i])) ++v.unprioritized;
        if ((int)i > v.dll_init_index) {
            v.reason = "initializer " + entries[i] + " runs AFTER dll_init";
            return v;
        }
    }
    if (v.unprioritized > 0) {
        v.reason = "an initializer is missing CK3_EARLY_INIT (unprioritized _GLOBAL__sub_I)";
        return v;
    }
    if (v.initializers == 0) {
        v.reason = "no C++ dynamic initializers found — is this the right .so?";
        return v;
    }
    v.ok = true;
    return v;
}

// ---------------------------------------------------------------------------
// The ELF reader
// ---------------------------------------------------------------------------
struct InitArray {
    bool loaded = false;
    std::string error;
    std::vector<std::string> names;     // symbol name per entry, in order
    std::vector<uint64_t>    addrs;
};

std::vector<unsigned char> read_file(const char* path, std::string* err) {
    std::vector<unsigned char> buf;
    FILE* f = fopen(path, "rb");
    if (!f) { *err = std::string("cannot open ") + path; return buf; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        buf.resize((size_t)n);
        if (fread(buf.data(), 1, buf.size(), f) != buf.size()) {
            *err = "short read";
            buf.clear();
        }
    }
    fclose(f);
    return buf;
}

InitArray read_init_array(const char* path) {
    InitArray ia;
    std::string err;
    std::vector<unsigned char> f = read_file(path, &err);
    if (f.empty()) { ia.error = err.empty() ? "empty file" : err; return ia; }
    if (f.size() < sizeof(Elf64_Ehdr) || memcmp(f.data(), ELFMAG, SELFMAG) != 0 ||
        f[EI_CLASS] != ELFCLASS64) {
        ia.error = "not a 64-bit ELF";
        return ia;
    }
    const Elf64_Ehdr* eh = reinterpret_cast<const Elf64_Ehdr*>(f.data());
    if (eh->e_shoff == 0 || eh->e_shnum == 0) { ia.error = "no section headers"; return ia; }
    const Elf64_Shdr* sh = reinterpret_cast<const Elf64_Shdr*>(f.data() + eh->e_shoff);

    const Elf64_Shdr* init = nullptr;
    const Elf64_Shdr* symtab = nullptr;
    for (unsigned i = 0; i < eh->e_shnum; ++i) {
        if (sh[i].sh_type == SHT_INIT_ARRAY) init = &sh[i];
        if (sh[i].sh_type == SHT_SYMTAB)     symtab = &sh[i];
    }
    if (!init)   { ia.error = "no .init_array section"; return ia; }
    if (!symtab) { ia.error = ".so has no .symtab (stripped?) — cannot name entries"; return ia; }
    if (symtab->sh_link >= eh->e_shnum) { ia.error = "bad symtab strtab link"; return ia; }
    const Elf64_Shdr& strtab = sh[symtab->sh_link];

    // address -> name, for FUNC symbols
    std::vector<std::pair<uint64_t, std::string>> funcs;
    const Elf64_Sym* syms = reinterpret_cast<const Elf64_Sym*>(f.data() + symtab->sh_offset);
    size_t nsyms = symtab->sh_entsize ? symtab->sh_size / symtab->sh_entsize : 0;
    const char* strs = reinterpret_cast<const char*>(f.data() + strtab.sh_offset);
    for (size_t i = 0; i < nsyms; ++i) {
        if (ELF64_ST_TYPE(syms[i].st_info) != STT_FUNC) continue;
        if (syms[i].st_name == 0 || syms[i].st_name >= strtab.sh_size) continue;
        funcs.emplace_back(syms[i].st_value, std::string(strs + syms[i].st_name));
    }

    size_t count = init->sh_size / sizeof(uint64_t);
    const unsigned char* p = f.data() + init->sh_offset;
    for (size_t i = 0; i < count; ++i) {
        uint64_t a = 0;
        memcpy(&a, p + i * sizeof(uint64_t), sizeof(uint64_t));
        ia.addrs.push_back(a);
        std::string name = "?";
        for (const auto& kv : funcs) {
            if (kv.first == a) { name = kv.second; break; }
        }
        ia.names.push_back(name);
    }
    ia.loaded = true;
    return ia;
}

bool has_tu(const std::vector<std::string>& names, const char* tu) {
    const std::string needle = std::string("_GLOBAL__sub_I.00101_") + tu + ".cpp";
    for (const auto& n : names) if (n == needle) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : CK3_BRIDGE_SO_PATH;
    printf("test_init_order (ck-22n3) — %s\n", path);

    InitArray ia = read_init_array(path);
    ok(ia.loaded, std::string("1. .init_array read") +
                  (ia.loaded ? "" : (" — " + ia.error)));
    if (!ia.loaded) {
        printf("\n%d/%d checks passed — FAIL\n", g_checks - g_failures, g_checks);
        return 1;
    }

    printf("\n  .init_array, in execution order:\n");
    for (size_t i = 0; i < ia.names.size(); ++i) {
        printf("    [%2zu] 0x%08llx  %s\n", i,
               (unsigned long long)ia.addrs[i], ia.names[i].c_str());
    }
    printf("\n");

    OrderVerdict v = verify_order(ia.names);

    ok(v.dll_init_index >= 0, "2. dll_init() is in .init_array");
    ok(v.ok, std::string("3+4. every _GLOBAL__sub_I runs before dll_init and carries "
                         "CK3_EARLY_INIT") + (v.ok ? "" : (" — " + v.reason)));
    printf("        (%d initializers, %d unprioritized, dll_init at index %d)\n",
           v.initializers, v.unprioritized, v.dll_init_index);

    // 5. The translation units dll_init provably reaches. Named individually so
    //    a regression says WHICH subsystem lost its priority, not merely that
    //    something did.
    //      bridge_init    — g_valid_ranges, written by validate_offsets() and
    //                       read by is_mapped() on every bridge thread
    //      ipc_server     — g_clients / g_msg_queue / g_queue_cv / the thread
    //                       handles (ck-xqol's crash)
    //      session        — g_session_registry, reached by the reader thread on
    //                       the first handshake
    //      pipeline       — g_exhausted, reached from the conductor thread's
    //                       telemetry and from every get_bridge_state
    //      instance_paths — the five std::strings bridge_init() fills FIRST
    static const char* kReached[] = {
        "bridge_init", "ipc_server", "session", "pipeline", "instance_paths"
    };
    for (const char* tu : kReached) {
        ok(has_tu(ia.names, tu),
           std::string("5. ") + tu + ".cpp initializes at priority 101");
    }

    // 6. NEGATIVE CONTROL. Same checker, a layout with one initializer demoted
    //    to after dll_init. It must NOT pass.
    {
        std::vector<std::string> bad = {
            "_GLOBAL__sub_I.00101_ipc_server.cpp",
            "frame_dummy",
            "_ZL8dll_initv",
            "_GLOBAL__sub_I_pipeline.cpp",      // the regression being simulated
        };
        OrderVerdict bv = verify_order(bad);
        ok(!bv.ok, "6. negative control: an initializer after dll_init is REJECTED");
        printf("        (rejected with: %s)\n",
               bv.reason.empty() ? "(nothing — BUG)" : bv.reason.c_str());

        std::vector<std::string> unpri = {
            "_GLOBAL__sub_I_pipeline.cpp",      // before dll_init, but no priority
            "_ZL8dll_initv",
        };
        OrderVerdict uv = verify_order(unpri);
        ok(!uv.ok, "6. negative control: an unprioritized initializer is REJECTED");
        printf("        (rejected with: %s)\n",
               uv.reason.empty() ? "(nothing — BUG)" : uv.reason.c_str());
    }

    printf("\n%d/%d checks passed — %s\n", g_checks - g_failures, g_checks,
           g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
