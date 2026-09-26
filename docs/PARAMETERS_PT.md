# Referência de parâmetros — ferramenta de configuração Odrive-Wheel

Explicação mais detalhada de cada parâmetro e opção do `odrive-wheel.html`, na
mesma ordem da barra lateral. O tooltip da ferramenta dá o resumo; aqui entra o
porquê, a interação com os outros parâmetros e quando mexer.

Convenções:

- Parâmetros do **ODrive** (abas PSU/RBrake, Axis 0, Motor, Encoder, Controller)
  são gravados com o botão **Save** (`ss`, reinicia a placa).
- Parâmetros do **FFB** (abas FFB Wheel, FFB Effects, FFB Filters e EQ) são
  gravados pelo `sys.save!`, que o mesmo botão **Save** também executa.
- "Volta" = uma rotação mecânica completa do motor.
- Itens marcados **(não usado em volante)** existem no ODrive, mas não têm função
  num wheelbase direct-drive. Mantenha o valor padrão.

---

# 🚀 Quick Start

## Visão geral

Assistente de 13 passos que leva uma placa recém-gravada até um volante
funcionando. Cada passo escreve direto no device e mostra o resultado. O botão
**Aplicar** fica vermelho enquanto há valores não enviados e verde depois de
aplicados. Os passos seguem a ordem necessária: alimentação antes do motor,
motor antes do encoder, calibração antes do FFB.

## Passo 1 — Gravar firmware (DFU)

Leva para a aba DFU Flash. Só é necessário quando a placa ainda não tem o
firmware Odrive-Wheel ou quando há uma versão nova.

## Passo 2 — Conectar à placa

Abre a conexão Web Serial (o mesmo botão Conectar do cabeçalho). Ao conectar, a
ferramenta lê todos os campos do device para preencher as abas.

## Passo 3 — Apagar config existente

Executa `sys.eeformat!` (parâmetros FFB) e `se` (configuração ODrive), reinicia e
reconecta. Recomendado no primeiro flash: uma configuração antiga de outro
firmware pode ter valores que conflitam com este (modos de GPIO, limites, modo
de encoder).

## Passo 4 — Alimentação e proteções

**Definir tensão** lê a tensão real da fonte e configura automaticamente os
limites em volta dela: subtensão = V−5, sobretensão = V+5, rampa de freio de
V+0,5 a V+3. Por isso o botão é obrigatório antes de **Aplicar**. Protege contra
o erro mais comum: limites de tensão copiados de outra instalação com fonte
diferente.

**Aplicar** grava o resistor de freio, os limites de corrente da fonte e o
divisor de VBUS. Também ativa o resistor de freio, define
`dc_max_negative_current = −7 A` e `max_regen_current = 0,1 A`, para que qualquer
energia de regeneração acima de 0,1 A vá para o resistor.

## Passo 5 — Configurar motor

Pares de polos e constante de torque. Os dois precisam estar corretos antes de
calibrar: pares de polos errados fazem a calibração do encoder falhar com
`CPR_POLEPAIRS_MISMATCH`, e a constante de torque errada deixa todo o FFB fora de
escala (a conversão Nm → A usa esse valor).

## Passo 6 — Configurar encoder

Modo, CPR, uso de índice Z e pino CS (para encoders SPI). Para AS5047 também é
preciso salvar e reiniciar antes de calibrar, porque o driver SPI só é
inicializado no boot.

## Passo 7 — Calibrar motor

Roda `MOTOR_CALIBRATION`: mede resistência e indutância de fase (o motor apita,
sem girar). Esses valores ajustam o controle de corrente. Mostra os erros de
eixo e de motor se falhar.

## Passo 8 — Calibrar offset do encoder

Antes, mostra uma agulha com a posição do encoder ao vivo. Gire o volante com a
mão para confirmar que o encoder está sendo lido. Depois roda
`ENCODER_OFFSET_CALIBRATION`, que gira o motor devagar para achar o alinhamento
entre o encoder e as fases. Posicione o volante no centro mecânico antes, porque
a posição inicial serve de referência.

## Passo 9 — Marcar pré-calibrado e salvar

Marca `motor.pre_calibrated` e `encoder.pre_calibrated` e configura o boot para
entrar direto em malha fechada. Assim a placa não recalibra a cada ligação.

## Passo 10 — Configurar Z (pulso de índice)

Só para encoders incrementais com fio Z. **Rodar** procura o índice e grava a
referência. **Pular** é para quem não tem o fio Z ou usa encoder absoluto (nesse
caso a posição absoluta já dá a referência).

## Passo 11 — Configurar FFB

Ângulo de rotação, torque máximo e multiplicador final. São os três valores que
definem a escala do volante (ver aba FFB Wheel).

## Passo 12 — Testar FFB (Spring)

Envia um efeito de mola pelo HID, como um jogo faria. Se o volante volta ao
centro, a cadeia inteira (HID → cálculo de efeitos → motor) está funcionando.

## Passo 13 — Pronto

Resumo e atalhos para os próximos passos opcionais: inputs, overlay e ajuste
fino de efeitos.

---

# ⚙ ODrive

## PSU/RBrake — Alimentação / Brake

### `config.brake_resistance`

Valor em ohms do resistor de freio instalado. O firmware usa esse número para
calcular o PWM do freio: para dissipar uma corrente I no resistor, ele aplica o
duty que produz essa corrente com a tensão atual. Valor menor que o real faz o
freio dissipar menos do que deveria, e a tensão sobe. Valor maior faz o freio
exagerar.

Meça o resistor com um multímetro. A potência dissipada pode ser acompanhada no
Overlay (badge P brake).

### `config.enable_brake_resistor`

Liga o circuito do freio. Quando o motor é freado ou quando você empurra o
volante contra o FFB, o motor vira gerador e devolve energia ao barramento. Sem
freio, essa energia volta para a fonte. Fontes chaveadas geralmente não aceitam
corrente reversa, a tensão sobe e a placa desarma por sobretensão. Num wheelbase
deve ficar sempre ligado.

### `config.dc_bus_undervoltage_trip_level`

