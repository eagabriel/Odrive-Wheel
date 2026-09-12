# Odrive-Wheel SimHub plugin

A SimHub plugin that reads live telemetry from an Odrive-Wheel board over
USB HID at the firmware's native 1 kHz rate. No firmware changes required —
the board already broadcasts these values in every HID input report as part
of the FFB gamepad interface (see `src/ffb_task.cpp` in this repo).

## What you get

Properties published to SimHub under **Odrive-Wheel Telemetry**:

| Property                       | Type   | Source            |
|-------------------------------|--------|--------------------|
| `Connected`                    | bool   | HID stream alive within 500 ms |
| `ReportRateHz`                 | double | Measured, should read ~1000 |
| `LastReportMillisAgo`          | long   | Age of most recent report |
| `WheelPositionDeg`             | double | Raw X × HalfRange / 32767 |
| `WheelPositionNormalized`      | double | −1..+1 |
| `WheelVelocityTurnsPerSec`     | double | firmware Y / 1000 |
| `WheelVelocityDegPerSec`       | double | × 360 |
| `IqCurrentA`                   | double | firmware Z / 1000 |
| `TorqueOutputNm`               | double | firmware Dial / 1000 |
| `VBusVoltage`                  | double | firmware VBus / 100 |
| `IBusCurrentA`                 | double | firmware IBus / 100 |
| `BrakeResistorCurrentA`        | double | firmware IBrake / 100 |
| `ButtonsBitmask`               | ulong  | 64-bit button state |
| `Button01`..`Button32`         | bool   | individual bits |
| `FetTempC`                     | double | FET/inverter temperature in °C (NaN if no sensor) |
| `MotorTempC`                   | double | Motor thermistor temperature in °C (NaN if no sensor) |
| `FetTempValid`                 | bool   | true iff FET thermistor is present |
| `MotorTempValid`               | bool   | true iff motor thermistor is present |
| `MotorMechPowerW`              | double | torque · ω · 2π |
| `MotorCopperLossW`             | double | 1.5 · R · Iq² (needs R_phase) |
| `BrakePowerAvgW`               | double | R · ⟨I²⟩ over 60 s (needs R_brake) |
| `ClipOutPercent`               | double | Fraction where \|Iq\| ≥ 0.95 · lim over 30 s (needs current_lim) |

No telemetry is fetched over the serial CLI channel — this plugin uses only
what the firmware already broadcasts in the 1 kHz HID stream. Temperatures
are not part of that stream and are therefore not exposed here.

## Building

Requirements:

- .NET Framework 4.8 SDK (installed by Visual Studio 2019/2022 or as a
  standalone Developer Pack)
- SimHub installed at `C:\Program Files (x86)\SimHub` (or set the `SimHubPath`
  MSBuild property to your install location)
- `dotnet` CLI (comes with any recent .NET SDK)

```bash
dotnet restore
dotnet build -c Release
```

The `AfterBuild` target copies `OdriveWheel.SimHubPlugin.dll` and
`HidSharp.dll` straight into your SimHub install directory. Restart SimHub
and enable the plugin under **Settings → Plugins**.

If SimHub is installed elsewhere:

```bash
dotnet build -c Release -p:SimHubPath="D:\SimHub"
```

## Manual install (if the copy step fails)

Copy from `bin/Release/net48/`:

- `OdriveWheel.SimHubPlugin.dll`
- `HidSharp.dll`

into your SimHub install directory (next to `SimHubWPF.exe`). Restart SimHub.

## Configuration

Open SimHub → left menu → **Odrive-Wheel**. Fill in:

- **Half range (°)** — half of `axis.range` from the Odrive HTML configurator.
  Default 450 (matches 900° total). Determines the scale of `WheelPositionDeg`.
- **Phase resistance (Ω)** — motor phase resistance from motor calibration.
  Enables `MotorCopperLossW`. Leave 0 to disable.
- **Brake resistance (Ω)** — regen brake resistor value on the board
  (typically 0.5 Ω). Enables `BrakePowerAvgW`. Leave 0 to disable.
- **Current limit (A)** — `motor.current_lim` from the HTML tool. Enables
  `ClipOutPercent`. Leave 0 to disable.

Click **Save**. Settings persist to SimHub's `PluginsData` folder.

## Coexistence

- **The game reading the wheel via DirectInput/WGI**: works. Windows allows
  multiple HID readers on the same device for Input reports, so this plugin
  and the game read the same stream in parallel with no contention.
- **The Odrive HTML configurator open at the same time**: works, as long as
  it isn't holding the HID device with `exclusive: true`. Overlay uses
  `inputreport` listeners like this plugin does, so both can coexist.
- **The plugin serial-writing to the board**: never. This plugin is read-only.
  For configuration, use the HTML tool.

## Troubleshooting

- **Plugin loads but `Connected` stays false**: unplug/replug the board,
  check Windows Device Manager for `Odrive-Wheel` under HID devices, confirm
  VID 0x1209 / PID 0x0D40. SimHub log (`Documents\SimHub\Logs`) shows the
  reader thread's device-open messages.
- **`ReportRateHz` reads much lower than 1000**: the game or another app may
  be blocking the USB endpoint. Native HID Input Reports should always
  arrive at the polling interval (1 ms), but heavy USB bus contention on
  the same hub can slow it. Move the wheel to its own USB root port.
- **`WheelPositionDeg` reads wrong**: your `HalfRangeDeg` doesn't match the
  firmware's `axis.range` setting. Divide the HTML tool value by 2.

## License

MIT. Firmware, HTML configurator, and this plugin are all part of the
[Odrive-Wheel](https://github.com/eagabriel/Odrive-Wheel) project.
