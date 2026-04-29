// cmd_table.cpp — handlers do CmdParser do OpenFFBoard pra Configurator GUI.
//
// Slim port da versão de Firmware-Merged: descarta os handlers ligados ao
// init faseado (odrive_arm_hardware/odrive_init_motor), que não existem em
// V56-Stock (a gente usa odrive_main() padrão). Mantém só:
//   - Handshake da Configurator (main.id, sys.lsmain, sys.lsactive, sys.cmdinfo)
//   - axis.* (range, maxtorque, fxratio) — knobs do FFB do volante
//   - fx.*   (gains de spring/damper/friction/inertia)
//   - sys.*  (uid, swver, hwtype, save, reboot, ping, uptime, help, fxtest)
//   - odrv.* (vbus, state, errors, connected — só leitura)
//
// Os handlers que precisam ler estado do ODrive vão via odrive_bridge pra
// evitar a colisão entre class Axis (OpenFFBoard) e class Axis (ODrive).

#include "cmdparser.h"
#include "odrive_bridge.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>
#include <cstdlib>

extern "C" {

static long parse_long(const char *v, long def) {
    if (!v) return def;
    char *end = nullptr;
    long r = strtol(v, &end, 0);
    return end == v ? def : r;
}
static float parse_float(const char *v, float def) {
    if (!v) return def;
    char *end = nullptr;
    float r = strtof(v, &end);
    return end == v ? def : r;
}

// ======================== Handshake do Configurator =========================

static int h_main_id(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    strncpy(r, "1", s);                 // CLSID_MAIN_FFBWHEEL
    return 0;
}

static int h_sys_lsmain(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "1:1:FFB Wheel (1 Axis)");
    return 0;
}

static int h_sys_lsactive(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s,
        "FFB Wheel:main:0:1\n"
        "ODrive (M0):odrv:0:133\n"
        "Axis 0:axis:0:2561\n"
        "Effects:effects:0:2562");
    return 0;
}

static int h_sys_heapfree(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "%u", (unsigned)xPortGetFreeHeapSize());
    return 0;
}

static int h_sys_cmdinfo(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    strncpy(r, "0", s);
    return 0;
}

static int h_sys_temp(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    strncpy(r, "25", s); // placeholder — TODO ler termistor FET
    return 0;
}

static int h_sys_swver(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    strncpy(r, "1.17.0", s);  // hardcoded pra passar MIN_FW da Configurator
    return 0;
}

static int h_sys_hwtype(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    strncpy(r, "ODrive-Wheel", s);
    return 0;
}

// sys.main — id da mainclass atualmente rodando. CRITICO pro probe da
// Configurator: ela compara esse valor contra a lista do sys.lsmain pra
// decidir qual UI carregar. Sem isso, "Can't detect board".
// CLSID_MAIN_FFBWHEEL = 0x01.
static int h_sys_main(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    (void)v;
    // Read sempre retorna FFBWheel; SET (mudar mainclass) ignorado — só temos um.
    snprintf(r, s, "1");
    return 0;
}

// sys.devid — device ID + revision. Configurator usa pra log/diagnóstico.
// STM32F405: DEVID = 0x413, REVID varies por silicon stepping.
static int h_sys_devid(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    uint32_t idcode = *(volatile uint32_t*)0xE0042000; // DBGMCU_IDCODE
    uint32_t devid  = idcode & 0xFFF;
    uint32_t revid  = (idcode >> 16) & 0xFFFF;
    snprintf(r, s, "%lu:%lu", (unsigned long)devid, (unsigned long)revid);
    return 0;
}

// sys.errors / sys.errorsclr — Configurator pode pollar pra mostrar status
static int h_sys_errors_emp(uint8_t, CmdType t, const char*, char *r, size_t s) {
    (void)t;
    strncpy(r, "0", s);   // sem erros do sistema reportados
    return 0;
}
static int h_sys_errorsclr(uint8_t, CmdType, const char*, char *r, size_t s) {
    strncpy(r, "OK", s);
    return 0;
}

