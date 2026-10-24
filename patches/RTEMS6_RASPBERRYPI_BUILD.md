# RTEMS 6 for the Raspberry Pi 3B+: toolchain, kernel, libbsd, EPICS

How the RTEMS 6 system used by this IOC was built, with the patches that
are required. Written 2026-10-14 from the working installation on the
development laptop after the USB "hang" was traced to a bug in the RTEMS
Raspberry Pi BSP and fixed. Every step below has been run on this machine;
the exact revisions are the ones in use.

Companion files in this repository:

| File | Purpose |
|---|---|
| `rtems-raspberrypi-irq-dispatch-fix.patch` | **Required.** One-line fix in the RTEMS kernel BSP interrupt dispatcher. Without it the IOC freezes after 30 s to 3 min of USB/network traffic. |
| `rtems-telnetd-pty-eof-fix.patch` | **Required.** Fix in the RTEMS kernel telnetd pty driver (`cpukit/telnetd/pty.c`, builds `libtelnetd.a`). Without it a telnet session that is closed from the client side (not with `bye`) leaves its pty stuck at end-of-file, and every later telnet connection prints the prompt and is dropped at once until the IOC reboots (2026-10-22). Present in RTEMS 6.2 and in upstream main. |
| `rtems-kernel-raspberrypi2-config.ini` | The `config.ini` used to build the kernel (POSIX API on, tests on). |
| `rtems-libbsd-raspberrypi.patch` | **Required.** rtems-libbsd additions for the Pi 3B+: LAN7515/LAN7800 `muge` USB Ethernet driver, nexus wiring, VideoCore USB power-on shim, high-speed UTMI PHY selection, BCM2835 DWC OTG errata workarounds. |
| `epics-base-R7.0.10-raspberrypi.patch` | **Required.** EPICS base changes (targets, image rules, libbsd NTP time provider with full-resolution NTP sync). See `EPICS_BASE_RASPBERRYPI_PATCH.md`, which also covers adding the PVAccess submodules. |
| `epics-base-osdTime-ntp-fraction.patch` | Only for a Base tree patched before 2026-10-14: the NTP-fraction fix alone. |
| `RPi_IOSup/README.md` | The GPIO driver module: labelled pins, interrupt inputs, records, measured 1 kHz results. |
| `rtems-libbsd-raspberrypi-debug-instrumentation.patch` | Optional. The hang-hunt instrumentation (RAM crash ring, tracer, probes) behind the `RPI_IOC_DEBUG` switch. Apply on top of the required libbsd patch only when debugging. |
| `rtems-raspberrypi-irq-dispatch-debug-markers.patch` | Optional. Crash-ring markers in the kernel dispatcher, same switch. Apply on top of the fix patch. |

## 1. Versions in use

| Component | Source | Revision |
|---|---|---|
| RTEMS Source Builder | github.com/RTEMS/rtems-source-builder, branch `6.2` | commit 3814cb0e |
| Cross tools | built by RSB | arm-rtems6-gcc 13.3.0, binutils 2.43, gdb 15.2, newlib 1b3dcfd, rtems-tools a2538346 |
| RTEMS kernel | github.com/RTEMS/rtems, branch `6.2` | be446b02d0 (2025-09-04) + fix patch |
| rtems-libbsd | github.com/RTEMS/rtems-libbsd, branch `6-freebsd-14` | c4fc2033 (2025-11-11), submodule freebsd-org at release/14.3.0-p2 |
| rtems-net-services | github.com/RTEMS/rtems-net-services, tag `6.2` | 3381863 (ntp, telnetd, ttcp) |
| rtems-addon-packages | git.rtems.org/rtems-addon-packages | 4f5951a, libtecla 1.6.3 |
| EPICS base | github.com/epics-base/epics-base, branch `7.0` | 5b23dcb46 (R7.0.10 + 51) + patch |
| Target | Raspberry Pi 3 Model B+ (BCM2837, LAN7515 USB hub + LAN7800 Ethernet) | BSP `arm/raspberrypi2`, 32-bit, single core |

Everything in sections 3 to 8 is automated by `scripts/build_rtems_rpi3.sh`
(tools, kernel, libbsd, net-services, libtecla) and
`scripts/build_epics_rpi3.sh` (EPICS base); see `scripts/README.md`. The
scripts pin the exact commits of the table above. Note that upstream moved
the `6.2` tags of rtems, rtems-source-builder and rtems-net-services after
this installation was made, so `git checkout 6.2` today gives different
code; use the commits.

Directory conventions used below and in the scripts under `~/RTEMS_SRC`:

```bash
export RTEMS_VERSION=6
export RTEMS_CPU=arm
export RTEMS_BSP=raspberrypi2
export HOME_DIR=$HOME/RTEMS_SRC                       # all sources
export RTEMS_ROOT=$HOME/RTEMS-PI/rtems-6/6            # install prefix
export PATH=$RTEMS_ROOT/bin:$PATH
```

The BSP library directory that everything installs into is
`$RTEMS_ROOT/arm-rtems6/raspberrypi2/lib`.

## 2. Host prerequisites (Ubuntu 22.04/24.04)

Per the RTEMS User Manual, "Host Computer":

```bash
sudo apt install build-essential g++ gdb unzip pax bison flex texinfo \
     python3-dev libpython3-dev libncurses-dev zlib1g-dev git u-boot-tools
```

About 10 GB of disk for sources and build trees. Builds of the tools take
30 to 60 minutes, kernel and libbsd a few minutes each.

## 3. Cross toolchain (RTEMS Source Builder)

```bash
mkdir -p $HOME_DIR && cd $HOME_DIR
git clone https://github.com/RTEMS/rtems-source-builder rsb
cd rsb && git checkout 6.2 && cd rtems
../source-builder/sb-check
../source-builder/sb-set-builder --jobs=$(nproc) --prefix=$RTEMS_ROOT 6/rtems-arm
```

Result: `$RTEMS_ROOT/bin/arm-rtems6-*`. Build reports land in
`$RTEMS_ROOT/share/rtems/rsb/`.

## 4. RTEMS kernel with the Raspberry Pi BSP

```bash
cd $HOME_DIR
git clone https://github.com/RTEMS/rtems.git kernel
cd kernel && git checkout 6.2

# REQUIRED: interrupt dispatcher fix (see section 9)
patch -p1 < /path/to/EPICS_APP_RPI3/patches/rtems-raspberrypi-irq-dispatch-fix.patch
# REQUIRED: telnetd pty end-of-file fix (see section 10, "Telnet")
patch -p1 < /path/to/EPICS_APP_RPI3/patches/rtems-telnetd-pty-eof-fix.patch

# BSP configuration: either generate and edit ...
./waf bspdefaults --rtems-bsps=arm/raspberrypi2 > config.ini
#   set RTEMS_POSIX_API = True   (EPICS needs the POSIX API)
#   BUILD_TESTS = True is optional (adds minutes; samples are handy for
#   checking a new SD card setup with ticker.exe)
# ... or use the one that was used here:
cp /path/to/EPICS_APP_RPI3/rtems-kernel-raspberrypi2-config.ini config.ini

./waf configure --prefix=$RTEMS_ROOT
./waf -j$(nproc)
./waf install
```

