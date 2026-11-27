// Shots.cs: opens brodalf.exe on a Windows desktop, goes through its tabs,
// menus and dialogs, and saves a screenshot of each, for people to look
// at. It is a test tool only: it is not part of BRODALF and not shipped.
//
// It works from outside, as a person would: it sends the window the same
// WM_COMMAND messages its menus and buttons send, presses keys, and copies
// what is on the screen. Every dialog it opens it closes with Cancel or
// No, so it changes nothing, except where a step says so (the backup at
// the end, to show the window while a job runs).
//
// Written for the C# 5 compiler in .NET Framework (csc.exe), which every
// Windows PC has:
//   csc /platform:x64 /r:System.Drawing.dll /out:shots.exe Shots.cs
// Then:
//   shots.exe --resolution 1920 1080
//   shots.exe --exe brodalf.exe --db "C:\BrodalfDemo\My backups.brodalf" --ids ids.txt
//             --out shots\en-light --lang en --theme light --pass PASSPHRASE
//             --vhd-dir C:\BrodalfVHD [--extras]
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32;

[StructLayout(LayoutKind.Sequential)]
struct RECT { public int Left, Top, Right, Bottom; }

[StructLayout(LayoutKind.Sequential)]
struct TVITEM {
    public uint mask; public IntPtr hItem; public uint state, stateMask; public IntPtr pszText;
    public int cchTextMax, iImage, iSelectedImage, cChildren; public IntPtr lParam;
}

[StructLayout(LayoutKind.Sequential)]
struct LVITEM {
    public uint mask; public int iItem, iSubItem; public uint state, stateMask; public IntPtr pszText;
    public int cchTextMax, iImage; public IntPtr lParam; public int iIndent, iGroupId; public uint cColumns;
    public IntPtr puColumns, piColFmt; public int iGroup;
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
    public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
    public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
    public short dmLogPixels;
    public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
}

static class N {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr wp, IntPtr lp, uint flags, uint ms, out IntPtr result);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageTimeoutW")]
    public static extern IntPtr SendMessageTimeoutStr(IntPtr h, uint msg, IntPtr wp, string lp, uint flags, uint ms, out IntPtr result);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageTimeoutW")]
    public static extern IntPtr SendMessageTimeoutSb(IntPtr h, uint msg, IntPtr wp, StringBuilder lp, uint flags, uint ms, out IntPtr result);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr m, int pos);
    [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr m);
    [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr m, uint id, uint flags);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int ChangeDisplaySettings(ref DEVMODE dm, int flags);
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern IntPtr VirtualAllocEx(IntPtr p, IntPtr addr, UIntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll")] public static extern bool VirtualFreeEx(IntPtr p, IntPtr addr, UIntPtr size, uint type);
    [DllImport("kernel32.dll")] public static extern bool WriteProcessMemory(IntPtr p, IntPtr addr, byte[] buf, UIntPtr n, out UIntPtr done);
    [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr p, IntPtr addr, byte[] buf, UIntPtr n, out UIntPtr done);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}

// A block of memory in brodalf.exe, for the list and tree messages that
// take a pointer (Windows copies those only for its oldest controls).
class Remote : IDisposable {
    IntPtr proc, mem;
    const int Size = 8192;
    public Remote(int pid) {
        proc = N.OpenProcess(0x0008 | 0x0010 | 0x0020 | 0x0400, false, pid);
        if (proc == IntPtr.Zero) throw new Exception("cannot open brodalf.exe's memory");
        mem = N.VirtualAllocEx(proc, IntPtr.Zero, (UIntPtr)Size, 0x3000, 0x04);
        if (mem == IntPtr.Zero) throw new Exception("cannot allocate in brodalf.exe");
    }
    public IntPtr At(int off) { return new IntPtr(mem.ToInt64() + off); }
    public void Write(int off, byte[] b) { UIntPtr n; N.WriteProcessMemory(proc, At(off), b, (UIntPtr)b.Length, out n); }
    public byte[] Read(int off, int len) { byte[] b = new byte[len]; UIntPtr n; N.ReadProcessMemory(proc, At(off), b, (UIntPtr)len, out n); return b; }
    public void Dispose() {
        if (mem != IntPtr.Zero) N.VirtualFreeEx(proc, mem, UIntPtr.Zero, 0x8000);
        if (proc != IntPtr.Zero) N.CloseHandle(proc);
        mem = proc = IntPtr.Zero;
    }
    public static byte[] Bytes<T>(T s) where T : struct {
        int n = Marshal.SizeOf(typeof(T));
        byte[] b = new byte[n];
        IntPtr p = Marshal.AllocHGlobal(n);
        Marshal.StructureToPtr(s, p, false);
        Marshal.Copy(p, b, 0, n);
        Marshal.FreeHGlobal(p);
        return b;
    }
    public string ReadString(int off, int chars) {
        string s = Encoding.Unicode.GetString(Read(off, chars * 2));
        int z = s.IndexOf('\0');
        return z >= 0 ? s.Substring(0, z) : s;
    }
}

static class Shots {
    const uint WM_COMMAND = 0x0111, WM_CLOSE = 0x0010, WM_SETTEXT = 0x000C, WM_GETTEXT = 0x000D, WM_GETTEXTLENGTH = 0x000E;
    const uint WM_CANCELMODE = 0x001F, BFFM_SETSELECTIONW = 0x0400 + 103;
    const uint WM_USER = 0x0400, TDM_CLICK_BUTTON = WM_USER + 102, MN_GETHMENU = 0x01E1;
    const uint TVM_EXPAND = 0x1102, TVM_GETNEXTITEM = 0x110A, TVM_SELECTITEM = 0x110B, TVM_GETITEMW = 0x113E, TVM_ENSUREVISIBLE = 0x1114;
    const uint LVM_GETITEMCOUNT = 0x1004, LVM_ENSUREVISIBLE = 0x1013, LVM_SETITEMSTATE = 0x102B, LVM_GETITEMTEXTW = 0x1073;
    const uint CB_GETCOUNT = 0x0146, CB_GETLBTEXT = 0x0148, CB_GETLBTEXTLEN = 0x0149, CB_SETCURSEL = 0x014E;
    const int IDOK = 1, IDCANCEL = 2, IDYES = 6, IDNO = 7;
    const byte VK_RETURN = 0x0D, VK_ESCAPE = 0x1B, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_F10 = 0x79, VK_HOME = 0x24;

    static Dictionary<string, int> ids = new Dictionary<string, int>();
    static string exe, db, outDir, lang = "en", theme = "light", pass = "", vhdDir = "";
    static bool extras;
    static Process proc;
    static IntPtr main;
    static int shotNo;
    static StreamWriter manifest, log;
    static HashSet<IntPtr> handled = new HashSet<IntPtr>();

    static int Main(string[] args) {
        N.SetProcessDPIAware();
        try {
            if (args.Length == 3 && args[0] == "--resolution") return SetResolution(int.Parse(args[1]), int.Parse(args[2]));
            for (int i = 0; i + 1 < args.Length; i++) {
                if (args[i] == "--exe") exe = args[++i];
                else if (args[i] == "--db") db = args[++i];
                else if (args[i] == "--out") outDir = args[++i];
                else if (args[i] == "--lang") lang = args[++i];
                else if (args[i] == "--theme") theme = args[++i];
                else if (args[i] == "--pass") pass = args[++i];
                else if (args[i] == "--vhd-dir") vhdDir = args[++i];
                else if (args[i] == "--ids") LoadIds(args[++i]);
            }
            foreach (string a in args) if (a == "--extras") extras = true;
            Directory.CreateDirectory(outDir);
            manifest = new StreamWriter(Path.Combine(outDir, "manifest.tsv"), false, new UTF8Encoding(false));
            log = new StreamWriter(Path.Combine(outDir, "driver.log"), false, new UTF8Encoding(false));
            manifest.AutoFlush = log.AutoFlush = true;
            Run();
            return 0;
        } catch (Exception e) {
            Log("FAILED: " + e);
            Console.Error.WriteLine(e);
            return 1;
        } finally {
            try { if (proc != null && !proc.HasExited) { Log("killing brodalf.exe"); proc.Kill(); } } catch (Exception) { }
            if (manifest != null) manifest.Close();
            if (log != null) log.Close();
        }
    }

    static void LoadIds(string path) {
        foreach (string line in File.ReadAllLines(path)) {
            int eq = line.IndexOf('=');
            if (eq > 0) ids[line.Substring(0, eq).Trim()] = int.Parse(line.Substring(eq + 1).Trim());
        }
    }

    static int Id(string name) {
        int v;
        if (!ids.TryGetValue(name, out v)) throw new Exception("no ID named " + name + " in the window app's source");
        return v;
    }

    static void Log(string s) {
        string line = DateTime.Now.ToString("HH:mm:ss.fff") + " " + s;
        Console.WriteLine(line);
        if (log != null) log.WriteLine(line);
    }

