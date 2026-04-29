// Stub flash_helpers.h — wraps nossa EEPROM emulada (eeprom.c) com a API
// que OpenFFBoard espera.

#ifndef FLASH_HELPERS_H_
#define FLASH_HELPERS_H_

#include "constants.h"
#include <stdbool.h>
#include <stdint.h>

// EEPROM emulation C functions — wrapped in extern "C" pra chamar do C++
#ifdef __cplusplus
extern "C" {
#endif
#include "eeprom.h"
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline bool Flash_Write(uint16_t adr, uint16_t dat) {
    return EE_WriteVariable(adr, dat) == 0;
}
static inline bool Flash_Init(void) {
    return EE_Init() == 0;
}

#ifdef __cplusplus
}

// Sobrecarga C++ com default arg como no OpenFFBoard
inline bool Flash_Read(uint16_t adr, uint16_t *buf, bool checkempty = true) {
    (void)checkempty;
    return EE_ReadVariable(adr, buf) == 0;
}
inline bool Flash_ReadWriteDefault(uint16_t adr, uint16_t *buf, uint16_t def) {
    if (EE_ReadVariable(adr, buf) == 0) return true;
    *buf = def;
    return EE_WriteVariable(adr, def) == 0;
}

template<typename T>
inline T Flash_ReadDefault(uint16_t adr, T def) {
    uint16_t buf;
    return (EE_ReadVariable(adr, &buf) == 0) ? T(buf) : def;
}

inline uint16_t pack(uint8_t hb, uint8_t lb) { return (hb << 8) | lb; }
#endif

#endif