Notes
- `bspdefaults` on 6.2 emits some keys twice; `waf configure` then fails.
  The script `buildRTEMS-6-kernel-rpi3.sh` in `~/RTEMS_SRC` removes the
  duplicates automatically. The provided `config.ini` is already clean.
- Keep `RTEMS_SMP = False`. The IOC runs on core 0 only.
- Relevant BSP options in the file: `BSP_START_IN_HYP_SUPPORT = True`,
  `BSP_START_COPY_FDT_FROM_U_BOOT = True` (the GPU firmware passes a
  device tree and command line, the BSP reads `--console=` from it),
  `RPI_RAM_LENGTH_AVAILABLE = 0x10000000` (RTEMS uses the first 256 MB;
  the debug crash ring lives above that at 0x28000000).
- Incremental rebuild after a source change: `./waf -j8 && ./waf install`
  (about one minute with tests enabled).

## 5. rtems-libbsd (network stack, USB, Ethernet)

```bash
cd $HOME_DIR
git clone https://github.com/RTEMS/rtems-libbsd.git
cd rtems-libbsd && git checkout 6-freebsd-14
git submodule init && git submodule update        # freebsd-org, rtems_waf

# REQUIRED: Pi 3B+ support
patch -p1 < /path/to/EPICS_APP_RPI3/rtems-libbsd-raspberrypi.patch

./waf configure --prefix=$RTEMS_ROOT --rtems-bsps=arm/raspberrypi2
./waf -j$(nproc)
./waf install
```

What the patch adds and why (all of it was missing on `6-freebsd-14`):

- `freebsd/sys/dev/usb/net/if_muge.c`, `if_mugereg.h`: FreeBSD's Microchip
  LAN78xx driver, copied from the `freebsd-org` submodule with the five
  include-path adaptations libbsd needs (kernel-space header first,
  `rtems/bsd/...` paths for `unistd.h`, `opt_platform.h`, `usbdevs.h`,
  `miibus_if.h`). The Pi 3B+ Ethernet is a LAN7800 behind a LAN7515 hub;
  the Pi 3B (LAN9514) would use the existing `smsc` driver instead.
- `libbsd.py`: adds the two files above and `dwc_otg_rpi.c` to the build.
- `rtemsbsd/include/bsp/nexus-devices.h`: a `LIBBSP_ARM_RASPBERRYPI_BSP_H`
  block declaring the DWC OTG controller (memory at `BCM2835_USB_BASE`,
  IRQ `BCM2835_IRQ_ID_USB` = 9), USB hub/mass storage, `smsc` and `muge`
  on `uhub`, `ukphy` on `miibus`.
- `rtemsbsd/sys/dev/usb/controller/dwc_otg_rpi.c`: `dwc_otg_platform_init()`
  asks the VideoCore firmware through the mailbox to power the USB host
  controller, as FreeBSD's `bcm283x_dwc_fdt.c` does. Without it the hub
  never enumerates.
- `rtemsbsd/sys/dev/usb/controller/dwc_otg_nexus.c`: PHY type
  `DWC_OTG_PHY_UTMI`, 16 bit, instead of `DWC_OTG_PHY_INTERNAL`. The
  INTERNAL setting selects the full-speed-only PHY path and left the root
  port at 12 Mbit/s, which prevented the LAN7800 from attaching.
- `freebsd/sys/dev/usb/controller/dwc_otg.c`: three BCM2835/BCM2837 errata
  workarounds taken from Linux `dwc2` and the Ultibo project (wait for AHB
  idle after core reset, cap the FIFO at 4080 words, `GAHBCFG` AXI burst
  bits 0x10), plus one "DWC_OTG init done" summary line at boot.

Do **not** run `./freebsd-to-rtems.py` on this branch to import `if_muge.c`;
it aborts on `contrib/libpcap/grammar.y` and overwrites already-adapted
files on the way. The patch contains the adapted file; `git checkout -- .`
recovers the tree if the script was run by accident.

`config.inc` in the libbsd tree only sets defaults for libbsd's own test
programs; the IOC configures its network itself.

## 6. rtems-net-services (NTP client library, telnetd, ttcp)

```bash
cd $HOME_DIR
git clone https://github.com/RTEMS/rtems-net-services.git
cd rtems-net-services && git checkout 6.2
git submodule init && git submodule update
./waf configure --prefix=$RTEMS_ROOT --rtems-bsps=arm/raspberrypi2
./waf -j$(nproc)
./waf install
```

Provides `libntp.a` (linked by EPICS through `-lntp`) and `libttcp.a`.

## 7. libtecla (iocsh line editing)

EPICS is built with `COMMANDLINE_LIBRARY = LIBTECLA`; the RTEMS 6 libtecla
is linked without ncurses.

```bash
cd $HOME_DIR
# git://git.rtems.org/rtems-addon-packages.git (commit 4f5951a) is offline since
# the RTEMS move to GitLab; the libtecla tree and the RTEMS makefiles are kept
# in this repository:
mkdir rtems-addon-packages
tar -xzf /path/to/EPICS_APP_RPI3/patches/rtems-addon-packages-libtecla-1.6.3.tar.gz -C rtems-addon-packages
cd rtems-addon-packages/libtecla-1.6.3
export RTEMS_MAKEFILE_PATH=$RTEMS_ROOT/arm-rtems6/raspberrypi2
make -f ../RTEMS_Makefiles/Makefile.libtecla
```

Result: `libtecla.a` and `libtecla_r.a` in the BSP library directory. The
archive differs from upstream 4f5951a in two points: `Makefile.libtecla`
leaves the demo programs out, and `config.guess`/`config.sub` are 2022
versions so that `configure` accepts current hosts.

## 8. EPICS base and the IOC

```bash
cd /usr/local/epics/R7.0.10        # or anywhere
git clone -b 7.0 https://github.com/epics-base/epics-base.git base
cd base && git checkout 5b23dcb46
patch -p1 < /path/to/EPICS_APP_RPI3/epics-base-R7.0.10-raspberrypi.patch
# configure/os/CONFIG_SITE.Common.RTEMS: RTEMS_BASE = $RTEMS_ROOT, RTEMS_VERSION = 6
# configure/CONFIG_SITE:  CROSS_COMPILER_TARGET_ARCHS = RTEMS-raspberrypi2
# PVAccess: the modules are git submodules and are EMPTY after a clone
git submodule update --init modules/pvData modules/pvAccess modules/normativeTypes \
    modules/pvaClient modules/pvDatabase modules/pva2pva
make -j$(nproc)
```

