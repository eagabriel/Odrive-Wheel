using System;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using GameReaderCommon;
using SimHub.Plugins;

namespace OdriveWheel.SimHubPlugin
{
    [PluginDescription("Reads Odrive-Wheel telemetry over USB HID at 1 kHz + syncs profile config via serial CLI.")]
    [PluginAuthor("Egabriel")]
    [PluginName("Odrive-Wheel Telemetry")]
    public sealed class OdriveWheelPlugin : IPlugin, IDataPlugin, IWPFSettingsV2
    {
        public PluginManager        PluginManager { get; set; }
        public OdrivePluginSettings Settings      { get; private set; }
        public OdriveTelemetrySnapshot Snapshot   { get; private set; }
        public OdriveProfileService   ProfileService { get; private set; }
        public OdriveSerialCli        SerialCli   { get; private set; }

        private OdriveHidReader _reader;
        private string _lastSeenGame = "";

        // IWPFSettingsV2
        public string LeftMenuTitle => "Odrive-Wheel";
        public ImageSource PictureIcon => BuildMenuIcon();
        public System.Windows.Controls.Control GetWPFSettingsControl(PluginManager pluginManager)
            => new SettingsControl(this);

        // Menu icon — SimHub renders sidebar icons as a single-color mask
        // (any opaque pixel becomes the sidebar tint colour). That means a
        // colourful PNG like our badge just fills as a white rectangle. So
        // this icon has to be a WHITE-ON-TRANSPARENT WIREFRAME: only strokes,
        // no fills — SimHub then draws them as visible outlines.
        //
        // Design references the shield-wheel-gear badge but stripped down:
        //   - Outer shield (rounded triangle) — reads as "badge"
        //   - Inner wheel rim (circle) — reads as "wheel"
        //   - 6 gear teeth around rim — reads as "motor / gear"
        //   - Four thin spokes to a small hub — reads as "steering wheel"
        //
        // The full colour badge lives in the header banner where we control
        // rendering directly.
        private static ImageSource BuildMenuIcon()
        {
            var white = Brushes.White;
            var stroke = new Pen(white, 1.6)
            {
                StartLineCap = PenLineCap.Round,
                EndLineCap   = PenLineCap.Round,
                LineJoin     = PenLineJoin.Round,
            };
            stroke.Freeze();
            var thin = new Pen(white, 1.1)
            {
                StartLineCap = PenLineCap.Round,
                EndLineCap   = PenLineCap.Round,
            };
            thin.Freeze();

            var dg = new DrawingGroup();
            using (var dc = dg.Open())
            {
                const double cx = 16, cy = 16;

                // Shield outline — approximates the rounded-triangle silhouette
                // of the badge. Three arcs connected by short verticals.
                var shield = new StreamGeometry();
                using (var g = shield.Open())
                {
                    // Start top-left
                    g.BeginFigure(new Point(6, 4), isFilled: false, isClosed: true);
                    g.LineTo   (new Point(26, 4), isStroked: true, isSmoothJoin: false);
                    g.ArcTo    (new Point(28.5, 18.5), new Size(4, 6), 0, false,
                                SweepDirection.Clockwise, true, false);
                    g.LineTo   (new Point(16, 30), isStroked: true, isSmoothJoin: false);
                    g.LineTo   (new Point(3.5, 18.5), isStroked: true, isSmoothJoin: false);
                    g.ArcTo    (new Point(6, 4), new Size(4, 6), 0, false,
                                SweepDirection.Clockwise, true, false);
                }
                shield.Freeze();
                dc.DrawGeometry(null, stroke, shield);

                // Inner wheel rim
                dc.DrawEllipse(null, stroke, new Point(cx, cy), 8, 8);

                // 6 gear teeth (small radial dashes just outside rim)
                for (int i = 0; i < 6; i++)
                {
                    double a = i * Math.PI / 3 + Math.PI / 6;
                    double c = Math.Cos(a), s = Math.Sin(a);
                    dc.DrawLine(thin,
                        new Point(cx + 8.4 * c, cy + 8.4 * s),
                        new Point(cx + 9.6 * c, cy + 9.6 * s));
                }

                // 4 spokes hub→rim (cross)
                for (int i = 0; i < 4; i++)
                {
                    double a = i * Math.PI / 2;
                    double c = Math.Cos(a), s = Math.Sin(a);
                    dc.DrawLine(stroke,
                        new Point(cx + 2.2 * c, cy + 2.2 * s),
                        new Point(cx + 7.2 * c, cy + 7.2 * s));
                }

                // Hub — small OUTLINED ring (not filled) so it reads as a hole
                dc.DrawEllipse(null, stroke, new Point(cx, cy), 2.2, 2.2);
            }
            var img = new DrawingImage(dg);
            img.Freeze();
            return img;
        }

