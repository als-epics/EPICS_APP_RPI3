/*
 * rtems_fs_setup.c - where the IOC's files (st.cmd, envPaths, the db files, the dbd)
 * come from on the Raspberry Pi. Overrides EPICS base's weak
 * epicsRtemsMountLocalFilesystem() in modules/libcom/RTEMS/posix/rtems_init.c.
 *
 * Two sources, tried in this order:
 *
 * 1. A file loaded into RAM by the Raspberry Pi firmware. config.txt:
 *
 *        initramfs epics.tar.gz 0x30000000
 *
 *    The firmware loads the file to that address and records the range in
 *    the device tree, /chosen linux,initrd-start and linux,initrd-end. The
 *    BSP copies the device tree at start (BSP_START_COPY_FDT_FROM_U_BOOT),
 *    so bsp_fdt_get() gives us the range and the exact size. The file is a
 *    gzip'ed (or plain) ustar archive with paths relative to "/":
 *    iocBoot/iocrpi3/st.cmd, db/<name>.db, dbd/rpi3.dbd. It is built by
 *    rpi3App/src/Makefile into bin/RTEMS-raspberrypi2/epics.tar.gz from
 *    the same file list as the compiled-in image, so editing a db or st.cmd
 *    only needs a new epics.tar.gz on the card, not a new kernel7.img.
 *
 *    The address must be explicit, not "followkernel": the firmware would
 *    then put the file right behind kernel7.img, inside the BSS/workspace
 *    area that the kernel zeroes at start. But no fixed address is safe
 *    for long: with CONFIGURE_UNIFIED_WORK_AREAS the RTEMS heap spans all
 *    ARM memory and the heap allocator carves allocations from the END of
 *    the free block, i.e. the heap grows DOWNWARD from the top of RAM
 *    (about 948 MB). By the time the network is up, allocations have
 *    reached below 0x30000000 and the tail of the archive is overwritten
 *    (seen 2026-10-15: "Zlib inflate failed" after the first files).
 *    Therefore ramfs_early_capture(), a sysinit item that runs right after
 *    the heap exists and before any driver, copies the archive into a
 *    malloc'ed buffer and records its crc32; the unpack later uses that
 *    copy and prints the checksums so damage would be visible.
 *
 * 2. The compiled-in epicsMemFs image (epicsRtemsFSImage.c, generated from
 *    rpi3_IMFS_FILES), unless the Makefile was built with RAMFS_EMBED = NO.
 *
 * In both cases argv[1] is pointed at the startup script inside the
 * in-memory file system. The stock version sets argv[1] = "/", which makes
 * main() call iocsh("/") (a no-op on a directory) and drop into the shell
 * with no records loaded. set_directory() chdirs to the script's directory,
 * so relative paths in st.cmd resolve.
 *
 * Keep STARTUP_SCRIPT in sync with rpi3_IMFS_FILES in src/Makefile.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rtems.h>
#include <rtems/sysinit.h>
#include <rtems/bspIo.h>
#include <rtems/untar.h>
#include <rtems/printer.h>
#include <zlib.h>                    /* crc32() */
#include <bsp/fdt.h>
#include <libfdt.h>

#include <epicsMemFs.h>

#define STARTUP_SCRIPT     "/iocBoot/iocrpi3/st.cmd"
#define RAMFS_INFLATE_BUF  (64 * 1024)      /* zlib output buffer */
#define RAMFS_MAX_SIZE     (256u * 1024 * 1024)

extern const epicsMemFS *epicsRtemsFSImage;

int epicsRtemsMountLocalFilesystem(char **argv);

/* A device-tree address cell: 4 or 8 bytes, big endian. */
static uint64_t
ramfs_prop_to_u64(const void *prop, int len)
{
    if (len == 8) {
        fdt64_t v;
        memcpy(&v, prop, sizeof v);
        return fdt64_to_cpu(v);
    }
    if (len == 4) {
        fdt32_t v;
        memcpy(&v, prop, sizeof v);
        return fdt32_to_cpu(v);
    }
    return 0;
}

/*
 * Did the firmware load an "initramfs" file? If so, return 0 and its RAM
 * range, taken from /chosen in the device tree.
 */
static int
ramfs_locate(uintptr_t *start, size_t *size)
{
    const void *fdt = bsp_fdt_get();
    const void *ps, *pe;
    int node, ls, le;
    uint64_t s, e;

    if (fdt == NULL || fdt_check_header(fdt) != 0)
        return -1;
    node = fdt_path_offset(fdt, "/chosen");
    if (node < 0)
        return -1;
    ps = fdt_getprop(fdt, node, "linux,initrd-start", &ls);
    pe = fdt_getprop(fdt, node, "linux,initrd-end", &le);
    if (ps == NULL || pe == NULL)
        return -1;
    s = ramfs_prop_to_u64(ps, ls);
    e = ramfs_prop_to_u64(pe, le);
    if (s == 0 || e <= s || (e - s) > RAMFS_MAX_SIZE) {
        printk("RAMFS: ignoring odd initrd range 0x%08llx..0x%08llx\n",
            (unsigned long long)s, (unsigned long long)e);
        return -1;
    }
    *start = (uintptr_t)s;
    *size = (size_t)(e - s);
    return 0;
}