See `EPICS_BASE_RASPBERRYPI_PATCH.md` for the PVAccess/QSRV details and
the checks that the libraries were really built for the RTEMS target.

Then this application:

```bash
git clone git@github.com:mirek23/EPICS_APP_RPI3.git
cd EPICS_APP_RPI3
# configure/RELEASE: EPICS_BASE = /usr/local/epics/R7.0.10/base
# rpi3App/src/rtems_netsetup.c: default static IP, netmask, gateway, NTP server
#   (cmdline.txt --net= overrides them at boot, section 10)
make
ls -l bin/RTEMS-raspberrypi2/kernel7.img      # -> rpi3.img
```

`RULES.RTEMS-Image` from the EPICS patch embeds `iocBoot`, `db` and `dbd`
into the ELF and runs `objcopy -O binary` to produce `rpi3.img`, linked as
`kernel7.img`. The SD card needs no NFS or TFTP server.

The same `make` also writes `bin/RTEMS-raspberrypi2/epics.tar.gz`, the
same eleven files as a gzip'ed tar (about 30 KB). Put it on the SD card
next to `kernel7.img` and add one line to `config.txt` (section 10) and
the firmware loads it into RAM at boot; the IOC then unpacks that archive
instead of the compiled-in copy. After that, a change to `st.cmd`, a
database or `rpi3.dbd` only needs a new `epics.tar.gz` on the card, not a
new kernel. Details in section 8a.

### 8a. Separate file for st.cmd, db and dbd (`epics.tar.gz`)

How it works (`rpi3App/src/rtems_fs_setup.c`, `rpi3App/src/Makefile`):

- `make` at the TOP builds two kernels and the archive; `make ramfs` builds
  only the archive (installs changed `*/Db` files first, under a second):
  - `bin/RTEMS-raspberrypi2/rpi3.img` (symlinked `kernel7.img`): st.cmd,
    db and dbd compiled in; uses `epics.tar.gz` when the firmware loaded
    it, the compiled-in copy otherwise. Boots with or without the archive.
  - `bin/RTEMS-raspberrypi2/rpi3_plain.img`: no compiled-in copy, about
    490 KB less code (the `.img` file is nearly the same size because the
    data segment sits at a fixed offset); needs `epics.tar.gz` on the card
    and refuses to start without it, printing what to add to `config.txt`.
  - `bin/RTEMS-raspberrypi2/epics.tar.gz`: st.cmd, envPaths, `db/*`,
    `dbd/rpi3.dbd`, paths relative to TOP (`tar --format=ustar -czf`,
    rule and file list in `rpi3App/src/ramfs.mk`). The same list feeds the
    compiled-in image (`rpi3_IMFS_FILES`), so the two cannot drift apart.
  Either kernel goes to the card as `kernel7.img`; both products are linked
  from the same objects (`rpi3_plain_SRCS` is `rpi3_SRCS` without
  `epicsRtemsFSImage.c`), so the second kernel costs one extra link.