        public void Init(PluginManager pluginManager)
        {
            SimHub.Logging.Current.Info("Odrive-Wheel plugin — Init");

            Settings = this.ReadCommonSettings("Settings", () => new OdrivePluginSettings());
            Snapshot = new OdriveTelemetrySnapshot();

            SerialCli      = new OdriveSerialCli(msg => SimHub.Logging.Current.Info("OdriveSerial: " + msg));
            ProfileService = new OdriveProfileService(
                SerialCli,
                () => Settings,
                msg => SimHub.Logging.Current.Info("OdriveProfile: " + msg));
            ProfileService.Start();

            _reader = new OdriveHidReader(
                Snapshot,
                () => Settings,
                // Constants sourced from ProfileService — updated as the
                // service reads them from the device.
                () => AsDouble(ProfileService.GetTyped(FindField("axis.range"))) / 2.0,
                () => AsDouble(ProfileService.GetTyped(FindField("axis0.motor.config.phase_resistance"))),
                () => AsDouble(ProfileService.GetTyped(FindField("axis0.motor.config.current_lim"))),
                () => AsDouble(ProfileService.GetTyped(FindField("config.brake_resistance"))),
                msg => SimHub.Logging.Current.Info("OdriveHid: " + msg));
            _reader.Start();

            RegisterProperties(pluginManager);
            OdriveActions.Register(pluginManager, this);
        }

        public void DataUpdate(PluginManager pluginManager, ref GameData data)
        {
            if (Snapshot == null) return;
            Snapshot.RefreshStaleness();

            PublishHidProperties(pluginManager);
            PublishProfileProperties(pluginManager);
            PublishServiceStatus(pluginManager);

            // Auto-per-game hook.
            var g = data?.GameName ?? "";
            if (!string.IsNullOrEmpty(g) && g != _lastSeenGame)
            {
                _lastSeenGame = g;
                try { ProfileService.MaybeAutoLoadForGame(g); }
                catch (Exception ex) { SimHub.Logging.Current.Info("auto-per-game: " + ex.Message); }
            }
        }

        public void End(PluginManager pluginManager)
        {
            SimHub.Logging.Current.Info("Odrive-Wheel plugin — End");
            _reader?.Stop();
            _reader = null;
            ProfileService?.Stop();
            ProfileService = null;
            this.SaveCommonSettings("Settings", Settings);
        }

        public void SaveSettings() => this.SaveCommonSettings("Settings", Settings);

        // ---- Property registration ------------------------------------------

        private void RegisterProperties(PluginManager pm)
        {
            var t = GetType();

            // HID direct fields
            pm.AddProperty("Connected",                  t, typeof(bool));
            pm.AddProperty("ReportRateHz",               t, typeof(double));
            pm.AddProperty("LastReportMillisAgo",        t, typeof(long));

            pm.AddProperty("WheelPositionDeg",           t, typeof(double));
            pm.AddProperty("WheelPositionNormalized",    t, typeof(double));
            pm.AddProperty("WheelVelocityTurnsPerSec",   t, typeof(double));
            pm.AddProperty("WheelVelocityDegPerSec",     t, typeof(double));

            pm.AddProperty("IqCurrentA",                 t, typeof(double));
            pm.AddProperty("TorqueOutputNm",             t, typeof(double));
            pm.AddProperty("VBusVoltage",                t, typeof(double));
            pm.AddProperty("IBusCurrentA",               t, typeof(double));
            pm.AddProperty("BrakeResistorCurrentA",      t, typeof(double));

            pm.AddProperty("ButtonsBitmask",             t, typeof(ulong));
            for (int i = 1; i <= 32; i++)
                pm.AddProperty("Button" + i.ToString("00"), t, typeof(bool));

            // Temperatures
            pm.AddProperty("FetTempC",                   t, typeof(double));
            pm.AddProperty("MotorTempC",                 t, typeof(double));
            pm.AddProperty("FetTempValid",               t, typeof(bool));
            pm.AddProperty("MotorTempValid",             t, typeof(bool));

            // ODrive state + error bitmasks (firmware 1.3+; older = 0/false)
            pm.AddProperty("AxisState",                  t, typeof(long));
            pm.AddProperty("AxisError",                  t, typeof(long));
            pm.AddProperty("MotorError",                 t, typeof(long));
            pm.AddProperty("EncoderError",               t, typeof(long));
            pm.AddProperty("ControllerError",            t, typeof(long));
            pm.AddProperty("AnyError",                   t, typeof(bool));

            // Derived
            pm.AddProperty("MotorMechPowerW",            t, typeof(double));
            pm.AddProperty("MotorCopperLossW",           t, typeof(double));
            pm.AddProperty("BrakePowerAvgW",             t, typeof(double));
            pm.AddProperty("ClipOutPercent",             t, typeof(double));

            // Profile fields — every one from the spec. Type = double for
            // numerics (unified), bool for booleans.
            foreach (var f in OdriveProfileFields.All)
            {
                pm.AddProperty(f.PropName, t, f.Type == FieldType.Boolean ? typeof(bool) : typeof(double));
            }
            foreach (var f in OdriveProfileFields.ExtrasNotInProfile)
            {
                pm.AddProperty(f.PropName, t, f.Type == FieldType.Boolean ? typeof(bool) : typeof(double));
            }

            // Service status
            pm.AddProperty("SerialPortName",            t, typeof(string));
            pm.AddProperty("SerialLastRefreshSecondsAgo", t, typeof(double));
        }

