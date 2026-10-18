#!/usr/bin/env python3
"""
Resident Ghidra query server for the CK3 project.

Opens a FULLY-ANALYZED Ghidra project ONCE, keeps the Program +
DecompInterface resident in memory, and serves JSON requests over TCP so that
agents can decompile / look up functions / find xrefs without paying the
multi-minute JVM + project-open cost per query.

Run it with start.sh beside this file (detached, one per project). Directly:
    GHIDRA_INSTALL_DIR=<ghidra> CK3_GHIDRA_PROJECT_DIR=<dir with .gpr> \
      CK3_GHIDRA_PROJECT_NAME=<name> python3 ghserver.py
  (or --project-dir / --project-name / --port). Needs pyghidra.
  If the project holds several programs it serves the one with most functions.

Protocol: one JSON object per line over TCP 127.0.0.1:$GHIDRA_MCP_PORT
(default 18741); replies one JSON line. Use gh.py as the thin client.

Commands (see gh.py):
    {"cmd":"ping"}
    {"cmd":"func_at","va":"0x062b8cf0"}
    {"cmd":"decompile","va":"0x062b8cf0"}   (also accepts "name")
    {"cmd":"xrefs","va":"0x..."}
    {"cmd":"callees","va":"0x..."}
    {"cmd":"by_name","name":"FUN_..."}
    {"cmd":"symsearch","q":"substring","limit":50}
    {"cmd":"findstr","s":"19CCreateTitleCommand"}   ASCII bytes anywhere in memory
    {"cmd":"findptr","va":"0x..."}                  8-byte LE pointers to va
    {"cmd":"read","va":"0x...","n":16}              n qwords + the function each hits

OFFSET CONVENTION: queries take IMAGE VAs (Ghidra addresses).
bridge header_offset = IMAGE VA - image base (0x100000 for the Linux ELF).
"""
import argparse
import os
import sys
import json
import socket
import socketserver
import threading
import traceback

HOST = "127.0.0.1"   # loopback only: the protocol has no authentication
PORT = int(os.environ.get("GHIDRA_MCP_PORT", "18741"))

PROJECT_PATH = os.environ.get("CK3_GHIDRA_PROJECT_DIR")
PROJECT_NAME = os.environ.get("CK3_GHIDRA_PROJECT_NAME", "ck3")

# ---- globals populated at startup, then read-only -------------------------
_program = None
_flat = None
_decomp = None
_decomp_lock = threading.Lock()
_ready = False
_addr_space = None


def log(msg):
    print(f"[ghserver] {msg}", flush=True)


def _get_program():
    """Open the project (read-only) and return the largest/full-analysis program."""
    import pyghidra
    log("starting pyghidra (JVM)...")
    pyghidra.start(verbose=False)
    log("opening project (read-only)...")
    project = pyghidra.open_project(PROJECT_PATH, PROJECT_NAME, create=False)

    # Enumerate domain files and pick the program with the most functions.
    from ghidra.framework.model import DomainFile  # noqa

    best = {"prog": None, "df": None, "count": -1}

    def walk_folder(folder):
        for df in folder.getFiles():
            yield df
        for sub in folder.getFolders():
            yield from walk_folder(sub)

    root = project.getProjectData().getRootFolder()
    candidates = list(walk_folder(root))
    log(f"found {len(candidates)} domain file(s): {[c.getName() for c in candidates]}")

    from ghidra.util.task import ConsoleTaskMonitor
    from java.lang import Object as JObject
    monitor = ConsoleTaskMonitor()
    CONSUMER = JObject()  # Java Object used as the DomainObject consumer

    for df in candidates:
        try:
            ct = df.getContentType()
        except Exception:
            ct = "?"
        if "Program" not in str(ct):
            log(f"  skip {df.getName()} (content type {ct})")
            continue
        log(f"  opening program {df.getName()} (read-only)...")
        # getReadOnlyDomainObject(consumer, version, monitor)
        prog = df.getReadOnlyDomainObject(CONSUMER, -1, monitor)
        try:
            n = prog.getFunctionManager().getFunctionCount()
        except Exception as e:
            n = -1
            log(f"    function count failed: {e}")
        log(f"    {df.getName()}: {n} functions")
        if n > best["count"]:
            # release the previous loser
            if best["prog"] is not None:
                try:
                    best["prog"].release(CONSUMER)
                except Exception:
                    pass
            best = {"prog": prog, "df": df, "count": n}
        else:
            try:
                prog.release(CONSUMER)
            except Exception:
                pass

    if best["prog"] is None:
        raise RuntimeError("no Program-type domain file found in project")
    log(f"selected program: {best['df'].getName()} with {best['count']} functions")
    return best["prog"], best["count"]


