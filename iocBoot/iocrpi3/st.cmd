#- SPDX-FileCopyrightText: 2001 Argonne National Laboratory
#-
#- SPDX-License-Identifier: EPICS

#- Example RTEMS startup script

#- You may have to change rpi3 to something else
#- everywhere it appears in this file

< envPaths
#if executed on the raspbery pi than TOP is /
epicsEnvSet("TOP","/")
cd "${TOP}"
## Register all support components
dbLoadDatabase("dbd/rpi3.dbd")
rpi3_registerRecordDeviceDriver(pdbbase)
## Raspberry Pi GPIO support, loaded at run time (RTEMS libdl): loads
## dbd/RPi_IO.dbd, dlopen()s bin/RTEMS-raspberrypi2/RPi_IO.o from the
## archive and runs RPi_IO_registerRecordDeviceDriver(pdbbase). Must come
## before the rpiGpioConfig* calls and the records that use DTYP "RPiGpio".
epicsLoadModule "RPi_IO"
## iocStats (IOC statistics records), also loaded at run time:
## dbd/devIocStats.dbd + bin/RTEMS-raspberrypi2/devIocStats.o from the archive.
epicsLoadModule "devIocStats"

epicsEnvSet EPICS_TS_NTP_INET   172.233.177.198
NTPTimeSyncNow
# Pacific time zone: PST=UTC-8, DST starts 2nd Sun Mar, ends 1st Sun Nov
epicsEnvSet TZ                   "PST8PDT,M3.2.0,M11.1.0"
# EPICS_TZ for iocStats (RPI3:TIMEZONE) and Base time routines: same rule as TZ
epicsEnvSet EPICS_TZ             "$(TZ)"

## Load record instances
dbLoadTemplate("db/user.substitutions")
dbLoadRecords("db/rpi3Version.db", "user=RPI3")
dbLoadRecords("db/dbSubExample.db", "user=RPI3")
## ---------------------------------------------------------------------
## Raspberry Pi GPIO (RPi_IOSup). Configure the pins first, then load the
## records that use them. Both functions must run before iocInit.
##
## rpiGpioConfigOutput("LABEL", pin, initialValue)
##   LABEL        name the records use: OUT "@LABEL" (bo) or
##                OUT "@L0 L1 L2 ..." (mbboDirect, bit 0 = first label);
##                1..31 chars, no spaces/commas, case-insensitive, unique
##   pin          BCM GPIO number 0..31 (NOT the header pin number);
##                the 40-pin header carries GPIO 2..27:
##                GPIO17 = header pin 11, GPIO27 = header pin 13,
##                GPIO22 = pin 15, GPIO23 = pin 16, GPIO24 = pin 18
##   initialValue 0 = low (0 V), 1 = high (3.3 V) as soon as the pin
##                becomes an output
##
## rpiGpioConfigInput("LABEL", pin, "pull", "detect")
##   LABEL, pin   as above; records use INP "@LABEL" (bi, longin),
##                INP "@LABEL latched" (bi: level sampled in the ISR),
##                INP "@LABEL count" (longin: detections since boot),
##                INP "@L0 L1 ..." (mbbiDirect)
##   pull         internal ~50 kohm resistor:
##                  "none"  pin floats (drive it externally)
##                  "up"    to 3.3 V: switch to GND reads 1 open, 0 closed
##                  "down"  to GND:   switch to 3.3 V reads 0 open, 1 closed
##   detect       interrupt source; every detection counts, latches the
##                level and processes the pin's "I/O Intr" records:
##                  "none"     no interrupt, records read when scanned
##                  "rising"   low -> high edges
##                  "falling"  high -> low edges
##                  "both"     every edge
##                  "high"     level 1 (re-armed each time a record reads)
##                  "low"      level 0 (re-armed each time a record reads)
##                No debouncing: a mechanical switch on "both" counts
##                its bounces; use "rising" or "falling" for switches.
##
## Shell helpers: rpiGpioReport 1, rpiGpioSet "LED",1, rpiGpioGet "BTN"
## Full description: RPi_IOSup/README.md
## ---------------------------------------------------------------------
## LED on GPIO17 (header pin 11) via 330 ohm to GND; starts off
rpiGpioConfigOutput("LED", 17, 0)
## Push button between GPIO27 (header pin 13) and GND, internal pull-up,
## interrupt on both edges (press and release)
rpiGpioConfigInput("BTN", 27, "up", "both")
#dbLoadRecords("db/rpiGpioLed.db", "P=RPI3,OUT=LED,IN=BTN")
## iocStats records: $(IOC):HEARTBEAT, :CPU_LOAD, :MEM_USED, :UPTIME ...
dbLoadRecords("db/iocAdminSoft.db", "IOC=RPI3")
## Timing test (verified 2026-10-14: 1000 edges/s, ~7 us jitter): wire
## GPIO17 to GPIO27, comment the rpiGpioLed.db line above and uncomment
## this one (both use the LED/BTN pins). RATE .01 second gives 100 edges/s.
dbLoadRecords("db/rpiGpioToggleTest.db", "P=RPI3,OUT=LED,IN=BTN,RATE=.001 second")

#- Set this to see messages from mySub
#-var mySubDebug 1

#- Run this to trace the stages of iocInit
#-traceIocInit

iocInit

## Start any sequence programs
#seq(sncExample, "user=RPI3")
