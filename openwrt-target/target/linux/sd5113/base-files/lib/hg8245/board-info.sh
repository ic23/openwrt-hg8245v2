# Read one address from the stock BoardInfo XML in the first page of syspara.
hg8245_board_mac() {
	local field="$1" part mac attr
	case "$field" in LanMac|WlanMac) attr=infoStr;; WanMac) attr=mac_1;; *) return 1;; esac
	part=$(find_mtd_part syspara)
	[ -n "$part" ] || return 1
	mac=$(dd if="$part" bs=4096 count=1 2>/dev/null | tr '\000' '\n' |
		sed -n "s/^[[:space:]]*<$field $attr=\"\([0-9A-Fa-f:]*\)\".*/\1/p" | head -n 1)
	[ "$mac" != 00:00:00:00:00:00 ] &&
		printf '%s\n' "$mac" | grep -Ei '^[[:xdigit:]][02468ace](:[[:xdigit:]]{2}){5}$'
}

hg8245_managed_switch() {
	local node=/proc/device-tree/soc/mdio@43b0a000/switch@0
	[ -r "$node/compatible" ] && [ "$(tr -d '\000' < "$node/compatible")" = qca,ar8316 ]
}

# AR8316 port numbers in front-panel order, LAN1 through LAN4.
HG8245_LAN_SWITCH_PORTS='2 3 4 1'
