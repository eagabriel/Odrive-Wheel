/*
* The ASCII protocol is a simpler, human readable alternative to the main native
* protocol.
* In the future this protocol might be extended to support selected GCode commands.
* For a list of supported commands see doc/ascii-protocol.md
*/

/* Includes ------------------------------------------------------------------*/

#include "odrive_main.h"
#include "communication.h"
#include "ascii_protocol.hpp"
#include <utils.hpp>
#include <fibre/cpp_utils.hpp>

#include "autogen/type_info.hpp"
#include "communication/interface_can.hpp"

using namespace fibre;

// Phase 2c — FFB diagnostic counters (defined in src/ffb_task.cpp).
// Acessadas pelo comando ASCII custom 'd' (FFB diag).
extern "C" {
    uint32_t ffb_diag_hidout_total(void);
    uint32_t ffb_diag_hidout_ctrl(void);
    uint32_t ffb_diag_hidout_neweff(void);
    uint32_t ffb_diag_hidout_seteff(void);
    uint32_t ffb_diag_hidout_cond(void);
    uint32_t ffb_diag_hidout_const(void);
    uint32_t ffb_diag_hidout_period(void);
    uint32_t ffb_diag_hidout_efop(void);
    uint32_t ffb_diag_hidout_gain(void);
    uint32_t ffb_diag_hidout_other(void);
    uint32_t ffb_diag_hidget(void);
    uint32_t ffb_diag_set_eff_torque(void);
    int32_t  ffb_diag_last_torque(void);
    int      ffb_diag_active_effects(void);
    float    ffb_diag_pending_torque(void);
    int      ffb_diag_ffb_active_flag(void);
    int      ffb_diag_eff_index(void);
    int      ffb_diag_eff_state(void);
    int      ffb_diag_eff_type(void);
    int32_t  ffb_diag_eff_magnitude(void);
    float    ffb_diag_eff_axmag0(void);
    int      ffb_diag_eff_gain(void);
    int      ffb_diag_global_gain(void);
    int      ffb_diag_eff_index_n(int n);
    int      ffb_diag_eff_state_n(int n);
    int      ffb_diag_eff_type_n(int n);
    int32_t  ffb_diag_eff_magnitude_n(int n);
    float    ffb_diag_eff_axmag0_n(int n);
    int      ffb_diag_eff_gain_n(int n);
    int      ffb_diag_slot_state(int slot);
    int      ffb_diag_slot_type(int slot);
    int32_t  ffb_diag_slot_magnitude(int slot);
    int      ffb_diag_total_slots(void);

    // OpenFFBoard CmdParser — habilita Configurator GUI no mesmo CDC.
    // Detecta linhas no formato "class.cmd?/=/!" e roteia pra esse parser.
    #include "cmdparser.h"

    // Bypass do pipeline ODrive — escreve direto na FIFO do TinyUSB CDC.
    // Necessário porque o BufferedStreamSink + Stm32UsbTxStream do ODrive
    // entram em estado quebrado quando o Configurator floodea comandos.
    int cmdparser_cdc_write(const uint8_t *buf, uint32_t len);

    float    ffb_diag_ibus_max(void);
    float    ffb_diag_ibus_min(void);
    float    ffb_diag_motor_ibus_max(void);
    float    ffb_diag_motor_ibus_min(void);
    float    ffb_diag_vbus_max(void);
    float    ffb_diag_vbus_min(void);
    void     ffb_diag_reset_ibus_peaks(void);
    float    ffb_get_axis_range(void);
    float    ffb_get_axis_maxtq(void);
    float    ffb_get_axis_fxratio(void);
    void     ffb_set_axis_range(float v);
    void     ffb_set_axis_maxtq(float v);
    void     ffb_set_axis_fxratio(float v);
}

/* Private macros ------------------------------------------------------------*/
/* Private typedef -----------------------------------------------------------*/
/* Global constant data ------------------------------------------------------*/
/* Global variables ----------------------------------------------------------*/
/* Private constant data -----------------------------------------------------*/

