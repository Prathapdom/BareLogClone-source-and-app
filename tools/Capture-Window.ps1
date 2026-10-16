# Captures a BareLog window to PNG (works for hidden-behind/background windows via PrintWindow).
# Usage: tools\Capture-Window.ps1 -Out shot.png [-Class BareLog.Dialog] [-Send <cmdId>] [-Post <cmdId>] [-W 800 -H 600]
# -Send runs a WM_COMMAND synchronously; use -Post for commands that open modal dialogs (e.g. 105 = Highlighting).
param([string]$Out, [string]$Class = "BareLog.Main", [int]$Post = 0, [int]$Send = 0, [int]$W = 0, [int]$H = 0)
Add-Type -AssemblyName System.Drawing
if (-not ([System.Management.Automation.PSTypeName]'W2').Type) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W2 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int hh, uint f);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  public struct RECT { public int L, T, R, B; }
  public static IntPtr Find(string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { var sb = new System.Text.StringBuilder(64); GetClassNameW(h, sb, 64); if (sb.ToString() == cls && IsWindowVisible(h)) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
"@
}
$main = [W2]::Find("BareLog.Main")
if ($W) { [void][W2]::SetWindowPos($main, [IntPtr]::Zero, 0, 0, $W, $H, 0x16) }
if ($Send) { [void][W2]::SendMessageW($main, 0x111, [IntPtr]$Send, [IntPtr]::Zero) }
if ($Post) { [void][W2]::PostMessageW($main, 0x111, [IntPtr]$Post, [IntPtr]::Zero) }
Start-Sleep -Milliseconds 800
$h = [W2]::Find($Class)
if ($h -eq [IntPtr]::Zero) { throw "no $Class" }
$r = New-Object W2+RECT; [void][W2]::GetWindowRect($h, [ref]$r)
$bmp = New-Object Drawing.Bitmap(($r.R - $r.L), ($r.B - $r.T))
$g = [Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
[void][W2]::PrintWindow($h, $dc, 2)
$g.ReleaseHdc($dc); $g.Dispose()
$bmp.Save($Out, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
