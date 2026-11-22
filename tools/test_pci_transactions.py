"""Compile the actual PCI transaction functions against simulated MMIO."""

import argparse
from pathlib import Path

from test_mdio_transactions import ROOT, run_host_test

SOURCE = ROOT / (
    'openwrt-target/target/linux/sd5113/files-6.6/drivers/pci/controller/pci-sd5113.c'
)

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint8_t u8;
typedef u32 __le32;
typedef u32 dma_addr_t;
#define __iomem
#define BIT(n) (1U << (n))
#define PCI_SLOT(devfn) (((devfn) >> 3) & 31)
#define PCI_FUNC(devfn) ((devfn) & 7)
#define PCI_DEVFN(slot, func) (((slot) << 3) | (func))
#define PCI_CFG_SPACE_SIZE 256
#define PCI_COMMAND 4
#define PCI_STATUS_PARITY 0x0100
#define PCI_STATUS_SIG_TARGET_ABORT 0x0800
#define PCI_STATUS_REC_TARGET_ABORT 0x1000
#define PCI_STATUS_REC_MASTER_ABORT 0x2000
#define PCI_STATUS_SIG_SYSTEM_ERROR 0x4000
#define PCI_STATUS_DETECTED_PARITY 0x8000
#define PCIBIOS_SUCCESSFUL 0
#define PCIBIOS_DEVICE_NOT_FOUND 0x86
#define PCIBIOS_BAD_REGISTER_NUMBER 0x87
#define U32_MAX UINT32_MAX
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v, n) ((v) = (n))
#define cpu_to_le32(v) (v)
#define le32_to_cpu(v) (v)
#define lower_32_bits(v) ((u32)(v))
#define dma_wmb() ((void)0)
#define dma_rmb() ((void)0)
#define dev_err_ratelimited(...) ((void)0)
struct pci_bus { unsigned int number; void *sysdata; };
static unsigned char regs[0x84];
static u32 mailbox, read_data, written_data, rdaddr, wraddr;
static int reads, writes, delay, stuck;

static unsigned int offset(const void *p) {
    return (unsigned int)((const unsigned char *)p - regs);
}

static void writel(u32 value, void *p) {
    unsigned int off = offset(p);
    switch (off) {
    case 0: wraddr = value; break;
    case 0x20: rdaddr = value; break;
    case 0x04: case 0x24: assert(value == 0x90001000); break;
    case 0x08:
        assert(value == 0x4b8 || value == 0x4b9);
        if (value & 1) { ++writes; written_data = mailbox; delay = 2; }
        break;
    case 0x28:
        assert(value == 0x4a8 || value == 0x4a9);
        if (value & 1) { ++reads; delay = 2; }
        break;
    default: assert(0);
    }
}

static u32 readl(const void *p) {
    assert(offset(p) == 0x08 || offset(p) == 0x28);
    if (delay) {
        if (!stuck)
            --delay;
        return 1;
    }
    if (offset(p) == 0x28 && reads)
        mailbox = read_data;
    return 0;
}

static int poll(const void *p, u32 *val) {
    unsigned int i;
    for (i = 0; i < 10; i++) {
        *val = readl(p);
        if (!(*val & 1))
            return 0;
    }
    return -ETIMEDOUT;
}
#define readl_poll_timeout_atomic(p, v, cond, delay_us, timeout_us) poll(p, &(v))
'''

TESTS = r'''
int main(void) {
    struct sd5113_pci host = {0};
    struct pci_bus bus = {0};
    u32 val = 0;
    host.base = regs;
    host.scratch_cpu = &mailbox;
    host.scratch_dma = 0x90001000;
    bus.sysdata = &host;
    read_data = 0x12345678;
    assert(!sd5113_pci_config_read(&bus, 0x10, 2, 2, &val));
    assert(reads == 1 && !writes && val == 0x1234 && rdaddr == BIT(18));
    assert(sd5113_pci_config_read(&bus, 0x80, 0, 4, &val) ==
           PCIBIOS_DEVICE_NOT_FOUND && val == U32_MAX);
    assert(sd5113_pci_config_read(&bus, 0, 0x100, 4, &val) ==
           PCIBIOS_BAD_REGISTER_NUMBER);
    assert(sd5113_pci_config_read(&bus, 0, 3, 2, &val) ==
           PCIBIOS_BAD_REGISTER_NUMBER);
    assert(reads == 1);
    bus.number = 3;
    assert(!sd5113_pci_config_write(&bus, 0x24, 0xfc, 4, 0x1ee7));
    assert(wraddr == 0x324fd && reads == 1 && writes == 1 && written_data == 0x1ee7);
    read_data = 0xf912aace;
    assert(!sd5113_pci_config_write(&bus, 0x24, 4, 2, 0x123));
    assert(rdaddr == 0x32405 && wraddr == rdaddr && reads == 2 && writes == 2);
    assert(written_data == 0x00120123);
    host.onboard_wifi_only = true;
    assert(sd5113_pci_config_read(&bus, 0x10, 0, 4, &val) ==
           PCIBIOS_DEVICE_NOT_FOUND && val == U32_MAX);
    bus.number = 0;
    assert(sd5113_pci_config_read(&bus, 0x18, 0, 4, &val) ==
           PCIBIOS_DEVICE_NOT_FOUND && val == U32_MAX);
    assert(sd5113_pci_config_write(&bus, 0x11, 0, 4, 0) ==
           PCIBIOS_DEVICE_NOT_FOUND);
    stuck = 1;
    assert(sd5113_pci_config_read(&bus, 0x10, 0, 4, &val) ==
           PCIBIOS_DEVICE_NOT_FOUND && val == U32_MAX && host.cfg_failed);
    assert(sd5113_pci_config_write(&bus, 0x10, 4, 4, 0x123) ==
           PCIBIOS_DEVICE_NOT_FOUND);
    assert(reads == 3 && writes == 2);
    puts("PASS: PCI read/write banks, busy-clear, address bounds, RW1C, slot filter, timeout quarantine");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--source', type=Path, default=SOURCE)
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    definitions = source[source.index('#define SD5113_PCI_WR_CFGADDR'):
                         source.index('static void sd5113_pci_intx_enable')]
    functions = source[source.index('static int sd5113_pci_encode_cfg'):
                       source.index('static struct pci_ops sd5113_pci_ops')]
    run_host_test(args.cc, PRELUDE + definitions + functions + TESTS)


if __name__ == '__main__':
    main()