#define TO_STR_INNER(s) #s
#define TO_STR(s) TO_STR_INNER(s)

/* Private variables ---------------------------------------------------------*/

#if HW_VERSION_MAJOR == 3
static Introspectable root_obj = ODrive3TypeInfo<ODrive>::make_introspectable(odrv);
#elif HW_VERSION_MAJOR == 4
static Introspectable root_obj = ODrive4TypeInfo<ODrive>::make_introspectable(odrv);
#endif

/* Private function prototypes -----------------------------------------------*/

/* Function implementations --------------------------------------------------*/

// @brief Sends a line on the specified output.
template<typename ... TArgs>
void AsciiProtocol::respond(bool include_checksum, const char * fmt, TArgs&& ... args) {
    char tx_buf[64];

    size_t len = snprintf(tx_buf, sizeof(tx_buf), fmt, std::forward<TArgs>(args)...);

    // Silently truncate the output if it's too long for the buffer.
    len = std::min(len, sizeof(tx_buf));

    if (include_checksum) {
        uint8_t checksum = 0;
        for (size_t i = 0; i < len; ++i)
            checksum ^= tx_buf[i];
        len += snprintf(tx_buf + len, sizeof(tx_buf) - len, "*%u\r\n", checksum);
    } else {
        len += snprintf(tx_buf + len, sizeof(tx_buf) - len, "\r\n");
    }

    // Silently truncate the output if it's too long for the buffer.
    len = std::min(len, sizeof(tx_buf));

    sink_.write({(const uint8_t*)tx_buf, len});
    sink_.maybe_start_async_write();
}


