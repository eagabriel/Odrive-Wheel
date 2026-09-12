using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Threading;

namespace OdriveWheel.SimHubPlugin
{
    /// <summary>
    /// Owns the OdriveSerialCli instance and orchestrates:
    ///   - initial bulk read of every profile field on startup
    ///   - periodic refresh (every 10 s) so external changes propagate
    ///   - applying a profile file (writes all fields + optional save-to-flash)
    ///   - auto-per-game selection (looks for &lt;GameName&gt;.json in the folder)
    ///
    /// Values are stored in a thread-safe dictionary keyed by ProfileField.Path.
    /// The plugin snapshots this dictionary per DataUpdate and publishes to SimHub.
    /// </summary>
    public sealed class OdriveProfileService
    {
        private readonly OdriveSerialCli    _cli;
        private readonly Action<string>     _log;
        private readonly Func<OdrivePluginSettings> _getSettings;

        private readonly ConcurrentDictionary<string, string> _rawValues
            = new ConcurrentDictionary<string, string>();

        private Thread  _refreshThread;
        private volatile bool _running;
        private DateTime _lastRefreshUtc = DateTime.MinValue;

        private string _lastAutoLoadedGame = "";

        public OdriveProfileService(
            OdriveSerialCli cli,
            Func<OdrivePluginSettings> getSettings,
            Action<string> log)
        {
            _cli         = cli;
            _getSettings = getSettings;
            _log         = log ?? (_ => { });
        }

        // ---- Lifecycle -------------------------------------------------------

        public void Start()
        {
            if (_running) return;
            _running = true;
            _refreshThread = new Thread(RefreshLoop)
            {
                IsBackground = true,
                Name         = "OdriveProfileRefresh"
            };
            _refreshThread.Start();
        }

        public void Stop()
        {
            _running = false;
            try { _refreshThread?.Join(500); } catch { }
            _refreshThread = null;
        }

        // ---- Public queries -------------------------------------------------

        /// <summary>Latest device value for the given field path, or null if never read.</summary>
        public string GetRaw(string path)
        {
            string v;
            return _rawValues.TryGetValue(path, out v) ? v : null;
        }

        /// <summary>Overwrites one cached value. Used by inc/dec actions after
        /// a successful write so dashboards and the settings panel reflect the
        /// new value immediately, without waiting for the next 10 s refresh.</summary>
        public void PokeCache(string path, string value)
        {
            if (path == null || value == null) return;
            _rawValues[path] = value;
        }

        /// <summary>Typed accessor — parses per ProfileField.Type.
        /// Returns null (for Integer/Double) or false (for Boolean) if unset/invalid.</summary>
        public object GetTyped(ProfileField f)
        {
            var raw = GetRaw(f.Path);
            if (raw == null) return null;
            return ParseTyped(raw, f.Type);
        }

        public bool IsConnected => _cli.IsDevicePresent();

        public DateTime LastRefreshUtc => _lastRefreshUtc;

        public string CurrentPortName => _cli.CurrentPortName;

        // ---- Bulk read ------------------------------------------------------

        /// <summary>Reads every field from the device once. Runs synchronously
        /// on the caller's thread. Skips fields that fail (logs, moves on).
        /// Uses a single serial session so the port is held ONCE for a bounded
        /// window rather than opened/closed per field.</summary>
        public int ReadAllOnce()
        {
            if (!_cli.BeginSession())
            {
                _log("ReadAll: could not open serial (device missing or busy)");
                return 0;
            }
            int ok = 0;
            try
            {
                foreach (var f in OdriveProfileFields.AllReadable)
                {
                    if (!_running) break;
                    string val = null;
                    try
                    {
                        val = (f.Protocol == FieldProtocol.Openffb)
                            ? _cli.ReadOffb(f.Path)
                            : _cli.ReadOdrive(f.Path);
                    }
                    catch (Exception ex) { _log("ReadAll " + f.Path + ": " + ex.Message); }

                    if (val != null)
                    {
                        _rawValues[f.Path] = val;
                        ok++;
                    }
                }
            }
            finally
            {
                _cli.EndSession();
            }
            _lastRefreshUtc = DateTime.UtcNow;
            _log("ReadAll done — " + ok + "/" + CountReadable() + " fields");
            return ok;
        }

        private static int CountReadable()
        {
            int n = 0;
            foreach (var _ in OdriveProfileFields.AllReadable) n++;
            return n;
        }

