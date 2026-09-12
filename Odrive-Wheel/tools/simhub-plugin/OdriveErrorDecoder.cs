using System.Collections.Generic;
using System.Text;

namespace OdriveWheel.SimHubPlugin
{
    /// <summary>
    /// Decodes ODrive Axis/Motor/Encoder/Controller error bitmasks and the
    /// AxisState enum into human-readable names. Values pulled from
    /// ODrive-fw-v0.5.6 autogen interfaces.hpp.
    ///
    /// Note on Motor error: ODrive's motor.error is a uint64 in firmware,
    /// but the HID field is int32 so bits ≥ 32 (four rarely-seen "unknown"
    /// initialization errors) are truncated. Not exposed here.
    /// </summary>
    public static class OdriveErrorDecoder
    {
        // ----- AxisState (interfaces.hpp:234) --------------------------------

        private static readonly Dictionary<int, string> _axisStates = new Dictionary<int, string>
        {
            {  0, "UNDEFINED" },
            {  1, "IDLE" },
            {  2, "STARTUP_SEQUENCE" },
            {  3, "FULL_CALIBRATION_SEQUENCE" },
            {  4, "MOTOR_CALIBRATION" },
            {  6, "ENCODER_INDEX_SEARCH" },
            {  7, "ENCODER_OFFSET_CALIBRATION" },
            {  8, "CLOSED_LOOP_CONTROL" },
            {  9, "LOCKIN_SPIN" },
            { 10, "ENCODER_DIR_FIND" },
            { 11, "HOMING" },
            { 12, "ENCODER_HALL_POLARITY_CALIBRATION" },
            { 13, "ENCODER_HALL_PHASE_CALIBRATION" },
        };

        public static string DecodeAxisState(int state)
        {
            return _axisStates.TryGetValue(state, out var name)
                ? state + " — " + name
                : state + " — ?";
        }

        // ----- Axis::Error (interfaces.hpp:219) ------------------------------

        private static readonly (uint bit, string name)[] _axisBits = new[]
        {
            (0x00000001u, "INVALID_STATE"),
            (0x00000040u, "MOTOR_FAILED"),
            (0x00000080u, "SENSORLESS_ESTIMATOR_FAILED"),
            (0x00000100u, "ENCODER_FAILED"),
            (0x00000200u, "CONTROLLER_FAILED"),
            (0x00000800u, "WATCHDOG_TIMER_EXPIRED"),
            (0x00001000u, "MIN_ENDSTOP_PRESSED"),
            (0x00002000u, "MAX_ENDSTOP_PRESSED"),
            (0x00004000u, "ESTOP_REQUESTED"),
            (0x00020000u, "HOMING_WITHOUT_ENDSTOP"),
            (0x00040000u, "OVER_TEMP"),
            (0x00080000u, "UNKNOWN_POSITION"),
        };

        // ----- Motor::Error (interfaces.hpp:417) — low 32 bits only ---------

        private static readonly (uint bit, string name)[] _motorBits = new[]
        {
            (0x00000001u, "PHASE_RESISTANCE_OUT_OF_RANGE"),
            (0x00000002u, "PHASE_INDUCTANCE_OUT_OF_RANGE"),
            (0x00000008u, "DRV_FAULT"),
            (0x00000010u, "CONTROL_DEADLINE_MISSED"),
            (0x00000080u, "MODULATION_MAGNITUDE"),
            (0x00000400u, "CURRENT_SENSE_SATURATION"),
            (0x00001000u, "CURRENT_LIMIT_VIOLATION"),
            (0x00010000u, "MODULATION_IS_NAN"),
            (0x00020000u, "MOTOR_THERMISTOR_OVER_TEMP"),
            (0x00040000u, "FET_THERMISTOR_OVER_TEMP"),
            (0x00080000u, "TIMER_UPDATE_MISSED"),
            (0x00100000u, "CURRENT_MEASUREMENT_UNAVAILABLE"),
            (0x00200000u, "CONTROLLER_FAILED"),
            (0x00400000u, "I_BUS_OUT_OF_RANGE"),
            (0x00800000u, "BRAKE_RESISTOR_DISARMED"),
            (0x01000000u, "SYSTEM_LEVEL"),
            (0x02000000u, "BAD_TIMING"),
            (0x04000000u, "UNKNOWN_PHASE_ESTIMATE"),
            (0x08000000u, "UNKNOWN_PHASE_VEL"),
            (0x10000000u, "UNKNOWN_TORQUE"),
            (0x20000000u, "UNKNOWN_CURRENT_COMMAND"),
            (0x40000000u, "UNKNOWN_CURRENT_MEASUREMENT"),
            (0x80000000u, "UNKNOWN_VBUS_VOLTAGE"),
        };

        // ----- Encoder::Error (interfaces.hpp:739) ---------------------------

        private static readonly (uint bit, string name)[] _encoderBits = new[]
        {
            (0x00000001u, "UNSTABLE_GAIN"),
            (0x00000002u, "CPR_POLEPAIRS_MISMATCH"),
            (0x00000004u, "NO_RESPONSE"),
            (0x00000008u, "UNSUPPORTED_ENCODER_MODE"),
            (0x00000010u, "ILLEGAL_HALL_STATE"),
            (0x00000020u, "INDEX_NOT_FOUND_YET"),
            (0x00000040u, "ABS_SPI_TIMEOUT"),
            (0x00000080u, "ABS_SPI_COM_FAIL"),
            (0x00000100u, "ABS_SPI_NOT_READY"),
            (0x00000200u, "HALL_NOT_CALIBRATED_YET"),
        };

        // ----- Controller::Error (interfaces.hpp:614) ------------------------

        private static readonly (uint bit, string name)[] _controllerBits = new[]
        {
            (0x00000001u, "OVERSPEED"),
            (0x00000002u, "INVALID_INPUT_MODE"),
            (0x00000004u, "UNSTABLE_GAIN"),
            (0x00000008u, "INVALID_MIRROR_AXIS"),
            (0x00000010u, "INVALID_LOAD_ENCODER"),
            (0x00000020u, "INVALID_ESTIMATE"),
            (0x00000040u, "INVALID_CIRCULAR_RANGE"),
            (0x00000080u, "SPINOUT_DETECTED"),
        };

        // ----- Public API ----------------------------------------------------

        public static string DecodeAxisError(uint mask)      => Decode(mask, _axisBits);
        public static string DecodeMotorError(uint mask)     => Decode(mask, _motorBits);
        public static string DecodeEncoderError(uint mask)   => Decode(mask, _encoderBits);
        public static string DecodeControllerError(uint mask)=> Decode(mask, _controllerBits);

        private static string Decode(uint mask, (uint bit, string name)[] table)
        {
            if (mask == 0) return "0x00000000 — NONE";
            var sb = new StringBuilder();
            sb.Append("0x").Append(mask.ToString("X8")).Append(" — ");
            bool first = true;
            uint matched = 0;
            foreach (var e in table)
            {
                if ((mask & e.bit) != 0)
                {
                    if (!first) sb.Append(" | ");
                    sb.Append(e.name);
                    first = false;
                    matched |= e.bit;
                }
            }
            uint unknown = mask & ~matched;
            if (unknown != 0)
            {
                if (!first) sb.Append(" | ");
                sb.Append("unknown(0x").Append(unknown.ToString("X8")).Append(")");
            }
            return sb.ToString();
        }
    }
}
