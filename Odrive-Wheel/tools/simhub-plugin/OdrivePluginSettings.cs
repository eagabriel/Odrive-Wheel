using System;

namespace OdriveWheel.SimHubPlugin
{
    /// <summary>
    /// Persisted plugin settings. SimHub serializes this via
    /// PluginManager.LoadCommonSettings / SaveCommonSettings.
    ///
    /// The old "type in your motor/brake resistances" fields are gone —
    /// the plugin reads them via serial from the device. The user only
    /// picks WHERE their profile files live, and toggles behavior.
    /// </summary>
    public sealed class OdrivePluginSettings
    {
        // Folder path where the HTML tool's profile .json files live.
        // Blank = user hasn't picked one yet.
        public string ProfilesFolder { get; set; } = "";

        // Auto-apply <GameName>.json when SimHub reports a game change.
        public bool AutoLoadByGame { get; set; } = false;

        // If true, running sys.save! + ss right after AutoLoad — persists
        // to flash so the setting survives reboot. WARNING: ss reboots
        // the ODrive, which briefly drops FFB mid-race. Default OFF.
        public bool SaveToFlashOnAutoLoad { get; set; } = false;

        // Iq clip fraction — unchanged from before. Used together with
        // MotorCurrentLimA (now read from device) for ClipOutPercent.
        public double ClipFraction       { get; set; } = 0.95;
        public double ClipWindowSeconds  { get; set; } = 30.0;
        public double PowerWindowSeconds { get; set; } = 60.0;
    }
}
