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

- **Version numbers.** The version comes from `project(SwineWM VERSION …)`
  in `swinewm/CMakeLists.txt`. Windows Installer only upgrades to a higher
  version, so bump it for every release.
- **Upgrade code.** `CPACK_WIX_UPGRADE_GUID` identifies "SwineWM" across all
  versions; that is how a new MSI knows to replace an old one. It must never
  change.
- **Code signing.** Unsigned installers trigger a Windows SmartScreen
  warning ("Windows protected your PC"; users click *More info → Run
  anyway*). Avoiding it requires a code-signing certificate; that can wait
  until there are users.
- **Per-machine install.** The MSI installs for all users into Program
  Files and therefore asks for administrator rights.
- **Starting at login.** The installer does not make SwineWM start
  automatically or replace Explorer. For now, start it from the Start menu,
  or put a shortcut in `shell:startup`. Running it as the real Windows shell
  is a later milestone (see the project summary).

## Assets

- The wallpaper and the icon are generated by scripts so they can be
  changed or redrawn: `swinewm/resources/wallpapers/generate.sh` and
  `swinewm/resources/icons/generate.sh` (need ImageMagick). Both are MIT
  licensed like the rest of SwineWM.
- The icon is compiled into `swinewm.exe` through
  `swinewm/swinewm/win32/swinewm.rc.in`, which also carries the version
  information shown in the file's properties.
