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
    float fxRatio_       = 0.80f;     // 0..1 — margem pra endstop futuro

    // ----------- Axis effects (sempre ativos, somam ao FFB do jogo) ---------
    // Defaults TODOS em 0 — não muda comportamento até user setar.
    uint8_t idleSpring_     = 0;      // spring centralizadora quando FFB inativo
    uint8_t axisDamper_     = 0;      // damper sempre ativo (resistência velocidade)
    uint8_t axisInertia_    = 0;      // inertia sempre ativa (resistência aceleração)
    uint8_t axisFriction_   = 0;      // friction sempre ativa (atrito constante)
    uint8_t endstopStrength_= 0;      // batente eletrônico quando volante passa ±range/2 (0-255)

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
    bool inverted_ = false;

    void setEffectTorque(int32_t torque) override {
        // Phase 4.x — inverte torque vindo do jogo se inverted_ ativo.
        if (inverted_) torque = -torque;

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

        // 5. Endstop eletrônico — força crescente proporcional ao overshoot.
        // Quando posição passa ±range/2, aplica torque oposto cresce com profundidade.
        // Replica formula do OpenFFBoard: F = overshoot_deg * strength * gain (gain const = 25).
        if (endstopStrength_ != 0) {
            float halfRange = rangeDegrees_ / 2.0f;
            float pos = metrics_.posDegrees;
            const float endstopGain = 25.0f;
            if (pos > halfRange) {
                float overshoot = pos - halfRange;
                float force = -overshoot * (float)endstopStrength_ * endstopGain;
                if (force < -32767.0f) force = -32767.0f;
                axisEffectTorque_ += (int32_t)force;
            } else if (pos < -halfRange) {
                float overshoot = -pos - halfRange;
                float force = overshoot * (float)endstopStrength_ * endstopGain;
                if (force > 32767.0f) force = 32767.0f;
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
        float new_speed = (degrees - metrics_.posDegrees) / dt;
        float new_accel = (new_speed - metrics_.speed) / dt;

        metrics_.posDegrees = degrees;
        metrics_.pos_f = pos_f;
        metrics_.pos_scaled_16b = (int32_t)(pos_f * 32767.0f);
        metrics_.speed = new_speed;
        metrics_.accel = new_accel;

        // Ownership do input_torque é determinado pelo HID FFB control flag.
        if (s_hidffb && s_hidffb->getFfbActive()) {
            odrive_bridge_set_input_torque(pending_torque_);
        }
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
        return inverted_ ? -metrics_.pos_scaled_16b : metrics_.pos_scaled_16b;
    }

private:
    metric_t metrics_{};
    float pending_torque_     = 0.0f;
    int32_t axisEffectTorque_ = 0;       // calculado em calculateAxisEffects
    int32_t lastTorque_       = 0;       // pra slew rate limit
    float zeroOffset_         = 0.0f;    // offset (graus) aplicado em update()

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

extern "C" void ffb_task_init(void) {
    odrive_bridge_init();   // no-op em v56-stock (motor.setup já rodou no odrive_main())

    // Phase 3.5 — inicializa EEPROM emulada ANTES do EffectsCalculator.
    // Construtor do EffectsCalculator chama restoreFlash() pra carregar gains
    // e filtros — sem EE_Init prévio, leituras retornam erro e tudo cai em
    // defaults. Pages em S1+S2 (movidas na Phase 4.x — antes em S10+S11
    // que colidia com ODrive NVM).
    HAL_FLASH_Unlock();
    // Phase 4.x — STM32F4 latch nas flags de erro de flash (PGAERR/PGSERR/
    // WRPERR/PGPERR). Se ODrive (ou qualquer outro código antes de nós)
    // deixou alguma seteada, FLASH_WaitForLastOperation retorna HAL_ERROR
    // em CASCATA pra todas as próximas ops, mesmo as válidas. ClearError
    // resolve. ODrive's stm32_nvm.c faz isso, nossa EE não fazia → bug.
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP    | FLASH_FLAG_OPERR  |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGSERR | FLASH_FLAG_PGPERR);
    s_boot_ee_init_rc = EE_Init();

    // Phase 4.x — cookie de versão de layout. Se EE foi formatada por uma
    // versão antiga do firmware com layout diferente (ex: outras posições
    // de bits, outros endereços virtuais), os dados velhos NÃO devem ser
    // interpretados como válidos. Lê ADR_FLASH_VERSION; se não bater com
    // EE_LAYOUT_VERSION, formata. Após formatar, escreve a versão atual
    // pra próximos boots passarem direto. Também trata o caso de EE_Init
    // ter falhado (s_boot_ee_init_rc != 0) — força format pra recuperar.
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

    // Phase 4.x — VBUS divider configurável. Carrega da EE (se válido) e
    // recalcula g_vbus_voltage_scale que é lido pelo IRQ de leitura do ADC.
    // Default 19 (MKS XDrive Mini); ODrive v3.6 oficial = 11.
    {
        uint16_t v;
        if (Flash_Read(ADR_VBUS_DIVIDER, &v, false) && v != 0xFFFF && v >= 1 && v <= 50) {
            ffb_set_vbus_divider((int)v);
        }
    }

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
    if (Flash_Read(ADR_AXIS1_ENC_RATIO, &v, false) && v != 0xFFFF && v <= 255) {
        s_axis_raw->endstopStrength_ = (uint8_t)(v & 0xFF);
    }
    // Phase 4.x — bitfield de flags do axis (ADR_AXIS1_CONFIG)
    if (Flash_Read(ADR_AXIS1_CONFIG, &v, false) && v != 0xFFFF) {
        s_axis_raw->inverted_ = (v & 0x0001) != 0;
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
        if (!Flash_Write(ADR_AXIS1_ENC_RATIO, (uint16_t)s_axis_raw->endstopStrength_)) s_last_save_errors++;
        s_last_save_writes += 6;

        // Phase 4.x — bitfield de flags do axis. Atualmente só bit 0 = inverted_.
        // Outros bits ficam reservados pra futuras flags (ex: deadzone enable,
        // smoothing, etc.) sem precisar de novos endereços na EE.
        uint16_t cfgFlags = (s_axis_raw->inverted_ ? 0x0001 : 0x0000);
        if (!Flash_Write(ADR_AXIS1_CONFIG, cfgFlags)) s_last_save_errors++;
        s_last_save_writes++;
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
extern "C" int  ffb_get_axis_invert(void) { return (s_axis_raw && s_axis_raw->inverted_) ? 1 : 0; }
extern "C" void ffb_set_axis_invert(int v) { if (s_axis_raw) s_axis_raw->inverted_ = (v != 0); }

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
    if (v < 0.1f) v = 0.1f; if (v > 20.0f) v = 20.0f;  // hard cap de seguranca
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
extern "C" int  ffb_get_axis_maxtorquerate(void){ return s_axis_raw ? (int)s_axis_raw->maxTorqueRate_: 0; }
extern "C" void ffb_set_axis_maxtorquerate(int v){ if (s_axis_raw) s_axis_raw->maxTorqueRate_= (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); }
extern "C" int  ffb_get_axis_expo(void)         { return s_axis_raw ? (int)s_axis_raw->expo_         : 0; }
extern "C" void ffb_set_axis_expo(int v)        { if (s_axis_raw) s_axis_raw->expo_         = (int16_t)(v < -32767 ? -32767 : v > 32767 ? 32767 : v); }
extern "C" int  ffb_get_axis_exposcale(void)    { return s_axis_raw ? (int)s_axis_raw->exposcale_    : 100; }
extern "C" void ffb_set_axis_exposcale(int v)   { if (s_axis_raw) s_axis_raw->exposcale_    = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v); }
extern "C" void ffb_axis_zeroenc(void)          { if (s_axis_raw) s_axis_raw->zeroEncoder(); }

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
