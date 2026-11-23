# OpenWrt for the Huawei EchoLife HG8245V2

[Русский](README.md) · **English**

## ⚠️ RC12 is an experimental release

> [!WARNING]
> **Only download and install these images if you can recover this router yourself using UART and U-Boot.** The board has a single system slot. An interrupted flash can leave the router unable to boot or serve LuCI. Before writing to NOR, save a full factory backup from your own unit and verify that you can interrupt U-Boot and start the recovery image over TFTP. [Backup and installation guide (Russian)](validation/hg8245v2-rc12.md)

## Hardware covered by RC12

The tested unit is a **Huawei EchoLife HG8245V2, board 1 / PCB 3**, with 16 MiB of NOR flash, Huawei firmware `V100R003C00SPC106B176`, and U-Boot 1.1.4. RC12 runs OpenWrt 24.10.8 with Linux 6.6.144. Installation has been tested on one physical unit. Check the board revision and fitted components before choosing an image. Other HG8245V2 revisions need separate testing and may require additional drivers.

| Component | Hardware on the tested board | What works |
| --- | --- | --- |
| CPU | **HiSilicon SD5113**, ARM1176JZF-S (ARMv6) | RC12 boots from NOR. U-Boot detects 64 MiB of RAM and passes 50 MiB to Linux. |
| 2.4 GHz Wi-Fi | The board owner identifies the chip as **Atheros AR9227**. The `ath9k` driver reports **AR9287 revision 2**, PCI ID `168c:002d`. | `owl-loader` applies the factory calibration. A client associated with the access point and exchanged traffic on the LAN. [RC12 boot log](validation/hg8245v2-rc12-board-boot.txt) |
| Ethernet | **SD5113** MAC and external **Atheros AR8316 rev. 1** gigabit switch | LAN1–LAN4 are individually exposed and belong to `br-lan` by default. |
| GPON controller | Integrated **SD5113** GPON block | The port includes PLOAM handling, an OMCI agent, and the `optic` network interface for Ethernet over GEM. The board reported loss of signal with the fiber unplugged. Registration with a live OLT and internet access over fiber still need testing. |
| Optical transceiver | **Huawei HPSF2120** with I²C diagnostics | The board returned the module ID, receive power with the fiber disconnected, and sensor readings. The driver also recognizes **RTXM167-407**, **RTXM167-407-C10**, and other IDs found in the stock firmware. Only the HPSF2120 has been tested on hardware. [Optical module profiles](validation/hg8245v2-optics.md) |
| Analog telephony | Dual-channel **Le88266DLC** SLIC and the SD5113’s **ZSP** voice DSP | TEL1 and TEL2 support ringing, hang-up, and intercom. A SIP call over Ethernet carried audio in both directions. [DSP firmware and line profiles](docs/voice-firmware.md) |
| Flash and USB | **16 MiB x16 parallel CFI NOR** and integrated **EHCI/OHCI** controllers | RC12 booted from NOR. EHCI detected a USB drive and Linux enumerated both partitions. |

The port needs further development and support for more chip variants. Optical tests on this board were performed with the fiber disconnected. Memory use and service stability after days of continuous load are still unknown.

**Estimated completion for home internet over GPON, LAN, and telephone service is 70–75%.** NOR installation, local networking, and calls between real endpoints work. The largest unknown is whether the board can register with an operator’s OLT, receive its service configuration over OMCI, and carry subscriber traffic in both directions.

## RC12 release files — which ones to download

**To try OpenWrt in RAM**, download `recovery`. **To install over the supported Huawei firmware**, use `recovery` for the backup, then `installer` and `factory-region` from the same release to flash the system. **To update through LuCI**, choose the target release’s `sysupgrade` image.

