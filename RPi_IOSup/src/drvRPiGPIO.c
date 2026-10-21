/*
 * drvRPiGPIO.c - Raspberry Pi (BCM2835/BCM2837) GPIO driver and EPICS
 * device support for RTEMS.
 *
 * Pins are configured once, in the IOC startup script, each under a LABEL
 * that the database then uses in INP/OUT links:
 *
 *   rpiGpioConfigOutput("LED", 17, 0)            label, BCM pin, initial level
 *   rpiGpioConfigInput("BTN", 27, "up", "falling") label, BCM pin, pull, detect
 *       pull   : "none" | "up" | "down"
 *       detect : "none" | "rising" | "falling" | "both" | "high" | "low"
 *   rpiGpioReport(1)                             table of configured pins
 *   dbior                                        the same table, via the
 *                                                driver entry table
 *   rpiGpioSet("LED", 1)   rpiGpioGet("BTN")    shell helpers
 *
 * Records (DTYP "RPiGpio", INST_IO links):
 *   bo          OUT "@LED"                 write one output pin
 *   bi          INP "@BTN"                 live level of a pin (input or output)
 *               INP "@BTN latched"         level captured in the interrupt
 *               SCAN "I/O Intr" processes on every detected edge/level
 *   mbboDirect  OUT "@D0 D1 D2 D3"         bit i of VAL -> i-th label
 *   mbbiDirect  INP "@I0 I1 I2 I3"         i-th label -> bit i; I/O Intr fires
 *                                          when any member pin detects
 *   longin      INP "@BTN count"           number of detections since boot
 *               INP "@BTN level"           live level as integer
 *
 * Input detection uses the GPIO controller's edge/level detect logic and
 * the bank-0 GPIO interrupt (BCM2835 IRQ 49, pins 0..31). The ISR clears
 * GPEDS, latches the level, counts, marks the pin pending and releases an
 * RTEMS semaphore; a high-priority worker thread then calls
 * scanIoRequest() for the pin and for every mbbiDirect group the pin
 * belongs to. The EPICS scan functions cannot be called from the ISR
 * itself on this RTEMS/POSIX build (they take POSIX mutexes, which RTEMS
 * reports as INTERNAL_ERROR_BAD_THREAD_DISPATCH_DISABLE_LEVEL), hence the
 * thread. For "high"/"low" (level) detection the detect bit is disarmed in
 * the ISR, because a level interrupt re-fires continuously while the level
 * persists, and re-armed when a record reads the pin.
 *
 * Output writes are single stores to GPSET0/GPCLR0, so a bo processed from
 * a 1 kHz calc record toggles the pin with only the scan-thread latency.
 *
 * Only BCM GPIO 0..31 (bank 0) are handled; the 40-pin header exposes
 * GPIO 2..27. Pin numbers are BCM numbers, not header positions.
 *
 * Linux host build: register access goes to a RAM array and no interrupt is
 * installed, so the dbd and the host IOC still build.
 */

/* typed drvet for the dbior entry table (RPi_IOSup/src/Makefile sets the
 * RSET/DSET variants for the whole directory; this one must not reach the
 * registration code generated from RPi_IO.dbd) */
#define USE_TYPED_DRVET 1
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <dbAccess.h>
#include <dbScan.h>
#include <devSup.h>
#include <drvSup.h>
#include <recGbl.h>
#include <alarm.h>
#include <link.h>
#include <errlog.h>
#include <iocsh.h>
#include <epicsExport.h>
#include <epicsMutex.h>
#include <epicsString.h>
#include <epicsThread.h>
#include <boRecord.h>
#include <biRecord.h>
#include <mbboDirectRecord.h>
#include <mbbiDirectRecord.h>
#include <longinRecord.h>

#ifdef __rtems__
#include <rtems.h>
#include <rtems/irq-extension.h>
#include <bsp.h>
#include <bsp/irq.h>
#include <bsp/raspberrypi.h>
#define GPIO_REG(off) (*(volatile uint32_t *)(BCM2835_GPIO_REGS_BASE + (off)))
#else
static uint32_t rpiGpioFakeRegs[0x100 / 4];
#define GPIO_REG(off) (rpiGpioFakeRegs[(off) / 4])
#endif

/* BCM2835 GPIO register offsets (bank 0 = pins 0..31) */
#define GPFSEL(n)   (0x00 + 4 * (n))
#define GPSET0      0x1C
#define GPCLR0      0x28
#define GPLEV0      0x34
#define GPEDS0      0x40
#define GPREN0      0x4C
#define GPFEN0      0x58
#define GPHEN0      0x64
#define GPLEN0      0x70
#define GPPUD       0x94
#define GPPUDCLK0   0x98

#define RPI_GPIO_MAX_PIN   32
#define RPI_GPIO_MAX_GROUPS_PER_PIN 4
#define RPI_GPIO_LABEL_LEN 32

enum rpiGpioDir   { RPI_UNUSED = 0, RPI_INPUT, RPI_OUTPUT };
enum rpiGpioPull  { RPI_PULL_NONE = 0, RPI_PULL_DOWN = 1, RPI_PULL_UP = 2 };
enum rpiGpioDetect { RPI_DET_NONE = 0, RPI_DET_RISING, RPI_DET_FALLING,
                     RPI_DET_BOTH, RPI_DET_HIGH, RPI_DET_LOW };

typedef struct rpiGpioPin {
    char      label[RPI_GPIO_LABEL_LEN];
    int       pin;
    int       dir;
    int       pull;
    int       detect;
    volatile uint32_t count;      /* detections since boot (ISR) */
    volatile int      latched;    /* level sampled in the ISR */
    volatile int      armed;      /* level detect currently enabled */
    IOSCANPVT ioscan;
    IOSCANPVT groups[RPI_GPIO_MAX_GROUPS_PER_PIN];
    int       ngroups;
} rpiGpioPin;

