// Kraven mod installer for Spider-Man: The Movie (2002, Windows).
//
// Adds the Xbox-only Bronx Zoo levels (Kraven's Test, The Mighty Hunter) as
// new levels. Built by tools/installer/build_installer.py, which embeds the
// mod files as resources named "payload/<path relative to the game folder>".
//
// What it changes, each step backed up first into <game>\KravenMod\backup and
// listed in <game>\KravenMod\install.log (uninstall undoes exactly that list):
//   1. copies the new files (levels, movies, tips, voice lines, music);
//   2. data\COMMON.pkg: in SOUNDS\GLOBAL.SND switches on the five zoo
//      cut-scene dialogue lines (";audio\levels\m7bronxzoo\zoo_* ..." ->
//      "audio\levels\m7bronxzoo\zoo_* ... bink"). The new text is appended to
//      the package's data and the file record pointed at it; nothing else in
//      the package moves. Works on whichever COMMON.pkg is installed;
//   3. SpiderMan.exe: the zero-time-step physics fix (see pc_nan_guard.py).
//      The code is found by its bytes, not by address, so another build of
//      the exe with the same code (patch 1.3, community fixes) gets it too;
//      if the code is not found the exe is left alone and this is reported;
//   4. game.ini [Flags] BONUS_LEVELS_AVAILABLE=1 (the level-select gate), in
//      the game folder and in its VirtualStore copy, if present.
// It does not touch the community fix files (d3d8.dll, dxwrapper, Xidi...).
//
// Usage: KravenModInstaller.exe [install|uninstall] ["game folder"]
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;

static class KravenModInstaller
{
    const string ModDir = "KravenMod";
    static string game, backup, logPath;
    static List<string> log = new List<string>();

    static int Main(string[] args)
    {
        Console.Title = "Kraven mod installer - Spider-Man (2002)";
        Console.WriteLine("Kraven mod for Spider-Man: The Movie (2002, Windows)");
        Console.WriteLine("Adds the Bronx Zoo levels: Kraven's Test, The Mighty Hunter.");
        Console.WriteLine();
        string action = null;
        foreach (string a in args)
        {
            string l = a.ToLowerInvariant().TrimStart('-', '/');
            if (l == "install" || l == "uninstall") action = l;
            else if (Directory.Exists(a)) game = Path.GetFullPath(a);
        }
        try
        {
            if (game == null) game = FindGame();
            if (game == null)
            {
                Console.Write("Game folder not found. Type the folder that holds SpiderMan.exe: ");
                string t = (Console.ReadLine() ?? "").Trim().Trim('"');
                if (File.Exists(Path.Combine(t, "SpiderMan.exe"))) game = Path.GetFullPath(t);
            }
            if (game == null) return Fail("No game folder with SpiderMan.exe.");
            Console.WriteLine("Game folder: " + game);
            backup = Path.Combine(game, ModDir, "backup");
            logPath = Path.Combine(game, ModDir, "install.log");
            bool installed = File.Exists(logPath);
            Console.WriteLine(installed ? "The Kraven mod is installed here." : "The Kraven mod is not installed here.");
            if (action == null)
            {
                Console.WriteLine();
                Console.WriteLine(installed ? "  [R] reinstall   [U] uninstall   [Q] quit" : "  [I] install   [Q] quit");
                Console.Write("> ");
                string k = (Console.ReadLine() ?? "").Trim().ToLowerInvariant();
                if (k == "i" || k == "r") action = "install";
                else if (k == "u") action = "uninstall";
                else return 0;
            }
            if (GameRunning())
                return Fail("Spider-Man is running from this folder. Close the game first.");
            if (action == "uninstall") Uninstall();
            else
            {
                if (installed) Uninstall();
                Install();
            }
        }
        catch (Exception e)
        {
            return Fail(e.Message);
        }
        Pause();
        return 0;
    }

