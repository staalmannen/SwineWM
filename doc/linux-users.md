# Windows for Linux users

You know sway, a terminal and a package manager; Windows is the alien
part. This guide maps what you know onto Windows, and tells you how to
get unstuck when swinewm runs as your shell. Commands are for PowerShell
unless they say otherwise.

## The most important thing: getting unstuck

When swinewm is your shell (`swinewm --set-shell`) there is no Explorer,
no taskbar and no Start menu. If something goes wrong:

1. Press **Ctrl+Alt+Del** and choose **Task Manager**. This always works:
   Windows handles that key combination itself, not the shell.
2. In Task Manager: **Run new task** (File menu, or the button at the
   top) and type `explorer.exe`. You get the normal Windows desktop back
   for this session.
3. To start Explorer instead of swinewm from the next logon, run
   `swinewm --unset-shell` in a terminal.

Other rescues:

- `swinewm --recover` shows windows that a crashed swinewm left hidden on
  other workspaces, and puts Explorer's taskbar setting back.
- If swinewm crashes as the shell, it starts Explorer by itself.
- A black screen at logon can also mean that Microsoft Defender
  quarantined `swinewm.exe` (a false positive; it is unsigned and watches
  the keyboard): start `explorer.exe` from Task Manager as above, then see
  [Antivirus warnings](packaging.md#antivirus-warnings-microsoft-defender).
- Its log is `%LOCALAPPDATA%\swinewm\swinewm.log`, the previous session's
  `swinewm.old.log`. For more detail: `setx SWINEWM_LOG debug` and log on
  again.

## Where things are

| Linux | Windows | Notes |
| --- | --- | --- |
| `$HOME` | `%USERPROFILE%` = `C:\Users\<you>` | `~` works in PowerShell |
| `~/.config` | `%APPDATA%` = `…\AppData\Roaming` | Settings that follow you between machines |
| `~/.local/share`, `~/.cache` | `%LOCALAPPDATA%` = `…\AppData\Local` | Per-machine data, caches, logs |
| `/usr/bin`, `/opt` | `C:\Program Files` (64-bit), `C:\Program Files (x86)` | Writing there needs administrator rights |
| `/etc` | The registry (`regedit`): `HKEY_CURRENT_USER` (you), `HKEY_LOCAL_MACHINE` (everyone) | Plus files in `C:\ProgramData` |
| `/tmp` | `%TEMP%` | |
| `/` | Drive letters: `C:\` | Paths use `\`, but most programs and PowerShell accept `/` too |

AppData is hidden in the file manager; type `%APPDATA%` in its address
bar. File names are case-insensitive (`Config` and `config` are the same
file).

swinewm's config: `%USERPROFILE%\.swinewm\config`, with `waybar\` and
`mako\` folders next to it, just like `~/.config/sway`. `%APPDATA%\swinewm\`
works too. An existing sway, Waybar or mako config from Linux can be
copied over; see [compatibility.md](compatibility.md) for what is
supported.

## Environment variables

| Linux | PowerShell | cmd.exe |
| --- | --- | --- |
| `echo $VAR` | `$env:VAR` | `echo %VAR%` |
| `export VAR=x` (this shell) | `$env:VAR = "x"` | `set VAR=x` |
| Add to `~/.profile` (permanent) | `setx VAR x` | `setx VAR x` |
| `$PATH` (`:`-separated) | `$env:Path` (`;`-separated) | `%PATH%` |

`setx` stores the variable for your user; programs started afterwards
see it, already running ones (including swinewm) do not. The graphical
editor: run `rundll32 sysdm.cpl,EditEnvironmentVariables`.

## Installing software

| Linux | Windows |
| --- | --- |
| `apt`/`pacman`/`dnf` | **winget** (built into Windows 11): `winget install Mozilla.Firefox`, `winget upgrade --all` |
| | **Chocolatey** (`choco install firefox`), the oldest community repository; needs an administrator shell |
| | **Scoop** (`scoop install alacritty`): installs into `~\scoop` without administrator rights, closest to a home-directory package manager |
| Flatpak/Snap | The Microsoft Store |

Store ("packaged") apps normally start through a part of Explorer.
swinewm starts them itself, so as the shell they open from swinemenu (the
default launcher, Win+d) and from bindings such as
`exec shell:AppsFolder\<AppUserModelID>` (PowerShell's `Get-StartApps`
lists the IDs), but third-party launchers like Flow Launcher cannot open
them without Explorer. The winget/Chocolatey/Scoop versions are normal
programs and work everywhere.

## Shells and terminals

- **PowerShell 7** (`pwsh`, `winget install Microsoft.PowerShell`) is the
  modern shell; the preinstalled **Windows PowerShell 5.1**
  (`powershell`) is older. Both know `ls`, `cd`, `cat`, `rm`, `cp`, `mv`
  as aliases, but their options differ (`rm -r -Force dir`, not
  `rm -rf dir`).
- **cmd.exe** is the old DOS-style shell; you meet it in documentation
  and `.bat` files.
- **Git Bash** (comes with Git for Windows) is a real bash with the usual
  Unix tools.
- **WSL** (`wsl --install`) runs a real Linux distribution next to
  Windows. Good for Linux tools; its files are at `\\wsl$\`.
- Terminals: **Alacritty** (swinewm's default, `$mod+Return`) or
  **Windows Terminal**. Each can run any of the shells above.

Useful equivalents:

| Linux | PowerShell |
| --- | --- |
| `which foo` | `Get-Command foo` or `where.exe foo` |
| `grep -r text .` | `Select-String -Path * -Pattern text -Recurse`, or `rg` (`winget install BurntSushi.ripgrep.MSVC`) |
| `ps aux`, `top` | `Get-Process`, Task Manager (Ctrl+Shift+Esc or Ctrl+Alt+Del) |
| `kill`, `pkill -9 foo` | `Stop-Process -Name foo`, `taskkill /im foo.exe /f` |
| `sudo cmd` | Right-click → *Run as administrator*; Windows 11 24H2 and later also have `sudo` (Settings → System → For developers) |
| `man foo` | `Get-Help foo`, or `foo /?` / `foo --help` |
| `journalctl` | Event Viewer (`eventvwr.msc`) |
| `systemctl` | Services (`services.msc`), `Get-Service`, `Restart-Service` |

## Things that work differently

- **A running program's file cannot be replaced.** Windows locks a
  running `.exe` (and its DLLs). It can be renamed, though; that is how
  swinewm is updated in place (see [packaging.md](packaging.md):
  rename, copy, `swinemsg restart`).
- **Administrator windows.** A program run as administrator is out of
  reach of normal programs: while such a window has the focus, swinewm's
  key bindings do not work, and swinewm cannot move it. Avoid running
  terminals as administrator by habit.
- **Autostart.** Instead of `exec` lines in a session script, Windows has
  the Startup folder (`explorer shell:startup`), the `Run` registry keys
  and Task Scheduler. As the shell, swinewm starts the Startup folder and
  `Run` entries at logon, as Explorer would; Task Scheduler runs on its
  own.
- **Line endings.** Windows text files often end lines with CRLF. Git for
  Windows converts by default; for shared repositories,
  `git config --global core.autocrlf input` keeps LF in the repository.
- **Reserved keys.** Nothing can intercept Win+L (lock) or Ctrl+Alt+Del,
  so sway's `$mod+l` locks the screen. Use the arrow keys, or change
  `$mod` in the config.
- **Display scaling.** Each monitor has its own scaling (Settings →
  System → Display); swinewm's bar follows it.
- **Settings.** The Settings app is itself a Store-style app; if it does
  not open while swinewm is the shell, `control` (the classic Control
  Panel) covers most settings, and `explorer.exe` from Task Manager
  brings everything back for the session.

## Git and GitHub

- **Git for Windows** (`winget install Git.Git`) brings git, Git Bash and
  a credential manager: the first push opens a browser window to log in
  to GitHub, no SSH key needed. Existing SSH keys work too: copy them to
  `%USERPROFILE%\.ssh`.
- Tags, branches and pushes are the same commands as on Linux, in Git
  Bash or PowerShell. Releasing swinewm: see
  [packaging.md](packaging.md#making-a-release).

## Building swinewm on Windows

- **Visual Studio**: *Open a local folder* on the repository; it finds
  the CMake project. The executables end up in
  `swinewm\out\build\x64-Debug\`. Ctrl+Shift+B builds.
- **MSYS2** gives a Unix-like environment with MinGW-w64 GCC and Ninja;
  the commands in [`swinewm/README.md`](../swinewm/README.md) work as on
  Linux.
- Updating the installed copy while it runs: rename the running
  `swinewm.exe`, copy the new one, `swinemsg restart`.
