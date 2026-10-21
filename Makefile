# SPDX-FileCopyrightText: 1997 Argonne National Laboratory
#
# SPDX-License-Identifier: EPICS

# Makefile at top of application tree
TOP = .
include $(TOP)/configure/CONFIG

# Directories to build, any order
DIRS += configure
DIRS += $(wildcard *Sup)
DIRS += $(wildcard *App)
DIRS += $(wildcard *Top)
DIRS += $(wildcard iocBoot)

# The build order is controlled by these dependency rules:

# All dirs except configure depend on configure
$(foreach dir, $(filter-out configure, $(DIRS)), \
    $(eval $(dir)_DEPEND_DIRS += configure))

# Any *App dirs depend on all *Sup dirs
$(foreach dir, $(filter %App, $(DIRS)), \
    $(eval $(dir)_DEPEND_DIRS += $(filter %Sup, $(DIRS))))

# Any *Top dirs depend on all *Sup and *App dirs
$(foreach dir, $(filter %Top, $(DIRS)), \
    $(eval $(dir)_DEPEND_DIRS += $(filter %Sup %App, $(DIRS))))

# iocBoot only generates envPaths (from configure/RELEASE) and needs
# nothing from the *App dirs; the *App dirs need envPaths, because
# rpi3App/src packs iocBoot/iocrpi3/envPaths into the kernel image and
# into epics.tar.gz. So build iocBoot BEFORE the *App dirs (reversed
# from the makeBaseApp default).
$(foreach dir, $(filter %App, $(DIRS)), \
    $(eval $(dir)_DEPEND_DIRS += iocBoot))

# Add any additional dependency rules here:

include $(TOP)/configure/RULES_TOP

# "make ramfs": only bin/RTEMS-raspberrypi2/epics.tar.gz (st.cmd, db, dbd
# for the Raspberry Pi IOC, loaded by the firmware at boot, see
# RTEMS6_RASPBERRYPI_BUILD.md section 8a). Installs changed db files first;
# a dbd change (new menu choice, record, device) still needs a full "make".
# The plain "make" builds the kernels AND the archive.
include $(TOP)/rpi3App/src/ramfs.mk
.PHONY: ramfs
ramfs:
	@test -f dbd/rpi3.dbd || { echo "ramfs: dbd/rpi3.dbd missing, run a full make first"; exit 1; }
	$(foreach d,$(wildcard *Sup/Db *App/Db) iocBoot,$(MAKE) -C $(d) install &&) true
	$(MAKE) ramfs-tar

# "make tftp": fill the TFTP directory for network boot (default
# /srv/tftp/rpi, override with TFTP_ROOT=...) from SD_card/ and the build
# outputs; see scripts/update_tftp.sh and the build guide section 10b.
.PHONY: tftp
tftp:
	scripts/update_tftp.sh $(TFTP_ROOT)
