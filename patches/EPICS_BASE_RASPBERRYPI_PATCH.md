# EPICS Base 7.0.10 for RTEMS 6 on Raspberry Pi 3B+ (and 4B): patch and PVAccess

File: `epics-base-R7.0.10-raspberrypi.patch` (unified diff, `-p1`).

Base version it applies to: the EPICS `7.0` branch at commit
`5b23dcb46` ("Added Doxygen annotations to iocInit.h (#804)", 51 commits
after the `R7.0.10` tag). It applies cleanly there; on the `R7.0.10` tag
itself the hunks should still apply with small offsets.

## What the patch contains

New files
- `configure/os/CONFIG.Common.RTEMS-raspberrypi2` - target definition for
  the RTEMS `raspberrypi2` BSP (Pi 2/3, 32-bit ARM): libbsd networking,
  `-lntp`, `--gc-sections`, BSP library path, `RPI_KERNEL_IMG = kernel7.img`.
- `configure/os/CONFIG.Common.RTEMS-raspberrypi4b` - same for the Pi 4
  (`aarch64`, `kernel8.img`). Written at the same time, not yet tested.
- `configure/os/RULES.RTEMS-Image` - optional rules an application includes
  at the end of its `src/Makefile`: embeds `iocBoot`/`db`/`dbd` files into
  the ELF with `epicsMakeMemFs.pl` and, after `make install`, runs
  `objcopy -O binary` to produce `<prod>.img` plus a `kernel7.img` symlink
  the Pi GPU bootloader loads from the SD card. No NFS/TFTP server needed.
- `modules/libcom/src/osi/os/RTEMS-posix/osdTime.cpp` - RTEMS 6 / libbsd
  time provider (Chris Johns' rtems-ntpd based version with local changes:
  for the Raspberry Pi BSPs `rtems_ntpd` is not started and the clock is
  synchronised with `epicsNtpGetTime()` against `EPICS_TS_NTP_INET`).
  2026-10-14: the NTP reply's 32-bit fraction is now used (`tv_nsec` was
  forced to 0). Without it the EPICS NTPTime provider measured its tick
  rate from whole seconds, ran up to 1.7 % off between the 60 s syncs and
  corrected the IOC clock with a jump at each sync, seen as a 70 ms
  disturbance of every periodic scan once a minute. CLOCK_REALTIME is
  now set only once. The same change alone is in
  `epics-base-osdTime-ntp-fraction.patch` for an already patched tree:
  `sudo patch -p1 -d /usr/local/epics/R7.0.10/base < epics-base-osdTime-ntp-fraction.patch`
  followed by the Base rebuild of the PVAccess section and an IOC relink.
  2026-10-15: the file includes `epicsNtp.h` by a source-relative path
  (`../../../../RTEMS/epicsNtp.h`), because `modules/libcom/RTEMS`, which
  installs that header, is built after `modules/libcom/src`; with the plain
  include a build from a fresh clone stopped with "epicsNtp.h: No such file"
  (found by `scripts/build_epics_rpi3.sh`; an already built tree had the
  header installed from an earlier run and never showed it).
- `modules/libcom/src/osi/os/RTEMS-posix/osdReadline.c` - copy of the
  RTEMS-score file so `COMMANDLINE_LIBRARY = LIBTECLA` builds on the
  RTEMS-posix OS API (found through the `os/RTEMS-posix` include path).

Modified files
- `configure/os/CONFIG.Common.RTEMS` - drop the RTEMS 5 "legacy internal"
  network stack variant, add `-lntp` to the posix/libbsd link libraries,
  trim `GESYS_LIBS`, link libtecla without `-lncurses` (the RTEMS 6 tecla
  package has no ncurses).
- `configure/os/CONFIG_SITE.Common.RTEMS` - **site specific, edit after
  applying:** `RTEMS_VERSION = 6`, `RTEMS_BASE = /home/mirek/RTEMS-PI/rtems-6/6`,
  `COMMANDLINE_LIBRARY = LIBTECLA`.
- `modules/libcom/RTEMS/posix/rtems_init.c` - two fixes: the time-zone
  check tested `getenv("")` instead of `getenv("TZ")`; start `telnetd` on
  RTEMS 6 (the call was commented out upstream).
- `modules/libcom/src/osi/os/RTEMS/osdVME.h` - no `<bsp/VME.h>` on
  `aarch64` either (Pi 4).

## Intentionally left out

The local EPICS tree also carries changes for other boards (mcp750,
mvme167, mvme2100, RL8 host configs, TFTP boot path) and a modified
`modules/libcom/RTEMS/posix/rtems_config.c`. None of them is needed for
the Raspberry Pi: the IOC application ships its own `rtems_config.c`
(`rpi3App/src/rtems_config.c`, 500 us tick, larger interrupt stack) that
shadows the one in libcom.

## How to apply and build

```bash
git clone -b 7.0 https://github.com/epics-base/epics-base.git
cd epics-base
git checkout 5b23dcb46          # or R7.0.10
patch -p1 < /path/to/epics-base-R7.0.10-raspberrypi.patch
# edit configure/os/CONFIG_SITE.Common.RTEMS: RTEMS_BASE, RTEMS_VERSION
# add to configure/CONFIG_SITE:  CROSS_COMPILER_TARGET_ARCHS = RTEMS-raspberrypi2
make -j4
```