static rpiGpioPin rpiGpioPins[RPI_GPIO_MAX_PIN];
static volatile uint32_t rpiGpioIrqMask;      /* pins with detection enabled */
static volatile uint32_t rpiGpioPendingMask;  /* ISR -> worker: pins to scan */
static int rpiGpioIsrInstalled;
static epicsMutexId rpiGpioCfgLock;
#ifdef __rtems__
static rtems_id rpiGpioIsrSem;                /* released by the ISR */
#endif

/* ------------------------------------------------------------------ */
/* low level                                                            */

#ifdef __rtems__
RTEMS_INTERRUPT_LOCK_DEFINE(static, rpiGpioRegLock, "rpiGpio")
#define REG_LOCK(ctx)   rtems_interrupt_lock_acquire(&rpiGpioRegLock, &(ctx))
#define REG_UNLOCK(ctx) rtems_interrupt_lock_release(&rpiGpioRegLock, &(ctx))
typedef rtems_interrupt_lock_context rpiGpioLockCtx;
#else
#define REG_LOCK(ctx)   ((void)(ctx))
#define REG_UNLOCK(ctx) ((void)(ctx))
typedef int rpiGpioLockCtx;
#endif

static void rpiGpioDelayCycles(int n)
{
    volatile int i;
    for (i = 0; i < n; i++)
        ;
}

static void rpiGpioSetFunction(int pin, int output)
{
    rpiGpioLockCtx ctx;
    uint32_t reg = GPFSEL(pin / 10);
    int shift = (pin % 10) * 3;
    uint32_t v;

    REG_LOCK(ctx);
    v = GPIO_REG(reg);
    v &= ~(7u << shift);
    if (output)
        v |= (1u << shift);
    GPIO_REG(reg) = v;
    REG_UNLOCK(ctx);
}

/* BCM2835 datasheet sequence: mode, 150 cycles, clock the pin, 150 cycles */
static void rpiGpioSetPull(int pin, int pull)
{
    rpiGpioLockCtx ctx;

    REG_LOCK(ctx);
    GPIO_REG(GPPUD) = (uint32_t)pull;
    rpiGpioDelayCycles(300);
    GPIO_REG(GPPUDCLK0) = 1u << pin;
    rpiGpioDelayCycles(300);
    GPIO_REG(GPPUD) = 0;
    GPIO_REG(GPPUDCLK0) = 0;
    REG_UNLOCK(ctx);
}

static inline void rpiGpioWriteLevel(int pin, int level)
{
    if (level)
        GPIO_REG(GPSET0) = 1u << pin;
    else
        GPIO_REG(GPCLR0) = 1u << pin;
}

static inline int rpiGpioReadLevel(int pin)
{
    return (GPIO_REG(GPLEV0) >> pin) & 1u;
}

/* set or clear one bit in a detect register, under the register lock */
static void rpiGpioDetectBit(uint32_t reg, int pin, int enable)
{
    rpiGpioLockCtx ctx;
    uint32_t v;

    REG_LOCK(ctx);
    v = GPIO_REG(reg);
    if (enable)
        v |= 1u << pin;
    else
        v &= ~(1u << pin);
    GPIO_REG(reg) = v;
    REG_UNLOCK(ctx);
}

static void rpiGpioApplyDetect(rpiGpioPin *p, int enable)
{
    uint32_t bit = 1u << p->pin;

    rpiGpioDetectBit(GPREN0, p->pin, enable &&
        (p->detect == RPI_DET_RISING || p->detect == RPI_DET_BOTH));
    rpiGpioDetectBit(GPFEN0, p->pin, enable &&
        (p->detect == RPI_DET_FALLING || p->detect == RPI_DET_BOTH));
    rpiGpioDetectBit(GPHEN0, p->pin, enable && p->detect == RPI_DET_HIGH);
    rpiGpioDetectBit(GPLEN0, p->pin, enable && p->detect == RPI_DET_LOW);
    p->armed = enable;
    /* a stale event from before arming must not fire immediately */
    GPIO_REG(GPEDS0) = bit;
}

static inline int rpiGpioIsLevelDetect(const rpiGpioPin *p)
{
    return p->detect == RPI_DET_HIGH || p->detect == RPI_DET_LOW;
}

/* ------------------------------------------------------------------ */
/* interrupt                                                            */

#ifdef __rtems__
/*
 * Interrupt handler: hardware bookkeeping only. No EPICS call is allowed
 * here (see the file header); the worker thread does the scan requests.
 */
static void rpiGpioIsr(void *arg)
{
    uint32_t eds, lev, pending;
    (void)arg;

    eds = GPIO_REG(GPEDS0);
    GPIO_REG(GPEDS0) = eds;             /* write 1 to clear */
    lev = GPIO_REG(GPLEV0);
    pending = eds & rpiGpioIrqMask;
    if (pending == 0)
        return;

    rpiGpioPendingMask |= pending;
    while (pending != 0) {
        int n = __builtin_ctz(pending);
        rpiGpioPin *p = &rpiGpioPins[n];

        pending &= pending - 1;
        p->count++;
        p->latched = (lev >> n) & 1u;
        if (rpiGpioIsLevelDetect(p)) {
            /* disarm; a record read re-arms (see rpiGpioRearm) */
            uint32_t reg = (p->detect == RPI_DET_HIGH) ? GPHEN0 : GPLEN0;
            GPIO_REG(reg) &= ~(1u << n);
            p->armed = 0;
        }
    }
    (void)rtems_semaphore_release(rpiGpioIsrSem);   /* ISR-safe */
}

