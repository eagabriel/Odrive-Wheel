// Phase 5 — Bridge FFB <-> ODrive:
//   game -> HID OUT -> HidFFB -> EffectsCalculator -> ODriveLocalAxis -> axes[0].input_torque_
//   axes[0].encoder_.pos -> ODriveLocalAxis.metrics -> HID IN report -> game
//
// Task rodando a 1kHz faz o loop inteiro.
// odrive_main.h NAO eh incluido aqui (colide com o nome Axis do OpenFFBoard);
// acesso aos internals da ODrive vai via odrive_bridge.h.

#include <memory>
#include <vector>
#include <cmath>
#include <cstring>

#include "HidFFB.h"
#include "EffectsCalculator.h"
#include "UsbHidHandler.h"
#include "Axis.h"          // stub interface com metric_t
#include "ffb_defs.h"
#include "cmsis_os.h"
#include "tusb.h"

#include "odrive_bridge.h"

extern "C" {
#include "gpio_inputs.h"
}

// -------------------- Forward decls dos globals --------------------
// Necessário pra ODriveLocalAxis (definida abaixo) referenciar s_hidffb
// e os contadores de diagnóstico antes das suas definições.
class ODriveLocalAxis;
static std::shared_ptr<class HidFFB> s_hidffb;
static ODriveLocalAxis *s_axis_raw;

static volatile uint32_t s_diag_hidout_total = 0;
static volatile uint32_t s_diag_hidout_ctrl = 0;
static volatile uint32_t s_diag_hidout_neweff = 0;
static volatile uint32_t s_diag_hidout_seteff = 0;
static volatile uint32_t s_diag_hidout_cond = 0;
static volatile uint32_t s_diag_hidout_const = 0;
static volatile uint32_t s_diag_hidout_period = 0;
static volatile uint32_t s_diag_hidout_efop = 0;
static volatile uint32_t s_diag_hidout_gain = 0;
static volatile uint32_t s_diag_hidout_other = 0;
static volatile uint32_t s_diag_hidget = 0;
static volatile uint32_t s_diag_set_eff_torque = 0;
static volatile int32_t  s_diag_last_torque_int32 = 0;

// -------------------- ODriveLocalAxis --------------------
// Implementa Axis do OpenFFBoard mapeando pra axes[0] via odrive_bridge_*.
// Defaults inspirados no OpenFFBoard (Axis.h):
//   - degreesOfRotation = 900 (range do volante)
//   - fx_ratio = 204/255 = 80% (reduz FFB pra deixar margem pro endstop - futuro)
//   - maxTorque_Nm: comecamos conservador (5 Nm) apesar do motor permitir
//     17.4 Nm teoricos (torque_constant 0.87 Nm/A * current_lim 20A).
//     Usuario pode subir via axis.maxtorque=N depois de validar termicamente.
class ODriveLocalAxis : public Axis {
public:
    // ----------- Params ajustáveis via CDC (axis.*) -------------------------
    // Volante (escalonamento básico)
    float rangeDegrees_  = 900.0f;    // steering lock-to-lock
    float maxTorque_Nm_  = 5.0f;      // scaling do torque FFB (int32 -> Nm)
    float fxRatio_       = 1.0f;      // 0..1 — escala final (1.0 = sem perda)

    // ----------- Axis effects (sempre ativos, somam ao FFB do jogo) ---------
    uint8_t idleSpring_     = 0;      // spring centralizadora quando FFB inativo
    uint8_t axisDamper_     = 0;      // damper sempre ativo (resistência velocidade)
    uint8_t axisInertia_    = 0;      // inertia sempre ativa (resistência aceleração)
    uint8_t axisFriction_   = 0;      // friction sempre ativa (atrito constante)
    uint8_t endstopStrength_= 15;     // batente eletrônico: força da mola na região >±range/2 (0-255). 15 = leve por default.
    uint8_t endstopDamper_  = 15;     // amortecimento do endstop, INDEPENDENTE de esgain (0-255). 15 ~ leve.

    // Slew rate — limita derivada do torque. 0 = desativado (sem limit).
    // Em counts/ms. Ex: 5000 = max 5000 counts de mudança por ms.
    uint16_t maxTorqueRate_ = 0;

    // Curva exponencial — força = sign(x) * |x|^(expo/exposcale + 1).
    // 0 = linear (default). Positivo = curva concentrada nas pontas (ex: ABS center).
    int16_t expo_      = 0;
    uint8_t exposcale_ = 100;          // divisor pro expo (default 100 = 1.0)

    // Phase 4.x — Inverte direção lógica do volante sem precisar trocar fios
    // do motor nem mexer em motor.config.direction da ODrive. Quando true:
    //   - getScaledAxisPos() retorna -pos: HID reporta direção contrária à
    //     do encoder, então jogo "vê" o volante girando no sentido natural
    //     pro usuário.
    //   - setEffectTorque(t) negativa o torque vindo do jogo (em HID space)
    //     antes de somar axis effects: motor empurra no sentido oposto ao
    //     do encoder, o que casa com o sinal invertido visto pelo jogo.
    // Axis effects (idleSpring/damper/friction/etc.) usam metrics nativas
    // do encoder e não são invertidos — eles se mantêm consistentes
    // automaticamente porque computam torque na mesma referência do motor.
    // Duas flags de inversão INDEPENDENTES (antes era uma só que fazia
    // pos+torque juntos, o que amarra o comportamento e não serve pra
    // casos como FFFSake Forwarder — inverte só o FFB mantendo pos ok):
    //   axisInverted_ → getScaledAxisPos() retorna -pos (axis.invert)
    //   ffbInverted_  → setEffectTorque() nega o torque   (axis.ffbinvert)
    bool axisInverted_ = false;
    bool ffbInverted_  = false;

    void setEffectTorque(int32_t torque) override {
        // Inverte torque vindo do jogo se ffbInverted_ ativo (independente
        // de axisInverted_ — permite casos onde pos está correta mas FFB
        // veio invertido pela stack do jogo).
        if (ffbInverted_) torque = -torque;

        // EffectsCalculator entrega torque clipado em [-0x7FFF, +0x7FFF].
        // Soma os axis effects calculados em calculateAxisEffects (sempre ativos).
        int32_t totalTorque = torque + axisEffectTorque_;

        // Slew limit (max change por ms) — 0 desativa
        if (maxTorqueRate_ > 0) {
            int32_t maxRate = (int32_t)maxTorqueRate_;
            int32_t diff = totalTorque - lastTorque_;
            if (diff >  maxRate) totalTorque = lastTorque_ + maxRate;
            if (diff < -maxRate) totalTorque = lastTorque_ - maxRate;
        }
        lastTorque_ = totalTorque;

        // Clip final
        if (totalTorque >  0x7FFF) totalTorque = 0x7FFF;
        if (totalTorque < -0x7FFF) totalTorque = -0x7FFF;

        float t = (float)totalTorque / (float)0x7FFF * maxTorque_Nm_ * fxRatio_;
        pending_torque_ = t;
        metrics_.torque = totalTorque;
        s_diag_set_eff_torque++;
        s_diag_last_torque_int32 = totalTorque;
    }

    void calculateAxisEffects(bool ffb_on) override {
        // Replica Axis::calculateAxisEffects do OpenFFBoard. Soma efeitos
        // sempre-ativos do volante (independentes do FFB do jogo).
        axisEffectTorque_ = 0;

        // 1. Idle spring — só quando FFB do jogo está OFF
        if (!ffb_on && idleSpring_ != 0) {
            float scale = 0.5f + ((float)idleSpring_ * 0.01f);
            int32_t clip = (int32_t)idleSpring_ * 35;
            if (clip > 10000) clip = 10000;
            int32_t f = (int32_t)(-(float)metrics_.pos_scaled_16b * scale);
            if (f >  clip) f =  clip;
            if (f < -clip) f = -clip;
            axisEffectTorque_ += f;
        }

        // 2. Damper — sempre ativo (proporcional à velocidade)
        if (axisDamper_ != 0) {
            float speedF = metrics_.speed * (float)axisDamper_ * 0.0625f;  // AXIS_DAMPER_RATIO
            if (speedF >  10000.0f) speedF =  10000.0f;
            if (speedF < -10000.0f) speedF = -10000.0f;
            axisEffectTorque_ -= (int32_t)speedF;
        }

        // 3. Inertia — sempre ativa (proporcional à aceleração)
        if (axisInertia_ != 0) {
            float accelF = metrics_.accel * (float)axisInertia_ * 0.0078125f;  // AXIS_INERTIA_RATIO
            if (accelF >  10000.0f) accelF =  10000.0f;
            if (accelF < -10000.0f) accelF = -10000.0f;
            axisEffectTorque_ -= (int32_t)accelF;
        }

        // 4. Friction — sempre ativa (atrito constante com sign de velocidade)
        if (axisFriction_ != 0) {
            float speed = metrics_.speed;
            float intensity = (float)axisFriction_ * 39.0f;  // INTERNAL_SCALER_FRICTION
            // Ramp linear nos primeiros pcs/seg pra evitar bouncing em zero
            const float rampThreshold = 50.0f;
            float fricForce = speed >= 0 ? intensity : -intensity;
            if (speed > -rampThreshold && speed < rampThreshold) {
                fricForce = (speed / rampThreshold) * intensity;
            }
            if (fricForce >  10000.0f) fricForce =  10000.0f;
            if (fricForce < -10000.0f) fricForce = -10000.0f;
            axisEffectTorque_ -= (int32_t)fricForce;
        }

        // 5. Endstop eletrônico — mola (esgain) + damper (esdamp) na região de overshoot.
        // Os dois parâmetros são INDEPENDENTES — usuário pode configurar mola firme
        // com damper suave (batente "duro" que ricochetea), ou mola suave com damper
        // forte (batente "macio" que prende suavemente), ou qualquer combinação.
        //
        // Fórmulas:
        //   F_spring = -overshoot_deg × esgain  × SPRING_GAIN    (SPRING_GAIN=25)
        //   F_damper = -speed         × esdamp  × DAMP_RATIO     (DAMP_RATIO=1.0)
        //
        // SPRING_GAIN=25 (compat OpenFFBoard).
        // DAMP_RATIO=1.0 — direto: esdamp=N → multiplier=N. esdamp=100 satura o
        // canal a partir de ~330°/s (agressivo). esdamp=15 (default) é leve:
        // ~28% saturação a 600°/s — segura bouncing sem travar o volante.
        if (endstopStrength_ != 0) {
            const float halfRange       = rangeDegrees_ / 2.0f;
            const float SPRING_GAIN     = 25.0f;
            const float DAMP_RATIO      = 1.0f;
            const float pos             = metrics_.posDegrees;
            const float speed           = metrics_.speed;

            float spring = 0.0f;
            bool inOvershoot = false;

            if (pos > halfRange) {
                spring = -(pos - halfRange) * (float)endstopStrength_ * SPRING_GAIN;
                inOvershoot = true;
            } else if (pos < -halfRange) {
                spring = (-pos - halfRange) * (float)endstopStrength_ * SPRING_GAIN;
                inOvershoot = true;
            }

            if (inOvershoot) {
                float damper = -speed * (float)endstopDamper_ * DAMP_RATIO;
                float force = spring + damper;
                if (force >  32767.0f) force =  32767.0f;
                if (force < -32767.0f) force = -32767.0f;
                axisEffectTorque_ += (int32_t)force;
            }
        }
    }

