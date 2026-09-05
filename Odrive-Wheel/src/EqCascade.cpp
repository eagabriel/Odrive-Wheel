/*
 * EqCascade.cpp — ver EqCascade.h pra docs.
 */

#include "EqCascade.h"
#include <cmath>

namespace {
    // Tipo, centro e Q de cada banda. WEIGHT e ROAD são SHELVES (afetam
    // toda a região abaixo/acima da freq de transição), CHASSIS é PEAK
    // (banda específica). Read-only via getters.
    constexpr BiquadType BAND_TYPE[EqCascade::BAND_COUNT] = {
        BiquadType::lowshelf,   // WEIGHT — tudo abaixo de ~5 Hz
        BiquadType::peak,       // CHASSIS — pico em ~12 Hz
        BiquadType::highshelf,  // ROAD   — tudo acima de ~25 Hz
    };
    constexpr float BAND_FREQ[EqCascade::BAND_COUNT] = { 5.0f,  12.0f, 25.0f };
    constexpr float BAND_Q   [EqCascade::BAND_COUNT] = { 0.7f,  1.0f,  0.7f  };

    constexpr float GAIN_MAX_DB =  12.0f;
    constexpr float GAIN_MIN_DB = -12.0f;

    inline float clamp_db(float x) {
        if (x < GAIN_MIN_DB) return GAIN_MIN_DB;
        if (x > GAIN_MAX_DB) return GAIN_MAX_DB;
        return x;
    }
}

EqCascade::EqCascade() {
    for (uint8_t i = 0; i < BAND_COUNT; ++i) {
        gain_db_[i] = 0.0f;
        recalc_((Band)i);
    }
    update_bypass_and_norm_();
}

void EqCascade::setSamplerate(float fs) {
    if (fs <= 0.0f) return;
    samplerate_ = fs;
    for (uint8_t i = 0; i < BAND_COUNT; ++i) recalc_((Band)i);
    // Coeficientes mudaram — dc_norm_ é derivado deles e precisa acompanhar.
    update_bypass_and_norm_();
}

void EqCascade::setGain(Band band, float gainDb) {
    if (band >= BAND_COUNT) return;
    gain_db_[band] = clamp_db(gainDb);
    recalc_(band);
    update_bypass_and_norm_();
}

float EqCascade::getFreq(Band band) const {
    return (band < BAND_COUNT) ? BAND_FREQ[band] : 0.0f;
}

float EqCascade::getQ(Band band) const {
    return (band < BAND_COUNT) ? BAND_Q[band] : 0.0f;
}

void EqCascade::reset() {
    for (uint8_t i = 0; i < BAND_COUNT; ++i) {
        gain_db_[i] = 0.0f;
        recalc_((Band)i);   // recalc também zera z1/z2 (ver Biquad::calcBiquad)
    }
    update_bypass_and_norm_();
}

float EqCascade::process(float in) {
    if (bypass_) return in;
    // Pré-scale por 1/H(DC): trava o ganho da cascata em 0 Hz em unity, de
    // forma que a força de curva sustentada nunca muda com o ajuste do EQ.
    const float in_scaled = in * dc_norm_;
    float out = bq_[0].process(in_scaled);
    out       = bq_[1].process(out);
    out       = bq_[2].process(out);

    // Sanity final: NaN/inf nunca deveriam acontecer (Q ≤ 1.5, gain ≤ ±12 dB
    // = biquads estáveis), mas se vier garbage do input ou se houver
    // overflow numérico, reseta todo o estado e devolve bypass. Sem isso,
    // (int32_t)NaN é undefined behavior no cast downstream.
    if (!std::isfinite(out)) {
        for (uint8_t i = 0; i < BAND_COUNT; ++i) recalc_((Band)i);
        return in;
    }
    return out;
}

void EqCascade::recalc_(Band band) {
    // Biquad::setBiquad espera Fc normalizado (freq / sample_rate).
    const float fc = BAND_FREQ[band] / samplerate_;
    bq_[band].setBiquad(BAND_TYPE[band], fc, BAND_Q[band], gain_db_[band]);
}

void EqCascade::update_bypass_and_norm_() {
    bool flat = true;
    for (uint8_t i = 0; i < BAND_COUNT; ++i) {
        if (gain_db_[i] != 0.0f) { flat = false; break; }
    }
    bypass_ = flat;

    // Ganho da cascata em 0 Hz = produto do H(DC) de cada biquad. Percorre
    // todas as bandas em vez de assumir quais tocam em DC, pra continuar
    // correto se BAND_TYPE mudar. Custo: 3 divisões, só aqui — o hot path
    // (process) segue com uma única multiplicação.
    float dc = 1.0f;
    for (uint8_t i = 0; i < BAND_COUNT; ++i) dc *= bq_[i].dcGain();

    // Guard: H(DC) ≈ 0 tornaria a normalização explosiva. Com os tipos e o
    // range de ±12 dB usados aqui, dc fica em [0.25, 4] — nunca perto disso.
    dc_norm_ = (std::fabs(dc) > 1e-6f) ? (1.0f / dc) : 1.0f;
}
