#pragma once
// OscillationGuard — proteção contra oscilação com o volante solto
// (hands-off shimmy). Baseado no "oscillation guard" do fork Gargul, reescrito
// como classe isolada (sem HAL/ODrive) pra poder ser testado fora da placa.
//
// Unidades: graus, graus/s, graus/s², segundos, N·m. Referência: encoder (a
// mesma dos axis effects — damper, endstop etc.).
//
// Funcionamento
//   Detecção: cada inversão de sentido fecha meia oscilação. Ela é "perigosa"
//   se for rápida, com amplitude útil, período coerente com a anterior,
//   crescendo (ou já grande) E com o volante SOLTO. Três seguidas armam.
//
//   Volante solto x mãos no volante: sem as mãos, o aro obedece só ao motor,
//   então em cada janela curta J·Δω ≈ ∫T_motor·dt (impulso = variação de
//   momento). Com as mãos, o torque do piloto aparece como resíduo. A razão
//   RMS(resíduo)/RMS(impulso) separa os dois casos. Usa Δω, não a aceleração:
//   derivar a velocidade amplifica o ruído do PLL a ponto de parecer "mãos". Exige a inércia J (medida pelo Performance Test e gravada em
//   axis.oscj). Com J = 0 a confirmação fica desligada e a detecção é só
//   cinemática (comportamento do fork original).
//   Critérios descartados: trabalho do torque do jogo (a mola com atraso do
//   jogo faz trabalho positivo com ou sem as mãos) e correlação torque ×
//   aceleração (num movimento senoidal do piloto as duas ficam correlacionadas).
//
//   Ação, enquanto armada:
//   - Corta 80 % do FFB do jogo, uniforme. É ele que alimenta a oscilação (mola
//     com atraso) e ninguém está segurando o volante, então não se perde
//     feeling. Um corte que varia com a posição modula o FFB a cada ida e
//     volta e gera um balanço menor e mais rápido.
//   - Amortecimento viscoso linear, F = −c·ω, com c = strength/255 · J / 25 ms.
//     Calibrar pelo J mantém a constante de tempo do freio (J/c) acima dos
//     atrasos da malha (PLL, task, malha de corrente): um freio mais rápido que
//     isso oscila sozinho em frequência alta. Sem J usa 0,02 kg·m².
//   Sem parede nem dependência de posição: nenhum degrau na força.
//
//   Saída: giro contínuo do piloto ou mãos de volta (resíduo de torque alto
//   numa janela contínua de ~150 ms — funciona mesmo com o aro seguro parado).
//   Parado não é saída: com o volante solto, devolver o FFB recria a
//   oscilação e o ciclo liga-desliga. Rampa de saída suave (~330 ms).
//
// Garantias
//   - Passiva: o torque devolvido é −c·ω, sempre contra a velocidade — só
//     retira energia, nunca alimenta a oscilação.
//   - Teto do freio = 80 % do maxtorque do usuário × strength/255.
//   - strength = 0 desliga e zera o estado.

#include <cmath>
#include <cstdint>

class OscillationGuard {
public:
    struct Config {
        uint8_t strength   = 0;      // 0 = desligado; 255 = kCeilFrac × maxtorque
        float   inertiaKgm2 = 0.0f;  // J do aro+motor; 0 = sem confirmação de "solto"
    };
    struct Output {
        float torqueNm;   // a SOMAR ao torque do motor (referência do encoder)
        float gameScale;  // multiplicador do torque do jogo (0.2..1)
    };

    Config cfg;

    // Diagnóstico (leitura). Os campos last* descrevem a última meia
    // oscilação medida, armada ou não — servem pra acompanhar a detecção.
    bool     active            = false;
    uint32_t activations       = 0;
    uint32_t halfCount         = 0;      // meias oscilações medidas (inversões)
    float    lastAmplitudeDeg  = 0.0f;
    float    lastFreqHz        = 0.0f;
    float    lastPeakSpeed     = 0.0f;   // graus/s
    float    lastFreeRatio     = NAN;    // RMS(J·α−T)/RMS(T) da última meia oscilação (NAN = sem J/sem dado)
    float    currentNm         = 0.0f;
    float    peakNm            = 0.0f;
    uint8_t  confidence() const { return confidence_; }   // arma em 3

    void reset(bool clearDiag = false) {
        filtSpeed_ = 0; reversalPos_ = 0; peakSpeed_ = 0;
        prevPP_ = 0; prevPeakSpeed_ = 0; halfT_ = 0; prevHalfT_ = 0;
        noReversalT_ = 0; emaII_ = 0; emaRR_ = 0; handsT_ = 0;
        winImp_ = 0; winT_ = 0; winW0_ = 0;
        confidence_ = 0; handsOnHalfs_ = 0; dir_ = 0;
        brake_ = 0; active = false; currentNm = 0;
        corrReset_();
        if (clearDiag) {
            activations = 0; halfCount = 0; lastAmplitudeDeg = 0; lastFreqHz = 0;
            lastPeakSpeed = 0; lastFreeRatio = NAN; peakNm = 0;
        }
    }

