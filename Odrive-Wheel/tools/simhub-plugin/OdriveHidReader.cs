using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using HidSharp;

namespace OdriveWheel.SimHubPlugin
{
    /// <summary>
    /// Background thread that owns the HID device and parses input reports.
    ///
    /// Wire layout — matches Odrive-Wheel firmware (ffb_task.cpp:367-401,
    /// ffb_defs.h reportHID_t). HidSharp returns the report with the ID
    /// byte at buf[0]; payload follows.
    ///
    ///   buf[0]         report id      = 0x01
    ///   buf[1..8]      buttons        uint64 LE
    ///   buf[9..10]     X    (pos)     int16 LE, raw → deg = x/32767 × halfRange
    ///   buf[11..12]    Y    (vel)     int16 LE, /1000 = turns/s
    ///   buf[13..14]    Z    (Iq)      int16 LE, /1000 = A
    ///   buf[15..16]    RX             int16 LE — gpio, ignored
    ///   buf[17..18]    RY             int16 LE — gpio, ignored
    ///   buf[19..20]    RZ             int16 LE — gpio, ignored
    ///   buf[21..22]    Dial (torque)  int16 LE, /1000 = Nm
    ///   buf[23..24]    Slider         int16 LE — gpio, ignored
    ///   buf[25..26]    VBus           int16 LE, /100  = V
    ///   buf[27..28]    IBus           int16 LE, /100  = A
    ///   buf[29..30]    IBrake         int16 LE, /100  = A
    ///   buf[31]        FetTempC       int8, °C   (-128 = no sensor)
    ///   buf[32]        MotorTempC     int8, °C   (-128 = no sensor)
    ///   buf[33]        AxisState       int8   (ODrive AXIS_STATE enum: 0=UNDEFINED, 1=IDLE, 8=CLOSED_LOOP...)
    ///   buf[34..37]    AxisError       int32 LE  (bitmask, 0 = ERROR_NONE)
    ///   buf[38..41]    MotorError      int32 LE  (bitmask)
    ///   buf[42..45]    EncoderError    int32 LE  (bitmask)
    ///   buf[46..49]    ControllerError int32 LE  (bitmask)
    ///
    /// A shorter report (from firmware without a given tail block) is still
    /// parsed correctly — missing fields fall back to safe defaults.
    ///
    /// A blocking Read() on the HID stream unblocks 1000× per second when
    /// the firmware is up. If no report arrives for the timeout we mark
    /// the snapshot disconnected and loop again looking for the device.
    /// </summary>
    public sealed class OdriveHidReader
    {
        public const int  VID       = 0x1209;
        public const int  PID       = 0x0D40;
        public const byte REPORT_ID = 0x01;

        private readonly OdriveTelemetrySnapshot _snapshot;
        private readonly Func<OdrivePluginSettings> _getSettings;
        // Constants sourced from ProfileService (device reads). Null-safe:
        // if the service hasn't populated them yet we fall back to safe
        // defaults so the plugin still ships useful HID data day-one.
        private readonly Func<double> _getHalfRangeDeg;      // axis.range / 2
        private readonly Func<double> _getPhaseResistanceOhm;// axis0.motor.config.phase_resistance
        private readonly Func<double> _getCurrentLimA;       // axis0.motor.config.current_lim
        private readonly Func<double> _getBrakeResistanceOhm;// config.brake_resistance
        private readonly Action<string> _log;

        private Thread _thread;
        private volatile bool _running;

        // Rolling buffers for derived quantities. Sized to the max window
        // (60 s at 1 kHz ⇒ 60k samples). One entry = one HID report.
        // Time is derived from index count × 1 ms (nominal); good enough
        // for rolling means. If report rate deviates significantly the
        // rate-tracker rescales.
        private const int MAX_ROLLING = 65_000;
        private readonly Queue<double> _ibrakeSq   = new Queue<double>(MAX_ROLLING);
        private readonly Queue<double> _mechPower  = new Queue<double>(MAX_ROLLING);
        private readonly Queue<double> _copperLoss = new Queue<double>(MAX_ROLLING);
        private readonly Queue<byte>   _satMarks   = new Queue<byte>(MAX_ROLLING);

        // Report-rate tracker — average of last N inter-arrival times.
        private const int RATE_WINDOW = 200;
        private readonly Queue<long> _rateTicks = new Queue<long>(RATE_WINDOW);
        private long _lastTick;

        // Sparkline decimator — pushes to snapshot ring buffers every N reports.
        private int _sparkSkip;
        private int _scopeSkip;

