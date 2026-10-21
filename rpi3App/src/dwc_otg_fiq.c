/*
 * DWC OTG FIQ FSM — Layer A: --wrap functions + Phase 1 stub FSM.
 *
 * This file provides:
 *   1. __wrap_bsp_interrupt_facility_initialize — programs FIQ_CTRL
 *   2. __wrap_rtems_interrupt_server_handler_install — intercepts USB IRQ 9
 *   3. dwc_otg_fiq_handler — C entry called from inline asm FIQ vector
 *   4. Phase 1 stub FSM: reads GINTSTS, raises SWIRQ, passes through
 *   5. SWIRQ handler (GPU timer C3 or MPHI) that calls the original driver
 *   6. FIQ circular debug buffer drain (called from the existing 1-Hz diag)
 *
 * Phase 1 behaviour: FIQ fires on USB activity, immediately raises SWIRQ.
 * The SWIRQ handler calls the original filter + handler.  No FSM logic yet.
 * This is a pure dispatch-path reroute that validates the plumbing.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef __rtems__

/*
 * FIQ vector entry — inline asm replaces dwc_otg_fiq_entry.S.
 * EPICS build system has no .S rule, so we embed the asm here.
 *
 * On ARM FIQ entry: r8-r12 banked, r0-r7 shared with interrupted mode.
 * Save r0-r3 (AAPCS caller-saved, shared) + lr_fiq (clobbered by bl).
 * C callee preserves banked r8-r12 per AAPCS.
 *
 * SPSR_fiq save/restore: the C handler is Thumb code; its epilogue does
 * ldmia {.., pc} which interworks Thumb→ARM.  On Cortex-A53 AArch32 this
 * can corrupt SPSR_fiq (clearing T-bit).  We save SPSR before the call
 * and restore it after, so subs pc,lr,#4 always uses the correct value.
 */
__asm__ (
    ".syntax unified\n"
    ".arm\n"
    ".section .text.__wrap__ARMV4_Exception_fiq_default, \"ax\"\n"
    ".global __wrap__ARMV4_Exception_fiq_default\n"
    ".type   __wrap__ARMV4_Exception_fiq_default, %function\n"
    "__wrap__ARMV4_Exception_fiq_default:\n"
    "    push    {r0-r3, lr}\n"
    "    mrs     r0, spsr\n"
    "    push    {r0}\n"
    "    bl      dwc_otg_fiq_handler\n"
    "    pop     {r0}\n"
    "    mrs     r1, spsr\n"
    "    msr     spsr_cxsf, r0\n"
    "    cmp     r0, r1\n"
    "    ldrne   r2, .Lfix_addr\n"
    "    ldrne   r3, [r2]\n"
    "    addne   r3, r3, #1\n"
    "    strne   r3, [r2]\n"
    "    pop     {r0-r3, lr}\n"
    "    subs    pc, lr, #4\n"
    ".Lfix_addr:\n"
    "    .word   g_fiq_spsr_fix_count\n"
    ".size   __wrap__ARMV4_Exception_fiq_default, . - __wrap__ARMV4_Exception_fiq_default\n"
);

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <rtems.h>
#include <rtems/rtems/intr.h>
#include <bsp/irq.h>
#include <bsp/raspberrypi.h>

volatile uint32_t g_fiq_spsr_fix_count;

#include "dwc_otg_fiq_state.h"

/* ================================================================== */
/* Globals                                                             */
/* ================================================================== */

struct dwc_fiq_state g_fiq_state;  /* exposed for heartbeat.c diagnostic */

/*
 * Captured from the nexus bus_setup_intr path.
 * Layout matches rtems-kernel-nexus.c struct nexus_intr.
 */
struct nexus_intr_layout {
    void *filt;   /* driver_filter_t * */
    void *intr;   /* driver_intr_t *   */
    void *arg;    /* softc             */
};

/* Original filter + handler + arg captured at registration time. */
static int  (*g_dwc_filter)(void *);
static void (*g_dwc_handler)(void *);
static void  *g_dwc_arg;

/* The original handler wrapper passed by nexus (for SWIRQ forwarding). */
static rtems_interrupt_handler g_nexus_handler;
static void                   *g_nexus_arg;

/* ================================================================== */
/* SWIRQ mechanism — GPU system timer channel 3 (fallback, Phase 1).   */
/* MPHI can be added in Phase 1b after verifying peripheral access.    */
/* ================================================================== */