/* Worker: wakes on the semaphore, hands the pending pins to the scanner. */
static void rpiGpioWorker(void *arg)
{
    (void)arg;
    for (;;) {
        rtems_interrupt_lock_context ctx;
        uint32_t pending;

        (void)rtems_semaphore_obtain(rpiGpioIsrSem, RTEMS_WAIT, RTEMS_NO_TIMEOUT);
        rtems_interrupt_lock_acquire(&rpiGpioRegLock, &ctx);
        pending = rpiGpioPendingMask;
        rpiGpioPendingMask = 0;
        rtems_interrupt_lock_release(&rpiGpioRegLock, &ctx);

        while (pending != 0) {
            int n = __builtin_ctz(pending);
            rpiGpioPin *p = &rpiGpioPins[n];
            int g;

            pending &= pending - 1;
            if (p->ioscan)
                scanIoRequest(p->ioscan);
            for (g = 0; g < p->ngroups; g++)
                scanIoRequest(p->groups[g]);
        }
    }
}

static int rpiGpioInstallIsr(void)
{
    rtems_status_code sc;
    epicsThreadId tid;

    if (rpiGpioIsrInstalled)
        return 0;
    sc = rtems_semaphore_create(rtems_build_name('G', 'P', 'I', 'O'), 0,
        RTEMS_SIMPLE_BINARY_SEMAPHORE | RTEMS_FIFO, 0, &rpiGpioIsrSem);
    if (sc != RTEMS_SUCCESSFUL) {
        errlogPrintf("rpiGpio: cannot create ISR semaphore: %s\n",
            rtems_status_text(sc));
        return -1;
    }
    tid = epicsThreadCreate("rpiGpioIrq", epicsThreadPriorityHigh,
        epicsThreadGetStackSize(epicsThreadStackMedium), rpiGpioWorker, NULL);
    if (tid == NULL) {
        errlogPrintf("rpiGpio: cannot create worker thread\n");
        return -1;
    }
    sc = rtems_interrupt_handler_install(BCM2835_IRQ_ID_GPIO_0, "RPiGPIO",
        RTEMS_INTERRUPT_UNIQUE, rpiGpioIsr, NULL);
    if (sc != RTEMS_SUCCESSFUL) {
        errlogPrintf("rpiGpio: cannot install GPIO ISR (vector %d): %s\n",
            BCM2835_IRQ_ID_GPIO_0, rtems_status_text(sc));
        return -1;
    }
    rpiGpioIsrInstalled = 1;
    return 0;
}
#else
static int rpiGpioInstallIsr(void) { rpiGpioIsrInstalled = 1; return 0; }
#endif

/* re-enable level detection after a record has consumed the event */
static void rpiGpioRearm(rpiGpioPin *p)
{
    if (rpiGpioIsLevelDetect(p) && !p->armed)
        rpiGpioApplyDetect(p, 1);
}

/* ------------------------------------------------------------------ */
/* configuration                                                        */

static void rpiGpioInitOnce(void)
{
    if (rpiGpioCfgLock == NULL)
        rpiGpioCfgLock = epicsMutexMustCreate();
}

static rpiGpioPin *rpiGpioFindLabel(const char *label)
{
    int i;

    if (label == NULL)
        return NULL;
    for (i = 0; i < RPI_GPIO_MAX_PIN; i++)
        if (rpiGpioPins[i].dir != RPI_UNUSED &&
            epicsStrCaseCmp(rpiGpioPins[i].label, label) == 0)
            return &rpiGpioPins[i];
    return NULL;
}

static int rpiGpioCheckNew(const char *label, int pin, const char *func)
{
    if (label == NULL || *label == '\0' || strlen(label) >= RPI_GPIO_LABEL_LEN) {
        errlogPrintf("%s: label missing or longer than %d characters\n",
            func, RPI_GPIO_LABEL_LEN - 1);
        return -1;
    }
    if (strchr(label, ' ') || strchr(label, ',')) {
        errlogPrintf("%s: label '%s' must not contain spaces or commas\n",
            func, label);
        return -1;
    }
    if (pin < 0 || pin >= RPI_GPIO_MAX_PIN) {
        errlogPrintf("%s: pin %d out of range 0..%d (BCM numbering)\n",
            func, pin, RPI_GPIO_MAX_PIN - 1);
        return -1;
    }
    if (rpiGpioPins[pin].dir != RPI_UNUSED) {
        errlogPrintf("%s: GPIO %d already configured as '%s'\n",
            func, pin, rpiGpioPins[pin].label);
        return -1;
    }
    if (rpiGpioFindLabel(label) != NULL) {
        errlogPrintf("%s: label '%s' already used by GPIO %d\n",
            func, label, rpiGpioFindLabel(label)->pin);
        return -1;
    }
    if (interruptAccept) {
        errlogPrintf("%s: pins must be configured before iocInit\n", func);
        return -1;
    }
    return 0;
}

/*
 * rpiGpioConfigOutput - configure one GPIO pin as a digital output.
 *
 *   label         Name used by the records to refer to this pin, e.g. in
 *                 OUT "@LED" of a bo or OUT "@LED0 LED1 LED2" of an
 *                 mbboDirect. 1..31 characters, case-insensitive, no
 *                 spaces or commas. Must be unique.
 *   pin           BCM GPIO number 0..31 (bank 0). The 40-pin header
 *                 carries GPIO 2..27; e.g. GPIO17 is header pin 11,
 *                 GPIO27 is header pin 13. Not the header pin number.
 *                 A pin can be configured only once.
 *   initialValue  Level driven as soon as the pin becomes an output:
 *                 0 = low (0 V), any other value = high (3.3 V).
 *                 Written before the direction is switched, so the pin
 *                 never glitches to the wrong state.
 *
 * Returns 0 on success, -1 with a message on the console on any error.
 * Must be called in the startup script before iocInit.
 *
 *   iocsh:  rpiGpioConfigOutput("LED", 17, 0)
 */
