"""A failed indirect call must leave esp where the callee's ret would.

For a cdecl call (`push arg; call eax; add esp, 4`) the callee pops only the
return address; the caller pops the argument. RECOMP_ICALL_SAFE rewinds g_esp
to the saved value on a miss, so the save has to sit after the argument pushes
there -- saving before them made the caller pop the argument twice."""
import unittest

from .translator import _fixup_icall_esp_save


ICALL = ("    { uint32_t _icall_target = eax; PUSH32(esp, 0x000E817Du); "
         "RECOMP_ICALL_SAFE(_icall_target, _icall_esp); } /* indirect call */")


class CdeclIcallEspTest(unittest.TestCase):
    def test_cdecl_saves_after_the_argument_push(self):
        lines = ["    PUSH32(esp, esi);", ICALL, "", "loc_000E817D: ;",
                 "    esp = esp + 4;"]
        out = _fixup_icall_esp_save(lines)
        save = out.index("    { uint32_t _icall_esp = g_esp;")
        self.assertLess(out.index("    PUSH32(esp, esi);"), save)
        self.assertEqual(out[save + 1], ICALL)

    def test_callee_cleans_keeps_the_save_before_the_arguments(self):
        lines = ["    PUSH32(esp, esi);", ICALL, "", "loc_000E817D: ;",
                 "    eax = eax + 1;"]
        out = _fixup_icall_esp_save(lines)
        save = out.index("    { uint32_t _icall_esp = g_esp;")
        self.assertLess(save, out.index("    PUSH32(esp, esi);"))


if __name__ == "__main__":
    unittest.main()
