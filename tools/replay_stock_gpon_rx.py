"""Replay HG8245V2 stock LOS, LOF and HPSF212 RX acquisition on a model."""
from pathlib import Path
from runpy import run_path
from struct import pack, unpack, unpack_from
from xml.etree import ElementTree

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_FP, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
    UC_ARM_REG_LR, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R7, UC_ARM_REG_R9, UC_ARM_REG_SP,
)

loader = run_path(str(Path(__file__).with_name("hg8245-slicprobe") / "replay-stock-spi.py"))
SOC, DP = "wap/hw_soc_sd5113", "hisi_sdk/hi_dp"
OPTIC = "wap/hw_module_optic"
MODULES = {SOC: loader["STOCK"][SOC],
           DP: "23a015ef4fd4f04f7cbcda3ef9348372bb9c7f82b7f3c8bad372d496876a11eb",
           OPTIC: "78d41b416afc984f0e0fdbca19f1acf601f72c7d535502751419eaea04dd70ba"}
GLB, DGTC, DGEM, PRBS = 0x42000000, 0x43200000, 0x43300000, 0x43900000


class Replay(loader["Replay"]):
    def __init__(self):
        super().__init__(loader["ROOTFS"], b"", MODULES)
        self.maps = []
        self.reads = []
        self.i2c = []
        self.trace = {}
        for base, offsets in ((GLB, (0xa0,)), (DGTC, (8, 12, 0x2c)),
                              (DGEM, (0x14, 0x70)), (PRBS, (0x2c,))):
            self.regs[base] = dict.fromkeys(offsets, 0)
            self.uc.mmio_map(base, 0x1000, self.mmio_read, base, self.mmio_write, base)

    def stub(self, uc, address, size, data):
        name = self.stub_names.get(address)
        if name == "__ioremap":
            self.maps.append((uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)))
            return  # Identity mapping in the simulated address space.
        if name == "hw_drv_i2c_read":
            offset = uc.reg_read(UC_ARM_REG_R1)
            self.i2c.append((uc.reg_read(UC_ARM_REG_R0), offset))
            uc.mem_write(uc.reg_read(UC_ARM_REG_R2), bytes([offset]))
            uc.reg_write(UC_ARM_REG_R0, 0)
        elif name in ("HW_LDSP_Kernel_MemCpy", "memcpy"):
            uc.mem_write(uc.reg_read(UC_ARM_REG_R0), bytes(uc.mem_read(
                uc.reg_read(UC_ARM_REG_R1), uc.reg_read(UC_ARM_REG_R2))))
        elif name == "proc_dbg_trace_ktrace":
            label, value = unpack("<II", uc.mem_read(uc.reg_read(UC_ARM_REG_SP), 8))
            fmt = bytes(uc.mem_read(label, 96)).split(b"\0", 1)[0]
            self.trace[fmt.split(b":", 1)[0].strip()] = value
        elif name not in ("__iounmap", "HW_LDSP_Kernel_MSleep"):
            super().stub(uc, address, size, data)

    def mmio_read(self, uc, offset, size, base):
        self.reads.append(base + offset)
        return super().mmio_read(uc, offset, size, base)

    def mmio_write(self, uc, offset, size, value, base):
        raise AssertionError(f"unexpected MMIO write at {base + offset:#x}")


def optic_profile(part="HPSF212"):
    fs = loader["FS"](loader["ROOTFS"])
    _, entries = fs.walk()
    inode, = [num for name, num, _, _ in entries if name == "etc/wap/HG8245V2/hw_ldsp_cfg.xml"]
    xml = fs.read_file(fs.inodes[inode])
    # Other sections contain mismatched end tags. Parse the actual OPTIC subtree.
    optic = ElementTree.fromstring(b"<OPTIC>" + xml.split(b"<OPTIC>")[1].split(b"</OPTIC>")[0] +
                                   b"</OPTIC>", parser=ElementTree.XMLParser(encoding="iso-8859-1"))
    cfg, = [item for item in optic if item.tag == part or item.findtext("para2") == part]
    data = bytearray()
    for field in list(cfg)[1:]:
        size = int(field.attrib["length"])
        value = (field.text or "").strip()
        if field.attrib["type"] == "string":
            data.extend(value.encode("ascii").ljust(size, b"\0"))
        else:
            data.extend(int(value, 0).to_bytes(size, "little"))
    assert len(data) == 84
    return bytes(data)


