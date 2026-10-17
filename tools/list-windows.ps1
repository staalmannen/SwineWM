# Lists every visible top-level window, top of the z-order first, with what swinewm needs to know about
# it: process, class, title, rectangle, styles, owner and DWM cloaking.
# For bug reports about windows swinewm handles badly (dialogs off screen,
# popups that get tiled, windows that are not managed).
#
#   tools\list-windows.bat [seconds]
#
# waits the given number of seconds (default 5) so that the window in
# question can be opened first, then prints the list and saves it to
# %TEMP%\swinewm-windows.txt.
param([int]$Delay = 5)

Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class SwineWindows {
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint c);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern IntPtr GetDesktopWindow();
  // Undocumented but exported since Windows 8: the z-order band (1 = desktop).
  [DllImport("user32.dll")] public static extern bool GetWindowBand(IntPtr h, out uint band);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out int v, int s);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT v, int s);
  public struct RECT { public int L, T, R, B; }
}
"@

# Physical pixels, as swinewm sees them (per-monitor DPI aware v2).
[void][SwineWindows]::SetProcessDpiAwarenessContext([IntPtr](-4))

for ($i = $Delay; $i -gt 0; $i--) {
  Write-Host "Listing windows in $i s - open the window now..."
  Start-Sleep 1
}

$lines = New-Object System.Collections.Generic.List[string]
# EnumWindows only returns windows of desktop apps (see its documentation);
# walking the desktop's children with GetWindow also finds the "immersive"
# windows of modern Windows dialogs, flyouts and Store apps.
$h = [SwineWindows]::GetWindow([SwineWindows]::GetDesktopWindow(), 5)   # GW_CHILD
while ($h -ne [IntPtr]::Zero) {
  if ([SwineWindows]::IsWindowVisible($h)) {
    $title = New-Object Text.StringBuilder 256
    [void][SwineWindows]::GetWindowText($h, $title, 256)
    $class = New-Object Text.StringBuilder 256
    [void][SwineWindows]::GetClassName($h, $class, 256)
    $r = New-Object SwineWindows+RECT
    [void][SwineWindows]::GetWindowRect($h, [ref]$r)
    $f = New-Object SwineWindows+RECT
    # DWMWA_EXTENDED_FRAME_BOUNDS (9): the visible frame
    [void][SwineWindows]::DwmGetWindowAttribute($h, 9, [ref]$f, 16)
    $cloaked = 0
    # DWMWA_CLOAKED (14)
    [void][SwineWindows]::DwmGetWindowAttribute($h, 14, [ref]$cloaked, 4)
    $band = [uint32]0
    try { [void][SwineWindows]::GetWindowBand($h, [ref]$band) } catch { }
    $procId = [uint32]0
    [void][SwineWindows]::GetWindowThreadProcessId($h, [ref]$procId)
    $proc = (Get-Process -Id $procId -ErrorAction SilentlyContinue).ProcessName
    $style = [SwineWindows]::GetWindowLong($h, -16)
    $exstyle = [SwineWindows]::GetWindowLong($h, -20)
    $owner = [SwineWindows]::GetWindow($h, 4)   # GW_OWNER
    $flags = @()
    if ([SwineWindows]::IsIconic($h)) { $flags += "minimized" }
    if ($cloaked -ne 0) { $flags += "cloaked=$cloaked" }
    if ($band -gt 1) { $flags += "band=$band" }
    if (($style -band 0x00C00000) -eq 0x00C00000) { $flags += "caption" }
    if ($style -band 0x40000000) { $flags += "child" }
    if ($exstyle -band 0x00000080) { $flags += "toolwindow" }
    if ($exstyle -band 0x00040000) { $flags += "appwindow" }
    if ($exstyle -band 0x08000000) { $flags += "noactivate" }
    if ($exstyle -band 0x00000020) { $flags += "transparent" }
    if ($exstyle -band 0x00080000) { $flags += "layered" }
    if ($exstyle -band 0x00000008) { $flags += "topmost" }
    $lines.Add(("0x{0:x} [{1}] '{2}' class='{3}'" -f [int64]$h, $proc, $title, $class))
    $lines.Add(("    rect {0},{1} {2}x{3}  frame {4},{5} {6}x{7}  style=0x{8:x8} ex=0x{9:x8} owner=0x{10:x} {11}" -f `
      $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T), $f.L, $f.T, ($f.R - $f.L), ($f.B - $f.T),
      $style, $exstyle, [int64]$owner, ($flags -join " ")))
  }
  $h = [SwineWindows]::GetWindow($h, 2)   # GW_HWNDNEXT, in z-order
}

$out = Join-Path $env:TEMP "swinewm-windows.txt"
$lines | Set-Content -Path $out -Encoding UTF8
$lines | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "Saved to $out"