Abaixo desta tensão a placa desarma com `DC_BUS_UNDER_VOLTAGE`. Protege contra
fonte caindo ou cabo com resistência alta, situações em que o controle de
corrente perde precisão. O Quick Start usa tensão medida − 5 V: folga suficiente
para quedas momentâneas em picos de torque, mas detecta fonte realmente fraca.

### `config.dc_bus_overvoltage_trip_level`

Acima desta tensão a placa desarma com `DC_BUS_OVER_VOLTAGE`. É a última proteção
contra regeneração que o freio não conseguiu absorver. Deve ficar acima do fim
da rampa de freio (senão o freio nunca atua antes do desarme) e abaixo da tensão
máxima dos capacitores e MOSFETs da placa.

### `config.enable_dc_bus_overvoltage_ramp`

Liga um freio proporcional à tensão, além do freio por corrente. Entre
`ramp_start` e `ramp_end`, o duty do freio sobe linearmente. Funciona mesmo quando
a estimativa de corrente de regeneração erra, porque reage à consequência (tensão
subindo). Recomendado ligado.

### `config.dc_bus_overvoltage_ramp_start`

Tensão em que a rampa começa (duty 0 %). Precisa ficar acima da tensão normal da
fonte, senão o freio fica dissipando o tempo todo e esquenta o resistor à toa. O
Quick Start usa tensão medida + 0,5 V.

### `config.dc_bus_overvoltage_ramp_end`

Tensão em que o freio chega a 100 %. Precisa ficar abaixo do limite de
sobretensão. O Quick Start usa tensão medida + 3 V, deixando 2 V entre freio
total e desarme.

### `config.dc_max_positive_current`

Corrente máxima que a placa pode puxar da fonte. Se o motor pedir mais, a placa
desarma com `DC_BUS_OVER_CURRENT`. Configure um pouco abaixo do limite da fonte:
é melhor a placa desarmar do que a fonte entrar em proteção e desligar no meio
de uma curva. Não confunda com `current_lim`: a corrente no motor pode ser maior
que a da fonte em baixa velocidade (o inversor funciona como conversor abaixador).

### `config.dc_max_negative_current`

Corrente máxima que pode voltar para o barramento (valor negativo). Ultrapassar
gera `DC_BUS_OVER_REGEN_CURRENT`. Sem resistor de freio, use valores pequenos
(−0,5 a −1 A) para não jogar corrente numa fonte que não aceita. Com resistor
ligado, o Quick Start usa −7 A porque o resistor absorve a regeneração.

### `config.max_regen_current` (Regeneration Threshold current)

Apesar do nome, é o **limiar** a partir do qual o freio atua. Regeneração até
esse valor vai para a fonte; acima dele, o excesso vai para o resistor. 0 faz o
freio absorver toda regeneração. O Quick Start usa 0,1 A: praticamente tudo vai
para o resistor, sem o freio ficar pulsando por ruído de medição.

### `sys.vbusdiv` (vbus_divider)

Razão do divisor resistivo que a placa usa para medir a tensão do barramento:
(R1+R2)/R2. MKS XDrive Mini = 19; ODrive v3.6 original = 11. Com o valor errado,
a tensão medida fica errada na mesma proporção, e com ela todas as proteções de
tensão e o cálculo do freio.

Depois de ajustar, compare a tensão mostrada na ferramenta com um multímetro nos
terminais da fonte. O valor é carregado antes do ADC iniciar, justamente para não
haver desarme falso no boot.

## PSU/RBrake — Comunicação

### `config.enable_i2c_a` (não usado em volante)

Liga o I2C nos pinos correspondentes. Deixe desligado: os pinos são usados pelas
entradas do joystick (aba Inputs).

### `config.enable_uart_a`, `enable_uart_b`, `enable_uart_c`

Ligam as portas seriais físicas. O volante usa só USB. Ligar uma UART ocupa GPIOs
que podem estar em uso como botões ou eixos. Só faz sentido para depurar por um
conversor USB-serial externo.

### `config.uart0_protocol`, `uart1_protocol`, `uart2_protocol`

Protocolo de cada UART: ASCII (texto, como o console), Fibre (binário do
`odrivetool`) ou Stdout (só saída de log). Irrelevante com as UARTs desligadas.

### `config.uart_a_baudrate`, `uart_b_baudrate`, `uart_c_baudrate`

Velocidade de cada UART. 115200 é o padrão. Os dois lados da conexão precisam
usar o mesmo valor.

### `config.usb_cdc_protocol`

Protocolo da porta serial USB que a ferramenta usa. Precisa ser ASCII (ou ASCII
com Stdout). Trocar para Fibre faz a ferramenta perder a comunicação com a placa.

### `config.error_gpio_pin`

GPIO que vai para nível alto quando há erro. Pode acionar um LED externo. 0
desliga. Não use um GPIO que já esteja configurado na aba Inputs.

## Axis 0 Geral — Startup Sequence

### `startup_motor_calibration`

Roda a calibração de motor a cada boot. Fica desligado depois do Passo 9: com
`motor.pre_calibrated` os valores medidos são reaproveitados, e o boot fica mais
rápido e silencioso.

### `startup_encoder_index_search`

Procura o pulso Z do encoder a cada boot. Necessário com encoder incremental e
`use_index = True`, porque sem o índice o encoder incremental não sabe onde está
depois de ligar. O motor gira até achar o pulso. Com encoder absoluto (SPI),
deixe desligado.

### `startup_encoder_offset_calibration`

Roda a calibração de offset do encoder a cada boot. Só é necessária com encoder
incremental **sem** Z, porque não há referência de posição que sobreviva ao
desligamento. Com Z ou encoder absoluto, deixe desligado.

### `startup_closed_loop_control`

Entra em malha fechada automaticamente após o boot (e após as calibrações
ligadas acima). Com isso o volante já liga com FFB ativo, sem abrir a ferramenta.
Recomendado ligado.

### `startup_homing` (não usado em volante)

Executa rotina de homing (buscar fim de curso). Não se aplica a volante.

## Axis 0 Geral — Modo de operação

### `enable_sensorless_mode` (não usado em volante)

