// Phase 2c — Bridge FFB ⇄ ODrive v0.5.6 stock.
// Este é o UNICO arquivo que inclui odrive_main.h fora da árvore ODrive — evita
// colisão entre nomes "Axis" (ODrive) e "Axis" (OpenFFBoard FFB stack).

#include "odrive_bridge.h"
#include "odrive_main.h"

extern "C" void odrive_bridge_init(void) {
    // v0.5.6 stock: motor.setup() + axis.start_thread() já correm pelo
    // odrive_main() normal. TIM3 encoder quadrature está ativo. Nada a fazer.
}

extern "C" float odrive_bridge_get_pos_turns(void) {
    // shadow_count_ é int32 acumulado pelo axis thread (não wrappa em 16-bit
    // como o timer CNT direto). turns = shadow_count / cpr.
    int32_t cnt = axes[0].encoder_.shadow_count_;
    int32_t cpr = axes[0].encoder_.config_.cpr;
    if (cpr <= 0) cpr = 10000;
    return (float)cnt / (float)cpr;
}

extern "C" void odrive_bridge_set_input_torque(float nm) {
    // Efetivo apenas se axis está em CLOSED_LOOP_CONTROL + control_mode=TORQUE
    // + input_mode=PASSTHROUGH. Fora desses estados, escrita é ignorada pelo
    // Controller::update(). Defaults bakados no firmware atendem (control_mode=1,
    // input_mode=1) mas state precisa ser setado via ASCII ou pela UI HTML.
    axes[0].controller_.input_torque_ = nm;
}

// Bus current/voltage readouts — usados pelo peak tracker do ffb_task.
// Centralizados aqui pra não precisar incluir odrive_main.h em outros .cpp
// (colide com class Axis do OpenFFBoard).
extern "C" float odrive_bridge_get_ibus(void)        { return odrv.ibus_; }
extern "C" float odrive_bridge_get_vbus(void)        { return odrv.vbus_voltage_; }
extern "C" float odrive_bridge_get_motor_ibus(void)  { return axes[0].motor_.I_bus_; }
extern "C" int   odrive_bridge_motor_is_armed(void)  { return axes[0].motor_.is_armed_ ? 1 : 0; }
