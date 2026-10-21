# DHCP, TFTP and NFS servers for the Raspberry Pi 3B+ IOC

How to install, configure and start the servers the IOC needs on the
machine cabled to the Pi, and which files go where. Written 2026-10-20
after the cardless network boot was verified; the mechanisms are in
`RTEMS6_RASPBERRYPI_BUILD.md` sections 10 and 10b.

## 1. Picture

```
 Pi 3B+ (ue0, MAC b8:27:eb:<serial>) ----cable---- server port 192.168.2.10/24
   DHCP client (ROM, bootcode.bin, RTEMS dhcpcd)       dnsmasq: DHCP + TFTP (+ NTP option 42)
   TFTP client (firmware)                              /srv/tftp/rpi  (make tftp)
   NFS client (rtemsNfsMount, optional)                nfs-kernel-server: /srv/nfs/ioc (optional)
                                                       NAT to the internet for NTP (optional)
```

The Pi gets 192.168.2.21 and the hostname `rtems-pi3` from a DHCP
reservation keyed on its MAC. The firmware derives that MAC from the board
serial (`b8:27:eb` + low 24 bits), and the IOC sets the same one on `ue0`
before its own DHCP, so one reservation covers the boot ROM, the firmware
and RTEMS.

## 2. The files and where they go

Built by `make` in this repository (`bin/RTEMS-raspberrypi2/`):

| File | What it is | Goes to |
|---|---|---|
| `rpi3_plain.img` | the IOC kernel, needs `epics.tar.gz` | SD card as `kernel7.img`, and TFTP root as `kernel7.img` |
| `epics.tar.gz` | st.cmd, envPaths, db, dbd, loadable modules (14 files), loaded to RAM by the firmware (`initramfs epics.tar.gz 0x30000000` in config.txt) | SD card and TFTP root, same name |
| `rpi3.img` | alternative kernel with the same files compiled in; boots without the archive | only if you want a one-file card: copy it as `kernel7.img` |

Fixed files from `SD_card/` (Raspberry Pi firmware and settings):
`bootcode.bin`, `start.elf`, `fixup.dat`, `bcm2710-rpi-3-b-plus.dtb`,
`config.txt`, `cmdline.txt` (static network) or `cmdline-dhcp.txt`
(DHCP). `SD_card/kernel7.img` and `SD_card/epics.tar.gz` are symlinks
into `bin/`, so `SD_card/` is always the complete, current card set.

| Target | Command |
|---|---|
| SD card, new card | `cp -L SD_card/* /media/$USER/<CARD>/` then rename `cmdline-dhcp.txt` to `cmdline.txt` if the IOC should use DHCP; `sync` |
| SD card, update after `make` | `cp -L SD_card/kernel7.img SD_card/epics.tar.gz /media/$USER/<CARD>/ && sync` |
| SD card, update after `make ramfs` (st.cmd/db change only) | `cp -L SD_card/epics.tar.gz /media/$USER/<CARD>/ && sync` |
| TFTP root (network boot) | `make tftp` (= `scripts/update_tftp.sh`, see section 4) |

`cmdline.txt` is one line. `--net=dhcp[,ntp=A.B.C.D][,host=NAME]` makes the
IOC use DHCP, `--net=A.B.C.D/N[,gw=..][,ntp=..]` a static address, nothing
or `--net=static` the compiled-in default 192.168.2.21/24. The TFTP root
always gets the DHCP version.

## 3. DHCP server

The ROM of the 3B+ and `bootcode.bin` need, in the DHCP offer: an
address, `next-server` = the TFTP server, and option 43 containing the
text `Raspberry Pi Boot`. RTEMS additionally takes option 42 (NTP server)
and option 12 (hostname) from the lease. dnsmasq provides all of it.

### 3a. On a laptop with NetworkManager (what this project uses)

NetworkManager's "shared" mode turns the wired port into a small router
with its own dnsmasq (DHCP + DNS, NAT towards the uplink). Steps, done
once; `<PROFILE>` is the wired connection's name (`nmcli con show`), here
"SB 12", `<PORT>` the interface, here `enp1s0`:

```bash
# 1. the port becomes 192.168.2.10/24 and shares the laptop's uplink
nmcli con modify "<PROFILE>" ipv4.method shared ipv4.addresses 192.168.2.10/24 ipv4.gateway "" ipv4.dns "" ipv6.method ignore

# 2. extra dnsmasq settings for the Pi (NTP option, reservation, TFTP)
sudo mkdir -p /etc/NetworkManager/dnsmasq-shared.d
sudo cp patches/dnsmasq-shared-rpi-ioc.conf /etc/NetworkManager/dnsmasq-shared.d/rpi-ioc.conf
#    edit the dhcp-host line: MAC of your board (IOC boot line "Board serial ..., MAC ...";
#    or b8:27:eb:*:*:* for any Pi), and the NTP address if yours differs

# 3. firewall: forwarded packets (NTP to the internet) are dropped by ufw's default
sudo sed -i 's/^DEFAULT_FORWARD_POLICY="DROP"/DEFAULT_FORWARD_POLICY="ACCEPT"/' /etc/default/ufw && sudo ufw reload

# 4. (re)start the profile; dnsmasq reads the conf dir at every start
nmcli con up "<PROFILE>"
```

NetworkManager starts dnsmasq when the port has a link and stops it when
the link drops. On this laptop the restart takes 0.2 s after link-up and
the Pi's first DHCP request comes 0.8 s later, so it works; if a server is
slower, add to `/etc/NetworkManager/NetworkManager.conf`:

```
[device]
match-device=interface-name:<PORT>
ignore-carrier=yes
```

and `sudo systemctl restart NetworkManager`; the profile and dnsmasq then
stay up without a cable.

### 3b. On a server without NetworkManager (plain dnsmasq)

```bash
sudo apt install dnsmasq
sudo tee /etc/dnsmasq.d/rpi-ioc.conf >/dev/null <<'CONF'
interface=<PORT>
bind-interfaces
port=0                                  # no DNS, DHCP + TFTP only
dhcp-range=192.168.2.50,192.168.2.99,255.255.255.0,12h
dhcp-option=option:router,192.168.2.10  # only if this host routes to the internet (NTP)
dhcp-option=option:ntp-server,172.233.177.198
dhcp-host=b8:27:eb:45:d7:b0,192.168.2.21,rtems-pi3,infinite
log-dhcp
enable-tftp
tftp-root=/srv/tftp/rpi
pxe-service=0,"Raspberry Pi Boot"
CONF
sudo ip addr add 192.168.2.10/24 dev <PORT>      # or the distribution's static-address config
sudo systemctl enable --now dnsmasq
```

Without a route to the internet, drop the `router` option and let the IOC
use a local NTP server in `ntp-server`. Without any NTP server the IOC
boots anyway but keeps the RTEMS default time of 1988 (build guide,
section on `osdTime.cpp` and the early NTP step).

## 4. TFTP server (network boot, and the rpiKexec kernel fetch)

TFTP is served by the same dnsmasq (`enable-tftp`, `tftp-root`), as user
`nobody`, so the directory must be world-readable.

```bash
sudo mkdir -p /srv/tftp/rpi && sudo chown $USER /srv/tftp/rpi
make tftp            # copies SD_card/* with symlinks resolved, cmdline-dhcp.txt as cmdline.txt,
                     # and creates <serial>/ with symlinks (firmware asks <serial>/<file> first)
```

