/*
 * hello_mod.c - libdl feasibility module (branch libdl, 2026-10-17).
 *
 * Compiled to a plain relocatable object (hello_mod.o, installed into
 * bin/RTEMS-raspberrypi2 and packed into epics.tar.gz), NOT linked into the
 * kernel. Loaded at run time with
 *
 *     rtemsLoad "/bin/RTEMS-raspberrypi2/hello_mod.o"
 *
 * which calls rtl_module_init() below; that registers the iocsh command
 * helloMod, which uses libc, libm and EPICS functions of the kernel to show
 * that the run-time linker resolved them. If this works on the Pi, the
 * GPIO support (RPi_IOSup) can be shipped the same way.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "iocsh.h"
#include "epicsTime.h"
#include "epicsThread.h"

static int hello_calls;

static void helloModCallFunc(const iocshArgBuf *args)
{
    epicsTimeStamp now;
    char buf[40];

    epicsTimeGetCurrent(&now);
    epicsTimeToStrftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S.%06f", &now);
    hello_calls++;
    printf("helloMod: call %d at %s from thread %s, sqrt(2) = %.6f, "
           "text = '%s'\n", hello_calls, buf, epicsThreadGetNameSelf(),
           sqrt(2.0), args[0].sval ? args[0].sval : "");
}

static const iocshArg helloArg0 = { "text", iocshArgString };
static const iocshArg * const helloArgs[] = { &helloArg0 };
static const iocshFuncDef helloFuncDef = { "helloMod", 1, helloArgs,
    "libdl test: prints time, thread and the argument.\n" };

/* entry point: rtemsLoad looks it up with dlsym() and calls it */
int rtl_module_init(void)
{
    iocshRegister(&helloFuncDef, helloModCallFunc);
    printf("helloMod: loaded, iocsh command helloMod registered\n");
    return 0;
}
