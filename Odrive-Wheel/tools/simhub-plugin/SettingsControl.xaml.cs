using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace OdriveWheel.SimHubPlugin
{
    public partial class SettingsControl : System.Windows.Controls.UserControl
    {
        private readonly OdriveWheelPlugin _plugin;
        private readonly DispatcherTimer _refresh;

        // Per-field editor control that shows the current serial-read value.
        // Populated by BuildDeviceConfigPanel() at construction, refreshed
        // every 300 ms by RefreshLive() (which skips controls that are
        // focused or have unsaved edits).
        //
        // Value type depends on the field:
        //   - profile field, numeric  → TextBox (commit on Enter)
        //   - profile field, boolean  → CheckBox (commit on click)
        //   - extras (phase_R, brake_R) → TextBlock (read-only)
        private readonly Dictionary<string, FrameworkElement> _fieldEditors
            = new Dictionary<string, FrameworkElement>();
        private readonly Dictionary<string, ProfileField> _fieldSpecs
            = new Dictionary<string, ProfileField>();

        // Set of paths that live inside a profile (== writable). Extras like
        // phase_resistance stay read-only.
        private static readonly HashSet<string> _profileWritablePaths
            = new HashSet<string>(OdriveProfileFields.All.Select(f => f.Path), StringComparer.Ordinal);

        private static readonly SolidColorBrush BrushOk        = MakeBrush(0x2A, 0x9D, 0x8F);
        private static readonly SolidColorBrush BrushOkSoft    = MakeBrush(0x3F, 0xB8, 0xA9);
        private static readonly SolidColorBrush BrushWarn      = MakeBrush(0xE9, 0xC4, 0x6A);
        private static readonly SolidColorBrush BrushOrange    = MakeBrush(0xF4, 0xA2, 0x61);
        private static readonly SolidColorBrush BrushDanger    = MakeBrush(0xE7, 0x6F, 0x51);
        private static readonly SolidColorBrush BrushOff       = MakeBrush(0x55, 0x55, 0x55);

        private static readonly Color ColorOk     = Color.FromRgb(0x2A, 0x9D, 0x8F);
        private static readonly Color ColorOkSoft = Color.FromRgb(0x3F, 0xB8, 0xA9);
        private static readonly Color ColorWarn   = Color.FromRgb(0xE9, 0xC4, 0x6A);
        private static readonly Color ColorDanger = Color.FromRgb(0xE7, 0x6F, 0x51);
        private static readonly Color ColorOff    = Color.FromRgb(0x55, 0x55, 0x55);

        private static SolidColorBrush MakeBrush(byte r, byte g, byte b)
        {
            var br = new SolidColorBrush(Color.FromRgb(r, g, b));
            br.Freeze();
            return br;
        }

        // Pick a colour based on temperature (0..100°C typical operating range).
        private static SolidColorBrush TempColour(double c)
        {
            if (!IsFinite(c))   return BrushOff;
            if (c < 60)  return BrushOk;
            if (c < 75)  return BrushWarn;
            if (c < 85)  return BrushOrange;
            return BrushDanger;
        }

        private static bool IsFinite(double v) => !double.IsNaN(v) && !double.IsInfinity(v);

        public SettingsControl() { InitializeComponent(); }

        public SettingsControl(OdriveWheelPlugin plugin) : this()
        {
            _plugin = plugin;
            LoadFromSettings();
            RefreshProfilesList();
            BuildDeviceConfigPanel();
            InitHeaderFooter();

            _refresh = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(300) };
            _refresh.Tick += (s, e) => RefreshLive();
            Loaded   += (s, e) => _refresh.Start();
            Unloaded += (s, e) => _refresh.Stop();
        }

        // ---- Header / footer initialization --------------------------------

        private void InitHeaderFooter()
        {
            // Version from the plugin assembly — no manual sync needed when
            // csproj <Version> bumps.
            string ver;
            try
            {
                ver = "v" + typeof(OdriveWheelPlugin).Assembly.GetName().Version.ToString(3);
            }
            catch { ver = "v?"; }

            if (HdrVersion != null) HdrVersion.Text = "plugin " + ver;
            if (FtVersion  != null) FtVersion.Text  = ver;
            if (FtBuild    != null) FtBuild.Text    = "github.com/eagabriel/Odrive-Wheel";
        }

        private void OnOpenGitHub(object sender, RoutedEventArgs e)
            => OpenUrl("https://github.com/eagabriel/Odrive-Wheel");

        private void OnOpenPix(object sender, RoutedEventArgs e)
            => OpenUrl("https://github.com/eagabriel/Odrive-Wheel#pix-brazil");

        private void OnOpenBmc(object sender, RoutedEventArgs e)
            => OpenUrl("https://www.buymeacoffee.com/eduardogabq");

        private void OnOpenSponsor(object sender, RoutedEventArgs e)
            => OpenUrl("https://github.com/sponsors/eagabriel");

        private static void OpenUrl(string url)
        {
            try
            {
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url)
                {
                    UseShellExecute = true,
                });
            }
            catch { /* browser missing / firewall — swallow */ }
        }

        // ---- Device config panel — dynamic build ---------------------------

        // Same fields the profile knows about (31 + 2 extras), grouped so
        // the panel actually reads well. Order within each group matches
        // OdriveProfileFields so serial reads and UI display are consistent.
        private static readonly (string Title, string[] Paths)[] Groups = new[]
        {
            ("Motor & Encoder (ODrive)", new[] {
                "axis0.motor.config.current_lim",
                "axis0.motor.config.current_control_bandwidth",
                "axis0.encoder.config.bandwidth",
                "axis0.motor.config.phase_resistance",
                "config.brake_resistance",
            }),
            ("FFB Axis", new[] {
                "axis.range",
                "axis.maxtorque",
                "axis.fxratio",
                "axis.invert",
                "axis.ffbinvert",
                "axis.idlespring",
                "axis.axisdamper",
                "axis.axisinertia",
                "axis.axisfriction",
                "axis.esgain",
                "axis.esdamp",
                "axis.maxtorquerate",
                "axis.expo",
                "axis.exposcale",
            }),
            ("FX Master gains", new[] {
                "fx.master",
                "fx.spring",
                "fx.damper",
                "fx.friction",
                "fx.inertia",
            }),
            ("FX Filters (LPF)", new[] {
                "fx.filterCfFreq", "fx.filterCfQ",
                "fx.filterFrFreq", "fx.filterFrQ",
                "fx.filterDaFreq", "fx.filterDaQ",
                "fx.filterInFreq", "fx.filterInQ",
            }),
            ("3-band EQ", new[] {
                "axis.eqweight",
                "axis.eqchassis",
                "axis.eqroad",
            }),
        };

        private void BuildDeviceConfigPanel()
        {
            DeviceConfigStack.Children.Clear();
            _fieldEditors.Clear();
            _fieldSpecs.Clear();

            foreach (var g in Groups)
            {
                var header = new TextBlock
                {
                    Text = g.Title,
                    FontWeight = FontWeights.SemiBold,
                    Foreground = new SolidColorBrush(Color.FromRgb(0xB0, 0xB0, 0xB0)),
                    Margin = new Thickness(0, 12, 0, 6),
                };
                DeviceConfigStack.Children.Add(header);

                // 3-column grid: path label / editor / property-name hint.
                var grid = new Grid();
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(210) });
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(120) });
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

                int row = 0;
                foreach (var path in g.Paths)
                {
                    var f = FindField(path);
                    if (f == null) continue;
                    grid.RowDefinitions.Add(new RowDefinition { MinHeight = 26 });

                    var lbl = new TextBlock
                    {
                        Text = f.Path, FontFamily = new FontFamily("Consolas"),
                        FontSize = 12, Margin = new Thickness(0, 4, 6, 4),
                        VerticalAlignment = VerticalAlignment.Center,
                    };
                    Grid.SetRow(lbl, row); Grid.SetColumn(lbl, 0);
                    grid.Children.Add(lbl);

                    bool writable = _profileWritablePaths.Contains(path);
                    var editor = CreateFieldEditor(f, writable);
                    Grid.SetRow(editor, row); Grid.SetColumn(editor, 1);
                    grid.Children.Add(editor);
                    _fieldEditors[path] = editor;
                    _fieldSpecs[path]   = f;

                    var propHint = new TextBlock
                    {
                        Text     = writable ? "→ " + f.PropName : "→ " + f.PropName + " (read-only)",
                        FontSize = 11,
                        Foreground = new SolidColorBrush(Color.FromRgb(0x88, 0x88, 0x88)),
                        Margin   = new Thickness(0, 4, 0, 4),
                        VerticalAlignment = VerticalAlignment.Center,
                    };
                    Grid.SetRow(propHint, row); Grid.SetColumn(propHint, 2);
                    grid.Children.Add(propHint);

                    row++;
                }
                DeviceConfigStack.Children.Add(grid);
            }

            var footerHint = new TextBlock
            {
                Text = "Edit any writable field, press Enter to write via serial (Esc to reset). "
                     + "Writes are RAM-only — use Save to flash to persist.",
                Foreground = new SolidColorBrush(Color.FromRgb(0x9A, 0xA0, 0xA6)),
                FontSize = 11,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 12, 0, 0),
            };
            DeviceConfigStack.Children.Add(footerHint);
        }

        // Build the appropriate editor for a field. Writable numeric →
        // TextBox with Enter/Esc handling; writable boolean → CheckBox;
        // read-only extras → TextBlock.
        private FrameworkElement CreateFieldEditor(ProfileField f, bool writable)
        {
            if (!writable)
            {
                return new TextBlock
                {
                    Text = "—",
                    FontFamily = new FontFamily("Consolas"), FontSize = 12,
                    Foreground = new SolidColorBrush(Color.FromRgb(0xB8, 0xB8, 0xB8)),
                    Margin = new Thickness(0, 4, 6, 4),
                    VerticalAlignment = VerticalAlignment.Center,
                };
            }

            if (f.Type == FieldType.Boolean)
            {
                var cb = new CheckBox
                {
                    Margin = new Thickness(0, 4, 6, 4),
                    VerticalAlignment = VerticalAlignment.Center,
                    Foreground = new SolidColorBrush(Color.FromRgb(0xEE, 0xEE, 0xEE)),
                };
                // Click fires only on user interaction — programmatic
                // IsChecked updates from RefreshLive don't trigger this.
                cb.Click += (s, e) => CommitField(f, cb.IsChecked == true ? "1" : "0");
                return cb;
            }

            var tb = new TextBox
            {
                Margin = new Thickness(0, 3, 6, 3),
                Padding = new Thickness(4, 1, 4, 1),
                FontFamily = new FontFamily("Consolas"),
                FontSize = 12,
                Background  = MakeBrush(0x1E, 0x20, 0x23),
                Foreground  = new SolidColorBrush(Color.FromRgb(0xEE, 0xEE, 0xEE)),
                BorderBrush = MakeBrush(0x3A, 0x3D, 0x42),
                BorderThickness = new Thickness(1),
                VerticalAlignment = VerticalAlignment.Center,
                // Suppress the WPF-default dashed focus rectangle. Without
                // this, focus paints a blue outline over our BorderBrush,
                // hiding the yellow "dirty" state.
                FocusVisualStyle = null,
                Tag = null,   // null = clean, "dirty" = unsaved edits
            };
            // PreviewKeyDown fires before any parent (ScrollViewer, panel,
            // SimHub-provided input handlers) can eat the event. Plain
            // KeyDown gets swallowed in some hosting contexts, which was
            // why Enter appeared to do nothing.
            tb.PreviewKeyDown += (s, e) =>
            {
                if (e.Key == Key.Enter || e.Key == Key.Return)
                {
                    CommitField(f, tb.Text);
                    e.Handled = true;
                }
                else if (e.Key == Key.Escape)
                {
                    var raw = _plugin.ProfileService?.GetRaw(f.Path);
                    tb.Text = raw ?? "";
                    tb.Tag  = null;
                    SetEditorDirtyVisual(tb, false);
                    e.Handled = true;
                }
            };
            tb.TextChanged += (s, e) =>
            {
                var raw = _plugin.ProfileService?.GetRaw(f.Path);
                bool dirty = tb.Text != (raw ?? "");
                tb.Tag = dirty ? "dirty" : null;
                SetEditorDirtyVisual(tb, dirty);
            };
            // Overlay a matching background under the TextBox's transparent
            // areas so the yellow "dirty" border reads cleanly even when
            // the focus adorner is trying to paint over it.
            tb.GotFocus  += (s, e) => tb.SelectAll();
            return tb;
        }

        private void SetEditorDirtyVisual(TextBox tb, bool dirty)
        {
            tb.BorderBrush = dirty
                ? new SolidColorBrush(Color.FromRgb(0xE9, 0xC4, 0x6A))   // warning yellow
                : MakeBrush(0x3A, 0x3D, 0x42);
            tb.BorderThickness = new Thickness(dirty ? 1.8 : 1.0);
        }

        // Write one field via serial then update local cache. Runs on
        // Task pool so the UI doesn't freeze during the ~30-500 ms serial
        // round-trip.
        private void CommitField(ProfileField f, string val)
        {
            var cli = _plugin?.SerialCli;
            var svc = _plugin?.ProfileService;
            if (cli == null || svc == null) return;

            ApplyStatus.Text = "writing " + f.Path + " = " + val + "…";
            Task.Run(() =>
            {
                bool ok;
                try
                {
                    ok = f.Protocol == FieldProtocol.Openffb
                        ? cli.WriteOffb(f.Path, val)
                        : cli.WriteOdrive(f.Path, val);
                }
                catch (Exception ex)
                {
                    Dispatcher.Invoke(() => ApplyStatus.Text = "write error: " + ex.Message);
                    return;
                }
                if (ok) svc.PokeCache(f.Path, val);
                Dispatcher.Invoke(() =>
                {
                    if (ok)
                    {
                        ApplyStatus.Text = "✓ " + f.Path + " = " + val
                                         + "  (RAM only — use Save to flash to persist)";
                        if (_fieldEditors.TryGetValue(f.Path, out var e) && e is TextBox tb2)
                        {
                            tb2.Text = val;
                            tb2.Tag  = null;
                            SetEditorDirtyVisual(tb2, false);
                        }
                    }
                    else
                    {
                        ApplyStatus.Text = "✗ " + f.Path
                                         + " → rejected by firmware (invalid value?)";
                    }
                });
            });
        }

        private static ProfileField FindField(string path)
        {
            foreach (var f in OdriveProfileFields.All)
                if (f.Path == path) return f;
            foreach (var f in OdriveProfileFields.ExtrasNotInProfile)
                if (f.Path == path) return f;
            return null;
        }

        // ---- Wiring ---------------------------------------------------------

        private void LoadFromSettings()
        {
            if (_plugin?.Settings == null) return;
            TxtProfilesFolder.Text = _plugin.Settings.ProfilesFolder ?? "";
            ChkAutoLoad.IsChecked  = _plugin.Settings.AutoLoadByGame;
            ChkSaveOnAuto.IsChecked= _plugin.Settings.SaveToFlashOnAutoLoad;
        }

        private void OnBrowseFolder(object sender, RoutedEventArgs e)
        {
            // WPF has no built-in FolderBrowser; use the WinForms one that
            // ships in .NET 4.8. It's the pragmatic path and needs no extra dep.
            var dlg = new System.Windows.Forms.FolderBrowserDialog
            {
                Description = "Pick your Odrive-Wheel profiles folder",
                SelectedPath = TxtProfilesFolder.Text ?? ""
            };
            if (dlg.ShowDialog() == System.Windows.Forms.DialogResult.OK)
            {
                TxtProfilesFolder.Text = dlg.SelectedPath;
                _plugin.Settings.ProfilesFolder = dlg.SelectedPath;
                _plugin.SaveSettings();
                RefreshProfilesList();
            }
        }

        private void OnReloadFolder(object sender, RoutedEventArgs e)
        {
            _plugin.Settings.ProfilesFolder = TxtProfilesFolder.Text?.Trim() ?? "";
            _plugin.SaveSettings();
            RefreshProfilesList();
        }

        private void OnToggleAutoLoad(object sender, RoutedEventArgs e)
        {
            _plugin.Settings.AutoLoadByGame = ChkAutoLoad.IsChecked == true;
            _plugin.SaveSettings();
        }

        private void OnToggleSaveOnAuto(object sender, RoutedEventArgs e)
        {
            _plugin.Settings.SaveToFlashOnAutoLoad = ChkSaveOnAuto.IsChecked == true;
            _plugin.SaveSettings();
        }

        private void OnRefreshNow(object sender, RoutedEventArgs e)
        {
            ApplyStatus.Text = "reading from device…";
            // Run in background — WMI + serial takes a couple hundred ms.
            var svc = _plugin.ProfileService;
            System.Threading.Tasks.Task.Run(() =>
            {
                int ok = svc.ReadAllOnce();
                Dispatcher.Invoke(() => ApplyStatus.Text = $"refreshed — {ok} fields read");
            });
        }

        private void OnApplyProfile(object sender, RoutedEventArgs e)
        {
            var item = CmbProfiles.SelectedItem as ProfileListItem;
            if (item == null)
            {
                ApplyStatus.Text = "pick a profile first";
                return;
            }
            ApplyStatus.Text = "applying '" + item.Name + "'…";
            var svc = _plugin.ProfileService;
            var path = item.Path;
            System.Threading.Tasks.Task.Run(() =>
            {
                OdriveProfileFile.LoadedProfile p;
                try { p = OdriveProfileFile.Load(path); }
                catch (Exception ex)
                {
                    Dispatcher.Invoke(() => ApplyStatus.Text = "load error: " + ex.Message);
                    return;
                }
                var r = svc.ApplyProfile(p);
                Dispatcher.Invoke(() =>
                    ApplyStatus.Text = "'" + p.Name + "' — " + r.Ok + " ok"
                                     + (r.Fail > 0 ? ", " + r.Fail + " failed" : "")
                                     + " (not persisted; use Save to flash)"
                );
            });
        }

        // ---- Save current state as profile file -----------------------------

        private void OnSaveAsNew(object sender, RoutedEventArgs e)
        {
            var folder = _plugin?.Settings?.ProfilesFolder;
            if (string.IsNullOrEmpty(folder) || !Directory.Exists(folder))
            {
                ApplyStatus.Text = "pick a profiles folder first";
                return;
            }
            var name = (TxtNewProfileName?.Text ?? "").Trim();
            if (string.IsNullOrEmpty(name))
            {
                ApplyStatus.Text = "type a profile name";
                return;
            }

            var safe = OdriveProfileFile.ToSafeFileName(name);
            var path = System.IO.Path.Combine(folder, safe + ".json");

            if (File.Exists(path))
            {
                var res = MessageBox.Show(
                    "A file with this name already exists:\n\n" + path +
                    "\n\nOverwrite it?",
                    "Overwrite existing?",
                    MessageBoxButton.YesNo, MessageBoxImage.Warning);
                if (res != MessageBoxResult.Yes) return;
            }

            SaveProfileTo(path, name);
        }

        private void OnOverwriteSelected(object sender, RoutedEventArgs e)
        {
            var item = CmbProfiles.SelectedItem as ProfileListItem;
            if (item == null || string.IsNullOrEmpty(item.Path))
            {
                ApplyStatus.Text = "no profile selected to overwrite";
                return;
            }
            var res = MessageBox.Show(
                "Overwrite '" + item.Name + "' with the current cached values?\n\n"
                + item.Path,
                "Overwrite selected profile?",
                MessageBoxButton.OKCancel, MessageBoxImage.Warning);
            if (res != MessageBoxResult.OK) return;

            SaveProfileTo(item.Path, item.Name);
        }

        // Common save helper — snapshots the service cache, writes the file,
        // refreshes the dropdown.
        private void SaveProfileTo(string path, string profileName)
        {
            var svc = _plugin?.ProfileService;
            if (svc == null) { ApplyStatus.Text = "plugin not ready"; return; }

            IDictionary<string, string> values = svc.CaptureCurrentAsProfile();
            if (values.Count == 0)
            {
                ApplyStatus.Text = "no cached values — device not connected yet?";
                return;
            }
            try
            {
                OdriveProfileFile.Save(path, profileName, values);
            }
            catch (Exception ex)
            {
                ApplyStatus.Text = "save failed: " + ex.Message;
                return;
            }
            ApplyStatus.Text = "✓ saved '" + profileName + "' — " + values.Count + " fields → " + System.IO.Path.GetFileName(path);

            // Refresh dropdown and re-select the file we just wrote.
            RefreshProfilesList();
            for (int i = 0; i < CmbProfiles.Items.Count; i++)
            {
                if (CmbProfiles.Items[i] is ProfileListItem it &&
                    string.Equals(it.Path, path, StringComparison.OrdinalIgnoreCase))
                {
                    CmbProfiles.SelectedIndex = i;
                    break;
                }
            }
        }

        private void OnSaveToFlash(object sender, RoutedEventArgs e)
        {
            var res = MessageBox.Show(
                "This will run sys.save! (FFB EEPROM) and ss (ODrive NVM). The ODrive REBOOTS.\n\n" +
                "FFB drops for ~1-2 seconds. Are you sure?",
                "Save to flash",
                MessageBoxButton.OKCancel, MessageBoxImage.Warning);
            if (res != MessageBoxResult.OK) return;

            ApplyStatus.Text = "saving to flash…";
            var svc = _plugin.ProfileService;
            System.Threading.Tasks.Task.Run(() =>
            {
                try { svc.SaveToFlash(); }
                catch (Exception ex)
                {
                    Dispatcher.Invoke(() => ApplyStatus.Text = "save error: " + ex.Message);
                    return;
                }
                Dispatcher.Invoke(() => ApplyStatus.Text = "saved. board rebooting…");
            });
        }

        // ---- Profiles list --------------------------------------------------

        private sealed class ProfileListItem
        {
            public string Name { get; set; }
            public string Path { get; set; }
            public override string ToString() => Name + "  (" + System.IO.Path.GetFileName(Path) + ")";
        }

        private void RefreshProfilesList()
        {
            CmbProfiles.Items.Clear();
            var folder = _plugin?.Settings?.ProfilesFolder;
            if (string.IsNullOrEmpty(folder) || !Directory.Exists(folder))
            {
                CmbProfiles.Items.Add(new ProfileListItem { Name = "(pick a folder)", Path = "" });
                CmbProfiles.SelectedIndex = 0;
                return;
            }
            var items = OdriveProfileFile.ListInFolder(folder)
                                          .OrderBy(x => x.Name, StringComparer.OrdinalIgnoreCase)
                                          .ToList();
            if (!items.Any())
            {
                CmbProfiles.Items.Add(new ProfileListItem { Name = "(no profiles in this folder)", Path = "" });
                CmbProfiles.SelectedIndex = 0;
                return;
            }
            foreach (var it in items)
                CmbProfiles.Items.Add(new ProfileListItem { Name = it.Name, Path = it.Path });
            CmbProfiles.SelectedIndex = 0;
        }

        // ---- Live refresh (called ~3 Hz) -----------------------------------

        private void RefreshLive()
        {
            var snap = _plugin?.Snapshot;
            var svc  = _plugin?.ProfileService;
            if (snap == null) return;

            // HID status LED — glow when live.
            if (snap.Connected)
            {
                HidStatusLed.Fill = BrushOk;
                SetLedGlow(HidLedGlow, ColorOk, on: true);
                HidStatusText.Text = $"connected — {snap.ReportRateHz:F0} Hz, last {snap.LastReportMillisAgo} ms";
            }
            else
            {
                HidStatusLed.Fill = BrushOff;
                SetLedGlow(HidLedGlow, ColorOff, on: false);
                HidStatusText.Text = snap.LastReportMillisAgo > 0 ? "stale" : "waiting for device…";
            }

            // Serial status LED.
            if (svc != null)
            {
                var port = svc.CurrentPortName;
                if (!string.IsNullOrEmpty(port))
                {
                    SerialStatusLed.Fill = BrushOk;
                    SetLedGlow(SerialLedGlow, ColorOk, on: true);
                    var age = svc.LastRefreshUtc == DateTime.MinValue ? -1 : (int)(DateTime.UtcNow - svc.LastRefreshUtc).TotalSeconds;
                    SerialStatusText.Text = age < 0
                        ? $"{port} — no read yet"
                        : $"{port} — initial read {age}s ago (cache authoritative)";
                }
                else
                {
                    SerialStatusLed.Fill = BrushWarn;
                    SetLedGlow(SerialLedGlow, ColorWarn, on: true);
                    SerialStatusText.Text = "COM port not discovered";
                }
            }

            // Live HID readings
            LiveRate.Text     = snap.ReportRateHz.ToString("F0", CultureInfo.InvariantCulture);
            LivePosition.Text = snap.PositionDeg.ToString("F1", CultureInfo.InvariantCulture) + "°";
            LiveVelocity.Text = snap.VelocityTurnsPerSec.ToString("F3", CultureInfo.InvariantCulture);
            LiveIq.Text       = snap.IqCurrentA.ToString("F2", CultureInfo.InvariantCulture);
            LiveTorque.Text   = snap.TorqueOutputNm.ToString("F2", CultureInfo.InvariantCulture);
            LiveVBus.Text     = snap.VBusVoltage.ToString("F2", CultureInfo.InvariantCulture);
            LiveIBus.Text     = snap.IBusCurrentA.ToString("F2", CultureInfo.InvariantCulture);
            LiveIBrake.Text   = snap.BrakeResistorCurrentA.ToString("F2", CultureInfo.InvariantCulture);
            LiveClip.Text     = snap.ClipOutPercent.ToString("F1", CultureInfo.InvariantCulture) + " %";
            LiveMech.Text     = snap.MotorMechPowerW.ToString("F1", CultureInfo.InvariantCulture);
            LiveFetTemp.Text  = snap.FetTempValid   ? snap.FetTempC.ToString("F0", CultureInfo.InvariantCulture)   + " °C" : "— (no sensor)";
            LiveMotorTemp.Text= snap.MotorTempValid ? snap.MotorTempC.ToString("F0", CultureInfo.InvariantCulture) + " °C" : "— (no sensor)";

            LiveAxisState.Text       = OdriveErrorDecoder.DecodeAxisState(snap.AxisState);
            LiveAxisError.Text       = OdriveErrorDecoder.DecodeAxisError(snap.AxisError);
            LiveMotorError.Text      = OdriveErrorDecoder.DecodeMotorError(snap.MotorError);
            LiveEncoderError.Text    = OdriveErrorDecoder.DecodeEncoderError(snap.EncoderError);
            LiveControllerError.Text = OdriveErrorDecoder.DecodeControllerError(snap.ControllerError);

            // Warning banner — visible only when at least one error bit is set.
            if (snap.AnyError)
            {
                WarningBanner.Visibility = Visibility.Visible;
                var parts = new List<string>();
                if (snap.AxisError       != 0) parts.Add("Axis: "       + OdriveErrorDecoder.DecodeAxisError(snap.AxisError));
                if (snap.MotorError      != 0) parts.Add("Motor: "      + OdriveErrorDecoder.DecodeMotorError(snap.MotorError));
                if (snap.EncoderError    != 0) parts.Add("Encoder: "    + OdriveErrorDecoder.DecodeEncoderError(snap.EncoderError));
                if (snap.ControllerError != 0) parts.Add("Controller: " + OdriveErrorDecoder.DecodeControllerError(snap.ControllerError));
                WarningText.Text = string.Join("\n", parts);
            }
            else
            {
                WarningBanner.Visibility = Visibility.Collapsed;
            }

            // Temperature bars — full width on 0..100 °C scale, coloured by band.
            UpdateTempBar(LiveFetTempBar,  snap.FetTempC,  snap.FetTempValid);
            UpdateTempBar(LiveMotorTempBar, snap.MotorTempC, snap.MotorTempValid);

            // Iq vs current_lim bar (from service — MotorCurrentLimA property).
            UpdateCurrentBar(snap);

            // Sparklines.
            DrawSparkline(SparkVBus,   snap.SparkVBus.Snapshot(),   ColorOkSoft, autoScale: true);
            DrawSparkline(SparkTorque, snap.SparkTorque.Snapshot(), ColorOkSoft, autoScale: true);

            // Wheel gauge — rotate 1:1 with real position. Not clamped, so
            // 2+ turns are visible as continued spin.
            if (WheelRot != null && IsFinite(snap.PositionDeg))
                WheelRot.Angle = snap.PositionDeg;

            // Big hero readouts.
            if (LivePositionBig != null) LivePositionBig.Text = snap.PositionDeg.ToString("F1", CultureInfo.InvariantCulture) + "°";
            if (LiveVelocityBig != null) LiveVelocityBig.Text = snap.VelocityTurnsPerSec.ToString("F2", CultureInfo.InvariantCulture) + " turns/s";
            if (LiveTorqueBig   != null) LiveTorqueBig.Text   = snap.TorqueOutputNm.ToString("F2", CultureInfo.InvariantCulture) + " Nm";

            // Scope — Iq (blue) + Torque (orange) overlaid, shared Y axis
            // (both roughly ±Nm-scale magnitudes — Iq slightly larger).
            DrawScope(ScopeCanvas,
                      snap.ScopeIq.Snapshot(),     Color.FromRgb(0x4F, 0xC3, 0xF7),
                      snap.ScopeTorque.Snapshot(), Color.FromRgb(0xF4, 0xA2, 0x61));

            // Header status pill — coarse heuristic based on features seen.
            if (HdrFirmware != null)
            {
                if (!snap.Connected)
                    HdrFirmware.Text = "firmware —";
                else if (snap.AxisState != 0 || snap.AnyError ||
                         snap.FetTempValid || snap.MotorTempValid)
                    HdrFirmware.Text = "firmware ≥ 1.3 (errors + temps)";
                else
                    HdrFirmware.Text = "firmware connected";
            }

            // Live device config (from serial) — sync each editor back from
            // the cache. Skips controls that are focused or marked "dirty"
            // (user has unsaved edits) so we don't stomp on typing.
            if (svc != null)
            {
                foreach (var kv in _fieldEditors)
                {
                    var raw = svc.GetRaw(kv.Key);
                    var editor = kv.Value;
                    if (editor is TextBox tb)
                    {
                        if (tb.IsKeyboardFocusWithin) continue;
                        if ("dirty".Equals(tb.Tag))   continue;
                        var target = raw ?? "";
                        if (tb.Text != target) tb.Text = target;
                    }
                    else if (editor is CheckBox cb)
                    {
                        if (cb.IsKeyboardFocusWithin) continue;
                        bool want = raw == "1" ||
                                    (raw != null && raw.Equals("True", StringComparison.OrdinalIgnoreCase));
                        if (cb.IsChecked != want) cb.IsChecked = want;
                    }
                    else if (editor is TextBlock tbl)
                    {
                        tbl.Text = string.IsNullOrEmpty(raw) ? "—" : raw;
                    }
                }
            }
        }

        // ---- Visual helpers ------------------------------------------------

        // LED glow — sets the DropShadowEffect on the LED ellipse. When on,
        // colour matches the LED and radius blooms out; when off, opacity 0.
        private static void SetLedGlow(DropShadowEffect effect, Color color, bool on)
        {
            if (effect == null) return;
            if (on)
            {
                effect.Color      = color;
                effect.BlurRadius = 10;
                effect.Opacity    = 0.9;
            }
            else
            {
                effect.Opacity    = 0;
                effect.BlurRadius = 0;
            }
        }

        // Fills a temperature bar (0..100 °C mapped to the container width)
        // with a colour that shifts green → yellow → orange → red as temps rise.
        private void UpdateTempBar(Rectangle bar, double c, bool valid)
        {
            if (bar == null) return;
            var container = bar.Parent as FrameworkElement;
            double avail = container != null ? Math.Max(0, container.ActualWidth - 4) : 0;
            if (!valid || !IsFinite(c) || avail <= 0)
            {
                bar.Width = 0;
                return;
            }
            double pct   = Math.Max(0, Math.Min(1, c / 100.0));
            bar.Width    = avail * pct;
            bar.Fill     = TempColour(c);
        }

        // Iq bar: fills from the left as |Iq| grows against current_lim.
        // Last 5 % of the width is a permanent red "clip zone" that shows
        // when the reading is at or beyond the saturation threshold.
        private void UpdateCurrentBar(OdriveTelemetrySnapshot snap)
        {
            if (LiveIqBarFill == null) return;
            var container = LiveIqBarFill.Parent as FrameworkElement;
            double avail  = container != null ? Math.Max(0, container.ActualWidth - 4) : 0;

            double lim = 0;
            var svc = _plugin?.ProfileService;
            if (svc != null)
            {
                var raw = svc.GetRaw("axis0.motor.config.current_lim");
                if (!string.IsNullOrEmpty(raw))
                    double.TryParse(raw, NumberStyles.Float, CultureInfo.InvariantCulture, out lim);
            }

            // Clip zone at right — 5 % of the total width, tinted red-brown.
            LiveIqBarClipZone.Width = avail * 0.05;

            if (lim <= 0 || avail <= 0)
            {
                LiveIqBarFill.Width = 0;
                LiveIqBarText.Text  = lim <= 0 ? "no current_lim" : "";
                return;
            }
            double absIq = Math.Abs(snap.IqCurrentA);
            double pct   = Math.Max(0, Math.Min(1, absIq / lim));
            LiveIqBarFill.Width = avail * pct;
            // Colour by threshold — green under 80 %, orange 80-95 %, red ≥ 95 %.
            LiveIqBarFill.Fill  = pct < 0.80 ? BrushOk
                               : pct < 0.95 ? BrushOrange
                               :              BrushDanger;
            LiveIqBarText.Text  = $"{absIq:F1} / {lim:F0} A ({pct*100:F0} %)";
        }

        // Sparkline — polyline drawing on a Canvas. Recreates children each
        // call (cheap, few hundred samples). autoScale: false = fixed y range
        // (currently unused, all our sparklines auto-scale).
        private void DrawSparkline(Canvas canvas, double[] data, Color color, bool autoScale)
        {
            if (canvas == null) return;
            canvas.Children.Clear();
            if (data == null || data.Length < 2) return;

            double w = canvas.ActualWidth;
            double h = canvas.ActualHeight;
            if (w <= 2 || h <= 2) return;

            double min = data[0], max = data[0];
            for (int i = 1; i < data.Length; i++)
            {
                if (data[i] < min) min = data[i];
                if (data[i] > max) max = data[i];
            }
            double span = Math.Max(1e-6, max - min);
            // Small padding so the line doesn't kiss the top/bottom.
            double top  = 1, bot = h - 1;
            double xStep = (w - 2) / (data.Length - 1);

            var pts = new PointCollection(data.Length);
            for (int i = 0; i < data.Length; i++)
            {
                double x = 1 + i * xStep;
                double norm = (data[i] - min) / span;
                double y = bot - norm * (bot - top);
                pts.Add(new Point(x, y));
            }
            var line = new Polyline
            {
                Points          = pts,
                Stroke          = new SolidColorBrush(color),
                StrokeThickness = 1.2,
                StrokeLineJoin  = PenLineJoin.Round,
            };
            canvas.Children.Add(line);
        }

        // Two-trace scope. Shares one Y axis (min/max across both traces) so
        // the visual relationship between Iq and Torque is preserved. Draws
        // a subtle zero line + a light grid to give the wave shape context.
        private void DrawScope(Canvas canvas, double[] a, Color colorA, double[] b, Color colorB)
        {
            if (canvas == null) return;
            canvas.Children.Clear();
            double w = canvas.ActualWidth;
            double h = canvas.ActualHeight;
            if (w <= 4 || h <= 4) return;

            bool hasA = a != null && a.Length >= 2;
            bool hasB = b != null && b.Length >= 2;
            if (!hasA && !hasB)
            {
                var placeholder = new TextBlock
                {
                    Text = "waiting for data…",
                    Foreground = MakeBrush(0x55, 0x5A, 0x60),
                    FontSize = 11,
                };
                Canvas.SetLeft(placeholder, 8);
                Canvas.SetTop (placeholder, h / 2 - 8);
                canvas.Children.Add(placeholder);
                return;
            }

            // Shared min/max across visible traces, with a small pad.
            double min = double.PositiveInfinity, max = double.NegativeInfinity;
            if (hasA) foreach (var v in a) { if (v < min) min = v; if (v > max) max = v; }
            if (hasB) foreach (var v in b) { if (v < min) min = v; if (v > max) max = v; }
            if (min == max) { min -= 1; max += 1; }
            double range = max - min;
            min -= range * 0.10;
            max += range * 0.10;
            double span = max - min;

            // Grid — 3 horizontal lines including zero line if it's in view.
            var gridBrush = MakeBrush(0x2A, 0x2E, 0x33);
            for (int i = 1; i < 4; i++)
            {
                double y = h * i / 4;
                canvas.Children.Add(new Line
                {
                    X1 = 0, X2 = w, Y1 = y, Y2 = y,
                    Stroke = gridBrush, StrokeThickness = 0.5,
                });
            }
            // Zero line highlighted if in range.
            if (min <= 0 && max >= 0)
            {
                double y0 = h - ((0 - min) / span) * h;
                canvas.Children.Add(new Line
                {
                    X1 = 0, X2 = w, Y1 = y0, Y2 = y0,
                    Stroke = MakeBrush(0x40, 0x45, 0x4B),
                    StrokeThickness = 1,
                    StrokeDashArray = new DoubleCollection { 2, 3 },
                });
            }

            // Y-axis min/max labels (right edge, subtle).
            var lblBrush = MakeBrush(0x6B, 0x70, 0x78);
            var lblMax = new TextBlock { Text = max.ToString("F1"), Foreground = lblBrush, FontSize = 10, FontFamily = new FontFamily("Consolas") };
            Canvas.SetRight(lblMax, 4); Canvas.SetTop(lblMax, 2);
            canvas.Children.Add(lblMax);
            var lblMin = new TextBlock { Text = min.ToString("F1"), Foreground = lblBrush, FontSize = 10, FontFamily = new FontFamily("Consolas") };
            Canvas.SetRight(lblMin, 4); Canvas.SetBottom(lblMin, 2);
            canvas.Children.Add(lblMin);

            // Draw the two traces.
            if (hasA) canvas.Children.Add(BuildScopePolyline(a, colorA, w, h, min, span));
            if (hasB) canvas.Children.Add(BuildScopePolyline(b, colorB, w, h, min, span));
        }

        private static Polyline BuildScopePolyline(double[] data, Color color,
                                                   double w, double h, double min, double span)
        {
            var pts = new PointCollection(data.Length);
            double xStep = w / (data.Length - 1);
            for (int i = 0; i < data.Length; i++)
            {
                double norm = (data[i] - min) / span;
                pts.Add(new Point(i * xStep, h - norm * h));
            }
            var brush = new SolidColorBrush(color);
            brush.Freeze();
            return new Polyline
            {
                Points          = pts,
                Stroke          = brush,
                StrokeThickness = 1.4,
                StrokeLineJoin  = PenLineJoin.Round,
            };
        }
    }
}
