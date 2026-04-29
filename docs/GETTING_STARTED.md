# Getting Started — Odrive-Wheel

Existem três rotas pra colocar o firmware na MKS XDrive Mini:

- **Rota A — dfu-util (CLI)** — baixa o `.bin` pronto e grava via linha de comando. Necessária na **primeira gravação**, ou se algo der errado.
- **Rota A2 — Web Flasher (navegador)** — depois que o firmware Odrive-Wheel já estiver na placa, você pode atualizar pelo próprio HTML config tool, sem dfu-util. Mais simples no dia-a-dia.
- **Rota B — Compilar do código (VS Code)** — clona o repo, builda e grava. Pra quem quer modificar.

Depois do firmware gravado, a seção **"Primeira vez ligando o motor com segurança"** é **obrigatória** — não pule, mesmo na rota A. ODrive sem calibração ou com `brake_resistance` errado pode danificar a fonte ou o resistor de freio.

---

## Hardware esperado

- Placa **MKS XDrive Mini** (clone do ODrive v3.6, STM32F405)
- Motor **BLDC** (3 fases — sem hall, sem driver de passo)
- Encoder **incremental ABZ** (Z opcional mas recomendado pra sim racing)
- Fonte **12 a 48 V** (Atenção a versão de tensão da sua MKS Xdrive Mini)
- **Resistor de freio** conectados nos terminais AUX (tipicamente 2 Ω 50 W)
- Cabo **USB-C** pra dados (não só carga!)

---

## Rota A — Apenas gravar firmware pronto

### 1. Baixar o binário

Página de Releases: https://github.com/eagabriel/OpenffboardOdrive/releases

Baixe `odrive-wheel.bin` da última release.

### 2. Instalar dfu-util

