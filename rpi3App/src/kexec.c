/*
 * kexec.c - warm restart of the Raspberry Pi 3B+ IOC into a kernel image
 * fetched over the network (iocsh command rpiKexec).
 *
 *   rpiKexec "192.168.2.10", "kernel7.img", 0     fetch, check, report
 *   rpiKexec "192.168.2.10", "kernel7.img", 1     fetch, check, boot it
 *
 * The firmware boots from the SD card whenever the card holds a kernel,
 * and RTEMS has no SD card driver on this board, so a new kernel normally
 * means a trip to the card slot. This command does what the firmware did
 * at power-on, from the running IOC:
 *
 *   1. mount RTEMS's TFTP client file system and read <file> from <host>
 *      into a RAM buffer (the laptop's dnsmasq serves /srv/tftp/rpi, see
 *      scripts/update_tftp.sh and the build guide section 10b);
 *   2. sanity-check the image (size, ARM vector table at the start,
 *      same first words as the running kernel at 0x200000) and print its
 *      crc32, so it can be compared with the file on the host;
 *   3. with boot=1: copy the firmware's device tree out of the kernel's
 *      .bss (the new kernel copies it again in start.S), re-arm the
 *      hardware watchdog with 15 s, disable interrupts, quiesce the
 *      interrupt controller, the ARM timer and the USB controller (soft
 *      reset, so no DMA runs into the old buffers), clean the data caches
 *      to RAM, and run a small position-independent stub from the heap.
 *      The stub turns MMU and caches off, copies the image to 0x200000
 *      (over the running kernel, which is why it cannot run from there),
 *      invalidates the instruction cache and jumps to the entry point
 *      0x200080 in ARM state with r0 = 0, r1 = machine type, r2 = device
 *      tree, exactly as the firmware does.
 *
 * Safety: the watchdog stays armed across the jump. If the new image does
 * not reach heartbeat_start() within 15 s, the board resets and the
 * firmware boots the kernel on the SD card again. The card is never
 * written, so the on-card kernel is always the fallback. The archive at
 * 0x30000000 (epics.tar.gz loaded by the firmware) is left in place; the
 * heap never grows that far (about 60 MB used of 938 MB).
 *
 * Limits: the new kernel lives in RAM until the next cold boot; a kernel
 * with a different load address or entry point is not supported (both
 * are checked against this kernel's values).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "iocsh.h"
#include "epicsExport.h"

#ifdef __rtems__

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zlib.h>
#include <rtems.h>
#include <rtems/libio.h>
#include <rtems/tftp.h>
#include <bsp.h>
#include <bsp/fdt.h>
#include <libfdt.h>

#define KEXEC_LOAD_ADDR   0x00200000u     /* config.txt kernel_address */
#define KEXEC_ENTRY       0x00200080u     /* _start of the RTEMS image */
#define KEXEC_MIN_SIZE    (1u << 20)
#define KEXEC_MAX_SIZE    (32u << 20)

/* ARM vector table entry "ldr pc, [pc, #24]" at the start of the image */
#define KEXEC_VECTOR_WORD 0xe59ff018u

/* BCM2837 peripherals (ARM physical view) */
#define PERIPH_BASE        0x3F000000u
#define INTC_FIQ_CTRL      (*(volatile uint32_t *)(PERIPH_BASE + 0xB20C))
#define INTC_DISABLE1      (*(volatile uint32_t *)(PERIPH_BASE + 0xB21C))
#define INTC_DISABLE2      (*(volatile uint32_t *)(PERIPH_BASE + 0xB220))
#define INTC_DISABLE_BASIC (*(volatile uint32_t *)(PERIPH_BASE + 0xB224))
#define ARM_TIMER_CTL      (*(volatile uint32_t *)(PERIPH_BASE + 0xB408))
#define USB_GAHBCFG        (*(volatile uint32_t *)(PERIPH_BASE + 0x980008))
#define USB_GRSTCTL        (*(volatile uint32_t *)(PERIPH_BASE + 0x980010))
#define USB_GINTMSK        (*(volatile uint32_t *)(PERIPH_BASE + 0x980018))
#define PM_RSTC            (*(volatile uint32_t *)(PERIPH_BASE + 0x10001c))
#define PM_WDOG            (*(volatile uint32_t *)(PERIPH_BASE + 0x100024))
#define PM_PASSWORD        0x5a000000u
#define PM_RSTC_WRCFG_CLR  0xffffffcfu
#define PM_RSTC_WRCFG_FULL 0x00000020u
#define WDOG_TICKS(s)      (((s) << 16) & 0x000fffffu)

