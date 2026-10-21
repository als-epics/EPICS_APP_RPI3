# epics.tar.gz: st.cmd, envPaths, db/* and dbd/rpi3.dbd as a gzip'ed ustar
# archive that the Raspberry Pi firmware loads into RAM at boot
# ("initramfs epics.tar.gz 0x30000000" in config.txt) and that
# rtems_fs_setup.c unpacks into the IOC's in-memory file system.
#
# Included twice:
#   - from $(TOP)/Makefile          -> "make ramfs" builds only the archive
#   - from rpi3App/src/Makefile    -> the same list feeds the compiled-in
#     copy (rpi3_IMFS_FILES) and "make" refreshes the archive on install
# Requires TOP and PERL (both come from configure/CONFIG).
#
# Paths inside the archive are relative to TOP (iocBoot/iocrpi3/st.cmd,
# db/*.db, dbd/rpi3.dbd), the same layout as the compiled-in image.
# --mode makes every entry owner-writable: some sources (db/circle.db) are
# read-only and a read-only file left by a failed unpack would otherwise
# block the compiled-in fallback ("open: Permission denied").
# ramfsPad.pl appends a second, empty gzip member so that the file ends on
# a 4 KB boundary plus one full 4 KB page: the Pi firmware delivers the last
# partial page of the loaded file damaged (2026-10-15), so the real data
# must not end in a partial page. zlib stops at the end of the first
# member; host gzip/tar accept concatenated members.

RAMFS_ARCH ?= RTEMS-raspberrypi2
RAMFS_TAR  ?= $(TOP)/bin/$(RAMFS_ARCH)/epics.tar.gz
RAMFS_PAD   = $(TOP)/rpi3App/src/ramfsPad.pl

# The list is taken when the Makefile is read, so everything in it must
# exist by then: db/ comes from the *Sup/Db and *App/Db directories
# (src_DEPEND_DIRS = Db in rpi3App/Makefile), envPaths from iocBoot (built
# before the *App dirs, see the top Makefile). dbd/rpi3.dbd is generated
# by rpi3App/src itself and is therefore listed unconditionally: make then
# installs it before packing. The same list feeds the compiled-in image.
RAMFS_ENVPATHS = $(TOP)/iocBoot/iocrpi3/envPaths
RAMFS_DBD      = $(TOP)/dbd/rpi3.dbd
# Loadable modules (libdl, branch libdl): plain ELF objects installed into
# bin/ by rpi3App/src/Makefile (BIN_INSTALLS), loaded with rtemsLoad.
# Optional, so that "make ramfs" works before the App directory was built.
RAMFS_MODULES ?= $(wildcard $(TOP)/bin/$(RAMFS_ARCH)/*.o)
# dbd files of loadable modules (dbd/RPi_IO.dbd, dbd/devIocStats.dbd ...),
# loaded by epicsLoadModule: every installed dbd except the kernel's own
RAMFS_EXTRA_DBDS ?= $(wildcard $(TOP)/dbd/RPi_IO.dbd $(TOP)/dbd/devIocStats.dbd)
# the modules' libraries (as built) and the loader's archive list: the
# registration object loaded by epicsLoadModule pulls the members it needs
# out of lib/<arch>/libNAME.a at run time (see etc/libdl.conf)
RAMFS_MODULE_LIBS ?= $(wildcard $(TOP)/lib/$(RAMFS_ARCH)/libRPi_IO.a $(TOP)/lib/$(RAMFS_ARCH)/libdevIocStats.a)
RAMFS_LIBDL_CONF  = $(TOP)/etc/libdl.conf
RAMFS_FILES = $(wildcard $(TOP)/iocBoot/iocrpi3/st.cmd) \
              $(wildcard $(RAMFS_ENVPATHS)) \
              $(RAMFS_MODULES) $(RAMFS_MODULE_LIBS) $(RAMFS_LIBDL_CONF) \
              $(wildcard $(TOP)/db/*) \
              $(RAMFS_DBD) $(RAMFS_EXTRA_DBDS)

$(RAMFS_TAR): $(RAMFS_FILES) $(RAMFS_PAD)
	@test -f $(RAMFS_ENVPATHS) || { echo "ramfs: $(RAMFS_ENVPATHS) missing (iocBoot not built yet?)"; exit 1; }
	@test $(words $(filter %.db %.substitutions,$(RAMFS_FILES))) -gt 0 || { echo "ramfs: no files in $(TOP)/db (Db directories not installed yet?)"; exit 1; }
	@mkdir -p $(@D)
	@echo "TAR  $(words $(RAMFS_FILES)) file(s) -> $(@F)"
	tar --format=ustar --owner=0 --group=0 --numeric-owner --mode='u+rw,go+r' -C $(TOP) -czf $@ $(patsubst $(TOP)/%,%,$(RAMFS_FILES))
	$(PERL) $(RAMFS_PAD) $@

.PHONY: ramfs-tar
ramfs-tar: $(RAMFS_TAR)
