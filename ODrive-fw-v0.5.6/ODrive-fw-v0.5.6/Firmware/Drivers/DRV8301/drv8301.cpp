
#include "drv8301.hpp"
#include "utils.hpp"
#include "cmsis_os.h"
#include "board.h"

const SPI_InitTypeDef Drv8301::spi_config_ = {
    .Mode = SPI_MODE_MASTER,
    .Direction = SPI_DIRECTION_2LINES,
    .DataSize = SPI_DATASIZE_16BIT,
    .CLKPolarity = SPI_POLARITY_LOW,
    .CLKPhase = SPI_PHASE_2EDGE,
    .NSS = SPI_NSS_SOFT,
    .BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16,
    .FirstBit = SPI_FIRSTBIT_MSB,
    .TIMode = SPI_TIMODE_DISABLE,
    .CRCCalculation = SPI_CRCCALCULATION_DISABLE,
    .CRCPolynomial = 10,
};

bool Drv8301::config(float requested_gain, float* actual_gain) {
    // Calculate gain setting: Snap down to have equal or larger range as
    // requested or largest possible range otherwise

    // for reference:
    // 20V/V on 500uOhm gives a range of +/- 150A
    // 40V/V on 500uOhm gives a range of +/- 75A
    // 20V/V on 666uOhm gives a range of +/- 110A
    // 40V/V on 666uOhm gives a range of +/- 55A

    uint16_t gain_setting = 3;
    float gain_choices[] = {10.0f, 20.0f, 40.0f, 80.0f};
    while (gain_setting && (gain_choices[gain_setting] > requested_gain)) {
        gain_setting--;
    }

    if (actual_gain) {
        *actual_gain = gain_choices[gain_setting];
    }

    RegisterFile new_config;

    new_config.control_register_1 =
          (21 << 6) // Overcurrent set to approximately 150A at 100degC. This may need tweaking.
        | (0b01 << 4) // OCP_MODE: latch shut down
        | (0b0 << 3) // 6x PWM mode
        | (0b0 << 2) // don't reset latched faults
        | (0b00 << 0); // gate-drive peak current: 1.7A

    new_config.control_register_2 =
          (0b0 << 6) // OC_TOFF: cycle by cycle
        | (0b00 << 4) // calibration off (normal operation)
        | (gain_setting << 2) // select gain
        | (0b00 << 0); // report both over temperature and over current on nOCTW pin

    bool regs_equal = (regs_.control_register_1 == new_config.control_register_1)
                   && (regs_.control_register_2 == new_config.control_register_2);

    if (!regs_equal) {
        regs_ = new_config;
        state_ = kStateUninitialized;
        enable_gpio_.write(false);
    }

    return true;
}

// Debug trace from odrive_main.cpp
extern "C" void odrive_drv_trace(uint8_t v);

bool Drv8301::init() {
    uint16_t val;
    odrive_drv_trace(0x20);  // init entry

    if (state_ == kStateReady) {
        odrive_drv_trace(0x21);
        return true;
    }

    odrive_drv_trace(0x22);
    // Reset DRV chip. The enable pin also controls the SPI interface, not only
    // the driver stages.
    enable_gpio_.write(false);
    odrive_drv_trace(0x23);
    delay_us(40); // mimumum pull-down time for full reset: 20us
    state_ = kStateUninitialized; // make is_ready() ignore transient errors before registers are set up
    enable_gpio_.write(true);
    odrive_drv_trace(0x24);
    osDelay(20); // t_spi_ready, max = 10ms
    odrive_drv_trace(0x25);

    // Write current configuration
    bool wrote_regs = true;
    for (int i = 0; i < 5 && wrote_regs; i++) {
        odrive_drv_trace(0x30 + i);
        wrote_regs = write_reg(kRegNameControl1, regs_.control_register_1);
        odrive_drv_trace(wrote_regs ? 0x40 + i : 0x50 + i);
    }
    if (wrote_regs) {
        odrive_drv_trace(0x36);
        wrote_regs = write_reg(kRegNameControl2, regs_.control_register_2);
        odrive_drv_trace(wrote_regs ? 0x46 : 0x56);
    }

    if (!wrote_regs) {
        odrive_drv_trace(0x57);
        return false;
    }

    // Wait for configuration to be applied
    delay_us(100);
    state_ = kStateStartupChecks;
    odrive_drv_trace(0x60);

    // ODrive v0.5.6 fazia readback verify (val == regs_.control_register_1).
    // Isso falha na MKS Mini por diferença sutil no protocolo SPI/chip
    // (v0.5.1 pula esse verify e funciona). Seguimos v0.5.1: checamos apenas
    // que a leitura SPI nao errou. Se chip nao estiver respondendo, read_reg
    // retorna false (rx_buf == 0xbeef). Caso contrario, confiamos.
    bool is_read_regs = read_reg(kRegNameControl1, &val);
    odrive_drv_trace(is_read_regs ? 0x61 : 0x71);
    // Log do valor lido pra debug (sobrescreve marker anterior com valor real)
    odrive_drv_trace((uint8_t)(val & 0xff));
    odrive_drv_trace((uint8_t)((val >> 8) & 0xff));

    if (is_read_regs) is_read_regs = read_reg(kRegNameControl2, &val);
    odrive_drv_trace(is_read_regs ? 0x63 : 0x73);
    odrive_drv_trace((uint8_t)(val & 0xff));
    odrive_drv_trace((uint8_t)((val >> 8) & 0xff));

    if (!is_read_regs) {
        odrive_drv_trace(0x75);
        return false;
    }
    odrive_drv_trace(0x66);

    if (get_error() != FaultType_NoFault) {
        return false;
    }

    // There could have been an nFAULT edge meanwhile. In this case we shouldn't
    // consider the driver ready.
    CRITICAL_SECTION() {
        if (state_ == kStateStartupChecks) {
            state_ = kStateReady;
        }
    }

    return state_ == kStateReady;
}