        // ---- Apply profile --------------------------------------------------

        public sealed class ApplyResult
        {
            public int Ok   { get; internal set; }
            public int Fail { get; internal set; }
            public IList<string> Errors { get; } = new List<string>();
        }

        /// <summary>Writes every profile field to the device. Returns per-field
        /// success/failure counts. Does NOT save to flash — call SaveToFlash()
        /// separately if you want persistence.</summary>
        public ApplyResult ApplyProfile(OdriveProfileFile.LoadedProfile profile)
        {
            var r = new ApplyResult();
            if (profile == null) return r;

            _log("ApplyProfile '" + profile.Name + "' — start (" + profile.Values.Count + " values in file)");
            if (!_cli.BeginSession())
            {
                r.Errors.Add("serial: could not open port (device missing or busy)");
                _log("ApplyProfile: session open failed");
                return r;
            }
            int considered = 0, skipped = 0;
            try
            {
                // Iterate in ProfileFields order (hardware-first) so dependent
                // writes see settled state.
                foreach (var f in OdriveProfileFields.All)
                {
                    if (!_running) break;
                    if (!profile.Values.TryGetValue(f.Path, out var val))
                    {
                        skipped++;
                        continue;
                    }
                    considered++;

                    // Boolean paths in profiles may be "True"/"False" or "1"/"0".
                    if (f.Type == FieldType.Boolean)
                    {
                        val = ParseBoolAsString(val);
                    }

                    bool ok = false;
                    string errNote = null;
                    try
                    {
                        ok = (f.Protocol == FieldProtocol.Openffb)
                            ? _cli.WriteOffb(f.Path, val)
                            : _cli.WriteOdrive(f.Path, val);
                    }
                    catch (Exception ex)
                    {
                        errNote = ex.Message;
                    }

                    if (ok)
                    {
                        r.Ok++;
                        _rawValues[f.Path] = val;
                        _log("  write ✓ " + f.Path + " = " + val);
                    }
                    else
                    {
                        r.Fail++;
                        var line = "  write ✗ " + f.Path + " = " + val
                                 + " (" + (errNote ?? "no ack / rejected") + ")";
                        _log(line);
                        r.Errors.Add(f.Path + ": " + (errNote ?? "no ack / rejected"));
                    }
                }
            }
            finally
            {
                _cli.EndSession();
            }
            _log("ApplyProfile '" + profile.Name + "' — done: "
                 + r.Ok + " ok, " + r.Fail + " fail, "
                 + considered + " considered, " + skipped + " skipped (not in plugin scope)");
            return r;
        }

        /// <summary>sys.save! (persists OpenFFBoard params) + ss (persists ODrive config, reboots).</summary>
        public void SaveToFlash()
        {
            _log("SaveToFlash: sys.save! + ss (device will reboot)");
            _cli.ExecOffb("sys.save");
            _cli.ODriveSave();
        }

        /// <summary>
        /// Snapshots the current cache into a values dictionary suitable for
        /// passing to <see cref="OdriveProfileFile.Save"/>. Includes every
        /// path in <c>OdriveProfileFields.All</c> that has a cached value.
        /// Booleans are normalised to "True"/"False" so the file matches
        /// what the HTML tool writes (device returns "0"/"1" over the wire).
        /// Fields with no cache entry are skipped rather than saved as empty
        /// strings.
        /// </summary>
        public IDictionary<string, string> CaptureCurrentAsProfile()
        {
            var dict = new Dictionary<string, string>();
            foreach (var f in OdriveProfileFields.All)
            {
                var raw = GetRaw(f.Path);
                if (string.IsNullOrEmpty(raw)) continue;
                if (f.Type == FieldType.Boolean)
                {
                    // Cache holds device echoes ("0"/"1"); HTML profiles use
                    // "True"/"False". Normalise here so a save-then-load
                    // round-trip in the HTML tool sees the expected form.
                    dict[f.Path] = raw == "1" ||
                                   raw.Equals("True", StringComparison.OrdinalIgnoreCase)
                        ? "True" : "False";
                }
                else
                {
                    dict[f.Path] = raw;
                }
            }
            return dict;
        }

        // ---- Auto-per-game --------------------------------------------------

