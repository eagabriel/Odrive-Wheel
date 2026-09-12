using System;
using System.Collections.Generic;
using System.IO.Ports;
using System.Management;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace OdriveWheel.SimHubPlugin
{
    /// <summary>
    /// On-demand serial CLI client for the Odrive-Wheel board.
    ///
    /// Protocol notes (mirrors what the HTML tool does):
    ///
    ///   • OpenFFBoard commands (class.property syntax):
    ///       - Read:  "axis.range?\n"  →  "[axis.range?|900]\n"
    ///       - Write: "axis.range=900\n"  →  "[axis.range=900|900]\n"
    ///       - Exec:  "sys.save!\n"  →  "[sys.save!|OK]\n"
    ///     The value is extracted from between the pipe and the closing bracket.
    ///
    ///   • ODrive ASCII commands (r/w for deep paths like axis0.motor.config.*):
    ///       - Read:  "r axis0.motor.config.current_lim\n"  →  "25.000000\n"
    ///       - Write: "w axis0.motor.config.current_lim 25\n"  →  (silent on success)
    ///                Errors return text like "invalid property".
    ///       - "ss" persists ODrive config to NVM (reboots).
    ///
    /// Coexistence with the HTML tool: this class opens the port only for the
    /// duration of one operation, then closes it. If the HTML tool already
    /// holds the port, Open() throws and the caller retries later.
    ///
    /// COM port detection: enumerates USB devices via WMI, finds the one with
    /// VID_1209 PID_0D40, extracts the COMn assignment. Cached briefly to
    /// avoid the ~100 ms WMI round-trip on every read.
    /// </summary>
    public sealed class OdriveSerialCli
    {
        private const int VID = 0x1209;
        private const int PID = 0x0D40;
        private const int BAUD = 115200;

        // WMI is slow; cache the discovered port name for a few seconds.
        private string _cachedPort;
        private DateTime _cachedPortUtc = DateTime.MinValue;
        private static readonly TimeSpan PORT_CACHE_TTL = TimeSpan.FromSeconds(30);

        private readonly object _ioLock = new object();
        private readonly Action<string> _log;

        // Optional long-lived port for a batched session. When non-null, all
        // SendLine calls reuse it instead of opening a fresh SerialPort per
        // command. Used by ReadAllOnce and ApplyProfile so the OS-level port
        // is held for ONE bounded window (~0.2 s) rather than rapidly cycled
        // open/close 30+ times during the initial read.
        private SerialPort _sessionPort;

        public OdriveSerialCli(Action<string> log)
        {
            _log = log ?? (_ => { });
        }

        /// <summary>Opens the port and holds it. Follow with any number of
        /// ReadOffb/WriteOffb/ReadOdrive/WriteOdrive calls, then EndSession().
        /// Returns false if the port isn't found or is busy (in which case
        /// EndSession is a no-op).</summary>
        public bool BeginSession()
        {
            lock (_ioLock)
            {
                if (_sessionPort != null) return true;
                var port = FindPortName(fresh: false);
                if (string.IsNullOrEmpty(port))
                {
                    _log("BeginSession: no port found");
                    return false;
                }
                SerialPort sp = null;
                try
                {
                    sp = BuildPort(port, 200);
                    sp.Open();
                    Thread.Sleep(20);
                    try { sp.DiscardInBuffer(); } catch { }
                    _sessionPort = sp;
                    return true;
                }
                catch (Exception ex)
                {
                    _log("BeginSession: " + ex.Message);
                    if (sp != null) { try { sp.Dispose(); } catch { } }
                    _sessionPort = null;
                    return false;
                }
            }
        }

        public void EndSession()
        {
            lock (_ioLock)
            {
                if (_sessionPort == null) return;
                try { _sessionPort.Close(); }   catch { }
                try { _sessionPort.Dispose(); } catch { }
                _sessionPort = null;
            }
        }

        private SerialPort BuildPort(string port, int readTimeoutMs)
        {
            return new SerialPort(port, BAUD, Parity.None, 8, StopBits.One)
            {
                NewLine      = "\n",
                ReadTimeout  = readTimeoutMs,
                WriteTimeout = 500,
                Handshake    = Handshake.None,
                // DTR/RTS MUST be true — matches what WebSerial in
                // Chrome/Edge sets by default. Many USB CDC ACM firmwares
                // (including TinyUSB on the STM32) treat DTR=false as
                // "host disconnected" and silently drop incoming bytes.
                DtrEnable    = true,
                RtsEnable    = true,
                Encoding     = Encoding.ASCII,
            };
        }

        // ---- Public API ------------------------------------------------------

        /// <summary>Read an OpenFFBoard property. Returns null on failure.</summary>
        public string ReadOffb(string path, int timeoutMs = 500)
        {
            var reply = SendLine(path + "?", true, timeoutMs);
            return ExtractOffbValue(reply);
        }

        /// <summary>Write an OpenFFBoard property. Returns true on success.</summary>
        public bool WriteOffb(string path, string value, int timeoutMs = 500)
        {
            var reply = SendLine(path + "=" + value, true, timeoutMs);
            var v = ExtractOffbValue(reply);
            return v != null;
        }

        /// <summary>Exec an OpenFFBoard action (e.g. "sys.save"). Returns reply value or null.</summary>
        public string ExecOffb(string path, int timeoutMs = 2000)
        {
            var reply = SendLine(path + "!", true, timeoutMs);
            return ExtractOffbValue(reply);
        }

        /// <summary>Read an ODrive ASCII property. Returns raw first-line reply or null.</summary>
        public string ReadOdrive(string path, int timeoutMs = 500)
        {
            return SendLine("r " + path, true, timeoutMs)?.Trim();
        }

        /// <summary>
        /// Write an ODrive ASCII property. ODrive is SILENT on success — any
        /// reply within ~80 ms is an error message we log and treat as failure.
        /// </summary>
        public bool WriteOdrive(string path, string value, int timeoutMs = 120)
        {
            // Special-case timeoutMs: with true silence expected, we WANT
            // the timeout to fire; that's success. A non-null reply = error.
            var reply = SendLine("w " + path + " " + value, true, timeoutMs);
            if (reply != null)
            {
                _log($"WriteOdrive {path}={value} rejected: {reply}");
                return false;
            }
            return true;
        }

        /// <summary>Persist ODrive config to NVM. Reboots the board.</summary>
        public bool ODriveSave(int timeoutMs = 2000)
        {
            var reply = SendLine("ss", true, timeoutMs);
            // "ss" typically returns nothing or a status; treat non-error as ok.
            return true;
        }

        /// <summary>true if the device is currently plugged in (WMI-discoverable).</summary>
        public bool IsDevicePresent()
        {
            return !string.IsNullOrEmpty(FindPortName(fresh: true));
        }

        public string CurrentPortName => _cachedPort;

        // ---- Internal I/O ----------------------------------------------------

        // One line in, one line out. Serializes port access so concurrent
        // callers from the ProfileService don't step on each other. If a
        // batched session is open, reuses that port; otherwise opens a
        // fresh SerialPort just for this call.
        private string SendLine(string cmd, bool expectReply, int timeoutMs)
        {
            lock (_ioLock)
            {
                // Fast path: reuse the batched session port.
                if (_sessionPort != null)
                {
                    try
                    {
                        _sessionPort.ReadTimeout = timeoutMs;
                        return SendLineOn(_sessionPort, cmd, expectReply, timeoutMs);
                    }
                    catch (Exception ex)
                    {
                        _log("SendLine (session) error: " + ex.Message);
                        return null;
                    }
                }

                // Slow path: open-per-call, for one-off ops outside a session.
                string port = FindPortName(fresh: false);
                if (string.IsNullOrEmpty(port))
                {
                    _log("SendLine: no port found");
                    return null;
                }

                SerialPort sp = null;
                try
                {
                    sp = BuildPort(port, timeoutMs);
                    try { sp.Open(); }
                    catch (UnauthorizedAccessException)
                    {
                        _log("SendLine: port busy (in use by another app)");
                        return null;
                    }
                    // Line-settle delay so first char isn't eaten by a CDC
                    // that resets RX state when DTR toggles.
                    Thread.Sleep(20);
                    try { sp.DiscardInBuffer(); } catch { }
                    return SendLineOn(sp, cmd, expectReply, timeoutMs);
                }
                catch (Exception ex)
                {
                    _log("SendLine error: " + ex.Message);
                    _cachedPort = null;
                    _cachedPortUtc = DateTime.MinValue;
                    return null;
                }
                finally
                {
                    if (sp != null)
                    {
                        try { sp.Close(); } catch { }
                        try { sp.Dispose(); } catch { }
                    }
                }
            }
        }

        // Shared inner loop — write one line, wait for one reply, ignore
        // blank lines (some firmwares emit them as keep-alives).
        //
        // Discards the RX buffer BEFORE the write. Critical for session mode:
        // during a batched ApplyProfile, a previous command's late reply or
        // an async diagnostic line from the firmware can sit in the buffer
        // and get read as the current command's reply, causing WriteOffb to
        // see a mismatched string and report false for every subsequent
        // field. Single-shot mode already discards on connect, but session
        // reuses the port so we must flush here too.
        private static string SendLineOn(SerialPort sp, string cmd, bool expectReply, int timeoutMs)
        {
            try { sp.DiscardInBuffer(); } catch { }
            sp.WriteLine(cmd);
            if (!expectReply) return null;

            var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (DateTime.UtcNow < deadline)
            {
                try
                {
                    sp.ReadTimeout = Math.Max(50, (int)(deadline - DateTime.UtcNow).TotalMilliseconds);
                    var line = sp.ReadLine();
                    if (string.IsNullOrEmpty(line)) continue;
                    line = line.TrimEnd('\r', '\n');
                    if (line.Length == 0) continue;
                    return line;
                }
                catch (TimeoutException) { return null; }
            }
            return null;
        }

        // ---- Reply parsing ---------------------------------------------------

        // OpenFFBoard replies come as "[path?|VALUE]" or "[path=x|VALUE]".
        // Extract just VALUE from between the pipe and the closing bracket.
        private static string ExtractOffbValue(string line)
        {
            if (string.IsNullOrEmpty(line)) return null;
            var m = Regex.Match(line, @"^\[[^|]+\|([\s\S]*)\]$");
            return m.Success ? m.Groups[1].Value : null;
        }

        // ---- COM port discovery ---------------------------------------------

        // Enumerates USB devices via WMI, matches by VID/PID, extracts COMn.
        // The relevant Win32_PnPEntity rows have a Name like
        //   "USB Serial Device (COM12)"
        // and a DeviceID starting with "USB\VID_1209&PID_0D40\...".
        private string FindPortName(bool fresh)
        {
            if (!fresh && !string.IsNullOrEmpty(_cachedPort) &&
                DateTime.UtcNow - _cachedPortUtc < PORT_CACHE_TTL)
            {
                return _cachedPort;
            }

            string vidpid = string.Format("VID_{0:X4}&PID_{1:X4}", VID, PID);
            string discovered = null;

            try
            {
                using (var searcher = new ManagementObjectSearcher(
                    "SELECT DeviceID, Name FROM Win32_PnPEntity WHERE Name LIKE '%(COM%)'"))
                {
                    foreach (ManagementObject obj in searcher.Get())
                    {
                        var deviceId = obj["DeviceID"] as string ?? "";
                        var name     = obj["Name"]     as string ?? "";
                        if (deviceId.IndexOf(vidpid, StringComparison.OrdinalIgnoreCase) < 0)
                            continue;
                        var m = Regex.Match(name, @"\((COM\d+)\)");
                        if (m.Success)
                        {
                            discovered = m.Groups[1].Value;
                            break;
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                _log("WMI enumerate failed: " + ex.Message);
            }

            _cachedPort    = discovered;
            _cachedPortUtc = DateTime.UtcNow;
            return discovered;
        }
    }
}
