/*
 * irqwrap.c - 2026-10-13 hang diagnosis, revision 2.
 *
 * The RTEMS interrupt server writes the BCM2835 interrupt controller
 * (DISABLE_IRQS_1 in the ISR, ENABLE_IRQS_1 after the handler) around
 * every USB interrupt (vector 9).  The third crash-ring capture of
 * 2026-10-13 ended with marker 0x4945 (ENABLE_IRQS_1 write) PENDING while
 * every DWC OTG register access before it had completed: the access that
 * stalls the whole SoC is the interrupt-controller write, not a USB
 * register access.
 *
 * Linked with -Wl,--wrap=bsp_interrupt_vector_enable and
 * -Wl,--wrap=bsp_interrupt_vector_disable, so every call from the RTEMS
 * interrupt server (and the one-time enable at handler install) comes
 * through here.  Two modes, selected by dwc_otg_icmask_enable:
 *
 *   "core" mode (1): the interrupt-controller bit for the USB vector is
 *      enabled once and never touched again; the per-interrupt mask /
 *      unmask is done in the DWC core (GAHBCFG.GlblIntrMsk) via
 *      dwc_otg_rtems_core_irq() in dwc_otg.c.  No interrupt-controller
 *      write happens in the USB hot path any more.  If the hang goes
 *      away, this is the workaround.
 *
 *   "ic" mode (0): original behaviour (interrupt-controller writes) with a
 *      three-marker probe, run with IRQs locally masked so no exception
 *      can be taken in the middle:
 *         MK 0x4945 / 0x4944  = ENABLE / DISABLE write + dsb
 *         MK 0x4946 / 0x4947  = read of IRQ_PENDING1 right after it
 *                               (val = pending bits)
 *      0x4945 PENDING -> the write (its completion) stalls
 *      0x4946 PENDING -> a controller READ after a completed write stalls
 *      both done, nothing after -> stall is in the exception entry /
 *                               BSP dispatcher (its IRQ_BASIC/PENDING1
 *                               reads) or later
 *
 *   auto (-1, default): odd boot number (1st boot after power-up, 3rd, ...)
 *      runs "core", even boot number (after a watchdog reset) runs "ic".
 *      One SD card flash therefore yields both experiments: if boot #1
 *      hangs, the dump printed by boot #2 shows where the stall moved to,
 *      and boot #2 itself runs the three-marker probe.
 *
 *   iocsh: var dwc_otg_icmask_enable 0|1|-1  (takes effect immediately)
 */
#include <rtems.h>
#include <rtems/bspIo.h>
#include <stdint.h>

extern void *dwc_otg_ring_mark_begin(uint32_t tag16, uint32_t val);
extern void dwc_otg_ring_mark_end(void *handle);
extern void dwc_otg_ring_mark_end_val(void *handle, uint32_t val);
extern uint32_t dwc_otg_ring_boot_count(void);
extern void dwc_otg_rtems_core_irq(int enable);

extern rtems_status_code __real_bsp_interrupt_vector_enable(rtems_vector_number v);
extern rtems_status_code __real_bsp_interrupt_vector_disable(rtems_vector_number v);

/* BCM2835 interrupt controller (RPi 2/3 peripheral base 0x3F000000) */
#define BCM2835_INTC_BASE   0x3F00B200u
#define IC_IRQ_PENDING1     (*(volatile uint32_t *)(BCM2835_INTC_BASE + 0x04))
#define USB_IRQ_VECTOR      9u

volatile int dwc_otg_icmask_enable = 0;     /* -1 auto, 0 ic (production path), 1 core */

static int ic_enabled_once;     /* IRQ-ctrl bit for USB vector set at least once */
static int ic_disabled;         /* IRQ-ctrl bit currently cleared by "ic" mode */
static int core_masked;         /* GAHBCFG.GlblIntrMsk currently cleared */
static int mode_reported = -2;  /* last mode printed on the console */

/* 1 = core mode, 0 = ic mode. Falls back to ic mode until the ring is up. */
static int
core_mode_now(void)
{
    int m = dwc_otg_icmask_enable;
    uint32_t boots = dwc_otg_ring_boot_count();

    if (m < 0) {
        if (boots == 0)
            m = 0;
        else
            m = (int)(boots & 1u);
    }
    if (m != mode_reported && boots != 0) {
        mode_reported = m;
        printk("IRQWRAP: boot #%u -> USB IRQ mask mode = %s\n",
            (unsigned)boots, m ? "core (GAHBCFG, IRQ-ctrl untouched)"
                               : "ic (plain RTEMS IRQ-ctrl path)");
    }
    return m;
}