        private void PublishHidProperties(PluginManager pm)
        {
            var t = GetType();
            pm.SetPropertyValue("Connected",                  t, Snapshot.Connected);
            pm.SetPropertyValue("ReportRateHz",               t, Snapshot.ReportRateHz);
            pm.SetPropertyValue("LastReportMillisAgo",        t, Snapshot.LastReportMillisAgo);

            pm.SetPropertyValue("WheelPositionDeg",           t, Snapshot.PositionDeg);
            pm.SetPropertyValue("WheelPositionNormalized",    t, Snapshot.PositionNormalized);
            pm.SetPropertyValue("WheelVelocityTurnsPerSec",   t, Snapshot.VelocityTurnsPerSec);
            pm.SetPropertyValue("WheelVelocityDegPerSec",     t, Snapshot.VelocityDegPerSec);

            pm.SetPropertyValue("IqCurrentA",                 t, Snapshot.IqCurrentA);
            pm.SetPropertyValue("TorqueOutputNm",             t, Snapshot.TorqueOutputNm);
            pm.SetPropertyValue("VBusVoltage",                t, Snapshot.VBusVoltage);
            pm.SetPropertyValue("IBusCurrentA",               t, Snapshot.IBusCurrentA);
            pm.SetPropertyValue("BrakeResistorCurrentA",      t, Snapshot.BrakeResistorCurrentA);

            pm.SetPropertyValue("ButtonsBitmask",             t, Snapshot.ButtonsBitmask);
            for (int i = 1; i <= 32; i++)
                pm.SetPropertyValue("Button" + i.ToString("00"), t, Snapshot.GetButton(i - 1));

            pm.SetPropertyValue("FetTempC",                   t, Snapshot.FetTempC);
            pm.SetPropertyValue("MotorTempC",                 t, Snapshot.MotorTempC);
            pm.SetPropertyValue("FetTempValid",               t, Snapshot.FetTempValid);
            pm.SetPropertyValue("MotorTempValid",             t, Snapshot.MotorTempValid);

            pm.SetPropertyValue("AxisState",                  t, (long)Snapshot.AxisState);
            pm.SetPropertyValue("AxisError",                  t, (long)Snapshot.AxisError);
            pm.SetPropertyValue("MotorError",                 t, (long)Snapshot.MotorError);
            pm.SetPropertyValue("EncoderError",               t, (long)Snapshot.EncoderError);
            pm.SetPropertyValue("ControllerError",            t, (long)Snapshot.ControllerError);
            pm.SetPropertyValue("AnyError",                   t, Snapshot.AnyError);

            pm.SetPropertyValue("MotorMechPowerW",            t, Snapshot.MotorMechPowerW);
            pm.SetPropertyValue("MotorCopperLossW",           t, Snapshot.MotorCopperLossW);
            pm.SetPropertyValue("BrakePowerAvgW",             t, Snapshot.BrakePowerAvgW);
            pm.SetPropertyValue("ClipOutPercent",             t, Snapshot.ClipOutPercent);
        }

        private void PublishProfileProperties(PluginManager pm)
        {
            var t = GetType();
            foreach (var f in OdriveProfileFields.All)
                PublishProfileField(pm, t, f);
            foreach (var f in OdriveProfileFields.ExtrasNotInProfile)
                PublishProfileField(pm, t, f);
        }

        private void PublishProfileField(PluginManager pm, Type t, ProfileField f)
        {
            var v = ProfileService.GetTyped(f);
            if (f.Type == FieldType.Boolean)
            {
                pm.SetPropertyValue(f.PropName, t, v is bool b && b);
            }
            else
            {
                pm.SetPropertyValue(f.PropName, t, AsDouble(v));
            }
        }

        private void PublishServiceStatus(PluginManager pm)
        {
            var t = GetType();
            pm.SetPropertyValue("SerialPortName", t, ProfileService.CurrentPortName ?? "");
            var refUtc = ProfileService.LastRefreshUtc;
            var age = refUtc == DateTime.MinValue ? -1.0
                    : (DateTime.UtcNow - refUtc).TotalSeconds;
            pm.SetPropertyValue("SerialLastRefreshSecondsAgo", t, age);
        }

        // ---- Helpers --------------------------------------------------------

        private static double AsDouble(object v)
        {
            if (v == null) return 0.0;
            if (v is double d) return d;
            if (v is long l)   return l;
            if (v is int i)    return i;
            if (v is float f)  return f;
            if (v is bool b)   return b ? 1.0 : 0.0;
            return 0.0;
        }

        private static ProfileField FindField(string path)
        {
            foreach (var f in OdriveProfileFields.All)
                if (f.Path == path) return f;
            foreach (var f in OdriveProfileFields.ExtrasNotInProfile)
                if (f.Path == path) return f;
            return null;
        }
    }
}
