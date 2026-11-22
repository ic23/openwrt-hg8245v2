"""Run just the stock ARM SPI control path against scripted MMIO, not hardware.

Dependencies: pyelftools and unicorn (tested with Unicorn 2.1.4).
No firmware boot, system calls, device access, or analog initialization.
"""

import argparse
import json
import sys
from collections import deque
from hashlib import sha256
from io import BytesIO
from pathlib import Path
from struct import pack, pack_into, unpack_from

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from unicorn import (
    Uc, UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_ARM, UC_PROT_EXEC, UC_PROT_READ, UC_PROT_WRITE,
)
from unicorn.arm_const import (
    UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
    UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_CPU_ARM_1176,
)

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "firmware_current/tools"))
from extract_squashfs3_full import FS

STOCK = {
    "hisi_sdk/hi_spi": "cead6a69a3f885ba8eb1419be4e78077a2211098685dcd88cd3f806155353154",
    "wap/hw_module_spi": "0657deef0813706a2d44fe31d3dbab745ae0edd7d9eecc5d4c9bf25c8f33ccff",
    "wap/hw_soc_sd5113": "599af8d24d5a096202fc6d0f5fc53dd621c158f27af260f9642b112890e2966b",
    "wap/hw_ker_codec_le88226": "ad5f9c34191735942d397251194405068096b0940a518db9ba009ccb6056f7c6",
}
ROOTFS = ROOT / "firmware_current/extracted/rootfs.bin"
CRG, GPIO, SPI = 0x16140000, 0x16146000, 0x16150000
CRG_VA = 0xf7140000
STACK, END, STUBS, DATA = 0x21000000, 0x11000000, 0x11001000, 0x22000000
ARGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)


