using System;
using System.Globalization;
using SimHub.Plugins;

namespace OdriveWheel.SimHubPlugin
{
    /// <summary>
    /// SimHub mappable actions — increment/decrement of a curated set of
    /// live-tunable properties + one one-shot (Zero Wheel Position).
    ///
    /// Each inc/dec follows the same recipe:
    ///   1. lock (so back-to-back button presses can't interleave)
    ///   2. read current value from the ProfileService cache (instant —
    ///      the plugin is the sole writer, so the cache is authoritative
    ///      unless the user has been tuning in the HTML tool, in which
    ///      case they hit the manual refresh button first)
    ///   3. parse → clamp(current + delta) → format
    ///   4. serial write
    ///   5. PokeCache with the newly written value — dashboards and the
    ///      settings panel reflect the change immediately, no extra read
    ///
    /// SimHub renders the RegistrationName (the string passed to
    /// pluginManager.AddAction) under Controls & Events → Additional
    /// plugins → Odrive-Wheel Telemetry. Names use CamelCase and encode
    /// direction + step size in the identifier itself so the mapping UI
    /// is self-explanatory.
    /// </summary>
    public static class OdriveActions
    {
        // Registered from OdriveWheelPlugin.Init.
        public static void Register(PluginManager pm, OdriveWheelPlugin plugin)
        {
            // ---- FX Master (int, 0..255, ±10 fine / ±25 coarse) ----
            RegisterIncInt(pm, plugin, "FxMasterUpFine",     "fx.master",   +10, 0, 255);
            RegisterIncInt(pm, plugin, "FxMasterDownFine",   "fx.master",   -10, 0, 255);
            RegisterIncInt(pm, plugin, "FxMasterUpCoarse",   "fx.master",   +25, 0, 255);
            RegisterIncInt(pm, plugin, "FxMasterDownCoarse", "fx.master",   -25, 0, 255);

            // ---- FX Ratio (double, 0..1, ±0.05 fine / ±0.20 coarse) ----
            RegisterIncFloat(pm, plugin, "FxRatioUpFine",     "axis.fxratio", +0.05, 0.0, 1.0, "F2");
            RegisterIncFloat(pm, plugin, "FxRatioDownFine",   "axis.fxratio", -0.05, 0.0, 1.0, "F2");
            RegisterIncFloat(pm, plugin, "FxRatioUpCoarse",   "axis.fxratio", +0.20, 0.0, 1.0, "F2");
            RegisterIncFloat(pm, plugin, "FxRatioDownCoarse", "axis.fxratio", -0.20, 0.0, 1.0, "F2");

            // ---- EQ Weight (dB, -12..+12, ±0.5 fine / ±2 coarse) ----
            RegisterIncFloat(pm, plugin, "EqWeightUpFine",     "axis.eqweight", +0.5, -12.0, +12.0, "F1");
            RegisterIncFloat(pm, plugin, "EqWeightDownFine",   "axis.eqweight", -0.5, -12.0, +12.0, "F1");
            RegisterIncFloat(pm, plugin, "EqWeightUpCoarse",   "axis.eqweight", +2.0, -12.0, +12.0, "F1");
            RegisterIncFloat(pm, plugin, "EqWeightDownCoarse", "axis.eqweight", -2.0, -12.0, +12.0, "F1");

            // ---- EQ Chassis (dB, -12..+12, ±0.5 fine / ±2 coarse) ----
            RegisterIncFloat(pm, plugin, "EqChassisUpFine",     "axis.eqchassis", +0.5, -12.0, +12.0, "F1");
            RegisterIncFloat(pm, plugin, "EqChassisDownFine",   "axis.eqchassis", -0.5, -12.0, +12.0, "F1");
            RegisterIncFloat(pm, plugin, "EqChassisUpCoarse",   "axis.eqchassis", +2.0, -12.0, +12.0, "F1");
            RegisterIncFloat(pm, plugin, "EqChassisDownCoarse", "axis.eqchassis", -2.0, -12.0, +12.0, "F1");

            // ---- EQ Road (dB, -12..+12, ±0.5 fine / ±2 coarse) ----
            RegisterIncFloat(pm, plugin, "EqRoadUpFine",     "axis.eqroad", +0.5, -12.0, +12.0, "F1");
            RegisterIncFloat(pm, plugin, "EqRoadDownFine",   "axis.eqroad", -0.5, -12.0, +12.0, "F1");
            RegisterIncFloat(pm, plugin, "EqRoadUpCoarse",   "axis.eqroad", +2.0, -12.0, +12.0, "F1");
            RegisterIncFloat(pm, plugin, "EqRoadDownCoarse", "axis.eqroad", -2.0, -12.0, +12.0, "F1");

            // ---- Zero Wheel Position (one-shot) ----
            pm.AddAction<OdriveWheelPlugin>("ZeroWheelPosition", (a, b) =>
            {
                try
                {
                    // axis.zeroenc! is a firmware-native exec that snapshots
                    // the current wheel position and applies it as the
                    // zero offset in one atomic op. No read-modify-write
                    // needed from the plugin side.
                    var reply = plugin.SerialCli.ExecOffb("axis.zeroenc");
                    if (reply != null)
                    {
                        SimHub.Logging.Current.Info("Odrive-Wheel: ZeroWheelPosition → " + reply);
                        // Firmware axis.zeroenc computes the new offset from
                        // the LIVE position — we don't know the number to
                        // poke, and the value isn't in the profile scope
                        // anyway. Users interested in axis.zeroofs can hit
                        // "Refresh from device now" if they care.
                    }
                }
                catch (Exception ex)
                {
                    SimHub.Logging.Current.Info("Odrive-Wheel: ZeroWheelPosition failed: " + ex.Message);
                }
            });
        }