/*
 * State of the firmware-loaded archive, filled in by ramfs_early_capture()
 * at RTEMS_SYSINIT_BSP_START (heap initialised, no driver and no thread has
 * allocated anything yet) and consumed by epicsRtemsMountLocalFilesystem().
 */
#define RAMFS_PAGE        4096u
#define RAMFS_MAX_PAGES   64u           /* page checksums kept for diagnosis */

static struct {
    uintptr_t src;                      /* where the firmware put it */
    size_t    size;
    void     *copy;                     /* our safe copy, NULL if malloc failed */
    uint32_t  crc_early;                /* crc32 of the original at capture */
    unsigned  npages;
    uint32_t  page_crc[RAMFS_MAX_PAGES];
} ramfs;

static void
ramfs_early_capture(void)
{
    uintptr_t start;
    size_t size, off;
    unsigned i;

    if (ramfs_locate(&start, &size) != 0)
        return;
    ramfs.src = start;
    ramfs.size = size;
    ramfs.crc_early = crc32(0, (const Bytef *)start, size);
    ramfs.npages = (unsigned)((size + RAMFS_PAGE - 1) / RAMFS_PAGE);
    if (ramfs.npages > RAMFS_MAX_PAGES)
        ramfs.npages = RAMFS_MAX_PAGES;
    for (i = 0, off = 0; i < ramfs.npages; i++, off += RAMFS_PAGE) {
        size_t n = size - off < RAMFS_PAGE ? size - off : RAMFS_PAGE;
        ramfs.page_crc[i] = crc32(0, (const Bytef *)(start + off), n);
    }
    ramfs.copy = malloc(size);
    if (ramfs.copy != NULL)
        memcpy(ramfs.copy, (const void *)start, size);
}

RTEMS_SYSINIT_ITEM(ramfs_early_capture, RTEMS_SYSINIT_BSP_START,
    RTEMS_SYSINIT_ORDER_LAST);

static void ramfs_dump_tail(void);

/* Report what happened to the original RAM range since the early capture. */
static void
ramfs_report(void)
{
    uint32_t crc_now = crc32(0, (const Bytef *)ramfs.src, ramfs.size);
    unsigned i;

    printf("RAMFS: %lu bytes at 0x%08lx, crc32 at boot 0x%08lx, now 0x%08lx, "
        "early copy %s\n", (unsigned long)ramfs.size, (unsigned long)ramfs.src,
        (unsigned long)ramfs.crc_early, (unsigned long)crc_now,
        ramfs.copy != NULL ? "ok" : "MISSING (malloc failed)");
    if (crc_now != ramfs.crc_early && ramfs.copy != NULL) {
        const uint8_t *a = (const uint8_t *)ramfs.src;
        const uint8_t *b = (const uint8_t *)ramfs.copy;
        size_t first = ramfs.size, last = 0, n = 0, k;
        for (k = 0; k < ramfs.size; k++) {
            if (a[k] != b[k]) {
                if (k < first) first = k;
                last = k;
                n++;
            }
        }
        printf("RAMFS: original overwritten after boot: %lu bytes differ "
            "between offsets %lu and %lu (RTEMS heap grows down from the top "
            "of RAM); unpacking the early copy\n", (unsigned long)n,
            (unsigned long)first, (unsigned long)last);
    }
    printf("RAMFS: page crc32 at boot:");
    for (i = 0; i < ramfs.npages; i++)
        printf(" %08lx", (unsigned long)ramfs.page_crc[i]);
    printf("\n");
    ramfs_dump_tail();
}

/*
 * Hex dump of the last 4 KB page of the firmware-loaded range plus the
 * 256 bytes after it, 32 bytes per line. Diagnostic used on 2026-10-15
 * when the firmware delivered the last partial 4 KB page of the file
 * damaged (fixed by padding the archive, see ramfsPad.pl); compile with
 * -DRAMFS_DUMP_TAIL=1 to switch it on again.
 */
