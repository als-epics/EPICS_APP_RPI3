/*
 * Minimal stub used when the FIQ FSM is disabled in the build.
 * Provides the only hard symbol the modified libbsd dwc_otg.c references.
 * All other FIQ entry points (dwc_fiq_get_state, dwc_fiq_log_drain) have
 * weak fallbacks in libbsd that return NULL/0, leaving sc_fiq == NULL
 * so all the shadow-state writes are skipped.
 */
#include <stdint.h>

volatile uint32_t g_fiq_spsr_fix_count;
