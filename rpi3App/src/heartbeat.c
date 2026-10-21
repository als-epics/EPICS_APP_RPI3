/*
 * heartbeat.c - BCM2835 hardware watchdog keeper (production) and,
 * with RPI_IOC_DEBUG, the one-line-per-second liveness heartbeat used
 * during the 2026 hang hunt.
 *
 * Production behaviour:
 *   - at start: print the previous reset reason (PM_RSTS) once, so a
 *     watchdog reboot can be recognised in the boot log afterwards;
 *   - arm the PM watchdog (HB_WDOG_SECS) and pet it once a second from a
 *     high-priority task.  If the IOC ever freezes, the board reboots by
 *     itself instead of waiting for someone to cycle the power;
 *   - iocsh:  var hb_wdog_enable 0   disarms it (e.g. before stopping the
 *             IOC in a debugger or running a long blocking shell command),
 *             var hb_wdog_enable 1   re-arms it.
 *   Nothing else is printed.
 *
 * Debug behaviour (RPI_IOC_DEBUG, see rpi3App/src/Makefile): additionally
 * prints "[HB n=N up=Xs fiq=N irq=mode]" every second via printk, writes a
 * marker into the libbsd RAM crash ring and starts the FIQ GPIO toggler.
 */
#include <rtems.h>
#include <rtems/bspIo.h>
#include <stdint.h>

#define PM_BASE        0x3F100000u
#define PM_RSTC        (*(volatile uint32_t *)(PM_BASE + 0x1c))
#define PM_RSTS        (*(volatile uint32_t *)(PM_BASE + 0x20))
#define PM_WDOG        (*(volatile uint32_t *)(PM_BASE + 0x24))
#define PM_PASSWORD    0x5a000000u
#define PM_RSTC_WRCFG_CLR  0xffffffcfu
#define PM_RSTC_WRCFG_FULL 0x00000020u
#define PM_RSTC_RESET      0x00000102u
#define HB_WDOG_SECS   8u
#define WDOG_TICKS(s)  (((s) << 16) & 0x000fffffu)   /* 16 us units */

volatile int hb_wdog_enable = 1;

static void
hb_wdog_pet(void)
{
    uint32_t rstc;

    PM_WDOG = PM_PASSWORD | WDOG_TICKS(HB_WDOG_SECS);
    rstc = PM_RSTC & PM_RSTC_WRCFG_CLR;
    PM_RSTC = PM_PASSWORD | rstc | PM_RSTC_WRCFG_FULL;
}

static void
hb_wdog_stop(void)
{
    PM_RSTC = PM_PASSWORD | PM_RSTC_RESET;
}

#ifdef RPI_IOC_DEBUG
extern void dwc_otg_ring_mark(uint32_t tag16, uint32_t val);  /* libbsd dwc_otg.c */
extern const char *dwc_irqmask_mode_string(void);            /* irqwrap.c */
extern volatile uint32_t fiq_gpio_count;                      /* fiq_gpio_toggle.c */
extern void fiq_gpio_toggle_init(void);
#endif

static rtems_task
heartbeat_task(rtems_task_argument arg)
{
    (void)arg;
    const rtems_interval one_sec = rtems_clock_get_ticks_per_second();
    int wdog_on = 0;
#ifdef RPI_IOC_DEBUG
    const rtems_interval start = rtems_clock_get_ticks_since_boot();
    uint32_t i = 0;
#endif

    /*
     * PM_RSTS "HAD" flags (BCM2835, same bit names as the Linux
     * bcm2835-pm driver): 0x1000 power-on reset, 0x40/0x20/0x10
     * watchdog reset, 0x400/0x200/0x100 software reset, 0x4/0x2/0x1
     * debugger reset. Observed on the Pi 3B+: 0x1000 after power-up or
     * an SD-card swap, 0x0 after a reset caused by this watchdog (the
     * GPU firmware appears to clear the flags on that path).
     */
    {
        uint32_t rsts = PM_RSTS;
        const char *why = (rsts & 0x1000u) ? " (power-on reset)" :
                          (rsts & 0x0070u) ? " (watchdog reset)" :
                          (rsts & 0x0700u) ? " (software reset)" :
                          (rsts & 0x0007u) ? " (debugger reset)" :
                          " (no reason flag; after a watchdog reboot the"
                          " firmware leaves this at 0)";
        printk("WDOG: previous reset status PM_RSTS=0x%08x%s\n",
            (unsigned)rsts, why);
    }

    for (;;) {
        if (hb_wdog_enable) {
            hb_wdog_pet();
            if (!wdog_on) {
                printk("WDOG: hardware watchdog armed (%u s); "
                    "'var hb_wdog_enable 0' disarms it\n", HB_WDOG_SECS);
                wdog_on = 1;
            }
        } else if (wdog_on) {
            hb_wdog_stop();
            printk("WDOG: hardware watchdog disarmed\n");
            wdog_on = 0;
        }
#ifdef RPI_IOC_DEBUG
        {
            rtems_interval now = rtems_clock_get_ticks_since_boot();
            uint32_t up_s = (now - start) / one_sec;
            dwc_otg_ring_mark(0x4842, ++i);     /* 'HB' marker, val = n */
            printk("[HB n=%u up=%us fiq=%u irq=%s]\n",
                (unsigned)i, (unsigned)up_s, (unsigned)fiq_gpio_count,
                dwc_irqmask_mode_string());
        }
#endif
        rtems_task_wake_after(one_sec);
    }
}

void
heartbeat_start(void)
{
    rtems_id          tid;
    rtems_status_code sc;

#ifdef RPI_IOC_DEBUG
    fiq_gpio_toggle_init();
#endif

    sc = rtems_task_create(
        rtems_build_name('W','D','O','G'),
        100,
        RTEMS_MINIMUM_STACK_SIZE * 4,
        RTEMS_DEFAULT_MODES,
        RTEMS_DEFAULT_ATTRIBUTES,
        &tid);
    if (sc != RTEMS_SUCCESSFUL) {
        printk("WDOG: rtems_task_create failed: %d\n", (int)sc);
        return;
    }
    sc = rtems_task_start(tid, heartbeat_task, 0);
    if (sc != RTEMS_SUCCESSFUL)
        printk("WDOG: rtems_task_start failed: %d\n", (int)sc);
}