Prerequisites: RTEMS 6 tools and the `raspberrypi2` BSP installed under
`RTEMS_BASE`, rtems-libbsd (branch `6-freebsd-14`) built and installed into
the same prefix, and the RTEMS kernel built with the interrupt-dispatcher fix
in `rtems-raspberrypi-irq-dispatch-fix.patch` (without it the IOC freezes
under USB/network load).

Application side: see `rpi3App/src/Makefile` for the `RULES.RTEMS-Image`
usage and `rpi3App/src/rtems_netsetup.c` for static IP configuration
through `--wrap=rtems_dhcpcd_start`.

## PVAccess (pvData, pvAccess, normativeTypes, pvaClient, pvDatabase, QSRV)

EPICS 7 carries the PVAccess modules as git submodules under `modules/`.
A plain `git clone` leaves those directories empty, and the EPICS build
skips an empty submodule silently: Base builds and the IOC links, but
there is no `libpvAccess`, no `libqsrv` and no `PVAServerRegister.dbd`.
That was the state of the Base tree here until 2026-10-14.

Versions pinned by this Base (`git submodule status`):

| Module | Directory | Version |
|---|---|---|
| pvDataCPP | `modules/pvData` | 8.0.7 |
| pvAccessCPP | `modules/pvAccess` | 7.1.8 |
| normativeTypesCPP | `modules/normativeTypes` | 6.0.2 |
| pvaClientCPP | `modules/pvaClient` | 4.8.1 |
| pvDatabaseCPP | `modules/pvDatabase` | 4.7.2 |
| pva2pva (QSRV, p2p gateway) | `modules/pva2pva` | 1.4.1 |

### Install the modules

```bash
cd /usr/local/epics/R7.0.10/base
sudo git submodule update --init modules/pvData modules/pvAccess modules/normativeTypes \
     modules/pvaClient modules/pvDatabase modules/pva2pva
```

Leave `.ci` out, it is only for continuous integration. `sudo` is needed
on this laptop because the Base tree belongs to root. The pinned commits
are the ones listed above; no patching of the modules is required for
RTEMS 6 with rtems-libbsd.

### Rebuild Base

The cross tools must be on the PATH, also under `sudo`, otherwise the
RTEMS target is skipped or the build stops at the first module:

```bash
cd /usr/local/epics/R7.0.10/base
sudo env PATH=/home/mirek/RTEMS-PI/rtems-6/6/bin:$PATH make -j4
```

Only the new modules compile (libcom, ca and database are up to date),
for `linux-x86_64` and `RTEMS-raspberrypi2`, about 10 minutes. Always
run `make` from the top of Base; `make -C modules/pvAccess` alone fails
with `No rule to make target '/configure/RULES_TOP'` because the module
needs the variables the top-level build passes down.

Check the result:

```bash
ls lib/RTEMS-raspberrypi2/ | grep -i "pv\|libnt\|qsrv"
#  libnt.a libpvAccess.a libpvAccessCA.a libpvAccessIOC.a libpvaClient.a
#  libpvData.a libpvDatabase.a libqsrv.a
ls dbd | grep -i "PVA\|qsrv"        # PVAClientRegister.dbd PVAServerRegister.dbd qsrv.dbd
grep MAJOR cfg/CONFIG_QSRV_VERSION   # EPICS_QSRV_MAJOR_VERSION = 1
```

### Rebuild the IOC

`rpi3App/src/Makefile` already contains the standard block

```make
ifdef EPICS_QSRV_MAJOR_VERSION
    rpi3_LIBS += qsrv
    rpi3_LIBS += $(EPICS_BASE_PVA_CORE_LIBS)
    rpi3_DBD += PVAServerRegister.dbd
    rpi3_DBD += qsrv.dbd
endif
```

so nothing changes in the application; the variable now exists and the
block becomes active. Because the generated dbd changes, remove it and
the binaries before building:

```bash
cd ~/EPICS_APP_RPI3
rm -f dbd/rpi3.dbd rpi3App/src/O.Common/rpi3.dbd rpi3App/src/O.*/rpi3_registerRecordDeviceDriver.* \
      bin/RTEMS-raspberrypi2/rpi3* rpi3App/src/O.RTEMS-raspberrypi2/rpi3*
make
```

The RTEMS link line then contains `-lpvAccess -lpvAccessCA -lpvAccessIOC
-lpvData -lqsrv`, `dbd/rpi3.dbd` has `registrar(QSRVRegistrar)`, and
`kernel7.img` grows from 6.5 MB to about 7.6 MB. QSRV starts by itself at
`iocInit`; nothing is added to `st.cmd`. From a client on the same subnet:

```bash
pvlist 192.168.2.21          # lists the IOC's PVA server
pvget RPI3:<record>          # any record is served over PVA by QSRV
```

PVAccess uses UDP 5076 for discovery and TCP 5075 for data; with the
laptop acting as the Pi's gateway (see `NETWORK_SERVERS_SETUP.md`) the
laptop itself is on the Pi's subnet and sees the broadcasts directly.
