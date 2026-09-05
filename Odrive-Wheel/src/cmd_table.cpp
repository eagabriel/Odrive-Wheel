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
#include <cmath>

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

// FET thermistor onboard em °C. NaN vira "0.0" pra não quebrar parsers.
static int h_sys_temp(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    float temp = odrive_bridge_get_fet_temp();
    if (std::isnan(temp)) {
        snprintf(r, s, "0.0");
    } else {
        snprintf(r, s, "%.1f", temp);
    }
    return 0;
}

// Motor thermistor offboard (NTC via GPIO). NaN vira "0.0".
static int h_sys_motortemp(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    float temp = odrive_bridge_get_motor_temp();
    if (std::isnan(temp)) {
        snprintf(r, s, "0.0");
    } else {
        snprintf(r, s, "%.1f", temp);
    }
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

// Phase 4.x — sys.vbusdiv: divisor de tensão pra leitura de VBUS.
// Default 19 (MKS XDrive Mini), ODrive v3.6 oficial = 11. Range 1-50.
extern "C" int  ffb_get_vbus_divider(void);
extern "C" void ffb_set_vbus_divider(int v);
static int h_sys_vbusdiv(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_SET) {
        long val = parse_long(v, ffb_get_vbus_divider());
        if (val < 1)  val = 1;
        if (val > 50) val = 50;
        ffb_set_vbus_divider((int)val);
    }
    snprintf(r, s, "%d", ffb_get_vbus_divider());
    return 0;
}

// sys.eeformat! — escape hatch que força format completo do EE com clear de
// error flags entre cada operação. Usar quando bootRC != 0 e sys.save! continua
// falhando (caso típico: flash com flags PGAERR/PGSERR latched de operação
// anterior, ou pages com 0x00 stuck do .bin antigo que não foi gap-fillado).
extern "C" void ffb_eeformat(char *buf, int bufsize);
static int h_sys_eeformat(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_EXEC && t != CMD_TYPE_GET) return -1;
    ffb_eeformat(r, (int)s);
    return 0;
}

// ==================== GPIO inputs (1-4) — handlers ====================
// Sintaxe: gpio.<inst>.<field>?/= onde inst = 1..4 (= GPIO 1..4).
// Fields: mode (0/1/2/3 = off/button/axis/zerowheel), idx (botão 0-63 ou
// eixo 0-3), invert (0/1), amin/amax (0-4095, só axis), cur (read-only).
extern "C" {
#include "gpio_inputs.h"
}

static int h_gpio_mode(uint8_t inst, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_GET) {
        snprintf(r, s, "%u", (unsigned)gpio_inputs_get_mode(inst));
        return 0;
    } else if (t == CMD_TYPE_SET) {
        long val = parse_long(v, -1);
        if (val < 0 || val > GPIO_INPUT_ZEROWHEEL) return -1;
        if (gpio_inputs_set_mode(inst, (uint8_t)val) != 0) return -1;
        snprintf(r, s, "%ld", val);
        return 0;
    }
    return -1;
}
static int h_gpio_idx(uint8_t inst, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_GET) {
        snprintf(r, s, "%u", (unsigned)gpio_inputs_get_idx(inst));
        return 0;
    } else if (t == CMD_TYPE_SET) {
        long val = parse_long(v, -1);
        if (val < 0 || val > 63) return -1;
        if (gpio_inputs_set_idx(inst, (uint8_t)val) != 0) return -1;
        snprintf(r, s, "%ld", val);
        return 0;
    }
    return -1;
}
static int h_gpio_invert(uint8_t inst, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_GET) {
        snprintf(r, s, "%u", (unsigned)gpio_inputs_get_invert(inst));
        return 0;
    } else if (t == CMD_TYPE_SET) {
        long val = parse_long(v, -1);
        if (val < 0 || val > 1) return -1;
        if (gpio_inputs_set_invert(inst, (uint8_t)val) != 0) return -1;
        snprintf(r, s, "%ld", val);
        return 0;
    }
    return -1;
}
static int h_gpio_amin(uint8_t inst, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_GET) {
        snprintf(r, s, "%u", (unsigned)gpio_inputs_get_amin(inst));
        return 0;
    } else if (t == CMD_TYPE_SET) {
        long val = parse_long(v, -1);
        if (val < 0 || val > 4095) return -1;
        if (gpio_inputs_set_amin(inst, (uint16_t)val) != 0) return -1;
        snprintf(r, s, "%ld", val);
        return 0;
    }
    return -1;
}
static int h_gpio_amax(uint8_t inst, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_GET) {
        snprintf(r, s, "%u", (unsigned)gpio_inputs_get_amax(inst));
        return 0;
    } else if (t == CMD_TYPE_SET) {
        long val = parse_long(v, -1);
        if (val < 0 || val > 4095) return -1;
        if (gpio_inputs_set_amax(inst, (uint16_t)val) != 0) return -1;
        snprintf(r, s, "%ld", val);
        return 0;
    }
    return -1;
}
static int h_gpio_cur(uint8_t inst, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "%u", (unsigned)gpio_inputs_read_raw(inst));
    return 0;
}