static void dwc_otg_swirq_handler(void *arg)
{
    (void)arg;

    /* Clear GPU timer M1 match pending (W1C). */
    BCM2835_REG(BCM2835_GPU_TIMER_CS) = BCM2835_GPU_TIMER_CS_M1;

    g_fiq_state.swirq_count++;

    if (g_nexus_handler != NULL) {
        g_nexus_handler(g_nexus_arg);
    }

    /*
     * Re-enable FIQ at the INTC.  If USB interrupt is still asserted
     * (GINTSTS & GINTMSK != 0), FIQ fires immediately after return.
     * Order: clear irq_pending, barrier, then re-enable FIQ source.
     */
    g_fiq_state.irq_pending = 0;
    dwc_fiq_dmb();
    BCM2835_REG(BCM2835_IRQ_FIQ_CTRL) = 0x80u | BCM2835_IRQ_ID_USB;
}

/*
 * Raise SWIRQ from FIQ context (no RTEMS API).
 * Writes GPU timer C1 with (CLO + 1) to trigger match IRQ in ~1 µs.
 * (C3 is used by the RTEMS clock driver.)
 */
static inline void dwc_fiq_raise_swirq(void)
{
    uint32_t now = DWC_FIQ_REG(DWC_FIQ_TIMER_CLO);
    DWC_FIQ_REG(DWC_FIQ_TIMER_C1) = now + 20u;
}

/* ================================================================== */
/* FSM helper: restart a channel (write HCCHAR with CHENA).            */
/* ================================================================== */

static inline void
dwc_fiq_fsm_restart_channel(struct dwc_fiq_state *st, int ch)
{
    uint32_t hcchar = DWC_FIQ_REG(st->usb_base + DWC_FIQ_HCCHAR(ch));
    uint32_t hfnum  = DWC_FIQ_REG(st->usb_base + DWC_FIQ_HFNUM);

    /* Set oddfrm based on current frame parity. */
    if (hfnum & 1u)
        hcchar |= DWC_FIQ_HCCHAR_ODDFRM;
    else
        hcchar &= ~DWC_FIQ_HCCHAR_ODDFRM;

    hcchar |= DWC_FIQ_HCCHAR_CHENA;
    hcchar &= ~DWC_FIQ_HCCHAR_CHDIS;
    DWC_FIQ_REG(st->usb_base + DWC_FIQ_HCCHAR(ch)) = hcchar;
}

/* ================================================================== */
/* FSM helper: configure channel for CSPLIT.                           */
/* ================================================================== */

static inline void
dwc_fiq_fsm_setup_csplit(struct dwc_fiq_state *st, int ch)
{
    uint32_t hcsplt;

    /* Clear previous HCINT. */
    DWC_FIQ_REG(st->usb_base + DWC_FIQ_HCINT(ch)) = 0xFFFFFFFFu;

    /* Set COMPSPLT in HCSPLT. */
    hcsplt = st->channel[ch].hcsplt_copy;
    hcsplt |= DWC_FIQ_HCSPLT_COMPSPLT;
    DWC_FIQ_REG(st->usb_base + DWC_FIQ_HCSPLT(ch)) = hcsplt;

    dwc_fiq_fsm_restart_channel(st, ch);
}

/* ================================================================== */
/* FSM: handle per-channel HCINT for periodic split states.            */
/* Returns 1 if channel reached a terminal state, 0 if still active.   */
/* ================================================================== */

