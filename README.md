# Odrive-Wheel — MKS XDrive Mini / ODESC V4.2 FFB Firmware

Custom firmware for ODrive v0.5.6 running on **MKS XDrive Mini** and
**ODESC V4.2** hardware (STM32F405-based clones of ODrive v3.6), adding
full **HID Force Feedback** support to use the motor as a sim racing wheel.

<p align="center">
  <a href="https://eagabriel.github.io/Odrive-Wheel/"><b>🛠 Open the configuration tool →</b></a><br>
  <sub>Runs in Chrome/Edge — no install. Connects via Web Serial / WebHID.</sub>
</p>

<p align="center">
  <a href="Odrive-Wheel/tools/simhub-plugin/USER_MANUAL.md"><b>🎮 SimHub plugin — live telemetry, profile sync, mappable actions →</b></a><br>
  <sub>Reads the 1 kHz HID stream, syncs profile config via serial, adds 21 wheel-mappable actions. <a href="Odrive-Wheel/tools/simhub-plugin/USER_MANUAL.md">User manual</a>.</sub>
</p>

<p align="center">
  <img src="docs/screenshots/MKSXdriveMini.png" alt="MKS XDrive Mini board" width="420">
  <img src="docs/screenshots/Overlay%20FFT.png" alt="PiP Overlay with live FFT spectrum analyzer" width="420">
</p>

<table align="center">
  <tr>
    <td width="50%">
      <a href="docs/screenshots/01-Header.png">
        <img src="docs/screenshots/01-Header.png" alt="Configuration tool — main overview"></a>
    </td>
    <td width="50%">
      <a href="docs/screenshots/06-GPIOConfig.png">
        <img src="docs/screenshots/06-GPIOConfig.png" alt="Inputs / GPIO config"></a>
    </td>
  </tr>
  <tr>
    <td width="50%">
      <a href="docs/screenshots/07-FFB%20test.png">
        <img src="docs/screenshots/07-FFB%20test.png" alt="FFB Force Test — built-in WebHID tester"></a>
    </td>
    <td width="50%">
      <a href="docs/screenshots/04-Iracing%20overlay.png">
        <img src="docs/screenshots/04-Iracing%20overlay.png" alt="iRacing overlay (Picture-in-Picture)"></a>
    </td>
  </tr>
</table>

<p align="center">
  <a href="https://github.com/sponsors/eagabriel" target="_blank">
    <img src="https://img.shields.io/badge/Sponsor%20on%20GitHub-30363D?style=for-the-badge&logo=GitHub-Sponsors&logoColor=EA4AAA"
         alt="Sponsor on GitHub" height="80">
  </a>
  &nbsp;&nbsp;
  <a href="https://www.buymeacoffee.com/eduardogabq" target="_blank">
    <img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png"
         alt="Buy me a coffee" height="80">
  </a>
  &nbsp;&nbsp;
  <a href="#pix-brazil">
    <img src="https://img.shields.io/badge/PIX-Brazil-32BCAD?style=for-the-badge&logoColor=white"
         alt="Donate via PIX (Brazil)" height="80">
  </a>
</p>

<p align="center">
  <a href="https://discord.com/channels/704355326291607614/1499185654033158305" target="_blank">
    <img src="https://img.shields.io/badge/Discussion%20on%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white"
         alt="Project discussion on Discord" height="40">
  </a>
</p>

<p align="center">
  <sub>Questions, build logs, tuning tips and bug reports —
  <a href="https://discord.com/channels/704355326291607614/1499185654033158305">join the project thread on Discord</a>.</sub>
</p>

