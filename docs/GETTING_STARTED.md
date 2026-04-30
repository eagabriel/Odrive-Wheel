# Getting Started — Odrive-Wheel

There are three ways to get the firmware onto an MKS XDrive Mini board:

- **Route A — `dfu-util` (CLI)** — download the prebuilt `.bin` and flash it from the command line. Required for the **first flash**, or if something ever goes wrong.
- **Route A2 — Web Flasher (browser)** — once the Odrive-Wheel firmware is already on the board, you can update it from the HTML config tool itself, no `dfu-util` needed. The easier option for day-to-day use.
- **Route B — Compile from source (VS Code)** — clone the repo, build, and flash. For when you want to modify the code.

After the firmware is flashed, the **"First-time motor bring-up"** section is **mandatory** — do not skip it, even on Route A. An ODrive without calibration or with the wrong `brake_resistance` can damage the PSU or the brake resistor.

---

## Expected hardware

- **MKS XDrive Mini** board (ODrive v3.6 clone, STM32F405)
- **BLDC motor** (3-phase — no hall, no stepper driver)
- **Incremental ABZ encoder** (Z optional but recommended for sim racing)
- **12 to 48 V power supply** (mind the voltage variant of your MKS XDrive Mini)
- **Brake resistor** wired to the AUX terminals (typically 2 Ω 50 W)
- **USB-C cable for data** (not just for charging!)

---

## Route A — Flash the prebuilt firmware

### 1. Download the binary

Releases page: https://github.com/eagabriel/Odrive-Wheel/releases

Download `odrive-wheel.bin` from the latest release.

### 2. Install dfu-util