- `config.txt` line `initramfs epics.tar.gz 0x30000000` (no `=` sign, this
  is the firmware's syntax) makes the GPU firmware load the file to physical
  address 0x30000000 (768 MB) and write the range into the device tree as
  `/chosen` `linux,initrd-start` / `linux,initrd-end`.
- The BSP copies the device tree at start (`BSP_START_COPY_FDT_FROM_U_BOOT`
  in `rtems-kernel-raspberrypi2-config.ini`). `epicsRtemsMountLocalFilesystem()`
  reads the two properties with libfdt, checks for the gzip magic (a plain
  `.tar` also works) and unpacks with RTEMS `Untar_FromGzChunk_Print()` into
  the in-memory file system at `/`, then starts `/iocBoot/iocrpi3/st.cmd`.
- Boot log with the archive: `***** Using file data loaded by the firmware:
  32768 bytes at 0x30000000 (config.txt initramfs) *****` followed by two
  `RAMFS:` checksum lines and the untar output. Without it the
  old line `***** Using compiled in file data *****` appears; the card keeps
  booting with the compiled-in copy, so an old card or a missing archive is
  harmless.

Why the address is fixed, why the archive is padded, and why the IOC
copies it at once:

- `followkernel` would place the file directly behind `kernel7.img`,
  inside the BSS/workspace area that the kernel zeroes at start, hence the
  fixed address 0x30000000.
- The firmware delivers the last partial 4 KB page of the file damaged.
  Two boots (2026-10-15) with a 28319-byte archive showed the first six
  pages checksum-identical to the host file and only the seventh, partial
  page (3743 bytes) different, already at `RTEMS_SYSINIT_BSP_START`, i.e.
  before any IOC or RTEMS code could have touched it; the unpack then failed
  with "Zlib inflate failed". A file whose size is a multiple of 4 KB
  arrives intact (verified: all eight pages of a 32768-byte archive matched
  the host, byte for byte in the last page). `rpi3App/src/ramfsPad.pl`,
  run by the Makefile after `tar`, therefore appends an empty stored gzip
  member so that the file ends on a 4 KB boundary plus one whole padding
  page; zlib stops after the first member, and `gzip -t` / `tar tzf` accept
  the file. The padding is about 4 to 8 KB.
- A fixed high address is inside the heap: the IOC uses
  `CONFIGURE_UNIFIED_WORK_AREAS`, so one RTEMS heap spans all ARM memory
  (0x00981EC0 to 0x3B400000, 938 MB, `rt malloc info` on the Pi). Plain
  `malloc` is first-fit from the bottom (a running IOC uses about 55 MB,
  all task stacks sit below 0x0320_0000), but aligned requests
  (`posix_memalign`, cache-aligned and DMA buffers of libbsd) are placed at
  the END of the free block, i.e. at the top of RAM, and work downward from
  there (`cpukit/score/src/heapallocate.c`). Nothing reaches 0x30000000 in
  normal operation, but nothing protects it either. `rtems_fs_setup.c`
  therefore registers a sysinit item (`RTEMS_SYSINIT_BSP_START`, right
  after the heap exists and before any driver or thread allocates) that
  copies the archive into a malloc'ed buffer and records its crc32. The
  unpack uses the copy; the "crc32 at boot X, now Y" line shows whether
  the original was touched in between (it never was so far).

The IOC prints two `RAMFS:` lines: the crc32 of the range at boot and at
unpack time (plus how many bytes changed in between, informational), and
the per-4 KB page checksums, which can be compared with the host:

```bash
python3 -c "import zlib;d=open('bin/RTEMS-raspberrypi2/epics.tar.gz','rb').read();print('%08x'%(zlib.crc32(d)&0xffffffff));print(' '.join('%08x'%(zlib.crc32(d[i:i+4096])&0xffffffff) for i in range(0,len(d),4096)))"
```

A hex dump of the last page is available with `-DRAMFS_DUMP_TAIL=1`
(`rpi3_CPPFLAGS` in `rpi3App/src/Makefile`), and the `rt` shell has
`mdump`, `wdump`, `ldump`, `medit`, `mfill` and `mmove`, e.g.
`rt mdump 0x30000000 0x100`.

The tar entries are made owner-writable (`--mode=u+rw,go+r`): a read-only
file left behind by a failed unpack blocked the compiled-in fallback with
"open: Permission denied" in that first test, which ended in an EPICS panic
and a watchdog reboot loop.

Day-to-day use:

```bash
vi iocBoot/iocrpi3/st.cmd            # or rpi3App/Db/*.db, RPi_IOSup/Db/*.db
make ramfs                            # only epics.tar.gz (kernels untouched)
cp bin/RTEMS-raspberrypi2/epics.tar.gz /media/$USER/boot/   # only this file needs to go to the card
```

`make` (everything) also refreshes the archive, so after a full build the
card needs the new `epics.tar.gz` as well as the kernel when the compiled-in
kernel should stay in step with the card copy.

Limits: a `.dbd` change (a new menu choice, record type or device) needs
a full `make`, because `dbd/rpi3.dbd` is generated in the kernel build;
`make ramfs` only packs the installed `dbd/rpi3.dbd` and reports an error
if it is missing. A change that adds code needs a new kernel, because the
code must be linked in. Everything that is only text (records, menus,
st.cmd commands) is covered by the archive.

What this application adds on top of a plain makeBaseApp IOC:
- `RPi_IOSup/`: GPIO driver and device support (library `RPi_IO`), pins
  configured by label in `st.cmd`, interrupt-driven inputs; see its README.
- `rpi3App/src/menuScan.dbd`: periodic scan rates `.01 second` and
  `.001 second` in addition to the standard ones (needs the 500 us RTEMS
  tick set in `rpi3App/src/rtems_config.c` and `configure/CONFIG_SITE.local`).
- `rpi3App/src/heartbeat.c`: BCM2835 hardware watchdog keeper, 8 s,
  `var hb_wdog_enable 0` disarms it from iocsh.
- `rpi3App/src/rtems_netsetup.c`: `--wrap=rtems_dhcpcd_start`; static IP
  (default, with link-up wait and early NTP sync) or libbsd's DHCP client,
  chosen by `--net=` in `cmdline.txt` (section 10).
- PVAccess/QSRV linked automatically once Base provides it
  (`ifdef EPICS_QSRV_MAJOR_VERSION` block in `rpi3App/src/Makefile`).
- `RPI_IOC_DEBUG=YES` in `rpi3App/src/Makefile` brings back the hang-hunt
  instrumentation (section 11).

Rebuild rules learned the hard way:
- EPICS make does not track `libbsd.a` or `librtemsbsp.a`. After rebuilding
  libbsd or the kernel, delete the binaries before `make`:
  `rm -f bin/RTEMS-raspberrypi2/rpi3* rpi3App/src/O.RTEMS-raspberrypi2/rpi3*`.
- EPICS make does not regenerate `dbd/rpi3.dbd` when an included `.dbd`
  changes: also `rm -f dbd/rpi3.dbd rpi3App/src/O.Common/rpi3.dbd
  rpi3App/src/O.*/rpi3_registerRecordDeviceDriver.*`.

## 9. The interrupt dispatcher bug (why the kernel patch is required)

`bsps/arm/raspberrypi/irq/irq.c`, `bsp_interrupt_dispatch()`, reads the
interrupt controller's basic pending register and, when any "speed-up" bit
is set, calls `ffs()` on the **unmasked** value. Bit 8 of that register
("one or more bits pending in register 1") is set whenever a non-speed-up
GPU interrupt is pending, for example the system-timer clock tick (GPU
IRQ 3). When the tick and the USB interrupt (GPU IRQ 9, speed-up bit 11)
are pending together the register reads 0x900, `ffs()` returns bit 8,
the speed-up table entry for bit 8 is -1, no handler is called, the
interrupt stays asserted and the CPU re-enters the dispatcher forever.
From outside the board looks dead: no console, no tasks, no LED.

The probability per USB interrupt is small, so the freeze came after
about 30 s under network load and after minutes when idle. The fix masks
the value before the bit scan:

```c
pend_bit = ffs(pend & BCM2835_IRQ_BASIC_SPEEDUP_USED_BITS) - 1;
```

Verified: 15.5 hours of continuous operation with 450 million USB
transactions after the fix, versus a freeze within a minute before.
Upstream RTEMS `main` still carries the bug as of October 2025; the patch
file is in upstream-ready form.

## 10. SD card and console

FAT32 partition with the Raspberry Pi firmware files from
`https://github.com/raspberrypi/firmware/tree/master/boot` (the set in
use, with `config.txt` and `cmdline.txt`, is kept in `SD_card/` at the top
of this repository; the two symlinks there point at the build outputs):

```
bootcode.bin                 first-stage loader (Pi 3 has no boot EEPROM)
start.elf  fixup.dat         GPU firmware (not the *4* variants)
bcm2710-rpi-3-b-plus.dtb     device tree for the 3B+ (bcm2710-rpi-3-b.dtb for a 3B)
config.txt
cmdline.txt
kernel7.img                  the IOC: bin/RTEMS-raspberrypi2/rpi3.img (files compiled in,
                             archive optional) or rpi3_plain.img (needs the archive)
epics.tar.gz                 st.cmd, db, dbd (bin/RTEMS-raspberrypi2/epics.tar.gz, section 8a)
```

`config.txt`:

```
kernel=kernel7.img
enable_uart=1
core_freq=250
initramfs epics.tar.gz 0x30000000
```

The last line is only needed with the separate `epics.tar.gz`; note the
firmware syntax without `=`. Without the line or the file the IOC uses the
copy compiled into `kernel7.img`.

`cmdline.txt` (the BSP picks its console from `--console=`; the IOC reads
an optional `--net=` from the same line through `rpi_cmdline_get_arg()`):

```
--console=/dev/ttyS0 console=serial0,115200 console=tty1
```

Network mode, no rebuild needed, one line, no comments allowed in the file:

| `cmdline.txt` contains | Effect |
|---|---|
| no `--net=` or `--net=static` | the static address, gateway and NTP server compiled into `rpi3App/src/rtems_netsetup.c` (192.168.2.21/24, gw 192.168.2.10) |
| `--net=192.168.2.31/24,gw=192.168.2.10,ntp=172.233.177.198,host=rtems-pi3b` | static with these values; `mask=255.255.255.0` works instead of `/24`; fields left out keep the compiled-in defaults |
| `--net=dhcp` | libbsd `dhcpcd`: address, route, hostname (option 12) and NTP server (option 42) from the lease; `--net=dhcp,ntp=172.233.177.198` gives the NTP server when the DHCP server sends no option 42 |
| `...,mac=b8:27:eb:12:34:56` / `...,mac=random` | interface MAC in any mode. Default: the firmware's board MAC (device tree `local-mac-address`, else `b8:27:eb` + low 24 bits of the board serial), set with `ifconfig ue0 ether` before the interface is configured, so the IOC and the ROM's network boot show the same MAC; `random` keeps libbsd's random address |

With DHCP the DHCP server must offer an NTP server (dnsmasq:
`dhcp-option=option:ntp-server,172.233.177.198`, for the laptop's shared
profile in `/etc/NetworkManager/dnsmasq-shared.d/`), otherwise the clock
is never set. Without a lease the IOC waits up to 10 minutes and then
starts without network (no static fallback). The boot log shows
`***** cmdline.txt: --net=... *****` followed by either `***** DHCP via
libbsd dhcpcd ... *****`, `dhcpcd ---> 'reason=BOUND'`, `dhcpd BOUND`, or
`***** Static IP override: ... *****`.

Serial console: 115200 8N1 on GPIO 14 (TXD) and GPIO 15 (RXD), 3.3 V.
Expected boot sequence: firmware messages, `RTEMS RPi`, `dwcotg0`,
`DWC_OTG init done`, `usbus1: 480Mbps High Speed`, `WDOG: hardware
watchdog armed (8 s)`, `muge0: Chip ID 0x7800`, `ue0: link state changed
to UP`, `Early NTP sync with ... OK`, `iocRun: All initialization
complete`, then the `rtems-pi3>` prompt. Telnet to port 23 also works.

Telnet (2026-10-22): two bugs made the telnet iocsh usable only once per
boot. (1) RTEMS telnetd's pty driver signals a dropped client connection
by setting the termios line-buffer index past its count; nothing reset
it, so the next session on that pty (always the same one, the slot goes
back to the head of the free list) read end-of-file immediately and was
closed. Fixed in `patches/rtems-telnetd-pty-eof-fix.patch` (reset in
`rtems_pty_set_socket()`), part of the kernel build above. (2) EPICS
base's session handler `telnet_pseudoIocsh()` saved descriptors 1 and 2
with `dup2(1,1)`, a no-op, so after the first session they stayed on the
first pty and all later `printf` output from other tasks went to a closed
socket (the serial console went quiet). Fixed in the base patch (`dup()`
and `close()` of the saved descriptors). (3) The session task opens
`stdin`/`stdout`/`stderr` on the pty once and reuses the FILEs for every
connection; newlib keeps a sticky end-of-file flag on the FILE after
`fgets()` returned NULL, so the next session read EOF immediately even
with (1) fixed. The handler now calls `clearerr()` on the three streams
at the start of each session (also in the base patch). Leaving a session
with `bye` or by closing the client are now equivalent.

The IOC arms the BCM2835 hardware watchdog (8 s); `var hb_wdog_enable 0`
in iocsh disarms it before stopping the IOC in a debugger.

## 10b. Network boot: no SD card, or a card with only `bootcode.bin`

Prepared 2026-10-16, **verified 2026-10-20 with no SD card at all**
(capture `raspberyPI3B+_20_10_2026_8.cap`, laptop journal): power-on
reset, ROM DHCP 1.0 s after link-up, `bootcode.bin`, `start.elf`,
`fixup.dat`, `config.txt`, dtb, `cmdline.txt`, `kernel7.img` and
`epics.tar.gz` all fetched from `/srv/tftp/rpi/9645d7b0/`, RTEMS up 17 s
after link-up, lease .21 with the firmware MAC, NTP, archive crc intact,
`epicsLoadModule`, `iocInit`: identical to the card boot of the same
morning. The 3B+ can fetch everything it otherwise reads from the card
over DHCP + TFTP. Two stages:

1. The boot ROM (3B+: network boot enabled in OTP from the factory) does
   DHCP with vendor class `PXEClient:Arch:00000:UNDI:002001`, accepts only
   an offer whose option 43 contains `Raspberry Pi Boot`, and fetches
   `bootcode.bin` by TFTP from the offer's next-server. With a card that
   holds only `bootcode.bin` this stage reads the file from the card
   instead (more robust: newer code, longer retries, UART messages).
2. `bootcode.bin` repeats the DHCP exchange and fetches `start.elf`,
   `fixup.dat`, `config.txt`, the dtb, `cmdline.txt`, `kernel7.img` and
   the `initramfs` file `epics.tar.gz`, trying `<serial>/<file>` before
   `<file>` (serial = last 8 hex digits of the board serial, printed by the
   IOC in its `Board serial` line; this board: `9645d7b0`).

Both stages use the firmware MAC `b8:27:eb` + low 24 bits of the serial,
the same one the IOC sets on `ue0` (section 10), so one DHCP reservation
covers the whole boot.

Server side (the laptop's shared profile, `patches/NETWORK_SERVERS_SETUP.md`),
in `/etc/NetworkManager/dnsmasq-shared.d/rpi-ioc.conf`
(`patches/dnsmasq-shared-rpi-ioc.conf`):

```
enable-tftp
tftp-root=/srv/tftp/rpi
pxe-service=0,"Raspberry Pi Boot"
```

TFTP directory: `make tftp` (= `scripts/update_tftp.sh`, default root
`/srv/tftp/rpi`, created once with `sudo mkdir -p` and `chown` to the
user) copies the `SD_card/` set with the symlinks resolved, puts the DHCP
`cmdline.txt` in place (a network-booted IOC has no reason to use a static
address), makes everything world-readable (dnsmasq serves as `nobody`) and
creates `<serial>/` with symlinks to the shared files, so a per-board
override is one file. Verified 2026-10-16 with a local dnsmasq instance:
all files served byte-identical. Run `make tftp` after every `make` or
`make ramfs` meant for a network-booted board.

What the laptop sees (`sudo journalctl -f -t dnsmasq-dhcp -t dnsmasq-tftp
-t NetworkManager`): link up -> NetworkManager restarts dnsmasq 0.2 s later
-> ROM `DHCPDISCOVER` with vendor class `PXEClient:Arch:00000:UNDI:002001`
0.8 s after that -> `bootcode.bin` -> second DHCP exchange -> the firmware
files. "Early terminate received" followed by "failed sending" is the
firmware probing a file's size and is normal; "not found" for
`bootsig.bin`, `autoboot.txt`, `recovery.elf`, `dt-blob.bin`, `bootcfg.txt`,
`overlays/overlay_map.dtb`, `armstub8-32.bin` is normal too (optional
files). When RTEMS takes over it resets the LAN7515, the link drops for a
moment, NetworkManager tears the shared profile down and brings it back
with a NEW dnsmasq, which then answers the IOC's own dhcpcd 0.5 s later;
so both boot stages live on that restart being fast. It was, twice. If a
future laptop is slower, `ignore-carrier=enp1s0` in
`/etc/NetworkManager/NetworkManager.conf` keeps the profile and dnsmasq
up permanently. The intermediate variant (card with only `bootcode.bin`)
was not needed and was skipped; it would only help on a board whose ROM
network boot misbehaves. Operational note: a cardless IOC boots the files
of `/srv/tftp/rpi` at that moment, so `make tftp` after every build is
the equivalent of the card copy, and the laptop must be up before the Pi.

## 10e. NFS mounts with options, NFSv4 (`rtemsNfsMount`, branch `nfsv4`)

Written 2026-10-17, **verified on the board 2026-10-20** (capture
`raspberyPI3B+_20_10_2026_6.cap`, kernel delivered with `rpiKexec`):
`rtemsNfsMount "192.168.2.10:/srv/nfs/ioc", "/mnt/ioc"` mounted NFSv4 over
TCP against the laptop's `nfs-kernel-server` (export
`/srv/nfs/ioc 192.168.2.0/24(rw,sync,no_subtree_check,insecure,no_root_squash,sec=sys)`),
`rt ls`, `rt cat`, `rt cp` into a subdirectory, `rt mkdir` in the mount
root and `rt cp` into the new directory all worked, the files arrived on
the server owned by root (no_root_squash, numeric ids). Two limitations of
the RTEMS libbsd glue were found, see section 12; neither is in our code.
libbsd's default
build set includes FreeBSD's "new NFS" client (v3 and v4, `fs_nfsclient`),
and the RTEMS mount glue `rtemsbsd/fs/nfsclient/nfs.c` (a port of
`mount_nfs`) parses an option string. EPICS Base's `nfsMount(uidhost,
path, mountpoint)` passes none, so it always mounts v3. The iocsh command
in `rpi3App/src/nfs_mount.c` exposes the options:

```
rtemsNfsMount "192.168.2.10:/export/ioc", "/mnt/ioc"              # default options nfsv4,tcp
rtemsNfsMount "192.168.2.10:/export/ioc", "/mnt/ioc", "nfsv3,udp"
rtemsUmount   "/mnt/ioc"
```

Options the glue understands: `nfsv2`, `nfsv3`, `nfsv4` (port 2049
directly, no mount protocol and no portmapper, which also suits
firewalls), `tcp`, `udp`, `port=N`, `noinet4`, `noinet6`; anything else is
accepted and ignored; the string must be shorter than 64 characters; a
`user@` prefix in the device is rejected. The mount point is created if
missing. After mounting, the command reads the directory once so that an
unreachable server fails right there.

NFSv4 specifics of this build: no `nfsuserd`, so owner strings are mapped
only when the server sends numeric ids (Linux servers do so with
`nfs4_disable_idmapping=1`, the default for `sec=sys`), otherwise files
appear as "nobody", which does not matter for reading `st.cmd` and
databases; no callback daemon, so no delegations; no Kerberos, `sec=sys`
only.

Known limitations (libbsd glue, verified 2026-10-20):

- **No file creation directly in the mount root.** `rt cp x /mnt/ioc/new`
  fails with "Not owner" (EPERM) without any request reaching the server.
  Creating the file in any subdirectory works, and `mkdir` in the root
  works. Workaround: keep writable data in a subdirectory of the export.
- **One mount per server per boot.** A second `rtemsNfsMount` of the same
  server after `rtemsUmount` fails with "No such file or directory"; the
  first mount of a boot always works. Mount once in `st.cmd` and leave it.

Still to try: leave the mount for some minutes (v4 renews its lease in
the background) and restart the server once to see the client reconnect.
## 10d. EPICS support modules loaded at run time (libdl): module-agnostic kernel, three files per module

History: run-time loading was written on the branch `libdl` on 2026-10-17
(one `ld -r` object per module, a per-module list of symbols to keep in
the kernel), verified on the board 2026-10-20 10:00 and merged. The same
afternoon it was redesigned on Mirek's two requirements: the kernel must
not know which modules will be loaded, and a module must consist of its
own unchanged build products plus one generated file. Both are
implemented and board-verified (captures `raspberyPI3B+_20_10_2026_12`
to `_15.cap`). Nothing below the IOC changed: the RTEMS kernel, the BSP,
libbsd and EPICS Base trees and their installed libraries are untouched
(all dated 2026-10-14); everything here lives in this repository.

### What a module is now

| File in `epics.tar.gz` | Content |
|---|---|
| `lib/RTEMS-raspberrypi2/libNAME.a` | the module's library as its own build produced it (`LIB_INSTALLS` for a module tree outside this repository, the normal `LIBRARY_IOC` install for `RPi_IOSup`) |
| `dbd/NAME.dbd` | the module's dbd as is (`DBD_INSTALLS`); it is NOT expanded: `dbdExpand` would need Base's record types and the loader needs none of that |
| `bin/RTEMS-raspberrypi2/NAME_registerRecordDeviceDriver.o` | generated by Base's `registerRecordDeviceDriver.pl` from the dbd, exactly as for a linked IOC, compiled with `-fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables` (per-file `<name>_CXXFLAGS`) because the loader data-aborts on `R_ARM_PREL31` relocations of `.ARM.exidx` |
| `etc/libdl.conf` (one for all) | `/lib/RTEMS-raspberrypi2/lib*.a`: the archives the loader searches for unresolved symbols; read at loader start, re-read when changed; absolute glob, the directory is scanned |

`epicsLoadModule "NAME" [, verbose] [, "object"] [, "dbd"]`
(`rpi3App/src/rtl_load.c`): `dbLoadDatabase(dbd/NAME.dbd)`; `dlopen()`
of the registration object; the loader (`cpukit/libdl/rtl-unresolved.c`,
`rtl-archive.c`) looks every unresolved symbol up in the archives' ranlib
indexes and loads the defining members, repeating until nothing is pending,
so exactly the members a static link would take come in; then
`dlinfo(handle, RTLD_DI_UNRESOLVED)` must report 0 (dlopen succeeds even
with holes, and a hole would be a crash on first use); then
`NAME_registerRecordDeviceDriver(pdbbase)`. Silent on success, `verbose`
= 1 lists the three files and the result. `rtemsLoadReport` lists the
loaded objects including the pulled members; `rtemsLoadTrace mask` sets the
loader's trace (0x20 symbols, 0x40 relocations, 0x100 section placement),
which is what found the bug below. Modules load before `iocInit` only.

### The kernel side: every API symbol present, no module knowledge

`dlopen()` resolves a module's references against the table that
`rtems-syms -e` generates from the linked kernel (two-pass link: PROD
`rpi3_prelink` -> `rpi3_syms.c` -> compiled into `rpi3` and
`rpi3_plain`). Only what is in the kernel can be in the table, and
`--gc-sections` normally leaves out everything the IOC itself does not
call (`rtems_task_is_suspended`, `getppid`, `scanIoRequest` once the GPIO
driver left the kernel ...). The old answer was a per-module keep list;
the new one is generic:

- `scripts/libdl_api_symbols.py` (called from `rpi3App/src/Makefile`,
  output `libdl_api.rsp`, passed as `@libdl_api.rsp` in `rpi3_LDFLAGS`)
  writes one `-Wl,-u,<symbol>` per strong public symbol of librtemscpu,
  librtemsbsp, libc, libm and Base's libdbRecStd, libdbCore, libca, libCom
  (about 6400 roots from about 2530 members). With `--gc-sections` still
  on, each root keeps its function and callees; nothing unreachable stays.
- Members are NOT forced when (a) they define a symbol the application's
  own objects define (RTEMS confdefs defaults such as `_Workspace_Is_unified`
  live as strong members of librtemscpu and are meant to be overridden by
  `rtems_config.o`), (b) any symbol they define, weak or strong, is also
  defined by another member of the forced or pool libraries (SHA-512 in
  librtemscpu and libbsd; `bsp_interrupt_get_dispatch_table_slot`, weak
  default in `irq-generic.c`, strong tracing variant in `irq-record.c`), or
  (c) a symbol they reference has no viable definer anywhere, transitively
  (flashdisk, nvdisk, ramdisk, trace recorder, CBS scheduler: their
  configuration tables exist only in applications that use them). "Anywhere"
  includes the pool: libbsd and the other on-demand libraries, libgcc,
  libstdc++/libsupc++, the crt start files and the BSP linker scripts
  (`_TLS_Data_size`, `_init`), otherwise `malloc`, `printf` and `dbScanLock`
  cascade into "doomed".
- Two things that do NOT work and were tried first: `--whole-archive` of
  librtemscpu (the confdefs defaults collide), and `--no-gc-sections`
  (libbsd's VFS and syscall layer reference 142 FreeBSD kernel symbols the
  RTEMS port lacks; only garbage collection keeps those dead functions out;
  `--gc-sections` also arrives via the BSP spec file, not only via Base).
- `scripts/libdl_syms_filter.py` (table post-processing) now drops only
  `_bsd_*`, `rtems__rtl_*` and `__TLS_*` names and renumbers the TLS index
  list (see the bug below). The "multiply defined across the library
  directories" rule is off: the first link holds one definition of every
  name and the second link resolves the table against it (0 duplicates).
- Result: `rpi3_plain.img` 9.7 MB (was 7.6 MB), table 15 814 names (was
  11 748), `LIBDL_KEEP_SYMBOLS` empty. `scripts/libdl_check_module.py`
  (run after the table) checks every registration object and library in
  the archive; symbols defined by the given files satisfy each other.

### The three bugs found on the way (all in our own tooling, none in the BSP)

1. **Weak-default override by a forced member** (first boot, 2026-10-20
   15:48): the generator counted only strong definitions, so
   `irq-record.c` was forced in and its strong
   `bsp_interrupt_get_dispatch_table_slot` replaced the weak default of
   `irq-generic.c`: handlers went into one table, the dispatcher read
   another, "spurious interrupt: 3" (clock tick) from the first tick on.
   Fix: rule (b) above counts weak definitions too (38 more members
   excluded).
2. **TLS index shift in the symbol-table filter** (latent since
   2026-10-17, hit 15:54): the table ends with
   `rtems_rtl_tls_offsets[] = {{RTEMS_TLS_INDEX__tls_stderr, ...}, ...}`,
   indexes into the entry list; `rtems_rtl_base_sym_global_add` walks the
   list with a counter and stores the TLS OFFSET instead of the address
   when the counter matches. The filter removed entries without
   renumbering, so 32 arbitrary symbols received small TLS offsets:
   `rtems_libio_number_iops` became 0x814, `rtems_libio_iops` 0x840, and
   iocStats's FD-usage init dereferenced 0x814 during `iocInit` ("fatal
   source 9, VEC 4, R3 = 0x00000814"). The R03.00 kernel has the same
   defect with other victims that no module touched. Found with the loader
   trace: the relocation lines showed `G:rtems_libio_number_iops=00000814`
   while the kernel image's own table bytes held the right address. Fix: the
   filter renumbers `RTEMS_TLS_INDEX_*` from the kept list.
3. **A module dbd cannot be expanded** (build time): `DBD += x.dbd` with an
   `x_DBD` list runs `dbdExpand`, which rejects `device()` lines whose
   record types it has not seen; the module dbd is installed verbatim with
   `DBD_INSTALLS` and the registration rule points at the module tree (a
   `vpath` for the dbd source fails on EPICS's `.dbd.d` dependency rule).

Earlier (2026-10-17/20, kept): the loader data-aborts on `R_ARM_PREL31`
relocations of a module's `.ARM.exidx` (RTEMS main has "Preserve bit 31
of a PREL31 place", 32595811b3, not in 6.2), hence the no-unwind flags
for the generated C++ registration file; dbd/SRCS lines for RTEMS-only
sources must sit outside `ifeq ($(OS_CLASS),RTEMS)` (the dbd is assembled
in the host pass); `-DUSE_TYPED_DRVET` belongs in the driver source, not in
`USR_CPPFLAGS`, because the generated registration code uses the classic
`struct drvet`.

### Files involved

`rpi3App/src/Makefile` (API roots block, `LIBDL_KEEP_SYMBOLS` empty,
devIocStats block: `DBD_INSTALLS`, `LIB_INSTALLS`, `BIN_INSTALLS`, RAMFS
lists, registration rule, skipped when the module tree is absent),
`rpi3App/src/ramfs.mk` (`RAMFS_MODULE_LIBS`, `etc/libdl.conf`),
`rpi3App/src/rtl_load.c` (`epicsLoadModule` three-file logic, verbose
flag, unresolved check, `rtemsLoadTrace`), `RPi_IOSup/src/Makefile`
(registration object only, no `ld -r`), `scripts/libdl_api_symbols.py`
(new), `scripts/libdl_syms_filter.py` (TLS renumbering, duplicate rule
off), `scripts/libdl_check_module.py` (archives), `configure/RELEASE`
(`IOCSTATS`), `iocBoot/iocrpi3/st.cmd` (`epicsLoadModule "devIocStats"`,
`iocAdminSoft.db`), `rpi3App/Db/Makefile` (`DB_INSTALLS`),
`etc/libdl.conf` (new). iocStats itself: Mirek's `iocStatsALS` tree at
`/usr/local/epics/R7.0.10/modules/soft/iocStatsALS`, built for
`RTEMS-raspberrypi2` on 2026-10-18 (its `README_RTEMS6_fixes.md` lists the
RTEMS 6 source fixes); one harmless warning at `iocInit`, the `TIMEZONE`
stringin's INP form is rejected by this Base.

## 10c. Warm restart into a kernel fetched over TFTP (`rpiKexec`, branch `kexec`)

Written 2026-10-17, **verified on the board 2026-10-20**: check-only run
(7.6 MB in 2 s, crc32 equal to the host file), then `boot=1`: the RTEMS
banner followed the `booting` line directly, no firmware stage, and the
warm-booted IOC came up identical to a cold boot (DHCP lease, NTP,
archive, `iocRun`; capture `raspberyPI3B+_20_10_2026_3.cap`). Two bugs
found by the first attempts and fixed: the TFTP mount needs an empty
device string, not NULL (`rtems_tftpfs_initialize()` does `strlen()` on
it; the fatal exception reset the board to the card kernel, as designed),
and a second call must accept the already mounted `/TFTP` (EEXIST).
The firmware always boots
from a card that holds a kernel, and RTEMS has no SD card driver here, so
a new kernel normally needs a hand on the card. The iocsh command
`rpiKexec` in `rpi3App/src/kexec.c` does the firmware's job from the
running IOC instead:

```
rpiKexec "192.168.2.10", "kernel7.img"        # fetch over TFTP, check, print crc32
rpiKexec "192.168.2.10", "kernel7.img", 1     # fetch, check and boot the image
rpiKexec "192.168.2.10", "kernel7.img", 2     # boot even if the checks fail
```

1. The image is read through RTEMS's TFTP client file system
   (`/TFTP/<host>:<file>`, mounted on demand) into a RAM buffer; the
   laptop's dnsmasq serves `/srv/tftp/rpi`, filled by `make tftp`.
2. Checks: 1 to 32 MB, first word is an ARM vector entry (`e59ff018`),
   first 32 bytes identical to the running kernel at 0x200000. The crc32
   is printed; `scripts/crc32.py <file>` gives the host value to compare.
3. Boot: the firmware device tree is copied out of the kernel's `.bss`
   (the new kernel copies it again in `start.S`), the hardware watchdog is
   re-armed with 15 s, interrupts are disabled, the interrupt controller,
   the ARM timer and the USB core (soft reset, stops DMA) are quiesced,
   the data caches are cleaned to RAM, and a 112-byte position-independent
   ARM stub copied into the heap turns MMU and caches off, copies the image
   to 0x200000 over the running kernel, invalidates the instruction cache
   and jumps to 0x200080 with r0 = 0, r1 = 0xc42, r2 = device tree, as the
   firmware does. `epics.tar.gz` at 0x30000000 stays in place.

Safety: if the new image does not reach `heartbeat_start()` within 15 s,
the watchdog resets the board and the firmware boots the card's kernel.
The card is never written. Limits: RAM only until the next cold boot;
same load address and entry point as this kernel.

First use needs one on-site card update with a kernel that contains the
command; afterwards kernels can be swapped remotely. Test order: check-only
run (crc32 must match the host), then boot=1 with a serial log running.

## 11. Debugging support

All instrumentation written during the hang hunt is kept behind one
compile-time switch, `RPI_IOC_DEBUG`, in three places:

1. libbsd `dwc_otg.c`: apply `rtems-libbsd-raspberrypi-debug-instrumentation.patch`
   on top of the required patch and uncomment `#define RPI_IOC_DEBUG 1`
   near the top of the RTEMS block. Gives a RAM crash ring at physical
   0x28000000 that survives a watchdog reset and is printed by the next
   boot, a register read-back probe, a UART tracer, per-channel counters
   and a periodic status dump.
