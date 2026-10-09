"""Run the actual linked Thumb-2 TX/parser against a handshaking GPIO peer.

python -m pip install -r tests/requirements.txt
python tests/arm_transport_tests.py build/arm/transport.elf
Instruction ticks are synthetic: this verifies behavior, NOT STM32 timing.
"""
from pathlib import Path
import random
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/python'))
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import *
from elftools.elf.elffile import ELFFile

PR, CLOCK = 0x40010414, 0xE0001004
PACKET, DATA, ARGS, STACK = (0x20008000, 0x20009000, 0x2000A000, 0x2000F000)
STOP = 0x0801F000
MASK = 0xFFFFFFFF
WIDTHS = [[0, 0, 1, 8, 12, 16, 32], [0, 1, 8, 12, 16, 32, 16, 32]]


def field(value, width):
    return [(value >> i) & 1 for i in reversed(range(width))]


def frame(slave, cells, state=0, info=b''):
    bits = field(11, 4)
    if slave:
        bits += field(state, 3) + field(len(info), 7)
        for ch in info:
            bits += field(ch, 8)
    bits += field(len(cells), 6)
    for typ, cmd, value in cells:
        bits += field(typ, 3)
        if not slave and typ:
            bits += field(cmd, 6)
        bits += field(value, WIDTHS[slave][typ])
    crc = MASK
    for bit in bits:
        top = (crc >> 31) ^ bit
        crc = ((crc << 1) ^ (0x04C11DB7 if top else 0)) & MASK
    return bits + field(crc, 32)


def packed(bits):
    result = bytearray((len(bits) + 7) // 8)
    for i, bit in enumerate(bits):
        result[i // 8] |= bit << (7 - i % 8)
    return bytes(result)


class Machine:
    def __init__(self, peer, start=100):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        for base, size in [(0x08000000, 0x20000), (0x20000000, 0x10000),
                           (0x40010000, 0x10000), (0xE0000000, 0x20000)]:
            self.uc.mem_map(base, size)
        for address, data in SEGMENTS:
            self.uc.mem_write(address, data)
        self.peer, self.ticks, self.steps, self.pr, self.odr = peer, start, 0, 0, 0
        self.lowest_sp = STACK
        peer.machine = self
        self.uc.hook_add(UC_HOOK_CODE, self.instruction)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.read)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write)

    def put(self, address, *values):
        self.uc.mem_write(address, struct.pack('<' + 'I' * len(values), *values))

    def words(self, address, count):
        return struct.unpack('<' + 'I' * count, self.uc.mem_read(address, 4 * count))

    def instruction(self, uc, address, size, _):
        self.steps += 1
        self.ticks = (self.ticks + 1) & MASK
        self.lowest_sp = min(self.lowest_sp, uc.reg_read(UC_ARM_REG_SP))

    def read(self, uc, access, address, size, value, _):
        if address == IDR:
            self.put(IDR, self.peer.read())
        elif address == CLOCK:
            self.put(CLOCK, self.ticks)
        elif address == PR:
            self.put(PR, self.pr)

    def write(self, uc, access, address, size, value, _):
        if address == BSRR:
            old = self.odr
            self.odr = ((old | (value & 0xFFFF)) & ~(value >> 16)) & 0xFFFF
            self.peer.write(old, self.odr)
        elif address == PR:
            self.pr &= ~value

    def call(self, symbol, args, budget=1000000):
        regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
        saved_regs = [UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
                      UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11]
        for i, reg in enumerate(saved_regs):
            self.uc.reg_write(reg, 0xA1100000 + i)
        for reg, arg in zip(regs, args):
            self.uc.reg_write(reg, arg)
        if len(args) > 4:
            self.put(STACK, *args[4:])
        self.uc.reg_write(UC_ARM_REG_SP, STACK)
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.uc.emu_start(SYMBOLS[symbol] | 1, STOP, count=budget)
        assert self.uc.reg_read(UC_ARM_REG_PC) == STOP, (symbol, 'did not return')
        assert self.uc.reg_read(UC_ARM_REG_SP) == STACK, 'stack corruption'
        for i, reg in enumerate(saved_regs):
            assert self.uc.reg_read(reg) == 0xA1100000 + i, 'AAPCS register corruption'
        return self.uc.reg_read(UC_ARM_REG_R0)