static int
dwc_fiq_fsm_do_hcintr(struct dwc_fiq_state *st, int ch, uint32_t hcint)
{
    struct dwc_fiq_channel_state *chst = &st->channel[ch];

    switch (chst->fsm) {
    case FIQ_PER_SSPLIT_STARTED:
        if (hcint & DWC_FIQ_HCINT_ACK) {
            chst->fsm = FIQ_PER_CSPLIT_WAIT;
            chst->nyet_count = 0;
            chst->csplit_count = 0;
            return 0;
        }
        if (hcint & DWC_FIQ_HCINT_NAK) {
            chst->fsm = FIQ_PER_SPLIT_DONE;
            chst->hcint_copy = hcint;
            return 1;
        }
        if (hcint & (DWC_FIQ_HCINT_XACTERR | DWC_FIQ_HCINT_BBLERR |
                     DWC_FIQ_HCINT_STALL | DWC_FIQ_HCINT_FRMOVRUN)) {
            chst->nr_errors++;
            if (chst->nr_errors >= DWC_FIQ_MAX_ERRORS) {
                chst->fsm = FIQ_PER_SPLIT_HS_ABORTED;
                chst->hcint_copy = hcint;
                return 1;
            }
            /* Retry SSPLIT: clear HCINT, restart. */
            DWC_FIQ_REG(st->usb_base + DWC_FIQ_HCINT(ch)) = 0xFFFFFFFFu;
            dwc_fiq_fsm_restart_channel(st, ch);
            return 0;
        }
        break;

    case FIQ_PER_CSPLIT_POLL:
    case FIQ_PER_CSPLIT_NYET1:
        if (hcint & (DWC_FIQ_HCINT_ACK | DWC_FIQ_HCINT_XFERCOMPL)) {
            chst->fsm = FIQ_PER_SPLIT_DONE;
            chst->hcint_copy = hcint;
            return 1;
        }
        if (hcint & DWC_FIQ_HCINT_NYET) {
            chst->nyet_count++;
            if (chst->nyet_count >= DWC_FIQ_CSPLIT_RETRIES) {
                chst->fsm = FIQ_PER_SPLIT_NYET_ABORTED;
                chst->hcint_copy = hcint;
                return 1;
            }
            chst->fsm = FIQ_PER_CSPLIT_NYET1;
            return 0;
        }
        if (hcint & DWC_FIQ_HCINT_NAK) {
            chst->fsm = FIQ_PER_SPLIT_DONE;
            chst->hcint_copy = hcint;
            return 1;
        }
        if (hcint & DWC_FIQ_HCINT_STALL) {
            chst->fsm = FIQ_PER_SPLIT_LS_ABORTED;
            chst->hcint_copy = hcint;
            return 1;
        }
        if (hcint & (DWC_FIQ_HCINT_XACTERR | DWC_FIQ_HCINT_BBLERR)) {
            chst->nr_errors++;
            if (chst->nr_errors >= DWC_FIQ_MAX_ERRORS) {
                chst->fsm = FIQ_PER_SPLIT_HS_ABORTED;
                chst->hcint_copy = hcint;
                return 1;
            }
            /* Retry CSPLIT next microframe. */
            chst->fsm = FIQ_PER_CSPLIT_NYET1;
            return 0;
        }
        if (hcint & DWC_FIQ_HCINT_FRMOVRUN) {
            chst->fsm = FIQ_PER_SPLIT_TIMEOUT;
            chst->hcint_copy = hcint;
            return 1;
        }
        break;

    default:
        break;
    }

    /* Unhandled state or bits — forward to IRQ as passthrough. */
    chst->fsm = FIQ_PASSTHROUGH_ERRORSTATE;
    chst->hcint_copy = hcint;
    return 1;
}

/* ================================================================== */
/* FSM: SOF handler — advance channels waiting on microframe timing.   */
/* ================================================================== */

static void
dwc_fiq_fsm_do_sof(struct dwc_fiq_state *st)
{
    int ch;

    for (ch = 0; ch < (int)st->num_channels; ch++) {
        struct dwc_fiq_channel_state *chst = &st->channel[ch];

        switch (chst->fsm) {
        case FIQ_PER_SSPLIT_QUEUED:
            DWC_FIQ_REG(st->usb_base + DWC_FIQ_HCINT(ch)) = 0xFFFFFFFFu;
            dwc_fiq_fsm_restart_channel(st, ch);
            chst->fsm = FIQ_PER_SSPLIT_STARTED;
            dwc_fiq_log_char(st, 'S');
            break;

        case FIQ_PER_CSPLIT_WAIT:
            chst->csplit_count++;
            dwc_fiq_fsm_setup_csplit(st, ch);
            chst->fsm = FIQ_PER_CSPLIT_POLL;
            dwc_fiq_log_char(st, 'C');
            break;

        case FIQ_PER_CSPLIT_NYET1:
            if (chst->csplit_count >= 6) {
                chst->fsm = FIQ_PER_SPLIT_NYET_ABORTED;
                chst->hcint_copy = DWC_FIQ_HCINT_NYET;
                st->channel_done_mask |= (1u << ch);
                st->fsm_active_mask &= ~(1u << ch);
                dwc_fiq_log_char(st, 'A');
                break;
            }
            chst->csplit_count++;
            dwc_fiq_fsm_setup_csplit(st, ch);
            chst->fsm = FIQ_PER_CSPLIT_POLL;
            dwc_fiq_log_char(st, 'R');
            break;

        default:
            break;
        }
    }
}

