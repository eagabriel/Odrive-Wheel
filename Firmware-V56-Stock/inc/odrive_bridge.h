// Phase 5 — Bridge minimal pros internals da ODrive.
// Isola odrive_main.h (que define sua propria classe Axis) do resto do codigo
// FFB, que usa a classe Axis stub de OpenFFBoard. Evita colisao de nomes.

#ifndef ODRIVE_BRIDGE_H_
#define ODRIVE_BRIDGE_H_

#ifdef __cplusplus
extern "C" {
#endif

// Inicia o quadrature decoder do encoder (HAL_TIM_Encoder_Start em axes[0]).
// Necessario porque motor.setup() — que normalmente faz isso — foi deferido.
void odrive_bridge_init(void);

// Posicao absoluta do encoder em voltas. 0.0 se estimativa indisponivel.
float odrive_bridge_get_pos_turns(void);

// Escreve setpoint de torque (Nm) em axes[0].controller_.input_torque_.
// Sem motor armado a ODrive ignora — seguro chamar sempre.
void odrive_bridge_set_input_torque(float nm);

// Telemetry — leituras de bus/motor pra peak tracker do ffb_task. Não pode
// ler odrv.ibus_ direto do ffb_task porque odrive_main.h colide com Axis.h.
float odrive_bridge_get_ibus(void);
float odrive_bridge_get_vbus(void);
float odrive_bridge_get_motor_ibus(void);
int   odrive_bridge_motor_is_armed(void);

#ifdef __cplusplus
}
#endif

#endif