2. kernel `irq.c`: apply `rtems-raspberrypi-irq-dispatch-debug-markers.patch`
   on top of the fix and uncomment the same define. Logs every dispatcher
   access into the ring.
3. `rpi3App/src/Makefile`: set `RPI_IOC_DEBUG = YES`. Links the interrupt
   wrapper, the FIQ LED on GPIO 16, the per-second heartbeat line and the
   iocsh debug variables.

Rebuild libbsd, kernel, then the IOC (with the rm commands of section 8).
The method, "watchdog plus reset-surviving RAM ring with markers in every
code path that touches the bus, including the kernel dispatcher", is what
finally located the bug after months of guessing at hardware causes.

## 12. Things that cost time and are worth knowing

- Two upstream bugs met while adding DHCP (2026-10-16), both worked
  around in `rpi3App/src/rtems_netsetup.c`: EPICS Base
  `modules/libcom/RTEMS/posix/rtems_init.c` never writes
  `/etc/dhcpcd.conf` (`ENOENT == stat(...)` cannot be true; and its option
  names use hyphens where dhcpcd 6 wants underscores), so dhcpcd asks for
  neither option 42 nor option 12; and the RTEMS 6 BSP
  `bsps/arm/raspberrypi/start/vc.c` flushes `sizeof(&buffer)` = 4 bytes
  instead of `sizeof(buffer)` in three mailbox calls (board serial among
  them, also on `main`), so `bcm2835_mailbox_get_board_serial()` returns
  0. The IOC reads the serial from the device tree root property
  `serial-number` instead.
