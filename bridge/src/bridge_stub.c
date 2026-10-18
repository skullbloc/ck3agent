/*
 * libck3_bridge_stub.so — i386 no-op stub for LD_PRELOAD coexistence.
 *
 * Built as a 32-bit ELF .so to satisfy the ELFCLASS check inside 32-bit
 * pre-init shims (Steam's ubuntu12_32/steam-launch-wrapper, pressure-vessel
 * helpers, etc.). Without this stub, those shims emit:
 *
 *   "ld.so: object '/home/gaming/libck3_bridge.so' from LD_PRELOAD cannot
 *    be preloaded (wrong ELF class: ELFCLASS64): ignored."
 *
 * With both libck3_bridge_stub.so (i386) and libck3_bridge.so (x86-64) on
 * LD_PRELOAD, the dynamic loader transparently picks the matching arch for
 * each process: the 32-bit shim loads the stub (this file, no-op), and the
 * 64-bit ck3 binary loads the real bridge. No process sees a mismatch.
 *
 * Init mechanism: we emit a single function pointer into the .init_array
 * section by hand, instead of relying on __attribute__((constructor)).
 * The attribute-form requires crtbeginS.o/crtendS.o to synthesize the
 * .init_array, which is fine when crt files are present but couples the
 * build to a specific gcc version path. The hand-rolled init_array entry
 * is just as effective and lets us build with `-nostdlib` on hosts that
 * don't have 32-bit glibc dev files installed (e.g., Fedora without
 * glibc-devel.i686).
 *
 * Bead: ck-dq7ym.1
 */

static void ck3_bridge_stub_init(void) {
    /* Intentional no-op. The 32-bit shim only needs us to load cleanly. */
}

__attribute__((used, section(".init_array")))
static void (*const ck3_bridge_stub_init_entry)(void) = ck3_bridge_stub_init;
