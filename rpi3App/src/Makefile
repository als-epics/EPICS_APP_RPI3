# SPDX-FileCopyrightText: 1998 Argonne National Laboratory
#
# SPDX-License-Identifier: EPICS

TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS BELOW HERE

# Use typed rset structure (see 3.16.1 release notes)
USR_CPPFLAGS += -DUSE_TYPED_RSET -DUSE_TYPED_DSET

# xxxRecord.h will be created from xxxRecord.dbd
DBDINC += xxxRecord

# Install xxxSupport.dbd into <top>/dbd
DBD += xxxSupport.dbd

# Raspberry Pi GPIO: driver + device support live in $(TOP)/RPi_IOSup
# (library RPi_IO, dbd RPi_IO.dbd, see RPi_IOSup/README.md).

# Build an IOC support library
LIBRARY_IOC += rpi3Support

# Compile and add code to the support library
rpi3Support_SRCS += xxxRecord.c
rpi3Support_SRCS += devXxxSoft.c

# Link locally-provided code into the support library,
# rather than directly into the IOC application, that
# causes problems on Windows DLL builds
rpi3Support_SRCS += dbSubExample.c
rpi3Support_SRCS += devrpi3Version.c
rpi3Support_SRCS += rpi3Hello.c
rpi3Support_SRCS += initTrace.c

rpi3Support_LIBS += $(EPICS_BASE_IOC_LIBS)

# Auto-generate a header file containing a version string.
# Version comes from the VCS if available, else date+time.
GENVERSION = rpi3Version.h
# Macro name
GENVERSIONMACRO = rpi3VERSION

# Build the IOC application
PROD_IOC = rpi3

# RPi_IO (GPIO driver + device support): linked into the kernel (NO) or
# loaded at run time from epics.tar.gz by st.cmd's epicsLoadModule "RPi_IO"
# (YES, branch libdl; needs the libdl symbol table below).
RPI_IO_LOADABLE = YES

# --- RTEMS: bake iocBoot/db/dbd into the ELF so the SD card needs no NFS ---
# Use ifeq OS_CLASS (not rpi3_SRCS_RTEMS +=) so we ADD to rpi3_SRCS
# without displacing rpi3_SRCS_DEFAULT (which carries rpi3Main.cpp).
ifeq ($(OS_CLASS),RTEMS)
# Two kernels are built from the same objects:
#   rpi3        (bin/.../rpi3.img, symlinked kernel7.img): st.cmd, db and
#                dbd compiled in; uses epics.tar.gz from the SD card when
#                the firmware loaded it, the compiled-in copy otherwise.
#   rpi3_plain  (bin/.../rpi3_plain.img): no compiled-in copy, about
#                490 KB smaller; needs epics.tar.gz on the card, refuses to
#                start without it. Copy either one to the card as kernel7.img.
# The archive itself is described in ramfs.mk ("make ramfs" at the TOP
# rebuilds only the archive). See rtems_fs_setup.c and
# patches/RTEMS6_RASPBERRYPI_BUILD.md section 8a.
PROD_IOC += rpi3_plain
rpi3_SRCS += epicsRtemsFSImage.c
# The gzip untar code in librtemscpu.a needs zlib (symbols are z_-prefixed
# in RTEMS). EPICS puts -lz before the RTEMS libraries on the link line, so
# ask the linker up front for the three symbols; libz.a then satisfies them
# when it is scanned.
rpi3_LDFLAGS += -Wl,-u,z_inflateInit2_ -Wl,-u,z_inflate -Wl,-u,z_inflateEnd
rpi3_SRCS += rtems_netsetup.c
rpi3_SRCS += rtems_fs_setup.c
# Shadows librtemsCom.a's rtems_config.o to force a 500 us tick (2 kHz),
# enabling ".01 second" periodic scans. The base ship was built with the
# 10000 us default, and OP_SYS_CFLAGS can't retro-fix an archive.
rpi3_SRCS += rtems_config.c
# Redirect every call to rtems_dhcpcd_start() to __wrap_rtems_dhcpcd_start()
# (defined in rtems_netsetup.c): static IP by default, or the real libbsd
# dhcpcd when cmdline.txt on the SD card says "--net=dhcp".
rpi3_LDFLAGS += -Wl,--wrap=rtems_dhcpcd_start