Controla o motor sem encoder, estimando a posição pela força contraeletromotriz.
Só funciona em velocidade alta. Um volante opera parado ou devagar, então precisa
de encoder.

### `enable_step_dir`, `step_dir_always_on`, `step_gpio_pin`, `dir_gpio_pin` (não usado em volante)

Controle por pulsos step/dir, usado em CNC. O FFB entra pelo USB HID. Deixe
desligado para não ocupar GPIOs.

### `enable_watchdog`

Desarma o motor se o host não enviar atualizações no intervalo definido. O FFB
roda dentro da própria placa (a tarefa FFB a 1 kHz), então esse watchdog do
ODrive não é necessário. Além disso, o firmware zera o torque quando o USB é
desconectado.

### `watchdog_timeout`

Intervalo do watchdog em segundos. Irrelevante com o watchdog desligado.

## Axis 0 Geral — Calibration Lockin

Parâmetros da fase de "travamento" usada pelas calibrações: o firmware aplica
uma corrente fixa para alinhar o rotor e depois gira devagar em malha aberta.

### `calibration_lockin.current`

Corrente usada para segurar e girar o rotor durante a calibração. Precisa vencer
o atrito e o cogging do motor. Se a calibração do encoder falhar em motores
grandes ou com volante montado, aumentar este valor costuma resolver.

### `calibration_lockin.ramp_time`

Tempo para a corrente subir de zero até o valor acima. Rampa lenta evita tranco
quando o rotor se alinha.

### `calibration_lockin.ramp_distance`

Ângulo percorrido durante a rampa. Garante que o rotor saia de uma posição de
equilíbrio instável.

### `calibration_lockin.accel`

Aceleração até a velocidade de lockin.

### `calibration_lockin.vel`

Velocidade de giro durante o lockin. Muito rápida pode perder passo (o rotor não
acompanha o campo); muito lenta aumenta o tempo de calibração.

## Axis 0 Geral — General Lockin e Sensorless Ramp (não usado em volante)

### `general_lockin.*`

Mesma lógica do lockin de calibração, mas para o estado `LOCKIN_SPIN` (girar em
malha aberta). Os campos `finish_on_distance`, `finish_on_enc_idx` e
`finish_on_vel` definem quando o giro termina: após uma distância, ao encontrar o
índice Z ou ao atingir a velocidade. O botão Lockin da aba Debug usa esses
valores. Útil só para teste manual.

### `sensorless_ramp.*`

Rampa de partida do modo sensorless. Sem uso com encoder.

## Motor — Tipo / calibração

### `motor_type`

`HIGH_CURRENT` é o motor BLDC comum de wheelbase. `GIMBAL` é para motores de alta
resistência, controlados por tensão em vez de corrente. `ACIM` é motor de
indução. Tipo errado impede a calibração ou faz o controle de corrente se
comportar mal.

### `pole_pairs`

Número de pares de polos (ímãs ÷ 2). O firmware usa para converter posição
mecânica em ângulo elétrico. Valor errado faz o FOC calcular o ângulo errado. A
calibração do encoder detecta isso e desarma com `CPR_POLEPAIRS_MISMATCH`.

### `torque_constant`

Nm por ampere. Todo o FFB é calculado em Nm e convertido em corrente por este
valor. Se estiver maior que o real, o volante entrega menos torque do que o
indicado (e vice-versa). Estimativa: 8,27 / KV do motor. A ficha do motor, quando
existe, é mais precisa.

### `pre_calibrated`

Reaproveita a resistência e a indutância medidas, sem recalibrar no boot. Só
vale depois de uma calibração bem-sucedida e salva.

### `phase_resistance` e `phase_inductance` (somente leitura)

Medidas pela calibração de motor. Definem os ganhos do controle de corrente
(Kp = banda × L, Ki = banda × R). Editar manualmente desajusta o controle de
corrente. Se parecerem errados, recalibre.

### `calibration_current`

Corrente usada para medir R e L. Maior melhora a precisão da medida, mas esquenta
mais. 10 A serve para a maioria dos motores de wheelbase.

### `resistance_calib_max_voltage`

Tensão máxima aplicada durante a medida de resistência. Se a calibração falhar
com erro de tensão de fase, a corrente de calibração pede mais tensão que o
limite. Aumente este valor ou reduza `calibration_current`.

### `requested_current_range`

Faixa de medição do sensor de corrente (ganho do amplificador). Faixa maior mede
correntes maiores com menos resolução. Precisa ser ≥ `current_lim` +
`current_lim_margin`, senão a medida satura em picos. Só vale após salvar e
reiniciar.

### `current_control_bandwidth`

Banda da malha de corrente em rad/s. Define quão rápido a corrente real segue o
pedido. Maior: torque mais nítido em detalhes rápidos (textura de pista, zebras),
mas mais ruído e chiado no motor. Menor: motor mais silencioso, detalhes rápidos
suavizados.

Como regra geral, a banda precisa ficar bem abaixo da frequência em que a
corrente é amostrada (8 kHz); valores altos demais deixam a malha instável. O
Frequency Sweep do Performance Test mostra se ela está limitando a resposta.

### `current_control_deadband` (modificação Odrive-Wheel)

Zona morta do erro de corrente em A. Com o volante parado, o controle de corrente
fica perseguindo ruído de medição e o motor vibra ou chia. Abaixo desta zona
morta o controle ignora o erro. Fora dela o comportamento é idêntico ao original.

O erro estático introduzido é o valor × `torque_constant` (poucos mNm,
imperceptível). Faixa útil 0,02–0,20 A; 0 desliga.

### `dc_calib_tau`

Constante de tempo do filtro que mede o offset dos sensores de corrente no boot.
Não precisa ser alterada.

## Motor — Limites

### `current_lim`

Corrente máxima no motor, que define o torque máximo físico
(`current_lim × torque_constant`). É a proteção do motor e da placa contra
aquecimento. O torque do FFB é limitado por `axis.maxtorque` e por este valor, o
que for menor. A aba FFB Wheel avisa quando `maxtorque` passa do que este limite
permite.

### `current_lim_margin`

Folga acima de `current_lim` antes de gerar erro de sobrecorrente. Absorve picos
transitórios que o controle ainda não corrigiu, sem desarmar a cada curva mais
forte.