    static bool GameRunning()
    {
        string exe = Path.Combine(game, "SpiderMan.exe");
        foreach (Process p in Process.GetProcessesByName("SpiderMan"))
        {
            try
            {
                if (string.Equals(Path.GetFullPath(p.MainModule.FileName), exe, StringComparison.OrdinalIgnoreCase)) return true;
            }
            catch (Exception)
            {
                return true;      // can't tell where it runs from: be safe
            }
        }
        return false;
    }

    static int Fail(string msg)
    {
        Console.WriteLine();
        Console.WriteLine("ERROR: " + msg);
        Pause();
        return 1;
    }

    static void Pause()
    {
        if (Environment.UserInteractive && !Console.IsInputRedirected)
        {
            Console.WriteLine();
            Console.WriteLine("Press Enter to close.");
            Console.ReadLine();
        }
    }

    static string FindGame()
    {
        var c = new List<string>();
        string pf86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
        string pf = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        string here = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
        c.Add(here);
        c.Add(Path.Combine(pf86, @"Activision\Spider-Man (tm) Movie"));
        c.Add(Path.Combine(pf, @"Activision\Spider-Man (tm) Movie"));
        foreach (string root in new[] { @"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall", @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall" })
        {
            using (var k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(root))
            {
                if (k == null) continue;
                foreach (string s in k.GetSubKeyNames())
                    using (var e = k.OpenSubKey(s))
                    {
                        string name = e == null ? null : e.GetValue("DisplayName") as string;
                        string loc = e == null ? null : e.GetValue("InstallLocation") as string;
                        if (name != null && loc != null && name.IndexOf("Spider-Man", StringComparison.OrdinalIgnoreCase) >= 0) c.Add(loc);
                    }
            }
        }
        foreach (string d in c)
            if (!string.IsNullOrEmpty(d) && File.Exists(Path.Combine(d, "SpiderMan.exe")) && File.Exists(Path.Combine(d, @"data\COMMON.pkg")))
                return Path.GetFullPath(d);
        return null;
    }

    // ---------------------------------------------------------------- install

    static void Install()
    {
        Console.WriteLine();
        Console.WriteLine("Installing...");
        CheckGame();
        Directory.CreateDirectory(backup);
        log.Clear();
        try
        {
            CopyPayload();
            PatchCommon();
            PatchExe();
            EnableLevels();
        }
        finally
        {
            File.WriteAllLines(logPath, log.ToArray());
        }
        Console.WriteLine();
        Console.WriteLine("Done. Start the game; the zoo levels are in the level select");
        Console.WriteLine("(Kraven's Test / The Mighty Hunter) and follow Escape From Oscorp.");
    }

    static void CheckGame()
    {
        byte[] exe = File.ReadAllBytes(Path.Combine(game, "SpiderMan.exe"));
        if (IndexOf(exe, Encoding.ASCII.GetBytes("bronxzoo_b"), 0) < 0 && IndexOf(exe, Encoding.ASCII.GetBytes("BRONXZOO_B"), 0) < 0
            && IndexOf(exe, Encoding.ASCII.GetBytes("bronxzoo\\bronxzoo_b"), 0) < 0)
        {
            // the level table spells it M7bronxzoo\bronxzoo_b
            if (IndexOfI(exe, "bronxzoo") < 0)
                throw new Exception("This SpiderMan.exe has no Bronx Zoo entry in its level table; it is not a build this mod supports.");
        }
    }

    static IEnumerable<string> PayloadNames()
    {
        foreach (string n in Assembly.GetExecutingAssembly().GetManifestResourceNames())
            if (n.StartsWith("payload/")) yield return n;
    }

    static void CopyPayload()
    {
        int added = 0, replaced = 0;
        foreach (string res in PayloadNames())
        {
            string rel = res.Substring("payload/".Length).Replace('/', '\\');
            string dst = Path.Combine(game, rel);
            Directory.CreateDirectory(Path.GetDirectoryName(dst));
            if (File.Exists(dst))
            {
                Backup(rel);
                log.Add("REPLACE\t" + rel);
                replaced++;
            }
            else
            {
                log.Add("ADD\t" + rel);
                added++;
            }
            using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream(res))
            using (FileStream f = File.Create(dst))
                s.CopyTo(f);
        }
        Console.WriteLine(string.Format("  files: {0} added, {1} replaced (originals backed up)", added, replaced));
    }

    static void Backup(string rel)
    {
        string b = Path.Combine(backup, rel);
        Directory.CreateDirectory(Path.GetDirectoryName(b));
        if (!File.Exists(b)) File.Copy(Path.Combine(game, rel), b);
    }

    // data\COMMON.pkg: u32 dirs, files, data_size; dirs { name[0x40], u32 n,
    // n x u32 index }; files { name[0x28], u32 size, u32 offset }; data = the
    // last data_size bytes, offsets from its start.
    static readonly Regex ZooLine = new Regex(@"^;?(audio\\levels\\m7bronxzoo\\zoo_[bc]_\w+[ \t]+voice[ \t]+cd[ \t]+volume[ \t]+\d+)[ \t]*(bink)?[ \t]*(?=\r?$)",
                                             RegexOptions.IgnoreCase | RegexOptions.Multiline);

    static void PatchCommon()
    {
        string rel = @"data\COMMON.pkg";
        string path = Path.Combine(game, rel);
        byte[] d = File.ReadAllBytes(path);
        uint ndir = BitConverter.ToUInt32(d, 0), nfile = BitConverter.ToUInt32(d, 4), dsize = BitConverter.ToUInt32(d, 8);
        int at = 12;
        var sounds = new HashSet<uint>();
        for (int i = 0; i < ndir; i++)
        {
            string dn = CStr(d, at, 0x40);
            uint n = BitConverter.ToUInt32(d, at + 0x40);
            at += 0x44;
            for (int k = 0; k < n; k++)
                if (dn.Equals(@"SOUNDS\", StringComparison.OrdinalIgnoreCase)) sounds.Add(BitConverter.ToUInt32(d, at + 4 * k));
            at += 4 * (int)n;
        }
        int recs = at;
        long dataBase = d.LongLength - dsize;
        if (dataBase != recs + 0x30L * nfile) throw new Exception("COMMON.pkg: unexpected layout.");
        int rec = -1;
        for (int i = 0; i < nfile; i++)
            if (sounds.Contains((uint)i) && CStr(d, recs + 0x30 * i, 0x28).Equals("GLOBAL.SND", StringComparison.OrdinalIgnoreCase)) rec = recs + 0x30 * i;
        if (rec < 0) throw new Exception(@"COMMON.pkg has no SOUNDS\GLOBAL.SND.");
        uint size = BitConverter.ToUInt32(d, rec + 0x28), off = BitConverter.ToUInt32(d, rec + 0x2C);
        string text = Encoding.GetEncoding(1252).GetString(d, (int)(dataBase + off), (int)size);
        if (text.IndexOf(@"characters\kraven", StringComparison.OrdinalIgnoreCase) < 0)
            Console.WriteLine(@"  warning: GLOBAL.SND declares no Kraven voice lines; Kraven may be silent.");
        int changed = 0;
        string nt = ZooLine.Replace(text, delegate (Match m)
        {
            string want = m.Groups[1].Value + " bink";
            if (m.Value == want) return m.Value;
            changed++;
            return want;
        });
        if (changed == 0)
        {
            Console.WriteLine("  COMMON.pkg: zoo dialogue already switched on");
            return;
        }
        Backup(rel);
        byte[] add = Encoding.GetEncoding(1252).GetBytes(nt);
        int pad = (int)((4 - dsize % 4) % 4);
        uint newOff = dsize + (uint)pad;
        using (var f = new FileStream(path, FileMode.Open, FileAccess.ReadWrite))
        {
            f.Seek(0, SeekOrigin.End);
            f.Write(new byte[pad], 0, pad);
            f.Write(add, 0, add.Length);
            // every record that pointed at the old text (aliases) moves too
            for (int i = 0; i < nfile; i++)
            {
                int r = recs + 0x30 * i;
                if (BitConverter.ToUInt32(d, r + 0x28) == size && BitConverter.ToUInt32(d, r + 0x2C) == off)
                {
                    f.Seek(r + 0x28, SeekOrigin.Begin);
                    f.Write(BitConverter.GetBytes((uint)add.Length), 0, 4);
                    f.Write(BitConverter.GetBytes(newOff), 0, 4);
                }
            }
            f.Seek(8, SeekOrigin.Begin);
            f.Write(BitConverter.GetBytes(newOff + (uint)add.Length), 0, 4);
        }
        log.Add("PATCH\t" + rel);
        Console.WriteLine(string.Format("  COMMON.pkg: {0} zoo dialogue lines switched on", changed));
    }

    // SpiderMan.exe: FUN_004C5060 (the per-entity physics step) rebuilds the
    // velocity as displacement * (1/dt); with dt = 0 that is NaN, which in the
    // zoo left Spider-Man's height NaN (every trap fired, the level ended at
    // once, Kraven's top half vanished). The fld 1.0 before the fdiv becomes a
    // jump to a cave in int3 padding: if dt is +-0 skip to just after
    // "velocity = ...", else do the fld and go on.
    static readonly int[] Sig = Hex(
        "d9 05 ?? ?? ?? ?? d8 b4 24 14 01 00 00 83 ec 0c 8d 4c 24 2c d9 44 24 24 d8 c9 d9 5c 24 08 d9 44 24 20 d8 c9" +
        " d9 5c 24 04 d8 4c 24 1c d9 1c 24 e8 ?? ?? ?? ?? d9 44 24 34 d8 44 24 28 83 ec 0c 8d 4c 24 38 d9 5c 24 08" +
        " d9 44 24 3c d8 44 24 30 d9 5c 24 04 d9 44 24 38 d8 44 24 2c d9 1c 24 e8 ?? ?? ?? ?? 8d 44 24 2c 8b cd 50" +
        " e8 ?? ?? ?? ??");
    static readonly byte[] CaveHead = { 0x8b, 0x84, 0x24, 0x14, 0x01, 0x00, 0x00, 0xa9, 0xff, 0xff, 0xff, 0x7f, 0x0f, 0x84 };

    static void PatchExe()
    {
        string rel = "SpiderMan.exe";
        string path = Path.Combine(game, rel);
        byte[] d = File.ReadAllBytes(path);
        int pe = BitConverter.ToInt32(d, 0x3C);
        uint imageBase = BitConverter.ToUInt32(d, pe + 0x34);
        int nsec = BitConverter.ToUInt16(d, pe + 6), optSize = BitConverter.ToUInt16(d, pe + 20);
        int textOff = 0, textLen = 0;
        uint textVa = 0;
        for (int i = 0; i < nsec; i++)
        {
            int s = pe + 24 + optSize + 40 * i;
            uint chr = BitConverter.ToUInt32(d, s + 36);
            if ((chr & 0x20000000) != 0 && textLen == 0)   // first executable section
            {
                textVa = BitConverter.ToUInt32(d, s + 12);
                textLen = BitConverter.ToInt32(d, s + 16);
                textOff = BitConverter.ToInt32(d, s + 20);
            }
        }
        Func<int, uint> va = o => imageBase + textVa + (uint)(o - textOff);
        // already patched? (a jmp whose target starts with the cave code)
        var hits = new List<int>();
        for (int o = textOff; o + Sig.Length <= textOff + textLen; o++)
            if (Match(d, o, Sig)) hits.Add(o);
        if (hits.Count == 0)
        {
            for (int o = textOff; o + 6 <= textOff + textLen; o++)
            {
                if (d[o] != 0xE9 || d[o + 5] != 0x90) continue;
                long t = o + 5 + BitConverter.ToInt32(d, o + 1);
                if (t > 0 && t + CaveHead.Length < d.Length && Match(d, (int)t, CaveHead) && Match(d, o + 6, Tail(Sig, 6)))
                {
                    Console.WriteLine("  SpiderMan.exe: physics fix already present");
                    return;
                }
            }
            Console.WriteLine("  SpiderMan.exe: physics code not recognised in this build; left unchanged.");
            Console.WriteLine("    (Without it Spider-Man can glitch at the zoo's start: traps firing, instant mission complete.)");
            return;
        }
        if (hits.Count > 1) { Console.WriteLine("  SpiderMan.exe: physics code found more than once; left unchanged."); return; }
        int hook = hits[0];
        int resume = hook + 6, skip = hook + Sig.Length;
        byte[] orig = new byte[6];
        Array.Copy(d, hook, orig, 0, 6);
        // cave: 64 bytes of int3 padding, 16-aligned, after a 16-byte guard
        int cave = -1;
        for (int o = textOff; o + 96 <= textOff + textLen && cave < 0; o++)
        {
            if (d[o] != 0xCC) continue;
            int run = 0;
            while (o + run < textOff + textLen && d[o + run] == 0xCC) run++;
            int start = (o + 16 + 15) & ~15;
            if (start + 48 <= o + run) cave = start;
            o += run;
        }
        if (cave < 0) { Console.WriteLine("  SpiderMan.exe: no free padding for the physics fix; left unchanged."); return; }
        var c = new List<byte>(CaveHead);
        int jz = c.Count; c.AddRange(new byte[4]);
        c.AddRange(orig);
        int jmp = c.Count; c.Add(0xE9); c.AddRange(new byte[4]);
        byte[] cb = c.ToArray();
        Array.Copy(BitConverter.GetBytes(skip - (cave + jz + 4)), 0, cb, jz, 4);
        Array.Copy(BitConverter.GetBytes(resume - (cave + jmp + 5)), 0, cb, jmp + 1, 4);
        Backup(rel);
        Array.Copy(cb, 0, d, cave, cb.Length);
        d[hook] = 0xE9;
        Array.Copy(BitConverter.GetBytes(cave - (hook + 5)), 0, d, hook + 1, 4);
        d[hook + 5] = 0x90;
        File.WriteAllBytes(path, d);
        log.Add("PATCH\t" + rel);
        Console.WriteLine(string.Format("  SpiderMan.exe: physics fix applied at {0:X} (cave {1:X})", va(hook), va(cave)));
    }

    static void EnableLevels()
    {
        string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        string vs = Path.Combine(local, "VirtualStore", game.Substring(Path.GetPathRoot(game).Length));
        foreach (string dir in new[] { game, vs })
        {
            string p = Path.Combine(dir, "game.ini");
            if (!File.Exists(p)) continue;
            string t = File.ReadAllText(p, Encoding.GetEncoding(1252));
            Match sec = Regex.Match(t, @"(?im)^\[Flags\][ \t]*\r?$");
            if (!sec.Success) continue;
            int end = t.Length;
            Match next = Regex.Match(t.Substring(sec.Index + sec.Length), @"(?m)^\[");
            if (next.Success) end = sec.Index + sec.Length + next.Index;
            string body = t.Substring(sec.Index + sec.Length, end - sec.Index - sec.Length);
            Regex flag = new Regex(@"(?im)^[ \t]*BONUS_LEVELS_AVAILABLE[ \t]*=[^\r\n]*");
            string nb = flag.IsMatch(body) ? flag.Replace(body, "BONUS_LEVELS_AVAILABLE=1")
                                           : (t.Contains("\r\n") ? "\r\n" : "\n") + "BONUS_LEVELS_AVAILABLE=1" + body;
            string nt = t.Substring(0, sec.Index + sec.Length) + nb + t.Substring(end);
            if (nt == t) continue;
            string rel = dir == game ? "game.ini" : null;
            if (rel != null) { Backup(rel); log.Add("PATCH\tgame.ini"); }
            else
            {
                string b = Path.Combine(backup, "VirtualStore-game.ini");
                if (!File.Exists(b)) File.Copy(p, b);
                log.Add("VSPATCH\t" + p);
            }
            File.WriteAllText(p, nt, Encoding.GetEncoding(1252));
            Console.WriteLine("  game.ini: level-select gate opened (" + (rel != null ? "game folder" : "VirtualStore") + ")");
        }
    }

    // -------------------------------------------------------------- uninstall

    static void Uninstall()
    {
        Console.WriteLine();
        Console.WriteLine("Removing the Kraven mod...");
        if (!File.Exists(logPath)) { Console.WriteLine("  nothing to remove"); return; }
        int n = 0;
        foreach (string line in File.ReadAllLines(logPath))
        {
            string[] p = line.Split('\t');
            if (p.Length != 2) continue;
            if (p[0] == "ADD")
            {
                string f = Path.Combine(game, p[1]);
                if (File.Exists(f)) { File.Delete(f); n++; }
            }
            else if (p[0] == "REPLACE" || p[0] == "PATCH")
            {
                string b = Path.Combine(backup, p[1]);
                if (File.Exists(b)) { File.Copy(b, Path.Combine(game, p[1]), true); n++; }
            }
            else if (p[0] == "VSPATCH")
            {
                string b = Path.Combine(backup, "VirtualStore-game.ini");
                if (File.Exists(b)) { File.Copy(b, p[1], true); n++; }
            }
        }
        Directory.Delete(Path.Combine(game, ModDir), true);
        Console.WriteLine(string.Format("  {0} files removed or restored", n));
    }

    // ---------------------------------------------------------------- helpers

    static string CStr(byte[] d, int at, int len)
    {
        int e = at;
        while (e < at + len && d[e] != 0) e++;
        return Encoding.GetEncoding(1252).GetString(d, at, e - at);
    }

    static int[] Hex(string s)
    {
        var r = new List<int>();
        foreach (string t in s.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries))
            r.Add(t == "??" ? -1 : Convert.ToInt32(t, 16));
        return r.ToArray();
    }

    static int[] Tail(int[] a, int from)
    {
        int[] r = new int[a.Length - from];
        Array.Copy(a, from, r, 0, r.Length);
        return r;
    }

    static bool Match(byte[] d, int at, int[] sig)
    {
        if (at < 0 || at + sig.Length > d.Length) return false;
        for (int i = 0; i < sig.Length; i++)
            if (sig[i] >= 0 && d[at + i] != sig[i]) return false;
        return true;
    }

    static bool Match(byte[] d, int at, byte[] sig)
    {
        if (at < 0 || at + sig.Length > d.Length) return false;
        for (int i = 0; i < sig.Length; i++)
            if (d[at + i] != sig[i]) return false;
        return true;
    }

    static int IndexOf(byte[] d, byte[] s, int from)
    {
        for (int i = from; i + s.Length <= d.Length; i++)
            if (Match(d, i, s)) return i;
        return -1;
    }

    static int IndexOfI(byte[] d, string s)
    {
        byte[] lo = Encoding.ASCII.GetBytes(s.ToLowerInvariant()), up = Encoding.ASCII.GetBytes(s.ToUpperInvariant());
        for (int i = 0; i + lo.Length <= d.Length; i++)
        {
            int k = 0;
            while (k < lo.Length && (d[i + k] == lo[k] || d[i + k] == up[k])) k++;
            if (k == lo.Length) return i;
        }
        return -1;
    }
}