# --- Hang-hunt diagnostics (2026-04 .. 2026-10), OFF by default --------
# The RPi 3B+ freeze was a bug in the RTEMS raspberrypi BSP interrupt
# dispatcher (bsps/arm/raspberrypi/irq/irq.c, fixed 2026-10-13).  All the
# instrumentation built to find it is kept but compiled out.  To get it
# back set RPI_IOC_DEBUG=YES below AND define RPI_IOC_DEBUG in
#   rtems-libbsd/freebsd/sys/dev/usb/controller/dwc_otg.c  (RAM crash
#   ring, write probe, UART tracer, periodic "DWC_OTG t=" dump)
#   kernel/bsps/arm/raspberrypi/irq/irq.c                  (dispatcher
#   markers), rebuild both, then rebuild this app.
# Debug pieces linked here: fiq_gpio_toggle.c (FIQ LED on GPIO16, needs
# the F-bit patch in arm_exc_interrupt.S), irqwrap.c (--wrap of the
# interrupt-controller enable/disable), dwc_otg_fiq_stub.c, the per-second
# "[HB ...]" line in heartbeat.c and the iocsh debug variables.
#RPI_IOC_DEBUG = YES
ifeq ($(RPI_IOC_DEBUG),YES)
USR_CPPFLAGS += -DRPI_IOC_DEBUG
rpi3_LDFLAGS += -Wl,--defsym=bsp_stack_fiq_size=4096
rpi3_LDFLAGS += -Wl,--wrap=_ARMV4_Exception_fiq_default
rpi3_SRCS += fiq_gpio_toggle.c
rpi3_SRCS += dwc_otg_fiq_stub.c
rpi3_SRCS += irqwrap.c
rpi3_LDFLAGS += -Wl,--wrap=bsp_interrupt_vector_enable
rpi3_LDFLAGS += -Wl,--wrap=bsp_interrupt_vector_disable
rpi3_DBD += dwcDebugVarsDebug.dbd
endif
# Hardware watchdog keeper (production; 'var hb_wdog_enable 0' disarms).
rpi3_SRCS += heartbeat.c

# --- Run-time loading (libdl, branch libdl 2026-10-17) -------------------
# dlopen() in the kernel needs a table of the kernel's own global symbols.
# rtems-syms makes it from a linked image, so the kernel is linked twice:
# rpi3_prelink (the plain kernel without the table, not for the card),
# then rpi3_syms.c is generated from it and compiled into rpi3 and
# rpi3_plain. The table references the symbols by name, so the second link
# resolves their final addresses; rpi3_prelink has no compiled-in file
# image, hence its symbol set is a subset of both real kernels.
RTEMS_SYMS = $(RTEMS_TOOLS)/bin/rtems-syms
# Extra symbols to keep in the link (and hence in the table) with -u.
# Normally empty since the API roots below cover everything;
# scripts/libdl_check_module.py (run after the table is made) reports what
# a module in the archive still misses.
LIBDL_KEEP_SYMBOLS =
COMMA = ,
rpi3_LDFLAGS += $(addprefix -Wl$(COMMA)-u$(COMMA),$(LIBDL_KEEP_SYMBOLS))
# (the rpi3_prelink product and "rpi3_SRCS += rpi3_syms.c" are set up
# below, next to rpi3_plain, once the rpi3_* lists are complete)
RTEMS_BSP_LIBDIR = $(RTEMS_BASE)/$(GNU_TARGET)$(RTEMS_VERSION)/$(RTEMS_BSP)/lib
# Module-agnostic kernel (2026-10-20): a module built elsewhere must find
# every RTEMS, libc, libm and Base function in the kernel without the
# kernel knowing the module. scripts/libdl_api_symbols.py writes a linker
# response file with one -u root per public symbol of those libraries
# (members that collide with rtems_config.o, duplicate another archive or
# reference unsatisfiable configuration tables are left out), so the
# functions survive --gc-sections, which must stay on (libbsd does not
# link without it), and --whole-archive is not an option for librtemscpu
# (its confdefs defaults are meant to be overridden by rtems_config.o).
# Cost: about 2 MB of kernel text. The table filter below then needs no
# per-module keep list any more.
LIBDL_BASE_LIBS = $(EPICS_BASE)/lib/$(T_A)/libdbRecStd.a $(EPICS_BASE)/lib/$(T_A)/libdbCore.a \
    $(EPICS_BASE)/lib/$(T_A)/libca.a $(EPICS_BASE)/lib/$(T_A)/libCom.a