// Último valor filtrado (após Biquad do axis processor). 0..4095 em counts
// ADC, mesmo formato do `cur`. 65535 = nunca foi processado (não-AXIS).
extern "C" uint16_t gpio_inputs_get_filt(uint8_t inst);
static int h_gpio_filt(uint8_t inst, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_GET) return -1;
    snprintf(r, s, "%u", (unsigned)gpio_inputs_get_filt(inst));
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
extern "C" int  ffb_get_axis_esdamp(void);
extern "C" void ffb_set_axis_esdamp(int v);
extern "C" int  ffb_get_axis_maxtorquerate(void);
extern "C" void ffb_set_axis_maxtorquerate(int v);
extern "C" int  ffb_get_axis_expo(void);
extern "C" void ffb_set_axis_expo(int v);
extern "C" int  ffb_get_axis_exposcale(void);
extern "C" void ffb_set_axis_exposcale(int v);
extern "C" void ffb_axis_zeroenc(void);
extern "C" int  odrive_bridge_start_anticogcal(void);
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
// Phase 4.x — axis.invert agora opera de verdade (era stub que só guardava
// um inteiro local). Inverte direção lógica do volante em runtime: HID position
// e FFB torque do jogo são negados em sincronia. Persiste em EE via
// ADR_AXIS1_CONFIG bit 0 (sys.save!).
extern "C" int  ffb_get_axis_invert(void);
extern "C" void ffb_set_axis_invert(int v);
extern "C" int  ffb_get_ffb_invert(void);
extern "C" void ffb_set_ffb_invert(int v);
static int h_axis_invert(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_SET) {
        int val = parse_long(v, ffb_get_axis_invert()) ? 1 : 0;
        ffb_set_axis_invert(val);
    }
    snprintf(r, s, "%d", ffb_get_axis_invert());
    return 0;
}
// axis.ffbinvert — inverte APENAS o torque FFB do jogo (independente de
// axis.invert que só afeta a posição HID). Útil pra FFFSake Forwarder e
// stacks que já mandam FFB com sinal trocado.
static int h_axis_ffbinvert(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_SET) {
        int val = parse_long(v, ffb_get_ffb_invert()) ? 1 : 0;
        ffb_set_ffb_invert(val);
    }
    snprintf(r, s, "%d", ffb_get_ffb_invert());
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
AXIS_INT_HANDLER(esdamp)
AXIS_INT_HANDLER(maxtorquerate)
AXIS_INT_HANDLER(expo)
AXIS_INT_HANDLER(exposcale)
#undef AXIS_INT_HANDLER

static int h_axis_zeroenc(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t == CMD_TYPE_EXEC || t == CMD_TYPE_GET) ffb_axis_zeroenc();
    strncpy(r, "OK", s);
    return 0;
}

