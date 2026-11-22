"""Check the actual GEM admission and packet-selection functions against RAM registers."""
from test_mdio_transactions import ROOT, run_host_test

TARGET = ROOT / "openwrt-target"
gpon = next(TARGET.rglob("sd5113_gpon_rx.c")).read_text()
mac = next(TARGET.rglob("eth-sd5113.c")).read_text()
header = next(TARGET.rglob("sd5113-gem.h")).read_text()


def block(source, begin, end):
    pos = source.index(begin)
    return source[pos:source.index(end, pos)]


PRELUDE = r"""
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
#define BIT(n) (1U << (n))
#define U32_MAX UINT32_MAX
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define __iomem
#define test_bit(gem, bits) ((bits)[(gem)/32] & BIT((gem)%32))
#define VLAN_VID_MASK 4095
#define VLAN_ETH_HLEN 18
#define ETH_P_8021Q 0x8100
#define ETH_P_8021AD 0x88a8
#define ETH_P_IP 0x0800
#define ETH_P_IPV6 0x86dd
struct sk_buff { u8 bytes[18]; bool tagged; u8 pcp; };
#define skb_header_pointer(s,off,n,tmp) ((void)(tmp), (s)->bytes + (off))
#define skb_vlan_tag_present(s) ((s)->tagged)
#define skb_vlan_tag_get_prio(s) ((s)->pcp)
static u16 get_unaligned_be16(const u8 *p) { return p[0] * 256u + p[1]; }
static u32 readl(const void *p) { return *(const u32 *)p; }
static void writel(u32 v, void *p) { *(u32 *)p = v; }
"""

SHIM = r"""
struct sd5113_gpon_rx {
	struct sd5113_plan plan;
	struct sd5113_admitted_gem data[8];
	unsigned int admitted, owned_tcont;
	int omcc_port;
	void *gem_table, *tcont;
	u32 encrypted[128];
};
struct sd5113_eth { struct sd5113_gem_profile qos; u32 gem_route; };
static struct sd5113_eth eth;
static bool fail;
static int sd5113_eth_set_gem(const struct sd5113_gem_profile *p) {
	if (fail) return -EIO;
	memset(&eth, 0, sizeof(eth));
	if (p) {
		eth.qos = *p;
		for (int i=0; i<8 && !eth.gem_route; i++) eth.gem_route=p->route[i];
	}
	return 0;
}
"""

TEST = r"""
int main(void) {
	u32 table[4096] = {0}, grants[8] = {0};
	struct sd5113_gpon_rx rx = {.omcc_port=12, .owned_tcont=6,
		.gem_table=table, .tcont=grants};
	struct sd5113_plan plan = {.unmarked=8};
	struct sk_buff skb = {0};
	grants[1]=BIT(31)|(400<<4)|1;
	grants[2]=BIT(31)|(401<<4)|2;
	for (unsigned i=0; i<8; i++)
		plan.paths[i]=i<4 ? (100<<12)|400 : (101<<12)|401;
	plan.paths[3]=UINT32_MAX;
	for (unsigned i=0; i<64; i++) {
		unsigned prio=(i+1)%8;
		for (unsigned j=0; j<3; j++) {
			unsigned bit=3*i+j;
			if (prio & (4>>j)) plan.dscp[bit/8] |= 0x80>>(bit%8);
		}
	}
	/* Combined policies on the VEIP and ANI bridge ports, on the wire
	 * side of the symmetric tag treatment in either direction. */
	plan.filters[1][0]=16; plan.filters[1][1]=2;
	plan.filters[1][2]=1; plan.filters[1][3]=46;
	plan.filters[1][4]=1; plan.filters[1][5]=244;
	u8 frame[18]={0};
	assert(!sd5113_bridge_allows(plan.filters, frame, sizeof(frame)));
	frame[12]=0x81;frame[14]=0xa1;frame[15]=46;
	assert(sd5113_bridge_allows(plan.filters,frame,18));
	assert(!sd5113_bridge_allows(plan.filters,frame,14));
	frame[15]=244;assert(sd5113_bridge_allows(plan.filters,frame,18));
	frame[15]=45;assert(!sd5113_bridge_allows(plan.filters,frame,18));
	frame[15]=46;
	plan.filters[0][0]=18;plan.filters[0][1]=1;plan.filters[0][2]=0x20;
	assert(!sd5113_bridge_allows(plan.filters,frame,18));
	plan.filters[0][2]=0xa0;
	assert(sd5113_bridge_allows(plan.filters,frame,18));
	plan.filters[0][0]=20;plan.filters[0][2]=0xa1;plan.filters[0][3]=46;
	assert(sd5113_bridge_allows(plan.filters,frame,18));
	frame[14] |= 0x10;
	assert(!sd5113_bridge_allows(plan.filters,frame,18));
	table[100]=0x4000;
	table[12]=0x11;
	assert(!sd5113_apply_plan(&rx,&plan) && rx.admitted==2);
	assert(!memcmp(eth.qos.filters,plan.filters,sizeof(plan.filters)));
	assert(table[100]==0x4010 && table[101]==0x10 && table[12]==0x11);
	assert(eth.qos.route[3]==0 && (eth.qos.route[4]&0x7fff)==((2<<12)|101));
	for (unsigned d=0; d<64; d++) {
		skb.bytes[12]=8; skb.bytes[13]=0; skb.bytes[15]=(d<<2)|3;
		assert(sd5113_tx_pbit(&skb,&eth)==(d+1)%8);
		skb.bytes[12]=0x86; skb.bytes[13]=0xdd;
		skb.bytes[14]=0x60|(d>>2); skb.bytes[15]=(d&3)<<6;
		assert(sd5113_tx_pbit(&skb,&eth)==(d+1)%8);
	}
	skb.bytes[12]=0x81; skb.bytes[13]=0; skb.bytes[14]=5<<5;
	assert(sd5113_tx_pbit(&skb,&eth)==5);
	skb.tagged=true; skb.pcp=2;
	assert(sd5113_tx_pbit(&skb,&eth)==2);
	plan.vid=302; plan.tag_pcp=6;
	assert(!sd5113_apply_plan(&rx,&plan));
	assert(sd5113_tx_pbit(&skb,&eth)==6);
	plan.paths[4]=(102<<12)|402;
	assert(sd5113_apply_plan(&rx,&plan)==-EAGAIN && !table[102]);
	plan.paths[4]=(102<<12)|401; fail=true;
	assert(sd5113_apply_plan(&rx,&plan)==-EIO && !table[102] && rx.admitted==2);
	fail=false;
	table[555]=0x10;
	assert(sd5113_apply_plan(&rx,&plan)==-EBUSY && table[555]==0x10);
	sd5113_clear_data(&rx);
	assert(!rx.admitted && !eth.gem_route && table[100]==0x4000 && !table[101]);
	puts("PASS GEM admission, rollback, DSCP 0..63, VLAN PCP and dropped priority");
	return 0;
}
"""

if __name__ == "__main__":
    pieces = [PRELUDE, block(header, "struct sd5113_gem_profile", "int sd5113_eth"),
              block(gpon, "struct sd5113_plan", "struct sd5113_gpon_rx {"), SHIM,
              block(gpon, "static int sd5113_data_index", "static int sd5113_apply_plan"),
              block(gpon[gpon.index("static DEVICE_ATTR_RO(omci_session)"):],
                    "static int sd5113_apply_plan", "static ssize_t gem_service_show"),
              block(mac, "static bool sd5113_bridge_allows", "static netdev_tx_t sd5113_xmit"), TEST]
    run_host_test("cc", "\n".join(pieces))
