"""Exercise the SD5113 forwarding table emitter against bounded mock MMIO."""

import argparse

from test_mdio_transactions import ROOT, run_host_test

SOURCE = (ROOT / "openwrt-target/target/linux/sd5113/files-6.6"
          / "drivers/net/ethernet/hisilicon/eth-sd5113.c")

PRELUDE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
struct sd5113_fe { unsigned char *classifier, *policy, *dest; };
static u32 classifier[0x380], policy[0x40], dest[0x40];

static void writel(u32 value, void *address)
{
    u32 *p = address;
    uintptr_t a = (uintptr_t)p;
    assert((a >= (uintptr_t)classifier &&
            a < (uintptr_t)(classifier + 0x380)) ||
           (a >= (uintptr_t)policy && a < (uintptr_t)(policy + 0x40)) ||
           (a >= (uintptr_t)dest && a < (uintptr_t)(dest + 0x40)));
    *p = value;
}

static void reset(void)
{
    memset(classifier, 0, sizeof(classifier));
    memset(policy, 0, sizeof(policy));
    memset(dest, 0, sizeof(dest));
}
"""

TESTS = r"""
static void run_group_tests(struct sd5113_fe *fe, unsigned int type,
                            unsigned int aux, unsigned int mask)
{
    static const unsigned int starts[] = {0x200, 0x240, 0x280, 0x2c0, 0x300};
    unsigned int i, prio;

    reset();
    sd5113_fe_program_routes(fe, type, aux);
    for (i = 0; i < 5; ++i) {
        unsigned int slot = 0x20 + i * 4;

        for (prio = 0; prio < 8; ++prio) {
            if (!(mask & (1U << i))) {
                assert(classifier[starts[i] + prio] == 0);
                continue;
            }
            assert(classifier[starts[i] + prio] ==
                   (0x2000U | (prio << 14) | (slot + prio / 2)));
            assert(policy[slot + prio / 2] ==
                   (0x04000000U | ((prio | 1U) << 9) | (8 + i)));
            assert(dest[slot + prio / 2] == 0x00012010);
        }
    }
    for (prio = 0; prio < 8; ++prio) {
        assert(classifier[0x340 + prio] ==
               (0x2000U | (prio << 14) | (0x34 + prio)));
        assert(policy[0x34 + prio] == (0x0400000dU | (prio << 9)));
        assert(dest[0x34 + prio] == 0x00001e10);
    }
}

int main(void)
{
    struct sd5113_fe fe = {
        (unsigned char *)classifier, (unsigned char *)policy,
        (unsigned char *)dest
    };

    run_group_tests(&fe, 1, 0, 0x01);
    run_group_tests(&fe, 4, 0, 0x1e);
    run_group_tests(&fe, 5, 0, 0x1e);
    run_group_tests(&fe, 8, 4, 0x1e);
    run_group_tests(&fe, 8, 3, 0x01);
    run_group_tests(&fe, 19, 0, 0x03);
    run_group_tests(&fe, 20, 0, 0x03);
    run_group_tests(&fe, 21, 0, 0x02);
    run_group_tests(&fe, 22, 0, 0x02);
    run_group_tests(&fe, 23, 0, 0x06);
    puts("PASS: SD5113 U-Boot LAN table branches and MMIO bounds");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = SOURCE.read_text(encoding="utf-8")
    helpers = source[source.index("static void sd5113_fe_route_group"):
                     source.index("\n}", source.index("static void sd5113_fe_program_routes")) + 2]
    run_host_test(args.cc, "\n".join([PRELUDE, helpers, TESTS]))


if __name__ == "__main__":
    main()