def check_optic_boot():
    class OpticReplay(Replay):
        def stub(self, uc, address, size, data):
            name = self.stub_names.get(address)
            a, b, c = [uc.reg_read(reg) for reg in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)]
            if name == "hw_drv_i2c_read":
                self.i2c.append(("R", a, b))
                uc.mem_write(c, bytes(self.eeprom[b:b + 1]))
            elif name == "hw_drv_i2c_write":
                self.i2c.append(("W", a, b, c))
            elif name == "strlen":
                uc.reg_write(UC_ARM_REG_R0, bytes(uc.mem_read(a, 128)).index(0))
                return
            elif name == "HW_LDSP_Kernel_Strncmp":
                x, y = [bytes(uc.mem_read(v, c)).split(b"\0")[0] for v in (a, b)]
                uc.reg_write(UC_ARM_REG_R0, (x > y) - (x < y))
                return
            else:
                return super().stub(uc, address, size, data)
            uc.reg_write(UC_ARM_REG_R0, 0)

    r = OpticReplay()
    # These two board callbacks program SoC registers. Isolate the optic
    # module here and record every I2C import it executes.
    callbacks = []
    def soc_call(uc, address, size, name):
        callbacks.append(name)
        uc.reg_write(UC_ARM_REG_R0, 0)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    for name in ("hw_chip_optic_init", "hw_chip_optic_los_level_set"):
        addr = r.symbols[SOC, name]
        r.uc.hook_add(UC_HOOK_CODE, soc_call, name, addr, addr)
    r.eeprom = bytearray(256)
    cfg = loader["DATA"] + 0x100
    for number in range(2, 19):
        if number == 4:  # No identifying part in this XML entry.
            continue
        profile = optic_profile(f"Cfg{number}")
        vendor, pn = (profile[:32].rstrip(b"\0"), profile[32:48].rstrip(b"\0"))
        date = pn == b"HPSF212"
        if date:
            pn = b"HPSF2120"
        r.i2c.clear()
        callbacks.clear()
        r.eeprom[96:128] = vendor.ljust(32, b" ")
        r.eeprom[40:56] = pn.ljust(16, b" ")
        r.eeprom[84:90] = b"160401"
        r.uc.mem_write(cfg, profile)
        assert r.run(OPTIC, "hw_drv_optic_init", cfg) == 0
        offsets = [*range(0x60, 0x80), *range(0x28, 0x38)]
        if date:
            offsets.extend(range(0x54, 0x5a))
        offsets.append(0x5c)
        assert r.i2c == [("R", 0x50, off) for off in offsets], r.i2c
        assert callbacks == ["hw_chip_optic_init", "hw_chip_optic_los_level_set"]
        print(f"PASS: {(vendor or pn).decode()} init, {len(offsets)} EEPROM reads, zero I2C data writes")
    r.i2c.clear()
    r.uc.mem_write(cfg, pack("<4H", 0x0123, 0x4567, 0x89ab, 0xcdef))
    assert r.run(OPTIC, "hw_drv_optic_threshold_set", cfg, 0x10) == 0
    assert r.i2c == [("W", 0x51, 0x10 + i, v) for i, v in enumerate(bytes.fromhex("0123456789abcdef"))]
    print("PASS: separate threshold setter sends four big-endian words as eight byte writes")


def check_ploam_ready(r):
    irq = r.symbols[DP, "hi_kernel_dp_gpon_dn_irq_process"]
    # Replay the stock queue loop from loading DGTC to the empty branch or ACK store.
    frame = loader["STACK"] + 0xfe00
    words = (0xff01a203, 0x04b506c7, 0x08d90aeb)
    for flags in (0, 2, 1, 3):
        r.regs[DGTC].update(zip((0x38, 0x3c, 0x40), words))
        r.regs[DGTC][0x34] = flags
        r.uc.reg_write(UC_ARM_REG_R7, r.symbols[DP, "g_dp_DGTC_reg_s"])
        r.uc.reg_write(UC_ARM_REG_R9, loader["DATA"] + 0x200)
        r.uc.reg_write(UC_ARM_REG_FP, frame)
        r.reads.clear()
        ready = flags & 1
        stop = irq + ((0x4cf8 if ready else 0x4d3c) - 0x4b98)
        r.uc.emu_start(irq + (0x4c3c - 0x4b98), stop, count=96)
        assert r.uc.reg_read(UC_ARM_REG_PC) == stop
        expected = [DGTC + 0x34]
        if ready:
            expected += [DGTC + off for off in (0x38, 0x3c, 0x40) for _ in range(4)]
            expected.append(DGTC + 0x34)
            assert r.uc.mem_read(frame - 292, 12) == b"".join(w.to_bytes(4, "big") for w in words)
            assert r.uc.reg_read(UC_ARM_REG_R3) == flags & ~1
        assert r.reads == expected
    print("PASS: stock IRQ checks bit 0, assembles 12 bytes and prepares ACK only when ready")


