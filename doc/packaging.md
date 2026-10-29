# Packaging: the MSI installer

This explains how SwineWM becomes an installable `.msi`, assuming no prior
knowledge of Windows installers.

## What an MSI is

An `.msi` file is a database that Windows Installer (built into Windows)
reads to install a program. It lists the files and where they go, Start menu
shortcuts, registry entries and environment changes. Windows Installer then
handles everything around it: the install wizard, administrator rights,
"Apps & features" / "Add or remove programs" entries, repair, clean
uninstall, and upgrading an older version in place.

Nobody writes the database by hand. The usual tool is the
[WiX Toolset](https://wixtoolset.org/), which compiles an XML description
into an MSI. SwineWM does not even write the XML: **CPack**, the packaging
tool that comes with CMake, generates it from the project's `install()`
rules. So the chain is:

```text
CMakeLists.txt install() rules ─► CPack ─► WiX XML ─► wix build ─► SwineWM-x.y.z-win64.msi
```

## What gets installed

Everything goes into one directory, `C:\Program Files\SwineWM` by default:

| File | Purpose |
| --- | --- |
| `swinewm.exe` | The window manager, bar and notifications |
| `swinemsg.exe` | IPC client (like `swaymsg`) |
| `config` | Default sway config ([`swinewm/config.in`](../swinewm/config.in)) |
| `waybar\config.jsonc`, `waybar\style.css` | Default bar ([`swinewm/resources/waybar/`](../swinewm/resources/waybar/)) |
| `mako\config` | Default notifications ([`swinewm/resources/mako/`](../swinewm/resources/mako/)) |
| `wallpapers\swinewm-default.png` | Default wallpaper |
| `LICENSE.txt` | MIT licence |

The installer also:

- adds a **Start menu** shortcut "SwineWM";
- adds the install directory to the system **PATH**, so `swinemsg` works in
  any terminal (removed again on uninstall);
- registers the app with an icon in **Add or remove programs**.

### How the defaults work

swinewm looks for each config file in the user's directories first
(`%USERPROFILE%\.swinewm\`, `%XDG_CONFIG_HOME%\swinewm\`,
`%APPDATA%\swinewm\`) and only then next to `swinewm.exe`. The installed
files are therefore the defaults, and a user customises by copying one to
`%USERPROFILE%\.swinewm\` and editing it. Upgrades replace the defaults but
never touch user files, and uninstalling leaves the user's configs alone.

`%SWINEWM_DATA%` is set to the install directory when swinewm starts, so a
config can refer to installed files, as the default wallpaper line does:

```text
output * bg %SWINEWM_DATA%\wallpapers\swinewm-default.png fill
```

The built-in fallbacks compiled into `swinewm.exe` (used if even the
installed files are missing) are generated from the same files in
`swinewm/resources/` at build time, so there is one copy of each default.

## Building the MSI

### Automatically, on GitHub

The [`build` workflow](../.github/workflows/build.yml) runs on every push:
it builds with MSVC, runs the unit tests and creates the MSI and a portable
zip. Download them from the workflow run's **Artifacts** section on the
repository's Actions tab. This is the easiest way to get an installer.

### On your own Windows machine

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) with
   "Desktop development with C++" (includes CMake), and the
   [.NET SDK](https://dotnet.microsoft.com/download).
2. Install WiX as a .NET tool, with the two extensions CPack uses:

   ```powershell
   dotnet tool install --global wix --version 5.0.2
   wix extension add -g WixToolset.UI.wixext/5.0.2
   wix extension add -g WixToolset.Util.wixext/5.0.2
   ```

3. Build and package, from the repository root:

   ```powershell
   cmake -S swinewm -B build -A x64
   cmake --build build --config Release
   cd build
   cpack -C Release            # MSI and zip
   cpack -C Release -G WIX     # only the MSI
   ```

The result is `build\SwineWM-<version>-win64.msi`. Double-click to install,
or `msiexec /i SwineWM-0.0.1-win64.msi` (add `/qn` for a silent install).

The packaging settings are in
[`swinewm/cmake/packaging.cmake`](../swinewm/cmake/packaging.cmake), and the
PATH entry in [`swinewm/cmake/wix-patch.xml`](../swinewm/cmake/wix-patch.xml).

## Things to know before distributing

- **Version numbers.** The version comes from the git tag
  (`swinewm/cmake/version.cmake`): `0.1.0` on the tagged commit,
  `0.1.0-5-g1234abc` five commits later (`-dirty` with uncommitted
  changes). The MSI gets `0.1.0.5`; Windows Installer compares the first
  three parts, and builds with the same three parts replace each other.
  Without git (a source archive) the fallback in `version.cmake` is used.
- **Upgrade code.** `CPACK_WIX_UPGRADE_GUID` identifies "SwineWM" across all
  versions; that is how a new MSI knows to replace an old one. It must never
  change.
- **Code signing.** Unsigned installers trigger a Windows SmartScreen
  warning ("Windows protected your PC"; users click *More info → Run
  anyway*), and unsigned programs are more likely to be flagged by
  Microsoft Defender (see [Antivirus warnings](#antivirus-warnings-microsoft-defender)
  below). Avoiding both requires a code-signing certificate; that is
  planned for 1.0 (`project_summary.md`).
- **Per-machine install.** The MSI installs for all users into Program
  Files and therefore asks for administrator rights.
- **Starting at login.** The installer does not make SwineWM start
  automatically or replace Explorer. Users choose (per user, no admin
  rights; each command removes the other setting):
  - instead of Explorer (the supported mode): `swinewm --set-shell`
    (`swinewm --unset-shell` undoes it). If SwineWM is the shell and crashes, it starts Explorer so
    the session stays usable; Ctrl+Alt+Del → Task Manager → Run
    `explorer.exe` always works too. Some Windows 11 dialogs do not work
    without Explorer as the shell (see `compatibility.md`).
  - next to Explorer (experimental): `swinewm --set-autostart` adds SwineWM
    to the startup programs (`HKCU\...\Run`, value `SwineWM`;
    `--unset-autostart` removes it), with Explorer as the shell behind it
    and its taskbar hidden. In first tests Explorer still took many key
    combinations and swinewm broke easily.

## Antivirus warnings (Microsoft Defender)

Microsoft Defender (Windows Security) may report `swinewm.exe` as a threat,
quarantine it, or stop it from starting. This is a false positive: SwineWM
is not signed, and to do its job it does several things that malware does
too, which is what Defender's heuristic and machine-learning detections
look for (their names often look like `Trojan:Win32/Wacatac…` or end in
`!ml`):

- it watches every key press and mouse click (low-level keyboard and mouse
  hooks), to find its key bindings and mouse moves;
- it sends synthetic input, which Windows requires before a program may
  bring another program's window to the front;
- it hides, moves and restyles other programs' windows;
- it can replace Explorer as the logon shell (`swinewm --set-shell`
  writes the Winlogon `Shell` value for your user) and start at logon;
- its watchdog may end a hung swinewm process.

### Before allowing it

Make sure the file is the one you meant to install: an MSI or zip from the
project's GitHub Releases page (its checksum should match, see
[Verifying a download](#verifying-a-download)), or a `build` run's
artifacts in the Actions tab, or your own build. In **Windows Security → Virus & threat
protection → Protection history**, check that the detection names
`swinewm.exe` (or `swinemenu.exe`, `swinemsg.exe`) in the SwineWM folder.

### Allowing it

1. If it was quarantined: **Protection history** → open the entry →
   **Actions** → **Restore** (or **Allow on device**).
2. Add an exclusion so it is not flagged again: **Windows Security →
   Virus & threat protection → Virus & threat protection settings: Manage
   settings → Exclusions: Add or remove exclusions → Add an exclusion →
   Folder**, and choose `C:\Program Files\SwineWM` (or wherever you unpacked
   the zip). This needs administrator rights.

The folder exclusion covers `swinewm.exe`, `swinemenu.exe` and
`swinemsg.exe`, and stays valid when an update replaces the files, so a
separate file exclusion is not needed. Exclude only SwineWM's own folder:
Defender no longer scans anything in it.

The same from an administrator PowerShell:

```powershell
Add-MpPreference -ExclusionPath "C:\Program Files\SwineWM"
# to undo it:
Remove-MpPreference -ExclusionPath "C:\Program Files\SwineWM"
```

Other antivirus programs have similar exclusion settings.

A folder exclusion stops Defender from scanning the files in the folder,
but it can still flag what a program does (detections whose names start
with `Behavior:`), and a detection made before the exclusion stays. If a
SwineWM program is still blocked, open its entry in **Protection history**
and choose **Actions → Allow on device**: Defender then allows that
detection everywhere.

### Without Explorer (from a terminal)

When SwineWM is the shell, open Windows Security from a terminal or with
`exec` in swinewm:

| Command | Opens |
| --- | --- |
| `start windowsdefender:` (cmd) or `Start-Process windowsdefender:` (PowerShell) | Windows Security |
| `start windowsdefender://threat` | Virus & threat protection, with the link to Protection history |
| `start windowsdefender://threatsettings` | Virus & threat protection settings, with Exclusions |

The same from an administrator PowerShell, without the window:

```powershell
# What Defender detected, and which files
Get-MpThreatDetection | Format-List ThreatID, Resources, InitialDetectionTime
Get-MpThreat | Format-List ThreatID, ThreatName
# Allow one detection on this device (ThreatID from above)
Add-MpPreference -ThreatIDDefaultAction_Ids <ThreatID> -ThreatIDDefaultAction_Actions Allow
# Restore quarantined files
& "$env:ProgramFiles\Windows Defender\MpCmdRun.exe" -Restore -ListAll
& "$env:ProgramFiles\Windows Defender\MpCmdRun.exe" -Restore -Name "<ThreatName>"
```

If SwineWM is your shell and Defender removed `swinewm.exe`, logging on
gives a black screen: press **Ctrl+Alt+Del → Task Manager → Run new task**,
start `explorer.exe`, then restore the file and add the exclusion as above
(see also [`linux-users.md`](linux-users.md#the-most-important-thing-getting-unstuck)).

### Reporting the false positive

Reports help Defender stop flagging SwineWM for everybody: submit the
detected file at <https://www.microsoft.com/wdsi/filesubmission> as an
incorrectly detected file (a false positive). Microsoft usually updates its
definitions within a few days. Signing the executables (planned for 1.0)
is the lasting fix.

## Verifying a download

Each GitHub Release has a `SHA256SUMS` file, and the same checksums in its
notes. A checksum shows that the download is the file CI built and
published, not one damaged on the way or swapped on a mirror. It does not
say who made it: that is what code signing (planned for 1.0) adds, and the
checksums keep being published then.

In PowerShell, compare the output with the line for that file:

```powershell
Get-FileHash .\SwineWM-0.5.0-win64.msi -Algorithm SHA256
```

`certutil -hashfile SwineWM-0.5.0-win64.msi SHA256` does the same in
cmd. On Linux, with the files and `SHA256SUMS` in one folder:

```sh
sha256sum -c --ignore-missing SHA256SUMS
```

GitHub also shows a `sha256:` digest next to each file of a release; it
should be the same.

## Making a release

1. In `CHANGELOG.md`, rename `## [Unreleased]` to `## [0.1.0] - <date>`,
   start a new empty `## [Unreleased]`, and update the links at the end.
2. Merge to `main`.
3. Tag the merge commit on `main` and push the tag:
   `git tag 0.1.0 && git push origin 0.1.0` (`0.1` works too and means
   0.1.0). Creating the release on GitHub instead also works: CI then
   attaches the files to it.

CI builds the tag, and the `release` job publishes a GitHub Release named
"SwineWM 0.1.0" with `SwineWM-0.1.0-win64.msi`, the zip, `SHA256SUMS`, and
that changelog section with the checksums as the notes (see
[Verifying a download](#verifying-a-download)).

## Updating a running swinewm

Windows does not let a running `.exe` be overwritten or deleted, but it
can be renamed. In an administrator PowerShell:

```powershell
cd "C:\Program Files\SwineWM"
Remove-Item swinewm.old.exe -ErrorAction SilentlyContinue
Rename-Item swinewm.exe swinewm.old.exe
Copy-Item <build folder>\swinewm.exe .
swinemsg restart
```

`restart` starts `swinewm.exe` from its original path, which is now the
new file; as the shell it does not log off. The new instance waits until
the old one has exited (`SWINEWM_RESTART_PID`). Windows on other
workspaces end up on the current one, as after a crash. Installing a newer
MSI asks to close swinewm instead.

## Assets

- The wallpaper and the icon are generated by scripts so they can be
  changed or redrawn: `swinewm/resources/wallpapers/generate.sh` and
  `swinewm/resources/icons/generate.sh` (need ImageMagick). Both are MIT
  licensed like the rest of SwineWM.
- The icon is compiled into `swinewm.exe` through
  `swinewm/swinewm/win32/swinewm.rc.in`, which also carries the version
  information shown in the file's properties.