// sys.format / sys.flashdump — stubs pra Configurator não reclamar
static int h_sys_format(uint8_t, CmdType, const char*, char *r, size_t s) {
    strncpy(r, "0", s);
    return 0;
}
static int h_sys_flashdump(uint8_t, CmdType, const char*, char *r, size_t s) {
    strncpy(r, "(empty)", s);
    return 0;
}

// sys.vint / sys.vext — voltagem interna/externa em mV. Stub plausível.
static int h_sys_vint(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    extern float odrive_bridge_get_vbus(void);
    snprintf(r, s, "%d", (int)(odrive_bridge_get_vbus() * 1000.0f));
    return 0;
}
static int h_sys_vext(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "0");
    return 0;
}

// sys.heap — info de RAM
static int h_sys_heap(uint8_t, CmdType, const char*, char *r, size_t s) {
    snprintf(r, s, "%u", (unsigned)xPortGetFreeHeapSize());
    return 0;
}

// sys.save — persiste axis params + gains + filtros do FFB na EEPROM
// emulada (sectors 10+11, isolada da NVM ODrive). NÃO toca na config ODrive
// (essa é gravada via ASCII `ss`); então depois de mexer em ambos, usa-se:
//   sys.save!         ← persiste FFB
//   w axis0.requested_state 1; ss   ← persiste ODrive (precisa motor desarmado)
extern "C" int ffb_save_flash(void);
extern "C" int ffb_save_writes(void);
extern "C" int ffb_save_errors(void);
static int h_sys_save(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_EXEC && t != CMD_TYPE_GET) return -1;
    int ok = ffb_save_flash();
    strncpy(r, ok ? "OK" : "FAIL", s);
    return ok ? 0 : -1;
}

// sys.savestat — diagnóstico do último save: writes / errors. Se errors > 0
// algum Flash_Write falhou (provavelmente EE_WriteVariable retornou erro).
static int h_sys_savestat(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "writes=%d errors=%d", ffb_save_writes(), ffb_save_errors());
    return 0;
}

// sys.eetest — escreve 0xABCD no slot reservado 0x04F1 e lê de volta.
// Reporta sucesso/falha no nível baixo da EEPROM emulada. Se isso falhar,
// problema está na page formatting / write/read do EE — não no save lógico.
extern "C" int ffb_eetest(uint16_t want, uint16_t *got_out);
static int h_sys_eetest(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_EXEC && t != CMD_TYPE_GET) return -1;
    uint16_t got = 0;
    int ok = ffb_eetest(0xABCD, &got);
    snprintf(r, s, "%s want=0xABCD got=0x%04X", ok ? "PASS" : "FAIL", (unsigned)got);
    return 0;
}

// sys.eedump — diagnóstico bruto das pages e return codes EE
extern "C" void ffb_eedump(char *buf, int bufsize);
static int h_sys_eedump(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    ffb_eedump(r, (int)s);
    return 0;
}

// sys.reboot — Configurator às vezes oferece botão. Stub respondendo OK
// (nao reboota de verdade pra evitar perda de estado durante probe).
static int h_sys_reboot(uint8_t, CmdType t, const char*, char *r, size_t s) {
    (void)t;
    strncpy(r, "OK", s);
    return 0;
}

static int h_sys_uid(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    uint32_t a = *(uint32_t*)(UID_BASE + 0);
    uint32_t b = *(uint32_t*)(UID_BASE + 4);
    uint32_t c = *(uint32_t*)(UID_BASE + 8);
    snprintf(r, s, "%08lX%08lX%08lX",
             (unsigned long)a, (unsigned long)b, (unsigned long)c);
    return 0;
}

static int h_sys_signature(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    strncpy(r, "0", s);
    return 0;
}

static int h_sys_debug(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    strncpy(r, "0", s);
    return 0;
}

// ======================== main.* (FFB stubs) ================================

