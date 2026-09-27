"""rcl/rcr rotate through carry.

The CRT's 64-bit divide helpers shift a 64-bit value right one bit as
"shr hi, 1 ; rcr lo, 1": rcr moves the bit shr pushed out of the high half
into the top of the low half. Left as TODO comments, the carry vanished and
those divides returned wrong results with no other sign."""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block
from .translator import FunctionTranslator


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(v):
    return Operand(type="imm", imm=v)


class RotateThroughCarryTest(unittest.TestCase):
    def _lift(self, insns):
        lifter = Lifter()
        lifter.needs_cf = True
        lifted, _ = lift_basic_block(lifter, BasicBlock(start=0, instructions=insns))
        return "\n".join(lifted)

    def test_shr_rcr_pair_moves_the_carry(self):
        shr = Instruction(0, 2, "shr", "edx, 1", "d1ea")
        shr.operands = [_reg("edx"), _imm(1)]
        rcr = Instruction(2, 2, "rcr", "eax, 1", "d1d8")
        rcr.operands = [_reg("eax"), _imm(1)]
        out = self._lift([shr, rcr])
        self.assertNotIn("TODO", out)
        self.assertIn("/* rcr */", out)
        # the shr's carry-out is produced before the rcr consumes it
        self.assertLess(out.index("_cf = "), out.index("/* rcr */"))
        self.assertIn("% 33u", out)

    def test_rcl_is_lifted(self):
        rcl = Instruction(0, 2, "rcl", "ebx, 1", "d1d3")
        rcl.operands = [_reg("ebx"), _imm(1)]
        out = self._lift([rcl])
        self.assertNotIn("TODO", out)
        self.assertIn("/* rcl */", out)

    def test_jc_after_rcr_reads_carry(self):
        from .lifter import _make_condition as resolve
        cond = resolve("jb", "rcr", [_reg("eax"), _imm(1)])
        self.assertEqual(cond[0], "_cf")

    def test_translator_declares_cf_for_rcr(self):
        rcr = Instruction(0, 2, "rcr", "eax, 1", "d1d8")
        rcr.operands = [_reg("eax"), _imm(1)]
        self.assertTrue(FunctionTranslator._function_needs_cf([rcr]))


if __name__ == "__main__":
    unittest.main()
