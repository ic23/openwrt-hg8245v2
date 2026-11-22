RAMFS_COPY_BIN="${RAMFS_COPY_BIN} sha256sum tr"

hg8245_install_part() {
	local entry
	for entry in /sys/class/mtd/mtd[0-9]*; do
		[ -r "$entry/name" ] || continue
		[ "$(cat "$entry/name")" = "$1" ] || continue
		[ "$(cat "$entry/offset")" = "$2" ] &&
			[ "$(cat "$entry/size")" = "$3" ] || return 1
		echo "/dev/${entry##*/}"
		return 0
	done
	return 1
}

hg8245_digest_matches() {
	local expected actual
	expected=$(tr -d '\000' < "/sys/firmware/devicetree/base/chosen/$2") || return 1
	[ "${#expected}" = 64 ] || return 1
	actual=$(sha256sum "$1") || return 1
	[ "${actual%% *}" = "$expected" ]
}

hg8245_check_install_image() {
	local boot layout system flags
	type notify_firmware_no_backup >/dev/null 2>&1 && notify_firmware_no_backup
	[ -r /sys/firmware/devicetree/base/chosen/openwrt,system-sha256 ] || {
		echo "Load the release-matched HG8245V2 RAM installer first." >&2
		return 1
	}
	boot=$(hg8245_install_part boot 0 262144) &&
	layout=$(hg8245_install_part flashcfg 262144 4096) &&
	system=$(hg8245_install_part openwrt-system 393216 11010048) &&
	[ -c "$system" ] &&
	flags=$(cat "/sys/class/mtd/${system##*/}/flags") &&
	[ $((flags & 0x400)) -ne 0 ] &&
	[ -f "$1" ] && [ "$(wc -c < "$1")" -eq 11010048 ] &&
	hg8245_digest_matches "${boot}ro" openwrt,boot-sha256 &&
	hg8245_digest_matches "${layout}ro" openwrt,layout-sha256 &&
	hg8245_digest_matches "$1" openwrt,system-sha256 || {
		echo "Board layout, bootloader or system image did not match this installer." >&2
		return 1
	}
}

hg8245_do_install() {
	local dev
	[ -z "$UPGRADE_BACKUP" ] || { echo "Use sysupgrade -n for installation." >&2; exit 1; }
	hg8245_check_install_image "$1" || exit 1
	dev=$(hg8245_install_part openwrt-system 393216 11010048) || exit 1
	mtd write "$1" "$dev" || exit 1
	sync
	hg8245_digest_matches "${dev}ro" openwrt,system-sha256 || {
		echo "NOR readback failed. Boot the RAM installer again via UART/TFTP." >&2
		exit 1
	}
}

hg8245_member() {
	tar -xOf "$1" "sysupgrade-hg8245v2/$2"
}

hg8245_writable_part() {
	local dev dir flags
	dev=$(hg8245_install_part "$@") || return 1
	dir="/sys/class/mtd/${dev##*/}"
	flags=$(cat "$dir/flags") || return 1
	[ -c "$dev" ] && [ "$(cat "$dir/erasesize")" = 131072 ] &&
		[ $((flags & 0x400)) -ne 0 ] || return 1
	echo "$dev"
}