static int h_main_hidrate(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    (void)v;
    if (t != CMD_TYPE_GET && t != CMD_TYPE_SET) return -1;
    strncpy(r, "1000", s);   // FFB rodando em 1kHz
    return 0;
}
static int h_main_cfrate(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    (void)v;
    if (t != CMD_TYPE_GET && t != CMD_TYPE_SET) return -1;
    strncpy(r, "1000", s);
    return 0;
}
extern int ffb_diag_ffb_active_flag(void);
static int h_main_ffbactive(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "%d", ffb_diag_ffb_active_flag());
    return 0;
}
static int h_main_hidsendspd(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    static uint8_t s_val = 0;
    if (t == CMD_TYPE_EXEC) {
        strncpy(r, "1000Hz:0,500Hz:1,250Hz:2,125Hz:3", s);
        return 0;
    }
    if (t == CMD_TYPE_SET) s_val = (uint8_t)parse_long(v, s_val);
    snprintf(r, s, "%u", s_val);
    return 0;
}
static int h_main_errors(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    strncpy(r, "0", s);
    return 0;
}
static int h_main_lsbtn(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    strncpy(r, "", s);
    return 0;
}
static int h_main_btntypes(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    (void)v;
    strncpy(r, "0", s);
    return 0;
}
static int h_main_lsain(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    strncpy(r, "", s);
    return 0;
}
static int h_main_aintypes(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    (void)v;
    strncpy(r, "0", s);
    return 0;
}

// ======================== axis.* (params do FFB volante) ====================

extern float ffb_get_axis_range(void);
extern float ffb_get_axis_maxtq(void);
extern float ffb_get_axis_fxratio(void);
extern void  ffb_set_axis_range(float v);
extern void  ffb_set_axis_maxtq(float v);
extern void  ffb_set_axis_fxratio(float v);

// Phase 3.12 — params extras do Axis
extern "C" int  ffb_get_axis_idlespring(void);
extern "C" void ffb_set_axis_idlespring(int v);
extern "C" int  ffb_get_axis_damper(void);
extern "C" void ffb_set_axis_damper(int v);
extern "C" int  ffb_get_axis_inertia(void);
extern "C" void ffb_set_axis_inertia(int v);
extern "C" int  ffb_get_axis_friction(void);
extern "C" void ffb_set_axis_friction(int v);
extern "C" int  ffb_get_axis_esgain(void);
extern "C" void ffb_set_axis_esgain(int v);
extern "C" int  ffb_get_axis_maxtorquerate(void);
extern "C" void ffb_set_axis_maxtorquerate(int v);
extern "C" int  ffb_get_axis_expo(void);
extern "C" void ffb_set_axis_expo(int v);
extern "C" int  ffb_get_axis_exposcale(void);
extern "C" void ffb_set_axis_exposcale(int v);
extern "C" void ffb_axis_zeroenc(void);
extern "C" int   ffb_get_axis_curtorque(void);
extern "C" float ffb_get_axis_curpos(void);
extern "C" float ffb_get_axis_curspd(void);
extern "C" float ffb_get_axis_curaccel(void);

static int h_axis_range(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_SET) ffb_set_axis_range(parse_float(v, ffb_get_axis_range()));
    snprintf(r, s, "%.0f", (double)ffb_get_axis_range());
    return 0;
}
static int h_axis_maxtorque(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_SET) ffb_set_axis_maxtq(parse_float(v, ffb_get_axis_maxtq()));
    snprintf(r, s, "%.2f", (double)ffb_get_axis_maxtq());
    return 0;
}
static int h_axis_fxratio(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_SET) ffb_set_axis_fxratio(parse_float(v, ffb_get_axis_fxratio()));
    snprintf(r, s, "%.2f", (double)ffb_get_axis_fxratio());
    return 0;
}
static int h_axis_invert(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    static int s_inv = 0;
    if (t == CMD_TYPE_SET) s_inv = parse_long(v, s_inv) ? 1 : 0;
    snprintf(r, s, "%d", s_inv);
    return 0;
}
static int h_axis_drvtype(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    (void)v;
    if (t == CMD_TYPE_EXEC) { snprintf(r, s, "5:ODrive (M0)"); return 0; }
    strncpy(r, "5", s);
    return 0;
}
static int h_axis_enctype(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    (void)v;
    if (t == CMD_TYPE_EXEC) { snprintf(r, s, "1:ODrive Internal"); return 0; }
    strncpy(r, "1", s);
    return 0;
}
extern float ffb_get_pos_degrees(void);
static int h_axis_pos(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "%.2f", (double)ffb_get_pos_degrees());
    return 0;
}