LIBDL_ABI_FLAGS := $(shell sed -n 's/^ABI_FLAGS=//p' $(RTEMS_BASE)/lib/pkgconfig/$(GNU_TARGET)$(RTEMS_VERSION)-$(RTEMS_BSP).pc)
LIBDL_LIBC := $(shell $(RTEMS_TOOLS)/bin/$(CC_FOR_TARGET) $(LIBDL_ABI_FLAGS) -print-file-name=libc.a)
LIBDL_LIBM := $(shell $(RTEMS_TOOLS)/bin/$(CC_FOR_TARGET) $(LIBDL_ABI_FLAGS) -print-file-name=libm.a)
LIBDL_LIBGCC := $(shell $(RTEMS_TOOLS)/bin/$(CC_FOR_TARGET) $(LIBDL_ABI_FLAGS) -print-libgcc-file-name)
LIBDL_CRT := $(foreach f,libstdc++.a libsupc++.a crti.o crtn.o crtbegin.o crtend.o,$(shell $(RTEMS_TOOLS)/bin/$(CC_FOR_TARGET) $(LIBDL_ABI_FLAGS) -print-file-name=$(f)))
LIBDL_LINKCMDS := $(wildcard $(RTEMS_BSP_LIBDIR)/linkcmds*)
LIBDL_API_LIBS = $(RTEMS_BSP_LIBDIR)/librtemscpu.a $(RTEMS_BSP_LIBDIR)/librtemsbsp.a $(LIBDL_LIBC) $(LIBDL_LIBM) \
    $(LIBDL_BASE_LIBS)
LIBDL_POOL_LIBS = $(RTEMS_BSP_LIBDIR)/libbsd.a $(RTEMS_BSP_LIBDIR)/libtftpfs.a $(RTEMS_BSP_LIBDIR)/libz.a \
    $(RTEMS_BSP_LIBDIR)/libtelnetd.a $(RTEMS_BSP_LIBDIR)/libntp.a $(RTEMS_BSP_LIBDIR)/libtecla_r.a \
    $(INSTALL_LIB)/librpi3Support.a $(LIBDL_LIBGCC) $(LIBDL_CRT) $(LIBDL_LINKCMDS) \
    $(foreach l,qsrv pvAccessIOC pvAccessCA pvAccess pvData rtemsCom,$(EPICS_BASE)/lib/$(T_A)/lib$(l).a)
LIBDL_APP_OBJS = $(addsuffix $(OBJ),$(basename $(filter-out rpi3_syms.c,$(rpi3_SRCS) $(rpi3_SRCS_DEFAULT))))
libdl_api.rsp: $(TOP)/scripts/libdl_api_symbols.py
	python3 $(TOP)/scripts/libdl_api_symbols.py $(RTEMS_TOOLS)/bin/$(NM_FOR_TARGET) $@ \
	    --objs $(LIBDL_APP_OBJS) --libs $(LIBDL_API_LIBS) --pool $(LIBDL_POOL_LIBS)
rpi3_LDFLAGS += @libdl_api.rsp
rpi3_syms.c: rpi3_prelink $(TOP)/scripts/libdl_syms_filter.py
	$(ECHO) "Generating $@ (kernel symbol table for libdl) from $<"
	$(RTEMS_SYMS) -e -C $(RTEMS_TOOLS)/bin/$(CC_FOR_TARGET) -c "$(GCCSPECS)" -S $@ $<
	python3 $(TOP)/scripts/libdl_syms_filter.py $(RTEMS_TOOLS)/bin/$(NM_FOR_TARGET) $@ \
	    $(RTEMS_BSP_LIBDIR) $(EPICS_BASE)/lib/$(T_A) $(INSTALL_LIB)
	-python3 $(TOP)/scripts/libdl_check_module.py $(RTEMS_TOOLS)/bin/$(NM_FOR_TARGET) $@ $(RAMFS_MODULES) $(RAMFS_MODULE_LIBS)
# hello_mod.o: the feasibility module, a plain object that is NOT linked
# into the kernel; installed into bin/ and packed into epics.tar.gz, loaded
# at run time with rtemsLoad "/bin/RTEMS-raspberrypi2/hello_mod.o".
BIN_INSTALLS += hello_mod.o
# (rtl_load.c / rtl_load.dbd are added outside this block: the dbd is
# assembled in the host pass, where OS_CLASS is not RTEMS)
# --------------------------------------------------------------------------