    // posDeg / speedDegS: estado do aro (referência do encoder).
    // appliedNm: torque efetivamente comandado no motor no ciclo anterior.
    // userMaxNm: teto de torque configurado pelo usuário. dt em segundos.
    Output tick(float posDeg, float speedDegS,
                float appliedNm, float userMaxNm, float dt) {
        if (cfg.strength == 0) {
            if (active || brake_ > 0.0f) reset();
            return { 0.0f, 1.0f };
        }
        if (!(dt > 0.0f) || dt > 0.02f || !std::isfinite(posDeg)
                || !std::isfinite(speedDegS)) {
            return { 0.0f, 1.0f };
        }

        // Passa-baixas curto (~22 Hz): tira degraus de count sem esconder
        // uma inversão real. Histerese de 90 °/s define o sentido.
        const float a = dt / (dt + kSpeedTau);
        filtSpeed_ += a * (speedDegS - filtSpeed_);
        const float absSpeed = std::fabs(filtSpeed_);
        if (absSpeed > peakSpeed_) peakSpeed_ = absSpeed;
        if (halfT_ < 2.0f) halfT_ += dt;
        if (noReversalT_ < 10.0f) noReversalT_ += dt;

        if (cfg.inertiaKgm2 > 0.0f && std::isfinite(appliedNm)) {
            winImp_ += appliedNm * dt;
            winT_   += dt;
            if (winT_ >= kWin) {
                const float w1 = speedDegS * kDegToRad;
                const float jdw = cfg.inertiaKgm2 * (w1 - winW0_);
                const float res = jdw - winImp_;
                corrAdd_(winImp_, jdw);
                // Janela contínua (EMA ~150 ms) do resíduo — detecta as mãos
                // de volta sem depender de inversões (piloto segura o aro).
                const float k = winT_ / (winT_ + kHandsTau);
                emaII_ += k * (winImp_ * winImp_ - emaII_);
                emaRR_ += k * (res * res - emaRR_);
                const float minImp = kHandsMinNm * winT_;
                const bool torquePresent = emaII_ > minImp * minImp;
                if (torquePresent && emaRR_ > kFreeHands * kFreeHands * emaII_)
                    handsT_ += winT_;
                else
                    handsT_ = 0.0f;
                winW0_ = w1; winImp_ = 0.0f; winT_ = 0.0f;
            }
        } else {
            handsT_ = 0.0f; winImp_ = 0.0f; winT_ = 0.0f;
            winW0_ = speedDegS * kDegToRad;
        }

        int8_t dir = 0;
        if (filtSpeed_ >  kDirSpeed) dir =  1;
        if (filtSpeed_ < -kDirSpeed) dir = -1;

        if (dir_ == 0 && dir != 0) {
            dir_ = dir; reversalPos_ = posDeg; halfT_ = 0;
            peakSpeed_ = absSpeed; corrReset_();
        } else if (dir != 0 && dir != dir_) {
            onReversal_(posDeg);
            dir_ = dir; reversalPos_ = posDeg; halfT_ = 0;
            noReversalT_ = 0; peakSpeed_ = absSpeed; corrReset_();
        }

        if (active) {
            const bool pilotTurn = noReversalT_ >= 0.8f && absSpeed >= kDirSpeed;
            const bool handsBack = handsOnHalfs_ >= 2 || handsT_ >= kHandsHold;
            if (pilotTurn || handsBack) {
                active = false; confidence_ = 0;
            }
        }

        // Entrada em ~25 ms (mais rápida que meia onda a 10 Hz), saída ~330 ms.
        const float target = active ? 1.0f : 0.0f;
        const float rate   = (active ? 40.0f : 3.0f) * dt;
        if (brake_ < target) brake_ = std::fmin(target, brake_ + rate);
        else if (brake_ > target) brake_ = std::fmax(target, brake_ - rate);

        currentNm = 0.0f;
        if (brake_ <= 0.0f) return { 0.0f, 1.0f };

        const float configured = (float)cfg.strength / 255.0f;
        const float J = cfg.inertiaKgm2 > 0.0f ? cfg.inertiaKgm2 : kDefaultJ;
        const float c = configured * J / kDampTau;              // N·m·s/rad
        // Velocidade crua (PLL): o filtro da detecção somaria ~7 ms de atraso.
        float cmd = -c * speedDegS * kDegToRad * brake_;
        const float allowed = userMaxNm > 0.0f ? kCeilFrac * userMaxNm * configured : 0.0f;
        cmd = clip_(cmd, -allowed, allowed);

        currentNm = std::fabs(cmd);
        if (currentNm > peakNm) peakNm = currentNm;

        return { cmd, 1.0f - kGameCut * brake_ };
    }

private:
    static constexpr float kSpeedTau   = 0.0073f; // ≈ α 0.12 a 1 kHz
    static constexpr float kDirSpeed   = 90.0f;   // histerese de sentido, °/s
    static constexpr float kDegToRad   = 0.017453293f;
    // Razão resíduo/torque. Solto: ~0.1-0.3 (atraso do torque + erro de J).
    // Piloto movendo o aro: tipicamente > 0.6.
    static constexpr float kFreeArm    = 0.45f;   // abaixo = volante solto
    static constexpr float kFreeHands  = 0.70f;   // acima = mãos no aro
    static constexpr float kWin        = 0.015f;  // janela de impulso × Δω, s
    static constexpr float kHandsTau   = 0.15f;   // janela contínua do resíduo, s
    static constexpr float kHandsHold  = 0.10f;   // resíduo alto por este tempo = mãos
    static constexpr float kHandsMinNm = 0.30f;   // abaixo disso o torque não decide

