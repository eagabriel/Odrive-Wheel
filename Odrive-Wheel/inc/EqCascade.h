/*
 * EqCascade.h — EQ de 3 bandas (WEIGHT / CHASSIS / ROAD) sobre o sinal
 * de Constant Force, aplicado depois que o EffectsCalculator soma todos
 * os efeitos por axis e ANTES do clip final.
 *
 * Topologia: 3 biquads em série, tipo por banda escolhido pra ser mais
 * intuitivo no UX de tone-control que peaks puros:
 *
 *   WEIGHT  — low-shelf  @ 5 Hz,  Q=0.7  — transferência de peso, slip lento
 *   CHASSIS — peak       @ 12 Hz, Q=1.0  — suspensão, body roll
 *   ROAD    — high-shelf @ 25 Hz, Q=0.7  — textura de pista, kerbs altos
 *
 * Low-shelf nas baixas afeta TODA a região grave (não só um pico em 2 Hz);
 * high-shelf nas altas afeta TODA a região aguda; CHASSIS fica peak porque
 * suspensão É uma banda específica entre carroceria e textura.
 *
 * Range de ganho: ±12 dB (clamped). Resolução prática: 0.5 dB.
 *
 * NORMALIZAÇÃO POR DC
 * -------------------
 * O sinal de FFB tem a maior parte da energia em DC / muito baixa frequência:
 * a força de auto-alinhamento numa curva sustentada é essencialmente constante.
 * Por isso o pré-scale é 1 / H_cascata(0 Hz) — assim a força de curva fica
 * SEMPRE em ganho unitário, independente de como as bandas estão ajustadas.
 * Mexer no EQ vira uma decisão puramente de timbre: nunca altera o peso
 * percebido, nunca exige recalibrar maxtorque.
 *
 * Na prática só o low-shelf (WEIGHT) contribui pro H(DC) — peak e high-shelf
 * têm H(DC) = 1 exato por construção (RBJ). Consequência: boostar WEIGHT
 * atenua médias/altas em vez de levantar as baixas, ou seja, WEIGHT controla
 * quanto a força base domina sobre a textura. Pra mais peso ABSOLUTO o lugar
 * é maxtorque / gain do jogo, não o EQ.
 *
 * (A versão anterior pré-escalava por 1/10^(maxBoost/20). Como peak e
 * high-shelf não tocam em DC, isso fazia boost de CHASSIS/ROAD virar corte
 * puro da força de curva — +12 dB derrubava o torque sustentado pra 25%.)
 *
 * Proteções runtime:
 *   - Sanity check final: se output virar NaN/inf por qualquer motivo
 *     (input garbage, instabilidade numérica), reseta os 3 biquads e
 *     retorna o input cru pra evitar UB no cast int32 downstream.
 *   - Em flat (default 0 dB em todas), bypass total — process() retorna
 *     in direto, zero custo de CPU.
 *   - Boost de CHASSIS/ROAD pode levar o pico além de ±0x7FFF; o clip
 *     downstream no EffectsCalculator continua sendo o limitador, como em
 *     qualquer EQ. DC nunca satura por causa do EQ (fica em unity).
 */

#ifndef EQCASCADE_H_
#define EQCASCADE_H_

#include "Filters.h"   // Biquad (já existente, suporta type=peak)
#include <cstdint>

class EqCascade {
public:
    enum Band : uint8_t {
        BAND_WEIGHT  = 0,    // low-shelf
        BAND_CHASSIS = 1,    // peak
        BAND_ROAD    = 2,    // high-shelf
        BAND_COUNT   = 3,
    };

    EqCascade();

    // Sample rate em Hz (default 1000 = taxa do calculateEffects).
    // Chamado por EffectsCalculator::updateSamplerate quando necessário.
    void setSamplerate(float fs);

    // Ganho em dB, clamped a [-12, +12].
    void  setGain(Band band, float gainDb);
    float getGain(Band band) const { return (band < BAND_COUNT) ? gain_db_[band] : 0.0f; }

    // Config fixa exposta como read-only — pro HTML poder mostrar onde
    // cada banda está centrada sem hardcodar o valor em 2 lugares.
    float getFreq(Band band) const;
    float getQ(Band band) const;

    // Zera todos os ganhos (flat) e reseta estados internos dos biquads.
    void reset();

    // Processa 1 amostra. Em flat (todos os ganhos 0 dB) retorna `in`
    // direto sem percorrer biquads — custo zero pro caso comum.
    float process(float in);

private:
    Biquad bq_[BAND_COUNT];
    float  gain_db_[BAND_COUNT];
    float  samplerate_ = 1000.0f;
    float  dc_norm_    = 1.0f;     // pré-escala = 1 / H_cascata(0 Hz)
    bool   bypass_     = true;     // true ⇔ todos os ganhos == 0

    void recalc_(Band band);
    // Recalcula bypass_ e dc_norm_. PRECISA rodar depois de recalc_() das
    // bandas afetadas — lê os coeficientes já atualizados dos biquads.
    void update_bypass_and_norm_();
};

#endif /* EQCASCADE_H_ */
