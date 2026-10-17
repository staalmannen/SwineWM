# Experiment: waits, then moves and resizes the foreground window (the one
# that has the keyboard focus), and reports whether Windows allowed it.
# Used to find out whether swinewm can fix windows that Windows places
# badly when it is the shell, such as the "Open with" dialog.
#
#   tools\move-foreground.bat [seconds] [x] [y] [width] [height]
#
# Coordinates are physical pixels on the virtual screen.
param([int]$Delay = 10, [int]$X = 800, [int]$Y = 300, [int]$Width = 0,
  [int]$Height = 800)

Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class SwineMove {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", SetLastError=true)] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  public struct RECT { public int L, T, R, B; }
}
"@
[void][SwineMove]::SetProcessDpiAwarenessContext([IntPtr](-4))

for ($i = $Delay; $i -gt 0; $i--) {
  Write-Host "Moving the foreground window in $i s - open it now and do not click elsewhere..."
  Start-Sleep 1
}
$h = [SwineMove]::GetForegroundWindow()
$class = New-Object Text.StringBuilder 256
[void][SwineMove]::GetClassName($h, $class, 256)
$r = New-Object SwineMove+RECT
[void][SwineMove]::GetWindowRect($h, [ref]$r)
$w = if ($Width -gt 0) { $Width } else { $r.R - $r.L }
Write-Host ("Window 0x{0:x} class='{1}' at {2},{3} {4}x{5}" -f [int64]$h, $class, $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T))

# SWP_NOZORDER | SWP_NOACTIVATE
$ok = [SwineMove]::SetWindowPos($h, [IntPtr]::Zero, $X, $Y, $w, $Height, 0x0014)
$err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
Start-Sleep -Milliseconds 500
[void][SwineMove]::GetWindowRect($h, [ref]$r)
Write-Host ("SetWindowPos returned {0} (error {1}); now at {2},{3} {4}x{5}" -f $ok, $err, $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T))
