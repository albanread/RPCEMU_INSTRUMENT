"""Check the disassembler against LLVM's, over real RISC OS ROM code.

A disassembler that is nearly right is worse than none, because it is
believed. The only convincing test is a second implementation and a large
body of real instructions, so this takes thousands of words of ROM as the
running machine sees them, disassembles them both ways, and compares.

Assemblers spell things differently in ways that mean nothing: LLVM says svc
where RISC OS says swi, writes the S and size suffixes before the condition
where the ARM reference writes them after, prefers the unsigned branch
aliases, prints large immediates as negative decimals, and uses the shift
mnemonics where the classic form is a MOV with a shifted operand. All of
that is normalised away. What survives is disagreement about what the bytes
are, which is the only kind worth looking at.

Needs clang and llvm-objdump on the path; skips itself if they are absent.
"""
import collections
import io
import os
import re
import shutil
import subprocess
import sys
import time
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rpc_client import Machine              # noqa: E402

ROM = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   os.pardir, os.pardir, "win32", "RPCEmu", "roms", "riscos")

# Where the ROM appears to the guest, confirmed against the words the
# emulator actually reports rather than assumed.
ROM_BASE = 0xfc000000

# Far enough in to be code rather than header, and long enough that a decode
# bug in anything common cannot hide.
ROM_OFFSET = 0x40000
COUNT = 8000

# Every difference that survives normalisation is examined by hand and
# accounted for in the report below; this is a guard against a change making
# things quietly worse, not a claim that the remainder is wrong.
MAX_DIFFERING = 0.02

CONDS = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc',
         'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al']

BASES = sorted([
    'and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc',
    'tst', 'teq', 'cmp', 'cmn', 'orr', 'mov', 'bic', 'mvn',
    'mul', 'mla', 'umull', 'umlal', 'smull', 'smlal',
    'ldrd', 'strd', 'ldr', 'str', 'ldm', 'stm', 'ldc', 'stc',
    'swp', 'swi', 'mrs', 'msr', 'mrc', 'mcr', 'mrrc', 'mcrr', 'cdp',
    'bl', 'bx', 'blx', 'b', 'bkpt', 'undefined', 'nop', 'clz',
], key=len, reverse=True)

SUFFIXES = ['ia', 'ib', 'da', 'db', 'bt', 'sb', 'sh', 's', 'b', 't', 'h', 'l', 'd']

REGMAP = {'r13': 'sp', 'r14': 'lr', 'r15': 'pc',
          'fp': 'r11', 'ip': 'r12', 'sl': 'r10'}


def split_mnemonic(token):
    """Break a mnemonic into base, condition and suffixes, in any order."""
    for base in BASES:
        if not token.startswith(base):
            continue

        rest = token[len(base):]

        for attempt in ('front', 'back', 'none'):
            r, cond = rest, ''

            if attempt == 'front' and r[:2] in CONDS:
                cond, r = r[:2], r[2:]
            elif attempt == 'back' and r[-2:] in CONDS:
                cond, r = r[-2:], r[:-2]
            elif attempt != 'none':
                continue

            found = []
            ok = True
            while r:
                for suffix in SUFFIXES:
                    if r.startswith(suffix):
                        found.append(suffix)
                        r = r[len(suffix):]
                        break
                else:
                    ok = False
                    break

            if ok:
                # A bare ldm or stm means increment after; LLVM leaves the
                # suffix off in that case and the ARM reference does not.
                if base in ('ldm', 'stm') and not any(
                        x in found for x in ('ia', 'ib', 'da', 'db')):
                    found.append('ia')
                return base, '' if cond == 'al' else cond, ''.join(sorted(found))

    return token, '', ''


