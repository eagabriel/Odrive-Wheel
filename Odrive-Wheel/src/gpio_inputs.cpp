// gpio_inputs.cpp — implementa GPIOs 1-4 como botões/eixos do joystick HID.
// Ver gpio_inputs.h pro contrato. Usa ADC1 do ODrive (já configurado em
// round-robin DMA) via get_adc_relative_voltage_ch().

#include "gpio_inputs.h"
#include "eeprom_addresses.h"
#include "flash_helpers.h"
#include "stm32f4xx_hal.h"

// Não dá pra incluir low_level.h direto: ele declara funções com Stm32Gpio
// (classe C++) e arrasta dependências de drivers. Só preciso de uma função
// que usa channel (uint16_t), declaro aqui. Símbolo no .o é C-mangled.
extern "C" float get_adc_relative_voltage_ch(uint16_t channel);

extern "C" {
#include "eeprom.h"
}

#include <string.h>

// -------------------- Pinout (hardcoded pra MKS XDrive Mini / ODrive v3.6) --------------------
// idx 0..3 → GPIO 1..4 → PA0..PA3 → ADC1_IN0..3
struct gpio_pin_t {
    GPIO_TypeDef* port;
    uint16_t pin;
    uint16_t adc_channel;   // pra get_adc_relative_voltage_ch
};

static const gpio_pin_t s_pins[GPIO_INPUTS_COUNT] = {
    { GPIOA, GPIO_PIN_0, 0 },  // GPIO 1 → PA0 → ADC1_IN0
    { GPIOA, GPIO_PIN_1, 1 },  // GPIO 2 → PA1 → ADC1_IN1
    { GPIOA, GPIO_PIN_2, 2 },  // GPIO 3 → PA2 → ADC1_IN2
    { GPIOA, GPIO_PIN_3, 3 },  // GPIO 4 → PA3 → ADC1_IN3
};

// -------------------- Config em RAM --------------------
struct gpio_cfg_t {
    uint8_t  mode;     // GPIO_INPUT_DISABLED/BUTTON/AXIS
    uint8_t  idx;      // 0..63 (button) ou 0..3 (axis: 0=RX, 1=RY, 2=RZ, 3=Slider)
    uint8_t  invert;   // 0/1
    uint16_t amin;     // ADC raw 0..4095 (axis only)
    uint16_t amax;     // ADC raw 0..4095 (axis only)
};

static gpio_cfg_t s_cfg[GPIO_INPUTS_COUNT];

// -------------------- Endereços EE por GPIO (helpers) --------------------
static const uint16_t s_addr_cfg[GPIO_INPUTS_COUNT]  = { ADR_GPIO1_CFG,  ADR_GPIO2_CFG,  ADR_GPIO3_CFG,  ADR_GPIO4_CFG };
static const uint16_t s_addr_amin[GPIO_INPUTS_COUNT] = { ADR_GPIO1_AMIN, ADR_GPIO2_AMIN, ADR_GPIO3_AMIN, ADR_GPIO4_AMIN };
static const uint16_t s_addr_amax[GPIO_INPUTS_COUNT] = { ADR_GPIO1_AMAX, ADR_GPIO2_AMAX, ADR_GPIO3_AMAX, ADR_GPIO4_AMAX };

// Empacota mode/idx/invert em uint16
static inline uint16_t pack_cfg(uint8_t mode, uint8_t idx, uint8_t invert) {
    return (uint16_t)((mode & 0x03) | ((idx & 0x3F) << 2) | ((invert & 1) << 8));
}
static inline void unpack_cfg(uint16_t p, uint8_t *mode, uint8_t *idx, uint8_t *invert) {
    *mode   = (uint8_t)(p & 0x03);
    *idx    = (uint8_t)((p >> 2) & 0x3F);
    *invert = (uint8_t)((p >> 8) & 1);
}

// -------------------- Pin configuration (HAL) --------------------
// Reconfigura o pino físico baseado no s_cfg[idx0]. Chamado de init e dos
// setters de mode.
static void apply_pin_mode(int idx0) {
    if (idx0 < 0 || idx0 >= GPIO_INPUTS_COUNT) return;
    const gpio_pin_t *p = &s_pins[idx0];
    GPIO_InitTypeDef init = {};
    init.Pin = p->pin;
    init.Speed = GPIO_SPEED_FREQ_LOW;

    switch (s_cfg[idx0].mode) {
        case GPIO_INPUT_BUTTON:
            init.Mode = GPIO_MODE_INPUT;
            init.Pull = GPIO_PULLUP;   // botão pra GND quando pressionado
            HAL_GPIO_Init(p->port, &init);
            break;
        case GPIO_INPUT_AXIS:
            init.Mode = GPIO_MODE_ANALOG;
            init.Pull = GPIO_NOPULL;
            HAL_GPIO_Init(p->port, &init);
            break;
        case GPIO_INPUT_DISABLED:
        default:
            // Não reconfigura; deixa em hi-Z (seguro pra qualquer estado prévio).
            init.Mode = GPIO_MODE_INPUT;
            init.Pull = GPIO_NOPULL;
            HAL_GPIO_Init(p->port, &init);
            break;
    }
}