def check_activation(r):
    # Capture the attribute structs produced by stock handlers, then run their
    # register-write blocks separately from the MAC-wide configuration checks.
    ugtc_ptr = r.symbols[DP, "g_dp_UGTC_reg_s"]
    ugtc, = unpack("<I", r.uc.mem_read(ugtc_ptr, 4))
    assert ugtc == 0x43400000
    r.uc.mem_map(ugtc, 4096)
    attrs = []
    scratch = loader["DATA"] + 0x500
    msg = loader["DATA"] + 0x100

    def attr_call(uc, address, size, mode):
        ptr = uc.reg_read(UC_ARM_REG_R0)
        if mode == "get":
            uc.mem_write(ptr, bytes(156))
        else:
            attrs.append(bytes(uc.mem_read(ptr, 156)))
        uc.reg_write(UC_ARM_REG_R0, 0)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    for mode in ("get", "set"):
        addr = r.symbols[DP, "hi_kernel_dp_gpon_up_attr_" + mode]
        r.uc.hook_add(UC_HOOK_CODE, attr_call, mode, addr, addr)

    def apply_block(start, end):
        origin = r.symbols[DP, "hi_kernel_dp_gpon_up_attr_set"] - 0x2e604
        r.uc.mem_write(scratch, attrs.pop())
        r.uc.reg_write(UC_ARM_REG_R4, scratch)
        r.uc.reg_write(UC_ARM_REG_R7, ugtc_ptr)
        r.uc.emu_start(origin + start, origin + end, count=70)
        assert r.uc.reg_read(UC_ARM_REG_PC) == origin + end

    r.uc.mem_write(ugtc, pack("<I", 0xabcdefff))
    assert r.run(SOC, "hw_chip_ploam_onuid_set", 27) == 0
    assert unpack_from("<I", attrs[-1], 20)[0] == 27
    apply_block(0x2ed94, 0x2edb8)
    assert r.uc.mem_read(ugtc, 4) == pack("<I", 0xabcdef1b)
    for bits in (0x01234567, 0x01234500, 0):
        r.uc.mem_write(msg, bytes([27, 4, 0]) + bits.to_bytes(4, "big") + bytes(5))
        assert r.run(SOC, "PLOAM_ReceiveRangingTime", msg) == 0
        assert len(attrs) == 1 and unpack_from("<III", attrs[0], 48) == (0, bits >> 3, bits & 7)
        r.uc.mem_write(ugtc + 0x28, pack("<II", 0xff, ~(bits >> 3) & 0xffffffff))
        r.uc.mem_write(ugtc + 0x3c, pack("<I", 0x30))
        apply_block(0x2ef44, 0x2efc0)
        assert unpack("<III", r.uc.mem_read(ugtc + 0x28, 12)) == (0, bits >> 3, bits & 7)
        assert r.uc.mem_read(ugtc + 0x3c, 4) == pack("<I", 0x31)
    print("PASS: stock ONU-ID masked write and main-path ranging/commit for three delays")