// @brief Executes an ASCII protocol command
// @param buffer buffer of ASCII encoded characters
// @param len size of the buffer
void AsciiProtocol::process_line(cbufptr_t buffer) {
    static_assert(sizeof(char) == sizeof(uint8_t));

    // OpenFFBoard CmdParser usa ';' como SEPARADOR de comandos (não comentário).
    // Detecta antes do parser ODrive consumir: se a linha tem '.' + ('?'/'='/'!'),
    // pula o tratamento de ';' como comentário.
    bool is_offboard = false;
    {
        bool has_dot = false, has_op = false;
        for (size_t i = 0; i < buffer.size(); ++i) {
            uint8_t c = buffer.begin()[i];
            if (c == '.') has_dot = true;
            if (c == '?' || c == '=' || c == '!') has_op = true;
        }
        is_offboard = has_dot && has_op;
    }

    // scan line to find beginning of checksum and prune comment
    uint8_t checksum = 0;
    size_t checksum_start = SIZE_MAX;
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (!is_offboard && buffer.begin()[i] == ';') { // ';' é comentário só pra ODrive ASCII
            buffer = buffer.take(i);
            break;
        }
        if (checksum_start > i) {
            if (buffer[i] == '*') {
                checksum_start = i + 1;
            } else {
                checksum ^= buffer[i];
            }
        }
    }

    // copy everything into a local buffer so we can insert null-termination
    char cmd[MAX_LINE_LENGTH + 1];
    size_t len = std::min(buffer.size(), MAX_LINE_LENGTH);
    memcpy(cmd, buffer.begin(), len);
    cmd[len] = 0; // null-terminate

    // optional checksum validation
    bool use_checksum = (checksum_start < len);
    if (use_checksum) {
        unsigned int received_checksum;
        int numscan = sscanf(&cmd[checksum_start], "%u", &received_checksum);
        if ((numscan < 1) || (received_checksum != checksum))
            return;
        len = checksum_start - 1; // prune checksum and asterisk
        cmd[len] = 0; // null-terminate
    }


    // OpenFFBoard CmdParser detection — antes do switch ODrive.
    // Linhas que contêm '.' E (?, =, !) são tratadas como comandos do
    // Configurator do OpenFFBoard. Ex: "axis.maxtorque?", "fx.spring=128"
    // CRÍTICO: reply vai DIRETO no TinyUSB CDC via cmdparser_cdc_write,
    // bypassando BufferedStreamSink + Stm32UsbTxStream do ODrive. Esse
    // pipeline da ODrive entra em deadlock permanente quando Configurator
    // mandava flood de comandos no probe (sintoma: serial morre depois).
    // Mesma estratégia que Firmware-Merged usa (usb_task drena CDC raw).
    if (strchr(cmd, '.') != nullptr && strpbrk(cmd, "?=!") != nullptr) {
        char reply[512];
        size_t n = cmdparser_feed((const uint8_t*)cmd, len, reply, sizeof(reply));
        n += cmdparser_feed((const uint8_t*)";", 1,
                            reply + n,
                            sizeof(reply) > n ? sizeof(reply) - n : 0);
        if (n > 0) {
            cmdparser_cdc_write((const uint8_t*)reply, n);
        }
        return;
    }

    // check incoming packet type
    switch(cmd[0]) {
        case 'p': cmd_set_position(cmd, use_checksum);                break;  // position control
        case 'q': cmd_set_position_wl(cmd, use_checksum);             break;  // position control with limits
        case 'v': cmd_set_velocity(cmd, use_checksum);                break;  // velocity control
        case 'c': cmd_set_torque(cmd, use_checksum);                  break;  // current control
        case 't': cmd_set_trapezoid_trajectory(cmd, use_checksum);    break;  // trapezoidal trajectory
        case 'f': cmd_get_feedback(cmd, use_checksum);                break;  // feedback
        case 'h': cmd_help(cmd, use_checksum);                        break;  // Help
        case 'i': cmd_info_dump(cmd, use_checksum);                   break;  // Dump device info
        case 's': cmd_system_ctrl(cmd, use_checksum);                 break;  // System
        case 'r': cmd_read_property(cmd,  use_checksum);              break;  // read property
        case 'w': cmd_write_property(cmd, use_checksum);              break;  // write property
        case 'u': cmd_update_axis_wdg(cmd, use_checksum);             break;  // Update axis watchdog.
        case 'e': cmd_encoder(cmd, use_checksum);                     break;  // Encoder commands
        case 'd': {                                                            // FFB diagnostic dump (custom)
            // Uma única respond curta mostrando os 4 valores mais importantes.
            // Múltiplas respond() consecutivas trava o sink — não tentar de novo.
            respond(use_checksum, "ffb=%d eff=%d hout=%lu setEf=%lu",
                ffb_diag_ffb_active_flag(),
                ffb_diag_active_effects(),
                ffb_diag_hidout_total(),
                ffb_diag_set_eff_torque());
            break;
        }
        case 'D': {                                                            // FFB diag detalhada (apenas se 'd' funcionou)
            respond(use_checksum, "ctrl=%lu newef=%lu setef=%lu efop=%lu",
                ffb_diag_hidout_ctrl(), ffb_diag_hidout_neweff(),
                ffb_diag_hidout_seteff(), ffb_diag_hidout_efop());
            break;
        }
        case 'C': {                                                            // FFB diag conditions/forces
            respond(use_checksum, "cond=%lu cnst=%lu prdc=%lu",
                ffb_diag_hidout_cond(),
                ffb_diag_hidout_const(),
                ffb_diag_hidout_period());
            break;
        }
        case 'T': {                                                            // FFB diag torque
            respond(use_checksum, "lt=%ld nm=%.4f",
                (long)ffb_diag_last_torque(),
                (double)ffb_diag_pending_torque());
            break;
        }
        case 'I': {                                                            // Bus current/voltage peaks
            // ibus negativo = regen voltando pra fonte (suspeito de derrubar PSU)
            // ibus positivo = consumo da fonte
            respond(use_checksum, "Ibus=%.2f..%.2f Mot=%.2f..%.2f V=%.2f..%.2f",
                (double)ffb_diag_ibus_min(),       (double)ffb_diag_ibus_max(),
                (double)ffb_diag_motor_ibus_min(), (double)ffb_diag_motor_ibus_max(),
                (double)ffb_diag_vbus_min(),       (double)ffb_diag_vbus_max());
            break;
        }
        case 'R': {                                                            // Reset peak trackers
            ffb_diag_reset_ibus_peaks();
            respond(use_checksum, "peaks reset");
            break;
        }
        case 'M': {                                                            // Get/set FFB axis params: 'M' lê, 'M <maxtq> [fxratio] [range]' escreve
            float v_max=0, v_fx=-1, v_rg=-1;
            int n = sscanf(cmd, "M %f %f %f", &v_max, &v_fx, &v_rg);
            if (n >= 1) {
                ffb_set_axis_maxtq(v_max);
                if (n >= 2) ffb_set_axis_fxratio(v_fx);
                if (n >= 3) ffb_set_axis_range(v_rg);
            }
            respond(use_checksum, "maxtq=%.2f fxratio=%.2f range=%.0f eff=%.2fNm",
                (double)ffb_get_axis_maxtq(),
                (double)ffb_get_axis_fxratio(),
                (double)ffb_get_axis_range(),
                (double)(ffb_get_axis_maxtq() * ffb_get_axis_fxratio()));
            break;
        }
        case 'E': {                                                            // FFB diag — N-ésimo efeito ativo
            // E      → primeiro efeito (n=0)
            // E0..E9 → N-ésimo efeito (após N skips de efeitos ativos)
            //   idx<0 → não há efeito nessa posição
            //   axisMagnitudes[0]=0 → bug de direção (efeito não chega no eixo X)
            int n = 0;
            if (cmd[1] >= '0' && cmd[1] <= '9') n = cmd[1] - '0';
            respond(use_checksum, "idx=%d st=%d t=%d mag=%ld ax=%.2f g=%d GG=%d",
                ffb_diag_eff_index_n(n),
                ffb_diag_eff_state_n(n),
                ffb_diag_eff_type_n(n),
                (long)ffb_diag_eff_magnitude_n(n),
                (double)ffb_diag_eff_axmag0_n(n),
                ffb_diag_eff_gain_n(n),
                ffb_diag_global_gain());
            break;
        }
        case 'S': {                                                            // FFB diag — slot físico (mesmo se INACTIVE)
            // S0..S39 — dump do slot N (independente de state). Útil pra ver
            // efeitos que jogo alocou mas não startou (state=0). Cmd retorna
            // contagem total de slots alocados se vier sem dígito (apenas 'S').
            if (cmd[1] >= '0' && cmd[1] <= '9') {
                int slot = cmd[1] - '0';
                if (cmd[2] >= '0' && cmd[2] <= '9') slot = slot * 10 + (cmd[2] - '0');
                respond(use_checksum, "s=%d st=%d t=%d mag=%ld",
                    slot,
                    ffb_diag_slot_state(slot),
                    ffb_diag_slot_type(slot),
                    (long)ffb_diag_slot_magnitude(slot));
            } else {
                respond(use_checksum, "total=%d alloc", ffb_diag_total_slots());
            }
            break;
        }
        default : cmd_unknown(nullptr, use_checksum);                 break;
    }
}