    metric_t *getMetrics() override { return &metrics_; }

    void update() {
        float turns = odrive_bridge_get_pos_turns();
        // zeroOffset_ é a posição (em graus) capturada na última chamada de
        // axis.zeroenc!. Subtraindo aqui, todo o pipeline downstream (pos_f,
        // pos_scaled_16b, metrics_.posDegrees, endstops, speed/accel)
        // automaticamente trabalha em torno do "novo zero".
        float degrees = turns * 360.0f - zeroOffset_;
        float halfRange = rangeDegrees_ / 2.0f;

        float pos_f = degrees / halfRange;
        if (pos_f > 1.0f) pos_f = 1.0f;
        if (pos_f < -1.0f) pos_f = -1.0f;

        // Aplica curva exponencial (se habilitada) — afeta como pos_scaled_16b
        // é gerado, dando perfil não-linear (mais sensível no centro ou pontas)
        if (expo_ != 0 && exposcale_ > 0) {
            float sign = pos_f >= 0 ? 1.0f : -1.0f;
            float absp = pos_f >= 0 ? pos_f : -pos_f;
            float curve = (float)expo_ / (float)exposcale_ + 1.0f;
            // powf é caro, mas só roda em axis que tem expo configurado
            absp = (curve > 0.01f && curve < 10.0f) ?
                   __builtin_powf(absp, curve) : absp;
            pos_f = sign * absp;
        }

        const float dt = 0.001f;
        // Velocidade vem da PLL do encoder (vel_estimate, turns/s → deg/s) em
        // vez de derivada bruta da posição. A derivada dupla do count bruto
        // amplifica o ruído do LSB do encoder em ~10⁶ no accel, obrigando
        // current_control_bandwidth ficar baixo (~100) só pra mascarar o
        // chiado no motor. Com a PLL como fonte, encoder.config.bandwidth vira
        // o knob real de ruído/latência dos efeitos de velocidade (damper/
        // friction/inertia — axis e jogo).
        float new_speed = odrive_bridge_get_vel_estimate() * 360.0f;
        // Accel: UMA derivada da velocidade já filtrada pela PLL, mais um
        // low-pass de 1ª ordem (~100 Hz @ 1 kHz) pra suavizar os degraus
        // residuais da PLL antes de multiplicar pelos gains de inertia.
        const float ACCEL_LPF_ALPHA = 0.386f;  // dt/(dt + 1/(2π·100Hz))
        float raw_accel = (new_speed - metrics_.speed) / dt;
        accelLpf_ += ACCEL_LPF_ALPHA * (raw_accel - accelLpf_);

        metrics_.posDegrees = degrees;
        metrics_.pos_f = pos_f;
        metrics_.pos_scaled_16b = (int32_t)(pos_f * 32767.0f);
        metrics_.speed = new_speed;
        metrics_.accel = accelLpf_;

        // Aplica pending_torque_ no motor SEMPRE (não condicionado ao FFB do
        // jogo estar ativo). Razão: EffectsCalculator::calculateEffects() chama
        // setEffectTorque(0) a cada tick antes de processar efeitos do jogo,
        // o que faz pending_torque_ conter `0 + axisEffectTorque_` (em Nm,
        // com slew + clip aplicados) mesmo quando o jogo está OFF. Esse valor
        // representa o torque dos axis effects sempre-ativos (endstop, damper,
        // inertia, friction, idle_spring) — todos devem atuar mesmo sem jogo
        // conectado, comportamento idêntico ao OpenFFBoard original.
        // Quando há jogo ativo, setEffectTorque(force) é chamado depois e
        // pending_torque_ vira `force + axisEffectTorque_`.
        // Segurança: input_torque_ só é consumido pelo motor em CLOSED_LOOP_CONTROL
        // (state=8); em IDLE é no-op. Sem risco de pulso ao chamar incondicionalmente.
        odrive_bridge_set_input_torque(pending_torque_);
    }

    void zeroEncoder() {
        // Captura posição atual em graus como o "novo zero" lógico. O offset
        // é aplicado em update() — todo o pipeline (HID position, FFB endstops,
        // diag pos=...) passa a reportar 0 imediatamente após chamar isto.
        // Não toca no encoder do ODrive (shadow_count continua incrementando
        // monotonicamente), só no offset virtual da camada FFB.
        zeroOffset_ = odrive_bridge_get_pos_turns() * 360.0f;
    }

    int32_t getScaledAxisPos() const {
        return axisInverted_ ? -metrics_.pos_scaled_16b : metrics_.pos_scaled_16b;
    }

    // zeroOffset_ é public pra ffb_load/save_flash conseguir persistir.
    // Mesma convenção dos outros axis params com persistência (axisInertia_,
    // axisFriction_, etc). Outros campos só de cálculo (metrics_, pending_torque_)
    // ficam private abaixo.
    float zeroOffset_         = 0.0f;    // offset (graus) aplicado em update()

private:
    metric_t metrics_{};
    float pending_torque_     = 0.0f;
    int32_t axisEffectTorque_ = 0;       // calculado em calculateAxisEffects
    int32_t lastTorque_       = 0;       // pra slew rate limit
    float accelLpf_           = 0.0f;    // LPF 1ª ordem ~100 Hz sobre d(speed)/dt

public:
    void reset_ffb_state() {
        pending_torque_ = 0.0f;
    }
};

// s_hidffb e s_axis_raw já declarados acima (forward decls). Aqui só faltam:
static std::shared_ptr<EffectsCalculator> s_effects_calc;
static std::vector<std::unique_ptr<Axis>> s_axes_vec;

// -------------------- Bus current peak tracker --------------------
// Amostra odrv.ibus a 1kHz e armazena pico positivo (consumo da fonte) e
// negativo (regen voltando pra fonte). Reseta via ffb_diag_reset_ibus_peaks().
// As leituras vão via odrive_bridge porque odrive_main.h colide com Axis.h.
static volatile float s_ibus_max =  0.0f;   // pico de consumo (A)
static volatile float s_ibus_min =  0.0f;   // pico de regen (A negativo)
static volatile float s_motor_ibus_max = 0.0f;
static volatile float s_motor_ibus_min = 0.0f;
static volatile float s_vbus_max = 0.0f;
static volatile float s_vbus_min = 100.0f;  // valor inicial alto pra capturar mínimo

// Phase 4.x — divisor de tensão VBUS configurável.
// Variável GLOBAL (sem static) referenciada em low_level.cpp::vbus_sense_adc_cb.
// Inicializada com default 19 (MKS XDrive Mini) escalado pra ADC. Se EE tiver
// valor salvo, ffb_task_init recalcula via ffb_set_vbus_divider().
//
// Math: voltage_scale = adc_ref_voltage * divider / adc_full_scale
//   adc_ref_voltage = 3.3V, adc_full_scale = 4096 → scale = 3.3 * D / 4096
//   Pra D=19: scale ≈ 0.01531 V/count → ADC=1568 → vbus = 24.0V ✓
volatile float g_vbus_voltage_scale = 3.3f * 19.0f / 4096.0f;
static uint8_t  s_vbus_divider = 19;
// Forward declaration — definição completa lá embaixo junto com outros wrappers.
extern "C" void ffb_set_vbus_divider(int v);

