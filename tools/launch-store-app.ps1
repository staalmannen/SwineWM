# Experiment: can a Store (packaged) app be started while swinewm is the
# shell? Tries the documented activation API and Explorer as a helper, and
# reports what happened.
#
#   tools\launch-store-app.bat <part of the app name> [aam|explorer|shell]
#
#   aam       IApplicationActivationManager::ActivateApplication (default)
#   explorer  explorer.exe shell:AppsFolder\<app id>
#   shell     Start-Process shell:AppsFolder\<app id> (failed before with
#             "Class not registered")
param([Parameter(Mandatory=$true)][string]$Name, [string]$Method = "aam")

Add-Type @"
using System; using System.Runtime.InteropServices;
[ComImport, Guid("2e941141-7f97-4756-ba1d-9decde894a3d"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IApplicationActivationManager {
  [PreserveSig] int ActivateApplication([MarshalAs(UnmanagedType.LPWStr)] string appUserModelId,
    [MarshalAs(UnmanagedType.LPWStr)] string arguments, int options, out uint processId);
  [PreserveSig] int ActivateForFile(string a, IntPtr items, string verb, out uint pid);
  [PreserveSig] int ActivateForProtocol(string a, IntPtr items, out uint pid);
}
[ComImport, Guid("45BA127D-10A8-46EA-8AB7-56EA9078943C")]
public class ApplicationActivationManager { }
public static class StoreLaunch {
  public static int Activate(string aumid, out uint pid) {
    var aam = (IApplicationActivationManager)new ApplicationActivationManager();
    return aam.ActivateApplication(aumid, null, 0, out pid);
  }
}
"@

# Get-StartApps lists the Start menu's apps with their AppUserModelID.
$apps = @(Get-StartApps | Where-Object { $_.Name -like "*$Name*" })
if ($apps.Count -eq 0) {
  Write-Host "No app whose name contains '$Name'. Get-StartApps lists them all."
  exit 1
}
$apps | ForEach-Object { Write-Host ("found: {0}  ->  {1}" -f $_.Name, $_.AppID) }
$app = $apps[0]
Write-Host ("Starting '{0}' ({1}) with method '{2}'..." -f $app.Name, $app.AppID, $Method)

switch ($Method) {
  "aam" {
    try {
      $procId = [uint32]0
      $hr = [StoreLaunch]::Activate($app.AppID, [ref]$procId)
      Write-Host ("ActivateApplication returned 0x{0:x8}, process id {1}" -f $hr, $procId)
    } catch {
      Write-Host ("ActivateApplication failed: {0}" -f $_.Exception.Message)
    }
  }
  "explorer" {
    Start-Process explorer.exe -ArgumentList ("shell:AppsFolder\" + $app.AppID)
    Write-Host "explorer.exe started; did the app open?"
  }
  "shell" {
    try {
      Start-Process ("shell:AppsFolder\" + $app.AppID)
      Write-Host "Start-Process returned; did the app open?"
    } catch {
      Write-Host ("Start-Process failed: {0}" -f $_.Exception.Message)
    }
  }
  default { Write-Host "Unknown method '$Method' (aam, explorer or shell)" }
}