// @brief Executes the set position command
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_set_position(char * pStr, bool use_checksum) {
    unsigned motor_number;
    float pos_setpoint, vel_feed_forward, torque_feed_forward;

    int numscan = sscanf(pStr, "p %u %f %f %f", &motor_number, &pos_setpoint, &vel_feed_forward, &torque_feed_forward);
    if (numscan < 2) {
        respond(use_checksum, "invalid command format");
    } else if (motor_number >= AXIS_COUNT) {
        respond(use_checksum, "invalid motor %u", motor_number);
    } else {
        Axis& axis = axes[motor_number];
        axis.controller_.config_.control_mode = Controller::CONTROL_MODE_POSITION_CONTROL;
        axis.controller_.input_pos_ = pos_setpoint;
        if (numscan >= 3)
            axis.controller_.input_vel_ = vel_feed_forward;
        if (numscan >= 4)
            axis.controller_.input_torque_ = torque_feed_forward;
        axis.controller_.input_pos_updated();
        axis.watchdog_feed();
    }
}

// @brief Executes the set position with current and velocity limit command
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_set_position_wl(char * pStr, bool use_checksum) {
    unsigned motor_number;
    float pos_setpoint, vel_limit, torque_lim;

    int numscan = sscanf(pStr, "q %u %f %f %f", &motor_number, &pos_setpoint, &vel_limit, &torque_lim);
    if (numscan < 2) {
        respond(use_checksum, "invalid command format");
    } else if (motor_number >= AXIS_COUNT) {
        respond(use_checksum, "invalid motor %u", motor_number);
    } else {
        Axis& axis = axes[motor_number];
        axis.controller_.config_.control_mode = Controller::CONTROL_MODE_POSITION_CONTROL;
        axis.controller_.input_pos_ = pos_setpoint;
        if (numscan >= 3)
            axis.controller_.config_.vel_limit = vel_limit;
        if (numscan >= 4)
            axis.motor_.config_.torque_lim = torque_lim;
        axis.controller_.input_pos_updated();
        axis.watchdog_feed();
    }
}

