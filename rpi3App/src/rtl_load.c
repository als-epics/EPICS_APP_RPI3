/*
 * rtl_load.c - load code into the running IOC with the RTEMS run-time
 * loader (libdl), iocsh commands rtemsLoad and rtemsLoadReport.
 *
 *   rtemsLoad "/bin/RTEMS-raspberrypi2/hello_mod.o"             # init: rtl_module_init
 *   rtemsLoad "/path/module.o", "my_init_function"
 *
 * dlopen() links an ELF relocatable object (or a .a archive) against the
 * symbol table of the running kernel. That table is generated at build
 * time by rtems-syms from a first link of the same objects (rpi3_prelink)
 * and compiled into both kernels as rpi3_syms.c, see rpi3App/src/Makefile.
 * After loading, the init function named in the second argument (default
 * rtl_module_init) is looked up with dlsym() and called; EPICS modules use
 * it to register their records, devices and iocsh commands.
 *
 * Objects loaded this way stay loaded (RTLD_GLOBAL): a second module may
 * use the symbols of the first. Nothing is unloaded; an EPICS module with
 * registered records cannot be removed from a running IOC anyway.
 */

#include <stdio.h>
#include <string.h>

#include "iocsh.h"
#include "epicsExport.h"

#ifdef __rtems__

#include <dlfcn.h>
#include <sys/stat.h>
#include <rtems/rtl/rtl.h>
#include <rtems/rtl/rtl-shell.h>
#include <rtems/rtl/rtl-trace.h>
#include "dbAccess.h"        /* pdbbase */
#include "dbStaticLib.h"     /* dbLoadDatabase */

static int
rtemsLoad(const char *path, const char *init)
{
    void *handle;
    int (*fn)(void);
    int rc;

    if (path == NULL || *path == '\0') {
        printf("usage: rtemsLoad \"path\" [, \"init-function\"]\n");
        return -1;
    }
    if (init == NULL || *init == '\0')
        init = "rtl_module_init";

    handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (handle == NULL) {
        printf("rtemsLoad: dlopen %s: %s\n", path, dlerror());
        return -1;
    }
    printf("rtemsLoad: %s loaded\n", path);

    fn = (int (*)(void))(uintptr_t)dlsym(handle, init);
    if (fn == NULL) {
        printf("rtemsLoad: %s has no %s(): %s (object stays loaded)\n",
               path, init, dlerror());
        return -1;
    }
    rc = fn();
    printf("rtemsLoad: %s() -> %d\n", init, rc);
    return rc;
}

/* "rtemsLoadReport": the loader's own object list (rtl shell "list"). */
static void
rtemsLoadReport(void)
{
    char *argv[] = { "rtl", "list", NULL };
    rtems_rtl_shell_command(2, argv);
}

/*
 * epicsLoadModule "NAME" [, verbose] [, "object"] [, "dbd"]
 *
 * Silent on success (errors only); verbose = 1 lists the three files with
 * their paths and the registration result.
 *
 * Loads an EPICS support module shipped as three files (2026-10-20): the
 * module's own library lib/<arch>/libNAME.a as built, its dbd as is, and
 * the registration object generated from the dbd:
 *   1. dbLoadDatabase(dbd)   default dbd/NAME.dbd, relative to the IOC's
 *                            working directory ("/" on the Pi)
 *   2. dlopen(object)        default /bin/RTEMS-raspberrypi2/
 *                            NAME_registerRecordDeviceDriver.o; the RTEMS
 *                            loader resolves its references to the device
 *                            supports, drivers and registrars by pulling
 *                            the members out of the libraries listed in
 *                            /etc/libdl.conf (same semantics as a static
 *                            link: only the referenced members load)
 *   3. check nothing is left unresolved (dlopen succeeds even then; the
 *                            hole would surface as a crash on first use)
 *   4. NAME_registerRecordDeviceDriver(pdbbase)
 * After that the module's record and device supports, drivers (dbior)
 * and iocsh commands are available exactly as if it had been linked into
 * the kernel. Use before iocInit and before anything that needs the module.
 */
static int
epicsLoadModule(const char *name, int verbose, const char *object, const char *dbd)
{
    char objbuf[128], dbdbuf[128], libbuf[128], fnbuf[128];
    void *handle;
    int (*reg)(DBBASE *);
    int rc;

    if (name == NULL || *name == '\0') {
        printf("usage: epicsLoadModule \"NAME\" [, verbose] [, \"object\"] [, \"dbd\"]\n");
        return -1;
    }
    if (object == NULL || *object == '\0') {
        snprintf(objbuf, sizeof objbuf,
                 "/bin/RTEMS-raspberrypi2/%s_registerRecordDeviceDriver.o", name);
        object = objbuf;
    }
    if (dbd == NULL || *dbd == '\0') {
        snprintf(dbdbuf, sizeof dbdbuf, "dbd/%s.dbd", name);
        dbd = dbdbuf;
    }
    snprintf(fnbuf, sizeof fnbuf, "%s_registerRecordDeviceDriver", name);
    snprintf(libbuf, sizeof libbuf, "/lib/RTEMS-raspberrypi2/lib%s.a", name);

    if (verbose) {
        struct stat st;
        printf("epicsLoadModule %s:\n", name);
        printf("  dbd      %s\n", dbd);
        printf("  object   %s\n", object);
        printf("  library  %s%s\n", libbuf,
               stat(libbuf, &st) == 0 ? "" : "  (not found; the loader searches /etc/libdl.conf)");
    }

    if (dbLoadDatabase(dbd, NULL, NULL) != 0) {
        printf("epicsLoadModule: dbLoadDatabase(%s) failed (missing file, or called after iocInit?)\n", dbd);
        return -1;
    }
    handle = dlopen(object, RTLD_NOW | RTLD_GLOBAL);
    if (handle == NULL) {
        printf("epicsLoadModule: dlopen %s: %s\n", object, dlerror());
        return -1;
    }
    {
        int unresolved = 0;
        if (dlinfo(handle, RTLD_DI_UNRESOLVED, &unresolved) == 0 && unresolved != 0) {
            printf("epicsLoadModule: %s: %d unresolved symbol(s) after loading; "
                   "is lib%s.a in /lib/RTEMS-raspberrypi2 and listed in /etc/libdl.conf? "
                   "(rtemsLoadReport shows the loaded objects)\n", object, unresolved, name);
            return -1;
        }
    }
    reg = (int (*)(DBBASE *))(uintptr_t)dlsym(handle, fnbuf);
    if (reg == NULL) {
        printf("epicsLoadModule: %s has no %s(): %s\n", object, fnbuf, dlerror());
        return -1;
    }
    rc = reg(pdbbase);
    if (rc != 0)
        printf("epicsLoadModule: %s: %s() -> %d\n", name, fnbuf, rc);
    else if (verbose)
        printf("  %s() -> 0\n", fnbuf);
    return rc;
}

