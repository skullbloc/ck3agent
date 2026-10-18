#!/usr/bin/env python3
"""rc2 B2: the default-build libck3_bridge.so carries no debug-verb handler.

Usage: check_debug_verbs_not_linked.py <libck3_bridge.so>

Reads the symbol table with nm. A CONTROL symbol that every build has must be
present, so a stripped or unreadable library fails instead of passing empty.
Exit 0 pass, 1 fail.
"""
import subprocess
import sys

HANDLERS = (
    "bridge::do_query_memory_probe(",
    "bridge::do_query_resolve_global(",
    "bridge::do_inject_mouse_click(",
    "bridge::do_select_observe_default(",
    "bridge::do_execute_console_command(",
    "bridge::do_set_cb_selector_override(",
)
CONTROL = "bridge::dispatch_action("
STRINGS = (b"XTestFakeButtonEvent", b"/dev/uinput")


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 1
    so = argv[1]
    try:
        nm = subprocess.run(["nm", "-C", "--defined-only", so], capture_output=True,
                            text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"FAIL cannot read symbols of {so}: {e}")
        return 1
    fails = 0
    if CONTROL not in nm:
        print(f"FAIL control symbol {CONTROL} absent: the symbol table is not readable")
        return 1
    for h in HANDLERS:
        hit = h in nm
        print(f"{'FAIL' if hit else 'PASS'}  {h} {'LINKED' if hit else 'absent'}")
        fails += hit
    blob = open(so, "rb").read()
    for s in STRINGS:
        hit = s in blob
        print(f"{'FAIL' if hit else 'PASS'}  string {s.decode()} {'present' if hit else 'absent'}")
        fails += hit
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
