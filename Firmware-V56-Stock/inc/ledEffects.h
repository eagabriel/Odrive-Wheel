// Stub ledEffects.h — sem LEDs no firmware fundido, só placeholders.
#ifndef LEDEFFECTS_H_
#define LEDEFFECTS_H_

#include <cstdint>

enum class LedEffect : uint8_t { NONE = 0, EFFECT = 1 };
inline void pulseLed(LedEffect e = LedEffect::EFFECT) { (void)e; }
inline void setClipLed(bool state) { (void)state; }
inline void setErrorLed(bool state) { (void)state; }

#endif