RAMFS_TAR = $(INSTALL_BIN)/epics.tar.gz
# from inside this directory the module is a hard prerequisite, so a clean
# build installs it before the archive and the compiled-in image are made
RAMFS_MODULES = $(INSTALL_BIN)/hello_mod.o
ifeq ($(RPI_IO_LOADABLE),YES)
RAMFS_MODULES += $(INSTALL_BIN)/RPi_IO_registerRecordDeviceDriver.o
RAMFS_MODULE_LIBS = $(INSTALL_LIB)/libRPi_IO.a
RAMFS_EXTRA_DBDS = $(TOP)/dbd/RPi_IO.dbd
endif
# devIocStats as THREE files (2026-10-20): the module's own library is
# installed as is into lib/<arch>/ (LIB_INSTALLS), its dbd as is into dbd/
# (DBD_INSTALLS, above), and the only generated file is the registration
# object EPICS derives from the dbd, installed into bin/<arch>/. At run time
# epicsLoadModule loads the registration object and the RTEMS loader pulls
# the referenced members out of the library (etc/libdl.conf lists the
# archives). The registration file is C++ and must be built without unwind
# tables (the loader data-aborts on the R_ARM_PREL31 relocations of
# .ARM.exidx); EPICS applies <file>_CXXFLAGS to that one object only.
ifneq ($(wildcard $(IOCSTATS)/dbd/devIocStats.dbd),)
LIB_INSTALLS += $(IOCSTATS)/lib/$(T_A)/libdevIocStats.a
BIN_INSTALLS += devIocStats_registerRecordDeviceDriver.o
RAMFS_MODULES += $(INSTALL_BIN)/devIocStats_registerRecordDeviceDriver.o
RAMFS_MODULE_LIBS += $(INSTALL_LIB)/libdevIocStats.a
RAMFS_EXTRA_DBDS += $(TOP)/dbd/devIocStats.dbd
devIocStats_registerRecordDeviceDriver_CXXFLAGS += -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables
# same recipe as Base's RULES.Db, with the dbd taken from the module tree
devIocStats_registerRecordDeviceDriver.cpp: $(IOCSTATS)/dbd/devIocStats.dbd $(REGRECDEVDRV_dep)
	@$(RM) $@
	$(REGISTERRECORDDEVICEDRIVER) $(REGRDDFLAGS) -o $@ $< $(basename $@) $(IOCS_APPL_TOP)
endif
include $(TOP)/rpi3App/src/ramfs.mk
rpi3_IMFS_FILES = $(RAMFS_FILES)
endif
# ------------------------------------------------------------------------

# rpi3.dbd will be created and installed
DBD += rpi3.dbd

# rpi3.dbd will include these files:
rpi3_DBD += base.dbd
rpi3_DBD += xxxSupport.dbd
rpi3_DBD += dbSubExample.dbd
rpi3_DBD += devrpi3Version.dbd
rpi3_DBD += rpi3Hello.dbd
rpi3_DBD += initTrace.dbd
ifneq ($(RPI_IO_LOADABLE),YES)
rpi3_DBD += RPi_IO.dbd
endif
# iocStats (IOCSTATS in configure/RELEASE) as a run-time loaded module
# named devIocStats: the module's own dbd is installed verbatim as
# dbd/devIocStats.dbd (like RPi_IO.dbd: a plain file, not expanded, so it
# needs no record types) and loaded by epicsLoadModule together with
# bin/<arch>/devIocStats_registerRecordDeviceDriver.o (built below) and
# lib/<arch>/libdevIocStats.a (installed as is); nothing of it goes into
# the kernel or into rpi3.dbd.
ifneq ($(wildcard $(IOCSTATS)/dbd/devIocStats.dbd),)
DBD_INSTALLS += $(IOCSTATS)/dbd/devIocStats.dbd
endif
# iocsh "var" access: hb_wdog_enable always; the DWC OTG hang-debug
# switches only with RPI_IOC_DEBUG=YES (dwcDebugVarsDebug.dbd above).
# dwcDebugVars.c has host placeholders, so no OS_CLASS guard is needed.
rpi3_DBD += dwcDebugVars.dbd
# nfs_mount.c: iocsh rtemsNfsMount (NFS with options, NFSv4 by default) and
# rtemsUmount; RTEMS body under #ifdef __rtems__, host build gets stubs.
# Outside the OS_CLASS block: the dbd is assembled in the host pass.
rpi3_SRCS += nfs_mount.c
rpi3_DBD += nfs_mount.dbd
# rtl_load.c: iocsh rtemsLoad / rtemsLoadReport (RTEMS libdl dlopen + init
# function; host build gets stub commands). Outside the OS_CLASS block so
# that the dbd, assembled in the host pass, lists the registrar.
rpi3_SRCS += rtl_load.c
rpi3_DBD += rtl_load.dbd
# kexec.c: iocsh rpiKexec, warm restart into a kernel fetched over TFTP
# (RTEMS body under #ifdef __rtems__, the host build gets a stub command).
rpi3_SRCS += kexec.c
rpi3_DBD += kexec.dbd
rpi3_SRCS += dwcDebugVars.c