// @brief Executes the set velocity command
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_set_velocity(char * pStr, bool use_checksum) {
    unsigned motor_number;
    float vel_setpoint, torque_feed_forward;
    int numscan = sscanf(pStr, "v %u %f %f", &motor_number, &vel_setpoint, &torque_feed_forward);
    if (numscan < 2) {
        respond(use_checksum, "invalid command format");
    } else if (motor_number >= AXIS_COUNT) {
        respond(use_checksum, "invalid motor %u", motor_number);
    } else {
        Axis& axis = axes[motor_number];
        axis.controller_.config_.control_mode = Controller::CONTROL_MODE_VELOCITY_CONTROL;
        axis.controller_.input_vel_ = vel_setpoint;
        if (numscan >= 3)
            axis.controller_.input_torque_ = torque_feed_forward;
        axis.watchdog_feed();
    }
}

// @brief Executes the set torque control command
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_set_torque(char * pStr, bool use_checksum) {
    unsigned motor_number;
    float torque_setpoint;

    if (sscanf(pStr, "c %u %f", &motor_number, &torque_setpoint) < 2) {
        respond(use_checksum, "invalid command format");
    } else if (motor_number >= AXIS_COUNT) {
        respond(use_checksum, "invalid motor %u", motor_number);
    } else {
        Axis& axis = axes[motor_number];
        axis.controller_.config_.control_mode = Controller::CONTROL_MODE_TORQUE_CONTROL;
        axis.controller_.input_torque_ = torque_setpoint;
        axis.watchdog_feed();
    }
}

// @brief Sets the encoder linear count
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_encoder(char * pStr, bool use_checksum) {
    if (pStr[1] == 's') {
        pStr += 2; // Substring two characters to the right (ok because we have guaranteed null termination after all chars)

        unsigned motor_number;
        int encoder_count;

        if (sscanf(pStr, "l %u %i", &motor_number, &encoder_count) < 2) {
            respond(use_checksum, "invalid command format");
        } else if (motor_number >= AXIS_COUNT) {
            respond(use_checksum, "invalid motor %u", motor_number);
        } else {
            Axis& axis = axes[motor_number];
            axis.encoder_.set_linear_count(encoder_count);
            axis.watchdog_feed();
            respond(use_checksum, "encoder set to %u", encoder_count);
        }
    } else {
        respond(use_checksum, "invalid command format");
    }
}