        /// <summary>Called from the plugin's DataUpdate. If GameName changed and
        /// a matching &lt;GameName&gt;.json exists in the profiles folder AND
        /// AutoLoadByGame is enabled, applies it. Idempotent per game — won't
        /// re-apply the same game's profile back-to-back.</summary>
        public void MaybeAutoLoadForGame(string gameName)
        {
            var s = _getSettings();
            if (!s.AutoLoadByGame) return;
            if (string.IsNullOrEmpty(gameName)) return;
            if (gameName == _lastAutoLoadedGame) return;
            if (string.IsNullOrEmpty(s.ProfilesFolder) || !Directory.Exists(s.ProfilesFolder))
                return;

            // Sanitize just in case — game names sometimes have spaces/colons.
            var safe = SanitizeFileName(gameName);
            var candidate = Path.Combine(s.ProfilesFolder, safe + ".json");
            if (!File.Exists(candidate)) return;

            OdriveProfileFile.LoadedProfile p = null;
            try { p = OdriveProfileFile.Load(candidate); }
            catch (Exception ex) { _log("auto-load parse fail: " + ex.Message); return; }

            _log("auto-load for game '" + gameName + "' → " + candidate);
            var r = ApplyProfile(p);
            if (s.SaveToFlashOnAutoLoad && r.Fail == 0)
                SaveToFlash();

            _lastAutoLoadedGame = gameName;
        }

        // ---- Refresh loop ---------------------------------------------------
        //
        // The plugin owns every write to the device (inc/dec actions, profile
        // apply, save-to-flash). Its cache is authoritative — periodic
        // polling would just re-read values it already knows, contending
        // uselessly with the HTML tool. Instead:
        //
        //   - Read once on first connect
        //   - Re-read on device reconnect (unplug → replug)
        //   - Otherwise trust the cache; the user can force a re-read from
        //     the settings panel if they've been tuning in the HTML tool
        //
        // The loop still runs at 2 Hz so device presence changes are detected
        // quickly, but IsDevicePresent hits a 30 s WMI cache and is cheap.
        private void RefreshLoop()
        {
            // Initial read as soon as the device is up.
            Thread.Sleep(500);
            bool wasConnected = _cli.IsDevicePresent();
            if (wasConnected) ReadAllOnce();

            while (_running)
            {
                // Sleep in short chunks so Stop() responds quickly.
                for (int i = 0; i < 20 && _running; i++) Thread.Sleep(100);
                if (!_running) return;

                bool nowConnected = _cli.IsDevicePresent();
                if (nowConnected && !wasConnected)
                {
                    _log("device reappeared — re-reading all fields");
                    ReadAllOnce();
                }
                wasConnected = nowConnected;
            }
        }

        // ---- Helpers --------------------------------------------------------

        internal static object ParseTyped(string raw, FieldType t)
        {
            if (raw == null) return null;
            raw = raw.Trim();
            switch (t)
            {
                case FieldType.Integer:
                    long li;
                    if (long.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out li))
                        return li;
                    // Some fields (axis.range) come back as "900.000000" — accept.
                    double di;
                    if (double.TryParse(raw, NumberStyles.Float, CultureInfo.InvariantCulture, out di))
                        return (long)di;
                    return null;

                case FieldType.Double:
                    double d;
                    if (double.TryParse(raw, NumberStyles.Float, CultureInfo.InvariantCulture, out d))
                        return d;
                    return null;

                case FieldType.Boolean:
                    if (raw == "1" || raw.Equals("True", StringComparison.OrdinalIgnoreCase))
                        return true;
                    if (raw == "0" || raw.Equals("False", StringComparison.OrdinalIgnoreCase))
                        return false;
                    return false;
            }
            return raw;
        }

        // Firmware accepts "0"/"1" reliably; profiles from HTML may store "True"/"False".
        // Normalize to "0"/"1" for the write.
        private static string ParseBoolAsString(string s)
        {
            if (string.IsNullOrEmpty(s)) return "0";
            var t = s.Trim();
            if (t == "1" || t.Equals("True", StringComparison.OrdinalIgnoreCase)) return "1";
            return "0";
        }

        private static string SanitizeFileName(string s)
        {
            var invalid = Path.GetInvalidFileNameChars();
            var chars = s.ToCharArray();
            for (int i = 0; i < chars.Length; i++)
            {
                foreach (var c in invalid) if (chars[i] == c) { chars[i] = '_'; break; }
            }
            return new string(chars);
        }
    }
}
