# SPDX-FileCopyrightText: 1998 Argonne National Laboratory
#
# SPDX-License-Identifier: EPICS

TOP=../..
include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS BELOW HERE

# Install databases, templates & substitutions like this
DB += circle.db
DB += dbExample1.db
DB += dbExample2.db
DB += rpi3Version.db
DB += dbSubExample.db
# iocStats records, taken from the module tree so that epics.tar.gz carries them
ifneq ($(wildcard $(IOCSTATS)/db/iocAdminSoft.db),)
DB_INSTALLS += $(IOCSTATS)/db/iocAdminSoft.db
endif
DB += user.substitutions

# If <anyname>.db template is not named <anyname>*.template add
# <anyname>_TEMPLATE = <templatename>

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD EXTRA GNUMAKE RULES BELOW HERE