    // ---- The screen ------------------------------------------------------

    static int SetResolution(int w, int h) {
        DEVMODE dm = new DEVMODE();
        dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        int best = -1, bw = 0, bh = 0;
        for (int i = 0; N.EnumDisplaySettings(null, i, ref dm); i++) {
            Console.WriteLine("mode {0}x{1} {2} bit", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel);
            if (dm.dmPelsWidth <= w && dm.dmPelsHeight <= h && dm.dmBitsPerPel >= 24 && dm.dmPelsWidth * dm.dmPelsHeight > bw * bh) {
                best = i; bw = dm.dmPelsWidth; bh = dm.dmPelsHeight;
            }
        }
        if (best < 0) { Console.WriteLine("no display mode up to {0}x{1}", w, h); return 0; }
        N.EnumDisplaySettings(null, best, ref dm);
        dm.dmFields = 0x00080000 | 0x00100000; // DM_PELSWIDTH | DM_PELSHEIGHT
        int r = N.ChangeDisplaySettings(ref dm, 0);
        Console.WriteLine("display set to {0}x{1}: result {2}; screen now {3}x{4}", bw, bh, r, N.GetSystemMetrics(0), N.GetSystemMetrics(1));
        return 0;
    }

    static Rectangle Bounds(IntPtr h) {
        RECT r;
        if (N.DwmGetWindowAttribute(h, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, out r, Marshal.SizeOf(typeof(RECT))) != 0) N.GetWindowRect(h, out r);
        return Rectangle.FromLTRB(r.Left, r.Top, r.Right, r.Bottom);
    }

    static Rectangle OnScreen(Rectangle r) {
        Rectangle screen = new Rectangle(0, 0, N.GetSystemMetrics(0), N.GetSystemMetrics(1));
        r.Intersect(screen);
        return r;
    }

    static string Slug(string name) {
        StringBuilder sb = new StringBuilder();
        foreach (char c in name.ToLowerInvariant()) sb.Append(char.IsLetterOrDigit(c) ? c : '-');
        return sb.ToString();
    }

    static void Save(Rectangle r, string name, string kind, IntPtr h) {
        r = OnScreen(r);
        if (r.Width < 8 || r.Height < 8) { Log("nothing on screen to capture for " + name); return; }
        string file = string.Format("{0:000}-{1}.png", ++shotNo, Slug(name));
        using (Bitmap bmp = new Bitmap(r.Width, r.Height, PixelFormat.Format24bppRgb)) {
            using (Graphics g = Graphics.FromImage(bmp)) g.CopyFromScreen(r.Left, r.Top, 0, 0, r.Size);
            bmp.Save(Path.Combine(outDir, file), ImageFormat.Png);
        }
        string title = h != IntPtr.Zero ? Text(h) : "";
        string words = h != IntPtr.Zero ? DialogWords(h) : "";
        manifest.WriteLine(string.Join("\t", new string[] { file, kind, Clean(title), Clean(words), r.Width + "x" + r.Height }));
        Log("shot " + file + (words.Length > 0 ? ": " + Clean(words) : ""));
    }

    static string Clean(string s) { return s.Replace("\t", " ").Replace("\r", " ").Replace("\n", " | "); }

    static void Shot(IntPtr h, string name) {
        Thread.Sleep(500);
        Save(Bounds(h), name, Kind(h), h);
    }

    // The window with any menus that are open over it.
    static void ShotWithMenus(string name) {
        Thread.Sleep(500);
        Rectangle r = Bounds(main);
        foreach (IntPtr m in Windows("#32768")) r = Rectangle.Union(r, Bounds(m));
        Save(r, name, "menu", IntPtr.Zero);
    }

    static void ShotTogether(IntPtr a, IntPtr b, string name) {
        Thread.Sleep(500);
        Save(Rectangle.Union(Bounds(a), Bounds(b)), name, "window+dialog", b);
    }

    // ---- Windows -----------------------------------------------------------

    static string ClassOf(IntPtr h) { StringBuilder sb = new StringBuilder(256); N.GetClassName(h, sb, 256); return sb.ToString(); }

    static string Text(IntPtr h) {
        IntPtr len;
        if (N.SendMessageTimeout(h, WM_GETTEXTLENGTH, IntPtr.Zero, IntPtr.Zero, 2, 2000, out len) == IntPtr.Zero) return "";
        StringBuilder sb = new StringBuilder((int)len + 2);
        IntPtr got;
        N.SendMessageTimeoutSb(h, WM_GETTEXT, (IntPtr)((int)len + 1), sb, 2, 2000, out got);
        return sb.ToString();
    }

    static List<IntPtr> Children(IntPtr parent) {
        List<IntPtr> l = new List<IntPtr>();
        N.EnumChildWindows(parent, delegate(IntPtr h, IntPtr lp) { l.Add(h); return true; }, IntPtr.Zero);
        return l;
    }

    // The text a message box or dialog shows (a task dialog draws its own: none).
    static string DialogWords(IntPtr h) {
        if (ClassOf(h) != "#32770") return "";
        StringBuilder sb = new StringBuilder();
        foreach (IntPtr c in Children(h)) {
            string cls = ClassOf(c);
            if ((cls == "Static" || cls == "Button") && N.IsWindowVisible(c)) {
                string t = Text(c);
                if (t.Length > 0) sb.Append(sb.Length > 0 ? " / " : "").Append(t);
            }
        }
        return sb.ToString();
    }

    static string Kind(IntPtr h) {
        if (h == main) return "window";
        if (IsTaskDialog(h)) return "task dialog";
        return "dialog";
    }

    static bool IsTaskDialog(IntPtr h) {
        bool dui = false, shell = false;
        foreach (IntPtr c in Children(h)) {
            string cls = ClassOf(c);
            if (cls == "DirectUIHWND") dui = true;
            if (cls == "DUIViewWndClassName" || cls == "SHELLDLL_DefView" || cls == "SysTreeView32") shell = true;
        }
        return dui && !shell;
    }

    static List<IntPtr> Windows(string cls) {
        List<IntPtr> l = new List<IntPtr>();
        if (proc == null) return l;
        uint want = (uint)proc.Id;
        N.EnumWindows(delegate(IntPtr h, IntPtr lp) {
            uint pid;
            N.GetWindowThreadProcessId(h, out pid);
            if (pid == want && N.IsWindowVisible(h) && ClassOf(h) == cls) l.Add(h);
            return true;
        }, IntPtr.Zero);
        return l;
    }

    // The dialog that has the input now (a dialog over a dialog disables the one under it).
    static IntPtr TopDialog() {
        IntPtr found = IntPtr.Zero;
        foreach (IntPtr d in Windows("#32770")) if (N.IsWindowEnabled(d)) found = d;
        return found;
    }