> 💛 **Open-source, maintained on personal time.** If this firmware saved
> you a few hundred bucks on a commercial wheelbase, please consider
> [becoming a GitHub Sponsor](https://github.com/sponsors/eagabriel),
> [buying a coffee](https://www.buymeacoffee.com/eduardogabq),
> or [donating via PIX](#pix-brazil) — it really helps keep the project
> going.

Based on:
- [ODrive Firmware v0.5.6](https://github.com/odriverobotics/ODrive) (motor control)
- [OpenFFBoard](https://github.com/Ultrawipf/OpenFFBoard) (FFB stack: HidFFB + EffectsCalculator)

Community contributions:
- [@aksc857-stack](https://github.com/aksc857-stack) — MT6835 21-bit SPI encoder: driver, threaded read path, atomic SPI arbiter guard, chip register access and CLI. Also velocity/acceleration from the encoder PLL, and the split of axis position invert from FFB torque invert
- [@TelksBr](https://github.com/TelksBr) — [ODrive-Wheel-Forge](https://github.com/TelksBr/ODrive-Wheel-Forge): USB HID PID compatibility fix suite and thermal telemetry
- [@simachines](https://github.com/simachines) — autogen tooling backup

## 🚀 Quick start

**Fastest path — in-browser Quick Start wizard.**

Open **<https://eagabriel.github.io/Odrive-Wheel/>** in Chrome/Edge,
connect the board via Web Serial, and follow the **Quick Start** tab —
a 13-step guided wizard that walks you from blank firmware all the way
to FFB responding in-game:

1. Flash firmware (DFU) → 2. Connect → 3. Erase old config → 4. Power &
protections (with **⚡ Set voltage** button — reads real `vbus_voltage`
and auto-configures over/undervoltage trips + regen ramp) → 5. Motor
config → 6. Encoder config → 7. Motor calibration → 8. Encoder offset →
9. Mark pre-calibrated & Save → 10. Configure Z (index pulse) → 11. FFB
config → 12. Test Spring → 13. Done.

Each step shows suggested values, links to the relevant config tab, and
decodes errors inline. UI is PT/EN.

## 📚 Guides

- **[Getting started (long-form)](docs/GETTING_STARTED.md)** — first
  flash routes (`dfu-util` and browser DFU), minimum safe motor
  bring-up, Windows driver setup (Zadig).
- **[How torque control works](docs/TORQUE_CONTROL.md)** — deep dive
  into the FFB pipeline from Nm demand to motor current. Explains why
  `pos_gain` / `vel_gain` are inert in TORQUE mode.
- **[Tuning feeling](docs/TUNING_FEELING.md)**
  ([PT](docs/TUNING_FEELING_PT.md)) — post-setup guide for shaping the
  FFB chain: gains, filters, end-stop, EQ.
- **[Building from source](docs/BUILDING.md)** — for when you want to
  modify the firmware, sync with upstream OpenFFBoard, or investigate a
  build issue.
- **[SimHub plugin user manual](Odrive-Wheel/tools/simhub-plugin/USER_MANUAL.md)**
  — install, panel walkthrough with screenshots, property reference,
  mappable actions, profile workflow, troubleshooting.

## ✨ What you get

- **Full HID Force Feedback** via USB HID PID (DirectInput / Windows
  FFB). Effects: Constant, Spring, Damper, Friction, Inertia, Ramp,
  Periodic (Sine / Square / Triangle / Sawtooth). Nine PID
  compatibility fixes in v1.1.0 restore FFB in titles where it was
  silently broken (iRacing, AC, ACC, rFactor 2, Forza Motorsport, AMS2,
  BeamNG, DiRT Rally, F1, EA WRC).
  *Forza Horizon 5/6 uses Windows.Gaming.Input and still needs the
  community EmuWheel workaround — see Discord.*
- **Absolute encoders** — AS5047 (onboard MKS, 14-bit) and MagnTek
  MT6835 (21-bit, ODESC V4.2 SPI port) as first-class citizens.
  Incremental ABZ also supported.
- **3-band EQ** in the FFB Filters tab (WEIGHT / CHASSIS / ROAD, ±12
  dB), normalised on DC so cornering weight never changes as you tune.
- **Tuning profiles** in their own tab — save your setup per car class,
  switch in one click, share as JSON files.
- **1 kHz HID telemetry** — `vel_estimate`, `Iq_measured`,
  `torque_output`, `Vbus`, `Ibus`, `Ibrake` embedded in the input
  report at native rate, so tools and dashboards read the wheel without
  fighting the game for the serial channel.
- **iRacing PiP overlay** — always-on-top window with DC Bus, Wheel and
  live FFT panels, plus P_brake, P_motor and Clip OUT% indicators.
- **Performance Test suite** — J via median `Iq·τ/α`, coastdown for
  `b`/`τ`, frequency sweep with real Bode overlay on the filter chart.
- **In-browser DFU flasher** (WebUSB + DfuSe) with 📡 Fetch latest
  from GitHub — no `dfu-util` needed after the first flash. FFB
  settings survive the reflash (isolated flash sectors).
- **SimHub plugin** — [Odrive-Wheel Telemetry](Odrive-Wheel/tools/simhub-plugin/USER_MANUAL.md):
  reads all 1 kHz HID fields (position, velocity, Iq, torque, VBus,
  IBus, brake current, FET/motor temps, ODrive errors) as SimHub
  properties for dashboards; syncs every profile field over serial;
  applies profiles with a click (and auto-per-game if you want);
  exposes 21 wheel-mappable actions (fine/coarse inc-dec of FX master,
  FX ratio, 3-band EQ + one-shot Zero-wheel-position).

Full feature inventory and per-tab details live in the config tool
itself — every field has a hover tooltip.

## Flash the firmware

**Recommended path:** open the [HTML config tool](https://eagabriel.github.io/Odrive-Wheel/),
go to the **DFU Flash** tab, click **📡 Fetch latest from GitHub**, follow
the four buttons. Zero downloads on your side.

**First flash from a blank board:** hold **BOOT0** and press **RESET** (or
power-cycle with BOOT0 held) to enter DFU mode via the built-in bootloader
— no extra tooling needed to get the board discovered. From there the
in-browser flasher (WebUSB) handles the rest on Chrome/Edge. If the browser
does not see the DFU device — happens on some Windows setups because
`0483:df11` needs the WinUSB driver — install it once with
[Zadig](https://zadig.akeo.ie/), or fall back to `dfu-util`. Full
walkthrough in [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md).

## Build from source

See **[docs/BUILDING.md](docs/BUILDING.md)** — CI, Windows and Linux
build routes, submodule notes, and how to sync with upstream OpenFFBoard.

## Screenshots

Additional views of the config tool:

![Header detail](docs/screenshots/02-Header.png)

![Header sidebar](docs/screenshots/03-Header.png)

![Encoder tab — Zero wheel position + AS5047 preset](docs/screenshots/05-Zero%20e%20encoder.png)

## Licenses

- **ODrive Firmware** — MIT — `ODrive-fw-v0.5.6/`
- **OpenFFBoard** — GPLv3 — `OpenFFBoard-master/`
- **Our own code** (`Odrive-Wheel/src`, `inc`, `tools`) — GPLv3

Because GPL-licensed code from OpenFFBoard is included, the combined
work (compiled firmware) is distributed under **GPLv3**. See `LICENSE`
at the repo root and individual licenses in subdirectories.

## 💛 Support the project

A commercial direct-drive wheelbase with similar specs costs **R$3,000 to
R$10,000+**. Odrive-Wheel turns a generic BLDC motor + a $40 board into
something that competes with that — entirely open-source, no licensing,
no subscriptions, no firmware lock-ins.

Every hour invested here came out of nights and weekends. **If this
project saved you real money**, please consider supporting it so I can
keep adding features, fixing bugs, and answering questions:

<p align="center">
  <a href="https://github.com/sponsors/eagabriel" target="_blank">
    <img src="https://img.shields.io/badge/Sponsor%20on%20GitHub-30363D?style=for-the-badge&logo=GitHub-Sponsors&logoColor=EA4AAA"
         alt="Sponsor on GitHub" height="80">
  </a>
  &nbsp;&nbsp;
  <a href="https://www.buymeacoffee.com/eduardogabq" target="_blank">
    <img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png"
         alt="Buy me a coffee" height="80">
  </a>
  &nbsp;&nbsp;
  <a href="#pix-brazil">
    <img src="https://img.shields.io/badge/PIX-Brazil-32BCAD?style=for-the-badge&logoColor=white"
         alt="Donate via PIX (Brazil)" height="80">
  </a>
</p>

**GitHub Sponsors** is the easiest way for recurring support and gives
you visibility on the repo. **Buy Me a Coffee** is great for a one-time
thanks. **PIX** (details below) is instant and free for donors in Brazil.
Every contribution — no matter the size — makes a real difference.

<a id="pix-brazil"></a>

### 💚 PIX (Brazil)

If you're in Brazil, PIX is the fastest way to donate — instant, no fees.
Scan the QR below with your bank app, or copy the code and paste it into
the "PIX Copia e Cola" field:

<p align="center">
  <img src="docs/pix-qr.png" alt="PIX QR code — recipient Eduardo Gabriel" width="260">
</p>

```
00020126580014br.gov.bcb.pix0136a6228ec1-c293-4a94-8523-6b8b26c47e085204000053039865802BR5915EDUARDO.GABRIEL6009Sao Paulo610901227-20062210517daqr643133736513963048268
```

<p align="center"><sub>Recipient: <b>Eduardo Gabriel</b> · São Paulo</sub></p>

🙏 **Other ways to help, even for free:**
- ⭐ Star the repo
- 🐛 Open issues for bugs you find
- 📣 Share with sim racing friends (Reddit, Discord, forums)
- 📸 Post videos/photos of your build — tag the project

## Status

Current stable release: **[v1.1.0](https://github.com/eagabriel/Odrive-Wheel/releases/latest)**.
Firmware is validated end-to-end in iRacing; MT6835 encoder is out of
experimental status; USB HID PID fix suite restores FFB in several
titles where it was silently broken.

Full changelog per release: **<https://github.com/eagabriel/Odrive-Wheel/releases>**.
