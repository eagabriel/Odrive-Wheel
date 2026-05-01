// gpio_inputs.h — GPIOs 1-4 da MKS XDrive Mini configurados via EE
// pra atuar como botões / eixos analógicos do joystick HID.
//
// Pinout (ODrive v3.6 / MKS XDrive Mini):
//   GPIO 1 = PA0 (ADC1_IN0)
//   GPIO 2 = PA1 (ADC1_IN1)
//   GPIO 3 = PA2 (ADC1_IN2)
//   GPIO 4 = PA3 (ADC1_IN3)
//
// Modos por pino (cfg armazenada em EE):
//   0 = disabled (pino não é tocado)
//   1 = button   (digital input, pull-up interno; gera bit em rpt.buttons)
//   2 = axis     (analog input via ADC1 round-robin do ODrive; popula
//                 rpt.RX/RY/RZ/Slider conforme idx)
//
// Leitura: chamada gpio_inputs_update_report() é feita pelo ffb_thread
// a 1 kHz dentro de tud_hid_ready() — popula buttons + axes diretamente
// na struct reportHID_t antes de tud_hid_report().

#ifndef GPIO_INPUTS_H_
#define GPIO_INPUTS_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// Modos
#define GPIO_INPUT_DISABLED  0
#define GPIO_INPUT_BUTTON    1
#define GPIO_INPUT_AXIS      2

// Quantos GPIOs gerenciamos. Indexados internamente como 0..3 (= GPIO 1..4).
#define GPIO_INPUTS_COUNT    4

// Inicializa: carrega cfg da EE (se válida), configura cada pino conforme.
// Chamar APÓS EE_Init() no boot.
void gpio_inputs_init(void);

// Persiste cfg atual na EE. Chamado de dentro de ffb_save_flash() — entre
// HAL_FLASH_Unlock e Lock. Usa Flash_Write internamente. Atualiza contadores
// de save/error já existentes em ffb_task.cpp.
// Retorna 1 em sucesso (zero erros), 0 caso contrário.
int gpio_inputs_save(int *writes_out, int *errors_out);

// Atualiza buttons + axes dum reportHID. Chamar dentro do loop FFB a 1 kHz.
void gpio_inputs_update_report(uint64_t *buttons,
                                int16_t *RX, int16_t *RY,
                                int16_t *RZ, int16_t *Slider);

// ---------- Setters/getters pra ASCII (cmd_table) ----------
// Todos recebem instance 1..4 (= GPIO 1..4). Retorno 0 = OK, -1 = inválido.
// Get retorna o valor; setters reconfiguram o pino se mode mudou.

uint8_t  gpio_inputs_get_mode(uint8_t inst);
int      gpio_inputs_set_mode(uint8_t inst, uint8_t mode);

uint8_t  gpio_inputs_get_idx(uint8_t inst);
int      gpio_inputs_set_idx(uint8_t inst, uint8_t idx);

uint8_t  gpio_inputs_get_invert(uint8_t inst);
int      gpio_inputs_set_invert(uint8_t inst, uint8_t inv);

uint16_t gpio_inputs_get_amin(uint8_t inst);
int      gpio_inputs_set_amin(uint8_t inst, uint16_t v);

uint16_t gpio_inputs_get_amax(uint8_t inst);
int      gpio_inputs_set_amax(uint8_t inst, uint16_t v);

// Leitura ao vivo. Pra mode=button retorna 0/1. Pra mode=axis retorna raw
// ADC 0-4095. Pra disabled retorna 0xFFFF.
uint16_t gpio_inputs_read_raw(uint8_t inst);

#ifdef __cplusplus
}
#endif

#endif // GPIO_INPUTS_H_