| File in `artifacts/hg8245v2-rc12/` | How to use it |
| --- | --- |
| [hg8245v2-rc12-recovery.bin](artifacts/hg8245v2-rc12/hg8245v2-rc12-recovery.bin) | Boot it from U-Boot over TFTP to run OpenWrt in RAM. Use it to test hardware and back up the full 16 MiB factory NOR to your PC before installation. |
| [hg8245v2-rc12-installer.bin](artifacts/hg8245v2-rc12/hg8245v2-rc12-installer.bin) | After the backup, boot this installer from U-Boot over TFTP. It runs in RAM and accepts the matching `factory-region` file from your PC. Writing begins when you issue the flash command in the installer console. |
| [hg8245v2-rc12-factory-region.bin](artifacts/hg8245v2-rc12/hg8245v2-rc12-factory-region.bin) | The contents of the NOR system region. Serve it over HTTP from your PC for the RAM installer to write. It covers addresses `0x060000–0xADFFFF` inclusive. U-Boot and per-unit factory data reside outside this range. |
| [hg8245v2-rc12-sysupgrade.bin](artifacts/hg8245v2-rc12/hg8245v2-rc12-sysupgrade.bin) | For an existing compatible OpenWrt installation. Upload it under **LuCI → System → Backup / Flash Firmware → Flash image…**. Leave **Keep settings** enabled to carry over your configuration. |
| [SHA256SUMS](artifacts/hg8245v2-rc12/SHA256SUMS) | SHA-256 checksums for the four images. If you downloaded the full set, run `sha256sum -c SHA256SUMS` from the directory containing the files. |

Before writing, the installer checks the hashes of the bootloader, partition layout, and system image. The guide lists the PC, U-Boot, and OpenWrt console commands in order. [Installation and upgrade guide (Russian)](validation/hg8245v2-rc12.md)

## Readiness by subsystem

Each range estimates how much of the implementation and validation is complete. A conversation between two handsets exercises the entire path from microphone to earpiece. Loading a driver or creating a network interface verifies a single step. The table credits results from earlier builds and identifies which scenarios still need testing on RC12.

The overall estimate gives equal weight to local functions and the optical WAN. The first nine rows, from installation through USB, average about 85%. The next five, from optical diagnostics through subscriber internet access, average about 60%. Using the midpoints gives an overall estimate of about 73%. A live OLT test is needed for a more precise estimate. QoS, IPTV, and BBU are assessed separately. There is too little data to rate long-term reliability.

| Subsystem | Estimate | Evidence and remaining work |
| --- | ---: | --- |
| First installation, boot, SquashFS, and JFFS2 | 90–95% | RC12 was installed over `V100R003C00SPC106B176` on one unit. The boot log records startup from NOR and filesystem mounts. Repeated upgrades and cold boots across multiple units remain untested. |
| Factory data and recovery | 80–90% | Full-chip backup and pre-flash checks for the bootloader, layout, and image are implemented. All 16 MiB were compared byte for byte after a simulated installation in QEMU, including BoardInfo and calibration data. Recovery after an interrupted write on real hardware remains untested. There is one system slot. |
| LuCI upgrade with settings retained | 75–85% | QEMU tests covered HTTP upload, sysupgrade, reboot, and UCI restoration. A subsequent LuCI upgrade from an installed RC12 still needs confirmation on a physical board. |
| LAN1–LAN4 and Ethernet bridging | 85–95% | The standard recovery image exposes four separate ports. DHCP, ping, and LuCI access worked on earlier builds. All four interfaces joined the bridge in the RC12 boot log. Throughput and packet loss with all ports active have yet to be measured. |
| 2.4 GHz Wi-Fi | 85–90% | A client joined via WPA2 and received an IP address on an earlier build. RC12 loads `ath9k`. Sustained transfers, multiple clients, and repeated reconnections still need testing on RC12. |
| LuCI and local management | 80–90% | LuCI was accessible on the board in earlier builds. RC12 includes telephony, GPON, and system pages. Managing all services simultaneously under memory pressure remains untested. |
| TEL1/TEL2, ringing, and intercom | 90–95% | An earlier build handled ringing, hang-up, and two-way speech between handsets. RC12 needs repeated calls and a long call with memory monitoring. |
| SIP over Ethernet | 85–90% | An earlier build carried two-way audio on a SIP call. Network interruption recovery and an operator-provisioned voice service over GPON remain untested. |
| USB 2.0 and filesystems | 75–85% | RC12 detected a physical drive and both partitions. FAT32 and ext4 write/read tests passed in QEMU. Sustained I/O on a physical drive remains untested. |
| Optical module and LOS diagnostics | 80–90% | The board identified the HPSF2120 and reported −45.47 dBm receive power and `LOS=1` with the fiber unplugged. Readings under a live signal and other module models still need testing. |
| GPON registration (PLOAM, ranging, O5) | 45–60% | The driver reads the factory serial number, handles PLOAM and ranging parameters, and forms ONU responses. ONU-ID assignment, entry into O5, and re-registration have yet to be observed with a live OLT. |
| OMCI | 55–65% | The agent handles MIB Reset/Upload and managed-entity create, get, set, and delete operations. Host tests applied a single-bridge VEIP profile with VLANs. No session has yet been captured in which an operator’s OLT provisioned a service on this board. |
| Ethernet over GEM | 50–65% | GEM/T-CONT selection and the receive path into `optic` are implemented. Upstream and downstream frames on an OLT-assigned GEM port still need verification on fiber. |
| Subscriber internet over the optical WAN | 35–50% | OpenWrt binds `wan` to `optic` and includes PPPoE. End-to-end service depends on registration and GEM. A DHCP lease or PPPoE session over an OLT and internet traffic to and from a LAN client still need demonstration. |
| Optical QoS | 35–50% | Host tests cover GEM selection by PCP/DSCP. The current profile lacks hardware queue weights and shaping. Prioritization under load remains unmeasured on fiber. |
| IPTV | 35–50% | The image includes an IGMP proxy and configuration page. Operator channel reception, multicast group changes, and a dedicated multicast GEM still need verification on hardware. |
| LEDs and buttons | 65–80% | Pin assignments were mapped against the stock firmware, UCI exposes the LED rules, and LAN link indication was observed. The full set of PON, LOS, WPS, POTS, and POWER states still needs testing on RC12. |
| External battery backup unit (BBU) | 5–10% | Analysis of `V100R003C00SPC106B176` identified the UART, baud rate, and part of the message format. The connector pinout and communication with a physical BBU remain unknown. |
| Operator remote management (TR-069) | 0% | RC12 has no CWMP client. ACS registration and provisioning through an ACS remain untested. |
| Long-term operation, RAM use, and service recovery | No data | There are no long-duration measurements covering memory use, call cycles, reconnections, or combined Ethernet, Wi-Fi, and USB load. Whether there are memory leaks is unknown. |

