#ifndef __ENCODER_HPP
#define __ENCODER_HPP

class Encoder;

#include <board.h> // needed for arm_math.h
#include <Drivers/STM32/stm32_spi_arbiter.hpp>
#include "utils.hpp"
#include <autogen/interfaces.hpp>
#include "component.hpp"


class Encoder : public ODriveIntf::EncoderIntf {
public:
    static constexpr uint32_t MODE_FLAG_ABS = 0x100;
    static constexpr std::array<float, 6> hall_edge_defaults = 
        {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f};

    struct Config_t {
        // MKS Mini + user's encoder working defaults (ABZ incremental, 10000 CPR, reversed).
        // Values from user's post-offset-calibration config dump.
        Mode mode = MODE_INCREMENTAL;
        float calib_range = 0.02f; // Accuracy required to pass encoder cpr check
        float calib_scan_distance = 16.0f * M_PI; // rad electrical
        float calib_scan_omega = 4.0f * M_PI; // rad/s electrical
        float bandwidth = 200.0f;
        int32_t phase_offset = -10059;              // user's measured offset
        float phase_offset_float = 0.174459f;       // user's measured fractional offset
        int32_t cpr = 10000;                        // user's ABZ encoder (stock default was 8192)
        float index_offset = 0.0f;
        bool use_index = false;
        bool pre_calibrated = false; // If true, this means the offset stored in
                                    // configuration is valid and does not need
                                    // be determined by run_offset_calibration.
                                    // In this case the encoder will enter ready
                                    // state as soon as the index is found.
        int32_t direction = -1;                     // user's config (encoder wiring reversed relative to motor)
        bool use_index_offset = true;
        bool enable_phase_interpolation = true; // Use velocity to interpolate inside the count state
        bool find_idx_on_lockin_only = false; // Only be sensitive during lockin scan constant vel state
        bool ignore_illegal_hall_state = false; // dont error on bad states like 000 or 111
        uint8_t hall_polarity = 0;
        bool hall_polarity_calibrated = false;
        std::array<float, 6> hall_edge_phcnt = hall_edge_defaults;
        uint16_t abs_spi_cs_gpio_pin = 1;
        uint16_t sincos_gpio_pin_sin = 3;
        uint16_t sincos_gpio_pin_cos = 4;


        // custom setters
        Encoder* parent = nullptr;
        void set_use_index(bool value) { use_index = value; parent->set_idx_subscribe(); }
        void set_find_idx_on_lockin_only(bool value) { find_idx_on_lockin_only = value; parent->set_idx_subscribe(); }
        void set_abs_spi_cs_gpio_pin(uint16_t value) { abs_spi_cs_gpio_pin = value; parent->abs_spi_cs_pin_init(); }
        void set_pre_calibrated(bool value) { pre_calibrated = value; parent->check_pre_calibrated(); }
        void set_bandwidth(float value) { bandwidth = value; parent->update_pll_gains(); }
    };

    Encoder(TIM_HandleTypeDef* timer, Stm32Gpio index_gpio,
            Stm32Gpio hallA_gpio, Stm32Gpio hallB_gpio, Stm32Gpio hallC_gpio,
            Stm32SpiArbiter* spi_arbiter);
    
    bool apply_config(ODriveIntf::MotorIntf::MotorType motor_type);
    void setup();
    void set_error(Error error);
    bool do_checks();

    void enc_index_cb();
    void set_idx_subscribe(bool override_enable = false);
    void update_pll_gains();
    void check_pre_calibrated();

    void set_linear_count(int32_t count);
    void set_circular_count(int32_t count, bool update_offset);
    bool calib_enc_offset(float voltage_magnitude);

    bool run_index_search();
    bool run_direction_find();
    bool run_hall_polarity_calibration();
    bool run_hall_phase_calibration();
    bool run_offset_calibration();
    void sample_now();
    bool read_sampled_gpio(Stm32Gpio gpio);
    void decode_hall_samples();
    int32_t hall_model(float internal_pos);
    bool update();

    TIM_HandleTypeDef* timer_;
    Stm32Gpio index_gpio_;
    Stm32Gpio hallA_gpio_;
    Stm32Gpio hallB_gpio_;
    Stm32Gpio hallC_gpio_;
    Stm32SpiArbiter* spi_arbiter_;
    Axis* axis_ = nullptr; // set by Axis constructor

    Config_t config_;

    Error error_ = ERROR_NONE;
    bool index_found_ = false;
    bool is_ready_ = false;
    int32_t shadow_count_ = 0;
    int32_t count_in_cpr_ = 0;
    float interpolation_ = 0.0f;
    OutputPort<float> phase_ = 0.0f;     // [rad]
    OutputPort<float> phase_vel_ = 0.0f; // [rad/s]
    float pos_estimate_counts_ = 0.0f;  // [count]
    float pos_cpr_counts_ = 0.0f;  // [count]
    float delta_pos_cpr_counts_ = 0.0f;  // [count] phase detector result for debug
    float vel_estimate_counts_ = 0.0f;  // [count/s]
    float pll_kp_ = 0.0f;   // [count/s / count]
    float pll_ki_ = 0.0f;   // [(count/s^2) / count]
    float calib_scan_response_ = 0.0f; // debug report from offset calib
    int32_t pos_abs_ = 0;
    float spi_error_rate_ = 0.0f;