/* ================================================================== */
/* FIQ C handler — called from inline asm FIQ vector.                  */
/* WARNING: No RTEMS API calls permitted.  Only MMIO + static writes.  */
/* ================================================================== */

__attribute__((target("arm")))
void dwc_otg_fiq_handler(void)
{
    struct dwc_fiq_state *st = &g_fiq_state;
    uint32_t gintsts;
    uint32_t haint;
    int need_swirq = 0;
    int ch;

    st->fiq_count++;

    /* LED diagnostic on GPIO16. Toggle every 1024 USB FIQs so at ~2000
     * USB IRQs/sec we get ~2 toggles/sec = ~1 Hz visible blink. At
     * AHB lock-up, USB FIQs stop and LED freezes. */
    {
        volatile uint32_t *gpio_base = (volatile uint32_t *)0x3F200000u;
        uint32_t bit = 1u << 16;
        if (st->fiq_count & 0x400u) {
            gpio_base[0x1Cu / 4] = bit;  /* GPSET0: drive HIGH */
        } else {
            gpio_base[0x28u / 4] = bit;  /* GPCLR0: drive LOW */
        }
    }

    if (st->usb_base == 0)
        return;

    gintsts = DWC_FIQ_REG(st->usb_base + DWC_FIQ_GINTSTS);

    /* Handle SOF — advance FSM channels waiting on microframe timing.
     * Only consume SOF if there are FIQ-managed channels; otherwise
     * leave it for the driver's normal poll path. */
    if ((gintsts & DWC_FIQ_GINTSTS_SOF) && st->fsm_active_mask) {
        DWC_FIQ_REG(st->usb_base + DWC_FIQ_GINTSTS) = DWC_FIQ_GINTSTS_SOF;
        st->sof_count++;
        dwc_fiq_fsm_do_sof(st);
        if (st->channel_done_mask)
            need_swirq = 1;
    }

    /* Handle per-channel interrupts. */
    if (gintsts & DWC_FIQ_GINTSTS_HCHINT) {
        haint = DWC_FIQ_REG(st->usb_base + DWC_FIQ_HAINT);

        while (haint) {
            ch = __builtin_ctz(haint);
            if (ch >= (int)st->num_channels)
                break;
            haint &= ~(1u << ch);

            if (st->channel[ch].fsm == FIQ_PASSTHROUGH ||
                st->channel[ch].fsm == FIQ_PASSTHROUGH_ERRORSTATE) {
                /* Not FIQ-managed — pass through to SWIRQ. */
                need_swirq = 1;
                continue;
            }

            /* FIQ-managed channel: read and clear HCINT, run FSM. */
            {
                uint32_t hcint = DWC_FIQ_REG(st->usb_base + DWC_FIQ_HCINT(ch));
                DWC_FIQ_REG(st->usb_base + DWC_FIQ_HCINT(ch)) = hcint;

                if (dwc_fiq_fsm_do_hcintr(st, ch, hcint)) {
                    /* Terminal state — mark done, release from FSM. */
                    st->channel_done_mask |= (1u << ch);
                    st->fsm_active_mask &= ~(1u << ch);
                    need_swirq = 1;
                    dwc_fiq_log_char(st, 'D');
                    dwc_fiq_log_hex32(st, (uint32_t)ch);
                }
            }
        }
    }

    /* Any non-SOF, non-HCHINT GINTSTS bits also need SWIRQ. */
    if (gintsts & ~(DWC_FIQ_GINTSTS_SOF | DWC_FIQ_GINTSTS_HCHINT))
        need_swirq = 1;

    if (need_swirq) {
        /*
         * Disable FIQ_CTRL routing for USB source. Without this,
         * FIQ re-fires before SWIRQ has a chance to ACK GINTSTS,
         * pegging CPU at 100% in FIQ context (confirmed empirically
         * 2026-05-23 — system hung before reaching any HB lines).
         * SWIRQ handler re-enables FIQ_CTRL after running the
         * original handler.
         */
        DWC_FIQ_REG(DWC_FIQ_INTC_FIQ_CTRL) = DWC_FIQ_IRQ_USB;

        if (!st->irq_pending) {
            st->irq_pending = 1;
            dwc_fiq_raise_swirq();
        }
    }
}

