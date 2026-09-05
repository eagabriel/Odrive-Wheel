
#include "odrive_main.h"
#include <Drivers/STM32/stm32_system.h>
#include <bitset>
#include <cmsis_os.h>   // osDelay no retry do mt6835_transfer24

Encoder::Encoder(TIM_HandleTypeDef* timer, Stm32Gpio index_gpio,
                 Stm32Gpio hallA_gpio, Stm32Gpio hallB_gpio, Stm32Gpio hallC_gpio,
                 Stm32SpiArbiter* spi_arbiter) :
        timer_(timer), index_gpio_(index_gpio),
        hallA_gpio_(hallA_gpio), hallB_gpio_(hallB_gpio), hallC_gpio_(hallC_gpio),
        spi_arbiter_(spi_arbiter)
{
}

static void enc_index_cb_wrapper(void* ctx) {
    reinterpret_cast<Encoder*>(ctx)->enc_index_cb();
}

// Option A (MT6835) — kick do semáforo da thread de leitura SPI, definido em
// Odrive-Wheel/src/odrive_bridge.cpp. Chamado do update() (control loop IRQ de
// baixa prioridade — CMSIS permitido), NUNCA do sampling_cb (prio-0, proibido).
extern "C" void odrive_bridge_enc_spi_kick(void);

// Contadores de diagnostico do callback do Z — expostos via ASCII pelo
// nosso firmware (axis.zhits?, axis.zglitch?). Vivem aqui como statics
// pra serem incrementados dentro do callback sem dependencia de instancia.
// volatile porque sao escritos por ISR e lidos por main loop.
volatile uint32_t g_z_hit_count = 0;       // pulsos Z aceitos (HIGH no callback)
volatile uint32_t g_z_glitch_count = 0;    // IRQs rejeitadas (LOW = glitch capacitivo)

bool Encoder::apply_config(ODriveIntf::MotorIntf::MotorType motor_type) {
    config_.parent = this;

    update_pll_gains();

    if (config_.pre_calibrated) {
        if (config_.mode == Encoder::MODE_HALL && config_.hall_polarity_calibrated)
            is_ready_ = true;
        if (config_.mode == Encoder::MODE_SINCOS)
            is_ready_ = true;
        if (motor_type == Motor::MOTOR_TYPE_ACIM)
            is_ready_ = true;
    }

    return true;
}

void Encoder::setup() {
    HAL_TIM_Encoder_Start(timer_, TIM_CHANNEL_ALL);
    set_idx_subscribe();

    mode_ = config_.mode;

    spi_task_.config = {
        .Mode = SPI_MODE_MASTER,
        .Direction = SPI_DIRECTION_2LINES,
        .DataSize = SPI_DATASIZE_16BIT,
        // MT6835 usa SPI mode 3 (CPOL=1, CPHA=1) per datasheet 7.6.2; AEAT/MA732 também CPOL=1.
        .CLKPolarity = (mode_ == MODE_SPI_ABS_AEAT || mode_ == MODE_SPI_ABS_MA732 || mode_ == MODE_SPI_ABS_MT6835) ? SPI_POLARITY_HIGH : SPI_POLARITY_LOW,
        .CLKPhase = SPI_PHASE_2EDGE,
        .NSS = SPI_NSS_SOFT,
        // MKS XDrive Mini: a 2.6 MHz (/16) o MISO chega no STM32 com edges
        // arredondadas (R-série + C parasita no trace) e amplitude de 2.2V em
        // vez de 3.3V — bits podem flippar no threshold. /64 = 656 kHz dá
        // ~1.5 µs por bit, tempo de sobra pra settling. Custo: ~20% CPU no
        // ISR de 8 kHz (era 5% em /16) — aceitável.
        // Alinhado com DRV8301 (drv8301.cpp:14) — /16 = 2.625 MHz no SPI3.
        // 6 µs por transação polled — sobra muito tempo no ISR de 8 kHz.
        // MT6835: lê 3 frames por transação (vs 1 do AS5047), então a /16 ela
        // bloqueia ~3x mais tempo. Aceita SCK até 16.67 MHz (datasheet 7.6.2),
        // então usamos /4 (10.5 MHz) pra cortar o tempo ao máximo. /2 (21 MHz)
        // passaria do limite. Se aparecerem CRC fails (pty subindo em
        // sys.encraw!), baixar pra /8 e, se persistir, /16.
        .BaudRatePrescaler = (mode_ == MODE_SPI_ABS_MT6835) ? SPI_BAUDRATEPRESCALER_4 : SPI_BAUDRATEPRESCALER_16,
        .FirstBit = SPI_FIRSTBIT_MSB,
        .TIMode = SPI_TIMODE_DISABLE,
        .CRCCalculation = SPI_CRCCALCULATION_DISABLE,
        .CRCPolynomial = 10,
    };

    if (mode_ == MODE_SPI_ABS_MA732) {
        abs_spi_dma_tx_[0] = 0x0000;
    }

    if (mode_ == MODE_SPI_ABS_MT6835) {
        // Burst Read Angle (datasheet 7.6.9): comando C3~C0=1010 (0xA) + endereço
        // 0x003 → palavra de 16 bits 0xA003. Depois do comando o chip emite em
        // sequência os regs 0x003,0x004,0x005,0x006 (4 bytes). Lemos tudo numa
        // única transação de 3×16 bits (CS fica baixo o tempo todo):
        //   word0 (TX 0xA003): comando; MISO=Hi-Z → RX0 descartado
        //   word1 (TX 0x0000): RX1 = [reg0x003][reg0x004]  (ANGLE[20:13],[12:5])
        //   word2 (TX 0x0000): RX2 = [reg0x005][reg0x006]  (ANGLE[4:0]+STATUS, CRC8)
        abs_spi_dma_tx_[0] = 0xA003;
        abs_spi_dma_tx_[1] = 0x0000;
        abs_spi_dma_tx_[2] = 0x0000;
    }

    if(mode_ & MODE_FLAG_ABS){
        abs_spi_cs_pin_init();

        // MKS XDrive Mini: AS5047 latcha o bit EF (Error Flag, bit 14 do response)
        // quando recebe qualquer comando malformado — incluindo o lixo enviado nos
        // primeiros clocks do boot antes do MOSI estar estável. EF é sticky: só
        // limpa lendo o registro ERRFL (0x0001). ODrive original nunca lê ERRFL
        // — só faz read de ANGLECOM em loop — então EF fica preso pra sempre,
        // e toda leitura subsequente é rejeitada por COM_FAIL mesmo com posição
        // válida e magneto presente.
        //
        // Solução: na boot, mandar 2 transações dummy: a 1ª pede ERRFL (resposta
        // é lixo), a 2ª pede ANGLECOM (resposta = ERRFL contents, e ato de ler
        // ERRFL limpa o bit no chip). A partir daí EF deve estar 0 enquanto o
        // chip está operando normal.
        if (mode_ == MODE_SPI_ABS_AMS) {
            uint16_t saved_tx = abs_spi_dma_tx_[0];
            // Transaction 1: send "read ERRFL" command (0x4001 = R bit + addr 0x0001,
            // parity bit 0 because addr already has even number of 1s).
            abs_spi_dma_tx_[0] = 0x4001;
            spi_arbiter_->transfer(spi_task_.config, abs_spi_cs_gpio_,
                                   (uint8_t*)abs_spi_dma_tx_,
                                   (uint8_t*)abs_spi_dma_rx_,
                                   1, /*timeout_ms=*/5);
            // Transaction 2: send "read ANGLECOM" command. Response is ERRFL contents
            // — read clears the error flags in the chip per AS5047 spec.
            abs_spi_dma_tx_[0] = saved_tx;  // 0xFFFF
            spi_arbiter_->transfer(spi_task_.config, abs_spi_cs_gpio_,
                                   (uint8_t*)abs_spi_dma_tx_,
                                   (uint8_t*)abs_spi_dma_rx_,
                                   1, /*timeout_ms=*/5);
        }

        // MT6835 boot init. Roda ANTES da enc_spi_thread existir (rtos_main
        // chama encoder.setup() antes de odrive_bridge_start_enc_thread) —
        // sem contenção no barramento aqui.
        if (mode_ == MODE_SPI_ABS_MT6835) {
            // (a) Verificação de comunicação: até 3 burst reads validadas por
            // CRC (mais forte que ler USER_ID — um MISO preso em 0 devolveria
            // um "dado" plausível, mas nunca um CRC válido). Falha aqui não
            // bloqueia o boot — sys.encraw! conta CRC fail em runtime.
            for (int i = 0; i < 3 && !mt6835_boot_comm_ok_; ++i) {
                uint32_t ok_before = abs_spi_ok_count_;
                abs_spi_start_transaction();
                mt6835_boot_comm_ok_ = (abs_spi_ok_count_ != ok_before);
            }

            // (b) HYST = 0 (reg 0x00D bits[2:0] = 0x4, datasheet 10.7). O
            // default de fábrica (0x7 = 0.011°) é uma zona morta de ~64 counts
            // em 2^21 a cada inversão de sentido — indesejável pra FFB. Escrita
            // VOLÁTIL (register map recarrega da EEPROM no power-on), então
            // reaplicada a cada boot e sem desgaste de EEPROM. RMW preserva
            // ROT_DIR (bit 3) e os bits MagnTek Use Only (7:4).
            uint8_t reg0d = 0;
            if (mt6835_boot_comm_ok_ && mt6835_read_reg(0x00D, &reg0d)) {
                if ((reg0d & 0x07) == 0x04) {
                    mt6835_hyst_zeroed_ = true;
                } else if (mt6835_write_reg(0x00D, (uint8_t)((reg0d & ~0x07) | 0x04))) {
                    uint8_t rb = 0;
                    mt6835_hyst_zeroed_ = mt6835_read_reg(0x00D, &rb)
                                          && (rb & 0x07) == 0x04;
                }
            }
        }

        if (axis_->controller_.config_.anticogging.pre_calibrated) {
            axis_->controller_.anticogging_valid_ = true;
        }
    }
}