// axis.zeroofs — leitura/escrita do offset persistente em graus.
// Útil pra UI mostrar o que zeroenc! capturou, ou pra resetar manualmente
// (axis.zeroofs=0 → desfaz o centro). Persiste em sys.save! via 2 slots EE.
extern "C" float ffb_get_axis_zeroofs(void);
extern "C" void  ffb_set_axis_zeroofs(float v);
static int h_axis_zeroofs(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_SET) ffb_set_axis_zeroofs(parse_float(v, ffb_get_axis_zeroofs()));
    snprintf(r, s, "%.3f", (double)ffb_get_axis_zeroofs());
    return 0;
}

// Contadores do callback do Z — diagnostico de leitura do pulso index
// pelo STM32 EXTI. Escreve qualquer valor (tipico 0) pra resetar.
extern "C" uint32_t ffb_get_z_hits(void);
extern "C" void     ffb_set_z_hits(uint32_t v);
extern "C" uint32_t ffb_get_z_glitches(void);
extern "C" void     ffb_set_z_glitches(uint32_t v);
static int h_axis_zhits(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_SET) ffb_set_z_hits((uint32_t)parse_long(v, (long)ffb_get_z_hits()));
    snprintf(r, s, "%lu", (unsigned long)ffb_get_z_hits());
    return 0;
}
static int h_axis_zglitch(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_SET) ffb_set_z_glitches((uint32_t)parse_long(v, (long)ffb_get_z_glitches()));
    snprintf(r, s, "%lu", (unsigned long)ffb_get_z_glitches());
    return 0;
}

// Anticogging calibration trigger via cmdparser OpenFFBoard.
// Workaround pra `calib_anticogging` ser readonly no YAML do ODrive (write
// via ASCII responde "not implemented"). Síntaxe: axis.anticogcal!
//
// Pré-requisitos pra cal funcionar (firmware checa internamente):
//   - axis sem erros (axis0.error == 0)
//   - motor calibrado + encoder pronto
//   - control_mode = POSITION_CONTROL (3) — start_anticogging_calibration
//     força isso, mas vel_gain/pos_gain precisam estar razoáveis pro servo
//     manter posição em cada um dos 3600 pontos
//   - axis em CLOSED_LOOP_CONTROL (state=8)
//   - motor LIVRE (sem volante físico)
//
// Retorna OK se disparou, FAIL se axis tem erros ou se start não pôde
// rodar (controller.cpp:50 só seta a flag se axis.error == ERROR_NONE).
static int h_axis_anticogcal(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_EXEC && t != CMD_TYPE_GET) return -1;
    int ok = odrive_bridge_start_anticogcal();
    strncpy(r, ok ? "OK" : "FAIL (verifique axis0.error e estado closed_loop)", s);
    return ok ? 0 : -1;
}

// ============= GPIO axis processor (AnalogAxisProcessing port simplificado) =============
// Apenas Biquad filter (por canal HID) + flag autocal global.
// Min/max real ficam no AMIN/AMAX existentes por GPIO (gpio_inputs.N.amin/amax).
extern "C" {
    int   axis_proc_get_filter_enabled(void);
    void  axis_proc_set_filter_enabled(int en);
    int   axis_proc_get_autorange_enabled(void);
    void  axis_proc_set_autorange_enabled(int en);
    float axis_proc_get_filter_freq(void);
    void  axis_proc_set_filter_freq(float hz);
}

// axis.gpiofilt — habilita filtro low-pass Biquad pros GPIOs em modo axis
static int h_axis_gpiofilt(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_SET) axis_proc_set_filter_enabled((int)parse_long(v, axis_proc_get_filter_enabled()));
    snprintf(r, s, "%d", axis_proc_get_filter_enabled());
    return 0;
}

// axis.gpiofiltf — cutoff do filtro em Hz (0.5-500)
static int h_axis_gpiofiltf(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_SET) axis_proc_set_filter_freq(parse_float(v, axis_proc_get_filter_freq()));
    snprintf(r, s, "%.2f", (double)axis_proc_get_filter_freq());
    return 0;
}