// Phase 3.12 — handlers pros axis effects e params extras
#define AXIS_INT_HANDLER(name)                                                 \
    static int h_axis_##name(uint8_t, CmdType t, const char *v, char *r, size_t s) { \
        if (t == CMD_TYPE_SET) ffb_set_axis_##name((int)parse_long(v, ffb_get_axis_##name())); \
        snprintf(r, s, "%d", ffb_get_axis_##name());                           \
        return 0;                                                              \
    }
AXIS_INT_HANDLER(idlespring)
AXIS_INT_HANDLER(damper)
AXIS_INT_HANDLER(inertia)
AXIS_INT_HANDLER(friction)
AXIS_INT_HANDLER(esgain)
AXIS_INT_HANDLER(maxtorquerate)
AXIS_INT_HANDLER(expo)
AXIS_INT_HANDLER(exposcale)
#undef AXIS_INT_HANDLER

static int h_axis_zeroenc(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t == CMD_TYPE_EXEC || t == CMD_TYPE_GET) ffb_axis_zeroenc();
    strncpy(r, "OK", s);
    return 0;
}

// Live readouts (read-only)
static int h_axis_curtorque(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "%d", ffb_get_axis_curtorque());
    return 0;
}
static int h_axis_curpos(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "%.2f", (double)ffb_get_axis_curpos());
    return 0;
}
static int h_axis_curspd(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "%.2f", (double)ffb_get_axis_curspd());
    return 0;
}
static int h_axis_curaccel(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "%.2f", (double)ffb_get_axis_curaccel());
    return 0;
}

// ======================== fx.* (gains wired ao EffectsCalculator) ===========
// effect_gain_t tem .spring/.damper/.friction/.inertia (uint8_t cada).
// global_gain é separado (master). Defaults: spring=64, damper=64, friction=254,
// inertia=127, master=255.
//
// Exposto via odrive_bridge no formato C linkage pra evitar puxar EffectsCalc
// inteiro pra dentro do cmd_table (header conflita com class Axis).
extern "C" int  ffb_get_master_gain(void);
extern "C" void ffb_set_master_gain(int v);
extern "C" int  ffb_get_spring_gain(void);
extern "C" void ffb_set_spring_gain(int v);
extern "C" int  ffb_get_damper_gain(void);
extern "C" void ffb_set_damper_gain(int v);
extern "C" int  ffb_get_friction_gain(void);
extern "C" void ffb_set_friction_gain(int v);
extern "C" int  ffb_get_inertia_gain(void);
extern "C" void ffb_set_inertia_gain(int v);

#define FX_GAIN_HANDLER(name)                                                  \
    static int h_fx_##name(uint8_t, CmdType t, const char *v, char *r, size_t s) { \
        if (t == CMD_TYPE_EXEC) {                                              \
            strncpy(r, "Full:255,Half:128,None:0", s);                         \
            return 0;                                                          \
        }                                                                      \
        if (t == CMD_TYPE_SET) {                                               \
            int val = (int)parse_long(v, ffb_get_##name##_gain());             \
            if (val < 0)   val = 0;                                            \
            if (val > 255) val = 255;                                          \
            ffb_set_##name##_gain(val);                                        \
        }                                                                      \
        snprintf(r, s, "%d", ffb_get_##name##_gain());                         \
        return 0;                                                              \
    }
FX_GAIN_HANDLER(spring)
FX_GAIN_HANDLER(damper)
FX_GAIN_HANDLER(friction)
FX_GAIN_HANDLER(inertia)
#undef FX_GAIN_HANDLER

// fx.master — global_gain (master gain). Game também escreve aqui via HID
// Set Gain Report; valor da última fonte vence.
static int h_fx_master(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_EXEC) {
        strncpy(r, "Full:255,Half:128,None:0", s);
        return 0;
    }
    if (t == CMD_TYPE_SET) {
        int val = (int)parse_long(v, ffb_get_master_gain());
        if (val < 0)   val = 0;
        if (val > 255) val = 255;
        ffb_set_master_gain(val);
    }
    snprintf(r, s, "%d", ffb_get_master_gain());
    return 0;
}

