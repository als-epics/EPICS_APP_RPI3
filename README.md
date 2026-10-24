# EPICS_APP_RPI3: EPICS 7 IOC on RTEMS 6, Raspberry Pi 3B+

EPICS 7.0.10 IOC running on RTEMS 6.2 (`arm/raspberrypi2` BSP) on a
Raspberry Pi 3 Model B+, with USB Ethernet (LAN7515/LAN7800), PVAccess and
Channel Access, NTP time, a hardware watchdog, 1 kHz periodic scans and an
interrupt-capable GPIO driver. Verified 2026-10-14 after a 15.5 h soak
run and again 2026-10-15 after a second overnight run at 1 kHz scan rate
(17 h 47 min, no reboot, 1000 edges/s constant); the six-month "USB hang" turned out to be a one-line bug in the RTEMS
BSP interrupt dispatcher (fixed, patch included).

Author: Miroslaw Dach (mdach@lbl.com). Claude Code assisted in
creating this project.

## Where to start

Documents, patches and the kernel build configuration live in `patches/`;
the SD card contents (firmware files, `config.txt`, `cmdline.txt`) in
`SD_card/`; the scripts that rebuild the tool stack in `scripts/`.

| Document | Content |
|---|---|
| `patches/RTEMS6_RASPBERRYPI_BUILD.md` | **Start here.** Toolchain, kernel, rtems-libbsd, net-services, libtecla, EPICS base and this IOC: versions, commands, SD card, console, `cmdline.txt` network switch (section 10), network boot without SD card (10b), the dispatcher bug, the debug switch, the separate `epics.tar.gz` for st.cmd/db/dbd (section 8a). |
| `patches/EPICS_BASE_RASPBERRYPI_PATCH.md` | The EPICS base patch in detail, adding the PVAccess submodules, the NTP-fraction fix. |
| `RPi_IOSup/README.md` | GPIO driver: `rpiGpioConfigOutput/Input`, records, interrupts, measured results; loaded at run time on the `libdl` branch (section "EPICS support modules" below). |
| `patches/NETWORK_SERVERS_SETUP.md` | Install, configure and start the DHCP, TFTP and NFS servers; which files go on the card and in the TFTP root. |

## Building RTEMS and EPICS on a new machine (`scripts/`)

Two scripts download, patch and build everything this IOC needs, with the
same upstream commits and patches as the installation that was verified on
the Pi. Run them in this order on Ubuntu 22.04/24.04:

```bash
scripts/build_rtems_rpi3.sh     # 1. cross tools, RTEMS kernel, rtems-libbsd, rtems-net-services, libtecla
scripts/build_epics_rpi3.sh     # 2. EPICS base 7.0.10 with the Raspberry Pi patch and PVAccess
make                            # 3. this IOC
```

| Script | Asks for | Does | Time |
|---|---|---|---|
| `scripts/build_rtems_rpi3.sh` | sources directory (default `~/RTEMS_SRC`), install directory (default `~/RTEMS_PI`; the RTEMS prefix becomes `<dir>/rtems-6/6`) | RTEMS Source Builder tools `arm-rtems6-*`, kernel (BSP `arm/raspberrypi2`, `patches/rtems-raspberrypi-irq-dispatch-fix.patch`, `patches/rtems-kernel-raspberrypi2-config.ini`), rtems-libbsd (`patches/rtems-libbsd-raspberrypi.patch`), rtems-net-services, libtecla from `patches/rtems-addon-packages-libtecla-1.6.3.tar.gz` | 45 to 90 min |
| `scripts/build_epics_rpi3.sh` | EPICS directory (default `/usr/local/epics/R7.0.10/base`), the RTEMS install directory of the first script | clone of EPICS 7.0 at commit 5b23dcb46, the six PVAccess submodules, `patches/epics-base-R7.0.10-raspberrypi.patch`, `RTEMS_BASE` and `CROSS_COMPILER_TARGET_ARCHS = RTEMS-raspberrypi2` in the Base configuration, `make`; offers to point `configure/RELEASE` of this IOC at the new Base | 15 to 30 min |

Both scripts accept `-y` (take the defaults, or `RTEMS_SRC_DIR`,
`RTEMS_INSTALL_DIR`, `EPICS_DIR` from the environment), `--prepare-only`
(clone and patch, no build) and `-h`; `JOBS=n` limits the parallel jobs.
They can be re-run: trees with local changes are left alone, patches
already applied are detected, and a step whose library is installed is
skipped (`FORCE=1` rebuilds it). Details and the reasons behind the pinned
commits and the vendored libtecla are in `scripts/README.md`.