// @brief Executes the set trapezoid trajectory command
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_set_trapezoid_trajectory(char* pStr, bool use_checksum) {
    unsigned motor_number;
    float goal_point;

    if (sscanf(pStr, "t %u %f", &motor_number, &goal_point) < 2) {
        respond(use_checksum, "invalid command format");
    } else if (motor_number >= AXIS_COUNT) {
        respond(use_checksum, "invalid motor %u", motor_number);
    } else {
        Axis& axis = axes[motor_number];
        axis.controller_.config_.input_mode = Controller::INPUT_MODE_TRAP_TRAJ;
        axis.controller_.config_.control_mode = Controller::CONTROL_MODE_POSITION_CONTROL;
        axis.controller_.input_pos_ = goal_point;
        axis.controller_.input_pos_updated();
        axis.watchdog_feed();
    }
}

// @brief Executes the get position and velocity feedback command
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_get_feedback(char * pStr, bool use_checksum) {
    unsigned motor_number;

    if (sscanf(pStr, "f %u", &motor_number) < 1) {
        respond(use_checksum, "invalid command format");
    } else if (motor_number >= AXIS_COUNT) {
        respond(use_checksum, "invalid motor %u", motor_number);
    } else {
        Axis& axis = axes[motor_number];
        respond(use_checksum, "%f %f",
                (double)axis.encoder_.pos_estimate_.any().value_or(0.0f),
                (double)axis.encoder_.vel_estimate_.any().value_or(0.0f));
    }
}

// @brief Shows help text
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_help(char * pStr, bool use_checksum) {
    (void)pStr;
    respond(use_checksum, "Please see documentation for more details");
    respond(use_checksum, "");
    respond(use_checksum, "Available commands syntax reference:");
    respond(use_checksum, "Position: q axis pos vel-lim I-lim");
    respond(use_checksum, "Position: p axis pos vel-ff I-ff");
    respond(use_checksum, "Velocity: v axis vel I-ff");
    respond(use_checksum, "Torque: c axis T");
    respond(use_checksum, "");
    respond(use_checksum, "Properties start at odrive root, such as axis0.requested_state");
    respond(use_checksum, "Read: r property");
    respond(use_checksum, "Write: w property value");
    respond(use_checksum, "");
    respond(use_checksum, "Save config: ss");
    respond(use_checksum, "Erase config: se");
    respond(use_checksum, "Reboot: sr");
}

// @brief Gets the hardware, firmware and serial details
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_info_dump(char * pStr, bool use_checksum) {
    // respond(use_checksum, "Signature: %#x", STM_ID_GetSignature());
    // respond(use_checksum, "Revision: %#x", STM_ID_GetRevision());
    // respond(use_checksum, "Flash Size: %#x KiB", STM_ID_GetFlashSize());
    respond(use_checksum, "Hardware version: %d.%d-%dV", odrv.hw_version_major_, odrv.hw_version_minor_, odrv.hw_version_variant_);
    respond(use_checksum, "Firmware version: %d.%d.%d", odrv.fw_version_major_, odrv.fw_version_minor_, odrv.fw_version_revision_);
    respond(use_checksum, "Serial number: %s", serial_number_str);
}

// @brief Executes the system control command
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_system_ctrl(char * pStr, bool use_checksum) {
    switch (pStr[1])
    {
        case 's':   odrv.save_configuration();  break;  // Save config
        case 'e':   odrv.erase_configuration(); break;  // Erase config
        case 'r':   odrv.reboot();              break;  // Reboot
        case 'c':   odrv.clear_errors();        break;  // clear all errors and rearm brake resistor if necessary
        // Odrive-Wheel: 'sd' reboota direto pro bootloader DFU interno do STM32 (sem precisar
        // de jumper BOOT0). Usa _reboot_cookie=0xDEADBEEF + NVIC_SystemReset; early_start_checks
        // detecta e salta pra 0x1FFF0000. Usado pelo flasher via WebUSB do odrive-wheel.html.
        case 'd':   odrv.enter_dfu_mode();      break;  // Reboot to DFU bootloader
        default:    /* default */               break;
    }
}