    OutputPort<float> pos_estimate_ = 0.0f; // [turn]
    OutputPort<float> vel_estimate_ = 0.0f; // [turn/s]
    OutputPort<float> pos_circular_ = 0.0f; // [turn]

    bool pos_estimate_valid_ = false;
    bool vel_estimate_valid_ = false;

    int16_t tim_cnt_sample_ = 0; // 
    static const constexpr GPIO_TypeDef* ports_to_sample[] = { GPIOA, GPIOB, GPIOC };
    uint16_t port_samples_[sizeof(ports_to_sample) / sizeof(ports_to_sample[0])];
    // Updated by low_level pwm_adc_cb
    uint8_t hall_state_ = 0x0; // bit[0] = HallA, .., bit[2] = HallC
    std::optional<uint8_t> last_hall_cnt_ = std::nullopt; // Used to find hall edges for calibration
    bool calibrate_hall_phase_ = false;
    bool sample_hall_states_ = false;
    bool sample_hall_phase_ = false;
    std::array<int, 8> states_seen_count_; // for hall polarity calibration
    std::array<int, 6> hall_phase_calib_seen_count_;

    float sincos_sample_s_ = 0.0f;
    float sincos_sample_c_ = 0.0f;

    bool abs_spi_start_transaction();
    void abs_spi_cb(bool success);
    void abs_spi_cs_pin_init();
    bool abs_spi_pos_updated_ = false;
    Mode mode_ = MODE_INCREMENTAL;
    Stm32Gpio abs_spi_cs_gpio_;
    uint32_t abs_spi_cr1;
    uint32_t abs_spi_cr2;
    // 3 words: AS5047/MA732/etc. use only [0] (length=1). MT6835 burst read uses
    // all 3 (length=3): [0]=command 0xA003, [1]/[2]=clock-out words for the
    // 4 data bytes (regs 0x003..0x006). See abs_spi_cb()/sample_now().
    uint16_t abs_spi_dma_tx_[3] = {0xFFFF, 0x0000, 0x0000};
    uint16_t abs_spi_dma_rx_[3];
    Stm32SpiArbiter::SpiTask spi_task_;

    // MKS XDrive Mini debug — contadores expostos pra command sys.encraw!
    // (já estamos numa seção public: aberta no topo da classe)
    uint32_t abs_spi_ok_count_     = 0;  // # de transações com parity+EF OK
    uint32_t abs_spi_fail_parity_  = 0;  // # de rejeitadas por paridade ruim
    uint32_t abs_spi_fail_ef_      = 0;  // # de rejeitadas com EF=1
    uint32_t abs_spi_fail_xfer_    = 0;  // # de transfer() retornando false
    uint16_t abs_spi_last_rx_      = 0;  // último raw recebido (qualquer status)

    // AS5047 DIAAGC register (0x3FFC) snapshot pra sys.magnet!
    // Atualizado a cada 256 transações (~31 Hz @ 8 kHz update). Bits do raw:
    //   [7:0]  AGC value (0-255, ideal ≈ 128)
    //   [8]    LF — Loop Finished (offset compensation done)
    //   [9]    COF — CORDIC Overflow
    //   [10]   MAGH — Magnetic field too strong (magnet too close)
    //   [11]   MAGL — Magnetic field too weak (magnet too far)
    uint16_t diaagc_raw_           = 0;  // valor cru da última leitura DIAAGC
    uint32_t diaagc_update_count_  = 0;  // # de leituras DIAAGC bem-sucedidas
    uint16_t abs_spi_cmd_phase_    = 0;  // contador interno pra interleaving

    // MT6835 (21-bit) — snapshot do STATUS[2:0] da última leitura de ângulo válida.
    //   bit[0] = Rotation Over Speed Warning
    //   bit[1] = Weak Magnetic Field Warning (magneto longe/fraco)
    //   bit[2] = Under Voltage Warning
    // Exposto pra inspeção via sys.encraw! / debug; warnings não bloqueiam o FFB.
    uint8_t  mt6835_status_        = 0;

    // MT6835 — acesso a registro único (trama 24 bits, DataSize 8-bit).
    // Chamável só de contexto de thread (setup, command handlers) — tem retry
    // com osDelay. Ver implementação em encoder.cpp.
    bool mt6835_transfer24(uint8_t cmd, uint16_t addr, uint8_t data_in, uint8_t* data_out);
    bool mt6835_read_reg(uint16_t addr, uint8_t* val);
    bool mt6835_write_reg(uint16_t addr, uint8_t val);
    bool mt6835_auto_set_zero();      // ZERO_POS ← posição atual (volátil)
    bool mt6835_program_eeprom();     // persiste register map (aguardar 6 s!)

    // Diag do boot (setup()): comm verificada por CRC + HYST zerado com sucesso.
    bool mt6835_boot_comm_ok_  = false;
    bool mt6835_hyst_zeroed_   = false;

    // Pausa das leituras de ângulo da thread (ticks osKernelSysTick). Usado
    // pelo program EEPROM: datasheet 7.6.6 proíbe qualquer operação SPI por
    // >= 6 s após o comando. A thread pula leituras enquanto now < este tick.
    volatile uint32_t mt6835_spi_pause_until_tick_ = 0;

    constexpr float getCoggingRatio(){
        return 1.0f / 3600.0f;
    }

};

#endif // __ENCODER_HPP
