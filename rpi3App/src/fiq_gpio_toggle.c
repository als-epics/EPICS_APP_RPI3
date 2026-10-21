/*
 * fiq_gpio_toggle.c — Pure-ARM FIQ-driven GPIO heartbeat for hang diagnosis.
 *
 * BCM2837 System Timer C1 (IRQ source 1) is routed to ARM FIQ via the
 * legacy IRQ controller's FIQ_CTRL register. Every ~100 ms the FIQ
 * handler increments fiq_gpio_count and toggles GPIO16 (Pi pin 36).
 *
 * Pure ARM naked function — no ARM<->Thumb interworking, no function
 * calls, uses only FIQ-banked registers (r8-r12). Interrupted context
 * (r0-r7) is untouched.
 *
 * Diagnostic semantics at hang:
 *   LED stops blinking + HB stops -> CPU/AHB is dead.
 *   LED keeps blinking + HB stops -> only scheduler is wedged.
 *
 * C1 is the only ARM-side timer channel free: C0/C2 belong to the
 * VideoCore GPU; C3 is the RTEMS BSP system tick (see
 * [[project_rpi3_bsp_uses_c3]]).
 *
 * Note (2026-05-23): RTEMS kernel arm_exc_interrupt.S is patched to
 * clear CPSR.F on IRQ epilogue (see [[feedback_arm_fiq_fbit_resets]]).
 * With that patch FIQ fires at the full ~10/sec, not the ~2/sec we
 * previously got while relying on the HB task's cpsie f.
 */

#include <stdint.h>
#include <rtems/bspIo.h>

/* Counter incremented by the FIQ handler. Read by heartbeat task. */
volatile uint32_t fiq_gpio_count = 0;

/* FIQ vector handler - pure ARM mode, no prologue/epilogue. */
__attribute__((naked, target("arm")))
void __wrap__ARMV4_Exception_fiq_default(void)
{
    __asm__ volatile (
        /* Increment fiq_gpio_count. r8-r12 are FIQ-banked. */
        "ldr   r8, =fiq_gpio_count          \n"
        "ldr   r9, [r8]                     \n"
        "add   r9, r9, #1                   \n"
        "str   r9, [r8]                     \n"

        /* Drive GPIO16 from bit 1 of the counter: 4-FIQ period.
         * C1 fires every 100 ms -> ~400 ms LOW, ~400 ms HIGH. */
        "ldr   r10, =0x3F200000             \n"   /* GPIO_BASE */
        "mov   r11, #0x00010000             \n"   /* (1<<16) */
        "tst   r9, #2                       \n"
        "streq r11, [r10, #0x28]            \n"   /* GPCLR0 */
        "strne r11, [r10, #0x1C]            \n"   /* GPSET0 */

        /* Reschedule C1 = CLO + 100000 us. */
        "ldr   r10, =0x3F003000             \n"   /* ST_BASE */
        "ldr   r11, [r10, #0x04]            \n"   /* CLO */
        "ldr   r12, =100000                 \n"
        "add   r11, r11, r12                \n"
        "str   r11, [r10, #0x10]            \n"   /* C1 */

        /* ACK C1 match (W1C bit 1 of CS). */
        "mov   r11, #0x02                   \n"
        "str   r11, [r10, #0x00]            \n"

        "dsb                                \n"

        /* Defensive F-bit clear in SPSR_fiq (kernel patch handles this
         * normally, but keep for safety against any future regressions). */
        "mrs   r9, spsr                     \n"
        "bic   r9, r9, #0x40                \n"
        "msr   spsr_cxsf, r9                \n"

        "subs  pc, lr, #4                   \n"
    );
}

#define BCM_PERIPH_BASE  0x3F000000U
#define ST_BASE          (BCM_PERIPH_BASE + 0x3000U)
#define ST_CS_REG        (ST_BASE + 0x00U)
#define ST_CLO_REG       (ST_BASE + 0x04U)
#define ST_C1_REG        (ST_BASE + 0x10U)
#define GPIO_BASE        (BCM_PERIPH_BASE + 0x200000U)
#define GPFSEL1_REG      (GPIO_BASE + 0x04U)
#define IC_BASE          (BCM_PERIPH_BASE + 0xB000U)
#define FIQ_CONTROL_REG  (IC_BASE + 0x20CU)
#define DISABLE_IRQS_1   (IC_BASE + 0x21CU)
#define C1_BIT           (1U << 1)
#define TOGGLE_INTERVAL  100000U   /* 100 ms */

static inline uint32_t mmio_r(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void     mmio_w(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

void
fiq_gpio_toggle_init(void)
{
    /* GPIO16 = OUTPUT (function 001 in GPFSEL1 bits [20:18]). */
    uint32_t v = mmio_r(GPFSEL1_REG);
    v = (v & ~(7U << 18)) | (1U << 18);
    mmio_w(GPFSEL1_REG, v);

    /* Make sure C1 is not also routed to ARM IRQ. */
    mmio_w(DISABLE_IRQS_1, C1_BIT);
    mmio_w(ST_CS_REG, C1_BIT);  /* clear stale C1 match */
    mmio_w(ST_C1_REG, mmio_r(ST_CLO_REG) + TOGGLE_INTERVAL);

    /* Route IRQ source 1 (Timer C1) to ARM FIQ. */
    mmio_w(FIQ_CONTROL_REG, 0x80U | 1U);

    /* Defensive unmask (kernel patch should keep F clear anyway). */
    __asm__ volatile ("cpsie f" ::: "memory");

    printk("FIQ-GPIO: init done. FIQ_CTRL=0x%08x CLO=%u C1=%u\n",
           (unsigned)mmio_r(FIQ_CONTROL_REG),
           (unsigned)mmio_r(ST_CLO_REG),
           (unsigned)mmio_r(ST_C1_REG));
}
