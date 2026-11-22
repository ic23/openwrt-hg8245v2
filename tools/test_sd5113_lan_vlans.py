"""Replay the U-Boot broadcast chain and nonzero SD5113 table states."""
from test_eth_fe_routes import SOURCE
from test_mdio_transactions import run_host_test

source = SOURCE.read_text()
defs = source[source.index("#define CAP_MEMBER_MASK"):source.index("#define SD5113_FE_TX_CTRL1")]
state = source[source.index("struct sd5113_fe {"):source.index("static DEFINE_MUTEX")]
code = source[source.index("static void sd5113_lan_vlans_stop"):source.index("static void sd5113_fe_start")]
shim = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define __iomem
#define VLAN_N_VID 4096
#define SD5113_LAN_VID_BASE 101
#define SD5113_LAN_PORT_COUNT 4
#define BIT(n) (1u << (n))
#define GENMASK(h,l) ((~0u >> (31-(h))) & (~0u << (l)))
#define FIELD_PREP(m,v) (((v) << __builtin_ctz(m)) & (m))
#define FIELD_GET(m,v) (((v) & (m)) >> __builtin_ctz(m))
#define DECLARE_BITMAP(a,n) unsigned char a[n]
#define bitmap_zero(a,n) memset(a,0,n)
#define __set_bit(n,a) ((a)[n] = 1)
static unsigned find_first_zero_bit(unsigned char *a, unsigned n) {
	unsigned i = 0;
	while (i < n && a[i]) i++;
	return i;
}
static u32 *fail_once, *writes_forbidden;
static u32 readl(const u32 *p) { return *p; }
static void writel(u32 v, u32 *p) {
	assert(p != writes_forbidden);
	if (p == fail_once) { fail_once = NULL; return; }
	*p = v;
}
"""
cases = r"""
int main(void) {
	u32 bc[MCD_WORDS], cap[VLAN_N_VID], original[MCD_WORDS], caps[VLAN_N_VID];
	struct sd5113_fe fe = {.members = cap, .broadcast = bc};
	const unsigned pool = MCD_TAIL_BASE;
	memset(cap, 0x87, sizeof(cap));
	memcpy(caps, cap, sizeof(cap));
	/* Saturate the pool, then overlay the exact seven U-Boot words. */
	for (unsigned i = 0; i < MCD_WORDS; i++) bc[i] = 0x02002000;
	bc[1] = 0x03001000;
	for (unsigned i = 0; i < 5; i++) bc[pool+i] = 0x03001001 + (i+1)*0x2000 + i;
	bc[pool+5] = 0x0200c000;
	writes_forbidden = &bc[pool+5];
	memcpy(original, bc, sizeof(bc));
	assert(!sd5113_lan_vlans_start(&fe) && !fe.bc_tail_owned);
	for (int vid = 101; vid <= 104; vid++) {
		assert(bc[vid] == 0x03001005);
		assert((cap[vid] & 127) == 0x41);
	}
	sd5113_lan_vlans_stop(&fe);
	assert(!memcmp(original, bc, sizeof(bc)) && !memcmp(caps, cap, sizeof(cap)));
	/* Same pool with a shared GE terminal and no unused records. */
	bc[pool+5] = 0x82000abc;
	assert(!sd5113_lan_vlans_start(&fe) && bc[101] == 0x0300d005);
	sd5113_lan_vlans_stop(&fe);
	assert(bc[pool+5] == 0x82000abc);
	writes_forbidden = NULL;
	/* Leave two invalid records with stale payload. One has a live incoming link. */
	bc[pool+5] = 0x02012000;
	bc[12] = 0x03001000;
	bc[pool] = 0x01009999;
	bc[pool+1] = 0x81001234;
	bc[1] = 0; /* Release the bootloader chain. */
	memcpy(original, bc, sizeof(bc));
	assert(!sd5113_lan_vlans_start(&fe) && fe.bc_tail == pool+1 && fe.bc_tail_owned);
	assert(bc[pool+1] == 0x82000000);
	sd5113_lan_vlans_stop(&fe);
	assert(!memcmp(original, bc, sizeof(bc)));
	fail_once = &cap[103];
	assert(sd5113_lan_vlans_start(&fe) == -EIO && !fe.bc_tail);
	assert(!memcmp(original, bc, sizeof(bc)) && !memcmp(caps, cap, sizeof(cap)));
	bc[pool+1] |= MCD_VALID;
	assert(sd5113_lan_vlans_start(&fe) == -ENOSPC && !fe.bc_tail);
	puts("PASS SD5113 shared U-Boot terminal, dirty pool, restoration, true exhaustion");
	return 0;
}
"""
if __name__ == "__main__":
    run_host_test("cc", "\n".join([shim, defs, state, code, cases]))