- Third one (2026-10-17): Base's dhcpcd hook reacts only to `BOUND`; a
  lease that replaces an IPv4LL address comes as `REBIND` and is ignored
  (10-minute wait). `rpi3App/src/rtems_netsetup.c` registers a second
  hook (`netsetup_dhcpcd_hook`) for `REBIND`/`RENEW`/`REBOOT` and writes
  `noipv4ll` into dhcpcd.conf.

- Two RTEMS libbsd NFS glue bugs (2026-10-20, found with the `nfsv4`
  branch, packet capture plus source reading, both reproducible with
  NFSv3 too since they are in the RTEMS/libbsd layer, not in the NFS
  client): (a) `rtemsbsd/rtems/rtems-bsd-syscall-api.c`
  `rtems_bsd_sysgen_open_node()` compares the evaluated vnode with the
  mount root to detect an open of "."; for a file that does not exist
  yet the path evaluation (`rtems-kernel-vfs.c`, "Leave the currentloc
  where it is") hands over the parent directory, so creating a file
  directly in the mount root is mistaken for a write-open of "." and
  refused with EPERM before any OPEN/CREATE is sent. A one-line fix would
  skip that shortcut when `O_CREAT` is set. (b) `rtemsbsd/fs/nfsclient/nfs.c`
  mounts every file system under an internal name `/<host>-<counter>` in
  the libbsd root; a second mount of the same server in one boot
  LOOKUPs that internal name on the server and fails with ENOENT. Both
  are documented as limitations in section 10e; not yet reported upstream.
- Isolation switches left in the source trees from an earlier experiment
  (a `return ENXIO` in `muge_probe()`) masqueraded as a driver bug for a
  whole session. Grep the libbsd and kernel trees for `TEST`, `RPI3_`,
  `#if 0` before debugging anything that "used to work".
- A CPU spinning in an exception loop looks exactly like a dead bus from
  the outside; a blinking-LED liveness test cannot tell them apart.
- The `muge` driver polls the LAN7800 PHY with about 8000 USB control
  transfers per second. Harmless, but it is the dominant USB load and it
  is why the hang used to come within 30 s once the network was up.
- `osdTime.cpp` from the EPICS patch does not start `rtems_ntpd` on the
  Pi; it synchronises with `epicsNtpGetTime()` against `EPICS_TS_NTP_INET`.
  The IOC needs a route to that server (see `NETWORK_SERVERS_SETUP.md`).