class Sender:
    """External sender, driving data before DOL, advances only after ACK LOW."""
    def __init__(self, bits, delay=0, stuck=None):
        self.bits, self.delay, self.stuck = bits, delay, stuck
        self.index, self.dol, self.pending, self.wait = 0, False, True, delay
        self.acks = 0

    def read(self):
        if self.pending and self.stuck != 'low':
            if self.wait:
                self.wait -= 1
            else:
                self.dol = self.index < len(self.bits)
                self.pending = False
        bit = self.bits[self.index] if self.index < len(self.bits) else 0
        return (SIGNAL_RX if self.dol else 0) | (bit << DATA_RX_PIN)

    def write(self, old, new):
        assert not new & DATA_TX, 'receiver activated its data emitter'
        if not old & SIGNAL_TX and new & SIGNAL_TX:
            assert self.dol and self.index < len(self.bits), 'ACK without data'
            self.acks += 1
            if self.stuck != 'high':
                self.dol = False
        if old & SIGNAL_TX and not new & SIGNAL_TX:
            # A timeout may abort a still-HIGH peer; normal completion must not.
            if self.stuck != 'high':
                assert not self.dol
            self.index += 1
            self.pending, self.wait = True, self.delay


class Receiver:
    """External receiver, including a short last ACK LOW followed by reply DOL."""
    def __init__(self, count, delay=0, stuck=None, turnaround=False):
        self.count, self.delay, self.stuck, self.turnaround = count, delay, stuck, turnaround
        self.bits, self.ack, self.target, self.wait = [], False, False, 0
        self.data_writes_during_ack = 0

    def read(self):
        if self.ack != self.target:
            if self.wait:
                self.wait -= 1
            elif self.stuck != ('low' if self.target else 'high'):
                if not self.target:
                    self.machine.pr |= SIGNAL_RX
                self.ack = self.target
                if self.turnaround and len(self.bits) == self.count and not self.ack:
                    self.ack = self.target = True  # response DOL already high
        return SIGNAL_RX if self.ack else 0

    def write(self, old, new):
        if (old ^ new) & DATA_TX:
            assert self.stuck or not old & SIGNAL_TX or len(self.bits) == self.count, 'data changed while DOL HIGH'
            if self.ack:
                self.data_writes_during_ack += 1
        if not old & SIGNAL_TX and new & SIGNAL_TX:
            assert not self.ack and len(self.bits) < self.count
            self.bits.append(1 if new & DATA_TX else 0)
            self.target, self.wait = True, self.delay
        if old & SIGNAL_TX and not new & SIGNAL_TX:
            if self.stuck != 'low':
                assert self.ack, 'sender removed DOL before ACK'
            if len(self.bits) == self.count:
                assert not new & DATA_TX, 'emitter not released before direction reversal'
            self.target, self.wait = False, self.delay


def receive(bits, slave, expected, state=0, info=b'', delay=0, start=100):
    peer = Sender(bits, delay)
    m = Machine(peer, start)
    result = m.call('mfi_receive_asm', [PACKET, slave, start, 900000])
    assert result == 0, ('receive', result, len(bits), peer.index)
    assert peer.index == len(bits) and peer.acks == len(bits)
    assert not m.odr & OUTPUTS
    data = bytes(m.uc.mem_read(PACKET, 636))
    assert data[0] == (state if slave else 0)
    assert data[1] == (len(info) if slave else 0)
    assert data[2:2 + data[1] + 1] == (info if slave else b'') + b'\0'
    assert data[130] == len(expected)
    for i, (typ, cmd, val) in enumerate(expected):
        base = 132 + 8 * i
        assert data[base] == typ
        assert data[base + 1] == (cmd if not slave and typ else 0)
        assert struct.unpack_from('<I', data, base + 4)[0] == val
    return m.steps