/* Arguments of the stub, read with one ldmia: keep the order. */
struct kexec_params {
    uint32_t dst;        /* r4: load address */
    uint32_t src;        /* r5: image buffer */
    uint32_t size;       /* r6: bytes, multiple of 4, > 0 */
    uint32_t entry;      /* r7: entry point (ARM state) */
    uint32_t dtb;        /* r8: device tree for r2 */
};

/*
 * The stub. ARM state, no stack, no literal pool, no absolute references:
 * it is copied into a heap buffer and executed there, because the kernel
 * text it belongs to is overwritten by the copy loop.
 */
__asm__(
"    .pushsection .text.rpi_kexec_stub, \"ax\", %progbits\n"
"    .arm\n"
"    .align 2\n"
"    .globl rpi_kexec_stub_begin\n"
"    .globl rpi_kexec_stub_end\n"
"    .type rpi_kexec_stub_begin, %function\n"
"rpi_kexec_stub_begin:\n"
"    cpsid   if\n"
"    ldmia   r0, {r4, r5, r6, r7, r8}\n"
"    dsb\n"
"    mrc     p15, 0, r1, c1, c0, 0\n"       /* SCTLR */
"    bic     r1, r1, #0x00000005\n"         /* M (MMU), C (data cache) */
"    bic     r1, r1, #0x00001000\n"         /* I (instruction cache) */
"    bic     r1, r1, #0x00000800\n"         /* Z (branch prediction) */
"    mcr     p15, 0, r1, c1, c0, 0\n"
"    isb\n"
"    mov     r1, #0\n"
"    mcr     p15, 0, r1, c8, c7, 0\n"       /* TLBIALL */
"    mcr     p15, 0, r1, c7, c5, 0\n"       /* ICIALLU */
"    mcr     p15, 0, r1, c7, c5, 6\n"       /* BPIALL */
"    dsb\n"
"    isb\n"
"1:  ldr     r1, [r5], #4\n"
"    str     r1, [r4], #4\n"
"    subs    r6, r6, #4\n"
"    bne     1b\n"
"    dsb\n"
"    mcr     p15, 0, r6, c7, c5, 0\n"       /* ICIALLU, r6 is 0 here */
"    mcr     p15, 0, r6, c7, c5, 6\n"       /* BPIALL */
"    dsb\n"
"    isb\n"
"    mov     r0, #0\n"
"    movw    r1, #0x0c42\n"                 /* machine type, as the firmware */
"    mov     r2, r8\n"                      /* device tree */
"    bx      r7\n"
"rpi_kexec_stub_end:\n"
"    .size rpi_kexec_stub_begin, . - rpi_kexec_stub_begin\n"
"    .popsection\n"
"    .thumb\n"
);

extern const uint8_t rpi_kexec_stub_begin[];
extern const uint8_t rpi_kexec_stub_end[];