**Memory.** At startup, the RC12 kernel reports `41576K/51200K available`. The published log ends around 245 seconds after boot. An earlier RAM build killed processes and eventually panicked. Installed RC12 needs longer measurements to distinguish peak memory pressure from a gradual leak. [RC12 boot log](validation/hg8245v2-rc12-board-boot.txt)

Test records cover [installation, upgrade, and USB](validation/hg8245v2-rc12.md), [LAN and Wi-Fi](validation/hg8245-lan-wifi-luci-test.txt), and [UART output with the fiber disconnected](validation/hg8245-gpon-rx.uart.txt). Component notes cover [stock optical profiles](validation/hg8245v2-optics.md), [voice firmware](docs/voice-firmware.md), [front-panel LEDs](validation/hg8245-front-panel-leds.md), and [BBU research](validation/hg8245-bbu.md).

## GPON on a live fiber connection

RC12 binds the `wan` network to `optic`. The OMCI agent implements the baseline message format and a single-bridge VEIP profile. Host tests cover MIB operations, mapping two GEM ports to different T-CONTs, PCP/DSCP selection, and symmetric VLAN translation. [OMCI test](tools/test_sd5113_omci.py) · [GEM and priority test](tools/test_sd5113_gem_qos.py)

| Step | What still needs to be verified on fiber |
| --- | --- |
| Registration | Observe LOS clearing, the OLT receiving the serial number, ONU-ID assignment, ranging, and entry into O5. Repeat after signal loss and restoration. |
| OLT provisioning | Capture OMCI traffic on the assigned OMCC and compare T-CONT/Alloc-ID, GEM Port-ID, and VLAN rules against the subscriber profile on the OLT. |
| Internet over GEM | Verify transmit and receive on the assigned GEM, obtain a DHCP lease or establish a PPPoE session, and reach the internet from a LAN client. |
| QoS and IPTV | Measure traffic distribution across GEM ports under load and receive an operator IPTV stream after an IGMP join. |

The implementation covers a limited single-bridge VLAN profile. Extended OMCI messages, a dedicated multicast GEM with operator ACLs, and hardware queue weights and shaping remain outside that profile. The image includes `igmpproxy` for routed IPv4 IPTV.

On a connected line, use these commands to inspect registration, configured GEM ports, WAN state, and Ethernet counters:

