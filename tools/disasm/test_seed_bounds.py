"""
Self-check: a seed (or other weak entry) inside a function does not split it.

Run: py -3 tools/disasm/test_seed_bounds.py

Blinx's unresolved-stub audit seeded ~100 tail-jump targets that are labels
inside other functions. Every function start used to clamp the function before
it, so each seed cut its enclosing function in two. Where the cut went through
a switch, the arms landed in different pieces, the table no longer resolved
inside one function, and the lifter fell back to an indirect jump through the
dispatcher -- to case labels that are not entries. The CRT's number parser
(0x12F914) and printf state machine broke that way, and D3D CreateDevice then
asked for a 498 MB surface.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402
from tools.disasm.test_function_end import _Insn, _Engine  # noqa: E402


class _Section:
    name = ".text"
    virtual_addr = 0x1000
    virtual_size = 0x1000


class _Image:
    def get_section_at_va(self, addr):
        return _Section()


class _Labels:
    def get(self, addr):
        return None

    def auto_name_function(self, *args):
        pass


class _RangeEngine(_Engine):
    def get_instructions_in_range(self, start, end):
        return [i for a, i in sorted(self.by_addr.items()) if start <= a < end]


# A switch whose second arm (0x1030) is also reached by a tail jump from
# elsewhere, which is what gets it seeded.
#   0x1000  mov
#   0x1004  jmp [eax*4 + 0x1010]      table 0x1010..0x1020, inline after it
#   0x1020  mov / ret                  arm 1
#   0x1030  mov / ret                  arm 2
SWITCH = [
    _Insn(0x1000, 4),
    _Insn(0x1004, 7, "jmp", is_jump=True, jump_table=0x1010),
    _Insn(0x1020, 15),
    _Insn(0x102F, 1, "ret", is_ret=True),
    _Insn(0x1030, 4),
    _Insn(0x1034, 1, "ret", is_ret=True),
]


def _build(method_at_1030):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _RangeEngine(SWITCH, jump_tables={0x1010: 0x1020},
                              entries={0x1010: [0x1020, 0x1030, 0x1020, 0x1030]})
    det.image = _Image()
    det.labels = _Labels()
    det.functions = {}
    det._candidates = {0x1000: (0.9, "call_target"),
                       0x1030: (0.95, method_at_1030)}
    det._build_functions([_Section()])
    return det.functions


def test_seed_does_not_split_its_enclosing_function():
    funcs = _build("seed_vtable_thunk")
    assert funcs[0x1000].end == 0x1035, (
        f"switch function cut at the seed: end {funcs[0x1000].end:#x}")
    assert funcs[0x1030].end == 0x1035, (
        f"seed lost its own body: end {funcs[0x1030].end:#x}")


def test_imm_ref_and_tail_jump_targets_do_not_split_either():
    # An address taken as an immediate or reached by a tail jump is an entry
    # point, not evidence that the function before it ended.
    for method in ("imm_ref_target", "tail_jump_target"):
        funcs = _build(method)
        assert funcs[0x1000].end == 0x1035, (
            f"{method} cut the switch function: end {funcs[0x1000].end:#x}")


def test_other_starts_still_bound_the_function_before():
    # Unchanged for everything that is not a seed.
    funcs = _build("call_target")
    assert funcs[0x1000].end == 0x1030, (
        f"a detected start no longer bounds its predecessor: "
        f"end {funcs[0x1000].end:#x}")


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"PASS {name}")
            except AssertionError as e:
                failures += 1
                print(f"FAIL {name}: {e}")
    sys.exit(1 if failures else 0)