int rpiGpioConfigOutput(const char *label, int pin, int initialValue)
{
    rpiGpioPin *p;

    rpiGpioInitOnce();
    if (rpiGpioCheckNew(label, pin, "rpiGpioConfigOutput"))
        return -1;
    p = &rpiGpioPins[pin];
    memset(p, 0, sizeof(*p));
    strcpy(p->label, label);
    p->pin = pin;
    p->dir = RPI_OUTPUT;
    rpiGpioWriteLevel(pin, initialValue != 0);   /* level before direction */
    rpiGpioSetFunction(pin, 1);
    scanIoInit(&p->ioscan);
    return 0;
}

static int rpiGpioParsePull(const char *s, int *pull)
{
    if (s == NULL || *s == '\0' || !epicsStrCaseCmp(s, "none") ||
        !epicsStrCaseCmp(s, "off"))
        *pull = RPI_PULL_NONE;
    else if (!epicsStrCaseCmp(s, "up") || !epicsStrCaseCmp(s, "pullup"))
        *pull = RPI_PULL_UP;
    else if (!epicsStrCaseCmp(s, "down") || !epicsStrCaseCmp(s, "pulldown"))
        *pull = RPI_PULL_DOWN;
    else
        return -1;
    return 0;
}

static int rpiGpioParseDetect(const char *s, int *det)
{
    if (s == NULL || *s == '\0' || !epicsStrCaseCmp(s, "none"))
        *det = RPI_DET_NONE;
    else if (!epicsStrCaseCmp(s, "rising"))
        *det = RPI_DET_RISING;
    else if (!epicsStrCaseCmp(s, "falling"))
        *det = RPI_DET_FALLING;
    else if (!epicsStrCaseCmp(s, "both") || !epicsStrCaseCmp(s, "edge") ||
             !epicsStrCaseCmp(s, "any"))
        *det = RPI_DET_BOTH;
    else if (!epicsStrCaseCmp(s, "high"))
        *det = RPI_DET_HIGH;
    else if (!epicsStrCaseCmp(s, "low"))
        *det = RPI_DET_LOW;
    else
        return -1;
    return 0;
}

/*
 * rpiGpioConfigInput - configure one GPIO pin as a digital input, with
 * optional internal pull resistor and interrupt detection.
 *
 *   label    Name used by the records: INP "@BTN" (bi, longin) or
 *            INP "@IN0 IN1 IN2" (mbbiDirect). Same rules as for outputs.
 *   pin      BCM GPIO number 0..31, see rpiGpioConfigOutput.
 *   pull     Internal pull resistor (about 50 kohm), case-insensitive:
 *              "none" (or "" / "off")  no resistor, pin floats
 *              "up"   (or "pullup")   pulled to 3.3 V; a switch to GND
 *                                      reads 1 open, 0 pressed
 *              "down" (or "pulldown") pulled to GND; a switch to 3.3 V
 *                                      reads 0 open, 1 pressed
 *   detect   What the GPIO controller detects and raises the interrupt
 *            for (bank-0 GPIO interrupt, IRQ 49). Case-insensitive:
 *              "none"                  no interrupt; records read the
 *                                      pin only when they are scanned
 *              "rising"                low -> high transitions
 *              "falling"               high -> low transitions
 *              "both" (or "edge"/"any") every transition
 *              "high"                  level: fires while the pin is 1
 *              "low"                   level: fires while the pin is 0
 *            Each detection increments the pin's count, latches the
 *            level and processes all records on the pin's "I/O Intr"
 *            scan (bi, longin, and mbbiDirect groups containing it).
 *            For "high"/"low" the detection is switched off inside the
 *            interrupt handler and switched on again when a record reads
 *            the pin, otherwise a level interrupt would repeat
 *            continuously; so a level input gives one record processing
 *            per read while the level persists.
 *            Edge detection is synchronous (sampled with the GPIO
 *            clock); pulses shorter than about 10 ns may be missed.
 *            There is no debouncing: a mechanical switch on "both" will
 *            count its bounces.
 *
 * Returns 0 on success, -1 with a message on the console on any error
 * (bad label/pin/choice, pin or label already used, called after iocInit,
 * interrupt handler could not be installed).
 * Must be called in the startup script before iocInit.
 *
 *   iocsh:  rpiGpioConfigInput("BTN", 27, "up", "falling")
 *           rpiGpioConfigInput("SENSE", 22, "none", "both")
 *           rpiGpioConfigInput("DOOR", 23, "down", "high")
 */
