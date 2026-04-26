// Stub cppmain.h — helpers + defines que o core FFB do OpenFFBoard usa.
#ifndef CPPMAIN_H_
#define CPPMAIN_H_

#include "constants.h"
#include "stm32f4xx_hal.h"

#ifdef __cplusplus
// sgn — sinal como inteiro (-1, 0, +1)
template<class T>
int sgn(T v) {
    return (T(0) < v) - (v < T(0));
}

// clip — trunca v na faixa [l, h]
template<class T, class C>
T clip(T v, C l, C h) {
    return { v > h ? h : v < l ? l : v };
}

#include "class/hid/hid.h"  // hid_report_type_t
#endif

uint32_t micros(void);

#endif