// Phase 3.4 — Filtros biquad lowpass por tipo de efeito.
// freq = cutoff em Hz (1-500), q = Q-factor × 100 (1-500, default ~70 ≈ 0.707).
// Convenção OpenFFBoard:
//   filterCfFreq/Q  → Constant Force (HID streaming, é o filtro mais usado)
//   filterFrFreq/Q  → Friction
//   filterDaFreq/Q  → Damper
//   filterInFreq/Q  → Inertia
extern "C" int  ffb_get_filter_constant_freq(void);
extern "C" void ffb_set_filter_constant_freq(int v);
extern "C" int  ffb_get_filter_constant_q(void);
extern "C" void ffb_set_filter_constant_q(int v);
extern "C" int  ffb_get_filter_friction_freq(void);
extern "C" void ffb_set_filter_friction_freq(int v);
extern "C" int  ffb_get_filter_friction_q(void);
extern "C" void ffb_set_filter_friction_q(int v);
extern "C" int  ffb_get_filter_damper_freq(void);
extern "C" void ffb_set_filter_damper_freq(int v);
extern "C" int  ffb_get_filter_damper_q(void);
extern "C" void ffb_set_filter_damper_q(int v);
extern "C" int  ffb_get_filter_inertia_freq(void);
extern "C" void ffb_set_filter_inertia_freq(int v);
extern "C" int  ffb_get_filter_inertia_q(void);
extern "C" void ffb_set_filter_inertia_q(int v);

#define FX_FILTER_HANDLER(cmd_name, accessor)                                  \
    static int h_fx_##cmd_name(uint8_t, CmdType t, const char *v, char *r, size_t s) { \
        if (t == CMD_TYPE_EXEC) {                                              \
            strncpy(r, "Default:0", s);                                        \
            return 0;                                                          \
        }                                                                      \
        if (t == CMD_TYPE_SET) {                                               \
            int val = (int)parse_long(v, ffb_get_##accessor());                \
            ffb_set_##accessor(val);                                           \
        }                                                                      \
        snprintf(r, s, "%d", ffb_get_##accessor());                            \
        return 0;                                                              \
    }
FX_FILTER_HANDLER(filterCfFreq, filter_constant_freq)
FX_FILTER_HANDLER(filterCfQ,    filter_constant_q)
FX_FILTER_HANDLER(filterFrFreq, filter_friction_freq)
FX_FILTER_HANDLER(filterFrQ,    filter_friction_q)
FX_FILTER_HANDLER(filterDaFreq, filter_damper_freq)
FX_FILTER_HANDLER(filterDaQ,    filter_damper_q)
FX_FILTER_HANDLER(filterInFreq, filter_inertia_freq)
FX_FILTER_HANDLER(filterInQ,    filter_inertia_q)
#undef FX_FILTER_HANDLER

// ======================== odrv.* (read-only via bridge) =====================

static int h_odrv_vbus(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "%d", (int)(odrive_bridge_get_vbus() * 1000.0f));
    return 0;
}
static int h_odrv_connected(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    strncpy(r, "1", s);
    return 0;
}
// Mock locais — Configurator pergunta, não tem efeito hardware.
static int h_odrv_canid(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    static uint16_t s_val = 0;
    if (t == CMD_TYPE_SET) s_val = (uint16_t)parse_long(v, s_val);
    snprintf(r, s, "%u", (unsigned)s_val);
    return 0;
}
static int h_odrv_canspd(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    static uint8_t s_val = 3;
    if (t == CMD_TYPE_SET) s_val = (uint8_t)parse_long(v, s_val);
    snprintf(r, s, "%u", (unsigned)s_val);
    return 0;
}
static int h_odrv_maxtorque(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    static uint16_t s_val = 100;
    if (t == CMD_TYPE_SET) s_val = (uint16_t)parse_long(v, s_val);
    snprintf(r, s, "%u", (unsigned)s_val);
    return 0;
}

// ======================== sys.* (utilities) =================================