def transmit(bits, delay=0, turnaround=False, start=100):
    peer = Receiver(len(bits), delay, turnaround=turnaround)
    m = Machine(peer, start)
    m.uc.mem_write(DATA, packed(bits))
    m.put(ARGS, DATA, len(bits), start, 900000)
    assert m.call('mfi_send_asm', [ARGS]) == 0
    assert peer.bits == bits and not m.odr & OUTPUTS
    return m.steps


def tests():
    rng = random.Random(0xA55E)
    packets = 0
    for slave in [0, 1]:
        for n in range(70):
            cells = []
            for i in range([0, 63, 2][n % 3] if n < 3 else rng.randrange(64)):
                typ = i % len(WIDTHS[slave])
                width = WIDTHS[slave][typ]
                cells.append((typ, rng.randrange(64), rng.getrandbits(width)))
            info = bytes(rng.randrange(128) for _ in range([0, 127, 5][n % 3])) if slave else b''
            state = n % 3
            bits = frame(slave, cells, state, info)
            receive(bits, slave, cells, state, info, delay=n % 4, start=0xFFFFF000)
            transmit(bits, delay=n % 4,
                     turnaround=bool(n % 2), start=0xFFFFF000)
            packets += 1

    request = frame(0, [(1, 1, 0)])
    response = frame(1, [(1, 0, 1), (6, 0, 1204)])
    for slave, bits in [(0, request), (1, response)]:
        for i in range(len(bits)):
            corrupt = bits.copy()
            corrupt[i] ^= 1
            m = Machine(Sender(corrupt))
            status = m.call('mfi_receive_asm', [PACKET, slave, 100, 20000])
            assert status == 1 if i >= len(bits) - 32 else status != 0
        for length in range(len(bits)):
            m = Machine(Sender(bits[:length]))
            assert m.call('mfi_receive_asm', [PACKET, slave, 100, 10000]) == 3
        for stuck in ['low', 'high']:
            for start in [100, 0xFFFFFFF0]:
                m = Machine(Sender(bits, stuck=stuck), start)
                assert m.call('mfi_receive_asm', [PACKET, slave, start, 1000]) == 3
                assert not m.odr & OUTPUTS
                m = Machine(Receiver(len(bits), stuck=stuck), start)
                m.uc.mem_write(DATA, packed(bits))
                m.put(ARGS, DATA, len(bits), start, 1000)
                assert m.call('mfi_send_asm', [ARGS]) == 3
                assert not m.odr & OUTPUTS
    print(f'ARM ELF: {packets} RX + {packets} TX frames; all types, max sizes, GPIO ordering,')
    print('CRC corruption at every example bit, every truncated prefix, stuck lines,')
    print('short ACK LOW / direction reversal, DWT wrap and AAPCS passed.')


with open(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build/arm/transport.elf', 'rb') as f:
    elf = ELFFile(f)
    SEGMENTS = [(s['p_vaddr'], s.data()) for s in elf.iter_segments()
                if s['p_type'] == 'PT_LOAD' and s['p_filesz']]
    SYMBOLS = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    IDR = SYMBOLS['MFI_GPIO_BASE'] + 8
    BSRR = SYMBOLS['MFI_GPIO_BASE'] + 16
    SIGNAL_RX = SYMBOLS['MFI_SIGNAL_RX_MASK']
    DATA_TX = SYMBOLS['MFI_DATA_TX_MASK']
    SIGNAL_TX = SYMBOLS['MFI_SIGNAL_TX_MASK']
    DATA_RX_PIN = SYMBOLS['MFI_DATA_RX_PIN']
    OUTPUTS = DATA_TX | SIGNAL_TX


if __name__ == '__main__':
    tests()