void Encoder::set_error(Error error) {
    vel_estimate_valid_ = false;
    pos_estimate_valid_ = false;
    error_ |= error;
    axis_->error_ |= Axis::ERROR_ENCODER_FAILED;
}

bool Encoder::do_checks(){
    return error_ == ERROR_NONE;
}

//--------------------
// Hardware Dependent
//--------------------

// Triggered when an encoder passes over the "Index" pin
// TODO: only arm index edge interrupt when we know encoder has powered up
// (maybe by attaching the interrupt on start search, synergistic with following)
void Encoder::enc_index_cb() {
    if (config_.use_index) {
        // Debounce/glitch rejection — verifica nivel ATUAL do pino antes
        // de aceitar a IRQ rising como pulso Z real. Glitches capacitivos
        // do PWM do driver (acoplamento via cabo) podem disparar EXTI
        // rising falsa, mas o nivel volta a LOW em ~50-100 ns. Pulso Z
        // real do encoder dura tipicamente >= 1 us. Quando o callback
        // roda (latencia ~100 ns no F405), pulso real ainda esta HIGH;
        // glitch ja voltou pra LOW.
        //
        // Polaridade assumida: idle=LOW, pulse=HIGH (padrao ABZ
        // industrial com pull-down). Se o encoder usar idle=HIGH, esse
        // teste rejeitaria pulsos reais — nesse caso precisa de uma
        // flag de config invert, mas a maioria de encoders ABZ usa
        // idle=LOW.
        if (!index_gpio_.read()) {
            // glitch — ignora, NAO desinscreve (continua armado)
            g_z_glitch_count++;
            return;
        }
        g_z_hit_count++;

        set_circular_count(0, false);
        if (config_.use_index_offset)
            set_linear_count((int32_t)(config_.index_offset * config_.cpr));
        if (config_.pre_calibrated) {
            is_ready_ = true;
            if(axis_->controller_.config_.anticogging.pre_calibrated){
                axis_->controller_.anticogging_valid_ = true;
            }
        } else {
            // We can't use the update_offset facility in set_circular_count because
            // we also set the linear count before there is a chance to update. Therefore:
            // Invalidate offset calibration that may have happened before idx search
            is_ready_ = false;
        }
        index_found_ = true;
    }

    // Disable interrupt
    index_gpio_.unsubscribe();
}

void Encoder::set_idx_subscribe(bool override_enable) {
    if (config_.use_index && (override_enable || !config_.find_idx_on_lockin_only)) {
        if (!index_gpio_.subscribe(true, false, enc_index_cb_wrapper, this)) {
            odrv.misconfigured_ = true;
        }
    } else if (!config_.use_index || config_.find_idx_on_lockin_only) {
        index_gpio_.unsubscribe();
    }
}

void Encoder::update_pll_gains() {
    pll_kp_ = 2.0f * config_.bandwidth;  // basic conversion to discrete time
    pll_ki_ = 0.25f * (pll_kp_ * pll_kp_); // Critically damped

    // Check that we don't get problems with discrete time approximation
    if (!(current_meas_period * pll_kp_ < 1.0f)) {
        set_error(ERROR_UNSTABLE_GAIN);
    }
}