- **Windows**: download `dfu-util-0.11-binaries.tar.xz` from [dfu-util.sourceforge.net](http://dfu-util.sourceforge.net/releases/), extract, and add it to PATH. It also ships with STM32CubeProgrammer.
- **Linux**: `sudo apt install dfu-util`
- **macOS**: `brew install dfu-util`

Verify: `dfu-util --version` (version ≥ 0.10).

### 3. Put the board into DFU mode

1. Power off the supply.
2. Hold the **BOOT0 button** (some clones have a jumper instead — close the BOOT0 jumper).
3. Plug the USB-C cable into the PC.
4. Release BOOT0.

Alternatively, hold BOOT0 and tap the reset button.

On Windows, you may need to install the **WinUSB** driver once via [Zadig](https://zadig.akeo.ie/) — pick the `STM32 BOOTLOADER` device and replace its driver with `WinUSB`.

Verify: `dfu-util -l` should list `[0483:df11]`.

### 4. Flash

```bash
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D odrive-wheel.bin
```

Expected output ends with `Done!` and the board reboots on its own. You're done — jump straight to **First-time motor bring-up**.

---

## Route A2 — Update firmware from the browser (Web Flasher)

> ⚠️ This route only works **after** the Odrive-Wheel firmware has been flashed at least once (Route A or B). It is for subsequent updates.

The `tools/odrive-wheel.html` tool has a **"DFU Flash"** tab that does everything inside the browser: reboots the board into DFU mode, detects the bootloader, lets you pick the `.bin`, and flashes — no `dfu-util`, no need to press BOOT0.

### Prerequisites (one time per machine)

- **Up-to-date Chrome or Edge** (needs WebUSB + Web Serial).
- **On Windows**: install the **WinUSB** driver on the STM32 bootloader via [Zadig](https://zadig.akeo.ie/):
  1. Put the board into DFU mode once (Route A step 3, or run `sd` from the Console if you already have Odrive-Wheel firmware).
  2. Open Zadig → menu **Options → List All Devices**.
  3. Select **STM32 BOOTLOADER** in the dropdown.
  4. Target driver: **WinUSB** → click **Replace Driver**.
  5. Wait for "Driver installed" and close.

  This is one-time per machine. The driver stays installed afterwards. (Don't confuse it with the CDC `Odrive-Wheel CDC` device — that one keeps the default Windows driver and works with Web Serial out of the box.)

### Step by step

1. Open the config tool — easiest is the hosted version at
   **<https://eagabriel.github.io/Odrive-Wheel/>** (no install). Or open
   `Odrive-Wheel/tools/odrive-wheel.html` locally if you prefer.
2. Connect to the board over serial (the **Connect** button in the header).
3. Go to the **DFU Flash** tab in the sidebar (under "Tools").
4. **Step 1 — Reboot to DFU**: click the button. The tool sends `sd` over serial and the board reboots straight into the STM32 bootloader. Wait ~2 s.
5. **Step 2 — Find bootloader**: click "Find bootloader". The browser opens a dialog asking for permission to access `STM32 BOOTLOADER` — approve it. The badge turns green with `✓` and shows `VID:0x0483 PID:0xDF11`.
6. **Step 3 — Choose firmware**: click "Choose file" and pick `odrive-wheel.bin` (from the Release, or from `Odrive-Wheel/build/` if you compiled it).
7. **Step 4 — Flash firmware**: click "Flash firmware". It erases sectors S0–S9 (256 KB of application flash), writes in 1 KB chunks (~30–60 s total), and the board reboots into the new firmware automatically.

The operation log at the bottom of the page shows every step in real time. The progress bar runs from 0 to 100%.

### Troubleshooting

| Problem | Fix |
|---|---|
| Step 2 says "No device selected" | Step 1 didn't work, or Zadig/WinUSB wasn't installed. Enter DFU via BOOT0 + try again. |
| Step 4 fails with "DFU status=0x..." | Bootloader is stuck in an error state — unplug the USB cable, plug it back in DFU mode (BOOT0), and try Step 4 directly without Step 1. |
| Board doesn't reappear as CDC after flashing | Wait 5–10 s; Windows sometimes takes a moment to re-enumerate. If it persists, unplug and replug the USB cable. |
| WebUSB asks for permission every time | Normal — it's a Chrome safety feature. Each `.html` file under `file://` is treated as a fresh origin. |

### EEPROM (settings) is preserved

The Web Flasher only erases sectors S0–S9 (firmware). Sectors **S10 and S11** (where your saved FFB settings live — gain, filters, range, idle spring, etc.) are **not touched**. You don't lose calibration or tuning when you update.

> ⚠️ However, the ODrive Save NVM (in S1) **lives within** the range we erase. Updating the firmware will reset ODrive parameters (motor, encoder, controller, brake_resistance) to defaults. Always export the JSON via the **Export** button before updating, and re-import afterwards.

---

## Route B — Compile from source (VS Code)

### 1. Prerequisites

| Tool | Where to get it | Notes |
|---|---|---|
| **arm-none-eabi-gcc** 12.x | [Arm GNU Toolchain](https://developer.arm.com/Tools%20and%20Software/GNU%20Toolchain) | Add to PATH |
| **make** | Windows: [chocolatey](https://chocolatey.org) `choco install make` or MSYS2 | Linux/Mac come with it |
| **Git** | [git-scm.com](https://git-scm.com/) | — |
| **dfu-util** ≥ 0.10 | see Route A | For `make flash-dfu` |
| **VS Code** | [code.visualstudio.com](https://code.visualstudio.com/) | — |

Verify:
```bash
arm-none-eabi-gcc --version    # should say 12.x
make --version
git --version
dfu-util --version
```

### 2. Clone with submodule

OpenFFBoard is a Git submodule, so use `--recurse-submodules`:

```bash
git clone --recurse-submodules https://github.com/eagabriel/Odrive-Wheel.git
cd Odrive-Wheel
```

If you already cloned without submodules:
```bash
git submodule update --init --recursive
```

### 3. Recommended VS Code extensions

Open VS Code at the repo root and install:

- **C/C++** (`ms-vscode.cpptools`) — IntelliSense
- **Makefile Tools** (`ms-vscode.makefile-tools`) — build shortcuts
- **Cortex-Debug** (`marus25.cortex-debug`) — optional, for debugging with ST-Link

### 4. Build

From the integrated terminal in VS Code (Ctrl+Shift+`):

```bash
cd Odrive-Wheel
make -j4
```

The first build takes ~1 min. The output ends up at `build/odrive-wheel.bin` (~400 KB).

Common errors:
- `arm-none-eabi-gcc: command not found` → toolchain not on PATH
- `fatal error: tusb.h: No such file` → submodule not initialized, run `git submodule update --init --recursive`

### 5. Flash

Put the board into DFU mode (see Route A step 3) and:

```bash
make flash-dfu
```

Equivalent to:
```bash
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D build/odrive-wheel.bin
```

---

## First-time motor bring-up

> ⚠️ **Before powering up:**
> - **Disconnect the motor from the wheel.** The first calibration spins the motor — if it's already mounted to the wheel shaft, it will hit the physical end-stop.
> - **Confirm the brake resistor is physically wired** to the AUX terminals. Without it, any regen current goes back to the PSU and will likely trip its OVP.



### 1. Connect the configuration tool

1. Open the config tool: <https://eagabriel.github.io/Odrive-Wheel/> in **Chrome or Edge** (Web Serial only works there). Local copy at `Odrive-Wheel/tools/odrive-wheel.html` works equivalently.
2. Click **Connect** and pick the board's serial port (`Odrive-Wheel CDC` or similar).
3. The status pill should turn green ("Connected").

### 2. Minimum configuration — power & protections

**ODrive** tab:

| Field | Suggested value | Why |
|---|---|---|
| `brake_resistance` | **2.0** (Ω) | Must match the physical resistor — wrong here = lockup or burn |
| `enable_brake_resistor` | **true** | Enables active regen control |
| `dc_max_positive_current` | **20** (A) | Current limit from PSU → motor |
| `dc_max_negative_current` | **-5** (A) | How much can flow back to the brake (negative!) |
| `dc_bus_overvoltage_trip_level` | **28** (V) | Trip threshold — protects the PSU (set 4 V above your supply voltage) |
| `dc_bus_undervoltage_trip_level` | **8** (V) | Prevents brown-outs |
| `max_regen_current` | **0** (A) | Threshold above which the brake kicks in as a dump load (0 means *all* regen current goes to the resistor — safer to start with) |

### 3. Minimum configuration — motor

**Motor** tab (calibrated first; conservative values):

| Field | Suggested value |
|---|---|
| `motor_type` | **0** (HIGH_CURRENT) |
| `pole_pairs` | depends on your motor — count rotor magnets and divide by 2 (typical: 7) |
| `torque_constant` | **0.87** (Nm/A) |
| `current_lim` | **10** (A) — start low! |
| `calibration_current` | **5** (A) |
| `resistance_calib_max_voltage` | **12.0** (V) |
| `pre_calibrated` | **false** |

### 4. Minimum configuration — encoder

**Encoder** tab:

| Field | Suggested value |
|---|---|
| `mode` | **0** (INCREMENTAL) |
| `cpr` | **encoder lines × 4** (e.g. 1000 lines → 4000) |
| `use_index` | **true** if Z is wired, otherwise false |
| `pre_calibrated` | **false** |

### 5. Minimum configuration — controller (FFB mode)

**Controller** tab:

| Field | Suggested value |
|---|---|
| `control_mode` | **1** (TORQUE_CONTROL) — required for FFB |
| `input_mode` | **1** (PASSTHROUGH) |
| `vel_limit` | **8** (turns/s) — safety cutoff |

**Axis 0** tab:

| Field | Suggested value |
|---|---|
| `startup_motor_calibration` | **false** (calibrate manually first) |
| `startup_encoder_offset_calibration` | **false** |
| `startup_closed_loop_control` | **false** (we'll enable this later) |

### 6. Save and calibrate

1. Click **Save** (the green button in the header).
   - Save does this: persists FFB to EEPROM → writes ODrive configs → reboots the board.
2. After the reboot, reconnect.
3. **Debug / Status** tab, "Actions" section:
   - **Motor calibration** (`w axis0.requested_state 4`) — the motor "beeps" and measures R/L. Check in **Live monitor** that `motor.config.phase_resistance` lands between 0.05–1.0 Ω and `phase_inductance` between 1e-5 and 5e-4 H. If out of range, the motor is mis-wired or `pole_pairs` is wrong.
   - If OK, set `motor.config.pre_calibrated` → **true** + Save.
   - **Encoder offset calibration** (`w axis0.requested_state 7`) — the motor turns ~1 rev slowly to measure the offset. Verify `encoder.config.offset` ≠ 0 and `axis0.error` = 0.
   - If OK, `encoder.config.pre_calibrated` → **true** + Save.
4. Reboot.

### 7. First closed-loop test (no FFB yet)

1. **Debug / Status** tab → action **Closed loop** (`w axis0.requested_state 8`).
2. Try to spin the shaft by hand — it should **resist** (holding torque command = 0). That confirms the loop closed.
3. If the shaft spins freely, calibration is bad. If it locks up with vibration, `pole_pairs` or `cpr` is wrong.
4. All good → disarm with `w axis0.requested_state 1` (IDLE).

### 8. Minimum configuration — FFB

**FFB Wheel** tab:

| Field | Suggested first-test value |
|---|---|
| `range` | **900** (degrees lock-to-lock) |
| `maxtorque` | **2.0** (Nm) — **start low!** It will pull hard, you don't want surprises |
| `fxratio` | **1.0** (100%) |
| `idlespring` | **5** (centering spring when the game isn't sending FFB) |

**FFB Effects** tab:

| Field | Value |
|---|---|
| `master` | **255** (100%) |

Save → reboot.

### 9. Validate FFB

1. Use a tool such as **ForceTest** (in `docs/Tools`) to exercise each effect.
2. In Windows, `joy.cpl` → "Odrive-Wheel" → **Test Forces** tab if available.
3. **FFB Live** tab in the web tool — you'll see `HID OUT counter` ticking up and the active effects.

If you feel torque proportional to the input → FFB is working. Raise `maxtorque` slowly (2 → 4 → 6 Nm) only after you've mounted the motor on the wheel shaft and verified the physical end-stop can take it.

---

## Final checklist before driving

- [ ] Steering wheel mounted on the shaft, shaft mechanically or software end-stopped (no infinite rotation)
- [ ] Brake resistor in a ventilated spot (it can hit 60–80 °C in heavy use, that's normal)
- [ ] In game, **start with low FFB strength** (50%) and dial up
- [ ] Roll back to conservative values if you get vibration or audible noise
- [ ] **Export your working config** before tweaking anything else

---

## Where to ask for help

- Project issues: https://github.com/eagabriel/Odrive-Wheel/issues
- OpenFFBoard Discord