class Replay:
    def __init__(self, rootfs, replies, modules=STOCK):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
        self.uc.mem_map(STACK, 0x10000, UC_PROT_READ | UC_PROT_WRITE)
        self.uc.mem_map(DATA, 0x1000, UC_PROT_READ | UC_PROT_WRITE)
        self.uc.mem_write(DATA, b"\x01")  # external g_uc_chip_num for CS0
        self.uc.mem_map(END, 0x10000)
        self.uc.hook_add(UC_HOOK_CODE, self.stub, begin=STUBS, end=END + 0xffff)
        self.sections, self.symbols = {}, {}
        self.stubs, self.stub_names = {}, {}
        self.phase, self.events = None, []
        self.reply = list(replies)
        self.status_script = None
        self.status_tail = 0
        self.regs = {
            CRG: {0x18: 0x08010000, 0x1c: 0xbffbfff0, 0x24: 0, 0x28: 0, 0x2c: 0xefffc000,
                  0x40: 0xa4255000, 0x48: 0x000f0aff, 0x54: 0xac47e0b5,
                  0x60: 0x02054505, 0x64: 0x00a3d70a},
            GPIO: {0x08: 0},
            SPI: {0x00: 7, 0x08: 0, 0x10: 0, 0x14: 0, 0x2c: 0x3f},
        }
        self.received = None
        for addr in (CRG, CRG_VA, GPIO, SPI):
            phys = CRG if addr == CRG_VA else addr
            self.uc.mmio_map(addr, 0x1000, self.mmio_read, phys,
                             self.mmio_write, phys)
        self.load(rootfs, modules)

    def load(self, path, modules):
        fs = FS(path)
        _, entries = fs.walk()
        files = {name: inode for name, inode, kind, _ in entries if kind == 2}
        elfs = {}
        cursor = 0x10000000
        for mod, digest in modules.items():
            b = fs.read_file(fs.inodes[files[f"lib/modules/{mod}.ko"]])
            if sha256(b).hexdigest() != digest:
                raise ValueError(f"unexpected {mod} bytes")
            elf = elfs[mod] = ELFFile(BytesIO(b))
            if not elf.little_endian or elf["e_machine"] != "EM_ARM" or elf["e_type"] != "ET_REL":
                raise ValueError(f"not little-endian ARM ET_REL: {mod}")
            for index, sec in enumerate(elf.iter_sections()):
                if not (sec["sh_flags"] & 2) or not sec["sh_size"]:
                    continue
                size = (sec["sh_size"] + 0xfff) & ~0xfff
                self.sections[mod, index] = cursor
                self.uc.mem_map(cursor, size)
                if sec["sh_type"] != "SHT_NOBITS":
                    self.uc.mem_write(cursor, sec.data())
                cursor += size
            for sym in elf.get_section_by_name(".symtab").iter_symbols():
                base = self.sections.get((mod, sym["st_shndx"]))
                if base is not None and sym.name:
                    self.symbols[mod, sym.name] = base + sym["st_value"]

        for mod, elf in elfs.items():
            for sec in elf.iter_sections():
                if not isinstance(sec, RelocationSection):
                    continue
                base = self.sections.get((mod, sec["sh_info"]))
                if base is None:
                    continue
                data = bytearray(elf.get_section(sec["sh_info"]).data())
                symbols = elf.get_section(sec["sh_link"])
                for r in sec.iter_relocations():
                    offset = r["r_offset"]
                    sym = symbols.get_symbol(r["r_info_sym"])
                    target = self.sections.get((mod, sym["st_shndx"]))
                    if target is not None:
                        target += sym["st_value"]
                    elif sym["st_shndx"] == "SHN_UNDEF":
                        matches = [a for (m, name), a in self.symbols.items() if name == sym.name]
                        target = matches[0] if len(matches) == 1 else self.import_address(sym.name)
                    else:
                        raise ValueError(f"unhandled symbol {mod}:{sym.name} {sym['st_shndx']}")
                    word = unpack_from("<I", data, offset)[0]
                    kind = r["r_info_type"]
                    if kind == 2:  # R_ARM_ABS32
                        value = (word + target) & 0xffffffff
                    elif kind in (28, 29):  # R_ARM_CALL, R_ARM_JUMP24 (ARM state)
                        addend = ((word & 0xffffff) ^ 0x800000) - 0x800000
                        disp = (addend << 2) + target - (base + offset)
                        if disp & 3 or not (-0x2000000 <= disp < 0x2000000):
                            raise ValueError(f"branch out of range: {mod}:{sym.name}")
                        value = (word & 0xff000000) | ((disp >> 2) & 0xffffff)
                    else:
                        raise ValueError(f"unhandled ARM relocation {kind}")
                    pack_into("<I", data, offset, value)
                self.uc.mem_write(base, bytes(data))
            for index, sec in enumerate(elf.iter_sections()):
                base = self.sections.get((mod, index))
                if base is None:
                    continue
                perms = UC_PROT_READ
                if sec["sh_flags"] & 1:
                    perms |= UC_PROT_WRITE
                if sec["sh_flags"] & 4:
                    perms |= UC_PROT_EXEC
                self.uc.mem_protect(base, (sec["sh_size"] + 0xfff) & ~0xfff, perms)

    def import_address(self, name):
        if name == "g_uc_chip_num":
            return DATA
        if name not in self.stubs:
            address = STUBS + len(self.stubs) * 4
            self.stubs[name] = address
            self.stub_names[address] = name
            self.uc.mem_write(address, b"\x1e\xff\x2f\xe1")  # bx lr
        return self.stubs[name]

    def stub(self, uc, address, _size, _user_data):
        name = self.stub_names.get(address)
        a, b, c, _ = [uc.reg_read(reg) for reg in ARGS]
        ret = 0
        if name == "__ioremap":
            if not any(base <= a and a + b <= base + 0x1000 for base in self.regs):
                raise ValueError(f"unmodelled ioremap {a:#x}/{b:#x}")
            ret = a
        elif name == "__memzero":
            uc.mem_write(a, bytes(b))
        elif name in ("memset", "HW_LDSP_Kernel_MemSet"):
            uc.mem_write(a, bytes((b & 0xff,)) * c)
            ret = a
        elif name == "memcpy":
            uc.mem_write(a, bytes(uc.mem_read(b, c)))
            ret = a
        elif name in ("__aeabi_uidiv", "__aeabi_uidivmod"):
            ret, rem = divmod(a, b)
            if name.endswith("mod"):
                uc.reg_write(UC_ARM_REG_R1, rem)
        elif name == "__const_udelay":
            self.events.append((self.phase, "delay-xloops", a))
        elif name not in ("printk", "hw_recorderr_4"):
            raise ValueError(f"unmodelled import {name} (LR={uc.reg_read(UC_ARM_REG_LR):#x})")
        uc.reg_write(UC_ARM_REG_R0, ret)

    def mmio_read(self, _, offset, size, base):
        if size != 4:
            raise ValueError(f"unexpected MMIO width {size}")
        if base == SPI and not self.regs[CRG][0x2c] & (1 << 26):
            raise ValueError("SPI MMIO while clock disabled")
        if base == SPI and offset == 0x28:
            if self.status_script is not None:
                status = self.status_script.popleft() if self.status_script else self.status_tail
                self.events.append((self.phase, "status", base + offset, status))
                return status
            return 6 | (8 if self.received is not None else 0)
        if base == SPI and offset == 0x60:
            if self.received is None:
                raise ValueError("RX FIFO read without scripted data")
            word, self.received = self.received, None
            self.events.append((self.phase, "read", base + offset, word))
            return word
        return self.regs[base][offset]

    def mmio_write(self, uc, offset, size, value, base):
        if size != 4:
            raise ValueError(f"unexpected MMIO width {size}")
        self.events.append((self.phase, "write", base + offset, value))
        regs = self.regs[base]
        if base == CRG and offset in (0x24, 0x28):
            regs[0x2c] = (regs[0x2c] | value) if offset == 0x24 else (regs[0x2c] & ~value)
            return
        if base == SPI:
            if not self.regs[CRG][0x2c] & (1 << 26):
                raise ValueError("SPI MMIO while clock disabled")
            if offset == 0x60:
                if not regs[0x08] & 1 or (regs[0x00] & 0x300) not in (0x100, 0x200):
                    raise ValueError("data write outside a configured transfer")
                if self.phase == "Vp880Init" and not uc.reg_read(UC_ARM_REG_CPSR) & 0x80:
                    raise ValueError("unexpected interrupt state in stock MPI transaction")
                if regs[0x00] & 0x200:
                    if value != 0xffff or self.received is not None:
                        raise ValueError("unexpected RX exchange")
                    self.received = self.reply.pop(0)
        regs[offset] = value

    def run(self, mod, name, *args, end=END, budget=300_000, timeout_us=2_000_000):
        self.phase = name
        self.uc.reg_write(UC_ARM_REG_CPSR, 0x13)
        self.uc.reg_write(UC_ARM_REG_SP, STACK + 0xff00)
        self.uc.reg_write(UC_ARM_REG_LR, END)
        for reg, val in zip(ARGS, args):
            self.uc.reg_write(reg, val)
        self.uc.emu_start(self.symbols[mod, name], end, timeout=timeout_us, count=budget)
        if self.uc.reg_read(UC_ARM_REG_PC) != end:
            raise ValueError(f"instruction/time limit or wrong return in {name}")
        return self.uc.reg_read(UC_ARM_REG_R0)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--rootfs", type=Path, default=ROOTFS)
    p.add_argument("--reply", choices=("04b3", "ffff"), default="04b3",
                   help="scripted FIFO bytes, not a measurement")
    p.add_argument("--json", action="store_true", help="full ordered write/delay/FIFO trace")
    p.add_argument("--waits", action="store_true", help="exercise vendor SPI waits with scripted status reads")
    args = p.parse_args()
    reply = bytes.fromhex(args.reply)
    r = Replay(args.rootfs, reply)
    codec = "wap/hw_ker_codec_le88226"
    assert r.run("hisi_sdk/hi_spi", "hi_kernel_spi_init") == 0
    assert r.run(codec, "hw_chip_codec_le88226_pre_init") == 0
    if args.waits:
        results = check_waits(r)
        if args.json:
            print(json.dumps(results, indent=2))
        else:
            for name, result in results.items():
                print(f"PASS: {name}: return={result['return']:#x}, "
                      f"status reads={result['status_reads']}, "
                      f"delays={result['delay_xloops']}, RUN clear")
        return
    ctx, dev = DATA + 0x100, DATA + 0x300
    r.uc.mem_write(ctx + 4, pack("<I", dev))
    r.uc.mem_write(dev, b"\0\0\0\x02")  # device index 0, two line slots
    begin = r.symbols[codec, "Vp880Init"]
    r.run(codec, "Vp880Init", ctx, end=begin + 0xb0)  # .text+0xd648, right after first ID
    observed = bytes(r.uc.mem_read(dev + 1, 2))
    events = r.events
    sends = [e[3] for e in events if e[1:3] == ("write", SPI + 0x60)]
    assert sends == [0x4a, 1] + [6] * 17 + [0x4a, 1, 0x73, 0xffff, 0xffff], sends
    assert observed == reply and not r.reply and r.received is None
    assert (r.regs[SPI][0] & 0xff, r.regs[SPI][0x10] & 0xffff,
            r.regs[SPI][0x14] & 0xffff, r.regs[SPI][8] & 1) == (0xc7, 1, 0x12, 0)
    reject = None
    if reply == b"\xff\xff":
        # Continue the exact factory function after its first ID read.
        first_after_id = len(events)
        r.uc.emu_start(begin + 0xb0, END, timeout=1_000_000, count=10_000)
        assert r.uc.reg_read(UC_ARM_REG_PC) == END
        reject = r.uc.reg_read(UC_ARM_REG_R0)
        assert reject and not any(e[1:3] == ("write", SPI + 0x60)
                                  for e in events[first_after_id:])
    if args.json:
        print(json.dumps({"scripted_reply": observed.hex(), "stock_reject": reject,
                          "events": events}, indent=2))
    else:
        print("PASS: stock init -> pre_init -> Vp880Init up to ID")
        print(f"SPI+60: {' '.join(f'{x:02x}' for x in sends)}")
        print(f"ID copied from scripted FIFO: {observed.hex(' ')}")
        if reject:
            print(f"PASS: stock Vp880Init returns {reject:#x} for ff ff before any later SPI write")


