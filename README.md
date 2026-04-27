# ODrive FFB Wheel — MKS XDrive Mini Firmware

Custom firmware for ODrive v0.5.6 running on **MKS XDrive Mini** hardware
(STM32F405-based clone of ODrive v3.6-56V), adding full **HID Force Feedback**
support to use the motor as a sim racing wheel.

Based on:
- [ODrive Firmware v0.5.6](https://github.com/odriverobotics/ODrive) (motor control)
- [OpenFFBoard](https://github.com/Ultrawipf/OpenFFBoard) (FFB stack: HidFFB + EffectsCalculator)

## Repository structure

```
.
├── Firmware-V56-Stock/         ← Main project (CDC + HID composite + FFB)
│   ├── src/                    ← Local sources (USB, FFB bridge, cmd_table)
│   ├── inc/                    ← Local headers
│   ├── linker/                 ← Custom linker script (S0/S3-9 app, S10-11 EEPROM)
│   ├── tools/                  ← HTML config tool (Web Serial API, pt/en i18n)
│   └── Makefile                ← Build via arm-none-eabi-gcc
├── ODrive-fw-v0.5.6/           ← ODrive firmware (with minimal patches)
└── OpenFFBoard-master/         ← Submodule → upstream Ultrawipf/OpenFFBoard
```

## What's been done

### Hardware supported
- MKS XDrive Mini (STM32F405RGT6, BLDC motor, ABZ encoder, brake resistor)
- 24 V power supply, configurable regen via brake resistor

### FFB pipeline
- USB enumerates as **CDC + HID composite** (TinyUSB)
- HID descriptor: 2-axis PID gamepad (DirectInput / Windows FFB compatible)
- Game sends HID OUT reports → `HidFFB` → `EffectsCalculator` (1 kHz tick) → bridge → `axes[0].controller_.input_torque_` → motor
- Supports effects: **Constant Force**, **Spring**, **Damper**, **Friction**, **Inertia**, **Periodic** (Sine/Triangle/Square/etc.), **Ramp**

### Separated persistence
- ODrive NVM: sectors 1+2 (ODrive's native config_manager)
- FFB emulated EEPROM: sectors 10+11 (filters, gains, wheel params)
- No flash collision

### HTML configuration tool
Tool at `Firmware-V56-Stock/tools/odrive_config.html` — Web Serial API, opens directly in Chrome/Edge.

Tabs:
- **ODrive** / **Axis 0** / **Motor** / **Encoder** / **Controller** — params via ODrive ASCII
- **FFB Wheel** — range, maxtorque, fxratio, axis effects (idlespring, damper, inertia, friction, esgain, slew, expo)
- **FFB Effects** — master gain + per-effect gains
- **FFB Filters** — biquad lowpass cutoff + Q per effect type
- **FFB Live** — live dashboard (FFB state, HID counters, active effects, bus current peaks, torque/position chart)
- **Debug / Status** — device info, state machine actions, decoded errors, live monitor, vbus/ibus/Iq/Ibrake chart
- **Console** — serial TX/RX log

Each configurable field has a **tooltip explaining its function** on hover, and the UI supports **PT/EN** with a header toggle.

## Clone

`OpenFFBoard-master/OpenFFBoard-master/` is a **git submodule** pointing to upstream
[`Ultrawipf/OpenFFBoard`](https://github.com/Ultrawipf/OpenFFBoard) (currently locked at **v1.17.0**).
Clone with submodules:

```bash
git clone --recurse-submodules https://github.com/eagabriel/OpenffboardOdrive.git
```

If you already cloned without `--recurse-submodules`:
```bash
git submodule update --init --recursive
```

## Build

Prerequisites:
- `arm-none-eabi-gcc` (tested with 12.2)
- `make`
- `dfu-util` (for flashing)

```bash
cd Firmware-V56-Stock
make -j4
```

Artifact: `build/firmware-v56-stock.bin`

## Flash

Put the board into DFU mode (hold BOOT0 + reset, or power-cycle with BOOT0 held), then:

```bash
make flash-dfu
```

Equivalent to:
```bash
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D build/firmware-v56-stock.bin
```

## Screenshots

The HTML configuration tool runs entirely in the browser via Web Serial API
(Chrome/Edge), with no install required. Below are key screens.

### Header / language toggle / connection controls
![Header](docs/screenshots/01-header.png)

### ODrive tab — power, brake resistor, communication, GPIO
![ODrive tab](docs/screenshots/02-tab-odrive.png)

### Controller tab — PID gains, spinout protection, anticogging
![Controller tab](docs/screenshots/03-tab-controller.png)

### FFB Wheel tab — force scaling, axis effects, slew/curve
![FFB Wheel tab](docs/screenshots/04-tab-ffb-wheel.png)

### FFB Effects tab — master gain + per-effect gains
![FFB Effects tab](docs/screenshots/05-tab-ffb-effects.png)

### FFB Filters tab — biquad cutoff/Q per effect type
![FFB Filters tab](docs/screenshots/06-tab-ffb-filters.png)

### FFB Live — dashboard with active effects + dynamics analysis + chart
![FFB Live tab](docs/screenshots/07-tab-ffb-live.png)

### Debug / Status — live monitor + bus current chart (vbus, ibus, Iq, Ibrake)
![Debug tab](docs/screenshots/08-tab-debug.png)

### Tooltip on hover — every configurable field has a description
![Tooltip example](docs/screenshots/09-tooltip-example.png)

## Updating OpenFFBoard upstream

Several FFB stack files (HidFFB, EffectsCalculator) were **forked and modified** locally in
`Firmware-V56-Stock/src/` and `inc/`. The originals live in the `OpenFFBoard-master/` submodule.

When upstream releases relevant updates, workflow:

```bash
# 1. Pull latest upstream commit into the submodule
git submodule update --remote OpenFFBoard-master/OpenFFBoard-master

# 2. See what changed
cd OpenFFBoard-master/OpenFFBoard-master
git log --oneline HEAD@{1}..HEAD          # new commits
git diff HEAD@{1}..HEAD --stat            # changed files
cd ../..

# 3. Compare our forks against the updated upstream
./Firmware-V56-Stock/tools/check-openffboard-upstream.sh           # summary
./Firmware-V56-Stock/tools/check-openffboard-upstream.sh --verbose # with diffs

# 4. For each file marked "DIVERGE" with relevant upstream changes,
#    manually integrate into our fork in Firmware-V56-Stock/

# 5. Compile + test
cd Firmware-V56-Stock && make -j4

# 6. Commit
git add OpenFFBoard-master/OpenFFBoard-master Firmware-V56-Stock/...
git commit -m "Bump OpenFFBoard upstream to <hash> + integrate changes"
```

Forked files have a header at the top indicating:
- Upstream version that was the fork base (commit hash)
- Description of local modifications
- Exact command to diff against upstream

E.g., `Firmware-V56-Stock/src/HidFFB.cpp` documents the `set_effect` modification for single-axis fallback.

## Licenses

This project combines code from multiple sources with different licenses:

- **ODrive Firmware** — MIT License — `ODrive-fw-v0.5.6/`
- **OpenFFBoard** — GPLv3 — `OpenFFBoard-master/`
- **Our own code** (`Firmware-V56-Stock/src`, `inc`, `tools`) — GPLv3 (compatible with OpenFFBoard)

Because GPL-licensed code from OpenFFBoard is included, the **combined work** (compiled firmware) is
distributed under **GPLv3**. See `LICENSE` at the repo root and individual licenses in subdirectories.

## Status

✅ Motor + encoder calibration working
✅ FFB validated: Spring / Constant / Friction / Periodic responding in ForceTest
✅ Brake resistor + regen stable (no PSU resets)
✅ Separated FFB / ODrive persistence
✅ Full HTML config tool in PT/EN
✅ End-to-end validation in **iRacing**

## History

Built iteratively, in phases:

- **Phase 1** — ODrive v0.5.6 stock working + calibration
- **Phase 2a/b** — TinyUSB CDC + HID composite enumerating
- **Phase 2c** — FFB stack ported from OpenFFBoard
- **Phase 2d** — OpenFFBoard CmdParser via CDC (dual parser: ODrive ASCII + OpenFFBoard)
- **Phase 3** — FFB persistence in emulated EEPROM (S10+S11), full HTML tool, dashboards