void Encoder::check_pre_calibrated() {
    // TODO: restoring config from python backup is fragile here (ACIM motor type must be set first)
    if (axis_->motor_.config_.motor_type != Motor::MOTOR_TYPE_ACIM) {
        if (!is_ready_)
            config_.pre_calibrated = false;
        if (mode_ == MODE_INCREMENTAL && !index_found_)
            config_.pre_calibrated = false;
    }
}

// Function that sets the current encoder count to a desired 32-bit value.
void Encoder::set_linear_count(int32_t count) {
    // Disable interrupts to make a critical section to avoid race condition
    uint32_t prim = cpu_enter_critical();

    // Update states
    shadow_count_ = count;
    pos_estimate_counts_ = (float)count;
    tim_cnt_sample_ = count;

    //Write hardware last
    timer_->Instance->CNT = count;

    cpu_exit_critical(prim);
}

// Function that sets the CPR circular tracking encoder count to a desired 32-bit value.
// Note that this will get mod'ed down to [0, cpr)
void Encoder::set_circular_count(int32_t count, bool update_offset) {
    // Disable interrupts to make a critical section to avoid race condition
    uint32_t prim = cpu_enter_critical();

    if (update_offset) {
        config_.phase_offset += count - count_in_cpr_;
        config_.phase_offset = mod(config_.phase_offset, config_.cpr);
    }

    // Update states
    count_in_cpr_ = mod(count, config_.cpr);
    pos_cpr_counts_ = (float)count_in_cpr_;

    cpu_exit_critical(prim);
}

bool Encoder::run_index_search() {
    config_.use_index = true;
    index_found_ = false;
    set_idx_subscribe();

    bool success = axis_->run_lockin_spin(axis_->config_.calibration_lockin, false);
    return success;
}

bool Encoder::run_direction_find() {
    int32_t init_enc_val = shadow_count_;

    Axis::LockinConfig_t lockin_config = axis_->config_.calibration_lockin;
    lockin_config.finish_distance = lockin_config.vel * 3.0f; // run for 3 seconds
    lockin_config.finish_on_distance = true;
    lockin_config.finish_on_enc_idx = false;
    lockin_config.finish_on_vel = false;
    bool success = axis_->run_lockin_spin(lockin_config, false);

    if (success) {
        // Check response and direction
        if (shadow_count_ > init_enc_val + 8) {
            // motor same dir as encoder
            config_.direction = 1;
        } else if (shadow_count_ < init_enc_val - 8) {
            // motor opposite dir as encoder
            config_.direction = -1;
        } else {
            config_.direction = 0;
        }
    }

    return success;
}


bool Encoder::run_hall_polarity_calibration() {
    Axis::LockinConfig_t lockin_config = axis_->config_.calibration_lockin;
    lockin_config.finish_distance = lockin_config.vel * 3.0f; // run for 3 seconds
    lockin_config.finish_on_distance = true;
    lockin_config.finish_on_enc_idx = false;
    lockin_config.finish_on_vel = false;

    auto loop_cb = [this](bool const_vel) {
        if (const_vel)
            sample_hall_states_ = true;
        // No need to cancel early
        return true;
    };

    config_.hall_polarity_calibrated = false;
    states_seen_count_.fill(0);
    bool success = axis_->run_lockin_spin(lockin_config, false, loop_cb);
    sample_hall_states_ = false;

    if (success) {
        std::bitset<8> state_seen;
        std::bitset<8> state_confirmed;
        for (int i = 0; i < 8; i++) {
            if (states_seen_count_[i] > 0)
                state_seen[i] = true;
            if (states_seen_count_[i] > 50)
                state_confirmed[i] = true;
        }
        if (!(state_seen == state_confirmed)) {
            set_error(ERROR_ILLEGAL_HALL_STATE);
            return false;
        }

        // Hall effect sensors can be arranged at 60 or 120 electrical degrees.
        // Out of 8 possible states, 120 and 60 deg arrangements each miss 2 states.
        // ODrive assumes 120 deg separation - if a 60 deg setup is used, it can
        // be converted to 120 deg states by flipping the polarity of one sensor.
        uint8_t states = state_seen.to_ulong();
        uint8_t hall_polarity = 0;
        auto flip_detect = [](uint8_t states, unsigned int idx)->bool {
            return (~states & 0xFF) == (1<<(0+idx) | 1<<(7-idx));
        };
        if (flip_detect(states, 0)) {
            hall_polarity = 0b000;
        } else if (flip_detect(states, 1)) {
            hall_polarity = 0b001;
        } else if (flip_detect(states, 2)) {
            hall_polarity = 0b010;
        } else if (flip_detect(states, 3)) {
            hall_polarity = 0b100;
        } else {
            set_error(ERROR_ILLEGAL_HALL_STATE);
            return false;
        }
        config_.hall_polarity = hall_polarity;
        config_.hall_polarity_calibrated = true;
    }

    return success;
}

bool Encoder::run_hall_phase_calibration() {
    Axis::LockinConfig_t lockin_config = axis_->config_.calibration_lockin;
    lockin_config.finish_distance = lockin_config.vel * 30.0f; // run for 30 seconds
    lockin_config.finish_on_distance = true;
    lockin_config.finish_on_enc_idx = false;
    lockin_config.finish_on_vel = false;

    auto loop_cb = [this](bool const_vel) {
        if (const_vel)
            sample_hall_phase_ = true;
        // No need to cancel early
        return true;
    };

    // TODO: There is a race condition here with the execution in Encoder::update.
    // We should evaluate making thread execution synchronous with the control loops
    // at least optionally.
    // Perhaps the new loop_sync feature will give a loose timing guarantee that may be sufficient
    calibrate_hall_phase_ = true;
    config_.hall_edge_phcnt.fill(0.0f);
    hall_phase_calib_seen_count_.fill(0);
    bool success = axis_->run_lockin_spin(lockin_config, false, loop_cb);
    if (error_ & ERROR_ILLEGAL_HALL_STATE)
        success = false;

    if (success) {
        // Check deltas to dicern rotation direction
        float delta_phase = 0.0f;
        for (int i = 0; i < 6; i++) {
            int next_i = (i == 5) ? 0 : i+1;
            delta_phase += wrap_pm_pi(config_.hall_edge_phcnt[next_i] - config_.hall_edge_phcnt[i]);
        }
        // Correct reverse rotation
        if (delta_phase < 0.0f) {
            config_.direction = -1;
            for (int i = 0; i < 6; i++)
                config_.hall_edge_phcnt[i] = wrap_pm_pi(-config_.hall_edge_phcnt[i]);
        } else {
            config_.direction = 1;
        }
        // Normalize edge timing to 1st edge in sequence, and change units to counts
        float offset = config_.hall_edge_phcnt[0];
        for (int i = 0; i < 6; i++) {
            float& phcnt = config_.hall_edge_phcnt[i];
            phcnt = fmodf_pos((6.0f / (2.0f * M_PI)) * (phcnt - offset), 6.0f);
        }
    } else {
        config_.hall_edge_phcnt = hall_edge_defaults;
    }

    calibrate_hall_phase_ = false;
    return success;
}