### `torque_lim`

Limite de torque em Nm, aplicado antes da conversão em corrente. `inf` deixa só
`current_lim` limitar. Útil quando se quer limitar em Nm direto.

### `I_bus_hard_min` e `I_bus_hard_max`

Limites rígidos da corrente que o motor puxa ou devolve ao barramento.
Ultrapassar desarma imediatamente. São proteções de hardware, diferentes dos
limites de fonte da aba PSU. Normalmente ficam no padrão.

### `I_leak_max`

Máxima corrente de fuga (soma das três fases, que deveria ser zero). Valores
acima indicam curto para a carcaça ou falha no sensor. Não altere.

## Motor — Termistor FET (onboard)

### `fet_thermistor.config.enabled`

Liga a proteção térmica pelos MOSFETs da placa. Acima de `temp_limit_lower`, a
corrente máxima cai linearmente até zero em `temp_limit_upper`. Recomendado
sempre ligado: o volante perde força gradualmente antes de chegar ao desarme.

### `fet_thermistor.config.temp_limit_lower`

Temperatura em que a redução de corrente começa. Padrão 100 °C. A temperatura
aparece no badge do cabeçalho da ferramenta e no Overlay.

### `fet_thermistor.config.temp_limit_upper`

Temperatura em que a corrente chega a zero. Alguns graus acima gera
`FET_THERMISTOR_OVER_TEMP`. Padrão 120 °C.

## Motor — Termistor do motor (NTC offboard)

### `motor_thermistor.config.enabled`

Liga a proteção térmica do motor por um NTC externo. Mesma lógica de redução do
termistor FET. Útil em uso prolongado com torque alto, porque o motor aquece mais
devagar que os MOSFETs e pode passar do limite sem a placa perceber.

### `motor_thermistor.config.gpio_pin`

GPIO onde o NTC está ligado (em modo analógico). É obrigatório um resistor de
pull-up externo (10 kΩ para 3,3 V): o STM32 não tem pull-up interno em modo
analógico. Sem ele a leitura fica em 0 V e a temperatura parece travada. Esse
GPIO não pode ser usado como botão ou eixo.

### `motor_thermistor.config.temp_limit_lower` e `temp_limit_upper`

Início da redução e corte total, como no termistor FET. O erro gerado é
`MOTOR_THERMISTOR_OVER_TEMP`. Verifique a temperatura máxima na ficha do motor.

### `motor_thermistor.config.poly_coefficient_0` a `_3`

Coeficientes do polinômio que converte a tensão do ADC (0 a 1) em °C. Não
precisam ser calculados à mão: o botão **Calcular coeficientes** gera os quatro a
partir do valor nominal, do beta do NTC e do pull-up.

## Motor — Feed-forward

### `R_wL_FF_enable`

Compensa antecipadamente a queda na resistência e o acoplamento entre eixos da
indutância. Melhora o controle de corrente em rotação alta. Num volante, que gira
devagar, o efeito é pequeno. Depende de R e L corretos.

### `bEMF_FF_enable`

Compensa antecipadamente a força contraeletromotriz. Ajuda a manter o torque
pedido em movimentos rápidos (contraesterço brusco). Depende de
`torque_constant` correto; se estiver errado, piora em vez de ajudar.

## Motor — ACIM (não usado em volante)

### `acim_gain_min_flux`, `acim_autoflux_*`

Parâmetros de controle de fluxo para motor de indução. Ignorados com
`motor_type = HIGH_CURRENT`.

## Encoder — Modo / resolução

### `mode`

Tipo de encoder: incremental ABZ, Hall, SPI absoluto (AS5047, MT6835) etc. Modo
errado dá leitura nula ou erro. Encoders SPI só são inicializados no boot: depois
de trocar o modo, salve e reinicie antes de calibrar.

### `cpr`

Contagens por volta. Incremental: 4 × pulsos por volta (quadratura). SPI: fixo
pelo modelo (AS5047 = 16384). A calibração confere o CPR com os pares de polos;
se não baterem, gera `CPR_POLEPAIRS_MISMATCH`.

### `direction` (somente leitura)

Sentido do encoder em relação às fases, medido na calibração. Editar inverte o
sinal do FOC e o motor oscila ou dispara. Para inverter o sentido do volante para
o jogo, use `axis.invert` na aba FFB Wheel.

### `bandwidth`

Banda da PLL que estima posição e velocidade a partir do encoder, **em rad/s**.
A ferramenta mostra ao lado o equivalente em Hz (÷ 2π): 1000 rad/s ≈ 160 Hz. Desde a v1.1.0, a velocidade usada pelos efeitos damper, friction e
inertia vem desta PLL. Por isso este parâmetro controla diretamente a relação
entre ruído e atraso desses efeitos.

Maior: resposta imediata, mas o ruído de quantização do encoder passa e vira
chiado ou aspereza. Menor: efeitos mais limpos, com alguns milissegundos de
atraso. Encoder de alta resolução (MT6835, AS5047) aguenta valores bem mais altos
que um ABZ de baixa resolução.

Teste prático: com o volante parado e os efeitos zerados, a velocidade no Overlay
deve ficar praticamente em zero. Com uma batida leve no volante, deve reagir sem
atraso visível.

### `pre_calibrated`

Reaproveita o offset calibrado, sem recalibrar no boot. Só funciona se houver
referência de posição após ligar: encoder absoluto ou incremental com índice Z.

### `phase_offset` e `phase_offset_float` (somente leitura)

Ângulo entre o zero do encoder e as fases do motor, medido na calibração de
offset. É o que alinha o campo magnético ao rotor. Editar faz o motor perder
torque ou girar sozinho. Recalibre se suspeitar do valor.

### `enable_phase_interpolation`

Estima a posição entre duas contagens do encoder usando a velocidade. Deixa o
ângulo de comutação mais suave com encoders de baixa resolução. Não reduz o
cogging do motor (esse é magnético). Com MT6835 ou AS5047 a resolução já é alta e
o efeito é desprezível.

## Encoder — Index / offset

### `use_index`

