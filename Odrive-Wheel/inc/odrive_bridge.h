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

// Telemetria 1 kHz embedded no HID input report (Tier 1 task 1).
// vel_estimate em turns/s (PLL filtrado), Iq em A, torque_output em Nm.
float odrive_bridge_get_vel_estimate(void);
float odrive_bridge_get_iq_measured(void);
float odrive_bridge_get_torque_output(void);

// Corrente no resistor de freio (regen). Já existe vbus/ibus acima — esta
// completa a tríade pro overlay HID-only computar P_brake real a 1 kHz.
float odrive_bridge_get_brake_resistor_current(void);

// Leituras térmicas em °C (Onboard FET thermistor e Offboard Motor thermistor).
// Alimentam sys.temp? / sys.motortemp? e ferramentas externas que monitoram
// thermal derating sem precisar do canal ODrive ASCII completo.
float odrive_bridge_get_fet_temp(void);
float odrive_bridge_get_motor_temp(void);

// Snapshot dos contadores de SPI ABS do encoder pra debugar AS5047.
// Preenche o struct com: ok_count, fail_parity, fail_ef, fail_xfer, last_rx.
struct encraw_snap_t {
    unsigned int ok_count;
    unsigned int fail_parity;
    unsigned int fail_ef;
    unsigned int fail_xfer;
    unsigned int last_rx;       // raw 16-bit recebido na última transação
    unsigned int pos_abs;       // último pos_abs (que só atualiza quando validação passa)
};
void odrive_bridge_enc_get_raw(struct encraw_snap_t *snap);

// Snapshot do registro DIAAGC do AS5047 (atualizado a cada 256 transações ≈ 31 Hz).
struct magnet_snap_t {
    unsigned int raw;           // raw 14-bit (MAGL|MAGH|COF|LF|AGC[7:0])
    unsigned int update_count;  // # de leituras DIAAGC bem-sucedidas desde boot
    unsigned int agc;           // 0-255 (ideal ~128)
    unsigned int magl;          // 1 = magneto longe/fraco
    unsigned int magh;          // 1 = magneto perto/forte
    unsigned int cof;           // 1 = CORDIC overflow (ângulo inválido)
    unsigned int lf;            // 1 = loop finished (offset compensation OK)
};
void odrive_bridge_enc_get_magnet(struct magnet_snap_t *snap);

// ==== Option A: thread de leitura SPI do encoder MT6835 =====================
// Chamado uma vez em rtos_main após ffb_task_init(). A thread bloqueia num
// semáforo binário e é acordada por odrive_bridge_enc_spi_kick() a partir do
// control loop (Encoder::update, IRQ de baixa prioridade), pra tirar a leitura
// SPI de 24 bits do MT6835 do ISR prio-0 (sampling_cb). Ver odrive_bridge.cpp.
void odrive_bridge_start_enc_thread(void);
// Kick ISR-safe (osSemaphoreRelease) — chamado do control loop IRQ.
void odrive_bridge_enc_spi_kick(void);

// ==== MT6835 (mode 261) — acesso a registro + comandos do chip ==============
// Todos retornam falha (-1 / 0) se o encoder não está em MODE_SPI_ABS_MT6835.
int  odrive_bridge_mt6835_read_reg(int addr);            // >= 0: valor, -1: falha
int  odrive_bridge_mt6835_write_reg(int addr, int val);  // 1 OK, 0 falha
int  odrive_bridge_mt6835_set_zero(void);                // 1 OK (ack 0x55), 0 falha/armado
int  odrive_bridge_mt6835_program_eeprom(void);          // 1 OK — aguardar 6 s antes de power-off!

struct mt6835_snap_t {
    int is_mt6835;     // 1 = encoder mode é MT6835 (261)
    int boot_ok;       // 1 = comm verificada por CRC no setup()
    int hyst_zeroed;   // 1 = HYST=0 aplicado com sucesso no boot
    int overspeed;     // STATUS[0] da última leitura de ângulo válida
    int weak_field;    // STATUS[1] — 1 = campo magnético fraco (magneto longe!)
    int undervolt;     // STATUS[2]
    int cal_state;     // reg 0x113[7:6]: 0 none, 1 running, 2 failed, 3 ok; -1 = leitura falhou
};
void odrive_bridge_mt6835_get_status(struct mt6835_snap_t *snap);

#ifdef __cplusplus
}
#endif

#endif