// axis.gpioautocal — habilita autocal global (logica de update em gpio_inputs.cpp,
// que escreve direto nos AMIN/AMAX existentes por GPIO)
static int h_axis_gpioautocal(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t == CMD_TYPE_SET) axis_proc_set_autorange_enabled((int)parse_long(v, axis_proc_get_autorange_enabled()));
    snprintf(r, s, "%d", axis_proc_get_autorange_enabled());
    return 0;
}

// EQ por banda — bridge pra ffb_eq_* (ver ffb_task.cpp).
extern "C" {
    void  ffb_eq_set_gain_db(int band, float gainDb);
    float ffb_eq_get_gain_db(int band);
    float ffb_eq_get_freq_hz(int band);
    float ffb_eq_get_q(int band);
}

// Macro pra reduzir copy/paste — gera handlers de get/set pro ganho de cada
// banda. Band: 0=WEIGHT (low-shelf), 1=CHASSIS (peak), 2=ROAD (high-shelf).
// Valor em dB com 0.1 de resolução, clamped pelo EqCascade a ±12.
#define EQ_GAIN_HANDLER(name, band_id) \
    static int h_axis_eq##name(uint8_t, CmdType t, const char *v, char *r, size_t s) { \
        if (t == CMD_TYPE_SET) ffb_eq_set_gain_db(band_id, parse_float(v, ffb_eq_get_gain_db(band_id))); \
        snprintf(r, s, "%.1f", (double)ffb_eq_get_gain_db(band_id)); \
        return 0; \
    }
EQ_GAIN_HANDLER(weight,  0)
EQ_GAIN_HANDLER(chassis, 1)
EQ_GAIN_HANDLER(road,    2)
#undef EQ_GAIN_HANDLER

// Read-only — centro e Q de cada banda. UI usa pra plotar a resposta sem
// duplicar os valores hardcoded no firmware.
#define EQ_INFO_HANDLER(name, band_id, fn, fmt) \
    static int h_axis_eq##name(uint8_t, CmdType t, const char*, char *r, size_t s) { \
        if (t != CMD_TYPE_GET) return -1; \
        snprintf(r, s, fmt, (double)fn(band_id)); \
        return 0; \
    }
EQ_INFO_HANDLER(weightfreq,  0, ffb_eq_get_freq_hz, "%.2f")
EQ_INFO_HANDLER(chassisfreq, 1, ffb_eq_get_freq_hz, "%.2f")
EQ_INFO_HANDLER(roadfreq,    2, ffb_eq_get_freq_hz, "%.2f")
EQ_INFO_HANDLER(weightq,     0, ffb_eq_get_q,       "%.2f")
EQ_INFO_HANDLER(chassisq,    1, ffb_eq_get_q,       "%.2f")
EQ_INFO_HANDLER(roadq,       2, ffb_eq_get_q,       "%.2f")
#undef EQ_INFO_HANDLER

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

