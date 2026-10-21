#!/usr/bin/env python3
"""Force the complete RTEMS kernel and libc API into the IOC kernel.

Writes a linker response file with one "-Wl,-u,<symbol>" per archive member
of the given libraries, so that every member is linked (with section garbage
collection off, every function in it then stays in the kernel and, via
rtems-syms, in the run-time loader's symbol table). A module built elsewhere
can thus use any RTEMS, libc or libm function without the kernel knowing the
module.

Members are skipped when they would conflict:
  - a member that defines a symbol the application objects define (RTEMS
    confdefs defaults in librtemscpu.a, e.g. _Workspace_Is_unified, which
    rtems_config.o overrides);
  - a member that defines a strong symbol another member also defines
    strongly (ambiguous defaults, both libc/librtemscpu variants);
  - members without a strong global data/text symbol;
  - members with an undefined reference that nothing in the link defines
    (optional RTEMS subsystems whose configuration tables only an
    application that uses them provides: flashdisk, nvdisk, ramdisk, the
    trace recorder, ...). "Nothing in the link" = the application objects,
    the --libs archives and the --pool entries (libraries linked on demand
    such as libbsd and the C++ runtime, the crt start files, and the BSP
    linker scripts whose symbol assignments count as definitions).

usage: libdl_api_symbols.py NM OUTPUT.rsp --objs OBJ.o... --libs LIB.a... --pool LIB.a...
"""
import subprocess, sys

def nm(nm_tool, path, undefined=False):
    flags = ["-g", "-A", "-u" if undefined else "--defined-only"]
    out = subprocess.run([nm_tool] + flags + [path],
                         capture_output=True, text=True, check=True).stdout
    members = {}
    for line in out.splitlines():
        # path:member:addr type name   (objects: path:addr type name; undefined: no addr)
        head, _, rest = line.rpartition(":")
        parts = rest.split()
        if len(parts) == 3:
            _addr, typ, name = parts
        elif len(parts) == 2:
            typ, name = parts
        else:
            continue
        members.setdefault(head, []).append((typ, name))
    return members

def main():
    args = sys.argv[1:]
    nm_tool, output = args[0], args[1]
    objs = args[args.index("--objs") + 1: args.index("--libs")]
    pool_at = args.index("--pool") if "--pool" in args else len(args)
    libs = args[args.index("--libs") + 1: pool_at]
    pool = args[pool_at + 1:]

    app_defined = set()
    for o in objs:
        for syms in nm(nm_tool, o).values():
            app_defined.update(name for _t, name in syms)

    members = {}
    for lib in libs:
        members.update(nm(nm_tool, lib))
    undefined = {}
    for lib in libs:
        undefined.update(nm(nm_tool, lib, undefined=True))


    # Definitions of any binding, counted across the forced libraries AND
    # the pool libraries. A member is not forced when a symbol it defines
    # is also defined by another member: strong twice (SHA-512 in
    # librtemscpu and libbsd) would be a duplicate, and weak default plus
    # strong alternative (bsp_interrupt_get_dispatch_table_slot in
    # irq-generic.c and the tracing variant in irq-record.c) is an override
    # that the normal link decides; forcing the alternative in changed the
    # interrupt dispatch (spurious-interrupt storm at boot, 2026-10-20).
    strong_count = {}
    def count(syms):
        for typ, name in syms:
            if typ in "TDBRGWV":
                strong_count[name] = strong_count.get(name, 0) + 1
    for syms in members.values():
        count(syms)
    for lib in pool:
        if lib.endswith(".a"):
            for syms in nm(nm_tool, lib).values():
                count(syms)

    pool_defined = set()
    for lib in pool:
        if lib.endswith((".a", ".o")):
            for syms in nm(nm_tool, lib).values():
                pool_defined.update(name for _t, name in syms)
        else:                                   # a linker script: symbol assignments and PROVIDEs
            import re
            with open(lib, errors="replace") as f:
                text = f.read()
            pool_defined.update(re.findall(r"PROVIDE\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)", text))
            pool_defined.update(re.findall(r"(?m)^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=[^=]", text))

    # candidates: not conflicting with the application, not ambiguous
    candidates = {}
    skipped_app = skipped_dup = skipped_none = 0
    for member, syms in members.items():
        names = {name for _t, name in syms}
        strong = [name for typ, name in syms if typ in "TDBRG"]
        if names & app_defined:
            skipped_app += 1
        elif any(strong_count[n] > 1 for n in names):
            skipped_dup += 1
        elif not strong:
            skipped_none += 1
        else:
            candidates[member] = (names, sorted(strong)[0],
                                  {name for typ, name in undefined.get(member, []) if typ == "U"})

    # Which members would the linker pull in on demand, and can every
    # reference then be satisfied? A member is doomed when one of its
    # references has no viable definer: not defined by the application, not
    # in a pool library, and every library member defining it is itself
    # doomed or collides with the application (it would come in as a
    # duplicate). Least fixpoint over "doomed"; everything else is viable
    # (flashdisk, nvdisk, the trace recorder ... reference configuration
    # tables nobody defines and fall out here, together with their callers).
    definers = {}
    for m, syms in members.items():
        for _t, n in syms:
            definers.setdefault(n, set()).add(m)
    app_members = {m for m, syms in members.items() if {n for _t, n in syms} & app_defined}
    needs_of = {m: {n for t, n in undefined.get(m, []) if t == "U"} for m in members}
    doomed = set()
    while True:
        changed = False
        for m in members:
            if m in doomed:
                continue
            for n in needs_of[m]:
                if n in app_defined or n in pool_defined:
                    continue
                if any(d not in doomed and d not in app_members for d in definers.get(n, ())):
                    continue
                doomed.add(m); changed = True
                break
        if not changed:
            break
    admissible = {m: v for m, v in candidates.items() if m not in doomed}
    skipped_unsat = len(candidates) - len(admissible)
    # every strong text/data symbol becomes a linker root: with section
    # garbage collection on (libbsd needs it), -u per member would keep only
    # that one function; -u per symbol keeps each function and its callees
    kept = set()
    for m in admissible:
        kept.update(name for typ, name in members[m] if typ in "TDBRG")
    kept = sorted(kept)

    with open(output, "w") as f:
        for name in kept:
            f.write("-Wl,-u,%s\n" % name)
    print("libdl_api_symbols: %d symbols of %d members forced into the link, skipped %d "
          "members (defined by the application), %d (ambiguous), %d (no strong symbol), "
          "%d (unsatisfiable references)"
          % (len(kept), len(admissible), skipped_app, skipped_dup, skipped_none, skipped_unsat))

if __name__ == "__main__":
    main()