- **Windows**: baixe `dfu-util-0.11-binaries.tar.xz` em [dfu-util.sourceforge.net](http://dfu-util.sourceforge.net/releases/), extraia e adicione ao PATH. Pode também vir junto com o STM32CubeProgrammer.
- **Linux**: `sudo apt install dfu-util`
- **macOS**: `brew install dfu-util`

Confirme: `dfu-util --version` (versão ≥ 0.10).

### 3. Colocar a placa em DFU mode

1. Desligue a fonte.
2. Mantenha o **botão BOOT0** pressionado (alguns clones têm jumper — feche o jumper BOOT0).
3. Conecte o USB-C ao PC.
4. Solte o BOOT0.

Alternativamente pressione BOOT0 e de um pulso no botão reset

No Windows, pode ser preciso instalar o driver **WinUSB** uma vez via [Zadig](https://zadig.akeo.ie/) — selecione o device `STM32 BOOTLOADER` e troque o driver pra `WinUSB`.

Confirme: `dfu-util -l` deve listar `[0483:df11]`.

### 4. Gravar

```bash
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D odrive-wheel.bin
```

Saída esperada termina com `Done!` e a placa reboota sozinha. Pronto — pula direto pra **Primeira vez ligando o motor com segurança**.

---

## Rota A2 — Atualizar firmware pelo navegador (Web Flasher)

> ⚠️ Esta rota só funciona **depois** que o firmware Odrive-Wheel já estiver gravado na placa pela primeira vez (Rota A ou B). É pra atualizações futuras.

A ferramenta `tools/odrive-wheel.html` tem uma aba **"DFU Flash"** que faz tudo dentro do navegador: reinicia a placa em modo DFU, detecta o bootloader, escolhe o `.bin` e grava — sem dfu-util e sem precisar pressionar BOOT0.

### Pré-requisitos (uma vez por máquina)

- **Chrome ou Edge atualizado** (precisa de WebUSB + Web Serial).
- **No Windows**: instalar driver **WinUSB** no bootloader STM32 via [Zadig](https://zadig.akeo.ie/):
  1. Coloque a placa em DFU mode uma vez (Rota A passo 3, ou rode `sd` pelo Console se já tem firmware Odrive-Wheel).
  2. Abra Zadig → menu **Options → List All Devices**.
  3. Selecione **STM32 BOOTLOADER** no dropdown.
  4. Driver alvo: **WinUSB** → clique **Replace Driver**.
  5. Aguarde "Driver installed" e feche.

  Isso é uma vez só por máquina. Depois disso o driver fica permanentemente. (Não confunda com o dispositivo CDC `Odrive-Wheel CDC` — esse continua usando o driver padrão do Windows e funciona com Web Serial sem mexer em nada.)

### Passo a passo

1. Abra `Odrive-Wheel/tools/odrive-wheel.html` no Chrome/Edge.
2. Conecte normalmente à placa pela serial (botão **Conectar** no header).
3. Vá na aba **DFU Flash** (sidebar, embaixo em "Tools").
4. **Step 1 — Reiniciar em DFU**: clica no botão. A ferramenta envia `sd` pela serial e a placa reboota direto pro bootloader STM32. Aguarde ~2 s.
5. **Step 2 — Procurar bootloader**: clica em "Procurar bootloader". O navegador abre um diálogo pedindo permissão pra acessar o `STM32 BOOTLOADER` — aprove. O badge fica verde com `✓` e mostra `VID:0x0483 PID:0xDF11`.
6. **Step 3 — Selecionar firmware**: clica em "Escolher arquivo" e seleciona o `odrive-wheel.bin` (do Release, ou do `Odrive-Wheel/build/` se você compilou).
7. **Step 4 — Gravar firmware**: clica em "Gravar firmware". Apaga setores S0–S9 (256 KB onde fica o app), grava em chunks de 1 KB (~30–60 s no total) e a placa reboota sozinha rodando o firmware novo.

O Log de operações no fim da página mostra cada etapa em tempo real. A barra de progresso vai de 0 a 100%.

### Troubleshooting

| Problema | Solução |
|---|---|
| Step 2 dá "No device selected" | Step 1 não funcionou, ou Zadig/WinUSB não foi instalado. Vá em DFU pelo BOOT0 + tente de novo. |
| Step 4 falha em "DFU status=0x..." | Bootloader em estado de erro — desconecta o cabo USB, religa em DFU mode (BOOT0) e tenta o Step 4 sem o Step 1. |
| Placa não volta a aparecer como CDC depois do flash | Aguarde 5–10 s; algumas vezes o Windows demora pra re-enumerar. Se persistir, desconecta/reconecta o USB. |
| WebUSB pede permissão toda vez | Normal — é uma proteção do Chrome. Cada arquivo `.html` em `file://` é tratado como uma origem nova. |

### EEPROM (configurações) é preservada

O Web Flasher só apaga os setores S0–S9 (firmware). Os setores **S10 e S11** (onde estão suas configurações FFB salvas — gain, filtros, range, idlespring, etc.) **NÃO são tocados**. Você não perde calibração nem ajustes ao atualizar.

> ⚠️ Mas o Save da ODrive (NVM em S1) **fica dentro** do range que apagamos. Se atualizar o firmware, os parâmetros ODrive (motor, encoder, controller, brake_resistance) voltam aos defaults. Sempre exporte o JSON pelo botão **Export** antes de atualizar, e reimporte depois.

---

## Rota B — Compilar do código (VS Code)

### 1. Pré-requisitos

| Ferramenta | Onde pegar | Notas |
|---|---|---|
| **arm-none-eabi-gcc** 12.x | [Arm GNU Toolchain](https://developer.arm.com/Tools%20and%20Software/GNU%20Toolchain) | Adicionar ao PATH |
| **make** | Windows: [chocolatey](https://chocolatey.org) `choco install make` ou MSYS2 | Linux/Mac já vêm com |
| **Git** | [git-scm.com](https://git-scm.com/) | — |
| **dfu-util** ≥ 0.10 | ver Rota A | Pra `make flash-dfu` |
| **VS Code** | [code.visualstudio.com](https://code.visualstudio.com/) | — |

Verifique:
```bash
arm-none-eabi-gcc --version    # deve dizer 12.x
make --version
git --version
dfu-util --version
```

### 2. Clonar com submódulo

O OpenFFBoard fica em submódulo Git, então use `--recurse-submodules`:

```bash
git clone --recurse-submodules https://github.com/eagabriel/OpenffboardOdrive.git
cd OpenffboardOdrive
```

Se já clonou sem submódulos:
```bash
git submodule update --init --recursive
```

### 3. Extensões recomendadas no VS Code

Abra o VS Code na pasta do repo e instale:

- **C/C++** (`ms-vscode.cpptools`) — IntelliSense
- **Makefile Tools** (`ms-vscode.makefile-tools`) — atalhos de build
- **Cortex-Debug** (`marus25.cortex-debug`) — opcional, pra debug com ST-Link

### 4. Compilar

Pelo terminal integrado do VS Code (Ctrl+Shift+`):

```bash
cd Odrive-Wheel
make -j4
```

Build leva ~1 min na primeira vez. Saída em `build/odrive-wheel.bin` (~400 KB).

Erros comuns:
- `arm-none-eabi-gcc: command not found` → toolchain não está no PATH
- `fatal error: tusb.h: No such file` → submódulo não inicializado, rode `git submodule update --init --recursive`

### 5. Gravar

Coloque a placa em DFU mode (ver Rota A passo 3) e:

```bash
make flash-dfu
```

Equivalente a:
```bash
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D build/odrive-wheel.bin
```

---

## Primeira vez ligando o motor com segurança

> ⚠️ **Antes de ligar a fonte:**
> - **Desacople o motor do volante.** A primeira calibração faz o motor girar — se ele estiver no eixo do volante, vai bater no batente físico.
> - **Confirme o resistor de freio fisicamente conectado** nos terminais AUX. Sem ele, qualquer regen vai pra fonte e provavelmente desarma a OVP da PSU.



### 1. Conectar a ferramenta de configuração

1. Abra `Odrive-Wheel/tools/odrive-wheel.html` no **Chrome ou Edge** (Web Serial só funciona nesses dois).
2. Clique em **Conectar** e escolha a porta serial da placa (`Odrive-Wheel CDC` ou similar).
3. O status deve ficar verde ("Conectado").

### 2. Configuração mínima — alimentação e proteções

Aba **ODrive**:

| Campo | Valor sugerido | Por quê |
|---|---|---|
| `brake_resistance` | **2.0** (Ω) | Casa com o resistor físico — ERRADO aqui = trava ou queima |
| `enable_brake_resistor` | **true** | Liga o controle ativo do regen |
| `dc_max_positive_current` | **20** (A) | Limite de corrente da fonte → motor |
| `dc_max_negative_current` | **-5** (A) | Quanto pode voltar pro brake (negativo!) |
| `dc_bus_overvoltage_trip_level` | **28** (V) | Acima disso desarma — protege a fonte (coloque 4V acima da tensão da sua fonte) |
| `dc_bus_undervoltage_trip_level` | **8** (V) | Evita brown-out |
| `max_regen_current` | **0** (A) | Threshold pro brake virar dump load | (Significa que toda corrente de regeneração vai para o resistor, mais seguro pra começar.

### 3. Configuração mínima — motor

Aba **Motor** (primeiro a calibrar; valores conservadores):

| Campo | Valor sugerido |
|---|---|
| `motor_type` | **0** (HIGH_CURRENT) |
| `pole_pairs` | depende do motor — conte os ímãs do rotor e divida por 2 (típico 7) |
| `torque_constant` | **0.87 / Nm/A** |
| `current_lim` | **10** (A) — começa baixo! |
| `calibration_current` | **5** (A) |
| `resistance_calib_max_voltage` | **12.0** (V) |
| `pre_calibrated` | **false** |

### 4. Configuração mínima — encoder

Aba **Encoder**:

| Campo | Valor sugerido |
|---|---|
| `mode` | **0** (INCREMENTAL) |
| `cpr` | **linhas do encoder × 4** (ex: 1000 linhas → 4000) |
| `use_index` | **true** se Z conectado, senão false |
| `pre_calibrated` | **false** |

### 5. Configuração mínima — controller (modo FFB)

Aba **Controller**:

| Campo | Valor sugerido |
|---|---|
| `control_mode` | **1** (TORQUE_CONTROL) — obrigatório pra FFB |
| `input_mode` | **1** (PASSTHROUGH) |
| `vel_limit` | **8** (turns/s) — corte de segurança |

Aba **Axis 0**:

| Campo | Valor sugerido |
|---|---|
| `startup_motor_calibration` | **false** (calibra manual primeiro) |
| `startup_encoder_offset_calibration` | **false** |
| `startup_closed_loop_control` | **false** (vamos ligar depois) |

### 6. Salvar e calibrar

1. Clique em **Salvar** (botão verde no header).
   - O Save faz: persiste FFB EEPROM → grava configs ODrive → reboota a placa.
2. Após o reboot, reconecte.
3. Aba **Debug / Status**, na seção "Ações":
   - **Motor calibration** (`w axis0.requested_state 4`) — motor faz "beep" e mede R/L. Verifique em **Live monitor** que `motor.config.phase_resistance` ficou entre 0.05–1.0 Ω e `phase_inductance` entre 1e-5 e 5e-4 H. Se ficou fora, motor mal conectado ou `pole_pairs` errado.
   - Se OK, vá no campo `motor.config.pre_calibrated` → **true** + Save.
   - **Encoder offset calibration** (`w axis0.requested_state 7`) — motor gira ~1 volta lento, mede o offset. Verifique `encoder.config.offset` ≠ 0 e `axis0.error` = 0.
   - Se OK, `encoder.config.pre_calibrated` → **true** + Save.
4. Reboota.

### 7. Primeiro teste em closed loop (sem FFB ainda)

1. Aba **Debug / Status** → ação **Closed loop** (`w axis0.requested_state 8`).
2. Tente girar o eixo com a mão — ele deve **resistir** (seguindo torque comando = 0). Isso confirma que o controle fechou.
3. Se gira solto, calibração ruim. Se trava com vibração, `pole_pairs` ou `cpr` errado.
4. Tudo OK → desarma com `w axis0.requested_state 1` (IDLE).

### 8. Configuração mínima — FFB

Aba **FFB Wheel**:

| Campo | Valor sugerido pro 1º teste |
|---|---|
| `range` | **900** (graus de batente a batente) |
| `maxtorque` | **2.0** (Nm) — **começa baixo!** Vai girar puxando, não quer surpresa |
| `fxratio` | **1.0** (100%) |
| `idlespring` | **5** (mola de centro quando jogo não envia FFB) |

Aba **FFB Effects**:

| Campo | Valor |
|---|---|
| `master` | **255** (100%) |

Save → reboot.

### 9. Validar FFB

1. Use algum app como o Forcetest presente dentro de docs/Tools para testar cada efeito.
2. Aba **Test Forces** (se disponível).
3. Aba **FFB Live** na ferramenta web — você verá `HID OUT counter` subindo e os efeitos ativos.

Se sentir torque proporcional ao input → FFB OK. Suba `maxtorque` aos poucos (2 → 4 → 6 Nm)

---

## Checklist final antes de jogar

- [ ] Motor montado no eixo, eixo travado mecanicamente (não fica dando volta infinita)
- [ ] Resistor de freio em local ventilado (pode esquentar a 60–80 °C em uso, isso é normal)
- [ ] No jogo, **comece com FFB strength baixo** (50%) e ajuste pra cima
- [ ] Volte aos valores conservadores se sentir vibração ou ruído alto
- [ ] Faça um export de suas configurações que funcionam antes de começar a alterar outras.

---

## Onde pedir ajuda

- Issues do projeto: https://github.com/eagabriel/Odrive-Wheel/issues
- Discord do OpenFFBoard