int rpiGpioConfigInput(const char *label, int pin, const char *pull,
    const char *detect)
{
    rpiGpioPin *p;
    int ipull, idet;

    rpiGpioInitOnce();
    if (rpiGpioCheckNew(label, pin, "rpiGpioConfigInput"))
        return -1;
    if (rpiGpioParsePull(pull, &ipull)) {
        errlogPrintf("rpiGpioConfigInput: pull '%s' not one of none/up/down\n", pull);
        return -1;
    }
    if (rpiGpioParseDetect(detect, &idet)) {
        errlogPrintf("rpiGpioConfigInput: detect '%s' not one of "
            "none/rising/falling/both/high/low\n", detect);
        return -1;
    }
    p = &rpiGpioPins[pin];
    memset(p, 0, sizeof(*p));
    strcpy(p->label, label);
    p->pin = pin;
    p->dir = RPI_INPUT;
    p->pull = ipull;
    p->detect = idet;
    scanIoInit(&p->ioscan);

    rpiGpioSetFunction(pin, 0);
    rpiGpioSetPull(pin, ipull);
    rpiGpioApplyDetect(p, 0);                /* start clean */
    if (idet != RPI_DET_NONE) {
        if (rpiGpioInstallIsr())
            return -1;
        rpiGpioApplyDetect(p, 1);
        rpiGpioIrqMask |= 1u << pin;
    }
    p->latched = rpiGpioReadLevel(pin);
    return 0;
}

static const char *rpiGpioPullName(int pull)
{
    return pull == RPI_PULL_UP ? "up" : pull == RPI_PULL_DOWN ? "down" : "none";
}

static const char *rpiGpioDetectName(int det)
{
    static const char *names[] = { "none", "rising", "falling", "both",
                                   "high", "low" };
    return (det >= 0 && det <= RPI_DET_LOW) ? names[det] : "?";
}

/*
 * rpiGpioReport - print the configured pins.
 *   level   0: table of label, GPIO number, direction, pull, detect, live
 *              level, detection count and mbbiDirect group count;
 *           1 or more: also the raw GPFSEL/GPLEV/detect registers.
 *   iocsh:  rpiGpioReport(1)
 */
int rpiGpioReport(int level)
{
    int i, n = 0;

    printf("RPi GPIO driver: bank 0 (BCM GPIO 0..31), ISR+worker %s, "
        "detect mask 0x%08x, pending 0x%08x\n",
        rpiGpioIsrInstalled ? "installed" : "not installed",
        (unsigned)rpiGpioIrqMask, (unsigned)rpiGpioPendingMask);
    printf("  %-12s %4s %-6s %-5s %-8s %5s %8s %6s\n",
        "label", "gpio", "dir", "pull", "detect", "level", "count", "groups");
    for (i = 0; i < RPI_GPIO_MAX_PIN; i++) {
        rpiGpioPin *p = &rpiGpioPins[i];
        if (p->dir == RPI_UNUSED)
            continue;
        n++;
        printf("  %-12s %4d %-6s %-5s %-8s %5d %8u %6d%s\n", p->label, p->pin,
            p->dir == RPI_OUTPUT ? "output" : "input",
            p->dir == RPI_OUTPUT ? "-" : rpiGpioPullName(p->pull),
            p->dir == RPI_OUTPUT ? "-" : rpiGpioDetectName(p->detect),
            rpiGpioReadLevel(p->pin), (unsigned)p->count, p->ngroups,
            (rpiGpioIsLevelDetect(p) && !p->armed) ? " (level: disarmed)" : "");
    }
    if (n == 0)
        printf("  no pins configured (use rpiGpioConfigOutput / rpiGpioConfigInput)\n");
    if (level > 0)
        printf("  GPFSEL0..2=%08x %08x %08x GPLEV0=%08x GPREN0=%08x GPFEN0=%08x "
            "GPHEN0=%08x GPLEN0=%08x\n",
            (unsigned)GPIO_REG(GPFSEL(0)), (unsigned)GPIO_REG(GPFSEL(1)),
            (unsigned)GPIO_REG(GPFSEL(2)), (unsigned)GPIO_REG(GPLEV0),
            (unsigned)GPIO_REG(GPREN0), (unsigned)GPIO_REG(GPFEN0),
            (unsigned)GPIO_REG(GPHEN0), (unsigned)GPIO_REG(GPLEN0));
    return 0;
}

/*
 * rpiGpioSet - write an output pin from the shell (bypasses the records).
 *   label   label of a pin configured with rpiGpioConfigOutput
 *   value   0 = low, any other value = high
 *   iocsh:  rpiGpioSet("LED", 1)
 */
int rpiGpioSet(const char *label, int value)
{
    rpiGpioPin *p = rpiGpioFindLabel(label);

    if (p == NULL || p->dir != RPI_OUTPUT) {
        errlogPrintf("rpiGpioSet: '%s' is not a configured output\n",
            label ? label : "(null)");
        return -1;
    }
    rpiGpioWriteLevel(p->pin, value != 0);
    return 0;
}

/*
 * rpiGpioGet - print and return the live level (0/1) of any configured
 * pin, input or output; -1 if the label is unknown.
 *   iocsh:  rpiGpioGet("BTN")
 */
int rpiGpioGet(const char *label)
{
    rpiGpioPin *p = rpiGpioFindLabel(label);

    if (p == NULL) {
        errlogPrintf("rpiGpioGet: '%s' is not configured\n",
            label ? label : "(null)");
        return -1;
    }
    printf("%s (GPIO %d) = %d\n", p->label, p->pin, rpiGpioReadLevel(p->pin));
    return rpiGpioReadLevel(p->pin);
}

/* ------------------------------------------------------------------ */
/* link parsing                                                         */