def _init():
    global _program, _flat, _decomp, _ready, _addr_space
    prog, count = _get_program()
    _program = prog

    from ghidra.program.flatapi import FlatProgramAPI
    _flat = FlatProgramAPI(_program)

    from ghidra.app.decompiler import DecompInterface
    from ghidra.app.decompiler import DecompileOptions
    _decomp = DecompInterface()
    opts = DecompileOptions()
    _decomp.setOptions(opts)
    _decomp.openProgram(_program)

    _addr_space = _program.getAddressFactory().getDefaultAddressSpace()

    # Report BuildID / binary info if discoverable.
    try:
        exe_path = _program.getExecutablePath()
        exe_md5 = _program.getExecutableMD5()
        log(f"executable path: {exe_path}")
        log(f"executable MD5: {exe_md5}")
    except Exception as e:
        log(f"executable info unavailable: {e}")

    _ready = True
    log(f"READY: {count} functions, listening on {HOST}:{PORT}")


def _addr(va):
    if isinstance(va, str):
        va = va.strip()
        if va.lower().startswith("0x"):
            va = int(va, 16)
        else:
            va = int(va, 16) if all(c in "0123456789abcdefABCDEF" for c in va) else int(va)
    return _addr_space.getAddress(int(va))


def _func_obj(req):
    """Resolve a function from a request that may carry va or name."""
    fm = _program.getFunctionManager()
    if req.get("name"):
        st = _program.getSymbolTable()
        syms = st.getGlobalSymbols(req["name"])
        for s in syms:
            f = fm.getFunctionAt(s.getAddress())
            if f is not None:
                return f
        # fall through to listing iteration
        it = fm.getFunctions(True)
        for f in it:
            if f.getName() == req["name"]:
                return f
        return None
    a = _addr(req["va"])
    f = fm.getFunctionAt(a)
    if f is None:
        f = fm.getFunctionContaining(a)
    return f


def _func_info(f):
    return {
        "name": f.getName(),
        "entry": "0x%x" % f.getEntryPoint().getOffset(),
        "size": f.getBody().getNumAddresses(),
        "signature": str(f.getSignature().getPrototypeString()),
    }