// -------------------- FFB task --------------------
static void ffb_thread(void *arg) {
    (void)arg;
    const uint32_t period_ms = 1; // 1 kHz
    uint32_t tick = osKernelSysTick();
    while (true) {
        s_axis_raw->update();
        s_effects_calc->calculateEffects(s_axes_vec);

        // Sample bus currents/voltage at 1kHz, track extremes.
        {
            float ibus = odrive_bridge_get_ibus();
            if (ibus > s_ibus_max) s_ibus_max = ibus;
            if (ibus < s_ibus_min) s_ibus_min = ibus;
            if (odrive_bridge_motor_is_armed()) {
                float mi = odrive_bridge_get_motor_ibus();
                if (mi > s_motor_ibus_max) s_motor_ibus_max = mi;
                if (mi < s_motor_ibus_min) s_motor_ibus_min = mi;
            }
            float vbus = odrive_bridge_get_vbus();
            if (vbus > s_vbus_max) s_vbus_max = vbus;
            if (vbus < s_vbus_min) s_vbus_min = vbus;
        }

        if (tud_hid_ready()) {
            reportHID_t<int16_t> rpt;
            rpt.id = 1;
            rpt.buttons = 0;
            rpt.X = (int16_t)s_axis_raw->getScaledAxisPos();
            // Phase 4.x — popula buttons + axes extras (RX/RY/RZ/Slider) a partir
            // dos GPIOs 1-4 configurados em modo button/axis.
            gpio_inputs_update_report(&rpt.buttons, &rpt.RX, &rpt.RY, &rpt.RZ, &rpt.Slider);
            // Telemetria 1 kHz nos axes não usados (Y, Z, Dial, +VBus/IBus/IBrake):
            //   Y      = vel_estimate × 1000 (turns/s × 1000, range ±32.767 t/s)
            //   Z      = Iq_measured × 1000  (A × 1000,       range ±32.767 A)
            //   Dial   = torque_output × 1000 (Nm × 1000,     range ±32.767 N·m)
            //   VBus   = vbus_voltage × 100  (V × 100,        range ±327.67 V)
            //   IBus   = ibus × 100          (A × 100,        range ±327.67 A)
            //   IBrake = brake_resistor_current × 100 (A × 100, range ±327.67 A)
            // Clamp pra não wrap em motores/PSUs extremos.
            auto sat = [](float v) -> int16_t {
                if (v >  32.767f) return  32767;
                if (v < -32.767f) return -32767;
                return (int16_t)(v * 1000.0f);
            };
            auto sat100 = [](float v) -> int16_t {
                if (v >  327.67f) return  32767;
                if (v < -327.67f) return -32767;
                return (int16_t)(v * 100.0f);
            };
            rpt.Y      = sat(odrive_bridge_get_vel_estimate());
            rpt.Z      = sat(odrive_bridge_get_iq_measured());
            rpt.Dial   = sat(odrive_bridge_get_torque_output());
            rpt.VBus   = sat100(odrive_bridge_get_vbus());
            rpt.IBus   = sat100(odrive_bridge_get_ibus());
            rpt.IBrake = sat100(odrive_bridge_get_brake_resistor_current());
            // report_id=1: tud_hid_report strippa o id; payload = buttons..Slider
            tud_hid_report(1, ((uint8_t*)&rpt) + 1, sizeof(rpt) - 1);
        }

        osDelayUntil(&tick, period_ms);
    }
}

// -------------------- Init --------------------
// Forward decl pra carregar params do axis depois que ODriveLocalAxis existe.
static void ffb_load_axis_params_internal(void);

extern "C" {
#include "eeprom.h"
#include "eeprom_addresses.h"
}

// Captura o return code do EE_Init de boot pra debug via sys.eedump
static volatile uint16_t s_boot_ee_init_rc = 0xFFFF;

// Flag estático pra ffb_init_storage_early() ser idempotente — pode ser
// chamado por main.cpp antes do ADC começar E também por ffb_task_init()
// como fallback se o early call não rodou.
static bool s_storage_initialized = false;

// Inicialização CRÍTICA da EEPROM emulada + carga do vbus_divider.
// Esta função PRECISA ser chamada ANTES de start_general_purpose_adc() em
// main.cpp — caso contrário, a IRQ vbus_sense_adc_cb roda com o
// g_vbus_voltage_scale default (divider 19) escalando ADC errado.
//
// Bug clássico: placa real com divider 11 (ODrive v3.6 oficial), real vbus=24V,
// ADC=2718. Antes do early init: vbus_voltage = 2718 * (3.3*19/4096) = 41.5V.
// Se dc_bus_overvoltage_trip_level = 28V, do_fast_checks() dispara
// ERROR_DC_BUS_OVER_VOLTAGE imediatamente, antes mesmo do ffb_task_init
// rodar pra corrigir g_vbus_voltage_scale. Erro fica latched.
//
// Solução: rodar EE_Init + carregar divider antes do ADC iniciar. main.cpp
// chama isto explicitamente antes de start_general_purpose_adc().
extern "C" void ffb_init_storage_early(void) {
    if (s_storage_initialized) return;

    HAL_FLASH_Unlock();
    // STM32F4 latch nas flags de erro de flash (PGAERR/PGSERR/WRPERR/PGPERR).
    // Se ODrive (ou qualquer outro código antes de nós) deixou alguma seteada,
    // FLASH_WaitForLastOperation retorna HAL_ERROR em CASCATA pra todas as
    // próximas ops, mesmo as válidas. ClearError resolve.
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP    | FLASH_FLAG_OPERR  |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGSERR | FLASH_FLAG_PGPERR);
    s_boot_ee_init_rc = EE_Init();

    // Cookie de versão de layout. Se EE foi formatada por uma versão antiga
    // do firmware com layout diferente, os dados velhos NÃO devem ser
    // interpretados como válidos. Lê ADR_FLASH_VERSION; se não bater com
    // EE_LAYOUT_VERSION, formata. Também trata EE_Init failed.
    {
        uint16_t stored_version = 0;
        uint16_t rc = EE_ReadVariable(ADR_FLASH_VERSION, &stored_version);
        bool needs_format = (s_boot_ee_init_rc != 0) ||
                            (rc == NO_VALID_PAGE) ||
                            (rc == 0 && stored_version != EE_LAYOUT_VERSION);
        if (needs_format) {
            __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP    | FLASH_FLAG_OPERR  |
                                   FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                                   FLASH_FLAG_PGSERR | FLASH_FLAG_PGPERR);
            EE_Format();
            EE_WriteVariable(ADR_FLASH_VERSION, EE_LAYOUT_VERSION);
        }
    }
    HAL_FLASH_Lock();

    // VBUS divider configurável. Carrega da EE (se válido) e recalcula
    // g_vbus_voltage_scale ANTES do ADC começar a rodar. Default 19 (MKS
    // XDrive Mini); ODrive v3.6 oficial = 11.
    {
        uint16_t v;
        if (Flash_Read(ADR_VBUS_DIVIDER, &v, false) && v != 0xFFFF && v >= 1 && v <= 50) {
            ffb_set_vbus_divider((int)v);
        }
    }

    // GPIO axis processor — restaura flags + freq do EE.
    // axis_proc_init() inicializa Biquads com defaults; aqui sobrescrevemos
    // com config salva (se houver).
    {
        extern void  axis_proc_init(void);
        extern void  axis_proc_set_filter_enabled(int en);
        extern void  axis_proc_set_autorange_enabled(int en);
        extern void  axis_proc_set_filter_freq(float hz);
        axis_proc_init();
        uint16_t flags = 0xFFFF;
        if (Flash_Read(ADR_GPIO_AXIS_FLAGS, &flags, false) && flags != 0xFFFF) {
            axis_proc_set_filter_enabled((flags & 0x01) ? 1 : 0);
            axis_proc_set_autorange_enabled((flags & 0x02) ? 1 : 0);
        }
        uint16_t freq_x10 = 0xFFFF;
        if (Flash_Read(ADR_GPIO_AXIS_FREQ_X10, &freq_x10, false)
                && freq_x10 != 0xFFFF && freq_x10 >= 5 && freq_x10 <= 5000) {
            axis_proc_set_filter_freq((float)freq_x10 / 10.0f);
        }
    }

    // EQ por banda — load dos ganhos persistidos. Valores armazenados em
    // décimos de dB (int16 reinterpretado pela leitura uint16). 0xFFFF =
    // nunca escrito → fica em flat (default do EqCascade). Range válido:
    // -120..+120 (= ±12.0 dB).
    if (s_effects_calc) {
        const uint16_t eqAddrs[3] = { ADR_AXIS_EQ_WEIGHT, ADR_AXIS_EQ_CHASSIS, ADR_AXIS_EQ_ROAD };
        for (uint8_t band = 0; band < 3; ++band) {
            uint16_t raw = 0xFFFF;
            if (!Flash_Read(eqAddrs[band], &raw, false) || raw == 0xFFFF) continue;
            int16_t signed_x10 = (int16_t)raw;
            if (signed_x10 < -120 || signed_x10 > 120) continue;
            const float gainDb = (float)signed_x10 / 10.0f;
            for (uint8_t axis = 0; axis < MAX_AXIS; ++axis) {
                s_effects_calc->getEqCascade(axis).setGain((EqCascade::Band)band, gainDb);
            }
        }
    }

    s_storage_initialized = true;
}

