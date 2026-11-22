"""Compile actual driver transaction functions against a scripted MMIO device.

Run from a compiler environment: python tools/test_mdio_transactions.py --cc cl
or --cc cc. This is a host behavioral test, not kernel/hardware validation.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'openwrt-target/target/linux/sd5113/files-6.6/drivers/net/mdio/mdio-sd5113.c'

PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define BIT(n) (1u << (n))
#define GENMASK(h,l) ((~0u >> (31-(h))) & (~0u << (l)))
#define ETIMEDOUT 110
struct sd5113_mdio { unsigned char *base; };
struct mii_bus { void *priv; };
static unsigned char regs[0x104];
static u32 control, data_word, command;
static int busy_reads, completion_stuck, writes, delays, commands;
static u32 readl(void *p) {
    if (p == regs) {
        if (busy_reads) { --busy_reads; return control | BIT(20); }
        return control;
    }
    assert(p == regs + 0x100);
    return data_word;
}
static void writel(u32 v, void *p) {
    ++writes;
    if (p == regs) {
        command = v; ++commands;
        control = completion_stuck ? v : v & ~BIT(20);
    } else { assert(p == regs + 0x100); data_word = v; }
}
static void udelay(unsigned int us) { assert(us == 10); ++delays; }
static void reset(void) {
    control = BIT(30); data_word = 0xabcd7654u; command = 0;
    busy_reads = completion_stuck = writes = delays = commands = 0;
}
'''

TESTS = r'''
int main(void) {
    struct sd5113_mdio mdio = { regs };
    struct mii_bus bus = { &mdio };
    reset(); control |= BIT(16);
    assert(sd5113_mdio_read(&bus, 3, 7) == 0xabcd);
    assert(command == (BIT(30) | BIT(17) | BIT(20) | (3u << 8) | 7));
    assert(writes == 1 && commands == 1);
    reset(); control |= BIT(17);
    assert(sd5113_mdio_write(&bus, 31, 30, 0x1234) == 0);
    assert(data_word == 0xabcd1234u);
    assert(command == (BIT(30) | BIT(16) | BIT(20) | (31u << 8) | 30));
    assert(writes == 2 && commands == 1);
    reset(); busy_reads = 1001;
    assert(sd5113_mdio_read(&bus, 1, 1) == -ETIMEDOUT);
    assert(writes == 0 && delays == 1000);
    reset(); busy_reads = 1001;
    assert(sd5113_mdio_write(&bus, 1, 1, 0) == -ETIMEDOUT);
    assert(writes == 0 && data_word == 0xabcd7654u);
    reset(); busy_reads = 3;
    assert(sd5113_mdio_write(&bus, 1, 1, 0x1234) == 0);
    assert(delays == 3 && commands == 1);
    reset(); completion_stuck = 1;
    assert(sd5113_mdio_read(&bus, 1, 1) == -ETIMEDOUT);
    assert(commands == 1);
    assert(sd5113_mdio_write(&bus, 2, 2, 0) == -ETIMEDOUT);
    assert(commands == 1 && writes == 1 && data_word == 0xabcd7654u);
    control &= ~BIT(20); completion_stuck = 0;
    assert(sd5113_mdio_write(&bus, 2, 2, 0x5678) == 0);
    assert(commands == 2 && data_word == 0xabcd5678u);
    reset(); completion_stuck = 1;
    assert(sd5113_mdio_write(&bus, 2, 2, 0x5678) == -ETIMEDOUT);
    assert(commands == 1 && writes == 2);
    puts("PASS: read/write fields, data preservation, pre-busy timeout, delayed idle, completion timeout, retry and recovery");
    return 0;
}
'''

def run_host_test(cc, code):
    with tempfile.TemporaryDirectory(prefix='sd5113-host-test-') as directory:
        directory = Path(directory)
        c = directory / 'test.c'
        exe = directory / 'test.exe'
        c.write_text(code, encoding='utf-8')
        if Path(cc).stem.lower() == 'cl':
            command = [cc, '/nologo', '/W4', '/WX', str(c), '/Fe:' + str(exe)]
        else:
            command = [cc, '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)]
        subprocess.run(command, cwd=directory, check=True)
        subprocess.run([str(exe)], cwd=directory, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--source', type=Path, default=SOURCE)
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    defines = '\n'.join(re.findall(r'^#define SD5113_MDIO_(?:CTRL\w*|DATA\w*|POLL\w*)[^\n]*', source, re.M))
    functions = source[source.index('static int sd5113_mdio_wait_ready'):source.index('static int sd5113_mdio_probe')]
    run_host_test(args.cc, PRELUDE + defines + '\n' + functions + TESTS)


if __name__ == '__main__':
    main()