// -------------------- Init --------------------
extern "C" void gpio_inputs_init(void) {
    // Defaults: tudo disabled, calibração razoável (0.16V .. 3.07V)
    for (int i = 0; i < GPIO_INPUTS_COUNT; i++) {
        s_cfg[i].mode = GPIO_INPUT_DISABLED;
        s_cfg[i].idx = (uint8_t)i;       // botão 0/1/2/3 default
        s_cfg[i].invert = 0;
        s_cfg[i].amin = 200;
        s_cfg[i].amax = 3800;
    }

    // Tenta carregar da EE. Cada slot que vier 0xFFFF (não existe) ignoramos.
    uint16_t v;
    for (int i = 0; i < GPIO_INPUTS_COUNT; i++) {
        if (Flash_Read(s_addr_cfg[i], &v, false) && v != 0xFFFF) {
            uint8_t m, idx, inv;
            unpack_cfg(v, &m, &idx, &inv);
            // Sanity: mode válido
            if (m <= GPIO_INPUT_AXIS) {
                s_cfg[i].mode = m;
                s_cfg[i].idx = idx;
                s_cfg[i].invert = inv;
            }
        }
        if (Flash_Read(s_addr_amin[i], &v, false) && v != 0xFFFF) {
            s_cfg[i].amin = v;
        }
        if (Flash_Read(s_addr_amax[i], &v, false) && v != 0xFFFF) {
            s_cfg[i].amax = v;
        }
    }

    // Configura todos os pinos baseado no cfg final
    for (int i = 0; i < GPIO_INPUTS_COUNT; i++) {
        apply_pin_mode(i);
    }
}

// -------------------- Save --------------------
extern "C" int gpio_inputs_save(int *writes_out, int *errors_out) {
    int writes = 0, errors = 0;
    for (int i = 0; i < GPIO_INPUTS_COUNT; i++) {
        if (!Flash_Write(s_addr_cfg[i],
                          pack_cfg(s_cfg[i].mode, s_cfg[i].idx, s_cfg[i].invert))) errors++;
        writes++;
        if (!Flash_Write(s_addr_amin[i], s_cfg[i].amin)) errors++;
        writes++;
        if (!Flash_Write(s_addr_amax[i], s_cfg[i].amax)) errors++;
        writes++;
    }
    if (writes_out) *writes_out = writes;
    if (errors_out) *errors_out = errors;
    return errors == 0 ? 1 : 0;
}

// -------------------- Update report (chamado a 1 kHz) --------------------
// Dado raw ADC 0..4095, escala pra -32767..+32767 baseado em [amin, amax]
static inline int16_t scale_axis(uint16_t raw, uint16_t amin, uint16_t amax, uint8_t invert) {
    // Sanity: amin < amax. Se invertido ou igual, retorna 0 (deadcenter).
    if (amin >= amax) return 0;
    int32_t r = (int32_t)raw;
    if (r <= (int32_t)amin) r = amin;
    if (r >= (int32_t)amax) r = amax;
    // Mapeia [amin..amax] → [-32767..+32767]
    int32_t span = (int32_t)amax - (int32_t)amin;     // > 0
    int32_t pos = r - (int32_t)amin;                  // 0..span
    int32_t scaled = (pos * 65534) / span - 32767;    // -32767..+32767
    if (invert) scaled = -scaled;
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32767) scaled = -32767;
    return (int16_t)scaled;
}

