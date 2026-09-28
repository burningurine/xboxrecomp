"""
Self-check: "mov edi, edi" before a switch table is padding, not a prologue.

Run: py -3 tools/disasm/test_hotpatch_pad.py

MSVC's two-byte nop 8B FF is the hot-patch prologue, and also what it uses to
align an embedded jump table. After a ret, the gap-prologue pass took the
padding in front of default.xbe's CRT table at 0x11E87C for a function start,
and the function end walk stepped over the table into its index bytes: 1.1 KB
of junk at 0x11E87A with two unresolved stubs (0x11E860, 0x1111FAA8).
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.test_decode_at import _engine, BASE  # noqa: E402


def _code():
    #  BASE+0: 8b ff           mov edi, edi        <- padding or prologue
    #  BASE+2: 55              push ebp
    #  BASE+3: 8b ec           mov ebp, esp
    #  BASE+5: c3              ret
    return b"\x8b\xff\x55\x8b\xec\xc3"


def test_hotpatch_prologue_is_a_prologue():
    eng, _ = _engine(BASE, _code())
    assert eng.probes_as_prologue(BASE)


def test_pad_before_a_jump_table_is_not():
    eng, _ = _engine(BASE, _code())
    eng.jump_tables[BASE + 2] = BASE + 14     # a table starts after the pad
    assert not eng.probes_as_prologue(BASE)


if __name__ == "__main__":
    assert isinstance(DisasmEngine, type)
    test_hotpatch_prologue_is_a_prologue()
    test_pad_before_a_jump_table_is_not()
    print("ok")