/* ================================================================== */
/* __wrap_bsp_interrupt_facility_initialize                            */
/*                                                                     */
/* Called once during BSP init.  The real function zeros FIQ_CTRL.      */
/* We call through, then prepare state but do NOT arm FIQ yet —        */
/* that happens when the USB driver registers its IRQ (review #14).    */
/* ================================================================== */

extern void __real_bsp_interrupt_facility_initialize(void);

void __wrap_bsp_interrupt_facility_initialize(void)
{
    /* Let the BSP do its normal INTC setup. */
    __real_bsp_interrupt_facility_initialize();

    /* Prepare global FIQ state (zero-init is mostly fine). */
    g_fiq_state.usb_base = 0;          /* armed later */
    g_fiq_state.num_channels = 0;
    g_fiq_state.use_mphi = 0;          /* Phase 1: timer C3 */
    g_fiq_state.irq_pending = 0;
    g_fiq_state.channel_done_mask = 0;
    g_fiq_state.fsm_active_mask = 0;
    g_fiq_state.fiq_count = 0;
    g_fiq_state.sof_count = 0;
    g_fiq_state.swirq_count = 0;
    g_fiq_state.log_head = 0;
    g_fiq_spsr_fix_count = 0;

    /*
     * FIQ_CTRL is NOT programmed here (too early — USB driver not
     * registered yet).  It will be armed in __wrap_rtems_interrupt_
     * server_handler_install when vector 9 is registered.
     */
}

/* ================================================================== */
/* __wrap_rtems_interrupt_server_handler_install                       */
/*                                                                     */
/* Intercepts libbsd nexus registration of USB IRQ 9.                  */
/* Captures filter/handler/sc, installs SWIRQ handler on timer C3,     */
/* then arms FIQ routing.                                              */
/* ================================================================== */

extern rtems_status_code __real_rtems_interrupt_server_handler_install(
    uint32_t                server_index,
    rtems_vector_number     vector,
    const char             *info,
    rtems_option            options,
    rtems_interrupt_handler routine,
    void                   *arg
);