// encraw! — snapshot dos contadores SPI do encoder + último raw recebido
// pra debug do AS5047. Resposta:
//   "ok=N pty=N ef=N xfr=N last=0xNNNN pos=NNNN"
// Onde:
//   ok    — # transações que passaram parity + EF check (pos_abs atualiza)
//   pty   — # transações rejeitadas por parity ruim
//   ef    — # transações com EF=1 (chip reportou erro de comm)
//   xfr   — # transações com transfer() falhando (timeout HAL)
//   last  — raw 16-bit recebido na última transação (qualquer resultado)
//   pos   — pos_abs cacheado (só atualiza em transação OK)
// Roda 2× com 1s entre as chamadas pra ver os deltas — diz EXATAMENTE
// onde a falha tá:
//   ok subindo = sucesso, pos deveria mudar conforme magneto move
//   pty subindo = SPI integridade (bits flippando)
//   ef subindo = AS5047 setando EF (precisa repensar setup)
//   xfr subindo = transfer() falhando (raro)
//   last variando entre calls = AS5047 enviando dado fresh ✓
//   last fixo = AS5047 enviando mesmo byte (chip travado ou OTP errado)
// magnet! — leitura do DIAAGC register do AS5047. Resposta:
//   "agc=N magl=N magh=N cof=N lf=N updates=N status=TXT"
// Interpretação:
//   agc 0-255   — AGC value. Ideal ~128. <50 = magneto perto demais. >200 = longe.
//   magl=1      — Magnetic Low: campo fraco demais (magneto longe ou faltando)
//   magh=1      — Magnetic High: campo forte demais (magneto perto demais)
//   cof=1       — CORDIC Overflow: cálculo angular inválido (raro)
//   lf=1        — Loop Finished: offset compensation rodou OK no boot
//   updates     — # de leituras DIAAGC desde boot (deve crescer ~31/seg)
//   status      — heurística textual: OK/WEAK/STRONG/MISSING/NOT_READY
static int h_sys_magnet(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_EXEC && t != CMD_TYPE_GET) return -1;
    struct magnet_snap_t snap;
    odrive_bridge_enc_get_magnet(&snap);
    const char *status;
    if (snap.update_count == 0)               status = "NOT_READY";
    else if (snap.magl)                       status = "MAGNET_TOO_FAR";
    else if (snap.magh)                       status = "MAGNET_TOO_CLOSE";
    else if (!snap.lf)                        status = "WAITING_COMPENSATION";
    else if (snap.cof)                        status = "CORDIC_OVERFLOW";
    else if (snap.agc < 30 || snap.agc > 220) status = "MARGINAL";
    else                                      status = "OK";
    snprintf(r, s, "agc=%u magl=%u magh=%u cof=%u lf=%u updates=%u status=%s",
             snap.agc, snap.magl, snap.magh, snap.cof, snap.lf,
             snap.update_count, status);
    return 0;
}

// encraw_snap_t e odrive_bridge_enc_get_raw já declarados em odrive_bridge.h
static int h_sys_encraw(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_EXEC && t != CMD_TYPE_GET) return -1;
    struct encraw_snap_t snap;
    odrive_bridge_enc_get_raw(&snap);
    snprintf(r, s, "ok=%u pty=%u ef=%u xfr=%u last=0x%04X pos=%u",
             snap.ok_count, snap.fail_parity, snap.fail_ef,
             snap.fail_xfer, snap.last_rx, snap.pos_abs);
    return 0;
}

// ==================== MT6835 (encoder mode 261) ====================
// Acesso direto ao register map do chip (datasheet cap. 10) + comandos.
// Escritas em registro são VOLÁTEIS (register map recarrega da EEPROM no
// power-on) — pra persistir, sys.mteeprom! depois (e aguardar 6 s ligado).
//
// sys.mtread=<addr>          → lê registro (ex.: sys.mtread=0x011 → BW)
// sys.mtwrite=<addr> <val>   → escreve registro (aceita "addr val" ou "addr:val")
// sys.mtzero!                → ZERO_POS ← posição atual (recusa motor armado)
// sys.mteeprom!              → persiste register map na EEPROM (recusa armado;
//                              pausa leituras de ângulo 6.5 s — datasheet 7.6.6)
// sys.mtstatus?              → boot check + STATUS warnings + estado da auto-cal

static int h_sys_mtread(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t != CMD_TYPE_SET) return -1;   // "SET" carrega o endereço como valor
    long addr = parse_long(v, -1);
    int val = odrive_bridge_mt6835_read_reg((int)addr);
    if (val < 0) { strncpy(r, "FAIL", s); return -1; }
    snprintf(r, s, "reg[0x%03lX]=0x%02X", (unsigned long)addr, (unsigned)val);
    return 0;
}