// @brief Turns the motor in one direction for a bit and then in the other
// direction in order to find the offset between the electrical phase 0
// and the encoder state 0.
bool Encoder::run_offset_calibration() {
    const float start_lock_duration = 1.0f;

    // Require index found if enabled
    if (config_.use_index && !index_found_) {
        set_error(ERROR_INDEX_NOT_FOUND_YET);
        return false;
    }

    if (config_.mode == MODE_HALL && !config_.hall_polarity_calibrated) {
        set_error(ERROR_HALL_NOT_CALIBRATED_YET);
        return false;
    }

    // We use shadow_count_ to do the calibration, but the offset is used by count_in_cpr_
    // Therefore we have to sync them for calibration
    shadow_count_ = count_in_cpr_;

    CRITICAL_SECTION() {
        // Reset state variables
        axis_->open_loop_controller_.Idq_setpoint_ = {0.0f, 0.0f};
        axis_->open_loop_controller_.Vdq_setpoint_ = {0.0f, 0.0f};
        axis_->open_loop_controller_.phase_ = 0.0f;
        axis_->open_loop_controller_.phase_vel_ = 0.0f;

        float max_current_ramp = axis_->motor_.config_.calibration_current / start_lock_duration * 2.0f;
        axis_->open_loop_controller_.max_current_ramp_ = max_current_ramp;
        axis_->open_loop_controller_.max_voltage_ramp_ = max_current_ramp;
        axis_->open_loop_controller_.max_phase_vel_ramp_ = INFINITY;
        axis_->open_loop_controller_.target_current_ = axis_->motor_.config_.motor_type != Motor::MOTOR_TYPE_GIMBAL ? axis_->motor_.config_.calibration_current : 0.0f;
        axis_->open_loop_controller_.target_voltage_ = axis_->motor_.config_.motor_type != Motor::MOTOR_TYPE_GIMBAL ? 0.0f : axis_->motor_.config_.calibration_current;
        axis_->open_loop_controller_.target_vel_ = 0.0f;
        axis_->open_loop_controller_.total_distance_ = 0.0f;
        axis_->open_loop_controller_.phase_ = axis_->open_loop_controller_.initial_phase_ = wrap_pm_pi(0 - config_.calib_scan_distance / 2.0f);

        axis_->motor_.current_control_.enable_current_control_src_ = (axis_->motor_.config_.motor_type != Motor::MOTOR_TYPE_GIMBAL);
        axis_->motor_.current_control_.Idq_setpoint_src_.connect_to(&axis_->open_loop_controller_.Idq_setpoint_);
        axis_->motor_.current_control_.Vdq_setpoint_src_.connect_to(&axis_->open_loop_controller_.Vdq_setpoint_);
        
        axis_->motor_.current_control_.phase_src_.connect_to(&axis_->open_loop_controller_.phase_);
        axis_->acim_estimator_.rotor_phase_src_.connect_to(&axis_->open_loop_controller_.phase_);

        axis_->motor_.phase_vel_src_.connect_to(&axis_->open_loop_controller_.phase_vel_);
        axis_->motor_.current_control_.phase_vel_src_.connect_to(&axis_->open_loop_controller_.phase_vel_);
        axis_->acim_estimator_.rotor_phase_vel_src_.connect_to(&axis_->open_loop_controller_.phase_vel_);
    }
    axis_->wait_for_control_iteration();

    axis_->motor_.arm(&axis_->motor_.current_control_);

    // go to start position of forward scan for start_lock_duration to get ready to scan
    for (size_t i = 0; i < (size_t)(start_lock_duration * 1000.0f); ++i) {
        if (!axis_->motor_.is_armed_) {
            return false; // TODO: return "disarmed" error code
        }
        if (axis_->requested_state_ != Axis::AXIS_STATE_UNDEFINED) {
            axis_->motor_.disarm();
            return false; // TODO: return "aborted" error code
        }
        osDelay(1);
    }


    int32_t init_enc_val = shadow_count_;
    uint32_t num_steps = 0;
    int64_t encvaluesum = 0;

    CRITICAL_SECTION() {
        axis_->open_loop_controller_.target_vel_ = config_.calib_scan_omega;
        axis_->open_loop_controller_.total_distance_ = 0.0f;
    }

    // scan forward
    while ((axis_->requested_state_ == Axis::AXIS_STATE_UNDEFINED) && axis_->motor_.is_armed_) {
        bool reached_target_dist = axis_->open_loop_controller_.total_distance_.any().value_or(-INFINITY) >= config_.calib_scan_distance;
        if (reached_target_dist) {
            break;
        }
        encvaluesum += shadow_count_;
        num_steps++;
        osDelay(1);
    }

    // Check response and direction
    if (shadow_count_ > init_enc_val + 8) {
        // motor same dir as encoder
        config_.direction = 1;
    } else if (shadow_count_ < init_enc_val - 8) {
        // motor opposite dir as encoder
        config_.direction = -1;
    } else {
        // Encoder response error
        set_error(ERROR_NO_RESPONSE);
        axis_->motor_.disarm();
        return false;
    }

    // Check CPR
    float elec_rad_per_enc = axis_->motor_.config_.pole_pairs * 2 * M_PI * (1.0f / (float)(config_.cpr));
    float expected_encoder_delta = config_.calib_scan_distance / elec_rad_per_enc;
    calib_scan_response_ = std::abs(shadow_count_ - init_enc_val);
    if (std::abs(calib_scan_response_ - expected_encoder_delta) / expected_encoder_delta > config_.calib_range) {
        set_error(ERROR_CPR_POLEPAIRS_MISMATCH);
        axis_->motor_.disarm();
        return false;
    }

    CRITICAL_SECTION() {
        axis_->open_loop_controller_.target_vel_ = -config_.calib_scan_omega;
    }

    // scan backwards
    while ((axis_->requested_state_ == Axis::AXIS_STATE_UNDEFINED) && axis_->motor_.is_armed_) {
        bool reached_target_dist = axis_->open_loop_controller_.total_distance_.any().value_or(INFINITY) <= 0.0f;
        if (reached_target_dist) {
            break;
        }
        encvaluesum += shadow_count_;
        num_steps++;
        osDelay(1);
    }

    // Motor disarmed because of an error
    if (!axis_->motor_.is_armed_) {
        return false;
    }

    axis_->motor_.disarm();

    config_.phase_offset = encvaluesum / num_steps;
    int32_t residual = encvaluesum - ((int64_t)config_.phase_offset * (int64_t)num_steps);
    config_.phase_offset_float = (float)residual / (float)num_steps + 0.5f;  // add 0.5 to center-align state to phase

    is_ready_ = true;
    return true;
}