## Patches for the external trees

| File | Applies to | Status |
|---|---|---|
| `patches/rtems-raspberrypi-irq-dispatch-fix.patch` | RTEMS kernel 6.2 | required; upstream: work item [#5810](https://gitlab.rtems.org/rtems/rtos/rtems/-/work_items/5810), merge request [!1542](https://gitlab.rtems.org/rtems/rtos/rtems/-/merge_requests/1542) against `main` (opened 2026-10-18, pending) |
| `patches/rtems-main-raspberrypi-irq-dispatch-fix.git.patch` | RTEMS main, `git am` format with the commit message for the GitLab merge request (submitted upstream as RTEMS merge request !1542, work item #5810) | upstreaming |
| `patches/rtems-kernel-raspberrypi2-config.ini` | RTEMS kernel build | the configuration used |
| `patches/rtems-libbsd-raspberrypi.patch` | rtems-libbsd 6-freebsd-14 | required |
| `patches/epics-base-R7.0.10-raspberrypi.patch` | EPICS base 7.0 @ 5b23dcb46 | required |
| `patches/epics-base-osdTime-ntp-fraction.patch` | EPICS base already patched before 2026-10-14 | incremental |
| `patches/rtems-raspberrypi-irq-dispatch-debug-markers.patch` | RTEMS kernel, on top of the fix | optional, debugging |
| `patches/rtems-libbsd-raspberrypi-debug-instrumentation.patch` | rtems-libbsd, on top of the required patch | optional, debugging |
| `patches/rtems-addon-packages-libtecla-1.6.3.tar.gz` | libtecla 1.6.3 + RTEMS makefiles from rtems-addon-packages 4f5951a (git.rtems.org is offline) | required for iocsh line editing |

## Layout

```
configure/            EPICS build configuration (CONFIG_SITE.local: 500 us tick)
patches/              documents, patches for kernel/libbsd/EPICS base, kernel config.ini, libtecla sources
scripts/              build_rtems_rpi3.sh, build_epics_rpi3.sh (rebuild the stack on a new machine), update_tftp.sh (network boot)
SD_card/              what goes on the FAT32 card: firmware files, config.txt, cmdline.txt,
                      and symlinks kernel7.img -> bin/.../rpi3_plain.img, epics.tar.gz -> bin/...
rpi3App/src/         IOC: rtems_netsetup.c (static IP or DHCP, --net= in cmdline.txt), rtems_config.c, heartbeat.c (watchdog),
                      menuScan.dbd (.01/.001 second), debug-only sources behind RPI_IOC_DEBUG
rpi3App/Db/          databases
RPi_IOSup/            GPIO driver module (library RPi_IO, dbd RPi_IO.dbd, Db/, README.md)
iocBoot/iocrpi3/     st.cmd (compiled into rpi3.img and packed into epics.tar.gz with db/ and dbd/)
bin/RTEMS-raspberrypi2/               make output; the three files below are committed with each release tag:
  rpi3.img (= kernel7.img)           IOC with st.cmd/db/dbd compiled in, uses epics.tar.gz when present
  rpi3_plain.img                     IOC without the compiled-in files, needs epics.tar.gz
  epics.tar.gz                        st.cmd, db, dbd for the SD card (config.txt: initramfs epics.tar.gz 0x30000000)
```

## Build and run, in short

```bash
make                                   # kernels + epics.tar.gz; needs the RTEMS/EPICS stack of the build guide
cp bin/RTEMS-raspberrypi2/kernel7.img bin/RTEMS-raspberrypi2/epics.tar.gz /media/$USER/boot/   # SD card, FAT32
make ramfs                             # after a st.cmd or db edit: only epics.tar.gz, copy just that file
make tftp                              # network boot: fill /srv/tftp/rpi for the laptop's dnsmasq (build guide 10b)
```

After changing `*.dbd` files or rebuilding libbsd/kernel/Base:
`rm -f dbd/rpi3.dbd rpi3App/src/O.Common/rpi3.dbd rpi3App/src/O.*/rpi3_registerRecordDeviceDriver.* bin/RTEMS-raspberrypi2/rpi3* rpi3App/src/O.RTEMS-raspberrypi2/rpi3*`
before `make` (EPICS does not track those dependencies).

Serial console 115200 8N1 on GPIO 14/15; telnet on port 23 (repeated
sessions need the two telnet fixes listed in `patches/`, see
`RTEMS6_RASPBERRYPI_BUILD.md` section 10); CA and PVA on the usual ports.

## Network address: static or DHCP, chosen in `cmdline.txt` (2026-10-15)

The IOC replaces libbsd's `rtems_dhcpcd_start()` with its own
`rpi3App/src/rtems_netsetup.c` (linker `--wrap`). It arms the hardware
watchdog, waits for the USB Ethernet interface, and then configures the
network in one of two ways, selected at boot time by an optional `--net=`
argument in `cmdline.txt` on the SD card. The firmware hands that line to
the kernel and the BSP exposes it, the same way the `--console=` option
already works, so switching needs no rebuild and no new `kernel7.img`.

| `cmdline.txt` | Behaviour |
|---|---|
| no `--net=`, or `--net=static` | the static setup compiled into `rtems_netsetup.c`: 192.168.2.21/24, gateway 192.168.2.10 (the laptop), NTP 172.233.177.198, hostname `rtems-pi3`; link-up wait and early NTP sync as before |
| `--net=192.168.2.31/24,gw=192.168.2.10,ntp=172.233.177.198,host=pi-b` | static with these values; `mask=255.255.255.0` can replace `/24`; every field left out keeps the compiled-in default |
| `--net=dhcp` | libbsd `dhcpcd`: address, route, hostname (DHCP option 12) and NTP server (option 42) from the lease; EPICS continues when the lease is BOUND (Base's hook in `rtems_init.c`), then Base runs its one-shot NTP sync |
| `--net=dhcp,ntp=172.233.177.198` | DHCP, with that NTP server when the lease carries no option 42 |
| `...,mac=b8:27:eb:12:34:56` or `...,mac=random` | MAC address of the interface in any mode; see below. Default: the board MAC of the firmware |

Example line, one line only, no comments allowed in the file:

```
--console=/dev/ttyS0 --net=dhcp,ntp=172.233.177.198 console=serial0,115200 console=tty1
```

Notes:

- A bad field (`--net=foo`, `gw=bar`, unknown key) is reported on the
  console as `cmdline.txt: --net ...` and ignored; the default for that
  field stays.
- MAC address (2026-10-16): the LAN7515 has no address EEPROM. The boot
  ROM and `start.elf` use `b8:27:eb` + the low three bytes of the board
  serial, but libbsd's `muge` driver finds nothing in the chip and makes
  up a random address at every boot. Before configuring the interface the
  IOC now sets the firmware MAC itself: from the device tree property
  `local-mac-address` (alias `ethernet0`, filled by the firmware), or
  computed from the board serial read through the mailbox. The log line
  `Board serial ..., MAC b8:27:eb:xx:yy:zz (<source>) -> ifconfig ue0
  ether: 0` shows what was used; the earlier `ue0: Ethernet address:`
  line still shows the random one from attach time. One MAC therefore
  identifies the board in the ROM's network boot and in the IOC, and a
  normal `dhcp-host=<MAC>,...` reservation serves both. `mac=random`
  restores the old behaviour, `mac=<address>` forces one.
- With DHCP the server must offer an NTP server, otherwise the clock is
  never set. For the laptop's shared profile the file
  `patches/dnsmasq-shared-rpi-ioc.conf` (installed in
  `/etc/NetworkManager/dnsmasq-shared.d/`) sends option 42 and reserves
  192.168.2.21 for the IOC by its board MAC (`dhcp-host=b8:27:eb:*:*:*,...`,
  wildcards until the exact address is known from the boot log). The same
  line serves the ROM and `bootcode.bin` during network boot. Details in
  `patches/NETWORK_SERVERS_SETUP.md`.
- Without a lease the IOC waits up to 10 minutes and then starts without
  network. There is no automatic fallback to the static address.
- A switch file inside `epics.tar.gz` would not work: Base brings the
  network up before it sets up the file system. The command line is the
  only configuration available at that point.
- The boot log shows `***** cmdline.txt: --net=... *****` followed by
  `***** Static IP override: ... *****` or `***** DHCP via libbsd dhcpcd
  ... *****`, `dhcpcd ---> 'reason=BOUND'`, `dhcpd BOUND`.
- EPICS Base bug found on the way (2026-10-16): `rtems_init.c` means to
  write `/etc/dhcpcd.conf` but tests `ENOENT == stat(...)`, which is never
  true, so dhcpcd runs with its defaults and requests neither the NTP
  server (option 42) nor the hostname (option 12); the option names it
  would write (`ntp-servers`) are also not dhcpcd 6's (`ntp_servers`).
  The IOC now writes the file itself before dhcpcd starts (`nodhcp6`,
  `ipv4only`, `option ntp_servers`, `option host_name`, no client id).
- Second Base limitation (2026-10-17, late-server test): Base's dhcpcd
  hook wakes EPICS only on the reason `BOUND`. When the server appears
  after dhcpcd has already taken a link-local 169.254 address, the lease
  arrives as `REBIND`, Base ignores it, EPICS sits in its 10-minute wait
  (`---- DHCP timed out!`) and then starts with the fallback NTP server and
  hostname. The IOC now adds its own hook that accepts `REBIND`, `RENEW`
  and `REBOOT` (NTP server and hostname from the lease, EPICS woken once)
  and writes `noipv4ll` into dhcpcd.conf so there is no link-local detour.
  Built 2026-10-17, not yet booted. Observed after the 10-minute path with
  the previous kernel, and absent after a normal boot of the same kernel:
  `RPI3:TGL:RATE` showed 999/1001 pairs every 10 s (1 ms toggle itself
  exact) and the telnet iocsh accepted connections but executed nothing.
  Cause not yet identified; to be re-checked with the fixed kernel and a
  server that stays absent for the whole boot.
- Status: DHCP mode verified on the Pi 2026-10-16 (capture
  `raspberyPI3B+_16_10_2026_1.cap`): MAC `b8:27:eb:45:d7:b0` from the
  device tree, lease 192.168.2.21 from the laptop by MAC reservation, NTP
  via the `ntp=` fallback, IOC running. Second boot with the dhcpcd.conf
  fix (`raspberyPI3B+_16_10_2026_2.cap`): lease carries NTP server and
  hostname (`new_ntp_servers`, `new_host_name=rtems-pi3`), board serial
  `000000009645d7b0` from the device tree, prompt `rtems-pi3>`. Third boot
  with the static default line and the same kernel
  (`raspberyPI3B+_16_10_2026_3.cap`): unchanged behaviour plus the fixed
  MAC. Overnight soak in DHCP mode 2026-10-16/17
  (`raspberyPI3B+_16_10_2026_5.cap`): 19 h 50 min at 1 kHz toggle rate,
  no reboot, counter consistent with 1000/s throughout, one DHCP exchange
  only (infinite lease), heap flat at 60 MB used (minimum free ever within
  200 bytes of the current value), dhcpcd task stack 2.5 KB of 8 KB. DHCP
  mode is therefore as trustworthy as the static one. The same `--net=`
  line works when the Pi boots from a TFTP server instead of the card,
  since the firmware fetches `cmdline.txt` the same way.

The SD card copy in `SD_card/cmdline.txt` keeps the static default.

`rtemsNfsMount "host:/export", "/mnt" [, "options"]` mounts NFS with
mount options, NFSv4 over TCP by default (Base's `nfsMount` is v3 only);
`rtemsUmount` unmounts. Verified on the board 2026-10-20 (mount, read,
write, mkdir). Two libbsd limitations: files cannot be created directly in
the mount root (use a subdirectory) and a server can be mounted only once
per boot. Build guide section 10e.

Network boot without any SD card works (verified 2026-10-20): the same
dnsmasq serves the firmware files, kernel and `epics.tar.gz` by TFTP from
`/srv/tftp/rpi`, filled by `make tftp`; the Pi then boots in 17 s exactly
as from the card. Build guide section 10b.
The GPIO support is loaded at run time instead of being linked into the
kernel (verified 2026-10-20); see "EPICS support modules: linked into the
kernel or loaded at run time" below.

## EPICS support modules loaded at run time

An EPICS support module such as `RPi_IOSup` or iocStats consists of code
(device support, drivers, iocsh commands, built into a library) and a dbd
file that names what the code provides. Since 2026-10-20 this IOC loads
such modules at run time with the RTEMS loader, from **three files per
module** in `epics.tar.gz`, and the kernel knows nothing about any
particular module:

| File | Origin | Made by |
|---|---|---|
| `lib/RTEMS-raspberrypi2/libNAME.a` | the module's ordinary static library for `RTEMS-raspberrypi2`, byte-identical to the module's own build | the module (`make` in its tree) |
| `dbd/NAME.dbd` | the module's dbd, as is | the module |
| `bin/RTEMS-raspberrypi2/NAME_registerRecordDeviceDriver.o` | the registration code EPICS generates from the dbd (the same file a linked IOC gets), compiled without unwind tables | this repository |

plus one shared file, `etc/libdl.conf`, which tells the loader where the
libraries are (`/lib/RTEMS-raspberrypi2/lib*.a`). In `st.cmd`, after the
IOC's own `rpi3_registerRecordDeviceDriver` and before anything that uses
the module:

```
epicsLoadModule "RPi_IO"          # silent; errors only
epicsLoadModule "devIocStats", 1  # verbose: lists the three files and the result
```

`epicsLoadModule "NAME" [, verbose] [, "object"] [, "dbd"]` loads
`dbd/NAME.dbd`, `dlopen()`s the registration object, lets the loader pull
the referenced members out of `libNAME.a` (exactly the members a static
link would take), refuses to continue if a symbol is still unresolved, and
calls `NAME_registerRecordDeviceDriver(pdbbase)`. Afterwards the module's
record types, device supports (`DTYP`), drivers (`dbior`) and iocsh
commands exist as if it had been linked in. Modules load only before
`iocInit` (Base refuses dbd additions afterwards). `rtemsLoadReport` lists
the loaded objects, library members included; `rtemsLoadTrace 0x160`
switches the loader's own trace on for the next load (diagnostic).

### The kernel is module-agnostic

A module may call any function of RTEMS, libc, libm or Base's classic IOC
libraries without the kernel being rebuilt or told about it. The link of
`kernel7.img` forces every public symbol of those libraries in
(`scripts/libdl_api_symbols.py` writes a linker response file with about
6400 `-u` roots; members that would collide with `rtems_config.o`,
duplicate or override another member, or reference unsatisfiable
configuration tables are left out), and the loader's symbol table built by
`rtems-syms` lists all of them (about 15800 names). Cost: the kernel grew
from 7.6 MB to 9.7 MB; nothing at run time. The build prints, for every
module in the archive, whether all its external symbols are in the table
(`scripts/libdl_check_module.py`); with this kernel they are. Build guide
section 10d has the mechanism, the limits and the three bugs found on the
way.

### Making another module loadable

For a module `xxx` built for `RTEMS-raspberrypi2` in a tree `$(XXX)` (set
in `configure/RELEASE`), add to `rpi3App/src/Makefile`, modelled on the
`devIocStats` block there:

```make
DBD_INSTALLS += $(XXX)/dbd/xxx.dbd                                  # host pass, outside the OS_CLASS block
ifeq ($(OS_CLASS),RTEMS)
LIB_INSTALLS += $(XXX)/lib/$(T_A)/libxxx.a
BIN_INSTALLS += xxx_registerRecordDeviceDriver.o
RAMFS_MODULES += $(INSTALL_BIN)/xxx_registerRecordDeviceDriver.o
RAMFS_MODULE_LIBS += $(INSTALL_LIB)/libxxx.a
RAMFS_EXTRA_DBDS += $(TOP)/dbd/xxx.dbd
xxx_registerRecordDeviceDriver_CXXFLAGS += -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables
xxx_registerRecordDeviceDriver.cpp: $(XXX)/dbd/xxx.dbd $(REGRECDEVDRV_dep)
	@$(RM) $@
	$(REGISTERRECORDDEVICEDRIVER) $(REGRDDFLAGS) -o $@ $< $(basename $@) $(IOCS_APPL_TOP)
endif
```

Database files the module ships go through `DB_INSTALLS` in
`rpi3App/Db/Makefile`; then `epicsLoadModule "xxx"` and the
`dbLoadRecords` lines in `st.cmd`, and `make`. For a module built inside
this repository (like `RPi_IOSup`) only the `BIN_INSTALLS` of the
registration object is needed, the library and dbd are installed anyway.
Requirements for the module: built against the same Base and RTEMS trees
as the kernel, C or C++ without exception unwinding in the library
(EPICS's default flags give that), no symbols outside RTEMS, libc, libm and
Base's classic libraries (the check script reports any).

The classic alternative, linking a module into the kernel, still works:
put its dbd into `rpi3_DBD` and its library into `rpi3_LIBS` and do not
load it at run time. `RPI_IO_LOADABLE = NO` in `rpi3App/src/Makefile`
does that for the GPIO driver.
