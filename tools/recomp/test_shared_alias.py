"""
Self-check for shared-body aliases (FunctionTranslator.discover_shared_aliases).

Run: py -3 tools/recomp/test_shared_alias.py

An entry point inside another function (a seed, a tail-jump target) used to
get its own C function lifted from the entry to the host's end. A loop that
jumps back above the entry then had no target and became an unresolved stub,
i.e. a silent return. Blinx's CRT had nine of those after its stub audit
seeded such entries (0x11F1B3 -> 0x11F02E, ...).

The host is now emitted once as sub_H__body(int entry), entered through a
switch; sub_H and the alias are wrappers. The loop's back edge is a local goto
from either entry.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000
ENTRY = BASE + 3       # alias entry: inside the loop, below the loop head

#   BASE+0: dec ecx          <- loop head (above the entry)
#   BASE+1: nop
#   BASE+2: nop
#   BASE+3: inc eax          <- alias entry
#   BASE+4: test ecx, ecx
#   BASE+6: jnz BASE+0       (back edge above the entry)
#   BASE+8: ret
CODE = b"\x49\x90\x90\x40\x85\xC9\x75\xF8\xC3"


def _translator():
    config._install(
        [config.Section(".text", BASE, len(CODE), 0x0000, len(CODE), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="shared-alias-test")
    db = {
        BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(CODE), "_addr": BASE,
               "size": len(CODE), "detection_method": "prologue"},
        ENTRY: {"start": f"0x{ENTRY:08X}", "end": BASE + len(CODE), "_addr": ENTRY,
                "size": BASE + len(CODE) - ENTRY, "detection_method": "seed_vtable_thunk"},
    }
    return FunctionTranslator(CODE, db), db


def test_alias_shares_the_host_body():
    t, db = _translator()
    t.discover_shared_aliases()
    assert t.alias_host == {ENTRY: BASE}, t.alias_host
    host = t.translate_function(BASE, db[BASE])
    assert f"void sub_{BASE:08X}__body(int _entry)" in host, host
    assert f"case 1: goto loc_{ENTRY:08X};" in host, host
    assert f"loc_{ENTRY:08X}: ;" in host, host
    assert f"sub_{BASE:08X}__body(0);" in host, host
    # The back edge is a local goto, not a call to an unresolved stub.
    assert f"goto loc_{BASE:08X};" in host, host
    wrapper = t.alias_wrapper(ENTRY, f"sub_{ENTRY:08X}")
    assert f"sub_{BASE:08X}__body(1);" in wrapper, wrapper
    assert ENTRY not in t.alias_failed
    print("ok  alias_shares_the_host_body")


def test_mid_instruction_entry_is_not_an_alias():
    # BASE+5 is inside `test ecx, ecx`: not an instruction of the host.
    t, db = _translator()
    db[BASE + 5] = dict(db[ENTRY], _addr=BASE + 5, start=f"0x{BASE + 5:08X}")
    del db[ENTRY]
    t.discover_shared_aliases()
    assert t.alias_host == {}, t.alias_host
    host = t.translate_function(BASE, db[BASE])
    assert "__body" not in host, host
    print("ok  mid_instruction_entry_is_not_an_alias")


def test_excluded_host_keeps_separate_functions():
    # A host that recomp_manual.c replaces or wraps has no generated body to
    # share, so its entries keep their own translation.
    t, db = _translator()
    t.discover_shared_aliases(exclude={BASE})
    assert t.alias_host == {}, t.alias_host
    print("ok  excluded_host_keeps_separate_functions")


def test_seeded_host_takes_inner_seeds():
    # The outermost covering function is the host even when it is a seed
    # itself (Blinx's CRT printf body 0x11ED20 is seeded). Two nested seeds:
    # both entries go to the outer one, the inner one is not a host.
    t, db = _translator()
    db[BASE]["detection_method"] = "seed_vtable_thunk"
    db[BASE + 1] = dict(db[ENTRY], _addr=BASE + 1, start=f"0x{BASE + 1:08X}")
    t.discover_shared_aliases()
    assert t.alias_host == {BASE + 1: BASE, ENTRY: BASE}, t.alias_host
    print("ok  seeded_host_takes_inner_seeds")


def test_entry_past_the_host_end_keeps_its_body():
    # A host that stops short of the entry's own code (a junk host whose
    # stream happens to fall into step at the entry) does not hold it.
    t, db = _translator()
    db[BASE]["end"] = BASE + 6
    t.discover_shared_aliases()
    assert t.alias_host == {}, t.alias_host
    print("ok  entry_past_the_host_end_keeps_its_body")


def test_tail_jump_alias_is_never_a_host():
    # A tail_jump_alias's end is stale: it neither hosts nor bounds a root,
    # and one inside a root is taken whatever end it recorded.
    t, db = _translator()
    db[BASE]["detection_method"] = "tail_jump_alias"
    db[BASE + 1] = dict(db[ENTRY], _addr=BASE + 1, start=f"0x{BASE + 1:08X}",
                        detection_method="prologue")
    db[ENTRY]["detection_method"] = "tail_jump_alias"
    db[ENTRY]["end"] = BASE + 4                  # stale, shorter than the root
    t.discover_shared_aliases()
    assert t.alias_host == {ENTRY: BASE + 1}, t.alias_host
    print("ok  tail_jump_alias_is_never_a_host")


def test_alias_entry_keeps_the_hosts_flags():
    # An alias entry between a compare and its jcc: the fall-through path
    # still takes the compare's flags (the entry switch brings undefined
    # ones). D3D's PLL clock code 0x13FF73 guards `div` with such a `je`, and
    # the never-set `_flags` fallback divided by zero.
    #   BASE+0: cmp ecx, 0
    #   BASE+3: mov eax, ecx     <- alias entry
    #   BASE+5: je BASE+9
    #   BASE+7: div ecx
    #   BASE+9: ret
    code = b"\x83\xF9\x00\x89\xC8\x74\x02\xF7\xF1\xC3"
    entry = BASE + 3
    config._install(
        [config.Section(".text", BASE, len(code), 0x0000, len(code), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="shared-alias-test")
    db = {
        BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(code), "_addr": BASE,
               "size": len(code), "detection_method": "prologue"},
        entry: {"start": f"0x{entry:08X}", "end": BASE + len(code), "_addr": entry,
                "size": BASE + len(code) - entry, "detection_method": "imm_ref_target"},
    }
    t = FunctionTranslator(code, db)
    t.discover_shared_aliases()
    assert t.alias_host == {entry: BASE}, t.alias_host
    host = t.translate_function(BASE, db[BASE])
    assert f"loc_{entry:08X}: ;" in host, host
    assert "if (_flags" not in host, host
    assert "CMP_EQ(_fa, _fb)" in host, host
    print("ok  alias_entry_keeps_the_hosts_flags")


if __name__ == "__main__":
    test_alias_shares_the_host_body()
    test_mid_instruction_entry_is_not_an_alias()
    test_excluded_host_keeps_separate_functions()
    test_seeded_host_takes_inner_seeds()
    test_entry_past_the_host_end_keeps_its_body()
    test_tail_jump_alias_is_never_a_host()
    test_alias_entry_keeps_the_hosts_flags()