static bool decode_hall(uint8_t hall_state, int32_t* hall_cnt) {
    switch (hall_state) {
        case 0b001: *hall_cnt = 0; return true;
        case 0b011: *hall_cnt = 1; return true;
        case 0b010: *hall_cnt = 2; return true;
        case 0b110: *hall_cnt = 3; return true;
        case 0b100: *hall_cnt = 4; return true;
        case 0b101: *hall_cnt = 5; return true;
        default: return false;
    }
}

void Encoder::sample_now() {
    switch (mode_) {
        case MODE_INCREMENTAL: {
            tim_cnt_sample_ = (int16_t)timer_->Instance->CNT;
        } break;

        case MODE_HALL: {
            // do nothing: samples already captured in general GPIO capture
        } break;

        case MODE_SINCOS: {
            sincos_sample_s_ = get_adc_relative_voltage(get_gpio(config_.sincos_gpio_pin_sin)) - 0.5f;
            sincos_sample_c_ = get_adc_relative_voltage(get_gpio(config_.sincos_gpio_pin_cos)) - 0.5f;
        } break;

        case MODE_SPI_ABS_AMS: {
            // Interleave a cada 32 transações (250 Hz cycle @ 8 kHz):
            //   ph=0: send 0x4001 (read ERRFL) → AS5047 limpa o ERRFL ao receber
            //   ph=1: send 0xFFFC (read DIAAGC); RX é ERRFL data (descarta)
            //   ph=2: send 0xFFFF (read ANGLECOM); RX é DIAAGC ← captura aqui
            //   ph=3..31: send 0xFFFF normal; RX é ANGLECOM
            // Net: ERRFL limpa a 250 Hz (impede EF sticky), DIAAGC capturado a 250 Hz,
            // pos_abs perde só 2/32 = 6.25% das amostras (ainda imperceptível pro FFB).
            uint16_t ph = abs_spi_cmd_phase_ % 256;
            if      (ph == 0) abs_spi_dma_tx_[0] = 0x4001;  // ERRFL read (clears)
            else if (ph == 1) abs_spi_dma_tx_[0] = 0xFFFC;  // DIAAGC read
            else              abs_spi_dma_tx_[0] = 0xFFFF;  // ANGLECOM read
            abs_spi_start_transaction();
        } break;
        case MODE_SPI_ABS_CUI:
        case MODE_SPI_ABS_AEAT:
        case MODE_SPI_ABS_RLS:
        case MODE_SPI_ABS_MA732:
        {
            abs_spi_start_transaction();
            // Do nothing
        } break;
        case MODE_SPI_ABS_MT6835: {
            // Option A: leitura SPI do MT6835 roda na thread dedicada
            // (enc_spi_thread, odrive_bridge.cpp), FORA deste ISR prio-0, pra
            // não estrangular USB/FFB. Aqui NÃO tocamos no SPI; marcamos
            // pos_updated=true pra o update() usar o pos_abs_ em cache sem
            // contar COM fail. Sincronização via semáforo kickado no update()
            // (8 kHz, atraso constante ≈ 1 período). CRC fails ficam nos
            // contadores da thread (sys.encraw!). PLL interpola a 8 kHz.
            abs_spi_pos_updated_ = true;
        } break;

        default: {
           set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
        } break;
    }

    // Sample all GPIO digital input data registers, used for HALL sensors for example.
    for (size_t i = 0; i < sizeof(ports_to_sample) / sizeof(ports_to_sample[0]); ++i) {
        port_samples_[i] = ports_to_sample[i]->IDR;
    }
}

bool Encoder::read_sampled_gpio(Stm32Gpio gpio) {
    for (size_t i = 0; i < sizeof(ports_to_sample) / sizeof(ports_to_sample[0]); ++i) {
        if (ports_to_sample[i] == gpio.port_) {
            return port_samples_[i] & gpio.pin_mask_;
        }
    }
    return false;
}

void Encoder::decode_hall_samples() {
    hall_state_ = (read_sampled_gpio(hallA_gpio_) ? 1 : 0)
                | (read_sampled_gpio(hallB_gpio_) ? 2 : 0)
                | (read_sampled_gpio(hallC_gpio_) ? 4 : 0);
}

bool Encoder::abs_spi_start_transaction() {
    if (mode_ & MODE_FLAG_ABS){
        // MKS XDrive Mini fix: usa transfer() POLLED em vez de transfer_async(DMA).
        // Stm32SpiArbiter documenta que DMA async travava nesse hardware (ISR
        // do DMA não disparava — conflito provavel com USB OTG ou prioridades
        // IRQ). DRV8301 já foi migrado pra polled; encoder estava esquecido.
        // length=1 = 1× transação de 16 bits (DataSize=16BIT no spi_task_.config).
        // Em /16 baud (~2.6 MHz) cada transação leva ~6 µs — barato no ISR de 8 kHz.
        // MT6835 (burst read 21 bits) precisa de 3 words: 1 comando + 2 words de
        // dados (4 bytes). CS fica baixo a transação inteira (NSS soft), então o
        // chip emite 0x003..0x006 continuamente conforme o protocolo burst.
        size_t spi_len = (mode_ == MODE_SPI_ABS_MT6835) ? 3 : 1;
        bool ok = spi_arbiter_->transfer(spi_task_.config, abs_spi_cs_gpio_,
                                         (uint8_t*)abs_spi_dma_tx_,
                                         (uint8_t*)abs_spi_dma_rx_,
                                         spi_len, /*timeout_ms=*/2);
        abs_spi_cb(ok);
    }
    return true;
}

uint8_t ams_parity(uint16_t v) {
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}

uint8_t cui_parity(uint16_t v) {
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    return ~v & 3;
}

