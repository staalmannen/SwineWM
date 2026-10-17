# Lists every visible top-level window, top of the z-order first, and every
# window (hidden ones too) of the processes that host Windows' own dialogs
# and flyouts, with what swinewm needs to know about each: process, class,
# title, rectangle, styles, owner, DWM cloaking and z-order band.
# For bug reports about windows swinewm handles badly (dialogs off screen,
# popups that get tiled, windows that are not managed).
#
#   tools\list-windows.bat [seconds]
#
# waits the given number of seconds (default 5) so that the window in
# question can be opened first, then prints the list and saves it to
# %TEMP%\swinewm-windows.txt.
param([int]$Delay = 5)
# Processes that host Windows' own dialogs and flyouts. All their windows
# are listed, hidden ones and child windows included.
$ShellProcesses = @("OpenWith", "PickerHost", "ApplicationFrameHost",
  "ShellHost", "ShellExperienceHost", "StartMenuExperienceHost",
  "SearchHost", "TextInputHost", "explorer")

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
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
  // Undocumented but exported since Windows 8: the z-order band (1 = desktop).
  [DllImport("user32.dll")] public static extern bool GetWindowBand(IntPtr h, out uint band);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out int v, int s);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT v, int s);
  public struct RECT { public int L, T, R, B; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumThreadWindows(uint thread, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc f, IntPtr l);
}
"@

# Physical pixels, as swinewm sees them (per-monitor DPI aware v2).
[void][SwineWindows]::SetProcessDpiAwarenessContext([IntPtr](-4))

for ($i = $Delay; $i -gt 0; $i--) {
  Write-Host "Listing windows in $i s - open the window now..."
  Start-Sleep 1
}

$lines = New-Object System.Collections.Generic.List[string]

function Describe-Window([IntPtr]$h, [string]$indent) {
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
  if (-not [SwineWindows]::IsWindowVisible($h)) { $flags += "HIDDEN" }
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
  $lines.Add(("{0}0x{1:x} [{2}] '{3}' class='{4}'" -f $indent, [int64]$h, $proc, $title, $class))
  $lines.Add(("{0}    rect {1},{2} {3}x{4}  frame {5},{6} {7}x{8}  style=0x{9:x8} ex=0x{10:x8} owner=0x{11:x} {12}" -f `
    $indent, $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T), $f.L, $f.T, ($f.R - $f.L), ($f.B - $f.T),
    $style, $exstyle, [int64]$owner, ($flags -join " ")))
}

$lines.Add(("list-windows v4, {0}" -f (Get-Date -Format s)))
# The window that has the keyboard focus (the one Esc would close).
$fg = [SwineWindows]::GetForegroundWindow()
$lines.Add("== Foreground window (and its root and owner chain) ==")
if ($fg -ne [IntPtr]::Zero) {
  Describe-Window $fg ""
  $root = [SwineWindows]::GetAncestor($fg, 2)   # GA_ROOT
  if ($root -ne $fg -and $root -ne [IntPtr]::Zero) { Describe-Window $root "  root " }
  $o = [SwineWindows]::GetWindow($fg, 4)        # GW_OWNER
  while ($o -ne [IntPtr]::Zero) {
    Describe-Window $o "  owner "
    $o = [SwineWindows]::GetWindow($o, 4)
  }
} else {
  $lines.Add("(none)")
}
$lines.Add("")
$lines.Add("== Visible top-level windows, top of the z-order first ==")
# EnumWindows only returns windows of desktop apps (see its documentation);
# walking the desktop's children with GetWindow also finds the "immersive"
# windows of modern Windows dialogs, flyouts and Store apps.
$h = [SwineWindows]::GetWindow([SwineWindows]::GetDesktopWindow(), 5)   # GW_CHILD
while ($h -ne [IntPtr]::Zero) {
  if ([SwineWindows]::IsWindowVisible($h)) {
    Describe-Window $h ""
  }
  $h = [SwineWindows]::GetWindow($h, 2)   # GW_HWNDNEXT, in z-order
}

$lines.Add("")
$lines.Add("== All windows of Windows' dialog and flyout processes ==")
foreach ($p in Get-Process -Name $ShellProcesses -ErrorAction SilentlyContinue) {
  $lines.Add(("-- {0} (pid {1}) {2}" -f $p.ProcessName, $p.Id, $p.Path))
  foreach ($t in $p.Threads) {
    [void][SwineWindows]::EnumThreadWindows([uint32]$t.Id, {
      param($w, $l)
      Describe-Window $w "  "
      [void][SwineWindows]::EnumChildWindows($w, {
        param($c, $l2)
        Describe-Window $c "      "
        $true
      }, [IntPtr]::Zero)
      $true
    }, [IntPtr]::Zero)
  }
}

$out = Join-Path $env:TEMP "swinewm-windows.txt"
$lines | Set-Content -Path $out -Encoding UTF8
# The second part is long (Explorer alone has hundreds of windows): show
# the first part, keep everything in the file.
$split = $lines.IndexOf("== All windows of Windows' dialog and flyout processes ==")
$lines | Select-Object -First $split | ForEach-Object { Write-Host $_ }
$lines | Where-Object { $_ -like "-- *" } | ForEach-Object { Write-Host $_ }
Write-Host ("({0} more lines with all windows of these processes in the file)" -f ($lines.Count - $split))
Write-Host ""
Write-Host "Saved to $out"
