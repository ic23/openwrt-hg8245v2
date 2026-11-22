"""Run exact rootfs hi_i2c.ko ARM instructions with mocked MMIO; no board access."""
from pathlib import Path
from runpy import run_path
from struct import pack

from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1


loader = run_path(str(Path(__file__).with_name("hg8245-slicprobe") / "replay-stock-spi.py"))
CRG, GPIO, DATA = (loader[k] for k in ("CRG", "GPIO", "DATA"))
I2C = 0x16151000
GATE = 1 << 27
ROUTE = 1 << 22
STOCK = "hisi_sdk/hi_i2c"
DIGEST = "c6087419fa48c8080cb6db94b81f6ce2e0dc58459028135605176bb790edf67d"


class Replay(loader["Replay"]):
    def __init__(self):
        super().__init__(loader["ROOTFS"], b"", {STOCK: DIGEST})
        self.regs[I2C] = dict.fromkeys(range(0, 0x100, 4), 0)
        self.regs[CRG][0x1c] |= ROUTE
        self.regs[CRG][0x2c] |= GATE
        self.regs[GPIO][8] = 0x8f
        self.regs[I2C].update({0x14: 0x12340000, 0x18: 0x56780000, 0x30: 0x55aa0123})
        self.uc.mmio_map(I2C, 0x1000, self.mmio_read, I2C, self.mmio_write, I2C)
        self.maps = []
        self.reply = None

    def stub(self, uc, address, size, user_data):
        if self.stub_names.get(address) == "__ioremap":
            self.maps.append((uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)))
        super().stub(uc, address, size, user_data)

    def mmio_read(self, uc, offset, size, base):
        if base == I2C and offset == 0x10:
            self.events.append((self.phase, "read", base + offset, self.reply))
            return self.reply or 0
        return super().mmio_read(uc, offset, size, base)

    def mmio_write(self, uc, offset, size, value, base):
        if base == I2C and offset == 0x10:
            assert self.regs[CRG][0x2c] & GATE and self.regs[CRG][0x1c] & ROUTE
            assert self.regs[I2C][0x6c] & 1
            if value & 0x100:
                self.regs[I2C][0x70] |= 8
                self.reply = 0x48  # inject a byte; does not represent hardware
        super().mmio_write(uc, offset, size, value, base)

    def writes(self):
        return [event[2:] for event in self.events if event[1] == "write"]


def main():
    r = Replay()
    assert r.run(STOCK, "hi_kernel_i2c_init") == 0
    assert r.maps == [(I2C, 0x100), (CRG + 0x1c, 4), (CRG + 0x24, 4),
                      (CRG + 0x28, 4), (GPIO + 8, 4)], r.maps
    assert r.writes() == [(CRG + 0x28, GATE)] and not r.regs[CRG][0x2c] & GATE

    route = r.regs[CRG][0x1c]
    r.events.clear()
    r.uc.mem_write(DATA, pack("<IIII", 1, 0, 0, 100))
    assert r.run(STOCK, "hi_kernel_i2c_attr_set", DATA) == 0
    assert r.writes() == [
        (CRG + 0x1c, route & ~ROUTE), (I2C + 0x6c, 0),
        (GPIO + 8, 0xaf), (GPIO + 8, 0xef), (CRG + 0x24, GATE),
        (I2C + 0x30, 0x55aa0fff), (I2C + 0x18, 0x56780000 | 665),
        (I2C + 0x14, 0x12340000 | 665), (I2C, 3), (I2C + 0x6c, 1),
        (CRG + 0x1c, route | ROUTE),
    ], r.writes()
    assert r.regs[CRG][0x2c] & GATE

    r.events.clear()
    r.regs[I2C][0x70] = 6  # idle, TX can accept a byte
    assert r.run(STOCK, "hi_kernel_i2c_read", 0x50, 0x28, DATA + 0x100) == 0
    assert r.writes() == [(I2C + 0x6c, 0), (I2C + 4, 0x50), (I2C + 0x6c, 1),
                          (I2C + 0x10, 0x28), (I2C + 0x10, 0x100)], r.writes()
    assert r.uc.mem_read(DATA + 0x100, 1) == b"H"

    r.events.clear()
    r.regs[I2C][0x70] = 4  # idle, never ready to accept TX
    assert r.run(STOCK, "hi_kernel_i2c_read", 0x50, 0x28, DATA + 0x100) == 0xf0000104
    assert not any(a == I2C + 0x10 for a, _ in r.writes())
    print("PASS: stock ARM I2C map, gate, FUNC/CRG, 100 kHz divisor, offset/READ, TX timeout")


if __name__ == "__main__":
    main()
