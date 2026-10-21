# RPi_IOSup: Raspberry Pi GPIO for EPICS on RTEMS

Driver and device support for the BCM2835/BCM2837 GPIO controller
(Raspberry Pi 2/3, RTEMS `raspberrypi2` BSP). Library `RPi_IO`, dbd
`RPi_IO.dbd`, source `src/drvRPiGPIO.c`.

## Configure pins in the startup script (before iocInit)

```
rpiGpioConfigOutput("LED", 17, 0)
#                    label  BCM pin  initial level

rpiGpioConfigInput("BTN", 27, "up", "falling")
#                   label  BCM pin  pull   detect
#   pull   : none | up | down          (internal resistor)
#   detect : none | rising | falling | both | high | low
```

Pin numbers are BCM GPIO numbers (the header exposes GPIO 2..27), bank 0
only (0..31). Each pin and each label can be configured once. Labels are
case-insensitive and must not contain spaces or commas.

Shell helpers: `rpiGpioReport(1)` (table of pins, counts, raw registers),
`rpiGpioSet("LED", 1)`, `rpiGpioGet("BTN")`. The driver also has an entry
table (`driver(drvRPiGpio)`), so `dbior` and `dbior 1` print the same
table.

## Records

All with `DTYP "RPiGpio"` and INST_IO links that name the label:

| Record | Link | Meaning |
|---|---|---|
| bo | `OUT "@LED"` | write the output pin (one store to GPSET0/GPCLR0) |
| bi | `INP "@BTN"` | live level of any configured pin |
| bi | `INP "@BTN latched"` | level sampled in the interrupt handler |
| mbboDirect | `OUT "@D0 D1 D2 D3"` | bit i of VAL drives the i-th label (all pins change within a few ns) |
| mbbiDirect | `INP "@I0 I1 I2 I3"` | i-th label becomes bit i |
| longin | `INP "@BTN count"` | number of detections since boot |
| longin | `INP "@BTN level"` | live level as integer |

`SCAN "I/O Intr"` on bi, longin and mbbiDirect processes the record on
every detection of the pin (for mbbiDirect: of any member pin). NOBT of
the mbb*Direct records defaults to the number of labels in the link.

### Live level, latched level and count: which one to use

An input pin keeps three values. `rpiGpioReport` shows all of them.

| Link option | Value | Updated | Use it for |
|---|---|---|---|
| `@BTN` (none) or `@BTN level` | the pin as it is **now**, read from GPLEV0 when the record processes | at every record processing | the present state of a switch, a door contact, a ready line; periodic scan or `I/O Intr` both fine |
| `@BTN latched` | the level the ISR read **at the moment of the detection** | in the interrupt, before the record is scheduled | short pulses, and "which edge was it" on a `both` input |
| `@BTN count` | number of detections since boot, 32-bit, wraps | in the interrupt | counting events, rates, missed-event checks |

Why the distinction matters: between the interrupt and the moment the
record reads the pin lie the semaphore hand-off, the `rpiGpioIrq` thread
and the scan thread, typically 10 to 30 us, more under load. A pulse
shorter than that is gone before a live read sees it. The latched value
was taken inside the ISR, so it is the level that caused the interrupt.

- **Push button, `detect "falling"` with pull-up.** A `bi` with `@BTN` and
  `SCAN "I/O Intr"` processes once per press and shows 0 (pressed) if the
  button is still down, otherwise already 1 again. The same record with
  `@BTN latched` always shows 0, the level at the edge. For presses that is
  rarely important; for a 5 us trigger pulse it is the only way to see it.
- **Direction of an edge, `detect "both"`.** A `bi` with `@BTN latched` and
  `I/O Intr` processes on every edge and its value tells which edge
  happened: 1 after a rising, 0 after a falling edge. A live read would
  often be too late.
- **Counting, `@BTN count`.** A `longin` with `I/O Intr` holds the number of
  edges or pulses so far; a `calc` on a periodic scan that subtracts the
  previous value gives a rate. The toggle test below counts 1000 edges per
  second this way (`RPI3:TGL:RATE`). The count is also the way to notice
  events the records did not see one by one: detections that arrive while
  the previous one is still on its way through the worker thread and the
  scan queue are merged into one processing, so two consecutive `I/O Intr`
  processings can be several counts apart. The count starts at 0 at boot
  and cannot be reset at run time; keep a reference value in a record.
- **Mechanical switches bounce.** With `detect "both"` every bounce counts
  and latches. Use `falling` or `rising` for switches and treat the count
  as "at least one press"; a 10 to 20 ms debounce belongs in the database
  (for example a `calcout` that ignores a second event within that time).
- **Level detection, `detect "high"` or `"low"`.** The interrupt fires as
  long as the level persists, so the driver disarms it in the ISR and
  re-arms it when a record reads the pin (live or latched). The count
  therefore advances once per read while the level is present, not once per
  microsecond, and a `bi` with `I/O Intr` on such a pin samples the level
  every time it is read, which gives a self-paced polling loop at the
  speed of the scan path. Edge detection is the better choice unless the
  signal source really is a level.
