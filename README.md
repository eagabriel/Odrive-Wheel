# ODrive FFB Wheel — Firmware MKS Mini

Firmware customizado pra ODrive v0.5.6 rodando em hardware **MKS Mini ODrive S** (clone STM32F405),
adicionando suporte completo a **Force Feedback HID** pra usar o motor como volante de simulador.

Baseado em:
- [ODrive Firmware v0.5.6](https://github.com/odriverobotics/ODrive) (motor control)
- [OpenFFBoard](https://github.com/Ultrawipf/OpenFFBoard) (FFB stack: HidFFB + EffectsCalculator)

## Estrutura

```
.
├── Firmware-V56-Stock/         ← Projeto principal (CDC + HID composite + FFB)
│   ├── src/                    ← Sources locais (USB, FFB bridge, cmd_table)
│   ├── inc/                    ← Headers locais
│   ├── linker/                 ← Linker script customizado (S0/S3-9 app, S10-11 EEPROM)
│   ├── tools/                  ← HTML config tool (Web Serial API, pt/en)
│   └── Makefile                ← Build via arm-none-eabi-gcc
├── ODrive-fw-v0.5.6/           ← ODrive firmware (com patches mínimos)
└── OpenFFBoard-master/         ← OpenFFBoard FFB stack (TinyUSB + HidFFB + EffectsCalculator)
```

## O que foi feito

### Hardware suportado
- MKS Mini ODrive S (STM32F405RGT6, motor BLDC, encoder ABZ, brake resistor)
- Fonte 24V, regen via brake resistor configurável

### Pipeline de FFB
- USB enumera como **CDC + HID composite** (TinyUSB)
- HID descriptor: PID gamepad 2-axis (compatível DirectInput/Windows FFB)
- Game manda HID OUT reports → `HidFFB` → `EffectsCalculator` (1 kHz tick) → ponte pra `axes[0].controller_.input_torque_` → motor
- Suporta efeitos: **Constant Force**, **Spring**, **Damper**, **Friction**, **Inertia**, **Periodic** (Sine/Triangle/Square/etc), **Ramp**

### Persistência separada
- ODrive NVM: setores 1+2 (config_manager nativo do ODrive)
- FFB EEPROM emulada: setores 10+11 (filtros, gains, params do volante)
- Sem colisão de flash

### Configuração via HTML
Tool em `Firmware-V56-Stock/tools/odrive_config.html` — Web Serial API, abre direto no Chrome/Edge.

Tabs:
- **ODrive** / **Axis 0** / **Motor** / **Encoder** / **Controller** — params via ODrive ASCII
- **FFB Wheel** — range, maxtorque, fxratio, axis effects (idlespring, damper, inertia, friction, esgain, slew, expo)
- **FFB Effects** — master gain + per-effect gains
- **FFB Filters** — biquad lowpass cutoff + Q por tipo de efeito
- **FFB Live** — dashboard ao vivo (estado FFB, contadores HID, efeitos ativos, picos de bus, gráfico torque/posição)
- **Debug / Status** — info do device, ações state machine, erros decodificados, monitor live, gráfico vbus/ibus/Iq/Ibrake
- **Console** — log TX/RX da serial

Cada campo configurável tem **tooltip explicando função** ao passar o mouse, e a interface tem suporte **PT/EN** com toggle no header.

## Clone

`OpenFFBoard-master/OpenFFBoard-master/` é um **git submodule** apontando pro upstream
[`Ultrawipf/OpenFFBoard`](https://github.com/Ultrawipf/OpenFFBoard) (atualmente travado em **v1.17.0**).
Pra clonar com submodule:

```bash
git clone --recurse-submodules <URL_DESTE_REPO>
```

Se já clonou sem `--recurse-submodules`:
```bash
git submodule update --init --recursive
```

## Build

Pré-requisitos:
- `arm-none-eabi-gcc` (testado com 12.2)
- `make`
- `dfu-util` (pra flash)

```bash
cd Firmware-V56-Stock
make -j4
```

Artefato: `build/firmware-v56-stock.bin`

## Flash

Coloca a placa em DFU (segura BOOT0 + reset, ou desliga/liga com BOOT0 pressionado), depois:

```bash
make flash-dfu
```

Equivalente a:
```bash
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D build/firmware-v56-stock.bin
```

## Configuração inicial recomendada

Após primeiro flash, conecta na HTML tool, calibra motor + encoder, e ajusta:

```
config.brake_resistance = 2.0          # ou valor real do seu resistor
config.enable_brake_resistor = True
config.max_regen_current = 0           # brake ativa imediatamente em qualquer regen
config.dc_max_negative_current = -1    # pra fonte chaveada
config.enable_dc_bus_overvoltage_ramp = True
config.dc_bus_overvoltage_ramp_start = 25
config.dc_bus_overvoltage_ramp_end = 27
axis0.controller.config.spinout_electrical_power_threshold = 50
axis0.controller.config.spinout_mechanical_power_threshold = -50
axis0.controller.config.electrical_power_bandwidth = 5
axis0.controller.config.mechanical_power_bandwidth = 5
```

Save ODrive (botão no header). Reboot. Calibra e arma motor.

Pra FFB:
```
axis.range = 900             # graus de rotação total
axis.maxtorque = 12          # Nm máximo (depende do seu motor)
axis.fxratio = 1.0           # multiplicador final
fx.master = 255              # gain global
```

Save FFB.

## Licenças

Este projeto combina código de múltiplas origens com licenças diferentes:

- **ODrive Firmware** — MIT License — `ODrive-fw-v0.5.6/`
- **OpenFFBoard** — GPLv3 — `OpenFFBoard-master/`
- **Código próprio** (`Firmware-V56-Stock/src`, `inc`, `tools`) — GPLv3 (compatível com OpenFFBoard)

Devido à inclusão de código GPL do OpenFFBoard, o **projeto inteiro** (firmware compilado) é
distribuído sob **GPLv3**. Veja `LICENSE` na raiz e licenças individuais nos subdiretórios.

## Status

✅ Calibração motor + encoder funcional
✅ FFB validado: Spring/Constant/Friction/Periodic respondendo no ForceTest
✅ Brake resistor + regen estável (sem reset de fonte)
✅ Persistência separada FFB / ODrive
✅ HTML config tool completa em PT/EN
🚧 Validação end-to-end em iRacing/AMS2/ETS2 (em andamento)

## Diagnóstico

Comandos úteis via terminal serial CDC (115200 baud):

| ASCII | OpenFFBoard | Função |
|-------|-------------|--------|
| `r path` / `w path val` | — | Read/write ODrive properties |
| `ss` / `se` / `sr` / `sc` | — | Save / erase / reboot / clear errors |
| `d` / `D` / `C` / `T` / `E[N]` | — | FFB diagnostics |
| `I` / `R` | — | Bus current peaks (read / reset) |
| `S[N]` | — | FFB effect slot raw dump |
| — | `axis.maxtorque?`/`=N` | FFB axis params |
| — | `fx.spring?`/`=N` | Effect gains |
| — | `sys.save!` | Persiste FFB EEPROM |
| — | `sys.eetest!` / `sys.eedump?` | EEPROM low-level diagnostic |

## Atualizando OpenFFBoard upstream

Algumas funções do FFB stack (HidFFB, EffectsCalculator) foram **forkadas e modificadas** localmente
em `Firmware-V56-Stock/src/` e `inc/`. As cópias originais ficam no submodule `OpenFFBoard-master/`.

Quando o upstream lançar updates relevantes, workflow:

```bash
# 1. Puxa último commit do upstream pro submodule
git submodule update --remote OpenFFBoard-master/OpenFFBoard-master

# 2. Vê o que mudou
cd OpenFFBoard-master/OpenFFBoard-master
git log --oneline HEAD@{1}..HEAD          # commits novos
git diff HEAD@{1}..HEAD --stat            # arquivos alterados
cd ../..

# 3. Compara nossos forks com o upstream atualizado
./Firmware-V56-Stock/tools/check-openffboard-upstream.sh           # resumo
./Firmware-V56-Stock/tools/check-openffboard-upstream.sh --verbose # com diffs

# 4. Pra cada arquivo que mostra "DIVERGE" e teve mudanças relevantes
#    no upstream, manualmente integra no nosso fork em Firmware-V56-Stock/

# 5. Compila + testa
cd Firmware-V56-Stock && make -j4

# 6. Commit
git add OpenFFBoard-master/OpenFFBoard-master Firmware-V56-Stock/...
git commit -m "Bump OpenFFBoard upstream to <hash> + integrate changes"
```

Os arquivos forkados têm um header no início indicando:
- Versão upstream que foi base do fork (commit hash)
- Descrição das modificações locais
- Comando exato pra fazer diff vs upstream

Ex: `Firmware-V56-Stock/src/HidFFB.cpp` indica modificação do `set_effect` pra single-axis fallback.

## Histórico

Construído iterativamente, fases:

- **Fase 1** — ODrive v0.5.6 stock funcionando + calibração
- **Fase 2a/b** — TinyUSB CDC + HID composite enumerando
- **Fase 2c** — FFB stack portada do OpenFFBoard
- **Fase 2d** — OpenFFBoard CmdParser via CDC (parser dual: ODrive ASCII + OpenFFBoard)
- **Fase 3** — Persistência FFB em EEPROM emulada (S10+S11), HTML completo, dashboards
