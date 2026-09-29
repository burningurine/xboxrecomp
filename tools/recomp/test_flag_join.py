"""Comparison snapshots must survive a join of different CMP operands."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator, _merge_flag_states
from tools.recomp.disasm import Operand

BASE = 0x10000

def translate_join(consumer=bytes.fromhex('0f95c0c3')):
    # test ecx,ecx; jz alternate; cmp eax,edx; jmp join; nop;
    # alternate: cmp ebx,esi; join: consumer.
    image = bytes.fromhex('85c9740539d0eb039039f3') + consumer
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                              entry_point=BASE, kernel_thunk_addr=BASE,
                              origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])

def test_different_cmp_operands_join_for_setne():
    code = translate_join()
    assert 'CMP_NE(_fa, _fb)' in code, code
    assert '_flags /* setne */' not in code

def test_different_cmp_operands_join_for_cmovne():
    code = translate_join(bytes.fromhex('0f45c7c3'))
    assert 'if (CMP_NE(_fa, _fb)) eax = edi;' in code, code

def test_unknown_path_is_not_guessed():
    assert _merge_flag_states([None, ('cmp', [])]) is None

def test_mixed_operations_are_not_guessed():
    a = Operand(type='reg', reg='eax')
    b = Operand(type='reg', reg='edx')
    assert _merge_flag_states([('cmp', [a, b]), ('test', [a, b])]) is None

def test_mixed_widths_are_not_guessed():
    wide = [Operand(type='reg', reg='eax'), Operand(type='reg', reg='edx')]
    narrow = [Operand(type='reg', reg='al'), Operand(type='reg', reg='dl')]
    assert _merge_flag_states([('cmp', wide), ('cmp', narrow)]) is None


def translate_mod_idiom(consumer_jcc):
    # and ebp,0x800007FF; jns L; dec ebp; or ebp,0xFFFFF800; inc ebp;
    # L: je/jne +0; ret  -- MSVC's signed `x % 2048` feeding a zero test.
    image = (bytes.fromhex('81e5ff070080' '7908' '4d' '81cd00f8ffff' '45')
             + bytes([consumer_jcc, 0x00]) + bytes.fromhex('c3'))
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_signed_mod_idiom_je_reads_result_register():
    code = translate_mod_idiom(0x74)
    assert '_flags' not in code.split('loc_')[-2] + code.split('loc_')[-1], code
    assert '(ebp == 0)' in code, code


def test_signed_mod_idiom_jne_reads_result_register():
    code = translate_mod_idiom(0x75)
    assert '(ebp != 0)' in code, code

def translate(image):
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_byte_add_jns_tests_bit_7():
    # add byte [esi+0xC], -16; jns +4; mov byte [esi+0xC], 0; ret -- Blinx's
    # player weight clamp. A zero-extended read never goes negative, so the
    # clamp never ran and the weight wrapped to 0xF0.
    code = translate(bytes.fromhex('80460cf0' '7904' 'c6460c00' 'c3'))
    assert '((int8_t)(MEM8(esi + 0xC)) >= 0)' in code, code


def test_word_sub_js_tests_bit_15():
    # sub ax, 1; js +0; ret
    code = translate(bytes.fromhex('6683e801' '7800' 'c3'))
    assert '((int16_t)(LO16(eax)) < 0)' in code, code


def test_result_join_through_a_store():
    # mov al,[esi+0xD]; add al,-6; jmp L; mov al,[esi+0xD]; add al,-12;
    # L: mov [esi+0xD],al; jns +4; mov byte [esi+0xD],0; ret -- the store
    # leaves al and the flags alone, so jns still reads the result in al.
    code = translate(bytes.fromhex('8a460d04faeb05' '8a460d04f4' '88460d' '7904'
                                   'c6460d00' 'c3'))
    assert '((int8_t)(LO8(eax)) >= 0)' in code, code
    assert 'if (_flags' not in code, code


def test_result_join_refused_when_the_result_is_overwritten():
    # As above but L: mov al,[esi]; jns -- al no longer holds the result.
    code = translate(bytes.fromhex('8a460d04faeb05' '8a460d04f4' '8a06' '7904'
                                   'c6460d00' 'c3'))
    assert '((int8_t)(LO8(eax)) >= 0)' not in code, code


if __name__ == '__main__':
    for _name, _fn in sorted(globals().items()):
        if _name.startswith('test_') and callable(_fn):
            _fn()
            print('ok ', _name)
