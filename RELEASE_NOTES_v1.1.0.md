🎉 Odrive-Wheel v1.1.0 — What's new

Release focused on making force feedback actually work in the games where it silently didn't, plus tuning profiles you can switch between cars.

🎮 Force feedback fixed in a lot of titles
Nine separate defects in the USB HID PID layer were found and fixed. The worst one: on a single-axis wheel, any effect sent with a direction of 0° or 180° produced **zero torque** — the entire force landed on a Y axis that doesn't physically exist. That's the default direction many games use, which is why the wheel could go completely limp for no apparent reason.

Also fixed: Forza turned FFB off by itself because the wheel reported "no power" when asked for its state. Spring, damper and friction were muted in Assetto Corsa, AMS2 and rFactor 2 because those games send "no saturation limit" and the firmware read it as "clip everything to zero". Changing car or track could freeze the board outright. Effects came back on their own after a crash or a reset. Codemasters titles (DiRT, F1, GRID) lost spring and damper entirely. Long sessions accumulated vibration jitter and slowly leaked effect slots until the wheel started refusing new effects.

**Who it's for:** everyone. If you ever felt "the FFB just died" or "this game feels wrong and I don't know why", this is the release.

🗂 Tuning profiles
New **Profiles** tab. Save your setup under a name — GT3 dry, Rally wet, Kart — and switch between them in one click.

A profile stores how the wheel *feels*: the three FFB tabs, the EQ bands, and three tuning parameters. It deliberately leaves out motor and encoder identity, so loading a profile can never break your calibration. Profiles are ordinary files in a folder you choose, and the folder is remembered between sessions — so you can back them up, sync them through Drive or OneDrive, and send them to a friend.

Applying a profile writes to the board but doesn't commit to permanent memory: test it on track, and if you don't like it just reboot to get back what you had. Hit Save only when you're happy.

**Who it's for:** anyone who drives more than one car class, or shares a rig.

🎚 3-band EQ
New EQ in the FFB Filters tab with three bands — **WEIGHT** (5 Hz), **CHASSIS** (12 Hz) and **ROAD** (25 Hz), ±12 dB each — that reshape the forces coming from the game before they reach the motor. Raise ROAD for more kerb and surface texture, raise CHASSIS for more body roll and suspension movement.

The important part: cornering weight stays exactly the same no matter how you set the bands. You calibrate maximum torque once and never touch it again — the EQ only changes texture, never how heavy the wheel is.

**Who it's for:** drivers who want more detail without cranking overall gain, or who find one frequency range too dominant.

🧲 MT6835 encoder ready for real use
The 21-bit MagnTek MT6835 (2,097,152 counts per revolution) is now usable in production, not just experimentally.

Reading it used to strangle USB: three SPI frames plus a checksum inside the 8 kHz control interrupt, with a full peripheral reset on every switch between encoder and gate driver. Force feedback input rate collapsed from about 670 Hz to somewhere between 2 and 45 Hz. That read now runs on its own thread and the switch only rewrites what actually changes.

The configuration tool gained an MT6835 panel showing communication status, magnet strength warnings and calibration state, plus buttons to set the mechanical zero and store it permanently in the encoder chip — with the mandatory 6-second wait handled for you.

**Who it's for:** ODESC V4.2 owners using the exposed SPI port, or anyone wanting higher resolution than the AS5047.

🔇 Quieter motor, sharper effects
Damper, friction and inertia used to derive speed by differentiating raw encoder position twice, which amplified sensor noise enormously and forced the current loop bandwidth down just to hide the resulting whine.

They now read velocity straight from the encoder's own estimator. The motor is quieter, and `encoder.config.bandwidth` becomes the real control over how sharp or smooth these effects feel — you can push it to 1000–2000 Hz on absolute encoders.

↔️ Separate axis and FFB inversion
`axis.invert` and `axis.ffbinvert` are now independent. One flips which way the game sees the wheel turning, the other flips which way the forces push. Before they were tied together, so fixing one broke the other.

⚠️ Coming from an older version with `axis.invert` enabled? Turn on `axis.ffbinvert` as well to keep the previous behaviour.

🌡 Motor and MOSFET temperature
`sys.temp` and `sys.motortemp` report the inverter and motor thermistors in °C, so external tools and dashboards can watch for thermal derating.

⚡ High-impedance motors accepted
Phase inductance measurement used to reject anything above 4 mH, failing calibration on perfectly good high-impedance motors. The limit is now 25 mH.

---

**Flashing:** download `odrive-wheel-v1.1.0.hex` below and flash via DFU, or use the 📡 Fetch latest from GitHub button in the DFU tab.

**Configuration tool:** <https://eagabriel.github.io/Odrive-Wheel/> (Chrome/Edge)

**Questions and bug reports:** [join the discussion on Discord](https://discord.com/channels/704355326291607614/1499185654033158305)

Thanks to [@aksc857-stack](https://github.com/aksc857-stack) for the MT6835 driver and the invert split, and to [@TelksBr](https://github.com/TelksBr) ([ODrive-Wheel-Forge](https://github.com/TelksBr/ODrive-Wheel-Forge)) for the HID PID fix suite, thermal telemetry and the threaded encoder read.
