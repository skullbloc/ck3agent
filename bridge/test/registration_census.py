#!/usr/bin/env python3
"""ck-tdco: are all five bridge threads REALLY registered? (§C of ck-duv5)

Replaces a `grep -c 'register_thread('` that ck-t5bc showed could not fail for
the reason it named. Three shapes, driven by them on real disk:

    delete a registration                         -> 4 sites, RED    (caught)
    COMMENT ONE OUT                               -> 5 sites, GREEN  (missed)
    delete a real call AND add a prose comment
      mentioning register_thread( elsewhere       -> 5 sites, GREEN  (missed)

The last is the damaging one: a comment substitutes for a registration, so the
check reports five registered threads over a tree with four. Same family as the
FLAG in acceptance_observer_declaration.py and ck-zi99's census.

TWO CHANGES. Comments are stripped first — reusing the shared stripper in
scripts/cpp_source.py, which already knows BOTH comment forms because ck-9rto
mutated a `//`-only stripper with `/* DISABLED: ... */` and it passed. And the count is
PER ROLE rather than a total, so deleting one real call drops that role to zero
and no amount of prose elsewhere can make it up.

STILL A READ, NOT A RUN. These TUs need the engine to link, so this asserts the
registrations are present as live statements — not that they EXECUTE. ck-t5bc
hand-verified all five at 1628541e; this keeps that true as the tree moves.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "scripts"))
try:
    # scripts/cpp_source.py — NO dependencies, deliberately. It used to live in
    # acceptance_clock_start.py, and importing it from there dragged that
    # module's whole chain: a shadow tree missing one transitive import turned a
    # mutation run into tracebacks the runner scored as FAIL. An import failure
    # here is NOT EVALUABLE, never a verdict.
    from cpp_source import strip_comments  # noqa: E402  (one declaration)
except Exception as exc:  # pragma: no cover
    print(f"NOT EVALUABLE: cannot import the shared comment stripper: {exc}")
    sys.exit(3)

REPO = Path(__file__).resolve().parent.parent.parent

# role -> the file it must be registered in. Naming the file too means moving a
# registration to the wrong thread is a failure, not a silent pass.
EXPECTED = {
    "GameThread":        "bridge/src/bridge_init.cpp",
    "HookInstaller":     "bridge/src/bridge_init.cpp",
    "IpcReader":         "bridge/src/ipc_server.cpp",
    "IpcServer":         "bridge/src/ipc_server.cpp",
    "PipelineConductor": "bridge/src/pipeline.cpp",
}

def main() -> int:
    srcs = {}
    for rel in sorted(set(EXPECTED.values())):
        p = REPO / rel
        if not p.exists():
            print(f"NOT EVALUABLE: missing {rel}")
            return 3
        srcs[rel] = strip_comments(p.read_text(encoding="utf-8"))

    failed = 0
    for role, rel in sorted(EXPECTED.items()):
        pat = re.compile(r"register_thread\s*\(\s*(?:bridge::)?ThreadRole::"
                         + re.escape(role) + r"\s*\)")
        n = len(pat.findall(srcs[rel]))
        ok = n == 1
        if not ok:
            failed += 1
        print(f"registers_{role}\t{1 if ok else 0}\t({n} live statement(s) in {rel}, want 1)")

    # And nothing registers a role we did not expect — a sixth thread appearing
    # without a row here would otherwise be invisible to this census.
    all_roles = set()
    for rel, text in srcs.items():
        all_roles |= set(re.findall(r"register_thread\s*\(\s*(?:bridge::)?ThreadRole::(\w+)\s*\)", text))
    extra = all_roles - set(EXPECTED)
    print(f"no_unexpected_roles_registered\t{0 if extra else 1}\t"
          f"({'unexpected: ' + ', '.join(sorted(extra)) if extra else 'none'})")
    if extra:
        failed += 1

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