def check_waits(r):
    # Independent one-byte calls with an initialized controller. These
    # scenarios inject status words, not physical transitions or elapsed time.
    cases = (
        ("tx-ready-late", False, [0x04, 0x06, 0x06], 0x06, True, [0x73], []),
        ("tx-idle-late", False, [0x06, 0x07, 0x02, 0x06], 0x06, True, [0x73], []),
        ("tx-no-ready", False, [], 0x04, False, [], []),
        ("tx-no-idle", False, [0x06], 0x07, False, [0x73], []),
        ("rx-ready-late", True, [0x06, 0x06, 0x0e], 0x0e, True, [0xffff], [0xab04]),
        ("rx-no-ready", True, [], 0x06, False, [0xffff], []),
    )
    results = {}
    buf = DATA + 0x800
    for name, rx, samples, tail, success, sends, reads in cases:
        r.events.clear()
        r.status_script, r.status_tail = deque(samples), tail
        r.reply, r.received = [0xab04], None
        r.uc.mem_write(buf, pack("<I", 0x73))
        fn = "hi_kernel_spi_receive" if rx else "hi_kernel_spi_send"
        rc = r.run("hisi_sdk/hi_spi", fn, buf, 1, budget=12_000_000, timeout_us=25_000_000)
        events = r.events
        assert rc == (0 if success else 0xf0000104), (name, rc)
        assert not r.status_script and not r.regs[SPI][8] & 1, name
        assert [e[3] for e in events if e[1:3] == ("write", SPI + 0x60)] == sends
        assert [e[3] for e in events if e[1:3] == ("read", SPI + 0x60)] == reads
        if rx:
            assert unpack_from("<I", r.uc.mem_read(buf, 4))[0] == (reads[0] if success else 0x73)
        results[name] = {
            "return": rc,
            "status_reads": sum(e[1] == "status" for e in events),
            "delay_xloops": {hex(x): sum(e[1:] == ("delay-xloops", x) for e in events)
                             for x in sorted({e[2] for e in events if e[1] == "delay-xloops"})},
        }
    return results


if __name__ == "__main__":
    main()