        // ---- Integer increment helper --------------------------------------

        private static readonly object _incLock = new object();

        private static void RegisterIncInt(PluginManager pm, OdriveWheelPlugin plugin,
                                           string actionName, string path, int delta,
                                           int min, int max)
        {
            pm.AddAction<OdriveWheelPlugin>(actionName, (a, b) =>
            {
                try { IncrementInt(plugin, path, delta, min, max, actionName); }
                catch (Exception ex) { SimHub.Logging.Current.Info(actionName + ": " + ex.Message); }
            });
        }

        private static void IncrementInt(OdriveWheelPlugin plugin, string path, int delta,
                                         int min, int max, string actionName)
        {
            lock (_incLock)
            {
                // Cache-first. Cache empty means initial read hasn't finished
                // (device just plugged in) — fall back to one serial read.
                var raw = plugin.ProfileService.GetRaw(path)
                       ?? plugin.SerialCli.ReadOffb(path);
                if (raw == null)
                {
                    SimHub.Logging.Current.Info(actionName + ": no value known (device not connected?)");
                    return;
                }
                if (!int.TryParse(raw.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out int cur))
                {
                    // Some ints come back with decimals (e.g. "255.000000").
                    if (!double.TryParse(raw.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out double dcur))
                    {
                        SimHub.Logging.Current.Info(actionName + ": parse failed '" + raw + "'");
                        return;
                    }
                    cur = (int)dcur;
                }
                int next = cur + delta;
                if (next < min) next = min;
                if (next > max) next = max;
                if (next == cur)
                {
                    SimHub.Logging.Current.Info(actionName + ": already at limit (" + cur + ")");
                    return;
                }
                var nextStr = next.ToString(CultureInfo.InvariantCulture);
                var ok = plugin.SerialCli.WriteOffb(path, nextStr);
                if (ok)
                {
                    plugin.ProfileService.PokeCache(path, nextStr);
                    SimHub.Logging.Current.Info(actionName + ": " + cur + " → " + next);
                }
                else
                {
                    SimHub.Logging.Current.Info(actionName + ": write rejected");
                }
            }
        }

        // ---- Float increment helper ----------------------------------------

        private static void RegisterIncFloat(PluginManager pm, OdriveWheelPlugin plugin,
                                             string actionName, string path, double delta,
                                             double min, double max, string fmt)
        {
            pm.AddAction<OdriveWheelPlugin>(actionName, (a, b) =>
            {
                try { IncrementFloat(plugin, path, delta, min, max, fmt, actionName); }
                catch (Exception ex) { SimHub.Logging.Current.Info(actionName + ": " + ex.Message); }
            });
        }

        private static void IncrementFloat(OdriveWheelPlugin plugin, string path, double delta,
                                           double min, double max, string fmt, string actionName)
        {
            lock (_incLock)
            {
                // Cache-first (see IncrementInt for the rationale).
                var raw = plugin.ProfileService.GetRaw(path)
                       ?? plugin.SerialCli.ReadOffb(path);
                if (raw == null)
                {
                    SimHub.Logging.Current.Info(actionName + ": no value known (device not connected?)");
                    return;
                }
                if (!double.TryParse(raw.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out double cur))
                {
                    SimHub.Logging.Current.Info(actionName + ": parse failed '" + raw + "'");
                    return;
                }
                double next = cur + delta;
                if (next < min) next = min;
                if (next > max) next = max;
                // Guard against float dust — treat sub-quantum change as no-op.
                if (Math.Abs(next - cur) < Math.Abs(delta) * 0.01)
                {
                    SimHub.Logging.Current.Info(actionName + ": already at limit (" + cur.ToString(fmt, CultureInfo.InvariantCulture) + ")");
                    return;
                }
                var nextStr = next.ToString(fmt, CultureInfo.InvariantCulture);
                var ok = plugin.SerialCli.WriteOffb(path, nextStr);
                if (ok)
                {
                    plugin.ProfileService.PokeCache(path, nextStr);
                    SimHub.Logging.Current.Info(actionName + ": " + cur.ToString(fmt, CultureInfo.InvariantCulture) + " → " + nextStr);
                }
                else
                {
                    SimHub.Logging.Current.Info(actionName + ": write rejected");
                }
            }
        }
    }
}
