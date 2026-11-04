using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace HoYoShadeHub.CliInspection
{
    // Desktop metadata only. No focus, show, input, or process-creation APIs.
    public sealed class WindowState
    {
        public long Window { get; set; }
        public int Pid { get; set; }
        public bool Exists { get; set; }
        public bool TopLevel { get; set; }
        public bool Visible { get; set; }
        public bool Minimized { get; set; }
        public long Owner { get; set; }
        public int CloakStatus { get; set; }
        public uint Cloaked { get; set; }
        public bool ClientAvailable { get; set; }
        public int Width { get; set; }
        public int Height { get; set; }
        public string ClassName { get; set; }
        // Minimized windows are eligible for the launcher's existing one-time restore.
        public bool Candidate => Exists && TopLevel && Visible && Owner == 0
            && CloakStatus == 0 && Cloaked == 0 && ClientAvailable && Width > 0 && Height > 0;
        public bool RecoverableMain => Exists && TopLevel && Owner == 0
            && CloakStatus == 0 && Cloaked == 0 && ClientAvailable && Width > 0 && Height > 0
            && string.Equals(ClassName, "UnityWndClass", StringComparison.Ordinal);
    }

    public static class Windows
    {
        public static WindowState Inspect(long handle)
        {
            IntPtr window = new IntPtr(handle);
            uint pid;
            GetWindowThreadProcessId(window, out pid);
            Rect rect;
            bool client = GetClientRect(window, out rect);
            uint cloaked;
            int cloakStatus = DwmGetWindowAttribute(window, 14, out cloaked, sizeof(uint));
            var name = new StringBuilder(256);
            GetClassNameW(window, name, name.Capacity);
            return new WindowState {
                Window = handle, Pid = (int)pid, Exists = IsWindow(window),
                TopLevel = GetAncestor(window, 2) == window, Visible = IsWindowVisible(window),
                Minimized = IsIconic(window), Owner = GetWindow(window, 4).ToInt64(),
                CloakStatus = cloakStatus, Cloaked = cloaked, ClientAvailable = client,
                Width = rect.Right - rect.Left, Height = rect.Bottom - rect.Top, ClassName = name.ToString()
            };
        }

        public static WindowState[] ForProcess(int pid)
        {
            var states = new List<WindowState>();
            Exception error = null;
            EnumProc callback = (window, _) => {
                try {
                    uint actual;
                    GetWindowThreadProcessId(window, out actual);
                    if (actual == pid) states.Add(Inspect(window.ToInt64()));
                    return true;
                } catch (Exception ex) { error = ex; return false; }
            };
            bool success = EnumWindows(callback, IntPtr.Zero);
            if (error != null) throw error;
            if (!success) throw new Win32Exception(Marshal.GetLastWin32Error(), "EnumWindows failed.");
            return states.ToArray();
        }

        public static long Foreground() => GetForegroundWindow().ToInt64();
        private delegate bool EnumProc(IntPtr window, IntPtr parameter);
        [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
        [DllImport("user32.dll", SetLastError = true)] private static extern bool EnumWindows(EnumProc callback, IntPtr parameter);
        [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr window);
        [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
        [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr window);
        [DllImport("user32.dll")] private static extern IntPtr GetAncestor(IntPtr window, uint flags);
        [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr window, uint command);
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
        [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr window, out Rect rect);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassNameW(IntPtr window, StringBuilder name, int capacity);
        [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
        [DllImport("dwmapi.dll")] private static extern int DwmGetWindowAttribute(IntPtr window, uint attribute, out uint value, int size);
    }

    public sealed class ForegroundGrant
    {
        public bool Accepted { get; set; }
        public int Error { get; set; }
    }

    public static class ForegroundPermission
    {
        // Standard Windows permission delegation to ONE launcher CLI child only.
        // Windows rejects this when the helper itself lacks foreground permission.
        // This neither activates a window nor grants permission to arbitrary processes.
        public static ForegroundGrant Grant(int processId)
        {
            if (processId <= 0) throw new ArgumentOutOfRangeException(nameof(processId));
            bool accepted = AllowSetForegroundWindow((uint)processId);
            return new ForegroundGrant { Accepted = accepted, Error = accepted ? 0 : Marshal.GetLastWin32Error() };
        }
        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool AllowSetForegroundWindow(uint processId);
    }
}