Usa o pulso Z do encoder incremental como referência absoluta. Sem o índice, a
posição de um encoder incremental se perde a cada desligamento.

### `use_index_offset` e `index_offset`

Deslocamento, em voltas, entre o pulso Z e o que o firmware considera zero. Serve
para fazer o centro do volante coincidir com o centro mecânico, em vez do ponto
onde o Z caiu na montagem. O botão **Capturar centro mecânico** da aba Encoder
calcula esse valor automaticamente a partir da posição atual.

### `find_idx_on_lockin_only`

Só aceita o pulso Z durante o giro de busca. Evita que ruído elétrico no fio Z
durante o uso seja confundido com um índice e desloque a referência. Recomendado
com cabos longos.

## Encoder — Calibração

### `calib_range`

Tolerância relativa na checagem de CPR × pares de polos. 0,02 = o giro medido
pode diferir 2 % do esperado. Se o volante montado atrapalha o giro de
calibração, a checagem pode falhar por pouco; aumentar um pouco resolve. Aumentar
muito esconde um CPR ou número de polos realmente errado.

### `calib_scan_distance`

Distância do giro de calibração, em radianos **elétricos** (padrão 16π = 8
voltas elétricas). Mais distância dá uma média melhor, porém leva mais tempo e
precisa de mais espaço livre de rotação. O botão Pular do Passo 10 divide esse
valor por 4.

### `calib_scan_omega`

Velocidade do giro de calibração em rad/s elétricos. Rápido demais: o rotor não
acompanha o campo e a medida sai errada.

## Encoder — Hall (não usado em volante)

### `hall_polarity`, `hall_polarity_calibrated`, `ignore_illegal_hall_state`

Configuração de sensores Hall. Sem uso com encoder incremental ou SPI.

## Encoder — Pinos

### `abs_spi_cs_gpio_pin`

GPIO de chip select do encoder SPI. Precisa bater com a ligação física. Valor
errado dá `ABS_SPI_COM_FAIL`. O painel de diagnóstico do encoder ajuda a
investigar.

### `sincos_gpio_pin_sin` e `sincos_gpio_pin_cos` (não usado em volante)

Pinos de encoder analógico seno/cosseno.

## Encoder — Ferramentas da aba

- **Diagnóstico do Encoder**: leitura ao vivo das contagens e erros. Útil para
  investigar `ABS_SPI_FAILED` e ruído.
- **Capturar centro mecânico**: desarma o motor. Você move o volante até o centro
  e o `index_offset` é calculado e gravado.
- **Painel MT6835** (aba Debug): lê status, grava o zero dentro do próprio
  encoder e programa a EEPROM dele.

## Controller — Modo de controle

### `control_mode`

Torque, velocidade ou posição. O FFB usa **torque**: o torque calculado vai
direto para o controle de corrente, sem malhas de velocidade ou posição no meio.
As ferramentas de calibração trocam o modo temporariamente e restauram no final.

### `input_mode`

Como o valor de entrada é tratado antes do controle: direto (PASSTHROUGH),
rampa de velocidade, filtro de posição, trajetória trapezoidal etc. O FFB usa
PASSTHROUGH. Qualquer filtro aqui atrasaria o FFB.

### `input_filter_bandwidth`

Banda do filtro de entrada. Só atua em `input_mode = POS_FILTER`.

### `torque_ramp_rate`

Taxa máxima de variação do torque em Nm/s. Só atua em `input_mode =
TORQUE_RAMP`. Para suavizar o FFB, use `axis.maxtorquerate`, que atua dentro da
cadeia do FFB.

### `vel_ramp_rate`

Aceleração máxima em `input_mode = VEL_RAMP`. Usado pelas ferramentas que giram
o motor em velocidade.

## Controller — Ganhos (PID)

Em modo torque, os ganhos desta seção são ignorados. Eles só importam para
calibrações e testes que usam velocidade ou posição: anticogging, Performance
Test e Auto-tune PI.

### `pos_gain`

Ganho da malha de posição, em (voltas/s)/volta: para cada volta de erro, pede
essa velocidade. Alto demais: o motor oscila em volta do alvo. Baixo demais:
chega devagar. A calibração nativa de anticogging depende muito deste valor.

### `vel_gain`

Ganho proporcional da malha de velocidade, em Nm/(volta/s). Define quanto torque
é aplicado por unidade de erro de velocidade. Em velocidade muito baixa, a força
disponível é limitada por `vel_gain × velocidade_alvo`. Por isso, girar devagar
com o cogging do motor gera trancos (stick-slip).

### `vel_integrator_gain`

Ganho integral da malha de velocidade. Elimina erro de velocidade que persiste
(atrito constante, carga). Alto demais gera oscilação lenta. O Auto-tune PI
(seção Anticogging desta aba) calcula este valor e `vel_gain` a partir da
inércia e do atrito medidos no Performance Test.

### `vel_integrator_limit`

Limite do integrador em Nm. Evita que ele acumule um valor enorme quando o motor
está bloqueado. `inf` = sem limite.

### `inertia`

Inércia usada como alimentação antecipada de aceleração em trajetórias, em
Nm/(volta/s²). Não tem relação com o efeito `axis.axisinertia` do FFB e não atua
em modo torque.

### `enable_gain_scheduling` e `gain_scheduling_width`

Reduz os ganhos quando o erro de **posição** é menor que a largura configurada,
proporcionalmente ao erro. Evita que o motor fique "caçando" o alvo em modo
posição. Só atua em modo posição.

## Controller — Limites de velocidade

### `enable_vel_limit`

Liga o limite de velocidade. Nos modos velocidade e posição, a velocidade pedida
é limitada.

### `enable_torque_mode_vel_limit`

Aplica o limite de velocidade também em modo torque: o torque vai sendo reduzido
conforme a velocidade se aproxima do limite. É uma segurança contra o volante
disparar solto (por exemplo, FFB invertido). O custo: se `vel_limit` estiver
baixo, corta o FFB em contraesterços rápidos. Se ligar, use um `vel_limit` alto.

### `vel_limit`

Velocidade máxima em voltas/s. Num volante, 10 voltas/s (600 RPM) nunca é
atingido pelas mãos e ainda funciona como segurança.