extern "C" void ffb_task_init(void) {
    odrive_bridge_init();   // no-op em v56-stock (motor.setup já rodou no odrive_main())

    // Idempotente: se main.cpp já chamou ffb_init_storage_early() antes do
    // ADC iniciar (que é o caminho correto), esta chamada é no-op. Caso
    // contrário (build antiga sem o early hook), faz como fallback — mas
    // o overvoltage transiente pode ter disparado entre boot e este ponto.
    ffb_init_storage_early();

    // Phase 4.x — GPIO inputs (botões/eixos via GPIOs 1-4). Carrega cfg da EE
    // e configura cada pino conforme modo (button/axis/disabled).
    gpio_inputs_init();

    s_effects_calc = std::make_shared<EffectsCalculator>();
    s_hidffb = std::make_shared<HidFFB>(s_effects_calc, /*axisCount=*/1);

    // Phase 3.5 — força CUSTOM_PROFILE_ID. saveFlash do EffectsCalculator
    // só grava friction/damper/inertia se filterProfileId == CUSTOM_PROFILE_ID.
    // Sem isso esses filtros nunca persistiriam.
    s_effects_calc->setFilterProfileToCustom();

    auto axis_owned = std::make_unique<ODriveLocalAxis>();
    s_axis_raw = axis_owned.get();
    s_axes_vec.emplace_back(std::move(axis_owned));

    // Phase 3.5 — restore axis params (range/maxtorque/fxratio) + master gain
    // depois do ODriveLocalAxis construído. EffectsCalculator já restaurou
    // seus próprios dados (gains spring/damper/friction/inertia + filtros)
    // no construtor via restoreFlash().
    ffb_load_axis_params_internal();
    {
        uint16_t mg;
        if (Flash_Read(0x04F0, &mg, false) && mg != 0xFFFF && mg <= 255) {
            s_effects_calc->setGain((uint8_t)mg);
        }
    }

    s_hidffb->registerHidCallback();

    osThreadDef(ffbThread, ffb_thread, osPriorityAboveNormal, 0, 2048 / sizeof(StackType_t));
    osThreadCreate(osThread(ffbThread), NULL);
}

// Lê axis params da EEPROM. Se algum endereço retornar erro (primeira boot
// ou sector recém-formatado), mantém o default já bakado em ODriveLocalAxis.
// Encoding por endereço:
//   ADR_AXIS1_DEGREES      → range (uint16, graus)
//   ADR_AXIS1_POWER        → maxtorque ×100 (uint16)
//   ADR_AXIS1_EFFECTS1     → fxratio ×1000 (uint16)
//   ADR_AXIS1_EFFECTS2     → (idleSpring << 8) | axisDamper
//   ADR_AXIS1_POSTPROCESS1 → (axisInertia << 8) | axisFriction
//   ADR_AXIS1_MAX_SPEED    → maxTorqueRate (uint16)
//   ADR_AXIS1_MAX_ACCEL    → expo (signed 16, cast pra uint16)
//   ADR_AXIS1_ENDSTOP      → exposcale (uint16, mas só 1 byte usado)
static void ffb_load_axis_params_internal(void) {
    if (!s_axis_raw) return;
    uint16_t v;
    if (Flash_Read(ADR_AXIS1_DEGREES, &v, false) && v != 0xFFFF && v > 0) {
        s_axis_raw->rangeDegrees_ = (float)v;
    }
    if (Flash_Read(ADR_AXIS1_POWER, &v, false) && v != 0xFFFF && v > 0) {
        s_axis_raw->maxTorque_Nm_ = (float)v / 100.0f;
    }
    if (Flash_Read(ADR_AXIS1_EFFECTS1, &v, false) && v != 0xFFFF) {
        s_axis_raw->fxRatio_ = (float)v / 1000.0f;
    }
    if (Flash_Read(ADR_AXIS1_EFFECTS2, &v, false) && v != 0xFFFF) {
        s_axis_raw->idleSpring_ = (uint8_t)((v >> 8) & 0xFF);
        s_axis_raw->axisDamper_ = (uint8_t)(v & 0xFF);
    }
    if (Flash_Read(ADR_AXIS1_POSTPROCESS1, &v, false) && v != 0xFFFF) {
        s_axis_raw->axisInertia_  = (uint8_t)((v >> 8) & 0xFF);
        s_axis_raw->axisFriction_ = (uint8_t)(v & 0xFF);
    }
    if (Flash_Read(ADR_AXIS1_MAX_SPEED, &v, false) && v != 0xFFFF) {
        s_axis_raw->maxTorqueRate_ = v;
    }
    if (Flash_Read(ADR_AXIS1_MAX_ACCEL, &v, false) && v != 0xFFFF) {
        s_axis_raw->expo_ = (int16_t)v;
    }
    if (Flash_Read(ADR_AXIS1_ENDSTOP, &v, false) && v != 0xFFFF && v > 0 && v <= 255) {
        s_axis_raw->exposcale_ = (uint8_t)(v & 0xFF);
    }
    if (Flash_Read(ADR_AXIS1_ENC_RATIO, &v, false) && v != 0xFFFF) {
        s_axis_raw->endstopStrength_ = (uint8_t)(v & 0xFF);
        // High byte: endstopDamper_. Se = 0, é formato antigo (firmware antes da
        // separação spring/damper) — preserva default já inicializado (30).
        uint8_t dampStored = (uint8_t)((v >> 8) & 0xFF);
        if (dampStored != 0) {
            s_axis_raw->endstopDamper_ = dampStored;
        }
    }
    // Phase 4.x — bitfield de flags do axis (ADR_AXIS1_CONFIG)
    //   bit 0 = axisInverted_  (posição HID invertida)
    //   bit 1 = ffbInverted_   (torque FFB invertido) — novo
    // Compat com config antiga: bit 0 solto = comportamento herdado
    // (invertia posição, ficava aplicando o mesmo bit implicitamente no
    // torque via lógica antiga). Aqui só carregamos axis; usuário aciona
    // o novo ffbinvert explicitamente pela UI.
    if (Flash_Read(ADR_AXIS1_CONFIG, &v, false) && v != 0xFFFF) {
        s_axis_raw->axisInverted_ = (v & 0x0001) != 0;
        s_axis_raw->ffbInverted_  = (v & 0x0002) != 0;
    }
    // zeroOffset_ — split em 2 slots uint16 reconstruindo um float32.
    // Inicial 0.0f no construtor é o default seguro (sem offset).
    // Se ambos slots = 0xFFFF (EE virgem), preserva o default.
    {
        uint16_t lo, hi;
        bool okLo = Flash_Read(ADR_AXIS1_ZEROOFS_LO, &lo, false);
        bool okHi = Flash_Read(ADR_AXIS1_ZEROOFS_HI, &hi, false);
        if (okLo && okHi && !(lo == 0xFFFF && hi == 0xFFFF)) {
            union { uint32_t u; float f; } u;
            u.u = ((uint32_t)hi << 16) | (uint32_t)lo;
            // Sanity check: NaN/Inf → ignora (mantém default 0.0)
            if (__builtin_isfinite(u.f)) {
                s_axis_raw->zeroOffset_ = u.f;
            }
        }
    }
}

// -------------------- Bridges pros callbacks TinyUSB --------------------
extern "C" uint16_t usbhid_hidGet_bridge(uint8_t report_id, hid_report_type_t type,
                                           uint8_t *buf, uint16_t reqlen) {
    s_diag_hidget++;
    if (UsbHidHandler::globalHidHandler) {
        return UsbHidHandler::globalHidHandler->hidGet(report_id, type, buf, reqlen);
    }
    return 0;
}
extern "C" void usbhid_hidOut_bridge(uint8_t report_id, hid_report_type_t type,
                                       const uint8_t *buf, uint16_t size) {
    if (!UsbHidHandler::globalHidHandler) return;

    if (report_id == 0 && size > 0 && type == HID_REPORT_TYPE_OUTPUT) {
        report_id = buf[0];
    }

    // Diagnóstico — incrementa contador específico por report_id
    s_diag_hidout_total++;
    switch (report_id) {
        case 1:  s_diag_hidout_seteff++;  break;  // HID_ID_EFFREP (Set Effect)
        case 2:  s_diag_hidout_period++; break;   // HID_ID_PRIDREP (Periodic)
        case 5:  s_diag_hidout_const++;  break;   // HID_ID_CONSTREP (Constant Force)
        case 6:  s_diag_hidout_cond++;   break;   // HID_ID_CONDREP (Condition: Spring/Damper)
        case 10: s_diag_hidout_efop++;   break;   // HID_ID_EFOPREP (Effect Operation)
        case 12: s_diag_hidout_ctrl++;   break;   // HID_ID_CTRLREP (Enable/Disable Actuators)
        case 13: s_diag_hidout_gain++;   break;   // HID_ID_GAINREP
        case 17: s_diag_hidout_neweff++; break;   // HID_ID_NEWEFREP (Create Effect — Feature)
        default: s_diag_hidout_other++;  break;
    }

    UsbHidHandler::globalHidHandler->hidOut(report_id, type, buf, size);
}

extern "C" int ffb_is_active(void) {
    return (s_hidffb && s_hidffb->getFfbActive()) ? 1 : 0;
}

// Chamado pelo glue TinyUSB quando USB desconecta (tud_umount_cb).
// Force failsafe: stop_FFB para HidFFB.ffb_active=false, zera pending_torque,
// e escreve input_torque=0 imediato pra motor não manter torque com USB fora.
extern "C" void ffb_on_usb_unmount(void) {
    if (s_hidffb)   s_hidffb->stop_FFB();
    if (s_axis_raw) s_axis_raw->reset_ffb_state();
    odrive_bridge_set_input_torque(0.0f);
}