rtems_status_code __wrap_rtems_interrupt_server_handler_install(
    uint32_t                server_index,
    rtems_vector_number     vector,
    const char             *info,
    rtems_option            options,
    rtems_interrupt_handler routine,
    void                   *arg)
{
    static int fiq_armed = 0;

    printf("FIQ-WRAP: vector=%u info=%s (USB=%u) armed=%d\n",
           (unsigned)vector, info ? info : "(null)",
           (unsigned)BCM2835_IRQ_ID_USB, fiq_armed);

    /* Pass through for non-USB vectors. */
    if (vector != BCM2835_IRQ_ID_USB) {
        return __real_rtems_interrupt_server_handler_install(
            server_index, vector, info, options, routine, arg);
    }

    /*
     * The DWC OTG driver registers USB IRQ 9 twice through nexus
     * (once for filter, once for handler — or two device instances).
     * Only arm FIQ on the first call; silently absorb subsequent ones.
     * Returning RTEMS_SUCCESSFUL prevents __real_ from re-installing
     * IRQ 9 as a normal IRQ, which would override FIQ_CTRL routing.
     */
    if (fiq_armed) {
        printf("FIQ-WRAP: vector=9 duplicate — absorbed (FIQ already armed)\n");
        return RTEMS_SUCCESSFUL;
    }

    /*
     * USB IRQ 9 registration from libbsd nexus.
     *
     * The nexus wraps the FreeBSD filter/handler in a nexus_intr struct:
     *   struct nexus_intr { driver_filter_t *filt;
     *                       driver_intr_t   *intr;
     *                       void            *arg; };
     * and passes routine = nexus_intr_with_filter, arg = &ni.
     *
     * Extract the real softc for future FSM use.
     */
    struct nexus_intr_layout *ni = (struct nexus_intr_layout *)arg;
    if (ni != NULL) {
        g_dwc_filter  = (int (*)(void *))ni->filt;
        g_dwc_handler = (void (*)(void *))ni->intr;
        g_dwc_arg     = ni->arg;
    }

    /* Save the nexus wrapper for SWIRQ forwarding in Phase 1. */
    g_nexus_handler = routine;
    g_nexus_arg     = arg;

    /*
     * Install SWIRQ handler on GPU timer channel 3, on the SAME
     * interrupt server that nexus uses.  The handler calls
     * nexus_intr_with_filter → dwc_otg_filter_interrupt →
     * dwc_otg_interrupt, which acquires bus_mtx — must run in
     * thread context, not ISR context.
     *
     * IMPORTANT: call __real_ here, NOT the bare function name —
     * the bare name resolves to our __wrap_ and re-enters this
     * function, which corrupts the USB registration flow.
     */
    rtems_status_code sc;

    /* Clear any stale M1 pending. */
    BCM2835_REG(BCM2835_GPU_TIMER_CS) = BCM2835_GPU_TIMER_CS_M1;

    sc = __real_rtems_interrupt_server_handler_install(
        server_index,
        BCM2835_IRQ_ID_GPU_TIMER_M1,
        "dwc_fiq_swirq",
        RTEMS_INTERRUPT_UNIQUE,
        dwc_otg_swirq_handler,
        NULL);
    if (sc != RTEMS_SUCCESSFUL) {
        printf("FIQ-WRAP: SWIRQ install FAILED sc=%u — falling back to IRQ\n",
               (unsigned)sc);
        return __real_rtems_interrupt_server_handler_install(
            server_index, vector, info, options, routine, arg);
    }

    /*
     * Populate FIQ state with DWC OTG base address.
     * On RPi 2/3, USB core is at RPI_PERIPHERAL_BASE + 0x980000.
     */
    g_fiq_state.usb_base     = DWC_FIQ_USB_BASE;
    g_fiq_state.num_channels = 8;  /* BCM2835/2837 has 8 host channels */
    g_fiq_state.use_mphi     = 0;  /* Phase 1: timer C3 */
    g_fiq_state.fsm_active_mask = 0;

    /* Memory barrier: ensure all state is visible before FIQ is armed. */
    dwc_fiq_dmb();

    /*
     * Arm FIQ: route USB IRQ 9 to FIQ via BCM2835 INTC FIQ_CTRL.
     * Bit 7 = FIQ enable, bits [6:0] = IRQ source number.
     * Once set, IRQ 9 no longer fires as a normal IRQ.
     */
    BCM2835_REG(BCM2835_IRQ_FIQ_CTRL) = 0x80u | BCM2835_IRQ_ID_USB;

    /*
     * Unmask FIQ at the CPU level.  RTEMS keeps CPSR.F=1 by default
     * because it never uses FIQ.  The ARM context switch does NOT
     * save/restore CPSR, so clearing F here persists across all
     * threads and interrupt returns.
     */
    __asm__ volatile ("cpsie f" ::: "memory");

    fiq_armed = 1;

    printf("*** FIQ-FSM Phase1b ARMED *** build=2026-04-25L FIQ_CTRL=0x%x cpsie_f=done\n",
           (unsigned)BCM2835_REG(BCM2835_IRQ_FIQ_CTRL));

    /*
     * Do NOT call __real_ — normal IRQ path for vector 9 is now
     * disabled (FIQ_CTRL redirected it).  The original filter/handler
     * will be called from our SWIRQ handler instead.
     */
    return RTEMS_SUCCESSFUL;
}

/* ================================================================== */
/* Public API: FIQ debug log drain.                                    */
/*                                                                     */
/* Call from the existing 1-Hz diagnostic print in dwc_otg.c to dump   */
/* the FIQ circular buffer.  Safe to call from task context.           */
/* ================================================================== */

/*
 * Returns a pointer to the global FIQ state for diagnostic access.
 * Used by the existing __rtems__ diagnostic print in dwc_otg.c.
 */
struct dwc_fiq_state *dwc_fiq_get_state(void)
{
    return &g_fiq_state;
}

/*
 * Drain up to 'max_chars' from the FIQ log buffer into 'out'.
 * Returns number of characters copied.  NOT FIQ-safe (task context only).
 */
uint32_t dwc_fiq_log_drain(char *out, uint32_t max_chars)
{
    struct dwc_fiq_state *st = &g_fiq_state;
    uint32_t head = st->log_head;
    uint32_t n = 0;
    static uint32_t tail = 0;

    while (tail != head && n < max_chars) {
        out[n++] = st->log_buf[tail];
        tail = (tail + 1) & (DWC_FIQ_LOG_SIZE - 1);
    }
    return n;
}

#endif /* __rtems__ */