- **`mbbiDirect "@I0 I1 I2 latched"`** applies the latched rule to every
  member pin that has detection; pins without detection contribute their
  live level.

The latched value starts as the level at configuration time, and a
`latched` link on a pin configured with `detect "none"` falls back to the
live level with a message at record initialisation.

## Interrupts

Inputs with `detect` other than `none` use the controller's edge/level
detect registers and the bank-0 GPIO interrupt (IRQ 49). The ISR clears
the event, latches the level, increments the count, marks the pin pending
and releases an RTEMS semaphore; the thread `rpiGpioIrq` (priority
`epicsThreadPriorityHigh`) then calls `scanIoRequest()`. EPICS scan calls
are not allowed directly in an ISR on the RTEMS/POSIX build: they take
POSIX mutexes and RTEMS stops with
`INTERNAL_ERROR_BAD_THREAD_DISPATCH_DISABLE_LEVEL` (seen 2026-10-14 with
the first version of this driver). The extra thread switch adds roughly
10 to 30 us before the record processes. For `high`/`low` the detection is disarmed in the ISR,
since a level interrupt would re-fire continuously, and re-armed when a
record reads the pin. There is no debouncing; a mechanical switch on a
`both` input will count bounces.

## Timing test (`Db/rpiGpioToggleTest.db`)

Wire GPIO17 to GPIO27 and to an oscilloscope, then

```
rpiGpioConfigOutput("TOGGLE", 17, 0)
rpiGpioConfigInput("SENSE", 27, "none", "both")
dbLoadRecords("db/rpiGpioToggleTest.db", "P=RPI3,OUT=TOGGLE,IN=SENSE,RATE=.001 second")
```

A calc record on the `.001 second` scan flips its value and processes the
bo through FLNK, giving a 500 Hz square wave. `RPI3:TGL:RATE` shows the
edges per second counted by the interrupt (1000 expected) and the scope
shows the period jitter: RTEMS tick quantization (500 us) plus record
processing. No CA/PVA monitor is needed for the measurement.

## Measured on the Pi 3B+ (2026-10-14)

Toggle test with GPIO17 wired to GPIO27, IOC image with PVAccess, QSRV,
telnet and the muge USB Ethernet polling active:

| RATE | output toggles | edges counted by interrupt (`TGL:RATE`) | note |
|---|---|---|---|
| `.01 second` | every 10 ms | 100 per second, constant | |
| `.001 second` | every 1 ms | 1000 per second, constant over 5 min | no scan overrun messages |
| `.001 second`, overnight | every 1 ms | 1000 per second after 17 h 47 min (2026-10-14 14:47 to 2026-10-15 08:34, still running) | no reboot, no overrun, timestamps still 1.000 ms apart |

Record timestamps of `TGL:OUT` at the 1 ms rate advance by 1.000 ms with a
spread of about 7 us (…853 / …860 us), i.e. the scan-thread wake-up
jitter is well below the 500 us RTEMS tick. Every edge is counted, so
the ISR, the semaphore hand-off and the I/O Intr scan keep up at 1 kHz
with margin.

A once-per-minute disturbance seen in the first runs (93 and 107 edges in
consecutive seconds) was not the driver but the EPICS NTP time provider
on RTEMS discarding the NTP fraction; fixed in Base, see
`patches/EPICS_BASE_RASPBERRYPI_PATCH.md`.

## Loaded at run time instead of linked into the kernel (verified 2026-10-20)

On the `libdl` branch the support is not part of `kernel7.img`. The build
makes `bin/RTEMS-raspberrypi2/RPi_IO.o`, one relocatable object holding
the library and the registration code generated from `RPi_IO.dbd`, and
packs it with `dbd/RPi_IO.dbd` into `epics.tar.gz`. The startup script
loads it before anything else uses it:

```
dbLoadDatabase("dbd/rpi3.dbd")
rpi3_registerRecordDeviceDriver(pdbbase)
epicsLoadModule "RPi_IO"
rpiGpioConfigOutput("LED", 17, 0)
```

Everything in this README works unchanged afterwards: `DTYP "RPiGpio"`,
the `rpiGpio*` iocsh commands, `dbior`. A change to `drvRPiGPIO.c` needs
`make` (new `RPi_IO.o`, new archive) and a copy of `epics.tar.gz` to the
card, not a new kernel. To go back to the static link set
`RPI_IO_LOADABLE = NO` in `rpi3App/src/Makefile` and remove the
`epicsLoadModule` line from `st.cmd`. The top-level README section "EPICS
support modules: linked into the kernel or loaded at run time" explains
both modes, the Makefile switch, the recipe for other modules and the
`epicsLoadModule` command; build guide section 10d has the build details.

