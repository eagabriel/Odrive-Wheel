using System.Collections.Generic;

namespace OdriveWheel.SimHubPlugin
{
    public enum FieldProtocol
    {
        /// <summary>OpenFFBoard "class.property" syntax (axis.range, fx.master, ...).</summary>
        Openffb,
        /// <summary>ODrive ASCII "r path" / "w path value" (axis0.motor.config.*).</summary>
        Odrive,
    }

    public enum FieldType
    {
        Double,   // free-form float
        Integer,  // whole number
        Boolean,  // "0"/"1" or "True"/"False"
    }

    public sealed class ProfileField
    {
        /// <summary>Firmware-side path (e.g. "axis.range", "axis0.motor.config.current_lim").</summary>
        public string        Path       { get; }
        /// <summary>OpenFFBoard vs ODrive ASCII.</summary>
        public FieldProtocol Protocol   { get; }
        /// <summary>Property name exposed to SimHub dashboards (CamelCase).</summary>
        public string        PropName   { get; }
        /// <summary>How to parse device-returned string into a value.</summary>
        public FieldType     Type       { get; }
        /// <summary>Optional short description shown in the plugin UI.</summary>
        public string        Description { get; }

        public ProfileField(string path, FieldProtocol proto, string propName, FieldType type, string desc = null)
        {
            Path        = path;
            Protocol    = proto;
            PropName    = propName;
            Type        = type;
            Description = desc ?? "";
        }
    }