// Fase 6 — introspeccao pra validacao em jogo via CDC (sys.fxtest?;)
extern "C" float ffb_get_pending_torque_nm(void) {
    // Le o ultimo valor escrito no bridge — reflete o que sera entregue pra
    // axes[0].controller_.input_torque_ no proximo tick de 1kHz.
    // Acesso via ODriveLocal — pending_torque_ eh private, entao retornamos
    // via reverse-engineering do metrics.torque (int32 scaled) + maxTorque.
    if (!s_axis_raw) return 0.0f;
    metric_t *m = s_axis_raw->getMetrics();
    constexpr float maxTorque_Nm = 1.0f;
    // metrics.torque está no range [-0x7FFF, +0x7FFF] do EffectsCalculator.
    return (float)m->torque / (float)0x7FFF * maxTorque_Nm;
}

extern "C" float ffb_get_pos_degrees(void) {
    if (!s_axis_raw) return 0.0f;
    return s_axis_raw->getMetrics()->posDegrees;
}

extern "C" float ffb_get_speed(void) {
    if (!s_axis_raw) return 0.0f;
    return s_axis_raw->getMetrics()->speed;
}

extern "C" int ffb_count_active_effects(void) {
    // Conta efeitos com type != FFB_EFFECT_NONE na pool do EffectsCalculator.
    if (!s_effects_calc) return 0;
    int n = 0;
    for (size_t i = 0; i < s_effects_calc->effects.size(); i++) {
        if (s_effects_calc->effects[i].type != FFB_EFFECT_NONE) n++;
    }
    return n;
}

// Diagnósticos pro debug do pipeline FFB. Acessível via comando ASCII custom
// 'fd' (FFB diag) que adicionamos em ascii_protocol.cpp.
extern "C" uint32_t ffb_diag_hidout_total(void)   { return s_diag_hidout_total; }
extern "C" uint32_t ffb_diag_hidout_ctrl(void)    { return s_diag_hidout_ctrl; }
extern "C" uint32_t ffb_diag_hidout_neweff(void)  { return s_diag_hidout_neweff; }
extern "C" uint32_t ffb_diag_hidout_seteff(void)  { return s_diag_hidout_seteff; }
extern "C" uint32_t ffb_diag_hidout_cond(void)    { return s_diag_hidout_cond; }
extern "C" uint32_t ffb_diag_hidout_const(void)   { return s_diag_hidout_const; }
extern "C" uint32_t ffb_diag_hidout_period(void)  { return s_diag_hidout_period; }
extern "C" uint32_t ffb_diag_hidout_efop(void)    { return s_diag_hidout_efop; }
extern "C" uint32_t ffb_diag_hidout_gain(void)    { return s_diag_hidout_gain; }
extern "C" uint32_t ffb_diag_hidout_other(void)   { return s_diag_hidout_other; }
extern "C" uint32_t ffb_diag_hidget(void)         { return s_diag_hidget; }
extern "C" uint32_t ffb_diag_set_eff_torque(void) { return s_diag_set_eff_torque; }
extern "C" int32_t  ffb_diag_last_torque(void)    { return s_diag_last_torque_int32; }
extern "C" int      ffb_diag_active_effects(void) { return ffb_count_active_effects(); }
extern "C" float    ffb_diag_pending_torque(void) { return s_axis_raw ? s_axis_raw->getMetrics()->torque / (float)0x7FFF * (s_axis_raw->maxTorque_Nm_ * s_axis_raw->fxRatio_) : 0.0f; }
extern "C" int      ffb_diag_ffb_active_flag(void){ return (s_hidffb && s_hidffb->getFfbActive()) ? 1 : 0; }

// EQ por banda — bridge pro cmd_table.cpp. Band: 0=GRIP, 1=CHASSIS, 2=ROAD.
// Set vai pros dois axes (mesma config). Get lê do axis 0 (canônico).
// Freq/Q são read-only — expostos pra UI conseguir mostrar onde cada banda
// está centrada sem hardcodar em dois lugares.
extern "C" void ffb_eq_set_gain_db(int band, float gainDb) {
    if (!s_effects_calc) return;
    if (band < 0 || band >= EqCascade::BAND_COUNT) return;
    for (uint8_t axis = 0; axis < MAX_AXIS; ++axis) {
        s_effects_calc->getEqCascade(axis).setGain((EqCascade::Band)band, gainDb);
    }
}
extern "C" float ffb_eq_get_gain_db(int band) {
    if (!s_effects_calc) return 0.0f;
    if (band < 0 || band >= EqCascade::BAND_COUNT) return 0.0f;
    return s_effects_calc->getEqCascade(0).getGain((EqCascade::Band)band);
}
extern "C" float ffb_eq_get_freq_hz(int band) {
    if (!s_effects_calc) return 0.0f;
    if (band < 0 || band >= EqCascade::BAND_COUNT) return 0.0f;
    return s_effects_calc->getEqCascade(0).getFreq((EqCascade::Band)band);
}
extern "C" float ffb_eq_get_q(int band) {
    if (!s_effects_calc) return 0.0f;
    if (band < 0 || band >= EqCascade::BAND_COUNT) return 0.0f;
    return s_effects_calc->getEqCascade(0).getQ((EqCascade::Band)band);
}

// Encontra o N-ésimo effect com state != INACTIVE (n=0 → primeiro, n=1 →
// segundo, etc). Retorna ponteiro ou nullptr quando não há mais.
static FFB_Effect *find_nth_active_effect(int n, int *out_index) {
    if (!s_effects_calc || n < 0) return nullptr;
    int seen = 0;
    for (size_t i = 0; i < s_effects_calc->effects.size(); i++) {
        if (s_effects_calc->effects[i].state != 0 &&
            s_effects_calc->effects[i].type  != FFB_EFFECT_NONE) {
            if (seen == n) {
                if (out_index) *out_index = (int)i;
                return &s_effects_calc->effects[i];
            }
            seen++;
        }
    }
    return nullptr;
}

// Versões N-indexed pra dump de múltiplos efeitos no dashboard FFB Live.
extern "C" int     ffb_diag_eff_index_n(int n)     { int i=-1; find_nth_active_effect(n, &i); return i; }
extern "C" int     ffb_diag_eff_state_n(int n)     { auto *e=find_nth_active_effect(n, nullptr); return e?e->state:0; }
extern "C" int     ffb_diag_eff_type_n(int n)      { auto *e=find_nth_active_effect(n, nullptr); return e?e->type:0; }
extern "C" int32_t ffb_diag_eff_magnitude_n(int n) { auto *e=find_nth_active_effect(n, nullptr); return e?(int32_t)e->magnitude:0; }
extern "C" float   ffb_diag_eff_axmag0_n(int n)    { auto *e=find_nth_active_effect(n, nullptr); return e?e->axisMagnitudes[0]:0.0f; }
extern "C" int     ffb_diag_eff_gain_n(int n)      { auto *e=find_nth_active_effect(n, nullptr); return e?(int)e->gain:0; }

// "Slot dump" — info de QUALQUER slot por índice físico (não pula INACTIVE).
// Pra detectar efeitos alocados pelo jogo mas não startados (state=0).
extern "C" int     ffb_diag_slot_state(int slot)     {
    if (!s_effects_calc || slot < 0 || slot >= (int)s_effects_calc->effects.size()) return 0;
    return s_effects_calc->effects[slot].state;
}
extern "C" int     ffb_diag_slot_type(int slot)      {
    if (!s_effects_calc || slot < 0 || slot >= (int)s_effects_calc->effects.size()) return 0;
    return s_effects_calc->effects[slot].type;
}
extern "C" int32_t ffb_diag_slot_magnitude(int slot) {
    if (!s_effects_calc || slot < 0 || slot >= (int)s_effects_calc->effects.size()) return 0;
    return (int32_t)s_effects_calc->effects[slot].magnitude;
}

// Conta total de slots com type != FFB_EFFECT_NONE (alocados, mesmo INACTIVE)
extern "C" int     ffb_diag_total_slots(void) {
    if (!s_effects_calc) return 0;
    int n = 0;
    for (size_t i = 0; i < s_effects_calc->effects.size(); i++) {
        if (s_effects_calc->effects[i].type != FFB_EFFECT_NONE) n++;
    }
    return n;
}

// Backwards-compat: getters antigos retornam o primeiro (n=0)
extern "C" int     ffb_diag_eff_index(void)     { return ffb_diag_eff_index_n(0); }
extern "C" int     ffb_diag_eff_state(void)     { return ffb_diag_eff_state_n(0); }
extern "C" int     ffb_diag_eff_type(void)      { return ffb_diag_eff_type_n(0); }
extern "C" int32_t ffb_diag_eff_magnitude(void) { return ffb_diag_eff_magnitude_n(0); }
extern "C" float   ffb_diag_eff_axmag0(void)    { return ffb_diag_eff_axmag0_n(0); }
extern "C" int     ffb_diag_eff_gain(void)      { return ffb_diag_eff_gain_n(0); }
extern "C" int     ffb_diag_global_gain(void)   { return s_effects_calc?(int)s_effects_calc->getGlobalGain():0; }