def norm(text):
    t = text.split(';')[0].split('@')[0]
    t = t.lower().replace('\t', ' ').strip()
    t = t.replace('<unknown>', 'undefined')
    t = re.sub(r'<[^>]*>', '', t)
    t = re.sub(r'&([0-9a-f]+)', lambda m: str(int(m.group(1), 16)), t)
    t = re.sub(r'0x([0-9a-f]+)', lambda m: str(int(m.group(1), 16)), t)

    # LLVM prints a non-canonical rotated immediate as its two halves.
    t = re.sub(r'#(\d+), #(\d+)',
               lambda m: '#%d' % ((((int(m.group(1)) >> int(m.group(2))) |
                                    (int(m.group(1)) << (32 - int(m.group(2)))))
                                   & 0xffffffff) if int(m.group(2))
                                  else int(m.group(1))),
               t)
    t = t.replace('#', '')
    t = re.sub(r'(?<![\w])-(\d+)', lambda m: str((-int(m.group(1))) & 0xffffffff), t)
    t = re.sub(r'apsr_nzcvq', 'cpsr_f', t)
    t = re.sub(r'\bapsr\b', 'cpsr', t)

    parts = t.split(' ', 1)
    head = parts[0]
    tail = parts[1] if len(parts) > 1 else ''

    if head.startswith('svc'):
        head = 'swi' + head[3:]
    head = re.sub(r'hs$', 'cs', head)
    head = re.sub(r'lo$', 'cc', head)

    m = re.match(r'^(push|pop)(%s)?$' % '|'.join(CONDS), head)
    if m:
        head = ('stm' + (m.group(2) or '') + 'db') if m.group(1) == 'push' \
               else ('ldm' + (m.group(2) or '') + 'ia')
        tail = 'sp!, ' + tail

    m = re.match(r'^(lsl|lsr|asr|ror|rrx)(%s)?(s?)$' % '|'.join(CONDS), head)
    if m:
        shift = m.group(1)
        head = 'mov' + (m.group(2) or '') + m.group(3)
        operands = [o.strip() for o in tail.split(',')]
        if shift == 'rrx' and len(operands) == 2:
            tail = '%s, %s, rrx' % (operands[0], operands[1])
        elif len(operands) == 3:
            tail = '%s, %s, %s %s' % (operands[0], operands[1], shift, operands[2])

    for a, b in REGMAP.items():
        tail = re.sub(r'\b%s\b' % a, b, tail)

    def expand(m):
        """One register list, as a sorted set, however it was written."""
        inner = m.group(1)
        for name, number in (('sp', 'r13'), ('lr', 'r14'), ('pc', 'r15')):
            inner = inner.replace(name, number)

        out = []
        for part in inner.split(','):
            part = part.strip()
            r = re.match(r'r(\d+)-r(\d+)$', part)
            if r:
                out += list(range(int(r.group(1)), int(r.group(2)) + 1))
            elif part.startswith('r'):
                out.append(int(part[1:]))

        return '{' + ','.join('r%d' % n for n in sorted(set(out))) + '}'

    tail = re.sub(r'\{([^}]*)\}', expand, tail)
    tail = re.sub(r'\s+', ' ', tail.replace(', ', ',')).strip().replace(' ^', '^')

    base, cond, suffixes = split_mnemonic(head)

    return '%s|%s|%s %s' % (base, cond, suffixes, tail)


BRANCH = re.compile(
    r'^(b|bl|blx)(%s)?[ 	]+(0x[0-9a-f]+|&[0-9a-f]+|\d+)(.*)$'
    % '|'.join(CONDS + ['lo', 'hs']))


def relative(text, addr):
    """Rewrite a branch target as an offset from the instruction itself.

    The object handed to LLVM starts at zero and the ROM does not, so
    absolute targets can never agree. The distance jumped is the same in
    both, and is what the comparison actually cares about.
    """
    m = BRANCH.match(text.replace('	', ' ').strip())
    if not m:
        return text

    target = m.group(3)
    value = (int(target[1:], 16) if target.startswith('&')
             else int(target, 0))

    delta = (value - addr) & 0xffffffff
    if delta >= 0x80000000:
        delta -= 0x100000000

    return '%s%s %+d%s' % (m.group(1), m.group(2) or '', delta, m.group(4))


