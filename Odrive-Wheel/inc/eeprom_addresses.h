// Virtual addresses da EEPROM emulada. Endereços 0x0001..0x00FF reservados
// pra sistema. A partir de 0x0100 são endereços das classes FFB/Axis/etc.

#ifndef EEPROM_ADDRESSES_H_
#define EEPROM_ADDRESSES_H_

#include <stdint.h>

// Layout/cookie atual da EE. Bumpar quando mudarmos significado de bits ou
// adicionarmos/removermos endereços. ffb_task_init lê ADR_FLASH_VERSION no
// boot — se diferir desta constante, o EE inteiro é re-formatado pra evitar
// que dados de layout antigo sejam interpretados como válidos.
//
// Histórico:
//   0x0001 — layout inicial (S10/S11, 128K pages)
//   0x0002 — layout movido pra S1/S2 (16K pages) após colisão com ODrive NVM
//   0x0003 — adicionados ADR_GPIO_AXIS_FLAGS + ADR_GPIO_AXIS_FREQ_X10 (NB_OF_VAR 45→47)
//   0x0004 — adicionados ADR_AXIS_EQ_WEIGHT/CHASSIS/ROAD (NB_OF_VAR 47→50)
#define EE_LAYOUT_VERSION       0x0004

// System / meta
#define ADR_SYSTEM_MARKER       0x0001
#define ADR_SW_VERSION          0x0002
#define ADR_FLASH_VERSION       0x0003
#define ADR_CURRENT_CONFIG      0x0004

// FFB effects filters / gains (EffectsCalculator)
#define ADR_FFB_CF_FILTER       0x0100
#define ADR_FFB_FR_FILTER       0x0101
#define ADR_FFB_DA_FILTER       0x0102
#define ADR_FFB_IN_FILTER       0x0103
#define ADR_FFB_EFFECTS1        0x0104
#define ADR_FFB_EFFECTS2        0x0105
#define ADR_FFB_EFFECTS3        0x0106

// Axis / wheel settings (Phase 7 popula)
#define ADR_AXIS1_CONFIG        0x0200
#define ADR_AXIS1_MAX_SPEED     0x0201
#define ADR_AXIS1_MAX_ACCEL     0x0202
#define ADR_AXIS1_ENDSTOP       0x0203
#define ADR_AXIS1_POWER         0x0204
#define ADR_AXIS1_DEGREES       0x0205
#define ADR_AXIS1_EFFECTS1      0x0206
#define ADR_AXIS1_EFFECTS2      0x0207
#define ADR_AXIS1_ENC_RATIO     0x0208
#define ADR_AXIS1_SPEEDACCEL_FILTER 0x0209
#define ADR_AXIS1_POSTPROCESS1  0x020A
// zeroOffset_ persistente — em graus, armazenado como float32 split em 2 slots
// uint16. O Zero Position button captura a posição atual e Salvar persiste,
// corrigindo o pequeno deslocamento residual após encoder_offset_calibration.
#define ADR_AXIS1_ZEROOFS_LO    0x020B   // 16 LSB do float32
#define ADR_AXIS1_ZEROOFS_HI    0x020C   // 16 MSB do float32

// ODrive (config retrieval pela ODriveCAN do OpenFFBoard, ainda referenciada)
#define ADR_ODRIVE_CANID        0x0300
#define ADR_ODRIVE_SETTING1_M0  0x0301
#define ADR_ODRIVE_SETTING1_M1  0x0302
#define ADR_ODRIVE_OFS_M0       0x0303
#define ADR_ODRIVE_OFS_M1       0x0304

// GPIO inputs (Phase 4.x): 3 entradas por pino × 4 pinos (1-4) = 12.
// CFG packed em uint16: bits[0:1]=mode, bits[2:7]=idx, bit[8]=invert.
// AMIN/AMAX só usados em mode=axis.
#define ADR_GPIO1_CFG           0x0250
#define ADR_GPIO1_AMIN          0x0251
#define ADR_GPIO1_AMAX          0x0252
#define ADR_GPIO2_CFG           0x0253
#define ADR_GPIO2_AMIN          0x0254
#define ADR_GPIO2_AMAX          0x0255
#define ADR_GPIO3_CFG           0x0256
#define ADR_GPIO3_AMIN          0x0257
#define ADR_GPIO3_AMAX          0x0258
#define ADR_GPIO4_CFG           0x0259
#define ADR_GPIO4_AMIN          0x025A
#define ADR_GPIO4_AMAX          0x025B
// GPIO 6 (PB2) — digital only, sem ADC. AMIN/AMAX não usados na prática
// mas reservados pra consistência de layout (e caso firmware mude).
#define ADR_GPIO6_CFG           0x025C
#define ADR_GPIO6_AMIN          0x025D
#define ADR_GPIO6_AMAX          0x025E

// Hardware (Phase 4.x): divisor de tensão pro VBUS sense.
// Default 19 (MKS XDrive Mini). ODrive v3.6 oficial = 11. Range válido 1-50.
// Armazenado como uint16 mas só usa os 8 LSBs.
#define ADR_VBUS_DIVIDER        0x0260

// GPIO axis processor (port AnalogAxisProcessing): flags bitmap + freq.
// Bits: bit0=filter_enabled, bit1=autorange_enabled. Outros bits reservados.
// freq_x10: cutoff em décimos de Hz (ex: 600 = 60.0 Hz). Range 5..5000 (0.5-500Hz).
#define ADR_GPIO_AXIS_FLAGS     0x0270
#define ADR_GPIO_AXIS_FREQ_X10  0x0271

// EQ por banda (WEIGHT/CHASSIS/ROAD). Ganhos em DÉCIMOS de dB pra preservar
// 0.1 dB de resolução num int16 (range -120..+120 = ±12 dB). 0 = flat.
// Tipo, centro e Q de cada banda hardcoded no firmware (EqCascade.cpp),
// não persistido. Sentinel 0xFFFF = nunca escrito (default flat).
#define ADR_AXIS_EQ_WEIGHT      0x0272
#define ADR_AXIS_EQ_CHASSIS     0x0273
#define ADR_AXIS_EQ_ROAD        0x0274

// 50 entradas: 4 system + 7 ffb + 12 axis (10 + 2 ZEROOFS) + 5 odrive
//             + 15 gpio (5 pinos × 3 campos) + 1 vbus_divider + 2 axis_proc
//             + 3 axis_eq
#define NB_OF_VAR 50
extern const uint16_t VirtAddVarTab[NB_OF_VAR];

#endif