#else /* host build: commands exist, do nothing */

static int
rtemsLoad(const char *path, const char *init)
{
    (void)path; (void)init;
    printf("rtemsLoad: only on the RTEMS build\n");
    return -1;
}

static void
rtemsLoadReport(void)
{
    printf("rtemsLoadReport: only on the RTEMS build\n");
}

static int
epicsLoadModule(const char *name, int verbose, const char *object, const char *dbd)
{
    (void)name; (void)verbose; (void)object; (void)dbd;
    printf("epicsLoadModule: only on the RTEMS build\n");
    return -1;
}

#endif

static const iocshArg modArg0 = { "NAME (module, e.g. RPi_IO)", iocshArgString };
static const iocshArg modArg1 = { "verbose (1 = list the files loaded; default 0 = errors only)", iocshArgInt };
static const iocshArg modArg2 = { "object (default /bin/RTEMS-raspberrypi2/NAME_registerRecordDeviceDriver.o)", iocshArgString };
static const iocshArg modArg3 = { "dbd (default dbd/NAME.dbd)", iocshArgString };
static const iocshArg * const modArgs[] = { &modArg0, &modArg1, &modArg2, &modArg3 };
static const iocshFuncDef modFuncDef = { "epicsLoadModule", 4, modArgs,
    "Load an EPICS support module at run time from three files: dbd/NAME.dbd,\n"
    "bin/<arch>/NAME_registerRecordDeviceDriver.o and lib/<arch>/libNAME.a\n"
    "(members pulled in by the RTEMS loader via /etc/libdl.conf), then\n"
    "NAME_registerRecordDeviceDriver(pdbbase). Silent unless verbose or an\n"
    "error. Before iocInit.\n" };

static void
modCallFunc(const iocshArgBuf *args)
{
    iocshSetError(epicsLoadModule(args[0].sval, args[1].ival, args[2].sval, args[3].sval));
}

static const iocshArg loadArg0 = { "path", iocshArgString };
static const iocshArg loadArg1 = { "init function (default rtl_module_init)", iocshArgString };
static const iocshArg * const loadArgs[] = { &loadArg0, &loadArg1 };
static const iocshFuncDef loadFuncDef = { "rtemsLoad", 2, loadArgs,
    "Load an ELF object into the IOC with the RTEMS run-time loader and call\n"
    "its init function.\n" };

static void
loadCallFunc(const iocshArgBuf *args)
{
    iocshSetError(rtemsLoad(args[0].sval, args[1].sval));
}

/* rtemsLoadTrace mask: loader trace bits (rtems/rtl/rtl-trace.h): 0x20
 * symbols, 0x40 relocations, 0x100 section placement, 0 off. Diagnostic
 * only; prints thousands of lines on the console for one module. */
static void traceCallFunc(const iocshArgBuf *args)
{
#if RTEMS_RTL_TRACE
    rtems_rtl_trace_set_mask((rtems_rtl_trace_mask) args[0].ival);
    printf("rtemsLoadTrace: mask 0x%x\n", args[0].ival);
#else
    printf("rtemsLoadTrace: loader built without tracing\n");
#endif
}
static const iocshArg traceArg0 = { "mask", iocshArgInt };
static const iocshArg *traceArgs[] = { &traceArg0 };
static const iocshFuncDef traceFuncDef = { "rtemsLoadTrace", 1, traceArgs,
    "Set the RTEMS loader trace mask (0x20 symbols, 0x40 relocs, 0x100 sections, 0 off)" };

static const iocshFuncDef reportFuncDef = { "rtemsLoadReport", 0, NULL,
    "List the objects loaded with rtemsLoad.\n" };

static void
reportCallFunc(const iocshArgBuf *args)
{
    (void)args;
    rtemsLoadReport();
}

static void
rtemsLoadRegistrar(void)
{
    iocshRegister(&loadFuncDef, loadCallFunc);
    iocshRegister(&reportFuncDef, reportCallFunc);
    iocshRegister(&modFuncDef, modCallFunc);
    iocshRegister(&traceFuncDef, traceCallFunc);
}
epicsExportRegistrar(rtemsLoadRegistrar);