# rpi3_registerRecordDeviceDriver.cpp derives from rpi3.dbd
rpi3_SRCS += rpi3_registerRecordDeviceDriver.cpp

# Build the main IOC entry point where needed
rpi3_SRCS_DEFAULT += rpi3Main.cpp
rpi3_SRCS_vxWorks += -nil-

# Link in the code from our support libraries
rpi3_LIBS += rpi3Support
ifneq ($(RPI_IO_LOADABLE),YES)
rpi3_LIBS += RPi_IO
endif

# To build SNL programs, SNCSEQ must be defined
# in the <top>/configure/RELEASE file
ifneq ($(SNCSEQ),)
    # Build sncExample into rpi3Support
    sncExample_SNCFLAGS += +r
    rpi3_DBD += sncExample.dbd
    # A .stt sequence program is *not* pre-processed:
    rpi3Support_SRCS += sncExample.stt
    rpi3Support_LIBS += seq pv
    rpi3_LIBS += seq pv

    # Build sncProgram as a standalone program
    PROD_HOST += sncProgram
    sncProgram_SNCFLAGS += +m
    # A .st sequence program *is* pre-processed:
    sncProgram_SRCS += sncProgram.st
    sncProgram_LIBS += seq pv
    sncProgram_LIBS += $(EPICS_BASE_HOST_LIBS)
endif

# Link QSRV (pvAccess Server) if available
ifdef EPICS_QSRV_MAJOR_VERSION
    rpi3_LIBS += qsrv
    rpi3_LIBS += $(EPICS_BASE_PVA_CORE_LIBS)
    rpi3_DBD += PVAServerRegister.dbd
    rpi3_DBD += qsrv.dbd
endif

# Finally link IOC to the EPICS Base libraries
rpi3_LIBS += $(EPICS_BASE_IOC_LIBS)

# rpi3_plain: everything rpi3 has except the compiled-in file image.
# Evaluated here (:=, immediately), after the rpi3_* lists are complete and
# before RULES appends rpi3_SRCS_DEFAULT to rpi3_SRCS.
ifeq ($(OS_CLASS),RTEMS)
# rpi3_prelink: the plain kernel without the libdl symbol table; only the
# input of rtems-syms (see the libdl block above), not for the card.
rpi3_prelink_SRCS        := $(filter-out epicsRtemsFSImage.c,$(rpi3_SRCS))
rpi3_prelink_SRCS_DEFAULT := $(rpi3_SRCS_DEFAULT)
rpi3_prelink_LIBS        := $(rpi3_LIBS)
rpi3_prelink_LDFLAGS     := $(rpi3_LDFLAGS)
rpi3_prelink_SYS_LIBS    := $(rpi3_SYS_LIBS)
PROD_IOC += rpi3_prelink
# both real kernels carry the symbol table
rpi3_SRCS += rpi3_syms.c
rpi3_plain_SRCS        := $(filter-out epicsRtemsFSImage.c,$(rpi3_SRCS))
rpi3_plain_SRCS_DEFAULT := $(rpi3_SRCS_DEFAULT)
rpi3_plain_LIBS        := $(rpi3_LIBS)
rpi3_plain_LDFLAGS     := $(rpi3_LDFLAGS)
rpi3_plain_SYS_LIBS    := $(rpi3_SYS_LIBS)
endif

include $(TOP)/configure/RULES

ifeq ($(OS_CLASS),RTEMS)
libdl_api.rsp: $(LIBDL_APP_OBJS)
rpi3_prelink rpi3 rpi3_plain: libdl_api.rsp
endif
#----------------------------------------
#  ADD EXTRA GNUMAKE RULES BELOW HERE

# Explicit dependency needed for generated header file
devrpi3Version$(DEP): $(COMMON_DIR)/$(GENVERSION)

# RTEMS: epicsMemFs + kernelN.img post-install (no-op for other targets)
-include $(EPICS_BASE)/configure/os/RULES.RTEMS-Image

# RTEMS: refresh bin/RTEMS-raspberrypi2/epics.tar.gz (rule in ramfs.mk)
ifeq ($(OS_CLASS),RTEMS)
install: $(RAMFS_TAR)
endif