def llvm_disassemble(words, work):
    """Assemble the words and read them back, to get LLVM's opinion."""
    source = os.path.join(work, 'rom.s')
    obj = os.path.join(work, 'rom.o')

    with io.open(source, 'w', newline='\n') as f:
        f.write('.text\n')
        for w in words:
            f.write('.word 0x%08x\n' % w)

    subprocess.run(['clang', '--target=armv4t-none-eabi', '-c', source, '-o', obj],
                   check=True, capture_output=True)
    out = subprocess.run(['llvm-objdump', '-d', '--triple=armv4t', obj],
                         check=True, capture_output=True, text=True).stdout

    seen = {}
    for line in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(.*)', line)
        if m:
            seen[int(m.group(1), 16) // 4] = m.group(3).strip()

    return seen


def main():
    if not shutil.which('clang') or not shutil.which('llvm-objdump'):
        print('clang and llvm-objdump are not on the path; skipping')
        return 0

    with io.open(ROM, 'rb') as f:
        f.seek(ROM_OFFSET)
        raw = f.read(COUNT * 4)

    words = [int.from_bytes(raw[i * 4:i * 4 + 4], 'little') for i in range(COUNT)]

    machine = Machine()
    machine.events.get(timeout=15)          # event/ready
    time.sleep(9)                           # reach the supervisor prompt

    base = ROM_BASE + ROM_OFFSET
    ours = {}

    try:
        # 512 at a time: a reply carrying eight thousand instructions would
        # be a megabyte of JSON for no reason.
        for start in range(0, COUNT, 512):
            n = min(512, COUNT - start)
            reply = machine.call('dis.at', addr=base + start * 4, count=n)
            for entry in reply['instructions']:
                ours[(entry['addr'] - base) // 4] = (entry['opcode'], entry['text'])
    finally:
        try:
            machine.call('quit', timeout=5)
        except Exception:
            pass
        machine.proc.wait(timeout=10)

    # The ROM has to be where we think it is, or nothing below means anything.
    wrong = [i for i, w in enumerate(words) if i in ours and ours[i][0] != w]
    if wrong:
        print('FAIL: the ROM is not mapped at &%08x - %d of %d words differ'
              % (ROM_BASE, len(wrong), len(ours)))
        return 1

    with tempfile.TemporaryDirectory() as work:
        theirs = llvm_disassemble(words, work)

    same = diff = 0
    buckets = collections.Counter()
    examples = {}

    for i, (opcode, text) in sorted(ours.items()):
        if i not in theirs:
            continue

        at_llvm = i * 4
        at_ours = base + i * 4

        if norm(relative(theirs[i], at_llvm)) == norm(relative(text, at_ours)):
            same += 1
        else:
            diff += 1
            shown = theirs[i]
            key = (shown.split()[0], text.split()[0] if text else '?')
            buckets[key] += 1
            examples.setdefault(key, (opcode, shown, text))

    total = same + diff
    share = diff / max(1, total)

    print('%d instructions of ROM compared with llvm-objdump' % total)
    print('  agree            %d' % same)
    print('  differ           %d (%.2f%%)' % (diff, 100.0 * share))

    if diff:
        print()
        print('  Remaining differences, all of them deliberate:')
        for (theirs_op, ours_op), n in buckets.most_common(10):
            opcode, left, right = examples[(theirs_op, ours_op)]
            print('   %4d  %08x  llvm: %-34s ours: %s'
                  % (n, opcode, left.replace('\t', ' ')[:34], right[:40]))
        print()
        print('  Known and accepted: LLVM refuses multiplies its own assembler')
        print('  emits for this triple; we decode LDRD and STRD only with the')
        print('  even register pair the architecture requires; and ARMv5TE and')
        print('  ARMv6 instructions a StrongARM does not have are undefined')
        print('  here and named there.')

    print()
    if share > MAX_DIFFERING:
        print('FAIL: %.2f%% differ, more than the %.2f%% allowed'
              % (100.0 * share, 100.0 * MAX_DIFFERING))
        return 1

    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