hg8245_check_upgrade() {
	local manifest part size limit align expected actual layout
	layout=$(tr -d '\000' < /sys/firmware/devicetree/base/chosen/openwrt,upgrade-layout) || return 1
	[ "$layout" = b176-nor-v1 ] && [ "$(cat /tmp/sysinfo/board_name)" = huawei,hg8245 ] || return 1
	entrydev=$(hg8245_writable_part krnl 393216 1572864) &&
	kerneldev=$(hg8245_writable_part kernel 1966080 3014656) &&
	rootdev=$(hg8245_writable_part rootfs 4980736 6422528) || return 1

	fwtool -q -i /dev/null "$1" || return 1
	manifest=$(hg8245_member "$1" CONTROL) || return 1
	[ "$(echo "$manifest" | sed -n '1p')" = 'BOARD=huawei,hg8245' ] &&
		[ "$(echo "$manifest" | sed -n '2p')" = "LAYOUT=$layout" ] || return 1
	for part in entry kernel root; do
		size=$(echo "$manifest" | awk -v p="$part" '$1 == p { print $2 }')
		expected=$(echo "$manifest" | awk -v p="$part" '$1 == p { print $3 }')
		case "$size" in ''|*[!0-9]*) return 1;; esac
		[ "${#size}" -le 7 ] && [ "${#expected}" -eq 64 ] || return 1
		align=4096
		case "$part" in
			entry) limit=1572864;;
			kernel) limit=3014656;;
			root) limit=$((6422528 - 5 * 131072)); align=131072;;
		esac
		[ "$size" -gt 0 ] && [ "$size" -le "$limit" ] &&
			[ $((size % align)) -eq 0 ] || return 1
		[ "$(hg8245_member "$1" "$part" | wc -c)" -eq "$size" ] || return 1
		actual=$(hg8245_member "$1" "$part" | sha256sum) || return 1
		[ "${actual%% *}" = "$expected" ] || return 1
		case "$part" in
			entry) entrysize=$size; entryhash=$expected;;
			kernel) kernelsize=$size; kernelhash=$expected;;
			root) rootsize=$size; roothash=$expected;;
		esac
	done
}

platform_check_image() {
	if [ -r /sys/firmware/devicetree/base/chosen/openwrt,system-sha256 ]; then
		hg8245_check_install_image "$1"
	else
		hg8245_check_upgrade "$1" || {
			echo "Expected an HG8245V2 sysupgrade image and the installed B176 NOR layout." >&2
			return 1
		}
	fi
}

hg8245_compare_flash() {
	local actual
	actual=$(dd if="${1}ro" bs=4096 count="$(($2 / 4096))" 2>/dev/null | sha256sum)
	[ "${actual%% *}" = "$3" ] || { echo "Readback mismatch on $1" >&2; return 1; }
}

platform_do_upgrade() {
	local entrydev kerneldev rootdev entrysize kernelsize rootsize entryhash kernelhash roothash
	local image="$1" archive unpacked room
	if [ -r /sys/firmware/devicetree/base/chosen/openwrt,system-sha256 ]; then
		hg8245_do_install "$1"
		return
	fi
	hg8245_check_upgrade "$1" || exit 1
	set --
	if [ -n "$UPGRADE_BACKUP" ]; then
		archive=$UPGRADE_BACKUP
		[ -s "$archive" ] && gzip -t "$archive" || exit 1
		unpacked=$(gzip -dc "$archive" | wc -c)
		room=$((6422528 - rootsize - 4 * 131072))
		[ $((unpacked + $(wc -c < "$archive"))) -le "$room" ] || {
			echo "Saved settings exceed the available JFFS2 space." >&2
			exit 1
		}
		set -- -s "$rootsize" -j "$archive"
	fi

	# Stage 2 runs in RAM. Erase the old overlay even when its boundary moves.
	# The new SquashFS ends on the real NOR erase boundary.
	mtd erase "$rootdev" || exit 1
	( hg8245_member "$image" root && {
		if [ -n "$UPGRADE_BACKUP" ]; then printf '\336\255\300\336'
		else printf '\205\031\003\040\014\000\000\000\261\260\036\344'; fi
	} ) | mtd "$@" write - "$rootdev" || exit 1
	hg8245_compare_flash "$rootdev" "$rootsize" "$roothash" || exit 1
	hg8245_member "$image" kernel | mtd write - "$kerneldev" || exit 1
	hg8245_compare_flash "$kerneldev" "$kernelsize" "$kernelhash" || exit 1
	hg8245_member "$image" entry | mtd write - "$entrydev" || exit 1
	hg8245_compare_flash "$entrydev" "$entrysize" "$entryhash" || exit 1
	sync
}