        public OdriveHidReader(OdriveTelemetrySnapshot snapshot,
                               Func<OdrivePluginSettings> getSettings,
                               Func<double> getHalfRangeDeg,
                               Func<double> getPhaseResistanceOhm,
                               Func<double> getCurrentLimA,
                               Func<double> getBrakeResistanceOhm,
                               Action<string> log)
        {
            _snapshot                = snapshot;
            _getSettings             = getSettings;
            _getHalfRangeDeg         = getHalfRangeDeg          ?? (() => 450.0);
            _getPhaseResistanceOhm   = getPhaseResistanceOhm    ?? (() => 0.0);
            _getCurrentLimA          = getCurrentLimA           ?? (() => 0.0);
            _getBrakeResistanceOhm   = getBrakeResistanceOhm    ?? (() => 0.0);
            _log                     = log ?? (_ => { });
        }

        public void Start()
        {
            if (_running) return;
            _running = true;
            _thread = new Thread(RunLoop)
            {
                IsBackground = true,
                Name         = "OdriveHidReader"
            };
            _thread.Start();
        }

        public void Stop()
        {
            _running = false;
            try { _thread?.Join(500); } catch { }
            _thread = null;
        }

        private void RunLoop()
        {
            _log("HID reader thread started");

            while (_running)
            {
                HidDevice device = null;
                try
                {
                    var list = DeviceList.Local.GetHidDevices(VID, PID);
                    foreach (var d in list) { device = d; break; }

                    if (device == null)
                    {
                        _snapshot.MarkDisconnected();
                        Thread.Sleep(1000);
                        continue;
                    }

                    OpenAndPump(device);
                }
                catch (Exception ex)
                {
                    _log("HID reader error: " + ex.Message);
                    _snapshot.MarkDisconnected();
                    Thread.Sleep(1000);
                }
            }

            _log("HID reader thread stopped");
        }

        private void OpenAndPump(HidDevice device)
        {
            using (var stream = device.Open())
            {
                stream.ReadTimeout = 500;
                int reportLen = device.GetMaxInputReportLength();
                byte[] buf = new byte[reportLen];
                _log($"HID opened — {device.GetFriendlyName()} maxInput={reportLen}");

                _rateTicks.Clear();
                _lastTick = 0;

                while (_running)
                {
                    int n;
                    try
                    {
                        n = stream.Read(buf);
                    }
                    catch (TimeoutException)
                    {
                        _snapshot.MarkDisconnected();
                        continue;
                    }
                    if (n < 31)              continue;   // not our report
                    if (buf[0] != REPORT_ID) continue;

                    ParseAndPublish(buf, n);
                }
            }
        }