def check_management_registers():
    r = Replay()
    assert r.run(DP, "hi_kernel_dp_reg_init") == 0
    assert r.run(DP, "hi_kernel_dp_table_init") == 0
    addr = loader["DATA"] + 0x400
    def reg(name):
        return unpack("<I", r.uc.mem_read(r.symbols[DP, name], 4))[0]
    upstream, gem = reg("g_dp_UGTC_reg_s"), reg("g_dp_DGEM_dnport_table_s")
    assert (upstream, gem, reg("g_dp_OMCI_reg_s")) == (0x43400000, 0x43380000, 0x42900000)
    r.uc.mem_map(upstream, 4096)
    r.uc.mem_map(gem, 16384)
    for port in (0, 27, 511, 4095):
        for valid in (1, 0):
            r.uc.mem_write(addr, pack("<5I", port, 0, valid, 0, 1))
            r.uc.mem_write(gem + port * 4, pack("<I", 0xa55aa5ab))
            assert r.run(DP, "hi_kernel_dp_gpon_dn_gemport_table_item_set", addr) == 0
            assert unpack("<I", r.uc.mem_read(gem + port * 4, 4))[0] == 0xa55aa501 | (valid << 4)
    for copies, frame in ((0, bytes.fromhex("ff01 41424344 12345678 0984")),
                          (1, bytes.fromhex("1b09 0e1b0e01 10200000 0000")),
                          (3, bytes.fromhex("1b02 11223344 55667788 99aa"))):
        r.uc.mem_write(upstream, bytes(32))
        r.uc.mem_write(addr, pack("<4I", copies, 1, 0, 0) + frame)
        assert r.run(DP, "hi_kernel_dp_gpon_up_ploam_send", addr) == 0
        registers = unpack("<7I", r.uc.mem_read(upstream + 4, 28))
        assert registers == (1, *unpack(">3I", frame), copies or 3, int(copies == 0), 1)
    print("PASS: stock grant-driven serial template, single ACK and triple password mailbox formats")
    print("PASS: stock 4096-entry management GEM table layout, activation and revocation")


def main():
    check_optic_boot()
    check_management_registers()
    r = Replay()
    assert r.run(DP, "hi_kernel_dp_reg_init") == 0
    for symbol, expected in (("g_dp_DGTC_reg_s", DGTC), ("g_dp_DGEM_reg_s", DGEM),
                             ("g_dp_PRBS_reg_s", PRBS)):
        assert unpack("<I", r.uc.mem_read(r.symbols[DP, symbol], 4))[0] == expected
    out = loader["DATA"] + 0x100
    for word in (0, 1, 2, 3, 0xfffffffc, 0xffffffff):
        r.regs[GLB][0xa0] = r.regs[DGTC][0x0c] = word
        r.regs[DGTC][0x08] = word ^ 2  # Distinguish adjacent status fields.
        r.reads.clear()
        assert r.run(SOC, "hw_chip_ploam_los_status_get", out) == 0
        assert r.maps[-1] == (GLB + 0xa0, 4) and r.reads == [GLB + 0xa0]
        assert unpack("<I", r.uc.mem_read(out, 4))[0] == word & 1
        r.reads.clear()
        assert r.run(SOC, "hw_chip_ploam_lof_status_get", out) == 0
        assert r.reads == [DGEM + 0x14, DGTC + 0x2c, DGEM + 0x70,
                           PRBS + 0x2c, DGTC + 0x08, DGTC + 0x0c]
        assert unpack("<I", r.uc.mem_read(out, 4))[0] == (word >> 1) & 1
    print("PASS: stock ARM register map and six LOS/LOF cases, no MMIO writes")
    check_ploam_ready(r)
    counter_blocks = {GLB, DGTC, DGEM, PRBS}
    for symbol in ("g_dp_OMCI_reg_s", "g_dp_QM_stat0_table_s"):
        region, = unpack("<I", r.uc.mem_read(r.symbols[DP, symbol], 4))
        base = region & ~0xfff
        counter_blocks.add(base)
        if base not in r.regs:
            r.regs[base] = {}
            r.uc.mmio_map(base, 0x1000, r.mmio_read, base, r.mmio_write, base)
    for base in counter_blocks:
        r.regs[base].update({off: base + off for off in range(0, 4096, 4)})
    # Tag each word with its address and inspect the fields stock actually prints.
    assert r.run(SOC, "hw_chip_gmac_dn_cnt_get", out) == 0
    assert r.trace[b"Dn Ploam Recv Frames"] == DGTC + 0x44
    assert r.trace[b"Dn Ploam CRC Err Frames"] == DGTC + 0x80
    assert r.trace[b"Dn Ploam Drop Frames"] == DGTC + 0x84
    print("PASS: stock trace identifies receive, CRC error and drop counters, no MMIO writes")
    r.uc.mem_write(r.symbols[OPTIC, "g_st_optic_attr"], optic_profile())
    assert r.run(OPTIC, "hw_drv_optic_rxpower_get", out) == 0
    offsets = [0x68, 0x69, *range(0x38, 0x4c)]
    assert r.i2c == [(0x51, offset) for offset in offsets]
    assert r.uc.mem_read(out, 22) == bytes(offsets)
    print("PASS: stock board profile and ARM getter read the RX sample and five coefficients")
    check_activation(r)


if __name__ == "__main__":
    main()