    static IntPtr WaitDialog(int ms, IntPtr not) {
        Stopwatch sw = Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < ms) {
            IntPtr d = TopDialog();
            if (d != IntPtr.Zero && d != not) { Thread.Sleep(700); return TopDialog() != IntPtr.Zero ? TopDialog() : d; }
            Thread.Sleep(100);
        }
        return IntPtr.Zero;
    }

    static bool WaitGone(IntPtr h, int ms) {
        Stopwatch sw = Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < ms) {
            if (!N.IsWindow(h) || !N.IsWindowVisible(h)) return true;
            Thread.Sleep(100);
        }
        return false;
    }

    static void Post(IntPtr h, uint msg, int wp, IntPtr lp) { N.PostMessage(h, msg, (IntPtr)wp, lp); }

    static IntPtr Send(IntPtr h, uint msg, IntPtr wp, IntPtr lp) {
        IntPtr r;
        N.SendMessageTimeout(h, msg, wp, lp, 2, 5000, out r);
        return r;
    }

    static void ClickTask(IntPtr d, int button) { Post(d, TDM_CLICK_BUTTON, button, IntPtr.Zero); }

    // Say yes to one of BRODALF's questions: Yes on a message box, or the
    // yes button of a task dialog (BRODALF gives it the ID IDYES). With no
    // yes to press: OK if ok, else close it as Dismiss does.
    static void Yes(IntPtr d, bool ok) {
        if (IsTaskDialog(d)) {
            ClickTask(d, IDYES);
            if (WaitGone(d, 1500)) return;
        } else if (N.GetDlgItem(d, IDYES) != IntPtr.Zero) {
            Post(d, WM_COMMAND, IDYES, IntPtr.Zero);
            return;
        }
        if (ok) Post(d, WM_COMMAND, IDOK, IntPtr.Zero);
        else Dismiss(d);
    }

    // Close a dialog without saying yes to anything: Cancel, else No, else
    // (a message box with one button) OK, else the close box.
    static void Dismiss(IntPtr d) {
        if (d == IntPtr.Zero || !N.IsWindow(d)) return;
        if (IsTaskDialog(d)) {
            ClickTask(d, IDCANCEL);
            if (WaitGone(d, 1500)) return;
            Post(d, WM_CLOSE, 0, IntPtr.Zero);
            if (WaitGone(d, 1500)) return;
        }
        Post(d, WM_COMMAND, IDCANCEL, IntPtr.Zero);
        if (WaitGone(d, 1200)) return;
        if (N.GetDlgItem(d, IDNO) != IntPtr.Zero) {
            Post(d, WM_COMMAND, IDNO, IntPtr.Zero);
            if (WaitGone(d, 1200)) return;
        }
        int buttons = 0;
        foreach (IntPtr c in Children(d)) if (ClassOf(c) == "Button" && N.IsWindowVisible(c)) buttons++;
        if (buttons == 1 && N.GetDlgItem(d, IDOK) != IntPtr.Zero) {
            Post(d, WM_COMMAND, IDOK, IntPtr.Zero);
            if (WaitGone(d, 1200)) return;
        }
        Post(d, WM_CLOSE, 0, IntPtr.Zero);
        if (!WaitGone(d, 1500)) Log("could not close a dialog: " + Clean(DialogWords(d)));
    }

    static void Foreground(IntPtr h) {
        if (N.GetForegroundWindow() == h) return;
        uint pid;
        uint fg = N.GetWindowThreadProcessId(N.GetForegroundWindow(), out pid);
        uint target = N.GetWindowThreadProcessId(h, out pid);
        uint me = N.GetCurrentThreadId();
        N.AttachThreadInput(me, fg, true);
        N.AttachThreadInput(me, target, true);
        N.ShowWindow(h, 5);
        N.BringWindowToTop(h);
        N.SetForegroundWindow(h);
        N.AttachThreadInput(me, target, false);
        N.AttachThreadInput(me, fg, false);
        Thread.Sleep(200);
        if (N.GetForegroundWindow() != h) Log("the window did not come to the front");
    }

    static void Key(byte vk) {
        bool ext = vk >= 0x21 && vk <= 0x2E;
        byte scan = (byte)N.MapVirtualKey(vk, 0);
        N.keybd_event(vk, scan, ext ? 1u : 0u, UIntPtr.Zero);
        N.keybd_event(vk, scan, (ext ? 1u : 0u) | 2u, UIntPtr.Zero);
        Thread.Sleep(250);
    }

    static void Keys(byte vk, int times) { for (int i = 0; i < times; i++) Key(vk); }

    // Close the open menus, and leave the menu bar: while it is still
    // selected, Windows opens no other menu (a button's menu stays shut).
    static void CloseMenus() {
        for (int i = 0; i < 4 && Windows("#32768").Count > 0; i++) Key(VK_ESCAPE);
        if (Windows("#32768").Count > 0) Log("a menu stayed open");
        Send(main, WM_CANCELMODE, IntPtr.Zero, IntPtr.Zero);
        Thread.Sleep(300);
    }

    static IntPtr Control(int id, string cls) {
        foreach (IntPtr c in Children(main))
            if (N.GetDlgCtrlID(c) == id && (cls == null || ClassOf(c) == cls)) return c;
        return IntPtr.Zero;
    }

    // ---- The app -------------------------------------------------------------

    static void Registry(string lastTab) { Registry(lastTab, db); }

    static void Registry(string lastTab, string lastDb) {
        using (RegistryKey k = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(@"Software\BRODALF")) {
            k.SetValue("Language", lang, RegistryValueKind.String);
            k.SetValue("Appearance", theme == "dark" ? 2 : 1, RegistryValueKind.DWord);
            k.SetValue("LastTab", int.Parse(lastTab), RegistryValueKind.DWord);
            k.SetValue("LastCatalog", lastDb, RegistryValueKind.String);
            k.DeleteValue("Interrupted", false);
        }
        try { Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree(@"Software\BRODALF\IgnoredDisks"); } catch (Exception) { }
    }

    static void Start(string arg) {
        ProcessStartInfo si = new ProcessStartInfo(exe);
        if (arg != null) si.Arguments = "\"" + arg + "\"";
        si.UseShellExecute = false;
        si.WorkingDirectory = Path.GetDirectoryName(Path.GetFullPath(exe));
        proc = Process.Start(si);
        handled.Clear();
        Stopwatch sw = Stopwatch.StartNew();
        main = IntPtr.Zero;
        while (main == IntPtr.Zero && sw.ElapsedMilliseconds < 30000) {
            List<IntPtr> l = Windows("BrodalfMain");
            if (l.Count > 0) main = l[0];
            else Thread.Sleep(200);
        }
        if (main == IntPtr.Zero) throw new Exception("the BRODALF window did not open");
        Log("BRODALF is open" + (arg != null ? " with " + arg : ""));
        Thread.Sleep(800);
        Foreground(main);
    }

    static void Stop() {
        if (proc == null || proc.HasExited) return;
        Dismiss(TopDialog());
        Post(main, WM_CLOSE, 0, IntPtr.Zero);
        Stopwatch sw = Stopwatch.StartNew();
        int n = 0;
        while (!proc.HasExited && sw.ElapsedMilliseconds < 30000) {
            IntPtr d = TopDialog();
            if (d != IntPtr.Zero && n < 3) {
                Shot(d, "closing-question-" + (++n));
                Yes(d, false);
            }
            Thread.Sleep(300);
        }
        if (!proc.HasExited) { Log("BRODALF did not close; ending it"); proc.Kill(); }
        proc.WaitForExit(10000);
        proc = null;
        main = IntPtr.Zero;
    }

    static bool Busy() {
        IntPtr stop = Control(Id("ID_BTN_STOP"), "Button");
        return stop != IntPtr.Zero && N.IsWindowVisible(stop);
    }

    // Wait until no job runs and nothing asks anything, for a moment. A
    // question that comes up meanwhile is captured, then answered No or
    // Cancel.
    static void WaitIdle(string tag, int ms) {
        Stopwatch sw = Stopwatch.StartNew(), calm = Stopwatch.StartNew();
        int n = 0;
        while (sw.ElapsedMilliseconds < ms) {
            IntPtr d = TopDialog();
            if (d != IntPtr.Zero) {
                Thread.Sleep(600);
                d = TopDialog();
                if (d != IntPtr.Zero) {
                    Shot(d, tag + "-question-" + (++n));
                    Dismiss(d);
                }
                calm.Restart();
                continue;
            }
            if (Busy() || Windows("#32768").Count > 0) calm.Restart();
            if (calm.ElapsedMilliseconds > 1500) return;
            Thread.Sleep(150);
        }
        Log("still busy after " + ms + " ms (" + tag + ")");
    }

    static void Command(string id) {
        WaitIdle("before-" + Slug(id), 30000);
        Post(main, WM_COMMAND, Id(id), IntPtr.Zero);
    }

    static void Click(string id) {
        WaitIdle("before-" + Slug(id), 30000);
        IntPtr b = Control(Id(id), null);
        Post(main, WM_COMMAND, Id(id) | (0 /* BN_CLICKED */ << 16), b);
    }

    // A button pressed at once, even while a job runs (Stop).
    static void ClickNow(string id) {
        IntPtr b = Control(Id(id), null);
        Post(main, WM_COMMAND, Id(id) | (0 /* BN_CLICKED */ << 16), b);
    }

    // The right-click menu of a control, opened from the keyboard (Shift+F10 sends the same message).
    static void ContextMenu(string name, string id, string cls) {
        try {
            WaitIdle(name, 30000);
            Foreground(main);
            IntPtr c = Control(Id(id), cls);
            if (c == IntPtr.Zero) { Log("no control for " + name); return; }
            N.PostMessage(main, 0x007B /* WM_CONTEXTMENU */, c, (IntPtr)(-1));
            Stopwatch sw = Stopwatch.StartNew();
            while (Windows("#32768").Count == 0 && sw.ElapsedMilliseconds < 5000) Thread.Sleep(100);
            if (Windows("#32768").Count == 0) { Log("no right-click menu for " + name); return; }
            ShotWithMenus(name);
            CloseMenus();
            WaitIdle(name + "-after", 10000);
        } catch (Exception e) { Log(name + " failed: " + e.Message); CloseMenus(); }
    }

    // Every tab, one screenshot each.
    static readonly string[] tabNames = { "primary-database", "next-backup", "local-backups", "starred", "encryption", "restore" };
    static void AllTabs(string prefix) {
        for (int i = 0; i < 6; i++) { Tab(i); Shot(main, prefix + "tab-" + (i + 1) + "-" + tabNames[i]); }
        Tab(0);
    }

    // Start BRODALF on a database (or none), capture what it says on the way in, close what it asks.
    static void OpenAndCapture(string name, string arg, string lastDb) {
        try {
            Registry("0", lastDb);
            Start(arg);
            Stopwatch sw = Stopwatch.StartNew();
            int n = 0;
            while (sw.ElapsedMilliseconds < 25000 && n < 4) {
                IntPtr d = WaitDialog(4000, IntPtr.Zero);
                if (d == IntPtr.Zero) break;
                Shot(d, name + (n == 0 ? "" : "-then-" + (n + 1)));
                if (n == 0) ShotTogether(main, d, name + "-over-the-window");
                n++;
                Dismiss(d);
                Thread.Sleep(800);
            }
            WaitIdle(name, 30000);
            Shot(main, name + "-the-window-after");
        } catch (Exception e) { Log(name + " failed: " + e.Message); }
        Stop();
    }

    // A command that opens a dialog: capture it and what follows it (up to
    // three in a row), saying no to each.
    static void DialogStep(string name, string id, bool button) {
        try {
            if (button) Click(id); else Command(id);
            IntPtr d = WaitDialog(8000, IntPtr.Zero);
            if (d == IntPtr.Zero) { Log("no dialog came up for " + name); Shot(main, name + "-no-dialog"); return; }
            for (int i = 1; i <= 3 && d != IntPtr.Zero; i++) {
                Shot(d, i == 1 ? name : name + "-then-" + i);
                Dismiss(d);
                d = WaitDialog(1500, IntPtr.Zero);
            }
            WaitIdle(name + "-after", 20000);
        } catch (Exception e) { Log("step " + name + " failed: " + e.Message); }
    }

    static void Tab(int i) {
        WaitIdle("before-tab-" + (i + 1), 30000);
        Post(main, WM_COMMAND, Id("ID_M_TAB_FIRST") + i, IntPtr.Zero);
        Thread.Sleep(700);
    }

    // ---- The tree and lists ----------------------------------------------------

    static string TreeText(Remote rm, IntPtr tree, IntPtr item) {
        TVITEM it = new TVITEM();
        it.mask = 0x0001 | 0x0010; // TVIF_TEXT | TVIF_HANDLE
        it.hItem = item;
        it.pszText = rm.At(1024);
        it.cchTextMax = 1000;
        rm.Write(0, Remote.Bytes(it));
        Send(tree, TVM_GETITEMW, IntPtr.Zero, rm.At(0));
        return rm.ReadString(1024, 1000);
    }

    static IntPtr TreeChild(Remote rm, IntPtr tree, IntPtr parent, string starts) {
        IntPtr item = parent == IntPtr.Zero ? Send(tree, TVM_GETNEXTITEM, (IntPtr)0, IntPtr.Zero) : Send(tree, TVM_GETNEXTITEM, (IntPtr)4, parent);
        while (item != IntPtr.Zero) {
            string t = TreeText(rm, tree, item);
            if (starts == null || t.StartsWith(starts, StringComparison.OrdinalIgnoreCase)) return item;
            item = Send(tree, TVM_GETNEXTITEM, (IntPtr)1, item);
        }
        return IntPtr.Zero;
    }

    // Select backup root / folder / ... / file in the tree, by the start of each name.
    static bool TreeSelect(params string[] path) {
        IntPtr tree = Control(Id("ID_TREE"), "SysTreeView32");
        if (tree == IntPtr.Zero) { Log("no tree"); return false; }
        using (Remote rm = new Remote(proc.Id)) {
            IntPtr item = IntPtr.Zero;
            for (int i = 0; i < path.Length; i++) {
                if (item != IntPtr.Zero) { Send(tree, TVM_EXPAND, (IntPtr)2, item); Thread.Sleep(400); }
                IntPtr next = TreeChild(rm, tree, item, path[i]);
                if (next == IntPtr.Zero) { Log("not in the tree: " + string.Join("/", path, 0, i + 1)); return false; }
                item = next;
            }
            Send(tree, TVM_SELECTITEM, (IntPtr)9, item);
            Send(tree, TVM_ENSUREVISIBLE, IntPtr.Zero, item);
            Thread.Sleep(600);
            return true;
        }
    }

    // Select backup root / folder / ... and then, if the last name is a file,
    // that file in the file list beside the tree (the tree holds folders only).
    static bool Select(params string[] path) {
        IntPtr tree = Control(Id("ID_TREE"), "SysTreeView32");
        if (tree == IntPtr.Zero) { Log("no tree"); return false; }
        if (path.Length > 1) {
            using (Remote rm = new Remote(proc.Id)) {
                IntPtr item = IntPtr.Zero;
                for (int i = 0; i < path.Length - 1; i++) {
                    if (item != IntPtr.Zero) { Send(tree, TVM_EXPAND, (IntPtr)2, item); Thread.Sleep(400); }
                    item = TreeChild(rm, tree, item, path[i]);
                    if (item == IntPtr.Zero) break;
                }
                if (item != IntPtr.Zero) {
                    Send(tree, TVM_EXPAND, (IntPtr)2, item);
                    Thread.Sleep(400);
                    if (TreeChild(rm, tree, item, path[path.Length - 1]) == IntPtr.Zero) {
                        string[] folder = new string[path.Length - 1];
                        Array.Copy(path, folder, folder.Length);
                        return TreeSelect(folder) && ListSelect("ID_FILES", path[path.Length - 1]);
                    }
                }
            }
        }
        return TreeSelect(path);
    }

    static string ListText(Remote rm, IntPtr lv, int row, int col) {
        LVITEM it = new LVITEM();
        it.iSubItem = col;
        it.pszText = rm.At(1024);
        it.cchTextMax = 1000;
        rm.Write(0, Remote.Bytes(it));
        Send(lv, LVM_GETITEMTEXTW, (IntPtr)row, rm.At(0));
        return rm.ReadString(1024, 1000);
    }

    // Select the first row whose first column starts with starts (null: the first row).
    static bool ListSelect(string listId, string starts) {
        IntPtr lv = Control(Id(listId), "SysListView32");
        if (lv == IntPtr.Zero) { Log("no list " + listId); return false; }
        using (Remote rm = new Remote(proc.Id)) {
            int n = (int)Send(lv, LVM_GETITEMCOUNT, IntPtr.Zero, IntPtr.Zero);
            for (int row = 0; row < n; row++) {
                if (starts != null && !ListText(rm, lv, row, 0).StartsWith(starts, StringComparison.OrdinalIgnoreCase)) continue;
                LVITEM it = new LVITEM();
                it.stateMask = it.state = 0x0003; // LVIS_SELECTED | LVIS_FOCUSED
                rm.Write(0, Remote.Bytes(it));
                Send(lv, LVM_SETITEMSTATE, (IntPtr)row, rm.At(0));
                Send(lv, LVM_ENSUREVISIBLE, (IntPtr)row, IntPtr.Zero);
                Thread.Sleep(500);
                return true;
            }
        }
        Log("no row starting " + starts + " in " + listId);
        return false;
    }

    static bool ComboSelect(string comboId, string contains) {
        IntPtr cb = Control(Id(comboId), "ComboBox");
        if (cb == IntPtr.Zero) return false;
        int n = (int)Send(cb, CB_GETCOUNT, IntPtr.Zero, IntPtr.Zero);
        for (int i = 0; i < n; i++) {
            int len = (int)Send(cb, CB_GETLBTEXTLEN, (IntPtr)i, IntPtr.Zero);
            StringBuilder sb = new StringBuilder(Math.Max(len, 0) + 2);
            IntPtr r;
            N.SendMessageTimeoutSb(cb, CB_GETLBTEXT, (IntPtr)i, sb, 2, 2000, out r);
            if (sb.ToString().IndexOf(contains, StringComparison.OrdinalIgnoreCase) < 0) continue;
            Send(cb, CB_SETCURSEL, (IntPtr)i, IntPtr.Zero);
            Post(main, WM_COMMAND, Id(comboId) | (1 /* CBN_SELCHANGE */ << 16), cb);
            Thread.Sleep(800);
            return true;
        }
        Log("nothing with " + contains + " in " + comboId);
        return false;
    }

    static void SetText(IntPtr h, string text) {
        IntPtr r;
        N.SendMessageTimeoutStr(h, WM_SETTEXT, IntPtr.Zero, text, 2, 3000, out r);
    }

    // ---- Menus ---------------------------------------------------------------------

    // How many Down presses reach item pos of a menu (separators are skipped).
    static int DownsTo(IntPtr menu, int pos) {
        int n = 0;
        for (int i = 0; i < pos; i++) if ((N.GetMenuState(menu, (uint)i, 0x400) & 0x800) == 0) n++; // MF_BYPOSITION, MF_SEPARATOR
        return n;
    }

    static List<int> SubmenuPositions(IntPtr menu) {
        List<int> l = new List<int>();
        int n = N.GetMenuItemCount(menu);
        for (int i = 0; i < n; i++) if (N.GetSubMenu(menu, i) != IntPtr.Zero) l.Add(i);
        return l;
    }

    // Open each menu of the menu bar, and the menus inside them.
    static void MenuBar(string prefix, int only) {
        try {
            WaitIdle(prefix + "menus", 30000);
            Foreground(main);
            IntPtr bar = N.GetMenu(main);
            int count = only > 0 ? only : N.GetMenuItemCount(bar);
            Key(VK_F10);
            Key(VK_DOWN);
            for (int m = 0; m < count; m++) {
                if (m > 0) { Key(VK_RIGHT); }
                if (Windows("#32768").Count == 0) { Log("menu " + m + " did not open"); break; }
                string[] names = { "file", "view", "tools", "help" };
                string name = prefix + "menu-" + (m < names.Length ? names[m] : m.ToString());
                ShotWithMenus(name);
                IntPtr sub = N.GetSubMenu(bar, m);
                int at = 0;
                foreach (int pos in SubmenuPositions(sub)) {
                    int downs = DownsTo(sub, pos);
                    Keys(VK_DOWN, downs - at);
                    at = downs;
                    Key(VK_RIGHT);
                    ShotWithMenus(name + "-submenu-" + pos);
                    Key(VK_LEFT);
                }
                // Back to the top, so Right moves to the next menu, not into a submenu.
                if (at > 0) Keys(VK_UP, at);
            }
            CloseMenus();
        } catch (Exception e) { Log("menus failed: " + e.Message); CloseMenus(); }
    }

    // A button that opens a menu under itself.
    static void ButtonMenu(string name, string id, bool submenus) { ButtonMenu(name, id, submenus, null); }

    // firstItem: also choose the menu's first item and capture the dialog it opens.
    static void ButtonMenu(string name, string id, bool submenus, string firstItem) {
        try {
            WaitIdle(name, 30000);
            Foreground(main);
            Click(id);
            Stopwatch sw = Stopwatch.StartNew();
            while (Windows("#32768").Count == 0 && sw.ElapsedMilliseconds < 5000) Thread.Sleep(100);
            if (Windows("#32768").Count == 0) {
                IntPtr d = WaitDialog(1500, IntPtr.Zero);
                if (d != IntPtr.Zero) { Shot(d, name + "-message"); Dismiss(d); } else Log("no menu for " + name);
                return;
            }
            ShotWithMenus(name);
            if (firstItem != null) {
                Key(VK_DOWN);
                Key(VK_RETURN);
                IntPtr d = WaitDialog(8000, IntPtr.Zero);
                if (d != IntPtr.Zero) { Shot(d, firstItem); Dismiss(d); } else Log("no dialog from the first item of " + name);
                WaitIdle(name + "-after", 10000);
                return;
            }
            if (submenus) {
                IntPtr m = Send(Windows("#32768")[0], MN_GETHMENU, IntPtr.Zero, IntPtr.Zero);
                int at = -1;
                foreach (int pos in SubmenuPositions(m)) {
                    int downs = DownsTo(m, pos);
                    Keys(VK_DOWN, downs - at);
                    at = downs;
                    Key(VK_RIGHT);
                    ShotWithMenus(name + "-submenu-" + pos);
                    Key(VK_LEFT);
                }
            }
            CloseMenus();
            WaitIdle(name + "-after", 10000);
        } catch (Exception e) { Log(name + " failed: " + e.Message); CloseMenus(); }
    }

    // ---- Plugging a disk in: a virtual disk, attached and detached ---------------

    static void Diskpart(string script) {
        string path = Path.Combine(vhdDir, script);
        if (!File.Exists(path)) { Log("no " + path); return; }
        ProcessStartInfo si = new ProcessStartInfo("diskpart.exe", "/s \"" + path + "\"");
        si.UseShellExecute = false;
        si.RedirectStandardOutput = true;
        si.CreateNoWindow = true;
        using (Process p = Process.Start(si)) {
            string o = p.StandardOutput.ReadToEnd();
            p.WaitForExit(60000);
            Log("diskpart " + script + ": exit " + p.ExitCode + " " + Clean(o.Trim()).Replace("  ", " "));
        }
    }

    static bool WaitDrive(string root, bool present, int ms) {
        Stopwatch sw = Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < ms) {
            if (Directory.Exists(root) == present) return true;
            Thread.Sleep(250);
        }
        return false;
    }

    // ---- The run --------------------------------------------------------------------------

    static void Run() {
        N.SetCursorPos(N.GetSystemMetrics(0) - 2, N.GetSystemMetrics(1) / 2);
        Log(string.Format("{0} {1}, screen {2}x{3}", lang, theme, N.GetSystemMetrics(0), N.GetSystemMetrics(1)));
        if (Environment.GetEnvironmentVariable("SHOTS_ONLY_NO_LOCAL") == "1") { NoLocalData(); return; }

        // 1. Started with no database named: the question it asks, then
        //    the window with none open.
        Registry("0");
        Start(null);
        IntPtr d = WaitDialog(20000, IntPtr.Zero);
        if (d != IntPtr.Zero) {
            Shot(d, "start-which-database");
            ShotTogether(main, d, "start-which-database-over-the-window");
            ClickTask(d, IDCANCEL);
            WaitGone(d, 3000);
        } else Log("no start-up question");
        Thread.Sleep(1000);
        d = TopDialog();
        if (d != IntPtr.Zero) { Shot(d, "start-after-cancel"); Dismiss(d); }
        Shot(main, "no-database-open");
        MenuBar("no-database-", 1);
        Stop();

        // 1b. What goes wrong on the way in: the last database was moved
        //     away, a database file that is damaged.
        string here = Path.GetDirectoryName(db);
        OpenAndCapture("error-last-database-moved-away", null, Path.Combine(here, "Moved away.brodalf"));
        OpenAndCapture("error-a-damaged-database-file", Path.Combine(here, "Damaged.brodalf"), db);

        // 1c. A database with nothing in it yet: every tab empty.
        string empty = Path.Combine(here, "Empty.brodalf");
        if (File.Exists(empty)) {
            Registry("0", empty);
            Start(empty);
            WaitIdle("empty-on-opening", 60000);
            Foreground(main);
            AllTabs("empty-");
            ButtonMenu("empty-button-back-up", "ID_BTN_BACKUP", false);
            ButtonMenu("empty-button-check-drive", "ID_BTN_CHECK", false);
            ButtonMenu("empty-button-drives", "ID_BTN_DRIVES", false);
            DialogStep("empty-view-status-of-every-drive", "ID_M_STATUS_ALL", false);
            DialogStep("empty-view-files-short-of-copies", "ID_M_RISK", false);
            Stop();
        } else Log("no empty database at " + empty);

        // 2. Started with the sample database.
        Registry("0");
        Start(db);
        WaitIdle("on-opening", 90000);
        Foreground(main);
        Shot(main, "tab-1-primary-database");
        if (Select("Documents", "Taxes", "2023")) Shot(main, "tab-1-a-folder-selected");
        if (Select("Documents", "Taxes", "2023", "Tax return 2023")) Shot(main, "tab-1-a-file-selected");
        if (Select("Documents", "Letters", "Car insurance")) Shot(main, "tab-1-a-deleted-file-selected");
        if (Select("Photos", "2023", "Holiday", "IMG_0001")) Shot(main, "tab-1-a-changed-file-selected");
        IntPtr search = Control(Id("ID_SEARCH"), "Edit");
        if (search != IntPtr.Zero) {
            SetText(search, "receipt");
            Thread.Sleep(1500);
            Shot(main, "tab-1-search-receipt");
            SetText(search, "");
            Thread.Sleep(1000);
        }
        if (TreeSelect("Documents", "Letters")) ContextMenu("tab-1-right-click-a-folder", "ID_TREE", "SysTreeView32");
        if (Select("Photos", "2023", "Holiday", "IMG_0001")) ContextMenu("tab-1-right-click-a-changed-file", "ID_FILES", "SysListView32");
        Tab(0);
        string[] tabs = { "primary-database", "next-backup", "local-backups", "starred", "encryption", "restore" };
        for (int i = 1; i < 6; i++) {
            Tab(i);
            Shot(main, "tab-" + (i + 1) + "-" + tabs[i]);
            if (i == 1 && ComboSelect("ID_NEXT_FOR", "Red")) Shot(main, "tab-2-next-backup-for-the-red-drive");
            if (i == 2 && ListSelect("ID_LOCAL_LIST", "Blue")) Shot(main, "tab-3-blue-drive-selected");
            if (i == 3 && ListSelect("ID_PLACE_LIST", null)) Shot(main, "tab-4-extra-place-selected");
            if (i == 4 && ListSelect("ID_ENC_LIST", null)) Shot(main, "tab-5-encrypted-folder-selected");
        }

        // A smaller window, as on a 1366 by 768 laptop screen.
        Rectangle full = Bounds(main);
        N.SetWindowPos(main, IntPtr.Zero, 0, 0, 1280, 720, 0x0004 | 0x0010); // SWP_NOZORDER | SWP_NOACTIVATE
        for (int i = 0; i < 6; i++) { Tab(i); Shot(main, "small-window-tab-" + (i + 1) + "-" + tabs[i]); }
        N.SetWindowPos(main, IntPtr.Zero, full.Left, full.Top, full.Width, full.Height, 0x0004 | 0x0010);
        Tab(0);
        Thread.Sleep(500);

        MenuBar("", 0);
        DialogStep("button-settings", "ID_BTN_SECURITY", true);
        ButtonMenu("button-drives", "ID_BTN_DRIVES", false);
        ButtonMenu("button-drives-first", "ID_BTN_DRIVES", false, "button-drives-a-drive-s-details");
        ButtonMenu("button-back-up", "ID_BTN_BACKUP", false);
        ButtonMenu("button-check-drive", "ID_BTN_CHECK", false);

        // The dialogs, from the menus.
        DialogStep("file-new-database", "ID_M_NEW_DB", false);
        DialogStep("file-recover-the-database", "ID_M_RECOVER_DB", false);
        DialogStep("file-print-a-recovery-sheet", "ID_M_SHEET", false);
        DialogStep("view-status-of-every-drive", "ID_M_STATUS_ALL", false);
        DialogStep("view-files-short-of-copies", "ID_M_RISK", false);
        DialogStep("view-largest-files", "ID_M_LARGEST", false);
        DialogStep("view-deleted-files", "ID_M_DELETED", false);
        DialogStep("view-damaged-files", "ID_M_DAMAGED", false);
        DialogStep("view-files-in-use", "ID_M_IN_USE", false);
        DialogStep("view-files-the-restore-left-alone", "ID_M_CONFLICTS", false);
        DialogStep("tools-bit-rot-scan", "ID_M_BITROT", false);
        DialogStep("tools-add-a-backup-root", "ID_M_ADD_FOLDER", false);
        if (TreeSelect("Photos")) DialogStep("tools-change-where-a-root-is", "ID_M_RELOCATE", false);
        if (TreeSelect("Photos", "2024")) DialogStep("tools-encrypt-what-is-selected", "ID_M_ENCRYPT", false);
        if (TreeSelect("Documents", "Letters")) DialogStep("tools-restore-what-is-selected", "ID_M_RESTORE", false);
        DialogStep("file-open-a-database", "ID_M_OPEN_DB", false);
        DialogStep("tools-check-a-drive", "ID_M_CHECK", false);
        DialogStep("tools-all-disks", "ID_M_DRIVES", false);
        DialogStep("tools-restore-deleted-files", "ID_M_RESTORE_DELETED", false);
        DialogStep("help-what-the-file-list-shows", "ID_M_LEGEND", false);
        DialogStep("help-about", "ID_M_ABOUT", false);

        // The dialogs, from the buttons on the tabs.
        Tab(0);
        DialogStep("tab-1-what-to-leave-out", "ID_DB_SKIP", true);
        DialogStep("tab-1-add-a-backup-root", "ID_DB_ADD", true);
        DialogStep("tab-1-export", "ID_DB_EXPORT", true);
        Tab(1);
        ComboSelect("ID_NEXT_FOR", "Blue");
        DialogStep("tab-2-files-changed-on-it", "ID_NEXT_CHANGED", true);
        DialogStep("tab-2-short-of-copies", "ID_NEXT_RISK", true);
        DialogStep("tab-2-back-up-to-blue-the-preview", "ID_NEXT_BACKUP", true);
        Tab(2);
        if (ListSelect("ID_LOCAL_LIST", "Blue")) {
            DialogStep("tab-3-name-and-where-it-is-kept", "ID_LOCAL_EDIT", true);
            DialogStep("tab-3-compress-copies", "ID_LOCAL_COMPRESS", true);
            DialogStep("tab-3-mark-lost-or-broken", "ID_LOCAL_LOST", true);
            DialogStep("tab-3-files-changed-since", "ID_LOCAL_CHANGED", true);
        }
        DialogStep("tab-3-status-of-every-drive", "ID_LOCAL_STATUS", true);
        DialogStep("tab-3-set-up-a-drive", "ID_LOCAL_ADD", true);
        Tab(3);
        if (ListSelect("ID_PLACE_LIST", null)) {
            DialogStep("tab-4-change-its-size-limit", "ID_PLACE_LIMIT", true);
            DialogStep("tab-4-description-and-details", "ID_PLACE_EDIT", true);
            DialogStep("tab-4-mark-lost", "ID_PLACE_LOST", true);
            DialogStep("tab-4-back-up-to-it-the-preview", "ID_PLACE_BACKUP", true);
        }
        DialogStep("tab-4-add-an-extra-place", "ID_PLACE_ADD", true);
        Tab(4);
        DialogStep("tab-5-recovery-key", "ID_ENC_KEY", true);
        DialogStep("tab-5-recovery-sheet", "ID_ENC_SHEET", true);
        DialogStep("tab-5-encrypt-the-database-file", "ID_ENC_CATALOG", true);
        DialogStep("tab-5-set-up-an-encrypted-drive", "ID_ENC_DRIVE", true);
        DialogStep("tab-5-encrypt-a-folder", "ID_ENC_PICK", true);
        WrongPassphrase();
        ChangePassphrase();
        Tab(5);
        DialogStep("tab-6-restore-everything", "ID_REST_ALL", true);
        DialogStep("tab-6-restore-deleted-files", "ID_REST_DELETED", true);
        Tab(0);

        // Plugging in: the red drive (known, not plugged in so far), then a new disk.
        PlugIn();

        if (extras) {
            // Switching the appearance while the window is open.
            Command("ID_M_THEME_DARK");
            Thread.Sleep(1200);
            Shot(main, "switched-to-dark-while-open-tab-1");
            Tab(2);
            Shot(main, "switched-to-dark-while-open-tab-3");
            Command("ID_M_THEME_LIGHT");
            Thread.Sleep(1200);
            Shot(main, "switched-back-to-light-tab-3");
            Tab(0);
        }

        // With the red drive plugged in: the buttons' drive menus, and a check.
        ButtonMenu("red-plugged-in-button-back-up", "ID_BTN_BACKUP", false);
        ButtonMenu("red-plugged-in-button-check-drive", "ID_BTN_CHECK", false);
        Tab(2);
        if (ListSelect("ID_LOCAL_LIST", "Red")) {
            DialogStep("tab-3-red-quick-check", "ID_LOCAL_VERIFY", true);
            JobStep("check-the-red-drive", delegate { Click("ID_LOCAL_CHECK"); });
            Tab(2);
            Shot(main, "tab-3-after-checking-the-red-drive");
        }

        // Then a backup to every plugged-in drive, to show the window while a job runs.
        BackupAll();

        // A full check of the red drive, now that it holds more, to see one running.
        Tab(2);
        if (ListSelect("ID_LOCAL_LIST", "Red")) JobStep("full-check-of-red", delegate { Click("ID_LOCAL_CHECK"); });
        // A job stopped with the Stop button.
        Tab(2);
        if (ListSelect("ID_LOCAL_LIST", "Blue")) StoppedJob("check-of-blue-stopped");
        // The red drive pulled out while it is being checked.
        Tab(2);
        if (ListSelect("ID_LOCAL_LIST", "Red")) PulledOut("red-pulled-out-during-a-check");

        // A restore into a folder that already holds a different IMG_0001.jpg.
        Restore("restore-photos", @"C:\BrodalfRestore", "Photos");
        DialogStep("after-restore-files-left-alone", "ID_M_CONFLICTS", false);
        Restore("restore-photos-again", @"C:\BrodalfRestore", "Photos");

        // A bit-rot scan, to show a scan running.
        JobStep("bit-rot-scan", delegate { Command("ID_M_BITROT"); });
        Stop();

        // 4. A backup root that is not where it was (moved, or its drive not plugged in).
        string projects = Path.Combine(Path.GetDirectoryName(db), @"Data\Projects");
        string moved = projects + " (moved)";
        try {
            if (Directory.Exists(projects)) Directory.Move(projects, moved);
            OpenAndCapture("error-a-backup-root-is-missing", db, db);
        } catch (Exception e) { Log("missing-root step failed: " + e.Message); }
        try { if (Directory.Exists(moved)) Directory.Move(moved, projects); } catch (Exception) { }

        // 5. The database opened with none of its files here and no drive
        //    plugged in (as on a new PC): everything is faded, and each file
        //    says which drives hold it. Then the red drive is plugged in.
        NoLocalData();
    }

    static void NoLocalData() {
        string here = Path.GetDirectoryName(db);
        string[] away = { "Data", @"Drives\Blue", @"Drives\Gray", @"Cloud\OneDrive" };
        List<string> moved = new List<string>();
        try {
            foreach (string a in away) {
                string from = Path.Combine(here, a);
                if (Directory.Exists(from)) { Directory.Move(from, from + " (away)"); moved.Add(from); }
            }
            Registry("0", db);
            Start(db);
            Stopwatch sw = Stopwatch.StartNew();
            for (int n = 0; sw.ElapsedMilliseconds < 25000 && n < 4; n++) {
                IntPtr d = WaitDialog(4000, IntPtr.Zero);
                if (d == IntPtr.Zero) break;
                Shot(d, "no-local-data-on-opening" + (n == 0 ? "" : "-then-" + (n + 1)));
                Dismiss(d);
                Thread.Sleep(800);
            }
            WaitIdle("no-local-data", 60000);
            Foreground(main);
            Shot(main, "no-local-data-tab-1");
            if (Select("Documents", "Letters")) Shot(main, "no-local-data-letters");
            if (Select("Documents", "Letters", "Lease agreement")) Shot(main, "no-local-data-a-faded-file-selected");
            if (Select("Photos", "2023", "Holiday")) Shot(main, "no-local-data-holiday-photos");
            if (Select("Documents", "Letters", "Lease agreement")) ContextMenu("no-local-data-right-click-a-faded-file", "ID_FILES", "SysListView32");
            if (TreeSelect("Documents")) Shot(main, "no-local-data-a-backup-root-selected");
            Tab(2);
            Shot(main, "no-local-data-tab-3-local-backups");
            Tab(0);
            DialogStep("no-local-data-help-what-the-file-list-shows", "ID_M_LEGEND", false);

            // The red drive plugged in: what it holds is no longer faded.
            Diskpart("attach-red.txt");
            if (!WaitDrive(@"R:\", true, 20000)) Log("the red drive's letter did not appear");
            Thread.Sleep(3000);
            IntPtr q = WaitDialog(3000, IntPtr.Zero);
            if (q != IntPtr.Zero) { Shot(q, "no-local-data-red-plugged-in-question"); Dismiss(q); }
            WaitIdle("no-local-data-red", 60000);
            Tab(0);
            if (Select("Documents", "Letters")) Shot(main, "no-local-data-red-plugged-in-letters");
            if (Select("Documents", "Letters", "Lease agreement")) Shot(main, "no-local-data-red-plugged-in-a-file-selected");
            if (Select("Photos", "2023", "Holiday")) Shot(main, "no-local-data-red-plugged-in-holiday-photos");
            Diskpart("detach-red.txt");
            WaitDrive(@"R:\", false, 15000);
            Thread.Sleep(1500);
        } catch (Exception e) { Log("no-local-data step failed: " + e.Message); }
        Stop();
        foreach (string from in moved) {
            try { Directory.Move(from + " (away)", from); } catch (Exception e) { Log("could not put back " + from + ": " + e.Message); }
        }
    }

    static void WrongPassphrase() {
        try {
            Click("ID_ENC_PASS");
            IntPtr d = WaitDialog(8000, IntPtr.Zero);
            if (d == IntPtr.Zero) { Log("no passphrase dialog"); return; }
            IntPtr cur = N.GetDlgItem(d, Id("IDC_PASS_CUR"));
            if (cur != IntPtr.Zero && N.IsWindowVisible(cur)) {
                // One dialog asks for the current passphrase (wrong here) and the new one twice;
                // it says in place that the current one is not right.
                SetText(cur, "not the passphrase");
                SetText(N.GetDlgItem(d, Id("IDC_PASS_EDIT")), "a new passphrase");
                SetText(N.GetDlgItem(d, Id("IDC_PASS_EDIT2")), "a new passphrase");
                Thread.Sleep(300);
                Post(d, WM_COMMAND, IDOK, IntPtr.Zero);
                Thread.Sleep(4000);
                Shot(d, "tab-5-wrong-passphrase");
                Dismiss(d);
                WaitIdle("after-wrong-passphrase", 20000);
                return;
            }
            if (!IsPassphrase(d)) { Dismiss(d); return; }
            SetText(N.GetDlgItem(d, Id("IDC_PASS_EDIT")), "not the passphrase");
            Thread.Sleep(300);
            Post(d, WM_COMMAND, IDOK, IntPtr.Zero);
            WaitGone(d, 5000);
            IntPtr e = WaitDialog(6000, IntPtr.Zero);
            for (int i = 1; i <= 3 && e != IntPtr.Zero; i++) {
                Shot(e, "tab-5-wrong-passphrase" + (i > 1 ? "-" + i : ""));
                Dismiss(e);
                e = WaitDialog(1500, IntPtr.Zero);
            }
            WaitIdle("after-wrong-passphrase", 20000);
        } catch (Exception e) { Log("wrong passphrase step failed: " + e.Message); }
    }

    // Start a check, capture it running, press Stop, capture what it says.
    static void StoppedJob(string name) {
        try {
            WaitIdle("before-" + name, 30000);
            Foreground(main);
            Click("ID_LOCAL_CHECK");
            IntPtr d = NextDialogOrBusy(5000);
            for (int i = 1; i <= 3 && d != IntPtr.Zero; i++) {
                if (IsPassphrase(d)) { SetText(N.GetDlgItem(d, Id("IDC_PASS_EDIT")), pass); Post(d, WM_COMMAND, IDOK, IntPtr.Zero); }
                else { Shot(d, name + "-question-" + i); Yes(d, false); }
                WaitGone(d, 5000);
                d = NextDialogOrBusy(3000);
            }
            if (!Busy()) { Log(name + ": over before Stop could be pressed"); Shot(main, name + "-over-too-soon"); return; }
            Thread.Sleep(150);
            Shot(main, name + "-running");
            ClickNow("ID_BTN_STOP");
            Thread.Sleep(400);
            Shot(main, name + "-stopping");
            WaitIdle(name, 60000);
            Shot(main, name + "-after");
        } catch (Exception e) { Log(name + " failed: " + e.Message); }
    }

    // Start a check of the selected drive (the red one), unplug it under the check.
    static void PulledOut(string name) {
        try {
            WaitIdle("before-" + name, 30000);
            Foreground(main);
            Click("ID_LOCAL_CHECK");
            IntPtr d = NextDialogOrBusy(5000);
            for (int i = 1; i <= 3 && d != IntPtr.Zero; i++) {
                if (IsPassphrase(d)) { SetText(N.GetDlgItem(d, Id("IDC_PASS_EDIT")), pass); Post(d, WM_COMMAND, IDOK, IntPtr.Zero); }
                else { Yes(d, false); }
                WaitGone(d, 5000);
                d = NextDialogOrBusy(3000);
            }
            Diskpart("detach-red.txt");
            WaitDrive(@"R:\", false, 15000);
            Thread.Sleep(1500);
            Shot(main, name);
            WaitIdle(name, 120000);
            Shot(main, name + "-after");
            Tab(2);
            Shot(main, "tab-3-red-unplugged-again");
            ListSelect("ID_LOCAL_LIST", "Red");
            Shot(main, "tab-3-red-unplugged-selected");
            Tab(1);
            ComboSelect("ID_NEXT_FOR", "Red");
            Shot(main, "tab-2-next-backup-for-red-unplugged");
            Tab(0);
            Shot(main, "tab-1-with-red-unplugged");
        } catch (Exception e) { Log(name + " failed: " + e.Message); }
    }

    static void ChangePassphrase() {
        try {
            Click("ID_ENC_PASS");
            IntPtr d = WaitDialog(8000, IntPtr.Zero);
            if (d == IntPtr.Zero) { Log("no passphrase dialog"); return; }
            Shot(d, "tab-5-change-passphrase-enter-the-current-one");
            IntPtr cur = N.GetDlgItem(d, Id("IDC_PASS_CUR"));
            if (cur != IntPtr.Zero && N.GetDlgItem(d, Id("IDC_PASS_EDIT2")) != IntPtr.Zero) {
                // The ticks under the new one as it is typed, and Show.
                SetText(N.GetDlgItem(d, Id("IDC_PASS_EDIT")), "too short");
                Thread.Sleep(400);
                Shot(d, "tab-5-change-passphrase-too-short");
                SetText(N.GetDlgItem(d, Id("IDC_PASS_EDIT")), "four unrelated words");
                SetText(N.GetDlgItem(d, Id("IDC_PASS_EDIT2")), "four unrelated words");
                Thread.Sleep(400);
                Shot(d, "tab-5-change-passphrase-ready");
                IntPtr show = N.GetDlgItem(d, Id("IDC_PASS_SHOW"));
                if (show != IntPtr.Zero) {
                    Post(show, 0x00F5, 0, IntPtr.Zero); // BM_CLICK
                    Thread.Sleep(400);
                    Shot(d, "tab-5-change-passphrase-shown");
                }
                Dismiss(d);
                WaitIdle("after-passphrase", 20000);
                return;
            }
            IntPtr edit = N.GetDlgItem(d, Id("IDC_PASS_EDIT"));
            if (edit != IntPtr.Zero && N.GetDlgItem(d, Id("IDC_PASS_EDIT2")) == IntPtr.Zero) {
                SetText(edit, pass);
                Post(d, WM_COMMAND, IDOK, IntPtr.Zero);
                WaitGone(d, 5000);
                IntPtr d2 = WaitDialog(8000, IntPtr.Zero);
                if (d2 != IntPtr.Zero) { Shot(d2, "tab-5-change-passphrase-the-new-one"); Dismiss(d2); }
            } else Dismiss(d);
            WaitIdle("after-passphrase", 20000);
        } catch (Exception e) { Log("passphrase step failed: " + e.Message); }
    }

    static void PlugIn() {
        try {
            Diskpart("attach-red.txt");
            if (!WaitDrive(@"R:\", true, 20000)) Log("the red drive's letter did not appear");
            Thread.Sleep(3000);
            WaitIdle("red-drive-plugged-in", 60000);
            Shot(main, "red-drive-plugged-in");
            Tab(2);
            Shot(main, "tab-3-with-the-red-drive-plugged-in");
            if (ListSelect("ID_LOCAL_LIST", "Red")) {
                Click("ID_LOCAL_BACKUP");
                IntPtr d = WaitDialog(10000, IntPtr.Zero);
                for (int i = 1; i <= 3 && d != IntPtr.Zero; i++) {
                    // Passphrase questions are answered; the backup itself is not started.
                    IntPtr edit = N.GetDlgItem(d, Id("IDC_PASS_EDIT"));
                    if (edit != IntPtr.Zero && N.GetDlgItem(d, Id("IDC_PASS_EDIT2")) == IntPtr.Zero) {
                        Shot(d, "back-up-to-the-red-drive-passphrase");
                        SetText(edit, pass);
                        Post(d, WM_COMMAND, IDOK, IntPtr.Zero);
                        WaitGone(d, 5000);
                    } else {
                        Shot(d, "back-up-to-the-red-drive" + (i > 1 ? "-" + i : ""));
                        Dismiss(d);
                    }
                    d = WaitDialog(2500, IntPtr.Zero);
                }
                // If no question came, the backup started: let it finish.
                WaitIdle("after-red-drive", 120000);
            }
            Tab(2);
            Diskpart("attach-new.txt");
            IntPtr q = WaitDialog(30000, IntPtr.Zero);
            if (q != IntPtr.Zero) {
                Shot(q, "new-disk-plugged-in");
                ClickTask(q, 640);
                WaitGone(q, 3000);
                IntPtr label = WaitDialog(8000, IntPtr.Zero);
                if (label != IntPtr.Zero) { Shot(label, "new-disk-set-it-up"); Dismiss(label); }
            } else Log("no question when the new disk was plugged in");
            WaitIdle("after-new-disk", 30000);
            Diskpart("detach-new.txt");
            Thread.Sleep(2000);
            WaitIdle("new-disk-unplugged", 30000);
            Tab(0);
        } catch (Exception e) { Log("plug-in step failed: " + e.Message); }
    }

    // After a question is answered: the next question, or none once the job runs.
    static IntPtr NextDialogOrBusy(int ms) {
        Stopwatch sw = Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < ms) {
            IntPtr d = TopDialog();
            if (d != IntPtr.Zero) { Thread.Sleep(600); d = TopDialog(); if (d != IntPtr.Zero) return d; }
            if (Busy()) return IntPtr.Zero;
            Thread.Sleep(50);
        }
        return IntPtr.Zero;
    }

    static bool IsPassphrase(IntPtr d) {
        return N.GetDlgItem(d, Id("IDC_PASS_EDIT")) != IntPtr.Zero && N.GetDlgItem(d, Id("IDC_PASS_EDIT2")) == IntPtr.Zero;
    }

    // Start a job and let it run: a passphrase question is answered, a
    // yes/no question is answered yes (go ahead), anything else is
    // captured and cancelled. Captures the window while the job runs.
    static void JobStep(string name, Action start) {
        try {
            WaitIdle("before-" + name, 30000);
            Foreground(main);
            start();
            IntPtr d = WaitDialog(4000, IntPtr.Zero);
            for (int i = 1; i <= 4 && d != IntPtr.Zero; i++) {
                if (IsPassphrase(d)) {
                    Shot(d, name + "-passphrase");
                    SetText(N.GetDlgItem(d, Id("IDC_PASS_EDIT")), pass);
                    Post(d, WM_COMMAND, IDOK, IntPtr.Zero);
                } else {
                    Shot(d, name + "-question-" + i);
                    Yes(d, false);
                }
                WaitGone(d, 5000);
                d = NextDialogOrBusy(3000);
            }
            Running(name);
            WaitIdle(name + "-finished", 240000);
            Shot(main, name + "-finished");
        } catch (Exception e) { Log(name + " failed: " + e.Message); }
    }

    static void Running(string name) {
        Stopwatch sw = Stopwatch.StartNew();
        int shots = 0;
        long[] at = { 150, 1200, 3500 };
        while (sw.ElapsedMilliseconds < 60000 && shots < at.Length) {
            if (TopDialog() != IntPtr.Zero) break;
            if (!Busy() && sw.ElapsedMilliseconds > 1500) break;
            if (sw.ElapsedMilliseconds >= at[shots] && Busy()) Shot(main, name + "-running-" + (++shots));
            Thread.Sleep(50);
        }
        if (shots == 0) Log(name + ": over before it could be captured running");
    }

    // A restore: the folder picker is pointed at dest (it has no box to type in).
    static void Restore(string name, string dest, params string[] treePath) {
        try {
            Tab(0);
            if (!TreeSelect(treePath)) return;
            Tab(5);
            Click("ID_REST_SEL");
            IntPtr d = WaitDialog(8000, IntPtr.Zero);
            for (int i = 1; i <= 4 && d != IntPtr.Zero; i++) {
                bool picker = false;
                foreach (IntPtr c in Children(d)) if (ClassOf(c) == "SysTreeView32") picker = true;
                if (IsPassphrase(d)) {
                    Shot(d, name + "-passphrase");
                    SetText(N.GetDlgItem(d, Id("IDC_PASS_EDIT")), pass);
                    Post(d, WM_COMMAND, IDOK, IntPtr.Zero);
                } else if (picker) {
                    using (Remote rm = new Remote(proc.Id)) {
                        rm.Write(0, Encoding.Unicode.GetBytes(dest + "\0"));
                        Send(d, BFFM_SETSELECTIONW, (IntPtr)1, rm.At(0));
                    }
                    Thread.Sleep(800);
                    Shot(d, name + "-where-to");
                    Post(d, WM_COMMAND, IDOK, IntPtr.Zero);
                } else {
                    Shot(d, name + "-question-" + i);
                    Yes(d, true);
                }
                WaitGone(d, 5000);
                d = NextDialogOrBusy(3000);
            }
            Running(name);
            WaitIdle(name + "-finished", 240000);
            Shot(main, name + "-finished-restore-tab");
        } catch (Exception e) { Log(name + " failed: " + e.Message); }
    }

    static void BackupAll() {
        try {
            Tab(0);
            Command("ID_M_BACKUP_ALL");
            IntPtr d = WaitDialog(10000, IntPtr.Zero);
            for (int i = 1; i <= 4 && d != IntPtr.Zero; i++) {
                IntPtr edit = N.GetDlgItem(d, Id("IDC_PASS_EDIT"));
                if (edit != IntPtr.Zero && N.GetDlgItem(d, Id("IDC_PASS_EDIT2")) == IntPtr.Zero) {
                    Shot(d, "back-up-to-every-drive-passphrase");
                    SetText(edit, pass);
                    Post(d, WM_COMMAND, IDOK, IntPtr.Zero);
                } else if (IsTaskDialog(d)) {
                    Shot(d, "back-up-to-every-drive-question-" + i);
                    ClickTask(d, 620); // Back up anyway
                } else {
                    Shot(d, "back-up-to-every-drive-question-" + i);
                    Yes(d, true);
                }
                WaitGone(d, 5000);
                d = NextDialogOrBusy(3000);
            }
            Stopwatch sw = Stopwatch.StartNew();
            int shots = 0;
            long[] at = { 800, 3000, 7000 };
            while (sw.ElapsedMilliseconds < 120000 && shots < at.Length) {
                if (TopDialog() != IntPtr.Zero) break;
                if (!Busy() && sw.ElapsedMilliseconds > 2000) break;
                if (sw.ElapsedMilliseconds >= at[shots] && Busy()) Shot(main, "backup-running-" + (++shots));
                Thread.Sleep(100);
            }
            WaitIdle("backup-finished", 240000);
            Shot(main, "backup-finished");
            Tab(1);
            Shot(main, "backup-finished-tab-2-next-backup");
            Tab(2);
            Shot(main, "backup-finished-tab-3-local-backups");
        } catch (Exception e) { Log("backup step failed: " + e.Message); }
    }
}