const char *
dwc_irqmask_mode_string(void)
{
    return core_mode_now() ? "core" : "ic";
}

/*
 * "ic" mode, soak-build version (2026-10-13 evening): the plain RTEMS
 * path, exactly what production will run, with only a ring marker around
 * the interrupt-controller write.  (The IRQ-masked 3-marker probe used
 * during the diagnosis is no longer needed; the root cause was found in
 * bsp_interrupt_dispatch().)  Set dwc_otg_icmask_probe = 1 to get the
 * old probe back.
 */
volatile int dwc_otg_icmask_probe = 0;

static rtems_status_code
ic_probe_enable(rtems_vector_number v)
{
    rtems_interrupt_level level;
    rtems_status_code sc;
    void *h;
    uint32_t pend;

    if (!dwc_otg_icmask_probe) {
        h = dwc_otg_ring_mark_begin(0x4945, v);
        sc = __real_bsp_interrupt_vector_enable(v);
        dwc_otg_ring_mark_end(h);
        return sc;
    }
    rtems_interrupt_local_disable(level);
    h = dwc_otg_ring_mark_begin(0x4945, v);
    sc = __real_bsp_interrupt_vector_enable(v);
    __asm__ volatile ("dsb" ::: "memory");
    dwc_otg_ring_mark_end(h);
    h = dwc_otg_ring_mark_begin(0x4946, v);
    pend = IC_IRQ_PENDING1;
    dwc_otg_ring_mark_end_val(h, pend);
    rtems_interrupt_local_enable(level);
    return sc;
}

static rtems_status_code
ic_probe_disable(rtems_vector_number v)
{
    rtems_interrupt_level level;
    rtems_status_code sc;
    void *h;
    uint32_t pend;

    if (!dwc_otg_icmask_probe) {
        h = dwc_otg_ring_mark_begin(0x4944, v);
        sc = __real_bsp_interrupt_vector_disable(v);
        dwc_otg_ring_mark_end(h);
        return sc;
    }
    rtems_interrupt_local_disable(level);
    h = dwc_otg_ring_mark_begin(0x4944, v);
    sc = __real_bsp_interrupt_vector_disable(v);
    __asm__ volatile ("dsb" ::: "memory");
    dwc_otg_ring_mark_end(h);
    h = dwc_otg_ring_mark_begin(0x4947, v);
    pend = IC_IRQ_PENDING1;
    dwc_otg_ring_mark_end_val(h, pend);
    rtems_interrupt_local_enable(level);
    return sc;
}

rtems_status_code
__wrap_bsp_interrupt_vector_enable(rtems_vector_number v)
{
    rtems_status_code sc = RTEMS_SUCCESSFUL;

    if (v != USB_IRQ_VECTOR) {
        void *h = dwc_otg_ring_mark_begin(0x4945, v);
        sc = __real_bsp_interrupt_vector_enable(v);
        __asm__ volatile ("dsb" ::: "memory");
        dwc_otg_ring_mark_end(h);
        return sc;
    }

    if (core_mode_now() && ic_enabled_once && !ic_disabled) {
        /* core mode: unmask in the DWC core, controller bit stays set */
        void *h = dwc_otg_ring_mark_begin(0x4943, 1);
        dwc_otg_rtems_core_irq(1);
        core_masked = 0;
        dwc_otg_ring_mark_end(h);
        return RTEMS_SUCCESSFUL;
    }

    /* ic mode, first-ever enable, or mode switched while the IRQ-ctrl bit
     * is still cleared: write the interrupt controller. */
    sc = ic_probe_enable(v);
    ic_enabled_once = 1;
    ic_disabled = 0;
    if (core_masked) {
        /* mode switched core->ic between ISR and handler end: undo mask */
        dwc_otg_rtems_core_irq(1);
        core_masked = 0;
    }
    return sc;
}

rtems_status_code
__wrap_bsp_interrupt_vector_disable(rtems_vector_number v)
{
    rtems_status_code sc;

    if (v != USB_IRQ_VECTOR) {
        void *h = dwc_otg_ring_mark_begin(0x4944, v);
        sc = __real_bsp_interrupt_vector_disable(v);
        __asm__ volatile ("dsb" ::: "memory");
        dwc_otg_ring_mark_end(h);
        return sc;
    }

    if (core_mode_now() && ic_enabled_once) {
        void *h = dwc_otg_ring_mark_begin(0x4943, 0);
        dwc_otg_rtems_core_irq(0);
        core_masked = 1;
        dwc_otg_ring_mark_end(h);
        return RTEMS_SUCCESSFUL;
    }

    sc = ic_probe_disable(v);
    ic_disabled = 1;
    return sc;
}