extern "C" void gpio_inputs_update_report(uint64_t *buttons,
                                           int16_t *RX, int16_t *RY,
                                           int16_t *RZ, int16_t *Slider) {
    if (!buttons) return;

    for (int i = 0; i < GPIO_INPUTS_COUNT; i++) {
        const gpio_cfg_t &c = s_cfg[i];
        if (c.mode == GPIO_INPUT_BUTTON) {
            // Botão pra GND com pull-up: pressionado = nível 0.
            // Se invert=1, lógica inversa (active high).
            GPIO_PinState st = HAL_GPIO_ReadPin(s_pins[i].port, s_pins[i].pin);
            bool pressed = (st == GPIO_PIN_RESET);    // active low default
            if (c.invert) pressed = !pressed;
            if (pressed && c.idx < 64) {
                *buttons |= ((uint64_t)1 << c.idx);
            }
        } else if (c.mode == GPIO_INPUT_AXIS) {
            // ADC1 round-robin do ODrive: get_adc_relative_voltage_ch retorna
            // 0.0 .. 1.0 (= 0 .. 4095 / 4095). Volta pra raw.
            float rel = get_adc_relative_voltage_ch(s_pins[i].adc_channel);
            if (rel < 0) rel = 0;
            if (rel > 1) rel = 1;
            uint16_t raw = (uint16_t)(rel * 4095.0f + 0.5f);
            int16_t v = scale_axis(raw, c.amin, c.amax, c.invert);

            switch (c.idx) {
                case 0: if (RX)     *RX     = v; break;
                case 1: if (RY)     *RY     = v; break;
                case 2: if (RZ)     *RZ     = v; break;
                case 3: if (Slider) *Slider = v; break;
                default: break;
            }
        }
        // GPIO_INPUT_DISABLED: nothing
    }
}

// -------------------- Setters/getters pra ASCII --------------------
static inline int idx0_from_inst(uint8_t inst) {
    if (inst < 1 || inst > GPIO_INPUTS_COUNT) return -1;
    return inst - 1;
}

extern "C" uint8_t gpio_inputs_get_mode(uint8_t inst) {
    int i = idx0_from_inst(inst);
    return (i < 0) ? 0 : s_cfg[i].mode;
}
extern "C" int gpio_inputs_set_mode(uint8_t inst, uint8_t mode) {
    int i = idx0_from_inst(inst);
    if (i < 0 || mode > GPIO_INPUT_AXIS) return -1;
    if (s_cfg[i].mode != mode) {
        s_cfg[i].mode = mode;
        apply_pin_mode(i);
    }
    return 0;
}

extern "C" uint8_t gpio_inputs_get_idx(uint8_t inst) {
    int i = idx0_from_inst(inst);
    return (i < 0) ? 0 : s_cfg[i].idx;
}
extern "C" int gpio_inputs_set_idx(uint8_t inst, uint8_t idx) {
    int i = idx0_from_inst(inst);
    if (i < 0 || idx > 63) return -1;
    s_cfg[i].idx = idx;
    return 0;
}

extern "C" uint8_t gpio_inputs_get_invert(uint8_t inst) {
    int i = idx0_from_inst(inst);
    return (i < 0) ? 0 : s_cfg[i].invert;
}
extern "C" int gpio_inputs_set_invert(uint8_t inst, uint8_t inv) {
    int i = idx0_from_inst(inst);
    if (i < 0) return -1;
    s_cfg[i].invert = inv ? 1 : 0;
    return 0;
}

extern "C" uint16_t gpio_inputs_get_amin(uint8_t inst) {
    int i = idx0_from_inst(inst);
    return (i < 0) ? 0 : s_cfg[i].amin;
}
extern "C" int gpio_inputs_set_amin(uint8_t inst, uint16_t v) {
    int i = idx0_from_inst(inst);
    if (i < 0 || v > 4095) return -1;
    s_cfg[i].amin = v;
    return 0;
}

extern "C" uint16_t gpio_inputs_get_amax(uint8_t inst) {
    int i = idx0_from_inst(inst);
    return (i < 0) ? 0 : s_cfg[i].amax;
}
extern "C" int gpio_inputs_set_amax(uint8_t inst, uint16_t v) {
    int i = idx0_from_inst(inst);
    if (i < 0 || v > 4095) return -1;
    s_cfg[i].amax = v;
    return 0;
}

extern "C" uint16_t gpio_inputs_read_raw(uint8_t inst) {
    int i = idx0_from_inst(inst);
    if (i < 0) return 0xFFFF;
    if (s_cfg[i].mode == GPIO_INPUT_BUTTON) {
        return HAL_GPIO_ReadPin(s_pins[i].port, s_pins[i].pin) == GPIO_PIN_RESET ? 1 : 0;
    } else if (s_cfg[i].mode == GPIO_INPUT_AXIS) {
        float rel = get_adc_relative_voltage_ch(s_pins[i].adc_channel);
        if (rel < 0) rel = 0;
        if (rel > 1) rel = 1;
        return (uint16_t)(rel * 4095.0f + 0.5f);
    }
    return 0xFFFF;
}