```sh
ubus call hg8245.gpon status
ubus call network.interface.wan status
grep 'optic:' /proc/net/dev
```

## Build

Build as a regular Linux user. Under WSL2, put the build directory in your Linux home directory. The script fetches OpenWrt, verifies revision `0b795ce79e23b553aa184080c390f9ce92a2b6d4`, and applies the sources in this repository. Install the build dependencies on Ubuntu 24.04:

```sh
sudo apt update
sudo apt install build-essential clang flex bison gawk gcc-multilib g++-multilib gettext git libncurses-dev libssl-dev python3 python3-setuptools rsync swig unzip zlib1g-dev file wget curl ca-certificates device-tree-compiler xz-utils perl libelf-dev
```

In WSL2, run `export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin` in the build shell. This keeps Windows paths containing spaces from interrupting OpenWrt’s filesystem packaging step.

You can build all four RC12 images without a factory dump. From the repository root, run:

```sh
HG8245_RELEASE_DIR="$PWD/dist/rc12" bash tools/build-hg8245-factory.sh
(cd dist/rc12 && sha256sum -c SHA256SUMS)
```

The repository includes the Huawei container format, reference bootloader and partition layout hashes for `V100R003C00SPC106B176`, and voice DSP firmware. OpenWrt reads the serial number, MAC address, and Wi-Fi calibration from the device itself. Before the first NOR write, save a full backup of your unit for recovery. [Installation guide (Russian)](validation/hg8245v2-rc12.md)

To build only the RAM image:

```sh
HG8245_CONFIG="$PWD/openwrt-target/config.sd5113_hg8245.rc12" HG8245_ARTIFACT_DIR="$PWD/dist/ram" bash tools/build-hg8245-ram.sh
```

## Repository layout

### Where to find the RC12 sources

Board support lives in the [SD5113 target](openwrt-target/target/linux/sd5113/). Kernel module packages and userspace services live in the [port’s package tree](openwrt-target/package/). The [build script](tools/build-hg8245-ram.sh) adds these sources to OpenWrt 24.10.8. The [RC12 configuration](openwrt-target/config.sd5113_hg8245.rc12) selects the packages.

| Component | Sources in this repository |
| --- | --- |
| SD5113 and board support | [Controller drivers, Ethernet MAC, and MDIO](openwrt-target/target/linux/sd5113/files-6.6/drivers/), [device trees, including USB and NOR](openwrt-target/target/linux/sd5113/files-6.6/arch/arm/boot/dts/hisilicon/), [kernel and AR8316 patches](openwrt-target/target/linux/sd5113/patches-6.6/) |
| GPON and optics | [PLOAM/GEM driver](openwrt-target/package/kernel/sd5113-gpon-rx/), [OMCI agent](openwrt-target/package/network/sd5113-omcid/), [optical transceiver driver](openwrt-target/package/kernel/hpsf2120/) |
| Telephony | [Le88266DLC driver, line profiles, and firmware](openwrt-target/package/kernel/le88266/), [ZSP voice DSP and PCM code](tools/hg8245-zsp/), [SIP](openwrt-target/package/utils/hg8245-sip/). The build script copies the ZSP sources into the Le88266 module. The [package recipe](openwrt-target/package/kernel/le88266/Makefile) pins the externally downloaded VoicePath source archive and its SHA-256. |
| Network setup, LEDs, and LuCI | [Board defaults and upgrade scripts](openwrt-target/target/linux/sd5113/base-files/), [LED service and LuCI packages](openwrt-target/package/utils/) |

The base OpenWrt and Linux kernel trees supply the `ath9k` Wi-Fi driver and the EHCI/OHCI USB drivers. The device trees and RC12 configuration supply the board-specific settings.
- `tools/` contains the build, packaging, image verification, and test scripts. `firmware_current/tools/` contains the reader for Huawei’s stock SquashFS. Some research tests expect modules extracted from `V100R003C00SPC106B176`.
- `validation/` contains the RC12 installation guide, hardware test results, and board notes. The [Ethernet, timer, and watchdog notes](firmware_current/extracted/README.md) document stock register use and its relationship to RC12.
- `artifacts/hg8245v2-rc12/` contains the verified release files. Store personal NOR dumps, BoardInfo, configuration backups, and calibration data separately.
