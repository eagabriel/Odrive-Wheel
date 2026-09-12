using System;
using System.Threading;

namespace OdriveWheel.SimHubPlugin
{
    /// <summary>
    /// Fixed-size circular buffer for one telemetry channel. Written by the
    /// HID reader thread (1 kHz), read by the UI thread (3-30 Hz) to draw
    /// sparklines. Short lock is fine — HID work per push is minimal and
    /// UI reads are infrequent.
    /// </summary>
    public sealed class SparklineBuffer
    {
        public const int Capacity = 240;
        private readonly double[] _data = new double[Capacity];
        private int _pos, _count;
        private readonly object _lock = new object();

        public void Push(double v)
        {
            lock (_lock)
            {
                _data[_pos] = v;
                _pos = (_pos + 1) % Capacity;
                if (_count < Capacity) _count++;
            }
        }

        public double[] Snapshot()
        {
            lock (_lock)
            {
                if (_count == 0) return Array.Empty<double>();
                var result = new double[_count];
                int start = (_pos - _count + Capacity) % Capacity;
                for (int i = 0; i < _count; i++)
                    result[i] = _data[(start + i) % Capacity];
                return result;
            }
        }

        public int Count { get { lock (_lock) return _count; } }
    }

    /// <summary>
    /// Thread-safe snapshot of the latest telemetry frame parsed from the
    /// Odrive-Wheel HID input report. The HID reader thread writes;
    /// SimHub's DataUpdate thread reads. Fields are copied under a short
    /// lock — the data is small and the rate is bounded.
    /// </summary>
    public sealed class OdriveTelemetrySnapshot
    {
        private readonly object _lock = new object();

        public bool   Connected              { get; private set; }
        public double PositionDeg            { get; private set; }
        public double PositionNormalized     { get; private set; } // -1..+1
        public double VelocityTurnsPerSec    { get; private set; }
        public double VelocityDegPerSec      { get; private set; }
        public double IqCurrentA             { get; private set; }
        public double TorqueOutputNm         { get; private set; }
        public double VBusVoltage            { get; private set; }
        public double IBusCurrentA           { get; private set; }
        public double BrakeResistorCurrentA  { get; private set; }
        public ulong  ButtonsBitmask         { get; private set; }
        public double ReportRateHz           { get; private set; }
        public long   LastReportMillisAgo    { get; private set; }

        // Temperatures — INT8_MIN (-128) from firmware = sensor not
        // connected / NaN. Plugin exposes those as double.NaN.
        public double FetTempC               { get; private set; }
        public double MotorTempC             { get; private set; }
        public bool   FetTempValid           { get; private set; }
        public bool   MotorTempValid         { get; private set; }

        // ODrive state + 4 error bitmasks. Firmware ≥ 1.3 only; older
        // firmware ships shorter reports and these stay 0 (harmless —
        // AnyError also stays false).
        public int    AxisState              { get; private set; }
        public uint   AxisError              { get; private set; }
        public uint   MotorError             { get; private set; }
        public uint   EncoderError           { get; private set; }
        public uint   ControllerError        { get; private set; }
        public bool   AnyError               { get; private set; }

        // Sparkline history — populated by HidReader at 1 kHz, downsampled
        // internally (see HidReader decimation) so the buffer holds ~60 s
        // of trend rather than 240 ms of raw 1 kHz data.
        public SparklineBuffer SparkVBus    { get; } = new SparklineBuffer();
        public SparklineBuffer SparkIq      { get; } = new SparklineBuffer();
        public SparklineBuffer SparkTorque  { get; } = new SparklineBuffer();
        public SparklineBuffer SparkFetTemp { get; } = new SparklineBuffer();

        // Scope buffers — higher rate, shorter window (~100 Hz × ~4 s).
        // Feed decimated 10× from the 1 kHz HID stream. Used by the Scope
        // card to draw fast oscilloscope-style traces of Iq and Torque.
        public SparklineBuffer ScopeIq     { get; } = new SparklineBuffer();
        public SparklineBuffer ScopeTorque { get; } = new SparklineBuffer();

