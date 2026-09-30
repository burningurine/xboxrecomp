"""A function recomp_manual.c wraps (defines sub_X, calls sub_X_gen) must be
reached through the wrapper from direct call sites too.

The --exclude-manual wrap mode renames the generated body to sub_X_gen in the
function database. The lifter names call targets from that database, so
without wrapped_functions every direct caller called sub_X_gen and the
hand-written wrapper only ever ran for indirect calls."""

from tools.recomp.disasm import Instruction
from tools.recomp.lifter import Lifter

TARGET = 0x00076E20


def _direct_call():
    insn = Instruction(0x000A2DED, 5, "call", f"0x{TARGET:x}", "e800000000")
    insn.call_target = TARGET
    return insn


def test_wrapped_direct_call_names_the_wrapper():
    lifter = Lifter(func_db={TARGET: {"name": "sub_00076E20_gen"}})
    lifter.wrapped_functions[TARGET] = "sub_00076E20"
    out = "\n".join(lifter.lift_instruction(_direct_call()))
    assert "RECOMP_ABI_CALL(0x00076E20u, sub_00076E20)" in out, out
    assert "sub_00076E20_gen" not in out, out


def test_unwrapped_call_uses_database_name():
    lifter = Lifter(func_db={TARGET: {"name": "sub_00076E20_gen"}})
    out = "\n".join(lifter.lift_instruction(_direct_call()))
    assert "sub_00076E20_gen" in out, out


if __name__ == "__main__":
    test_wrapped_direct_call_names_the_wrapper()
    test_unwrapped_call_uses_database_name()
    print("test_wrap_call_routing: OK")