// Phase 3.3 — getters/setters dos gains do EffectsCalculator. C linkage pra
// poder ser chamado do cmd_table.cpp sem expor o header completo (que
// colide com class Axis do ODrive).
extern "C" int  ffb_get_master_gain(void)  { return s_effects_calc ? (int)s_effects_calc->getGlobalGain() : 255; }
extern "C" void ffb_set_master_gain(int v) { if (s_effects_calc) s_effects_calc->setGain((uint8_t)v); }
extern "C" int  ffb_get_spring_gain(void)  { return s_effects_calc ? (int)s_effects_calc->getGainStruct().spring   : 64; }
extern "C" void ffb_set_spring_gain(int v) { if (s_effects_calc) s_effects_calc->getGainStruct().spring   = (uint8_t)v; }
extern "C" int  ffb_get_damper_gain(void)  { return s_effects_calc ? (int)s_effects_calc->getGainStruct().damper   : 64; }
extern "C" void ffb_set_damper_gain(int v) { if (s_effects_calc) s_effects_calc->getGainStruct().damper   = (uint8_t)v; }
extern "C" int  ffb_get_friction_gain(void){ return s_effects_calc ? (int)s_effects_calc->getGainStruct().friction : 254; }
extern "C" void ffb_set_friction_gain(int v){ if (s_effects_calc) s_effects_calc->getGainStruct().friction = (uint8_t)v; }
extern "C" int  ffb_get_inertia_gain(void) { return s_effects_calc ? (int)s_effects_calc->getGainStruct().inertia  : 127; }
extern "C" void ffb_set_inertia_gain(int v){ if (s_effects_calc) s_effects_calc->getGainStruct().inertia  = (uint8_t)v; }

// Phase 3.4 — getters/setters dos filtros biquad por tipo de efeito.
// Cada filtro tem freq (Hz, cutoff lowpass) e q (Q-factor scaled por 0.01).
// Setter aplica também aos efeitos já em execução via applyFilterChanges().
//
// IMPORTANTE — slot de filtro por tipo de efeito (Phase 4.x bug fix):
//   - friction/damper/inertia → filter[CUSTOM_PROFILE_ID] (= filter[1])
//     (consistente: restoreFlash escreve aqui, saveFlash lê daqui quando
//      filterProfileId==CUSTOM_PROFILE_ID, setFilters lê daqui em runtime)
//   - constant force         → filter[0]
//     (EffectsCalculator hardcoda filter[0] em setFilters/restoreFlash/
//      saveFlash; setter precisa escrever aqui pra valor persistir e
//      ser aplicado em runtime — antes escrevia em filter[1] e o valor
//      era silenciosamente ignorado).
//
// Macro só atende friction/damper/inertia. Constant força tem accessors
// próprios apontando pra filter[0].

extern "C" int ffb_get_filter_constant_freq(void) {
    return s_effects_calc ? (int)s_effects_calc->getFilterStructDefault().constant.freq : 0;
}
extern "C" void ffb_set_filter_constant_freq(int v) {
    if (!s_effects_calc) return;
    if (v < 1)   v = 1;
    if (v > 500) v = 500;
    s_effects_calc->getFilterStructDefault().constant.freq = (uint16_t)v;
    // Sincroniza filter[1] pra eventual refatoração futura ler consistente
    s_effects_calc->getFilterStruct().constant.freq = (uint16_t)v;
    s_effects_calc->applyFilterChanges();
}
extern "C" int ffb_get_filter_constant_q(void) {
    return s_effects_calc ? (int)s_effects_calc->getFilterStructDefault().constant.q : 0;
}
extern "C" void ffb_set_filter_constant_q(int v) {
    if (!s_effects_calc) return;
    if (v < 1)   v = 1;
    if (v > 500) v = 500;
    s_effects_calc->getFilterStructDefault().constant.q = (uint16_t)v;
    s_effects_calc->getFilterStruct().constant.q = (uint16_t)v;
    s_effects_calc->applyFilterChanges();
}

#define FFB_FILTER_ACCESSORS(name)                                              \
    extern "C" int  ffb_get_filter_##name##_freq(void) {                       \
        return s_effects_calc ? (int)s_effects_calc->getFilterStruct().name.freq : 0; \
    }                                                                          \
    extern "C" void ffb_set_filter_##name##_freq(int v) {                      \
        if (!s_effects_calc) return;                                           \
        if (v < 1)    v = 1;                                                   \
        if (v > 500)  v = 500;                                                 \
        s_effects_calc->getFilterStruct().name.freq = (uint16_t)v;             \
        s_effects_calc->applyFilterChanges();                                  \
    }                                                                          \
    extern "C" int  ffb_get_filter_##name##_q(void) {                          \
        return s_effects_calc ? (int)s_effects_calc->getFilterStruct().name.q : 0; \
    }                                                                          \
    extern "C" void ffb_set_filter_##name##_q(int v) {                         \
        if (!s_effects_calc) return;                                           \
        if (v < 1)   v = 1;                                                    \
        if (v > 500) v = 500;                                                  \
        s_effects_calc->getFilterStruct().name.q = (uint16_t)v;                \
        s_effects_calc->applyFilterChanges();                                  \
    }
FFB_FILTER_ACCESSORS(friction)
FFB_FILTER_ACCESSORS(damper)
FFB_FILTER_ACCESSORS(inertia)
#undef FFB_FILTER_ACCESSORS

// Bus current/voltage peaks — amostrado em 1kHz pelo ffb_thread acima.
// Sinal de ibus: positivo = consumo, negativo = regen voltando pra fonte.
extern "C" float ffb_diag_ibus_max(void)       { return s_ibus_max; }
extern "C" float ffb_diag_ibus_min(void)       { return s_ibus_min; }
extern "C" float ffb_diag_motor_ibus_max(void) { return s_motor_ibus_max; }
extern "C" float ffb_diag_motor_ibus_min(void) { return s_motor_ibus_min; }
extern "C" float ffb_diag_vbus_max(void)       { return s_vbus_max; }
extern "C" float ffb_diag_vbus_min(void)       { return s_vbus_min; }
extern "C" void  ffb_diag_reset_ibus_peaks(void) {
    s_ibus_max = 0.0f; s_ibus_min = 0.0f;
    s_motor_ibus_max = 0.0f; s_motor_ibus_min = 0.0f;
    s_vbus_max = 0.0f; s_vbus_min = 100.0f;
}

// Fase 7 — persiste os settings de FFB (filtros, gains) na EEPROM emulada.
// Chamado por sys.save. EE_WriteVariable nao faz Unlock/Lock internamente,
// entao envolvemos a chamada aqui.
extern "C" {
#include "stm32f4xx_hal.h"
}
// Contador de erros do último ffb_save_flash — exposto via sys.savestat
// pra diagnóstico de persistência.
static volatile int s_last_save_writes = 0;
static volatile int s_last_save_errors = 0;