static int h_help(uint8_t, CmdType, const char*, char *reply, size_t size) {
    size_t off = 0;
    for (size_t i = 0; i < cmdtable_size; i++) {
        int n = snprintf(reply + off, size - off, "%s%s.%s",
                         i == 0 ? "" : " | ",
                         cmdtable[i].class_name, cmdtable[i].cmd_name);
        if (n < 0 || (size_t)n >= size - off) break;
        off += (size_t)n;
    }
    return 0;
}
static int h_sys_uptime(uint8_t, CmdType, const char*, char *r, size_t s) {
    snprintf(r, s, "%lums", (unsigned long)HAL_GetTick());
    return 0;
}
static int h_sys_ping(uint8_t, CmdType, const char*, char *r, size_t s) {
    strncpy(r, "pong", s); return 0;
}

// fxtest — diagnóstico FFB sumarizado em uma linha
extern int   ffb_is_active(void);
extern float ffb_get_pending_torque_nm(void);
extern float ffb_get_speed(void);
extern int   ffb_count_active_effects(void);
static int h_sys_fxtest(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "ffb=%d pos=%.1f spd=%.1f trq=%.3f fx=%d",
        ffb_is_active(),
        (double)ffb_get_pos_degrees(),
        (double)ffb_get_speed(),
        (double)ffb_get_pending_torque_nm(),
        ffb_count_active_effects());
    return 0;
}

// ======================== Tabela ============================================

// Metadados de classe — usados pelo cmdparser pra responder os meta-commands
// (id/name/help/cmdinfo/cmduid/instance) automaticamente. CLSIDs batem com o
// que sys.lsactive retorna pra Configurator carregar a UI certa de cada tab.
const CmdClassMeta cmdclasses[] = {
    { "main",    1,    0, "FFB Wheel"      },  // CLSID_MAIN_FFBWHEEL
    { "sys",     0,    0, "System"         },  // sem clsid próprio
    { "odrv",    133,  0, "ODrive (M0)"    },  // 0x85 truncado p/ 16-bit
    { "axis",    2561, 0, "Axis 0"         },  // 0xA01
    { "fx",      2562, 0, "Effects"        },  // 0xA02
    { "effects", 2562, 0, "Effects"        },  // alias — Configurator pergunta com qq nome
};
const size_t cmdclasses_size = sizeof(cmdclasses) / sizeof(cmdclasses[0]);

