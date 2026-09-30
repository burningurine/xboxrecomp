"""A function table inside a code section names functions too.

XAPI keeps its USB class-driver table -- each driver's add/remove device
routines -- at the head of its own executable XPP section, and nothing else
takes those routines' addresses. _pass_data_ptr_targets scanned only data
sections, so they were never found: in Blinx the controller's driver and five
others were missing, and a plugged-in pad enumerated and then called into
nothing (an unresolved indirect call to 0x00192ED0).

Run: py -3 -m pytest tools/disasm/test_code_section_tables.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import Function, FunctionDetector  # noqa: E402

BASE = 0x00190000


class _Section:
    def __init__(self, name, va, data, executable=True, flags="W, PRE, X"):
        self.name = name
        self.virtual_addr = va
        self.virtual_size = len(data)
        self.executable = executable
        self.flags = flags
        self.data = data


class _Image:
    def __init__(self, *sections):
        self.sections = list(sections)
        self.base_address = min(s.virtual_addr for s in sections)
        self.image_size = max(s.virtual_addr + s.virtual_size
                              for s in sections) - self.base_address

    def get_section_at_va(self, va):
        for s in self.sections:
            if s.virtual_addr <= va < s.virtual_addr + s.virtual_size:
                return s
        return None

    def get_section_data(self, section):
        return section.data

    def read_bytes_at_va(self, va, size):
        s = self.get_section_at_va(va)
        if not s:
            return b""
        off = va - s.virtual_addr
        return s.data[off:off + size]


def _xpp(table_targets, func_b=bytes.fromhex("66a100001900c3"), **kw):
    """XPP-like section: a pointer table, then a known function A, then
    function B, which only the table names. B starts the way XAPI's XID
    routine does (mov ax, [moffs]), which no prologue pass recognises."""
    table = b"".join(struct.pack("<I", t) for t in table_targets)
    table += b"\x00" * (0x20 - len(table))
    func_a = bytes.fromhex("558bec5dc3") + b"\xcc" * 11      # at +0x20
    return _Section("XPP", BASE, table + func_a + func_b, **kw)   # B at +0x30


def _sweep_from(det, sec, off):
    """Record the instructions from +off, as the sweep would at a start."""
    for insn in det.engine._cs.disasm(sec.data[off:], BASE + off):
        det.engine.instructions[insn.address] = det.engine._classify_instruction(insn)


def _detector(section):
    image = _Image(section)
    engine = DisasmEngine(image)
    for insn in engine._cs.disasm(section.data, section.virtual_addr):
        engine.instructions[insn.address] = engine._classify_instruction(insn)
    det = FunctionDetector(engine, image, None, None)
    det.functions[BASE + 0x20] = Function(start=BASE + 0x20, end=BASE + 0x25,
                                          name="sub_A", section="XPP")
    return det


def test_table_in_a_code_section_names_a_missed_function():
    sec = _xpp([BASE + 0x20, BASE + 0x30])
    det = _detector(sec)
    # The table's decoded bytes are in the sweep too; B must still start at
    # its own boundary.
    det.engine.instructions.pop(BASE + 0x2E, None)
    for insn in det.engine._cs.disasm(sec.data[0x30:], BASE + 0x30):
        det.engine.instructions[insn.address] = det.engine._classify_instruction(insn)
    det._pass_data_ptr_targets([sec])
    assert BASE + 0x30 in det._alias_entries


def test_bytes_inside_a_function_are_not_read_as_a_table():
    # A's own bytes, read as a dword, point nowhere useful; and a pointer
    # word that only appears inside a covered function is not a table entry.
    sec = _xpp([])
    det = _detector(sec)
    det.functions[BASE] = Function(start=BASE, end=BASE + 0x20, name="sub_T", section="XPP")
    sec.data = struct.pack("<I", BASE + 0x30) + sec.data[4:]
    for insn in det.engine._cs.disasm(sec.data[0x30:], BASE + 0x30):
        det.engine.instructions[insn.address] = det.engine._classify_instruction(insn)
    det._pass_data_ptr_targets([sec])
    assert BASE + 0x30 not in det._alias_entries


def test_jump_table_targets_inside_a_function_add_nothing():
    # A switch's table after its function: the targets are case labels
    # inside the body, not entry points.
    sec = _xpp([BASE + 0x21, BASE + 0x23])
    det = _detector(sec)
    _sweep_from(det, sec, 0x20)
    det._pass_data_ptr_targets([sec])
    assert BASE + 0x21 not in det._alias_entries
    assert BASE + 0x23 not in det._alias_entries


def test_sections_loaded_on_demand_are_not_read():
    # Blinx marks its models and maps executable but does not preload them;
    # 40 MB of those read as tables added 12,000 entries.
    sec = _xpp([BASE + 0x20, BASE + 0x30], flags="W, X")
    det = _detector(sec)
    _sweep_from(det, sec, 0x30)
    det._pass_data_ptr_targets([sec])
    assert BASE + 0x30 not in det._alias_entries


def test_a_table_linking_its_own_entries_names_nothing():
    # XAPI's table points at its own descriptors, in the same gap as the
    # table; bytes there can decode to a ret without being code.
    sec = _xpp([BASE + 0x10])
    sec.data = sec.data[:0x10] + bytes.fromhex("33c0c3") + sec.data[0x13:]
    det = _detector(sec)
    _sweep_from(det, sec, 0x10)
    det._pass_data_ptr_targets([sec])
    assert BASE + 0x10 not in det._alias_entries


def test_a_long_routine_named_only_by_a_code_table_is_found():
    # XAPI's XID routine runs 66 instructions before its first ret; the
    # 64-instruction probe used for data words rejected it.
    long_b = bytes.fromhex("66a100001900") + bytes.fromhex("40") * 70 + bytes.fromhex("c3")
    sec = _xpp([BASE + 0x30], func_b=long_b)
    det = _detector(sec)
    _sweep_from(det, sec, 0x30)
    det._pass_data_ptr_targets([sec])
    assert BASE + 0x30 in det._alias_entries


def test_a_switch_table_names_no_functions():
    # The engine found the table behind a `jmp [reg*4 + table]`: its entries
    # are case labels, even where the switch's body is not measured yet
    # (Blinx 0x00122308, 85 of them).
    sec = _xpp([BASE + 0x30])
    det = _detector(sec)
    det.engine.jump_tables[BASE] = BASE + 4
    _sweep_from(det, sec, 0x30)
    det._pass_data_ptr_targets([sec])
    assert BASE + 0x30 not in det._alias_entries


def test_a_target_in_the_middle_of_a_block_names_nothing():
    # Uncovered code read as words: a call's bytes can point just past an
    # ordinary instruction, where no function begins.
    mid = bytes.fromhex("33c0") + bytes.fromhex("66a100001900c3")
    sec = _xpp([BASE + 0x32], func_b=mid)
    det = _detector(sec)
    _sweep_from(det, sec, 0x30)
    det._pass_data_ptr_targets([sec])
    assert BASE + 0x32 not in det._alias_entries