// @brief Executes the read parameter command
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_read_property(char * pStr, bool use_checksum) {
    char name[MAX_LINE_LENGTH];

    if (sscanf(pStr, "r %255s", name) < 1) {
        respond(use_checksum, "invalid command format");
    } else {
        Introspectable property = root_obj.get_child(name, sizeof(name));
        const StringConvertibleTypeInfo* type_info = dynamic_cast<const StringConvertibleTypeInfo*>(property.get_type_info());
        if (!type_info) {
            respond(use_checksum, "invalid property");
        } else {
            char response[10];
            bool success = type_info->get_string(property, response, sizeof(response));
            respond(use_checksum, success ? response : "not implemented");
        }
    }
}

// @brief Executes the set write position command
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_write_property(char * pStr, bool use_checksum) {
    char name[MAX_LINE_LENGTH];
    char value[MAX_LINE_LENGTH];

    if (sscanf(pStr, "w %255s %255s", name, value) < 1) {
        respond(use_checksum, "invalid command format");
    } else {
        Introspectable property = root_obj.get_child(name, sizeof(name));
        const StringConvertibleTypeInfo* type_info = dynamic_cast<const StringConvertibleTypeInfo*>(property.get_type_info());
        if (!type_info) {
            respond(use_checksum, "invalid property");
        } else {
            bool success = type_info->set_string(property, value, sizeof(value));
            if (!success) {
                respond(use_checksum, "not implemented");
            }
        }
    }
}

// @brief Executes the motor watchdog update command
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_update_axis_wdg(char * pStr, bool use_checksum) {
    unsigned motor_number;

    if (sscanf(pStr, "u %u", &motor_number) < 1) {
        respond(use_checksum, "invalid command format");
    } else if (motor_number >= AXIS_COUNT) {
        respond(use_checksum, "invalid motor %u", motor_number);
    } else {
        axes[motor_number].watchdog_feed();
    }
}

// @brief Sends the unknown command response
// @param pStr buffer of ASCII encoded values
// @param response_channel reference to the stream to respond on
// @param use_checksum bool to indicate whether a checksum is required on response
void AsciiProtocol::cmd_unknown(char * pStr, bool use_checksum) {
    (void)pStr;
    respond(use_checksum, "unknown command");
}

void AsciiProtocol::on_read_finished(ReadResult result) {
    if (result.status != kStreamOk) {
        return;
    }

    for (;;) {
        // OpenFFBoard CmdParser usa '!' como sufixo de comando EXEC (ex.:
        // "sys.save!"). ODrive ASCII original tratava '!' como terminator de
        // linha, o que quebra todos os comandos EXEC do Configurator. Como
        // ODrive ASCII não usa '!' em sintaxe alguma, removê-lo aqui é
        // 100% seguro pro pipeline original.
        uint8_t* end_of_line = std::find_if(rx_buf_, result.end, [](uint8_t c) {
            return c == '\r' || c == '\n';
        });

        if (end_of_line >= result.end) {
            break;
        }

        if (read_active_) {
            process_line({rx_buf_, end_of_line});
        } else {
            // Ignoring this line cause it didn't start at a new-line character
            read_active_ = true;
        }
        
        // Discard the processed bytes and shift the remainder to the beginning of the buffer
        size_t n_remaining = result.end - end_of_line - 1;
        memmove(rx_buf_, end_of_line + 1, n_remaining);
        result.end = rx_buf_ + n_remaining;
    }

    // No more new-line characters in buffer

    if (result.end >= rx_buf_ + sizeof(rx_buf_)) {
        // If the line becomes too long, reset buffer and wait for the next line
        result.end = rx_buf_;
        read_active_ = false;
    }

    TransferHandle dummy;
    rx_channel_->start_read({result.end, rx_buf_ + sizeof(rx_buf_)}, &dummy, MEMBER_CB(this, on_read_finished));
}

void AsciiProtocol::start() {
    TransferHandle dummy;
    rx_channel_->start_read(rx_buf_, &dummy, MEMBER_CB(this, on_read_finished));
}
