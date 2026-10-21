# SPDX-License-Identifier: EPICS
#
# RPi_IOSup - Raspberry Pi GPIO driver and device support for EPICS on
# RTEMS (BCM2835/2837, Pi 2/3). Builds a stub on the Linux host so the
# dbd and host tools can be generated.
TOP = ../..
include $(TOP)/configure/CONFIG

# USE_TYPED_DRVET is defined in drvRPiGPIO.c itself: the registration code
# generated from RPi_IO.dbd (RPi_IO_registerRecordDeviceDriver.cpp, for the
# loadable module) uses the classic "struct drvet" and does not build with it.
USR_CPPFLAGS += -DUSE_TYPED_RSET -DUSE_TYPED_DSET

LIBRARY_IOC += RPi_IO
DBD += RPi_IO.dbd
RPi_IO_SRCS += drvRPiGPIO.c
RPi_IO_LIBS += $(EPICS_BASE_IOC_LIBS)

ifeq ($(OS_CLASS),RTEMS)
# Run-time loadable module (libdl, branch libdl): the whole library plus its
# own registration code (generated from RPi_IO.dbd by EPICS's
# registerRecordDeviceDriver.pl, like an IOC's) in ONE relocatable object.
# Installed into bin/RTEMS-raspberrypi2, packed into epics.tar.gz by
# rpi3App/src/ramfs.mk, loaded in st.cmd with
#     epicsLoadModule "RPi_IO"
# which does dbLoadDatabase("dbd/RPi_IO.dbd"), dlopen() of this object and
# RPi_IO_registerRecordDeviceDriver(pdbbase). $(LD) is "arm-rtems6-ld -r".
# The RTEMS run-time loader (RTEMS 6, rtl-mdreloc-arm.c) crashed on the
# R_ARM_PREL31 relocations of the .ARM.exidx unwind tables that g++ emits
# for the generated registration file (2026-10-20, NULL target address).
# The file needs no exceptions and no unwinding: build it without them, so
# the module carries no exidx sections at all.
USR_CXXFLAGS += -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables
# Three files per loadable module (2026-10-20): libRPi_IO.a stays the plain
# library EPICS builds, RPi_IO.dbd is installed as is, and only the
# registration object generated from the dbd is installed into bin/. At
# run time epicsLoadModule loads that object and the RTEMS loader pulls
# the referenced members out of the library (etc/libdl.conf).
BIN_INSTALLS += RPi_IO_registerRecordDeviceDriver.o
endif

include $(TOP)/configure/RULES