extern "C" int ffb_save_flash(void) {
    if (!s_effects_calc) return 0;
    s_last_save_writes = 0;
    s_last_save_errors = 0;

    HAL_FLASH_Unlock();
    // Phase 4.x — limpa flags de erro latched (ver ffb_task_init pra explicação).
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP    | FLASH_FLAG_OPERR  |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGSERR | FLASH_FLAG_PGPERR);
    // EE_Init é chamado no boot (ffb_task_init). Redundância aqui foi removida
    // após Phase 3.13 — cada chamada de EE_Init pode triggerar erase em estados
    // raros (VALID+VALID transitório), o que adiciona latência e complexidade.
    s_effects_calc->saveFlash();   // gains spring/damper/friction/inertia + filtros

    // Master gain (global_gain) — não está no saveFlash do EffectsCalculator.
    // Endereço 0x04F0 (slot livre da VirtAddVarTab).
    if (!Flash_Write(0x04F0, (uint16_t)s_effects_calc->getGlobalGain())) s_last_save_errors++;
    s_last_save_writes++;

    // Phase 4.x — divisor VBUS (hw config, não axis-specific).
    if (!Flash_Write(ADR_VBUS_DIVIDER, (uint16_t)s_vbus_divider)) s_last_save_errors++;
    s_last_save_writes++;

    // GPIO axis processor (AnalogAxisProcessing port)
    // Flags bitmap: bit0=filter, bit1=autorange. Freq em décimos de Hz.
    {
        extern int   axis_proc_get_filter_enabled(void);
        extern int   axis_proc_get_autorange_enabled(void);
        extern float axis_proc_get_filter_freq(void);
        uint16_t flags = 0;
        if (axis_proc_get_filter_enabled())    flags |= 0x01;
        if (axis_proc_get_autorange_enabled()) flags |= 0x02;
        uint16_t freq_x10 = (uint16_t)(axis_proc_get_filter_freq() * 10.0f);
        if (!Flash_Write(ADR_GPIO_AXIS_FLAGS,    flags))    s_last_save_errors++;
        s_last_save_writes++;
        if (!Flash_Write(ADR_GPIO_AXIS_FREQ_X10, freq_x10)) s_last_save_errors++;
        s_last_save_writes++;
    }

    // EQ por banda — persiste ganhos em décimos de dB como int16
    // reinterpretado pra uint16 (Flash API só aceita uint16). Lê do
    // axis 0 porque os dois axes têm o mesmo ganho (só estado interno
    // difere). EqCascade clamps em ±12 dB → range guaranteed em ±120.
    if (s_effects_calc) {
        const uint16_t eqAddrs[3] = { ADR_AXIS_EQ_WEIGHT, ADR_AXIS_EQ_CHASSIS, ADR_AXIS_EQ_ROAD };
        for (uint8_t band = 0; band < 3; ++band) {
            const float dB = s_effects_calc->getEqCascade(0).getGain((EqCascade::Band)band);
            const int16_t x10 = (int16_t)(dB * 10.0f);
            if (!Flash_Write(eqAddrs[band], (uint16_t)x10)) s_last_save_errors++;
            s_last_save_writes++;
        }
    }

    if (s_axis_raw) {
        // Escala básica
        uint16_t r  = (uint16_t)(s_axis_raw->rangeDegrees_ < 65535.0f ? s_axis_raw->rangeDegrees_ : 65535.0f);
        uint16_t mt = (uint16_t)(s_axis_raw->maxTorque_Nm_ * 100.0f < 65535.0f ? s_axis_raw->maxTorque_Nm_ * 100.0f : 65535.0f);
        uint16_t fr = (uint16_t)(s_axis_raw->fxRatio_ * 1000.0f);
        if (!Flash_Write(ADR_AXIS1_DEGREES,  r))  s_last_save_errors++;
        if (!Flash_Write(ADR_AXIS1_POWER,    mt)) s_last_save_errors++;
        if (!Flash_Write(ADR_AXIS1_EFFECTS1, fr)) s_last_save_errors++;
        s_last_save_writes += 3;

        // Phase 3.13 — axis effects extras
        uint16_t spDa = ((uint16_t)s_axis_raw->idleSpring_ << 8) | s_axis_raw->axisDamper_;
        uint16_t inFr = ((uint16_t)s_axis_raw->axisInertia_ << 8) | s_axis_raw->axisFriction_;
        if (!Flash_Write(ADR_AXIS1_EFFECTS2,     spDa)) s_last_save_errors++;
        if (!Flash_Write(ADR_AXIS1_POSTPROCESS1, inFr)) s_last_save_errors++;
        if (!Flash_Write(ADR_AXIS1_MAX_SPEED, s_axis_raw->maxTorqueRate_)) s_last_save_errors++;
        if (!Flash_Write(ADR_AXIS1_MAX_ACCEL, (uint16_t)s_axis_raw->expo_))  s_last_save_errors++;
        if (!Flash_Write(ADR_AXIS1_ENDSTOP,   (uint16_t)s_axis_raw->exposcale_)) s_last_save_errors++;
        // ADR_AXIS1_ENC_RATIO empacota: low byte = esgain, high byte = esdamp
        uint16_t esPack = ((uint16_t)s_axis_raw->endstopDamper_ << 8) | s_axis_raw->endstopStrength_;
        if (!Flash_Write(ADR_AXIS1_ENC_RATIO, esPack)) s_last_save_errors++;
        s_last_save_writes += 6;

        // Phase 4.x — bitfield de flags do axis.
        //   bit 0 = axisInverted_ (posição HID)
        //   bit 1 = ffbInverted_  (torque FFB)
        // Bits 2..15 reservados pra futuras flags (deadzone, smoothing, etc.)
        // sem precisar de novos endereços na EE.
        uint16_t cfgFlags = (s_axis_raw->axisInverted_ ? 0x0001 : 0x0000)
                          | (s_axis_raw->ffbInverted_  ? 0x0002 : 0x0000);
        if (!Flash_Write(ADR_AXIS1_CONFIG, cfgFlags)) s_last_save_errors++;
        s_last_save_writes++;

        // zeroOffset_ (float32) split em 2 slots uint16. Reconstruído no load
        // como union{uint32, float}. Permite que o "Zero wheel position"
        // sobreviva reboots após Save — corrige o deslocamento residual da
        // encoder_offset_calibration sem precisar zerar a cada boot.
        {
            union { uint32_t u; float f; } u;
            u.f = s_axis_raw->zeroOffset_;
            if (!Flash_Write(ADR_AXIS1_ZEROOFS_LO, (uint16_t)(u.u & 0xFFFF))) s_last_save_errors++;
            if (!Flash_Write(ADR_AXIS1_ZEROOFS_HI, (uint16_t)(u.u >> 16)))    s_last_save_errors++;
            s_last_save_writes += 2;
        }
    }

    // Phase 4.x — GPIO inputs config (12 writes: 3 entries × 4 GPIOs)
    {
        int g_writes = 0, g_errors = 0;
        gpio_inputs_save(&g_writes, &g_errors);
        s_last_save_writes += g_writes;
        s_last_save_errors += g_errors;
    }

    HAL_FLASH_Lock();
    return s_last_save_errors == 0 ? 1 : 0;
}

extern "C" int ffb_save_writes(void) { return s_last_save_writes; }
extern "C" int ffb_save_errors(void) { return s_last_save_errors; }

// Phase 3.5 — diagnóstico EEPROM: write+read um valor conhecido em endereço
// reservado (0x04F1) e reporta sucesso/falha no nível baixo.
extern "C" int ffb_eetest(uint16_t want, uint16_t *got_out) {
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP    | FLASH_FLAG_OPERR  |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGSERR | FLASH_FLAG_PGPERR);
    bool wok = Flash_Write(0x04F1, want);
    HAL_FLASH_Lock();
    uint16_t got = 0xFFFF;
    bool rok = Flash_Read(0x04F1, &got, false);
    if (got_out) *got_out = got;
    return (wok && rok && got == want) ? 1 : 0;
}

// Diagnóstico cru das pages do EEPROM emulado. Reporta:
//   p0/p1 — primeiros uint16 das pages (status: ERASED=FFFF, RECEIVE=EEEE,
//           VALID=0000). Se ambos FFFF, EE_Init nunca formatou.
//   init  — return code de EE_Init re-executado agora
//   wrc/rrc/got — return codes de write/read e valor lido
extern "C" {
#include "eeprom.h"
}
extern "C" void ffb_eedump(char *buf, int bufsize) {
    if (!buf || bufsize < 96) { if (buf && bufsize > 0) buf[0] = 0; return; }
    uint16_t p0_pre = *(volatile uint16_t*)PAGE0_BASE_ADDRESS;
    uint16_t p1_pre = *(volatile uint16_t*)PAGE1_BASE_ADDRESS;

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP    | FLASH_FLAG_OPERR  |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGSERR | FLASH_FLAG_PGPERR);
    uint16_t init_rc = EE_Init();
    uint16_t wrc = EE_WriteVariable(0x04F2, 0x5A5A);
    uint32_t hal_err = HAL_FLASH_GetError();
    HAL_FLASH_Lock();

    uint16_t got = 0xFFFF;
    uint16_t rrc = EE_ReadVariable(0x04F2, &got);

    uint16_t p0_post = *(volatile uint16_t*)PAGE0_BASE_ADDRESS;
    uint16_t p1_post = *(volatile uint16_t*)PAGE1_BASE_ADDRESS;

    snprintf(buf, bufsize,
             "pre=%04X/%04X post=%04X/%04X bootRC=%u init=%u wrc=%u rrc=%u got=%04X halErr=%lX",
             (unsigned)p0_pre,  (unsigned)p1_pre,
             (unsigned)p0_post, (unsigned)p1_post,
             (unsigned)s_boot_ee_init_rc,
             (unsigned)init_rc, (unsigned)wrc, (unsigned)rrc,
             (unsigned)got, (unsigned long)hal_err);
}

// Phase 4.x — escape hatch: força format completo do EE com clear error
// flags. Usar quando bootRC != 0 e sys.save! continua falhando, ou quando
// suspeitamos de estado corrompido das pages. Reporta status de cada etapa
// pra diagnóstico.
extern "C" void ffb_eeformat(char *buf, int bufsize) {
    if (!buf || bufsize < 96) { if (buf && bufsize > 0) buf[0] = 0; return; }

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP    | FLASH_FLAG_OPERR  |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGSERR | FLASH_FLAG_PGPERR);

    // Erase explícito de PAGE0 (S1) — não confia em VerifyPageFullyErased
    FLASH_EraseInitTypeDef pE0;
    pE0.TypeErase = FLASH_TYPEERASE_SECTORS;
    pE0.Sector = PAGE0_ID;
    pE0.NbSectors = 1;
    pE0.VoltageRange = VOLTAGE_RANGE;
    uint32_t err0 = 0;
    HAL_StatusTypeDef rc_e0 = HAL_FLASHEx_Erase(&pE0, &err0);
    uint32_t hal0 = HAL_FLASH_GetError();

    // Programa VALID_PAGE em PAGE0_BASE (header)
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP    | FLASH_FLAG_OPERR  |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGSERR | FLASH_FLAG_PGPERR);
    HAL_StatusTypeDef rc_pgm = HAL_FLASH_Program(TYPEPROGRAM_HALFWORD,
                                                 PAGE0_BASE_ADDRESS, VALID_PAGE);

    // Erase explícito de PAGE1 (S2)
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP    | FLASH_FLAG_OPERR  |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGSERR | FLASH_FLAG_PGPERR);
    FLASH_EraseInitTypeDef pE1;
    pE1.TypeErase = FLASH_TYPEERASE_SECTORS;
    pE1.Sector = PAGE1_ID;
    pE1.NbSectors = 1;
    pE1.VoltageRange = VOLTAGE_RANGE;
    uint32_t err1 = 0;
    HAL_StatusTypeDef rc_e1 = HAL_FLASHEx_Erase(&pE1, &err1);
    uint32_t hal1 = HAL_FLASH_GetError();

    // Re-escreve cookie de layout
    EE_WriteVariable(ADR_FLASH_VERSION, EE_LAYOUT_VERSION);

    HAL_FLASH_Lock();

    uint16_t p0_post = *(volatile uint16_t*)PAGE0_BASE_ADDRESS;
    uint16_t p1_post = *(volatile uint16_t*)PAGE1_BASE_ADDRESS;

    snprintf(buf, bufsize,
             "e0=%u pgm=%u e1=%u hal0=%lX hal1=%lX p0=%04X p1=%04X",
             (unsigned)rc_e0, (unsigned)rc_pgm, (unsigned)rc_e1,
             (unsigned long)hal0, (unsigned long)hal1,
             (unsigned)p0_post, (unsigned)p1_post);
}