        // Derived (updated by reader when config is present).
        public double MotorMechPowerW        { get; private set; }
        public double MotorCopperLossW       { get; private set; }
        public double BrakePowerAvgW         { get; private set; }
        public double ClipOutPercent         { get; private set; }

        private DateTime _lastReportUtc = DateTime.MinValue;

        public void UpdateFromReport(
            double positionDeg,
            double positionNormalized,
            double velocityTps,
            double iqA,
            double torqueNm,
            double vbusV,
            double ibusA,
            double ibrakeA,
            ulong  buttons,
            double reportRateHz,
            double motorMechW,
            double motorCopperW,
            double brakePowerW,
            double clipOutPct,
            sbyte  fetTempRaw,
            sbyte  motorTempRaw,
            bool   fetTempPresent,
            bool   motorTempPresent,
            sbyte  axisState,
            uint   axisError,
            uint   motorError,
            uint   encoderError,
            uint   controllerError,
            bool   errorsPresent)
        {
            lock (_lock)
            {
                Connected              = true;
                PositionDeg            = positionDeg;
                PositionNormalized     = positionNormalized;
                VelocityTurnsPerSec    = velocityTps;
                VelocityDegPerSec      = velocityTps * 360.0;
                IqCurrentA             = iqA;
                TorqueOutputNm         = torqueNm;
                VBusVoltage            = vbusV;
                IBusCurrentA           = ibusA;
                BrakeResistorCurrentA  = ibrakeA;
                ButtonsBitmask         = buttons;
                ReportRateHz           = reportRateHz;
                MotorMechPowerW        = motorMechW;
                MotorCopperLossW       = motorCopperW;
                BrakePowerAvgW         = brakePowerW;
                ClipOutPercent         = clipOutPct;

                // -128 = firmware sentinel for "sensor not present / NaN".
                // If the report is too short to include temps (old firmware),
                // fetTempPresent/motorTempPresent are false — also NaN.
                if (fetTempPresent && fetTempRaw != -128) {
                    FetTempC     = fetTempRaw;
                    FetTempValid = true;
                } else {
                    FetTempC     = double.NaN;
                    FetTempValid = false;
                }
                if (motorTempPresent && motorTempRaw != -128) {
                    MotorTempC     = motorTempRaw;
                    MotorTempValid = true;
                } else {
                    MotorTempC     = double.NaN;
                    MotorTempValid = false;
                }

                // ODrive state + errors. Older firmware without these fields
                // ships errorsPresent=false → stays at safe defaults (all 0).
                if (errorsPresent)
                {
                    AxisState       = axisState;
                    AxisError       = axisError;
                    MotorError      = motorError;
                    EncoderError    = encoderError;
                    ControllerError = controllerError;
                    AnyError = (axisError | motorError | encoderError | controllerError) != 0;
                }
                else
                {
                    AxisState       = 0;
                    AxisError       = 0;
                    MotorError      = 0;
                    EncoderError    = 0;
                    ControllerError = 0;
                    AnyError        = false;
                }

                _lastReportUtc = DateTime.UtcNow;
            }
        }

        public void MarkDisconnected()
        {
            lock (_lock)
            {
                Connected      = false;
                ReportRateHz   = 0;
            }
        }

        /// <summary>
        /// Called periodically by SimHub (from DataUpdate at ~60 Hz) to
        /// refresh the "milliseconds since last report" counter. Kept
        /// separate from UpdateFromReport to avoid touching the lock 1000×/s.
        /// </summary>
        public void RefreshStaleness()
        {
            lock (_lock)
            {
                if (_lastReportUtc == DateTime.MinValue)
                {
                    LastReportMillisAgo = -1;
                    return;
                }
                LastReportMillisAgo = (long)(DateTime.UtcNow - _lastReportUtc).TotalMilliseconds;
                if (LastReportMillisAgo > 500) Connected = false;
            }
        }

        public bool GetButton(int index)
        {
            if (index < 0 || index > 63) return false;
            return (ButtonsBitmask & (1UL << index)) != 0UL;
        }
    }
}
