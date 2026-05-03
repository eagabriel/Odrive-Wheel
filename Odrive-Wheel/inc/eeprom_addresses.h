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
#define EE_LAYOUT_VERSION       0x0002

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

// Hardware (Phase 4.x): divisor de tensão pro VBUS sense.
// Default 19 (MKS XDrive Mini). ODrive v3.6 oficial = 11. Range válido 1-50.
// Armazenado como uint16 mas só usa os 8 LSBs.
#define ADR_VBUS_DIVIDER        0x0260

#define NB_OF_VAR 43
extern const uint16_t VirtAddVarTab[NB_OF_VAR];

#endif
