using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace OdriveWheel.SimHubPlugin
{
    /// <summary>
    /// Reads a profile .json produced by the HTML tool's Profiles tab.
    ///
    /// Expected format (top level) — matches what the HTML tool writes:
    ///   {
    ///     "_type":    "odrive-wheel-profile",
    ///     "_version": 1,
    ///     "name":     "MyRallyProfile",
    ///     "created":  "...",
    ///     "modified": "...",
    ///     "values": {
    ///        "axis.range":     "900",
    ///        "axis.maxtorque": "12.00",
    ///        ...
    ///     }
    ///   }
    ///
    /// Older exports used `"kind"` instead of `"_type"`; both are accepted
    /// as the discriminator for backwards compatibility.
    ///
    /// A separate export shape used by "Export current config" wraps every
    /// path directly at top level (no `values` key, no type marker). This
    /// loader accepts BOTH shapes so the user can pick either kind of file.
    /// Values are always returned as strings (no type coercion) — the
    /// profile service converts as needed per-field.
    /// </summary>
    public static class OdriveProfileFile
    {
        public const string ExpectedKind = "odrive-wheel-profile";

        public sealed class LoadedProfile
        {
            public string                       Name   { get; internal set; } = "";
            public string                       Kind   { get; internal set; } = "";
            public IDictionary<string, string>  Values { get; internal set; }
                                                = new Dictionary<string, string>();
            /// <summary>true iff it's a real "odrive-wheel-profile" (not a bare export).</summary>
            public bool IsBrandedProfile => Kind == ExpectedKind;
        }

        public static LoadedProfile Load(string path)
        {
            var text = File.ReadAllText(path);
            var root = JObject.Parse(text);
            var p = new LoadedProfile();
            // Accept both "_type" (current HTML tool) and "kind" (legacy).
            p.Kind = (root["_type"] ?? root["kind"] ?? "").ToString();
            p.Name = (root["name"] ?? Path.GetFileNameWithoutExtension(path)).ToString();

            // Preferred: values under "values"
            var vals = root["values"] as JObject;
            if (vals == null)
            {
                // Fallback: bare export — every top-level string entry that
                // LOOKS like a path (contains a dot) is treated as a value.
                vals = new JObject();
                foreach (var kv in root)
                {
                    // Skip metadata keys: leading "_" (like "_type", "_version"),
                    // plus the legacy top-level ones.
                    if (kv.Key.StartsWith("_")) continue;
                    if (kv.Key == "kind" || kv.Key == "name" ||
                        kv.Key == "meta" || kv.Key == "created" ||
                        kv.Key == "modified") continue;
                    if (!kv.Key.Contains(".")) continue;
                    if (kv.Value == null || kv.Value.Type != JTokenType.String) continue;
                    vals[kv.Key] = kv.Value;
                }
            }
            foreach (var kv in vals)
            {
                if (kv.Value == null) continue;
                p.Values[kv.Key] = kv.Value.ToString();
            }
            return p;
        }

        /// <summary>Lists .json files in the folder whose top-level has kind="odrive-wheel-profile".
        /// Files that fail to parse or don't match are silently skipped.</summary>
        public static IEnumerable<(string Name, string Path)> ListInFolder(string folder)
        {
            if (string.IsNullOrEmpty(folder) || !Directory.Exists(folder))
                yield break;
            foreach (var path in Directory.GetFiles(folder, "*.json"))
            {
                LoadedProfile p = null;
                try { p = Load(path); }
                catch { continue; }
                if (!p.IsBrandedProfile) continue;
                yield return (p.Name, path);
            }
        }

        /// <summary>
        /// Writes a profile to disk in the exact format the HTML tool uses.
        /// If <paramref name="path"/> exists, the file's "created" timestamp is
        /// preserved and only "modified" is refreshed; otherwise both are set
        /// to now. Values are written as strings (matches the HTML side).
        ///
        /// Layout produced:
        ///   {
        ///     "_type":    "odrive-wheel-profile",
        ///     "_version": 1,
        ///     "name":     "...",
        ///     "created":  "ISO-8601 UTC",
        ///     "modified": "ISO-8601 UTC",
        ///     "values":   { "axis.range": "900", ... }
        ///   }
        /// </summary>
        public static void Save(string path, string name, IDictionary<string, string> values)
        {
            if (string.IsNullOrEmpty(path))  throw new ArgumentException("path is empty");
            if (values == null)              throw new ArgumentNullException(nameof(values));

            var nowIso = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ",
                                                  CultureInfo.InvariantCulture);
            string createdIso = nowIso;

            // Preserve original "created" if the file already exists — matches
            // the HTML tool's "overwrite" behaviour where only "modified" moves.
            if (File.Exists(path))
            {
                try
                {
                    var existing = JObject.Parse(File.ReadAllText(path));
                    var c = existing["created"]?.ToString();
                    if (!string.IsNullOrEmpty(c)) createdIso = c;
                }
                catch { /* corrupt existing file — start fresh */ }
            }

            var root = new JObject
            {
                ["_type"]    = ExpectedKind,
                ["_version"] = 1,
                ["name"]     = string.IsNullOrWhiteSpace(name)
                                   ? Path.GetFileNameWithoutExtension(path)
                                   : name.Trim(),
                ["created"]  = createdIso,
                ["modified"] = nowIso,
                ["values"]   = new JObject(),
            };
            var valuesObj = (JObject)root["values"];
            foreach (var kv in values)
            {
                if (string.IsNullOrEmpty(kv.Key)) continue;
                valuesObj[kv.Key] = kv.Value ?? "";
            }

            var json = root.ToString(Formatting.Indented);
            // Atomic write via temp file + move — avoids leaving a truncated
            // file on disk if the process is killed mid-write.
            var tmp = path + ".tmp";
            File.WriteAllText(tmp, json, new UTF8Encoding(false));
            if (File.Exists(path)) File.Delete(path);
            File.Move(tmp, path);
        }

        /// <summary>Sanitises a profile name into a safe filename (no path
        /// separators or reserved characters). Returns just the file base
        /// name — caller adds the folder path and .json extension.</summary>
        public static string ToSafeFileName(string profileName)
        {
            if (string.IsNullOrWhiteSpace(profileName)) return "profile";
            var invalid = Path.GetInvalidFileNameChars();
            var chars = profileName.Trim().ToCharArray();
            for (int i = 0; i < chars.Length; i++)
            {
                foreach (var c in invalid) if (chars[i] == c) { chars[i] = '_'; break; }
            }
            return new string(chars);
        }
    }
}