/* "@LABEL [option]" -> pin, option copied (may be empty) */
static rpiGpioPin *rpiGpioParseSingle(dbCommon *prec, const struct link *plink,
    char *option, size_t optlen)
{
    char label[RPI_GPIO_LABEL_LEN];
    const char *s;
    size_t n;
    rpiGpioPin *p;

    if (plink->type != INST_IO) {
        recGblRecordError(S_db_badField, prec, "RPiGpio: link must be INST_IO (\"@LABEL\")");
        return NULL;
    }
    s = plink->value.instio.string;
    while (*s && isspace((unsigned char)*s))
        s++;
    n = strcspn(s, " \t,");
    if (n == 0 || n >= sizeof(label)) {
        recGblRecordError(S_db_badField, prec, "RPiGpio: bad or missing label");
        return NULL;
    }
    memcpy(label, s, n);
    label[n] = '\0';
    s += n;
    while (*s && (isspace((unsigned char)*s) || *s == ','))
        s++;
    if (option) {
        strncpy(option, s, optlen - 1);
        option[optlen - 1] = '\0';
        n = strlen(option);
        while (n > 0 && isspace((unsigned char)option[n - 1]))
            option[--n] = '\0';
    }
    p = rpiGpioFindLabel(label);
    if (p == NULL) {
        errlogPrintf("%s: RPiGpio label '%s' not configured\n", prec->name, label);
        recGblRecordError(S_db_badField, prec, "RPiGpio: label not configured");
        return NULL;
    }
    return p;
}

/* "@L0 L1 L2 ..." -> list of pins, bit i = i-th label */
typedef struct rpiGpioGroup {
    int        n;
    rpiGpioPin *pins[32];
    IOSCANPVT  ioscan;
} rpiGpioGroup;