static int h_sys_mtwrite(uint8_t, CmdType t, const char *v, char *r, size_t s) {
    if (t != CMD_TYPE_SET || !v) return -1;
    char *end = nullptr;
    long addr = strtol(v, &end, 0);
    if (end == v) return -1;
    while (*end == ' ' || *end == ':' || *end == ',') end++;
    const char *v2 = end;
    long val = strtol(v2, &end, 0);
    if (end == v2) return -1;
    if (!odrive_bridge_mt6835_write_reg((int)addr, (int)val)) {
        strncpy(r, "FAIL", s);
        return -1;
    }
    // Read-back como confirmação (a escrita é assíncrona do ponto de vista do
    // host — devolver o valor relido evita "escreveu no vazio" silencioso).
    int rb = odrive_bridge_mt6835_read_reg((int)addr);
    snprintf(r, s, "reg[0x%03lX]=0x%02X", (unsigned long)addr, (unsigned)(rb < 0 ? 0xFF : rb));
    return 0;
}

static int h_sys_mtzero(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_EXEC && t != CMD_TYPE_GET) return -1;
    int ok = odrive_bridge_mt6835_set_zero();
    strncpy(r, ok ? "OK (volatil - sys.mteeprom! para persistir)"
                  : "FAIL (motor armado? mode!=261? ack!=0x55?)", s);
    return ok ? 0 : -1;
}

static int h_sys_mteeprom(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_EXEC && t != CMD_TYPE_GET) return -1;
    int ok = odrive_bridge_mt6835_program_eeprom();
    strncpy(r, ok ? "OK - NAO desligar por 6s (leituras pausadas 6.5s)"
                  : "FAIL (motor armado? ack!=0x55?)", s);
    return ok ? 0 : -1;
}