// Axis params (axis.range / axis.maxtorque / axis.fxratio) — persistem via
// EEPROM emulada quando integrarmos esses no saveFlash futuramente. Por
// enquanto sao runtime-only (resetam nos defaults no boot).
extern "C" int  ffb_get_axis_invert(void) { return (s_axis_raw && s_axis_raw->axisInverted_) ? 1 : 0; }
extern "C" void ffb_set_axis_invert(int v) { if (s_axis_raw) s_axis_raw->axisInverted_ = (v != 0); }
extern "C" int  ffb_get_ffb_invert(void)  { return (s_axis_raw && s_axis_raw->ffbInverted_)  ? 1 : 0; }
extern "C" void ffb_set_ffb_invert(int v) { if (s_axis_raw) s_axis_raw->ffbInverted_  = (v != 0); }

// Phase 4.x — divisor VBUS. Setter clamp em 1-50 e recalcula scale cache
// que é lido pelo IRQ de leitura de ADC (vbus_sense_adc_cb em low_level.cpp).
extern "C" int ffb_get_vbus_divider(void) { return (int)s_vbus_divider; }
extern "C" void ffb_set_vbus_divider(int v) {
    if (v < 1)  v = 1;
    if (v > 50) v = 50;
    s_vbus_divider = (uint8_t)v;
    g_vbus_voltage_scale = 3.3f * (float)s_vbus_divider / 4096.0f;
}

extern "C" float ffb_get_axis_range(void)  { return s_axis_raw ? s_axis_raw->rangeDegrees_ : 900.0f; }
extern "C" float ffb_get_axis_maxtq(void)  { return s_axis_raw ? s_axis_raw->maxTorque_Nm_ : 5.0f;   }
extern "C" float ffb_get_axis_fxratio(void){ return s_axis_raw ? s_axis_raw->fxRatio_      : 0.80f;  }
extern "C" void  ffb_set_axis_range(float v)  { if (s_axis_raw) s_axis_raw->rangeDegrees_ = v; }
extern "C" void  ffb_set_axis_maxtq(float v)  {
    if (!s_axis_raw) return;
    if (v < 0.1f) v = 0.1f; if (v > 25.0f) v = 25.0f;  // hard cap de seguranca (25 Nm)
    s_axis_raw->maxTorque_Nm_ = v;
}
extern "C" void  ffb_set_axis_fxratio(float v){
    if (!s_axis_raw) return;
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    s_axis_raw->fxRatio_ = v;
}

// Phase 3.12 — params extras do Axis (idlespring, damper, inertia, friction,
// maxtorquerate, expo). Defaults TODOS em 0 (no efeito).
extern "C" int  ffb_get_axis_idlespring(void)   { return s_axis_raw ? (int)s_axis_raw->idleSpring_   : 0; }
extern "C" void ffb_set_axis_idlespring(int v)  { if (s_axis_raw) s_axis_raw->idleSpring_   = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
extern "C" int  ffb_get_axis_damper(void)       { return s_axis_raw ? (int)s_axis_raw->axisDamper_   : 0; }
extern "C" void ffb_set_axis_damper(int v)      { if (s_axis_raw) s_axis_raw->axisDamper_   = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
extern "C" int  ffb_get_axis_inertia(void)      { return s_axis_raw ? (int)s_axis_raw->axisInertia_  : 0; }
extern "C" void ffb_set_axis_inertia(int v)     { if (s_axis_raw) s_axis_raw->axisInertia_  = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
extern "C" int  ffb_get_axis_friction(void)     { return s_axis_raw ? (int)s_axis_raw->axisFriction_ : 0; }
extern "C" void ffb_set_axis_friction(int v)    { if (s_axis_raw) s_axis_raw->axisFriction_ = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
extern "C" int  ffb_get_axis_esgain(void)       { return s_axis_raw ? (int)s_axis_raw->endstopStrength_ : 0; }
extern "C" void ffb_set_axis_esgain(int v)      { if (s_axis_raw) s_axis_raw->endstopStrength_ = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
extern "C" int  ffb_get_axis_esdamp(void)       { return s_axis_raw ? (int)s_axis_raw->endstopDamper_ : 15; }
extern "C" void ffb_set_axis_esdamp(int v)      { if (s_axis_raw) s_axis_raw->endstopDamper_ = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
extern "C" int  ffb_get_axis_maxtorquerate(void){ return s_axis_raw ? (int)s_axis_raw->maxTorqueRate_: 0; }
extern "C" void ffb_set_axis_maxtorquerate(int v){ if (s_axis_raw) s_axis_raw->maxTorqueRate_= (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); }
extern "C" int  ffb_get_axis_expo(void)         { return s_axis_raw ? (int)s_axis_raw->expo_         : 0; }
extern "C" void ffb_set_axis_expo(int v)        { if (s_axis_raw) s_axis_raw->expo_         = (int16_t)(v < -32767 ? -32767 : v > 32767 ? 32767 : v); }
extern "C" int  ffb_get_axis_exposcale(void)    { return s_axis_raw ? (int)s_axis_raw->exposcale_    : 100; }
extern "C" void ffb_set_axis_exposcale(int v)   { if (s_axis_raw) s_axis_raw->exposcale_    = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v); }
extern "C" void ffb_axis_zeroenc(void)          { if (s_axis_raw) s_axis_raw->zeroEncoder(); }
extern "C" float ffb_get_axis_zeroofs(void)     { return s_axis_raw ? s_axis_raw->zeroOffset_ : 0.0f; }
extern "C" void  ffb_set_axis_zeroofs(float v)  { if (s_axis_raw) s_axis_raw->zeroOffset_ = v; }

// Contadores de diagnostico do callback do Z do encoder (definidos em
// encoder.cpp do ODrive, instrumentados pelo nosso patch). Expostos via
// ASCII pra confirmar se Z esta sendo lido pelo STM32 EXTI:
//   - axis.zhits?   conta pulsos Z aceitos (nivel HIGH no callback = Z real)
//   - axis.zglitch? conta IRQs rejeitadas pelo debounce (nivel LOW = glitch
//                   capacitivo, EMI, ruido). Indicador de problema eletrico.
// Reset via axis.zhits=0 e axis.zglitch=0 (escreve zerando).
extern "C" {
extern volatile uint32_t g_z_hit_count;
extern volatile uint32_t g_z_glitch_count;
}
extern "C" uint32_t ffb_get_z_hits(void)     { return g_z_hit_count; }
extern "C" void     ffb_set_z_hits(uint32_t v)     { g_z_hit_count = v; }
extern "C" uint32_t ffb_get_z_glitches(void) { return g_z_glitch_count; }
extern "C" void     ffb_set_z_glitches(uint32_t v) { g_z_glitch_count = v; }

// Live readouts — mapeados pra metric_t do axis
extern "C" int   ffb_get_axis_curtorque(void)   { return s_axis_raw ? (int)s_axis_raw->getMetrics()->torque : 0; }
extern "C" float ffb_get_axis_curpos(void)      { return s_axis_raw ? s_axis_raw->getMetrics()->posDegrees : 0.0f; }
extern "C" float ffb_get_axis_curspd(void)      { return s_axis_raw ? s_axis_raw->getMetrics()->speed : 0.0f; }
extern "C" float ffb_get_axis_curaccel(void)    { return s_axis_raw ? s_axis_raw->getMetrics()->accel : 0.0f; }

// Fase 7 — accessors pros gains (fx.spring / fx.damper / fx.friction / fx.inertia).
// name: "spring" | "damper" | "friction" | "inertia".
// Retorna -1 se nome invalido.
extern "C" int ffb_get_gain(const char *name) {
    if (!s_effects_calc) return -1;
    if (!strcmp(name, "spring"))   return s_effects_calc->getGainStruct().spring;
    if (!strcmp(name, "damper"))   return s_effects_calc->getGainStruct().damper;
    if (!strcmp(name, "friction")) return s_effects_calc->getGainStruct().friction;
    if (!strcmp(name, "inertia"))  return s_effects_calc->getGainStruct().inertia;
    return -1;
}

extern "C" int ffb_set_gain(const char *name, int value) {
    if (!s_effects_calc) return 0;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    if (!strcmp(name, "spring"))        s_effects_calc->getGainStruct().spring   = (uint8_t)value;
    else if (!strcmp(name, "damper"))   s_effects_calc->getGainStruct().damper   = (uint8_t)value;
    else if (!strcmp(name, "friction")) s_effects_calc->getGainStruct().friction = (uint8_t)value;
    else if (!strcmp(name, "inertia"))  s_effects_calc->getGainStruct().inertia  = (uint8_t)value;
    else return 0;
    return 1;
}