#ifndef RAMFS_DUMP_TAIL
#define RAMFS_DUMP_TAIL 0
#endif
static void
ramfs_dump_tail(void)
{
#if RAMFS_DUMP_TAIL
    uintptr_t page = (ramfs.src + ramfs.size - 1) & ~(uintptr_t)(RAMFS_PAGE - 1);
    uintptr_t end = page + RAMFS_PAGE + 256;
    uintptr_t a;

    printf("RAMFS: dump of the last page 0x%08lx..0x%08lx (file ends at "
        "0x%08lx) and 256 bytes after it\n", (unsigned long)page,
        (unsigned long)(page + RAMFS_PAGE - 1),
        (unsigned long)(ramfs.src + ramfs.size - 1));
    for (a = page; a < end; a += 32) {
        const uint8_t *q = (const uint8_t *)a;
        int i;
        printf("%08lx:", (unsigned long)a);
        for (i = 0; i < 32; i++)
            printf("%s%02x", (i % 8) ? "" : " ", q[i]);
        printf("\n");
    }
#endif
}

/* Unpack a gzip'ed or plain ustar archive from RAM into the IMFS at "/". */
static int
ramfs_unpack(uintptr_t start, size_t size)
{
    const uint8_t *p = (const uint8_t *)start;
    rtems_printer printer;
    int rc;

    rtems_print_printer_printf(&printer);
    if (chdir("/") != 0) {
        printf("RAMFS: chdir(\"/\") failed\n");
        return -1;
    }

    if (size >= 2 && p[0] == 0x1f && p[1] == 0x8b) {
        Untar_GzChunkContext ctx;
        void *buf = malloc(RAMFS_INFLATE_BUF);

        if (buf == NULL) {
            printf("RAMFS: no memory for the inflate buffer\n");
            return -1;
        }
        rc = Untar_GzChunkContext_Init(&ctx, buf, RAMFS_INFLATE_BUF);
        if (rc == UNTAR_SUCCESSFUL)
            rc = Untar_FromGzChunk_Print(&ctx, (void *)start, size, &printer);
        free(buf);
    } else if (size >= 512 && memcmp(p + 257, "ustar", 5) == 0) {
        rc = Untar_FromMemory_Print((void *)start, size, &printer);
    } else {
        printf("RAMFS: data at 0x%08lx is neither gzip nor tar "
            "(starts with %02x %02x %02x %02x)\n",
            (unsigned long)start, p[0], p[1], p[2], p[3]);
        return -1;
    }
    if (rc != UNTAR_SUCCESSFUL) {
        printf("RAMFS: unpacking failed (untar status %d)\n", rc);
        return -1;
    }
    if (access(STARTUP_SCRIPT, R_OK) != 0) {
        printf("RAMFS: archive unpacked but %s is not in it\n", STARTUP_SCRIPT);
        return -1;
    }
    return 0;
}

int
epicsRtemsMountLocalFilesystem(char **argv)
{
    /* Base leaves the pointer at its own address when no image is linked. */
    const int have_embedded = epicsRtemsFSImage != NULL &&
        epicsRtemsFSImage != (const epicsMemFS *)&epicsRtemsFSImage;
    uintptr_t start;
    size_t size;

    if (ramfs.size != 0) {
        /* captured early by the sysinit item, the normal case */
        start = ramfs.copy != NULL ? (uintptr_t)ramfs.copy : ramfs.src;
        size = ramfs.size;
        printf("***** Using file data loaded by the firmware: %lu bytes at "
            "0x%08lx (config.txt initramfs) *****\n",
            (unsigned long)size, (unsigned long)ramfs.src);
        ramfs_report();
    } else if (ramfs_locate(&start, &size) == 0) {
        /* sysinit item did not run (should not happen); use the original */
        printf("***** Using file data loaded by the firmware: %lu bytes at "
            "0x%08lx (config.txt initramfs, no early copy) *****\n",
            (unsigned long)size, (unsigned long)start);
    } else {
        size = 0;
    }
    if (size != 0) {
        int ok = ramfs_unpack(start, size) == 0;
        if (ramfs.copy != NULL) {
            free(ramfs.copy);
            ramfs.copy = NULL;
        }
        if (ok) {
            argv[1] = STARTUP_SCRIPT;
            return 0;
        }
        printf("RAMFS: %s\n", have_embedded ?
            "falling back to the compiled in file data" :
            "no compiled in file data to fall back to");
    }
    if (size == 0 && !have_embedded) {
        printf("RAMFS: no initramfs in the device tree and no compiled in "
            "file data: add 'initramfs epics.tar.gz 0x30000000' to "
            "config.txt next to kernel7.img\n");
    }

    if (!have_embedded)
        return -1;

    printf("***** Using compiled in file data *****\n");
    if (epicsMemFsLoad(epicsRtemsFSImage) != 0) {
        printf("Can't unpack tar filesystem\n");
        return -1;
    }
    argv[1] = STARTUP_SCRIPT;
    return 0;
}
