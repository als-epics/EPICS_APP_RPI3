# scripts/

Two scripts rebuild the whole tool stack of this IOC on a fresh Ubuntu
22.04/24.04 machine. They reproduce the installation described in
`../patches/RTEMS6_RASPBERRYPI_BUILD.md`, with the same upstream commits and
the patches from `../patches/`.

| Script | Builds | Asks for | Time |
|---|---|---|---|
| `build_rtems_rpi3.sh` | RTEMS Source Builder cross tools, RTEMS kernel 6 (BSP `arm/raspberrypi2`) with the dispatcher fix, rtems-libbsd with the Pi 3B+ patch, rtems-net-services, libtecla | sources directory (default `~/RTEMS_SRC`), install directory (default `~/RTEMS_PI`, prefix becomes `<dir>/rtems-6/6`) | 45 to 90 min, mostly the tools |
| `build_epics_rpi3.sh` | EPICS base 7.0 at commit 5b23dcb46 (R7.0.10 + 51) with the PVAccess submodules and the Raspberry Pi patch, for the host and `RTEMS-raspberrypi2` | EPICS directory (default `/usr/local/epics/R7.0.10/base`), the RTEMS install directory from the first script | 15 to 30 min |

A third script, `update_tftp.sh`, is for network boot: it fills the TFTP
directory of the laptop's dnsmasq (default `/srv/tftp/rpi`) from
`../SD_card/` and the build outputs; `make tftp` at the top calls it. See
`../patches/RTEMS6_RASPBERRYPI_BUILD.md` section 10b.

```bash
scripts/build_rtems_rpi3.sh          # answer the two questions, wait
scripts/build_epics_rpi3.sh          # then this one
make                                 # the IOC: bin/RTEMS-raspberrypi2/{rpi3.img,rpi3_plain.img,epics.tar.gz}
```

Options, both scripts: `-y` takes the defaults without asking (directories
can also come from `RTEMS_SRC_DIR`, `RTEMS_INSTALL_DIR`, `EPICS_DIR`),
`--prepare-only` clones and patches but does not build, `-h` prints the
header. `JOBS=n` limits parallel jobs, `DRY_RUN=1` prints the build commands
instead of running them, `FORCE=1` (first script) rebuilds a step whose
result is already installed.

What the scripts do that the manual recipe does not say:

- The upstream `6.2` tags of rtems, rtems-source-builder and
  rtems-net-services were moved after this installation was made. The
  scripts check out the exact commits that were verified on the Pi, not the
  tags.
- `rtems-addon-packages` lived on git.rtems.org, which is offline. The
  libtecla 1.6.3 tree and the RTEMS makefiles it needs are kept in
  `../patches/rtems-addon-packages-libtecla-1.6.3.tar.gz` (commit 4f5951a
  plus two local changes: the demo programs are not built, and
  `config.guess`/`config.sub` are 2022 versions so `configure` accepts
  current hosts).
- EPICS base under `/usr/local` is created with `sudo` once and then owned
  by the user, so no `sudo` is needed for the build or later rebuilds.
- The EPICS script sets `RTEMS_BASE` in
  `configure/os/CONFIG_SITE.Common.RTEMS` and
  `CROSS_COMPILER_TARGET_ARCHS = RTEMS-raspberrypi2` in `configure/CONFIG_SITE`
  of the Base tree, and offers to point this application's
  `configure/RELEASE` at the new Base.

First real run of `build_rtems_rpi3.sh` on another machine (2026-10-17):
the tools step took 41 min and worked; the kernel step found an older
`~/RTEMS_SRC/kernel` configured for `aarch64/raspberrypi4b` and failed
("Could not find the program aarch64-rtems6-gcc", "invalid lock file").
The script now keeps a pre-existing `config.ini` only if it configures
`arm/raspberrypi2`; otherwise it saves it as `config.ini.<date>.bak`,
installs ours and removes the old `build/`. Stale `.lock-waf_*` files are
removed before every `waf configure`. If you keep other RTEMS trees, point
the script at a fresh sources directory instead (first question).

Re-running either script is safe: cloned trees are left alone when they
carry local changes, applied patches are detected by a marker and skipped,
and a step whose library already exists in the BSP library directory is
skipped.

## Which patches the scripts apply (checked against the tested trees 2026-10-20)

| Layer | Pinned upstream | Patch applied by the script | Result |
|---|---|---|---|
| RTEMS kernel | be446b02 | `patches/rtems-raspberrypi-irq-dispatch-fix.patch` + `patches/rtems-kernel-raspberrypi2-config.ini` | identical to the tree that built the R03.00 images, apart from compiled-out debug markers |
| rtems-libbsd | c4fc2033 | `patches/rtems-libbsd-raspberrypi.patch` (LAN7800 `muge` driver, DWC OTG nexus, Pi device list) | identical in behaviour to the tested tree: the debug instrumentation patch that tree also carries is compiled out (`RPI_IOC_DEBUG` off) |
| EPICS base | 5b23dcb46 | `patches/epics-base-R7.0.10-raspberrypi.patch` (includes the NTP fraction fix) | identical for the Pi target; the development tree additionally holds local changes for other boards (mvme/mcp750) and legacy-stack code paths that the Pi build does not compile |

Not applied on purpose: `rtems-libbsd-raspberrypi-debug-instrumentation.patch`
and `rtems-raspberrypi-irq-dispatch-debug-markers.patch` (hang diagnosis
only, see build guide section 11). The other upstream bugs found during
the tests (BSP `vc.c` cache flush, tftpfs `NULL` device, run-time loader
`.ARM.exidx`, Base dhcpcd.conf and REBIND, libbsd NFS root-create and
remount) are worked around inside this repository's IOC sources and need
no patch to the lower layers. A stack built by these scripts has so far
been built end to end (2026-10-15) but not booted on the board; the board
tests ran on the development trees described above.

## 2026-10-20: module-agnostic kernel, three-file modules

Nothing below the IOC changed for the run-time loaded modules: the kernel,
BSP, libbsd and Base trees and their installed libraries are the ones the
scripts reproduce (all dated 2026-10-14). The whole mechanism lives in this
repository (`rpi3App/src/Makefile`, `scripts/libdl_*.py`, `rtl_load.c`,
`etc/libdl.conf`; build guide section 10d).

One optional dependency the scripts do not build: iocStats. `configure/RELEASE`
points `IOCSTATS` at `/usr/local/epics/R7.0.10/modules/soft/iocStatsALS`,
Mirek's copy with RTEMS 6 fixes (see its `README_RTEMS6_fixes.md`), built
for `RTEMS-raspberrypi2` with Base's cross target list. On a machine
without that tree the IOC build skips the iocStats module (the Makefile
blocks test for the dbd file) and `st.cmd`'s `epicsLoadModule "devIocStats"`
reports an error at boot and the IOC continues without the statistics
records; remove or comment that line and the `iocAdminSoft.db` load in
that case.