// MT6835 CRC8 (datasheet 7.6.8): polinômio X^8+X^2+X+1 (0x07), aplicado sobre os
// 24 bits ANGLE[20:0]+STATUS[2:0] = bytes dos regs 0x003,0x004,0x005, com o MSB
// (ANGLE[20]) entrando primeiro. Init=0x00, sem reflexão, sem XOR final
// (CRC-8/SMBUS). Retorna o CRC esperado pra comparar com o reg 0x006.
uint8_t mt6835_crc8(uint8_t b0, uint8_t b1, uint8_t b2) {
    const uint8_t data[3] = {b0, b1, b2};
    uint8_t crc = 0x00;
    for (int i = 0; i < 3; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x80)
                crc = (uint8_t)((crc << 1) ^ 0x07);
            else
                crc = (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// ============================ MT6835 register access =========================
// Transação genérica de 24 bits (datasheet 7.6.3): [cmd C3~C0 | addr A11~A0 |
// data 8b]. O DataSize normal é 16-bit (frames do burst read); aqui trocamos
// pra 8-bit (3 bytes) só nesta transação — o arbiter reescreve CR1 quando a
// config difere, e volta na próxima leitura de ângulo.
// Comandos: 0x3=read, 0x6=write, 0x5=auto-set-zero, 0xC=program EEPROM.
// Pros dois últimos o byte de resposta é o ack (0x55 = OK).
//
// Retry: transfer() polled retorna false se o barramento está tomado. Com a
// thread lendo ângulo a 8 kHz (~6% duty), colisão é provável. Retenta com
// osDelay(1); chamável só de contexto de thread (setup, command handlers).
bool Encoder::mt6835_transfer24(uint8_t cmd, uint16_t addr, uint8_t data_in, uint8_t* data_out) {
    if (mode_ != MODE_SPI_ABS_MT6835)
        return false;

    SPI_InitTypeDef config = spi_task_.config;
    config.DataSize = SPI_DATASIZE_8BIT;

    uint8_t tx[3] = {
        (uint8_t)((cmd << 4) | ((addr >> 8) & 0x0F)),
        (uint8_t)(addr & 0xFF),
        data_in,
    };
    uint8_t rx[3] = {0, 0, 0};

    bool ok = false;
    for (int attempt = 0; attempt < 10 && !ok; ++attempt) {
        if (attempt > 0)
            osDelay(1);
        ok = spi_arbiter_->transfer(config, abs_spi_cs_gpio_, tx, rx, 3, /*timeout_ms=*/5);
    }
    if (ok && data_out)
        *data_out = rx[2];
    return ok;
}

bool Encoder::mt6835_read_reg(uint16_t addr, uint8_t* val) {
    return mt6835_transfer24(0x3, addr, 0x00, val);
}

bool Encoder::mt6835_write_reg(uint16_t addr, uint8_t val) {
    return mt6835_transfer24(0x6, addr, val, nullptr);
}

// Auto Setting Zero-Position (datasheet 7.6.7): grava a posição atual no
// registro ZERO_POS[11:0] (volátil até um Program EEPROM). Ack 0x55 = sucesso.
bool Encoder::mt6835_auto_set_zero() {
    uint8_t ack = 0;
    return mt6835_transfer24(0x5, 0x000, 0x00, &ack) && ack == 0x55;
}

// Program EEPROM (datasheet 7.6.6): persiste TODO o register map. Ack 0x55 =
// aceito. ATENÇÃO: aguardar >= 6 s antes de desligar, sem nenhuma outra
// operação SPI. Caller garante motor desarmado e avisa o usuário do delay.
bool Encoder::mt6835_program_eeprom() {
    uint8_t ack = 0;
    return mt6835_transfer24(0xC, 0x000, 0x00, &ack) && ack == 0x55;
}

void Encoder::abs_spi_cb(bool success) {
    uint32_t pos;  // 14 bits (AMS/CUI/...) ou 21 bits (MT6835)

    if (!success) {
        abs_spi_fail_xfer_++;
        goto done;
    }
    // MKS XDrive Mini debug: registra último raw recebido pra inspeção via
    // sys.encraw! (mesmo que a validação falhe abaixo, queremos ver o byte cru).
    abs_spi_last_rx_ = abs_spi_dma_rx_[0];

    switch (mode_) {
        case MODE_SPI_ABS_AMS: {
            uint16_t rawVal = abs_spi_dma_rx_[0];
            // check if parity is correct (even) and error flag clear
            if (ams_parity(rawVal)) {
                abs_spi_fail_parity_++;
                abs_spi_cmd_phase_++;
                goto done;
            }
            // MKS XDrive Mini: NÃO rejeitar mais por EF=1. Investigação empírica
            // (com scope mostrando sinal MISO 0→3V3 limpo em /16, /64, /128, com
            // ou sem pull-up externo) mostrou que EF=1 vinha COM dados de ângulo
            // perfeitos — parity sempre 0, valores consistentes com magneto.
            //
            // EF é flag housekeeping do AS5047: "ERRFL tem algum bit". Em algumas
            // condições/quirks do chip ela permanece set mesmo após ERRFL read,
            // especialmente quando enviamos reads não-ANGLECOM (DIAAGC, ERRFL).
            // PARITY é o juiz da integridade da transação — passou parity,
            // dado é íntegro. Conta EF separadamente como info, não erro.
            if ((rawVal >> 14) & 1) {
                abs_spi_fail_ef_++;  // counter informacional, NÃO descarta dado
            }
            // Interleave decoding (offset por 1 tick — AS5047 protocol):
            //   ph_now=1: resposta ao ERRFL cmd (phase 0) → discard
            //   ph_now=2: resposta ao DIAAGC cmd (phase 1) → capture
            //   outros:   resposta a ANGLECOM cmd → use como angle
            uint16_t ph_now = abs_spi_cmd_phase_ % 256;
            abs_spi_cmd_phase_++;
            if (ph_now == 1) {
                // Resposta é ERRFL data — descarta (importante é o clear no chip)
                // Marca pos_updated=true: skip intencional, NÃO é erro. Sem isso
                // o spi_error_rate sobe artificialmente em 0.78% e contribui pro
                // ERROR_ABS_SPI_COM_FAIL ficar voltando após Clear.
                abs_spi_pos_updated_ = true;
                goto done;
            }
            if (ph_now == 2) {
                // Resposta é DIAAGC data — captura
                diaagc_raw_ = rawVal & 0x3FFF;
                diaagc_update_count_++;
                abs_spi_pos_updated_ = true;  // skip intencional
                goto done;
            }
            pos = rawVal & 0x3fff;
            abs_spi_ok_count_++;
        } break;

        case MODE_SPI_ABS_CUI: {
            uint16_t rawVal = abs_spi_dma_rx_[0];
            // check if parity is correct
            if (cui_parity(rawVal)) {
                goto done;
            }
            pos = rawVal & 0x3fff;
        } break;

        case MODE_SPI_ABS_RLS: {
            uint16_t rawVal = abs_spi_dma_rx_[0];
            pos = (rawVal >> 2) & 0x3fff;
        } break;

        case MODE_SPI_ABS_MA732: {
            uint16_t rawVal = abs_spi_dma_rx_[0];
            pos = (rawVal >> 2) & 0x3fff;
        } break;

        case MODE_SPI_ABS_MT6835: {
            // RX[0] = resposta ao comando (MISO Hi-Z) → descarta.
            // RX[1] = [reg0x003][reg0x004], RX[2] = [reg0x005][reg0x006].
            uint8_t b003 = abs_spi_dma_rx_[1] >> 8;          // ANGLE[20:13]
            uint8_t b004 = abs_spi_dma_rx_[1] & 0xFF;        // ANGLE[12:5]
            uint8_t b005 = abs_spi_dma_rx_[2] >> 8;          // ANGLE[4:0]<<3 | STATUS[2:0]
            uint8_t crc  = abs_spi_dma_rx_[2] & 0xFF;        // CRC[7:0]

            // Diag inofensivo: expõe o 1º word de dados ([reg0x003][reg0x004]) em
            // `last` pra sys.encraw! (em vez da phase comando, que é Hi-Z).
            abs_spi_last_rx_ = abs_spi_dma_rx_[1];

            // CRC8 é o juiz da integridade (não há paridade nessa parte). Se falhar,
            // REJEITA a amostra (não atualiza pos_abs_): posição-lixo NÃO chega no
            // FOC → motor não trava. O spi_error_rate em update() sobe e dispara
            // ERROR_ABS_SPI_COM_FAIL com histerese se for persistente.
            // abs_spi_fail_parity_ = contador de CRC fail (visível em sys.encraw!).
            if (mt6835_crc8(b003, b004, b005) != crc) {
                abs_spi_fail_parity_++;
                goto done;
            }

            pos = ((uint32_t)b003 << 13) | ((uint32_t)b004 << 5) | (b005 >> 3);
            mt6835_status_ = b005 & 0x07;  // overspeed / weak-field / undervoltage warnings
            abs_spi_ok_count_++;
        } break;

        default: {
           set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
           goto done;
        } break;
    }

    pos_abs_ = pos;
    abs_spi_pos_updated_ = true;
    if (config_.pre_calibrated) {
        is_ready_ = true;
    }

done:
    Stm32SpiArbiter::release_task(&spi_task_);
}

void Encoder::abs_spi_cs_pin_init(){
    // Decode and init cs pin
#if HW_VERSION_MAJOR == 4
    if (mode_ == MODE_SPI_ABS_MA732)
        abs_spi_cs_gpio_ = {GPIOA, GPIO_PIN_15};
    else
#else
    abs_spi_cs_gpio_ = get_gpio(config_.abs_spi_cs_gpio_pin);
#endif
    abs_spi_cs_gpio_.config(GPIO_MODE_OUTPUT_PP, GPIO_PULLUP);

    // Write pin high
    abs_spi_cs_gpio_.write(true);
}

// Note that this may return counts +1 or -1 without any wrapping
int32_t Encoder::hall_model(float internal_pos) {
    int32_t base_cnt = (int32_t)std::floor(internal_pos);

    float pos_in_range = fmodf_pos(internal_pos, 6.0f);
    int pos_idx = (int)pos_in_range;
    if (pos_idx == 6) pos_idx = 5; // in case of rounding error
    int next_i = (pos_idx == 5) ? 0 : pos_idx+1;

    float below_edge = config_.hall_edge_phcnt[pos_idx];
    float above_edge = config_.hall_edge_phcnt[next_i];

    // if we are blow the "below" edge, we are the count under
    if (wrap_pm(pos_in_range - below_edge, 6.0f) < 0.0f)
        return base_cnt - 1;
    // if we are above the "above" edge, we are the count over
    else if (wrap_pm(pos_in_range - above_edge, 6.0f) > 0.0f)
        return base_cnt + 1;
    // otherwise we are in the nominal count (or completely lost)
    return base_cnt;
}

bool Encoder::update() {
    // update internal encoder state.
    int32_t delta_enc = 0;
    int32_t pos_abs_latched = pos_abs_; //LATCH

    switch (mode_) {
        case MODE_INCREMENTAL: {
            //TODO: use count_in_cpr_ instead as shadow_count_ can overflow
            //or use 64 bit
            int16_t delta_enc_16 = (int16_t)tim_cnt_sample_ - (int16_t)shadow_count_;
            delta_enc = (int32_t)delta_enc_16; //sign extend
        } break;

        case MODE_HALL: {
            decode_hall_samples();
            if (sample_hall_states_) {
                states_seen_count_[hall_state_]++;
            }
            if (config_.hall_polarity_calibrated) {
                int32_t hall_cnt;
                if (decode_hall((hall_state_ ^ config_.hall_polarity), &hall_cnt)) {
                    if (calibrate_hall_phase_) {
                        if (sample_hall_phase_ && last_hall_cnt_.has_value()) {
                            int mod_hall_cnt = mod(hall_cnt - last_hall_cnt_.value(), 6);
                            size_t edge_idx;
                            if (mod_hall_cnt == 0) { goto skip; } // no count - do nothing
                            else if (mod_hall_cnt == 1) { // counted up
                                edge_idx = hall_cnt;
                            } else if (mod_hall_cnt == 5) { // counted down
                                edge_idx = last_hall_cnt_.value();
                            } else {
                                set_error(ERROR_ILLEGAL_HALL_STATE);
                                return false;
                            }

                            auto maybe_phase = axis_->open_loop_controller_.phase_.any();
                            if (maybe_phase) {
                                float phase = maybe_phase.value();
                                // Early increment to get the right divisor in recursive average
                                hall_phase_calib_seen_count_[edge_idx]++;
                                float& edge_phase = config_.hall_edge_phcnt[edge_idx];
                                if (hall_phase_calib_seen_count_[edge_idx] == 1)
                                    edge_phase = phase;
                                else {
                                    // circularly wrapped recursive average
                                    edge_phase += (phase - edge_phase) / hall_phase_calib_seen_count_[edge_idx];
                                    edge_phase = wrap_pm_pi(edge_phase);
                                }
                            }
                        }
                    skip:
                        last_hall_cnt_ = hall_cnt;

                        return true; // Skip all velocity and phase estimation
                    }

                    delta_enc = hall_cnt - count_in_cpr_;
                    delta_enc = mod(delta_enc, 6);
                    if (delta_enc > 3)
                        delta_enc -= 6;
                } else {
                    if (!config_.ignore_illegal_hall_state) {
                        set_error(ERROR_ILLEGAL_HALL_STATE);
                        return false;
                    }
                }
            }
        } break;

        case MODE_SINCOS: {
            float phase = fast_atan2(sincos_sample_s_, sincos_sample_c_);
            int fake_count = (int)(1000.0f * phase);
            //CPR = 6283 = 2pi * 1k

            delta_enc = fake_count - count_in_cpr_;
            delta_enc = mod(delta_enc, 6283);
            if (delta_enc > 6283/2)
                delta_enc -= 6283;
        } break;
        
        case MODE_SPI_ABS_RLS:
        case MODE_SPI_ABS_AMS:
        case MODE_SPI_ABS_CUI:
        case MODE_SPI_ABS_AEAT:
        case MODE_SPI_ABS_MA732:
        case MODE_SPI_ABS_MT6835: {
            if (abs_spi_pos_updated_ == false) {
                // Low pass filter the error
                spi_error_rate_ += current_meas_period * (1.0f - spi_error_rate_);
                if (spi_error_rate_ > 0.05f) {
                    set_error(ERROR_ABS_SPI_COM_FAIL);
                    return false;
                }
            } else {
                // Low pass filter the error
                spi_error_rate_ += current_meas_period * (0.0f - spi_error_rate_);
                // MKS XDrive Mini: auto-recovery do ERROR_ABS_SPI_COM_FAIL com
                // histerese. ODrive vanilla seta o erro mas NUNCA limpa — fica
                // grudado pra sempre mesmo com SPI saudável depois.
                // Trigger no upstream em 5%, nosso auto-clear em 1% — boa
                // margem de segurança. Agora que EF não conta mais como falha
                // (parity é o único critério real), o rate de equilíbrio deve
                // ficar bem perto de 0%, então auto-clear em 1% é confortável.
                if (spi_error_rate_ < 0.01f && (error_ & ERROR_ABS_SPI_COM_FAIL)) {
                    error_ = (Error)(error_ & ~ERROR_ABS_SPI_COM_FAIL);
                }
            }

            abs_spi_pos_updated_ = false;
            delta_enc = pos_abs_latched - count_in_cpr_; //LATCH
            delta_enc = mod(delta_enc, config_.cpr);
            if (delta_enc > config_.cpr/2) {
                delta_enc -= config_.cpr;
            }

            // Option A (MT6835): pos_abs_ já foi consumido acima — kicka a
            // thread pra ler a próxima amostra agora, assim ela chega fresca
            // pro próximo ciclo (atraso constante ≈ 1 período de 125 µs).
            if (mode_ == MODE_SPI_ABS_MT6835) {
                odrive_bridge_enc_spi_kick();
            }

        }break;
        default: {
            set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
            return false;
        } break;
    }

    shadow_count_ += delta_enc;
    count_in_cpr_ += delta_enc;
    count_in_cpr_ = mod(count_in_cpr_, config_.cpr);

    if(mode_ & MODE_FLAG_ABS)
        count_in_cpr_ = pos_abs_latched;

    // Memory for pos_circular
    float pos_cpr_counts_last = pos_cpr_counts_;

    //// run pll (for now pll is in units of encoder counts)
    // Predict current pos
    pos_estimate_counts_ += current_meas_period * vel_estimate_counts_;
    pos_cpr_counts_      += current_meas_period * vel_estimate_counts_;
    // Encoder model
    auto encoder_model = [this](float internal_pos)->int32_t {
        if (config_.mode == MODE_HALL)
            return hall_model(internal_pos);
        else
            return (int32_t)std::floor(internal_pos);
    };
    // discrete phase detector
    float delta_pos_counts = (float)(shadow_count_ - encoder_model(pos_estimate_counts_));
    float delta_pos_cpr_counts = (float)(count_in_cpr_ - encoder_model(pos_cpr_counts_));
    delta_pos_cpr_counts = wrap_pm(delta_pos_cpr_counts, (float)(config_.cpr));
    delta_pos_cpr_counts_ += 0.1f * (delta_pos_cpr_counts - delta_pos_cpr_counts_); // for debug
    // pll feedback
    pos_estimate_counts_ += current_meas_period * pll_kp_ * delta_pos_counts;
    pos_cpr_counts_ += current_meas_period * pll_kp_ * delta_pos_cpr_counts;
    pos_cpr_counts_ = fmodf_pos(pos_cpr_counts_, (float)(config_.cpr));
    vel_estimate_counts_ += current_meas_period * pll_ki_ * delta_pos_cpr_counts;
    bool snap_to_zero_vel = false;
    if (std::abs(vel_estimate_counts_) < 0.5f * current_meas_period * pll_ki_) {
        vel_estimate_counts_ = 0.0f;  //align delta-sigma on zero to prevent jitter
        snap_to_zero_vel = true;
    }

    // Outputs from Encoder for Controller
    pos_estimate_ = pos_estimate_counts_ / (float)config_.cpr;
    vel_estimate_ = vel_estimate_counts_ / (float)config_.cpr;
    
    // TODO: we should strictly require that this value is from the previous iteration
    // to avoid spinout scenarios. However that requires a proper way to reset
    // the encoder from error states.
    float pos_circular = pos_circular_.any().value_or(0.0f);
    pos_circular +=  wrap_pm((pos_cpr_counts_ - pos_cpr_counts_last) / (float)config_.cpr, 1.0f);
    pos_circular = fmodf_pos(pos_circular, axis_->controller_.config_.circular_setpoint_range);
    pos_circular_ = pos_circular;

    //// run encoder count interpolation
    int32_t corrected_enc = count_in_cpr_ - config_.phase_offset;
    // if we are stopped, make sure we don't randomly drift
    if (snap_to_zero_vel || !config_.enable_phase_interpolation) {
        interpolation_ = 0.5f;
    // reset interpolation if encoder edge comes
    // TODO: This isn't correct. At high velocities the first phase in this count may very well not be at the edge.
    } else if (delta_enc > 0) {
        interpolation_ = 0.0f;
    } else if (delta_enc < 0) {
        interpolation_ = 1.0f;
    } else {
        // Interpolate (predict) between encoder counts using vel_estimate,
        interpolation_ += current_meas_period * vel_estimate_counts_;
        // don't allow interpolation indicated position outside of [enc, enc+1)
        if (interpolation_ > 1.0f) interpolation_ = 1.0f;
        if (interpolation_ < 0.0f) interpolation_ = 0.0f;
    }
    float interpolated_enc = corrected_enc + interpolation_;

    //// compute electrical phase
    //TODO avoid recomputing elec_rad_per_enc every time
    float elec_rad_per_enc = axis_->motor_.config_.pole_pairs * 2 * M_PI * (1.0f / (float)(config_.cpr));
    float ph = elec_rad_per_enc * (interpolated_enc - config_.phase_offset_float);
    
    if (is_ready_) {
        phase_ = wrap_pm_pi(ph) * config_.direction;
        phase_vel_ = (2*M_PI) * *vel_estimate_.present() * axis_->motor_.config_.pole_pairs * config_.direction;
    }

    return true;
}