    float   filtSpeed_ = 0, reversalPos_ = 0, peakSpeed_ = 0;
    float   prevPP_ = 0, prevPeakSpeed_ = 0, halfT_ = 0, prevHalfT_ = 0;
    float   noReversalT_ = 0, brake_ = 0;
    float   emaII_ = 0, emaRR_ = 0, handsT_ = 0;
    float   winImp_ = 0, winT_ = 0, winW0_ = 0;
    uint8_t confidence_ = 0, handsOnHalfs_ = 0;
    int8_t  dir_ = 0;
    // somas da meia oscilação corrente (por janela): Σ impulso², Σ (J·Δω − impulso)²
    float   cn_ = 0, cTT_ = 0, cRR_ = 0;

    static constexpr float kCeilFrac = 0.80f;   // teto do freio / maxtorque
    static constexpr float kDampTau  = 0.025f;  // J/c na força máxima, s
    static constexpr float kDefaultJ = 0.02f;   // J assumido sem axis.oscj, kg·m²
    static constexpr float kGameCut  = 0.80f;   // corte do FFB do jogo armada

    static float clip_(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

    void corrReset_() { cn_ = cTT_ = cRR_ = 0.0f; }
    void corrAdd_(float t, float jAlpha) {   // t = impulso, jAlpha = J·Δω
        const float r = jAlpha - t;
        cn_ += 1.0f; cTT_ += t * t; cRR_ += r * r;
    }
    // RMS(resíduo)/RMS(impulso); NAN sem J, com < 3 janelas ou torque ~0.
    float freeRatio_() const {
        const float minImp = kHandsMinNm * kWin;
        if (cn_ < 3.0f || !(cTT_ > minImp * minImp * cn_)) return NAN;
        return std::sqrt(cRR_ / cTT_);
    }

    void onReversal_(float posDeg) {
        const float pp = std::fabs(posDeg - reversalPos_);
        const float dur = halfT_;
        const float r = freeRatio_();
        lastFreeRatio = r;

        const bool durationOk  = dur >= 0.030f && dur <= 0.320f;
        const bool amplitudeOk = pp >= 6.0f;
        const bool speedOk     = peakSpeed_ >= 220.0f;
        bool periodOk = true;
        if (prevHalfT_ > 0.0f) {
            const float ratio = dur / prevHalfT_;
            periodOk = ratio >= 0.55f && ratio <= 1.80f;
        }
        const bool growing = prevPP_ <= 0.0f || pp >= prevPP_ + 1.5f
                          || peakSpeed_ >= prevPeakSpeed_ + 60.0f;
        const bool alreadyBad = pp >= 28.0f || peakSpeed_ >= 650.0f;
        // Sem J configurado: só cinemática (como o fork). Com J: exige "solto".
        const bool handsOff = !(cfg.inertiaKgm2 > 0.0f) || (std::isfinite(r) && r <= kFreeArm);
        const bool dangerous = durationOk && amplitudeOk && speedOk && periodOk
                            && (growing || alreadyBad) && handsOff;

        if (dangerous) { if (confidence_ < 6) confidence_++; }
        else if (confidence_ > 0) confidence_--;

        // Mãos de volta: duas meias oscilações seguidas com resíduo grande.
        // NAN (sem J ou sem torque) não conta pra nenhum lado.
        if (std::isfinite(r) && r > kFreeHands) { if (handsOnHalfs_ < 10) handsOnHalfs_++; }
        else if (std::isfinite(r)) handsOnHalfs_ = 0;

        prevPP_ = pp; prevPeakSpeed_ = peakSpeed_; prevHalfT_ = dur;

        halfCount++;
        lastAmplitudeDeg = pp * 0.5f;
        lastPeakSpeed    = peakSpeed_;
        lastFreqHz       = dur > 0.0f ? 1.0f / (2.0f * dur) : 0.0f;

        if (confidence_ >= 3) {
            if (!active) {
                activations++;
                handsOnHalfs_ = 0;
            }
            active = true;
        }
    }
};
