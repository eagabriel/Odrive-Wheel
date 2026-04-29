/*
 * Stub definitions for debug instrumentation globals that our v0.5.6 tree
 * references (added during Firmware-Merged iteration). Declared as volatile
 * uint32 and initialized to 0. odrive_drv_trace is a no-op here — we don't
 * need trace output for the stock-ODrive v0.5.6 build.
 */

#include <stdint.h>

// Counters referenced from motor.cpp, drv8301.cpp etc.
volatile uint32_t g_sat_count = 0;
volatile uint32_t g_last_sat_adc = 0;
volatile uint32_t g_apply_pwm_count = 0;
volatile uint32_t g_pwm_update_cb_count = 0;
volatile uint32_t g_last_disarm_error = 0;
volatile uint32_t g_arm_last_cl_ptr = 0;
volatile uint32_t g_last_isr_armed = 0;
volatile uint32_t g_last_isr_cl_ptr = 0;
volatile uint32_t g_arm_call_cnt = 0;
volatile uint32_t g_arm_success_cnt = 0;
volatile uint32_t g_disarm_call_cnt = 0;
volatile uint32_t g_brake_resistor_armed_snapshot = 0;
volatile uint32_t g_enable_brake_snapshot = 0;
volatile uint32_t g_disarm_caller = 0;
volatile uint32_t g_disarm_reason_checks_fail = 0;
volatile uint32_t g_disarm_callers[8] = {0,0,0,0,0,0,0,0};
volatile uint8_t g_disarm_idx = 0;
volatile uint32_t g_pwm_status_none_cnt = 0;
volatile uint32_t g_pwm_status_init_cnt = 0;
volatile uint32_t g_pwm_status_other_cnt = 0;
volatile uint32_t g_pwm_cl_null_cnt = 0;
volatile uint32_t g_pwm_is_armed_cnt = 0;
volatile uint32_t g_pwm_last_status = 0;
volatile uint32_t g_nfault_flicker_count = 0;

// Trace function — no-op in stock build
void odrive_drv_trace(uint8_t v) {
    (void)v;
}