static rpiGpioGroup *rpiGpioParseGroup(dbCommon *prec, const struct link *plink,
    int needOutput)
{
    rpiGpioGroup *grp;
    char buf[256];
    char *tok, *save = NULL;

    if (plink->type != INST_IO) {
        recGblRecordError(S_db_badField, prec, "RPiGpio: link must be INST_IO (\"@L0 L1 ...\")");
        return NULL;
    }
    grp = calloc(1, sizeof(*grp));
    if (grp == NULL)
        return NULL;
    strncpy(buf, plink->value.instio.string, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (tok = strtok_r(buf, " \t,", &save); tok != NULL;
         tok = strtok_r(NULL, " \t,", &save)) {
        rpiGpioPin *p = rpiGpioFindLabel(tok);
        if (p == NULL) {
            errlogPrintf("%s: RPiGpio label '%s' not configured\n", prec->name, tok);
            free(grp);
            recGblRecordError(S_db_badField, prec, "RPiGpio: label not configured");
            return NULL;
        }
        if (needOutput && p->dir != RPI_OUTPUT) {
            errlogPrintf("%s: RPiGpio label '%s' is not an output\n", prec->name, tok);
            free(grp);
            recGblRecordError(S_db_badField, prec, "RPiGpio: not an output");
            return NULL;
        }
        if (grp->n >= 32) {
            free(grp);
            recGblRecordError(S_db_badField, prec, "RPiGpio: more than 32 labels");
            return NULL;
        }
        grp->pins[grp->n++] = p;
    }
    if (grp->n == 0) {
        free(grp);
        recGblRecordError(S_db_badField, prec, "RPiGpio: no labels in link");
        return NULL;
    }
    return grp;
}

static uint32_t rpiGpioGroupRead(const rpiGpioGroup *grp, int latched)
{
    uint32_t lev = GPIO_REG(GPLEV0);
    uint32_t v = 0;
    int i;

    for (i = 0; i < grp->n; i++) {
        rpiGpioPin *p = grp->pins[i];
        int bit = (latched && p->dir == RPI_INPUT && p->detect != RPI_DET_NONE) ?
            p->latched : (int)((lev >> p->pin) & 1u);
        if (bit)
            v |= 1u << i;
        rpiGpioRearm(p);
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* bo                                                                   */

static long bo_init_record(dbCommon *pcommon)
{
    boRecord *prec = (boRecord *)pcommon;
    rpiGpioPin *p = rpiGpioParseSingle(pcommon, &prec->out, NULL, 0);

    if (p == NULL) {
        prec->pact = TRUE;
        return S_db_badField;
    }
    if (p->dir != RPI_OUTPUT) {
        errlogPrintf("%s: RPiGpio label '%s' is not an output\n", prec->name, p->label);
        prec->pact = TRUE;
        return S_db_badField;
    }
    prec->dpvt = p;
    prec->rval = rpiGpioReadLevel(p->pin);
    return 0;                                   /* convert RVAL -> VAL */
}

static long bo_write(boRecord *prec)
{
    rpiGpioPin *p = prec->dpvt;

    if (p == NULL)
        return S_dev_NoInit;
    rpiGpioWriteLevel(p->pin, prec->rval != 0);
    return 0;
}

bodset devBoRPiGpio = {
    { 5, NULL, NULL, bo_init_record, NULL },
    bo_write
};
epicsExportAddress(dset, devBoRPiGpio);

/* ------------------------------------------------------------------ */
/* bi                                                                   */

typedef struct biPvt {
    rpiGpioPin *pin;
    int latched;
} biPvt;

static long bi_init_record(dbCommon *pcommon)
{
    biRecord *prec = (biRecord *)pcommon;
    char option[32];
    rpiGpioPin *p = rpiGpioParseSingle(pcommon, &prec->inp, option, sizeof(option));
    biPvt *pvt;

    if (p == NULL) {
        prec->pact = TRUE;
        return S_db_badField;
    }
    pvt = calloc(1, sizeof(*pvt));
    pvt->pin = p;
    pvt->latched = (epicsStrCaseCmp(option, "latched") == 0);
    if (pvt->latched && p->detect == RPI_DET_NONE)
        errlogPrintf("%s: 'latched' needs an input with detection, using live level\n",
            prec->name);
    prec->dpvt = pvt;
    return 0;
}

static long bi_get_ioint_info(int cmd, dbCommon *prec, IOSCANPVT *ppvt)
{
    biPvt *pvt = prec->dpvt;
    (void)cmd;

    if (pvt == NULL)
        return S_dev_NoInit;
    if (pvt->pin->dir == RPI_INPUT && pvt->pin->detect == RPI_DET_NONE)
        errlogPrintf("%s: I/O Intr on '%s' but detection is 'none'\n",
            prec->name, pvt->pin->label);
    *ppvt = pvt->pin->ioscan;
    return 0;
}

static long bi_read(biRecord *prec)
{
    biPvt *pvt = prec->dpvt;
    rpiGpioPin *p;

    if (pvt == NULL)
        return S_dev_NoInit;
    p = pvt->pin;
    prec->rval = (pvt->latched && p->detect != RPI_DET_NONE) ?
        (epicsUInt32)p->latched : (epicsUInt32)rpiGpioReadLevel(p->pin);
    rpiGpioRearm(p);
    return 0;
}

bidset devBiRPiGpio = {
    { 5, NULL, NULL, bi_init_record, bi_get_ioint_info },
    bi_read
};
epicsExportAddress(dset, devBiRPiGpio);

/* ------------------------------------------------------------------ */
/* mbboDirect                                                           */

static long mbboDirect_init_record(dbCommon *pcommon)
{
    mbboDirectRecord *prec = (mbboDirectRecord *)pcommon;
    rpiGpioGroup *grp = rpiGpioParseGroup(pcommon, &prec->out, 1);

    if (grp == NULL) {
        prec->pact = TRUE;
        return S_db_badField;
    }
    prec->dpvt = grp;
    if (prec->nobt == 0 || prec->nobt > grp->n) {
        prec->nobt = grp->n;
        prec->mask = ((grp->n >= 32) ? 0xffffffffu : ((1u << grp->n) - 1u)) << prec->shft;
    }
    prec->rval = rpiGpioGroupRead(grp, 0) << prec->shft;
    return 0;                                   /* convert RVAL -> VAL */
}

static long mbboDirect_write(mbboDirectRecord *prec)
{
    rpiGpioGroup *grp = prec->dpvt;
    uint32_t v, set = 0, clr = 0;
    int i;

    if (grp == NULL)
        return S_dev_NoInit;
    v = (prec->rval & prec->mask) >> prec->shft;
    for (i = 0; i < grp->n && i < (int)prec->nobt; i++) {
        if (v & (1u << i))
            set |= 1u << grp->pins[i]->pin;
        else
            clr |= 1u << grp->pins[i]->pin;
    }
    /* two stores: all selected pins change within a few ns of each other */
    if (clr)
        GPIO_REG(GPCLR0) = clr;
    if (set)
        GPIO_REG(GPSET0) = set;
    return 0;
}

mbbodirectdset devMbboDirectRPiGpio = {
    { 5, NULL, NULL, mbboDirect_init_record, NULL },
    mbboDirect_write
};
epicsExportAddress(dset, devMbboDirectRPiGpio);

/* ------------------------------------------------------------------ */
/* mbbiDirect                                                           */

static long mbbiDirect_init_record(dbCommon *pcommon)
{
    mbbiDirectRecord *prec = (mbbiDirectRecord *)pcommon;
    rpiGpioGroup *grp = rpiGpioParseGroup(pcommon, &prec->inp, 0);
    int i;

    if (grp == NULL) {
        prec->pact = TRUE;
        return S_db_badField;
    }
    prec->dpvt = grp;
    if (prec->nobt == 0 || prec->nobt > grp->n) {
        prec->nobt = grp->n;
        prec->mask = ((grp->n >= 32) ? 0xffffffffu : ((1u << grp->n) - 1u)) << prec->shft;
    }
    /* own scan list, requested from the ISR of every member input pin */
    scanIoInit(&grp->ioscan);
    for (i = 0; i < grp->n; i++) {
        rpiGpioPin *p = grp->pins[i];
        if (p->dir == RPI_INPUT && p->detect != RPI_DET_NONE) {
            if (p->ngroups < RPI_GPIO_MAX_GROUPS_PER_PIN)
                p->groups[p->ngroups++] = grp->ioscan;
            else
                errlogPrintf("%s: GPIO '%s' is already in %d mbbiDirect groups\n",
                    prec->name, p->label, RPI_GPIO_MAX_GROUPS_PER_PIN);
        }
    }
    return 0;
}

static long mbbiDirect_get_ioint_info(int cmd, dbCommon *prec, IOSCANPVT *ppvt)
{
    rpiGpioGroup *grp = prec->dpvt;
    (void)cmd;

    if (grp == NULL)
        return S_dev_NoInit;
    *ppvt = grp->ioscan;
    return 0;
}

static long mbbiDirect_read(mbbiDirectRecord *prec)
{
    rpiGpioGroup *grp = prec->dpvt;

    if (grp == NULL)
        return S_dev_NoInit;
    prec->rval = rpiGpioGroupRead(grp, 0) << prec->shft;
    return 0;
}

mbbidirectdset devMbbiDirectRPiGpio = {
    { 5, NULL, NULL, mbbiDirect_init_record, mbbiDirect_get_ioint_info },
    mbbiDirect_read
};
epicsExportAddress(dset, devMbbiDirectRPiGpio);

/* ------------------------------------------------------------------ */
/* longin: "@LABEL count" | "@LABEL level"                              */

typedef struct liPvt {
    rpiGpioPin *pin;
    int what;               /* 0 count, 1 level */
} liPvt;

static long li_init_record(dbCommon *pcommon)
{
    longinRecord *prec = (longinRecord *)pcommon;
    char option[32];
    rpiGpioPin *p = rpiGpioParseSingle(pcommon, &prec->inp, option, sizeof(option));
    liPvt *pvt;

    if (p == NULL) {
        prec->pact = TRUE;
        return S_db_badField;
    }
    pvt = calloc(1, sizeof(*pvt));
    pvt->pin = p;
    if (option[0] == '\0' || !epicsStrCaseCmp(option, "count"))
        pvt->what = 0;
    else if (!epicsStrCaseCmp(option, "level"))
        pvt->what = 1;
    else {
        errlogPrintf("%s: RPiGpio longin option '%s' not 'count' or 'level'\n",
            prec->name, option);
        free(pvt);
        prec->pact = TRUE;
        return S_db_badField;
    }
    prec->dpvt = pvt;
    return 0;
}

static long li_get_ioint_info(int cmd, dbCommon *prec, IOSCANPVT *ppvt)
{
    liPvt *pvt = prec->dpvt;
    (void)cmd;

    if (pvt == NULL)
        return S_dev_NoInit;
    *ppvt = pvt->pin->ioscan;
    return 0;
}

static long li_read(longinRecord *prec)
{
    liPvt *pvt = prec->dpvt;

    if (pvt == NULL)
        return S_dev_NoInit;
    prec->val = pvt->what ? (epicsInt32)rpiGpioReadLevel(pvt->pin->pin)
                          : (epicsInt32)pvt->pin->count;
    rpiGpioRearm(pvt->pin);
    return 0;
}

longindset devLiRPiGpio = {
    { 5, NULL, NULL, li_init_record, li_get_ioint_info },
    li_read
};
epicsExportAddress(dset, devLiRPiGpio);

/* ------------------------------------------------------------------ */
/* iocsh                                                                */

static const iocshArg cfgOutArg0 = { "label", iocshArgString };
static const iocshArg cfgOutArg1 = { "bcmPin", iocshArgInt };
static const iocshArg cfgOutArg2 = { "initialValue", iocshArgInt };
static const iocshArg *cfgOutArgs[] = { &cfgOutArg0, &cfgOutArg1, &cfgOutArg2 };
static const iocshFuncDef cfgOutDef = { "rpiGpioConfigOutput", 3, cfgOutArgs,
    "Configure a BCM GPIO pin as output under a label.\n"
    "  rpiGpioConfigOutput(\"LED\", 17, 0)\n" };
static void cfgOutCall(const iocshArgBuf *args)
{
    rpiGpioConfigOutput(args[0].sval, args[1].ival, args[2].ival);
}

static const iocshArg cfgInArg0 = { "label", iocshArgString };
static const iocshArg cfgInArg1 = { "bcmPin", iocshArgInt };
static const iocshArg cfgInArg2 = { "pull (none|up|down)", iocshArgString };
static const iocshArg cfgInArg3 = { "detect (none|rising|falling|both|high|low)", iocshArgString };
static const iocshArg *cfgInArgs[] = { &cfgInArg0, &cfgInArg1, &cfgInArg2, &cfgInArg3 };
static const iocshFuncDef cfgInDef = { "rpiGpioConfigInput", 4, cfgInArgs,
    "Configure a BCM GPIO pin as input under a label, with pull resistor\n"
    "and interrupt detection.\n"
    "  rpiGpioConfigInput(\"BTN\", 27, \"up\", \"falling\")\n" };
static void cfgInCall(const iocshArgBuf *args)
{
    rpiGpioConfigInput(args[0].sval, args[1].ival, args[2].sval, args[3].sval);
}

static const iocshArg reportArg0 = { "level", iocshArgInt };
static const iocshArg *reportArgs[] = { &reportArg0 };
static const iocshFuncDef reportDef = { "rpiGpioReport", 1, reportArgs,
    "List configured GPIO pins (level 1 adds raw registers).\n" };
static void reportCall(const iocshArgBuf *args)
{
    rpiGpioReport(args[0].ival);
}

static const iocshArg setArg0 = { "label", iocshArgString };
static const iocshArg setArg1 = { "value", iocshArgInt };
static const iocshArg *setArgs[] = { &setArg0, &setArg1 };
static const iocshFuncDef setDef = { "rpiGpioSet", 2, setArgs,
    "Write an output pin from the shell.\n" };
static void setCall(const iocshArgBuf *args)
{
    rpiGpioSet(args[0].sval, args[1].ival);
}

static const iocshArg getArg0 = { "label", iocshArgString };
static const iocshArg *getArgs[] = { &getArg0 };
static const iocshFuncDef getDef = { "rpiGpioGet", 1, getArgs,
    "Print the level of a pin.\n" };
static void getCall(const iocshArgBuf *args)
{
    rpiGpioGet(args[0].sval);
}

static void rpiGpioRegistrar(void)
{
    iocshRegister(&cfgOutDef, cfgOutCall);
    iocshRegister(&cfgInDef, cfgInCall);
    iocshRegister(&reportDef, reportCall);
    iocshRegister(&setDef, setCall);
    iocshRegister(&getDef, getCall);
}
epicsExportRegistrar(rpiGpioRegistrar);

/*
 * Driver entry table, so that dbior (and dbior <level>) lists this driver
 * like any other. The pins are configured from st.cmd before iocInit, so
 * init has nothing to do.
 */
static long rpiGpioDrvReport(int level)
{
    return rpiGpioReport(level);
}

static long rpiGpioDrvInit(void)
{
    return 0;
}

static drvet drvRPiGpio = { 2, rpiGpioDrvReport, rpiGpioDrvInit };
epicsExportAddress(drvet, drvRPiGpio);