### `vel_limit_tolerance`

Múltiplo de `vel_limit` que dispara o erro de sobrevelocidade. 1,2 = 20 % acima.

### `enable_overspeed_error`

Desarma com `OVERSPEED` quando a velocidade passa de `vel_limit ×
vel_limit_tolerance`. Protege contra perda de controle. Com limite alto, não
atrapalha o uso normal.

## Controller — Posição circular (não usado em volante)

### `circular_setpoints`, `circular_setpoint_range`, `steps_per_circular_range`

Posição que dá a volta (0–1 volta) em vez de acumular. Para mecanismos de rotação
contínua. O volante tem curso limitado e usa posição acumulada.

## Controller — Homing / mirror (não usado em volante)

### `homing_speed`, `load_encoder_axis`, `axis_to_mirror`, `mirror_ratio`, `torque_mirror_ratio`

Busca de fim de curso, encoder da carga em outro eixo e espelhamento de outro
eixo. Sem uso num eixo único de volante.

## Controller — Power monitoring

### `electrical_power_bandwidth` e `mechanical_power_bandwidth`

Filtros das estimativas de potência elétrica e mecânica, em rad/s (a ferramenta
mostra o equivalente em Hz ao lado). Valores menores ignoram picos curtos. Essas estimativas alimentam a detecção de spinout.

### `spinout_electrical_power_threshold` e `spinout_mechanical_power_threshold`

Spinout é quando a placa gasta potência elétrica mas a potência mecânica é
negativa: sinal de que o encoder escorregou ou o offset está errado, e o motor
está "brigando" com o próprio campo. Ultrapassar os dois limiares dispara
`SPINOUT_DETECTED`.

O original do ODrive usa 10 W / −10 W, que disparava com FFB forte. Este firmware
usa 50 W / −50 W. Se ainda disparar em uso normal pesado, aumente com cuidado.
Se disparar em uso leve, suspeite do encoder.

## Controller — Anticogging

O cogging é a ondulação de torque causada pela atração entre os ímãs e os dentes
do estator. O anticogging guarda um mapa de 3600 posições por volta com o torque
que compensa essa ondulação, e soma o valor da posição atual ao torque do
motor. O mesmo valor vale nos dois sentidos de giro. O mapa pode ser gerado pela
calibração nativa ou pela captura em velocidade, as duas nesta seção.

**A seção inteira é avançada:** fica escondida ao abrir a ferramenta e aparece
com o botão de opções avançadas da aba Debug. Além dos campos abaixo, ela tem
quatro painéis:

- **Rodar Anticogging Calibration**: a calibração nativa do ODrive (posição a
  posição). Mostra o progresso e o estado do mapa.
- **Auto-tune Velocity PI**: calcula `vel_gain` e `vel_integrator_gain` a partir
  de J e b medidos no Performance Test, e verifica com resposta a degrau. Deve
  ser rodado antes da captura via host.
- **Anticogging via host**: gira o motor devagar nos dois sentidos, em modo
  velocidade, e mede o Iq por posição via HID. Monta o mapa com mediana por
  posição e média entre ida e volta (cancela o atrito). Não grava nada, só
  analisa. Limitação conhecida: em velocidade muito baixa (padrão 1,5 RPM), a
  malha de velocidade não tem força para vencer o cogging e o giro fica aos
  trancos; 5–7,5 RPM tendem a dar dados melhores.
- **Aplicar mapa ao motor**: grava o mapa escolhido nas 3600 posições do
  anticogging do ODrive, ativa `pre_calibrated` e `anticogging_enabled`, salva e
  reinicia.

### `anticogging.anticogging_enabled`

Liga a aplicação do mapa durante o uso. Desligado, o mapa fica guardado mas não
é somado.

### `anticogging.pre_calibrated`

Indica que existe um mapa válido salvo e ativa o uso dele no boot. Sem isso, o
mapa não é aplicado mesmo com `anticogging_enabled`.

### `anticogging.cogging_ratio`

Existe na configuração, mas nesta versão não tem efeito: o mapa sempre usa 3600
posições por volta mecânica.

### `anticogging.calib_pos_threshold` e `calib_vel_threshold`

Critérios da calibração nativa: em cada uma das 3600 posições, o firmware espera
o erro de posição e a velocidade ficarem abaixo destes limites (em contagens do
encoder) para gravar o valor. Não há limite de tempo por posição. Limites muito
apertados com encoder de alta resolução fazem a calibração demorar horas ou
travar. Limites muito largos gravam valores imprecisos.

### `anticogging.index`

Posição atual da calibração nativa (0–3599). Serve como indicador de progresso.
Não deve ser editado.

---

# ◯ Force Feedback

## 🗂 Perfis

Salva e carrega conjuntos de parâmetros FFB como arquivos JSON numa pasta
escolhida. O mesmo formato é usado pelo plugin do SimHub, que pode trocar de
perfil automaticamente por jogo.

- **Escolher pasta / Recarregar**: define e relê a pasta de perfis.
- **Aplicar no volante**: escreve os valores do perfil na placa, **sem gravar na
  memória permanente**. Dá para testar à vontade; reiniciar a placa volta ao que
  estava salvo. Para manter, use Save.
- **Salvar atual como / Sobrescrever**: grava os valores atuais da placa num
  arquivo novo ou no selecionado.
- **Renomear, Excluir, Exportar, Importar**: gerenciam os arquivos.

## FFB Wheel — Escala de força

### `axis.range` (Rotation range)

Ângulo total de rotação, de batente a batente, em graus. Define a escala de
posição enviada ao jogo e onde começa o batente eletrônico. Use o mesmo valor
configurado no jogo; se forem diferentes, o volante virtual e o físico ficam
desalinhados.

### `axis.maxtorque` (Maximum torque)

Torque em Nm que corresponde a 100 % do FFB. Define a escala de tudo: jogo,
efeitos sempre-ativos e batente. Limitado a 40 Nm pelo firmware e, na prática,
por `current_lim × torque_constant`. É o "teto" físico do conjunto e deve ser
ajustado uma vez, pensando no motor e na segurança.