    /// <summary>
    /// Full profile scope, matching what the HTML tool's _profCaptureInputs
    /// captures: 25 FFB axis/fx params + 3 EQ + 3 ODrive extras = 31 fields.
    /// Order matters for the "apply profile" sequence — some fields depend
    /// on others (e.g. axis.range affects position scaling; write it first).
    /// </summary>
    public static class OdriveProfileFields
    {
        public static readonly IReadOnlyList<ProfileField> All = new List<ProfileField>
        {
            // ----- ODrive extras (motor/encoder config) -----
            // Applied FIRST because these are the "hardware" limits — they
            // constrain what the FFB layer can do.
            new ProfileField("axis0.motor.config.current_lim",              FieldProtocol.Odrive, "MotorCurrentLimA",             FieldType.Double,  "Motor current limit (A) — hard cap on torque"),
            new ProfileField("axis0.motor.config.current_control_bandwidth",FieldProtocol.Odrive, "CurrentControlBandwidthHz",    FieldType.Double,  "Current loop bandwidth (Hz) — motor grit/noise trade-off"),
            new ProfileField("axis0.encoder.config.bandwidth",              FieldProtocol.Odrive, "EncoderBandwidthHz",           FieldType.Double,  "Encoder PLL bandwidth (Hz) — position/velocity smoothing"),

            // ----- FFB axis params (12) -----
            new ProfileField("axis.range",         FieldProtocol.Openffb, "AxisRangeDeg",       FieldType.Integer, "Wheel range in degrees"),
            new ProfileField("axis.maxtorque",     FieldProtocol.Openffb, "AxisMaxTorqueNm",    FieldType.Double,  "Max torque cap (Nm)"),
            new ProfileField("axis.fxratio",       FieldProtocol.Openffb, "AxisFxRatio",        FieldType.Double,  "FX gain ratio (0..1)"),
            new ProfileField("axis.invert",        FieldProtocol.Openffb, "AxisInvert",         FieldType.Boolean, "Invert wheel direction"),
            new ProfileField("axis.ffbinvert",     FieldProtocol.Openffb, "AxisFfbInvert",      FieldType.Boolean, "Invert FFB output direction"),
            new ProfileField("axis.idlespring",    FieldProtocol.Openffb, "AxisIdleSpring",     FieldType.Integer, "Idle spring strength"),
            new ProfileField("axis.axisdamper",    FieldProtocol.Openffb, "AxisDamper",         FieldType.Integer, "Constant damping"),
            new ProfileField("axis.axisinertia",   FieldProtocol.Openffb, "AxisInertia",        FieldType.Integer, "Constant inertia"),
            new ProfileField("axis.axisfriction",  FieldProtocol.Openffb, "AxisFriction",       FieldType.Integer, "Constant friction"),
            new ProfileField("axis.esgain",        FieldProtocol.Openffb, "AxisEndstopGain",    FieldType.Integer, "Endstop spring gain"),
            new ProfileField("axis.esdamp",        FieldProtocol.Openffb, "AxisEndstopDamp",    FieldType.Integer, "Endstop damper strength"),
            new ProfileField("axis.maxtorquerate", FieldProtocol.Openffb, "AxisMaxTorqueRate",  FieldType.Integer, "Max torque slew rate"),
            new ProfileField("axis.expo",          FieldProtocol.Openffb, "AxisExpo",           FieldType.Integer, "Expo curve"),
            new ProfileField("axis.exposcale",     FieldProtocol.Openffb, "AxisExpoScale",      FieldType.Integer, "Expo scale"),

            // ----- FX master + effect gains (5) -----
            new ProfileField("fx.master",   FieldProtocol.Openffb, "FxMaster",   FieldType.Integer, "Master FX gain (0..255)"),
            new ProfileField("fx.spring",   FieldProtocol.Openffb, "FxSpring",   FieldType.Integer, "Spring effect gain (0..255)"),
            new ProfileField("fx.damper",   FieldProtocol.Openffb, "FxDamper",   FieldType.Integer, "Damper effect gain (0..255)"),
            new ProfileField("fx.friction", FieldProtocol.Openffb, "FxFriction", FieldType.Integer, "Friction effect gain (0..255)"),
            new ProfileField("fx.inertia",  FieldProtocol.Openffb, "FxInertia",  FieldType.Integer, "Inertia effect gain (0..255)"),

            // ----- FX filter freq/Q (8) -----
            new ProfileField("fx.filterCfFreq", FieldProtocol.Openffb, "FxConstantForceFreqHz",FieldType.Integer, "Constant Force LPF freq"),
            new ProfileField("fx.filterCfQ",    FieldProtocol.Openffb, "FxConstantForceQ",     FieldType.Integer, "Constant Force LPF Q (×100)"),
            new ProfileField("fx.filterFrFreq", FieldProtocol.Openffb, "FxFrictionFreqHz",     FieldType.Integer, "Friction LPF freq"),
            new ProfileField("fx.filterFrQ",    FieldProtocol.Openffb, "FxFrictionQ",          FieldType.Integer, "Friction LPF Q (×100)"),
            new ProfileField("fx.filterDaFreq", FieldProtocol.Openffb, "FxDamperFreqHz",       FieldType.Integer, "Damper LPF freq"),
            new ProfileField("fx.filterDaQ",    FieldProtocol.Openffb, "FxDamperQ",            FieldType.Integer, "Damper LPF Q (×100)"),
            new ProfileField("fx.filterInFreq", FieldProtocol.Openffb, "FxInertiaFreqHz",      FieldType.Integer, "Inertia LPF freq"),
            new ProfileField("fx.filterInQ",    FieldProtocol.Openffb, "FxInertiaQ",           FieldType.Integer, "Inertia LPF Q (×100)"),

            // ----- 3-band EQ (3) -----
            new ProfileField("axis.eqweight",  FieldProtocol.Openffb, "EqWeightGainDb",  FieldType.Double, "3-band EQ: weight band gain (dB)"),
            new ProfileField("axis.eqchassis", FieldProtocol.Openffb, "EqChassisGainDb", FieldType.Double, "3-band EQ: chassis band gain (dB)"),
            new ProfileField("axis.eqroad",    FieldProtocol.Openffb, "EqRoadGainDb",    FieldType.Double, "3-band EQ: road band gain (dB)"),
        };

        // ----- Helpers -----

        /// <summary>Fields to read from device even outside a profile — needed
        /// for scaling and derived math. Currently subset of All plus
        /// axis0.motor.config.phase_resistance (used by CopperLoss). </summary>
        public static readonly IReadOnlyList<ProfileField> ExtrasNotInProfile = new List<ProfileField>
        {
            new ProfileField("axis0.motor.config.phase_resistance", FieldProtocol.Odrive, "MotorPhaseResistanceOhm", FieldType.Double, "Motor phase resistance (Ω) — from motor cal"),
            new ProfileField("config.brake_resistance",             FieldProtocol.Odrive, "BrakeResistanceOhm",      FieldType.Double, "Regen brake resistor value (Ω)"),
        };

        /// <summary>Union of All + ExtrasNotInProfile — everything the plugin reads.</summary>
        public static IEnumerable<ProfileField> AllReadable
        {
            get
            {
                foreach (var f in All)                yield return f;
                foreach (var f in ExtrasNotInProfile) yield return f;
            }
        }
    }
}
