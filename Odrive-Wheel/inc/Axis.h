// Stub Axis.h — interface que o EffectsCalculator do OpenFFBoard espera.
// Implementação concreta é minha própria (FFBAxisBridge) que conecta ao
// axes[0] da ODrive. Phase 5 implementa o bridge.

#ifndef FFB_AXIS_STUB_H_
#define FFB_AXIS_STUB_H_

#ifdef __cplusplus

#include <cstdint>

// Dados cinemáticos do eixo, calculados pelo bridge a partir do encoder da ODrive.
struct metric_t {
    float accel = 0;               // deg/s^2
    float speed = 0;               // deg/s
    int32_t pos_scaled_16b = 0;    // posição escalada pra faixa HID (-32767..32767)
    float pos_f = 0;               // posição normalizada -1.0..1.0
    float posDegrees = 0;          // posição absoluta em graus
    int32_t torque = 0;            // total de torque aplicado
};

// Interface abstrata. EffectsCalculator itera axes[] e chama setEffectTorque,
// calculateAxisEffects e getMetrics. O "bridge" pra ODrive vira a implementação.
class Axis {
public:
    virtual ~Axis() = default;
    virtual void setEffectTorque(int32_t torque) = 0;
    virtual void calculateAxisEffects(bool ffb_on) = 0;
    virtual metric_t *getMetrics() = 0;
};

#endif
#endif