def handle(req):
    cmd = req.get("cmd")
    if cmd == "ping":
        fm = _program.getFunctionManager()
        return {"ok": True, "ready": _ready, "functions": fm.getFunctionCount()}

    if cmd == "func_at":
        f = _func_obj(req)
        if f is None:
            return {"ok": False, "error": "no function at/containing %s" % req.get("va")}
        info = _func_info(f)
        a = _addr(req["va"])
        info["query_va"] = "0x%x" % a.getOffset()
        info["is_entry"] = (f.getEntryPoint().getOffset() == a.getOffset())
        return {"ok": True, **info}

    if cmd == "symsearch":
        needle = req["q"]
        limit = int(req.get("limit", 50))
        st = _program.getSymbolTable()
        out = []
        it = st.getAllSymbols(True)
        for s in it:
            nm = s.getName()
            if needle.lower() in nm.lower():
                out.append({
                    "name": nm,
                    "va": "0x%x" % s.getAddress().getOffset(),
                    "type": str(s.getSymbolType()),
                })
                if len(out) >= limit:
                    break
        return {"ok": True, "q": needle, "count": len(out), "symbols": out}

    if cmd == "by_name":
        f = _func_obj({"name": req["name"]})
        if f is None:
            return {"ok": False, "error": "no function named %s" % req.get("name")}
        return {"ok": True, **_func_info(f)}

    if cmd == "decompile":
        f = _func_obj(req)
        if f is None:
            return {"ok": False, "error": "no function for %s" % (req.get("va") or req.get("name"))}
        from ghidra.util.task import ConsoleTaskMonitor
        with _decomp_lock:
            res = _decomp.decompileFunction(f, 60, ConsoleTaskMonitor())
        if res is None or not res.decompileCompleted():
            err = res.getErrorMessage() if res is not None else "null result"
            return {"ok": False, "error": "decompile failed: %s" % err, **_func_info(f)}
        c = res.getDecompiledFunction().getC()
        return {"ok": True, **_func_info(f), "c": c}

    if cmd == "xrefs":
        a = _addr(req["va"])
        rm = _program.getReferenceManager()
        fm = _program.getFunctionManager()
        refs = []
        for r in rm.getReferencesTo(a):
            fa = r.getFromAddress()
            cf = fm.getFunctionContaining(fa)
            refs.append({
                "from": "0x%x" % fa.getOffset(),
                "type": str(r.getReferenceType()),
                "from_func": cf.getName() if cf is not None else None,
                "from_func_entry": ("0x%x" % cf.getEntryPoint().getOffset()) if cf is not None else None,
            })
        return {"ok": True, "to": "0x%x" % a.getOffset(), "count": len(refs), "xrefs": refs}

    if cmd == "callees":
        f = _func_obj(req)
        if f is None:
            return {"ok": False, "error": "no function for %s" % (req.get("va") or req.get("name"))}
        from ghidra.util.task import ConsoleTaskMonitor
        callees = []
        for c in f.getCalledFunctions(ConsoleTaskMonitor()):
            callees.append({
                "name": c.getName(),
                "entry": "0x%x" % c.getEntryPoint().getOffset(),
            })
        return {"ok": True, "from": _func_info(f), "count": len(callees), "callees": callees}

    if cmd == "findstr":
        # Find a literal ASCII string in memory. {"cmd":"findstr","s":"19CCreateTitleCommand"}
        s = req["s"]
        mem = _program.getMemory()
        from ghidra.util.task import ConsoleTaskMonitor
        pat = s.encode("ascii")
        hits = []
        start = _addr(req.get("from", "0x100000")) if req.get("from") else None
        addr = mem.findBytes(start, pat, None, True, ConsoleTaskMonitor()) if start else \
               mem.findBytes(mem.getMinAddress(), pat, None, True, ConsoleTaskMonitor())
        while addr is not None and len(hits) < int(req.get("limit", 10)):
            hits.append("0x%x" % addr.getOffset())
            nxt = addr.add(1)
            addr = mem.findBytes(nxt, pat, None, True, ConsoleTaskMonitor())
        return {"ok": True, "s": s, "count": len(hits), "addrs": hits}

    if cmd == "findptr":
        # Find 8-byte little-endian pointer value in memory. {"cmd":"findptr","va":"0x..."}
        target = _addr(req["va"]).getOffset()
        pat = bytes([(target >> (8*i)) & 0xff for i in range(8)])
        mem = _program.getMemory()
        from ghidra.util.task import ConsoleTaskMonitor
        hits = []
        addr = mem.findBytes(mem.getMinAddress(), pat, None, True, ConsoleTaskMonitor())
        while addr is not None and len(hits) < int(req.get("limit", 20)):
            hits.append("0x%x" % addr.getOffset())
            addr = mem.findBytes(addr.add(1), pat, None, True, ConsoleTaskMonitor())
        return {"ok": True, "target": "0x%x" % target, "count": len(hits), "addrs": hits}

    if cmd == "read":
        # Read N 8-byte pointers from memory at va. {"cmd":"read","va":"0x...","n":16}
        a = _addr(req["va"])
        n = int(req.get("n", 16))
        mem = _program.getMemory()
        out = []
        for i in range(n):
            try:
                cur = a.add(i * 8)
                v = mem.getLong(cur) & 0xFFFFFFFFFFFFFFFF
                # what symbol/func is at v?
                va2 = _addr_space.getAddress(v)
                fm = _program.getFunctionManager()
                f = fm.getFunctionContaining(va2)
                sym = None
                if f is not None:
                    sym = f.getName() + ("" if f.getEntryPoint().getOffset() == v else "+%#x" % (v - f.getEntryPoint().getOffset()))
                out.append({"slot": i, "off": "0x%x" % cur.getOffset(), "val": "0x%x" % v, "func": sym})
            except Exception as e:
                out.append({"slot": i, "error": str(e)})
        return {"ok": True, "base": "0x%x" % a.getOffset(), "n": n, "ptrs": out}

    return {"ok": False, "error": "unknown cmd %r" % cmd}


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        for line in self.rfile:
            line = line.strip()
            if not line:
                continue
            try:
                req = json.loads(line)
                resp = handle(req)
            except Exception as e:
                resp = {"ok": False, "error": str(e), "trace": traceback.format_exc()}
            self.wfile.write((json.dumps(resp) + "\n").encode())
            self.wfile.flush()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main(argv=None):
    global PROJECT_PATH, PROJECT_NAME, PORT
    ap = argparse.ArgumentParser(description="Resident read-only Ghidra query server.")
    ap.add_argument("--project-dir", default=PROJECT_PATH,
                    help="directory holding the .gpr (default: $CK3_GHIDRA_PROJECT_DIR)")
    ap.add_argument("--project-name", default=PROJECT_NAME,
                    help="project name (default: $CK3_GHIDRA_PROJECT_NAME or ck3)")
    ap.add_argument("--port", type=int, default=PORT,
                    help="TCP port on 127.0.0.1 (default: $GHIDRA_MCP_PORT or 18741)")
    args = ap.parse_args(argv)
    if not args.project_dir:
        ap.error("no project: pass --project-dir or set CK3_GHIDRA_PROJECT_DIR")
    if not os.environ.get("GHIDRA_INSTALL_DIR"):
        ap.error("set GHIDRA_INSTALL_DIR to the Ghidra install root")
    PROJECT_PATH, PROJECT_NAME, PORT = args.project_dir, args.project_name, args.port
    _init()
    srv = Server((HOST, PORT), Handler)
    log("serving forever")
    srv.serve_forever()


if __name__ == "__main__":
    main()