### `axis.fxratio` (Torque multiplier)

Multiplicador final entre 0 e 1. Matematicamente tem o mesmo efeito que reduzir
`maxtorque`: os dois multiplicam o mesmo valor final. A diferença é de uso:
`maxtorque` fica fixo como teto e `fxratio` é o ajuste do dia a dia, com mais
resolução (0,001).

Também reduz os efeitos sempre-ativos e o batente. O firmware inicia em 1,0.

### `axis.invert` (Axis position invert)

Inverte só a posição enviada ao jogo. Use quando virar para a direita aparece
como esquerda no jogo. Não mexe no torque.

### `axis.ffbinvert` (FFB torque invert)

Inverte só o torque recebido do jogo. Use quando o volante empurra para o lado
errado (sai do centro em vez de voltar), o que acontece com algumas ferramentas
intermediárias de FFB. Independente de `axis.invert`: cada situação pode precisar
de uma, das duas ou de nenhuma.

## FFB Wheel — Efeitos sempre ativos

Efeitos calculados pela própria placa, somados ao FFB do jogo. Funcionam com ou
sem jogo aberto. Desde a v1.1.2 usam as mesmas constantes e filtros do
OpenFFBoard original.

### `axis.idlespring` (Idle Spring)

Mola de centralização que atua **só quando nenhum jogo está enviando FFB**.
Mantém o volante centrado fora do jogo e some quando o jogo assume. Não interfere
na sensação durante a corrida.

### `axis.axisdamper` (Axis damper)

Resistência proporcional à velocidade: quanto mais rápido gira, mais pesa. Dá
peso ao volante e amortece oscilações (por exemplo, o volante balançando sozinho
em reta). Excesso deixa o volante "pastoso" e mascara detalhes.

### `axis.axisinertia` (Axis inertia)

Resistência proporcional à aceleração: simula massa extra. Pesa ao mudar de
direção rapidamente, mas não freia quando já está girando.

Antes da v1.1.2, valores acima de ~50 geravam chiado no motor por falta de
filtro. Continua dependendo da qualidade da velocidade (ver
`encoder.config.bandwidth`): se chiar, reduza o valor ou a banda do encoder.

### `axis.axisfriction` (Axis friction)

Atrito constante, com o sinal da velocidade: a mesma força para qualquer
velocidade. Perto do zero, a força cresce suavemente numa faixa de ~91 graus/s,
sem degrau ao sair do repouso. Imita o atrito de uma coluna de direção real.

### `axis.esgain` (EndStop gain)

Força da mola do batente eletrônico, aplicada quando o volante passa de ±range/2.
Um volante direct-drive não tem batente mecânico, então é essa mola que impede
que ele passe do ângulo configurado. Maior: batente mais duro.

### `axis.esdamp` (EndStop damper)

Amortecimento do batente, independente da mola. Absorve a energia da batida para
o volante não ricochetear para o outro lado. Mola forte com amortecimento fraco
= batente duro que rebate. Mola fraca com amortecimento forte = batente macio
que "segura".

## FFB Wheel — Slew rate / curva

### `axis.maxtorquerate` (Max torque rate)

Limita a variação do torque por milissegundo, na escala interna (32767 = 100 %
do torque). 327 ≈ 1 % por ms, ou seja, de 0 a 100 % em 100 ms. Suaviza trancos
secos, mas também achata impactos e texturas. 0 = desligado (recomendado; use o
EQ ou os filtros se quiser suavizar).

### `axis.expo` e `axis.exposcale`

Curva aplicada à **posição do volante enviada ao jogo** (não ao torque):
posição^(expo/exposcale + 1). Positivo = centro menos sensível (é preciso girar
mais para o jogo ver movimento perto do centro). Negativo = centro mais sensível.
0 = linear.

A curva também afeta a mola de centralização (`idlespring`), que usa a mesma
posição. `exposcale` só define a escala do número (100 = expo em centésimos).

## ✦ FFB Effects

### `fx.master`

Ganho global do jogo (0–255), correspondente ao "Device Gain" do HID. **Não é
persistente:** o jogo sobrescreve pelo slider de força dele assim que conecta, e
o firmware volta a 255 a cada boot. Para ajustar força de forma permanente, use
`axis.maxtorque` e `axis.fxratio`.

### `fx.spring`

Ganho dos efeitos de mola **enviados pelo jogo** (0–255). Alguns jogos usam mola
para centralizar em menus ou para simular pneu parado. Não confunda com
`axis.idlespring`.

### `fx.damper`

Ganho dos efeitos de amortecimento enviados pelo jogo. Só atua se o jogo usar
esse tipo de efeito (muitos simuladores enviam tudo como força constante).

### `fx.friction`

Ganho dos efeitos de atrito enviados pelo jogo.

### `fx.inertia`

Ganho dos efeitos de inércia enviados pelo jogo. São independentes dos efeitos
sempre-ativos da aba FFB Wheel.

## ≋ FFB Filters

### `fx.filterCfFreq` e `fx.filterCfQ`

Filtro passa-baixa sobre a força constante, que é o sinal principal de FFB da
maioria dos simuladores. Frequência de corte em Hz e Q em centésimos (70 ≈
Butterworth, sem pico).

Corte baixo: suaviza, mas também remove detalhes de pista e atrasa a resposta.
500 Hz praticamente não filtra. O gráfico de resposta da aba mostra o efeito
comparado com a resposta mecânica medida no Performance Test.

### `fx.filterFrFreq` e `fx.filterFrQ`

Filtro sobre a saída dos efeitos de atrito do jogo. Remove oscilação quando o
sinal de velocidade é ruidoso.

### `fx.filterDaFreq` e `fx.filterDaQ`

Filtro sobre os efeitos de amortecimento do jogo. Mesma função.

### `fx.filterInFreq` e `fx.filterInQ`

Filtro sobre os efeitos de inércia do jogo. O corte padrão é baixo porque o sinal
de aceleração é o mais ruidoso.

### EQ por banda (WEIGHT / CHASSIS / ROAD)

Três filtros em série sobre a força do jogo, de −12 a +12 dB cada:

- **WEIGHT** (low-shelf 5 Hz): baixas frequências, como transferência de peso e
  escorregamento lento.
- **CHASSIS** (peak 12 Hz): faixa de suspensão e rolagem da carroceria.
- **ROAD** (high-shelf 25 Hz): textura de asfalto e zebras.

A força sustentada numa curva não muda com o EQ: a cadeia é normalizada para
ganho 1 em 0 Hz. Por isso dá para ajustar o caráter do FFB sem recalibrar o
torque máximo. As mudanças valem na hora e são gravadas pelo Save (os valores
são restaurados no boot desde a correção desta versão). **Reset flat** zera as
três bandas.

## ◔ FFB Live

Monitoramento do FFB enquanto o jogo roda:

- **Estado FFB**: se o jogo ativou o FFB e quantos efeitos estão ativos.
- **Contadores HID OUT** e **Taxa de atualização**: quantos comandos o jogo envia
  por segundo. Taxa baixa indica gargalo no jogo ou no USB.
- **Efeitos ativos**: tipo e parâmetros de até 3 efeitos.
- **Análise de dinâmica**: estatísticas da magnitude do efeito 0.
- **Picos do barramento DC**: tensão e corrente mínimas e máximas desde o último
  reset. Útil para dimensionar fonte e freio.
- **Gráfico ao vivo**: torque pedido e posição.

## ⚒ FFB Force Test (WebHID)

Envia efeitos HID direto do navegador, como se fosse um jogo. Testa a cadeia
completa (HID → cálculo de efeitos → motor) sem jogo. Serve para separar
problemas: se funciona aqui e não no jogo, o problema está na configuração do
jogo ou em ferramentas intermediárias.

---

# ⌨ Inputs

## Modos por GPIO

Os GPIOs 1–4 (com ADC) e 6 (só digital) viram entradas do joystick HID:

- **Desligado**: pino livre.
- **Botão**: entrada digital. Escolha o número do botão (0–63).
- **Eixo**: tensão analógica (pedal, freio de mão). Escolha o eixo e calibre
  mínimo e máximo. Só GPIOs 1–4.
- **Termistor (NTC)**: reserva o pino para o termistor do motor (configuração na
  aba Motor).
- **Zero Wheel**: um botão que, ao ser pressionado, define a posição atual como
  centro. Não grava sozinho; para manter após reiniciar, use Save.

## Processamento de eixo analógico

Vale para todos os GPIOs em modo Eixo:

- **Filtro passa-baixa** (com corte em Hz): elimina tremor do ADC e
  interferência. Corte baixo demais atrasa o pedal.
- **Autocalibrar mín/máx**: ajusta os limites conforme o pedal é movido. Após
  pressionar o pedal até o fim uma vez, a faixa fica completa.

---

# ⚒ Tools

## Debug / Status

- **Info do Device**: tensão, corrente, estado do eixo, flags de calibração,
  posição, velocidade, correntes e temperaturas, com atualização automática
  opcional.
- **Ações**: calibração de motor, busca de índice, calibração de encoder,
  calibração completa, malha fechada, lockin, salvar NVM, apagar + reiniciar,
  reiniciar, limpar erros.
- **Erros**: registradores de erro decodificados bit a bit.
- **Gráfico ao vivo**: tensão, corrente do barramento, Iq e corrente de freio
  nos últimos 60 s.

## Console

Terminal serial com a placa. Aceita comandos ASCII do ODrive (`r`, `w`, `ss`,
`sr`, `sc`) e comandos do OpenFFBoard (`axis.*`, `fx.*`, `sys.*`). A lista
**Comandos disponíveis** tem busca e descrição de cada um. As setas ↑/↓ navegam
no histórico.

## DFU Flash

Grava o firmware pelo navegador, sem instalar ferramentas: reinicia a placa no
bootloader do STM32, detecta, seleciona o `.bin` (arquivo local ou busca da
versão no GitHub) e grava. Os parâmetros FFB podem ser preservados.

## Overlay

Janela sempre no topo (Picture-in-Picture) com telemetria a 1 kHz via HID, sem
disputar a serial com o jogo. Funciona com o jogo em modo janela ou borderless.

- **Janela (s)**: quanto tempo o gráfico mostra (padrão 60 s, até 1200 s). As
  médias de potência continuam em 60 s.
- **Painéis**: barramento DC, volante (torque e posição) e espectro (FFT do
  torque pedido comparado com o Iq). Desligar um painel dá o espaço dele aos
  outros.
- **Séries do barramento**: tensão, corrente do barramento, Iq, corrente de
  freio, temperatura dos FETs e do motor.
- **Indicadores**:
  - P brake: potência média no resistor.
  - P motor: potência mecânica e perda no cobre.
  - Clip OUT: % do tempo com corrente no limite. Indica que o motor não está
    entregando o que o FFB pede.
- **Botão ⏸/▶**: congela e retoma a aquisição.
- **Logger**: grava todos os sinais HID a 1 kHz em CSV compactado
  (~40 MB/hora), escrevendo em disco continuamente.

## Performance Test

Mede o conjunto motor + volante para ajustar com dados em vez de tentativa e
erro. Todos os testes exigem o volante livre e sem as mãos.

- **Performance Test**: aplica força constante e mede pico de velocidade,
  aceleração, atrito de partida e **inércia (J)**. Detecta saturação do motor.
- **Salvar / carregar resultado**: guarda J e os dados do hardware em JSON,
  para uso nos outros testes e comparação entre montagens.
- **Coastdown**: gira, solta e mede a desaceleração até parar. O ajuste
  exponencial dá o amortecimento viscoso (b).
- **Frequency Sweep**: aplica torque senoidal de 0,3 a 100 Hz e mede a resposta
  real. Sobrepõe ao modelo teórico (J + b) no gráfico da aba FFB Filters.
  Diferenças apontam ressonâncias ou limite de banda.
- **Análise e sugestões**: sugere banda de corrente, filtros e outros ajustes a
  partir dos testes, com botão para aplicar e gráfico de prévia.

As ferramentas de anticogging (Auto-tune PI, captura via host e aplicar mapa)
ficam na aba Controller, seção Anticogging (avançada).