static int
kexec_mount_tftp(void)
{
    /* The device string must not be NULL: rtems_tftpfs_initialize() runs
     * strlen() on it (data abort on the Pi, 2026-10-20). An empty string
     * means "no fixed server", the server is then part of every path
     * (/TFTP/<host>:<file>). */
    /* A second call finds /TFTP mounted already: EBUSY or EEXIST
     * (seen 2026-10-20: "File exists" from the second rpiKexec). */
    if (mount_and_make_target_path("", "/TFTP",
            RTEMS_FILESYSTEM_TYPE_TFTPFS, RTEMS_FILESYSTEM_READ_ONLY,
            NULL) != 0 && errno != EBUSY && errno != EEXIST) {
        printf("rpiKexec: mount tftpfs on /TFTP: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Read /TFTP/<host>:<file> into a malloc'd buffer; returns size or 0. */
static size_t
kexec_fetch(const char *host, const char *file, uint8_t **out)
{
    char path[256];
    int fd;
    size_t cap = 12u << 20, len = 0, next_report = 1u << 20;
    uint8_t *buf;
    const rtems_interval tps = rtems_clock_get_ticks_per_second();
    rtems_interval t0 = rtems_clock_get_ticks_since_boot();

    if (kexec_mount_tftp() != 0)
        return 0;
    snprintf(path, sizeof path, "/TFTP/%s:%s", host, file);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("rpiKexec: open %s: %s\n", path, strerror(errno));
        return 0;
    }
    buf = malloc(cap);
    if (buf == NULL) {
        printf("rpiKexec: no memory for the image buffer\n");
        close(fd);
        return 0;
    }
    printf("rpiKexec: reading %s", path);
    fflush(stdout);
    for (;;) {
        ssize_t n;
        if (len == cap) {
            uint8_t *nb;
            if (cap >= KEXEC_MAX_SIZE) {
                printf("\nrpiKexec: image larger than %u MB, giving up\n",
                       KEXEC_MAX_SIZE >> 20);
                free(buf);
                close(fd);
                return 0;
            }
            cap *= 2;
            nb = realloc(buf, cap);
            if (nb == NULL) {
                printf("\nrpiKexec: no memory to grow the image buffer\n");
                free(buf);
                close(fd);
                return 0;
            }
            buf = nb;
        }
        n = read(fd, buf + len, cap - len);
        if (n < 0) {
            printf("\nrpiKexec: read: %s after %u bytes\n", strerror(errno),
                   (unsigned)len);
            free(buf);
            close(fd);
            return 0;
        }
        if (n == 0)
            break;
        len += (size_t)n;
        if (len >= next_report) {
            printf(" %uM", (unsigned)(len >> 20));
            fflush(stdout);
            next_report += 1u << 20;
        }
    }
    close(fd);
    printf("\nrpiKexec: %u bytes in %lu s\n", (unsigned)len,
           (unsigned long)((rtems_clock_get_ticks_since_boot() - t0) / tps));
    *out = buf;
    return len;
}

static int
kexec_check(const uint8_t *img, size_t len, int force)
{
    const uint32_t *w = (const uint32_t *)img;
    const uint32_t *running = (const uint32_t *)KEXEC_LOAD_ADDR;
    uint32_t crc = crc32(0, img, len);
    int ok = 1;

    printf("rpiKexec: image crc32 %08lx (compare with the host: "
           "scripts/crc32.py <file>)\n", (unsigned long)crc);
    if (len < KEXEC_MIN_SIZE || len > KEXEC_MAX_SIZE) {
        printf("rpiKexec: size %u bytes is not a kernel image\n", (unsigned)len);
        ok = 0;
    }
    if (len >= 32 && w[0] != KEXEC_VECTOR_WORD) {
        printf("rpiKexec: first word %08lx is not an ARM vector entry "
               "(%08lx)\n", (unsigned long)w[0],
               (unsigned long)KEXEC_VECTOR_WORD);
        ok = 0;
    }
    if (len >= 32 && memcmp(w, running, 32) != 0) {
        printf("rpiKexec: the first 32 bytes differ from the running kernel "
               "at 0x%08x\n", KEXEC_LOAD_ADDR);
        ok = 0;
    }
    if (!ok && force) {
        printf("rpiKexec: force set, booting anyway\n");
        ok = 1;
    }
    return ok ? 0 : -1;
}

static void
kexec_boot(uint8_t *img, size_t len, size_t cap)
{
    const void *fdt = bsp_fdt_get();
    size_t fdt_size = 0, stub_size, copy_size;
    void *dtb, *stub;
    struct kexec_params *p;
    void (*run)(struct kexec_params *);
    rtems_interrupt_level level;
    uintptr_t stub_addr;
    int i;

    if (fdt != NULL && fdt_check_header(fdt) == 0)
        fdt_size = fdt_totalsize(fdt);
    if (fdt_size == 0) {
        printf("rpiKexec: no device tree to hand over, not booting\n");
        return;
    }
    copy_size = (len + 3) & ~(size_t)3;
    if (copy_size > cap) {
        printf("rpiKexec: image buffer too small to pad, not booting\n");
        return;
    }
    dtb = malloc(fdt_size + 64);
    stub_size = (size_t)(rpi_kexec_stub_end - rpi_kexec_stub_begin);
    stub = malloc(stub_size + 64);
    p = malloc(sizeof *p + 64);
    if (dtb == NULL || stub == NULL || p == NULL) {
        printf("rpiKexec: no memory for the hand-over buffers\n");
        return;
    }
    memcpy(dtb, fdt, fdt_size);

    stub_addr = ((uintptr_t)stub + 7) & ~(uintptr_t)7;
    memcpy((void *)stub_addr, rpi_kexec_stub_begin, stub_size);
    run = (void (*)(struct kexec_params *))stub_addr;   /* bit 0 clear: ARM */

    if (copy_size > len)
        memset(img + len, 0, copy_size - len);
    p->dst = KEXEC_LOAD_ADDR;
    p->src = (uint32_t)(uintptr_t)img;
    p->size = (uint32_t)copy_size;
    p->entry = KEXEC_ENTRY;
    p->dtb = (uint32_t)(uintptr_t)dtb;

    printf("rpiKexec: booting %u bytes at 0x%08x, entry 0x%08x, device tree "
           "%u bytes at %p, stub %u bytes at %p\n", (unsigned)copy_size,
           KEXEC_LOAD_ADDR, KEXEC_ENTRY, (unsigned)fdt_size, dtb,
           (unsigned)stub_size, (void *)stub_addr);
    printf("rpiKexec: watchdog re-armed with 15 s; if the new kernel does not "
           "come up, the board resets to the SD card kernel\n");
    fflush(stdout);
    rtems_task_wake_after(rtems_clock_get_ticks_per_second() / 4);  /* drain the console */

    rtems_interrupt_local_disable(level);
    (void)level;

    /* Watchdog: fresh 15 s window for the new kernel to reach heartbeat_start. */
    PM_WDOG = PM_PASSWORD | WDOG_TICKS(15u);
    PM_RSTC = PM_PASSWORD | (PM_RSTC & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL;

    /* No more interrupts from the controller, the ARM timer or the USB core. */
    INTC_FIQ_CTRL = 0;
    INTC_DISABLE1 = 0xffffffffu;
    INTC_DISABLE2 = 0xffffffffu;
    INTC_DISABLE_BASIC = 0xffffffffu;
    ARM_TIMER_CTL = 0;
    USB_GAHBCFG = 0;                 /* global interrupt mask off, DMA off */
    USB_GINTMSK = 0;
    USB_GRSTCTL = 1;                 /* core soft reset: stops all channels */
    for (i = 0; i < 1000000 && (USB_GRSTCTL & 1u); ++i)
        ;

    /* Everything the stub reads must be in RAM before caches go off. */
    rtems_cache_flush_entire_data();
    rtems_cache_invalidate_entire_instruction();

    run(p);
    /* not reached */
}

static void
rpiKexec(const char *host, const char *file, int boot)
{
    uint8_t *img = NULL;
    size_t len;
    int force = boot > 1;

    if (host == NULL || *host == '\0' || file == NULL || *file == '\0') {
        printf("usage: rpiKexec \"host\", \"file\" [, boot]   boot: 0 = fetch and "
               "check (default), 1 = fetch, check and boot, 2 = boot even if "
               "the checks fail\n");
        return;
    }
    len = kexec_fetch(host, file, &img);
    if (len == 0)
        return;
    if (kexec_check(img, len, force) != 0) {
        printf("rpiKexec: checks failed, not booting (boot=2 overrides)\n");
        free(img);
        return;
    }
    if (boot <= 0) {
        printf("rpiKexec: checks passed; use boot=1 to start this image\n");
        free(img);
        return;
    }
    /* the fetch buffer is at least 12 MB, so padding to 4 bytes fits */
    kexec_boot(img, len, len < (12u << 20) ? (12u << 20) : KEXEC_MAX_SIZE);
    free(img);      /* only reached if kexec_boot refused */
}

#else /* host build: command exists, does nothing */

static void
rpiKexec(const char *host, const char *file, int boot)
{
    (void)host; (void)file; (void)boot;
    printf("rpiKexec: only on the RTEMS Raspberry Pi build\n");
}

#endif

static const iocshArg kexecArg0 = { "host", iocshArgString };
static const iocshArg kexecArg1 = { "file", iocshArgString };
static const iocshArg kexecArg2 = { "boot (0 check, 1 boot, 2 force)", iocshArgInt };
static const iocshArg * const kexecArgs[] = { &kexecArg0, &kexecArg1, &kexecArg2 };
static const iocshFuncDef kexecFuncDef = { "rpiKexec", 3, kexecArgs,
    "Fetch a kernel image over TFTP and warm-restart the board into it.\n"
    "rpiKexec \"192.168.2.10\", \"kernel7.img\"      fetch and check only\n"
    "rpiKexec \"192.168.2.10\", \"kernel7.img\", 1   fetch, check and boot\n" };

static void
kexecCallFunc(const iocshArgBuf *args)
{
    rpiKexec(args[0].sval, args[1].sval, args[2].ival);
}

static void
rpiKexecRegistrar(void)
{
    iocshRegister(&kexecFuncDef, kexecCallFunc);
}
epicsExportRegistrar(rpiKexecRegistrar);