        private void ParseAndPublish(byte[] buf, int n)
        {
            // buf[0] = report id (0x01); actual payload starts at buf[1].
            ulong buttons = BitConverter.ToUInt64(buf, 1);
            short xRaw   = BitConverter.ToInt16(buf, 9);
            short yRaw   = BitConverter.ToInt16(buf, 11);
            short zRaw   = BitConverter.ToInt16(buf, 13);
            short dRaw   = BitConverter.ToInt16(buf, 21);
            short vbRaw  = BitConverter.ToInt16(buf, 25);
            short ibRaw  = BitConverter.ToInt16(buf, 27);
            short ibrRaw = BitConverter.ToInt16(buf, 29);

            // Temperatures at offsets 31, 32 — firmware ≥ 1.2.
            bool  tempPresent  = n >= 33;
            sbyte fetTempRaw   = tempPresent ? (sbyte)buf[31] : (sbyte)-128;
            sbyte motorTempRaw = tempPresent ? (sbyte)buf[32] : (sbyte)-128;

            // AxisState + 4 error bitmasks — firmware ≥ 1.3 (need 50-byte report).
            bool  errorsPresent = n >= 50;
            sbyte axisStateRaw  = errorsPresent ? (sbyte)buf[33] : (sbyte)0;
            uint  axisErrRaw    = errorsPresent ? BitConverter.ToUInt32(buf, 34) : 0u;
            uint  motorErrRaw   = errorsPresent ? BitConverter.ToUInt32(buf, 38) : 0u;
            uint  encErrRaw     = errorsPresent ? BitConverter.ToUInt32(buf, 42) : 0u;
            uint  ctrlErrRaw    = errorsPresent ? BitConverter.ToUInt32(buf, 46) : 0u;

            var s = _getSettings();
            double halfRange = _getHalfRangeDeg();
            if (halfRange <= 0) halfRange = 450.0;

            double posNorm = xRaw / 32767.0;
            double posDeg  = posNorm * halfRange;
            double vel     = yRaw   / 1000.0;
            double iq      = zRaw   / 1000.0;
            double torque  = dRaw   / 1000.0;
            double vbus    = vbRaw  / 100.0;
            double ibus    = ibRaw  / 100.0;
            double ibrake  = ibrRaw / 100.0;

            // Rate tracker (sliding avg of last N inter-arrival intervals).
            long now = Stopwatch.GetTimestamp();
            if (_lastTick != 0)
            {
                _rateTicks.Enqueue(now - _lastTick);
                while (_rateTicks.Count > RATE_WINDOW) _rateTicks.Dequeue();
            }
            _lastTick = now;
            double rateHz = 0;
            if (_rateTicks.Count > 4)
            {
                double sum = 0;
                foreach (var t in _rateTicks) sum += t;
                double avgSec = (sum / _rateTicks.Count) / Stopwatch.Frequency;
                if (avgSec > 0) rateHz = 1.0 / avgSec;
            }

            // Assumed sample period for rolling-window sizing. Uses the
            // measured rate when available, falls back to 1 kHz.
            double samplePeriodMs = rateHz > 100 ? 1000.0 / rateHz : 1.0;

            double phaseR       = _getPhaseResistanceOhm();
            double brakeR       = _getBrakeResistanceOhm();
            double currentLim   = _getCurrentLimA();

            double motorMechW    = torque * vel * 2.0 * Math.PI;
            double motorCopperW  = phaseR > 0 ? 1.5 * phaseR * iq * iq : 0.0;
            double brakePowerAvg = UpdateRollingBrake(ibrake * ibrake, brakeR, s, samplePeriodMs);
            double clipOutPct    = UpdateRollingClip(iq, currentLim, s, samplePeriodMs);

            // Rolling mech / copper only used if the caller wants an avg
            // exposed later — for now the instantaneous values are what
            // SimHub sees, matching the overlay's "current" reading.
            _ = _mechPower;   _ = _copperLoss;

            // Feed sparkline buffers at ~4 Hz effective (decimate 1 kHz by 250).
            // 240-sample buffer × 250 ms/sample ≈ 60 s of history — enough for
            // meaningful trend, cheap to draw.
            if (++_sparkSkip >= 250)
            {
                _sparkSkip = 0;
                _snapshot.SparkVBus.Push(vbus);
                _snapshot.SparkIq.Push(iq);
                _snapshot.SparkTorque.Push(torque);
                if (tempPresent && fetTempRaw != -128)
                    _snapshot.SparkFetTemp.Push(fetTempRaw);
            }

            // Scope: 100 Hz effective (decimate by 10). 240 samples × 10 ms
            // ≈ 2.4 s window — enough to see FFB spikes, cornering ramps.
            if (++_scopeSkip >= 10)
            {
                _scopeSkip = 0;
                _snapshot.ScopeIq.Push(iq);
                _snapshot.ScopeTorque.Push(torque);
            }

            _snapshot.UpdateFromReport(
                positionDeg:        posDeg,
                positionNormalized: posNorm,
                velocityTps:        vel,
                iqA:                iq,
                torqueNm:           torque,
                vbusV:              vbus,
                ibusA:              ibus,
                ibrakeA:            ibrake,
                buttons:            buttons,
                reportRateHz:       rateHz,
                motorMechW:         motorMechW,
                motorCopperW:       motorCopperW,
                brakePowerW:        brakePowerAvg,
                clipOutPct:         clipOutPct,
                fetTempRaw:         fetTempRaw,
                motorTempRaw:       motorTempRaw,
                fetTempPresent:     tempPresent,
                motorTempPresent:   tempPresent,
                axisState:          axisStateRaw,
                axisError:          axisErrRaw,
                motorError:         motorErrRaw,
                encoderError:       encErrRaw,
                controllerError:    ctrlErrRaw,
                errorsPresent:      errorsPresent);
        }

        private double UpdateRollingBrake(double iSquared, double brakeR, OdrivePluginSettings s, double periodMs)
        {
            int cap = (int)(s.PowerWindowSeconds * 1000.0 / periodMs);
            if (cap < 100)         cap = 100;
            if (cap > MAX_ROLLING) cap = MAX_ROLLING;

            _ibrakeSq.Enqueue(iSquared);
            while (_ibrakeSq.Count > cap) _ibrakeSq.Dequeue();

            if (brakeR <= 0 || _ibrakeSq.Count == 0) return 0.0;
            double sum = 0;
            foreach (var v in _ibrakeSq) sum += v;
            double meanI2 = sum / _ibrakeSq.Count;
            return brakeR * meanI2;
        }

        private double UpdateRollingClip(double iq, double currentLim, OdrivePluginSettings s, double periodMs)
        {
            int cap = (int)(s.ClipWindowSeconds * 1000.0 / periodMs);
            if (cap < 100)         cap = 100;
            if (cap > MAX_ROLLING) cap = MAX_ROLLING;

            if (currentLim <= 0)
            {
                _satMarks.Clear();
                return 0.0;
            }
            byte sat = (byte)(Math.Abs(iq) >= s.ClipFraction * currentLim ? 1 : 0);
            _satMarks.Enqueue(sat);
            while (_satMarks.Count > cap) _satMarks.Dequeue();

            if (_satMarks.Count == 0) return 0.0;
            int count = 0;
            foreach (var v in _satMarks) if (v != 0) count++;
            return 100.0 * count / _satMarks.Count;
        }
    }
}