// Debounce counter: nFAULT tem que ficar LOW em varios checks consecutivos
// pra considerar fault real. Motor desconectado ou transientes PWM podem
// fazer nFAULT flicker LOW brevemente sem ser falha real — na v0.5.1 isso
// era tolerado por nao ter um state machine reativo.
static volatile uint32_t g_nfault_low_counter = 0;
static constexpr uint32_t NFAULT_DEBOUNCE_THRESHOLD = 100;  // ~100 checks (~5ms a 20kHz)
extern "C" volatile uint32_t g_nfault_flicker_count;

void Drv8301::do_checks() {
    if (state_ == kStateUninitialized) {
        g_nfault_low_counter = 0;
        return;
    }
    bool nfault_low = !nfault_gpio_.read();
    if (nfault_low) {
        g_nfault_low_counter++;
        if (g_nfault_low_counter >= NFAULT_DEBOUNCE_THRESHOLD) {
            state_ = kStateUninitialized;
            g_nfault_flicker_count++;  // visibilidade via CDC
        }
    } else {
        // nFAULT voltou HIGH — reset contador (single flicker ignorado)
        if (g_nfault_low_counter > 0) g_nfault_flicker_count++;
        g_nfault_low_counter = 0;
    }
}

bool Drv8301::is_ready() {
    return state_ == kStateReady;
}

Drv8301::FaultType_e Drv8301::get_error() {
    uint16_t fault1, fault2;

    if (!read_reg(kRegNameStatus1, &fault1) ||
        !read_reg(kRegNameStatus2, &fault2)) {
        return (FaultType_e)0xffffffff;
    }

    return (FaultType_e)((uint32_t)fault1 | ((uint32_t)(fault2 & 0x0080) << 16));
}

bool Drv8301::read_reg(const RegName_e regName, uint16_t* data) {
    tx_buf_ = build_ctrl_word(DRV8301_CtrlMode_Read, regName, 0);
    if (!spi_arbiter_->transfer(spi_config_, ncs_gpio_, (uint8_t *)(&tx_buf_), nullptr, 1, 100)) {
        return false;
    }
    
    delay_us(1);

    tx_buf_ = build_ctrl_word(DRV8301_CtrlMode_Read, regName, 0);
    rx_buf_ = 0xffff;
    if (!spi_arbiter_->transfer(spi_config_, ncs_gpio_, (uint8_t *)(&tx_buf_), (uint8_t *)(&rx_buf_), 1, 100)) {
        return false;
    }

    delay_us(1);

    if (rx_buf_ == 0xbeef) {
        return false;
    }

    if (data) {
        *data = rx_buf_ & 0x07FF;
    }
    
    return true;
}

bool Drv8301::write_reg(const RegName_e regName, const uint16_t data) {
    // Do blocking write
    tx_buf_ = build_ctrl_word(DRV8301_CtrlMode_Write, regName, data);
    if (!spi_arbiter_->transfer(spi_config_, ncs_gpio_, (uint8_t *)(&tx_buf_), nullptr, 1, 100)) {
        return false;
    }
    delay_us(1);

    return true;
}
