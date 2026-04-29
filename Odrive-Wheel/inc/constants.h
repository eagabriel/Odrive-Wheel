// Stub de constants.h do OpenFFBoard — só o que o core FFB usa
#ifndef __CONSTANTS_H
#define __CONSTANTS_H

#include <stdint.h>

// MAX_AXIS=2 obrigatorio — o proprio OpenFFBoard avisa em constants.h:
// "ONLY USE 2 for now else screws HID Reports". Com MAX_AXIS=1, Windows
// DirectInput rejeita CreateEffect com E_INVALIDARG porque test apps e
// jogos usam cAxes=2. Nosso firmware ainda usa apenas 1 eixo fisico
// (axes[0]); Y eh fantasma so pra satisfazer o descriptor.
#define MAX_AXIS 2

// Descritor FFB 2-axis (copiado de OpenFFBoard, known-working pra Windows FFB)
#define AXIS2_FFB_HID_DESC

// HID polling rate (1ms)
#define HID_BINTERVAL 0x01

// Flash EEPROM emulada (já definido no Makefile também)
#ifndef USE_EEPROM_EMULATION
#define USE_EEPROM_EMULATION
#endif

// FFB internal scalers (valores padrão do OpenFFBoard)
#define INTERNAL_SCALER_DAMPER      0x04
#define INTERNAL_SCALER_FRICTION    0x04
#define INTERNAL_SCALER_INERTIA     0x04
#define INTERNAL_SCALER_SPRING      0x10

#endif
