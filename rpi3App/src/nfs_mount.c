/*
 * nfs_mount.c - NFS mounts with options from iocsh (NFSv4 in particular).
 *
 *   rtemsNfsMount "192.168.2.10:/export/ioc", "/mnt/ioc"              # nfsv4,tcp
 *   rtemsNfsMount "192.168.2.10:/export/ioc", "/mnt/ioc", "nfsv3,udp"
 *   rtemsUmount   "/mnt/ioc"
 *
 * EPICS Base's nfsMount(uidhost, path, mountpoint) calls mount() without
 * an option string, which the libbsd NFS glue (rtemsbsd/fs/nfsclient/nfs.c,
 * a port of FreeBSD's mount_nfs) takes as NFSv3. The same glue understands
 * an option string, so this command exposes it. Options the glue parses
 * (others are accepted and ignored, the string must be shorter than 64
 * characters):
 *
 *   nfsv2 | nfsv3 | nfsv4   protocol version; nfsv4 talks to port 2049
 *                           directly, no mount protocol, no portmapper
 *   tcp | udp               transport (default of the glue: UDP for v2/v3)
 *   port=N                  server port
 *   noinet4 | noinet6       address family to avoid
 *
 * NFSv4 notes for this build: libbsd has FreeBSD's "new NFS" client with
 * the v4 state machine, but no nfsuserd (owner strings are mapped only
 * when the server sends numeric ids, otherwise files show as "nobody"),
 * no nfscbd (no delegations) and no Kerberos (sec=sys only). "user@" in
 * the device string is rejected by the glue. The mount point is created
 * if it does not exist (mount_and_make_target_path).
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "iocsh.h"
#include "epicsExport.h"

#ifdef __rtems__

#include <sys/stat.h>
#include <dirent.h>
#include <rtems/libio.h>

static int
rtemsNfsMount(const char *dev, const char *mnt, const char *options)
{
    DIR *d;
    int n = 0;

    if (dev == NULL || *dev == '\0' || mnt == NULL || *mnt == '\0') {
        printf("usage: rtemsNfsMount \"host:/export\", \"/mountpoint\" "
               "[, \"options\"]   (default options: nfsv4,tcp)\n");
        return -1;
    }
    if (strchr(dev, ':') == NULL) {
        printf("rtemsNfsMount: device must be host:/path, got %s\n", dev);
        return -1;
    }
    if (options == NULL || *options == '\0')
        options = "nfsv4,tcp";
    if (strlen(options) >= 64) {
        printf("rtemsNfsMount: option string longer than 63 characters\n");
        return -1;
    }

    if (mount_and_make_target_path(dev, mnt, RTEMS_FILESYSTEM_TYPE_NFS,
            RTEMS_FILESYSTEM_READ_WRITE, options) != 0) {
        printf("rtemsNfsMount: mount %s on %s (%s): %s\n", dev, mnt, options,
               strerror(errno));
        return -1;
    }
    /* touch the mount so a dead server shows up here and not later */
    d = opendir(mnt);
    if (d != NULL) {
        while (readdir(d) != NULL && n < 1000)
            n++;
        closedir(d);
        printf("rtemsNfsMount: %s mounted on %s (%s), %d directory entries\n",
               dev, mnt, options, n);
    } else {
        printf("rtemsNfsMount: %s mounted on %s (%s), but opendir: %s\n",
               dev, mnt, options, strerror(errno));
    }
    return 0;
}

static int
rtemsUmount(const char *mnt)
{
    if (mnt == NULL || *mnt == '\0') {
        printf("usage: rtemsUmount \"/mountpoint\"\n");
        return -1;
    }
    if (unmount(mnt) != 0) {
        printf("rtemsUmount: %s: %s\n", mnt, strerror(errno));
        return -1;
    }
    printf("rtemsUmount: %s unmounted\n", mnt);
    return 0;
}

#else /* host build */

static int
rtemsNfsMount(const char *dev, const char *mnt, const char *options)
{
    (void)dev; (void)mnt; (void)options;
    printf("rtemsNfsMount: only on the RTEMS build\n");
    return -1;
}

static int
rtemsUmount(const char *mnt)
{
    (void)mnt;
    printf("rtemsUmount: only on the RTEMS build\n");
    return -1;
}

#endif

static const iocshArg mntArg0 = { "host:/export", iocshArgString };
static const iocshArg mntArg1 = { "/mountpoint", iocshArgString };
static const iocshArg mntArg2 = { "options (default nfsv4,tcp)", iocshArgString };
static const iocshArg * const mntArgs[] = { &mntArg0, &mntArg1, &mntArg2 };
static const iocshFuncDef mntFuncDef = { "rtemsNfsMount", 3, mntArgs,
    "Mount an NFS export with mount options; NFSv4 over TCP by default.\n"
    "Options understood: nfsv2 nfsv3 nfsv4 tcp udp port=N noinet4 noinet6.\n" };

static void
mntCallFunc(const iocshArgBuf *args)
{
    iocshSetError(rtemsNfsMount(args[0].sval, args[1].sval, args[2].sval));
}

static const iocshArg umntArg0 = { "/mountpoint", iocshArgString };
static const iocshArg * const umntArgs[] = { &umntArg0 };
static const iocshFuncDef umntFuncDef = { "rtemsUmount", 1, umntArgs,
    "Unmount a file system mounted with rtemsNfsMount or nfsMount.\n" };

static void
umntCallFunc(const iocshArgBuf *args)
{
    iocshSetError(rtemsUmount(args[0].sval));
}

static void
rtemsNfsMountRegistrar(void)
{
    iocshRegister(&mntFuncDef, mntCallFunc);
    iocshRegister(&umntFuncDef, umntCallFunc);
}
epicsExportRegistrar(rtemsNfsMountRegistrar);