Board serial: last 8 hex digits of the IOC's `Board serial` boot line
(this board `9645d7b0`); more boards: `SERIALS="9645d7b0 1234abcd" make tftp`
(how to find a new board's serial and MAC: section 5).
A board-specific `config.txt` or `cmdline.txt` is a real file in
`/srv/tftp/rpi/<serial>/` replacing the symlink.

Run `make tftp` after every `make` or `make ramfs` whose result should
reach a network-booted board; for such a board it IS the card copy. A
running IOC can fetch and warm-start the new kernel from there without a
reboot from the card: `rpiKexec "192.168.2.10","kernel7.img"` (check,
prints crc32; compare with `python3 scripts/crc32.py SD_card/kernel7.img`)
then `rpiKexec "192.168.2.10","kernel7.img",1`.

Check from the server itself:

```bash
curl -s -o /dev/null -w '%{size_download}\n' tftp://192.168.2.10/bootcode.bin        # 52624
curl -s -o /dev/null -w '%{size_download}\n' tftp://192.168.2.10/9645d7b0/kernel7.img
```

## 5. Adding a board: finding its MAC address and serial

Every Pi 3B+ has a fixed MAC derived from its board serial: `b8:27:eb` +
the low 24 bits of the serial. The serial is not printed on the board, so
the number is known only after the board has run once. Three ways, all
giving the same result:

1. **Cardless power-on, nothing else needed.** Cable the board to the
   server port with no SD card and power it on. The boot ROM sends a DHCP
   request with the MAC; watch it on the server:

   ```bash
   sudo journalctl -f -t dnsmasq-dhcp
   # ... DHCPDISCOVER(enp1s0) b8:27:eb:xx:xx:xx   with "vendor class: PXEClient..."
   ```

   The board gets a pool address, fetches the shared files from the TFTP
   root and boots the IOC even before it is registered.

2. **IOC console.** Boot the board from any card or over the network and
   read its boot line; the last 8 hex digits of the serial name the
   board's TFTP directory, the MAC is the dnsmasq key:

   ```
   Board serial 00000000xxxxxxxx, MAC b8:27:eb:xx:xx:xx (device tree local-mac-address)
   ```

3. **Raspberry Pi OS card**, if one is at hand: `grep Serial /proc/cpuinfo`
   and `cat /sys/class/net/eth0/address`.

Then register the board:

```bash
# DHCP: one reservation per board, each with its own address (dnsmasq refuses two for one IP)
#   in patches/dnsmasq-shared-rpi-ioc.conf, then copy it to /etc/NetworkManager/dnsmasq-shared.d/
dhcp-host=b8:27:eb:xx:xx:xx,192.168.2.22,rtems-pi3-2,infinite
# TFTP: its own directory next to the first board's
SERIALS="9645d7b0 xxxxxxxx" make tftp
```

A board without a reservation and without a directory still boots: pool
address, shared default files. The reservation is what gives it a fixed
address and hostname, the directory is what allows a board-specific
kernel, archive or cmdline.

## 6. Booting the Pi

- With a card: the normal case; the card's `cmdline.txt` decides static or
  DHCP. Firmware boot to iocInit takes about 17 s.
- Without a card (verified 2026-10-20): power on with no card; the ROM
  tries SD, USB, then Ethernet, and the firmware fetches everything from
  the TFTP root. Same 17 s. The server must be up first.
- A card holding only `bootcode.bin` is a fallback for boards whose ROM
  network boot misbehaves; not needed on this 3B+.

## 7. NFS server (optional, for `rtemsNfsMount`)

```bash
sudo apt install nfs-kernel-server
sudo mkdir -p /srv/nfs/ioc && sudo chown $USER /srv/nfs/ioc
echo '/srv/nfs/ioc 192.168.2.0/24(rw,sync,no_subtree_check,insecure,no_root_squash,sec=sys)' | sudo tee -a /etc/exports
sudo exportfs -ra && sudo systemctl enable --now nfs-kernel-server
```

On the IOC: `rtemsNfsMount "192.168.2.10:/srv/nfs/ioc", "/mnt/ioc"`
(NFSv4 over TCP by default; `"nfsv3,udp"` as third argument for v3). Files
written by the Pi arrive as root. Two libbsd limitations: create files in
a subdirectory, not in the mount root, and mount a server only once per
boot (build guide 10e).

## 8. Watching and troubleshooting

```bash
# every DHCP exchange and TFTP transfer (NetworkManager's dnsmasq logs under these tags)
sudo journalctl -f -t dnsmasq-dhcp -t dnsmasq-tftp -t NetworkManager
pgrep -a dnsmasq                      # exactly one instance with --conf-dir=/etc/NetworkManager/dnsmasq-shared.d
nmcli -t -f ipv4.method,ipv4.addresses con show "<PROFILE>"   # shared, 192.168.2.10/24
ping -c 3 192.168.2.21                # the IOC once up
```

Normal log lines during a network boot: `vendor class:
PXEClient:Arch:00000:UNDI:002001` (ROM and bootcode.bin), `Early terminate
received` followed by `failed sending <file>` (the firmware probes the
file size first), `file ... not found` for `bootsig.bin`, `autoboot.txt`,
`recovery.elf`, `dt-blob.bin`, `bootcfg.txt`, `overlays/overlay_map.dtb`,
`armstub8-32.bin` (optional files), a second dnsmasq start when RTEMS
resets the Ethernet chip, then `vendor class: dhcpcd-6.2.1:RTEMS-6.0.0`
from the IOC. Problems seen and their causes: no offer at all = dnsmasq not
running (link down, profile not shared, two instances fighting for the
port); offer but no TFTP = `pxe-service` line missing or the TFTP root not
readable by `nobody`; IOC up but no NTP = forwarding blocked (ufw) or
option 42 missing; IOC waits 10 min then has no network = no DHCP server
at all (there is no static fallback in DHCP mode; put `--net=static` or an
address in cmdline.txt instead).