const CmdEntry cmdtable[] = {
    // Handshake do OpenFFBoard Configurator
    { "main",  "id",           h_main_id },
    { "sys",   "lsmain",       h_sys_lsmain },
    { "sys",   "lsactive",     h_sys_lsactive },
    { "sys",   "heapfree",     h_sys_heapfree },
    { "sys",   "cmdinfo",      h_sys_cmdinfo },
    { "sys",   "temp",         h_sys_temp },

    { "main",  "hidrate",      h_main_hidrate },
    { "main",  "cfrate",       h_main_cfrate },
    { "main",  "ffbactive",    h_main_ffbactive },
    { "main",  "hidsendspd",   h_main_hidsendspd },
    { "main",  "errors",       h_main_errors },
    { "main",  "lsbtn",        h_main_lsbtn },
    { "main",  "btntypes",     h_main_btntypes },
    { "main",  "lsain",        h_main_lsain },
    { "main",  "aintypes",     h_main_aintypes },

    // fx.* — gains wired ao EffectsCalculator (Phase 3.3).
    // Default: spring=64 damper=64 friction=254 inertia=127 master=255.
    { "fx",    "spring",       h_fx_spring },
    { "fx",    "damper",       h_fx_damper },
    { "fx",    "friction",     h_fx_friction },
    { "fx",    "inertia",      h_fx_inertia },
    { "fx",    "master",       h_fx_master },         // global_gain
    // Filter params — biquad lowpass por tipo de efeito (Phase 3.4)
    { "fx",    "filterCfFreq", h_fx_filterCfFreq },   // Constant force
    { "fx",    "filterCfQ",    h_fx_filterCfQ },
    { "fx",    "filterFrFreq", h_fx_filterFrFreq },   // Friction
    { "fx",    "filterFrQ",    h_fx_filterFrQ },
    { "fx",    "filterDaFreq", h_fx_filterDaFreq },   // Damper
    { "fx",    "filterDaQ",    h_fx_filterDaQ },
    { "fx",    "filterInFreq", h_fx_filterInFreq },   // Inertia
    { "fx",    "filterInQ",    h_fx_filterInQ },

    // axis.* — knobs essenciais do FFB do volante
    { "axis",  "range",         h_axis_range },
    { "axis",  "maxtorque",     h_axis_maxtorque },
    { "axis",  "fxratio",       h_axis_fxratio },
    { "axis",  "invert",        h_axis_invert },
    { "axis",  "drvtype",       h_axis_drvtype },
    { "axis",  "enctype",       h_axis_enctype },
    { "axis",  "pos",           h_axis_pos },
    // axis.* extras (Phase 3.12) — efeitos sempre-ativos somados ao FFB
    { "axis",  "idlespring",    h_axis_idlespring },     // mola quando jogo desligado (0-255)
    { "axis",  "axisdamper",    h_axis_damper },         // damper sempre ativo (0-255)
    { "axis",  "axisinertia",   h_axis_inertia },        // inertia sempre ativa (0-255)
    { "axis",  "axisfriction",  h_axis_friction },       // friction sempre ativa (0-255)
    { "axis",  "esgain",        h_axis_esgain },         // batente eletrônico (0-255)
    { "axis",  "maxtorquerate", h_axis_maxtorquerate },  // slew limit (counts/ms, 0=off)
    { "axis",  "expo",          h_axis_expo },           // curva exponencial (-32767..32767)
    { "axis",  "exposcale",     h_axis_exposcale },      // divisor pro expo (1-255)
    { "axis",  "zeroenc",       h_axis_zeroenc },        // zera posição atual
    // Live readouts (read-only)
    { "axis",  "curtorque",     h_axis_curtorque },
    { "axis",  "curpos",        h_axis_curpos },
    { "axis",  "curspd",        h_axis_curspd },
    { "axis",  "curaccel",      h_axis_curaccel },

    // sys.* meta + utilities
    { "sys",   "swver",        h_sys_swver },
    { "sys",   "hwtype",       h_sys_hwtype },
    { "sys",   "uid",          h_sys_uid },
    { "sys",   "signature",    h_sys_signature },
    { "sys",   "debug",        h_sys_debug },
    { "sys",   "main",         h_sys_main },          // ID da mainclass atual
    { "sys",   "devid",        h_sys_devid },         // STM32 device + rev id
    { "sys",   "errors",       h_sys_errors_emp },    // lista erros (vazia)
    { "sys",   "errorsclr",    h_sys_errorsclr },     // limpa erros
    { "sys",   "format",       h_sys_format },        // erase config
    { "sys",   "flashdump",    h_sys_flashdump },     // dump flash vars
    { "sys",   "vint",         h_sys_vint },          // VBUS interno em mV
    { "sys",   "vext",         h_sys_vext },          // tensão externa
    { "sys",   "heap",         h_sys_heap },          // free heap
    { "sys",   "save",         h_sys_save },          // persist config
    { "sys",   "savestat",     h_sys_savestat },      // diag last save
    { "sys",   "eetest",       h_sys_eetest },        // EEPROM low-level test
    { "sys",   "eedump",       h_sys_eedump },        // EEPROM raw status
    { "sys",   "reboot",       h_sys_reboot },        // reset chip
    { "sys",   "uptime",       h_sys_uptime },
    { "sys",   "ping",         h_sys_ping },
    { "sys",   "fxtest",       h_sys_fxtest },

    // odrv.* (read-only; Configurator não escreve hardware aqui)
    { "odrv",  "vbus",         h_odrv_vbus },
    { "odrv",  "connected",    h_odrv_connected },
    { "odrv",  "canid",        h_odrv_canid },
    { "odrv",  "canspd",       h_odrv_canspd },
    { "odrv",  "maxtorque",    h_odrv_maxtorque },
};
const size_t cmdtable_size = sizeof(cmdtable) / sizeof(cmdtable[0]);

} // extern "C"
