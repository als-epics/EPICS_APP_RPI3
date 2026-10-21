/*
 * dwcDebugVars.c - iocsh "var" access to runtime switches.
 *   var hb_wdog_enable 0        disarm the BCM2835 hardware watchdog (heartbeat.c)
 * With RPI_IOC_DEBUG (see Makefile) also the hang-hunt switches:
 *   var dwc_otg_fix_enable 0    original FreeBSD channel/NAK behaviour
 *   var dwc_otg_trace_enable 1  UART breadcrumb trace (very verbose)
 *   var dwc_otg_wprobe_enable 1 GSNPSID read-back after each DWC register write
 *   var dwc_otg_icmask_enable 1|0|-1  USB IRQ mask in DWC core | IRQ-ctrl | auto by boot#
 *   var dwc_otg_icmask_probe 1  IRQ-masked 3-marker probe around IRQ-ctrl writes
 *   var dwc_otg_diag_period N   seconds between "DWC_OTG t=" dumps (0 = off)
 */
#include <epicsExport.h>

#ifdef __rtems__
extern volatile int hb_wdog_enable;
#else
static volatile int hb_wdog_enable;          /* host build placeholder */
#endif
/* the (char *) cast inside epicsExportAddress drops the volatile qualifier */
epicsExportAddress(int, hb_wdog_enable);

#ifdef RPI_IOC_DEBUG
#ifdef __rtems__
extern volatile int dwc_otg_fix_enable;
extern volatile int dwc_otg_trace_enable;
extern volatile int dwc_otg_wprobe_enable;
extern volatile int dwc_otg_icmask_enable;
extern volatile int dwc_otg_icmask_probe;
extern volatile int dwc_otg_diag_period;
#else
static volatile int dwc_otg_fix_enable;
static volatile int dwc_otg_trace_enable;
static volatile int dwc_otg_wprobe_enable;
static volatile int dwc_otg_icmask_enable;
static volatile int dwc_otg_icmask_probe;
static volatile int dwc_otg_diag_period;
#endif
epicsExportAddress(int, dwc_otg_fix_enable);
epicsExportAddress(int, dwc_otg_trace_enable);
epicsExportAddress(int, dwc_otg_wprobe_enable);
epicsExportAddress(int, dwc_otg_icmask_enable);
epicsExportAddress(int, dwc_otg_icmask_probe);
epicsExportAddress(int, dwc_otg_diag_period);
#endif /* RPI_IOC_DEBUG */