static int h_sys_mtstatus(uint8_t, CmdType t, const char*, char *r, size_t s) {
    if (t != CMD_TYPE_EXEC && t != CMD_TYPE_GET) return -1;
    struct mt6835_snap_t snap;
    odrive_bridge_mt6835_get_status(&snap);
    if (!snap.is_mt6835) { strncpy(r, "N/A (encoder mode != 261)", s); return 0; }
    static const char *cal_names[] = {"none", "running", "failed", "ok"};
    snprintf(r, s, "boot=%d hyst0=%d overspeed=%d weakfield=%d undervolt=%d cal=%s",
             snap.boot_ok, snap.hyst_zeroed, snap.overspeed, snap.weak_field,
             snap.undervolt,
             (snap.cal_state >= 0 && snap.cal_state <= 3) ? cal_names[snap.cal_state] : "read_fail");
    return 0;
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
    { "sys",   "motortemp",    h_sys_motortemp },

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
    { "axis",  "invert",        h_axis_invert },       // inverte só a posição HID
    { "axis",  "ffbinvert",     h_axis_ffbinvert },    // inverte só o torque FFB
    { "axis",  "drvtype",       h_axis_drvtype },
    { "axis",  "enctype",       h_axis_enctype },
    { "axis",  "pos",           h_axis_pos },
    // axis.* extras (Phase 3.12) — efeitos sempre-ativos somados ao FFB
    { "axis",  "idlespring",    h_axis_idlespring },     // mola quando jogo desligado (0-255)
    { "axis",  "axisdamper",    h_axis_damper },         // damper sempre ativo (0-255)
    { "axis",  "axisinertia",   h_axis_inertia },        // inertia sempre ativa (0-255)
    { "axis",  "axisfriction",  h_axis_friction },       // friction sempre ativa (0-255)
    { "axis",  "esgain",        h_axis_esgain },         // batente eletrônico: força da mola (0-255)
    { "axis",  "esdamp",        h_axis_esdamp },         // batente eletrônico: amortecimento, independente da mola (0-255)
    { "axis",  "maxtorquerate", h_axis_maxtorquerate },  // slew limit (counts/ms, 0=off)
    { "axis",  "expo",          h_axis_expo },           // curva exponencial (-32767..32767)
    { "axis",  "exposcale",     h_axis_exposcale },      // divisor pro expo (1-255)
    { "axis",  "zeroenc",       h_axis_zeroenc },        // zera posição atual (EXEC)
    { "axis",  "zeroofs",       h_axis_zeroofs },        // offset persistente em graus (GET/SET)
    { "axis",  "zhits",         h_axis_zhits },          // contador de pulsos Z aceitos (GET/SET=reset)
    { "axis",  "zglitch",       h_axis_zglitch },        // contador de IRQs Z rejeitadas como glitch
    // GPIO axis processor (port simplificado do AnalogAxisProcessing)
    { "axis",  "gpiofilt",      h_axis_gpiofilt },       // habilita filter low-pass global
    { "axis",  "gpiofiltf",     h_axis_gpiofiltf },      // cutoff do filter em Hz (default 60)
    { "axis",  "gpioautocal",   h_axis_gpioautocal },    // habilita autocal global (atualiza AMIN/AMAX)
    { "axis",  "anticogcal",    h_axis_anticogcal },     // dispara anticogging calibration
    // EQ por banda (WEIGHT/CHASSIS/ROAD) — ver EqCascade
    { "axis",  "eqweight",      h_axis_eqweight },       // ganho em dB, [-12, +12], default 0
    { "axis",  "eqchassis",     h_axis_eqchassis },
    { "axis",  "eqroad",        h_axis_eqroad },
    { "axis",  "eqweightfreq",  h_axis_eqweightfreq },   // read-only: 5.00 Hz (low-shelf)
    { "axis",  "eqchassisfreq", h_axis_eqchassisfreq },  // read-only: 12.00 Hz (peak)
    { "axis",  "eqroadfreq",    h_axis_eqroadfreq },     // read-only: 25.00 Hz (high-shelf)
    { "axis",  "eqweightq",     h_axis_eqweightq },      // read-only: 0.70
    { "axis",  "eqchassisq",    h_axis_eqchassisq },     // read-only: 1.00
    { "axis",  "eqroadq",       h_axis_eqroadq },        // read-only: 0.70
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
    { "sys",   "eeformat",     h_sys_eeformat },      // EEPROM force format (escape hatch)
    { "sys",   "vbusdiv",      h_sys_vbusdiv },       // VBUS voltage divider (1-50)
    // GPIO inputs (1-4) — sintaxe: gpio.<inst>.<field>
    { "gpio",  "mode",         h_gpio_mode },         // 0/1/2/3 = off/button/axis/zerowheel
    { "gpio",  "idx",          h_gpio_idx },          // 0-63 botão, 0-3 eixo
    { "gpio",  "invert",       h_gpio_invert },       // 0/1
    { "gpio",  "amin",         h_gpio_amin },         // 0-4095 (só axis)
    { "gpio",  "amax",         h_gpio_amax },         // 0-4095 (só axis)
    { "gpio",  "cur",          h_gpio_cur },          // raw atual (debug/UI)
    { "gpio",  "filt",         h_gpio_filt },         // último valor filtrado (debug/UI)
    { "sys",   "reboot",       h_sys_reboot },        // reset chip
    { "sys",   "uptime",       h_sys_uptime },
    { "sys",   "ping",         h_sys_ping },
    { "sys",   "encraw",       h_sys_encraw },        // Encoder SPI debug counters
    { "sys",   "magnet",       h_sys_magnet },        // AS5047 DIAAGC (magnet status)
    { "sys",   "mtread",       h_sys_mtread },        // MT6835 lê registro (sys.mtread=<addr>)
    { "sys",   "mtwrite",      h_sys_mtwrite },       // MT6835 escreve registro (sys.mtwrite=<addr> <val>)
    { "sys",   "mtzero",       h_sys_mtzero },        // MT6835 ZERO_POS ← posição atual
    { "sys",   "mteeprom",     h_sys_mteeprom },      // MT6835 persiste register map (aguardar 6 s!)
    { "sys",   "mtstatus",     h_sys_mtstatus },      // MT6835 boot/warnings/auto-cal
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
